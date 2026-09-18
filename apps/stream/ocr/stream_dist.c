/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */
/* Based on stream benchmark at https://www.cs.virginia.edu/stream/FTP/Code/stream.c
 *
 * Distributed variant: uses OCR_HINT_EDT_AFFINITY and OCR_HINT_DB_AFFINITY
 * to route EDTs and DBs to specific nodes for multi-node execution.
 * Based on stream_org.c (static allocation with 3 persistent DBs per thread).
 *
 * NOTE: the times[] array is per-process; printTimes() reports only the
 * sweeps whose start and end both ran on the reporting process.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ocr.h"
#include <stdlib.h>
#include <stdio.h>

# include <math.h>
# include <float.h>
#include "pthread.h"

#ifdef ENABLE_EXTENSION_AFFINITY
#include "extensions/ocr-affinity.h"
#endif

#ifndef STREAM_ARRAY_SIZE
#define STREAM_ARRAY_SIZE 9000000
#endif
#ifndef NUM_THREADS
#define NUM_THREADS  32
#endif

#define FLAGS DB_PROP_NONE
#define PROPERTIES EDT_PROP_NONE

# ifndef MIN
# define MIN(x,y) ((x)<(y)?(x):(y))
# endif
# ifndef MAX
# define MAX(x,y) ((x)>(y)?(x):(y))
# endif

# define HLINE "-------------------------------------------------------------\n"

#ifndef STREAM_TYPE
#define STREAM_TYPE double
#endif

#define MOD NUM_THREADS

#ifndef NTIMES
#define NTIMES 1000
#endif

#define OFFSET 0

#define PER_THREAD_SIZE (STREAM_ARRAY_SIZE/NUM_THREADS)

/* Use 0.42 (MPI STREAM standard) to avoid FP overflow with many iterations */
#ifndef SCALAR
#define SCALAR 0.42
#endif
STREAM_TYPE scalar = SCALAR;

#include <sys/time.h>

double mysecond()
{
        struct timeval tp;
        struct timezone tzp;
        int i;

        i = gettimeofday(&tp,&tzp);
        return ( (double) tp.tv_sec + (double) tp.tv_usec * 1.e-6 );
}

void checkSTREAMresults (void);
void preamble(void);
int printTimes(void);

/* Runtime-resolved sizes (positional argv in mainEdt); defaults mirror the
 * compile-time macros so an argument-free run behaves identically.  These
 * globals are populated only on the rank that runs mainEdt; the per-EDT sizes
 * that remote ranks need travel through paramv (see the *_PARAMC layouts). */
u64 streamArraySize = STREAM_ARRAY_SIZE;
u64 numThreads      = NUM_THREADS;
u64 nTimes          = NTIMES;
u64 perThreadSize   = PER_THREAD_SIZE;

/* Per-process timing array — allocated only on the rank that runs mainEdt.
 * Remote ranks leave it NULL; the reporting process says so and reports no
 * bandwidth, so every write is guarded by a non-NULL check. */
double *times;             /* [numThreads][nTimes] flattened; index via TIMES() */

/* Row-major access to the runtime-sized timing matrix.  A cell holds the
 * negated start stamp until the task that ends the sweep turns it into a
 * positive duration, so a cell whose start and end did not both run in this
 * process stays non-positive and is distinguishable from a complete one. */
#define TIMES(t, k) times[(u64)(t) * nTimes + (u64)(k)]

/* Cross-rank GUIDs and sizes travel through paramv (deep-copied at EDT
 * create), never file-scope statics: statics are per-process, so only the
 * rank that ran mainEdt would see them initialized.
 *
 * paramv layouts:
 *   mainLet (paramc=10): [0] tid, [1] evt_finalize_my,
 *     [2..6] tmp_copy/scale/add/triad/loop, [7] perThreadSize, [8] nTimes,
 *     [9] tmp_check
 *   loop (paramc=11): [0] iter, [1] tid, [2] evt_finalize_my,
 *     [3..7] templates, [8] perThreadSize, [9] nTimes, [10] tmp_check
 *   kernels (paramc=3): [0] iter, [1] tid, [2] perThreadSize
 *   check (paramc=3): [0] tid, [1] perThreadSize, [2] nTimes
 *   finalize (paramc=2): [0] numThreads, [1] perThreadSize */
#define MAINLET_PARAMC 10
#define LOOP_PARAMC    11

/* What one chain reports to the join: a count of its own verified elements
 * and one sampled element, so the join carries counts and not arrays. */
typedef struct {
    u64    matched;
    double first;
} streamCheck_t;

/* --- Helper: build EDT affinity hint for a given thread id --- */
static void getAffinityHints(u64 tid, ocrHint_t *edtHint, ocrHint_t *dbHint)
{
#ifdef ENABLE_EXTENSION_AFFINITY
    u64 affCount = 1;
    ocrAffinityCount(AFFINITY_PD, &affCount);
    u64 node_idx = tid % affCount;
    ocrGuid_t affinity;
    ocrAffinityGetAt(AFFINITY_PD, node_idx, &affinity);

    if (edtHint) {
        ocrHintInit(edtHint, OCR_HINT_EDT_T);
        ocrSetHintValue(edtHint, OCR_HINT_EDT_AFFINITY,
                        ocrAffinityToHintValue(affinity));
    }
    if (dbHint) {
        ocrHintInit(dbHint, OCR_HINT_DB_T);
        ocrSetHintValue(dbHint, OCR_HINT_DB_AFFINITY,
                        ocrAffinityToHintValue(affinity));
    }
#else
    /* No affinity extension: fall back to NULL_HINT behavior */
    if (edtHint) ocrHintInit(edtHint, OCR_HINT_EDT_T);
    if (dbHint) ocrHintInit(dbHint, OCR_HINT_DB_T);
#endif
}

/* ================================================================
 * Kernel EDTs: operate in-place on persistent DBs (stream_org pattern)
 * ================================================================ */

ocrGuid_t copy(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    double *a = (double *)depv[0].ptr;
    double *c = (double *)depv[1].ptr;
    u64 pts = paramv[2];
    u32 i;

    for(i = 0; i<pts; i++) c[i] = a[i];
    return depv[1].guid;
}

ocrGuid_t scale(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    double *a = (double *)depv[0].ptr;
    double *b = (double *)depv[1].ptr;
    u64 pts = paramv[2];
    u32 i;

    for(i = 0; i<pts; i++) b[i] = scalar*a[i];

    return depv[1].guid;
}

ocrGuid_t add(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    double *a = (double *)depv[0].ptr;
    double *b = (double *)depv[1].ptr;
    double *c = (double *)depv[2].ptr;
    u64 pts = paramv[2];
    u32 i;

    for(i = 0; i<pts; i++) c[i] = a[i] + b[i];

    return depv[2].guid;
}

ocrGuid_t triad(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    double *a = (double *)depv[0].ptr;
    double *b = (double *)depv[1].ptr;
    double *c = (double *)depv[2].ptr;
    u64 pts = paramv[2];
    u32 i;

    for(i = 0; i<pts; i++) a[i] = b[i] + scalar*c[i];

    if(times) {
        double start = TIMES(paramv[1], paramv[0]);
        if(start < 0.0) TIMES(paramv[1], paramv[0]) = mysecond() + start;
    }
    return depv[0].guid;
}

/* ================================================================ */

/* Verification runs where the data is: each chain checks its own slice
 * against the closed form of the kernel recurrence and returns the count it
 * verified, so no task ever holds more than one slice and the join carries
 * one record per chain instead of the whole result. */
ocrGuid_t check(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
  double *a  = (double *)depv[0].ptr;
  u64 tid    = paramv[0];
  u64 pts    = paramv[1];
  u64 ntimes = paramv[2];
  u64 i, k, matched = 0;
  double epsilon = (sizeof(STREAM_TYPE) == 4) ? 1.e-6 : 1.e-13;
  STREAM_TYPE aj = 2.0;  /* initial value from mainLet */
  streamCheck_t *r;
  ocrGuid_t db_r;
  ocrHint_t dbHint;

  for (k = 0; k < ntimes; k++) {
      STREAM_TYPE bj = scalar * aj;      /* scale */
      STREAM_TYPE cj = aj + bj;          /* add   */
      aj = bj + scalar * cj;             /* triad: a = b(scale) + scalar*c(add) */
  }

  for (i = 0; i < pts; i++) {
      double relErr = (aj != 0.0) ? fabs((a[i] - aj) / aj) : fabs(a[i]);
      if (relErr <= epsilon) matched++;
  }

  getAffinityHints(tid, NULL, &dbHint);
  ocrDbCreate(&db_r, (void **)&r, sizeof(streamCheck_t),
                           FLAGS, &dbHint, NO_ALLOC);
  r->matched = matched;
  r->first   = a[0];

  return db_r;
}

ocrGuid_t finalize(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
  /* Runtime sizes travel through paramv; file-scope globals are per-process
   * under fork-launched multinode runtimes and unset on ranks other than the
   * one that ran mainEdt. */
  u64 numThreads    = paramv[0];
  u64 perThreadSize = paramv[1];

  u64 total = perThreadSize * numThreads;
  u64 matched = 0;
  u64 t;

  for (t = 0; t < numThreads; t++)
      matched += ((streamCheck_t *)depv[t].ptr)->matched;

  if (matched == total)
      PRINTF("Solution Validates: all %llu elements match expected value\n",
             (unsigned long long)matched);
  else
      PRINTF("Solution FAILED Validation: %llu of %llu elements in a[] differ\n",
             (unsigned long long)(total - matched), (unsigned long long)total);

  PRINTF("STREAM_RESULT a[0] = %.15e\n", ((streamCheck_t *)depv[0].ptr)->first);

  printTimes();
  ocrShutdown();
  return NULL_GUID;
}

/* ================================================================
 * Loop EDT: chains Copy -> Scale -> Add -> Triad per iteration,
 * with affinity hints targeting the owning node.
 * paramv[0] = iteration, paramv[1] = thread id
 * ================================================================ */

ocrGuid_t loop(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  /* paramv: [iter, tid, evt_finalize_my, tmp_copy, tmp_scale, tmp_add,
   *          tmp_triad, tmp_loop]; depv[0..2] = db_a/b/c (RW) */
  ocrGuid_t db_a = depv[0].guid;
  ocrGuid_t db_b = depv[1].guid;
  ocrGuid_t db_c = depv[2].guid;
  u64 iter = paramv[0];
  u64 tid = paramv[1];
  ocrGuid_t evt_finalize_my = (ocrGuid_t){.guid = (intptr_t)paramv[2]};
  ocrGuid_t tmp_copy_g  = (ocrGuid_t){.guid = (intptr_t)paramv[3]};
  ocrGuid_t tmp_scale_g = (ocrGuid_t){.guid = (intptr_t)paramv[4]};
  ocrGuid_t tmp_add_g   = (ocrGuid_t){.guid = (intptr_t)paramv[5]};
  ocrGuid_t tmp_triad_g = (ocrGuid_t){.guid = (intptr_t)paramv[6]};
  ocrGuid_t tmp_loop_g  = (ocrGuid_t){.guid = (intptr_t)paramv[7]};
  ocrGuid_t tmp_check_g = (ocrGuid_t){.guid = (intptr_t)paramv[10]};
  u64 pts    = paramv[8];
  u64 ntimes = paramv[9];

  /* Build affinity hint for this thread's target node */
  ocrHint_t edtHint;
  getAffinityHints(tid, &edtHint, NULL);

  if(times) TIMES(tid, iter) = -mysecond();
  ocrGuid_t copy_output, scale_output, add_output, triad_output;
  ocrGuid_t edt_copy, edt_scale, edt_add, edt_triad;

  /* Child kernel paramv: [iter, tid, perThreadSize]. */
  u64 child_param[3] = { iter, tid, pts };
  ocrEdtCreate(&edt_copy,  tmp_copy_g,  EDT_PARAM_DEF, child_param, EDT_PARAM_DEF, NULL, PROPERTIES, &edtHint, &copy_output);
  ocrEdtCreate(&edt_scale, tmp_scale_g, EDT_PARAM_DEF, child_param, EDT_PARAM_DEF, NULL, PROPERTIES, &edtHint, &scale_output);
  ocrEdtCreate(&edt_add,   tmp_add_g,   EDT_PARAM_DEF, child_param, EDT_PARAM_DEF, NULL, PROPERTIES, &edtHint, &add_output);
  ocrEdtCreate(&edt_triad, tmp_triad_g, EDT_PARAM_DEF, child_param, EDT_PARAM_DEF, NULL, PROPERTIES, &edtHint, &triad_output);

  /* edt_triad / edt_add deps are relay-style (slots 1/2 wait on
   * scale_output / add_output / copy_output); none is runnable yet. */
  ocrAddDependence(db_a, edt_triad, 0, DB_MODE_RW);
  ocrAddDependence(scale_output, edt_triad, 1, DB_MODE_RO);
  ocrAddDependence(add_output, edt_triad, 2, DB_MODE_RO);

  ocrAddDependence(db_a, edt_add, 0, DB_MODE_RO);
  ocrAddDependence(scale_output, edt_add, 1, DB_MODE_RO);
  ocrAddDependence(copy_output, edt_add, 2, DB_MODE_RW);

  /* Every waiter on a ONCE event must be registered before the event's
   * source EDT becomes runnable, so wire the output-event waiters here and
   * keep the chain-starting addDeps last. */
  iter++;
  if (iter < ntimes) {
    /* Forward all paramv slots to the next iteration. */
    u64 next_param[LOOP_PARAMC] = {
      iter, tid, paramv[2], paramv[3], paramv[4],
      paramv[5], paramv[6], paramv[7], pts, ntimes, paramv[10]
    };
    ocrGuid_t edt_loop;
    ocrEdtCreate(&edt_loop, tmp_loop_g, EDT_PARAM_DEF, next_param, EDT_PARAM_DEF, NULL, PROPERTIES, &edtHint, NULL);

    ocrAddDependence(triad_output, edt_loop, 0, DB_MODE_RW);
    ocrAddDependence(scale_output, edt_loop, 1, DB_MODE_RW);
    ocrAddDependence(add_output, edt_loop, 2, DB_MODE_RW);
  } else {
    /* Last sweep: this chain verifies its own slice and hands the join a
     * record instead of the array. */
    u64 check_param[3] = { tid, pts, ntimes };
    ocrGuid_t edt_check, check_output;
    ocrEdtCreate(&edt_check, tmp_check_g, EDT_PARAM_DEF, check_param, EDT_PARAM_DEF, NULL, PROPERTIES, &edtHint, &check_output);

    ocrAddDependence(check_output, evt_finalize_my, 0, DB_DEFAULT_MODE);
    ocrAddDependence(triad_output, edt_check, 0, DB_MODE_RO);
  }

  /* Chain-starting addDeps last: these make edt_scale/edt_copy runnable. */
  ocrAddDependence(db_a, edt_scale, 0, DB_MODE_RO);
  ocrAddDependence(db_b, edt_scale, 1, DB_MODE_RW);

  ocrAddDependence(db_a, edt_copy, 0, DB_MODE_RO);
  ocrAddDependence(db_c, edt_copy, 1, DB_MODE_RW);

  return NULL_GUID;
}

/* ================================================================
 * mainLet: creates persistent a/b/c DBs on the target node
 * and starts the loop chain.
 * paramv[0] = thread id
 * ================================================================ */

ocrGuid_t mainLet(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    u64 tid                   = paramv[0];
    ocrGuid_t evt_finalize_my = (ocrGuid_t){.guid = (intptr_t)paramv[1]};
    ocrGuid_t tmp_copy_g      = (ocrGuid_t){.guid = (intptr_t)paramv[2]};
    ocrGuid_t tmp_scale_g     = (ocrGuid_t){.guid = (intptr_t)paramv[3]};
    ocrGuid_t tmp_add_g       = (ocrGuid_t){.guid = (intptr_t)paramv[4]};
    ocrGuid_t tmp_triad_g     = (ocrGuid_t){.guid = (intptr_t)paramv[5]};
    ocrGuid_t tmp_loop_g      = (ocrGuid_t){.guid = (intptr_t)paramv[6]};
    u64 pts    = paramv[7];
    u64 ntimes = paramv[8];

    ocrGuid_t db_a, db_b, db_c;
    double *a, *b, *c;
    u32 i;
    ocrGuid_t edt_loop, output_evt;

    /* Build affinity hints: DBs on same node as this EDT */
    ocrHint_t edtHint, dbHint;
    getAffinityHints(tid, &edtHint, &dbHint);

    /* Create DBs with DB affinity hint (placed on target node) */
    ocrDbCreate(&db_a, (void **)&a, sizeof(double)*pts,
                             FLAGS, &dbHint, NO_ALLOC);
    ocrDbCreate(&db_b, (void **)&b, sizeof(double)*pts,
                             FLAGS, &dbHint, NO_ALLOC);
    ocrDbCreate(&db_c, (void **)&c, sizeof(double)*pts,
                             FLAGS, &dbHint, NO_ALLOC);

    /* Initialize arrays */
    for(i = 0; i<pts; i++) {
        a[i] = 2.0;
        b[i] = 2.0;
        c[i] = 0.0;
    }

    /* Forward all GUIDs and runtime sizes to loop via paramv. */
    u64 param[LOOP_PARAMC] = {
      0 /*iter*/, tid,
      paramv[1] /*evt_finalize_my*/,
      paramv[2] /*tmp_copy*/,
      paramv[3] /*tmp_scale*/,
      paramv[4] /*tmp_add*/,
      paramv[5] /*tmp_triad*/,
      paramv[6] /*tmp_loop*/,
      pts, ntimes,
      paramv[9] /*tmp_check*/
    };
    ocrEdtCreate(&edt_loop, tmp_loop_g, EDT_PARAM_DEF, param, EDT_PARAM_DEF, NULL,
                 EDT_PROP_FINISH, &edtHint, &output_evt);

    ocrAddDependence(db_a, edt_loop, 0, DB_MODE_RW);
    ocrAddDependence(db_b, edt_loop, 1, DB_MODE_RW);
    ocrAddDependence(db_c, edt_loop, 2, DB_MODE_RW);

    return output_evt;
}

/* ================================================================ */

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    // Positional argv: [streamArraySize] [numThreads] [nTimes]; absent args keep the #define defaults.
    if(depc >= 1 && depv[0].ptr) {
        u64 argc = getArgc(depv[0].ptr);
        if(argc > 1) streamArraySize = strtoull(getArgv(depv[0].ptr, 1), NULL, 10);
        if(argc > 2) numThreads      = strtoull(getArgv(depv[0].ptr, 2), NULL, 10);
        if(argc > 3) nTimes          = strtoull(getArgv(depv[0].ptr, 3), NULL, 10);
    }
    if(numThreads == 0)      numThreads      = NUM_THREADS;   // guard degenerate counts
    if(streamArraySize == 0) streamArraySize = STREAM_ARRAY_SIZE;
    if(nTimes == 0)          nTimes          = NTIMES;
    if(streamArraySize % numThreads != 0)
        fprintf(stderr,
                "stream_dist: array size %llu not a multiple of %llu threads; "
                "truncating to %llu elements/thread (%llu total)\n",
                (unsigned long long)streamArraySize, (unsigned long long)numThreads,
                (unsigned long long)(streamArraySize / numThreads),
                (unsigned long long)((streamArraySize / numThreads) * numThreads));
    perThreadSize = streamArraySize / numThreads;
    times = (double *)calloc(numThreads * nTimes, sizeof(double));

    preamble();

    ocrGuid_t tmp_mainLet, tmp_loop, tmp_copy, tmp_scale, tmp_add, tmp_triad;
    ocrGuid_t tmp_check, tmp_finalize;
    ocrGuid_t edt_finalize, edt_mainLet;
    u64 i;

    /* Create the templates once on this rank and forward their GUIDs
     * to children through paramv so cross-rank EDTs can use the same
     * GUIDs without per-rank template recreation. */
    ocrEdtTemplateCreate(&tmp_mainLet, mainLet, MAINLET_PARAMC, 0);
    ocrEdtTemplateCreate(&tmp_finalize, finalize, 2, numThreads);
    ocrEdtTemplateCreate(&tmp_copy,  copy,  3, 2);
    ocrEdtTemplateCreate(&tmp_scale, scale, 3, 2);
    ocrEdtTemplateCreate(&tmp_add,   add,   3, 3);
    ocrEdtTemplateCreate(&tmp_triad, triad, 3, 3);
    ocrEdtTemplateCreate(&tmp_check, check, 3, 1);
    ocrEdtTemplateCreate(&tmp_loop,  loop,  LOOP_PARAMC, 3);
    /* finalize paramv: [numThreads, perThreadSize] (remote ranks cannot read
     * the file-scope size globals). */
    u64 finalize_param[2] = { numThreads, perThreadSize };
    ocrEdtCreate(&edt_finalize, tmp_finalize, EDT_PARAM_DEF, finalize_param, EDT_PARAM_DEF, NULL, PROPERTIES, NULL_HINT, NULL);

    for(i = 0; i<numThreads; i++) {
        /* Build per-thread EDT affinity hint */
        ocrHint_t edtHint;
        getAffinityHints(i, &edtHint, NULL);

        ocrGuid_t evt_finalize_i;
        ocrEventCreate(&evt_finalize_i, OCR_EVENT_ONCE_T, true);
        ocrAddDependence(evt_finalize_i, edt_finalize, i, DB_MODE_RO);

        u64 mainLet_param[MAINLET_PARAMC] = {
          i /*tid*/,
          (u64)evt_finalize_i.guid,
          (u64)tmp_copy.guid,
          (u64)tmp_scale.guid,
          (u64)tmp_add.guid,
          (u64)tmp_triad.guid,
          (u64)tmp_loop.guid,
          perThreadSize, nTimes,
          (u64)tmp_check.guid
        };
        ocrEdtCreate(&edt_mainLet, tmp_mainLet, EDT_PARAM_DEF, mainLet_param,
                     EDT_PARAM_DEF, NULL,
                     PROPERTIES, &edtHint, NULL);
    }

    return NULL_GUID;
}

/* ================================================================
 * Preamble and results reporting
 * ================================================================ */

void preamble(void)
{
    int BytesPerWord;

    PRINTF(HLINE);
    PRINTF("STREAM version $Revision: 5.10 $ (distributed OCR variant)\n");
    PRINTF(HLINE);
    BytesPerWord = sizeof(STREAM_TYPE);
    PRINTF("This system uses %d bytes per array element.\n", BytesPerWord);

    PRINTF(HLINE);

    PRINTF("Array size = %llu (elements), Offset = %d (elements)\n",
           (unsigned long long) streamArraySize, OFFSET);
    PRINTF("Memory per array = %.1f MiB (= %.1f GiB).\n",
        BytesPerWord * ( (double) streamArraySize / 1024.0/1024.0),
        BytesPerWord * ( (double) streamArraySize / 1024.0/1024.0/1024.0));
    PRINTF("Total memory required = %.1f MiB (= %.1f GiB).\n",
        (3.0 * BytesPerWord) * ( (double) streamArraySize / 1024.0/1024.),
        (3.0 * BytesPerWord) * ( (double) streamArraySize / 1024.0/1024./1024.));

#ifdef ENABLE_EXTENSION_AFFINITY
    {
        u64 affCount = 1;
        ocrAffinityCount(AFFINITY_PD, &affCount);
        PRINTF("Data is distributed across %llu node(s)\n",
               (unsigned long long) affCount);
        PRINTF("   %d threads total, ~%d threads per node\n",
               (int)numThreads, (int)(numThreads / affCount));
        PRINTF("   Per-thread partition = %d elements (%.1f MiB)\n",
               (int)perThreadSize,
               BytesPerWord * ((double)perThreadSize / 1024.0/1024.0));
    }
#endif

    PRINTF("Each kernel will be executed %d times.\n", (int)nTimes);
    PRINTF(" The *best* time for each kernel (excluding the first iteration)\n");
    PRINTF(" will be used to compute the reported bandwidth.\n");
    PRINTF("The SCALAR value used for this run is %f\n", (double)SCALAR);

    PRINTF(HLINE);
}

/* A sample is written by the task that ran the sweep and read here, so a
 * cell is meaningful only on the process that holds the array and only when
 * both the start and the end of that sweep ran on it; the sign encoding
 * leaves every other cell non-positive.  Report what is present and say so
 * when nothing is. */
int printTimes(void)
{
    double avgtime = 0.0;
    double mintime = FLT_MAX;
    double maxtime = 0.0;
    u64 j, k, samples = 0;

    PRINTF(HLINE);
    if (times == NULL) {
        PRINTF("No kernel timings on this process: MB/s not reported\n");
        PRINTF(HLINE);
        return 0;
    }

    for (k=1; k<nTimes; k++) /* note -- skip first iteration */
        {
        for (j=0; j<numThreads; j++)
            {
            double t = TIMES(j, k);
            if (t <= 0.0) continue;   /* start or end ran in another process */
            avgtime = avgtime + t;
            mintime = MIN(mintime, t);
            maxtime = MAX(maxtime, t);
            samples++;
            }
        }

    if (samples == 0)
        PRINTF("No complete kernel timing: MB/s not reported\n");
    else
        PRINTF("%f MB/s %f %f %f\n",
               1.0E-06*10*sizeof(STREAM_TYPE)*perThreadSize/mintime,
               avgtime/(double)samples, mintime, maxtime);
    PRINTF(HLINE);

    return 0;
}
