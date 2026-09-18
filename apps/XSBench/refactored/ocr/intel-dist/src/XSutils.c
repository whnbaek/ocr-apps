#include "XSbench_header.h"

// Compare function for two grid points. Used for sorting during init
int NGP_compare( const void * a, const void * b )
{
    NuclideGridPoint *i, *j;

    i = (NuclideGridPoint *) a;
    j = (NuclideGridPoint *) b;

    if( i->energy > j->energy )
        return 1;
    else if ( i->energy < j->energy)
        return -1;
    else
        return 0;
}

int double_compare( const void * a, const void * b )
{
    double i = *(const double *) a;
    double j = *(const double *) b;

    if( i > j )
        return 1;
    else if( i < j )
        return -1;
    else
        return 0;
}

// Binary Search function for nuclide grid
// Returns ptr to energy less than the quarry that is closest to the quarry
int binary_search( NuclideGridPoint * A, double quarry, int n )
{
    int min = 0;
    int max = n-1;
    int mid;

    // checks to ensure we're not reading off the end of the grid
    if( A[0].energy > quarry )
        return 0;
    else if( A[n-1].energy < quarry )
        return n-2;

    // Begins binary search
    while( max >= min )
    {
        mid = min + floor( (max-min) / 2.0);
        if( A[mid].energy < quarry )
            min = mid+1;
        else if( A[mid].energy > quarry )
            max = mid-1;
        else
            return mid;
    }
    return max;
}

// Park & Miller Multiplicative Conguential Algorithm
// From "Numerical Recipes" Second Edition
double rn(unsigned long * seed)
{
    double ret;
    unsigned long n1;
    unsigned long a = 16807;
    unsigned long m = 2147483647;
    n1 = ( a * (*seed) ) % m;
    *seed = n1;
    ret = (double) n1 / m;
    return ret;
}

size_t estimate_mem_usage( Inputs in )
{
    size_t U                   = (size_t) in.n_isotopes * (size_t) in.n_gridpoints;
    size_t all_nuclide_grids   = U * sizeof( NuclideGridPoint );
    size_t size_UEG            = U * sizeof(double) + U * in.n_isotopes * sizeof(int);
    size_t memtotal;

    // Every place carries its own replica of the whole table.
    memtotal = (size_t) in.places * (all_nuclide_grids + size_UEG);
    memtotal = memtotal / 1048576;
    return memtotal;
}
