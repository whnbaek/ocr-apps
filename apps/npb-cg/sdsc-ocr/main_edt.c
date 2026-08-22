#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <ocr.h>

#include "cg_ocr.h"
#include "la_ocr.h"

ocrGuid_t head_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t loop_top_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t loop_bottom_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t tail_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    u64 argc = ocrGetArgc(depv[0].ptr);

    classdb_t* class; ocrGuid_t classid;
    u32 niter_override = 0;
    if(argc>1) {
        if(argc!=3 && argc != 5 && argc != 7) {
            ocrPrintf("cg [-t class] [-b blocking ] [-i niter] (class=T|S|W|A|B|C|D|E; default: -t S, -b 1)\n");
            ocrShutdown();
            return NULL_GUID;
        }
        u32 blocking = 1;
        char classt = 'S';
        while(--argc) {
            if(strcmp(ocrGetArgv(depv[0].ptr,argc-1), "-t")==0)
                classt = *ocrGetArgv(depv[0].ptr,argc--);
            else if(strcmp(ocrGetArgv(depv[0].ptr,argc-1), "-b")==0)
                blocking = atoi(ocrGetArgv(depv[0].ptr,argc--));
            else if(strcmp(ocrGetArgv(depv[0].ptr,argc-1), "-i")==0)
                niter_override = atoi(ocrGetArgv(depv[0].ptr,argc--));
        }
        class_init(&class, &classid, classt, blocking);
        if(class->c == 'U') {
            ocrPrintf("cg [-t] [T,S,W,A,B,C,D,E] (default: S -t)\n");
            ocrShutdown();
            return NULL_GUID;
        }
    }
    else
        class_init(&class, &classid, 'S', 1);
    if(niter_override >= 1)
        class->niter = niter_override;

    /* The matrix is built and gathered in na/blk equal row blocks, so a
       blocking that does not divide the class size leaves the tail block
       undefined. */
    if(class->blk < 1 || class->na % class->blk != 0) {
        ocrPrintf("cg: blocking -b %u must be >= 1 and divide the class size %lu\n",
                  class->blk, class->na);
        ocrShutdown();
        return NULL_GUID;
    }

    timerdb_t* timer; ocrGuid_t timerid;
    timer_init(&timer, &timerid, class, class->on);
    ocrPrintf("CG Benchmark: size=%lu, iterations=%u\n", class->na, class->niter);

    timer_start(timer);

    ocrGuid_t aid, xid;
    double* x;
    ocrDbCreate(&xid, (void**)&x, sizeof(double)*class->na, 0, NULL_HINT, NO_ALLOC);

    if(makea(class, &aid) == -1)
        return NULL_GUID;

    u32 i;
    for(i=0; i<class->na; ++i)
        x[i] = 1;

    /* Writes are only visible to other EDTs after release: release before
       wiring the consumers. */
    ocrDbRelease(xid);
    ocrDbRelease(timerid);
    ocrGuid_t tmp,edt;
    ocrEdtTemplateCreate(&tmp, head_edt, 0, 6);
    ocrEdtCreate(&edt, tmp, 0, NULL, 6, NULL, 0, NULL_HINT, NULL);
    ocrAddDependence(classid, edt, 0, DB_MODE_CONST);
    ocrAddDependence(timerid, edt, 1, DB_MODE_RW);
    ocrAddDependence(aid, edt, 2, DB_MODE_CONST);
    ocrAddDependence(xid, edt, 3, DB_MODE_RW);
    conj_grad(class->na, class->blk, aid, xid, edt, 4);
    ocrEdtTemplateDestroy(tmp);

    ocrDbDestroy(depv[0].guid);

    return NULL_GUID;
}

//     0        1  2  3  4  5
// class, timerdb, a, x, z, norm
ocrGuid_t head_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    classdb_t* class = (classdb_t*)depv[0].ptr;
    timerdb_t* tdb = (timerdb_t*)depv[1].ptr;
    timer_stop(tdb,0);
    ocrDbDestroy(depv[4].guid);
    ocrDbDestroy(depv[5].guid);

    ocrPrintf("Iteration               ||r||               zeta\n");

    double* x = (double*)depv[3].ptr;
    u32 i;
    for(i=0; i<class->na; ++i)
        x[i] = 1;

    timer_start(tdb);

    u64 it = 1;

    ocrGuid_t tmp,edt;
    ocrEdtTemplateCreate(&tmp, loop_top_edt, 1, 4);
    ocrEdtCreate(&edt, tmp, 1, &it, 4, NULL, 0, NULL_HINT, NULL);
    /* Release the re-initialized x and the timer before wiring (visibility),
       and declare the actual access modes: class and the assembled matrix a
       are read-only from here on; the loop top itself writes only the timer,
       and x is written next by the loop bottom, not the loop top. */
    ocrDbRelease(depv[1].guid);
    ocrDbRelease(depv[3].guid);
    ocrAddDependence(depv[0].guid, edt, 0, DB_MODE_CONST);
    ocrAddDependence(depv[1].guid, edt, 1, DB_MODE_RW);
    ocrAddDependence(depv[2].guid, edt, 2, DB_MODE_CONST);
    ocrAddDependence(depv[3].guid, edt, 3, DB_MODE_CONST);
    ocrEdtTemplateDestroy(tmp);

    return NULL_GUID;
}

//     0        1  2  3
// class, timerdb, a, x
ocrGuid_t loop_top_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    classdb_t* class = (classdb_t*)depv[0].ptr;
    timerdb_t* tdb = (timerdb_t*)depv[1].ptr;

    if(class->on) timer_start(tdb);

    ocrGuid_t tmp,edt;
    ocrEdtTemplateCreate(&tmp, loop_bottom_edt, 1, 6);
    ocrEdtCreate(&edt, tmp, 1, paramv, 6, NULL, 0, NULL_HINT, NULL);
    /* Release the timer (possibly started above) before wiring, and declare
       actual access: class/a are read-only; x is written by the loop bottom
       (the only x consumer wired RW). */
    ocrDbRelease(depv[1].guid);
    ocrAddDependence(depv[0].guid, edt, 0, DB_MODE_CONST);
    ocrAddDependence(depv[1].guid, edt, 1, DB_MODE_RW);
    ocrAddDependence(depv[2].guid, edt, 2, DB_MODE_CONST);
    ocrAddDependence(depv[3].guid, edt, 3, DB_MODE_RW);
    conj_grad(class->na, class->blk, depv[2].guid, depv[3].guid, edt, 4);
    ocrEdtTemplateDestroy(tmp);

    return NULL_GUID;
}

//     0        1  2  3  4  5
// class, timerdb, a, x, z, norm
ocrGuid_t loop_bottom_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    classdb_t* class = (classdb_t*)depv[0].ptr;
    timerdb_t* tdb = (timerdb_t*)depv[1].ptr;
    double* x = (double*)depv[3].ptr;
    double* z = (double*)depv[4].ptr;

    if(class->on) timer_stop(tdb,paramv[0]+1);

    double zeta = class->shift+1/_dot(class->na,x,z);
    double norm_temp2 = 1.0/sqrt(_dot(class->na,z,z));
    __scale(class->na,x,norm_temp2,z);
    ocrPrintf("%9lu, %20.13f %10.13f\n", paramv[0], *(double*)depv[5].ptr, zeta);
    ocrDbDestroy(depv[4].guid);

    if(++paramv[0] > class->niter) {
        *(double*)depv[5].ptr = zeta;
        /* Release before wiring the consumer so it cannot acquire a stale copy. */
        ocrDbRelease(depv[5].guid);
        ocrDbRelease(depv[1].guid);
        ocrGuid_t tmp,edt;
        ocrEdtTemplateCreate(&tmp, tail_edt, 0, 3);
        ocrEdtCreate(&edt, tmp, 0, NULL, 3, NULL, 0, NULL_HINT, NULL);
        ocrAddDependence(depv[0].guid, edt, 0, DB_MODE_CONST);
        ocrAddDependence(depv[1].guid, edt, 1, DB_MODE_CONST);
        ocrAddDependence(depv[5].guid, edt, 2, DB_MODE_CONST);
        ocrDbDestroy(depv[2].guid);
        ocrDbDestroy(depv[3].guid);
        ocrEdtTemplateDestroy(tmp);
    }
    else {
        /* Release before wiring the next iteration so its readers cannot
           acquire a stale copy. */
        ocrDbRelease(depv[3].guid);
        ocrDbRelease(depv[1].guid);
        ocrGuid_t tmp,edt;
        ocrEdtTemplateCreate(&tmp, loop_top_edt, 1, 4);
        ocrEdtCreate(&edt, tmp, 1, paramv, 4, NULL, 0, NULL_HINT, NULL);
        /* Actual access modes: class/a read-only; the loop top only passes x
           through (the next writer is the following loop bottom). */
        ocrAddDependence(depv[0].guid, edt, 0, DB_MODE_CONST);
        ocrAddDependence(depv[1].guid, edt, 1, DB_MODE_RW);
        ocrAddDependence(depv[2].guid, edt, 2, DB_MODE_CONST);
        ocrAddDependence(depv[3].guid, edt, 3, DB_MODE_CONST);
        ocrDbDestroy(depv[5].guid);
        ocrEdtTemplateDestroy(tmp);
    }

    return NULL_GUID;
}

//     0        1     2
// class, timerdb, zeta
ocrGuid_t tail_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    classdb_t* class = (classdb_t*)depv[0].ptr;
    timerdb_t* tdb = (timerdb_t*)depv[1].ptr;
    double zeta = *(double*)depv[2].ptr;

    timer_stop(tdb,1);
    ocrPrintf("Benchmark completed\n");

    double err = fabs(zeta-class->zvv)/class->zvv;
#ifdef TG_ARCH
    if(err<=1e-3) {
#else
    if(err<=1e-8) {
#endif
        double mflops;
        double t_bench = timer_read(tdb,1);
        if(t_bench)
            mflops = 2*class->niter*class->na*
                     (((double)3+class->nonzer*(class->nonzer+1))+((double)25*(5+class->nonzer*(class->nonzer+1))+3))/
                     t_bench/1000000;
        ocrPrintf("Verification SUCCESSFUL (zeta=%.13f, error=%.13f\n", zeta, err);
        print_results(class, t_bench, mflops);

        if(class->on) {
            ocrPrintf("Timing (sec,%%)\n");
            double t_cg = 0;
            int it;
            for(it = 1; it<=class->niter; ++it)
                t_cg += timer_read(tdb,it+1);
            double t_init = timer_read(tdb,0);
            double wall = t_init+timer_read(tdb,1);
            ocrPrintf("init:      %10.3f (%3.1f%%)\n", t_init, t_init/wall*100);
            ocrPrintf("cg:        %10.3f (%3.1f%%)\n", t_cg, t_cg/wall*100);
            ocrPrintf("norm:      %10.3f (%3.1f%%)\n", (t_bench-t_cg), (t_bench-t_cg)/wall*100);
            ocrPrintf("wall time: %10.3f\n", wall);
        }
    }
    else {
        ocrPrintf("Verification FAILED (zeta=%.13f, correct zeta=%.13f\n", zeta, class->zvv);
    }
    ocrDbDestroy(depv[0].guid);
    ocrDbDestroy(depv[1].guid);
    ocrDbDestroy(depv[2].guid);

    ocrPrintf("DONE... going for shutdown\n");
    ocrShutdown();
    return NULL_GUID;
}
