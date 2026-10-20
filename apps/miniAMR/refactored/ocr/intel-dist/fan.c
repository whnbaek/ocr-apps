#include <stdlib.h>
#include <ocr.h>

#include "amr.h"

/* A node's range is the rank interval [RLO, RHI) while that spans more than
 * one rank, and the block interval [LO, HI) of rank RLO's own share of the
 * directory once it spans exactly one.  The tree fans over ranks before it
 * fans over blocks, so no node below a rank straddles an ownership boundary
 * and every task a leaf emits is placed where the blocks it names live. */
enum { FP_PHASE, FP_RLO, FP_RHI, FP_LO, FP_HI, FP_SLOT, FP_A0, FP_A1, FP_A2,
       FP_PARENT, FP_NPARAM = FP_PARENT + GUID_WORDS };
enum { CP_SLOT, CP_LEAF, CP_PARENT, CP_NPARAM = CP_PARENT + GUID_WORDS };
#define FP_HI_UNSET 0xFFFFFFFFu

enum { FD_MESH, FD_GUIDS, FD_OMESH, FD_OGUIDS, FD_DEPC };

static ocrGuid_t fan_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

static inline u8 phase_collects(u32 phase)
{
  return phase == PH_BUILD;
}

/* What a rank's range is made of: the blocks of the epoch being opened, or —
 * for the pass that reclaims the epoch it replaced — the blocks of that one. */
static u32 phase_items(const mesh* m, const mesh* om, u32 phase, u32 rank)
{
  const mesh* s = (phase == PH_RELEASE) ? om : m;
  const u32* r = mesh_rstart(s);
  return r[rank+1] - r[rank];
}

/* A phase whose tasks each produce one block's datablock handles: the tree
 * concatenates them in range order, so the array the root hands up is the
 * directory's own order and no task ever writes into another's slot. */
static ocrGuid_t cat_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  u32 leaf = (u32)paramv[CP_LEAF];
  u32 n = 0;
  for(u32 i = 0; i < depc; ++i)
    n += leaf ? 1 : ((const gslice*)depv[i].ptr)->n;

  ocrGuid_t out;
  gslice* p;
  ocrDbCreate(&out, (void**)&p, sizeof(gslice) + (u64)n*sizeof(bguid), 0, NULL_HINT, NO_ALLOC);
  p->n = n;
  u32 k = 0;
  for(u32 i = 0; i < depc; ++i) {
    if(leaf) {
      p->g[k++] = *(const bguid*)depv[i].ptr;
    } else {
      const gslice* s = (const gslice*)depv[i].ptr;
      memcpy(p->g + k, s->g, (size_t)s->n*sizeof(bguid));
      k += s->n;
    }
    ocrDbDestroy(depv[i].guid);
  }
  ocrDbRelease(out);
  ocrAddDependence(out, guid_load(paramv+CP_PARENT), (u32)paramv[CP_SLOT], DB_MODE_CONST);

  return NULL_GUID;
}

/// A phase that carries no value: its completion is all its parent waits for.
static ocrGuid_t join_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  return NULL_GUID;
}

/* A collector over a leaf's range holds one dependence per block, and the leaf
 * width is a run-time knob: a create the implementation refuses leaves its
 * GUID unwritten, so the failure is caught where it happens. */
static ocrGuid_t node_create(const control* c, u8 collects, u8 leaf, u32 n,
                             ocrGuid_t parent, u32 slot, u32 rank)
{
  ocrGuid_t tmp, node;
  ocrHint_t hnt;
  if(collects) {
    u64 pv[CP_NPARAM];
    pv[CP_SLOT] = slot;
    pv[CP_LEAF] = leaf;
    guid_store(pv+CP_PARENT, parent);
    if(ocrEdtTemplateCreate(&tmp, cat_edt, CP_NPARAM, n) ||
       ocrEdtCreate(&node, tmp, CP_NPARAM, pv, n, NULL, 0, edt_at(&hnt, c, rank), NULL)) {
      ocrPrintf("ERROR cannot create a collector with %u dependences TERMINATING\n", n);
      ocrShutdown();
      return NULL_GUID;
    }
  } else {
    ocrGuid_t done;
    OEVT_COUNTED_PRE(done);
    if(ocrEdtTemplateCreate(&tmp, join_edt, 0, n) ||
       ocrEdtCreate(&node, tmp, 0, NULL, n, NULL, OEVT_COUNTED_PROP(0),
                    edt_at(&hnt, c, rank), &done)) {
      ocrPrintf("ERROR cannot create a collector with %u dependences TERMINATING\n", n);
      ocrShutdown();
      return NULL_GUID;
    }
    ocrAddDependence(done, parent, slot, DB_MODE_CONST);
  }
  ocrEdtTemplateDestroy(tmp);
  return node;
}

/* Boundary i of the k-way split of the rank interval [rlo, rhi).  The tree
 * that fans out and the collector that folds back must cut identically. */
static inline u32 rank_split(u32 rlo, u32 rhi, u32 k, u32 i)
{
  return rlo + (u32)(((u64)(rhi - rlo)*i) / k);
}

static void child_ranks(u64* pv, u32 rlo, u32 rhi)
{
  pv[FP_RLO] = rlo; pv[FP_RHI] = rhi;
  pv[FP_LO] = 0;
  /* A node that covers exactly one rank reads that rank's item count out of
   * the directory it is handed, so the launch site needs no directory of its
   * own. */
  pv[FP_HI] = rhi - rlo == 1 ? FP_HI_UNSET : 0;
}

/* One phase's fan-out is a tree: every node emits only its own subrange, so
 * the rate at which a phase reaches the machine grows with the machine
 * instead of being one task's serial loop. */
static ocrGuid_t fan_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  const mesh* m = (const mesh*)depv[FD_MESH].ptr;
  const mesh* om = depv[FD_OMESH].ptr ? (const mesh*)depv[FD_OMESH].ptr : NULL;
  /* A copy: a node that only forwards the directories releases them before
   * its first hand-off and still needs the record for placement. */
  control cl = m->c;
  const control* c = &cl;
  u32 phase = (u32)paramv[FP_PHASE];
  u32 rlo = (u32)paramv[FP_RLO], rhi = (u32)paramv[FP_RHI];
  u32 lo = (u32)paramv[FP_LO], hi = (u32)paramv[FP_HI];
  if(hi == FP_HI_UNSET) hi = phase_items(m, om, phase, rlo);
  ocrGuid_t parent = guid_load(paramv+FP_PARENT);
  u32 slot = (u32)paramv[FP_SLOT];
  u8 collects = phase_collects(phase);
  u32 rank = rlo;

  if(rhi - rlo > 1) {
    for(u32 d = 0; d < FD_DEPC; ++d)
      if(!ocrGuidIsNull(depv[d].guid)) ocrDbRelease(depv[d].guid);
    u32 k = rhi - rlo < FAN_BRANCH ? rhi - rlo : FAN_BRANCH;
    ocrGuid_t node = node_create(c, collects, 0, k, parent, slot, rank);
    if(ocrGuidIsNull(node)) return NULL_GUID;
    ocrGuid_t tmp;
    ocrHint_t hnt;
    ocrEdtTemplateCreate(&tmp, fan_edt, FP_NPARAM, FD_DEPC);
    for(u32 i = 0; i < k; ++i) {
      u64 pv[FP_NPARAM];
      memcpy(pv, paramv, sizeof(pv));
      child_ranks(pv, rank_split(rlo, rhi, k, i), rank_split(rlo, rhi, k, i+1));
      pv[FP_SLOT] = i;
      guid_store(pv+FP_PARENT, node);
      ocrGuid_t child;
      ocrEdtCreate(&child, tmp, FP_NPARAM, pv, FD_DEPC, NULL, 0,
                   edt_at(&hnt, c, (u32)pv[FP_RLO]), NULL);
      for(u32 d = 0; d < FD_DEPC; ++d)
        ocrAddDependence(depv[d].guid, child, d,
                         ocrGuidIsNull(depv[d].guid) ? DB_MODE_NULL : DB_MODE_CONST);
    }
    ocrEdtTemplateDestroy(tmp);
    return NULL_GUID;
  }

  u32 n = hi - lo;
  if(n == 0) {
    /* A rank with nothing to do still has to report, or the phase's collector
     * would wait on a slot nobody ever fills. */
    for(u32 d = 0; d < FD_DEPC; ++d)
      if(!ocrGuidIsNull(depv[d].guid)) ocrDbRelease(depv[d].guid);
    ocrGuid_t node = node_create(c, collects, 1, 0, parent, slot, rank);
    (void)node;
    return NULL_GUID;
  }
  if(n > c->width) {
    /* Split on whole leaf widths, so a range's leaf count is exactly the range
     * over the leaf width however deep the recursion runs. */
    u32 u = (n + c->width - 1)/c->width;
    u32 k = u < FAN_BRANCH ? u : FAN_BRANCH;
    for(u32 d = 0; d < FD_DEPC; ++d)
      if(!ocrGuidIsNull(depv[d].guid)) ocrDbRelease(depv[d].guid);
    ocrGuid_t node = node_create(c, collects, 0, k, parent, slot, rank);
    if(ocrGuidIsNull(node)) return NULL_GUID;
    ocrGuid_t tmp;
    ocrHint_t hnt;
    ocrEdtTemplateCreate(&tmp, fan_edt, FP_NPARAM, FD_DEPC);
    for(u32 i = 0; i < k; ++i) {
      u32 clo = lo + (u32)(((u64)u*i) / k)*c->width;
      u32 chi = lo + (u32)(((u64)u*(i+1)) / k)*c->width;
      if(chi > hi) chi = hi;
      u64 pv[FP_NPARAM];
      memcpy(pv, paramv, sizeof(pv));
      pv[FP_LO] = clo; pv[FP_HI] = chi; pv[FP_SLOT] = i;
      guid_store(pv+FP_PARENT, node);
      ocrGuid_t child;
      ocrEdtCreate(&child, tmp, FP_NPARAM, pv, FD_DEPC, NULL, 0, edt_at(&hnt, c, rank), NULL);
      for(u32 d = 0; d < FD_DEPC; ++d)
        ocrAddDependence(depv[d].guid, child, d,
                         ocrGuidIsNull(depv[d].guid) ? DB_MODE_NULL : DB_MODE_CONST);
    }
    ocrEdtTemplateDestroy(tmp);
    return NULL_GUID;
  }

  spawn_ctx x;
  x.mesh = depv[FD_MESH].guid;   x.m = m;
  x.guids = depv[FD_GUIDS].guid; x.g = depv[FD_GUIDS].ptr ? ((const gslice*)depv[FD_GUIDS].ptr)->g : NULL;
  x.omesh = depv[FD_OMESH].guid; x.om = om;
  x.oguids = depv[FD_OGUIDS].guid;
  x.og = depv[FD_OGUIDS].ptr ? ((const gslice*)depv[FD_OGUIDS].ptr)->g : NULL;
  x.rank = rank;
  x.arg = paramv + FP_A0;
  x.join = node_create(c, collects, 1, n, parent, slot, rank);
  if(ocrGuidIsNull(x.join)) return NULL_GUID;

  switch(phase) {
  case PH_BUILD:   spawn_build(&x, lo, hi);   break;
  case PH_STAGE:   spawn_stage(&x, lo, hi);   break;
  default:         spawn_release(&x, lo, hi); break;
  }

  return NULL_GUID;
}

void fan_launch(u32 phase, const control* c, const ocrGuid_t* dep, ocrGuid_t parent,
                u32 slot, const u64* arg)
{
  u64 pv[FP_NPARAM];
  pv[FP_PHASE] = phase; pv[FP_SLOT] = slot;
  child_ranks(pv, 0, c->nranks);
  for(u32 i = 0; i < 3; ++i) pv[FP_A0+i] = arg ? arg[i] : 0;
  guid_store(pv+FP_PARENT, parent);

  ocrGuid_t tmp, edt;
  ocrHint_t hnt;
  ocrEdtTemplateCreate(&tmp, fan_edt, FP_NPARAM, FD_DEPC);
  ocrEdtCreate(&edt, tmp, FP_NPARAM, pv, FD_DEPC, NULL, 0, edt_at(&hnt, c, 0), NULL);
  for(u32 d = 0; d < FD_DEPC; ++d)
    ocrAddDependence(dep[d], edt, d, ocrGuidIsNull(dep[d]) ? DB_MODE_NULL : DB_MODE_CONST);
  ocrEdtTemplateDestroy(tmp);
}
