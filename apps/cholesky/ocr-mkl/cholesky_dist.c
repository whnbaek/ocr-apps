/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

/* Tiled Cholesky whose task graph is named, built and fed where it runs.
 *
 * A right-looking tiled factorization is a per-tile DAG: at step k the diagonal
 * tile is factored, the panel below it is solved against that factor, and every
 * tile of the trailing submatrix is updated from the two panel tiles in its row
 * and its column.  Expressing that the obvious way builds the entire graph in
 * the starting task -- on the order of t^3/6 creations for t tiles per
 * dimension, and as many event names again -- and each creation whose task
 * belongs elsewhere is a message.  The graph construction then serializes on
 * one place however well the work itself is placed.
 *
 * Here the tile partition is an argument.  The tiles are divided across
 * `places` by the two-dimensional block-cyclic map below, and a place creates
 * the names, the blocks and the tasks for the tiles it owns, on itself.  Each
 * of the three is proportional to the tiles owned and each enumerates only the
 * owned indices, so no phase of startup walks the whole index space and none of
 * it rests on one place.  What crosses a place boundary at startup is the input
 * path and one block of names per place; everything else is local.  The
 * partition, the task count and the order the factor is computed in do not
 * depend on how many ranks the program runs on -- a place is a unit of
 * ownership, and the machine enters only through the hint that maps a place
 * onto a rank.
 */

#include "ocr.h"
#include "extensions/ocr-affinity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cblas.h>
#include <lapacke.h>

#include "cholesky_instance.h"

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

/* The owner of place index g on that grid holds exactly the tiles whose row is
 * congruent to g/Q modulo P and whose column is congruent to g%Q modulo Q, so
 * an owner steps through its own indices instead of testing every one: this is
 * the first index at or after `lo` that is congruent to r modulo m. */
static u64 strideFrom(u64 lo, u64 r, u64 m) { return lo + ((r + m - lo % m) % m); }

/* Lower-triangular tiles in (i, j<=i) order. */
static u64 tileIdx(u64 i, u64 j) { return i * (i + 1) / 2 + j; }

/* Tile (i,j) exists in versions 0..j+1 and no further: it is updated once at
 * each step below its own column and finalised at step j.  Numbering every tile
 * up to the last step instead wastes two thirds of the names on versions
 * nothing ever satisfies or reads.
 *
 * A tile's version names live in its owner's block, contiguous and in the
 * owner's own enumeration order.  That layout is a pure function of the
 * partition, so any place can address any tile's names from the partition alone
 * and no name has to be gathered anywhere.  off[] is filled for every tile;
 * fill[] is scratch of one counter per place. */
static void tileOffsets(u64 t, u64 P, u64 Q, u64 places, u64 *off, u64 *fill) {
    u64 i, j, g;
    for(g = 0; g < places; ++g) fill[g] = 0;
    for(i = 0; i < t; ++i)
        for(j = 0; j <= i; ++j) {
            g = ownerOf(i, j, P, Q);
            off[tileIdx(i, j)] = fill[g];
            fill[g] += j + 2;
        }
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
    /* An update waits on the two panel tiles it reads.  Gathering a place's
     * panel into one copy coalesces its fetches but orders every update of that
     * place behind the whole panel, which is the longer critical path. */
    const double *pj = (const double*)depv[1].ptr;
    const double *pi = (const double*)depv[2].ptr;
    cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasTrans, (int)ts, (int)ts, (int)ts,
                -1.0, pj, (int)ts, pi, (int)ts, 1.0, a, (int)ts);
    ocrDbRelease(depv[0].guid);
    ocrEventSatisfy(outEvt, depv[0].guid);
    return NULL_GUID;
}


/* The trace of the factor.  Its diagonal tiles are the last tiles the
 * factorization finishes, and each of them absorbed every update of its own
 * row and column, so the sum is a function of the whole computation.  The
 * summation walks the diagonal tiles in index order, which the dependence
 * slots fix, so the value does not depend on the order the tasks ran in. */
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
     * version is the one the panel solve at its own column produces, and that
     * version is exactly what the panel readers of that step consume -- a
     * reaper hung on it races the updates that still have to read it, which is
     * a destroy without an ordering edge and hangs the run.  Freeing them needs
     * a per-tile last-reader count, which the DAG does not carry; the tiles are
     * the program's own working set and one block per tile is live throughout,
     * so what is left unfreed at shutdown is the matrix itself. */
    for(d = 0; d < depc; d++) ocrDbDestroy(depv[d].guid);
    ocrShutdown();
    return NULL_GUID;
}

/* ------------------------------------------------- the place's own program */

/* A tile is rewritten at every step below its column, so the events that carry
 * it are VERSIONED: one event per tile would be satisfied once per step, which
 * is not a thing an event does.
 *
 * This task creates the names for the tiles this place owns and loads those
 * tiles, and publishes the names as one block.  Both loops step by the
 * partition, so the cost of each is proportional to the tiles owned rather than
 * to the whole index space.
 *
 * paramv {place, t, ts, places, nameEvt}
 * depv 0: the input path
 */
ocrGuid_t placeNameTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 g = paramv[0], t = paramv[1], ts = paramv[2], places = paramv[3];
    ocrGuid_t nameEvt = (ocrGuid_t){.guid = paramv[4]};
    const char *fileIn = (const char*)depv[0].ptr;
    u64 P, Q, ri, cj, i, j, k, n = 0;
    gridOf(places, &P, &Q);
    ri = g / Q; cj = g % Q;

    for(i = ri; i < t; i += P)
        for(j = cj; j <= i; j += Q) n += j + 2;

    ocrGuid_t nameDb; u64 *names;
    ocrDbCreate(&nameDb, (void**)&names, (n ? n : 1) * sizeof(u64),
                FLAGS, NULL_HINT, NO_ALLOC);

    /* Each place produces ITS OWN tiles.  With no file the contents of a tile
     * are a function of its indices, so a place fills what it owns and nothing
     * about the matrix crosses a place boundary at all.  With a file the stream
     * is fixed-size records in (i, j<=i) order, so a tile's offset is
     * arithmetic and a place seeks to the ones it owns without any other place
     * being involved.  Either way, doing it all on one place -- t(t+1)/2
     * records and as many datablock creates -- is a serial term that no node
     * count reduces. */
    FILE *fin = NULL;
    if(fileIn[0] != '\0') {
        fin = fopen(fileIn, "rb");
        if(!fin) {
            ocrPrintf("Place %lu cannot open %s\n", g, fileIn);
            ocrShutdown();
            return NULL_GUID;
        }
    }
    ocrHint_t dh = dbHintAt(placeRank(g, places));
    u64 sz = ts * ts * sizeof(double);
    n = 0;

    /* Sticky, not counted.  A counted event is reclaimed once its declared
     * consumers have bound, and the count is expressible here, but it is not a
     * portable event flavour: an implementation may support only the ones it
     * needs.  What counting would buy is small against a working set that is
     * the matrix itself, so the names stay sticky and the saving comes from not
     * creating names nothing ever reads. */
    for(i = ri; i < t; i += P)
        for(j = cj; j <= i; j += Q) {
            for(k = 0; k <= j + 1; k++) {
                ocrGuid_t e;
                ocrEventCreate(&e, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);
                names[n + k] = (u64)e.guid;
            }
            ocrGuid_t db; void *p;
            ocrDbCreate(&db, &p, sz, FLAGS, &dh, NO_ALLOC);
            if(fin == NULL) {
                choleskyFillTile((double*)p, i, j, ts);
            } else if(fseek(fin, (long)(tileIdx(i, j) * sz), SEEK_SET) != 0 ||
                      fread(p, sz, 1, fin) != 1) {
                ocrPrintf("Tile-binary input ends short at tile (%lu,%lu)\n", i, j);
                fclose(fin);
                ocrShutdown();
                return NULL_GUID;
            }
            ocrDbRelease(db);
            ocrEventSatisfy((ocrGuid_t){.guid = names[n]}, db);
            n += j + 2;
        }
    if(fin) fclose(fin);

    ocrDbRelease(nameDb);
    ocrEventSatisfy(nameEvt, nameDb);
    return NULL_GUID;
}

/* paramv {place, t, ts, places, finishEdt}
 * depv g: place g's block of version names, for every place
 *
 * Everything below is created HERE, on the place that will run it: a task
 * created with a remote affinity is a message, so a graph assembled in one
 * place carries a startup cost that no node count reduces.  The enumeration is
 * strided by the partition too, so a place walks its own tiles instead of the
 * whole index space and discarding what it does not own.
 */
ocrGuid_t placeInitTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 g = paramv[0], t = paramv[1], ts = paramv[2], places = paramv[3];
    ocrGuid_t finishEdt = (ocrGuid_t){.guid = paramv[4]};
    u64 P, Q, ri, cj, k, i, j;
    gridOf(places, &P, &Q);
    ri = g / Q; cj = g % Q;

    u64 *off = (u64*)malloc(t * (t + 1) / 2 * sizeof(u64));
    u64 *fill = (u64*)malloc(places * sizeof(u64));
    if(off == NULL || fill == NULL) {
        ocrPrintf("Place %lu cannot hold the name index\n", g);
        ocrShutdown();
        return NULL_GUID;
    }
    tileOffsets(t, P, Q, places, off, fill);
    free(fill);

    #define TV(I,J,K) (((const u64*)depv[ownerOf((I),(J),P,Q)].ptr) \
                       [off[tileIdx((I),(J))] + (K)])

    ocrHint_t h = edtHintAt(placeRank(g, places));
    ocrGuid_t potrfTml, trsmTml, syrkTml, gemmTml;
    ocrEdtTemplateCreate(&potrfTml, dpotrfTask, 2, 1);
    ocrEdtTemplateCreate(&trsmTml,  dtrsmTask,  2, 2);
    ocrEdtTemplateCreate(&syrkTml,  dsyrkTask,  2, 2);
    ocrEdtTemplateCreate(&gemmTml,  dgemmTask,  2, 3);

    for(k = 0; k < t; k++) {
        /* 1. the diagonal factorisation: (k,k) at version k becomes version k+1.
         *    Its owner is also the only place holding the name of that final
         *    version, so it is the one that wires the diagonal to the finisher. */
        if(k % P == ri && k % Q == cj) {
            ocrGuid_t e;
            u64 prm[2] = {ts, TV(k,k,k+1)};
            ocrEdtCreate(&e, potrfTml, EDT_PARAM_DEF, prm, EDT_PARAM_DEF, NULL,
                         EDT_PROP_NONE, &h, NULL);
            ocrAddDependence((ocrGuid_t){.guid = TV(k,k,k)}, e, 0, DB_MODE_RW);
            ocrAddDependence((ocrGuid_t){.guid = TV(k,k,k+1)}, finishEdt,
                             (u32)k, DB_MODE_RO);
        }
        /* 2. the panel column, each tile on its owner */
        if(k % Q == cj)
            for(j = strideFrom(k + 1, ri, P); j < t; j += P) {
                ocrGuid_t e;
                u64 prm[2] = {ts, TV(j,k,k+1)};
                ocrEdtCreate(&e, trsmTml, EDT_PARAM_DEF, prm, EDT_PARAM_DEF, NULL,
                             EDT_PROP_NONE, &h, NULL);
                ocrAddDependence((ocrGuid_t){.guid = TV(j,k,k)},   e, 0, DB_MODE_RW);
                ocrAddDependence((ocrGuid_t){.guid = TV(k,k,k+1)}, e, 1, DB_MODE_RO);
            }
        /* 3. the trailing update: one task per tile, so the width follows the
         *    tile count and not the place count. */
        for(j = strideFrom(k + 1, ri, P); j < t; j += P)
            for(i = strideFrom(k + 1, cj, Q); i <= j; i += Q) {
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
    ocrEdtTemplateDestroy(potrfTml);
    ocrEdtTemplateDestroy(trsmTml);
    ocrEdtTemplateDestroy(syrkTml);
    ocrEdtTemplateDestroy(gemmTml);
    #undef TV
    free(off);
    return NULL_GUID;
}

/* --------------------------------------------------------------- the driver */

static void usage(void) {
    ocrPrintf("USAGE: cholesky_dist --ds <n> --ts <n> [--fib <tile-stream>] "
              "[--places <n>]   (ds must be a multiple of ts; a given places "
              "must lie in 1..(ds/ts)^2, the default is clamped to it; with no "
              "--fib the matrix is generated from its indices by the place that "
              "owns each tile)\n");
}

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    void *argv = depv[0].ptr;
    u64 argc = ocrGetArgc(argv);
    u64 ds = 0, ts = 100, places = 32;
    char *fileIn = NULL;
    u64 a, i, gg, t;
    int bad = 0, placesGiven = 0;

    /* Every token is either a known option with a value or an error: an
     * argument that is ignored silently runs a decomposition nobody asked
     * for. */
    for(a = 1; a < argc; a += 2) {
        char *o = ocrGetArgv(argv, a);
        if(a + 1 >= argc) {
            ocrPrintf("cholesky_dist: option %s has no value\n", o);
            bad = 1;
            break;
        }
        char *v = ocrGetArgv(argv, a + 1);
        if(!strcmp(o, "--ds")) ds = (u64)atol(v);
        else if(!strcmp(o, "--ts")) ts = (u64)atol(v);
        else if(!strcmp(o, "--places")) { places = (u64)atol(v); placesGiven = 1; }
        else if(!strcmp(o, "--fib")) fileIn = v;
        else {
            ocrPrintf("cholesky_dist: unknown option %s\n", o);
            bad = 1;
            break;
        }
    }
    if(bad || ds == 0 || ts == 0 || ds % ts != 0) {
        usage();
        ocrShutdown();
        return NULL_GUID;
    }
    t = ds / ts;
    /* A REQUESTED partition that cannot be honoured is rejected rather than
     * clamped: the partition is the subject of the measurement, so a value that
     * was asked for must not be replaced by one the run then reports as if it
     * had been.  The default was not asked for, so it is clamped to the largest
     * partition the tile count admits and the line below reports what is used
     * -- a size small enough to shrink the default must not turn into an error
     * about an option nobody passed. */
    if(placesGiven && (places == 0 || places > t * t)) {
        ocrPrintf("cholesky_dist: --places %lu is outside 1..%lu for t=%lu\n",
                  places, t * t, t);
        usage();
        ocrShutdown();
        return NULL_GUID;
    }
    if(places > t * t) places = t * t;
    ocrPrintf("cholesky_dist: ds=%lu ts=%lu places=%lu\n", ds, ts, places);

    /* The finisher reads every tile's final diagonal version: tile (i,i) is
     * last written at step i, so its final version is i+1.  The names live in
     * the owning places, so the owners register those dependences. */
    ocrGuid_t finishTml, finishEdt;
    u64 finishPrm[1] = {ts};
    ocrEdtTemplateCreate(&finishTml, finishTask, 1, (u32)t);
    ocrEdtCreate(&finishEdt, finishTml, EDT_PARAM_DEF, finishPrm, EDT_PARAM_DEF,
                 NULL, EDT_PROP_NONE, NULL_HINT, NULL);
    ocrEdtTemplateDestroy(finishTml);

    /* The path travels as a block: every place opens the file itself.  An
     * empty path is the absence of one, and a place then generates the tiles
     * it owns instead of reading them. */
    const char *path = (fileIn != NULL) ? fileIn : "";
    ocrGuid_t pathDb; char *pathPtr;
    ocrDbCreate(&pathDb, (void**)&pathPtr, strlen(path) + 1, FLAGS, NULL_HINT, NO_ALLOC);
    strcpy(pathPtr, path);
    ocrDbRelease(pathDb);

    /* A place's names are published through one event, so a place can build its
     * own tasks as soon as every place has named its tiles.  A labeled GUID
     * range would let a place derive the others' names with no exchange at all;
     * it does not hold here, because creating a labeled guid on this runtime
     * REPLACES whatever stands at it, so a second creation silently discards
     * dependences already registered on the first. */
    ocrGuid_t *nameEvt = (ocrGuid_t*)malloc(places * sizeof(ocrGuid_t));
    if(nameEvt == NULL) {
        ocrPrintf("cholesky_dist: cannot hold %lu place names\n", places);
        ocrShutdown();
        return NULL_GUID;
    }
    for(gg = 0; gg < places; gg++)
        ocrEventCreate(&nameEvt[gg], OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);

    ocrGuid_t nameTml;
    ocrEdtTemplateCreate(&nameTml, placeNameTask, 5, 1);
    for(gg = 0; gg < places; gg++) {
        ocrGuid_t e;
        ocrHint_t h = edtHintAt(placeRank(gg, places));
        u64 prm[5] = {gg, t, ts, places, (u64)nameEvt[gg].guid};
        ocrEdtCreate(&e, nameTml, EDT_PARAM_DEF, prm, EDT_PARAM_DEF, NULL,
                     EDT_PROP_NONE, &h, NULL);
        ocrAddDependence(pathDb, e, 0, DB_MODE_RO);
    }
    ocrEdtTemplateDestroy(nameTml);

    ocrGuid_t placeTml;
    ocrEdtTemplateCreate(&placeTml, placeInitTask, 5, (u32)places);
    for(gg = 0; gg < places; gg++) {
        ocrGuid_t e;
        ocrHint_t h = edtHintAt(placeRank(gg, places));
        u64 prm[5] = {gg, t, ts, places, (u64)finishEdt.guid};
        ocrEdtCreate(&e, placeTml, EDT_PARAM_DEF, prm, EDT_PARAM_DEF, NULL,
                     EDT_PROP_NONE, &h, NULL);
        for(i = 0; i < places; i++)
            ocrAddDependence(nameEvt[i], e, (u32)i, DB_MODE_RO);
    }
    ocrEdtTemplateDestroy(placeTml);
    free(nameEvt);

    return NULL_GUID;
}
