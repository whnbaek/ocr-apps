#ifndef TG_ARCH
#include <time.h>
#endif
#include <math.h>
#include <stdio.h>
#include <ocr.h>

#include "timers.h"
#include "command.h"
#include "simulation.h"
#include "cells.h"

static void timestamp(const char* msg)
{
#ifdef TG_ARCH
  ocrPrintf(msg);
#else
  time_t t= time(NULL);
  char* timeString = ctime(&t);
  timeString[24] = '\0';
  ocrPrintf("%s: %s\n", timeString, msg);
#endif
}

/* Spine slots: 0 timer, 1 simulation state, 2 configuration, 3 the cell
 * directory, 4 (where present) the phase that must finish first. */
enum { SL_TIMER, SL_SIM, SL_CFG, SL_LIST, SL_PHASE, SL_DEPC };

/* The task between a step's two phases: it holds the step's arguments and the
 * final its cell tasks report to, and nothing else, because everything it
 * needs of the run is already in the configuration and the directory. */
enum { HS_CFG, HS_LIST, HS_PHASE, HS_DEPC };
enum { HS_ARG, HS_PARENT = HS_ARG + SA_NARG, HS_NPARAM = HS_PARENT + GUID_WORDS };

static ocrGuid_t init_done_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
static ocrGuid_t after_temp_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
static ocrGuid_t after_step0_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
static ocrGuid_t top_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
static ocrGuid_t step_next_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
static ocrGuid_t bot_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
static ocrGuid_t end_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
static ocrGuid_t release_done_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
static ocrGuid_t step_final_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
static ocrGuid_t halo_done_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
static ocrGuid_t halo_freed_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

static ocrGuid_t spine(ocrEdt_t fn, u32 depc, u32 paramc, u64* pv, const config* c,
                       ocrGuid_t timer, ocrGuid_t sim, ocrDbAccessMode_t sim_mode,
                       ocrGuid_t cfg, ocrGuid_t list)
{
  ocrGuid_t tmp, edt;
  ocrHint_t edtHNT;
  ocrEdtTemplateCreate(&tmp, fn, paramc, depc);
  ocrEdtCreate(&edt, tmp, paramc, pv, depc, NULL, 0, edt_at(&edtHNT, c, 0), NULL);
  ocrEdtTemplateDestroy(tmp);
  ocrAddDependence(timer, edt, SL_TIMER, DB_MODE_RW);
  ocrAddDependence(sim, edt, SL_SIM, sim_mode);
  ocrAddDependence(cfg, edt, SL_CFG, DB_MODE_CONST);
  ocrAddDependence(list, edt, SL_LIST, DB_MODE_CONST);
  return edt;
}

/* A reduction's last edge: it writes the whole-system scalars and its own
 * completion is what the next control task waits for. */
static ocrGuid_t final_create(ocrEdt_t fn, const config* c, ocrGuid_t sim,
                              ocrGuid_t next, u32 slot, u32 paramc, u64* pv)
{
  ocrGuid_t tmp, edt, done;
  ocrHint_t edtHNT;
  OEVT_COUNTED_PRE(done);
  ocrEdtTemplateCreate(&tmp, fn, paramc, 2);
  ocrEdtCreate(&edt, tmp, paramc, pv, 2, NULL, OEVT_COUNTED_PROP(0),
               edt_at(&edtHNT, c, 0), &done);
  ocrEdtTemplateDestroy(tmp);
  ocrAddDependence(done, next, slot, DB_MODE_CONST);
  ocrAddDependence(sim, edt, 0, DB_MODE_RW);
  return edt;
}

/* Launch step k, whose final reports to `next`.  Inside a block the two half
 * kicks that meet between consecutive force evaluations are one kick of dt;
 * at a block's end the step kicks by dt/2, takes its energy point and kicks
 * by dt/2 into the next block; step 0 has no kick before its energy point
 * and the last step of the run stops at its energy point.
 *
 * Across bricks a step is two phases: the halo of the generation it reads is
 * packed and pulled first, so its cell tasks find every neighbour at home and
 * a boundary cell costs the fabric nothing of its own.  On one rank there is
 * no halo and the step is the one phase it always was. */
static void step_launch(ocrEdtDep_t depv[], const config* c, u32 k, u8 end, u8 last,
                        ocrGuid_t next)
{
  real_t dt = c->dt;
  u64 fpv[1] = { k };
  ocrGuid_t fin = final_create(step_final_edt, c, depv[SL_SIM].guid, next, SL_PHASE, 1, fpv);
  u64 arg[SA_NARG];
  arg[SA_GEN] = k & 1;
  arg[SA_KICK_PRE] = real_bits(k == 0 ? 0.0 : (end ? 0.5*dt : dt));
  arg[SA_KICK_POST] = real_bits(end && !last ? 0.5*dt : 0.0);
  arg[SA_FLAGS] = (end ? SF_ENERGY : 0) | (last ? 0 : SF_DRIFT);
  profile_start(compute_force_timer, depv[SL_TIMER].ptr);
  ocrDbRelease(depv[SL_TIMER].guid);
  ocrDbRelease(depv[SL_SIM].guid);
  if(c->nranks <= 1) {
    fan_launch(PH_STEP, depv[SL_CFG].guid, c, depv[SL_LIST].guid, fin, 1, arg);
    return;
  }

  u64 pv[HS_NPARAM];
  for(u32 j = 0; j < SA_NARG; ++j) pv[HS_ARG+j] = arg[j];
  guid_store(pv+HS_PARENT, fin);
  ocrGuid_t tmp, edt;
  ocrHint_t edtHNT;
  ocrEdtTemplateCreate(&tmp, halo_done_edt, HS_NPARAM, HS_DEPC);
  ocrEdtCreate(&edt, tmp, HS_NPARAM, pv, HS_DEPC, NULL, 0,
               edt_at(&edtHNT, c, 0), NULL);
  ocrEdtTemplateDestroy(tmp);
  ocrAddDependence(depv[SL_CFG].guid, edt, HS_CFG, DB_MODE_CONST);
  ocrAddDependence(depv[SL_LIST].guid, edt, HS_LIST, DB_MODE_CONST);
  fan_launch(PH_HALO, depv[SL_CFG].guid, c, depv[SL_LIST].guid, edt, HS_PHASE, arg);
}

/* Issue the block's next step.  Its final hands the reduced scalars to the
 * control task that follows: another step of the block, or the block's end. */
static void step_issue(ocrEdtDep_t depv[], const config* c, const simulation* sim,
                       ocrGuid_t bot)
{
  u32 k = sim->step + 1;
  u8 end = (k % sim->period) == 0;
  u8 last = end && k >= sim->steps;
  ocrGuid_t next = bot;
  if(!end) {
    u64 pv[GUID_WORDS];
    guid_store(pv, bot);
    next = spine(step_next_edt, SL_DEPC, GUID_WORDS, pv, c,
                 depv[SL_TIMER].guid, depv[SL_SIM].guid, DB_MODE_CONST,
                 depv[SL_CFG].guid, depv[SL_LIST].guid);
  }
  step_launch(depv, c, k, end, last, next);
}

ocrGuid_t mainEdt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  u64 argc = ocrGetArgc(depv[0].ptr);
  ocrGuid_t args;
  char** argv;
  ocrDbCreate(&args, (void**)&argv, sizeof(char*)*argc, 0, NULL_HINT, NO_ALLOC);
  for(u32 a = 0; a < argc; ++a)
    argv[a] = ocrGetArgv(depv[0].ptr,a);

  ocrGuid_t timer_i;
  timer* timer_p;
  ocrDbCreate(&timer_i, (void**)&timer_p, sizeof(timer)*number_of_timers, 0,
              NULL_HINT, NO_ALLOC);
  profile_start(total_timer, timer_p);
  timestamp("Starting Initialization\n");

  ocrGuid_t cmd_i;
  command* cmd_p;
  ocrDbCreate(&cmd_i, (void**)&cmd_p, sizeof(command), 0, NULL_HINT, NO_ALLOC);
  parse_command(argc, argv, cmd_p);
  ocrDbDestroy(args);

  ocrGuid_t cfg_i, sim_i;
  config* cfg_p;
  simulation* sim_p;
  ocrDbCreate(&cfg_i, (void**)&cfg_p, sizeof(config), 0, NULL_HINT, NO_ALLOC);
  ocrDbCreate(&sim_i, (void**)&sim_p, sizeof(simulation), 0, NULL_HINT, NO_ALLOC);
  u8 insane = init_config(cmd_p, cfg_p, sim_p);
  ocrDbDestroy(cmd_i);
  if(insane) {
    ocrShutdown();
    return ERROR_GUID;
  }

  profile_start(redistribute_timer, timer_p);
  ocrDbRelease(timer_i);
  ocrDbRelease(sim_i);

  ocrGuid_t tmp, edt;
  ocrHint_t edtHNT;
  ocrEdtTemplateCreate(&tmp, init_done_edt, 0, SL_PHASE);
  ocrEdtCreate(&edt, tmp, 0, NULL, SL_PHASE, NULL, 0, edt_at(&edtHNT, cfg_p, 0), NULL);
  ocrEdtTemplateDestroy(tmp);
  ocrAddDependence(timer_i, edt, SL_TIMER, DB_MODE_RW);
  ocrAddDependence(sim_i, edt, SL_SIM, DB_MODE_RW);
  ocrAddDependence(cfg_i, edt, SL_CFG, DB_MODE_CONST);
  init_launch(cfg_i, cfg_p, edt, SL_LIST);

  ocrDbDestroy(depv[0].guid);

  return NULL_GUID;
}

ocrGuid_t init_done_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  simulation* sim = (simulation*)depv[SL_SIM].ptr;
  const config* c = (const config*)depv[SL_CFG].ptr;
  const cell_list* cl = (const cell_list*)depv[SL_LIST].ptr;
  profile_stop(redistribute_timer, depv[SL_TIMER].ptr);

  sim->atoms = (u32)cl->st.atoms;
  sim->atoms0 = sim->atoms;
  sim->max_occupancy = (u32)cl->st.max_occupancy;

  real_t vcm[3] = {0.0, 0.0, 0.0};
  real_t scale = 1.0;
  if(c->temperature != 0.0 && cl->st.atoms > 0) {
    real_t n = (real_t)cl->st.atoms;
    for(u32 d = 0; d < 3; ++d) vcm[d] = cl->st.p[d]/n;
    /* The kinetic energy left after the mean momentum is removed follows from
     * the same two sums, so no second pass over the atoms is needed. */
    real_t p2 = cl->st.p2 - n*(vcm[0]*vcm[0]+vcm[1]*vcm[1]+vcm[2]*vcm[2]);
    real_t e_kinetic = c->pot.inv_mass_2*p2;
    real_t temp = (e_kinetic/n)*kB_eV_1_5;
    if(temp > 0.0) scale = sqrt(c->temperature/temp);
  }

  ocrDbRelease(depv[SL_TIMER].guid);
  ocrDbRelease(depv[SL_SIM].guid);
  ocrGuid_t next = spine(after_temp_edt, SL_DEPC, 0, NULL, c,
                         depv[SL_TIMER].guid, depv[SL_SIM].guid, DB_MODE_CONST,
                         depv[SL_CFG].guid, depv[SL_LIST].guid);
  u64 arg[4];
  arg[0] = real_bits(vcm[0]); arg[1] = real_bits(vcm[1]);
  arg[2] = real_bits(vcm[2]); arg[3] = real_bits(scale);
  fan_launch(PH_TEMP, depv[SL_CFG].guid, c, depv[SL_LIST].guid, next, SL_PHASE, arg);

  return NULL_GUID;
}

ocrGuid_t after_temp_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  const config* c = (const config*)depv[SL_CFG].ptr;

  ocrGuid_t next = spine(after_step0_edt, SL_DEPC, 0, NULL, c,
                         depv[SL_TIMER].guid, depv[SL_SIM].guid, DB_MODE_RW,
                         depv[SL_CFG].guid, depv[SL_LIST].guid);
  step_launch(depv, c, 0, 1, 0, next);

  return NULL_GUID;
}

ocrGuid_t after_step0_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  simulation* sim = (simulation*)depv[SL_SIM].ptr;
  const config* c = (const config*)depv[SL_CFG].ptr;
  profile_stop(compute_force_timer, depv[SL_TIMER].ptr);

  print_simulation(c, sim);
  sim->energy0 = (sim->e_potential + sim->e_kinetic) / sim->atoms;
  sim->atoms0 = sim->atoms;
  ocrPrintf("\n\nInitial energy : %14.12f, atom count : %d \n\n", sim->energy0, sim->atoms0);

  timestamp("Initialization Finished\n");
  timestamp("Starting simulation\n");
  print_status_header();

  config cc = *c;
  ocrDbRelease(depv[SL_TIMER].guid);
  ocrDbRelease(depv[SL_SIM].guid);
  ocrDbRelease(depv[SL_CFG].guid);
  ocrDbRelease(depv[SL_LIST].guid);
  spine(top_edt, SL_PHASE, 0, NULL, &cc,
        depv[SL_TIMER].guid, depv[SL_SIM].guid, DB_MODE_CONST,
        depv[SL_CFG].guid, depv[SL_LIST].guid);

  return NULL_GUID;
}

ocrGuid_t top_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  simulation* sim = (simulation*)depv[SL_SIM].ptr;
  const config* c = (const config*)depv[SL_CFG].ptr;

  print_status(c, sim, get_elapsed_time(timestep_timer, depv[SL_TIMER].ptr));
  profile_start(loop_timer, depv[SL_TIMER].ptr);

  ocrGuid_t bot = spine(bot_edt, SL_DEPC, 0, NULL, c,
                        depv[SL_TIMER].guid, depv[SL_SIM].guid, DB_MODE_CONST,
                        depv[SL_CFG].guid, depv[SL_LIST].guid);

  profile_start(timestep_timer, depv[SL_TIMER].ptr);
  step_issue(depv, c, sim, bot);

  return NULL_GUID;
}

ocrGuid_t step_next_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  const simulation* sim = (const simulation*)depv[SL_SIM].ptr;
  const config* c = (const config*)depv[SL_CFG].ptr;
  profile_stop(compute_force_timer, depv[SL_TIMER].ptr);

  step_issue(depv, c, sim, guid_load(paramv));

  return NULL_GUID;
}

ocrGuid_t bot_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  const simulation* sim = (const simulation*)depv[SL_SIM].ptr;
  const config* c = (const config*)depv[SL_CFG].ptr;
  profile_stop(compute_force_timer, depv[SL_TIMER].ptr);
  profile_stop(timestep_timer, depv[SL_TIMER].ptr);

  ocrEdt_t fn = sim->step < sim->steps ? top_edt : end_edt;
  config cc = *c;
  ocrDbRelease(depv[SL_TIMER].guid);
  ocrDbRelease(depv[SL_SIM].guid);
  ocrDbRelease(depv[SL_CFG].guid);
  ocrDbRelease(depv[SL_LIST].guid);
  spine(fn, SL_PHASE, 0, NULL, &cc,
        depv[SL_TIMER].guid, depv[SL_SIM].guid, DB_MODE_CONST,
        depv[SL_CFG].guid, depv[SL_LIST].guid);

  return NULL_GUID;
}

/* A step's cell tasks, once the halo of the generation they read is packed
 * and in the cache of every brick that reads it. */
ocrGuid_t halo_done_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  const config* c = (const config*)depv[HS_CFG].ptr;
  fan_launch(PH_STEP, depv[HS_CFG].guid, c, depv[HS_LIST].guid,
             guid_load(paramv+HS_PARENT), 1, paramv+HS_ARG);

  return NULL_GUID;
}

/* The kinetic energy is reduced only by a step that took its energy point;
 * any other step's reduction leaves the last one standing. */
ocrGuid_t step_final_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  simulation* sim = (simulation*)depv[0].ptr;
  const partial* p = (const partial*)depv[1].ptr;
  u32 k = (u32)paramv[0];
  sim->e_potential = p->pe;
  if(k % sim->period == 0) sim->e_kinetic = p->ke;
  sim->atoms = (u32)p->n;
  sim->step = k;
  ocrDbDestroy(depv[1].guid);

  return NULL_GUID;
}

ocrGuid_t end_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  simulation* sim = (simulation*)depv[SL_SIM].ptr;
  const config* c = (const config*)depv[SL_CFG].ptr;

  profile_stop(loop_timer, depv[SL_TIMER].ptr);
  print_status(c, sim, get_elapsed_time(timestep_timer, depv[SL_TIMER].ptr));
  timestamp("Ending simulation\n");

  validate_result(sim);
  profile_stop(total_timer, depv[SL_TIMER].ptr);
  print_timers(depv[SL_TIMER].ptr, sim->atoms0);

  /* Teardown is two phases where there is a halo: the blocks that carried it
   * go back first, then the cells, each reclaimed by the rank that owns it. */
  u8 halo = c->nranks > 1;
  ocrDbRelease(depv[SL_TIMER].guid);
  ocrDbRelease(depv[SL_SIM].guid);
  ocrGuid_t next = spine(halo ? halo_freed_edt : release_done_edt, SL_DEPC, 0, NULL, c,
                         depv[SL_TIMER].guid, depv[SL_SIM].guid, DB_MODE_CONST,
                         depv[SL_CFG].guid, depv[SL_LIST].guid);
  fan_launch(halo ? PH_HALO_FREE : PH_RELEASE, depv[SL_CFG].guid, c,
             depv[SL_LIST].guid, next, SL_PHASE, NULL);

  return NULL_GUID;
}

ocrGuid_t halo_freed_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  const config* c = (const config*)depv[SL_CFG].ptr;

  ocrDbRelease(depv[SL_TIMER].guid);
  ocrDbRelease(depv[SL_SIM].guid);
  ocrGuid_t next = spine(release_done_edt, SL_DEPC, 0, NULL, c,
                         depv[SL_TIMER].guid, depv[SL_SIM].guid, DB_MODE_CONST,
                         depv[SL_CFG].guid, depv[SL_LIST].guid);
  fan_launch(PH_RELEASE, depv[SL_CFG].guid, c, depv[SL_LIST].guid, next, SL_PHASE, NULL);

  return NULL_GUID;
}

ocrGuid_t release_done_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  ocrDbDestroy(depv[SL_LIST].guid);
  ocrDbDestroy(depv[SL_CFG].guid);
  ocrDbDestroy(depv[SL_SIM].guid);
  ocrDbDestroy(depv[SL_TIMER].guid);

  timestamp("CoMD Ending\n");
  ocrShutdown();

  return NULL_GUID;
}
