/*
 *Author: Bryan Pawlowski
 *Copywrite Intel Corporation 2015
 *
 *This file is subject to the license agreement located in the file ../../../../LICENSE (apps/LICENSE)
 *and cannot be distributed without it. This notice cannot be removed or modified.
 */

/*
 * Stencil1D code with labeled sticky events and parallel init.
 */

#define ENABLE_EXTENSION_LABELING

#include <ocr.h>
#include <stdio.h>
#include <stdlib.h>
#include <ocr-std.h>
#include <extensions/ocr-labeling.h>

#ifndef TEST_PATCH
#define TEST_PATCH 0
#endif

#define DURATION 100

#define DEFAULT_LG_PROPS GUID_PROP_IS_LABELED | GUID_PROP_CHECK | EVT_PROP_TAKES_ARG
#define N   0
#define E   1
#define S   2
#define W   3
#define NE  4
#define SE  5
#define SW  6
#define NW  7

typedef struct{
    u32 panelNum;
    u64 patchRange;
    ocrGuid_t guidRanges[8];
}panel_t;

typedef struct{
    u64 patchNum;
    u64 k;
    ocrGuid_t patchTPT;
    s64 n, e, s, w, ne, se, sw, nw;
    u64 timestep;
    /* Persistent per-direction halo channels: recvChannels[dir] receives from
     * the neighbor in direction dir; sendChannels[dir] is that neighbor's recv
     * channel, learned once at init.  Timestep generations stream through the
     * persistent FIFOs, so no per-generation event create/destroy is needed. */
    ocrGuid_t recvChannels[8];
    ocrGuid_t sendChannels[8];
}patch_t;

typedef struct{
    s64 patchNum;
    //there will be other stuff here later.
}nbData_t;

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

ocrGuid_t patchEdt( u32 paramc, u64 * paramv, u32 depc, ocrEdtDep_t depv[] )
{
    patch_t * patchPTR = depv[8].ptr;
    ocrGuid_t patchGUID = depv[8].guid;
    ocrGuid_t edtGUID;
    u64 i;

    nbData_t * neighborPTRs[8];
    ocrGuid_t neighborGUIDs[8];
    s64 nPatches[8];                        //set up neighboring patches just for

    s64 * dirptr = &patchPTR->n;

    for(i = 0; i < 8; i++){
        if( dirptr[i] > -1 ) neighborPTRs[i] = depv[i].ptr;
        else neighborPTRs[i] = NULL;
        neighborGUIDs[i] = depv[i].guid;
    }

    if( patchPTR->patchNum == TEST_PATCH ) printf("timestep: %u\n", patchPTR->timestep);

    if( patchPTR->timestep == DURATION - 1 ){


        if( patchPTR->patchNum == TEST_PATCH ){

            printf("%d\t%d\t%d\n", dirptr[NW], dirptr[N], dirptr[NE]);
            printf("%d\t%d\t%d\n", dirptr[W], patchPTR->patchNum, dirptr[E]);
            printf("%d\t%d\t%d\n", dirptr[SW], dirptr[S], dirptr[SE]);

            printf("\n*CROSS-CHECKING NEIGHBOR DATA EXCHANGE*\n\n");

            for( i = 0; i < 8; i++ ){
                if(!ocrGuidIsNull(neighborGUIDs[i])) nPatches[i] = neighborPTRs[i]->patchNum;
                else nPatches[i] = -1;
            }

            printf("%d\t%d\t%d\n", nPatches[7], nPatches[0], nPatches[4]);
            printf("%d\t%d\t%d\n", nPatches[3], patchPTR->patchNum, nPatches[1]);
            printf("%d\t%d\t%d\n", nPatches[6], nPatches[2], nPatches[5]);

            //ocrShutdown();
        }
        return NULL_GUID;
    }

    ocrEdtCreate( &edtGUID, patchPTR->patchTPT, EDT_PARAM_DEF, NULL, EDT_PARAM_DEF,
                                                NULL, EDT_PROP_NONE, NULL_HINT, NULL );

    /* Wire the next generation's halo receives onto the persistent channels;
     * each dependence pops one generation from the FIFO. */
    for( i = 0; i < 8; i++ ){
        if( dirptr[i] > -1 ){
            ocrAddDependence( patchPTR->recvChannels[i], edtGUID, i, DB_MODE_RW );
        }else{
            ocrAddDependence( NULL_GUID, edtGUID, i, DB_MODE_RW );
        }
    }

    /* Forward each received block to the matching neighbor by satisfying its
     * recv channel; one satisfy enqueues one generation. */
    for( i = 0; i < 8; i++ ){
        if( neighborPTRs[i] != NULL ){
            neighborPTRs[i]->patchNum = patchPTR->patchNum;
            ocrDbRelease( neighborGUIDs[i] );
            ocrEventSatisfy( patchPTR->sendChannels[i], neighborGUIDs[i] );
        }
    }

    patchPTR->timestep++;

    ocrDbRelease( patchGUID );
    ocrAddDependence( patchGUID, edtGUID, 8, DB_MODE_RW );

    return NULL_GUID;
}

u32 findNeighborRelation( u64 patchNum, u64 neighborPatchNum, u64 k )
{
    u32 relation;

    for(relation = 0; relation < 8; relation++)
    {
        if( findNeighborPatch( neighborPatchNum, k, relation ) != patchNum ) continue;
        break;
    }

    return relation;
}


ocrGuid_t channelSetupEdt( u32 paramc, u64 * paramv, u32 depc, ocrEdtDep_t depv[] )
{
    /*
     * Second half of the channel handshake.
     * depv[0..7]: neighbors' published recv-channel GUIDs (my send targets;
     *             NULL_GUID where there is no neighbor); depv[8]: patch block.
     * Record the send channels, then launch the first patchEdt with the
     * timestep-0 seed halo blocks.
     */
    patch_t * patchPTR = depv[8].ptr;
    ocrGuid_t patchGUID = depv[8].guid;
    s64 *dirptr = &patchPTR->n;
    u64 i;

    for( i = 0; i < 8; i++ ){
        if( dirptr[i] > -1 && depv[i].ptr != NULL ){
            patchPTR->sendChannels[i] = *((ocrGuid_t *)depv[i].ptr);
        }else{
            patchPTR->sendChannels[i] = NULL_GUID;
        }
    }

    ocrGuid_t patchEdtGUID;
    ocrEdtCreate( &patchEdtGUID, patchPTR->patchTPT, EDT_PARAM_DEF, NULL, EDT_PARAM_DEF,
                  NULL, EDT_PROP_NONE, NULL_HINT, NULL );

    ocrGuid_t dbGuid;
    u64 *dummy;
    for( i = 0; i < 8; i++ ){
        ocrDbCreate( &dbGuid, (void **)&dummy, sizeof(nbData_t), 0, NULL_HINT, NO_ALLOC );
        ocrDbRelease( dbGuid );
        ocrAddDependence( dbGuid, patchEdtGUID, i, DB_MODE_RW );
    }

    ocrDbRelease( patchGUID );
    ocrAddDependence( patchGUID, patchEdtGUID, 8, DB_MODE_RW );

    return NULL_GUID;
}


ocrGuid_t patchInitEdt( u32 paramc, u64 * paramv, u32 depc, ocrEdtDep_t depv[] )
{

    /*
     * paramv[2]:
     *  0: patch's x position on panel
     *  1: patch's y position on panel
     *
     * depv[2]:
     *  0: panel info
     *  1: patch info
     *
     * - Populate patch with necessary info.
     * - Create and fire off patchEdts.
     */


    /*---------- unpack deps ----------*/
    panel_t * panelPTR = depv[0].ptr;
    ocrGuid_t panelGUID = depv[0].guid;

    patch_t * patchPTR = depv[1].ptr;
    ocrGuid_t patchGUID = depv[1].guid;
    /*-------- end unpack deps --------*/
    u64 i;
    u32 rel;

    patchPTR->k = panelPTR->patchRange;

    patchPTR->patchNum = paramv[0];


    /*find our neighbors*/
    patchPTR->n = findNeighborPatch( patchPTR->patchNum, patchPTR->k, N );
    patchPTR->e = findNeighborPatch( patchPTR->patchNum, patchPTR->k, E );
    patchPTR->s = findNeighborPatch( patchPTR->patchNum, patchPTR->k, S );
    patchPTR->w = findNeighborPatch( patchPTR->patchNum, patchPTR->k, W );
    patchPTR->ne = findNeighborPatch( patchPTR->patchNum, patchPTR->k, NE );
    patchPTR->se = findNeighborPatch( patchPTR->patchNum, patchPTR->k, SE );
    patchPTR->sw = findNeighborPatch( patchPTR->patchNum, patchPTR->k, SW );
    patchPTR->nw = findNeighborPatch( patchPTR->patchNum, patchPTR->k, NW );
    /*neighbors found*/

    /* One-time channel handshake over shared rendezvous slots
     * ocrGuidFromIndex(guidRanges[r], q): I publish my recvChannels[dir] at
     * (r = neighbor's relation back to me, q = neighbor's patchNum) and learn
     * sendChannels[dir] from (r = dir, q = my patchNum).  The encodings are
     * symmetric, so both endpoints agree on every slot. */

    s64 *dirptr = &patchPTR->n;

    ocrEventParams_t chParams;
    chParams.EVENT_CHANNEL.maxGen = 2;
    chParams.EVENT_CHANNEL.nbSat  = 1;
    chParams.EVENT_CHANNEL.nbDeps = 1;

    ocrEdtTemplateCreate( &patchPTR->patchTPT, patchEdt, 0, 9 );

    ocrGuid_t setupTPT, setupEdtGUID;
    ocrEdtTemplateCreate( &setupTPT, channelSetupEdt, 0, 9 );
    ocrEdtCreate( &setupEdtGUID, setupTPT, EDT_PARAM_DEF, NULL, EDT_PARAM_DEF,
                  NULL, EDT_PROP_NONE, NULL_HINT, NULL );

    for( i = 0; i < 8; i++ ){
        if( dirptr[i] > -1 ){
            /* create the channel on which I receive from this neighbor */
            ocrEventCreateParams( &(patchPTR->recvChannels[i]), OCR_EVENT_CHANNEL_T, false, &chParams );

            /* publish recvChannels[i] at the slot the neighbor reads */
            rel = findNeighborRelation( patchPTR->patchNum, dirptr[i], patchPTR->k );
            ocrGuid_t pubSticky;
            ocrGuidFromIndex( &pubSticky, panelPTR->guidRanges[rel], dirptr[i] );
            ocrEventCreate( &pubSticky, OCR_EVENT_STICKY_T, DEFAULT_LG_PROPS );

            ocrGuid_t chanDb;
            ocrGuid_t *chanSlot;
            ocrDbCreate( &chanDb, (void **)&chanSlot, sizeof(ocrGuid_t), 0, NULL_HINT, NO_ALLOC );
            *chanSlot = patchPTR->recvChannels[i];
            ocrDbRelease( chanDb );
            ocrEventSatisfy( pubSticky, chanDb );

            /* learn the neighbor's recv channel (my send target in dir i) */
            ocrGuid_t learnSticky;
            ocrGuidFromIndex( &learnSticky, panelPTR->guidRanges[i], patchPTR->patchNum );
            ocrEventCreate( &learnSticky, OCR_EVENT_STICKY_T, DEFAULT_LG_PROPS );
            ocrAddDependence( learnSticky, setupEdtGUID, i, DB_MODE_RO );
        }else{
            patchPTR->recvChannels[i] = NULL_GUID;
            patchPTR->sendChannels[i] = NULL_GUID;
            ocrAddDependence( NULL_GUID, setupEdtGUID, i, DB_MODE_RO );
        }
    }

    patchPTR->timestep = 0;

    ocrDbRelease( patchGUID );
    ocrAddDependence( patchGUID, setupEdtGUID, 8, DB_MODE_RW );

    return NULL_GUID;
}

ocrGuid_t panelInitEdt( u32 paramc, u64 * paramv, u32 depc, ocrEdtDep_t depv[] )
{

    /* paramv[0]:
     *
     * depv[1]:
     *  0: panel datablock
     *
     *
     * - use panel datablock to generate the proper number of datablocks
     * - use panel datablock to generate the proper number of EDTs.
     * - add the panel datablock as a dependency for every EDT.
     * - add edt's corresponding datablock as the second and final dependency.
     */

    /*UNPACK DEPS*/
    ocrGuid_t panelGUID = depv[0].guid;
    panel_t * panelPTR = depv[0].ptr;
    /*DONE UNPACKING*/

    u64 i, patchNum;
    u64 k = panelPTR->patchRange;
    u32 panelNum = panelPTR->panelNum;

    ocrGuid_t patchDb, patchInitGUID, patchInitTPT;

    patch_t * dummyPatch;

    ocrEdtTemplateCreate( &patchInitTPT, patchInitEdt, 1, 2 );

    ocrDbRelease( panelGUID );
    for( i = 0; i < (k * k); i++ ){     //we use the xy array, such that we can just pass
                                        //it in as the 2 params we need to denote our patch's position
                                        //on the current panel.

        patchNum = ((k * k) * panelNum) + i;
        ocrDbCreate( &patchDb, (void **)&dummyPatch, sizeof(patch_t), 0, NULL_HINT, NO_ALLOC );

        ocrEdtCreate( &patchInitGUID, patchInitTPT, EDT_PARAM_DEF, &patchNum,
                EDT_PARAM_DEF, NULL, EDT_PROP_NONE, NULL_HINT, NULL );

        ocrAddDependence( panelGUID, patchInitGUID, 0, DB_MODE_RW );
        ocrDbRelease( patchDb );
        ocrAddDependence( patchDb, patchInitGUID, 1, DB_MODE_RW );

    }


    return NULL_GUID;
}

ocrGuid_t realmainEdt( u32 paramc, u64 * paramv, u32 depc, ocrEdtDep_t depv[] )
{
    /*
     * paramv[0]:
     *
     * depv[1]:
     *  0: panelDb
     *
     *  set up panel information for later. create patches for each panel.
     */

    ocrGuid_t panelGUID[6];
    panel_t * panelPTR[6];
    ocrGuid_t panelInitGUID, panelInitTPT;
    u32 i, j;
    ocrGuid_t ranges[8];
    ocrEdtTemplateCreate( &panelInitTPT, panelInitEdt, 0, 1 );
    u64 k = paramv[0];


    /* One labeled-sticky range per direction; used only for the one-time
     * channel-GUID handshake (no toggle buffering needed -- the persistent
     * channels carry every timestep generation). */
    for( i = 0; i < 8; i++ )
    {
        ocrGuidRangeCreate( &(ranges[i]), 6 * (k * k), GUID_USER_EVENT_STICKY );
    }
    for( i = 0; i < 6; i++ )
    {
        panelGUID[i] = depv[i].guid;
        panelPTR[i] = depv[i].ptr;

        panelPTR[i]->panelNum = i;

        for( j = 0; j < 8; j++ )
        {
            panelPTR[i]->guidRanges[j] = ranges[j];
        }

        panelPTR[i]->patchRange = paramv[0];

        ocrEdtCreate( &panelInitGUID, panelInitTPT, EDT_PARAM_DEF, NULL, EDT_PARAM_DEF, NULL,
                        EDT_PROP_NONE, NULL_HINT, NULL );
        ocrDbRelease( panelGUID[i] );
        ocrAddDependence( panelGUID[i], panelInitGUID, 0, DB_MODE_RW );
    }
    return NULL_GUID;
}

ocrGuid_t wrapupEdt( u32 paramc, u64 * paramv, u32 depc, ocrEdtDep_t depv[] )
{
    ocrPrintf("DONE.\n");
    ocrShutdown();
    return NULL_GUID;
}

ocrGuid_t mainEdt( u32 paramc, u64 * paramv, u32 depc, ocrEdtDep_t depv[] )
{

    /*
     * paramv[0]:
     *
     * depv[1]:
     *  0: workload args db.
     *
     *
     * creates 6 panel datablocks to be used in realmain, sends it on.
     */

    void * programArgv = depv[0].ptr;

    ocrGuid_t realmainGUID, realmainTPT;
    ocrGuid_t wrapupGUID, wrapupTPT;
    ocrGuid_t panelDb;
    ocrGuid_t finishEVT;
    panel_t * panel;
    u32 i, argc;
    u64 patchRange;

    argc = ocrGetArgc( programArgv );

    if( argc != 2 ){
        ocrPrintf( "INCORRECT NUMBER OF ARGS. USING DEFAULT PARAMS. %s\n", ocrGetArgv( programArgv, 0 ) );
        patchRange = 2;
    }else{
        patchRange = (u64)atoi(ocrGetArgv(programArgv, 1));
    }

    u64 patchNums;


    ocrEdtTemplateCreate( &realmainTPT, realmainEdt, 1, 6 );

    ocrEdtCreate( &realmainGUID, realmainTPT, EDT_PARAM_DEF, &patchRange, EDT_PARAM_DEF, NULL, EDT_PROP_FINISH,
                    NULL_HINT, &finishEVT );

    ocrEdtTemplateCreate( &wrapupTPT, wrapupEdt, 0, 1 );

    ocrEdtCreate( &wrapupGUID, wrapupTPT, EDT_PARAM_DEF, NULL, EDT_PARAM_DEF, &finishEVT, EDT_PROP_NONE,
                    NULL_HINT, NULL );

    for( i = 0; i < 6; i++ ){

        ocrDbCreate( &panelDb, (void **)&panel, sizeof(panel_t), 0, NULL_HINT, NO_ALLOC );
        ocrDbRelease( panelDb );
        ocrAddDependence( panelDb, realmainGUID, i, DB_MODE_RW );

    }


    return NULL_GUID;
}
