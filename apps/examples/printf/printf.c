#include "ocr.h"
#include <stdlib.h>

ocrGuid_t mainEdt ( u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 r, reps = 1;
    u64 argc = getArgc(depv[0].ptr);
    if (argc > 1) {
        u64 v = strtoull(getArgv(depv[0].ptr, 1), NULL, 10);
        if (v >= 1) reps = v;
    }
    for (r = 0; r < reps; r++)
        ocrPrintf("Hello from mainEdt()\n");
    ocrShutdown();
    return NULL_GUID;
}
