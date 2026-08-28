/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

/* Tiled Cholesky whose task graph is built where it runs.
 *
 * A right-looking tiled factorization is a per-tile DAG: at step k the diagonal
 * tile is factored, the panel below it is solved against that factor, and every
 * tile of the trailing submatrix is updated from the two panel tiles in its row
 * and its column.  Expressing that the obvious way builds the entire graph in
 * the starting task -- on the order of t^3/6 creations for t tiles per
 * dimension -- and each creation whose task belongs elsewhere is a message.
 * The graph construction then serializes on one place however well the work
 * itself is placed.
 *
 * Here the tile partition is an argument.  The tiles are divided across
 * `places` by the two-dimensional block-cyclic map below, and a place creates
 * the tasks for the tiles it owns, on itself.  What crosses a place boundary at
 * startup is one grid of event names and the input path; everything else is
 * local creation, and a place reads its own tiles from the input rather than
 * receiving them.  The partition, the task count and the order the factor is
 * computed in do not depend on how many ranks the program runs on -- a place is
 * a unit of ownership, and the machine enters only through the hint that maps
 * a place onto a rank.
 */

#include "ocr.h"
#include "extensions/ocr-affinity.h"
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <math.h>
#include <cblas.h>
#include <lapacke.h>

#define FLAGS DB_PROP_NONE

/* ---------------------------------------------------------------- ownership */

/* The two-dimensional block-cyclic map ScaLAPACK uses: tile (i,j) belongs to place
 * (i % P) * Q + (j % Q) on a near-square P x Q grid.  Two independent axes are
 * what keep the active trailing submatrix spread; one linear index modulo the
 * place count folds a whole frontier onto one place. */
static void gridOf(u64 places, u64 *P, u64 *Q) {
    u64 p, best = 1;
    for(p = 1; p <= places; ++p)
        if(places % p == 0 && p * p >= places) { best = p; break; }
    *P = best; *Q = places / best;
}
static u64 ownerOf(u64 i, u64 j, u64 P, u64 Q) { return (i % P) * Q + (j % Q); }

/* Tile (i,j) exists in versions 0..j+1 and no further: it is updated once at
 * each step below its own column and finalised at step j.  Numbering every
 * tile up to the last step instead wastes two thirds of the grid on names
 * nothing ever satisfies or reads.  This is the offset of tile (i,j)'s first
 * version, over the tiles in (i, j<=i) order each contributing j+2 entries. */
static u64 evBase(u64 i, u64 j) {
    u64 rows = i ? (i * (i * i - 1)) / 6 + i * (i + 1) : 0;
    return rows + j * (j + 3) / 2;
}

static u64 placeRank(u64 place, u64 places) {
    u64 nranks = 1;
    ocrAffinityCount(AFFINITY_PD, &nranks);
    if(nranks <= 1) return 0;
    u64 P, Q, Pr, Qr;
    gridOf(places, &P, &Q);
    gridOf(nranks, &Pr, &Qr);
    return ((place / Q) % Pr) * Qr + ((place % Q) % Qr);
}
static ocrHint_t edtHintAt(u64 rank) {
    ocrHint_t h; ocrGuid_t aff;
    ocrHintInit(&h, OCR_HINT_EDT_T);
    ocrAffinityGetAt(AFFINITY_PD, rank, &aff);
    ocrSetHintValue(&h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
}
static ocrHint_t dbHintAt(u64 rank) {
    ocrHint_t h; ocrGuid_t aff;
    ocrHintInit(&h, OCR_HINT_DB_T);
    ocrAffinityGetAt(AFFINITY_PD, rank, &aff);
    ocrSetHintValue(&h, OCR_HINT_DB_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
}


/* ------------------------------------------------------------------ kernels */

/* paramv {ts, outEvt}; depv 0: the (k,k) tile, RW */
ocrGuid_t dpotrfTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 ts = paramv[0];
    ocrGuid_t outEvt = (ocrGuid_t){.guid = paramv[1]};
    double *a = (double*)depv[0].ptr;
    if(LAPACKE_dpotrf(LAPACK_ROW_MAJOR, 'L', (int)ts, a, (int)ts) != 0) {
        ocrPrintf("CHOLESKY_DIST INVALID: matrix is not SPD\n");
        ocrShutdown();
        return NULL_GUID;
    }
    ocrDbRelease(depv[0].guid);
    ocrEventSatisfy(outEvt, depv[0].guid);
    return NULL_GUID;
}

/* paramv {ts, outEvt}; depv 0: (j,k) RW, depv 1: the factored (k,k) RO */
ocrGuid_t dtrsmTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 ts = paramv[0];
    ocrGuid_t outEvt = (ocrGuid_t){.guid = paramv[1]};
    double *b = (double*)depv[0].ptr;
    const double *l = (const double*)depv[1].ptr;
    cblas_dtrsm(CblasColMajor, CblasLeft, CblasUpper, CblasTrans, CblasNonUnit,
                (int)ts, (int)ts, 1.0, l, (int)ts, b, (int)ts);
    ocrDbRelease(depv[0].guid);
    ocrEventSatisfy(outEvt, depv[0].guid);
    return NULL_GUID;
}


/* paramv {ts, outEvt}; depv 0: (j,j) RW, depv 1: the panel tile (j,k) RO */
ocrGuid_t dsyrkTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 ts = paramv[0];
    ocrGuid_t outEvt = (ocrGuid_t){.guid = paramv[1]};
    double *a = (double*)depv[0].ptr;
    const double *pj = (const double*)depv[1].ptr;
    cblas_dsyrk(CblasRowMajor, CblasLower, CblasNoTrans, (int)ts, (int)ts, -1.0,
                pj, (int)ts, 1.0, a, (int)ts);
    ocrDbRelease(depv[0].guid);
    ocrEventSatisfy(outEvt, depv[0].guid);
    return NULL_GUID;
}

/* paramv {ts, outEvt}; depv 0: (j,i) RW, depv 1: (j,k) RO, depv 2: (i,k) RO */
ocrGuid_t dgemmTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 ts = paramv[0];
    ocrGuid_t outEvt = (ocrGuid_t){.guid = paramv[1]};
    double *a = (double*)depv[0].ptr;
    /* Each update names its own two panel tiles rather than a gathered copy of
     * the step's panel.  A gather would coalesce a place's panel fetches into
     * one, but it is also a barrier: every update of that place then waits on
     * the whole panel instead of on the two tiles it reads, which is a longer
     * critical path than the fetches it saves. */
    const double *pj = (const double*)depv[1].ptr;
    const double *pi = (const double*)depv[2].ptr;
    cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasTrans, (int)ts, (int)ts, (int)ts,
                -1.0, pj, (int)ts, pi, (int)ts, 1.0, a, (int)ts);
    ocrDbRelease(depv[0].guid);
    ocrEventSatisfy(outEvt, depv[0].guid);
    return NULL_GUID;
}


/* The trace of the factor.  An identity input has a unit factor, so the value
 * is exactly the matrix size and can be checked without a reference run. */
ocrGuid_t finishTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 ts = paramv[0], i, d;
    double trace = 0.0;
    for(d = 0; d < depc; d++) {
        const double *a = (const double*)depv[d].ptr;
        for(i = 0; i < ts; i++) trace += a[i * ts + i];
    }
    ocrPrintf("CHOLESKY trace = %f\n", trace);
    /* Only the diagonal tiles, and only here: this task is ordered after every
     * producer and is the last reader of them.
     *
     * The off-diagonal tiles are deliberately NOT reclaimed.  A tile's final
     * version is the one the TRSM at its own column produces, and that version
     * is exactly what the panel readers of that step consume -- a reaper hung
     * on it races the GEMMs and SYRKs that still have to read it, which is a
     * destroy without an ordering edge and hangs the run.  Freeing them needs a
     * per-tile last-reader count, which the DAG does not carry; the tiles are
     * the program's own working set and one block per tile is live throughout,
     * so what is left unfreed at shutdown is the matrix itself. */
    for(d = 0; d < depc; d++) ocrDbDestroy(depv[d].guid);
    ocrShutdown();
    return NULL_GUID;
}

/* ------------------------------------------------- the place's own program */

/* A tile is rewritten at every step below its column, so the events that carry
 * it are VERSIONED: `tileEvt[(i,j), k]` is tile (i,j) as it stands after step
 * k.  One event per tile would be satisfied once per step, which is not a
 * thing an event does.  The base port versions the same way.
 *
 * paramv {place, t, ts, places}
 * depv 0: the versioned tile-event grid
 * depv 1: the input path
 *
 * Everything below is created HERE, on the place that will run it: a task
 * created with a remote affinity is a message, so a graph assembled in one
 * place carries a startup cost that no node count reduces.
 */
ocrGuid_t placeInitTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 g = paramv[0], t = paramv[1], ts = paramv[2], places = paramv[3];
    #define EVBASE(I,J) evBase((I),(J))
    const u64 *tileEvt = (const u64*)depv[0].ptr;
    const char *fileIn = (const char*)depv[1].ptr;
    u64 P, Q, k, i, j, n;
    gridOf(places, &P, &Q);
    ocrHint_t h = edtHintAt(placeRank(g, places));
    #define TV(I,J,K) tileEvt[EVBASE(I,J) + (K)]

    /* Each place reads ITS OWN tiles.  The tile stream is fixed-size records in
     * (i, j<=i) order, so a tile's offset is arithmetic and a place can seek to
     * the ones it owns without any other place being involved.  Reading them
     * all on one rank -- t(t+1)/2 records and as many datablock creates -- is a
     * serial term that no node count reduces.  Other OCR ports reach the same
     * effect with a per-block reader invoked on the block's owner; a seek does
     * it here because the record layout is regular. */
    {
        FILE *fin = fopen(fileIn, "rb");
        if(!fin) {
            ocrPrintf("Place %lu cannot open %s\n", g, fileIn);
            ocrShutdown();
            return NULL_GUID;
        }
        u64 sz = ts * ts * sizeof(double);
        for(i = 0; i < t; i++)
            for(j = 0; j <= i; j++) {
                if(ownerOf(i, j, P, Q) != g) continue;
                ocrGuid_t db; void *p;
                ocrHint_t dh = dbHintAt(placeRank(g, places));
                ocrDbCreate(&db, &p, sz, FLAGS, &dh, NO_ALLOC);
                if(fseek(fin, (long)((i*(i+1)/2 + j) * sz), SEEK_SET) != 0 ||
                   fread(p, sz, 1, fin) != 1) {
                    ocrPrintf("Tile-binary input ends short at tile (%lu,%lu)\n", i, j);
                    fclose(fin);
                    ocrShutdown();
                    return NULL_GUID;
                }
                ocrDbRelease(db);
                ocrEventSatisfy((ocrGuid_t){.guid = TV(i,j,0)}, db);
            }
        fclose(fin);
    }

    ocrGuid_t potrfTml, trsmTml, syrkTml, gemmTml;
    ocrEdtTemplateCreate(&potrfTml, dpotrfTask, 2, 1);
    ocrEdtTemplateCreate(&trsmTml,  dtrsmTask,  2, 2);
    ocrEdtTemplateCreate(&syrkTml,  dsyrkTask,  2, 2);
    ocrEdtTemplateCreate(&gemmTml,  dgemmTask,  2, 3);

    for(k = 0; k < t; k++) {
        /* 1. the diagonal factorisation: (k,k) at version k becomes version k+1 */
        if(ownerOf(k, k, P, Q) == g) {
            ocrGuid_t e;
            u64 prm[2] = {ts, TV(k,k,k+1)};
            ocrEdtCreate(&e, potrfTml, EDT_PARAM_DEF, prm, EDT_PARAM_DEF, NULL,
                         EDT_PROP_NONE, &h, NULL);
            ocrAddDependence((ocrGuid_t){.guid = TV(k,k,k)}, e, 0, DB_MODE_RW);
        }
        /* 2. the panel column, each tile on its owner */
        for(j = k + 1; j < t; j++) {
            if(ownerOf(j, k, P, Q) != g) continue;
            ocrGuid_t e;
            u64 prm[2] = {ts, TV(j,k,k+1)};
            ocrEdtCreate(&e, trsmTml, EDT_PARAM_DEF, prm, EDT_PARAM_DEF, NULL,
                         EDT_PROP_NONE, &h, NULL);
            ocrAddDependence((ocrGuid_t){.guid = TV(j,k,k)},   e, 0, DB_MODE_RW);
            ocrAddDependence((ocrGuid_t){.guid = TV(k,k,k+1)}, e, 1, DB_MODE_RO);
        }
        /* 3. the trailing update: one task per tile, so the width follows the
         *    tile count and not the place count.  There is no per-place panel
         *    gather -- it was built, measured and removed; see dgemmTask. */

        for(j = k + 1; j < t; j++) {
            for(i = k + 1; i <= j; i++) {
                if(ownerOf(j, i, P, Q) != g) continue;
                ocrGuid_t e;
                if(i == j) {
                    u64 prm[2] = {ts, TV(j,j,k+1)};
                    ocrEdtCreate(&e, syrkTml, EDT_PARAM_DEF, prm, EDT_PARAM_DEF,
                                 NULL, EDT_PROP_NONE, &h, NULL);
                } else {
                    u64 prm[2] = {ts, TV(j,i,k+1)};
                    ocrEdtCreate(&e, gemmTml, EDT_PARAM_DEF, prm, EDT_PARAM_DEF,
                                 NULL, EDT_PROP_NONE, &h, NULL);
                }
                ocrAddDependence((ocrGuid_t){.guid = TV(j,i,k)}, e, 0, DB_MODE_RW);
                ocrAddDependence((ocrGuid_t){.guid = TV(j,k,k+1)}, e, 1, DB_MODE_RO);
                if(i != j)
                    ocrAddDependence((ocrGuid_t){.guid = TV(i,k,k+1)}, e, 2, DB_MODE_RO);
            }
        }
    }
    ocrEdtTemplateDestroy(potrfTml);
    ocrEdtTemplateDestroy(trsmTml);
    ocrEdtTemplateDestroy(syrkTml);
    ocrEdtTemplateDestroy(gemmTml);
    #undef TV
    #undef EVBASE
    return NULL_GUID;
}

/* --------------------------------------------------------------- the driver */

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    void *argv = depv[0].ptr;
    u64 argc = ocrGetArgc(argv);
    u64 ds = 0, ts = 100, places = 32;
    char *fileIn = NULL;
    u64 a;

    for(a = 1; a + 1 < argc; a += 2) {
        char *o = ocrGetArgv(argv, a), *v = ocrGetArgv(argv, a + 1);
        if(!strcmp(o, "--ds")) ds = (u64)atol(v);
        else if(!strcmp(o, "--ts")) ts = (u64)atol(v);
        else if(!strcmp(o, "--places")) places = (u64)atol(v);
        else if(!strcmp(o, "--fib")) fileIn = v;
    }
    if(ds == 0 || ts == 0 || ds % ts != 0 || fileIn == NULL) {
        ocrPrintf("USAGE: cholesky_dist --ds <n> --ts <n> --fib <tile-stream> "
                  "[--places <n>]   (ds must be a multiple of ts)\n");
        ocrShutdown();
        return NULL_GUID;
    }
    ocrPrintf("cholesky_dist: ds=%lu ts=%lu places=%lu\n", ds, ts, places);
    u64 t = ds / ts;
    if(places > t * t) places = t * t;
    if(places == 0) places = 1;

    u64 P, Q; gridOf(places, &P, &Q);
    u64 ntile = t * (t + 1) / 2;
    u64 i, j, k, gg;

    /* The versioned tile-event grid travels as a block: one entry per tile
     * version is far past a template's parameter count, and every place reads
     * all of it.
     *
     * A labeled GUID range would let each place derive these names arithmetically
     * and keep the creations off this task -- which is what the PNNL port does
     * with ocrReserveGuidExt.  It was tried and does not hold here: creating a
     * labeled guid on this runtime REPLACES whatever stands at it, so a second
     * creation silently discards dependences already registered on the first,
     * and the program hangs.  Left as the grid. */
    ocrGuid_t tileDb; u64 *tileEvt;
    u64 nevt = evBase(t - 1, t - 1) + (t - 1) + 2;
    ocrDbCreate(&tileDb, (void**)&tileEvt, nevt * sizeof(u64),
                FLAGS, NULL_HINT, NO_ALLOC);
    for(i = 0; i < t; i++)
        for(j = 0; j <= i; j++)
            for(k = 0; k <= j + 1; k++) {
                /* Sticky, not counted.  A counted event is reclaimed once its
                 * declared consumers have bound, and the count is expressible
                 * here, but it is not a portable event flavour: an
                 * implementation may support only the ones it needs, and one
                 * this program must run on accepts only latches and channels
                 * through the params entry point.  What counting would buy is
                 * small against a working set that is the matrix itself, so
                 * the grid stays sticky and the saving comes from not creating
                 * names nothing ever reads. */
                ocrGuid_t e;
                ocrEventCreate(&e, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);
                tileEvt[evBase(i, j) + k] = (u64)e.guid;
            }
    ocrDbRelease(tileDb);


    /* One init task per place, and nothing else: O(P) work here. */
    /* The finisher reads every tile's final diagonal version: tile (i,i) is
     * last written by the POTRF at step i, so its final version is i+1. */
    ocrGuid_t finishTml, finishEdt;
    u64 finishPrm[1] = {ts};
    ocrEdtTemplateCreate(&finishTml, finishTask, 1, (u32)t);
    ocrEdtCreate(&finishEdt, finishTml, EDT_PARAM_DEF, finishPrm, EDT_PARAM_DEF,
                 NULL, EDT_PROP_NONE, NULL_HINT, NULL);
    ocrEdtTemplateDestroy(finishTml);
    for(i = 0; i < t; i++)
        ocrAddDependence((ocrGuid_t){.guid = tileEvt[evBase(i, i) + i + 1]},
                         finishEdt, (u32)i, DB_MODE_RO);

    /* The path travels as a block: every place opens the file itself. */
    ocrGuid_t nameDb; char *namePtr;
    ocrDbCreate(&nameDb, (void**)&namePtr, strlen(fileIn) + 1, FLAGS, NULL_HINT, NO_ALLOC);
    strcpy(namePtr, fileIn);
    ocrDbRelease(nameDb);

    ocrGuid_t placeTml;
    ocrEdtTemplateCreate(&placeTml, placeInitTask, 4, 2);
    for(gg = 0; gg < places; gg++) {
        ocrGuid_t e;
        ocrHint_t h = edtHintAt(placeRank(gg, places));
        u64 prm[4] = {gg, t, ts, places};
        ocrEdtCreate(&e, placeTml, EDT_PARAM_DEF, prm, EDT_PARAM_DEF, NULL,
                     EDT_PROP_NONE, &h, NULL);
        ocrAddDependence(tileDb, e, 0, DB_MODE_RO);
        ocrAddDependence(nameDb, e, 1, DB_MODE_RO);
    }
    ocrEdtTemplateDestroy(placeTml);

    return NULL_GUID;
}

