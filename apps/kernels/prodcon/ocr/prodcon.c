/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#define _GNU_SOURCE
#include "ocr.h"
#include <stdlib.h>
#include <stdio.h>
#include <sched.h>

#define ENABLE_EXTENSION_LABELING
#include "extensions/ocr-labeling.h"

#include <math.h>
#include <float.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE (1310720*32)
#endif
#ifndef NUM_THREADS
#define NUM_THREADS  128
#endif
#define SKEW 1
#define BUFFER NUM_THREADS
#ifndef NTIMES
#define NTIMES 200
#endif
#define PER_THREAD_SIZE ARRAY_SIZE/NUM_THREADS

#define FLAGS DB_PROP_NONE
#define PROPERTIES EDT_PROP_NONE

# ifndef MIN
# define MIN(x,y) ((x)<(y)?(x):(y))
# endif
# ifndef MAX
# define MAX(x,y) ((x)>(y)?(x):(y))
# endif

#include <sys/time.h>

double mysecond()
{
        struct timeval tp;
        struct timezone tzp;

        gettimeofday(&tp,&tzp);
        return ( (double) tp.tv_sec + (double) tp.tv_usec * 1.e-6 );
}

/* Cross-EDT values (workload sizes, template GUIDs, labeled-GUID range bases,
 * the per-thread finalize event) travel through paramv, deep-copied at EDT
 * create time.  File-scope statics are per-process under a fork-launched
 * multi-node runtime and would read back as zero-init BSS on a remote rank.
 *
 * The timing matrix is a rank-local diagnostic: it is allocated only on the
 * rank that runs mainEdt, so every access is guarded by a non-NULL check and
 * printTimes reports the rank that holds it.
 *
 * mainLet paramv (paramc=11):
 *   [0] tid, [1] evt_finalize (per-thread ONCE event GUID),
 *   [2] mapProdGuid, [3] mapConsGuid (labeled-GUID range bases),
 *   [4] tmp_producer, [5] tmp_consumer, [6] tmp_loop (ocrEdtTemplate GUIDs),
 *   [7] numThreads, [8] nTimes, [9] buffer, [10] perThreadSize.
 * loop paramv (paramc=12): mainLet's [0..10] prefixed by [0] iter.
 * producer paramv (paramc=3): [0] iter, [1] tid, [2] perThreadSize.
 * consumer paramv (paramc=8):
 *   [0] iter, [1] tid, [2] mapProdGuid, [3] mapConsGuid,
 *   [4] numThreads, [5] buffer, [6] perThreadSize, [7] nTimes.
 * finalize paramv (paramc=4): [0] numThreads, [1] nTimes,
 *   [2] perThreadSize, [3] arraySize. */
#define MAINLET_PARAMC 11
#define LOOP_PARAMC    12

/* Rank-local timing matrix — allocated only on the rank that runs mainEdt. */
double *times;              /* [numThreads][nTimes] flattened; index via TIMES() */

/* Row-major access to the runtime-sized timing matrix.  Requires nTimes and
 * times to be in scope at each use site. */
#define TIMES(t, k) times[(u64)(t) * nTimes + (u64)(k)]

ocrGuid_t producer(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    double *a = (double *)depv[0].ptr;
    u64 tid = paramv[1];
    u64 perThreadSize = paramv[2];
    u32 i;

    for(i = 0; i<perThreadSize; i++) a[i] = i*i;
    a[0] = tid;
    return depv[0].guid;
}

ocrGuid_t consumer(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    double *a = (double *)depv[0].ptr;
    u32 i;
    double sum = 0.0;
    u64 iter = paramv[0];
    u64 tid  = paramv[1];
    ocrGuid_t mapProdGuid = (ocrGuid_t){.guid = (intptr_t)paramv[2]};
    ocrGuid_t mapConsGuid = (ocrGuid_t){.guid = (intptr_t)paramv[3]};
    u64 numThreads = paramv[4];
    u64 buffer     = paramv[5];
    u64 perThreadSize = paramv[6];
    u64 nTimes     = paramv[7];
    ocrGuid_t evt1 = NULL_GUID;
    ocrGuid_t evt2 = NULL_GUID;

    ocrGuidFromIndex(&evt1, mapProdGuid, buffer*a[0] + iter%buffer);
    ocrGuidFromIndex(&evt2, mapConsGuid, buffer*tid + iter%buffer);
    ocrEventDestroy(evt1);
    ocrEventDestroy(evt2);
    if(tid != (((u64)a[0]+SKEW)%numThreads))
    PRINTF("ID %ld, DB %ld\n", tid, (u64)a[0]);
    for(i = 0; i<perThreadSize; i++) sum += a[i];
    ocrDbDestroy(depv[0].guid);
    if(times) TIMES(tid, iter) = mysecond() - TIMES(tid, iter);
    if(sum == 0.0) PRINTF("Hello\n");
    return NULL_GUID;
}

int printTimes(u64 numThreads, u64 nTimes, u64 perThreadSize, u64 arraySize);
ocrGuid_t finalize(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
  printTimes(paramv[0], paramv[1], paramv[2], paramv[3]);
  ocrShutdown();
  return NULL_GUID;
}

ocrGuid_t loop(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  u64 iter = paramv[0];
  u64 tid  = paramv[1];
  ocrGuid_t evt_finalize = (ocrGuid_t){.guid = (intptr_t)paramv[2]};
  ocrGuid_t mapProdGuid  = (ocrGuid_t){.guid = (intptr_t)paramv[3]};
  ocrGuid_t mapConsGuid  = (ocrGuid_t){.guid = (intptr_t)paramv[4]};
  ocrGuid_t tmp_producer = (ocrGuid_t){.guid = (intptr_t)paramv[5]};
  ocrGuid_t tmp_consumer = (ocrGuid_t){.guid = (intptr_t)paramv[6]};
  ocrGuid_t tmp_loop     = (ocrGuid_t){.guid = (intptr_t)paramv[7]};
  u64 numThreads    = paramv[8];
  u64 nTimes        = paramv[9];
  u64 buffer        = paramv[10];
  u64 perThreadSize = paramv[11];

  ocrGuid_t db_a;
  double *a;

  ocrDbCreate(&db_a, (void **)&a, sizeof(double)*perThreadSize,
                             FLAGS, NULL_HINT, NO_ALLOC);

  if(times) TIMES(tid, iter) = mysecond();
  ocrGuid_t prod_output, cons_output;
  ocrGuid_t edt_prod, edt_cons;
  ocrGuid_t evt1 = NULL_GUID;
  ocrGuid_t evt2 = NULL_GUID;
  ocrGuid_t evt3 = NULL_GUID;

  u64 cons_param[8] = { iter, tid, (u64)mapProdGuid.guid, (u64)mapConsGuid.guid,
                        numThreads, buffer, perThreadSize, nTimes };
  u64 prod_param[3] = { iter, tid, perThreadSize };

  // Phase 1: Create all EDTs and labeled events (no triggering deps yet).
  ocrEdtCreate(&edt_cons, tmp_consumer, EDT_PARAM_DEF, cons_param, EDT_PARAM_DEF, NULL, PROPERTIES, NULL_HINT, &cons_output);

  ocrEdtCreate(&edt_prod, tmp_producer, EDT_PARAM_DEF, prod_param, EDT_PARAM_DEF, NULL, PROPERTIES, NULL_HINT, &prod_output);

  ocrGuidFromIndex(&evt1, mapProdGuid, buffer*tid + iter%buffer);
  ocrEventCreate(&evt1, OCR_EVENT_STICKY_T, GUID_PROP_IS_LABELED | GUID_PROP_CHECK | EVT_PROP_TAKES_ARG);

  ocrGuidFromIndex(&evt2, mapConsGuid, buffer*((tid+SKEW) % numThreads) + iter%buffer);
  ocrEventCreate(&evt2, OCR_EVENT_STICKY_T, GUID_PROP_IS_LABELED | GUID_PROP_CHECK | EVT_PROP_TAKES_ARG);

  ocrGuidFromIndex(&evt3, mapConsGuid, buffer*tid + iter%buffer);
  ocrEventCreate(&evt3, OCR_EVENT_STICKY_T, GUID_PROP_IS_LABELED | GUID_PROP_CHECK | EVT_PROP_TAKES_ARG);

  // Phase 2: wire cons_output's waiters first — it is a ONCE event, so all
  // its uses must be registered before any triggering dependency.
  u64 nextIter = iter + 1;
  if (nextIter < nTimes) {  // Spawn another set
    ocrGuid_t edt_loop;
    u64 next_param[LOOP_PARAMC] = {
      nextIter, tid, paramv[2], paramv[3], paramv[4], paramv[5], paramv[6],
      paramv[7], numThreads, nTimes, buffer, perThreadSize
    };
    ocrEdtCreate(&edt_loop, tmp_loop, EDT_PARAM_DEF, next_param, EDT_PARAM_DEF, NULL, PROPERTIES, NULL_HINT, NULL);

    ocrAddDependence(cons_output, edt_loop, 0, DB_MODE_NULL);
  } else {
      ocrAddDependence(cons_output, evt_finalize, 0, DB_DEFAULT_MODE);
  }

  // Phase 3: Wire non-triggering event chains.
  ocrAddDependence(prod_output, evt1, 0, DB_MODE_RO);
  ocrAddDependence(evt1, evt2, 0, DB_MODE_RO);

  // Phase 4: Wire triggering deps LAST (these can fire EDTs immediately).
  // evt3 is a cross-thread labeled STICKY that may already be satisfied.
  ocrAddDependence(evt3, edt_cons, 0, DB_MODE_RO);
  ocrAddDependence(db_a, edt_prod, 0, DB_MODE_RW);

  return NULL_GUID;

}

ocrGuid_t mainLet(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    ocrGuid_t edt_loop, output_evt;
    ocrGuid_t tmp_loop = (ocrGuid_t){.guid = (intptr_t)paramv[6]};

    u64 loop_param[LOOP_PARAMC] = {
      0 /*iter*/, paramv[0] /*tid*/,
      paramv[1] /*evt_finalize*/,
      paramv[2] /*mapProdGuid*/,
      paramv[3] /*mapConsGuid*/,
      paramv[4] /*tmp_producer*/,
      paramv[5] /*tmp_consumer*/,
      paramv[6] /*tmp_loop*/,
      paramv[7] /*numThreads*/,
      paramv[8] /*nTimes*/,
      paramv[9] /*buffer*/,
      paramv[10] /*perThreadSize*/
    };
    ocrEdtCreate(&edt_loop, tmp_loop, EDT_PARAM_DEF, loop_param, EDT_PARAM_DEF, NULL, EDT_PROP_FINISH, NULL_HINT, &output_evt);
    ocrAddDependence(NULL_GUID, edt_loop, 0, DB_MODE_NULL);

    return output_evt;
}

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{

    // Create NUM_THREADS sets of datablocks (3/each)
    // Initialize them
    // Spawn the following tasks in a loop
    // 1. Producer
    // 2. Consumer

    ocrGuid_t tmp_mainLet;
    ocrGuid_t edt_mainLet;
    u64 i;
    ocrGuid_t tmp_finalize, edt_finalize;
    ocrGuid_t tmp_producer, tmp_consumer, tmp_loop;
    ocrGuid_t mapProdGuid = NULL_GUID;
    ocrGuid_t mapConsGuid = NULL_GUID;

    // Positional argv: [numThreads] [arraySize] [nTimes]; absent args keep the #define defaults.
    u64 numThreads    = NUM_THREADS;
    u64 arraySize     = ARRAY_SIZE;
    u64 nTimes        = NTIMES;
    u64 buffer        = BUFFER;
    u64 perThreadSize = PER_THREAD_SIZE;
    if(depc >= 1 && depv[0].ptr) {
        u64 argc = getArgc(depv[0].ptr);
        if(argc > 1) numThreads = strtoull(getArgv(depv[0].ptr, 1), NULL, 10);
        if(argc > 2) arraySize  = strtoull(getArgv(depv[0].ptr, 2), NULL, 10);
        if(argc > 3) nTimes     = strtoull(getArgv(depv[0].ptr, 3), NULL, 10);
    }
    if(numThreads == 0) numThreads = NUM_THREADS;   // guard degenerate counts
    if(arraySize == 0)  arraySize  = ARRAY_SIZE;
    if(nTimes == 0)     nTimes     = NTIMES;
    buffer = numThreads;                            // BUFFER is defined as NUM_THREADS
    if(arraySize % numThreads != 0)
        fprintf(stderr,
                "prodcon: arraySize %llu not a multiple of numThreads %llu; "
                "truncating to %llu elements/thread (%llu total)\n",
                (unsigned long long)arraySize, (unsigned long long)numThreads,
                (unsigned long long)(arraySize / numThreads),
                (unsigned long long)((arraySize / numThreads) * numThreads));
    perThreadSize = arraySize / numThreads;
    times = (double *)calloc(numThreads * nTimes, sizeof(double));

    ocrGuidRangeCreate(&mapProdGuid, buffer*numThreads, GUID_USER_EVENT_STICKY);
    ocrGuidRangeCreate(&mapConsGuid, buffer*numThreads, GUID_USER_EVENT_STICKY);

    ocrEdtTemplateCreate(&tmp_mainLet, mainLet, MAINLET_PARAMC, 0);

    // Spawn the threads
    ocrEdtTemplateCreate(&tmp_loop, loop, LOOP_PARAMC, 1);
    ocrEdtTemplateCreate(&tmp_producer, producer, 3, 1);
    ocrEdtTemplateCreate(&tmp_consumer, consumer, 8, 1);

    ocrEdtTemplateCreate(&tmp_finalize, finalize, 4, numThreads);
    u64 fin_param[4] = { numThreads, nTimes, perThreadSize, arraySize };
    ocrEdtCreate(&edt_finalize, tmp_finalize, EDT_PARAM_DEF, fin_param, EDT_PARAM_DEF, NULL, PROPERTIES, NULL_HINT, NULL);

    for(i = 0; i<numThreads; i++) {
        ocrGuid_t evt_finalize_i;
        ocrEventCreate(&evt_finalize_i, OCR_EVENT_ONCE_T, true);
        ocrAddDependence(evt_finalize_i, edt_finalize, i, DB_MODE_RO);

        u64 mainLet_param[MAINLET_PARAMC] = {
          i /*tid*/,
          (u64)evt_finalize_i.guid,
          (u64)mapProdGuid.guid,
          (u64)mapConsGuid.guid,
          (u64)tmp_producer.guid,
          (u64)tmp_consumer.guid,
          (u64)tmp_loop.guid,
          numThreads, nTimes, buffer, perThreadSize
        };
        ocrEdtCreate(&edt_mainLet, tmp_mainLet, EDT_PARAM_DEF, mainLet_param, EDT_PARAM_DEF, NULL, PROPERTIES, NULL_HINT, NULL);
    }

    return NULL_GUID;
}

int printTimes(u64 numThreads, u64 nTimes, u64 perThreadSize, u64 arraySize)
{
    double avgtime = 0.0;
    double mintime = FLT_MAX;
    double maxtime = 0.0;
    int j, k;

    for (k=1; k<nTimes; k++) /* note -- skip first iteration */
        {
        for (j=0; j<numThreads; j++)
            {
            avgtime = avgtime + TIMES(j, k);
            mintime = MIN(mintime, TIMES(j, k));
            maxtime = MAX(maxtime, TIMES(j, k));
            }
        }

    PRINTF("%f MB/s %f MB/s %f %f %f\n", 1.0e-06*nTimes*8*perThreadSize/mintime, 1.0e-06*8*arraySize*nTimes/avgtime, avgtime/((nTimes-1)*numThreads), mintime, maxtime);

    return 0;

}
