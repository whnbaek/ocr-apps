#include <string.h>
#include <ocr.h>

#include "simulation.h"
#include "cells.h"

enum { TP_VX, TP_VY, TP_VZ, TP_SCALE, TP_NPARAM };
enum { ST_BOX, ST_SLOT, ST_ARG, ST_PARENT = ST_ARG + SA_NARG, ST_NPARAM = ST_PARENT + GUID_WORDS };
/* Step slots: the configuration, the cell's block being read, the block
 * being written, then the blocks the 26 neighbours are read from -- one per
 * neighbour inside the brick, one per layer run for the ones outside it, so
 * how many there are is the cell's own and travels in the create. */
enum { SS_CFG, SS_IN, SS_OUT, SS_NB };

/* Give the system the requested temperature: subtracting the mean momentum
 * and rescaling are both pure per-atom maps once the two whole-system sums
 * are known, so the pass needs no second reduction. */
static ocrGuid_t temp_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  box* bx = (box*)depv[0].ptr;
  real_t vcm[3];
  vcm[0] = bits_real(paramv[TP_VX]);
  vcm[1] = bits_real(paramv[TP_VY]);
  vcm[2] = bits_real(paramv[TP_VZ]);
  real_t scale = bits_real(paramv[TP_SCALE]);

  for(u32 a = 0; a < bx->atoms; ++a) {
    bx->p[a][0] = (bx->p[a][0]-vcm[0])*scale;
    bx->p[a][1] = (bx->p[a][1]-vcm[1])*scale;
    bx->p[a][2] = (bx->p[a][2]-vcm[2])*scale;
  }

  return NULL_GUID;
}

void spawn_temp(const spawn_ctx* x, u32 lo, u32 hi)
{
  ocrGuid_t tmp;
  ocrEdtTemplateCreate(&tmp, temp_edt, TP_NPARAM, 1);
  for(u32 i = lo; i < hi; ++i) {
    u32 b = spawn_cell(x, i);
    ocrGuid_t edt, done;
    ocrHint_t edtHNT;
    OEVT_COUNTED_PRE(done);
    ocrEdtCreate(&edt, tmp, TP_NPARAM, (u64*)x->arg, 1, NULL, OEVT_COUNTED_PROP(0),
                 edt_at(&edtHNT, x->c, x->rank), &done);
    ocrAddDependence(done, x->join, i-lo, DB_MODE_CONST);
    ocrAddDependence(x->l->cell[b].bx[0], edt, 0, DB_MODE_RW);
  }
  ocrEdtTemplateDestroy(tmp);
}

/* Neighbour k's box, wherever the read set put it: a block of its own, or a
 * position inside a layer run. */
static const box* neighbour(const ocrEdtDep_t depv[], const read_set* s, u32 k)
{
  return (const box*)depv[SS_NB + s->slot[k]].ptr + s->at[k];
}

/* The cell's atoms for this step: those of its own block whose position is
 * still in the cell, in their stored order, then those of each neighbour's
 * block that drifted in, in neighbour order. */
static u8 gather(const config* c, u32 b, const u32 nb[26], const u32 sh[26],
                 const box* in, ocrEdtDep_t depv[], const read_set* s,
                 u8 rel_in[MAXATOMS], u8 rel[26][MAXATOMS], box* out)
{
  out->atoms = 0;
  cell_classify(c, b, nb, sh, in, 26, rel_in);
  for(u32 j = 0; j < in->atoms; ++j)
    if(rel_in[j] == ATOM_MINE && cell_take(out, in, j)) return 1;
  for(u32 k = 0; k < 26; ++k) {
    const box* x = neighbour(depv, s, k);
    cell_classify(c, b, nb, sh, x, k, rel[k]);
    for(u32 j = 0; j < x->atoms; ++j)
      if(rel[k][j] == ATOM_MINE && cell_take(out, x, j)) return 1;
  }
  return 0;
}

static void kick(box* bx, const real3* f, real_t dt)
{
  for(u32 a = 0; a < bx->atoms; ++a) {
    bx->p[a][0] += dt*f[a][0];
    bx->p[a][1] += dt*f[a][1];
    bx->p[a][2] += dt*f[a][2];
  }
}

/* One step of velocity Verlet for one cell, from the blocks as the previous
 * step left them: take in the atoms whose positions are now in the cell,
 * compute the forces against every atom within reach, kick, take the energy
 * point, kick again and drift.  Which kicks apply and whether the energy
 * point is taken depend on where the step sits in its block; the arithmetic
 * per atom is the published integrator's, in its order. */
static ocrGuid_t step_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  const config* c = (const config*)depv[SS_CFG].ptr;
  const box* in = (const box*)depv[SS_IN].ptr;
  box* out = (box*)depv[SS_OUT].ptr;
  u32 b = (u32)paramv[ST_BOX];
  const u64* arg = paramv + ST_ARG;
  u32 flags = (u32)arg[SA_FLAGS];

  u32 nb[26], sh[26];
  box_neighbours(b, c->grid, nb, sh);
  read_set rs;
  cell_reads(c, b, nb, &rs);

  u8 rel_in[MAXATOMS], rel[26][MAXATOMS];
  if(gather(c, b, nb, sh, in, depv, &rs, rel_in, rel, out)) {
    ocrPrintf("ERROR cell %u holds more than %d atoms TERMINATING\n", b, MAXATOMS);
    ocrShutdown();
    return NULL_GUID;
  }

  real3 f[MAXATOMS];
  memset(f, 0, sizeof(real3)*out->atoms);
  real_t pe = lj_cell_self(&c->pot, out, f);
  for(u32 k = 0; k < 26; ++k)
    pe += lj_cell_pair(&c->pot, c->shift, out, f, neighbour(depv, &rs, k), rel[k]);
  pe += lj_cell_pair(&c->pot, c->shift, out, f, in, rel_in);

  real_t dt = bits_real(arg[SA_KICK_PRE]);
  if(dt != 0.0) kick(out, f, dt);

  real_t ke = 0.0;
  if(flags & SF_ENERGY)
    for(u32 a = 0; a < out->atoms; ++a)
      ke += c->pot.inv_mass_2*(out->p[a][0]*out->p[a][0]+
                               out->p[a][1]*out->p[a][1]+
                               out->p[a][2]*out->p[a][2]);

  dt = bits_real(arg[SA_KICK_POST]);
  if(dt != 0.0) kick(out, f, dt);

  if(flags & SF_DRIFT) {
    real_t dt_mass = c->pot.inv_mass_dt;
    for(u32 a = 0; a < out->atoms; ++a) {
      for(u32 d = 0; d < 3; ++d) {
        out->r[a][d] += dt_mass*out->p[a][d];
        if(out->r[a][d] < 0.0) out->r[a][d] += c->local_max[d];
        else if(out->r[a][d] >= c->local_max[d]) out->r[a][d] -= c->local_max[d];
      }
      u32 dst = coordinates2box(c->grid, c->inv_box_size, out->r[a]);
      if(dst != b && !is_neighbour(c->grid, b, dst)) {
        ocrPrintf("ERROR cell %u cannot hand atom to cell %u TERMINATING\n", b, dst);
        ocrShutdown();
        return NULL_GUID;
      }
    }
  }

  ocrGuid_t res; partial* res_ptr;
  ocrDbCreate(&res, (void**)&res_ptr, sizeof(partial), 0, NULL_HINT, NO_ALLOC);
  res_ptr->pe = pe;
  res_ptr->ke = ke;
  res_ptr->n = out->atoms;
  ocrAddDependence(res, guid_load(paramv+ST_PARENT), (u32)paramv[ST_SLOT], DB_MODE_CONST);

  return NULL_GUID;
}

void spawn_step(const spawn_ctx* x, u32 lo, u32 hi)
{
  u32 gen = (u32)x->arg[SA_GEN];
  const ocrGuid_t* hb = halo_table(x->l);
  const u32* tab = layer_table(x->l);
  ocrGuid_t tmp;
  ocrEdtTemplateCreate(&tmp, step_edt, ST_NPARAM, EDT_PARAM_UNK);
  for(u32 i = lo; i < hi; ++i) {
    u32 b = spawn_cell(x, i);
    u32 nb[26], sh[26];
    box_neighbours(b, x->c->grid, nb, sh);
    read_set rs;
    cell_reads(x->c, b, nb, &rs);
    u64 pv[ST_NPARAM];
    pv[ST_BOX] = b;
    pv[ST_SLOT] = i-lo;
    for(u32 j = 0; j < SA_NARG; ++j) pv[ST_ARG+j] = x->arg[j];
    guid_store(pv+ST_PARENT, x->join);
    ocrGuid_t edt;
    ocrHint_t edtHNT;
    ocrEdtCreate(&edt, tmp, ST_NPARAM, pv, SS_NB + rs.count, NULL, 0,
                 edt_at(&edtHNT, x->c, x->rank), NULL);
    ocrAddDependence(x->cfg, edt, SS_CFG, DB_MODE_CONST);
    ocrAddDependence(x->l->cell[b].bx[gen], edt, SS_IN, DB_MODE_CONST);
    ocrAddDependence(x->l->cell[b].bx[gen^1], edt, SS_OUT, DB_MODE_RW);
    for(u32 k = 0; k < rs.count; ++k) {
      const read_blk* r = &rs.blk[k];
      ocrAddDependence(r->cell != ~(u32)0
                         ? x->l->cell[r->cell].bx[gen]
                         : hb[tab[r->rank*26 + r->dir] + r->run],
                       edt, SS_NB+k, DB_MODE_CONST);
    }
  }
  ocrEdtTemplateDestroy(tmp);
}
