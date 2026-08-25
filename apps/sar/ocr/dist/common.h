#ifndef TG_ARCH
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include "macros.h"
#include <fftw3.h>
#ifndef TG_ARCH
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

#ifndef M_PI
#define M_PI		(3.1415926535897932384626433832795029)
#endif

#ifdef RAG_PURE_FLOAT
#define c_mks_mps (300000000.0f)
#else
#define c_mks_mps (300000000.0)
#endif

struct complexData {
	float real;
	float imag;
};

struct point {
	int x;		// x coordinate (pixel)
	int y;		// y coordinate (pixel)
	float p;	// correlation value
};

struct detects {
	float x;	// x coordinate (m)
	float y;	// y coordinate (m)
	float p;	// correlation value
};

struct Inputs {
	float *Tp;		// Timestamp of pulse transmissions
	float **Pt;		// Transmitter positions at each pulse
	struct complexData **X;	// Pulse compressed SAR data
	ocrGuid_t Tp_dbg;	// Timestamp of pulse transmissions
	ocrGuid_t Pt_dbg;	// Transmitter positions at each pulse
	ocrGuid_t X_dbg;	// Pulse compressed SAR data DB guid
};

struct RadarParams {
	float fc;		// Carrier frequency (Hz)
	float fs;		// Sampling frequency (Sa/s)
	float r0;		// Range from platform to scene center (m)
	float R0;		// Range of the zeroth range bin (m)
	float PRF;		// Pulse repetition frequency (Hz)
	float R0_prime;		// Range of the zeroth range bin if digital spotlighting is used (m)
};

struct AffineParams {
	int Nc;			// Number of affine registration control points
	int Sc;			// Affine registration neighborhood size
	int Rc;			// Range of affine registration
	float Tc;		// Affine registration correlation threshold
};

struct CfarParams {
	int Ncfar;		// Constant false alarm rate (CFAR) neighborhood size
	int Nguard;		// Number of guard cells for CFAR neighborhood
	float Tcfar;		// CFAR threshold
};

struct ImageParams {
	int F;			// Oversampling factor
	int TF;			// Tiling factor
	int Ix;			// Number of pixels in x direction
	int Iy;			// Number of pixels in y direction
	int Sx;			// Number of pixels in x direction of digital spotlighting subimage
	int Sy;			// Number of pixels in y direction of digital spotlighting subimage
	int P1;			// Number of pulses used to form images
	int S1;			// Number of complex return samples per pulse
	int Fbp;		// Image space oversampling during backprojection
	int P2;			// Number of reduced synthetic pulses after digital spotlighting
	int S2;			// Intermediate number of samples per simulated pulse during digital spotlighting
	int P3;			// Number of pulses after backprojection
	int S3;			// Number of samples per simulated pulse after digital spotlighting
	int S4;			// Length of oversampled IDFT
	int numImages;		// Number of images to process
	float dr;		// Pixel edge size (m)
	float *xr;		// x axis vector
	float *yr;		// y-axis vectors
	int   Ncor;             // CCD/CFAR correlation neighborhood size
	int imageNumber;	// Image number that is being processed RAG
};

struct DigSpotVars {
	float **Pt2;
	float *freqVec;
	struct complexData **X2;
	struct complexData **X3;
	struct complexData **X4;
	struct complexData *filtOut;
	struct complexData *tmpVector;
};

#ifndef TG_ARCH
#define MAX_Ncor (10)
#define MAX_Ncor_sqr (MAX_Ncor*MAX_Ncor)

/* Trigger slot the resample gather satisfies on the registered image's consumer. */
#define AFFINE_IMAGE_CONSUMER_SLOT 4

/* What one control-point task reports.  The base program had every task claim
 * an index in a shared counter and write into shared Fx/Fy/A arrays, which
 * makes those three blocks writable by thousands of tasks: write permission is
 * exclusive per policy domain, so the grant tours the domains and each task
 * holds all three for the whole of its correlation search.  The detections
 * were already changed from that shape to per-block results; this is the same
 * change one phase earlier.  The least-squares consumer sums A'A and A'F over
 * the retained set, and those sums do not care in what order they arrive. */
struct ctrlpt_res_s {
	int keep;      /* did this control point clear the correlation threshold */
	int fx, fy;    /* the displaced point */
	int a[6];      /* its row of the design matrix */
};

#ifdef ENABLE_EXTENSION_AFFINITY
#include <extensions/ocr-affinity.h>
#endif

/* The image as a set of row-stripes, one per rank.
 *
 * A phase whose tasks each read a small rectangle must not depend on the whole
 * image: under a retaining protocol that replicates it to every node the phase
 * runs on, and the replication is charged to whichever phase touches it first
 * -- so migrating one consumer only moves the bill.  Two things are needed
 * together: the object has to be divisible, and its consumers have to be
 * PLACED with the piece they read.  A decomposition whose consumers are
 * scattered still ends up sending everything everywhere, and worse, because a
 * task that reaches one row past its own piece pulls a whole neighbouring one.
 *
 * Stripes rather than a 2-D grid, and one per rank: every consumer's footprint
 * (the correlation's 47 rows, the detection's Ncfar+Ncor window, the
 * resample's block) is far shorter than a stripe, so a task needs its own
 * stripe and at most the next one -- which its neighbours on the same rank
 * need too, so the rank fetches it once.  Boundaries fall on multiples of the
 * projection's tile so that a tile never straddles two stripes. */
#define IMG_MAX_STRIPES 64
/* No stripe shorter than this: every consumer's footprint must fit inside two
   adjacent stripes, and the tallest is the detection window (Ncfar + Ncor). */
#define IMG_MIN_STRIPE 512

struct img_stripes_s {
	int ns;                       /* how many stripes */
	int nr;                       /* ranks they are spread over, contiguously:
	                                 stripe s lives on s*nr/ns, so the stripes a
	                                 rank owns are adjacent and the halo one of
	                                 its tasks reaches into is a piece its
	                                 neighbours on that rank need too */
	int iy, ix;                   /* the image's extent */
	int y0[IMG_MAX_STRIPES + 1];  /* stripe s covers rows [y0[s], y0[s+1]) */
	ocrGuid_t g[IMG_MAX_STRIPES]; /* the stripe blocks, once the producer fills them */
};

/* Lay out ns stripes over iy rows with every boundary a multiple of `align`. */
void img_stripes_layout(struct img_stripes_s *st, int ns, int nr, int iy, int ix, int align);
/* Which stripe holds row y. */
static inline int img_stripe_of(const struct img_stripes_s *st, int y) {
	int s = 0;
	while(s + 1 < st->ns && y >= st->y0[s+1]) s++;
	return s;
}
/* The rank a stripe lives on, and an EDT hint for it. */
ocrHint_t *img_stripe_hint(ocrHint_t *h, const struct img_stripes_s *st, int s);
/* Cut `image` into freshly created stripe blocks, each homed on its rank. */
void img_stripes_scatter(struct img_stripes_s *st, struct complexData **image);
/* Copy rows [y0,y1) x cols [x0,x1) out of the stripes wired at depv[slot0...]
 * -- the covering stripes in order -- into dst (row-major, stride x1-x0). */
void img_stripes_read(const struct img_stripes_s *st, ocrEdtDep_t *depv, int slot0,
                      int y0, int y1, int x0, int x1, struct complexData *dst);
/* A rigorous over-approximation of the quadratic warp's range over a block. */
void warp_bound(const float c[6], int n0, int n1, int m0, int m1, float *lo, float *hi);
/* The resample block's fixed slots, then two stripe slots. */
#define RESAMPLE_STRIPE_SLOT0 4
/* Bound on a resample source rectangle's side.  The warp is a registration
   correction, so the source is the block plus a few pixels; a fit that moved
   it further than this is not a resample any more, and the block's automatic
   storage is sized from this. */
#define RAG_AFFINE_MAX_SRC (RAG_AFFINE_BLK_SIZE + 64)

/* A control point's entire read footprint: an Sc square of the current image
 * and an (Sc + 2*Rc) square of the reference, both centred on the point.  The
 * point grid is inset by exactly the larger half-extent, so both windows are
 * whole.  The task fills this from the stripes it covers and the correlation
 * reads it, which is what keeps a task's dependence the size of its access. */
struct ctrlpt_win_s {
	int cur_n;
	int ref_n;
	int cur_y0, cur_x0;
	int ref_y0, ref_x0;
	const struct complexData *cur;  /* cur_n x cur_n, row-major */
	const struct complexData *ref;  /* ref_n x ref_n */
};

/* The control-point task's fixed slots, then two stripe slots per image. */
#define CTRLPT_STRIPE_SLOT0 5
/* Bound on a window side, for the task's automatic storage.  Checked against
   the parameters at run time -- a set that exceeded it would otherwise walk
   off the task's stack without a word. */
#define RAG_CTRLPT_MAX_WIN 128

/* A detection block's entire read footprint.  The block covers
 * [m1,m2) x [n1,n2) of the window grid; the correlation it computes spans that
 * plus the detection halo (Ncfar-1), and each correlation window is Ncor wide,
 * so the images are read over [m1, m1 + (m2-m1) + Ncfar + Ncor - 2).  Both
 * images are needed at the same rectangle, so one block carries both. */
/* Fixed slots of a detection block, then two stripe slots per image. */
#define CFAR_STRIPE_SLOT0 4
/* Bound on a detection window's side, for the task's automatic storage.
   Checked against the parameters at run time. */
#define RAG_CFAR_MAX_WIN (RAG_CFAR_BLK_SIZE + 64)

/* One row-stripe of the backprojection tile grid.  The tiles of a stripe are
 * created by their own EDT rather than all of them by one task: a single task
 * issuing seven runtime operations per tile is a serial ramp at the head of
 * every backprojection, and under the default placement most of those
 * operations are remote messages.  Spawning per stripe puts the work on every
 * node at once and creates each tile on the rank that will run it. */
typedef struct {
	ocrGuid_t gather_scg;
	ocrGuid_t image_params_dbg, radar_params_dbg;
	ocrGuid_t Xin_dbg, Pt_dbg, Tp_dbg;
	ocrGuid_t tile_clg;   /* template the stripe instantiates */
	ocrGuid_t row_clg;    /* template of this stripe's own gather */
	int m, m2, n1, n2;    /* this stripe's row band and column range */
	int bm, bn;           /* tile dimensions */
	int slot;             /* the slot this stripe's slab occupies on the image gather */
} bpStripePRM_t;

/* One row-band of tiles, assembled into a slab before the image gather sees
 * it.  A single gather with one dependence per tile does not survive a large
 * grid: the dependence list is part of the EDT's control message, and at the
 * shipped ladder's top rung it serialises past the transport's bound.  Two
 * levels keep both fan-ins at the grid's side length. */
typedef struct { u64 m1, m2, n1, n2, bn; } bpRowPRM_t;

/* Why two levels and not one dependence per tile: a dependence costs about
 * forty bytes in the EDT's control message and the transport bounds that
 * message, so a flat gather stops being expressible once the grid is large --
 * at the shipped ladder's top rung its list is 5 MB against a 2 MB bound.  It
 * is also slower well before that, because the image gather then pulls one
 * small block per tile instead of one slab per row band. */

/* First dependence slot of the per-task results on the reducing EDT. */
/* The reducer's fixed slots: ..7 as before, 8 the current image's stripe set
   (it wires the resample from their layout) and 9 the registered image's,
   which it hands to its gather to fill.  The per-point results follow. */
#define CTRLPT_ST_CUR_SLOT 8
#define CTRLPT_ST_REG_SLOT 9
#define CTRLPT_RES_SLOT0 10

/* Wire a datablock to an EDT that only needs its GUID -- to hand it on to the
 * tasks it creates -- and never dereferences it.  DB_MODE_NULL fills the
 * slot's guid and leaves ptr NULL, so the payload is not fetched at all; RO
 * would drag the whole block to the rank for nothing.  On a whole-image block
 * that is gigabytes per rewiring task.  Using this where the body DOES read is
 * an immediate NULL dereference, not a silent wrong answer. */
#define RAG_DEF_MACRO_GUID_ONLY(scg,dbg,slot) \
	retval = ocrAddDependence(dbg,scg,slot,DB_MODE_NULL); assert(retval==0);

#define RAG_PATH_MAX 1024

#endif

// FILE* handles are process-local: a task placed on another node cannot use a
// handle opened here.  Carry the file PATHS instead; every task that touches a
// file (re)opens it on the node it executes on (ReadData_edt seeks to its
// image's slice, post_CFAR_edt opens the detects output).  Requires the paths
// to be visible on every node (shared or replicated filesystem).
struct file_args_t {
#ifndef TG_ARCH
	char in_data[RAG_PATH_MAX];       // SAR pulse data
	char in_platpos[RAG_PATH_MAX];    // platform positions
	char in_pulsetime[RAG_PATH_MAX];  // pulse transmission timestamps
	char out_detects[RAG_PATH_MAX];   // detects output
#else
	void *pInFile, *pInFile2, *pInFile3, *pOutFile;
#endif
};

struct corners_t {
	int m1; int m2; int n1; int n2;
};

/* Paramv for main_body_edt */
typedef struct{
    ocrGuid_t post_main_scg;
} mainBodyPRM_t;

/* Paramv for ReadData_edt */
typedef struct{
    ocrGuid_t arg_scg;
} readDataPRM_t;

/* Paramv for FormImage_edt */
typedef struct{
    ocrGuid_t arg_scg;
    /* Which stripe set this image's projection fills.  A GUID travels in
       paramv freely; only a task that ACCESSES the block needs a dependence,
       which is added where the access happens. */
    ocrGuid_t stripes_dbg;
} formImagePRM_t;

/* Paramv for post_FormImage_edt */
typedef struct{
    ocrGuid_t arg_scg;
} postFormImagePRM_t;

/* Paramv for Affine_edt */
typedef struct{
    ocrGuid_t post_affine_async_scg;
    ocrGuid_t st_cur_dbg;       /* stripe sets the control points read */
    ocrGuid_t st_ref_dbg;
} affinePRM_t;

/* Paramv for affine_async_edt(s) */
typedef struct{
    struct corners_t corners;
    int sy0, sy1, sx0, sx1;     /* the source rectangle the resample reads */
}affineAsyncPRM_t;

/* Paramv for post_affine_async_edt(s) */
typedef struct{
    ocrGuid_t post_affine_async_scg;
    /* The task that consumes the registered image.  The resample gather
       satisfies its trigger slot directly: a consumer waits on the producer's
       own event, not on an enclosing scope. */
    ocrGuid_t image_consumer_scg;
} postAffineAsyncPRM_t;

/* Paramv for cfar_write_edt */
typedef struct{
    u64 offset;
    u64 nbytes;
    char out_detects[RAG_PATH_MAX];
} cfarWritePRM_t;

/* Paramv for post_CFAR_edt */
typedef struct{
#ifdef TG_ARCH
    void* pOutFile;
#else
    // Detects output path, by value: post_CFAR_edt opens it on whichever node
    // it executes on (a FILE* would be process-local).
    char out_detects[RAG_PATH_MAX];
#endif
} postCFARPRM_t;

/* Paramv for CFAR_edt: it creates post_CFAR_edt itself, so it carries that
   task's parameters through. */
typedef struct{
    postCFARPRM_t post;
} CFARPRM_t;

/* Paramv for cfar_async_edt */
typedef struct{
    struct corners_t corners;
} CFARAsyncPRM_t;

/* Paramv for BackProj_edt */
typedef struct{
    struct corners_t corners;
    ocrGuid_t stripes_dbg;      /* the stripe set its gather fills */
} backProjPRM_t;

/* Paramv for backproject_async_edt */
typedef struct{
    struct corners_t corners;
}backProjAsyncPRM_t;

int  ReadParams(struct RadarParams*,  struct ImageParams*, \
                struct AffineParams*, struct CfarParams*);

ocrGuid_t ReadData_edt (uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv);
ocrGuid_t FormImage_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv);
ocrGuid_t BackProj_edt (uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv);
ocrGuid_t Affine_edt   (uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv);
ocrGuid_t CCD_edt      (uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv);
ocrGuid_t CFAR_edt     (uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv);
ocrGuid_t post_CFAR_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv);

#if RAG_DIG_SPOT_ON
struct complexData** DigSpot(float, float, struct DigSpotVars*, struct ImageParams*, struct RadarParams*, struct Inputs*);
#endif

float sinc(float x);
void  sinc_interp(float *X, struct complexData *Y, struct complexData *YI, int Nz, float B, int M, int lenY);
