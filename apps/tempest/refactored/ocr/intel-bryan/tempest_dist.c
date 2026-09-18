/*
 *Author: Bryan Pawlowski
 *Copywrite Intel Corporation 2015
 *
 *This file is subject to the license agreement located in the file ../../../../LICENSE (apps/LICENSE)
 *and cannot be distributed without it. This notice cannot be removed or modified.
 */

/*
 * Neighbour exchange over a cube sphere: every patch stamps its own number into
 * each of its eight neighbours once per timestep, and the neighbour table one
 * patch collects is printed at the end as the cross-check.
 *
 * The exchange is expressed per RANK PAIR: what leaves a rank in a timestep
 * travels as one block per peer, so the number of crossings a stamp makes is
 * the topology's and the number of messages is the rank graph's.
 *
 * A rank's state is sized by what the rank owns, not by the sphere: the
 * ownership map is invertible, so a rank enumerates its own patches without
 * touching the others.  Ownership is a box of the k x 6k patch strip, cut again
 * into tiles -- the groups -- that are the unit of parallelism: a group builds
 * its own patches' geometry, opens their tasks each round, and delivers every
 * stamp that stays inside it.  Only what leaves a tile reaches the rank's one
 * serial point per round.
 */

#ifndef ENABLE_EXTENSION_LABELING
#define ENABLE_EXTENSION_LABELING
#endif

#include <ocr.h>
#include <errno.h>
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
 * the cube sphere reverses orientation across some face seams, so the direction
 * of `me` as seen from `neighbour` is whichever of its eight points back. */
static u32 reverseDir( u64 me, u64 neighbour, u64 k )
{
    u32 e;
    for( e = 0; e < 8; e++ )
        if( findNeighborPatch( neighbour, k, e ) == (s64)me ) return e;
    return 8;                            /* no reverse link: nothing to deliver */
}

/* One stamp on the wire: `from` belongs in direction `dir` of patch `to`.  The
 * triple travels rather than an agreed ordering, so the two sides need agree
 * on nothing beyond the geometry they both compute. */
typedef struct{ s64 to, dir, from; }xfer_t;

/* A stamp whose destination lies outside the group that produced it.  A
 * negative `peer` names a group of this rank and the element of that group's
 * view to write; otherwise it names the peer slot the pair (a, b) = (patch,
 * direction) is delivered to.  `a < 0` terminates the block. */
typedef struct{ s64 peer, a, b, from; }esc_t;

#define TEMPEST_GROUPS       512      /* default group target; argv[3] overrides */
#define TEMPEST_MAX_RANKS    64
#define TEMPEST_MAX_RANGE    65535    /* every size derived from 6k^2 stays in u64 */
#define TEMPEST_MAX_DURATION 1000000
#define TEMPEST_MAX_GROUPS   1048576
#define TEMPEST_MAXGEN       2        /* generations in flight, as the base uses */

typedef struct{
    u64 k, rounds, nranks, myRank;
    u64 nlocal, ngroup, npeers, maxTile;
    u64 Pn, Q, R0, R1, C0, C1;       /* the rank's row/column box in the strip */
    u64 gr, gc;                      /* the box's grid of groups              */
    ocrGuid_t patchTPT, groupTPT, spawnTPT, applyTPT;
    ocrGuid_t doneEvt;
    ocrGuid_t chan[TEMPEST_MAX_RANKS];
    ocrGuid_t peerChan[TEMPEST_MAX_RANKS];
    u64 peerCount[TEMPEST_MAX_RANKS];
    u64 peerIdx[TEMPEST_MAX_RANKS];  /* rank -> peer slot, TEMPEST_MAX_RANKS where none */
}rankState_t;

/* A group owns one tile of its rank's box and everything those patches need for
 * the whole run: their numbers, the destination of each of their eight stamps,
 * one slice per patch, the tile's view and the block its escaping stamps are
 * collected in.  Trailing arrays, in order:
 * u64 pnum[n]; ocrGuid_t slice[n]; s64 dst[n*8]. */
typedef struct{
    u64 r0, c0, h, w;                /* the tile, in the rank box's coordinates */
    u64 n, nesc;
    ocrGuid_t vw, esc;
}groupState_t;

static u64 groupBytes( u64 n )
{
    return sizeof(groupState_t) + n * sizeof(u64) + n * sizeof(ocrGuid_t)
         + n * 8 * sizeof(s64);
}
static u64 *       gsPnum ( groupState_t * gs ){ return (u64 *)( gs + 1 ); }
static ocrGuid_t * gsSlice( groupState_t * gs ){ return (ocrGuid_t *)( gsPnum( gs ) + gs->n ); }
static s64 *       gsDst  ( groupState_t * gs ){ return (s64 *)( gsSlice( gs ) + gs->n ); }

static u64 ceilDiv( u64 a, u64 b ){ return ( a + b - 1 ) / b; }

/* Patch numbering unrolls the six faces into a k x 6k strip: row R within a
 * face, column C across the strip. */
static void stripCoord( u64 P, u64 k, u64 * R, u64 * C )
{
    u64 face = P / ( k * k ), idx = P % ( k * k );
    *R = idx / k;
    *C = face * k + ( idx % k );
}
static u64 patchAt( u64 k, u64 R, u64 C )
{
    return ( C / k ) * k * k + R * k + ( C % k );
}

/* The ownership map and its inverse.  The strip is cut into a Pn x Q grid of
 * boxes whose total cut length is minimal over the factorisations of the rank
 * count.  Cutting an axis of length L into m parts at ceil(i*L/m) puts x in
 * part floor(x*m/L); homeOf and ownedRange must stay that pair, since a rank
 * routes a stamp with the one and indexes its own state with the other. */
static u64 homeOf( const rankState_t * st, u64 P )
{
    u64 R, C;
    stripCoord( P, st->k, &R, &C );
    return ( ( R * st->Pn ) / st->k ) * st->Q + ( C * st->Q ) / ( 6 * st->k );
}

static void ownedRange( rankState_t * st )
{
    u64 k = st->k, nranks = st->nranks, rank = st->myRank;
    u64 P = 1, best = (u64)-1, p, br, bc;

    for( p = 1; p <= nranks; ++p ){
        u64 q, cut;
        if( nranks % p ) continue;
        q = nranks / p;
        cut = ( p - 1 ) * 6 * k + ( q - 1 ) * k;
        if( cut < best ){ best = cut; P = p; }
    }
    st->Pn = P; st->Q = nranks / P;
    br = rank / st->Q; bc = rank % st->Q;
    st->R0 = ceilDiv( br * k, st->Pn );
    st->R1 = ceilDiv( ( br + 1 ) * k, st->Pn );
    st->C0 = ceilDiv( bc * 6 * k, st->Q );
    st->C1 = ceilDiv( ( bc + 1 ) * 6 * k, st->Q );
    if( st->R1 > k ) st->R1 = k;
    if( st->C1 > 6 * k ) st->C1 = 6 * k;
    if( st->R1 <= st->R0 || st->C1 <= st->C0 ){
        st->R1 = st->R0; st->C1 = st->C0; st->nlocal = 0;
    }else{
        st->nlocal = ( st->R1 - st->R0 ) * ( st->C1 - st->C0 );
    }
}

/* The rank's box is cut again, into about `groups` tiles.  A tile delivers
 * every stamp that stays inside it and no other, so its perimeter -- its SHAPE,
 * not the tile count -- is what has to pass through the rank's single point of
 * contact; tiles are therefore kept as square as the box allows. */
static void groupGrid( rankState_t * st, u64 groups )
{
    u64 h = st->R1 - st->R0, w = st->C1 - st->C0, gr = 1, gc;

    if( !st->nlocal || !groups ){
        st->gr = st->gc = st->ngroup = 0; st->maxTile = 1; return;
    }
    while( gr < h && ( gr + 1 ) * ( gr + 1 ) * w <= groups * h ) gr++;
    gc = ceilDiv( groups, gr );
    if( gc > w ) gc = w;
    st->gr = gr; st->gc = gc;
    st->ngroup = gr * gc;
    st->maxTile = ceilDiv( h, gr ) * ceilDiv( w, gc );
}

static void groupTile( const rankState_t * st, u64 g,
                       u64 * r0, u64 * c0, u64 * h, u64 * w )
{
    u64 bh = st->R1 - st->R0, bw = st->C1 - st->C0;
    u64 gy = g / st->gc, gx = g % st->gc;
    u64 a = ceilDiv( gy * bh, st->gr ), b = ceilDiv( ( gy + 1 ) * bh, st->gr );
    u64 c = ceilDiv( gx * bw, st->gc ), d = ceilDiv( ( gx + 1 ) * bw, st->gc );
    *r0 = a; *c0 = c; *h = b - a; *w = d - c;
}

/* Where a patch sits on this rank: the group that owns it and the first of its
 * eight elements in that group's view.  Zero when the patch is not here. */
static int localPlace( const rankState_t * st, u64 P, u64 * grp, u64 * elem )
{
    u64 R, C, r, c, bh, bw, gy, gx, r0, c0, h, w;

    stripCoord( P, st->k, &R, &C );
    if( R < st->R0 || R >= st->R1 || C < st->C0 || C >= st->C1 ) return 0;
    bh = st->R1 - st->R0; bw = st->C1 - st->C0;
    r = R - st->R0; c = C - st->C0;
    gy = ( r * st->gr ) / bh; gx = ( c * st->gc ) / bw;
    *grp = gy * st->gc + gx;
    groupTile( st, *grp, &r0, &c0, &h, &w );
    *elem = ( ( r - r0 ) * w + ( c - c0 ) ) * 8;
    return 1;
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

static void printCross( const s64 * v )
{
    printf( "*CROSS-CHECKING NEIGHBOR DATA EXCHANGE*\n" );
    printf( "%lld\t%lld\t%lld\n", (long long)v[NW], (long long)v[N],  (long long)v[NE] );
    printf( "%lld\t%lld\t%lld\n", (long long)v[W],  (long long)TEST_PATCH, (long long)v[E] );
    printf( "%lld\t%lld\t%lld\n", (long long)v[SW], (long long)v[S],  (long long)v[SE] );
    fflush( stdout );
}

/* A round: every patch stamps its own slice; the group that owns the patch
 * turns each stamp into a write into its own view when the destination is one
 * of its patches, and into an entry of its escaping block otherwise; the rank's
 * single apply routes what escaped -- into the view of another group of the
 * rank, or into one block per peer.  A destination was decided once, at setup,
 * so no round evaluates geometry.
 *
 * Three ordering rules hold the structure together.  Every hand-off between
 * tasks is an output event, so a consumer acquires a block only after its
 * producer released it.  A round's incoming blocks are all consumed before any
 * outgoing block is published, so a peer that is a round ahead cannot overtake
 * a reader of the buffer it reuses.  And the blocks a round reuses in place are
 * ordered by creation: the producers of round t+1 are created only after the
 * last access round t makes to them. */

/* One patch, one round: the same eight stores the base port's patch task does,
 * into this patch's own slice.  A direction with no reverse link stores -1,
 * which delivers nothing. */
ocrGuid_t patchEdt( u32 paramc, u64 * paramv, u32 depc, ocrEdtDep_t depv[] )
{
    groupState_t * gs = (groupState_t *)depv[0].ptr;
    s64 * out         = (s64 *)depv[1].ptr;
    u64 j = paramv[0];
    s64 p = (s64)gsPnum( gs )[j];
    const s64 * dst = gsDst( gs ) + j * 8;
    u32 d;

    for( d = 0; d < 8; d++ ) out[d] = dst[d] == -1 ? -1 : p;
    return depv[1].guid;
}

/* A group delivers what it can: a stamp landing on one of its own patches
 * becomes a write into its own view, the rest are copied into the block the
 * rank's apply routes.  This is the only writer of either block in a round, and
 * it is as wide as the rank's group count. */
ocrGuid_t groupEdt( u32 paramc, u64 * paramv, u32 depc, ocrEdtDep_t depv[] )
{
    esc_t * esc       = (esc_t *)depv[0].ptr;
    s64 * vw          = (s64 *)depv[1].ptr;
    groupState_t * gs = (groupState_t *)depv[2].ptr;
    const s64 * dst   = gsDst( gs );
    u64 j;
    u32 d;

    for( j = 0; j < gs->n; j++ ){
        const s64 * in = (const s64 *)depv[3 + j].ptr;
        if( in == NULL ) continue;
        for( d = 0; d < 8; d++ ){
            s64 v = in[d], t = dst[ j * 8 + d ];
            if( v < 0 || t == -1 ) continue;
            if( t >= 0 ) vw[t] = v;
            else esc[ -t - 2 ].from = v;
        }
    }
    return depv[0].guid;
}

/* Setup for one group: the patch numbers of its tile, the destination of each
 * of their eight stamps, one slice per patch, the tile's view and its escaping
 * block.  Runs once per group, so the geometry is evaluated once per patch for
 * the whole run and the groups of a rank build in parallel. */
ocrGuid_t groupBuildEdt( u32 paramc, u64 * paramv, u32 depc, ocrEdtDep_t depv[] )
{
    groupState_t * gs = (groupState_t *)depv[0].ptr;
    rankState_t *  st = (rankState_t *)depv[1].ptr;
    ocrGuid_t apply   = (ocrGuid_t){ .guid = paramv[1] };
    u64 g = paramv[0];
    u64 * pnum      = gsPnum( gs );
    ocrGuid_t * sl  = gsSlice( gs );
    s64 * dst       = gsDst( gs );
    esc_t * ep;
    void * vp;
    ocrHint_t dh;
    u64 r, c, j, nesc = 0;
    u32 d;

    for( r = 0; r < gs->h; r++ )
        for( c = 0; c < gs->w; c++ )
            pnum[ r * gs->w + c ] = patchAt( st->k, st->R0 + gs->r0 + r,
                                             st->C0 + gs->c0 + c );

    for( j = 0; j < gs->n; j++ )
        for( d = 0; d < 8; d++ ){
            s64 nb = findNeighborPatch( pnum[j], st->k, d );
            u64 grp, elem;
            u32 rev;
            if( nb < 0 ){ dst[ j * 8 + d ] = -1; continue; }
            rev = reverseDir( pnum[j], (u64)nb, st->k );
            if( rev >= 8 ){ dst[ j * 8 + d ] = -1; continue; }
            if( localPlace( st, (u64)nb, &grp, &elem ) && grp == g )
                dst[ j * 8 + d ] = (s64)( elem + rev );
            else
                dst[ j * 8 + d ] = -2 - (s64)nesc++;
        }
    gs->nesc = nesc;

    ocrDbCreate( &gs->esc, (void **)&ep, sizeof(esc_t) * ( nesc + 1 ), 0,
                 rankDbHint( &dh, st->myRank ), NO_ALLOC );
    for( j = 0; j < gs->n; j++ )
        for( d = 0; d < 8; d++ ){
            s64 t = dst[ j * 8 + d ], nb;
            u64 grp, elem, home;
            u32 rev;
            if( t >= -1 ) continue;
            nb   = findNeighborPatch( pnum[j], st->k, d );
            rev  = reverseDir( pnum[j], (u64)nb, st->k );
            home = homeOf( st, (u64)nb );
            ep[ -t - 2 ].from = -1;
            if( home == st->myRank && localPlace( st, (u64)nb, &grp, &elem ) ){
                ep[ -t - 2 ].peer = -1;
                ep[ -t - 2 ].a    = (s64)grp;
                ep[ -t - 2 ].b    = (s64)( elem + rev );
            }else{
                ep[ -t - 2 ].peer = (s64)st->peerIdx[home];
                ep[ -t - 2 ].a    = nb;
                ep[ -t - 2 ].b    = (s64)rev;
            }
        }
    ep[nesc].peer = -1; ep[nesc].a = -1; ep[nesc].b = -1; ep[nesc].from = -1;
    ocrDbRelease( gs->esc );

    for( j = 0; j < gs->n; j++ ){
        s64 * sp;
        ocrDbCreate( &sl[j], (void **)&sp, sizeof(s64) * 8, 0,
                     rankDbHint( &dh, st->myRank ), NO_ALLOC );
        for( d = 0; d < 8; d++ ) sp[d] = -1;
        ocrDbRelease( sl[j] );
    }
    ocrDbCreate( &gs->vw, &vp, sizeof(s64) * gs->n * 8, 0,
                 rankDbHint( &dh, st->myRank ), NO_ALLOC );
    for( j = 0; j < gs->n * 8; j++ ) ((s64 *)vp)[j] = -1;
    ocrDbRelease( gs->vw );

    ocrAddDependence( gs->vw, apply, (u32)( 2 + st->ngroup + g ), DB_MODE_RW );
    return depv[0].guid;
}

/* Open one group's next round: its patch tasks, its delivery task, and that
 * task's edge into the rank's apply.  One of these per group, so the rank's
 * spawn is as wide as its group count and a round's creation cost per task is
 * bounded by the group knob rather than by the problem. */
ocrGuid_t spawnEdt( u32 paramc, u64 * paramv, u32 depc, ocrEdtDep_t depv[] )
{
    groupState_t * gs = (groupState_t *)depv[0].ptr;
    rankState_t *  st = (rankState_t *)depv[1].ptr;
    ocrGuid_t apply = (ocrGuid_t){ .guid = paramv[1] };
    u64 g = paramv[0], j;
    ocrGuid_t grp, gOut, t, tOut;
    ocrHint_t eh;

    /* A task's output event is registered on its consumer before the task's
     * own inputs are wired: a once event that fires with no waiter is gone,
     * and a task whose inputs are all blocks is runnable the moment the last
     * one is added. */
    ocrEdtCreate( &grp, st->groupTPT, EDT_PARAM_DEF, NULL, EDT_PARAM_DEF, NULL,
                  EDT_PROP_NONE, rankEdtHint( &eh, st->myRank ), &gOut );
    ocrAddDependence( gOut, apply, (u32)( 2 + g ), DB_MODE_RO );

    for( j = 0; j < gs->n; j++ ){
        u64 prm[1] = { j };
        ocrEdtCreate( &t, st->patchTPT, EDT_PARAM_DEF, prm, EDT_PARAM_DEF, NULL,
                      EDT_PROP_NONE, rankEdtHint( &eh, st->myRank ), &tOut );
        ocrAddDependence( tOut, grp, (u32)( 3 + j ), DB_MODE_RO );
        ocrAddDependence( depv[0].guid,     t, 0, DB_MODE_RO );
        ocrAddDependence( gsSlice( gs )[j], t, 1, DB_MODE_RW );
    }
    for( j = gs->n; j < st->maxTile; j++ )
        ocrAddDependence( NULL_GUID, grp, (u32)( 3 + j ), DB_MODE_RO );

    ocrAddDependence( gs->esc,      grp, 0, DB_MODE_RW );
    ocrAddDependence( gs->vw,       grp, 1, DB_MODE_RW );
    ocrAddDependence( depv[0].guid, grp, 2, DB_MODE_RO );
    return NULL_GUID;
}

static void spawnRound( const rankState_t * st, const ocrGuid_t * gg,
                        ocrGuid_t stDb, ocrGuid_t apply )
{
    ocrHint_t eh;
    u64 g;
    for( g = 0; g < st->ngroup; g++ ){
        ocrGuid_t s;
        u64 prm[2] = { g, (u64)apply.guid };
        ocrEdtCreate( &s, st->spawnTPT, EDT_PARAM_DEF, prm, EDT_PARAM_DEF, NULL,
                      EDT_PROP_NONE, rankEdtHint( &eh, st->myRank ), NULL );
        ocrAddDependence( gg[g],     s, 0, DB_MODE_RO );
        ocrAddDependence( stDb,      s, 1, DB_MODE_RO );
        ocrAddDependence( NULL_GUID, s, 2, DB_MODE_RO );
    }
}

/* The rank's single point of contact for a round.  It sees only the stamps that
 * left their group: those bound for another group of this rank become writes
 * into that group's view, the rest become one block per peer.  Its cost is the
 * perimeter of the rank's tiles, never the number of patches it owns. */
ocrGuid_t applyEdt( u32 paramc, u64 * paramv, u32 depc, ocrEdtDep_t depv[] )
{
    rankState_t * st = (rankState_t *)depv[0].ptr;
    u64 step   = paramv[0];
    u64 ngroup = st->ngroup, npeers = st->npeers;
    u64 esc0 = 2, view0 = 2 + ngroup, ch0 = 2 + 2 * ngroup;
    u64 g, i, j;
    int sending = ( step + 1 < st->rounds );
    ocrGuid_t outDb[TEMPEST_MAX_RANKS];
    xfer_t *  outPtr[TEMPEST_MAX_RANKS];
    u64       outN[TEMPEST_MAX_RANKS];

    for( i = 0; i < npeers; i++ ){
        const xfer_t * in = (const xfer_t *)depv[ ch0 + i ].ptr;
        if( in == NULL ) continue;
        for( j = 0; in[j].to >= 0; j++ ){
            u64 grp, elem;
            if( !localPlace( st, (u64)in[j].to, &grp, &elem ) ) continue;
            ((s64 *)depv[ view0 + grp ].ptr)[ elem + (u64)in[j].dir ] = in[j].from;
        }
    }

    if( sending )
        for( i = 0; i < npeers; i++ ){
            ocrHint_t h;
            ocrDbCreate( &outDb[i], (void **)&outPtr[i],
                         sizeof(xfer_t) * ( st->peerCount[i] + 1 ), 0,
                         rankDbHint( &h, st->myRank ), NO_ALLOC );
            outN[i] = 0;
        }

    for( g = 0; g < ngroup; g++ ){
        const esc_t * e = (const esc_t *)depv[ esc0 + g ].ptr;
        if( e == NULL ) continue;
        for( j = 0; e[j].a >= 0; j++ ){
            s64 pi = e[j].peer;
            if( pi < 0 ){
                ((s64 *)depv[ view0 + (u64)e[j].a ].ptr)[ (u64)e[j].b ] = e[j].from;
            }else if( sending && (u64)pi < npeers ){
                xfer_t * o = &outPtr[pi][ outN[pi]++ ];
                o->to = e[j].a; o->dir = e[j].b; o->from = e[j].from;
            }
        }
    }

    if( sending )
        for( i = 0; i < npeers; i++ ){
            outPtr[i][ outN[i] ].to = -1;
            ocrDbRelease( outDb[i] );
            ocrEventSatisfy( st->peerChan[i], outDb[i] );
        }

    if( !sending ){
        u64 grp, elem;
        if( localPlace( st, TEST_PATCH, &grp, &elem ) && grp < ngroup )
            printCross( &((const s64 *)depv[ view0 + grp ].ptr)[ elem ] );
        /* No further round is opened, so the rank's templates are done. */
        ocrEdtTemplateDestroy( st->patchTPT );
        ocrEdtTemplateDestroy( st->groupTPT );
        ocrEdtTemplateDestroy( st->spawnTPT );
        ocrEdtTemplateDestroy( st->applyTPT );
        { ocrGuid_t d; void * dp; ocrHint_t dh;
          ocrDbCreate( &d, &dp, sizeof(u64), 0, rankDbHint( &dh, st->myRank ), NO_ALLOC );
          *(u64 *)dp = st->myRank; ocrDbRelease( d );
          ocrEventSatisfy( st->doneEvt, d ); }
        return NULL_GUID;
    }

    /* The next round's producers are created here, after the last read of every
     * block this round reuses, so a buffer's writer for round t+1 is ordered
     * behind this task's own accesses to it. */
    { ocrHint_t eh;
      ocrGuid_t next;
      u64 aprm[1] = { step + 1 };
      ocrEdtCreate( &next, st->applyTPT, EDT_PARAM_DEF, aprm, EDT_PARAM_DEF, NULL,
                    EDT_PROP_NONE, rankEdtHint( &eh, st->myRank ), NULL );
      ocrAddDependence( depv[0].guid, next, 0, DB_MODE_RO );
      ocrAddDependence( depv[1].guid, next, 1, DB_MODE_RO );
      for( g = 0; g < ngroup; g++ )
          ocrAddDependence( depv[ view0 + g ].guid, next, (u32)( view0 + g ), DB_MODE_RW );
      for( i = 0; i < npeers; i++ )
          ocrAddDependence( st->chan[i], next, (u32)( ch0 + i ), DB_MODE_RO );
      spawnRound( st, (const ocrGuid_t *)depv[1].ptr, depv[0].guid, next ); }
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
    u64 groups = paramv[5];
    ocrGuid_t * recvChan = (ocrGuid_t *)depv[0].ptr;
    u64 i, p, g, r, c, bh, bw, ngroup, npeers;
    u32 d;
    ocrHint_t dh, eh;
    ocrGuid_t stDb, ggDb, buildTPT, applyTPT, spawnTPT, apply0;
    rankState_t * st; ocrGuid_t * gg;
    ocrGuid_t inChan[TEMPEST_MAX_RANKS];
    u64 cnt[TEMPEST_MAX_RANKS];

    ocrDbCreate( &stDb, (void **)&st, sizeof(rankState_t), 0, rankDbHint( &dh, myRank ), NO_ALLOC );
    st->k = k; st->nranks = nranks; st->myRank = myRank;
    st->rounds = duration;
    st->doneEvt = doneEvt; st->npeers = 0;
    ownedRange( st );
    groupGrid( st, groups );
    ngroup = st->ngroup;
    bh = st->R1 - st->R0; bw = st->C1 - st->C0;

    /* A stamp with no reverse link is never sent, so counting every outbound
     * neighbour is an upper bound on what a peer receives -- which is all the
     * outgoing block needs to be sized by. */
    for( p = 0; p < TEMPEST_MAX_RANKS; p++ ) cnt[p] = 0;
    for( r = 0; r < bh; r++ )
        for( c = 0; c < bw; c++ ){
            u64 pn = patchAt( k, st->R0 + r, st->C0 + c );
            for( d = 0; d < 8; d++ ){
                s64 nb = findNeighborPatch( pn, k, d );
                u64 home;
                if( nb < 0 ) continue;
                home = homeOf( st, (u64)nb );
                if( home != myRank ) cnt[home]++;
            }
        }
    for( p = 0; p < TEMPEST_MAX_RANKS; p++ ) st->peerIdx[p] = TEMPEST_MAX_RANKS;
    { u64 slot = 1;
      for( p = 0; p < nranks; p++ ){
        if( p == myRank ) continue;
        if( cnt[p] ){
            u64 pi = st->npeers++;
            st->peerCount[pi] = cnt[p]; st->peerIdx[p] = pi;
            st->chan[pi] = recvChan[p];
            st->peerChan[pi] = *(ocrGuid_t *)depv[slot].ptr;
        }
        slot++;
      } }
    npeers = st->npeers;
    for( i = 0; i < npeers; i++ ) inChan[i] = st->chan[i];

    ocrEdtTemplateCreate( &st->patchTPT, patchEdt, 1, 2 );
    ocrEdtTemplateCreate( &st->groupTPT, groupEdt, 0, (u32)( 3 + st->maxTile ) );
    ocrEdtTemplateCreate( &st->spawnTPT, spawnEdt, 2, 3 );
    ocrEdtTemplateCreate( &st->applyTPT, applyEdt, 1,
                          (u32)( 2 + 2 * ngroup + npeers ) );
    ocrEdtTemplateCreate( &buildTPT, groupBuildEdt, 2, 2 );
    applyTPT = st->applyTPT; spawnTPT = st->spawnTPT;

    /* Seed each incoming channel once.  The first round's apply waits on what
     * the peers send, and the peers' first apply waits on this rank in the same
     * way, so without a seed generation neither ever runs -- the base port
     * seeds its halo blocks at timestep 0 for the same reason. */
    for( i = 0; i < npeers; i++ ){
        ocrGuid_t seed; void * sp; ocrHint_t sh;
        ocrDbCreate( &seed, &sp, sizeof(xfer_t), 0, rankDbHint( &sh, myRank ), NO_ALLOC );
        ((xfer_t *)sp)[0].to = -1;
        ocrDbRelease( seed );
        ocrEventSatisfy( inChan[i], seed );
    }

    ocrDbCreate( &ggDb, (void **)&gg, sizeof(ocrGuid_t) * ( ngroup ? ngroup : 1 ), 0,
                 rankDbHint( &dh, myRank ), NO_ALLOC );
    for( g = 0; g < ngroup; g++ ){
        groupState_t * gs;
        u64 gr0, gc0, gh, gw;
        groupTile( st, g, &gr0, &gc0, &gh, &gw );
        ocrDbCreate( &gg[g], (void **)&gs, groupBytes( gh * gw ), 0,
                     rankDbHint( &dh, myRank ), NO_ALLOC );
        gs->r0 = gr0; gs->c0 = gc0; gs->h = gh; gs->w = gw;
        gs->n = gh * gw; gs->nesc = 0;
        gs->vw = NULL_GUID; gs->esc = NULL_GUID;
        ocrDbRelease( gg[g] );
    }

    /* A datablock dependence is satisfied the moment it is added, so every
     * block is released before anything is wired to it: a task must not become
     * runnable while its producer still holds the block. */
    ocrDbRelease( stDb );

    { u64 aprm[1] = { 0 };
      ocrEdtCreate( &apply0, applyTPT, EDT_PARAM_DEF, aprm, EDT_PARAM_DEF, NULL,
                    EDT_PROP_NONE, rankEdtHint( &eh, myRank ), NULL );
      ocrAddDependence( stDb, apply0, 0, DB_MODE_RO );
      for( i = 0; i < npeers; i++ )
          ocrAddDependence( inChan[i], apply0, (u32)( 2 + 2 * ngroup + i ), DB_MODE_RO ); }

    for( g = 0; g < ngroup; g++ ){
        ocrGuid_t build, buildOut, spawn;
        u64 bprm[2] = { g, (u64)apply0.guid };
        u64 sprm[2] = { g, (u64)apply0.guid };
        ocrEdtCreate( &build, buildTPT, EDT_PARAM_DEF, bprm, EDT_PARAM_DEF, NULL,
                      EDT_PROP_NONE, rankEdtHint( &eh, myRank ), &buildOut );
        ocrEdtCreate( &spawn, spawnTPT, EDT_PARAM_DEF, sprm, EDT_PARAM_DEF, NULL,
                      EDT_PROP_NONE, rankEdtHint( &eh, myRank ), NULL );
        ocrAddDependence( buildOut,  spawn, 0, DB_MODE_RO );
        ocrAddDependence( stDb,      spawn, 1, DB_MODE_RO );
        ocrAddDependence( NULL_GUID, spawn, 2, DB_MODE_RO );
        ocrAddDependence( gg[g], build, 0, DB_MODE_RW );
        ocrAddDependence( stDb,  build, 1, DB_MODE_RO );
    }
    ocrEdtTemplateDestroy( buildTPT );

    ocrDbRelease( ggDb );
    ocrAddDependence( ggDb, apply0, 1, DB_MODE_RO );
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
    u64 k = 2, duration = DURATION, groups = TEMPEST_GROUPS, p, r;

    if( argc > 4 ){
        ocrPrintf( "USAGE: %s [patchRange [duration [groups]]]  expected at most 3 arguments, got %u\n",
                   ocrGetArgv( marshalled, 0 ), argc - 1 );
        ocrShutdown(); return NULL_GUID;
    }
    { u64 * out[3] = { &k, &duration, &groups };
      const u64 cap[3] = { TEMPEST_MAX_RANGE, TEMPEST_MAX_DURATION, TEMPEST_MAX_GROUPS };
      const char * names[3] = { "patchRange", "duration", "groups" };
      for( i = 1; i < argc; i++ ){
        char * arg = ocrGetArgv( marshalled, i ), * endptr;
        long val;
        errno = 0;
        val = strtol( arg, &endptr, 10 );
        if( *arg == '\0' || *endptr != '\0' || errno == ERANGE
            || val <= 0 || (u64)val > cap[i-1] ){
            ocrPrintf( "USAGE: %s [patchRange [duration [groups]]]  %s must be a positive integer <= %lu, got '%s'\n",
                       ocrGetArgv( marshalled, 0 ), names[i-1],
                       (unsigned long)cap[i-1], arg );
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
    ocrEdtTemplateCreate( &startTPT, rankStartEdt, 6, (u32)nranks );
    for( r = 0; r < nranks; r++ ){
        ocrHint_t eh;
        ocrGuid_t chanGuid, chanOut, startGuid;
        u64 cprm[3] = { r, nranks, (u64)range.guid };
        ocrEdtCreate( &chanGuid, chanTPT, EDT_PARAM_DEF, cprm, EDT_PARAM_DEF, NULL,
                      EDT_PROP_NONE, rankEdtHint( &eh, r ), &chanOut );
        u64 sprm[6] = { r, nranks, k, duration, (u64)doneEvt[r].guid, groups };
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
        ocrAddDependence( chanDb[r], chanGuid, 0, DB_MODE_RW );
    }
    /* Every task that uses them has been created. */
    ocrEdtTemplateDestroy( wrapTPT );
    ocrEdtTemplateDestroy( chanTPT );
    ocrEdtTemplateDestroy( startTPT );
    return NULL_GUID;
}
