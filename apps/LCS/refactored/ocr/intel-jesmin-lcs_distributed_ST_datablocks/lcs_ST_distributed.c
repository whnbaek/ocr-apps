/*
Author Jesmin Jahan Tithi
Copyright Intel Corporation 2016
*/
//export OCR_ASAN=yes
#ifndef ENABLE_EXTENSION_LABELING
#define ENABLE_EXTENSION_LABELING
#endif

#include "ocr.h"
#include "extensions/ocr-labeling.h" //currently needed for labeled guids
#include "extensions/ocr-affinity.h" //needed for affinity


#ifdef USE_PROFILER
#include "extensions/ocr-profiler.h"
#endif
#include <stdio.h>
#include "string.h"
#include <stdlib.h>
#include <math.h>
#include <assert.h>

// User defined libraries
#include "cilktime.h" // timing library

#define MAX(a, b) ((a>b)?a:b)

#define GAP_PENALTY 0 //for LCS 0, for edit distance 1 and use min, for Smith waterman depends on the blosum matrix


typedef struct {
      long N, base, xi, xj, n;  // input size, top-left corner index, and size of the current
	  ocrGuid_t s_guid, t_guid, score_guid; // s_label, t_label, score_matrix_labels
} LCS_task_params;

typedef struct {
      long N, xi, xj, n;  // input and base case size, top-left corner index
} LCS_base_params;


/* Digest of the computed score array: an FNV-1a walk over the cells in index
 * order, folded to 31 bits so the value is exact in every consumer's numeric
 * type.  The caller must pass only the cells the boundary fill and the kernel
 * write; a datablock's payload is not zero-initialised, so digesting one cell
 * past the written range makes the value allocator-dependent.  Every written
 * cell contributes, so a dropped, repeated or mis-indexed write changes it --
 * but this collapsed max-recurrence saturates to a value that does not depend
 * on the input characters, so the digest cannot witness the comparison
 * itself. */
/* Character of the input strings: a pure function of (tile index, position)
 * built from a self-contained mixer, so the instance is a property of the
 * program alone -- no shared generator state, no library generator whose
 * sequence is defined by the host's C library, and the same characters at
 * every node count and in every tier. */
static int lcsChar(u64 tile, u64 pos)
{
    u64 z = tile * 0x9E3779B97F4A7C15ULL + pos * 0xBF58476D1CE4E5B9ULL
          + 0x165667B19E3779F9ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return (int)(z % 4) + 'A';
}

static u64 lcsChecksum(const long *v, long n)
{
    u64 h = 1469598103934665603ULL;
    for (long k = 0; k < n; k++) {
        h ^= (u64)(u32)v[k];
        h *= 1099511628211ULL;
    }
    return h & 0x7fffffffULL;
}

//base case kernel
void inline serial_lcs_1D(long* X, int *S, int *T, long N, long int xi, long int xj, long int n){
    long Nplus1 = N + 1;
    long int endi = (xi + n > N) ? (Nplus1  - xi) : n;//not needed
    long int endj = (xj + n > N) ? (Nplus1  - xj) : n;//not needed

    // Diagonal by diagonal computation.



    // First half of the diagonals
    for (int t = 0; t < endi; t++)
    {
        for(int i = 0; i <= t;i++)
        {
            int j = t - i;
            long int idx = (N+(xj+j-xi-i));
            //Optimize
            int del = X[idx - 1];
            int ins = X[idx + 1];
            int del_ins = (del > ins) ? (del + GAP_PENALTY):(ins + GAP_PENALTY);
            int match = X[idx] + (S[i] != T[j]);
            X[idx]= MAX(match, del_ins);

        }

    }
    // Second half of the diagonals


    int n_minus1 = endi - 1;
	int nx2 = n_minus1 + endi;
    for (int t = endi; t < nx2; t++) {
        for(int i = n_minus1; i >= (t - n_minus1);i--)
        {
            int j = t - i;
            long int idx = (N+(xj+j-xi-i));
            //Optimize
            int del = X[idx - 1];
            int ins = X[idx + 1];
            int del_ins = (del > ins) ? (del + GAP_PENALTY):(ins + GAP_PENALTY);
            int match = X[idx] + (S[i] != T[j]);
            X[idx]= MAX(match, del_ins);
        }
    }
}

// OCR libraries



/* --------------------------------------------------------------------------
 * Placement of the recursion.
 *
 * The one mutable object of this program is written under an exclusive turn
 * by every base case, so a base case that runs on a different node than its
 * predecessor moves the whole block.  What costs least is therefore not
 * spreading the base cases but keeping a whole sub-recursion -- a long run of
 * consecutive turns -- on one node.
 *
 * The map cuts the square into a G x G grid of equal quadrant subtrees, G the
 * smallest power of two whose square covers every node, and gives each node a
 * contiguous band of that grid.  Equal quadrants and a contiguous band make
 * the per-node share exact whenever the node count divides G*G.  The order
 * the band is cut along is not row-major: inside a recursion node the two
 * children that may run CONCURRENTLY are indexed adjacently, so a cut falls
 * between children an event already orders rather than between two that would
 * otherwise contend for the block from different nodes.
 *
 * The band is a pure function of the region's own corner, which every parent
 * knows for every child, so the map is decided without consulting where the
 * creating task happens to run -- reading that would funnel the tree back
 * onto its creator.
 * -------------------------------------------------------------------------- */
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
static u64 quadBand(long N, long base, long xi, long xj)
{
	u64 nranks;
	ocrAffinityCount(AFFINITY_PD, &nranks);
	if(nranks <= 1) return 0;

	long tiles = (base > 0) ? (N / base) : 1;
	if(tiles < 1) tiles = 1;

	u64 g = 1;
	while(g * g < nranks) g <<= 1;
	while(g > (u64)tiles) g >>= 1;
	if(g < 1) g = 1;

	long span = N / (long)g;
	if(span < 1) span = 1;

	u64 ci = (u64)((xi - 1) / span);
	u64 cj = (u64)((xj - 1) / span);
	if(ci >= g) ci = g - 1;
	if(cj >= g) cj = g - 1;

	/* Descend the quadrant grid the way the recursion does: each level's two
	 * corner bits pick one digit.  The two children that differ in exactly one
	 * bit are the concurrent pair, and they take the two adjacent high
	 * digits. */
	u64 q = 0;
	u64 bit;
	for(bit = g >> 1; bit != 0; bit >>= 1)
	{
		u64 a = (ci & bit) ? 1 : 0;
		u64 b = (cj & bit) ? 1 : 0;
		q = q * 4 + ((a == b) ? a : (a ? 2 : 3));
	}
	return (q * nranks) / (g * g);
}

static ocrHint_t * rankHint(ocrHint_t * h, u64 rank)
{
	ocrGuid_t aff;
	ocrAffinityGetAt(AFFINITY_PD, rank, &aff);
	ocrHintInit(h, OCR_HINT_EDT_T);
	ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
	return h;
}
#endif

/* Node of the quadrant band that owns the region whose top-left corner is
 * (xi, xj). */
static ocrHint_t * quadHint(ocrHint_t * h, long N, long base, long xi, long xj)
{
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
	return rankHint(h, quadBand(N, base, xi, xj));
#else
	(void)h; (void)N; (void)base; (void)xi; (void)xj;
	return NULL_HINT;
#endif
}

/* A labeled range spreads its homes over the index, so a tile's own index
 * names the node its block was born on; the task that fills it belongs
 * there. */
static ocrHint_t * tileHint(ocrHint_t * h, long index)
{
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
	u64 nranks;
	ocrAffinityCount(AFFINITY_PD, &nranks);
	return rankHint(h, (u64)index % nranks);
#else
	(void)h; (void)index;
	return NULL_HINT;
#endif
}

ocrGuid_t seqLCSEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]){
   //Take the passcoresed arg and convert it back to the original type
   LCS_base_params* p = (LCS_base_params *)paramv;

   // if xi=1 || xj=1: offset = 1. in all other cases, its zero.

   //ocrPrintf("Calling seq basecase N: %lu, xi: %lu, xj: %lu, n: %lu \n", p->N, p->xi, p->xj, p->n);

   int *S = depv[0].ptr;
   int *T = depv[1].ptr;

   S = (p->xi==1)? S+1: S;
   T = (p->xj==1)? T+1: T;

   long *score = depv[2].ptr;
    // Call the base task
   serial_lcs_1D(score, S, T, p->N, p->xi, p->xj, p->n);

   return NULL_GUID;
}
ocrGuid_t recLCSEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]){

	// Cast the parameter to the original parameter format.
    LCS_task_params* p = (LCS_task_params *)paramv;

	long base = p->base;
    long n = p->n;

    if(n<=base)
    {

		//ocrPrintf("Calling basecase N: %lu, base: %lu, xi: %lu, xj: %lu, n: %lu \n", p->N, base, p->xi, p->xj, p->n);

		// First get the data blocks you need by using their guids
        //inline serial_lcs_1D(long* X, int *S, int *T, long int xi, long int xj, long int n)
	    //serial_lcs_1D(X, S, T, p->N, p->xi, p->xj, n);

		LCS_base_params p1;

	    // you can just pass n
		p1.N = p->N;
		p1.xi = p->xi;
		p1.xj = p->xj;
		p1.n = n;

		// a base case spans n columns, and the strings are cut into tiles of
		// exactly that width, so its corner names one whole tile
		ocrGuid_t S, T;
		ocrGuidFromIndex(&S, p->s_guid, (p->xi-1)/n);
		ocrGuidFromIndex(&T, p->t_guid, (p->xj-1)/n);


		// Create the task template
		ocrGuid_t base_template;
		ocrEdtTemplateCreate(&base_template, seqLCSEdt, sizeof(p1)/sizeof(u64) , 3 );

		// Create the task
		ocrGuid_t baseEdt, baseoutputEventGuidOnce;
		ocrEventCreate(&baseEdt, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);

		ocrHint_t baseHNT;

		ocrEdtCreate(&baseEdt,
					 base_template,
					 EDT_PARAM_DEF,
					 (u64*) &p1,
					 EDT_PARAM_DEF,
					 NULL,
					 EDT_PROP_NONE,
					 quadHint(&baseHNT, p1.N, base, p1.xi, p1.xj),
					 &baseoutputEventGuidOnce);

		ocrAddDependence( S, baseEdt, 0, DB_MODE_RO );
		ocrAddDependence( T, baseEdt, 1, DB_MODE_RO );

		//////////////////////////////////////////////////
		ocrAddDependence( p->score_guid, baseEdt, 2, DB_MODE_RW );

		return NULL_GUID;
		return NULL_GUID;

    }
    else
    {

        long nn = n >> 1;
		ocrHint_t quadHNT;

		//ocrPrintf("Working on size: %lu\n", nn);

		//---------------------------Call for x11-------------------------------------------
		LCS_task_params p1;

		p1.N = p->N;
		p1.base = base;
		p1.xi = p->xi;
		p1.xj = p->xj;
		p1.n = nn;
		p1.s_guid = p->s_guid;
		p1.t_guid = p->t_guid;
		p1.score_guid = p->score_guid;

		// Create the task template
		ocrGuid_t LCSGuidTmp_X11;
		ocrEdtTemplateCreate(&LCSGuidTmp_X11, recLCSEdt, sizeof(p1)/sizeof(u64) , 1);

		// Create the task
		ocrGuid_t x11Edt, x11outputEventGuid, x11outputEventGuidOnce;
		ocrEventCreate(&x11outputEventGuid, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);

		ocrEdtCreate(&x11Edt,
					  LCSGuidTmp_X11,
					  EDT_PARAM_DEF,
					  (u64*) &p1,
					  EDT_PARAM_DEF,
					  NULL,
					  EDT_PROP_FINISH,
					  quadHint(&quadHNT, p1.N, base, p1.xi, p1.xj),
					  &x11outputEventGuidOnce);
		ocrAddDependence( x11outputEventGuidOnce, x11outputEventGuid, 0, DB_MODE_NULL );
		ocrAddDependence( NULL_GUID, x11Edt, 0, DB_MODE_NULL );

		//---------------------------Call for x12-------------------------------------------
        ocrGuid_t LCSGuidTmp_X12_x21;
		ocrEdtTemplateCreate(&LCSGuidTmp_X12_x21, recLCSEdt, sizeof(p1)/sizeof(u64) , 1 );

		ocrGuid_t x12Edt, x12outputEventGuid, x12outputEventGuidOnce;
		ocrEventCreate(&x12outputEventGuid, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);

	    p1.xi = p->xi + nn, p1.xj = p->xj;

		ocrEdtCreate(&x12Edt,
					  LCSGuidTmp_X12_x21,
					  EDT_PARAM_DEF,
					  (u64*) &p1,
					  EDT_PARAM_DEF,
					  &x11outputEventGuid,
					  EDT_PROP_FINISH,
					  quadHint(&quadHNT, p1.N, base, p1.xi, p1.xj),
					  &x12outputEventGuidOnce);

	    ocrAddDependence( x12outputEventGuidOnce, x12outputEventGuid, 0, DB_MODE_NULL );

		//---------------------------Call for x21-------------------------------------------
		ocrGuid_t x21Edt, x21outputEventGuid, x21outputEventGuidOnce;
		ocrEventCreate(&x21outputEventGuid, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);

	    p1.xi = p->xi, p1.xj = p->xj+nn;

		ocrEdtCreate(&x21Edt,
					  LCSGuidTmp_X12_x21,
					  EDT_PARAM_DEF,
					  (u64*) &p1,
					  EDT_PARAM_DEF,
					  &x11outputEventGuid,
					  EDT_PROP_FINISH,
					  quadHint(&quadHNT, p1.N, base, p1.xi, p1.xj),
					  &x21outputEventGuidOnce);

		ocrAddDependence( x21outputEventGuidOnce, x21outputEventGuid, 0, DB_MODE_NULL );

		//---------------------------Call for x22-------------------------------------------

		ocrGuid_t LCSGuidTmp_X22;
		ocrEdtTemplateCreate(&LCSGuidTmp_X22, recLCSEdt, sizeof(p1)/sizeof(u64) , 2 );

		ocrGuid_t x22Edt, x22outputEventGuid;
		ocrEventCreate(&x22outputEventGuid, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);

	    p1.xi = p->xi + nn, p1.xj = p->xj+nn;

		ocrEdtCreate(&x22Edt,
					  LCSGuidTmp_X22,
					  EDT_PARAM_DEF,
					  (u64*) &p1,
					  EDT_PARAM_DEF,
					  NULL,
					  EDT_PROP_FINISH,
					  quadHint(&quadHNT, p1.N, base, p1.xi, p1.xj),
					  &x22outputEventGuid);


		ocrAddDependence(x21outputEventGuid, x22Edt, 0, DB_DEFAULT_MODE);
		ocrAddDependence(x12outputEventGuid, x22Edt, 1, DB_DEFAULT_MODE);

    }
	return NULL_GUID;
}



ocrGuid_t wrapupEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    long *score = depv[1].ptr;
	long N = (long)paramv[0];

	long end = cilk_getticks();
	ocrPrintf("runtime: %f\n",cilk_ticks_to_seconds(end-(long)paramv[1]));

	ocrPrintf("LCS length: %ld\n", score[N]);
	ocrPrintf("LCS checksum: %lu\n",
		  (unsigned long)lcsChecksum(score, 2 * N + 1));

    return NULL_GUID;
}

/* Shutdown in its own task, gated on the wrapup's completion, so the measured
 * run never truncates the wrapup's release work. */
ocrGuid_t shutDownEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrPrintf("\nShutting down OCR runtime\n");
	ocrShutdown();
    return NULL_GUID;
}

typedef struct {
      long len, base, tile;
} LCS_fill_params;

typedef struct {
      long N, base;
	  ocrGuid_t s_labels, t_labels;
} LCS_init_params;

/* Each string tile is written by its own task through an RW dependence.  The
 * content is a pure function of (tile index, position), so every tier and
 * every node count builds the same instance without a shared generator
 * state; tile 0 carries the kernel's leading boundary sentinel. */
ocrGuid_t fillStringEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    LCS_fill_params* p = (LCS_fill_params *)paramv;
	int *X = depv[0].ptr;
	long len = p->len;

	if(len == p->base + 1)
	{
		X[0] = 32;
		X++;
		len--;
	}

	for(long l = 0; l < len; l++)
	{
		X[l] = lcsChar((u64)p->tile, (u64)l);
	}
    return NULL_GUID;
}

ocrGuid_t initEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    LCS_init_params* p = (LCS_init_params *)paramv;
	long num_labels = p->N / p->base;

	ocrGuid_t fill_template;
	ocrEdtTemplateCreate(&fill_template, fillStringEdt,
			     sizeof(LCS_fill_params)/sizeof(u64), 1);

	LCS_fill_params fp;
	fp.base = p->base;

	for(long i = 0; i < num_labels; i++)
	{
		fp.len = (i == 0) ? p->base + 1 : p->base;

		ocrGuid_t tile, fillEdt;
		int *unused;
		ocrHint_t fillHNT;

		// NO_ACQUIRE: the creating task never touches the block, so it is
		// born at its labeled home instead of on the creating rank.
		ocrGuidFromIndex(&tile, p->t_labels, i);
		ocrDbCreate(&tile, (void**) &unused, sizeof(int) * fp.len,
			    GUID_PROP_IS_LABELED | DB_PROP_NO_ACQUIRE, NULL_HINT, NO_ALLOC);
		fp.tile = i;
		ocrEdtCreate(&fillEdt, fill_template, EDT_PARAM_DEF, (u64*) &fp,
			     EDT_PARAM_DEF, NULL, EDT_PROP_NONE,
			     tileHint(&fillHNT, i), NULL);
		ocrAddDependence( tile, fillEdt, 0, DB_MODE_RW );

		ocrGuidFromIndex(&tile, p->s_labels, i);
		ocrDbCreate(&tile, (void**) &unused, sizeof(int) * fp.len,
			    GUID_PROP_IS_LABELED | DB_PROP_NO_ACQUIRE, NULL_HINT, NO_ALLOC);
		fp.tile = num_labels + i;
		ocrEdtCreate(&fillEdt, fill_template, EDT_PARAM_DEF, (u64*) &fp,
			     EDT_PARAM_DEF, NULL, EDT_PROP_NONE,
			     tileHint(&fillHNT, i), NULL);
		ocrAddDependence( tile, fillEdt, 0, DB_MODE_RW );
	}

    return NULL_GUID;
}
//----------------------------------------------------- Main EDT --------------------------//
ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[] )
{
	// Collect command line arguments.
    ocrGuid_t DBK_cmdLineArgs = depv[0].guid;
    void * PTR_cmdLineArgs = depv[0].ptr;
	u32 argc = ocrGetArgc(PTR_cmdLineArgs);

	// Input params and default values.
    long N = 1024; // string length
    long base = 256;	// basecase

	if(argc != 3)
	{
		ocrPrintf("USAGE: please run the program as follows:\n"
		     "executable input_string_length basecase\n");
		ocrShutdown();
		return NULL_GUID;
	}

	N = (long) atol(ocrGetArgv(PTR_cmdLineArgs, 1));
	base = (long) atol(ocrGetArgv(PTR_cmdLineArgs, 2));

	// base can not be > N.
    if(N > 0 && N < base) base = N;

	// The recursion halves the problem until it is <= base, so a base case is
	// exactly `base` wide -- and so covers exactly one string tile -- only
	// when the ratio is a power of two; any other ratio silently drops the
	// tail rows and columns.
	if(N <= 0 || base <= 0 || N % base != 0 || ((N / base) & (N / base - 1)) != 0)
	{
		ocrPrintf("input_string_length (%ld) / basecase (%ld) must be a power of two\n",
			  N, base);
		ocrShutdown();
		return NULL_GUID;
	}

    ocrPrintf("\n");
    ocrPrintf("Running LCS.\nStrings len: %ld  basecase: %ld\n", N, base);

    s64 Nplus1 = N + 1;


	////////////////////////Initialize score ///////////////////////

	ocrGuid_t score_guid;
	s64 *lcs_score;

	// Create the data block with the guid
	ocrDbCreate(&score_guid,
		       (void**) &lcs_score,
				sizeof(long) * 2 * Nplus1,
				GUID_PROP_NONE,
				NULL_HINT,
				NO_ALLOC
				);

	for ( int i = 0; i < Nplus1; i++ )
    {
        lcs_score[N - i] = i;
    }
    for ( int j = 0; j < Nplus1; j++ )
    {
        lcs_score[N + j] = j;
    }

	// One labeled tile per base case, so a leaf's string reads are a whole
	// tile.  The tiles are created and filled by their own tasks below.
	s64 num_labels = N / base;

	ocrGuid_t s_labels, t_labels;
	ocrGuidRangeCreate(&s_labels, num_labels, GUID_USER_DB);
    ocrGuidRangeCreate(&t_labels, num_labels, GUID_USER_DB);

	LCS_init_params ip;
	ip.N = N;
	ip.base = base;
	ip.s_labels = s_labels;
	ip.t_labels = t_labels;

	ocrGuid_t init_tmp;
	ocrEdtTemplateCreate(&init_tmp, initEdt, sizeof(ip)/sizeof(u64), 1);

	ocrGuid_t initEdtGuid, initEventGuid, initEventGuidOnce;
	ocrEventCreate(&initEventGuid, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);

	ocrEdtCreate(&initEdtGuid,
		     init_tmp,
		     EDT_PARAM_DEF,
		     (u64*) &ip,
		     EDT_PARAM_DEF,
		     NULL,
		     EDT_PROP_FINISH,
		     NULL_HINT,
		     &initEventGuidOnce);

	ocrAddDependence( initEventGuidOnce, initEventGuid, 0, DB_MODE_NULL );
	ocrAddDependence( NULL_GUID, initEdtGuid, 0, DB_MODE_NULL );

	// Print values for debug purpose
	//Create a new finish EDT
	//params are N, base, xi, xj, and the guids.

	LCS_task_params p;

	p.N = N;
	p.base = base;
	p.xi = 1;
	p.xj = 1;
	p.n = N;
	p.s_guid = s_labels;
	p.t_guid = t_labels;
	p.score_guid = score_guid;

	unsigned long start;
	start = cilk_getticks();

	// Create the task template
	ocrGuid_t recLCSGuid;
    ocrEdtTemplateCreate(&recLCSGuid, recLCSEdt, sizeof(p)/sizeof(u64) , 1 );

	// Create the task
	ocrGuid_t recLCSEdt, outputEventGuid, outputEventGuidOnce;
	ocrEventCreate(&outputEventGuid, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);

	ocrHint_t rootHNT;

    ocrEdtCreate(&recLCSEdt,
				  recLCSGuid,
				  EDT_PARAM_DEF,
				  (u64*) &p,
				  EDT_PARAM_DEF,
				  NULL,
                  EDT_PROP_FINISH,
				  quadHint(&rootHNT, p.N, base, p.xi, p.xj),
				  &outputEventGuidOnce);

	ocrAddDependence( outputEventGuidOnce, outputEventGuid, 0, DB_MODE_NULL );
	ocrAddDependence( initEventGuid, recLCSEdt, 0, DB_MODE_NULL );

	// Create the task template
	ocrGuid_t wrapup_tmp;

	u64 paramv2[2];
	paramv2[0]=N;
	paramv2[1]=start;
    ocrEdtTemplateCreate(&wrapup_tmp, wrapupEdt, sizeof(paramv2)/sizeof(u64) , 2);

	// Create the task
	ocrGuid_t wrapup_taskGuid, wrapupEventGuid;
    ocrEdtCreate(&wrapup_taskGuid,
				  wrapup_tmp,
				  EDT_PARAM_DEF,
				  (u64*) paramv2,
				  EDT_PARAM_DEF,
				  NULL,
                  EDT_PROP_NONE,
				  NULL_HINT,
				  &wrapupEventGuid);


    ocrAddDependence(outputEventGuid, wrapup_taskGuid, 0, DB_MODE_NULL );
	ocrAddDependence(score_guid, wrapup_taskGuid, 1, DB_MODE_RO );

	ocrGuid_t shutDown_tmp, shutDown_taskGuid;
    ocrEdtTemplateCreate(&shutDown_tmp, shutDownEdt, 0 , 1);
    ocrEdtCreate(&shutDown_taskGuid,
				  shutDown_tmp,
				  EDT_PARAM_DEF,
				  NULL,
				  EDT_PARAM_DEF,
				  NULL,
                  EDT_PROP_NONE,
				  NULL_HINT,
				  NULL);
	ocrAddDependence(wrapupEventGuid, shutDown_taskGuid, 0, DB_MODE_NULL );

	return NULL_GUID;
}
