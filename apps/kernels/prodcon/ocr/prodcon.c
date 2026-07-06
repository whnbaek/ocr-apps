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

/* Runtime-resolved sizes (positional argv in mainEdt); defaults mirror the
 * compile-time macros so an argument-free run behaves identically. */
u64 numThreads    = NUM_THREADS;
u64 arraySize     = ARRAY_SIZE;
u64 nTimes        = NTIMES;
u64 buffer        = BUFFER;
u64 perThreadSize = PER_THREAD_SIZE;

/* TODO: get rid of ugly globals below */
double *times;              /* [numThreads][nTimes] flattened; index via TIMES() */
ocrGuid_t tmp_producer, tmp_consumer, tmp_loop;
ocrGuid_t *evt_finalize;    /* [numThreads] */

ocrGuid_t mapProdGuid = NULL_GUID;
ocrGuid_t mapConsGuid = NULL_GUID;
/* End of ugly globals */

/* Row-major access to the runtime-sized timing matrix. */
#define TIMES(t, k) times[(u64)(t) * nTimes + (u64)(k)]

ocrGuid_t producer(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    double *a = (double *)depv[0].ptr;
    u32 i;

    for(i = 0; i<perThreadSize; i++) a[i] = i*i;
    a[0] = paramv[1];
    return depv[0].guid;
}

ocrGuid_t consumer(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    double *a = (double *)depv[0].ptr;
    u32 i;
    double sum = 0.0;
    ocrGuid_t evt1 = NULL_GUID;
    ocrGuid_t evt2 = NULL_GUID;

    ocrGuidFromIndex(&evt1, mapProdGuid, buffer*a[0] + paramv[0]%buffer);
    ocrGuidFromIndex(&evt2, mapConsGuid, buffer*paramv[1] + paramv[0]%buffer);
    ocrEventDestroy(evt1);
    ocrEventDestroy(evt2);
    if(paramv[1] != (((u64)a[0]+SKEW)%numThreads))
    PRINTF("ID %ld, DB %ld\n", paramv[1], (u64)a[0]);
    for(i = 0; i<perThreadSize; i++) sum += a[i];
    ocrDbDestroy(depv[0].guid);
    TIMES(paramv[1], paramv[0]) = mysecond() - TIMES(paramv[1], paramv[0]);
    if(sum == 0.0) PRINTF("Hello\n");
    return NULL_GUID;
}

int printTimes(void);
ocrGuid_t finalize(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
  printTimes();
  ocrShutdown();
  return NULL_GUID;
}

ocrGuid_t loop(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{

  u64 iter = paramv[0];
  u64 param[2] = { iter, paramv[1] };
  ocrGuid_t db_a;
  double *a;

  ocrDbCreate(&db_a, (void **)&a, sizeof(double)*perThreadSize,
                             FLAGS, NULL_HINT, NO_ALLOC);

  TIMES(paramv[1], iter) = mysecond();
  ocrGuid_t prod_output, cons_output;
  ocrGuid_t edt_prod, edt_cons;
  ocrGuid_t evt1 = NULL_GUID;
  ocrGuid_t evt2 = NULL_GUID;
  ocrGuid_t evt3 = NULL_GUID;

  // Phase 1: Create all EDTs and labeled events (no triggering deps yet).
  ocrEdtCreate(&edt_cons, tmp_consumer, EDT_PARAM_DEF, param, EDT_PARAM_DEF, NULL, PROPERTIES, NULL_HINT, &cons_output);

  ocrEdtCreate(&edt_prod, tmp_producer, EDT_PARAM_DEF, param, EDT_PARAM_DEF, NULL, PROPERTIES, NULL_HINT, &prod_output);

  ocrGuidFromIndex(&evt1, mapProdGuid, buffer*paramv[1] + paramv[0]%buffer);
  ocrEventCreate(&evt1, OCR_EVENT_STICKY_T, GUID_PROP_IS_LABELED | GUID_PROP_CHECK | EVT_PROP_TAKES_ARG);

  ocrGuidFromIndex(&evt2, mapConsGuid, buffer*((paramv[1]+SKEW) % numThreads) + paramv[0]%buffer);
  ocrEventCreate(&evt2, OCR_EVENT_STICKY_T, GUID_PROP_IS_LABELED | GUID_PROP_CHECK | EVT_PROP_TAKES_ARG);

  ocrGuidFromIndex(&evt3, mapConsGuid, buffer*paramv[1] + paramv[0]%buffer);
  ocrEventCreate(&evt3, OCR_EVENT_STICKY_T, GUID_PROP_IS_LABELED | GUID_PROP_CHECK | EVT_PROP_TAKES_ARG);

  // Phase 2: wire cons_output's waiters first — it is a ONCE event, so all
  // its uses must be registered before any triggering dependency.
  iter++;
  param[0] = iter;
  if (iter < nTimes) {  // Spawn another set
    ocrGuid_t edt_loop;
    param[1] = paramv[1];
    ocrEdtCreate(&edt_loop, tmp_loop, EDT_PARAM_DEF, param, EDT_PARAM_DEF, NULL, PROPERTIES, NULL_HINT, NULL);

    ocrAddDependence(cons_output, edt_loop, 0, DB_MODE_NULL);
  } else {
      u64 tid = paramv[1];
      ocrAddDependence(cons_output, evt_finalize[tid], 0, DB_DEFAULT_MODE);
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

    u64 param[2] = { 0, paramv[0] };
    ocrEdtCreate(&edt_loop, tmp_loop, EDT_PARAM_DEF, param, EDT_PARAM_DEF, NULL, EDT_PROP_FINISH, NULL_HINT, &output_evt);
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

    // Positional argv: [numThreads] [arraySize] [nTimes]; absent args keep the #define defaults.
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
    evt_finalize = (ocrGuid_t *)malloc(numThreads * sizeof(ocrGuid_t));

    ocrGuidRangeCreate(&mapProdGuid, buffer*numThreads, GUID_USER_EVENT_STICKY);
    ocrGuidRangeCreate(&mapConsGuid, buffer*numThreads, GUID_USER_EVENT_STICKY);

    ocrEdtTemplateCreate(&tmp_mainLet, mainLet, 1, 0);

    // Spawn the threads
    ocrEdtTemplateCreate(&tmp_loop, loop, 2, 1);
    ocrEdtTemplateCreate(&tmp_producer, producer, 2, 1);
    ocrEdtTemplateCreate(&tmp_consumer, consumer, 2, 1);

    ocrEdtTemplateCreate(&tmp_finalize, finalize, 0, numThreads);
    ocrEdtCreate(&edt_finalize, tmp_finalize, EDT_PARAM_DEF, NULL, EDT_PARAM_DEF, NULL, PROPERTIES, NULL_HINT, NULL);

    for(i = 0; i<numThreads; i++) {
        ocrEventCreate(&evt_finalize[i], OCR_EVENT_ONCE_T, true);
        ocrAddDependence(evt_finalize[i], edt_finalize, i, DB_MODE_RO);
        ocrEdtCreate(&edt_mainLet, tmp_mainLet, EDT_PARAM_DEF, &i, EDT_PARAM_DEF, NULL, PROPERTIES, NULL_HINT, NULL);
    }

    return NULL_GUID;
}

int printTimes(void)
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
