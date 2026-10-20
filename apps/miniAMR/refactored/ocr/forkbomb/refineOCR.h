#define MAY_REFINE  0
#define WILL_REFINE 1
#define WONT_REFINE 2

#define MORE_REFINED -1
#define SAME_REFINED  0
#define LESS_REFINED  1

#include<stdlib.h>

ocrGuid_t blockEdt( u32 paramc, u64 * paramv, u32 depc, ocrEdtDep_t depv[] );

ocrGuid_t refineControlEdt( u32 paramc, u64 * paramv, u32 depc, ocrEdtDep_t depv[] )
{
    block_t PRM_block;
    memcpy( &PRM_block, paramv, sizeof(block_t) );
    u32 pCount = (sizeof( block_t ) / sizeof( u64 )) +1;

    //The die is a function of the block id alone, drawn from task-local state so concurrent tasks cannot perturb it
    struct random_data dieState = { 0 };
    int32_t dieTable[32];
    initstate_r( (unsigned int)((PRM_block.id + 1) * 2654435761u), (char *)dieTable, sizeof(dieTable), &dieState );
    ocrGuid_t blockDriverGUID, blockTML;
    ocrEdtTemplateCreate( &blockTML, blockEdt, pCount, 1 );

    int32_t dieDraw;
    random_r( &dieState, &dieDraw );
    s64 willRef = dieDraw;

    if( (willRef % 20 == 0) && PRM_block.refLvl < PRM_block.maxRefLvl )
    {
        u64 i;
        block_t childBlock = PRM_block;
        childBlock.refLvl++;
        if( PRM_block.refLvl == 0 ) PRM_block.parent = PRM_block.id;
        childBlock.id = PRM_block.id * 8 * childBlock.refLvl;

        for( i = 0; i < 8; i++ )
        {
            childBlock.id += i;
            ocrEdtCreate( &blockDriverGUID, blockTML, EDT_PARAM_DEF, (u64 *)&childBlock, 1, NULL, EDT_PROP_NONE, NULL_HINT, NULL );
            ocrAddDependence( NULL_GUID, blockDriverGUID, 0, DB_MODE_RW );
        }

    }
    else
    {
        //A hit that cannot refine still continues the chain: every block chain must reach its last timestep
        if( (willRef % 20 == 0) && PRM_block.refLvl == PRM_block.maxRefLvl )
            ocrPrintf("%ld cannot refine more!\n", PRM_block.id);
        ocrEdtCreate( &blockDriverGUID, blockTML, EDT_PARAM_DEF, (u64 *)&PRM_block, 1, NULL, EDT_PROP_NONE, NULL_HINT, NULL );
        ocrAddDependence( NULL_GUID, blockDriverGUID, 0, DB_MODE_RW );
    }

    ocrEdtTemplateDestroy( blockTML );

    return NULL_GUID;
}
