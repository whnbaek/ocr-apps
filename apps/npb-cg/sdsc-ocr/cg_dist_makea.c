/* The row-band decomposition's half of the matrix construction: one CSR image
 * per row slice, built by the task that will multiply it.  Everything the
 * build needs -- the drawn entries, the running scale and the inverse index
 * that names, per destination row, the (source row, entry) pairs contributing
 * to it -- is the shared construction in cg_gen.h.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <ocr.h>

#include "cg_ocr.h"
#include "cg_gen.h"
#include "cg_dist.h"

/* Builds one contiguous run of rows into its own CSR datablock, performing
   exactly the insertions the serial sweep would have performed on those rows,
   in the same order. */
ocrGuid_t cg_build_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    cg_gen_t g; cg_gen_ptr_t p; double shift;
    cg_gen_unpack(&g, &shift, paramv);
    cg_gen_ptr_from_dep(&p, depv);

    u64 b = paramv[CG_GEN_P_UNIT];
    u64 lo = cg_gen_bucket_lo(&g, b), hi = cg_gen_bucket_hi(&g, b);
    u64 nrow = hi - lo, r;
    u64 npair = cg_gen_bucket_size(&g, &p, b);

    ocrGuid_t wsid; char* ws;
    u64 pairb = sizeof(u64)*(npair ? npair : 1);
    u64 ptrb  = sizeof(u64)*(nrow+1);
    /* the cursor array is 8-byte wide, so the slot count array before it is
       padded to keep it aligned */
    u64 capb  = (sizeof(u32)*(nrow ? nrow : 1) + 7) & ~(u64)7;
    u64 curb  = sizeof(u64)*(nrow ? nrow : 1);
    ocrDbCreate(&wsid, (void**)&ws, pairb+ptrb+capb+curb, 0, NULL_HINT, NO_ALLOC);
    u64* pair = (u64*)ws;
    u64* ptr  = (u64*)(ws + pairb);
    u32* cap  = (u32*)(ws + pairb + ptrb);
    u64* cur  = (u64*)(ws + pairb + ptrb + capb);
    cg_gen_bucket_group(&g, &p, b, lo, nrow, pair, ptr, cap, cur);

    /* The reserved slots are an upper bound: only merged duplicates lower the
       count, and there are few, so the slice is allocated at the bound and
       carries the slack rather than being copied once to shrink it. */
    u64 capsum = 0, maxcap = 1;
    for(r = 0; r < nrow; ++r) {
        capsum += cap[r];
        if(cap[r] > maxcap) maxcap = cap[r];
    }

    /* One row is assembled at a time, so the workspace is a row's reserved
       slots, not a slice's. */
    ocrGuid_t wkid; char* wk;
    ocrDbCreate(&wkid, (void**)&wk, maxcap*(sizeof(double)+sizeof(u32)),
                0, NULL_HINT, NO_ALLOC);
    double* wval = (double*)wk;
    u32* wcol = (u32*)(wk + maxcap*sizeof(double));

    cg_csr_t* sl; ocrGuid_t slid;
    ocrDbCreate(&slid, (void**)&sl, cg_csr_bytes(nrow, capsum), 0, NULL_HINT, NO_ALLOC);
    sl->nrow = nrow;
    sl->capacity = capsum;
    double* sv = cg_csr_values(sl);
    u64* sr = cg_csr_rowstr(sl);
    u32* sc = cg_csr_colidx(sl);

    u64 pos = 0;
    for(r = 0; r < nrow; ++r) {
        u32 cnt = cg_gen_row_build(&p, shift, lo+r, pair, ptr[r], ptr[r+1],
                                   cap[r], wcol, wval);
        sr[r] = pos;
        memcpy(sv+pos, wval, sizeof(double)*cnt);
        memcpy(sc+pos, wcol, sizeof(u32)*cnt);
        pos += cnt;
    }
    sr[nrow] = pos;
    sl->nnz = pos;
    ocrDbRelease(slid);

    ocrDbDestroy(wkid);
    ocrDbDestroy(wsid);

    ocrGuid_t out; cg_gen_ref_t* ref;
    ocrDbCreate(&out, (void**)&ref, sizeof(cg_gen_ref_t), 0, NULL_HINT, NO_ALLOC);
    ref->guid = slid;
    ref->nnz = pos;
    return out;
}
