#include <math.h>
#include <ocr.h>

#include "simulation.h"
#include "cells.h"

#define DOUBLE_VAL (1.0/UINT64_C(2305843009213693951))

static inline double lcg61(u64* seed)
{
  *seed *= UINT64_C(437799614237992725);
  *seed %= UINT64_C(2305843009213693951);
  return *seed*DOUBLE_VAL;
}

static inline real_t gasdev(u64* seed)
{
   real_t rsq,v2;
   do {
      rsq = 2.0*lcg61(seed)-1.0;
      v2 = 2.0*lcg61(seed)-1.0;
      rsq = rsq*rsq+v2*v2;
   } while(rsq >= 1.0 || rsq == 0.0);

   return v2 * sqrt(-2.0*log(rsq)/rsq);
}

/* The state an atom is born with is a pure function of its global index, so
 * any task can generate the atoms of the cell it owns without consulting a
 * shared generator or any other cell. */
static inline u64 mk_seed(u32 id, u32 call_site)
{
   u32 s1 = id * UINT32_C(2654435761);
   u32 s2 = (id+call_site) * UINT32_C(2654435761);

   u64 seed = (UINT64_C(0x100000000) * s1) + s2;
   lcg61(&seed); lcg61(&seed);
   lcg61(&seed); lcg61(&seed);
   lcg61(&seed); lcg61(&seed);
   lcg61(&seed); lcg61(&seed);
   lcg61(&seed); lcg61(&seed);

   return seed;
}

static const real3 fcc_basis[4] = { {0.25, 0.25, 0.25},
                                    {0.25, 0.75, 0.75},
                                    {0.75, 0.25, 0.75},
                                    {0.75, 0.75, 0.25} };

static void unit_range(const config* c, u32 lo, u32 hi, u32 d, s64* t0, s64* t1)
{
  real_t low  = (real_t)lo * c->box_size[d];
  real_t high = (real_t)hi * c->box_size[d];
  *t0 = (s64)floor(low / c->lat) - 2;
  *t1 = (s64)floor(high / c->lat) + 2;
  if(*t1 - *t0 + 1 >= (s64)c->unit[d]) { *t0 = 0; *t1 = (s64)c->unit[d] - 1; }
}

/* Generate every atom whose lattice site lands in cell b.  Positions carry
 * their displacement before the cell is chosen, so an atom is born in the
 * cell that owns it and no migration pass is needed to settle generation. */
static u8 fill_box(const config* c, u32 b, box* bx)
{
  bx->atoms = 0;
  u32 row = c->grid[0], plane = c->grid[0]*c->grid[1];
  u32 bg[3]; box2grid(b, bg, row, plane);

  s64 t0[3], t1[3];
  for(u32 d = 0; d < 3; ++d)
    unit_range(c, bg[d], bg[d]+1, d, &t0[d], &t1[d]);

  real_t sigma = sqrt(kB_eV * c->temperature / c->pot.mass);
  for(s64 tx = t0[0]; tx <= t1[0]; ++tx) {
    u32 ix = (u32)(((tx % (s64)c->unit[0]) + (s64)c->unit[0]) % (s64)c->unit[0]);
    for(s64 ty = t0[1]; ty <= t1[1]; ++ty) {
      u32 iy = (u32)(((ty % (s64)c->unit[1]) + (s64)c->unit[1]) % (s64)c->unit[1]);
      for(s64 tz = t0[2]; tz <= t1[2]; ++tz) {
        u32 iz = (u32)(((tz % (s64)c->unit[2]) + (s64)c->unit[2]) % (s64)c->unit[2]);
        for(u32 ib = 0; ib < 4; ++ib) {
          u32 gid = ((ix*c->unit[1] + iy)*c->unit[2] + iz)*4 + ib;
          real3 r;
          r[0] = (ix+fcc_basis[ib][0]) * c->lat;
          r[1] = (iy+fcc_basis[ib][1]) * c->lat;
          r[2] = (iz+fcc_basis[ib][2]) * c->lat;
          if(c->delta > 0.0) {
            u64 seed = mk_seed(gid, 457);
            for(u32 d = 0; d < 3; ++d) {
              r[d] += (2.0*lcg61(&seed)-1.0) * c->delta;
              if(r[d] < 0.0) r[d] += c->local_max[d];
              else if(r[d] >= c->local_max[d]) r[d] -= c->local_max[d];
            }
          }
          if(coordinates2box(c->grid, c->inv_box_size, r) != b) continue;
          if(bx->atoms >= MAXATOMS) return 1;
          u32 a = bx->atoms++;
          bx->gid[a] = gid;
          bx->r[a][0] = r[0]; bx->r[a][1] = r[1]; bx->r[a][2] = r[2];
          u64 seed = mk_seed(gid, 123);
          bx->p[a][0] = c->pot.mass*sigma*gasdev(&seed);
          bx->p[a][1] = c->pot.mass*sigma*gasdev(&seed);
          bx->p[a][2] = c->pot.mass*sigma*gasdev(&seed);
        }
      }
    }
  }
  return 0;
}

static ocrGuid_t init_fan_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

/* The generation tree is shaped like the phase trees: a node covers the rank
 * interval [RLO, RHI) while that spans more than one rank, and the
 * brick-local cell interval [LO, HI) of rank RLO once it spans exactly one. */
enum { IP_RLO, IP_RHI, IP_LO, IP_HI, IP_SLOT, IP_PARENT, IP_NPARAM = IP_PARENT + GUID_WORDS };
enum { MP_RLO, MP_RHI, MP_SLOT, MP_PARENT, MP_NPARAM = MP_PARENT + GUID_WORDS };

/* The directory's arrays while it is being filled.  Only generation writes
 * them; every later task reads them through the accessors in cells.h. */
static ocrGuid_t* halo_slots(cell_list* l)
{
  return (ocrGuid_t*)(void*)(l->cell + l->count);
}

static u32* layer_slots(cell_list* l)
{
  return (u32*)(void*)(halo_slots(l) + l->halo);
}

/* An interior node concatenates its children's directories and adds up their
 * sums, so the directory is assembled by the tree that produced it rather
 * than by one task gathering every cell.  A slice below a brick is a
 * contiguous brick-local interval and a slice over ranks lists whole bricks
 * in rank order, so concatenation in slot order preserves both; only the
 * root's slice is read by cell index, so only the root places each entry at
 * its cell.  Halo blocks are concatenated whatever the level, because a brick
 * contributes them all from one leaf and the bricks arrive in rank order.
 * Slot 0 is the configuration. */
static ocrGuid_t init_merge_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  const config* c = (const config*)depv[0].ptr;
  u32 rlo = (u32)paramv[MP_RLO], rhi = (u32)paramv[MP_RHI];
  u32 k = depc - 1;
  u8 scatter = rhi - rlo > 1 && rlo == 0 && rhi == c->nranks;

  u32 total = 0, halo = 0;
  for(u32 i = 1; i < depc; ++i) {
    total += ((const cell_list*)depv[i].ptr)->count;
    halo += ((const cell_list*)depv[i].ptr)->halo;
  }
  /* Where a brick's layers begin can only be said once every brick is in, so
   * the table is laid down by the node that covers them all. */
  u32 dirs = scatter ? c->nranks*26 + 1 : 0;

  ocrGuid_t out; cell_list* cl;
  ocrDbCreate(&out, (void**)&cl, list_bytes(total, halo, dirs),
              0, NULL_HINT, NO_ALLOC);
  cl->count = total; cl->halo = halo; cl->dirs = dirs;
  cl->st.atoms = 0; cl->st.max_occupancy = 0;
  cl->st.p[0] = cl->st.p[1] = cl->st.p[2] = 0.0; cl->st.p2 = 0.0;

  u32 at = 0, hat = 0;
  for(u32 i = 1; i < depc; ++i) {
    const cell_list* s = (const cell_list*)depv[i].ptr;
    if(scatter) {
      u32 from = 0;
      for(u32 r = rank_split(rlo, rhi, k, i-1); r < rank_split(rlo, rhi, k, i); ++r) {
        u32 blo[3], bdim[3];
        rank_brick(c, r, blo, bdim);
        u32 nb = bdim[0]*bdim[1]*bdim[2];
        for(u32 j = 0; j < nb; ++j)
          cl->cell[brick_cell(c, blo, bdim, j)] = s->cell[from++];
      }
    } else {
      memcpy(cl->cell+at, s->cell, (u64)s->count*sizeof(cell_ref));
    }
    at += s->count;
    memcpy(halo_slots(cl)+hat, halo_table(s), (u64)s->halo*sizeof(ocrGuid_t));
    hat += s->halo;
    cl->st.atoms += s->st.atoms;
    if(s->st.max_occupancy > cl->st.max_occupancy)
      cl->st.max_occupancy = s->st.max_occupancy;
    cl->st.p[0] += s->st.p[0];
    cl->st.p[1] += s->st.p[1];
    cl->st.p[2] += s->st.p[2];
    cl->st.p2 += s->st.p2;
    ocrDbDestroy(depv[i].guid);
  }
  if(dirs) {
    u32* tab = layer_slots(cl);
    u32 acc = 0;
    for(u32 r = 0; r < c->nranks; ++r) {
      u32 blo[3], bdim[3];
      rank_brick(c, r, blo, bdim);
      for(u32 j = 0; j < 26; ++j) {
        s32 d[3]; dir_vec(j, d);
        tab[r*26+j] = acc;
        acc += layer_runs(c, bdim, d);
      }
    }
    tab[c->nranks*26] = acc;
  }
  ocrDbRelease(out);
  ocrDbRelease(depv[0].guid);
  ocrAddDependence(out, guid_load(paramv+MP_PARENT), (u32)paramv[MP_SLOT], DB_MODE_CONST);

  return NULL_GUID;
}

/* A brick's halo blocks are born beside the cells they will carry, one per
 * layer run, sized to that run.  Their contents are the step's, so generation
 * only brings them into being. */
static void init_halo(const config* c, u32 rank, const u32 bdim[3], ocrGuid_t* hb)
{
  ocrHint_t dbHNT;
  u32 at = 0;
  for(u32 k = 0; k < 26; ++k) {
    s32 d[3]; dir_vec(k, d);
    u32 runs = layer_runs(c, bdim, d);
    u32 cells = layer_cells(bdim, d);
    for(u32 j = 0; j < runs; ++j) {
      u32 len = cells - j*HALO_CHUNK;
      if(len > HALO_CHUNK) len = HALO_CHUNK;
      box* run;
      ocrDbCreate(&hb[at++], (void**)&run, (u64)len*sizeof(box), 0,
                  db_at(&dbHNT, c, rank), NO_ALLOC);
    }
  }
}

/* A leaf runs on the rank that owns its brick and creates its cells there,
 * so a generated cell is born at its home.  The first generation of a cell's
 * block is filled; the second is what the first step writes.  The leaf that
 * opens a brick's range also creates that brick's halo blocks, so each of
 * them appears exactly once and on its owner. */
static void init_leaf(ocrGuid_t cfg, const config* c, u32 rank, u32 lo, u32 hi,
                      ocrGuid_t parent, u32 slot)
{
  u32 blo[3], bdim[3];
  rank_brick(c, rank, blo, bdim);
  u32 n = hi - lo;
  u32 nh = lo == 0 ? brick_halo(c, rank) : 0;
  ocrGuid_t out; cell_list* cl;
  ocrHint_t dbHNT;
  ocrDbCreate(&out, (void**)&cl, list_bytes(n, nh, 0),
              0, db_at(&dbHNT, c, rank), NO_ALLOC);
  cl->count = n; cl->halo = nh; cl->dirs = 0;
  cl->st.atoms = 0; cl->st.max_occupancy = 0;
  cl->st.p[0] = cl->st.p[1] = cl->st.p[2] = 0.0; cl->st.p2 = 0.0;
  if(nh) init_halo(c, rank, bdim, halo_slots(cl));

  for(u32 i = lo; i < hi; ++i) {
    u32 b = brick_cell(c, blo, bdim, i);
    box* bx; box* next;
    ocrDbCreate(&cl->cell[i-lo].bx[0], (void**)&bx, sizeof(box), 0,
                db_at(&dbHNT, c, rank), NO_ALLOC);
    ocrDbCreate(&cl->cell[i-lo].bx[1], (void**)&next, sizeof(box), 0,
                db_at(&dbHNT, c, rank), NO_ALLOC);
    next->atoms = 0;
    if(fill_box(c, b, bx)) {
      ocrPrintf("ERROR cell %u holds more than %d atoms TERMINATING\n", b, MAXATOMS);
      ocrShutdown();
      return;
    }
    cl->st.atoms += bx->atoms;
    if(bx->atoms > cl->st.max_occupancy) cl->st.max_occupancy = bx->atoms;
    for(u32 a = 0; a < bx->atoms; ++a) {
      cl->st.p[0] += bx->p[a][0];
      cl->st.p[1] += bx->p[a][1];
      cl->st.p[2] += bx->p[a][2];
      cl->st.p2 += bx->p[a][0]*bx->p[a][0] + bx->p[a][1]*bx->p[a][1]
                 + bx->p[a][2]*bx->p[a][2];
    }
  }
  for(u32 i = 0; i < n; ++i) {
    ocrDbRelease(cl->cell[i].bx[0]);
    ocrDbRelease(cl->cell[i].bx[1]);
  }
  for(u32 h = 0; h < nh; ++h)
    ocrDbRelease(halo_slots(cl)[h]);
  ocrDbRelease(out);
  ocrDbRelease(cfg);
  ocrAddDependence(out, parent, slot, DB_MODE_CONST);
}

static void child_ranks(const config* c, u64* pv, u32 rlo, u32 rhi)
{
  pv[IP_RLO] = rlo; pv[IP_RHI] = rhi;
  pv[IP_LO] = 0; pv[IP_HI] = rhi - rlo == 1 ? brick_cells(c, rlo) : 0;
}

/* The merge node of an interior node: k children report to slots 1..k. */
static ocrGuid_t merge_create(const config* c, ocrGuid_t cfg, u32 k, u32 rlo, u32 rhi,
                              ocrGuid_t parent, u32 slot, u32 rank)
{
  ocrGuid_t tmp, node;
  ocrHint_t edtHNT;
  u64 mpv[MP_NPARAM];
  mpv[MP_RLO] = rlo; mpv[MP_RHI] = rhi; mpv[MP_SLOT] = slot;
  guid_store(mpv+MP_PARENT, parent);
  ocrEdtTemplateCreate(&tmp, init_merge_edt, MP_NPARAM, k+1);
  ocrEdtCreate(&node, tmp, MP_NPARAM, mpv, k+1, NULL, 0,
               edt_at(&edtHNT, c, rank), NULL);
  ocrEdtTemplateDestroy(tmp);
  ocrAddDependence(cfg, node, 0, DB_MODE_CONST);
  return node;
}

static ocrGuid_t init_fan_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  const config* c = (const config*)depv[0].ptr;
  u32 rlo = (u32)paramv[IP_RLO], rhi = (u32)paramv[IP_RHI];
  u32 lo = (u32)paramv[IP_LO], hi = (u32)paramv[IP_HI];
  ocrGuid_t parent = guid_load(paramv+IP_PARENT);
  u32 slot = (u32)paramv[IP_SLOT];
  u32 rank = rlo;

  if(rhi - rlo > 1) {
    u32 k = rhi - rlo < FAN_BRANCH ? rhi - rlo : FAN_BRANCH;
    ocrGuid_t node = merge_create(c, depv[0].guid, k, rlo, rhi, parent, slot, rank);
    ocrGuid_t tmp;
    ocrHint_t edtHNT;
    ocrEdtTemplateCreate(&tmp, init_fan_edt, IP_NPARAM, 1);
    for(u32 i = 0; i < k; ++i) {
      u64 pv[IP_NPARAM];
      child_ranks(c, pv, rank_split(rlo, rhi, k, i), rank_split(rlo, rhi, k, i+1));
      pv[IP_SLOT] = i+1;
      guid_store(pv+IP_PARENT, node);
      ocrGuid_t child;
      ocrEdtCreate(&child, tmp, IP_NPARAM, pv, 1, NULL, 0,
                   edt_at(&edtHNT, c, (u32)pv[IP_RLO]), NULL);
      if(i+1 == k) ocrDbRelease(depv[0].guid);
      ocrAddDependence(depv[0].guid, child, 0, DB_MODE_CONST);
    }
    ocrEdtTemplateDestroy(tmp);
    return NULL_GUID;
  }

  u32 n = hi - lo;
  if(n <= c->width) {
    init_leaf(depv[0].guid, c, rank, lo, hi, parent, slot);
    return NULL_GUID;
  }

  /* Split on whole leaf widths, so a range's leaf count is exactly the range
   * divided by the leaf width at any depth of the recursion. */
  u32 u = (n + c->width - 1)/c->width;
  u32 k = u < FAN_BRANCH ? u : FAN_BRANCH;
  ocrGuid_t node = merge_create(c, depv[0].guid, k, rlo, rhi, parent, slot, rank);
  ocrGuid_t tmp;
  ocrHint_t edtHNT;
  ocrEdtTemplateCreate(&tmp, init_fan_edt, IP_NPARAM, 1);
  for(u32 i = 0; i < k; ++i) {
    u32 clo = lo + (u32)(((u64)u * i) / k) * c->width;
    u32 chi = lo + (u32)(((u64)u * (i+1)) / k) * c->width;
    if(chi > hi) chi = hi;
    u64 pv[IP_NPARAM];
    pv[IP_RLO] = rlo; pv[IP_RHI] = rhi;
    pv[IP_LO] = clo; pv[IP_HI] = chi; pv[IP_SLOT] = i+1;
    guid_store(pv+IP_PARENT, node);
    ocrGuid_t child;
    ocrEdtCreate(&child, tmp, IP_NPARAM, pv, 1, NULL, 0,
                 edt_at(&edtHNT, c, rank), NULL);
    if(i+1 == k) ocrDbRelease(depv[0].guid);
    ocrAddDependence(depv[0].guid, child, 0, DB_MODE_CONST);
  }
  ocrEdtTemplateDestroy(tmp);

  return NULL_GUID;
}

void init_launch(ocrGuid_t cfg, const config* c, ocrGuid_t parent, u32 slot)
{
  ocrGuid_t tmp, edt;
  ocrHint_t edtHNT;
  u64 pv[IP_NPARAM];
  child_ranks(c, pv, 0, c->nranks);
  pv[IP_SLOT] = slot;
  guid_store(pv+IP_PARENT, parent);
  ocrEdtTemplateCreate(&tmp, init_fan_edt, IP_NPARAM, 1);
  ocrEdtCreate(&edt, tmp, IP_NPARAM, pv, 1, NULL, 0, edt_at(&edtHNT, c, 0), NULL);
  ocrDbRelease(cfg);
  ocrAddDependence(cfg, edt, 0, DB_MODE_CONST);
  ocrEdtTemplateDestroy(tmp);
}
