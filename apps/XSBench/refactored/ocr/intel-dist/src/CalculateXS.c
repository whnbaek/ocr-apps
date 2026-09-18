#include "XSbench_header.h"

// Calculates the microscopic cross section for a given nuclide & energy
void calculate_micro_xs_new(   double p_energy,
                           long n_gridpoints, int xs_indices_nuc,
                           NuclideGridPoint * restrict PTR_nuclide_data_j,
                           double * restrict xs_vector ){

    // Variables
    double f;
    NuclideGridPoint * low, * high;

    // pull ptr from energy grid and check to ensure that
    // we're not reading off the end of the nuclide's grid
    if( xs_indices_nuc == n_gridpoints - 1 )
        low = &PTR_nuclide_data_j[xs_indices_nuc - 1];
    else
        low = &PTR_nuclide_data_j[xs_indices_nuc];

    high = low + 1;

    // calculate the re-useable interpolation factor
    f = (high->energy - p_energy) / (high->energy - low->energy);

    // Total XS
    xs_vector[0] = high->total_xs - f * (high->total_xs - low->total_xs);

    // Elastic XS
    xs_vector[1] = high->elastic_xs - f * (high->elastic_xs - low->elastic_xs);

    // Absorbtion XS
    xs_vector[2] = high->absorbtion_xs - f * (high->absorbtion_xs - low->absorbtion_xs);

    // Fission XS
    xs_vector[3] = high->fission_xs - f * (high->fission_xs - low->fission_xs);

    // Nu Fission XS
    xs_vector[4] = high->nu_fission_xs - f * (high->nu_fission_xs - low->nu_fission_xs);
}

// (fixed) binary search for energy on unionized energy grid
// returns lower index
long grid_search_double( long n, double quarry, double * A)
{
    long lowerLimit = 0;
    long upperLimit = n-1;
    long examinationPoint;
    long length = upperLimit - lowerLimit;

    while( length > 1 )
    {
        examinationPoint = lowerLimit + ( length / 2 );

        if( A[examinationPoint] > quarry )
            upperLimit = examinationPoint;
        else
            lowerLimit = examinationPoint;

        length = upperLimit - lowerLimit;
    }

    return lowerLimit;
}
