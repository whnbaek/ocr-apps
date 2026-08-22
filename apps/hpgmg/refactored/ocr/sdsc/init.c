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


void init_all(mg_type* mg_ptr, int box_dim, int boxes_in_i, int boundary_condition, int level) {

#ifdef ENABLE_EXTENSION_AFFINITY
  ocrPrintf("Using affinity API\n");
#else
  ocrPrintf("NOTE: Not using affinity API\n");
#endif

  level_type* fine_ptr = create_level(mg_ptr, box_dim, boxes_in_i, boundary_condition, 0);
  fine_ptr->level = level;

  int minCoarseDim = 1;
#ifdef USE_HELMHOLTZ
//  ocrPrintf("  Creating Helmholtz (a=2.0, b=1.0) test problem\n");
  initialize_problem(fine_ptr, 2.0, 1.0);
  rebuild_operator(fine_ptr,NULL, 2.0, 1.0);
  mg_build(mg_ptr, fine_ptr, 2.0, 1.0, minCoarseDim);
#else
//  ocrPrintf("  Creating Poisson (a=0.0, b=1.0) test problem\n");
  initialize_problem(fine_ptr, 0.0, 1.0);
  rebuild_operator(fine_ptr,NULL, 0.0, 1.0);
  mg_build(mg_ptr, fine_ptr, 0.0, 1.0, minCoarseDim);
#endif

  ocrDbDestroy(fine_ptr->tempGuid);
}

level_type* create_level(mg_type* mg, int box_dim, int boxes_in_i, int boundary_condition, int level) {

  u32 totalBoxes = boxes_in_i*boxes_in_i*boxes_in_i;

  // how to compute the size of this level? For now using some calculation
  u32 levelSize = sizeof(level_type) + sizeof(ocrGuid_t)*totalBoxes + sizeof(ocrGuid_t)*totalBoxes*6 /* ToDo: Manu: not sure why this is needed */ + totalBoxes*sizeof(double); // need to add fine and coarse related guids


  ocrPrintf("attempting to create a %d^3 level using a %d^3 grid of %d^3 boxes ...\n",box_dim*boxes_in_i,boxes_in_i,box_dim);

  // create level and populate mg and level related info
  ocrGuid_t levelGuid;
  level_type *levelPtr;
  ocrDbCreate(&levelGuid, (void**)&levelPtr, levelSize, 0,NULL_HINT,NO_ALLOC);
  mg->levels[level] = levelGuid;

#if DEBUG
  ocrPrintf("level %u: guid %u, box_dim %u\n", level, mg->levels[level], box_dim);
#endif

  // not sure when to populate mg->max_levels, current_level, vcycle_level

  levelPtr->alpha = -1;
  levelPtr->boundary_condition = boundary_condition;
  levelPtr->level = level;
  levelPtr->alpha_is_zero = -1;
  levelPtr->num_boxes = totalBoxes;
  levelPtr->box_dim = box_dim;
  levelPtr->boxes_in.i = levelPtr->boxes_in.j = levelPtr->boxes_in.k = boxes_in_i;
  levelPtr->dim.i = levelPtr->dim.j = levelPtr->dim.k = boxes_in_i*box_dim;
  levelPtr->boxes = sizeof(level_type);
  levelPtr->time_operators[0] = levelPtr->time_operators[1] = levelPtr->time_operators[2] = 0.0;
  levelPtr->time_operators[3] = levelPtr->time_operators[4] = 0.0;
  levelPtr->time_temp[0] = levelPtr->time_temp[1] = levelPtr->time_temp[2] = 0.0;
  levelPtr->time_temp[3] = levelPtr->time_temp[4] = 0.0;


  levelPtr->b_norms = levelPtr->boxes + sizeof(ocrGuid_t)*totalBoxes;

  // create a temporary structure to hold all the box pointers
  ocrDbCreate(&levelPtr->tempGuid, (void**)&levelPtr->temp, sizeof(box_type *)*totalBoxes, 0,NULL_HINT,NO_ALLOC);

  // create boxes
  int i,j,k;
  for(k=0;k<levelPtr->boxes_in.k;k++) {
    for(j=0;j<levelPtr->boxes_in.j;j++) {
      for(i=0;i<levelPtr->boxes_in.i;i++) {
        int jStride = levelPtr->boxes_in.i;
        int kStride = levelPtr->boxes_in.i*levelPtr->boxes_in.j;
        int b=i + j*jStride + k*kStride;
        box_type *box;
        box = create_box(levelPtr,NUM_VECTORS, box_dim,NUM_GHOSTS,b); // Manu: check is NUM_GHOSTS is always 1 for all levels
        levelPtr->temp[b] = box;
        box->low.i = i*levelPtr->box_dim;
        box->low.j = j*levelPtr->box_dim;
        box->low.k = k*levelPtr->box_dim;
        box->global_box_id = b;
      }
    }
  }


  // printing all boxguids
#if DEBUG
  ocrGuid_t *t =  (ocrGuid_t *)(((char*)levelPtr)+ levelPtr->boxes);
  ocrPrintf("----------\n");
  for (i=0;i<totalBoxes;i++)
    ocrPrintf("%u\n", *(t+i));
#endif

  initialize_valid_region(levelPtr);

  return levelPtr;
}


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

void initialize_valid_region(level_type * level){
  int box;
  for(box=0;box<level->num_boxes;box++)
    hpgmg_valid_box(level, level->temp[box]);
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

box_type* create_box(level_type* lPtr, int num_vecs, int box_dim, int num_ghosts,int box_num) {


  ocrGuid_t boxGuid;
  box_type *boxPtr;
  u32 boxVolume, totalMemSize;

  #ifndef BOX_SIMD_ALIGNMENT
  #define BOX_SIMD_ALIGNMENT 1 // allignment requirement for j+/-1
  #endif
  int jStride = (box_dim+2*num_ghosts);while(jStride % BOX_SIMD_ALIGNMENT)jStride++; // pencil
  #ifndef BOX_PLANE_PADDING
  #define BOX_PLANE_PADDING  8 // scratch space to avoid unrolled loop cleanup
  #endif
  int kStride = jStride*(box_dim+2*num_ghosts); // plane
  if(jStride<BOX_PLANE_PADDING)kStride += (BOX_PLANE_PADDING-jStride); // allow the ghost zone to be clobbered...
  while(kStride % BOX_SIMD_ALIGNMENT)kStride++;

  boxVolume = (box_dim+2*num_ghosts)*kStride;
  totalMemSize = sizeof(box_type) + boxVolume*num_vecs*sizeof(double);

  ocrGuid_t currentAffinity = NULL_GUID;
  ocrHint_t myDbAffinityHNT;
  ocrHintInit( &myDbAffinityHNT, OCR_HINT_DB_T );

#ifdef ENABLE_EXTENSION_AFFINITY
  s64 affinityCount;
  ocrAffinityCount( AFFINITY_PD, &affinityCount );

#ifdef OCR_APP_OPTIMIZED_PLACEMENT
  ocrAffinityGetAt( AFFINITY_PD,
                    boxHomePD(box_num, lPtr->boxes_in.i, affinityCount),
                    &currentAffinity );
#else
  ocrAffinityGetAt( AFFINITY_PD, box_num%affinityCount, &currentAffinity );
#endif
#endif
  ocrSetHintValue( &myDbAffinityHNT, OCR_HINT_DB_AFFINITY, ocrAffinityToHintValue(currentAffinity) );
  ocrDbCreate(&boxGuid, (void**)&boxPtr, totalMemSize, 0, &myDbAffinityHNT, NO_ALLOC);

  bzero(boxPtr, totalMemSize);

  lPtr->jStride = jStride; lPtr->kStride = kStride, lPtr->volume = boxVolume;


  // assign offsets to level->alpha, beta_i.... no alignment at present
  if (lPtr->alpha != sizeof(box_type)) {
    lPtr->alpha = sizeof(box_type);
    lPtr->beta_i = lPtr->alpha + boxVolume*sizeof(double);
    lPtr->beta_j = lPtr->beta_i + boxVolume*sizeof(double);
    lPtr->beta_k =  lPtr->beta_j + boxVolume*sizeof(double);
    lPtr->Dinv =  lPtr->beta_k + boxVolume*sizeof(double);
    lPtr->L1inv = lPtr->Dinv + boxVolume*sizeof(double);
    lPtr->valid = lPtr->L1inv + boxVolume*sizeof(double);
    lPtr->u_true =  lPtr->valid + boxVolume*sizeof(double);
    lPtr->f = lPtr->u_true + boxVolume*sizeof(double);
    lPtr->f_Av = lPtr->f + boxVolume*sizeof(double);
    lPtr->u = lPtr->f_Av + boxVolume*sizeof(double);
    lPtr->vec_temp = lPtr->u + boxVolume*sizeof(double);
    box_type *const_box;
    ocrDbCreate(&lPtr->constant_box_guid, (void**)&const_box, totalMemSize, 0, &myDbAffinityHNT, NO_ALLOC);
    const_box->global_box_id = -1;

    bzero((char*)boxPtr + lPtr->u, (lPtr->volume)*sizeof(double));

#if DEBUG
    ocrPrintf("constant_box_gui = %u\n", lPtr->constant_box_guid);
#endif
  }

  ocrGuid_t *tBox;
  tBox  = (ocrGuid_t *)(((char*)lPtr)+ lPtr->boxes)+box_num;
  *tBox = boxGuid;

  return boxPtr;

}

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

void initialize_problem(level_type* level, double a, double b) {

  double hLevel = 1.0/( (double)level->boxes_in.i*(double)level->box_dim );
  level->h = hLevel;

  int box;
  for (box=0; box<level->num_boxes; box++)
    hpgmg_fill_box(level, level->temp[box], a, b);

  if(level->alpha_is_zero==-1)
    level->alpha_is_zero = (dot(level,level->alpha,level->alpha) == 0.0);


  if(level->boundary_condition == BC_PERIODIC) {
    double average_value_of_f = mean(level,level->f);
    if(average_value_of_f!=0.0) ocrPrintf("WARNING... Periodic boundary conditions, but f does not sum to zero... mean(f)=%f\n", average_value_of_f);
    if((a==0.0) || (level->alpha_is_zero==1) ) {
      double average_value_of_u = mean(level,level->u_true);
      ocrPrintf(" average value of u = %20.12f... shifting u to ensure it sums to zero...\n",average_value_of_u);
      shift_vector(level,level->u_true,level->u_true,-average_value_of_u);
      shift_vector(level,level->f,level->f,-average_value_of_f);
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

void rebuild_operator(level_type* level, level_type* fromLevel, double a, double b) {

  ocrPrintf(" rebuilding operator for level...  h=%e  ",level->h);


  // form restriction of alpha[], beta_*[] coefficients from fromLevel
  if(fromLevel != NULL) {
    int count = fromLevel->num_boxes/level->num_boxes;
    int fine[MAX_FINE_BOXES];
    int b,fb;
    for (b = 0; b < level->num_boxes; b++) {
      get_fine_box_ids(fromLevel, level, b, fine);
      for (fb = 0; fb < count; fb++)
        hpgmg_coeff_restrict_box(level, level->temp[b],
                                 fromLevel, fromLevel->temp[fine[fb]], fb, count);
    }
  }

  //  exchange alpha/beta/...  (must be done before calculating Dinv)
 int nbrs[26];
  int box;
 /* ToDo: may not be required due to the way we restrict!
  for (box = 0; box < level->num_boxes; box++) {
    double *alpha, *beta_i, *beta_j, *beta_k;
    get_neighbors(box, level->boxes_in.i, level->boxes_in.j, level->boxes_in.k, nbrs);
    alpha = (double*)(((char*)level->temp[box])+ level->alpha) + NUM_GHOSTS * (1+level->jStride+level->kStride);
    beta_i =  (double*)(((char*)level->temp[box])+ level->beta_i) + NUM_GHOSTS * (1+level->jStride+level->kStride);
    beta_j =  (double*)(((char*)level->temp[box])+ level->beta_j) + NUM_GHOSTS * (1+level->jStride+level->kStride);
    beta_k =  (double*)(((char*)level->temp[box])+ level->beta_k) + NUM_GHOSTS * (1+level->jStride+level->kStride);
    int bi;
    for (bi = 0; bi < 6; bi++) {
//         ocrPrintf("box %d, Nbox %d\n", box, nbrs[bi]);
      if (nbrs[bi] < 0 || nbrs[bi] >= level->num_boxes)
        continue;
      else {
       populate_boundary(level,alpha,level->temp[nbrs[bi]],level->alpha,bi);
	populate_boundary(level,beta_i,level->temp[nbrs[bi]],level->beta_i,bi);
	populate_boundary(level,beta_j,level->temp[nbrs[bi]],level->beta_j,bi);
	populate_boundary(level,beta_k,level->temp[nbrs[bi]],level->beta_k,bi);
      }
    }
  }
*/

  double dominant_eigenvalue = -1e9;

  for(box=0;box<level->num_boxes;box++) {
    double box_eigenvalue = hpgmg_gershgorin_box(level, level->temp[box], a, b);
    if(box_eigenvalue>dominant_eigenvalue){
      dominant_eigenvalue = box_eigenvalue;
    }
  }


  ocrPrintf("eigenvalue_max < %f\n",dominant_eigenvalue);
  level->dominant_eigenvalue_of_DinvA = dominant_eigenvalue;

/*
  // exchange Dinv/L1inv/...
  for (box = 0; box < level->num_boxes; box++) {
    double *Dinv, *L1inv;
    get_neighbors(box, level->boxes_in.i, level->boxes_in.j, level->boxes_in.k, nbrs);
    Dinv = (double*)(((char*)level->temp[box])+ level->Dinv) + NUM_GHOSTS * (1+level->jStride+level->kStride);
    L1inv =  (double*)(((char*)level->temp[box])+ level->L1inv) + NUM_GHOSTS * (1+level->jStride+level->kStride);
    int bi;
    for (bi = 0; bi < 6; bi++) {
      if (nbrs[bi] < 0 || nbrs[bi] >= level->num_boxes)
        continue;
      else {
        populate_boundary(level,Dinv,level->temp[nbrs[bi]],level->Dinv,bi);
	populate_boundary(level,L1inv,level->temp[nbrs[bi]],level->L1inv,bi);
      }
    }
  }
*/
  // exchange Dinv/L1inv/...
  for (box = 0; box < level->num_boxes; box++) {
    double *Dinv, *L1inv;
    get_neighbors_all(box, level->boxes_in.i, level->boxes_in.j, level->boxes_in.k, nbrs);
    Dinv = (double*)(((char*)level->temp[box])+ level->Dinv) + NUM_GHOSTS * (1+level->jStride+level->kStride);
    L1inv =  (double*)(((char*)level->temp[box])+ level->L1inv) + NUM_GHOSTS * (1+level->jStride+level->kStride);
    int bi;
    for (bi = 0; bi < 26; bi++) {
      if (nbrs[bi] == -1)
        continue;
      else {
        update_boundary_all(level,level->temp[box],level->Dinv,level->temp[nbrs[bi]],level->Dinv,0);
        update_boundary_all(level,level->temp[box],level->L1inv,level->temp[nbrs[bi]],level->L1inv,0);
      }
    }
  }



}


void mg_build(mg_type* all_grids, level_type* fine_grid, double a, double b, int minCoarseGridDim) {

  int maxLevels = MG_MAXLEVELS;
  int      dim_i[MG_MAXLEVELS];
  int boxes_in_i[MG_MAXLEVELS];
  int    box_dim[MG_MAXLEVELS];
  int box_ghosts[MG_MAXLEVELS];

  int level=1;
  int coarse_dim = fine_grid->dim.i;

  while( (coarse_dim>=2*minCoarseGridDim) && ((coarse_dim&0x1)==0) ) { // grid dimension is even and big enough...
    level++;
    coarse_dim = coarse_dim / 2;
  }

  ocrPrintf("mg_build: maxLevels: %d, %d\n", level,coarse_dim);
  if(level<maxLevels)maxLevels=level;

       dim_i[0] = fine_grid->dim.i;
  boxes_in_i[0] = fine_grid->boxes_in.i;
     box_dim[0] = fine_grid->box_dim;
  box_ghosts[0] = NUM_GHOSTS; //fine_grid->box_ghosts;

  // build the list of levels...
  all_grids->num_levels=1;

  // build a table to guide the construction of the v-cycle...
  int doRestrict=1;if(maxLevels<2)doRestrict=0; // i.e. can't restrict if there is only one level !!!
  #ifdef USE_UCYCLES
  while(doRestrict){
    level = all_grids->num_levels;
    doRestrict=0;
    if( (box_dim[level-1] % 2 == 0) ){
           dim_i[level] =      dim_i[level-1]/2;
         box_dim[level] =    box_dim[level-1]/2;
      boxes_in_i[level] = boxes_in_i[level-1];
      box_ghosts[level] = box_ghosts[level-1];
             doRestrict = 1;
    }
    if(box_dim[level] < box_ghosts[level])doRestrict=0;
    if(doRestrict)all_grids->num_levels++;
  }
  #else // TRUE V-Cycle...
  while(doRestrict){
    level = all_grids->num_levels;
    doRestrict=0;
    int fine_box_dim    =    box_dim[level-1];
    int fine_dim_i      =      dim_i[level-1];
    int fine_boxes_in_i = boxes_in_i[level-1];
    int stencil_radius  = box_ghosts[level-1]; // FIX tune the number of ghost zones...
#if DEBUG
    ocrPrintf("fine_dim: %u, fine_dim_i: %u, fine_boxes_in_i: %u, stencil_radius: %u\n", fine_box_dim, fine_dim_i, fine_boxes_in_i, stencil_radius);
#endif
    if( (fine_box_dim % 2 == 0) && (fine_box_dim > MG_AGGLOMERATION_START) ){ // Boxes are too big to agglomerate
           dim_i[level] = fine_dim_i/2;
         box_dim[level] = fine_box_dim/2;
      boxes_in_i[level] = fine_boxes_in_i;
      box_ghosts[level] = stencil_radius;
             doRestrict = 1;
    }else
    if( (fine_boxes_in_i % 2 == 0) ){ // 8:1 box agglomeration
           dim_i[level] = fine_dim_i/2;
         box_dim[level] = fine_box_dim;
      boxes_in_i[level] = fine_boxes_in_i/2;
      box_ghosts[level] = stencil_radius;
             doRestrict = 1;
    }else
    if( (coarse_dim != 1) && (fine_dim_i == 2*coarse_dim) ){ // agglomerate everything
           dim_i[level] = fine_dim_i/2;
         box_dim[level] = fine_dim_i/2;
      boxes_in_i[level] = 1;
      box_ghosts[level] = stencil_radius;
             doRestrict = 1;
    }else
    if( (coarse_dim != 1) && (fine_dim_i == 4*coarse_dim) ){ // restrict box dimension, and run on fewer ranks
           dim_i[level] = fine_dim_i/2;
         box_dim[level] = fine_box_dim/2;
      boxes_in_i[level] = fine_boxes_in_i;
      box_ghosts[level] = stencil_radius;
             doRestrict = 1;
    }else
    if( (coarse_dim != 1) && (fine_dim_i == 8*coarse_dim) ){ // restrict box dimension, and run on fewer ranks
           dim_i[level] = fine_dim_i/2;
         box_dim[level] = fine_box_dim/2;
      boxes_in_i[level] = fine_boxes_in_i;
      box_ghosts[level] = stencil_radius;
             doRestrict = 1;
    }else
    if( (fine_box_dim % 2 == 0) ){ // restrict box dimension, and run on the same number of ranks
           dim_i[level] = fine_dim_i/2;
         box_dim[level] = fine_box_dim/2;
      boxes_in_i[level] = fine_boxes_in_i;
      box_ghosts[level] = stencil_radius;
             doRestrict = 1;
    }
    if(box_dim[level] < stencil_radius)doRestrict=0;
    if(doRestrict)all_grids->num_levels++;
  }
  #endif

  all_grids->max_levels = all_grids->num_levels;
  // now build all the coarsened levels...
  double h = fine_grid->h;
  level_type **all_levels;
  ocrGuid_t all_levels_guid;
  ocrDbCreate(&all_levels_guid, (void**)&all_levels, sizeof(level_type *)*all_grids->num_levels, 0,NULL_HINT,NO_ALLOC);
  all_levels[0] = fine_grid;

  for(level=1;level<all_grids->num_levels;level++){
    level_type *level_ptr;
    level_ptr = create_level(all_grids,box_dim[level],boxes_in_i[level],fine_grid->boundary_condition,level);
    level_ptr->h = 2.0*h;
    h = level_ptr->h;
    all_levels[level] = level_ptr;
  }

/* // ToDo: Manu: may not be required
  // bottom solver gets extra grids...
  level = all_grids->num_levels-1;
  int box;
  int numAdditionalVectors = IterativeSolver_NumVectors();
  all_grids->levels[level]->box_vectors += numAdditionalVectors;
  if(numAdditionalVectors){
    for(box=0;box<all_grids->levels[level]->num_my_boxes;box++){
      add_vectors_to_box(all_grids->levels[level]->my_boxes+box,numAdditionalVectors);
      all_grids->levels[level]->memory_allocated += numAdditionalVectors*all_grids->levels[level]->my_boxes[box].volume*sizeof(double);
    }
  }
*/


  // rebuild various coefficients for the operator... must occur after build_restriction !!!
  for(level=1;level<all_grids->num_levels;level++){
    rebuild_operator(all_levels[level],all_levels[level-1],a,b);
  }


  // used for quick test for poisson
  for(level=0;level<all_grids->num_levels;level++){
    all_levels[level]->alpha_is_zero = (dot(all_levels[level],all_levels[level]->alpha, all_levels[level]->alpha) == 0.0);
  }
}
