/*
 * fib.c
 * Does not use finish EDTs
 * Originally written in Feb 2012 by Justin Teller
 * Modified for OCR 0.9 by Romain Cledat
 */

#include "ocr.h"
#include "ocr-std.h"
#include "macros.h"
#include "extensions/ocr-affinity.h"


#include "stdlib.h"

/* Placement-optimization layer: with OCR_APP_OPTIMIZED_PLACEMENT defined the
 * helpers below fill *h and return it; otherwise they return NULL_HINT and
 * the application keeps its as-born placement. */

/* Top recursion levels whose EDTs are round-robin distributed across policy
 * domains keyed on a deterministic path id; below this the whole subtree pins
 * to the creating rank and runs wire-free.  The depth is an argument, not a
 * constant: it trades scatter against the migrations a subtree would have
 * amortised locally, so it is calibrated by measurement.  This is only the
 * default when the argument is absent. */
#ifndef FIB_RR_LEVELS
#define FIB_RR_LEVELS 11
#endif

typedef struct {
    ocrGuid_t completeGuid;
    u64 level;
    u64 pathId;
    u64 rrLevels;
} fibPRM_t;

typedef struct {
    ocrGuid_t depGuid;
} completePRM_t;

typedef struct {
    u64 correctAns;
}absFinalPRM_t;

/* Finalizer-style bit mix: the path id is the branch sequence read as a
 * binary number, so its low bits are the most recent branches and a raw
 * modulus aliases with the tree's own shape -- sibling subtrees of very
 * different size land on the same rank.  Mixing first breaks the alias. */
static inline u64 fibMixKey(u64 x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/* Pin the sum EDT to the creating rank. */
static ocrHint_t * fibLocalEdtHint(ocrHint_t * h) {
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
    ocrGuid_t aff;
    ocrAffinityGetCurrent(&aff);
    ocrHintInit(h, OCR_HINT_EDT_T);
    ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
#else
    (void)h;
    return NULL_HINT;
#endif
}

/* Top-level children scatter round-robin on their path id; deeper children
 * stay on the creating rank so the subtree beneath them is wire-free. */
static ocrHint_t * fibChildEdtHint(ocrHint_t * h, u64 childLevel, u64 childPath,
                                   u64 rrLevels) {
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
    u64 nranks;
    ocrAffinityCount(AFFINITY_PD, &nranks);
    ocrGuid_t aff;
    if (childLevel <= rrLevels)
        ocrAffinityGetAt(AFFINITY_PD, fibMixKey(childPath) % nranks, &aff);
    else
        ocrAffinityGetCurrent(&aff);
    ocrHintInit(h, OCR_HINT_EDT_T);
    ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
#else
    (void)h; (void)childLevel; (void)childPath; (void)rrLevels;
    return NULL_HINT;
#endif
}

ocrGuid_t complete(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    completePRM_t *completeParamvIn = (completePRM_t *)paramv;
    ocrGuid_t arg = completeParamvIn->depGuid;
    ocrGuid_t inDep;
    u32 in1, in2;
    u32 out;

    inDep = arg;
    /* When we run, we got our inputs from fib(n-1) and fib(n-2) */
    in1 = *(u32*)depv[0].ptr;
    in2 = *(u32*)depv[1].ptr;
    out = *(u32*)depv[2].ptr;
    //ocrPrintf("Done with %d (%d + %d)\n", out, in1, in2);
    /* we return our answer in the 3rd db passed in as an argument */
    *((u32*)(depv[2].ptr)) = in1 + in2;

    /* The app is done with the answers from fib(n-1) and fib(n-2) */
    ocrDbDestroy(depv[0].guid);
    ocrDbDestroy(depv[1].guid);
    ocrDbRelease(depv[2].guid);
    /* and let our parent's completion know we're done with fib(n) */
    ocrEventSatisfy(inDep, depv[2].guid);
    return NULL_GUID;
}

ocrGuid_t fibEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    void* ptr;
    ocrGuid_t inDep;
    ocrGuid_t fib0, fib1, comp;
    ocrGuid_t fibDone[2];
    ocrGuid_t fibArg[2];

    fibPRM_t *fibParamvIn = (fibPRM_t *)paramv;
    inDep = fibParamvIn->completeGuid;
    u64 level = fibParamvIn->level;
    u64 pathId = fibParamvIn->pathId;
    u64 rrLevels = fibParamvIn->rrLevels;

    u32 n = *(u32*)(depv[0].ptr);
    /* This EDT only reads the argument block; release it before any
     * satisfy/add-dependence exposes it — the release is the publication
     * point consumers are entitled to observe. */
    ocrDbRelease(depv[0].guid);
    //ocrPrintf("Starting fibEdt(%u)\n", n);
    if (n < 2) {
        //ocrPrintf("In fibEdt(%d) -- done (sat "GUIDF")\n", n, GUIDA(inDep));
        ocrEventSatisfy(inDep, depv[0].guid);
        return NULL_GUID;
    }
    //ocrPrintf("In fibEdt(%d) -- spawning children\n", n);

    completePRM_t completeParamv;
    /* create the completion EDT and pass it the in/out argument as a dependency */
    /* create the EDT with the done_event as the argument */
    {
        completeParamv.depGuid = inDep;
        ocrGuid_t templateGuid;
        ocrEdtTemplateCreate(&templateGuid, complete, PRMNUM(complete), 3);
        ocrHint_t compHint;
        ocrEdtCreate(&comp, templateGuid, PRMNUM(complete), (u64 *)&completeParamv, 3, NULL, EDT_PROP_NONE,
                     fibLocalEdtHint(&compHint), NULL);
        ocrEdtTemplateDestroy(templateGuid);
    }
    //ocrPrintf("In fibEdt(%u) -- spawned complete EDT GUID 0x%llx\n", n, (u64)comp);
    ocrAddDependence(depv[0].guid, comp, 2, DB_DEFAULT_MODE);

    /* create the events that the completion EDT will "wait" on */
    ocrEventCreate(&fibDone[0], OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);
    ocrEventCreate(&fibDone[1], OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);
    /* Slots 0/1 deliver the children's result blocks, which complete() only
     * reads: the mode declares that intent. */
    ocrAddDependence(fibDone[0], comp, 0, DB_MODE_RO);
    ocrAddDependence(fibDone[1], comp, 1, DB_MODE_RO);
    /* allocate the argument to pass to fib(n-1) */

    ocrDbCreate(&fibArg[0], (void**)&ptr, sizeof(u32), DB_PROP_NONE, NULL_HINT, NO_ALLOC);
    //ocrPrintf("In fibEdt(%u) -- created arg DB GUID "GUIDF"\n", n, GUIDA(fibArg[0]));
    *((u32*)ptr) = n-1;
    ocrDbRelease(fibArg[0]);
    /* sched the EDT, passing the fibDone event as it's argument */
    fibPRM_t fibParamv0;
    {
        fibParamv0.completeGuid = fibDone[0];
        fibParamv0.level = level + 1;
        fibParamv0.pathId = pathId * 2 + 0;
        fibParamv0.rrLevels = rrLevels;

        ocrGuid_t templateGuid;
        ocrEdtTemplateCreate(&templateGuid, fibEdt, PRMNUM(fib), 1);
        ocrHint_t fib0Hint;
        ocrEdtCreate(&fib0, templateGuid, PRMNUM(fib), (u64 *)&fibParamv0, 1, NULL, EDT_PROP_NONE,
                     fibChildEdtHint(&fib0Hint, fibParamv0.level, fibParamv0.pathId,
                                     rrLevels), NULL);
        ocrEdtTemplateDestroy(templateGuid);
        /* The child only reads its argument; wire it RO, after the release
         * above published the value. */
        ocrAddDependence(fibArg[0], fib0, 0, DB_MODE_RO);
    }

    //ocrPrintf("In fibEdt(%u) -- spawned first sub-part EDT GUID "GUIDF"\n", n, GUIDA(fib0));
    /* then do the exact same thing for n-2 */
    ocrDbCreate(&fibArg[1], (void**)&ptr, sizeof(u32), DB_PROP_NONE, NULL_HINT, NO_ALLOC);
    //ocrPrintf("In fibEdt(%u) -- created arg DB GUID "GUIDF"\n", n, GUIDA(fibArg[1]));
    *((u32*)ptr) = n-2;
    ocrDbRelease(fibArg[1]);
    fibPRM_t fibParamv1;
    {
        fibParamv1.completeGuid = fibDone[1];
        fibParamv1.level = level + 1;
        fibParamv1.pathId = pathId * 2 + 1;
        fibParamv1.rrLevels = rrLevels;

        ocrGuid_t templateGuid;
        ocrEdtTemplateCreate(&templateGuid, fibEdt, PRMNUM(fib), 1);
        ocrHint_t fib1Hint;
        ocrEdtCreate(&fib1, templateGuid, PRMNUM(fib), (u64 *)&fibParamv1, 1, NULL, EDT_PROP_NONE,
                     fibChildEdtHint(&fib1Hint, fibParamv1.level, fibParamv1.pathId,
                                     rrLevels), NULL);
        ocrEdtTemplateDestroy(templateGuid);
        /* The child only reads its argument; wire it RO, after the release
         * above published the value. */
        ocrAddDependence(fibArg[1], fib1, 0, DB_MODE_RO);
    }
    //ocrPrintf("In fibEdt(%u) -- spawned first sub-part EDT GUID "GUIDF"\n", n, GUIDA(fib1));

    //ocrPrintf("Returning from fibEdt(%u)\n", n);
    return NULL_GUID;

}

ocrGuid_t absFinal(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    u32 ans;
    ans = *(u32*)depv[0].ptr;
    absFinalPRM_t *absFinalParamvIn = (absFinalPRM_t *)paramv;
    u32 correctAns = (u32) absFinalParamvIn->correctAns;
    VERIFY(ans == correctAns, "Totally done: answer is %d\n", ans);
    ocrDbDestroy(depv[0].guid);
    ocrShutdown();

    return NULL_GUID;
}

u64 fib(u32 n)
{
    if(n<=0) return 0;
    if(n<=2) return 1;
    else return fib(n-1) + fib(n-2);
}

/* just define the main EDT function */
ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrPrintf("Starting mainEdt\n");
    u32 input;
    u64 rrLevels = FIB_RR_LEVELS;
    u32 argc = ocrGetArgc(depv[0].ptr);
    if(argc < 2 || argc > 3) {
        ocrPrintf("Usage: fib <num> [scatter-levels], defaulting to 10\n");
        input = 10;
    } else {
        input = atoi(ocrGetArgv(depv[0].ptr, 1));
        if(argc == 3)
            rrLevels = strtoull(ocrGetArgv(depv[0].ptr, 2), NULL, 10);
    }

    ocrGuid_t fibC, totallyDoneEvent, absFinalEdt, templateGuid;

    absFinalPRM_t absFinalParamv;
    absFinalParamv.correctAns = fib(input);

    {
        ocrGuid_t templateGuid;
        ocrEdtTemplateCreate(&templateGuid, absFinal, PRMNUM(absFinal), 1);
        ocrPrintf("Created template and got GUID "GUIDF"\n", GUIDA(templateGuid));
        ocrEdtCreate(&absFinalEdt, templateGuid, PRMNUM(absFinal), (u64 *)&absFinalParamv, 1, NULL, EDT_PROP_NONE,
                     NULL_HINT, NULL);
        ocrPrintf("Created ABS EDT and got  GUID "GUIDF"\n", GUIDA(absFinalEdt));
        ocrEdtTemplateDestroy(templateGuid);
    }

    /* create a db for the results */
    ocrGuid_t fibArg;
    u32* res;

    ocrPrintf("Before 1st DB create\n");
    ocrDbCreate(&fibArg, (void**)&res, sizeof(u32), DB_PROP_NONE, NULL_HINT, NO_ALLOC);
    ocrPrintf("Got DB created\n");

    /* DB is in/out */
    *res = input;
    ocrDbRelease(fibArg);
    /* and an event for when the results are finished */
    ocrEventCreate(&totallyDoneEvent, OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG);
    /* The final checker only reads the delivered result block. */
    ocrAddDependence(totallyDoneEvent, absFinalEdt, 0, DB_MODE_RO);

    fibPRM_t fibParamv;

    /* create the EDT with the done_event as the argument */
    {
        fibParamv.completeGuid = totallyDoneEvent;
        fibParamv.level = 0;
        fibParamv.pathId = 0;
        fibParamv.rrLevels = rrLevels;

        ocrGuid_t templateGuid;
        ocrEdtTemplateCreate(&templateGuid, fibEdt, PRMNUM(fib), 1);
        ocrEdtCreate(&fibC, templateGuid, PRMNUM(fib), (u64 *)&fibParamv, 1, NULL, EDT_PROP_NONE,
                     NULL_HINT, NULL);
        ocrEdtTemplateDestroy(templateGuid);
        /* The child only reads its argument; wire it RO, after the release
         * above published the value. */
        ocrAddDependence(fibArg, fibC, 0, DB_MODE_RO);
    }

    return NULL_GUID;
}
