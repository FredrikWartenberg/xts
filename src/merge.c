/*
#   xts: eXtensible time-series 
#
#   Copyright (C) 2008  Jeffrey A. Ryan jeff.a.ryan @ gmail.com
#
#   Contributions from Joshua M. Ulrich
#
#   This program is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 2 of the License, or
#   (at your option) any later version.
#
#   This program is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include <R.h>
#include <Rinternals.h>
#include <Rdefines.h>
#include "xts.h"

/*
 * These types and functions allow us to perform a merge on either double-
 * or integer-indexed xts objects without repeating the code for the
 * generic algorithm.
 */

/* A single xts index, either double or int */
typedef union xts_index {
  double *d;
  int *i;
} xts_index;

/* The set of xts indices (result, x, y) involved in the merge */
typedef struct xts_indices {
  xts_index r;
  xts_index x;
  xts_index y;
} xts_indices;

/* Functions to set the result observation to either x or y */
typedef void (*set_index_func)(xts_indices *, int, int);
void set_index_from_x_double(xts_indices *idx, int rp, int xp) {
  idx->r.d[rp] = idx->x.d[xp];
}
void set_index_from_y_double(xts_indices *idx, int rp, int yp) {
  idx->r.d[rp] = idx->y.d[yp];
}
void set_index_from_x_int(xts_indices *idx, int rp, int xp) {
  idx->r.i[rp] = idx->x.i[xp];
}
void set_index_from_y_int(xts_indices *idx, int rp, int yp) {
  idx->r.i[rp] = idx->y.i[yp];
}

/* Functions to compare the indices for x and y */
typedef int (*compare_func)(xts_indices *, int, int);
int compare_indexes_double(xts_indices *idx, int xp, int yp) {
  if (idx->x.d[xp] > idx->y.d[yp]) return  1;
  if (idx->x.d[xp] < idx->y.d[yp]) return -1;
  return 0;
}
int compare_indexes_int(xts_indices *idx, int xp, int yp) {
  if (idx->x.i[xp] > idx->y.i[yp]) return  1;
  if (idx->x.i[xp] < idx->y.i[yp]) return -1;
  return 0;
}

/* Set merge result methods */
typedef void (*set_result_func)(SEXP, SEXP, int, int);
void set_result_dbl(SEXP _result, SEXP _x, int iresult, int ix)
{
  REAL(_result)[iresult] = REAL(_x)[ix];
}
void set_result_int(SEXP _result, SEXP _x, int iresult, int ix)
{
  INTEGER(_result)[iresult] = INTEGER(_x)[ix];
}
void set_result_lgl(SEXP _result, SEXP _x, int iresult, int ix)
{
  LOGICAL(_result)[iresult] = LOGICAL(_x)[ix];
}
void set_result_cplx(SEXP _result, SEXP _x, int iresult, int ix)
{
  COMPLEX(_result)[iresult] = COMPLEX(_x)[ix];
}
void set_result_str(SEXP _result, SEXP _x, int iresult, int ix)
{
  SET_STRING_ELT(_result, iresult, STRING_ELT(_x, ix));
}

/* Calculate merge result index length */
int merge_index_length(xts_indices*, compare_func, int, int, int, int);
int merge_index_length(xts_indices *idx, compare_func cmpfn,
    int nx, int ny, int ljoin, int rjoin)
{
  int i = 0, xi = 1, yi = 1;
  int max_length = nx + ny + 1;

  /* Special-case the inner join, since it is the default */
  if (rjoin && ljoin) {
    while ((xi + yi) <= max_length) {
      if (xi > nx) {
        i += (ny - yi + 1);
        return i;
      }
      if (yi > ny) {
        i += (nx - xi + 1);
        return i;
      } else {
        int comp = cmpfn(idx, xi-1, yi-1);
        i++;
        if (comp == 0) {
          /* INNER JOIN */
          yi++;
          xi++;
        } else if (comp < 0) {
          /* LEFT JOIN */
          xi++;
        } else if (comp > 0) {
          /* RIGHT JOIN */
          yi++;
        } else
          error("invalid comparison function output; should not happen");
      }
    }
  } else
  /* Run the generic algorithm */
  while ((xi + yi) <= max_length) {
    if (xi > nx) {
      yi++;
      if (rjoin) i++;
    } else if (yi > ny) {
      xi++;
      if (ljoin) i++;
    } else {
      int comp = cmpfn(idx, xi-1, yi-1);
      if (comp == 0) {
        /* INNER JOIN  --- only result if all=FALSE */
        yi++;
        xi++;
        i++;
      } else
      if (comp < 0) {
        /* LEFT JOIN */
        xi++;
        if (ljoin) i++;
      } else
      if (comp > 0) {
        /* RIGHT JOIN */
        yi++;
        if (rjoin) i++;
      } else
        error("invalid comparison function output; should not happen");
    }
  }
  return i;
}

/* 

  This is a merge_join algorithm used to
  allow two xts objects to be merged as one
  along a common index efficiently and fast

  The code is branched for REAL and INTEGER indexed values
  which allows for efficient memory usage and minimal
  testing/coercion

  
  Copyright Jeffrey A. Ryan 2008

*/
/* do_merge_xts {{{ */
SEXP do_merge_xts (SEXP x, SEXP y,
                   SEXP all,
                   SEXP fill,
                   SEXP retclass,
                   SEXP colnames, 
                   SEXP suffixes,
                   SEXP retside,
                   SEXP env,
                   int coerce)
{
  int nrx, ncx, nry, ncy, len;
  int left_join, right_join;
  int i = 0, j = 0, xp = 1, yp = 1; /* x and y positions in index */
  int mode;
  int ij_original, ij_result;
  int p = 0;
  SEXP xindex, yindex, index, result, attr, len_xindex;
  SEXP s, t, unique;

  int *int_result=NULL, *int_x=NULL, *int_y=NULL, int_fill=0;
  double *real_result=NULL, *real_x=NULL, *real_y=NULL;

  /* we do not check that 'x' is an xts object.  Dispatch and mergeXts
    (should) make this unecessary.  So we just get the index value 

    This assumption seems to be invalid when dispatched from cbind.xts
    So we need to check that the objects are not NULL, or at least
    treat NULL objects as zero-width with an index that matches the non-null
   
    2009/01/07: calling merge(NA,x) or merge(1,1,xts) causes a segfault;
                calling merge(1,x) causes the xts-info (none!) from the 1st arg
                to be used, resulting in a classless object. [fixed - jar]
  */
  if( isNull(x) || isNull(y) ) {
    if(!isNull(x)) return(x);
    return(y);
  }

  PROTECT( xindex = getAttrib(x, xts_IndexSymbol) ); p++;

  /* convert to xts object if needed */
  if( !isXts(y) ) {
    PROTECT(s = t = allocList(4)); p++;
    SET_TYPEOF(s, LANGSXP);
    SETCAR(t, install("try.xts")); t = CDR(t);
    SETCAR(t, y); t = CDR(t);
    PROTECT( len_xindex = allocVector(INTSXP, 1)); p++;
    INTEGER(len_xindex)[0] = length(xindex);
    SETCAR(t, len_xindex);
    SET_TAG(t, install("length.out")); t = CDR(t);
    SETCAR(t, install(".merge.xts.scalar"));
    SET_TAG(t, install("error"));
    PROTECT(y = eval(s, env)); p++;
  } /* end conversion process */

  mode = TYPEOF(x);

  if( isXts(y) ) {
    PROTECT( yindex = getAttrib(y, xts_IndexSymbol) ); p++;
  } else {
    PROTECT( yindex = getAttrib(x, xts_IndexSymbol) ); p++;
  }

  if( TYPEOF(retside) != LGLSXP )
    error("retside must be a logical value of TRUE or FALSE");

  nrx = nrows(x);
  ncx = ncols(x);
  /* if object is zero-width */
  if( LENGTH(x)==0 || INTEGER(retside)[0]==0 ) {
    nrx = nrows(xindex);
    ncx = 0;
    PROTECT(x = coerceVector(x, TYPEOF(y))); p++;
  }
  
  nry = nrows(y);
  ncy = ncols(y);
  /* if object is zero-width */
  if( LENGTH(y)==0 || INTEGER(retside)[1]==0) {
    nry = nrows(yindex);
    ncy = 0;
    PROTECT(y = coerceVector(y, TYPEOF(x))); p++;
  }

  len = nrx + nry;

  /* at present we are failing the call if the indexing is of
     mixed type.  This should probably instead simply coerce
     to REAL so as not to lose any information (at the expense
     of conversion cost and memory), and issue a warning. */
  if( TYPEOF(xindex) != TYPEOF(yindex) )
  {
    PROTECT(xindex = coerceVector(xindex, REALSXP)); p++;
    PROTECT(yindex = coerceVector(yindex, REALSXP)); p++;
  }

  if( TYPEOF(all) != LGLSXP )
    error("all must be a logical value of TRUE or FALSE");

  left_join = INTEGER(all)[ 0 ];
  right_join = INTEGER(all)[ 1 ];

  /* Support both double and integer indexes. Assign comparison and setter
   * functions based on which index type we are merging.
   */
  compare_func compare_indexes = NULL;
  set_index_func set_index_from_x = NULL;
  set_index_func set_index_from_y = NULL;
  xts_indices *idx = NULL;
  switch (TYPEOF(xindex)) {
    case REALSXP:
      compare_indexes = compare_indexes_double;
      set_index_from_x = set_index_from_x_double;
      set_index_from_y = set_index_from_y_double;
      xts_index rd, xd, yd;
      rd.d = NULL;
      xd.d = REAL(xindex);
      yd.d = REAL(yindex);
      idx = &(xts_indices){ rd, xd, yd };
      /* Check for illegal values before looping. Due to ordered index,
       * -Inf must be first, while NA, Inf, and NaN must be last. */
      if (!R_FINITE(xd.d[0]) || !R_FINITE(xd.d[nrx-1])
       || !R_FINITE(yd.d[0]) || !R_FINITE(yd.d[nry-1])) {
        error("'index' cannot contain 'NA', 'NaN', or '+/-Inf'");
      }
      break;
    case INTSXP:
      compare_indexes = compare_indexes_int;
      set_index_from_x = set_index_from_x_int;
      set_index_from_y = set_index_from_y_int;
      xts_index ri, xi, yi;
      ri.i = NULL;
      xi.i = INTEGER(xindex);
      yi.i = INTEGER(yindex);
      idx = &(xts_indices){ ri, xi, yi };

      //idx = &(xts_indices){ NULL,
      //  (xts_index)INTEGER(xindex),
      //  (xts_index)INTEGER(yindex) };

      /* Check for NA before looping; logical ops on NA may yield surprising
       * results. Note that the NA_integer_ will appear in the last value of
       * the index because of sorting at the R level, even though NA_INTEGER
       * equals INT_MIN at the C level. */
      if (xi.i[nrx-1] == NA_INTEGER || yi.i[nry-1] == NA_INTEGER) {
         error("'index' cannot contain 'NA'");
      }
      break;
    default:
      error("invalid index type");
      break;
  }

  /* determine num_rows of final merged xts object
     
     this seems to only cost 1/1000 of a sec per
     1e6 observations.  Acceptable 'waste' given
     that now we can properly allocate space
     for our results

     We also check the index type and use the appropriate macros
   */
  i = merge_index_length(idx, compare_indexes, nrx, nry, left_join, right_join);

  if(i == 0) {
    /* if no rows match, return an empty xts object, similar in style to zoo */
    PROTECT( result = allocVector(TYPEOF(x), 0) ); p++;
    PROTECT( index  = allocVector(TYPEOF(xindex), 0) ); p++;
    SET_xtsIndex(result, index);
    if(LOGICAL(retclass)[0])
      setAttrib(result, R_ClassSymbol, getAttrib(x, R_ClassSymbol));
    UNPROTECT(p);
    return result;
  }

  int num_rows = i;
  xp = 1; yp = 1;

  PROTECT( index  = allocVector(TYPEOF(xindex), num_rows) ); p++;
  /* coercion/matching of TYPE for x and y needs to be checked,
     either here or in the calling R code.  I suspect here is
     more useful if other function can call the C code as well. 
     If objects are not the same type, convert to REALSXP. */
  if( coerce || TYPEOF(x) != TYPEOF(y) ) {
    PROTECT( x = coerceVector(x, REALSXP) ); p++;
    PROTECT( y = coerceVector(y, REALSXP) ); p++;
  }
  PROTECT( result = allocVector(TYPEOF(x), (ncx + ncy) * num_rows) ); p++;

  /* Ensure fill is the correct length and type */
  if( length(fill) < 1 ) {
    PROTECT( fill = ScalarLogical(NA_LOGICAL) ); p++;
  }
  if( TYPEOF(fill) != TYPEOF(x) ) {
    PROTECT( fill = coerceVector(fill, TYPEOF(x)) ); p++;
  } 

  mode = TYPEOF(x);

  /* use pointers instead of function calls */
  set_result_func set_result = NULL;
  switch(TYPEOF(x)) {
    case LGLSXP:
      set_result = set_result_lgl;
      break;
    case INTSXP:
      set_result = set_result_int;
      break;
    case REALSXP:
      set_result = set_result_dbl;
      break;
    case CPLXSXP:
      set_result = set_result_cplx;
      break;
    case STRSXP:
      set_result = set_result_str;
      break;
    default:
      error("unsupported data type");
      break;
   }

  switch(TYPEOF(xindex)) {
    case INTSXP:
      idx->r.i = INTEGER(index);
      break;
    case REALSXP:
      idx->r.d = REAL(index);
      break;
    default:
      error("invalid index type");
      break;
  }

  /* INDEXING */
  for(i = 0; i < num_rows; i++) {
    /* If we are past the last row in x, assign NA to merged data 
       and copy the y column values to the second side of result
    */
    if( xp > nrx ) {
      if(right_join) {
        set_index_from_y(idx, i, yp-1);
        for(j = 0; j < ncx; j++) { /* x-values */
          ij_result = i + j * num_rows;
          set_result(result, fill, ij_result, 0);
        }
        for(j = 0; j < ncy; j++) { /* y-values */
          ij_result = i + (j+ncx) * num_rows;
          ij_original = (yp-1) + j * nry;
          set_result(result, y, ij_result, ij_original);
        }
      }
      yp++;
      if(!right_join) i--;  /* if all=FALSE, we must decrement i for each non-match */
    } else

    /* past the last row of y */
    if( yp > nry ) {
      if(left_join) {

        /* record new index value */
        set_index_from_x(idx, i, xp-1);

        /* copy values from x and y to result */
        for(j = 0; j < ncx; j++) { /* x-values */
          ij_result = i + j * num_rows;
          ij_original = (xp-1) + j * nrx; 
          set_result(result, x, ij_result, ij_original);
        }

        /* we are out of y-values, so fill merged result with NAs */
        for(j = 0; j < ncy; j++) { /* y-values */
          ij_result = i + (j+ncx) * num_rows;
          set_result(result, fill, ij_result, 0);
        }
      }
      xp++;
      if(!left_join) i--;
    } else {

    int comp = compare_indexes(idx, xp-1, yp-1);
    /* matching index values copy all column values from x and y to results */
    if (comp == 0) {
      set_index_from_x(idx, i, xp-1);
      /* copy x-values to result */
      for(j = 0; j < ncx; j++) { /* x-values */
        ij_result = i + j * num_rows;
        ij_original = (xp-1) + j * nrx;
        set_result(result, x, ij_result, ij_original);
      }

      /* copy y-values to result */
      for(j = 0; j < ncy; j++) { /* y-values */
        ij_result = i + (j+ncx) * num_rows;
        ij_original = (yp-1) + j * nry;
        set_result(result, y, ij_result, ij_original);
      }
      xp++;
      yp++;
    } else

    if (comp < 0) {
      if(left_join) {
        set_index_from_x(idx, i, xp-1);
        for(j = 0; j < ncx; j++) { /* x-values */
          ij_result = i + j * num_rows;
          ij_original = (xp-1) + j * nrx;
          set_result(result, x, ij_result, ij_original);
        }
        for(j = 0; j < ncy; j++) { /* y-values */
          ij_result = i + (j+ncx) * num_rows;
          set_result(result, fill, ij_result, 0);
        }
      }
      xp++;
      if(!left_join) i--;
    } else

    if (comp > 0) {
      if(right_join) {
        set_index_from_y(idx, i, yp-1);
        for(j = 0; j < ncx; j++) { /* x-values */
          ij_result = i + j * num_rows;
          set_result(result, fill, ij_result, 0);
        }
        for(j = 0; j < ncy; j++) { /* y-values */
          ij_result = i + (j+ncx) * num_rows;
          ij_original = (yp-1) + j * nry;
          set_result(result, y, ij_result, ij_original);
        }
      }
      yp++;
      if(!right_join) i--;
    } else {
      error("invalid comparison, should never happen");
    }
    }
  }

  /* following logic to allow for 
     dimensionless xts objects (unsupported)
     to be used in Ops.xts calls
     This maps to how zoo behaves */
  if(LOGICAL(retside)[0] &&
     !LOGICAL(retside)[1] && 
     isNull(getAttrib(x,R_DimSymbol))) {
     /* retside=c(T,F) AND is.null(dim(x)) */ 
     setAttrib(result, R_DimSymbol, R_NilValue);
  } else 
  if(LOGICAL(retside)[1] &&
     !LOGICAL(retside)[0] && 
     isNull(getAttrib(y,R_DimSymbol))) {
     /* retside=c(F,T) AND is.null(dim(y)) */ 
     setAttrib(result, R_DimSymbol, R_NilValue);
  } else /* set Dim and DimNames */
  if(num_rows >= 0 && (ncx + ncy) >= 0) {
    /* DIM */
    PROTECT(attr = allocVector(INTSXP, 2));
    INTEGER(attr)[0] = num_rows;
    INTEGER(attr)[1] = ncx + ncy;
    setAttrib(result, R_DimSymbol, attr);
    UNPROTECT(1);
    /* DIMNAMES */
    if(!isNull(colnames)) { // only set DimNamesSymbol if passed colnames is not NULL
      SEXP dimnames, dimnames_x, dimnames_y, newcolnames;
      PROTECT(dimnames = allocVector(VECSXP, 2)); p++;
      PROTECT(dimnames_x = getAttrib(x, R_DimNamesSymbol)); p++;
      PROTECT(dimnames_y = getAttrib(y, R_DimNamesSymbol)); p++;
      PROTECT(newcolnames = allocVector(STRSXP, ncx+ncy)); p++;
      for(i = 0; i < (ncx + ncy); i++) {
        if( i < ncx ) {
          if(!isNull(dimnames_x) && !isNull(VECTOR_ELT(dimnames_x,1))) {
            SET_STRING_ELT(newcolnames, i, STRING_ELT(VECTOR_ELT(dimnames_x,1),i));
          } else {
            SET_STRING_ELT(newcolnames, i, STRING_ELT(colnames, i));
          }
        } else { // i >= ncx; 
          if(!isNull(dimnames_y) && !isNull(VECTOR_ELT(dimnames_y,1))) {
            SET_STRING_ELT(newcolnames, i, STRING_ELT(VECTOR_ELT(dimnames_y,1),i-ncx));
          } else {
            SET_STRING_ELT(newcolnames, i, STRING_ELT(colnames, i));
          }
        }
      }
      SET_VECTOR_ELT(dimnames, 0, R_NilValue);  // ROWNAMES are NULL

      PROTECT(s = t = allocList(3)); p++;
      SET_TYPEOF(s, LANGSXP);
      SETCAR(t, install("make.names")); t = CDR(t);
      SETCAR(t, newcolnames); t = CDR(t);
      PROTECT(unique = allocVector(LGLSXP, 1)); p++;  LOGICAL(unique)[0] = 1;
      SETCAR(t, unique);  SET_TAG(t, install("unique"));
      SET_VECTOR_ELT(dimnames, 1, eval(s, env));
 
      //SET_VECTOR_ELT(dimnames, 1, newcolnames); // COLNAMES are passed in
      setAttrib(result, R_DimNamesSymbol, dimnames);
    }
  } else {
    // only used for zero-width results! xts always has dimension
    setAttrib(result, R_DimSymbol, R_NilValue);
  }

  setAttrib(result, xts_IndexSymbol, index);
  if(LOGICAL(retclass)[0])
    setAttrib(result, R_ClassSymbol, getAttrib(x, R_ClassSymbol));
  setAttrib(result, xts_IndexClassSymbol, getAttrib(x, xts_IndexClassSymbol));
  setAttrib(result, xts_IndexTZSymbol, getAttrib(x, xts_IndexTZSymbol));
  setAttrib(result, xts_IndexFormatSymbol, getAttrib(x, xts_IndexFormatSymbol));
  setAttrib(result, xts_ClassSymbol, getAttrib(x, xts_ClassSymbol));
  copy_xtsAttributes(x, result);

  UNPROTECT(p);
  return result;  
} //}}}

//SEXP mergeXts (SEXP all, SEXP fill, SEXP retclass, SEXP colnames, SEXP retside, SEXP env, SEXP args)
/* called via .External("mergeXts", ...) */
SEXP mergeXts (SEXP args) // mergeXts {{{
{
  SEXP _x, _y, xtmp, result, _INDEX;
  /* colnames should be renamed as suffixes, as colnames need to be added at the C level */
  SEXP all, fill, retc, retclass, symnames,
       suffixes, rets, retside, env, tzone;
  int nr, nc, ncs=0;
  int index_len;
  int i, n=0, P=0;

  SEXP argstart;

  args = CDR(args); all = CAR(args);
  args = CDR(args); fill = CAR(args);
  args = CDR(args); retclass = CAR(args);
  args = CDR(args); symnames = CAR(args);
  args = CDR(args); suffixes = CAR(args);
  args = CDR(args); retside = CAR(args);
  args = CDR(args); env = CAR(args);
  args = CDR(args); tzone = CAR(args);
  args = CDR(args);
  // args should now correspond to the ... objects we are looking to merge 
  argstart = args; // use this to rewind list...

  n = 0;
  int type_of;
  int coerce_to_double=0;
  if(args != R_NilValue) type_of = TYPEOF(CAR(args));
  while(args != R_NilValue) {
    if(length(CAR(args)) > 0) {
      ncs += ncols(CAR(args));
      /* need to convert all objects if one non-zero-width needs to be converted */
      if(TYPEOF(CAR(args)) != type_of) {
        coerce_to_double = 1;
      }
    }
    args = CDR(args);
    n++;
  }


  /* build an index to be used in all subsequent calls */
  args = argstart;

  _x = CAR(args);
  args = CDR(args);

  int leading_non_xts = 0;
  while( !isXts(_x) ) {
    if( args == R_NilValue ) error("no xts object to merge");
    leading_non_xts = 1;
    /*warning("leading non-xts objects may have been dropped");*/
    _x = CAR(args);
    args = CDR(args);
  }
  /* test for NULLs that may be present from cbind dispatch */
  if(!leading_non_xts) { /* leading non-xts in 2 case scenario was igoring non-xts value */
    if(n < 3 && (args == R_NilValue || (isNull(CAR(args)) && length(args) == 1))) {/* no y arg or y==NULL */
      return(_x);
    }
  }

  if( args != R_NilValue) {
    _y = CAR(args);
    args = CDR(args);
  } else {
    PROTECT(_y = duplicate(_x)); P++;
  }

  if(n > 2 || leading_non_xts) { /*args != R_NilValue) {*/
    /* generalized n-case optimization
       currently if n>2 this is faster and more memory efficient
       than recursively building a merged object, object by object. */

    PROTECT(retc = allocVector(LGLSXP, 1)); P++;
    LOGICAL(retc)[0] = 1; /* return class == TRUE */
    PROTECT(rets = allocVector(LGLSXP, 2)); P++;
    LOGICAL(rets)[0] = 0; /* don't return left */
    LOGICAL(rets)[1] = 0; /* don't return right */
  
    if( isNull(_y) ) {
      PROTECT(_y = duplicate(_x)); P++;
    }

    // REPROTECT _INDEX in while loop
    PROTECT_INDEX idx;
    PROTECT_WITH_INDEX(_INDEX = do_merge_xts(_x,
                                             _y,
                                             all,
                                             fill,
                                             retc,
                                             R_NilValue,
                                             R_NilValue,
                                             rets,
                                             env,
                                             coerce_to_double), &idx); P++;

    /* merge all objects into one zero-width common index */
    while(args != R_NilValue) { 
      if( !isNull(CAR(args)) ) {
        REPROTECT(_INDEX = do_merge_xts(_INDEX,
                                        CAR(args),
                                        all,
                                        fill,
                                        retc,
                                        R_NilValue,
                                        R_NilValue,
                                        rets,
                                        env,
                                        coerce_to_double), idx);
      }
      args = CDR(args);
    }

    index_len = length(GET_xtsIndex(_INDEX));
  
    args = argstart; // reset args
    int ii, jj, iijj, jj_result;
    int *int_result=NULL, *int_xtmp=NULL;
    double *real_result=NULL, *real_xtmp=NULL;

    PROTECT(result = allocVector(TYPEOF(_INDEX), index_len * ncs)); P++;
    switch(TYPEOF(result)) {
      case LGLSXP:
      case INTSXP:
        int_result = INTEGER(result);
        break;
      case REALSXP:
        real_result = REAL(result);
        break;
      default:
        error("unsupported data type");
    }

    SEXP ColNames, NewColNames;
    PROTECT(NewColNames = allocVector(STRSXP, ncs)); P++;
    ncs = 0;
    // REPROTECT xtmp inside for loop
    PROTECT_INDEX idxtmp, cnmtmp;
    PROTECT_WITH_INDEX(xtmp = NULL, &idxtmp); P++;
    PROTECT_WITH_INDEX(ColNames = NULL, &cnmtmp); P++;

    for(i = 0, nc=0; args != R_NilValue; i = i+nc, args = CDR(args)) { // merge each object with index
      // i is object current being merged/copied
      // nc is offset in current object
      if( isNull(CAR(args)) ) {
        i = i-nc;
        continue;  // if NULL is passed, skip to the next object.
      }

      REPROTECT(xtmp = do_merge_xts(_INDEX,
                                    CAR(args),
                                    all,
                                    fill,
                                    retclass,
                        /*colnames*/R_NilValue,
                                    R_NilValue,
                                    retside,
                                    env,
                                    coerce_to_double), idxtmp);

      nr = nrows(xtmp);
      nc = (0 == nr) ? 0 : ncols(xtmp);  // ncols(numeric(0)) == 1
      ncs += nc;
      REPROTECT(ColNames = getAttrib(CAR(args),R_DimNamesSymbol), cnmtmp);
      switch(TYPEOF(xtmp)) { // by type, insert merged data into result object
        case LGLSXP:
        case INTSXP:
          int_xtmp = INTEGER(xtmp);
          for(jj=0; jj < nc; jj++) {
            if(!isNull(ColNames) && !isNull(VECTOR_ELT(ColNames,1))) {
              /* if merged object has colnames, use these, otherwise use deparse names */
              SET_STRING_ELT(NewColNames, i+jj, STRING_ELT(VECTOR_ELT(ColNames,1),jj));
            } else {
              SET_STRING_ELT(NewColNames, i+jj, STRING_ELT(symnames,i+jj));
            }
            for(ii=0; ii < nr; ii++) {
              iijj = ii + jj * nr;
              jj_result = ii + ( (i+jj) * nr);
              int_result[ jj_result ] = int_xtmp[ iijj ];
            }
          }
          break;
        case REALSXP:
          real_xtmp = REAL(xtmp);
          for(jj=0; jj < nc; jj++) {
            if(!isNull(ColNames) && !isNull(VECTOR_ELT(ColNames,1))) {
              SET_STRING_ELT(NewColNames, i+jj, STRING_ELT(VECTOR_ELT(ColNames,1),jj));
            } else {
              SET_STRING_ELT(NewColNames, i+jj, STRING_ELT(symnames,i+jj));
            }
            for(ii=0; ii < nr; ii++) {
              iijj = ii + jj * nr;
              jj_result = ii + ( (i+jj) * nr);
              real_result[ jj_result ] = real_xtmp[ iijj ];
            }
          }
          break;
      }
    }

    if(ncs > 0) {
      SEXP dim;
      PROTECT(dim = allocVector(INTSXP, 2)); P++;
      INTEGER(dim)[0] = index_len;
      INTEGER(dim)[1] = ncs;
      setAttrib(result, R_DimSymbol, dim);

      SEXP dimnames;
      PROTECT(dimnames = allocVector(VECSXP, 2)); P++;
      SET_VECTOR_ELT(dimnames, 0, R_NilValue); // rownames are always NULL in xts

      /* colnames, assure they are unique before returning */
      SEXP s, t, unique;
      PROTECT(s = t = allocList(3)); P++;
      SET_TYPEOF(s, LANGSXP);
      SETCAR(t, install("make.names")); t = CDR(t);
      SETCAR(t, NewColNames); t = CDR(t);
      PROTECT(unique = allocVector(LGLSXP, 1)); P++;  LOGICAL(unique)[0] = 1;
      SETCAR(t, unique);  SET_TAG(t, install("unique"));
      SET_VECTOR_ELT(dimnames, 1, eval(s, env));
      setAttrib(result, R_DimNamesSymbol, dimnames);
    }

    SET_xtsIndex(result, GET_xtsIndex(_INDEX));
    SET_xtsIndexTZ(result, GET_xtsIndexTZ(_INDEX));
    copy_xtsCoreAttributes(_INDEX, result);
    copy_xtsAttributes(_INDEX, result);

  } else { /* 2-case optimization --- simply call main routine */
    /* likely bug in handling of merge(1, xts) case */
    PROTECT(result = do_merge_xts(_x,
                                  _y, 
                                 all,
                                fill,
                            retclass,
                            symnames /*R_NilValue*/,
                            suffixes,
                             retside,
                                 env,
                    coerce_to_double)); P++;
  }

  SEXP index_tmp = getAttrib(result, xts_IndexSymbol);
  PROTECT(index_tmp); P++;
  if(isNull(tzone)) {
    setAttrib(index_tmp, xts_IndexTzoneSymbol,
              getAttrib(getAttrib(_x,xts_IndexSymbol), xts_IndexTzoneSymbol));
  } else {
    setAttrib(index_tmp, xts_IndexTzoneSymbol, tzone);
  }
  copyMostAttrib(getAttrib(_x,xts_IndexSymbol), index_tmp);
  setAttrib(result, xts_IndexSymbol, index_tmp);
  setAttrib(result, xts_IndexTZSymbol, getAttrib(index_tmp, xts_IndexTzoneSymbol));

  UNPROTECT(P);
  return(result);
} //}}} end of mergeXts
