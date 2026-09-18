#include "rsbench.h"

// JRT
void generate_n_poles( Inputs input, int *R )
{
    rs_gen_counts( input.n_nuclides, input.avg_n_poles, RS_STREAM_NPOLES, R );
}

void generate_n_windows( Inputs input, int *R )
{
    rs_gen_counts( input.n_nuclides, input.avg_n_windows, RS_STREAM_NWINDOWS, R );
}

unsigned long long generate_poles_i( Inputs input, int i, int n_poles, Pole *R )
{
    return rs_gen_poles_i( input.numL, i, n_poles, R );
}

void generate_window_params_i( int i, int n_windows, int n_poles, Window *R )
{
    rs_gen_windows_i( i, n_windows, n_poles, R );
}

void generate_pseudo_K0RS_i( Inputs input, int i, double *R )
{
    rs_gen_k0rs_i( input.numL, i, R );
}
