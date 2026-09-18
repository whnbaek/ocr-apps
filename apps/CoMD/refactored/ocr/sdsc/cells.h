#ifndef CELLS_H
#define CELLS_H

#include <stdlib.h>
#include <math.h>
#include <ocr.h>

#include "simulation.h"
#include "command.h"

void init_lattice(simulation* s, command* cmd, real_t lattice_constant, ocrGuid_t** list);

void init_boxes(ocrGuid_t* list, const u32 grid[3]);

void fork_generate_atoms(ocrGuid_t sim, ocrGuid_t cont, u32 depc, ocrGuid_t* list, const u32 grid[3]);

void fork_set_temperature(ocrGuid_t cont, u32 depc, ocrGuid_t* list,
                          const u32 grid[3], const real3 vcm, real_t scale);

void fork_redistribute(ocrGuid_t sim, ocrGuid_t cont, u32 depc, ocrGuid_t* list, u32 boxes_num);

static inline u32 coordinates2box(const u32 grid[3], const real3 inv_box_size, const real3 r)
{
  return ((u32)(r[0]*inv_box_size[0])) +
         ((u32)(r[1]*inv_box_size[1])*grid[0]) +
         ((u32)(r[2]*inv_box_size[2])*grid[0]*grid[1]);
};

static inline void box2grid(u32 box, u32 bg[3], u32 row, u32 plane)
{
  bg[0] = box%row; bg[1] = (box%plane)/row; bg[2] = box/plane;
};

static inline u32 grid2box(u32 bg[3], u32 row, u32 plane)
{
  return bg[0]+bg[1]*row+bg[2]*plane;
};

static inline u32 faces(u32 bg[3], u32 bbg[3], u32 g[3])
{
  return (abs(bg[0]-bbg[0])<g[0]-1 ? 1 : (bg[0]==0 ? 0 : 2)) +
         (abs(bg[1]-bbg[1])<g[1]-1 ? 3 : (bg[1]==0 ? 0 : 6)) +
         (abs(bg[2]-bbg[2])<g[2]-1 ? 9 : (bg[2]==0 ? 0 : 18));
};

/* Every completion event in this port has exactly one consumer, registered
 * before the event can fire.  Declaring that count (a COUNTED event) lets
 * the runtime reclaim the event as soon as the consumer is in; a runtime
 * that cannot prove no registration will follow has to keep an undeclared
 * single-fire event for the rest of the run -- at one event per box per
 * phase that grows without bound in the step count.  A registration after
 * the fire is not legal on any runtime, whichever kind the event is. */
#include "ocrAppUtils.h"
#ifdef OCR_APP_COUNTED_OEVT
#define comdJoinEvt(e) createEventHelper(&(e), 1)
#else
#define comdJoinEvt(e) ocrEventCreate(&(e), OCR_EVENT_ONCE_T, false)
#endif

/* Placement-optimization layer: a recursive bisection of the box grid gives
 * every rank one compact 3-D block of boxes.  At each level the rank set is
 * halved and the sub-grid is cut on whichever axis leaves the two box counts
 * closest to proportional, ties to the longest axis; a block's box count then
 * tracks the mean and its surface grows only as the 2/3 power of its volume.
 * Pinning every per-box task (force, kinetic energy, advance) and every box
 * datablock of box b to that block's rank keeps the box's own read-write data
 * and the bulk of its 26 neighbour reads on one rank, step after step -- with
 * no hint each of those tasks lands on a fresh rank every step and the box
 * travels with it.  A linear index band would balance exactly but is a slab:
 * once a band is thinner than one plane of the grid every out-of-plane
 * neighbour is remote, which is the regime a wide machine runs in. */
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
#include <extensions/ocr-affinity.h>
static inline u32 comdBlockRank(u64 b, const u32 grid[3], u64 nranks)
{
  u32 lo[3] = {0, 0, 0};
  u32 hi[3] = {grid[0], grid[1], grid[2]};
  u32 c[3];
  box2grid((u32)b, c, grid[0], grid[0]*grid[1]);

  u64 base = 0;
  u64 p = nranks;
  while(p > 1) {
    u64 pl = p/2, pr = p-pl;
    u32 axis = 3, cut = 0;
    u64 skew = 0;
    for(u32 a = 0; a < 3; ++a) {
      u32 len = hi[a]-lo[a];
      if(len < 2) continue;
      u64 cross = 1;
      for(u32 t = 0; t < 3; ++t)
        if(t != a) cross *= hi[t]-lo[t];
      u64 k = (len*pl + p/2)/p;
      if(k < 1) k = 1;
      if(k > len-1) k = len-1;
      u64 nl = k*cross*pr, nr = (len-k)*cross*pl;
      u64 s = nl > nr ? nl : nr;
      if(axis == 3 || s < skew || (s == skew && len > hi[axis]-lo[axis])) {
        axis = a; cut = lo[a]+(u32)k; skew = s;
      }
    }
    if(axis == 3) break;
    if(c[axis] < cut) { hi[axis] = cut; p = pl; }
    else { lo[axis] = cut; base += pl; p = pr; }
  }
  return (u32)base;
}
static inline ocrHint_t * comdBlockEdtHint(ocrHint_t *h, u64 b, const u32 grid[3]) {
  u64 nranks;
  ocrAffinityCount(AFFINITY_PD, &nranks);
  if(nranks <= 1) return NULL_HINT;
  ocrGuid_t aff;
  ocrAffinityGetAt(AFFINITY_PD, comdBlockRank(b, grid, nranks), &aff);
  ocrHintInit(h, OCR_HINT_EDT_T);
  ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
  return h;
}
static inline ocrHint_t * comdBlockDbHint(ocrHint_t *h, u64 b, const u32 grid[3]) {
  u64 nranks;
  ocrAffinityCount(AFFINITY_PD, &nranks);
  if(nranks <= 1) return NULL_HINT;
  ocrGuid_t aff;
  ocrAffinityGetAt(AFFINITY_PD, comdBlockRank(b, grid, nranks), &aff);
  ocrHintInit(h, OCR_HINT_DB_T);
  ocrSetHintValue(h, OCR_HINT_DB_AFFINITY, ocrAffinityToHintValue(aff));
  return h;
}
/* The control spine (per-phase continuations, the join tasks, the serial
 * redistribute) is pinned to one rank: the simulation singleton and the
 * scalars those tasks mutate then live and stay where every one of their
 * writers runs, instead of the spine round-robining to a fresh rank each
 * phase and dragging the singleton's write ownership along with it.  The
 * per-box reduction results are homed on that rank too: their consumer is the
 * join, so the home is the reader's rank and the lookup is local.  What that
 * buys depends on the write policy -- a policy that publishes at release makes
 * the payload local as well, one that keeps the payload with the last writer
 * still fetches it -- and it is paid for by concentrating one datablock create
 * and destroy per box per reduction on the single home rank. */
static inline ocrHint_t * comdHomeEdtHint(ocrHint_t *h) {
  u64 nranks;
  ocrAffinityCount(AFFINITY_PD, &nranks);
  if(nranks <= 1) return NULL_HINT;
  ocrGuid_t aff;
  ocrAffinityGetAt(AFFINITY_PD, 0, &aff);
  ocrHintInit(h, OCR_HINT_EDT_T);
  ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
  return h;
}
static inline ocrHint_t * comdHomeDbHint(ocrHint_t *h) {
  u64 nranks;
  ocrAffinityCount(AFFINITY_PD, &nranks);
  if(nranks <= 1) return NULL_HINT;
  ocrGuid_t aff;
  ocrAffinityGetAt(AFFINITY_PD, 0, &aff);
  ocrHintInit(h, OCR_HINT_DB_T);
  ocrSetHintValue(h, OCR_HINT_DB_AFFINITY, ocrAffinityToHintValue(aff));
  return h;
}
#else
#define comdBlockEdtHint(h, b, grid) NULL_HINT
#define comdBlockDbHint(h, b, grid) NULL_HINT
#define comdHomeEdtHint(h) NULL_HINT
#define comdHomeDbHint(h) NULL_HINT
#endif

#endif
