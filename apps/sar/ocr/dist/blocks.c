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

void img_stripes_layout(struct img_stripes_s *st, int ns, int nr, int iy, int ix, int align) {
	if(ns < 1) ns = 1;
	if(ns > IMG_MAX_STRIPES) ns = IMG_MAX_STRIPES;
	/* Boundaries on multiples of `align` so a producer tile never straddles
	   two stripes; the remainder is spread one unit at a time rather than
	   piled onto the last stripe. */
	const int units = (iy + align - 1) / align;
	if(ns > units) ns = units;
	/* A stripe must stay taller than any consumer's footprint, or a task would
	   reach across more than two of them and the covering set stops being
	   bounded.  Below this the image is simply not worth striping. */
	if(ns > iy / IMG_MIN_STRIPE) ns = iy / IMG_MIN_STRIPE;
	if(ns < 1) ns = 1;
	/* Keep the count a whole multiple of the rank count: a remainder leaves
	   some ranks owning more stripes than others, and the extra one's halo is
	   a neighbour nobody on that rank shares. */
	if(nr > 0 && ns > nr) ns -= ns % nr;
	if(ns < 1) ns = 1;
	st->ns = ns;
	st->nr = nr < 1 ? 1 : nr;
	st->iy = iy;
	st->ix = ix;
	int acc = 0;
	for(int s = 0; s < ns; s++) {
		st->y0[s] = acc * align < iy ? acc * align : iy;
		acc += units / ns + (s < units % ns ? 1 : 0);
	}
	st->y0[ns] = iy;
	for(int s = 0; s < ns; s++) st->g[s] = NULL_GUID;
}

ocrHint_t *img_stripe_hint(ocrHint_t *h, const struct img_stripes_s *st, int s) {
#ifdef ENABLE_EXTENSION_AFFINITY
	u64 n = 0;
	if(ocrAffinityCount(AFFINITY_PD, &n) != 0 || n == 0) return NULL_HINT;
	ocrGuid_t aff = NULL_GUID;
	if(ocrAffinityGetAt(AFFINITY_PD, ((u64)s * st->nr / st->ns) % n, &aff) != 0) return NULL_HINT;
	ocrHintInit(h, OCR_HINT_EDT_T);
	ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
	return h;
#else
	(void)h; (void)st; (void)s;
	return NULL_HINT;
#endif
}

static ocrHint_t *img_stripe_db_hint(ocrHint_t *h, const struct img_stripes_s *st, int s) {
#ifdef ENABLE_EXTENSION_AFFINITY
	u64 n = 0;
	if(ocrAffinityCount(AFFINITY_PD, &n) != 0 || n == 0) return NULL_HINT;
	ocrGuid_t aff = NULL_GUID;
	if(ocrAffinityGetAt(AFFINITY_PD, ((u64)s * st->nr / st->ns) % n, &aff) != 0) return NULL_HINT;
	ocrHintInit(h, OCR_HINT_DB_T);
	ocrSetHintValue(h, OCR_HINT_DB_AFFINITY, ocrAffinityToHintValue(aff));
	return h;
#else
	(void)h; (void)st; (void)s;
	return NULL_HINT;
#endif
}

void img_stripes_scatter(struct img_stripes_s *st, struct complexData **image) {
	for(int s = 0; s < st->ns; s++) {
		const int rows = st->y0[s+1] - st->y0[s];
		if(rows <= 0) { st->g[s] = NULL_GUID; continue; }
		ocrHint_t dbh;
		ocrGuid_t dbg; struct complexData *p = NULL;
		u8 rv = ocrDbCreate(&dbg, (void **)&p,
		                    (size_t)rows * (size_t)st->ix * sizeof(struct complexData),
		                    0, img_stripe_db_hint(&dbh, st, s), NO_ALLOC);
		if(rv != 0) { ocrPrintf("Error creating image stripe %d\n", s);RAG_FLUSH;xe_exit(1); }
		for(int r = 0; r < rows; r++)
			memcpy(&p[(size_t)r * st->ix], &image[st->y0[s] + r][0],
			       (size_t)st->ix * sizeof(struct complexData));
		/* Release before the consumers' dependences are wired: a reader
		   cannot acquire a block its producer still holds. */
		ocrDbRelease(dbg);
		st->g[s] = dbg;
	}
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
