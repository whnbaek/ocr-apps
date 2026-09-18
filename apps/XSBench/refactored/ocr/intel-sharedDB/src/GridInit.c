#include "XSbench_header.h"

// Generates the randomized energy grid of every nuclide.  Each value is keyed
// by its own position in the grid, so the grid is a pure function of the
// instance's data seed and is identical however the work is distributed.
void generate_grids( NuclideGridPoint ** nuclide_grids,
                     long n_isotopes, long n_gridpoints, u64 dataSeed ) {
	u64 key = xs_stream( dataSeed, XS_STREAM_GRIDS );

	for( long i = 0; i < n_isotopes; i++ )
		for( long j = 0; j < n_gridpoints; j++ )
		{
			u64 base = (u64)(i*n_gridpoints + j) * 6;
			nuclide_grids[i][j].energy       = xs_draw_unit(key, base + 0);
			nuclide_grids[i][j].total_xs     = xs_draw_unit(key, base + 1);
			nuclide_grids[i][j].elastic_xs   = xs_draw_unit(key, base + 2);
			nuclide_grids[i][j].absorbtion_xs= xs_draw_unit(key, base + 3);
			nuclide_grids[i][j].fission_xs   = xs_draw_unit(key, base + 4);
			nuclide_grids[i][j].nu_fission_xs= xs_draw_unit(key, base + 5);
		}
}

// Sorts the nuclide grids by energy (lowest -> highest)
void sort_nuclide_grids( NuclideGridPoint ** nuclide_grids, long n_isotopes,
                         long n_gridpoints )
{
	int (*cmp) (const void *, const void *);
	cmp = NGP_compare;

	for( long i = 0; i < n_isotopes; i++ )
		qsort( nuclide_grids[i], n_gridpoints, sizeof(NuclideGridPoint),
		       cmp );

	// error debug check
	/*
	for( int i = 0; i < n_isotopes; i++ )
	{
		ocrPrintf("NUCLIDE %d ==============================\n", i);
		for( int j = 0; j < n_gridpoints; j++ )
			ocrPrintf("E%d = %lf\n", j, nuclide_grids[i][j].energy);
	}
	*/
}

// Allocates unionized energy grid, and assigns union of energy levels
// from nuclide grids to it.
GridPoint * generate_energy_grid( ocrDBK_t* DBK_uEnergy_grid,  ocrDBK_t* DBK_xs_grid, long n_isotopes, long n_gridpoints,
                                  NuclideGridPoint ** nuclide_grids, int mype, int ** PTR_xs_grid ) {
	if( mype == 0 ) ocrPrintf("Generating Unionized Energy Grid...\n");

	long n_unionized_grid_points = n_isotopes*n_gridpoints;
	int (*cmp) (const void *, const void *);
	cmp = NGP_compare;

	GridPoint * energy_grid; // = (GridPoint *)malloc( n_unionized_grid_points
	                         //                      * sizeof( GridPoint ) );
    ocrDbCreate( DBK_uEnergy_grid, (void **) &energy_grid, n_unionized_grid_points*sizeof(NuclideGridPoint),
                    0, NULL_HINT, NO_ALLOC );
	if( mype == 0 ) ocrPrintf("Copying and Sorting all nuclide grids...\n");

    ocrDBK_t DBK_nuclide_grids, DBK_nuclide_grid_ptrs;
	NuclideGridPoint ** n_grid_sorted = gpmatrix( &DBK_nuclide_grids, &DBK_nuclide_grid_ptrs,
                                                    n_isotopes, n_gridpoints );

	memcpy( n_grid_sorted[0], nuclide_grids[0], n_isotopes*n_gridpoints*
	                                      sizeof( NuclideGridPoint ) );

	qsort( &n_grid_sorted[0][0], n_unionized_grid_points,
	       sizeof(NuclideGridPoint), cmp);

	if( mype == 0 ) ocrPrintf("Assigning energies to unionized grid...\n");

	for( long i = 0; i < n_unionized_grid_points; i++ )
		energy_grid[i].energy = n_grid_sorted[0][i].energy;


	//gpmatrix_free(n_grid_sorted);
    ocrDbDestroy(DBK_nuclide_grids);
    ocrDbDestroy(DBK_nuclide_grid_ptrs);

	int * full; // = (int *) malloc( n_isotopes * n_unionized_grid_points
	            //                 * sizeof(int) );

    ocrDbCreate( DBK_xs_grid, (void **) &full, n_isotopes*n_unionized_grid_points*sizeof(int),
                    0, NULL_HINT, NO_ALLOC );

	if( full == NULL )
	{
		ocrPrintf("ERROR - Out Of Memory!\n");
	}

	*PTR_xs_grid = full;

	// debug error checking

    /*
	for( int i = 0; i < n_unionized_grid_points; i++ )
		ocrPrintf("E%d = %f\n", i, energy_grid[i].energy);
    */

	return energy_grid;
}

// Searches each nuclide grid for the closest energy level and assigns the
// index from the unionized grid to the correct spot in the nuclide grid, for
// the half-open row range [ibegin, iend).  Rows are independent -- a row reads
// only its own unionized energy and every nuclide grid -- so the range is a
// pure function of its bounds and the caller may split the rows over as many
// producers as it likes.  Output is written densely from the range's own
// origin, so a producer never shares a destination with another producer.
void set_grid_ptrs_range( GridPoint * energy_grid, int * xs_rows,
                          NuclideGridPoint ** nuclide_grids,
                          long n_isotopes, long n_gridpoints,
                          u64 ibegin, u64 iend )
{
	for( u64 i = ibegin; i < iend; i++ )
	{
		double quarry = energy_grid[i].energy;
		for( long j = 0; j < n_isotopes; j++ )
		{
			// j is the nuclide i.d.
			// log n binary search
			xs_rows[(i - ibegin) * (u64) n_isotopes + (u64) j] =
				binary_search( nuclide_grids[j], quarry, n_gridpoints);
		}
	}
}
