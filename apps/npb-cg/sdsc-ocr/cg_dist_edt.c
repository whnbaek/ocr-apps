/* Distributed NPB CG driver.

   The vectors are partitioned by row band across the policy domains: rank i
   owns rows [i*na/R, (i+1)*na/R).  Every rank runs a persistent chain of EDTs
   pinned to its own policy domain; the only data that crosses a rank boundary
   is (a) one immutable vector fragment per neighbour per matrix-vector
   product and (b) the scalars of the two conjugate-gradient reductions.

   Fragments are published as FRESH datablocks handed over persistent channel
   events, one channel per ordered (producer, consumer) pair.  A fragment is
   written exactly once, read once by its consumer and then destroyed, so no
   coherence protocol ever has to invalidate, re-validate or migrate a vector
   between ranks.  That is the whole point of the rewrite: a whole-vector
   datablock updated every inner iteration forces every reader's copy to be
   re-fetched at every iteration, which is quadratic in the rank count.

   The matrix follows the same rule.  It is assembled once as a single image,
   sliced into per-rank row bands, and each rank then copies its band into a
   datablock it creates itself, so the band is home-resident for the rest of
   the run.  A shared read-only image would instead be re-acquired once per
   matrix-vector product from a single home rank, which is free only for the
   protocols that let a reader keep its copy across turns.
*/

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <ocr.h>
#include <extensions/ocr-affinity.h>
#include <extensions/ocr-labeling.h>

#include "cg_ocr.h"
#include "la_ocr.h"
#include "cg_dist.h"

ocrGuid_t cg_flatten_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t cg_rank_init_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t cg_chan_init_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t cg_bcast_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t cg_spmv_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t cg_slice_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t cg_slicejoin_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t cg_chunk_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t cg_join_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t cg_alpha_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t cg_beta_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t cg_outer_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t cg_final_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t cg_shutdown_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

static void cg_hint_at(ocrHint_t* hnt, u64 rank)
{
    ocrGuid_t affinity;
    ocrHintInit(hnt, OCR_HINT_EDT_T);
    ocrAffinityGetAt(AFFINITY_PD, rank, &affinity);
    ocrSetHintValue(hnt, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(affinity));
}

static void cg_release(ocrGuid_t guid)
{
    if(!ocrGuidIsNull(guid))
        ocrDbRelease(guid);
}

/* Hand the four datablocks every chain EDT carries to the next link.  The
   caller must already have released the ones it wrote. */
static void cg_chain_wire(ocrGuid_t edt, ocrGuid_t priv, ocrGuid_t vec,
                          ocrGuid_t timer, ocrGuid_t red)
{
    ocrAddDependence(priv, edt, CG_SL_PRIV, DB_MODE_RW);
    ocrAddDependence(vec, edt, CG_SL_VEC, DB_MODE_RW);
    ocrAddDependence(timer, edt, CG_SL_TIMER, DB_MODE_RW);
    ocrAddDependence(red, edt, CG_SL_RED, DB_MODE_RW);
}

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    u64 argc = ocrGetArgc(depv[0].ptr);

    classdb_t* class; ocrGuid_t classid;
    u32 niter_override = 0;
    if(argc>1) {
        if(argc!=3 && argc != 5 && argc != 7) {
            ocrPrintf("cg [-t class] [-b blocking ] [-i niter] (class=T|S|W|A|B|C|D|E; default: -t S, -b 1)\n");
            ocrShutdown();
            return NULL_GUID;
        }
        u32 blocking = 1;
        char classt = 'S';
        while(--argc) {
            if(strcmp(ocrGetArgv(depv[0].ptr,argc-1), "-t")==0)
                classt = *ocrGetArgv(depv[0].ptr,argc--);
            else if(strcmp(ocrGetArgv(depv[0].ptr,argc-1), "-b")==0)
                blocking = atoi(ocrGetArgv(depv[0].ptr,argc--));
            else if(strcmp(ocrGetArgv(depv[0].ptr,argc-1), "-i")==0)
                niter_override = atoi(ocrGetArgv(depv[0].ptr,argc--));
        }
        class_init(&class, &classid, classt, blocking);
        if(class->c == 'U') {
            ocrPrintf("cg [-t] [T,S,W,A,B,C,D,E] (default: S -t)\n");
            ocrShutdown();
            return NULL_GUID;
        }
    }
    else
        class_init(&class, &classid, 'S', 1);
    if(niter_override >= 1)
        class->niter = niter_override;

    if(class->blk == 0 || class->na % class->blk) {
        ocrPrintf("blocking %u must divide the problem size %lu\n", class->blk, class->na);
        ocrShutdown();
        return NULL_GUID;
    }

    u64 nrank;
    ocrAffinityCount(AFFINITY_PD, &nrank);
    if(nrank > CG_DIST_MAX_RANKS) {
        ocrPrintf("cg_dist supports at most %d policy domains (%lu present)\n",
                  CG_DIST_MAX_RANKS, nrank);
        ocrShutdown();
        return NULL_GUID;
    }
    if(nrank > class->na) {
        ocrPrintf("problem size %lu is smaller than the policy domain count %lu\n",
                  class->na, nrank);
        ocrShutdown();
        return NULL_GUID;
    }

    timerdb_t* timer; ocrGuid_t timerid;
    timer_init(&timer, &timerid, class, class->on);
    ocrPrintf("CG Benchmark: size=%lu, iterations=%u\n", class->na, class->niter);

    timer_start(timer);

    ocrGuid_t aid;
    if(makea(class, &aid) == -1)
        return NULL_GUID;

    /* makea's own admission bound on the number of generated entries; the
       duplicate-merging pass can only lower the count, so it is a safe
       capacity for the flattened image and spares a counting sweep. */
    u64 capacity = class->na * (u64)(class->nonzer+1) * (u64)(class->nonzer+1);

    cg_csr_t* csr; ocrGuid_t matrixid;
    ocrDbCreate(&matrixid, (void**)&csr, cg_csr_bytes(class->na, capacity),
                0, NULL_HINT, NO_ALLOC);
    csr->nrow = class->na;
    csr->nnz = 0;
    csr->capacity = capacity;

    cg_dist_shared_t* shared; ocrGuid_t sharedid;
    ocrDbCreate(&sharedid, (void**)&shared, sizeof(cg_dist_shared_t),
                0, NULL_HINT, NO_ALLOC);
    shared->nrank = nrank;
    shared->na = class->na;
    shared->blk = class->blk;
    shared->niter = class->niter;
    shared->capacity = capacity;
    shared->shift = class->shift;
    shared->zvv = class->zvv;
    shared->timing_on = class->on;
    shared->classdb = classid;
    shared->timer = timerid;
    ocrGuidRangeCreate(&shared->reduceRange, nrank, GUID_USER_EVENT_STICKY);
    ocrGuidRangeCreate(&shared->channelRange, nrank*nrank, GUID_USER_EVENT_STICKY);

    ocrHint_t hnt;
    cg_hint_at(&hnt, 0);

    /* The per-row-block GUIDs are readable only from inside the container
       datablock, so the chain starts with an empty batch whose only job is to
       acquire the container and wire the first real batch from it. */
    ocrGuid_t tmp, edt;
    u64 start = 0;
    ocrEdtTemplateCreate(&tmp, cg_flatten_edt, 1, 3);
    ocrEdtCreate(&edt, tmp, 1, &start, 3, NULL, 0, &hnt, NULL);
    ocrEdtTemplateDestroy(tmp);

    ocrDbRelease(classid);
    ocrDbRelease(timerid);
    ocrDbRelease(sharedid);
    ocrDbRelease(matrixid);

    ocrAddDependence(sharedid, edt, 0, DB_MODE_RO);
    ocrAddDependence(matrixid, edt, 1, DB_MODE_RW);
    ocrAddDependence(aid, edt, 2, DB_MODE_RO);

    ocrDbDestroy(depv[0].guid);

    return NULL_GUID;
}

/* paramv[0]: first row block of this batch.
   depv: 0 shared, 1 matrix, 2 block-GUID container, 3.. the batch's blocks. */
ocrGuid_t cg_flatten_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    cg_dist_shared_t* shared = (cg_dist_shared_t*)depv[0].ptr;
    cg_csr_t* csr = (cg_csr_t*)depv[1].ptr;
    ocrGuid_t* ablocks = (ocrGuid_t*)depv[2].ptr;

    u64 nblk = shared->na / shared->blk;
    u64 blk = shared->blk;
    u64 even = 2*((blk+2)>>1);
    u64 start = paramv[0];
    u64 count = depc - 3;

    double* values = cg_csr_values(csr);
    u64* rowstr = cg_csr_rowstr(csr);
    u32* colidx = cg_csr_colidx(csr);
    u64 pos = csr->nnz;

    u64 t, b;
    for(t = 0; t < count; ++t) {
        u32* rows = (u32*)depv[3+t].ptr;
        double* bvals = (double*)(rows + even);
        u32* bcols = rows + rows[blk];
        for(b = 0; b < blk; ++b) {
            u64 row = (start+t)*blk + b;
            u64 cnt = rows[b];
            rowstr[row] = pos;
            memcpy(values+pos, bvals, cnt*sizeof(double));
            memcpy(colidx+pos, bcols, cnt*sizeof(u32));
            pos += cnt;
            bvals += cnt;
            bcols += cnt;
        }
        ocrDbDestroy(depv[3+t].guid);
    }
    if(pos > csr->capacity) {
        ocrPrintf("flattened matrix exceeded its capacity (%lu > %lu)\n", pos, csr->capacity);
        ocrShutdown();
        return NULL_GUID;
    }
    csr->nnz = pos;

    ocrGuid_t sharedid = depv[0].guid;
    ocrGuid_t matrixid = depv[1].guid;
    ocrGuid_t containerid = depv[2].guid;
    u64 nrank = shared->nrank;

    ocrHint_t hnt;
    ocrGuid_t tmp, edt;

    if(start + count < nblk) {
        u64 next = start + count;
        u64 batch = nblk - next;
        if(batch > CG_DIST_FLATTEN_CHUNK) batch = CG_DIST_FLATTEN_CHUNK;
        cg_hint_at(&hnt, 0);
        ocrEdtTemplateCreate(&tmp, cg_flatten_edt, 1, 3+batch);
        ocrEdtCreate(&edt, tmp, 1, &next, 3+batch, NULL, 0, &hnt, NULL);
        ocrEdtTemplateDestroy(tmp);
        ocrDbRelease(matrixid);
        ocrAddDependence(sharedid, edt, 0, DB_MODE_RO);
        ocrAddDependence(matrixid, edt, 1, DB_MODE_RW);
        ocrAddDependence(containerid, edt, 2, DB_MODE_RO);
        u64 i;
        for(i = 0; i < batch; ++i)
            ocrAddDependence(ablocks[next+i], edt, 3+i, DB_MODE_RO);
        return NULL_GUID;
    }

    rowstr[shared->na] = pos;
    ocrPrintf("number of nonzeros = %lu\n", pos);
    ocrDbDestroy(containerid);

    /* Slice the assembled image into one exactly sized band per rank.  These
       seeds are still homed here; each rank re-homes its own by copying it,
       which is why only 1/R of the matrix has to travel to any one rank. */
    u64 r;
    ocrGuid_t seed[CG_DIST_MAX_RANKS];
    for(r = 0; r < nrank; ++r) {
        u64 lo = r*shared->na/nrank;
        u64 hi = (r+1)*shared->na/nrank;
        u64 nrow = hi - lo;
        u64 base = rowstr[lo];
        u64 bnnz = rowstr[hi] - base;
        cg_csr_t* band;
        ocrDbCreate(&seed[r], (void**)&band, cg_csr_bytes(nrow, bnnz),
                    0, NULL_HINT, NO_ALLOC);
        band->nrow = nrow;
        band->nnz = bnnz;
        band->capacity = bnnz;
        memcpy(cg_csr_values(band), values+base, bnnz*sizeof(double));
        memcpy(cg_csr_colidx(band), colidx+base, bnnz*sizeof(u32));
        u64* brow = cg_csr_rowstr(band);
        u64 i;
        for(i = 0; i <= nrow; ++i)
            brow[i] = rowstr[lo+i] - base;
        ocrDbRelease(seed[r]);
    }
    ocrDbDestroy(matrixid);

    ocrEdtTemplateCreate(&tmp, cg_rank_init_edt, 1, 2);
    for(r = 0; r < nrank; ++r) {
        cg_hint_at(&hnt, r);
        ocrEdtCreate(&edt, tmp, 1, &r, 2, NULL, 0, &hnt, NULL);
        ocrAddDependence(sharedid, edt, 0, DB_MODE_RO);
        ocrAddDependence(seed[r], edt, 1, DB_MODE_RO);
    }
    ocrEdtTemplateDestroy(tmp);

    return NULL_GUID;
}

/* paramv[0]: my rank.  depv: 0 shared, 1 this rank's band seed. */
ocrGuid_t cg_rank_init_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    cg_dist_shared_t* shared = (cg_dist_shared_t*)depv[0].ptr;
    cg_csr_t* seed = (cg_csr_t*)depv[1].ptr;
    u64 myrank = paramv[0];
    u64 nrank = shared->nrank;
    u64 na = shared->na;

    cg_dist_private_t* priv; ocrGuid_t privid;
    ocrDbCreate(&privid, (void**)&priv, sizeof(cg_dist_private_t), 0, NULL_HINT, NO_ALLOC);
    memset(priv, 0, sizeof(cg_dist_private_t));

    priv->nrank = nrank;
    priv->myrank = myrank;
    priv->na = na;
    priv->niter = shared->niter;
    priv->shift = shared->shift;
    priv->zvv = shared->zvv;
    priv->timing_on = shared->timing_on;
    priv->outer = 0;
    priv->inner = 0;
    priv->residual = 0;
    priv->timer = (myrank == 0) ? shared->timer : NULL_GUID;
    priv->classdb = (myrank == 0) ? shared->classdb : NULL_GUID;

    u64 j;
    for(j = 0; j <= nrank; ++j)
        priv->rowsplit[j] = j*na/nrank;
    priv->lo = priv->rowsplit[myrank];
    priv->hi = priv->rowsplit[myrank+1];
    priv->nloc = priv->hi - priv->lo;

    cg_hint_at(&priv->myHNT, myrank);

    double* v; ocrGuid_t vecid;
    ocrDbCreate(&vecid, (void**)&v, sizeof(double)*(5*priv->nloc + na), 0, NULL_HINT, NO_ALLOC);
    priv->vec = vecid;
    { u64 i; double* x = cg_vec_x(v, priv->nloc);
      for(i = 0; i < priv->nloc; ++i) x[i] = 1; }

    reductionPrivate_t* rp; ocrGuid_t redid;
    ocrDbCreate(&redid, (void**)&rp, sizeof(reductionPrivate_t), 0, NULL_HINT, NO_ALLOC);
    memset(rp, 0, sizeof(reductionPrivate_t));
    rp->new = 1;
    rp->nrank = nrank;
    rp->myrank = myrank;
    rp->ndata = CG_DIST_REDUCE_NDATA;
    rp->type = ALLREDUCE;
    rp->reductionOperator = REDUCTION_F8_ADD;
    rp->rangeGUID = shared->reduceRange;
    priv->redPriv = redid;

    ocrEventParams_t params;
    params.EVENT_CHANNEL.maxGen = 8;
    params.EVENT_CHANNEL.nbSat = 1;
    params.EVENT_CHANNEL.nbDeps = 1;
    ocrEventCreateParams(&rp->returnEVT, OCR_EVENT_CHANNEL_T, false, &params);
    priv->returnEVT = rp->returnEVT;

    /* The product fans out over row slices so the whole worker pool of this
       rank works on it; the slice count is fixed for the run, so both
       templates are made once here. */
    priv->nchunk = (priv->nloc + CG_DIST_ROWS_PER_TASK - 1) / CG_DIST_ROWS_PER_TASK;
    if(priv->nchunk > CG_DIST_MAX_CHUNKS) priv->nchunk = CG_DIST_MAX_CHUNKS;
    if(priv->nchunk < 1) priv->nchunk = 1;

    ocrEdtTemplateCreate(&priv->bcastTML, cg_bcast_edt, 0, 4);
    ocrEdtTemplateCreate(&priv->spmvTML, cg_spmv_edt, 0, CG_SL_TAIL+nrank);
    ocrEdtTemplateCreate(&priv->chunkTML, cg_chunk_edt, 5, 2);
    ocrEdtTemplateCreate(&priv->joinTML, cg_join_edt, 0, CG_SL_TAIL+priv->nchunk);
    ocrEdtTemplateCreate(&priv->alphaTML, cg_alpha_edt, 0, 5);
    ocrEdtTemplateCreate(&priv->betaTML, cg_beta_edt, 0, 5);
    ocrEdtTemplateCreate(&priv->outerTML, cg_outer_edt, 0, 5);
    ocrEdtTemplateCreate(&priv->finalTML, cg_final_edt, 0, 6);

    /* One channel per ordered pair.  The producer owns the event and publishes
       its GUID through a labeled sticky event so that the consumer, which
       cannot see the producer's local creation, can name it. */
    ocrGuid_t chanEDT, chanTML;
    ocrEdtTemplateCreate(&chanTML, cg_chan_init_edt, 0, CG_SL_TAIL+nrank+1);
    ocrEdtCreate(&chanEDT, chanTML, 0, NULL, CG_SL_TAIL+nrank+1, NULL, 0, &priv->myHNT, NULL);
    ocrEdtTemplateDestroy(chanTML);

    /* Cut the band into one datablock per row slice, each created by a task
       of this rank, and gate the chain on the cut. */
    ocrGuid_t sliceTML, sjTML, sliceJoin, sliceJoinOut;
    ocrEdtTemplateCreate(&sjTML, cg_slicejoin_edt, 0, 2+priv->nchunk);
    ocrEdtCreate(&sliceJoin, sjTML, 0, NULL, 2+priv->nchunk, NULL, 0,
                 &priv->myHNT, &sliceJoinOut);
    ocrEdtTemplateDestroy(sjTML);
    ocrEdtTemplateCreate(&sliceTML, cg_slice_edt, 3, 1);
    { u64 k;
      for(k = 0; k < priv->nchunk; ++k) {
        u64 pv[3] = { k, priv->nchunk, priv->nloc };
        ocrGuid_t st, done;
        ocrEdtCreate(&st, sliceTML, 3, pv, 1, NULL, 0, &priv->myHNT, &done);
        ocrAddDependence(depv[1].guid, st, 0, DB_MODE_RO);
        ocrAddDependence(done, sliceJoin, 2+k, DB_MODE_RO);
      } }
    ocrEdtTemplateDestroy(sliceTML);
    ocrAddDependence(privid, sliceJoin, 0, DB_MODE_RW);
    ocrAddDependence(depv[1].guid, sliceJoin, 1, DB_MODE_RO);
    ocrAddDependence(sliceJoinOut, chanEDT, CG_SL_TAIL+nrank, DB_MODE_NULL);

    for(j = 0; j < nrank; ++j) {
        if(j == myrank) {
            priv->sendEVT[j] = NULL_GUID;
            ocrAddDependence(NULL_GUID, chanEDT, CG_SL_TAIL+j, DB_MODE_RO);
            continue;
        }
        ocrGuid_t stickyEVT, holder; ocrGuid_t* slot;
        ocrEventCreateParams(&priv->sendEVT[j], OCR_EVENT_CHANNEL_T, false, &params);
        ocrDbCreate(&holder, (void**)&slot, sizeof(ocrGuid_t), 0, NULL_HINT, NO_ALLOC);
        *slot = priv->sendEVT[j];
        ocrDbRelease(holder);
        ocrGuidFromIndex(&stickyEVT, shared->channelRange, myrank*nrank + j);
        ocrEventCreate(&stickyEVT, OCR_EVENT_STICKY_T, GUID_PROP_CHECK | EVT_PROP_TAKES_ARG);
        ocrEventSatisfy(stickyEVT, holder);

        ocrGuidFromIndex(&stickyEVT, shared->channelRange, j*nrank + myrank);
        ocrEventCreate(&stickyEVT, OCR_EVENT_STICKY_T, GUID_PROP_CHECK | EVT_PROP_TAKES_ARG);
        ocrAddDependence(stickyEVT, chanEDT, CG_SL_TAIL+j, DB_MODE_RO);
    }

    ocrGuid_t timerid = priv->timer;
    ocrDbRelease(privid);
    ocrDbRelease(vecid);
    ocrDbRelease(redid);
    cg_chain_wire(chanEDT, privid, vecid, timerid, redid);

    return NULL_GUID;
}

ocrGuid_t cg_chan_init_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    cg_dist_private_t* priv = (cg_dist_private_t*)depv[CG_SL_PRIV].ptr;
    u64 nrank = priv->nrank;
    u64 j;
    for(j = 0; j < nrank; ++j) {
        if(depv[CG_SL_TAIL+j].ptr == NULL) {
            priv->recvEVT[j] = NULL_GUID;
            continue;
        }
        priv->recvEVT[j] = *(ocrGuid_t*)depv[CG_SL_TAIL+j].ptr;
        ocrDbDestroy(depv[CG_SL_TAIL+j].guid);
    }

    ocrGuid_t edt;
    ocrEdtCreate(&edt, priv->bcastTML, 0, NULL, 4, NULL, 0, &priv->myHNT, NULL);
    ocrGuid_t timer = priv->timer;
    ocrDbRelease(depv[CG_SL_PRIV].guid);
    cg_release(depv[CG_SL_VEC].guid);
    cg_release(timer);
    cg_release(depv[CG_SL_RED].guid);
    cg_chain_wire(edt, depv[CG_SL_PRIV].guid, depv[CG_SL_VEC].guid, timer, depv[CG_SL_RED].guid);

    return NULL_GUID;
}

/* Publish this rank's fragment of the vector the next matrix-vector product
   consumes, then wire the product itself. */
ocrGuid_t cg_bcast_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    cg_dist_private_t* priv = (cg_dist_private_t*)depv[CG_SL_PRIV].ptr;
    double* v = (double*)depv[CG_SL_VEC].ptr;
    u64 nrank = priv->nrank;
    u64 myrank = priv->myrank;
    u64 nloc = priv->nloc;

    if(!priv->residual && priv->inner == 0) {
        /* Start of a conjugate-gradient pass: z = 0, r = x, p = x. */
        double* x = cg_vec_x(v, nloc);
        double* z = cg_vec_z(v, nloc);
        double* r = cg_vec_r(v, nloc);
        double* p = cg_vec_p(v, nloc);
        u64 i;
        for(i = 0; i < nloc; ++i) { z[i] = 0; r[i] = x[i]; p[i] = x[i]; }
    }

    double* src = priv->residual ? cg_vec_z(v, nloc) : cg_vec_p(v, nloc);

    ocrGuid_t sendEVT[CG_DIST_MAX_RANKS];
    ocrGuid_t recvEVT[CG_DIST_MAX_RANKS];
    u64 j;
    for(j = 0; j < nrank; ++j) {
        sendEVT[j] = priv->sendEVT[j];
        recvEVT[j] = priv->recvEVT[j];
    }
    ocrGuid_t spmvTML = priv->spmvTML;
    ocrHint_t hnt = priv->myHNT;
    ocrGuid_t timer = priv->timer;

    for(j = 0; j < nrank; ++j) {
        if(j == myrank) continue;
        ocrGuid_t frag; double* fptr;
        ocrDbCreate(&frag, (void**)&fptr, sizeof(double)*nloc, 0, NULL_HINT, NO_ALLOC);
        memcpy(fptr, src, sizeof(double)*nloc);
        ocrDbRelease(frag);
        ocrEventSatisfy(sendEVT[j], frag);
    }

    ocrGuid_t edt;
    ocrEdtCreate(&edt, spmvTML, 0, NULL, CG_SL_TAIL+nrank, NULL, 0, &hnt, NULL);

    ocrDbRelease(depv[CG_SL_PRIV].guid);
    ocrDbRelease(depv[CG_SL_VEC].guid);
    cg_release(timer);
    cg_release(depv[CG_SL_RED].guid);
    cg_chain_wire(edt, depv[CG_SL_PRIV].guid, depv[CG_SL_VEC].guid, timer, depv[CG_SL_RED].guid);
    for(j = 0; j < nrank; ++j)
        ocrAddDependence(j == myrank ? NULL_GUID : recvEVT[j], edt,
                         CG_SL_TAIL+j, DB_MODE_RO);

    return NULL_GUID;
}

/* Cuts one row slice out of this rank's band seed into a datablock this task
   creates, so the slices are first touched by tasks spread over the rank's
   workers instead of all living where one initializing worker put them.
   paramv: slice index, slice count, rows in the band.  depv: 0 seed. */
ocrGuid_t cg_slice_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    cg_csr_t* seed = (cg_csr_t*)depv[0].ptr;
    u64 k = paramv[0], nchunk = paramv[1], nloc = paramv[2];
    u64 lo = k*nloc/nchunk, hi = (k+1)*nloc/nchunk;
    u64* srow = cg_csr_rowstr(seed);
    u64 base = srow[lo], nnz = srow[hi] - base, nrow = hi - lo;

    cg_csr_t* sl; ocrGuid_t slid;
    ocrDbCreate(&slid, (void**)&sl, cg_csr_bytes(nrow, nnz), 0, NULL_HINT, NO_ALLOC);
    sl->nrow = nrow;
    sl->nnz = nnz;
    sl->capacity = nnz;
    memcpy(cg_csr_values(sl), cg_csr_values(seed)+base, nnz*sizeof(double));
    memcpy(cg_csr_colidx(sl), cg_csr_colidx(seed)+base, nnz*sizeof(u32));
    u64* drow = cg_csr_rowstr(sl);
    u64 i;
    for(i = 0; i <= nrow; ++i)
        drow[i] = srow[lo+i] - base;
    ocrDbRelease(slid);

    ocrGuid_t out; ocrGuid_t* slot;
    ocrDbCreate(&out, (void**)&slot, sizeof(ocrGuid_t), 0, NULL_HINT, NO_ALLOC);
    *slot = slid;
    return out;
}

/* Records the cut in the rank's private block and drops the seed.
   depv: 0 private, 1 seed, 2.. one slice guid each. */
ocrGuid_t cg_slicejoin_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    cg_dist_private_t* priv = (cg_dist_private_t*)depv[0].ptr;
    u32 k;
    for(k = 2; k < depc; ++k) {
        priv->slice[k-2] = *(ocrGuid_t*)depv[k].ptr;
        ocrDbDestroy(depv[k].guid);
    }
    ocrDbDestroy(depv[1].guid);
    return NULL_GUID;
}

/* One row slice of this rank's band: writes its own rows of q and returns the
   partial dots those rows contribute.  Every slice of a rank runs on that
   rank and writes a disjoint range, so the fan-out is intra-node and needs no
   ordering beyond the join that consumes the partials.
   paramv: nloc, lo, hi, residual, first-inner.  depv: 0 vector, 1 band. */
ocrGuid_t cg_chunk_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    double* v = (double*)depv[0].ptr;
    cg_csr_t* csr = (cg_csr_t*)depv[1].ptr;
    u64 nloc = paramv[0], lo = paramv[1], hi = paramv[2];
    u32 n = (u32)(hi - lo);

    double* values = cg_csr_values(csr);
    u64* rowstr = cg_csr_rowstr(csr);
    u32* colidx = cg_csr_colidx(csr);
    double* full = cg_vec_full(v, nloc);
    double* q = cg_vec_q(v, nloc);
    u64 i;
    /* the slice's row offsets are its own, so row lo+i is local row i */
    for(i = 0; i < (u64)n; ++i) {
        u64 s = rowstr[i];
        q[lo+i] = _dotg((u32)(rowstr[i+1]-s), values+s, colidx+s, full);
    }

    ocrGuid_t pg; double* part;
    ocrDbCreate(&pg, (void**)&part, sizeof(double)*CG_DIST_REDUCE_NDATA,
                0, NULL_HINT, NO_ALLOC);
    part[0] = part[1] = part[2] = 0;
    if(paramv[3]) {
        double* x = cg_vec_x(v, nloc);
        double* z = cg_vec_z(v, nloc);
        double d = 0;
        for(i = lo; i < hi; ++i) d += (q[i]-x[i])*(q[i]-x[i]);
        part[0] = d;
        part[1] = _dot(n, x+lo, z+lo);
        part[2] = _dot(n, z+lo, z+lo);
    }
    else {
        double* p = cg_vec_p(v, nloc);
        double* x = cg_vec_x(v, nloc);
        part[0] = _dot(n, p+lo, q+lo);
        /* rho of the first inner iteration is x.x; folding it into this
           reduction keeps the pass free of a separate synchronization. */
        part[1] = paramv[4] ? _dot(n, x+lo, x+lo) : 0;
    }

    return pg;
}

/* Sums this rank's slice partials and resumes the chain. */
ocrGuid_t cg_join_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    cg_dist_private_t* priv = (cg_dist_private_t*)depv[CG_SL_PRIV].ptr;
    reductionPrivate_t* rp = (reductionPrivate_t*)depv[CG_SL_RED].ptr;

    double partial[CG_DIST_REDUCE_NDATA] = {0, 0, 0};
    u32 k;
    for(k = CG_SL_TAIL; k < depc; ++k) {
        double* part = (double*)depv[k].ptr;
        partial[0] += part[0];
        partial[1] += part[1];
        partial[2] += part[2];
        ocrDbDestroy(depv[k].guid);
    }

    ocrGuid_t nextTML = priv->residual ? priv->outerTML : priv->alphaTML;
    ocrGuid_t timer = priv->timer;
    ocrGuid_t returnEVT = priv->returnEVT;
    ocrHint_t hnt = priv->myHNT;

    ocrGuid_t edt;
    ocrEdtCreate(&edt, nextTML, 0, NULL, 5, NULL, 0, &hnt, NULL);
    ocrDbRelease(depv[CG_SL_PRIV].guid);
    cg_release(depv[CG_SL_VEC].guid);
    cg_release(timer);
    cg_chain_wire(edt, depv[CG_SL_PRIV].guid, depv[CG_SL_VEC].guid, timer,
                  depv[CG_SL_RED].guid);
    ocrAddDependence(returnEVT, edt, CG_SL_TAIL, DB_MODE_RO);

    reductionLaunch(rp, depv[CG_SL_RED].guid, partial);

    return NULL_GUID;
}

/* Assembles the operand from the incoming fragments, then fans the product
   out over the rank's workers: one task per row slice, joined by cg_join. */
ocrGuid_t cg_spmv_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    cg_dist_private_t* priv = (cg_dist_private_t*)depv[CG_SL_PRIV].ptr;
    double* v = (double*)depv[CG_SL_VEC].ptr;

    u64 nrank = priv->nrank;
    u64 myrank = priv->myrank;
    u64 nloc = priv->nloc;

    double* full = cg_vec_full(v, nloc);
    double* src = priv->residual ? cg_vec_z(v, nloc) : cg_vec_p(v, nloc);

    u64 j;
    for(j = 0; j < nrank; ++j) {
        u64 blo = priv->rowsplit[j];
        u64 bn = priv->rowsplit[j+1] - blo;
        if(j == myrank) {
            memcpy(full+blo, src, sizeof(double)*bn);
            continue;
        }
        memcpy(full+blo, depv[CG_SL_TAIL+j].ptr, sizeof(double)*bn);
        ocrDbDestroy(depv[CG_SL_TAIL+j].guid);
    }

    u64 nchunk = priv->nchunk;
    u64 residual = priv->residual;
    u64 first = (priv->inner == 0);
    ocrGuid_t slice[CG_DIST_MAX_CHUNKS];
    for(j = 0; j < nchunk; ++j) slice[j] = priv->slice[j];
    ocrGuid_t chunkTML = priv->chunkTML;
    ocrGuid_t joinTML = priv->joinTML;
    ocrGuid_t timer = priv->timer;
    ocrHint_t hnt = priv->myHNT;

    ocrGuid_t join;
    ocrEdtCreate(&join, joinTML, 0, NULL, CG_SL_TAIL+nchunk, NULL, 0, &hnt, NULL);

    ocrDbRelease(depv[CG_SL_PRIV].guid);
    ocrDbRelease(depv[CG_SL_VEC].guid);
    cg_release(timer);
    cg_release(depv[CG_SL_RED].guid);

    u64 k;
    for(k = 0; k < nchunk; ++k) {
        u64 pv[5];
        pv[0] = nloc;
        pv[1] = k*nloc/nchunk;
        pv[2] = (k+1)*nloc/nchunk;
        pv[3] = residual;
        pv[4] = first;
        ocrGuid_t chunk, done;
        ocrEdtCreate(&chunk, chunkTML, 5, pv, 2, NULL, 0, &hnt, &done);
        ocrAddDependence(depv[CG_SL_VEC].guid, chunk, 0, DB_MODE_RW);
        ocrAddDependence(slice[k], chunk, 1, DB_MODE_RO);
        ocrAddDependence(done, join, CG_SL_TAIL+k, DB_MODE_RO);
    }

    cg_chain_wire(join, depv[CG_SL_PRIV].guid, depv[CG_SL_VEC].guid, timer,
                  depv[CG_SL_RED].guid);

    return NULL_GUID;
}

ocrGuid_t cg_alpha_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    cg_dist_private_t* priv = (cg_dist_private_t*)depv[CG_SL_PRIV].ptr;
    double* v = (double*)depv[CG_SL_VEC].ptr;
    reductionPrivate_t* rp = (reductionPrivate_t*)depv[CG_SL_RED].ptr;
    double* res = (double*)depv[CG_SL_TAIL].ptr;

    u64 nloc = priv->nloc;
    double* z = cg_vec_z(v, nloc);
    double* r = cg_vec_r(v, nloc);
    double* p = cg_vec_p(v, nloc);
    double* q = cg_vec_q(v, nloc);

    if(priv->inner == 0)
        priv->rho = res[1];
    double alpha = priv->rho / res[0];
    ocrDbDestroy(depv[CG_SL_TAIL].guid);

    __daxpy((u32)nloc, z, alpha, p, z);

    ocrGuid_t timer = priv->timer;
    ocrHint_t hnt = priv->myHNT;
    ocrGuid_t edt;

    if(priv->inner + 1 < CG_DIST_CGITMAX) {
        __daxpy((u32)nloc, r, -alpha, q, r);
        double partial[CG_DIST_REDUCE_NDATA];
        partial[0] = _dot((u32)nloc, r, r);
        partial[1] = 0;
        partial[2] = 0;
        ocrGuid_t returnEVT = priv->returnEVT;
        ocrEdtCreate(&edt, priv->betaTML, 0, NULL, 5, NULL, 0, &hnt, NULL);
        ocrDbRelease(depv[CG_SL_PRIV].guid);
        ocrDbRelease(depv[CG_SL_VEC].guid);
        cg_release(timer);
        cg_chain_wire(edt, depv[CG_SL_PRIV].guid, depv[CG_SL_VEC].guid, timer,
                      depv[CG_SL_RED].guid);
        ocrAddDependence(returnEVT, edt, CG_SL_TAIL, DB_MODE_RO);
        reductionLaunch(rp, depv[CG_SL_RED].guid, partial);
        return NULL_GUID;
    }

    /* Last inner iteration: r, rho and p are dead, so the pass moves straight
       to the residual product r = A z. */
    priv->residual = 1;
    ocrEdtCreate(&edt, priv->bcastTML, 0, NULL, 4, NULL, 0, &hnt, NULL);
    ocrDbRelease(depv[CG_SL_PRIV].guid);
    ocrDbRelease(depv[CG_SL_VEC].guid);
    cg_release(timer);
    cg_release(depv[CG_SL_RED].guid);
    cg_chain_wire(edt, depv[CG_SL_PRIV].guid, depv[CG_SL_VEC].guid, timer,
                  depv[CG_SL_RED].guid);

    return NULL_GUID;
}

ocrGuid_t cg_beta_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    cg_dist_private_t* priv = (cg_dist_private_t*)depv[CG_SL_PRIV].ptr;
    double* v = (double*)depv[CG_SL_VEC].ptr;
    double* res = (double*)depv[CG_SL_TAIL].ptr;

    u64 nloc = priv->nloc;
    double* r = cg_vec_r(v, nloc);
    double* p = cg_vec_p(v, nloc);

    double rho = res[0];
    ocrDbDestroy(depv[CG_SL_TAIL].guid);
    double beta = rho / priv->rho;
    priv->rho = rho;
    __daxpy((u32)nloc, p, beta, p, r);
    priv->inner++;

    ocrGuid_t timer = priv->timer;
    ocrGuid_t edt;
    ocrEdtCreate(&edt, priv->bcastTML, 0, NULL, 4, NULL, 0, &priv->myHNT, NULL);
    ocrDbRelease(depv[CG_SL_PRIV].guid);
    ocrDbRelease(depv[CG_SL_VEC].guid);
    cg_release(timer);
    cg_release(depv[CG_SL_RED].guid);
    cg_chain_wire(edt, depv[CG_SL_PRIV].guid, depv[CG_SL_VEC].guid, timer,
                  depv[CG_SL_RED].guid);

    return NULL_GUID;
}

ocrGuid_t cg_outer_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    cg_dist_private_t* priv = (cg_dist_private_t*)depv[CG_SL_PRIV].ptr;
    double* v = (double*)depv[CG_SL_VEC].ptr;
    reductionPrivate_t* rp = (reductionPrivate_t*)depv[CG_SL_RED].ptr;
    timerdb_t* tdb = (timerdb_t*)depv[CG_SL_TIMER].ptr;
    double* res = (double*)depv[CG_SL_TAIL].ptr;

    u64 nloc = priv->nloc;
    double* x = cg_vec_x(v, nloc);
    double* z = cg_vec_z(v, nloc);

    double rnorm = sqrt(res[0]);
    double xz = res[1];
    double zz = res[2];
    ocrDbDestroy(depv[CG_SL_TAIL].guid);

    priv->residual = 0;
    priv->inner = 0;

    u8 reporter = (priv->myrank == 0);

    if(priv->outer == 0) {
        /* The warm-up pass exists only to touch the code paths; its answers
           are discarded and the measured run starts from x = 1 again. */
        if(reporter) {
            timer_stop(tdb, 0);
            ocrPrintf("Iteration               ||r||               zeta\n");
            timer_start(tdb);
            if(priv->timing_on) timer_start(tdb);
        }
        u64 i;
        for(i = 0; i < nloc; ++i) x[i] = 1;
    }
    else {
        if(reporter && priv->timing_on) timer_stop(tdb, priv->outer+1);
        double zeta = priv->shift + 1/xz;
        double scale = 1.0/sqrt(zz);
        priv->zeta = zeta;
        priv->rnorm = rnorm;
        if(reporter)
            ocrPrintf("%9lu, %20.13f %10.13f\n", priv->outer, rnorm, zeta);
        __scale((u32)nloc, x, scale, z);
        if(reporter && priv->timing_on && priv->outer < priv->niter)
            timer_start(tdb);
    }

    priv->outer++;

    ocrGuid_t timer = priv->timer;
    ocrHint_t hnt = priv->myHNT;
    ocrGuid_t edt;

    if(priv->outer <= priv->niter) {
        ocrEdtCreate(&edt, priv->bcastTML, 0, NULL, 4, NULL, 0, &hnt, NULL);
        ocrDbRelease(depv[CG_SL_PRIV].guid);
        ocrDbRelease(depv[CG_SL_VEC].guid);
        cg_release(timer);
        cg_release(depv[CG_SL_RED].guid);
        cg_chain_wire(edt, depv[CG_SL_PRIV].guid, depv[CG_SL_VEC].guid, timer,
                      depv[CG_SL_RED].guid);
        return NULL_GUID;
    }

    /* One last collective so that no rank is still running when the reporting
       rank shuts the runtime down. */
    double partial[CG_DIST_REDUCE_NDATA] = {0, 0, 0};
    ocrGuid_t returnEVT = priv->returnEVT;
    ocrGuid_t classdb = priv->classdb;
    u64 myrank = priv->myrank;

    ocrGuid_t finalOut = NULL_GUID;
    ocrEdtCreate(&edt, priv->finalTML, 0, NULL, 6, NULL, 0, &hnt,
                 myrank == 0 ? &finalOut : NULL);
    if(myrank == 0) {
        ocrGuid_t sdTML, sd;
        ocrEdtTemplateCreate(&sdTML, cg_shutdown_edt, 0, 1);
        ocrEdtCreate(&sd, sdTML, 0, NULL, 1, NULL, 0, &hnt, NULL);
        ocrEdtTemplateDestroy(sdTML);
        ocrAddDependence(finalOut, sd, 0, DB_MODE_NULL);
    }
    ocrDbRelease(depv[CG_SL_PRIV].guid);
    ocrDbRelease(depv[CG_SL_VEC].guid);
    cg_release(timer);
    cg_chain_wire(edt, depv[CG_SL_PRIV].guid, depv[CG_SL_VEC].guid, timer,
                  depv[CG_SL_RED].guid);
    ocrAddDependence(returnEVT, edt, CG_SL_TAIL, DB_MODE_RO);
    ocrAddDependence(classdb, edt, CG_SL_TAIL+1, DB_MODE_RO);

    reductionLaunch(rp, depv[CG_SL_RED].guid, partial);

    return NULL_GUID;
}

ocrGuid_t cg_final_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    cg_dist_private_t* priv = (cg_dist_private_t*)depv[CG_SL_PRIV].ptr;
    timerdb_t* tdb = (timerdb_t*)depv[CG_SL_TIMER].ptr;
    classdb_t* class = (classdb_t*)depv[CG_SL_TAIL+1].ptr;

    ocrDbDestroy(depv[CG_SL_TAIL].guid);

    if(priv->myrank == 0) {
        timer_stop(tdb, 1);
        ocrPrintf("Benchmark completed\n");

        double zeta = priv->zeta;
        double err = fabs(zeta-class->zvv)/class->zvv;
#ifdef TG_ARCH
        if(err<=1e-3) {
#else
        if(err<=1e-8) {
#endif
            double mflops = 0;
            double t_bench = timer_read(tdb,1);
            if(t_bench)
                mflops = 2*class->niter*class->na*
                         (((double)3+class->nonzer*(class->nonzer+1))+((double)25*(5+class->nonzer*(class->nonzer+1))+3))/
                         t_bench/1000000;
            ocrPrintf("Verification SUCCESSFUL (zeta=%.13f, error=%.13f\n", zeta, err);
            print_results(class, t_bench, mflops);

            if(class->on) {
                ocrPrintf("Timing (sec,%%)\n");
                double t_cg = 0;
                int it;
                for(it = 1; it<=class->niter; ++it)
                    t_cg += timer_read(tdb,it+1);
                double t_init = timer_read(tdb,0);
                double wall = t_init+timer_read(tdb,1);
                ocrPrintf("init:      %10.3f (%3.1f%%)\n", t_init, t_init/wall*100);
                ocrPrintf("cg:        %10.3f (%3.1f%%)\n", t_cg, t_cg/wall*100);
                ocrPrintf("norm:      %10.3f (%3.1f%%)\n", (t_bench-t_cg), (t_bench-t_cg)/wall*100);
                ocrPrintf("wall time: %10.3f\n", wall);
            }
        }
        else
            ocrPrintf("Verification FAILED (zeta=%.13f, correct zeta=%.13f\n", zeta, class->zvv);

        ocrDbDestroy(depv[CG_SL_TAIL+1].guid);
        ocrDbDestroy(depv[CG_SL_TIMER].guid);
        ocrPrintf("DONE... going for shutdown\n");
    }

    { u64 k; for(k = 0; k < priv->nchunk; ++k) ocrDbDestroy(priv->slice[k]); }
    ocrDbDestroy(depv[CG_SL_VEC].guid);
    ocrDbDestroy(depv[CG_SL_RED].guid);
    ocrDbDestroy(depv[CG_SL_PRIV].guid);

    return NULL_GUID;
}

ocrGuid_t cg_shutdown_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    ocrShutdown();
    return NULL_GUID;
}
