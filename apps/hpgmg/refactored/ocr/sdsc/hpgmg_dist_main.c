// The distributed decomposition's program shell: same arguments, same
// top-level flow as the base program, with the serial initializer and the
// serial solve spine replaced by their distributed builders.
#include <ocr.h>
#include <stdlib.h>

#include "hpgmg.h"
#include "mg_dist.h"

#ifndef OCR_APP_OPTIMIZED_PLACEMENT
#error "the distributed decomposition builds its placement in"
#endif

static ocrGuid_t top_warm_dist(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
static ocrGuid_t top_loop_dist(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  u64 argc = ocrGetArgc(depv[0].ptr);
  if(argc!=3){
    ocrPrintf("usage: hpgmg log2_box_dim target_boxes\n");
    ocrShutdown(); return NULL_GUID;
  }

  char* argv[2];
  argv[0] = ocrGetArgv(depv[0].ptr,1);
  argv[1] = ocrGetArgv(depv[0].ptr,2);

  int log2_box_dim=atoi(argv[0]);
  int target_boxes=atoi(argv[1]);

  if(log2_box_dim<4){
    ocrPrintf("log2_box_dim must be at least 4\n");
    ocrShutdown(); return NULL_GUID;
  }

  if(target_boxes<1){
    ocrPrintf("target_boxes_per_rank must be at least 1\n");
    ocrShutdown(); return NULL_GUID;
  }

  // calculate the problem size...
  int box_dim=1<<log2_box_dim;
  int boxes_in_i = 1000;
  int total_boxes = boxes_in_i*boxes_in_i*boxes_in_i;
  while(total_boxes>target_boxes){
    boxes_in_i--;
    total_boxes = boxes_in_i*boxes_in_i*boxes_in_i;
  }

  ocrGuid_t mg; mg_type* mg_ptr;
  ocrDbCreate(&mg, (void**)&mg_ptr, sizeof(mg_type), 0, NULL_HINT, NO_ALLOC);

  // distributed initialization: the solve starts when its chain completes
  ocrGuid_t initDone = dist_init(mg_ptr, box_dim, boxes_in_i);
  ocrDbRelease(mg);

  ocrGuid_t tmp,edt;
  ocrHint_t spineHint;
  ocrEdtTemplateCreate(&tmp, top_warm_dist, 0, 2);
  ocrEdtCreate(&edt, tmp, 0, NULL, 2, NULL, 0, mgHomeEdtHint(&spineHint), NULL);
  ocrAddDependence(mg, edt, 0, DB_MODE_CONST);
  ocrAddDependence(initDone, edt, 1, DB_MODE_NULL);
  ocrEdtTemplateDestroy(tmp);

  ocrDbDestroy(depv[0].guid);

  return NULL_GUID;
}

static ocrGuid_t top_warm_dist(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  ocrGuid_t e; ocrEventCreate(&e, OCR_EVENT_ONCE_T, 1);
  ocrGuid_t cont = dist_solves(e, (mg_type*)depv[0].ptr, WARMUP, 1);
  ocrGuid_t tmp,edt;
  ocrHint_t spineHint;
  ocrEdtTemplateCreate(&tmp, top_loop_dist, 0, 2);
  ocrEdtCreate(&edt, tmp, 0, NULL, 2, NULL, 0, mgHomeEdtHint(&spineHint), NULL);
  ocrAddDependence(depv[0].guid, edt, 0, DB_MODE_CONST);
  ocrAddDependence(cont, edt, 1, DB_MODE_CONST);
  ocrEdtTemplateDestroy(tmp);

  ocrEventSatisfy(e, depv[0].guid);

  return NULL_GUID;
}

static ocrGuid_t top_loop_dist(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  ocrGuid_t e; ocrEventCreate(&e, OCR_EVENT_ONCE_T, 1);
  ocrGuid_t cont = dist_solves(e, (mg_type*)depv[0].ptr, TIMED, 0);
  ocrGuid_t tmp,edt;
  ocrHint_t spineHint;
  ocrEdtTemplateCreate(&tmp, finalize_dist, 0, 3);
  ocrEdtCreate(&edt, tmp, 0, NULL, 3, NULL, 0, mgHomeEdtHint(&spineHint), NULL);
  ocrAddDependence(depv[0].guid, edt, 0, DB_MODE_CONST);
  ocrAddDependence(((mg_type *)(depv[0].ptr))->levels[0], edt, 1, DB_MODE_CONST);
  ocrAddDependence(cont, edt, 2, DB_MODE_CONST);
  ocrEdtTemplateDestroy(tmp);

  ocrEventSatisfy(e, depv[0].guid);

  return NULL_GUID;
}
