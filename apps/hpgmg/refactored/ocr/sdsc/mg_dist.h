// The distributed decomposition's own interface: hpgmg_dist is a separate
// program (hpgmg_dist_main.c + mg_dist.c + exchange_dist.c) that links the
// base port's kernel and per-box task sources unchanged — nothing here is
// visible to the base build.
#ifndef MG_DIST_H
#define MG_DIST_H

#include <ocr.h>
#ifdef ENABLE_EXTENSION_AFFINITY
#include <extensions/ocr-affinity.h>
#endif

#include "hpgmg.h"

/* Pin an EDT to the rank that homes a given rank index. */
static inline ocrHint_t * mgRankEdtHint(ocrHint_t * h, u64 r)
{
#ifdef ENABLE_EXTENSION_AFFINITY
  ocrGuid_t aff = NULL_GUID;
  ocrAffinityGetAt(AFFINITY_PD, r, &aff);
  ocrHintInit(h, OCR_HINT_EDT_T);
  ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
  return h;
#else
  (void)h; (void)r;
  return NULL_HINT;
#endif
}

// per-box initialization bodies shared with the serial path (init.c)
void hpgmg_valid_box(level_type *level, box_type *bx);
void hpgmg_fill_box(level_type *level, box_type *bx, double a, double b);
double hpgmg_gershgorin_box(level_type *level, box_type *bx, double a, double b);
void hpgmg_coeff_restrict_box(level_type *level, box_type *cb,
                              level_type *fromLevel, box_type *fine_bx,
                              int fb, int count);
void boxFactor3(s64 P, int *px, int *py, int *pz);
int boxHomePD(int box_num, int S, s64 P);

// face slabs (exchange_dist.c)
ocrGuid_t* dist_face_guids(level_type *l);
ocrGuid_t get_face_guid(level_type *l, int box_id, int face);
int dist_slab_face(int fi, int fj, int fk);
ocrGuid_t pack_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t unpack_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

// the driver (mg_dist.c)
ocrGuid_t dist_init(mg_type *mg_ptr, int box_dim0, int boxes_in_i0);
ocrGuid_t dist_solves(ocrGuid_t start, mg_type *mg_ptr, int num, int warmup);
ocrGuid_t finalize_dist(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

// defined in the shared sources (mg_edt.c)
ocrGuid_t shutdown_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

#endif
