#ifndef AMR_H
#define AMR_H

#include <string.h>
#include <ocr.h>
#include <extensions/ocr-affinity.h>

#include "ocrAppUtils.h"

/// Children of an interior spawn-tree node.
#define FAN_BRANCH 8
/// Refinement levels the neighbour-level byte can name.
#define AMR_MAX_LEVELS 15
/// A face has no block on the far side of it.
#define NBR_NONE 0xFF

#define GUID_WORDS ((sizeof(ocrGuid_t)+sizeof(u64)-1)/sizeof(u64))

static inline void guid_store(u64* w, ocrGuid_t g)
{
  memcpy(w, &g, sizeof(g));
}

static inline ocrGuid_t guid_load(const u64* w)
{
  ocrGuid_t g;
  memcpy(&g, w, sizeof(g));
  return g;
}

/// A block's disposition in a refinement round.  The three values are ordered
/// so that a round can only ever raise a block's demand on the mesh.
typedef enum { UNREFINE_BLK = -1, LEAVE_BLK_AT_LVL = 0, REFINE_BLK = 1 } decision;

typedef struct
{
  s32 type;
  s32 bounce;
  double cen[3];
  double move[3];
  double size[3];
  double inc[3];
} object;

/// Everything the command line fixes, plus what follows from it in closed
/// form.  Written once and read by every task afterwards.
typedef struct
{
  double tol;
  s32 nx, ny, nz;              //!< cells per block per axis
  s32 npx, npy, npz;           //!< unrefined mesh in blocks
  s32 num_refine;
  s32 block_change;
  s32 uniform_refine;
  s32 refine_freq;
  s32 num_vars;
  s32 comm_vars;
  s32 num_tsteps;
  s32 stages_per_ts;
  s32 checksum_freq;
  s32 stencil;
  s32 error_tol;
  s32 report_diffusion;
  s32 report_perf;
  s32 plot_freq;
  s32 code;
  s32 permute;
  s32 refine_ghost;
  s32 num_objects;
  s32 p2[AMR_MAX_LEVELS+3];    //!< powers of two, indexed by level difference
  s32 mesh_size[3];            //!< the mesh in units of the finest half-block
  u32 nranks;
  u32 pd[3];                   //!< rank grid the blocks are mapped onto
  u32 width;                   //!< blocks emitted by one spawn-tree leaf
} control;

/// One live block.  `nbr` holds the refinement level of the block on the far
/// side of each face, or NBR_NONE where the face is the edge of the mesh;
/// `in` names, per face and per incoming strip, the live block that writes
/// it, so a stage task resolves its halo without searching the directory.
typedef struct
{
  u32 key;                     //!< level-tagged linear position, unique over the forest
  u32 pos[3];
  u32 owner;
  u8  level;
  u8  nbr[6];                  //!< face order (axis*2 + pole)
  s8  disp;                    //!< what the block asked of the round that made it
  s32 in[6][4];
} blk;

/// An interior node of the block forest.  A child is a live block by its
/// directory index, or another interior node encoded as -(index)-1.
typedef struct
{
  s32 child[8];
} fnode;

/// How a block of the new mesh gets its payload.
enum { BK_KEEP, BK_CHILD, BK_MERGE };

/// Which phase of a refinement round a directory is computed for.
enum { MP_INIT, MP_REFINE, MP_UNREFINE };

/// The block directory of one mesh epoch: the topology, the forest the
/// checksum reduces over, where each rank's blocks begin, and how each block
/// is filled when the epoch opens.  Everything in it is a pure function of
/// the command line, the object trajectory and the previous epoch, so one
/// task computes it and every other task reads it.
typedef struct
{
  control c;
  u32 nblk;
  u32 nbase;
  u32 nint;                    //!< interior forest nodes
  u32 nold;                    //!< blocks of the epoch this one replaces
  u32 epoch;
  u32 gen;                     //!< strip generation the epoch's first stage reads
  s32 unref_level;             //!< level the next coarsening pass collapses, -1 when done
  u8  changed;                 //!< the block set differs from the epoch this replaces
} mesh;

static inline blk* mesh_brec(const mesh* m)
{
  return (blk*)(void*)((char*)m + sizeof(mesh));
}

static inline s32* mesh_root(const mesh* m)
{
  return (s32*)(void*)(mesh_brec(m) + m->nblk);
}

static inline fnode* mesh_fnode(const mesh* m)
{
  return (fnode*)(void*)(mesh_root(m) + m->nbase);
}

static inline u32* mesh_rstart(const mesh* m)
{
  return (u32*)(void*)(mesh_fnode(m) + m->nint);
}

static inline s32* mesh_src(const mesh* m)
{
  return (s32*)(void*)(mesh_rstart(m) + m->c.nranks + 1);
}

static inline u8* mesh_kind(const mesh* m)
{
  return (u8*)(void*)(mesh_src(m) + 8*(u64)m->nblk);
}

static inline u8* mesh_odead(const mesh* m)
{
  return mesh_kind(m) + m->nblk;
}

static inline u64 mesh_bytes(u32 nblk, u32 nbase, u32 nint, u32 nold, u32 nranks)
{
  return sizeof(mesh) + (u64)nblk*sizeof(blk) + (u64)nbase*sizeof(s32)
       + (u64)nint*sizeof(fnode) + (u64)(nranks+1)*sizeof(u32)
       + 8*(u64)nblk*sizeof(s32) + (u64)nblk + (u64)nold;
}

/// The datablocks a live block owns: its payload, its checksum contribution,
/// and one outgoing strip per face per generation.  A strip's size depends
/// only on the block dimensions, so these outlive every refinement the block
/// itself survives.
typedef struct
{
  ocrGuid_t payload;
  ocrGuid_t chk;
  ocrGuid_t face[6][2];
} bguid;

/// A contiguous run of the directory's handles, as the build tree folds it up.
typedef struct
{
  u64 n;
  bguid g[];
} gslice;

/// A block's contribution to a checksum round: the weighted reduction that
/// sees the mesh, then one plain sum per variable.
typedef struct
{
  double weighted;
  double sum[];
} chksum;

static inline u64 chksum_bytes(const control* c)
{
  return sizeof(chksum) + (u64)c->num_vars*sizeof(double);
}

/* ---- geometry -------------------------------------------------------- */

/// Blocks at level L, per axis.
static inline u32 level_dim(const control* c, u32 axis, u32 level)
{
  s32 n = axis == 0 ? c->npx : (axis == 1 ? c->npy : c->npz);
  return (u32)n << level;
}

/// The first key of a level: the forest holds `nbase * 8^L` positions at
/// level L, and the levels are laid out one after another, so a key names a
/// (level, position) pair uniquely.
static inline u32 level_base(const control* c, u32 level)
{
  u32 b = 0, w = (u32)(c->npx*c->npy*c->npz);
  for(u32 l = 0; l < level; ++l) { b += w; w <<= 3; }
  return b;
}

static inline u32 blk_key(const control* c, u32 level, const u32 p[3])
{
  return level_base(c, level)
       + ((p[0]*level_dim(c, 1, level) + p[1])*level_dim(c, 2, level) + p[2]);
}

/// Half the extent of a block at `level`, in the units `mesh_size` is in.
static inline s32 blk_half(const control* c, u32 level)
{
  return c->p2[c->num_refine - (s32)level];
}

/// The centre of a block, and the only geometry the refinement test reads.
/// A block at level 0 is centred in its own extent; every split then shifts a
/// child by the published offset, which is HALF the child's half-extent and,
/// at the deepest level, the word that precedes the table of powers in the
/// control record — the object count.  Both are reproduced verbatim: the
/// centre decides the mesh, so an arithmetically tidier offset would put this
/// program on a different mesh than the one it re-implements.  Since the
/// offsets are a function of the position bits, the walk is still closed form.
static inline void blk_centre(const control* c, u32 level, const u32 p[3], s32 cen[3])
{
  s32 size = c->p2[c->num_refine + 1];
  for(u32 d = 0; d < 3; ++d) {
    s32 v = (s32)(p[d] >> level)*size + (size >> 1);
    for(u32 j = 1; j <= level; ++j) {
      s32 e = (s32)((p[d] >> (level - j)) & 1);
      s32 off = ((s32)j == c->num_refine) ? c->num_objects
                                          : c->p2[c->num_refine - (s32)j - 1];
      v += (2*e - 1)*off;
    }
    cen[d] = v;
  }
}

/// The rank a block lives on.  The map is level-consistent — doubling the
/// position and the level's extent together leaves the quotient alone — so a
/// block, its parent and its children share a rank wherever the rank grid
/// divides the mesh, and a refinement never moves a payload.
static inline u32 blk_owner(const control* c, u32 level, const u32 p[3])
{
  if(c->nranks <= 1) return 0;
  u32 r[3];
  for(u32 d = 0; d < 3; ++d)
    r[d] = (u32)(((u64)p[d]*c->pd[d]) / level_dim(c, d, level));
  return (r[2]*c->pd[1] + r[1])*c->pd[0] + r[0];
}

/// Strips a face carries: one for a neighbour at the same level or coarser,
/// four quarter strips where the far side is finer.
static inline u32 face_slots(const blk* b, u32 f)
{
  if(b->nbr[f] == NBR_NONE) return 0;
  return b->nbr[f] == b->level + 1 ? 4 : 1;
}

/// The two block dimensions a face of `axis` spans, rows first.
static inline void face_dims(const control* c, u32 axis, u32 d[2])
{
  d[0] = axis == 0 ? (u32)c->ny : (u32)c->nx;
  d[1] = axis == 2 ? (u32)c->ny : (u32)c->nz;
}

/// A strip datablock holds one whole face, which is also exactly four
/// quarter faces, so its size never changes with the mesh around it.
static inline u64 face_bytes(const control* c, u32 axis)
{
  u32 d[2]; face_dims(c, axis, d);
  return (u64)c->num_vars*d[0]*d[1]*sizeof(double);
}

static inline u64 payload_bytes(const control* c)
{
  return (u64)(c->nx+2)*(c->ny+2)*(c->nz+2)*c->num_vars*sizeof(double);
}

/* ---- placement hints ------------------------------------------------- */

static inline ocrHint_t* edt_at(ocrHint_t* h, const control* c, u32 rank)
{
  if(c->nranks <= 1) return NULL_HINT;
  ocrGuid_t aff;
  ocrAffinityGetAt(AFFINITY_PD, rank, &aff);
  ocrHintInit(h, OCR_HINT_EDT_T);
  ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
  return h;
}

/* ---- phases ---------------------------------------------------------- */

enum phase { PH_BUILD, PH_STAGE, PH_RELEASE };

/// Argument words of a phase: the stage's strip generation, the timestep, and
/// whether the stage also takes its checksum point.
enum { PA_GEN, PA_TS, PA_CHK, PA_NARG };

/// What a spawn-tree leaf needs to emit one phase's tasks for a contiguous
/// range of one rank's blocks.  A leaf never crosses a rank, so everything it
/// names is owned by `rank` and every task it creates is placed there.
typedef struct
{
  ocrGuid_t mesh;
  const mesh* m;
  ocrGuid_t guids;
  const bguid* g;   //!< NULL while the epoch's handles are still being made
  ocrGuid_t omesh;             //!< the previous epoch, during PH_BUILD
  const mesh* om;
  ocrGuid_t oguids;
  const bguid* og;
  ocrGuid_t join;
  const u64* arg;
  u32 rank;
} spawn_ctx;

void spawn_build(const spawn_ctx* x, u32 lo, u32 hi);
void spawn_stage(const spawn_ctx* x, u32 lo, u32 hi);
void spawn_release(const spawn_ctx* x, u32 lo, u32 hi);

void fan_launch(u32 phase, const control* c, const ocrGuid_t* dep, ocrGuid_t parent,
                u32 slot, const u64* arg);

/* ---- the mesh -------------------------------------------------------- */

u8 parse_command(u32 argc, void* argv, control* c, object** obj, ocrGuid_t* obj_db);
void print_help_message(void);
s32 check_input(control* c);

void object_move(const control* c, object* obj);
decision check_objects(const control* c, const object* obj, u32 level, const u32 pos[3]);

void* mesh_compute(const control* c, const mesh* old, const object* obj, s32 ts, u32 mode);
u64 mesh_size_bytes(const void* handle);
u8 mesh_changed(const void* handle);
u8 mesh_emit(void* handle, mesh* m);
void mesh_free(void* handle);
u32 face_quarter(u32 f, const u32 p[3]);

/* ---- physics --------------------------------------------------------- */

void cells_init(const control* c, const u32 pos[3], double* cells);
void cells_blank(const control* c, double* cells);
void cells_upsample(const control* c, u32 prong, const double* parent, double* cells);
void cells_merge(const control* c, u32 prong, const double* child, double* cells);
void stencil_calc(const control* c, double* cells, s32 var);
void checksum_block(const control* c, const double* cells, chksum* out);
void face_pack(const control* c, const blk* b, u32 f, const double* cells, double* strip);
void face_unpack(const control* c, const blk* b, u32 f, u32 slot, const double* strip,
                 double* cells);
void face_edge(const control* c, u32 f, double* cells);

#endif
