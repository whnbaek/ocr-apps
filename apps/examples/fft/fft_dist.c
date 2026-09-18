/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */
/* Based on fft.c - distributed variant: Bailey four-step transpose FFT on a
 * rank-persistent decomposition.  The length-N transform is viewed as an
 * N1 x N2 matrix and computed in four steps -- an FFT down every column, a
 * twiddle, a transpose, an FFT along every row -- over a grid of t tiles, so
 * no two tasks write the same object.  Both matrix dimensions are cut by
 * index, so a tile count need not divide either dimension: any t up to the
 * smaller dimension is legal and every tile is non-empty.  A single-tone input
 * has a closed-form spectrum, so each row tile verifies its own output block
 * analytically and the partial checksums reduce to exactly N.
 *
 * The program is SPMD, not a centrally built task graph.  mainEdt creates one
 * task per place and the events the places hand data through, and nothing
 * else; each place then builds its own tiles, its own packing and its own row
 * work on its own rank.  A graph built in one place cannot scale: every
 * creation carrying a remote affinity is a message, so the builder's cost
 * grows with the rank count instead of shrinking with it.
 *
 * The transpose is aggregated per (place pair, wave), which is what the
 * distributed FFT libraries do: what a place owes a peer for one wave of its
 * tiles travels as one buffer, so the block count is places*waves*places
 * whatever the tile count -- expressed per tile pair it would be quadratic in
 * a quantity chosen for compute parallelism.  Aggregating the payload does not
 * require aggregating the WORK: one task exists per block, so the wave count
 * is what makes the exchange stage as wide as the tile stages around it, at no
 * cost in messages or in message size.
 *
 * A rendezvous event is homed where it was created -- neither endpoint -- so a
 * registration on one and every delivery from one is a message to and from
 * that rank.  Exactly one task per transfer therefore waits on one: it
 * republishes the arrival on an event created on the consuming rank, and the
 * readers of a block -- as many as the tile count makes -- register and are
 * woken there.  Traffic through the creating rank stays one message per
 * transfer instead of one per reader.
 *
 * Every object is reclaimed by the task that can know it is finished with.
 * A shared object outlives its readers, so its reaper is a task ordered after
 * all of them: a per-wave reaper frees the column tiles once every destination
 * has packed its share, and a place's join frees the arrivals once every row
 * tile of the place has read them.  Peak live data is therefore the two forms
 * of the matrix at once -- 32*N bytes -- whatever order the scheduler happens
 * to run the stages in.
 */
#include <ocr.h>
#include <extensions/ocr-affinity.h>
#include <stdlib.h>
#include <string.h>
#include "math.h"

//Input tone bin: X[TONE_BIN] = X[N-TONE_BIN] = N/2, all other bins 0.
#define TONE_BIN 5
//Relative tolerance for the analytic per-bin self-check.
#define FFT_TOL 1e-3
//Default tile count when argv gives none (clamped to the matrix dimensions).
#define FFT_DEFAULT_TILES 64
//Default number of places the transpose aggregates into.  A place is a unit of
//ownership and of communication, not a machine fact: the program is written for
//a fixed number of them and the runtime maps them onto whatever ranks exist, so
//the task and datablock counts are the same in every geometry.
#define FFT_DEFAULT_PLACES 8

//The control's ditfft2 recursion (verify.c), extended to complex input and
//double precision: the distributed four-step's row stage transforms complex
//data, and the analytic (closed-form) verification needs double accumulation.
static void ditfft2c(double *X_real, double *X_imag,
                     const double *x_real, const double *x_imag,
                     int N, int step) {
    if(N == 1) {
        X_real[0] = x_real[0];
        X_imag[0] = x_imag[0];
    } else {
        ditfft2c(X_real, X_imag, x_real, x_imag, N/2, 2 * step);
        ditfft2c(X_real+N/2, X_imag+N/2, x_real+step, x_imag+step, N/2, 2 * step);
        int k;
        for(k=0;k<N/2;k++) {
            double t_real = X_real[k];
            double t_imag = X_imag[k];
            double twiddle_real = cos(-2 * M_PI * k / N);
            double twiddle_imag = sin(-2 * M_PI * k / N);
            double xr = X_real[k+N/2];
            double xi = X_imag[k+N/2];

            // (a+bi)(c+di) = (ac - bd) + (bc + ad)i
            X_real[k] = t_real + (twiddle_real*xr - twiddle_imag*xi);
            X_imag[k] = t_imag + (twiddle_imag*xr + twiddle_real*xi);
            X_real[k+N/2] = t_real - (twiddle_real*xr - twiddle_imag*xi);
            X_imag[k+N/2] = t_imag - (twiddle_imag*xr + twiddle_real*xi);
        }
    }
}

//The rank a place is mapped onto.  This is the ONLY thing the program asks the
//machine, and it asks it for a hint -- the decomposition above it is fixed by
//argument, so where a place lands changes nothing about what the program is.
static u64 placeRank(u64 place, u64 places) {
    u64 nranks = 1;
    ocrAffinityCount(AFFINITY_PD, &nranks);
    return (place * nranks) / places;
}

static ocrHint_t edtHintAt(u64 rank) {
    ocrGuid_t aff;
    ocrAffinityGetAt(AFFINITY_PD, rank, &aff);
    ocrHint_t h;
    ocrHintInit(&h, OCR_HINT_EDT_T);
    ocrSetHintValue(&h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
}

// Home pin for a single-use handoff block: its directory round-trip is on
// the critical path and ownership migration cannot amortize a block consumed
// exactly once, so it is created directly on its consumer's home.
static ocrHint_t dbHintAt(u64 rank) {
    ocrGuid_t aff;
    ocrAffinityGetAt(AFFINITY_PD, rank, &aff);
    ocrHint_t h;
    ocrHintInit(&h, OCR_HINT_DB_T);
    ocrSetHintValue(&h, OCR_HINT_DB_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
}

//One part's half-open share of n, split by index.  Parts need not divide n,
//and consecutive parts abut, so a run of parts is a run of indices.
static void spanOf(u64 idx, u64 n, u64 parts, u64 *lo, u64 *hi) {
    *lo = (idx * n) / parts;
    *hi = ((idx + 1) * n) / parts;
}

//How many of a place's tiles fall in one wave, and where that run starts.
static u64 waveTiles(u64 place, u64 t, u64 places, u64 wave, u64 nwave,
                     u64 *first) {
    u64 lo, hi;
    spanOf(place, t, places, &lo, &hi);
    u64 n = hi - lo;
    u64 a = lo + (wave * n) / nwave, b = lo + ((wave + 1) * n) / nwave;
    if(first) *first = a;
    return b - a;
}

//The default wave count for a decomposition.  It is chosen once, in the entry
//task, and handed to every place as a parameter -- either derived here or taken
//from an argument -- so the builder and the places index the transpose grid
//identically without the rule itself being an invariant.  One packing task
//exists per (place, wave, destination place), so this is the factor that
//carries the exchange stage up to the width of the tile stages it sits between;
//it is bounded so that a wave still holds at least one tile.
static u64 waveCount(u64 t, u64 places) {
    u64 w = (t + places * places - 1) / (places * places);
    u64 cap = t / places;
    if(w < 1) w = 1;
    if(w > cap) w = cap;
    return w;
}

// paramv: {i, m, t, evt}
// Column stage for one tile: generate its columns of the tone, run a length-N1
// FFT down each, apply the four-step twiddle, and publish the result as a
// row-major rectangle of the matrix's rows by this tile's columns.
//
// Row-major over the matrix rows is what lets the pack below take a
// destination's share as a contiguous run rather than a strided gather.
ocrGuid_t colTileTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 ti = paramv[0];
    u64 m = paramv[1];
    u64 t = paramv[2];
    ocrGuid_t evt = (ocrGuid_t){.guid = paramv[3]};
    u64 N = (u64)1 << m;
    u64 N1 = (u64)1 << ((m + 1) / 2);
    u64 N2 = N / N1;
    u64 c0, c1, ncols, cc, rr, k1;
    spanOf(ti, N2, t, &c0, &c1);
    ncols = c1 - c0;

    double *inRe = (double*)malloc(N1 * sizeof(double));
    double *inIm = (double*)malloc(N1 * sizeof(double));
    double *colRe = (double*)malloc(N1 * sizeof(double));
    double *colIm = (double*)malloc(N1 * sizeof(double));

    ocrGuid_t db;
    double *rect;
    //the block is created by the task that fills it, on its own rank
    ocrDbCreate(&db, (void**)&rect, N1 * ncols * 2 * sizeof(double), 0, NULL_HINT,
                NO_ALLOC);

    for(cc = 0; cc < ncols; cc++) {
        u64 c = c0 + cc;
        //column c of the N1 x N2 row-major matrix view: x[rr*N2 + c]
        for(rr = 0; rr < N1; rr++) {
            u64 n = rr * N2 + c;
            inRe[rr] = cos(2.0 * M_PI * (double)TONE_BIN * (double)n / (double)N);
            inIm[rr] = 0.0;
        }
        ditfft2c(colRe, colIm, inRe, inIm, (int)N1, 1);
        for(k1 = 0; k1 < N1; k1++) {
            double ang = -2.0 * M_PI * (double)k1 * (double)c / (double)N;
            double wr = cos(ang), wi = sin(ang);
            double xr = colRe[k1], xi = colIm[k1];
            rect[(k1 * ncols + cc) * 2]     = wr * xr - wi * xi;
            rect[(k1 * ncols + cc) * 2 + 1] = wi * xr + wr * xi;
        }
    }
    free(inRe);
    free(inIm);
    free(colRe);
    free(colIm);

    ocrDbRelease(db);
    ocrEventSatisfy(evt, db);
    return NULL_GUID;
}

// paramv: {g, m, t, places, wave, nwave, d, arrival event, done event}
// depv: this wave's column tiles (RO)
// One task, and one block, per (place, wave, destination place).  The payload
// is the whole of what this place owes that destination for this wave, so the
// message count does not follow the tile count; the task count does, because
// the wave count is chosen to make it.
ocrGuid_t packTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 g = paramv[0];
    u64 m = paramv[1];
    u64 t = paramv[2];
    u64 places = paramv[3];
    u64 wave = paramv[4];
    u64 nwave = paramv[5];
    u64 d = paramv[6];
    ocrGuid_t arrEvt = (ocrGuid_t){.guid = paramv[7]};
    ocrGuid_t doneEvt = (ocrGuid_t){.guid = paramv[8]};
    u64 N = (u64)1 << m;
    u64 N1 = (u64)1 << ((m + 1) / 2);
    u64 N2 = N / N1;
    u64 lod, hid, r0, r1, nrows, first, ntile, cbase, ccap, ci;

    spanOf(d, t, places, &lod, &hid);
    //the destination's row tiles abut, so its rows are one run of the matrix
    r0 = (lod * N1) / t;
    r1 = (hid * N1) / t;
    nrows = r1 - r0;
    ntile = waveTiles(g, t, places, wave, nwave, &first);
    cbase = (first * N2) / t;
    ccap = ((first + ntile) * N2) / t;

    ocrGuid_t db;
    double *out;
    //created on the rank that reads it
    ocrHint_t h = dbHintAt(placeRank(d, places));
    ocrDbCreate(&db, (void**)&out, nrows * (ccap - cbase) * 2 * sizeof(double), 0,
                &h, NO_ALLOC);

    //layout: [column tile of this wave][row of the destination][column].  The
    //source is row-major, so one destination's rows are a contiguous run.
    for(ci = 0; ci < ntile; ci++) {
        u64 c0, c1;
        spanOf(first + ci, N2, t, &c0, &c1);
        memcpy(out + (nrows * (c0 - cbase)) * 2,
               (const double*)depv[ci].ptr + (r0 * (c1 - c0)) * 2,
               nrows * (c1 - c0) * 2 * sizeof(double));
    }
    ocrDbRelease(db);
    ocrEventSatisfy(arrEvt, db);
    //the column tiles are shared with this wave's other destinations: release
    //them before signalling, so their reaper's turn cannot overlap this read
    for(ci = 0; ci < ntile; ci++) ocrDbRelease(depv[ci].guid);
    ocrEventSatisfy(doneEvt, NULL_GUID);
    return NULL_GUID;
}

// paramv: {blocks}
// depv 0..blocks-1: the blocks to reclaim (RO); the remaining slots are the
// completion signals of the tasks that shared them.
ocrGuid_t reapTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 nblk = paramv[0], i;
    for(i = 0; i < nblk; i++) ocrDbDestroy(depv[i].guid);
    return NULL_GUID;
}

// paramv: {local event}
// depv 0: the arrival this task is the sole waiter for (RO)
// The rendezvous event a producer satisfies is homed where it was created,
// which is neither endpoint, so every waiter registered on it costs a message
// to that home and every delivery costs a message back.  One task per
// rendezvous, created on the consuming rank, is therefore the only waiter the
// program ever puts on one: it republishes the arrival on an event created
// where its readers run, so the readers' registrations and deliveries are all
// rank-local and the traffic through the creating rank stays one message per
// transfer rather than one per reader.  It carries no data of its own -- the
// block it hands on is the one the producer made.
ocrGuid_t forwardTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrGuid_t evt = (ocrGuid_t){.guid = paramv[0]};
    //the readers acquire it in turn; do not hold an acquire across the handoff
    ocrDbRelease(depv[0].guid);
    ocrEventSatisfy(evt, depv[0].guid);
    return NULL_GUID;
}

// paramv: {j, m, t, places, g, evt, nwave}
// depv[wave*places + source place]: this place's arrival from that place (RO)
// Row stage for one tile: its rows arrive as one contiguous run per (source
// place, wave, column tile), so assembling a row is a walk rather than a
// gather.  The arrivals are shared with this place's other row tiles, so this
// task reads and releases them but does not reclaim them.
ocrGuid_t rowTileTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 tj = paramv[0];
    u64 m = paramv[1];
    u64 t = paramv[2];
    u64 places = paramv[3];
    u64 g = paramv[4];
    ocrGuid_t evt = (ocrGuid_t){.guid = paramv[5]};
    u64 nwave = paramv[6];
    u64 N = (u64)1 << m;
    u64 N1 = (u64)1 << ((m + 1) / 2);
    u64 N2 = N / N1;
    u64 lod, hid, r0, r1, nrowsd, k1lo, k1hi, nrows;
    u64 a, gi, ci, rr, cc, k2;

    spanOf(g, t, places, &lod, &hid);
    r0 = (lod * N1) / t;
    r1 = (hid * N1) / t;
    nrowsd = r1 - r0;
    spanOf(tj, N1, t, &k1lo, &k1hi);
    nrows = k1hi - k1lo;

    double *out = (double*)malloc(N2 * 2 * sizeof(double));
    double *rowRe = (double*)malloc(N2 * sizeof(double));
    double *rowIm = (double*)malloc(N2 * sizeof(double));

    double checksum = 0.0, maxerr = 0.0;
    for(rr = 0; rr < nrows; rr++) {
        u64 k1 = k1lo + rr;
        for(gi = 0; gi < nwave; gi++)
            for(a = 0; a < places; a++) {
                const double *in = (const double*)depv[gi * places + a].ptr;
                u64 first, ntile = waveTiles(a, t, places, gi, nwave, &first);
                u64 cbase = (first * N2) / t;
                for(ci = 0; ci < ntile; ci++) {
                    u64 c0, c1;
                    spanOf(first + ci, N2, t, &c0, &c1);
                    const double *s = in + (nrowsd * (c0 - cbase)
                                            + (k1 - r0) * (c1 - c0)) * 2;
                    for(cc = 0; cc < c1 - c0; cc++) {
                        rowRe[c0 + cc] = s[cc * 2];
                        rowIm[c0 + cc] = s[cc * 2 + 1];
                    }
                }
            }
        double *Xr = out, *Xi = out + N2;
        ditfft2c(Xr, Xi, rowRe, rowIm, (int)N2, 1);
        for(k2 = 0; k2 < N2; k2++) {
            //four-step output index map
            u64 k = k2 * N1 + k1;
            double expRe =
                (k == TONE_BIN || k == N - TONE_BIN) ? (double)N / 2.0 : 0.0;
            double err = fabs(Xr[k2] - expRe) + fabs(Xi[k2]);
            if(err > maxerr) maxerr = err;
            checksum += fabs(Xr[k2]) + fabs(Xi[k2]);
        }
    }
    free(rowRe);
    free(rowIm);
    free(out);

    ocrGuid_t partDb;
    double *part;
    ocrDbCreate(&partDb, (void**)&part, 2 * sizeof(double), 0, NULL_HINT, NO_ALLOC);
    part[0] = checksum;
    part[1] = maxerr;
    ocrDbRelease(partDb);
    //release the shared arrivals before reporting, so the reclaiming join's
    //turn cannot overlap this read
    for(a = 0; a < depc; a++) ocrDbRelease(depv[a].guid);
    ocrEventSatisfy(evt, partDb);
    return NULL_GUID;
}

// paramv: {g, places, doneEvt, nwave, tiles of g}
// depv 0: the transpose event grid (RO)
// depv 1..tpg: this place's row-tile partials
// depv 1+tpg..: this place's arrivals, [wave][source place] (RO)
// One per place: combine its partials into one, so the finisher sees P reports
// rather than one per tile.  Every row tile of the place has run by then, so
// this is also the one task that can know the arrivals are finished with.
ocrGuid_t rankJoinTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 g = paramv[0];
    u64 places = paramv[1];
    ocrGuid_t doneEvt = (ocrGuid_t){.guid = paramv[2]};
    u64 nwave = paramv[3];
    u64 tpg = paramv[4];
    const u64 *xevt = (const u64*)depv[0].ptr;
    u64 i, a, gi;

    double checksum = 0.0, maxerr = 0.0;
    for(i = 0; i < tpg; i++) {
        const double *p = (const double*)depv[1 + i].ptr;
        checksum += p[0];
        if(p[1] > maxerr) maxerr = p[1];
        ocrDbDestroy(depv[1 + i].guid);
    }
    /* A rendezvous event is sticky, so it does not reclaim itself when it
     * fires.  This task is ordered after every reader of what the exchange
     * delivered, so it is the one place that can know both the block and the
     * event it arrived on are finished with. */
    for(gi = 0; gi < nwave; gi++)
        for(a = 0; a < places; a++) {
            ocrDbDestroy(depv[1 + tpg + gi * places + a].guid);
            ocrEventDestroy((ocrGuid_t){
                .guid = xevt[(a * nwave + gi) * places + g]});
        }

    ocrGuid_t db;
    double *out;
    ocrDbCreate(&db, (void**)&out, 2 * sizeof(double), 0, NULL_HINT, NO_ALLOC);
    out[0] = checksum;
    out[1] = maxerr;
    ocrDbRelease(db);
    //the grid is shared with every place: release it before reporting, so the
    //finisher's reclaim cannot overlap this read
    ocrDbRelease(depv[0].guid);
    ocrEventSatisfy(doneEvt, db);
    return NULL_GUID;
}

// paramv: {g, m, t, places, doneEvt, nwave}
// depv 0: the transpose event grid, [source place][wave][destination place] (RO)
// A place's whole program, built where it runs.  The builder never reaches
// across the machine, so what it costs does not depend on how many ranks there
// are.  The event grid arrives as a datablock rather than as parameters: it has
// one entry per transfer, which is far past what a template's parameter count
// can encode, and every place reads all of it anyway.
ocrGuid_t rankInitTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 g = paramv[0];
    u64 m = paramv[1];
    u64 t = paramv[2];
    u64 places = paramv[3];
    u64 doneEvt = paramv[4];
    u64 ngrp = paramv[5];
    const u64 *xevt = (const u64*)depv[0].ptr;   //[place][wave][place]
    u64 lo, hi, k, a, d, gi, ci;
    spanOf(g, t, places, &lo, &hi);
    u64 tpg = hi - lo;
    ocrHint_t h = edtHintAt(placeRank(g, places));

    //One local event per arrival.  Every reader of an arrival binds to this
    //event rather than to the rendezvous event the producer satisfies: a
    //rendezvous event is homed where it was created, so a registration on it
    //and each of its deliveries is a message, and the number of readers is the
    //tile count.  These are created here, so their home is the rank the readers
    //run on and their whole fan-out is rank-local.
    ocrGuid_t *arrEvt = (ocrGuid_t*)malloc(ngrp * places * sizeof(ocrGuid_t));
    for(gi = 0; gi < ngrp; gi++)
        for(a = 0; a < places; a++)
            ocrEventCreate(&arrEvt[gi * places + a], OCR_EVENT_ONCE_T,
                           EVT_PROP_TAKES_ARG);

    //Per wave: the column tiles, one packing task per destination, and the
    //reaper that frees the wave's tiles once every destination has taken its
    //share.  Every consumer of a tile registers before the tile's producer
    //exists, so a single-fire event never has to serve a late binder.
    //
    //This stage is built first because it is the one that can start: it waits
    //on nothing, while the row side below waits on the exchange either way.
    //Wiring the row side first would leave the place idle for the length of a
    //build whose dependence count is the tile count times the arrival count.
    ocrGuid_t colTml;
    ocrEdtTemplateCreate(&colTml, colTileTask, 4, 0);
    ocrGuid_t *colEvt = (ocrGuid_t*)malloc(tpg * sizeof(ocrGuid_t));
    ocrGuid_t *packDone = (ocrGuid_t*)malloc(places * sizeof(ocrGuid_t));
    u64 packPrm[9];
    for(gi = 0; gi < ngrp; gi++) {
        u64 glo, ntile = waveTiles(g, t, places, gi, ngrp, &glo);
        ocrGuid_t packTml, reapTml, reapEdt;
        u64 reapPrm[1];

        for(ci = 0; ci < ntile; ci++)
            ocrEventCreate(&colEvt[ci], OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);
        for(d = 0; d < places; d++)
            ocrEventCreate(&packDone[d], OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);

        reapPrm[0] = ntile;
        ocrEdtTemplateCreate(&reapTml, reapTask, 1, (u32)(ntile + places));
        ocrEdtCreate(&reapEdt, reapTml, EDT_PARAM_DEF, reapPrm, EDT_PARAM_DEF,
                     NULL, EDT_PROP_NONE, &h, NULL);
        ocrEdtTemplateDestroy(reapTml);
        for(ci = 0; ci < ntile; ci++)
            ocrAddDependence(colEvt[ci], reapEdt, (u32)ci, DB_MODE_RO);
        for(d = 0; d < places; d++)
            ocrAddDependence(packDone[d], reapEdt, (u32)(ntile + d), DB_MODE_RO);

        ocrEdtTemplateCreate(&packTml, packTask, 9, (u32)ntile);
        for(d = 0; d < places; d++) {
            ocrGuid_t packEdt;
            packPrm[0] = g; packPrm[1] = m; packPrm[2] = t; packPrm[3] = places;
            packPrm[4] = gi; packPrm[5] = ngrp; packPrm[6] = d;
            packPrm[7] = xevt[(g * ngrp + gi) * places + d];
            packPrm[8] = (u64)packDone[d].guid;
            ocrEdtCreate(&packEdt, packTml, EDT_PARAM_DEF, packPrm, EDT_PARAM_DEF,
                         NULL, EDT_PROP_NONE, &h, NULL);
            for(ci = 0; ci < ntile; ci++)
                ocrAddDependence(colEvt[ci], packEdt, (u32)ci, DB_MODE_RO);
        }
        ocrEdtTemplateDestroy(packTml);

        for(ci = 0; ci < ntile; ci++) {
            ocrGuid_t colEdt;
            u64 colPrm[4] = {glo + ci, m, t, (u64)colEvt[ci].guid};
            ocrEdtCreate(&colEdt, colTml, EDT_PARAM_DEF, colPrm, EDT_PARAM_DEF,
                         NULL, EDT_PROP_NONE, &h, NULL);
        }
    }
    free(colEvt);
    free(packDone);
    ocrEdtTemplateDestroy(colTml);

    //the join, so the row tiles below have somewhere to report, and so the
    //arrivals they share have a reaper
    ocrGuid_t joinTml, joinEdt;
    u64 joinPrm[5] = {g, places, doneEvt, ngrp, tpg};
    ocrEdtTemplateCreate(&joinTml, rankJoinTask, 5,
                         (u32)(1 + tpg + ngrp * places));
    ocrEdtCreate(&joinEdt, joinTml, EDT_PARAM_DEF, joinPrm, EDT_PARAM_DEF, NULL,
                 EDT_PROP_NONE, &h, NULL);
    ocrEdtTemplateDestroy(joinTml);
    ocrAddDependence(depv[0].guid, joinEdt, 0, DB_MODE_RO);
    for(gi = 0; gi < ngrp; gi++)
        for(a = 0; a < places; a++)
            ocrAddDependence(arrEvt[gi * places + a], joinEdt,
                             (u32)(1 + tpg + gi * places + a), DB_MODE_RO);

    //row tiles: each reads this place's arrivals and reports one partial
    ocrGuid_t rowTml;
    ocrEdtTemplateCreate(&rowTml, rowTileTask, 7, (u32)(ngrp * places));
    for(k = 0; k < tpg; k++) {
        ocrGuid_t partEvt, rowEdt;
        ocrEventCreate(&partEvt, OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);
        ocrAddDependence(partEvt, joinEdt, (u32)(1 + k), DB_MODE_RO);

        u64 rowPrm[7] = {lo + k, m, t, places, g, (u64)partEvt.guid, ngrp};
        ocrEdtCreate(&rowEdt, rowTml, EDT_PARAM_DEF, rowPrm, EDT_PARAM_DEF, NULL,
                     EDT_PROP_NONE, &h, NULL);
        for(gi = 0; gi < ngrp; gi++)
            for(a = 0; a < places; a++)
                ocrAddDependence(arrEvt[gi * places + a], rowEdt,
                                 (u32)(gi * places + a), DB_MODE_RO);
    }
    ocrEdtTemplateDestroy(rowTml);

    //The forwarders, created only once every local reader has registered, so a
    //single-fire local event never has to serve a late binder.  One per
    //rendezvous, and the only waiter on it.
    ocrGuid_t fwdTml;
    ocrEdtTemplateCreate(&fwdTml, forwardTask, 1, 1);
    for(gi = 0; gi < ngrp; gi++)
        for(a = 0; a < places; a++) {
            ocrGuid_t fwdEdt;
            u64 fwdPrm[1] = {(u64)arrEvt[gi * places + a].guid};
            ocrEdtCreate(&fwdEdt, fwdTml, EDT_PARAM_DEF, fwdPrm, EDT_PARAM_DEF,
                         NULL, EDT_PROP_NONE, &h, NULL);
            ocrAddDependence(
                (ocrGuid_t){.guid = xevt[(a * ngrp + gi) * places + g]}, fwdEdt,
                0, DB_MODE_RO);
        }
    ocrEdtTemplateDestroy(fwdTml);
    free(arrEvt);
    return NULL_GUID;
}

// paramv: {places, m}
// depv 0..places-1: one {partial checksum, max err} per place
// depv places: the transpose event grid
ocrGuid_t finishTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 places = paramv[0];
    u64 m = paramv[1];
    u64 N = (u64)1 << m;
    u64 j;
    double checksum = 0.0, maxerr = 0.0;
    for(j = 0; j < places; j++) {
        const double *part = (const double*)depv[j].ptr;
        checksum += part[0];
        if(part[1] > maxerr) maxerr = part[1];
        ocrDbDestroy(depv[j].guid);
    }
    ocrDbDestroy(depv[places].guid);
    double relerr = maxerr / ((double)N / 2.0);
    if(relerr <= FFT_TOL) {
        ocrPrintf("FFT_DIST checksum = %.6f peak_bins = %lu,%lu max_err = %.3e\n",
                  checksum, (u64)TONE_BIN, N - TONE_BIN, relerr);
    } else {
        ocrPrintf("FFT_DIST INVALID max_err = %.3e\n", relerr);
    }
    ocrShutdown();
    return NULL_GUID;
}

// The whole of the central work: the events the places hand data through, and
// one task per place.  P*waves*P + P events and P tasks -- nothing here grows
// with the tile count, which is what lets the tile count be chosen for
// parallelism.
ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 m = 10;
    u64 tReq = FFT_DEFAULT_TILES;
    u64 places = FFT_DEFAULT_PLACES;
    u64 wReq = 0;
    if(depc >= 1 && depv[0].ptr) {
        u64 argc = ocrGetArgc(depv[0].ptr);
        if(argc > 1) m = strtoull(ocrGetArgv(depv[0].ptr, 1), NULL, 10);
        if(argc > 2) tReq = strtoull(ocrGetArgv(depv[0].ptr, 2), NULL, 10);
        if(argc > 3) places = strtoull(ocrGetArgv(depv[0].ptr, 3), NULL, 10);
        if(argc > 4) wReq = strtoull(ocrGetArgv(depv[0].ptr, 4), NULL, 10);
    }
    if(places < 1) places = 1;
    if(m < 4) m = 4;
    //Two forms of the matrix are live at once, so the size ceiling is memory:
    //a transform of length N holds 32*N bytes.
    if(m > 34) m = 34;
    if(tReq < 1) tReq = 1;

    u64 N1 = (u64)1 << ((m + 1) / 2);
    u64 N2 = ((u64)1 << m) / N1;

    //A tile is a range of indices in each dimension, so the only bound on the
    //tile count is that every tile hold a row and a column.  t is the app's
    //whole concurrency expression -- the PD count never enters it (tune t per
    //machine, not per rank).
    u64 t = tReq;
    if(t > N1) t = N1;
    if(t > N2) t = N2;

    //A place with no tile would own nothing.  Note what is NOT here: the
    //machine.  The decomposition is fixed by argument, so the task and
    //datablock counts -- and the order the partial checksums combine in -- are
    //the same in every geometry.
    if(places > t) places = t;

    u64 ngrp = waveCount(t, places);
    if(wReq >= 1) {
        ngrp = wReq;
        if(ngrp > t / places) ngrp = t / places;
    }
    u64 r, s, nx = places * ngrp * places;

    ocrGuid_t finishTemplate, finishEdt;
    u64 finishPrm[2] = {places, m};
    ocrEdtTemplateCreate(&finishTemplate, finishTask, 2, (u32)(places + 1));
    ocrEdtCreate(&finishEdt, finishTemplate, EDT_PARAM_DEF, finishPrm,
                 EDT_PARAM_DEF, NULL, EDT_PROP_NONE, NULL_HINT, NULL);
    ocrEdtTemplateDestroy(finishTemplate);

    //One transpose event per (source place, pack wave, destination place): the
    //exchange is pair-wise, which is what keeps a block big enough to amortize
    //a message while one task per block keeps the stage as wide as the tile
    //stages.  The grid is [place][pack wave][place]; the wave count is chosen
    //once here and passed to every place, so the builder and the places index
    //the grid identically.
    //
    //These are STICKY, and that is a correctness requirement rather than a
    //preference.  A ONCE event delivers to the waiters registered when it is
    //satisfied and is gone afterwards, but the waiter here registers inside
    //the DESTINATION place's own init task while the producer satisfies from
    //the SOURCE place's pack -- tasks on two ranks whose only common ancestor
    //is this one, so nothing orders the registration before the satisfy.  A
    //sticky event delivers to a late registrant; the destination's join
    //destroys it, since a sticky event does not reclaim itself.
    //
    //They are homed on this rank, so each one costs one registration message
    //and one delivery message to it.  Exactly one waiter per event -- the
    //destination's forwarder -- keeps that at places*waves*places messages
    //whatever the tile count.
    ocrGuid_t *xevt = (ocrGuid_t*)malloc(nx * sizeof(ocrGuid_t));
    ocrGuid_t *devt = (ocrGuid_t*)malloc(places * sizeof(ocrGuid_t));
    for(r = 0; r < places; r++) {
        for(s = 0; s < ngrp * places; s++)
            ocrEventCreate(&xevt[r * ngrp * places + s], OCR_EVENT_STICKY_T,
                           EVT_PROP_TAKES_ARG);
        ocrEventCreate(&devt[r], OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);
        ocrAddDependence(devt[r], finishEdt, (u32)r, DB_MODE_RO);
    }

    //the grid travels as a block: one entry per transfer is past what a
    //template's parameter count can hold, and every place reads all of it
    ocrGuid_t wireDb;
    u64 *wire;
    ocrDbCreate(&wireDb, (void**)&wire, nx * sizeof(u64), 0, NULL_HINT, NO_ALLOC);
    for(s = 0; s < nx; s++) wire[s] = (u64)xevt[s].guid;
    ocrDbRelease(wireDb);
    ocrAddDependence(wireDb, finishEdt, (u32)places, DB_MODE_RO);

    ocrGuid_t rankTml;
    u64 prm[6];
    ocrEdtTemplateCreate(&rankTml, rankInitTask, 6, 1);
    for(r = 0; r < places; r++) {
        ocrHint_t h = edtHintAt(placeRank(r, places));
        prm[0] = r; prm[1] = m; prm[2] = t; prm[3] = places;
        prm[4] = (u64)devt[r].guid; prm[5] = ngrp;
        ocrGuid_t rankEdt;
        ocrEdtCreate(&rankEdt, rankTml, EDT_PARAM_DEF, prm, EDT_PARAM_DEF, NULL,
                     EDT_PROP_NONE, &h, NULL);
        ocrAddDependence(wireDb, rankEdt, 0, DB_MODE_RO);
    }
    ocrEdtTemplateDestroy(rankTml);

    free(xevt);
    free(devt);
    return NULL_GUID;
}
