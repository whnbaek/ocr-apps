#include <string.h>
#include <ocr.h>

#include "command.h"
#include "simulation.h"
#include "cells.h"

/* Relate every atom of a block read this step to cell b.  The block is that
 * of b's neighbour `self`, or b's own when `self` is 26.  An atom is placed
 * by its position alone, so every reader of a block agrees with its writer
 * about which cell each atom is in without the block saying so. */
void cell_classify(const config* c, u32 b, const u32 nb[26], const u32 sh[26],
                   const box* x, u32 self, u8 rel[MAXATOMS])
{
  for(u32 j = 0; j < x->atoms; ++j) {
    u32 d = coordinates2box(c->grid, c->inv_box_size, x->r[j]);
    if(d == b) { rel[j] = ATOM_MINE; continue; }
    if(self < 26 && d == nb[self]) { rel[j] = (u8)sh[self]; continue; }
    u32 k = 0;
    while(k < 26 && nb[k] != d) ++k;
    rel[j] = k < 26 ? (u8)sh[k] : ATOM_FAR;
  }
}

/* Where a step of cell b finds each of its 26 neighbours.  A neighbour the
 * brick holds is read from its own block; one outside it is read from the run
 * its owner packed, which is that owner's layer facing back along the axes
 * the neighbour left the brick on.  Nothing about this map travels between
 * the task that wires the dependences and the task that reads them: both
 * derive it from the geometry, so both agree by construction. */
void cell_reads(const config* c, u32 b, const u32 nb[26], read_set* s)
{
  u32 row = c->grid[0], plane = c->grid[0]*c->grid[1];
  u32 lo[3], n[3];
  rank_brick(c, cell_owner(c, b), lo, n);

  s->count = 0;
  for(u32 k = 0; k < 26; ++k) {
    u32 mg[3]; box2grid(nb[k], mg, row, plane);
    s32 v[3]; dir_vec(k, v);
    s32 out[3]; u8 leaves = 0;
    for(u32 i = 0; i < 3; ++i) {
      out[i] = (mg[i] >= lo[i] && mg[i] < lo[i]+n[i]) ? 0 : v[i];
      if(out[i]) leaves = 1;
    }

    read_blk want; u32 at = 0;
    want.cell = nb[k]; want.rank = 0; want.dir = 0; want.run = 0;
    if(leaves) {
      s32 d[3];
      for(u32 i = 0; i < 3; ++i) d[i] = -out[i];
      u32 owner = cell_owner(c, nb[k]);
      u32 olo[3], on[3];
      rank_brick(c, owner, olo, on);
      u32 pos = layer_pos(olo, on, d, mg);
      want.cell = ~(u32)0; want.rank = owner; want.dir = dir_index(d);
      want.run = pos/HALO_CHUNK;
      at = pos%HALO_CHUNK;
    }

    u32 j = 0;
    while(j < s->count && (s->blk[j].cell != want.cell || s->blk[j].rank != want.rank ||
                           s->blk[j].dir != want.dir || s->blk[j].run != want.run)) ++j;
    if(j == s->count) s->blk[s->count++] = want;
    s->slot[k] = (u8)j;
    s->at[k] = (u8)at;
  }
}

u8 cell_take(box* out, const box* x, u32 j)
{
  if(out->atoms >= MAXATOMS) return 1;
  u32 a = out->atoms++;
  out->gid[a] = x->gid[j];
  memcpy(out->r[a], x->r[j], sizeof(real3));
  memcpy(out->p[a], x->p[j], sizeof(real3));
  return 0;
}

/* Copy the run's cells into the one block the brick that reads them takes.
 * A run holds the generation the step is about to read, which the previous
 * step wrote and this one only reads, and the tree-shaped barrier between
 * steps stands between this write of the run and the last step's readers of
 * it -- which is why one block per run, rather than one per generation,
 * carries the whole run. */
static ocrGuid_t pack_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  box* out = (box*)depv[0].ptr;
  for(u32 j = 1; j < depc; ++j)
    memcpy(out+(j-1), depv[j].ptr, sizeof(box));

  return depv[0].guid;
}

/* Take the packed run to the brick that reads it, once, before any of that
 * brick's cell tasks exists: however many of them the run serves, it has
 * crossed the fabric once. */
static ocrGuid_t proxy_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  return NULL_GUID;
}

void spawn_halo(const spawn_ctx* x, u32 lo, u32 hi)
{
  const config* c = x->c;
  u32 gen = (u32)x->arg[SA_GEN];
  const ocrGuid_t* hb = halo_table(x->l) + layer_table(x->l)[x->rank*26];
  ocrGuid_t ptmp, xtmp;
  ocrEdtTemplateCreate(&ptmp, pack_edt, 0, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&xtmp, proxy_edt, 0, 1);
  for(u32 i = lo; i < hi; ++i) {
    u32 dir, run;
    brick_halo_at(c, x->rank, i, &dir, &run);
    s32 d[3]; dir_vec(dir, d);
    u32 first = run*HALO_CHUNK;
    u32 len = layer_cells(x->bdim, d) - first;
    if(len > HALO_CHUNK) len = HALO_CHUNK;

    ocrGuid_t pack, packed;
    ocrHint_t edtHNT;
    OEVT_COUNTED_PRE(packed);
    ocrEdtCreate(&pack, ptmp, 0, NULL, len+1, NULL, OEVT_COUNTED_PROP(0),
                 edt_at(&edtHNT, c, x->rank), &packed);
    ocrAddDependence(hb[i], pack, 0, DB_MODE_RW);
    for(u32 j = 0; j < len; ++j) {
      u32 b = layer_cell(c, x->blo, x->bdim, d, first+j);
      ocrAddDependence(x->l->cell[b].bx[gen], pack, j+1, DB_MODE_CONST);
    }

    ocrGuid_t proxy, done;
    OEVT_COUNTED_PRE(done);
    ocrEdtCreate(&proxy, xtmp, 0, NULL, 1, NULL, OEVT_COUNTED_PROP(0),
                 edt_at(&edtHNT, c, rank_shift(c, x->rank, d)), &done);
    ocrAddDependence(done, x->join, i-lo, DB_MODE_CONST);
    spawn_last(x, i, hi);
    ocrAddDependence(packed, proxy, 0, DB_MODE_CONST);
  }
  ocrEdtTemplateDestroy(ptmp);
  ocrEdtTemplateDestroy(xtmp);
}

static ocrGuid_t halo_free_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  ocrDbDestroy(depv[0].guid);

  return NULL_GUID;
}

void spawn_halo_free(const spawn_ctx* x, u32 lo, u32 hi)
{
  const ocrGuid_t* hb = halo_table(x->l) + layer_table(x->l)[x->rank*26];
  ocrGuid_t tmp;
  ocrEdtTemplateCreate(&tmp, halo_free_edt, 0, 1);
  for(u32 i = lo; i < hi; ++i) {
    ocrGuid_t edt, done;
    ocrHint_t edtHNT;
    OEVT_COUNTED_PRE(done);
    ocrEdtCreate(&edt, tmp, 0, NULL, 1, NULL, OEVT_COUNTED_PROP(0),
                 edt_at(&edtHNT, x->c, x->rank), &done);
    ocrAddDependence(done, x->join, i-lo, DB_MODE_CONST);
    ocrGuid_t h = hb[i];
    spawn_last(x, i, hi);
    ocrAddDependence(h, edt, 0, DB_MODE_RW);
  }
  ocrEdtTemplateDestroy(tmp);
}

/* Reclaim a cell's blocks from the rank that owns them, so teardown costs
 * the tree's depth rather than one task's round trip per cell. */
static ocrGuid_t release_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  ocrDbDestroy(guid_load(paramv));
  ocrDbDestroy(depv[0].guid);

  return NULL_GUID;
}

void spawn_release(const spawn_ctx* x, u32 lo, u32 hi)
{
  ocrGuid_t tmp;
  ocrEdtTemplateCreate(&tmp, release_edt, GUID_WORDS, 1);
  for(u32 i = lo; i < hi; ++i) {
    u32 b = spawn_cell(x, i);
    u64 pv[GUID_WORDS];
    guid_store(pv, x->l->cell[b].bx[1]);
    ocrGuid_t edt, done;
    ocrHint_t edtHNT;
    OEVT_COUNTED_PRE(done);
    ocrEdtCreate(&edt, tmp, GUID_WORDS, pv, 1, NULL, OEVT_COUNTED_PROP(0),
                 edt_at(&edtHNT, x->c, x->rank), &done);
    ocrAddDependence(done, x->join, i-lo, DB_MODE_CONST);
    ocrGuid_t bx = x->l->cell[b].bx[0];
    spawn_last(x, i, hi);
    ocrAddDependence(bx, edt, 0, DB_MODE_RW);
  }
  ocrEdtTemplateDestroy(tmp);
}
