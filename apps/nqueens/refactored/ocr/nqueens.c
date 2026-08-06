// \file nqueens.c
// \author Jorge Bellon <jorge.bellon.castro@intel.com>
//

#include "nqueens.h"

#include <ocr.h>
#include <stdlib.h>

struct nqueens_sum_args
{
    ocrGuid_t parent_event;
};

static inline void create_task( struct nqueens_args* args )
{
    u32 paramc = sizeof(struct nqueens_args)/sizeof(u64)+1;
    u64* paramv = (u64*)args;

    u8 err;
    ocrGuid_t edt;
    err  = ocrEdtCreate( &edt, args->find_template, paramc, paramv, 0, NULL,
              EDT_PROP_NONE, NULL_HINT, NULL );
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
    err = ocrEdtCreate( &sum_edt, sum_template,
              EDT_PARAM_DEF, (u64*)&sum_args, EDT_PARAM_DEF, NULL,
              EDT_PROP_NONE, NULL_HINT, NULL );
    ocrAssert( err == 0 );
    err = ocrEdtTemplateDestroy( sum_template );
    ocrAssert( err == 0 );

    struct nqueens_args arguments;
    arguments.find_template = args->find_template;
    arguments.all = args->all;
    arguments.max_set = args->max_set;

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

void solve_nqueens( u32 n, u32 cutoff, u32 rounds_left,
                    ocrGuid_t find_template, ocrGuid_t shutdown_template )
{
    u8 err;
    ocrGuid_t rootDone;
    ocrGuid_t shutdownEdt;
    struct nqueens_args app_args = {
        .find_template = find_template,
        .max_set = n-cutoff, .all = (1 << n) - 1,
        .ldiag = 0, .cols = 0, .rdiag = 0 };
    struct shutdown_args shutdown_args = {
        .find_template = find_template, .shutdown_template = shutdown_template,
        .n = n, .cutoff = cutoff, .rounds_left = rounds_left };

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
    u32 found = (u32)*(u64*)depv[0].ptr;
    ocrDbDestroy( depv[0].guid );

    if( args->rounds_left > 1 ) {
        solve_nqueens( args->n, args->cutoff, args->rounds_left - 1,
                       args->find_template, args->shutdown_template );
        return NULL_GUID;
    }
    timestamp_t stop;
    get_time(&stop);

    ocrPrintf( "%d-queens; %dx%d; sols: %d\n",
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
    if( ocrGetArgc(depv[0].ptr) != 3 ) {
        ocrPrintf("Usage %s size cutoff", ocrGetArgv(depv[0].ptr,0) );
        ocrAbort(EXIT_FAILURE);
    }

    u32 n = atoi( ocrGetArgv(depv[0].ptr,1) );
    u32 cutoff = atoi( ocrGetArgv(depv[0].ptr,2) );
    u32 rounds = 1;
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

    solve_nqueens( n, cutoff, rounds, find_template, shutdown_template );
    return NULL_GUID;
}

