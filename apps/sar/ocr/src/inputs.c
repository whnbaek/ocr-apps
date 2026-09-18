#include <stdint.h>

/* Radar-parameters file consumed by the runtime-input build; the working-
 * directory default matches the historical behavior, callers may point it
 * at any parameter file before ReadParams(). */
const char *rag_params_path = "Parameters.txt";

#include "ocr.h"
#include "rag_ocr.h"
#include "common.h"

int ReadParams(struct RadarParams *radar_params, struct ImageParams *image_params, struct AffineParams *affine_params, struct CfarParams *cfar_params)
{
#ifdef RAG_IMPLICIT_INPUTS
#include "Parameters.h"
  radar_params->fs        = RAG_Fs;
  radar_params->fc        = RAG_Fc;
  radar_params->PRF       = RAG_PRF;
  image_params->F         = RAG_F;
  image_params->Ix        = RAG_Ix;
  image_params->Iy        = RAG_Iy;
  image_params->Sx        = RAG_Sx;
  image_params->Sy        = RAG_Sy;
  image_params->Fbp       = RAG_Fbp;
  image_params->P1        = RAG_P1;
  image_params->S1        = RAG_S1;
  radar_params->r0        = RAG_r0;
  radar_params->R0        = RAG_R0;
  affine_params->Nc       = RAG_Nc;
  affine_params->Rc       = RAG_Rc;
  affine_params->Sc       = RAG_Sc;
  affine_params->Tc       = RAG_Tc;
  image_params->Ncor      = RAG_Ncor;
  cfar_params->Ncfar      = RAG_Ncfar;
  cfar_params->Tcfar      = RAG_Tcfar;
  cfar_params->Nguard     = RAG_Nguard;
  image_params->numImages = RAG_NumberImages;
  image_params->imageNumber=0; // RAG
  radar_params->fs        = RAG_Fs;
#else // RAG_IMPLICIT_INPUTS
  FILE *pFile;
  char param[80];

  pFile = fopen(rag_params_path, "r");

  if(pFile == NULL) {
    return 1;
  }

  fscanf(pFile, "%s %f", param, &radar_params->fs);
  if(strcmp(param, "Fs") != 0) {
#ifndef TG_ARCH
    fprintf(stderr,"Fs\n");
#endif
    return 2;
  }
  fscanf(pFile, "%s %f", param, &radar_params->fc);
  if(strcmp(param, "Fc") != 0) {
#ifndef TG_ARCH
    fprintf(stderr,"Fc\n");
#endif
    return 2;
  }
  fscanf(pFile, "%s %f", param, &radar_params->PRF);
  if(strcmp(param, "PRF") != 0) {
#ifndef TG_ARCH
    fprintf(stderr,"PRF\n");
#endif
    return 2;
  }
  fscanf(pFile, "%s %d", param, &image_params->F);
  if(strcmp(param, "F") != 0) {
#ifndef TG_ARCH
    fprintf(stderr,"F\n");
#endif
    return 2;
  }
  fscanf(pFile, "%s %d", param, &image_params->Ix);
  if(strcmp(param, "Ix") != 0) {
#ifndef TG_ARCH
    fprintf(stderr,"Ix\n");
#endif
    return 2;
  }
  fscanf(pFile, "%s %d", param, &image_params->Iy);
  if(strcmp(param, "Iy") != 0) {
#ifndef TG_ARCH
    fprintf(stderr,"Iy\n");
#endif
    return 2;
  }
  fscanf(pFile, "%s %d", param, &image_params->Sx);
  if(strcmp(param, "Sx") != 0) {
#ifndef TG_ARCH
    fprintf(stderr,"Sx\n");
#endif
    return 2;
  }
  fscanf(pFile, "%s %d", param, &image_params->Sy);
  if(strcmp(param, "Sy") != 0) {
#ifndef TG_ARCH
    fprintf(stderr,"Sy\n");
#endif
    return 2;
  }
  fscanf(pFile, "%s %d", param, &image_params->Fbp);
  if(strcmp(param, "Fbp") != 0) {
#ifndef TG_ARCH
    fprintf(stderr,"Fbp\n");
#endif
    return 2;
  }
  fscanf(pFile, "%s %d", param, &image_params->P1);
  if(strcmp(param, "P1") != 0) {
#ifndef TG_ARCH
    fprintf(stderr,"P1\n");
#endif
    return 2;
  }
  fscanf(pFile, "%s %d", param, &image_params->S1);
  if(strcmp(param, "S1") != 0) {
#ifndef TG_ARCH
    fprintf(stderr,"S1\n");
#endif
    return 2;
  }
  fscanf(pFile, "%s %f", param, &radar_params->r0);
  if(strcmp(param, "r0") != 0) {
#ifndef TG_ARCH
    fprintf(stderr,"r0\n");
#endif
    return 2;
  }
  fscanf(pFile, "%s %f", param, &radar_params->R0);
  if(strcmp(param, "R0") != 0) {
#ifndef TG_ARCH
    fprintf(stderr,"R0\n");
#endif
    return 2;
  }
  fscanf(pFile, "%s %d", param, &affine_params->Nc);
  if(strcmp(param, "Nc") != 0) {
#ifndef TG_ARCH
    fprintf(stderr,"Nc\n");
#endif
    return 2;
  }
  fscanf(pFile, "%s %d", param, &affine_params->Rc);
  if(strcmp(param, "Rc") != 0) {
#ifndef TG_ARCH
    fprintf(stderr,"Rc\n");
#endif
    return 2;
  }
  fscanf(pFile, "%s %d", param, &affine_params->Sc);
  if(strcmp(param, "Sc") != 0) {
#ifndef TG_ARCH
    fprintf(stderr,"Sc\n");
#endif
    return 2;
  }
  fscanf(pFile, "%s %f", param, &affine_params->Tc);
  if(strcmp(param, "Tc") != 0) {
#ifndef TG_ARCH
    fprintf(stderr,"Tc\n");
#endif
    return 2;
  }

  fscanf(pFile, "%s %d", param, &image_params->Ncor);
  if(strcmp(param, "Ncor") != 0) {
#ifndef TG_ARCH
    fprintf(stderr,"Ncor\n");
#endif
    return 2;
  }
  fscanf(pFile, "%s %d", param, &cfar_params->Ncfar);
  if(strcmp(param, "Ncfar") != 0) {
#ifndef TG_ARCH
    fprintf(stderr,"Ncfar\n");
#endif
    return 2;
  }
  fscanf(pFile, "%s %f", param, &cfar_params->Tcfar);
  if(strcmp(param, "Tcfar") != 0) {
#ifndef TG_ARCH
    fprintf(stderr,"Tcfar\n");
#endif
    return 2;
  }
  fscanf(pFile, "%s %d", param, &cfar_params->Nguard);
  if(strcmp(param, "Nguard") != 0) {
#ifndef TG_ARCH
    fprintf(stderr,"Nguard\n");
#endif
    return 2;
  }
  fscanf(pFile, "%s %d", param, &image_params->numImages);
  if(strcmp(param, "NumberImages") != 0) {
#ifndef TG_ARCH
    fprintf(stderr,"NumberImages\n");
#endif
    return 2;
  }

  fclose(pFile);

#endif // RAG_IMPLICIT_INPUTS

  image_params->imageNumber=0; // RAG

  return 0;
}


#ifndef RAG_IMPLICIT_INPUTS
/* Reading the pulse history is not the exhibit, but it is inside the measured
 * window, so it is read in parallel.  The three input blocks are one object
 * each and their consumers read them whole, so the readers cannot own separate
 * pieces: concurrent writers to disjoint regions of one block are defined
 * within a node and undefined across one, and every slab is therefore placed
 * on the node that runs the task that hands the blocks on.  Without an affinity
 * extension there is no way to express that placement, and the read would be a
 * cross-node write race on a single object, so the build is refused rather than
 * silently producing an undefined image. */
#ifndef ENABLE_EXTENSION_AFFINITY
#error "the parallel input read needs OCR_HINT_EDT_AFFINITY: concurrent writers to disjoint regions of one datablock are defined only within a node"
#endif

#define RAG_READ_SLABS 128

typedef struct { u64 p0, p1, img; } readSlabPRM_t;
typedef struct { ocrGuid_t arg_scg, X_dbg, Pt_dbg, Tp_dbg; } readJoinPRM_t;

ocrGuid_t read_slab_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv) {
  readSlabPRM_t *p = (readSlabPRM_t *)paramv;
  assert(paramc==PRMNUM(readSlab));
  assert(depc==5);
  RAG_REF_MACRO_SPAD(struct ImageParams,image_params,image_params_ptr,image_params_lcl,image_params_dbg,0);
  RAG_REF_MACRO_SPAD(struct file_args_t,file_args,file_args_ptr,file_args_lcl,file_args_dbg,1);
  RAG_REF_MACRO_BSM( struct complexData **,X,NULL,NULL,X_dbg,2);
  RAG_REF_MACRO_BSM( float **,Pt,NULL,NULL,Pt_dbg,3);
  RAG_REF_MACRO_BSM( float *,Tp,NULL,NULL,Tp_dbg,4);
  const long P1 = image_params->P1;
  const long S1 = image_params->S1;
  const long p0 = (long)p->p0, np = (long)(p->p1 - p->p0);
  /* A packed 2-D block lays its row-pointer table ahead of the payload; a slab
     writes the payload rows it owns and leaves the table untouched. */
  struct complexData *Xd  = (struct complexData *)&X[P1];
  float              *Ptd = (float *)&Pt[P1];
  FILE *f1 = fopen(file_args_lcl.in_data,      "rb");
  FILE *f2 = fopen(file_args_lcl.in_platpos,   "rb");
  FILE *f3 = fopen(file_args_lcl.in_pulsetime, "rb");
  if( f1 == NULL || f2 == NULL || f3 == NULL ) {
    ocrPrintf("Error opening %s / %s / %s\n", file_args_lcl.in_data,
              file_args_lcl.in_platpos, file_args_lcl.in_pulsetime);RAG_FLUSH;
    xe_exit(1);
  }
  const long base = (long)p->img*P1 + p0;
  fseek(f1, base*S1*(long)sizeof(struct complexData), SEEK_SET);
  fseek(f2, base*3L*(long)sizeof(float),              SEEK_SET);
  fseek(f3, base*(long)sizeof(float),                 SEEK_SET);
  fread(&Xd[(size_t)p0*(size_t)S1], sizeof(struct complexData), (size_t)np*(size_t)S1, f1);
  fread(&Ptd[(size_t)p0*3],         sizeof(float),              (size_t)np*3,          f2);
  fread(&Tp[p0],                    sizeof(float),              (size_t)np,            f3);
  fclose(f1); fclose(f2); fclose(f3);
  return NULL_GUID;
}

/* The successor is wired only once every slab is in: a data-block dependence
 * satisfies its slot immediately, so wiring from the spawner would let the
 * consumer acquire the blocks while they are still being filled. */
ocrGuid_t read_join_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv) {
  int retval;
  readJoinPRM_t *p = (readJoinPRM_t *)paramv;
  assert(paramc==PRMNUM(readJoin));
  (void)depc; (void)depv;
  RAG_DEF_MACRO_SPAD_RO(p->arg_scg,NULL,NULL,NULL,NULL,p->X_dbg,4);
  RAG_DEF_MACRO_SPAD_RO(p->arg_scg,NULL,NULL,NULL,NULL,p->Pt_dbg,5);
  RAG_DEF_MACRO_SPAD_RO(p->arg_scg,NULL,NULL,NULL,NULL,p->Tp_dbg,6);
  return NULL_GUID;
}
#endif // RAG_IMPLICIT_INPUTS

// ReadData edt

ocrGuid_t ReadData_edt(uint32_t paramc, uint64_t *paramv, uint32_t depc, ocrEdtDep_t *depv) {
  int retval;
  readDataPRM_t *readDataParamvIn = (readDataPRM_t *)paramv;
#ifdef TRACE_LVL_2
  ocrPrintf("//// enter ReadData_edt\n");RAG_FLUSH;
#endif
  assert(paramc==PRMNUM(readData));
  ocrGuid_t arg_scg = readDataParamvIn->arg_scg;; // FormImage_scg or refFormImage_scg
  assert(depc==5);
  RAG_REF_MACRO_SPAD(struct ImageParams,image_params,image_params_ptr,image_params_lcl,image_params_dbg,0);
  RAG_REF_MACRO_SPAD(struct file_args_t,file_args,file_args_ptr,file_args_lcl,file_args_dbg,1);
  RAG_REF_MACRO_BSM( struct complexData **,X,NULL,NULL,X_dbg,2);
  RAG_REF_MACRO_BSM( float **,Pt,NULL,NULL,Pt_dbg,3);
  RAG_REF_MACRO_BSM( float *,Tp,NULL,NULL,Tp_dbg,4);

#ifdef RAG_IMPLICIT_INPUTS
  // The row-pointer tables are absolute and go stale when the block is
  // relocated into this EDT's address space; rebuild them against the current
  // base before writing the pulse/platform rows below.
  RAG_REMAP_2D(X,  image_params_lcl.P1, image_params_lcl.S1, struct complexData);
  RAG_REMAP_2D(Pt, image_params_lcl.P1, 3,                    float);
#ifdef TG_ARCH
  void *pInFile  = file_args_lcl.pInFile;
  void *pInFile2 = file_args_lcl.pInFile2;
  void *pInFile3 = file_args_lcl.pInFile3;
  void ReadData(struct ImageParams *image_params, void *pFile1, void *pFile2, void *pFile3,
                struct complexData **X, float **Pt, float *Tp);
#else
  FILE *pInFile = NULL, *pInFile2 = NULL, *pInFile3 = NULL;
  void ReadData(struct ImageParams *image_params, FILE *pFile1, FILE *pFile2, FILE *pFile3,
                struct complexData **X, float **Pt, float *Tp);
#endif
  ReadData(image_params, pInFile, pInFile2, pInFile3, X, Pt, Tp);
#else // RAG_IMPLICIT_INPUTS
  (void)file_args; (void)X; (void)Pt; (void)Tp;
  /* Image i occupies the i-th contiguous run of P1 pulses in each of the three
     files, so a slab's offset is a pure function of its pulse range. */
  const int img = image_params->imageNumber;
  image_params->imageNumber++;
  int nslab = RAG_READ_SLABS;
  if(nslab > image_params->P1) nslab = image_params->P1;
  if(nslab < 1) nslab = 1;
#endif // RAG_IMPLICIT_INPUTS

#ifdef TG_ARCH
  SPADtoBSM(image_params_ptr,image_params,sizeof(struct ImageParams));
#else
  memcpy(image_params_ptr,image_params,sizeof(struct ImageParams));
#endif

  // Release the freshly written blocks BEFORE wiring them into the successor:
  // a data-block dependence satisfies the pre-slot immediately, so the
  // successor may start (and acquire) before this task's implicit end-of-task
  // release — and the memory model only guarantees visibility of writes whose
  // release ordered before the reader's acquire.
  retval = ocrDbRelease(image_params_dbg); assert(retval==0);
  retval = ocrDbRelease(X_dbg);            assert(retval==0);
  retval = ocrDbRelease(Pt_dbg);           assert(retval==0);
  retval = ocrDbRelease(Tp_dbg);           assert(retval==0);

#ifdef RAG_IMPLICIT_INPUTS
  RAG_DEF_MACRO_SPAD_RO(arg_scg,NULL,NULL,NULL,NULL,X_dbg,4);
  RAG_DEF_MACRO_SPAD_RO(arg_scg,NULL,NULL,NULL,NULL,Pt_dbg,5);
  RAG_DEF_MACRO_SPAD_RO(arg_scg,NULL,NULL,NULL,NULL,Tp_dbg,6);
#else
  /* Every slab must land on this node; a slab placed elsewhere would be a
     cross-node concurrent writer to one datablock, whose result is undefined. */
  ocrHint_t _hnt; ocrHint_t *_hp;
  {
    ocrGuid_t _aff = NULL_GUID;
    if(ocrAffinityGetCurrent(&_aff) != 0) {
      ocrPrintf("Error reading the current affinity; the parallel input read"
                " cannot place its slabs on one node\n");RAG_FLUSH;
      xe_exit(1);
    }
    ocrHintInit(&_hnt, OCR_HINT_EDT_T);
    ocrSetHintValue(&_hnt, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(_aff));
    _hp = &_hnt;
  }
  ocrGuid_t _join_clg, _join_scg;
  readJoinPRM_t _jp;
  _jp.arg_scg = arg_scg; _jp.X_dbg = X_dbg; _jp.Pt_dbg = Pt_dbg; _jp.Tp_dbg = Tp_dbg;
  retval = ocrEdtTemplateCreate(&_join_clg, read_join_edt, PRMNUM(readJoin), nslab);
  assert(retval==0);
  RAG_TEMPLATE_REGISTER(_join_clg);
  retval = ocrEdtCreate(&_join_scg, _join_clg, EDT_PARAM_DEF, (u64 *)&_jp,
                        EDT_PARAM_DEF, NULL, EDT_PROP_NONE, _hp, NULL);
  assert(retval==0);

  ocrGuid_t _slab_clg;
  retval = ocrEdtTemplateCreate(&_slab_clg, read_slab_edt, PRMNUM(readSlab), 5);
  assert(retval==0);
  RAG_TEMPLATE_REGISTER(_slab_clg);
  for(int _i = 0; _i < nslab; _i++) {
    readSlabPRM_t _sp;
    _sp.p0  = (u64)((long)image_params->P1 * _i       / nslab);
    _sp.p1  = (u64)((long)image_params->P1 * (_i + 1) / nslab);
    _sp.img = (u64)img;
    ocrGuid_t _slab_scg, _slab_evg;
    retval = ocrEdtCreate(&_slab_scg, _slab_clg, EDT_PARAM_DEF, (u64 *)&_sp,
                          EDT_PARAM_DEF, NULL, EDT_PROP_NONE, _hp, &_slab_evg);
    assert(retval==0);
    retval = ocrAddDependence(_slab_evg, _join_scg, _i, DB_MODE_NULL);
    assert(retval==0);
    RAG_DEF_MACRO_SPAD_RO(_slab_scg,NULL,NULL,NULL,NULL,image_params_dbg,0);
    RAG_DEF_MACRO_SPAD_RO(_slab_scg,NULL,NULL,NULL,NULL,file_args_dbg,1);
    RAG_DEF_MACRO_BSM(_slab_scg,NULL,NULL,NULL,NULL,X_dbg,2);
    RAG_DEF_MACRO_BSM(_slab_scg,NULL,NULL,NULL,NULL,Pt_dbg,3);
    RAG_DEF_MACRO_BSM(_slab_scg,NULL,NULL,NULL,NULL,Tp_dbg,4);
  }
#endif // RAG_IMPLICIT_INPUTS

#ifdef TRACE_LVL_2
  ocrPrintf("//// leave ReadData_edt\n");RAG_FLUSH;
#endif
  return NULL_GUID;
}

// ReadData

#ifndef TG_ARCH
void ReadData(struct ImageParams *image_params, FILE *pFile1, FILE *pFile2, FILE *pFile3,
              struct complexData **X, float **Pt, float *Tp)
#else
void ReadData(struct ImageParams *image_params, void *pFile1, void *pFile2, void *pFile3,
              struct complexData **X, float **Pt, float *Tp)
#endif
{
#ifdef TRACE_LVL_2
  ocrPrintf("//// enter ReadData()\n");RAG_FLUSH;
#endif

#ifdef RAG_IMPLICIT_INPUTS
  extern SHARED struct {int32_t real; int32_t imag;} image_0[RAG_P1][RAG_S1];
  extern SHARED struct {int32_t real; int32_t imag;} image_1[RAG_P1][RAG_S1];
  extern SHARED struct {int32_t x; int32_t y; int32_t z;} platform_0[RAG_P1][1];
  extern SHARED struct {int32_t x; int32_t y; int32_t z;} platform_1[RAG_P1][1];
  extern SHARED struct {int32_t t; } pulse_0[RAG_P1][1];
  extern SHARED struct {int32_t t; } pulse_1[RAG_P1][1];
#ifdef TRACE_LVL_2
  ocrPrintf("//// RAG_IMPLICIT_INPUTS P1 = %d S1 = %d\n",image_params->P1,image_params->S1);RAG_FLUSH;
#endif
#endif

  for(int m=0; m<image_params->P1; m++) {
#ifdef TRACE_LVL_2
    if(!m)ocrPrintf("//// Read complex SAR data\n");RAG_FLUSH;
#endif
#ifndef RAG_IMPLICIT_INPUTS
    fread(&X[m][0], sizeof(struct complexData), image_params->S1, pFile1);
#else
    if ( image_params->imageNumber == 0 ) {
      // &X[m][0]
      GlobalPtrToDataBlock(X[m],&image_0[m][0], sizeof(struct complexData)*image_params->S1);
    } else if ( image_params->imageNumber == 1 ) {
      // &X[m][0]
      GlobalPtrToDataBlock(X[m],&image_1[m][0], sizeof(struct complexData)*image_params->S1);
    } // endif image
#endif
#ifdef TRACE_LVL_2
    if(!m){ocrPrintf("//// Read platform positions\n");RAG_FLUSH;}
#endif
#ifndef RAG_IMPLICIT_INPUTS
    fread(&Pt[m][0], sizeof(float), 3, pFile2);
#else
    if ( image_params->imageNumber == 0 ) {
      // &Pt[m][0]
      GlobalPtrToDataBlock(Pt[m],&platform_0[m][0], sizeof(float)*3);
    } else if ( image_params->imageNumber == 1 ) {
      // &Pt[m][0]
      GlobalPtrToDataBlock(Pt[m],&platform_1[m][0], sizeof(float)*3);
    } // endif image
#endif
  } // for m (P1)
#ifdef TRACE_LVL_2
  ocrPrintf("//// Read pulse transmission timestamps\n");RAG_FLUSH;
#endif
#ifndef RAG_IMPLICIT_INPUTS
  fread(Tp, sizeof(float), image_params->P1, pFile3);
#else
  for(int m=0; m<image_params->P1; m++) {
    if ( image_params->imageNumber == 0 ) {
      // &Tp[m]
      GlobalPtrToDataBlock(Tp+m,&pulse_0[m][0].t, sizeof(float)*1);
    } else if ( image_params->imageNumber == 1 ) {
      // &Tp[m]
      GlobalPtrToDataBlock(Tp+m,&pulse_1[m][0].t, sizeof(float)*1);
    } // endif image
  } // for m (P1)
#endif
  /* Advance to the next image: the runtime-input build seeks to this index's
   * file slice, the implicit build selects the matching embedded arrays.  The
   * caller writes ImageParams back, so the ordered next ReadData sees it. */
  image_params->imageNumber++;
#ifdef TRACE_LVL_2
  ocrPrintf("//// leave ReadData()\n");RAG_FLUSH;
#endif
}
