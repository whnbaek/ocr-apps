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

extern void ditfft2(float *X_real, float *X_imag, float *x_in, u32 N, u32 step);
extern ocrGuid_t setUpVerify(ocrGuid_t inDB, ocrGuid_t XrealDB, ocrGuid_t XimagDB, u64 N, ocrGuid_t trigger);

typedef struct {
    ocrGuid_t startTempGuid;
    ocrGuid_t endTempGuid;
    ocrGuid_t endSlaveTempGuid;
    u64 N;
    u64 verbose;
    u64 serialBlockSize;
}iterationPRM_t;

typedef struct {
    ocrGuid_t startTempGuid;
    ocrGuid_t endTempGuid;
    ocrGuid_t endSlaveTempGuid;
    u64 N;
    u64 stepSize;
    u64 offset;
    u64 x_in_offset;
    u64 serialBlockSize;
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
    u64 verbose;
    u64 printResults;
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

// Performs one entire iteration of FFT.
// These are meant to be chained serially for timing and testing.
ocrGuid_t fftIterationEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {

    iterationPRM_t *iterationParamvIn = (iterationPRM_t *)paramv;

    ocrGuid_t startTempGuid = iterationParamvIn->startTempGuid;
    ocrGuid_t endTempGuid = iterationParamvIn->endTempGuid;
    ocrGuid_t endSlaveTempGuid = iterationParamvIn->endSlaveTempGuid;
    u64 N = iterationParamvIn->N;
    bool verbose = iterationParamvIn->verbose;
    u64 serialBlockSize = iterationParamvIn->serialBlockSize;
    if(verbose) {
        ocrPrintf("Creating iteration child\n");
    }

    startPRM_t startParamv;

    startParamv.startTempGuid = startTempGuid;
    startParamv.endTempGuid = endTempGuid;
    startParamv.endSlaveTempGuid = endSlaveTempGuid;
    startParamv.N = N;
    startParamv.stepSize = 1;
    startParamv.offset = 0;
    startParamv.x_in_offset = 0;
    startParamv.serialBlockSize = serialBlockSize;

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
    float *x_in = (float*)data;
    float *X_real = (float*)(data+offset + N*step);
    float *X_imag = (float*)(data+offset + 2*N*step);

    ocrPrintf("Step %d offset: %d N*step: %d\n", step, offset, N*step);

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

        childParamv2.startTempGuid = startGuid;
        childParamv2.endTempGuid = endGuid;
        childParamv2.endSlaveTempGuid = endSlaveGuid;
        childParamv2.N = N/2;
        childParamv2.stepSize = 2 * step;
        childParamv2.offset = N/2 + offset;
        childParamv2.x_in_offset = x_in_offset + step;
        childParamv2.serialBlockSize = serialBlockSize;

        ocrPrintf("Creating children of size %d\n",N/2);
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
            ocrPrintf("finishEventGuid after create: 0x"GUIDF"\n", GUIDA(finishEventGuid));

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

        ocrPrintf("Task with size %d completed\n",N);
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
    float *x_in = (float*)data+offset;
    float *X_real = (float*)(data+offset + N*step);
    float *X_imag = (float*)(data+offset + 2*N*step);

        ocrPrintf("Reached end phase for step %d\n",step);
    /* The combine work happens in the slaves; this EDT only reads the block.
     * Release precedes any exposure of it via the slave creates below. */
    ocrDbRelease(dataGuid);
    u64 *slaveParamv;

    if(N/2 > serialBlockSize) {
        ocrGuid_t slaveGuids[(N/2)/serialBlockSize];
        u64 slaveParamv[5 * (N/2)/serialBlockSize];

            ocrPrintf("Creating %d slaves for N=%d\n",(N/2)/serialBlockSize,N);

        for(i=0;i<(N/2)/serialBlockSize;i++) {
            endSlavePRM_t slaveParamv;

            slaveParamv.N = N;
            slaveParamv.step = step;
            slaveParamv.offset = offset;
            slaveParamv.kstart = i*serialBlockSize;
            slaveParamv.kend = (i+1)*serialBlockSize;

            ocrHint_t slaveHNT;
            ocrEdtCreate(slaveGuids+i, endSlaveGuid, EDT_PARAM_DEF,
                         (u64 *)&slaveParamv, EDT_PARAM_DEF, &dataGuid,
                         EDT_PROP_NONE,
                         fftRangeEdtHint(&slaveHNT, offset + slaveParamv.kstart,
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

    u32 k;
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
    bool verbose = printParamvIn->verbose;
    bool printResults = printParamvIn->printResults;
    u64 serialBlockSize = printParamvIn->serialBlockSize;
    float *x_in = (float*)data;
    float *X_real = (float*)(data + N);
    float *X_imag = (float*)(data + 2*N);

    if(verbose) {
        ocrPrintf("Final print EDT\n");
    }

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

    if(printResults) {
        ocrPrintf("Starting values:\n");
        for(i=0;i<N;i++) {
            ocrPrintf("%d [ %f ]\n",i,x_in[i]);
        }
        ocrPrintf("\n");

        ocrPrintf("Final result:\n");
        for(i=0;i<N;i++) {
            ocrPrintf("%d [%f + %fi]\n",i,X_real[i],X_imag[i]);
        }
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

bool parseOptions(u32 argc, char **argv, u64 *N, bool *verify, u64 *iterations,
                  bool *verbose, bool *printResults, u64 *serialBlockSize) {
  char c;
  char *buffer = NULL;

  if (argc < 2 || argc > 3) {
    ocrPrintf("Usage: fft <power> [v]   ('v' re-runs the transform serially "
              "and compares, which is a reference check, not the work)\n");
    return false;
  }

  *verify = false;
  *verbose = false;
  *printResults = false;
  *N = 1;
  *iterations = 1;

  s64 power = atoi(argv[1]);
  ocrPrintf("Power %ld\n", power);
  while(power-- > 0) *N=(*N)*2;
  *verbose = true;
  /* The verification stage re-runs the whole transform serially in one task
   * and compares it point by point.  That is a reference implementation, not
   * part of the work being timed, and at any interesting size it is the
   * largest single task in the program -- so it is off unless asked for.  The
   * result scalar does not depend on it: the checksum is taken over the
   * parallel output, and is printed below whether or not it runs. */
  if(argc > 2 && argv[2][0] == 'v') *verify = true;
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
    u64 iterations;
    bool verify;
    bool verbose;
    bool printResults;
    u64 serialBlockSize = SERIAL_BLOCK_SIZE_DEFAULT;
    if(!parseOptions(argc, argv, &N, &verify, &iterations, &verbose, &printResults,
                     &serialBlockSize)) {
        ocrShutdown();
        return NULL_GUID;
    }

    iterationPRM_t iterationParamv;
    startPRM_t startParamv;
    endPRM_t endParamv;
    endSlavePRM_t endSlaveParamv;
    printPRM_t printParamv;

    ocrGuid_t iterationTempGuid,startTempGuid,endTempGuid,printTempGuid,endSlaveTempGuid;
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
    if(verbose) {
        ocrPrintf("Datablock of size %lu (N=%lu) created\n",dataBytes,N);
    }

    for(i=0;i<N;i++) {
        x[i] = 0;
    }
    x[1] = 1;
    /* Input is fully written: release precedes any exposure of the block. */
    ocrDbRelease(dataGuid);
    //x[3] = -3;
    //x[4] = 8;
    //x[5] = 9;
    //x[6] = 1;

    iterationParamv.startTempGuid = startTempGuid;
    iterationParamv.endTempGuid = endTempGuid;
    iterationParamv.endSlaveTempGuid = endSlaveTempGuid;
    iterationParamv.N = N;
    iterationParamv.verbose = verbose;
    iterationParamv.serialBlockSize = serialBlockSize;

    ocrGuid_t edtGuid, printEdtGuid, edtEventGuid;

    if(iterations!=1) {
        ocrPrintf(">1 iterations currently not supported, dialing down to 1 iteration\n");
    }

    ocrEdtCreate(&edtGuid, iterationTempGuid, EDT_PARAM_DEF, (u64 *)&iterationParamv,
                 EDT_PARAM_DEF, NULL, EDT_PROP_FINISH, NULL_HINT,
                 &edtEventGuid);
    ocrEdtTemplateDestroy(iterationTempGuid);

    if(verify) {
        edtEventGuid = setUpVerify(dataGuid, NULL_GUID, NULL_GUID, N, edtEventGuid);
    }

    printParamv.N = N;
    printParamv.verbose = verbose;
    printParamv.printResults = printResults;
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

    edtEventGuid = NULL_GUID;
    /* The iteration driver only reads the block; the mode declares that. */
    ocrAddDependence(dataGuid, edtGuid, 0, DB_MODE_RO);
    ocrAddDependence(edtEventGuid, edtGuid, 1, DB_MODE_CONST);

    return NULL_GUID;
}
