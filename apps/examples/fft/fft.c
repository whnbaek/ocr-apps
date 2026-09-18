/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */

// OCR implementation of the Cooley-Tukey algorithm. Same as
// naive-parallel.c, but recursive creation of StartEDTs stops once
// the matrix size reaches serialBlockSize. For these small matrices ditfft2
// is called to compute the answer serially. This is meant to minimize the overhead
// of creating EDTs while still maximizing parallelism.
//
// EndEDTs are also changed to divide their work to a number of slave EDTs, such that
// each slave handles serialBlockSize elements.
//

#include "ocr.h"

#include "math.h"
#include "stdlib.h"
#include "macros.h"

/* Placement-optimization layer.  The whole recursion -- every fftStartEdt,
 * fftEndEdt and fftEndSlaveEdt -- slices ONE shared data block acquired
 * DB_MODE_RW; there is no per-subtree data to distribute.  Write access is
 * exclusive at rank granularity, so scattering the tree cannot add
 * parallelism across ranks: it can only hand the whole block from rank to
 * rank, paying a full transfer per hop.  The best a hint can do is keep the
 * tree where the block is; a real multinode FFT needs the restructured
 * decomposition, not a hint. */
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
#include <extensions/ocr-affinity.h>
/* Placement-optimization layer.  A subtree's tasks are placed by the part of
 * the transform they own -- its output offset -- rather than by where they were
 * created, so the tasks that revisit a region of the block keep returning to
 * the same place while the offsets, being a partition of the transform, keep
 * every place equally loaded.  The whole extent needs no extra parameter: the
 * recursion halves N and doubles the step, so N * stepSize is invariant and
 * equals the transform length at every level.
 *
 * Placing by the creating task instead would keep the entire recursion on the
 * one place the root started on -- perfectly local, and using a single node of
 * however many the machine has.  That is not a scheduling answer, and a run
 * that leaves most of the machine idle is not comparable with one that does
 * not. */
static ocrHint_t * fftRangeEdtHint(ocrHint_t *h, u64 offset, u64 totalN) {
    u64 pdCount;
    ocrAffinityCount(AFFINITY_PD, &pdCount);
    if (pdCount <= 1) return NULL_HINT;
    u64 place = totalN ? (offset * pdCount) / totalN : 0;
    if (place >= pdCount) place = pdCount - 1;
    ocrGuid_t aff;
    ocrAffinityGetAt(AFFINITY_PD, place, &aff);
    ocrHintInit(h, OCR_HINT_EDT_T);
    ocrSetHintValue(h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));
    return h;
}
#else
#define fftRangeEdtHint(h, off, tot) NULL_HINT
#endif

#define SERIAL_BLOCK_SIZE_DEFAULT (1024*16)

/* Two ceilings sit below what the data block alone would allow, and both fail
 * silently if they are crossed rather than checked.
 *
 * A combine node builds its whole fan-out on the worker stack -- two arrays
 * sized by the slave count, of which only the guid array is read; the other is
 * dead upstream code that nonetheless still occupies the frame -- so the ratio
 * of transform length to serial block size is bounded by the stack a worker
 * gets, and that is the ceiling the code actually has.
 *
 * The transform length itself is capped separately and conservatively: it is
 * chosen headroom below the fan-out ceiling, not an arithmetic limit of any
 * index in the program. */
#define MAX_POWER 31
#define MAX_FANOUT (1024*64)

extern void ditfft2(float *X_real, float *X_imag, float *x_in, u64 N, u64 step);
extern ocrGuid_t setUpVerify(ocrGuid_t inDB, ocrGuid_t XrealDB, ocrGuid_t XimagDB, u64 N, ocrGuid_t trigger);

typedef struct {
    ocrGuid_t startTempGuid;
    ocrGuid_t endTempGuid;
    ocrGuid_t endSlaveTempGuid;
    u64 N;
    u64 serialBlockSize;
    u64 trace;
}iterationPRM_t;

typedef struct {
    ocrGuid_t sliceTempGuid;
    u64 N;
    u64 serialBlockSize;
}initPRM_t;

typedef struct {
    u64 start;
    u64 end;
}initSlicePRM_t;

typedef struct {
    ocrGuid_t startTempGuid;
    ocrGuid_t endTempGuid;
    ocrGuid_t endSlaveTempGuid;
    u64 N;
    u64 stepSize;
    u64 offset;
    u64 x_in_offset;
    u64 serialBlockSize;
    u64 trace;
}startPRM_t;

typedef struct {
    ocrGuid_t startTempGuid;
    ocrGuid_t endTempGuid;
    ocrGuid_t endSlaveTempGuid;
    u64 N;
    u64 stepSize;
    u64 offset;
    u64 x_in_offset;
    u64 serialBlockSize;
    u64 trace;
}endPRM_t;

typedef struct {
    u64 N;
    u64 step;
    u64 offset;
    u64 kstart;
    u64 kend;
}endSlavePRM_t;

typedef struct {
    u64 N;
    u64 serialBlockSize;
    ocrGuid_t startTempGuid;
    ocrGuid_t endTempGuid;
    ocrGuid_t endSlaveTempGuid;
}printPRM_t;

/* The result scalar is summed where the values are produced rather than in one
 * sweep afterwards: the last combine phase already touches every output, and a
 * separate pass over it is a serial task on the critical path whose cost grows
 * with the transform.  The slaves of that phase partition the output, so each
 * accumulates its own share into its own slot -- no two of them write the same
 * word -- and the final task adds the slots up.
 *
 * The slots live past the three arrays in the same block, which is why the
 * block is created with room for them.  Both sides derive the count with this
 * function so the layout cannot disagree. */
static u64 fftPartialSlots(u64 N, u64 serialBlockSize) {
    if(N <= serialBlockSize) return 0;          /* no combine phase at all */
    if(N/2 > serialBlockSize) return (N/2)/serialBlockSize;
    return 1;
}

/* The input is a pure function of its index, so the slice that owns a range of
 * the signal writes it: no task generates data another task owns, and the
 * generated values do not depend on how the range is cut. */
ocrGuid_t fftInitSliceEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    initSlicePRM_t *sliceParamvIn = (initSlicePRM_t *)paramv;
    float *x_in = (float*)depv[0].ptr;
    u64 start = sliceParamvIn->start;
    u64 end = sliceParamvIn->end;

    u64 i;
    for(i=start;i<end;i++) {
        x_in[i] = 0;
    }
    if(start <= 1 && 1 < end) {
        x_in[1] = 1;
    }

    return NULL_GUID;
}

ocrGuid_t fftInitEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    initPRM_t *initParamvIn = (initPRM_t *)paramv;

    ocrGuid_t sliceTempGuid = initParamvIn->sliceTempGuid;
    ocrGuid_t dataGuid = depv[0].guid;
    u64 N = initParamvIn->N;
    u64 serialBlockSize = initParamvIn->serialBlockSize;
    u64 grain = (serialBlockSize < N) ? serialBlockSize : N;

    ocrPrintf("Creating %lu init slices for N=%lu\n",
              (unsigned long)((N + grain - 1)/grain), (unsigned long)N);

    /* This EDT only spawns the writers of the block; release precedes any
     * exposure of it. */
    ocrDbRelease(dataGuid);

    u64 start;
    for(start=0;start<N;start+=grain) {
        initSlicePRM_t sliceParamv;

        sliceParamv.start = start;
        sliceParamv.end = (start + grain < N) ? start + grain : N;

        ocrGuid_t sliceGuid;
        ocrHint_t sliceHNT;
        ocrEdtCreate(&sliceGuid, sliceTempGuid, EDT_PARAM_DEF,
                     (u64 *)&sliceParamv, EDT_PARAM_DEF, &dataGuid,
                     EDT_PROP_NONE,
                     fftRangeEdtHint(&sliceHNT, start, N), NULL);
    }
    ocrEdtTemplateDestroy(sliceTempGuid);

    return NULL_GUID;
}

// Performs one entire iteration of FFT.
// These are meant to be chained serially for timing and testing.
ocrGuid_t fftIterationEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {

    iterationPRM_t *iterationParamvIn = (iterationPRM_t *)paramv;

    ocrGuid_t startTempGuid = iterationParamvIn->startTempGuid;
    ocrGuid_t endTempGuid = iterationParamvIn->endTempGuid;
    ocrGuid_t endSlaveTempGuid = iterationParamvIn->endSlaveTempGuid;
    u64 N = iterationParamvIn->N;
    u64 serialBlockSize = iterationParamvIn->serialBlockSize;
    ocrPrintf("Creating iteration child\n");

    startPRM_t startParamv;

    startParamv.startTempGuid = startTempGuid;
    startParamv.endTempGuid = endTempGuid;
    startParamv.endSlaveTempGuid = endSlaveTempGuid;
    startParamv.N = N;
    startParamv.stepSize = 1;
    startParamv.offset = 0;
    startParamv.x_in_offset = 0;
    startParamv.serialBlockSize = serialBlockSize;
    startParamv.trace = iterationParamvIn->trace;

    /* This EDT only reads the block; release precedes any exposure of it. */
    ocrDbRelease(depv[0].guid);
    ocrGuid_t dependencies[1] = { depv[0].guid };

    ocrGuid_t edtGuid;
    ocrHint_t edtHNT;
    ocrEdtCreate(&edtGuid, startTempGuid, EDT_PARAM_DEF, (u64 *)&startParamv, 1,
                 dependencies, EDT_PROP_FINISH,
                 fftRangeEdtHint(&edtHNT, 0, startParamv.N), NULL);

    return NULL_GUID;
}

ocrGuid_t fftStartEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    u32 i;
    startPRM_t *startParamvIn = (startPRM_t *)paramv;

    ocrGuid_t startGuid = startParamvIn->startTempGuid;
    ocrGuid_t endGuid = startParamvIn->endTempGuid;
    ocrGuid_t endSlaveGuid = startParamvIn->endSlaveTempGuid;
    float *data = (float*)depv[0].ptr;
    ocrGuid_t dataGuid = depv[0].guid;
    u64 N = startParamvIn->N;;
    u64 step = startParamvIn->stepSize;
    u64 offset = startParamvIn->offset;
    u64 x_in_offset = startParamvIn->x_in_offset;
    u64 serialBlockSize = startParamvIn->serialBlockSize;
    u64 trace = startParamvIn->trace;
    float *x_in = (float*)data;
    float *X_real = (float*)(data+offset + N*step);
    float *X_imag = (float*)(data+offset + 2*N*step);

    if(trace) ocrPrintf("Step %d offset: %d N*step: %d\n", step, offset, N*step);

    startPRM_t childParamv;
    startPRM_t childParamv2;

    if(N <= serialBlockSize) {
        ditfft2(X_real, X_imag, x_in+x_in_offset, N, step);
    } else {

        // DFT even side
        childParamv.startTempGuid = startGuid;
        childParamv.endTempGuid = endGuid;
        childParamv.endSlaveTempGuid = endSlaveGuid;
        childParamv.N = N/2;
        childParamv.stepSize = 2*step;
        childParamv.offset = 0 + offset;
        childParamv.x_in_offset = x_in_offset;
        childParamv.serialBlockSize = serialBlockSize;
        childParamv.trace = trace;

        childParamv2.startTempGuid = startGuid;
        childParamv2.endTempGuid = endGuid;
        childParamv2.endSlaveTempGuid = endSlaveGuid;
        childParamv2.N = N/2;
        childParamv2.stepSize = 2 * step;
        childParamv2.offset = N/2 + offset;
        childParamv2.x_in_offset = x_in_offset + step;
        childParamv2.serialBlockSize = serialBlockSize;
        childParamv2.trace = trace;

        if(trace) ocrPrintf("Creating children of size %d\n",N/2);
        ocrGuid_t edtGuid, edtGuid2, endEdtGuid, finishEventGuid, finishEventGuid2;

        ocrHint_t edtHNT;
        ocrEdtCreate(&edtGuid, startGuid, EDT_PARAM_DEF, (u64 *)&childParamv,
                     EDT_PARAM_DEF, NULL, EDT_PROP_FINISH,
                     fftRangeEdtHint(&edtHNT, childParamv.offset, N * step),
                     &finishEventGuid);
        ocrEdtCreate(&edtGuid2, startGuid, EDT_PARAM_DEF, (u64 *)&childParamv2,
                     EDT_PARAM_DEF, NULL, EDT_PROP_FINISH,
                     fftRangeEdtHint(&edtHNT, childParamv2.offset, N * step),
                     &finishEventGuid2);
        if(trace) ocrPrintf("finishEventGuid after create: 0x"GUIDF"\n", GUIDA(finishEventGuid));

        /* This branch only spawned readers/writers of the block and wrote
         * nothing itself; release precedes any exposure of it below. */
        ocrDbRelease(dataGuid);
        ocrGuid_t endDependencies[3] = { dataGuid, finishEventGuid, finishEventGuid2 };
        // Do calculations after having divided and conquered
        ocrEdtCreate(&endEdtGuid, endGuid, EDT_PARAM_DEF, paramv, 3,
                     endDependencies, EDT_PROP_FINISH,
                     fftRangeEdtHint(&edtHNT, offset, N * step), NULL);

        ocrAddDependence(dataGuid, edtGuid, 0, DB_MODE_RW);
        ocrAddDependence(dataGuid, edtGuid2, 0, DB_MODE_RW);
    }

    if(trace) ocrPrintf("Task with size %d completed\n",N);
    return NULL_GUID;
}

ocrGuid_t fftEndEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    u32 i;
    endPRM_t *endParamvIn = (endPRM_t *)paramv;

    ocrGuid_t startGuid = endParamvIn->startTempGuid;
    ocrGuid_t endGuid = endParamvIn->endTempGuid;
    ocrGuid_t endSlaveGuid = endParamvIn->endSlaveTempGuid;
    float *data = (float*)depv[0].ptr;
    ocrGuid_t dataGuid = depv[0].guid;
    u64 N = endParamvIn->N;
    u64 step = endParamvIn->stepSize;
    u64 offset = endParamvIn->offset;
    u64 serialBlockSize = endParamvIn->serialBlockSize;
    u64 trace = endParamvIn->trace;
    float *x_in = (float*)data+offset;
    float *X_real = (float*)(data+offset + N*step);
    float *X_imag = (float*)(data+offset + 2*N*step);

    if(trace) ocrPrintf("Reached end phase for step %d\n",step);
    /* The combine work happens in the slaves; this EDT only reads the block.
     * Release precedes any exposure of it via the slave creates below. */
    ocrDbRelease(dataGuid);
    u64 *slaveParamv;

    if(N/2 > serialBlockSize) {
        ocrGuid_t slaveGuids[(N/2)/serialBlockSize];
        u64 slaveParamv[5 * (N/2)/serialBlockSize];

        if(trace) ocrPrintf("Creating %d slaves for N=%d\n",(N/2)/serialBlockSize,N);

        for(i=0;i<(N/2)/serialBlockSize;i++) {
            endSlavePRM_t slaveParamv;

            slaveParamv.N = N;
            slaveParamv.step = step;
            slaveParamv.offset = offset;
            slaveParamv.kstart = i*serialBlockSize;
            slaveParamv.kend = (i+1)*serialBlockSize;

            /* A slave writes k and k+N/2 alike, so its share of the subtree is
             * two ranges, not one; keying it on k alone would place every
             * slave of a subtree in the subtree's low half.  Doubling the
             * index spreads the slaves monotonically over the whole range the
             * subtree owns, which is both balanced and still clustered. */
            ocrHint_t slaveHNT;
            ocrEdtCreate(slaveGuids+i, endSlaveGuid, EDT_PARAM_DEF,
                         (u64 *)&slaveParamv, EDT_PARAM_DEF, &dataGuid,
                         EDT_PROP_NONE,
                         fftRangeEdtHint(&slaveHNT,
                                         offset + 2*slaveParamv.kstart,
                                         N * step), NULL);
        }
    } else {
        ocrGuid_t slaveGuids[1];
        endSlavePRM_t slaveParamv;
        ocrHint_t slaveHNT2;

        slaveParamv.N = N;
        slaveParamv.step = step;
        slaveParamv.offset = offset;
        slaveParamv.kstart = 0;
        slaveParamv.kend = N/2;

        ocrEdtCreate(slaveGuids, endSlaveGuid, EDT_PARAM_DEF, (u64 *)&slaveParamv,
                     EDT_PARAM_DEF, &dataGuid, EDT_PROP_NONE,
                     fftRangeEdtHint(&slaveHNT2, offset, N * step), NULL);
    }
    return NULL_GUID;
}
ocrGuid_t fftEndSlaveEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
    u32 i;
    float *data = (float*)depv[0].ptr;
    ocrGuid_t dataGuid = depv[0].guid;

    endSlavePRM_t *slaveParamvIn = (endSlavePRM_t *)paramv;

    u64 N = slaveParamvIn->N;
    u64 step = slaveParamvIn->step;
    u64 offset = slaveParamvIn->offset;
    float *x_in = (float*)data+offset;
    float *X_real = (float*)(data+offset + N*step);
    float *X_imag = (float*)(data+offset + 2*N*step);
    u64 kStart = slaveParamvIn->kstart;
    u64 kEnd = slaveParamvIn->kend;

    /* step is 1 only at the root of the recursion, and it is the root's
     * combine phase that writes the transform's actual output. */
    int atTop = (step == 1);
    double part = 0.0;

    u64 k;
    for(k=kStart;k<kEnd;k++) {
        float t_real = X_real[k];
        float t_imag = X_imag[k];
        double twiddle_real;
        double twiddle_imag;
        twiddle_imag = sin(-2 * M_PI * k / N);
        twiddle_real = cos(-2 * M_PI * k / N);
        float xr = X_real[k+N/2];
        float xi = X_imag[k+N/2];

        // (a+bi)(c+di) = (ac - bd) + (bc + ad)i
        X_real[k] = t_real +
            (twiddle_real*xr - twiddle_imag*xi);
        X_imag[k] = t_imag +
            (twiddle_imag*xr + twiddle_real*xi);
        X_real[k+N/2] = t_real -
            (twiddle_real*xr - twiddle_imag*xi);
        X_imag[k+N/2] = t_imag -
            (twiddle_imag*xr + twiddle_real*xi);

        if(atTop) {
            part += fabs((double)X_real[k])       + fabs((double)X_imag[k])
                  + fabs((double)X_real[k+N/2])   + fabs((double)X_imag[k+N/2]);
        }
    }

    if(atTop) {
        /* The slaves partition k, so this slot belongs to this slave alone. */
        double *slot = (double*)(data + 3*N);
        slot[kStart / (kEnd - kStart)] = part;
    }

    return NULL_GUID;
}

ocrGuid_t finalPrintEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {

    printPRM_t *printParamvIn = (printPRM_t *)paramv;

    u32 i;
    float *data = (float*)depv[1].ptr;
    ocrGuid_t dataGuid = depv[1].guid;
    u64 N = printParamvIn->N;
    u64 serialBlockSize = printParamvIn->serialBlockSize;
    float *X_real = (float*)(data + N);
    float *X_imag = (float*)(data + 2*N);

    ocrPrintf("Final print EDT\n");

    {   /* The result scalar: the sum of |Re| + |Im| over the parallel output.
         * It was always taken over this array; it merely used to be printed
         * from the verification stage, which made a reference recomputation
         * look like part of the program.  The combine phase's slaves have
         * already summed their own shares, so this adds up their slots; a
         * transform small enough to have had no combine phase has no slots,
         * and is swept here. */
        double fft_checksum = 0.0;
        u64 slots = fftPartialSlots(N, serialBlockSize);
        if(slots) {
            const double *slot = (const double*)(data + 3*N);
            for(i=0;i<slots;i++) fft_checksum += slot[i];
        } else {
            for(i=0;i<N;i++) {
                fft_checksum += fabs((double)X_real[i]) + fabs((double)X_imag[i]);
            }
        }
        ocrPrintf("FFT checksum = %f\n", fft_checksum);
    }

    ocrDbDestroy(dataGuid);

    ocrGuid_t startTempGuid = printParamvIn->startTempGuid;
    ocrGuid_t endTempGuid = printParamvIn->endTempGuid;
    ocrGuid_t endSlaveTempGuid = printParamvIn->endSlaveTempGuid;
    ocrEdtTemplateDestroy(startTempGuid);
    ocrEdtTemplateDestroy(endTempGuid);
    ocrEdtTemplateDestroy(endSlaveTempGuid);

ocrPrintf("FFT calling shutdown\n");
    ocrShutdown();
    return NULL_GUID;
}

bool parseOptions(u32 argc, char **argv, u64 *N, bool *verify,
                  u64 *serialBlockSize, bool *trace) {
  u32 a;

  if (argc < 2 || argc > 5) {
    ocrPrintf("Usage: fft <power> [serialBlockSize] [v] [t]   ('v' re-runs the "
              "transform serially and compares, which is a reference check, "
              "not the work; 't' turns on the per-task trace)\n");
    return false;
  }

  *verify = false;
  *trace = false;

  s64 power = atoi(argv[1]);
  ocrPrintf("Power %ld\n", power);
  if (power < 1 || power > MAX_POWER) {
    ocrPrintf("fft: power must be in [1, %d]; got %ld\n", MAX_POWER, power);
    return false;
  }
  *N = (u64)1 << power;

  for (a = 2; a < argc; a++) {
    /* The verification stage re-runs the whole transform serially in one task
     * and compares it point by point.  That is a reference implementation, not
     * part of the work being timed, and at any interesting size it is the
     * largest single task in the program -- so it is off unless asked for.
     * The result scalar does not depend on it: the checksum is taken over the
     * parallel output, and is printed whether or not it runs. */
    if (argv[a][0] == 'v') {
      *verify = true;
      continue;
    }
    /* The per-task trace emits one line per task of the recursion.  Each line
     * is a write() on a stream every worker shares, so the trace serializes
     * what the program exists to run in parallel and its cost grows with the
     * task count; it is a diagnostic, off unless asked for. */
    if (argv[a][0] == 't') {
      *trace = true;
      continue;
    }
    s64 sbs = atoi(argv[a]);
    if (sbs < 1 || (sbs & (sbs - 1)) != 0 || (u64)sbs > *N) {
      ocrPrintf("fft: serialBlockSize must be a power of two in [1, %lu]; "
                "got %ld\n", (unsigned long)*N, sbs);
      return false;
    }
    *serialBlockSize = (u64)sbs;
  }

  if (*N/2 > *serialBlockSize && (*N/2)/(*serialBlockSize) > MAX_FANOUT) {
    ocrPrintf("fft: a combine fan-out of %lu exceeds the %lu a stack frame "
              "holds; raise serialBlockSize or lower power\n",
              (unsigned long)((*N/2)/(*serialBlockSize)),
              (unsigned long)MAX_FANOUT);
    return false;
  }
  return true;
}


ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
    u64 argc = ocrGetArgc(depv[0].ptr);
    u32 i;
    char *argv[argc];

    for(i=0;i<argc;i++) {
        argv[i] = ocrGetArgv(depv[0].ptr,i);
    }

    u64 N;
    bool verify;
    bool trace;
    u64 serialBlockSize = SERIAL_BLOCK_SIZE_DEFAULT;
    if(!parseOptions(argc, argv, &N, &verify, &serialBlockSize, &trace)) {
        ocrShutdown();
        return NULL_GUID;
    }

    initPRM_t initParamv;
    iterationPRM_t iterationParamv;
    startPRM_t startParamv;
    endPRM_t endParamv;
    endSlavePRM_t endSlaveParamv;
    printPRM_t printParamv;

    ocrGuid_t initTempGuid,initSliceTempGuid;
    ocrGuid_t iterationTempGuid,startTempGuid,endTempGuid,printTempGuid,endSlaveTempGuid;
    ocrEdtTemplateCreate(&initTempGuid, &fftInitEdt, PRMNUM(init), 1);
    ocrEdtTemplateCreate(&initSliceTempGuid, &fftInitSliceEdt, PRMNUM(initSlice), 1);
    ocrEdtTemplateCreate(&iterationTempGuid, &fftIterationEdt, PRMNUM(iteration), 2);
    ocrEdtTemplateCreate(&startTempGuid, &fftStartEdt, PRMNUM(start), 1);
    ocrEdtTemplateCreate(&endTempGuid, &fftEndEdt, PRMNUM(end), 3);
    ocrEdtTemplateCreate(&endSlaveTempGuid, &fftEndSlaveEdt, PRMNUM(endSlave), 1);
    ocrEdtTemplateCreate(&printTempGuid, &finalPrintEdt, PRMNUM(print), 2);

    // x_in, X_real, and X_imag in a contiguous block
    float *x;
    ocrGuid_t dataGuid;
    // TODO: OCR cannot handle large datablocks
    u64 dataBytes = sizeof(float) * N * 3
                  + sizeof(double) * fftPartialSlots(N, serialBlockSize);
    ocrDbCreate(&dataGuid, (void **) &x, dataBytes, 0, NULL_HINT, NO_ALLOC);
    ocrPrintf("Datablock of size %lu (N=%lu) created\n",dataBytes,N);
    /* Nothing is written here: the input is generated by the tasks that own
     * it.  Release precedes any exposure of the block. */
    ocrDbRelease(dataGuid);

    initParamv.sliceTempGuid = initSliceTempGuid;
    initParamv.N = N;
    initParamv.serialBlockSize = serialBlockSize;

    iterationParamv.startTempGuid = startTempGuid;
    iterationParamv.endTempGuid = endTempGuid;
    iterationParamv.endSlaveTempGuid = endSlaveTempGuid;
    iterationParamv.N = N;
    iterationParamv.serialBlockSize = serialBlockSize;
    iterationParamv.trace = trace ? 1 : 0;

    ocrGuid_t initEdtGuid, edtGuid, printEdtGuid, initEventGuid, edtEventGuid;

    ocrEdtCreate(&initEdtGuid, initTempGuid, EDT_PARAM_DEF, (u64 *)&initParamv,
                 EDT_PARAM_DEF, NULL, EDT_PROP_FINISH, NULL_HINT,
                 &initEventGuid);
    ocrEdtTemplateDestroy(initTempGuid);

    ocrEdtCreate(&edtGuid, iterationTempGuid, EDT_PARAM_DEF, (u64 *)&iterationParamv,
                 EDT_PARAM_DEF, NULL, EDT_PROP_FINISH, NULL_HINT,
                 &edtEventGuid);
    ocrEdtTemplateDestroy(iterationTempGuid);

    if(verify) {
        edtEventGuid = setUpVerify(dataGuid, NULL_GUID, NULL_GUID, N, edtEventGuid);
    }

    printParamv.N = N;
    printParamv.serialBlockSize = serialBlockSize;
    printParamv.startTempGuid = startTempGuid;
    printParamv.endTempGuid = endTempGuid;
    printParamv.endSlaveTempGuid = endSlaveTempGuid;

    ocrEdtCreate(&printEdtGuid, printTempGuid, EDT_PARAM_DEF, (u64 *)&printParamv,
                 EDT_PARAM_DEF, NULL, EDT_PROP_NONE, NULL_HINT, NULL);
    ocrEdtTemplateDestroy(printTempGuid);
    /* The print EDT only reads the result block; the mode declares that. */
    ocrAddDependence(edtEventGuid, printEdtGuid, 0, DB_MODE_CONST);
    ocrAddDependence(dataGuid, printEdtGuid, 1, DB_MODE_RO);

    /* The transform waits on the generation phase's scope, so the input exists
     * before any leaf reads it. */
    ocrAddDependence(dataGuid, edtGuid, 0, DB_MODE_RO);
    ocrAddDependence(initEventGuid, edtGuid, 1, DB_MODE_CONST);

    /* Launched last: every consumer of the generation phase's scope is wired
     * before the phase can complete. */
    ocrAddDependence(dataGuid, initEdtGuid, 0, DB_MODE_RO);

    return NULL_GUID;
}
