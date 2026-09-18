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

/* Top recursion levels whose EDTs are distributed across policy domains;
 * below this the whole subtree pins to the rank it landed on and runs
 * wire-free.  The depth trades scatter against the migrations a subtree would
 * have amortised locally, so it is an argument, not a constant.  This is only
 * the default when the argument is absent. */
#ifndef FIB_RR_LEVELS
#define FIB_RR_LEVELS 14
#endif

/* The result travels in a 32-bit block and is summed as u32, so beyond this
 * input both the tree's answer and the reference wrap and the comparison
 * stops being a check of anything. */
#define FIB_MAX_N 47

/* Below this the root is already a leaf: the run creates no child, computes
 * nothing and would still report a checked answer. */
#define FIB_MIN_N 2

typedef struct {
    ocrGuid_t completeGuid;
    u64 level;
    u64 lo;
    u64 total;
    u64 rrLevels;
} fibPRM_t;

typedef struct {
    ocrGuid_t depGuid;
} completePRM_t;

typedef struct {
    u64 correctAns;
}absFinalPRM_t;

/* A decimal argument, whole-string.  strtoull alone accepts a sign and stops
 * at the first junk character, so a typo would become a silently different
 * run instead of a refusal. */
static u64 fibArgNumber(const char * arg, const char * what) {
    char * end;
    u64 v;
    if (arg[0] == '-' || arg[0] == '+') {
        ocrPrintf("fib: %s '%s' must be a plain non-negative decimal\n", what, arg);
        ocrAbort(1);
    }
    v = strtoull(arg, &end, 10);
    if (end == arg || *end != '\0') {
        ocrPrintf("fib: %s '%s' is not a number\n", what, arg);
        ocrAbort(1);
    }
    return v;
}

/* F(v) with F(0) = 0, F(1) = 1.  The recursion tree for input v has F(v+1)
 * leaves, so the same iteration answers both "what does the tree compute" and
 * "how much work is under this node". */
static u64 fibNumber(u64 v) {
    u64 a = 0, b = 1, i;
    for (i = 0; i < v; ++i) {
        u64 t = a + b;
        a = b;
        b = t;
    }
    return a;
}

/* Order the leaves of the whole tree left to right and give each rank an equal
 * contiguous share of that order; a subtree is placed where its own leaf
 * interval starts.  The interval is exact -- a node of value v owns F(v+1)
 * leaves and its two children split them F(v) / F(v-1) -- so a rank's share of
 * the leaves, and therefore of the work, deviates from an equal share by at
 * most one straddling subtree. */
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
static ocrGuid_t fibChildAffinity(u64 childLevel, u64 lo, u64 total,
                                  u64 rrLevels) {
    ocrGuid_t aff;
    if (childLevel <= rrLevels) {
        u64 nranks;
        ocrAffinityCount(AFFINITY_PD, &nranks);
        ocrAffinityGetAt(AFFINITY_PD, (lo * nranks) / total, &aff);
    } else {
        ocrAffinityGetCurrent(&aff);
    }
    return aff;
}
#endif

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

/* Top-level children go to the rank that owns their leaf interval; deeper
 * children stay on the creating rank so the subtree beneath them is
 * wire-free. */
static ocrHint_t * fibChildEdtHint(ocrHint_t * h, u64 childLevel, u64 lo,
                                   u64 total, u64 rrLevels) {
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
    ocrGuid_t aff = fibChildAffinity(childLevel, lo, total, rrLevels);
    ocrHintInit(h, OCR_HINT_EDT_T);
    ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
#else
    (void)h; (void)childLevel; (void)lo; (void)total; (void)rrLevels;
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
    u64 lo = fibParamvIn->lo;
    u64 total = fibParamvIn->total;
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

    /* [lo, lo + F(n+1)) is this node's leaf interval; the n-1 child takes its
     * first F(n) leaves and the n-2 child the rest.  Only the scatter prefix
     * consults the interval, so it is derived only while it can still be
     * read. */
    u64 childLevel = level + 1;
    u64 lo0 = lo;
    u64 lo1 = lo;
    if (childLevel <= rrLevels)
        lo1 = lo + fibNumber(n);

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
        fibParamv0.level = childLevel;
        fibParamv0.lo = lo0;
        fibParamv0.total = total;
        fibParamv0.rrLevels = rrLevels;

        ocrGuid_t templateGuid;
        ocrEdtTemplateCreate(&templateGuid, fibEdt, PRMNUM(fib), 1);
        ocrHint_t fib0Hint;
        ocrEdtCreate(&fib0, templateGuid, PRMNUM(fib), (u64 *)&fibParamv0, 1, NULL, EDT_PROP_NONE,
                     fibChildEdtHint(&fib0Hint, childLevel, lo0, total, rrLevels), NULL);
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
        fibParamv1.level = childLevel;
        fibParamv1.lo = lo1;
        fibParamv1.total = total;
        fibParamv1.rrLevels = rrLevels;

        ocrGuid_t templateGuid;
        ocrEdtTemplateCreate(&templateGuid, fibEdt, PRMNUM(fib), 1);
        ocrHint_t fib1Hint;
        ocrEdtCreate(&fib1, templateGuid, PRMNUM(fib), (u64 *)&fibParamv1, 1, NULL, EDT_PROP_NONE,
                     fibChildEdtHint(&fib1Hint, childLevel, lo1, total, rrLevels), NULL);
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

/* just define the main EDT function */
ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrPrintf("Starting mainEdt\n");
    u64 num;
    u32 input;
    u64 rrLevels = FIB_RR_LEVELS;
    u32 argc = ocrGetArgc(depv[0].ptr);
    if(argc < 2 || argc > 3) {
        ocrPrintf("Usage: fib <num> [scatter-levels]\n");
        ocrAbort(1);
    }

    num = fibArgNumber(ocrGetArgv(depv[0].ptr, 1), "num");
    if(argc == 3)
        rrLevels = fibArgNumber(ocrGetArgv(depv[0].ptr, 2), "scatter-levels");

    if(num < FIB_MIN_N || num > FIB_MAX_N) {
        ocrPrintf("fib: num %lu is outside [%d, %d]; below that the root is a "
                  "leaf, above it the 32-bit result wraps and the check is "
                  "vacuous\n", (unsigned long)num, FIB_MIN_N, FIB_MAX_N);
        ocrAbort(1);
    }
    input = (u32)num;

    /* Leaves of the whole tree; also the denominator every placement decision
     * divides by, so it travels with each task. */
    u64 total = fibNumber((u64)input + 1);

    /* The scatter frontier must be finer than a rank's share of the leaves,
     * or a rank receives no subtree at all and idles.  The heaviest subtree
     * at depth d is the one reached by taking the n-1 branch every time. */
    u64 nranks;
    ocrAffinityCount(AFFINITY_PD, &nranks);
    if(rrLevels < 1 || rrLevels > num) {
        ocrPrintf("fib: scatter-levels %lu is outside [1, %lu]; depth 0 pins "
                  "the whole recursion to one rank and no level below the "
                  "input exists to scatter\n",
                  (unsigned long)rrLevels, (unsigned long)num);
        ocrAbort(1);
    }
    {
        u64 deepest = (u64)input - rrLevels;
        u64 heaviest = fibNumber(deepest + 1);
        if(heaviest * nranks > total) {
            ocrPrintf("fib: scatter-levels %lu leaves subtrees of %lu leaves "
                      "against %lu per rank over %lu ranks; deepen it\n",
                      (unsigned long)rrLevels, (unsigned long)heaviest,
                      (unsigned long)(total / nranks), (unsigned long)nranks);
            ocrAbort(1);
        }
    }

    ocrGuid_t fibC, totallyDoneEvent, absFinalEdt;

    absFinalPRM_t absFinalParamv;
    absFinalParamv.correctAns = fibNumber((u64)input);

    {
        ocrGuid_t templateGuid;
        ocrEdtTemplateCreate(&templateGuid, absFinal, PRMNUM(absFinal), 1);
        ocrPrintf("Created template and got GUID "GUIDF"\n", GUIDA(templateGuid));
        /* The checker consumes the root's result, so it belongs on the rank
         * that owns the root's leaf interval -- the rank the root itself
         * runs on. */
        ocrHint_t absHint;
        ocrEdtCreate(&absFinalEdt, templateGuid, PRMNUM(absFinal), (u64 *)&absFinalParamv, 1, NULL, EDT_PROP_NONE,
                     fibChildEdtHint(&absHint, 0, 0, total, rrLevels), NULL);
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
        fibParamv.lo = 0;
        fibParamv.total = total;
        fibParamv.rrLevels = rrLevels;

        ocrGuid_t templateGuid;
        ocrEdtTemplateCreate(&templateGuid, fibEdt, PRMNUM(fib), 1);
        ocrHint_t rootHint;
        ocrEdtCreate(&fibC, templateGuid, PRMNUM(fib), (u64 *)&fibParamv, 1, NULL, EDT_PROP_NONE,
                     fibChildEdtHint(&rootHint, 0, 0, total, rrLevels), NULL);
        ocrEdtTemplateDestroy(templateGuid);
        /* The child only reads its argument; wire it RO, after the release
         * above published the value. */
        ocrAddDependence(fibArg, fibC, 0, DB_MODE_RO);
    }

    return NULL_GUID;
}
