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
#include <extensions/ocr-affinity.h>
#include <stdlib.h>
#define BOARDSIZE 15
#define MOVESIZE 36

#define BOTTOM 13

/* The constants above are the 5-row board written out: holes = r(r+1)/2, a
 * full game is holes-2 moves (every jump removes one peg and needs two).
 * `rows` (argv[3]): absent = the author's table drives, verbatim; given (5
 * included) = the generator drives.  On the author's board the generator is
 * checked against the author's table at startup.  The placement key is a
 * u64 hole bitmask, so holes <= 64 (rows <= 10). */
#define TRI_ROWS_DEFAULT 5
#define TRI_ROWS_MAX 10

static u64 triHoles(u64 rows) { return rows * (rows + 1) / 2; }
static u64 triHoleIdx(u64 i, u64 j) { return i * (i + 1) / 2 + j; }

/* Enumerate the (from, over, to) jumps of a rows-row board into `moves`
 * (when non-NULL); return the count.  Jumps run along the triangle's three
 * axes, both directions; legal iff over and landing are on the board. */
static u64 triGenMoves(u64 rows, u64 *moves) {
    static const s64 dir[6][2] = {{0,1},{0,-1},{1,0},{-1,0},{1,1},{-1,-1}};
    u64 n = 0;
    s64 i, j;
    u64 d;
    for(i = 0; i < (s64)rows; i++)
        for(j = 0; j <= i; j++)
            for(d = 0; d < 6; d++) {
                s64 oi = i + dir[d][0], oj = j + dir[d][1];
                s64 ti = i + 2*dir[d][0], tj = j + 2*dir[d][1];
                if(ti < 0 || ti >= (s64)rows || tj < 0 || tj > ti) continue;
                if(oi < 0 || oi >= (s64)rows || oj < 0 || oj > oi) continue;
                if(moves) {
                    moves[3*n]   = triHoleIdx((u64)i, (u64)j);
                    moves[3*n+1] = triHoleIdx((u64)oi, (u64)oj);
                    moves[3*n+2] = triHoleIdx((u64)ti, (u64)tj);
                }
                n++;
            }
    return n;
}

/* Levels of the search tree (moves made) scattered across ranks by a
 * deterministic hash of the board bitmask — placement is a pure function of
 * the position, independent of creation order; deeper tasks pin to the
 * creating rank so each scattered subtree runs wire-free.  Calibrated by
 * measurement. */
#ifndef TRIANGLE_SCATTER_LEVELS
#define TRIANGLE_SCATTER_LEVELS 3
#endif


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

#ifdef OCR_APP_OPTIMIZED_PLACEMENT
/* Finalizer-style bit mix: a board bitmask carries semantically-fixed low
 * bits (the opening cells every legal line of play must touch), so a raw
 * modulus is parity-biased toward one rank — mixing first makes the residue
 * uniform across sibling subtrees. */
static inline u64 mixKey(u64 x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/* Top-level children scatter on the hashed board bitmask; deeper children
 * stay on the creating rank so the subtree beneath them is wire-free. */
static ocrHint_t * triChildEdtHint(ocrHint_t * h, u64 level, u64 key) {
    u64 nranks;
    ocrAffinityCount(AFFINITY_PD, &nranks);
    ocrGuid_t aff;
    if(level <= TRIANGLE_SCATTER_LEVELS) ocrAffinityGetAt(AFFINITY_PD, mixKey(key) % nranks, &aff);
    else ocrAffinityGetCurrent(&aff);
    ocrHintInit(h, OCR_HINT_EDT_T);
    ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
}

/* Pin the summing continuation to the creating rank. */
static ocrHint_t * triLocalEdtHint(ocrHint_t * h) {
    ocrGuid_t aff;
    ocrAffinityGetCurrent(&aff);
    ocrHintInit(h, OCR_HINT_EDT_T);
    ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
}
#endif /* OCR_APP_OPTIMIZED_PLACEMENT */

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
    /* This task runs once every child has returned, so it is the first — and
     * only — point at which nobody holds this node's board any more. */
    {
        ocrGuid_t board = (ocrGuid_t){.guid = paramv[1]};
        ocrDbDestroy(board);
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
4: depth (number of moves to search; holes-2 solves the full puzzle)
5: holes (board size for this run's rows)
6: nmoves (jump-table entries for this run's rows)
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
    u64 holes = paramv[5];
    u64 nmoves = paramv[6];
    u64 * oldboard = depv[0].ptr;
    u64 * board = depv[1].ptr;
    u64 * pmoves = depv[2].ptr;
    ocrGuid_t newboardDb;
    ocrGuid_t triangleEdt, once;
    u64 i;
    u64 *newboard;
//ocrPrintf("starting Triangle with nummoves %d oldmove %d \n", nummoves, oldmove);
    for(i=0;i<holes;i++) board[i] = oldboard[i];
    if(oldmove != -1){
        nummoves++;
        board[pmoves[3*oldmove]] = 0;
        board[pmoves[3*oldmove+1]] = 0;
        board[pmoves[3*oldmove+2]] = 1;
    }
//printboard(board);
    if(nummoves == depth){
        /* a full line of play reaching the requested depth is one solution */
        /* no children and no summer: the board's last reachable point */
        ocrDbDestroy(depv[1].guid);
        returnCount(parentEvent, 1);
        return NULL_GUID;
    }

    u64 nlegal = 0;
    for(i=0;i<nmoves;i++)
        if(board[pmoves[3*i]] && board[pmoves[3*i+1]] && (!board[pmoves[3*i+2]])) nlegal++;
    if(nlegal == 0){
        /* dead position short of the requested depth */
        ocrDbDestroy(depv[1].guid);
        returnCount(parentEvent, 0);
        return NULL_GUID;
    }

    ocrGuid_t sumTemplate, sumEdt;
    u64 sumParamv[2] = { parentEvent.guid, depv[1].guid.guid };
    ocrEdtTemplateCreate(&sumTemplate, sumCountsTask, 2, nlegal);
    {
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
        ocrHint_t sumHint;
        ocrHint_t *sumH = triLocalEdtHint(&sumHint);
#else
        ocrHint_t *sumH = NULL_HINT;
#endif
        ocrEdtCreate(&sumEdt, sumTemplate, EDT_PARAM_DEF, sumParamv, EDT_PARAM_DEF, NULL,
                     EDT_PROP_NONE, sumH, NULL);
    }
    ocrEdtTemplateDestroy(sumTemplate);

#ifdef OCR_APP_OPTIMIZED_PLACEMENT
    /* board occupancy as a bitmask: the deterministic placement key */
    u64 bits = 0;
    for(i=0;i<holes;i++) if(board[i]) bits |= (1ull << i);
#endif

    u64 triangleParamv[7] = {nummoves, 0, triangleTemplate.guid, 0, depth, holes, nmoves};
#ifdef OCR_APP_COUNTED_EVENTS
    /* The consumer counts are known before each event exists (nlegal here,
     * one summer slot per childDone, one wrapup for rootDone), so COUNTED
     * gives the runtime the reclaim point a ONCE cannot have. */
    {
      ocrEventParams_t onceParams;
      onceParams.EVENT_COUNTED.nbDeps = nlegal;
      ocrEventCreateParams(&once, OCR_EVENT_COUNTED_T, true, &onceParams);
    }
#else
    ocrEventCreate(&once, OCR_EVENT_ONCE_T, true);
#endif
    u64 slot = 0;
    for(i=0;i<nmoves;i++) {
        if(board[pmoves[3*i]] && board[pmoves[3*i+1]] && (!board[pmoves[3*i+2]])) { //legal move
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
            u64 childBits = (bits & ~(1ull<<pmoves[3*i]) & ~(1ull<<pmoves[3*i+1]))
                            | (1ull<<pmoves[3*i+2]);
            ocrHint_t edtHint;
            ocrHint_t *childHint = triChildEdtHint(&edtHint, nummoves+1, childBits);
#else
            ocrHint_t *childHint = NULL_HINT;
#endif

            ocrDbCreate(&newboardDb, (void**) &newboard, sizeof(u64)*holes, 0, NULL_HINT, NO_ALLOC);
            /* never written here: release before wiring so the child is the
             * sole writer of its board */
            ocrDbRelease(newboardDb);

            /* wire the child's return event into the summer before the child
             * exists, so it cannot fire unregistered */
            ocrGuid_t childDone;
#ifdef OCR_APP_COUNTED_EVENTS
            {
              ocrEventParams_t cdParams;
              cdParams.EVENT_COUNTED.nbDeps = 1;
              ocrEventCreateParams(&childDone, OCR_EVENT_COUNTED_T, true,
                                   &cdParams);
            }
#else
            ocrEventCreate(&childDone, OCR_EVENT_ONCE_T, true);
#endif
            ocrAddDependence(childDone, sumEdt, slot, DB_MODE_RO);

            triangleParamv[1] = i;
            triangleParamv[3] = childDone.guid;
            ocrEdtCreate(&triangleEdt, triangleTemplate, EDT_PARAM_DEF, triangleParamv, EDT_PARAM_DEF, NULL, EDT_PROP_NONE, childHint, NULL);
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
static void launch_round(u64 depth, u64 rounds_left, u64 rows);

/* paramv: {depth, rounds_left} — sequential full-search repetitions chain
 * through the wrapup EDT; all round state travels in paramv.
 * depv[0]: the root subtree's count block. */
ocrGuid_t wrapupTask(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 depth = paramv[0];
    u64 rounds_left = paramv[1];
    u64 rows = paramv[2];
    u64 * count = depv[0].ptr;
    if(rounds_left > 1) {
        ocrDbDestroy(depv[0].guid);
        launch_round(depth, rounds_left - 1, rows);
        return NULL_GUID;
    }
    /* 29,760 is the author's puzzle's constant; larger boards are judged by
     * cross-configuration consensus. */
    if((rows == 0 || rows == TRI_ROWS_DEFAULT) && depth == BOTTOM) {
        if(*count == 29760) ocrPrintf("PASS  final count %d \n", *count);
            else ocrPrintf("FAIL final count %d should be 29760 \n", *count);
    } else {
        ocrPrintf("final count %d at depth %d rows %d \n", *count, depth,
                  rows ? rows : TRI_ROWS_DEFAULT);
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
2: rows (board rows; TRI_ROWS_DEFAULT is the author's puzzle)
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
    u64 rows = paramv[2]; /* 0 = not given: the author's board and table */
    u64 erows = rows ? rows : TRI_ROWS_DEFAULT;
    u64 holes = triHoles(erows);
    u64 nmoves;
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
    if(erows == TRI_ROWS_DEFAULT) {
        /* The generator must reproduce the author's table as a SET (order
         * may differ) — the one independent oracle, checked every start. */
        u64 gen[MOVESIZE*3];
        u64 k, found = 0;
        if(triGenMoves(erows, gen) != MOVESIZE) {
            ocrPrintf("triangle: move generator count mismatch on the author's board\n");
            ocrAbort(1);
        }
        for(i=0;i<MOVESIZE;i++) {
            for(k=0;k<MOVESIZE;k++)
                if(gen[3*k] == ptemp[i][0] && gen[3*k+1] == ptemp[i][1] &&
                   gen[3*k+2] == ptemp[i][2]) { found++; break; }
        }
        if(found != MOVESIZE) {
            ocrPrintf("triangle: move generator disagrees with the author's table\n");
            ocrAbort(1);
        }
        nmoves = MOVESIZE;
        if(rows == 0) /* author mode: the author's ordering drives the run */
            for(i=0;i<MOVESIZE;i++)for(j=0;j<3;j++)  pmoves[3*i+j] = ptemp[i][j];
        else          /* explicit size: the generator drives, 5 included */
            for(i=0;i<MOVESIZE*3;i++) pmoves[i] = gen[i];
    } else {
        nmoves = triGenMoves(erows, pmoves);
    }
//initialize oldboard: the apex empty, every other hole pegged
    oldboard[0] = 0;
    for(i=1;i<holes;i++) oldboard[i] = 1;
    ocrEdtTemplateCreate(&triangleTemplate, triangleTask, 7, 3);
    oldmove = -1;
//the root's count block arrives at wrapup on this event
    ocrGuid_t rootDone;
#ifdef OCR_APP_COUNTED_EVENTS
    {
      ocrEventParams_t rdParams;
      rdParams.EVENT_COUNTED.nbDeps = 1;
      ocrEventCreateParams(&rootDone, OCR_EVENT_COUNTED_T, true, &rdParams);
    }
#else
    ocrEventCreate(&rootDone, OCR_EVENT_ONCE_T, true);
#endif
    u64 triangleParamv[7] = {nummoves, oldmove, triangleTemplate.guid, rootDone.guid, depth, holes, nmoves};
    ocrEdtCreate(&triangleEdt, triangleTemplate, EDT_PARAM_DEF, triangleParamv,
                 EDT_PARAM_DEF, NULL, EDT_PROP_NONE, NULL_HINT, NULL);
    // create and launch wrapup
    ocrGuid_t wrapupTemplate;
    ocrGuid_t wrapupEdt;
    u64 wparams[3] = { depth, paramv[1], rows };
    ocrEdtTemplateCreate(&wrapupTemplate, wrapupTask, 3, 1);
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
static void launch_round(u64 depth, u64 rounds_left, u64 rows) {
    ocrGuid_t realmain, realmainTemplate, boardDb, oldboardDb, pmovesDb;
    u64 *oldboard, *board, *pmoves;
    u64 erows = rows ? rows : TRI_ROWS_DEFAULT;
    u64 holes = triHoles(erows);
    u64 nmoves = triGenMoves(erows, NULL);

ocrDbCreate(&oldboardDb, (void **)&oldboard, sizeof(u64) * holes, 0,
            NULL_HINT, NO_ALLOC);
ocrDbCreate(&boardDb, (void **)&board, sizeof(u64) * holes, 0, NULL_HINT,
            NO_ALLOC);
ocrDbCreate(&pmovesDb, (void **)&pmoves, sizeof(u64) * nmoves * 3, 0,
            NULL_HINT, NO_ALLOC);
u64 rparams[3] = { depth, rounds_left, rows };
ocrEdtTemplateCreate(&realmainTemplate, realmainTask, 3, 3);
ocrEdtCreate(&realmain, realmainTemplate, EDT_PARAM_DEF, rparams, EDT_PARAM_DEF,
             NULL, EDT_PROP_NONE, NULL_HINT, NULL);
ocrAddDependence(oldboardDb, realmain, 0, DB_MODE_RW);
ocrAddDependence(boardDb, realmain, 1, DB_MODE_RW);
// the initializer must acquire the table writable; CONST grants a non-written-back copy
ocrAddDependence(pmovesDb, realmain, 2, DB_MODE_RW);
}

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]){
//optional arguments: search depth in moves (full board by default), rounds,
//board rows (the author's 5-row puzzle by default)
    u64 argc = ocrGetArgc(depv[0].ptr);
    u64 rows = 0; /* absent: the author's board, the author's table */
    if(argc > 3) {
        u64 rr = (u64) atoi(ocrGetArgv(depv[0].ptr, 3));
        if(rr >= 3 && rr <= TRI_ROWS_MAX) rows = rr;
    }
    u64 maxdepth = triHoles(rows ? rows : TRI_ROWS_DEFAULT) - 2;
    u64 depth = maxdepth;
    if(argc > 1) depth = (u64) atoi(ocrGetArgv(depv[0].ptr, 1));
    if(depth < 1 || depth > maxdepth) depth = maxdepth;
    u64 rounds = 1;
    if(argc > 2) {
        u64 r = (u64) atoi(ocrGetArgv(depv[0].ptr, 2));
        if(r >= 1) rounds = r;
    }
    if(rows == 0)
        ocrPrintf("triangle puzzle depth %d \n", depth);
    else
        ocrPrintf("triangle puzzle depth %d rows %d \n", depth, rows);
    launch_round(depth, rounds, rows);
    return NULL_GUID;
}
