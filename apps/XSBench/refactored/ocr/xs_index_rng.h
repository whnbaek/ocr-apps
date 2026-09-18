#ifndef __XS_INDEX_RNG_H__
#define __XS_INDEX_RNG_H__

#include <string.h>

/* Index-seeded generation of the synthetic cross-section data.
 *
 * A generated value is a pure function of its stream and of the position it
 * occupies in the array being filled: no state is carried from one element to
 * the next, so an element can be produced by whichever task owns the block it
 * lives in, in any order, in any number of processes, and the resulting table
 * is the same.  Ports that include this header therefore build bit-identical
 * tables from the same arguments, which is what makes a checksum over the
 * table comparable between them; a port that copies these definitions instead
 * of including them must keep the copy identical or that comparability is
 * lost.
 */

/* Distinct streams keep independently generated arrays from correlating. */
#define XS_STREAM_GRID 226ULL
#define XS_STREAM_CONC 227ULL

static inline unsigned long long xs_mix64( unsigned long long z )
{
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Uniform on [0,1) over the 53 bits a double holds exactly. */
static inline double rn_indexed( unsigned long long stream, unsigned long long index )
{
    unsigned long long bits = xs_mix64( xs_mix64( stream ) ^ xs_mix64( index ) );
    return (double) (bits >> 11) * (1.0 / 9007199254740992.0);
}

/* Position of one generated field in the grid stream.  The index space is the
 * flattened (nuclide, gridpoint, field) triple, so a nuclide's grid does not
 * depend on how many nuclides are generated beside it, nor on which task
 * generates them. */
static inline unsigned long long xs_grid_index( long nuclide, long gridpoint,
                                                long n_gridpoints, int field )
{
    return ( (unsigned long long) nuclide * (unsigned long long) n_gridpoints
             + (unsigned long long) gridpoint ) * 6ULL + (unsigned long long) field;
}

/* Position of one generated concentration in the concentration stream.  The
 * material and element indices occupy disjoint halves of the index word, so a
 * material's list depends on neither the length nor the order of any other
 * material's. */
static inline unsigned long long xs_conc_index( long material, long element )
{
    return ( (unsigned long long) material << 32 ) ^ (unsigned long long) element;
}

/* Sum of the IEEE-754 bit patterns of an array of doubles.  The arithmetic is
 * exact and commutative, so the value is a function of the multiset of values
 * alone -- independent of the order they were produced in, of the order they
 * are stored in, and of how the array was partitioned. */
static inline unsigned long long xs_grid_checksum( const double * a, long n )
{
    unsigned long long sum = 0;
    long i;
    for( i = 0; i < n; i++ )
    {
        unsigned long long bits;
        memcpy( &bits, &a[i], sizeof(bits) );
        sum += bits;
    }
    return sum;
}

#endif
