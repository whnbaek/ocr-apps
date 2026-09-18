#include <string.h>
#include <ocr.h>

#include "command.h"
#include "simulation.h"
#include "cells.h"

#define MAX(a,b) ((a)>(b) ? (a):(b))

void init_lattice(simulation* sim, command* cmd, real_t lattice_constant, ocrGuid_t** list)
{
  boxes* bxs = &sim->bxs;
  bxs->unit[0] = cmd->nx;
  bxs->unit[1] = cmd->ny;
  bxs->unit[2] = cmd->nz;
  bxs->lat = lattice_constant;
  bxs->temperature = cmd->temperature;
  bxs->delta = cmd->initial_delta;
  bxs->local_max[0] = lattice_constant*cmd->nx;
  bxs->local_max[1] = lattice_constant*cmd->ny;
  bxs->local_max[2] = lattice_constant*cmd->nz;
  for(u8 i = 0; i < 3; i++) {
    bxs->local_min[i] = 0;
    bxs->grid[i] = bxs->local_max[i]/sim->pot.cutoff;
    bxs->box_size[i] = bxs->local_max[i]/(real_t)bxs->grid[i];
    bxs->inv_box_size[i] = 1.0/bxs->box_size[i];
  }
  bxs->atoms = 0;
  bxs->max_occupancy = 0;

  sim->bxs.boxes_num = sim->bxs.grid[0]*sim->bxs.grid[1]*sim->bxs.grid[2];
  ocrDbCreate(&bxs->list, (void**)list, sizeof(ocrGuid_t)*sim->bxs.boxes_num, 0, NULL_HINT, NO_ALLOC);
  init_boxes(*list, sim->bxs.grid);

  for(int f = 0; f < 27; ++f) {
    sim->bxs.shift[f][0] = ((f%3)-1)*sim->bxs.local_max[0];
    sim->bxs.shift[f][1] = (((f/3)%3)-1)*sim->bxs.local_max[1];
    sim->bxs.shift[f][2] = ((f/9)-1)*sim->bxs.local_max[2];
  }
}

static void copy_atom(box* src_ptr, u32 a, box* dst_ptr, u32 b)
{
   dst_ptr->gid[b] = src_ptr->gid[a];
   memcpy(dst_ptr->r[b], src_ptr->r[a], sizeof(real3));
   memcpy(dst_ptr->p[b], src_ptr->p[a], sizeof(real3));
   memcpy(dst_ptr->f[b], src_ptr->f[a], sizeof(real3));
   memcpy(dst_ptr->U+b,  src_ptr->U+a,  sizeof(real_t));
}

static void sort_atoms(box* bxs_ptr)
{
}

static void move_atom2(box* src_ptr, u32 a, box* dst_ptr)
{
  copy_atom(src_ptr, a, dst_ptr, dst_ptr->atoms);
  ++dst_ptr->atoms;
  ocrAssert(dst_ptr->atoms < MAXATOMS);
  if(--src_ptr->atoms)
    copy_atom(src_ptr, src_ptr->atoms, src_ptr, a);
}

static ocrGuid_t redistribute_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  u32 boxes_num = depc-1;
  simulation* sim = (simulation*)depv[depc-1].ptr;
  sim->bxs.max_occupancy = 0;
  for(u32 b = 0; b < boxes_num; ++b) {
    box* bxs_ptr = (box*)depv[b].ptr;
    /* A departure refills its slot from the box's tail, so the slot must be
     * re-examined rather than stepped over: advancing only when the atom in
     * hand stays is what makes every atom visited exactly once. */
    u32 a = 0;
    while(a < bxs_ptr->atoms) {
      u32 bb = coordinates2box(sim->bxs.grid, sim->bxs.inv_box_size, bxs_ptr->r[a]);
      if(b == bb) { ++a; continue; }
      move_atom2(bxs_ptr, a, (box*)depv[bb].ptr);
    }
  }
  for(u32 b = 0; b < boxes_num; ++b) {
    box* bxs_ptr = (box*)depv[b].ptr;
    sort_atoms(bxs_ptr);
    sim->bxs.max_occupancy = MAX(sim->bxs.max_occupancy, bxs_ptr->atoms);
  }

  return NULL_GUID;
}

void fork_redistribute(ocrGuid_t sim, ocrGuid_t cont, u32 depc, ocrGuid_t* list, u32 boxes_num)
{
  ocrGuid_t tmp, red, red_e;
  ocrHint_t homeHNT;
  //A join over every box can exceed the implementation's dependence limit; the
  //failed create leaves its GUID unwritten, so it must not be used.
  OEVT_COUNTED_PRE(red_e);
  if(ocrEdtTemplateCreate(&tmp,redistribute_edt, 0, boxes_num+1) ||
     ocrEdtCreate(&red, tmp, 0, NULL, boxes_num+1, NULL, OEVT_COUNTED_PROP(0), comdHomeEdtHint(&homeHNT), &red_e)) {
    ocrPrintf("ERROR cannot create a join EDT with %u dependences TERMINATING\n", boxes_num+1);
    ocrShutdown();
    return;
  }
  ocrAddDependence(red_e, cont, depc, DB_MODE_CONST);
  for(u32 b = 0; b < boxes_num; ++b)
    ocrAddDependence(list[b], red, b, DB_MODE_RW);
  ocrAddDependence(sim, red, boxes_num, DB_MODE_RW);
  ocrEdtTemplateDestroy(tmp);
}
