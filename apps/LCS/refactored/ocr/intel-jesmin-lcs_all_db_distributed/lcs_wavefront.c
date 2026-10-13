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
 *     labeled STICKY event.  Every event has exactly one creator, a task
 *     that precedes the strip's producer and the task that registers its
 *     consumer in the dependence order (see createLeaf), and is destroyed by
 *     its single consumer, so a label lives exactly one generation.  Full
 *     score tiles are never materialized: the leaf computes with two rolling
 *     rows.
 *   - Each leaf is created by its north neighbour (row 1 by a seeding task)
 *     and becomes runnable exactly when its three input strips exist —
 *     anti-diagonal wavefront parallelism of width up to N/base, not the
 *     source recursion's quadrant ordering.
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
    ocrGuid_t s_labels, t_labels, rc_labels, br_labels, co_labels, leaf_tml;
} wf_params_t;

/* Row-band placement, shared with the tiled original: block-row bi's work
 * and data live on band((bi-1)) of the rank grid, so a leaf's west strip is
 * always band-local and its north strip crosses ranks only at band
 * boundaries.  Callers pass the grid side while only rows 0..rows-2 carry
 * tiles, so the partition is taken over those: every rank gets floor or ceil
 * of that count and none is left without work. */
static u64 bandOf(u64 row, u64 rows)
{
    u64 nranks;
    ocrAffinityCount(AFFINITY_PD, &nranks);
    u64 used = (rows > 1) ? rows - 1 : 1;
    if (row >= used)
        row = used - 1;
    u64 band = (row * nranks) / used;
    return (band < nranks) ? band : nranks - 1;
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

/* The name of the event that carries the strip emitted by tile (bi, bj). */
static ocrGuid_t stripName(ocrGuid_t range, u64 bi, u64 bj, u64 L)
{
    ocrGuid_t g;
    ocrGuidFromIndex(&g, range, tileIdx(bi, bj, L));
    return g;
}

/* The event itself, created exactly once.  A reserved name is not an
 * object: a dependence may only be added to, and a strip only handed to, an
 * event that exists.  The creator is therefore a task that precedes both
 * parties: the task that creates the emitting leaf (createLeaf), or mainEdt
 * for the final corner strip.  The consumer, the only party that knows the
 * strip has been delivered, destroys it. */
static ocrGuid_t createStripEvt(ocrGuid_t range, u64 bi, u64 bj, u64 L)
{
    ocrGuid_t g = stripName(range, bi, bj, L);
    u8 rc = ocrEventCreate(&g, OCR_EVENT_STICKY_T,
                           GUID_PROP_IS_LABELED | EVT_PROP_TAKES_ARG);
    if (rc != 0) {
        ocrPrintf("strip event (%lu,%lu) could not be created: %u\n",
                  (unsigned long)bi, (unsigned long)bj, (unsigned)rc);
        ocrAbort(rc);
    }
    return g;
}

/* Emit one strip: its payload block, handed through the strip's event. */
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

    ocrEventSatisfy(stripName(range, bi, bj, L), db);
}

/* One leaf of the wavefront, wired to the three strips it consumes.  Every
 * leaf is created by its north neighbour (row 1 by the seeding task), so the
 * creation plane is as wide as the wavefront and never runs more than one row
 * ahead of it.
 *
 * The creator also creates the events the leaf will emit on.  That makes it
 * the one creator of each strip event, and it precedes every use: the leaf
 * satisfies them only after it was created; the east consumer (bi, bj+1) is
 * created by (bi-1, bj+1), which runs only after this creator emitted its own
 * rc strip, and a leaf creates its child before emitting (on row 1 the seeding
 * task creates both leaves, in order); the south and southeast consumers are
 * created by the leaf itself or by its east neighbour, both later still.  The
 * final corner event is the one exception: mainEdt creates it, since it also
 * registers its consumer.  The events are created before the leaf's
 * dependences are added, because the last dependence can make the leaf
 * runnable at once. */
static void createLeaf(const wf_params_t *p, u64 bi, u64 bj)
{
    u64 L = p->N / p->base;
    wf_params_t lp = *p;
    lp.bi = bi;
    lp.bj = bj;

    if (bj < L)
        createStripEvt(p->rc_labels, bi, bj, L);
    if (bi < L)
        createStripEvt(p->br_labels, bi, bj, L);
    if (bi < L && bj < L)
        createStripEvt(p->co_labels, bi, bj, L);

    ocrHint_t h;
    ocrGuid_t leaf;
    ocrEdtCreate(&leaf, p->leaf_tml, EDT_PARAM_DEF, (u64 *)&lp, EDT_PARAM_DEF,
                 NULL, EDT_PROP_NONE, bandEdtHint(&h, bi - 1, L + 1), NULL);

    ocrGuid_t sTile, tTile;
    ocrGuidFromIndex(&sTile, p->s_labels, bandIdx(bi - 1, bi - 1, L + 1));
    ocrGuidFromIndex(&tTile, p->t_labels, bj - 1);
    ocrAddDependence(sTile, leaf, 0, DB_MODE_RO);
    ocrAddDependence(tTile, leaf, 1, DB_MODE_RO);
    ocrAddDependence(bj > 1 ? stripName(p->rc_labels, bi, bj - 1, L) : NULL_GUID,
                     leaf, 2, DB_MODE_RO);
    ocrAddDependence(bi > 1 ? stripName(p->br_labels, bi - 1, bj, L) : NULL_GUID,
                     leaf, 3, DB_MODE_RO);
    ocrAddDependence((bi > 1 && bj > 1)
                         ? stripName(p->co_labels, bi - 1, bj - 1, L)
                         : NULL_GUID,
                     leaf, 4, DB_MODE_RO);
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

    /* The last tile is the program's result: its cells are digested the same
     * way the tiled original digests the equivalent block, so the two rows
     * print the same checksum for the same arguments. */
    int last = (bi == L && bj == L);
    u64 h = 1469598103934665603ULL;

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
        if (last) {
            for (u64 c = 0; c < n; c++) {
                h ^= (u64)(u32)cur[c];
                h *= 1099511628211ULL;
            }
        }
        rc[r] = cur[n - 1];
        int *tmp = prev;
        prev = cur;
        cur = tmp;
    }
    /* prev now holds the tile's bottom row; rc its right column. */

    /* The south leaf, and the events it will emit on, exist before this
     * leaf's rc strip is handed over: the task that strip wakes creates the
     * southeast leaf, which registers on the south leaf's rc event. */
    if (bi < L)
        createLeaf(p, bi + 1, bj);

    if (bj < L)
        emitStrip(p->rc_labels, bi, bj, L, rc, n);
    if (bi < L)
        emitStrip(p->br_labels, bi, bj, L, prev, n);
    if (last) {
        int fin[2];
        fin[0] = prev[n - 1];
        fin[1] = (int)(h & 0x7fffffffULL);
        emitStrip(p->co_labels, bi, bj, L, fin, 2);
    }
    else if (bi < L && bj < L) {
        int c0 = prev[n - 1];
        emitStrip(p->co_labels, bi, bj, L, &c0, 1);
    }

    free(scratch);

    /* Each consumed strip, and the event that carried it, had exactly this
     * leaf as its consumer. */
    if (depv[2].ptr != NULL) {
        ocrDbDestroy(depv[2].guid);
        ocrEventDestroy(stripName(p->rc_labels, bi, bj - 1, L));
    }
    if (depv[3].ptr != NULL) {
        ocrDbDestroy(depv[3].guid);
        ocrEventDestroy(stripName(p->br_labels, bi - 1, bj, L));
    }
    if (depv[4].ptr != NULL) {
        ocrDbDestroy(depv[4].guid);
        ocrEventDestroy(stripName(p->co_labels, bi - 1, bj - 1, L));
    }

    return NULL_GUID;
}

/* Seeds the wavefront's first row once the strings exist.  Every later row
 * is created by the row above it, so this is the only bulk creation task and
 * the creation plane is L-wide from row 2 on. */
static ocrGuid_t rowSeedEdt(u32 paramc, u64 *paramv, u32 depc,
                            ocrEdtDep_t depv[])
{
    (void)paramc;
    (void)depc;
    (void)depv;
    wf_params_t *p = (wf_params_t *)paramv;
    u64 L = p->N / p->base;

    for (u64 bj = 1; bj <= L; bj++)
        createLeaf(p, 1, bj);

    return NULL_GUID;
}

/* Character of the input strings: a pure function of (tile index, position)
 * built from a self-contained mixer, so the instance is a property of the
 * program alone -- no shared generator state, no library generator whose
 * sequence is defined by the host's C library, and the same characters at
 * every node count and in every tier. */
static int lcsChar(u64 tile, u64 pos)
{
    u64 z = tile * 0x9E3779B97F4A7C15ULL + pos * 0xBF58476D1CE4E5B9ULL
          + 0x165667B19E3779F9ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return (int)(z % 4) + 'A';
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
    u64 tile = (u64)paramv[2];
    if (len == base + 1) {
        Ti[0] = 32;
        Ti++;
        len--;
    }
    for (int l = 0; l < len; l++)
        Ti[l] = lcsChar(tile, (u64)l);
    return NULL_GUID;
}

/* depv[0] = the final strip: the answer cell and the result digest.
 * paramv[1] = the event that carried it, consumed here. */
static ocrGuid_t wrapupEdt(u32 paramc, u64 *paramv, u32 depc,
                           ocrEdtDep_t depv[])
{
    (void)paramc;
    (void)depc;
    const int *corner = (const int *)depv[0].ptr;

    long end = cilk_getticks();
    ocrPrintf("runtime: %f\n", cilk_ticks_to_seconds(end - (long)paramv[0]));
    ocrPrintf("LCS length: %d\n", corner[0]);
    ocrPrintf("LCS checksum: %lu\n", (unsigned long)(u32)corner[1]);
    ocrDbDestroy(depv[0].guid);
    ocrEventDestroy((ocrGuid_t){ .guid = paramv[1] });
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
    if (argc != 3) {
        ocrPrintf("USAGE: please run the program as follows:\n"
                  "executable input_string_length basecase\n");
        ocrShutdown();
        return NULL_GUID;
    }
    N = (u64)atol(ocrGetArgv(cmd, 1));
    base = (u64)atol(ocrGetArgv(cmd, 2));
    if (N > 0 && base > N)
        base = N;
    if (N == 0 || base == 0 || N % base != 0) {
        ocrPrintf("input_string_length (%lu) must be a multiple of basecase (%lu)\n",
                  N, base);
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
    /* One template for every leaf: leaves outlive their creator, so it is
     * created once here and never destroyed. */
    ocrEdtTemplateCreate(&p.leaf_tml, leafEdt,
                         sizeof(wf_params_t) / sizeof(u64), 5);
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
     * an app-supplied COUNTED(1) completion event, and the row that seeds
     * the wavefront waits on all 2L of them. */
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
    ocrGuid_t finEvt = createStripEvt(p.co_labels, L, L, L);
    ocrEdtTemplateCreate(&wrapTml, wrapupEdt, 2, 1);
    u64 wp[2] = {(u64)start, (u64)finEvt.guid};
    ocrEdtCreate(&wrap, wrapTml, EDT_PARAM_DEF, wp, EDT_PARAM_DEF, NULL,
                 EDT_PROP_NONE, NULL_HINT, &wrapOut);
    ocrEdtTemplateDestroy(wrapTml);
    ocrAddDependence(finEvt, wrap, 0, DB_MODE_RO);

    ocrGuid_t shutTml, shut;
    ocrEdtTemplateCreate(&shutTml, shutdownEdt, 0, 1);
    ocrEdtCreate(&shut, shutTml, EDT_PARAM_DEF, NULL, EDT_PARAM_DEF, NULL,
                 EDT_PROP_NONE, NULL_HINT, NULL);
    ocrEdtTemplateDestroy(shutTml);
    ocrAddDependence(wrapOut, shut, 0, DB_MODE_NULL);

    ocrGuid_t spTml, sp0;
    ocrEdtTemplateCreate(&spTml, rowSeedEdt,
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
