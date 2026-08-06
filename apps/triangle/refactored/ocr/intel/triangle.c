/*
Author: David Scott
Copywrite Intel Corporation 2015

 This file is subject to the license agreement located in the file ../../../../LICENSE (apps/LICENSE)
 and cannot be distributed without it. This notice cannot be removed or modified.

*/

/*
This code implements a recursive search of the game tree of the "14 peg puzzle" in OCR
to count the number of solutions.

Each spawned task returns its subtree's solution count in an 8-byte block
carried on its completion event; a summing continuation per spawner adds the
children's counts and forwards the total upward, so the recursion's return
path is the reduction tree and no shared counter exists.

See the README file for more information.

*/

#include <ocr.h>
#include <stdlib.h>
#define BOARDSIZE 15
#define MOVESIZE 36

#define BOTTOM 13


/*
void printboard(u64 board[15]) {
    ocrPrintf("board\n");
    ocrPrintf("          %3d \n", board[0]);
    ocrPrintf("         %3d %3d \n", board[1], board[2]);
    ocrPrintf("        %3d %3d %3d \n", board[3], board[4], board[5]);
    ocrPrintf("       %3d %3d %3d %3d \n", board[6], board[7], board[8], board[9]);
    ocrPrintf("      %3d %3d %3d %3d %3d\n", board[10], board[11], board[12], board[13], board[14]);
    return ;
}
*/

/* Deliver a subtree's solution count upward: an 8-byte block carried on the
 * parent's completion event is the scalar return channel. */
static void returnCount(ocrGuid_t parentEvent, u64 count) {
    ocrGuid_t db;
    u64 *ptr;
    ocrDbCreate(&db, (void**)&ptr, sizeof(u64), 0, NULL_HINT, NO_ALLOC);
    *ptr = count;
    ocrDbRelease(db);
    ocrEventSatisfy(parentEvent, db);
}

/* Sums the children's returned counts and forwards the total upward. */
ocrGuid_t sumCountsTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrGuid_t parentEvent = (ocrGuid_t){.guid = paramv[0]};
    u64 total = 0;
    u64 i;
    for(i = 0; i < depc; i++) {
        total += *(u64*)depv[i].ptr;
        ocrDbDestroy(depv[i].guid);
    }
    returnCount(parentEvent, total);
    return NULL_GUID;
}

ocrGuid_t triangleTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
/*
paramv
0: nummoves
1: oldmove
2: triangleTemplate
3: parentEvent (receives this subtree's solution count)
4: depth (number of moves to search; BOTTOM solves the full puzzle)
depv
0: oldboard
1: board
2: moveblock
make move
check for bottom
look for legal moves
*/

    u64 nummoves = paramv[0];
    u64 oldmove = paramv[1];
    ocrGuid_t triangleTemplate = (ocrGuid_t){.guid = paramv[2]};
    ocrGuid_t parentEvent = (ocrGuid_t){.guid = paramv[3]};
    u64 depth = paramv[4];
    u64 * oldboard = depv[0].ptr;
    u64 * board = depv[1].ptr;
    u64 * pmoves = depv[2].ptr;
    ocrGuid_t newboardDb;
    ocrGuid_t triangleEdt, once;
    u64 i;
    u64 *newboard;
//ocrPrintf("starting Triangle with nummoves %d oldmove %d \n", nummoves, oldmove);
    for(i=0;i<BOARDSIZE;i++) board[i] = oldboard[i];
    if(oldmove != -1){
        nummoves++;
        board[pmoves[3*oldmove]] = 0;
        board[pmoves[3*oldmove+1]] = 0;
        board[pmoves[3*oldmove+2]] = 1;
    }
//printboard(board);
    if(nummoves == depth){
        /* a full line of play reaching the requested depth is one solution */
        returnCount(parentEvent, 1);
        return NULL_GUID;
    }

    u64 nlegal = 0;
    for(i=0;i<MOVESIZE;i++)
        if(board[pmoves[3*i]] && board[pmoves[3*i+1]] && (!board[pmoves[3*i+2]])) nlegal++;
    if(nlegal == 0){
        /* dead position short of the requested depth */
        returnCount(parentEvent, 0);
        return NULL_GUID;
    }

    ocrGuid_t sumTemplate, sumEdt;
    u64 sumParamv[1] = { parentEvent.guid };
    ocrEdtTemplateCreate(&sumTemplate, sumCountsTask, 1, nlegal);
    ocrEdtCreate(&sumEdt, sumTemplate, EDT_PARAM_DEF, sumParamv, EDT_PARAM_DEF, NULL,
                 EDT_PROP_NONE, NULL_HINT, NULL);
    ocrEdtTemplateDestroy(sumTemplate);

    u64 triangleParamv[5] = {nummoves, 0, triangleTemplate.guid, 0, depth};
    ocrEventCreate(&once, OCR_EVENT_ONCE_T, true);
    u64 slot = 0;
    for(i=0;i<MOVESIZE;i++) {
        if(board[pmoves[3*i]] && board[pmoves[3*i+1]] && (!board[pmoves[3*i+2]])) { //legal move
            ocrDbCreate(&newboardDb, (void**) &newboard, sizeof(u64)*BOARDSIZE, 0, NULL_HINT, NO_ALLOC);
            /* never written here: release before wiring so the child is the
             * sole writer of its board */
            ocrDbRelease(newboardDb);

            /* wire the child's return event into the summer before the child
             * exists, so it cannot fire unregistered */
            ocrGuid_t childDone;
            ocrEventCreate(&childDone, OCR_EVENT_ONCE_T, true);
            ocrAddDependence(childDone, sumEdt, slot, DB_MODE_RO);

            triangleParamv[1] = i;
            triangleParamv[3] = childDone.guid;
            ocrEdtCreate(&triangleEdt, triangleTemplate, EDT_PARAM_DEF, triangleParamv, EDT_PARAM_DEF, NULL, EDT_PROP_NONE, NULL_HINT, NULL);
            ocrAddDependence(once, triangleEdt, 0, DB_MODE_CONST);
            ocrAddDependence(newboardDb, triangleEdt, 1, DB_MODE_RW);
            ocrAddDependence(depv[2].guid, triangleEdt, 2, DB_MODE_CONST);
            slot++;
        }
    }
    ocrDbRelease(depv[1].guid);
    ocrEventSatisfy(once, depv[1].guid);
    return NULL_GUID;
}
//print final count
//paramv 0: depth.  The known answer (29760 solutions) only applies to the
//full-depth puzzle; for a partial search the count is reported as-is.
static void launch_round(u64 depth, u64 rounds_left);

/* paramv: {depth, rounds_left} — sequential full-search repetitions chain
 * through the wrapup EDT; all round state travels in paramv.
 * depv[0]: the root subtree's count block. */
ocrGuid_t wrapupTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 depth = paramv[0];
    u64 rounds_left = paramv[1];
    u64 * count = depv[0].ptr;
    if(rounds_left > 1) {
        ocrDbDestroy(depv[0].guid);
        launch_round(depth, rounds_left - 1);
        return NULL_GUID;
    }
    if(depth == BOTTOM) {
        if(*count == 29760) ocrPrintf("PASS  final count %d \n", *count);
            else ocrPrintf("FAIL final count %d should be 29760 \n", *count);
    } else {
        ocrPrintf("final count %d at depth %d \n", *count, depth);
    }
    ocrDbDestroy(depv[0].guid);

    ocrShutdown();
    return NULL_GUID;
}
ocrGuid_t realmainTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]){
/*
params
0: depth (number of moves to search)
1: rounds_left
depv
0: oldboard
1: board
2: move block
initialize datablocks
create triangleEdt
create and launch wrapup
launch triangleEdt
*/

    u64 depth = paramv[0];
    u64 nummoves = 0;
    u64 i, j;
    ocrGuid_t triangleTemplate, triangleEdt;
    u64 oldmove;
    u64 *oldboard = depv[0].ptr;
    u64 *pmoves = depv[2].ptr;
//initialize pmoves
    u64 ptemp[MOVESIZE][3] ={
        {0,1,3},
        {3,1,0},
        {0,2,5},
        {5,2,0},
        {3,4,5},
        {5,4,3},
        {1,3,6},
        {6,3,1},
        {1,4,8},
        {8,4,1},
        {2,4,7},
        {7,4,2},
        {2,5,9},
        {9,5,2},
        {6,7,8},
        {8,7,6},
        {7,8,9},
        {9,8,7},
        {3,6,10},
        {10,6,3},
        {3,7,12},
        {12,7,3},
        {4,7,11},
        {11,7,4},
        {4,8,13},
        {13,8,4},
        {5,8,12},
        {12,8,5},
        {5,9,14},
        {14,9,5},
        {10,11,12},
        {12,11,10},
        {11,12,13},
        {13,12,11},
        {12,13,14},
        {14,13,12}
        };
    for(i=0;i<MOVESIZE;i++)for(j=0;j<3;j++)  pmoves[3*i+j] = ptemp[i][j];
//initialize oldboard
    u64 btemp[BOARDSIZE] = {0,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
    for(i=0;i<BOARDSIZE;i++) oldboard[i] = btemp[i];
    ocrEdtTemplateCreate(&triangleTemplate, triangleTask, 5, 3);
    oldmove = -1;
//the root's count block arrives at wrapup on this event
    ocrGuid_t rootDone;
    ocrEventCreate(&rootDone, OCR_EVENT_ONCE_T, true);
    u64 triangleParamv[5] = {nummoves, oldmove, triangleTemplate.guid, rootDone.guid, depth};
    ocrEdtCreate(&triangleEdt, triangleTemplate, EDT_PARAM_DEF, triangleParamv,
                 EDT_PARAM_DEF, NULL, EDT_PROP_NONE, NULL_HINT, NULL);
    // create and launch wrapup
    ocrGuid_t wrapupTemplate;
    ocrGuid_t wrapupEdt;
    u64 wparams[2] = { depth, paramv[1] };
    ocrEdtTemplateCreate(&wrapupTemplate, wrapupTask, 2, 1);
    ocrEdtCreate(&wrapupEdt, wrapupTemplate, EDT_PARAM_DEF, wparams, EDT_PARAM_DEF,
                 NULL, EDT_PROP_NONE, NULL_HINT, NULL);
    ocrAddDependence(rootDone, wrapupEdt, 0, DB_MODE_RO);
//launch triangleEdt
    ocrDbRelease(depv[0].guid);
    ocrAddDependence(depv[0].guid, triangleEdt, 0, DB_MODE_CONST);
    ocrDbRelease(depv[1].guid);
    ocrAddDependence(depv[1].guid, triangleEdt, 1, DB_MODE_RW);
    ocrDbRelease(depv[2].guid);
    ocrAddDependence(depv[2].guid, triangleEdt, 2, DB_MODE_CONST);
    return NULL_GUID;
}
static void launch_round(u64 depth, u64 rounds_left) {
    ocrGuid_t realmain, realmainTemplate, boardDb, oldboardDb, pmovesDb;
    u64 *oldboard, *board, *pmoves;

ocrDbCreate(&oldboardDb, (void **)&oldboard, sizeof(u64) * BOARDSIZE, 0,
            NULL_HINT, NO_ALLOC);
ocrDbCreate(&boardDb, (void **)&board, sizeof(u64) * BOARDSIZE, 0, NULL_HINT,
            NO_ALLOC);
ocrDbCreate(&pmovesDb, (void **)&pmoves, sizeof(u64) * MOVESIZE * 3, 0,
            NULL_HINT, NO_ALLOC);
u64 rparams[2] = { depth, rounds_left };
ocrEdtTemplateCreate(&realmainTemplate, realmainTask, 2, 3);
ocrEdtCreate(&realmain, realmainTemplate, EDT_PARAM_DEF, rparams, EDT_PARAM_DEF,
             NULL, EDT_PROP_NONE, NULL_HINT, NULL);
ocrAddDependence(oldboardDb, realmain, 0, DB_MODE_RW);
ocrAddDependence(boardDb, realmain, 1, DB_MODE_RW);
// the initializer must acquire the table writable; CONST grants a non-written-back copy
ocrAddDependence(pmovesDb, realmain, 2, DB_MODE_RW);
}

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]){
//optional argument: search depth in moves; the full puzzle (BOTTOM moves) by default
    u64 depth = BOTTOM;
    if(ocrGetArgc(depv[0].ptr) > 1) depth = (u64) atoi(ocrGetArgv(depv[0].ptr, 1));
    if(depth < 1 || depth > BOTTOM) depth = BOTTOM;
    u64 rounds = 1;
    ocrPrintf("triangle puzzle depth %d \n", depth);
    launch_round(depth, rounds);
    return NULL_GUID;
}
