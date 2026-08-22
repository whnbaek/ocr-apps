// Face slabs: the ghost-exchange payload of the distributed decomposition.
//
// Each box owns six slabs of box_dim^2 doubles, one per interior boundary
// plane.  A pack task copies one vector's six planes from its box into its
// own slabs; an unpack task fills its box's ghost layer from its
// neighbours' slabs.  Edge and corner ghosts of the 26-neighbour form read
// the boundary lines and points a full face slab already contains.  The
// spine orders pack before unpack; which vector moves is the spawner's
// choice, passed as the vector's byte offset.
#include <ocr.h>
#include <math.h>

#include "hpgmg.h"
#include "utils.h"
#include "mg_dist.h"

// The reserved tail of the level datablock: [boxes: N guids][b_norms: N
// doubles][faces: 6N guids].  The offsets are derived, not stored, so the
// level struct itself is unchanged.
ocrGuid_t* dist_face_guids(level_type *l) {
  return (ocrGuid_t*)(((char*)l) + l->b_norms + l->num_boxes*sizeof(double));
}

ocrGuid_t get_face_guid(level_type *l, int box_id, int face) {
  return dist_face_guids(l)[box_id*6 + face];
}

// Slab layout: face 0/1 fix i (slab[j + k*dim]), 2/3 fix j (slab[i +
// k*dim]), 4/5 fix k (slab[i + j*dim]).  Even face = low side (index 0),
// odd face = high side (index dim-1) of the interior; x points at the
// interior origin (ghost offset already applied).
static void pack_face(level_type *l, const double *x, double *slab, int face) {
  int dim = l->box_dim, jS = l->jStride, kS = l->kStride;
  int a, c;
  int fixed = (face & 1) ? dim - 1 : 0;
  for (c = 0; c < dim; c++)
    for (a = 0; a < dim; a++) {
      int ijk;
      if (face < 2)       ijk = fixed + a*jS + c*kS;   // a=j, c=k
      else if (face < 4)  ijk = a + fixed*jS + c*kS;   // a=i, c=k
      else                ijk = a + c*jS + fixed*kS;   // a=i, c=j
      slab[a + c*dim] = x[ijk];
    }
}

// The neighbour offset picks the slab: the first nonzero axis is the slab
// axis, and the side is the one facing this box (a neighbour at +i shows
// its low-i face).  The remaining offsets index inside the slab — a
// boundary line or point lies within the full face.
int dist_slab_face(int fi, int fj, int fk) {
  if (fi) return (fi == 1) ? 0 : 1;
  if (fj) return (fj == 1) ? 2 : 3;
  return (fk == 1) ? 4 : 5;
}

// Fill this box's ghost cells for neighbour offset (fi,fj,fk) from that
// neighbour's slab.  Mirrors populate_boundary/update_boundary_all: the
// ghost index runs over the free axes at -1/dim, the slab supplies the
// neighbour's facing interior plane.
static void unpack_offset(level_type *l, double *x, const double *slab,
                          int fi, int fj, int fk) {
  int dim = l->box_dim, jS = l->jStride, kS = l->kStride;
  int gi = (fi == 1) ? dim : -1;
  int gj = (fj == 1) ? dim : -1;
  int gk = (fk == 1) ? dim : -1;
  // neighbour-local index of the plane it shows us, per axis
  int nj = (fj == 1) ? 0 : dim - 1;
  int nk = (fk == 1) ? 0 : dim - 1;
  int a, c;

  if (fi && !fj && !fk) {              // face: free axes j,k; slab[j+k*dim]
    for (c = 0; c < dim; c++)
      for (a = 0; a < dim; a++)
        x[gi + a*jS + c*kS] = slab[a + c*dim];
  } else if (!fi && fj && !fk) {       // slab[i + k*dim]
    for (c = 0; c < dim; c++)
      for (a = 0; a < dim; a++)
        x[a + gj*jS + c*kS] = slab[a + c*dim];
  } else if (!fi && !fj && fk) {       // slab[i + j*dim]
    for (c = 0; c < dim; c++)
      for (a = 0; a < dim; a++)
        x[a + c*jS + gk*kS] = slab[a + c*dim];
  } else if (fi && fj && !fk) {        // edge along k; i-axis slab [j+k*dim]
    for (a = 0; a < dim; a++)
      x[gi + gj*jS + a*kS] = slab[nj + a*dim];
  } else if (fi && !fj && fk) {        // edge along j; i-axis slab [j+k*dim]
    for (a = 0; a < dim; a++)
      x[gi + a*jS + gk*kS] = slab[a + nk*dim];
  } else if (!fi && fj && fk) {        // edge along i; j-axis slab [i+k*dim]
    for (a = 0; a < dim; a++)
      x[a + gj*jS + gk*kS] = slab[a + nk*dim];
  } else {                             // corner; i-axis slab [j+k*dim]
    x[gi + gj*jS + gk*kS] = slab[nj + nk*dim];
  }
}

// deps: level - box - own 6 slabs (RW); pars: vector offset
ocrGuid_t pack_edt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  level_type *l = (level_type*) depv[0].ptr;
  box_type *b = (box_type*) depv[1].ptr;
  double *x = (double*)((char*)b + paramv[0])
              + NUM_GHOSTS * (1 + l->jStride + l->kStride);
  int f;
  for (f = 0; f < 6; f++)
    pack_face(l, x, (double*) depv[2+f].ptr, f);
  return NULL_GUID;
}

// deps: level - box (RW) - 6 or 26 neighbour slabs (count = depc-2)
// pars: vector offset - presence bitmask (an absent neighbour's slot
// carries the box's own slab for that direction, never read)
ocrGuid_t unpack_edt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  level_type *l = (level_type*) depv[0].ptr;
  box_type *b = (box_type*) depv[1].ptr;
  u64 mask = paramv[1];
  int nnb = (int) depc - 2;
  double *x = (double*)((char*)b + paramv[0])
              + NUM_GHOSTS * (1 + l->jStride + l->kStride);

  if (nnb == 6) {
#ifndef STENCIL_FUSE_BC
    apply_bcs(l, b, x);
#endif
    static const int off6[6][3] =
      {{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1}};
    int i;
    for (i = 0; i < 6; i++) {
      if (!(mask & (1ull << i))) continue;
      unpack_offset(l, x, (const double*) depv[2+i].ptr,
                    off6[i][0], off6[i][1], off6[i][2]);
    }
  } else {
    // get_neighbors_all order: i outermost, then j, then k, self skipped
    int fi, fj, fk, n = 0;
    for (fi = -1; fi <= 1; fi++)
      for (fj = -1; fj <= 1; fj++)
        for (fk = -1; fk <= 1; fk++) {
          if (!fi && !fj && !fk) continue;
          if (mask & (1ull << n))
            unpack_offset(l, x, (const double*) depv[2+n].ptr, fi, fj, fk);
          n++;
        }
  }
  return NULL_GUID;
}
