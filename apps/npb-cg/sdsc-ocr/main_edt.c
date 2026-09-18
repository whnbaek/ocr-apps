#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <ocr.h>

#include "cg_ocr.h"
#include "cg_gen.h"
#include "la_ocr.h"

ocrGuid_t makea_join_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t head_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t loop_top_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t loop_bottom_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t tail_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t shutdown_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

static void cg_usage(void)
{
    ocrPrintf("cg [-t class] [-b blocking] [-i niter]"
              " (class=T|S|W|A|B|C|D|E; default: -t S, -b 1)\n");
}

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    u64 argc = ocrGetArgc(depv[0].ptr);

    classdb_t* class; ocrGuid_t classid;
    u32 niter_override = 0;
    u32 blocking = 1;
    char classt = 'S';
    u64 ai;
    /* Flag/value pairs, every flag recognised: an argument the program does
       not know is a mis-specified run, not something to walk past. */
    for(ai = 1; ai < argc; ai += 2) {
        const char* f = ocrGetArgv(depv[0].ptr, ai);
        const char* v;
        if(ai+1 >= argc) {
            ocrPrintf("cg: %s takes a value\n", f);
            cg_usage();
            ocrShutdown();
            return NULL_GUID;
        }
        v = ocrGetArgv(depv[0].ptr, ai+1);
        if(strcmp(f, "-t") == 0)
            classt = *v;
        else if(strcmp(f, "-b") == 0)
            blocking = atoi(v);
        else if(strcmp(f, "-i") == 0)
            niter_override = atoi(v);
        else {
            ocrPrintf("cg: unknown argument %s\n", f);
            cg_usage();
            ocrShutdown();
            return NULL_GUID;
        }
    }

    class_init(&class, &classid, classt, blocking);
    if(class->c == 'U') {
        ocrPrintf("cg: unknown class %c\n", classt);
        cg_usage();
        ocrShutdown();
        return NULL_GUID;
    }
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

    u64 na = class->na, blk = class->blk, nb = na/blk;
    u32 nonzer = class->nonzer;
    double shift = class->shift;

    ocrGuid_t xid;
    double* x;
    ocrDbCreate(&xid, (void**)&x, sizeof(double)*na, 0, NULL_HINT, NO_ALLOC);
    u64 i;
    for(i = 0; i < na; ++i)
        x[i] = 1;

    /* The generated entries and the inverse index over them: the part of the
       construction that is a sequential stream, and the only part of it that
       is not handed to the tasks that own the rows.  One scanner, because
       this decomposition declares no affinity: scanners write disjoint
       ranges of one index datablock, which is only a disjoint write while
       they share a policy domain. */
    cg_gen_t gen; cg_gen_ptr_t gp;
    if(cg_gen_draw(na, nonzer, &gen, &gp) == -1) {
        ocrShutdown();
        return NULL_GUID;
    }
    cg_gen_index_alloc(&gen, &gp, 0, na, nb, 1);
    cg_gen_index_range(&gen, &gp, 0);
    cg_gen_release(&gen);

    /* Writes are only visible to other EDTs after release: release before
       wiring the consumers. */
    ocrDbRelease(xid);
    ocrDbRelease(timerid);
    ocrDbRelease(classid);

    u64 jpv[CG_GEN_FULL_PARAMC];
    cg_gen_pack_full(&gen, 0, shift, jpv);
    ocrGuid_t joinTML, join;
    /* One join dependence per row block: the count follows the blocking, so
       it can exceed what the runtime accepts as a dependence count.  Both
       calls leave their guid untouched when they refuse, so neither return
       may be dropped. */
    if(ocrEdtTemplateCreate(&joinTML, makea_join_edt, CG_GEN_FULL_PARAMC, 3+nb) != 0
    || ocrEdtCreate(&join, joinTML, CG_GEN_FULL_PARAMC, jpv, 3+nb, NULL, 0,
                    NULL_HINT, NULL) != 0) {
        ocrPrintf("cg: construction of %lu row blocks exceeds the runtime's limit"
                  " on dependences per EDT; raise the blocking (-b)\n", nb);
        ocrAbort(1);
    }
    ocrEdtTemplateDestroy(joinTML);
    /* The join only forwards the timer and x to their next writer, so it
       declares neither as written. */
    ocrAddDependence(classid, join, 0, DB_MODE_CONST);
    ocrAddDependence(timerid, join, 1, DB_MODE_CONST);
    ocrAddDependence(xid, join, 2, DB_MODE_CONST);
    cg_gen_spawn(&gen, shift, cg_block_build_edt, join, 3, NULL_HINT);

    ocrDbDestroy(depv[0].guid);

    return NULL_GUID;
}

//     0        1  2  3..
// class, timerdb, x, one reference per row block
ocrGuid_t makea_join_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    classdb_t* class = (classdb_t*)depv[0].ptr;
    u64 nb = class->na/class->blk;
    cg_gen_t gen; double shift;
    cg_gen_unpack_full(&gen, &shift, paramv);

    ocrGuid_t aid; ocrGuid_t* a_ptr;
    ocrDbCreate(&aid, (void**)&a_ptr, sizeof(ocrGuid_t)*nb, 0, NULL_HINT, NO_ALLOC);
    u64 nza = 0, e;
    for(e = 0; e < nb; ++e) {
        cg_gen_ref_t* ref = (cg_gen_ref_t*)depv[3+e].ptr;
        a_ptr[e] = ref->guid;
        nza += ref->nnz;
        ocrDbDestroy(depv[3+e].guid);
    }
    ocrPrintf("number of nonzeros = %lu\n", nza);
    cg_gen_destroy(&gen);

    /* Writes are only visible to other EDTs after release: release before
       wiring the consumers. */
    ocrDbRelease(aid);

    ocrGuid_t tmp, edt;
    ocrEdtTemplateCreate(&tmp, head_edt, 0, 6);
    ocrEdtCreate(&edt, tmp, 0, NULL, 6, NULL, 0, NULL_HINT, NULL);
    ocrAddDependence(depv[0].guid, edt, 0, DB_MODE_CONST);
    ocrAddDependence(depv[1].guid, edt, 1, DB_MODE_RW);
    ocrAddDependence(aid, edt, 2, DB_MODE_CONST);
    ocrAddDependence(depv[2].guid, edt, 3, DB_MODE_RW);
    conj_grad(class->na, class->blk, aid, depv[2].guid, edt, 4);
    ocrEdtTemplateDestroy(tmp);

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
        /* The output event fires only after the tail's dependence releases
           have completed, so the shutdown EDT below never truncates release
           work out of the measured run. */
        ocrGuid_t tailOut;
        ocrEdtCreate(&edt, tmp, 0, NULL, 3, NULL, 0, NULL_HINT, &tailOut);
        ocrGuid_t sdt, sd;
        ocrEdtTemplateCreate(&sdt, shutdown_edt, 0, 1);
        ocrEdtCreate(&sd, sdt, 0, NULL, 1, NULL, 0, NULL_HINT, NULL);
        ocrAddDependence(tailOut, sd, 0, DB_MODE_NULL);
        ocrEdtTemplateDestroy(sdt);
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
    /* Shutdown is NOT called here: this EDT still owes the release of its
       dependences; a dedicated shutdown EDT wired to its output event runs
       once those releases have completed. */
    return NULL_GUID;
}

ocrGuid_t shutdown_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
    ocrShutdown();
    return NULL_GUID;
}
