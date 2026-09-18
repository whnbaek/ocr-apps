#include "ocr.h"

#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<math.h>
#include<complex.h>

#include <extensions/ocr-affinity.h>

#ifdef PAPI
#include "papi.h"
#endif

#define PI 3.14159265359

/* Placement.  The decomposition is a fixed number of lookup partitions
 * ("places") that does not follow the machine: partition p runs on
 * policy domain p % pdCount, and every object a partition owns is created
 * by a task already running there, so a home never has to be named twice.
 * Datasets are sharded the same way — nuclide i is generated, and therefore
 * homed, on pd i % pdCount. */
static inline ocrHint_t * rsEdtAt(ocrHint_t *h, u64 index)
{
    u64 pdCount;
    ocrAffinityCount(AFFINITY_PD, &pdCount);
    if (pdCount <= 1) return NULL_HINT;
    ocrGuid_t aff;
    ocrAffinityGetAt(AFFINITY_PD, index % pdCount, &aff);
    ocrHintInit(h, OCR_HINT_EDT_T);
    ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
}

static inline ocrHint_t * rsEdtHere(ocrHint_t *h)
{
    u64 pdCount;
    ocrAffinityCount(AFFINITY_PD, &pdCount);
    if (pdCount <= 1) return NULL_HINT;
    ocrGuid_t aff;
    ocrAffinityGetCurrent(&aff);
    ocrHintInit(h, OCR_HINT_EDT_T);
    ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
}

static inline ocrHint_t * rsDbHere(ocrHint_t *h)
{
    u64 pdCount;
    ocrAffinityCount(AFFINITY_PD, &pdCount);
    if (pdCount <= 1) return NULL_HINT;
    ocrGuid_t aff;
    ocrAffinityGetCurrent(&aff);
    ocrHintInit(h, OCR_HINT_DB_T);
    ocrSetHintValue(h, OCR_HINT_DB_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
}

/* A guid handed to a task that must add a dependence on it, but that never
 * reads a block holding it.  Two slots so the packing holds for either guid
 * width. */
#define RS_GUID_PARAMS 2

static inline void rsPackGuid(u64 *p, ocrGuid_t g)
{
    p[0] = 0; p[1] = 0;
    memcpy(p, &g, sizeof(g));
}

static inline ocrGuid_t rsUnpackGuid(const u64 *p)
{
    ocrGuid_t g;
    memcpy(&g, p, sizeof(g));
    return g;
}

// typedefs
typedef enum __hm{SMALL, LARGE, XL, XXL} HM_size;

typedef struct{
    int nthreads;
    int n_nuclides;
    int n_mats;
    int lookups;
    int avg_n_poles;
    int avg_n_windows;
    HM_size HM;
    int numL;
    int doppler;
    // Lookups per sync batch inside one partition.  The batch is a FINISH
    // scope and a partition's next batch starts only when it has drained,
    // so this is one partition's in-flight width.
    int batch;
    // Lookup partitions.  The lookup range is split into this many
    // contiguous shares, each with its own producer chain; it is a property
    // of the decomposition, not of the machine, so the answer does not
    // depend on how many nodes run it.
    int places;
} Inputs;

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

typedef struct
{
    int* n_poles;
    int* n_windows;
    Pole* poles_inuc;
    Window* windows_inuc;
    double* pseudo_K0RS_inuc;
} CalcDataPtrs;

/* What one nuclide generator publishes about the blocks it created. */
typedef struct
{
    ocrGuid_t DBK_poles;
    ocrGuid_t DBK_windows;
    ocrGuid_t DBK_pseudoK0RS;
    unsigned long long cksum;
} nuclideResult_t;

/* What one material generator publishes about the blocks it created. */
typedef struct
{
    ocrGuid_t DBK_nuclideIDs;
    ocrGuid_t DBK_nuclideConcs;
    int num_nucs;
} materialResult_t;

// io.c
void logo(int version);
void center_print(const char *s, int width);
void border_print(void);
void fancy_int( long a );
Inputs read_CLI( int argc, char * argv[] );
void print_CLI_error(void);
void print_input_summary(Inputs input);
void print_results(Inputs input, double runtime);

/* The data set generator.  It is shared verbatim with the other
 * decompositions of this benchmark, which is what makes their checksums
 * comparable: they describe one instance. */
#include "../../rs_generate.h"

// init.c
void generate_n_poles( Inputs input, int *R );
void generate_n_windows( Inputs input, int *R );
unsigned long long generate_poles_i( Inputs input, int i, int n_poles, Pole *R );
void generate_window_params_i( int i, int n_windows, int n_poles, Window *R );
void generate_pseudo_K0RS_i( Inputs input, int i, double *R );

// material.c
int  load_num_nucs_i( Inputs input, int i );
void load_mat_i( int i, int num_nucs, int *ids );
void load_concs_i( int i, int num_nucs, double *concs );
int pick_mat( unsigned long * seed );

// utils.c
double rn(unsigned long * seed);
size_t get_mem_estimate( Inputs input );

// rs_kernel.c
void calculate_micro_xs( double * micro_xs, int nuc, double E, Inputs input, CalcDataPtrs data, complex double * sigTfactors);
void calculate_micro_xs_doppler( double * micro_xs, int nuc, double E, Inputs input, CalcDataPtrs data, complex double * sigTfactors);
void calculate_sig_T( int nuc, double E, Inputs input, CalcDataPtrs data, complex double * sigTfactors );

// papi.c
void counter_init( int *eventset, int *num_papi_events );
void counter_stop( int * eventset, int num_papi_events );
