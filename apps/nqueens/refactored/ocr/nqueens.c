// \file nqueens.c
// \author Jorge Bellon <jorge.bellon.castro@intel.com>
//

#include "nqueens.h"

#include <ocr.h>
#include <extensions/ocr-affinity.h>
#include <stdlib.h>

/* Top tree levels (queens placed) whose partial-board EDTs are round-robin
 * distributed across policy domains; deeper levels pin to the creating rank so
 * each independent subtree runs wire-free. */
/* How many levels of the search tree get scattered across ranks before the
 * subtree is pinned to whatever rank it landed on.  Too few and the ranks
 * starve; too many and every level pays a migration the subtree would have
 * amortised locally.  The value is calibrated by measurement, not derived,
 * so it is an argument; this is only the default when it is absent. */
#ifndef NQUEENS_RR_LEVELS
#define NQUEENS_RR_LEVELS 3
#endif

struct nqueens_sum_args
{
    ocrGuid_t parent_event;
};


/* Finalizer-style bit mix: the column bitmask's low bits are semantically
 * special (column 0 is the only odd single-bit residue), so a raw modulus is
 * parity-biased toward one rank — mixing first makes the residue uniform
 * across sibling subtrees. */
static inline u64 mixKey( u64 x )
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/* Pin the summing continuation to the creating rank. */
static ocrHint_t * nqLocalEdtHint( ocrHint_t * h )
{
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
    ocrGuid_t aff;
    ocrAffinityGetCurrent( &aff );
    ocrHintInit( h, OCR_HINT_EDT_T );
    ocrSetHintValue( h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue( aff ) );
    return h;
#else
    (void)h;
    return NULL_HINT;
#endif
}

/* Static placement: the column bitmask is a distinct key per subtree, and
 * its set-bit count is the number of queens placed (the tree level). */
static ocrHint_t * nqPlaceEdtHint( ocrHint_t * h, u64 cols, u32 rr_levels )
{
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
    u64 nranks;
    ocrAffinityCount( AFFINITY_PD, &nranks );
    ocrGuid_t aff;
    if( NumberOfSetBits( cols ) < rr_levels )
        ocrAffinityGetAt( AFFINITY_PD, mixKey( cols ) % nranks, &aff );
    else
        ocrAffinityGetCurrent( &aff );
    ocrHintInit( h, OCR_HINT_EDT_T );
    ocrSetHintValue( h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue( aff ) );
    return h;
#else
    (void)h; (void)cols; (void)rr_levels;
    return NULL_HINT;
#endif
}

static inline void create_task( struct nqueens_args* args )
{
    u32 paramc = sizeof(struct nqueens_args)/sizeof(u64)+1;
    u64* paramv = (u64*)args;

    u8 err;
    ocrGuid_t edt;
    ocrHint_t hint;
    err  = ocrEdtCreate( &edt, args->find_template, paramc, paramv, 0, NULL,
              EDT_PROP_NONE, nqPlaceEdtHint(&hint, args->cols, args->rr_levels), NULL );
    ocrAssert( err == 0 );
}

/* Deliver a subtree's solution count upward: an 8-byte block carried on the
 * parent's completion event is the scalar return channel. */
static void return_count( ocrGuid_t parent_event, u64 count )
{
    ocrGuid_t db;
    u64* ptr;
    u8 err = ocrDbCreate( &db, (void**)&ptr, sizeof(u64), DB_PROP_NONE, NULL_HINT, NO_ALLOC );
    ocrAssert( err == 0 );
    *ptr = count;
    ocrDbRelease( db );
    ocrEventSatisfy( parent_event, db );
}

/* Below the spawn cutoff the subtree is searched sequentially in-process. */
static u64 count_solutions_seq( const struct nqueens_args* args )
{
    if( args->cols == args->all ) return 1;

    u64 count = 0;
    u32 available = ~( args->ldiag | args->cols | args->rdiag ) & args->all;
    u32 spot = available & (-available);

    struct nqueens_args arguments = *args;
    while( spot != 0 ) {
        arguments.ldiag = (args->ldiag|spot)<<1;
        arguments.cols  = (args->cols |spot);
        arguments.rdiag = (args->rdiag|spot)>>1;

        count += count_solutions_seq( &arguments );

        available = available - spot;
        spot = available & (-available);
    }
    return count;
}

/* Continuation of a spawning task: sums the children's returned counts and
 * forwards the total upward — the recursion's return path is the reduction
 * tree. */
ocrGuid_t sumSolutionsEdt( u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[] )
{
    const struct nqueens_sum_args* args = (const struct nqueens_sum_args*)paramv;

    u64 total = 0;
    u32 i;
    for( i = 0; i < depc; i++ ) {
        total += *(u64*)depv[i].ptr;
        ocrDbDestroy( depv[i].guid );
    }
    return_count( args->parent_event, total );
    return NULL_GUID;
}

// Find solutions: recursive EDT
ocrGuid_t findSolutionsEdt( u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[] )
{
    // Decode EDT paramv
    const struct nqueens_args* args = (const struct nqueens_args*)paramv;
    const u8 final = NumberOfSetBits(args->cols) > args->max_set;

    if( final ) {
        return_count( args->parent_event, count_solutions_seq( args ) );
        return NULL_GUID;
    }
    if( args->cols == args->all ) {
        return_count( args->parent_event, 1 );
        return NULL_GUID;
    }

    u32 available = ~( args->ldiag | args->cols | args->rdiag ) & args->all;
    u32 nchildren = NumberOfSetBits( available );
    if( nchildren == 0 ) {
        /* dead end: no legal column remains */
        return_count( args->parent_event, 0 );
        return NULL_GUID;
    }

    u8 err;
    ocrGuid_t sum_template, sum_edt;
    struct nqueens_sum_args sum_args = { .parent_event = args->parent_event };
    err = ocrEdtTemplateCreate( &sum_template, sumSolutionsEdt,
                          sizeof(struct nqueens_sum_args)/sizeof(u64)+1, nchildren );
    ocrAssert( err == 0 );
    {
        ocrHint_t hint;
        err = ocrEdtCreate( &sum_edt, sum_template,
                  EDT_PARAM_DEF, (u64*)&sum_args, EDT_PARAM_DEF, NULL,
                  EDT_PROP_NONE, nqLocalEdtHint(&hint), NULL );
        ocrAssert( err == 0 );
    }
    err = ocrEdtTemplateDestroy( sum_template );
    ocrAssert( err == 0 );

    struct nqueens_args arguments;
    arguments.find_template = args->find_template;
    arguments.all = args->all;
    arguments.max_set = args->max_set;
    arguments.rr_levels = args->rr_levels;

    u32 spot = available & (-available);
    u32 slot = 0;
    while( spot != 0 ) {
        arguments.ldiag   = (args->ldiag|spot)<<1;
        arguments.cols    = (args->cols |spot);
        arguments.rdiag   = (args->rdiag|spot)>>1;

        /* wire the child's return event into the summer before the child
         * exists, so it cannot fire unregistered */
        ocrGuid_t child_done;
        err = ocrEventCreate( &child_done, OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG );
        ocrAssert( err == 0 );
        err = ocrAddDependence( child_done, sum_edt, slot, DB_MODE_RO );
        ocrAssert( err == 0 );

        arguments.parent_event = child_done;
        create_task( &arguments );

        slot++;
        available = available - spot;
        spot = available & (-available);
    }

    return NULL_GUID;
}

void solve_nqueens( u32 n, u32 cutoff, u32 rounds_left, u32 rr_levels,
                    ocrGuid_t find_template, ocrGuid_t shutdown_template )
{
    u8 err;
    ocrGuid_t rootDone;
    ocrGuid_t shutdownEdt;
    struct nqueens_args app_args = {
        .find_template = find_template,
        .max_set = n-cutoff, .all = (1 << n) - 1,
        .ldiag = 0, .cols = 0, .rdiag = 0, .rr_levels = rr_levels };
    struct shutdown_args shutdown_args = {
        .find_template = find_template, .shutdown_template = shutdown_template,
        .n = n, .cutoff = cutoff, .rounds_left = rounds_left,
        .rr_levels = rr_levels };

    get_time(&shutdown_args.start);

    err = ocrEventCreate( &rootDone, OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG );
    ocrAssert( err == 0 );

    err = ocrEdtCreate( &shutdownEdt, shutdown_template,
                  sizeof(shutdown_args)/sizeof(u64)+1, (u64*)&shutdown_args,
                  1, &rootDone,
                  EDT_PROP_NONE, NULL_HINT, NULL );
    ocrAssert( err == 0 );

    app_args.parent_event = rootDone;
    create_task( &app_args );
}

/* Renamed from 'shutdown' to avoid POSIX shutdown(2) symbol collision
 * that crashes Open MPI 5.x PMIx during MPI_Finalize. */
ocrGuid_t shutdownEdt( u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[] )
{
    struct shutdown_args* args = (struct shutdown_args*)paramv;
    u64 found = *(u64*)depv[0].ptr;
    ocrDbDestroy( depv[0].guid );

    if( args->rounds_left > 1 ) {
        solve_nqueens( args->n, args->cutoff, args->rounds_left - 1,
                       args->rr_levels,
                       args->find_template, args->shutdown_template );
        return NULL_GUID;
    }
    timestamp_t stop;
    get_time(&stop);

    /* the count exceeds 32 bits from n=19 on */
    ocrPrintf( "%d-queens; %dx%d; sols: %lu\n",
            args->n, args->n, args->n, found );
    summary_throughput_timer(&args->start,&stop,1);

    u8 err;
    err = ocrEdtTemplateDestroy( args->find_template );
    ocrAssert( err == 0 );

    err = ocrEdtTemplateDestroy( args->shutdown_template );
    ocrAssert( err == 0 );

    ocrShutdown();

    return NULL_GUID;
}

ocrGuid_t mainEdt ( u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[] )
{
    u64 argc = ocrGetArgc(depv[0].ptr);
    if( argc < 3 || argc > 5 ) {
        ocrPrintf("Usage %s size cutoff [rounds [scatter-levels]]",
                  ocrGetArgv(depv[0].ptr,0) );
        ocrAbort(EXIT_FAILURE);
    }

    u32 n = atoi( ocrGetArgv(depv[0].ptr,1) );
    u32 cutoff = atoi( ocrGetArgv(depv[0].ptr,2) );
    u32 rounds = 1;
    if( argc >= 4 ) {
        u32 r = atoi( ocrGetArgv(depv[0].ptr,3) );
        if( r >= 1 ) rounds = r;
    }
    u32 rr_levels = NQUEENS_RR_LEVELS;
    if( argc == 5 )
        rr_levels = atoi( ocrGetArgv(depv[0].ptr,4) );
    ocrAssert( 0 < n && n < 31 );
    ocrAssert( cutoff < n );

    u8 err;
    ocrGuid_t find_template, shutdown_template;
    err = ocrEdtTemplateCreate( &find_template, findSolutionsEdt,
                          sizeof(struct nqueens_args)/sizeof(u64)+1, 0 );
    ocrAssert( err == 0 );

    err = ocrEdtTemplateCreate( &shutdown_template, shutdownEdt,
                          sizeof(struct shutdown_args)/sizeof(u64)+1, 1 );
    ocrAssert( err == 0 );

    solve_nqueens( n, cutoff, rounds, rr_levels, find_template, shutdown_template );
    return NULL_GUID;
}

