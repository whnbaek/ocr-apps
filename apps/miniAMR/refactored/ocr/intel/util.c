// ************************************************************************
//
// miniAMR: stencil computations with boundary exchange and AMR.
//
// Copyright (2014) Sandia Corporation. Under the terms of Contract
// DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government
// retains certain rights in this software.
//
// Portions Copyright (2016) Intel Corporation.
//
// This library is free software; you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as
// published by the Free Software Foundation; either version 2.1 of the
// License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
// Questions? Contact Courtenay T. Vaughan (ctvaugh@sandia.gov)
//                    Richard F. Barrett (rfbarre@sandia.gov)
//
// ************************************************************************


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <proto.h>
#include <ocr.h>
#include <ocr-guid-functions.h>
#include <extensions/ocr-labeling.h>
#include <extensions/ocr-affinity.h>
//#include <mpi.h>

#ifdef NANNY_FUNC_NAMES
#line __LINE__ "util   "
#endif

double wtime();

double timer(void)
{
   return(wtime());
}


// Static block-to-policy-domain placement.
//
// A block is identified by its integer grid position (x,y,z) at a refinement
// `level`, within a base grid of npx*npy*npz unrefined blocks.  We map it to a
// policy domain by a Cartesian partition of the domain across the available
// PDs.  The map is normalized by the level extent (npx<<level), so it is
// level-consistent: a predecessor computing a successor's PD at fork time and
// the successor recomputing its own PD from its stored coordinates agree.
// Consequently a block keeps a single home PD across its whole continuation
// chain, and spatially adjacent blocks (regardless of level) tend to co-locate,
// while distinct spatial regions distribute across PDs.

#ifdef OCR_APP_OPTIMIZED_PLACEMENT

// Factor P into a 3-D grid (px*py*pz == P) as close to cubic as possible.
static void amr_factor3(int P, int * px, int * py, int * pz) {
   int z = 1, i;
   for (i = 1; (long) i * i * i <= (long) P; i++) if (P % i == 0) z = i;   // largest divisor <= cbrt(P)
   int rem = P / z, y = 1;
   for (i = 1; (long) i * i <= (long) rem; i++) if (rem % i == 0) y = i;   // largest divisor <= sqrt(rem)
   *pz = z; *py = y; *px = rem / y;
}

int amrNumPolicyDomains(void) {
   // The domain count is fixed for the run; resolving it per EDT creation is
   // pure overhead in a program that creates tens of millions of them.  The
   // cache is written concurrently by every worker, so it is a single word and
   // a racing writer stores the same value it read.
   static _Atomic int cached = 0;
   int n = atomic_load_explicit(&cached, memory_order_relaxed);
   if (n == 0) {
      u64 count = 1;
      ocrAffinityCount(AFFINITY_PD, &count);
      n = (count < 1) ? 1 : (int) count;
      atomic_store_explicit(&cached, n, memory_order_relaxed);
   }
   return n;
}

#define AMR_PD_FACTOR_BITS 10
#define AMR_PD_FACTOR_MAX  (1 << AMR_PD_FACTOR_BITS)

int amrBlockHomePD(int x, int y, int z, int level, int npx, int npy, int npz) {
   int P = amrNumPolicyDomains();
   if (P <= 1) return 0;
   // The factorization depends only on the domain count.  Every worker shares
   // the cache, so the three factors are published as one word: a reader that
   // saw a half-written triple would place against a mixture of two grids.
   static _Atomic unsigned int packedPD = 0;
   int PDx, PDy, PDz;
   unsigned int packed = atomic_load_explicit(&packedPD, memory_order_relaxed);
   if (packed != 0) {
      PDx = (int) (packed >> (2 * AMR_PD_FACTOR_BITS));
      PDy = (int) ((packed >> AMR_PD_FACTOR_BITS) & (AMR_PD_FACTOR_MAX - 1));
      PDz = (int) (packed & (AMR_PD_FACTOR_MAX - 1));
   } else {
      amr_factor3(P, &PDx, &PDy, &PDz);
      if (PDx < AMR_PD_FACTOR_MAX && PDy < AMR_PD_FACTOR_MAX && PDz < AMR_PD_FACTOR_MAX) {
         packed = (((unsigned int) PDx) << (2 * AMR_PD_FACTOR_BITS)) |
                  (((unsigned int) PDy) << AMR_PD_FACTOR_BITS) |
                   ((unsigned int) PDz);
         atomic_store_explicit(&packedPD, packed, memory_order_relaxed);
      }
   }
   int ex = npx << level, ey = npy << level, ez = npz << level;
   int px = (ex > 0) ? (int) (((long) x * PDx) / ex) : 0;
   int py = (ey > 0) ? (int) (((long) y * PDy) / ey) : 0;
   int pz = (ez > 0) ? (int) (((long) z * PDz) / ez) : 0;
   if (px < 0) px = 0; else if (px >= PDx) px = PDx - 1;
   if (py < 0) py = 0; else if (py >= PDy) py = PDy - 1;
   if (pz < 0) pz = 0; else if (pz >= PDz) pz = PDz - 1;
   return PDx * PDy * pz + PDx * py + px;
}

#else  // OCR_APP_OPTIMIZED_PLACEMENT

int amrNumPolicyDomains(void) { return 1; }
int amrBlockHomePD(int x, int y, int z, int level, int npx, int npy, int npz) {
   (void)x; (void)y; (void)z; (void)level; (void)npx; (void)npy; (void)npz;
   return 0;
}

#endif // OCR_APP_OPTIMIZED_PLACEMENT

ocrHint_t * amrEdtHintForPD(ocrHint_t * hint, int pd) {
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
   // One hint per domain, built once and thereafter shared.  A domain's
   // affinity does not change, and a program that creates tens of millions of
   // EDTs cannot afford a runtime query plus a hint rebuild at every one.
   // A slot is claimed by one filler and published only when complete; a
   // concurrent reader that finds it unpublished builds into the caller's own
   // hint rather than reading a half-built one.
   #define AMR_MAX_CACHED_PD 256
   #define AMR_PD_SLOT_EMPTY     0
   #define AMR_PD_SLOT_FILLING   1
   #define AMR_PD_SLOT_PUBLISHED 2
   static ocrHint_t    cache[AMR_MAX_CACHED_PD];
   static _Atomic char cacheState[AMR_MAX_CACHED_PD];
   ocrGuid_t aff;
   int owned = 0;
   if (pd >= 0 && pd < AMR_MAX_CACHED_PD) {
      char state = atomic_load_explicit(&cacheState[pd], memory_order_acquire);
      if (state == AMR_PD_SLOT_PUBLISHED) return &cache[pd];
      if (state == AMR_PD_SLOT_EMPTY) {
         char expected = AMR_PD_SLOT_EMPTY;
         owned = atomic_compare_exchange_strong_explicit(&cacheState[pd], &expected,
                                                         AMR_PD_SLOT_FILLING,
                                                         memory_order_acq_rel,
                                                         memory_order_relaxed);
      }
   }
   // An index the runtime cannot resolve leaves the affinity unwritten, so it
   // must degrade to "no preference" rather than to an indeterminate hint.
   if (ocrAffinityGetAt(AFFINITY_PD, (u64) pd, &aff) != 0) {
      if (owned) atomic_store_explicit(&cacheState[pd], AMR_PD_SLOT_EMPTY, memory_order_release);
      return NULL_HINT;
   }
   ocrHint_t * built = owned ? &cache[pd] : hint;
   ocrHintInit(built, OCR_HINT_EDT_T);
   ocrSetHintValue(built, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
   if (owned) atomic_store_explicit(&cacheState[pd], AMR_PD_SLOT_PUBLISHED, memory_order_release);
   return built;
#else
   (void)hint; (void)pd;
   return NULL_HINT;
#endif
}

ocrHint_t * amrEdtHintForBlock(ocrHint_t * hint, int x, int y, int z, int level, int npx, int npy, int npz) {
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
   return amrEdtHintForPD(hint, amrBlockHomePD(x, y, z, level, npx, npy, npz));
#else
   (void)hint; (void)x; (void)y; (void)z; (void)level; (void)npx; (void)npy; (void)npz;
   return NULL_HINT;
#endif
}


// A halo channel's first exchange goes through a label: one end block's inbound or outbound label for its channel in direction
// (axis, pole) and quarter qrtr (non-zero only toward a finer neighbor), in the refinement round that established the channel.
// Round r is the refine pass at timestep r*refine_freq (round 0, the initial mesh) and the timesteps before the next pass.

#define HALO_LABELS_PER_BLOCK 48

int haloRoundOf(Control_t * control, int ts) {
   return (control->num_refine && !control->uniform_refine) ? ts / control->refine_freq : 0;
}

int haloRoundCount(Control_t * control) {
   return haloRoundOf(control, control->num_tsteps) + 1;
}

unsigned long long haloLabelCount(Control_t * control) {
   unsigned long long blocks = (011111111111111111LL & ((1LL << ((control->num_refine+1) * 3LL)) - 1LL)) * control->npx * control->npy * control->npz;
   return blocks * HALO_LABELS_PER_BLOCK * haloRoundCount(control);
}

ocrGuid_t haloLabel(ocrGuid_t range, Control_t * control, int round, int level, int x, int y, int z, int axis, int pole, int qrtr, int outbound) {
   unsigned long long base   = control->npx * control->npy * control->npz;
   unsigned long long blocks = (011111111111111111LL & ((1LL << ((control->num_refine+1) * 3LL)) - 1LL)) * base;
   unsigned long long block  = (0111111111111111111111L >> (63L - (level*3))) * base +
                               ((((unsigned long long) x * (control->npy << level)) + y) * (control->npz << level)) + z;
   ocrGuid_t label;
   ocrGuidFromIndex(&label, range, ((round * blocks + block) * HALO_LABELS_PER_BLOCK) + ((axis * 2 + pole) * 4 + qrtr) * 2 + outbound);
   return label;
}

// Both labels of one end's channel are created by exactly one task, which precedes every task that names them.
void haloLabelsCreate(ocrGuid_t range, Control_t * control, int round, int level, int x, int y, int z, int axis, int pole, int qrtr) {
   int outbound;
   for (outbound = 0; outbound < 2; outbound++) {
      ocrGuid_t label = haloLabel(range, control, round, level, x, y, z, axis, pole, qrtr, outbound);
      gasket__ocrEventCreate(&label, OCR_EVENT_STICKY_T, GUID_PROP_IS_LABELED | EVT_PROP_TAKES_ARG, __FILE__, __func__, __LINE__, NULL, "haloLabel");
   }
}

// Destroys a pair its creator learned no channel will use.
void haloLabelsDestroy(ocrGuid_t range, Control_t * control, int round, int level, int x, int y, int z, int axis, int pole, int qrtr) {
   int outbound;
   for (outbound = 0; outbound < 2; outbound++) {
      ocrGuid_t label = haloLabel(range, control, round, level, x, y, z, axis, pole, qrtr, outbound);
      gasket__ocrEventAbandon(&label, __FILE__, __func__, __LINE__, NULL, "haloLabel");
   }
}

// Opens a channel on its labels.  The receiver destroys a label it received on; a label never sent on is destroyed by its sender, the one
// end that knows it was not sent.
void haloChannelOpen(BlockMeta_t * meta, int axis, int pole, int qrtr, ocrGuid_t send, ocrGuid_t receive) {
   meta->conveyFaceToNeighbor_Event[axis][pole][qrtr] = send;
   meta->onDeckToReceiveFace_Event [axis][pole][qrtr] = receive;
   meta->conveyLabelUnsent |= HALO_SLOT_BIT(axis, pole, qrtr);
}

// The channel's labels are this block's own.
void haloChannelOpenOwn(BlockMeta_t * meta, Control_t * control, int round, int axis, int pole, int qrtr) {
   ocrGuid_t range = meta->labeledGuidRangeForHaloExchange;
   haloChannelOpen(meta, axis, pole, qrtr,
                   haloLabel(range, control, round, meta->refinementLevel, meta->xPos, meta->yPos, meta->zPos, axis, pole, qrtr, 1),
                   haloLabel(range, control, round, meta->refinementLevel, meta->xPos, meta->yPos, meta->zPos, axis, pole, qrtr, 0));
}

// The channel's labels are those of the peer block (level, x, y, z) for its channel in direction (axis, peerPole), quarter peerQrtr.
void haloChannelOpenPeer(BlockMeta_t * meta, Control_t * control, int round, int axis, int pole, int qrtr,
                         int level, int x, int y, int z, int peerPole, int peerQrtr) {
   ocrGuid_t range = meta->labeledGuidRangeForHaloExchange;
   haloChannelOpen(meta, axis, pole, qrtr,
                   haloLabel(range, control, round, level, x, y, z, axis, peerPole, peerQrtr, 0),
                   haloLabel(range, control, round, level, x, y, z, axis, peerPole, peerQrtr, 1));
}

void haloConveySent(BlockMeta_t * meta, int axis, int pole, int qrtr) {
   meta->conveyLabelUnsent &= ~HALO_SLOT_BIT(axis, pole, qrtr);
}

// Forgets the outgoing slot; a label in it that was never sent on is destroyed.
void haloConveyFlush(BlockMeta_t * meta, int axis, int pole, int qrtr, const char * name) {
   if (meta->conveyLabelUnsent & HALO_SLOT_BIT(axis, pole, qrtr)) {
      gasket__ocrEventAbandon(&meta->conveyFaceToNeighbor_Event[axis][pole][qrtr], __FILE__, __func__, __LINE__, NULL, name);
   } else {
      gasket__ocrEventFlush  (&meta->conveyFaceToNeighbor_Event[axis][pole][qrtr], __FILE__, __func__, __LINE__, NULL, name);
   }
   haloConveySent(meta, axis, pole, qrtr);
}


// Gaskets for OCR service calls


u8 gasket__ocrEdtCreate(ocrGuid_t * guid, int whoAmI_slot, ocrGuid_t templateGuid,
                        u32 paramc, u64* paramv, u32 depc, ocrGuid_t *depv,
                        u16 properties, ocrHint_t *affinity, ocrGuid_t *outputEvent,
                        char       * file,     // File of calling site.
                        const char * func,     // Function of calling site.
                        int          line,     // Line number of calling site.
                        const char * detail,   // Context of the calling site.
                        const char * name) {   // Name of the datablock being created.
   u8 returnVal = ocrEdtCreate(guid, templateGuid, paramc, paramv, depc, depv, properties, affinity, outputEvent);
   void * whoAmI;
   ocrGuid_t whoAmI_guid;
   ocrDbCreate(&whoAmI_guid, &whoAmI, sizeof(ocrGuid_t), 0, NULL_HINT, NO_ALLOC);
   *((ocrGuid_t *) whoAmI) = *guid;
   ocrDbRelease(whoAmI_guid);
   ocrAddDependence(whoAmI_guid, *guid, whoAmI_slot, DB_MODE_RO);

#ifdef NANNY_ON_STEROIDS
   printf ("grep -E \"^NOS:EDT:"GUIDF"\" df > XXEDT_"GUIDF"; grep -E -v \"^NOS:EDT:"GUIDF"\" df > dfTemp; mv dfTemp df\n", GUIDA(*guid), GUIDA(*guid), GUIDA(*guid)); fflush(stdout);
   printf ("NOS:EDT:"GUIDF" created   EDT of %s  at %20s line %d in %s context: %s\n", GUIDA(*guid), name, func, line, file, detail); fflush(stdout);
#endif
   return returnVal;
} // gasket__ocrEdtCreate



void gasket__ocrDbCreate (ocrGuid_t  * guid,     // Guid of created datablock.
                          void      ** addr,     // Address of created datablock.  (Set to NULL if RELAX_DATABLOCK_SEASONING is NOT set, forcing caller to "season" by cloning the EDT.
                          int          size,     // Size of requested datablock in bytes.
                          char       * file,     // File of calling site.
                          const char * func,     // Function of calling site.
                          int          line,     // Line number of calling site.
                          const char * detail,   // Context of the calling site.
                          const char * name) {   // Name of the datablock being created.

   // Home is left policy-selected: a datablock created here is acquired RW by
   // the creating chain's own next task, so the payload moves once whatever the
   // home is, and the home is only a directory entry.
   ocrDbCreate (guid, addr, size, 0, NULL_HINT, NO_ALLOC);

#ifdef NANNY_ON_STEROIDS
   printf ("grep -E \"^NOS:DBK:"GUIDF"\" df > XXDBK_"GUIDF"; grep -E -v \"^NOS:DBK:"GUIDF"\" df > dfTemp; mv dfTemp df\n", GUIDA(*guid), GUIDA(*guid), GUIDA(*guid)); fflush(stdout);
   printf ("NOS:DBK:"GUIDF" created   DBK of %s  at %20s line %d in %s, addr = %p, size = %d / 0x%x, context: %s\n", GUIDA(*guid), name, func, line, file, *addr, size, size, detail); fflush(stdout);
#endif

//printf ("In %s, called from %s in %s line %d, Allocation size = %d, address is at %p, guid = 0x%lx\n", __func__, func, file, line, size, *block_base, (unsigned long) (*block_dblk));
#ifndef RELAX_DATABLOCK_SEASONING
   *addr = NULL;              // We have to "season" datablocks by passing them to a continuation clone.  NULLify the pointer.  It will become valid in the clone.
#endif
} // gasket__ocrDbCreate


u8 gasket__ocrEventCreate(ocrGuid_t *guid, ocrEventTypes_t eventType, u16 properties,
                        char       * file,        // File of calling site.
                        const char * func,        // Function of calling site.
                        int          line,        // Line number of calling site.
                        const char * detail,      // Context of the calling site.
                        const char * name) {      // Name of the datablock being destroyed.
   u8 returnVal = ocrEventCreate(guid, eventType, properties);
#ifdef NANNY_ON_STEROIDS
   printf ("if [ ! -e \"XXEVT_"GUIDF"\" ]  # grep -E NOS:EVT:"GUIDF"\nthen # grep -E NOS:EVT:"GUIDF"\ngrep -E \"^NOS:EVT:"GUIDF"\" df > XXEVT_"GUIDF"; grep -E -v \"^NOS:EVT:"GUIDF"\" df > dfTemp; mv dfTemp df\nfi  # grep -E NOS:EVT:"GUIDF"\n", GUIDA(*guid), GUIDA(*guid), GUIDA(*guid), GUIDA(*guid), GUIDA(*guid), GUIDA(*guid), GUIDA(*guid)); fflush(stdout);
   printf ("NOS:EVT:"GUIDF" created   EVT of %s  at %20s line %d in %s, context: %s\n", GUIDA(*guid), name, func, line, file, detail); fflush(stdout);
#endif
   return returnVal;
} // gasket__ocrEventCreate


void gasket__ocrGuidFromIndex (ocrGuid_t  * guid,     // Guid of the event returned.
                               ocrGuid_t    range,    // Guid of the range.
                               unsigned long index,   // Index into the range
                               char       * file,     // File of calling site.
                               const char * func,     // Function of calling site.
                               int          line,     // Line number of calling site.
                               const char * detail,   // Context of the calling site.
                               const char * name) {   // Name of the datablock brought in by the event being destroyed.
   ocrGuidFromIndex(guid, range, index);
#ifdef NANNY_ON_STEROIDS
   printf ("if [ ! -e \"XXEVT_"GUIDF"\" ]  # grep -E NOS:EVT:"GUIDF"\nthen # grep -E NOS:EVT:"GUIDF"\ngrep -E \"^NOS:EVT:"GUIDF"\" df > XXEVT_"GUIDF"; grep -E -v \"^NOS:EVT:"GUIDF"\" df > dfTemp; mv dfTemp df\nfi  # grep -E NOS:EVT:"GUIDF"\n", GUIDA(*guid), GUIDA(*guid), GUIDA(*guid), GUIDA(*guid), GUIDA(*guid), GUIDA(*guid), GUIDA(*guid)); fflush(stdout);
   printf ("NOS:EVT:"GUIDF" looked-up from range "GUIDF" index %ld (0x%lx)   EVT of %s  at %20s line %d in %s, context: %s\n", GUIDA(*guid), GUIDA(range), index, index, name, func, line, file, detail); fflush(stdout);
#endif

} // gasket__ocrGuidFromIndex

u8 gasket__ocrDbRelease(ocrGuid_t guid,
                        char       * file,        // File of calling site.
                        const char * func,        // Function of calling site.
                        int          line,        // Line number of calling site.
                        const char * detail,      // Context of the calling site.
                        const char * name) {      // Name of the datablock being destroyed.

#ifdef NANNY_ON_STEROIDS
   printf ("NOS:DBK:"GUIDF" released  DBK of %s  at %20s line %d in %s, context: %s\n", GUIDA(guid), name, func, line, file, detail); fflush(stdout);
#endif

   u8 returnVal = ocrDbRelease(guid);
   return returnVal;

} // gasket__ocrDbRelease


u8 gasket__ocrAddDependence(ocrGuid_t source, ocrGuid_t destination, u32 slot, ocrDbAccessMode_t mode,
                        char       * file,        // File of calling site.
                        const char * func,        // Function of calling site.
                        int          line,        // Line number of calling site.
                        const char * detail,      // Context of the calling site.
                        const char * name) {      // Name of the datablock being sent to recipient.
   if (ocrGuidIsNull(source)) mode = DB_MODE_NULL;
#ifdef NANNY_ON_STEROIDS
   if (!ocrGuidIsNull(source)) {
      printf ("NOS:DBK:"GUIDF" addDepend DBK of %s  to slot %2d of EDT "GUIDF" provisioning %s access  at %20s line %d in %s, context: %s\n",
              GUIDA(source), name, slot, GUIDA(destination), mode == DB_MODE_RW ? "RW" : mode == DB_MODE_RO ? "RO" : mode == DB_MODE_NULL ? "NULL" : "????", func, line, file, detail); fflush(stdout);
   }
   printf ("NOS:EDT:"GUIDF" slot %d (%s) filled with DBK "GUIDF" for %s access, via addDepend done at %20s line %d in %s, context: %s\n",
          GUIDA(destination), slot, name, GUIDA(source), mode == DB_MODE_RW ? "RW" : mode == DB_MODE_RO ? "RO" : mode == DB_MODE_NULL ? "NULL" : "????", func, line, file, detail); fflush(stdout);
#endif
   u8 returnVal = ocrAddDependence(source, destination, slot, mode);
   return returnVal;
} // gasket__ocrAddDependence


u8 gasket__ocrEvtDependence(ocrGuid_t source, ocrGuid_t destination, u32 slot, ocrDbAccessMode_t mode,
                        char       * file,        // File of calling site.
                        const char * func,        // Function of calling site.
                        int          line,        // Line number of calling site.
                        const char * detail,      // Context of the calling site.
                        const char * name) {      // Name of the datablock being sent to recipient.
   if (ocrGuidIsNull(source)) mode = DB_MODE_NULL;
#ifdef NANNY_ON_STEROIDS
   if (!ocrGuidIsNull(source)) {
      printf ("NOS:EVT:"GUIDF" addDepend via EVT of %s  to slot %2d of EDT "GUIDF" provisioning %s access  at %20s line %d in %s, context: %s\n",
              GUIDA(source), name, slot, GUIDA(destination), mode == DB_MODE_RW ? "RW" : mode == DB_MODE_RO ? "RO" : mode == DB_MODE_NULL ? "NULL" : "????", func, line, file, detail); fflush(stdout);
   }
   printf ("NOS:EDT:"GUIDF" slot %d (%s) filled with EVT "GUIDF" for %s access, via addDepend done at %20s line %d in %s, context: %s\n",
          GUIDA(destination), slot, name, GUIDA(source), mode == DB_MODE_RW ? "RW" : mode == DB_MODE_RO ? "RO" : mode == DB_MODE_NULL ? "NULL" : "????", func, line, file, detail); fflush(stdout);
#endif
   u8 returnVal = ocrAddDependence(source, destination, slot, mode);
   return returnVal;
} // gasket__ocrEvtDependence


u8 gasket__ocrEventSatisfy(ocrGuid_t event, ocrGuid_t source,
                        char       * file,        // File of calling site.
                        const char * func,        // Function of calling site.
                        int          line,        // Line number of calling site.
                        const char * detail,      // Context of the calling site.
                        const char * name) {      // Name of the datablock being sent to recipient.
#ifdef NANNY_ON_STEROIDS
   printf ("NOS:EVT:"GUIDF" is conveying DBK "GUIDF" of %s to its destination EDT  at %20s line %d in %s,  context: %s\n", GUIDA(event), GUIDA(source), name, func, line, file, detail); fflush(stdout);
   printf ("NOS:DBK:"GUIDF" of %s is being carried by EVT "GUIDF" to its destination EDT  at %20s line %d in %s,  context: %s\n", GUIDA(source), name, GUIDA(event), func, line, file, detail); fflush(stdout);
#endif
   return ocrEventSatisfy(event, source);
} // gasket__ocrEventSatisfy

void gasket__InductDependence(ocrEdtDep_t whoAmI, ocrEdtDep_t source, u32 slot,
                        char       * file,        // File of calling site.
                        const char * func,        // Function of calling site.
                        int          line,        // Line number of calling site.
                        const char * detail,      // Context of the calling site.
                        const char * name) {      // Name of the datablock being sent to recipient.
#ifdef NANNY_ON_STEROIDS
   if (!ocrGuidIsNull(source.guid)) {
      printf ("NOS:DBK:"GUIDF" %s received to slot %d of EDT "GUIDF", at %20s line %d in %s, context: %s\n", GUIDA(source.guid), name, slot, GUIDA(*((ocrGuid_t *) (whoAmI.ptr))), func, line, file, detail); fflush(stdout);
      printf ("NOS:EDT:"GUIDF" received DBK "GUIDF" %s in slot %d, at %20s line %d in %s, context: %s\n", GUIDA(*((ocrGuid_t *) (whoAmI.ptr))), GUIDA(source.guid), name, slot, func, line, file, detail); fflush(stdout);
   }
#endif
} // gasket__InductDependence

void gasket__reportEdtDemise(ocrEdtDep_t whoAmI) {
#ifdef NANNY_ON_STEROIDS
   printf ("NOS:EDT:"GUIDF" done and eliminated: destroyed\n", GUIDA(*((ocrGuid_t *) (whoAmI.ptr)))); fflush(stdout);
#endif
   ocrDbDestroy(whoAmI.guid);
} // gasket__reportEdtDemise

u8 gasket__ocrDbDestroy (ocrGuid_t  * guid,     // Guid of datablock to be destroyed.  (We clobber the guid!)
                         void      ** addr,     // Address of datablock.               (We nullify the address!)
                         char       * file,     // File of calling site.
                         const char * func,     // Function of calling site.
                         int          line,     // Line number of calling site.
                         const char * detail,   // Context of the calling site.
                         const char * name) {   // Name of the datablock being destroyed.
#ifdef NANNY_ON_STEROIDS
   printf ("NOS:DBK:"GUIDF" eliminated: destroyed DBK of %s  at %20s line %d in %s, addr = %p, context: %s\n", GUIDA(*guid), name, func, line, file, *addr, detail); fflush(stdout);
#endif
   u8 returnVal = ocrDbDestroy(*guid);
   *guid = NULL_GUID;
   *addr = NULL;
   return returnVal;
} // gasket__ocrDbDestroy



u8 gasket__ocrEventDestroy (ocrGuid_t  * guid,     // Guid of the event being destroyed.
                            char       * file,     // File of calling site.
                            const char * func,     // Function of calling site.
                            int          line,     // Line number of calling site.
                            const char * detail,   // Context of the calling site.
                            const char * name) {   // Name of the datablock brought in by the event being destroyed.
#ifdef NANNY_ON_STEROIDS
   printf ("NOS:EVT:"GUIDF" eliminated: destroyed EVT of %s  at %20s line %d in %s, context: %s\n", GUIDA(*guid), name, func, line, file, detail); fflush(stdout);
#endif
   u8 returnVal = ocrEventDestroy(*guid);
   *guid = NULL_GUID;
   return returnVal;

} // gasket__ocrEventDestroy


u8 gasket__ocrEventAbandon (ocrGuid_t  * guid,     // Guid of the event being destroyed.   // Like ocrEventDestroy, but this is abandoning (destroying) an event that was never used.
                            char       * file,     // File of calling site.
                            const char * func,     // Function of calling site.
                            int          line,     // Line number of calling site.
                            const char * detail,   // Context of the calling site.
                            const char * name) {   // Name of the datablock brought in by the event being destroyed.
#ifdef NANNY_ON_STEROIDS
   printf ("NOS:EVT:"GUIDF" eliminated: abandoned EVT of %s  at %20s line %d in %s, context: %s\n", GUIDA(*guid), name, func, line, file, detail); fflush(stdout);
#endif
   u8 returnVal = ocrEventDestroy(*guid);
   *guid = NULL_GUID;
   return returnVal;

} // gasket__ocrEventAbandon

void gasket__ocrEventFlush (ocrGuid_t  * guid,     // Guid of the event being destroyed.   // Like ocrEventAbandon, but does NOT destroy the event.  Only ONE side of the comm should abandon; other should flush.
                            char       * file,     // File of calling site.
                            const char * func,     // Function of calling site.
                            int          line,     // Line number of calling site.
                            const char * detail,   // Context of the calling site.
                            const char * name) {   // Name of the datablock brought in by the event being destroyed.
#ifdef NANNY_ON_STEROIDS
   printf ("NOS:EVT:"GUIDF" eliminated: flushed   EVT of %s  at %20s line %d in %s, context: %s\n", GUIDA(*guid), name, func, line, file, detail); fflush(stdout);
#endif
   *guid = NULL_GUID;

} // gasket__ocrEventFlush
