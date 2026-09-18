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
 * machine's reads: the per-nuclide pole/window/K0RS blocks spread by
 * descending expected read rate (one nuclide's three blocks co-located), so
 * that the few nuclides most materials contain never share a rank, and the
 * material tables and the handle singletons occupy index bands of their own
 * — one datablock's serving cannot be split, but the set's aggregate load
 * can. */
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
/* A task that fills a datablock is placed on that block's home, so the write
 * of a block that is read-only for the rest of the run happens where the run
 * will read it. */
static inline ocrHint_t * mcSpreadEdtHint(ocrHint_t *h, u64 i) {
    u64 pdCount;
    ocrAffinityCount(AFFINITY_PD, &pdCount);
    if (pdCount <= 1) return NULL_HINT;
    ocrGuid_t aff;
    ocrAffinityGetAt(AFFINITY_PD, i % pdCount, &aff);
    ocrHintInit(h, OCR_HINT_EDT_T);
    ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
}
#else
#define mcChainEdtHint(h) NULL_HINT
#define mcSpreadDbHint(h,i) NULL_HINT
#define mcSpreadEdtHint(h,i) NULL_HINT
#endif

/* A task that has to name an object created by another task can only be told
 * about it through the parameter block, which is an array of u64. */
_Static_assert( sizeof(ocrGuid_t) == sizeof(u64),
                "guid must fit a parameter word" );

static inline u64 guidToParam(ocrGuid_t g)
{
    u64 v;
    memcpy(&v, &g, sizeof(v));
    return v;
}

static inline ocrGuid_t guidFromParam(u64 v)
{
    ocrGuid_t g;
    memcpy(&g, &v, sizeof(v));
    return g;
}

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
void print_results(Inputs input, double runtime, unsigned long long cksum);

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
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
u64 mcNuclideSpreadIndex( Inputs input, int nuclide );
#endif
void load_num_nucs( Inputs input, int* num_nucs );
int  load_num_nucs_i( Inputs input, int i );
void load_mat_i( int i, int num_nucs, int *ids );
void load_concs_i( int i, int num_nucs, double *concs );
int pick_mat( unsigned long * seed );

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
