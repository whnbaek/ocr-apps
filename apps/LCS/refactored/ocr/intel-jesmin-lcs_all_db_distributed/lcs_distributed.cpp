
/*
Author Jesmin Jahan Tithi
Copyright Intel Corporation 2016
*/
//export OCR_ASAN=yes
#ifndef ENABLE_EXTENSION_LABELING
#define ENABLE_EXTENSION_LABELING
#endif

#include "ocr.h"
#include "extensions/ocr-labeling.h" //currently needed for labeled guids
#include "extensions/ocr-affinity.h" //needed for affinity

#ifdef USE_PROFILER
#include "extensions/ocr-profiler.h"
#endif
#include <stdio.h>
#include "string.h"
#include <stdlib.h>
#include <math.h>
#include <assert.h>

// User defined libraries
#include "cilktime.h" // timing library

//#define PRINT

#define MAX(a, b) ((a > b) ? a : b)
#define MIN(a, b) ((a < b) ? a : b)
#define GAP_PENALTY 0
//
typedef struct {
    int N, base, xi, xj, n; // input size, top-left corner index, and size of the current
    ocrGuid_t s_labels, t_labels, score_matrix_labels; // s_label, t_label, score_matrix_labels
} LCS_task_params;

typedef struct {
    int N, xi, xj, n; // input and base case size, top-left corner index
} LCS_base_params;

/* Content-sensitive digest of a computed score tile: an FNV-1a walk over the
 * cells in memory order, folded to 31 bits so the value is exact in every
 * consumer's numeric type.  Every cell contributes, so a wrong string, a
 * dropped task or a lost write changes it. */
static u64 lcsChecksum(const int* v, long n)
{
    u64 h = 1469598103934665603ULL;
    for (long k = 0; k < n; k++) {
        h ^= (u64)(u32)v[k];
        h *= 1099511628211ULL;
    }
    return h & 0x7fffffffULL;
}

//base case kernel
void serial_lcs_1D(int* current, int* left, int* above, int* diag,
    int* S, int* T, int N, int xi, int xj, int n)
{
    int my_i_size = (xi + n > N) ? (N + 1 - xi) : n;
    int my_j_size = (xj + n > N) ? (N + 1 - xj) : n;

    int neighbor_i_size = (xi == 1) ? 1 : n;
    int neighbor_j_size = (xj == 1) ? 1 : n;

#ifdef PRINT
    ocrPrintf("Calling seq basecase: xi: %d, xj: %d, n: %d \n", xi, xj, n);
#endif
    //copy left to a seperate location.

    //CASE 1: i=0, j=0;

    int match = diag[(neighbor_i_size - 1) * neighbor_j_size
                    + (neighbor_j_size - 1)]
        + (S[0] != T[0]);
    int del = left[(neighbor_j_size - 1)];
    int ins = above[(neighbor_i_size - 1) * n];
    current[0] = MIN(match, (MAX(del, ins) + GAP_PENALTY));

    //CASE 2: i=0, j>0; both diagonal and above will come from above neigh
    int i = 0, j = 0;

    int neighbor_row = (neighbor_i_size - 1) * n;

    for (j = 1; j < my_j_size; j++) {
        int ins = above[neighbor_row + j];
        int match = above[neighbor_row + (j - 1)] + (S[0] != T[j]);
        int del = current[(j - 1)];
        current[j] = MIN(match, (MAX(del, ins) + GAP_PENALTY));
    }

    //CASE 3: i>0, j=0; both diagonal and left will come from left neigh
    j = 0;
    for (i = 1; i < my_i_size; i++) {
        int match = left[(i - 1) * neighbor_j_size + (neighbor_j_size - 1)] + (S[i] != T[0]);
        int del = left[(i)*neighbor_j_size + (neighbor_j_size - 1)];
        int ins = current[(i - 1) * my_j_size];
        current[i * my_j_size] = MIN(match, (MAX(del, ins) + GAP_PENALTY));

        //neighbor_col = neighbor_col+ neighbor_j_size;			//next row col
    }

    //CASE 4: i>0, j>0;
    for (i = 1; i < my_i_size; i++) {
        for (j = 1; j < my_j_size; j++) {

            int match = current[(i - 1) * my_j_size + (j - 1)] + (S[i] != T[j]);
            int ins = current[(i - 1) * my_j_size + j];
            int del = current[(i)*my_j_size + (j - 1)];
            current[i * my_j_size + (j)] = MIN(match, (MAX(del, ins) + GAP_PENALTY));
        }
    }
#ifdef PRINT
    for (i = 0; i < my_i_size; i++) {
        for (j = 0; j < my_j_size; j++) {
            ocrPrintf("%d ", current[i * my_j_size + (j)]);
        }
        ocrPrintf("\n");
    }
#endif
    return;
}

// OCR libraries
ocrGuid_t seqLCSEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    //Take the passcoresed arg and convert it back to the original type
    LCS_base_params* p = (LCS_base_params*)paramv;

    int* S = (int*)depv[0].ptr;
    int* T = (int*)depv[1].ptr;

    S = (p->xi == 1) ? S + 1 : S;
    T = (p->xj == 1) ? T + 1 : T;

    int* current = (int*)depv[2].ptr;
    int* left = (int*)depv[3].ptr;
    int* above = (int*)depv[4].ptr;
    int* diag = (int*)depv[5].ptr;

    serial_lcs_1D(current, left, above, diag, S, T, p->N, p->xi, p->xj, p->n);

    /* A score block is read by the block to its east (as `left`), the one to
     * its south (`above`), and the one to its south-east (`diag`).  The third
     * depends on the first two, so a block reaching THIS point as `diag` has
     * no reader left anywhere: the other two completed before this task could
     * be scheduled, and nothing else ever names it.  Each block is the diag of
     * exactly one successor, so the destroy is single-flight by construction.
     *
     * The last row and last column are nobody's diag and so are never freed —
     * which is correct: the program's answer is the bottom-right cell, read at
     * shutdown, and there is no traceback that would need the interior.  What
     * this leaves resident is the wavefront frontier instead of the whole
     * matrix. */
    ocrDbDestroy(depv[5].guid);

    return NULL_GUID;
}
// Contiguous row-band placement for wavefront leaves: the block rows in play
// are split into one contiguous band per rank, so every rank owns a band and
// the bands differ in height by at most one row.  A block's West neighbor
// shares its row and therefore its rank; North/NW share its band for every
// row except the nranks-1 band-boundary rows, so the heavy north-south
// payload stays rank-local almost everywhere.  The anti-diagonal frontier
// still spreads: once it is at least one band tall it has blocks in every
// band.  A striped map (row % nranks) spreads the frontier sooner but makes
// EVERY north neighbor remote, shipping each block's payload across ranks
// once per south-facing read -- bandwidth, not spread, is what limits the
// wavefront.
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
/* Band of a block row.  Callers pass the grid side (L+1) while only rows
 * 0..L-1 carry leaves and interior blocks, so the partition is taken over
 * those L rows: every rank gets floor or ceil of L/nranks of them and none
 * is left without work. */
static u64 bandOfRow(u64 row, int gridSide)
{
    u64 nranks;
    ocrAffinityCount(AFFINITY_PD, &nranks);
    u64 rows = (gridSide > 1) ? (u64)(gridSide - 1) : 1;
    if (row >= rows)
        row = rows - 1;
    u64 band = (row * nranks) / rows;
    return (band < nranks) ? band : nranks - 1;
}

static ocrHint_t * rankHint(ocrHint_t * h, u64 rank)
{
    ocrGuid_t aff;
    ocrAffinityGetAt(AFFINITY_PD, rank, &aff);
    ocrHintInit(h, OCR_HINT_EDT_T);
    ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
}
#endif

/* Labeled-range index for a score block (row-major linear index t over the
 * block grid).  The runtimes distribute a labeled range's homes round-robin
 * by index (home = index mod nranks), so under the optimized placement the
 * index is re-encoded as band(row) + nranks*t: the low residue steers each
 * block's home onto its wavefront band's rank while distinct t stay distinct
 * (label uniqueness) -- pure index arithmetic, agreed on by every producer
 * and consumer of the range. */
static u64 scoreIdx(int t, int gridSide)
{
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
    u64 nranks;
    ocrAffinityCount(AFFINITY_PD, &nranks);
    int bi = t / gridSide;
    u64 row = (bi > 0) ? (u64)(bi - 1) : 0;
    return bandOfRow(row, gridSide) + nranks * (u64)t;
#else
    (void)gridSide;
    return (u64)t;
#endif
}

/* Labeled-range index for an S string tile.  Row i's tile is read only by
 * the leaves of block-row i+1, whose band rank blockRowHint derives from the
 * same row value -- steering the tile's home there makes every S read
 * band-local.  Same re-encoding trick as scoreIdx: low residue = home,
 * distinct rows stay distinct. */
static u64 sIdx(int row, int blockRows)
{
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
    u64 nranks;
    ocrAffinityCount(AFFINITY_PD, &nranks);
    return bandOfRow((u64)row, blockRows) + nranks * (u64)row;
#else
    (void)blockRows;
    return (u64)row;
#endif
}

static ocrHint_t * blockRowHint(ocrHint_t * h, int blockRow, int blockRows)
{
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
    u64 row = (blockRow > 0) ? (u64)(blockRow - 1) : 0;
    return rankHint(h, bandOfRow(row, blockRows));
#else
    (void)h; (void)blockRow; (void)blockRows;
    return NULL_HINT;
#endif
}

/* A T string tile is read by every band, so its home stays the range's own
 * round-robin (index j); the task that fills it is placed there. */
static ocrHint_t * tileRankHint(ocrHint_t * h, int index)
{
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
    u64 nranks;
    ocrAffinityCount(AFFINITY_PD, &nranks);
    return rankHint(h, (u64)index % nranks);
#else
    (void)h; (void)index;
    return NULL_HINT;
#endif
}
ocrGuid_t recLCSEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{

    // Cast the parameter to the original parameter format.
    LCS_task_params* p = (LCS_task_params*)paramv;

    int base = p->base;
    int n = p->n;

    if (n <= base) {
        //Setup data dependency

        LCS_base_params p1;
        p1.N = p->N;
        p1.xi = p->xi;
        p1.xj = p->xj;
        p1.n = n;

        int block_i_end = (p->xi - 1) / base + 1;
        int block_j_end = (p->xj - 1) / base + 1;
        int block_grid_size = (p->N / base + 1);
        int current_block = (block_i_end * block_grid_size + block_j_end);

        // Create the task template
        ocrGuid_t base_template;
        ocrEdtTemplateCreate(&base_template, seqLCSEdt, sizeof(LCS_base_params) / sizeof(u64), 6);

        ocrGuid_t baseEdt, baseoutputEventGuidOnce;
        ocrEventCreate(&baseEdt, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);

        ocrHint_t baseHint;

        ocrEdtCreate(&baseEdt,
            base_template,
            EDT_PARAM_DEF,
            (u64*)&p1,
            EDT_PARAM_DEF,
            NULL,
            EDT_PROP_NONE,
            blockRowHint(&baseHint, block_i_end, block_grid_size),
            &baseoutputEventGuidOnce);

        ocrGuid_t S, T;

        // Optimization effort.

        ocrGuidFromIndex(&S, p->s_labels, sIdx((int)((p->xi - 1) / base), block_grid_size));
        ocrGuidFromIndex(&T, p->t_labels, (p->xj - 1) / base);

        ocrAddDependence(S, baseEdt, 0, DB_MODE_RO);
        ocrAddDependence(T, baseEdt, 1, DB_MODE_RO);

        int left_block = (block_i_end * block_grid_size + block_j_end - 1);
        int above_block = ((block_i_end - 1) * block_grid_size + block_j_end);
        int diag_block = ((block_i_end - 1) * block_grid_size + block_j_end - 1);

        ocrGuid_t current, left, above, diag;
        ocrGuidFromIndex(&current, p->score_matrix_labels, scoreIdx(current_block, block_grid_size));
        ocrAddDependence(current, baseEdt, 2, DB_MODE_RW);

        ocrGuidFromIndex(&left, p->score_matrix_labels, scoreIdx(left_block, block_grid_size));
        ocrAddDependence(left, baseEdt, 3, DB_MODE_RO);

        ocrGuidFromIndex(&above, p->score_matrix_labels, scoreIdx(above_block, block_grid_size));
        ocrAddDependence(above, baseEdt, 4, DB_MODE_RO);

        ocrGuidFromIndex(&diag, p->score_matrix_labels, scoreIdx(diag_block, block_grid_size));
        ocrAddDependence(diag, baseEdt, 5, DB_MODE_RO);

        return NULL_GUID;
    }
    else {

        int nn = n >> 1;

        //ocrPrintf("Working on size: %lu\n", nn);

        /* A quadrant creates the leaves of its own block rows, so placing it
         * on the band of its first row keeps those creates and their
         * dependence registrations rank-local. */
        int block_grid_size = (p->N / base + 1);
        ocrHint_t quadHint;

        //--Call for x11-------------------------------------------
        LCS_task_params p1;

        p1.N = p->N;
        p1.base = base;
        p1.xi = p->xi;
        p1.xj = p->xj;
        p1.n = nn;
        p1.s_labels = p->s_labels;
        p1.t_labels = p->t_labels;
        p1.score_matrix_labels = p->score_matrix_labels;

        //ocrPrintf("Calling rec case N: %lu, xi: %lu, xj: %lu, n: %lu \n", p->N, p->xi, p->xj, p->n);
        // Create the task template
        ocrGuid_t LCSGuidTmp_X11;
        ocrEdtTemplateCreate(&LCSGuidTmp_X11, recLCSEdt, sizeof(p1) / sizeof(u64), 1);

        // Create the task
        ocrGuid_t x11Edt, x11outputEventGuid, x11outputEventGuidOnce;
        ocrEventCreate(&x11outputEventGuid, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);

        ocrEdtCreate(&x11Edt,
            LCSGuidTmp_X11,
            EDT_PARAM_DEF,
            (u64*)&p1,
            EDT_PARAM_DEF,
            NULL,
            EDT_PROP_FINISH,
            blockRowHint(&quadHint, (p1.xi - 1) / base + 1, block_grid_size),
            &x11outputEventGuidOnce);
        ocrAddDependence(x11outputEventGuidOnce, x11outputEventGuid, 0, DB_MODE_NULL);
        ocrAddDependence(NULL_GUID, x11Edt, 0, DB_MODE_NULL);

        //---Call for x12-------------------------------------------
        ocrGuid_t LCSGuidTmp_X12_x21;
        ocrEdtTemplateCreate(&LCSGuidTmp_X12_x21, recLCSEdt, sizeof(p1) / sizeof(u64), 1);

        ocrGuid_t x12Edt, x12outputEventGuid, x12outputEventGuidOnce;
        ocrEventCreate(&x12outputEventGuid, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);

        p1.xi = p->xi + nn, p1.xj = p->xj;

        /* A task's output event is chained to its consumer before the task
         * can become runnable: a once event that fires with no waiter is
         * gone.  The input is therefore added last. */
        ocrEdtCreate(&x12Edt,
            LCSGuidTmp_X12_x21,
            EDT_PARAM_DEF,
            (u64*)&p1,
            EDT_PARAM_DEF,
            NULL,
            EDT_PROP_FINISH,
            blockRowHint(&quadHint, (p1.xi - 1) / base + 1, block_grid_size),
            &x12outputEventGuidOnce);

        ocrAddDependence(x12outputEventGuidOnce, x12outputEventGuid, 0, DB_MODE_NULL);
        ocrAddDependence(x11outputEventGuid, x12Edt, 0, DB_DEFAULT_MODE);

        //---------------------------Call for x21-------------------------------------------
        ocrGuid_t x21Edt, x21outputEventGuid, x21outputEventGuidOnce;
        ocrEventCreate(&x21outputEventGuid, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);

        p1.xi = p->xi, p1.xj = p->xj + nn;

        ocrEdtCreate(&x21Edt,
            LCSGuidTmp_X12_x21,
            EDT_PARAM_DEF,
            (u64*)&p1,
            EDT_PARAM_DEF,
            NULL,
            EDT_PROP_FINISH,
            blockRowHint(&quadHint, (p1.xi - 1) / base + 1, block_grid_size),
            &x21outputEventGuidOnce);

        ocrAddDependence(x21outputEventGuidOnce, x21outputEventGuid, 0, DB_MODE_NULL);
        ocrAddDependence(x11outputEventGuid, x21Edt, 0, DB_DEFAULT_MODE);

        //---------------------------Call for x22-------------------------------------------

        ocrGuid_t LCSGuidTmp_X22;
        ocrEdtTemplateCreate(&LCSGuidTmp_X22, recLCSEdt, sizeof(p1) / sizeof(u64), 2);

        ocrGuid_t x22Edt, x22outputEventGuid;
        ocrEventCreate(&x22outputEventGuid, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);

        p1.xi = p->xi + nn, p1.xj = p->xj + nn;

        ocrEdtCreate(&x22Edt,
            LCSGuidTmp_X22,
            EDT_PARAM_DEF,
            (u64*)&p1,
            EDT_PARAM_DEF,
            NULL,
            EDT_PROP_FINISH,
            blockRowHint(&quadHint, (p1.xi - 1) / base + 1, block_grid_size),
            &x22outputEventGuid);

        ocrAddDependence(x21outputEventGuid, x22Edt, 0, DB_DEFAULT_MODE);
        ocrAddDependence(x12outputEventGuid, x22Edt, 1, DB_DEFAULT_MODE);
    }
    return NULL_GUID;
}

ocrGuid_t wrapupEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    int* score = (int*)depv[1].ptr;
    int len = (int)paramv[0];

    long end = cilk_getticks();
    ocrPrintf("runtime: %f\n", cilk_ticks_to_seconds(end - (long)paramv[1]));

    // Bottom-right cell of the last score tile is the final LCS length.
    ocrPrintf("LCS length: %d\n", score[len * len - 1]);
    ocrPrintf("LCS checksum: %lu\n",
        (unsigned long)lcsChecksum(score, (long)len * (long)len));

    return NULL_GUID;
}

/* Shutdown in its own task, gated on the wrapup's completion, so the measured
 * run never truncates the wrapup's release work. */
ocrGuid_t shutDownEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    ocrPrintf("\nShutting down OCR runtime\n");
    ocrShutdown();
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

ocrGuid_t randInitEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    int* Ti = (int*)depv[0].ptr;
    int len = (int)paramv[0];
    int base = (int)paramv[1];
    u64 tile = (u64)paramv[2];
    if (len == base + 1) {
        Ti[0] = 32;
        Ti++;
        len--; // the leading boundary slot consumed one of the len elements;
               // the loop below must fill only the remaining len-1
    }

    for (int l = 0; l < len; l++) {
        Ti[l] = lcsChar(tile, (u64)l);
    }
    return NULL_GUID;
}

ocrGuid_t scoreInitEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    int* score = (int*)depv[0].ptr;
    int len = (int)paramv[0];
    int start = (int)paramv[1];

    if (len == 1) {
        score[0] = start;
        //ocrPrintf("%d ", score[0]);
    }
    else {

        for (int l = 0; l < len; l++) {
            score[l] = start + l; // Initialize T
            //ocrPrintf("%d ", score[l]);
        }
    }
    return NULL_GUID;
}

typedef struct
    {
    ocrGuid_t s_labels, t_labels, score_matrix_labels;
    int base, num_labels;
} init_param;

ocrGuid_t InitEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    ocrGuid_t s_labels, t_labels, score_matrix_labels;

    int** S = (int**)depv[0].ptr;
    int** T = (int**)depv[1].ptr;
    int** lcs_score = (int**)depv[2].ptr;

    init_param* p = (init_param*)paramv;
    s_labels = p->s_labels;
    t_labels = p->t_labels;
    score_matrix_labels = p->score_matrix_labels;
    int base = p->base;
    int num_labels = p->num_labels;

    // Create the DBs for S, T and lcs_score matrix and initialize as needed.
    // Use DBcreate for these as well.

    int grid_block_size = num_labels + 1;
    int num_blocks = (grid_block_size) * (grid_block_size);

    /* Each fill task is placed on the home of the block it writes, so the
     * init phase writes locally instead of pulling every block to whichever
     * rank the round-robin picked. */
    ocrHint_t initHint;

    ////////////////////////Initialize T ///////////////////////

    //case0:
    ocrGuid_t T_ij;
    ocrGuidFromIndex(&T_ij, t_labels, 0);

    int block_size = base + 1;
    // Create the data block with the guid
    /* NO_ACQUIRE: the creator never touches any block it creates here --
     * each one is filled by its own init EDT through an RW dependence -- so
     * the home stays the sole idle owner, the create moves no payload, and
     * a block whose home is remote is born there instead of materializing
     * on the creating rank. */
    ocrDbCreate(&T_ij,
        (void**)&T[0],
        sizeof(int) * (block_size),
        GUID_PROP_IS_LABELED | DB_PROP_NO_ACQUIRE,
        NULL_HINT,
        NO_ALLOC);

    //create randInitEDT
    //Now initialize it
    //Create the task
    ocrGuid_t randInitTmp;
    ocrEdtTemplateCreate(&randInitTmp, randInitEdt, 3, 1);

    // string_param[2] is a per-tile seed index so each tile is filled
    // deterministically regardless of the concurrent init EDTs' run order.
    u64 string_param[3];
    string_param[0] = block_size;
    string_param[1] = base;
    string_param[2] = 0;            // T tile 0

    ocrGuid_t randInitTEdt;
    ocrEdtCreate(&randInitTEdt,
        randInitTmp,
        EDT_PARAM_DEF,
        (u64*)&string_param,
        EDT_PARAM_DEF,
        NULL,
        EDT_PROP_NONE,
        tileRankHint(&initHint, 0),
        NULL);
    ocrAddDependence(T_ij, randInitTEdt, 0, DB_MODE_RW);

    //case2: block size = base;
    block_size = base;

    for (int j = 1; j < num_labels; j++) {
        ocrGuid_t T_ij;
        // First get the already created guids
        ocrGuidFromIndex(&T_ij, t_labels, j);

        // Create the data block with the guid
        ocrDbCreate(&T_ij,
            (void**)&T[j],
            sizeof(int) * block_size,
            GUID_PROP_IS_LABELED | DB_PROP_NO_ACQUIRE,
            NULL_HINT,
            NO_ALLOC);
        // Now initialize it

        ocrGuid_t randInitTEdt;

        string_param[0] = block_size;
        string_param[2] = j;            // T tile j

        ocrEdtCreate(&randInitTEdt,
            randInitTmp,
            EDT_PARAM_DEF,
            (u64*)&string_param,
            EDT_PARAM_DEF,
            NULL,
            EDT_PROP_NONE,
            tileRankHint(&initHint, j),
            NULL);

        ocrAddDependence(T_ij, randInitTEdt, 0, DB_MODE_RW);
    }

    //ocrPrintf("\n S:");
    ////////////////////////////////////////////////////////

    //////////////////Initialize S/////////////////////////

    // First get the already created guids
    ocrGuid_t S_ij;
    ocrGuidFromIndex(&S_ij, s_labels, sIdx(0, grid_block_size));
    block_size = base + 1;
    // Create the data block with the guid and size
    ocrDbCreate(&S_ij,
        (void**)&S[0],
        sizeof(int) * (block_size),
        GUID_PROP_IS_LABELED | DB_PROP_NO_ACQUIRE,
        NULL_HINT,
        NO_ALLOC);
    // Now initialize it

    ocrGuid_t randInitSEdt;

    string_param[0] = block_size;
    string_param[2] = num_labels;   // S tile 0 (seed range distinct from T)

    ocrEdtCreate(&randInitSEdt,
        randInitTmp,
        EDT_PARAM_DEF,
        (u64*)&string_param,
        EDT_PARAM_DEF,
        NULL,
        EDT_PROP_NONE,
        blockRowHint(&initHint, 1, grid_block_size),
        NULL);

    ocrAddDependence(S_ij, randInitSEdt, 0, DB_MODE_RW);

    block_size = base;

    for (int i = 1; i < num_labels; i++) {
        ocrGuid_t S_ij;
        // First get the already created guids
        ocrGuidFromIndex(&S_ij, s_labels, sIdx(i, grid_block_size));

        // Create the data block with the guid and size
        ocrDbCreate(&S_ij,
            (void**)&S[i],
            sizeof(int) * block_size,
            GUID_PROP_IS_LABELED | DB_PROP_NO_ACQUIRE,
            NULL_HINT,
            NO_ALLOC);
        // Now initialize it

        string_param[0] = block_size;
        string_param[2] = num_labels + i;   // S tile i

        ocrGuid_t randInitSEdt;

        ocrEdtCreate(&randInitSEdt,
            randInitTmp,
            EDT_PARAM_DEF,
            (u64*)&string_param,
            EDT_PARAM_DEF,
            NULL,
            EDT_PROP_NONE,
            blockRowHint(&initHint, i + 1, grid_block_size),
            NULL);

        ocrAddDependence(S_ij, randInitSEdt, 0, DB_MODE_RW);
    }

    ////////////////////////Initialize score ///////////////////////
    int data_block_size = (base * base);

    //CASE1: i=0;j=0
    ocrGuid_t lcs_score_ij;
    block_size = 1;
    // First get the guid
    ocrGuidFromIndex(&lcs_score_ij, score_matrix_labels, scoreIdx(0, grid_block_size));
    // Create the data block with the guid
    ocrDbCreate(&lcs_score_ij,
        (void**)&lcs_score[0],
        sizeof(int) * 1,
        GUID_PROP_IS_LABELED | DB_PROP_NO_ACQUIRE,
        NULL_HINT,
        NO_ALLOC);

    //Create the task
    ocrGuid_t scoreInitTmp;
    ocrEdtTemplateCreate(&scoreInitTmp, scoreInitEdt, 2, 1);

    ocrGuid_t scoreEdt;

    u64 score_param[2];
    score_param[0] = block_size;
    score_param[1] = 0; //start
    ocrEdtCreate(&scoreEdt,
        scoreInitTmp,
        EDT_PARAM_DEF,
        (u64*)&score_param,
        EDT_PARAM_DEF,
        NULL,
        EDT_PROP_NONE,
        blockRowHint(&initHint, 0, grid_block_size),
        NULL);

    ocrAddDependence(lcs_score_ij, scoreEdt, 0, DB_MODE_RW);

    //CASE2: i = 0, j > 0
    int i = 0, j = 0;
    block_size = base;
    int t;
    for (j = 1; j < grid_block_size; j++) {
        t = j;
        // First get the guid
        ocrGuidFromIndex(&lcs_score_ij, score_matrix_labels, scoreIdx(t, grid_block_size));
        // Create the data block with the guid
        ocrDbCreate(&lcs_score_ij,
            (void**)&lcs_score[t],
            sizeof(int) * block_size,
            GUID_PROP_IS_LABELED | DB_PROP_NO_ACQUIRE,
            NULL_HINT,
            NO_ALLOC);

        ocrGuid_t scoreEdt;

        score_param[0] = block_size;
        score_param[1] = (j - 1) * base + 1;
        ocrEdtCreate(&scoreEdt,
            scoreInitTmp,
            EDT_PARAM_DEF,
            (u64*)&score_param,
            EDT_PARAM_DEF,
            NULL,
            EDT_PROP_NONE,
            blockRowHint(&initHint, 0, grid_block_size),
            NULL);

        ocrAddDependence(lcs_score_ij, scoreEdt, 0, DB_MODE_RW);
    }

    //CASE3: j-0, i>1
    j = 0;

    for (i = 1; i < grid_block_size; i++) {
        t = i * grid_block_size;
        ocrGuidFromIndex(&lcs_score_ij, score_matrix_labels, scoreIdx(t, grid_block_size));
        ocrDbCreate(&lcs_score_ij,
            (void**)&lcs_score[t],
            sizeof(int) * base,
            GUID_PROP_IS_LABELED | DB_PROP_NO_ACQUIRE,
            NULL_HINT,
            NO_ALLOC);

        ocrGuid_t scoreEdt;

        score_param[0] = block_size;
        score_param[1] = (i - 1) * base + 1;
        ocrEdtCreate(&scoreEdt,
            scoreInitTmp,
            EDT_PARAM_DEF,
            (u64*)&score_param,
            EDT_PARAM_DEF,
            NULL,
            EDT_PROP_NONE,
            blockRowHint(&initHint, i, grid_block_size),
            NULL);

        ocrAddDependence(lcs_score_ij, scoreEdt, 0, DB_MODE_RW);
    }

    //CASE4: i>0, j>0
    for (int i = 1; i < grid_block_size; i++) {
        for (int j = 1; j < grid_block_size; j++) {
            int t = (i * grid_block_size + j);

            ocrGuid_t lcs_score_ij;
            // First get the guid
            ocrGuidFromIndex(&lcs_score_ij, score_matrix_labels, scoreIdx(t, grid_block_size));

            // Create the data block with the guid
            ocrDbCreate(&lcs_score_ij,
                (void**)&lcs_score[t],
                sizeof(int) * data_block_size,
                GUID_PROP_IS_LABELED | DB_PROP_NO_ACQUIRE,
                NULL_HINT,
                NO_ALLOC);
        }
        // initialize data
    }
    return NULL_GUID;
}

//----------------------------------------------------- Main EDT --------------------------//
extern "C" ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    // Collect command line arguments.
    ocrGuid_t DBK_cmdLineArgs = depv[0].guid;
    void* PTR_cmdLineArgs = depv[0].ptr;
    u32 argc = ocrGetArgc(PTR_cmdLineArgs);

    // Input params and default values.
    int N = 1024; // string length
    int base = 256; // basecase

    if (argc != 3) {
        ocrPrintf("USAGE: please run the program as follows:\n"
               "executable input_string_length basecase\n");
        ocrShutdown();
        return NULL_GUID;
    }

    N = (int)atol(ocrGetArgv(PTR_cmdLineArgs, 1));
    base = (int)atol(ocrGetArgv(PTR_cmdLineArgs, 2));

    // base can not be > N.
    if (N > 0 && N < base)
        base = N;

    /* The recursion halves the problem until it is <= base while the leaf
     * addresses its tile as (xi-1)/base, so a leaf covers exactly one tile
     * only when the ratio is a power of two; any other ratio silently
     * computes a different, wrong table. */
    if (N <= 0 || base <= 0 || N % base != 0 || ((N / base) & (N / base - 1)) != 0) {
        ocrPrintf("input_string_length (%d) / basecase (%d) must be a power of two\n",
               N, base);
        ocrShutdown();
        return NULL_GUID;
    }

    ocrPrintf("\n");
    ocrPrintf("Running LCS.\nStrings len: %d  basecase: %d\n", N, base);
    fflush(stdout);

    // Create mapguids for S, T and lcs_score matrix.
    ocrGuid_t s_labels, t_labels, score_matrix_labels;
    int num_labels = N / base;
    int grid_block_size = num_labels + 1;
    int num_blocks = (grid_block_size) * (grid_block_size);

    u64 s_range = (u64)num_labels;
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
    {
        u64 nranks;
        ocrAffinityCount(AFFINITY_PD, &nranks);
        s_range *= nranks;
    }
#endif
    ocrGuidRangeCreate(&s_labels, s_range, GUID_USER_DB);
    ocrGuidRangeCreate(&t_labels, num_labels, GUID_USER_DB);
    u64 score_range = (u64)num_blocks;
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
    {
        u64 nranks;
        ocrAffinityCount(AFFINITY_PD, &nranks);
        score_range *= nranks;
    }
#endif
    ocrGuidRangeCreate(&score_matrix_labels, score_range, GUID_USER_DB);

    int **S, **T;
    int** lcs_score;

    ocrGuid_t S_guid, T_guid, score_guid;

    ocrDbCreate(&S_guid,
        (void**)&S,
        sizeof(int*) * num_labels,
        DB_PROP_NONE,
        NULL_HINT,
        NO_ALLOC);

    assert(S != NULL);
    ocrDbCreate(&T_guid,
        (void**)&T,
        sizeof(int*) * num_labels,
        DB_PROP_NONE,
        NULL_HINT,
        NO_ALLOC);

    assert(T != NULL);
    // Using quadratic space.
    ocrDbCreate(&score_guid,
        (void**)&lcs_score,
        sizeof(int*) * num_blocks,
        DB_PROP_NONE,
        NULL_HINT,
        NO_ALLOC);

    assert(lcs_score != NULL);

    // Initialize the data
    //ocrPrintf("%d HERE\n", );

    init_param pv;

    pv.s_labels = s_labels;
    pv.t_labels = t_labels;
    pv.score_matrix_labels = score_matrix_labels;
    pv.base = base;
    pv.num_labels = num_labels;

    // Create the task template
    ocrGuid_t initGuidTmp;
    ocrEdtTemplateCreate(&initGuidTmp, InitEdt, sizeof(init_param) / sizeof(u64), 3);
    // Create the task
    ocrGuid_t initDataEdt, outputEventGuid, outputEventGuidOnce;
    ocrEventCreate(&outputEventGuid, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);

    ocrEdtCreate(&initDataEdt,
        initGuidTmp,
        EDT_PARAM_DEF,
        (u64*)&pv,
        EDT_PARAM_DEF,
        NULL,
        EDT_PROP_FINISH,
        NULL_HINT,
        &outputEventGuidOnce);

    ocrAddDependence(outputEventGuidOnce, outputEventGuid, 0, DB_MODE_NULL);
    ocrAddDependence(S_guid, initDataEdt, 0, DB_MODE_RW);
    ocrAddDependence(T_guid, initDataEdt, 1, DB_MODE_RW);
    ocrAddDependence(score_guid, initDataEdt, 2, DB_MODE_RW);

    //Create a new finish EDT
    //params are N, base, xi, xj, and the guids.

    LCS_task_params p;

    p.N = N;
    p.base = base;
    p.xi = 1;
    p.xj = 1;
    p.n = N;
    p.s_labels = s_labels;
    p.t_labels = t_labels;
    p.score_matrix_labels = score_matrix_labels;

    unsigned long start;
    start = cilk_getticks();

    // Create the task template
    ocrGuid_t recLCSGuid;
    ocrEdtTemplateCreate(&recLCSGuid, recLCSEdt, sizeof(LCS_task_params) / sizeof(u64), 1);
    //ocrPrintf("%d HERE\n", sizeof(LCS_task_params)/sizeof(u64) );

    // Create the task
    ocrGuid_t recLCSEdt, outputEventGuid2, outputEventGuidOnce2;
    ocrEventCreate(&outputEventGuid2, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);

    ocrEdtCreate(&recLCSEdt,
        recLCSGuid,
        EDT_PARAM_DEF,
        (u64*)&p,
        EDT_PARAM_DEF,
        NULL,
        EDT_PROP_FINISH,
        NULL_HINT,
        &outputEventGuidOnce2);

    ocrAddDependence(outputEventGuidOnce2, outputEventGuid2, 0, DB_MODE_NULL);
    ocrAddDependence(outputEventGuid, recLCSEdt, 0, DB_MODE_NULL);

    // Create the task template
    ocrGuid_t wrapup_tmp;
    u64 paramv2[2];
    paramv2[0] = base;
    paramv2[1] = start;

    ocrEdtTemplateCreate(&wrapup_tmp, wrapupEdt, 2, 2);

    // Create the task
    ocrGuid_t wrapup_taskGuid, wrapupEventGuid;
    ocrEdtCreate(&wrapup_taskGuid,
        wrapup_tmp,
        EDT_PARAM_DEF,
        (u64*)paramv2,
        EDT_PARAM_DEF,
        NULL,
        EDT_PROP_NONE,
        NULL_HINT,
        &wrapupEventGuid);

    ocrGuid_t shutDown_tmp, shutDown_taskGuid;
    ocrEdtTemplateCreate(&shutDown_tmp, shutDownEdt, 0, 1);
    ocrEdtCreate(&shutDown_taskGuid,
        shutDown_tmp,
        EDT_PARAM_DEF,
        NULL,
        EDT_PARAM_DEF,
        NULL,
        EDT_PROP_NONE,
        NULL_HINT,
        NULL);
    ocrAddDependence(wrapupEventGuid, shutDown_taskGuid, 0, DB_MODE_NULL);

    ocrGuid_t lcs_score_ij;
    ocrGuidFromIndex(&lcs_score_ij, score_matrix_labels, scoreIdx(num_blocks - 1, grid_block_size));

    ocrAddDependence(outputEventGuid2, wrapup_taskGuid, 0, DB_MODE_NULL);
    ocrAddDependence(lcs_score_ij, wrapup_taskGuid, 1, DB_MODE_RO);

    return NULL_GUID;
}
