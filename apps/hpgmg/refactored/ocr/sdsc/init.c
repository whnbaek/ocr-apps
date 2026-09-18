#include <ocr.h>
#include <string.h>
#include <math.h>
#include <extensions/ocr-affinity.h>

#ifdef TG_ARCH
#include "strings.h"
#endif

#include "hpgmg.h"
#include "utils.h"
#include "operators.h"

void evaluateBeta(double x, double y, double z, double *B, double *Bx, double *By, double *Bz);
void evaluateU(double x, double y, double z, double *U, double *Ux, double *Uy, double *Uz, double *Uxx, double *Uyy, double *Uzz, int isPeriodic);

//------------------------------------------------------------------------------------------------------------------------------
int IterativeSolver_NumVectors(){
  // additionally number of grids required by an iterative solver...
  #ifdef USE_BICGSTAB
  return(6);                  // BiCGStab requires additional grids r0,r,p,s,Ap,As
  #elif  USE_CG
  return(4);                  // CG requires extra grids r0,r,p,Ap
  #endif
  return(0);                  // simply doing multiple smooths requires no extra grids
}
//------------------------------------------------------------------------------------------------------------------------------



void hpgmg_valid_box(level_type *level, box_type *bx) {
  int i,j,k;
  int jStride = level->jStride;
  int kStride = level->kStride;
  int  ghosts = NUM_GHOSTS;
  int     dim = level->box_dim;
  double * __restrict__ valid = (double*)(((char*)bx)+ level->valid) + ghosts*(1+jStride+kStride);
  for(k=-ghosts;k<dim+ghosts;k++){
  for(j=-ghosts;j<dim+ghosts;j++){
  for(i=-ghosts;i<dim+ghosts;i++){
    int ijk = i + j*jStride + k*kStride;
    valid[ijk] = 1.0; // i.e. all cells including ghosts are valid for periodic BC's
    if(level->boundary_condition == BC_DIRICHLET){ // cells outside the domain boundaries are not valid
      if(i + bx->low.i <             0)valid[ijk] = 0.0;
      if(j + bx->low.j <             0)valid[ijk] = 0.0;
      if(k + bx->low.k <             0)valid[ijk] = 0.0;
      if(i + bx->low.i >= level->dim.i)valid[ijk] = 0.0;
      if(j + bx->low.j >= level->dim.j)valid[ijk] = 0.0;
      if(k + bx->low.k >= level->dim.k)valid[ijk] = 0.0;
    }
  }}}
}

#ifdef OCR_APP_OPTIMIZED_PLACEMENT
/* Factor P into a near-cubic PX*PY*PZ grid (PX*PY*PZ == P). */
void boxFactor3(s64 P, int *px, int *py, int *pz) {
  int z = 1, i;
  for (i = 1; (s64)i*i*i <= P; i++) if (P % i == 0) z = i;
  s64 rem = P / z; int y = 1;
  for (i = 1; (s64)i*i <= rem; i++) if (rem % i == 0) y = i;
  *pz = z; *py = y; *px = (int)(rem / y);
}

/* Spatial home for a box: the box's normalized position (i,j,k)/S runs every
 * level through ONE partition of the unit cube into a near-cubic rank grid,
 * so neighbouring boxes of a level co-locate AND a coarse box lands on the
 * rank of the fine boxes it restricts from / prolongs to — the inter-level
 * transfers stay rank-local.  A round-robin map scatters both. */
int boxHomePD(int box_num, int S, s64 P) {
  if (P <= 1 || S <= 0) return 0;
  int PX, PY, PZ;
  boxFactor3(P, &PX, &PY, &PZ);
  int i = box_num % S, j = (box_num / S) % S, k = box_num / (S * S);
  int px = (i * PX) / S; if (px >= PX) px = PX - 1;
  int py = (j * PY) / S; if (py >= PY) py = PY - 1;
  int pz = (k * PZ) / S; if (pz >= PZ) pz = PZ - 1;
  return px + py * PX + pz * PX * PY;
}
#endif

void hpgmg_fill_box(level_type *level, box_type *bx, double a, double b) {

  double hLevel = level->h;
  double *alpha, *beta_i, *beta_j, *beta_k, *u_true, *f;

  alpha = (double*)(((char*)bx)+ level->alpha);
  beta_i =  (double*)(((char*)bx)+ level->beta_i);
  beta_j =  (double*)(((char*)bx)+ level->beta_j);
  beta_k =  (double*)(((char*)bx)+ level->beta_k);
  u_true = (double*)(((char*)bx)+ level->u_true);
  f = (double*)(((char*)bx)+ level->f);
  bzero(alpha, (level->volume)*sizeof(double));
  bzero(beta_i, (level->volume)*sizeof(double));
  bzero(beta_j, (level->volume)*sizeof(double));
  bzero(beta_k, (level->volume)*sizeof(double));
  bzero(u_true, (level->volume)*sizeof(double));
  bzero(f, (level->volume)*sizeof(double));

  int i,j,k;
  int jStride = level->jStride;
  int kStride = level->kStride;
  int ghosts = NUM_GHOSTS;
  int dim_i = level->box_dim;
  int dim_j =  level->box_dim;
  int dim_k =  level->box_dim;

  for(k=0;k<=dim_k;k++) {
    for(j=0;j<=dim_j;j++) {
      for(i=0;i<=dim_i;i++) {
         int ijk = (i+ghosts) + (j+ghosts)*jStride + (k+ghosts)*kStride;
         double x = hLevel*( (double)(i+bx->low.i) + 0.5 );
         double y = hLevel*( (double)(j+bx->low.j) + 0.5 );
         double z = hLevel*( (double)(k+bx->low.k) + 0.5 );
         double A,B,Bx,By,Bz,Bi,Bj,Bk;
         double U,Ux,Uy,Uz,Uxx,Uyy,Uzz;

         A=B=Bi=Bj=Bk=1.0; Bx=By=Bz=0.0;
         #ifdef STENCIL_VARIABLE_COEFFICIENT
         evaluateBeta(x-hLevel*0.5,y           ,z           ,&Bi,&Bx,&By,&Bz);
         evaluateBeta(x           ,y-hLevel*0.5,z           ,&Bj,&Bx,&By,&Bz);
         evaluateBeta(x           ,y           ,z-hLevel*0.5,&Bk,&Bx,&By,&Bz);
         evaluateBeta(x           ,y           ,z           ,&B ,&Bx,&By,&Bz);
         #endif

         evaluateU(x,y,z,&U,&Ux,&Uy,&Uz,&Uxx,&Uyy,&Uzz, (level->boundary_condition == BC_PERIODIC) );
         double F1 = a*A*U - b*( (Bx*Ux + By*Uy + Bz*Uz)  +  B*(Uxx + Uyy + Uzz) );

         beta_i[ijk] = Bi;
         beta_j[ijk] = Bj;
         beta_k[ijk] = Bk;
         alpha[ijk] = A;
         u_true[ijk] = U;
         f[ijk] = F1;

       }
     }
   }
}

void evaluateBeta(double x, double y, double z, double *B, double *Bx, double *By, double *Bz){
  double Bmin =  1.0;
  double Bmax = 10.0;
  double c2 = (Bmax-Bmin)/2; // coefficients to affect this transition
  double c1 = (Bmax+Bmin)/2;
  double c3 = 10.0;          // how sharply (B)eta transitions
  double xcenter = 0.50;
  double ycenter = 0.50;
  double zcenter = 0.50;
  // calculate distance from center of the domain (0.5,0.5,0.5)
  double r2   = pow((x-xcenter),2) +  pow((y-ycenter),2) +  pow((z-zcenter),2);
  double r2x  = 2.0*(x-xcenter);
  double r2y  = 2.0*(y-ycenter);
  double r2z  = 2.0*(z-zcenter);
  double r    = pow(r2,0.5);
  double rx   = 0.5*r2x*pow(r2,-0.5);
  double ry   = 0.5*r2y*pow(r2,-0.5);
  double rz   = 0.5*r2z*pow(r2,-0.5);
/*  *B  =           c1+c2*tanh( c3*(r-0.25) );
  *Bx = c2*c3*rx*(1-pow(tanh( c3*(r-0.25) ),2));
  *By = c2*c3*ry*(1-pow(tanh( c3*(r-0.25) ),2));
  *Bz = c2*c3*rz*(1-pow(tanh( c3*(r-0.25) ),2));
*/
#ifndef TG_ARCH
  *B  =           c1+c2*tanh( c3*(r-0.25) );
  *Bx = c2*c3*rx*(1-pow(tanh( c3*(r-0.25) ),2));
  *By = c2*c3*ry*(1-pow(tanh( c3*(r-0.25) ),2));
  *Bz = c2*c3*rz*(1-pow(tanh( c3*(r-0.25) ),2));
#else
  *B  =           c1+c2*tanh_approx( c3*(r-0.25) );
  *Bx = c2*c3*rx*(1-pow(tanh_approx( c3*(r-0.25) ),2));
  *By = c2*c3*ry*(1-pow(tanh_approx( c3*(r-0.25) ),2));
  *Bz = c2*c3*rz*(1-pow(tanh_approx( c3*(r-0.25) ),2));
#endif

}


void evaluateU(double x, double y, double z, double *U, double *Ux, double *Uy, double *Uz, double *Uxx, double *Uyy, double *Uzz, int isPeriodic){
  // should be continuous in u, u', and u''
  // v(w) = w^4 - 2w^3 + w^2 + c
  // u(x,y,z) = v(x)v(y)v(z)
  // If Periodic, then the integral of the RHS should sum to zero.
  //   Setting shift=1/30 should ensure that the integrals of X, Y, or Z should sum to zero...
  //   That should(?) make the integrals of u,ux,uy,uz,uxx,uyy,uzz sum to zero and thus make the integral of f sum to zero
  // If dirichlet, then w(0)=w(1) = 0.0
  //   Setting shift to 0 should ensure that U(x,y,z) = 0 on boundary
  double shift = 0.0;if(isPeriodic)shift= -1.0/30.0;
  double X   =  1.0*pow(x,4) -  2.0*pow(x,3) + 1.0*pow(x,2) + shift;
  double Y   =  1.0*pow(y,4) -  2.0*pow(y,3) + 1.0*pow(y,2) + shift;
  double Z   =  1.0*pow(z,4) -  2.0*pow(z,3) + 1.0*pow(z,2) + shift;
  double Xx  =  4.0*pow(x,3) -  6.0*pow(x,2) + 2.0*x;
  double Yy  =  4.0*pow(y,3) -  6.0*pow(y,2) + 2.0*y;
  double Zz  =  4.0*pow(z,3) -  6.0*pow(z,2) + 2.0*z;
  double Xxx = 12.0*pow(x,2) - 12.0*x        + 2.0;
  double Yyy = 12.0*pow(y,2) - 12.0*y        + 2.0;
  double Zzz = 12.0*pow(z,2) - 12.0*z        + 2.0;
        *U   = X*Y*Z;
        *Ux  = Xx*Y*Z;
        *Uy  = X*Yy*Z;
        *Uz  = X*Y*Zz;
        *Uxx = Xxx*Y*Z;
        *Uyy = X*Yyy*Z;
        *Uzz = X*Y*Zzz;
}



// Writes the box's Dinv/L1inv and returns its Gershgorin eigenvalue bound.
double hpgmg_gershgorin_box(level_type *level, box_type *bx, double a, double b) {
  int i,j,k;
  int jStride = level->jStride;
  int kStride = level->kStride;
  int  ghosts = NUM_GHOSTS;
  int     dim = level->box_dim;
  double h2inv = 1.0/(level->h*level->h);
  double *alpha, *beta_i, *beta_j, *beta_k, *Dinv, *L1inv, *valid;
  alpha = (double*)(((char*)bx)+ level->alpha) + ghosts*(1+jStride+kStride);
  beta_i =  (double*)(((char*)bx)+ level->beta_i) + ghosts*(1+jStride+kStride);
  beta_j =  (double*)(((char*)bx)+ level->beta_j) + ghosts*(1+jStride+kStride);
  beta_k =  (double*)(((char*)bx)+ level->beta_k) + ghosts*(1+jStride+kStride);
  Dinv = (double*)(((char*)bx)+ level->Dinv) + ghosts*(1+jStride+kStride);
  L1inv = (double*)(((char*)bx)+ level->L1inv)  + ghosts*(1+jStride+kStride);
  valid = (double*)(((char*)bx)+ level->valid)  + ghosts*(1+jStride+kStride);
  double box_eigenvalue = -1e9;
  for(k=0;k<dim;k++) {
    for(j=0;j<dim;j++) {
      for(i=0;i<dim;i++) {
        int ijk = i + j*jStride + k*kStride;
        // radius of Gershgorin disc is the sum of the absolute values of the off-diagonal elements...
        double sumAbsAij = fabs(b*h2inv) * (
                    fabs( beta_i[ijk        ]*valid[ijk-1      ] )+
                    fabs( beta_j[ijk        ]*valid[ijk-jStride] )+
                    fabs( beta_k[ijk        ]*valid[ijk-kStride] )+
                    fabs( beta_i[ijk+1      ]*valid[ijk+1      ] )+
                    fabs( beta_j[ijk+jStride]*valid[ijk+jStride] )+
                    fabs( beta_k[ijk+kStride]*valid[ijk+kStride] )
                    );

        // centr of Gershgorin disc is the diagonal element...
        double    Aii = a*alpha[ijk] - b*h2inv*(
                                     beta_i[ijk        ]*( valid[ijk-1      ]-2.0)+
                                     beta_j[ijk        ]*( valid[ijk-jStride]-2.0)+
                                     beta_k[ijk        ]*( valid[ijk-kStride]-2.0)+
                                     beta_i[ijk+1      ]*( valid[ijk+1      ]-2.0)+
                                     beta_j[ijk+jStride]*( valid[ijk+jStride]-2.0)+
                                     beta_k[ijk+kStride]*( valid[ijk+kStride]-2.0)
                                   );
         Dinv[ijk] = 1.0/Aii;
         if(Aii>=1.5*sumAbsAij)
           L1inv[ijk] = 1.0/(Aii              );
         else
           L1inv[ijk] = 1.0/(Aii+0.5*sumAbsAij);
         double Di = (Aii + sumAbsAij)/Aii;
         if(Di>box_eigenvalue)
           box_eigenvalue=Di;
      }
    }
  }
  return box_eigenvalue;
}

void hpgmg_coeff_restrict_box(level_type *level, box_type *cb,
                              level_type *fromLevel, box_type *fine_bx,
                              int fb, int count) {
  if (count <= 8) {
    // restrict alpha -- cell
    restrict_generic(level, cb, level->alpha, fromLevel, fine_bx,
       fromLevel->alpha, fb, count==1?fromLevel->box_dim:fromLevel->box_dim>>1, 0);

    // restrict beta_i -- face_i
    restrict_generic(level, cb, level->beta_i, fromLevel, fine_bx,
       fromLevel->beta_i, fb, count==1?fromLevel->box_dim:fromLevel->box_dim>>1, 1);

    // restrict beta_j -- face_j
    restrict_generic(level, cb, level->beta_j, fromLevel, fine_bx,
       fromLevel->beta_j, fb, count==1?fromLevel->box_dim:fromLevel->box_dim>>1, 2);

    // restrict beta_k -- face_k
    restrict_generic(level, cb, level->beta_k, fromLevel, fine_bx,
       fromLevel->beta_k, fb, count==1?fromLevel->box_dim:fromLevel->box_dim>>1, 3);
  } else {
    restrict_generic_all(level, cb, level->alpha, fromLevel, fine_bx, fromLevel->alpha, 0);
    restrict_generic_all(level, cb, level->beta_i, fromLevel, fine_bx, fromLevel->beta_i, 1);
    restrict_generic_all(level, cb, level->beta_j, fromLevel, fine_bx, fromLevel->beta_j, 2);
    restrict_generic_all(level, cb, level->beta_k, fromLevel, fine_bx, fromLevel->beta_k, 3);
  }
}


// The per-box term of the level-wide dot(alpha,alpha).  The whole dot is a
// sum of squares, so it is zero exactly when every box's term is zero: the
// level-wide test survives a max reduction, which is exact and independent
// of the order the boxes are reduced in.
double hpgmg_alpha_dot_box(level_type *level, box_type *bx) {
  int i,j,k;
  int jStride = level->jStride;
  int kStride = level->kStride;
  int     dim = level->box_dim;
  double * __restrict__ a = (double*)(((char*)bx) + level->alpha)
                          + NUM_GHOSTS*(1+jStride+kStride);
  double s = 0.0;
  for(k=0;k<dim;k++){
  for(j=0;j<dim;j++){
  for(i=0;i<dim;i++){
    int ijk = i + j*jStride + k*kStride;
    s += a[ijk]*a[ijk];
  }}}
  return s;
}

/* ------------------------------------------------------------------ */
/* geometry                                                            */
/* ------------------------------------------------------------------ */

// Halve the box dimension down to the agglomeration threshold, then
// agglomerate the box grid, then halve the last box down to one cell.
int hpgmg_level_table(int box_dim0, int boxes_in_i0, int *box_dim, int *boxes_in_i) {
  int minCoarseGridDim = 1;
  int maxLevels = MG_MAXLEVELS;
  int level = 1;
  int coarse_dim = box_dim0 * boxes_in_i0;
  int num_levels, doRestrict;

  while( (coarse_dim>=2*minCoarseGridDim) && ((coarse_dim&0x1)==0) ) {
    level++;
    coarse_dim = coarse_dim / 2;
  }
  if(level<maxLevels)maxLevels=level;

  box_dim[0] = box_dim0;
  boxes_in_i[0] = boxes_in_i0;
  num_levels = 1;
  doRestrict = (maxLevels<2) ? 0 : 1;

  while(doRestrict){
    int lv = num_levels;
    int fine_box_dim    = box_dim[lv-1];
    int fine_boxes_in_i = boxes_in_i[lv-1];
    int fine_dim_i      = fine_box_dim * fine_boxes_in_i;
    doRestrict = 0;
    if( (fine_box_dim % 2 == 0) && (fine_box_dim > MG_AGGLOMERATION_START) ){
      box_dim[lv] = fine_box_dim/2; boxes_in_i[lv] = fine_boxes_in_i; doRestrict = 1;
    }else
    if( (fine_boxes_in_i % 2 == 0) ){                    // 8:1 box agglomeration
      box_dim[lv] = fine_box_dim; boxes_in_i[lv] = fine_boxes_in_i/2; doRestrict = 1;
    }else
    if( (coarse_dim != 1) && (fine_dim_i == 2*coarse_dim) ){   // agglomerate everything
      box_dim[lv] = fine_dim_i/2; boxes_in_i[lv] = 1; doRestrict = 1;
    }else
    if( (coarse_dim != 1) && (fine_dim_i == 4*coarse_dim) ){
      box_dim[lv] = fine_box_dim/2; boxes_in_i[lv] = fine_boxes_in_i; doRestrict = 1;
    }else
    if( (coarse_dim != 1) && (fine_dim_i == 8*coarse_dim) ){
      box_dim[lv] = fine_box_dim/2; boxes_in_i[lv] = fine_boxes_in_i; doRestrict = 1;
    }else
    if( (fine_box_dim % 2 == 0) ){
      box_dim[lv] = fine_box_dim/2; boxes_in_i[lv] = fine_boxes_in_i; doRestrict = 1;
    }
    if(doRestrict && box_dim[lv] < NUM_GHOSTS)doRestrict = 0;
    if(doRestrict && lv+1 >= MG_MAXLEVELS)doRestrict = 0;
    if(doRestrict)num_levels++;
  }
  return num_levels;
}

// The box memory geometry, one place for every decomposition.
u32 hpgmg_box_geometry(int box_dim, int *jStrideOut, int *kStrideOut) {
  int jStride = (box_dim+2*NUM_GHOSTS);
  int kStride;
  while(jStride % BOX_SIMD_ALIGNMENT)jStride++;      // pencil
  kStride = jStride*(box_dim+2*NUM_GHOSTS);          // plane
  if(jStride<BOX_PLANE_PADDING)kStride += (BOX_PLANE_PADDING-jStride);
  while(kStride % BOX_SIMD_ALIGNMENT)kStride++;
  if(jStrideOut)*jStrideOut = jStride;
  if(kStrideOut)*kStrideOut = kStride;
  return (u32)((box_dim+2*NUM_GHOSTS)*kStride);
}

// The per-rank memory a run is allowed to occupy.  A geometry above it cannot
// run, so it is refused while the arguments are still the only thing that
// exists rather than at the first allocation that fails.
#define HPGMG_MAX_RANK_BYTES (190.0*1000.0*1000.0*1000.0)

// The constraints a chosen box grid must satisfy, checked before any datablock
// exists: the transfer operators carry a fixed-size fine-box list, the bottom
// solver runs on the coarsest level as a single box, every datablock size must
// be representable, and the hierarchy must fit the memory each rank has.
void hpgmg_check_geometry(int box_dim, int boxes_in_i) {
  int bd[MG_MAXLEVELS], bi[MG_MAXLEVELS];
  int L = 0, l, bad = 0;
  double bytes = 0.0, perRank;
  u64 ranks = 1;

#ifdef ENABLE_EXTENSION_AFFINITY
  ocrAffinityCount(AFFINITY_PD, &ranks);
#endif
  if(ranks < 1) ranks = 1;

  if(boxes_in_i < 1){
    ocrPrintf("target_boxes must select at least one box per dimension\n");
    bad = 1;
  } else {
    L = hpgmg_level_table(box_dim, boxes_in_i, bd, bi);
    for(l=1;l<L;l++){
      int r = bi[l-1]/bi[l];
      r = r*r*r;
      if(r > MAX_FINE_BOXES){
        ocrPrintf("box grid %d^3 of %d^3 cells: level %d -> %d agglomerates %d fine "
                  "boxes per coarse box, more than the %d the transfer operators "
                  "carry; choose a target_boxes whose box grid coarsens in smaller "
                  "steps\n", boxes_in_i, box_dim, l-1, l, r, MAX_FINE_BOXES);
        bad = 1;
      }
    }
    if(bi[L-1] != 1){
      ocrPrintf("box grid %d^3 of %d^3 cells: the hierarchy bottoms out on a %d^3 box "
                "grid, but the bottom solver runs on a single box; choose a "
                "target_boxes whose box grid reduces to one box\n",
                boxes_in_i, box_dim, bi[L-1]);
      bad = 1;
    }
    for(l=0;l<L;l++){
      double n = (double)bi[l]*bi[l]*bi[l];
      double boxBytes = (double)sizeof(box_type)
                      + (double)hpgmg_box_geometry(bd[l], NULL, NULL)
                        *NUM_VECTORS*sizeof(double);
      // A level datablock carries its box guid table, the reserved slab-guid
      // tail and one norm per box; both it and a box datablock are sized in a
      // 32-bit count, so a level whose table or whose box overflows that count
      // is unbuildable whatever the memory.
      double lvlBytes = (double)sizeof(level_type)
                      + n*(7.0*(double)sizeof(ocrGuid_t) + (double)sizeof(double));
      if(lvlBytes > 4294967295.0 || boxBytes > 4294967295.0){
        ocrPrintf("box grid %d^3 of %d^3 cells: level %d needs a %.0f B level "
                  "datablock and %.0f B box datablocks, and a datablock size is "
                  "a 32-bit count; choose a smaller target_boxes or "
                  "log2_box_dim\n", boxes_in_i, box_dim, l, lvlBytes, boxBytes);
        bad = 1;
        break;
      }
      bytes += n*boxBytes + lvlBytes;
    }
  }

  if(!bad){
    perRank = bytes/(double)ranks;
    ocrPrintf("%d levels, %d^3 cells, %.2f GiB of datablocks over %lu rank(s), "
              "%.2f GiB per rank\n",
              L, box_dim*boxes_in_i, bytes/(1024.0*1024.0*1024.0),
              (unsigned long)ranks, perRank/(1024.0*1024.0*1024.0));
    if(perRank > HPGMG_MAX_RANK_BYTES){
      ocrPrintf("box grid %d^3 of %d^3 cells: %.2f GiB per rank exceeds the "
                "%.2f GiB a rank may occupy; choose a smaller target_boxes or "
                "log2_box_dim, or run on more ranks\n",
                boxes_in_i, box_dim, perRank/(1024.0*1024.0*1024.0),
                HPGMG_MAX_RANK_BYTES/(1024.0*1024.0*1024.0));
      bad = 1;
    }
  }

  if(bad){
#if TG_ARCH
    ABORT(0);
#else
    ocrAbort(0);
#endif
  }
}

/* ------------------------------------------------------------------ */
/* per-box initialization tasks                                        */
/* ------------------------------------------------------------------ */

// deps: level - box(RW); pars: fine flag
// Everything the box needs before the operator build: zero the vector
// region, mark the valid cells and, on the fine level, evaluate the analytic
// problem.  The evaluation is a pure function of the cell's coordinates, so
// it is deterministic and independent of how the boxes are distributed.
ocrGuid_t hpgmg_prep_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  level_type *l = (level_type*) depv[0].ptr;
  box_type *bx = (box_type*) depv[1].ptr;
  bzero((char*)bx + sizeof(box_type), l->volume*NUM_VECTORS*sizeof(double));
  hpgmg_valid_box(l, bx);
  if(paramv[0])
    hpgmg_fill_box(l, bx, HPGMG_A, HPGMG_B);
  return NULL_GUID;
}

// deps: coarse level - coarse box(RW) - fine level - fine boxes; pars: count
ocrGuid_t hpgmg_coeff_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  level_type *cl = (level_type*) depv[0].ptr;
  level_type *f = (level_type*) depv[2].ptr;
  box_type *cb = (box_type*) depv[1].ptr;
  int count = (int) paramv[0];
  int o;
  for(o = 3; o < (int) depc; ++o)
    hpgmg_coeff_restrict_box(cl, cb, f, (box_type*) depv[o].ptr, o-3, count);
  return NULL_GUID;
}

// deps: level - box(RW); returns {Gershgorin bound, dot(alpha,alpha) term}
ocrGuid_t hpgmg_gersh_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  level_type *l = (level_type*) depv[0].ptr;
  box_type *bx = (box_type*) depv[1].ptr;
  ocrGuid_t g; double *d;
  ocrDbCreate(&g, (void**)&d, 2*sizeof(double), 0, NULL_HINT, NO_ALLOC);
  d[0] = hpgmg_gershgorin_box(l, bx, HPGMG_A, HPGMG_B);
  d[1] = hpgmg_alpha_dot_box(l, bx);
  return g;
}

// deps: level - box(RW) - the 26 neighbours
// Fills this box's ghost layers of the smoother's diagonals.  Only the box's
// own ghosts are written and only the neighbours' interiors are read, so the
// boxes of a level may run in any order.
ocrGuid_t hpgmg_diag_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  level_type *l = (level_type*) depv[0].ptr;
  box_type *b = (box_type*) depv[1].ptr;
  int i;
  for(i = 0; i < 26; i++){
    box_type *nb = (box_type*) depv[i+2].ptr;
    if(nb->global_box_id == -1)
      continue;
    update_boundary_all(l, b, l->Dinv, nb, l->Dinv, 0);
    update_boundary_all(l, b, l->L1inv, nb, l->L1inv, 0);
  }
  return NULL_GUID;
}

// deps: level - box(RW); returns {max |u - u_true|, max |scaled residual|}
// The verification pass: an O(n) sweep of the box the task owns, over values
// the solve already produced.  Both extrema are maxima, so reducing them over
// the boxes is exact and independent of the reduction order and of how the
// boxes are distributed.
ocrGuid_t hpgmg_error_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  level_type *l = (level_type*) depv[0].ptr;
  box_type *bx = (box_type*) depv[1].ptr;
  int jS = l->jStride, kS = l->kStride, dim = l->box_dim;
  double *u  = (double*)((char*)bx + l->u) + NUM_GHOSTS*(1+jS+kS);
  double *ut = (double*)((char*)bx + l->u_true) + NUM_GHOSTS*(1+jS+kS);
  double *vt = (double*)((char*)bx + l->vec_temp) + NUM_GHOSTS*(1+jS+kS);
  double e = 0.0, r = 0.0, a;
  int i,j,k;
  for(k = 0; k < dim; k++){
  for(j = 0; j < dim; j++){
  for(i = 0; i < dim; i++){
    int ijk = i + j*jS + k*kS;
    a = fabs(vt[ijk]);            // the scaled residual the solve ended on
    if(a > r) r = a;
    vt[ijk] = u[ijk] - ut[ijk];
    a = fabs(vt[ijk]);
    if(a > e) e = a;
  }}}
  ocrGuid_t g; double *d;
  ocrDbCreate(&g, (void**)&d, 2*sizeof(double), 0, NULL_HINT, NO_ALLOC);
  d[0] = e; d[1] = r;
  return g;
}

// deps: level - gate; pars: the event the reduced pair is handed to
ocrGuid_t hpgmg_error_level_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  level_type *l = (level_type*) depv[0].ptr;
  ocrGuid_t *boxes = (ocrGuid_t*)(((char*)l) + l->boxes);
  int n = l->num_boxes, b;
  ocrGuid_t m, m_t, c, c_t, fin;
  ocrHint_t hh;

  ocrEdtTemplateCreate(&m_t, hpgmg_max_merge_edt, 1, n);
  ocrEdtCreate(&m, m_t, 1, paramv, n, NULL, 0, mgHomeEdtHint(&hh), NULL);
  ocrEdtTemplateDestroy(m_t);

  ocrEdtTemplateCreate(&c_t, hpgmg_error_edt, 0, 2);
  for(b = 0; b < n; ++b){
    ocrEdtCreate(&c, c_t, 0, NULL, 2, NULL, 0, mgBoxEdtHint(&hh, boxes[b]), &fin);
    ocrAddDependence(fin, m, b, DB_MODE_CONST);
    ocrAddDependence(depv[0].guid, c, 0, DB_MODE_CONST);
    ocrAddDependence(boxes[b], c, 1, DB_MODE_RW);
  }
  ocrEdtTemplateDestroy(c_t);
  return NULL_GUID;
}

// deps: level - gate - the reduced pairs
ocrGuid_t hpgmg_error_final_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  level_type *l = (level_type*) depv[0].ptr;
  double e = 0.0, r = 0.0;
  u32 i;
  for(i = 2; i < depc; i++){
    double *v = (double*) depv[i].ptr;
    if(v[0] > e) e = v[0];
    if(v[1] > r) r = v[1];
    ocrDbDestroy(depv[i].guid);
  }
  ocrPrintf("h = %f  ||error|| = %22.15f\n\n", l->h, e);
  ocrPrintf("final residual norm = %22.20f\n", r);
  ocrPrintf("Time = %22f\n", l->time_operators[4]/TIMED);
  return NULL_GUID;
}

// deps: partial results; pars: the event the merged pair is handed to
ocrGuid_t hpgmg_max_merge_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  double m0 = -1e9, m1 = -1e9;
  u32 i;
  for(i = 0; i < depc; i++){
    double *v = (double*) depv[i].ptr;
    if(v[0] > m0) m0 = v[0];
    if(v[1] > m1) m1 = v[1];
    ocrDbDestroy(depv[i].guid);
  }
  ocrGuid_t g; double *d;
  ocrDbCreate(&g, (void**)&d, 2*sizeof(double), 0, NULL_HINT, NO_ALLOC);
  d[0] = m0; d[1] = m1;
  ocrDbRelease(g);
  ocrEventSatisfy(hpgmg_guid_from(paramv[0]), g);
  return NULL_GUID;
}

// deps: level(RW) - gate - the merged pairs
ocrGuid_t hpgmg_eigen_final_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  level_type *l = (level_type*) depv[0].ptr;
  double m0 = -1e9, m1 = -1e9;
  u32 i;
  for(i = 2; i < depc; i++){
    double *v = (double*) depv[i].ptr;
    if(v[0] > m0) m0 = v[0];
    if(v[1] > m1) m1 = v[1];
    ocrDbDestroy(depv[i].guid);
  }
  l->dominant_eigenvalue_of_DinvA = m0;
  l->alpha_is_zero = (m1 == 0.0);
  ocrPrintf(" rebuilding operator for level...  h=%e  eigenvalue_max < %f\n", l->h, m0);
  return NULL_GUID;
}

/* ------------------------------------------------------------------ */
/* the initialization phases                                           */
/* ------------------------------------------------------------------ */

static ocrGuid_t init_prep_level_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
static ocrGuid_t init_coeff_level_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
static ocrGuid_t init_gersh_level_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);
static ocrGuid_t init_diag_level_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]);

// deps: level - gate; pars: fine flag
static ocrGuid_t init_prep_level_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  level_type *l = (level_type*) depv[0].ptr;
  ocrGuid_t *boxes = (ocrGuid_t*)(((char*)l) + l->boxes);
  ocrGuid_t t, c;
  ocrHint_t hh;
  int b;
  ocrEdtTemplateCreate(&t, hpgmg_prep_edt, 1, 2);
  for(b = 0; b < l->num_boxes; ++b){
    ocrEdtCreate(&c, t, 1, paramv, 2, NULL, 0, mgBoxEdtHint(&hh, boxes[b]), NULL);
    ocrAddDependence(depv[0].guid, c, 0, DB_MODE_CONST);
    ocrAddDependence(boxes[b], c, 1, DB_MODE_RW);
  }
  ocrEdtTemplateDestroy(t);
  return NULL_GUID;
}

// deps: coarse level - fine level - gate
static ocrGuid_t init_coeff_level_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  level_type *cl = (level_type*) depv[0].ptr;
  level_type *f = (level_type*) depv[1].ptr;
  ocrGuid_t *boxesC = (ocrGuid_t*)(((char*)cl) + cl->boxes);
  ocrGuid_t fine[MAX_FINE_BOXES];
  int count = f->num_boxes / cl->num_boxes;
  u64 cnt = (u64) count;
  ocrGuid_t t, c;
  ocrHint_t hh;
  int b, fc;
  ocrEdtTemplateCreate(&t, hpgmg_coeff_edt, 1, 3+count);
  for(b = 0; b < cl->num_boxes; ++b){
    get_fine_boxes(f, cl, b, fine);
    ocrEdtCreate(&c, t, 1, &cnt, 3+count, NULL, 0, mgBoxEdtHint(&hh, boxesC[b]), NULL);
    ocrAddDependence(depv[0].guid, c, 0, DB_MODE_CONST);
    ocrAddDependence(boxesC[b], c, 1, DB_MODE_RW);
    ocrAddDependence(depv[1].guid, c, 2, DB_MODE_CONST);
    for(fc = 0; fc < count; ++fc)
      ocrAddDependence(fine[fc], c, 3+fc, DB_MODE_CONST);
  }
  ocrEdtTemplateDestroy(t);
  return NULL_GUID;
}

// deps: level - gate; pars: the event the level's reduced pair is handed to
static ocrGuid_t init_gersh_level_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  level_type *l = (level_type*) depv[0].ptr;
  ocrGuid_t *boxes = (ocrGuid_t*)(((char*)l) + l->boxes);
  int n = l->num_boxes, b;
  ocrGuid_t m, m_t, c, c_t, fin;
  ocrHint_t hh;

  ocrEdtTemplateCreate(&m_t, hpgmg_max_merge_edt, 1, n);
  ocrEdtCreate(&m, m_t, 1, paramv, n, NULL, 0, mgHomeEdtHint(&hh), NULL);
  ocrEdtTemplateDestroy(m_t);

  ocrEdtTemplateCreate(&c_t, hpgmg_gersh_edt, 0, 2);
  for(b = 0; b < n; ++b){
    ocrEdtCreate(&c, c_t, 0, NULL, 2, NULL, 0, mgBoxEdtHint(&hh, boxes[b]), &fin);
    ocrAddDependence(fin, m, b, DB_MODE_CONST);
    ocrAddDependence(depv[0].guid, c, 0, DB_MODE_CONST);
    ocrAddDependence(boxes[b], c, 1, DB_MODE_RW);
  }
  ocrEdtTemplateDestroy(c_t);
  return NULL_GUID;
}

// deps: level - gate
static ocrGuid_t init_diag_level_edt(u32 paramc, u64* paramv, u32 depc, ocrEdtDep_t depv[]) {
  level_type *l = (level_type*) depv[0].ptr;
  ocrGuid_t *boxes = (ocrGuid_t*)(((char*)l) + l->boxes);
  ocrGuid_t neighbors[26];
  ocrGuid_t t, c;
  ocrHint_t hh;
  int b, i;
  ocrEdtTemplateCreate(&t, hpgmg_diag_edt, 0, 28);
  for(b = 0; b < l->num_boxes; ++b){
    get_neighbor_guids_all(b, l, neighbors);
    ocrEdtCreate(&c, t, 0, NULL, 28, NULL, 0, mgBoxEdtHint(&hh, boxes[b]), NULL);
    ocrAddDependence(depv[0].guid, c, 0, DB_MODE_CONST);
    ocrAddDependence(boxes[b], c, 1, DB_MODE_RW);
    for(i = 0; i < 26; i++)
      ocrAddDependence(neighbors[i], c, 2+i, DB_MODE_CONST);
  }
  ocrEdtTemplateDestroy(t);
  return NULL_GUID;
}

// One phase of the chain: a FINISH task per level whose fan-out is the
// level's boxes, so the phase completes only when every box has been
// produced.  lvlB is the partner level of a two-level phase.
static ocrGuid_t init_phase(ocrEdt_t body, u64 *pv, u32 pc, u32 nlvl,
                            ocrGuid_t lvlA, ocrGuid_t lvlB, ocrGuid_t gate) {
  ocrGuid_t t, e, fin;
  ocrHint_t hh;
  u32 depc = nlvl + 1;
  ocrEdtTemplateCreate(&t, body, pc, depc);
  ocrEdtCreate(&e, t, pc, pv, depc, NULL, EDT_PROP_FINISH, mgHomeEdtHint(&hh), &fin);
  ocrAddDependence(lvlA, e, 0, DB_MODE_CONST);
  if(nlvl > 1)
    ocrAddDependence(lvlB, e, 1, DB_MODE_CONST);
  ocrAddDependence(gate, e, nlvl, DB_MODE_NULL);
  ocrEdtTemplateDestroy(t);
  return fin;
}

/* ------------------------------------------------------------------ */
/* hierarchy layout                                                    */
/* ------------------------------------------------------------------ */

// Field-complete level datablock and its box datablocks, headers only: the
// per-box tasks produce the contents.
static void init_create_level(mg_type *mg, int box_dim, int boxes_in_i,
                              int bc, int level, double h) {
  u32 totalBoxes = (u32)boxes_in_i*boxes_in_i*boxes_in_i;
  u32 levelSize = sizeof(level_type) + sizeof(ocrGuid_t)*totalBoxes
                + sizeof(ocrGuid_t)*totalBoxes*6 + totalBoxes*sizeof(double);
  int jStride, kStride;
  u32 boxVolume = hpgmg_box_geometry(box_dim, &jStride, &kStride);
  u32 totalMemSize = sizeof(box_type) + boxVolume*NUM_VECTORS*sizeof(double);
  ocrGuid_t levelGuid, *boxes;
  level_type *lp;
  int b;

  ocrPrintf("attempting to create a %d^3 level using a %d^3 grid of %d^3 boxes ...\n",
            box_dim*boxes_in_i, boxes_in_i, box_dim);

  ocrDbCreate(&levelGuid, (void**)&lp, levelSize, 0, NULL_HINT, NO_ALLOC);
  mg->levels[level] = levelGuid;

  lp->h = h;
  lp->boundary_condition = bc;
  lp->level = level;
  lp->alpha_is_zero = -1;
  lp->dominant_eigenvalue_of_DinvA = -1e9;
  lp->num_boxes = totalBoxes;
  lp->box_dim = box_dim;
  lp->boxes_in.i = lp->boxes_in.j = lp->boxes_in.k = boxes_in_i;
  lp->dim.i = lp->dim.j = lp->dim.k = boxes_in_i*box_dim;
  lp->boxes = sizeof(level_type);
  lp->b_norms = lp->boxes + sizeof(ocrGuid_t)*totalBoxes;
  lp->jStride = jStride;
  lp->kStride = kStride;
  lp->volume = boxVolume;
  memset(lp->time_operators, 0, sizeof(lp->time_operators));
  memset(lp->time_temp, 0, sizeof(lp->time_temp));

  lp->alpha = sizeof(box_type);
  lp->beta_i = lp->alpha + boxVolume*sizeof(double);
  lp->beta_j = lp->beta_i + boxVolume*sizeof(double);
  lp->beta_k = lp->beta_j + boxVolume*sizeof(double);
  lp->Dinv = lp->beta_k + boxVolume*sizeof(double);
  lp->L1inv = lp->Dinv + boxVolume*sizeof(double);
  lp->valid = lp->L1inv + boxVolume*sizeof(double);
  lp->u_true = lp->valid + boxVolume*sizeof(double);
  lp->f = lp->u_true + boxVolume*sizeof(double);
  lp->f_Av = lp->f + boxVolume*sizeof(double);
  lp->u = lp->f_Av + boxVolume*sizeof(double);
  lp->vec_temp = lp->u + boxVolume*sizeof(double);

  boxes = (ocrGuid_t*)(((char*)lp) + lp->boxes);

  for(b = 0; b < (int)totalBoxes; b++){
    ocrGuid_t boxGuid, currentAffinity = NULL_GUID;
    ocrHint_t myDbAffinityHNT;
    box_type *boxPtr;
    ocrHintInit(&myDbAffinityHNT, OCR_HINT_DB_T);
#ifdef ENABLE_EXTENSION_AFFINITY
    {
      s64 affinityCount;
      ocrAffinityCount(AFFINITY_PD, &affinityCount);
#ifdef OCR_APP_OPTIMIZED_PLACEMENT
      ocrAffinityGetAt(AFFINITY_PD, boxHomePD(b, boxes_in_i, affinityCount),
                       &currentAffinity);
#else
      ocrAffinityGetAt(AFFINITY_PD, b%affinityCount, &currentAffinity);
#endif
    }
#endif
    ocrSetHintValue(&myDbAffinityHNT, OCR_HINT_DB_AFFINITY,
                    ocrAffinityToHintValue(currentAffinity));
    ocrDbCreate(&boxGuid, (void**)&boxPtr, totalMemSize, 0, &myDbAffinityHNT, NO_ALLOC);
    boxPtr->low.i = (b % boxes_in_i) * box_dim;
    boxPtr->low.j = ((b / boxes_in_i) % boxes_in_i) * box_dim;
    boxPtr->low.k = (b / (boxes_in_i*boxes_in_i)) * box_dim;
    boxPtr->global_box_id = b;
    /* Only released writes are guaranteed visible to a consumer, and the
     * per-box tasks below consume this header. */
    ocrDbRelease(boxGuid);
    boxes[b] = boxGuid;

    if(b == 0){
      box_type *const_box;
      ocrDbCreate(&lp->constant_box_guid, (void**)&const_box, totalMemSize, 0,
                  &myDbAffinityHNT, NO_ALLOC);
      const_box->global_box_id = -1;
      ocrDbRelease(lp->constant_box_guid);
    }
  }

  ocrDbRelease(levelGuid);
}

// Lays the hierarchy out and wires the initialization chain.  Nothing but
// the level and box headers is produced here: the zeroing, the analytic
// evaluation, the coefficient restriction, the Gershgorin sweep and the
// diagonal ghost exchange all run as tasks placed with the box they own.
ocrGuid_t init_all(mg_type* mg_ptr, int box_dim, int boxes_in_i,
                   int boundary_condition, ocrGuid_t *startEvt) {
  int bd[MG_MAXLEVELS], bi[MG_MAXLEVELS];
  int L, l;
  double h;
  ocrGuid_t chain;

#ifdef ENABLE_EXTENSION_AFFINITY
  ocrPrintf("Using affinity API\n");
#else
  ocrPrintf("NOTE: Not using affinity API\n");
#endif

  L = hpgmg_level_table(box_dim, boxes_in_i, bd, bi);
  ocrPrintf("level hierarchy: %d levels\n", L);
  mg_ptr->num_levels = L;
  mg_ptr->max_levels = L;

  h = 1.0/((double)boxes_in_i*(double)box_dim);
  for(l = 0; l < L; l++){
    init_create_level(mg_ptr, bd[l], bi[l], boundary_condition, l, h);
    h *= 2.0;
  }

  ocrEventCreate(startEvt, OCR_EVENT_ONCE_T, 1);
  chain = *startEvt;

  // zero / valid / analytic evaluation, one task per box
  for(l = 0; l < L; l++){
    u64 fine = (l == 0);
    chain = init_phase(init_prep_level_edt, &fine, 1, 1,
                       mg_ptr->levels[l], NULL_GUID, chain);
  }

  // the operator build, level by level down the hierarchy
  for(l = 0; l < L; l++){
    ocrGuid_t evt, f, f_t, fOut;
    ocrHint_t hh;
    u64 pv;

    if(l > 0)
      chain = init_phase(init_coeff_level_edt, NULL, 0, 2,
                         mg_ptr->levels[l], mg_ptr->levels[l-1], chain);

    ocrEventCreate(&evt, OCR_EVENT_ONCE_T, 1);
    pv = hpgmg_guid_bits(evt);
    chain = init_phase(init_gersh_level_edt, &pv, 1, 1,
                       mg_ptr->levels[l], NULL_GUID, chain);

    ocrEdtTemplateCreate(&f_t, hpgmg_eigen_final_edt, 0, 3);
    ocrEdtCreate(&f, f_t, 0, NULL, 3, NULL, 0, mgHomeEdtHint(&hh), &fOut);
    ocrEdtTemplateDestroy(f_t);
    ocrAddDependence(mg_ptr->levels[l], f, 0, DB_MODE_RW);
    ocrAddDependence(chain, f, 1, DB_MODE_NULL);
    ocrAddDependence(evt, f, 2, DB_MODE_CONST);
    chain = fOut;

    chain = init_phase(init_diag_level_edt, NULL, 0, 1,
                       mg_ptr->levels[l], NULL_GUID, chain);
  }

  return chain;
}
