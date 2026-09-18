/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

/* The symmetric positive-definite instance a tiled Cholesky factors when no
 * input file is given.
 *
 * A = D + v v^T, with the vector entry v_i and the diagonal shift d_i pure
 * functions of the GLOBAL row index.  A rank-one term is positive
 * semi-definite for any v and D is strictly positive, so the matrix is
 * positive definite by construction and no candidate has to be tested; its
 * condition number grows with |v|^2, which is linear in the matrix size.  No
 * entry is zero, so every element of the factor depends on every element of
 * its own row and column and a scalar taken from the factor's diagonal is a
 * function of the whole computation.
 *
 * An entry depends on its two indices alone: a tile can be produced wherever
 * it is owned, under any partition and any number of ranks, with no stream, no
 * shared generator state and no ordering between producers.
 *
 * Two programs factoring the same instance must start from identical bits, so
 * an entry has ONE definition, evaluated as one multiply and one add in one
 * expression, and a tile is filled by one shared loop.
 */

#ifndef CHOLESKY_INSTANCE_H
#define CHOLESKY_INSTANCE_H

#include <stdint.h>

/* An integer hash whose output bits do not follow the bits the index varies
 * in, so consecutive indices give unrelated values and nothing has to be
 * carried from one index to the next. */
static inline uint64_t choleskyMix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

/* The top 53 bits are the mantissa of a double in [0,1); the scale is a power
 * of two, so the conversion is exact. */
static inline double choleskyUnit(uint64_t h) {
    return (double)(h >> 11) * (1.0 / 9007199254740992.0);
}

/* The shift is drawn from a second point of the hash so that it is independent
 * of the vector entry at the same index. */
#define CHOLESKY_SHIFT_KEY 0x632BE59BD9B4E019ULL

/* v_i in [-1,1). */
static inline double choleskyVec(uint64_t i) {
    return 2.0 * choleskyUnit(choleskyMix(i)) - 1.0;
}

/* d_i in [1,2): bounded away from zero, so the smallest eigenvalue is too. */
static inline double choleskyShift(uint64_t i) {
    return 1.0 + choleskyUnit(choleskyMix(i ^ CHOLESKY_SHIFT_KEY));
}

/* a_ij = v_i v_j off the diagonal, d_i + v_i^2 on it. */
static inline double choleskyEntry(uint64_t i, uint64_t j) {
    double shift = (i == j) ? choleskyShift(i) : 0.0;
    return choleskyVec(i) * choleskyVec(j) + shift;
}

/* One tile of the lower triangle, row-major with leading dimension ts: tile
 * (row,col) holds the elements whose global indices are
 * (row*ts + r, col*ts + c).  This is the layout the tile-stream input carries,
 * so a generated tile and a read tile are interchangeable. */
static inline void choleskyFillTile(double *tile, uint64_t row, uint64_t col,
                                    uint64_t ts) {
    uint64_t r, c, gi, gj;
    for (r = 0; r < ts; ++r) {
        gi = row * ts + r;
        for (c = 0; c < ts; ++c) {
            gj = col * ts + c;
            tile[r * ts + c] = choleskyEntry(gi, gj);
        }
    }
}

#endif /* CHOLESKY_INSTANCE_H */
