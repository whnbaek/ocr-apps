/* Coherent change detection and constant-false-alarm-rate detection, fused.
 *
 * The published form runs them as two passes over the image with a full
 * correlation map between them: every window's correlation value is stored,
 * and the detection pass then reads a (Ncfar x Ncfar) neighbourhood of that
 * map per window.  The map is as large as the image and both passes write it
 * -- respectively write and read -- from every block, which makes it a single
 * writable object shared by every task in the phase.
 *
 * The two passes are stencils over the same grid, so a block can compute the
 * correlation values its own detection windows need and consume them in
 * place: a block covering [m1,m2) x [n1,n2) needs the correlation values on
 * [m1, m2+Ncfar-1) x [n1, n2+Ncfar-1), which it computes from the images
 * directly.  The map then never has to exist, and the only thing a block
 * produces is its own detections, in its own datablock.  Boundary blocks
 * recompute the shared halo -- the ratio is (blk+Ncfar-1)^2 / blk^2 -- which
 * is what buys the removal of the shared object.
 */
#ifndef TG_ARCH
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "ocr.h"
#include "rag_ocr.h"
#include "common.h"

/* What a detection block reports: how many detections it found, how many bytes
 * they occupy in the output file, and where those bytes live.  The text itself
 * stays in its own datablock so that summing the counts -- which needs every
 * block -- does not have to pull the whole output through one task. */
struct cfar_blk_s {
	u64 cnt;
	u64 nbytes;
	ocrGuid_t text;
};

/* Longest line the detection format can produce, with room for the sign and
 * the widest coordinate the image grid can hold. */
#define CFAR_LINE_MAX 96


/* Every block's slice of the output is a known length at a known offset, so the
 * blocks write it themselves.  The task that computed the offsets created the
 * file; the writes extend it, and they never overlap. */
ocrGuid_t cfar_write_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv) {
	cfarWritePRM_t *p = (cfarWritePRM_t *)paramv;
	assert(paramc==PRMNUM(cfarWrite));
	assert(depc==1);
#ifndef TG_ARCH
	const char *text = (const char *)depv[0].ptr;
	if(text == NULL || p->nbytes == 0) return NULL_GUID;
	int fd = open(p->out_detects, O_WRONLY);
	if(fd < 0) {
		ocrPrintf("Error opening %s for writing\n", p->out_detects);RAG_FLUSH;
		xe_exit(1);
	}
	u64 done = 0;
	while(done < p->nbytes) {
		ssize_t w = pwrite(fd, text + done, (size_t)(p->nbytes - done), (off_t)(p->offset + done));
		if(w <= 0) {
			ocrPrintf("Short write to %s\n", p->out_detects);RAG_FLUSH;
			close(fd);
			xe_exit(1);
		}
		done += (u64)w;
	}
	close(fd);
#endif
	return NULL_GUID;
}

ocrGuid_t post_CFAR_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv) {
	int retval;
	postCFARPRM_t *postCFARParamvIn  = (postCFARPRM_t *)paramv;
#ifdef TRACE_LVL_2
ocrPrintf("//// enter post_CFAR_edt\n");RAG_FLUSH;
#endif
	assert(paramc==PRMNUM(postCFAR));

	u64 Nd = 0, total = 0;
	for(u32 b = 0; b < depc; b++) {
		struct cfar_blk_s *blk = (struct cfar_blk_s *)depv[b].ptr;
		if(blk == NULL) continue;
		Nd    += blk->cnt;
		total += blk->nbytes;
	}

	/* Deterministic correctness scalar for a given input dataset. */
	ocrPrintf("SAR detects: %lu\n", Nd);RAG_FLUSH;

#ifndef TG_ARCH
	/* Create the file once, empty; the blocks extend it from their offsets. */
	{
		FILE *pOutFile = fopen(postCFARParamvIn->out_detects, "wb");
		if( pOutFile == NULL ) {
			ocrPrintf("Error opening %s\n", postCFARParamvIn->out_detects);RAG_FLUSH;
			xe_exit(1);
		}
		fclose(pOutFile);
	}

	ocrGuid_t write_clg;
	retval = ocrEdtTemplateCreate(&write_clg, cfar_write_edt, PRMNUM(cfarWrite), 1);
	assert(retval==0);
	RAG_TEMPLATE_REGISTER(write_clg);

	u64 offset = 0;
	for(u32 b = 0; b < depc; b++) {
		struct cfar_blk_s *blk = (struct cfar_blk_s *)depv[b].ptr;
		if(blk == NULL || blk->nbytes == 0) continue;
		cfarWritePRM_t wp;
		wp.offset = offset;
		wp.nbytes = blk->nbytes;
		memcpy(wp.out_detects, postCFARParamvIn->out_detects, sizeof(wp.out_detects));
		offset += blk->nbytes;

		ocrGuid_t write_scg;
		retval = ocrEdtCreate(&write_scg, write_clg, EDT_PARAM_DEF, (u64 *)&wp,
		                      EDT_PARAM_DEF, NULL, EDT_PROP_NONE, NULL_HINT, NULL);
		assert(retval==0);
		retval = ocrAddDependence(blk->text, write_scg, 0, DB_MODE_RO);
		assert(retval==0);
	}
	assert(offset == total);
#else
	for(u32 b = 0; b < depc; b++) {
		struct cfar_blk_s *blk = (struct cfar_blk_s *)depv[b].ptr;
		if(blk == NULL) continue;
		ocrPrintf("block %u: %lu detects\n", b, blk->cnt);
	}
#endif

#ifdef TRACE_LVL_2
ocrPrintf("//// leave post_CFAR_edt\n");RAG_FLUSH;
#endif
	return NULL_GUID;
}

ocrGuid_t cfar_async_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv) {
	int retval;
	CFARAsyncPRM_t *CFARAsyncParamvIn = (CFARAsyncPRM_t *)paramv;
	assert(paramc==PRMNUM(CFARAsync));
	struct corners_t *corners = &(CFARAsyncParamvIn->corners);
	const int m1 = corners->m1;
	const int m2 = corners->m2;
	const int n1 = corners->n1;
	const int n2 = corners->n2;
	assert(depc==CFAR_STRIPE_SLOT0 + 4);
RAG_REF_MACRO_SPAD(struct ImageParams,image_params,image_params_ptr,image_params_lcl,image_params_dbg,0);
RAG_REF_MACRO_SPAD(struct CfarParams,cfar_params,cfar_parms_ptr,cfar_parms_lcl,cfar_params_dbg,1);
RAG_REF_MACRO_BSM( const struct img_stripes_s *,st_cur,NULL,NULL,st_cur_dbg,2);
RAG_REF_MACRO_BSM( const struct img_stripes_s *,st_ref,NULL,NULL,st_ref_dbg,3);
	/* image_params->xr / ->yr are read below to fill the detect coordinates;
	 * rebuild them locally (stale sibling-DB pointers under relocation). */
	float rag_xr[image_params->Ix];
	float rag_yr[image_params->Iy];
	RAG_REBUILD_AXIS(image_params, rag_xr, rag_yr);

	const int Ncor     = image_params->Ncor;
	const int Ncor_sqr = Ncor * Ncor;
	const int Ncfar    = cfar_params->Ncfar;
	const int Nguard   = cfar_params->Nguard;
	const int cm       = m2 - m1 + Ncfar - 1;	/* correlation halo extent */
	const int cn       = n2 - n1 + Ncfar - 1;

	/* This block's rectangle of both images, assembled out of the stripes that
	   cover it.  The images themselves stay where they were produced. */
	const int _wny = (m2 - m1) + Ncfar + Ncor - 2;
	const int _wnx = (n2 - n1) + Ncfar + Ncor - 2;
	if(_wny > RAG_CFAR_MAX_WIN || _wnx > RAG_CFAR_MAX_WIN) {
		ocrPrintf("detection window %d exceeds RAG_CFAR_MAX_WIN %d\n",
		          _wny > _wnx ? _wny : _wnx, RAG_CFAR_MAX_WIN);RAG_FLUSH;
		xe_exit(1);
	}
	struct complexData _wdata[2*(size_t)RAG_CFAR_MAX_WIN*(size_t)RAG_CFAR_MAX_WIN];
	const int _wy0 = m1, _wx0 = n1;
	img_stripes_read(st_cur, depv, CFAR_STRIPE_SLOT0,     m1, m1+_wny, n1, n1+_wnx, _wdata);
	img_stripes_read(st_ref, depv, CFAR_STRIPE_SLOT0 + 2, m1, m1+_wny, n1, n1+_wnx,
	                 _wdata + (size_t)_wny*(size_t)_wnx);
	const struct complexData *_cw = _wdata;
	const struct complexData *_rw = _wdata + (size_t)_wny*(size_t)_wnx;

	/* Correlation value threshold: a candidate's own value must be below it. */
	const float Tcorr = 0.8;

	ocrGuid_t corr_dbg;
	float *corr = (float *)spad_malloc(&corr_dbg,(size_t)cm*(size_t)cn*sizeof(float));
	if(corr == NULL) {
		ocrPrintf("Error allocating the block's correlation window.\n");RAG_FLUSH;
		xe_exit(1);
	}
	ocrGuid_t det_dbg;
	struct detects *det = (struct detects *)spad_malloc(&det_dbg,
				(size_t)(m2-m1)*(size_t)(n2-n1)*sizeof(struct detects));
	if(det == NULL) {
		ocrPrintf("Error allocating the block's detect list.\n");RAG_FLUSH;
		xe_exit(1);
	}

	/* correlation over the block's own windows and the halo the detection
	   windows below reach into */
	for(int a=0; a<cm; a++) {
		const int mIndex = (Ncor-1)/2 + m1 + a;
		for(int b=0; b<cn; b++) {
			const int nIndex = (Ncor-1)/2 + n1 + b;
			struct complexData f[MAX_Ncor_sqr];
			struct complexData g[MAX_Ncor_sqr];
			float     den1, den2;
			struct    complexData mu_f, mu_g, num;
			mu_f.real=0.0f;
			mu_f.imag=0.0f;
			mu_g.real=0.0f;
			mu_g.imag=0.0f;
			for(int k=0,i=mIndex-(Ncor-1)/2; i<=mIndex+(Ncor-1)/2; i++) {
				for(int j=nIndex-(Ncor-1)/2; j<=nIndex+(Ncor-1)/2; j++, k++) {
					const size_t _o = (size_t)(i - _wy0)*(size_t)_wnx
					                + (size_t)(j - _wx0);
					f[k].real  = _cw[_o].real;
					f[k].imag  = _cw[_o].imag;
					g[k].real  = _rw[_o].real;
					g[k].imag  = _rw[_o].imag;
					mu_f.real += _cw[_o].real;
					mu_f.imag += _cw[_o].imag;
					mu_g.real += _rw[_o].real;
					mu_g.imag += _rw[_o].imag;
				} // for j
			} // for i
			mu_f.real /= Ncor_sqr;
			mu_f.imag /= Ncor_sqr;
			mu_g.real /= Ncor_sqr;
			mu_g.imag /= Ncor_sqr;

			num.real = 0.0f;
			num.imag = 0.0f;
			den1 = 0.0f;
			den2 = 0.0f;
			for(int k=0; k<Ncor_sqr; k++) {
				f[k].real -= mu_f.real;
				f[k].imag -= mu_f.imag;
				g[k].real -= mu_g.real;
				g[k].imag -= mu_g.imag;

				num.real += f[k].real*g[k].real + f[k].imag*g[k].imag;
				num.imag += f[k].real*g[k].imag - f[k].imag*g[k].real;

				den1 += f[k].real*f[k].real + f[k].imag*f[k].imag;
				den2 += g[k].real*g[k].real + g[k].imag*g[k].imag;
			} // for k

			if( (den1 != 0.0f) && (den2 != 0.0f) ) {
				corr[a*cn+b] = sqrtf( (num.real*num.real + num.imag*num.imag) / (den1*den2) );
			} else {
				corr[a*cn+b] = 0.0f;
			}
		} // for b
	} // for a

	ocrGuid_t pLocal_dbg;
	float **pLocal = (float **)spad_malloc(&pLocal_dbg,Ncfar*sizeof(float*)
						       +Ncfar*Ncfar*sizeof(float));
	if(pLocal == NULL) {
		ocrPrintf("Error allocating edge vector for local correlation map.\n");RAG_FLUSH;
		xe_exit(1);
	}
	float *pLocal_data_ptr = (float *)&pLocal[Ncfar];
	for(int m=0; m<Ncfar; m++) {
		pLocal[m] = pLocal_data_ptr + m*Ncfar;
	}

	const int T = (int)floorf(cfar_params->Tcfar/100.0*(Ncfar*Ncfar-Nguard*Nguard));
	u64 nd = 0;

	for(int m=m1; m<m2; m++) {
		for(int n=n1; n<n2; n++) {
			/* the detection window's own corner in the block's correlation
			   window: index (m-m1, n-n1), extent Ncfar */
			for(int k=0; k<Ncfar; k++) {
				for(int l=0; l<Ncfar; l++) {
					pLocal[k][l] = corr[(m-m1+k)*cn + (n-n1+l)];
				} // for l
			} // for k

			for(int i=(Ncfar-1)/2-(Nguard-1)/2; i<=(Ncfar-1)/2+(Nguard-1)/2; i++) {
				for(int j=(Ncfar-1)/2-(Nguard-1)/2; j<=(Ncfar-1)/2+(Nguard-1)/2; j++) {
					pLocal[i][j] = -1;
				} // for j
			} // for i

			const float CUT = corr[(m-m1+(Ncfar-1)/2)*cn + (n-n1+(Ncfar-1)/2)];

			if(CUT < Tcorr) {
				int cnt;
				for(int i=cnt=0; i<Ncfar; i++) {
					for(int j=0; j<Ncfar; j++) {
						if(CUT < pLocal[i][j]) {
							cnt++;
						} // if CUT
					} // for j
				} // for i

				if(cnt >= T) {
					/* the correlation map's coordinates for this window are
					   its own indices offset by the two window radii */
					det[nd].x = image_params->xr[(Ncor-1)/2 + (Ncfar-1)/2 + n];
					det[nd].y = image_params->yr[(Ncor-1)/2 + (Ncfar-1)/2 + m];
					det[nd].p = CUT;
					nd++;
				} // if cnt
			} // if CUT
		} // for n
	} // for m

	spad_free(pLocal,pLocal_dbg);
	spad_free(corr,corr_dbg);

	/* Format this block's slice of the output here: its length is then known
	   without anyone reading the detections, which is what lets the blocks
	   place their own bytes in the file rather than funnelling through one
	   task. */
	ocrGuid_t text_dbg = NULL_GUID;
	u64 nbytes = 0;
#ifndef TG_ARCH
	if(nd) {
		ocrGuid_t scratch_dbg;
		char *scratch = (char *)spad_malloc(&scratch_dbg, (size_t)nd*CFAR_LINE_MAX);
		if(scratch == NULL) {
			ocrPrintf("Error allocating the block's output buffer.\n");RAG_FLUSH;
			xe_exit(1);
		}
		for(u64 m = 0; m < nd; m++) {
			int w = snprintf(scratch + nbytes, CFAR_LINE_MAX,
			                 "(x=%7.2f m, y=%7.2f m, p=%4.2f)\n",
			                 det[m].x, det[m].y, det[m].p);
			if(w < 0 || w >= CFAR_LINE_MAX) {
				ocrPrintf("Detection line exceeds the format's bound.\n");RAG_FLUSH;
				xe_exit(1);
			}
			nbytes += (u64)w;
		}
		char *text = NULL;
		retval = ocrDbCreate(&text_dbg, (void **)&text, (size_t)nbytes, 0, NULL_HINT, NO_ALLOC);
		assert(retval==0);
		memcpy(text, scratch, (size_t)nbytes);
		ocrDbRelease(text_dbg);
		spad_free(scratch,scratch_dbg);
	}
#endif

	ocrGuid_t out_dbg; struct cfar_blk_s *out = NULL;
	retval = ocrDbCreate(&out_dbg, (void **)&out, sizeof(struct cfar_blk_s),
	                     0, NULL_HINT, NO_ALLOC);
	assert(retval==0);
	out->cnt    = nd;
	out->nbytes = nbytes;
	out->text   = text_dbg;
	ocrDbRelease(out_dbg);
	spad_free(det,det_dbg);
	return out_dbg;
}

ocrGuid_t CFAR_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv) {
	int retval;
	CFARPRM_t *CFARParamvIn = (CFARPRM_t *)paramv;
#ifdef TRACE_LVL_2
ocrPrintf("//// enter CFAR_edt\n");RAG_FLUSH;
#endif
	assert(paramc==PRMNUM(CFAR));
	assert(depc==7);
RAG_REF_MACRO_PASS(NULL,NULL,NULL,NULL,curImage_dbg,0);
RAG_REF_MACRO_PASS(NULL,NULL,NULL,NULL,refImage_dbg,1);
RAG_REF_MACRO_SPAD(struct ImageParams,image_params,image_params_ptr,image_params_lcl,image_params_dbg,2);
RAG_REF_MACRO_SPAD(struct CfarParams,cfar_params,cfar_params_ptr,cfar_params_lcl,cfar_params_dbg,3);
RAG_REF_MACRO_BSM( const struct img_stripes_s *,st_reg,NULL,NULL,st_reg_dbg,5);
RAG_REF_MACRO_BSM( const struct img_stripes_s *,st_ref,NULL,NULL,st_ref_dbg,6);

	const int Mwins = image_params->Iy - image_params->Ncor - cfar_params->Ncfar + 2;
	const int Nwins = image_params->Ix - image_params->Ncor - cfar_params->Ncfar + 2;
#ifdef RAG_CFAR_BLK_SIZE
	const int BM = RAG_CFAR_BLK_SIZE;
	const int BN = RAG_CFAR_BLK_SIZE;
#else
	const int BM = blk_size(Mwins,32);
	const int BN = blk_size(Nwins,32);
#endif

	int nbm = 0, nbn = 0;
	for(int m=0; m<Mwins; m+=BM) nbm++;
	for(int n=0; n<Nwins; n+=BN) nbn++;
	const int nblks = nbm * nbn;

	ocrGuid_t post_CFAR_clg, post_CFAR_scg;
	retval = ocrEdtTemplateCreate(&post_CFAR_clg, post_CFAR_edt, PRMNUM(postCFAR), nblks);
	assert(retval==0);
	RAG_TEMPLATE_REGISTER(post_CFAR_clg);
	retval = ocrEdtCreate(&post_CFAR_scg, post_CFAR_clg, EDT_PARAM_DEF,
	                      (u64 *)&CFARParamvIn->post, EDT_PARAM_DEF, NULL,
	                      EDT_PROP_NONE, NULL_HINT, NULL);
	assert(retval==0);

	ocrGuid_t cfar_async_clg;
	retval = ocrEdtTemplateCreate(&cfar_async_clg, cfar_async_edt, PRMNUM(CFARAsync), CFAR_STRIPE_SLOT0 + 4);
	assert(retval==0);
	RAG_TEMPLATE_REGISTER(cfar_async_clg);

	int bidx = 0;
	for(int m=0; m<Mwins; m+=BM) {
		for(int n=0; n<Nwins; n+=BN) {
			CFARAsyncPRM_t cfarAsyncParamv;
			cfarAsyncParamv.corners.m1 = m;
			cfarAsyncParamv.corners.m2 = (m+BM)<Mwins?(m+BM):Mwins;
			cfarAsyncParamv.corners.n1 = n;
			cfarAsyncParamv.corners.n2 = (n+BN)<Nwins?(n+BN):Nwins;

			/* The stripes this block's rectangle falls in.  The block is placed
			   on the stripe that holds it, so the piece it reads is already
			   there and the blocks beside it on that rank share the fetch. */
			const int _wny = (cfarAsyncParamv.corners.m2 - cfarAsyncParamv.corners.m1)
			               + cfar_params->Ncfar + image_params->Ncor - 2;
			const int _gs0 = img_stripe_of(st_reg, m);
			const int _gs1 = img_stripe_of(st_reg, m + _wny - 1);
			const int _fs0 = img_stripe_of(st_ref, m);
			const int _fs1 = img_stripe_of(st_ref, m + _wny - 1);
			assert(_gs1 - _gs0 <= 1 && _fs1 - _fs0 <= 1);
			ocrHint_t _hnt;
			ocrGuid_t cfar_async_scg, blk_evg;
			retval = ocrEdtCreate(&cfar_async_scg, cfar_async_clg, EDT_PARAM_DEF,
			                      (u64 *)&cfarAsyncParamv, EDT_PARAM_DEF, NULL,
			                      EDT_PROP_NONE, img_stripe_hint(&_hnt, st_reg, _gs0), &blk_evg);
			assert(retval==0);
			retval = ocrAddDependence(blk_evg, post_CFAR_scg, bidx, DB_MODE_RO);
			assert(retval==0);
			bidx++;

RAG_DEF_MACRO_PASS_RO(cfar_async_scg,NULL,NULL,NULL,NULL,image_params_dbg,0);
RAG_DEF_MACRO_PASS_RO(cfar_async_scg,NULL,NULL,NULL,NULL,cfar_params_dbg,1);
RAG_DEF_MACRO_PASS_RO(cfar_async_scg,NULL,NULL,NULL,NULL,st_reg_dbg,2);
RAG_DEF_MACRO_PASS_RO(cfar_async_scg,NULL,NULL,NULL,NULL,st_ref_dbg,3);
			for(int _k = 0; _k < 2; _k++) {
				if(_gs0 + _k <= _gs1) {
RAG_DEF_MACRO_PASS_RO(cfar_async_scg,NULL,NULL,NULL,NULL,st_reg->g[_gs0+_k],CFAR_STRIPE_SLOT0+_k);
				} else {
RAG_DEF_MACRO_GUID_ONLY(cfar_async_scg,st_reg_dbg,CFAR_STRIPE_SLOT0+_k);
				}
				if(_fs0 + _k <= _fs1) {
RAG_DEF_MACRO_PASS_RO(cfar_async_scg,NULL,NULL,NULL,NULL,st_ref->g[_fs0+_k],CFAR_STRIPE_SLOT0+2+_k);
				} else {
RAG_DEF_MACRO_GUID_ONLY(cfar_async_scg,st_ref_dbg,CFAR_STRIPE_SLOT0+2+_k);
				}
			}
		} // for n
	} // for m

#ifdef TRACE_LVL_2
ocrPrintf("//// leave CFAR_edt\n");RAG_FLUSH;
#endif
	return NULL_GUID;
}
