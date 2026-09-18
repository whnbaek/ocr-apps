#ifndef __XSBENCH_HEADER_H__
#define __XSBENCH_HEADER_H__

#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<strings.h>
#include<math.h>

#include<time.h>
#include<sys/time.h>

#ifndef ENABLE_EXTENSION_LABELING
#define ENABLE_EXTENSION_LABELING
#endif

#include "ocr.h"
#include "extensions/ocr-labeling.h" //currently needed for labeled guids
#include "extensions/ocr-affinity.h" //needed for affinity

#include "ocrAppUtils.h"
#include "timers.h"

// I/O Specifiers
#define INFO 1

//0 --> "openmp" static scheduling equivalent
//1 --> dynamic scheduling equivalent
#ifndef SCHEDULER_TYPE
#define SCHEDULER_TYPE 1
#endif

//Chunk size of lookup loop if dynamic scheduling is desired
#ifndef CHUNK_SIZE
#define CHUNK_SIZE 1000
#endif

#ifndef MIN
#define MIN(x,y) ((x)<(y)?(x):(y))
#endif

// Counter-based generation.  A generated value is a pure function of its
// stream key and of the position it occupies in the array being filled, so the
// data is the same whatever order, concurrency or process produces it, and no
// generator state is shared between producers.  The mixer is the one the
// unshared port uses, but the draws are composed differently, so the two
// ports' checksums are not comparable to each other.
static inline u64 xs_mix64( u64 x )
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static inline u64 xs_stream( u64 seed, u64 stream )
{
    return xs_mix64( seed ^ (stream * 0xD1B54A32D192ED03ULL) );
}

static inline u64 xs_draw( u64 key, u64 index )
{
    return xs_mix64( key + index );
}

// Uniform on [0,1) over the 53 bits a double holds exactly.
static inline double xs_draw_unit( u64 key, u64 index )
{
    return (double)(xs_draw(key, index) >> 11) * (1.0 / 9007199254740992.0);
}

// Magnitude-relative quantization: a digest built on it is insensitive to
// differences below the kept mantissa bits and moves on anything larger.
static inline u64 xs_quantize( double v )
{
    int e = 0;
    double m = frexp( v, &e );
    return (u64)( (s64)e * 2097152 + (s64)(m * 1048576.0) );
}

#define XS_DATA_SEED 42

enum xs_stream_id {
    XS_STREAM_GRIDS = 1,
    XS_STREAM_CONCS
};

// Structures
typedef struct{
    double energy;
    double total_xs;
    double elastic_xs;
    double absorbtion_xs;
    double fission_xs;
    double nu_fission_xs;
} NuclideGridPoint;

/* The unionized grid keeps only energies; a point's per-nuclide index row
 * lives in the flat xs_grid at stride n_isotopes.  Storing a pointer to that
 * row here would pin the row's address into shared read-only data, and a DB's
 * mapping is acquire-relative — every reader would have to re-derive and
 * re-write the pointers, a write into read-only-acquired memory. */
typedef struct{
    double energy;
} GridPoint;

/* One lookup chain's private result cell: the last lookup's macroscopic cross
 * sections (kept so the kernel's result cannot be optimized away) and the
 * chain's running digest of every lookup it has performed. */
typedef struct{
    double xs[5];
    u64 digest;
} LaneResult;

typedef struct{
    int nprocs;
    int nthreads;
    long n_isotopes;
    int n_mats;
    long n_gridpoints;
    int lookups;
    char HM[10];
} Inputs;

typedef struct
{
    timer timers[number_of_timers];

    ocrDBK_t DBK_nuclide_grids, DBK_nuclide_grid_ptrs;
    ocrDBK_t DBK_uEnergy_grid;
    ocrDBK_t DBK_xs_grid;
    ocrDBK_t DBK_num_nucs;
    ocrDBK_t DBK_mats_all;
    ocrDBK_t DBK_concs_all;
    //ocrDBK_t DBK_mats[12], DBK_concs[12];
    ocrDBK_t DBK_mat_ptrs, DBK_conc_ptrs;
} rankDataH_t;

typedef struct
{
    ocrTML_t iterationsPerThreadTML, lookUpKernelPerThreadTML;
} rankTemplateH_t;

typedef struct
{
    ocrGuid_t perfTimerReductionRangeGUID;
    ocrEVT_t finalOnceEVT;
} globalOcrParamH_t;

typedef struct
{
    Inputs in;
    globalOcrParamH_t ocrParamH;
} globalParamH_t;

typedef struct
{
    s64 nRanks, myRank;
    globalParamH_t globalParamH;
    Inputs in;
    rankTemplateH_t rankTemplateH;
    ocrDBK_t rpPerfTimerDBK;
    ocrEVT_t rpPerfTimerEVT;
    ocrEVT_t loopCompletionLatchEVT;

    ocrHint_t myEdtAffinityHNT;
    ocrHint_t myDbkAffinityHNT;
} rankH_t;

// Function Prototypes
void logo(int version);
void center_print(const char *s, int width);
void border_print(void);
void fancy_int(long a);

NuclideGridPoint ** gpmatrix(ocrDBK_t *DBK_nuclide_grids, ocrDBK_t *DBK_nuclide_grid_ptrs, size_t m, size_t n);

void gpmatrix_free( NuclideGridPoint ** M );

int NGP_compare( const void * a, const void * b );

void generate_grids( NuclideGridPoint ** nuclide_grids,
                     long n_isotopes, long n_gridpoints, u64 dataSeed );

void sort_nuclide_grids( NuclideGridPoint ** nuclide_grids, long n_isotopes,
                         long n_gridpoints );

GridPoint * generate_energy_grid( ocrDBK_t* DBK_uEnergy_grid, ocrDBK_t* DBK_xs_grid, long n_isotopes, long n_gridpoints,
                                  NuclideGridPoint ** nuclide_grids, int mype, int ** PTR_xs_grid);

void set_grid_ptrs_range( GridPoint * energy_grid, int * xs_rows,
                          NuclideGridPoint ** nuclide_grids,
                          long n_isotopes, long n_gridpoints,
                          u64 ibegin, u64 iend );

int binary_search( NuclideGridPoint * A, double quarry, int n );

void calculate_macro_xs(   double p_energy, int mat, long n_isotopes,
                           long n_gridpoints, int * restrict num_nucs,
                           double ** restrict concs,
                           GridPoint * restrict energy_grid,
                           int * restrict xs_grid,
                           NuclideGridPoint ** restrict nuclide_grids,
                           int ** restrict mats,
                           double * restrict macro_xs_vector );

void calculate_micro_xs(   double p_energy, int nuc, long n_isotopes,
                           long n_gridpoints,
                           int * restrict xs_grid,
                           NuclideGridPoint ** restrict nuclide_grids, int idx,
                           double * restrict xs_vector );
void calculate_micro_xs_new(   double p_energy,
                           long n_gridpoints, int xs_indices_nuc,
                           NuclideGridPoint * restrict PTR_nuclide_data_j,
                           double * restrict xs_vector );

long grid_search( long n, double quarry, GridPoint * A);
long grid_search_double( long n, double quarry, double * A);

int * load_num_nucs( ocrDBK_t* DBK_num_nucs, long n_isotopes);
int ** load_mats( rankDataH_t* PTR_rankDataH, int * num_nucs, long n_isotopes );
double ** load_concs( rankDataH_t* PTR_rankDataH, int * num_nucs, u64 dataSeed );
int pick_mat(u64* seed);
double rn(u64* seed);
void counter_stop( int * eventset, int num_papi_events );
void counter_init( int * eventset, int * num_papi_events );
void do_flops(void);
void do_loads( int nuc,
               NuclideGridPoint ** restrict nuclide_grids,
               long n_gridpoints );
int read_CLI( int argc, char * argv[], Inputs * input );
void print_CLI_error(const char * reason);
double round_double( double input );
size_t estimate_mem_usage( Inputs in );
void print_inputs(Inputs in, int nprocs, int version);
void print_results( Inputs in, int mype, double runtime, int nprocs, u64 checksum );
void binary_dump(long n_isotopes, long n_gridpoints, NuclideGridPoint ** nuclide_grids, GridPoint * energy_grid, int * xs_grid);
void binary_read(long n_isotopes, long n_gridpoints, NuclideGridPoint ** nuclide_grids, GridPoint * energy_grid, int * xs_grid);

void assign_mats_ptrs( int* num_nucs, int* mats_all, int** mats );
void assign_concs_ptrs( int* num_nucs, double* concs_all, double** concs );
void assignNuclideGridPtrs( NuclideGridPoint *full, NuclideGridPoint **M, size_t m, size_t n);

#endif
