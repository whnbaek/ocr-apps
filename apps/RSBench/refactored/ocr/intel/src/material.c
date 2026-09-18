#include "rsbench.h"

// num_nucs represents the number of nuclides that each material contains
int load_num_nucs_i( Inputs input, int i )
{
    return rs_gen_num_nucs_i( input.n_nuclides, i );
}

void load_num_nucs( Inputs input, int* num_nucs )
{
    for( int i = 0; i < input.n_mats; i++ )
        num_nucs[i] = rs_gen_num_nucs_i( input.n_nuclides, i );

    return;
}

// Assigns the array of nuclide ID's of one material
void load_mat_i( int i, int num_nucs, int *ids )
{
    rs_gen_mat_i( i, num_nucs, ids );
}

// The 'concentrations' of the nuclides in one material
void load_concs_i( int i, int num_nucs, double *concs )
{
    rs_gen_concs_i( i, num_nucs, concs );
}

/* Fraction of core volume per material: the probability that a lookup draws
 * it, and so the expected rate at which every nuclide it contains is read. */
static const double mat_dist[12] = { 0.140,   // fuel
                                     0.052,   // cladding
                                     0.275,   // cold, borated water
                                     0.134,   // hot, borated water
                                     0.154,   // RPV
                                     0.064,   // Lower, radial reflector
                                     0.066,   // Upper reflector / top plate
                                     0.055,   // bottom plate
                                     0.008,   // bottom nozzle
                                     0.015,   // top nozzle
                                     0.025,   // top of fuel assemblies
                                     0.013 }; // bottom of fuel assemblies

// picks a material based on a probabilistic distribution
int pick_mat( unsigned long * seed )
{
    // I have a nice spreadsheet supporting these numbers. They are
    // the fractions (by volume) of material in the core. Not a
    // *perfect* approximation of where XS lookups are going to occur,
    // but this will do a good job of biasing the system nonetheless.

    // Also could be argued that doing fractions by weight would be
    // a better approximation, but volume does a good enough job for now.

    double roll = rn(seed);

    // makes a pick based on the distro
    for( int i = 0; i < 12; i++ )
    {
        double running = 0;
        for( int j = i; j > 0; j-- )
            running += mat_dist[j];
        if( roll < running )
            return i;
    }

    return 0;
}

#ifdef OCR_APP_OPTIMIZED_PLACEMENT
/* Highest nuclide ID the composition tables can name. */
#define RS_NUCLIDE_ID_LIMIT 355

/* Reads one lookup draws from the objects whose home index is fixed: the ten
 * handle/index blocks at indices 0..9 (a lookup's three tasks acquire each of
 * them between one and three times), and the two tables of the material it
 * draws, at that material's own index.  They are the same order of magnitude
 * as the whole nuclide set's reads, so a map that balances only the nuclides
 * piles them onto ranks that are already serving. */
static const double handle_reads[10] = { 3.0, 3.0, 2.0, 2.0, 2.0,
                                         1.0, 1.0, 1.0, 3.0, 2.0 };
#define RS_MAT_TABLE_READS 4.0
#define RS_NUCLIDE_BLOCKS  3.0

/* Expected acquires of one nuclide per lookup: the volume fraction of every
 * material that contains it. */
static void nuclide_read_weights( Inputs input, const int * num_nucs, double * w )
{
    for( int j = 0; j < RS_NUCLIDE_ID_LIMIT; j++ )
        w[j] = 0.0;

    for( int m = 0; m < input.n_mats; m++ )
        for( int j = 0; j < num_nucs[m]; j++ )
        {
            int id = rs_gen_mat_id( m, j );
            if( id < RS_NUCLIDE_ID_LIMIT )
                w[id] += mat_dist[m];
        }
}

/* Home rank for a nuclide's three blocks.  One datablock is served by one
 * rank, so the quantity to balance is expected reads per rank, not objects
 * per rank: a nuclide's read rate is the sum of the fractions of the
 * materials that hold it and spreads about fivefold, and the fixed-index
 * objects above add a whole rank's worth of reads to each of the first few
 * ranks.  Placing the heaviest nuclide first on the rank serving least so far
 * -- starting from the load the fixed-index objects already impose -- is the
 * standard greedy for that, and it leaves a rank holding a heavy object
 * correspondingly fewer nuclides.
 *
 * The map is a pure function of the composition and the rank count, so it is
 * built once and cached.  The cache carries no synchronisation: its
 * precondition is that a single task decides the placement of the whole data
 * set, which is the same task that creates the blocks and names their
 * homes. */
u64 mcNuclideSpreadIndex( Inputs input, int nuclide )
{
    static int builtNuclides = 0;
    static u64 builtRanks = 0;
    static u64 home[RS_NUCLIDE_ID_LIMIT];

    u64 pdCount;
    ocrAffinityCount( AFFINITY_PD, &pdCount );

    /* With more ranks than the tables can name, or for an ID no material
     * names (it is never read), the plain round-robin is already right. */
    if( pdCount <= 1 || pdCount > (u64) RS_NUCLIDE_ID_LIMIT
        || nuclide >= RS_NUCLIDE_ID_LIMIT )
        return (u64) nuclide;

    if( builtNuclides != input.n_nuclides || builtRanks != pdCount )
    {
        int P = (int) pdCount;
        int num_nucs[12];
        double w[RS_NUCLIDE_ID_LIMIT];
        double load[RS_NUCLIDE_ID_LIMIT];
        int placed[RS_NUCLIDE_ID_LIMIT];

        load_num_nucs( input, num_nucs );
        nuclide_read_weights( input, num_nucs, w );

        int n = ( input.n_nuclides < RS_NUCLIDE_ID_LIMIT )
                ? input.n_nuclides : RS_NUCLIDE_ID_LIMIT;

        for( int r = 0; r < P; r++ )
            load[r] = 0.0;
        for( int k = 0; k < 10; k++ )
            load[k % P] += handle_reads[k];
        for( int m = 0; m < input.n_mats; m++ )
            load[( 10 + input.n_nuclides + m ) % P] += RS_MAT_TABLE_READS * mat_dist[m];

        for( int j = 0; j < RS_NUCLIDE_ID_LIMIT; j++ )
            placed[j] = 0;

        for( int step = 0; step < n; step++ )
        {
            int heaviest = -1;
            for( int j = 0; j < n; j++ )
                if( !placed[j] && ( heaviest < 0 || w[j] > w[heaviest] ) )
                    heaviest = j;

            int rank = 0;
            for( int r = 1; r < P; r++ )
                if( load[r] < load[rank] )
                    rank = r;

            load[rank] += RS_NUCLIDE_BLOCKS * w[heaviest];
            placed[heaviest] = 1;
            home[heaviest] = (u64) rank;
        }

        for( int j = n; j < RS_NUCLIDE_ID_LIMIT; j++ )
            home[j] = (u64) j;

        builtNuclides = input.n_nuclides;
        builtRanks = pdCount;
    }

    return home[nuclide];
}
#endif
