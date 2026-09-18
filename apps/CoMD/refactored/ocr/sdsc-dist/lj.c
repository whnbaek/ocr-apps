#include <stdio.h>
#include <string.h>
#include <ocr.h>

#include "simulation.h"
#include "cells.h"

#define POT_SHIFT 1.0

void lj_print(const potential* p)
{
  ocrPrintf("  Potential type   : Lennard-Jones\n");
  ocrPrintf("  Species name     : %s\n", p->name);
  ocrPrintf("  Atomic number    : %d\n", p->atomic_no);
  ocrPrintf("  Mass             : "FMT1" amu\n", p->mass / amu2internal_mass);
  ocrPrintf("  Lattice Type     : %s\n", p->lattice_type);
  ocrPrintf("  Lattice spacing  : %f Angstroms\n", p->lat);
  ocrPrintf("  Cutoff           : %f Angstroms\n", p->cutoff);
  ocrPrintf("  Epsilon          : %f eV\n", p->lj.epsilon);
  ocrPrintf("  Sigma            : %f Angstroms\n", p->lj.sigma);
}

void init_lj(potential* p, real_t dt)
{
  p->cutoff = 2.5*2.315;
  p->mass = 63.55 * amu2internal_mass;
  p->inv_mass_2 = 1/(2*p->mass);
  p->inv_mass_dt = dt/p->mass;
  p->lat = 3.615;
  strcpy(p->lattice_type, "FCC");
  strcpy(p->name, "Cu");
  p->atomic_no = 29;
  p->lj.epsilon = 0.167;
  p->lj.sigma = 2.315;
}

/* Pairs between a cell's atoms and the atoms of a block read this step, each
 * of the latter shifted by the periodic offset of the cell its position is
 * in.  An atom that is in the cell itself pairs inside the cell, and one out
 * of reach cannot be within the cutoff, so both are skipped. */
real_t lj_cell_pair(const potential* p, const real3* shift, const box* a, real3* f,
                    const box* b, const u8* rel)
{
  real_t sigma = p->lj.sigma, epsilon = p->lj.epsilon, cut = p->cutoff;
  real_t e_pot = 0;
  real_t cut2 = cut*cut;
  real_t s6 = sigma*sigma*sigma*sigma*sigma*sigma;
  real_t cut6 = s6/(cut2*cut2*cut2);
  real_t eshift = POT_SHIFT*cut6*(cut6-1);

  for(u32 aa = 0; aa < a->atoms; ++aa)
    for(u32 bb = 0; bb < b->atoms; ++bb) {
      if(rel[bb] > 26) continue;
      const real_t* fshift = shift[rel[bb]];

      real_t r2; real3 dr;
      dr[0] = a->r[aa][0]-(b->r[bb][0]+fshift[0]);
      dr[1] = a->r[aa][1]-(b->r[bb][1]+fshift[1]);
      dr[2] = a->r[aa][2]-(b->r[bb][2]+fshift[2]);
      r2=dr[0]*dr[0]+dr[1]*dr[1]+dr[2]*dr[2];

      if ( r2 > cut2) continue;
      r2 = 1.0/r2;

      real_t r6 = s6 * (r2*r2*r2);
      real_t energy = (r6*(r6-1.0)-eshift)*0.5;
      e_pot += energy;

      real_t fr = -4.0*epsilon*r6*r2*(12.0*r6-6.0);
      f[aa][0] -= dr[0]*fr;
      f[aa][1] -= dr[1]*fr;
      f[aa][2] -= dr[2]*fr;
    }

  return e_pot*4.0*epsilon;
}

real_t lj_cell_self(const potential* p, const box* a, real3* f)
{
  real_t sigma = p->lj.sigma, epsilon = p->lj.epsilon, cut = p->cutoff;
  real_t e_pot = 0;
  real_t s6 = sigma*sigma*sigma*sigma*sigma*sigma;
  real_t cut2 = cut*cut;
  real_t cut6 = s6/(cut2*cut2*cut2);
  real_t eshift = POT_SHIFT*cut6*(cut6-1);

  for(u32 aa = 0; aa < a->atoms; ++aa)
    for(u32 bb = aa+1; bb < a->atoms; ++bb) {

      real_t r2; real3 dr;
      dr[0] = a->r[aa][0]-a->r[bb][0];
      dr[1] = a->r[aa][1]-a->r[bb][1];
      dr[2] = a->r[aa][2]-a->r[bb][2];
      r2=dr[0]*dr[0]+dr[1]*dr[1]+dr[2]*dr[2];

      if(r2 > cut2) continue;
      r2 = 1.0/r2;

      real_t r6 = s6 * (r2*r2*r2);
      real_t energy = r6*(r6-1.0)-eshift;
      e_pot += energy;

      real_t fr = -4.0*epsilon*r6*r2*(12.0*r6-6.0);
      f[aa][0] -= dr[0]*fr;
      f[aa][1] -= dr[1]*fr;
      f[aa][2] -= dr[2]*fr;
      f[bb][0] += dr[0]*fr;
      f[bb][1] += dr[1]*fr;
      f[bb][2] += dr[2]*fr;
    }

  return e_pot*4.0*epsilon;
}
