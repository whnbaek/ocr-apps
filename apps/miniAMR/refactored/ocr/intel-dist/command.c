#include <stdlib.h>
#include <math.h>
#include <ocr.h>

#include "amr.h"

/* The refinement-driving object supplied when the argument vector names none.
 * With no object in the mesh no block ever changes level, so the published
 * one-object specification is the default rather than an empty list. */
#define DEFAULT_OBJECT_TYPE     2
#define DEFAULT_OBJECT_BOUNCE   0
#define DEFAULT_OBJECT_CENTER (-0.11)
#define DEFAULT_OBJECT_MOVE     0.04
#define DEFAULT_OBJECT_SIZE     1.7
#define DEFAULT_OBJECT_INC      0.0
#define DEFAULT_NUM_OBJECTS     1

void print_help_message(void)
{
  ocrPrintf("(Optional) command line input is of the form: \n\n");
  ocrPrintf("--nx - block size x (even && > 0)\n");
  ocrPrintf("--ny - block size y (even && > 0)\n");
  ocrPrintf("--nz - block size z (even && > 0)\n");
  ocrPrintf("--npx - number of blocks in the x direction, before any refinement\n");
  ocrPrintf("--npy - number of blocks in the y direction, before any refinement\n");
  ocrPrintf("--npz - number of blocks in the z direction, before any refinement\n");
  ocrPrintf("--num_refine - (>= 0, <= 15) number of levels of refinement\n");
  ocrPrintf("--block_change - (>= 0) number of levels a block can change in a timestep\n");
  ocrPrintf("--uniform_refine - if 1, then grid is uniformly refined\n");
  ocrPrintf("--refine_freq - frequency (in timesteps) of checking for refinement\n");
  ocrPrintf("--num_vars - number of variables (> 0)\n");
  ocrPrintf("--comm_vars - number of vars to communicate together\n");
  ocrPrintf("--num_tsteps - number of timesteps (> 0)\n");
  ocrPrintf("--stages_per_ts - number of comm/calc stages per timestep\n");
  ocrPrintf("--checksum_freq - number of stages between checksums\n");
  ocrPrintf("--stencil - 7 or 27 point (27 will not work with refinement (except uniform))\n");
  ocrPrintf("--error_tol - (e^{-error_tol} ; >= 0) \n");
  ocrPrintf("--report_diffusion - (>= 0) none if 0\n");
  ocrPrintf("--report_perf - 0, 1, 2\n");
  ocrPrintf("--plot_freq - frequency (timesteps) of plotting (0 for none)\n");
  ocrPrintf("--code - closely mimic communication of different applications (for which this is merely a proxy-app)\n");
  ocrPrintf("         0 minimal sends, 1 send ghosts, 2 send ghosts and process on send\n");
  ocrPrintf("--permute - altenates directions in communication\n");
  ocrPrintf("--refine_ghost - use full extent of block (including ghosts) to determine if block is refined\n");
  ocrPrintf("--num_objects - (>= 0) number of objects to cause refinement\n");
  ocrPrintf("--object - type, position, movement, size, size rate of change\n");
  ocrPrintf("--width - blocks emitted by one spawn-tree leaf\n");
  ocrPrintf("All associated settings are integers except for objects\n");
}

s32 check_input(control* c)
{
  s32 error = 0;

  if(c->nx < 1 || c->ny < 1 || c->nz < 1) {
    ocrPrintf("block size must be positive\n");
    error = 1;
  }
  if(((c->nx/2)*2) != c->nx) {
    ocrPrintf("block size in x direction must be even\n");
    error = 1;
  }
  if(((c->ny/2)*2) != c->ny) {
    ocrPrintf("block size in y direction must be even\n");
    error = 1;
  }
  if(((c->nz/2)*2) != c->nz) {
    ocrPrintf("block size in z direction must be even\n");
    error = 1;
  }
  if(c->num_refine < 0) {
    ocrPrintf("number of refinement levels must be non-negative\n");
    error = 1;
  }
  if(c->num_refine > AMR_MAX_LEVELS) {
    ocrPrintf("number of refinement levels must not exceed fifteen\n");
    error = 1;
  }
  if(c->block_change < 0) {
    ocrPrintf("number of refinement levels must be non-negative\n");
    error = 1;
  }
  if(c->num_vars < 1) {
    ocrPrintf("number of variables must be positive\n");
    error = 1;
  }
  if(c->stencil != 7 && c->stencil != 27) {
    ocrPrintf("illegal value for stencil\n");
    error = 1;
  }
  if(c->stencil == 27 && c->num_refine && !c->uniform_refine)
    ocrPrintf("WARNING: 27 point stencil with non-uniform refinement: answers may diverge\n");
  if(c->comm_vars == 0 || c->comm_vars > c->num_vars)
    c->comm_vars = c->num_vars;
  if(c->code < 0 || c->code > 2) {
    ocrPrintf("code must be 0, 1, or 2\n");
    error = 1;
  }
  if(c->code != 0) {
    ocrPrintf("only --code 0 (minimal sends) is implemented\n");
    error = 1;
  }
  if(c->plot_freq != 0) {
    ocrPrintf("--plot_freq is not implemented: this program writes no plot files\n");
    error = 1;
  }
  if(c->num_objects < 0) {
    ocrPrintf("number of objects must be non-negative\n");
    error = 1;
  }
  if(c->npx < 1 || c->npy < 1 || c->npz < 1) {
    ocrPrintf("number of blocks per axis must be positive\n");
    error = 1;
  }
  if(c->num_tsteps < 1) {
    ocrPrintf("number of timesteps must be positive\n");
    error = 1;
  }
  if(c->stages_per_ts < 1) {
    ocrPrintf("number of stages per timestep must be positive\n");
    error = 1;
  }
  if(c->refine_freq < 1) {
    ocrPrintf("refinement frequency must be positive\n");
    error = 1;
  }
  if(c->checksum_freq < 0) {
    ocrPrintf("checksum frequency must be non-negative\n");
    error = 1;
  }
  if(c->width < 1 || c->width > 1024) {
    ocrPrintf("spawn-tree leaf width must be in 1..1024\n");
    error = 1;
  }
  /* A block is named by a level-tagged linear position that has to stay inside
   * one 32-bit word, or two blocks at different levels would answer to the
   * same name and the directory would hand a face the wrong writer.  The bound
   * is the whole forest, `npx*npy*npz * (8^(num_refine+1)-1)/7`. */
  if(!error) {
    u64 span = 0, w = (u64)c->npx*c->npy*c->npz;
    for(s32 l = 0; l <= c->num_refine && span <= 0xFFFFFFFFull; ++l) {
      span += w;
      w <<= 3;
    }
    if(span > 0xFFFFFFFFull) {
      ocrPrintf("The mesh needs %llu block names for %d unrefined blocks at %d "
                "refinement levels, which exceeds the addressable range.\n",
                (unsigned long long)span, c->npx*c->npy*c->npz, c->num_refine);
      ocrPrintf("Reduce --num_refine or the block grid: the requirement is "
                "npx*npy*npz * (8^(num_refine+1)-1)/7 <= 4294967295.\n");
      error = 1;
    }
  }
  return error;
}

/* A flag in the last argument position has no value to read, and reading past
 * it is an out-of-bounds access of the vector, so the shortfall ends the run
 * rather than being parsed from whatever follows the vector in memory. */
static const char* arg_value(void* argv, u32 argc, u32* i, const char* flag, u8* bad)
{
  static const char zero[] = "0";
  if(*i + 1 >= argc) {
    ocrPrintf("** Error ** %s must be followed by a value.\n", flag);
    *bad = 1;
    return zero;
  }
  return ocrGetArgv(argv, (u64)++(*i));
}

u8 parse_command(u32 argc, void* argv, control* c, object** obj, ocrGuid_t* obj_db)
{
  u8 bad = 0;
  *obj_db = NULL_GUID;
  *obj = NULL;
  s32 num_objects = -9999;

  /* The object count is read first: it sizes the array the full parse
   * indexes, and the two passes must agree on the default. */
  for(u32 i = 1; i < argc; ++i) {
    char* flag = ocrGetArgv(argv, (u64)i);
    if(!strcmp(flag, "--num_objects")) {
      if(num_objects != -9999) {
        ocrPrintf("--num_objects should only appear once on the command line.\n");
        return 1;
      }
      num_objects = atoi(arg_value(argv, argc, &i, flag, &bad));
      if(num_objects < 0) {
        ocrPrintf("Number of objects must be non-negative.\n");
        return 1;
      }
    } else if(!strcmp(flag, "--object")) {
      i += 14;
    } else if(strncmp(flag, "--", 2) == 0 && strcmp(flag, "--permute")
              && strcmp(flag, "--refine_ghost") && strcmp(flag, "--help")) {
      ++i;
    }
  }
  if(bad) return 1;
  if(num_objects == -9999) num_objects = DEFAULT_NUM_OBJECTS;

  c->nx = c->ny = c->nz = 10;
  c->npx = c->npy = c->npz = 1;
  c->num_refine = 5;
  c->block_change = 0;
  c->uniform_refine = 0;
  c->refine_freq = 5;
  c->num_vars = 40;
  c->comm_vars = 0;
  c->num_tsteps = 20;
  c->stages_per_ts = 20;
  c->checksum_freq = 5;
  c->stencil = 7;
  c->error_tol = 8;
  c->report_diffusion = 0;
  c->report_perf = 4;
  c->plot_freq = 0;
  c->code = 0;
  c->permute = 0;
  c->refine_ghost = 0;
  c->num_objects = num_objects;
  c->width = 8;

  object* op;
  u64 bytes = (u64)(num_objects ? num_objects : 1)*sizeof(object);
  ocrDbCreate(obj_db, (void**)&op, bytes, 0, NULL_HINT, NO_ALLOC);
  memset(op, 0, bytes);
  *obj = op;

  s32 object_num = 0;
  s32 objects_from_argv = 0;

  for(u32 i = 1; i < argc; ++i) {
    char* flag = ocrGetArgv(argv, (u64)i);
    if(!strcmp(flag, "--nx")) {
      c->nx = atoi(arg_value(argv, argc, &i, flag, &bad));
    } else if(!strcmp(flag, "--ny")) {
      c->ny = atoi(arg_value(argv, argc, &i, flag, &bad));
    } else if(!strcmp(flag, "--nz")) {
      c->nz = atoi(arg_value(argv, argc, &i, flag, &bad));
    } else if(!strcmp(flag, "--npx")) {
      c->npx = atoi(arg_value(argv, argc, &i, flag, &bad));
    } else if(!strcmp(flag, "--npy")) {
      c->npy = atoi(arg_value(argv, argc, &i, flag, &bad));
    } else if(!strcmp(flag, "--npz")) {
      c->npz = atoi(arg_value(argv, argc, &i, flag, &bad));
    } else if(!strcmp(flag, "--num_refine")) {
      c->num_refine = atoi(arg_value(argv, argc, &i, flag, &bad));
    } else if(!strcmp(flag, "--block_change")) {
      c->block_change = atoi(arg_value(argv, argc, &i, flag, &bad));
    } else if(!strcmp(flag, "--uniform_refine")) {
      c->uniform_refine = atoi(arg_value(argv, argc, &i, flag, &bad));
    } else if(!strcmp(flag, "--refine_freq")) {
      c->refine_freq = atoi(arg_value(argv, argc, &i, flag, &bad));
    } else if(!strcmp(flag, "--num_vars")) {
      c->num_vars = atoi(arg_value(argv, argc, &i, flag, &bad));
    } else if(!strcmp(flag, "--comm_vars")) {
      c->comm_vars = atoi(arg_value(argv, argc, &i, flag, &bad));
    } else if(!strcmp(flag, "--num_tsteps")) {
      c->num_tsteps = atoi(arg_value(argv, argc, &i, flag, &bad));
    } else if(!strcmp(flag, "--stages_per_ts")) {
      c->stages_per_ts = atoi(arg_value(argv, argc, &i, flag, &bad));
    } else if(!strcmp(flag, "--checksum_freq")) {
      c->checksum_freq = atoi(arg_value(argv, argc, &i, flag, &bad));
    } else if(!strcmp(flag, "--stencil")) {
      c->stencil = atoi(arg_value(argv, argc, &i, flag, &bad));
    } else if(!strcmp(flag, "--error_tol")) {
      c->error_tol = atoi(arg_value(argv, argc, &i, flag, &bad));
    } else if(!strcmp(flag, "--report_diffusion")) {
      c->report_diffusion = atoi(arg_value(argv, argc, &i, flag, &bad));
    } else if(!strcmp(flag, "--report_perf")) {
      c->report_perf = atoi(arg_value(argv, argc, &i, flag, &bad));
    } else if(!strcmp(flag, "--plot_freq")) {
      c->plot_freq = atoi(arg_value(argv, argc, &i, flag, &bad));
    } else if(!strcmp(flag, "--code")) {
      c->code = atoi(arg_value(argv, argc, &i, flag, &bad));
    } else if(!strcmp(flag, "--width")) {
      c->width = (u32)atoi(arg_value(argv, argc, &i, flag, &bad));
    } else if(!strcmp(flag, "--permute")) {
      c->permute = 1;
    } else if(!strcmp(flag, "--refine_ghost")) {
      c->refine_ghost = 1;
    } else if(!strcmp(flag, "--num_objects")) {
      c->num_objects = atoi(arg_value(argv, argc, &i, flag, &bad));
      object_num = 0;
      objects_from_argv = 1;
    } else if(!strcmp(flag, "--object")) {
      objects_from_argv = 1;
      if(object_num >= c->num_objects) {
        ocrPrintf("object number greater than num_objects\n");
        return 1;
      }
      object* o = &op[object_num];
      o->type    = atoi(arg_value(argv, argc, &i, flag, &bad));
      o->bounce  = atoi(arg_value(argv, argc, &i, flag, &bad));
      o->cen[0]  = atof(arg_value(argv, argc, &i, flag, &bad));
      o->cen[1]  = atof(arg_value(argv, argc, &i, flag, &bad));
      o->cen[2]  = atof(arg_value(argv, argc, &i, flag, &bad));
      o->move[0] = atof(arg_value(argv, argc, &i, flag, &bad));
      o->move[1] = atof(arg_value(argv, argc, &i, flag, &bad));
      o->move[2] = atof(arg_value(argv, argc, &i, flag, &bad));
      o->size[0] = atof(arg_value(argv, argc, &i, flag, &bad));
      o->size[1] = atof(arg_value(argv, argc, &i, flag, &bad));
      o->size[2] = atof(arg_value(argv, argc, &i, flag, &bad));
      o->inc[0]  = atof(arg_value(argv, argc, &i, flag, &bad));
      o->inc[1]  = atof(arg_value(argv, argc, &i, flag, &bad));
      o->inc[2]  = atof(arg_value(argv, argc, &i, flag, &bad));
      ++object_num;
    } else if(!strcmp(flag, "--help")) {
      print_help_message();
      return 1;
    } else {
      ocrPrintf("** Error ** Unknown input parameter %s\n", flag);
      print_help_message();
      return 1;
    }
  }
  if(bad) return 1;

  /* Slots the command line named a count for but never described are inert,
   * which is the caller's stated intent; with no --object at all every slot
   * takes the published specification, so that the mesh refines. */
  for(s32 o = object_num; o < c->num_objects; ++o) {
    if(objects_from_argv) continue;
    op[o].type   = DEFAULT_OBJECT_TYPE;
    op[o].bounce = DEFAULT_OBJECT_BOUNCE;
    for(s32 j = 0; j < 3; ++j) {
      op[o].cen[j]  = DEFAULT_OBJECT_CENTER;
      op[o].move[j] = DEFAULT_OBJECT_MOVE;
      op[o].size[j] = DEFAULT_OBJECT_SIZE;
      op[o].inc[j]  = DEFAULT_OBJECT_INC;
    }
  }
  if(check_input(c)) return 1;
  if(!c->block_change) c->block_change = c->num_refine;

  c->tol = pow(10.0, (double)-c->error_tol);
  c->p2[0] = 1;
  for(s32 i = 0; i < c->num_refine + 1; ++i) c->p2[i+1] = c->p2[i]*2;
  s32 size = c->p2[c->num_refine+1];
  c->mesh_size[0] = c->npx*size;
  c->mesh_size[1] = c->npy*size;
  c->mesh_size[2] = c->npz*size;

  u64 x, y, z;
  splitDimension_Cart3D(c->nranks, &x, &y, &z);
  c->pd[0] = (u32)x; c->pd[1] = (u32)y; c->pd[2] = (u32)z;

  return 0;
}
