#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ocr.h>

#include "cg_ocr.h"
#include "cg_gen.h"

// generate nz values for different indexes, which are stored in iv,
// and ensures that the index di is present or adds it with value dv,
// in which case there will be nz+1 values and nz is incremented accordingly
void sprnvc(u32 n, u32* nz, u32 nn1, double* v, u32* iv, u32 di, double dv, randdb_t* randdb)
{
    int nzv = 0;
    while(nzv<*nz) {
        u32 i=n;
        while(i>=n)
            i = randi(randdb, nn1);
        u32 r = 0;
        while(r<nzv && iv[r]!=i) ++r;
        if(r==nzv) {
            v[nzv] = randd(randdb);
            iv[nzv] = i;
            ++nzv;
            if(i==di)
                di=CG_GEN_ABSENT;
        }
    }
    if(di<CG_GEN_ABSENT) {
        v[*nz] = dv;
        iv[*nz] = di;
        ++*nz;
    }
}

int cg_gen_draw(u64 na, u32 nonzer, cg_gen_t* g, cg_gen_ptr_t* p)
{
    u64 k1 = (u64)nonzer + 1;
    u32 nn1;
    for(nn1 = 1; nn1 < na; nn1 <<= 1);

    g->na = na;
    g->np = 0;
    g->lo = 0; g->hi = 0; g->nbucket = 0; g->nscan = 0;
    g->idx = NULL_GUID; g->bktoff = NULL_GUID;
    ocrDbCreate(&g->arow,   (void**)&p->arow,   sizeof(u32)*na,       0, NULL_HINT, NO_ALLOC);
    ocrDbCreate(&g->rowoff, (void**)&p->rowoff, sizeof(u64)*(na+1),   0, NULL_HINT, NO_ALLOC);
    ocrDbCreate(&g->acol,   (void**)&p->acol,   sizeof(u32)*na*k1,    0, NULL_HINT, NO_ALLOC);
    ocrDbCreate(&g->aelt,   (void**)&p->aelt,   sizeof(double)*na*k1, 0, NULL_HINT, NO_ALLOC);
    ocrDbCreate(&g->scale,  (void**)&p->scale,  sizeof(double)*na,    0, NULL_HINT, NO_ALLOC);

    randdb_t* rnd; ocrGuid_t rndid;
    rand_init(&rnd, &rndid);

    u64 i, off = 0;
    for(i = 0; i < na; ++i) {
        p->rowoff[i] = off;
        p->arow[i] = nonzer;
        sprnvc((u32)na, p->arow+i, nn1, p->aelt+off, p->acol+off, (u32)i, 0.5, rnd);
        off += p->arow[i];
    }
    p->rowoff[na] = off;
    g->np = off;
    ocrDbDestroy(rndid);

    /* the running scale is a product in row order, so it is carried, not
       recomputed per row */
    double size = 1.0, ratio = pow(0.1, 1.0/(double)na);
    for(i = 0; i < na; ++i) { p->scale[i] = size; size *= ratio; }

    /* the admission bound is on the whole matrix: a source row contributes
       its own entry count once per entry it generated */
    u64 nza = 0;
    for(i = 0; i < na; ++i) nza += (u64)p->arow[i]*p->arow[i];
    if(nza > na*k1*k1) {
        ocrPrintf("Space for matrix elements exceeded in sparse %lu > %lu\n", nza, na*k1*k1);
        ocrDbDestroy(g->arow);  ocrDbDestroy(g->rowoff); ocrDbDestroy(g->acol);
        ocrDbDestroy(g->aelt);  ocrDbDestroy(g->scale);
        return -1;
    }
    return 0;
}

u64 cg_gen_scan_width(u64 np, u64 nbucket)
{
    u64 w = np/(2*(nbucket ? nbucket : 1));
    if(w > nbucket) w = nbucket;
    if(w < 1) w = 1;
    return w;
}

void cg_gen_index_alloc(cg_gen_t* g, cg_gen_ptr_t* p,
                        u64 lo, u64 hi, u64 nbucket, u64 nscan)
{
    g->lo = lo; g->hi = hi; g->nbucket = nbucket; g->nscan = nscan;
    ocrDbCreate(&g->idx, (void**)&p->idx,
                sizeof(u64)*(g->np ? g->np : 1), 0, NULL_HINT, NO_ALLOC);
    ocrDbCreate(&g->bktoff, (void**)&p->bktoff,
                sizeof(u64)*nscan*(nbucket+1), 0, NULL_HINT, NO_ALLOC);
}

void cg_gen_index_range(const cg_gen_t* g, const cg_gen_ptr_t* p, u64 w)
{
    u64 nbucket = g->nbucket, nloc = g->hi - g->lo;
    u64 slo = w*g->na/g->nscan, shi = (w+1)*g->na/g->nscan;
    u64* off = p->bktoff + w*(nbucket+1);

    ocrGuid_t curid; u64* cur;
    ocrDbCreate(&curid, (void**)&cur, sizeof(u64)*nbucket, 0, NULL_HINT, NO_ALLOC);

    u64 i, b;
    u32 j;
    for(b = 0; b < nbucket; ++b) cur[b] = 0;
    for(i = slo; i < shi; ++i) {
        const u32* aci = p->acol + p->rowoff[i];
        for(j = 0; j < p->arow[i]; ++j) {
            u64 d = aci[j];
            if(d < g->lo || d >= g->hi) continue;
            cur[cg_gen_bucket_of(d - g->lo, nloc, nbucket)] += 1;
        }
    }
    /* a scanner deposits only inside its own reserved region, so no two
       scanners ever address the same slot of idx */
    u64 run = p->rowoff[slo];
    for(b = 0; b < nbucket; ++b) { u64 c = cur[b]; off[b] = run; cur[b] = run; run += c; }
    off[nbucket] = run;

    for(i = slo; i < shi; ++i) {
        const u32* aci = p->acol + p->rowoff[i];
        for(j = 0; j < p->arow[i]; ++j) {
            u64 d = aci[j];
            if(d < g->lo || d >= g->hi) continue;
            p->idx[cur[cg_gen_bucket_of(d - g->lo, nloc, nbucket)]++] = CG_GEN_PAIR(i, j);
        }
    }
    ocrDbDestroy(curid);
}

void cg_gen_release(const cg_gen_t* g)
{
    ocrGuid_t dep[CG_GEN_DEPC];
    u32 d;
    cg_gen_guids(g, dep);
    for(d = 0; d < CG_GEN_DEPC; ++d) ocrDbRelease(dep[d]);
}

void cg_gen_destroy(const cg_gen_t* g)
{
    ocrGuid_t dep[CG_GEN_DEPC];
    u32 d;
    cg_gen_guids(g, dep);
    for(d = 0; d < CG_GEN_DEPC; ++d) ocrDbDestroy(dep[d]);
}

void cg_gen_pack(const cg_gen_t* g, u64 unit, double shift, u64* pv)
{
    pv[CG_GEN_P_UNIT]    = unit;
    memcpy(pv+CG_GEN_P_SHIFT, &shift, sizeof(double));
    pv[CG_GEN_P_NA]      = g->na;
    pv[CG_GEN_P_NBUCKET] = g->nbucket;
    pv[CG_GEN_P_NSCAN]   = g->nscan;
    pv[CG_GEN_P_LO]      = g->lo;
    pv[CG_GEN_P_HI]      = g->hi;
}

void cg_gen_unpack(cg_gen_t* g, double* shift, const u64* pv)
{
    memcpy(shift, pv+CG_GEN_P_SHIFT, sizeof(double));
    g->na      = pv[CG_GEN_P_NA];
    g->nbucket = pv[CG_GEN_P_NBUCKET];
    g->nscan   = pv[CG_GEN_P_NSCAN];
    g->lo      = pv[CG_GEN_P_LO];
    g->hi      = pv[CG_GEN_P_HI];
    g->np      = 0;
}

void cg_gen_pack_full(const cg_gen_t* g, u64 unit, double shift, u64* pv)
{
    ocrGuid_t dep[CG_GEN_DEPC];
    u32 d;
    cg_gen_pack(g, unit, shift, pv);
    cg_gen_guids(g, dep);
    for(d = 0; d < CG_GEN_DEPC; ++d) pv[CG_GEN_PARAMC+d] = (u64)dep[d].guid;
}

void cg_gen_unpack_full(cg_gen_t* g, double* shift, const u64* pv)
{
    ocrGuid_t dep[CG_GEN_DEPC];
    u32 d;
    cg_gen_unpack(g, shift, pv);
    for(d = 0; d < CG_GEN_DEPC; ++d) dep[d].guid = (intptr_t)pv[CG_GEN_PARAMC+d];
    g->arow = dep[0]; g->rowoff = dep[1]; g->acol = dep[2]; g->aelt = dep[3];
    g->scale = dep[4]; g->idx = dep[5]; g->bktoff = dep[6];
}

u64 cg_gen_bucket_size(const cg_gen_t* g, const cg_gen_ptr_t* p, u64 b)
{
    u64 w, n = 0;
    for(w = 0; w < g->nscan; ++w) {
        const u64* off = p->bktoff + w*(g->nbucket+1);
        n += off[b+1] - off[b];
    }
    return n;
}

void cg_gen_bucket_group(const cg_gen_t* g, const cg_gen_ptr_t* p, u64 b,
                         u64 blo, u64 nrow,
                         u64* pair, u64* ptr, u32* cap, u64* cur)
{
    u64 w, k, r;
    for(r = 0; r <= nrow; ++r) ptr[r] = 0;
    for(r = 0; r < nrow; ++r) cap[r] = 0;

    for(w = 0; w < g->nscan; ++w) {
        const u64* off = p->bktoff + w*(g->nbucket+1);
        for(k = off[b]; k < off[b+1]; ++k) {
            u64 pk = p->idx[k], i = CG_GEN_SRC(pk);
            u64 d = p->acol[p->rowoff[i] + CG_GEN_ENT(pk)];
            ptr[d-blo+1] += 1;
            cap[d-blo] += p->arow[i];
        }
    }
    for(r = 1; r <= nrow; ++r) ptr[r] += ptr[r-1];
    for(r = 0; r < nrow; ++r) cur[r] = ptr[r];

    /* scanners are walked in order and each scanner's segment is in source
       order, so a row's pairs come out in the order the serial sweep visited
       them */
    for(w = 0; w < g->nscan; ++w) {
        const u64* off = p->bktoff + w*(g->nbucket+1);
        for(k = off[b]; k < off[b+1]; ++k) {
            u64 pk = p->idx[k], i = CG_GEN_SRC(pk);
            u64 d = p->acol[p->rowoff[i] + CG_GEN_ENT(pk)];
            pair[cur[d-blo]++] = pk;
        }
    }
}

u32 cg_gen_row_build(const cg_gen_ptr_t* p, double shift, u64 r,
                     const u64* pair, u64 lo, u64 hi, u32 cap,
                     u32* wcol, double* wval)
{
    const double rcond = 0.1;
    u32 vn = cap, t, cnt = vn;
    u64 q, jj;
    for(t = 0; t < vn; ++t) { wcol[t] = CG_GEN_ABSENT; wval[t] = 0.0; }

    for(q = lo; q < hi; ++q) {
        u64 pk = pair[q], i = CG_GEN_SRC(pk), j = CG_GEN_ENT(pk);
        const u32* aci = p->acol + p->rowoff[i];
        const double* aei = p->aelt + p->rowoff[i];
        double sc0 = p->scale[i] * aei[j];
        for(jj = 0; jj < p->arow[i]; ++jj) {
            u32 j1 = aci[jj];
            double va = aei[jj] * sc0;
            u32 k;
            if(j1 == r && j1 == i) va += rcond - shift;
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
    return cnt;
}

void cg_gen_spawn(const cg_gen_t* g, double shift, ocrEdt_t fn,
                  ocrGuid_t join, u32 firstSlot, ocrHint_t* hnt)
{
    ocrGuid_t dep[CG_GEN_DEPC], tml;
    u64 b;
    cg_gen_guids(g, dep);
    ocrEdtTemplateCreate(&tml, fn, CG_GEN_PARAMC, CG_GEN_DEPC);
    for(b = 0; b < g->nbucket; ++b) {
        u64 pv[CG_GEN_PARAMC];
        ocrGuid_t st, done;
        u32 d;
        cg_gen_pack(g, b, shift, pv);
        ocrEdtCreate(&st, tml, CG_GEN_PARAMC, pv, CG_GEN_DEPC, NULL, 0, hnt, &done);
        ocrAddDependence(done, join, firstSlot + (u32)b, DB_MODE_RO);
        for(d = 0; d < CG_GEN_DEPC; ++d)
            ocrAddDependence(dep[d], st, d, DB_MODE_RO);
    }
    ocrEdtTemplateDestroy(tml);
}

void cg_gen_spawn_scan(const cg_gen_t* g, double shift, ocrGuid_t join,
                       u32 firstSlot, ocrHint_t* hnt)
{
    ocrGuid_t dep[CG_GEN_DEPC], tml;
    u64 w;
    cg_gen_guids(g, dep);
    ocrEdtTemplateCreate(&tml, cg_gen_index_edt, CG_GEN_PARAMC, CG_GEN_DEPC);
    for(w = 0; w < g->nscan; ++w) {
        u64 pv[CG_GEN_PARAMC];
        ocrGuid_t st, done;
        u32 d;
        cg_gen_pack(g, w, shift, pv);
        ocrEdtCreate(&st, tml, CG_GEN_PARAMC, pv, CG_GEN_DEPC, NULL, 0, hnt, &done);
        ocrAddDependence(done, join, firstSlot + (u32)w, DB_MODE_NULL);
        for(d = 0; d < CG_GEN_DEPC; ++d)
            ocrAddDependence(dep[d], st, d, d >= 5 ? DB_MODE_RW : DB_MODE_RO);
    }
    ocrEdtTemplateDestroy(tml);
}

ocrGuid_t cg_gen_index_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    cg_gen_t g; cg_gen_ptr_t p; double shift;
    cg_gen_unpack(&g, &shift, paramv);
    cg_gen_ptr_from_dep(&p, depv);
    cg_gen_index_range(&g, &p, paramv[CG_GEN_P_UNIT]);
    return NULL_GUID;
}

/* One row block of the blocked layout the block-fork decomposition reads.
   As u32 words: blk per-row entry counts, one word holding where the column
   indices start, padding to an even word so the values are 8-byte aligned,
   then the values and then the column indices, both contiguous over the
   block's rows. */
ocrGuid_t cg_block_build_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
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

    u64 counts = 0, maxcap = 1;
    for(r = 0; r < nrow; ++r) {
        counts += cap[r];
        if(cap[r] > maxcap) maxcap = cap[r];
    }

    ocrGuid_t wkid; char* wk;
    ocrDbCreate(&wkid, (void**)&wk, maxcap*(sizeof(double)+sizeof(u32)),
                0, NULL_HINT, NO_ALLOC);
    double* wval = (double*)wk;
    u32* wcol = (u32*)(wk + maxcap*sizeof(double));

    u32 even = (u32)nrow + 1 + ((nrow & 1) ? 0 : 1);
    ocrGuid_t blkid; u32* rows;
    ocrDbCreate(&blkid, (void**)&rows,
                sizeof(double)*counts + sizeof(u32)*(counts+even),
                0, NULL_HINT, NO_ALLOC);
    double* vals = (double*)(rows + even);
    u64 reserved = (u64)even + 2*counts;
    u64 pos = 0;
    for(r = 0; r < nrow; ++r) {
        u32 cnt = cg_gen_row_build(&p, shift, lo+r, pair, ptr[r], ptr[r+1],
                                   cap[r], wcol, wval);
        memcpy(vals + pos, wval, sizeof(double)*cnt);
        memcpy(rows + reserved + pos, wcol, sizeof(u32)*cnt);
        rows[r] = cnt;
        pos += cnt;
    }
    /* the column indices were laid down behind the reserved value slots; only
       the surviving values are kept, so they move down against them */
    memmove(rows + even + 2*pos, rows + reserved, sizeof(u32)*pos);
    rows[nrow] = (u32)(even + 2*pos);
    ocrDbRelease(blkid);

    ocrDbDestroy(wkid);
    ocrDbDestroy(wsid);

    ocrGuid_t out; cg_gen_ref_t* ref;
    ocrDbCreate(&out, (void**)&ref, sizeof(cg_gen_ref_t), 0, NULL_HINT, NO_ALLOC);
    ref->guid = blkid;
    ref->nnz = pos;
    return out;
}
