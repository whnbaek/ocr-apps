#include <ocr.h>

#include "simulation.h"
#include "cells.h"

/* A node's range is the rank interval [RLO, RHI) while that spans more than
 * one rank, and the brick-local cell interval [LO, HI) of rank RLO once it
 * spans exactly one.  The tree fans over ranks before it fans over cells, so
 * no node below a brick straddles an ownership boundary. */
enum { FP_PHASE, FP_RLO, FP_RHI, FP_LO, FP_HI, FP_SLOT, FP_A0, FP_A1, FP_A2, FP_A3,
       FP_PARENT, FP_NPARAM = FP_PARENT + GUID_WORDS };
enum { SP_SLOT, SP_PARENT, SP_NPARAM = SP_PARENT + GUID_WORDS };

static ocrGuid_t fan_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

static inline u8 phase_reduces(u32 phase)
{
  return phase == PH_STEP;
}

/* What a brick's range is made of: its cells, or the halo blocks it packs and
 * reclaims.  Both are brick-local intervals, so one tree serves either. */
static inline u32 phase_items(const config* c, u32 phase, u32 rank)
{
  return phase == PH_HALO || phase == PH_HALO_FREE ? brick_halo(c, rank)
                                                   : brick_cells(c, rank);
}

/* An interior or leaf collector for a reducing phase: it adds what its
 * children contributed and hands one contribution to its own parent, so no
 * task ever holds a dependence per cell.  Inputs are added in slot order,
 * and slots are assigned in ascending cell index. */
static ocrGuid_t sum_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  real_t pe = 0.0, ke = 0.0;
  u64 n = 0;
  for(u32 i = 0; i < depc; ++i) {
    const partial* p = (const partial*)depv[i].ptr;
    pe += p->pe; ke += p->ke; n += p->n;
    ocrDbDestroy(depv[i].guid);
  }
  ocrGuid_t out; partial* out_p;
  ocrDbCreate(&out, (void**)&out_p, sizeof(partial), 0, NULL_HINT, NO_ALLOC);
  out_p->pe = pe; out_p->ke = ke; out_p->n = n;
  ocrDbRelease(out);
  ocrAddDependence(out, guid_load(paramv+SP_PARENT), (u32)paramv[SP_SLOT], DB_MODE_CONST);

  return NULL_GUID;
}

/* The same collector for a phase that carries no value: its completion is
 * the only thing its parent waits for. */
static ocrGuid_t join_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  return NULL_GUID;
}

/* A collector over a leaf's range holds one dependence per cell, and the leaf
 * width is a run-time knob: a create that exceeds the implementation's
 * dependence limit leaves its GUID unwritten, so the failure must be caught
 * where it happens rather than propagated as a GUID. */
static ocrGuid_t node_create(const config* c, u8 reduces, u32 n,
                            ocrGuid_t parent, u32 slot, u32 rank)
{
  ocrGuid_t tmp, node;
  ocrHint_t edtHNT;
  if(reduces) {
    u64 pv[SP_NPARAM];
    pv[SP_SLOT] = slot;
    guid_store(pv+SP_PARENT, parent);
    if(ocrEdtTemplateCreate(&tmp, sum_edt, SP_NPARAM, n) ||
       ocrEdtCreate(&node, tmp, SP_NPARAM, pv, n, NULL, 0,
                    edt_at(&edtHNT, c, rank), NULL)) {
      ocrPrintf("ERROR cannot create a join EDT with %u dependences TERMINATING\n", n);
      ocrShutdown();
      return NULL_GUID;
    }
  } else {
    ocrGuid_t done;
    OEVT_COUNTED_PRE(done);
    if(ocrEdtTemplateCreate(&tmp, join_edt, 0, n) ||
       ocrEdtCreate(&node, tmp, 0, NULL, n, NULL, OEVT_COUNTED_PROP(0),
                    edt_at(&edtHNT, c, rank), &done)) {
      ocrPrintf("ERROR cannot create a join EDT with %u dependences TERMINATING\n", n);
      ocrShutdown();
      return NULL_GUID;
    }
    ocrAddDependence(done, parent, slot, DB_MODE_CONST);
  }
  ocrEdtTemplateDestroy(tmp);
  return node;
}

/* A child covering the rank interval [rlo, rhi); once that is one rank the
 * child's interval is that rank's whole brick. */
static void child_ranks(const config* c, u32 phase, u64* pv, u32 rlo, u32 rhi)
{
  pv[FP_RLO] = rlo; pv[FP_RHI] = rhi;
  pv[FP_LO] = 0; pv[FP_HI] = rhi - rlo == 1 ? phase_items(c, phase, rlo) : 0;
}

/* One phase's fan-out is a tree: every node emits only its own subrange, so
 * the rate at which the frontier can be issued grows with the machine
 * instead of being one task's serial loop.  A node and its collector sit on
 * the first rank of the node's range, which below a brick is the owner of
 * every cell the node covers. */
static ocrGuid_t fan_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  const config* c = (const config*)depv[0].ptr;
  const cell_list* l = (const cell_list*)depv[1].ptr;
  u32 phase = (u32)paramv[FP_PHASE];
  u32 rlo = (u32)paramv[FP_RLO], rhi = (u32)paramv[FP_RHI];
  u32 lo = (u32)paramv[FP_LO], hi = (u32)paramv[FP_HI];
  ocrGuid_t parent = guid_load(paramv+FP_PARENT);
  u32 slot = (u32)paramv[FP_SLOT];
  u8 reduces = phase_reduces(phase);
  u32 rank = rlo;

  if(rhi - rlo > 1) {
    u32 k = rhi - rlo < FAN_BRANCH ? rhi - rlo : FAN_BRANCH;
    ocrGuid_t node = node_create(c, reduces, k, parent, slot, rank);
    if(ocrGuidIsNull(node)) return NULL_GUID;
    ocrGuid_t tmp;
    ocrHint_t edtHNT;
    ocrEdtTemplateCreate(&tmp, fan_edt, FP_NPARAM, 2);
    for(u32 i = 0; i < k; ++i) {
      u64 pv[FP_NPARAM];
      memcpy(pv, paramv, sizeof(pv));
      child_ranks(c, phase, pv, rank_split(rlo, rhi, k, i), rank_split(rlo, rhi, k, i+1));
      pv[FP_SLOT] = i;
      guid_store(pv+FP_PARENT, node);
      ocrGuid_t child;
      ocrEdtCreate(&child, tmp, FP_NPARAM, pv, 2, NULL, 0,
                   edt_at(&edtHNT, c, (u32)pv[FP_RLO]), NULL);
      if(i+1 == k) { ocrDbRelease(depv[0].guid); ocrDbRelease(depv[1].guid); }
      ocrAddDependence(depv[0].guid, child, 0, DB_MODE_CONST);
      ocrAddDependence(depv[1].guid, child, 1, DB_MODE_CONST);
    }
    ocrEdtTemplateDestroy(tmp);
    return NULL_GUID;
  }

  u32 n = hi - lo;
  if(n > c->width) {
    /* Split on whole leaf widths rather than on the range: every level then
     * cuts a number of leaves, so the leaf count of a range is exactly the
     * range divided by the leaf width however deep the recursion runs. */
    u32 u = (n + c->width - 1)/c->width;
    u32 k = u < FAN_BRANCH ? u : FAN_BRANCH;
    ocrGuid_t node = node_create(c, reduces, k, parent, slot, rank);
    if(ocrGuidIsNull(node)) return NULL_GUID;
    ocrGuid_t tmp;
    ocrHint_t edtHNT;
    ocrEdtTemplateCreate(&tmp, fan_edt, FP_NPARAM, 2);
    for(u32 i = 0; i < k; ++i) {
      u32 clo = lo + (u32)(((u64)u * i) / k) * c->width;
      u32 chi = lo + (u32)(((u64)u * (i+1)) / k) * c->width;
      if(chi > hi) chi = hi;
      u64 pv[FP_NPARAM];
      memcpy(pv, paramv, sizeof(pv));
      pv[FP_LO] = clo; pv[FP_HI] = chi; pv[FP_SLOT] = i;
      guid_store(pv+FP_PARENT, node);
      ocrGuid_t child;
      ocrEdtCreate(&child, tmp, FP_NPARAM, pv, 2, NULL, 0,
                   edt_at(&edtHNT, c, rank), NULL);
      if(i+1 == k) { ocrDbRelease(depv[0].guid); ocrDbRelease(depv[1].guid); }
      ocrAddDependence(depv[0].guid, child, 0, DB_MODE_CONST);
      ocrAddDependence(depv[1].guid, child, 1, DB_MODE_CONST);
    }
    ocrEdtTemplateDestroy(tmp);
    return NULL_GUID;
  }

  spawn_ctx x;
  x.cfg = depv[0].guid; x.c = c;
  x.list = depv[1].guid; x.l = l;
  x.rank = rank;
  rank_brick(c, rank, x.blo, x.bdim);
  x.join = node_create(c, reduces, n, parent, slot, rank);
  if(ocrGuidIsNull(x.join)) return NULL_GUID;
  x.arg = paramv + FP_A0;

  switch(phase) {
  case PH_TEMP:      spawn_temp(&x, lo, hi);      break;
  case PH_STEP:      spawn_step(&x, lo, hi);      break;
  case PH_HALO:      spawn_halo(&x, lo, hi);      break;
  case PH_HALO_FREE: spawn_halo_free(&x, lo, hi); break;
  default:           spawn_release(&x, lo, hi);   break;
  }

  return NULL_GUID;
}

void fan_launch(u32 phase, ocrGuid_t cfg, const config* c, ocrGuid_t list,
                ocrGuid_t parent, u32 slot, const u64* arg)
{
  u64 pv[FP_NPARAM];
  pv[FP_PHASE] = phase; pv[FP_SLOT] = slot;
  child_ranks(c, phase, pv, 0, c->nranks);
  for(u32 i = 0; i < 4; ++i) pv[FP_A0+i] = arg ? arg[i] : 0;
  guid_store(pv+FP_PARENT, parent);

  ocrGuid_t tmp, edt;
  ocrHint_t edtHNT;
  ocrEdtTemplateCreate(&tmp, fan_edt, FP_NPARAM, 2);
  ocrEdtCreate(&edt, tmp, FP_NPARAM, pv, 2, NULL, 0, edt_at(&edtHNT, c, 0), NULL);
  /* Every caller holds both blocks and reads neither after launching the
   * phase, whose completion leads to their destroy. */
  ocrDbRelease(cfg);
  ocrDbRelease(list);
  ocrAddDependence(cfg, edt, 0, DB_MODE_CONST);
  ocrAddDependence(list, edt, 1, DB_MODE_CONST);
  ocrEdtTemplateDestroy(tmp);
}
