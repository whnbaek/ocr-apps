// Material data is hard coded into the functions in this file.
// Note that there are 12 materials present in H-M (large or small)

#include "XSbench_header.h"

// Small H-M has 34 fuel nuclides
static const int mats0_Sml[] =  { 58, 59, 60, 61, 40, 42, 43, 44, 45, 46, 1, 2, 3, 7,
                 8, 9, 10, 29, 57, 47, 48, 0, 62, 15, 33, 34, 52, 53,
                 54, 55, 56, 18, 23, 41 }; //fuel

// These are the non-fuel materials
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

static const int * const mats[12] = { mats0_Sml, mats1, mats2, mats3, mats4, mats5,
                                      mats6, mats7, mats8, mats9, mats10, mats11 };

static const int num_nucs_table[12] = { 34, 5, 4, 4, 27, 21, 21, 21, 21, 21, 9, 9};

// Material 0 is a special case (fuel). The H-M small reactor uses
// 34 nuclides, while H-M large uses 321.
int mat_num_nucs( int mat, long n_isotopes )
{
    if( mat == 0 && n_isotopes != 68 )
        return 321;
    return num_nucs_table[mat];
}

// H-M large adds nuclides to fuel only, as a contiguous range above the small
// reactor's isotope set.
void load_mat_nuclides( int mat, long n_isotopes, int * nucl_index_list )
{
    int n = mat_num_nucs( mat, n_isotopes );
    int base = num_nucs_table[mat];
    int i;

    for( i = 0; i < base && i < n; i++ )
        nucl_index_list[i] = mats[mat][i];

    for( i = base; i < n; i++ )
        nucl_index_list[i] = 68 + (i - base);
}

// picks a material based on a probabilistic distribution
int pick_mat( unsigned long * seed )
{
    // I have a nice spreadsheet supporting these numbers. They are
    // the fractions (by volume) of material in the core. Not a
    // *perfect* approximation of where XS lookups are going to occur,
    // but this will do a good job of biasing the system nonetheless.

    // Also could be argued that doing fractions by weight would be
    // a better approximation, but volume does a good enough job for now.

    double dist[12];
    dist[0]  = 0.140;   // fuel
    dist[1]  = 0.052;   // cladding
    dist[2]  = 0.275;   // cold, borated water
    dist[3]  = 0.134;   // hot, borated water
    dist[4]  = 0.154;   // RPV
    dist[5]  = 0.064;   // Lower, radial reflector
    dist[6]  = 0.066;   // Upper reflector / top plate
    dist[7]  = 0.055;   // bottom plate
    dist[8]  = 0.008;   // bottom nozzle
    dist[9]  = 0.015;   // top nozzle
    dist[10] = 0.025;   // top of fuel assemblies
    dist[11] = 0.013;   // bottom of fuel assemblies

    double roll = rn(seed);

    // makes a pick based on the distro
    for( int i = 0; i < 12; i++ )
    {
        double running = 0;
        for( int j = i; j > 0; j-- )
            running += dist[j];
        if( roll < running )
            return i;
    }

    return 0;
}
