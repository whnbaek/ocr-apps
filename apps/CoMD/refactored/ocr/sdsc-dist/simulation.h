#ifndef COMD_H
#define COMD_H

#include <string.h>
#include <ocr.h>

#include "timers.h"
#include "command.h"

#define MAXATOMS 64
/// Children of an interior spawn-tree node.
#define FAN_BRANCH 8
/// Link cells one halo block carries.
#define HALO_CHUNK 64

// The units for this code are:
//     - Time in femtoseconds (fs)
//     - Length in Angstroms (Angs)
//     - Energy in electron Volts (eV)
//     - Force in eV/Angstrom
//     - Mass read in as Atomic Mass Units (amu) and then converted for
//       consistency (energy*time^2/length^2)
// Values are taken from NIST, http://physics.nist.gov/cuu/Constants/

/// 1 amu in kilograms
#define amu2Kilograms  1.660538921e-27

/// 1 fs in seconds
#define fs2seconds     1.0e-15

/// 1 Ang in meters
#define Angs2meters    1.0e-10

/// 1 eV in Joules
#define eV2Joules      1.602176565e-19

/// Internal mass units are eV * fs^2 / Ang^2
static const double amu2internal_mass =
         amu2Kilograms * Angs2meters * Angs2meters
         / (fs2seconds * fs2seconds  * eV2Joules);

/// Boltmann constant in eV's
static const double kB_eV = 8.6173324e-5;  // eV/K

/// Conversion to Temperature
static const double kB_eV_1_5 = 7736.34618837;  // K/eV

typedef double real_t;
#define FMT1 "%lf"
#define EMT1 "%lf"

typedef real_t real3[3];

typedef struct
{
  real_t sigma;
  real_t epsilon;
} lj_potential;

typedef struct
{
  real_t cutoff;         //!< potential cutoff distance in Angstroms
  real_t mass;           //!< mass of atoms in internal units
  real_t inv_mass_2;     //!< 1/(2 mass)
  real_t inv_mass_dt;    //!< dt/mass
  real_t lat;            //!< lattice spacing (angs) of unit cell
  char lattice_type[8];  //!< lattice type, e.g. FCC, BCC, etc.
  char name[3];          //!< element name
  u32 atomic_no;         //!< atomic number
  lj_potential lj;
} potential;

/// The atoms a cell held when a step began, positions as the drift left them:
/// an atom that drifted out of the cell stays in the block, and every reader
/// tells the cell an atom is in from its position.
typedef struct
{
  u32 atoms;          //!< total number of atoms in the cell
  u32 gid[MAXATOMS];  //!< A globally unique id for each atom
  real3 r[MAXATOMS];  //!< positions
  real3 p[MAXATOMS];  //!< momenta of atoms
} box;

/// A cell keeps two generations of its block: a step reads one, of its own
/// cell and of its 26 neighbours, and writes the other, so no block has a
/// reader and a writer in the same step.
typedef struct
{
  ocrGuid_t bx[2];
} cell_ref;

typedef struct
{
  u64 atoms;
  u64 max_occupancy;
  real_t p[3];   //!< sum of the momenta
  real_t p2;     //!< sum of |p|^2
} init_stats;

/// Produced bottom-up by the generation tree: a subrange's cells, the halo
/// blocks of the bricks it covers, and the whole-system sums taken over them.
/// The root's copy is the block directory every later task reads.  The three
/// arrays share one allocation in this order -- cells, halo blocks, and the
/// table saying where each brick's layers begin -- because only the root knows
/// the last of them and a slice must be able to carry the first two alone.
typedef struct
{
  init_stats st;
  u32 count;   //!< cells named
  u32 halo;    //!< halo blocks named
  u32 dirs;    //!< entries of the layer table, set once the whole grid is in
  cell_ref cell[];
} cell_list;

/// Everything a per-cell task reads and nothing writes after generation:
/// separating it from the mutable state keeps the wide phases off a block
/// whose ownership moves.
typedef struct
{
  u32 grid[3];         //!< number of link cells in each dimension
  u32 boxes_num;       //!< total number of link cells
  u32 unit[3];         //!< number of unit cells in each dimension
  u32 width;           //!< link cells per spawn-tree leaf
  u32 ranks[3];        //!< rank grid the cells are blocked over
  u32 nranks;
  real3 local_max;     //!< maximum bounds
  real3 box_size;      //!< size of a link cell in each dimension
  real3 inv_box_size;  //!< inverse size of a link cell in each dimension
  real3 shift[27];     //!< shift for periodic boundaries
  real_t dt;
  real_t lat;
  real_t temperature;
  real_t delta;
  potential pot;
} config;

typedef struct
{
  u32 step;             //!< current step
  u32 steps;            //!< number of time steps to run
  u32 period;           //!< number of steps between output
  u32 atoms;            //!< live atom count, refreshed by the energy reduction
  u32 max_occupancy;    //!< max number of atoms in a cell at generation
  real_t e_potential;
  real_t e_kinetic;
  double energy0;       //!< initial total energy per atom
  u32 atoms0;           //!< initial global number of atoms
} simulation;

/// One tree edge's contribution: the two energies and an atom count.
typedef struct
{
  real_t pe;
  real_t ke;
  u64 n;
} partial;

enum phase { PH_TEMP, PH_STEP, PH_HALO, PH_HALO_FREE, PH_RELEASE };

/// Argument words of a step: the generation it reads (it writes the other),
/// the kick applied before its energy point, the kick applied after it, and
/// whether the energy point is taken and whether the drift follows.
enum { SA_GEN, SA_KICK_PRE, SA_KICK_POST, SA_FLAGS, SA_NARG };
enum { SF_ENERGY = 1, SF_DRIFT = 2 };

#define GUID_WORDS ((sizeof(ocrGuid_t)+sizeof(u64)-1)/sizeof(u64))

static inline void guid_store(u64* w, ocrGuid_t g)
{
  memcpy(w, &g, sizeof(g));
}

static inline ocrGuid_t guid_load(const u64* w)
{
  ocrGuid_t g;
  memcpy(&g, w, sizeof(g));
  return g;
}

static inline u64 real_bits(real_t v)
{
  u64 u;
  memcpy(&u, &v, sizeof(u));
  return u;
}

static inline real_t bits_real(u64 u)
{
  real_t v;
  memcpy(&v, &u, sizeof(v));
  return v;
}

/// What a spawn-tree leaf needs to emit one phase's tasks for its own share
/// of a brick.  A leaf's range is a brick-local interval -- of cells, or of
/// the brick's halo blocks where the phase is one of those -- so everything it
/// names is owned by `rank` and every task it creates is placed there.
typedef struct
{
  ocrGuid_t cfg;
  const config* c;
  ocrGuid_t list;
  const cell_list* l;
  ocrGuid_t join;      //!< the node collecting this leaf's tasks
  const u64* arg;      //!< phase argument words
  u32 rank;            //!< owner of the brick the range lies in
  u32 blo[3];          //!< the brick's first cell on each axis
  u32 bdim[3];         //!< the brick's extent on each axis
} spawn_ctx;

void spawn_temp(const spawn_ctx* x, u32 lo, u32 hi);
void spawn_step(const spawn_ctx* x, u32 lo, u32 hi);
void spawn_halo(const spawn_ctx* x, u32 lo, u32 hi);
void spawn_halo_free(const spawn_ctx* x, u32 lo, u32 hi);
void spawn_release(const spawn_ctx* x, u32 lo, u32 hi);

void fan_launch(u32 phase, ocrGuid_t cfg, const config* c, ocrGuid_t list,
                ocrGuid_t parent, u32 slot, const u64* arg);

void init_launch(ocrGuid_t cfg, const config* c, ocrGuid_t parent, u32 slot);

void init_lj(potential* p, real_t dt);
real_t lj_cell_self(const potential* p, const box* a, real3* f);
real_t lj_cell_pair(const potential* p, const real3* shift, const box* a, real3* f,
                    const box* b, const u8* rel);
u8 init_config(command* cmd, config* c, simulation* s);

void print_status_header();
void print_status(const config* c, simulation* s, double time);
void print_simulation(const config* c, const simulation* s);
void validate_result(const simulation* sim);

#endif
