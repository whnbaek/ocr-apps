/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */
/* Based on fft.c - distributed variant: Bailey four-step transpose FFT on a
 * rank-persistent decomposition.  The length-N transform is viewed as an
 * N1 x N2 matrix and computed in four steps -- an FFT down every column, a
 * twiddle, a transpose, an FFT along every row -- over a grid of t tiles, so
 * no two tasks write the same object.  A single-tone input has a closed-form
 * spectrum, so each row tile verifies its own output block analytically and
 * the partial checksums reduce to exactly N.
 *
 * The program is SPMD, not a centrally built task graph.  mainEdt creates one
 * task per rank and the events the ranks hand data through, and nothing else;
 * each rank then builds its own tiles, its own pack and its own row work on
 * its own rank.  A graph built in one place cannot scale: every creation
 * carrying a remote affinity is a message, so the builder's cost grows with
 * the rank count instead of shrinking with it.
 *
 * The transpose is aggregated per rank, which is what the distributed FFT
 * libraries do: a rank packs everything it owes a peer into one buffer and
 * sends it once, so the exchange is P(P-1) messages whatever the tile count.
 * Expressed per tile it would be one message per tile pair -- quadratic in a
 * quantity chosen for compute parallelism, and three orders of magnitude more
 * messages than the data needs.
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
//Default tile count when argv gives none (clamped to divide N1 and N2).
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

//A place's share of the tiles, as a half-open range.  Ownership is contiguous
//so that a place's tiles are its own on both sides of the transpose.
static void tileRange(u64 place, u64 t, u64 places, u64 *lo, u64 *hi) {
    *lo = (place * t) / places;
    *hi = ((place + 1) * t) / places;
}

//The wave count, derived from the decomposition rather than exchanged, so every
//place and the builder index the transpose grid identically.
static u64 waveCount(u64 t, u64 places) {
    u64 lo, hi;
    tileRange(0, t, places, &lo, &hi);
    return (hi - lo) >= 8 ? 8 : 1;
}

//How many of a place's tiles fall in one wave, and where that run starts.
static u64 waveTiles(u64 place, u64 t, u64 places, u64 wave, u64 nwave,
                     u64 *first) {
    u64 lo, hi;
    tileRange(place, t, places, &lo, &hi);
    u64 n = hi - lo;
    u64 a = lo + (wave * n) / nwave, b = lo + ((wave + 1) * n) / nwave;
    if(first) *first = a;
    return b - a;
}

// paramv: {i, m, t, evt}
// Column stage for one tile: generate its N2/t columns of the tone, run a
// length-N1 FFT down each, apply the four-step twiddle, and publish the result
// as a rectangle of N1t x N2t point blocks, one per row block.
//
// Row-block major is what lets the pack below take a peer's share as a
// contiguous run rather than a strided gather.
ocrGuid_t colTileTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 ti = paramv[0];
    u64 m = paramv[1];
    u64 t = paramv[2];
    ocrGuid_t evt = (ocrGuid_t){.guid = paramv[3]};
    u64 N = (u64)1 << m;
    u64 N1 = (u64)1 << ((m + 1) / 2);
    u64 N2 = N / N1;
    u64 N1t = N1 / t, N2t = N2 / t;
    u64 cc, rr, k1, rb;

    double *inRe = (double*)malloc(N1 * sizeof(double));
    double *inIm = (double*)malloc(N1 * sizeof(double));
    double *colRe = (double*)malloc(N1 * sizeof(double));
    double *colIm = (double*)malloc(N1 * sizeof(double));

    ocrGuid_t db;
    double *rect;
    //the block is created by the task that fills it, on its own rank
    ocrDbCreate(&db, (void**)&rect, N1 * N2t * 2 * sizeof(double), 0, NULL_HINT,
                NO_ALLOC);

    for(cc = 0; cc < N2t; cc++) {
        u64 c = ti * N2t + cc;
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
            colRe[k1] = wr * xr - wi * xi;
            colIm[k1] = wi * xr + wr * xi;
        }
        //scatter the finished column into row-block major order
        for(rb = 0; rb < t; rb++)
            for(rr = 0; rr < N1t; rr++) {
                u64 src = rb * N1t + rr;
                rect[((rb * N1t + rr) * N2t + cc) * 2]     = colRe[src];
                rect[((rb * N1t + rr) * N2t + cc) * 2 + 1] = colIm[src];
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

// paramv: {g, m, t, places, wave, nwave, destination event[places]}
// depv: this wave's column tiles (RO)
// One block per destination PLACE.  Per destination ROW BLOCK every arrival
// would have exactly one reader and could die with it, but a row tile cannot
// run until every place has packed every wave, so nothing is actually freed
// earlier -- while the transfer count becomes places*waves*t, which at any real
// tile count fills the wire with blocks too small to amortize a message.  Per
// place pair the count is places*waves*places, and the receiver's unpack is
// what cuts an arrival into per-reader blocks and drops it.
ocrGuid_t packTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 m = paramv[1];
    u64 t = paramv[2];
    u64 places = paramv[3];
    const u64 *evt = paramv + 6;
    u64 N = (u64)1 << m;
    u64 N1 = (u64)1 << ((m + 1) / 2);
    u64 N2 = N / N1;
    u64 blk = (N1 / t) * (N2 / t);       //points in one (row block, column block)
    u64 d, ci;
    u64 ntile = depc;

    for(d = 0; d < places; d++) {
        u64 lod, hid, tpd;
        ocrGuid_t db;
        double *out;
        ocrHint_t h;
        tileRange(d, t, places, &lod, &hid);
        tpd = hid - lod;
        //created on the rank that unpacks it
        h = dbHintAt(placeRank(d, places));
        ocrDbCreate(&db, (void**)&out, ntile * tpd * blk * 2 * sizeof(double), 0,
                    &h, NO_ALLOC);
        //layout: [column tile of this wave][row block of d][point].  The source
        //is row-block major, so one destination's rows are a contiguous run.
        for(ci = 0; ci < ntile; ci++)
            memcpy(out + (ci * tpd * blk) * 2,
                   (const double*)depv[ci].ptr + (lod * blk) * 2,
                   tpd * blk * 2 * sizeof(double));
        ocrDbRelease(db);
        ocrEventSatisfy((ocrGuid_t){.guid = evt[d]}, db);
    }
    //a wave's tiles die with the wave, so the column form is never all
    //resident: it peaks at one wave per place rather than the whole place
    for(ci = 0; ci < ntile; ci++) ocrDbDestroy(depv[ci].guid);
    return NULL_GUID;
}

// paramv: {d, m, t, places, wave, nwave, row-tile event[tiles of d]}
// depv: this wave's arrival from every place (RO)
// The receiving half of the pair-wise exchange: it cuts one wave's arrivals
// into one block per row tile, so a row tile takes one dependence per wave
// rather than one per (place, wave), and the arrivals die here instead of
// living until the last row tile of the place has run.
ocrGuid_t unpackTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 d = paramv[0];
    u64 m = paramv[1];
    u64 t = paramv[2];
    u64 places = paramv[3];
    u64 wave = paramv[4];
    u64 nwave = paramv[5];
    const u64 *evt = paramv + 6;
    u64 N = (u64)1 << m;
    u64 N1 = (u64)1 << ((m + 1) / 2);
    u64 N2 = N / N1;
    u64 blk = (N1 / t) * (N2 / t);
    u64 lod, hid, tpd, a, ci, rl, j, ntw = 0;
    tileRange(d, t, places, &lod, &hid);
    tpd = hid - lod;
    for(a = 0; a < places; a++) ntw += waveTiles(a, t, places, wave, nwave, NULL);

    for(rl = 0; rl < tpd; rl++) {
        ocrGuid_t db;
        double *out;
        ocrHint_t h = dbHintAt(placeRank(d, places));
        ocrDbCreate(&db, (void**)&out, ntw * blk * 2 * sizeof(double), 0, &h,
                    NO_ALLOC);
        //layout: [column tile of this wave, places in order][point] -- which is
        //the order the row tile walks, so it gathers rather than searches
        j = 0;
        for(a = 0; a < places; a++) {
            u64 nta = waveTiles(a, t, places, wave, nwave, NULL);
            const double *in = (const double*)depv[a].ptr;
            for(ci = 0; ci < nta; ci++, j++)
                memcpy(out + (j * blk) * 2,
                       in + ((ci * tpd + rl) * blk) * 2,
                       blk * 2 * sizeof(double));
        }
        ocrDbRelease(db);
        ocrEventSatisfy((ocrGuid_t){.guid = evt[rl]}, db);
    }
    for(a = 0; a < places; a++) ocrDbDestroy(depv[a].guid);
    return NULL_GUID;
}

// paramv: {j, m, t, places, g, evt}
// depv 0..places-1: this row block's share from each place
// Row stage for one tile: its rows arrive as one contiguous run per source
// place, so assembling a row is a walk rather than a gather.  The arrivals are
// destroyed here -- each belongs to this row tile alone, which is what keeps
// the transpose's destination form from having to be resident all at once.
ocrGuid_t rowTileTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 tj = paramv[0];
    u64 m = paramv[1];
    u64 t = paramv[2];
    u64 places = paramv[3];
    ocrGuid_t evt = (ocrGuid_t){.guid = paramv[5]};
    u64 N = (u64)1 << m;
    u64 N1 = (u64)1 << ((m + 1) / 2);
    u64 N2 = N / N1;
    u64 N1t = N1 / t, N2t = N2 / t;
    u64 a, ci, rr, cc, k2, gi;
    //one arrival per wave, each already cut to this row tile by the unpack
    u64 nwave = depc;

    double *out = (double*)malloc(N2 * 2 * sizeof(double));
    double *rowRe = (double*)malloc(N2 * sizeof(double));
    double *rowIm = (double*)malloc(N2 * sizeof(double));

    double checksum = 0.0, maxerr = 0.0;
    for(rr = 0; rr < N1t; rr++) {
        u64 k1 = tj * N1t + rr;
        for(gi = 0; gi < nwave; gi++) {
            const double *in = (const double*)depv[gi].ptr;
            u64 j = 0;
            for(a = 0; a < places; a++) {
                u64 glo, nta = waveTiles(a, t, places, gi, nwave, &glo);
                for(ci = 0; ci < nta; ci++, j++)
                    for(cc = 0; cc < N2t; cc++) {
                        u64 o = ((j * N1t + rr) * N2t + cc) * 2;
                        rowRe[(glo + ci) * N2t + cc] = in[o];
                        rowIm[(glo + ci) * N2t + cc] = in[o + 1];
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
    for(a = 0; a < depc; a++) ocrDbDestroy(depv[a].guid);
    free(rowRe);
    free(rowIm);
    free(out);

    ocrGuid_t partDb;
    double *part;
    ocrDbCreate(&partDb, (void**)&part, 2 * sizeof(double), 0, NULL_HINT, NO_ALLOC);
    part[0] = checksum;
    part[1] = maxerr;
    ocrDbRelease(partDb);
    ocrEventSatisfy(evt, partDb);
    return NULL_GUID;
}

// paramv: {g, places, doneEvt}
// depv 0..tpg-1: this place's row-tile partials
// One per place: combine its partials into one, so the finisher sees P reports
// rather than one per tile.
ocrGuid_t rankJoinTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrGuid_t doneEvt = (ocrGuid_t){.guid = paramv[2]};
    u64 i;

    double checksum = 0.0, maxerr = 0.0;
    for(i = 0; i < depc; i++) {
        const double *p = (const double*)depv[i].ptr;
        checksum += p[0];
        if(p[1] > maxerr) maxerr = p[1];
        ocrDbDestroy(depv[i].guid);
    }

    ocrGuid_t db;
    double *out;
    ocrDbCreate(&db, (void**)&out, 2 * sizeof(double), 0, NULL_HINT, NO_ALLOC);
    out[0] = checksum;
    out[1] = maxerr;
    ocrDbRelease(db);
    ocrEventSatisfy(doneEvt, db);
    return NULL_GUID;
}

// paramv: {g, m, t, places, doneEvt}
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
    const u64 *xevt = (const u64*)depv[0].ptr;   //[place][wave][place]
    u64 lo, hi, k, a, gi;
    tileRange(g, t, places, &lo, &hi);
    u64 tpg = hi - lo;
    //how many waves a place packs in: enough that the column form peaks at a
    //fraction of itself, bounded so a wave still holds several tiles.  Derived
    //from the decomposition, never from this place's own share, so that every
    //place and the builder index the transpose grid the same way.
    u64 ngrp = waveCount(t, places);
    ocrHint_t h = edtHintAt(placeRank(g, places));

    //the join, so the row tiles below have somewhere to report
    ocrGuid_t joinTml, joinEdt;
    u64 joinPrm[3] = {g, places, doneEvt};
    ocrEdtTemplateCreate(&joinTml, rankJoinTask, 3, (u32)tpg);
    ocrEdtCreate(&joinEdt, joinTml, EDT_PARAM_DEF, joinPrm, EDT_PARAM_DEF, NULL,
                 EDT_PROP_NONE, &h, NULL);
    ocrEdtTemplateDestroy(joinTml);

    //Where the unpack hands a wave to a row tile.  These never leave the place,
    //so they are created here rather than in the grid the builder publishes.
    ocrGuid_t *rowEvt = (ocrGuid_t*)malloc(ngrp * tpg * sizeof(ocrGuid_t));
    for(gi = 0; gi < ngrp; gi++)
        for(k = 0; k < tpg; k++)
            ocrEventCreate(&rowEvt[gi * tpg + k], OCR_EVENT_ONCE_T,
                           EVT_PROP_TAKES_ARG);

    //row tiles: one arrival per wave, and each dies with its reader
    ocrGuid_t rowTml;
    ocrEdtTemplateCreate(&rowTml, rowTileTask, 6, (u32)ngrp);
    for(k = 0; k < tpg; k++) {
        ocrGuid_t partEvt, rowEdt;
        ocrEventCreate(&partEvt, OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);
        ocrAddDependence(partEvt, joinEdt, (u32)k, DB_MODE_RO);

        u64 rowPrm[6] = {lo + k, m, t, places, g, (u64)partEvt.guid};
        ocrEdtCreate(&rowEdt, rowTml, EDT_PARAM_DEF, rowPrm, EDT_PARAM_DEF, NULL,
                     EDT_PROP_NONE, &h, NULL);
        for(gi = 0; gi < ngrp; gi++)
            ocrAddDependence(rowEvt[gi * tpg + k], rowEdt, (u32)gi, DB_MODE_RO);
    }
    ocrEdtTemplateDestroy(rowTml);

    //one unpack per wave, taking this place's arrival from every place
    {
        u64 *unpPrm = (u64*)malloc((6 + tpg) * sizeof(u64));
        for(gi = 0; gi < ngrp; gi++) {
            ocrGuid_t unpTml, unpEdt;
            unpPrm[0] = g; unpPrm[1] = m; unpPrm[2] = t; unpPrm[3] = places;
            unpPrm[4] = gi; unpPrm[5] = ngrp;
            for(k = 0; k < tpg; k++) unpPrm[6 + k] = (u64)rowEvt[gi * tpg + k].guid;
            ocrEdtTemplateCreate(&unpTml, unpackTask, (u32)(6 + tpg), (u32)places);
            ocrEdtCreate(&unpEdt, unpTml, EDT_PARAM_DEF, unpPrm, EDT_PARAM_DEF,
                         NULL, EDT_PROP_NONE, &h, NULL);
            ocrEdtTemplateDestroy(unpTml);
            for(a = 0; a < places; a++)
                ocrAddDependence(
                    (ocrGuid_t){.guid = xevt[(a * ngrp + gi) * places + g]},
                    unpEdt, (u32)a, DB_MODE_RO);
        }
        free(unpPrm);
    }
    free(rowEvt);

    //The pack runs in groups so a group's column tiles are released as soon as
    //that group has packed: with one pack per place, every tile of the place
    //has to be resident before any of them can be freed, which is the whole
    //column form of the transform alive at once.
    ocrGuid_t colTml;
    ocrEdtTemplateCreate(&colTml, colTileTask, 4, 0);
    u64 *packPrm = (u64*)malloc((6 + places) * sizeof(u64));
    for(gi = 0; gi < ngrp; gi++) {
        u64 glo = lo + (gi * tpg) / ngrp, ghi = lo + ((gi + 1) * tpg) / ngrp;
        ocrGuid_t packTml, packEdt;
        packPrm[0] = g; packPrm[1] = m; packPrm[2] = t; packPrm[3] = places;
        packPrm[4] = gi; packPrm[5] = ngrp;
        for(a = 0; a < places; a++)
            packPrm[6 + a] = xevt[(g * ngrp + gi) * places + a];
        ocrEdtTemplateCreate(&packTml, packTask, (u32)(6 + places),
                             (u32)(ghi - glo));
        ocrEdtCreate(&packEdt, packTml, EDT_PARAM_DEF, packPrm, EDT_PARAM_DEF,
                     NULL, EDT_PROP_NONE, &h, NULL);
        ocrEdtTemplateDestroy(packTml);

        for(k = glo; k < ghi; k++) {
            ocrGuid_t colEvt, colEdt;
            ocrEventCreate(&colEvt, OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);
            ocrAddDependence(colEvt, packEdt, (u32)(k - glo), DB_MODE_RO);

            u64 colPrm[4] = {k, m, t, (u64)colEvt.guid};
            ocrEdtCreate(&colEdt, colTml, EDT_PARAM_DEF, colPrm, EDT_PARAM_DEF,
                         NULL, EDT_PROP_NONE, &h, NULL);
        }
    }
    free(packPrm);
    ocrEdtTemplateDestroy(colTml);
    return NULL_GUID;
}

// paramv: {places, m}
// depv 0..places-1: one {partial checksum, max err} per rank
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

// The whole of the central work: the events the ranks hand data through, and
// one task per rank.  P*P + P events and P tasks -- nothing here grows with the
// tile count, which is what lets the tile count be chosen for parallelism.
ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 m = 10;
    u64 tReq = FFT_DEFAULT_TILES;
    u64 places = FFT_DEFAULT_PLACES;
    if(depc >= 1 && depv[0].ptr) {
        u64 argc = ocrGetArgc(depv[0].ptr);
        if(argc > 1) m = strtoull(ocrGetArgv(depv[0].ptr, 1), NULL, 10);
        if(argc > 2) tReq = strtoull(ocrGetArgv(depv[0].ptr, 2), NULL, 10);
        if(argc > 3) places = strtoull(ocrGetArgv(depv[0].ptr, 3), NULL, 10);
    }
    if(places < 1) places = 1;
    if(m < 4) m = 4;
    //This program's own ceiling, not the one the base carries: it holds N
    //complex points once per stage rather than three float arrays at once, and
    //it is calibrated to its own window, so the sizes it is asked for are
    //larger.
    if(m > 34) m = 34;
    if(tReq < 1) tReq = 1;

    u64 N1 = (u64)1 << ((m + 1) / 2);
    u64 N2 = ((u64)1 << m) / N1;

    //t must divide both matrix dimensions: largest power of two <= the
    //requested tile count.  t is the app's whole concurrency expression --
    //the PD count never enters it (tune t per machine, not per rank).
    u64 t = 1;
    while(t * 2 <= tReq && (N1 % (t * 2)) == 0 && (N2 % (t * 2)) == 0)
        t *= 2;

    //A place with no tile would own nothing.  Note what is NOT here: the
    //machine.  The decomposition is fixed by argument, so the task and
    //datablock counts -- and the order the partial checksums combine in -- are
    //the same in every geometry.
    if(places > t) places = t;
    u64 r, s;

    ocrGuid_t finishTemplate, finishEdt;
    u64 finishPrm[2] = {places, m};
    ocrEdtTemplateCreate(&finishTemplate, finishTask, 2, (u32)places);
    ocrEdtCreate(&finishEdt, finishTemplate, EDT_PARAM_DEF, finishPrm,
                 EDT_PARAM_DEF, NULL, EDT_PROP_NONE, NULL_HINT, NULL);
    ocrEdtTemplateDestroy(finishTemplate);

    //One transpose event per (source place, pack wave, destination place): the
    //exchange is pair-wise, which is what keeps a block big enough to amortize
    //a message.  The receiver's unpack is what turns an arrival into per-reader
    //blocks, so nothing is held longer for it than for a per-row-block split.
    //the grid is [place][pack wave][place]; the wave count is derived by the
    //same rule on both sides, so the two index it identically
    u64 ngrp = waveCount(t, places);
    ocrGuid_t *xevt =
        (ocrGuid_t*)malloc(places * ngrp * places * sizeof(ocrGuid_t));
    ocrGuid_t *devt = (ocrGuid_t*)malloc(places * sizeof(ocrGuid_t));
    for(r = 0; r < places; r++) {
        for(s = 0; s < ngrp * places; s++)
            ocrEventCreate(&xevt[r * ngrp * places + s], OCR_EVENT_ONCE_T,
                           EVT_PROP_TAKES_ARG);
        ocrEventCreate(&devt[r], OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);
        ocrAddDependence(devt[r], finishEdt, (u32)r, DB_MODE_RO);
    }

    //the grid travels as a block: one entry per transfer is past what a
    //template's parameter count can hold, and every place reads all of it
    ocrGuid_t wireDb;
    u64 *wire;
    ocrDbCreate(&wireDb, (void**)&wire, places * ngrp * places * sizeof(u64), 0,
                NULL_HINT, NO_ALLOC);
    for(s = 0; s < places * ngrp * places; s++) wire[s] = (u64)xevt[s].guid;
    ocrDbRelease(wireDb);

    ocrGuid_t rankTml;
    u64 prm[5];
    ocrEdtTemplateCreate(&rankTml, rankInitTask, 5, 1);
    for(r = 0; r < places; r++) {
        ocrHint_t h = edtHintAt(placeRank(r, places));
        prm[0] = r; prm[1] = m; prm[2] = t; prm[3] = places;
        prm[4] = (u64)devt[r].guid;
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
