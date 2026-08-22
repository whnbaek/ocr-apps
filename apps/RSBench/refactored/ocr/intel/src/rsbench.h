#include "ocr.h"

#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<math.h>
#include<complex.h>

#ifdef PAPI
#include "papi.h"
#endif

#define PI 3.14159265359

/* Placement-optimization layer.  Two placement facts drive it, and the
 * helpers live here because the datablocks they hint are created across
 * several translation units.  Each lookup is a three-task chain whose links
 * round-robin independently as-born, so a chain's intermediate results
 * cross ranks twice for nothing: the first link keeps the round-robin (that
 * is the load balance across lookups), the two links it spawns pin to the
 * rank it landed on.  And every dataset object is created inside one init
 * task and therefore homed on a single rank, which then serves the whole
 * machine's reads: the per-nuclide pole/window/K0RS blocks spread
 * round-robin by nuclide (one nuclide's three blocks co-located), the
 * material tables spread by material, and the handle singletons every
 * lookup acquires each land on a different rank — one datablock's serving
 * cannot be split, but the set's aggregate load can. */
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
#include <extensions/ocr-affinity.h>
static inline ocrHint_t * mcChainEdtHint(ocrHint_t *h) {
    u64 pdCount;
    ocrAffinityCount(AFFINITY_PD, &pdCount);
    if (pdCount <= 1) return NULL_HINT;
    ocrGuid_t aff;
    ocrAffinityGetCurrent(&aff);
    ocrHintInit(h, OCR_HINT_EDT_T);
    ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
}
static inline ocrHint_t * mcSpreadDbHint(ocrHint_t *h, u64 i) {
    u64 pdCount;
    ocrAffinityCount(AFFINITY_PD, &pdCount);
    if (pdCount <= 1) return NULL_HINT;
    ocrGuid_t aff;
    ocrAffinityGetAt(AFFINITY_PD, i % pdCount, &aff);
    ocrHintInit(h, OCR_HINT_DB_T);
    ocrSetHintValue(h, OCR_HINT_DB_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
}
#else
#define mcChainEdtHint(h) NULL_HINT
#define mcSpreadDbHint(h,i) NULL_HINT
#endif

// typedefs
typedef enum __hm{SMALL, LARGE, XL, XXL} HM_size;

typedef struct{
    int nprocs;
    int nthreads;
    int n_nuclides;
    int n_mats;
    int lookups;
    int avg_n_poles;
    int avg_n_windows;
    HM_size HM;
    int numL;
    int doppler;
    // Lookups per compute-phase sync batch.  The batch is a FINISH scope and
    // the next one starts only when it has drained, so this is the in-flight
    // width of the whole compute phase -- it has to be able to track the
    // machine.
    int batch;
} Inputs;

typedef struct{
    int * num_nucs;
    int ** mats;
    double ** concs;
} Materials;

typedef struct{
    complex double MP_EA;
    complex double MP_RT;
    complex double MP_RA;
    complex double MP_RF;
    short int l_value;
} Pole;

typedef struct{
    double T;
    double A;
    double F;
    int start;
    int end;
} Window;

//typedef struct{
//  int * n_poles;
//  int * n_windows;
//  Materials materials;
//  Pole ** poles;
//  Window ** windows;
//  double ** pseudo_K0RS;
//} CalcDataPtrs;

typedef struct
{
    int* n_poles;
    int* n_windows;
    Pole* poles_inuc;
    Window* windows_inuc;
    double* pseudo_K0RS_inuc;
} CalcDataPtrs;


// io.c
void logo(int version);
void center_print(const char *s, int width);
void border_print(void);
void fancy_int( int a );
Inputs read_CLI( int argc, char * argv[] );
void print_CLI_error(void);
void print_input_summary(Inputs input);
void print_results(Inputs input, double runtime);

// init.c
void generate_n_poles( Inputs input, int *R );
void generate_n_windows( Inputs input, int *R );
void generate_poles( Inputs input, int * n_poles, ocrGuid_t* PTR_pole_DBguids_nuclide );
void generate_window_params( Inputs input, int * n_windows, int * n_poles, ocrGuid_t* PTR_window_DBguids_nuclide );
void generate_pseudo_K0RS( Inputs input, ocrGuid_t* PTR_pseudoK0RS_DBguids_nuclide );

// material.c
void load_num_nucs(Inputs input, int* num_nucs);
void load_mats( Inputs input, int * num_nucs, ocrGuid_t* PTR_nuclideIDs_DBguids_mat);
void load_concs( int * num_nucs, ocrGuid_t* PTR_nuclideConcs_DBguids_mat );
int pick_mat( unsigned long * seed );
void get_materials(Inputs input, int* PTR_mat_num_nucs, ocrGuid_t* PTR_nuclideIDs_DBguids_mat, ocrGuid_t* PTR_nuclideConcs_DBguids_mat);

// utils.c
double rn(unsigned long * seed);
size_t get_mem_estimate( Inputs input );

// rs_kernel.c
//void calculate_macro_xs( double * macro_xs, int mat, double E, Inputs input, CalcDataPtrs data, complex double * sigTfactors );
void calculate_micro_xs( double * micro_xs, int nuc, double E, Inputs input, CalcDataPtrs data, complex double * sigTfactors);
void calculate_micro_xs_doppler( double * micro_xs, int nuc, double E, Inputs input, CalcDataPtrs data, complex double * sigTfactors);
void calculate_sig_T( int nuc, double E, Inputs input, CalcDataPtrs data, complex double * sigTfactors );

// papi.c
void counter_init( int *eventset, int *num_papi_events );
void counter_stop( int * eventset, int num_papi_events );
