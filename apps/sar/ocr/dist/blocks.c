/* The image's stripe layer: cutting it into per-rank pieces, and reading a
 * rectangle back out of the pieces that cover it.  See common.h for why the
 * pieces are stripes and why placement travels with them.
 */
#ifndef TG_ARCH
#include <string.h>
#endif

#include "ocr.h"
#include "rag_ocr.h"
#include "common.h"

void img_stripes_layout(struct img_stripes_s *st, int ns, int nr, int iy, int ix,
                        int align, int min_rows) {
	if(nr < 1) nr = 1;
	if(ns < nr) ns = nr;
	if(align < 1) align = 1;
	if(min_rows < align) min_rows = align;
	/* Boundaries on multiples of `align` so a producer band never straddles
	   two stripes; the remainder is spread one unit at a time rather than
	   piled onto the last stripe. */
	const int units = (iy + align - 1) / align;
	/* The bound on the stripe count is the one that keeps the SHORTEST stripe
	   above the footprint, not the one the average height satisfies: the units
	   are spread as evenly as whole units allow, so the shortest stripe is
	   floor(units/ns) of them and a count derived from iy/min_rows admits
	   layouts whose short stripes are well below min_rows. */
	const int min_units = (min_rows + align - 1) / align;
	int max_ns = units / min_units;
	if(max_ns > IMG_MAX_STRIPES) max_ns = IMG_MAX_STRIPES;
	/* Every rank owns a stripe or the phases placed by stripe would run on a
	   subset of the machine without saying so. */
	if(max_ns < nr) {
		ocrPrintf("%d image rows aligned to %d cannot be cut into %d stripes of at"
		          " least %d rows each (at most %d such stripes fit); raise the"
		          " image, lower the block sizes, or lower the rank count.\n",
		          iy, align, nr, min_rows, max_ns);RAG_FLUSH;
		xe_exit(1);
	}
	if(ns > max_ns) ns = max_ns;
	/* A whole multiple of the rank count: a remainder leaves some ranks owning
	   more stripes than others, and the extra one's halo is a neighbour nobody
	   on that rank shares. */
	ns -= ns % nr;
	st->ns = ns;
	st->nr = nr;
	st->iy = iy;
	st->ix = ix;
	int acc = 0;
	for(int s = 0; s < ns; s++) {
		st->y0[s] = acc * align < iy ? acc * align : iy;
		acc += units / ns + (s < units % ns ? 1 : 0);
	}
	st->y0[ns] = iy;
	/* The realized heights, not the bound that chose the count: the last stripe
	   is clipped when the image is not a whole number of units. */
	for(int s = 0; s < ns; s++) {
		if(st->y0[s+1] - st->y0[s] < min_rows) {
			ocrPrintf("stripe %d of %d is %d rows, below the %d a consumer's"
			          " footprint needs; lower the rank count or the block sizes.\n",
			          s, ns, st->y0[s+1] - st->y0[s], min_rows);RAG_FLUSH;
			xe_exit(1);
		}
	}
	for(int s = 0; s < ns; s++) st->g[s] = NULL_GUID;
}

ocrHint_t *rank_edt_hint(ocrHint_t *h, u64 r) {
#ifdef ENABLE_EXTENSION_AFFINITY
	u64 n = 0;
	if(ocrAffinityCount(AFFINITY_PD, &n) != 0 || n == 0) return NULL_HINT;
	ocrGuid_t aff = NULL_GUID;
	if(ocrAffinityGetAt(AFFINITY_PD, r % n, &aff) != 0) return NULL_HINT;
	ocrHintInit(h, OCR_HINT_EDT_T);
	ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
	return h;
#else
	(void)h; (void)r;
	return NULL_HINT;
#endif
}

ocrHint_t *rank_db_hint(ocrHint_t *h, u64 r) {
#ifdef ENABLE_EXTENSION_AFFINITY
	u64 n = 0;
	if(ocrAffinityCount(AFFINITY_PD, &n) != 0 || n == 0) return NULL_HINT;
	ocrGuid_t aff = NULL_GUID;
	if(ocrAffinityGetAt(AFFINITY_PD, r % n, &aff) != 0) return NULL_HINT;
	ocrHintInit(h, OCR_HINT_DB_T);
	ocrSetHintValue(h, OCR_HINT_DB_AFFINITY, ocrAffinityToHintValue(aff));
	return h;
#else
	(void)h; (void)r;
	return NULL_HINT;
#endif
}

ocrHint_t *img_stripe_hint(ocrHint_t *h, const struct img_stripes_s *st, int s) {
	return rank_edt_hint(h, img_stripe_rank(st, s));
}

ocrHint_t *img_stripe_db_hint(ocrHint_t *h, const struct img_stripes_s *st, int s) {
	return rank_db_hint(h, img_stripe_rank(st, s));
}

ocrGuid_t img_stripe_gather_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv) {
	stGatherPRM_t *p = (stGatherPRM_t *)paramv;
	assert(paramc==PRMNUM(stGather));
	(void)depc;
	const size_t cols = (size_t)p->ix;
	const int y0 = (int)p->y0, y1 = (int)p->y1;
	ocrHint_t dbh;
	ocrGuid_t dbg; struct complexData *dst = NULL;
	int retval = ocrDbCreate(&dbg, (void **)&dst,
	                         (size_t)(y1 - y0) * cols * sizeof(struct complexData),
	                         0, rank_db_hint(&dbh, p->rank), NO_ALLOC);
	if(retval != 0) { ocrPrintf("Error creating an image stripe\n");RAG_FLUSH;xe_exit(1); }
	for(u64 k = 0; k < p->nb; k++) {
		const struct complexData *src = (const struct complexData *)depv[k].ptr;
		if(src == NULL) continue;
		const int b0 = (int)((p->j0 + k) * p->bh);
		int b1 = b0 + (int)p->bh;
		if(b1 > (int)p->iy) b1 = (int)p->iy;
		const int lo = b0 > y0 ? b0 : y0;
		const int hi = b1 < y1 ? b1 : y1;
		for(int r = lo; r < hi; r++)
			memcpy(&dst[(size_t)(r - y0) * cols],
			       &src[(size_t)(r - b0) * cols],
			       cols * sizeof(struct complexData));
	}
	/* Release before handing the block on: a reader cannot acquire a block its
	   producer still holds. */
	ocrDbRelease(dbg);
	ocrGuid_t res_dbg; struct img_stripe_res_s *res = NULL;
	retval = ocrDbCreate(&res_dbg, (void **)&res, sizeof(*res), 0, NULL_HINT, NO_ALLOC);
	if(retval != 0) { ocrPrintf("Error creating a stripe result\n");RAG_FLUSH;xe_exit(1); }
	res->g = dbg;
	ocrDbRelease(res_dbg);
	return res_dbg;
}

ocrGuid_t img_stripes_collect_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv) {
	(void)paramc; (void)paramv;
	RAG_REF_MACRO_BSM( struct img_stripes_s *,st,NULL,NULL,st_dbg,0);
	for(u32 s = 1; s < depc; s++) {
		const struct img_stripe_res_s *res = (const struct img_stripe_res_s *)depv[s].ptr;
		st->g[s-1] = res ? res->g : NULL_GUID;
	}
	return NULL_GUID;
}

void img_stripes_read(const struct img_stripes_s *st, ocrEdtDep_t *depv, int slot0,
                      int y0, int y1, int x0, int x1, struct complexData *dst) {
	const int s0 = img_stripe_of(st, y0);
	const int s1 = img_stripe_of(st, y1 - 1);
	const size_t w = (size_t)(x1 - x0);
	for(int s = s0; s <= s1; s++) {
		const struct complexData *src = (const struct complexData *)depv[slot0 + (s - s0)].ptr;
		if(src == NULL) continue;
		const int lo = y0 > st->y0[s]   ? y0 : st->y0[s];
		const int hi = y1 < st->y0[s+1] ? y1 : st->y0[s+1];
		for(int r = lo; r < hi; r++)
			memcpy(&dst[(size_t)(r - y0) * w],
			       &src[(size_t)(r - st->y0[s]) * st->ix + x0],
			       w * sizeof(struct complexData));
	}
}

/* A rigorous bound on the quadratic warp q(n,m) = c0 + c1 n + c2 m + c3 n^2 +
 * c4 m^2 + c5 n m over the rectangle [n0,n1] x [m0,m1].  Image coordinates are
 * non-negative, so each term's range is given by its endpoints and the sum of
 * the term intervals contains the function's range -- an over-approximation,
 * which is what a source window must be. */
void warp_bound(const float c[6], int n0, int n1, int m0, int m1,
                float *lo, float *hi) {
	const double t[6][2] = {
		{ c[0], c[0] },
		{ (double)c[1]*n0, (double)c[1]*n1 },
		{ (double)c[2]*m0, (double)c[2]*m1 },
		{ (double)c[3]*n0*n0, (double)c[3]*n1*n1 },
		{ (double)c[4]*m0*m0, (double)c[4]*m1*m1 },
		{ (double)c[5]*n0*m0, (double)c[5]*n1*m1 },
	};
	double l = 0.0, h = 0.0;
	for(int i = 0; i < 6; i++) {
		l += t[i][0] < t[i][1] ? t[i][0] : t[i][1];
		h += t[i][0] < t[i][1] ? t[i][1] : t[i][0];
	}
	*lo = (float)l;
	*hi = (float)h;
}
