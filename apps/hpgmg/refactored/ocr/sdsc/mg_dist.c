// Distributed decomposition: the data plane is rank-persistent, the control
// plane is a thin spine.
//
// Every box, its six face slabs, and the tasks that touch them live on one
// rank for the whole run (the spatial boxHomePD partition).  Each spine
// phase, instead of creating every per-box task itself — O(boxes) remote
// creations serialized through one rank, repeated once per phase — creates
// one pinned FINISH slice per rank; the slice creates its own boxes' tasks
// locally and its finish latch counts their completions locally, so a phase
// costs the spine O(ranks) messages rather than O(boxes).
//
// Initialization is distributed the same way: per-rank creators allocate,
// zero and fill their own boxes (the analytic problem evaluation is the
// bulk of it, spawned as per-box tasks), a merge assembles the guid tables,
// and the operator build (coefficient restriction, Gershgorin, ghost
// exchange of Dinv/L1inv over the face slabs) runs as sliced phases.  The
// serial preamble this replaces did all of it on one worker.
#ifndef OCR_APP_OPTIMIZED_PLACEMENT
#error "the distributed decomposition builds its placement in"
#endif

#include <ocr.h>
#include <extensions/ocr-affinity.h>
#include <string.h>
#include <math.h>

#ifdef TG_ARCH
#include "strings.h"
#endif

#include "hpgmg.h"
#include "utils.h"
#include "mg_dist.h"

// slice kinds
enum {
  DK_INIT_UR, DK_SMOOTH, DK_RESIDUAL, DK_ZERO, DK_MULV,
  DK_PACK, DK_UNPACK, DK_RESTRICT, DK_INTERP,
  DK_COEFF, DK_GERSH, DK_NORM, DK_ERROR,
  // lattice-only phases (no slice fan-out): a single rank-0 task
  DK_TIME, DK_SOLVE, DK_NORMFINAL
};

// how a lattice phase is gated on the previous phase
enum { DG_OWN, DG_NBR, DG_ALL };

typedef struct {
  u8 kind;
  u8 lvl;        // levelA index (the level the fan-out runs over)
  u8 lvl2;       // levelB index (restrict/interpolate partner)
  u8 gate;
  s32 p;         // iter / flag / type / time-op
} dist_phase_t;

#define DIST_MAX_PHASES 4096

#define DIST_MAX_RANKS 64

// The Poisson test problem the port hard-codes (see solve_edt).
#define DIST_A 0.0
#define DIST_B 1.0

ocrGuid_t dist_slice_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t dist_subphase_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t dist_reduce_subphase_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t dist_prep_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t dist_coeff_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t dist_gersh_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t dist_norm_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t dist_error_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t dist_max_merge_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t dist_eigen_final_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t dist_norm_final_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t dist_error_final_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t dist_creator_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t dist_merge_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

static u64 dist_ranks(void) {
  u64 n = 1;
#ifdef ENABLE_EXTENSION_AFFINITY
  ocrAffinityCount(AFFINITY_PD, &n);
#endif
  if (n > DIST_MAX_RANKS) {
    ocrPrintf("hpgmg_dist supports at most %d ranks (%lu configured)\n",
              DIST_MAX_RANKS, n);
#if TG_ARCH
    ABORT(0);
#else
    ocrAbort(0);
#endif
  }
  return n;
}

// A guid rides paramv as its 64-bit representation.
static inline u64 dist_guid_bits(ocrGuid_t g) {
  u64 v; memcpy(&v, &g, sizeof(v)); return v;
}
static inline ocrGuid_t dist_guid_from(u64 v) {
  ocrGuid_t g; memcpy(&g, &v, sizeof(g)); return g;
}

/* ------------------------------------------------------------------ */
/* slice fan-out                                                       */
/* ------------------------------------------------------------------ */

// One pinned FINISH slice per rank.  evts, when non-NULL, gives each slice
// a per-rank result event (reduction kinds).
static void dist_spawn_slices(u64 kind, u64 p0, u64 p1,
                              ocrGuid_t levelA, ocrGuid_t levelB,
                              ocrGuid_t *evts) {
  u64 R = dist_ranks(), r;
  ocrGuid_t s_t, s;
  ocrHint_t h;
  ocrEdtTemplateCreate(&s_t, dist_slice_edt, 6, 2);
  for (r = 0; r < R; r++) {
    u64 pv[6] = { kind, p0, p1, 0, r, 0 };
    if (evts) pv[3] = dist_guid_bits(evts[r]);
    ocrEdtCreate(&s, s_t, 6, pv, 2, NULL, EDT_PROP_FINISH,
                 mgRankEdtHint(&h, r), NULL);
    ocrAddDependence(levelA, s, 0, DB_MODE_CONST);
    ocrAddDependence(levelB, s, 1, DB_MODE_CONST);
  }
  ocrEdtTemplateDestroy(s_t);
}

// deps: levelA - levelB - gate; pars: kind p0 p1
// A sliced phase as one FINISH EDT: the output event is the phase barrier.
ocrGuid_t dist_subphase_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  dist_spawn_slices(paramv[0], paramv[1], paramv[2],
                    depv[0].guid, depv[1].guid, NULL);
  return NULL_GUID;
}

// deps: level - events DB - gate; pars: kind p0 p1
// The reduction flavour: the per-rank result events ride in a datablock.
ocrGuid_t dist_reduce_subphase_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  ocrGuid_t *evts = (ocrGuid_t*) depv[1].ptr;
  dist_spawn_slices(paramv[0], paramv[1], paramv[2],
                    depv[0].guid, depv[0].guid, evts);
  ocrDbDestroy(depv[1].guid);
  return NULL_GUID;
}

// pars: kind - p0 - p1 - evt - rank - sub; deps: levelA - levelB
// sub == 0 is the rank's top slice; a large local box count is split into
// K local sub-slices (sub = K<<16 | k) so task creation itself, one
// worker's serial loop otherwise, parallelizes within the rank.
ocrGuid_t dist_slice_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  u64 kind = paramv[0], p0 = paramv[1], p1 = paramv[2];
  ocrGuid_t evt = dist_guid_from(paramv[3]);
  u64 r = paramv[4], sub = paramv[5], R = dist_ranks();
  u64 K = sub ? (sub >> 16) : 1, kk = sub ? (sub & 0xffff) : 0;
  u64 ord = 0;

  level_type *lA = (level_type*) depv[0].ptr;
  level_type *lB = (level_type*) depv[1].ptr;
  ocrGuid_t *boxesA = (ocrGuid_t*)(((char*)lA) + lA->boxes);
  ocrHint_t h;
  ocrGuid_t c, c_t;
  int b;

  if (sub == 0 && kind != DK_GERSH && kind != DK_NORM && kind != DK_ERROR) {
    level_type *dom = (kind == DK_RESTRICT || kind == DK_INTERP ||
                       kind == DK_COEFF) ? lB : lA;
    if (kind == DK_COEFF) dom = lA;   // COEFF loops the coarse level = A
    u64 nlocal = 0;
    for (b = 0; b < dom->num_boxes; ++b)
      if (boxHomePD(b, dom->boxes_in.i, R) == (int) r) nlocal++;
    if (nlocal >= 128) {
      u64 nk = (nlocal >= 1024) ? 16 : 8, k2;
      ocrGuid_t s_t, s;
      ocrEdtTemplateCreate(&s_t, dist_slice_edt, 6, 2);
      for (k2 = 0; k2 < nk; k2++) {
        u64 pv[6] = { kind, p0, p1, paramv[3], r, (nk << 16) | k2 };
        ocrEdtCreate(&s, s_t, 6, pv, 2, NULL, 0, mgRankEdtHint(&h, r), NULL);
        ocrAddDependence(depv[0].guid, s, 0, DB_MODE_CONST);
        ocrAddDependence(depv[1].guid, s, 1, DB_MODE_CONST);
      }
      ocrEdtTemplateDestroy(s_t);
      return NULL_GUID;
    }
  }

  switch (kind) {

  case DK_INIT_UR: {
    ocrEdtTemplateCreate(&c_t, init_ur_edt, 4, 2);
    u64 pv[4] = { lA->u, lA->f_Av, lA->f, p0 };
    for (b = 0; b < lA->num_boxes; ++b) {
      if (boxHomePD(b, lA->boxes_in.i, R) != (int) r) continue;
      if ((ord++ % K) != kk) continue;
      ocrEdtCreate(&c, c_t, 4, pv, 2, NULL, 0, mgRankEdtHint(&h, r), NULL);
      ocrAddDependence(depv[0].guid, c, 0, DB_MODE_CONST);
      ocrAddDependence(boxesA[b], c, 1, DB_MODE_RW);
    }
    ocrEdtTemplateDestroy(c_t);
    break;
  }

  case DK_SMOOTH: {
    ocrEdtTemplateCreate(&c_t, smooth_edt, 1, 2);
    for (b = 0; b < lA->num_boxes; ++b) {
      if (boxHomePD(b, lA->boxes_in.i, R) != (int) r) continue;
      if ((ord++ % K) != kk) continue;
      ocrEdtCreate(&c, c_t, 1, &p0, 2, NULL, 0, mgRankEdtHint(&h, r), NULL);
      ocrAddDependence(depv[0].guid, c, 0, DB_MODE_CONST);
      ocrAddDependence(boxesA[b], c, 1, DB_MODE_RW);
    }
    ocrEdtTemplateDestroy(c_t);
    break;
  }

  case DK_RESIDUAL: {
    u32 cpc = (u32) p0;             // 0 = f_Av rhs, 1 = f rhs (scaled norm)
    ocrEdtTemplateCreate(&c_t, residual_edt, cpc, 2);
    for (b = 0; b < lA->num_boxes; ++b) {
      if (boxHomePD(b, lA->boxes_in.i, R) != (int) r) continue;
      if ((ord++ % K) != kk) continue;
      ocrEdtCreate(&c, c_t, cpc, cpc ? &p1 : NULL, 2, NULL, 0,
                   mgRankEdtHint(&h, r), NULL);
      ocrAddDependence(depv[0].guid, c, 0, DB_MODE_CONST);
      ocrAddDependence(boxesA[b], c, 1, DB_MODE_RW);
    }
    ocrEdtTemplateDestroy(c_t);
    break;
  }

  case DK_ZERO: {
    ocrEdtTemplateCreate(&c_t, zero_vector_edt, 1, 2);
    for (b = 0; b < lA->num_boxes; ++b) {
      if (boxHomePD(b, lA->boxes_in.i, R) != (int) r) continue;
      if ((ord++ % K) != kk) continue;
      ocrEdtCreate(&c, c_t, 1, &p0, 2, NULL, 0, mgRankEdtHint(&h, r), NULL);
      ocrAddDependence(depv[0].guid, c, 0, DB_MODE_CONST);
      ocrAddDependence(boxesA[b], c, 1, DB_MODE_RW);
    }
    ocrEdtTemplateDestroy(c_t);
    break;
  }

  case DK_MULV: {
    ocrEdtTemplateCreate(&c_t, mulv_edt, 3, 2);
    u64 pv[3] = { lA->u, lA->f_Av, lA->f };
    for (b = 0; b < lA->num_boxes; ++b) {
      if (boxHomePD(b, lA->boxes_in.i, R) != (int) r) continue;
      if ((ord++ % K) != kk) continue;
      ocrEdtCreate(&c, c_t, 3, pv, 2, NULL, 0, mgRankEdtHint(&h, r), NULL);
      ocrAddDependence(depv[0].guid, c, 0, DB_MODE_CONST);
      ocrAddDependence(boxesA[b], c, 1, DB_MODE_RW);
    }
    ocrEdtTemplateDestroy(c_t);
    break;
  }

  case DK_PACK: {                    // p0 = vector offset
    ocrGuid_t *faces = dist_face_guids(lA);
    ocrEdtTemplateCreate(&c_t, pack_edt, 1, 8);
    for (b = 0; b < lA->num_boxes; ++b) {
      if (boxHomePD(b, lA->boxes_in.i, R) != (int) r) continue;
      if ((ord++ % K) != kk) continue;
      int f;
      ocrEdtCreate(&c, c_t, 1, &p0, 8, NULL, 0, mgRankEdtHint(&h, r), NULL);
      ocrAddDependence(depv[0].guid, c, 0, DB_MODE_CONST);
      ocrAddDependence(boxesA[b], c, 1, DB_MODE_CONST);
      for (f = 0; f < 6; f++)
        ocrAddDependence(faces[b*6 + f], c, 2 + f, DB_MODE_RW);
    }
    ocrEdtTemplateDestroy(c_t);
    break;
  }

  case DK_UNPACK: {                  // p0 = vector offset, p1 = 6 or 26
    ocrGuid_t *faces = dist_face_guids(lA);
    int nnb = (int) p1;
    int nbrs[26];
    static const int off6[6][3] =
      {{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1}};
    ocrEdtTemplateCreate(&c_t, unpack_edt, 2, 2 + nnb);
    for (b = 0; b < lA->num_boxes; ++b) {
      if (boxHomePD(b, lA->boxes_in.i, R) != (int) r) continue;
      if ((ord++ % K) != kk) continue;
      int i;
      if (nnb == 6)
        get_neighbors(b, lA->boxes_in.i, lA->boxes_in.j, lA->boxes_in.k, nbrs);
      else
        get_neighbors_all(b, lA->boxes_in.i, lA->boxes_in.j, lA->boxes_in.k, nbrs);
      u64 mask = 0;
      for (i = 0; i < nnb; i++)
        if (nbrs[i] >= 0 && nbrs[i] < lA->num_boxes) mask |= (1ull << i);
      u64 pv[2] = { p0, mask };
      ocrEdtCreate(&c, c_t, 2, pv, 2 + nnb, NULL, 0, mgRankEdtHint(&h, r), NULL);
      ocrAddDependence(depv[0].guid, c, 0, DB_MODE_CONST);
      ocrAddDependence(boxesA[b], c, 1, DB_MODE_RW);
      if (nnb == 6) {
        for (i = 0; i < 6; i++) {
          int face = dist_slab_face(off6[i][0], off6[i][1], off6[i][2]);
          ocrGuid_t g = (mask & (1ull << i))
            ? faces[nbrs[i]*6 + face] : faces[b*6 + face];
          ocrAddDependence(g, c, 2 + i, DB_MODE_CONST);
        }
      } else {
        int fi, fj, fk, n = 0;
        for (fi = -1; fi <= 1; fi++)
          for (fj = -1; fj <= 1; fj++)
            for (fk = -1; fk <= 1; fk++) {
              if (!fi && !fj && !fk) continue;
              int face = dist_slab_face(fi, fj, fk);
              ocrGuid_t g = (mask & (1ull << n))
                ? faces[nbrs[n]*6 + face] : faces[b*6 + face];
              ocrAddDependence(g, c, 2 + n, DB_MODE_CONST);
              n++;
            }
      }
    }
    ocrEdtTemplateDestroy(c_t);
    break;
  }

  case DK_RESTRICT: {                // A = fine, B = coarse; p0 = flag, p1 = box evt
    level_type *f = lA; level_type *cl = lB;
    ocrGuid_t *boxesC = (ocrGuid_t*)(((char*)cl) + cl->boxes);
    int count = f->num_boxes / cl->num_boxes;
    ocrGuid_t fine[MAX_FINE_BOXES];
    PRM_restrict_level_edt_t prm;
    prm.box = dist_guid_from(p1);
    prm.flag = (s64) p0;
    ocrEdtTemplateCreate(&c_t, restrict_edt,
                         sizeof(PRM_restrict_level_edt_t)/sizeof(u64), 3 + count);
    for (b = 0; b < cl->num_boxes; ++b) {
      if (boxHomePD(b, cl->boxes_in.i, R) != (int) r) continue;
      if ((ord++ % K) != kk) continue;
      int fc;
      get_fine_boxes(f, cl, b, fine);
      ocrEdtCreate(&c, c_t, EDT_PARAM_DEF, (u64*)&prm, 3 + count, NULL, 0,
                   mgRankEdtHint(&h, r), NULL);
      ocrAddDependence(depv[1].guid, c, 0, DB_MODE_CONST);
      ocrAddDependence(boxesC[b], c, 1, DB_MODE_RW);
      ocrAddDependence(depv[0].guid, c, 2, DB_MODE_CONST);
      for (fc = 0; fc < count; ++fc)
        ocrAddDependence(fine[fc], c, 3 + fc, DB_MODE_CONST);
    }
    ocrEdtTemplateDestroy(c_t);
    break;
  }

  case DK_INTERP: {                  // A = fine, B = coarse; p0 = type
    level_type *f = lA; level_type *cl = lB;
    ocrGuid_t *boxesC = (ocrGuid_t*)(((char*)cl) + cl->boxes);
    int count = f->num_boxes / cl->num_boxes;
    ocrGuid_t fine[MAX_FINE_BOXES];
    ocrEdtTemplateCreate(&c_t, interpolate_edt, 1, 3 + count);
    for (b = 0; b < cl->num_boxes; ++b) {
      if (boxHomePD(b, cl->boxes_in.i, R) != (int) r) continue;
      if ((ord++ % K) != kk) continue;
      int fc;
      get_fine_boxes(f, cl, b, fine);
      ocrEdtCreate(&c, c_t, 1, &p0, 3 + count, NULL, 0,
                   mgRankEdtHint(&h, r), NULL);
      ocrAddDependence(depv[1].guid, c, 0, DB_MODE_CONST);
      ocrAddDependence(boxesC[b], c, 1,
                       p0 == FMG_INTERPOLATE ? DB_MODE_RW : DB_MODE_CONST);
      ocrAddDependence(depv[0].guid, c, 2, DB_MODE_CONST);
      for (fc = 0; fc < count; ++fc)
        ocrAddDependence(fine[fc], c, 3 + fc, DB_MODE_RW);
    }
    ocrEdtTemplateDestroy(c_t);
    break;
  }

  case DK_COEFF: {                   // A = coarse, B = fine (restrict coefficients)
    level_type *cl = lA; level_type *f = lB;
    int count = f->num_boxes / cl->num_boxes;
    ocrGuid_t fine[MAX_FINE_BOXES];
    u64 cnt = count;
    ocrEdtTemplateCreate(&c_t, dist_coeff_edt, 1, 3 + count);
    for (b = 0; b < cl->num_boxes; ++b) {
      if (boxHomePD(b, cl->boxes_in.i, R) != (int) r) continue;
      if ((ord++ % K) != kk) continue;
      int fc;
      get_fine_boxes(f, cl, b, fine);
      ocrEdtCreate(&c, c_t, 1, &cnt, 3 + count, NULL, 0,
                   mgRankEdtHint(&h, r), NULL);
      ocrAddDependence(depv[0].guid, c, 0, DB_MODE_CONST);
      ocrAddDependence(boxesA[b], c, 1, DB_MODE_RW);
      ocrAddDependence(depv[1].guid, c, 2, DB_MODE_CONST);
      for (fc = 0; fc < count; ++fc)
        ocrAddDependence(fine[fc], c, 3 + fc, DB_MODE_CONST);
    }
    ocrEdtTemplateDestroy(c_t);
    break;
  }

  // Reduction kinds: per-box children hand small result DBs to a local
  // merge; the merge satisfies this rank's event with the local extremum.
  case DK_GERSH:
  case DK_NORM:
  case DK_ERROR: {
    ocrGuid_t m, m_t, fin;
    int nlocal = 0;
    for (b = 0; b < lA->num_boxes; ++b)
      if (boxHomePD(b, lA->boxes_in.i, R) == (int) r) nlocal++;

    if (nlocal == 0) {
      ocrGuid_t dbg; double *dp;
      ocrDbCreate(&dbg, (void**)&dp, sizeof(double), 0, NULL_HINT, NO_ALLOC);
      *dp = -1e9;
      ocrDbRelease(dbg);
      ocrEventSatisfy(evt, dbg);
      break;
    }

    u64 mpv[2] = { dist_guid_bits(evt), 0 };
    ocrEdtTemplateCreate(&m_t, dist_max_merge_edt, 2, nlocal);
    ocrEdtCreate(&m, m_t, 2, mpv, nlocal, NULL, 0, mgRankEdtHint(&h, r), NULL);
    ocrEdtTemplateDestroy(m_t);

    if (kind == DK_NORM) ocrEdtTemplateCreate(&c_t, dist_norm_edt, 0, 2);
    else if (kind == DK_GERSH) ocrEdtTemplateCreate(&c_t, dist_gersh_edt, 0, 2);
    else ocrEdtTemplateCreate(&c_t, dist_error_edt, 0, 2);

    int slot = 0;
    for (b = 0; b < lA->num_boxes; ++b) {
      if (boxHomePD(b, lA->boxes_in.i, R) != (int) r) continue;
      if ((ord++ % K) != kk) continue;
      ocrEdtCreate(&c, c_t, 0, NULL, 2, NULL, 0, mgRankEdtHint(&h, r), &fin);
      ocrAddDependence(depv[0].guid, c, 0, DB_MODE_CONST);
      ocrAddDependence(boxesA[b], c, 1,
                       kind == DK_GERSH ? DB_MODE_RW : DB_MODE_CONST);
      ocrAddDependence(fin, m, slot++, DB_MODE_CONST);
    }
    ocrEdtTemplateDestroy(c_t);
    break;
  }
  }

  return NULL_GUID;
}

/* ------------------------------------------------------------------ */
/* per-box bodies the base program does not have                       */
/* ------------------------------------------------------------------ */

// deps: level - box(RW); pars: fine flag
// Everything the box needs before the operator build, off the creator's
// serial loop: zero the vector region, mark the valid cells, and — on the
// fine level — evaluate the analytic problem.
ocrGuid_t dist_prep_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  level_type *l = (level_type*) depv[0].ptr;
  box_type *bx = (box_type*) depv[1].ptr;
  bzero((char*)bx + sizeof(box_type), l->volume*NUM_VECTORS*sizeof(double));
  hpgmg_valid_box(l, bx);
  if (paramv[0])
    hpgmg_fill_box(l, bx, DIST_A, DIST_B);
  return NULL_GUID;
}

// deps: coarse level - coarse box(RW) - fine level - fine boxes; pars: count
ocrGuid_t dist_coeff_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  level_type *cl = (level_type*) depv[0].ptr;
  level_type *f = (level_type*) depv[2].ptr;
  box_type *cb = (box_type*) depv[1].ptr;
  int count = (int) paramv[0];
  int o;
  for (o = 3; o < (int) depc; ++o)
    hpgmg_coeff_restrict_box(cl, cb, f, (box_type*) depv[o].ptr, o - 3, count);
  return NULL_GUID;
}

// deps: level - box(RW); returns this box's Gershgorin bound
ocrGuid_t dist_gersh_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  ocrGuid_t g; double *d;
  ocrDbCreate(&g, (void**)&d, sizeof(double), 0, NULL_HINT, NO_ALLOC);
  *d = hpgmg_gershgorin_box((level_type*) depv[0].ptr, (box_type*) depv[1].ptr,
                            DIST_A, DIST_B);
  return g;
}

// deps: level - box; returns this box's residual norm
ocrGuid_t dist_norm_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  level_type *l = (level_type*) depv[0].ptr;
  box_type *bx = (box_type*) depv[1].ptr;
  double *temp = (double*)((char*)bx + l->vec_temp);
  ocrGuid_t g; double *d;
  ocrDbCreate(&g, (void**)&d, sizeof(double), 0, NULL_HINT, NO_ALLOC);
  *d = norm_coarse(l, temp);
  return g;
}

// deps: level - box; returns this box's max |u - u_true|
ocrGuid_t dist_error_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  level_type *l = (level_type*) depv[0].ptr;
  box_type *bx = (box_type*) depv[1].ptr;
  int jS = l->jStride, kS = l->kStride, dim = l->box_dim;
  double *u = (double*)((char*)bx + l->u) + NUM_GHOSTS*(1+jS+kS);
  double *ut = (double*)((char*)bx + l->u_true) + NUM_GHOSTS*(1+jS+kS);
  double m = 0.0;
  int i,j,k;
  for (k = 0; k < dim; k++)
    for (j = 0; j < dim; j++)
      for (i = 0; i < dim; i++) {
        int ijk = i + j*jS + k*kS;
        double d = fabs(u[ijk] - ut[ijk]);
        if (d > m) m = d;
      }
  ocrGuid_t g; double *d;
  ocrDbCreate(&g, (void**)&d, sizeof(double), 0, NULL_HINT, NO_ALLOC);
  *d = m;
  return g;
}

// deps: per-box result DBs; pars: rank event - offset of the double
ocrGuid_t dist_max_merge_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  double m = -1e9;
  u32 i;
  for (i = 0; i < depc; i++) {
    double v = *(double*)((char*)depv[i].ptr + paramv[1]);
    if (v > m) m = v;
    ocrDbDestroy(depv[i].guid);
  }
  ocrGuid_t g; double *d;
  ocrDbCreate(&g, (void**)&d, sizeof(double), 0, NULL_HINT, NO_ALLOC);
  *d = m;
  ocrDbRelease(g);
  ocrEventSatisfy(dist_guid_from(paramv[0]), g);
  return NULL_GUID;
}

// deps: level(RW) - subphase gate - R rank maxima
ocrGuid_t dist_eigen_final_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  level_type *l = (level_type*) depv[0].ptr;
  double m = -1e9;
  u32 i;
  for (i = 2; i < depc; i++) {
    double v = *(double*) depv[i].ptr;
    if (v > m) m = v;
    ocrDbDestroy(depv[i].guid);
  }
  l->dominant_eigenvalue_of_DinvA = m;
  ocrPrintf("eigenvalue_max < %f\n", m);
  return NULL_GUID;
}

// deps: R rank maxima
ocrGuid_t dist_norm_final_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  double m = -1e9;
  u32 i;
  for (i = 0; i < depc; i++) {
    double v = *(double*) depv[i].ptr;
    if (v > m) m = v;
    ocrDbDestroy(depv[i].guid);
  }
  ocrPrintf("f-cycle,    norm=%22.20f\n", m);
  return NULL_GUID;
}

// deps: level - subphase gate - R rank maxima
ocrGuid_t dist_error_final_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  level_type *l = (level_type*) depv[0].ptr;
  double m = 0.0;
  u32 i;
  for (i = 2; i < depc; i++) {
    double v = *(double*) depv[i].ptr;
    if (v > m) m = v;
    ocrDbDestroy(depv[i].guid);
  }
  ocrPrintf("h = %f  ||error|| = %22.15f\n\n", l->h, m);
  ocrPrintf("Time = %22f\n", l->time_operators[4]/TIMED);
  return NULL_GUID;
}

/* ------------------------------------------------------------------ */
/* distributed initialization                                          */
/* ------------------------------------------------------------------ */

// The level table construction mirrored from the serial builder: halve the
// box dimension down to the agglomeration threshold, then agglomerate the
// box grid, then halve the last box down to one cell.
static int dist_level_table(int box_dim0, int boxes_in_i0,
                            int *box_dim, int *boxes_in_i) {
  int minCoarseDim = 1;
  int maxLevels = MG_MAXLEVELS;
  int level = 1;
  int coarse_dim = box_dim0 * boxes_in_i0;
  while ((coarse_dim >= 2*minCoarseDim) && ((coarse_dim & 0x1) == 0)) {
    level++;
    coarse_dim = coarse_dim / 2;
  }
  if (level < maxLevels) maxLevels = level;

  box_dim[0] = box_dim0;
  boxes_in_i[0] = boxes_in_i0;
  int num_levels = 1;
  int doRestrict = (maxLevels < 2) ? 0 : 1;
  while (doRestrict) {
    int lv = num_levels;
    int fine_box_dim = box_dim[lv-1];
    int fine_dim_i = box_dim[lv-1] * boxes_in_i[lv-1];
    int fine_boxes_in_i = boxes_in_i[lv-1];
    doRestrict = 0;
    if ((fine_box_dim % 2 == 0) && (fine_box_dim > MG_AGGLOMERATION_START)) {
      box_dim[lv] = fine_box_dim/2; boxes_in_i[lv] = fine_boxes_in_i; doRestrict = 1;
    } else if (fine_boxes_in_i % 2 == 0) {
      box_dim[lv] = fine_box_dim; boxes_in_i[lv] = fine_boxes_in_i/2; doRestrict = 1;
    } else if ((coarse_dim != 1) && (fine_dim_i == 2*coarse_dim)) {
      box_dim[lv] = fine_dim_i/2; boxes_in_i[lv] = 1; doRestrict = 1;
    } else if ((coarse_dim != 1) && (fine_dim_i == 4*coarse_dim)) {
      box_dim[lv] = fine_box_dim/2; boxes_in_i[lv] = fine_boxes_in_i; doRestrict = 1;
    } else if ((coarse_dim != 1) && (fine_dim_i == 8*coarse_dim)) {
      box_dim[lv] = fine_box_dim/2; boxes_in_i[lv] = fine_boxes_in_i; doRestrict = 1;
    } else if (fine_box_dim % 2 == 0) {
      box_dim[lv] = fine_box_dim/2; boxes_in_i[lv] = fine_boxes_in_i; doRestrict = 1;
    }
    if (doRestrict && box_dim[lv] < NUM_GHOSTS) doRestrict = 0;
    if (doRestrict) num_levels++;
  }
  return num_levels;
}

// Field-complete level datablock, no boxes yet (the creators bring those).
static void dist_create_level_db(mg_type *mg, int box_dim, int boxes_in_i,
                                 int level, double h) {
  u32 totalBoxes = boxes_in_i*boxes_in_i*boxes_in_i;
  u32 levelSize = sizeof(level_type) + sizeof(ocrGuid_t)*totalBoxes
                + sizeof(ocrGuid_t)*totalBoxes*6 + totalBoxes*sizeof(double);

  ocrPrintf("attempting to create a %d^3 level using a %d^3 grid of %d^3 boxes ...\n",
            box_dim*boxes_in_i, boxes_in_i, box_dim);

  ocrGuid_t levelGuid;
  level_type *lp;
  ocrDbCreate(&levelGuid, (void**)&lp, levelSize, 0, NULL_HINT, NO_ALLOC);
  mg->levels[level] = levelGuid;

  lp->boundary_condition = BC_DIRICHLET;
  lp->level = level;
  lp->alpha_is_zero = 0;
  lp->num_boxes = totalBoxes;
  lp->box_dim = box_dim;
  lp->boxes_in.i = lp->boxes_in.j = lp->boxes_in.k = boxes_in_i;
  lp->dim.i = lp->dim.j = lp->dim.k = boxes_in_i*box_dim;
  lp->boxes = sizeof(level_type);
  lp->b_norms = lp->boxes + sizeof(ocrGuid_t)*totalBoxes;
  lp->h = h;
  lp->constant_box_guid = NULL_GUID;
  lp->tempGuid = NULL_GUID;
  lp->temp = NULL;
  memset(lp->time_operators, 0, sizeof(lp->time_operators));
  memset(lp->time_temp, 0, sizeof(lp->time_temp));

  // the box memory geometry, exactly as the serial creator computes it
  int jStride = (box_dim + 2*NUM_GHOSTS);
  int kStride = jStride * (box_dim + 2*NUM_GHOSTS);
  if (jStride < 8) kStride += (8 - jStride);   // BOX_PLANE_PADDING
  u32 boxVolume = (box_dim + 2*NUM_GHOSTS) * kStride;
  lp->jStride = jStride; lp->kStride = kStride; lp->volume = boxVolume;

  lp->alpha = sizeof(box_type);
  lp->beta_i = lp->alpha + boxVolume*sizeof(double);
  lp->beta_j = lp->beta_i + boxVolume*sizeof(double);
  lp->beta_k = lp->beta_j + boxVolume*sizeof(double);
  lp->Dinv = lp->beta_k + boxVolume*sizeof(double);
  lp->L1inv = lp->Dinv + boxVolume*sizeof(double);
  lp->valid = lp->L1inv + boxVolume*sizeof(double);
  lp->u_true = lp->valid + boxVolume*sizeof(double);
  lp->f = lp->u_true + boxVolume*sizeof(double);
  lp->f_Av = lp->f + boxVolume*sizeof(double);
  lp->u = lp->f_Av + boxVolume*sizeof(double);
  lp->vec_temp = lp->u + boxVolume*sizeof(double);

  ocrDbRelease(levelGuid);
}

// pars: rank - result event - num_levels; deps: the level DBs
// Creates this rank's boxes and slabs on this rank and hands the guid
// table to the merge; zeroing, valid marking and the fine level's analytic
// fill run as per-box prep tasks, off this loop's one worker.
ocrGuid_t dist_creator_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  u64 r = paramv[0];
  ocrGuid_t resultEvt = dist_guid_from(paramv[1]);
  int L = (int) paramv[2];
  u64 R = dist_ranks();
  int l, b, f;

  u64 owned = 0;
  for (l = 0; l < L; l++) {
    level_type *lp = (level_type*) depv[l].ptr;
    for (b = 0; b < lp->num_boxes; b++)
      if (boxHomePD(b, lp->boxes_in.i, R) == (int) r) owned++;
  }

  ocrGuid_t resGuid; ocrGuid_t *res;
  ocrDbCreate(&resGuid, (void**)&res, (owned ? owned : 1) * 7 * sizeof(ocrGuid_t),
              0, NULL_HINT, NO_ALLOC);

  ocrGuid_t aff = NULL_GUID;
  ocrHint_t dbh;
  ocrHintInit(&dbh, OCR_HINT_DB_T);
#ifdef ENABLE_EXTENSION_AFFINITY
  ocrAffinityGetAt(AFFINITY_PD, r, &aff);
#endif
  ocrSetHintValue(&dbh, OCR_HINT_DB_AFFINITY, ocrAffinityToHintValue(aff));

  ocrGuid_t f_t;
  ocrHint_t eh;
  ocrEdtTemplateCreate(&f_t, dist_prep_edt, 1, 2);

  u64 w = 0;
  for (l = 0; l < L; l++) {
    level_type *lp = (level_type*) depv[l].ptr;
    int S = lp->boxes_in.i;
    u32 totalMemSize = sizeof(box_type) + lp->volume*NUM_VECTORS*sizeof(double);
    u64 fine = (l == 0);
    for (b = 0; b < lp->num_boxes; b++) {
      if (boxHomePD(b, S, R) != (int) r) continue;
      ocrGuid_t bg; box_type *bp;
      ocrDbCreate(&bg, (void**)&bp, totalMemSize, 0, &dbh, NO_ALLOC);
      // header only; the per-box prep task zeroes and fills the vectors
      bp->low.i = (b % S) * lp->box_dim;
      bp->low.j = ((b / S) % S) * lp->box_dim;
      bp->low.k = (b / (S*S)) * lp->box_dim;
      bp->global_box_id = b;
      ocrDbRelease(bg);
      res[w*7] = bg;
      for (f = 0; f < 6; f++) {
        ocrGuid_t sg; double *sp;
        ocrDbCreate(&sg, (void**)&sp, lp->box_dim*lp->box_dim*sizeof(double),
                    0, &dbh, NO_ALLOC);
        bzero(sp, lp->box_dim*lp->box_dim*sizeof(double));
        ocrDbRelease(sg);
        res[w*7 + 1 + f] = sg;
      }
      {
        ocrGuid_t c;
        ocrEdtCreate(&c, f_t, 1, &fine, 2, NULL, 0, mgRankEdtHint(&eh, r), NULL);
        ocrAddDependence(depv[l].guid, c, 0, DB_MODE_CONST);
        ocrAddDependence(bg, c, 1, DB_MODE_RW);
      }
      w++;
    }
  }
  ocrEdtTemplateDestroy(f_t);

  ocrDbRelease(resGuid);
  ocrEventSatisfy(resultEvt, resGuid);
  return NULL_GUID;
}

// pars: num_levels - R; deps: L levels(RW) - R result DBs - R creator gates
// Replays each creator's deterministic enumeration to place its guids.
ocrGuid_t dist_merge_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  int L = (int) paramv[0];
  u64 R = paramv[1], r;
  int l, b, f;
  for (r = 0; r < R; r++) {
    ocrGuid_t *res = (ocrGuid_t*) depv[L + r].ptr;
    u64 w = 0;
    for (l = 0; l < L; l++) {
      level_type *lp = (level_type*) depv[l].ptr;
      ocrGuid_t *boxes = (ocrGuid_t*)(((char*)lp) + lp->boxes);
      ocrGuid_t *faces = dist_face_guids(lp);
      for (b = 0; b < lp->num_boxes; b++) {
        if (boxHomePD(b, lp->boxes_in.i, R) != (int) r) continue;
        boxes[b] = res[w*7];
        for (f = 0; f < 6; f++)
          faces[b*6 + f] = res[w*7 + 1 + f];
        w++;
      }
    }
    ocrDbDestroy(depv[L + r].guid);
  }
  return NULL_GUID;
}

// Builds the whole initialization chain and returns the event the solve
// spine starts on.
ocrGuid_t dist_init(mg_type *mg_ptr, int box_dim0, int boxes_in_i0) {
  int box_dim[MG_MAXLEVELS], boxes_in_i[MG_MAXLEVELS];
  u64 dinv_off[MG_MAXLEVELS], l1inv_off[MG_MAXLEVELS];
  int L = dist_level_table(box_dim0, boxes_in_i0, box_dim, boxes_in_i);
  u64 R = dist_ranks();
  int l;
  u64 r;
  ocrHint_t h;

  mg_ptr->num_levels = L;
  mg_ptr->max_levels = L;

  double h0 = 1.0 / ((double) boxes_in_i0 * (double) box_dim0);
  for (l = 0; l < L; l++) {
    dist_create_level_db(mg_ptr, box_dim[l], boxes_in_i[l], l, h0);
    h0 *= 2.0;
    // the diagonal vectors' offsets, from the same creation-time formulas
    int jS = (box_dim[l] + 2*NUM_GHOSTS);
    int kS = jS * (box_dim[l] + 2*NUM_GHOSTS);
    if (jS < 8) kS += (8 - jS);
    u64 vol = (u64)(box_dim[l] + 2*NUM_GHOSTS) * kS;
    dinv_off[l] = sizeof(box_type) + 4*vol*sizeof(double);
    l1inv_off[l] = dinv_off[l] + vol*sizeof(double);
  }

  // creators, one per rank
  ocrGuid_t resEvts[DIST_MAX_RANKS], gates[DIST_MAX_RANKS];
  ocrGuid_t c_t, c;
  ocrEdtTemplateCreate(&c_t, dist_creator_edt, 3, L);
  for (r = 0; r < R; r++) {
    ocrEventCreate(&resEvts[r], OCR_EVENT_ONCE_T, 1);
    u64 pv[3] = { r, dist_guid_bits(resEvts[r]), (u64) L };
    ocrEdtCreate(&c, c_t, 3, pv, L, NULL, EDT_PROP_FINISH,
                 mgRankEdtHint(&h, r), &gates[r]);
    for (l = 0; l < L; l++)
      ocrAddDependence(mg_ptr->levels[l], c, l, DB_MODE_CONST);
  }
  ocrEdtTemplateDestroy(c_t);

  // merge, gated on every creator's finish (fills included)
  ocrGuid_t m, m_t, mOut;
  u64 mpv[2] = { (u64) L, R };
  ocrEdtTemplateCreate(&m_t, dist_merge_edt, 2, L + 2*R);
  ocrEdtCreate(&m, m_t, 2, mpv, L + 2*R, NULL, 0, mgHomeEdtHint(&h), &mOut);
  ocrEdtTemplateDestroy(m_t);
  for (l = 0; l < L; l++)
    ocrAddDependence(mg_ptr->levels[l], m, l, DB_MODE_RW);
  for (r = 0; r < R; r++)
    ocrAddDependence(resEvts[r], m, L + r, DB_MODE_CONST);
  for (r = 0; r < R; r++)
    ocrAddDependence(gates[r], m, L + R + r, DB_MODE_NULL);

  // operator build, level by level down the hierarchy
  ocrGuid_t chain = mOut;
  ocrGuid_t sp_t, red_t;
  ocrEdtTemplateCreate(&sp_t, dist_subphase_edt, 3, 3);
  ocrEdtTemplateCreate(&red_t, dist_reduce_subphase_edt, 3, 3);
  for (l = 0; l < L; l++) {
    ocrGuid_t lg = mg_ptr->levels[l];
    ocrGuid_t sub, subOut;

    if (l > 0) {  // restrict alpha/beta from the finer level
      u64 pv[3] = { DK_COEFF, 0, 0 };
      ocrEdtCreate(&sub, sp_t, 3, pv, 3, NULL, EDT_PROP_FINISH,
                   mgHomeEdtHint(&h), &subOut);
      ocrAddDependence(lg, sub, 0, DB_MODE_CONST);
      ocrAddDependence(mg_ptr->levels[l-1], sub, 1, DB_MODE_CONST);
      ocrAddDependence(chain, sub, 2, DB_MODE_NULL);
      chain = subOut;
    }

    {  // Gershgorin: Dinv/L1inv per box, eigenvalue reduced onto the level
      ocrGuid_t evts[DIST_MAX_RANKS];
      for (r = 0; r < R; r++)
        ocrEventCreate(&evts[r], OCR_EVENT_ONCE_T, 1);
      ocrGuid_t evg; ocrGuid_t *evp;
      ocrDbCreate(&evg, (void**)&evp, R * sizeof(ocrGuid_t), 0, NULL_HINT, NO_ALLOC);
      for (r = 0; r < R; r++) evp[r] = evts[r];
      ocrDbRelease(evg);

      ocrGuid_t red, gsubOut;
      u64 rpv[3] = { DK_GERSH, 0, 0 };
      ocrEdtCreate(&red, red_t, 3, rpv, 3, NULL, EDT_PROP_FINISH,
                   mgHomeEdtHint(&h), &gsubOut);
      ocrAddDependence(lg, red, 0, DB_MODE_CONST);
      ocrAddDependence(evg, red, 1, DB_MODE_CONST);
      ocrAddDependence(chain, red, 2, DB_MODE_NULL);

      ocrGuid_t fin, finOut, fe_t;
      ocrEdtTemplateCreate(&fe_t, dist_eigen_final_edt, 0, 2 + R);
      ocrEdtCreate(&fin, fe_t, 0, NULL, 2 + R, NULL, 0, mgHomeEdtHint(&h), &finOut);
      ocrEdtTemplateDestroy(fe_t);
      ocrAddDependence(lg, fin, 0, DB_MODE_RW);
      ocrAddDependence(gsubOut, fin, 1, DB_MODE_NULL);
      for (r = 0; r < R; r++)
        ocrAddDependence(evts[r], fin, 2 + r, DB_MODE_CONST);
      chain = finOut;
    }

    {  // ghost exchange of the smoother's diagonals over the face slabs
      u64 offs[2] = { dinv_off[l], l1inv_off[l] };
      int v;
      for (v = 0; v < 2; v++) {
        ocrGuid_t pk, pkOut, up, upOut;
        u64 ppv[3] = { DK_PACK, offs[v], 0 };
        u64 upv[3] = { DK_UNPACK, offs[v], 26 };
        ocrEdtCreate(&pk, sp_t, 3, ppv, 3, NULL, EDT_PROP_FINISH,
                     mgHomeEdtHint(&h), &pkOut);
        ocrAddDependence(lg, pk, 0, DB_MODE_CONST);
        ocrAddDependence(lg, pk, 1, DB_MODE_CONST);
        ocrAddDependence(chain, pk, 2, DB_MODE_NULL);
        ocrEdtCreate(&up, sp_t, 3, upv, 3, NULL, EDT_PROP_FINISH,
                     mgHomeEdtHint(&h), &upOut);
        ocrAddDependence(lg, up, 0, DB_MODE_CONST);
        ocrAddDependence(lg, up, 1, DB_MODE_CONST);
        ocrAddDependence(pkOut, up, 2, DB_MODE_NULL);
        chain = upOut;
      }
    }
  }
  ocrEdtTemplateDestroy(sp_t);
  ocrEdtTemplateDestroy(red_t);

  return chain;
}

/* ------------------------------------------------------------------ */
/* the solve lattice                                                   */
/*                                                                     */
/* The serial spine is replaced by one phase chain per rank over the   */
/* same total order, gated point-to-point: a phase that touches only   */
/* the rank's own boxes waits for the rank's own previous phase; the   */
/* exchange phases wait for the rank-grid neighbours (which bounds     */
/* chain skew at every halo hand-off and covers the one cross-rank     */
/* writer, prolongation into straddling fine boxes); level transitions */
/* and single-owner phases wait for everyone.  Each rank creates and   */
/* homes its own phase-completion events, so a hand-off is one direct  */
/* message from producer to consumer — no relay rank, no serial spine. */
/* Registration may trail satisfaction across ranks; events linger     */
/* after firing, so a late registration still sees the satisfy.        */
/* ------------------------------------------------------------------ */

ocrGuid_t dist_evmake_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t dist_evgather_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t dist_chain_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t dist_notify_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

// pars: the event to fire; deps: whatever gates it
ocrGuid_t dist_notify_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  ocrEventSatisfy(dist_guid_from(paramv[0]), NULL_GUID);
  return NULL_GUID;
}

static void dist_rank_grid(u64 R, int *px, int *py, int *pz) {
  boxFactor3((s64) R, px, py, pz);
}

// self plus the valid rank-grid 26-neighbourhood
static int dist_rank_nbrs(u64 r, u64 R, int *out) {
  int PX, PY, PZ;
  dist_rank_grid(R, &PX, &PY, &PZ);
  int x = r % PX, y = (r / PX) % PY, z = r / (PX*PY);
  int n = 0, dx, dy, dz;
  for (dz = -1; dz <= 1; dz++)
    for (dy = -1; dy <= 1; dy++)
      for (dx = -1; dx <= 1; dx++) {
        int nx = x+dx, ny = y+dy, nz = z+dz;
        if (nx < 0 || ny < 0 || nz < 0 || nx >= PX || ny >= PY || nz >= PZ)
          continue;
        out[n++] = nx + ny*PX + nz*PX*PY;
      }
  return n;
}

// The F-cycle phase sequence, mirrored from the spine construction (one
// timed solve; the per-operator timer links are no-ops here and dropped).
static int dist_schedule_build(int L, const int *boxes_in_i, u64 R,
                               dist_phase_t *s) {
  int PX, PY, PZ;
  dist_rank_grid(R, &PX, &PY, &PZ);
  int Pmax = PX > PY ? PX : PY; if (PZ > Pmax) Pmax = PZ;
  int n = 0, l, l2, it;

#define PH(k, la, lb, g, pp) do { \
    s[n].kind = (k); s[n].lvl = (la); s[n].lvl2 = (lb); \
    s[n].gate = (g); s[n].p = (pp); n++; } while (0)
#define XG(la) ((boxes_in_i[(la)] >= 2*Pmax) ? DG_NBR : DG_ALL)
#define SMOOTH4(la) do { for (it = 0; it < NUM_SMOOTHS*CHEBYSHEV_DEGREE; it++) { \
    PH(DK_PACK, (la), (la), XG(la), it); \
    PH(DK_UNPACK, (la), (la), XG(la), it); \
    PH(DK_SMOOTH, (la), (la), DG_OWN, it); } } while (0)

  PH(DK_TIME, 0, 0, DG_ALL, 4);
  PH(DK_INIT_UR, 0, 0, DG_OWN, L);
  PH(DK_INIT_UR, L-1, L-1, DG_OWN, L);
  for (l = 0; l < L-1; l++)
    PH(DK_RESTRICT, l, l+1, DG_ALL, 0);
  PH(DK_SOLVE, L-1, L-1, DG_ALL, 0);

  for (l = L-1; l >= 1; l--) {
    // FMG prolongation: 26-neighbour exchange of u, then interpolate
    PH(DK_PACK, l, l, XG(l), -1);
    PH(DK_UNPACK, l, l, XG(l), -1);
    PH(DK_INTERP, l-1, l, DG_ALL, FMG_INTERPOLATE);
    // vcycle(l-1)
    for (l2 = l-1; l2 < L-1; l2++) {
      SMOOTH4(l2);
      PH(DK_PACK, l2, l2, XG(l2), 0);
      PH(DK_UNPACK, l2, l2, XG(l2), 0);
      PH(DK_RESIDUAL, l2, l2, DG_OWN, 0);
      PH(DK_RESTRICT, l2, l2+1, DG_ALL, 1);
      PH(DK_ZERO, l2+1, l2+1, DG_OWN, 0);
    }
    PH(DK_SOLVE, L-1, L-1, DG_ALL, 0);
    for (l2 = L-1; l2 > l-1; l2--) {
      PH(DK_INTERP, l2-1, l2, DG_ALL, VC_INTERPOLATE);
      SMOOTH4(l2-1);
    }
  }

  PH(DK_PACK, 0, 0, XG(0), 0);
  PH(DK_UNPACK, 0, 0, XG(0), 0);
  PH(DK_RESIDUAL, 0, 0, DG_OWN, 1);
  PH(DK_MULV, 0, 0, DG_OWN, 0);
  PH(DK_NORM, 0, 0, DG_OWN, 0);
  PH(DK_NORMFINAL, 0, 0, DG_ALL, 0);
  PH(DK_TIME, 0, 0, DG_ALL, 4);

#undef SMOOTH4
#undef XG
#undef PH
  return n;
}

// pars: nphases - result event; creates this rank's phase events
ocrGuid_t dist_evmake_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  u64 np = paramv[0], i;
  ocrGuid_t resGuid; ocrGuid_t *res;
  ocrDbCreate(&resGuid, (void**)&res, np * sizeof(ocrGuid_t), 0, NULL_HINT, NO_ALLOC);
  for (i = 0; i < np; i++)
    ocrEventCreate(&res[i], OCR_EVENT_ONCE_T, 1);
  ocrDbRelease(resGuid);
  ocrEventSatisfy(dist_guid_from(paramv[1]), resGuid);
  return NULL_GUID;
}

// pars: nphases - R - table event; deps: R per-rank event-guid DBs
ocrGuid_t dist_evgather_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  u64 np = paramv[0], R = paramv[1], r, i;
  ocrGuid_t tabGuid; ocrGuid_t *tab;
  ocrDbCreate(&tabGuid, (void**)&tab, R * np * sizeof(ocrGuid_t), 0,
              NULL_HINT, NO_ALLOC);
  for (r = 0; r < R; r++) {
    ocrGuid_t *res = (ocrGuid_t*) depv[r].ptr;
    for (i = 0; i < np; i++)
      tab[r*np + i] = res[i];
    ocrDbDestroy(depv[r].guid);
  }
  ocrDbRelease(tabGuid);
  ocrEventSatisfy(dist_guid_from(paramv[2]), tabGuid);
  return NULL_GUID;
}

// pars: rank - start event - terminal event - L; deps: event table - levels
// Builds this rank's whole chain: every phase's task(s), gated on the
// previous phase's events per the schedule's gate class.
ocrGuid_t dist_chain_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  u64 r = paramv[0], R = dist_ranks();
  ocrGuid_t startEvt = dist_guid_from(paramv[1]);
  ocrGuid_t termEvt = dist_guid_from(paramv[2]);
  int L = (int) paramv[3];
  ocrGuid_t *tab = (ocrGuid_t*) depv[0].ptr;

  dist_phase_t sched[DIST_MAX_PHASES];
  int boxes_in_i[MG_MAXLEVELS];
  int l;
  for (l = 0; l < L; l++)
    boxes_in_i[l] = ((level_type*) depv[1+l].ptr)->boxes_in.i;
  int np = dist_schedule_build(L, boxes_in_i, R, sched);

  int nbrs[27];
  int nnbr = dist_rank_nbrs(r, R, nbrs);
  ocrHint_t h;
  ocrGuid_t n_t, s_t;
  ocrEdtTemplateCreate(&n_t, dist_notify_edt, 1, 1);   // depc overridden per use

  int i, g;
  for (i = 0; i < np; i++) {
    dist_phase_t *p = &sched[i];
    level_type *lA = (level_type*) depv[1 + p->lvl].ptr;
    ocrGuid_t lgA = depv[1 + p->lvl].guid;
    ocrGuid_t lgB = depv[1 + p->lvl2].guid;
    ocrGuid_t ev = tab[r*np + i];

    // the gate set: this phase's predecessors' completion events
    ocrGuid_t gates[DIST_MAX_RANKS];
    int ng = 0;
    if (i == 0) {
      gates[ng++] = startEvt;
    } else if (p->gate == DG_OWN) {
      gates[ng++] = tab[r*np + (i-1)];
    } else if (p->gate == DG_NBR) {
      for (g = 0; g < nnbr; g++)
        gates[ng++] = tab[((u64)nbrs[g])*np + (i-1)];
    } else {
      u64 r2;
      for (r2 = 0; r2 < R; r2++)
        gates[ng++] = tab[r2*np + (i-1)];
    }

    if (p->kind == DK_TIME || p->kind == DK_SOLVE || p->kind == DK_NORMFINAL) {
      // single-owner phases: rank 0 runs the task, everyone else just
      // reports its (empty) phase done
      if (r != 0) {
        ocrGuid_t nf; u64 evb = dist_guid_bits(ev);
        ocrEdtCreate(&nf, n_t, 1, &evb, 1, NULL, 0, mgRankEdtHint(&h, r), NULL);
        ocrAddDependence(i ? tab[r*np + (i-1)] : startEvt, nf, 0, DB_MODE_NULL);
        continue;
      }
      ocrGuid_t t, tOut;
      if (p->kind == DK_TIME) {
        u64 pp = (u64) p->p;
        ocrEdtTemplateCreate(&s_t, time_edt, 1, 1 + ng);
        ocrEdtCreate(&t, s_t, 1, &pp, 1 + ng, NULL, 0, mgRankEdtHint(&h, r), &tOut);
        ocrAddDependence(depv[1 + 0].guid, t, 0, DB_MODE_RW);
        for (g = 0; g < ng; g++)
          ocrAddDependence(gates[g], t, 1 + g, DB_MODE_NULL);
      } else if (p->kind == DK_SOLVE) {
        level_type *lc = (level_type*) depv[1 + (L-1)].ptr;
        ocrGuid_t cbox = ((ocrGuid_t*)(((char*)lc) + lc->boxes))[0];
        ocrEdtTemplateCreate(&s_t, solve_edt, 0, 2 + ng);
        ocrEdtCreate(&t, s_t, 0, NULL, 2 + ng, NULL, 0, mgRankEdtHint(&h, r), &tOut);
        ocrAddDependence(depv[1 + (L-1)].guid, t, 0, DB_MODE_CONST);
        ocrAddDependence(cbox, t, 1, DB_MODE_RW);
        for (g = 0; g < ng; g++)
          ocrAddDependence(gates[g], t, 2 + g, DB_MODE_NULL);
      } else { // DK_NORMFINAL: the gates carry the per-rank norm partials
        ocrEdtTemplateCreate(&s_t, dist_norm_final_edt, 0, ng);
        ocrEdtCreate(&t, s_t, 0, NULL, ng, NULL, 0, mgRankEdtHint(&h, r), &tOut);
        for (g = 0; g < ng; g++)
          ocrAddDependence(gates[g], t, g, DB_MODE_CONST);
      }
      ocrEdtTemplateDestroy(s_t);
      ocrGuid_t nf; u64 evb = dist_guid_bits(ev);
      ocrEdtCreate(&nf, n_t, 1, &evb, 1, NULL, 0, mgRankEdtHint(&h, r), NULL);
      ocrAddDependence(tOut, nf, 0, DB_MODE_NULL);
      continue;
    }

    // fan-out phases: one slice, gated; its finish reports the phase.
    // The norm reduction's local merge satisfies the phase event itself
    // (carrying the rank's partial), so it takes no notifier.
    u64 pv[6] = { p->kind, 0, 0, 0, r, 0 };
    switch (p->kind) {
    case DK_INIT_UR: pv[1] = (u64) p->p; break;
    case DK_SMOOTH: pv[1] = (u64) p->p; break;
    case DK_RESIDUAL: pv[1] = (u64)(p->p ? 1 : 0); pv[2] = 1; break;
    case DK_ZERO: pv[1] = 0; break;
    case DK_MULV: break;
    case DK_PACK:
    case DK_UNPACK:
      pv[1] = (p->p < 0) ? (u64) lA->u
            : (((p->p & 1) == 0) ? (u64) lA->u : (u64) lA->vec_temp);
      if (p->kind == DK_UNPACK) pv[2] = (p->p < 0) ? 26 : 6;
      break;
    case DK_RESTRICT: pv[1] = (u64) p->p; pv[2] = 0; break;
    case DK_INTERP: pv[1] = (u64) p->p; break;
    case DK_NORM: pv[3] = dist_guid_bits(ev); break;
    }

    ocrGuid_t sl, slOut;
    ocrEdtTemplateCreate(&s_t, dist_slice_edt, 6, 2 + ng);
    ocrEdtCreate(&sl, s_t, 6, pv, 2 + ng, NULL, EDT_PROP_FINISH,
                 mgRankEdtHint(&h, r), &slOut);
    ocrEdtTemplateDestroy(s_t);
    // restrict/interpolate take (fine, coarse) as (A, B)
    ocrAddDependence(lgA, sl, 0, DB_MODE_CONST);
    ocrAddDependence(lgB, sl, 1, DB_MODE_CONST);
    for (g = 0; g < ng; g++)
      ocrAddDependence(gates[g], sl, 2 + g, DB_MODE_NULL);

    if (p->kind != DK_NORM) {
      ocrGuid_t nf; u64 evb = dist_guid_bits(ev);
      ocrEdtCreate(&nf, n_t, 1, &evb, 1, NULL, 0, mgRankEdtHint(&h, r), NULL);
      ocrAddDependence(slOut, nf, 0, DB_MODE_NULL);
    }
  }

  // rank 0 hangs the terminal on everyone's last phase
  if (r == 0) {
    ocrGuid_t nf; u64 evb = dist_guid_bits(termEvt);
    u64 r2;
    ocrGuid_t t_t;
    ocrEdtTemplateCreate(&t_t, dist_notify_edt, 1, R);
    ocrEdtCreate(&nf, t_t, 1, &evb, R, NULL, 0, mgRankEdtHint(&h, r), NULL);
    for (r2 = 0; r2 < R; r2++)
      ocrAddDependence(tab[r2*np + (np-1)], nf, r2, DB_MODE_NULL);
    ocrEdtTemplateDestroy(t_t);
  }

  ocrEdtTemplateDestroy(n_t);
  return NULL_GUID;
}

// The spine replacement: same signature and contract as the serial
// builder, returning the event the caller's continuation hangs on.
ocrGuid_t dist_solves(ocrGuid_t start, mg_type *mg_ptr, int num, int warmup) {
  if (num <= 0) return start;
  u64 R = dist_ranks(), r;
  int L = mg_ptr->max_levels;
  int boxes_in_i[MG_MAXLEVELS];
  int l;
  // schedule length only; the chain builders re-derive the details
  dist_phase_t tmp[DIST_MAX_PHASES];
  for (l = 0; l < L; l++) boxes_in_i[l] = 1;
  int np = dist_schedule_build(L, boxes_in_i, R, tmp);
  ocrHint_t h;

  ocrGuid_t resEvts[DIST_MAX_RANKS];
  ocrGuid_t e_t, e;
  ocrEdtTemplateCreate(&e_t, dist_evmake_edt, 2, 1);
  for (r = 0; r < R; r++) {
    ocrEventCreate(&resEvts[r], OCR_EVENT_ONCE_T, 1);
    u64 pv[2] = { (u64) np, dist_guid_bits(resEvts[r]) };
    ocrEdtCreate(&e, e_t, 2, pv, 1, NULL, 0, mgRankEdtHint(&h, r), NULL);
    ocrAddDependence(NULL_GUID, e, 0, DB_MODE_NULL);
  }
  ocrEdtTemplateDestroy(e_t);

  ocrGuid_t tabEvt;
  ocrEventCreate(&tabEvt, OCR_EVENT_ONCE_T, 1);
  ocrGuid_t g_t, gth;
  u64 gpv[3] = { (u64) np, R, dist_guid_bits(tabEvt) };
  ocrEdtTemplateCreate(&g_t, dist_evgather_edt, 3, R);
  ocrEdtCreate(&gth, g_t, 3, gpv, R, NULL, 0, mgHomeEdtHint(&h), NULL);
  for (r = 0; r < R; r++)
    ocrAddDependence(resEvts[r], gth, r, DB_MODE_CONST);
  ocrEdtTemplateDestroy(g_t);

  ocrGuid_t termEvt;
  ocrEventCreate(&termEvt, OCR_EVENT_ONCE_T, 1);

  ocrGuid_t c_t, c;
  ocrEdtTemplateCreate(&c_t, dist_chain_edt, 4, 1 + L);
  for (r = 0; r < R; r++) {
    u64 pv[4] = { r, dist_guid_bits(start), dist_guid_bits(termEvt), (u64) L };
    ocrEdtCreate(&c, c_t, 4, pv, 1 + L, NULL, 0, mgRankEdtHint(&h, r), NULL);
    ocrAddDependence(tabEvt, c, 0, DB_MODE_CONST);
    for (l = 0; l < L; l++)
      ocrAddDependence(mg_ptr->levels[l], c, 1 + l, DB_MODE_CONST);
  }
  ocrEdtTemplateDestroy(c_t);
  (void) warmup;
  return termEvt;
}

/* ------------------------------------------------------------------ */
/* finalize                                                            */
/* ------------------------------------------------------------------ */

// deps: mg - level0 - solve chain
// Timing table, then a distributed error reduction, then shutdown.
ocrGuid_t finalize_dist(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  mg_type *mg_ptr = (mg_type*) depv[0].ptr;
  u64 num_levels = mg_ptr->num_levels;
  u64 R = dist_ranks(), r;
  ocrHint_t h;
  int b;

  ocrGuid_t tm, tm_t, tmOut;
  ocrEdtTemplateCreate(&tm_t, print_timing_edt, 1, num_levels);
  ocrEdtCreate(&tm, tm_t, 1, &num_levels, num_levels, NULL, 0,
               mgHomeEdtHint(&h), &tmOut);
  ocrEdtTemplateDestroy(tm_t);

  ocrGuid_t evts[DIST_MAX_RANKS];
  for (r = 0; r < R; r++)
    ocrEventCreate(&evts[r], OCR_EVENT_ONCE_T, 1);
  ocrGuid_t evg; ocrGuid_t *evp;
  ocrDbCreate(&evg, (void**)&evp, R * sizeof(ocrGuid_t), 0, NULL_HINT, NO_ALLOC);
  for (r = 0; r < R; r++) evp[r] = evts[r];
  ocrDbRelease(evg);

  ocrGuid_t red, red_t, redOut;
  u64 rpv[3] = { DK_ERROR, 0, 0 };
  ocrEdtTemplateCreate(&red_t, dist_reduce_subphase_edt, 3, 3);
  ocrEdtCreate(&red, red_t, 3, rpv, 3, NULL, EDT_PROP_FINISH,
               mgHomeEdtHint(&h), &redOut);
  ocrEdtTemplateDestroy(red_t);

  ocrGuid_t fin, fin_t, finOut;
  ocrEdtTemplateCreate(&fin_t, dist_error_final_edt, 0, 2 + R);
  ocrEdtCreate(&fin, fin_t, 0, NULL, 2 + R, NULL, 0, mgHomeEdtHint(&h), &finOut);
  ocrEdtTemplateDestroy(fin_t);
  ocrAddDependence(mg_ptr->levels[0], fin, 0, DB_MODE_CONST);
  ocrAddDependence(redOut, fin, 1, DB_MODE_NULL);
  for (r = 0; r < R; r++)
    ocrAddDependence(evts[r], fin, 2 + r, DB_MODE_CONST);

  ocrGuid_t sd, sd_t;
  ocrEdtTemplateCreate(&sd_t, shutdown_edt, 0, 1);
  ocrEdtCreate(&sd, sd_t, 0, NULL, 1, NULL, 0, mgHomeEdtHint(&h), NULL);
  ocrEdtTemplateDestroy(sd_t);
  ocrAddDependence(finOut, sd, 0, DB_MODE_NULL);

  ocrAddDependence(mg_ptr->levels[0], red, 0, DB_MODE_CONST);
  ocrAddDependence(evg, red, 1, DB_MODE_CONST);
  ocrAddDependence(tmOut, red, 2, DB_MODE_NULL);

  for (b = 0; b < (int) num_levels; b++)
    ocrAddDependence(mg_ptr->levels[b], tm, b, DB_MODE_CONST);

  return NULL_GUID;
}
