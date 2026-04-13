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
 * threads that ran on the reporting node.
 */

#define _GNU_SOURCE
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
double times[NUM_THREADS][NTIMES];
ocrGuid_t tmp_copy, tmp_scale, tmp_add, tmp_triad, tmp_loop, tmp_finalize;
ocrGuid_t edt_finalize;
ocrGuid_t evt_finalize[NUM_THREADS];

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
    u32 i;

    for(i = 0; i<PER_THREAD_SIZE; i++) c[i] = a[i];
    return depv[1].guid;
}

ocrGuid_t scale(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    double *a = (double *)depv[0].ptr;
    double *b = (double *)depv[1].ptr;
    u32 i;

    for(i = 0; i<PER_THREAD_SIZE; i++) b[i] = scalar*a[i];

    return depv[1].guid;
}

ocrGuid_t add(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    double *a = (double *)depv[0].ptr;
    double *b = (double *)depv[1].ptr;
    double *c = (double *)depv[2].ptr;
    u32 i;

    for(i = 0; i<PER_THREAD_SIZE; i++) c[i] = a[i] + b[i];

    return depv[2].guid;
}

ocrGuid_t triad(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    double *a = (double *)depv[0].ptr;
    double *b = (double *)depv[1].ptr;
    double *c = (double *)depv[2].ptr;
    u32 i;

    for(i = 0; i<PER_THREAD_SIZE; i++) a[i] = b[i] + scalar*c[i];

    times[paramv[1]][paramv[0]] = mysecond() - times[paramv[1]][paramv[0]];
    return depv[0].guid;
}

/* ================================================================ */

ocrGuid_t finalize(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
  /* Validate a[] — same approach as checkSTREAMresults() in reference STREAM.
   * a's final value encodes all four kernels over all iterations. */
  STREAM_TYPE aj = 2.0;  /* initial value from mainLet */
  int k;
  for (k = 0; k < NTIMES; k++) {
      STREAM_TYPE cj = aj;               /* copy  */
      STREAM_TYPE bj = scalar * aj;      /* scale */
      cj = aj + bj;                      /* add   */
      aj = bj + scalar * cj;             /* triad: a = b(scale) + scalar*c(add) */
  }

  double epsilon = (sizeof(STREAM_TYPE) == 4) ? 1.e-6 : 1.e-13;
  u64 errCount = 0;
  u32 t, i;
  for (t = 0; t < NUM_THREADS; t++) {
      double *a = (double *)depv[t].ptr;
      for (i = 0; i < PER_THREAD_SIZE; i++) {
          double relErr = (aj != 0.0) ? fabs((a[i] - aj) / aj) : fabs(a[i]);
          if (relErr > epsilon) errCount++;
      }
  }

  if (errCount == 0)
      PRINTF("Solution Validates: all %d elements match expected value\n",
             PER_THREAD_SIZE * NUM_THREADS);
  else
      PRINTF("Solution FAILED Validation: %llu errors in a[]\n",
             (unsigned long long)errCount);

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
  ocrGuid_t db_a = depv[0].guid;
  ocrGuid_t db_b = depv[1].guid;
  ocrGuid_t db_c = depv[2].guid;
  u64 iter = paramv[0];
  u64 tid = paramv[1];
  u64 param[2] = { iter, tid };

  /* Build affinity hint for this thread's target node */
  ocrHint_t edtHint;
  getAffinityHints(tid, &edtHint, NULL);

  times[tid][iter] = mysecond();
  ocrGuid_t copy_output, scale_output, add_output, triad_output;
  ocrGuid_t edt_copy, edt_scale, edt_add, edt_triad;

  /* Create kernel EDTs with affinity hint (routed to owning node) */
  ocrEdtCreate(&edt_copy, tmp_copy, EDT_PARAM_DEF, param, EDT_PARAM_DEF, NULL, PROPERTIES, &edtHint, &copy_output);
  ocrEdtCreate(&edt_scale, tmp_scale, EDT_PARAM_DEF, param, EDT_PARAM_DEF, NULL, PROPERTIES, &edtHint, &scale_output);
  ocrEdtCreate(&edt_add, tmp_add, EDT_PARAM_DEF, param, EDT_PARAM_DEF, NULL, PROPERTIES, &edtHint, &add_output);
  ocrEdtCreate(&edt_triad, tmp_triad, EDT_PARAM_DEF, param, EDT_PARAM_DEF, NULL, PROPERTIES, &edtHint, &triad_output);

  ocrAddDependence(db_a, edt_triad, 0, DB_MODE_RW);
  ocrAddDependence(scale_output, edt_triad, 1, DB_MODE_RO);
  ocrAddDependence(add_output, edt_triad, 2, DB_MODE_RO);

  ocrAddDependence(db_a, edt_add, 0, DB_MODE_RO);
  ocrAddDependence(scale_output, edt_add, 1, DB_MODE_RO);
  ocrAddDependence(copy_output, edt_add, 2, DB_MODE_RW);

  ocrAddDependence(db_a, edt_scale, 0, DB_MODE_RO);
  ocrAddDependence(db_b, edt_scale, 1, DB_MODE_RW);

  ocrAddDependence(db_a, edt_copy, 0, DB_MODE_RO);
  ocrAddDependence(db_c, edt_copy, 1, DB_MODE_RW);

  iter++;
  param[0] = iter;
  if (iter < NTIMES) {
    ocrGuid_t edt_loop;
    ocrEdtCreate(&edt_loop, tmp_loop, EDT_PARAM_DEF, param, EDT_PARAM_DEF, NULL, PROPERTIES, &edtHint, NULL);

    ocrAddDependence(triad_output, edt_loop, 0, DB_MODE_RW);
    ocrAddDependence(scale_output, edt_loop, 1, DB_MODE_RW);
    ocrAddDependence(add_output, edt_loop, 2, DB_MODE_RW);
  } else {
      ocrAddDependence(triad_output, evt_finalize[tid], 0, DB_DEFAULT_MODE);
  }

  return NULL_GUID;
}

/* ================================================================
 * mainLet: creates persistent a/b/c DBs on the target node
 * and starts the loop chain.
 * paramv[0] = thread id
 * ================================================================ */

ocrGuid_t mainLet(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    ocrGuid_t db_a, db_b, db_c;
    double *a, *b, *c;
    u32 i;
    ocrGuid_t edt_loop, output_evt;
    u64 tid = paramv[0];

    /* Build affinity hints: DBs on same node as this EDT */
    ocrHint_t edtHint, dbHint;
    getAffinityHints(tid, &edtHint, &dbHint);

    /* Create DBs with DB affinity hint (placed on target node) */
    ocrDbCreate(&db_a, (void **)&a, sizeof(double)*PER_THREAD_SIZE,
                             FLAGS, &dbHint, NO_ALLOC);
    ocrDbCreate(&db_b, (void **)&b, sizeof(double)*PER_THREAD_SIZE,
                             FLAGS, &dbHint, NO_ALLOC);
    ocrDbCreate(&db_c, (void **)&c, sizeof(double)*PER_THREAD_SIZE,
                             FLAGS, &dbHint, NO_ALLOC);

    /* Initialize arrays */
    for(i = 0; i<PER_THREAD_SIZE; i++) {
        a[i] = 2.0;
        b[i] = 2.0;
        c[i] = 0.0;
    }

    u64 param[2] = { 0, tid };
    ocrEdtCreate(&edt_loop, tmp_loop, EDT_PARAM_DEF, param, EDT_PARAM_DEF, NULL,
                 EDT_PROP_FINISH, &edtHint, &output_evt);

    ocrAddDependence(db_a, edt_loop, 0, DB_MODE_RW);
    ocrAddDependence(db_b, edt_loop, 1, DB_MODE_RW);
    ocrAddDependence(db_c, edt_loop, 2, DB_MODE_RW);

    return output_evt;
}

/* ================================================================ */

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    preamble();

    ocrGuid_t tmp_mainLet;
    ocrGuid_t edt_mainLet;
    u64 i;

    ocrEdtTemplateCreate(&tmp_mainLet, mainLet, 1, 0);

    ocrEdtTemplateCreate(&tmp_loop, loop, 2, 3);
    ocrEdtTemplateCreate(&tmp_copy, copy, 2, 2);
    ocrEdtTemplateCreate(&tmp_scale, scale, 2, 2);
    ocrEdtTemplateCreate(&tmp_add, add, 2, 3);
    ocrEdtTemplateCreate(&tmp_triad, triad, 2, 3);

    ocrEdtTemplateCreate(&tmp_finalize, finalize, 0, NUM_THREADS);
    ocrEdtCreate(&edt_finalize, tmp_finalize, EDT_PARAM_DEF, NULL, EDT_PARAM_DEF, NULL, PROPERTIES, NULL_HINT, NULL);

    for(i = 0; i<NUM_THREADS; i++) {
        /* Build per-thread EDT affinity hint */
        ocrHint_t edtHint;
        getAffinityHints(i, &edtHint, NULL);

        ocrEventCreate(&evt_finalize[i], OCR_EVENT_ONCE_T, true);
        ocrAddDependence(evt_finalize[i], edt_finalize, i, DB_MODE_RO);
        ocrEdtCreate(&edt_mainLet, tmp_mainLet, EDT_PARAM_DEF, &i, EDT_PARAM_DEF, NULL,
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
           (unsigned long long) STREAM_ARRAY_SIZE, OFFSET);
    PRINTF("Memory per array = %.1f MiB (= %.1f GiB).\n",
        BytesPerWord * ( (double) STREAM_ARRAY_SIZE / 1024.0/1024.0),
        BytesPerWord * ( (double) STREAM_ARRAY_SIZE / 1024.0/1024.0/1024.0));
    PRINTF("Total memory required = %.1f MiB (= %.1f GiB).\n",
        (3.0 * BytesPerWord) * ( (double) STREAM_ARRAY_SIZE / 1024.0/1024.),
        (3.0 * BytesPerWord) * ( (double) STREAM_ARRAY_SIZE / 1024.0/1024./1024.));

#ifdef ENABLE_EXTENSION_AFFINITY
    {
        u64 affCount = 1;
        ocrAffinityCount(AFFINITY_PD, &affCount);
        PRINTF("Data is distributed across %llu node(s)\n",
               (unsigned long long) affCount);
        PRINTF("   %d threads total, ~%d threads per node\n",
               NUM_THREADS, NUM_THREADS / (int)affCount);
        PRINTF("   Per-thread partition = %d elements (%.1f MiB)\n",
               PER_THREAD_SIZE,
               BytesPerWord * ((double)PER_THREAD_SIZE / 1024.0/1024.0));
    }
#endif

    PRINTF("Each kernel will be executed %d times.\n", NTIMES);
    PRINTF(" The *best* time for each kernel (excluding the first iteration)\n");
    PRINTF(" will be used to compute the reported bandwidth.\n");
    PRINTF("The SCALAR value used for this run is %f\n", (double)SCALAR);

    PRINTF(HLINE);
}

int printTimes(void)
{
    double avgtime = 0.0;
    double mintime = FLT_MAX;
    double maxtime = 0.0;
    int j, k;

    for (k=1; k<NTIMES; k++) /* note -- skip first iteration */
        {
        for (j=0; j<NUM_THREADS; j++)
            {
            avgtime = avgtime + times[j][k];
            mintime = MIN(mintime, times[j][k]);
            maxtime = MAX(maxtime, times[j][k]);
            }
        }

    PRINTF(HLINE);
    PRINTF("%f MB/s %f %f %f\n",
           1.0E-06*10*sizeof(STREAM_TYPE)*PER_THREAD_SIZE/mintime,
           avgtime/((NTIMES-1)*NUM_THREADS), mintime, maxtime);
    PRINTF(HLINE);

    return 0;
}
