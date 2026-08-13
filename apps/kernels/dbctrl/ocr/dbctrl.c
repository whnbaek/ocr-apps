/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ocr.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>

#ifndef ENABLE_EXTENSION_LABELING
#define ENABLE_EXTENSION_LABELING
#endif
#include "extensions/ocr-labeling.h"

#include <math.h>
#include <float.h>

#if 0
#define FANOUT  100
#define DEPTH 200
#define TOTALDB (FANOUT*DEPTH)
#endif

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

/* All cross-EDT values travel through paramv (deep-copied at EDT create) or a
 * data block payload.  File-scope statics are per-process under a fork-launched
 * multi-node runtime: only the rank that runs mainEdt initializes them, so a
 * child EDT scheduled on a remote rank would observe zero-init BSS.
 *
 * generate paramv (paramc=8):
 *   [0] depth
 *   [1] fanout, [2] depthMax, [3] dbSize   (runtime workload sizes)
 *   [4] startTimeBits  (start time as double bit-pattern)
 *   [5] tmp_create, [6] tmp_generate, [7] tmp_destroy  (ocrEdtTemplate GUIDs)
 * generate depv[0] = evtMap data block (RO array of event GUIDs).
 *
 * create paramv (paramc=1): [0] dbSize.
 *
 * destroy paramv (paramc=6):
 *   [0] depth, [1] inst, [2] eventGuid (the evtMap entry to destroy),
 *   [3] fanout, [4] depthMax, [5] startTimeBits.
 * destroy depv[0] = the data block delivered through that event. */
#define GENERATE_PARAMC 8
#define DESTROY_PARAMC  6

ocrGuid_t destroy(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    u64 depth    = paramv[0];
    u64 inst     = paramv[1];
    ocrGuid_t evt = (ocrGuid_t){.guid = (intptr_t)paramv[2]};
    u64 fanout   = paramv[3];
    u64 depthMax = paramv[4];
    u64 startBits = paramv[5];

    ocrDbDestroy(depv[0].guid);
    ocrEventDestroy(evt);
    if((depth == depthMax+1) && (inst == fanout-1)) {
        double start_time;
        memcpy(&start_time, &startBits, sizeof(double));
        PRINTF("Total time %f\n", mysecond()-start_time);
        ocrShutdown();
    }

    return NULL_GUID;
}

ocrGuid_t create(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    ocrGuid_t db;
    void *ptr;
    ocrDbCreate(&db, &ptr, paramv[0], DB_PROP_NONE, NULL_HINT, NO_ALLOC);
    return db;
}

ocrGuid_t generate(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    u32 i, j;
    u64 depth     = paramv[0];
    u64 fanout    = paramv[1];
    u64 depthMax  = paramv[2];
    u64 dbSize    = paramv[3];
    u64 startBits = paramv[4];

    if(depth > depthMax) return NULL_GUID;

    ocrGuid_t *evtMap = (ocrGuid_t *)depv[0].ptr;
    ocrGuid_t evtMapDb = depv[0].guid;

    u64 delta = fanout - depth * fanout/depthMax;
    u64 nextDepth = depth + 1;
    ocrGuid_t scratchEdt;

    u64 create_param[1] = { dbSize };
    for(i = 0; i < delta; i++) {
        ocrGuid_t outputEvt;
        u32 index = 0;
        for(j = 0; j < depth; j++) index += (fanout-j*fanout/depthMax); index += i;
        ocrEdtCreate(&scratchEdt, (ocrGuid_t){.guid = (intptr_t)paramv[5]}, EDT_PARAM_DEF, create_param, EDT_PARAM_DEF, NULL, PROPERTIES, NULL_HINT, &outputEvt);
        ocrAddDependence(outputEvt, evtMap[index], 0, DB_MODE_RW);
        ocrAddDependence(NULL_GUID, scratchEdt, 0, DB_MODE_NULL);
    }

    for(i = delta; i<fanout; i++) {
        u32 index = 0;
        for(j = 0; j < depth; j++) index += j*fanout/depthMax; index += (i-delta);
        u64 destroy_param[DESTROY_PARAMC] = {
            nextDepth, i, (u64)evtMap[index].guid,
            fanout, depthMax, startBits
        };
        ocrEdtCreate(&scratchEdt, (ocrGuid_t){.guid = (intptr_t)paramv[7]}, EDT_PARAM_DEF, destroy_param, EDT_PARAM_DEF, NULL, PROPERTIES, NULL_HINT, NULL);
        ocrAddDependence(evtMap[index], scratchEdt, 0, DB_MODE_RO);
    }

    u64 gen_param[GENERATE_PARAMC] = {
        nextDepth, fanout, depthMax, dbSize, startBits,
        paramv[5], paramv[6], paramv[7]
    };
    ocrEdtCreate(&scratchEdt, (ocrGuid_t){.guid = (intptr_t)paramv[6]}, EDT_PARAM_DEF, gen_param, EDT_PARAM_DEF, NULL, PROPERTIES, NULL_HINT, NULL);
    ocrAddDependence(evtMapDb, scratchEdt, 0, DB_MODE_RO);

    return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    u64 i;
    ocrGuid_t genEdt;
    ocrGuid_t tmp_create, tmp_generate, tmp_destroy;

    u64 fanout, depthMax, dbSize, totaldb;

    i = getArgc(depv[0].ptr);
    fanout   = atoi(getArgv(depv[0].ptr, 1));
    depthMax = atoi(getArgv(depv[0].ptr, 2));
    dbSize   = atoi(getArgv(depv[0].ptr, 3));
    totaldb  = fanout*depthMax;

    // Spawn the threads
    ocrEdtTemplateCreate(&tmp_generate, generate, GENERATE_PARAMC, 1);
    ocrEdtTemplateCreate(&tmp_create, create, 1, 1);
    ocrEdtTemplateCreate(&tmp_destroy, destroy, DESTROY_PARAMC, 1);

    /* The event-GUID table is large; carry it in a data block (array of raw
     * event GUIDs) delivered as a dependence to the EDTs that index it, rather
     * than in a file-scope array. */
    ocrGuid_t evtMapDb;
    ocrGuid_t *evtMap;
    ocrDbCreate(&evtMapDb, (void **)&evtMap, totaldb*sizeof(ocrGuid_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC);
    for(i = 0; i<totaldb; i++) ocrEventCreate(&evtMap[i], OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);

    double start_time = mysecond();
    u64 startBits;
    memcpy(&startBits, &start_time, sizeof(double));

    u64 gen_param[GENERATE_PARAMC] = {
        0 /*depth*/, fanout, depthMax, dbSize, startBits,
        (u64)tmp_create.guid, (u64)tmp_generate.guid, (u64)tmp_destroy.guid
    };
    ocrEdtCreate(&genEdt, tmp_generate, EDT_PARAM_DEF, gen_param, EDT_PARAM_DEF, NULL, PROPERTIES, NULL_HINT, NULL);
    /* The table is fully written: release before the wire exposes it. */
    ocrDbRelease(evtMapDb);
    ocrAddDependence(evtMapDb, genEdt, 0, DB_MODE_RO);

    return NULL_GUID;
}
