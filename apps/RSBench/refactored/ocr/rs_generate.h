#ifndef RS_GENERATE_H
#define RS_GENERATE_H

/* Synthetic input for the resonance data set.
 *
 * Every generated value is a pure function of (stream, index), so whichever
 * task owns a piece of the data can produce it alone, in any order, and the
 * instance is the same however many tasks or ranks produced it.  Two
 * decompositions that share this definition therefore describe the same
 * instance and their result checksums are directly comparable.
 *
 * The including header supplies u64, Pole and Window and the standard string
 * and complex declarations; only the fields named below have to agree. */

#define RS_STREAM_NPOLES     1ULL
#define RS_STREAM_NWINDOWS   2ULL
#define RS_STREAM_POLE       3ULL
#define RS_STREAM_WINDOW     4ULL
#define RS_STREAM_K0RS       5ULL
#define RS_STREAM_CONC       6ULL

/* (outer, inner, field) packed into one index word. */
#define RS_INDEX3(i,j,k) ( ((u64)(i) << 40) | ((u64)(j) << 8) | (u64)(k) )

static inline u64 rs_rand_bits( u64 stream, u64 index )
{
    u64 x = stream * 0xD6E8FEB86659FD93ULL + (index + 1) * 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static inline double rs_rand_unit( u64 stream, u64 index )
{
    return (double)(rs_rand_bits(stream, index) >> 11) * (1.0 / 9007199254740992.0);
}

/* A count array is a histogram of one draw stream over the whole index range:
 * a bin depends on every draw, so it is not a function of the index that reads
 * it.  It is the one part of the data set that a single task has to build
 * before the per-index producers can run. */
static inline void rs_gen_counts( int n, int avg, u64 stream, int *R )
{
    int total = avg * n;

    for( int i = 0; i < n; i++ )
        R[i] = 0;

    for( int i = 0; i < total; i++ )
        R[rs_rand_bits( stream, (u64) i ) % (u64) n]++;

    // Ensure all nuclides have at least 1 resonance
    for( int i = 0; i < n; i++ )
        if( R[i] == 0 )
            R[i] = 1;
}

/* The poles of one nuclide.  The returned fold identifies the generated data:
 * a sum of IEEE-754 bit patterns is exact and order-independent, so it is the
 * same whatever produced the array and in whatever order. */
static inline unsigned long long rs_gen_poles_i( int numL, int i, int n_poles,
                                                 Pole *R )
{
    // Pole Scaling Factor -- Used to bias hitting of the fast Faddeeva
    // region to approximately 99.5% (i.e., only 0.5% of lookups should
    // require the full eval).
    double f = 152.5;

    unsigned long long cksum = 0;

    for( int j = 0; j < n_poles; j++ )
    {
        R[j].MP_EA = f*(rs_rand_unit( RS_STREAM_POLE, RS_INDEX3(i,j,0) )
                        + rs_rand_unit( RS_STREAM_POLE, RS_INDEX3(i,j,1) ) * I);
        R[j].MP_RT = f*rs_rand_unit( RS_STREAM_POLE, RS_INDEX3(i,j,2) )
                        + rs_rand_unit( RS_STREAM_POLE, RS_INDEX3(i,j,3) ) * I;
        R[j].MP_RA = f*rs_rand_unit( RS_STREAM_POLE, RS_INDEX3(i,j,4) )
                        + rs_rand_unit( RS_STREAM_POLE, RS_INDEX3(i,j,5) ) * I;
        R[j].MP_RF = f*rs_rand_unit( RS_STREAM_POLE, RS_INDEX3(i,j,6) )
                        + rs_rand_unit( RS_STREAM_POLE, RS_INDEX3(i,j,7) ) * I;
        R[j].l_value = (short int)( rs_rand_bits( RS_STREAM_POLE, RS_INDEX3(i,j,8) )
                                    % (u64) numL );

        double parts[8] = { creal(R[j].MP_EA), cimag(R[j].MP_EA),
                            creal(R[j].MP_RT), cimag(R[j].MP_RT),
                            creal(R[j].MP_RA), cimag(R[j].MP_RA),
                            creal(R[j].MP_RF), cimag(R[j].MP_RF) };
        for( int k = 0; k < 8; k++ )
        {
            unsigned long long bits;
            memcpy( &bits, &parts[k], sizeof(bits) );
            cksum += bits;
        }
        cksum += (unsigned long long) R[j].l_value;
    }

    return cksum;
}

/* The windows of one nuclide.  The window bounds partition that nuclide's own
 * pole range, so they need its pole count and nothing global. */
static inline void rs_gen_windows_i( int i, int n_windows, int n_poles,
                                     Window *R )
{
    int space = n_poles / n_windows;
    int remainder = n_poles - space * n_windows;
    int ctr = 0;

    for( int j = 0; j < n_windows; j++ )
    {
        R[j].T = rs_rand_unit( RS_STREAM_WINDOW, RS_INDEX3(i,j,0) );
        R[j].A = rs_rand_unit( RS_STREAM_WINDOW, RS_INDEX3(i,j,1) );
        R[j].F = rs_rand_unit( RS_STREAM_WINDOW, RS_INDEX3(i,j,2) );
        R[j].start = ctr;
        R[j].end = ctr + space - 1;

        ctr += space;

        if ( j < remainder )
        {
            ctr++;
            R[j].end++;
        }
    }
}

static inline void rs_gen_k0rs_i( int numL, int i, double *R )
{
    for( int j = 0; j < numL; j++ )
        R[j] = rs_rand_unit( RS_STREAM_K0RS, RS_INDEX3(i,j,0) );
}

/* Material composition.  Material 0 (fuel) is the special case: the table
 * holds the head that both benchmark sizes share, and the large size extends
 * it with consecutive ids above the shared range.  Which size is in use is
 * decided by the nuclide count alone. */
#define RS_FUEL_HEAD       34
#define RS_FUEL_EXT_FIRST  68
#define RS_FUEL_LARGE      321
#define RS_NUCLIDES_SMALL  68

static inline int rs_gen_num_nucs_i( int n_nuclides, int m )
{
    static const int num_nucs[12] = { 0, 5, 4, 4, 27, 21, 21, 21, 21, 21, 9, 9 };

    if( m == 0 )
        return ( n_nuclides == RS_NUCLIDES_SMALL ) ? RS_FUEL_HEAD : RS_FUEL_LARGE;

    return num_nucs[m];
}

/* The id of the j-th nuclide of material m.  Reading the composition through
 * one accessor keeps the fuel extension rule in a single place, whether the
 * caller is filling a material's list or weighing how often a nuclide is
 * read. */
static inline int rs_gen_mat_id( int m, int j )
{
    static const int mats0_head[RS_FUEL_HEAD] =
                   { 58, 59, 60, 61, 40, 42, 43, 44, 45, 46, 1, 2, 3, 7,
                     8, 9, 10, 29, 57, 47, 48, 0, 62, 15, 33, 34, 52, 53,
                     54, 55, 56, 18, 23, 41 }; //fuel
    static const int mats1[] =  { 63, 64, 65, 66, 67 }; // cladding
    static const int mats2[] =  { 24, 41, 4, 5 }; // cold borated water
    static const int mats3[] =  { 24, 41, 4, 5 }; // hot borated water
    static const int mats4[] =  { 19, 20, 21, 22, 35, 36, 37, 38, 39, 25, 27, 28, 29,
                     30, 31, 32, 26, 49, 50, 51, 11, 12, 13, 14, 6, 16,
                     17 }; // RPV
    static const int mats5[] =  { 24, 41, 4, 5, 19, 20, 21, 22, 35, 36, 37, 38, 39, 25,
                     49, 50, 51, 11, 12, 13, 14 }; // lower radial reflector
    static const int mats6[] =  { 24, 41, 4, 5, 19, 20, 21, 22, 35, 36, 37, 38, 39, 25,
                     49, 50, 51, 11, 12, 13, 14 }; // top reflector / plate
    static const int mats7[] =  { 24, 41, 4, 5, 19, 20, 21, 22, 35, 36, 37, 38, 39, 25,
                     49, 50, 51, 11, 12, 13, 14 }; // bottom plate
    static const int mats8[] =  { 24, 41, 4, 5, 19, 20, 21, 22, 35, 36, 37, 38, 39, 25,
                     49, 50, 51, 11, 12, 13, 14 }; // bottom nozzle
    static const int mats9[] =  { 24, 41, 4, 5, 19, 20, 21, 22, 35, 36, 37, 38, 39, 25,
                     49, 50, 51, 11, 12, 13, 14 }; // top nozzle
    static const int mats10[] = { 24, 41, 4, 5, 63, 64, 65, 66, 67 }; // top of FA's
    static const int mats11[] = { 24, 41, 4, 5, 63, 64, 65, 66, 67 }; // bottom FA's

    static const int * const mats[12] = { mats0_head, mats1, mats2, mats3,
                                          mats4, mats5, mats6, mats7,
                                          mats8, mats9, mats10, mats11 };

    if( m == 0 && j >= RS_FUEL_HEAD )
        return RS_FUEL_EXT_FIRST + ( j - RS_FUEL_HEAD );

    return mats[m][j];
}

static inline void rs_gen_mat_i( int m, int num_nucs, int *ids )
{
    for( int j = 0; j < num_nucs; j++ )
        ids[j] = rs_gen_mat_id( m, j );
}

static inline void rs_gen_concs_i( int m, int num_nucs, double *concs )
{
    for( int j = 0; j < num_nucs; j++ )
        concs[j] = rs_rand_unit( RS_STREAM_CONC, RS_INDEX3(m,j,0) );
}

#endif
