#ifndef CG_GEN_H
#define CG_GEN_H

#include <ocr.h>

/* Parallel construction of the generated sparse matrix, shared by every
 * decomposition of this benchmark.
 *
 * The published generator is one serial sweep: for every source row it walks
 * the entries that row generated and, for each, scatters a row of
 * contributions into whatever destination row that entry names, inserting
 * each into a column-sorted list.  The sweep cannot be split by source row --
 * two sources write the same destination -- which is why the serial form
 * looks inherent.
 *
 * Inverting the pairs removes it.  For every destination row, list the
 * (source row, entry) pairs that name it; building that index costs one pass
 * over the generated entries, not over the contributions those entries expand
 * to.  Each destination row is then assembled alone, by exactly the
 * insertions the serial sweep would have performed on it and in the same
 * order, so every row comes out bit-identical to the serial form with no
 * exchange, no buffering and no sort.
 *
 * What stays serial is the draw stream -- rejection sampled, so a row's draw
 * count is data dependent and no jump-ahead into the generator exists -- and
 * the running scale factor, a product accumulated in row order.  Both are
 * linear in the generated entry count, against a construction quadratic in
 * the entries per row.
 */

#define CG_GEN_ABSENT 0xFFFFFFFFu

/* An index slot names one generated entry: its source row and its position
   within that row's entries. */
#define CG_GEN_SRC(pk)   ((u64)((u64)(pk) >> 32))
#define CG_GEN_ENT(pk)   ((u64)((pk) & 0xffffffffu))
#define CG_GEN_PAIR(i,j) (((u64)(i) << 32) | (u64)(j))

/* The generated entries and the inverse index over a contiguous range of
   destination rows.  Written once, read-only afterwards.

   The index is bucketed: destination rows [lo, hi) are cut into nbucket
   consecutive buckets and each of nscan scanners, owning a contiguous range
   of SOURCE rows, deposits its own pairs into its own reserved region of idx.
   A bucket's pairs are therefore the concatenation, over scanners in order,
   of nscan contiguous segments -- and because scanner ranges are ordered by
   source row, that concatenation is in source order, which is what makes a
   row's assembly replay the serial sweep exactly. */
typedef struct {
    ocrGuid_t arow;    /* u32[na]                    entries of source row i  */
    ocrGuid_t rowoff;  /* u64[na+1]                  where row i's entries start */
    ocrGuid_t acol;    /* u32[np]                    destination row of an entry */
    ocrGuid_t aelt;    /* double[np]                 value of an entry        */
    ocrGuid_t scale;   /* double[na]                 running scale at row i   */
    ocrGuid_t idx;     /* u64[np]                    the pairs, bucketed      */
    ocrGuid_t bktoff;  /* u64[nscan*(nbucket+1)]     one scanner's bucket bounds */
    u64 na;
    u64 np;            /* generated entries in total                          */
    u64 lo, hi;        /* destination rows this index covers                  */
    u64 nbucket;
    u64 nscan;
} cg_gen_t;

/* The same arrays as the holder of the acquisitions sees them. */
typedef struct {
    u32*    arow;
    u64*    rowoff;
    u32*    acol;
    double* aelt;
    double* scale;
    u64*    idx;
    u64*    bktoff;
} cg_gen_ptr_t;

/* Dependence slots of a scanner or a builder, in this order. */
#define CG_GEN_DEPC 7

/* What a builder hands back: where its rows landed and how many entries
   survived the duplicate merge. */
typedef struct {
    ocrGuid_t guid;
    u64 nnz;
} cg_gen_ref_t;

/* Parameter slots.  A scanner's unit is its source range, a builder's unit is
   its destination bucket; everything else describes the index they share. */
enum {
    CG_GEN_P_UNIT = 0,
    CG_GEN_P_SHIFT,
    CG_GEN_P_NA,
    CG_GEN_P_NBUCKET,
    CG_GEN_P_NSCAN,
    CG_GEN_P_LO,
    CG_GEN_P_HI,
    CG_GEN_PARAMC
};
/* A task that must also dispose of the arrays carries their guids after the
   descriptor. */
#define CG_GEN_FULL_PARAMC (CG_GEN_PARAMC + CG_GEN_DEPC)

/* Which bucket a destination row falls in.  Bucket b spans
   [lo + b*nloc/nbucket, lo + (b+1)*nloc/nbucket), so the quotient is within
   one of the answer and one step corrects it. */
static inline u64 cg_gen_bucket_of(u64 local, u64 nloc, u64 nbucket)
{
    u64 b = (local * nbucket) / nloc;
    if(b + 1 < nbucket && ((b+1)*nloc)/nbucket <= local) ++b;
    return b;
}

static inline u64 cg_gen_bucket_lo(const cg_gen_t* g, u64 b)
{
    return g->lo + b*(g->hi - g->lo)/g->nbucket;
}

static inline u64 cg_gen_bucket_hi(const cg_gen_t* g, u64 b)
{
    return g->lo + (b+1)*(g->hi - g->lo)/g->nbucket;
}

static inline void cg_gen_guids(const cg_gen_t* g, ocrGuid_t out[CG_GEN_DEPC])
{
    out[0] = g->arow;  out[1] = g->rowoff; out[2] = g->acol; out[3] = g->aelt;
    out[4] = g->scale; out[5] = g->idx;    out[6] = g->bktoff;
}

static inline void cg_gen_ptr_from_dep(cg_gen_ptr_t* p, ocrEdtDep_t depv[])
{
    p->arow   = (u32*)    depv[0].ptr;
    p->rowoff = (u64*)    depv[1].ptr;
    p->acol   = (u32*)    depv[2].ptr;
    p->aelt   = (double*) depv[3].ptr;
    p->scale  = (double*) depv[4].ptr;
    p->idx    = (u64*)    depv[5].ptr;
    p->bktoff = (u64*)    depv[6].ptr;
}

/* Draws the entries and the running scale, and rejects a problem whose
   entries exceed the admission bound.  On success the five arrays are held
   acquired by the caller. */
int  cg_gen_draw(u64 na, u32 nonzer, cg_gen_t* g, cg_gen_ptr_t* p);

/* Reserves the inverse index over destination rows [lo, hi) for nbucket
   buckets and nscan scanners.  idx is reserved per scanner at that scanner's
   whole entry count, an upper bound on what it contributes to the band; the
   surplus is never touched. */
void cg_gen_index_alloc(cg_gen_t* g, cg_gen_ptr_t* p,
                        u64 lo, u64 hi, u64 nbucket, u64 nscan);

/* Widest scanner count whose per-scanner bucket histogram is still cheaper
   than the scan it divides. */
u64  cg_gen_scan_width(u64 np, u64 nbucket);

/* One scanner's share of the index: two passes over its own source rows. */
void cg_gen_index_range(const cg_gen_t* g, const cg_gen_ptr_t* p, u64 w);

void cg_gen_release(const cg_gen_t* g);
void cg_gen_destroy(const cg_gen_t* g);

void cg_gen_pack(const cg_gen_t* g, u64 unit, double shift, u64* pv);
void cg_gen_unpack(cg_gen_t* g, double* shift, const u64* pv);
void cg_gen_pack_full(const cg_gen_t* g, u64 unit, double shift, u64* pv);
void cg_gen_unpack_full(cg_gen_t* g, double* shift, const u64* pv);

/* Pairs in one bucket, and the same pairs grouped by destination row.  The
   grouping is stable, so a row keeps the source order the serial sweep
   visited it in; cap[] is the slot count that row reserves. */
u64  cg_gen_bucket_size(const cg_gen_t* g, const cg_gen_ptr_t* p, u64 b);
void cg_gen_bucket_group(const cg_gen_t* g, const cg_gen_ptr_t* p, u64 b,
                         u64 blo, u64 nrow,
                         u64* pair, u64* ptr, u32* cap, u64* cur);

/* Assembles one destination row from the pairs pair[lo, hi) into a workspace
   of cap column-sorted slots, and reports how many entries survived the
   duplicate merge. */
u32  cg_gen_row_build(const cg_gen_ptr_t* p, double shift, u64 r,
                      const u64* pair, u64 lo, u64 hi, u32 cap,
                      u32* wcol, double* wval);

/* Creates one task per bucket and wires each completion into a join slot. */
void cg_gen_spawn(const cg_gen_t* g, double shift, ocrEdt_t fn,
                  ocrGuid_t join, u32 firstSlot, ocrHint_t* hnt);

/* Creates one scanner per source range and wires each completion into a join
   slot.  Scanners take the index arrays RW: they write disjoint ranges of
   them, so they must all run inside one policy domain. */
void cg_gen_spawn_scan(const cg_gen_t* g, double shift, ocrGuid_t join,
                       u32 firstSlot, ocrHint_t* hnt);

ocrGuid_t cg_gen_index_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

#endif
