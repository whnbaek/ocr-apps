// \file nqueens.c
// \author Jorge Bellon <jorge.bellon.castro@intel.com>
//

#include "nqueens.h"

#include <ocr.h>
#include <extensions/ocr-affinity.h>
#include <stdlib.h>

/* Default for the scatter argument: a partial-board EDT with fewer queens
 * placed than this is spread across policy domains by key, and one at or
 * beyond it stays on its creating rank.  Too small and the deep majority of
 * the tree is confined to whichever rank its ancestor landed on; too large and
 * every level pays a migration the subtree would have amortised locally. */
#ifndef NQUEENS_RR_LEVELS
#define NQUEENS_RR_LEVELS 3
#endif

/* A scatter depth below this places only the empty board, so every descendant
 * pins to the rank the root landed on and the search never leaves it. */
#define NQUEENS_MIN_RR_LEVELS 2

struct nqueens_sum_args
{
    ocrGuid_t parent_event;
};


/* Finalizer-style bit mix: sibling boards differ in a few bits, so a raw
 * modulus of a board mask is biased toward one rank — mixing first makes the
 * residue uniform across siblings. */
static inline u64 mixKey( u64 x )
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/* A partial board is the triple (cols, ldiag, rdiag), and the column mask
 * alone is an order-INDEPENDENT set: every permutation of one column set
 * shares it, so keying on it collapses all of them onto one rank.  The two
 * diagonal masks are order-dependent, so the triple is the canonical
 * identity of a subtree and all three have to enter the key. */
static inline u64 nqStateKey( const struct nqueens_args * args )
{
    return mixKey( mixKey( ((u64)args->ldiag << 32) | (u64)args->rdiag )
                   ^ (u64)args->cols );
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

/* Static placement: the set-bit count of the column mask is the number of
 * queens already placed (the tree level).  A task whose count is LESS than the
 * scatter depth is placed by its board's canonical key; at or beyond that depth
 * the task stays on the rank that created it, so the subtree below runs where
 * its root landed.  The whole board state is passed so the key can use all of
 * it. */
static ocrHint_t * nqPlaceEdtHint( ocrHint_t * h, const struct nqueens_args * args )
{
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
    u64 nranks;
    ocrAffinityCount( AFFINITY_PD, &nranks );
    ocrGuid_t aff;
    if( NumberOfSetBits( args->cols ) < args->rr_levels )
        ocrAffinityGetAt( AFFINITY_PD, nqStateKey( args ) % nranks, &aff );
    else
        ocrAffinityGetCurrent( &aff );
    ocrHintInit( h, OCR_HINT_EDT_T );
    ocrSetHintValue( h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue( aff ) );
    return h;
#else
    (void)h; (void)args;
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
              EDT_PROP_NONE, nqPlaceEdtHint(&hint, args), NULL );
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

/* An out-of-range argument is rejected, never clamped: a silently corrected
 * value makes a run report on a workload nobody asked for. */
static void nqReject( void * argsPtr, const char * problem )
{
    ocrPrintf( "nqueens: %s\nUsage: %s size cutoff [rounds [scatter-levels]]\n",
               problem, ocrGetArgv( argsPtr, 0 ) );
    ocrAbort( EXIT_FAILURE );
}

ocrGuid_t mainEdt ( u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[] )
{
    u64 argc = ocrGetArgc(depv[0].ptr);
    if( argc < 3 || argc > 5 )
        nqReject( depv[0].ptr, "wrong number of arguments" );

    /* signed while they are checked, so a negative argument is rejected
     * instead of wrapping into a plausible unsigned value */
    int n_arg = atoi( ocrGetArgv(depv[0].ptr,1) );
    int cutoff_arg = atoi( ocrGetArgv(depv[0].ptr,2) );
    int rounds_arg = 1;
    if( argc >= 4 )
        rounds_arg = atoi( ocrGetArgv(depv[0].ptr,3) );
    int rr_arg = NQUEENS_RR_LEVELS;
    if( argc == 5 )
        rr_arg = atoi( ocrGetArgv(depv[0].ptr,4) );

    if( n_arg < 1 || n_arg > 30 )
        nqReject( depv[0].ptr, "size must be in [1,30]" );
    if( cutoff_arg < 0 || cutoff_arg >= n_arg )
        nqReject( depv[0].ptr, "cutoff must be in [0,size)" );
    if( rounds_arg < 1 )
        nqReject( depv[0].ptr, "rounds must be at least 1" );
    if( rr_arg < NQUEENS_MIN_RR_LEVELS )
        nqReject( depv[0].ptr,
                  "scatter-levels must be at least 2, or every subtree pins"
                  " to the rank the root landed on and one rank runs the"
                  " whole search" );

    u32 n = (u32)n_arg;
    u32 cutoff = (u32)cutoff_arg;
    u32 rounds = (u32)rounds_arg;
    u32 rr_levels = (u32)rr_arg;

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

