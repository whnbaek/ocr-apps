#ifndef TG_ARCH
#include <time.h>
#endif
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

ocrGuid_t init_done_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t init_force_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t main_edt2(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t main_edt3(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t top_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t bot_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
ocrGuid_t end_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

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
  ocrDbCreate(&timer_i, (void**)&timer_p, sizeof(timer)*number_of_timers, 0, NULL_HINT, NO_ALLOC);
  profile_start(total_timer, timer_p);
  timestamp("Starting Initialization\n");

  ocrGuid_t cmd_i;
  command* cmd_p;
  ocrDbCreate(&cmd_i, (void**)&cmd_p, sizeof(command), 0, NULL_HINT, NO_ALLOC);
  u8 bad_args = parse_command(argc, argv, cmd_p);
  ocrDbDestroy(args);
  if(bad_args) {
    ocrDbDestroy(cmd_i);
    ocrShutdown();
    return ERROR_GUID;
  }

  ocrGuid_t sim_i;
  simulation* sim_p;
  ocrDbCreate(&sim_i, (void**)&sim_p, sizeof(simulation), 0, NULL_HINT, NO_ALLOC);
  ocrGuid_t* list;
  u32 insane = init_simulation(cmd_p, sim_p, &list);
  ocrDbDestroy(cmd_i);
  if(insane) {
    ocrShutdown();
    return ERROR_GUID;
  }

  profile_start(redistribute_timer, timer_p);

  ocrGuid_t tmp,edt;
  ocrEdtTemplateCreate(&tmp, init_done_edt, 0, 4);
  ocrHint_t homeHNT;
  ocrEdtCreate(&edt, tmp, 0, NULL, 4, NULL, 0, comdHomeEdtHint(&homeHNT), NULL);
  ocrAddDependence(timer_i, edt, 0, DB_MODE_RW);
  ocrAddDependence(sim_i, edt, 1, DB_MODE_RW);
  ocrAddDependence(sim_p->bxs.list, edt, 2, DB_MODE_CONST);
  fork_generate_atoms(sim_i, edt, 3, list, sim_p->bxs.grid);
  ocrEdtTemplateDestroy(tmp);

  ocrDbDestroy(depv[0].guid);

  return NULL_GUID;
}

ocrGuid_t init_done_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  simulation* sim = (simulation*)depv[1].ptr;
  const init_stats* st = (const init_stats*)depv[3].ptr;
  profile_stop(redistribute_timer, depv[0].ptr);

  sim->bxs.atoms = st->atoms;
  sim->bxs.max_occupancy = st->max_occupancy;

  real3 vcm; vcm[0] = vcm[1] = vcm[2] = 0.0;
  real_t scale = 1.0;
  if(sim->bxs.temperature != 0.0 && st->atoms > 0) {
    real_t n = (real_t)st->atoms;
    for(u32 d = 0; d < 3; ++d) vcm[d] = st->p[d]/n;
    /* The kinetic energy left after the mean momentum is removed follows from
     * the same two sums, so no second pass over the atoms is needed. */
    real_t p2 = st->p2 - n*(vcm[0]*vcm[0]+vcm[1]*vcm[1]+vcm[2]*vcm[2]);
    real_t temp = ((sim->pot.inv_mass_2*p2)/n)*kB_eV_1_5;
    if(temp > 0.0) scale = sqrt(sim->bxs.temperature/temp);
  }
  ocrDbDestroy(depv[3].guid);

  ocrGuid_t tmp,edt;
  ocrEdtTemplateCreate(&tmp, init_force_edt, 0, 4);
  ocrHint_t homeHNT;
  ocrEdtCreate(&edt, tmp, 0, NULL, 4, NULL, 0, comdHomeEdtHint(&homeHNT), NULL);
  ocrAddDependence(depv[0].guid, edt, 0, DB_MODE_RW);
  ocrAddDependence(depv[1].guid, edt, 1, DB_MODE_CONST);
  ocrAddDependence(depv[2].guid, edt, 2, DB_MODE_CONST);
  fork_set_temperature(edt, 3, (ocrGuid_t*)depv[2].ptr, sim->bxs.grid, vcm, scale);
  ocrEdtTemplateDestroy(tmp);

  return NULL_GUID;
}

ocrGuid_t init_force_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  simulation* sim = (simulation*)depv[1].ptr;
  profile_start(compute_force_timer, depv[0].ptr);

  ocrGuid_t tmp,edt;
  ocrEdtTemplateCreate(&tmp, main_edt2, 0, 4);
  ocrHint_t homeHNT;
  ocrEdtCreate(&edt, tmp, 0, NULL, 4, NULL, 0, comdHomeEdtHint(&homeHNT), NULL);
  ocrAddDependence(depv[0].guid, edt, 0, DB_MODE_RW);
  ocrAddDependence(depv[1].guid, edt, 1, DB_MODE_CONST);
  ocrAddDependence(depv[2].guid, edt, 2, DB_MODE_CONST);
  sim->pot.fork_force_computation(depv[1].guid, sim, edt,
                                  (ocrGuid_t*)depv[2].ptr, sim->bxs.grid);
  ocrEdtTemplateDestroy(tmp);

  return NULL_GUID;
}

ocrGuid_t main_edt2(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  profile_stop(compute_force_timer, depv[0].ptr);

  ocrGuid_t tmp,edt;
  ocrEdtTemplateCreate(&tmp, main_edt3, 0, 3);
  ocrHint_t homeHNT;
  ocrEdtCreate(&edt, tmp, 0, NULL, 3, NULL, 0, comdHomeEdtHint(&homeHNT), NULL);
  ocrAddDependence(depv[0].guid, edt, 0, DB_MODE_RW);
  ocrAddDependence(depv[1].guid, edt, 1, DB_MODE_RW);
  fork_kinetic_energy(depv[1].guid, edt, 2, (ocrGuid_t*)depv[2].ptr, ((simulation*)depv[1].ptr)->bxs.grid);
  ocrEdtTemplateDestroy(tmp);

  return NULL_GUID;
}

ocrGuid_t main_edt3(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  simulation* sim = (simulation*) depv[1].ptr;
  print_simulation(sim);

  sim->energy0 = (sim->e_potential + sim->e_kinetic) / sim->bxs.atoms;
  sim->atoms0 = sim->bxs.atoms;
  ocrPrintf("\n\nInitial energy : %14.12f, atom count : %d \n\n", sim->energy0, sim->atoms0);

  timestamp("Initialization Finished\n");
  timestamp("Starting simulation\n");
  print_status_header();

  ocrGuid_t tmp,edt;
  ocrEdtTemplateCreate(&tmp, top_edt, 0, 3);
  ocrHint_t homeHNT;
  ocrEdtCreate(&edt, tmp, 0, NULL, 3, NULL, 0, comdHomeEdtHint(&homeHNT), NULL);
  ocrAddDependence(depv[0].guid, edt, 0, DB_MODE_RW);
  ocrAddDependence(depv[1].guid, edt, 1, DB_MODE_CONST);
  ocrAddDependence(sim->bxs.list, edt, 2, DB_MODE_CONST);
  ocrEdtTemplateDestroy(tmp);

  return NULL_GUID;
}

ocrGuid_t top_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  print_status(depv[1].ptr, get_elapsed_time(timestep_timer, depv[0].ptr));
  profile_start(loop_timer, depv[0].ptr);

  ocrGuid_t tmp,edt;
  ocrEdtTemplateCreate(&tmp, bot_edt, 0, 3);
  ocrHint_t homeHNT;
  ocrEdtCreate(&edt, tmp, 0, NULL, 3, NULL, 0, comdHomeEdtHint(&homeHNT), NULL);
  profile_start(timestep_timer, depv[0].ptr);
  ocrAddDependence(depv[0].guid, edt, 0, DB_MODE_RW);
  ocrAddDependence(depv[1].guid, edt, 1, DB_MODE_RW);
  timestep(depv[0].guid, depv[0].ptr, depv[1].guid, edt, depv[2].guid, (ocrGuid_t*)depv[2].ptr, ((simulation*)depv[1].ptr)->bxs.grid, ((simulation*)depv[1].ptr)->dt);
  ocrEdtTemplateDestroy(tmp);

  return NULL_GUID;
}

ocrGuid_t bot_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  profile_stop(timestep_timer, depv[0].ptr);
  simulation* sim = (simulation*) depv[1].ptr;

  ocrGuid_t tmp,edt;
  if(sim->step < sim->steps)
    ocrEdtTemplateCreate(&tmp, top_edt, 0, 3);
  else
    ocrEdtTemplateCreate(&tmp, end_edt, 0, 3);

  ocrHint_t homeHNT;
  ocrEdtCreate(&edt, tmp, 0, NULL, 3, NULL, 0, comdHomeEdtHint(&homeHNT), NULL);
  ocrAddDependence(depv[0].guid, edt, 0, DB_MODE_RW);
  ocrAddDependence(depv[1].guid, edt, 1, DB_MODE_CONST);
  ocrAddDependence(sim->bxs.list, edt, 2, DB_MODE_CONST);
  ocrEdtTemplateDestroy(tmp);

  return NULL_GUID;
}

ocrGuid_t end_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[])
{
  profile_stop(loop_timer, depv[0].ptr);
  print_status(depv[1].ptr, get_elapsed_time(timestep_timer, depv[0].ptr));
  timestamp("Ending simulation\n");

  validate_result(depv[1].ptr);
  profile_stop(total_timer, depv[0].ptr);

  print_timers(depv[0].ptr, ((simulation*) depv[1].ptr)->atoms0);
  destroy_simulation(depv[1].ptr, depv[2].ptr);
  ocrDbDestroy(depv[0].guid);
  ocrDbDestroy(depv[1].guid);

  timestamp("CoMD Ending\n");
  ocrShutdown();

  return NULL_GUID;
}
