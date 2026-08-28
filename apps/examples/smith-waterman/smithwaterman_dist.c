/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

/* Tiled Smith-Waterman whose task graph is built where it runs.
 *
 * The wavefront is not the constraint.  A W x W tile grid has W^2 tasks, an
 * anti-diagonal critical path of 2W-1, and so about W/2 of average
 * concurrency -- thousands of ready tasks at the sizes this runs at, far more
 * than any machine here offers.  What bounds the port this re-implements is
 * that ONE task builds the whole graph: a readiness event trio per tile, then
 * a task per tile with four dependences each.  Three O(W^2) loops on a single
 * rank, and a tile cannot start before the creator has reached it, so the run
 * cannot end before the loops do.  More workers do not help -- there is
 * nothing for them to take -- and more ranks hurt, because a task placed
 * elsewhere is a message.
 *
 * Here the tiles are divided across `places` and each place builds the tasks
 * for the tiles it owns, on itself.  Names are the mechanism: the readiness
 * events come from one labeled range, so a place derives the name of any
 * tile's event arithmetically and is never told it.  A place creates only the
 * events of tiles it owns, so each name is created exactly once, and a
 * consumer may register a dependence on a name whose object does not exist
 * yet.
 *
 * The tile-to-place map is cyclic on both axes.  Over a BLOCKED map a
 * wavefront leaves most places idle -- an anti-diagonal of blocks is only
 * O(sqrt(P)) wide -- while a cyclic map puts every place on every
 * anti-diagonal.
 */

#include "ocr.h"
#include "extensions/ocr-affinity.h"
#include "extensions/ocr-labeling.h"

#include <stdio.h>
#include <stdlib.h>
#include "macros.h"

/* Placement-optimization layer: contiguous row-band placement for the
 * wavefront.  A tile's West neighbour shares its row and therefore its rank;
 * North/NW share its band on every row except the nranks-1 band-boundary
 * rows, so the heavy row-to-row payload stays rank-local while the
 * anti-diagonal frontier still reaches every band once it is a band tall.
 * A round-robin map makes EVERY neighbour remote instead. */
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
#include <extensions/ocr-affinity.h>
static ocrHint_t * swBandEdtHint(ocrHint_t *h, u64 row, u64 nrows) {
    u64 nranks;
    ocrAffinityCount(AFFINITY_PD, &nranks);
    if (nranks <= 1 || nrows == 0) return NULL_HINT;
    u64 band = (row * nranks) / nrows;
    if (band >= nranks) band = nranks - 1;
    ocrGuid_t aff;
    ocrAffinityGetAt(AFFINITY_PD, band, &aff);
    ocrHintInit(h, OCR_HINT_EDT_T);
    ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
}
#else
#define swBandEdtHint(h, row, nrows) NULL_HINT
#endif

#define GAP_PENALTY -1
#define TRANSITION_PENALTY -2
#define TRANSVERSION_PENALTY -4
#define MATCH 2

enum Nucleotide {GAP=0, ADENINE, CYTOSINE, GUANINE, THYMINE};

/* Three readiness events per tile, laid out so any of them follows from the
 * tile coordinate alone: no table travels and none is consulted.
 *
 * The three are strided by the tile count rather than adjacent.  A reserved
 * range is homed by index, so adjacent indices put a tile's own three outputs
 * on three different ranks and every satisfy leaves the node that produced it.
 * Striding by a multiple of the rank count gives all three the same home. */
#define EVBR  0   /* bottom row */
#define EVRC  1   /* right column */
#define EVBRC 2   /* bottom right */

/* A band builder runs twice: once to create the names of its own tiles, and
 * once to create the tasks that bind to them. */
#define PHASE_NAMES 0
#define PHASE_TASKS 1
/* A reserved range is homed by index modulo the rank count, so the index is
 * built to put a tile's events on the rank that owns the tile: number the
 * tiles densely WITHIN each rank's band, then interleave the ranks.  All three
 * of a tile's events share that home because the stride is a multiple of the
 * rank count. */
#define EVIDX(I,J,K,NW,STRIDE,ROW0,RANK,NR) \
    ((u64)(K)*(u64)(STRIDE) + \
     ((((u64)(I) - (u64)(ROW0))*((u64)(NW)+1) + (u64)(J)) * (u64)(NR) + (u64)(RANK)))
/* ROW0 must be the first row of the RANK, not of the place: several places can
 * share a rank, and numbering from each place's own first row would give two
 * tiles the same name. */

typedef struct{
    u64 i;
    u64 j;
    ocrGuid_t bottom_right_event_guid;
    ocrGuid_t right_column_event_guid;
    ocrGuid_t bottom_row_event_guid;
    /* The three this tile CONSUMES, in dependence-slot order.  Each readiness
     * event is named by exactly one dependence, so its single consumer is also
     * its last user and can reclaim it along with the block it carried.
     * Without this the grid is 3 events per tile alive for the whole run. */
    ocrGuid_t in_right_column_event_guid;
    ocrGuid_t in_bottom_row_event_guid;
    ocrGuid_t in_bottom_right_event_guid;
    u32 score;
}smithWatermanPRM_t;


s8 char_mapping ( char c ) {
    s8 to_be_returned = -1;
    switch(c) {
    case '_':
        to_be_returned = GAP;
        break;
    case 'A':
        to_be_returned = ADENINE;
        break;
    case 'C':
        to_be_returned = CYTOSINE;
        break;
    case 'G':
        to_be_returned = GUANINE;
        break;
    case 'T':
        to_be_returned = THYMINE;
        break;
    }
    return to_be_returned;
}

static char alignment_score_matrix[5][5] = {
    {GAP_PENALTY,GAP_PENALTY,GAP_PENALTY,GAP_PENALTY,GAP_PENALTY},
    {GAP_PENALTY,MATCH,TRANSVERSION_PENALTY,TRANSITION_PENALTY,TRANSVERSION_PENALTY},
    {GAP_PENALTY,TRANSVERSION_PENALTY, MATCH,TRANSVERSION_PENALTY,TRANSITION_PENALTY},
    {GAP_PENALTY,TRANSITION_PENALTY,TRANSVERSION_PENALTY, MATCH,TRANSVERSION_PENALTY},
    {GAP_PENALTY,TRANSVERSION_PENALTY,TRANSITION_PENALTY,TRANSVERSION_PENALTY, MATCH}
};

u32 clear_whitespaces_do_mapping ( s8* buffer, u32 size ) {
    u32 non_ws_index = 0, traverse_index = 0;

    while ( traverse_index < (u32)size ) {
        char curr_char = buffer[traverse_index];
        switch ( curr_char ) {
        case 'A':
        case 'C':
        case 'G':
        case 'T':
            /*this used to be a copy not also does mapping*/
            buffer[non_ws_index++] = char_mapping(curr_char);
            break;
        }
        ++traverse_index;
    }
    return non_ws_index;
}

#ifdef TG_ARCH
s8* read_file( s8* filestart, u32* n_chars ) {
    static FILE *file = NULL;
    s8* file_buffer;

    if(file==NULL) file = fopen((const char *)filestart, "r");
    u32 file_size = *n_chars;
    ocrGuid_t filebuf;
    ocrDbCreate(&filebuf, (void **)&file_buffer, sizeof(s8)*(1+file_size), DB_PROP_NONE, NULL_HINT, NO_ALLOC);
    fread(file_buffer, sizeof(s8), file_size, file);
    file_buffer[file_size] = '\0';

    // Clean up what has been read
    *n_chars =  clear_whitespaces_do_mapping(file_buffer, file_size);
    return file_buffer;
}
#else
s8* read_file( s8* filename, u32* n_chars ) {
    FILE* file = fopen(filename, "r");

    if (!file) {
        ocrPrintf("could not open file %s\n",filename);
        return NULL;
    }
    fseek (file, 0L, SEEK_END);
    s32 file_size = ftell (file);
    fseek (file, 0L, SEEK_SET);
    ocrGuid_t filebuf;

    s8 *file_buffer;
    ocrDbCreate( &filebuf, (void **)&file_buffer, sizeof(s8)*(1+file_size), DB_PROP_NONE, NULL_HINT, NO_ALLOC );

    fread(file_buffer, sizeof(s8), file_size, file);
    file_buffer[file_size] = '\0';

    /* shams' sample inputs have newlines in them */
    *n_chars = clear_whitespaces_do_mapping(file_buffer, file_size);
    return file_buffer;
}
#endif


ocrGuid_t smith_waterman_task ( u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    s32 index, ii, jj;

    /* Get the input datablock data pos32ers acquired from dependences */
    s32* left_tile_right_column = (s32 *) depv[0].ptr;
    s32* above_tile_bottom_row = (s32 *) depv[1].ptr;
    s32* diagonal_tile_bottom_right = (s32 *) depv[2].ptr;
    u64* dbparamv = (u64 *) depv[3].ptr;

    smithWatermanPRM_t *smithWatermanParamvIn = (smithWatermanPRM_t *)paramv;

    /* Unbox parameters */
    s32 i = (s32) smithWatermanParamvIn->i;
    s32 j = (s32) smithWatermanParamvIn->j;
    s32 tile_width = (s32) dbparamv[0];
    s32 tile_height = (s32) dbparamv[1];
    s32 n_tiles_height = (s32) dbparamv[2];
    s32 n_tiles_width = (s32) dbparamv[3];
    s8* string_1 = (s8* ) &dbparamv[dbparamv[4]];
    s8* string_2 = (s8* ) &dbparamv[dbparamv[5]];
    s32 string1_length = (s32) dbparamv[6];
    s32 string2_length = (s32) dbparamv[7];

    /* Calculate effective tile dimensions for edge tiles */
    s32 effective_tile_width = tile_width;
    s32 effective_tile_height = tile_height;
    
    /* For the rightmost column of tiles, width may be smaller */
    if (j == n_tiles_width) {
        s32 remaining_width = string1_length - (j-1)*tile_width;
        if (remaining_width < tile_width && remaining_width > 0) {
            effective_tile_width = remaining_width;
        }
    }
    
    /* For the bottom row of tiles, height may be smaller */
    if (i == n_tiles_height) {
        s32 remaining_height = string2_length - (i-1)*tile_height;
        if (remaining_height < tile_height && remaining_height > 0) {
            effective_tile_height = remaining_height;
        }
    }

    s32  * curr_tile_tmp;
    ocrGuid_t db_curr_tile_tmp, db_curr_tile;

    /* Allocate a haloed local matrix for calculating 'this' tile*/
    ocrDbCreate(&db_curr_tile_tmp, (void **)&curr_tile_tmp, sizeof(u32)*(1+tile_width)*(1+tile_height), DB_PROP_NONE, NULL_HINT, NO_ALLOC);
    s32 ** curr_tile;
    /* 2D-ify it for readability */
    ocrDbCreate(&db_curr_tile, (void **)&curr_tile, sizeof(u32 *)*(1+tile_height), DB_PROP_NONE, NULL_HINT, NO_ALLOC);

    for (index = 0; index < tile_height+1; ++index) {
        curr_tile[index] = &curr_tile_tmp[index*(1+tile_width)];
    }

    /* Initialize halo from neighbouring tiles */
    /* Set local_tile[0][0] (top left) from the bottom right of the northwest tile */
    curr_tile[0][0] = diagonal_tile_bottom_right[0];

    /* Set local_tile[i+1][0] (left column) from the right column of the left tile */
    for ( index = 1; index < effective_tile_height+1; ++index ) {
        curr_tile[index][0] = left_tile_right_column[index-1];
    }

    /* Set local_tile[0][j+1] (top row) from the bottom row of the above tile */
    for ( index = 1; index < effective_tile_width+1; ++index ) {
        curr_tile[0][index] = above_tile_bottom_row[index-1];
    }

    /* Run a smith-waterman on the local tile */
    for ( ii = 1; ii < effective_tile_height+1; ++ii ) {
        for ( jj = 1; jj < effective_tile_width+1; ++jj ) {
            s8 char_from_1 = string_1[(j-1)*tile_width+(jj-1)];
            s8 char_from_2 = string_2[(i-1)*tile_height+(ii-1)];

            /* Get score from northwest, north and west */
            s32 diag_score = curr_tile[ii-1][jj-1] + alignment_score_matrix[char_from_2][char_from_1];
            s32 left_score = curr_tile[ii  ][jj-1] + alignment_score_matrix[char_from_1][GAP];
            s32  top_score = curr_tile[ii-1][jj  ] + alignment_score_matrix[GAP][char_from_2];

            s32 bigger_of_left_top = (left_score > top_score) ? left_score : top_score;

            /* Set the local tile[i][j] to the maximum value of northwest, north and west */
            curr_tile[ii][jj] = (bigger_of_left_top > diag_score) ? bigger_of_left_top : diag_score;
        }
    }

    /* Allocate datablock for bottom right of the local tile */
    ocrGuid_t db_guid_i_j_br;
    void* db_guid_i_j_br_data;
    ocrDbCreate( &db_guid_i_j_br, &db_guid_i_j_br_data, sizeof(s32), DB_PROP_NONE, NULL_HINT, NO_ALLOC );

    /* Satisfy the bottom right event of local tile with the data block allocated above */
    s32* curr_bottom_right = (s32*)db_guid_i_j_br_data;
    curr_bottom_right[0] = curr_tile[effective_tile_height][effective_tile_width];

    ocrDbRelease(db_guid_i_j_br); // For now, no auto-release it seems...

    ocrGuid_t bottom_right_event_guid = smithWatermanParamvIn->bottom_right_event_guid;
    ocrEventSatisfy(bottom_right_event_guid, db_guid_i_j_br);

    /* Allocate datablock for right column of the local tile */
    ocrGuid_t db_guid_i_j_rc;
    void* db_guid_i_j_rc_data;
    ocrDbCreate( &db_guid_i_j_rc, &db_guid_i_j_rc_data, sizeof(s32)*tile_height, DB_PROP_NONE, NULL_HINT, NO_ALLOC );

    /* Satisfy the right column event of local tile with the data block allocated above */
    s32* curr_right_column = (s32*)db_guid_i_j_rc_data;
    for ( index = 0; index < effective_tile_height; ++index ) {
        curr_right_column[index] = curr_tile[index+1][effective_tile_width];
    }
    ocrDbRelease(db_guid_i_j_rc);
    ocrGuid_t right_column_event_guid = smithWatermanParamvIn->right_column_event_guid;
    ocrEventSatisfy(right_column_event_guid, db_guid_i_j_rc);

    /* Allocate datablock for bottom row of the local tile */
    ocrGuid_t db_guid_i_j_brow;
    s32* db_guid_i_j_brow_data = NULL;
    ocrDbCreate( &db_guid_i_j_brow, (void *)&db_guid_i_j_brow_data, sizeof(s32)*tile_width, DB_PROP_NONE, NULL_HINT, NO_ALLOC );

    /* Satisfy the bottom row event of local tile with the data block allocated above */
    s32* curr_bottom_row = (s32*)db_guid_i_j_brow_data;
    for ( index = 0; index < effective_tile_width; ++index ) {
        curr_bottom_row[index] = curr_tile[effective_tile_height][index+1];
    }
    ocrDbRelease(db_guid_i_j_brow);
    ocrGuid_t bottom_row_event_guid = smithWatermanParamvIn->bottom_row_event_guid;
    ocrEventSatisfy(bottom_row_event_guid, db_guid_i_j_brow);

    ocrDbDestroy(db_curr_tile);
    ocrDbDestroy(db_curr_tile_tmp);
    /* We can also free all the input DBs we get */
    ocrDbDestroy(depv[0].guid);
    ocrDbDestroy(depv[1].guid);
    ocrDbDestroy(depv[2].guid);
    /* and the events that carried them: this task is their only consumer */
    ocrEventDestroy(smithWatermanParamvIn->in_right_column_event_guid);
    ocrEventDestroy(smithWatermanParamvIn->in_bottom_row_event_guid);
    ocrEventDestroy(smithWatermanParamvIn->in_bottom_right_event_guid);
    /* If this is the last tile (bottom right most tile), finish */
    if ( i == n_tiles_height && j == n_tiles_width ) {
        ocrPrintf("score: %d\n", curr_bottom_row[effective_tile_width-1]);
        u32 score = smithWatermanParamvIn->score;
        VERIFY(curr_bottom_row[effective_tile_width-1] == score, "Expected score: %d\n", score);
        ocrShutdown();
    }
    return NULL_GUID;
}

static u32 __attribute__ ((noinline)) ioHandling ( void* marshalled, s32* p_n_tiles_height, s32* p_n_tiles_width, s32* p_tile_width, s32* p_tile_height, s8** p_string_1, s8** p_string_2, u32 *check_score, s32* p_string1_len, s32* p_string2_len) {
    u64 argc = ocrGetArgc(marshalled);

    if(argc < 6) {
#ifdef TG_ARCH
        ocrPrintf("Usage: %s tileWidth tileHeight string1Length string2Length scoreLength\n", ocrGetArgv(marshalled, 0)/*argv[0]*/);
#else
        ocrPrintf("Usage: %s tileWidth tileHeight fileName1 fileName2 scoreFile [places]\n", ocrGetArgv(marshalled, 0)/*argv[0]*/);
#endif
        return 1;
    }

    u32 n_char_in_file_1 = 0;
    u32 n_char_in_file_2 = 0;
    u32 n_char_in_file_score = 0;
    s8 *file_name_1;
    s8 *file_name_2;
    s8 *file_name_score;

#ifdef TG_ARCH
    *p_tile_width = (s32) atoi(ocrGetArgv(marshalled, 1));
    *p_tile_height = (s32) atoi(ocrGetArgv(marshalled, 2));
    n_char_in_file_1 = (s32) atoi(ocrGetArgv(marshalled, 3));
    n_char_in_file_2 = (s32) atoi(ocrGetArgv(marshalled, 4));
    n_char_in_file_score = (s32) atoi(ocrGetArgv(marshalled, 5));
    file_name_1 = NULL; // Doesn't matter anyway
    file_name_2 = NULL; // since the filename is immaterial
    file_name_score = NULL;
#else
    *p_tile_width = (s32) atoi(ocrGetArgv(marshalled, 1));
    *p_tile_height = (s32) atoi(ocrGetArgv(marshalled, 2));
    file_name_1 = ocrGetArgv(marshalled, 3);
    file_name_2 = ocrGetArgv(marshalled, 4);
    file_name_score = ocrGetArgv(marshalled, 5);
#endif

    *p_string_1 = read_file(file_name_1, &n_char_in_file_1);
    if(*p_string_1 == NULL) return 1;
    ocrPrintf("Size of input string 1 is %d\n", n_char_in_file_1 );
    *p_string1_len = (s32)n_char_in_file_1;

    *p_string_2 = read_file(file_name_2, &n_char_in_file_2);
    if(*p_string_2 == NULL) return 1;
    ocrPrintf("Size of input string 2 is %d\n", n_char_in_file_2 );
    *p_string2_len = (s32)n_char_in_file_2;

    *check_score = atoi((char *)read_file(file_name_score, &n_char_in_file_score));
    ocrPrintf("Score to get it %u\n", *check_score);
    ocrPrintf("Tile width is %d\n", *p_tile_width);
    ocrPrintf("Tile height is %d\n", *p_tile_height);

    /* Use ceiling division to handle partial tiles at edges */
    *p_n_tiles_width = (n_char_in_file_1 + *p_tile_width - 1) / *p_tile_width;
    *p_n_tiles_height = (n_char_in_file_2 + *p_tile_height - 1) / *p_tile_height;

    ocrPrintf("Imported %d x %d tiles.\n", *p_n_tiles_width, *p_n_tiles_height);

    ocrPrintf("Allocating tile matrix\n");
    return 0;
}

/* Ownership is the program's, not the runtime's, and for a wavefront it is a
 * BAND of rows rather than a scatter.
 *
 * A wavefront is a pipeline, not a general DAG.  Give a place a contiguous
 * band of rows and the only dependence that leaves it is the band's top edge:
 * one row of tiles per column, so O(P*W) crossings against the O(W^2) a
 * cyclic or round-robin map pays, every neighbour of which is remote.  The
 * price is pipeline fill -- a place cannot start until the band above has
 * produced its first column -- and that is P of the 2W-1 anti-diagonal steps,
 * under a percent at the sizes this runs at.  Scattering to avoid an idle
 * that small while paying W/P times the messages is the wrong trade. */
static u64 ownerOf(u64 i, u64 nTilesH, u64 places) {
    u64 g = (i * places) / (nTilesH + 1);
    return g >= places ? places - 1 : g;
}
static u64 bandFirstRow(u64 g, u64 nTilesH, u64 places) {
    return (g * (nTilesH + 1) + places - 1) / places;
}
/* Places are bands and ranks are bands, so the linear map is the right one:
 * it keeps a rank's places adjacent, which keeps a rank's rows adjacent, which
 * is the whole point of banding.  (A two-dimensional ownership would lose an
 * axis through this map; a one-dimensional one does not.) */
static u64 placeRank(u64 place, u64 places) {
    u64 nranks = 1;
    ocrAffinityCount(AFFINITY_PD, &nranks);
    if(nranks == 0) nranks = 1;
    return (place * nranks) / places;
}

static ocrHint_t edtHintAt(u64 rank) {
    ocrHint_t h; ocrGuid_t aff;
    ocrHintInit(&h, OCR_HINT_EDT_T);
    ocrAffinityGetAt(AFFINITY_PD, rank, &aff);
    ocrSetHintValue(&h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
}
static ocrHint_t dbHintAt(u64 rank) {
    ocrHint_t h; ocrGuid_t aff;
    ocrHintInit(&h, OCR_HINT_DB_T);
    ocrAffinityGetAt(AFFINITY_PD, rank, &aff);
    ocrSetHintValue(&h, OCR_HINT_DB_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
}

/* paramv {place, places, nTilesW, nTilesH, tileW, tileH, len1, len2, score,
 *         rangeGuid}
 * depv 0: the shared parameter block every tile task also reads
 * depv 1: the band above's readiness -- its last row's names must exist
 *         before this band binds to them
 *
 * Everything below is created HERE, on the place that will run it. */
ocrGuid_t placeInitTask ( u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 g = paramv[0], places = paramv[1];
    s32 nw = (s32)paramv[2], nh = (s32)paramv[3];
    s32 tw = (s32)paramv[4], th = (s32)paramv[5];
    s32 l1 = (s32)paramv[6], l2 = (s32)paramv[7];
    u32 score = (u32)paramv[8];
    ocrGuid_t range = (ocrGuid_t){.guid = paramv[9]};
    u64 evStride = paramv[10], nr = paramv[11];
    ocrGuid_t dbParamsGuid = depv[0].guid;
    u64 phase = paramv[12];
    ocrGuid_t namesDone = (ocrGuid_t){.guid = paramv[13]};
    u64 myRank = placeRank(g, places);
    ocrHint_t h = edtHintAt(myRank);
    ocrHint_t dh = dbHintAt(myRank);
    s32 i, j;

    /* First row of each rank: a rank's places are adjacent bands, so this is
     * the first row of its first place.  Scanned once, P is small. */
    u64 rankRow0[256];
    { u64 r, gg2;
      for ( r = 0; r < nr && r < 256; ++r ) rankRow0[r] = (u64)nh + 1;
      for ( gg2 = 0; gg2 < places; ++gg2 ) {
          u64 rr = placeRank(gg2, places);
          u64 r0 = bandFirstRow(gg2, (u64)nh, places);
          if ( rr < 256 && r0 < rankRow0[rr] ) rankRow0[rr] = r0;
      } }

    /* A name is derived from the tile's own band, so any place can name a tile
     * it does not own -- which is exactly what the top edge of a band needs. */
#define EVOF(I,J,K) ({ \
        u64 _g = ownerOf((u64)(I), (u64)nh, places); \
        u64 _r = placeRank(_g, places); \
        ocrGuid_t _e; \
        ocrGuidFromIndex(&_e, range, EVIDX((I),(J),(K),nw,evStride,rankRow0[_r],_r,nr)); \
        _e; })

    s32 rowLo = (s32)bandFirstRow(g, (u64)nh, places);
    s32 rowHi = (s32)bandFirstRow(g + 1, (u64)nh, places);
    if ( g + 1 >= places ) rowHi = nh + 1;

    /* Names before bindings, in two phases.  A task binds to the names of its
     * west and north neighbours, so a band can build its tasks only once its
     * own names and the names of the band above exist.  Creating every name
     * first -- a phase that carries no dependence at all -- lets all bands
     * create at once, and the task phase then waits on just those two
     * announcements.  Registering a dependence against a name that has not
     * been installed is not portable, and this ordering never needs to. */

    if ( phase == PHASE_NAMES ) {
        /* The border cells of this band. */
        if ( rowLo == 0 ) {
            ocrGuid_t db; void* p_; ocrDbCreate(&db, &p_, sizeof(s32), DB_PROP_NONE, &dh, NO_ALLOC);
            ((s32*)p_)[0] = 0; ocrDbRelease(db);
            ocrGuid_t e = EVOF(0,0,EVBRC);
            ocrEventCreate(&e, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG | GUID_PROP_IS_LABELED);
            ocrEventSatisfy(e, db);
            for ( j = 1; j < nw+1; ++j ) {
                s32 ew = (j == nw && (l1 % tw)) ? (l1 % tw) : tw, k;
                ocrGuid_t d1; void* q1; ocrDbCreate(&d1, &q1, sizeof(s32)*ew, DB_PROP_NONE, &dh, NO_ALLOC);
                for ( k = 0; k < ew; ++k ) ((s32*)q1)[k] = GAP_PENALTY*((j-1)*tw+k+1);
                ocrDbRelease(d1);
                ocrGuid_t e1 = EVOF(0,j,EVBR);
            ocrEventCreate(&e1, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG | GUID_PROP_IS_LABELED);
            ocrEventSatisfy(e1, d1);
                ocrGuid_t d2; void* q2; ocrDbCreate(&d2, &q2, sizeof(s32), DB_PROP_NONE, &dh, NO_ALLOC);
                ((s32*)q2)[0] = GAP_PENALTY*(j*tw > l1 ? l1 : j*tw); ocrDbRelease(d2);
                ocrGuid_t e2 = EVOF(0,j,EVBRC);
            ocrEventCreate(&e2, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG | GUID_PROP_IS_LABELED);
            ocrEventSatisfy(e2, d2);
            }
        }
        for ( i = (rowLo == 0 ? 1 : rowLo); i < rowHi; ++i ) {
            s32 eh = (i == nh && (l2 % th)) ? (l2 % th) : th, k;
            ocrGuid_t d1; void* q1; ocrDbCreate(&d1, &q1, sizeof(s32)*eh, DB_PROP_NONE, &dh, NO_ALLOC);
            for ( k = 0; k < eh; ++k ) ((s32*)q1)[k] = GAP_PENALTY*((i-1)*th+k+1);
            ocrDbRelease(d1);
            ocrGuid_t e1 = EVOF(i,0,EVRC);
            ocrEventCreate(&e1, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG | GUID_PROP_IS_LABELED);
            ocrEventSatisfy(e1, d1);
            ocrGuid_t d2; void* q2; ocrDbCreate(&d2, &q2, sizeof(s32), DB_PROP_NONE, &dh, NO_ALLOC);
            ((s32*)q2)[0] = GAP_PENALTY*(i*th > l2 ? l2 : i*th); ocrDbRelease(d2);
            ocrGuid_t e2 = EVOF(i,0,EVBRC);
            ocrEventCreate(&e2, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG | GUID_PROP_IS_LABELED);
            ocrEventSatisfy(e2, d2);
        }

        /* and the three a tile of this band will satisfy for its neighbours */
        for ( i = (rowLo == 0 ? 1 : rowLo); i < rowHi; ++i ) {
            for ( j = 1; j < nw+1; ++j ) {
                ocrGuid_t a = EVOF(i,j,EVBRC), b = EVOF(i,j,EVRC), c = EVOF(i,j,EVBR);
                ocrEventCreate(&a, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG | GUID_PROP_IS_LABELED);
                ocrEventCreate(&b, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG | GUID_PROP_IS_LABELED);
                ocrEventCreate(&c, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG | GUID_PROP_IS_LABELED);
            }
        }

        /* This band's names all exist now. */
        {   ocrGuid_t db; void *p_;
            ocrDbCreate(&db, &p_, sizeof(u64), DB_PROP_NONE, &dh, NO_ALLOC);
            *(u64*)p_ = g; ocrDbRelease(db);
            ocrEventSatisfy(namesDone, db);
        }
    } else {
        /* The tasks of this band. */
        ocrGuid_t tml;
        ocrEdtTemplateCreate(&tml, smith_waterman_task, PRMNUM(smithWaterman), 4);
        smithWatermanPRM_t prm;
        for ( i = (rowLo == 0 ? 1 : rowLo); i < rowHi; ++i ) {
            for ( j = 1; j < nw+1; ++j ) {
                prm.i = i; prm.j = j; prm.score = score;
                prm.bottom_right_event_guid = EVOF(i,j,EVBRC);
                prm.right_column_event_guid = EVOF(i,j,EVRC);
                prm.bottom_row_event_guid   = EVOF(i,j,EVBR);
                prm.in_right_column_event_guid = EVOF(i,j-1,EVRC);
                prm.in_bottom_row_event_guid   = EVOF(i-1,j,EVBR);
                prm.in_bottom_right_event_guid = EVOF(i-1,j-1,EVBRC);
                ocrGuid_t t;
                ocrEdtCreate(&t, tml, EDT_PARAM_DEF, (u64*)&prm, EDT_PARAM_DEF, NULL,
                             EDT_PROP_NONE, &h, NULL);
                ocrAddDependence(prm.in_right_column_event_guid, t, 0, DB_MODE_CONST);
                ocrAddDependence(prm.in_bottom_row_event_guid,   t, 1, DB_MODE_CONST);
                ocrAddDependence(prm.in_bottom_right_event_guid, t, 2, DB_MODE_CONST);
                ocrAddDependence(dbParamsGuid, t, 3, DB_MODE_CONST);
            }
        }
        ocrEdtTemplateDestroy(tml);
    }
#undef EVOF
    return NULL_GUID;
}

ocrGuid_t mainEdt ( u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    ocrAssert ( 0 == paramc );
    ocrAssert ( 1 == depc );

    s32 n_tiles_height;
    s32 n_tiles_width;
    s32 tile_width;
    s32 tile_height;
    s8* string_1;
    s8* string_2;
    u32 check_score;
    s32 string1_len;
    s32 string2_len;

    s32 n_places = 32;   /* the ownership decomposition; an argument, not the rank count */

    if(ioHandling(depv[0].ptr, &n_tiles_height, &n_tiles_width, &tile_width, &tile_height, &string_1, &string_2, &check_score, &string1_len, &string2_len))
    {
        ocrShutdown();
        return NULL_GUID;
    }

    s32 i;

    /* No tile matrix and no event grid here: the names come from a labeled
     * range and each place creates only what it owns. */
    u64 nranksNow = 1; ocrAffinityCount(AFFINITY_PD, &nranksNow);
    if ( nranksNow == 0 ) nranksNow = 1;
    /* Room for the largest band a rank can hold, times the interleave, and a
     * multiple of the rank count so a tile's three events share a home. */
    u64 evStride = ((u64)(n_tiles_height+1)/nranksNow + 2)*(n_tiles_width+1)*nranksNow + nranksNow;
    evStride = ((evStride + nranksNow - 1) / nranksNow) * nranksNow;
    ocrGuid_t evRange;
    if ( ocrGuidRangeCreate(&evRange, evStride*3, GUID_USER_EVENT_STICKY) != 0 ) {
        ocrPrintf("Cannot reserve %lu event names\n", evStride*3);
        ocrShutdown();
        return NULL_GUID;
    }

    // Common information to all tasks
    // Use actual string lengths for proper edge tile handling
    u64 string1Length = (u64)string1_len;
    u64 string2Length = (u64)string2_len;
    // Computing size of the strings in u64
    u64 string1Size = sizeof(s8) * string1Length;
    string1Size = ((string1Size%(sizeof(u64))) ? (string1Size/sizeof(u64))+1 : (string1Size/sizeof(u64)));
    u64 string2Size = sizeof(s8) * string2Length;
    string2Size = ((string2Size%(sizeof(u64))) ? (string2Size/sizeof(u64))+1 : (string2Size/sizeof(u64)));
    u64 dbHeaderSize = 8;  /* Added 2 more for actual string lengths */
    u64 dbSize = (dbHeaderSize + string1Size + string2Size)*sizeof(u64);
    // Computing DB's offsets for strings in the u64
    u64 string1Offset = dbHeaderSize;
    u64 string2Offset = (dbHeaderSize+string1Size);
    ocrGuid_t dbParamsGuid;
    u64 *params;
    ocrDbCreate(&dbParamsGuid, (void **)&params, dbSize, DB_PROP_NONE, NULL_HINT, NO_ALLOC);
    params[0]=(u64) tile_width;
    params[1]=(u64) tile_height;
    params[2]=(u64) n_tiles_height;
    params[3]=(u64) n_tiles_width;
    params[4]=string1Offset;
    params[5]=string2Offset;
    params[6]=string1Length;  /* Actual string 1 length for edge handling */
    params[7]=string2Length;  /* Actual string 2 length for edge handling */

    i = 0; // Writing string 1 in DB
    s8 * string1Ptr = (s8*) &params[string1Offset];
    while (i < string1Length) {
        string1Ptr[i] = string_1[i];
        i++;
    }

    i = 0; // Writing string 2 in DB
    s8 * string2Ptr = (s8*) &params[string2Offset];
    while (i < string2Length) {
        string2Ptr[i] = string_2[i];
        i++;
    }

    ocrDbRelease(dbParamsGuid);

    {   /* optional 6th argument: how many ownership partitions the tiles split into */
        void *marshalled = depv[0].ptr;
        if ( ocrGetArgc(marshalled) > 6 ) n_places = (s32) atoi(ocrGetArgv(marshalled, 6));
        if ( n_places < 1 ) n_places = 1;
    }

    /* One builder per place, and nothing else: O(P) work here. */
    u64 places = (u64)n_places, gg;
    /* One announcement per band, all created before any builder runs, so no
     * builder ever registers a dependence on a name that is not installed. */
    ocrGuid_t *namesDone = (ocrGuid_t*)malloc(places * sizeof(ocrGuid_t));
    for ( gg = 0; gg < places; ++gg )
        ocrEventCreate(&namesDone[gg], OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);

    ocrGuid_t placeTml;
    ocrEdtTemplateCreate(&placeTml, placeInitTask, 14, 3);
    for ( gg = 0; gg < places; ++gg ) {
        u64 ph;
        for ( ph = PHASE_NAMES; ph <= PHASE_TASKS; ++ph ) {
            u64 prm[14] = { gg, places, (u64)n_tiles_width, (u64)n_tiles_height,
                            (u64)tile_width, (u64)tile_height,
                            (u64)string1_len, (u64)string2_len,
                            (u64)check_score, (u64)evRange.guid, evStride, nranksNow,
                            ph, (u64)namesDone[gg].guid };
            ocrGuid_t e;
            ocrHint_t h = edtHintAt(placeRank(gg, places));
            ocrEdtCreate(&e, placeTml, EDT_PARAM_DEF, prm, EDT_PARAM_DEF, NULL,
                         EDT_PROP_NONE, &h, NULL);
            ocrAddDependence(dbParamsGuid, e, 0, DB_MODE_CONST);
            if ( ph == PHASE_NAMES ) {
                /* nothing to wait for: every band names its own tiles at once */
                ocrAddDependence(dbParamsGuid, e, 1, DB_MODE_CONST);
                ocrAddDependence(dbParamsGuid, e, 2, DB_MODE_CONST);
            } else {
                /* a task binds only within its own band or the one above it */
                ocrAddDependence(namesDone[gg], e, 1, DB_MODE_CONST);
                ocrAddDependence(namesDone[gg ? gg-1 : gg], e, 2, DB_MODE_CONST);
            }
        }
    }
    ocrEdtTemplateDestroy(placeTml);
    free(namesDone);

    return NULL_GUID;
}
