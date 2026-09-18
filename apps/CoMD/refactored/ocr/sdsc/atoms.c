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
 * any task can generate the atoms of the box it owns without consulting a
 * shared generator or any other box. */
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

void init_boxes(ocrGuid_t* list, const u32 grid[3])
{
  u32 boxes_num = grid[0]*grid[1]*grid[2];
  for(u32 b = 0; b<boxes_num; ++b) {
    void* ptr;
    ocrHint_t blockHNT;
    // The creator never writes these blocks — DB_PROP_NO_ACQUIRE creates them
    // at their home (no acquire taken, hence no release and no payload to
    // publish) before the GUIDs are handed to the tasks that fill them.
    ocrDbCreate(list+b, &ptr, sizeof(box), DB_PROP_NO_ACQUIRE,
                comdBlockDbHint(&blockHNT, b, grid), NO_ALLOC);
  }
}

/* Lattice indices whose site can land in a box, from the box's own bounds: a
 * site sits at most one unit cell plus one displacement outside the interval
 * it belongs to, and an index outside the lattice does not exist because
 * positions are never wrapped. */
static void unit_range(const boxes* bxs, u32 bg, u32 d, s32* t0, s32* t1)
{
  real_t low  = (real_t)bg * bxs->box_size[d];
  real_t high = (real_t)(bg+1) * bxs->box_size[d];
  s32 lo = (s32)floor(low / bxs->lat) - 2;
  s32 hi = (s32)floor(high / bxs->lat) + 2;
  if(lo < 0) lo = 0;
  if(hi > (s32)bxs->unit[d]-1) hi = (s32)bxs->unit[d]-1;
  *t0 = lo; *t1 = hi;
}

/* Generate every atom whose site lands in box b.  The displacement is applied
 * before the box is chosen, so an atom is born in the box that owns it and no
 * migration pass is needed to settle generation. */
static u8 fill_box(const simulation* sim, u32 b, box* bx)
{
  const boxes* bxs = &sim->bxs;
  u32 bg[3]; box2grid(b, bg, bxs->grid[0], bxs->grid[0]*bxs->grid[1]);
  s32 t0[3], t1[3];
  for(u32 d = 0; d < 3; ++d)
    unit_range(bxs, bg[d], d, t0+d, t1+d);

  real_t mass = sim->pot.mass;
  real_t sigma = sqrt(kB_eV * bxs->temperature/mass);
  bx->atoms = 0;
  for(s32 ix = t0[0]; ix <= t1[0]; ++ix)
    for(s32 iy = t0[1]; iy <= t1[1]; ++iy)
      for(s32 iz = t0[2]; iz <= t1[2]; ++iz)
        for(u32 ib = 0; ib < 4; ++ib) {
          u32 gid = (((u32)ix*bxs->unit[1] + (u32)iy)*bxs->unit[2] + (u32)iz)*4 + ib;
          real3 r;
          r[0] = ((real_t)ix+fcc_basis[ib][0]) * bxs->lat;
          r[1] = ((real_t)iy+fcc_basis[ib][1]) * bxs->lat;
          r[2] = ((real_t)iz+fcc_basis[ib][2]) * bxs->lat;
          if(bxs->delta > 0.0) {
            u64 dseed = mk_seed(gid, 457);
            r[0] += (2.0*lcg61(&dseed)-1.0) * bxs->delta;
            r[1] += (2.0*lcg61(&dseed)-1.0) * bxs->delta;
            r[2] += (2.0*lcg61(&dseed)-1.0) * bxs->delta;
          }
          if(coordinates2box(bxs->grid, bxs->inv_box_size, r) != b) continue;
          if(bx->atoms >= MAXATOMS) return 1;
          u32 a = bx->atoms++;
          bx->gid[a] = gid;
          bx->r[a][0] = r[0]; bx->r[a][1] = r[1]; bx->r[a][2] = r[2];
          u64 seed = mk_seed(gid, 123);
          bx->p[a][0] = mass*sigma*gasdev(&seed);
          bx->p[a][1] = mass*sigma*gasdev(&seed);
          bx->p[a][2] = mass*sigma*gasdev(&seed);
          bx->f[a][0] = bx->f[a][1] = bx->f[a][2] = 0;
          bx->U[a] = bx->a[a] = 0;
        }
  return 0;
}

static ocrGuid_t gen_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  const simulation* sim = (const simulation*)depv[0].ptr;
  box* bx = (box*)depv[1].ptr;
  if(fill_box(sim, (u32)paramv[1], bx)) {
    ocrPrintf("ERROR box %u holds more than %d atoms TERMINATING\n",
              (u32)paramv[1], MAXATOMS);
    ocrShutdown();
    return NULL_GUID;
  }

  ocrGuid_t res; init_stats* res_ptr;
  ocrHint_t homeHNT;
  ocrDbCreate(&res, (void**)&res_ptr, sizeof(init_stats), 0,
              comdHomeDbHint(&homeHNT), NO_ALLOC);
  res_ptr->atoms = bx->atoms;
  res_ptr->max_occupancy = bx->atoms;
  res_ptr->p[0] = res_ptr->p[1] = res_ptr->p[2] = 0;
  res_ptr->p2 = 0;
  for(u32 a = 0; a < bx->atoms; ++a) {
    res_ptr->p[0] += bx->p[a][0];
    res_ptr->p[1] += bx->p[a][1];
    res_ptr->p[2] += bx->p[a][2];
    res_ptr->p2 += bx->p[a][0]*bx->p[a][0] + bx->p[a][1]*bx->p[a][1]
                 + bx->p[a][2]*bx->p[a][2];
  }
  ocrAddDependence(res, (ocrGuid_t){.guid = paramv[0]}, paramv[1], DB_MODE_CONST);

  return NULL_GUID;
}

/* Summing in dependence-slot order makes the whole-system sums a function of
 * the box index alone, so they do not move with the rank count. */
static ocrGuid_t gen_red_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  ocrGuid_t res; init_stats* res_ptr;
  ocrHint_t homeHNT;
  ocrDbCreate(&res, (void**)&res_ptr, sizeof(init_stats), 0,
              comdHomeDbHint(&homeHNT), NO_ALLOC);
  res_ptr->atoms = 0;
  res_ptr->max_occupancy = 0;
  res_ptr->p[0] = res_ptr->p[1] = res_ptr->p[2] = 0;
  res_ptr->p2 = 0;
  for(u32 b = 0; b < depc; ++b) {
    const init_stats* s = (const init_stats*)depv[b].ptr;
    res_ptr->atoms += s->atoms;
    if(s->max_occupancy > res_ptr->max_occupancy)
      res_ptr->max_occupancy = s->max_occupancy;
    res_ptr->p[0] += s->p[0];
    res_ptr->p[1] += s->p[1];
    res_ptr->p[2] += s->p[2];
    res_ptr->p2 += s->p2;
    ocrDbDestroy(depv[b].guid);
  }
  ocrAddDependence(res, (ocrGuid_t){.guid = paramv[0]}, paramv[1], DB_MODE_CONST);

  return NULL_GUID;
}

void fork_generate_atoms(ocrGuid_t sim, ocrGuid_t cont, u32 depc, ocrGuid_t* list, const u32 grid[3])
{
  u32 boxes_num = grid[0]*grid[1]*grid[2];
  ocrGuid_t tmp,red;
  ocrHint_t homeHNT;
  u64 red_paramv[2]; red_paramv[0] = cont.guid; red_paramv[1] = depc;
  //A join over every box can exceed the implementation's dependence limit; the
  //failed create leaves its GUID unwritten, so it must not be used.
  if(ocrEdtTemplateCreate(&tmp, gen_red_edt, 2, boxes_num) ||
     ocrEdtCreate(&red, tmp, 2, red_paramv, boxes_num, NULL, 0, comdHomeEdtHint(&homeHNT), NULL)) {
    ocrPrintf("ERROR cannot create a join EDT with %u dependences TERMINATING\n", boxes_num);
    ocrShutdown();
    return;
  }
  ocrEdtTemplateDestroy(tmp);

  ocrEdtTemplateCreate(&tmp, gen_edt, 2, 2);
  for(u32 b = 0; b < boxes_num; ++b) {
    u64 paramv[2]; paramv[0] = red.guid; paramv[1] = b;
    ocrGuid_t edt;
    ocrHint_t blockHNT;
    ocrEdtCreate(&edt, tmp, 2, paramv, 2, NULL, 0, comdBlockEdtHint(&blockHNT, b, grid), NULL);
    ocrAddDependence(sim, edt, 0, DB_MODE_CONST);
    ocrAddDependence(list[b], edt, 1, DB_MODE_RW);
  }
  ocrEdtTemplateDestroy(tmp);
}

/* Give the system the requested temperature: subtracting the mean momentum and
 * rescaling are both pure per-atom maps once the whole-system sums are known,
 * so the pass needs no second reduction. */
static ocrGuid_t temp_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  box* bx = (box*)depv[0].ptr;
  real_t vcm[3];
  vcm[0] = bits_real(paramv[0]);
  vcm[1] = bits_real(paramv[1]);
  vcm[2] = bits_real(paramv[2]);
  real_t scale = bits_real(paramv[3]);

  for(u32 a = 0; a < bx->atoms; ++a) {
    bx->p[a][0] = (bx->p[a][0]-vcm[0])*scale;
    bx->p[a][1] = (bx->p[a][1]-vcm[1])*scale;
    bx->p[a][2] = (bx->p[a][2]-vcm[2])*scale;
  }

  return NULL_GUID;
}

static ocrGuid_t temp_red_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  return NULL_GUID;
}

void fork_set_temperature(ocrGuid_t cont, u32 depc, ocrGuid_t* list,
                          const u32 grid[3], const real3 vcm, real_t scale)
{
  u32 boxes_num = grid[0]*grid[1]*grid[2];
  ocrGuid_t tmp,red,red_event;
  ocrHint_t homeHNT;
  OEVT_COUNTED_PRE(red_event);
  if(ocrEdtTemplateCreate(&tmp, temp_red_edt, 0, boxes_num) ||
     ocrEdtCreate(&red, tmp, 0, NULL, boxes_num, NULL, OEVT_COUNTED_PROP(0), comdHomeEdtHint(&homeHNT), &red_event)) {
    ocrPrintf("ERROR cannot create a join EDT with %u dependences TERMINATING\n", boxes_num);
    ocrShutdown();
    return;
  }
  ocrAddDependence(red_event, cont, depc, DB_MODE_CONST);
  ocrEdtTemplateDestroy(tmp);

  u64 paramv[4];
  paramv[0] = real_bits(vcm[0]);
  paramv[1] = real_bits(vcm[1]);
  paramv[2] = real_bits(vcm[2]);
  paramv[3] = real_bits(scale);
  ocrEdtTemplateCreate(&tmp, temp_edt, 4, 1);
  for(u32 b = 0; b < boxes_num; ++b) {
    ocrGuid_t edt;
    ocrHint_t blockHNT;
    OEVT_COUNTED_PRE(red_event);
    ocrEdtCreate(&edt, tmp, 4, paramv, 1, NULL, OEVT_COUNTED_PROP(0), comdBlockEdtHint(&blockHNT, b, grid), &red_event);
    ocrAddDependence(red_event, red, b, DB_MODE_CONST);
    ocrAddDependence(list[b], edt, 0, DB_MODE_RW);
  }
  ocrEdtTemplateDestroy(tmp);
}
