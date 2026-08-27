/* Based on quicksort.c - distributed variant: sample-splitter p-way partition
 * (PSRS/samplesort lineage) on a place-persistent decomposition.  A sorted
 * sample of the input gives ascending splitters, every element is routed to the
 * bucket its value falls in, and each bucket sorts what it receives; the
 * buckets are in splitter order, so concatenating them is the sorted array.
 * The finisher validates global sortedness and the permutation-preserving
 * element sum.
 *
 * The program is SPMD over a fixed number of PLACES, not a centrally built task
 * graph.  mainEdt creates one task per place and the events the places hand
 * data through, and nothing else; each place then builds its own chunks, its
 * own pack and its own buckets where it runs.  A graph built in one place
 * cannot scale -- every creation carrying a remote affinity is a message, so
 * the builder's cost grows with the machine instead of shrinking with it.
 *
 * The place count is an argument, not the rank count.  The program is written
 * for a fixed decomposition and the runtime maps it onto whatever ranks exist,
 * so the task and datablock counts, and the order the partial sums combine in,
 * are the same in every geometry.
 *
 * The exchange is aggregated per place, which is what a distributed sample sort
 * does: a place packs everything it owes a peer into one buffer and sends it
 * once, so the exchange is P(P-1) messages whatever the bucket count.
 * Expressed per bucket it would be one message per (chunk, bucket) pair --
 * quadratic in a quantity chosen for compute parallelism.
 */

#include <ocr.h>
#include <extensions/ocr-affinity.h>
#include <stdlib.h>
#include "macros.h"

//Size of array to be sorted (compile-time default; overridable at runtime)
#define ARRAY_SIZE 1000
//Range of numbers to be sorted.
#define RANGE 1000000
//Samples drawn per bucket for splitter selection.
#define SAMPLES_PER_BUCKET 32
//Classification chunks (parallel readers of the input array).
#define DEFAULT_CHUNKS 48
//Buckets per policy domain (sort-phase parallelism per rank).
#define DEFAULT_BUCKETS 64
//Below this many elements the local sort switches to insertion sort.
#define SORT_CUTOFF 32
//Default number of places the exchange aggregates into.  A place is a unit of
//ownership and of communication, not a machine fact.
#define DEFAULT_PLACES 8
//Largest number of ways one exchange round splits the key range.  The cost is
//O(p k log_k p) messages against log_k(p) forwardings of every element, so the
//total is flat over a wide middle range of k and steep only at the ends: k=2
//forwards the data as many times as there are rounds, k=p is the direct
//exchange this staging exists to avoid.
#define XCHG_RADIX 8

//Pseudo-RNG.  Gets rid of C stdlib dependence (identical to quicksort.c so
//both variants sort the same input for a given size/range).
int getRandNum(int seed) {
    int MAX = 1000;
    int i;
    int r[MAX];
    int ret;
    r[0] = seed;
    for(i=1; i<31; i++){
        r[i] = (16807LL * r[i-1]) % 2147483647;
        if (r[i] < 0){
            r[i] += 2147483647;
        }
    }
    for(i=31; i<34; i++){
        r[i] = r[i-31];
    }
    for(i=34; i<344; i++){
        r[i] = r[i-31] + r[i-3];
    }
    for(i=344; i<MAX; i++){
        r[i] = r[i-31] + r[i-3];
        ret = ((unsigned int)r[i]) >> 1;
    }
    return ret;
}

static void insertionSort(u64 *a, u64 n) {
    u64 i, j, v;
    for(i = 1; i < n; i++) {
        v = a[i];
        j = i;
        while(j > 0 && a[j-1] > v) {
            a[j] = a[j-1];
            j--;
        }
        a[j] = v;
    }
}

//In-place quicksort: median-of-three pivot, Hoare partition, recurse on the
//smaller side and loop on the larger so stack depth stays O(log n).
static void quicksortLocal(u64 *a, u64 n) {
    while(n > SORT_CUTOFF) {
        u64 mid = n / 2;
        u64 lo = a[0], mv = a[mid], hi = a[n-1], pivot, t;
        if(lo > mv) { t = lo; lo = mv; mv = t; }
        if(mv > hi) { t = mv; mv = hi; hi = t; }
        if(lo > mv) { t = lo; lo = mv; mv = t; }
        pivot = mv;

        u64 i = 0, j = n - 1;
        for(;;) {
            while(a[i] < pivot) i++;
            while(a[j] > pivot) j--;
            if(i >= j) break;
            t = a[i]; a[i] = a[j]; a[j] = t;
            i++; j--;
        }
        //[0..j] and [j+1..n-1]; recurse into the smaller partition
        u64 left = j + 1, right = n - left;
        if(left <= right) {
            quicksortLocal(a, left);
            a += left;
            n = right;
        } else {
            quicksortLocal(a + left, right);
            n = left;
        }
    }
    insertionSort(a, n);
}

//Factor W into the per-round radices of the staged exchange, most significant
//The rank a place is mapped onto.  This is the ONLY thing the program asks the
//machine, and it asks it for a hint -- the decomposition is fixed by argument,
//so where a place lands changes nothing about what the program is.
static u64 placeRank(u64 place, u64 places) {
    u64 nranks = 1;
    ocrAffinityCount(AFFINITY_PD, &nranks);
    return (place * nranks) / places;
}

static ocrHint_t edtHintAt(u64 rank) {
    ocrGuid_t aff;
    ocrHint_t h;
    ocrAffinityGetAt(AFFINITY_PD, rank, &aff);
    ocrHintInit(&h, OCR_HINT_EDT_T);
    ocrSetHintValue(&h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
}

//Home pin for a single-use handoff block: its directory round-trip is on the
//critical path and ownership migration cannot amortize a block consumed
//exactly once, so it is created directly on its consumer's home.
static ocrHint_t dbHintAt(u64 rank) {
    ocrGuid_t aff;
    ocrHint_t h;
    ocrAffinityGetAt(AFFINITY_PD, rank, &aff);
    ocrHintInit(&h, OCR_HINT_DB_T);
    ocrSetHintValue(&h, OCR_HINT_DB_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
}

//A place's share of a count, as a half-open range.  Ownership is contiguous so
//that a place's buckets are a contiguous key range.
static void placeRange(u64 place, u64 count, u64 places, u64 *lo, u64 *hi) {
    *lo = (place * count) / places;
    *hi = ((place + 1) * count) / places;
}

//How many waves a place packs in.  Derived from the decomposition rather than
//from a place's own share: the builder lays the exchange grid and the parameter
//block out with this count and every place decodes them with it, so a rule that
//read a place's own share would have the two disagree wherever the chunks do
//not divide evenly -- the builder sizing for one wave while a place that drew
//one chunk more indexes for eight, past the end of both.
static u64 waveCount(u64 nchunks, u64 places) {
    u64 lo, hi;
    placeRange(0, nchunks, places, &lo, &hi);
    return (hi - lo) >= 8 ? 8 : 1;
}

//First bucket whose upper splitter bounds v (splitters ascending, p-1 of them).
static u64 bucketOf(u64 v, const u64 *splitters, u64 nbuckets) {
    u64 lo = 0, hi = nbuckets - 1;
    while(lo < hi) {
        u64 mid = (lo + hi) / 2;
        if(v < splitters[mid]) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

// paramv: {lo, hi(excl), arraySize, nsamples, range, evt}
// Draw this sampler's share of the splitter sample.  The sample is drawn in the
// same pieces the input is, and for the same reason: it is nsamples generator
// calls, it grows with the bucket count, and drawing it in one task would put a
// serial term proportional to the width ahead of everything else.
ocrGuid_t sampleTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 lo = paramv[0], hi = paramv[1], arraySize = paramv[2];
    u64 nsamples = paramv[3], range = paramv[4];
    ocrGuid_t evt = (ocrGuid_t){.guid = paramv[5]};
    u64 n = hi - lo, i, span = nsamples > 1 ? nsamples - 1 : 1;

    ocrGuid_t db;
    u64 *out;
    //layout: [0]=count, [1..count]=values
    ocrDbCreate(&db, (void**)&out, (n + 1) * sizeof(u64), 0, NULL_HINT, NO_ALLOC);
    out[0] = n;
    for(i = 0; i < n; i++)
        out[1 + i] = getRandNum((int)(((lo + i) * (arraySize - 1)) / span)) % range;
    ocrDbRelease(db);
    ocrEventSatisfy(evt, db);
    return NULL_GUID;
}

// paramv: {nbuckets, evt}
// depv 0..depc-1: the samplers' blocks
// Sort the whole sample and cut it into the nbuckets-1 ascending splitters
// every later task classifies against.  One task builds them and every place
// reads the same block, so every place cuts identical buckets.
ocrGuid_t splitterTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 nbuckets = paramv[0];
    ocrGuid_t evt = (ocrGuid_t){.guid = paramv[1]};
    u64 c, i, b, ns = 0;
    for(c = 0; c < depc; c++) ns += ((const u64*)depv[c].ptr)[0];

    u64 *sample = (u64*)malloc((ns ? ns : 1) * sizeof(u64));
    u64 pos = 0;
    for(c = 0; c < depc; c++) {
        const u64 *s = (const u64*)depv[c].ptr;
        for(i = 1; i <= s[0]; i++) sample[pos++] = s[i];
        ocrDbDestroy(depv[c].guid);
    }
    quicksortLocal(sample, ns);

    ocrGuid_t db;
    u64 *splitters;
    ocrDbCreate(&db, (void**)&splitters,
                sizeof(u64) * (nbuckets > 1 ? nbuckets - 1 : 1), 0, NULL_HINT,
                NO_ALLOC);
    for(b = 0; b + 1 < nbuckets; b++)
        splitters[b] = sample[((b + 1) * ns) / nbuckets];
    free(sample);
    ocrDbRelease(db);
    ocrEventSatisfy(evt, db);
    return NULL_GUID;
}

// paramv: {low, high(excl), range, nbuckets, places, evt}
// depv 0: splitters (RO)
// Generate this chunk's own slice of the input and group it by destination
// place, so the pack below takes a peer's share as a contiguous run.  The
// generator is a pure function of the index, so the values are the ones the
// base program produces; what that removes is a serial fill of the whole input
// and a block every chunk had to acquire.
//
// Layout: [places counts][elements, destination-place major].  The chunk's own
// element sum rides in the block so no two chunks share a writable object --
// an N-wide exclusive fan-out onto one block costs more than the sort.
ocrGuid_t chunkTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 low = paramv[0], high = paramv[1], range = paramv[2];
    u64 nbuckets = paramv[3], places = paramv[4];
    ocrGuid_t evt = (ocrGuid_t){.guid = paramv[5]};
    const u64 *splitters = (const u64*)depv[0].ptr;
    u64 n = high - low, i, p;

    u64 *mine = (u64*)malloc((n ? n : 1) * sizeof(u64));
    u64 *dest = (u64*)malloc((n ? n : 1) * sizeof(u64));
    u64 *count = (u64*)malloc(places * sizeof(u64));
    u64 *at = (u64*)malloc(places * sizeof(u64));
    for(p = 0; p < places; p++) count[p] = 0;

    u64 mySum = 0;
    for(i = 0; i < n; i++) {
        u64 v = getRandNum((int)(low + i)) % range;
        mine[i] = v;
        mySum += v;
        //which place owns the bucket this value falls in
        u64 b = bucketOf(v, splitters, nbuckets);
        //the place whose bucket range contains b; the division inverts
        //placeRange only up to rounding, so walk to the true owner
        u64 d = (b * places) / nbuckets;
        if(d >= places) d = places - 1;
        while(d + 1 < places && ((d + 1) * nbuckets) / places <= b) d++;
        while(d > 0 && (d * nbuckets) / places > b) d--;
        dest[i] = d;
        count[d]++;
    }

    ocrGuid_t db;
    u64 *out;
    //layout: [0]=sum, [1..places]=per-place counts, then the elements
    ocrDbCreate(&db, (void**)&out, (1 + places + n) * sizeof(u64), 0, NULL_HINT,
                NO_ALLOC);
    out[0] = mySum;
    u64 base = 1 + places;
    for(p = 0; p < places; p++) { out[1 + p] = count[p]; at[p] = base; base += count[p]; }
    for(i = 0; i < n; i++) out[at[dest[i]]++] = mine[i];

    free(mine); free(dest); free(count); free(at);
    ocrDbRelease(db);
    ocrEventSatisfy(evt, db);
    return NULL_GUID;
}

// paramv: {g, places, send[places]}
// depv 0..ncnk-1: this wave's chunks, in order
// The exchange, aggregated.  Everything this place owes a peer goes into one
// block and is sent once, so the message count is P(P-1) whatever the bucket
// count.  The place's chunk sums ride along in the block bound for itself.
ocrGuid_t packTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 g = paramv[0], places = paramv[1];
    const u64 *send = paramv + 2;
    u64 cpg = depc, c, p;   //this wave's chunks, not the place's

    //Every destination's buffer is opened first, then the sources are walked
    //ONCE and each is released as it is consumed.  The other nesting -- a
    //destination outer, the sources inner -- holds every input alive until the
    //last output is finished, so the peak is the input set plus the output set
    //rather than the output set alone.
    u64 **out = (u64**)malloc(places * sizeof(u64*));
    ocrGuid_t *db = (ocrGuid_t*)malloc(places * sizeof(ocrGuid_t));
    u64 *at = (u64*)malloc(places * sizeof(u64));
    for(p = 0; p < places; p++) {
        u64 total = 0;
        for(c = 0; c < cpg; c++) total += ((const u64*)depv[c].ptr)[1 + p];
        ocrHint_t h = dbHintAt(placeRank(p, places));
        //layout: [0]=summed chunk sums (only meaningful to the owner), then
        //[1]=count and the elements
        ocrDbCreate(&db[p], (void**)&out[p], (2 + total) * sizeof(u64), 0, &h,
                    NO_ALLOC);
        out[p][0] = 0;
        out[p][1] = total;
        at[p] = 2;
    }
    for(c = 0; c < cpg; c++) {
        const u64 *in = (const u64*)depv[c].ptr;
        u64 off = 1 + places, i;
        for(p = 0; p < places; p++) {
            u64 cnt = in[1 + p];
            for(i = 0; i < cnt; i++) out[p][at[p]++] = in[off + i];
            off += cnt;
        }
        //the input sum is reported once, on the block this place sends itself
        out[g][0] += in[0];
        //a wave's chunks die with the wave, so the generated form is never all
        //resident: it peaks at one wave per place rather than the whole place
        ocrDbDestroy(depv[c].guid);
    }
    for(p = 0; p < places; p++) {
        ocrDbRelease(db[p]);
        ocrEventSatisfy((ocrGuid_t){.guid = send[p]}, db[p]);
    }
    free(out); free(db); free(at);
    return NULL_GUID;
}

// paramv: {g, places, nbuckets, evt[bpg]}
// depv 0..places-1: the packed block from each place; depv places: splitters
// Cut what arrived into this place's own buckets.  One task per place does it,
// so a bucket is handed exactly its own elements rather than scanning every
// arrival for them.
ocrGuid_t unpackTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 g = paramv[0], places = paramv[1], nbuckets = paramv[2];
    const u64 *evt = paramv + 3;
    u64 narr = depc - 1;                       //arrivals: one per (place, wave)
    const u64 *splitters = (const u64*)depv[narr].ptr;
    u64 lo, hi;
    placeRange(g, nbuckets, places, &lo, &hi);
    u64 bpg = hi - lo, a, i, b;
    const u64 *src = evt + bpg + 1;            //the events this task consumed

    u64 *count = (u64*)malloc((bpg ? bpg : 1) * sizeof(u64));
    u64 *at = (u64*)malloc((bpg ? bpg : 1) * sizeof(u64));
    u64 **out = (u64**)malloc((bpg ? bpg : 1) * sizeof(u64*));
    ocrGuid_t *db = (ocrGuid_t*)malloc((bpg ? bpg : 1) * sizeof(ocrGuid_t));
    for(b = 0; b < bpg; b++) count[b] = 0;

    u64 inputSum = 0;
    for(a = 0; a < narr; a++) {
        const u64 *in = (const u64*)depv[a].ptr;
        inputSum += in[0];
        for(i = 0; i < in[1]; i++)
            count[bucketOf(in[2 + i], splitters, nbuckets) - lo]++;
    }
    for(b = 0; b < bpg; b++) {
        //layout: [0]=count, [1..count]=elements -- created here, on this place
        ocrDbCreate(&db[b], (void**)&out[b], (count[b] + 1) * sizeof(u64), 0,
                    NULL_HINT, NO_ALLOC);
        out[b][0] = count[b];
        at[b] = 1;
    }
    for(a = 0; a < narr; a++) {
        const u64 *in = (const u64*)depv[a].ptr;
        for(i = 0; i < in[1]; i++) {
            u64 v = in[2 + i], k = bucketOf(v, splitters, nbuckets) - lo;
            out[k][at[k]++] = v;
        }
        ocrDbDestroy(depv[a].guid);
        /* The exchange events are sticky, so they do not reclaim themselves
         * when they fire.  This task is their only consumer and has just run,
         * so it is the one place that can know they are finished with. */
        ocrEventDestroy((ocrGuid_t){.guid = src[a]});
    }
    for(b = 0; b < bpg; b++) {
        ocrDbRelease(db[b]);
        ocrEventSatisfy((ocrGuid_t){.guid = evt[b]}, db[b]);
    }
    //the input sum this place gathered goes on the last slot
    ocrGuid_t sumDb;
    u64 *sum;
    ocrDbCreate(&sumDb, (void**)&sum, sizeof(u64), 0, NULL_HINT, NO_ALLOC);
    sum[0] = inputSum;
    ocrDbRelease(sumDb);
    ocrEventSatisfy((ocrGuid_t){.guid = evt[bpg]}, sumDb);

    free(count); free(at); free(out); free(db);
    return NULL_GUID;
}

// paramv: {evt}
// depv 0: this bucket's elements (RO)
// The key range is down to one bucket, so what arrived is the bucket: sort it
// and report the five words the finisher checks.  The bucket checks itself --
// it is the only task that sees these elements together.
ocrGuid_t bucketTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrGuid_t evt = (ocrGuid_t){.guid = paramv[0]};
    const u64 *in = (const u64*)depv[0].ptr;
    u64 total = in[0], i;

    ocrGuid_t bucketDb;
    u64 *out;
    ocrDbCreate(&bucketDb, (void**)&out, (total + 1) * sizeof(u64), 0, NULL_HINT,
                NO_ALLOC);
    out[0] = total;
    for(i = 0; i < total; i++) out[1 + i] = in[1 + i];
    ocrDbDestroy(depv[0].guid);
    quicksortLocal(out + 1, total);

    ocrGuid_t sumDb;
    u64 *v;
    //layout: [0]=count, [1]=sum, [2]=sorted, [3]=first, [4]=last
    ocrDbCreate(&sumDb, (void**)&v, 5 * sizeof(u64), 0, NULL_HINT, NO_ALLOC);
    {   u64 acc = 0, sorted = 1;
        for(i = 0; i < total; i++) {
            if(i && out[1 + i] < out[i]) sorted = 0;
            acc += out[1 + i];
        }
        v[0] = total; v[1] = acc; v[2] = sorted;
        v[3] = total ? out[1] : 0;
        v[4] = total ? out[total] : 0;
    }
    //The bucket has been checked -- count, sum, order and both ends are in the
    //verdict above -- and nothing reads these elements again: the finisher
    //works from the verdicts.  Holding them to the end would keep a second
    //copy of the whole array alive for no reader.
    ocrDbRelease(bucketDb);
    ocrDbDestroy(bucketDb);
    ocrDbRelease(sumDb);
    ocrEventSatisfy(evt, sumDb);
    return NULL_GUID;
}

// paramv: {g, places, nbuckets, doneEvt}
// depv 0..bpg-1: this place's bucket verdicts; depv bpg: its gathered input sum
// Combine this place's buckets into one verdict, so the finisher sees P of
// them rather than one per bucket.
ocrGuid_t placeJoinTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrGuid_t doneEvt = (ocrGuid_t){.guid = paramv[3]};
    u64 bpg = depc - 1, i;

    u64 total = 0, sum = 0, sorted = 1, first = 0, last = 0, prev = 0;
    int have = 0;
    for(i = 0; i < bpg; i++) {
        const u64 *d = (const u64*)depv[i].ptr;
        total += d[0];
        sum += d[1];
        if(!d[2]) sorted = 0;
        if(d[0]) {
            if(have && d[3] < prev) sorted = 0;
            if(!have) first = d[3];
            prev = d[4];
            last = d[4];
            have = 1;
        }
        ocrDbDestroy(depv[i].guid);
    }
    u64 inputSum = ((const u64*)depv[bpg].ptr)[0];
    ocrDbDestroy(depv[bpg].guid);

    ocrGuid_t db;
    u64 *v;
    //layout: [0]=count, [1]=sum, [2]=sorted, [3]=first, [4]=last, [5]=inputSum
    ocrDbCreate(&db, (void**)&v, 6 * sizeof(u64), 0, NULL_HINT, NO_ALLOC);
    v[0] = total; v[1] = sum; v[2] = sorted;
    v[3] = have ? first : 0; v[4] = have ? last : 0; v[5] = inputSum;
    ocrDbRelease(db);
    ocrEventSatisfy(doneEvt, db);
    return NULL_GUID;
}

// paramv: {g, arraySize, range, nbuckets, nchunks, places, splitEvt, doneEvt,
//          send[places], recv[places]}
// A place's whole program, built where it runs.  The builder never reaches
// across the machine, so what it costs does not depend on how many ranks there
// are -- which is the difference between a program that scales and one that
// does not.
ocrGuid_t placeInitTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 g = paramv[0], arraySize = paramv[1], range = paramv[2];
    u64 nbuckets = paramv[3], nchunks = paramv[4], places = paramv[5];
    u64 splitEvt = paramv[6], doneEvt = paramv[7];
    u64 clo, chi, blo, bhi, k, a, w;
    placeRange(g, nchunks, places, &clo, &chi);
    placeRange(g, nbuckets, places, &blo, &bhi);
    u64 cpg = chi - clo, bpg = bhi - blo;
    u64 nwave = waveCount(nchunks, places);
    const u64 *send = paramv + 8;                  //[destination place][wave]
    const u64 *recv = paramv + 8 + places * nwave; //[source place][wave]
    ocrHint_t h = edtHintAt(placeRank(g, places));

    //the join, so the buckets below have somewhere to report
    ocrGuid_t joinTml, joinEdt;
    u64 joinPrm[4] = {g, places, nbuckets, doneEvt};
    ocrEdtTemplateCreate(&joinTml, placeJoinTask, 4, (u32)(bpg + 1));
    ocrEdtCreate(&joinEdt, joinTml, EDT_PARAM_DEF, joinPrm, EDT_PARAM_DEF, NULL,
                 EDT_PROP_NONE, &h, NULL);
    ocrEdtTemplateDestroy(joinTml);

    //the unpack, and the buckets it feeds
    ocrGuid_t unpackTml, unpackEdt;
    u64 *unpackPrm =
        (u64*)malloc((3 + bpg + 1 + places * nwave) * sizeof(u64));
    unpackPrm[0] = g; unpackPrm[1] = places; unpackPrm[2] = nbuckets;

    ocrGuid_t bucketTml;
    ocrEdtTemplateCreate(&bucketTml, bucketTask, 1, 1);
    for(k = 0; k < bpg; k++) {
        ocrGuid_t inEvt, outEvt, bucketEdt;
        ocrEventCreate(&inEvt, OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);
        ocrEventCreate(&outEvt, OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);
        ocrAddDependence(outEvt, joinEdt, (u32)k, DB_MODE_RO);

        u64 bucketPrm[1] = {(u64)outEvt.guid};
        ocrEdtCreate(&bucketEdt, bucketTml, EDT_PARAM_DEF, bucketPrm,
                     EDT_PARAM_DEF, NULL, EDT_PROP_NONE, &h, NULL);
        ocrAddDependence(inEvt, bucketEdt, 0, DB_MODE_RO);
        unpackPrm[3 + k] = (u64)inEvt.guid;
    }
    ocrEdtTemplateDestroy(bucketTml);
    {   ocrGuid_t sumEvt;
        ocrEventCreate(&sumEvt, OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);
        ocrAddDependence(sumEvt, joinEdt, (u32)bpg, DB_MODE_RO);
        unpackPrm[3 + bpg] = (u64)sumEvt.guid;
    }
    for(a = 0; a < places * nwave; a++) unpackPrm[3 + bpg + 1 + a] = recv[a];
    ocrEdtTemplateCreate(&unpackTml, unpackTask,
                         (u32)(3 + bpg + 1 + places * nwave),
                         (u32)(places * nwave + 1));
    ocrEdtCreate(&unpackEdt, unpackTml, EDT_PARAM_DEF, unpackPrm, EDT_PARAM_DEF,
                 NULL, EDT_PROP_NONE, &h, NULL);
    ocrEdtTemplateDestroy(unpackTml);
    free(unpackPrm);
    for(a = 0; a < places * nwave; a++)
        ocrAddDependence((ocrGuid_t){.guid = recv[a]}, unpackEdt, (u32)a, DB_MODE_RO);
    ocrAddDependence((ocrGuid_t){.guid = splitEvt}, unpackEdt,
                     (u32)(places * nwave), DB_MODE_RO);

    //The pack runs in waves so a wave's chunks are released as soon as that
    //wave has packed: with one pack per place, every chunk of the place has to
    //be resident before any of them can be freed, which is the whole generated
    //input alive at once.  Each wave sends its own block per destination, so a
    //destination receives one per (place, wave) and frees each as it reads it.
    ocrGuid_t chunkTml;
    ocrEdtTemplateCreate(&chunkTml, chunkTask, 6, 1);
    u64 *packPrm = (u64*)malloc((2 + places) * sizeof(u64));
    for(w = 0; w < nwave; w++) {
        u64 wlo = clo + (w * cpg) / nwave, whi = clo + ((w + 1) * cpg) / nwave;
        ocrGuid_t packTml, packEdt;
        packPrm[0] = g; packPrm[1] = places;
        for(a = 0; a < places; a++) packPrm[2 + a] = send[a * nwave + w];
        ocrEdtTemplateCreate(&packTml, packTask, (u32)(2 + places),
                             (u32)(whi - wlo));
        ocrEdtCreate(&packEdt, packTml, EDT_PARAM_DEF, packPrm, EDT_PARAM_DEF,
                     NULL, EDT_PROP_NONE, &h, NULL);
        ocrEdtTemplateDestroy(packTml);

        for(k = wlo; k < whi; k++) {
            ocrGuid_t chunkEvt, chunkEdt;
            ocrEventCreate(&chunkEvt, OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);
            ocrAddDependence(chunkEvt, packEdt, (u32)(k - wlo), DB_MODE_RO);

            u64 chunkPrm[6] = {(k * arraySize) / nchunks,
                               ((k + 1) * arraySize) / nchunks,
                               range, nbuckets, places, (u64)chunkEvt.guid};
            ocrEdtCreate(&chunkEdt, chunkTml, EDT_PARAM_DEF, chunkPrm,
                         EDT_PARAM_DEF, NULL, EDT_PROP_NONE, &h, NULL);
            ocrAddDependence((ocrGuid_t){.guid = splitEvt}, chunkEdt, 0, DB_MODE_RO);
        }
    }
    free(packPrm);
    ocrEdtTemplateDestroy(chunkTml);
    return NULL_GUID;
}

// paramv: {arraySize, places}
// depv 0..places-1: one verdict per place, in bucket order
ocrGuid_t finishTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 arraySize = paramv[0], places = paramv[1], i;
    u64 total = 0, sum = 0, sorted = 1, inputSum = 0, prev = 0;
    int have = 0;
    for(i = 0; i < places; i++) {
        const u64 *d = (const u64*)depv[i].ptr;
        total += d[0];
        sum += d[1];
        inputSum += d[5];
        if(!d[2]) sorted = 0;
        if(d[0]) {
            if(have && d[3] < prev) sorted = 0;
            prev = d[4];
            have = 1;
        }
        ocrDbDestroy(depv[i].guid);
    }
    if(total != arraySize) sorted = 0;
    if(sum != inputSum) sorted = 0;

    ocrPrintf("QSORT_VALID sum=%lu sorted=%lu n=%lu\n", sum, sorted, total);
    ocrShutdown();
    return NULL_GUID;
}

// The whole of the central work: the events the places hand data through, the
// splitter build, and one task per place.  P*P + P + 1 events and P + 2 tasks --
// nothing here grows with the bucket or chunk count, which is what lets those
// be chosen for parallelism.
ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 arraySize = ARRAY_SIZE;
    u64 range = RANGE;
    u64 nbuckets = 0, nchunks = 0, places = DEFAULT_PLACES;
    if(depc >= 1 && depv[0].ptr) {
        u64 argc = getArgc(depv[0].ptr);
        if(argc > 1) arraySize = strtoull(getArgv(depv[0].ptr, 1), NULL, 10);
        if(argc > 2) range     = strtoull(getArgv(depv[0].ptr, 2), NULL, 10);
        if(argc > 3) nbuckets  = strtoull(getArgv(depv[0].ptr, 3), NULL, 10);
        if(argc > 4) nchunks   = strtoull(getArgv(depv[0].ptr, 4), NULL, 10);
        if(argc > 5) places    = strtoull(getArgv(depv[0].ptr, 5), NULL, 10);
    }
    if(arraySize == 0) arraySize = ARRAY_SIZE;
    if(range == 0)     range     = RANGE;
    if(nbuckets == 0)  nbuckets  = DEFAULT_BUCKETS;
    if(nchunks == 0)   nchunks   = DEFAULT_CHUNKS;
    if(places < 1)     places    = 1;
    if(nbuckets > arraySize) nbuckets = arraySize;
    if(nchunks > arraySize)  nchunks  = arraySize;
    //A place with no bucket or no chunk would own nothing.  Note what is NOT
    //here: the machine.  The decomposition is fixed by argument, so the task
    //and datablock counts are the same in every geometry.
    if(places > nbuckets) places = nbuckets;
    if(places > nchunks)  places = nchunks;

    u64 g, s, a;
    u64 nsamples = SAMPLES_PER_BUCKET * nbuckets;
    if(nsamples > arraySize) nsamples = arraySize;
    u64 nsamplers = nchunks < nsamples ? nchunks : nsamples;

    ocrGuid_t finishTemplate, finishEdt;
    u64 finishPrm[2] = {arraySize, places};
    ocrEdtTemplateCreate(&finishTemplate, finishTask, 2, (u32)places);
    ocrEdtCreate(&finishEdt, finishTemplate, EDT_PARAM_DEF, finishPrm,
                 EDT_PARAM_DEF, NULL, EDT_PROP_NONE, NULL_HINT, NULL);
    ocrEdtTemplateDestroy(finishTemplate);

    //one exchange event per (source place, wave, destination place), plus one
    //verdict per place.  The wave count comes from waveCount, which is what
    //placeInitTask decodes the grid with.
    //
    //The exchange events are STICKY, and that is a correctness requirement
    //rather than a preference.  A ONCE event delivers to the waiters registered
    //when it is satisfied and is gone afterwards, but the consumer here
    //registers inside the DESTINATION place's own init task while the producer
    //satisfies from the SOURCE place's pack -- two tasks on two ranks whose
    //only common ancestor is this one, so nothing orders the registration
    //before the satisfy.  A sticky event delivers to a late registrant; the
    //consuming unpack destroys it, since a sticky event does not reclaim
    //itself.
    u64 nwave = waveCount(nchunks, places);
    ocrGuid_t *xevt =
        (ocrGuid_t*)malloc(places * nwave * places * sizeof(ocrGuid_t));
    ocrGuid_t *devt = (ocrGuid_t*)malloc(places * sizeof(ocrGuid_t));
    for(g = 0; g < places; g++) {
        for(s = 0; s < nwave * places; s++)
            ocrEventCreate(&xevt[g * nwave * places + s], OCR_EVENT_STICKY_T,
                           EVT_PROP_TAKES_ARG);
        ocrEventCreate(&devt[g], OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);
        ocrAddDependence(devt[g], finishEdt, (u32)g, DB_MODE_RO);
    }

    //the splitters: every chunk and every unpack reads the same block, so the
    //event serves many consumers
    ocrGuid_t splitEvt;
    ocrEventCreate(&splitEvt, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);

    ocrGuid_t placeTml;
    u64 prmc = 8 + 2 * places * nwave;
    u64 *prm = (u64*)malloc(prmc * sizeof(u64));
    ocrEdtTemplateCreate(&placeTml, placeInitTask, (u32)prmc, 0);
    for(g = 0; g < places; g++) {
        ocrHint_t h = edtHintAt(placeRank(g, places));
        prm[0] = g; prm[1] = arraySize; prm[2] = range; prm[3] = nbuckets;
        prm[4] = nchunks; prm[5] = places;
        prm[6] = (u64)splitEvt.guid; prm[7] = (u64)devt[g].guid;
        //what g sends: [destination place][wave]; what g gets: one per
        //(source place, wave)
        for(s = 0; s < places; s++)
            for(a = 0; a < nwave; a++) {
                prm[8 + s * nwave + a] = (u64)xevt[(g * nwave + a) * places + s].guid;
                prm[8 + places * nwave + s * nwave + a] =
                    (u64)xevt[(s * nwave + a) * places + g].guid;
            }
        ocrGuid_t placeEdt;
        ocrEdtCreate(&placeEdt, placeTml, EDT_PARAM_DEF, prm, EDT_PARAM_DEF, NULL,
                     EDT_PROP_NONE, &h, NULL);
    }
    ocrEdtTemplateDestroy(placeTml);
    free(prm);

    //the splitters themselves, drawn in parallel and cut by one task
    ocrGuid_t splitterTemplate, splitterEdt;
    u64 splitterPrm[2] = {nbuckets, (u64)splitEvt.guid};
    ocrEdtTemplateCreate(&splitterTemplate, splitterTask, 2, (u32)nsamplers);
    ocrEdtCreate(&splitterEdt, splitterTemplate, EDT_PARAM_DEF, splitterPrm,
                 EDT_PARAM_DEF, NULL, EDT_PROP_NONE, NULL_HINT, NULL);
    ocrEdtTemplateDestroy(splitterTemplate);

    ocrGuid_t sampleTemplate;
    ocrEdtTemplateCreate(&sampleTemplate, sampleTask, 6, 0);
    for(g = 0; g < nsamplers; g++) {
        ocrGuid_t sampleEvt, sampleEdt;
        ocrEventCreate(&sampleEvt, OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);
        ocrAddDependence(sampleEvt, splitterEdt, (u32)g, DB_MODE_RO);

        u64 samplePrm[6] = {(g * nsamples) / nsamplers,
                            ((g + 1) * nsamples) / nsamplers,
                            arraySize, nsamples, range, (u64)sampleEvt.guid};
        ocrHint_t h = edtHintAt(placeRank((g * places) / nsamplers, places));
        ocrEdtCreate(&sampleEdt, sampleTemplate, EDT_PARAM_DEF, samplePrm,
                     EDT_PARAM_DEF, NULL, EDT_PROP_NONE, &h, NULL);
    }
    ocrEdtTemplateDestroy(sampleTemplate);

    free(xevt);
    free(devt);
    return NULL_GUID;
}
