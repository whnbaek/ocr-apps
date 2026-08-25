#include "ocr.h"
#include "rag_ocr.h"
#include "common.h"

#ifndef TG_ARCH // FIX-ME
#define dram_free(addr,guid)
#define  bsm_free(addr,guid)
#define spad_free(addr,guid)
#endif

#define GAUSS_ELIM_SUCCESS (0)
#define GAUSS_ELIM_SINGULAR_MATRIX (1)

struct async_1_args_t {
	struct point ctrl_pt;
	// Row count A was allocated with (the control-point count before it is
	// reset to zero and re-counted).  Needed to rebuild A's row-pointer table
	// when the block is relocated, since affine_params->Nc no longer holds it.
	int Nc_alloc;
};

struct async_2_args_t {
	float Wcx[6];
	float Wcy[6];
};

ocrGuid_t Affine_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv) {
	int retval;
    affinePRM_t *affineParamvIn = (affinePRM_t *)paramv;
#ifdef TRACE_LVL_2
ocrPrintf("//// enter Affine_edt\n");RAG_FLUSH;
#endif
	assert(paramc==PRMNUM(affine));
	ocrGuid_t post_affine_async_1_scg	= affineParamvIn->post_affine_async_scg;
	assert(depc==6);
RAG_REF_MACRO_PASS(NULL,NULL,NULL,NULL,curImage_dbg,0);
RAG_REF_MACRO_PASS(NULL,NULL,NULL,NULL,refImage_dbg,1);
RAG_REF_MACRO_SPAD(struct AffineParams,affine_params,affine_params_ptr,affine_params_lcl,affine_params_dbg,2);
RAG_REF_MACRO_SPAD(struct ImageParams,image_params,image_params_ptr,image_params_lcl,image_params_dbg,3);
RAG_REF_MACRO_BSM( const struct img_stripes_s *,st_cur,NULL,NULL,st_cur_dbg,4);
RAG_REF_MACRO_BSM( const struct img_stripes_s *,st_ref,NULL,NULL,st_ref_dbg,5);

	ocrGuid_t Affine(
		const struct img_stripes_s *st_cur,	ocrGuid_t st_cur_dbg,
		const struct img_stripes_s *st_ref,	ocrGuid_t st_ref_dbg,
		ocrGuid_t curImage_dbg,
		struct AffineParams *affine_params,	ocrGuid_t affine_params_dbg, struct AffineParams *affine_params_ptr,
		struct ImageParams *image_params,	ocrGuid_t image_params_dbg,
		ocrGuid_t post_affine_async_1_scg);

	Affine(	st_cur,		st_cur_dbg,
		st_ref,		st_ref_dbg,
		curImage_dbg,
		affine_params,	affine_params_dbg, affine_params_ptr,
		image_params,	image_params_dbg,
		post_affine_async_1_scg);

#ifdef TRACE_LVL_2
ocrPrintf("//// leave Affine_edt\n");RAG_FLUSH;
#endif
	return NULL_GUID;
}

int gauss_elim(float *AA[], float *x, int N);

struct point corr2D(struct point ctrl_pt, int Nwin, int R,
    const struct ctrlpt_win_s *, struct ImageParams*);

/* Tiles are a regular grid over the image, so a tile's rectangle is
 * recoverable from its slot: the dependences are added in creation order. */
typedef struct { u64 by, bx; } afGatherPRM_t;

/* The registered image replaces the current one, so the tiles land directly in
 * curImage and the whole-image intermediate the base program writes and then
 * copies from never has to exist.  Every block has read curImage before this
 * runs -- the gather waits on all of them. */
ocrGuid_t affine_gather_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv) {
	afGatherPRM_t *p = (afGatherPRM_t *)paramv;
	RAG_REF_MACRO_SPAD(struct ImageParams,image_params,image_params_ptr,image_params_lcl,image_params_dbg,0);
	RAG_REF_MACRO_BSM( struct complexData **,curImage,NULL,NULL,curImage_dbg,1);
	RAG_REF_MACRO_BSM( struct img_stripes_s *,stripes,NULL,NULL,stripes_dbg,2);
	RAG_REMAP_2D(curImage, image_params->Iy, image_params->Ix, struct complexData);
	int t = 0;
	for(int m = 0; m < image_params->Iy; m += (int)p->by) {
		const int mhi = (m + (int)p->by) < image_params->Iy ? (m + (int)p->by) : image_params->Iy;
		for(int n = 0; n < image_params->Ix; n += (int)p->bx, ++t) {
			const int nhi = (n + (int)p->bx) < image_params->Ix ? (n + (int)p->bx) : image_params->Ix;
			struct complexData *tile = (struct complexData *)depv[3+t].ptr;
			if(tile == NULL) continue;
			for(int r = m; r < mhi; ++r)
				memcpy(&curImage[r][n], &tile[(r-m)*(nhi-n)], (nhi-n)*sizeof(struct complexData));
		}
	}
	/* Cut the registered image into the stripes its detection blocks read. */
	img_stripes_scatter(stripes, curImage);
	return NULL_GUID;
}

ocrGuid_t post_affine_async_1_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv) {
	int retval;
    postAffineAsyncPRM_t *postAffineAsyncParamvIn  = (postAffineAsyncPRM_t *)paramv;
#ifdef TRACE_LVL_3
ocrPrintf("////// enter post_affine_async_1_edt\n");RAG_FLUSH;
#endif
	assert(paramc==PRMNUM(postAffineAsync));
	ocrGuid_t post_affine_async_2_scg = postAffineAsyncParamvIn->post_affine_async_scg; // post_affine_async_2_scg
	assert(depc>=CTRLPT_RES_SLOT0); // fixed slots, then one per control point
RAG_REF_MACRO_SPAD(struct async_1_args_t,post_affine_async_1_args,post_affine_async_1_args_ptr,post_affine_async_1_args_lcl,post_affine_async_1_args_dbg,0);
RAG_REF_MACRO_SPAD(struct AffineParams,affine_params,affine_params_ptr,affine_params_lcl,affine_params_dbg,1);
RAG_REF_MACRO_SPAD(struct ImageParams,image_params,image_params_ptr,image_params_lcl,image_params_dbg,2);
RAG_REF_MACRO_BSM( struct complexData **,curImage,NULL,NULL,curImage_dbg,3);
RAG_REF_MACRO_BSM( int *,Fx,NULL,NULL,Fx_dbg,4);
RAG_REF_MACRO_BSM( int *,Fy,NULL,NULL,Fy_dbg,5);
RAG_REF_MACRO_BSM( int **,A,NULL,NULL,A_dbg,6);
RAG_REF_MACRO_BSM( const struct img_stripes_s *,st_cur,NULL,NULL,st_cur_dbg,CTRLPT_ST_CUR_SLOT);
RAG_REF_MACRO_PASS(NULL,NULL,NULL,NULL,st_reg_dbg,CTRLPT_ST_REG_SLOT);

	// Rebuild A's row-pointer table against this node's copy before the
	// A[k][*] reads in the least-squares accumulation below.
	RAG_REMAP_2D(A, post_affine_async_1_args->Nc_alloc, 6, int);

	/* Collect the control points.  Each task reported into its own block, so
	   this walk is the whole reduction: the retained ones are compacted into
	   Fx/Fy/A in slot order, which is a permutation of the order the atomic
	   used to hand out and equally valid -- the least squares below sums A'A
	   and A'F over the set, and a sum does not care how it is ordered. */
	{
		int _n = 0;
		for(uint32_t _s = CTRLPT_RES_SLOT0; _s < depc; _s++) {
			const struct ctrlpt_res_s *_r =
				(const struct ctrlpt_res_s *)depv[_s].ptr;
			if(_r == NULL || !_r->keep) { continue; }
			Fx[_n] = _r->fx;
			Fy[_n] = _r->fy;
			for(int _j = 0; _j < 6; _j++) { A[_n][_j] = _r->a[_j]; }
			_n++;
		}
		affine_params->Nc = _n;
		affine_params_ptr->Nc = _n;
	}

	int rc;
	// b = 6 x 2
	float b[6][2];
	//
	// aug_mat = 6 x 7
	ocrGuid_t aug_mat_dbg;
	float **aug_mat;
	aug_mat = (float **)spad_malloc(&aug_mat_dbg,6*sizeof(float *)
						   +6*7*sizeof(float));
	if(aug_mat == NULL) {
		ocrPrintf("Unable to allocate memory for aug_mat.\n");RAG_FLUSH;
		xe_exit(1);
	}
	float *aug_mat_data_ptr = (float *)&aug_mat[6];
	if(aug_mat_data_ptr == NULL) {
		ocrPrintf("Unable to allocate memory for aug_mat.\n");RAG_FLUSH;
		xe_exit(1);
	}
	for(int n=0;n<6;n++) {
	        aug_mat[n] = aug_mat_data_ptr + n*7;
	        for(int i=0;i<7;i++) {
			aug_mat[n][i] = 0.0f;
		}
	}

	// Wcx[6];
	float Wcx[6];
	// Wcy[6];
	float Wcy[6];

#if defined(DEBUG) && !defined(TG_ARCH)
printf("Nc = %d\n",affine_params->Nc);RAG_FLUSH;
for(int i=0;i<affine_params->Nc;i++) {
	printf("A[%d][*] = %d %d %d %d %d %d\n",i, A[i][0], A[i][1], A[i][2], A[i][3], A[i][4], A[i][5]);RAG_FLUSH;
}
#endif

	// b = A'F
	// The computation of A'F and A'A requires better precision than would be obtained
	// by simply typecasting the A components to floats for the accumulation.  Here,
	// we use double precision accumulators, although 64-bit integer accumulations or
	// dynamic range rescaling provide other viable options to obtain additional precision.
	for(int m=0; m<6; m++) {
		double accum_x = 0.0, accum_y = 0.0;
		for(int n=0; n<affine_params->Nc; n++) {
			accum_x += (double)A[n][m]*(double)Fx[n];
			accum_y += (double)A[n][m]*(double)Fy[n];
		}
		b[m][0] = accum_x;
		b[m][1] = accum_y;
	}
#if defined(DEBUG) && !defined(TG_ARCH)
for(int i=0;i<6;i++) {
	printf("b[%d][*] = %f %f\n",i, b[i][0], b[i][1]);RAG_FLUSH;
}
#endif

	// aug_mat(1:6,1:6) = A'A
	for(int m=0; m<6; m++) {
		for(int n=0; n<6; n++) {
			double accum = 0.0;
			for(int k=0; k<affine_params->Nc; k++) {
				accum += (double)A[k][m] * (double) A[k][n];
			}
			aug_mat[m][n] = (float)accum;
		}
	}

	// aug_mat(1:6,7) = b(1:6,1)
	for(int m=0; m<6; m++) {
		aug_mat[m][6] = b[m][0];
	}
#if defined(DEBUG) && !defined(TG_ARCH)
for(int i=0;i<6;i++) {
	printf("aug[%d][*] = %f %f %f %f %f %f %f\n",i, aug_mat[i][0], aug_mat[i][1], aug_mat[i][2], aug_mat[i][3], aug_mat[i][4], aug_mat[i][5],aug_mat[i][6]);RAG_FLUSH;
}
#endif
#ifdef TRACE_LVL_3
ocrPrintf("////// Perform Gaussian elimination to find Wcx\n");RAG_FLUSH;
#endif
	rc = gauss_elim(aug_mat, Wcx, 6);
	if (rc != GAUSS_ELIM_SUCCESS) {
		// Default to the identity if Gaussian elimination failes
		spad_memset(Wcx, 0, sizeof(float)*6);
		Wcx[1] = 1.0f;
	}
#ifdef TG_ARCH
ocrPrintf("Wcx %x%x %x%x %x%x\n",
*(uint32_t *)(uint32_t *)(&Wcx[0])+0,
*(uint32_t *)(uint32_t *)(&Wcx[0])+1,
*(uint32_t *)(uint32_t *)(&Wcx[1])+0,
*(uint32_t *)(uint32_t *)(&Wcx[1])+1,
*(uint32_t *)(uint32_t *)(&Wcx[2])+0,
*(uint32_t *)(uint32_t *)(&Wcx[2])+1);RAG_FLUSH
ocrPrintf("Wcx %x%x %x%x %x%x\n",
*(uint32_t *)(uint32_t *)(&Wcx[3])+0,
*(uint32_t *)(uint32_t *)(&Wcx[3])+1,
*(uint32_t *)(uint32_t *)(&Wcx[4])+0,
*(uint32_t *)(uint32_t *)(&Wcx[4])+1,
*(uint32_t *)(uint32_t *)(&Wcx[5])+0,
*(uint32_t *)(uint32_t *)(&Wcx[5])+1);RAG_FLUSH
#else
fprintf(stderr,"Wcx %f %f %f %f %f %f\n",Wcx[0],Wcx[1],Wcx[2],Wcx[3],Wcx[4],Wcx[5]);fflush(stderr);
#endif

	// aug_mat(1:6,7) = b(1:6,2)
	for(int m=0; m<6; m++) {
		aug_mat[m][6] = b[m][1];
	}
#if defined(DEBUG) && !defined(TG_ARCH)
for(int i=0;i<6;i++) {
	printf("aug[%d][*] = %f %f %f %f %f %f %f\n",i, aug_mat[i][0], aug_mat[i][1], aug_mat[i][2], aug_mat[i][3], aug_mat[i][4], aug_mat[i][5],aug_mat[i][6]);RAG_FLUSH;
}
#endif
#ifdef TRACE_LVL_3
ocrPrintf("////// Perform Gaussian elimination to find Wcy\n");RAG_FLUSH;
#endif
	rc = gauss_elim(aug_mat, Wcy, 6);
	if (rc != GAUSS_ELIM_SUCCESS) {
		// Default to the identity if Gaussian elimination failes
		spad_memset(Wcy, 0, sizeof(float)*6);
		Wcy[2] = 1.0f;
	}
#ifdef TG_ARCH
ocrPrintf("Wcy %x%x %x%x %x%x\n",
*(uint32_t *)(uint32_t *)(&Wcy[0])+0,
*(uint32_t *)(uint32_t *)(&Wcy[0])+1,
*(uint32_t *)(uint32_t *)(&Wcy[1])+0,
*(uint32_t *)(uint32_t *)(&Wcy[1])+1,
*(uint32_t *)(uint32_t *)(&Wcy[2])+0,
*(uint32_t *)(uint32_t *)(&Wcy[2])+1);RAG_FLUSH
ocrPrintf("Wcy %x%x %x%x %x%x\n",
*(uint32_t *)(uint32_t *)(&Wcy[3])+0,
*(uint32_t *)(uint32_t *)(&Wcy[3])+1,
*(uint32_t *)(uint32_t *)(&Wcy[4])+0,
*(uint32_t *)(uint32_t *)(&Wcy[4])+1,
*(uint32_t *)(uint32_t *)(&Wcy[5])+0,
*(uint32_t *)(uint32_t *)(&Wcy[5])+1);RAG_FLUSH
#else
fprintf(stderr,"Wcy %f %f %f %f %f %f\n",Wcy[0],Wcy[1],Wcy[2],Wcy[3],Wcy[4],Wcy[5]);fflush(stderr);
#endif

	// Loop over the output pixel locations and interpolate the Target image
	// pixel values at these points. This is done by mapping the (rectangular)
	// Source coordinates into the Target coordinates and performing the
	// interpolation there.

	spad_free(aug_mat,aug_mat_dbg);

#if defined(RAG_AFFINE_BLK_SIZE)
	/* The resample is a per-pixel interpolation, so a block of the
	 * backprojection's size carries a few microseconds of work and the
	 * per-task cost dominates it.  This phase therefore sets its own
	 * blocking factor rather than sharing the projection's. */
	int Xend = image_params->Ix;
	int Yend = image_params->Iy;
	int AFFINE_ASYNC_2_BLOCK_SIZE_X = RAG_AFFINE_BLK_SIZE;
	int AFFINE_ASYNC_2_BLOCK_SIZE_Y = RAG_AFFINE_BLK_SIZE;
#elif defined(RAG_NEW_BLK_SIZE)
	int Xend = image_params->Ix;
	int Yend = image_params->Iy;
	int AFFINE_ASYNC_2_BLOCK_SIZE_X = RAG_NEW_BLK_SIZE;
	int AFFINE_ASYNC_2_BLOCK_SIZE_Y = RAG_NEW_BLK_SIZE;
#else
	int Xend = image_params->Ix;
	int Yend = image_params->Iy;
	int AFFINE_ASYNC_2_BLOCK_SIZE_X = blk_size(Xend,32);
	int AFFINE_ASYNC_2_BLOCK_SIZE_Y = blk_size(Yend,32);
	assert( (Xend%AFFINE_ASYNC_2_BLOCK_SIZE_X) == 0);
	assert( (Yend%AFFINE_ASYNC_2_BLOCK_SIZE_Y) == 0);
#endif

#ifdef TRACE_LVL_3
ocrPrintf("////// create a template for affine_async_2_edt function\n");RAG_FLUSH;
#endif
ocrGuid_t affine_async_2_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv);
	ocrGuid_t affine_async_2_clg;
	retval = ocrEdtTemplateCreate(
			&affine_async_2_clg,	// ocrGuid_t *new_guid
			 affine_async_2_edt,	// ocr_edt_ptr func_ptr
			PRMNUM(affineAsync), // paramc
			RESAMPLE_STRIPE_SLOT0 + 2);	// depc
	assert(retval==0);
	RAG_TEMPLATE_REGISTER(affine_async_2_clg);

	int _nty = 0, _ntx = 0;
	for(int m=0; m<Yend; m+=AFFINE_ASYNC_2_BLOCK_SIZE_Y) _nty++;
	for(int n=0; n<Xend; n+=AFFINE_ASYNC_2_BLOCK_SIZE_X) _ntx++;
	const int _ntiles = _nty * _ntx;
	/* This task does not touch the current image -- it fits the transform and
	 * hands the resampling out -- so it takes the block by GUID only and has
	 * nothing to acquire, and nothing to release before queuing its writer. */
	ocrGuid_t _gather_clg, _gather_scg;
	afGatherPRM_t _gp;
	_gp.by = AFFINE_ASYNC_2_BLOCK_SIZE_Y; _gp.bx = AFFINE_ASYNC_2_BLOCK_SIZE_X;
	retval = ocrEdtTemplateCreate(&_gather_clg, affine_gather_edt,
	                              (sizeof(afGatherPRM_t)+sizeof(u64)-1)/sizeof(u64),
	                              3 + _ntiles);
	assert(retval==0);
	RAG_TEMPLATE_REGISTER(_gather_clg);
	ocrGuid_t _gather_evg;
	retval = ocrEdtCreate(&_gather_scg, _gather_clg, EDT_PARAM_DEF, (u64 *)&_gp,
	                      EDT_PARAM_DEF, NULL, EDT_PROP_NONE, NULL_HINT, &_gather_evg);
	assert(retval==0);
	/* The registered image's consumer waits on the gather's own event.  An
	   enclosing finish scope is not the right edge here: the consumer needs
	   the image, which is exactly what this task publishes. */
	retval = ocrAddDependence(_gather_evg, postAffineAsyncParamvIn->image_consumer_scg,
	                          AFFINE_IMAGE_CONSUMER_SLOT, DB_MODE_NULL);
	assert(retval==0);
	RAG_DEF_MACRO_PASS_RO(_gather_scg,NULL,NULL,NULL,NULL,image_params_dbg,0);
	RAG_DEF_MACRO_PASS(_gather_scg,NULL,NULL,NULL,NULL,curImage_dbg,1);
	/* The registered image's stripes, cut by the one task that holds it. */
	RAG_DEF_MACRO_PASS(_gather_scg,NULL,NULL,NULL,NULL,st_reg_dbg,2);
	int _tidx = 0;

	struct async_2_args_t *async_2_args_ptr; ocrGuid_t async_2_args_dbg;
	async_2_args_ptr = bsm_malloc(&async_2_args_dbg,sizeof(struct async_2_args_t));
	SPADtoBSM(async_2_args_ptr->Wcx,Wcx,6*sizeof(float));
	SPADtoBSM(async_2_args_ptr->Wcy,Wcy,6*sizeof(float));

	RAG_DEF_MACRO_PASS_RO(post_affine_async_2_scg,NULL,NULL,NULL,NULL,affine_params_dbg,0);
	RAG_DEF_MACRO_PASS_RO(post_affine_async_2_scg,NULL,NULL,NULL,NULL,image_params_dbg,1);
	RAG_DEF_MACRO_GUID_ONLY(post_affine_async_2_scg,curImage_dbg,2);
	RAG_DEF_MACRO_PASS_RO(post_affine_async_2_scg,NULL,NULL,NULL,NULL,post_affine_async_1_args_dbg,3);
	RAG_DEF_MACRO_PASS_RO(post_affine_async_2_scg,NULL,NULL,NULL,NULL,async_2_args_dbg,4);
	RAG_DEF_MACRO_PASS_RO(post_affine_async_2_scg,NULL,NULL,NULL,NULL,Fx_dbg,5);
	RAG_DEF_MACRO_PASS_RO(post_affine_async_2_scg,NULL,NULL,NULL,NULL,Fy_dbg,6);
	RAG_DEF_MACRO_PASS_RO(post_affine_async_2_scg,NULL,NULL,NULL,NULL,A_dbg,7);

#ifdef RAG_NEW_BLK_SIZE
	for(int m=0; m<Yend; m+=AFFINE_ASYNC_2_BLOCK_SIZE_Y) {
		for(int n=0; n<Xend; n+=AFFINE_ASYNC_2_BLOCK_SIZE_X) {
			struct corners_t async_corners;
			async_corners.m1   = m;
			async_corners.m2   = (m+AFFINE_ASYNC_2_BLOCK_SIZE_Y)<Yend?(m+AFFINE_ASYNC_2_BLOCK_SIZE_Y):Yend;
			async_corners.n1   = n;
			async_corners.n2   = (n+AFFINE_ASYNC_2_BLOCK_SIZE_X)<Xend?(n+AFFINE_ASYNC_2_BLOCK_SIZE_X):Xend;
#else
	for(int m=0; m<Yend; m+=AFFINE_ASYNC_2_BLOCK_SIZE_Y) {
		for(int n=0; n<Xend; n+=AFFINE_ASYNC_2_BLOCK_SIZE_X) {
			struct corners_t async_corners;
			async_corners.m1   = m;
			async_corners.m2   = m+AFFINE_ASYNC_2_BLOCK_SIZE_Y;
			async_corners.n1   = n;
			async_corners.n2   = n+AFFINE_ASYNC_2_BLOCK_SIZE_X;
#endif
#ifdef TRACE_LVL_3
ocrPrintf("////// create an edt for affine_async_2\n");RAG_FLUSH;
#endif
			/* The source rectangle this block interpolates from: a rigorous
			   bound on the warp over the block, widened by the bilinear tap
			   and clipped to the image.  The block is placed on the stripe
			   that holds it. */
			float _pylo, _pyhi, _pxlo, _pxhi;
			warp_bound(Wcy, async_corners.n1, async_corners.n2-1,
			                async_corners.m1, async_corners.m2-1, &_pylo, &_pyhi);
			warp_bound(Wcx, async_corners.n1, async_corners.n2-1,
			                async_corners.m1, async_corners.m2-1, &_pxlo, &_pxhi);
			int _sy0 = (int)floorf(_pylo),      _sy1 = (int)floorf(_pyhi) + 2;
			int _sx0 = (int)floorf(_pxlo),      _sx1 = (int)floorf(_pxhi) + 2;
			if(_sy0 < 0) _sy0 = 0; if(_sy1 > image_params->Iy) _sy1 = image_params->Iy;
			if(_sx0 < 0) _sx0 = 0; if(_sx1 > image_params->Ix) _sx1 = image_params->Ix;
			if(_sy1 <= _sy0) { _sy0 = 0; _sy1 = 1; }
			if(_sx1 <= _sx0) { _sx0 = 0; _sx1 = 1; }
			affineAsyncPRM_t affineAsyncParamv;
			affineAsyncParamv.corners = async_corners;
			affineAsyncParamv.sy0 = _sy0; affineAsyncParamv.sy1 = _sy1;
			affineAsyncParamv.sx0 = _sx0; affineAsyncParamv.sx1 = _sx1;
			if(_sy1 - _sy0 > RAG_AFFINE_MAX_SRC || _sx1 - _sx0 > RAG_AFFINE_MAX_SRC) {
				ocrPrintf("resample source %dx%d exceeds RAG_AFFINE_MAX_SRC %d\n",
				          _sy1-_sy0, _sx1-_sx0, RAG_AFFINE_MAX_SRC);RAG_FLUSH;
				xe_exit(1);
			}
			const int _ss0 = img_stripe_of(st_cur, _sy0);
			const int _ss1 = img_stripe_of(st_cur, _sy1 - 1);
			if(_ss1 - _ss0 > 1) {
				ocrPrintf("resample source spans %d stripes; widen IMG_MIN_STRIPE\n", _ss1-_ss0+1);RAG_FLUSH;
				xe_exit(1);
			}
			ocrHint_t _hnt;
			ocrGuid_t affine_async_2_scg, _tile_evg;
			retval = ocrEdtCreate(
					&affine_async_2_scg,	// *created_edt_guid
					 affine_async_2_clg,	// edt_template_guid
					EDT_PARAM_DEF,		// paramc
					(u64 *)&affineAsyncParamv, // *paramv
					EDT_PARAM_DEF,		// depc
					NULL,			// *depv
					EDT_PROP_NONE,		// properties
					img_stripe_hint(&_hnt, st_cur, _ss0), // affinity
					&_tile_evg);		// *outputEvent
			assert(retval==0);
			retval = ocrAddDependence(_tile_evg, _gather_scg, 3 + _tidx, DB_MODE_RO);
			assert(retval==0);
			_tidx++;

RAG_DEF_MACRO_PASS_RO(affine_async_2_scg,NULL,NULL,NULL,NULL,affine_params_dbg,0);
RAG_DEF_MACRO_PASS_RO(affine_async_2_scg,NULL,NULL,NULL,NULL,image_params_dbg,1);
RAG_DEF_MACRO_PASS_RO(affine_async_2_scg,NULL,NULL,NULL,NULL,st_cur_dbg,2);
RAG_DEF_MACRO_PASS_RO(affine_async_2_scg,NULL,NULL,NULL,NULL,async_2_args_dbg,3);
			for(int _k = 0; _k < 2; _k++) {
				if(_ss0 + _k <= _ss1) {
RAG_DEF_MACRO_PASS_RO(affine_async_2_scg,NULL,NULL,NULL,NULL,st_cur->g[_ss0+_k],RESAMPLE_STRIPE_SLOT0+_k);
				} else {
RAG_DEF_MACRO_GUID_ONLY(affine_async_2_scg,st_cur_dbg,RESAMPLE_STRIPE_SLOT0+_k);
				}
			}
		} // for n
	} // for m

#ifdef TRACE_LVL_3
ocrPrintf("////// leave post_affine_async_1_edt\n");RAG_FLUSH;
#endif
	return NULL_GUID;
}

ocrGuid_t affine_async_1_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv) {
	int retval;
	assert(paramc==0);
	assert(depc==CTRLPT_STRIPE_SLOT0 + 4);
RAG_REF_MACRO_SPAD(struct async_1_args_t,async_1_args,async_1_args_ptr,async_1_args_lcl,async_1_args_dbg,0);
RAG_REF_MACRO_SPAD(struct AffineParams,affine_params,affine_params_ptr,affine_params_lcl,affine_params_dbg,1);
RAG_REF_MACRO_SPAD(struct ImageParams,image_params,image_params_ptr,image_params_lcl,image_params_dbg,2);
RAG_REF_MACRO_BSM( const struct img_stripes_s *,st_cur,NULL,NULL,st_cur_dbg,3);
RAG_REF_MACRO_BSM( const struct img_stripes_s *,st_ref,NULL,NULL,st_ref_dbg,4);

	struct point ctrl_pt = async_1_args->ctrl_pt;
#ifdef TRACE_LVL_3
ocrPrintf("////// enter affine_async_1_edt\n");RAG_FLUSH;
#endif
#if defined(TRACE_LVL_3) && !defined(TG_ARCH)
printf("////// enter affine_async_1_edt ctrl_pt %d %d %f\n",ctrl_pt.x,ctrl_pt.y,ctrl_pt.p);RAG_FLUSH;
#endif
	// disp_vec
	// disp_vec
	struct point disp_vec;

#ifdef TRACE_LVL_3
ocrPrintf("////// Perform 2D correlation\n");RAG_FLUSH;
#endif
	/* Assemble this point's two windows out of the stripes that cover them.
	   Both are a few tens of kilobytes; the images themselves stay where they
	   were produced. */
	const int _cn = affine_params->Sc;
	const int _rn = affine_params->Sc + 2*affine_params->Rc;
	if(_cn > RAG_CTRLPT_MAX_WIN || _rn > RAG_CTRLPT_MAX_WIN) {
		ocrPrintf("correlation window %d exceeds RAG_CTRLPT_MAX_WIN %d\n",
		          _rn > _cn ? _rn : _cn, RAG_CTRLPT_MAX_WIN);RAG_FLUSH;
		xe_exit(1);
	}
	struct complexData _wdata[2*(size_t)RAG_CTRLPT_MAX_WIN*(size_t)RAG_CTRLPT_MAX_WIN];
	struct ctrlpt_win_s _win;
	struct ctrlpt_win_s *const win = &_win;
	win->cur_n  = _cn;
	win->ref_n  = _rn;
	win->cur_y0 = ctrl_pt.y - (_cn-1)/2;
	win->cur_x0 = ctrl_pt.x - (_cn-1)/2;
	win->ref_y0 = ctrl_pt.y - (_rn-1)/2;
	win->ref_x0 = ctrl_pt.x - (_rn-1)/2;
	win->cur    = _wdata;
	win->ref    = _wdata + (size_t)_cn*(size_t)_cn;
	img_stripes_read(st_cur, depv, CTRLPT_STRIPE_SLOT0,
	                 win->cur_y0, win->cur_y0 + _cn,
	                 win->cur_x0, win->cur_x0 + _cn, _wdata);
	img_stripes_read(st_ref, depv, CTRLPT_STRIPE_SLOT0 + 2,
	                 win->ref_y0, win->ref_y0 + _rn,
	                 win->ref_x0, win->ref_x0 + _rn,
	                 _wdata + (size_t)_cn*(size_t)_cn);

	disp_vec = corr2D(ctrl_pt, affine_params->Sc, affine_params->Rc,
				win, image_params);
#ifdef TRACE_LVL_3
ocrPrintf("////// Only retain control points that exceed the threshold\n");RAG_FLUSH;
#endif
	/* Report into this task's OWN block instead of appending into shared
	   arrays under an atomic.  The block is created here, so it is homed
	   where it is written, and the reducer fetches each one once. */
	ocrGuid_t res_dbg; struct ctrlpt_res_s *res = NULL;
	retval = ocrDbCreate(&res_dbg, (void **)&res, sizeof(*res),
	                     0, NULL_HINT, NO_ALLOC);
	assert(retval==0);
	if(disp_vec.p >= affine_params->Tc) {
		res->keep = 1;
		res->fx = ctrl_pt.x + disp_vec.x;
		res->fy = ctrl_pt.y + disp_vec.y;
		res->a[0] = 1;
		res->a[1] = ctrl_pt.x;
		res->a[2] = ctrl_pt.y;
		res->a[3] = ctrl_pt.x*ctrl_pt.x;
		res->a[4] = ctrl_pt.y*ctrl_pt.y;
		res->a[5] = ctrl_pt.x*ctrl_pt.y;
	} else {
		res->keep = 0;
	}
	/* Release before the output event carries it on, so the reducer cannot
	   observe pre-release bytes. */
	ocrDbRelease(res_dbg);
	bsm_free(async_1_args,async_1_args_dbg);
#ifdef TRACE_LVL_3
ocrPrintf("////// leave affine_async_1_edt\n");RAG_FLUSH;
#endif
	return res_dbg;
}

ocrGuid_t post_affine_async_2_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv) {
	int retval;
#ifdef TRACE_LVL_3
ocrPrintf("////// enter post_affine_async_2_edt\n");RAG_FLUSH;
#endif
	assert(paramc==0);
	assert(depc==9); // 9th post_affine_async_1_evg
RAG_REF_MACRO_PASS(struct AffineParams,affine_params,affine_params_ptr,affine_params_lcl,affine_params_dbg,0);
RAG_REF_MACRO_PASS(struct ImageParams,image_params,image_params_ptr,image_params_lcl,image_params_dbg,1);
RAG_REF_MACRO_BSM( struct complexData **,curImage,NULL,NULL,curImage_dbg,2);
RAG_REF_MACRO_SPAD(struct async_1_args_t,post_affine_async_1_args,post_affine_async_1_args_ptr,post_affine_async_1_args_lcl,post_affine_async_1_args_dbg,3);
RAG_REF_MACRO_SPAD(struct async_2_args_t,async_2_args,async_2_args_ptr,async_2_args_lcl,async_2_args_dbg,4);
RAG_REF_MACRO_BSM( int *,Fx,NULL,NULL,Fx_dbg,5);
RAG_REF_MACRO_BSM( int *,Fy,NULL,NULL,Fy_dbg,6);
RAG_REF_MACRO_BSM( int **,A,NULL,NULL,A_dbg,7);


#ifdef TRACE_LVL_3
ocrPrintf("// Free data blocks\n");RAG_FLUSH;
#endif
	bsm_free(A, A_dbg);
	bsm_free(Fy,Fy_dbg);
	bsm_free(Fx,Fx_dbg);

	bsm_free(post_affine_async_1_args_ptr,post_affine_async_1_args_dbg);
	bsm_free(async_2_args_ptr,async_2_args_dbg);

#ifdef TRACE_LVL_3
ocrPrintf("////// leave post_affine_async_2_edt\n");RAG_FLUSH;
#endif
	return NULL_GUID;
}

ocrGuid_t affine_async_2_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv) {
	int retval;
    affineAsyncPRM_t *affineAsyncParamvIn = (affineAsyncPRM_t *)paramv;
#ifdef TRACE_LVL_3
ocrPrintf("////// enter affine_async_2_edt\n");RAG_FLUSH;
#endif
	assert(PRMNUM(affineAsync));
	struct corners_t *corners = &(affineAsyncParamvIn->corners);
	int m1   = corners->m1;
	int m2   = corners->m2;
	int n1   = corners->n1;
	int n2   = corners->n2;
	assert(depc==RESAMPLE_STRIPE_SLOT0 + 2);
RAG_REF_MACRO_PASS(struct AffineParams,affine_params,affine_params_ptr,affine_params_lcl,affine_params_dbg,0);
RAG_REF_MACRO_SPAD(struct ImageParams,image_params,image_params_ptr,image_params_lcl,image_params_dbg,1);
RAG_REF_MACRO_BSM( const struct img_stripes_s *,st_cur,NULL,NULL,st_cur_dbg,2);
RAG_REF_MACRO_SPAD(struct async_2_args_t,async_2_args,async_2_args_ptr,async_2_args_lcl,async_2_args_dbg,3);
	/* This block's own rectangle, created where its bytes are produced. */
	ocrGuid_t tile_dbg; struct complexData *tile = NULL;
	retval = ocrDbCreate(&tile_dbg, (void **)&tile,
	                     (size_t)(m2-m1)*(size_t)(n2-n1)*sizeof(struct complexData),
	                     0, NULL_HINT, NO_ALLOC);
	assert(retval==0);

	/* This block's source rectangle, assembled out of the stripes that cover
	   it.  The bound was computed where the block was created, so every tap
	   the interpolation below takes falls inside it. */
	const int _sy0 = affineAsyncParamvIn->sy0, _sy1 = affineAsyncParamvIn->sy1;
	const int _sx0 = affineAsyncParamvIn->sx0, _sx1 = affineAsyncParamvIn->sx1;
	const int _snx = _sx1 - _sx0;
	if(_sy1 - _sy0 > RAG_AFFINE_MAX_SRC || _sx1 - _sx0 > RAG_AFFINE_MAX_SRC) {
		ocrPrintf("resample source out of bound\n");RAG_FLUSH; xe_exit(1);
	}
	struct complexData _src[(size_t)RAG_AFFINE_MAX_SRC * (size_t)RAG_AFFINE_MAX_SRC];
	img_stripes_read(st_cur, depv, RESAMPLE_STRIPE_SLOT0, _sy0, _sy1, _sx0, _sx1, _src);
#define SRC_AT(y,x) _src[(size_t)((y) - _sy0)*(size_t)_snx + (size_t)((x) - _sx0)]
	const int tstride = n2 - n1;

	int aa, bb;
	float Px, Py, w, v;

	float Wcx[6];
	SPADtoSPAD(Wcx,async_2_args->Wcx,6*sizeof(float));
#if defined(DEBUG) && !defined(TG_ARCH)
printf("wCX = %f %f %f %f %f %f\n", Wcx[0], Wcx[1], Wcx[2], Wcx[3], Wcx[4], Wcx[5]);RAG_FLUSH;
#endif
	float Wcy[6];
	SPADtoSPAD(Wcy,async_2_args->Wcy,6*sizeof(float));
#if defined(DEBUG) && !defined(TG_ARCH)
printf("wCY = %f %f %f %f %f %f\n", Wcy[0], Wcy[1], Wcy[2], Wcy[3], Wcy[4], Wcy[5]);RAG_FLUSH;
#endif
	for(int m=m1; m<m2; m++) {
		for(int n=n1; n<n2; n++) {
			const float m_flt = (float)m;
			const float n_flt = (float)n;
			Px = Wcx[0] + Wcx[1]*n_flt + Wcx[2]*m_flt
			   + Wcx[3]*n_flt*n_flt + Wcx[4]*m_flt*m_flt + Wcx[5]*n_flt*m_flt;
			Py = Wcy[0] + Wcy[1]*(float)n + Wcy[2]*(float)m
			   + Wcy[3]*n_flt*n_flt + Wcy[4]*m_flt*m_flt + Wcy[5]*n_flt*m_flt;

			aa = (int)floorf(Py);
			bb = (int)floorf(Px);
			w = Py - (float)aa;
			v = Px - (float)bb;

			if( (aa >= 0) && (aa < image_params->Iy-1)
			&& (bb >= 0) && (bb < image_params->Ix-1) ) {
				tile[(m-m1)*tstride+(n-n1)].real 	= (1-v)*(1-w)*SRC_AT(aa  ,bb  ).real
							+ (  v)*(1-w)*SRC_AT(aa  ,bb+1).real
							+ (1-v)*(  w)*SRC_AT(aa+1,bb  ).real
							+ (  v)*(  w)*SRC_AT(aa+1,bb+1).real;
				tile[(m-m1)*tstride+(n-n1)].imag	= (1-v)*(1-w)*SRC_AT(aa  ,bb  ).imag
							+ (  v)*(1-w)*SRC_AT(aa  ,bb+1).imag
							+ (1-v)*(  w)*SRC_AT(aa+1,bb  ).imag
							+ (  v)*(  w)*SRC_AT(aa+1,bb+1).imag;
			} else {
				tile[(m-m1)*tstride+(n-n1)].real = 0.0f;
				tile[(m-m1)*tstride+(n-n1)].imag = 0.0f;
			}
		} // for n
	} // for m

#ifdef TRACE_LVL_3
ocrPrintf("////// leave affine_async_2_edt\n");RAG_FLUSH;
#endif
#undef SRC_AT
	ocrDbRelease(tile_dbg);
	return tile_dbg;
}

ocrGuid_t Affine(
    const struct img_stripes_s *st_cur,	ocrGuid_t st_cur_dbg,
    const struct img_stripes_s *st_ref,	ocrGuid_t st_ref_dbg,
    ocrGuid_t curImage_dbg,   /* forwarded to the resample, which writes it */
    struct AffineParams *affine_params,	ocrGuid_t affine_params_dbg, struct AffineParams *affine_params_ptr,
    struct ImageParams *image_params,	ocrGuid_t image_params_dbg,
    ocrGuid_t post_affine_async_1_scg)
{
	int retval;
#ifdef TRACE_LVL_2
ocrPrintf("//// enter Affine\n");RAG_FLUSH;
#endif
	int N, min;
	int dx, dy;
#ifdef TRACE_LVL_2
ocrPrintf("//// Affine registration dynamically allocated variables size = %d\n",affine_params->Nc);RAG_FLUSH;
#endif
	int **A;
	int *Fx, *Fy;

	// Capture the control-point count A/Fx/Fy are about to be sized with,
	// before it is reset to zero below and re-counted via __sync_fetch_and_add.
	// The async EDTs use it to rebuild A's row-pointer table after relocation.
	const int Nc_alloc = affine_params->Nc;

	// Fx = Nc x 1

	ocrGuid_t Fx_dbg;
	Fx = (int*)bsm_calloc(&Fx_dbg,affine_params->Nc,sizeof(int));
	if(Fx == NULL) {
		ocrPrintf("Unable to allocate memory for Fx.\n");RAG_FLUSH;
		xe_exit(1);
	}

	// Fy = Nc x 1

	ocrGuid_t Fy_dbg;
	Fy = (int*)bsm_calloc(&Fy_dbg,affine_params->Nc,sizeof(int));
	if(Fy == NULL) {
		ocrPrintf("Unable to allocate memory for Fy.\n");RAG_FLUSH;
		xe_exit(1);
	}

	// A = Nc x 6

	ocrGuid_t A_dbg;
	A = (int**)bsm_malloc(&A_dbg,(affine_params->Nc)*sizeof(int *)
				    +(affine_params->Nc)*6*sizeof(int));
	if(A == NULL) {
		ocrPrintf("Error allocating memory for A.\n");RAG_FLUSH;
		xe_exit(1);
	}
	int *A_data_ptr = (int *)&A[affine_params->Nc];
	if(A_data_ptr == NULL) {
		ocrPrintf("Unable to allocate memory for A.\n");RAG_FLUSH;
		xe_exit(1);
	}
	for(int m=0;m<affine_params->Nc;m++) {
	        A[m] = A_data_ptr + m*6;
	        for(int i=0;i<6;i++) {
			A[m][i] = 0;
		}
	}



#ifdef TRACE_LVL_2
ocrPrintf("//// Calculate misc. parameters\n");RAG_FLUSH;
#endif
	N = (int)sqrtf((float)affine_params->Nc);
#ifdef TRACE_LVL_2
ocrPrintf("//// N = %d (sqrtf(%d))\n",N,affine_params->Nc);RAG_FLUSH;
#endif
	min = (affine_params->Sc-1)/2 + affine_params->Rc;

	/* The correlation's footprint per point: an Sc square of the current image
	   and an (Sc + 2*Rc) square of the reference.  `min` above is exactly the
	   larger half-extent, so every window is whole. */
	const int _win_cur_n = affine_params->Sc;
	const int _win_ref_n = affine_params->Sc + 2*affine_params->Rc;

	dy = (image_params->Iy - affine_params->Sc + 1 - 2*affine_params->Rc)/N;
	dx = (image_params->Ix - affine_params->Sc + 1 - 2*affine_params->Rc)/N;

#ifdef TRACE
ocrPrintf("setting DRAM version of Nc to zero, will be updated with __sync_fetch_and_add() \n");RAG_FLUSH;
#endif
	affine_params->Nc = 0; 				// local copy of Nc
	affine_params_ptr->Nc = affine_params->Nc;	// global copy of Nc

#ifdef TRACE_LVL_2
ocrPrintf("//// create a template for affine_async_1_edt function (N=%d)\n",N);RAG_FLUSH;
#endif
	ocrGuid_t affine_async_1_clg;
	retval = ocrEdtTemplateCreate(
			&affine_async_1_clg,	// ocrGuid_t *new_guid
			 affine_async_1_edt,	// ocr_edt_ptr func_ptr
			0,			// paramc
			CTRLPT_STRIPE_SLOT0 + 4);	// depc
	assert(retval==0);
	RAG_TEMPLATE_REGISTER(affine_async_1_clg);

#ifdef TRACE_LVL_2
ocrPrintf("//// create a ctrl_pt affine_async_1_edt\n");RAG_FLUSH;
#endif
	struct point ctrl_pt;
	ctrl_pt.y =  0;
	ctrl_pt.x =  0;
	ctrl_pt.p = -1;
#ifdef TRACE_LVL_2
ocrPrintf("//// create a ctrl_pt post_affine_async_1_edt\n");RAG_FLUSH;
#endif
	struct async_1_args_t *post_affine_async_1_args_ptr; ocrGuid_t post_affine_async_1_args_dbg;
	post_affine_async_1_args_ptr = bsm_malloc(&post_affine_async_1_args_dbg,sizeof(struct async_1_args_t));
	post_affine_async_1_args_ptr->ctrl_pt = ctrl_pt;
	post_affine_async_1_args_ptr->Nc_alloc         = Nc_alloc;

#ifdef TRACE_LVL_2
ocrPrintf("//// statisy post_affine_async_1_edt\n");RAG_FLUSH;
#endif
	RAG_DEF_MACRO_PASS_RO(post_affine_async_1_scg,NULL,NULL,NULL,NULL,post_affine_async_1_args_dbg,0);
	RAG_DEF_MACRO_PASS_RO(post_affine_async_1_scg,NULL,NULL,NULL,NULL,affine_params_dbg,1);
	RAG_DEF_MACRO_PASS_RO(post_affine_async_1_scg,NULL,NULL,NULL,NULL,image_params_dbg,2);
	RAG_DEF_MACRO_GUID_ONLY(post_affine_async_1_scg,curImage_dbg,3);
	RAG_DEF_MACRO_PASS_RO(post_affine_async_1_scg,NULL,NULL,NULL,NULL,Fx_dbg,4);
	RAG_DEF_MACRO_PASS_RO(post_affine_async_1_scg,NULL,NULL,NULL,NULL,Fy_dbg,5);
	RAG_DEF_MACRO_PASS_RO(post_affine_async_1_scg,NULL,NULL,NULL,NULL,A_dbg,6);

	for(int m=0; m<N; m++) {
		for(int n=0; n<N; n++) {
#ifdef TRACE_LVL_2
ocrPrintf("//// create an edt for affine_async_1 m=%d n=%d\n",m,n);RAG_FLUSH;
#endif
			struct point ctrl_pt;
			ctrl_pt.y = m*dy + min;
			ctrl_pt.x = n*dx + min;
			struct async_1_args_t *async_1_args_ptr; ocrGuid_t async_1_args_dbg;
			async_1_args_ptr = bsm_malloc(&async_1_args_dbg,sizeof(struct async_1_args_t));
			async_1_args_ptr->ctrl_pt = ctrl_pt;
			async_1_args_ptr->Nc_alloc         = Nc_alloc;

			/* The stripes this point's two windows fall in.  The task is
			   placed on the stripe that holds it, so the piece it reads is
			   already there and its neighbours on that rank share it. */
			const int _cy0 = ctrl_pt.y - (_win_cur_n-1)/2;
			const int _ry0 = ctrl_pt.y - (_win_ref_n-1)/2;
			const int _cs0 = img_stripe_of(st_cur, _cy0);
			const int _cs1 = img_stripe_of(st_cur, _cy0 + _win_cur_n - 1);
			const int _rs0 = img_stripe_of(st_ref, _ry0);
			const int _rs1 = img_stripe_of(st_ref, _ry0 + _win_ref_n - 1);
			assert(_cs1 - _cs0 <= 1 && _rs1 - _rs0 <= 1);
			ocrHint_t _hnt;
			ocrGuid_t affine_async_1_scg, affine_async_1_evg;
			retval = ocrEdtCreate(
					&affine_async_1_scg,	// *created_edt_guid
					 affine_async_1_clg,	// edt_template_guid
					EDT_PARAM_DEF,		// paramc
					NULL,			// *paramv
					EDT_PARAM_DEF,		// depc
					NULL,			// *depv
					EDT_PROP_NONE,		// properties
					img_stripe_hint(&_hnt, st_ref, _rs0), // affinity
					&affine_async_1_evg);	// *outputEvent
			assert(retval==0);
			/* This point's result reaches the reducer through its own event,
			   at a slot fixed by its position in the grid. */
			retval = ocrAddDependence(affine_async_1_evg, post_affine_async_1_scg,
			                          CTRLPT_RES_SLOT0 + (m*N + n), DB_MODE_RO);
			assert(retval==0);

RAG_DEF_MACRO_PASS_RO(affine_async_1_scg,NULL,NULL,NULL,NULL,async_1_args_dbg,0);
/* Read-only now: the task reports through its own block, so it no longer
   writes the shared counter or the shared Fx/Fy/A arrays. */
RAG_DEF_MACRO_PASS_RO(affine_async_1_scg,NULL,NULL,NULL,NULL,affine_params_dbg,1);
RAG_DEF_MACRO_PASS_RO(affine_async_1_scg,NULL,NULL,NULL,NULL,image_params_dbg,2);
RAG_DEF_MACRO_PASS_RO(affine_async_1_scg,NULL,NULL,NULL,NULL,st_cur_dbg,3);
RAG_DEF_MACRO_PASS_RO(affine_async_1_scg,NULL,NULL,NULL,NULL,st_ref_dbg,4);
			/* Two slots per image so the shape is fixed; a window that falls
			   inside one stripe leaves the second slot carrying nothing. */
			for(int _k = 0; _k < 2; _k++) {
				if(_cs0 + _k <= _cs1) {
RAG_DEF_MACRO_PASS_RO(affine_async_1_scg,NULL,NULL,NULL,NULL,st_cur->g[_cs0+_k],CTRLPT_STRIPE_SLOT0+_k);
				} else {
RAG_DEF_MACRO_GUID_ONLY(affine_async_1_scg,st_cur_dbg,CTRLPT_STRIPE_SLOT0+_k);
				}
				if(_rs0 + _k <= _rs1) {
RAG_DEF_MACRO_PASS_RO(affine_async_1_scg,NULL,NULL,NULL,NULL,st_ref->g[_rs0+_k],CTRLPT_STRIPE_SLOT0+2+_k);
				} else {
RAG_DEF_MACRO_GUID_ONLY(affine_async_1_scg,st_ref_dbg,CTRLPT_STRIPE_SLOT0+2+_k);
				}
			}
			OCR_DB_RELEASE(async_1_args_dbg);
		} // for n
	} // for m

#ifdef TRACE_LVL_2
ocrPrintf("//// leave Affine\n");RAG_FLUSH;
#endif
    return NULL_GUID;
}

struct point corr2D(struct point ctrl_pt, int Nwin, int R, const struct ctrlpt_win_s *win, struct ImageParams *image_params)
{
    const struct complexData *_cw = win->cur;
    const struct complexData *_rw = win->ref;
    int m, n, i, j, k;
    float den1, den2;
    float rho;
    struct point pt;
    struct complexData num;
    struct complexData *f, *g;
    struct complexData mu_f, mu_g;
#ifdef TRACE_LVL_4
        ocrPrintf("//////// corr2D ctrl_pt.x %d ctrl_pt.y %d\n",ctrl_pt.x,ctrl_pt.y);RAG_FLUSH;
#endif
    ocrGuid_t f_dbg;
    f = (struct complexData*)spad_malloc(&f_dbg,Nwin*Nwin*sizeof(struct complexData));
    ocrGuid_t g_dbg;
    g = (struct complexData*)spad_malloc(&g_dbg,Nwin*Nwin*sizeof(struct complexData));

    if( f == NULL || g == NULL ) {
        ocrPrintf("Unable to allocate memory for correlation windows.\n");RAG_FLUSH;
        xe_exit(1);
    }

    for(i=ctrl_pt.y-(Nwin-1)/2, k=mu_f.real=mu_f.imag=0; i<=ctrl_pt.y+(Nwin-1)/2; i++)
    {
        for(j=ctrl_pt.x-(Nwin-1)/2; j<=ctrl_pt.x+(Nwin-1)/2; j++, k++)
        {
            if( (i < 0) || (j < 0) || (i >= image_params->Iy) || (j >= image_params->Ix) ) {
                ocrPrintf("Warning: Index out of bounds in registration correlation.\n");RAG_FLUSH;
            }
            const struct complexData _v =
                _cw[(size_t)(i - win->cur_y0)*(size_t)win->cur_n + (size_t)(j - win->cur_x0)];
            f[k].real  = _v.real;
            f[k].imag  = _v.imag;
            mu_f.real += _v.real;
            mu_f.imag += _v.imag;
        }
    }

    mu_f.real /= Nwin*Nwin;
    mu_f.imag /= Nwin*Nwin;

    for(k=0; k<Nwin*Nwin; k++)
    {
        f[k].real -= mu_f.real;
        f[k].imag -= mu_f.imag;
    }

    for(m=ctrl_pt.y-R, pt.p=0; m<=ctrl_pt.y+R; m++)
    {
        for(n=ctrl_pt.x-R; n<=ctrl_pt.x+R; n++)
        {
            for(i=m-(Nwin-1)/2, k=mu_g.real=mu_g.imag=0; i<=m+(Nwin-1)/2; i++)
            {
                for(j=n-(Nwin-1)/2; j<=n+(Nwin-1)/2; j++, k++)
                {
                    if( (i < 0) || (j < 0) || (i >= image_params->Iy) || (j >= image_params->Ix) ) {
                        ocrPrintf("Warning: Index out of bounds in registration correlation.\n");RAG_FLUSH;
                    }
                    const struct complexData _v =
                        _rw[(size_t)(i - win->ref_y0)*(size_t)win->ref_n + (size_t)(j - win->ref_x0)];
                    g[k].real  = _v.real;
                    g[k].imag  = _v.imag;
                    mu_g.real += _v.real;
                    mu_g.imag += _v.imag;
                }
            }

            mu_g.real /= Nwin*Nwin;
            mu_g.imag /= Nwin*Nwin;

            for(k=num.real=num.imag=den1=den2=0; k<Nwin*Nwin; k++)
            {
                g[k].real -= mu_g.real;
                g[k].imag -= mu_g.imag;

                num.real += f[k].real*g[k].real + f[k].imag*g[k].imag;
                num.imag += f[k].real*g[k].imag - f[k].imag*g[k].real;

                den1 += f[k].real*f[k].real + f[k].imag*f[k].imag;
                den2 += g[k].real*g[k].real + g[k].imag*g[k].imag;
            }

            if(den1 != 0.0 && den2 != 0.0) {
                rho = sqrtf( (num.real*num.real + num.imag*num.imag) / (den1*den2) );
            }
            else {
                rho = 0.0;
            }

/*          if(rho < 0 || rho > 1) {
                ocrPrintf("Correlation value out of range.\n");fflush(stderr); RAG_FLUSH;
                xe_exit(1);
            }*/

            if(rho > pt.p) {
                pt.x = ctrl_pt.x-n;
                pt.y = ctrl_pt.y-m;
                pt.p = rho;
            }
        }
    }

    spad_free(g,g_dbg);
    spad_free(f,f_dbg);

    return pt;
}

// Returns 0 on success and non-zero on failure
int gauss_elim(float *AA[], float *x, int N)
{
	int i, j, k, max;
	float **a, temp;
#ifdef TRACE_LVL_4
        ocrPrintf("//////// gauss_elim\n");RAG_FLUSH;
#endif
	ocrGuid_t a_dbg;
	a = (float **)spad_malloc(&a_dbg,(N)*sizeof(float *)
				       +(N)*(N+1)*sizeof(float));
	if(a == NULL) {
		ocrPrintf("Unable to allocate memory for a.\n");RAG_FLUSH;
		xe_exit(1);
	}
        float *a_data_ptr = (float *)&a[N];
        if(a_data_ptr == NULL) {
            ocrPrintf("Unable to allocate memory for a.\n");RAG_FLUSH;
            xe_exit(1);
        }
	for(i=0; i<N; i++)
		a[i] = a_data_ptr + i*(N+1);

	for(i=0; i<N; i++) {
		for(j=0; j<N+1; j++) {
			a[i][j] = AA[i][j];
		}
	}

	for(i=0; i<N; i++) {
        // Find the largest value row
		max = i;
		for(j=i+1; j<N; j++) {
			if(fabsf(a[j][i]) > fabsf(a[max][i])) {
				max = j;
			}
        	}

        // Swap the largest row with the ith row
		for(k=i; k<N+1; k++) {
			temp = a[i][k];
			a[i][k] = a[max][k];
			a[max][k] = temp;
		}

        // Check to see if this is a singular matrix
		if(fabsf(a[i][i]) == 0.0) {
			ocrPrintf("Warning: Encountered a singular matrix in registration correlation.\n");RAG_FLUSH;
			spad_free(a,a_dbg);
			return GAUSS_ELIM_SINGULAR_MATRIX;
		}

        // Starting from row i+1, eliminate the elements of the ith column
		for(j=i+1; j<N; j++) {
			if(a[j][i] != 0) {
				for(k=N; k>=i; k--) {
					a[j][k] -= a[i][k] * a[j][i] / a[i][i];
                		}
            		}
        	}
	}

	// Perform the back substitution
	for(j=N-1; j>=0; j--) {
		temp = 0;
		for(k=j+1; k<N; k++) {
			temp += a[j][k] * x[k];
		}
		x[j] = (a[j][N] - temp) / a[j][j];
	}

	spad_free(a,a_dbg);
	return GAUSS_ELIM_SUCCESS;
}
