#include <stdlib.h>
#include <math.h>
#include <ocr.h>

#include "amr.h"


/* The object trajectory is the only thing outside the mesh that decides how
 * the mesh changes, and it is a closed-form function of the timestep, so
 * every task that needs it advances the same array the same way. */
void object_move(const control* c, object* obj)
{
  for(s32 i = 0; i < c->num_objects; ++i) {
    for(s32 j = 0; j < 3; ++j) {
      obj[i].cen[j] += obj[i].move[j];
      if(obj[i].bounce) {
        if(obj[i].cen[j] >= 1.0) {
          obj[i].cen[j] = 2.0 - obj[i].cen[j];
          obj[i].move[j] = -obj[i].move[j];
        } else if(obj[i].cen[j] <= 0.0) {
          obj[i].cen[j] = 0.0 - obj[i].cen[j];
          obj[i].move[j] = -obj[i].move[j];
        }
      }
      obj[i].size[j] += obj[i].inc[j];
    }
  }
}

/* Does any object's surface or volume meet the box `cor`?  Verbatim geometry
 * from the published port: the object catalogue (rectangles, spheroids and
 * hemispheroids in six orientations, cylinders on three axes) and the
 * short-circuit on the first object that answers yes. */
static u8 check_block(const control* ctl, const object* obj, double cor[3][2])
{
  const object* op;
  s32 o, ca, c1, c2, xc, xv, yc, yv, zc, zv;
  u8 doesIntersect, tmp;

  doesIntersect = 0;
   for (o = 0; o < ctl->num_objects; o++) {
      op = &obj[o];
      if (doesIntersect || op->size[0] < 0.0 || op->size[1] < 0.0 || op->size[2] < 0) {
         /* skip since already determined that it will be refined or
          * can not be or object has size less than zero */
         ;
      } else if (op->type == 0) {   /* surface of rectangle */
         if (cor[0][1] > (op->cen[0] - op->size[0]) &&
             cor[0][0] < (op->cen[0] + op->size[0])) {
            /* some portion of block intersects with rectangle in x
             * 4 cases - the two that straddle can be treated the same
             * and the two that do not can also be treated the same */
            if ((cor[0][0] < (op->cen[0] - op->size[0]) &&
                 cor[0][1] < (op->cen[0] + op->size[0])) ||
                (cor[0][0] > (op->cen[0] - op->size[0]) &&
                 cor[0][1] > (op->cen[0] + op->size[0]))) {
               /* one of rectangle boundary between block sides */
               if (cor[1][1] > (op->cen[1] - op->size[1]) &&
                   cor[1][0] < (op->cen[1] + op->size[1]) &&
                   cor[2][1] > (op->cen[2] - op->size[2]) &&
                   cor[2][0] < (op->cen[2] + op->size[2])) {
                  /* some portion of block intersects rectangle in y,z */
                  doesIntersect = 1;
               }
            } else {
               /* rectangle in block (or vice-versa) in x */
               if (cor[1][1] > (op->cen[1] - op->size[1]) &&
                   cor[1][0] < (op->cen[1] + op->size[1])) {
                  /* some portion of block intersects rectangle in y */
                  if ((cor[1][0] < (op->cen[1] - op->size[1]) &&
                       cor[1][1] < (op->cen[1] + op->size[1])) ||
                      (cor[1][0] > (op->cen[1] - op->size[1]) &&
                       cor[1][1] > (op->cen[1] + op->size[1]))) {
                      if (cor[2][1] > (op->cen[2] - op->size[2]) &&
                          cor[2][0] < (op->cen[2] + op->size[2])) {
                         /* portion of block intersects rectangle in z */
                         doesIntersect = 1;
                      }
                  }
               } else {
                  /* rectangle in block (or vice-versa) in x and y */
                  if (cor[2][1] > (op->cen[2] - op->size[2]) &&
                      cor[2][0] < (op->cen[2] + op->size[2])) {
                     if ((cor[2][0] < (op->cen[2] - op->size[2]) &&
                          cor[2][1] < (op->cen[2] + op->size[2])) ||
                         (cor[2][0] > (op->cen[2] - op->size[2]) &&
                          cor[2][1] > (op->cen[2] + op->size[2]))) {
                        /* portion of block intersects rectangle in z */
                        doesIntersect = 1;
                     }
                  } /* else case need not be considered */
               }
            }
         }
      } else if (op->type == 1) {   /* solid rectangle */
         if (cor[0][1] > (op->cen[0] - op->size[0]) &&
             cor[0][0] < (op->cen[0] + op->size[0]) &&
             cor[1][1] > (op->cen[1] - op->size[1]) &&
             cor[1][0] < (op->cen[1] + op->size[1]) &&
             cor[2][1] > (op->cen[2] - op->size[2]) &&
             cor[2][0] < (op->cen[2] + op->size[2])) {
            doesIntersect = 1;
         }
      } else if (op->type >= 2 && op->type <= 14 && !(op->type%2)) {
         /* boundary of spheroid or hemispheroid */
         /* determine where center is and nearest and furthest
          * verticies and then determine if boundary is between them
          * 1 = (x/a)^2 + (y/b)^2 + (z/c)^2 is boundary */
         tmp = doesIntersect;
         xc = yc = zc = 0;
         if (op->cen[0] < cor[0][0]) {
            xv = 0;
         } else if (op->cen[0] > cor[0][1]) {
            xv = 1;
         } else {
            xc = 1;
            if (op->cen[0] < (cor[0][0] + cor[0][1])/2.0) {
               xv = 0;
            } else {
               xv = 1;
            }
         }
         if (op->cen[1] < cor[1][0]) {
            yv = 0;
         } else if (op->cen[1] > cor[1][1]) {
            yv = 1;
         } else {
            yc = 1;
            if (op->cen[1] < (cor[1][0] + cor[1][1])/2.0) {
               yv = 0;
            } else {
               yv = 1;
            }
         }
         if (op->cen[2] < cor[2][0]) {
            zv = 0;
         } else if (op->cen[2] > cor[2][1]) {
            zv = 1;
         } else {
            zc = 1;
            if (op->cen[2] < (cor[2][0] + cor[2][1])/2.0) {
               zv = 0;
            } else {
               zv = 1;
            }
         }
         if (xc) {
            if (yc) {
               if (zc) {   /* xc, yc, zc */
                  if ((((cor[0][1-xv] - op->cen[0])/op->size[0])*
                       ((cor[0][1-xv] - op->cen[0])/op->size[0]) +
                       ((cor[1][1-yv] - op->cen[1])/op->size[1])*
                       ((cor[1][1-yv] - op->cen[1])/op->size[1]) +
                       ((cor[2][1-zv] - op->cen[2])/op->size[2])*
                       ((cor[2][1-zv] - op->cen[2])/op->size[2]))
                       > 1.0) {
                     doesIntersect = 1;
                  }
               } else {   /* xc, yc, !zc */
                  if ((fabs(cor[2][zv] - op->cen[2]) < op->size[2]) &&
                      ((((cor[0][1-xv] - op->cen[0])/op->size[0])*
                        ((cor[0][1-xv] - op->cen[0])/op->size[0]) +
                        ((cor[1][1-yv] - op->cen[1])/op->size[1])*
                        ((cor[1][1-yv] - op->cen[1])/op->size[1]) +
                        ((cor[2][1-zv] - op->cen[2])/op->size[2])*
                        ((cor[2][1-zv] - op->cen[2])/op->size[2])) > 1.0)) {
                     doesIntersect = 1;
                  }
               }
            } else {
               if (zc) {   /* xc, !yc, zc */
                  if ((fabs(cor[1][yv] - op->cen[1]) < op->size[1]) &&
                      ((((cor[0][1-xv] - op->cen[0])/op->size[0])*
                        ((cor[0][1-xv] - op->cen[0])/op->size[0]) +
                        ((cor[1][1-yv] - op->cen[1])/op->size[1])*
                        ((cor[1][1-yv] - op->cen[1])/op->size[1]) +
                        ((cor[2][1-zv] - op->cen[2])/op->size[2])*
                        ((cor[2][1-zv] - op->cen[2])/op->size[2])) > 1.0)) {
                     doesIntersect = 1;
                  }
               } else {   /* xc, !yc, !zc */
                  if (((((cor[1][yv] - op->cen[1])/op->size[1])*
                        ((cor[1][yv] - op->cen[1])/op->size[1]) +
                        ((cor[2][zv] - op->cen[2])/op->size[2])*
                        ((cor[2][zv] - op->cen[2])/op->size[2])) < 1.0) &&
                      ((((cor[0][1-xv] - op->cen[0])/op->size[0])*
                        ((cor[0][1-xv] - op->cen[0])/op->size[0]) +
                        ((cor[1][1-yv] - op->cen[1])/op->size[1])*
                        ((cor[1][1-yv] - op->cen[1])/op->size[1]) +
                        ((cor[2][1-zv] - op->cen[2])/op->size[2])*
                        ((cor[2][1-zv] - op->cen[2])/op->size[2])) > 1.0)) {
                     doesIntersect = 1;
                  }
               }
            }
         } else {
            if (yc) {
               if (zc) {   /* !xc, yc, zc */
                  if ((fabs(cor[0][xv] - op->cen[0]) < op->size[0]) &&
                      ((((cor[0][1-xv] - op->cen[0])/op->size[0])*
                        ((cor[0][1-xv] - op->cen[0])/op->size[0]) +
                        ((cor[1][1-yv] - op->cen[1])/op->size[1])*
                        ((cor[1][1-yv] - op->cen[1])/op->size[1]) +
                        ((cor[2][1-zv] - op->cen[2])/op->size[2])*
                        ((cor[2][1-zv] - op->cen[2])/op->size[2])) > 1.0)) {
                     doesIntersect = 1;
                  }
               } else {   /* !xc, yc, !zc */
                  if (((((cor[0][xv] - op->cen[0])/op->size[0])*
                        ((cor[0][xv] - op->cen[0])/op->size[0]) +
                        ((cor[2][zv] - op->cen[2])/op->size[2])*
                        ((cor[2][zv] - op->cen[2])/op->size[2])) < 1.0) &&
                      ((((cor[0][1-xv] - op->cen[0])/op->size[0])*
                        ((cor[0][1-xv] - op->cen[0])/op->size[0]) +
                        ((cor[1][1-yv] - op->cen[1])/op->size[1])*
                        ((cor[1][1-yv] - op->cen[1])/op->size[1]) +
                        ((cor[2][1-zv] - op->cen[2])/op->size[2])*
                        ((cor[2][1-zv] - op->cen[2])/op->size[2])) > 1.0)) {
                     doesIntersect = 1;
                  }
               }
            } else {
               if (zc) {   /* !xc, !yc, zc */
                  if (((((cor[0][xv] - op->cen[0])/op->size[0])*
                        ((cor[0][xv] - op->cen[0])/op->size[0]) +
                        ((cor[1][yv] - op->cen[1])/op->size[1])*
                        ((cor[1][yv] - op->cen[1])/op->size[1])) < 1.0) &&
                      ((((cor[0][1-xv] - op->cen[0])/op->size[0])*
                        ((cor[0][1-xv] - op->cen[0])/op->size[0]) +
                        ((cor[1][1-yv] - op->cen[1])/op->size[1])*
                        ((cor[1][1-yv] - op->cen[1])/op->size[1]) +
                        ((cor[2][1-zv] - op->cen[2])/op->size[2])*
                        ((cor[2][1-zv] - op->cen[2])/op->size[2])) > 1.0)) {
                     doesIntersect = 1;
                  }
               } else {   /* !xc, !yc, !zc */
                  if (((((cor[0][xv] - op->cen[0])/op->size[0])*
                        ((cor[0][xv] - op->cen[0])/op->size[0]) +
                        ((cor[1][yv] - op->cen[1])/op->size[1])*
                        ((cor[1][yv] - op->cen[1])/op->size[1]) +
                        ((cor[2][zv] - op->cen[2])/op->size[2])*
                        ((cor[2][zv] - op->cen[2])/op->size[2])) < 1.0) &&
                      ((((cor[0][1-xv] - op->cen[0])/op->size[0])*
                        ((cor[0][1-xv] - op->cen[0])/op->size[0]) +
                        ((cor[1][1-yv] - op->cen[1])/op->size[1])*
                        ((cor[1][1-yv] - op->cen[1])/op->size[1]) +
                        ((cor[2][1-zv] - op->cen[2])/op->size[2])*
                        ((cor[2][1-zv] - op->cen[2])/op->size[2])) > 1.0)) {
                     doesIntersect = 1;
                  }
               }
            }
         }
         if (op->type != 2 && doesIntersect) {
            /* exclude halfplane of spheroid to make hemispheroid */
            if (op->type == 4 && cor[0][1] < op->cen[0]) {
               doesIntersect = tmp;
            } else if (op->type == 6 && cor[0][0] > op->cen[0]) {
               doesIntersect = tmp;
            } else if (op->type == 8 && cor[1][1] < op->cen[1]) {
               doesIntersect = tmp;
            } else if (op->type == 10 && cor[1][0] > op->cen[1]) {
               doesIntersect = tmp;
            } else if (op->type == 12 && cor[2][1] < op->cen[2]) {
               doesIntersect = tmp;
            } else if (op->type == 14 && cor[2][0] > op->cen[2]) {
               doesIntersect = tmp;
            }
         }
      } else if (op->type >= 3 && op->type <= 15 && op->type%2) {
         /* solid spheroid or hemispheroid */
         /* determine if center is in block - if so then refine,
          * if not determine nearest vertix and see if that is in
          * - if so refine
          * 1 = (x/a)^2 + (y/b)^2 + (z/c)^2 is boundary */
         tmp = doesIntersect;
         xc = yc = zc = 0;
         if (op->cen[0] < cor[0][0]) {
            xv = 0;
         } else if (op->cen[0] > cor[0][1]) {
            xv = 1;
         } else {
            xc = 1;
         }
         if (op->cen[1] < cor[1][0]) {
            yv = 0;
         } else if (op->cen[1] > cor[1][1]) {
            yv = 1;
         } else {
            yc = 1;
         } if (op->cen[2] < cor[2][0]) {
            zv = 0;
         } else if (op->cen[2] > cor[2][1]) {
            zv = 1;
         } else {
            zc = 1;
         }
         if (xc) {
            if (yc) {
               if (zc) {  /* xc, yc, zc */
                  doesIntersect = 1;
               } else {   /* xc, yc, !zc */
                  if (fabs(cor[2][zv] - op->cen[2]) < op->size[2]) {
                     doesIntersect = 1;
                  }
               }
            } else {
               if (zc) {   /* xc, !yc, zc */
                  if (fabs(cor[1][yv] - op->cen[1]) < op->size[1]) {
                     doesIntersect = 1;
                  }
               } else {   /* xc, !yc, !zc */
                  if ((((cor[1][yv] - op->cen[1])/op->size[1])*
                       ((cor[1][yv] - op->cen[1])/op->size[1]) +
                       ((cor[2][zv] - op->cen[2])/op->size[2])*
                       ((cor[2][zv] - op->cen[2])/op->size[2])) < 1.0) {
                     doesIntersect = 1;
                  }
               }
            }
         } else {
            if (yc) {
               if (zc) {   /* !xc, yc, zc */
                  if (fabs(cor[0][xv] - op->cen[0]) < op->size[0]) {
                     doesIntersect = 1;
                  }
               } else {   /* !xc, yc, !zc */
                  if ((((cor[0][xv] - op->cen[0])/op->size[0])*
                       ((cor[0][xv] - op->cen[0])/op->size[0]) +
                       ((cor[2][zv] - op->cen[2])/op->size[2])*
                       ((cor[2][zv] - op->cen[2])/op->size[2])) < 1.0) {
                     doesIntersect = 1;
                  }
               }
            } else {
               if (zc) {   /* !xc, !yc, zc */
                  if ((((cor[0][xv] - op->cen[0])/op->size[0])*
                       ((cor[0][xv] - op->cen[0])/op->size[0]) +
                       ((cor[1][yv] - op->cen[1])/op->size[1])*
                       ((cor[1][yv] - op->cen[1])/op->size[1])) < 1.0) {
                     doesIntersect = 1;
                  }
               } else {   /* !xc, !yc, !zc */
                  if ((((cor[0][xv] - op->cen[0])/op->size[0])*
                       ((cor[0][xv] - op->cen[0])/op->size[0]) +
                       ((cor[1][yv] - op->cen[1])/op->size[1])*
                       ((cor[1][yv] - op->cen[1])/op->size[1]) +
                       ((cor[2][zv] - op->cen[2])/op->size[2])*
                       ((cor[2][zv] - op->cen[2])/op->size[2])) < 1.0) {
                     doesIntersect = 1;
                  }
               }
            }
         }
         if (op->type != 3 && doesIntersect) {
            /* exclude halfplane of spheroid to make hemispheroid */
            if (op->type == 5 && cor[0][1] < op->cen[0]) {
               doesIntersect = tmp;
            } else if (op->type == 7 && cor[0][0] > op->cen[0]) {
               doesIntersect = tmp;
            } else if (op->type == 9 && cor[1][1] < op->cen[1]) {
               doesIntersect = tmp;
            } else if (op->type == 11 && cor[1][0] > op->cen[1]) {
               doesIntersect = tmp;
            } else if (op->type == 13 && cor[2][1] < op->cen[2]) {
               doesIntersect = tmp;
            } else if (op->type == 15 && cor[2][0] > op->cen[2]) {
               doesIntersect = tmp;
            }
         }
      } else if (op->type == 20 || op->type == 22 || op->type == 24) {
         /* boundary of cylinder, ca is axis of cylinder */
         if (op->type == 20) {
            ca = 0;
            c1 = 1;
            c2 = 2;
         } else if (op->type == 22) {
            ca = 1;
            c1 = 2;
            c2 = 0;
         } else {
            ca = 2;
            c1 = 0;
            c2 = 1;
         }
         if (cor[ca][1] > (op->cen[ca] - op->size[ca]) &&
             cor[ca][0] < (op->cen[ca] + op->size[ca])) {
            /* some part of block between planes that define ends */
            /* use y and z for directions perpendicular to axis */
            yc = zc = 0;
            if (op->cen[c1] < cor[c1][0]) {
               yv = 0;
            } else if (op->cen[c1] > cor[c1][1]) {
               yv = 1;
            } else {
               yc = 1;
               if (op->cen[c1] < (cor[c1][0] + cor[c1][1])/2.0) {
                  yv = 0;
               } else {
                  yv = 1;
               }
            }
            if (op->cen[c2] < cor[c2][0]) {
               zv = 0;
            } else if (op->cen[c2] > cor[c2][1]) {
               zv = 1;
            } else {
               zc = 1;
               if (op->cen[c2] < (cor[c2][0] + cor[c2][1])/2.0) {
                  zv = 0;
               } else {
                  zv = 1;
               }
            }
            if ((cor[0][0] < (op->cen[0] - op->size[0]) &&
                 cor[0][1] < (op->cen[0] + op->size[0])) ||
                (cor[0][0] > (op->cen[0] - op->size[0]) &&
                 cor[0][1] > (op->cen[0] + op->size[0]))) {
               /* block overlaps cylinder ends in aixs dir */
               if (yc) {
                  if (zc) {
                     doesIntersect = 1;
                  } else {
                     if (fabs(cor[c2][zv] - op->cen[c2]) < op->size[c2]) {
                        doesIntersect = 1;
                     }
                  }
               } else {
                  if (zc) {
                     if (fabs(cor[c1][yv] - op->cen[c1]) < op->size[c1])
                        doesIntersect = 1;
                  } else {
                     if ((((cor[c1][yv] - op->cen[c1])/op->size[c1])*
                          ((cor[c1][yv] - op->cen[c1])/op->size[c1]) +
                          ((cor[c2][zv] - op->cen[c2])/op->size[c2])*
                          ((cor[c2][zv] - op->cen[c2])/op->size[c2]))
                           < 1.0) {
                        doesIntersect = 1;
                     }
                  }
               }
            } else {
               /* block in cylinder or cylinder in block in aixs dir */
               /* in c1 c2 plane need block point in and out of circle */
               if (yc) {
                  if (zc) {
                    if ((((cor[c1][1-yv] - op->cen[c1])/op->size[c1])*
                         ((cor[c1][1-yv] - op->cen[c1])/op->size[c1]) +
                         ((cor[c2][1-zv] - op->cen[c2])/op->size[c2])*
                         ((cor[c2][1-zv] - op->cen[c2])/op->size[c2]))
                         > 1.0) {
                        doesIntersect = 1;
                     }
                  } else {
                     if ((fabs(cor[c2][zv]-op->cen[c2]) < op->size[c2])&&
                         ((((cor[c1][1-yv] - op->cen[c1])/op->size[c1])*
                           ((cor[c1][1-yv] - op->cen[c1])/op->size[c1]) +
                           ((cor[c2][1-zv] - op->cen[c2])/op->size[c2])*
                           ((cor[c2][1-zv] - op->cen[c2])/op->size[c2]))
                           > 1.0)) {
                        doesIntersect = 1;
                     }
                  }
               } else {
                  if (zc) {
                     if ((fabs(cor[c1][yv]-op->cen[c1]) < op->size[c1])&&
                         ((((cor[c1][1-yv] - op->cen[c1])/op->size[c1])*
                           ((cor[c1][1-yv] - op->cen[c1])/op->size[c1]) +
                           ((cor[c2][1-zv] - op->cen[c2])/op->size[c2])*
                           ((cor[c2][1-zv] - op->cen[c2])/op->size[c2]))
                           > 1.0)) {
                        doesIntersect = 1;
                     }
                  } else {
                     if (((((cor[c1][yv] - op->cen[c1])/op->size[c1])*
                           ((cor[c1][yv] - op->cen[c1])/op->size[c1]) +
                           ((cor[c2][zv] - op->cen[c2])/op->size[c2])*
                           ((cor[c2][zv] - op->cen[c2])/op->size[c2]))
                           < 1.0) &&
                         ((((cor[c1][1-yv] - op->cen[c1])/op->size[c1])*
                           ((cor[c1][1-yv] - op->cen[c1])/op->size[c1]) +
                           ((cor[c2][1-zv] - op->cen[c2])/op->size[c2])*
                           ((cor[c2][1-zv] - op->cen[c2])/op->size[c2]))
                           > 1.0)) {
                        doesIntersect = 1;
                     }
                  }
               }
            }
         }
      } else if (op->type == 21 || op->type == 23 || op->type == 25) {
         /* volume of cylinder, ca is axis of cylinder */
         if (op->type == 21) {
            ca = 0;
            c1 = 1;
            c2 = 2;
         } else if (op->type == 23) {
            ca = 1;
            c1 = 2;
            c2 = 0;
         } else {
            ca = 2;
            c1 = 0;
            c2 = 1;
         }
         if (cor[ca][1] > (op->cen[ca] - op->size[ca]) &&
             cor[ca][0] < (op->cen[ca] + op->size[ca])) {
            /* some part of block between planes that define ends */
            /* use y and z for directions perpendicular to axis */
            yc = zc = 0;
            if (op->cen[c1] < cor[c1][0]) {
               yv = 0;
            } else if (op->cen[c1] > cor[c1][1]) {
               yv = 1;
            } else {
               yc = 1;
            }
            if (op->cen[c2] < cor[c2][0]) {
               zv = 0;
            } else if (op->cen[c2] > cor[c2][1]) {
               zv = 1;
            } else {
               zc = 1;
            }
            if (yc) {
               if (zc) {
                  doesIntersect = 1;
               } else {
                  if (fabs(cor[c2][zv] - op->cen[c2]) < op->size[c2])
                     doesIntersect = 1;
               }
            } else {
               if (zc) {
                  if (fabs(cor[c1][yv] - op->cen[c1]) < op->size[c1]) {
                     doesIntersect = 1;
                  }
               } else {
                  if ((((cor[c1][yv] - op->cen[c1])/op->size[c1])*
                       ((cor[c1][yv] - op->cen[c1])/op->size[c1]) +
                       ((cor[c2][zv] - op->cen[c2])/op->size[c2])*
                       ((cor[c2][zv] - op->cen[c2])/op->size[c2])) < 1.0) {
                     doesIntersect = 1;
                  }
               }
            }
         }
      } else {
         ocrPrintf("undefined object %d\n", op->type);
      }
   }
   return(doesIntersect);}

/* A block's own demand on the mesh, before any neighbour has a say.  It reads
 * the block's level and centre and the objects, and nothing else — which is
 * why the whole topology of a refinement round is computable without the
 * field values and therefore without a distributed protocol. */
decision check_objects(const control* c, const object* obj, u32 level, const u32 pos[3])
{
  decision d = UNREFINE_BLK;
  s32 cen[3], sz;
  double cor[3][2];

  blk_centre(c, level, pos, cen);
  sz = blk_half(c, level);
  for(u32 i = 0; i < 3; ++i) {
    cor[i][0] = ((double)(cen[i] - sz))/((double)c->mesh_size[i]);
    cor[i][1] = ((double)(cen[i] + sz))/((double)c->mesh_size[i]);
  }
  if(c->refine_ghost) {
    double n[3] = { (double)c->nx, (double)c->ny, (double)c->nz };
    for(u32 i = 0; i < 3; ++i) {
      double e = 2.0*(((double)sz)/n[i])/((double)c->mesh_size[i]);
      cor[i][0] -= e;
      cor[i][1] += e;
    }
  }
  if(check_block(c, obj, cor)) {
    d = REFINE_BLK;
  } else if(c->refine_ghost && level != 0) {
    /* A block that would coarsen while its parent would refine must stay
     * where it is, or the pair would oscillate every round. */
    double n[3] = { (double)c->nx, (double)c->ny, (double)c->nz };
    sz = c->p2[c->num_refine - (s32)level + 1];
    for(u32 i = 0; i < 3; ++i) {
      double e = (((double)sz)/n[i])/((double)c->mesh_size[i]);
      cor[i][0] -= e;
      cor[i][1] += e;
    }
    if(check_block(c, obj, cor)) d = LEAVE_BLK_AT_LVL;
  }

  if((s32)level == c->num_refine && d == REFINE_BLK) d = LEAVE_BLK_AT_LVL;
  if(level == 0 && d == UNREFINE_BLK) d = LEAVE_BLK_AT_LVL;
  return d;
}

/* ---- the working mesh ------------------------------------------------ */

/* One block while the round is being computed.  `src` is the block of the
 * previous epoch this one draws its payload from — itself where it survives,
 * its ancestor where it was just split off, the first of its eight where an
 * octet was joined. */
typedef struct
{
  u32 pos[3];
  u32 key;
  u32 owner;
  u8  level;
  s8  disp;
  u8  kind;
  u8  levels;                  //!< levels of split between `src` and this block
  s32 src[8];
} wblk;

typedef struct
{
  control c;
  wblk* b;
  u32 n;
  u32 cap;
  u32 nint;
  u8  changed;
  s32 unref_level;
  u32 nold;
} wmesh;

static void wpush(wmesh* w, const wblk* x)
{
  if(w->n == w->cap) {
    w->cap = w->cap ? w->cap*2 : 1024;
    w->b = (wblk*)realloc(w->b, (size_t)w->cap*sizeof(wblk));
  }
  w->b[w->n++] = *x;
}

static int wcmp(const void* a, const void* b)
{
  const wblk* x = (const wblk*)a;
  const wblk* y = (const wblk*)b;
  if(x->owner != y->owner) return x->owner < y->owner ? -1 : 1;
  if(x->key != y->key) return x->key < y->key ? -1 : 1;
  return 0;
}

/* The directory is grouped by rank and ordered by key inside a rank, so a
 * spawn tree cuts it into whole-rank ranges and a lookup is one binary search
 * in the range of the owner the position names. */
static s32 wfind(const wmesh* w, u32 level, const s32 p[3])
{
  const control* c = &w->c;
  for(u32 d = 0; d < 3; ++d)
    if(p[d] < 0 || (u32)p[d] >= level_dim(c, d, level)) return -1;
  u32 pos[3] = { (u32)p[0], (u32)p[1], (u32)p[2] };
  u32 owner = blk_owner(c, level, pos);
  u32 key = blk_key(c, level, pos);
  u32 lo = 0, hi = w->n;
  while(lo < hi) {
    u32 m = lo + (hi-lo)/2;
    if(w->b[m].owner < owner || (w->b[m].owner == owner && w->b[m].key < key)) lo = m+1;
    else hi = m;
  }
  if(lo < w->n && w->b[lo].owner == owner && w->b[lo].key == key) return (s32)lo;
  return -1;
}

static void wsort(wmesh* w)
{
  qsort(w->b, w->n, sizeof(wblk), wcmp);
}

/* The position of neighbour `f` at the same level; the caller decides what to
 * do when no block lives there. */
static void face_pos(u32 f, const u32 p[3], s32 q[3])
{
  q[0] = (s32)p[0]; q[1] = (s32)p[1]; q[2] = (s32)p[2];
  q[f >> 1] += (f & 1) ? 1 : -1;
}

/* The finer block that writes quarter `slot` of face `f`.  Both endpoints
 * derive the quarter from the same two position bits, so the sender's slot
 * order and the receiver's choice of quarter agree without either being told. */
static void quarter_pos(u32 f, u32 slot, const u32 p[3], s32 q[3])
{
  u32 axis = f >> 1, pole = f & 1;
  u32 lr = slot >> 1, ud = slot & 1;
  q[0] = (s32)(p[0] << 1); q[1] = (s32)(p[1] << 1); q[2] = (s32)(p[2] << 1);
  q[axis] += pole ? 2 : -1;
  if(axis == 0)      { q[1] += (s32)lr; q[2] += (s32)ud; }
  else if(axis == 1) { q[0] += (s32)lr; q[2] += (s32)ud; }
  else               { q[0] += (s32)lr; q[1] += (s32)ud; }
}

/// Which quarter of a coarser neighbour's face this block reads.
u32 face_quarter(u32 f, const u32 p[3])
{
  u32 axis = f >> 1;
  if(axis == 0) return ((p[1] & 1) << 1) | (p[2] & 1);
  if(axis == 1) return ((p[0] & 1) << 1) | (p[2] & 1);
  return ((p[0] & 1) << 1) | (p[1] & 1);
}

/* The refinement level on the far side of each face.  A mesh where two blocks
 * sharing a face differ by more than one level has no defined halo, so the
 * three cases below are exhaustive by construction and a fourth is a defect
 * in the round that produced the mesh. */
static u8 face_level(const wmesh* w, u32 i, u32 f)
{
  const wblk* b = &w->b[i];
  s32 q[3];
  face_pos(f, b->pos, q);
  for(u32 d = 0; d < 3; ++d)
    if(q[d] < 0 || (u32)q[d] >= level_dim(&w->c, d, b->level)) return NBR_NONE;
  if(wfind(w, b->level, q) >= 0) return b->level;
  if(b->level > 0) {
    s32 cq[3] = { q[0] >> 1, q[1] >> 1, q[2] >> 1 };
    if(wfind(w, (u32)b->level - 1, cq) >= 0) return (u8)(b->level - 1);
  }
  return (u8)(b->level + 1);
}

/* ---- the refinement round -------------------------------------------- */

/* Recompute every face's neighbour level from the block set itself.  The
 * published protocol carries the same information incrementally, in the
 * quarter-mask nibble a forking block hands its children; deriving it from
 * the set that resulted is the same answer and needs no bookkeeping. */
static void wneighbours(const wmesh* w, u8* nbr)
{
  for(u32 i = 0; i < w->n; ++i)
    for(u32 f = 0; f < 6; ++f)
      nbr[i*6+f] = face_level(w, i, f);
}

/* One iteration of the published disposition protocol.  A block hears from
 * its finer neighbours before it publishes, so the demand for a finer mesh
 * travels from the finest level outward within the iteration — which is the
 * order the message rendezvous forces, and it is acyclic because a block only
 * ever reads strictly finer neighbours.  The same-level veto is applied after
 * every block has published, so a block that it changes was still seen as
 * asking to coarsen by everyone this iteration. */
static void refine_iteration(wmesh* w, const u8* nbr)
{
  u32 n = w->n;
  u8 maxlvl = 0;
  for(u32 i = 0; i < n; ++i) if(w->b[i].level > maxlvl) maxlvl = w->b[i].level;

  for(s32 L = (s32)maxlvl; L >= 0; --L) {
    for(u32 i = 0; i < n; ++i) {
      wblk* b = &w->b[i];
      if(b->level != (u8)L) continue;
      for(u32 f = 0; f < 6; ++f) {
        if(nbr[i*6+f] != b->level + 1) continue;
        for(u32 s = 0; s < 4; ++s) {
          s32 q[3];
          quarter_pos(f, s, b->pos, q);
          s32 j = wfind(w, (u32)b->level + 1, q);
          if(j < 0) continue;
          if(w->b[j].disp == REFINE_BLK) b->disp = REFINE_BLK;
          else if(w->b[j].disp == LEAVE_BLK_AT_LVL && b->disp != REFINE_BLK)
            b->disp = LEAVE_BLK_AT_LVL;
        }
      }
    }
  }

  /* A block cannot coarsen away from a neighbour that is about to double its
   * resolution: the pair would then differ by two levels. */
  for(u32 i = 0; i < n; ++i) {
    wblk* b = &w->b[i];
    if(b->disp != UNREFINE_BLK) continue;
    for(u32 f = 0; f < 6; ++f) {
      if(nbr[i*6+f] != b->level) continue;
      s32 q[3];
      face_pos(f, b->pos, q);
      s32 j = wfind(w, b->level, q);
      if(j >= 0 && w->b[j].disp == REFINE_BLK) { b->disp = LEAVE_BLK_AT_LVL; break; }
    }
  }
}

/* Replace every block marked to refine by its eight children.  A child keeps
 * its parent's disposition, so a block the objects put a boundary through
 * keeps splitting until it reaches the deepest level the run allows: the
 * geometry is tested once per round and the depth it buys is then bounded by
 * the round's iteration count, not by a second test. */
static u8 refine_split(wmesh* w)
{
  u32 n = w->n, m = 0;
  u8 any = 0;
  for(u32 i = 0; i < n; ++i) if(w->b[i].disp == REFINE_BLK) any = 1;
  if(!any) return 0;

  wblk* out = (wblk*)malloc((size_t)n*sizeof(wblk));
  memcpy(out, w->b, (size_t)n*sizeof(wblk));
  m = n;
  w->n = 0;
  for(u32 i = 0; i < m; ++i) {
    wblk* b = &out[i];
    if(b->disp != REFINE_BLK) { wpush(w, b); continue; }
    for(u32 p = 0; p < 8; ++p) {
      wblk k = *b;
      k.level = (u8)(b->level + 1);
      k.pos[0] = (b->pos[0] << 1) + (p & 1);
      k.pos[1] = (b->pos[1] << 1) + ((p >> 1) & 1);
      k.pos[2] = (b->pos[2] << 1) + ((p >> 2) & 1);
      k.key = blk_key(&w->c, k.level, k.pos);
      k.owner = blk_owner(&w->c, k.level, k.pos);
      k.kind = BK_CHILD;
      k.levels = (u8)(b->levels + 1);
      if((s32)k.level == w->c.num_refine) k.disp = LEAVE_BLK_AT_LVL;
      wpush(w, &k);
    }
  }
  free(out);
  wsort(w);
  return 1;
}

/* One level of coarsening.  An octet joins only when all eight of its members
 * are live at this level and all eight still ask to coarsen, and only when no
 * face of any of them still has a finer block behind it: the sibling
 * unanimity and the finer-neighbour query of the published protocol.  Levels
 * are collapsed finest first, so a block that has just joined is already at
 * this level when the level is processed and can join again. */
static u8 unrefine_level(wmesh* w, u32 level, const u8* nbr)
{
  u32 n = w->n;
  u8 any = 0;

  for(u32 i = 0; i < n; ++i) {
    wblk* b = &w->b[i];
    if(b->level != level || b->disp != UNREFINE_BLK) continue;
    for(u32 f = 0; f < 6; ++f)
      if(nbr[i*6+f] == b->level + 1) { b->disp = LEAVE_BLK_AT_LVL; break; }
  }

  for(u32 i = 0; i < n; ++i) {
    wblk* b = &w->b[i];
    if(b->level != level || b->disp != UNREFINE_BLK) continue;
    u32 base[3] = { b->pos[0] & ~1u, b->pos[1] & ~1u, b->pos[2] & ~1u };
    s32 sib[8];
    u8 all = 1;
    for(u32 p = 0; p < 8; ++p) {
      s32 q[3] = { (s32)(base[0] + (p & 1)), (s32)(base[1] + ((p >> 1) & 1)),
                   (s32)(base[2] + ((p >> 2) & 1)) };
      sib[p] = wfind(w, level, q);
      if(sib[p] < 0 || w->b[sib[p]].level != level
         || w->b[sib[p]].disp != UNREFINE_BLK) all = 0;
    }
    if(all) continue;
    for(u32 p = 0; p < 8; ++p)
      if(sib[p] >= 0 && w->b[sib[p]].level == level
         && w->b[sib[p]].disp == UNREFINE_BLK)
        w->b[sib[p]].disp = LEAVE_BLK_AT_LVL;
  }

  /* The sources are collected while the directory is still searchable: the
   * join reads all eight of the octet's payloads and writes the first, which
   * is also the block the merged one inherits its datablocks from. */
  for(u32 i = 0; i < n; ++i) {
    wblk* b = &w->b[i];
    if(b->level != level || b->disp != UNREFINE_BLK) continue;
    if((b->pos[0] & 1) || (b->pos[1] & 1) || (b->pos[2] & 1)) continue;
    for(u32 p = 0; p < 8; ++p) {
      s32 q[3] = { (s32)(b->pos[0] + (p & 1)), (s32)(b->pos[1] + ((p >> 1) & 1)),
                   (s32)(b->pos[2] + ((p >> 2) & 1)) };
      s32 j = wfind(w, level, q);
      b->src[p] = j >= 0 ? w->b[j].src[0] : -1;
    }
  }

  wblk* out = (wblk*)malloc((size_t)n*sizeof(wblk));
  memcpy(out, w->b, (size_t)n*sizeof(wblk));
  w->n = 0;
  for(u32 i = 0; i < n; ++i) {
    wblk* b = &out[i];
    if(b->level != level || b->disp != UNREFINE_BLK) { wpush(w, b); continue; }
    if((b->pos[0] & 1) || (b->pos[1] & 1) || (b->pos[2] & 1)) continue;
    wblk k = *b;
    k.level = (u8)(level - 1);
    k.pos[0] = b->pos[0] >> 1; k.pos[1] = b->pos[1] >> 1; k.pos[2] = b->pos[2] >> 1;
    k.key = blk_key(&w->c, k.level, k.pos);
    k.owner = blk_owner(&w->c, k.level, k.pos);
    k.kind = BK_MERGE;
    k.levels = 1;
    any = 1;
    wpush(w, &k);
  }
  free(out);
  wsort(w);
  return any;
}

/* ---- emitting a directory -------------------------------------------- */

/* The forest the checksum reduces over: a base block's own subtree, laid down
 * so that an interior node's eight children are in prong order and the base
 * blocks are in the order the mesh was created in.  That order, and not the
 * node count, is what makes the reduction bitwise reproducible. */
static s32 emit_node(const wmesh* w, u32 level, const u32 p[3], fnode* nodes, u32* nint,
                     u8* bad)
{
  s32 q[3] = { (s32)p[0], (s32)p[1], (s32)p[2] };
  s32 i = wfind(w, level, q);
  if(i >= 0) return i;
  if((s32)level >= w->c.num_refine) {
    /* Every position of the domain is covered by exactly one live block, so
     * the walk always reaches one before the deepest level the run allows;
     * arriving here means the round produced a mesh with a hole in it, and the
     * directory must not be published — a forest node standing in for a block
     * that is not there would be read as block zero. */
    if(!*bad)
      ocrPrintf("Internal error: no block covers (%u,%u,%u) at level %u\n",
                p[0], p[1], p[2], level);
    *bad = 1;
    return 0;
  }
  u32 me = (*nint)++;
  fnode local;
  for(u32 c = 0; c < 8; ++c) {
    u32 cp[3] = { (p[0] << 1) + (c & 1), (p[1] << 1) + ((c >> 1) & 1),
                  (p[2] << 1) + ((c >> 2) & 1) };
    local.child[c] = emit_node(w, level + 1, cp, nodes, nint, bad);
  }
  if(nodes) nodes[me] = local;
  return -(s32)me - 1;
}

static u32 count_nodes(const wmesh* w, u8* bad)
{
  u32 nint = 0;
  const control* c = &w->c;
  for(s32 x = 0; x < c->npx; ++x)
    for(s32 y = 0; y < c->npy; ++y)
      for(s32 z = 0; z < c->npz; ++z) {
        u32 p[3] = { (u32)x, (u32)y, (u32)z };
        emit_node(w, 0, p, NULL, &nint, bad);
      }
  return nint;
}

u64 mesh_size_bytes(const void* handle)
{
  const wmesh* w = (const wmesh*)handle;
  return mesh_bytes(w->n, (u32)(w->c.npx*w->c.npy*w->c.npz), w->nint, w->nold,
                    w->c.nranks);
}

u8 mesh_changed(const void* handle)
{
  return ((const wmesh*)handle)->changed;
}

void mesh_free(void* handle)
{
  wmesh* w = (wmesh*)handle;
  free(w->b);
  free(w);
}

/* Lay the working mesh down as the directory every task reads: the block
 * records in rank-major, key-ordered position, the reduction forest, where
 * each rank's blocks begin, and how each block draws its payload from the
 * epoch it replaces. */
u8 mesh_emit(void* handle, mesh* m)
{
  wmesh* w = (wmesh*)handle;
  u8 bad = 0;
  const control* c = &w->c;
  u32 nbase = (u32)(c->npx*c->npy*c->npz);

  m->c = *c;
  m->nblk = w->n;
  m->nbase = nbase;
  m->nint = w->nint;
  m->nold = w->nold;
  m->epoch = 0;
  m->gen = 0;
  m->unref_level = w->unref_level;
  m->changed = w->changed;

  blk* br = mesh_brec(m);
  u8* nbr = (u8*)malloc((size_t)w->n*6);
  wneighbours(w, nbr);
  for(u32 i = 0; i < w->n; ++i) {
    const wblk* b = &w->b[i];
    br[i].key = b->key;
    br[i].pos[0] = b->pos[0]; br[i].pos[1] = b->pos[1]; br[i].pos[2] = b->pos[2];
    br[i].owner = b->owner;
    br[i].level = b->level;
    br[i].disp = b->disp;
    for(u32 f = 0; f < 6; ++f) {
      br[i].nbr[f] = nbr[i*6+f];
      for(u32 s = 0; s < 4; ++s) br[i].in[f][s] = -1;
      if(br[i].nbr[f] == NBR_NONE) continue;
      if(br[i].nbr[f] == b->level) {
        s32 q[3]; face_pos(f, b->pos, q);
        br[i].in[f][0] = wfind(w, b->level, q);
      } else if(br[i].nbr[f] < b->level) {
        s32 q[3]; face_pos(f, b->pos, q);
        s32 cq[3] = { q[0] >> 1, q[1] >> 1, q[2] >> 1 };
        br[i].in[f][0] = wfind(w, (u32)b->level - 1, cq);
      } else {
        for(u32 s = 0; s < 4; ++s) {
          s32 q[3]; quarter_pos(f, s, b->pos, q);
          br[i].in[f][s] = wfind(w, (u32)b->level + 1, q);
        }
      }
      /* Two blocks that share a face may differ by at most one level, or the
       * halo between them has no definition; a slot with no writer means the
       * round that produced this mesh broke that invariant, and the directory
       * must not be published — a stage task would resolve the strip through a
       * negative index. */
      u32 slots = br[i].nbr[f] == b->level + 1 ? 4 : 1;
      for(u32 s = 0; s < slots; ++s)
        if(br[i].in[f][s] < 0) {
          if(!bad)
            ocrPrintf("Internal error: face %u of the block at (%u,%u,%u) level %u "
                      "has no writer for strip %u\n", f, b->pos[0], b->pos[1],
                      b->pos[2], (u32)b->level, s);
          bad = 1;
        }
    }
  }
  free(nbr);

  u32* rs = mesh_rstart(m);
  for(u32 r = 0; r <= c->nranks; ++r) rs[r] = 0;
  for(u32 i = 0; i < w->n; ++i) rs[w->b[i].owner + 1]++;
  for(u32 r = 0; r < c->nranks; ++r) rs[r+1] += rs[r];

  s32* root = mesh_root(m);
  fnode* nodes = mesh_fnode(m);
  u32 nint = 0, k = 0;
  for(s32 x = 0; x < c->npx; ++x)
    for(s32 y = 0; y < c->npy; ++y)
      for(s32 z = 0; z < c->npz; ++z) {
        u32 p[3] = { (u32)x, (u32)y, (u32)z };
        root[k++] = emit_node(w, 0, p, nodes, &nint, &bad);
      }

  s32* src = mesh_src(m);
  u8* kind = mesh_kind(m);
  for(u32 i = 0; i < w->n; ++i) {
    kind[i] = w->b[i].kind;
    for(u32 s = 0; s < 8; ++s) src[i*8+s] = w->b[i].src[s];
    if(kind[i] != BK_MERGE) src[i*8] = w->b[i].src[0];
  }
  /* `levels` rides in the unused slots of a split child's source list: a
   * child several levels below its source upsamples once per level. */
  for(u32 i = 0; i < w->n; ++i)
    if(kind[i] == BK_CHILD) src[i*8+1] = (s32)w->b[i].levels;

  /* A block of the previous epoch stays alive only where a block of this one
   * carries its datablocks forward; a split parent and every member of a
   * joined octet are given back once the pass that read them has finished. */
  u8* dead = mesh_odead(m);
  for(u32 i = 0; i < w->nold; ++i) dead[i] = 1;
  for(u32 i = 0; i < w->n; ++i)
    if(kind[i] == BK_KEEP && src[i*8] >= 0) dead[src[i*8]] = 0;

  return bad;
}

/* Compute the mesh one round produces.  `old` is the epoch being replaced,
 * NULL for the first; `mode` selects the round's phase. */
void* mesh_compute(const control* c, const mesh* old, const object* obj, s32 ts, u32 mode)
{
  wmesh* w = (wmesh*)calloc(1, sizeof(wmesh));
  w->c = *c;
  w->unref_level = -1;
  w->changed = 1;
  w->nold = old ? old->nblk : 0;

  if(!old) {
    for(s32 x = 0; x < c->npx; ++x)
      for(s32 y = 0; y < c->npy; ++y)
        for(s32 z = 0; z < c->npz; ++z) {
          wblk b;
          memset(&b, 0, sizeof(b));
          b.pos[0] = (u32)x; b.pos[1] = (u32)y; b.pos[2] = (u32)z;
          b.level = 0;
          b.key = blk_key(c, 0, b.pos);
          b.owner = blk_owner(c, 0, b.pos);
          b.kind = BK_KEEP;
          b.src[0] = -1;
          b.disp = LEAVE_BLK_AT_LVL;
          wpush(w, &b);
        }
    wsort(w);
    u8 bad = 0;
    w->nint = count_nodes(w, &bad);
    return w;
  }

  const blk* ob = mesh_brec(old);
  for(u32 i = 0; i < old->nblk; ++i) {
    wblk b;
    memset(&b, 0, sizeof(b));
    b.pos[0] = ob[i].pos[0]; b.pos[1] = ob[i].pos[1]; b.pos[2] = ob[i].pos[2];
    b.level = ob[i].level;
    b.key = ob[i].key;
    b.owner = ob[i].owner;
    b.kind = BK_KEEP;
    b.src[0] = (s32)i;
    b.disp = ob[i].disp;
    wpush(w, &b);
  }

  u8 moved = 0;
  if(mode == MP_REFINE) {
    for(u32 i = 0; i < w->n; ++i)
      w->b[i].disp = (s8)check_objects(c, obj, w->b[i].level, w->b[i].pos);
    s32 steps = (ts == 0) ? c->num_refine : c->block_change;
    u8* nbr = (u8*)malloc((size_t)w->cap*6 + 64);
    for(s32 it = 0; it < steps; ++it) {
      u8* n2 = (u8*)realloc(nbr, (size_t)w->n*6 + 8);
      nbr = n2;
      wneighbours(w, nbr);
      refine_iteration(w, nbr);
      if(refine_split(w)) moved = 1;
    }
    free(nbr);
    u8 maxlvl = 0;
    for(u32 i = 0; i < w->n; ++i) if(w->b[i].level > maxlvl) maxlvl = w->b[i].level;
    w->unref_level = (s32)maxlvl;
  } else {
    u32 level = (u32)old->unref_level;
    u8* nbr = (u8*)malloc((size_t)w->n*6 + 8);
    wneighbours(w, nbr);
    if(level >= 1) moved = unrefine_level(w, level, nbr);
    free(nbr);
    w->unref_level = (s32)level - 1;
  }

  w->changed = moved;
  u8 bad = 0;
  w->nint = count_nodes(w, &bad);
  return w;
}
