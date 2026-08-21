/*
 * Wavefront restructuring of the tiled LCS/edit-distance kernel.
 *
 * The same computation as lcs_distributed.cpp — identical strings (same
 * per-tile seeds), identical boundary values, identical per-cell recurrence,
 * so the two programs print the same answer — but the task and data planes
 * are rebuilt around the dependences the algorithm actually has:
 *
 *   - A tile's leaf consumes only its neighbors' BOUNDARY strips (the west
 *     tile's rightmost column, the north tile's bottom row, the northwest
 *     corner cell), so those strips are what travels: each leaf emits an
 *     rc/br/corner block and hands each to its single consumer through a
 *     labeled COUNTED event (declared consumer count 1 — reclaimed as soon
 *     as that consumer has registered).  Full score tiles are never
 *     materialized: the leaf computes with two rolling rows.
 *   - Leaves are created by a free-running per-row spawner chain and become
 *     runnable exactly when their three input strips exist — anti-diagonal
 *     wavefront parallelism of width up to N/base, not the source
 *     recursion's quadrant ordering.
 *   - Consumed strips are destroyed by their single consumer, and a leaf's
 *     scratch is transient, so the resident set follows the wavefront
 *     frontier instead of the score matrix.
 *
 * Boundary tiles synthesize their missing strips analytically (row 0 and
 * column 0 of the score matrix are the index sequences 0..N), which is what
 * the boundary-block init EDTs of the tiled original compute.
 */
#ifndef ENABLE_EXTENSION_LABELING
#define ENABLE_EXTENSION_LABELING
#endif

#include "ocr.h"
#include "extensions/ocr-labeling.h"
#include "extensions/ocr-affinity.h"

#include <stdio.h>
#include <stdlib.h>

#include "cilktime.h"

#define MAX(a, b) ((a > b) ? a : b)
#define MIN(a, b) ((a < b) ? a : b)
#define GAP_PENALTY 0

typedef struct {
    u64 N, base, bi, bj;
    ocrGuid_t s_labels, t_labels, rc_labels, br_labels, co_labels;
} wf_params_t;

#ifndef LCS_ROW_BANDS_PER_RANK
#define LCS_ROW_BANDS_PER_RANK 1
#endif

/* Row-band placement, shared with the tiled original: block-row bi's work
 * and data live on band((bi-1)) of the rank grid, so a leaf's west strip is
 * always band-local and its north strip crosses ranks only at band
 * boundaries. */
static u64 bandOf(u64 row, u64 rows)
{
    u64 nranks;
    ocrAffinityCount(AFFINITY_PD, &nranks);
    u64 band_total = nranks * (u64)LCS_ROW_BANDS_PER_RANK;
    u64 hgt = (rows + band_total - 1) / band_total;
    return (row / hgt) % nranks;
}

/* Labeled-range index re-encoding: the runtimes home a labeled range's
 * members round-robin by index, so encoding the band as the low residue
 * steers each member onto its band's rank while distinct indices stay
 * distinct.  Identity when the placement layer is off. */
static u64 bandIdx(u64 idx, u64 row, u64 rows)
{
    u64 nranks;
    ocrAffinityCount(AFFINITY_PD, &nranks);
    return bandOf(row, rows) + nranks * idx;
}

static ocrHint_t *bandEdtHint(ocrHint_t *h, u64 row, u64 rows)
{
    u64 nranks;
    ocrAffinityCount(AFFINITY_PD, &nranks);
    if (nranks <= 1)
        return NULL_HINT;
    ocrGuid_t aff;
    ocrAffinityGetAt(AFFINITY_PD, bandOf(row, rows), &aff);
    ocrHintInit(h, OCR_HINT_EDT_T);
    ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
}

/* One event per emitted strip, indexed by the emitting tile (bi, bj) over
 * the (L+1)-wide grid the tiled original uses. */
static u64 tileIdx(u64 bi, u64 bj, u64 L)
{
    return bandIdx(bi * (L + 1) + bj, bi - 1, L + 1);
}

static ocrGuid_t stripEvt(ocrGuid_t range, u64 bi, u64 bj, u64 L)
{
    ocrGuid_t g;
    ocrGuidFromIndex(&g, range, tileIdx(bi, bj, L));
    return g;
}

/* Emit one strip: its payload block, and the labeled COUNTED(1) event its
 * single consumer waits on.  Created at satisfy time — a consumer that
 * registered first parked on the absent label and is fired by the install. */
static void emitStrip(ocrGuid_t range, u64 bi, u64 bj, u64 L,
                      const int *src, u64 nelem)
{
    ocrGuid_t db;
    int *p = NULL;
    ocrDbCreate(&db, (void **)&p, sizeof(int) * nelem, DB_PROP_NONE,
                NULL_HINT, NO_ALLOC);
    for (u64 k = 0; k < nelem; k++)
        p[k] = src[k];
    ocrDbRelease(db);

    ocrGuid_t evt = stripEvt(range, bi, bj, L);
    ocrEventParams_t params;
    params.EVENT_COUNTED.nbDeps = 1;
    ocrEventCreateParams(&evt, OCR_EVENT_COUNTED_T,
                         GUID_PROP_IS_LABELED | EVT_PROP_TAKES_ARG, &params);
    ocrEventSatisfy(evt, db);
}

/* depv: 0 = S tile, 1 = T tile, 2 = west rc strip (NULL on column 1),
 * 3 = north br strip (NULL on row 1), 4 = northwest corner (NULL on either
 * boundary). */
static ocrGuid_t leafEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    (void)paramc;
    (void)depc;
    wf_params_t *p = (wf_params_t *)paramv;
    u64 n = p->base;
    u64 L = p->N / p->base;
    u64 bi = p->bi, bj = p->bj;

    /* Tile 0 of each string carries a leading boundary slot. */
    const int *S = (const int *)depv[0].ptr;
    const int *T = (const int *)depv[1].ptr;
    if (bi == 1)
        S++;
    if (bj == 1)
        T++;

    const int *left_strip = (const int *)depv[2].ptr;
    const int *above_strip = (const int *)depv[3].ptr;
    const int *corner_p = (const int *)depv[4].ptr;

    int *scratch = (int *)malloc(sizeof(int) * n * 3);
    int *prev = scratch, *cur = scratch + n, *rc = scratch + 2 * n;

    /* Score row 0 / column 0 are the index sequences the tiled original's
     * boundary-init tasks write; a first-row/column tile synthesizes them. */
    int corner;
    if (corner_p != NULL)
        corner = *corner_p;
    else if (bi == 1 && bj == 1)
        corner = 0;
    else if (bi == 1)
        corner = (int)((bj - 1) * n);
    else
        corner = (int)((bi - 1) * n);

    for (u64 r = 0; r < n; r++) {
        int lval = left_strip ? left_strip[r] : (int)((bi - 1) * n + 1 + r);
        int lprev;
        if (r == 0)
            lprev = corner;
        else
            lprev = left_strip ? left_strip[r - 1]
                               : (int)((bi - 1) * n + r);
        for (u64 c = 0; c < n; c++) {
            int aval = (r == 0)
                           ? (above_strip ? above_strip[c]
                                          : (int)((bj - 1) * n + 1 + c))
                           : prev[c];
            int upleft;
            if (c == 0)
                upleft = lprev;
            else if (r == 0)
                upleft = above_strip ? above_strip[c - 1]
                                     : (int)((bj - 1) * n + c);
            else
                upleft = prev[c - 1];
            int leftv = (c == 0) ? lval : cur[c - 1];
            int match = upleft + (S[r] != T[c]);
            cur[c] = MIN(match, MAX(leftv, aval) + GAP_PENALTY);
        }
        rc[r] = cur[n - 1];
        int *tmp = prev;
        prev = cur;
        cur = tmp;
    }
    /* prev now holds the tile's bottom row; rc its right column. */

    if (bj < L)
        emitStrip(p->rc_labels, bi, bj, L, rc, n);
    if (bi < L)
        emitStrip(p->br_labels, bi, bj, L, prev, n);
    if ((bi < L && bj < L) || (bi == L && bj == L)) {
        int c0 = prev[n - 1];
        emitStrip(p->co_labels, bi, bj, L, &c0, 1);
    }

    free(scratch);

    /* Each consumed strip had exactly this leaf as its consumer. */
    if (depv[2].ptr != NULL)
        ocrDbDestroy(depv[2].guid);
    if (depv[3].ptr != NULL)
        ocrDbDestroy(depv[3].guid);
    if (depv[4].ptr != NULL)
        ocrDbDestroy(depv[4].guid);

    return NULL_GUID;
}

/* Creates row bi's leaves band-locally, then the next row's spawner.  The
 * chain free-runs ahead of the data: a leaf parks on its input events, so
 * creation order never constrains the wavefront. */
static ocrGuid_t rowSpawnerEdt(u32 paramc, u64 *paramv, u32 depc,
                               ocrEdtDep_t depv[])
{
    (void)paramc;
    (void)depc;
    (void)depv;
    wf_params_t *p = (wf_params_t *)paramv;
    u64 L = p->N / p->base;
    u64 bi = p->bi;

    ocrGuid_t leafTml;
    ocrEdtTemplateCreate(&leafTml, leafEdt, sizeof(wf_params_t) / sizeof(u64),
                         5);

    wf_params_t lp = *p;
    for (u64 bj = 1; bj <= L; bj++) {
        lp.bj = bj;
        ocrHint_t h;
        ocrGuid_t leaf;
        ocrEdtCreate(&leaf, leafTml, EDT_PARAM_DEF, (u64 *)&lp, EDT_PARAM_DEF,
                     NULL, EDT_PROP_NONE, bandEdtHint(&h, bi - 1, L + 1),
                     NULL);
        ocrGuid_t sTile, tTile;
        ocrGuidFromIndex(&sTile, p->s_labels, bandIdx(bi - 1, bi - 1, L + 1));
        ocrGuidFromIndex(&tTile, p->t_labels, bj - 1);
        ocrAddDependence(sTile, leaf, 0, DB_MODE_RO);
        ocrAddDependence(tTile, leaf, 1, DB_MODE_RO);
        ocrAddDependence(bj > 1 ? stripEvt(p->rc_labels, bi, bj - 1, L)
                                : NULL_GUID,
                         leaf, 2, DB_MODE_RO);
        ocrAddDependence(bi > 1 ? stripEvt(p->br_labels, bi - 1, bj, L)
                                : NULL_GUID,
                         leaf, 3, DB_MODE_RO);
        ocrAddDependence((bi > 1 && bj > 1)
                             ? stripEvt(p->co_labels, bi - 1, bj - 1, L)
                             : NULL_GUID,
                         leaf, 4, DB_MODE_RO);
    }
    ocrEdtTemplateDestroy(leafTml);

    if (bi < L) {
        wf_params_t np = *p;
        np.bi = bi + 1;
        ocrGuid_t tml, next;
        ocrEdtTemplateCreate(&tml, rowSpawnerEdt,
                             sizeof(wf_params_t) / sizeof(u64), 1);
        ocrHint_t h;
        ocrEdtCreate(&next, tml, EDT_PARAM_DEF, (u64 *)&np, EDT_PARAM_DEF,
                     NULL, EDT_PROP_NONE, bandEdtHint(&h, bi, L + 1), NULL);
        ocrAddDependence(NULL_GUID, next, 0, DB_MODE_NULL);
        ocrEdtTemplateDestroy(tml);
    }
    return NULL_GUID;
}

/* Same per-tile deterministic fill as the tiled original, so both programs
 * align the same strings. */
static ocrGuid_t randInitEdt(u32 paramc, u64 *paramv, u32 depc,
                             ocrEdtDep_t depv[])
{
    (void)paramc;
    (void)depc;
    int *Ti = (int *)depv[0].ptr;
    int len = (int)paramv[0];
    int base = (int)paramv[1];
    unsigned int seed = (unsigned int)paramv[2] * 2654435761u + 12345u;
    if (len == base + 1) {
        Ti[0] = 32;
        Ti++;
        len--;
    }
    for (int l = 0; l < len; l++)
        Ti[l] = rand_r(&seed) % 4 + 'A';
    return NULL_GUID;
}

/* depv[0] = the final corner strip (one cell). */
static ocrGuid_t wrapupEdt(u32 paramc, u64 *paramv, u32 depc,
                           ocrEdtDep_t depv[])
{
    (void)paramc;
    (void)depc;
    const int *corner = (const int *)depv[0].ptr;

    long end = cilk_getticks();
    ocrPrintf("runtime: %f\n", cilk_ticks_to_seconds(end - (long)paramv[0]));
    ocrPrintf("LCS length: %d\n", corner[0]);
    ocrDbDestroy(depv[0].guid);
    return NULL_GUID;
}

/* Shutdown in its own task, gated on the wrapup's completion, so the
 * measured run never truncates the wrapup's release work. */
static ocrGuid_t shutdownEdt(u32 paramc, u64 *paramv, u32 depc,
                             ocrEdtDep_t depv[])
{
    (void)paramc;
    (void)paramv;
    (void)depc;
    (void)depv;
    ocrPrintf("\nShutting down OCR runtime\n");
    ocrShutdown();
    return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    (void)paramc;
    (void)depc;
    void *cmd = depv[0].ptr;
    u64 argc = ocrGetArgc(cmd);

    u64 N = 1024, base = 256;
    if (argc > 1)
        N = (u64)atol(ocrGetArgv(cmd, 1));
    if (argc > 2)
        base = (u64)atol(ocrGetArgv(cmd, 2));
    if (base > N)
        base = N;
    if (N % base != 0) {
        ocrPrintf("N (%lu) must be a multiple of base (%lu)\n", N, base);
        ocrShutdown();
        return NULL_GUID;
    }
    u64 L = N / base;
    ocrPrintf("\nRunning LCS (wavefront).\nStrings len: %lu  basecase: %lu\n",
              N, base);

    u64 nranks_mult;
    ocrAffinityCount(AFFINITY_PD, &nranks_mult);

    wf_params_t p;
    p.N = N;
    p.base = base;
    p.bi = 1;
    p.bj = 0;
    ocrGuidRangeCreate(&p.s_labels, L * nranks_mult, GUID_USER_DB);
    ocrGuidRangeCreate(&p.t_labels, L, GUID_USER_DB);
    u64 evt_range = (L + 1) * (L + 1) * nranks_mult;
    ocrGuidRangeCreate(&p.rc_labels, evt_range, GUID_USER_EVENT_STICKY);
    ocrGuidRangeCreate(&p.br_labels, evt_range, GUID_USER_EVENT_STICKY);
    ocrGuidRangeCreate(&p.co_labels, evt_range, GUID_USER_EVENT_STICKY);

    /* String tiles: created idle at their homes (the creator never touches
     * them — each is filled by its own init task through an RW dependence),
     * S band-steered like the leaves that read it, T left round-robin (a
     * column's readers span every band). */
    /* The wavefront must not start before the strings exist: a leaf's RO
     * read of a tile still being written is unordered.  Each init task gets
     * an app-supplied COUNTED(1) completion event, and the first row
     * spawner waits on all 2L of them. */
    ocrGuid_t *initDone =
        (ocrGuid_t *)malloc(sizeof(ocrGuid_t) * 2 * L);
    ocrGuid_t randTml;
    ocrEdtTemplateCreate(&randTml, randInitEdt, 3, 1);
    for (u64 i = 0; i < L; i++) {
        ocrGuid_t sTile, tTile;
        void *unused;
        ocrGuidFromIndex(&sTile, p.s_labels, bandIdx(i, i, L + 1));
        ocrGuidFromIndex(&tTile, p.t_labels, i);
        u64 sz = (i == 0) ? base + 1 : base;
        ocrDbCreate(&sTile, &unused, sizeof(int) * sz,
                    GUID_PROP_IS_LABELED | DB_PROP_NO_ACQUIRE, NULL_HINT,
                    NO_ALLOC);
        ocrDbCreate(&tTile, &unused, sizeof(int) * sz,
                    GUID_PROP_IS_LABELED | DB_PROP_NO_ACQUIRE, NULL_HINT,
                    NO_ALLOC);
        u64 sp[3] = {sz, base, L + i};
        u64 tp[3] = {sz, base, i};
        ocrEventParams_t cp;
        cp.EVENT_COUNTED.nbDeps = 1;
        ocrGuid_t e;
        ocrEventCreateParams(&initDone[2 * i], OCR_EVENT_COUNTED_T,
                             EVT_PROP_NONE, &cp);
        ocrEdtCreate(&e, randTml, EDT_PARAM_DEF, sp, EDT_PARAM_DEF, NULL,
                     EDT_PROP_OEVT_VALID, NULL_HINT, &initDone[2 * i]);
        ocrAddDependence(sTile, e, 0, DB_MODE_RW);
        ocrEventCreateParams(&initDone[2 * i + 1], OCR_EVENT_COUNTED_T,
                             EVT_PROP_NONE, &cp);
        ocrEdtCreate(&e, randTml, EDT_PARAM_DEF, tp, EDT_PARAM_DEF, NULL,
                     EDT_PROP_OEVT_VALID, NULL_HINT, &initDone[2 * i + 1]);
        ocrAddDependence(tTile, e, 0, DB_MODE_RW);
    }
    ocrEdtTemplateDestroy(randTml);

    long start = cilk_getticks();

    ocrGuid_t wrapTml, wrap, wrapOut;
    ocrEdtTemplateCreate(&wrapTml, wrapupEdt, 1, 1);
    u64 wp[1] = {(u64)start};
    ocrEdtCreate(&wrap, wrapTml, EDT_PARAM_DEF, wp, EDT_PARAM_DEF, NULL,
                 EDT_PROP_NONE, NULL_HINT, &wrapOut);
    ocrEdtTemplateDestroy(wrapTml);
    ocrAddDependence(stripEvt(p.co_labels, L, L, L), wrap, 0, DB_MODE_RO);

    ocrGuid_t shutTml, shut;
    ocrEdtTemplateCreate(&shutTml, shutdownEdt, 0, 1);
    ocrEdtCreate(&shut, shutTml, EDT_PARAM_DEF, NULL, EDT_PARAM_DEF, NULL,
                 EDT_PROP_NONE, NULL_HINT, NULL);
    ocrEdtTemplateDestroy(shutTml);
    ocrAddDependence(wrapOut, shut, 0, DB_MODE_NULL);

    ocrGuid_t spTml, sp0;
    ocrEdtTemplateCreate(&spTml, rowSpawnerEdt,
                         sizeof(wf_params_t) / sizeof(u64), (u32)(2 * L));
    ocrHint_t h;
    ocrEdtCreate(&sp0, spTml, EDT_PARAM_DEF, (u64 *)&p, EDT_PARAM_DEF, NULL,
                 EDT_PROP_NONE, bandEdtHint(&h, 0, L + 1), NULL);
    ocrEdtTemplateDestroy(spTml);
    for (u64 k = 0; k < 2 * L; k++)
        ocrAddDependence(initDone[k], sp0, (u32)k, DB_MODE_NULL);
    free(initDone);

    return NULL_GUID;
}
