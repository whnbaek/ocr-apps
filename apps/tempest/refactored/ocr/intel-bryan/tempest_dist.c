/*
 *Author: Bryan Pawlowski
 *Copywrite Intel Corporation 2015
 *
 *This file is subject to the license agreement located in the file ../../../../LICENSE (apps/LICENSE)
 *and cannot be distributed without it. This notice cannot be removed or modified.
 */

/*
 * Restructured neighbour exchange: the same cube-sphere decomposition, the same
 * neighbour topology and the same cross-check, with the halo expressed per RANK
 * PAIR rather than per patch edge.
 *
 * The port this replaces gives every patch edge its own datablock and bounces
 * it between the two patches: a timestep acquires it RW on one side, stamps
 * eight bytes into it and hands it back, so a boundary edge migrates exclusive
 * ownership across the rank line twice per timestep.  Placement already makes
 * almost every edge rank-local -- the crossing edges are under half a percent
 * -- but what does cross is one message per edge per timestep, and each of
 * those carries eight bytes of payload behind a few hundred bytes of protocol.
 *
 * Here each rank keeps its own patches' inbound stamps in plain memory and
 * batches everything bound for one peer into a single block per timestep.  The
 * same stamps cross, the same number of times; they cross together.
 */

#ifndef ENABLE_EXTENSION_LABELING
#define ENABLE_EXTENSION_LABELING
#endif

#include <ocr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ocr-std.h>
#include <extensions/ocr-labeling.h>
#include <extensions/ocr-affinity.h>

#ifndef TEST_PATCH
#define TEST_PATCH 0
#endif

#ifndef DURATION
#define DURATION 100
#endif

#define N   0
#define E   1
#define S   2
#define W   3
#define NE  4
#define SE  5
#define SW  6
#define NW  7


/*for testing purposes, I won't worry about the grid size within each patch for now.*/

u64 findY( u64 localPatchNum, u64 k )
{
    u64 y;

    y = localPatchNum % k;

    return y;
}

u64 findX( u64 localPatchNum, u64 k )
{
    u64 x;

    x = localPatchNum / k;

    return x;
}

u32 findNeighborDir( u64 myPanelNum, u64 neighPanelNum )
{
    u32 dir;

                                  //  N, E, S, W      // panels
    u32 panelNeighbors [6][4] =   { { 4, 1, 5, 3 },   // 0
                                    { 4, 2, 5, 0 },   // 1
                                    { 4, 3, 5, 1 },   // 2
                                    { 4, 0, 5, 2 },   // 3
                                    { 3, 2, 1, 0 },   // 4
                                    { 1, 2, 3, 0 } }; // 5

    for( dir = 0; dir < 4; dir++ ){
        if( panelNeighbors[neighPanelNum][dir] != myPanelNum ) continue;
        else break;
    }

    return dir;
}

u64 findLocalPatchNum( u64 patchNum, u64 k )
{
    u64 localPatchNum;

    localPatchNum = patchNum % (k * k);

    return localPatchNum;
}

u64 findPanelNum( u64 patchNum, u64 k )
{
    u64 panelNum, difference, panelStart;

    difference = findLocalPatchNum( patchNum, k );

    panelStart = patchNum - difference;

    panelNum = panelStart / (k * k);

    return panelNum;
}

u64 findNeighborPanel( u64 panel, u32 dir )
{

    u64 neighborPanel = 4;

    if( dir >= 4 ) ocrPrintf("it is not possible to have a neighboring panel on the diagonal!\n");
    else{
                                      //  N, E, S, W      // panels
        u32 panelNeighbors [6][4] =   { { 4, 1, 5, 3 },   // 0
                                        { 4, 2, 5, 0 },   // 1
                                        { 4, 3, 5, 1 },   // 2
                                        { 4, 0, 5, 2 },   // 3
                                        { 3, 2, 1, 0 },   // 4
                                        { 1, 2, 3, 0 } }; // 5
        neighborPanel = panelNeighbors[panel][dir];
    }
    return neighborPanel;
}

s64 findNeighborPatch( u64 patchNum, u64 k, u32 dir )
{

    u64 neighbor = 1999;
    u64 localPatchNum, panelNum, x, y, neighborPanel;

    panelNum = findPanelNum( patchNum, k );
    localPatchNum = findLocalPatchNum( patchNum, k );

    x = findX( localPatchNum, k );
    y = findY( localPatchNum, k );

    switch(dir){
        case N:
            if( y == (k-1) ){
                neighborPanel = findNeighborPanel( panelNum, dir );
                u32 neighDir = findNeighborDir( panelNum, neighborPanel );
                u64 neighborX, neighborY;

                switch( neighDir ){
                    case N:
                        neighborX = ((k - 1) - x);
                        neighborY = (k - 1); //we know this because we are at our neighbor's top border.
                        break;
                    case E:
                        neighborX = k - 1;
                        neighborY = x;
                        break;
                    case S:
                        neighborX = x;
                        neighborY = 0;
                        break;
                    case W:
                        neighborX = 0;
                        neighborY = (k - 1) - x;
                        break;
                    default:
                        break;
                }
                neighbor = (neighborPanel * ( k * k )) + (k * neighborX) + neighborY;

            }else{
                neighbor = patchNum + 1;
            }
            break;
        case E:
            if( x == (k-1) ){
                neighborPanel = findNeighborPanel( panelNum, dir );
                u32 neighDir = findNeighborDir( panelNum, neighborPanel );
                u64 neighborX, neighborY;

                switch( neighDir ){
                    case N:
                        neighborX = y;
                        neighborY = k-1;
                        break;
                    case E:
                        break;  //with current setup, this case is not possible.
                    case S:
                        //THIS NEEDS FIXING TOMORROW
                        neighborX = (k - 1) - y;
                        neighborY = 0;
                        break;
                    case W:
                        neighborX = 0;
                        neighborY = y;
                        break;
                    default:
                        break;
                }
                neighbor = (neighborPanel * (k * k)) + (k * neighborX) + neighborY;
            }else{
                neighbor = patchNum + k;
            }
            break;
        case S:
            if( y == 0 ){
                neighborPanel = findNeighborPanel( panelNum, dir );
                u32 neighDir = findNeighborDir( panelNum, neighborPanel );
                u64 neighborX, neighborY;

                switch( neighDir ){
                    case N:
                        neighborX = x;
                        neighborY = k-1;
                        break;
                    case E:
                        neighborX = k-1;
                        neighborY = (k-1) - x;
                        break;
                    case S:
                        neighborX = (k-1) - x;
                        neighborY = 0;
                        break;
                    case W:
                        neighborX = 0;
                        neighborY = x;
                        break;
                    default:
                        break;
                }
                neighbor = (neighborPanel * (k * k)) + (k * neighborX) + neighborY;
            }else{
                neighbor = patchNum - 1;
            }

            break;
        case W:
            if( x == 0 ){
                neighborPanel = findNeighborPanel( panelNum, dir );
                u32 neighDir = findNeighborDir( panelNum, neighborPanel );
                u64 neighborX, neighborY;

                switch( neighDir ){
                    case N:
                        neighborX = (k-1) - y;
                        neighborY = k-1;
                        break;
                    case E:
                        neighborX = k-1;
                        neighborY = y;
                        break;
                    case S:
                        neighborX = y;
                        neighborY = 0;
                        break;
                    case W:                 // a west-to-west relationship is not possible with this layout.
                        break;
                    default:
                        break;
                }
                neighbor = (neighborPanel * (k * k)) + (k * neighborX) + neighborY;
            }else{
                neighbor = patchNum - k;
            }
            break;
        case NE:
            if( (x == (k-1)) && (y == (k-1)) ) return -1;
            else if( x == (k-1)){                                   //at eastern border.
                neighborPanel = findNeighborPanel( panelNum, E );   //can't use dir for non-cardinal cases.
                u32 neighDir = findNeighborDir( panelNum, neighborPanel );
                u64 neighborX, neighborY;

                switch( neighDir ){
                    case N:
                        neighborX = y+1;
                        neighborY = k-1;
                        break;
                    case E:             //an east-to-east relationship is not possible with this layout.
                        break;
                    case S:
                        neighborX = ((k-1) - y) - 1;
                        neighborY = 0;
                        break;
                    case W:
                        neighborX = 0;
                        neighborY = y + 1;
                        break;
                    default:
                        break;
                }
                neighbor = (neighborPanel * (k * k)) + (k * neighborX) + neighborY;
            }else if( y == (k-1)){
                neighborPanel = findNeighborPanel( panelNum, N );
                u32 neighDir = findNeighborDir( panelNum, neighborPanel );
                u64 neighborX, neighborY;

                switch( neighDir ){
                    case N:
                        neighborX = ((k-1) - x) - 1;
                        neighborY = k - 1;
                        break;
                    case E:
                        neighborX = k - 1;
                        neighborY = x + 1;
                        break;
                    case S:
                        neighborX = x + 1;
                        neighborY = 0;
                        break;
                    case W:
                        neighborX = 0;
                        neighborY = ((k-1) - x) - 1;
                        break;
                    default:
                        break;
                }
                neighbor = (neighborPanel * (k * k)) + (k * neighborX) + neighborY;
            }else{
                neighbor = patchNum + k + 1;
            }
            break;
        case SE:
            if( (x == (k-1)) && (y == 0)) return -1; //no southeast neighbor in the southeast corner.
            else if( x == (k-1)){
                neighborPanel = findNeighborPanel( panelNum, E );
                u32 neighDir = findNeighborDir( panelNum, neighborPanel );
                u64 neighborX, neighborY;

                switch( neighDir ){
                    case N:
                        neighborX = y-1;
                        neighborY = k-1;
                        break;
                    case E:         //an east-to-east relationship is not possible with this layout.
                        break;
                    case S:
                        neighborX = ((k-1) - y) + 1;
                        neighborY = 0;
                        break;
                    case W:
                        neighborX = 0;
                        neighborY = y-1;
                        break;
                    default:
                        break;
                }
            neighbor = (neighborPanel * (k * k)) + (k * neighborX) + neighborY;
            }else if( y == 0 ){
                neighborPanel = findNeighborPanel( panelNum, S );
                u32 neighDir = findNeighborDir( panelNum, neighborPanel );
                u64 neighborX, neighborY;

                switch( neighDir ){
                    case N:
                        neighborX = x+1;
                        neighborY = k-1;
                        break;
                    case E:
                        neighborX = k-1;
                        neighborY = ((k-1) - x) - 1;
                        break;
                    case S:
                        neighborX = ((k-1) - x) - 1;
                        neighborY = 0;
                        break;
                    case W:
                        neighborX = 0;
                        neighborY = x + 1;
                        break;
                    default:
                        break;
                }
            neighbor = (neighborPanel * (k * k)) + (k * neighborX) + neighborY;
            }else{
                neighbor = patchNum + k - 1;
            }
            break;
        case SW:
            if( (x == 0) && (y == 0) ) return -1;
            else if( x == 0 ){      //we are at western border
                neighborPanel = findNeighborPanel( panelNum, W );
                u32 neighDir = findNeighborDir( panelNum, neighborPanel );
                u64 neighborX, neighborY;
                switch( neighDir ){
                    case N:
                        neighborX = k - y;
                        neighborY = k - 1;
                        break;
                    case E:
                        neighborX = k - 1;
                        neighborY = y - 1;
                        break;
                    case S:
                        neighborX = y - 1;
                        neighborY = 0;
                        break;
                    case W:     //a west-to-west relationship is not possible with this layout.
                        break;
                    default:
                        break;
                }
            neighbor = (neighborPanel * (k * k)) + (k * neighborX) + neighborY;
            }else if( y == 0 ){
                neighborPanel = findNeighborPanel( panelNum, S );
                u32 neighDir = findNeighborDir( panelNum, neighborPanel );
                u64 neighborX, neighborY;

                switch( neighDir ){
                    case N:
                        neighborX = x - 1;
                        neighborY = k - 1;
                        break;
                    case E:
                        neighborX = k - 1;
                        neighborY = k - x;
                        break;
                    case S:
                        neighborX = k - x;
                        neighborY = 0;
                        break;
                    case W:
                        neighborX = 0;
                        neighborY = x - 1;
                        break;
                    default:
                        break;
                }
            neighbor = (neighborPanel * (k * k)) + (k * neighborX) + neighborY;
            }else{
                neighbor = patchNum - k - 1;
            }
            break;
        case NW:
            if( (x == 0) && (y == (k - 1)) ) return -1;
            else if( x == 0 ){
                neighborPanel = findNeighborPanel( panelNum, W );
                u32 neighDir = findNeighborDir( panelNum, neighborPanel );
                u64 neighborX, neighborY;

                switch( neighDir ){
                    case N:
                        neighborX = (k - (y + 2));
                        neighborY = k - 1;
                        break;
                    case E:
                        neighborX = k - 1;
                        neighborY = y + 1;
                        break;
                    case S:
                        neighborX = y + 1;
                        neighborY = 0;
                        break;
                    case W:     //west-to-west relationship is not possible with this layout.
                        break;
                    default:
                        break;
                }
            neighbor = (neighborPanel * (k * k)) + (k * neighborX) + neighborY;
            }else if( y == (k - 1)){
                neighborPanel = findNeighborPanel( panelNum, N );
                u32 neighDir = findNeighborDir( panelNum, neighborPanel );
                u64 neighborX, neighborY;

                switch( neighDir ){
                    case N:
                        neighborX = k - x;
                        neighborY = k - 1;
                        break;
                    case E:
                        neighborX = k - 1;
                        neighborY = x - 1;
                        break;
                    case S:
                        neighborX = x - 1;
                        neighborY = 0;
                        break;
                    case W:
                        neighborX = 0;
                        neighborY = k - x;
                        break;
                    default:
                        break;
                }
            neighbor = (neighborPanel * (k * k)) + (k * neighborX) + neighborY;
            }else{
                neighbor = patchNum - k + 1;
            }
            break;
        default:
            break;
    }
    return neighbor;
}

/* The direction a stamp travels back along.  It is NOT the geometric opposite:
 * the cube sphere reverses orientation across some face seams, which is why
 * the port this replaces learns each reverse link by exchange at setup rather
 * than computing it.  Derive it from the topology instead -- the direction of
 * `me` as seen from `neighbour` is whichever of its eight points back. */
static u32 reverseDir( u64 me, u64 neighbour, u64 k )
{
    u32 e;
    for( e = 0; e < 8; e++ )
        if( findNeighborPatch( neighbour, k, e ) == (s64)me ) return e;
    return 8;                            /* no reverse link: nothing to deliver */
}

/* This tier changes ONE thing: what crosses a rank boundary.  The task
 * structure is the base program's -- one task per patch per timestep, so the
 * width follows the same dial and the two rows compare directly -- and each
 * task does the same eight stores.  What used to be one message per boundary
 * edge per timestep is one block per rank pair per timestep.
 *
 * Per rank per timestep: the patch tasks write their stamps into their own
 * persistent slice, a fan-in level concatenates the slices, and the rank's
 * single apply turns the result into memory writes for the patches it owns and
 * into one outgoing block per peer.  The fan-in is the shape of a reduction --
 * a block written by many tasks at once would serialise them, and an apply
 * that took every slice directly would carry one dependence per patch -- not a
 * limit on how many patches run at once. */
#define TEMPEST_GROUPS     512     /* fan-in arity cap; task count is unaffected */
#define TEMPEST_MAX_RANKS  64
#define TEMPEST_MAXGEN     2       /* generations in flight, as the base uses */

static u64 patchHomeRank( u64 patchNum, u64 k, u64 nranks )
{
    if( nranks < 6 )
        return ( patchNum * nranks ) / ( 6 * k * k );
    u64 P = 1, best = (u64)-1, p;
    for( p = 1; p <= nranks; ++p ){
        if( nranks % p ) continue;
        u64 q = nranks / p;
        u64 cut = ( p - 1 ) * 6 * k + ( q - 1 ) * k;
        if( cut < best ){ best = cut; P = p; }
    }
    u64 Q = nranks / P;
    u64 face = patchNum / ( k * k ), idx = patchNum % ( k * k );
    u64 R = idx / k, C = face * k + ( idx % k );
    return ( ( R * P ) / k ) * Q + ( C * Q ) / ( 6 * k );
}

static ocrHint_t * rankEdtHint( ocrHint_t * h, u64 rank )
{
    ocrGuid_t aff; ocrAffinityGetAt( AFFINITY_PD, rank, &aff );
    ocrHintInit( h, OCR_HINT_EDT_T );
    ocrSetHintValue( h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue( aff ) );
    return h;
}
static ocrHint_t * rankDbHint( ocrHint_t * h, u64 rank )
{
    ocrGuid_t aff; ocrAffinityGetAt( AFFINITY_PD, rank, &aff );
    ocrHintInit( h, OCR_HINT_DB_T );
    ocrSetHintValue( h, OCR_HINT_DB_AFFINITY, ocrAffinityToHintValue( aff ) );
    return h;
}

/* One stamp in flight: `from` belongs in direction `dir` of patch `to`.  The
 * triple travels rather than an agreed ordering, so the two sides need agree
 * on nothing beyond the geometry they both compute. */
typedef struct{ s64 to, dir, from; }xfer_t;

typedef struct{
    u64 k, duration, nranks, myRank, step, nlocal, ngroup, fanin;
    ocrGuid_t patchTPT, groupTPT, applyTPT;
    ocrGuid_t doneEvt;
    ocrGuid_t chan[TEMPEST_MAX_RANKS];
    ocrGuid_t peerChan[TEMPEST_MAX_RANKS];
    u64 peerCount[TEMPEST_MAX_RANKS];
    u64 peer[TEMPEST_MAX_RANKS];
    u64 npeers;
}rankState_t;

static u64 viewEntries( u64 k ){ return 6 * k * k * 8; }

/* One patch, one timestep: the same eight stores the base port's patch task
 * does, into this patch's own slice. */
ocrGuid_t patchEdt( u32 paramc, u64 * paramv, u32 depc, ocrEdtDep_t depv[] )
{
    rankState_t * st = (rankState_t *)depv[0].ptr;
    xfer_t * out = (xfer_t *)depv[1].ptr;
    u64 p = paramv[0], n = 0;
    u32 d;
    for( d = 0; d < 8; d++ ){
        s64 nb = findNeighborPatch( p, st->k, d );
        if( nb < 0 ) continue;
        u32 rev = reverseDir( p, (u64)nb, st->k );
        if( rev >= 8 ) continue;
        out[n].to = nb; out[n].dir = (s64)rev; out[n].from = (s64)p;
        n++;
    }
    out[n].to = -1;
    return NULL_GUID;
}

/* Fan-in: concatenate a group's slices so the rank's apply carries one
 * dependence per group rather than one per patch. */
ocrGuid_t groupEdt( u32 paramc, u64 * paramv, u32 depc, ocrEdtDep_t depv[] )
{
    xfer_t * out = (xfer_t *)depv[0].ptr;
    u64 n = 0, i, j;
    for( i = 1; i < depc; i++ ){
        xfer_t * s = (xfer_t *)depv[i].ptr;
        if( s == NULL ) continue;
        for( j = 0; s[j].to >= 0; j++ ) out[n++] = s[j];
    }
    out[n].to = -1;
    return NULL_GUID;
}

static void spawnTimestep( rankState_t * st, ocrGuid_t stDb, ocrGuid_t viewDb,
                           ocrGuid_t lpDb, ocrGuid_t sgDb, ocrGuid_t ggDb,
                           u64 * lp, ocrGuid_t * sliceG, ocrGuid_t * groupG )
{
    ocrHint_t eh;
    ocrGuid_t apply, t;
    u64 i, g;
    ocrEdtCreate( &apply, st->applyTPT, EDT_PARAM_DEF, NULL, EDT_PARAM_DEF, NULL,
                  EDT_PROP_NONE, rankEdtHint( &eh, st->myRank ), NULL );
    ocrAddDependence( stDb,   apply, 0, DB_MODE_RW );
    ocrAddDependence( viewDb, apply, 1, DB_MODE_RW );
    ocrAddDependence( lpDb,   apply, 2, DB_MODE_RO );
    ocrAddDependence( sgDb,   apply, 3, DB_MODE_RO );
    ocrAddDependence( ggDb,   apply, 4, DB_MODE_RO );

    /* one task per patch, as the base program has */
    for( i = 0; i < st->nlocal; i++ ){
        u64 prm[1] = { lp[i] };
        ocrEdtCreate( &t, st->patchTPT, EDT_PARAM_DEF, prm, EDT_PARAM_DEF, NULL,
                      EDT_PROP_NONE, rankEdtHint( &eh, st->myRank ), NULL );
        ocrAddDependence( stDb,      t, 0, DB_MODE_RO );
        ocrAddDependence( sliceG[i], t, 1, DB_MODE_RW );
    }
    /* fan-in, then the rank's single point of contact */
    for( g = 0; g < st->ngroup; g++ ){
        ocrEdtCreate( &t, st->groupTPT, EDT_PARAM_DEF, NULL, EDT_PARAM_DEF, NULL,
                      EDT_PROP_NONE, rankEdtHint( &eh, st->myRank ), NULL );
        ocrAddDependence( groupG[g], t, 0, DB_MODE_RW );
        for( i = 0; i < st->fanin; i++ ){
            u64 idx = g * st->fanin + i;
            ocrAddDependence( idx < st->nlocal ? sliceG[idx] : NULL_GUID,
                              t, (u32)( 1 + i ), DB_MODE_RO );
        }
        ocrAddDependence( groupG[g], apply, (u32)( 5 + g ), DB_MODE_RO );
    }
    for( i = 0; i < st->npeers; i++ )
        ocrAddDependence( st->chan[i], apply, (u32)( 5 + st->ngroup + i ), DB_MODE_RO );
}

/* The rank's single point of contact for a timestep: stamps for its own
 * patches become memory writes, everything else becomes one block per peer. */
ocrGuid_t applyEdt( u32 paramc, u64 * paramv, u32 depc, ocrEdtDep_t depv[] )
{
    rankState_t * st = (rankState_t *)depv[0].ptr;
    s64 * view       = (s64 *)depv[1].ptr;
    u64 * lp         = (u64 *)depv[2].ptr;
    ocrGuid_t * sliceG = (ocrGuid_t *)depv[3].ptr;
    ocrGuid_t * groupG = (ocrGuid_t *)depv[4].ptr;
    u64 ngroup = st->ngroup, npeers = st->npeers, i, j;
    u32 pi;

    for( pi = 0; pi < npeers; pi++ ){
        xfer_t * in = (xfer_t *)depv[5 + ngroup + pi].ptr;
        if( in == NULL ) continue;
        for( j = 0; in[j].to >= 0; j++ ) view[ in[j].to * 8 + in[j].dir ] = in[j].from;
    }

    ocrGuid_t outDb[TEMPEST_MAX_RANKS];
    xfer_t *  outPtr[TEMPEST_MAX_RANKS];
    u64       outN[TEMPEST_MAX_RANKS];
    for( pi = 0; pi < npeers; pi++ ){
        ocrHint_t h;
        ocrDbCreate( &outDb[pi], (void **)&outPtr[pi],
                     sizeof(xfer_t) * ( st->peerCount[pi] + 1 ), 0,
                     rankDbHint( &h, st->myRank ), NO_ALLOC );
        outN[pi] = 0;
    }
    for( i = 0; i < ngroup; i++ ){
        xfer_t * s = (xfer_t *)depv[5 + i].ptr;
        if( s == NULL ) continue;
        for( j = 0; s[j].to >= 0; j++ ){
            u64 home = patchHomeRank( (u64)s[j].to, st->k, st->nranks );
            if( home == st->myRank ){
                view[ s[j].to * 8 + s[j].dir ] = s[j].from;
            }else{
                for( pi = 0; pi < npeers; pi++ ) if( st->peer[pi] == home ) break;
                if( pi < npeers ) outPtr[pi][ outN[pi]++ ] = s[j];
            }
        }
    }
    for( pi = 0; pi < npeers; pi++ ){
        outPtr[pi][ outN[pi] ].to = -1;
        ocrDbRelease( outDb[pi] );
        ocrEventSatisfy( st->peerChan[pi], outDb[pi] );
    }

    st->step++;
    if( st->step >= st->duration ){
        if( patchHomeRank( TEST_PATCH, st->k, st->nranks ) == st->myRank ){
            s64 * v = &view[ TEST_PATCH * 8 ];
            printf( "*CROSS-CHECKING NEIGHBOR DATA EXCHANGE*\n" );
            printf( "%lld\t%lld\t%lld\n", (long long)v[NW], (long long)v[N], (long long)v[NE] );
            printf( "%lld\t%lld\t%lld\n", (long long)v[W], (long long)TEST_PATCH, (long long)v[E] );
            printf( "%lld\t%lld\t%lld\n", (long long)v[SW], (long long)v[S], (long long)v[SE] );
            fflush( stdout );
        }
        /* No further timestep is spawned, so the rank's templates are done. */
        ocrEdtTemplateDestroy( st->patchTPT );
        ocrEdtTemplateDestroy( st->groupTPT );
        ocrEdtTemplateDestroy( st->applyTPT );
        { ocrGuid_t d; void * dp; ocrHint_t dh;
          ocrDbCreate( &d, &dp, sizeof(u64), 0, rankDbHint( &dh, st->myRank ), NO_ALLOC );
          *(u64 *)dp = st->myRank; ocrDbRelease( d );
          ocrEventSatisfy( st->doneEvt, d ); }
        return NULL_GUID;
    }
    spawnTimestep( st, depv[0].guid, depv[1].guid, depv[2].guid, depv[3].guid, depv[4].guid,
                   lp, sliceG, groupG );
    return NULL_GUID;
}

/* Setup runs in two phases so no rank binds a dependence to a name that has
 * not been created: mainEdt creates every labeled name and per-rank block,
 * phase 1 fills them, phase 2 reads them.  A channel is created by the rank
 * that RECEIVES on it and published at the label (sender*nranks + receiver);
 * labeled channel ranges are not among this OCR's labeled kinds, so the guid
 * rides a labeled sticky, as the other rank-persistent ports here do. */
ocrGuid_t chanInitEdt( u32 paramc, u64 * paramv, u32 depc, ocrEdtDep_t depv[] )
{
    u64 myRank = paramv[0], nranks = paramv[1];
    ocrGuid_t range = (ocrGuid_t){ .guid = paramv[2] };
    ocrGuid_t * mine = (ocrGuid_t *)depv[0].ptr;
    ocrEventParams_t ch;
    ch.EVENT_CHANNEL.maxGen = TEMPEST_MAXGEN;
    ch.EVENT_CHANNEL.nbSat  = 1;
    ch.EVENT_CHANNEL.nbDeps = 1;
    u64 p;
    for( p = 0; p < nranks; p++ ){
        if( p == myRank ){ mine[p] = NULL_GUID; continue; }
        ocrEventCreateParams( &mine[p], OCR_EVENT_CHANNEL_T, 0, &ch );
        ocrGuid_t slot; ocrGuid_t * sp; ocrHint_t dh;
        ocrDbCreate( &slot, (void **)&sp, sizeof(ocrGuid_t), 0, rankDbHint( &dh, myRank ), NO_ALLOC );
        *sp = mine[p];
        ocrDbRelease( slot );
        ocrGuid_t name;
        ocrGuidFromIndex( &name, range, p * nranks + myRank );
        ocrEventSatisfy( name, slot );
    }
    /* Hand the block on through the output event: sharing a datablock orders
     * nothing, and phase 2 must not read this before it is filled. */
    return depv[0].guid;
}

ocrGuid_t rankStartEdt( u32 paramc, u64 * paramv, u32 depc, ocrEdtDep_t depv[] )
{
    u64 myRank = paramv[0], nranks = paramv[1], k = paramv[2], duration = paramv[3];
    ocrGuid_t doneEvt = (ocrGuid_t){ .guid = paramv[4] };
    ocrGuid_t * recvChan = (ocrGuid_t *)depv[0].ptr;
    u64 total = 6 * k * k, i, p;
    u32 d;

    ocrGuid_t stDb, viewDb, lpDb, sgDb, ggDb;
    rankState_t * st; s64 * view; u64 * lp; ocrGuid_t * sliceG, * groupG;
    ocrHint_t dh;
    ocrDbCreate( &stDb, (void **)&st, sizeof(rankState_t), 0, rankDbHint( &dh, myRank ), NO_ALLOC );
    ocrDbCreate( &viewDb, (void **)&view, sizeof(s64) * viewEntries( k ), 0, rankDbHint( &dh, myRank ), NO_ALLOC );
    for( i = 0; i < viewEntries( k ); i++ ) view[i] = -1;

    u64 nlocal = 0;
    for( i = 0; i < total; i++ ) if( patchHomeRank( i, k, nranks ) == myRank ) nlocal++;
    if( nlocal == 0 ) nlocal = 0;
    ocrDbCreate( &lpDb, (void **)&lp, sizeof(u64) * ( nlocal ? nlocal : 1 ), 0, rankDbHint( &dh, myRank ), NO_ALLOC );
    { u64 n = 0; for( i = 0; i < total; i++ ) if( patchHomeRank( i, k, nranks ) == myRank ) lp[n++] = i; }

    st->k = k; st->duration = duration; st->nranks = nranks; st->myRank = myRank;
    st->step = 0; st->nlocal = nlocal; st->doneEvt = doneEvt; st->npeers = 0;
    st->ngroup = nlocal <= TEMPEST_GROUPS ? nlocal : TEMPEST_GROUPS;
    st->fanin  = st->ngroup ? ( nlocal + st->ngroup - 1 ) / st->ngroup : 1;

    u64 cnt[TEMPEST_MAX_RANKS];
    for( p = 0; p < TEMPEST_MAX_RANKS; p++ ) cnt[p] = 0;
    for( i = 0; i < nlocal; i++ )
        for( d = 0; d < 8; d++ ){
            s64 nb = findNeighborPatch( lp[i], k, d );
            if( nb < 0 ) continue;
            if( reverseDir( lp[i], (u64)nb, k ) >= 8 ) continue;
            u64 home = patchHomeRank( (u64)nb, k, nranks );
            if( home != myRank ) cnt[home]++;
        }
    { u64 slot = 1;
      for( p = 0; p < nranks; p++ ){
        if( p == myRank ) continue;
        if( cnt[p] ){
            u64 pi = st->npeers++;
            st->peer[pi] = p; st->peerCount[pi] = cnt[p];
            st->chan[pi] = recvChan[p];
            st->peerChan[pi] = *(ocrGuid_t *)depv[slot].ptr;
        }
        slot++;
      } }

    /* Slices and group blocks are made once and reused every timestep, the way
     * the base port reuses its halo blocks. */
    ocrDbCreate( &sgDb, (void **)&sliceG, sizeof(ocrGuid_t) * ( nlocal ? nlocal : 1 ), 0,
                 rankDbHint( &dh, myRank ), NO_ALLOC );
    ocrDbCreate( &ggDb, (void **)&groupG, sizeof(ocrGuid_t) * ( st->ngroup ? st->ngroup : 1 ), 0,
                 rankDbHint( &dh, myRank ), NO_ALLOC );
    for( i = 0; i < nlocal; i++ ){
        void * sp;
        ocrDbCreate( &sliceG[i], &sp, sizeof(xfer_t) * 9, 0, rankDbHint( &dh, myRank ), NO_ALLOC );
        ((xfer_t *)sp)[0].to = -1;
        ocrDbRelease( sliceG[i] );
    }
    for( i = 0; i < st->ngroup; i++ ){
        void * gp;
        ocrDbCreate( &groupG[i], &gp, sizeof(xfer_t) * ( st->fanin * 8 + 1 ), 0,
                     rankDbHint( &dh, myRank ), NO_ALLOC );
        ((xfer_t *)gp)[0].to = -1;
        ocrDbRelease( groupG[i] );
    }

    ocrEdtTemplateCreate( &st->patchTPT, patchEdt, 1, 2 );
    ocrEdtTemplateCreate( &st->groupTPT, groupEdt, 0, (u32)( 1 + st->fanin ) );
    ocrEdtTemplateCreate( &st->applyTPT, applyEdt, 0, (u32)( 5 + st->ngroup + st->npeers ) );

    /* Seed each incoming channel once.  The first timestep's apply waits on
     * what the peers send, and the peers' first apply waits on this rank in
     * the same way, so without a seed generation neither ever runs -- the base
     * port seeds its halo blocks at timestep 0 for the same reason. */
    for( i = 0; i < st->npeers; i++ ){
        ocrGuid_t seed; void * sp; ocrHint_t sh;
        ocrDbCreate( &seed, &sp, sizeof(xfer_t), 0, rankDbHint( &sh, myRank ), NO_ALLOC );
        ((xfer_t *)sp)[0].to = -1;
        ocrDbRelease( seed );
        ocrEventSatisfy( st->chan[i], seed );
    }

    /* Spawn before releasing: lp, sliceG and groupG point into blocks this
     * task still holds, and a released block may not be read. */
    rankState_t snap = *st;
    spawnTimestep( &snap, stDb, viewDb, lpDb, sgDb, ggDb, lp, sliceG, groupG );
    ocrDbRelease( stDb ); ocrDbRelease( viewDb ); ocrDbRelease( lpDb );
    ocrDbRelease( sgDb ); ocrDbRelease( ggDb );
    return NULL_GUID;
}

ocrGuid_t wrapupEdt( u32 paramc, u64 * paramv, u32 depc, ocrEdtDep_t depv[] )
{
    ocrShutdown();
    return NULL_GUID;
}

ocrGuid_t mainEdt( u32 paramc, u64 * paramv, u32 depc, ocrEdtDep_t depv[] )
{
    void * marshalled = depv[0].ptr;
    u32 argc = ocrGetArgc( marshalled ), i;
    u64 k = 2, duration = DURATION, p, r;

    if( argc > 3 ){
        ocrPrintf( "USAGE: %s [patchRange [duration]]  expected at most 2 arguments, got %u\n",
                   ocrGetArgv( marshalled, 0 ), argc - 1 );
        ocrShutdown(); return NULL_GUID;
    }
    { u64 * out[2] = { &k, &duration };
      const char * names[2] = { "patchRange", "duration" };
      for( i = 1; i < argc; i++ ){
        char * arg = ocrGetArgv( marshalled, i ), * endptr;
        long val = strtol( arg, &endptr, 10 );
        if( *arg == '\0' || *endptr != '\0' || val <= 0 ){
            ocrPrintf( "USAGE: %s [patchRange [duration]]  %s must be a positive integer, got '%s'\n",
                       ocrGetArgv( marshalled, 0 ), names[i-1], arg );
            ocrShutdown(); return NULL_GUID;
        }
        *out[i-1] = (u64)val;
      } }

    u64 nranks;
    ocrAffinityCount( AFFINITY_PD, &nranks );
    if( nranks > TEMPEST_MAX_RANKS ){
        ocrPrintf( "tempest_dist: at most %u ranks\n", (u32)TEMPEST_MAX_RANKS );
        ocrShutdown(); return NULL_GUID;
    }

    ocrGuid_t range;
    if( ocrGuidRangeCreate( &range, nranks * nranks, GUID_USER_EVENT_STICKY ) != 0 ){
        ocrPrintf( "tempest_dist: cannot reserve %lu names\n", (unsigned long)( nranks * nranks ) );
        ocrShutdown(); return NULL_GUID;
    }
    for( r = 0; r < nranks; r++ )
        for( p = 0; p < nranks; p++ ){
            if( p == r ) continue;
            ocrGuid_t name;
            ocrGuidFromIndex( &name, range, p * nranks + r );
            ocrEventCreate( &name, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG | GUID_PROP_IS_LABELED );
        }

    ocrGuid_t chanDb[TEMPEST_MAX_RANKS], doneEvt[TEMPEST_MAX_RANKS];
    for( r = 0; r < nranks; r++ ){
        ocrGuid_t * slots; ocrHint_t dh;
        ocrDbCreate( &chanDb[r], (void **)&slots, sizeof(ocrGuid_t) * nranks, 0,
                     rankDbHint( &dh, r ), NO_ALLOC );
        ocrDbRelease( chanDb[r] );
        ocrEventCreate( &doneEvt[r], OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG );
    }

    ocrGuid_t wrapTPT, wrapGuid;
    ocrEdtTemplateCreate( &wrapTPT, wrapupEdt, 0, (u32)nranks );
    ocrEdtCreate( &wrapGuid, wrapTPT, EDT_PARAM_DEF, NULL, EDT_PARAM_DEF, NULL,
                  EDT_PROP_NONE, NULL_HINT, NULL );
    for( r = 0; r < nranks; r++ ) ocrAddDependence( doneEvt[r], wrapGuid, (u32)r, DB_MODE_RO );

    ocrGuid_t chanTPT, startTPT;
    ocrEdtTemplateCreate( &chanTPT,  chanInitEdt,  3, 1 );
    ocrEdtTemplateCreate( &startTPT, rankStartEdt, 5, (u32)nranks );
    for( r = 0; r < nranks; r++ ){
        ocrHint_t eh;
        ocrGuid_t chanGuid, chanOut, startGuid;
        u64 cprm[3] = { r, nranks, (u64)range.guid };
        ocrEdtCreate( &chanGuid, chanTPT, EDT_PARAM_DEF, cprm, EDT_PARAM_DEF, NULL,
                      EDT_PROP_NONE, rankEdtHint( &eh, r ), &chanOut );
        ocrAddDependence( chanDb[r], chanGuid, 0, DB_MODE_RW );
        u64 sprm[5] = { r, nranks, k, duration, (u64)doneEvt[r].guid };
        ocrEdtCreate( &startGuid, startTPT, EDT_PARAM_DEF, sprm, EDT_PARAM_DEF, NULL,
                      EDT_PROP_NONE, rankEdtHint( &eh, r ), NULL );
        ocrAddDependence( chanOut, startGuid, 0, DB_MODE_RO );
        { u32 slot = 1;
          for( p = 0; p < nranks; p++ ){
            if( p == r ) continue;
            ocrGuid_t name;
            ocrGuidFromIndex( &name, range, r * nranks + p );
            ocrAddDependence( name, startGuid, slot++, DB_MODE_RO );
          } }
    }
    /* Every task that uses them has been created. */
    ocrEdtTemplateDestroy( wrapTPT );
    ocrEdtTemplateDestroy( chanTPT );
    ocrEdtTemplateDestroy( startTPT );
    return NULL_GUID;
}
