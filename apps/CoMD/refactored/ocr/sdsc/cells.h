#ifndef CELLS_H
#define CELLS_H

#include <stdlib.h>
#include <math.h>
#include <ocr.h>

#include "simulation.h"
#include "command.h"

box** init_lattice(simulation* s, command* cmd, real_t lattice_constant, ocrGuid_t** list, ocrGuid_t* ptrs);

void set_temperature(simulation* sim, box** bxs_ptr, real_t temperature);

void random_displacement(box** bxs_ptr, u32 boxes_num, real_t delta);

void create_fcc_lattice(int nx, int ny, int nz, real_t lat, boxes* bxs, box** bxs_ptr);

void init_atoms(ocrGuid_t* list, u32 boxes_num, box** bxs_ptr);

void fork_redistribute(ocrGuid_t sim, ocrGuid_t cont, u32 depc, ocrGuid_t* list, u32 boxes_num);

static inline u32 coordinates2box(u32 grid[3], real3 inv_box_size, real3 r)
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
 * at creation.  Declaring that count (a COUNTED event) lets the runtime
 * reclaim the event once the consumer is in; an undeclared single-fire
 * event must linger for the rest of the run, because a later registration
 * on it is always legal -- at one event per box per phase that lingering
 * grows without bound in the step count. */
#include "ocrAppUtils.h"
#ifdef OCR_APP_COUNTED_OEVT
#define comdJoinEvt(e) createEventHelper(&(e), 1)
#else
#define comdJoinEvt(e) ocrEventCreate(&(e), OCR_EVENT_ONCE_T, false)
#endif

/* Placement-optimization layer: z-slab placement for per-box tasks.  Boxes
 * are linearized x-fastest, so a contiguous index band is a slab of whole
 * x-y planes; a box's 26 neighbours are in its own or the adjacent plane,
 * which is the same or the neighbouring band.  Pinning every per-box task
 * (force, kinetic-energy, advance) of box b to its band rank keeps each
 * box's RW data and most of its neighbour reads on one rank, step after
 * step -- as-born every one of those tasks lands on a fresh rank each step
 * and every box travels every timestep. */
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
#include <extensions/ocr-affinity.h>
static inline ocrHint_t * comdSlabEdtHint(ocrHint_t *h, u64 b, u64 boxes_num) {
  u64 nranks;
  ocrAffinityCount(AFFINITY_PD, &nranks);
  if (nranks <= 1 || boxes_num == 0) return NULL_HINT;
  u64 band = (b * nranks) / boxes_num;
  if (band >= nranks) band = nranks - 1;
  ocrGuid_t aff;
  ocrAffinityGetAt(AFFINITY_PD, band, &aff);
  ocrHintInit(h, OCR_HINT_EDT_T);
  ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
  return h;
}
/* The box datablock is homed on its band rank too, so a box's directory
 * lives where the tasks that touch it every step run, instead of every
 * box being homed on the one rank that ran the init task. */
static inline ocrHint_t * comdSlabDbHint(ocrHint_t *h, u64 b, u64 boxes_num) {
  u64 nranks;
  ocrAffinityCount(AFFINITY_PD, &nranks);
  if (nranks <= 1 || boxes_num == 0) return NULL_HINT;
  u64 band = (b * nranks) / boxes_num;
  if (band >= nranks) band = nranks - 1;
  ocrGuid_t aff;
  ocrAffinityGetAt(AFFINITY_PD, band, &aff);
  ocrHintInit(h, OCR_HINT_DB_T);
  ocrSetHintValue(h, OCR_HINT_DB_AFFINITY, ocrAffinityToHintValue(aff));
  return h;
}
/* The control spine (per-phase continuations, the join tasks, the serial
 * redistribute) is pinned to one rank: the simulation singleton and the
 * scalars those tasks mutate then live and stay where every one of their
 * writers runs, instead of the spine round-robining to a fresh rank each
 * phase and dragging the singleton's write ownership along with it. */
static inline ocrHint_t * comdHomeEdtHint(ocrHint_t *h) {
  u64 nranks;
  ocrAffinityCount(AFFINITY_PD, &nranks);
  if (nranks <= 1) return NULL_HINT;
  ocrGuid_t aff;
  ocrAffinityGetAt(AFFINITY_PD, 0, &aff);
  ocrHintInit(h, OCR_HINT_EDT_T);
  ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
  return h;
}
#else
#define comdSlabEdtHint(h, b, boxes_num) NULL_HINT
#define comdSlabDbHint(h, b, boxes_num) NULL_HINT
#define comdHomeEdtHint(h) NULL_HINT
#endif

#endif
