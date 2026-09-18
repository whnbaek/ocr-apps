#ifndef __XSBENCH_HEADER_H__
#define __XSBENCH_HEADER_H__

#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<strings.h>
#include<math.h>

// The generator the table is built from is shared with the other ports of this
// program: the tables they build must agree value for value.
#include "../../xs_index_rng.h"

// Fixed-point scale the per-lookup result is quantized to before it enters the
// checksum: the accumulation must be exact integer arithmetic so that it does
// not depend on the order the lookups complete in, and the quantization must
// be coarse enough that a last-bit difference in the floating-point kernel
// (contraction, vectorization) cannot change the pinned value.
#define XS_CHECKSUM_SCALE 1024.0

// Structures
typedef struct{
    double energy;
    double total_xs;
    double elastic_xs;
    double absorbtion_xs;
    double fission_xs;
    double nu_fission_xs;
} NuclideGridPoint;

typedef struct{
    long n_isotopes;
    int n_mats;
    long n_gridpoints;
    int lookups;
    char HM[10];
    // Lookups per sync batch within one place: the batch is a FINISH scope and
    // the next one starts only when it has drained, so this is a place's
    // in-flight width.  It also sets the number of blocks the unionized index
    // plane is cut into, i.e. the width of the parallel alignment phase.
    int batch;
    // Partitions of the lookup range.  Each place owns a contiguous share of
    // the lookups, its own producer and its own replica of the physics table,
    // and is placed at (place % policy-domain count).
    int places;
} Inputs;

// Function Prototypes
void logo(int version);
void center_print(const char *s, int width);
void border_print(void);
void fancy_int(long a);

int NGP_compare( const void * a, const void * b );
int double_compare( const void * a, const void * b );

int binary_search( NuclideGridPoint * A, double quarry, int n );

void calculate_micro_xs_new(   double p_energy,
                           long n_gridpoints, int xs_indices_nuc,
                           NuclideGridPoint * restrict PTR_nuclide_data_j,
                           double * restrict xs_vector );

long grid_search_double( long n, double quarry, double * A);

int pick_mat(unsigned long * seed);
double rn(unsigned long * seed);
int mat_num_nucs( int mat, long n_isotopes );
void load_mat_nuclides( int mat, long n_isotopes, int * nucl_index_list );

Inputs read_CLI( int argc, char * argv[] );
void print_CLI_error(void);
size_t estimate_mem_usage( Inputs in );
void print_inputs(Inputs in, int version);
void print_results( Inputs in, double runtime );

#endif
