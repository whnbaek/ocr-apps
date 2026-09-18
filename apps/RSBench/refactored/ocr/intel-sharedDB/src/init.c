#include "rsbench.h"

// JRT
int* generate_n_poles( ocrDBK_t* DBK_n_poles, Inputs input, u64 dataSeed )
{
    int total_resonances = input.avg_n_poles * input.n_nuclides;
    u64 key = rs_stream( dataSeed, RS_STREAM_N_POLES );
    int * R; // = (int *) calloc( input.n_nuclides, sizeof(int));
    ocrDbCreate( DBK_n_poles, (void **) &R, input.n_nuclides*sizeof(int),
                    0, NULL_HINT, NO_ALLOC );


    for( int i = 0; i < input.n_nuclides; i++ )
        R[i] = 0;

    for( int i = 0; i < total_resonances; i++ )
        R[rs_draw( key, (u64) i ) % (u64) input.n_nuclides]++;

    // Ensure all nuclides have at least 1 resonance
    for( int i = 0; i < input.n_nuclides; i++ )
        if( R[i] == 0 )
            R[i] = 1;

    //Debug
    //for( int i = 0; i < input.n_nuclides; i++ )
    //  printf("R[%d] = %d\n", i, R[i]);

    return R;
}

int * generate_n_windows( ocrDBK_t* DBK_n_windows, Inputs input, u64 dataSeed )
{
    int total_resonances = input.avg_n_windows * input.n_nuclides;
    u64 key = rs_stream( dataSeed, RS_STREAM_N_WINDOWS );

    int * R; // = (int *) calloc( input.n_nuclides, sizeof(int));
    ocrDbCreate( DBK_n_windows, (void **) &R, input.n_nuclides*sizeof(int),
                    0, NULL_HINT, NO_ALLOC );
    for( int i = 0; i < input.n_nuclides; i++ )
        R[i] = 0;

    for( int i = 0; i < total_resonances; i++ )
        R[rs_draw( key, (u64) i ) % (u64) input.n_nuclides]++;

    // Ensure all nuclides have at least 1 resonance
    for( int i = 0; i < input.n_nuclides; i++ )
        if( R[i] == 0 )
            R[i] = 1;

    /* Debug
    for( int i = 0; i < input.n_nuclides; i++ )
        printf("R[%d] = %d\n", i, R[i]);
    */

    return R;
}

void assign_pole_ptrs( int n_nuclides, int* n_poles, Pole* contiguous, Pole** R)
{
    int k = 0;
    for( int i = 0; i < n_nuclides; i++ )
    {
        R[i] = &contiguous[k];
        k += n_poles[i];
    }
}

//
Pole ** generate_poles( ocrDBK_t* DBK_poles, ocrDBK_t* DBK_pole_ptrs, Inputs input, int * n_poles, u64 dataSeed )
{
    // Pole Scaling Factor -- Used to bias hitting of the fast Faddeeva
    // region to approximately 99.5% (i.e., only 0.5% of lookups should
    // require the full eval).
    double f = 152.5;
    u64 key = rs_stream( dataSeed, RS_STREAM_POLES );
    Pole ** R; // = (Pole **) malloc( input.n_nuclides * sizeof( Pole *));
    ocrDbCreate( DBK_pole_ptrs, (void **) &R, input.n_nuclides*sizeof(Pole *),
                    0, NULL_HINT, NO_ALLOC );
    Pole * contiguous; // = (Pole *) malloc( input.n_nuclides * input.avg_n_poles * sizeof(Pole));

    // The per-nuclide counts are drawn, not derived from the requested
    // average: the floor that gives every nuclide at least one resonance can
    // push their sum above n_nuclides * avg, so the array is sized by the
    // counts it must actually hold.
    u64 total_poles = 0;
    for( int i = 0; i < input.n_nuclides; i++ )
        total_poles += (u64) n_poles[i];

    ocrDbCreate( DBK_poles, (void **) &contiguous, total_poles*sizeof(Pole),
                    0, NULL_HINT, NO_ALLOC );
    assign_pole_ptrs( input.n_nuclides, n_poles, contiguous, R );

    // fill with data; a pole's values are keyed by its slot in the contiguous
    // array, so they do not depend on the order the nuclides are walked in
    u64 k = 0;
    for( int i = 0; i < input.n_nuclides; i++ )
        for( int j = 0; j < n_poles[i]; j++ )
        {
            u64 d = 9*k;
            R[i][j].MP_EA = f*(rs_draw_unit(key,d+0) + rs_draw_unit(key,d+1) * I);
            R[i][j].MP_RT = f*rs_draw_unit(key,d+2) + rs_draw_unit(key,d+3) * I;
            R[i][j].MP_RA = f*rs_draw_unit(key,d+4) + rs_draw_unit(key,d+5) * I;
            R[i][j].MP_RF = f*rs_draw_unit(key,d+6) + rs_draw_unit(key,d+7) * I;
            R[i][j].l_value = (short int)( rs_draw(key,d+8) % (u64) input.numL );
            k++;
        }

    /* Debug
    for( int i = 0; i < input.n_nuclides; i++ )
        for( int j = 0; j < n_poles[i]; j++ )
            printf("R[%d][%d]: Eo = %lf lambda_o = %lf Tn = %lf Tg = %lf Tf = %lf\n", i, j, R[i][j].Eo, R[i][j].lambda_o, R[i][j].Tn, R[i][j].Tg, R[i][j].Tf);
    */

    return R;
}

void assign_window_ptrs( int n_nuclides, int* n_windows, Window* contiguous, Window** R)
{
    int k = 0;
    for( int i = 0; i < n_nuclides; i++ )
    {
        R[i] = &contiguous[k];
        k += n_windows[i];
    }
}

Window ** generate_window_params( ocrDBK_t* DBK_windows, ocrDBK_t* DBK_window_ptrs, Inputs input, int * n_windows, int * n_poles, u64 dataSeed )
{
    // Allocating 2D contiguous matrix
    u64 key = rs_stream( dataSeed, RS_STREAM_WINDOWS );
    Window ** R; // = (Window **) malloc( input.n_nuclides * sizeof( Window *));
    ocrDbCreate( DBK_window_ptrs, (void **) &R, input.n_nuclides*sizeof(Window *),
                    0, NULL_HINT, NO_ALLOC );
    Window * contiguous; // = (Window *) malloc( input.n_nuclides * input.avg_n_windows * sizeof(Window));

    u64 total_windows = 0;
    for( int i = 0; i < input.n_nuclides; i++ )
        total_windows += (u64) n_windows[i];

    ocrDbCreate( DBK_windows, (void **) &contiguous, total_windows*sizeof(Window),
                    0, NULL_HINT, NO_ALLOC );
    assign_window_ptrs( input.n_nuclides, n_windows, contiguous, R );

    // fill with data
    u64 k = 0;
    for( int i = 0; i < input.n_nuclides; i++ )
    {
        int space = n_poles[i] / n_windows[i];
        int remainder = n_poles[i] - space * n_windows[i];
        int ctr = 0;
        for( int j = 0; j < n_windows[i]; j++ )
        {
            u64 d = 3*k;
            k++;
            R[i][j].T = rs_draw_unit(key,d+0);
            R[i][j].A = rs_draw_unit(key,d+1);
            R[i][j].F = rs_draw_unit(key,d+2);
            R[i][j].start = ctr;
            R[i][j].end = ctr + space - 1;

            ctr += space;

            if ( j < remainder )
            {
                ctr++;
                R[i][j].end++;
            }
        }
    }

    return R;
}

void assign_pseudo_K0RS_ptrs( int n_nuclides, int numL, double* contiguous, double** R )
{
    for( int i = 0; i < n_nuclides; i++ )
        R[i] = &contiguous[i*numL];
}

double ** generate_pseudo_K0RS( ocrDBK_t* DBK_pseudo_K0RS, ocrDBK_t* DBK_pseudo_K0RS_ptrs, Inputs input, u64 dataSeed )
{
    u64 key = rs_stream( dataSeed, RS_STREAM_K0RS );

    double ** R; // = (double **) malloc( input.n_nuclides * sizeof( double * ));
    ocrDbCreate( DBK_pseudo_K0RS_ptrs, (void **) &R, input.n_nuclides*sizeof(double *),
                    0, NULL_HINT, NO_ALLOC );
    double * contiguous; // = (double *) malloc( input.n_nuclides * input.numL * sizeof(double));

    ocrDbCreate( DBK_pseudo_K0RS, (void **) &contiguous, input.n_nuclides*input.numL*sizeof(double),
                    0, NULL_HINT, NO_ALLOC );
    assign_pseudo_K0RS_ptrs( input.n_nuclides, input.numL, contiguous, R );

    for( int i = 0; i < input.n_nuclides; i++ )
        R[i] = &contiguous[i*input.numL];

    for( int i = 0; i < input.n_nuclides; i++)
        for( int j = 0; j < input.numL; j++ )
            R[i][j] = rs_draw_unit( key, (u64) i * input.numL + j );

    return R;
}
