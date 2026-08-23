/* Parallel matrix construction.
 *
 * The published generator is one serial sweep: for every row i it walks the
 * pairs that row generated and, for each, scatters a row of contributions
 * into whatever destination rows those pairs name, inserting each into a
 * sorted-by-column list.  That is where essentially all of the construction
 * time goes -- the random draws that feed it are two per generated entry and
 * cost nothing beside it.  The sweep cannot be split by source row (two
 * sources write the same destination), which is why the serial form looks
 * inherent.
 *
 * It is not.  Invert the pairs: for every destination row, the list of
 * (source row, entry) pairs that name it.  Building that index costs one
 * pass over the generated entries -- na*(nonzer+1) of them, not the
 * na*(nonzer+1)^2 contributions -- and once it exists each destination row
 * can be built alone, by exactly the insertions the serial sweep would have
 * performed on it, in the same order.  So the construction parallelizes over
 * destination rows with no exchange, no buffering and no sort, and every row
 * comes out bit-identical to the serial form.
 *
 * What stays serial is the draw sequence (a rejection-sampled stream, so the
 * draw count per row is not known in advance) and the running scale factor,
 * which is a product accumulated in row order.  Both are O(na*(nonzer+1)).
 * They are not generated once and shipped, though: every rank replays the
 * same stream itself and keeps only the index for the rows it owns, so the
 * replay costs one rank's time no matter how many ranks there are and
 * nothing about the construction crosses the wire.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <ocr.h>

#include "cg_ocr.h"
#include "cg_dist.h"

#define CG_GEN_ABSENT 0xFFFFFFFF

/* The serial prologue, run by every rank on its own: the draws (which every
   rank needs in full -- a row it owns can receive contributions from any
   source row), and then the capacity and the inverse index for the rows in
   [lo, hi) only. */
int cg_gen_prepare(u64 na, u32 nonzer, u64 lo, u64 hi, cg_gen_t* g)
{
    u64 k1 = nonzer + 1;
    u64 nloc = hi - lo;
    u32 nn1;
    for(nn1 = 1; nn1 < na; nn1 <<= 1);

    u32 *arow, *acol, *cap;
    double *aelt, *scale;
    u64 *rowoff, *idxoff, *idx;
    ocrDbCreate(&g->arow,   (void**)&arow,   sizeof(u32)*na,        0, NULL_HINT, NO_ALLOC);
    ocrDbCreate(&g->rowoff, (void**)&rowoff, sizeof(u64)*(na+1),    0, NULL_HINT, NO_ALLOC);
    ocrDbCreate(&g->acol,   (void**)&acol,   sizeof(u32)*na*k1,     0, NULL_HINT, NO_ALLOC);
    ocrDbCreate(&g->aelt,   (void**)&aelt,   sizeof(double)*na*k1,  0, NULL_HINT, NO_ALLOC);
    ocrDbCreate(&g->scale,  (void**)&scale,  sizeof(double)*na,     0, NULL_HINT, NO_ALLOC);
    ocrDbCreate(&g->cap,    (void**)&cap,    sizeof(u32)*nloc,      0, NULL_HINT, NO_ALLOC);
    ocrDbCreate(&g->idxoff, (void**)&idxoff, sizeof(u64)*(nloc+1),  0, NULL_HINT, NO_ALLOC);

    randdb_t* rnd; ocrGuid_t rndid;
    rand_init(&rnd, &rndid);

    u64 i, j, off = 0;
    for(i = 0; i < na; ++i) {
        rowoff[i] = off;
        arow[i] = nonzer;
        sprnvc((u32)na, arow+i, nn1, aelt+off, acol+off, (u32)i, 0.5, rnd);
        off += arow[i];
    }
    rowoff[na] = off;
    ocrDbDestroy(rndid);

    /* the running scale is a product in row order, so it is carried, not
       recomputed per row */
    double size = 1.0, ratio = pow(0.1, 1.0/(double)na);
    for(i = 0; i < na; ++i) { scale[i] = size; size *= ratio; }

    /* slots this rank's rows reserve, and the count of (source, entry) pairs
       naming each of them */
    for(i = 0; i < nloc; ++i) { cap[i] = 0; idxoff[i] = 0; }
    idxoff[nloc] = 0;
    /* the admission bound is on the whole matrix, as in the serial form: one
       band's share of it is not bounded by its share of the rows */
    u64 nza = 0;
    for(i = 0; i < na; ++i) {
        u32* aci = acol + rowoff[i];
        for(j = 0; j < arow[i]; ++j) {
            u64 d = aci[j];
            nza += arow[i];
            if(d < lo || d >= hi) continue;
            cap[d-lo] += arow[i];
            idxoff[d-lo] += 1;
        }
    }
    if(nza > na*k1*k1) {
        ocrPrintf("Space for matrix elements exceeded in sparse %lu > %lu\n", nza, na*k1*k1);
        return -1;
    }

    u64 acc = 0;
    for(i = 0; i < nloc; ++i) { u64 c = idxoff[i]; idxoff[i] = acc; acc += c; }
    idxoff[nloc] = acc;
    ocrDbCreate(&g->idx, (void**)&idx, sizeof(u64)*(acc ? acc : 1), 0, NULL_HINT, NO_ALLOC);

    ocrGuid_t curid; u64* cur;
    ocrDbCreate(&curid, (void**)&cur, sizeof(u64)*(nloc?nloc:1), 0, NULL_HINT, NO_ALLOC);
    for(i = 0; i < nloc; ++i) cur[i] = idxoff[i];
    /* filled in (source, entry) order, so a row's list replays the serial
       sweep's visit order exactly */
    for(i = 0; i < na; ++i) {
        u32* aci = acol + rowoff[i];
        for(j = 0; j < arow[i]; ++j) {
            u64 d = aci[j];
            if(d < lo || d >= hi) continue;
            idx[cur[d-lo]++] = (i << 32) | j;
        }
    }
    ocrDbDestroy(curid);

    ocrDbRelease(g->arow);   ocrDbRelease(g->rowoff); ocrDbRelease(g->acol);
    ocrDbRelease(g->aelt);   ocrDbRelease(g->scale);  ocrDbRelease(g->cap);
    ocrDbRelease(g->idxoff); ocrDbRelease(g->idx);
    return 0;
}

/* Builds one contiguous run of rows into its own CSR datablock, performing
   exactly the insertions the serial sweep would have performed on those
   rows, in the same order.
   paramv: first row, last row (exclusive), the problem's shift, the first row
   of the band the per-rank capacity and index arrays are relative to.
   depv:   0 arow, 1 rowoff, 2 acol, 3 aelt, 4 scale, 5 cap, 6 idxoff, 7 idx. */
ocrGuid_t cg_build_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    u32*    arow   = (u32*)    depv[0].ptr;
    u64*    rowoff = (u64*)    depv[1].ptr;
    u32*    acol   = (u32*)    depv[2].ptr;
    double* aelt   = (double*) depv[3].ptr;
    double* scale  = (double*) depv[4].ptr;
    u32*    cap    = (u32*)    depv[5].ptr;
    u64*    idxoff = (u64*)    depv[6].ptr;
    u64*    idx    = (u64*)    depv[7].ptr;

    u64 lo = paramv[0], hi = paramv[1], blo = paramv[3];
    double shift; memcpy(&shift, paramv+2, sizeof(double));
    const double rcond = 0.1;
    u64 nrow = hi - lo, r, p, jj;

    /* The reserved slots are an upper bound: only merged duplicates lower the
       count, and there are few, so the slice is allocated at the bound and
       carries the slack rather than being copied once to shrink it. */
    u64 capsum = 0, maxcap = 1;
    for(r = lo; r < hi; ++r) {
        capsum += cap[r-blo];
        if(cap[r-blo] > maxcap) maxcap = cap[r-blo];
    }

    /* One row is assembled at a time, so the workspace is a row's reserved
       slots, not a slice's. */
    ocrGuid_t wcolid, wvalid;
    u32* wcol; double* wval;
    ocrDbCreate(&wcolid, (void**)&wcol, sizeof(u32)*maxcap, 0, NULL_HINT, NO_ALLOC);
    ocrDbCreate(&wvalid, (void**)&wval, sizeof(double)*maxcap, 0, NULL_HINT, NO_ALLOC);

    cg_csr_t* sl; ocrGuid_t slid;
    ocrDbCreate(&slid, (void**)&sl, cg_csr_bytes(nrow, capsum), 0, NULL_HINT, NO_ALLOC);
    sl->nrow = nrow;
    sl->capacity = capsum;
    double* sv = cg_csr_values(sl);
    u64* sr = cg_csr_rowstr(sl);
    u32* sc = cg_csr_colidx(sl);

    u64 pos = 0;
    for(r = lo; r < hi; ++r) {
        u32 vn = cap[r-blo], t;
        for(t = 0; t < vn; ++t) { wcol[t] = CG_GEN_ABSENT; wval[t] = 0.0; }
        u32 cnt = vn;

        for(p = idxoff[r-blo]; p < idxoff[r-blo+1]; ++p) {
            u64 pk = idx[p];
            u64 i = pk >> 32, j = pk & 0xffffffffu;
            u32* aci = acol + rowoff[i];
            double* aei = aelt + rowoff[i];
            double sc0 = scale[i] * aei[j];
            for(jj = 0; jj < arow[i]; ++jj) {
                u32 j1 = aci[jj];
                double va = aei[jj] * sc0;
                if(j1 == r && j1 == i) va += rcond - shift;
                u32 k;
                for(k = 0; k < vn; ++k) {
                    if(wcol[k] > j1 && wcol[k] < CG_GEN_ABSENT) {
                        int m;
                        for(m = (int)vn-2; m >= (int)k; --m)
                            if(wcol[m] < CG_GEN_ABSENT) {
                                wval[m+1] = wval[m];
                                wcol[m+1] = wcol[m];
                            }
                        wcol[k] = j1;
                        wval[k] = 0.0;
                        break;
                    }
                    else if(wcol[k] == CG_GEN_ABSENT) { wcol[k] = j1; break; }
                    else if(wcol[k] == j1) { --cnt; break; }
                }
                if(k < vn) wval[k] += va;
            }
        }

        sr[r-lo] = pos;
        memcpy(sv+pos, wval, sizeof(double)*cnt);
        memcpy(sc+pos, wcol, sizeof(u32)*cnt);
        pos += cnt;
    }
    sr[nrow] = pos;
    sl->nnz = pos;
    ocrDbRelease(slid);

    ocrDbDestroy(wcolid);
    ocrDbDestroy(wvalid);

    ocrGuid_t out; cg_slice_ref_t* ref;
    ocrDbCreate(&out, (void**)&ref, sizeof(cg_slice_ref_t), 0, NULL_HINT, NO_ALLOC);
    ref->guid = slid;
    ref->nnz = pos;
    return out;
}
