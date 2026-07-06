/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#include <ocr.h>
#include <stdlib.h>
#include <sys/time.h>

/* Compile-time defaults; overridable per-run through positional argv. */
#if !defined(NTHREADS)
#define NTHREADS 16
#endif
#if !defined(DBSIZE)
#define DBSIZE   64*1024*1024
#endif
#if !defined(ITER)
#define ITER     200
#endif

struct timeval tv1, tv2;

ocrGuid_t done(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    gettimeofday(&tv2, NULL);
    PRINTF("Time: %d ms\n", ((tv2.tv_sec-tv1.tv_sec)*1000000 + (tv2.tv_usec - tv1.tv_usec))/1000);
    ocrShutdown();
    return NULL_GUID;
}

ocrGuid_t work(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 dbsize = paramv[0];
    u64 iter   = paramv[1];
    u64 i, j;
    u8 *ptr = depv[0].ptr;

    /* ocrDbCreate does not zero the payload; the checksum below needs a
     * known starting value. */
    for(j = 0; j<dbsize; j++) ptr[j] = 0;

    for(i = 0; i<iter; i++) {
        for(j = 0; j<dbsize; j++) {
            ptr[j]++ ;
        }
    }
    /* Checksum makes the increments observable (defeats dead-store
     * elimination); per-EDT sum is iter * dbsize while iter < 256. */
    u64 my_sum = 0;
    for(j = 0; j<dbsize; j++) {
        my_sum += ptr[j];
    }
    PRINTF("HIGHBW_WORK_SUM = %llu\n", (unsigned long long)my_sum);
    return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrGuid_t twork, tdone, workedt, doneedt, event;
    void *ptr;
    u64 i;

    // Positional argv: [nThreads] [dbSize] [iter]; absent args keep the #define defaults.
    u64 nThreads = NTHREADS;
    u64 dbSize   = DBSIZE;
    u64 nIter    = ITER;
    if(depc >= 1 && depv[0].ptr) {
        u64 argc = getArgc(depv[0].ptr);
        if(argc > 1) nThreads = strtoull(getArgv(depv[0].ptr, 1), NULL, 10);
        if(argc > 2) dbSize   = strtoull(getArgv(depv[0].ptr, 2), NULL, 10);
        if(argc > 3) nIter    = strtoull(getArgv(depv[0].ptr, 3), NULL, 10);
    }
    if(nThreads == 0) nThreads = NTHREADS;   // guard degenerate counts
    if(dbSize == 0)   dbSize   = DBSIZE;

    // Per-thread DB guids: heap-sized since the count is now a runtime value.
    ocrGuid_t *dbs = (ocrGuid_t *)malloc(sizeof(ocrGuid_t) * nThreads);

    gettimeofday(&tv1, NULL);

    ocrHint_t myHint;
    ocrHintInit(&myHint, OCR_HINT_DB_T);
    ocrSetHintValue(&myHint, OCR_HINT_DB_HIGHBW, 1);

    for(i = 0; i < nThreads; i++) ocrDbCreate(&dbs[i], &ptr, sizeof(char)*dbSize, DB_PROP_NONE, &myHint, NO_ALLOC);

    // Forward dbSize / iter to work through paramv so they reach the EDT on
    // whichever node it runs (file-scope globals would not cross ranks).
    ocrEdtTemplateCreate(&twork  , work  , 2, 1);
    ocrEdtTemplateCreate(&tdone , done , 0, nThreads);
    ocrEdtCreate(&doneedt, tdone , 0, NULL, nThreads, NULL, EDT_PROP_NONE, NULL_HINT, NULL);
    for(i = 0; i < nThreads; i++) {
        u64 wparamv[2] = { dbSize, nIter };
        ocrEdtCreate(&workedt, twork, EDT_PARAM_DEF, wparamv, 1, &dbs[i], EDT_PROP_NONE, NULL_HINT, &event);
        ocrAddDependence(event, doneedt, i, DB_MODE_NULL);
    }

    free(dbs);
    return NULL_GUID;
}
