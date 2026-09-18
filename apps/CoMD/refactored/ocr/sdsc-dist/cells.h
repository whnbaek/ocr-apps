#ifndef CELLS_H
#define CELLS_H

#include <stdlib.h>
#include <math.h>
#include <ocr.h>
#include <extensions/ocr-affinity.h>

#include "ocrAppUtils.h"
#include "simulation.h"
#include "command.h"

/* Each axis is clamped to its own last cell: a coordinate that rounds to or
 * past the domain bound would otherwise carry into the next row or plane and
 * name an in-range cell that is not the one the coordinate is in. */
static inline u32 coordinates2box(const u32 grid[3], const real3 inv_box_size, const real3 r)
{
  u32 bg[3];
  for(u32 d = 0; d < 3; ++d) {
    real_t x = r[d]*inv_box_size[d];
    if(!(x > 0.0)) bg[d] = 0;
    else if(x >= (real_t)(grid[d]-1)) bg[d] = grid[d]-1;
    else bg[d] = (u32)x;
  }
  return bg[0] + bg[1]*grid[0] + bg[2]*grid[0]*grid[1];
}

static inline void box2grid(u32 box, u32 bg[3], u32 row, u32 plane)
{
  bg[0] = box%row; bg[1] = (box%plane)/row; bg[2] = box/plane;
}

static inline u32 grid2box(const u32 bg[3], u32 row, u32 plane)
{
  return bg[0]+bg[1]*row+bg[2]*plane;
}

/* Index into the 27 periodic shifts: per axis 0 means the neighbour was
 * reached by wrapping down, 2 by wrapping up, 1 not at all.  A wrapped
 * neighbour's coordinate differs by grid-1 rather than 1. */
static inline u32 faces(const u32 bg[3], const u32 bbg[3], const u32 g[3])
{
  u32 f = 0;
  for(u32 i = 0; i < 3; ++i) {
    u32 d = bg[i] > bbg[i] ? bg[i]-bbg[i] : bbg[i]-bg[i];
    u32 c = (d == g[i]-1) ? (bg[i] == 0 ? 0 : 2) : 1;
    f += c * (i == 0 ? 1 : (i == 1 ? 3 : 9));
  }
  return f;
}

/* The step of neighbour k, in the same fixed order the neighbours themselves
 * are enumerated in.  The centre of the 27 steps is skipped, so the 13 after
 * it are shifted by one. */
static inline void dir_vec(u32 k, s32 v[3])
{
  u32 t = k < 13 ? k : k+1;
  v[0] = (s32)(t%3) - 1; v[1] = (s32)((t/3)%3) - 1; v[2] = (s32)(t/9) - 1;
}

static inline u32 dir_index(const s32 v[3])
{
  u32 t = (u32)(v[0]+1) + (u32)(v[1]+1)*3 + (u32)(v[2]+1)*9;
  return t < 13 ? t : t-1;
}

/* The 26 neighbours of a cell and the periodic shift each of them needs, in
 * one fixed order.  The task that reads them and the task that wires them
 * must enumerate identically, so both call this. */
static inline void box_neighbours(u32 b, const u32 g[3], u32 nb[26], u32 sh[26])
{
  u32 row = g[0], plane = g[0]*g[1];
  u32 bg[3]; box2grid(b, bg, row, plane);
  u32 k = 0;
  for(s32 dz = -1; dz <= 1; ++dz)
    for(s32 dy = -1; dy <= 1; ++dy)
      for(s32 dx = -1; dx <= 1; ++dx) {
        if(dx == 0 && dy == 0 && dz == 0) continue;
        u32 bbg[3];
        bbg[0] = (u32)(((s32)bg[0] + dx + (s32)g[0]) % (s32)g[0]);
        bbg[1] = (u32)(((s32)bg[1] + dy + (s32)g[1]) % (s32)g[1]);
        bbg[2] = (u32)(((s32)bg[2] + dz + (s32)g[2]) % (s32)g[2]);
        nb[k] = grid2box(bbg, row, plane);
        sh[k] = faces(bg, bbg, g);
        ++k;
      }
}

/* True when d is b itself or one of its 26 neighbours: an atom that leaves a
 * cell in one step can only reach those, so anything else means the step
 * moved an atom further than a cell width. */
static inline u8 is_neighbour(const u32 g[3], u32 b, u32 d)
{
  if(d >= g[0]*g[1]*g[2]) return 0;
  u32 row = g[0], plane = g[0]*g[1];
  u32 bg[3], dg[3];
  box2grid(b, bg, row, plane);
  box2grid(d, dg, row, plane);
  for(u32 i = 0; i < 3; ++i) {
    u32 diff = bg[i] > dg[i] ? bg[i]-dg[i] : dg[i]-bg[i];
    if(diff > 1 && diff != g[i]-1) return 0;
  }
  return 1;
}

/* Cells are blocked over a three-dimensional rank grid: a rank owns a compact
 * brick rather than a slab, so the share of a cell's 26 neighbours that is
 * remote falls with the brick's surface-to-volume ratio instead of with the
 * slab's thickness in planes. */
static inline u32 cell_owner(const config* c, u32 b)
{
  if(c->nranks <= 1) return 0;
  u32 bg[3]; box2grid(b, bg, c->grid[0], c->grid[0]*c->grid[1]);
  u32 rx = (bg[0]*c->ranks[0])/c->grid[0];
  u32 ry = (bg[1]*c->ranks[1])/c->grid[1];
  u32 rz = (bg[2]*c->ranks[2])/c->grid[2];
  return (rz*c->ranks[1] + ry)*c->ranks[0] + rx;
}

static inline void rank_coords(const config* c, u32 r, u32 rg[3])
{
  rg[0] = r % c->ranks[0];
  rg[1] = (r / c->ranks[0]) % c->ranks[1];
  rg[2] = r / (c->ranks[0]*c->ranks[1]);
}

/* The rank one step along d from r, the rank grid wrapping like the cell grid
 * it blocks.  It is r itself exactly when every axis d moves on holds a
 * single rank, which is also when a neighbour reached through d never leaves
 * r's brick. */
static inline u32 rank_shift(const config* c, u32 r, const s32 d[3])
{
  u32 rg[3]; rank_coords(c, r, rg);
  for(u32 i = 0; i < 3; ++i)
    rg[i] = (u32)((((s32)rg[i] + d[i]) + (s32)c->ranks[i]) % (s32)c->ranks[i]);
  return (rg[2]*c->ranks[1] + rg[1])*c->ranks[0] + rg[0];
}

/* The brick rank r owns: on each axis the cells [lo, lo+n) are exactly those
 * cell_owner() assigns to r's coordinate there, so the bricks of all ranks
 * tile the grid.  A single rank's brick is the whole grid. */
static inline void rank_brick(const config* c, u32 r, u32 lo[3], u32 n[3])
{
  u32 rg[3]; rank_coords(c, r, rg);
  for(u32 d = 0; d < 3; ++d) {
    u32 a = (rg[d]*c->grid[d] + c->ranks[d] - 1)/c->ranks[d];
    u32 b = ((rg[d]+1)*c->grid[d] + c->ranks[d] - 1)/c->ranks[d];
    lo[d] = a; n[d] = b - a;
  }
}

static inline u32 brick_cells(const config* c, u32 r)
{
  u32 lo[3], n[3];
  rank_brick(c, r, lo, n);
  return n[0]*n[1]*n[2];
}

/* The cell at position i of a brick.  A brick is enumerated in the grid's
 * own axis order, so brick-local order is ascending cell index. */
static inline u32 brick_cell(const config* c, const u32 lo[3], const u32 n[3], u32 i)
{
  u32 bg[3];
  bg[0] = lo[0] + i % n[0];
  bg[1] = lo[1] + (i / n[0]) % n[1];
  bg[2] = lo[2] + i / (n[0]*n[1]);
  return grid2box(bg, c->grid[0], c->grid[0]*c->grid[1]);
}

/* Boundary i of the k-way split of the rank interval [rlo, rhi).  The tree
 * that fans out and the merge that folds back must cut identically. */
static inline u32 rank_split(u32 rlo, u32 rhi, u32 k, u32 i)
{
  return rlo + (u32)(((u64)(rhi - rlo) * i) / k);
}

static inline u32 spawn_cell(const spawn_ctx* x, u32 i)
{
  return brick_cell(x->c, x->blo, x->bdim, i);
}

/* The layer of a brick facing direction d is the cells the brick one step
 * along d reads: the boundary plane, row or corner d names, free on the axes
 * d does not move on.  It is enumerated in the grid's own axis order over the
 * free axes, x fastest, and cut into runs of HALO_CHUNK.  A layer is packed
 * by its owner and read by one other brick, and the two must index it
 * identically, so both derive their positions from these. */
static inline void layer_extent(const u32 n[3], const s32 d[3], u32 e[3])
{
  for(u32 i = 0; i < 3; ++i) e[i] = d[i] ? 1 : n[i];
}

static inline u32 layer_cells(const u32 n[3], const s32 d[3])
{
  u32 e[3]; layer_extent(n, d, e);
  return e[0]*e[1]*e[2];
}

/* The cell at position j of the layer of brick [lo, lo+n) facing d. */
static inline u32 layer_cell(const config* c, const u32 lo[3], const u32 n[3],
                             const s32 d[3], u32 j)
{
  u32 e[3]; layer_extent(n, d, e);
  u32 bg[3];
  bg[0] = lo[0] + (d[0] ? (d[0] < 0 ? 0 : n[0]-1) : j % e[0]);
  bg[1] = lo[1] + (d[1] ? (d[1] < 0 ? 0 : n[1]-1) : (j / e[0]) % e[1]);
  bg[2] = lo[2] + (d[2] ? (d[2] < 0 ? 0 : n[2]-1) : j / (e[0]*e[1]));
  return grid2box(bg, c->grid[0], c->grid[0]*c->grid[1]);
}

/* Where the cell at grid position mg sits in that same layer. */
static inline u32 layer_pos(const u32 lo[3], const u32 n[3], const s32 d[3],
                            const u32 mg[3])
{
  u32 e[3]; layer_extent(n, d, e);
  u32 p = 0, m = 1;
  for(u32 i = 0; i < 3; ++i) {
    if(!d[i]) p += (mg[i]-lo[i])*m;
    m *= e[i];
  }
  return p;
}

/* A layer exists only where every axis its direction moves on is split across
 * ranks: along an axis one rank owns whole, a neighbour never leaves the
 * brick, so no cell ever reaches for that layer and none is kept. */
static inline u32 layer_runs(const config* c, const u32 n[3], const s32 d[3])
{
  for(u32 i = 0; i < 3; ++i)
    if(d[i] && c->ranks[i] == 1) return 0;
  return (layer_cells(n, d) + HALO_CHUNK - 1)/HALO_CHUNK;
}

/* A brick's halo blocks are indexed by direction and then by run, which is
 * the order the generation fills them in. */
static inline u32 brick_halo(const config* c, u32 r)
{
  u32 lo[3], n[3], t = 0;
  rank_brick(c, r, lo, n);
  for(u32 k = 0; k < 26; ++k) {
    s32 d[3]; dir_vec(k, d);
    t += layer_runs(c, n, d);
  }
  return t;
}

static inline void brick_halo_at(const config* c, u32 r, u32 i, u32* dir, u32* run)
{
  u32 lo[3], n[3];
  rank_brick(c, r, lo, n);
  for(u32 k = 0; k < 26; ++k) {
    s32 d[3]; dir_vec(k, d);
    u32 runs = layer_runs(c, n, d);
    if(i < runs) { *dir = k; *run = i; return; }
    i -= runs;
  }
  *dir = 0; *run = 0;
}

/* The directory's halo blocks follow its cells, and the layer table follows
 * them: `layer_table(l)[r*26 + k]` is the first block of brick r's layer k
 * and the entry after it the one past that layer's last. */
static inline const ocrGuid_t* halo_table(const cell_list* l)
{
  return (const ocrGuid_t*)(const void*)(l->cell + l->count);
}

static inline const u32* layer_table(const cell_list* l)
{
  return (const u32*)(const void*)(halo_table(l) + l->halo);
}

static inline u64 list_bytes(u32 count, u32 halo, u32 dirs)
{
  return sizeof(cell_list) + (u64)count*sizeof(cell_ref)
       + (u64)halo*sizeof(ocrGuid_t) + (u64)dirs*sizeof(u32);
}

/* How an atom of a block read during a step relates to the cell being
 * stepped: a periodic shift index when it is in an adjacent cell, ATOM_MINE
 * when its position is in the cell itself, ATOM_FAR when the cell it is in
 * is not adjacent and it cannot be within reach. */
enum { ATOM_MINE = 27, ATOM_FAR = 28 };

void cell_classify(const config* c, u32 b, const u32 nb[26], const u32 sh[26],
                   const box* x, u32 self, u8 rel[MAXATOMS]);
u8 cell_take(box* out, const box* x, u32 j);

/// One block a step takes for reading: a cell of its own brick, or a run of
/// the layer another brick packed, which `cell` set to all ones marks.
typedef struct
{
  u32 cell;
  u32 rank;
  u32 dir;
  u32 run;
} read_blk;

/// The distinct blocks a step of one cell reads and where each of its 26
/// neighbours lies in them.  A neighbour inside the brick has a block to
/// itself; the ones outside share their owner's layer runs, so a step takes
/// far fewer blocks than it has neighbours.
typedef struct
{
  u32 count;
  read_blk blk[26];
  u8 slot[26];  //!< block neighbour k lies in, counted from the first read block
  u8 at[26];    //!< its position in that block
} read_set;

void cell_reads(const config* c, u32 b, const u32 nb[26], read_set* s);

static inline ocrHint_t* edt_at(ocrHint_t* h, const config* c, u32 rank)
{
  if(c->nranks <= 1) return NULL_HINT;
  ocrGuid_t aff;
  ocrAffinityGetAt(AFFINITY_PD, rank, &aff);
  ocrHintInit(h, OCR_HINT_EDT_T);
  ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
  return h;
}

static inline ocrHint_t* db_at(ocrHint_t* h, const config* c, u32 rank)
{
  if(c->nranks <= 1) return NULL_HINT;
  ocrGuid_t aff;
  ocrAffinityGetAt(AFFINITY_PD, rank, &aff);
  ocrHintInit(h, OCR_HINT_DB_T);
  ocrSetHintValue(h, OCR_HINT_DB_AFFINITY, ocrAffinityToHintValue(aff));
  return h;
}

#endif
