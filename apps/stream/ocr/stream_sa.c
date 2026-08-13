/*
 * This file is subject to the license agreement located in the file LICENSE
 * and cannot be distributed without it. This notice cannot be
 * removed or modified.
 */
/* Based on stream benchmark at https://www.cs.virginia.edu/stream/FTP/Code/stream.c */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ocr.h"
#include <stdlib.h>
#include <stdio.h>

# include <math.h>
# include <float.h>
#include "pthread.h"

#ifndef STREAM_ARRAY_SIZE
#define STREAM_ARRAY_SIZE 9000000
#endif
#ifndef NUM_THREADS
#define NUM_THREADS  32
#endif

#define FLAGS DB_PROP_NONE
#define PROPERTIES EDT_PROP_NONE

# ifndef MIN
# define MIN(x,y) ((x)<(y)?(x):(y))
# endif
# ifndef MAX
# define MAX(x,y) ((x)>(y)?(x):(y))
# endif

# define HLINE "-------------------------------------------------------------\n"

#ifndef STREAM_TYPE
#define STREAM_TYPE double
#endif

#define MOD NUM_THREADS
#define NTIMES 1000
#define NTIMES_MAX 4000

#define OFFSET 0

#define PER_THREAD_SIZE (STREAM_ARRAY_SIZE/NUM_THREADS)

STREAM_TYPE scalar = 3.0;

/* Cross-rank GUIDs and the iteration count travel through paramv
 * (deep-copied at EDT create), never file-scope statics: statics are
 * per-process, so only the rank that ran mainEdt would see them initialized.
 *
 * loop paramv (paramc=LOOP_PARAMC):
 *   [0] iter, [1] tid, [2] ntimes,
 *   [3..7] tmp_copy, tmp_scale, tmp_add, tmp_triad, tmp_loop,
 *   [8] evt_finalize_my (this thread's ONCE event GUID)
 * mainLet paramv (paramc=MAINLET_PARAMC): loop's layout minus [0] iter. */
#define MAINLET_PARAMC 8
#define LOOP_PARAMC    9

#include <sys/time.h>

double mysecond()
{
        struct timeval tp;
        struct timezone tzp;
        int i;

        i = gettimeofday(&tp,&tzp);
        return ( (double) tp.tv_sec + (double) tp.tv_usec * 1.e-6 );
}

void checkSTREAMresults (void);
void preamble(u32 ntimes);
int printTimes(u32 ntimes);
double times[NUM_THREADS][NTIMES_MAX];

ocrGuid_t copy(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    double *a = (double *)depv[0].ptr;
    double *c;
    ocrGuid_t db_c;
    u32 i;

    ocrDbCreate(&db_c, (void **)&c, sizeof(double)*PER_THREAD_SIZE,
                             FLAGS, NULL_HINT, NO_ALLOC);
    for(i = 0; i<PER_THREAD_SIZE; i++) c[i] = a[i];
    return db_c;
}

ocrGuid_t scale(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    double *a = (double *)depv[0].ptr;
    double *b;
    ocrGuid_t db_b;
    u32 i;

    ocrDbCreate(&db_b, (void **)&b, sizeof(double)*PER_THREAD_SIZE,
                             FLAGS, NULL_HINT, NO_ALLOC);
    for(i = 0; i<PER_THREAD_SIZE; i++) b[i] = scalar*a[i];

    return db_b;
}

ocrGuid_t add(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    double *a = (double *)depv[0].ptr;
    double *b = (double *)depv[1].ptr;
    double *c;
    ocrGuid_t db_c;
    u32 i;

    ocrDbDestroy(depv[2].guid); // destroy c from copy
    ocrDbCreate(&db_c, (void **)&c, sizeof(double)*PER_THREAD_SIZE,
                             FLAGS, NULL_HINT, NO_ALLOC);
    for(i = 0; i<PER_THREAD_SIZE; i++) c[i] = a[i] + b[i];

    return db_c;
}

ocrGuid_t triad(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    double *a = (double *)depv[0].ptr;
    double *b = (double *)depv[1].ptr;
    double *c = (double *)depv[2].ptr;
    ocrGuid_t db_a;
    u32 i;

    ocrDbDestroy(depv[0].guid); // destroy old a
    ocrDbCreate(&db_a, (void **)&a, sizeof(double)*PER_THREAD_SIZE,
                             FLAGS, NULL_HINT, NO_ALLOC);
    for(i = 0; i<PER_THREAD_SIZE; i++) a[i] = b[i] + scalar*c[i];

    times[paramv[1]][paramv[0]] = mysecond() - times[paramv[1]][paramv[0]];

    ocrDbDestroy(depv[1].guid); // destroy old b
    ocrDbDestroy(depv[2].guid); // destroy old c
    return db_a;
}

ocrGuid_t finalize(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
  double *ptr = (double *)depv[0].ptr;
  printTimes((u32)paramv[0]);
  {
    // Correctness scalar: replay the per-element recurrence (copy, scale,
    // add, triad) and report a[0]'s relative error — identical op order, so
    // a correct run prints exactly zero.
    double aj = 2.0, bj = 2.0, cj = 0.0;
    u32 k;
    for (k = 0; k < (u32)paramv[0]; k++) {
      cj = aj;               /* copy  */
      bj = scalar * aj;      /* scale */
      cj = aj + bj;          /* add   */
      aj = cj + scalar * bj; /* triad: slot 1 = add output, slot 2 = scale output */
    }
    PRINTF("STREAM_VALID relerr = %.6e\n", fabs(ptr[0] - aj) / fabs(aj));
  }
  ocrShutdown();
  return NULL_GUID;
}

ocrGuid_t loop(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{

  ocrGuid_t db_a = depv[0].guid;
  u64 iter   = paramv[0];
  u64 tid    = paramv[1];
  u64 ntimes = paramv[2];
  ocrGuid_t tmp_copy_g  = (ocrGuid_t){.guid = (intptr_t)paramv[3]};
  ocrGuid_t tmp_scale_g = (ocrGuid_t){.guid = (intptr_t)paramv[4]};
  ocrGuid_t tmp_add_g   = (ocrGuid_t){.guid = (intptr_t)paramv[5]};
  ocrGuid_t tmp_triad_g = (ocrGuid_t){.guid = (intptr_t)paramv[6]};
  ocrGuid_t tmp_loop_g  = (ocrGuid_t){.guid = (intptr_t)paramv[7]};
  ocrGuid_t evt_finalize_my = (ocrGuid_t){.guid = (intptr_t)paramv[8]};

  /* Kernel paramv keeps the [iter, tid] layout the timing writes read. */
  u64 param[2] = { iter, tid };

  times[tid][iter] = mysecond();
  ocrGuid_t copy_output, scale_output, add_output, triad_output;
  ocrGuid_t edt_copy, edt_scale, edt_add, edt_triad;

  ocrEdtCreate(&edt_copy, tmp_copy_g, EDT_PARAM_DEF, param, EDT_PARAM_DEF, NULL, PROPERTIES, NULL_HINT, &copy_output);
  ocrEdtCreate(&edt_scale, tmp_scale_g, EDT_PARAM_DEF, param, EDT_PARAM_DEF, NULL, PROPERTIES, NULL_HINT, &scale_output);
  ocrEdtCreate(&edt_add, tmp_add_g, EDT_PARAM_DEF, param, EDT_PARAM_DEF, NULL, PROPERTIES, NULL_HINT, &add_output);
  ocrEdtCreate(&edt_triad, tmp_triad_g, EDT_PARAM_DEF, param, EDT_PARAM_DEF, NULL, PROPERTIES, NULL_HINT, &triad_output);

  /* ONCE output events are destroyed on satisfy, so every consumer must be
   * registered before any producer EDT can become runnable.  Wire the next
   * iteration (and finalize) first; only then add the DB dependences that
   * enable the kernels. */
  iter++;
  if (iter < ntimes) {  // Spawn another set
    /* Forward all paramv slots to the next iteration. */
    u64 next_param[LOOP_PARAMC] = {
      iter, tid, ntimes, paramv[3], paramv[4],
      paramv[5], paramv[6], paramv[7], paramv[8]
    };
    ocrGuid_t edt_loop;
    ocrEdtCreate(&edt_loop, tmp_loop_g, EDT_PARAM_DEF, next_param, EDT_PARAM_DEF, NULL, PROPERTIES, NULL_HINT, NULL);

    ocrAddDependence(triad_output, edt_loop, 0, DB_MODE_RW);
  } else {
      ocrAddDependence(triad_output, evt_finalize_my, 0, DB_DEFAULT_MODE);
  }

  ocrAddDependence(db_a, edt_triad, 0, DB_MODE_RW);
  ocrAddDependence(add_output, edt_triad, 1, DB_MODE_RO);
  ocrAddDependence(scale_output, edt_triad, 2, DB_MODE_RO);

  ocrAddDependence(db_a, edt_add, 0, DB_MODE_RO);
  ocrAddDependence(scale_output, edt_add, 1, DB_MODE_RO);
  ocrAddDependence(copy_output, edt_add, 2, DB_MODE_RW);

  ocrAddDependence(db_a, edt_scale, 0, DB_MODE_RO);

  ocrAddDependence(db_a, edt_copy, 0, DB_MODE_RO);

  return NULL_GUID;

}

ocrGuid_t mainLet(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    // Create DBs
    ocrGuid_t db_a;

    double *a;
    u32 i;
    ocrGuid_t edt_loop, output_evt;
    ocrGuid_t tmp_loop_g = (ocrGuid_t){.guid = (intptr_t)paramv[6]};

    ocrDbCreate(&db_a, (void **)&a, sizeof(double)*PER_THREAD_SIZE,
                             FLAGS, NULL_HINT, NO_ALLOC);

    // Init them
    for(i = 0; i<PER_THREAD_SIZE; i++) {
        a[i] = 2.0;
    }

    /* Forward tid, ntimes and all template/event GUIDs to loop. */
    u64 param[LOOP_PARAMC] = {
      0 /*iter*/, paramv[0] /*tid*/, paramv[1] /*ntimes*/,
      paramv[2], paramv[3], paramv[4], paramv[5], paramv[6], paramv[7]
    };
    ocrEdtCreate(&edt_loop, tmp_loop_g, EDT_PARAM_DEF, param, EDT_PARAM_DEF, NULL, EDT_PROP_FINISH, NULL_HINT, &output_evt);

    ocrAddDependence(db_a, edt_loop, 0, DB_MODE_RW);

    return output_evt;
}

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[])
{
    u32 ntimes = NTIMES;
    u64 argc = getArgc(depv[0].ptr);
    if (argc > 1) {
        u64 v = strtoull(getArgv(depv[0].ptr, 1), NULL, 10);
        if (v >= 2 && v <= NTIMES_MAX) ntimes = (u32)v;
    }

    preamble(ntimes);

    // Create NUM_THREADS sets of datablocks (3/each)
    // Initialize them
    // Spawn the following tasks in a loop
    // 1. Copy
    // 2. Scale
    // 3. Add
    // 4. Triad

    ocrGuid_t tmp_mainLet;
    ocrGuid_t tmp_copy, tmp_scale, tmp_add, tmp_triad, tmp_loop, tmp_finalize;
    ocrGuid_t edt_finalize, edt_mainLet;
    u64 i;
    ocrHint_t hint_disperse;

    if(ocrHintInit(&hint_disperse, OCR_HINT_EDT_T )) PRINTF("Error initializing hint\n");
    if(ocrSetHintValue(&hint_disperse, OCR_HINT_EDT_DISPERSE, OCR_HINT_EDT_DISPERSE_NEAR)) PRINTF("Error setting hint\n");

    ocrEdtTemplateCreate(&tmp_mainLet, mainLet, MAINLET_PARAMC, 0);

    // Spawn the threads
    ocrEdtTemplateCreate(&tmp_loop, loop, LOOP_PARAMC, 1);
    ocrEdtTemplateCreate(&tmp_copy, copy, 2, 1);
    ocrEdtTemplateCreate(&tmp_scale, scale, 2, 1);
    ocrEdtTemplateCreate(&tmp_add, add, 2, 3);
    ocrEdtTemplateCreate(&tmp_triad, triad, 2, 3);

    ocrEdtTemplateCreate(&tmp_finalize, finalize, 1, NUM_THREADS);
    u64 finalize_param[1] = { ntimes };
    ocrEdtCreate(&edt_finalize, tmp_finalize, EDT_PARAM_DEF, finalize_param, EDT_PARAM_DEF, NULL, PROPERTIES, NULL_HINT, NULL);

    for(i = 0; i<NUM_THREADS; i++) {
        ocrGuid_t evt_finalize_i;
        ocrEventCreate(&evt_finalize_i, OCR_EVENT_ONCE_T, true);
        ocrAddDependence(evt_finalize_i, edt_finalize, i, DB_MODE_RO);

        /* Forward all template GUIDs, this thread's finalize event and the
         * runtime iteration count through paramv (cross-rank safe). */
        u64 mainLet_param[MAINLET_PARAMC] = {
          i /*tid*/, ntimes,
          (u64)tmp_copy.guid, (u64)tmp_scale.guid, (u64)tmp_add.guid,
          (u64)tmp_triad.guid, (u64)tmp_loop.guid,
          (u64)evt_finalize_i.guid
        };
        ocrEdtCreate(&edt_mainLet, tmp_mainLet, EDT_PARAM_DEF, mainLet_param, EDT_PARAM_DEF, NULL, PROPERTIES, &hint_disperse, NULL);
    }

    return NULL_GUID;
}

void preamble(u32 ntimes)
{
    int                 BytesPerWord;
    int j;
    double t;

    /* --- SETUP --- determine precision and check timing --- */

    PRINTF(HLINE);
    PRINTF("STREAM version $Revision: 5.10 $\n");
    PRINTF(HLINE);
    BytesPerWord = sizeof(STREAM_TYPE);
    PRINTF("This system uses %d bytes per array element.\n",
        BytesPerWord);

    PRINTF(HLINE);
#ifdef N
    PRINTF("*****  WARNING: ******\n");
    PRINTF("      It appears that you set the preprocessor variable N when compiling this code.\n");
    PRINTF("      This version of the code uses the preprocesor variable STREAM_ARRAY_SIZE to control the array size\n");
    PRINTF("      Reverting to default value of STREAM_ARRAY_SIZE=%llu\n",(unsigned long long) STREAM_ARRAY_SIZE);
    PRINTF("*****  WARNING: ******\n");
#endif

    PRINTF("Array size = %llu (elements), Offset = %d (elements)\n" , (unsigned long long) STREAM_ARRAY_SIZE, OFFSET);
    PRINTF("Memory per array = %.1f MiB (= %.1f GiB).\n",
        BytesPerWord * ( (double) STREAM_ARRAY_SIZE / 1024.0/1024.0),
        BytesPerWord * ( (double) STREAM_ARRAY_SIZE / 1024.0/1024.0/1024.0));
    PRINTF("Total memory required = %.1f MiB (= %.1f GiB).\n",
        (3.0 * BytesPerWord) * ( (double) STREAM_ARRAY_SIZE / 1024.0/1024.),
        (3.0 * BytesPerWord) * ( (double) STREAM_ARRAY_SIZE / 1024.0/1024./1024.));
    PRINTF("Each kernel will be executed %d times.\n", ntimes);
    PRINTF(" The *best* time for each kernel (excluding the first iteration)\n");
    PRINTF(" will be used to compute the reported bandwidth.\n");

    PRINTF(HLINE);

}

int printTimes(u32 ntimes)
{
    double avgtime = 0.0;
    double mintime = FLT_MAX;
    double maxtime = 0.0;
    int j, k;

    for (k=1; k<ntimes; k++) /* note -- skip first iteration */
        {
        for (j=0; j<NUM_THREADS; j++)
            {
            avgtime = avgtime + times[j][k];
            mintime = MIN(mintime, times[j][k]);
            maxtime = MAX(maxtime, times[j][k]);
            }
        }

    PRINTF(HLINE);
    PRINTF("%f MB/s %f %f %f\n", 1.0E-06*10*sizeof(STREAM_TYPE)*PER_THREAD_SIZE/mintime, avgtime/((ntimes-1)*NUM_THREADS), mintime, maxtime);
    PRINTF(HLINE);

    return 0;

}

/* A gettimeofday routine to give access to the wall
 *    clock timer on most UNIX-like systems.  */
#if 0
#include <sys/time.h>

#ifndef abs
#define abs(a) ((a) >= 0 ? (a) : -(a))
#endif
void checkSTREAMresults ()
{
        STREAM_TYPE aj,bj,cj,scalar;
        STREAM_TYPE aSumErr,bSumErr,cSumErr;
        STREAM_TYPE aAvgErr,bAvgErr,cAvgErr;
        double epsilon;
        ssize_t j;
        int     k,ierr,err;

    /* reproduce initialization */
        aj = 1.0;
        bj = 2.0;
        cj = 0.0;
    /* a[] is modified during timing check */
        aj = 2.0E0 * aj;
    /* now execute timing loop */
        scalar = 3.0;
        for (k=0; k<NTIMES; k++)
        {
            cj = aj;
            bj = scalar*cj;
            cj = aj+bj;
            aj = bj+scalar*cj;
        }

    /* accumulate deltas between observed and expected results */
        aSumErr = 0.0;
        bSumErr = 0.0;
        cSumErr = 0.0;
        for (j=0; j<STREAM_ARRAY_SIZE; j++) {
                aSumErr += abs(a[j] - aj);
                bSumErr += abs(b[j] - bj);
                cSumErr += abs(c[j] - cj);
        }
        aAvgErr = aSumErr / (STREAM_TYPE) STREAM_ARRAY_SIZE;
        bAvgErr = bSumErr / (STREAM_TYPE) STREAM_ARRAY_SIZE;
        cAvgErr = cSumErr / (STREAM_TYPE) STREAM_ARRAY_SIZE;

        if (sizeof(STREAM_TYPE) == 4) {
                epsilon = 1.e-6;
        }
        else if (sizeof(STREAM_TYPE) == 8) {
                epsilon = 1.e-13;
        }
        else {
                PRINTF("WEIRD: sizeof(STREAM_TYPE) = %lu\n",sizeof(STREAM_TYPE));
                epsilon = 1.e-6;
        }

        err = 0;
        if (abs(aAvgErr/aj) > epsilon) {
                err++;
                PRINTF ("Failed Validation on array a[], AvgRelAbsErr > epsilon (%e)\n",epsilon);
                PRINTF ("     Expected Value: %e, AvgAbsErr: %e, AvgRelAbsErr: %e\n",aj,aAvgErr,abs(aAvgErr)/aj);
                ierr = 0;
                for (j=0; j<STREAM_ARRAY_SIZE; j++) {
                        if (abs(a[j]/aj-1.0) > epsilon) {
                                ierr++;
#ifdef VERBOSE
                                if (ierr < 10) {
                                        PRINTF("         array a: index: %ld, expected: %e, observed: %e, relative error: %e\n",
                                                j,aj,a[j],abs((aj-a[j])/aAvgErr));
                                }
#endif
                        }
                }
                PRINTF("     For array a[], %d errors were found.\n",ierr);
        }
        if (abs(bAvgErr/bj) > epsilon) {
                err++;
                PRINTF ("Failed Validation on array b[], AvgRelAbsErr > epsilon (%e)\n",epsilon);
                PRINTF ("     Expected Value: %e, AvgAbsErr: %e, AvgRelAbsErr: %e\n",bj,bAvgErr,abs(bAvgErr)/bj);
                PRINTF ("     AvgRelAbsErr > Epsilon (%e)\n",epsilon);
                ierr = 0;
                for (j=0; j<STREAM_ARRAY_SIZE; j++) {
                        if (abs(b[j]/bj-1.0) > epsilon) {
                                ierr++;
#ifdef VERBOSE
                                if (ierr < 10) {
                                        PRINTF("         array b: index: %ld, expected: %e, observed: %e, relative error: %e\n",
                                                j,bj,b[j],abs((bj-b[j])/bAvgErr));
                                }
#endif
                        }
                }
                PRINTF("     For array b[], %d errors were found.\n",ierr);
        }
        if (abs(cAvgErr/cj) > epsilon) {
                err++;
                PRINTF ("Failed Validation on array c[], AvgRelAbsErr > epsilon (%e)\n",epsilon);
                PRINTF ("     Expected Value: %e, AvgAbsErr: %e, AvgRelAbsErr: %e\n",cj,cAvgErr,abs(cAvgErr)/cj);
                PRINTF ("     AvgRelAbsErr > Epsilon (%e)\n",epsilon);
                ierr = 0;
                for (j=0; j<STREAM_ARRAY_SIZE; j++) {
                        if (abs(c[j]/cj-1.0) > epsilon) {
                                ierr++;
#ifdef VERBOSE
                                if (ierr < 10) {
                                        PRINTF("         array c: index: %ld, expected: %e, observed: %e, relative error: %e\n",
                                                j,cj,c[j],abs((cj-c[j])/cAvgErr));
                                }
#endif
                        }
                }
                PRINTF("     For array c[], %d errors were found.\n",ierr);
        }
        if (err == 0) {
                PRINTF ("Solution Validates: avg error less than %e on all three arrays\n",epsilon);
        }
#ifdef VERBOSE
        PRINTF ("Results Validation Verbose Results: \n");
        PRINTF ("    Expected a(1), b(1), c(1): %f %f %f \n",aj,bj,cj);
        PRINTF ("    Observed a(1), b(1), c(1): %f %f %f \n",a[1],b[1],c[1]);
        PRINTF ("    Rel Errors on a, b, c:     %e %e %e \n",abs(aAvgErr/aj),abs(bAvgErr/bj),abs(cAvgErr/cj));
#endif
}
#endif
