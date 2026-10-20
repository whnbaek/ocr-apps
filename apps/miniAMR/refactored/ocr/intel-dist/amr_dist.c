#include <stdlib.h>
#include <math.h>
#include <ocr.h>

#include "amr.h"

/* Where the run stands.  One task at a time holds it for writing, and it
 * carries the handles of the epoch in force, so a control task can wire the
 * next phase without being told what the previous one produced. */
typedef struct
{
  ocrGuid_t cfg;
  ocrGuid_t obj;
  ocrGuid_t mesh;
  ocrGuid_t guids;
  ocrGuid_t omesh;
  ocrGuid_t oguids;
  u32 step;
  u32 pre;                     //!< pre-loop checksum rounds taken
  s32 ts;
  s32 stage;
  u32 gen;                     //!< strip generation the next stage reads
  u32 rounds;
  s32 last_ts;
  double last_total;
  double last_weighted;
  double golden[];             //!< the reference round: weighted, then one per variable
} runstate;

enum { ST_BOOT, ST_PRE, ST_REFINE0, ST_LOOP, ST_REFINET };

/* Control slots: the run state, the parsed command line, the objects, the
 * directory in force, and where present the phase that must finish first. */
enum { SL_ST, SL_CFG, SL_OBJ, SL_MESH, SL_X, SL_DEPC };

enum { BD_MESH, BD_OGUIDS, BD_SRC };
enum { BP_IDX, BP_GEN, BP_INIT, BP_SLOT, BP_PARENT, BP_NPARAM = BP_PARENT + GUID_WORDS };

enum { SD_MESH, SD_PAY, SD_CHK, SD_STRIP };
enum { SP_IDX, SP_NPARAM };

enum { RP_IDX, RP_DEAD, RP_NPARAM };

/* Reduction slots and words: an interior node of the block forest, a leaf
 * over a run of base blocks, and an interior node of the run that
 * concatenates them. */
enum { KD_MESH, KD_GUIDS, KD_DEPC };
enum { KP_LO, KP_HI, KP_SLOT, KP_PARENT, KP_NPARAM = KP_PARENT + GUID_WORDS };
enum { NP_NODE, NP_SLOT, NP_PARENT, NP_NPARAM = NP_PARENT + GUID_WORDS };
enum { AP_LO, AP_SLOT, AP_PARENT, AP_NPARAM = AP_PARENT + GUID_WORDS };

/// A run of block contributions, in the order the reduction must add them.
typedef struct
{
  u64 n;
  double v[];
} chkarr;

static ocrGuid_t plan_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
static ocrGuid_t built_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
static ocrGuid_t released_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
static ocrGuid_t staged_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
static ocrGuid_t chkfinal_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
static ocrGuid_t chkfan_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

/* ---- geometry helpers used by more than one task --------------------- */

static u32 out_faces(const blk* b)
{
  u32 n = 0;
  for(u32 f = 0; f < 6; ++f) if(b->nbr[f] != NBR_NONE) ++n;
  return n;
}

static u32 in_slots(const blk* b)
{
  u32 n = 0;
  for(u32 f = 0; f < 6; ++f) n += face_slots(b, f);
  return n;
}

/* ---- opening an epoch ------------------------------------------------- */

/* Make the datablocks a block owns.  A strip carries a whole face however the
 * mesh around it is refined, so the set is fixed for the block's lifetime and
 * this runs once per block rather than once per epoch. */
static void block_create(const control* c, bguid* g, double** cells, u32 gen,
                         double* strip[6], chksum** ck)
{
  void* p;
  ocrDbCreate(&g->payload, (void**)cells, payload_bytes(c), 0, NULL_HINT, NO_ALLOC);
  ocrDbCreate(&g->chk, (void**)ck, chksum_bytes(c), 0, NULL_HINT, NO_ALLOC);
  for(u32 f = 0; f < 6; ++f)
    for(u32 e = 0; e < 2; ++e) {
      ocrDbCreate(&g->face[f][e], &p, face_bytes(c, f >> 1), 0, NULL_HINT, NO_ALLOC);
      if(e == gen) strip[f] = (double*)p;
    }
}

/* Open one block for the epoch: give it a payload, then fill every strip it
 * sends, so that the epoch's first stage finds a halo waiting for it exactly
 * as every later stage does.  What a strip holds depends on the level of the
 * block behind the face, so a mesh change makes every strip stale and this
 * pass covers the whole mesh, not only the blocks that moved. */
static ocrGuid_t build_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  const mesh* m = (const mesh*)depv[BD_MESH].ptr;
  const control* c = &m->c;
  const bguid* og = depv[BD_OGUIDS].ptr ? ((const gslice*)depv[BD_OGUIDS].ptr)->g : NULL;
  u32 i = (u32)paramv[BP_IDX];
  u32 gen = (u32)paramv[BP_GEN];
  const blk* b = mesh_brec(m) + i;
  const s32* src = mesh_src(m) + 8*(u64)i;
  u8 kind = mesh_kind(m)[i];
  u8 inherits = (kind == BK_KEEP && src[0] >= 0);

  ocrGuid_t out;
  bguid* g;
  ocrDbCreate(&out, (void**)&g, sizeof(bguid), 0, NULL_HINT, NO_ALLOC);

  const double* cells = NULL;
  double* strip[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
  u32 slot = BD_SRC;

  if(inherits) {
    *g = og[src[0]];
    cells = (const double*)depv[slot++].ptr;
    for(u32 f = 0; f < 6; ++f)
      if(b->nbr[f] != NBR_NONE) strip[f] = (double*)depv[slot++].ptr;
  } else {
    double* w;
    chksum* ck;
    block_create(c, g, &w, gen, strip, &ck);
    if(kind == BK_KEEP) {
      cells_init(c, b->pos, w);
    } else if(kind == BK_CHILD) {
      u32 levels = (u32)src[1];
      const double* from = (const double*)depv[slot++].ptr;
      double* scratch = levels > 1 ? (double*)malloc((size_t)payload_bytes(c)) : NULL;
      /* A block that gained several levels at once upsamples once per level.
       * Each level overwrites its whole destination before it fills the
       * interior, so a level must not write the buffer it reads: the two
       * buffers alternate, with the parity chosen so the last one is the
       * payload. */
      double* buf[2] = { w, scratch };
      for(u32 l = 0; l < levels; ++l) {
        u32 sh = levels - 1 - l;
        u32 prong = ((b->pos[0] >> sh) & 1)
                  | (((b->pos[1] >> sh) & 1) << 1)
                  | (((b->pos[2] >> sh) & 1) << 2);
        double* dst = buf[(levels - 1 - l) & 1];
        cells_upsample(c, prong, from, dst);
        from = dst;
      }
      free(scratch);
    } else {
      cells_blank(c, w);
      for(u32 p = 0; p < 8; ++p)
        cells_merge(c, p, (const double*)depv[slot++].ptr, w);
    }
    cells = w;
    if(paramv[BP_INIT]) checksum_block(c, w, ck);
  }

  for(u32 f = 0; f < 6; ++f)
    if(b->nbr[f] != NBR_NONE) face_pack(c, b, f, cells, strip[f]);

  ocrDbRelease(depv[BD_MESH].guid);
  if(!ocrGuidIsNull(depv[BD_OGUIDS].guid)) ocrDbRelease(depv[BD_OGUIDS].guid);
  for(u32 s = BD_SRC; s < depc; ++s) ocrDbRelease(depv[s].guid);
  if(!inherits) {
    ocrDbRelease(g->payload);
    ocrDbRelease(g->chk);
    for(u32 f = 0; f < 6; ++f)
      for(u32 e = 0; e < 2; ++e) ocrDbRelease(g->face[f][e]);
  }
  ocrDbRelease(out);
  ocrAddDependence(out, guid_load(paramv+BP_PARENT), (u32)paramv[BP_SLOT], DB_MODE_CONST);
  return NULL_GUID;
}

/* A leaf's last hand-off can start the last task its collector waits on, so
 * the leaf gives up what it holds before that hand-off. */
static void spawn_ctx_release(const spawn_ctx* x)
{
  if(!ocrGuidIsNull(x->mesh)) ocrDbRelease(x->mesh);
  if(!ocrGuidIsNull(x->guids)) ocrDbRelease(x->guids);
  if(!ocrGuidIsNull(x->omesh)) ocrDbRelease(x->omesh);
  if(!ocrGuidIsNull(x->oguids)) ocrDbRelease(x->oguids);
}

void spawn_build(const spawn_ctx* x, u32 lo, u32 hi)
{
  const mesh* m = x->m;
  const control* c = &m->c;
  const blk* br = mesh_brec(m);
  const u32* rs = mesh_rstart(m);
  const s32* src = mesh_src(m);
  const u8* kind = mesh_kind(m);
  u32 gen = (u32)x->arg[PA_GEN];
  ocrGuid_t tmp;
  ocrEdtTemplateCreate(&tmp, build_edt, BP_NPARAM, EDT_PARAM_UNK);

  for(u32 t = lo; t < hi; ++t) {
    u32 i = rs[x->rank] + t;
    const blk* b = &br[i];
    const s32* s0 = src + 8*(u64)i;
    u8 inherits = (kind[i] == BK_KEEP && s0[0] >= 0);
    u32 nsrc = kind[i] == BK_MERGE ? 8 : (inherits || kind[i] == BK_CHILD ? 1 : 0);
    u32 nstr = inherits ? out_faces(b) : 0;
    u64 pv[BP_NPARAM];
    pv[BP_IDX] = i;
    pv[BP_GEN] = gen;
    pv[BP_INIT] = x->arg[PA_CHK];
    pv[BP_SLOT] = t - lo;
    guid_store(pv+BP_PARENT, x->join);
    ocrGuid_t edt;
    ocrHint_t hnt;
    ocrEdtCreate(&edt, tmp, BP_NPARAM, pv, BD_SRC + nsrc + nstr, NULL, 0,
                 edt_at(&hnt, c, b->owner), NULL);
    ocrAddDependence(x->oguids, edt, BD_OGUIDS,
                     ocrGuidIsNull(x->oguids) ? DB_MODE_NULL : DB_MODE_CONST);
    u32 s = BD_SRC;
    if(kind[i] == BK_MERGE)
      for(u32 p = 0; p < 8; ++p)
        ocrAddDependence(x->og[s0[p]].payload, edt, s++, DB_MODE_CONST);
    else if(nsrc)
      ocrAddDependence(x->og[s0[0]].payload, edt, s++, DB_MODE_CONST);
    if(nstr)
      for(u32 f = 0; f < 6; ++f)
        if(b->nbr[f] != NBR_NONE)
          ocrAddDependence(x->og[s0[0]].face[f][gen], edt, s++, DB_MODE_RW);
    if(t + 1 == hi) spawn_ctx_release(x);
    ocrAddDependence(x->mesh, edt, BD_MESH, DB_MODE_CONST);
  }
  ocrEdtTemplateDestroy(tmp);
}

/* ---- a stage ---------------------------------------------------------- */

/* One stage of one block, in one task: take in the strips the neighbours
 * wrote at the previous stage, reflect the faces that are the edge of the
 * mesh, run the stencil over every variable, take the checksum point where
 * the stage is one, and write the strips the neighbours read next.  Reads and
 * writes are on different generations of the strips, so no strip has a reader
 * and a writer in the same stage. */
static ocrGuid_t stage_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  const mesh* m = (const mesh*)depv[SD_MESH].ptr;
  const control* c = &m->c;
  u32 i = (u32)paramv[SP_IDX];
  const blk* b = mesh_brec(m) + i;
  double* cells = (double*)depv[SD_PAY].ptr;

  u32 s = SD_STRIP + out_faces(b);
  for(u32 f = 0; f < 6; ++f) {
    if(b->nbr[f] == NBR_NONE) { face_edge(c, f, cells); continue; }
    u32 n = face_slots(b, f);
    for(u32 k = 0; k < n; ++k)
      face_unpack(c, b, f, k, (const double*)depv[s++].ptr, cells);
  }

  for(s32 v = 0; v < c->num_vars; ++v) stencil_calc(c, cells, v);

  if(depv[SD_CHK].ptr) checksum_block(c, cells, (chksum*)depv[SD_CHK].ptr);

  s = SD_STRIP;
  for(u32 f = 0; f < 6; ++f)
    if(b->nbr[f] != NBR_NONE) face_pack(c, b, f, cells, (double*)depv[s++].ptr);

  return NULL_GUID;
}

void spawn_stage(const spawn_ctx* x, u32 lo, u32 hi)
{
  const mesh* m = x->m;
  const control* c = &m->c;
  const blk* br = mesh_brec(m);
  const u32* rs = mesh_rstart(m);
  u32 rg = (u32)x->arg[PA_GEN];
  u8 chk = (u8)x->arg[PA_CHK];
  ocrGuid_t tmp;
  ocrEdtTemplateCreate(&tmp, stage_edt, SP_NPARAM, EDT_PARAM_UNK);

  for(u32 t = lo; t < hi; ++t) {
    u32 i = rs[x->rank] + t;
    const blk* b = &br[i];
    u64 pv[SP_NPARAM];
    pv[SP_IDX] = i;
    ocrGuid_t edt, done;
    ocrHint_t hnt;
    OEVT_COUNTED_PRE(done);
    ocrEdtCreate(&edt, tmp, SP_NPARAM, pv, SD_STRIP + out_faces(b) + in_slots(b), NULL,
                 OEVT_COUNTED_PROP(0), edt_at(&hnt, c, b->owner), &done);
    ocrAddDependence(done, x->join, t - lo, DB_MODE_CONST);
    ocrAddDependence(x->g[i].payload, edt, SD_PAY, DB_MODE_RW);
    if(chk) ocrAddDependence(x->g[i].chk, edt, SD_CHK, DB_MODE_RW);
    else    ocrAddDependence(NULL_GUID, edt, SD_CHK, DB_MODE_NULL);
    u32 s = SD_STRIP;
    for(u32 f = 0; f < 6; ++f)
      if(b->nbr[f] != NBR_NONE)
        ocrAddDependence(x->g[i].face[f][rg ^ 1], edt, s++, DB_MODE_RW);
    for(u32 f = 0; f < 6; ++f) {
      u32 n = face_slots(b, f);
      for(u32 k = 0; k < n; ++k)
        ocrAddDependence(x->g[b->in[f][k]].face[f ^ 1][rg], edt, s++, DB_MODE_CONST);
    }
    if(t + 1 == hi) spawn_ctx_release(x);
    ocrAddDependence(x->mesh, edt, SD_MESH, DB_MODE_CONST);
  }
  ocrEdtTemplateDestroy(tmp);
}

/* ---- reclaiming the previous epoch ------------------------------------ */

static ocrGuid_t release_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  if(paramv[RP_DEAD]) {
    const bguid* g = ((const gslice*)depv[0].ptr)->g + paramv[RP_IDX];
    ocrDbDestroy(g->payload);
    ocrDbDestroy(g->chk);
    for(u32 f = 0; f < 6; ++f)
      for(u32 e = 0; e < 2; ++e) ocrDbDestroy(g->face[f][e]);
  }
  return NULL_GUID;
}

void spawn_release(const spawn_ctx* x, u32 lo, u32 hi)
{
  const control* c = &x->m->c;
  const blk* br = mesh_brec(x->om);
  const u32* rs = mesh_rstart(x->om);
  const u8* dead = mesh_odead(x->m);
  ocrGuid_t tmp;
  ocrEdtTemplateCreate(&tmp, release_edt, RP_NPARAM, 1);

  for(u32 t = lo; t < hi; ++t) {
    u32 i = rs[x->rank] + t;
    ocrGuid_t edt, done;
    ocrHint_t hnt;
    u64 pv[RP_NPARAM];
    pv[RP_IDX] = i;
    pv[RP_DEAD] = dead[i];
    OEVT_COUNTED_PRE(done);
    ocrEdtCreate(&edt, tmp, RP_NPARAM, pv, 1, NULL, OEVT_COUNTED_PROP(0),
                 edt_at(&hnt, c, br[i].owner), &done);
    if(t + 1 == hi) spawn_ctx_release(x);
    ocrAddDependence(done, x->join, t - lo, DB_MODE_CONST);
    ocrAddDependence(x->oguids, edt, 0, DB_MODE_CONST);
  }
  ocrEdtTemplateDestroy(tmp);
}

/* ---- the checksum reduction ------------------------------------------- */

/* An interior node of the block forest adds its eight children in prong
 * order.  The grouping is what the reduction is defined by: the same values
 * added in another shape are a different number in floating point, so the
 * tree follows the mesh rather than the machine. */
static ocrGuid_t sum8_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  const mesh* m = (const mesh*)depv[0].ptr;
  const control* c = &m->c;
  const fnode* nd = mesh_fnode(m) + paramv[NP_NODE];

  ocrGuid_t out;
  chksum* o;
  ocrDbCreate(&out, (void**)&o, chksum_bytes(c), 0, NULL_HINT, NO_ALLOC);
  const chksum* first = (const chksum*)depv[1].ptr;
  o->weighted = first->weighted;
  for(s32 v = 0; v < c->num_vars; ++v) o->sum[v] = first->sum[v];
  for(u32 j = 1; j < 8; ++j) {
    const chksum* p = (const chksum*)depv[1+j].ptr;
    o->weighted += p->weighted;
    for(s32 v = 0; v < c->num_vars; ++v) o->sum[v] += p->sum[v];
  }
  for(u32 j = 0; j < 8; ++j)
    if(nd->child[j] < 0) ocrDbDestroy(depv[1+j].guid);
    else ocrDbRelease(depv[1+j].guid);
  ocrDbRelease(depv[0].guid);
  ocrDbRelease(out);

  ocrAddDependence(out, guid_load(paramv+NP_PARENT), (u32)paramv[NP_SLOT], DB_MODE_CONST);
  return NULL_GUID;
}

/// A run of base blocks, laid out in the order the final accumulation adds them.
static ocrGuid_t cbase_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  const mesh* m = (const mesh*)depv[0].ptr;
  const control* c = &m->c;
  const s32* root = mesh_root(m) + paramv[AP_LO];
  u32 n = depc - 1;
  u32 w = (u32)c->num_vars + 1;

  ocrGuid_t out;
  chkarr* a;
  ocrDbCreate(&out, (void**)&a, sizeof(chkarr) + (u64)n*w*sizeof(double), 0,
              NULL_HINT, NO_ALLOC);
  a->n = n;
  for(u32 j = 0; j < n; ++j) {
    const chksum* p = (const chksum*)depv[1+j].ptr;
    a->v[j*w] = p->weighted;
    for(s32 v = 0; v < c->num_vars; ++v) a->v[j*w + 1 + v] = p->sum[v];
    if(root[j] < 0) ocrDbDestroy(depv[1+j].guid);
    else ocrDbRelease(depv[1+j].guid);
  }
  ocrDbRelease(depv[0].guid);
  ocrDbRelease(out);
  ocrAddDependence(out, guid_load(paramv+AP_PARENT), (u32)paramv[AP_SLOT], DB_MODE_CONST);
  return NULL_GUID;
}

static ocrGuid_t ccat_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  const mesh* m = (const mesh*)depv[0].ptr;
  u32 w = (u32)m->c.num_vars + 1;
  u64 n = 0;
  for(u32 j = 1; j < depc; ++j) n += ((const chkarr*)depv[j].ptr)->n;

  ocrGuid_t out;
  chkarr* a;
  ocrDbCreate(&out, (void**)&a, sizeof(chkarr) + n*w*sizeof(double), 0, NULL_HINT, NO_ALLOC);
  a->n = n;
  u64 k = 0;
  for(u32 j = 1; j < depc; ++j) {
    const chkarr* s = (const chkarr*)depv[j].ptr;
    memcpy(a->v + k*w, s->v, (size_t)s->n*w*sizeof(double));
    k += s->n;
    ocrDbDestroy(depv[j].guid);
  }
  ocrDbRelease(depv[0].guid);
  ocrDbRelease(out);
  ocrAddDependence(out, guid_load(paramv+AP_PARENT), (u32)paramv[AP_SLOT], DB_MODE_CONST);
  return NULL_GUID;
}

/* Wire one base block's subtree into the run it belongs to.  A live block
 * contributes the block it already holds; anywhere the mesh went finer, the
 * eight below are added first. */
static void emit_subtree(const mesh* m, ocrGuid_t mguid, const bguid* g, s32 node,
                         u32 level, const u32 p[3], ocrGuid_t parent, u32 slot)
{
  const control* c = &m->c;
  if(node >= 0) {
    ocrAddDependence(g[node].chk, parent, slot, DB_MODE_CONST);
    return;
  }
  u32 idx = (u32)(-node - 1);
  u64 pv[NP_NPARAM];
  pv[NP_NODE] = idx;
  pv[NP_SLOT] = slot;
  guid_store(pv+NP_PARENT, parent);
  ocrGuid_t tmp, edt;
  ocrHint_t hnt;
  ocrEdtTemplateCreate(&tmp, sum8_edt, NP_NPARAM, 9);
  ocrEdtCreate(&edt, tmp, NP_NPARAM, pv, 9, NULL, 0,
               edt_at(&hnt, c, blk_owner(c, level, p)), NULL);
  ocrEdtTemplateDestroy(tmp);
  ocrAddDependence(mguid, edt, 0, DB_MODE_CONST);
  for(u32 k = 0; k < 8; ++k) {
    u32 cp[3] = { (p[0] << 1) + (k & 1), (p[1] << 1) + ((k >> 1) & 1),
                  (p[2] << 1) + ((k >> 2) & 1) };
    emit_subtree(m, mguid, g, mesh_fnode(m)[idx].child[k], level + 1, cp, edt, 1 + k);
  }
}

static ocrGuid_t chkfan_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  const mesh* m = (const mesh*)depv[KD_MESH].ptr;
  const control* c = &m->c;
  const bguid* g = ((const gslice*)depv[KD_GUIDS].ptr)->g;
  u32 lo = (u32)paramv[KP_LO], hi = (u32)paramv[KP_HI];
  ocrGuid_t parent = guid_load(paramv+KP_PARENT);
  u32 slot = (u32)paramv[KP_SLOT];
  u32 n = hi - lo;

  u32 bp[3];
  bp[0] = lo/((u32)c->npy*c->npz);
  bp[1] = (lo/(u32)c->npz) % (u32)c->npy;
  bp[2] = lo % (u32)c->npz;
  u32 rank = blk_owner(c, 0, bp);

  if(n > c->width) {
    u32 u = (n + c->width - 1)/c->width;
    u32 k = u < FAN_BRANCH ? u : FAN_BRANCH;
    u64 pv[AP_NPARAM];
    pv[AP_LO] = lo; pv[AP_SLOT] = slot;
    guid_store(pv+AP_PARENT, parent);
    ocrGuid_t tmp, node;
    ocrHint_t hnt;
    ocrEdtTemplateCreate(&tmp, ccat_edt, AP_NPARAM, k+1);
    ocrEdtCreate(&node, tmp, AP_NPARAM, pv, k+1, NULL, 0, edt_at(&hnt, c, rank), NULL);
    ocrEdtTemplateDestroy(tmp);
    ocrAddDependence(depv[KD_MESH].guid, node, 0, DB_MODE_CONST);
    ocrEdtTemplateCreate(&tmp, chkfan_edt, KP_NPARAM, KD_DEPC);
    for(u32 i = 0; i < k; ++i) {
      u32 clo = lo + (u32)(((u64)u*i) / k)*c->width;
      u32 chi = lo + (u32)(((u64)u*(i+1)) / k)*c->width;
      if(chi > hi) chi = hi;
      u64 kv[KP_NPARAM];
      kv[KP_LO] = clo; kv[KP_HI] = chi; kv[KP_SLOT] = 1 + i;
      guid_store(kv+KP_PARENT, node);
      ocrGuid_t child;
      ocrEdtCreate(&child, tmp, KP_NPARAM, kv, KD_DEPC, NULL, 0, edt_at(&hnt, c, rank), NULL);
      if(i + 1 == k) {
        ocrDbRelease(depv[KD_MESH].guid);
        ocrDbRelease(depv[KD_GUIDS].guid);
      }
      ocrAddDependence(depv[KD_MESH].guid, child, KD_MESH, DB_MODE_CONST);
      ocrAddDependence(depv[KD_GUIDS].guid, child, KD_GUIDS, DB_MODE_CONST);
    }
    ocrEdtTemplateDestroy(tmp);
    return NULL_GUID;
  }

  u64 pv[AP_NPARAM];
  pv[AP_LO] = lo; pv[AP_SLOT] = slot;
  guid_store(pv+AP_PARENT, parent);
  ocrGuid_t tmp, node;
  ocrHint_t hnt;
  ocrEdtTemplateCreate(&tmp, cbase_edt, AP_NPARAM, n+1);
  ocrEdtCreate(&node, tmp, AP_NPARAM, pv, n+1, NULL, 0, edt_at(&hnt, c, rank), NULL);
  ocrEdtTemplateDestroy(tmp);
  const s32* root = mesh_root(m);
  for(u32 j = lo; j < hi; ++j) {
    u32 p[3];
    p[0] = j/((u32)c->npy*c->npz);
    p[1] = (j/(u32)c->npz) % (u32)c->npy;
    p[2] = j % (u32)c->npz;
    emit_subtree(m, depv[KD_MESH].guid, g, root[j], 0, p, node, 1 + (j - lo));
  }
  ocrDbRelease(depv[KD_MESH].guid);
  ocrDbRelease(depv[KD_GUIDS].guid);
  ocrAddDependence(depv[KD_MESH].guid, node, 0, DB_MODE_CONST);
  return NULL_GUID;
}

/* ---- the control spine ------------------------------------------------ */

static ocrGuid_t spine(ocrEdt_t fn, u32 depc, u32 paramc, u64* pv, const control* c,
                       const runstate* st, ocrGuid_t stg)
{
  ocrGuid_t tmp, edt;
  ocrHint_t hnt;
  ocrEdtTemplateCreate(&tmp, fn, paramc, depc);
  ocrEdtCreate(&edt, tmp, paramc, pv, depc, NULL, 0, edt_at(&hnt, c, 0), NULL);
  ocrEdtTemplateDestroy(tmp);
  ocrAddDependence(stg, edt, SL_ST, DB_MODE_RW);
  ocrAddDependence(st->cfg, edt, SL_CFG, DB_MODE_CONST);
  ocrAddDependence(st->obj, edt, SL_OBJ, DB_MODE_RW);
  ocrAddDependence(st->mesh, edt, SL_MESH, DB_MODE_CONST);
  return edt;
}

static void launch_stage(const control* c, runstate* st, ocrGuid_t stg, u8 chk)
{
  ocrGuid_t next = spine(staged_edt, SL_DEPC, 0, NULL, c, st, stg);
  ocrGuid_t dep[4];
  dep[0] = st->mesh; dep[1] = st->guids;
  dep[2] = NULL_GUID; dep[3] = NULL_GUID;
  u64 arg[PA_NARG];
  arg[PA_GEN] = st->gen;
  arg[PA_TS] = (u64)(s64)st->ts;
  arg[PA_CHK] = chk;
  fan_launch(PH_STAGE, c, dep, next, SL_X, arg);
}

static void launch_checksum(const control* c, runstate* st, ocrGuid_t stg, s32 ts)
{
  u64 pv[1];
  pv[0] = (u64)(s64)ts;
  ocrGuid_t next = spine(chkfinal_edt, SL_DEPC, 1, pv, c, st, stg);
  u64 kv[KP_NPARAM];
  kv[KP_LO] = 0;
  kv[KP_HI] = (u64)(c->npx*c->npy*c->npz);
  kv[KP_SLOT] = SL_X;
  guid_store(kv+KP_PARENT, next);
  ocrGuid_t tmp, edt;
  ocrHint_t hnt;
  ocrEdtTemplateCreate(&tmp, chkfan_edt, KP_NPARAM, KD_DEPC);
  ocrEdtCreate(&edt, tmp, KP_NPARAM, kv, KD_DEPC, NULL, 0, edt_at(&hnt, c, 0), NULL);
  ocrEdtTemplateDestroy(tmp);
  ocrAddDependence(st->mesh, edt, KD_MESH, DB_MODE_CONST);
  ocrAddDependence(st->guids, edt, KD_GUIDS, DB_MODE_CONST);
}

static void launch_plan(const control* c, runstate* st, ocrGuid_t stg, u32 mode, s32 ts)
{
  u64 pv[2];
  pv[0] = mode;
  pv[1] = (u64)(s64)ts;
  spine(plan_edt, SL_MESH + 1, 2, pv, c, st, stg);
}

/* Every control task holds the run state for writing and the directory in
 * force for reading, so the chain of them is serial by construction and no
 * two ever look at the mesh at once. */

static void finish(const control* c, runstate* st)
{
  if(st->last_ts >= 0) {
    ocrPrintf("Grand Total Checksum == %f at timestep %d\n", st->last_total, st->last_ts);
    ocrPrintf("Weighted Mesh Checksum == %f at timestep %d\n", st->last_weighted, st->last_ts);
  }
  ocrPrintf("Success!  miniAMR shutting down normally.\n");
  ocrShutdown();
}

/* Advance past the stage that has just finished: the next stage of the
 * timestep, or the end of the timestep — where the objects move and, on the
 * rounds the run asks for, the mesh is remade. */
static void advance(const control* c, runstate* st, ocrGuid_t stg, object* obj)
{
  ++st->stage;
  if(st->stage < c->stages_per_ts) {
    launch_stage(c, st, stg,
                 (u8)(c->checksum_freq && !(st->stage % c->checksum_freq)));
    return;
  }
  st->stage = 0;
  if(c->num_refine && !c->uniform_refine) {
    object_move(c, obj);
    if(!(st->ts % c->refine_freq)) {
      st->step = ST_REFINET;
      launch_plan(c, st, stg, MP_REFINE, st->ts);
      return;
    }
  }
  ++st->ts;
  if(st->ts > c->num_tsteps) { finish(c, st); return; }
  launch_stage(c, st, stg, (u8)(c->checksum_freq != 0));
}

/* What follows an epoch, once the mesh it opened is packed and the epoch it
 * replaced is given back. */
static void epoch_next(const control* c, runstate* st, ocrGuid_t stg, object* obj)
{
  if(st->step == ST_BOOT) {
    st->step = ST_PRE;
    st->pre = 0;
    launch_checksum(c, st, stg, 0);
    return;
  }
  if(st->step == ST_REFINE0) {
    st->step = ST_LOOP;
    st->ts = 1;
    st->stage = 0;
    launch_stage(c, st, stg, (u8)(c->checksum_freq != 0));
    return;
  }
  ++st->ts;
  st->stage = 0;
  st->step = ST_LOOP;
  if(st->ts > c->num_tsteps) { finish(c, st); return; }
  launch_stage(c, st, stg, (u8)(c->checksum_freq != 0));
}

/* One phase of a refinement round.  The whole round is a sequence of them:
 * one that splits every block the objects and the level-difference rule put
 * a finer mesh under, then one per level that collapses the octets that may
 * coarsen.  A phase that leaves the block set alone changes only the
 * dispositions, so it costs a directory and nothing else. */
static ocrGuid_t plan_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  runstate* st = (runstate*)depv[SL_ST].ptr;
  const control* c = (const control*)depv[SL_CFG].ptr;
  object* obj = (object*)depv[SL_OBJ].ptr;
  const mesh* old = (const mesh*)depv[SL_MESH].ptr;
  ocrGuid_t stg = depv[SL_ST].guid;
  u32 mode = (u32)paramv[0];
  s32 ts = (s32)(s64)paramv[1];

  void* w = mesh_compute(c, old, obj, ts, mode);
  ocrGuid_t nm;
  mesh* m;
  ocrDbCreate(&nm, (void**)&m, mesh_size_bytes(w), 0, NULL_HINT, NO_ALLOC);
  u8 bad = mesh_emit(w, m);
  m->epoch = old->epoch + 1;
  u8 changed = mesh_changed(w);
  mesh_free(w);
  /* A directory that broke one of the mesh's own invariants is never
   * published: every task downstream would resolve a strip or a forest node
   * through it. */
  if(bad) { ocrShutdown(); return NULL_GUID; }
  ocrDbRelease(depv[SL_MESH].guid);
  s32 unref_level = m->unref_level;
  ocrDbRelease(nm);

  st->omesh = st->mesh;
  st->oguids = st->guids;
  st->mesh = nm;

  if(!changed) {
    ocrDbDestroy(st->omesh);
    st->omesh = NULL_GUID;
    st->oguids = NULL_GUID;
    if(unref_level >= 1) {
      launch_plan(c, st, stg, MP_UNREFINE, ts);
      return NULL_GUID;
    }
    epoch_next(c, st, stg, obj);
    return NULL_GUID;
  }

  st->gen = 0;
  ocrGuid_t next = spine(built_edt, SL_DEPC, 1, paramv+1, c, st, stg);
  ocrGuid_t dep[4];
  dep[0] = nm; dep[1] = NULL_GUID;
  dep[2] = st->omesh; dep[3] = st->oguids;
  u64 arg[PA_NARG];
  arg[PA_GEN] = 0;
  arg[PA_TS] = (u64)(s64)ts;
  arg[PA_CHK] = 0;
  fan_launch(PH_BUILD, c, dep, next, SL_X, arg);
  return NULL_GUID;
}

static ocrGuid_t built_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  runstate* st = (runstate*)depv[SL_ST].ptr;
  const control* c = (const control*)depv[SL_CFG].ptr;
  object* obj = (object*)depv[SL_OBJ].ptr;
  ocrGuid_t stg = depv[SL_ST].guid;

  st->guids = depv[SL_X].guid;
  ocrDbRelease(depv[SL_MESH].guid);
  ocrDbRelease(depv[SL_X].guid);

  if(ocrGuidIsNull(st->omesh)) { epoch_next(c, st, stg, obj); return NULL_GUID; }

  ocrGuid_t next = spine(released_edt, SL_DEPC, 1, paramv, c, st, stg);
  ocrGuid_t dep[4];
  dep[0] = st->mesh; dep[1] = st->guids;
  dep[2] = st->omesh; dep[3] = st->oguids;
  fan_launch(PH_RELEASE, c, dep, next, SL_X, NULL);
  return NULL_GUID;
}

static ocrGuid_t released_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  runstate* st = (runstate*)depv[SL_ST].ptr;
  const control* c = (const control*)depv[SL_CFG].ptr;
  object* obj = (object*)depv[SL_OBJ].ptr;
  const mesh* m = (const mesh*)depv[SL_MESH].ptr;
  ocrGuid_t stg = depv[SL_ST].guid;
  s32 ts = (s32)(s64)paramv[0];

  ocrDbDestroy(st->oguids);
  ocrDbDestroy(st->omesh);
  st->omesh = NULL_GUID;
  st->oguids = NULL_GUID;

  u8 unref = m->unref_level >= 1;
  ocrDbRelease(depv[SL_MESH].guid);
  if(unref) { launch_plan(c, st, stg, MP_UNREFINE, ts); return NULL_GUID; }
  epoch_next(c, st, stg, obj);
  return NULL_GUID;
}

static ocrGuid_t staged_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  runstate* st = (runstate*)depv[SL_ST].ptr;
  const control* c = (const control*)depv[SL_CFG].ptr;
  object* obj = (object*)depv[SL_OBJ].ptr;
  ocrGuid_t stg = depv[SL_ST].guid;
  ocrDbRelease(depv[SL_MESH].guid);

  st->gen ^= 1;
  if(c->checksum_freq && !(st->stage % c->checksum_freq)) {
    launch_checksum(c, st, stg, st->ts);
    return NULL_GUID;
  }
  advance(c, st, stg, obj);
  return NULL_GUID;
}

/* The last edge of a checksum round.  The base blocks are added in the order
 * the mesh lays them out, which is why the run's reported value does not
 * depend on how the work was spread; the first round is kept as the
 * reference every later round is held against. */
static ocrGuid_t chkfinal_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  runstate* st = (runstate*)depv[SL_ST].ptr;
  const control* c = (const control*)depv[SL_CFG].ptr;
  object* obj = (object*)depv[SL_OBJ].ptr;
  ocrGuid_t stg = depv[SL_ST].guid;
  const chkarr* a = (const chkarr*)depv[SL_X].ptr;
  s32 ts = (s32)(s64)paramv[0];
  u32 w = (u32)c->num_vars + 1;
  ocrDbRelease(depv[SL_MESH].guid);

  double weighted = 0.0;
  double* sum = (double*)malloc((size_t)c->num_vars*sizeof(double));
  for(s32 v = 0; v < c->num_vars; ++v) sum[v] = 0.0;
  for(u64 j = 0; j < a->n; ++j) {
    weighted += a->v[j*w];
    for(s32 v = 0; v < c->num_vars; ++v) sum[v] += a->v[j*w + 1 + v];
  }
  ocrDbDestroy(depv[SL_X].guid);

  u8 first = (st->rounds == 0);
  if(first) {
    st->golden[0] = weighted;
    for(s32 v = 0; v < c->num_vars; ++v) st->golden[1+v] = sum[v];
  }

  double total = 0.0;
  u8 bad = 0;
  for(s32 v = 0; v < c->num_vars; ++v) {
    total += sum[v];
    double g = st->golden[1+v];
    if(c->report_diffusion)
      ocrPrintf("%d var %d sum %lf old %lf diff %lf tol %lf\n", ts, v, sum[v], g,
                fabs(sum[v] - g)/g, c->tol);
    if(fabs(sum[v] - g)/g > c->tol) {
      ocrPrintf("Time step %d sum %lf (old %lf) variable %d difference too large\n",
                ts, sum[v], g, v);
      bad = 1;
    }
  }
  free(sum);

  st->last_total = total;
  st->last_weighted = weighted;
  st->last_ts = ts;
  ++st->rounds;
  if(first) ocrPrintf("Golden Checksum == %f (weighted %f)\n", total, weighted);
  if(bad) { ocrShutdown(); return NULL_GUID; }

  if(st->step == ST_PRE) {
    ++st->pre;
    if(st->pre < 2) { launch_checksum(c, st, stg, 0); return NULL_GUID; }
    if(c->num_refine != 0 || c->uniform_refine) {
      st->step = ST_REFINE0;
      launch_plan(c, st, stg, MP_REFINE, 0);
      return NULL_GUID;
    }
    st->step = ST_LOOP;
    st->ts = 1;
    st->stage = 0;
    launch_stage(c, st, stg, (u8)(c->checksum_freq != 0));
    return NULL_GUID;
  }
  advance(c, st, stg, obj);
  return NULL_GUID;
}

/* ---- start ------------------------------------------------------------ */

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  u64 argc = ocrGetArgc(depv[0].ptr);

  ocrGuid_t cfgdb;
  control* c;
  ocrDbCreate(&cfgdb, (void**)&c, sizeof(control), 0, NULL_HINT, NO_ALLOC);
  memset(c, 0, sizeof(control));
  u64 pds = 0;
  ocrAffinityCount(AFFINITY_PD, &pds);
  c->nranks = pds ? (u32)pds : 1;

  ocrGuid_t objdb = NULL_GUID;
  object* obj = NULL;
  if(parse_command((u32)argc, depv[0].ptr, c, &obj, &objdb)) {
    ocrShutdown();
    return ERROR_GUID;
  }
  ocrDbDestroy(depv[0].guid);

  ocrGuid_t stg;
  runstate* st;
  ocrDbCreate(&stg, (void**)&st, sizeof(runstate) + (u64)(c->num_vars+1)*sizeof(double),
              0, NULL_HINT, NO_ALLOC);
  memset(st, 0, sizeof(runstate));
  st->cfg = cfgdb;
  st->obj = objdb;
  st->omesh = NULL_GUID;
  st->oguids = NULL_GUID;
  st->guids = NULL_GUID;
  st->step = ST_BOOT;
  st->last_ts = -1;
  st->gen = 0;

  void* w = mesh_compute(c, NULL, obj, 0, MP_INIT);
  mesh* m;
  ocrDbCreate(&st->mesh, (void**)&m, mesh_size_bytes(w), 0, NULL_HINT, NO_ALLOC);
  u8 bad = mesh_emit(w, m);
  mesh_free(w);
  if(bad) { ocrShutdown(); return ERROR_GUID; }
  ocrDbRelease(st->mesh);

  u64 bpv[1] = { 0 };
  ocrGuid_t next = spine(built_edt, SL_DEPC, 1, bpv, c, st, stg);
  ocrGuid_t dep[4];
  dep[0] = st->mesh; dep[1] = NULL_GUID;
  dep[2] = NULL_GUID; dep[3] = NULL_GUID;
  u64 arg[PA_NARG];
  arg[PA_GEN] = 0;
  arg[PA_TS] = 0;
  arg[PA_CHK] = 1;
  fan_launch(PH_BUILD, c, dep, next, SL_X, arg);

  return NULL_GUID;
}
