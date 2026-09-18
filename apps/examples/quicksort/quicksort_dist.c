/* Based on quicksort.c - distributed variant: sample-splitter p-way partition
 * (PSRS/samplesort lineage) on a place-persistent decomposition.  A sorted
 * sample of the input gives ascending splitters, every element is routed to the
 * bucket its value falls in, and each bucket sorts what it receives; the
 * buckets are in splitter order, so concatenating them is the sorted array.
 * The finisher validates global sortedness and the permutation-preserving
 * element sum.
 *
 * The program is SPMD over a fixed number of PLACES, not a centrally built task
 * graph.  mainEdt creates one task per place, one sampler per place, the
 * splitter task and the events the places hand data through, and nothing else;
 * each place then builds its own chunks, its own exchange and its own buckets
 * where it runs.  A graph built in one place cannot scale -- every creation
 * carrying a remote affinity is a message, so the builder's cost grows with the
 * machine instead of shrinking with it.
 *
 * The place count is an argument, not the rank count.  The program is written
 * for a fixed decomposition and the runtime maps it onto whatever ranks exist,
 * so the task and datablock counts, and the order the partial sums combine in,
 * are the same in every geometry.
 *
 * The exchange is aggregated per place, which is what a distributed sample sort
 * does: a place packs everything it owes a peer into one buffer and sends it
 * once, so the exchange is P(P-1) buffers per wave whatever the bucket count.
 * Expressed per bucket it would be one buffer per (chunk, bucket) pair --
 * quadratic in a quantity chosen for compute parallelism.  Every stage that
 * touches an element is per (place, peer, wave) or per chunk or per bucket, so
 * none of them is narrower than the decomposition; the stages that are one per
 * place do arithmetic over the counts and never over the elements.
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

//Index-seeded value generator: a pure function of (index, range) carrying no
//state between calls, so any task can produce any range of the input and the
//multiset does not depend on how the array was divided among its producers.
//Identical to the recursive variant's, so both sort the same input for a given
//size and range.
static u64 genValue(u64 index, u64 range) {
    u64 z = (index + 1) * 0x9E3779B97F4A7C15UL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9UL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBUL;
    z = z ^ (z >> 31);
    return z % range;
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
// Draw this sampler's share of the splitter sample and sort it.  The sample is
// drawn in the same pieces the input is, and the shares arrive sorted so the
// task that cuts the splitters merges runs instead of sorting the whole sample.
ocrGuid_t sampleTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 lo = paramv[0], hi = paramv[1], arraySize = paramv[2];
    u64 nsamples = paramv[3], range = paramv[4];
    ocrGuid_t evt = (ocrGuid_t){.guid = paramv[5]};
    u64 n = hi - lo, i, span = nsamples > 1 ? nsamples - 1 : 1;

    ocrGuid_t db;
    u64 *out;
    //layout: [0]=count, [1..count]=values, ascending
    ocrDbCreate(&db, (void**)&out, (n + 1) * sizeof(u64), 0, NULL_HINT, NO_ALLOC);
    out[0] = n;
    for(i = 0; i < n; i++)
        out[1 + i] = genValue(((lo + i) * (arraySize - 1)) / span, range);
    quicksortLocal(out + 1, n);
    ocrDbRelease(db);
    ocrEventSatisfy(evt, db);
    return NULL_GUID;
}

//Move the run at heap position i down until the subtree rooted there is again
//a min-heap ordered by the value each run currently exposes.  A run appears in
//the heap only while it still has an unconsumed element, so heap[] positions
//always dereference.
static void heapSiftDown(ocrEdtDep_t depv[], const u64 *head, u64 *heap, u64 hn,
                         u64 i) {
    u64 run = heap[i];
    u64 key = ((const u64*)depv[run].ptr)[head[run]];
    for(;;) {
        u64 c = 2 * i + 1, ck;
        if(c >= hn) break;
        ck = ((const u64*)depv[heap[c]].ptr)[head[heap[c]]];
        if(c + 1 < hn) {
            u64 rk = ((const u64*)depv[heap[c + 1]].ptr)[head[heap[c + 1]]];
            if(rk < ck) { c++; ck = rk; }
        }
        if(ck >= key) break;
        heap[i] = heap[c];
        i = c;
    }
    heap[i] = run;
}

// paramv: {nbuckets, evt}
// depv 0..depc-1: the samplers' sorted runs
// Merge the runs and cut the merged sample into the nbuckets-1 ascending
// splitters every later task classifies against.  One task builds them and
// every place reads the same block, so every place cuts identical buckets.
// This is the one width-1 stage on the critical path, so its serial term is
// the thing to bound: the merge takes one heap step per sample element, i.e.
// O(sample * log(fan-in)) rather than a scan of every run head per element,
// and the fan-in is the place count rather than the chunk count.  The sample
// size itself is chosen per bucket, so the term IS linear in the bucket count.
ocrGuid_t splitterTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 nbuckets = paramv[0];
    ocrGuid_t evt = (ocrGuid_t){.guid = paramv[1]};
    u64 c, b, ns = 0, pos, hn = 0;
    for(c = 0; c < depc; c++) ns += ((const u64*)depv[c].ptr)[0];

    u64 *sample = (u64*)malloc((ns ? ns : 1) * sizeof(u64));
    u64 *head = (u64*)malloc((depc ? depc : 1) * sizeof(u64));
    u64 *heap = (u64*)malloc((depc ? depc : 1) * sizeof(u64));
    for(c = 0; c < depc; c++) {
        head[c] = 1;
        if(((const u64*)depv[c].ptr)[0] > 0) heap[hn++] = c;
    }
    for(c = hn; c-- > 0; ) heapSiftDown(depv, head, heap, hn, c);
    for(pos = 0; pos < ns; pos++) {
        u64 run = heap[0];
        const u64 *s = (const u64*)depv[run].ptr;
        sample[pos] = s[head[run]];
        head[run]++;
        if(head[run] > s[0]) {
            heap[0] = heap[--hn];
            if(hn > 1) heapSiftDown(depv, head, heap, hn, 0);
        } else {
            heapSiftDown(depv, head, heap, hn, 0);
        }
    }
    free(heap);
    free(head);
    for(c = 0; c < depc; c++) ocrDbDestroy(depv[c].guid);

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

// paramv: {nbuckets, evt}
// depv 0: the splitters (RO)
// Republish the splitters as a block of this place's own.  Every chunk, count
// and scatter of a place classifies against them, so without this each of them
// registers on and reads the one object the task that cut them produced, and
// that object's reader set grows with the whole decomposition rather than with
// the place count.
ocrGuid_t splitLocalTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 nbuckets = paramv[0];
    ocrGuid_t evt = (ocrGuid_t){.guid = paramv[1]};
    const u64 *in = (const u64*)depv[0].ptr;
    u64 n = nbuckets > 1 ? nbuckets - 1 : 1, i;

    ocrGuid_t db;
    u64 *out;
    ocrDbCreate(&db, (void**)&out, n * sizeof(u64), 0, NULL_HINT, NO_ALLOC);
    for(i = 0; i < n; i++) out[i] = in[i];
    ocrDbRelease(db);
    ocrEventSatisfy(evt, db);
    return NULL_GUID;
}

// paramv: {low, high(excl), range, nbuckets, places, evt}
// depv 0: splitters (RO)
// Generate this chunk's own slice of the input and group it by destination
// place, so a pack takes a peer's share as a contiguous run.  The generator is
// a pure function of the index, so the values are the ones the recursive
// program produces; what that removes is a fill of the whole input in one task
// and a block every chunk had to acquire.
//
// Layout: [sum][places counts][elements, destination-place major].  The chunk's
// own element sum rides in the block so no two chunks share a writable object
// -- an N-wide exclusive fan-out onto one block costs more than the sort.
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
        u64 v = genValue(low + i, range);
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

// paramv: {g, q, places, sendEvt}
// depv 0..ncnk-1: this wave's chunks, in order
// The exchange, aggregated: everything one place owes ONE peer in one wave goes
// into one block and is sent once.  One task per (place, wave, peer) rather
// than one per (place, wave), so the copy is as wide as the exchange grid
// instead of as wide as the place count.
ocrGuid_t packTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 g = paramv[0], q = paramv[1], places = paramv[2];
    ocrGuid_t sendEvt = (ocrGuid_t){.guid = paramv[3]};
    u64 ncnk = depc, c, p, i, total = 0;

    for(c = 0; c < ncnk; c++) total += ((const u64*)depv[c].ptr)[1 + q];

    ocrGuid_t db;
    u64 *out;
    ocrHint_t h = dbHintAt(placeRank(q, places));
    //layout: [0]=summed chunk sums (only meaningful to the owner), then
    //[1]=count and the elements
    ocrDbCreate(&db, (void**)&out, (2 + total) * sizeof(u64), 0, &h, NO_ALLOC);
    out[0] = 0;
    out[1] = total;
    u64 at = 2;
    for(c = 0; c < ncnk; c++) {
        const u64 *in = (const u64*)depv[c].ptr;
        u64 off = 1 + places;
        for(p = 0; p < q; p++) off += in[1 + p];
        for(i = 0; i < in[1 + q]; i++) out[at++] = in[off + i];
        //the input sum is reported once, on the block this place sends itself
        if(q == g) out[0] += in[0];
    }
    ocrDbRelease(db);
    ocrEventSatisfy(sendEvt, db);
    return NULL_GUID;
}

// paramv: {ncnk, chunkEvt[ncnk]}
// depv 0..ncnk-1: this wave's chunks; the rest: every pack that read them
// A chunk is read by one pack per peer, so no reader can free it; the wave's
// generated form is released here, once the last of them has run.
ocrGuid_t reapTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 ncnk = paramv[0], i;
    const u64 *evt = paramv + 1;
    for(i = 0; i < ncnk; i++) {
        ocrDbDestroy(depv[i].guid);
        ocrEventDestroy((ocrGuid_t){.guid = evt[i]});
    }
    return NULL_GUID;
}

// paramv: {g, places, nbuckets, evt}
// depv 0: one arrival (RO); depv 1: splitters (RO)
// Count what one arrival holds for each of this place's buckets.  Classifying
// is per-element work, so it is done once per arrival rather than once per
// place.
ocrGuid_t countTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 g = paramv[0], places = paramv[1], nbuckets = paramv[2];
    ocrGuid_t evt = (ocrGuid_t){.guid = paramv[3]};
    const u64 *in = (const u64*)depv[0].ptr;
    const u64 *splitters = (const u64*)depv[1].ptr;
    u64 lo, hi;
    placeRange(g, nbuckets, places, &lo, &hi);
    u64 bpg = hi - lo, i;

    ocrGuid_t db;
    u64 *out;
    //layout: [0]=this arrival's share of the input sum, [1..bpg]=bucket counts
    ocrDbCreate(&db, (void**)&out, (1 + bpg) * sizeof(u64), 0, NULL_HINT,
                NO_ALLOC);
    for(i = 0; i <= bpg; i++) out[i] = 0;
    out[0] = in[0];
    for(i = 0; i < in[1]; i++)
        out[1 + bucketOf(in[2 + i], splitters, nbuckets) - lo]++;
    ocrDbRelease(db);
    ocrEventSatisfy(evt, db);
    return NULL_GUID;
}

// paramv: {g, places, nbuckets, narr, placeEvt, offEvt, sumEvt}
// depv 0..narr-1: the per-arrival counts
// Lay this place's elements out: the buckets in splitter order, and inside a
// bucket the arrivals in order.  This is the one task per place in the
// redistribution, and it touches counts only -- its work is the exchange grid,
// never the elements -- so it is the same size whatever the array is.
ocrGuid_t offsetTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 g = paramv[0], places = paramv[1], nbuckets = paramv[2], narr = paramv[3];
    ocrGuid_t placeEvt = (ocrGuid_t){.guid = paramv[4]};
    ocrGuid_t offEvt = (ocrGuid_t){.guid = paramv[5]};
    ocrGuid_t sumEvt = (ocrGuid_t){.guid = paramv[6]};
    u64 lo, hi;
    placeRange(g, nbuckets, places, &lo, &hi);
    u64 bpg = hi - lo, a, b, total = 0, inputSum = 0;

    ocrGuid_t offDb;
    u64 *off;
    //layout: [b]=bucket start, [bpg+b]=bucket count,
    //[2*bpg + a*bpg + b]=where arrival a writes its share of bucket b
    ocrDbCreate(&offDb, (void**)&off, (2 * bpg + narr * bpg) * sizeof(u64), 0,
                NULL_HINT, NO_ALLOC);
    for(b = 0; b < bpg; b++) off[bpg + b] = 0;
    for(a = 0; a < narr; a++) {
        const u64 *c = (const u64*)depv[a].ptr;
        inputSum += c[0];
        for(b = 0; b < bpg; b++) off[bpg + b] += c[1 + b];
    }
    for(b = 0; b < bpg; b++) { off[b] = total; total += off[bpg + b]; }
    for(b = 0; b < bpg; b++) {
        u64 at = off[b];
        for(a = 0; a < narr; a++) {
            const u64 *c = (const u64*)depv[a].ptr;
            off[2 * bpg + a * bpg + b] = at;
            at += c[1 + b];
        }
    }
    for(a = 0; a < narr; a++) ocrDbDestroy(depv[a].guid);

    ocrGuid_t placeDb;
    u64 *elems;
    ocrDbCreate(&placeDb, (void**)&elems, (total ? total : 1) * sizeof(u64), 0,
                NULL_HINT, NO_ALLOC);
    ocrGuid_t sumDb;
    u64 *sum;
    ocrDbCreate(&sumDb, (void**)&sum, sizeof(u64), 0, NULL_HINT, NO_ALLOC);
    sum[0] = inputSum;

    ocrDbRelease(offDb);
    ocrDbRelease(placeDb);
    ocrDbRelease(sumDb);
    ocrEventSatisfy(offEvt, offDb);
    ocrEventSatisfy(placeEvt, placeDb);
    ocrEventSatisfy(sumEvt, sumDb);
    return NULL_GUID;
}

// paramv: {g, places, nbuckets, a, arrivalEvt}
// depv 0: this arrival (RO); 1: the offsets (RO); 2: the place's array (RW);
// 3: splitters (RO)
// Move one arrival into the place's array at the offsets already computed for
// it.  The arrivals write disjoint runs, so they are independent of each other
// and the stage is as wide as the exchange grid.
ocrGuid_t scatterTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 g = paramv[0], places = paramv[1], nbuckets = paramv[2], a = paramv[3];
    ocrGuid_t arrivalEvt = (ocrGuid_t){.guid = paramv[4]};
    const u64 *in = (const u64*)depv[0].ptr;
    const u64 *off = (const u64*)depv[1].ptr;
    u64 *dst = (u64*)depv[2].ptr;
    const u64 *splitters = (const u64*)depv[3].ptr;
    u64 lo, hi;
    placeRange(g, nbuckets, places, &lo, &hi);
    u64 bpg = hi - lo, i, b;

    u64 *at = (u64*)malloc((bpg ? bpg : 1) * sizeof(u64));
    for(b = 0; b < bpg; b++) at[b] = off[2 * bpg + a * bpg + b];
    for(i = 0; i < in[1]; i++) {
        u64 v = in[2 + i];
        dst[at[bucketOf(v, splitters, nbuckets) - lo]++] = v;
    }
    free(at);

    ocrDbDestroy(depv[0].guid);
    /* The exchange events are sticky, so they do not reclaim themselves when
     * they fire.  This task is the last of the arrival's readers, so it is the
     * one place that can know the event is finished with. */
    ocrEventDestroy(arrivalEvt);
    return NULL_GUID;
}

// paramv: {sortEvt}
// depv 0: the place's array; the rest: every scatter into it
// The one edge between filling the place's array and sorting inside it, so the
// buckets take one dependence each rather than one per arrival.
ocrGuid_t readyTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrEventSatisfy((ocrGuid_t){.guid = paramv[0]}, depv[0].guid);
    return NULL_GUID;
}

// paramv: {k, bpg, evt}
// depv 0: the place's array (RW); depv 1: the offsets (RO)
// The key range is down to one bucket and the bucket's elements are a
// contiguous run of the place's array, so sort the run in place and report the
// five words the finisher checks.  The bucket checks itself -- it is the only
// task that sees these elements together.
ocrGuid_t bucketTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 k = paramv[0], bpg = paramv[1];
    ocrGuid_t evt = (ocrGuid_t){.guid = paramv[2]};
    const u64 *off = (const u64*)depv[1].ptr;
    u64 *a = (u64*)depv[0].ptr + off[k];
    u64 total = off[bpg + k], i;

    quicksortLocal(a, total);

    ocrGuid_t sumDb;
    u64 *v;
    //layout: [0]=count, [1]=sum, [2]=sorted, [3]=first, [4]=last
    ocrDbCreate(&sumDb, (void**)&v, 5 * sizeof(u64), 0, NULL_HINT, NO_ALLOC);
    {   u64 acc = 0, sorted = 1;
        for(i = 0; i < total; i++) {
            if(i && a[i] < a[i-1]) sorted = 0;
            acc += a[i];
        }
        v[0] = total; v[1] = acc; v[2] = sorted;
        v[3] = total ? a[0] : 0;
        v[4] = total ? a[total-1] : 0;
    }
    ocrDbRelease(sumDb);
    ocrEventSatisfy(evt, sumDb);
    return NULL_GUID;
}

// paramv: {doneEvt, placeEvt, offEvt, sortEvt, localSplitEvt}
// depv 0..bpg-1: this place's bucket verdicts; bpg: its gathered input sum;
// bpg+1: the place's array; bpg+2: the offsets; bpg+3: its splitter copy
// Combine this place's buckets into one verdict, so the finisher sees P of
// them rather than one per bucket, and release what the place held.
ocrGuid_t placeJoinTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrGuid_t doneEvt = (ocrGuid_t){.guid = paramv[0]};
    u64 bpg = depc - 4, i;

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
    //the checks are computed and nothing reads these elements again: the
    //finisher works from the verdicts, so holding them would keep a copy of
    //the whole array alive for no reader
    ocrDbDestroy(depv[bpg + 1].guid);
    ocrDbDestroy(depv[bpg + 2].guid);
    ocrDbDestroy(depv[bpg + 3].guid);
    for(i = 1; i <= 4; i++)
        ocrEventDestroy((ocrGuid_t){.guid = paramv[i]});

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
//          send[places*nwave], recv[places*nwave]}
// A place's whole program, built where it runs.  The builder never reaches
// across the machine, so what it costs does not depend on how many ranks there
// are -- which is the difference between a program that scales and one that
// does not.
ocrGuid_t placeInitTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 g = paramv[0], arraySize = paramv[1], range = paramv[2];
    u64 nbuckets = paramv[3], nchunks = paramv[4], places = paramv[5];
    u64 splitEvt = paramv[6], doneEvt = paramv[7];
    u64 clo, chi, blo, bhi, k, a, w, q;
    placeRange(g, nchunks, places, &clo, &chi);
    placeRange(g, nbuckets, places, &blo, &bhi);
    u64 cpg = chi - clo, bpg = bhi - blo;
    u64 nwave = waveCount(nchunks, places);
    u64 narr = places * nwave;
    const u64 *send = paramv + 8;             //[destination place][wave]
    const u64 *recv = paramv + 8 + narr;      //[source place][wave]
    ocrHint_t h = edtHintAt(placeRank(g, places));

    //the place's array, its layout, its own copy of the splitters and the edge
    //that says the array is filled
    ocrGuid_t placeEvt, offEvt, sortEvt, sumEvt, splitLocalEvt;
    ocrEventCreate(&placeEvt, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);
    ocrEventCreate(&offEvt, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);
    ocrEventCreate(&sortEvt, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);
    ocrEventCreate(&sumEvt, OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);
    ocrEventCreate(&splitLocalEvt, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);

    ocrGuid_t splitLocalTml, splitLocalEdt;
    u64 splitLocalPrm[2] = {nbuckets, (u64)splitLocalEvt.guid};
    ocrEdtTemplateCreate(&splitLocalTml, splitLocalTask, 2, 1);
    ocrEdtCreate(&splitLocalEdt, splitLocalTml, EDT_PARAM_DEF, splitLocalPrm,
                 EDT_PARAM_DEF, NULL, EDT_PROP_NONE, &h, NULL);
    ocrEdtTemplateDestroy(splitLocalTml);
    ocrAddDependence((ocrGuid_t){.guid = splitEvt}, splitLocalEdt, 0, DB_MODE_RO);

    //the join, so the buckets below have somewhere to report
    ocrGuid_t joinTml, joinEdt;
    u64 joinPrm[5] = {doneEvt, (u64)placeEvt.guid, (u64)offEvt.guid,
                      (u64)sortEvt.guid, (u64)splitLocalEvt.guid};
    ocrEdtTemplateCreate(&joinTml, placeJoinTask, 5, (u32)(bpg + 4));
    ocrEdtCreate(&joinEdt, joinTml, EDT_PARAM_DEF, joinPrm, EDT_PARAM_DEF, NULL,
                 EDT_PROP_NONE, &h, NULL);
    ocrEdtTemplateDestroy(joinTml);
    ocrAddDependence(sumEvt, joinEdt, (u32)bpg, DB_MODE_RO);
    ocrAddDependence(placeEvt, joinEdt, (u32)(bpg + 1), DB_MODE_RO);
    ocrAddDependence(offEvt, joinEdt, (u32)(bpg + 2), DB_MODE_RO);
    ocrAddDependence(splitLocalEvt, joinEdt, (u32)(bpg + 3), DB_MODE_RO);

    //the buckets: each sorts its own run of the place's array in place
    ocrGuid_t bucketTml;
    ocrEdtTemplateCreate(&bucketTml, bucketTask, 3, 2);
    for(k = 0; k < bpg; k++) {
        ocrGuid_t outEvt, bucketEdt;
        ocrEventCreate(&outEvt, OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);
        ocrAddDependence(outEvt, joinEdt, (u32)k, DB_MODE_RO);

        u64 bucketPrm[3] = {k, bpg, (u64)outEvt.guid};
        ocrEdtCreate(&bucketEdt, bucketTml, EDT_PARAM_DEF, bucketPrm,
                     EDT_PARAM_DEF, NULL, EDT_PROP_NONE, &h, NULL);
        ocrAddDependence(sortEvt, bucketEdt, 0, DB_MODE_RW);
        ocrAddDependence(offEvt, bucketEdt, 1, DB_MODE_RO);
    }
    ocrEdtTemplateDestroy(bucketTml);

    //the edge from the last scatter to the buckets
    ocrGuid_t readyTml, readyEdt;
    u64 readyPrm[1] = {(u64)sortEvt.guid};
    ocrEdtTemplateCreate(&readyTml, readyTask, 1, (u32)(1 + narr));
    ocrEdtCreate(&readyEdt, readyTml, EDT_PARAM_DEF, readyPrm, EDT_PARAM_DEF,
                 NULL, EDT_PROP_NONE, &h, NULL);
    ocrEdtTemplateDestroy(readyTml);
    ocrAddDependence(placeEvt, readyEdt, 0, DB_MODE_RO);

    //the scatters, one per arrival
    ocrGuid_t scatterTml;
    ocrEdtTemplateCreate(&scatterTml, scatterTask, 5, 4);
    for(a = 0; a < narr; a++) {
        ocrGuid_t scatterEdt, scatterOut;
        u64 scatterPrm[5] = {g, places, nbuckets, a, recv[a]};
        ocrEdtCreate(&scatterEdt, scatterTml, EDT_PARAM_DEF, scatterPrm,
                     EDT_PARAM_DEF, NULL, EDT_PROP_NONE, &h, &scatterOut);
        ocrAddDependence(scatterOut, readyEdt, (u32)(1 + a), DB_MODE_NULL);
        ocrAddDependence(offEvt, scatterEdt, 1, DB_MODE_RO);
        ocrAddDependence(placeEvt, scatterEdt, 2, DB_MODE_RW);
        ocrAddDependence(splitLocalEvt, scatterEdt, 3, DB_MODE_RO);
        ocrAddDependence((ocrGuid_t){.guid = recv[a]}, scatterEdt, 0, DB_MODE_RO);
    }
    ocrEdtTemplateDestroy(scatterTml);

    //the layout task, and the per-arrival counts it reads
    ocrGuid_t offsetTml, offsetEdt;
    u64 offsetPrm[7] = {g, places, nbuckets, narr, (u64)placeEvt.guid,
                        (u64)offEvt.guid, (u64)sumEvt.guid};
    ocrEdtTemplateCreate(&offsetTml, offsetTask, 7, (u32)narr);
    ocrEdtCreate(&offsetEdt, offsetTml, EDT_PARAM_DEF, offsetPrm, EDT_PARAM_DEF,
                 NULL, EDT_PROP_NONE, &h, NULL);
    ocrEdtTemplateDestroy(offsetTml);

    ocrGuid_t countTml;
    ocrEdtTemplateCreate(&countTml, countTask, 4, 2);
    for(a = 0; a < narr; a++) {
        ocrGuid_t cEvt, countEdt;
        ocrEventCreate(&cEvt, OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);
        ocrAddDependence(cEvt, offsetEdt, (u32)a, DB_MODE_RO);

        u64 countPrm[4] = {g, places, nbuckets, (u64)cEvt.guid};
        ocrEdtCreate(&countEdt, countTml, EDT_PARAM_DEF, countPrm, EDT_PARAM_DEF,
                     NULL, EDT_PROP_NONE, &h, NULL);
        ocrAddDependence(splitLocalEvt, countEdt, 1, DB_MODE_RO);
        ocrAddDependence((ocrGuid_t){.guid = recv[a]}, countEdt, 0, DB_MODE_RO);
    }
    ocrEdtTemplateDestroy(countTml);

    //The pack runs in waves, and one pack per peer within a wave: the copy is
    //then as wide as the exchange grid, and a wave's chunks are released as
    //soon as every peer has taken its share of them.
    ocrGuid_t chunkTml;
    ocrEdtTemplateCreate(&chunkTml, chunkTask, 6, 1);
    for(w = 0; w < nwave; w++) {
        u64 wlo = clo + (w * cpg) / nwave, whi = clo + ((w + 1) * cpg) / nwave;
        u64 ncnk = whi - wlo;
        ocrGuid_t *cevt = (ocrGuid_t*)malloc((ncnk ? ncnk : 1) * sizeof(ocrGuid_t));
        for(k = 0; k < ncnk; k++)
            ocrEventCreate(&cevt[k], OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);

        ocrGuid_t reapTml, reapEdt;
        u64 *reapPrm = (u64*)malloc((1 + (ncnk ? ncnk : 1)) * sizeof(u64));
        reapPrm[0] = ncnk;
        for(k = 0; k < ncnk; k++) reapPrm[1 + k] = (u64)cevt[k].guid;
        ocrEdtTemplateCreate(&reapTml, reapTask, (u32)(1 + ncnk),
                             (u32)(ncnk + places));
        ocrEdtCreate(&reapEdt, reapTml, EDT_PARAM_DEF, reapPrm, EDT_PARAM_DEF,
                     NULL, EDT_PROP_NONE, &h, NULL);
        ocrEdtTemplateDestroy(reapTml);
        free(reapPrm);

        ocrGuid_t packTml;
        ocrEdtTemplateCreate(&packTml, packTask, 4, (u32)ncnk);
        for(q = 0; q < places; q++) {
            ocrGuid_t packEdt, packOut;
            u64 packPrm[4] = {g, q, places, send[q * nwave + w]};
            ocrEdtCreate(&packEdt, packTml, EDT_PARAM_DEF, packPrm,
                         EDT_PARAM_DEF, NULL, EDT_PROP_NONE, &h, &packOut);
            ocrAddDependence(packOut, reapEdt, (u32)(ncnk + q), DB_MODE_NULL);
            for(k = 0; k < ncnk; k++)
                ocrAddDependence(cevt[k], packEdt, (u32)k, DB_MODE_RO);
        }
        ocrEdtTemplateDestroy(packTml);
        for(k = 0; k < ncnk; k++)
            ocrAddDependence(cevt[k], reapEdt, (u32)k, DB_MODE_RO);

        for(k = wlo; k < whi; k++) {
            ocrGuid_t chunkEdt;
            u64 chunkPrm[6] = {(k * arraySize) / nchunks,
                               ((k + 1) * arraySize) / nchunks,
                               range, nbuckets, places, (u64)cevt[k - wlo].guid};
            ocrEdtCreate(&chunkEdt, chunkTml, EDT_PARAM_DEF, chunkPrm,
                         EDT_PARAM_DEF, NULL, EDT_PROP_NONE, &h, NULL);
            ocrAddDependence(splitLocalEvt, chunkEdt, 0, DB_MODE_RO);
        }
        free(cevt);
    }
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

// The whole of the central work: the events the places hand data through, one
// sampler per place, the task that cuts the splitters, and one task per place.
// P*P*W + P + 1 events and 2P + 2 tasks -- nothing here grows with the bucket
// or chunk count, which is what lets those be chosen for parallelism.
ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 arraySize = ARRAY_SIZE;
    u64 range = RANGE;
    u64 nbuckets = DEFAULT_BUCKETS, nchunks = DEFAULT_CHUNKS;
    u64 places = DEFAULT_PLACES;
    u64 argc = 0;
    if(depc >= 1 && depv[0].ptr) argc = getArgc(depv[0].ptr);
    if(argc > 6) {
        ocrPrintf("QSORT_ARG_ERROR at most five arguments: "
                  "arraySize range nbuckets nchunks places\n");
        ocrShutdown();
        return NULL_GUID;
    }
    if(argc > 1) arraySize = strtoull(getArgv(depv[0].ptr, 1), NULL, 10);
    if(argc > 2) range     = strtoull(getArgv(depv[0].ptr, 2), NULL, 10);
    if(argc > 3) nbuckets  = strtoull(getArgv(depv[0].ptr, 3), NULL, 10);
    if(argc > 4) nchunks   = strtoull(getArgv(depv[0].ptr, 4), NULL, 10);
    if(argc > 5) places    = strtoull(getArgv(depv[0].ptr, 5), NULL, 10);
    if(arraySize == 0 || range == 0 || nbuckets == 0 || nchunks == 0 ||
       places == 0) {
        ocrPrintf("QSORT_ARG_ERROR every argument must be positive\n");
        ocrShutdown();
        return NULL_GUID;
    }
    //A place with no bucket or no chunk would own nothing, and a bucket or a
    //chunk per element is as far as the decomposition can go.  Note what is
    //NOT here: the machine.  The decomposition is fixed by argument, so the
    //task and datablock counts are the same in every geometry.
    if(nbuckets > arraySize || nchunks > arraySize || places > nbuckets ||
       places > nchunks) {
        ocrPrintf("QSORT_ARG_ERROR need places <= nbuckets <= arraySize and "
                  "places <= nchunks <= arraySize\n");
        ocrShutdown();
        return NULL_GUID;
    }

    u64 g, s, a;
    //Stratifying the sample, cutting the chunk bounds and cutting the splitters
    //each multiply an index by a count before dividing, so the representable
    //size is set by those products and not by the width of the index type.
    if(nbuckets > (~(u64)0) / SAMPLES_PER_BUCKET ||
       nchunks > (~(u64)0) / arraySize) {
        ocrPrintf("QSORT_ARG_ERROR arraySize * nchunks and "
                  "SAMPLES_PER_BUCKET * nbuckets must fit in 64 bits\n");
        ocrShutdown();
        return NULL_GUID;
    }
    u64 nsamples = SAMPLES_PER_BUCKET * nbuckets;
    if(nsamples > arraySize) nsamples = arraySize;
    if(nsamples < places) nsamples = places;
    if(nsamples > (~(u64)0) / arraySize || nsamples > (~(u64)0) / nbuckets) {
        ocrPrintf("QSORT_ARG_ERROR arraySize * samples and nbuckets * samples "
                  "must fit in 64 bits\n");
        ocrShutdown();
        return NULL_GUID;
    }
    u64 nwave = waveCount(nchunks, places);
    ocrPrintf("QSORT_ARGS arraySize=%lu range=%lu nbuckets=%lu nchunks=%lu "
              "places=%lu waves=%lu samples=%lu\n",
              arraySize, range, nbuckets, nchunks, places, nwave, nsamples);

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
    //consuming scatter destroys it, since a sticky event does not reclaim
    //itself.
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

    //the splitters: one consumer per place, which copies them for the place's
    //own tasks, so this event's reader set is the place count and not the
    //decomposition
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

    //the splitters themselves: one sampler per place draws and sorts its share
    //and the cut merges the sorted runs, so the fan-in is the place count and
    //the serial term is the sample size times the log of that count
    ocrGuid_t splitterTemplate, splitterEdt;
    u64 splitterPrm[2] = {nbuckets, (u64)splitEvt.guid};
    ocrEdtTemplateCreate(&splitterTemplate, splitterTask, 2, (u32)places);
    ocrEdtCreate(&splitterEdt, splitterTemplate, EDT_PARAM_DEF, splitterPrm,
                 EDT_PARAM_DEF, NULL, EDT_PROP_NONE, NULL_HINT, NULL);
    ocrEdtTemplateDestroy(splitterTemplate);

    ocrGuid_t sampleTemplate;
    ocrEdtTemplateCreate(&sampleTemplate, sampleTask, 6, 0);
    for(g = 0; g < places; g++) {
        ocrGuid_t sampleEvt, sampleEdt;
        ocrEventCreate(&sampleEvt, OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);
        ocrAddDependence(sampleEvt, splitterEdt, (u32)g, DB_MODE_RO);

        u64 samplePrm[6] = {(g * nsamples) / places,
                            ((g + 1) * nsamples) / places,
                            arraySize, nsamples, range, (u64)sampleEvt.guid};
        ocrHint_t h = edtHintAt(placeRank(g, places));
        ocrEdtCreate(&sampleEdt, sampleTemplate, EDT_PARAM_DEF, samplePrm,
                     EDT_PARAM_DEF, NULL, EDT_PROP_NONE, &h, NULL);
    }
    ocrEdtTemplateDestroy(sampleTemplate);

    free(xevt);
    free(devt);
    return NULL_GUID;
}
