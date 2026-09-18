#include <stdlib.h>
#include <ocr.h>

#include "amr.h"

/* A block's cells are [var][x][y][z] over the interior plus a one-cell shell,
 * exactly as the published port lays them out — the stencil, the checksum and
 * the strips all index them through these three strides. */
typedef struct
{
  u64 var;
  u64 x;
  u64 y;
} stride;

static inline void strides(const control* c, stride* s)
{
  s->y = 1;
  s->x = (u64)(c->nz + 2);
  s->var = (u64)(c->ny + 2)*(c->nz + 2);
}

static inline u64 cell_at(const control* c, const stride* s, s32 v, s32 i, s32 j, s32 k)
{
  return (u64)v*(c->nx + 2)*s->var + (u64)i*s->var + (u64)j*s->x + (u64)k;
}

/* The whole block, shell included, is written before the interior, so no cell
 * of a fresh payload is ever read undefined; the interior then takes the
 * position-seeded fill that makes the golden checksum a function of the mesh
 * geometry and the variable count alone. */
void cells_init(const control* c, const u32 pos[3], double* cells)
{
  stride s; strides(c, &s);
  s64 l = 0;
  for(s32 var = 0; var < c->num_vars; ++var) {
    for(s32 i = 0; i <= c->nx+1; ++i)
      for(s32 j = 0; j <= c->ny+1; ++j)
        for(s32 k = 0; k <= c->nz+1; ++k)
          cells[cell_at(c, &s, var, i, j, k)] = (double)(999900 + l++);
    for(s32 i = 1; i <= c->nx; ++i)
      for(s32 j = 1; j <= c->ny; ++j)
        for(s32 k = 1; k <= c->nz; ++k)
          cells[cell_at(c, &s, var, i, j, k)] =
            (double)(((s32)pos[0]*2 + i)*100 + ((s32)pos[1]*2 + j)*10 + ((s32)pos[2]*4 + k));
  }
}

/* One level of refinement: the child takes its own eighth of the parent and
 * spreads each parent cell over the eight cells that replace it, scaled so
 * that the plain per-variable sum is conserved exactly in binary floating
 * point.  The shell is filled first because the halo of a block that has just
 * changed level carries nothing until the next exchange. */
void cells_upsample(const control* c, u32 prong, const double* parent, double* cells)
{
  stride s; strides(c, &s);
  s32 xs = (c->nx >> 1)*(s32)(prong & 1);
  s32 ys = (c->ny >> 1)*(s32)((prong >> 1) & 1);
  s32 zs = (c->nz >> 1)*(s32)((prong >> 2) & 1);

  for(s32 var = 0; var < c->num_vars; ++var) {
    for(s32 i = 0; i <= c->nx+1; ++i)
      for(s32 j = 0; j <= c->ny+1; ++j)
        for(s32 k = 0; k <= c->nz+1; ++k)
          cells[cell_at(c, &s, var, i, j, k)] = -8888.00;
    for(s32 x = 1; x <= c->nx >> 1; ++x)
      for(s32 y = 1; y <= c->ny >> 1; ++y)
        for(s32 z = 1; z <= c->nz >> 1; ++z) {
          double v = parent[cell_at(c, &s, var, xs+x, ys+y, zs+z)]*0.125;
          for(s32 a = 0; a < 2; ++a)
            for(s32 b = 0; b < 2; ++b)
              for(s32 d = 0; d < 2; ++d)
                cells[cell_at(c, &s, var, x+x-1+a, y+y-1+b, z+z-1+d)] = v;
        }
  }
}

/// Put a fresh payload in a defined state before only part of it is written.
void cells_blank(const control* c, double* cells)
{
  stride s; strides(c, &s);
  for(s32 var = 0; var < c->num_vars; ++var)
    for(s32 i = 0; i <= c->nx+1; ++i)
      for(s32 j = 0; j <= c->ny+1; ++j)
        for(s32 k = 0; k <= c->nz+1; ++k)
          cells[cell_at(c, &s, var, i, j, k)] = -8888.00;
}

/* The reverse: one eighth of the coarser block is the 2x2x2 sum of one of the
 * blocks that vanish.  Which eighth is the published port's mapping, in which
 * the octant a prong lands in reads its position bits in the opposite order
 * to the one the split used — the plain sum is conserved either way, but the
 * weighted reduction sees the difference, so it is the mapping the mesh is
 * defined by rather than a detail. */
void cells_merge(const control* c, u32 prong, const double* child, double* cells)
{
  stride s; strides(c, &s);
  s32 xs = (c->nx >> 1)*(s32)((prong >> 2) & 1);
  s32 ys = (c->ny >> 1)*(s32)((prong >> 1) & 1);
  s32 zs = (c->nz >> 1)*(s32)(prong & 1);

  for(s32 var = 0; var < c->num_vars; ++var)
    for(s32 x = 1; x <= c->nx >> 1; ++x)
      for(s32 y = 1; y <= c->ny >> 1; ++y)
        for(s32 z = 1; z <= c->nz >> 1; ++z) {
          double v = 0.0;
          for(s32 a = 0; a < 2; ++a)
            for(s32 b = 0; b < 2; ++b)
              for(s32 d = 0; d < 2; ++d)
                v += child[cell_at(c, &s, var, x+x-1+a, y+y-1+b, z+z-1+d)];
          cells[cell_at(c, &s, var, xs+x, ys+y, zs+z)] = v;
        }
}

void stencil_calc(const control* c, double* cells, s32 var)
{
  stride s; strides(c, &s);
  u64 n = (u64)(c->nx+2)*(c->ny+2)*(c->nz+2);
  double* work = (double*)malloc((size_t)n*sizeof(double));

  if(c->stencil == 7) {
    for(s32 i = 1; i <= c->nx; ++i)
      for(s32 j = 1; j <= c->ny; ++j)
        for(s32 k = 1; k <= c->nz; ++k) {
          u64 o = cell_at(c, &s, var, i, j, k);
          u64 w = cell_at(c, &s, 0, i, j, k);
          work[w] = (cells[o - s.var] + cells[o - s.x] + cells[o - 1] + cells[o]
                     + cells[o + 1] + cells[o + s.x] + cells[o + s.var])/7.0;
        }
  } else {
    for(s32 i = 1; i <= c->nx; ++i)
      for(s32 j = 1; j <= c->ny; ++j)
        for(s32 k = 1; k <= c->nz; ++k) {
          double sb = 0.0, sm = 0.0, sf = 0.0;
          for(s32 b = -1; b <= 1; ++b)
            for(s32 d = -1; d <= 1; ++d) {
              sb += cells[cell_at(c, &s, var, i-1, j+b, k+d)];
              sm += cells[cell_at(c, &s, var, i,   j+b, k+d)];
              sf += cells[cell_at(c, &s, var, i+1, j+b, k+d)];
            }
          work[cell_at(c, &s, 0, i, j, k)] = (sb + sm + sf)/27.0;
        }
  }
  for(s32 i = 1; i <= c->nx; ++i)
    for(s32 j = 1; j <= c->ny; ++j)
      for(s32 k = 1; k <= c->nz; ++k)
        cells[cell_at(c, &s, var, i, j, k)] = work[cell_at(c, &s, 0, i, j, k)];
  free(work);
}

/* The two reductions the run reports.  `weighted` runs one cell counter across
 * every variable of the block, so it is a function of where the cell sits in
 * the block it is in — which is exactly what a split or a join changes and
 * what makes it the reduction that sees the mesh. */
void checksum_block(const control* c, const double* cells, chksum* out)
{
  stride s; strides(c, &s);
  double weighted = 0.0;
  u64 index = 0;

  for(s32 var = 0; var < c->num_vars; ++var) {
    double sum = 0.0;
    for(s32 i = 1; i <= c->nx; ++i)
      for(s32 j = 1; j <= c->ny; ++j)
        for(s32 k = 1; k <= c->nz; ++k) {
          double v = cells[cell_at(c, &s, var, i, j, k)];
          sum += v;
          weighted += v*(double)++index;
        }
    out->sum[var] = sum;
  }
  out->weighted = weighted;
}

/* ---- faces ------------------------------------------------------------ */

/* The plane a face names and how it is walked.  Rows and columns are the two
 * block axes the face spans, in the order the strips are laid out; a face's
 * own plane is the outermost interior one, and its shell plane the one
 * outside it. */
typedef struct
{
  u64 plane;
  u64 row;
  u64 col;
  s32 rows;
  s32 cols;
  s32 blk;
  s32 halo;
} facegeom;

static void face_geom(const control* c, u32 f, facegeom* g)
{
  stride s; strides(c, &s);
  u32 axis = f >> 1, pole = f & 1;
  s32 count;
  if(axis == 0)      { count = c->nx; g->plane = s.var; g->row = s.x; g->col = 1;
                       g->rows = c->ny; g->cols = c->nz; }
  else if(axis == 1) { count = c->ny; g->plane = s.x;   g->row = s.var; g->col = 1;
                       g->rows = c->nx; g->cols = c->nz; }
  else               { count = c->nz; g->plane = 1;     g->row = s.var; g->col = s.x;
                       g->rows = c->nx; g->cols = c->ny; }
  g->blk  = pole ? count : 1;
  g->halo = pole ? count + 1 : 0;
}

static inline u64 face_cell(const control* c, const facegeom* g, s32 var, s32 plane,
                            s32 row, s32 col)
{
  return (u64)var*(c->nx + 2)*(u64)(c->ny + 2)*(c->nz + 2)
       + (u64)plane*g->plane + (u64)row*g->row + (u64)col*g->col;
}

/* Fill the strips a face sends.  A neighbour at the same level takes the whole
 * face; a coarser one takes the face summed in 2x2 groups, which is a quarter
 * of the buffer; four finer ones take a quarter each, scaled so that the sum
 * a coarser block would have read is preserved.  All three fit the same
 * buffer, so a strip datablock never has to be resized as the mesh moves. */
void face_pack(const control* c, const blk* b, u32 f, const double* cells, double* strip)
{
  facegeom g; face_geom(c, f, &g);
  s32 hr = g.rows >> 1, hc = g.cols >> 1;
  double* p = strip;

  if(b->nbr[f] == b->level) {
    for(s32 var = 0; var < c->num_vars; ++var)
      for(s32 r = 0; r < g.rows; ++r)
        for(s32 col = 0; col < g.cols; ++col)
          *p++ = cells[face_cell(c, &g, var, g.blk, 1+r, 1+col)];
  } else if(b->nbr[f] < b->level) {
    for(s32 var = 0; var < c->num_vars; ++var)
      for(s32 r = 0; r < hr; ++r)
        for(s32 col = 0; col < hc; ++col)
          *p++ = cells[face_cell(c, &g, var, g.blk, 1+2*r,   1+2*col)]
               + cells[face_cell(c, &g, var, g.blk, 1+2*r,   2+2*col)]
               + cells[face_cell(c, &g, var, g.blk, 2+2*r,   1+2*col)]
               + cells[face_cell(c, &g, var, g.blk, 2+2*r,   2+2*col)];
  } else {
    for(s32 lr = 0; lr < 2; ++lr)
      for(s32 ud = 0; ud < 2; ++ud) {
        p = strip + (u64)(lr*2 + ud)*c->num_vars*hr*hc;
        for(s32 var = 0; var < c->num_vars; ++var)
          for(s32 r = 0; r < hr; ++r)
            for(s32 col = 0; col < hc; ++col)
              *p++ = cells[face_cell(c, &g, var, g.blk, 1 + ud*hr + r, 1 + lr*hc + col)]*0.25;
      }
  }
}

/* Lay one incoming strip into the shell plane of a face.  `slot` is which of
 * a finer neighbour's four quarters this is; for a coarser neighbour it is
 * unused, because the quarter to take out of its buffer follows from this
 * block's own position bits. */
void face_unpack(const control* c, const blk* b, u32 f, u32 slot, const double* strip,
                 double* cells)
{
  facegeom g; face_geom(c, f, &g);
  s32 hr = g.rows >> 1, hc = g.cols >> 1;

  if(b->nbr[f] == b->level) {
    const double* p = strip;
    for(s32 var = 0; var < c->num_vars; ++var)
      for(s32 r = 0; r < g.rows; ++r)
        for(s32 col = 0; col < g.cols; ++col)
          cells[face_cell(c, &g, var, g.halo, 1+r, 1+col)] = *p++;
  } else if(b->nbr[f] < b->level) {
    u32 q = face_quarter(f, b->pos);
    const double* p = strip + (u64)q*c->num_vars*hr*hc;
    for(s32 var = 0; var < c->num_vars; ++var)
      for(s32 r = 0; r < hr; ++r)
        for(s32 col = 0; col < hc; ++col) {
          double v = *p++;
          cells[face_cell(c, &g, var, g.halo, 1+2*r, 1+2*col)] = v;
          cells[face_cell(c, &g, var, g.halo, 1+2*r, 2+2*col)] = v;
          cells[face_cell(c, &g, var, g.halo, 2+2*r, 1+2*col)] = v;
          cells[face_cell(c, &g, var, g.halo, 2+2*r, 2+2*col)] = v;
        }
  } else {
    s32 lr = (s32)(slot >> 1), ud = (s32)(slot & 1);
    const double* p = strip;
    for(s32 var = 0; var < c->num_vars; ++var)
      for(s32 r = 0; r < hr; ++r)
        for(s32 col = 0; col < hc; ++col)
          cells[face_cell(c, &g, var, g.halo, 1 + ud*hr + r, 1 + lr*hc + col)] = *p++;
  }
}

/// A face at the edge of the mesh reflects its own outermost interior plane.
void face_edge(const control* c, u32 f, double* cells)
{
  facegeom g; face_geom(c, f, &g);
  for(s32 var = 0; var < c->num_vars; ++var)
    for(s32 r = 0; r < g.rows; ++r)
      for(s32 col = 0; col < g.cols; ++col)
        cells[face_cell(c, &g, var, g.halo, 1+r, 1+col)] =
          cells[face_cell(c, &g, var, g.blk, 1+r, 1+col)];
}
