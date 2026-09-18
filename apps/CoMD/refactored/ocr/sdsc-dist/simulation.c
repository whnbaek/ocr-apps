#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ocr.h>

#include "command.h"
#include "timers.h"
#include "cells.h"
#include "simulation.h"

void lj_print(const potential* p);

static u8 sanity_checks(const command* cmd, const config* c)
{
  u8 failure = 0;
  double min = 2*c->pot.cutoff;
  if(cmd->nx*c->lat < min || cmd->ny*c->lat < min || cmd->nz*c->lat < min) {
    failure |= 2;
    ocrPrintf("\nSimulation too small.\n"
              "  Increase the number of unit cells to make the simulation\n"
              "  at least (%3.2f, %3.2f. %3.2f) Ansgstroms in size\n", min, min, min);
  }
  /* A cell's 26 neighbours are distinct, and a wrapped neighbour is told from
   * an unwrapped one by its coordinate differing by grid-1, only from three
   * cells per dimension upward. */
  for(u32 i = 0; i < 3; ++i)
    if(c->grid[i] < 3) {
      failure |= 4;
      ocrPrintf("\nAt least three link cells per dimension are required, have %u.\n",
                c->grid[i]);
      break;
    }
  /* Every rank must own a brick: a rank grid wider than the cell grid on
   * some axis would leave ranks with nothing to own. */
  for(u32 i = 0; i < 3; ++i)
    if(c->ranks[i] > c->grid[i]) {
      failure |= 1;
      ocrPrintf("\nThe rank grid (%u, %u, %u) exceeds the link cells (%u, %u, %u) on axis %u.\n",
                c->ranks[0], c->ranks[1], c->ranks[2], c->grid[0], c->grid[1], c->grid[2], i);
      break;
    }
  if(c->width < 1 || c->width > 1024) {
    failure |= 8;
    ocrPrintf("\nThe spawn width must be between 1 and 1024 link cells, have %u.\n",
              c->width);
  }
  /* The run advances in blocks of one period and the period divides the step
   * counter, so neither may be zero. */
  if(cmd->period == 0 || cmd->steps == 0) {
    failure |= 128;
    ocrPrintf("\nThe step count and the period must both be at least one, have %u and %u.\n",
              cmd->steps, cmd->period);
  }
  if(c->delta >= c->lat) {
    failure |= 16;
    ocrPrintf("\nThe initial displacement must stay below one lattice spacing.\n");
  }
  if(cmd->doeam) {
    failure |= 32;
    ocrPrintf("\nOnly the Lennard-Jones potential is implemented.\n");
  }
  if(cmd->bad) {
    failure |= 64;
    ocrPrintf("\nUnrecognised command line switch.\n");
  }
  return failure;
}

u8 init_config(command* cmd, config* c, simulation* sim)
{
  sim->step = 0;
  sim->steps = cmd->steps;
  sim->period = cmd->period;
  sim->atoms = 0;
  sim->max_occupancy = 0;
  sim->e_potential = 0.0;
  sim->e_kinetic = 0.0;
  sim->energy0 = 0.0;
  sim->atoms0 = 0;

  c->dt = cmd->dt;
  c->temperature = cmd->temperature;
  c->delta = cmd->initial_delta;
  c->width = cmd->width;
  c->unit[0] = cmd->nx; c->unit[1] = cmd->ny; c->unit[2] = cmd->nz;

  init_lj(&c->pot, c->dt);
  c->lat = cmd->lat < 0.0 ? c->pot.lat : cmd->lat;

  for(u32 i = 0; i < 3; ++i) {
    c->local_max[i] = c->lat*c->unit[i];
    c->grid[i] = c->local_max[i]/c->pot.cutoff;
    if(c->grid[i] == 0) c->grid[i] = 1;
    c->box_size[i] = c->local_max[i]/(real_t)c->grid[i];
    c->inv_box_size[i] = 1.0/c->box_size[i];
  }
  c->boxes_num = c->grid[0]*c->grid[1]*c->grid[2];

  for(u32 f = 0; f < 27; ++f) {
    c->shift[f][0] = ((real_t)(f%3)-1)*c->local_max[0];
    c->shift[f][1] = ((real_t)((f/3)%3)-1)*c->local_max[1];
    c->shift[f][2] = ((real_t)(f/9)-1)*c->local_max[2];
  }

  u64 nranks = 1;
  ocrAffinityCount(AFFINITY_PD, &nranks);
  if(nranks < 1) nranks = 1;
  c->nranks = (u32)nranks;
  u64 rx = 1, ry = 1, rz = 1;
  splitDimension_Cart3D(nranks, &rx, &ry, &rz);
  c->ranks[0] = (u32)rx; c->ranks[1] = (u32)ry; c->ranks[2] = (u32)rz;

  return sanity_checks(cmd, c);
}

void validate_result(const simulation* sim)
{
  real_t energy = (sim->e_potential + sim->e_kinetic) / sim->atoms;

  ocrPrintf("\n\n");
  ocrPrintf("Simulation Validation:\n");
  ocrPrintf("  Initial energy  : %14.12f\n", sim->energy0);
  ocrPrintf("  Final energy    : %14.12f\n", energy);
  ocrPrintf("  eFinal/eInitial : %f\n", energy/sim->energy0);

  int atoms_diff = sim->atoms - sim->atoms0;
  if(atoms_diff == 0)
    ocrPrintf("  Final atom count : %d, no atoms lost\n", sim->atoms);
  else {
    ocrPrintf("#############################\n");
    ocrPrintf("# WARNING: %6d atoms lost #\n", atoms_diff);
    ocrPrintf("#############################\n");
  }
}

void print_status_header()
{
  ocrPrintf(
  "#                                                                                         Performance\n"
  "#  Loop   Time(fs)       Total Energy   Potential Energy     Kinetic Energy  Temperature   (us/atom)     # Atoms\n");
}

void print_status(const config* c, simulation* sim, double time)
{
   u32 step = sim->step; u32 atoms = sim->atoms;
   real_t energy = (sim->e_potential+sim->e_kinetic) / atoms;
   real_t e_k = sim->e_kinetic / atoms;
   real_t e_u = sim->e_potential / atoms;
   real_t temp = e_k * kB_eV_1_5;

   double time_per_atom = 1.0e6*time/(double)((sim->period > step ? sim->period : 1)*atoms);
   ocrPrintf(" %6d %10.2f %18.12f %18.12f %18.12f %12.4f %10.4f %12d\n",
          step, step*c->dt, energy, e_u, e_k, temp, time_per_atom, atoms);
}

void print_simulation(const config* c, const simulation* sim)
{
   ocrPrintf("Simulation data: \n");
   ocrPrintf("  Total atoms        : %d\n", sim->atoms);
   ocrPrintf("  Min bounds  : [ %14.10f, %14.10f, %14.10f ]\n", 0.0, 0.0, 0.0);
   ocrPrintf("  Max bounds  : [ %14.10f, %14.10f, %14.10f ]\n\n",
          c->local_max[0], c->local_max[1], c->local_max[2]);
   ocrPrintf("  Boxes        : %6d,%6d,%6d = %8d\n",
          c->grid[0], c->grid[1], c->grid[2], c->boxes_num);
   ocrPrintf("  Box size           : [ %14.10f, %14.10f, %14.10f ]\n",
          c->box_size[0], c->box_size[1], c->box_size[2]);
   ocrPrintf("  Box factor         : [ %14.10f, %14.10f, %14.10f ] \n",
          c->box_size[0]/c->pot.cutoff,
          c->box_size[1]/c->pot.cutoff,
          c->box_size[2]/c->pot.cutoff);
   ocrPrintf("  Max Link Cell Occupancy: %d of %d\n\n", sim->max_occupancy, MAXATOMS);
   ocrPrintf("  Rank grid          : %6d,%6d,%6d\n", c->ranks[0], c->ranks[1], c->ranks[2]);
   ocrPrintf("  Cells per spawn leaf: %6d\n\n", c->width);
   ocrPrintf("Potential data: \n");
   lj_print(&c->pot);
}
