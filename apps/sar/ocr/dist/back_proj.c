#include "ocr.h"
#include "rag_ocr.h"
#include "common.h"

#ifndef TG_ARCH // FIX-ME
#define dram_free(addr,guid)
#define  bsm_free(addr,guid)
#define spad_free(addr,guid)
#endif

ocrGuid_t backproject_async_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv) {
  int retval;
  backProjAsyncPRM_t *backProjAsyncParamvIn = (backProjAsyncPRM_t *)paramv;
#ifdef TRACE_LVL_4
  ocrPrintf("//////// enter backproject_async\n");RAG_FLUSH;
#endif
  assert(paramc==(PRMNUM(backProjAsync)));
  struct corners_t *corners = &(backProjAsyncParamvIn->corners);
  int m1   = corners->m1;
  int m2   = corners->m2;
  int n1   = corners->n1;
  int n2   = corners->n2;
  assert(depc==5);
  RAG_REF_MACRO_SPAD(struct ImageParams,image_params,image_params_ptr,image_params_lcl,image_params_dbg,0);
  RAG_REF_MACRO_SPAD(struct RadarParams,radar_params,radar_params_ptr,radar_params_lcl,radar_params_dbg,1);
  /* Slot 2 is this tile's own rectangle, not the image.  The base program
   * hands every tile the whole image RW; write permission is exclusive per
   * policy domain, so the tiles queue for it and each hand-off carries the
   * whole image.  A tile only ever touches its own [m1,m2) x [n1,n2), so it
   * gets a block of exactly that and the image is assembled once, later. */
  RAG_REF_MACRO_BSM( struct complexData **,Xin,NULL,NULL,Xin_dbg,2);
  RAG_REF_MACRO_BSM( float **,platpos,NULL,NULL,platpos_dbg,3);
  RAG_REF_MACRO_BSM( float *,Tp,NULL,NULL,Tp_dbg,4);
  /* This tile's own rectangle, created here so that it is homed where its
     bytes are produced: the gather then fetches it once instead of the task
     fetching it from the spawner and the gather fetching it back. */
  ocrGuid_t tile_dbg; struct complexData *tile = NULL;
  retval = ocrDbCreate(&tile_dbg, (void **)&tile,
                       (size_t)(m2-m1)*(size_t)(n2-n1)*sizeof(struct complexData),
                       0, NULL_HINT, NO_ALLOC);
  assert(retval==0);

  // Rebuild the row-pointer tables against this node's copies before any
  // image[m][n] / Xin[k][b] / platpos[k][c] access.  Xin is X (P3==P1 rows,
  // S4==S1 stride when F==1) or the upsampled Xup (P3 rows, S4 stride) — both
  // covered by (P3, S4).
  RAG_REMAP_2D(Xin,     image_params->P3, image_params->S4, struct complexData);
  RAG_REMAP_2D(platpos, image_params->P1, 3,                float);

  // Rebuild image_params->xr / ->yr locally (stale sibling-DB pointers under
  // relocation) before the xr/yr reads in the distance computation below.
  float rag_xr[image_params->Ix];
  float rag_yr[image_params->Iy];
  RAG_REBUILD_AXIS(image_params, rag_xr, rag_yr);


  struct complexData sample, acc, arg;

#ifdef GANESH_STRENGTH_RED_OPT
  float imageParams_S4_1_float = image_params->S4-1;
  int   imageParams_S4_1_int   = image_params->S4-1;
  int   imageParams_P3_int     = image_params->P3;
#define IMAGE_PARAMS_S4_FLOAT imageParams_S4_1_float
#define IMAGE_PARAMS_S4_INT imageParams_S4_1_int
#define IMAGE_PARAMS_S4 imageParams_S4_1_int
#define IMAGE_PARAMS_P3 imageParams_P3_int
#else
#define IMAGE_PARAMS_S4_FLOAT (image_params->S4 - 1)
#define IMAGE_PARAMS_S4_INT (image_params->S4 - 1)
#define IMAGE_PARAMS_S4 (image_params->S4 - 1)
#define IMAGE_PARAMS_P3 image_params->P3
#endif

#ifdef RAG_PETER_DIST_AND_TRIG
  double const Pi = 3.1415926535897932384626433832795029l;
  double ku2   = 2.0*2.0*Pi*radar_params->fc/c_mks_mps;
  double ku2dr = ku2*image_params->dr;

  assert((m2-m1) == (n2-n1));
  int blk_size_whole = (n2-n1);
  int blk_size_half = blk_size_whole/2;
  ocrGuid_t A_m_dbg, Phi_m_dbg, image_ptr_dbg;
  float *A_m = spad_malloc(&A_m_dbg, blk_size_whole*sizeof(float));
  if(A_m == NULL){ocrPrintf("Error allocating memory for A_m\n");RAG_FLUSH;xe_exit(1);}
  struct complexData *Phi_m = spad_malloc(&Phi_m_dbg, blk_size_whole*sizeof(struct complexData));
  if(Phi_m == NULL){ocrPrintf("Error allocating memory for Phi_m\n");RAG_FLUSH;xe_exit(1);}
#ifdef RAG_SPAD
#ifdef TRACE_LVL_5
  ocrPrintf("////////// before spad setup in backproject_async\n");RAG_FLUSH;
#endif
  /* Row-pointer table ONLY: the rows point straight into this tile's own
     block.  The base program accumulated into a private staging buffer and
     copied the result out, because its destination was the shared whole-image
     block and it could not write there directly.  Here the destination IS
     private and freshly created, so the staging buffer and the copy-out are
     both dead weight -- one allocation and one full-tile memcpy per tile. */
  struct complexData **image_ptr;
  image_ptr = (struct complexData **)spad_malloc(&image_ptr_dbg,
                                       (m2-m1)*sizeof(struct complexData *));
  if(image_ptr == NULL) { ocrPrintf("Error allocating memory for image_ptr\n");RAG_FLUSH;xe_exit(1);}
  for(int m=0;m<(m2-m1);m++) {
    image_ptr[m] = &tile[(size_t)m*(size_t)(n2-n1)];
    /* The accumulation is +=, so the tile must start at zero. */
    spad_memset(image_ptr[m], 0, (n2-n1)*sizeof(struct complexData));
  }
#ifdef TRACE_LVL_5
  ocrPrintf("////////// after spad setup in backproject_async\n");RAG_FLUSH;
#endif
#endif
#ifdef RAG_HIST_BIN_DIFFS
  uint64_t hist[10] = {0,0,0,0,0,0,0,0,0,0,};
#endif
  for(int k=0; k<IMAGE_PARAMS_P3; k++) {
    int old_int_bin = -1;

#ifdef TRACE_LVL_5
  ocrPrintf("////////// compute distance to R_mid\n");RAG_FLUSH;
#endif
    double zr_mid  =                                   - platpos[k][2]; // Z
    double zr_mid2 = zr_mid * zr_mid;
    double yr_mid = image_params->yr[m1+blk_size_half] - platpos[k][1]; // Y
    double yr_mid2 = yr_mid * yr_mid;
    double sqrt_arg = zr_mid2 + yr_mid2;
    double xr_mid = image_params->xr[n1+blk_size_half] - platpos[k][0]; // X
    double xr_mid2 = xr_mid * xr_mid;
    double R_mid = sqrt (sqrt_arg + xr_mid2);
#ifdef TRACE_LVL_5
  ocrPrintf("////////// compute coefficients for computing bin\n");RAG_FLUSH;
#endif
    float ax  =  xr_mid/R_mid;
    float ay  =  yr_mid/R_mid;
    float bx  =  image_params->dr*(1-ax*ax)/2/R_mid;
    float by  =  image_params->dr*(1-ay*ay)/2/R_mid;
    float cxy = -image_params->dr*ax*ay/R_mid;
    float bin_mid;
    if(image_params->TF > 1) {
      bin_mid = (R_mid-radar_params->R0_prime)/image_params->dr;
    } else {
      bin_mid = (R_mid-radar_params->R0)/image_params->dr;
    }
    A_m[0] = -blk_size_half*(ax - blk_size_half*bx);
    for( int n=1;n<blk_size_whole;n++) {
      A_m[n] = A_m[n-1] + (ax+bx) + (2*(n-1)-blk_size_whole)*bx;
    }
#ifdef TRACE_LVL_5
  ocrPrintf("////////// compute cofficients for computing arg\n");RAG_FLUSH;
#endif
    float ux = ku2dr*ax;
    float uy = ku2dr*ay;
    float vx = ku2dr*bx;
    float vy = ku2dr*by;
    float wxy = ku2dr*cxy;
    struct complexData WXY;
#ifdef RAG_SINCOS
    sincosf(wxy, &WXY.imag, &WXY.real);
#else
    WXY.real = cosf(wxy);
    WXY.imag = sinf(wxy);
#endif
    struct complexData VX2;
#ifdef RAG_SINCOS
    sincosf(2*vx, &VX2.imag, &VX2.real);
#else
    VX2.real = cosf(2*vx);
    VX2.imag = sinf(2*vx);
#endif
    struct complexData VY2;
#ifdef RAG_SINCOS
    sincosf(2*vy, &VY2.imag, &VY2.real);
#else
    VY2.real = cosf(2*vy);
    VY2.imag = sinf(2*vy);
#endif
    struct complexData UX_VX;
#ifdef RAG_SINCOS
    sincosf((ux+(1-blk_size_whole)*vx), &UX_VX.imag, &UX_VX.real);
#else
    UX_VX.real = cosf(ux+(1-blk_size_whole)*vx);
    UX_VX.imag = sinf(ux+(1-blk_size_whole)*vx);
#endif
#ifdef TRACE_LVL_5
  ocrPrintf("////////// compute Phi_m\n");RAG_FLUSH;
#endif
#ifdef RAG_SINCOS
    sincosf((-blk_size_half*ux + blk_size_whole*blk_size_whole/4*vx), &(Phi_m[0].imag), &(Phi_m[0].real));
#else
    Phi_m[0].real = cosf(-blk_size_half*ux + blk_size_whole*blk_size_whole/4*vx);
    Phi_m[0].imag = sinf(-blk_size_half*ux + blk_size_whole*blk_size_whole/4*vx);
#endif
    for(int n=1;n<blk_size_whole;n++) {
      struct complexData tmp;
      Phi_m[n].real = Phi_m[n-1].real*UX_VX.real - Phi_m[n-1].imag*UX_VX.imag;
      Phi_m[n].imag = Phi_m[n-1].real*UX_VX.imag + Phi_m[n-1].imag*UX_VX.real;
      tmp.real = UX_VX.real*VX2.real - UX_VX.imag*VX2.imag;
      tmp.imag = UX_VX.real*VX2.imag + UX_VX.imag*VX2.real;
      UX_VX.real = tmp.real;
      UX_VX.imag = tmp.imag;
    }
#ifdef TRACE_LVL_5
  ocrPrintf("////////// compute Phi_n\n");RAG_FLUSH;
#endif
    double theta_mid = ku2*R_mid;
    double arg_mid = theta_mid - blk_size_half*uy + blk_size_whole*blk_size_whole/4*vy;
    struct complexData Psi_n;
#ifdef RAG_SINCOS
    double Psi_n_imag, Psi_n_real;
    sincos((arg_mid-2.0*Pi*round(arg_mid/2.0/Pi)), &Psi_n_imag, &Psi_n_real); // RAG -- Changed precision to match latest code from Dan Campbell
    Psi_n.imag = (float)Psi_n_imag;
    Psi_n.real = (float)Psi_n_real;
#else
    Psi_n.real = cos(arg_mid-2.0*Pi*round(arg_mid/2.0/Pi)); // RAG -- Changed precision to match latest code from Dan Campbell
    Psi_n.imag = sin(arg_mid-2.0*Pi*round(arg_mid/2.0/Pi)); // RAG -- Changed precision to match latest code from Dan Campbell
#endif
#ifdef TRACE_LVL_5
  ocrPrintf("////////// compute Gamma_m\n");RAG_FLUSH;
#endif
    struct complexData Gamma_m;
#ifdef RAG_SINCOS
    sincosf((-blk_size_half*wxy), &(Gamma_m.imag), &(Gamma_m.real));
#else
    Gamma_m.real = cosf(-blk_size_half*wxy);
    Gamma_m.imag = sinf(-blk_size_half*wxy);
#endif
    struct complexData Gamma_m_n_base_d;
#ifdef RAG_SINCOS
    sincosf((uy + (1-blk_size_whole)*vy - blk_size_half*wxy), &(Gamma_m_n_base_d.imag), &(Gamma_m_n_base_d.real));
#else
    Gamma_m_n_base_d.real = cosf(uy + (1-blk_size_whole)*vy - blk_size_half*wxy);
    Gamma_m_n_base_d.imag = sinf(uy + (1-blk_size_whole)*vy - blk_size_half*wxy);
#endif
    struct complexData Gamma_m_n_base;
#ifdef RAG_SINCOS
    sincosf((blk_size_whole*blk_size_whole/4*wxy), &(Gamma_m_n_base.imag), &(Gamma_m_n_base.real));
#else
    Gamma_m_n_base.real = cosf(blk_size_whole*blk_size_whole/4*wxy);
    Gamma_m_n_base.imag = sinf(blk_size_whole*blk_size_whole/4*wxy);
#endif
#ifdef RAG_SPAD
    for(int m=0; m<(m2-m1); m++) {
      float Bm = bin_mid + (m-blk_size_half)*(ay+(m-blk_size_half)*by);
      float Cm = (m-blk_size_half)*cxy;
#else
    for(int m=m1; m<m2; m++) {
      float Bm = bin_mid + ((m-m1)-blk_size_half)*(ay+((m-m1)-blk_size_half)*by);
      float Cm = ((m-m1)-blk_size_half)*cxy;
#endif
      struct complexData tmp;
      struct complexData Gamma_m_n;
      Gamma_m_n.real = Psi_n.real*Gamma_m_n_base.real - Psi_n.imag*Gamma_m_n_base.imag;
      Gamma_m_n.imag = Psi_n.real*Gamma_m_n_base.imag + Psi_n.imag*Gamma_m_n_base.real;
      tmp.real = Gamma_m_n_base.real*Gamma_m_n_base_d.real - Gamma_m_n_base.imag*Gamma_m_n_base_d.imag;
      tmp.imag = Gamma_m_n_base.real*Gamma_m_n_base_d.imag + Gamma_m_n_base.imag*Gamma_m_n_base_d.real;
      Gamma_m_n_base.real = tmp.real;
      Gamma_m_n_base.imag = tmp.imag;
      tmp.real = Gamma_m_n_base_d.real*VY2.real - Gamma_m_n_base_d.imag*VY2.imag;
      tmp.imag = Gamma_m_n_base_d.real*VY2.imag + Gamma_m_n_base_d.imag*VY2.real;
      Gamma_m_n_base_d.real = tmp.real;
      Gamma_m_n_base_d.imag = tmp.imag;
#ifdef RAG_SPAD
      for(int n=0; n<(n2-n1); n++) {
        float bin = A_m[n] + Bm + (n-blk_size_half)*Cm;
#else
      for(int n=n1; n<n2; n++) {
        float bin = A_m[(n-n1)] + Bm + ((n-n1)-blk_size_half)*Cm;
#endif
	struct complexData sample;
	if(bin >= 0.0f && bin < IMAGE_PARAMS_S4_FLOAT /*image_params->S4-1*/) {
	  struct complexData left,right;

	  int int_bin = (int)floorf(bin);

#ifdef RAG_HIST_BIN_DIFFS
          if(old_int_bin == -1) {
            old_int_bin = int_bin;
	  } else if(int_bin > old_int_bin) {
            if((int_bin-old_int_bin)<9) hist[(int_bin-old_int_bin)]++;
            else hist[9]++;
	  } else if(int_bin < old_int_bin) {
	    if((old_int_bin-int_bin)<9) hist[(old_int_bin-int_bin)]++;
	    else hist[9]++;
	  } else {
	    hist[0]++;
          }
#endif
          float w = bin - int_bin;
          left  = Xin[k][int_bin+0];
          right = Xin[k][int_bin+1];
          sample.real = (1-w)*left.real + w*right.real;
          sample.imag = (1-w)*left.imag + w*right.imag;
#if 0 // RAG to match change in latest code from Dan Campbell
        } else if (bin > IMAGE_PARAMS_S4_FLOAT /*image_params->S4-1*/) {
          sample = Xin[k][IMAGE_PARAMS_S4_INT];
#endif
        } else {
          sample.real = 0.0f;
          sample.imag = 0.0f;
        }
        struct complexData arg;
#ifdef RAG_SPAD
        arg.real = Phi_m[n].real*Gamma_m_n.real - Phi_m[n].imag*Gamma_m_n.imag;
        arg.imag = Phi_m[n].real*Gamma_m_n.imag + Phi_m[n].imag*Gamma_m_n.real;
#else
        arg.real = Phi_m[(n-n1)].real*Gamma_m_n.real - Phi_m[(n-n1)].imag*Gamma_m_n.imag;
        arg.imag = Phi_m[(n-n1)].real*Gamma_m_n.imag + Phi_m[(n-n1)].imag*Gamma_m_n.real;
#endif
        struct complexData tmp;
        tmp.real = Gamma_m_n.real*Gamma_m.real - Gamma_m_n.imag*Gamma_m.imag;
        tmp.imag = Gamma_m_n.real*Gamma_m.imag + Gamma_m_n.imag*Gamma_m.real;
        Gamma_m_n.real = tmp.real;
        Gamma_m_n.imag = tmp.imag;

        tmp.real = sample.real*arg.real - sample.imag*arg.imag;
        tmp.imag = sample.real*arg.imag + sample.imag*arg.real;
#ifdef RAG_SPAD
        image_ptr[m][n].real += tmp.real;
        image_ptr[m][n].imag += tmp.imag;
#else
        image[m][n].real += tmp.real;
        image[m][n].imag += tmp.imag;
#endif
      } // for n
      tmp.real = Gamma_m.real*WXY.real - Gamma_m.imag*WXY.imag;
      tmp.imag = Gamma_m.real*WXY.imag + Gamma_m.imag*WXY.real;
      Gamma_m.real = tmp.real;
      Gamma_m.imag = tmp.imag;
    } // for m
  } // for k
#ifdef RAG_HIST_BIN_DIFFS
  ocrPrintf("HIST = %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld\n",
	hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7], hist[8], hist[9]);
#endif
#ifdef RAG_SPAD
   /* No copy-out: image_ptr's rows ARE the tile. */
   spad_free(image_ptr, image_ptr_dbg);
#endif
   spad_free(Phi_m, Phi_m_dbg);
   spad_free(A_m, A_m_dbg);
#else // not RAG_PETER_DIST_OR_TRIG
#ifdef RAG_PURE_FLOAT
      const float ku = 2.0f*M_PI*radar_params->fc/c_mks_mps;
#else
      const float ku = 2.0*M_PI*radar_params->fc/c_mks_mps;
#endif

      for(int m=m1; m<m2; m++) {
#ifdef DEBUG_LVL_1
	ocrPrintf("backproject_async m(%d)\n",m);RAG_FLUSH;
#endif
	for(int n=n1; n<n2; n++) {
#ifdef DEBUG_LVL_2
	  ocrPrintf("backproject_async n(%d)\n",n);RAG_FLUSH;
#endif
	  acc.real = 0;
	  acc.imag = 0;
	  for(int k=0; k<IMAGE_PARAMS_P3; k++) {
#ifdef DEBUG_LVL_3
	    ocrPrintf("backproject_async k(%d)\n",k);RAG_FLUSH;
#endif
	    double x = (double)image_params->xr[m] - platpos[k][0];
	    double y = (double)image_params->yr[n] - platpos[k][1];
	    double z = (double)                      platpos[k][2];
	    double R = sqrt( x*x + y*y + z*z ); // RAG -- Changed precision to match latest code from Dan Campbell
#ifdef DEBUG_LVL_3
	    ocrPrintf("backproject_async                 R(%f)\n",R);RAG_FLUSH;
#endif
	    float bin;
	    if(image_params->TF > 1) {
	      bin = (R-radar_params->R0_prime)/image_params->dr;
	    }
	    else {
	      bin = (R-radar_params->R0)/image_params->dr;
	    }

	    if(bin >= 0.0f && bin < image_params->S4-1) {
	      struct complexData left,right;
	      int int_bin = (int)floorf(bin);
	      float w = bin - int_bin;
	      left  = Xin[k][int_bin+0];
	      right = Xin[k][int_bin+1];
	      sample.real = (1-w)*left.real + w*right.real;
	      sample.imag = (1-w)*left.imag + w*right.imag;
#if 0 // RAG to match change in latest code from Dan Campbell
	    } else if (bin > image_params->S4-1) {
	      sample = Xin[k][image_params->S4-1];
#endif
	    } else {
	      sample.real = 0.0f;
	      sample.imag = 0.0f;
	    }
#ifdef RAG_SINCOS
            double arg_imag, arg_real;
	    sincos(2.0*ku*R,&arg_imag,&arg_real); // RAG -- Changed precision to match latest code from Dan Campbell
            arg.imag = (float)arg_imag;
            arg.real = (float)arg_real;
#else
	    arg.real = cos(2.0*ku*R); // RAG -- Changed precision to match latest code from Dan Campbell
	    arg.imag = sin(2.0*ku*R); // RAG -- Changed precision to match latest code from Dan Campbell
#endif
	    acc.real += sample.real*arg.real - sample.imag*arg.imag;
	    acc.imag += sample.real*arg.imag + sample.imag*arg.real;
	  } // for k
#ifdef DEBUG_LVL_2
	  ocrPrintf("backproject_async                 update image[%d][%d]\n",m,n);RAG_FLUSH;
#endif
	  image[n][m] = acc;
#ifdef RAG_SPAD
	} // for n
#else
      } // for n
#endif
#ifdef RAG_SPAD
    } // for m
#else
  } // for m
#endif
#endif // RAG_PETER_DIST_AND_TRIG
#ifdef TRACE_LVL_4
  ocrPrintf("//////// leave backproject_async\n");RAG_FLUSH;
#endif
  /* Release before handing the block on: the gather reaches it through this
     task's output event, and a consumer cannot acquire a block its producer
     still holds. */
  ocrDbRelease(tile_dbg);
  return tile_dbg;
} // backproject_async


/* Tiles are a regular grid over [m1,m2) x [n1,n2), so a tile's rectangle is
 * recoverable from its slot: the dependences are added in creation order. */
typedef struct { u64 m1, m2, n1, n2, bm, bn; } bpGatherPRM_t;

/* Create one stripe's worth of tiles.  Runs on whatever rank the runtime put
   it on, so the tiles it creates are homed there and the creation traffic is
   spread instead of leaving one task to issue all of it. */
/* Assemble one row-band of tiles into a slab.  The image gather then depends
   on one slab per band instead of one block per tile. */
ocrGuid_t bp_row_gather_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv) {
  (void)paramc; (void)depc;
  bpRowPRM_t *p = (bpRowPRM_t *)paramv;
  RAG_REF_MACRO_SPAD(struct ImageParams,image_params,image_params_ptr,image_params_lcl,image_params_dbg,0);
  (void)image_params;
  const u64 rows = p->m2 - p->m1, cols = p->n2 - p->n1;
  ocrGuid_t slab_dbg; struct complexData *slab = NULL;
  int retval = ocrDbCreate(&slab_dbg, (void **)&slab,
                           (size_t)rows*(size_t)cols*sizeof(struct complexData),
                           0, NULL_HINT, NO_ALLOC);
  assert(retval==0);
  int t = 0;
  for(u64 n = p->n1; n < p->n2; n += p->bn, ++t) {
    const u64 nhi = (n + p->bn) < p->n2 ? (n + p->bn) : p->n2;
    const struct complexData *tile = (const struct complexData *)depv[1+t].ptr;
    if(tile == NULL) continue;
    for(u64 r = 0; r < rows; ++r)
      memcpy(&slab[r*cols + (n - p->n1)], &tile[r*(nhi-n)],
             (size_t)(nhi-n)*sizeof(struct complexData));
  }
  ocrDbRelease(slab_dbg);
  return slab_dbg;
}

ocrGuid_t bp_stripe_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv) {
  (void)depc; (void)depv;
  int retval;
  bpStripePRM_t *sp = (bpStripePRM_t *)paramv;
  assert(paramc == (sizeof(bpStripePRM_t)+sizeof(u64)-1)/sizeof(u64));
  ocrGuid_t _row_scg = NULL_GUID;
  bpRowPRM_t _rp;
  _rp.m1 = (u64)sp->m;
  _rp.m2 = (u64)((sp->m + sp->bm) < sp->m2 ? (sp->m + sp->bm) : sp->m2);
  _rp.n1 = (u64)sp->n1; _rp.n2 = (u64)sp->n2; _rp.bn = (u64)sp->bn;
  ocrGuid_t _row_evg;
  retval = ocrEdtCreate(&_row_scg, sp->row_clg, EDT_PARAM_DEF, (u64 *)&_rp,
                        EDT_PARAM_DEF, NULL, EDT_PROP_NONE, NULL_HINT, &_row_evg);
  assert(retval==0);
  retval = ocrAddDependence(_row_evg, sp->gather_scg, sp->slot, DB_MODE_RO);
  assert(retval==0);
  RAG_DEF_MACRO_PASS_RO(_row_scg,NULL,NULL,NULL,NULL,sp->image_params_dbg,0);
  int slot = 1;
  for(int n=sp->n1; n<sp->n2; n+=sp->bn) {
    struct corners_t async_corners;
    async_corners.m1 = sp->m;
    async_corners.m2 = (sp->m + sp->bm) < sp->m2 ? (sp->m + sp->bm) : sp->m2;
    async_corners.n1 = n;
    async_corners.n2 = (n + sp->bn) < sp->n2 ? (n + sp->bn) : sp->n2;
    ocrGuid_t tile_scg, tile_evg;
    backProjAsyncPRM_t pv;
    pv.corners = async_corners;
    retval = ocrEdtCreate(&tile_scg, sp->tile_clg, EDT_PARAM_DEF, (u64 *)&pv,
                          EDT_PARAM_DEF, NULL, EDT_PROP_NONE, NULL_HINT,
                          &tile_evg);
    assert(retval==0);
    retval = ocrAddDependence(tile_evg, _row_scg, slot++, DB_MODE_RO);
    assert(retval==0);
    RAG_DEF_MACRO_PASS_RO(tile_scg,NULL,NULL,NULL,NULL,sp->image_params_dbg,0);
    RAG_DEF_MACRO_PASS_RO(tile_scg,NULL,NULL,NULL,NULL,sp->radar_params_dbg,1);
    RAG_DEF_MACRO_PASS_RO(tile_scg,NULL,NULL,NULL,NULL,sp->Xin_dbg,2);
    RAG_DEF_MACRO_PASS_RO(tile_scg,NULL,NULL,NULL,NULL,sp->Pt_dbg,3);
    RAG_DEF_MACRO_PASS_RO(tile_scg,NULL,NULL,NULL,NULL,sp->Tp_dbg,4);
  }
  return NULL_GUID;
}

ocrGuid_t backproject_gather_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv) {
  bpGatherPRM_t *p = (bpGatherPRM_t *)paramv;
  RAG_REF_MACRO_SPAD(struct ImageParams,image_params,image_params_ptr,image_params_lcl,image_params_dbg,0);
  RAG_REF_MACRO_BSM( struct complexData **,image,NULL,NULL,image_dbg,1);
  RAG_REF_MACRO_BSM( struct img_stripes_s *,stripes,NULL,NULL,stripes_dbg,2);
  RAG_REMAP_2D(image, image_params->Iy, image_params->Ix, struct complexData);
  const u64 cols = p->n2 - p->n1;
  int s = 0;
  for(u64 m = p->m1; m < p->m2; m += p->bm, ++s) {
    const u64 mhi = (m + p->bm) < p->m2 ? (m + p->bm) : p->m2;
    const struct complexData *slab = (const struct complexData *)depv[3+s].ptr;
    if(slab == NULL) continue;
    for(u64 r = m; r < mhi; ++r)
      memcpy(&image[r][p->n1], &slab[(r-m)*cols], (size_t)cols*sizeof(struct complexData));
  }
  /* Cut the assembled image into its stripes, each homed on the rank whose
     tasks read it.  The consumers then depend on the stripe they cover
     instead of on the image, and the image itself never leaves this rank. */
  img_stripes_scatter(stripes, image);
  return NULL_GUID;
} // backproject_gather

ocrGuid_t BackProj_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv) {
  int retval;
#ifdef TRACE_LVL_3
  ocrPrintf("////// enter BackProj_edt\n");RAG_FLUSH;
#endif
  assert(paramc==(PRMNUM(backProj)));
  backProjPRM_t *_bpp = (backProjPRM_t *)paramv;
  struct corners_t *corners = &_bpp->corners;
  const ocrGuid_t _stripes_dbg = _bpp->stripes_dbg;
  int m1   = corners->m1;
  int m2   = corners->m2;
  int n1   = corners->n1;
  int n2   = corners->n2;
  assert(depc==7);
  RAG_REF_MACRO_SPAD(struct ImageParams,image_params,image_params_ptr,image_params_lcl,image_params_dbg,0);
  RAG_REF_MACRO_SPAD(struct RadarParams,radar_params,radar_params_ptr,radar_params_lcl,radar_params_dbg,1);
  RAG_REF_MACRO_BSM( struct complexData **,image,NULL,NULL,image_dbg,2);
  RAG_REF_MACRO_BSM( struct complexData **,refImage,NULL,NULL,refImage_dbg,3);
  RAG_REF_MACRO_BSM( struct complexData **,Xin,NULL,NULL,Xin_dbg,4);
  RAG_REF_MACRO_BSM( float **,Pt,NULL,NULL,Pt_dbg,5);
  RAG_REF_MACRO_BSM( float *,Tp,NULL,NULL,Tp_dbg,6);
#if 0
#warn RAG
#endif
  if(image_params->F > 1) {
    fftwf_complex *input, *fft_result, *ifft_result;
    ocrGuid_t input_dbg, fft_result_dbg, ifft_result_dbg;
    fftwf_plan plan_forward, plan_backward;	// FFTW plan variables

#ifdef TRACE_LVL_3
    ocrPrintf("////// BackProj FFTW initialization F = %d\n",image_params->F);RAG_FLUSH;
#endif
    input         = (fftwf_complex*)fftwf_malloc(&input_dbg, image_params->S3 * sizeof(fftwf_complex));
    fft_result    = (fftwf_complex*)fftwf_malloc(&fft_result_dbg, image_params->S4 * sizeof(fftwf_complex));
    ifft_result   = (fftwf_complex*)fftwf_malloc(&ifft_result_dbg, image_params->S4 * sizeof(fftwf_complex));
    plan_forward  = fftwf_plan_dft_1d(image_params->S3, input,      fft_result,  FFTW_FORWARD, FFTW_ESTIMATE);
    plan_backward = fftwf_plan_dft_1d(image_params->S4, fft_result, ifft_result, FFTW_BACKWARD, FFTW_ESTIMATE);

    float scale = 1/(float)image_params->S4;
    struct complexData **Xup;
    ocrGuid_t Xup_dbg;
#ifdef RAG_DRAM
    Xup = (struct complexData **)dram_malloc(&Xup_dbg,(image_params->P3)*sizeof(struct complexData *)
						     +(image_params->P3)*(image_params->S4)*sizeof(struct complexData));
#else
    Xup = (struct complexData **) bsm_malloc(&Xup_dbg,(image_params->P3)*sizeof(struct complexData *)
						     +(image_params->P3)*(image_params->S4)*sizeof(struct complexData));
#endif
    if(Xup == NULL) {
      ocrPrintf("Error allocating memory for Xup.\n");RAG_FLUSH;
      xe_exit(1);
    }
    struct complexData *Xup_data_ptr = (struct complexData *)&Xup[image_params->P3];
    if ( Xup_data_ptr == NULL) {
      ocrPrintf("Unable to allocate memory for Xup.\n");RAG_FLUSH;
      xe_exit(1);
    }
    for(int n=0; n<image_params->P3; n++) {
      Xup[n] =  Xup_data_ptr + n*image_params->S4;
    }

    for(int m=0; m<image_params->P3; m++) {
#ifdef RAG_DRAM
      DRAMtoSPAD(input, Xin[m], image_params->S3*sizeof(struct complexData));
#else
      BSMtoSPAD( input, Xin[m], image_params->S3*sizeof(struct complexData));
#endif

      fftwf_execute(plan_forward);

      spad_memset(&fft_result[image_params->S3][0], 0,
	     (image_params->S4-image_params->S3)*sizeof(fftwf_complex));

      fftwf_execute(plan_backward);

      for(int n=0; n<image_params->S4; n++) {
	ifft_result[n][0] *= scale;
	ifft_result[n][1] *= scale;
      }
#ifdef RAG_DRAM
      SPADtoDRAM(Xup[m],ifft_result, image_params->S3*sizeof(struct complexData));
#else
      SPADtoBSM( Xup[m],ifft_result, image_params->S3*sizeof(struct complexData));
#endif
    }
    // Free memory and deallocate plan
    fftwf_free(input, input_dbg);
    fftwf_free(fft_result, fft_result_dbg);
    fftwf_free(ifft_result, ifft_result_dbg);
    fftwf_destroy_plan(plan_forward);
    fftwf_destroy_plan(plan_backward);
#ifdef TRACE
    ocrPrintf("////// Performing backprojection over Ix[%d:%d] and Iy[%d:%d]\n",
	      m1, m2-1, n1, n2-1);RAG_FLUSH;
#endif
#if !defined(TG_ARCH)
    fprintf(stderr,"Performing backprojection over Ix[%d:%d] and Iy[%d:%d]\n",
	      m1, m2-1, n1, n2-1);fflush(stderr);
#endif

#ifdef RAG_NEW_BLK_SIZE
    int BACK_PROJ_ASYNC_BLOCK_SIZE_M = RAG_NEW_BLK_SIZE;
    int BACK_PROJ_ASYNC_BLOCK_SIZE_N = RAG_NEW_BLK_SIZE;
#else
    int BACK_PROJ_ASYNC_BLOCK_SIZE_M = blk_size(m2-m1,32);
    int BACK_PROJ_ASYNC_BLOCK_SIZE_N = blk_size(n2-n1,32);
    assert( ((m2-m1)%BACK_PROJ_ASYNC_BLOCK_SIZE_M) == 0);
    assert( ((n2-n1)%BACK_PROJ_ASYNC_BLOCK_SIZE_N) == 0);
#endif

    /* One block per tile, and a gather that assembles them into the image
     * once.  The tiles then never share a writable block, which is what the
     * base program's single whole-image RW dependence forces them to do. */
    int _ntm = 0, _ntn = 0;
    for(int m=m1; m<m2; m+=BACK_PROJ_ASYNC_BLOCK_SIZE_M) _ntm++;
    for(int n=n1; n<n2; n+=BACK_PROJ_ASYNC_BLOCK_SIZE_N) _ntn++;
    int _ntiles = _ntm * _ntn;
    /* This task does not touch the image any more -- the tiles write their own
     * blocks and the gather assembles them.  Release it BEFORE handing a write
     * dependence on it to the gather: holding a block while queuing a writer
     * for it puts that writer behind this task, and every reader that queues
     * afterwards behind the writer. */
    ocrDbRelease(image_dbg);
    ocrGuid_t _gather_clg, _gather_scg;
    bpGatherPRM_t _gp;
    _gp.m1 = m1; _gp.m2 = m2; _gp.n1 = n1; _gp.n2 = n2;
    _gp.bm = BACK_PROJ_ASYNC_BLOCK_SIZE_M; _gp.bn = BACK_PROJ_ASYNC_BLOCK_SIZE_N;
    retval = ocrEdtTemplateCreate(&_gather_clg, backproject_gather_edt,
                                  (sizeof(bpGatherPRM_t)+sizeof(u64)-1)/sizeof(u64),
                                  3 + _ntiles);
    assert(retval==0);
    RAG_TEMPLATE_REGISTER(_gather_clg);
    retval = ocrEdtCreate(&_gather_scg, _gather_clg, EDT_PARAM_DEF, (u64 *)&_gp,
                          EDT_PARAM_DEF, NULL, EDT_PROP_NONE, NULL_HINT, NULL);
    assert(retval==0);
    RAG_DEF_MACRO_PASS_RO(_gather_scg,NULL,NULL,NULL,NULL,image_params_dbg,0);
    RAG_DEF_MACRO_PASS(_gather_scg,NULL,NULL,NULL,NULL,image_dbg,1);
    /* The gather writes the stripe set: it is the one task that holds the
       whole image, so it is where the cut belongs. */
    RAG_DEF_MACRO_PASS(_gather_scg,NULL,NULL,NULL,NULL,_stripes_dbg,2);
    int _tidx = 0;

    // create a template for backproject_async function
    ocrGuid_t backproject_async_clg;
    retval = ocrEdtTemplateCreate(
	&backproject_async_clg, // ocrGuid_t *new_guid
	 backproject_async_edt,	// ocr_edt_ptr func_ptr
	PRMNUM(backProjAsync),
	5);			// depc
    assert(retval==0);
    RAG_TEMPLATE_REGISTER(backproject_async_clg);

#ifdef RAG_NEW_BLK_SIZE
    for(int m=m1; m<m2; m+=BACK_PROJ_ASYNC_BLOCK_SIZE_M) {
      for(int n=n1; n<n2; n+=BACK_PROJ_ASYNC_BLOCK_SIZE_N) {
	struct corners_t async_corners;
	async_corners.m1   = m;
	async_corners.m2   = (m+BACK_PROJ_ASYNC_BLOCK_SIZE_M)<m2?(m+BACK_PROJ_ASYNC_BLOCK_SIZE_M):m2;
	async_corners.n1   = n;
	async_corners.n2   = (n+BACK_PROJ_ASYNC_BLOCK_SIZE_N)<n2?(n+BACK_PROJ_ASYNC_BLOCK_SIZE_N):n2;
#else
    for(int m=m1; m<m2; m+=BACK_PROJ_ASYNC_BLOCK_SIZE_M) {
      for(int n=n1; n<n2; n+=BACK_PROJ_ASYNC_BLOCK_SIZE_N) {
	struct corners_t async_corners;
	async_corners.m1   = m;
	async_corners.m2   = m+BACK_PROJ_ASYNC_BLOCK_SIZE_M;
	async_corners.n1   = n;
	async_corners.n2   = n+BACK_PROJ_ASYNC_BLOCK_SIZE_N;
#endif
#ifdef TRACE_LVL_3
	ocrPrintf("////// create an edt for backproject_async\n");RAG_FLUSH;
#endif
	ocrGuid_t backproject_async_scg, _tile_evg;
    backProjAsyncPRM_t backProjAsyncParamv;
    backProjAsyncParamv.corners = async_corners;
	retval = ocrEdtCreate(
			&backproject_async_scg,	// *created_edt_guid
			 backproject_async_clg,	// edt_template_guid
			EDT_PARAM_DEF,		// paramc
			(u64 *)&backProjAsyncParamv, // *paramv
			EDT_PARAM_DEF,		// depc
			NULL,			// *depv
			EDT_PROP_NONE,		// properties
			NULL_HINT, // affinity
			&_tile_evg);		// *outputEvent
	assert(retval==0);
	retval = ocrAddDependence(_tile_evg, _gather_scg, 3 + _tidx, DB_MODE_RO);
	assert(retval==0);
	_tidx++;

	RAG_DEF_MACRO_PASS_RO(backproject_async_scg,NULL,NULL,NULL,NULL,image_params_dbg,0);
	RAG_DEF_MACRO_PASS_RO(backproject_async_scg,NULL,NULL,NULL,NULL,radar_params_dbg,1);
	RAG_DEF_MACRO_PASS_RO(backproject_async_scg,NULL,NULL,NULL,NULL,Xup_dbg,2);  // Xup
	RAG_DEF_MACRO_PASS_RO(backproject_async_scg,NULL,NULL,NULL,NULL,Pt_dbg,3);   // Platform Position
	RAG_DEF_MACRO_PASS_RO(backproject_async_scg,NULL,NULL,NULL,NULL,Tp_dbg,4);   // Pulse timestamps
      } // for n
    } // for m
#ifndef TG_ARCH
    dram_free(Xup,Xup_dbg); // Xup[]
#else
     bsm_free(Xup,Xup_dbg); // Xup[]
#endif
  } else { // if F
#ifdef TRACE
    ocrPrintf("////// Performing backprojection over Ix[%d:%d] and Iy[%d:%d]\n",
	      m1, m2-1, n1, n2-1);RAG_FLUSH;
#endif
#if !defined(TG_ARCH)
    fprintf(stderr,"Performing backprojection over Ix[%d:%d] and Iy[%d:%d]\n",
	      m1, m2-1, n1, n2-1);fflush(stderr);
#endif

#ifdef RAG_NEW_BLK_SIZE
    int BACK_PROJ_ASYNC_BLOCK_SIZE_M = RAG_NEW_BLK_SIZE;
    int BACK_PROJ_ASYNC_BLOCK_SIZE_N = RAG_NEW_BLK_SIZE;
#else
    int BACK_PROJ_ASYNC_BLOCK_SIZE_M = blk_size(m2-m1,32);
    int BACK_PROJ_ASYNC_BLOCK_SIZE_N = blk_size(n2-n1,32);
    assert( ((m2-m1)%BACK_PROJ_ASYNC_BLOCK_SIZE_M) == 0);
    assert( ((n2-n1)%BACK_PROJ_ASYNC_BLOCK_SIZE_N) == 0);
#endif

    /* One block per tile, and a gather that assembles them into the image
     * once.  The tiles then never share a writable block, which is what the
     * base program's single whole-image RW dependence forces them to do. */
    int _ntm = 0, _ntn = 0;
    for(int m=m1; m<m2; m+=BACK_PROJ_ASYNC_BLOCK_SIZE_M) _ntm++;
    for(int n=n1; n<n2; n+=BACK_PROJ_ASYNC_BLOCK_SIZE_N) _ntn++;
    int _ntiles = _ntm * _ntn;
    /* This task does not touch the image any more -- the tiles write their own
     * blocks and the gather assembles them.  Release it BEFORE handing a write
     * dependence on it to the gather: holding a block while queuing a writer
     * for it puts that writer behind this task, and every reader that queues
     * afterwards behind the writer. */
    ocrDbRelease(image_dbg);
    ocrGuid_t _gather_clg, _gather_scg;
    bpGatherPRM_t _gp;
    _gp.m1 = m1; _gp.m2 = m2; _gp.n1 = n1; _gp.n2 = n2;
    _gp.bm = BACK_PROJ_ASYNC_BLOCK_SIZE_M; _gp.bn = BACK_PROJ_ASYNC_BLOCK_SIZE_N;
    retval = ocrEdtTemplateCreate(&_gather_clg, backproject_gather_edt,
                                  (sizeof(bpGatherPRM_t)+sizeof(u64)-1)/sizeof(u64),
                                  3 + _ntm);
    assert(retval==0);
    RAG_TEMPLATE_REGISTER(_gather_clg);
    retval = ocrEdtCreate(&_gather_scg, _gather_clg, EDT_PARAM_DEF, (u64 *)&_gp,
                          EDT_PARAM_DEF, NULL, EDT_PROP_NONE, NULL_HINT, NULL);
    assert(retval==0);
    RAG_DEF_MACRO_PASS_RO(_gather_scg,NULL,NULL,NULL,NULL,image_params_dbg,0);
    RAG_DEF_MACRO_PASS(_gather_scg,NULL,NULL,NULL,NULL,image_dbg,1);
    /* The gather writes the stripe set: it is the one task that holds the
       whole image, so it is where the cut belongs. */
    RAG_DEF_MACRO_PASS(_gather_scg,NULL,NULL,NULL,NULL,_stripes_dbg,2);
    int _tidx = 0;

    // create a template for backproject_async function
    ocrGuid_t backproject_async_clg;
    retval = ocrEdtTemplateCreate(
	&backproject_async_clg, // ocrGuid_t *new_guid
	backproject_async_edt,	// ocr_edt_ptr func_ptr
	PRMNUM(backProjAsync), // paramc
	5);			// depc
    assert(retval==0);
    RAG_TEMPLATE_REGISTER(backproject_async_clg);

    /* One spawner per row-stripe: each creates its own stripe's tiles, so the
       creation traffic is spread over the machine instead of issuing from this
       one task, and each tile is created on the rank that will run it.  Slot
       ranges are handed out arithmetically, so the stripes never coordinate. */
    /* Each stripe assembles its own row band; the image gather then has one
       dependence per band.  Both fan-ins are the grid's side length, which is
       what keeps the dependence list inside the transport's message bound. */
    ocrGuid_t _row_clg;
    retval = ocrEdtTemplateCreate(&_row_clg, bp_row_gather_edt,
                                  (sizeof(bpRowPRM_t)+sizeof(u64)-1)/sizeof(u64),
                                  1 + _ntn);
    assert(retval==0);
    RAG_TEMPLATE_REGISTER(_row_clg);
    ocrGuid_t _stripe_clg;
    retval = ocrEdtTemplateCreate(&_stripe_clg, bp_stripe_edt,
                                  (sizeof(bpStripePRM_t)+sizeof(u64)-1)/sizeof(u64),
                                  0);
    assert(retval==0);
    RAG_TEMPLATE_REGISTER(_stripe_clg);
    for(int m=m1; m<m2; m+=BACK_PROJ_ASYNC_BLOCK_SIZE_M) {
      bpStripePRM_t _sp;
      _sp.gather_scg       = _gather_scg;
      _sp.image_params_dbg = image_params_dbg;
      _sp.radar_params_dbg = radar_params_dbg;
      _sp.Xin_dbg          = Xin_dbg;
      _sp.Pt_dbg           = Pt_dbg;
      _sp.Tp_dbg           = Tp_dbg;
      _sp.tile_clg         = backproject_async_clg;
      _sp.row_clg          = _row_clg;
      _sp.m  = m;   _sp.m2 = m2;
      _sp.n1 = n1;  _sp.n2 = n2;
      _sp.bm = BACK_PROJ_ASYNC_BLOCK_SIZE_M;
      _sp.bn = BACK_PROJ_ASYNC_BLOCK_SIZE_N;
      _sp.slot = 3 + _tidx;
      _tidx += 1;
      ocrGuid_t _stripe_scg;
      retval = ocrEdtCreate(&_stripe_scg, _stripe_clg, EDT_PARAM_DEF,
                            (u64 *)&_sp, EDT_PARAM_DEF, NULL, EDT_PROP_NONE,
                            NULL_HINT, NULL);
      assert(retval==0);
    } // for m
  } // if F

#ifdef TRACE_LVL_3
  ocrPrintf("////// leave BackProj_edt\n");RAG_FLUSH;
#endif
  return NULL_GUID;
}
