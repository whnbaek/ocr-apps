// ************************************************************************
//
// miniAMR: stencil computations with boundary exchange and AMR.
//
// Copyright (2014) Sandia Corporation. Under the terms of Contract
// DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government
// retains certain rights in this software.
//
// Portions Copyright (2016) Intel Corporation.
//
// This library is free software; you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as
// published by the Free Software Foundation; either version 2.1 of the
// License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
// Questions? Contact Courtenay T. Vaughan (ctvaugh@sandia.gov)
//                    Richard F. Barrett (rfbarre@sandia.gov)
//
// ************************************************************************

#ifndef __CHECKSUM_H__
#define __CHECKSUM_H__

typedef struct Checksum_t Checksum_t;

#include "commhdr.h"
#include "control.h"

// The per-variable sums are conserved by the refinement arithmetic -- a cell's value is divided among its eight children and summed back on coarsening -- which is what makes them a usable
// invariant to check a run against, and what makes them blind to the mesh's shape.  weightedSum weights each value by its position within its own block, so it does change when a block is split or
// joined, and it is therefore the reduction that witnesses the state the mesh actually reached.  It is aggregated alongside the sums and is never compared against the reference.

typedef struct Checksum_t {
   DbCommHeader_t  dbCommHeader;
   double      weightedSum;
   double      sum[0];
} Checksum_t;

#define sizeof_Checksum_t (sizeof(Checksum_t) + (control->num_vars * sizeof(double)))

#endif // __CHECKSUM_H__
