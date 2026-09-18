#ifndef HPGMG_H
#define HPGMG_H

#include <ocr.h>
#include <string.h>
#define WARMUP 0
#define TIMED 1

#define MG_MAXLEVELS 100
#define NUM_VECTORS 12   // Manu:: added temporary array
#define NUM_GHOSTS 1
#define BC_PERIODIC  0
#define BC_DIRICHLET 1

#ifndef BOX_SIMD_ALIGNMENT
#define BOX_SIMD_ALIGNMENT 1 // allignment requirement for j+/-1
#endif
#ifndef BOX_PLANE_PADDING
#define BOX_PLANE_PADDING  8 // scratch space to avoid unrolled loop cleanup
#endif

#define NUM_SMOOTHS      1
#define CHEBYSHEV_DEGREE 4
#define STENCIL_FUSE_BC

#define DEBUG 0

#if DEBUG
#define VERBOSEP(s) ocrPrintf(s)
#define VERBOSEPA(s,v) ocrPrintf(s,v)
#define VERBOSEPAA(s,v,w) ocrPrintf(s,v,w)
#else
#define VERBOSEP(s)
#define VERBOSEPA(s,v)
#define VERBOSEPAA(s,v,w)
#endif

#ifndef MG_AGGLOMERATION_START
#define MG_AGGLOMERATION_START  8 // i.e. start the distributed v-cycle when boxes are smaller than 8^3
#endif

#define FMG_INTERPOLATE 0
#define VC_INTERPOLATE 1

// the (a,b) pair of the elliptic operator the program is built for
#ifdef USE_HELMHOLTZ
#define HPGMG_A 2.0
#define HPGMG_B 1.0
#else
#define HPGMG_A 0.0
#define HPGMG_B 1.0
#endif

#define mu 0.00000001

#ifdef ENABLE_EXTENSION_AFFINITY
#include <extensions/ocr-affinity.h>
#endif

/* Pin a control-spine EDT to rank 0, where the level and mg datablocks are
 * homed: every spine phase acquires its level datablock RW for bookkeeping,
 * and an unpinned spine lands on a fresh rank each phase, dragging that
 * datablock's ownership around the cluster once per phase. */
static inline ocrHint_t * mgHomeEdtHint(ocrHint_t * h)
{
#if defined(OCR_APP_OPTIMIZED_PLACEMENT) && defined(ENABLE_EXTENSION_AFFINITY)
  ocrGuid_t aff = NULL_GUID;
  ocrAffinityGetAt(AFFINITY_PD, 0, &aff);
  ocrHintInit(h, OCR_HINT_EDT_T);
  ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
  return h;
#else
  (void)h;
  return NULL_HINT;
#endif
}

/* Home a per-box EDT with the box datablock it will acquire: the query
 * resolves the box's placement so the EDT lands where its dependency already
 * lives, turning the acquire local. */
static inline ocrHint_t * mgBoxEdtHint(ocrHint_t * h, ocrGuid_t box)
{
#if defined(OCR_APP_OPTIMIZED_PLACEMENT) && defined(ENABLE_EXTENSION_AFFINITY)
  u64 count = 1;
  ocrGuid_t aff = NULL_GUID;
  ocrAffinityQuery(box, &count, &aff);
  ocrHintInit(h, OCR_HINT_EDT_T);
  ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
  return h;
#else
  (void)h; (void)box;
  return NULL_HINT;
#endif
}

// A guid rides paramv as its 64-bit representation.
static inline u64 hpgmg_guid_bits(ocrGuid_t g) {
  u64 v; memcpy(&v, &g, sizeof(v)); return v;
}
static inline ocrGuid_t hpgmg_guid_from(u64 v) {
  ocrGuid_t g; memcpy(&g, &v, sizeof(g)); return g;
}

typedef struct {
  struct {int i, j, k;} low;
  int global_box_id;
} box_type;

typedef struct {
  double h;
  int num_boxes;
  int box_dim;                          // dimension of each cubical box (not counting ghost zones)
  struct {int i, j, k;} boxes_in;       // total number of boxes in i,j,k across this level
  struct {int i, j, k;} dim;            // global dimensions at this level (NOTE: dim.i == boxes_in.i * box_dim)
  int boundary_condition;
  int alpha_is_zero;
  double dominant_eigenvalue_of_DinvA;
  int jStride, kStride, volume;
  int level;

  // offset for box related vectors
  u32 f;
  u32 f_Av;
  u32 u_true;
  u32 u;
  u32 alpha;
  u32 beta_i;
  u32 beta_j;
  u32 beta_k;
  u32 Dinv;
  u32 L1inv;
  u32 valid;
  u32 vec_temp;

  // constant box per level
  ocrGuid_t constant_box_guid;

  // offset for list of box guids at even level
  u32 boxes;

  // offset for box norms: size == num_boxes * sizeof(double)
  u32 b_norms;

  // timing related variables
  double time_operators[5]; //smooth (0), residual (1), restriction(2), interpolation (3), all (4)
  double time_temp[5];

} level_type;

typedef struct {
  int max_levels;                  // depth of the largest v-cycle
  int current_level;               // current level
  int vcycle_level;                // depth of the current v-cycle
  int num_levels;
  ocrGuid_t levels[MG_MAXLEVELS];  // array of pointers to levels
} mg_type;

typedef struct
{
    ocrGuid_t box;
    s64 flag;
} PRM_restrict_level_edt_t;

/*
 mg_type: levels[]--> level_type
 level_type: ((char*)&level_type)+level_type.boxes[]-->box_type
 box_type: ((char*)&box_type)+level_type._var_[]-->_var_

 examples:

 ocrGud_t level0 = mg.levels[0];

 ocrGuid_t box0 = ((ocrGuid_t*)(((char*)depv[0].ptr)+((level_type*)depv[0].ptr)->boxes))[0];

 double*alpha = (double*)(((char*)depv[0].ptr)+((level_type*)depv[1].ptr)->alpha);
*/

// Initialization.  init_all lays the hierarchy out and returns the event the
// solve hangs on; the data itself is produced by per-box tasks that run on
// the rank owning the box, so the caller must not read a box afterwards.
ocrGuid_t init_all(mg_type* mg, int box_dim, int boxes_in_i, int bc, ocrGuid_t *startEvt);

// The level table: box dimension and box-grid edge per level, returning the
// level count.  A geometry whose table cannot be built is rejected loudly.
int hpgmg_level_table(int box_dim0, int boxes_in_i0, int *box_dim, int *boxes_in_i);
void hpgmg_check_geometry(int box_dim, int boxes_in_i);
u32 hpgmg_box_geometry(int box_dim, int *jStrideOut, int *kStrideOut);

// per-box initialization bodies, shared by every decomposition
void hpgmg_valid_box(level_type *level, box_type *bx);
void hpgmg_fill_box(level_type *level, box_type *bx, double a, double b);
double hpgmg_gershgorin_box(level_type *level, box_type *bx, double a, double b);
double hpgmg_alpha_dot_box(level_type *level, box_type *bx);
void hpgmg_coeff_restrict_box(level_type *level, box_type *cb,
                              level_type *fromLevel, box_type *fine_bx,
                              int fb, int count);

// per-box initialization tasks and the two-value (eigenvalue, alpha dot)
// max-reduction they feed
ocrGuid_t hpgmg_prep_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t hpgmg_coeff_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t hpgmg_gersh_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t hpgmg_diag_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t hpgmg_max_merge_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t hpgmg_eigen_final_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

// the verification reduction, shared the same way
ocrGuid_t hpgmg_error_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t hpgmg_error_level_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t hpgmg_error_final_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

void residual_coarse(level_type * level, box_type *box, double *res_id, double *x_id, double *rhs_id, double a, double b);

// MG functions and edts
ocrGuid_t do_solves(ocrGuid_t start, mg_type* mg_ptr, int num, int warmup);

ocrGuid_t init_ur(mg_type* mg_ptr, ocrGuid_t start, int flag);
ocrGuid_t time_all(mg_type* mg_ptr, ocrGuid_t start);
ocrGuid_t restrict_all(mg_type* mg_ptr, ocrGuid_t start);
ocrGuid_t solve(mg_type* mg_ptr, ocrGuid_t start1, ocrGuid_t start2);
ocrGuid_t interpolate(int ln, mg_type* mg_ptr, ocrGuid_t start, u64 type);
ocrGuid_t smooth(int ln, mg_type* mg_ptr, ocrGuid_t start);
ocrGuid_t residual(int ln, mg_type* mg_ptr, ocrGuid_t start);
ocrGuid_t vcycle(int ln, mg_type* mg_ptr, ocrGuid_t start);
ocrGuid_t scaled_residual_norm(mg_type* mg_ptr, ocrGuid_t start);
ocrGuid_t norm(mg_type* mg_ptr, ocrGuid_t start);

ocrGuid_t init_ur_level_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t init_ur_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t restrict_level_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t restrict_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
void restriction_f(level_type* l, box_type* lb, level_type* f, box_type* fb, int octant, int dim, int flag);
ocrGuid_t interpolate_level_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t interpolate_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
void interpolation_pc(level_type* l, box_type* lb, level_type* f, box_type* fb, int octant);
void interpolation_pl(level_type* l, box_type* lb, level_type* f, box_type* fb, int octant);
ocrGuid_t smooth_level_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t smooth_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t residual_level_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t residual_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t mulv_level_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t mulv_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t norm_level_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t norm_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t norm_final_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t exchange_level_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t exchange_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t solve_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t zero_vector_level_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t zero_vector_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t time_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t print_timing_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

ocrGuid_t zero_vector(int l, mg_type* mg_ptr, u64 type, ocrGuid_t start);
void restriction_f_all(level_type* l, box_type* lb, level_type* f, box_type* fb, int octant, int dim, int flag);
void restrict_generic_all(level_type* l, box_type* lb, int l_off, level_type* f, box_type* fb, int f_off, int side);
void restrict_generic(level_type* l, box_type* lb, int l_off, level_type* f, box_type* fb, int f_off, int octant, int dim, int side);
void interpolation_pc_all(level_type* l, box_type* lb, level_type* f, box_type* fb, int octant);
void interpolation_pl_all(level_type* l, box_type* lb, level_type* f, box_type* fb, int octant);


// Operator functions

#endif
