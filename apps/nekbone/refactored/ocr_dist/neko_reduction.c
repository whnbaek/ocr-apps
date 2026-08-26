#ifndef NEKBONE_REDUCTION_H
#include "neko_reduction.h"
#include "nekos_triplet.h"
#endif

#ifndef ENABLE_EXTENSION_LABELING
#define ENABLE_EXTENSION_LABELING  // For labeled GUIDs
#endif
#include "extensions/ocr-labeling.h"  // For labeled GUIDs

#include "reduction.h"

#define XMEMSET(SRC, CHARC, SZ) {unsigned int xmIT; for(xmIT=0; xmIT<SZ; ++xmIT) *((char*)SRC+xmIT)=CHARC;}
#define XMEMCPY(DEST, SRC, SZ) {unsigned int xmIT; for(xmIT=0; xmIT<SZ; ++xmIT) *((char*)DEST+xmIT)=*((char*)SRC+xmIT);}

Err_t copy_Reduct_private(reductionPrivate_t * in_from, reductionPrivate_t * o_target)
{
    if( !in_from || !o_target) return __LINE__;
    XMEMCPY(o_target, in_from, sizeof(reductionPrivate_t));
    return 0;
}


/* The restructured tier's numbering.  Kept in this source rather than in the
 * shared one: the two tiers are different programs from here down, and the
 * build picks whole files rather than switching inside them.
 *
 * nekbone_placeGrid is the placement layer's own box decomposition, reused so
 * that the numbering and the placement cannot disagree about which ranks
 * share a place.
 */
int nekbone_placeGrid(unsigned int in_places, Triplet in_lattice, Triplet * o_grid);

// The index a rank takes in the reduction tree.
//
// The tree is built over participant indices, so which indices a place holds
// decides which of its edges cross between places.  This numbering gives each
// place a consecutive run of indices, which is what lets a tree be built in
// two levels around a place -- see the reduction library's grouping.  On its
// own it is not a win: a flat tree over these indices still sends a
// participant's partial sum to an index near the root, i.e. to another place,
// at every level but the last.
//
// This is a permutation of the participants, so the all-reduce still runs over
// every rank; what changes is which partial sums are formed first.
unsigned long calcReductionIndex(unsigned int in_OCR_affinityCount, unsigned int in_rankID,
                                 Triplet in_lattice)
{
    Triplet grid = {0};
    if(!nekbone_placeGrid(in_OCR_affinityCount, in_lattice, &grid)){
        return in_rankID;   // no box: the tree has nothing to group by
    }
    {
        const Triplet at = index_to_coords((Idz)in_rankID, in_lattice);
        const unsigned long bx = (unsigned long)(in_lattice.a/grid.a);
        const unsigned long by = (unsigned long)(in_lattice.b/grid.b);
        const unsigned long bz = (unsigned long)(in_lattice.c/grid.c);
        const unsigned long px = (unsigned long)at.a / bx;
        const unsigned long py = (unsigned long)at.b / by;
        const unsigned long pz = (unsigned long)at.c / bz;
        const unsigned long place = px + (unsigned long)grid.a * (py + (unsigned long)grid.b * pz);
        const unsigned long lx = (unsigned long)at.a - px*bx;
        const unsigned long ly = (unsigned long)at.b - py*by;
        const unsigned long lz = (unsigned long)at.c - pz*bz;
        const unsigned long local = lx + bx*(ly + by*lz);
        return place * (bx*by*bz) + local;
    }
}

// How many consecutive indices one place holds under that numbering, or 0 when
// the ranks do not decompose into equal boxes -- in which case the numbering is
// the identity and there is no grouping for a two-level tree to exploit.
unsigned long calcReductionGroup(unsigned int in_OCR_affinityCount, Triplet in_lattice)
{
    Triplet grid = {0};
    if(!nekbone_placeGrid(in_OCR_affinityCount, in_lattice, &grid)) return 0;
    return (unsigned long)(in_lattice.a/grid.a)
         * (unsigned long)(in_lattice.b/grid.b)
         * (unsigned long)(in_lattice.c/grid.c);
}

Err_t init_Reduct_shared(unsigned long in_nrank,unsigned long in_ndata, Reduct_shared_t * io)
{
    XMEMSET(io,0,sizeof(Reduct_shared_t));
    io->nrank = in_nrank;
    io->ndata = in_ndata;
    io->reductionRangeGUID = NULL_GUID;
    return 0;
}
Err_t clear_Reduct_shared(Reduct_shared_t * io)
{
    XMEMSET(io,0,sizeof(Reduct_shared_t));
    return 0;
}
Err_t destroy_Reduct_shared(Reduct_shared_t * io)
{
    Err_t err=0;
    if( ! IS_GUID_NULL(io->reductionRangeGUID)){
        err = ocrGuidMapDestroy( io->reductionRangeGUID);
    }
    XMEMSET(io,0,sizeof(Reduct_shared_t));
    return err;
}
Err_t copy_Reduct_shared(Reduct_shared_t * in_from, Reduct_shared_t * o_target)
{
    if( !in_from || !o_target) return __LINE__;
    XMEMCPY(o_target, in_from, sizeof(Reduct_shared_t));
    return 0;
}
void  print_Reduct_shared(Reduct_shared_t * in)
{
    ocrPrintf("Reduct_shared>[myrank,nrank,ndata,reductRguid]= %lu %lu "GUIDF"\n",
           in->nrank, in->ndata, GUIDA(in->reductionRangeGUID)
           );
}

Err_t NEKO_mainEdt_reduction(unsigned long in_nrank,unsigned long in_ndata,
                           Reduct_shared_t * io_sharedRef, Reduct_shared_t * io_shared)
{
    Err_t err = 0;
    while(!err){
        err = init_Reduct_shared(in_nrank, in_ndata, io_sharedRef); IFEB;
        err = init_Reduct_shared(in_nrank, in_ndata, io_shared); IFEB;

        ocrGuid_t reductionRangeGUID = NULL_GUID;
        err = ocrGuidRangeCreate(&reductionRangeGUID, in_nrank, GUID_USER_EVENT_STICKY); IFEB;

        GUID_ASSIGN_VALUE(io_sharedRef->reductionRangeGUID, reductionRangeGUID);
        GUID_ASSIGN_VALUE(io_shared->reductionRangeGUID, reductionRangeGUID);
        break;
    }
    return err;
}

Err_t NEKO_finalEdt_reduction(Reduct_shared_t * io_sharedRef)
{
    Err_t err = 0;
    while(!err){
        err = destroy_Reduct_shared(io_sharedRef); IFEB;
        break;
    }
    return err;
}

Err_t NEKO_ForkTransit_reduction(unsigned int in_rankID, NEKOstatics_t * in_NEKOstatics,
                                 Reduct_shared_t * io_shared,
                                 reductionPrivate_t * io_reducPrivate)
{
    int err = 0;
    while(!err){
        io_reducPrivate->nrank  = io_shared->nrank;
        {   // Number the tree's participants place-major; see calcReductionIndex.
            Triplet lattice = { in_NEKOstatics->Rx, in_NEKOstatics->Ry, in_NEKOstatics->Rz };
            io_reducPrivate->myrank =
                (u64)calcReductionIndex(in_NEKOstatics->OCR_affinityCount, in_rankID, lattice);
            io_reducPrivate->nrankPerPlace =
                (u64)calcReductionGroup(in_NEKOstatics->OCR_affinityCount, lattice);
        }
        io_reducPrivate->ndata  = io_shared->ndata;
        io_reducPrivate->reductionOperator = REDUC_OPERATION_TYPE;
        io_reducPrivate->rangeGUID = io_shared->reductionRangeGUID;
        io_reducPrivate->new = 1;
        io_reducPrivate->type = ALLREDUCE;

        ocrEventParams_t params;
        params.EVENT_CHANNEL.maxGen = 2; //2 for channel exchange
        params.EVENT_CHANNEL.nbSat = 1;
        params.EVENT_CHANNEL.nbDeps = 1;
        err = ocrEventCreateParams(&(io_reducPrivate->returnEVT), OCR_EVENT_CHANNEL_T, false, &params); IFEB;

        err = clear_Reduct_shared(io_shared); IFEB;

        if(0 == in_rankID){
#           ifdef REDUCTION_CGSTEP0
                ocrPrintf("INFO> Reduction in CGstep0_start       is active. Slot count used = %d.\n", (int)REDUC_SLOT_4CGstep0);
#           else
                ocrPrintf("INFO> Reduction in CGstep0_start       is    off. Slot count used = %d.\n", (int)REDUC_SLOT_4CGstep0);
#           endif
#           ifdef REDUCTION_BETA
                ocrPrintf("INFO> Reduction in nekbone_beta_start  is active. Slot count used = %d.\n", (int)REDUC_SLOT_4Beta);
#           else
                ocrPrintf("INFO> Reduction in nekbone_beta_start  is    off. Slot count used = %d.\n", (int)REDUC_SLOT_4Beta);
#           endif
#           ifdef REDUCTION_ALPHA
                ocrPrintf("INFO> Reduction in nekbone_alpha_start is active. Slot count used = %d.\n", (int)REDUC_SLOT_4Alpha);
#           else
                ocrPrintf("INFO> Reduction in nekbone_alpha_start is    off. Slot count used = %d.\n", (int)REDUC_SLOT_4Alpha);
#           endif
#           ifdef REDUCTION_RTR
                ocrPrintf("INFO> Reduction in nekbone_rtr_start   is active. Slot count used = %d.\n", (int)REDUC_SLOT_4Rtr);
#           else
                ocrPrintf("INFO> Reduction in nekbone_rtr_start   is    off. Slot count used = %d.\n", (int)REDUC_SLOT_4Rtr);
#           endif
        }
        break;
    }
    return err;
}




















