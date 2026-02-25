#include <ocr.h>
#include "app_ocr_util.h"
#include "treeForkJoin.h" 
#include "spmd_global_data.h" 
#include "tailrecursion.h" 
#include "neko_globals.h" 
#include "nekos_tools.h" 
#include "nekbone_setup.h" 
#include "nekbone_cg.h" 
#include "reduction.h" 
#include "neko_reduction.h" 
#include "neko_halo.h" 
 
#define Nfoliation 2

ocrGuid_t mainEdt(EDT_ARGS);
ocrGuid_t finalEDT(EDT_ARGS);
ocrGuid_t SetupBtForkJoin(EDT_ARGS);
ocrGuid_t ConcludeBtForkJoin(EDT_ARGS);
ocrGuid_t BtForkIF(EDT_ARGS);
ocrGuid_t BtForkFOR(EDT_ARGS);
ocrGuid_t BtForkTransition_Start(EDT_ARGS);
ocrGuid_t channelExchange_start(EDT_ARGS);
ocrGuid_t channelExchange_stop(EDT_ARGS);
ocrGuid_t nekMultiplicity_start(EDT_ARGS);
ocrGuid_t nekMultiplicity_stop(EDT_ARGS);
ocrGuid_t nekSetF_start(EDT_ARGS);
ocrGuid_t nekSetF_stop(EDT_ARGS);
ocrGuid_t nekCGstep0_start(EDT_ARGS);
ocrGuid_t nekCGstep0_stop(EDT_ARGS);
ocrGuid_t BtForkTransition_Stop(EDT_ARGS);
ocrGuid_t BtJoinIFTHEN(EDT_ARGS);
ocrGuid_t setupTailRecursion(EDT_ARGS);
ocrGuid_t tailRecursionIFThen(EDT_ARGS);
ocrGuid_t tailRecurTransitBEGIN(EDT_ARGS);
ocrGuid_t nekCG_solveMi(EDT_ARGS);
ocrGuid_t nekCG_beta_start(EDT_ARGS);
ocrGuid_t nekCG_beta_stop(EDT_ARGS);
ocrGuid_t nekCG_axi_start(EDT_ARGS);
ocrGuid_t nekCG_axi_stop(EDT_ARGS);
ocrGuid_t nekCG_alpha_start(EDT_ARGS);
ocrGuid_t nekCG_alpha_stop(EDT_ARGS);
ocrGuid_t nekCG_rtr_start(EDT_ARGS);
ocrGuid_t nekCG_rtr_stop(EDT_ARGS);
ocrGuid_t tailRecurTransitEND(EDT_ARGS);
ocrGuid_t concludeTailRecursion(EDT_ARGS);

ocrGuid_t mainEdt(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 0
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs

        //----- Create_dataBlocks
        const u64 OA_Count_reducSharedRef = 1;
        const u64 OA_Count_reducShare = 1;
        const u64 OA_Count_gDone = 1;
        const u64 OA_Count_SPMDGlobals = 1;
        const u64 OA_Count_NEKOstatics = 1;

        ocrGuid_t gd_reducSharedRef= NULL_GUID;
        Reduct_shared_t * o_reductSharedRef=NULL;
        err = ocrDbCreate( &gd_reducSharedRef, (void**)&o_reductSharedRef, 1*sizeof(Reduct_shared_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_reducShare= NULL_GUID;
        Reduct_shared_t * o_reductShare=NULL;
        err = ocrDbCreate( &gd_reducShare, (void**)&o_reductShare, 1*sizeof(Reduct_shared_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_gDone= NULL_GUID;
        ocrGuid_t * o_gDone=NULL;
        err = ocrDbCreate( &gd_gDone, (void**)&o_gDone, 1*sizeof(ocrGuid_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_SPMDGlobals= NULL_GUID;
        SPMD_GlobalData_t * o_SPMDglobals=NULL;
        err = ocrDbCreate( &gd_SPMDGlobals, (void**)&o_SPMDglobals, 1*sizeof(SPMD_GlobalData_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_NEKOstatics= NULL_GUID;
        NEKOstatics_t * o_NEKOstatics=NULL;
        err = ocrDbCreate( &gd_NEKOstatics, (void**)&o_NEKOstatics, 1*sizeof(NEKOstatics_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs
        ocrGuid_t ga_finalEDT = NULL_GUID;
        err = ocrEdtXCreate(finalEDT, 0, NULL, 2, NULL, EDT_PROP_NONE, NULL_HINT, &ga_finalEDT, NULL); IFEB;
        ocrGuid_t ga_SetupBtForkJoin = NULL_GUID;
        err = ocrEdtXCreate(SetupBtForkJoin, 0, NULL, 4, NULL, EDT_PROP_NONE, NULL_HINT, &ga_SetupBtForkJoin, NULL); IFEB;

        //----- User section
        GUID_ASSIGN_VALUE(*o_gDone, ga_finalEDT);
        init_SPMDglobals(o_SPMDglobals);
        err = init_NEKOstatics(o_NEKOstatics, depv[0].ptr); IFEB;
        err = setup_SPMD_using_NEKOstatics(o_NEKOstatics, o_SPMDglobals); IFEB;
        err = NEKO_mainEdt_reduction(o_NEKOstatics->Rtotal, 1, o_reductSharedRef, o_reductShare); IFEB;

        //----- Release or destroy data blocks
        err = ocrDbRelease(gd_reducSharedRef); IFEB;
        err = ocrDbRelease(gd_reducShare); IFEB;
        err = ocrDbRelease(gd_gDone); IFEB;
        err = ocrDbRelease(gd_SPMDGlobals); IFEB;
        err = ocrDbRelease(gd_NEKOstatics); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_finalEDT, 0, DB_MODE_RW, gd_reducSharedRef); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_SetupBtForkJoin, 0, DB_MODE_RO, gd_reducShare); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_SetupBtForkJoin, 1, DB_MODE_RO, gd_SPMDGlobals); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_SetupBtForkJoin, 2, DB_MODE_RO, gd_gDone); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_SetupBtForkJoin, 3, DB_MODE_RO, gd_NEKOstatics); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t finalEDT(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 1
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E0_1_dep_reducSharedRef = depv[0];
        Reduct_shared_t * in_reducShare = IN_derefs_E0_1_dep_reducSharedRef.ptr;

        //----- Create_dataBlocks


        //----- Create children EDTs

        //----- User section
        err = NEKO_finalEdt_reduction(in_reducShare); IFEB;
        err = nekbone_finalEDTt(); IFEB;

        //----- Release or destroy data blocks
        //Skipping destruction of a NULL_GUID data block.
        err = ocrDbDestroy( IN_derefs_E0_1_dep_reducSharedRef.guid ); IFEB;

        //----- Link to other EDTs using Events
        ocrShutdown();
        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t SetupBtForkJoin(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 2
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E0_2_dep_gDone = depv[2];
        ocrEdtDep_t IN_derefs_E0_2_dep_SPMDGlobals = depv[1];
        SPMD_GlobalData_t * io_SPMDglobals = IN_derefs_E0_2_dep_SPMDGlobals.ptr;
        ocrEdtDep_t IN_derefs_E0_2_dep_NEKOstatics = depv[3];
        ocrEdtDep_t IN_derefs_E0_2_dep_reducShare = depv[0];

        //----- Create_dataBlocks
        const u64 OA_Count_RefTCsum = 1;
        const u64 OA_Count_TFJiterate = 1;

        ocrGuid_t gd_RefTCsum= NULL_GUID;
        TChecksum_work_t * o_refTCsum=NULL;
        err = ocrDbCreate( &gd_RefTCsum, (void**)&o_refTCsum, 1*sizeof(TChecksum_work_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_TFJiterate= NULL_GUID;
        TFJiterate_t * o_TFJiterate=NULL;
        err = ocrDbCreate( &gd_TFJiterate, (void**)&o_TFJiterate, 1*sizeof(TFJiterate_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs
        ocrGuid_t ga_ConcludeBtForkJoin = NULL_GUID;
        err = ocrEdtXCreate(ConcludeBtForkJoin, 0, NULL, 3, NULL, EDT_PROP_NONE, NULL_HINT, &ga_ConcludeBtForkJoin, NULL); IFEB;
        ocrGuid_t ga_BtForkIF = NULL_GUID;
        err = ocrEdtXCreate(BtForkIF, 0, NULL, 4, NULL, EDT_PROP_NONE, NULL_HINT, &ga_BtForkIF, NULL); IFEB;

        //----- User section
        err = setupBtForkJoin(OA_edtTypeNb, OA_DBG_thisEDT, o_TFJiterate, ga_ConcludeBtForkJoin, io_SPMDglobals, &o_refTCsum->result);

        //----- Release or destroy data blocks
        err = ocrDbRelease(gd_RefTCsum); IFEB;
        err = ocrDbRelease(gd_TFJiterate); IFEB;
        err = ocrDbRelease(IN_derefs_E0_2_dep_reducShare.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E0_2_dep_SPMDGlobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E0_2_dep_gDone.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E0_2_dep_NEKOstatics.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_ConcludeBtForkJoin, 0, DB_MODE_RO, gd_RefTCsum); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_ConcludeBtForkJoin, 1, DB_MODE_RO, IN_derefs_E0_2_dep_gDone.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtForkIF, 0, DB_MODE_RO, gd_TFJiterate); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtForkIF, 1, DB_MODE_RO, IN_derefs_E0_2_dep_reducShare.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtForkIF, 2, DB_MODE_RO, IN_derefs_E0_2_dep_SPMDGlobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtForkIF, 3, DB_MODE_RO, IN_derefs_E0_2_dep_NEKOstatics.guid); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t ConcludeBtForkJoin(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 3
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E2_3_dep_gDone = depv[1];
        ocrGuid_t * in_gDone = IN_derefs_E2_3_dep_gDone.ptr;
        ocrEdtDep_t IN_derefs_E2_3_dep_RefTCsum = depv[0];
        TChecksum_work_t * in_refwork = IN_derefs_E2_3_dep_RefTCsum.ptr;
        ocrEdtDep_t IN_derefs_E18_3_dep_TCsum = depv[2];
        TChecksum_work_t * in_calculated = IN_derefs_E18_3_dep_TCsum.ptr;

        //----- Create_dataBlocks


        //----- Create children EDTs

        //----- User section
        ocrGuid_t ga_finalEDT; GUID_ASSIGN_VALUE(ga_finalEDT, *in_gDone);
        err = concludeBtForkJoin(OA_edtTypeNb, OA_DBG_thisEDT, in_refwork->result, in_calculated->result); IFEB;

        //----- Release or destroy data blocks
        //Skipping release of a NULL_GUID data block.
        err = ocrDbDestroy( IN_derefs_E2_3_dep_RefTCsum.guid ); IFEB;
        err = ocrDbDestroy( IN_derefs_E2_3_dep_gDone.guid ); IFEB;
        err = ocrDbDestroy( IN_derefs_E18_3_dep_TCsum.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_NONE, ga_finalEDT, 1, DB_MODE_RW, NULL_GUID); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t BtForkIF(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 4
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E2_4_dep_TFJiterate = depv[0];
        TFJiterate_t * in_TFJiterate = IN_derefs_E2_4_dep_TFJiterate.ptr;
        unsigned int rankID = in_TFJiterate->low - 1;
        ocrEdtDep_t IN_derefs_E2_4_dep_SPMDGlobals = depv[2];
        SPMD_GlobalData_t * in_SPMDglobals = IN_derefs_E2_4_dep_SPMDGlobals.ptr;
        ocrEdtDep_t IN_derefs_E2_4_dep_NEKOstatics = depv[3];
        NEKOstatics_t * in_NEKOstatics = IN_derefs_E2_4_dep_NEKOstatics.ptr;
        unsigned long pdID = calcPDid_S(in_NEKOstatics->OCR_affinityCount, rankID);
        ocrHint_t hintEDT, *pHintEDT=0, hintDBK, *pHintDBK=0;
        err = ocrXgetEdtHint(pdID, &hintEDT, &pHintEDT); IFEB;
        err = ocrXgetDbkHint(pdID, &hintDBK, &pHintDBK); IFEB;
        ocrEdtDep_t IN_derefs_E2_4_dep_reducShare = depv[1];
        Reduct_shared_t * in_reducShare = IN_derefs_E2_4_dep_reducShare.ptr;

        if( conditionBtFork(OA_edtTypeNb, OA_DBG_thisEDT, in_TFJiterate) ){
            //----- Create_dataBlocks
            const u64 OA_Count_TFJiterate = 1;
            const u64 OA_Count_SPMDGlobals = 1;
            const u64 OA_Count_gDone = 1;
            const u64 OA_Count_NEKOstatics = 1;
            const u64 OA_Count_reducShare = 1;

            ocrGuid_t gd_TFJiterate= NULL_GUID;
            TFJiterate_t * o_TFJiterate=NULL;
            err = ocrDbCreate( &gd_TFJiterate, (void**)&o_TFJiterate, 1*sizeof(TFJiterate_t), DB_PROP_NONE, pHintDBK, NO_ALLOC); IFEB;
            ocrGuid_t gd_SPMDGlobals= NULL_GUID;
            SPMD_GlobalData_t * o_SPMDglobals=NULL;
            err = ocrDbCreate( &gd_SPMDGlobals, (void**)&o_SPMDglobals, 1*sizeof(SPMD_GlobalData_t), DB_PROP_NONE, pHintDBK, NO_ALLOC); IFEB;
            ocrGuid_t gd_gDone= NULL_GUID;
            ocrGuid_t * o_gDone=NULL;
            err = ocrDbCreate( &gd_gDone, (void**)&o_gDone, 1*sizeof(ocrGuid_t), DB_PROP_NONE, pHintDBK, NO_ALLOC); IFEB;
            ocrGuid_t gd_NEKOstatics= NULL_GUID;
            NEKOstatics_t * o_NEKOstatics=NULL;
            err = ocrDbCreate( &gd_NEKOstatics, (void**)&o_NEKOstatics, 1*sizeof(NEKOstatics_t), DB_PROP_NONE, pHintDBK, NO_ALLOC); IFEB;
            ocrGuid_t gd_reducShare= NULL_GUID;
            Reduct_shared_t * o_reducShare=NULL;
            err = ocrDbCreate( &gd_reducShare, (void**)&o_reducShare, 1*sizeof(Reduct_shared_t), DB_PROP_NONE, pHintDBK, NO_ALLOC); IFEB;

            //----- Create children EDTs
            ocrGuid_t ga_BtJoinIFTHEN = NULL_GUID;
            err = ocrEdtXCreate(BtJoinIFTHEN, 0, NULL, 3, NULL, EDT_PROP_NONE, NULL_HINT, &ga_BtJoinIFTHEN, NULL); IFEB;
            ocrGuid_t ga_BtForkFOR = NULL_GUID;
            err = ocrEdtXCreate(BtForkFOR, 0, NULL, 4, NULL, EDT_PROP_NONE, NULL_HINT, &ga_BtForkFOR, NULL); IFEB;

            //----- User section
            err = btForkThen(OA_edtTypeNb, OA_DBG_thisEDT, in_TFJiterate, ga_BtJoinIFTHEN, o_TFJiterate, o_gDone);
            copy_SPMDglobals(in_SPMDglobals, o_SPMDglobals);
            copy_NEKOstatics(in_NEKOstatics, o_NEKOstatics);
            copy_Reduct_shared(in_reducShare, o_reducShare);

            //----- Release or destroy data blocks
            err = ocrDbRelease(gd_TFJiterate); IFEB;
            err = ocrDbRelease(gd_SPMDGlobals); IFEB;
            err = ocrDbRelease(gd_gDone); IFEB;
            err = ocrDbRelease(gd_NEKOstatics); IFEB;
            err = ocrDbRelease(gd_reducShare); IFEB;
            err = ocrDbDestroy( IN_derefs_E2_4_dep_TFJiterate.guid ); IFEB;
            err = ocrDbDestroy( IN_derefs_E2_4_dep_reducShare.guid ); IFEB;
            err = ocrDbDestroy( IN_derefs_E2_4_dep_SPMDGlobals.guid ); IFEB;
            err = ocrDbDestroy( IN_derefs_E2_4_dep_NEKOstatics.guid ); IFEB;

            //----- Link to other EDTs using Events
            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtJoinIFTHEN, 0, DB_MODE_RO, gd_gDone); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtForkFOR, 0, DB_MODE_RO, gd_TFJiterate); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtForkFOR, 1, DB_MODE_RO, gd_reducShare); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtForkFOR, 2, DB_MODE_RO, gd_SPMDGlobals); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtForkFOR, 3, DB_MODE_RO, gd_NEKOstatics); IFEB;

        }else{
            //----- Create_dataBlocks
            const u64 OA_Count_TFJiterate = 1;
            const u64 OA_Count_SPMDGlobals = 1;
            const u64 OA_Count_gDone = 1;
            const u64 OA_Count_NEKOstatics = 1;
            const u64 OA_Count_reducShare = 1;

            ocrGuid_t gd_TFJiterate= NULL_GUID;
            TFJiterate_t * o_TFJiterate=NULL;
            err = ocrDbCreate( &gd_TFJiterate, (void**)&o_TFJiterate, 1*sizeof(TFJiterate_t), DB_PROP_NONE, pHintDBK, NO_ALLOC); IFEB;
            ocrGuid_t gd_SPMDGlobals= NULL_GUID;
            SPMD_GlobalData_t * o_SPMDglobals=NULL;
            err = ocrDbCreate( &gd_SPMDGlobals, (void**)&o_SPMDglobals, 1*sizeof(SPMD_GlobalData_t), DB_PROP_NONE, pHintDBK, NO_ALLOC); IFEB;
            ocrGuid_t gd_gDone= NULL_GUID;
            ocrGuid_t * o_gDone=NULL;
            err = ocrDbCreate( &gd_gDone, (void**)&o_gDone, 1*sizeof(ocrGuid_t), DB_PROP_NONE, pHintDBK, NO_ALLOC); IFEB;
            ocrGuid_t gd_NEKOstatics= NULL_GUID;
            NEKOstatics_t * o_NEKOstatics=NULL;
            err = ocrDbCreate( &gd_NEKOstatics, (void**)&o_NEKOstatics, 1*sizeof(NEKOstatics_t), DB_PROP_NONE, pHintDBK, NO_ALLOC); IFEB;
            ocrGuid_t gd_reducShare= NULL_GUID;
            Reduct_shared_t * o_reducShare=NULL;
            err = ocrDbCreate( &gd_reducShare, (void**)&o_reducShare, 1*sizeof(Reduct_shared_t), DB_PROP_NONE, pHintDBK, NO_ALLOC); IFEB;

            //----- Create children EDTs
            ocrGuid_t ga_BtJoinIFTHEN = NULL_GUID;
            err = ocrEdtXCreate(BtJoinIFTHEN, 0, NULL, 3, NULL, EDT_PROP_NONE, NULL_HINT, &ga_BtJoinIFTHEN, NULL); IFEB;
            ocrGuid_t ga_BtForkTransition_Start = NULL_GUID;
            err = ocrEdtXCreate(BtForkTransition_Start, 0, NULL, 4, NULL, EDT_PROP_NONE, pHintEDT, &ga_BtForkTransition_Start, NULL); IFEB;

            //----- User section
            err = btForkElse(OA_edtTypeNb, OA_DBG_thisEDT, in_TFJiterate, ga_BtJoinIFTHEN, o_TFJiterate, o_gDone);
            copy_SPMDglobals(in_SPMDglobals, o_SPMDglobals);
            copy_NEKOstatics(in_NEKOstatics, o_NEKOstatics);
            copy_Reduct_shared(in_reducShare, o_reducShare);

            //----- Release or destroy data blocks
            err = ocrDbRelease(gd_TFJiterate); IFEB;
            err = ocrDbRelease(gd_SPMDGlobals); IFEB;
            err = ocrDbRelease(gd_gDone); IFEB;
            err = ocrDbRelease(gd_NEKOstatics); IFEB;
            err = ocrDbRelease(gd_reducShare); IFEB;
            err = ocrDbDestroy( IN_derefs_E2_4_dep_TFJiterate.guid ); IFEB;
            err = ocrDbDestroy( IN_derefs_E2_4_dep_SPMDGlobals.guid ); IFEB;

            //----- Link to other EDTs using Events
            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtJoinIFTHEN, 0, DB_MODE_RO, gd_gDone); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtForkTransition_Start, 0, DB_MODE_RO, gd_TFJiterate); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtForkTransition_Start, 1, DB_MODE_RW, gd_reducShare); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtForkTransition_Start, 2, DB_MODE_RO, gd_SPMDGlobals); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtForkTransition_Start, 3, DB_MODE_RO, gd_NEKOstatics); IFEB;

        }
        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t BtForkFOR(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 5
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E4_5_dep_TFJiterate = depv[0];
        TFJiterate_t * in_TFJiterate = IN_derefs_E4_5_dep_TFJiterate.ptr;
        ocrEdtDep_t IN_derefs_E4_5_dep_SPMDGlobals = depv[2];
        SPMD_GlobalData_t * in_SPMDglobals = IN_derefs_E4_5_dep_SPMDGlobals.ptr;
        ocrEdtDep_t IN_derefs_E4_5_dep_NEKOstatics = depv[3];
        NEKOstatics_t * in_NEKOstatics = IN_derefs_E4_5_dep_NEKOstatics.ptr;
        ocrEdtDep_t IN_derefs_E4_5_dep_reducShare = depv[1];
        Reduct_shared_t * in_reducShare = IN_derefs_E4_5_dep_reducShare.ptr;

        int btFoliationIndex; for(btFoliationIndex=0; btFoliationIndex < Nfoliation; ++btFoliationIndex){
            //----- Create_dataBlocks
            const u64 OA_Count_TFJiterate = 1;
            const u64 OA_Count_SPMDGlobals = 1;
            const u64 OA_Count_NEKOstatics = 1;
            const u64 OA_Count_reducShare = 1;

            ocrGuid_t gd_TFJiterate= NULL_GUID;
            TFJiterate_t * o_TFJiterate=NULL;
            err = ocrDbCreate( &gd_TFJiterate, (void**)&o_TFJiterate, 1*sizeof(TFJiterate_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
            ocrGuid_t gd_SPMDGlobals= NULL_GUID;
            SPMD_GlobalData_t * o_SPMDglobals=NULL;
            err = ocrDbCreate( &gd_SPMDGlobals, (void**)&o_SPMDglobals, 1*sizeof(SPMD_GlobalData_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
            ocrGuid_t gd_NEKOstatics= NULL_GUID;
            NEKOstatics_t * o_NEKOstatics=NULL;
            err = ocrDbCreate( &gd_NEKOstatics, (void**)&o_NEKOstatics, 1*sizeof(NEKOstatics_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
            ocrGuid_t gd_reducShare= NULL_GUID;
            Reduct_shared_t * o_reducShare=NULL;
            err = ocrDbCreate( &gd_reducShare, (void**)&o_reducShare, 1*sizeof(Reduct_shared_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

            //----- Create children EDTs
            ocrGuid_t ga_BtForkIF = NULL_GUID;
            err = ocrEdtXCreate(BtForkIF, 0, NULL, 4, NULL, EDT_PROP_NONE, NULL_HINT, &ga_BtForkIF, NULL); IFEB;

            //----- User section
            err = btForkFOR(OA_edtTypeNb, OA_DBG_thisEDT, btFoliationIndex, in_TFJiterate, o_TFJiterate);
            copy_SPMDglobals(in_SPMDglobals, o_SPMDglobals);
            copy_NEKOstatics(in_NEKOstatics, o_NEKOstatics);
            copy_Reduct_shared(in_reducShare, o_reducShare);

            //----- Release or destroy data blocks
            err = ocrDbRelease(gd_TFJiterate); IFEB;
            err = ocrDbRelease(gd_SPMDGlobals); IFEB;
            err = ocrDbRelease(gd_NEKOstatics); IFEB;
            err = ocrDbRelease(gd_reducShare); IFEB;

            //----- Link to other EDTs using Events
            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtForkIF, 0, DB_MODE_RO, gd_TFJiterate); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtForkIF, 1, DB_MODE_RO, gd_reducShare); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtForkIF, 2, DB_MODE_RO, gd_SPMDGlobals); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtForkIF, 3, DB_MODE_RO, gd_NEKOstatics); IFEB;

        }; IFEB;
        //----- Release or destroy data blocks
        err = ocrDbDestroy( IN_derefs_E4_5_dep_TFJiterate.guid ); IFEB;
        err = ocrDbDestroy( IN_derefs_E4_5_dep_reducShare.guid ); IFEB;
        err = ocrDbDestroy( IN_derefs_E4_5_dep_SPMDGlobals.guid ); IFEB;
        err = ocrDbDestroy( IN_derefs_E4_5_dep_NEKOstatics.guid ); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t BtForkTransition_Start(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 7
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E6_7_dep_TFJiterate = depv[0];
        TFJiterate_t * io_TFJiterate = IN_derefs_E6_7_dep_TFJiterate.ptr;
            unsigned int rankID = io_TFJiterate->low - 1;
        ocrEdtDep_t IN_derefs_E6_7_dep_SPMDGlobals = depv[2];
        SPMD_GlobalData_t * io_SPMDglobals = IN_derefs_E6_7_dep_SPMDGlobals.ptr;
        ocrEdtDep_t IN_derefs_E6_7_dep_NEKOstatics = depv[3];
        NEKOstatics_t * io_NEKOstatics = IN_derefs_E6_7_dep_NEKOstatics.ptr;
            unsigned int sz_nekLGLEL = io_NEKOstatics->Etotal;
            unsigned int sz_nekGLO_NUM = io_NEKOstatics->pDOF3DperRmax;
            ocrHint_t hintEDT, *pHintEDT=0;
            err = ocrXgetEdtHint(NEK_OCR_USE_CURRENT_PD, &hintEDT, &pHintEDT); IFEB;
        ocrEdtDep_t IN_derefs_E6_7_dep_reducShare = depv[1];
        Reduct_shared_t * in_reducShare = IN_derefs_E6_7_dep_reducShare.ptr;

        //----- Create_dataBlocks
        const u64 OA_Count_gDone = 1;
        const u64 OA_Count_NEKOglobals = 1;
        const u64 OA_Count_nekLGLEL = (sz_nekLGLEL+1)*sizeof(unsigned int);
        const u64 OA_Count_nekGLO_NUM = (sz_nekGLO_NUM+1)*sizeof(unsigned long);
        const u64 OA_Count_reducPrivate = 1;
        const u64 OA_Count_gDone2 = 1;

        ocrGuid_t gd_gDone= NULL_GUID;
        ocrGuid_t * o_gDone=NULL;
        err = ocrDbCreate( &gd_gDone, (void**)&o_gDone, 1*sizeof(ocrGuid_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_NEKOglobals= NULL_GUID;
        NEKOglobals_t * o_NEKOglobals=NULL;
        err = ocrDbCreate( &gd_NEKOglobals, (void**)&o_NEKOglobals, 1*sizeof(NEKOglobals_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nekLGLEL= NULL_GUID;
        unsigned int * o_lglel=NULL;
        err = ocrDbCreate( &gd_nekLGLEL, (void**)&o_lglel, (sz_nekLGLEL+1)*sizeof(unsigned int), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nekGLO_NUM= NULL_GUID;
        unsigned long * o_glo_num=NULL;
        err = ocrDbCreate( &gd_nekGLO_NUM, (void**)&o_glo_num, (sz_nekGLO_NUM+1)*sizeof(unsigned long), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_reducPrivate= NULL_GUID;
        reductionPrivate_t * o_reducPrivate=NULL;
        err = ocrDbCreate( &gd_reducPrivate, (void**)&o_reducPrivate, 1*sizeof(reductionPrivate_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_gDone2= NULL_GUID;
        ocrGuid_t * o_gDone2=NULL;
        err = ocrDbCreate( &gd_gDone2, (void**)&o_gDone2, 1*sizeof(ocrGuid_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs
        ocrGuid_t ga_BtForkTransition_Stop = NULL_GUID;
        err = ocrEdtXCreate(BtForkTransition_Stop, 0, NULL, 6, NULL, EDT_PROP_NONE, pHintEDT, &ga_BtForkTransition_Stop, NULL); IFEB;
        ocrGuid_t ga_channelExchange_start = NULL_GUID;
        err = ocrEdtXCreate(channelExchange_start, 0, NULL, 4, NULL, EDT_PROP_NONE, pHintEDT, &ga_channelExchange_start, NULL); IFEB;
        ocrGuid_t ga_setupTailRecursion = NULL_GUID;
        err = ocrEdtXCreate(setupTailRecursion, 0, NULL, 19, NULL, EDT_PROP_NONE, pHintEDT, &ga_setupTailRecursion, NULL); IFEB;

        //----- User section
        *o_gDone = ga_BtForkTransition_Stop;
        err = init_NEKOglobals(io_NEKOstatics, rankID, o_NEKOglobals); IFEB;
        err = nekbone_setup(io_NEKOstatics, o_NEKOglobals, o_lglel, o_glo_num); IFEB;
        *o_gDone2 = ga_setupTailRecursion;
        err = NEKO_ForkTransit_reduction(rankID, in_reducShare, o_reducPrivate); IFEB;

        //----- Release or destroy data blocks
        err = ocrDbRelease(gd_gDone); IFEB;
        err = ocrDbRelease(gd_NEKOglobals); IFEB;
        err = ocrDbDestroy(gd_nekLGLEL); IFEB;
        err = ocrDbDestroy(gd_nekGLO_NUM); IFEB;
        err = ocrDbRelease(gd_reducPrivate); IFEB;
        err = ocrDbRelease(gd_gDone2); IFEB;
        err = ocrDbDestroy( IN_derefs_E6_7_dep_reducShare.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E6_7_dep_TFJiterate.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E6_7_dep_SPMDGlobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E6_7_dep_NEKOstatics.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtForkTransition_Stop, 0, DB_MODE_RO, IN_derefs_E6_7_dep_TFJiterate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_channelExchange_start, 0, DB_MODE_RW, gd_reducPrivate); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_channelExchange_start, 1, DB_MODE_RO, gd_NEKOglobals); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_channelExchange_start, 2, DB_MODE_RO, gd_gDone2); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_channelExchange_start, 3, DB_MODE_RO, IN_derefs_E6_7_dep_NEKOstatics.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_setupTailRecursion, 0, DB_MODE_RO, IN_derefs_E6_7_dep_SPMDGlobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_setupTailRecursion, 1, DB_MODE_RO, gd_gDone); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t channelExchange_start(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 8
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E7_8_dep_NEKOstatics = depv[3];
        NEKOstatics_t * io_NEKOstatics = IN_derefs_E7_8_dep_NEKOstatics.ptr;
        ocrEdtDep_t IN_derefs_E7_8_dep_NEKOglobals = depv[1];
        NEKOglobals_t * in_NEKOglobals = IN_derefs_E7_8_dep_NEKOglobals.ptr;
            ocrHint_t hintEDT, *pHintEDT=0;
            err = ocrXgetEdtHint(NEK_OCR_USE_CURRENT_PD, &hintEDT, &pHintEDT); IFEB;
        ocrEdtDep_t IN_derefs_E7_8_dep_gDone2 = depv[2];
        ocrEdtDep_t IN_derefs_E7_8_dep_reducPrivate = depv[0];

        //----- Create_dataBlocks
        const u64 OA_Count_NEKOglobals = 1;
        const u64 OA_Count_NEKOtools = 1;

        ocrGuid_t gd_NEKOglobals= NULL_GUID;
        NEKOglobals_t * o_NEKOglobals=NULL;
        err = ocrDbCreate( &gd_NEKOglobals, (void**)&o_NEKOglobals, 1*sizeof(NEKOglobals_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_NEKOtools= NULL_GUID;
        NEKOtools_t * o_NEKOtools=NULL;
        err = ocrDbCreate( &gd_NEKOtools, (void**)&o_NEKOtools, 1*sizeof(NEKOtools_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs
        ocrGuid_t ga_channelExchange_stop = NULL_GUID;
        err = ocrEdtXCreate(channelExchange_stop, 0, NULL, SLOTCNT_total_channelExchange, NULL, EDT_PROP_NONE, pHintEDT, &ga_channelExchange_stop, NULL); IFEB;

        //----- User section
        err = init_NEKOtools(o_NEKOtools, *io_NEKOstatics, in_NEKOglobals->rankID, in_NEKOglobals->pDOF); IFEB;
        err = start_channelExchange(OA_DEBUG_OUTVARS, o_NEKOtools, &io_NEKOstatics->haloLabeledGuids[0], &ga_channelExchange_stop, in_NEKOglobals, o_NEKOglobals); IFEB;

        //----- Release or destroy data blocks
        err = ocrDbRelease(gd_NEKOglobals); IFEB;
        err = ocrDbRelease(gd_NEKOtools); IFEB;
        err = ocrDbDestroy( IN_derefs_E7_8_dep_NEKOglobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E7_8_dep_reducPrivate.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E7_8_dep_gDone2.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E7_8_dep_NEKOstatics.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_channelExchange_stop, 0, DB_MODE_RW, IN_derefs_E7_8_dep_reducPrivate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_channelExchange_stop, 1, DB_MODE_RO, gd_NEKOtools); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_channelExchange_stop, 2, DB_MODE_RO, gd_NEKOglobals); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_channelExchange_stop, 3, DB_MODE_RO, IN_derefs_E7_8_dep_gDone2.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_channelExchange_stop, 4, DB_MODE_RO, IN_derefs_E7_8_dep_NEKOstatics.guid); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t channelExchange_stop(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 9
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E8_9_dep_NEKOstatics = depv[4];
        NEKOstatics_t * io_NEKOstatics = IN_derefs_E8_9_dep_NEKOstatics.ptr;
        ocrEdtDep_t IN_derefs_E8_9_dep_NEKOglobals = depv[2];
        NEKOglobals_t * in_NEKOglobals = IN_derefs_E8_9_dep_NEKOglobals.ptr;
            ocrHint_t hintEDT, *pHintEDT=0;
            err = ocrXgetEdtHint(NEK_OCR_USE_CURRENT_PD, &hintEDT, &pHintEDT); IFEB;
        ocrEdtDep_t IN_derefs_E8_9_dep_gDone2 = depv[3];
        ocrEdtDep_t IN_derefs_E8_9_dep_reducPrivate = depv[0];
        ocrEdtDep_t IN_derefs_E8_9_dep_NEKOtools = depv[1];
        NEKOtools_t * io_NEKOtools = IN_derefs_E8_9_dep_NEKOtools.ptr;

        //----- Create_dataBlocks
        const u64 OA_Count_NEKOglobals = 1;

        ocrGuid_t gd_NEKOglobals= NULL_GUID;
        NEKOglobals_t * o_NEKOglobals=NULL;
        err = ocrDbCreate( &gd_NEKOglobals, (void**)&o_NEKOglobals, 1*sizeof(NEKOglobals_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs
        ocrGuid_t ga_nekMultiplicity_start = NULL_GUID;
        err = ocrEdtXCreate(nekMultiplicity_start, 0, NULL, 5, NULL, EDT_PROP_NONE, pHintEDT, &ga_nekMultiplicity_start, NULL); IFEB;

        //----- User section
        err = stop_channelExchange(OA_DEBUG_OUTVARS, io_NEKOtools, depv, in_NEKOglobals, o_NEKOglobals); IFEB;

        //----- Release or destroy data blocks
        err = ocrDbRelease(gd_NEKOglobals); IFEB;
        err = ocrDbDestroy( IN_derefs_E8_9_dep_NEKOglobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E8_9_dep_reducPrivate.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E8_9_dep_NEKOtools.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E8_9_dep_gDone2.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E8_9_dep_NEKOstatics.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekMultiplicity_start, 0, DB_MODE_RW, IN_derefs_E8_9_dep_reducPrivate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekMultiplicity_start, 1, DB_MODE_RO, IN_derefs_E8_9_dep_NEKOtools.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekMultiplicity_start, 2, DB_MODE_RO, gd_NEKOglobals); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekMultiplicity_start, 3, DB_MODE_RO, IN_derefs_E8_9_dep_gDone2.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekMultiplicity_start, 4, DB_MODE_RO, IN_derefs_E8_9_dep_NEKOstatics.guid); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t nekMultiplicity_start(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 10
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E9_10_dep_NEKOstatics = depv[4];
        NEKOstatics_t * io_NEKOstatics = IN_derefs_E9_10_dep_NEKOstatics.ptr;
            Idz sz_C = io_NEKOstatics->pDOF3DperRmax+1;
        ocrEdtDep_t IN_derefs_E9_10_dep_NEKOglobals = depv[2];
        NEKOglobals_t * io_NEKOglobals = IN_derefs_E9_10_dep_NEKOglobals.ptr;
            ocrHint_t hintEDT, *pHintEDT=0;
            err = ocrXgetEdtHint(NEK_OCR_USE_CURRENT_PD, &hintEDT, &pHintEDT); IFEB;
            Triplet Rlattice = {io_NEKOstatics->Rx, io_NEKOstatics->Ry, io_NEKOstatics->Rz};
            Triplet Elattice = {io_NEKOstatics->Ex, io_NEKOstatics->Ey, io_NEKOstatics->Ez};
            const Idz length_riValues4multi = calculate_length_rankIndexedValue(io_NEKOglobals->pDOF, Elattice);
        ocrEdtDep_t IN_derefs_E9_10_dep_gDone2 = depv[3];
        ocrEdtDep_t IN_derefs_E9_10_dep_NEKOtools = depv[1];
        NEKOtools_t * io_NEKOtools = IN_derefs_E9_10_dep_NEKOtools.ptr;
        ocrEdtDep_t IN_derefs_E9_10_dep_reducPrivate = depv[0];
        reductionPrivate_t * io_reducPrivate = IN_derefs_E9_10_dep_reducPrivate.ptr;

        //----- Create_dataBlocks
        const u64 OA_Count_nekC = (sz_C)*sizeof(NBN_REAL);
        const u64 OA_Count_riValues4multi = length_riValues4multi * sizeof(rankIndexedValue_t);

        ocrGuid_t gd_nekC= NULL_GUID;
        NBN_REAL * o_C=NULL;
        err = ocrDbCreate( &gd_nekC, (void**)&o_C, (sz_C)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_riValues4multi= NULL_GUID;
        rankIndexedValue_t * riValues4multi=NULL;
        err = ocrDbCreate( &gd_riValues4multi, (void**)&riValues4multi, length_riValues4multi * sizeof(rankIndexedValue_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs
        ocrGuid_t ga_nekMultiplicity_stop = NULL_GUID;
        err = ocrEdtXCreate(nekMultiplicity_stop, 0, NULL, SLOTCNT_total_channels4multiplicity, NULL, EDT_PROP_NONE, pHintEDT, &ga_nekMultiplicity_stop, NULL); IFEB;

        //----- User section
        err = nekbone_set_multiplicity_start(io_NEKOglobals, o_C); IFEB;
        err = start_halo_multiplicity(OA_DEBUG_OUTVARS, io_NEKOtools, Rlattice,Elattice, io_NEKOglobals, sz_C, o_C, riValues4multi, &ga_nekMultiplicity_stop); IFEB;

        //----- Release or destroy data blocks
        err = ocrDbRelease(gd_nekC); IFEB;
        err = ocrDbDestroy(gd_riValues4multi); IFEB;
        err = ocrDbRelease(IN_derefs_E9_10_dep_reducPrivate.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E9_10_dep_NEKOtools.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E9_10_dep_NEKOglobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E9_10_dep_gDone2.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E9_10_dep_NEKOstatics.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekMultiplicity_stop, 0, DB_MODE_RW, IN_derefs_E9_10_dep_reducPrivate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekMultiplicity_stop, 1, DB_MODE_RO, IN_derefs_E9_10_dep_NEKOglobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekMultiplicity_stop, 2, DB_MODE_RO, IN_derefs_E9_10_dep_NEKOstatics.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekMultiplicity_stop, 3, DB_MODE_RW, gd_nekC); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekMultiplicity_stop, 4, DB_MODE_RO, IN_derefs_E9_10_dep_NEKOtools.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekMultiplicity_stop, 5, DB_MODE_RO, IN_derefs_E9_10_dep_gDone2.guid); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t nekMultiplicity_stop(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 11
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E10_11_dep_NEKOstatics = depv[2];
        NEKOstatics_t * io_NEKOstatics = IN_derefs_E10_11_dep_NEKOstatics.ptr;
            unsigned int pdof = io_NEKOstatics->pDOF_max;
            unsigned int pdof2D = io_NEKOstatics->pDOF_max * io_NEKOstatics->pDOF_max;
        ocrEdtDep_t IN_derefs_E10_11_dep_NEKOglobals = depv[1];
        NEKOglobals_t * io_NEKOglobals = IN_derefs_E10_11_dep_NEKOglobals.ptr;
            ocrHint_t hintEDT, *pHintEDT=0;
            err = ocrXgetEdtHint(NEK_OCR_USE_CURRENT_PD, &hintEDT, &pHintEDT); IFEB;
        ocrEdtDep_t IN_derefs_E10_11_dep_NEKOtools = depv[4];
        NEKOtools_t * io_NEKOtools = IN_derefs_E10_11_dep_NEKOtools.ptr;
        ocrEdtDep_t IN_derefs_E10_11_dep_gDone2 = depv[5];
        ocrGuid_t * io_gDone2 = IN_derefs_E10_11_dep_gDone2.ptr;
        ocrEdtDep_t IN_derefs_E10_11_dep_nekC = depv[3];
        NBN_REAL * io_C = IN_derefs_E10_11_dep_nekC.ptr;
        ocrEdtDep_t IN_derefs_E10_11_dep_reducPrivate = depv[0];
        reductionPrivate_t * io_reducPrivate = IN_derefs_E10_11_dep_reducPrivate.ptr;

        //----- Create_dataBlocks
        const u64 OA_Count_nek_wA = (pdof2D+1)*sizeof(NBN_REAL);
        const u64 OA_Count_nek_wC = (pdof2D+1)*sizeof(NBN_REAL);
        const u64 OA_Count_nek_wD = (pdof2D+1)*sizeof(NBN_REAL);
        const u64 OA_Count_nek_wB = (pdof+1)*sizeof(NBN_REAL);
        const u64 OA_Count_nek_wZ = (pdof+1)*sizeof(NBN_REAL);
        const u64 OA_Count_nek_wW = (2*pdof+1)*sizeof(NBN_REAL);
        const u64 OA_Count_nek_wZd = (pdof+1)*sizeof(double);
        const u64 OA_Count_nek_wWd = (2*pdof+1)*sizeof(double);
        const u64 OA_Count_nek_G1 = (io_NEKOstatics->pDOFmax3D+1)*sizeof(NBN_REAL);
        const u64 OA_Count_nek_G4 = (io_NEKOstatics->pDOFmax3D+1)*sizeof(NBN_REAL);
        const u64 OA_Count_nek_G6 = (io_NEKOstatics->pDOFmax3D+1)*sizeof(NBN_REAL);
        const u64 OA_Count_nek_dxm1 = (io_NEKOstatics->pDOFmax2D+1)*sizeof(BLAS_REAL_TYPE);
        const u64 OA_Count_nek_dxTm1 = (io_NEKOstatics->pDOFmax2D+1)*sizeof(BLAS_REAL_TYPE);

        ocrGuid_t gd_nek_wA= NULL_GUID;
        NBN_REAL * in_wA=NULL;
        err = ocrDbCreate( &gd_nek_wA, (void**)&in_wA, (pdof2D+1)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nek_wC= NULL_GUID;
        NBN_REAL * in_wC=NULL;
        err = ocrDbCreate( &gd_nek_wC, (void**)&in_wC, (pdof2D+1)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nek_wD= NULL_GUID;
        NBN_REAL * in_wD=NULL;
        err = ocrDbCreate( &gd_nek_wD, (void**)&in_wD, (pdof2D+1)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nek_wB= NULL_GUID;
        NBN_REAL * in_wB=NULL;
        err = ocrDbCreate( &gd_nek_wB, (void**)&in_wB, (pdof+1)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nek_wZ= NULL_GUID;
        NBN_REAL * in_wZ=NULL;
        err = ocrDbCreate( &gd_nek_wZ, (void**)&in_wZ, (pdof+1)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nek_wW= NULL_GUID;
        NBN_REAL * in_wW=NULL;
        err = ocrDbCreate( &gd_nek_wW, (void**)&in_wW, (2*pdof+1)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nek_wZd= NULL_GUID;
        double * in_wZd=NULL;
        err = ocrDbCreate( &gd_nek_wZd, (void**)&in_wZd, (pdof+1)*sizeof(double), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nek_wWd= NULL_GUID;
        double * in_wWd=NULL;
        err = ocrDbCreate( &gd_nek_wWd, (void**)&in_wWd, (2*pdof+1)*sizeof(double), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nek_G1= NULL_GUID;
        NBN_REAL * o_G1=NULL;
        err = ocrDbCreate( &gd_nek_G1, (void**)&o_G1, (io_NEKOstatics->pDOFmax3D+1)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nek_G4= NULL_GUID;
        NBN_REAL * o_G4=NULL;
        err = ocrDbCreate( &gd_nek_G4, (void**)&o_G4, (io_NEKOstatics->pDOFmax3D+1)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nek_G6= NULL_GUID;
        NBN_REAL * o_G6=NULL;
        err = ocrDbCreate( &gd_nek_G6, (void**)&o_G6, (io_NEKOstatics->pDOFmax3D+1)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nek_dxm1= NULL_GUID;
        BLAS_REAL_TYPE * o_dxm1=NULL;
        err = ocrDbCreate( &gd_nek_dxm1, (void**)&o_dxm1, (io_NEKOstatics->pDOFmax2D+1)*sizeof(BLAS_REAL_TYPE), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nek_dxTm1= NULL_GUID;
        BLAS_REAL_TYPE * o_dxTm1=NULL;
        err = ocrDbCreate( &gd_nek_dxTm1, (void**)&o_dxTm1, (io_NEKOstatics->pDOFmax2D+1)*sizeof(BLAS_REAL_TYPE), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs
        ocrGuid_t ga_nekSetF_start = NULL_GUID;
        err = ocrEdtXCreate(nekSetF_start, 0, NULL, 6, NULL, EDT_PROP_NONE, pHintEDT, &ga_nekSetF_start, NULL); IFEB;

        //----- User section
        err = stop_halo_multiplicity(OA_DEBUG_OUTVARS, io_NEKOtools, depv, io_C); IFEB;
        err = nekbone_set_multiplicity_stop(io_NEKOglobals, io_C); IFEB;
        err = nekbone_proxy_setup(io_NEKOstatics, io_NEKOglobals,in_wA,in_wC,in_wD, in_wB,in_wZ,in_wW, in_wZd,in_wWd,o_G1,o_G4,o_G6, o_dxm1,o_dxTm1); IFEB;
        ocrGuid_t ga_setupTailRecursion = *io_gDone2;

        //----- Release or destroy data blocks
        err = ocrDbDestroy(gd_nek_wA); IFEB;
        err = ocrDbDestroy(gd_nek_wC); IFEB;
        err = ocrDbDestroy(gd_nek_wD); IFEB;
        err = ocrDbDestroy(gd_nek_wB); IFEB;
        err = ocrDbDestroy(gd_nek_wZ); IFEB;
        err = ocrDbDestroy(gd_nek_wW); IFEB;
        err = ocrDbDestroy(gd_nek_wZd); IFEB;
        err = ocrDbDestroy(gd_nek_wWd); IFEB;
        err = ocrDbRelease(gd_nek_G1); IFEB;
        err = ocrDbRelease(gd_nek_G4); IFEB;
        err = ocrDbRelease(gd_nek_G6); IFEB;
        err = ocrDbRelease(gd_nek_dxm1); IFEB;
        err = ocrDbRelease(gd_nek_dxTm1); IFEB;
        err = ocrDbRelease(IN_derefs_E10_11_dep_reducPrivate.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E10_11_dep_NEKOglobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E10_11_dep_NEKOstatics.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E10_11_dep_nekC.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E10_11_dep_NEKOtools.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E10_11_dep_gDone2.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_setupTailRecursion, 2, DB_MODE_RO, gd_nek_G6); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_setupTailRecursion, 3, DB_MODE_RO, gd_nek_dxTm1); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_setupTailRecursion, 4, DB_MODE_RO, gd_nek_G4); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_setupTailRecursion, 5, DB_MODE_RO, gd_nek_dxm1); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_setupTailRecursion, 6, DB_MODE_RO, gd_nek_G1); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekSetF_start, 0, DB_MODE_RW, IN_derefs_E10_11_dep_reducPrivate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekSetF_start, 1, DB_MODE_RO, IN_derefs_E10_11_dep_NEKOglobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekSetF_start, 2, DB_MODE_RO, IN_derefs_E10_11_dep_NEKOstatics.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekSetF_start, 3, DB_MODE_RO, IN_derefs_E10_11_dep_nekC.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekSetF_start, 4, DB_MODE_RO, IN_derefs_E10_11_dep_NEKOtools.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekSetF_start, 5, DB_MODE_RO, IN_derefs_E10_11_dep_gDone2.guid); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t nekSetF_start(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 12
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E11_12_dep_NEKOstatics = depv[2];
        NEKOstatics_t * io_NEKOstatics = IN_derefs_E11_12_dep_NEKOstatics.ptr;
        ocrEdtDep_t IN_derefs_E11_12_dep_NEKOglobals = depv[1];
        NEKOglobals_t * io_NEKOglobals = IN_derefs_E11_12_dep_NEKOglobals.ptr;
            ocrHint_t hintEDT, *pHintEDT=0;
            err = ocrXgetEdtHint(NEK_OCR_USE_CURRENT_PD, &hintEDT, &pHintEDT); IFEB;
            Triplet Rlattice = {io_NEKOstatics->Rx, io_NEKOstatics->Ry, io_NEKOstatics->Rz};
            Triplet Elattice = {io_NEKOstatics->Ex, io_NEKOstatics->Ey, io_NEKOstatics->Ez};
            const Idz length_riValues4setF = calculate_length_rankIndexedValue(io_NEKOglobals->pDOF, Elattice);
            Idz sz_F = io_NEKOglobals->pDOF3DperR+1;
        ocrEdtDep_t IN_derefs_E11_12_dep_NEKOtools = depv[4];
        NEKOtools_t * io_NEKOtools = IN_derefs_E11_12_dep_NEKOtools.ptr;
        ocrEdtDep_t IN_derefs_E11_12_dep_gDone2 = depv[5];
        ocrGuid_t * in_gDone2 = IN_derefs_E11_12_dep_gDone2.ptr;
        ocrEdtDep_t IN_derefs_E11_12_dep_nekC = depv[3];
        ocrEdtDep_t IN_derefs_E11_12_dep_reducPrivate = depv[0];
        reductionPrivate_t * io_reducPrivate = IN_derefs_E11_12_dep_reducPrivate.ptr;

        //----- Create_dataBlocks
        const u64 OA_Count_nekF = (sz_F)*sizeof(NBN_REAL);
        const u64 OA_Count_riValues4setF = length_riValues4setF * sizeof(rankIndexedValue_t);

        ocrGuid_t gd_nekF= NULL_GUID;
        NBN_REAL * o_nekF=NULL;
        err = ocrDbCreate( &gd_nekF, (void**)&o_nekF, (sz_F)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_riValues4setF= NULL_GUID;
        rankIndexedValue_t * riValues4setF=NULL;
        err = ocrDbCreate( &gd_riValues4setF, (void**)&riValues4setF, length_riValues4setF * sizeof(rankIndexedValue_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs
        ocrGuid_t ga_nekSetF_stop = NULL_GUID;
        err = ocrEdtXCreate(nekSetF_stop, 0, NULL, SLOTCNT_total_channels4setf, NULL, EDT_PROP_NONE, pHintEDT, &ga_nekSetF_stop, NULL); IFEB;

        //----- User section
        err = nekbone_set_f_start(io_NEKOstatics, io_NEKOglobals, o_nekF);IFEB;
        err = start_halo_setf(OA_DEBUG_OUTVARS, io_NEKOtools, Rlattice,Elattice, io_NEKOglobals, sz_F, o_nekF, riValues4setF, &ga_nekSetF_stop); IFEB;

        //----- Release or destroy data blocks
        err = ocrDbRelease(gd_nekF); IFEB;
        err = ocrDbDestroy(gd_riValues4setF); IFEB;
        err = ocrDbRelease(IN_derefs_E11_12_dep_reducPrivate.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E11_12_dep_NEKOglobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E11_12_dep_NEKOstatics.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E11_12_dep_nekC.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E11_12_dep_NEKOtools.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E11_12_dep_gDone2.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekSetF_stop, 0, DB_MODE_RW, gd_nekF); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekSetF_stop, 1, DB_MODE_RW, IN_derefs_E11_12_dep_reducPrivate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekSetF_stop, 2, DB_MODE_RO, IN_derefs_E11_12_dep_NEKOglobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekSetF_stop, 3, DB_MODE_RO, IN_derefs_E11_12_dep_NEKOstatics.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekSetF_stop, 4, DB_MODE_RO, IN_derefs_E11_12_dep_nekC.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekSetF_stop, 5, DB_MODE_RO, IN_derefs_E11_12_dep_NEKOtools.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekSetF_stop, 6, DB_MODE_RO, IN_derefs_E11_12_dep_gDone2.guid); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t nekSetF_stop(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 13
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E12_13_dep_NEKOstatics = depv[3];
        NEKOstatics_t * io_NEKOstatics = IN_derefs_E12_13_dep_NEKOstatics.ptr;
        ocrEdtDep_t IN_derefs_E12_13_dep_NEKOglobals = depv[2];
        NEKOglobals_t * io_NEKOglobals = IN_derefs_E12_13_dep_NEKOglobals.ptr;
        ocrHint_t hintEDT, *pHintEDT=0;
        err = ocrXgetEdtHint(NEK_OCR_USE_CURRENT_PD, &hintEDT, &pHintEDT); IFEB;
        ocrEdtDep_t IN_derefs_E12_13_dep_reducPrivate = depv[1];
        reductionPrivate_t * io_reducPrivate = IN_derefs_E12_13_dep_reducPrivate.ptr;
        ocrEdtDep_t IN_derefs_E12_13_dep_NEKOtools = depv[5];
        NEKOtools_t * io_NEKOtools = IN_derefs_E12_13_dep_NEKOtools.ptr;
        ocrEdtDep_t IN_derefs_E12_13_dep_gDone2 = depv[6];
        ocrGuid_t * in_gDone2 = IN_derefs_E12_13_dep_gDone2.ptr;
        ocrEdtDep_t IN_derefs_E12_13_dep_nekC = depv[4];
        NBN_REAL * io_C = IN_derefs_E12_13_dep_nekC.ptr;
        ocrEdtDep_t IN_derefs_E12_13_dep_nekF = depv[0];
        NBN_REAL * io_nekF = IN_derefs_E12_13_dep_nekF.ptr;

        //----- Create_dataBlocks


        //----- Create children EDTs
        ocrGuid_t ga_nekCGstep0_start = NULL_GUID;
        err = ocrEdtXCreate(nekCGstep0_start, 0, NULL, 7, NULL, EDT_PROP_NONE, pHintEDT, &ga_nekCGstep0_start, NULL); IFEB;

        //----- User section
        err = stop_halo_setf(OA_DEBUG_OUTVARS, io_NEKOtools, depv, io_nekF); IFEB;
        err = nekbone_set_f_stop(io_NEKOglobals, io_C, io_nekF); IFEB;

        //----- Release or destroy data blocks
        err = ocrDbRelease(IN_derefs_E12_13_dep_nekF.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E12_13_dep_reducPrivate.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E12_13_dep_NEKOglobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E12_13_dep_NEKOstatics.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E12_13_dep_nekC.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E12_13_dep_NEKOtools.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E12_13_dep_gDone2.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCGstep0_start, 0, DB_MODE_RO, IN_derefs_E12_13_dep_nekF.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCGstep0_start, 1, DB_MODE_RW, IN_derefs_E12_13_dep_reducPrivate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCGstep0_start, 2, DB_MODE_RO, IN_derefs_E12_13_dep_NEKOglobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCGstep0_start, 3, DB_MODE_RO, IN_derefs_E12_13_dep_NEKOstatics.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCGstep0_start, 4, DB_MODE_RO, IN_derefs_E12_13_dep_nekC.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCGstep0_start, 5, DB_MODE_RO, IN_derefs_E12_13_dep_NEKOtools.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCGstep0_start, 6, DB_MODE_RO, IN_derefs_E12_13_dep_gDone2.guid); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t nekCGstep0_start(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 14
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E13_14_dep_NEKOstatics = depv[3];
        NEKOstatics_t * io_NEKOstatics = IN_derefs_E13_14_dep_NEKOstatics.ptr;
        ocrEdtDep_t IN_derefs_E13_14_dep_NEKOglobals = depv[2];
        NEKOglobals_t * io_NEKOglobals = IN_derefs_E13_14_dep_NEKOglobals.ptr;
            ocrHint_t hintEDT, *pHintEDT=0;
            err = ocrXgetEdtHint(NEK_OCR_USE_CURRENT_PD, &hintEDT, &pHintEDT); IFEB;
        ocrEdtDep_t IN_derefs_E13_14_dep_NEKOtools = depv[5];
        ocrEdtDep_t IN_derefs_E13_14_dep_gDone2 = depv[6];
        ocrGuid_t * in_gDone2 = IN_derefs_E13_14_dep_gDone2.ptr;
        ocrEdtDep_t IN_derefs_E13_14_dep_nekC = depv[4];
        NBN_REAL * io_C = IN_derefs_E13_14_dep_nekC.ptr;
        ocrEdtDep_t IN_derefs_E13_14_dep_nekF = depv[0];
        NBN_REAL * in_nekF = IN_derefs_E13_14_dep_nekF.ptr;
        ocrEdtDep_t IN_derefs_E13_14_dep_reducPrivate = depv[1];
        reductionPrivate_t * io_reducPrivate = IN_derefs_E13_14_dep_reducPrivate.ptr;
        ocrGuid_t guid_reducPrivate = IN_derefs_E13_14_dep_reducPrivate.guid;
            //DBK reducPrivate will be released by reductionLaunch() in nekbone_CGstep0_start().

        //----- Create_dataBlocks
        const u64 OA_Count_nekR = (io_NEKOglobals->pDOF3DperR+1)*sizeof(NBN_REAL);

        ocrGuid_t gd_nekR= NULL_GUID;
        NBN_REAL * o_nekR=NULL;
        err = ocrDbCreate( &gd_nekR, (void**)&o_nekR, (io_NEKOglobals->pDOF3DperR+1)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs
        ocrGuid_t ga_nekCGstep0_stop = NULL_GUID;
        err = ocrEdtXCreate(nekCGstep0_stop, 0, NULL, 8, NULL, EDT_PROP_NONE, pHintEDT, &ga_nekCGstep0_stop, NULL); IFEB;

        //----- User section
        err = nekbone_CGstep0_start(io_NEKOstatics, io_NEKOglobals, in_nekF, io_C, o_nekR, guid_reducPrivate, io_reducPrivate, REDUC_SLOT_4CGstep0-1, ga_nekCGstep0_stop); IFEB;

        //----- Release or destroy data blocks
        err = ocrDbRelease(gd_nekR); IFEB;
        err = ocrDbDestroy( IN_derefs_E13_14_dep_nekF.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E13_14_dep_NEKOglobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E13_14_dep_NEKOstatics.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E13_14_dep_nekC.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E13_14_dep_NEKOtools.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E13_14_dep_gDone2.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCGstep0_stop, 0, DB_MODE_RW, IN_derefs_E13_14_dep_reducPrivate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCGstep0_stop, 1, DB_MODE_RO, IN_derefs_E13_14_dep_NEKOglobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCGstep0_stop, 2, DB_MODE_RO, IN_derefs_E13_14_dep_NEKOstatics.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCGstep0_stop, 3, DB_MODE_RO, IN_derefs_E13_14_dep_nekC.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCGstep0_stop, 4, DB_MODE_RO, gd_nekR); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCGstep0_stop, 5, DB_MODE_RO, IN_derefs_E13_14_dep_NEKOtools.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCGstep0_stop, 6, DB_MODE_RO, IN_derefs_E13_14_dep_gDone2.guid); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t nekCGstep0_stop(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 15
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E14_15_dep_NEKOstatics = depv[2];
        NEKOstatics_t * io_NEKOstatics = IN_derefs_E14_15_dep_NEKOstatics.ptr;
        ocrEdtDep_t IN_derefs_E14_15_dep_NEKOglobals = depv[1];
        NEKOglobals_t * io_NEKOglobals = IN_derefs_E14_15_dep_NEKOglobals.ptr;
        ocrEdtDep_t IN_derefs_E14_15_dep_NEKOtools = depv[5];
        ocrEdtDep_t IN_derefs_E14_15_dep_gDone2 = depv[6];
        ocrGuid_t * in_gDone2 = IN_derefs_E14_15_dep_gDone2.ptr;
        ocrEdtDep_t IN_derefs_E14_15_dep_nekC = depv[3];
        ocrEdtDep_t IN_derefs_E14_15_dep_nekR = depv[4];
        ocrEdtDep_t IN_derefs_E14_15_dep_reducPrivate = depv[0];
            ocrEdtDep_t IN_BY_USER_depv_sum = depv[REDUC_SLOT_4CGstep0-1];  //From nekbone_CGstep0_start().
            ocrGuid_t in_sum_guid = IN_BY_USER_depv_sum.guid;
            ReducSum_t * in_sum = IN_BY_USER_depv_sum.ptr;

        //----- Create_dataBlocks
        const u64 OA_Count_nekX = (io_NEKOglobals->pDOF3DperR+1)*sizeof(NBN_REAL);
        const u64 OA_Count_nekW = (io_NEKOglobals->pDOF3DperR+1)*sizeof(NBN_REAL);
        const u64 OA_Count_nekP = (io_NEKOglobals->pDOF3DperR+1)*sizeof(NBN_REAL);
        const u64 OA_Count_nekZ = (io_NEKOglobals->pDOF3DperR+1)*sizeof(NBN_REAL);
        const u64 OA_Count_nekCGscalars = 1;
        const u64 OA_Count_CGtimes = 1;

        ocrGuid_t gd_nekX= NULL_GUID;
        NBN_REAL * o_nekX=NULL;
        err = ocrDbCreate( &gd_nekX, (void**)&o_nekX, (io_NEKOglobals->pDOF3DperR+1)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nekW= NULL_GUID;
        NBN_REAL * o_nekW=NULL;
        err = ocrDbCreate( &gd_nekW, (void**)&o_nekW, (io_NEKOglobals->pDOF3DperR+1)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nekP= NULL_GUID;
        NBN_REAL * o_nekP=NULL;
        err = ocrDbCreate( &gd_nekP, (void**)&o_nekP, (io_NEKOglobals->pDOF3DperR+1)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nekZ= NULL_GUID;
        NBN_REAL * o_nekZ=NULL;
        err = ocrDbCreate( &gd_nekZ, (void**)&o_nekZ, (io_NEKOglobals->pDOF3DperR+1)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nekCGscalars= NULL_GUID;
        NEKO_CGscalars_t * o_nekCGscalars=NULL;
        err = ocrDbCreate( &gd_nekCGscalars, (void**)&o_nekCGscalars, 1*sizeof(NEKO_CGscalars_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_CGtimes= NULL_GUID;
        NEKO_CGtimings_t * o_CGtimes=NULL;
        err = ocrDbCreate( &gd_CGtimes, (void**)&o_CGtimes, 1*sizeof(NEKO_CGtimings_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs

        //----- User section
        ocrGuid_t ga_setupTailRecursion = *in_gDone2;
        err = nekbone_CGstep0_stop(io_NEKOstatics, io_NEKOglobals, o_nekX, o_nekW, o_nekP, o_nekZ, o_nekCGscalars, o_CGtimes, in_sum_guid, in_sum); IFEB;

        //----- Release or destroy data blocks
        err = ocrDbRelease(gd_nekX); IFEB;
        err = ocrDbRelease(gd_nekW); IFEB;
        err = ocrDbRelease(gd_nekP); IFEB;
        err = ocrDbRelease(gd_nekZ); IFEB;
        err = ocrDbRelease(gd_nekCGscalars); IFEB;
        err = ocrDbRelease(gd_CGtimes); IFEB;
        err = ocrDbDestroy( IN_derefs_E14_15_dep_gDone2.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E14_15_dep_reducPrivate.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E14_15_dep_NEKOglobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E14_15_dep_NEKOstatics.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E14_15_dep_nekC.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E14_15_dep_nekR.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E14_15_dep_NEKOtools.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_setupTailRecursion, 7, DB_MODE_RO, gd_nekW); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_setupTailRecursion, 8, DB_MODE_RW, IN_derefs_E14_15_dep_reducPrivate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_setupTailRecursion, 9, DB_MODE_RO, IN_derefs_E14_15_dep_nekR.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_setupTailRecursion, 10, DB_MODE_RO, gd_nekP); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_setupTailRecursion, 11, DB_MODE_RO, IN_derefs_E14_15_dep_NEKOtools.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_setupTailRecursion, 12, DB_MODE_RO, gd_nekZ); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_setupTailRecursion, 13, DB_MODE_RO, gd_nekX); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_setupTailRecursion, 14, DB_MODE_RO, IN_derefs_E14_15_dep_NEKOglobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_setupTailRecursion, 15, DB_MODE_RO, IN_derefs_E14_15_dep_NEKOstatics.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_setupTailRecursion, 16, DB_MODE_RO, gd_nekCGscalars); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_setupTailRecursion, 17, DB_MODE_RO, gd_CGtimes); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_setupTailRecursion, 18, DB_MODE_RO, IN_derefs_E14_15_dep_nekC.guid); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t BtForkTransition_Stop(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 16
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E7_16_dep_TFJiterate = depv[0];
        TFJiterate_t * in_TFJiterate = IN_derefs_E7_16_dep_TFJiterate.ptr;
        ocrEdtDep_t IN_derefs_E33_16_dep_SPMDGlobals = depv[4];
        ocrEdtDep_t IN_derefs_E33_16_dep_NEKOstatics = depv[5];
        NEKOstatics_t * in_NEKOstatics = IN_derefs_E33_16_dep_NEKOstatics.ptr;
        ocrEdtDep_t IN_derefs_E33_16_dep_NEKOglobals = depv[3];
        NEKOglobals_t * in_NEKOglobals = IN_derefs_E33_16_dep_NEKOglobals.ptr;
        ocrEdtDep_t IN_derefs_E33_16_dep_NEKOtools = depv[2];
        NEKOtools_t * in_NEKOtools = IN_derefs_E33_16_dep_NEKOtools.ptr;
        ocrEdtDep_t IN_derefs_E33_16_dep_reducPrivate = depv[1];
        reductionPrivate_t * in_reducPrivate = IN_derefs_E33_16_dep_reducPrivate.ptr;

        //----- Create_dataBlocks
        const u64 OA_Count_TCsum2 = 1;
        const u64 OA_Count_TCsum = 1;

        ocrGuid_t gd_TCsum2= NULL_GUID;
        TChecksum_work_t * o_work2=NULL;
        err = ocrDbCreate( &gd_TCsum2, (void**)&o_work2, 1*sizeof(TChecksum_work_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_TCsum= NULL_GUID;
        TChecksum_work_t * o_work=NULL;
        err = ocrDbCreate( &gd_TCsum, (void**)&o_work, 1*sizeof(TChecksum_work_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs

        //----- User section
        ocrGuid_t ga_BtJoinIFTHEN; GUID_ASSIGN_VALUE(ga_BtJoinIFTHEN, in_TFJiterate->whereToGoWhenFalse);
        err = transitionBTFork(OA_edtTypeNb, OA_DBG_thisEDT, in_TFJiterate, o_work, o_work2);
        //2016Nov15:No known destructor for in_reducPrivate
        //          in_reducPrivate Guid map will be uniquely destroyed in the finalEdt.
        err = nekbone_BtForkTransition_Stop(in_NEKOglobals); IFEB;

        //----- Release or destroy data blocks
        err = ocrDbRelease(gd_TCsum2); IFEB;
        err = ocrDbRelease(gd_TCsum); IFEB;
        err = ocrDbDestroy( IN_derefs_E7_16_dep_TFJiterate.guid ); IFEB;
        err = ocrDbDestroy( IN_derefs_E33_16_dep_reducPrivate.guid ); IFEB;
        err = ocrDbDestroy( IN_derefs_E33_16_dep_NEKOtools.guid ); IFEB;
        err = ocrDbDestroy( IN_derefs_E33_16_dep_NEKOglobals.guid ); IFEB;
        err = ocrDbDestroy( IN_derefs_E33_16_dep_SPMDGlobals.guid ); IFEB;
        err = ocrDbDestroy( IN_derefs_E33_16_dep_NEKOstatics.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtJoinIFTHEN, 1, DB_MODE_RO, gd_TCsum2); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtJoinIFTHEN, 2, DB_MODE_RO, gd_TCsum); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t BtJoinIFTHEN(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 17
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E4_17_dep_gDone = depv[0];
        ocrGuid_t * in_gDone = IN_derefs_E4_17_dep_gDone.ptr;
        ocrEdtDep_t IN_derefs_E16_17_dep_TCsum = depv[2];
        TChecksum_work_t * in_workLeft = IN_derefs_E16_17_dep_TCsum.ptr;
        ocrEdtDep_t IN_derefs_E16_17_dep_TCsum2 = depv[1];
        TChecksum_work_t * in_workRight = IN_derefs_E16_17_dep_TCsum2.ptr;

        if( NOTA_BTindex != get_parent_index(in_workLeft->btindex) ){
            //----- Create_dataBlocks
            const u64 OA_Count_TCsum = 1;

            ocrGuid_t gd_TCsum= NULL_GUID;
            TChecksum_work_t * o_work=NULL;
            err = ocrDbCreate( &gd_TCsum, (void**)&o_work, 1*sizeof(TChecksum_work_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

            //----- Create children EDTs

            //----- User section
            err = joinOperationIFTHEN(OA_edtTypeNb, OA_DBG_thisEDT, *in_workLeft, *in_workRight, o_work);
            ocrGuid_t ga_BtJoinIFTHEN; GUID_ASSIGN_VALUE(ga_BtJoinIFTHEN, *in_gDone);
            unsigned int slot_for_work = btCalculateJoinIndex(in_workLeft->btindex);

            //----- Release or destroy data blocks
            err = ocrDbRelease(gd_TCsum); IFEB;
            err = ocrDbDestroy( IN_derefs_E4_17_dep_gDone.guid ); IFEB;
            err = ocrDbDestroy( IN_derefs_E16_17_dep_TCsum2.guid ); IFEB;
            err = ocrDbDestroy( IN_derefs_E16_17_dep_TCsum.guid ); IFEB;

            //----- Link to other EDTs using Events
            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtJoinIFTHEN, slot_for_work, DB_MODE_RO, gd_TCsum); IFEB;

        }else{
            //----- Create_dataBlocks
            const u64 OA_Count_TCsum = 1;

            ocrGuid_t gd_TCsum= NULL_GUID;
            TChecksum_work_t * o_work=NULL;
            err = ocrDbCreate( &gd_TCsum, (void**)&o_work, 1*sizeof(TChecksum_work_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

            //----- Create children EDTs

            //----- User section
            err = joinOperationELSE(OA_edtTypeNb, OA_DBG_thisEDT, *in_workLeft, *in_workRight, o_work);
            ocrGuid_t ga_ConcludeBtForkJoin; GUID_ASSIGN_VALUE(ga_ConcludeBtForkJoin, *in_gDone);

            //----- Release or destroy data blocks
            err = ocrDbRelease(gd_TCsum); IFEB;
            err = ocrDbDestroy( IN_derefs_E4_17_dep_gDone.guid ); IFEB;
            err = ocrDbDestroy( IN_derefs_E16_17_dep_TCsum.guid ); IFEB;
            err = ocrDbDestroy( IN_derefs_E16_17_dep_TCsum2.guid ); IFEB;

            //----- Link to other EDTs using Events
            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_ConcludeBtForkJoin, 2, DB_MODE_RO, gd_TCsum); IFEB;

        }
        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t setupTailRecursion(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 19
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E7_19_dep_gDone = depv[1];
        ocrEdtDep_t IN_derefs_E7_19_dep_SPMDGlobals = depv[0];
        SPMD_GlobalData_t * io_SPMDglobals = IN_derefs_E7_19_dep_SPMDGlobals.ptr;
        ocrEdtDep_t IN_derefs_E15_19_dep_NEKOstatics = depv[15];
        ocrEdtDep_t IN_derefs_E15_19_dep_NEKOglobals = depv[14];
        NEKOglobals_t * io_NEKOglobals = IN_derefs_E15_19_dep_NEKOglobals.ptr;
        ocrHint_t hintEDT, *pHintEDT=0;
        err = ocrXgetEdtHint(NEK_OCR_USE_CURRENT_PD, &hintEDT, &pHintEDT); IFEB;
        ocrEdtDep_t IN_derefs_E15_19_dep_NEKOtools = depv[11];
        ocrEdtDep_t IN_derefs_E11_19_dep_nek_G1 = depv[6];
        ocrEdtDep_t IN_derefs_E11_19_dep_nek_G4 = depv[4];
        ocrEdtDep_t IN_derefs_E11_19_dep_nek_G6 = depv[2];
        ocrEdtDep_t IN_derefs_E11_19_dep_nek_dxm1 = depv[5];
        ocrEdtDep_t IN_derefs_E11_19_dep_nek_dxTm1 = depv[3];
        ocrEdtDep_t IN_derefs_E15_19_dep_nekC = depv[18];
        ocrEdtDep_t IN_derefs_E15_19_dep_nekP = depv[10];
        ocrEdtDep_t IN_derefs_E15_19_dep_nekR = depv[9];
        ocrEdtDep_t IN_derefs_E15_19_dep_nekW = depv[7];
        ocrEdtDep_t IN_derefs_E15_19_dep_nekX = depv[13];
        ocrEdtDep_t IN_derefs_E15_19_dep_nekZ = depv[12];
        ocrEdtDep_t IN_derefs_E15_19_dep_nekCGscalars = depv[16];
        NEKO_CGscalars_t * in_nekCGscalars = IN_derefs_E15_19_dep_nekCGscalars.ptr;
        ocrEdtDep_t IN_derefs_E15_19_dep_CGtimes = depv[17];
        ocrEdtDep_t IN_derefs_E15_19_dep_reducPrivate = depv[8];
        reductionPrivate_t * io_reducPrivate = IN_derefs_E15_19_dep_reducPrivate.ptr;

        //----- Create_dataBlocks
        const u64 OA_Count_TailRecurIterate = 1;
        const u64 OA_Count_nekCGscalars = 1;

        ocrGuid_t gd_TailRecurIterate= NULL_GUID;
        TailRecurIterate_t * o_tailRecurIterate=NULL;
        err = ocrDbCreate( &gd_TailRecurIterate, (void**)&o_tailRecurIterate, 1*sizeof(TailRecurIterate_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nekCGscalars= NULL_GUID;
        NEKO_CGscalars_t * o_nekCGscalars=NULL;
        err = ocrDbCreate( &gd_nekCGscalars, (void**)&o_nekCGscalars, 1*sizeof(NEKO_CGscalars_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs
        ocrGuid_t ga_concludeTailRecursion = NULL_GUID;
        err = ocrEdtXCreate(concludeTailRecursion, 0, NULL, 6, NULL, EDT_PROP_NONE, pHintEDT, &ga_concludeTailRecursion, NULL); IFEB;
        ocrGuid_t ga_tailRecursionIFThen = NULL_GUID;
        err = ocrEdtXCreate(tailRecursionIFThen, 0, NULL, 19, NULL, EDT_PROP_NONE, pHintEDT, &ga_tailRecursionIFThen, NULL); IFEB;

        //----- User section
        err = tailRecurInitialize(o_tailRecurIterate, ga_concludeTailRecursion, io_SPMDglobals); IFEB;
        err = nekbone_setupTailRecusion(io_NEKOglobals, in_nekCGscalars, o_nekCGscalars);IFEB;

        //----- Release or destroy data blocks
        err = ocrDbRelease(gd_TailRecurIterate); IFEB;
        err = ocrDbRelease(gd_nekCGscalars); IFEB;
        err = ocrDbDestroy( IN_derefs_E15_19_dep_nekCGscalars.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E7_19_dep_SPMDGlobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E7_19_dep_gDone.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E11_19_dep_nek_G6.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E11_19_dep_nek_dxTm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E11_19_dep_nek_G4.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E11_19_dep_nek_dxm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E11_19_dep_nek_G1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E15_19_dep_nekW.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E15_19_dep_reducPrivate.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E15_19_dep_nekR.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E15_19_dep_nekP.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E15_19_dep_NEKOtools.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E15_19_dep_nekZ.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E15_19_dep_nekX.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E15_19_dep_NEKOglobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E15_19_dep_NEKOstatics.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E15_19_dep_CGtimes.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E15_19_dep_nekC.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_concludeTailRecursion, 0, DB_MODE_RO, IN_derefs_E7_19_dep_gDone.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 0, DB_MODE_RW, gd_TailRecurIterate); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 1, DB_MODE_RW, IN_derefs_E15_19_dep_reducPrivate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 2, DB_MODE_RO, IN_derefs_E11_19_dep_nek_dxm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 3, DB_MODE_RO, IN_derefs_E15_19_dep_nekR.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 4, DB_MODE_RO, IN_derefs_E15_19_dep_NEKOstatics.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 5, DB_MODE_RO, IN_derefs_E15_19_dep_nekP.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 6, DB_MODE_RO, IN_derefs_E15_19_dep_nekW.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 7, DB_MODE_RO, IN_derefs_E15_19_dep_NEKOtools.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 8, DB_MODE_RO, IN_derefs_E15_19_dep_nekZ.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 9, DB_MODE_RO, IN_derefs_E15_19_dep_nekX.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 10, DB_MODE_RO, IN_derefs_E11_19_dep_nek_G6.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 11, DB_MODE_RO, IN_derefs_E11_19_dep_nek_G4.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 12, DB_MODE_RO, IN_derefs_E15_19_dep_NEKOglobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 13, DB_MODE_RO, IN_derefs_E7_19_dep_SPMDGlobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 14, DB_MODE_RO, IN_derefs_E11_19_dep_nek_G1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 15, DB_MODE_RO, IN_derefs_E11_19_dep_nek_dxTm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 16, DB_MODE_RO, IN_derefs_E15_19_dep_CGtimes.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 17, DB_MODE_RO, IN_derefs_E15_19_dep_nekC.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 18, DB_MODE_RO, gd_nekCGscalars); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t tailRecursionIFThen(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 20
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E19_20_dep_SPMDGlobals = depv[13];
        ocrEdtDep_t IN_derefs_E19_20_dep_TailRecurIterate = depv[0];
        TailRecurIterate_t * io_tailRecurIterate = IN_derefs_E19_20_dep_TailRecurIterate.ptr;
        ocrGuid_t ga_concludeTailRecursion = io_tailRecurIterate->whereToGoWhenDone;
        ocrEdtDep_t IN_derefs_E19_20_dep_NEKOstatics = depv[4];
        ocrEdtDep_t IN_derefs_E19_20_dep_NEKOglobals = depv[12];
        ocrHint_t hintEDT, *pHintEDT=0;
        err = ocrXgetEdtHint(NEK_OCR_USE_CURRENT_PD, &hintEDT, &pHintEDT); IFEB;
        ocrEdtDep_t IN_derefs_E19_20_dep_NEKOtools = depv[7];
        ocrEdtDep_t IN_derefs_E19_20_dep_nek_G1 = depv[14];
        ocrEdtDep_t IN_derefs_E19_20_dep_nek_G4 = depv[11];
        ocrEdtDep_t IN_derefs_E19_20_dep_nek_G6 = depv[10];
        ocrEdtDep_t IN_derefs_E19_20_dep_nek_dxm1 = depv[2];
        ocrEdtDep_t IN_derefs_E19_20_dep_nek_dxTm1 = depv[15];
        ocrEdtDep_t IN_derefs_E19_20_dep_nekC = depv[17];
        ocrEdtDep_t IN_derefs_E19_20_dep_nekP = depv[5];
        ocrEdtDep_t IN_derefs_E19_20_dep_nekR = depv[3];
        ocrEdtDep_t IN_derefs_E19_20_dep_nekW = depv[6];
        ocrEdtDep_t IN_derefs_E19_20_dep_nekX = depv[9];
        ocrEdtDep_t IN_derefs_E19_20_dep_nekZ = depv[8];
        ocrEdtDep_t IN_derefs_E19_20_dep_nekCGscalars = depv[18];
        ocrEdtDep_t IN_derefs_E19_20_dep_CGtimes = depv[16];
        NEKO_CGtimings_t * io_CGtimes = IN_derefs_E19_20_dep_CGtimes.ptr;
        ocrEdtDep_t IN_derefs_E19_20_dep_reducPrivate = depv[1];
        reductionPrivate_t * io_reducPrivate = IN_derefs_E19_20_dep_reducPrivate.ptr;

        if( tailRecurCondition(io_tailRecurIterate) ){
            //----- Create_dataBlocks


            //----- Create children EDTs
            ocrGuid_t ga_tailRecurTransitBEGIN = NULL_GUID;
            err = ocrEdtXCreate(tailRecurTransitBEGIN, 0, NULL, 19, NULL, EDT_PROP_NONE, pHintEDT, &ga_tailRecurTransitBEGIN, NULL); IFEB;

            //----- User section
            err = tailRecurIfThenClause(io_tailRecurIterate); IFEB;

            //----- Release or destroy data blocks
            err = ocrDbRelease(IN_derefs_E19_20_dep_TailRecurIterate.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_reducPrivate.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_nek_dxm1.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_nekR.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_NEKOstatics.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_nekP.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_nekW.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_NEKOtools.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_nekZ.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_nekX.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_nek_G6.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_nek_G4.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_NEKOglobals.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_SPMDGlobals.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_nek_G1.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_nek_dxTm1.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_CGtimes.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_nekC.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_nekCGscalars.guid ); IFEB;

            //----- Link to other EDTs using Events
            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitBEGIN, 0, DB_MODE_RO, IN_derefs_E19_20_dep_TailRecurIterate.guid); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitBEGIN, 1, DB_MODE_RW, IN_derefs_E19_20_dep_reducPrivate.guid); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitBEGIN, 2, DB_MODE_RO, IN_derefs_E19_20_dep_nek_dxm1.guid); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitBEGIN, 3, DB_MODE_RO, IN_derefs_E19_20_dep_nekR.guid); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitBEGIN, 4, DB_MODE_RO, IN_derefs_E19_20_dep_NEKOstatics.guid); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitBEGIN, 5, DB_MODE_RO, IN_derefs_E19_20_dep_nekP.guid); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitBEGIN, 6, DB_MODE_RO, IN_derefs_E19_20_dep_nekW.guid); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitBEGIN, 7, DB_MODE_RO, IN_derefs_E19_20_dep_NEKOtools.guid); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitBEGIN, 8, DB_MODE_RO, IN_derefs_E19_20_dep_nekZ.guid); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitBEGIN, 9, DB_MODE_RO, IN_derefs_E19_20_dep_nekX.guid); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitBEGIN, 10, DB_MODE_RO, IN_derefs_E19_20_dep_nek_G6.guid); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitBEGIN, 11, DB_MODE_RO, IN_derefs_E19_20_dep_nek_G4.guid); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitBEGIN, 12, DB_MODE_RO, IN_derefs_E19_20_dep_NEKOglobals.guid); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitBEGIN, 13, DB_MODE_RO, IN_derefs_E19_20_dep_SPMDGlobals.guid); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitBEGIN, 14, DB_MODE_RO, IN_derefs_E19_20_dep_nek_G1.guid); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitBEGIN, 15, DB_MODE_RO, IN_derefs_E19_20_dep_nek_dxTm1.guid); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitBEGIN, 16, DB_MODE_RO, IN_derefs_E19_20_dep_CGtimes.guid); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitBEGIN, 17, DB_MODE_RO, IN_derefs_E19_20_dep_nekC.guid); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitBEGIN, 18, DB_MODE_RO, IN_derefs_E19_20_dep_nekCGscalars.guid); IFEB;

        }else{
            //----- Create_dataBlocks


            //----- Create children EDTs

            //----- User section
            err = tailRecurElseClause(io_tailRecurIterate); IFEB;
            err = nekbone_tailRecursionELSE(io_CGtimes); IFEB;

            //----- Release or destroy data blocks
            err = ocrDbDestroy( IN_derefs_E19_20_dep_TailRecurIterate.guid ); IFEB;
            err = ocrDbDestroy( IN_derefs_E19_20_dep_nek_G1.guid ); IFEB;
            err = ocrDbDestroy( IN_derefs_E19_20_dep_nek_G4.guid ); IFEB;
            err = ocrDbDestroy( IN_derefs_E19_20_dep_nek_G6.guid ); IFEB;
            err = ocrDbDestroy( IN_derefs_E19_20_dep_nek_dxm1.guid ); IFEB;
            err = ocrDbDestroy( IN_derefs_E19_20_dep_nek_dxTm1.guid ); IFEB;
            err = ocrDbDestroy( IN_derefs_E19_20_dep_nekC.guid ); IFEB;
            err = ocrDbDestroy( IN_derefs_E19_20_dep_nekP.guid ); IFEB;
            err = ocrDbDestroy( IN_derefs_E19_20_dep_nekR.guid ); IFEB;
            err = ocrDbDestroy( IN_derefs_E19_20_dep_nekW.guid ); IFEB;
            err = ocrDbDestroy( IN_derefs_E19_20_dep_nekX.guid ); IFEB;
            err = ocrDbDestroy( IN_derefs_E19_20_dep_nekZ.guid ); IFEB;
            err = ocrDbDestroy( IN_derefs_E19_20_dep_nekCGscalars.guid ); IFEB;
            err = ocrDbDestroy( IN_derefs_E19_20_dep_CGtimes.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_SPMDGlobals.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_NEKOstatics.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_NEKOglobals.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_NEKOtools.guid ); IFEB;
            err = ocrDbRelease(IN_derefs_E19_20_dep_reducPrivate.guid ); IFEB;

            //----- Link to other EDTs using Events
            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_concludeTailRecursion, 1, DB_MODE_RO, IN_derefs_E19_20_dep_reducPrivate.guid); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_concludeTailRecursion, 2, DB_MODE_RO, IN_derefs_E19_20_dep_NEKOtools.guid); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_concludeTailRecursion, 3, DB_MODE_RO, IN_derefs_E19_20_dep_NEKOglobals.guid); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_concludeTailRecursion, 4, DB_MODE_RO, IN_derefs_E19_20_dep_SPMDGlobals.guid); IFEB;

            err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_concludeTailRecursion, 5, DB_MODE_RO, IN_derefs_E19_20_dep_NEKOstatics.guid); IFEB;

        }
        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t tailRecurTransitBEGIN(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 22
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E20_22_dep_TailRecurIterate = depv[0];
        TailRecurIterate_t * io_tailRecurIterate = IN_derefs_E20_22_dep_TailRecurIterate.ptr;
        ocrEdtDep_t IN_derefs_E20_22_dep_SPMDGlobals = depv[13];
        ocrEdtDep_t IN_derefs_E20_22_dep_NEKOstatics = depv[4];
        ocrEdtDep_t IN_derefs_E20_22_dep_NEKOglobals = depv[12];
        ocrHint_t hintEDT, *pHintEDT=0;
        err = ocrXgetEdtHint(NEK_OCR_USE_CURRENT_PD, &hintEDT, &pHintEDT); IFEB;
        ocrEdtDep_t IN_derefs_E20_22_dep_NEKOtools = depv[7];
        ocrEdtDep_t IN_derefs_E20_22_dep_nek_G1 = depv[14];
        ocrEdtDep_t IN_derefs_E20_22_dep_nek_G4 = depv[11];
        ocrEdtDep_t IN_derefs_E20_22_dep_nek_G6 = depv[10];
        ocrEdtDep_t IN_derefs_E20_22_dep_nek_dxm1 = depv[2];
        ocrEdtDep_t IN_derefs_E20_22_dep_nek_dxTm1 = depv[15];
        ocrEdtDep_t IN_derefs_E20_22_dep_nekC = depv[17];
        ocrEdtDep_t IN_derefs_E20_22_dep_nekP = depv[5];
        ocrEdtDep_t IN_derefs_E20_22_dep_nekR = depv[3];
        ocrEdtDep_t IN_derefs_E20_22_dep_nekW = depv[6];
        ocrEdtDep_t IN_derefs_E20_22_dep_nekX = depv[9];
        ocrEdtDep_t IN_derefs_E20_22_dep_nekZ = depv[8];
        ocrEdtDep_t IN_derefs_E20_22_dep_nekCGscalars = depv[18];
        NEKO_CGscalars_t * in_nekCGscalars = IN_derefs_E20_22_dep_nekCGscalars.ptr;
        ocrEdtDep_t IN_derefs_E20_22_dep_CGtimes = depv[16];
        NEKO_CGtimings_t * in_CGtimes = IN_derefs_E20_22_dep_CGtimes.ptr;
        ocrEdtDep_t IN_derefs_E20_22_dep_reducPrivate = depv[1];
        reductionPrivate_t * io_reducPrivate = IN_derefs_E20_22_dep_reducPrivate.ptr;

        //----- Create_dataBlocks
        const u64 OA_Count_gDone3 = 1;
        const u64 OA_Count_nekCGscalars = 1;
        const u64 OA_Count_CGtimes = 1;

        ocrGuid_t gd_gDone3= NULL_GUID;
        ocrGuid_t * o_gDone3=NULL;
        err = ocrDbCreate( &gd_gDone3, (void**)&o_gDone3, 1*sizeof(ocrGuid_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nekCGscalars= NULL_GUID;
        NEKO_CGscalars_t * o_nekCGscalars=NULL;
        err = ocrDbCreate( &gd_nekCGscalars, (void**)&o_nekCGscalars, 1*sizeof(NEKO_CGscalars_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_CGtimes= NULL_GUID;
        NEKO_CGtimings_t * o_CGtimes=NULL;
        err = ocrDbCreate( &gd_CGtimes, (void**)&o_CGtimes, 1*sizeof(NEKO_CGtimings_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs
        ocrGuid_t ga_tailRecurTransitEND = NULL_GUID;
        err = ocrEdtXCreate(tailRecurTransitEND, 0, NULL, 19, NULL, EDT_PROP_NONE, pHintEDT, &ga_tailRecurTransitEND, NULL); IFEB;
        ocrGuid_t ga_nekCG_solveMi = NULL_GUID;
        err = ocrEdtXCreate(nekCG_solveMi, 0, NULL, 18, NULL, EDT_PROP_NONE, pHintEDT, &ga_nekCG_solveMi, NULL); IFEB;

        //----- User section
        *o_gDone3 = ga_tailRecurTransitEND;
        err = nekbone_tailTransitBegin(io_tailRecurIterate->current, in_nekCGscalars, o_nekCGscalars, in_CGtimes, o_CGtimes); IFEB;

        //----- Release or destroy data blocks
        err = ocrDbRelease(gd_gDone3); IFEB;
        err = ocrDbRelease(gd_nekCGscalars); IFEB;
        err = ocrDbRelease(gd_CGtimes); IFEB;
        err = ocrDbDestroy( IN_derefs_E20_22_dep_CGtimes.guid ); IFEB;
        err = ocrDbDestroy( IN_derefs_E20_22_dep_nekCGscalars.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E20_22_dep_TailRecurIterate.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E20_22_dep_reducPrivate.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E20_22_dep_nek_dxm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E20_22_dep_nekR.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E20_22_dep_NEKOstatics.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E20_22_dep_nekP.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E20_22_dep_nekW.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E20_22_dep_NEKOtools.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E20_22_dep_nekZ.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E20_22_dep_nekX.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E20_22_dep_nek_G6.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E20_22_dep_nek_G4.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E20_22_dep_NEKOglobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E20_22_dep_SPMDGlobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E20_22_dep_nek_G1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E20_22_dep_nek_dxTm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E20_22_dep_nekC.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitEND, 0, DB_MODE_RO, IN_derefs_E20_22_dep_TailRecurIterate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitEND, 1, DB_MODE_RO, IN_derefs_E20_22_dep_SPMDGlobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_solveMi, 0, DB_MODE_RO, IN_derefs_E20_22_dep_nek_dxTm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_solveMi, 1, DB_MODE_RW, IN_derefs_E20_22_dep_reducPrivate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_solveMi, 2, DB_MODE_RO, IN_derefs_E20_22_dep_nek_dxm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_solveMi, 3, DB_MODE_RO, IN_derefs_E20_22_dep_nekR.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_solveMi, 4, DB_MODE_RO, IN_derefs_E20_22_dep_nekP.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_solveMi, 5, DB_MODE_RO, IN_derefs_E20_22_dep_nekW.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_solveMi, 6, DB_MODE_RO, IN_derefs_E20_22_dep_NEKOtools.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_solveMi, 7, DB_MODE_RO, IN_derefs_E20_22_dep_nekZ.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_solveMi, 8, DB_MODE_RO, IN_derefs_E20_22_dep_nekX.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_solveMi, 9, DB_MODE_RO, IN_derefs_E20_22_dep_nek_G6.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_solveMi, 10, DB_MODE_RO, IN_derefs_E20_22_dep_nek_G4.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_solveMi, 11, DB_MODE_RO, gd_gDone3); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_solveMi, 12, DB_MODE_RO, IN_derefs_E20_22_dep_NEKOstatics.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_solveMi, 13, DB_MODE_RO, IN_derefs_E20_22_dep_nek_G1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_solveMi, 14, DB_MODE_RO, IN_derefs_E20_22_dep_NEKOglobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_solveMi, 15, DB_MODE_RO, gd_nekCGscalars); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_solveMi, 16, DB_MODE_RO, IN_derefs_E20_22_dep_nekC.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_solveMi, 17, DB_MODE_RO, gd_CGtimes); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t nekCG_solveMi(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 23
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E22_23_dep_NEKOstatics = depv[12];
        NEKOstatics_t * io_NEKOstatics = IN_derefs_E22_23_dep_NEKOstatics.ptr;
        ocrEdtDep_t IN_derefs_E22_23_dep_NEKOglobals = depv[14];
        NEKOglobals_t * io_NEKOglobals = IN_derefs_E22_23_dep_NEKOglobals.ptr;
        ocrHint_t hintEDT, *pHintEDT=0;
        err = ocrXgetEdtHint(NEK_OCR_USE_CURRENT_PD, &hintEDT, &pHintEDT); IFEB;
        ocrEdtDep_t IN_derefs_E22_23_dep_NEKOtools = depv[6];
        NEKOtools_t * io_NEKOtools = IN_derefs_E22_23_dep_NEKOtools.ptr;
        ocrEdtDep_t IN_derefs_E22_23_dep_gDone3 = depv[11];
        ocrGuid_t * in_gDone3 = IN_derefs_E22_23_dep_gDone3.ptr;
        ocrEdtDep_t IN_derefs_E22_23_dep_nekCGscalars = depv[15];
        NEKO_CGscalars_t * io_nekCGscalars = IN_derefs_E22_23_dep_nekCGscalars.ptr;
        ocrEdtDep_t IN_derefs_E22_23_dep_CGtimes = depv[17];
        NEKO_CGtimings_t * in_CGtimes = IN_derefs_E22_23_dep_CGtimes.ptr;
        ocrEdtDep_t IN_derefs_E22_23_dep_nek_G1 = depv[13];
        NBN_REAL * io_G1 = IN_derefs_E22_23_dep_nek_G1.ptr;
        ocrEdtDep_t IN_derefs_E22_23_dep_nek_G4 = depv[10];
        NBN_REAL * io_G4 = IN_derefs_E22_23_dep_nek_G4.ptr;
        ocrEdtDep_t IN_derefs_E22_23_dep_nek_G6 = depv[9];
        NBN_REAL * io_G6 = IN_derefs_E22_23_dep_nek_G6.ptr;
        ocrEdtDep_t IN_derefs_E22_23_dep_nek_dxm1 = depv[2];
        BLAS_REAL_TYPE * io_dxm1 = IN_derefs_E22_23_dep_nek_dxm1.ptr;
        ocrEdtDep_t IN_derefs_E22_23_dep_nek_dxTm1 = depv[0];
        BLAS_REAL_TYPE * io_dxTm1 = IN_derefs_E22_23_dep_nek_dxTm1.ptr;
        ocrEdtDep_t IN_derefs_E22_23_dep_nekC = depv[16];
        NBN_REAL * io_nekC = IN_derefs_E22_23_dep_nekC.ptr;
        ocrEdtDep_t IN_derefs_E22_23_dep_nekP = depv[4];
        NBN_REAL * io_nekP = IN_derefs_E22_23_dep_nekP.ptr;
        ocrEdtDep_t IN_derefs_E22_23_dep_nekR = depv[3];
        NBN_REAL * io_nekR = IN_derefs_E22_23_dep_nekR.ptr;
        ocrEdtDep_t IN_derefs_E22_23_dep_nekW = depv[5];
        NBN_REAL * io_nekW = IN_derefs_E22_23_dep_nekW.ptr;
        ocrEdtDep_t IN_derefs_E22_23_dep_nekX = depv[8];
        NBN_REAL * io_nekX = IN_derefs_E22_23_dep_nekX.ptr;
        ocrEdtDep_t IN_derefs_E22_23_dep_nekZ = depv[7];
        NBN_REAL * in_nekZ = IN_derefs_E22_23_dep_nekZ.ptr;
        ocrEdtDep_t IN_derefs_E22_23_dep_reducPrivate = depv[1];
        reductionPrivate_t * io_reducPrivate = IN_derefs_E22_23_dep_reducPrivate.ptr;

        //----- Create_dataBlocks
        const u64 OA_Count_CGtimes = 1;
        const u64 OA_Count_nekZ = (io_NEKOglobals->pDOF3DperR+1)*sizeof(NBN_REAL);

        ocrGuid_t gd_CGtimes= NULL_GUID;
        NEKO_CGtimings_t * o_CGtimes=NULL;
        err = ocrDbCreate( &gd_CGtimes, (void**)&o_CGtimes, 1*sizeof(NEKO_CGtimings_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nekZ= NULL_GUID;
        NBN_REAL * o_nekZ=NULL;
        err = ocrDbCreate( &gd_nekZ, (void**)&o_nekZ, (io_NEKOglobals->pDOF3DperR+1)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs
        ocrGuid_t ga_nekCG_beta_start = NULL_GUID;
        err = ocrEdtXCreate(nekCG_beta_start, 0, NULL, 18, NULL, EDT_PROP_NONE, pHintEDT, &ga_nekCG_beta_start, NULL); IFEB;

        //----- User section
        err = nekbone_solveMi(io_NEKOstatics, io_NEKOglobals, io_nekR, o_nekZ, in_CGtimes, o_CGtimes); IFEB;

        //----- Release or destroy data blocks
        err = ocrDbRelease(gd_CGtimes); IFEB;
        err = ocrDbRelease(gd_nekZ); IFEB;
        err = ocrDbDestroy( IN_derefs_E22_23_dep_nekZ.guid ); IFEB;
        err = ocrDbDestroy( IN_derefs_E22_23_dep_CGtimes.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E22_23_dep_nek_dxTm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E22_23_dep_reducPrivate.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E22_23_dep_nek_dxm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E22_23_dep_nekR.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E22_23_dep_nekP.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E22_23_dep_nekW.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E22_23_dep_NEKOtools.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E22_23_dep_nekX.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E22_23_dep_nek_G6.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E22_23_dep_nek_G4.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E22_23_dep_gDone3.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E22_23_dep_NEKOstatics.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E22_23_dep_nek_G1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E22_23_dep_NEKOglobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E22_23_dep_nekCGscalars.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E22_23_dep_nekC.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_start, 0, DB_MODE_RO, IN_derefs_E22_23_dep_nek_dxTm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_start, 1, DB_MODE_RW, IN_derefs_E22_23_dep_reducPrivate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_start, 2, DB_MODE_RO, IN_derefs_E22_23_dep_nek_dxm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_start, 3, DB_MODE_RO, IN_derefs_E22_23_dep_nekR.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_start, 4, DB_MODE_RO, IN_derefs_E22_23_dep_nekP.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_start, 5, DB_MODE_RO, IN_derefs_E22_23_dep_nekW.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_start, 6, DB_MODE_RO, IN_derefs_E22_23_dep_NEKOtools.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_start, 7, DB_MODE_RO, gd_nekZ); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_start, 8, DB_MODE_RO, IN_derefs_E22_23_dep_nekX.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_start, 9, DB_MODE_RO, IN_derefs_E22_23_dep_nek_G6.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_start, 10, DB_MODE_RO, IN_derefs_E22_23_dep_nek_G4.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_start, 11, DB_MODE_RO, IN_derefs_E22_23_dep_gDone3.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_start, 12, DB_MODE_RO, IN_derefs_E22_23_dep_NEKOstatics.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_start, 13, DB_MODE_RO, IN_derefs_E22_23_dep_nek_G1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_start, 14, DB_MODE_RO, IN_derefs_E22_23_dep_NEKOglobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_start, 15, DB_MODE_RO, IN_derefs_E22_23_dep_nekCGscalars.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_start, 16, DB_MODE_RO, IN_derefs_E22_23_dep_nekC.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_start, 17, DB_MODE_RO, gd_CGtimes); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t nekCG_beta_start(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 24
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E23_24_dep_NEKOstatics = depv[12];
        NEKOstatics_t * io_NEKOstatics = IN_derefs_E23_24_dep_NEKOstatics.ptr;
        ocrEdtDep_t IN_derefs_E23_24_dep_NEKOglobals = depv[14];
        NEKOglobals_t * io_NEKOglobals = IN_derefs_E23_24_dep_NEKOglobals.ptr;
            ocrHint_t hintEDT, *pHintEDT=0;
            err = ocrXgetEdtHint(NEK_OCR_USE_CURRENT_PD, &hintEDT, &pHintEDT); IFEB;
        ocrEdtDep_t IN_derefs_E23_24_dep_NEKOtools = depv[6];
        NEKOtools_t * io_NEKOtools = IN_derefs_E23_24_dep_NEKOtools.ptr;
        ocrEdtDep_t IN_derefs_E23_24_dep_gDone3 = depv[11];
        ocrEdtDep_t IN_derefs_E23_24_dep_nekCGscalars = depv[15];
        NEKO_CGscalars_t * in_nekCGscalars = IN_derefs_E23_24_dep_nekCGscalars.ptr;
        ocrEdtDep_t IN_derefs_E23_24_dep_CGtimes = depv[17];
        NEKO_CGtimings_t * in_CGtimes = IN_derefs_E23_24_dep_CGtimes.ptr;
        ocrEdtDep_t IN_derefs_E23_24_dep_nek_G1 = depv[13];
        ocrEdtDep_t IN_derefs_E23_24_dep_nek_G4 = depv[10];
        ocrEdtDep_t IN_derefs_E23_24_dep_nek_G6 = depv[9];
        ocrEdtDep_t IN_derefs_E23_24_dep_nek_dxm1 = depv[2];
        ocrEdtDep_t IN_derefs_E23_24_dep_nek_dxTm1 = depv[0];
        ocrEdtDep_t IN_derefs_E23_24_dep_nekC = depv[16];
        NBN_REAL * io_nekC = IN_derefs_E23_24_dep_nekC.ptr;
        ocrEdtDep_t IN_derefs_E23_24_dep_nekP = depv[4];
        ocrEdtDep_t IN_derefs_E23_24_dep_nekR = depv[3];
        NBN_REAL * io_nekR = IN_derefs_E23_24_dep_nekR.ptr;
        ocrEdtDep_t IN_derefs_E23_24_dep_nekW = depv[5];
        ocrEdtDep_t IN_derefs_E23_24_dep_nekX = depv[8];
        ocrEdtDep_t IN_derefs_E23_24_dep_nekZ = depv[7];
        NBN_REAL * io_nekZ = IN_derefs_E23_24_dep_nekZ.ptr;
        ocrEdtDep_t IN_derefs_E23_24_dep_reducPrivate = depv[1];
        reductionPrivate_t * io_reducPrivate = IN_derefs_E23_24_dep_reducPrivate.ptr;
        ocrGuid_t guid_reducPrivate = IN_derefs_E23_24_dep_reducPrivate.guid;
            //DBK reducPrivate will be released by reductionLaunch() in nekbone_beta_start().

        //----- Create_dataBlocks
        const u64 OA_Count_nekCGscalars = 1;
        const u64 OA_Count_CGtimes = 1;

        ocrGuid_t gd_nekCGscalars= NULL_GUID;
        NEKO_CGscalars_t * o_nekCGscalars=NULL;
        err = ocrDbCreate( &gd_nekCGscalars, (void**)&o_nekCGscalars, 1*sizeof(NEKO_CGscalars_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_CGtimes= NULL_GUID;
        NEKO_CGtimings_t * o_CGtimes=NULL;
        err = ocrDbCreate( &gd_CGtimes, (void**)&o_CGtimes, 1*sizeof(NEKO_CGtimings_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs
        ocrGuid_t ga_nekCG_beta_stop = NULL_GUID;
        err = ocrEdtXCreate(nekCG_beta_stop, 0, NULL, 19, NULL, EDT_PROP_NONE, pHintEDT, &ga_nekCG_beta_stop, NULL); IFEB;

        //----- User section
        err = nekbone_beta_start(io_NEKOstatics, io_NEKOglobals, in_nekCGscalars, o_nekCGscalars, io_nekR, io_nekC, io_nekZ, in_CGtimes,o_CGtimes, guid_reducPrivate, io_reducPrivate, REDUC_SLOT_4Beta-1, ga_nekCG_beta_stop); IFEB;

        //----- Release or destroy data blocks
        err = ocrDbRelease(gd_nekCGscalars); IFEB;
        err = ocrDbRelease(gd_CGtimes); IFEB;
        err = ocrDbDestroy( IN_derefs_E23_24_dep_nekCGscalars.guid ); IFEB;
        err = ocrDbDestroy( IN_derefs_E23_24_dep_CGtimes.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E23_24_dep_nek_dxTm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E23_24_dep_nek_dxm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E23_24_dep_nekR.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E23_24_dep_nekP.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E23_24_dep_nekW.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E23_24_dep_NEKOtools.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E23_24_dep_nekZ.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E23_24_dep_nekX.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E23_24_dep_nek_G6.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E23_24_dep_nek_G4.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E23_24_dep_gDone3.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E23_24_dep_NEKOstatics.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E23_24_dep_nek_G1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E23_24_dep_NEKOglobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E23_24_dep_nekC.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_stop, 0, DB_MODE_RO, IN_derefs_E23_24_dep_nek_dxTm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_stop, 1, DB_MODE_RW, IN_derefs_E23_24_dep_reducPrivate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_stop, 2, DB_MODE_RO, IN_derefs_E23_24_dep_nek_dxm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_stop, 3, DB_MODE_RO, IN_derefs_E23_24_dep_nekR.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_stop, 4, DB_MODE_RO, IN_derefs_E23_24_dep_nekP.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_stop, 5, DB_MODE_RO, IN_derefs_E23_24_dep_nekW.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_stop, 6, DB_MODE_RO, IN_derefs_E23_24_dep_NEKOtools.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_stop, 7, DB_MODE_RO, IN_derefs_E23_24_dep_nekZ.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_stop, 8, DB_MODE_RO, IN_derefs_E23_24_dep_nekX.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_stop, 9, DB_MODE_RO, IN_derefs_E23_24_dep_nek_G6.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_stop, 10, DB_MODE_RO, IN_derefs_E23_24_dep_nek_G4.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_stop, 11, DB_MODE_RO, IN_derefs_E23_24_dep_gDone3.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_stop, 12, DB_MODE_RO, IN_derefs_E23_24_dep_NEKOstatics.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_stop, 13, DB_MODE_RO, IN_derefs_E23_24_dep_nek_G1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_stop, 14, DB_MODE_RO, IN_derefs_E23_24_dep_NEKOglobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_stop, 15, DB_MODE_RO, gd_nekCGscalars); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_stop, 16, DB_MODE_RO, IN_derefs_E23_24_dep_nekC.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_beta_stop, 17, DB_MODE_RO, gd_CGtimes); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t nekCG_beta_stop(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 25
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E24_25_dep_NEKOstatics = depv[12];
        NEKOstatics_t * io_NEKOstatics = IN_derefs_E24_25_dep_NEKOstatics.ptr;
        ocrEdtDep_t IN_derefs_E24_25_dep_NEKOglobals = depv[14];
        NEKOglobals_t * io_NEKOglobals = IN_derefs_E24_25_dep_NEKOglobals.ptr;
            ocrHint_t hintEDT, *pHintEDT=0;
            err = ocrXgetEdtHint(NEK_OCR_USE_CURRENT_PD, &hintEDT, &pHintEDT); IFEB;
        ocrEdtDep_t IN_derefs_E24_25_dep_NEKOtools = depv[6];
        NEKOtools_t * io_NEKOtools = IN_derefs_E24_25_dep_NEKOtools.ptr;
        ocrEdtDep_t IN_derefs_E24_25_dep_gDone3 = depv[11];
        ocrGuid_t * in_gDone3 = IN_derefs_E24_25_dep_gDone3.ptr;
        ocrEdtDep_t IN_derefs_E24_25_dep_nekCGscalars = depv[15];
        NEKO_CGscalars_t * in_nekCGscalars = IN_derefs_E24_25_dep_nekCGscalars.ptr;
        ocrEdtDep_t IN_derefs_E24_25_dep_CGtimes = depv[17];
        NEKO_CGtimings_t * in_CGtimes = IN_derefs_E24_25_dep_CGtimes.ptr;
        ocrEdtDep_t IN_derefs_E24_25_dep_nek_G1 = depv[13];
        ocrEdtDep_t IN_derefs_E24_25_dep_nek_G4 = depv[10];
        ocrEdtDep_t IN_derefs_E24_25_dep_nek_G6 = depv[9];
        ocrEdtDep_t IN_derefs_E24_25_dep_nek_dxm1 = depv[2];
        ocrEdtDep_t IN_derefs_E24_25_dep_nek_dxTm1 = depv[0];
        ocrEdtDep_t IN_derefs_E24_25_dep_nekC = depv[16];
        ocrEdtDep_t IN_derefs_E24_25_dep_nekP = depv[4];
        NBN_REAL * in_nekP = IN_derefs_E24_25_dep_nekP.ptr;
        ocrEdtDep_t IN_derefs_E24_25_dep_nekR = depv[3];
        ocrEdtDep_t IN_derefs_E24_25_dep_nekW = depv[5];
        ocrEdtDep_t IN_derefs_E24_25_dep_nekX = depv[8];
        ocrEdtDep_t IN_derefs_E24_25_dep_nekZ = depv[7];
        NBN_REAL * io_nekZ = IN_derefs_E24_25_dep_nekZ.ptr;
        ocrEdtDep_t IN_derefs_E24_25_dep_reducPrivate = depv[1];
            ocrEdtDep_t IN_BY_USER_depv_sum = depv[REDUC_SLOT_4Beta-1];  //From nekCG_beta_start().
            ocrGuid_t in_sum_guid = IN_BY_USER_depv_sum.guid;
            ReducSum_t * in_sum = IN_BY_USER_depv_sum.ptr;

        //----- Create_dataBlocks
        const u64 OA_Count_nekCGscalars = 1;
        const u64 OA_Count_CGtimes = 1;
        const u64 OA_Count_nekP = (io_NEKOglobals->pDOF3DperR+1)*sizeof(NBN_REAL);

        ocrGuid_t gd_nekCGscalars= NULL_GUID;
        NEKO_CGscalars_t * o_nekCGscalars=NULL;
        err = ocrDbCreate( &gd_nekCGscalars, (void**)&o_nekCGscalars, 1*sizeof(NEKO_CGscalars_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_CGtimes= NULL_GUID;
        NEKO_CGtimings_t * o_CGtimes=NULL;
        err = ocrDbCreate( &gd_CGtimes, (void**)&o_CGtimes, 1*sizeof(NEKO_CGtimings_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nekP= NULL_GUID;
        NBN_REAL * o_nekP=NULL;
        err = ocrDbCreate( &gd_nekP, (void**)&o_nekP, (io_NEKOglobals->pDOF3DperR+1)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs
        ocrGuid_t ga_nekCG_axi_start = NULL_GUID;
        err = ocrEdtXCreate(nekCG_axi_start, 0, NULL, 18, NULL, EDT_PROP_NONE, pHintEDT, &ga_nekCG_axi_start, NULL); IFEB;

        //----- User section
        err = nekbone_beta_stop(io_NEKOstatics, io_NEKOglobals, in_nekCGscalars, o_nekCGscalars, in_nekP, io_nekZ, o_nekP, in_CGtimes,o_CGtimes, in_sum_guid, in_sum); IFEB;

        //----- Release or destroy data blocks
        err = ocrDbRelease(gd_nekCGscalars); IFEB;
        err = ocrDbRelease(gd_CGtimes); IFEB;
        err = ocrDbRelease(gd_nekP); IFEB;
        err = ocrDbDestroy( IN_derefs_E24_25_dep_nekP.guid ); IFEB;
        err = ocrDbDestroy( IN_derefs_E24_25_dep_nekCGscalars.guid ); IFEB;
        err = ocrDbDestroy( IN_derefs_E24_25_dep_CGtimes.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E24_25_dep_nek_dxTm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E24_25_dep_reducPrivate.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E24_25_dep_nek_dxm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E24_25_dep_nekR.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E24_25_dep_nekW.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E24_25_dep_NEKOtools.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E24_25_dep_nekZ.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E24_25_dep_nekX.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E24_25_dep_nek_G6.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E24_25_dep_nek_G4.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E24_25_dep_gDone3.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E24_25_dep_NEKOstatics.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E24_25_dep_nek_G1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E24_25_dep_NEKOglobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E24_25_dep_nekC.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_start, 0, DB_MODE_RO, IN_derefs_E24_25_dep_nek_dxTm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_start, 1, DB_MODE_RW, IN_derefs_E24_25_dep_reducPrivate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_start, 2, DB_MODE_RO, IN_derefs_E24_25_dep_nek_dxm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_start, 3, DB_MODE_RO, IN_derefs_E24_25_dep_nekR.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_start, 4, DB_MODE_RO, gd_nekP); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_start, 5, DB_MODE_RO, IN_derefs_E24_25_dep_nekW.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_start, 6, DB_MODE_RO, IN_derefs_E24_25_dep_NEKOtools.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_start, 7, DB_MODE_RO, IN_derefs_E24_25_dep_nekZ.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_start, 8, DB_MODE_RO, IN_derefs_E24_25_dep_nekX.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_start, 9, DB_MODE_RO, IN_derefs_E24_25_dep_nek_G6.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_start, 10, DB_MODE_RO, IN_derefs_E24_25_dep_nek_G4.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_start, 11, DB_MODE_RO, IN_derefs_E24_25_dep_gDone3.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_start, 12, DB_MODE_RO, IN_derefs_E24_25_dep_NEKOstatics.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_start, 13, DB_MODE_RO, IN_derefs_E24_25_dep_nek_G1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_start, 14, DB_MODE_RO, IN_derefs_E24_25_dep_NEKOglobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_start, 15, DB_MODE_RO, gd_nekCGscalars); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_start, 16, DB_MODE_RO, IN_derefs_E24_25_dep_nekC.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_start, 17, DB_MODE_RO, gd_CGtimes); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t nekCG_axi_start(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 26
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E25_26_dep_NEKOstatics = depv[12];
        NEKOstatics_t * io_NEKOstatics = IN_derefs_E25_26_dep_NEKOstatics.ptr;
        ocrEdtDep_t IN_derefs_E25_26_dep_NEKOglobals = depv[14];
        NEKOglobals_t * io_NEKOglobals = IN_derefs_E25_26_dep_NEKOglobals.ptr;
            ocrHint_t hintEDT, *pHintEDT=0;
            err = ocrXgetEdtHint(NEK_OCR_USE_CURRENT_PD, &hintEDT, &pHintEDT); IFEB;
            Triplet Rlattice = {io_NEKOstatics->Rx, io_NEKOstatics->Ry, io_NEKOstatics->Rz};
            Triplet Elattice = {io_NEKOstatics->Ex, io_NEKOstatics->Ey, io_NEKOstatics->Ez};
            const Idz length_riValues4axi = calculate_length_rankIndexedValue(io_NEKOglobals->pDOF, Elattice);
            Idz sz_nekW = io_NEKOglobals->pDOF3DperR+1;
        ocrEdtDep_t IN_derefs_E25_26_dep_NEKOtools = depv[6];
        NEKOtools_t * io_NEKOtools = IN_derefs_E25_26_dep_NEKOtools.ptr;
        ocrEdtDep_t IN_derefs_E25_26_dep_gDone3 = depv[11];
        ocrEdtDep_t IN_derefs_E25_26_dep_nekCGscalars = depv[15];
        ocrEdtDep_t IN_derefs_E25_26_dep_CGtimes = depv[17];
        NEKO_CGtimings_t * in_CGtimes = IN_derefs_E25_26_dep_CGtimes.ptr;
        ocrEdtDep_t IN_derefs_E25_26_dep_nek_G1 = depv[13];
        NBN_REAL * io_G1 = IN_derefs_E25_26_dep_nek_G1.ptr;
        ocrEdtDep_t IN_derefs_E25_26_dep_nek_G4 = depv[10];
        NBN_REAL * io_G4 = IN_derefs_E25_26_dep_nek_G4.ptr;
        ocrEdtDep_t IN_derefs_E25_26_dep_nek_G6 = depv[9];
        NBN_REAL * io_G6 = IN_derefs_E25_26_dep_nek_G6.ptr;
        ocrEdtDep_t IN_derefs_E25_26_dep_nek_dxm1 = depv[2];
        BLAS_REAL_TYPE * io_dxm1 = IN_derefs_E25_26_dep_nek_dxm1.ptr;
        ocrEdtDep_t IN_derefs_E25_26_dep_nek_dxTm1 = depv[0];
        BLAS_REAL_TYPE * io_dxTm1 = IN_derefs_E25_26_dep_nek_dxTm1.ptr;
        ocrEdtDep_t IN_derefs_E25_26_dep_nekC = depv[16];
        ocrEdtDep_t IN_derefs_E25_26_dep_nekP = depv[4];
        NBN_REAL * io_nekP = IN_derefs_E25_26_dep_nekP.ptr;
        ocrEdtDep_t IN_derefs_E25_26_dep_nekR = depv[3];
        ocrEdtDep_t IN_derefs_E25_26_dep_nekW = depv[5];
        NBN_REAL * io_nekW = IN_derefs_E25_26_dep_nekW.ptr;
        ocrEdtDep_t IN_derefs_E25_26_dep_nekX = depv[8];
        ocrEdtDep_t IN_derefs_E25_26_dep_nekZ = depv[7];
        ocrEdtDep_t IN_derefs_E25_26_dep_reducPrivate = depv[1];
        reductionPrivate_t * io_reducPrivate = IN_derefs_E25_26_dep_reducPrivate.ptr;

        //----- Create_dataBlocks
        const u64 OA_Count_CGtimes = 1;
        const u64 OA_Count_nekUR = (io_NEKOstatics->pDOFmax3D+1)*sizeof(NBN_REAL);
        const u64 OA_Count_nekUS = (io_NEKOstatics->pDOFmax3D+1)*sizeof(NBN_REAL);
        const u64 OA_Count_nekUT = (io_NEKOstatics->pDOFmax3D+1)*sizeof(NBN_REAL);
        const u64 OA_Count_AItemp = (io_NEKOstatics->pDOFmax3D+1)*sizeof(NBN_REAL);
        const u64 OA_Count_riValues4axi = length_riValues4axi * sizeof(rankIndexedValue_t);

        ocrGuid_t gd_CGtimes= NULL_GUID;
        NEKO_CGtimings_t * o_CGtimes=NULL;
        err = ocrDbCreate( &gd_CGtimes, (void**)&o_CGtimes, 1*sizeof(NEKO_CGtimings_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nekUR= NULL_GUID;
        NBN_REAL * nekUR=NULL;
        err = ocrDbCreate( &gd_nekUR, (void**)&nekUR, (io_NEKOstatics->pDOFmax3D+1)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nekUS= NULL_GUID;
        NBN_REAL * nekUS=NULL;
        err = ocrDbCreate( &gd_nekUS, (void**)&nekUS, (io_NEKOstatics->pDOFmax3D+1)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nekUT= NULL_GUID;
        NBN_REAL * nekUT=NULL;
        err = ocrDbCreate( &gd_nekUT, (void**)&nekUT, (io_NEKOstatics->pDOFmax3D+1)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_AItemp= NULL_GUID;
        NBN_REAL * AItemp=NULL;
        err = ocrDbCreate( &gd_AItemp, (void**)&AItemp, (io_NEKOstatics->pDOFmax3D+1)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_riValues4axi= NULL_GUID;
        rankIndexedValue_t * riValues4axi=NULL;
        err = ocrDbCreate( &gd_riValues4axi, (void**)&riValues4axi, length_riValues4axi * sizeof(rankIndexedValue_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs
        ocrGuid_t ga_nekCG_axi_stop = NULL_GUID;
        err = ocrEdtXCreate(nekCG_axi_stop, 0, NULL, SLOTCNT_total_channels4ax, NULL, EDT_PROP_NONE, pHintEDT, &ga_nekCG_axi_stop, NULL); IFEB;

        //----- User section
        err = nekbone_ai_start(io_NEKOstatics, io_NEKOglobals, io_nekW,io_nekP, nekUR,nekUS,nekUT, io_G1,io_G4,io_G6,io_dxm1,io_dxTm1, AItemp, in_CGtimes,o_CGtimes); IFEB;
        err = start_halo_ai(OA_DEBUG_OUTVARS, io_NEKOtools, Rlattice,Elattice, io_NEKOglobals, sz_nekW, io_nekW, riValues4axi, &ga_nekCG_axi_stop); IFEB;

        //----- Release or destroy data blocks
        err = ocrDbRelease(gd_CGtimes); IFEB;
        err = ocrDbDestroy(gd_nekUR); IFEB;
        err = ocrDbDestroy(gd_nekUS); IFEB;
        err = ocrDbDestroy(gd_nekUT); IFEB;
        err = ocrDbDestroy(gd_AItemp); IFEB;
        err = ocrDbDestroy(gd_riValues4axi); IFEB;
        err = ocrDbDestroy( IN_derefs_E25_26_dep_CGtimes.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E25_26_dep_nek_dxTm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E25_26_dep_reducPrivate.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E25_26_dep_nek_dxm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E25_26_dep_nekR.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E25_26_dep_nekP.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E25_26_dep_nekW.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E25_26_dep_NEKOtools.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E25_26_dep_nekZ.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E25_26_dep_nekX.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E25_26_dep_nek_G6.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E25_26_dep_nek_G4.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E25_26_dep_gDone3.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E25_26_dep_NEKOstatics.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E25_26_dep_nek_G1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E25_26_dep_NEKOglobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E25_26_dep_nekCGscalars.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E25_26_dep_nekC.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_stop, 0, DB_MODE_RO, IN_derefs_E25_26_dep_nek_dxTm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_stop, 1, DB_MODE_RW, IN_derefs_E25_26_dep_reducPrivate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_stop, 2, DB_MODE_RO, IN_derefs_E25_26_dep_nek_dxm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_stop, 3, DB_MODE_RO, IN_derefs_E25_26_dep_nekR.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_stop, 4, DB_MODE_RO, IN_derefs_E25_26_dep_nekP.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_stop, 5, DB_MODE_RW, IN_derefs_E25_26_dep_nekW.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_stop, 6, DB_MODE_RO, IN_derefs_E25_26_dep_NEKOtools.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_stop, 7, DB_MODE_RO, IN_derefs_E25_26_dep_nekZ.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_stop, 8, DB_MODE_RO, IN_derefs_E25_26_dep_nekX.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_stop, 9, DB_MODE_RO, IN_derefs_E25_26_dep_nek_G6.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_stop, 10, DB_MODE_RO, IN_derefs_E25_26_dep_nek_G4.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_stop, 11, DB_MODE_RO, IN_derefs_E25_26_dep_gDone3.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_stop, 12, DB_MODE_RO, IN_derefs_E25_26_dep_NEKOstatics.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_stop, 13, DB_MODE_RO, IN_derefs_E25_26_dep_nek_G1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_stop, 14, DB_MODE_RO, IN_derefs_E25_26_dep_NEKOglobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_stop, 15, DB_MODE_RO, IN_derefs_E25_26_dep_nekCGscalars.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_stop, 16, DB_MODE_RO, IN_derefs_E25_26_dep_nekC.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_axi_stop, 17, DB_MODE_RO, gd_CGtimes); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t nekCG_axi_stop(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 27
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E26_27_dep_NEKOstatics = depv[12];
        NEKOstatics_t * io_NEKOstatics = IN_derefs_E26_27_dep_NEKOstatics.ptr;
        ocrEdtDep_t IN_derefs_E26_27_dep_NEKOglobals = depv[14];
        NEKOglobals_t * io_NEKOglobals = IN_derefs_E26_27_dep_NEKOglobals.ptr;
            ocrHint_t hintEDT, *pHintEDT=0;
            err = ocrXgetEdtHint(NEK_OCR_USE_CURRENT_PD, &hintEDT, &pHintEDT); IFEB;
        ocrEdtDep_t IN_derefs_E26_27_dep_NEKOtools = depv[6];
        NEKOtools_t * io_NEKOtools = IN_derefs_E26_27_dep_NEKOtools.ptr;
        ocrEdtDep_t IN_derefs_E26_27_dep_gDone3 = depv[11];
        ocrEdtDep_t IN_derefs_E26_27_dep_nekCGscalars = depv[15];
        ocrEdtDep_t IN_derefs_E26_27_dep_CGtimes = depv[17];
        NEKO_CGtimings_t * in_CGtimes = IN_derefs_E26_27_dep_CGtimes.ptr;
        ocrEdtDep_t IN_derefs_E26_27_dep_nek_G1 = depv[13];
        ocrEdtDep_t IN_derefs_E26_27_dep_nek_G4 = depv[10];
        ocrEdtDep_t IN_derefs_E26_27_dep_nek_G6 = depv[9];
        ocrEdtDep_t IN_derefs_E26_27_dep_nek_dxm1 = depv[2];
        ocrEdtDep_t IN_derefs_E26_27_dep_nek_dxTm1 = depv[0];
        ocrEdtDep_t IN_derefs_E26_27_dep_nekC = depv[16];
        ocrEdtDep_t IN_derefs_E26_27_dep_nekP = depv[4];
        NBN_REAL * io_nekP = IN_derefs_E26_27_dep_nekP.ptr;
        ocrEdtDep_t IN_derefs_E26_27_dep_nekR = depv[3];
        ocrEdtDep_t IN_derefs_E26_27_dep_nekW = depv[5];
        NBN_REAL * io_nekW = IN_derefs_E26_27_dep_nekW.ptr;
        ocrEdtDep_t IN_derefs_E26_27_dep_nekX = depv[8];
        ocrEdtDep_t IN_derefs_E26_27_dep_nekZ = depv[7];
        ocrEdtDep_t IN_derefs_E26_27_dep_reducPrivate = depv[1];
        reductionPrivate_t * io_reducPrivate = IN_derefs_E26_27_dep_reducPrivate.ptr;

        //----- Create_dataBlocks
        const u64 OA_Count_CGtimes = 1;

        ocrGuid_t gd_CGtimes= NULL_GUID;
        NEKO_CGtimings_t * o_CGtimes=NULL;
        err = ocrDbCreate( &gd_CGtimes, (void**)&o_CGtimes, 1*sizeof(NEKO_CGtimings_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs
        ocrGuid_t ga_nekCG_alpha_start = NULL_GUID;
        err = ocrEdtXCreate(nekCG_alpha_start, 0, NULL, 18, NULL, EDT_PROP_NONE, pHintEDT, &ga_nekCG_alpha_start, NULL); IFEB;

        //----- User section
        err = stop_halo_ai(OA_DEBUG_OUTVARS, io_NEKOtools, depv, io_nekW); IFEB;
        err = nekbone_ai_stop(io_NEKOstatics, io_NEKOglobals, io_nekP, io_nekW, in_CGtimes,o_CGtimes); IFEB;

        //----- Release or destroy data blocks
        err = ocrDbRelease(gd_CGtimes); IFEB;
        err = ocrDbDestroy( IN_derefs_E26_27_dep_CGtimes.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E26_27_dep_nek_dxTm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E26_27_dep_reducPrivate.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E26_27_dep_nek_dxm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E26_27_dep_nekR.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E26_27_dep_nekP.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E26_27_dep_nekW.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E26_27_dep_NEKOtools.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E26_27_dep_nekZ.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E26_27_dep_nekX.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E26_27_dep_nek_G6.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E26_27_dep_nek_G4.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E26_27_dep_gDone3.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E26_27_dep_NEKOstatics.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E26_27_dep_nek_G1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E26_27_dep_NEKOglobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E26_27_dep_nekCGscalars.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E26_27_dep_nekC.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_start, 0, DB_MODE_RO, IN_derefs_E26_27_dep_nek_dxTm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_start, 1, DB_MODE_RW, IN_derefs_E26_27_dep_reducPrivate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_start, 2, DB_MODE_RO, IN_derefs_E26_27_dep_nek_dxm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_start, 3, DB_MODE_RO, IN_derefs_E26_27_dep_nekR.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_start, 4, DB_MODE_RO, IN_derefs_E26_27_dep_nekP.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_start, 5, DB_MODE_RO, IN_derefs_E26_27_dep_nekW.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_start, 6, DB_MODE_RO, IN_derefs_E26_27_dep_NEKOtools.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_start, 7, DB_MODE_RO, IN_derefs_E26_27_dep_nekZ.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_start, 8, DB_MODE_RO, IN_derefs_E26_27_dep_nekX.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_start, 9, DB_MODE_RO, IN_derefs_E26_27_dep_nek_G6.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_start, 10, DB_MODE_RO, IN_derefs_E26_27_dep_nek_G4.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_start, 11, DB_MODE_RO, IN_derefs_E26_27_dep_gDone3.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_start, 12, DB_MODE_RO, IN_derefs_E26_27_dep_NEKOstatics.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_start, 13, DB_MODE_RO, IN_derefs_E26_27_dep_nek_G1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_start, 14, DB_MODE_RO, IN_derefs_E26_27_dep_NEKOglobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_start, 15, DB_MODE_RO, IN_derefs_E26_27_dep_nekCGscalars.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_start, 16, DB_MODE_RO, IN_derefs_E26_27_dep_nekC.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_start, 17, DB_MODE_RO, gd_CGtimes); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t nekCG_alpha_start(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 28
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E27_28_dep_NEKOstatics = depv[12];
        NEKOstatics_t * io_NEKOstatics = IN_derefs_E27_28_dep_NEKOstatics.ptr;
        ocrEdtDep_t IN_derefs_E27_28_dep_NEKOglobals = depv[14];
        NEKOglobals_t * io_NEKOglobals = IN_derefs_E27_28_dep_NEKOglobals.ptr;
            ocrHint_t hintEDT, *pHintEDT=0;
            err = ocrXgetEdtHint(NEK_OCR_USE_CURRENT_PD, &hintEDT, &pHintEDT); IFEB;
        ocrEdtDep_t IN_derefs_E27_28_dep_NEKOtools = depv[6];
        ocrEdtDep_t IN_derefs_E27_28_dep_gDone3 = depv[11];
        ocrEdtDep_t IN_derefs_E27_28_dep_nekCGscalars = depv[15];
        NEKO_CGscalars_t * io_nekCGscalars = IN_derefs_E27_28_dep_nekCGscalars.ptr;
        ocrEdtDep_t IN_derefs_E27_28_dep_CGtimes = depv[17];
        NEKO_CGtimings_t * in_CGtimes = IN_derefs_E27_28_dep_CGtimes.ptr;
        ocrEdtDep_t IN_derefs_E27_28_dep_nek_G1 = depv[13];
        ocrEdtDep_t IN_derefs_E27_28_dep_nek_G4 = depv[10];
        ocrEdtDep_t IN_derefs_E27_28_dep_nek_G6 = depv[9];
        ocrEdtDep_t IN_derefs_E27_28_dep_nek_dxm1 = depv[2];
        ocrEdtDep_t IN_derefs_E27_28_dep_nek_dxTm1 = depv[0];
        ocrEdtDep_t IN_derefs_E27_28_dep_nekC = depv[16];
        NBN_REAL * io_nekC = IN_derefs_E27_28_dep_nekC.ptr;
        ocrEdtDep_t IN_derefs_E27_28_dep_nekP = depv[4];
        NBN_REAL * io_nekP = IN_derefs_E27_28_dep_nekP.ptr;
        ocrEdtDep_t IN_derefs_E27_28_dep_nekR = depv[3];
        ocrEdtDep_t IN_derefs_E27_28_dep_nekW = depv[5];
        NBN_REAL * io_nekW = IN_derefs_E27_28_dep_nekW.ptr;
        ocrEdtDep_t IN_derefs_E27_28_dep_nekX = depv[8];
        ocrEdtDep_t IN_derefs_E27_28_dep_nekZ = depv[7];
        ocrEdtDep_t IN_derefs_E27_28_dep_reducPrivate = depv[1];
        reductionPrivate_t * io_reducPrivate = IN_derefs_E27_28_dep_reducPrivate.ptr;
        ocrGuid_t guid_reducPrivate = IN_derefs_E27_28_dep_reducPrivate.guid;
            //DBK reducPrivate will be released by reductionLaunch() in nekbone_alpha_start().

        //----- Create_dataBlocks
        const u64 OA_Count_CGtimes = 1;

        ocrGuid_t gd_CGtimes= NULL_GUID;
        NEKO_CGtimings_t * o_CGtimes=NULL;
        err = ocrDbCreate( &gd_CGtimes, (void**)&o_CGtimes, 1*sizeof(NEKO_CGtimings_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs
        ocrGuid_t ga_nekCG_alpha_stop = NULL_GUID;
        err = ocrEdtXCreate(nekCG_alpha_stop, 0, NULL, 19, NULL, EDT_PROP_NONE, pHintEDT, &ga_nekCG_alpha_stop, NULL); IFEB;

        //----- User section
        err = nekbone_alpha_start(io_NEKOstatics, io_NEKOglobals, io_nekW, io_nekC, io_nekP, in_CGtimes,o_CGtimes, guid_reducPrivate, io_reducPrivate, REDUC_SLOT_4Alpha-1,ga_nekCG_alpha_stop); IFEB;

        //----- Release or destroy data blocks
        err = ocrDbRelease(gd_CGtimes); IFEB;
        err = ocrDbDestroy( IN_derefs_E27_28_dep_CGtimes.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E27_28_dep_nek_dxTm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E27_28_dep_nek_dxm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E27_28_dep_nekR.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E27_28_dep_nekP.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E27_28_dep_nekW.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E27_28_dep_NEKOtools.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E27_28_dep_nekZ.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E27_28_dep_nekX.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E27_28_dep_nek_G6.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E27_28_dep_nek_G4.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E27_28_dep_gDone3.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E27_28_dep_NEKOstatics.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E27_28_dep_nek_G1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E27_28_dep_NEKOglobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E27_28_dep_nekCGscalars.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E27_28_dep_nekC.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_stop, 0, DB_MODE_RO, IN_derefs_E27_28_dep_nek_dxTm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_stop, 1, DB_MODE_RW, IN_derefs_E27_28_dep_reducPrivate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_stop, 2, DB_MODE_RO, IN_derefs_E27_28_dep_nek_dxm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_stop, 3, DB_MODE_RO, IN_derefs_E27_28_dep_nekR.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_stop, 4, DB_MODE_RO, IN_derefs_E27_28_dep_nekP.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_stop, 5, DB_MODE_RO, IN_derefs_E27_28_dep_nekW.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_stop, 6, DB_MODE_RO, IN_derefs_E27_28_dep_NEKOtools.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_stop, 7, DB_MODE_RO, IN_derefs_E27_28_dep_nekZ.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_stop, 8, DB_MODE_RO, IN_derefs_E27_28_dep_nekX.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_stop, 9, DB_MODE_RO, IN_derefs_E27_28_dep_nek_G6.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_stop, 10, DB_MODE_RO, IN_derefs_E27_28_dep_nek_G4.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_stop, 11, DB_MODE_RO, IN_derefs_E27_28_dep_gDone3.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_stop, 12, DB_MODE_RO, IN_derefs_E27_28_dep_NEKOstatics.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_stop, 13, DB_MODE_RO, IN_derefs_E27_28_dep_nek_G1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_stop, 14, DB_MODE_RO, IN_derefs_E27_28_dep_NEKOglobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_stop, 15, DB_MODE_RO, IN_derefs_E27_28_dep_nekCGscalars.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_stop, 16, DB_MODE_RO, IN_derefs_E27_28_dep_nekC.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_alpha_stop, 17, DB_MODE_RO, gd_CGtimes); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t nekCG_alpha_stop(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 29
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E28_29_dep_NEKOstatics = depv[12];
        NEKOstatics_t * io_NEKOstatics = IN_derefs_E28_29_dep_NEKOstatics.ptr;
        ocrEdtDep_t IN_derefs_E28_29_dep_NEKOglobals = depv[14];
        NEKOglobals_t * io_NEKOglobals = IN_derefs_E28_29_dep_NEKOglobals.ptr;
            ocrHint_t hintEDT, *pHintEDT=0;
            err = ocrXgetEdtHint(NEK_OCR_USE_CURRENT_PD, &hintEDT, &pHintEDT); IFEB;
        ocrEdtDep_t IN_derefs_E28_29_dep_NEKOtools = depv[6];
        ocrEdtDep_t IN_derefs_E28_29_dep_gDone3 = depv[11];
        ocrEdtDep_t IN_derefs_E28_29_dep_nekCGscalars = depv[15];
        NEKO_CGscalars_t * in_nekCGscalars = IN_derefs_E28_29_dep_nekCGscalars.ptr;
        ocrEdtDep_t IN_derefs_E28_29_dep_CGtimes = depv[17];
        NEKO_CGtimings_t * in_CGtimes = IN_derefs_E28_29_dep_CGtimes.ptr;
        ocrEdtDep_t IN_derefs_E28_29_dep_nek_G1 = depv[13];
        ocrEdtDep_t IN_derefs_E28_29_dep_nek_G4 = depv[10];
        ocrEdtDep_t IN_derefs_E28_29_dep_nek_G6 = depv[9];
        ocrEdtDep_t IN_derefs_E28_29_dep_nek_dxm1 = depv[2];
        ocrEdtDep_t IN_derefs_E28_29_dep_nek_dxTm1 = depv[0];
        ocrEdtDep_t IN_derefs_E28_29_dep_nekC = depv[16];
        ocrEdtDep_t IN_derefs_E28_29_dep_nekP = depv[4];
        NBN_REAL * io_nekP = IN_derefs_E28_29_dep_nekP.ptr;
        ocrEdtDep_t IN_derefs_E28_29_dep_nekR = depv[3];
        NBN_REAL * in_nekR = IN_derefs_E28_29_dep_nekR.ptr;
        ocrEdtDep_t IN_derefs_E28_29_dep_nekW = depv[5];
        NBN_REAL * io_nekW = IN_derefs_E28_29_dep_nekW.ptr;
        ocrEdtDep_t IN_derefs_E28_29_dep_nekX = depv[8];
        NBN_REAL * in_nekX = IN_derefs_E28_29_dep_nekX.ptr;
        ocrEdtDep_t IN_derefs_E28_29_dep_nekZ = depv[7];
        ocrEdtDep_t IN_derefs_E28_29_dep_reducPrivate = depv[1];
            ocrEdtDep_t IN_BY_USER_depv_sum = depv[REDUC_SLOT_4Alpha-1];  //From nekCG_alpha_start().
            ocrGuid_t in_sum_guid = IN_BY_USER_depv_sum.guid;
            ReducSum_t * in_sum = IN_BY_USER_depv_sum.ptr;

        //----- Create_dataBlocks
        const u64 OA_Count_nekCGscalars = 1;
        const u64 OA_Count_CGtimes = 1;
        const u64 OA_Count_nekR = (io_NEKOglobals->pDOF3DperR+1)*sizeof(NBN_REAL);
        const u64 OA_Count_nekX = (io_NEKOglobals->pDOF3DperR+1)*sizeof(NBN_REAL);

        ocrGuid_t gd_nekCGscalars= NULL_GUID;
        NEKO_CGscalars_t * o_nekCGscalars=NULL;
        err = ocrDbCreate( &gd_nekCGscalars, (void**)&o_nekCGscalars, 1*sizeof(NEKO_CGscalars_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_CGtimes= NULL_GUID;
        NEKO_CGtimings_t * o_CGtimes=NULL;
        err = ocrDbCreate( &gd_CGtimes, (void**)&o_CGtimes, 1*sizeof(NEKO_CGtimings_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nekR= NULL_GUID;
        NBN_REAL * o_nekR=NULL;
        err = ocrDbCreate( &gd_nekR, (void**)&o_nekR, (io_NEKOglobals->pDOF3DperR+1)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_nekX= NULL_GUID;
        NBN_REAL * o_nekX=NULL;
        err = ocrDbCreate( &gd_nekX, (void**)&o_nekX, (io_NEKOglobals->pDOF3DperR+1)*sizeof(NBN_REAL), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs
        ocrGuid_t ga_nekCG_rtr_start = NULL_GUID;
        err = ocrEdtXCreate(nekCG_rtr_start, 0, NULL, 18, NULL, EDT_PROP_NONE, pHintEDT, &ga_nekCG_rtr_start, NULL); IFEB;

        //----- User section
        err = nekbone_alpha_stop(io_NEKOstatics, io_NEKOglobals, in_nekCGscalars, o_nekCGscalars, in_nekX,io_nekP,o_nekX, in_nekR,io_nekW,o_nekR, in_CGtimes,o_CGtimes, in_sum_guid, in_sum); IFEB;

        //----- Release or destroy data blocks
        err = ocrDbRelease(gd_nekCGscalars); IFEB;
        err = ocrDbRelease(gd_CGtimes); IFEB;
        err = ocrDbRelease(gd_nekR); IFEB;
        err = ocrDbRelease(gd_nekX); IFEB;
        err = ocrDbDestroy( IN_derefs_E28_29_dep_nekR.guid ); IFEB;
        err = ocrDbDestroy( IN_derefs_E28_29_dep_nekX.guid ); IFEB;
        err = ocrDbDestroy( IN_derefs_E28_29_dep_nekCGscalars.guid ); IFEB;
        err = ocrDbDestroy( IN_derefs_E28_29_dep_CGtimes.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E28_29_dep_nek_dxTm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E28_29_dep_reducPrivate.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E28_29_dep_nek_dxm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E28_29_dep_nekP.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E28_29_dep_nekW.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E28_29_dep_NEKOtools.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E28_29_dep_nekZ.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E28_29_dep_nek_G6.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E28_29_dep_nek_G4.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E28_29_dep_gDone3.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E28_29_dep_NEKOstatics.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E28_29_dep_nek_G1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E28_29_dep_NEKOglobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E28_29_dep_nekC.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_start, 0, DB_MODE_RO, IN_derefs_E28_29_dep_nek_dxTm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_start, 1, DB_MODE_RW, IN_derefs_E28_29_dep_reducPrivate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_start, 2, DB_MODE_RO, IN_derefs_E28_29_dep_nek_dxm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_start, 3, DB_MODE_RO, gd_nekR); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_start, 4, DB_MODE_RO, IN_derefs_E28_29_dep_nekP.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_start, 5, DB_MODE_RO, IN_derefs_E28_29_dep_nekW.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_start, 6, DB_MODE_RO, IN_derefs_E28_29_dep_NEKOtools.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_start, 7, DB_MODE_RO, IN_derefs_E28_29_dep_nekZ.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_start, 8, DB_MODE_RO, gd_nekX); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_start, 9, DB_MODE_RO, IN_derefs_E28_29_dep_nek_G6.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_start, 10, DB_MODE_RO, IN_derefs_E28_29_dep_nek_G4.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_start, 11, DB_MODE_RO, IN_derefs_E28_29_dep_gDone3.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_start, 12, DB_MODE_RO, IN_derefs_E28_29_dep_NEKOstatics.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_start, 13, DB_MODE_RO, IN_derefs_E28_29_dep_nek_G1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_start, 14, DB_MODE_RO, IN_derefs_E28_29_dep_NEKOglobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_start, 15, DB_MODE_RO, gd_nekCGscalars); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_start, 16, DB_MODE_RO, IN_derefs_E28_29_dep_nekC.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_start, 17, DB_MODE_RO, gd_CGtimes); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t nekCG_rtr_start(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 30
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E29_30_dep_NEKOstatics = depv[12];
        NEKOstatics_t * io_NEKOstatics = IN_derefs_E29_30_dep_NEKOstatics.ptr;
        ocrEdtDep_t IN_derefs_E29_30_dep_NEKOglobals = depv[14];
        NEKOglobals_t * io_NEKOglobals = IN_derefs_E29_30_dep_NEKOglobals.ptr;
            ocrHint_t hintEDT, *pHintEDT=0;
            err = ocrXgetEdtHint(NEK_OCR_USE_CURRENT_PD, &hintEDT, &pHintEDT); IFEB;
        ocrEdtDep_t IN_derefs_E29_30_dep_NEKOtools = depv[6];
        ocrEdtDep_t IN_derefs_E29_30_dep_gDone3 = depv[11];
        ocrEdtDep_t IN_derefs_E29_30_dep_nekCGscalars = depv[15];
        ocrEdtDep_t IN_derefs_E29_30_dep_CGtimes = depv[17];
        NEKO_CGtimings_t * in_CGtimes = IN_derefs_E29_30_dep_CGtimes.ptr;
        ocrEdtDep_t IN_derefs_E29_30_dep_nek_G1 = depv[13];
        ocrEdtDep_t IN_derefs_E29_30_dep_nek_G4 = depv[10];
        ocrEdtDep_t IN_derefs_E29_30_dep_nek_G6 = depv[9];
        ocrEdtDep_t IN_derefs_E29_30_dep_nek_dxm1 = depv[2];
        ocrEdtDep_t IN_derefs_E29_30_dep_nek_dxTm1 = depv[0];
        ocrEdtDep_t IN_derefs_E29_30_dep_nekC = depv[16];
        NBN_REAL * io_nekC = IN_derefs_E29_30_dep_nekC.ptr;
        ocrEdtDep_t IN_derefs_E29_30_dep_nekP = depv[4];
        ocrEdtDep_t IN_derefs_E29_30_dep_nekR = depv[3];
        NBN_REAL * io_nekR = IN_derefs_E29_30_dep_nekR.ptr;
        ocrEdtDep_t IN_derefs_E29_30_dep_nekW = depv[5];
        ocrEdtDep_t IN_derefs_E29_30_dep_nekX = depv[8];
        ocrEdtDep_t IN_derefs_E29_30_dep_nekZ = depv[7];
        ocrEdtDep_t IN_derefs_E29_30_dep_reducPrivate = depv[1];
        reductionPrivate_t * io_reducPrivate = IN_derefs_E29_30_dep_reducPrivate.ptr;
        ocrGuid_t guid_reducPrivate = IN_derefs_E29_30_dep_reducPrivate.guid;
            //DBK reducPrivate will be released by reductionLaunch() in nekbone_rtr_start().

        //----- Create_dataBlocks
        const u64 OA_Count_CGtimes = 1;

        ocrGuid_t gd_CGtimes= NULL_GUID;
        NEKO_CGtimings_t * o_CGtimes=NULL;
        err = ocrDbCreate( &gd_CGtimes, (void**)&o_CGtimes, 1*sizeof(NEKO_CGtimings_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs
        ocrGuid_t ga_nekCG_rtr_stop = NULL_GUID;
        err = ocrEdtXCreate(nekCG_rtr_stop, 0, NULL, 19, NULL, EDT_PROP_NONE, pHintEDT, &ga_nekCG_rtr_stop, NULL); IFEB;

        //----- User section
        err = nekbone_rtr_start(io_NEKOstatics, io_NEKOglobals, io_nekR, io_nekC, in_CGtimes,o_CGtimes, guid_reducPrivate, io_reducPrivate, REDUC_SLOT_4Rtr-1, ga_nekCG_rtr_stop); IFEB;

        //----- Release or destroy data blocks
        err = ocrDbRelease(gd_CGtimes); IFEB;
        err = ocrDbDestroy( IN_derefs_E29_30_dep_CGtimes.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E29_30_dep_nek_dxTm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E29_30_dep_nek_dxm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E29_30_dep_nekR.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E29_30_dep_nekP.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E29_30_dep_nekW.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E29_30_dep_NEKOtools.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E29_30_dep_nekZ.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E29_30_dep_nekX.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E29_30_dep_nek_G6.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E29_30_dep_nek_G4.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E29_30_dep_gDone3.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E29_30_dep_NEKOstatics.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E29_30_dep_nek_G1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E29_30_dep_NEKOglobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E29_30_dep_nekCGscalars.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E29_30_dep_nekC.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_stop, 0, DB_MODE_RO, IN_derefs_E29_30_dep_nek_dxTm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_stop, 1, DB_MODE_RW, IN_derefs_E29_30_dep_reducPrivate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_stop, 2, DB_MODE_RO, IN_derefs_E29_30_dep_nek_dxm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_stop, 3, DB_MODE_RO, IN_derefs_E29_30_dep_nekR.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_stop, 4, DB_MODE_RO, IN_derefs_E29_30_dep_nekP.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_stop, 5, DB_MODE_RO, IN_derefs_E29_30_dep_nekW.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_stop, 6, DB_MODE_RO, IN_derefs_E29_30_dep_NEKOtools.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_stop, 7, DB_MODE_RO, IN_derefs_E29_30_dep_nekZ.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_stop, 8, DB_MODE_RO, IN_derefs_E29_30_dep_nekX.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_stop, 9, DB_MODE_RO, IN_derefs_E29_30_dep_nek_G6.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_stop, 10, DB_MODE_RO, IN_derefs_E29_30_dep_nek_G4.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_stop, 11, DB_MODE_RO, IN_derefs_E29_30_dep_gDone3.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_stop, 12, DB_MODE_RO, IN_derefs_E29_30_dep_NEKOstatics.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_stop, 13, DB_MODE_RO, IN_derefs_E29_30_dep_nek_G1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_stop, 14, DB_MODE_RO, IN_derefs_E29_30_dep_NEKOglobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_stop, 15, DB_MODE_RO, IN_derefs_E29_30_dep_nekCGscalars.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_stop, 16, DB_MODE_RO, IN_derefs_E29_30_dep_nekC.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_nekCG_rtr_stop, 17, DB_MODE_RO, gd_CGtimes); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t nekCG_rtr_stop(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 31
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E30_31_dep_NEKOstatics = depv[12];
        NEKOstatics_t * io_NEKOstatics = IN_derefs_E30_31_dep_NEKOstatics.ptr;
        ocrEdtDep_t IN_derefs_E30_31_dep_NEKOglobals = depv[14];
        NEKOglobals_t * io_NEKOglobals = IN_derefs_E30_31_dep_NEKOglobals.ptr;
        ocrHint_t hintEDT, *pHintEDT=0;
        err = ocrXgetEdtHint(NEK_OCR_USE_CURRENT_PD, &hintEDT, &pHintEDT); IFEB;
        ocrEdtDep_t IN_derefs_E30_31_dep_NEKOtools = depv[6];
        ocrEdtDep_t IN_derefs_E30_31_dep_gDone3 = depv[11];
        ocrGuid_t * in_gDone3 = IN_derefs_E30_31_dep_gDone3.ptr;
        ocrEdtDep_t IN_derefs_E30_31_dep_nekCGscalars = depv[15];
        NEKO_CGscalars_t * in_nekCGscalars = IN_derefs_E30_31_dep_nekCGscalars.ptr;
        ocrEdtDep_t IN_derefs_E30_31_dep_CGtimes = depv[17];
        NEKO_CGtimings_t * in_CGtimes = IN_derefs_E30_31_dep_CGtimes.ptr;
        ocrEdtDep_t IN_derefs_E30_31_dep_nek_G1 = depv[13];
        ocrEdtDep_t IN_derefs_E30_31_dep_nek_G4 = depv[10];
        ocrEdtDep_t IN_derefs_E30_31_dep_nek_G6 = depv[9];
        ocrEdtDep_t IN_derefs_E30_31_dep_nek_dxm1 = depv[2];
        ocrEdtDep_t IN_derefs_E30_31_dep_nek_dxTm1 = depv[0];
        ocrEdtDep_t IN_derefs_E30_31_dep_nekC = depv[16];
        ocrEdtDep_t IN_derefs_E30_31_dep_nekP = depv[4];
        ocrEdtDep_t IN_derefs_E30_31_dep_nekR = depv[3];
        ocrEdtDep_t IN_derefs_E30_31_dep_nekW = depv[5];
        ocrEdtDep_t IN_derefs_E30_31_dep_nekX = depv[8];
        ocrEdtDep_t IN_derefs_E30_31_dep_nekZ = depv[7];
        ocrEdtDep_t IN_derefs_E30_31_dep_reducPrivate = depv[1];
            ocrEdtDep_t IN_BY_USER_depv_sum = depv[REDUC_SLOT_4Rtr-1];  //From nekbone_rtr_start().
            ocrGuid_t in_sum_guid = IN_BY_USER_depv_sum.guid;
            ReducSum_t * in_sum = IN_BY_USER_depv_sum.ptr;

        //----- Create_dataBlocks
        const u64 OA_Count_nekCGscalars = 1;
        const u64 OA_Count_CGtimes = 1;

        ocrGuid_t gd_nekCGscalars= NULL_GUID;
        NEKO_CGscalars_t * o_nekCGscalars=NULL;
        err = ocrDbCreate( &gd_nekCGscalars, (void**)&o_nekCGscalars, 1*sizeof(NEKO_CGscalars_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;
        ocrGuid_t gd_CGtimes= NULL_GUID;
        NEKO_CGtimings_t * o_CGtimes=NULL;
        err = ocrDbCreate( &gd_CGtimes, (void**)&o_CGtimes, 1*sizeof(NEKO_CGtimings_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs

        //----- User section
        err = nekbone_rtr_stop(io_NEKOstatics, io_NEKOglobals, in_nekCGscalars, o_nekCGscalars, in_CGtimes,o_CGtimes, in_sum_guid, in_sum); IFEB;
        ocrGuid_t ga_tailRecurTransitEND = *in_gDone3;

        //----- Release or destroy data blocks
        err = ocrDbRelease(gd_nekCGscalars); IFEB;
        err = ocrDbRelease(gd_CGtimes); IFEB;
        err = ocrDbDestroy( IN_derefs_E30_31_dep_gDone3.guid ); IFEB;
        err = ocrDbDestroy( IN_derefs_E30_31_dep_nekCGscalars.guid ); IFEB;
        err = ocrDbDestroy( IN_derefs_E30_31_dep_CGtimes.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E30_31_dep_nek_dxTm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E30_31_dep_reducPrivate.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E30_31_dep_nek_dxm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E30_31_dep_nekR.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E30_31_dep_nekP.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E30_31_dep_nekW.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E30_31_dep_NEKOtools.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E30_31_dep_nekZ.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E30_31_dep_nekX.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E30_31_dep_nek_G6.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E30_31_dep_nek_G4.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E30_31_dep_NEKOstatics.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E30_31_dep_nek_G1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E30_31_dep_NEKOglobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E30_31_dep_nekC.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitEND, 2, DB_MODE_RO, IN_derefs_E30_31_dep_nek_dxTm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitEND, 3, DB_MODE_RW, IN_derefs_E30_31_dep_reducPrivate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitEND, 4, DB_MODE_RO, IN_derefs_E30_31_dep_nek_dxm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitEND, 5, DB_MODE_RO, IN_derefs_E30_31_dep_nekR.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitEND, 6, DB_MODE_RO, IN_derefs_E30_31_dep_nekP.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitEND, 7, DB_MODE_RO, IN_derefs_E30_31_dep_nekW.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitEND, 8, DB_MODE_RO, IN_derefs_E30_31_dep_NEKOtools.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitEND, 9, DB_MODE_RO, IN_derefs_E30_31_dep_nekZ.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitEND, 10, DB_MODE_RO, IN_derefs_E30_31_dep_nekX.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitEND, 11, DB_MODE_RO, IN_derefs_E30_31_dep_nek_G6.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitEND, 12, DB_MODE_RO, IN_derefs_E30_31_dep_nek_G4.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitEND, 13, DB_MODE_RO, IN_derefs_E30_31_dep_NEKOglobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitEND, 14, DB_MODE_RO, IN_derefs_E30_31_dep_NEKOstatics.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitEND, 15, DB_MODE_RO, IN_derefs_E30_31_dep_nek_G1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitEND, 16, DB_MODE_RO, gd_CGtimes); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitEND, 17, DB_MODE_RO, IN_derefs_E30_31_dep_nekC.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecurTransitEND, 18, DB_MODE_RO, gd_nekCGscalars); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t tailRecurTransitEND(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 32
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E22_32_dep_TailRecurIterate = depv[0];
        TailRecurIterate_t * io_tailRecurIter = IN_derefs_E22_32_dep_TailRecurIterate.ptr;
        ocrEdtDep_t IN_derefs_E22_32_dep_SPMDGlobals = depv[1];
        ocrEdtDep_t IN_derefs_E31_32_dep_NEKOstatics = depv[14];
        ocrEdtDep_t IN_derefs_E31_32_dep_NEKOglobals = depv[13];
        NEKOglobals_t * io_NEKOglobals = IN_derefs_E31_32_dep_NEKOglobals.ptr;
        ocrHint_t hintEDT, *pHintEDT=0;
        err = ocrXgetEdtHint(NEK_OCR_USE_CURRENT_PD, &hintEDT, &pHintEDT); IFEB;
        ocrEdtDep_t IN_derefs_E31_32_dep_NEKOtools = depv[8];
        ocrEdtDep_t IN_derefs_E31_32_dep_nek_G1 = depv[15];
        ocrEdtDep_t IN_derefs_E31_32_dep_nek_G4 = depv[12];
        ocrEdtDep_t IN_derefs_E31_32_dep_nek_G6 = depv[11];
        ocrEdtDep_t IN_derefs_E31_32_dep_nek_dxm1 = depv[4];
        ocrEdtDep_t IN_derefs_E31_32_dep_nek_dxTm1 = depv[2];
        ocrEdtDep_t IN_derefs_E31_32_dep_nekC = depv[17];
        ocrEdtDep_t IN_derefs_E31_32_dep_nekP = depv[6];
        ocrEdtDep_t IN_derefs_E31_32_dep_nekR = depv[5];
        ocrEdtDep_t IN_derefs_E31_32_dep_nekW = depv[7];
        ocrEdtDep_t IN_derefs_E31_32_dep_nekX = depv[10];
        ocrEdtDep_t IN_derefs_E31_32_dep_nekZ = depv[9];
        ocrEdtDep_t IN_derefs_E31_32_dep_nekCGscalars = depv[18];
        NEKO_CGscalars_t * in_nekCGscalars = IN_derefs_E31_32_dep_nekCGscalars.ptr;
        ocrEdtDep_t IN_derefs_E31_32_dep_CGtimes = depv[16];
        NEKO_CGtimings_t * io_CGtimes = IN_derefs_E31_32_dep_CGtimes.ptr;
        ocrEdtDep_t IN_derefs_E31_32_dep_reducPrivate = depv[3];

        //----- Create_dataBlocks
        const u64 OA_Count_nekCGscalars = 1;

        ocrGuid_t gd_nekCGscalars= NULL_GUID;
        NEKO_CGscalars_t * o_nekCGscalars=NULL;
        err = ocrDbCreate( &gd_nekCGscalars, (void**)&o_nekCGscalars, 1*sizeof(NEKO_CGscalars_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC); IFEB;

        //----- Create children EDTs
        ocrGuid_t ga_tailRecursionIFThen = NULL_GUID;
        err = ocrEdtXCreate(tailRecursionIFThen, 0, NULL, 19, NULL, EDT_PROP_NONE, pHintEDT, &ga_tailRecursionIFThen, NULL); IFEB;

        //----- User section
        err = nekbone_tailRecurTransitEND(io_tailRecurIter->current, io_NEKOglobals->rankID, in_nekCGscalars, o_nekCGscalars); IFEB;

        //----- Release or destroy data blocks
        err = ocrDbRelease(gd_nekCGscalars); IFEB;
        err = ocrDbDestroy( IN_derefs_E31_32_dep_nekCGscalars.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E22_32_dep_TailRecurIterate.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E22_32_dep_SPMDGlobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E31_32_dep_nek_dxTm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E31_32_dep_reducPrivate.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E31_32_dep_nek_dxm1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E31_32_dep_nekR.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E31_32_dep_nekP.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E31_32_dep_nekW.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E31_32_dep_NEKOtools.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E31_32_dep_nekZ.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E31_32_dep_nekX.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E31_32_dep_nek_G6.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E31_32_dep_nek_G4.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E31_32_dep_NEKOglobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E31_32_dep_NEKOstatics.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E31_32_dep_nek_G1.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E31_32_dep_CGtimes.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E31_32_dep_nekC.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 0, DB_MODE_RW, IN_derefs_E22_32_dep_TailRecurIterate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 1, DB_MODE_RW, IN_derefs_E31_32_dep_reducPrivate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 2, DB_MODE_RO, IN_derefs_E31_32_dep_nek_dxm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 3, DB_MODE_RO, IN_derefs_E31_32_dep_nekR.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 4, DB_MODE_RO, IN_derefs_E31_32_dep_NEKOstatics.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 5, DB_MODE_RO, IN_derefs_E31_32_dep_nekP.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 6, DB_MODE_RO, IN_derefs_E31_32_dep_nekW.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 7, DB_MODE_RO, IN_derefs_E31_32_dep_NEKOtools.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 8, DB_MODE_RO, IN_derefs_E31_32_dep_nekZ.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 9, DB_MODE_RO, IN_derefs_E31_32_dep_nekX.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 10, DB_MODE_RO, IN_derefs_E31_32_dep_nek_G6.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 11, DB_MODE_RO, IN_derefs_E31_32_dep_nek_G4.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 12, DB_MODE_RO, IN_derefs_E31_32_dep_NEKOglobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 13, DB_MODE_RO, IN_derefs_E22_32_dep_SPMDGlobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 14, DB_MODE_RO, IN_derefs_E31_32_dep_nek_G1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 15, DB_MODE_RO, IN_derefs_E31_32_dep_nek_dxTm1.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 16, DB_MODE_RO, IN_derefs_E31_32_dep_CGtimes.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 17, DB_MODE_RO, IN_derefs_E31_32_dep_nekC.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_tailRecursionIFThen, 18, DB_MODE_RO, gd_nekCGscalars); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
ocrGuid_t concludeTailRecursion(EDT_ARGS)
{
    typedef enum ocr_constants {
        OA_edtTypeNb = 33
    } ocr_constants_t;
    ocrGuid_t OA_DBG_thisEDT = NULL_GUID;

    Err_t err=0;
    while(!err){
        //----- Setup input data structs
        ocrEdtDep_t IN_derefs_E21_33_dep_SPMDGlobals = depv[4];
        ocrEdtDep_t IN_derefs_E19_33_dep_gDone = depv[0];
        ocrGuid_t * in_gDone = IN_derefs_E19_33_dep_gDone.ptr;
        ocrGuid_t ga_BtForkTransition_Stop = *in_gDone;
        ocrEdtDep_t IN_derefs_E21_33_dep_NEKOstatics = depv[5];
        NEKOstatics_t * io_NEKOstatics = IN_derefs_E21_33_dep_NEKOstatics.ptr;
        ocrEdtDep_t IN_derefs_E21_33_dep_NEKOglobals = depv[3];
        ocrHint_t hintEDT, *pHintEDT=0;
        err = ocrXgetEdtHint(NEK_OCR_USE_CURRENT_PD, &hintEDT, &pHintEDT); IFEB;
        ocrEdtDep_t IN_derefs_E21_33_dep_NEKOtools = depv[2];
        ocrEdtDep_t IN_derefs_E21_33_dep_reducPrivate = depv[1];

        //----- Create_dataBlocks


        //----- Create children EDTs

        //----- User section
        err = tailRecurConclude(); IFEB;

        //----- Release or destroy data blocks
        err = ocrDbDestroy( IN_derefs_E19_33_dep_gDone.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E21_33_dep_reducPrivate.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E21_33_dep_NEKOtools.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E21_33_dep_NEKOglobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E21_33_dep_SPMDGlobals.guid ); IFEB;
        err = ocrDbRelease(IN_derefs_E21_33_dep_NEKOstatics.guid ); IFEB;

        //----- Link to other EDTs using Events
        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtForkTransition_Stop, 1, DB_MODE_RO, IN_derefs_E21_33_dep_reducPrivate.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtForkTransition_Stop, 2, DB_MODE_RO, IN_derefs_E21_33_dep_NEKOtools.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtForkTransition_Stop, 3, DB_MODE_RO, IN_derefs_E21_33_dep_NEKOglobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtForkTransition_Stop, 4, DB_MODE_RO, IN_derefs_E21_33_dep_SPMDGlobals.guid); IFEB;

        err = ocrXHookup(OCR_EVENT_ONCE_T, EVT_PROP_TAKES_ARG, ga_BtForkTransition_Stop, 5, DB_MODE_RO, IN_derefs_E21_33_dep_NEKOstatics.guid); IFEB;

        break; //while(!err)
    }
    EDT_ERROR(err);
    return NULL_GUID;
}
