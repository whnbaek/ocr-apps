/* Graph500-shaped BFS in owner-computes distributed form.
 *
 * The instance, the search and the answer are those of the row-and-column
 * grid program beside it: the same uniform-random edge stream drawn from the
 * same fixed seed, the same single level-synchronous search from the same
 * root, and the same digest over the finished search tree.  What differs is
 * the decomposition.
 *
 *   Construction.  The edge stream is a linear recurrence, so an index into
 *   it can be reached directly: a generator task jumps the state to the head
 *   of its own contiguous slice and draws only that slice.  Every generated
 *   edge is then routed once to the owner of its source vertex, which is the
 *   only place that ever needs it.  Total generation work is the stream, not
 *   the stream times the worker count.
 *
 *   Search.  A vertex is owned by exactly one worker and its adjacency lives
 *   with it, so a level is: fold the candidates addressed to me, keep the
 *   ones that reach an unvisited vertex, and walk the adjacency of exactly
 *   those.  Both are proportional to the frontier.  Candidates leave a node
 *   in one block per destination node per level, so the number of messages a
 *   level costs is the square of the node count and their volume is the
 *   frontier -- neither is a function of the logical grid.
 *
 *   Control.  Each node runs a persistent chain (hub -> workers -> pack ->
 *   hub) pinned to itself; nothing is created for a level that does not run,
 *   and no task ever issues a create for another node.
 *
 * WHY THE ANSWER IS THE SAME.  A vertex's level is its distance from the
 * root, which no decomposition can change.  Its recorded parent is decided
 * by a rule that reads only the row extent R: among the frontier vertices
 * adjacent to it, the ones with the smallest residue modulo R win, and the
 * largest of those is taken.  That rule is a minimum under a total order, so
 * it is commutative and associative and may be evaluated in any grouping --
 * here, once at the owner of the discovered vertex.  The digest is a sum of
 * an avalanche of (vertex, level, parent) over the reached vertices, so it
 * follows.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/time.h>

#ifndef ENABLE_EXTENSION_AFFINITY
#define ENABLE_EXTENSION_AFFINITY
#endif
#include <ocr.h>
#include <extensions/ocr-affinity.h>
#include <extensions/ocr-labeling.h>

#ifndef PRIu64
#define PRIu64 "llu"
#endif
#ifndef UINT64_C
#define UINT64_C(c) c ## ULL
#endif

#define FLUSH fflush(0)

typedef u64 vertexType;

#define VERTEX_NONE ((vertexType)-1)
#define LEVEL_NONE ((u64)-1)

/* Fixed properties of the search, as the specification-shaped program fixes
 * them: one search, from this root, on a stream drawn from this seed. */
#define ROOT_VERTEX ((vertexType)4)
#define GRAPH_SEED UINT64_C(123456789)

/* Target task counts per node for the phases whose width is not the logical
 * grid: the routing of generated edges, and the construction of adjacency.
 * Both are one-shot phases whose work per task is the volume they move
 * divided by these, so the only requirement is that they exceed a node's
 * worker count while staying small enough that a consumer's dependence count
 * stays bounded. */
#define ROUTE_TASKS_TARGET 128
#define BUILD_TASKS_MAX 128

/* The only depth bound in the program: a channel carries one generation per
 * level, and an implementation may size that buffer from the declared
 * maximum.  A search that reaches this many levels stops with a message
 * naming the bound rather than running past the buffer. */
#define MAX_LEVELS 1024

#define GUID_WORDS ((sizeof(ocrGuid_t)+sizeof(u64)-1)/sizeof(u64))
#define PV_MAX(a,b) ((int)(a) > (int)(b) ? (int)(a) : (int)(b))

static inline void guid_store(u64* w, ocrGuid_t g) { memcpy(w, &g, sizeof(g)); }
static inline ocrGuid_t guid_load(const u64* w)
{
  ocrGuid_t g;
  memcpy(&g, w, sizeof(g));
  return g;
}

/* Words every task is handed by value.  A parameter block travels with the
 * create, so no task has to acquire a shared block to learn the instance. */
enum { GP_SIZE, GP_R, GP_C, GP_W, GP_VSIZE, GP_ESIZE,
       GP_NRANK, GP_NROUTE, GP_N };

/* ---- vertex and worker maps ------------------------------------------- */

/* A worker owns one residue class of one contiguous vertex block; the maps
 * below are inverses of each other. */
static inline u64 vertex_worker(u64 v, u64 R, u64 C, u64 vSIZE)
{
  u64 y = v % (vSIZE*R);
  return (y % R)*C + v/(vSIZE*R);
}
static inline u64 vertex_index(u64 v, u64 R, u64 vSIZE)
{
  return (v % (vSIZE*R)) / R;
}
static inline u64 worker_vertex(u64 w, u64 id, u64 R, u64 C, u64 vSIZE)
{
  return (vSIZE*(w % C) + id)*R + (w / C);
}

/* The vertex count is a power of two and the grid extents must divide it, so
 * every extent is a power of two and the maps above are shifts and masks.
 * The phases that apply them once per edge and once per candidate take them
 * in this form; a compiler cannot know the extents are powers of two, and the
 * divisions are the whole of those loops' arithmetic. */
typedef struct {
  u64 blkMask; u32 blkShift;   /* the contiguous vertex block, vSIZE*R */
  u64 rMask;   u32 rShift;     /* the residue class, R */
  u64 C; u64 nrank; u32 wShift;
} vmap_t;

static u32 log2u(u64 x) { u32 s = 0; while ((((u64)1) << s) != x) ++s; return s; }

static void vmap_init(vmap_t* m, u64 R, u64 C, u64 vSIZE, u64 W, u64 nrank)
{
  m->blkShift = log2u(vSIZE*R); m->blkMask = vSIZE*R - 1;
  m->rShift = log2u(R);         m->rMask = R - 1;
  m->C = C; m->nrank = nrank;   m->wShift = log2u(W);
}
static inline u64 vmap_worker(const vmap_t* m, u64 v)
{
  return (v & m->rMask)*m->C + (v >> m->blkShift);
}
static inline u64 vmap_index(const vmap_t* m, u64 v)
{
  return (v & m->blkMask) >> m->rShift;
}
/* The node owning a vertex: its worker's place in the contiguous run. */
static inline u64 vmap_rank(const vmap_t* m, u64 v)
{
  return (((vmap_worker(m, v) + 1)*m->nrank - 1) >> m->wShift);
}

/* Workers are split into contiguous ranges, one per node, so a node's whole
 * share is named by two bounds and the inverse map is a division. */
static inline u64 rank_wlo(u64 rank, u64 W, u64 nrank) { return rank*W/nrank; }
static inline u64 worker_rank(u64 w, u64 W, u64 nrank)
{
  return ((w+1)*nrank - 1) / W;
}

/* ---- the answer ------------------------------------------------------- */

static u64 avalanche64(u64 x)
{
  x ^= x >> 30; x *= UINT64_C(0xBF58476D1CE4E5B9);
  x ^= x >> 27; x *= UINT64_C(0x94D049BB133111EB);
  x ^= x >> 31;
  return x;
}

/* One term of a set digest over the search result.  Distinct (vertex, level,
 * parent) triples must yield distinct terms with overwhelming probability,
 * and the caller must fold the terms with an operator that is commutative
 * and associative: the order in which a set of results is traversed is a
 * scheduling artefact and must not reach the digest. */
static u64 mixVisited(u64 vertex, u64 level, u64 parent)
{
  u64 x = avalanche64(vertex + UINT64_C(0x9E3779B97F4A7C15));
  x = avalanche64(x ^ (level + UINT64_C(0x9E3779B97F4A7C15)));
  return avalanche64(x ^ (parent + UINT64_C(0x9E3779B97F4A7C15)));
}

/* The tie-break that decides a discovered vertex's parent, as a total order
 * on the proposing vertices: the smallest residue class modulo the row
 * extent first, and within it the largest vertex.  A minimum under a total
 * order may be taken in any grouping, which is what lets one owner fold what
 * a grid of searches would have folded in stages. */
static inline int cand_better(vertexType a, vertexType b, u64 R)
{
  u64 ra = a % R, rb = b % R;
  if (ra != rb) return ra < rb;
  return a > b;
}

/* ---- the edge stream -------------------------------------------------- */

static u64 xorshift64star(u64 *x)
{
  *x ^= *x >> 12;
  *x ^= *x << 25;
  *x ^= *x >> 27;
  return *x * UINT64_C(2685821657736338717);
}

/* The state update above is linear over GF(2), so a power of it is a matrix
 * and an arbitrary jump costs a matrix-vector product rather than a walk.
 * A basis image array m holds the image of each unit vector, so applying it
 * is an exclusive-or of the images of the set bits. */
static u64 lin_apply(const u64* m, u64 v)
{
  u64 r = 0;
  int b;
  for (b = 0; v; ++b, v >>= 1)
    if (v & 1) r ^= m[b];
  return r;
}

/* tbl[j] is the basis image array of the update raised to 2^j, so any jump
 * is the composition of the entries named by the set bits of its length.
 * Powers of one matrix commute, so their order does not matter. */
#define JUMP_TABLE_WORDS (64*64)

static void jump_table_build(u64* tbl)
{
  u64 cur[64], nxt[64];
  int i, j, k;
  for (i = 0; i < 64; ++i) {
    u64 x = (u64)1 << i;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    cur[i] = x;
  }
  memcpy(tbl, cur, sizeof(cur));
  for (j = 1; j < 64; ++j) {
    for (k = 0; k < 64; ++k) nxt[k] = lin_apply(cur, cur[k]);
    memcpy(tbl + (size_t)j*64, nxt, sizeof(nxt));
    memcpy(cur, nxt, sizeof(nxt));
  }
}

static u64 prng_jump(const u64* tbl, u64 x, u64 k)
{
  int j;
  for (j = 0; j < 64 && k; ++j, k >>= 1)
    if (k & 1) x = lin_apply(tbl + (size_t)j*64, x);
  return x;
}

/* ---- block layouts ----------------------------------------------------
 *
 * Every block below is an array of u64 with a header of counts and offsets
 * followed by pairs.  A pair is two vertices: for construction the (source,
 * destination) of a directed edge, for the search the (discovered vertex,
 * proposing vertex) of a candidate.
 *
 *   generator block   [nrank+1 offsets by destination node][pairs]
 *   route block       [nw][nw+1 offsets by destination worker][pairs]
 *   candidate block   [count][digest][nrank+1 offsets by node][pairs]
 *   exchange block    [count][nw][nw+1 offsets by destination worker][pairs]
 *
 * A count is a number of pairs, never a number of words.  A worker's
 * adjacency is one block holding [vSIZE+1 offsets][neighbours].
 */

#define BLK_PAIR_WORDS 2

static inline u64* gen_off(u64* b) { return b; }
static inline u64* gen_pairs(u64* b, u64 nrank) { return b + nrank + 1; }
static inline u64 gen_words(u64 nrank, u64 npair)
{ return nrank + 1 + BLK_PAIR_WORDS*npair; }

static inline u64* rte_off(u64* b) { return b + 1; }
static inline u64* rte_pairs(u64* b) { return b + 2 + b[0]; }
static inline u64 rte_words(u64 nw, u64 npair)
{ return 2 + nw + BLK_PAIR_WORDS*npair; }

static inline u64* cnd_off(u64* b) { return b + 2; }
static inline u64* cnd_pairs(u64* b, u64 nrank) { return b + 3 + nrank; }
static inline u64 cnd_words(u64 nrank, u64 npair)
{ return 3 + nrank + BLK_PAIR_WORDS*npair; }

static inline u64* exc_off(u64* b) { return b + 2; }
static inline u64* exc_pairs(u64* b) { return b + 3 + b[1]; }
static inline u64 exc_words(u64 nw, u64 npair)
{ return 3 + nw + BLK_PAIR_WORDS*npair; }

/* A worker's persistent state: the level and parent of each vertex it owns,
 * the best proposal seen for each during the level being folded, and the
 * list of vertices that proposal touched.  The scratch arrays are cleared as
 * they are consumed, so no level pays for their extent. */
typedef struct { u64 level; vertexType parent; } vInfo;

static inline vInfo* vis_info(void* p) { return (vInfo*)p; }
static inline vertexType* vis_best(void* p, u64 vSIZE)
{ return (vertexType*)((char*)p + vSIZE*sizeof(vInfo)); }
static inline u64* vis_touch(void* p, u64 vSIZE)
{ return (u64*)((char*)p + vSIZE*(sizeof(vInfo) + sizeof(vertexType))); }
static inline u64 vis_bytes(u64 vSIZE)
{ return vSIZE*(sizeof(vInfo) + sizeof(vertexType) + sizeof(u64)); }

/* ---- per-node chain state --------------------------------------------- */

typedef struct {
  u64 SIZE, R, C, W, vSIZE, EDGE_SIZE;
  u64 nrank, myrank, nroute, nbuild;
  u64 wlo, whi, nw;
  u64 count_total, digest_total;
  ocrGuid_t startEDT, finishEDT, buildHubEDT;
  ocrGuid_t hubTML, workerTML;
  ocrHint_t myHNT;
  /* trailing arrays: sendEVT[nrank], recvEVT[nrank], adj[nw], vis[nw] */
} priv_t;

static inline ocrGuid_t* priv_send(priv_t* v) { return (ocrGuid_t*)(v + 1); }
static inline ocrGuid_t* priv_recv(priv_t* v) { return priv_send(v) + v->nrank; }
static inline ocrGuid_t* priv_adj(priv_t* v) { return priv_recv(v) + v->nrank; }
static inline ocrGuid_t* priv_vis(priv_t* v) { return priv_adj(v) + v->nw; }
static inline u64 priv_bytes(u64 nrank, u64 nw)
{ return sizeof(priv_t) + (2*nrank + 2*nw)*sizeof(ocrGuid_t); }

typedef struct {
  u64 SIZE, R, C, W, vSIZE, EDGE_SIZE;
  u64 nrank, nroute, nbuild;
  ocrGuid_t channelRange;
  ocrGuid_t startEDT, finishEDT;
} shared_t;

/* ---- forward declarations --------------------------------------------- */

static ocrGuid_t rankInitEdt(u32, u64*, u32, ocrEdtDep_t*);
static ocrGuid_t chanInitEdt(u32, u64*, u32, ocrEdtDep_t*);
static ocrGuid_t genEdt(u32, u64*, u32, ocrEdtDep_t*);
static ocrGuid_t routeEdt(u32, u64*, u32, ocrEdtDep_t*);
static ocrGuid_t buildHubEdt(u32, u64*, u32, ocrEdtDep_t*);
static ocrGuid_t buildEdt(u32, u64*, u32, ocrEdtDep_t*);
static ocrGuid_t buildJoinEdt(u32, u64*, u32, ocrEdtDep_t*);
static ocrGuid_t startEdt(u32, u64*, u32, ocrEdtDep_t*);
static ocrGuid_t hubEdt(u32, u64*, u32, ocrEdtDep_t*);
static ocrGuid_t workerEdt(u32, u64*, u32, ocrEdtDep_t*);
static ocrGuid_t packEdt(u32, u64*, u32, ocrEdtDep_t*);
static ocrGuid_t finishEdt(u32, u64*, u32, ocrEdtDep_t*);
static ocrGuid_t finalShutdownEdt(u32, u64*, u32, ocrEdtDep_t*);

static void hint_at(ocrHint_t* hnt, u64 rank)
{
  ocrGuid_t affinity;
  ocrHintInit(hnt, OCR_HINT_EDT_T);
  ocrAffinityGetAt(AFFINITY_PD, rank, &affinity);
  ocrSetHintValue(hnt, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(affinity));
}

/* ---- construction ------------------------------------------------------ */

/* Draws one contiguous slice of the edge stream and buckets what it draws by
 * the node that owns each entry's source vertex, which is the node whose
 * adjacency the entry belongs to.  Both directions of an undirected edge are
 * emitted; a self-loop is not an edge and is skipped, exactly as the stream's
 * consumer defines it.
 *
 * paramv: globals, slice index, first route task slot, route group base,
 *         build-hub slot.  depv: 0 jump table, 1 route directory. */
enum { GN_SLICE = GP_N, GN_RSLOT, GN_GROUP, GN_HSLOT, GN_NPARAM };

static ocrGuid_t genEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  u64 SIZE = paramv[GP_SIZE], R = paramv[GP_R], C = paramv[GP_C];
  u64 W = paramv[GP_W], vSIZE = paramv[GP_VSIZE];
  u64 EDGE_SIZE = paramv[GP_ESIZE], nrank = paramv[GP_NRANK];
  u64 slice = paramv[GN_SLICE];
  const u64* tbl = (const u64*)depv[0].ptr;
  const ocrGuid_t* dir = (const ocrGuid_t*)depv[1].ptr;
  /* A balanced split of the stream that never forms the product of an index
   * with the stream length. */
  u64 per = EDGE_SIZE/W, rem = EDGE_SIZE%W;
  u64 i0 = per*slice + (slice < rem ? slice : rem);
  u64 i1 = i0 + per + (slice < rem ? 1 : 0);
  u64 i, q, npair = 0;
  u64 x0 = prng_jump(tbl, GRAPH_SEED, 2*i0);
  u64 x;
  vmap_t vm;
  ocrGuid_t blk;
  u64* b;
  u64* off;
  u64* pairs;
  u64* cur;

  /* Two passes over the slice, as the counting sort needs: one to size each
   * destination, one to place.  Sizing the destinations also sizes the
   * block, so the stream is never drawn a third time. */
  vmap_init(&vm, R, C, vSIZE, W, nrank);
  cur = (u64*)malloc(nrank*sizeof(u64));
  if (cur == NULL) { ocrPrintf("ERROR: out of memory in edge generation\n"); ocrAbort(1); }
  for (q = 0; q < nrank; ++q) cur[q] = 0;
  x = x0;
  for (i = i0; i < i1; ++i) {
    u64 s = xorshift64star(&x) % SIZE;
    u64 d = xorshift64star(&x) % SIZE;
    if (s == d) continue;
    cur[vmap_rank(&vm, s)]++;
    cur[vmap_rank(&vm, d)]++;
    npair += 2;
  }

  ocrDbCreate(&blk, (void**)&b, gen_words(nrank, npair)*sizeof(u64),
              DB_PROP_NONE, NULL_HINT, NO_ALLOC);
  off = gen_off(b);
  pairs = gen_pairs(b, nrank);
  off[0] = 0;
  for (q = 0; q < nrank; ++q) off[q+1] = off[q] + cur[q];
  for (q = 0; q < nrank; ++q) cur[q] = off[q];

  x = x0;
  for (i = i0; i < i1; ++i) {
    u64 s = xorshift64star(&x) % SIZE;
    u64 d = xorshift64star(&x) % SIZE;
    u64 p;
    if (s == d) continue;
    q = vmap_rank(&vm, s);
    p = cur[q]++; pairs[BLK_PAIR_WORDS*p] = s; pairs[BLK_PAIR_WORDS*p + 1] = d;
    q = vmap_rank(&vm, d);
    p = cur[q]++; pairs[BLK_PAIR_WORDS*p] = d; pairs[BLK_PAIR_WORDS*p + 1] = s;
  }
  free(cur);
  ocrDbRelease(blk);

  /* The slice is read by one routing task per destination node and, once
   * routing has consumed it, reclaimed by the node's construction hub. */
  for (q = 0; q < nrank; ++q)
    ocrAddDependence(blk, dir[paramv[GN_GROUP]*nrank + q],
                     (u32)paramv[GN_RSLOT], DB_MODE_CONST);
  ocrAddDependence(blk, dir[paramv[GP_NROUTE]*nrank],
                   (u32)paramv[GN_HSLOT], DB_MODE_CONST);
  return NULL_GUID;
}

/* Takes what a group of slices drew for one destination node and republishes
 * it as a single block grouped by destination worker.  This is the only
 * reason construction is a two-step exchange: it turns the slice count times
 * the node count into a per-node-pair constant, so no node ever receives one
 * message per slice.
 *
 * paramv: globals, destination node, channel guid.  depv: the group's slices. */
enum { RT_DEST = GP_N, RT_EVT, RT_NPARAM = RT_EVT + GUID_WORDS };

static ocrGuid_t routeEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  u64 R = paramv[GP_R], C = paramv[GP_C], W = paramv[GP_W];
  u64 vSIZE = paramv[GP_VSIZE], nrank = paramv[GP_NRANK];
  u64 dest = paramv[RT_DEST];
  u64 wlo = rank_wlo(dest, W, nrank), whi = rank_wlo(dest + 1, W, nrank);
  u64 nw = whi - wlo;
  u64 npair = 0, i, k;
  u64* off;
  u64* pairs;
  u64* cur;
  ocrGuid_t blk;
  u64* b;
  vmap_t vm;

  vmap_init(&vm, R, C, vSIZE, W, nrank);
  for (i = 0; i < depc; ++i) {
    const u64* s = (const u64*)depv[i].ptr;
    if (s == NULL) continue;
    npair += gen_off((u64*)s)[dest + 1] - gen_off((u64*)s)[dest];
  }

  ocrDbCreate(&blk, (void**)&b, rte_words(nw, npair)*sizeof(u64),
              DB_PROP_NONE, NULL_HINT, NO_ALLOC);
  b[0] = nw;
  off = rte_off(b);
  pairs = rte_pairs(b);
  for (k = 0; k <= nw; ++k) off[k] = 0;

  for (i = 0; i < depc; ++i) {
    const u64* s = (const u64*)depv[i].ptr;
    u64 lo, hi, p;
    if (s == NULL) continue;
    lo = gen_off((u64*)s)[dest]; hi = gen_off((u64*)s)[dest + 1];
    for (p = lo; p < hi; ++p) {
      u64 src = gen_pairs((u64*)s, nrank)[BLK_PAIR_WORDS*p];
      off[vmap_worker(&vm, src) - wlo + 1]++;
    }
  }
  for (k = 0; k < nw; ++k) off[k+1] += off[k];

  cur = (u64*)malloc((nw ? nw : 1)*sizeof(u64));
  if (cur == NULL) { ocrPrintf("ERROR: out of memory in edge routing\n"); ocrAbort(1); }
  for (k = 0; k < nw; ++k) cur[k] = off[k];

  for (i = 0; i < depc; ++i) {
    const u64* s = (const u64*)depv[i].ptr;
    u64 lo, hi, p;
    if (s == NULL) continue;
    lo = gen_off((u64*)s)[dest]; hi = gen_off((u64*)s)[dest + 1];
    for (p = lo; p < hi; ++p) {
      u64 src = gen_pairs((u64*)s, nrank)[BLK_PAIR_WORDS*p];
      u64 dst = gen_pairs((u64*)s, nrank)[BLK_PAIR_WORDS*p + 1];
      u64 t = cur[vmap_worker(&vm, src) - wlo]++;
      pairs[BLK_PAIR_WORDS*t] = src;
      pairs[BLK_PAIR_WORDS*t + 1] = dst;
    }
  }
  free(cur);
  ocrDbRelease(blk);
  ocrEventSatisfy(guid_load(paramv + RT_EVT), blk);
  return NULL_GUID;
}

/* Receives the routed edges addressed to this node and issues the tasks that
 * turn them into adjacency.  It also reclaims the slices, which is safe
 * exactly here: a node's routing has published before this fires, and a
 * slice is read by nothing else.
 *
 * paramv: globals, my node, build task count, jump-table and directory
 * guids.  depv: this node's slices, then the routed blocks. */
enum { BH_RANK = GP_N, BH_NBUILD, BH_JUMP, BH_DIR = BH_JUMP + GUID_WORDS,
       BH_PRIV = BH_DIR + GUID_WORDS, BH_NPARAM = BH_PRIV + GUID_WORDS };
enum { BD_RANK = GP_N, BD_LO, BD_HI, BD_SLOT, BD_JOIN,
       BD_NPARAM = BD_JOIN + GUID_WORDS };

static ocrGuid_t buildHubEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  u64 W = paramv[GP_W], nrank = paramv[GP_NRANK], nroute = paramv[GP_NROUTE];
  u64 myrank = paramv[BH_RANK], nbuild = paramv[BH_NBUILD];
  u64 wlo = rank_wlo(myrank, W, nrank), whi = rank_wlo(myrank + 1, W, nrank);
  u64 nw = whi - wlo;
  u64 nrte = nrank*nroute;
  u64 i, b;
  ocrGuid_t joinTML, joinEDT, buildTML;
  ocrHint_t hnt;
  u64 pv[BD_NPARAM];

  for (i = 0; i < nw; ++i)
    if (!ocrGuidIsNull(depv[i].guid)) {
      ocrDbRelease(depv[i].guid);
      ocrDbDestroy(depv[i].guid);
    }
  ocrDbDestroy(guid_load(paramv + BH_JUMP));
  ocrDbDestroy(guid_load(paramv + BH_DIR));

  hint_at(&hnt, myrank);
  memcpy(pv, paramv, GP_N*sizeof(u64));
  pv[GP_N] = myrank;

  /* The join carries the routed blocks so that it can reclaim them once the
   * adjacency they feed is built. */
  ocrEdtTemplateCreate(&joinTML, buildJoinEdt, GP_N + 1, (u32)(1 + nbuild + nrte));
  ocrEdtCreate(&joinEDT, joinTML, GP_N + 1, pv, (u32)(1 + nbuild + nrte),
               NULL, EDT_PROP_NONE, &hnt, NULL);
  ocrEdtTemplateDestroy(joinTML);
  for (i = 0; i < nrte; ++i)
    ocrAddDependence(depv[nw + i].guid, joinEDT,
                     (u32)(1 + nbuild + i), DB_MODE_CONST);
  ocrAddDependence(guid_load(paramv + BH_PRIV), joinEDT, 0, DB_MODE_RW);

  ocrEdtTemplateCreate(&buildTML, buildEdt, BD_NPARAM, (u32)nrte);
  for (b = 0; b < nbuild; ++b) {
    ocrGuid_t edt;
    pv[BD_LO] = b*nw/nbuild;
    pv[BD_HI] = (b + 1)*nw/nbuild;
    pv[BD_SLOT] = 1 + b;
    guid_store(pv + BD_JOIN, joinEDT);
    ocrEdtCreate(&edt, buildTML, BD_NPARAM, pv, (u32)nrte, NULL,
                 EDT_PROP_NONE, &hnt, NULL);
    for (i = 0; i < nrte; ++i)
      ocrAddDependence(depv[nw + i].guid, edt, (u32)i, DB_MODE_CONST);
  }
  ocrEdtTemplateDestroy(buildTML);
  return NULL_GUID;
}

/* Builds the adjacency and the visit state of a contiguous run of workers.
 * The routed blocks are already grouped by worker, so a worker's edges are
 * one contiguous run in each of them and the build is a counting pass over
 * exactly those.
 *
 * paramv: globals, my node, first worker, last worker, report slot, join
 * task.  depv: routed blocks. */
static ocrGuid_t buildEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  u64 R = paramv[GP_R], W = paramv[GP_W];
  u64 vSIZE = paramv[GP_VSIZE], nrank = paramv[GP_NRANK];
  u64 myrank = paramv[BD_RANK], lo = paramv[BD_LO], hi = paramv[BD_HI];
  u64 wlo = rank_wlo(myrank, W, nrank);
  u64 i, k;
  vmap_t vm;
  ocrGuid_t out;
  void* op;
  u64* on;
  ocrGuid_t* og;

  /* A report is a count and then a pair of block identities per worker; the
   * two have no common type, so the layout names each part rather than
   * assuming an identity is one word. */
  ocrDbCreate(&out, &op, sizeof(u64) + 2*(hi - lo)*sizeof(ocrGuid_t),
              DB_PROP_NONE, NULL_HINT, NO_ALLOC);
  on = (u64*)op;
  og = (ocrGuid_t*)((char*)op + sizeof(u64));
  *on = hi - lo;
  vmap_init(&vm, R, paramv[GP_C], vSIZE, W, nrank);

  for (k = lo; k < hi; ++k) {
    u64 w = wlo + k;
    u64 m = 0, id;
    ocrGuid_t adjDBK, visDBK;
    u64* a;
    u64* aoff;
    u64* aadj;
    u64* cur;
    void* vp;
    vInfo* info;
    vertexType* best;

    for (i = 0; i < depc; ++i) {
      u64* s = (u64*)depv[i].ptr;
      if (s == NULL) continue;
      m += rte_off(s)[k+1] - rte_off(s)[k];
    }

    ocrDbCreate(&adjDBK, (void**)&a, (vSIZE + 1 + m)*sizeof(u64),
                DB_PROP_NONE, NULL_HINT, NO_ALLOC);
    aoff = a; aadj = a + vSIZE + 1;
    for (id = 0; id <= vSIZE; ++id) aoff[id] = 0;
    for (i = 0; i < depc; ++i) {
      u64* s = (u64*)depv[i].ptr;
      u64 p;
      if (s == NULL) continue;
      for (p = rte_off(s)[k]; p < rte_off(s)[k+1]; ++p)
        aoff[vmap_index(&vm, rte_pairs(s)[BLK_PAIR_WORDS*p]) + 1]++;
    }
    for (id = 0; id < vSIZE; ++id) aoff[id+1] += aoff[id];

    cur = (u64*)malloc(vSIZE*sizeof(u64));
    if (cur == NULL) { ocrPrintf("ERROR: out of memory building adjacency\n"); ocrAbort(1); }
    for (id = 0; id < vSIZE; ++id) cur[id] = aoff[id];
    for (i = 0; i < depc; ++i) {
      u64* s = (u64*)depv[i].ptr;
      u64 p;
      if (s == NULL) continue;
      for (p = rte_off(s)[k]; p < rte_off(s)[k+1]; ++p) {
        u64 src = rte_pairs(s)[BLK_PAIR_WORDS*p];
        u64 dst = rte_pairs(s)[BLK_PAIR_WORDS*p + 1];
        aadj[cur[vmap_index(&vm, src)]++] = dst;
      }
    }
    free(cur);
    ocrDbRelease(adjDBK);

    ocrDbCreate(&visDBK, &vp, vis_bytes(vSIZE), DB_PROP_NONE, NULL_HINT, NO_ALLOC);
    info = vis_info(vp);
    best = vis_best(vp, vSIZE);
    for (id = 0; id < vSIZE; ++id) {
      info[id].level = LEVEL_NONE;
      info[id].parent = VERTEX_NONE;
      best[id] = VERTEX_NONE;
    }
    ocrDbRelease(visDBK);

    og[2*(k - lo)] = adjDBK;
    og[2*(k - lo) + 1] = visDBK;
    (void)w;
  }
  ocrDbRelease(out);
  ocrAddDependence(out, guid_load(paramv + BD_JOIN), (u32)paramv[BD_SLOT],
                   DB_MODE_CONST);
  return NULL_GUID;
}

/* Records the built adjacency in the node's chain state, reclaims the routed
 * blocks and reports the node ready.
 * paramv: globals, my node.  depv: 0 chain state, then one report per build
 * task, then the routed blocks. */
static ocrGuid_t buildJoinEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  priv_t* priv = (priv_t*)depv[0].ptr;
  u64 nbuild = priv->nbuild, nrank = priv->nrank, nroute = priv->nroute;
  ocrGuid_t* adj = priv_adj(priv);
  ocrGuid_t* vis = priv_vis(priv);
  ocrGuid_t startEDT = priv->startEDT;
  u64 myrank = priv->myrank;
  u64 b, i, k = 0;
  ocrGuid_t rep;
  ocrGuid_t* rp;

  for (b = 0; b < nbuild; ++b) {
    void* op = depv[1 + b].ptr;
    u64 n = *(u64*)op;
    ocrGuid_t* og = (ocrGuid_t*)((char*)op + sizeof(u64));
    for (i = 0; i < n; ++i) {
      adj[k] = og[2*i];
      vis[k] = og[2*i + 1];
      ++k;
    }
    ocrDbRelease(depv[1 + b].guid);
    ocrDbDestroy(depv[1 + b].guid);
  }
  for (i = 0; i < nrank*nroute; ++i) {
    ocrDbRelease(depv[1 + nbuild + i].guid);
    ocrDbDestroy(depv[1 + nbuild + i].guid);
  }

  ocrDbCreate(&rep, (void**)&rp, sizeof(ocrGuid_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC);
  *rp = depv[0].guid;
  ocrDbRelease(rep);
  ocrDbRelease(depv[0].guid);
  ocrAddDependence(rep, startEDT, (u32)myrank, DB_MODE_CONST);
  return NULL_GUID;
}

/* ---- the search -------------------------------------------------------- */

/* Reports construction complete, starts the search clock and starts every
 * node's chain.
 * paramv: globals, the reporting task, the instance description.  depv: one
 * report per node, then the clock.  Every node has read the description by
 * the time this runs, so this is where it is reclaimed. */
enum { ST_FINISH = GP_N, ST_SHARED = ST_FINISH + GUID_WORDS,
       ST_NPARAM = ST_SHARED + GUID_WORDS };

static ocrGuid_t startEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  u64 nrank = paramv[GP_NRANK];
  struct timeval* pt0 = (struct timeval*)depv[nrank].ptr;
  struct timeval t1;
  double elapsed;
  ocrGuid_t hubTML;
  u64 q;
  u64 pv[GP_N + 2];

  gettimeofday(&t1, 0);
  elapsed = (double)((t1.tv_sec - pt0->tv_sec)*1000000 + t1.tv_usec - pt0->tv_usec);
  ocrPrintf("[kernel1 time %f]\n", elapsed/1000000); FLUSH;
  ocrPrintf("START Kernel 2 (id=0)\n");
  gettimeofday(pt0, 0);

  memcpy(pv, paramv, GP_N*sizeof(u64));
  pv[GP_N + 1] = 0;
  ocrEdtTemplateCreate(&hubTML, hubEdt, GP_N + 2, (u32)(1 + nrank));
  for (q = 0; q < nrank; ++q) {
    ocrGuid_t edt;
    ocrGuid_t privDBK = *(ocrGuid_t*)depv[q].ptr;
    ocrHint_t hnt;
    u64 p;
    hint_at(&hnt, q);
    pv[GP_N] = q;
    ocrEdtCreate(&edt, hubTML, GP_N + 2, pv, (u32)(1 + nrank), NULL,
                 EDT_PROP_NONE, &hnt, NULL);
    for (p = 0; p < nrank; ++p)
      ocrAddDependence(NULL_GUID, edt, (u32)(1 + p), DB_MODE_NULL);
    ocrAddDependence(privDBK, edt, 0, DB_MODE_RW);
    ocrDbRelease(depv[q].guid);
    ocrDbDestroy(depv[q].guid);
  }
  ocrEdtTemplateDestroy(hubTML);

  ocrDbDestroy(guid_load(paramv + ST_SHARED));
  ocrDbRelease(depv[nrank].guid);
  ocrAddDependence(depv[nrank].guid, guid_load(paramv + ST_FINISH),
                   (u32)nrank, DB_MODE_RW);
  return NULL_GUID;
}

/* One level on one node: read what the previous level addressed here, issue a
 * task for each worker that has something to fold, and hand the level to the
 * node's packing task.  A worker with nothing addressed to it cannot discover
 * anything, so it is not issued at all -- which is what makes the issue rate
 * follow the frontier rather than the grid.
 *
 * paramv: globals, my node, level.  depv: 0 chain state, then one block per
 * node (absent at the first level, whose frontier is the root alone). */
enum { HB_RANK = GP_N, HB_LEVEL, HB_NPARAM };

static ocrGuid_t hubEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  priv_t* priv = (priv_t*)depv[0].ptr;
  u64 nrank = priv->nrank, nw = priv->nw, wlo = priv->wlo;
  u64 R = priv->R, C = priv->C, vSIZE = priv->vSIZE;
  u64 level = paramv[HB_LEVEL];
  u64 total = 0;
  u64 p, i, nactive = 0;
  u64 rootWorker = vertex_worker(ROOT_VERTEX, R, C, vSIZE);
  ocrGuid_t packTML, packEDT;
  u64* active;
  u64 pv[GP_N + 3 + GUID_WORDS];

  if (level >= MAX_LEVELS) {
    ocrPrintf("ERROR: BFS level %" PRIu64 " reaches the exchange depth bound"
              " %d.  Raise MAX_LEVELS and rebuild.\n", level, (int)MAX_LEVELS);
    FLUSH;
    ocrAbort(1);
  }

  /* What arrived is the count the previous round discovered, so a round
   * reports the level below it; the first round's frontier is the root and
   * nothing arrived for it. */
  if (level == 0) {
    total = 1;
  } else {
    for (p = 0; p < nrank; ++p) total += ((u64*)depv[1 + p].ptr)[0];
    if (priv->myrank == 0) {
      ocrPrintf("%" PRIu64 ": %" PRIu64 "\n", level - 1, total); FLUSH;
    }
  }

  if (total == 0) {
    ocrGuid_t res, finishEDT = priv->finishEDT;
    u64 myrank = priv->myrank;
    u64* rp;
    ocrDbCreate(&res, (void**)&rp, 2*sizeof(u64), DB_PROP_NONE, NULL_HINT, NO_ALLOC);
    rp[0] = priv->count_total;
    rp[1] = priv->digest_total;
    ocrDbRelease(res);
    for (p = 0; p < nrank; ++p) {
      ocrDbRelease(depv[1 + p].guid);
      ocrDbDestroy(depv[1 + p].guid);
    }
    for (i = 0; i < nw; ++i) {
      ocrDbDestroy(priv_adj(priv)[i]);
      ocrDbDestroy(priv_vis(priv)[i]);
    }
    ocrDbRelease(depv[0].guid);
    ocrDbDestroy(depv[0].guid);
    ocrAddDependence(res, finishEDT, (u32)myrank, DB_MODE_CONST);
    return NULL_GUID;
  }

  /* Which of this node's workers were addressed. */
  active = (u64*)malloc((nw ? nw : 1)*sizeof(u64));
  if (active == NULL) { ocrPrintf("ERROR: out of memory issuing a level\n"); ocrAbort(1); }
  for (i = 0; i < nw; ++i) {
    u64 has = 0;
    if (level == 0) {
      has = (wlo + i == rootWorker);
    } else {
      for (p = 0; p < nrank && !has; ++p) {
        u64* b = (u64*)depv[1 + p].ptr;
        has = exc_off(b)[i+1] > exc_off(b)[i];
      }
    }
    if (has) active[nactive++] = i;
  }

  memcpy(pv, paramv, GP_N*sizeof(u64));
  pv[GP_N] = priv->myrank;
  pv[GP_N + 1] = level;
  pv[GP_N + 2] = nactive;
  ocrEdtTemplateCreate(&packTML, packEdt, GP_N + 3,
                       (u32)(1 + nrank + nactive));
  ocrEdtCreate(&packEDT, packTML, GP_N + 3, pv, (u32)(1 + nrank + nactive),
               NULL, EDT_PROP_NONE, &priv->myHNT, NULL);
  ocrEdtTemplateDestroy(packTML);

  {
    for (i = 0; i < nactive; ++i) {
      u64 k = active[i];
      ocrGuid_t edt;
      pv[GP_N] = wlo + k;
      pv[GP_N + 1] = level;
      pv[GP_N + 2] = 1 + nrank + i;
      guid_store(pv + GP_N + 3, packEDT);
      ocrEdtCreate(&edt, priv->workerTML, GP_N + 3 + (u32)GUID_WORDS, pv,
                   (u32)(nrank + 2), NULL, EDT_PROP_NONE, &priv->myHNT, NULL);
      for (p = 0; p < nrank; ++p)
        ocrAddDependence(level == 0 ? NULL_GUID : depv[1 + p].guid, edt,
                         (u32)p, level == 0 ? DB_MODE_NULL : DB_MODE_CONST);
      ocrAddDependence(priv_vis(priv)[k], edt, (u32)nrank, DB_MODE_RW);
      ocrAddDependence(priv_adj(priv)[k], edt, (u32)(nrank + 1), DB_MODE_CONST);
    }
  }
  free(active);

  for (p = 0; p < nrank; ++p)
    ocrAddDependence(level == 0 ? NULL_GUID : depv[1 + p].guid, packEDT,
                     (u32)(1 + p), level == 0 ? DB_MODE_NULL : DB_MODE_CONST);
  ocrDbRelease(depv[0].guid);
  ocrAddDependence(depv[0].guid, packEDT, 0, DB_MODE_RW);
  return NULL_GUID;
}

/* One worker's share of one level: fold the candidates addressed to it, keep
 * the ones that reach an unvisited vertex, and walk the adjacency of exactly
 * those.  The scratch used by the fold is cleared as it is read back, so the
 * cost is the candidates and not the worker's vertex extent.
 *
 * paramv: globals, worker, level, packing slot, packing task.
 * depv: one block per node, then visit state, then adjacency. */
enum { WK_W = GP_N, WK_LEVEL, WK_SLOT, WK_PACK,
       WK_NPARAM = WK_PACK + GUID_WORDS };

static ocrGuid_t workerEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  u64 R = paramv[GP_R], C = paramv[GP_C], W = paramv[GP_W];
  u64 vSIZE = paramv[GP_VSIZE], nrank = paramv[GP_NRANK];
  u64 w = paramv[WK_W], level = paramv[WK_LEVEL];
  u64 wlo = rank_wlo(worker_rank(w, W, nrank), W, nrank);
  u64 k = w - wlo;
  void* vp = depv[nrank].ptr;
  vInfo* info = vis_info(vp);
  vertexType* best = vis_best(vp, vSIZE);
  u64* touch = vis_touch(vp, vSIZE);
  const u64* a = (const u64*)depv[nrank + 1].ptr;
  const u64* aoff = a;
  const u64* aadj = a + vSIZE + 1;
  u64 nt = 0, digest = 0, npair = 0;
  u64 p, i, q;
  vmap_t vm;
  ocrGuid_t blk;
  u64* b;
  u64* off;
  u64* pairs;
  u64* cur;

  vmap_init(&vm, R, C, vSIZE, W, nrank);
  if (level == 0) {
    u64 id = vmap_index(&vm, ROOT_VERTEX);
    info[id].level = 0;
    info[id].parent = ROOT_VERTEX;
    touch[nt++] = id;
    digest = mixVisited((u64)ROOT_VERTEX, 0, (u64)ROOT_VERTEX);
  } else {
    for (p = 0; p < nrank; ++p) {
      u64* s = (u64*)depv[p].ptr;
      u64 lo, hi, j;
      if (s == NULL) continue;
      lo = exc_off(s)[k]; hi = exc_off(s)[k+1];
      for (j = lo; j < hi; ++j) {
        u64 v = exc_pairs(s)[BLK_PAIR_WORDS*j];
        u64 u = exc_pairs(s)[BLK_PAIR_WORDS*j + 1];
        u64 id = vmap_index(&vm, v);
        if (info[id].level != LEVEL_NONE) continue;
        if (best[id] == VERTEX_NONE) { best[id] = u; touch[nt++] = id; }
        else if (cand_better(u, best[id], R)) best[id] = u;
      }
    }
    for (i = 0; i < nt; ++i) {
      u64 id = touch[i];
      u64 v = worker_vertex(w, id, R, C, vSIZE);
      info[id].level = level;
      info[id].parent = best[id];
      digest += mixVisited(v, level, (u64)best[id]);
      best[id] = VERTEX_NONE;
    }
  }

  for (i = 0; i < nt; ++i) {
    u64 id = touch[i];
    npair += aoff[id+1] - aoff[id];
  }
  ocrDbCreate(&blk, (void**)&b, cnd_words(nrank, npair)*sizeof(u64),
              DB_PROP_NONE, NULL_HINT, NO_ALLOC);
  b[0] = nt;
  b[1] = digest;
  off = cnd_off(b);
  pairs = cnd_pairs(b, nrank);
  for (q = 0; q <= nrank; ++q) off[q] = 0;
  for (i = 0; i < nt; ++i) {
    u64 id = touch[i], e;
    for (e = aoff[id]; e < aoff[id+1]; ++e)
      off[vmap_rank(&vm, aadj[e]) + 1]++;
  }
  for (q = 0; q < nrank; ++q) off[q+1] += off[q];

  cur = (u64*)malloc(nrank*sizeof(u64));
  if (cur == NULL) { ocrPrintf("ERROR: out of memory expanding a frontier\n"); ocrAbort(1); }
  for (q = 0; q < nrank; ++q) cur[q] = off[q];
  for (i = 0; i < nt; ++i) {
    u64 id = touch[i], e;
    u64 u = worker_vertex(w, id, R, C, vSIZE);
    for (e = aoff[id]; e < aoff[id+1]; ++e) {
      u64 v = aadj[e];
      u64 t = cur[vmap_rank(&vm, v)]++;
      pairs[BLK_PAIR_WORDS*t] = v;
      pairs[BLK_PAIR_WORDS*t + 1] = u;
    }
  }
  free(cur);
  ocrDbRelease(blk);
  ocrDbRelease(depv[nrank].guid);
  ocrAddDependence(blk, guid_load(paramv + WK_PACK), (u32)paramv[WK_SLOT],
                   DB_MODE_CONST);
  return NULL_GUID;
}

/* Turns this node's whole level into one block per destination node, so a
 * level costs the square of the node count in messages whatever the frontier
 * looks like, and continues the chain.
 *
 * paramv: globals, my node, level, worker count.  depv: 0 chain state, then
 * the level's incoming blocks (reclaimed here), then the workers' output. */
enum { PK_RANK = GP_N, PK_LEVEL, PK_NACTIVE, PK_NPARAM };

static ocrGuid_t packEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  priv_t* priv = (priv_t*)depv[0].ptr;
  u64 nrank = priv->nrank, R = priv->R, C = priv->C;
  u64 vSIZE = priv->vSIZE, W = priv->W;
  u64 level = paramv[PK_LEVEL], nactive = paramv[PK_NACTIVE];
  u64 base = 1 + nrank;
  u64 count = 0, i, q, p;
  vmap_t vm;
  ocrGuid_t hubEDT;
  u64 pv[GP_N + 2];

  for (i = 0; i < nactive; ++i) {
    u64* s = (u64*)depv[base + i].ptr;
    count += s[0];
    priv->digest_total += s[1];
  }
  priv->count_total += count;

  vmap_init(&vm, R, C, vSIZE, W, nrank);
  for (q = 0; q < nrank; ++q) {
    u64 wlo = rank_wlo(q, W, nrank), whi = rank_wlo(q + 1, W, nrank);
    u64 nw = whi - wlo;
    u64 npair = 0, j;
    ocrGuid_t blk;
    u64* b;
    u64* off;
    u64* pairs;
    u64* cur;

    for (i = 0; i < nactive; ++i) {
      u64* s = (u64*)depv[base + i].ptr;
      npair += cnd_off(s)[q+1] - cnd_off(s)[q];
    }
    ocrDbCreate(&blk, (void**)&b, exc_words(nw, npair)*sizeof(u64),
                DB_PROP_NONE, NULL_HINT, NO_ALLOC);
    b[0] = count;
    b[1] = nw;
    off = exc_off(b);
    pairs = exc_pairs(b);
    for (j = 0; j <= nw; ++j) off[j] = 0;
    for (i = 0; i < nactive; ++i) {
      u64* s = (u64*)depv[base + i].ptr;
      for (j = cnd_off(s)[q]; j < cnd_off(s)[q+1]; ++j)
        off[vmap_worker(&vm, cnd_pairs(s, nrank)[BLK_PAIR_WORDS*j]) - wlo + 1]++;
    }
    for (j = 0; j < nw; ++j) off[j+1] += off[j];

    cur = (u64*)malloc((nw ? nw : 1)*sizeof(u64));
    if (cur == NULL) { ocrPrintf("ERROR: out of memory packing a level\n"); ocrAbort(1); }
    for (j = 0; j < nw; ++j) cur[j] = off[j];
    for (i = 0; i < nactive; ++i) {
      u64* s = (u64*)depv[base + i].ptr;
      for (j = cnd_off(s)[q]; j < cnd_off(s)[q+1]; ++j) {
        u64 v = cnd_pairs(s, nrank)[BLK_PAIR_WORDS*j];
        u64 u = cnd_pairs(s, nrank)[BLK_PAIR_WORDS*j + 1];
        u64 t = cur[vmap_worker(&vm, v) - wlo]++;
        pairs[BLK_PAIR_WORDS*t] = v;
        pairs[BLK_PAIR_WORDS*t + 1] = u;
      }
    }
    free(cur);
    ocrDbRelease(blk);
    ocrEventSatisfy(priv_send(priv)[q], blk);
  }

  for (i = 0; i < nactive; ++i) {
    ocrDbRelease(depv[base + i].guid);
    ocrDbDestroy(depv[base + i].guid);
  }
  if (level > 0)
    for (p = 0; p < nrank; ++p) {
      ocrDbRelease(depv[1 + p].guid);
      ocrDbDestroy(depv[1 + p].guid);
    }

  memcpy(pv, paramv, GP_N*sizeof(u64));
  pv[GP_N] = priv->myrank;
  pv[GP_N + 1] = level + 1;
  ocrEdtCreate(&hubEDT, priv->hubTML, GP_N + 2, pv, (u32)(1 + nrank), NULL,
               EDT_PROP_NONE, &priv->myHNT, NULL);
  for (p = 0; p < nrank; ++p)
    ocrAddDependence(priv_recv(priv)[p], hubEDT, (u32)(1 + p), DB_MODE_CONST);
  ocrDbRelease(depv[0].guid);
  ocrAddDependence(depv[0].guid, hubEDT, 0, DB_MODE_RW);
  return NULL_GUID;
}

/* Sums the per-node results, stops the search clock and reports.
 * depv: one result per node, then the clock. */
static ocrGuid_t finishEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  u64 SIZE = paramv[GP_SIZE], EDGE_SIZE = paramv[GP_ESIZE];
  u64 nrank = paramv[GP_NRANK];
  struct timeval* pt0 = (struct timeval*)depv[nrank].ptr;
  struct timeval t1;
  double etime;
  u64 vertex_count = 0, vertex_digest = 0, q;
  u64 edge_factor, edge_count;

  gettimeofday(&t1, 0);
  etime = (double)((t1.tv_sec - pt0->tv_sec)*1000000 + t1.tv_usec - pt0->tv_usec);
  ocrPrintf("[kernel2 time %f]\n", etime/1000000); FLUSH;

  for (q = 0; q < nrank; ++q) {
    u64* r = (u64*)depv[q].ptr;
    vertex_count += r[0];
    vertex_digest += r[1];
    ocrDbRelease(depv[q].guid);
    ocrDbDestroy(depv[q].guid);
  }
  edge_factor = EDGE_SIZE/SIZE;
  edge_count = vertex_count*edge_factor;

  ocrPrintf("--------------SEARCH %" PRIu64 "\n", (u64)0);
  ocrPrintf("nodes %" PRIu64 " edges %" PRIu64 " edge factor %f root %" PRIu64 "\n",
            vertex_count, edge_count, ((double)edge_count)/vertex_count,
            (u64)ROOT_VERTEX);
  ocrPrintf("BFS_DIGEST %" PRIu64 "\n", vertex_digest); FLUSH;
  ocrPrintf("MTEPS %f\n", ((double)edge_count)/etime);
  ocrPrintf("\n");
  ocrPrintf("mean MTEPS %f\n", ((double)edge_count)/etime);
  ocrDbDestroy(depv[nrank].guid);

  /* Shutdown moves to a dedicated task on this one's output event: this body
   * still owes the release of its acquired dependences, and a mid-body
   * shutdown would truncate that release work out of the measured run. */
  return NULL_GUID;
}

static ocrGuid_t finalShutdownEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  ocrShutdown();
  return NULL_GUID;
}

/* ---- per-node setup ---------------------------------------------------- */

/* paramv: globals, my node.  depv: 0 the shared instance description. */
static ocrGuid_t rankInitEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  const shared_t* sh = (const shared_t*)depv[0].ptr;
  u64 myrank = paramv[GP_N];
  u64 nrank = sh->nrank, W = sh->W, nroute = sh->nroute;
  u64 wlo = rank_wlo(myrank, W, nrank), whi = rank_wlo(myrank + 1, W, nrank);
  u64 nw = whi - wlo;
  u64 chunk = (nw + nroute - 1)/nroute;
  u64 q, r, g;
  priv_t* priv;
  ocrGuid_t privDBK, jumpDBK, dirDBK, chanTML, chanEDT, rteTML, genTML;
  u64* jump;
  ocrGuid_t* dir;
  ocrEventParams_t params;
  u64 pv[PV_MAX(PV_MAX(BH_NPARAM, RT_NPARAM), GN_NPARAM)];

  ocrDbCreate(&privDBK, (void**)&priv, priv_bytes(nrank, nw),
              DB_PROP_NONE, NULL_HINT, NO_ALLOC);
  memset(priv, 0, sizeof(priv_t));
  priv->SIZE = sh->SIZE; priv->R = sh->R; priv->C = sh->C; priv->W = W;
  priv->vSIZE = sh->vSIZE; priv->EDGE_SIZE = sh->EDGE_SIZE;
  priv->nrank = nrank; priv->myrank = myrank;
  priv->nroute = nroute; priv->nbuild = sh->nbuild;
  priv->wlo = wlo; priv->whi = whi; priv->nw = nw;
  priv->count_total = 0; priv->digest_total = 0;
  priv->startEDT = sh->startEDT; priv->finishEDT = sh->finishEDT;
  hint_at(&priv->myHNT, myrank);
  ocrEdtTemplateCreate(&priv->hubTML, hubEdt, GP_N + 2, (u32)(1 + nrank));
  ocrEdtTemplateCreate(&priv->workerTML, workerEdt,
                       GP_N + 3 + (u32)GUID_WORDS, (u32)(nrank + 2));

  ocrDbCreate(&jumpDBK, (void**)&jump, JUMP_TABLE_WORDS*sizeof(u64),
              DB_PROP_NONE, NULL_HINT, NO_ALLOC);
  jump_table_build(jump);
  ocrDbRelease(jumpDBK);

  memcpy(pv, paramv, GP_N*sizeof(u64));

  /* Every node owns one channel per destination and publishes it through a
   * labeled event, which is the only way a consumer can name an event its
   * producer created.  One channel carries every generation the run needs:
   * the routing of construction first, then one per level. */
  params.EVENT_CHANNEL.maxGen = (u32)(nroute + MAX_LEVELS + 1);
  params.EVENT_CHANNEL.nbSat = 1;
  params.EVENT_CHANNEL.nbDeps = 1;
  for (q = 0; q < nrank; ++q) {
    ocrEventCreateParams(&priv_send(priv)[q], OCR_EVENT_CHANNEL_T, false, &params);
    priv_recv(priv)[q] = NULL_GUID;
  }
  priv_recv(priv)[myrank] = priv_send(priv)[myrank];

  ocrEdtTemplateCreate(&chanTML, chanInitEdt, GP_N + 1, (u32)(1 + nrank));
  pv[GP_N] = myrank;
  ocrEdtCreate(&chanEDT, chanTML, GP_N + 1, pv, (u32)(1 + nrank), NULL,
               EDT_PROP_NONE, &priv->myHNT, NULL);
  ocrEdtTemplateDestroy(chanTML);
  ocrAddDependence(NULL_GUID, chanEDT, (u32)(1 + myrank), DB_MODE_NULL);
  for (q = 0; q < nrank; ++q) {
    ocrGuid_t stickyEVT, holder;
    ocrGuid_t* slot;
    if (q == myrank) continue;
    ocrDbCreate(&holder, (void**)&slot, sizeof(ocrGuid_t), DB_PROP_NONE,
                NULL_HINT, NO_ALLOC);
    *slot = priv_send(priv)[q];
    ocrDbRelease(holder);
    ocrGuidFromIndex(&stickyEVT, sh->channelRange, myrank*nrank + q);
    ocrEventCreate(&stickyEVT, OCR_EVENT_STICKY_T,
                   GUID_PROP_CHECK | EVT_PROP_TAKES_ARG);
    ocrEventSatisfy(stickyEVT, holder);

    ocrGuidFromIndex(&stickyEVT, sh->channelRange, q*nrank + myrank);
    ocrEventCreate(&stickyEVT, OCR_EVENT_STICKY_T,
                   GUID_PROP_CHECK | EVT_PROP_TAKES_ARG);
    ocrAddDependence(stickyEVT, chanEDT, (u32)(1 + q), DB_MODE_CONST);
  }

  /* The construction hub is created before the tasks that feed it, so a
   * generator can name it; its incoming channel slots are wired once the
   * channels themselves are known. */
  {
    ocrGuid_t hubTMLc;
    ocrDbCreate(&dirDBK, (void**)&dir, (nroute*nrank + 1)*sizeof(ocrGuid_t),
                DB_PROP_NONE, NULL_HINT, NO_ALLOC);
    pv[BH_RANK] = myrank;
    pv[BH_NBUILD] = sh->nbuild;
    guid_store(pv + BH_JUMP, jumpDBK);
    guid_store(pv + BH_DIR, dirDBK);
    guid_store(pv + BH_PRIV, privDBK);
    ocrEdtTemplateCreate(&hubTMLc, buildHubEdt, BH_NPARAM,
                         (u32)(nw + nrank*nroute));
    ocrEdtCreate(&priv->buildHubEDT, hubTMLc, BH_NPARAM, pv,
                 (u32)(nw + nrank*nroute), NULL, EDT_PROP_NONE,
                 &priv->myHNT, NULL);
    ocrEdtTemplateDestroy(hubTMLc);
    dir[nroute*nrank] = priv->buildHubEDT;
  }

  ocrEdtTemplateCreate(&rteTML, routeEdt, RT_NPARAM, (u32)chunk);
  for (r = 0; r < nroute; ++r) {
    u64 have = (r*chunk < nw) ? (nw - r*chunk < chunk ? nw - r*chunk : chunk) : 0;
    for (q = 0; q < nrank; ++q) {
      ocrGuid_t edt;
      u64 s;
      memcpy(pv, paramv, GP_N*sizeof(u64));
      pv[RT_DEST] = q;
      guid_store(pv + RT_EVT, priv_send(priv)[q]);
      ocrEdtCreate(&edt, rteTML, RT_NPARAM, pv, (u32)chunk, NULL,
                   EDT_PROP_NONE, &priv->myHNT, NULL);
      dir[r*nrank + q] = edt;
      for (s = have; s < chunk; ++s)
        ocrAddDependence(NULL_GUID, edt, (u32)s, DB_MODE_NULL);
    }
  }
  ocrEdtTemplateDestroy(rteTML);
  ocrDbRelease(dirDBK);

  memcpy(pv, paramv, GP_N*sizeof(u64));
  ocrEdtTemplateCreate(&genTML, genEdt, GN_NPARAM, 2);
  for (g = 0; g < nw; ++g) {
    ocrGuid_t edt;
    pv[GN_SLICE] = wlo + g;
    pv[GN_GROUP] = g/chunk;
    pv[GN_RSLOT] = g % chunk;
    pv[GN_HSLOT] = g;
    ocrEdtCreate(&edt, genTML, GN_NPARAM, pv, 2, NULL, EDT_PROP_NONE,
                 &priv->myHNT, NULL);
    ocrAddDependence(jumpDBK, edt, 0, DB_MODE_CONST);
    ocrAddDependence(dirDBK, edt, 1, DB_MODE_CONST);
  }
  ocrEdtTemplateDestroy(genTML);

  ocrDbRelease(privDBK);
  ocrAddDependence(privDBK, chanEDT, 0, DB_MODE_RW);
  return NULL_GUID;
}

/* Records this node's incoming channels and registers the construction hub
 * on them.  Registration order along a channel is what pairs a generation
 * with its consumer, and every later registration is issued by the chain, so
 * construction's generations are the first ones. */
static ocrGuid_t chanInitEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  priv_t* priv = (priv_t*)depv[0].ptr;
  u64 nrank = priv->nrank, nroute = priv->nroute, nw = priv->nw;
  u64 p, r;

  for (p = 0; p < nrank; ++p) {
    if (depv[1 + p].ptr == NULL) continue;
    priv_recv(priv)[p] = *(ocrGuid_t*)depv[1 + p].ptr;
    ocrDbRelease(depv[1 + p].guid);
    ocrDbDestroy(depv[1 + p].guid);
  }
  for (p = 0; p < nrank; ++p)
    for (r = 0; r < nroute; ++r)
      ocrAddDependence(priv_recv(priv)[p], priv->buildHubEDT,
                       (u32)(nw + p*nroute + r), DB_MODE_CONST);

  ocrDbRelease(depv[0].guid);
  return NULL_GUID;
}

/* ---- entry ------------------------------------------------------------- */

/* A count argument is a positive decimal integer and nothing else: a trailing
 * character, an empty string or a value the conversion could not represent
 * all mean the run was asked for something other than what was typed. */
static u64 parseCountArg(const char* s, const char* name)
{
  char* end = NULL;
  long v = 0;
  errno = 0;
  if (s != NULL) v = strtol(s, &end, 10);
  if (s == NULL || end == s || *end != '\0' || errno == ERANGE || v < 1) {
    ocrPrintf("ERROR: %s must be a positive decimal integer (got \"%s\").\n",
              name, s ? s : "(none)"); FLUSH;
    ocrAbort(1);
  }
  return (u64)v;
}

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  u64 argc = ocrGetArgc(depv[0].ptr);
  u64 SCALE, EDGEFACTOR, R, C, SIZE, EDGE_SIZE, W, vSIZE;
  u64 nrank, nroute, nbuild, q;
  shared_t* sh;
  ocrGuid_t sharedDBK, timeDBK, startEDT, finishEDT, finalEDT, finishEVT;
  ocrGuid_t startTML, finishTML, finalTML, initTML;
  ocrHint_t rootHNT;
  struct timeval* pt0;
  u64 pv[PV_MAX(ST_NPARAM, GP_N + 1)];

  if (argc < 5) {
    ocrPrintf("usage: graph500_dist SCALE EDGEFACTOR R C\n"); FLUSH;
    ocrShutdown();
    return NULL_GUID;
  }
  SCALE = parseCountArg(ocrGetArgv(depv[0].ptr, 1), "SCALE");
  EDGEFACTOR = parseCountArg(ocrGetArgv(depv[0].ptr, 2), "EDGEFACTOR");
  R = parseCountArg(ocrGetArgv(depv[0].ptr, 3), "R");
  C = parseCountArg(ocrGetArgv(depv[0].ptr, 4), "C");
  if (SCALE > 62) {
    ocrPrintf("ERROR: SCALE %" PRIu64 " exceeds 62.\n", SCALE); FLUSH;
    ocrAbort(1);
  }
  SIZE = (u64)1 << SCALE;
  if (SIZE % R || SIZE % C || SIZE % (R*C)) {
    ocrPrintf("ERROR: the worker grid %" PRIu64 "x%" PRIu64 " does not divide"
              " the vertex count %" PRIu64 ".\n", R, C, SIZE); FLUSH;
    ocrAbort(1);
  }
  if (EDGEFACTOR > (u64)-1/SIZE) {
    ocrPrintf("ERROR: SIZE*EDGEFACTOR does not fit 64 bits.\n"); FLUSH;
    ocrAbort(1);
  }
  EDGE_SIZE = SIZE*EDGEFACTOR;
  W = R*C;
  vSIZE = SIZE/W;
  if ((u64)ROOT_VERTEX >= SIZE) {
    ocrPrintf("ERROR: root %" PRIu64 " is outside a graph of %" PRIu64
              " vertices.\n", (u64)ROOT_VERTEX, SIZE); FLUSH;
    ocrAbort(1);
  }

  ocrAffinityCount(AFFINITY_PD, &nrank);
  if (W < nrank) {
    ocrPrintf("ERROR: the worker grid %" PRIu64 "x%" PRIu64 " is smaller than"
              " the %" PRIu64 " policy domains it must cover.\n", R, C, nrank);
    FLUSH;
    ocrAbort(1);
  }
  nroute = (ROUTE_TASKS_TARGET + nrank - 1)/nrank;
  if (nroute > W/nrank) nroute = W/nrank;
  if (nroute < 1) nroute = 1;
  nbuild = W/nrank;
  if (nbuild > BUILD_TASKS_MAX) nbuild = BUILD_TASKS_MAX;
  if (nbuild < 1) nbuild = 1;

  ocrDbCreate(&timeDBK, (void**)&pt0, sizeof(struct timeval), DB_PROP_NONE,
              NULL_HINT, NO_ALLOC);
  gettimeofday(pt0, 0);
  ocrDbRelease(timeDBK);

  pv[GP_SIZE] = SIZE; pv[GP_R] = R; pv[GP_C] = C; pv[GP_W] = W;
  pv[GP_VSIZE] = vSIZE; pv[GP_ESIZE] = EDGE_SIZE;
  pv[GP_NRANK] = nrank; pv[GP_NROUTE] = nroute;

  /* The reporting chain is three singleton tasks and both clock readings are
   * taken in it, so it is pinned where the first one was taken. */
  hint_at(&rootHNT, 0);

  ocrEdtTemplateCreate(&finalTML, finalShutdownEdt, 0, 1);
  ocrEdtCreate(&finalEDT, finalTML, 0, NULL, 1, NULL, EDT_PROP_NONE,
               &rootHNT, NULL);
  ocrEdtTemplateDestroy(finalTML);

  ocrEdtTemplateCreate(&finishTML, finishEdt, GP_N, (u32)(nrank + 1));
  ocrEdtCreate(&finishEDT, finishTML, GP_N, pv, (u32)(nrank + 1), NULL,
               EDT_PROP_NONE, &rootHNT, &finishEVT);
  ocrEdtTemplateDestroy(finishTML);
  ocrAddDependence(finishEVT, finalEDT, 0, DB_MODE_NULL);

  ocrDbCreate(&sharedDBK, (void**)&sh, sizeof(shared_t), DB_PROP_NONE,
              NULL_HINT, NO_ALLOC);
  sh->SIZE = SIZE; sh->R = R; sh->C = C; sh->W = W; sh->vSIZE = vSIZE;
  sh->EDGE_SIZE = EDGE_SIZE; sh->nrank = nrank;
  sh->nroute = nroute; sh->nbuild = nbuild;
  sh->finishEDT = finishEDT;
  ocrGuidRangeCreate(&sh->channelRange, nrank*nrank, GUID_USER_EVENT_STICKY);

  guid_store(pv + ST_FINISH, finishEDT);
  guid_store(pv + ST_SHARED, sharedDBK);
  ocrEdtTemplateCreate(&startTML, startEdt, ST_NPARAM, (u32)(nrank + 1));
  ocrEdtCreate(&startEDT, startTML, ST_NPARAM, pv, (u32)(nrank + 1), NULL,
               EDT_PROP_NONE, &rootHNT, NULL);
  ocrEdtTemplateDestroy(startTML);
  ocrAddDependence(timeDBK, startEDT, (u32)nrank, DB_MODE_RW);

  sh->startEDT = startEDT;
  ocrDbRelease(sharedDBK);

  ocrEdtTemplateCreate(&initTML, rankInitEdt, GP_N + 1, 1);
  for (q = 0; q < nrank; ++q) {
    ocrGuid_t edt;
    ocrHint_t hnt;
    hint_at(&hnt, q);
    pv[GP_N] = q;
    ocrEdtCreate(&edt, initTML, GP_N + 1, pv, 1, NULL, EDT_PROP_NONE,
                 &hnt, NULL);
    ocrAddDependence(sharedDBK, edt, 0, DB_MODE_CONST);
  }
  ocrEdtTemplateDestroy(initTML);

  ocrDbDestroy(depv[0].guid);
  return NULL_GUID;
}
