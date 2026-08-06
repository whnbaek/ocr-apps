#include "ocr.h"
#include <stdlib.h>

ocrGuid_t f(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    const u64 n = paramv[0];
    ocrPrintf("Hello from %lu\n", n);
    return NULL_GUID;
}

ocrGuid_t launcherEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    const u64 loop_bound = paramv[0];
    ocrGuid_t template, edt, depEvent;
    ocrEventCreate(&depEvent, OCR_EVENT_ONCE_T, EVT_PROP_NONE);
    ocrEdtTemplateCreate(&template, f, 1, 1);

    u64 i;
    for (i=0; i<loop_bound; i+=3) {
        u64 n = i%10;
        ocrEdtCreate(&edt, template, 1, &n, 1, &depEvent, EDT_PROP_NONE, NULL_HINT, NULL);
        { // OCR hints
            ocrHint_t _stepHints;
            ocrHintInit(&_stepHints, OCR_HINT_EDT_T);
            u64 _hintVal = n;
            ocrSetHintValue(&_stepHints, OCR_HINT_EDT_PRIORITY, _hintVal);
            ocrSetHint(edt, &_stepHints);
        }
        ocrPrintf("CREATE %lu\n", n);
    }

    ocrEventSatisfy(depEvent, NULL_GUID);

    return NULL_GUID;
}

ocrGuid_t shutdownEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrShutdown();
    return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    /* Spawn-loop bound: compiled default, overridable by argv[1]; forwarded
     * to the launcher through paramv (EDTs may run on any policy domain). */
    u64 loop_bound = 30;
    u64 argc = getArgc(depv[0].ptr);
    if (argc > 1) {
        u64 v = strtoull(getArgv(depv[0].ptr, 1), NULL, 10);
        if (v >= 3) loop_bound = v;
    }

    ocrPrintf("Hello from mainEdt()\n");

    ocrGuid_t template, edt, depEvent, outEvent;
    ocrEventCreate(&depEvent, OCR_EVENT_ONCE_T, EVT_PROP_NONE);

    ocrEdtTemplateCreate(&template, launcherEdt, 1, 1);
    ocrEdtCreate(&edt, template, 1, &loop_bound, 1, &depEvent, EDT_PROP_FINISH, NULL_HINT, &outEvent);

    ocrEdtTemplateCreate(&template, shutdownEdt, 0, 1);
    ocrEdtCreate(&edt, template, 0, NULL, 1, &outEvent, EDT_PROP_FINISH, NULL_HINT, NULL);

    ocrEventSatisfy(depEvent, NULL_GUID);

    return NULL_GUID;
}
