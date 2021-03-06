/*
   File name: fem_vmis.c
   Date:      2004/03/27 13:41
   Author:    Jiri Brozovsky

   Copyright (C) 2004 Jiri Brozovsky

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   in a file called COPYING along with this program; if not, write to
   the Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA
   02139, USA.

   FEM Solver - 3D von Mises plasticity condition
*/

#include "fem_pl3d.h"

/* from "fem_chen.c": */
extern double stress3D_I1(tVector *stress);
extern double stress3D_J2dev(tVector *stress);
extern int chen_Dep(tVector *deriv, double H, tMatrix *De, tMatrix *Dep);
extern int D_HookIso_planeRaw(double E, double nu, long Problem, tMatrix *D);
extern double stress2D_I1(tVector *stress);
extern double stress2D_J2dev(tVector *stress) ;

extern double fem_plast_H_linear(long ePos, 
                      double E0,
                      double E1,
											double fy,
                      double sigma);
extern double fem_plast_H_RO(long ePos, 
                      double k, 
                      double n, 
                      double E,
                      double sigma);

/* Material status values: 0=linear, 1=plastic, -1=unloading */


/* elasticity condition derivatives: */
int vmis_deriv(tVector *deriv, tVector *stress, long type, double alpha)
{
  double s_x, s_y, s_z, s_q, t_xy, t_yz, t_zx, mult, mult1, J2 ;

  s_q =  (femVecGet(stress,1) + femVecGet(stress,2) + femVecGet(stress,3)) / 3.0 ;

  s_x  = femVecGet (stress, 1 ) - s_q ;
  s_y  = femVecGet (stress, 2 ) - s_q ;
  s_z  = femVecGet (stress, 3 ) - s_q ;
  t_yz = femVecGet (stress, 4 ) ;
  t_zx = femVecGet (stress, 5 ) ;
  t_xy = femVecGet (stress, 6 ) ;

  J2 = 0.5*(s_x*s_x + s_y*s_y + s_z*s_z) + t_xy*t_xy + t_yz*t_yz + t_zx*t_zx ;

  if (type == 1) /* von mises */
  {
    mult = sqrt(3.0) / (2.0 * sqrt(J2) ) ;
    mult1 = 0 ;
  }
  else /* drucker-prager */
  {
    mult = 1.0 / (2.0 * sqrt(J2) ) ;
    mult1 = 1.0 ;
  }

  femVecPut(deriv,1, alpha*mult1 + mult*(s_x )) ;
  femVecPut(deriv,2, alpha*mult1 + mult*(s_y )) ;
  femVecPut(deriv,3, alpha*mult1 + mult*(s_z )) ;
  femVecPut(deriv,4, mult*(2.0 * t_yz)) ;
  femVecPut(deriv,5, mult*(2.0 * t_zx)) ;
  femVecPut(deriv,6, mult*(2.0 * t_xy)) ;

  return(AF_OK);
}

double compute_sigma_e(tVector *sigma)
{
	return(  sqrt( 0.5 * (
				pow ((femVecGet(sigma,1) - femVecGet(sigma,2)), 2) +
				pow ((femVecGet(sigma,2) - femVecGet(sigma,3)), 2) +
				pow ((femVecGet(sigma,3) - femVecGet(sigma,1)), 2) +
				6.0 * (
					pow(femVecGet(sigma,4), 2) +
					pow(femVecGet(sigma,5), 2) +
					pow(femVecGet(sigma,6), 2)
					)
				)));
}

/** von Mises elastoplastic matrix */
int fem_vmis_D_3D(long ePos, 
                  long e_rep, 
                  long eT, 
                  long mT, 
                  tVector *sigma, 
                  tVector *epsilon, 
                  long Mode, 
                  tMatrix *Dep)
{
  int rv = AF_OK ;
  long   state = 0 ;
  double Ex, E1, nu, fy, n, k ;
  double s_eqv2 ;
  double f ;
  double cohes, frict ;
  double H = 0.0 ;
  tVector deriv ;
  tVector old_sigma ;
  tMatrix De ;
  long type    = 1 ;
  double alpha = 0.0 ;
  double kk    = 0.0 ;
  double prev ;
  long i ;

  femVecNull(&deriv) ;
  femVecNull(&old_sigma) ;
  femMatNull(&De) ;

  if ((rv=femVecFullInit(&deriv, 6)) != AF_OK){goto memFree;}
  if ((rv=femVecFullInit(&old_sigma, 6)) != AF_OK){goto memFree;}
  if ((rv=femFullMatInit(&De, 6, 6)) != AF_OK){goto memFree;}

  femVecPut(&old_sigma,1, femGetEResVal(ePos,RES_SX,e_rep));
  femVecPut(&old_sigma,2, femGetEResVal(ePos,RES_SY,e_rep));
  femVecPut(&old_sigma,3, femGetEResVal(ePos,RES_SZ,e_rep));
  femVecPut(&old_sigma,4, femGetEResVal(ePos,RES_SYZ,e_rep));
  femVecPut(&old_sigma,5, femGetEResVal(ePos,RES_SZX,e_rep));
  femVecPut(&old_sigma,6, femGetEResVal(ePos,RES_SXY,e_rep));

	state =  (long)femGetEResVal(ePos, RES_CR1, e_rep);
	H     = femGetEResVal(ePos, RES_PSI, e_rep);

  Ex  = femGetMPValPos(ePos, MAT_EX,   0)  ;
  nu  = femGetMPValPos(ePos, MAT_NU,   0)  ;
  fy  = femGetMPValPos(ePos, MAT_F_YC, 0)  ;
  E1  = femGetMPValPos(ePos, MAT_HARD, 0)  ;

  if (fabs(fy) > FEM_ZERO)
  {
    type = 1 ;
  }
  else
  {
    type = 0;
    cohes  = femGetMPValPos(ePos, MAT_COHES, 0)  ;
    frict  = femGetMPValPos(ePos, MAT_FRICT, 0)  ;
    alpha = ( 2.0 * sin(frict)) / (sqrt(3.0) * (3.0 - sin(frict)) ) ;
    kk    = ( 6.0 * cohes * cos(frict)) / (sqrt(3.0) * (3.0 - sin(frict)) ) ;
  }

  if (E1 <= FEM_ZERO)
  {
    n  = femGetMPValPos(ePos, MAT_RAMB_N, 0)  ;
    k  = femGetMPValPos(ePos, MAT_RAMB_K, 0)  ;
    E1 = 0.0 ;
  }
  else
  {
    k = 0.0 ;
    n = 0.0 ;
  }

	if ((state < 1)||(E1 == Ex)) 
	{
    return(femD_3D_iso(ePos, Ex, nu, Dep)); /* linear solution */
	}
	else
	{
    femD_3D_iso(ePos, Ex, nu, &De);
    vmis_deriv(&deriv, &old_sigma, type, alpha) ;
    chen_Dep(&deriv, H, &De, Dep) ;
	}
    
  if (Mode == AF_YES)
  {
    if  (E1 == Ex)
	  {
      femD_3D_iso(ePos,Ex, nu, Dep); /* linear solution */
	  }
	  else
	  {
      femD_3D_iso(ePos,Ex, nu, &De); 

	    femMatVecMult(Dep, epsilon, sigma) ;
      for (i=1; i<=6; i++) { femVecAdd(sigma,i, femVecGet(&old_sigma, i)) ; }

      if (type == 1)
      {
        s_eqv2 = (3.0 * stress3D_J2dev(sigma) ) ;
        prev = (3.0*stress3D_J2dev(&old_sigma)) ;

        f = s_eqv2 - (fy*fy) ;
      }
      else
      {
        s_eqv2 = alpha*stress3D_I1(sigma) + (stress3D_J2dev(sigma) ) ;
        prev   = alpha*stress3D_I1(&old_sigma) + (stress3D_J2dev(&old_sigma) ) ;

        f = s_eqv2 - kk ;
      }

      if (state == 0)
      {
        /* still elastic */
        if (f <= 0.0)
        {
          state = 0 ; /* continues to be elastic */
        }
        else
        {
          state = 1 ; /* yields */
        }
      }
      else
      {
        if (s_eqv2 < prev)
        {
          state = -1 ; /* unloading */
        }
        else
        {
          state = 1 ; /* plastic */
        }
      }
  
      switch (state) /* new matrix computation: */
      {
        case 1: /* plastic */
          H = fem_plast_H_linear(ePos, Ex, E1, fy, compute_sigma_e(&old_sigma) );
          vmis_deriv(&deriv, &old_sigma, type, alpha) ; /* works better with old_sigma !? */
          chen_Dep(&deriv, H, &De, Dep) ;
          break;
        case 0: /* elastic */
        case -1: /* plastic unloading */
        default: /* error */
          femD_3D_iso(ePos,Ex, nu, Dep);
          break ;
      }

	    femPutEResVal(ePos, RES_CR1, e_rep, state);
	    femPutEResVal(ePos, RES_PSI, e_rep, H);
	  }
  }

memFree:
  femVecFree(&deriv) ;
  femVecFree(&old_sigma) ;
  femMatFree(&De) ;
  return(rv) ;
}

/** 2D VERSION OF VON MISES *************************** */

/* elasticity condition derivatives: */
int vmis_deriv2D(tVector *deriv, tVector *stress, long type, double alpha)
{
  double s_x, s_y, t_xy, s_q, J2, mult, mult1 ;

  s_q =  (femVecGet(stress,1) + femVecGet(stress,2)) / 3.0 ;
  s_x  = femVecGet (stress, 1 ) - s_q ;
  s_y  = femVecGet (stress, 2 ) - s_q ;
  t_xy = femVecGet (stress, 3 ) ;

  J2 = 0.5*(s_x*s_x + s_y*s_y) + t_xy*t_xy ;

  if (type == 1) /* von mises */
  {
    mult = sqrt(3.0) / (2.0 * sqrt(J2) ) ;
    mult1 = 0 ;
  }
  else /* drucker-prager */
  {
    mult = 1.0 / (2.0 * sqrt(J2) ) ;
    mult1 = 1.0 ;
  }

  femVecPut(deriv,1, alpha*mult1 + mult*(s_x  )) ;
  femVecPut(deriv,2, alpha*mult1 + mult*(s_y )) ;
  femVecPut(deriv,3, mult*(2.0 * t_xy)) ;

  return(AF_OK);
}

double sigma_vmis2D(tVector *sigma)
{
	double sx, sy, sxy;

	sx = femVecGet(sigma,1);
	sy = femVecGet(sigma,2);
	sxy = femVecGet(sigma,3);

	return( sqrt((sx*sx + sy*sy -sx*sy + 3*sxy*sxy)));
}

/** von Mises elastoplastic matrix */
int fem_vmis_D_2D(long ePos, 
                  long e_rep, 
                  long Problem, 
                  tVector *epsilon, 
                  long Mode, 
                  tMatrix *Dep)
{
  int rv = AF_OK ;
  long   state = 0 ;
  double Ex, E1, nu, fy ;
  double f ;
  double H = 0.0 ;
  double cohes, frict ;
  double k, n ;
  double s_eqv2 ;
  tVector deriv ;
  tVector sigma ;
  tVector old_sigma ;
  tMatrix De ;
  long type    = 1 ;
  double alpha = 0.0 ;
  double kk    = 0.0 ;
  double prev ;
  long i ;

  femVecNull(&deriv) ;
  femVecNull(&old_sigma) ;
  femVecNull(&sigma) ;
  femMatNull(&De) ;

  if ((rv=femVecFullInit(&deriv, 3)) != AF_OK){goto memFree;}
  if ((rv=femVecFullInit(&sigma, 3)) != AF_OK){goto memFree;}
  if ((rv=femVecFullInit(&old_sigma, 3)) != AF_OK){goto memFree;}
  if ((rv=femFullMatInit(&De, 3, 3)) != AF_OK){goto memFree;}

  femVecPut(&old_sigma,1, femGetEResVal(ePos,RES_SX,e_rep));
  femVecPut(&old_sigma,2, femGetEResVal(ePos,RES_SY,e_rep));
  femVecPut(&old_sigma,3, femGetEResVal(ePos,RES_SXY,e_rep));

	state =  (long)femGetEResVal(ePos, RES_STAT1, e_rep);
	H     = femGetEResVal(ePos, RES_STAT2, e_rep);

  Ex  = femGetMPValPos(ePos, MAT_EX,   0)  ;
  nu  = femGetMPValPos(ePos, MAT_NU,   0)  ;
  fy  = femGetMPValPos(ePos, MAT_F_YC, 0)  ;
  E1  = femGetMPValPos(ePos, MAT_HARD, 0)  ;

  if (fabs(fy) > FEM_ZERO)
  {
    type = 1 ;
  }
  else
  {
    type = 0;
    cohes  = femGetMPValPos(ePos, MAT_COHES, 0)  ;
    frict  = femGetMPValPos(ePos, MAT_FRICT, 0)  ;
    alpha = ( 2.0 * sin(frict)) / (sqrt(3.0) * (3.0 - sin(frict)) ) ;
    kk    = ( 6.0 * cohes * cos(frict)) / (sqrt(3.0) * (3.0 - sin(frict)) ) ;
  }

  if (E1 <= FEM_ZERO)
  {
    n  = femGetMPValPos(ePos, MAT_RAMB_N, 0)  ;
    k  = femGetMPValPos(ePos, MAT_RAMB_K, 0)  ;
    E1 = 0.0 ;
  }
  else
  {
    k = 0.0 ;
    n = 0.0 ;
  }

  if ((state < 1)||(E1 == Ex))
  {
    D_HookIso_planeRaw(Ex, nu, Problem, Dep);
  }
  else
  {
    D_HookIso_planeRaw(Ex, nu, Problem, &De);
    vmis_deriv2D(&deriv, &old_sigma, type, alpha) ;
    chen_Dep(&deriv, H, &De, Dep) ; /* should work in 2D, too */
  }
  
	/* NEW matrix: */
  if (Mode == AF_YES)
  {
    if  (E1 == Ex)
	  {
      D_HookIso_planeRaw(Ex, nu, Problem, Dep); /* linear solution */
	  }
	  else
	  {
      D_HookIso_planeRaw(Ex, nu, Problem, &De);

	    femMatVecMult(Dep, epsilon, &sigma) ;
      for (i=1; i<=3; i++) { femVecAdd(&sigma,i, femVecGet(&old_sigma, i)) ; }

      if (type == 1)
      {
        s_eqv2 = (3.0 * stress2D_J2dev(&sigma) ) ;
        prev = (3.0*stress2D_J2dev(&old_sigma)) ;

        f = s_eqv2 - (fy*fy) ;
      }
      else
      {
        s_eqv2 = alpha*stress2D_I1(&sigma) + (stress2D_J2dev(&sigma) ) ;
        prev   = alpha*stress2D_I1(&old_sigma) + (stress2D_J2dev(&old_sigma) ) ;

        f = s_eqv2 - kk ;
      }

      if (state == 0)
      {
        /* still elastic */
        if (f <= 0.0)
        {
          state = 0 ; /* continues to be elastic */
        }
        else
        {
          state = 1 ; /* yields */
        }
      }
      else
      {
        if (s_eqv2 < prev)
        {
          state = -1 ; /* unloading */
        }
        else
        {
          state = 1 ; /* plastic */
        }
      }
  
      switch (state) /* new matrix computation: */
      {
        case 1: /* plastic */
          H = fem_plast_H_linear(ePos, Ex, E1, fy, sigma_vmis2D(&old_sigma) );
          vmis_deriv2D(&deriv, &old_sigma, type, alpha) ; /* works better with old_sigma !? */
          chen_Dep(&deriv, H, &De, Dep) ;
          break;
        case 0: /* elastic */
        case -1: /* plastic unloading */
        default: /* error */
          D_HookIso_planeRaw(Ex, nu, Problem, Dep);
          break ;
      }
      
	    femPutEResVal(ePos, RES_STAT1, e_rep, state);
	    femPutEResVal(ePos, RES_STAT2, e_rep, H);
	  }
  }

memFree:
  femVecFree(&deriv) ;
  femVecFree(&old_sigma) ;
  femVecFree(&sigma) ;
  femMatFree(&De) ;
  return(rv) ;
}

/* end of fem_vmis.c */
