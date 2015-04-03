/*
  This is a C library of the Fortran SCRIP version 1.4

  ===>>> Please send bug reports to Uwe.Schulzweida@zmaw.de <<<===

  Spherical Coordinate Remapping and Interpolation Package (SCRIP)
  ================================================================

  SCRIP is a software package which computes addresses and weights for
  remapping and interpolating fields between grids in spherical coordinates.
  It was written originally for remapping fields to other grids in a coupled
  climate model, but is sufficiently general that it can be used in other 
  applications as well. The package should work for any grid on the surface
  of a sphere. SCRIP currently supports four remapping options:

  Conservative remapping
  ----------------------
  First- and second-order conservative remapping as described in
  Jones (1999, Monthly Weather Review, 127, 2204-2210).

  Bilinear interpolation
  ----------------------
  Slightly generalized to use a local bilinear approximation
  (only logically-rectangular grids).

  Bicubic interpolation
  ----------------------
  Similarly generalized (only logically-rectangular grids).

  Distance-weighted averaging
  ---------------------------
  Distance-weighted average of a user-specified number of nearest neighbor values.

  Documentation
  =============

  http://climate.lanl.gov/Software/SCRIP/SCRIPusers.pdf

*/
/*
  2012-01-16 Uwe Schulzweida: alloc grid2_bound_box only for conservative remapping
  2011-01-07 Uwe Schulzweida: Changed remap weights from 2D to 1D array
  2009-05-25 Uwe Schulzweida: Changed restrict data type from double to int
  2009-01-11 Uwe Schulzweida: OpenMP parallelization
 */


#if defined(HAVE_CONFIG_H)
#  include "config.h"
#endif

#include <limits.h>
#include <time.h>

#include <cdi.h>
#include "cdo.h"
#include "cdo_int.h"
#include "grid.h"
#include "remap.h"
#include "util.h"  /* progressStatus */


/* used for store_link_fast */

#define BLK_SIZE 4096
#define BLK_NUM(x) (x/grid_store->blk_size)
#define BLK_IDX(x) (x%grid_store->blk_size)
struct grid_layer
{
  int *grid2_link;
  struct grid_layer *next;
};

typedef struct grid_layer grid_layer_t;

typedef struct
{
  int blk_size;
  int max_size;
  int nblocks;
  int *blksize;
  int *nlayers;
  grid_layer_t **layers;
} grid_store_t;


/* constants */

/* #define  BABY_STEP  0.001 */ /* original value */
#define  BABY_STEP  0.001

#define  ZERO     0.0
#define  ONE      1.0
#define  TWO      2.0
#define  THREE    3.0
#define  HALF     0.5
#define  QUART    0.25
#define  BIGNUM   1.e+20
#define  TINY     1.e-14
#define  PI       M_PI
#define  PI2      TWO*PI
#define  PIH      HALF*PI


/* static double north_thresh =  1.45;  */ /* threshold for coord transformation */
/* static double south_thresh = -2.00;  */ /* threshold for coord transformation */
static double north_thresh =  2.00;  /* threshold for coord transformation */
static double south_thresh = -2.00;  /* threshold for coord transformation */

double intlin(double x, double y1, double x1, double y2, double x2);

extern int timer_remap, timer_remap_con, timer_remap_con_l1, timer_remap_con_l2;
extern int timer_remap_bil, timer_remap_nn;


void remapGridFree(remapgrid_t *rg)
{
  if ( rg->pinit == TRUE )
    {
      rg->pinit = FALSE;

      if ( rg->grid1_vgpm ) free(rg->grid1_vgpm);
      if ( rg->grid2_vgpm ) free(rg->grid2_vgpm);
      free(rg->grid1_mask);
      free(rg->grid2_mask);
      free(rg->grid1_center_lat);
      free(rg->grid1_center_lon);
      free(rg->grid2_center_lat);
      free(rg->grid2_center_lon);
      if ( rg->grid1_area ) free(rg->grid1_area);
      if ( rg->grid2_area ) free(rg->grid2_area);
      if ( rg->grid1_frac ) free(rg->grid1_frac);
      if ( rg->grid2_frac ) free(rg->grid2_frac);

      if ( rg->grid1_corner_lat ) free(rg->grid1_corner_lat);
      if ( rg->grid1_corner_lon ) free(rg->grid1_corner_lon);
      if ( rg->grid2_corner_lat ) free(rg->grid2_corner_lat);
      if ( rg->grid2_corner_lon ) free(rg->grid2_corner_lon);

      if ( rg->grid1_bound_box ) free(rg->grid1_bound_box);
      if ( rg->grid2_bound_box ) free(rg->grid2_bound_box);

      free(rg->bin_addr1);
      free(rg->bin_addr2);
      if ( rg->bin_lats ) free(rg->bin_lats);
      if ( rg->bin_lons ) free(rg->bin_lons);
    }
  else
    fprintf(stderr, "%s Warning: grid not initialized!\n", __func__);

} /* remapGridFree */

/*****************************************************************************/

void remapVarsFree(remapvars_t *rv)
{
  long i, num_blks;

  if ( rv->pinit == TRUE )
    {
      rv->pinit = FALSE;

      free(rv->grid1_add);
      free(rv->grid2_add);
      free(rv->wts);

      if ( rv->links.option == TRUE )
	{
	  rv->links.option = FALSE;

	  if ( rv->links.num_blks )
	    {
	      free(rv->links.num_links);
	      num_blks = rv->links.num_blks;
	      for ( i = 0; i < num_blks; ++i )
		{
		  free(rv->links.src_add[i]);
		  free(rv->links.dst_add[i]);
		  free(rv->links.w_index[i]);
		}
	      free(rv->links.src_add);
	      free(rv->links.dst_add);
	      free(rv->links.w_index);
	    }
	}
    }
  else
    fprintf(stderr, "%s Warning: vars not initialized!\n", __func__);

} /* remapVarsFree */

/*****************************************************************************/

void genXbounds(long xsize, long ysize, const double *restrict grid_center_lon, 
		double *restrict grid_corner_lon, double dlon)
{
  long i, j, index;
  double minlon, maxlon;

  if ( ! (dlon > 0) ) dlon = 360./xsize;
  /*
  if ( xsize == 1 || (grid_center_lon[xsize-1]-grid_center_lon[0]+dlon) < 359 )
    cdoAbort("Cannot calculate Xbounds for %d vals with dlon = %g", xsize, dlon);
  */
  for ( i = 0; i < xsize; ++i )
    {
      minlon = grid_center_lon[i] - HALF*dlon;
      maxlon = grid_center_lon[i] + HALF*dlon;
      for ( j = 0; j < ysize; ++j )
	{
	  index = (j<<2)*xsize + (i<<2);
	  grid_corner_lon[index  ] = minlon;
	  grid_corner_lon[index+1] = maxlon;
	  grid_corner_lon[index+2] = maxlon;
	  grid_corner_lon[index+3] = minlon;
	}
    }
}


double genYmin(double y1, double y2)
{
  double ymin, dy;

  dy = y2 - y1;
  ymin = y1 - dy/2;

  if ( y1 < -85 && ymin < -87.5 ) ymin = -90;

  if ( cdoVerbose )
    cdoPrint("genYmin: y1 = %g  y2 = %g  dy = %g  ymin = %g", y1, y2, dy, ymin);

  return (ymin);
}


double genYmax(double y1, double y2)
{
  double ymax, dy;

  dy = y1 - y2;
  ymax = y1 + dy/2;

  if ( y1 > 85 && ymax > 87.5 ) ymax = 90;

  if ( cdoVerbose )
    cdoPrint("genYmax: y1 = %g  y2 = %g  dy = %g  ymax = %g", y1, y2, dy, ymax);

  return (ymax);
}



/*****************************************************************************/

void genYbounds(long xsize, long ysize, const double *restrict grid_center_lat,
		double *restrict grid_corner_lat)
{
  long i, j, index;
  double minlat, maxlat;
  double firstlat, lastlat;

  firstlat = grid_center_lat[0];
  lastlat  = grid_center_lat[xsize*ysize-1];

  // if ( ysize == 1 ) cdoAbort("Cannot calculate Ybounds for 1 value!");

  for ( j = 0; j < ysize; ++j )
    {
      if ( ysize == 1 )
	{
	  minlat = grid_center_lat[0] - 360./ysize;
	  maxlat = grid_center_lat[0] + 360./ysize;
	}
      else
	{
	  index = j*xsize;
	  if ( firstlat > lastlat )
	    {
	      if ( j == 0 )
		maxlat = genYmax(grid_center_lat[index], grid_center_lat[index+xsize]);
	      else
		maxlat = 0.5*(grid_center_lat[index]+grid_center_lat[index-xsize]);

	      if ( j == (ysize-1) )
		minlat = genYmin(grid_center_lat[index], grid_center_lat[index-xsize]);
	      else
		minlat = 0.5*(grid_center_lat[index]+grid_center_lat[index+xsize]);
	    }
	  else
	    {
	      if ( j == 0 )
		minlat = genYmin(grid_center_lat[index], grid_center_lat[index+xsize]);
	      else
		minlat = 0.5*(grid_center_lat[index]+grid_center_lat[index-xsize]);

	      if ( j == (ysize-1) )
		maxlat = genYmax(grid_center_lat[index], grid_center_lat[index-xsize]);
	      else
		maxlat = 0.5*(grid_center_lat[index]+grid_center_lat[index+xsize]);
	    }
	}

      for ( i = 0; i < xsize; ++i )
	{
	  index = (j<<2)*xsize + (i<<2);
	  grid_corner_lat[index  ] = minlat;
	  grid_corner_lat[index+1] = minlat;
	  grid_corner_lat[index+2] = maxlat;
	  grid_corner_lat[index+3] = maxlat;
	}
    }
}

/*****************************************************************************/

void remapGridInitPointer(remapgrid_t *rg)
{
  rg->pinit = TRUE;

  rg->grid1_nvgp       = 0;
  rg->grid2_nvgp       = 0;
  
  rg->grid1_vgpm       = NULL;
  rg->grid2_vgpm       = NULL;

  rg->grid1_mask       = NULL;
  rg->grid2_mask       = NULL;
  rg->grid1_center_lat = NULL;
  rg->grid1_center_lon = NULL;
  rg->grid2_center_lat = NULL;
  rg->grid2_center_lon = NULL;
  rg->grid1_area       = NULL;
  rg->grid2_area       = NULL;
  rg->grid1_frac       = NULL;
  rg->grid2_frac       = NULL;

  rg->grid1_corner_lat = NULL;
  rg->grid1_corner_lon = NULL;
  rg->grid2_corner_lat = NULL;
  rg->grid2_corner_lon = NULL;

  rg->grid1_bound_box  = NULL;
  rg->grid2_bound_box  = NULL;

  rg->bin_addr1        = NULL;
  rg->bin_addr2        = NULL;
  rg->bin_lats         = NULL;
  rg->bin_lons         = NULL;
}

/*****************************************************************************/

void remapGridRealloc(int map_type, remapgrid_t *rg)
{
  long nalloc;

  if ( rg->grid1_nvgp )
    rg->grid1_vgpm     = (int *) realloc(rg->grid1_vgpm, rg->grid1_nvgp*sizeof(int));

  if ( rg->grid2_nvgp )
    rg->grid2_vgpm     = (int *) realloc(rg->grid2_vgpm, rg->grid2_nvgp*sizeof(int));

  rg->grid1_mask       = (int *) realloc(rg->grid1_mask, rg->grid1_size*sizeof(int));
  rg->grid2_mask       = (int *) realloc(rg->grid2_mask, rg->grid2_size*sizeof(int));
  rg->grid1_center_lat = (double *) realloc(rg->grid1_center_lat, rg->grid1_size*sizeof(double));
  rg->grid1_center_lon = (double *) realloc(rg->grid1_center_lon, rg->grid1_size*sizeof(double));
  rg->grid2_center_lat = (double *) realloc(rg->grid2_center_lat, rg->grid2_size*sizeof(double));
  rg->grid2_center_lon = (double *) realloc(rg->grid2_center_lon, rg->grid2_size*sizeof(double));

  if ( map_type == MAP_TYPE_CONSERV )
    {
      rg->grid1_area = (double *) realloc(rg->grid1_area, rg->grid1_size*sizeof(double));
      rg->grid2_area = (double *) realloc(rg->grid2_area, rg->grid2_size*sizeof(double));

      memset(rg->grid1_area, 0, rg->grid1_size*sizeof(double));
      memset(rg->grid2_area, 0, rg->grid2_size*sizeof(double));
    }

  rg->grid1_frac = (double *) realloc(rg->grid1_frac, rg->grid1_size*sizeof(double));
  rg->grid2_frac = (double *) realloc(rg->grid2_frac, rg->grid2_size*sizeof(double));

  memset(rg->grid1_frac, 0, rg->grid1_size*sizeof(double));
  memset(rg->grid2_frac, 0, rg->grid2_size*sizeof(double));

  if ( rg->lneed_grid1_corners )
    {
      if ( rg->grid1_corners == 0 )
	{
	  cdoAbort("grid1 corner missing!");
	}
      else
	{
	  nalloc = rg->grid1_corners*rg->grid1_size;
	  rg->grid1_corner_lat = (double *) realloc(rg->grid1_corner_lat, nalloc*sizeof(double));
	  rg->grid1_corner_lon = (double *) realloc(rg->grid1_corner_lon, nalloc*sizeof(double));
	  
	  memset(rg->grid1_corner_lat, 0, nalloc*sizeof(double));
	  memset(rg->grid1_corner_lon, 0, nalloc*sizeof(double));
	}
    }

  if ( rg->lneed_grid2_corners )
    {
      if ( rg->grid2_corners == 0 )
	{
	  cdoAbort("grid2 corner missing!");
	}
      else
	{
	  nalloc = rg->grid2_corners*rg->grid2_size;
	  rg->grid2_corner_lat = (double *) realloc(rg->grid2_corner_lat, nalloc*sizeof(double));
	  rg->grid2_corner_lon = (double *) realloc(rg->grid2_corner_lon, nalloc*sizeof(double));
  
	  memset(rg->grid2_corner_lat, 0, nalloc*sizeof(double));
	  memset(rg->grid2_corner_lon, 0, nalloc*sizeof(double));
	}
    }

  rg->grid1_bound_box = (restr_t *) realloc(rg->grid1_bound_box, 4*rg->grid1_size*sizeof(restr_t));
  if ( rg->luse_grid2_corners )
    rg->grid2_bound_box = (restr_t *) realloc(rg->grid2_bound_box, 4*rg->grid2_size*sizeof(restr_t));
}

/*****************************************************************************/
static
void boundbox_from_corners(long size, long nc, const double *restrict corner_lon,
			   const double *restrict corner_lat, restr_t *restrict bound_box)
{
  long i4, inc, i, j;
  restr_t clon, clat;

#if defined(_OPENMP)
#pragma omp parallel for default(none)        \
  shared(bound_box, corner_lat, corner_lon, nc, size)	\
  private(i4, inc, i, j, clon, clat)
#endif
  for ( i = 0; i < size; ++i )
    {
      i4 = i<<2; // *4
      inc = i*nc;
      clat = RESTR_SCALE(corner_lat[inc]);
      clon = RESTR_SCALE(corner_lon[inc]);
      bound_box[i4  ] = clat;
      bound_box[i4+1] = clat;
      bound_box[i4+2] = clon;
      bound_box[i4+3] = clon;
      for ( j = 1; j < nc; ++j )
	{
	  clat = RESTR_SCALE(corner_lat[inc+j]);
	  clon = RESTR_SCALE(corner_lon[inc+j]);
	  if ( clat < bound_box[i4  ] ) bound_box[i4  ] = clat;
	  if ( clat > bound_box[i4+1] ) bound_box[i4+1] = clat;
	  if ( clon < bound_box[i4+2] ) bound_box[i4+2] = clon;
	  if ( clon > bound_box[i4+3] ) bound_box[i4+3] = clon;
	}
    }
}

static
void boundbox_from_center(int lonIsCyclic, long size, long nx, long ny, const double *restrict center_lon,
			  const double *restrict center_lat, restr_t *restrict bound_box)
{
  long n4, i, j, k, n, ip1, jp1;
  long n_add, e_add, ne_add;
  restr_t tmp_lats[4], tmp_lons[4];  /* temps for computing bounding boxes */

#if defined(_OPENMP)
#pragma omp parallel for default(none)        \
  shared(lonIsCyclic, size, nx, ny, center_lon, center_lat, bound_box)	\
  private(n4, i, j, k, n, ip1, jp1, n_add, e_add, ne_add, tmp_lats, tmp_lons)
#endif
  for ( n = 0; n < size; n++ )
    {
      n4 = n<<2;

      /* Find N,S and NE points to this grid point */
      
      j = n/nx;
      i = n - j*nx;

      if ( i < (nx-1) )
	ip1 = i + 1;
      else
	{
	  /* 2009-01-09 Uwe Schulzweida: bug fix */
	  if ( lonIsCyclic )
	    ip1 = 0;
	  else
	    ip1 = i;
	}

      if ( j < (ny-1) )
	jp1 = j + 1;
      else
	{
	  /* 2008-12-17 Uwe Schulzweida: latitute cyclic ??? (bug fix) */
	  jp1 = j;
	}

      n_add  = jp1*nx + i;
      e_add  = j  *nx + ip1;
      ne_add = jp1*nx + ip1;

      /* Find N,S and NE lat/lon coords and check bounding box */

      tmp_lats[0] = RESTR_SCALE(center_lat[n]);
      tmp_lats[1] = RESTR_SCALE(center_lat[e_add]);
      tmp_lats[2] = RESTR_SCALE(center_lat[ne_add]);
      tmp_lats[3] = RESTR_SCALE(center_lat[n_add]);

      tmp_lons[0] = RESTR_SCALE(center_lon[n]);
      tmp_lons[1] = RESTR_SCALE(center_lon[e_add]);
      tmp_lons[2] = RESTR_SCALE(center_lon[ne_add]);
      tmp_lons[3] = RESTR_SCALE(center_lon[n_add]);

      bound_box[n4  ] = tmp_lats[0];
      bound_box[n4+1] = tmp_lats[0];
      bound_box[n4+2] = tmp_lons[0];
      bound_box[n4+3] = tmp_lons[0];

      for ( k = 1; k < 4; k++ )
	{
	  if ( tmp_lats[k] < bound_box[n4  ] ) bound_box[n4  ] = tmp_lats[k];
	  if ( tmp_lats[k] > bound_box[n4+1] ) bound_box[n4+1] = tmp_lats[k];
	  if ( tmp_lons[k] < bound_box[n4+2] ) bound_box[n4+2] = tmp_lons[k];
	  if ( tmp_lons[k] > bound_box[n4+3] ) bound_box[n4+3] = tmp_lons[k];
	}
    }
}


static
void check_lon_range(long nlons, double *lons)
{
  long n;

#if defined(_OPENMP)
#pragma omp parallel for default(none) shared(nlons, lons)
#endif
  for ( n = 0; n < nlons; ++n )
    {
      if ( lons[n] > PI2  ) lons[n] -= PI2;
      if ( lons[n] < ZERO ) lons[n] += PI2;
    }
}

static
void check_lat_range(long nlats, double *lats)
{
  long n;

#if defined(_OPENMP)
#pragma omp parallel for default(none) shared(nlats, lats)
#endif
  for ( n = 0; n < nlats; ++n )
    {
      if ( lats[n] >  PIH ) lats[n] =  PIH;
      if ( lats[n] < -PIH ) lats[n] = -PIH;
    }
}

static
void check_lon_boundbox_range(long nlons, restr_t *bound_box)
{
  long n, n4;

#if defined(_OPENMP)
#pragma omp parallel for default(none) shared(nlons, bound_box) private(n4)
#endif
  for ( n = 0; n < nlons; ++n )
    {
      n4 = n<<2;
      if ( RESTR_ABS(bound_box[n4+3] - bound_box[n4+2]) > RESTR_SCALE(PI) )
	{
	  bound_box[n4+2] = 0;
	  bound_box[n4+3] = RESTR_SCALE(PI2);
	}
    }
}

static
void check_lat_boundbox_range(long nlats, restr_t *restrict bound_box, double *restrict lats)
{
  long n, n4;

#if defined(_OPENMP)
#pragma omp parallel for default(none) shared(nlats, bound_box, lats) private(n4)
#endif
  for ( n = 0; n < nlats; ++n )
    {
      n4 = n<<2;
      if ( RESTR_SCALE(lats[n]) < bound_box[n4  ] ) bound_box[n4  ] = RESTR_SCALE(-PIH);
      if ( RESTR_SCALE(lats[n]) > bound_box[n4+1] ) bound_box[n4+1] = RESTR_SCALE( PIH);
    }
}

static
int expand_lonlat_grid(int gridID)
{
  char units[CDI_MAX_NAME];
  int gridIDnew;
  long nx, ny, nxp4, nyp4;
  double *xvals, *yvals;

  nx = gridInqXsize(gridID);
  ny = gridInqYsize(gridID);
  nxp4 = nx+4;
  nyp4 = ny+4;

  xvals = (double *) malloc(nxp4*sizeof(double));
  yvals = (double *) malloc(nyp4*sizeof(double));
  gridInqXvals(gridID, xvals+2);
  gridInqYvals(gridID, yvals+2);

  gridIDnew = gridCreate(GRID_LONLAT, nxp4*nyp4);
  gridDefXsize(gridIDnew, nxp4);
  gridDefYsize(gridIDnew, nyp4);
	      
  gridInqXunits(gridID,    units);
  gridDefXunits(gridIDnew, units);
  gridInqYunits(gridID,    units);
  gridDefYunits(gridIDnew, units);

  xvals[0] = xvals[2] - 2*gridInqXinc(gridID);
  xvals[1] = xvals[2] - gridInqXinc(gridID);
  xvals[nxp4-2] = xvals[nx+1] + gridInqXinc(gridID);
  xvals[nxp4-1] = xvals[nx+1] + 2*gridInqXinc(gridID);

  yvals[0] = yvals[2] - 2*gridInqYinc(gridID);
  yvals[1] = yvals[2] - gridInqYinc(gridID);
  yvals[nyp4-2] = yvals[ny+1] + gridInqYinc(gridID);
  yvals[nyp4-1] = yvals[ny+1] + 2*gridInqYinc(gridID);

  gridDefXvals(gridIDnew, xvals);
  gridDefYvals(gridIDnew, yvals);

  free(xvals);
  free(yvals);

  if ( gridIsRotated(gridID) )
    {
      gridDefXpole(gridIDnew, gridInqXpole(gridID));
      gridDefYpole(gridIDnew, gridInqYpole(gridID));
    }

  return(gridIDnew);
}

static
int expand_curvilinear_grid(int gridID)
{
  char units[CDI_MAX_NAME];
  int gridIDnew;
  long gridsize, gridsize_new;
  long nx, ny, nxp4, nyp4;
  long i, j;
  double *xvals, *yvals;

  gridsize = gridInqSize(gridID);
  nx = gridInqXsize(gridID);
  ny = gridInqYsize(gridID);
  nxp4 = nx+4;
  nyp4 = ny+4;
  gridsize_new = gridsize + 4*(nx+2) + 4*(ny+2);

  xvals = (double *) malloc(gridsize_new*sizeof(double));
  yvals = (double *) malloc(gridsize_new*sizeof(double));
  gridInqXvals(gridID, xvals);
  gridInqYvals(gridID, yvals);

  gridIDnew = gridCreate(GRID_CURVILINEAR, nxp4*nyp4);
  gridDefXsize(gridIDnew, nxp4);
  gridDefYsize(gridIDnew, nyp4);

  gridInqXunits(gridID,   units);
  gridDefXunits(gridIDnew, units);
  gridInqYunits(gridID,   units);
  gridDefYunits(gridIDnew, units);

  for ( j = ny-1; j >= 0; j-- )
    for ( i = nx-1; i >= 0; i-- )
      xvals[(j+2)*(nx+4)+i+2] = xvals[j*nx+i];

  for ( j = ny-1; j >= 0; j-- )
    for ( i = nx-1; i >= 0; i-- )
      yvals[(j+2)*(nx+4)+i+2] = yvals[j*nx+i];

  for ( j = 2; j < nyp4-2; j++ )
    {
      xvals[j*nxp4  ] = intlin(3.0, xvals[j*nxp4+3], 0.0, xvals[j*nxp4+2], 1.0);
      xvals[j*nxp4+1] = intlin(2.0, xvals[j*nxp4+3], 0.0, xvals[j*nxp4+2], 1.0); 
      yvals[j*nxp4  ] = intlin(3.0, yvals[j*nxp4+3], 0.0, yvals[j*nxp4+2], 1.0); 
      yvals[j*nxp4+1] = intlin(2.0, yvals[j*nxp4+3], 0.0, yvals[j*nxp4+2], 1.0); 

      xvals[j*nxp4+nxp4-2] = intlin(2.0, xvals[j*nxp4+nxp4-4], 0.0, xvals[j*nxp4+nxp4-3], 1.0); 
      xvals[j*nxp4+nxp4-1] = intlin(3.0, xvals[j*nxp4+nxp4-4], 0.0, xvals[j*nxp4+nxp4-3], 1.0); 
      yvals[j*nxp4+nxp4-2] = intlin(2.0, yvals[j*nxp4+nxp4-4], 0.0, yvals[j*nxp4+nxp4-3], 1.0); 
      yvals[j*nxp4+nxp4-1] = intlin(3.0, yvals[j*nxp4+nxp4-4], 0.0, yvals[j*nxp4+nxp4-3], 1.0); 
    }

  for ( i = 0; i < nxp4; i++ )
    {
      xvals[0*nxp4+i] = intlin(3.0, xvals[3*nxp4+i], 0.0, xvals[2*nxp4+i], 1.0);
      xvals[1*nxp4+i] = intlin(2.0, xvals[3*nxp4+i], 0.0, xvals[2*nxp4+i], 1.0);
      yvals[0*nxp4+i] = intlin(3.0, yvals[3*nxp4+i], 0.0, yvals[2*nxp4+i], 1.0);
      yvals[1*nxp4+i] = intlin(2.0, yvals[3*nxp4+i], 0.0, yvals[2*nxp4+i], 1.0);

      xvals[(nyp4-2)*nxp4+i] = intlin(2.0, xvals[(nyp4-4)*nxp4+i], 0.0, xvals[(nyp4-3)*nxp4+i], 1.0);
      xvals[(nyp4-1)*nxp4+i] = intlin(3.0, xvals[(nyp4-4)*nxp4+i], 0.0, xvals[(nyp4-3)*nxp4+i], 1.0);
      yvals[(nyp4-2)*nxp4+i] = intlin(2.0, yvals[(nyp4-4)*nxp4+i], 0.0, yvals[(nyp4-3)*nxp4+i], 1.0);
      yvals[(nyp4-1)*nxp4+i] = intlin(3.0, yvals[(nyp4-4)*nxp4+i], 0.0, yvals[(nyp4-3)*nxp4+i], 1.0);
    }
  /*
    {
    FILE *fp;
    fp = fopen("xvals.asc", "w");
    for ( i = 0; i < gridsize_new; i++ ) fprintf(fp, "%g\n", xvals[i]);
    fclose(fp);
    fp = fopen("yvals.asc", "w");
    for ( i = 0; i < gridsize_new; i++ ) fprintf(fp, "%g\n", yvals[i]);
    fclose(fp);
    }
  */
  gridDefXvals(gridIDnew, xvals);
  gridDefYvals(gridIDnew, yvals);
  
  free(xvals);
  free(yvals);

  return(gridIDnew);
}

static
void calc_lat_bins(remapgrid_t *rg, int map_type)
{
  long nbins;
  long n;      /* Loop counter                  */
  long nele;   /* Element loop counter          */
  long n2, nele4;
  double dlat;                /* lat/lon intervals for search bins  */
  long grid1_size, grid2_size;

  grid1_size = rg->grid1_size;
  grid2_size = rg->grid2_size;

  nbins = rg->num_srch_bins;
  dlat = PI/nbins;

  if ( cdoVerbose )
    cdoPrint("Using %d latitude bins to restrict search.", nbins);

  if ( nbins > 0 )
    {
      rg->bin_lats  = (restr_t *) realloc(rg->bin_lats, 2*nbins*sizeof(restr_t));
      rg->bin_lons  = (restr_t *) realloc(rg->bin_lons, 2*nbins*sizeof(restr_t));
    }

  for ( n = 0; n < nbins; ++n )
    {
      n2 = n<<1;
      rg->bin_lats[n2  ] = RESTR_SCALE((n  )*dlat - PIH);
      rg->bin_lats[n2+1] = RESTR_SCALE((n+1)*dlat - PIH);
      rg->bin_lons[n2  ] = 0;
      rg->bin_lons[n2+1] = RESTR_SCALE(PI2);
    }

  if ( nbins > 0 ) rg->bin_addr1 = (int *) realloc(rg->bin_addr1, 2*nbins*sizeof(int));
  for ( n = 0; n < nbins; ++n )
    {
      n2 = n<<1;
      rg->bin_addr1[n2  ] = grid1_size;
      rg->bin_addr1[n2+1] = 0;
    }

#if defined(_OPENMP)
#pragma omp parallel for default(none) \
  private(n, n2, nele4)		       \
  shared(grid1_size, nbins, rg)
#endif
  for ( nele = 0; nele < grid1_size; ++nele )
    {
      nele4 = nele<<2;
      for ( n = 0; n < nbins; ++n )
	{
	  n2 = n<<1;
	  if ( rg->grid1_bound_box[nele4  ] <= rg->bin_lats[n2+1] &&
	       rg->grid1_bound_box[nele4+1] >= rg->bin_lats[n2  ] )
	    {
#if defined(_OPENMP)
#pragma omp critical
#endif
	      {
		rg->bin_addr1[n2  ] = MIN(nele, rg->bin_addr1[n2  ]);
		rg->bin_addr1[n2+1] = MAX(nele, rg->bin_addr1[n2+1]);
	      }
	    }
	}
    }

  if ( map_type == MAP_TYPE_CONSERV )
    {
      if ( nbins > 0 ) rg->bin_addr2 = (int *) realloc(rg->bin_addr2, 2*nbins*sizeof(int));
      for ( n = 0; n < nbins; ++n )
	{
	  n2 = n<<1;
	  rg->bin_addr2[n2  ] = grid2_size;
	  rg->bin_addr2[n2+1] = 0;
	}

#if defined(_OPENMP)
#pragma omp parallel for default(none) \
  private(n, n2, nele4)		       \
  shared(grid2_size, nbins, rg)
#endif
      for ( nele = 0; nele < grid2_size; ++nele )
	{
	  nele4 = nele<<2;
	  for ( n = 0; n < nbins; n++ )
	    {
	      n2 = n<<1;
	      if ( rg->grid2_bound_box[nele4  ] <= rg->bin_lats[n2+1] &&
		   rg->grid2_bound_box[nele4+1] >= rg->bin_lats[n2  ] )
		{
#if defined(_OPENMP)
#pragma omp critical
#endif
		  {
		    rg->bin_addr2[n2  ] = MIN(nele, rg->bin_addr2[n2  ]);
		    rg->bin_addr2[n2+1] = MAX(nele, rg->bin_addr2[n2+1]);
		  }
		}
	    }
	}

      free(rg->bin_lats); rg->bin_lats = NULL;
      free(rg->bin_lons); rg->bin_lons = NULL;
    }

  if ( map_type == MAP_TYPE_DISTWGT || map_type == MAP_TYPE_DISTWGT1 )
    {
      free(rg->grid1_bound_box); rg->grid1_bound_box = NULL;
    }
}

static
void calc_lonlat_bins(remapgrid_t *rg, int map_type)
{
  long i, j;   /* Logical 2d addresses          */
  long nbins;
  long n;      /* Loop counter                  */
  long nele;   /* Element loop counter          */
  long n2, nele4;
  double dlat, dlon;                /* lat/lon intervals for search bins  */
  long grid1_size, grid2_size;

  grid1_size = rg->grid1_size;
  grid2_size = rg->grid2_size;

  nbins = rg->num_srch_bins;

  dlat = PI /nbins;
  dlon = PI2/nbins;

  if ( cdoVerbose )
    cdoPrint("Using %d lat/lon boxes to restrict search.", nbins);

  rg->bin_addr1 = (int *) realloc(rg->bin_addr1, 2*nbins*nbins*sizeof(int));
  if ( map_type == MAP_TYPE_CONSERV )
    rg->bin_addr2 = (int *) realloc(rg->bin_addr2, 2*nbins*nbins*sizeof(int));
  rg->bin_lats  = (restr_t *) realloc(rg->bin_lats, 2*nbins*nbins*sizeof(restr_t));
  rg->bin_lons  = (restr_t *) realloc(rg->bin_lons, 2*nbins*nbins*sizeof(restr_t));

  n = 0;
  for ( j = 0; j < nbins; j++ )
    for ( i = 0; i < nbins; i++ )
      {
	n2 = n<<1;
	rg->bin_lats[n2  ]  = RESTR_SCALE((j  )*dlat - PIH);
	rg->bin_lats[n2+1]  = RESTR_SCALE((j+1)*dlat - PIH);
	rg->bin_lons[n2  ]  = RESTR_SCALE((i  )*dlon);
	rg->bin_lons[n2+1]  = RESTR_SCALE((i+1)*dlon);
	rg->bin_addr1[n2  ] = grid1_size;
	rg->bin_addr1[n2+1] = 0;
	if ( map_type == MAP_TYPE_CONSERV )
	  {
	    rg->bin_addr2[n2  ] = grid2_size;
	    rg->bin_addr2[n2+1] = 0;
	  }

	n++;
      }

  rg->num_srch_bins = nbins*nbins;

  for ( nele = 0; nele < grid1_size; nele++ )
    {
      nele4 = nele<<2;
      for ( n = 0; n < nbins*nbins; n++ )
	if ( rg->grid1_bound_box[nele4  ] <= rg->bin_lats[2*n+1] &&
	     rg->grid1_bound_box[nele4+1] >= rg->bin_lats[2*n  ] &&
	     rg->grid1_bound_box[nele4+2] <= rg->bin_lons[2*n+1] &&
	     rg->grid1_bound_box[nele4+3] >= rg->bin_lons[2*n  ] )
	  {
	    rg->bin_addr1[2*n  ] = MIN(nele,rg->bin_addr1[2*n  ]);
	    rg->bin_addr1[2*n+1] = MAX(nele,rg->bin_addr1[2*n+1]);
	  }
    }
  
  if ( map_type == MAP_TYPE_CONSERV )
    {
      for ( nele = 0; nele < grid2_size; nele++ )
	{
	  nele4 = nele<<2;
	  for ( n = 0; n < nbins*nbins; n++ )
	    if ( rg->grid2_bound_box[nele4  ] <= rg->bin_lats[2*n+1] &&
		 rg->grid2_bound_box[nele4+1] >= rg->bin_lats[2*n  ] &&
		 rg->grid2_bound_box[nele4+2] <= rg->bin_lons[2*n+1] &&
		 rg->grid2_bound_box[nele4+3] >= rg->bin_lons[2*n  ] )
	      {
		rg->bin_addr2[2*n  ] = MIN(nele,rg->bin_addr2[2*n  ]);
		rg->bin_addr2[2*n+1] = MAX(nele,rg->bin_addr2[2*n+1]);
	      }
	}
  
      free(rg->bin_lats); rg->bin_lats = NULL;
      free(rg->bin_lons); rg->bin_lons = NULL;
    }

  if ( map_type == MAP_TYPE_DISTWGT || map_type == MAP_TYPE_DISTWGT1 )
    { 
      free(rg->grid1_bound_box); rg->grid1_bound_box = NULL;
    }
}

/*****************************************************************************/

void remapGridInit(int map_type, int lextrapolate, int gridID1, int gridID2, remapgrid_t *rg)
{
  char units[CDI_MAX_NAME];
  long i, i4;
  long nx, ny;
  long grid1_size, grid2_size;
  int lgrid1_destroy = FALSE, lgrid2_destroy = FALSE;
  int lgrid1_gen_bounds = FALSE, lgrid2_gen_bounds = FALSE;
  int gridID1_gme = -1;
  int gridID2_gme = -1;

  rg->store_link_fast = FALSE;

  north_thresh =  rg->threshhold;
  south_thresh = -rg->threshhold;

  if ( cdoVerbose ) cdoPrint("threshhold: north=%g  south=%g", north_thresh, south_thresh);

  if ( lextrapolate > 0 )
    rg->lextrapolate = TRUE;
  else
    rg->lextrapolate = FALSE;

  if ( map_type == MAP_TYPE_CONSERV )
    {
      rg->luse_grid1_corners  = TRUE;
      rg->luse_grid2_corners  = TRUE;
      rg->lneed_grid1_corners = TRUE;
      rg->lneed_grid2_corners = TRUE;
    }
  else
    {
      rg->luse_grid1_corners  = FALSE;
      rg->luse_grid2_corners  = FALSE;
      rg->lneed_grid1_corners = FALSE;
      rg->lneed_grid2_corners = FALSE;
    }

  /* Initialize all pointer */
  if ( rg->pinit == FALSE ) remapGridInitPointer(rg);

  rg->gridID1 = gridID1;
  rg->gridID2 = gridID2;

  if ( !rg->lextrapolate && gridInqSize(rg->gridID1) > 1 &&
       (map_type == MAP_TYPE_DISTWGT || map_type == MAP_TYPE_DISTWGT1) &&
       ((gridInqType(gridID1) == GRID_LONLAT && gridIsRotated(gridID1)) ||
	(gridInqType(gridID1) == GRID_LONLAT && rg->non_global)) )
    {
      rg->gridID1 = gridID1 = expand_lonlat_grid(gridID1);
    }

  if ( gridInqType(gridID1) == GRID_UNSTRUCTURED )
    {
      if ( gridInqYvals(gridID1, NULL) == 0 || gridInqXvals(gridID1, NULL) == 0 )
	{
	  if ( gridInqNumber(gridID1) > 0 )
	    {
	      rg->gridID1 = gridID1 = referenceToGrid(gridID1);
	      if ( gridID1 == -1 ) cdoAbort("Reference to source grid not found!");
	    }
	}
    }

  if ( gridInqType(gridID2) == GRID_UNSTRUCTURED )
    {
      if ( gridInqYvals(gridID2, NULL) == 0 || gridInqXvals(gridID2, NULL) == 0 )
	{
	  if ( gridInqNumber(gridID2) > 0 )
	    {
	      rg->gridID2 = gridID2 = referenceToGrid(gridID2);
	      if ( gridID2 == -1 ) cdoAbort("Reference to target grid not found!");
	    }
	}
    }

  if ( gridInqSize(rg->gridID1) > 1 && 
       (gridInqType(rg->gridID1) == GRID_LCC || 
	gridInqType(rg->gridID1) == GRID_LAEA || 
	gridInqType(rg->gridID1) == GRID_SINUSOIDAL) )
    {
      rg->gridID1 = gridID1 = gridToCurvilinear(rg->gridID1, 1);
    }

  if ( !rg->lextrapolate && gridInqSize(rg->gridID1) > 1 &&
       (map_type == MAP_TYPE_DISTWGT || map_type == MAP_TYPE_DISTWGT1) &&
       (gridInqType(gridID1) == GRID_CURVILINEAR && rg->non_global) )
    {
      rg->gridID1 = gridID1 = expand_curvilinear_grid(gridID1);
    }

  if ( map_type == MAP_TYPE_DISTWGT || map_type == MAP_TYPE_DISTWGT1 )
    {
      if ( gridInqType(rg->gridID1) == GRID_UNSTRUCTURED )
	{
	  rg->luse_grid1_corners  = TRUE;
	  rg->lneed_grid1_corners = FALSE; /* full grid search */
	}
      if ( gridInqType(rg->gridID2) == GRID_UNSTRUCTURED )
	{
	  rg->luse_grid2_corners  = TRUE;
	  rg->lneed_grid2_corners = FALSE; /* full grid search */
	}
    }

  if ( gridInqType(rg->gridID1) != GRID_UNSTRUCTURED && gridInqType(rg->gridID1) != GRID_CURVILINEAR )
    {
      if ( gridInqType(rg->gridID1) == GRID_GME )
	{
	  gridID1_gme = gridToUnstructured(rg->gridID1, 1);
	  rg->grid1_nvgp = gridInqSize(gridID1_gme);
	  gridID1 = gridDuplicate(gridID1_gme);
	  gridCompress(gridID1);
	  rg->luse_grid1_corners = TRUE;
	}
      else
	{
	  lgrid1_destroy = TRUE;
	  gridID1 = gridToCurvilinear(rg->gridID1, 1);
	  lgrid1_gen_bounds = TRUE;
	}
    }

  if ( gridInqType(rg->gridID2) != GRID_UNSTRUCTURED && gridInqType(rg->gridID2) != GRID_CURVILINEAR )
    {
      if ( gridInqType(rg->gridID2) == GRID_GME )
	{
	  gridID2_gme = gridToUnstructured(rg->gridID2, 1);
	  rg->grid2_nvgp = gridInqSize(gridID2_gme);
	  gridID2 = gridDuplicate(gridID2_gme);
	  gridCompress(gridID2);
	  rg->luse_grid2_corners = TRUE;
	}
      else
	{
	  lgrid2_destroy = TRUE;
	  gridID2 = gridToCurvilinear(rg->gridID2, 1);
	  lgrid2_gen_bounds = TRUE;
	}
    }

  grid1_size = rg->grid1_size = gridInqSize(gridID1);
  grid2_size = rg->grid2_size = gridInqSize(gridID2);

  rg->grid1_is_cyclic = gridIsCircular(gridID1);
  rg->grid2_is_cyclic = gridIsCircular(gridID2);

  if ( gridInqType(gridID1) == GRID_UNSTRUCTURED )
    rg->grid1_rank = 1;
  else
    rg->grid1_rank = 2;

  if ( gridInqType(gridID2) == GRID_UNSTRUCTURED )
    rg->grid2_rank = 1;
  else
    rg->grid2_rank = 2;

  if ( gridInqType(gridID1) == GRID_UNSTRUCTURED )
    rg->grid1_corners = gridInqNvertex(gridID1);
  else
    rg->grid1_corners = 4;

  if ( gridInqType(gridID2) == GRID_UNSTRUCTURED )
    rg->grid2_corners = gridInqNvertex(gridID2);
  else
    rg->grid2_corners = 4;

  remapGridRealloc(map_type, rg);

  rg->grid1_dims[0] = gridInqXsize(gridID1);
  rg->grid1_dims[1] = gridInqYsize(gridID1);
 
  gridInqXvals(gridID1, rg->grid1_center_lon);
  gridInqYvals(gridID1, rg->grid1_center_lat);

  if ( rg->lneed_grid1_corners )
    {
      if ( gridInqYbounds(gridID1, NULL) && gridInqXbounds(gridID1, NULL) )
	{
	  gridInqXbounds(gridID1, rg->grid1_corner_lon);
	  gridInqYbounds(gridID1, rg->grid1_corner_lat);
	}
      else if ( lgrid1_gen_bounds )
	{
	  genXbounds(rg->grid1_dims[0], rg->grid1_dims[1], rg->grid1_center_lon, rg->grid1_corner_lon, 0);
	  genYbounds(rg->grid1_dims[0], rg->grid1_dims[1], rg->grid1_center_lat, rg->grid1_corner_lat);
	}
      else
	{
	  cdoAbort("grid1 corner missing!");
	}
    }

  /* Initialize logical mask */

#if defined(_OPENMP)
#pragma omp parallel for default(none) shared(grid1_size, rg)
#endif
  for ( i = 0; i < grid1_size; ++i ) rg->grid1_mask[i] = TRUE;

  if ( gridInqType(rg->gridID1) == GRID_GME ) gridInqMaskGME(gridID1_gme, rg->grid1_vgpm);

  /* Convert lat/lon units if required */

  gridInqYunits(gridID1, units);

  grid_to_radian(units, rg->grid1_size, rg->grid1_center_lon, "grid1 center lon"); 
  grid_to_radian(units, rg->grid1_size, rg->grid1_center_lat, "grid1 center lat"); 
  /* Note: using units from latitude instead from bounds */
  if ( rg->grid1_corners && rg->lneed_grid1_corners )
    {
      grid_to_radian(units, rg->grid1_corners*rg->grid1_size, rg->grid1_corner_lon, "grid1 corner lon"); 
      grid_to_radian(units, rg->grid1_corners*rg->grid1_size, rg->grid1_corner_lat, "grid1 corner lat"); 
    }

  if ( lgrid1_destroy ) gridDestroy(gridID1);

  /* Data for grid 2 */

  rg->grid2_dims[0] = gridInqXsize(gridID2);
  rg->grid2_dims[1] = gridInqYsize(gridID2);

  gridInqXvals(gridID2, rg->grid2_center_lon);
  gridInqYvals(gridID2, rg->grid2_center_lat);

  if ( rg->lneed_grid2_corners )
    {
      if ( gridInqYbounds(gridID2, NULL) && gridInqXbounds(gridID2, NULL) )
	{
	  gridInqXbounds(gridID2, rg->grid2_corner_lon);
	  gridInqYbounds(gridID2, rg->grid2_corner_lat);
	}
      else if ( lgrid2_gen_bounds )
	{
	  genXbounds(rg->grid2_dims[0], rg->grid2_dims[1], rg->grid2_center_lon, rg->grid2_corner_lon, 0);
	  genYbounds(rg->grid2_dims[0], rg->grid2_dims[1], rg->grid2_center_lat, rg->grid2_corner_lat);
	}
      else
	{
	  cdoAbort("grid2 corner missing!");
	}
    }

  /* Initialize logical mask */

  if ( gridInqMask(rg->gridID2, NULL) )
    {
      gridInqMask(rg->gridID2, rg->grid2_mask);
      for ( i = 0; i < grid2_size; ++i )
	{
	  if ( rg->grid2_mask[i] > 0 && rg->grid2_mask[i] < 255 )
	    rg->grid2_mask[i] = TRUE;
	  else
	    rg->grid2_mask[i] = FALSE;
	}
    }
  else
    {
#if defined(_OPENMP)
#pragma omp parallel for default(none) shared(grid2_size, rg)
#endif
      for ( i = 0; i < grid2_size; ++i ) rg->grid2_mask[i] = TRUE;
    }

  if ( gridInqType(rg->gridID2) == GRID_GME ) gridInqMaskGME(gridID2_gme, rg->grid2_vgpm);

  /* Convert lat/lon units if required */

  gridInqYunits(gridID2, units);

  grid_to_radian(units, rg->grid2_size, rg->grid2_center_lon, "grid2 center lon"); 
  grid_to_radian(units, rg->grid2_size, rg->grid2_center_lat, "grid2 center lat"); 
  /* Note: using units from latitude instead from bounds */
  if ( rg->grid2_corners && rg->lneed_grid2_corners )
    {
      grid_to_radian(units, rg->grid2_corners*rg->grid2_size, rg->grid2_corner_lon, "grid2 corner lon"); 
      grid_to_radian(units, rg->grid2_corners*rg->grid2_size, rg->grid2_corner_lat, "grid2 corner lat"); 
    }

  if ( lgrid2_destroy ) gridDestroy(gridID2);

  /* Convert longitudes to 0,2pi interval */

  check_lon_range(rg->grid1_size, rg->grid1_center_lon);
  check_lon_range(rg->grid2_size, rg->grid2_center_lon);

  if ( rg->grid1_corners && rg->lneed_grid1_corners )
    check_lon_range(rg->grid1_corners*rg->grid1_size, rg->grid1_corner_lon);

  if ( rg->grid2_corners && rg->lneed_grid2_corners )
    check_lon_range(rg->grid2_corners*rg->grid2_size, rg->grid2_corner_lon);


  /*  Make sure input latitude range is within the machine values for +/- pi/2 */

  check_lat_range(rg->grid1_size, rg->grid1_center_lat);
  check_lat_range(rg->grid2_size, rg->grid2_center_lat);

  if ( rg->grid1_corners && rg->lneed_grid1_corners )
    check_lat_range(rg->grid1_corners*rg->grid1_size, rg->grid1_corner_lat);
  
  if ( rg->grid2_corners && rg->lneed_grid2_corners )
    check_lat_range(rg->grid2_corners*rg->grid2_size, rg->grid2_corner_lat);


  /*  Compute bounding boxes for restricting future grid searches */

  if ( rg->luse_grid1_corners )
    {
      if ( rg->lneed_grid1_corners )
	{
	  if ( cdoVerbose ) cdoPrint("Grid1: boundbox_from_corners");

	  boundbox_from_corners(rg->grid1_size, rg->grid1_corners, 
				rg->grid1_corner_lon, rg->grid1_corner_lat, rg->grid1_bound_box);
	}
      else /* full grid search */
	{
	  if ( cdoVerbose ) cdoPrint("Grid1: bounds missing -> full grid search!");

	  for ( i = 0; i < grid1_size; ++i )
	    {
	      i4 = i<<2;
	      rg->grid1_bound_box[i4  ] = RESTR_SCALE(-PIH);
	      rg->grid1_bound_box[i4+1] = RESTR_SCALE( PIH);
	      rg->grid1_bound_box[i4+2] = 0;
	      rg->grid1_bound_box[i4+3] = RESTR_SCALE(PI2);
	    }
	}
    }
  else
    {
      if ( rg->grid1_rank != 2 ) cdoAbort("Internal problem, grid1 rank = %d!", rg->grid1_rank);

      nx = rg->grid1_dims[0];
      ny = rg->grid1_dims[1];

      if ( cdoVerbose ) cdoPrint("Grid1: boundbox_from_center");

      boundbox_from_center(rg->grid1_is_cyclic, rg->grid1_size, nx, ny, 
			   rg->grid1_center_lon, rg->grid1_center_lat, rg->grid1_bound_box);
    }


  if ( rg->luse_grid2_corners )
    {
      if ( rg->lneed_grid2_corners )
	{
	  if ( cdoVerbose ) cdoPrint("Grid2: boundbox_from_corners");

	  boundbox_from_corners(rg->grid2_size, rg->grid2_corners, 
				rg->grid2_corner_lon, rg->grid2_corner_lat, rg->grid2_bound_box);
	}
      else /* full grid search */
	{
	  if ( cdoVerbose ) cdoPrint("Grid2: bounds missing -> full grid search!");

	  for ( i = 0; i < grid2_size; ++i )
	    {
	      i4 = i<<2;
	      rg->grid2_bound_box[i4  ] = RESTR_SCALE(-PIH);
	      rg->grid2_bound_box[i4+1] = RESTR_SCALE( PIH);
	      rg->grid2_bound_box[i4+2] = 0;
	      rg->grid2_bound_box[i4+3] = RESTR_SCALE(PI2);
	    }
	}
    }

  check_lon_boundbox_range(rg->grid1_size, rg->grid1_bound_box);
  if ( rg->lneed_grid2_corners )
    check_lon_boundbox_range(rg->grid2_size, rg->grid2_bound_box);


  /* Try to check for cells that overlap poles */

  check_lat_boundbox_range(rg->grid1_size, rg->grid1_bound_box, rg->grid1_center_lat);
  if ( rg->lneed_grid2_corners )
    check_lat_boundbox_range(rg->grid2_size, rg->grid2_bound_box, rg->grid2_center_lat);


  /*
    Set up and assign address ranges to search bins in order to 
    further restrict later searches
  */
  if ( rg->restrict_type == RESTRICT_LATITUDE || rg->restrict_type == 0 )
    {
      calc_lat_bins(rg, map_type);
    }
  else if ( rg->restrict_type == RESTRICT_LATLON )
    {
      calc_lonlat_bins(rg, map_type);
    }
  else
    cdoAbort("Unknown search restriction method!");

}  /* remapGridInit */

/*****************************************************************************/

/*
    This routine initializes some variables and provides an initial
    allocation of arrays (fairly large so frequent resizing unnecessary).
*/
void remapVarsInit(int map_type, remapgrid_t *rg, remapvars_t *rv)
{
  /* Initialize all pointer */
  if ( rv->pinit == FALSE )
    {
      rv->pinit = TRUE;

      rv->grid1_add = NULL;
      rv->grid2_add = NULL;
      rv->wts       = NULL;
    }

  /* Determine the number of weights */

  if      ( map_type == MAP_TYPE_CONSERV  ) rv->num_wts = 3;
  else if ( map_type == MAP_TYPE_BILINEAR ) rv->num_wts = 1;
  else if ( map_type == MAP_TYPE_BICUBIC  ) rv->num_wts = 4;
  else if ( map_type == MAP_TYPE_DISTWGT  ) rv->num_wts = 1;
  else if ( map_type == MAP_TYPE_DISTWGT1 ) rv->num_wts = 1;
  else cdoAbort("Unknown mapping method!");

  /*
    Initialize num_links and set max_links to four times the largest 
    of the destination grid sizes initially (can be changed later).
    Set a default resize increment to increase the size of link
    arrays if the number of links exceeds the initial size
  */
  rv->num_links = 0;
  rv->max_links = 4 * rg->grid2_size;

  rv->resize_increment = (int) (0.1 * MAX(rg->grid1_size, rg->grid2_size));

  /*  Allocate address and weight arrays for mapping 1 */

  rv->grid1_add = (int *) realloc(rv->grid1_add, rv->max_links*sizeof(int));
  rv->grid2_add = (int *) realloc(rv->grid2_add, rv->max_links*sizeof(int));

  rv->wts = (double *) realloc(rv->wts, rv->num_wts*rv->max_links*sizeof(double));

  rv->links.option    = FALSE;
  rv->links.max_links = 0;
  rv->links.num_blks  = 0;
  rv->links.num_links = NULL;
  rv->links.src_add   = NULL;
  rv->links.dst_add   = NULL;
  rv->links.w_index   = NULL;

} /* remapVarsInit */

/*****************************************************************************/

/*
   This routine resizes remapping arrays by increasing(decreasing) the max_links by increment
*/
void resize_remap_vars(remapvars_t *rv, int increment)
{
  /*
    Input variables:
    int  increment  ! the number of links to add(subtract) to arrays
  */

  /*  Reallocate arrays at new size */

  rv->max_links += increment;

  if ( rv->max_links )
    {
      rv->grid1_add = (int *) realloc(rv->grid1_add, rv->max_links*sizeof(int));
      rv->grid2_add = (int *) realloc(rv->grid2_add, rv->max_links*sizeof(int));

      rv->wts = (double *) realloc(rv->wts, rv->num_wts*rv->max_links*sizeof(double));
    }

} /* resize_remap_vars */


/*
  -----------------------------------------------------------------------

  Performs the remapping based on weights computed elsewhere
     
  -----------------------------------------------------------------------
*/
void remap(double *restrict dst_array, double missval, long dst_size, long num_links, double *restrict map_wts, 
	   long num_wts, const int *restrict dst_add, const int *restrict src_add, const double *restrict src_array, 
	   const double *restrict src_grad1, const double *restrict src_grad2, const double *restrict src_grad3,
	   remaplink_t links)
{
  /*
    Input arrays:

    int *dst_add         ! destination address for each link
    int *src_add         ! source      address for each link

    int num_wts          ! num of weights used in remapping

    double *map_wts      ! remapping weights for each link

    double *src_array    ! array with source field to be remapped

    optional:

    double *src_grad1    ! gradient arrays on source grid necessary for
    double *src_grad2    ! higher-order remappings
    double *src_grad3

    output variables:

    double *dst_array    ! array for remapped field on destination grid
  */

  /* Local variables */
  long n;
  int iorder;

  /* Check the order of the interpolation */

  if ( src_grad1 )
    iorder = 2;
  else
    iorder = 1;

  for ( n = 0; n < dst_size; ++n ) dst_array[n] = missval;

  if ( cdoTimer ) timer_start(timer_remap);

#ifdef SX
#pragma cdir nodep
#endif
  for ( n = 0; n < num_links; ++n ) dst_array[dst_add[n]] = ZERO;

  if ( iorder == 1 )   /* First order remapping */
    {
      if ( links.option == TRUE )
	{
	  long j;
	  for ( j = 0; j < links.num_blks; ++j )
	    {
#ifdef SX
#pragma cdir nodep
#endif
	      for ( n = 0; n < links.num_links[j]; ++n )
		{
		  dst_array[links.dst_add[j][n]] += src_array[links.src_add[j][n]]*map_wts[num_wts*links.w_index[j][n]];
		}
	    }
	}
      else
	{
	  for ( n = 0; n < num_links; ++n )
	    {
	      /*
		printf("%5d %5d %5d %g # dst_add src_add n\n", dst_add[n], src_add[n], n, map_wts[num_wts*n]);
	      */
	      dst_array[dst_add[n]] += src_array[src_add[n]]*map_wts[num_wts*n];
	    }
	}
    }
  else                 /* Second order remapping */
    {
      if ( num_wts == 3 )
	{
	  for ( n = 0; n < num_links; ++n )
	    {
	      dst_array[dst_add[n]] += src_array[src_add[n]]*map_wts[num_wts*n] +
                                       src_grad1[src_add[n]]*map_wts[num_wts*n+1] +
                                       src_grad2[src_add[n]]*map_wts[num_wts*n+2];
	    }
	}
      else if ( num_wts == 4 )
	{
      	  for ( n = 0; n < num_links; ++n )
	    {
              dst_array[dst_add[n]] += src_array[src_add[n]]*map_wts[num_wts*n] +
                                       src_grad1[src_add[n]]*map_wts[num_wts*n+1] +
                                       src_grad2[src_add[n]]*map_wts[num_wts*n+2] +
                                       src_grad3[src_add[n]]*map_wts[num_wts*n+3];
	    }
	}
    }

  if ( cdoTimer ) timer_stop(timer_remap);
}

static
long get_max_add(long num_links, long size, const int *restrict add)
{
  long n, i;
  long max_add;
  int *isum;

  isum = (int *) malloc(size*sizeof(int));
  memset(isum, 0, size*sizeof(int));

  for ( n = 0; n < num_links; ++n ) isum[add[n]]++;

  max_add = 0;
  for ( i = 0; i < size; ++i ) if ( isum[i] > max_add ) max_add = isum[i];
  free(isum);

  return (max_add);
}

static 
long binary_search_int(const int *array, long len, int value)
{       
  long low = 0, high = len - 1, midpoint = 0;
 
  while ( low <= high )
    {
      midpoint = low + (high - low)/2;      
 
      // check to see if value is equal to item in array
      if ( value == array[midpoint] ) return midpoint;

      if ( value < array[midpoint] )
	high = midpoint - 1;
      else
	low  = midpoint + 1;
    }
 
  // item was not found
  return -1L;
}

/*
  -----------------------------------------------------------------------

  Performs the remapping based on weights computed elsewhere
     
  -----------------------------------------------------------------------
*/
void remap_laf(double *restrict dst_array, double missval, long dst_size, long num_links, double *restrict map_wts,
	       long num_wts, const int *restrict dst_add, const int *restrict src_add, const double *restrict src_array)
{
  /*
    Input arrays:

    int *dst_add         ! destination address for each link
    int *src_add         ! source      address for each link

    int num_wts          ! num of weights used in remapping

    double *map_wts      ! remapping weights for each link

    double *src_array    ! array with source field to be remapped

    output variables:

    double *dst_array    ! array for remapped field on destination grid
  */

  /* Local variables */
  long i, n, k, ncls, imax;
  long max_cls;
  double wts;
  double *src_cls;
  double *src_wts;
#if defined(_OPENMP)
  double **src_cls2;
  double **src_wts2;
  int ompthID;
#endif

  for ( i = 0; i < dst_size; ++i ) dst_array[i] = missval;

  if ( num_links == 0 ) return;

  max_cls = get_max_add(num_links, dst_size, dst_add);

#if defined(_OPENMP)
  src_cls2 = (double **) malloc(ompNumThreads*sizeof(double *));
  src_wts2 = (double **) malloc(ompNumThreads*sizeof(double *));
  for ( i = 0; i < ompNumThreads; ++i )
    {
      src_cls2[i] = (double *) malloc(max_cls*sizeof(double));
      src_wts2[i] = (double *) malloc(max_cls*sizeof(double));
    }
#else
  src_cls = (double *) malloc(max_cls*sizeof(double));
  src_wts = (double *) malloc(max_cls*sizeof(double));
#endif

  for ( n = 0; n < num_links; ++n )
    if ( DBL_IS_EQUAL(dst_array[dst_add[n]], missval) ) dst_array[dst_add[n]] = ZERO;

#if defined(_OPENMP)
#pragma omp parallel for default(none) \
  shared(dst_size, src_cls2, src_wts2, num_links, dst_add, src_add, src_array, map_wts, \
	 num_wts, dst_array, max_cls)					\
  private(i, n, k, ompthID, src_cls, src_wts, ncls, imax, wts) \
  schedule(dynamic,1)
#endif
  for ( i = 0; i < dst_size; ++i )
    {
#if defined(_OPENMP)
      ompthID = omp_get_thread_num();
      src_cls = src_cls2[ompthID];
      src_wts = src_wts2[ompthID];
#endif
      memset(src_cls, 0, max_cls*sizeof(double));
      memset(src_wts, 0, max_cls*sizeof(double));
      /*
      ncls = 0;
      for ( n = 0; n < num_links; n++ )
	{
	  if ( i == dst_add[n] )
	    {
	      for ( k = 0; k < ncls; k++ )
		if ( IS_EQUAL(src_array[src_add[n]], src_cls[k]) ) break;
	      
	      if ( k == ncls )
		{
		  src_cls[k] = src_array[src_add[n]];
		  ncls++;
		}
	      
	      src_wts[k] += map_wts[num_wts*n];
	    }
	}
      */
      /* only for sorted dst_add! */
      {
      long min_add = 1, max_add = 0;

      n = binary_search_int(dst_add, num_links, (int)i);

      if ( n >= 0 && n < num_links )
	{
	  min_add = n;
	  
	  for ( n = min_add+1; n < num_links; ++n )
	    if ( i != dst_add[n] ) break;

	  max_add = n;

	  for ( n = min_add; n > 0; --n )
	    if ( i != dst_add[n-1] ) break;

	  min_add = n;
	}

      ncls = 0;
      for ( n = min_add; n < max_add; ++n )
	{
	  for ( k = 0; k < ncls; ++k )
	    if ( IS_EQUAL(src_array[src_add[n]], src_cls[k]) ) break;
	      
	  if ( k == ncls )
	    {
	      src_cls[k] = src_array[src_add[n]];
	      ncls++;
	    }
	      
	  src_wts[k] += map_wts[num_wts*n];
	}
      }
      
      if ( ncls )
	{
	  imax = 0;
	  wts = src_wts[0];
	  for ( k = 1; k < ncls; ++k )
	    {
	      if ( src_wts[k] > wts )
		{
		  wts  = src_wts[k];
		  imax = k;
		}
	    }

	  dst_array[i] = src_cls[imax];
	}
    }

#if defined(_OPENMP)
  for ( i = 0; i < ompNumThreads; ++i )
    {
      free(src_cls2[i]);
      free(src_wts2[i]);
    }

  free(src_cls2);
  free(src_wts2);
#else
  free(src_cls);
  free(src_wts);
#endif
}

/*
  -----------------------------------------------------------------------

  Performs the remapping based on weights computed elsewhere
     
  -----------------------------------------------------------------------
*/
void remap_sum(double *restrict dst_array, double missval, long dst_size, long num_links, double *restrict map_wts,
	       long num_wts, const int *restrict dst_add, const int *restrict src_add, const double *restrict src_array)
{
  /*
    Input arrays:

    int *dst_add         ! destination address for each link
    int *src_add         ! source      address for each link

    int num_wts          ! num of weights used in remapping

    double *map_wts      ! remapping weights for each link

    double *src_array    ! array with source field to be remapped

    output variables:

    double *dst_array    ! array for remapped field on destination grid
  */
  /* Local variables */
  long n;

  for ( n = 0; n < dst_size; ++n ) dst_array[n] = missval;

#ifdef SX
#pragma cdir nodep
#endif
  for ( n = 0; n < num_links; ++n )
    if ( DBL_IS_EQUAL(dst_array[dst_add[n]], missval) ) dst_array[dst_add[n]] = ZERO;

  for ( n = 0; n < num_links; ++n )
    {
      /*
	printf("%5d %5d %5d %g # dst_add src_add n\n", dst_add[n], src_add[n], n, map_wts[num_wts*n]);
      */
      //dst_array[dst_add[n]] += src_array[src_add[n]]*map_wts[num_wts*n];
      dst_array[dst_add[n]] += src_array[src_add[n]]*map_wts[num_wts*n];
      printf("%ld %d %d %g %g %g\n", n, dst_add[n], src_add[n],
	     src_array[src_add[n]], map_wts[num_wts*n], dst_array[dst_add[n]]);
    }
}


#define  DEFAULT_MAX_ITER  100

static long    Max_Iter = DEFAULT_MAX_ITER;  /* Max iteration count for i, j iteration */
static double  converge = 1.e-10;            /* Convergence criterion */

void remap_set_max_iter(long max_iter)
{
  if ( max_iter > 0 ) Max_Iter = max_iter;
}


/* !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! */
/*                                                                         */
/*      BILINEAR INTERPOLATION                                             */
/*                                                                         */
/* !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! */

static
int grid_search(remapgrid_t *rg, int *restrict src_add, double *restrict src_lats, 
		double *restrict src_lons,  double plat, double plon, const int *restrict src_grid_dims,
		const double *restrict src_center_lat, const double *restrict src_center_lon,
		const restr_t *restrict src_grid_bound_box, const int *restrict src_bin_add)
{
  /*
    Output variables:

    int    src_add[4]              ! address of each corner point enclosing P
    double src_lats[4]             ! latitudes  of the four corner points
    double src_lons[4]             ! longitudes of the four corner points

    Input variables:

    double plat                    ! latitude  of the search point
    double plon                    ! longitude of the search point

    int src_grid_dims[2]           ! size of each src grid dimension

    double src_center_lat[]        ! latitude  of each src grid center 
    double src_center_lon[]        ! longitude of each src grid center

    restr_t src_grid_bound_box[][4] ! bound box for source grid

    int src_bin_add[][2]           ! latitude bins for restricting
  */
  /*  Local variables */
  long n, n2, next_n, srch_add, srch_add4;    /* dummy indices                    */
  long nx, ny;                                /* dimensions of src grid           */
  long min_add, max_add;                      /* addresses for restricting search */
  long i, j, jp1, ip1, n_add, e_add, ne_add;  /* addresses                        */
  long nbins;
  /* Vectors for cross-product check */
  double vec1_lat, vec1_lon;
  double vec2_lat, vec2_lon, cross_product;
  double coslat_dst, sinlat_dst, coslon_dst, sinlon_dst;
  double dist_min, distance; /* For computing dist-weighted avg */
  int scross[4], scross_last = 0;
  int search_result = 0;
  restr_t rlat, rlon;

  nbins = rg->num_srch_bins;

  rlat = RESTR_SCALE(plat);
  rlon = RESTR_SCALE(plon);

  /* restrict search first using bins */

  for ( n = 0; n < 4; ++n ) src_add[n] = 0;

  min_add = rg->grid1_size-1;
  max_add = 0;

  for ( n = 0; n < nbins; ++n )
    {
      n2 = n<<1;
      if ( rlat >= rg->bin_lats[n2] && rlat <= rg->bin_lats[n2+1] &&
	   rlon >= rg->bin_lons[n2] && rlon <= rg->bin_lons[n2+1] )
	{
	  if ( src_bin_add[n2  ] < min_add ) min_add = src_bin_add[n2  ];
	  if ( src_bin_add[n2+1] > max_add ) max_add = src_bin_add[n2+1];
	}
    }
 
  /* Now perform a more detailed search */

  nx = src_grid_dims[0];
  ny = src_grid_dims[1];

  /* srch_loop */
  for ( srch_add = min_add; srch_add <= max_add; ++srch_add )
    {
      srch_add4 = srch_add<<2;
      /* First check bounding box */
      if ( rlat >= src_grid_bound_box[srch_add4  ] && 
           rlat <= src_grid_bound_box[srch_add4+1] &&
           rlon >= src_grid_bound_box[srch_add4+2] &&
	   rlon <= src_grid_bound_box[srch_add4+3] )
	{
	  /* We are within bounding box so get really serious */

          /* Determine neighbor addresses */
          j = srch_add/nx;
          i = srch_add - j*nx;

          if ( i < (nx-1) )
            ip1 = i + 1;
          else
	    {
	      /* 2009-01-09 Uwe Schulzweida: bug fix */
	      if ( rg->grid1_is_cyclic )
		ip1 = 0;
	      else
		ip1 = i;
	    }

          if ( j < (ny-1) )
            jp1 = j + 1;
          else
	    {
	      /* 2008-12-17 Uwe Schulzweida: latitute cyclic ??? (bug fix) */
	      jp1 = j;
	    }

          n_add  = jp1*nx + i;
          e_add  = j  *nx + ip1;
	  ne_add = jp1*nx + ip1;

          src_lats[0] = src_center_lat[srch_add];
          src_lats[1] = src_center_lat[e_add];
          src_lats[2] = src_center_lat[ne_add];
          src_lats[3] = src_center_lat[n_add];

          src_lons[0] = src_center_lon[srch_add];
          src_lons[1] = src_center_lon[e_add];
          src_lons[2] = src_center_lon[ne_add];
          src_lons[3] = src_center_lon[n_add];

	  /* For consistency, we must make sure all lons are in same 2pi interval */

          vec1_lon = src_lons[0] - plon;
          if      ( vec1_lon >  PI ) src_lons[0] -= PI2;
          else if ( vec1_lon < -PI ) src_lons[0] += PI2;

          for ( n = 1; n < 4; ++n )
	    {
	      vec1_lon = src_lons[n] - src_lons[0];
	      if      ( vec1_lon >  PI ) src_lons[n] -= PI2;
	      else if ( vec1_lon < -PI ) src_lons[n] += PI2;
	    }

          /* corner_loop */
          for ( n = 0; n < 4; ++n )
	    {
	      next_n = (n+1)%4;

	      /*
		Here we take the cross product of the vector making 
		up each box side with the vector formed by the vertex
		and search point.  If all the cross products are 
		positive, the point is contained in the box.
	      */
	      vec1_lat = src_lats[next_n] - src_lats[n];
	      vec1_lon = src_lons[next_n] - src_lons[n];
	      vec2_lat = plat - src_lats[n];
	      vec2_lon = plon - src_lons[n];

	      /* Check for 0,2pi crossings */

	      if      ( vec1_lon >  THREE*PIH ) vec1_lon -= PI2;
	      else if ( vec1_lon < -THREE*PIH ) vec1_lon += PI2;

	      if      ( vec2_lon >  THREE*PIH ) vec2_lon -= PI2;
	      else if ( vec2_lon < -THREE*PIH ) vec2_lon += PI2;

	      cross_product = vec1_lon*vec2_lat - vec2_lon*vec1_lat;

	      /* If cross product is less than ZERO, this cell doesn't work    */
	      /* 2008-10-16 Uwe Schulzweida: bug fix for cross_product eq zero */

	      scross[n] = cross_product < 0 ? -1 : cross_product > 0 ? 1 : 0;

	      if ( n == 0 ) scross_last = scross[n];

	      if ( (scross[n] < 0 && scross_last > 0) || (scross[n] > 0 && scross_last < 0) ) break;

	      scross_last = scross[n];
	    } /* corner_loop */

	  if ( n >= 4 )
	    {
	      n = 0;
	      if      ( scross[0]>=0 && scross[1]>=0 && scross[2]>=0 && scross[3]>=0 ) n = 4;
	      else if ( scross[0]<=0 && scross[1]<=0 && scross[2]<=0 && scross[3]<=0 ) n = 4;
	    }

	  /* If cross products all same sign, we found the location */
          if ( n >= 4 )
	    {
	      src_add[0] = srch_add;
	      src_add[1] = e_add;
	      src_add[2] = ne_add;
	      src_add[3] = n_add;

	      search_result = 1;

	      return (search_result);
	    }

	  /* Otherwise move on to next cell */

        } /* Bounding box check */
    } /* srch_loop */

  /*
    If no cell found, point is likely either in a box that straddles either pole or is outside 
    the grid. Fall back to a distance-weighted average of the four closest points.
    Go ahead and compute weights here, but store in src_lats and return -add to prevent the 
    parent routine from computing bilinear weights.
  */
  if ( ! rg->lextrapolate ) return (search_result);

  /*
    printf("Could not find location for %g %g\n", plat*RAD2DEG, plon*RAD2DEG);
    printf("Using nearest-neighbor average for this point\n");
  */
  coslat_dst = cos(plat);
  sinlat_dst = sin(plat);
  coslon_dst = cos(plon);
  sinlon_dst = sin(plon);

  dist_min = BIGNUM;
  for ( n = 0; n < 4; ++n ) src_lats[n] = BIGNUM;
  for ( srch_add = min_add; srch_add <= max_add; ++srch_add )
    {
      distance = acos(coslat_dst*cos(src_center_lat[srch_add])*
		     (coslon_dst*cos(src_center_lon[srch_add]) +
                      sinlon_dst*sin(src_center_lon[srch_add]))+
		      sinlat_dst*sin(src_center_lat[srch_add]));

      if ( distance < dist_min )
	{
          for ( n = 0; n < 4; ++n )
	    {
	      if ( distance < src_lats[n] )
		{
		  for ( i = 3; i > n; --i )
		    {
		      src_add [i] = src_add [i-1];
		      src_lats[i] = src_lats[i-1];
		    }
		  search_result = -1;
		  src_add [n] = srch_add;
		  src_lats[n] = distance;
		  dist_min = src_lats[3];
		  break;
		}
	    }
        }
    }

  for ( n = 0; n < 4; ++n ) src_lons[n] = ONE/(src_lats[n] + TINY);
  distance = 0.0;
  for ( n = 0; n < 4; ++n ) distance += src_lons[n];
  for ( n = 0; n < 4; ++n ) src_lats[n] = src_lons[n]/distance;

  return (search_result);
}  /* grid_search */


/*
  This routine stores the address and weight for four links associated with one destination
  point in the appropriate address and weight arrays and resizes those arrays if necessary.
*/
void store_link_bilin(remapvars_t *rv, int dst_add, const int *restrict src_add, const double *restrict weights)
{
  /*
    Input variables:
    int dst_add       ! address on destination grid
    int src_add[4]    ! addresses on source grid
    double weights[4] ! array of remapping weights for these links
  */
  long n;
  long nlink;

  /*
     Increment number of links and check to see if remap arrays need
     to be increased to accomodate the new link. Then store the link.
  */
  nlink = rv->num_links;
  rv->num_links += 4;

  if ( rv->num_links >= rv->max_links ) 
    resize_remap_vars(rv, rv->resize_increment);

  for ( n = 0; n < 4; ++n )
    {
      rv->grid1_add[nlink+n] = src_add[n];
      rv->grid2_add[nlink+n] = dst_add;
      rv->wts      [nlink+n] = weights[n];
    }

} /* store_link_bilin */

static
long find_ij_weights(double plon, double plat, double *restrict src_lats, double *restrict src_lons, double *ig, double *jg)
{
  long iter;                     /*  iteration counters   */
  double iguess, jguess;         /*  current guess for bilinear coordinate  */
  double deli, delj;             /*  corrections to iw,jw                   */
  double dth1, dth2, dth3;       /*  some latitude  differences             */
  double dph1, dph2, dph3;       /*  some longitude differences             */
  double dthp, dphp;             /*  difference between point and sw corner */
  double mat1, mat2, mat3, mat4; /*  matrix elements                        */
  double determinant;            /*  matrix determinant                     */

  /* Iterate to find iw,jw for bilinear approximation  */

  dth1 = src_lats[1] - src_lats[0];
  dth2 = src_lats[3] - src_lats[0];
  dth3 = src_lats[2] - src_lats[1] - dth2;

  dph1 = src_lons[1] - src_lons[0];
  dph2 = src_lons[3] - src_lons[0];
  dph3 = src_lons[2] - src_lons[1];

  if ( dph1 >  THREE*PIH ) dph1 -= PI2;
  if ( dph2 >  THREE*PIH ) dph2 -= PI2;
  if ( dph3 >  THREE*PIH ) dph3 -= PI2;
  if ( dph1 < -THREE*PIH ) dph1 += PI2;
  if ( dph2 < -THREE*PIH ) dph2 += PI2;
  if ( dph3 < -THREE*PIH ) dph3 += PI2;

  dph3 = dph3 - dph2;

  iguess = HALF;
  jguess = HALF;

  for ( iter = 0; iter < Max_Iter; ++iter )
    {
      dthp = plat - src_lats[0] - dth1*iguess - dth2*jguess - dth3*iguess*jguess;
      dphp = plon - src_lons[0];
      
      if ( dphp >  THREE*PIH ) dphp -= PI2;
      if ( dphp < -THREE*PIH ) dphp += PI2;

      dphp = dphp - dph1*iguess - dph2*jguess - dph3*iguess*jguess;

      mat1 = dth1 + dth3*jguess;
      mat2 = dth2 + dth3*iguess;
      mat3 = dph1 + dph3*jguess;
      mat4 = dph2 + dph3*iguess;

      determinant = mat1*mat4 - mat2*mat3;

      deli = (dthp*mat4 - dphp*mat2)/determinant;
      delj = (dphp*mat1 - dthp*mat3)/determinant;

      if ( fabs(deli) < converge && fabs(delj) < converge ) break;

      iguess += deli;
      jguess += delj;
    }

  *ig = iguess;
  *jg = jguess;

  return (iter);
}

/*
  -----------------------------------------------------------------------

  This routine computes the weights for a bilinear interpolation.

  -----------------------------------------------------------------------
*/
void remap_bilin(remapgrid_t *rg, remapvars_t *rv)
{
  /*   Local variables */
  int  lwarn = TRUE;
  int  search_result;
  long grid2_size;
  long dst_add;                  /*  destination addresss */
  long n, icount;
  long iter;                     /*  iteration counters   */

  int src_add[4];                /*  address for the four source points     */

  double src_lats[4];            /*  latitudes  of four bilinear corners    */
  double src_lons[4];            /*  longitudes of four bilinear corners    */
  double wgts[4];                /*  bilinear weights for four corners      */

  double plat, plon;             /*  lat/lon coords of destination point    */
  double findex = 0;

  if ( cdoTimer ) timer_start(timer_remap_bil);

  progressInit();

  grid2_size = rg->grid2_size;

  /* Compute mappings from grid1 to grid2 */

  if ( rg->grid1_rank != 2 )
    cdoAbort("Can not do bilinear interpolation when grid1_rank != 2"); 

  /* Loop over destination grid */

#if defined(_OPENMP)
#pragma omp parallel for default(none) \
  shared(ompNumThreads, cdoTimer, cdoVerbose, grid2_size, rg, rv, Max_Iter, converge, lwarn, findex) \
  private(dst_add, n, icount, iter, src_add, src_lats, src_lons, wgts, plat, plon, search_result)    \
  schedule(dynamic,1)
#endif
  /* grid_loop1 */
  for ( dst_add = 0; dst_add < grid2_size; ++dst_add )
    {
      int lprogress = 1;
#if defined(_OPENMP)
      if ( omp_get_thread_num() != 0 ) lprogress = 0;
#endif
#if defined(_OPENMP)
#pragma omp atomic
#endif
      findex++;
      if ( lprogress ) progressStatus(0, 1, findex/grid2_size);

      if ( ! rg->grid2_mask[dst_add] ) continue;

      plat = rg->grid2_center_lat[dst_add];
      plon = rg->grid2_center_lon[dst_add];

      /* Find nearest square of grid points on source grid  */

      search_result = grid_search(rg, src_add, src_lats, src_lons, 
				  plat, plon, rg->grid1_dims,
				  rg->grid1_center_lat, rg->grid1_center_lon,
				  rg->grid1_bound_box, rg->bin_addr1);
     

      /* Check to see if points are land points */
      if ( search_result > 0 )
	{
	  for ( n = 0; n < 4; ++n )
	    if ( ! rg->grid1_mask[src_add[n]] ) search_result = 0;
	}

      /* If point found, find local iw,jw coordinates for weights  */
      if ( search_result > 0 )
	{
	  double iw, jw;  /*  current guess for bilinear coordinate  */

          rg->grid2_frac[dst_add] = ONE;

	  iter = find_ij_weights(plon, plat, src_lats, src_lons, &iw, &jw);

          if ( iter < Max_Iter )
	    {
	      /* Successfully found iw,jw - compute weights */

	      wgts[0] = (ONE-iw) * (ONE-jw);
	      wgts[1] =      iw  * (ONE-jw);
	      wgts[2] =      iw  *      jw;
	      wgts[3] = (ONE-iw) *      jw;

#if defined(_OPENMP)
#pragma omp critical
#endif
	      store_link_bilin(rv, dst_add, src_add, wgts);
	    }
          else
	    {
	      if ( cdoVerbose )
		{
		  cdoPrint("Point coords: %g %g", plat, plon);
		  cdoPrint("Src grid lats: %g %g %g %g", src_lats[0], src_lats[1], src_lats[2], src_lats[3]);
		  cdoPrint("Src grid lons: %g %g %g %g", src_lons[0], src_lons[1], src_lons[2], src_lons[3]);
		  cdoPrint("Src grid addresses: %d %d %d %d", src_add[0], src_add[1], src_add[2], src_add[3]);
		  cdoPrint("Src grid lats: %g %g %g %g",
			   rg->grid1_center_lat[src_add[0]], rg->grid1_center_lat[src_add[1]],
			   rg->grid1_center_lat[src_add[2]], rg->grid1_center_lat[src_add[3]]);
		  cdoPrint("Src grid lons: %g %g %g %g",
			   rg->grid1_center_lon[src_add[0]], rg->grid1_center_lon[src_add[1]],
			   rg->grid1_center_lon[src_add[2]], rg->grid1_center_lon[src_add[3]]);
		  cdoPrint("Current iw,jw : %g %g", iw, jw);
		}

	      if ( cdoVerbose || lwarn )
		{
		  lwarn = FALSE;
		  //  cdoWarning("Iteration for iw,jw exceed max iteration count of %d!", Max_Iter);
		  cdoWarning("Bilinear interpolation failed for some grid points - used a distance-weighted average instead!");
		}

	      search_result = -1;
	    }
	}

      /*
	Search for bilinear failed - use a distance-weighted average instead (this is typically near the pole)
      */
      if ( search_result < 0 )
	{
          icount = 0;
          for ( n = 0; n < 4; ++n )
	    {
	      if ( rg->grid1_mask[src_add[n]] )
		icount++;
	      else
		src_lats[n] = ZERO;
	    }

          if ( icount > 0 )
	    {
	      /* Renormalize weights */
	      double sum_wgts = 0.0; /* sum of weights for normalization */
	      /* 2012-05-08 Uwe Schulzweida: using absolute value of src_lats (bug fix) */
	      for ( n = 0; n < 4; ++n ) sum_wgts += fabs(src_lats[n]);
	      for ( n = 0; n < 4; ++n ) wgts[n] = fabs(src_lats[n])/sum_wgts;

	      rg->grid2_frac[dst_add] = ONE;

#if defined(_OPENMP)
#pragma omp critical
#endif
	      store_link_bilin(rv, dst_add, src_add, wgts);
	    }
        }
    } /* grid_loop1 */

  if ( cdoTimer ) timer_stop(timer_remap_bil);
} /* remap_bilin */


/* !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! */
/*                                                                         */
/*      BICUBIC INTERPOLATION                                              */
/*                                                                         */
/* !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! */

/*
  This routine stores the address and weight for four links associated with one destination 
  point in the appropriate address and weight arrays and resizes those arrays if necessary.
*/
static
void store_link_bicub(remapvars_t *rv, int dst_add, const int *restrict src_add, double weights[4][4])
{
  /*
    Input variables:
    int dst_add          ! address on destination grid
    int src_add[4]       ! addresses on source grid
    double weights[4][4] ! array of remapping weights for these links
  */
  long n, k;
  long nlink;

  /*
     Increment number of links and check to see if remap arrays need
     to be increased to accomodate the new link. Then store the link.
  */
  nlink = rv->num_links;
  rv->num_links += 4;

  if ( rv->num_links >= rv->max_links ) 
    resize_remap_vars(rv, rv->resize_increment);

  for ( n = 0; n < 4; ++n )
    {
      rv->grid1_add[nlink+n] = src_add[n];
      rv->grid2_add[nlink+n] = dst_add;
      for ( k = 0; k < 4; ++k )
	rv->wts[4*(nlink+n)+k] = weights[k][n];
    }

} /* store_link_bicub */


/*
  -----------------------------------------------------------------------

  This routine computes the weights for a bicubic interpolation.

  -----------------------------------------------------------------------
*/
void remap_bicub(remapgrid_t *rg, remapvars_t *rv)
{
  /*   Local variables */
  int  lwarn = TRUE;
  int  search_result;
  long n, icount;
  long dst_add;        /*  destination addresss */
  long iter;           /*  iteration counters   */

  int src_add[4];     /* address for the four source points */

  double src_lats[4]; /*  latitudes  of four bilinear corners */
  double src_lons[4]; /*  longitudes of four bilinear corners */
  double wgts[4][4];  /*  bicubic weights for four corners    */

  double plat, plon;             /*  lat/lon coords of destination point    */
  double findex = 0;

  progressInit();

  /* Compute mappings from grid1 to grid2 */

  if ( rg->grid1_rank != 2 )
    cdoAbort("Can not do bicubic interpolation when grid1_rank != 2"); 

  /* Loop over destination grid */

#if defined(_OPENMP)
#pragma omp parallel for default(none) \
  shared(ompNumThreads, cdoTimer, cdoVerbose, rg, rv, Max_Iter, converge, lwarn, findex) \
  private(dst_add, n, icount, iter, src_add, src_lats, src_lons, wgts, plat, plon, search_result) \
  schedule(dynamic,1)
#endif
  /* grid_loop1 */
  for ( dst_add = 0; dst_add < rg->grid2_size; ++dst_add )
    {
      int lprogress = 1;
#if defined(_OPENMP)
      if ( omp_get_thread_num() != 0 ) lprogress = 0;
#endif
#if defined(_OPENMP)
#pragma omp atomic
#endif
      findex++;
      if ( lprogress ) progressStatus(0, 1, findex/rg->grid2_size);

      if ( ! rg->grid2_mask[dst_add] ) continue;

      plat = rg->grid2_center_lat[dst_add];
      plon = rg->grid2_center_lon[dst_add];

      /* Find nearest square of grid points on source grid  */
      search_result = grid_search(rg, src_add, src_lats, src_lons, 
				  plat, plon, rg->grid1_dims,
				  rg->grid1_center_lat, rg->grid1_center_lon,
				  rg->grid1_bound_box, rg->bin_addr1);

      /* Check to see if points are land points */
      if ( search_result > 0 )
	{
	  for ( n = 0; n < 4; ++n )
	    if ( ! rg->grid1_mask[src_add[n]] ) search_result = 0;
	}

      /* If point found, find local iw,jw coordinates for weights  */
      if ( search_result > 0 )
	{
	  double iw, jw;  /*  current guess for bilinear coordinate  */

          rg->grid2_frac[dst_add] = ONE;

	  iter = find_ij_weights(plon, plat, src_lats, src_lons, &iw, &jw);

          if ( iter < Max_Iter )
	    {
	      /* Successfully found iw,jw - compute weights */

	      wgts[0][0] = (ONE-jw*jw*(THREE-TWO*jw)) * (ONE-iw*iw*(THREE-TWO*iw));
	      wgts[0][1] = (ONE-jw*jw*(THREE-TWO*jw)) *      iw*iw*(THREE-TWO*iw);
	      wgts[0][2] =      jw*jw*(THREE-TWO*jw)  *      iw*iw*(THREE-TWO*iw);
	      wgts[0][3] =      jw*jw*(THREE-TWO*jw)  * (ONE-iw*iw*(THREE-TWO*iw));
	      wgts[1][0] = (ONE-jw*jw*(THREE-TWO*jw)) *      iw*(iw-ONE)*(iw-ONE);
	      wgts[1][1] = (ONE-jw*jw*(THREE-TWO*jw)) *      iw*iw*(iw-ONE);
	      wgts[1][2] =      jw*jw*(THREE-TWO*jw)  *      iw*iw*(iw-ONE);
	      wgts[1][3] =      jw*jw*(THREE-TWO*jw)  *      iw*(iw-ONE)*(iw-ONE);
	      wgts[2][0] =      jw*(jw-ONE)*(jw-ONE)  * (ONE-iw*iw*(THREE-TWO*iw));
	      wgts[2][1] =      jw*(jw-ONE)*(jw-ONE)  *      iw*iw*(THREE-TWO*iw);
	      wgts[2][2] =      jw*jw*(jw-ONE)        *      iw*iw*(THREE-TWO*iw);
	      wgts[2][3] =      jw*jw*(jw-ONE)        * (ONE-iw*iw*(THREE-TWO*iw));
	      wgts[3][0] =      iw*(iw-ONE)*(iw-ONE)  *      jw*(jw-ONE)*(jw-ONE);
              wgts[3][1] =      iw*iw*(iw-ONE)        *      jw*(jw-ONE)*(jw-ONE);
	      wgts[3][2] =      iw*iw*(iw-ONE)        *      jw*jw*(jw-ONE);
	      wgts[3][3] =      iw*(iw-ONE)*(iw-ONE)  *      jw*jw*(jw-ONE);

#if defined(_OPENMP)
#pragma omp critical
#endif
	      store_link_bicub(rv, dst_add, src_add, wgts);
	    }
          else
	    {
	      if ( cdoVerbose || lwarn )
		{
		  lwarn = FALSE;
		  // cdoWarning("Iteration for iw,jw exceed max iteration count of %d!", Max_Iter);
		  cdoWarning("Bicubic interpolation failed for some grid points - used a distance-weighted average instead!");
		}

	      search_result = -1;
	    }
	}
	  
      /*
	Search for bicubic failed - use a distance-weighted average instead (this is typically near the pole)
      */
      if ( search_result < 0 )
	{
          icount = 0;
          for ( n = 0; n < 4; ++n )
	    {
	      if ( rg->grid1_mask[src_add[n]] )
		icount++;
	      else
		src_lats[n] = ZERO;
	    }

          if ( icount > 0 )
	    {
	      /* Renormalize weights */
	      double sum_wgts = 0.0; /* sum of weights for normalization */
	      /* 2012-05-08 Uwe Schulzweida: using absolute value of src_lats (bug fix) */
	      for ( n = 0; n < 4; ++n ) sum_wgts += fabs(src_lats[n]);
	      for ( n = 0; n < 4; ++n ) wgts[0][n] = fabs(src_lats[n])/sum_wgts;
	      for ( n = 0; n < 4; ++n ) wgts[1][n] = ZERO;
	      for ( n = 0; n < 4; ++n ) wgts[2][n] = ZERO;
	      for ( n = 0; n < 4; ++n ) wgts[3][n] = ZERO;

	      rg->grid2_frac[dst_add] = ONE;

#if defined(_OPENMP)
#pragma omp critical
#endif
	      store_link_bicub(rv, dst_add, src_add, wgts);
	    }
        }
    } /* grid_loop1 */

} /* remap_bicub */


/* !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! */
/*                                                                         */
/*      INTERPOLATION USING A DISTANCE-WEIGHTED AVERAGE                    */
/*                                                                         */
/* !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! */

#define  num_neighbors  4  /* num nearest neighbors to interpolate from */

static
void get_restrict_add(remapgrid_t *rg, double plat, double plon, const int *restrict src_bin_add,
		      long *minadd, long *maxadd)
{
  long n, n2, nmax;
  long min_add = 0, max_add = 0, nm1, np1, i, j, ip1, im1, jp1, jm1;
  long nbins;
  restr_t rlat, rlon;

  nbins = rg->num_srch_bins;
  rlat = RESTR_SCALE(plat);
  rlon = RESTR_SCALE(plon);

  if ( rg->restrict_type == RESTRICT_LATITUDE )
    {
      for ( n = 0; n < nbins; ++n )
	{
	  n2 = n<<1;
	  if ( rlat >= rg->bin_lats[n2  ] && rlat <= rg->bin_lats[n2+1] )
	    {
	      min_add = src_bin_add[n2  ];
	      max_add = src_bin_add[n2+1];

	      nm1 = MAX(n-1, 0);
	      np1 = MIN(n+1, rg->num_srch_bins-1);

	      min_add = MIN(min_add, src_bin_add[2*nm1  ]);
	      max_add = MAX(max_add, src_bin_add[2*nm1+1]);
	      min_add = MIN(min_add, src_bin_add[2*np1  ]);
	      max_add = MAX(max_add, src_bin_add[2*np1+1]);
	    }
	}
    }
  else if ( rg->restrict_type == RESTRICT_LATLON )
    {
      n = 0;
      nmax = NINT(sqrt((double)rg->num_srch_bins)) - 1;
      for ( j = 0; j < nmax; ++j )
	{
	  jp1 = MIN(j+1,nmax);
	  jm1 = MAX(j-1,0);
	  for ( i = 0; i < nmax; ++i )
	    {
	      ip1 = MIN(i+1, nmax);
	      im1 = MAX(i-1, 0);

	      n = n+1;
	      if ( rlat >= rg->bin_lats[2*n  ] && rlat <= rg->bin_lats[2*n+1] &&
		   rlon >= rg->bin_lons[2*n  ] && rlon <= rg->bin_lons[2*n+1] )
		{
		  min_add = src_bin_add[2*n  ];
		  max_add = src_bin_add[2*n+1];

		  nm1 = (jm1-1)*nmax + im1;
		  np1 = (jp1-1)*nmax + ip1;
		  nm1 = MAX(nm1, 0);
		  np1 = MIN(np1, rg->num_srch_bins-1);

		  min_add = MIN(min_add, src_bin_add[2*nm1  ]);
		  max_add = MAX(max_add, src_bin_add[2*nm1+1]);
		  min_add = MIN(min_add, src_bin_add[2*np1  ]);
		  max_add = MAX(max_add, src_bin_add[2*np1+1]);
		}
	    }
	}
    }
  else
    cdoAbort("Unknown search restriction method!");

  *minadd = min_add;
  *maxadd = max_add;
  /*
  if ( cdoVerbose )
    printf("plon %g plat %g min_add %ld max_add %ld diff %ld\n",
	   plon, plat, min_add, max_add, max_add-min_add);
  */
}

/*
   This routine finds the closest num_neighbor points to a search 
   point and computes a distance to each of the neighbors.
*/
static
void grid_search_nbr(remapgrid_t *rg, int *restrict nbr_add, double *restrict nbr_dist, 
		     double plat, double plon, double coslat_dst, double coslon_dst, 
		     double sinlat_dst, double sinlon_dst, const int *restrict src_bin_add,
		     const double *restrict sinlat, const double *restrict coslat,
		     const double *restrict sinlon, const double *restrict coslon)
{
  /*
    Output variables:

    int nbr_add[num_neighbors]     ! address of each of the closest points
    double nbr_dist[num_neighbors] ! distance to each of the closest points

    Input variables:

    int src_bin_add[][2]  ! search bins for restricting search

    double plat,         ! latitude  of the search point
    double plon,         ! longitude of the search point
    double coslat_dst,   ! cos(lat)  of the search point
    double coslon_dst,   ! cos(lon)  of the search point
    double sinlat_dst,   ! sin(lat)  of the search point
    double sinlon_dst    ! sin(lon)  of the search point
  */
  /*  Local variables */
  long n, nadd, nchk;
  long min_add, max_add;
  double distance;     /* Angular distance */

  /* Loop over source grid and find nearest neighbors                         */
  /* restrict the search using search bins expand the bins to catch neighbors */

  get_restrict_add(rg, plat, plon, src_bin_add, &min_add, &max_add);

  /* Initialize distance and address arrays */
  for ( n = 0; n < num_neighbors; ++n )
    {
      nbr_add[n]  = 0;
      nbr_dist[n] = BIGNUM;
    }

  for ( nadd = min_add; nadd <= max_add; ++nadd )
    {
      /* Find distance to this point */
      distance =  sinlat_dst*sinlat[nadd] + coslat_dst*coslat[nadd]*
	         (coslon_dst*coslon[nadd] + sinlon_dst*sinlon[nadd]);
      /* 2008-07-30 Uwe Schulzweida: check that distance is inside the range of -1 to 1,
                                     otherwise the result of acos(distance) is NaN */
      if ( distance >  1 ) distance =  1;
      if ( distance < -1 ) distance = -1;
      distance = acos(distance);

      /* Uwe Schulzweida: if distance is zero, set to small number */
      if ( IS_EQUAL(distance, 0) ) distance = TINY;

      /* Store the address and distance if this is one of the smallest four so far */
      for ( nchk = 0; nchk < num_neighbors; ++nchk )
	{
          if ( distance < nbr_dist[nchk] )
	    {
	      for ( n = num_neighbors-1; n > nchk; --n )
		{
		  nbr_add[n]  = nbr_add[n-1];
		  nbr_dist[n] = nbr_dist[n-1];
		}
	      nbr_add[nchk]  = nadd + 1;
	      nbr_dist[nchk] = distance;
	      break;
	    }
        }
    }

}  /*  grid_search_nbr  */

/*
  This routine stores the address and weight for this link in the appropriate 
  address and weight arrays and resizes those arrays if necessary.
*/
static
void store_link_nbr(remapvars_t *rv, int add1, int add2, double weights)
{
  /*
    Input variables:
    int  add1         ! address on grid1
    int  add2         ! address on grid2
    double weights    ! remapping weight for this link
  */
  long nlink;

  /*
     Increment number of links and check to see if remap arrays need
     to be increased to accomodate the new link. Then store the link.
  */
  nlink = rv->num_links;
  rv->num_links++;

  if ( rv->num_links >= rv->max_links ) 
    resize_remap_vars(rv, rv->resize_increment);

  rv->grid1_add[nlink] = add1;
  rv->grid2_add[nlink] = add2;
  rv->wts[nlink]       = weights;

} /* store_link_nbr */


/*
  -----------------------------------------------------------------------

   This routine computes the inverse-distance weights for a
   nearest-neighbor interpolation.

  -----------------------------------------------------------------------
*/
void remap_distwgt(remapgrid_t *rg, remapvars_t *rv)
{
  /*  Local variables */

  long grid1_size;
  long grid2_size;
  long n;
  long dst_add;                   /* destination address                     */
  int nbr_mask[num_neighbors];    /* mask at nearest neighbors               */
  int nbr_add[num_neighbors];     /* source address at nearest neighbors     */
  double nbr_dist[num_neighbors]; /* angular distance four nearest neighbors */
  double coslat_dst;       /* cos(lat) of destination grid point */
  double coslon_dst;       /* cos(lon) of destination grid point */
  double sinlat_dst;       /* sin(lat) of destination grid point */
  double sinlon_dst;       /* sin(lon) of destination grid point */
  double dist_tot;         /* sum of neighbor distances (for normalizing) */
  double *coslat, *sinlat; /* cosine, sine of grid lats (for distance)    */
  double *coslon, *sinlon; /* cosine, sine of grid lons (for distance)    */
  double wgtstmp;          /* hold the link weight                        */
  double findex = 0;

  progressInit();

  /* Compute mappings from grid1 to grid2 */

  grid1_size = rg->grid1_size;
  grid2_size = rg->grid2_size;

  /* Compute cos, sin of lat/lon on source grid for distance calculations */

  coslat = (double *) malloc(grid1_size*sizeof(double));
  coslon = (double *) malloc(grid1_size*sizeof(double));
  sinlat = (double *) malloc(grid1_size*sizeof(double));
  sinlon = (double *) malloc(grid1_size*sizeof(double));

#if defined(_OPENMP)
#pragma omp parallel for default(none) \
  shared(rg, grid1_size, coslat, coslon, sinlat, sinlon)
#endif
  for ( n = 0; n < grid1_size; ++n )
    {
      coslat[n] = cos(rg->grid1_center_lat[n]);
      coslon[n] = cos(rg->grid1_center_lon[n]);
      sinlat[n] = sin(rg->grid1_center_lat[n]);
      sinlon[n] = sin(rg->grid1_center_lon[n]);
    }

  /* Loop over destination grid  */
  /* grid_loop1 */
#if defined(_OPENMP)
#pragma omp parallel for default(none) \
  shared(ompNumThreads, cdoTimer, rg, rv, grid2_size, coslat, coslon, sinlat, sinlon, findex) \
  private(dst_add, n, coslat_dst, coslon_dst, sinlat_dst, sinlon_dst, dist_tot, \
	  nbr_add, nbr_dist, nbr_mask, wgtstmp)	\
  schedule(dynamic,1)
#endif
  for ( dst_add = 0; dst_add < grid2_size; ++dst_add )
    {
      int lprogress = 1;
#if defined(_OPENMP)
      if ( omp_get_thread_num() != 0 ) lprogress = 0;
#endif
#if defined(_OPENMP)
#pragma omp atomic
#endif
      findex++;
      if ( lprogress ) progressStatus(0, 1, findex/grid2_size);

      if ( ! rg->grid2_mask[dst_add] ) continue;

      coslat_dst = cos(rg->grid2_center_lat[dst_add]);
      coslon_dst = cos(rg->grid2_center_lon[dst_add]);
      sinlat_dst = sin(rg->grid2_center_lat[dst_add]);
      sinlon_dst = sin(rg->grid2_center_lon[dst_add]);
	
      /* Find nearest grid points on source grid and distances to each point */

      grid_search_nbr(rg, nbr_add, nbr_dist, 
		      rg->grid2_center_lat[dst_add],
		      rg->grid2_center_lon[dst_add],
		      coslat_dst, coslon_dst, 
		      sinlat_dst, sinlon_dst,
		      rg->bin_addr1,
		      sinlat, coslat, sinlon, coslon);

      /*
         Compute weights based on inverse distance
	 if mask is false, eliminate those points
      */
      dist_tot = ZERO;
      for ( n = 0; n < num_neighbors; ++n )
	{
	  nbr_mask[n] = FALSE;

	  /* Uwe Schulzweida: check if nbr_add is valid */
	  if ( nbr_add[n] > 0 )
	    if ( rg->grid1_mask[nbr_add[n]-1] )
	      {
		nbr_dist[n] = ONE/nbr_dist[n];
		dist_tot = dist_tot + nbr_dist[n];
		nbr_mask[n] = TRUE;
	      }
	}

      /* Normalize weights and store the link */

      for ( n = 0; n < num_neighbors; ++n )
	{
          if ( nbr_mask[n] )
	    {
	      wgtstmp = nbr_dist[n]/dist_tot;

	      rg->grid2_frac[dst_add] = ONE;
#if defined(_OPENMP)
#pragma omp critical
#endif
	      store_link_nbr(rv, nbr_add[n]-1, dst_add, wgtstmp);
	    }
	}

    } /* grid_loop1 */

  free(coslat);
  free(coslon);
  free(sinlat);
  free(sinlon);

}  /* remap_distwgt */

/* !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! */
/*                                                                         */
/*      INTERPOLATION USING A DISTANCE-WEIGHTED AVERAGE WITH 1 NEIGHBOR    */
/*                                                                         */
/* !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! */

/*
   This routine finds the closest num_neighbor points to a search 
   point and computes a distance to each of the neighbors.
*/
static
void grid_search_nbr1(remapgrid_t *rg, int *restrict nbr_add, double *restrict nbr_dist, 
		      double plat, double plon, double coslat_dst, double coslon_dst, 
		      double sinlat_dst, double sinlon_dst, const int *restrict src_bin_add, 
                      const double *restrict sinlat, const double *restrict coslat,
		      const double *restrict sinlon, const double *restrict coslon)
{
  /*
    Output variables:

    int nbr_add[0]     ! address of each of the closest points
    double nbr_dist[0] ! distance to each of the closest points

    Input variables:

    int src_bin_add[][2] ! search bins for restricting search

    double plat,         ! latitude  of the search point
    double plon,         ! longitude of the search point
    double coslat_dst,   ! cos(lat)  of the search point
    double coslon_dst,   ! cos(lon)  of the search point
    double sinlat_dst,   ! sin(lat)  of the search point
    double sinlon_dst    ! sin(lon)  of the search point
  */
  /*  Local variables */
  long nadd;
  long min_add, max_add;
  double distance;     /* Angular distance */

  /* Loop over source grid and find nearest neighbors                         */
  /* restrict the search using search bins expand the bins to catch neighbors */

  get_restrict_add(rg, plat, plon, src_bin_add, &min_add, &max_add);

  /* Initialize distance and address arrays */
  nbr_add[0]  = 0;
  nbr_dist[0] = BIGNUM;

  // printf("%g %g  min %d  max %d  range %d\n", plon, plat, min_add, max_add, max_add-min_add);
  //#define TESTVECTOR

#ifdef  TESTVECTOR
  long i = 0;
  long ni = max_add - min_add + 1;
  double *distvect = malloc(ni*sizeof(double));

  for ( i = 0; i < ni; ++i )
    {
      nadd = min_add + i;
      /* Find distance to this point */
      distvect[i] =  sinlat_dst*sinlat[nadd] + coslat_dst*coslat[nadd]*
	            (coslon_dst*coslon[nadd] + sinlon_dst*sinlon[nadd]);
      /* 2008-07-30 Uwe Schulzweida: check that distance is inside the range of -1 to 1,
                                     otherwise the result of acos(distance) is NaN */
      if ( distvect[i] >  1 ) distvect[i] =  1;
      if ( distvect[i] < -1 ) distvect[i] = -1;
    }

  for ( i = 0; i < ni; ++i )
    distvect[i] = acos(distvect[i]);

  /* Store the address and distance if this is the smallest so far */
  for ( i = 0; i < ni; ++i )
    if ( distvect[i] < nbr_dist[0] )
      {
	nbr_add[0]  = min_add + i + 1;
	nbr_dist[0] = distvect[i];
      }

  free(distvect);

#else
  for ( nadd = min_add; nadd <= max_add; ++nadd )
    {
      /* Find distance to this point */
      distance =  sinlat_dst*sinlat[nadd] + coslat_dst*coslat[nadd]*
	         (coslon_dst*coslon[nadd] + sinlon_dst*sinlon[nadd]);
      /* 2008-07-30 Uwe Schulzweida: check that distance is inside the range of -1 to 1,
                                     otherwise the result of acos(distance) is NaN */
      if ( distance >  1 ) distance =  1;
      if ( distance < -1 ) distance = -1;
      distance = acos(distance);

      /* Store the address and distance if this is the smallest so far */
      if ( distance < nbr_dist[0] )
	{
	  nbr_add[0]  = nadd + 1;
	  nbr_dist[0] = distance;
        }
    }
#endif

}  /*  grid_search_nbr1  */

/*
  -----------------------------------------------------------------------

   This routine computes the inverse-distance weights for a
   nearest-neighbor interpolation.

  -----------------------------------------------------------------------
*/
void remap_distwgt1(remapgrid_t *rg, remapvars_t *rv)
{

  /*  Local variables */
  long grid1_size;
  long grid2_size;
  long n;
  long dst_add;            /* destination address */
  int nbr_mask;            /* mask at nearest neighbors */
  int nbr_add;             /* source address at nearest neighbors     */
  double nbr_dist;         /* angular distance four nearest neighbors */
  double coslat_dst;       /* cos(lat) of destination grid point */
  double coslon_dst;       /* cos(lon) of destination grid point */
  double sinlat_dst;       /* sin(lat) of destination grid point */
  double sinlon_dst;       /* sin(lon) of destination grid point */
  double *coslat, *sinlat; /* cosine, sine of grid lats (for distance)    */
  double *coslon, *sinlon; /* cosine, sine of grid lons (for distance)    */
  double wgtstmp;          /* hold the link weight                        */
  double findex = 0;

  if ( cdoTimer ) timer_start(timer_remap_nn);

  progressInit();

  /* Compute mappings from grid1 to grid2 */

  grid1_size = rg->grid1_size;
  grid2_size = rg->grid2_size;

  /* Compute cos, sin of lat/lon on source grid for distance calculations */

  coslat = (double *) malloc(grid1_size*sizeof(double));
  coslon = (double *) malloc(grid1_size*sizeof(double));
  sinlat = (double *) malloc(grid1_size*sizeof(double));
  sinlon = (double *) malloc(grid1_size*sizeof(double));

#if defined(_OPENMP)
#pragma omp parallel for default(none) \
  shared(rg, grid1_size, coslat, coslon, sinlat, sinlon)
#endif
  for ( n = 0; n < grid1_size; ++n )
    {
      coslat[n] = cos(rg->grid1_center_lat[n]);
      coslon[n] = cos(rg->grid1_center_lon[n]);
      sinlat[n] = sin(rg->grid1_center_lat[n]);
      sinlon[n] = sin(rg->grid1_center_lon[n]);
    }

  /* Loop over destination grid  */
  /* grid_loop1 */
#if defined(_OPENMP)
#pragma omp parallel for default(none) \
  shared(ompNumThreads, cdoTimer, rg, rv, grid2_size, coslat, coslon, sinlat, sinlon, findex) \
  private(dst_add, n, coslat_dst, coslon_dst, sinlat_dst, sinlon_dst, nbr_add, nbr_dist, nbr_mask, wgtstmp)	\
  schedule(dynamic,1)
#endif
  for ( dst_add = 0; dst_add < grid2_size; ++dst_add )
    {
      int lprogress = 1;
#if defined(_OPENMP)
      if ( omp_get_thread_num() != 0 ) lprogress = 0;
#endif
#if defined(_OPENMP)
#pragma omp atomic
#endif
      findex++;
      if ( lprogress ) progressStatus(0, 1, findex/grid2_size);

      if ( ! rg->grid2_mask[dst_add] ) continue;

      coslat_dst = cos(rg->grid2_center_lat[dst_add]);
      coslon_dst = cos(rg->grid2_center_lon[dst_add]);
      sinlat_dst = sin(rg->grid2_center_lat[dst_add]);
      sinlon_dst = sin(rg->grid2_center_lon[dst_add]);
	
      /* Find nearest grid points on source grid and  distances to each point */

      grid_search_nbr1(rg, &nbr_add, &nbr_dist,
		       rg->grid2_center_lat[dst_add],
		       rg->grid2_center_lon[dst_add],
		       coslat_dst, coslon_dst, 
		       sinlat_dst, sinlon_dst,
		       rg->bin_addr1,
		       sinlat, coslat, sinlon, coslon);

      /*
         Compute weights based on inverse distance
         if mask is false, eliminate those points
      */
      nbr_mask = FALSE;

      /* Uwe Schulzweida: check if nbr_add is valid */
      if ( nbr_add > 0 )
	if ( rg->grid1_mask[nbr_add-1] )
	  {
	    nbr_mask = TRUE;
	  }

      /* Store the link */

      if ( nbr_mask )
	{
	  wgtstmp = ONE;

	  rg->grid2_frac[dst_add] = ONE;
#if defined(_OPENMP)
#pragma omp critical
#endif
	  store_link_nbr(rv, nbr_add-1, dst_add, wgtstmp);
	}

    } /* grid_loop1 */

  free(coslat);
  free(coslon);
  free(sinlat);
  free(sinlon);

  if ( cdoTimer ) timer_stop(timer_remap_nn);
}  /* remap_distwgt1 */


/* !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! */
/*                                                                         */
/*      CONSERVATIVE INTERPOLATION                                         */
/*                                                                         */
/* !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! */

/*
    This routine is identical to the intersection routine except
    that a coordinate transformation (using a Lambert azimuthal
    equivalent projection) is performed to treat polar cells more
    accurately.
*/
static
void pole_intersection(long *location, double *intrsct_lat, double *intrsct_lon, int *lcoinc,
		       int *lthresh, double beglat, double beglon, double endlat, double endlon,
		       double *begseg, int lrevers,
		       long num_srch_cells, long srch_corners, const int *restrict srch_add,
		       const double *restrict srch_corner_lat, const double *restrict srch_corner_lon,
		       int *luse_last, double *intrsct_x, double *intrsct_y,
		       int *avoid_pole_count, double *avoid_pole_offset)
{
  /*
    Intent(in): 
    double beglat, beglon,  ! beginning lat/lon endpoints for segment
    double endlat, endlon   ! ending    lat/lon endpoints for segment
    int    lrevers          ! flag true if segment integrated in reverse

    Intent(inout) :
    double begseg[2] ! begin lat/lon of full segment
    int *location    ! address in destination array containing this
                     ! segment -- also may contain last location on entry
    int *lthresh     ! flag segment crossing threshold boundary

    intent(out): 
    *int lcoinc      ! flag segment coincident with grid line
    double *intrsct_lat, *intrsct_lon ! lat/lon coords of next intersect.
  */
  /* Local variables */
  long n, next_n, cell;
  long ioffset;
  int loutside; /* flags points outside grid */

  double pi4, rns;           /*  north/south conversion */
  double x1, x2;             /*  local x variables for segment */
  double y1, y2;             /*  local y variables for segment */
  double begx, begy;         /*  beginning x,y variables for segment */
  double endx, endy;         /*  beginning x,y variables for segment */
  double begsegx, begsegy;   /*  beginning x,y variables for segment */
  double grdx1, grdx2;       /*  local x variables for grid cell */
  double grdy1, grdy2;       /*  local y variables for grid cell */
  double vec1_y, vec1_x;     /*  vectors and cross products used */
  double vec2_y, vec2_x;     /*  during grid search */
  double cross_product, eps; /*  eps=small offset away from intersect */
  double s1, s2, determ;     /*  variables used for linear solve to   */
  double mat1, mat2, mat3, mat4, rhs1, rhs2;  /* find intersection */

  double *srch_corner_x;     /*  x of each corner of srch cells */
  double *srch_corner_y;     /*  y of each corner of srch cells */

  /*printf("pole_intersection: %g %g %g %g\n", beglat, beglon, endlat, endlon);*/

  /* Initialize defaults, flags, etc. */

  if ( ! *lthresh ) *location = -1;
  *lcoinc      = FALSE;
  *intrsct_lat = endlat;
  *intrsct_lon = endlon;

  loutside = FALSE;
  s1 = ZERO;

  /* Convert coordinates */

  srch_corner_x = (double *) malloc(srch_corners*num_srch_cells*sizeof(double));
  srch_corner_y = (double *) malloc(srch_corners*num_srch_cells*sizeof(double));

  if ( beglat > ZERO )
    {
      pi4 = QUART*PI;
      rns = ONE;
    }
  else
    {
      pi4 = -QUART*PI;
      rns = -ONE;
    }

  if ( *luse_last )
    {
      x1 = *intrsct_x;
      y1 = *intrsct_y;
    }
  else
    {
      x1 = rns*TWO*sin(pi4 - HALF*beglat)*cos(beglon);
      y1 =     TWO*sin(pi4 - HALF*beglat)*sin(beglon);
      *luse_last = TRUE;
    }

  x2 = rns*TWO*sin(pi4 - HALF*endlat)*cos(endlon);
  y2 =     TWO*sin(pi4 - HALF*endlat)*sin(endlon);

  for ( n = 0; n < srch_corners*num_srch_cells; ++n )
    {
      srch_corner_x[n] = rns*TWO*sin(pi4 - HALF*srch_corner_lat[n])*
	                         cos(srch_corner_lon[n]);
      srch_corner_y[n] =     TWO*sin(pi4 - HALF*srch_corner_lat[n])*
	                         sin(srch_corner_lon[n]);
    }

  begx = x1;
  begy = y1;
  endx = x2;
  endy = y2;
  begsegx = rns*TWO*sin(pi4 - HALF*begseg[0])*cos(begseg[1]);
  begsegy =     TWO*sin(pi4 - HALF*begseg[0])*sin(begseg[1]);
  *intrsct_x = endx;
  *intrsct_y = endy;

  /*
     Search for location of this segment in ocean grid using cross
     product method to determine whether a point is enclosed by a cell
  */
  while ( TRUE ) /* srch_loop */
    {
      /* If last segment crossed threshold, use that location */

      if ( *lthresh )
	{
	  for ( cell=0; cell < num_srch_cells; ++cell )
	    if ( srch_add[cell] == *location )
	      {
		eps = TINY;
		goto after_srch_loop;
	      }
	}

      /* Otherwise normal search algorithm */

      for ( cell = 0; cell < num_srch_cells; ++cell ) /* cell_loop  */
	{
	  ioffset = cell*srch_corners;
	  for ( n = 0; n < srch_corners; ++n ) /* corner_loop */
	    {
	      next_n = (n+1)%srch_corners;
	      /*
		Here we take the cross product of the vector making 
		up each cell side with the vector formed by the vertex
		and search point.  If all the cross products are 
		positive, the point is contained in the cell.
	      */
	      vec1_x = srch_corner_x[ioffset+next_n] - srch_corner_x[ioffset+n];
	      vec1_y = srch_corner_y[ioffset+next_n] - srch_corner_y[ioffset+n];
	      vec2_x = x1 - srch_corner_x[ioffset+n];
	      vec2_y = y1 - srch_corner_y[ioffset+n];

	      /* If endpoint coincident with vertex, offset the endpoint */

	      if ( IS_EQUAL(vec2_x, 0) && IS_EQUAL(vec2_y, 0) )
		{
		  x1 += 1.e-10*(x2 - x1);
		  y1 += 1.e-10*(y2 - y1);
		  vec2_x = x1 - srch_corner_x[ioffset+n];
		  vec2_y = y1 - srch_corner_y[ioffset+n];
		}

	      cross_product = vec1_x*vec2_y - vec2_x*vec1_y;

	      /*
		If the cross product for a side is ZERO, the point 
                  lies exactly on the side or the length of a side
                  is ZERO.  If the length is ZERO set det > 0.
                  otherwise, perform another cross 
                  product between the side and the segment itself. 
	        If this cross product is also ZERO, the line is 
	          coincident with the cell boundary - perform the 
                  dot product and only choose the cell if the dot 
                  product is positive (parallel vs anti-parallel).
	      */
	      if ( IS_EQUAL(cross_product, 0) )
		{
		  if ( IS_NOT_EQUAL(vec1_x, 0) || IS_NOT_EQUAL(vec1_y, 0) )
		    {
		      vec2_x = x2 - x1;
		      vec2_y = y2 - y1;
		      cross_product = vec1_x*vec2_y - vec2_x*vec1_y;
		    }
		  else
		    cross_product = ONE;

		  if ( IS_EQUAL(cross_product, 0) )
		    {
		      *lcoinc = TRUE;
		      cross_product = vec1_x*vec2_x + vec1_y*vec2_y;
		      if ( lrevers ) cross_product = -cross_product;
		    }
		}

	      /* If cross product is less than ZERO, this cell doesn't work */

	      if ( cross_product < ZERO ) break; /* corner_loop */
	     
	    } /* corner_loop */

	  /* If cross products all positive, we found the location */

	  if  ( n >= srch_corners )
	    {
	      *location = srch_add[cell];
	      /*
		If the beginning of this segment was outside the
		grid, invert the segment so the intersection found
		will be the first intersection with the grid
	      */
	      if ( loutside )
		{
		  x2 = begx;
		  y2 = begy;
		  *location = -1;
		  eps  = -TINY;
		}
	      else
		eps  = TINY;
            
	      goto after_srch_loop;
	    }

	  /* Otherwise move on to next cell */

	} /* cell_loop */

     /*
       If no cell found, the point lies outside the grid.
       take some baby steps along the segment to see if any
       part of the segment lies inside the grid.  
     */
      loutside = TRUE;
      s1 = s1 + BABY_STEP;
      x1 = begx + s1*(x2 - begx);
      y1 = begy + s1*(y2 - begy);

      /* Reached the end of the segment and still outside the grid return no intersection */

      if ( s1 >= ONE )
	{
          free(srch_corner_y);
          free(srch_corner_x);
          *luse_last = FALSE;
          return;
	}
    } /* srch_loop */

 after_srch_loop:

  /*
    Now that a cell is found, search for the next intersection.
    Loop over sides of the cell to find intersection with side
    must check all sides for coincidences or intersections
  */

  ioffset = cell*srch_corners;

  for ( n = 0; n < srch_corners; ++n ) /* intrsct_loop */
    {
      next_n = (n+1)%srch_corners;

      grdy1 = srch_corner_y[ioffset+n];
      grdy2 = srch_corner_y[ioffset+next_n];
      grdx1 = srch_corner_x[ioffset+n];
      grdx2 = srch_corner_x[ioffset+next_n];

      /* Set up linear system to solve for intersection */

      mat1 = x2 - x1;
      mat2 = grdx1 - grdx2;
      mat3 = y2 - y1;
      mat4 = grdy1 - grdy2;
      rhs1 = grdx1 - x1;
      rhs2 = grdy1 - y1;

      determ = mat1*mat4 - mat2*mat3;

      /*
         If the determinant is ZERO, the segments are either 
           parallel or coincident.  Coincidences were detected 
           above so do nothing.
         If the determinant is non-ZERO, solve for the linear 
           parameters s for the intersection point on each line 
           segment.
         If 0<s1,s2<1 then the segment intersects with this side.
           Return the point of intersection (adding a small
           number so the intersection is off the grid line).
      */
      if ( fabs(determ) > 1.e-30 )
	{
          s1 = (rhs1*mat4 - mat2*rhs2)/determ;
          s2 = (mat1*rhs2 - rhs1*mat3)/determ;

	  /* Uwe Schulzweida: s1 >= ZERO! (bug fix) */
          if ( s2 >= ZERO && s2 <= ONE && s1 >= ZERO && s1 <= ONE )
	    {
	      /*
		Recompute intersection based on full segment
		so intersections are consistent for both sweeps
	      */
	      if ( ! loutside )
		{
		  mat1 = x2 - begsegx;
		  mat3 = y2 - begsegy;
		  rhs1 = grdx1 - begsegx;
		  rhs2 = grdy1 - begsegy;
		}
	      else
		{
		  mat1 = x2 - endx;
		  mat3 = y2 - endy;
		  rhs1 = grdx1 - endx;
		  rhs2 = grdy1 - endy;
		}

	      determ = mat1*mat4 - mat2*mat3;

	      /*
		Sometimes due to roundoff, the previous 
		determinant is non-ZERO, but the lines
		are actually coincident.  If this is the
		case, skip the rest.
	      */
	      if ( IS_NOT_EQUAL(determ, 0) )
	       {
		 s1 = (rhs1*mat4 - mat2*rhs2)/determ;
		 s2 = (mat1*rhs2 - rhs1*mat3)/determ;

		 if ( ! loutside )
		   {
		     *intrsct_x = begsegx + s1*mat1;
		     *intrsct_y = begsegy + s1*mat3;
		   }
		 else 
		   {
		     *intrsct_x = endx + s1*mat1;
		     *intrsct_y = endy + s1*mat3;
		   }

		 /* Convert back to lat/lon coordinates */

		 *intrsct_lon = rns*atan2(*intrsct_y, *intrsct_x);
		 if ( *intrsct_lon < ZERO ) 
		   *intrsct_lon = *intrsct_lon + PI2;
		 
		 if ( fabs(*intrsct_x) > 1.e-10 )
		   *intrsct_lat = (pi4 - asin(rns*HALF*(*intrsct_x)/cos(*intrsct_lon)))*TWO;
		 else if ( fabs(*intrsct_y) > 1.e-10 )
		   *intrsct_lat = (pi4 - asin(HALF*(*intrsct_y)/sin(*intrsct_lon)))*TWO;
		 else
		   *intrsct_lat = TWO*pi4;

		 /* Add offset in transformed space for next pass. */

		 if ( s1 - eps/determ < ONE )
		   {
		     *intrsct_x = *intrsct_x - mat1*(eps/determ);
		     *intrsct_y = *intrsct_y - mat3*(eps/determ);
		   }
		 else
		   {
		     if ( ! loutside)
		       {
			 *intrsct_x = endx;
			 *intrsct_y = endy;
			 *intrsct_lat = endlat;
			 *intrsct_lon = endlon;
		       }
		     else 
		       {
			 *intrsct_x = begsegx;
			 *intrsct_y = begsegy;
			 *intrsct_lat = begseg[0];
			 *intrsct_lon = begseg[1];
		       }
		   }

		 break; /* intrsct_loop */
	       }
	    }
	}
 
      /* No intersection this side, move on to next side */

    } /* intrsct_loop */

  free(srch_corner_y);
  free(srch_corner_x);

  /*
     If segment manages to cross over pole, shift the beginning 
     endpoint in order to avoid hitting pole directly
     (it is ok for endpoint to be pole point)
  */

  if ( fabs(*intrsct_x) < 1.e-10 && fabs(*intrsct_y) < 1.e-10 &&
       (IS_NOT_EQUAL(endx, 0) && IS_NOT_EQUAL(endy, 0)) )
    {
      if ( *avoid_pole_count > 2 )
	{
	  *avoid_pole_count  = 0;
	  *avoid_pole_offset = 10.*(*avoid_pole_offset);
        }

      cross_product = begsegx*(endy-begsegy) - begsegy*(endx-begsegx);
      *intrsct_lat = begseg[0];
      if ( cross_product*(*intrsct_lat) > ZERO )
	{
          *intrsct_lon = beglon    + *avoid_pole_offset;
          begseg[1]    = begseg[1] + *avoid_pole_offset;
	}
      else
	{
          *intrsct_lon = beglon    - *avoid_pole_offset;
          begseg[1]    = begseg[1] - *avoid_pole_offset;
        }

      *avoid_pole_count = *avoid_pole_count + 1;
      *luse_last = FALSE;
    }
  else
    {
      *avoid_pole_count  = 0;
      *avoid_pole_offset = TINY;
    }

  /*
     If the segment crosses a pole threshold, reset the intersection
     to be the threshold latitude and do not reuse x,y intersect
     on next entry.  Only check if did not cross threshold last
     time - sometimes the coordinate transformation can place a
     segment on the other side of the threshold again
  */
  if ( *lthresh )
    {
      if ( *intrsct_lat > north_thresh || *intrsct_lat < south_thresh )
	*lthresh = FALSE;
    }
  else if ( beglat > ZERO && *intrsct_lat < north_thresh )
    {
      mat4 = endlat - begseg[0];
      mat3 = endlon - begseg[1];
      if ( mat3 >  PI ) mat3 = mat3 - PI2;
      if ( mat3 < -PI ) mat3 = mat3 + PI2;
      *intrsct_lat = north_thresh - TINY;
      s1 = (north_thresh - begseg[0])/mat4;
      *intrsct_lon = begseg[1] + s1*mat3;
      *luse_last = FALSE;
      *lthresh = TRUE;
    }
  else if ( beglat < ZERO && *intrsct_lat > south_thresh )
    {
      mat4 = endlat - begseg[0];
      mat3 = endlon - begseg[1];
      if ( mat3 >  PI ) mat3 = mat3 - PI2;
      if ( mat3 < -PI ) mat3 = mat3 + PI2;
      *intrsct_lat = south_thresh + TINY;
      s1 = (south_thresh - begseg[0])/mat4;
      *intrsct_lon = begseg[1] + s1*mat3;
      *luse_last = FALSE;
      *lthresh = TRUE;
    }

  /* If reached end of segment, do not use x,y intersect on next entry */

  if ( IS_EQUAL(*intrsct_lat, endlat) && IS_EQUAL(*intrsct_lon, endlon) ) *luse_last = FALSE;

}  /* pole_intersection */


/*
   This routine finds the next intersection of a destination grid line with 
   the line segment given by beglon, endlon, etc.
   A coincidence flag is returned if the segment is entirely coincident with 
   an ocean grid line.  The cells in which to search for an intersection must 
   have already been restricted in the calling routine.
*/
static
void intersection(long *location, double *intrsct_lat, double *intrsct_lon, int *lcoinc,
		  double beglat, double beglon, double endlat, double endlon, double *begseg,
		  int lbegin, int lrevers,
		  long num_srch_cells, long srch_corners, const int *restrict srch_add,
		  const double *restrict srch_corner_lat, const double *restrict srch_corner_lon,
		  int *last_loc, int *lthresh, double *intrsct_lat_off, double *intrsct_lon_off,
		  int *luse_last, double *intrsct_x, double *intrsct_y,
		  int *avoid_pole_count, double *avoid_pole_offset)
{
  /*
    Intent(in): 
    int lbegin,             ! flag for first integration along this segment
    int lrevers             ! flag whether segment integrated in reverse
    double beglat, beglon,  ! beginning lat/lon endpoints for segment
    double endlat, endlon   ! ending    lat/lon endpoints for segment

    Intent(inout) :: 
    double *begseg          ! begin lat/lon of full segment

    intent(out): 
    int *location           ! address in destination array containing this segment
    int *lcoinc             ! flag segments which are entirely coincident with a grid line
    double *intrsct_lat, *intrsct_lon ! lat/lon coords of next intersect.
  */
  /* Local variables */
  long n, next_n, cell;
  long ioffset;

  int  loutside;             /* flags points outside grid */

  double lon1, lon2;         /* local longitude variables for segment */
  double lat1, lat2;         /* local latitude  variables for segment */
  double grdlon1, grdlon2;   /* local longitude variables for grid cell */
  double grdlat1, grdlat2;   /* local latitude  variables for grid cell */
  double vec1_lat, vec1_lon; /* vectors and cross products used */
  double vec2_lat, vec2_lon; /* during grid search */
  double cross_product; 
  double eps, offset;        /* small offset away from intersect */
  double s1, s2, determ;     /* variables used for linear solve to */
  double mat1 = 0, mat2, mat3 = 0, mat4, rhs1, rhs2;  /* find intersection */

  /* Initialize defaults, flags, etc. */

  *location    = -1;
  *lcoinc      = FALSE;
  *intrsct_lat = endlat;
  *intrsct_lon = endlon;

  if ( num_srch_cells == 0 ) return;

  if ( beglat > north_thresh || beglat < south_thresh )
    {
      if ( *lthresh ) *location = *last_loc;
      pole_intersection(location,
			intrsct_lat, intrsct_lon, lcoinc, lthresh,
			beglat, beglon, endlat, endlon, begseg, lrevers,
			num_srch_cells, srch_corners, srch_add,
			srch_corner_lat, srch_corner_lon,
			luse_last, intrsct_x, intrsct_y,
			avoid_pole_count, avoid_pole_offset);

      if ( *lthresh )
	{
          *last_loc = *location;
          *intrsct_lat_off = *intrsct_lat;
          *intrsct_lon_off = *intrsct_lon;
        }
      return;
    }

  loutside = FALSE;
  if ( lbegin )
    {
      lat1 = beglat;
      lon1 = beglon;
    }
  else
    {
      lat1 = *intrsct_lat_off;
      lon1 = *intrsct_lon_off;
    }

  lat2 = endlat;
  lon2 = endlon;
  if      ( (lon2-lon1) >  THREE*PIH ) lon2 -= PI2;
  else if ( (lon2-lon1) < -THREE*PIH ) lon2 += PI2;

  s1 = ZERO;

  /*
     Search for location of this segment in ocean grid using cross
     product method to determine whether a point is enclosed by a cell
  */
  while ( TRUE ) /* srch_loop */
    {
      /* If last segment crossed threshold, use that location */

      if ( *lthresh )
       {
         for ( cell = 0; cell < num_srch_cells; ++cell )
	   if ( srch_add[cell] == *last_loc )
	     {
               *location = *last_loc;
               eps = TINY;
               goto after_srch_loop;
	     }
       }

      /* Otherwise normal search algorithm */

      for ( cell = 0; cell < num_srch_cells; ++cell ) /* cell_loop  */
	{
	  ioffset = cell*srch_corners;
	  for ( n = 0; n < srch_corners; n++ ) /* corner_loop */
	    {
	      next_n = (n+1)%srch_corners;
	      /*
		Here we take the cross product of the vector making 
		up each cell side with the vector formed by the vertex
		and search point.  If all the cross products are 
		positive, the point is contained in the cell.
	      */
	      vec1_lat = srch_corner_lat[ioffset+next_n] - srch_corner_lat[ioffset+n];
	      vec1_lon = srch_corner_lon[ioffset+next_n] - srch_corner_lon[ioffset+n];
	      vec2_lat = lat1 - srch_corner_lat[ioffset+n];
	      vec2_lon = lon1 - srch_corner_lon[ioffset+n];

	      /* If endpoint coincident with vertex, offset the endpoint */

	      if ( IS_EQUAL(vec2_lat, 0) && IS_EQUAL(vec2_lon, 0) )
		{
		  lat1 += 1.e-10*(lat2-lat1);
		  lon1 += 1.e-10*(lon2-lon1);
		  vec2_lat = lat1 - srch_corner_lat[ioffset+n];
		  vec2_lon = lon1 - srch_corner_lon[ioffset+n];
		}

	      /* Check for 0,2pi crossings */

	      if      ( vec1_lon >  PI ) vec1_lon -= PI2;
	      else if ( vec1_lon < -PI ) vec1_lon += PI2;

	      if      ( vec2_lon >  PI ) vec2_lon -= PI2;
	      else if ( vec2_lon < -PI ) vec2_lon += PI2;

	      cross_product = vec1_lon*vec2_lat - vec2_lon*vec1_lat;

	      /*
	       If the cross product for a side is ZERO, the point 
                 lies exactly on the side or the side is degenerate
                 (ZERO length).  If degenerate, set the cross 
                 product to a positive number.  Otherwise perform 
                 another cross product between the side and the 
                 segment itself. 
	       If this cross product is also ZERO, the line is 
                 coincident with the cell boundary - perform the 
                 dot product and only choose the cell if the dot 
                 product is positive (parallel vs anti-parallel).
	      */
	      if ( IS_EQUAL(cross_product, 0) )
		{
		  if ( IS_NOT_EQUAL(vec1_lat, 0) || IS_NOT_EQUAL(vec1_lon, 0) )
		    {
		      vec2_lat = lat2 - lat1;
		      vec2_lon = lon2 - lon1;

		      if      ( vec2_lon >  PI ) vec2_lon -= PI2;
		      else if ( vec2_lon < -PI ) vec2_lon += PI2;

		      cross_product = vec1_lon*vec2_lat - vec2_lon*vec1_lat;
		    }
		  else
		    cross_product = ONE;

		  if ( IS_EQUAL(cross_product, 0) )
		    {
		      *lcoinc = TRUE;
		      cross_product = vec1_lon*vec2_lon + vec1_lat*vec2_lat;
		      if ( lrevers ) cross_product = -cross_product;
		    }
		}

	      /* If cross product is less than ZERO, this cell doesn't work */

	      if ( cross_product < ZERO ) break; /* corner_loop */

	    } /* corner_loop */

	  /* If cross products all positive, we found the location */

	  if ( n >= srch_corners )
	    {
	      *location = srch_add[cell];
	      /*
		If the beginning of this segment was outside the
		grid, invert the segment so the intersection found
		will be the first intersection with the grid
	      */
	      if ( loutside )
		{
		  lat2 = beglat;
		  lon2 = beglon;
		  *location = -1;
		  eps  = -TINY;
		}
	      else
		eps  = TINY;

	      goto after_srch_loop;
	    }

	  /* Otherwise move on to next cell */

	} /* cell_loop */

      /*
	If still no cell found, the point lies outside the grid.
	Take some baby steps along the segment to see if any
	part of the segment lies inside the grid.  
      */
      loutside = TRUE;
      s1 = s1 + BABY_STEP;
      lat1 = beglat + s1*(endlat - beglat);
      lon1 = beglon + s1*(lon2   - beglon);

      /* Reached the end of the segment and still outside the grid return no intersection */

      if ( s1 >= ONE ) return;

    } /* srch_loop */

 after_srch_loop:

  /*
    Now that a cell is found, search for the next intersection.
    Loop over sides of the cell to find intersection with side
    must check all sides for coincidences or intersections
  */

  ioffset = cell*srch_corners;

  for ( n = 0; n < srch_corners; ++n ) /* intrsct_loop */
    {
      next_n = (n+1)%srch_corners;

      grdlon1 = srch_corner_lon[ioffset+n];
      grdlon2 = srch_corner_lon[ioffset+next_n];
      grdlat1 = srch_corner_lat[ioffset+n];
      grdlat2 = srch_corner_lat[ioffset+next_n];

      /* Set up linear system to solve for intersection */

      mat1 = lat2 - lat1;
      mat2 = grdlat1 - grdlat2;
      mat3 = lon2 - lon1;
      mat4 = grdlon1 - grdlon2;
      rhs1 = grdlat1 - lat1;
      rhs2 = grdlon1 - lon1;

      if      ( mat3 >  PI ) mat3 -= PI2;
      else if ( mat3 < -PI ) mat3 += PI2;

      if      ( mat4 >  PI ) mat4 -= PI2;
      else if ( mat4 < -PI ) mat4 += PI2;

      if      ( rhs2 >  PI ) rhs2 -= PI2;
      else if ( rhs2 < -PI ) rhs2 += PI2;

      determ = mat1*mat4 - mat2*mat3;

      /*
         If the determinant is ZERO, the segments are either 
           parallel or coincident.  Coincidences were detected 
           above so do nothing.
         If the determinant is non-ZERO, solve for the linear 
           parameters s for the intersection point on each line 
           segment.
         If 0<s1,s2<1 then the segment intersects with this side.
           Return the point of intersection (adding a small
           number so the intersection is off the grid line).
      */
      if ( fabs(determ) > 1.e-30 )
	{
	  s1 = (rhs1*mat4 - mat2*rhs2)/determ;
	  s2 = (mat1*rhs2 - rhs1*mat3)/determ;

	  if ( s2 >= ZERO && s2 <= ONE && s1 >= ZERO && s1 <= ONE )
	    {
	      /*
		Recompute intersection based on full segment
		so intersections are consistent for both sweeps
	      */
	      if ( ! loutside )
		{
		  mat1 = lat2 - begseg[0];
		  mat3 = lon2 - begseg[1];
		  rhs1 = grdlat1 - begseg[0];
		  rhs2 = grdlon1 - begseg[1];
		}
	      else
		{
		  mat1 = begseg[0] - endlat;
		  mat3 = begseg[1] - endlon;
		  rhs1 = grdlat1 - endlat;
		  rhs2 = grdlon1 - endlon;
		}

	      if      ( mat3 >  PI ) mat3 -= PI2;
	      else if ( mat3 < -PI ) mat3 += PI2;

	      if      ( rhs2 > PI  ) rhs2 -= PI2;
	      else if ( rhs2 < -PI ) rhs2 += PI2;

	      determ = mat1*mat4 - mat2*mat3;

	      /*
		Sometimes due to roundoff, the previous 
		determinant is non-ZERO, but the lines
		are actually coincident.  If this is the
		case, skip the rest.
	      */
	      if ( IS_NOT_EQUAL(determ, 0) )
		{
		  s1 = (rhs1*mat4 - mat2*rhs2)/determ;
		  s2 = (mat1*rhs2 - rhs1*mat3)/determ;

		  offset = s1 + eps/determ;
		  if ( offset > ONE ) offset = ONE;

		  if ( ! loutside )
		    {
		      *intrsct_lat = begseg[0] + mat1*s1;
		      *intrsct_lon = begseg[1] + mat3*s1;
		      *intrsct_lat_off = begseg[0] + mat1*offset;
		      *intrsct_lon_off = begseg[1] + mat3*offset;
		    }
		  else
		    {
		      *intrsct_lat = endlat + mat1*s1;
		      *intrsct_lon = endlon + mat3*s1;
		      *intrsct_lat_off = endlat + mat1*offset;
		      *intrsct_lon_off = endlon + mat3*offset;
		    }
		  break; /* intrsct_loop */
		}
	    }
	}

      /* No intersection this side, move on to next side */

    } /* intrsct_loop */

  /*
     If the segment crosses a pole threshold, reset the intersection
     to be the threshold latitude.  Only check if this was not a
     threshold segment since sometimes coordinate transform can end
     up on other side of threshold again.
  */
  if ( *lthresh )
    {
      if ( *intrsct_lat < north_thresh || *intrsct_lat > south_thresh )
	*lthresh = FALSE;
    }
  else if ( lat1 > ZERO && *intrsct_lat > north_thresh )
    {
      *intrsct_lat = north_thresh + TINY;
      *intrsct_lat_off = north_thresh + eps*mat1;
      s1 = (*intrsct_lat - begseg[0])/mat1;
      *intrsct_lon     = begseg[1] + s1*mat3;
      *intrsct_lon_off = begseg[1] + (s1+eps)*mat3;
      *last_loc = *location;
      *lthresh = TRUE;
    }
  else if ( lat1 < ZERO && *intrsct_lat < south_thresh )
    {
      *intrsct_lat = south_thresh - TINY;
      *intrsct_lat_off = south_thresh + eps*mat1;
      s1 = (*intrsct_lat - begseg[0])/mat1;
      *intrsct_lon     = begseg[1] + s1*mat3;
      *intrsct_lon_off = begseg[1] + (s1+eps)*mat3;
      *last_loc = *location;
      *lthresh = TRUE;
    }

}  /* intersection */


/*
   This routine computes the line integral of the flux function 
   that results in the interpolation weights.  The line is defined
   by the input lat/lon of the endpoints.
*/
static
void line_integral(double *weights, double in_phi1, double in_phi2, 
		   double theta1, double theta2, double grid1_lon, double grid2_lon)
{
  /*
    Intent(in): 
    double in_phi1, in_phi2,     ! Longitude endpoints for the segment
    double theta1, theta2,       ! Latitude  endpoints for the segment
    double grid1_lon,            ! Reference coordinates for each
    double grid2_lon             ! Grid (to ensure correct 0,2pi interv.)

    Intent(out):
    double weights[6]            ! Line integral contribution to weights
  */

  /*  Local variables  */
  double dphi, sinth1, sinth2, costh1, costh2, fac;
  double phi1, phi2;
  double f1, f2, fint;

  /*  Weights for the general case based on a trapezoidal approx to the integrals. */

  sinth1 = sin(theta1);
  sinth2 = sin(theta2);
  costh1 = cos(theta1);
  costh2 = cos(theta2);

  dphi = in_phi1 - in_phi2;
  if      ( dphi >  PI ) dphi -= PI2;
  else if ( dphi < -PI ) dphi += PI2;
      
  dphi = HALF*dphi;

  /*
     The first weight is the area overlap integral. The second and
     fourth are second-order latitude gradient weights.
  */
  weights[0] = dphi*(sinth1 + sinth2);
  weights[1] = dphi*(costh1 + costh2 + (theta1*sinth1 + theta2*sinth2));
  weights[3] = weights[0];
  weights[4] = weights[1];

  /*
     The third and fifth weights are for the second-order phi gradient
     component.  Must be careful of longitude range.
  */
  f1 = HALF*(costh1*sinth1 + theta1);
  f2 = HALF*(costh2*sinth2 + theta2);

  phi1 = in_phi1 - grid1_lon;
  if      ( phi1 >  PI ) phi1 -= PI2;
  else if ( phi1 < -PI ) phi1 += PI2;

  phi2 = in_phi2 - grid1_lon;
  if      ( phi2 >  PI ) phi2 -= PI2;
  else if ( phi2 < -PI ) phi2 += PI2;

  if ( (phi2-phi1) <  PI && (phi2-phi1) > -PI )
    weights[2] = dphi*(phi1*f1 + phi2*f2);
  else
    {
      if ( phi1 > ZERO )
	fac = PI;
      else
	fac = -PI;

      fint = f1 + (f2-f1)*(fac-phi1)/fabs(dphi);
      weights[2] = HALF*phi1*(phi1-fac)*f1 -
	           HALF*phi2*(phi2+fac)*f2 +
	           HALF*fac*(phi1+phi2)*fint;
    }

  phi1 = in_phi1 - grid2_lon;
  if      ( phi1 >  PI ) phi1 -= PI2;
  else if ( phi1 < -PI ) phi1 += PI2;

  phi2 = in_phi2 - grid2_lon;
  if      ( phi2 >  PI ) phi2 -= PI2;
  else if ( phi2 < -PI ) phi2 += PI2;

  if ( (phi2-phi1) <  PI  && (phi2-phi1) > -PI )
    weights[5] = dphi*(phi1*f1 + phi2*f2);
  else
    {
      if ( phi1 > ZERO ) fac =  PI;
      else               fac = -PI;

      fint = f1 + (f2-f1)*(fac-phi1)/fabs(dphi);
      weights[5] = HALF*phi1*(phi1-fac)*f1 -
     	           HALF*phi2*(phi2+fac)*f2 +
	           HALF*fac*(phi1+phi2)*fint;
    }

}  /* line_integral */

static
void grid_store_init(grid_store_t *grid_store, long gridsize)
{
  long iblk;
  long blksize[] = {128, 256, 512, 1024, 2048, 4096, 8192};
  long nblks = sizeof(blksize)/sizeof(long);
  long nblocks;

  for ( iblk = nblks-1; iblk >= 0; --iblk )
    if ( gridsize/blksize[iblk] > 99 ) break;

  if ( iblk < 0 ) iblk = 0;

  /* grid_store->blk_size = BLK_SIZE; */
  grid_store->blk_size = blksize[iblk];
  grid_store->max_size = gridsize;

  grid_store->nblocks = grid_store->max_size/grid_store->blk_size;
  if ( grid_store->max_size%grid_store->blk_size > 0 ) grid_store->nblocks++;

  if ( cdoVerbose )
    fprintf(stdout, "blksize = %d  lastblksize = %d  max_size = %d  nblocks = %d\n", 
	    grid_store->blk_size, grid_store->max_size%grid_store->blk_size, 
	    grid_store->max_size, grid_store->nblocks);

  grid_store->blksize = (int *) malloc(grid_store->nblocks*sizeof(int));
  grid_store->nlayers = (int *) malloc(grid_store->nblocks*sizeof(int));
  grid_store->layers  = (grid_layer_t **) malloc(grid_store->nblocks*sizeof(grid_layer_t *));

  nblocks = grid_store->nblocks;
  for ( iblk = 0; iblk < nblocks; ++iblk )
    {
      grid_store->blksize[iblk] = grid_store->blk_size;
      grid_store->nlayers[iblk] = 0;
      grid_store->layers[iblk]  = NULL;
    }
  if ( grid_store->max_size%grid_store->blk_size > 0 )
    grid_store->blksize[grid_store->nblocks-1] = grid_store->max_size%grid_store->blk_size;
}

static
void grid_store_delete(grid_store_t *grid_store)
{
  grid_layer_t *grid_layer, *grid_layer_f;
  long ilayer;
  long i, j;
  long iblk;

  for ( iblk = 0; iblk < grid_store->nblocks; ++iblk )
    {
      j = 0;
      grid_layer = grid_store->layers[iblk];
      for ( ilayer = 0; ilayer < grid_store->nlayers[iblk]; ++ilayer )
	{
	  if ( cdoVerbose )
	    {
	      for ( i = 0; i < grid_store->blksize[iblk]; ++i )
		if ( grid_layer->grid2_link[i] != -1 ) j++;
	    }
	      
	  grid_layer_f = grid_layer;
	  free(grid_layer->grid2_link);
	  grid_layer = grid_layer->next;
	  free(grid_layer_f);
	}

      if ( cdoVerbose )
	{
	  fprintf(stderr, "block = %ld nlayers = %d  allocated = %d  used = %ld\n",
		  iblk+1, grid_store->nlayers[iblk], 
		  grid_store->nlayers[iblk]*grid_store->blksize[iblk], j);
	}
    }

  free(grid_store->blksize);
  free(grid_store->layers);
  free(grid_store->nlayers);  
}

/*
    This routine stores the address and weight for this link in the appropriate 
    address and weight arrays and resizes those arrays if necessary.
*/
static
void store_link_cnsrv_fast(remapvars_t *rv, long add1, long add2, double *weights, grid_store_t *grid_store)
{
  /*
    Input variables:
    int  add1         ! address on grid1
    int  add2         ! address on grid2
    double weights[6] ! array of remapping weights for this link
  */
  /* Local variables */
  long nlink; /* link index */
  long ilayer, i, iblk, iadd2;
  long nlayer, blksize;
  int lstore_link;
  grid_layer_t *grid_layer, **grid_layer2;

  /*  If all weights are ZERO, do not bother storing the link */

  if ( IS_EQUAL(weights[0], 0) && IS_EQUAL(weights[1], 0) && IS_EQUAL(weights[2], 0) &&
       IS_EQUAL(weights[3], 0) && IS_EQUAL(weights[4], 0) && IS_EQUAL(weights[5], 0) ) return;

  /* If the link already exists, add the weight to the current weight arrays */

  iblk  = BLK_NUM(add2);
  iadd2 = BLK_IDX(add2);

  lstore_link = FALSE;
  grid_layer2 = &grid_store->layers[iblk];
  nlayer = grid_store->nlayers[iblk];
  for ( ilayer = 0; ilayer < nlayer; ++ilayer )
    {
      grid_layer = *grid_layer2;
      nlink = grid_layer->grid2_link[iadd2];
      if ( nlink == -1 )
	{
	  break;
	}
      else if ( add1 == rv->grid1_add[nlink] )
	{
	  lstore_link = TRUE;
	  break;
	}
      grid_layer2 = &(*grid_layer2)->next;
    }

  if ( lstore_link )
    {
      rv->wts[3*nlink  ] += weights[0];
      rv->wts[3*nlink+1] += weights[1];
      rv->wts[3*nlink+2] += weights[2];
	      
      return;
    }

  /*
     If the link does not yet exist, increment number of links and 
     check to see if remap arrays need to be increased to accomodate 
     the new link. Then store the link.
  */
  nlink = rv->num_links;

  if ( ilayer < grid_store->nlayers[iblk] )
    {
      grid_layer->grid2_link[iadd2] = nlink;
    }
  else
    {
      grid_layer = (grid_layer_t *) malloc(sizeof(grid_layer_t));
      grid_layer->next = NULL;
      grid_layer->grid2_link = (int *) malloc(grid_store->blksize[iblk]*sizeof(int));

      blksize = grid_store->blksize[iblk];
      for ( i = 0; i < blksize; ++i )
	grid_layer->grid2_link[i] = -1;

      grid_layer->grid2_link[iadd2] = nlink;
      *grid_layer2 = grid_layer;
      grid_store->nlayers[iblk]++;
    }

  rv->num_links++;
  if ( rv->num_links >= rv->max_links )
    resize_remap_vars(rv, rv->resize_increment);

  rv->grid1_add[nlink] = add1;
  rv->grid2_add[nlink] = add2;

  rv->wts[3*nlink  ] = weights[0];
  rv->wts[3*nlink+1] = weights[1];
  rv->wts[3*nlink+2] = weights[2];

}  /* store_link_cnsrv_fast */


/*
    This routine stores the address and weight for this link in the appropriate 
    address and weight arrays and resizes those arrays if necessary.
*/
static
void store_link_cnsrv(remapvars_t *rv, long add1, long add2, double *restrict weights,
		      int *link_add1[2], int *link_add2[2])
{
  /*
    Input variables:
    int  add1         ! address on grid1
    int  add2         ! address on grid2
    double weights[6] ! array of remapping weights for this link
  */
  /* Local variables */
  long nlink, min_link, max_link; /* link index */

  /*  If all weights are ZERO, do not bother storing the link */

  if ( IS_EQUAL(weights[0], 0) && IS_EQUAL(weights[1], 0) && IS_EQUAL(weights[2], 0) &&
       IS_EQUAL(weights[3], 0) && IS_EQUAL(weights[4], 0) && IS_EQUAL(weights[5], 0) ) return;

  /*  Restrict the range of links to search for existing links */

  min_link = MIN(link_add1[0][add1], link_add2[0][add2]);
  max_link = MAX(link_add1[1][add1], link_add2[1][add2]);
  if ( min_link == -1 )
    {
      min_link = 0;
      max_link = -1;
    }

  /* If the link already exists, add the weight to the current weight arrays */

#if defined(SX)
#define STRIPED 1
#if STRIPED
#define STRIPLENGTH 4096
  {
    long ilink = max_link + 1;
    long strip, estrip;
    nlink = 0;
    for ( strip=min_link; strip <= max_link; strip+=STRIPLENGTH )
      {
	estrip = MIN(max_link-strip+1, STRIPLENGTH);
	for ( nlink = 0; nlink < estrip; ++nlink )
	  {
	    if ( add2 == rv->grid2_add[strip+nlink] &&
		 add1 == rv->grid1_add[strip+nlink] )
	      ilink = strip + nlink;
	  }
	if (ilink != (max_link + 1)) break;
      }
    nlink += strip;
    if (ilink != (max_link + 1)) nlink = ilink;
  }
#else
  {
    long ilink = max_link + 1;
    for ( nlink = min_link; nlink <= max_link; ++nlink )
      {
	if ( add2 == rv->grid2_add[nlink] )
	  if ( add1 == rv->grid1_add[nlink] ) ilink = nlink;
      }
    if ( ilink != (max_link + 1) ) nlink = ilink;
  }
#endif
#else
  for ( nlink = min_link; nlink <= max_link; ++nlink )
    {
      if ( add2 == rv->grid2_add[nlink] )
	if ( add1 == rv->grid1_add[nlink] ) break;
    }
#endif

  if ( nlink <= max_link )
    {
      rv->wts[3*nlink  ] += weights[0];
      rv->wts[3*nlink+1] += weights[1];
      rv->wts[3*nlink+2] += weights[2];

      return;
    }

  /*
     If the link does not yet exist, increment number of links and 
     check to see if remap arrays need to be increased to accomodate 
     the new link. Then store the link.
  */
  nlink = rv->num_links;

  rv->num_links++;
  if ( rv->num_links >= rv->max_links )
    resize_remap_vars(rv, rv->resize_increment);

  rv->grid1_add[nlink] = add1;
  rv->grid2_add[nlink] = add2;

  rv->wts[3*nlink  ] = weights[0];
  rv->wts[3*nlink+1] = weights[1];
  rv->wts[3*nlink+2] = weights[2];

  if ( link_add1[0][add1] == -1 ) link_add1[0][add1] = (int)nlink;
  if ( link_add2[0][add2] == -1 ) link_add2[0][add2] = (int)nlink;
  link_add1[1][add1] = (int)nlink;
  link_add2[1][add2] = (int)nlink;

}  /* store_link_cnsrv */

static
long get_srch_cells(long grid1_add, long nbins, int *bin_addr1, int *bin_addr2,
		    restr_t *grid1_bound_box, restr_t *grid2_bound_box, long grid2_size, int *srch_add)
{
  long num_srch_cells;  /* num cells in restricted search arrays   */
  long min_add;         /* addresses for restricting search of     */
  long max_add;         /* destination grid                        */
  long n, n2;           /* generic counters                        */
  long grid2_add;       /* current linear address for grid2 cell   */
  long grid1_addm4, grid2_addm4;
  int  lmask;
  restr_t bound_box_lat1, bound_box_lat2, bound_box_lon1, bound_box_lon2;

  /* Restrict searches first using search bins */

  min_add = grid2_size - 1;
  max_add = 0;

  for ( n = 0; n < nbins; ++n )
    {
      n2 = n<<1;
      if ( grid1_add >= bin_addr1[n2] && grid1_add <= bin_addr1[n2+1] )
	{
	  if ( bin_addr2[n2  ] < min_add ) min_add = bin_addr2[n2  ];
	  if ( bin_addr2[n2+1] > max_add ) max_add = bin_addr2[n2+1];
	}
    }

  /* Further restrict searches using bounding boxes */

  grid1_addm4 = grid1_add<<2;
  bound_box_lat1 = grid1_bound_box[grid1_addm4  ];
  bound_box_lat2 = grid1_bound_box[grid1_addm4+1];
  bound_box_lon1 = grid1_bound_box[grid1_addm4+2];
  bound_box_lon2 = grid1_bound_box[grid1_addm4+3];

  num_srch_cells = 0;
  for ( grid2_add = min_add; grid2_add <= max_add; ++grid2_add )
    {
      grid2_addm4 = grid2_add<<2;
      lmask = (grid2_bound_box[grid2_addm4  ] <= bound_box_lat2)  &&
	      (grid2_bound_box[grid2_addm4+1] >= bound_box_lat1)  &&
	      (grid2_bound_box[grid2_addm4+2] <= bound_box_lon2)  &&
	      (grid2_bound_box[grid2_addm4+3] >= bound_box_lon1);

      if ( lmask )
	{
	  srch_add[num_srch_cells] = grid2_add;
	  num_srch_cells++;
	}
    }

  return (num_srch_cells);
}

#if defined(_OPENMP)
static
void *alloc_srch_corner(size_t n)
{
  return (malloc(n*sizeof(double)));
}

static
void free_srch_corner(void *p)
{
  free(p);
}
#endif

/*
  -----------------------------------------------------------------------

   This routine traces the perimeters of every grid cell on each
   grid checking for intersections with the other grid and computing
   line integrals for each subsegment.

  -----------------------------------------------------------------------
*/
void remap_conserv(remapgrid_t *rg, remapvars_t *rv)
{
  /* local variables */

  int lcheck = TRUE;

  long ioffset;
  long max_subseg = 100000; /* max number of subsegments per segment to prevent infinite loop */
                            /* 1000 is too small!!! */
  long grid1_size;
  long grid2_size;
  long grid1_corners;
  long grid2_corners;
  long grid1_add;       /* current linear address for grid1 cell   */
  long grid2_add;       /* current linear address for grid2 cell   */
  long n, n3, k;        /* generic counters                        */
  long corner;          /* corner of cell that segment starts from */
  long next_corn;       /* corner of cell that segment ends on     */
  long nbins, num_links;
  long num_subseg;      /* number of subsegments                   */

  int lcoinc;           /* flag for coincident segments            */
  int lrevers;          /* flag for reversing direction of segment */
  int lbegin;           /* flag for first integration of a segment */

  double intrsct_lat, intrsct_lon;         /* lat/lon of next intersect  */
  double beglat, endlat, beglon, endlon;   /* endpoints of current seg.  */
  double norm_factor = 0;                  /* factor for normalizing wts */

  double *grid2_centroid_lat, *grid2_centroid_lon;   /* centroid coords  */
  double *grid1_centroid_lat, *grid1_centroid_lon;   /* on each grid     */

  double begseg[2];         /* begin lat/lon for full segment */
  double weights[6];        /* local wgt array */

  long    max_srch_cells;   /* num cells in restricted search arrays  */
  long    num_srch_cells;   /* num cells in restricted search arrays  */
  long    srch_corners;     /* num of corners of srch cells           */
  long    nsrch_corners;
  int    *srch_add;         /* global address of cells in srch arrays */
#if defined(_OPENMP)
  int **srch_add2;
  int ompthID, i;
#endif
  double *srch_corner_lat;  /* lat of each corner of srch cells */
  double *srch_corner_lon;  /* lon of each corner of srch cells */

  int *link_add1[2];        /* min,max link add to restrict search */
  int *link_add2[2];        /* min,max link add to restrict search */

  /* Intersection */
  int last_loc = -1;        /* save location when crossing threshold  */
  int lthresh = FALSE;      /* flags segments crossing threshold bndy */
  double intrsct_lat_off = 0, intrsct_lon_off = 0; /* lat/lon coords offset for next search */

  /* Pole_intersection */
  /* Save last intersection to avoid roundoff during coord transformation */
  int luse_last = FALSE;
  double intrsct_x, intrsct_y;      /* x,y for intersection */
  /* Variables necessary if segment manages to hit pole */
  int avoid_pole_count = 0;         /* count attempts to avoid pole  */
  double avoid_pole_offset = TINY;  /* endpoint offset to avoid pole */
  grid_store_t *grid_store = NULL;
  double findex = 0;

  progressInit();

  nbins = rg->num_srch_bins;

  if ( rg->store_link_fast )
    {
      grid_store = (grid_store_t *) malloc(sizeof(grid_store_t));
      grid_store_init(grid_store, rg->grid2_size);
    }

  if ( cdoVerbose )
    {
      cdoPrint("north_thresh: %g", north_thresh);
      cdoPrint("south_thresh: %g", south_thresh);
    }

  if ( cdoTimer ) timer_start(timer_remap_con);

  grid1_size = rg->grid1_size;
  grid2_size = rg->grid2_size;

  grid1_corners = rg->grid1_corners;
  grid2_corners = rg->grid2_corners;

  if ( ! rg->store_link_fast )
    {
      link_add1[0] = (int *) malloc(grid1_size*sizeof(int));
      link_add1[1] = (int *) malloc(grid1_size*sizeof(int));
      link_add2[0] = (int *) malloc(grid2_size*sizeof(int));
      link_add2[1] = (int *) malloc(grid2_size*sizeof(int));

#if defined(SX)
#pragma vdir nodep
#endif
      for ( n = 0; n < grid1_size; ++n )
	{
	  link_add1[0][n] = -1;
	  link_add1[1][n] = -1;
	}

#if defined(SX)
#pragma vdir nodep
#endif
      for ( n = 0; n < grid2_size; ++n )
	{
	  link_add2[0][n] = -1;
	  link_add2[1][n] = -1;
	}
    }

  /* Initialize centroid arrays */

  grid1_centroid_lat = (double *) malloc(grid1_size*sizeof(double));
  grid1_centroid_lon = (double *) malloc(grid1_size*sizeof(double));
  grid2_centroid_lat = (double *) malloc(grid2_size*sizeof(double));
  grid2_centroid_lon = (double *) malloc(grid2_size*sizeof(double));

  for ( n = 0; n < grid1_size; ++n )
    {
      grid1_centroid_lat[n] = 0;
      grid1_centroid_lon[n] = 0;
    }

  for ( n = 0; n < grid2_size; ++n )
    {
      grid2_centroid_lat[n] = 0;
      grid2_centroid_lon[n] = 0;
    }

  /*  Integrate around each cell on grid1 */

#if defined(_OPENMP)
  srch_add2 = (int **) malloc(ompNumThreads*sizeof(int *));
  for ( i = 0; i < ompNumThreads; ++i )
    srch_add2[i] = (int *) malloc(grid2_size*sizeof(int));
#else
  srch_add = (int *) malloc(grid2_size*sizeof(int));
#endif

  srch_corners    = grid2_corners;
  max_srch_cells  = 0;
  srch_corner_lat = NULL;
  srch_corner_lon = NULL;

  if ( cdoTimer ) timer_start(timer_remap_con_l1);

#if defined(_OPENMP)
#pragma omp parallel for default(none) \
  shared(ompNumThreads, cdoTimer, nbins, grid1_centroid_lon, grid1_centroid_lat, \
         grid_store, link_add1, link_add2, rv, cdoVerbose, max_subseg, \
	 grid1_corners,	srch_corners, rg, grid2_size, grid1_size, srch_add2, findex) \
  private(ompthID, srch_add, n, k, num_srch_cells, max_srch_cells, \
	  grid1_add, grid2_add, ioffset, nsrch_corners, corner, next_corn, beglat, beglon, \
	  endlat, endlon, lrevers, begseg, lbegin, num_subseg, srch_corner_lat, srch_corner_lon, \
	  weights, intrsct_lat, intrsct_lon, intrsct_lat_off, intrsct_lon_off, intrsct_x, intrsct_y, \
	  last_loc, lcoinc, lthresh, luse_last, avoid_pole_count, avoid_pole_offset)
#endif
  for ( grid1_add = 0; grid1_add < grid1_size; ++grid1_add )
    {
      int lprogress = 1;
#if defined(_OPENMP)
      ompthID = omp_get_thread_num();
      srch_add = srch_add2[ompthID];
      if ( ompthID != 0 ) lprogress = 0;
#endif
#if defined(_OPENMP)
#pragma omp atomic
#endif
      findex++;
      if ( lprogress ) progressStatus(0, 0.5, findex/grid1_size);

      lthresh   = FALSE;
      luse_last = FALSE;
      avoid_pole_count  = 0;
      avoid_pole_offset = TINY;

      /* Get search cells */
      num_srch_cells = get_srch_cells(grid1_add, nbins, rg->bin_addr1, rg->bin_addr2,
				      rg->grid1_bound_box, rg->grid2_bound_box , grid2_size, srch_add);

      if ( num_srch_cells == 0 ) continue;

      /* Create search arrays */

#if defined(_OPENMP)
	  srch_corner_lat = (double *) alloc_srch_corner(srch_corners*num_srch_cells);
	  srch_corner_lon = (double *) alloc_srch_corner(srch_corners*num_srch_cells);
#else
      if ( num_srch_cells > max_srch_cells )
	{
	  max_srch_cells = num_srch_cells;
	  srch_corner_lat = (double *) realloc(srch_corner_lat, srch_corners*num_srch_cells*sizeof(double));
	  srch_corner_lon = (double *) realloc(srch_corner_lon, srch_corners*num_srch_cells*sizeof(double));
	}
#endif

      /* gather1 */
      for ( n = 0; n < num_srch_cells; ++n )
	{
	  grid2_add = srch_add[n];
	  ioffset = grid2_add*srch_corners;

	  nsrch_corners = n*srch_corners;
	  for ( k = 0; k < srch_corners; k++ )
	    {
	      srch_corner_lat[nsrch_corners+k] = rg->grid2_corner_lat[ioffset+k];
	      srch_corner_lon[nsrch_corners+k] = rg->grid2_corner_lon[ioffset+k];
	    }
	}

      /* Integrate around this cell */

      ioffset = grid1_add*grid1_corners;

      for ( corner = 0; corner < grid1_corners; ++corner )
	{
          next_corn = (corner+1)%grid1_corners;

          /* Define endpoints of the current segment */

          beglat = rg->grid1_corner_lat[ioffset+corner];
          beglon = rg->grid1_corner_lon[ioffset+corner];
          endlat = rg->grid1_corner_lat[ioffset+next_corn];
          endlon = rg->grid1_corner_lon[ioffset+next_corn];
          lrevers = FALSE;

	  /*  To ensure exact path taken during both sweeps, always integrate segments in the same direction (SW to NE). */
          if ( (endlat < beglat) || (IS_EQUAL(endlat, beglat) && endlon < beglon) )
	    {
	      beglat = rg->grid1_corner_lat[ioffset+next_corn];
	      beglon = rg->grid1_corner_lon[ioffset+next_corn];
	      endlat = rg->grid1_corner_lat[ioffset+corner];
	      endlon = rg->grid1_corner_lon[ioffset+corner];
	      lrevers = TRUE;
	    }

          begseg[0] = beglat;
          begseg[1] = beglon;
          lbegin = TRUE;

          /*
	    If this is a constant-longitude segment, skip the rest 
	    since the line integral contribution will be ZERO.
          */
          if ( IS_NOT_EQUAL(endlon, beglon) )
	    {
	      num_subseg = 0;
	      /*
		Integrate along this segment, detecting intersections 
		and computing the line integral for each sub-segment
	      */
	      while ( IS_NOT_EQUAL(beglat, endlat) || IS_NOT_EQUAL(beglon, endlon) )
		{
		  /*  Prevent infinite loops if integration gets stuck near cell or threshold boundary */
		  num_subseg++;
		  if ( num_subseg >= max_subseg )
		    cdoAbort("Integration stalled: num_subseg exceeded limit (grid1[%d]: lon1=%g lon2=%g lat1=%g lat2=%g)!",
			     grid1_add, beglon, endlon, beglat, endlat);

		  /* Uwe Schulzweida: skip very small regions */
		  if ( num_subseg%1000 == 0 )
		    {
		      if ( fabs(beglat-endlat) < 1.e-10 || fabs(beglon-endlon) < 1.e-10 )
			{
			  if ( cdoVerbose )
			    cdoPrint("Skip very small region (grid1[%d]): lon=%g dlon=%g lat=%g dlat=%g",
				     grid1_add, beglon, endlon-beglon, beglat, endlat-beglat);
			  break;
			}
		    }

		  /* Find next intersection of this segment with a gridline on grid 2. */

		  intersection(&grid2_add, &intrsct_lat, &intrsct_lon, &lcoinc,
			       beglat, beglon, endlat, endlon, begseg, 
			       lbegin, lrevers,
                               num_srch_cells, srch_corners, srch_add,
			       srch_corner_lat, srch_corner_lon,
			       &last_loc, &lthresh, &intrsct_lat_off, &intrsct_lon_off,
			       &luse_last, &intrsct_x, &intrsct_y,
			       &avoid_pole_count, &avoid_pole_offset);

		  lbegin = FALSE;

		  /* Compute line integral for this subsegment. */

		  if ( grid2_add != -1 )
		    line_integral(weights, beglon, intrsct_lon, beglat, intrsct_lat,
				  rg->grid1_center_lon[grid1_add], rg->grid2_center_lon[grid2_add]);
		  else
		    line_integral(weights, beglon, intrsct_lon, beglat, intrsct_lat,
				  rg->grid1_center_lon[grid1_add], rg->grid1_center_lon[grid1_add]);

		  /* If integrating in reverse order, change sign of weights */

		  if ( lrevers ) for ( k = 0; k < 6; ++k ) weights[k] = -weights[k];

		  /*
		    Store the appropriate addresses and weights. 
		    Also add contributions to cell areas and centroids.
		  */
		  if ( grid2_add != -1 )
		    if ( rg->grid1_mask[grid1_add] )
		      {
#if defined(_OPENMP)
#pragma omp critical
#endif
			{
			  if ( rg->store_link_fast )
			    store_link_cnsrv_fast(rv, grid1_add, grid2_add, weights, grid_store);
			  else
			    store_link_cnsrv(rv, grid1_add, grid2_add, weights, link_add1, link_add2);

			  rg->grid2_frac[grid2_add] += weights[3];
			}
			rg->grid1_frac[grid1_add] += weights[0];
		      }

		  rg->grid1_area[grid1_add]     += weights[0];
		  grid1_centroid_lat[grid1_add] += weights[1];
		  grid1_centroid_lon[grid1_add] += weights[2];

		  /* Reset beglat and beglon for next subsegment. */
		  beglat = intrsct_lat;
		  beglon = intrsct_lon;
		}
	    }
          /* End of segment */
        }
#if defined(_OPENMP)
      free_srch_corner(srch_corner_lat);
      free_srch_corner(srch_corner_lon);
#endif
    }

  if ( cdoTimer ) timer_stop(timer_remap_con_l1);

  /* Finished with all cells: deallocate search arrays */

#if ! defined(_OPENMP)
  if ( srch_corner_lon ) free(srch_corner_lon);
  if ( srch_corner_lat ) free(srch_corner_lat);
#endif

#if defined(_OPENMP)
  for ( i = 0; i < ompNumThreads; ++i )
    free(srch_add2[i]);

  free(srch_add2);
#else
  free(srch_add);
#endif

  /* Integrate around each cell on grid2 */

#if defined(_OPENMP)
  srch_add2 = (int **) malloc(ompNumThreads*sizeof(int *));
  for ( i = 0; i < ompNumThreads; ++i )
    srch_add2[i] = (int *) malloc(grid1_size*sizeof(int));
#else
  srch_add = (int *) malloc(grid1_size*sizeof(int));
#endif

  srch_corners    = grid1_corners;
  max_srch_cells  = 0;
  srch_corner_lat = NULL;
  srch_corner_lon = NULL;

  if ( cdoTimer ) timer_start(timer_remap_con_l2);

  findex = 0;

#if defined(_OPENMP)
#pragma omp parallel for default(none) \
  shared(ompNumThreads, cdoTimer, nbins, grid2_centroid_lon, grid2_centroid_lat, \
         grid_store, link_add1, link_add2, rv, cdoVerbose, max_subseg, \
	 grid2_corners, srch_corners, rg, grid2_size, grid1_size, srch_add2, findex) \
  private(ompthID, srch_add, n, k, num_srch_cells, max_srch_cells, \
	  grid1_add, grid2_add, ioffset, nsrch_corners, corner, next_corn, beglat, beglon, \
	  endlat, endlon, lrevers, begseg, lbegin, num_subseg, srch_corner_lat, srch_corner_lon, \
	  weights, intrsct_lat, intrsct_lon, intrsct_lat_off, intrsct_lon_off, intrsct_x, intrsct_y, \
	  last_loc, lcoinc, lthresh, luse_last, avoid_pole_count, avoid_pole_offset)
#endif
  for ( grid2_add = 0; grid2_add < grid2_size; ++grid2_add )
    {
      int lprogress = 1;
#if defined(_OPENMP)
      ompthID = omp_get_thread_num();
      srch_add = srch_add2[ompthID];
      if ( ompthID != 0 ) lprogress = 0;
#endif
#if defined(_OPENMP)
#pragma omp atomic
#endif
      findex++;
      if ( lprogress ) progressStatus(0.5, 0.5, findex/grid2_size);

      lthresh   = FALSE;
      luse_last = FALSE;
      avoid_pole_count  = 0;
      avoid_pole_offset = TINY;

      /* Get search cells */
      num_srch_cells = get_srch_cells(grid2_add, nbins, rg->bin_addr2, rg->bin_addr1,
				      rg->grid2_bound_box, rg->grid1_bound_box , grid1_size, srch_add);

      if ( num_srch_cells == 0 ) continue;

      /* Create search arrays */
      
#if defined(_OPENMP)
	  srch_corner_lat = (double *) alloc_srch_corner(srch_corners*num_srch_cells);
	  srch_corner_lon = (double *) alloc_srch_corner(srch_corners*num_srch_cells);
#else
      if ( num_srch_cells > max_srch_cells )
	{
	  max_srch_cells = num_srch_cells;
	  srch_corner_lat = (double *) realloc(srch_corner_lat, srch_corners*num_srch_cells*sizeof(double));
	  srch_corner_lon = (double *) realloc(srch_corner_lon, srch_corners*num_srch_cells*sizeof(double));
	}
#endif

      /* gather2 */
      for ( n = 0; n < num_srch_cells; ++n )
	{
	  grid1_add = srch_add[n];
	  ioffset = grid1_add*srch_corners;

	  nsrch_corners = n*srch_corners;
	  for ( k = 0; k < srch_corners; ++k )
	    {
	      srch_corner_lat[nsrch_corners+k] = rg->grid1_corner_lat[ioffset+k];
	      srch_corner_lon[nsrch_corners+k] = rg->grid1_corner_lon[ioffset+k];
	    }
	}

      /* Integrate around this cell */

      ioffset = grid2_add*grid2_corners;

      for ( corner = 0; corner < grid2_corners; ++corner )
	{
          next_corn = (corner+1)%grid2_corners;

          beglat = rg->grid2_corner_lat[ioffset+corner];
          beglon = rg->grid2_corner_lon[ioffset+corner];
          endlat = rg->grid2_corner_lat[ioffset+next_corn];
          endlon = rg->grid2_corner_lon[ioffset+next_corn];
          lrevers = FALSE;

	  /* To ensure exact path taken during both sweeps, always integrate in the same direction */
          if ( (endlat < beglat) || (IS_EQUAL(endlat, beglat) && endlon < beglon) )
	    {
	      beglat = rg->grid2_corner_lat[ioffset+next_corn];
	      beglon = rg->grid2_corner_lon[ioffset+next_corn];
	      endlat = rg->grid2_corner_lat[ioffset+corner];
	      endlon = rg->grid2_corner_lon[ioffset+corner];
	      lrevers = TRUE;
	    }

          begseg[0] = beglat;
          begseg[1] = beglon;
          lbegin = TRUE;

          /*
	    If this is a constant-longitude segment, skip the rest 
	    since the line integral contribution will be ZERO.
          */
          if ( IS_NOT_EQUAL(endlon, beglon) )
	    {
	      num_subseg = 0;
	      /*
		Integrate along this segment, detecting intersections 
		and computing the line integral for each sub-segment
	      */
	      while ( IS_NOT_EQUAL(beglat, endlat) || IS_NOT_EQUAL(beglon, endlon) )
		{
		  /*  Prevent infinite loops if integration gets stuck near cell or threshold boundary */
		  num_subseg++;
		  if ( num_subseg >= max_subseg )
		    cdoAbort("Integration stalled: num_subseg exceeded limit (grid2[%d]: lon1=%g lon2=%g lat1=%g lat2=%g)!",
			     grid2_add, beglon, endlon, beglat, endlat);

		  /* Uwe Schulzweida: skip very small regions */
		  if ( num_subseg%1000 == 0 )
		    {
		      if ( fabs(beglat-endlat) < 1.e-10 || fabs(beglon-endlon) < 1.e-10 )
			{
			  if ( cdoVerbose )
			    cdoPrint("Skip very small region (grid2[%d]): lon=%g dlon=%g lat=%g dlat=%g",
				     grid2_add, beglon, endlon-beglon, beglat, endlat-beglat);
			  break;
			}
		    }

		  /* Find next intersection of this segment with a gridline on grid 2. */

		  intersection(&grid1_add, &intrsct_lat, &intrsct_lon, &lcoinc,
			       beglat, beglon, endlat, endlon, begseg,
			       lbegin, lrevers,
                               num_srch_cells, srch_corners, srch_add,
			       srch_corner_lat, srch_corner_lon,
			       &last_loc, &lthresh, &intrsct_lat_off, &intrsct_lon_off,
			       &luse_last, &intrsct_x, &intrsct_y,
			       &avoid_pole_count, &avoid_pole_offset);

		  lbegin = FALSE;

		  /* Compute line integral for this subsegment. */

		  if ( grid1_add != -1 )
		    line_integral(weights, beglon, intrsct_lon, beglat, intrsct_lat,
				  rg->grid1_center_lon[grid1_add], rg->grid2_center_lon[grid2_add]);
		  else
		    line_integral(weights, beglon, intrsct_lon, beglat, intrsct_lat,
				  rg->grid2_center_lon[grid2_add], rg->grid2_center_lon[grid2_add]);

		  /* If integrating in reverse order, change sign of weights */

		  if ( lrevers ) for ( k = 0; k < 6; ++k ) weights[k] = -weights[k];

		  /*
		    Store the appropriate addresses and weights. 
		    Also add contributions to cell areas and centroids.
		    If there is a coincidence, do not store weights
		    because they have been captured in the previous loop.
		    The grid1 mask is the master mask
		  */
		  if ( ! lcoinc && grid1_add != -1 )
		    if ( rg->grid1_mask[grid1_add] )
		      {
#if defined(_OPENMP)
#pragma omp critical
#endif
			{
			  if ( rg->store_link_fast )
			    store_link_cnsrv_fast(rv, grid1_add, grid2_add, weights, grid_store);
			  else
			    store_link_cnsrv(rv, grid1_add, grid2_add, weights, link_add1, link_add2);

			  rg->grid1_frac[grid1_add] += weights[0];
			}
			rg->grid2_frac[grid2_add] += weights[3];
		      }

		  rg->grid2_area[grid2_add]     += weights[3];
		  grid2_centroid_lat[grid2_add] += weights[4];
		  grid2_centroid_lon[grid2_add] += weights[5];

		  /* Reset beglat and beglon for next subsegment. */
		  beglat = intrsct_lat;
		  beglon = intrsct_lon;
		}
	    }
          /* End of segment */
	}
#if defined(_OPENMP)
      free_srch_corner(srch_corner_lat);
      free_srch_corner(srch_corner_lon);
#endif
    }

  if ( cdoTimer ) timer_stop(timer_remap_con_l2);

  /* Finished with all cells: deallocate search arrays */

#if ! defined(_OPENMP)
  if ( srch_corner_lon ) free(srch_corner_lon);
  if ( srch_corner_lat ) free(srch_corner_lat);
#endif

#if defined(_OPENMP)
  for ( i = 0; i < ompNumThreads; ++i )
    free(srch_add2[i]);

  free(srch_add2);
#else
  free(srch_add);
#endif

  /*
     Correct for situations where N/S pole not explicitly included in
     grid (i.e. as a grid corner point). If pole is missing from only
     one grid, need to correct only the area and centroid of that 
     grid.  If missing from both, do complete weight calculation.
  */

  /* North Pole */
  weights[0] =  PI2;
  weights[1] =  PI*PI;
  weights[2] =  ZERO;
  weights[3] =  PI2;
  weights[4] =  PI*PI;
  weights[5] =  ZERO;

  grid1_add = -1;
  /* pole_loop1 */
  for ( n = 0; n < grid1_size; ++n )
    if ( rg->grid1_area[n] < -THREE*PIH && rg->grid1_center_lat[n] > ZERO )
      {
	grid1_add = n;
#ifndef SX
	break;
#endif
      }

  grid2_add = -1;
  /* pole_loop2 */
  for ( n = 0; n < grid2_size; ++n )
    if ( rg->grid2_area[n] < -THREE*PIH && rg->grid2_center_lat[n] > ZERO )
      {
	grid2_add = n;
#ifndef SX
	break;
#endif
      }

  if ( grid1_add != -1 )
    {
      rg->grid1_area[grid1_add]     += weights[0];
      grid1_centroid_lat[grid1_add] += weights[1];
      grid1_centroid_lon[grid1_add] += weights[2];
    }

  if ( grid2_add != -1 )
    {
      rg->grid2_area[grid2_add]     += weights[3];
      grid2_centroid_lat[grid2_add] += weights[4];
      grid2_centroid_lon[grid2_add] += weights[5];
    }

  if ( grid1_add != -1 && grid2_add != -1 )
    {
      if ( rg->store_link_fast )
	store_link_cnsrv_fast(rv, grid1_add, grid2_add, weights, grid_store);
      else
	store_link_cnsrv(rv, grid1_add, grid2_add, weights, link_add1, link_add2);

      rg->grid1_frac[grid1_add] += weights[0];
      rg->grid2_frac[grid2_add] += weights[3];
    }

  /* South Pole */
  weights[0] =  PI2;
  weights[1] = -PI*PI;
  weights[2] =  ZERO;
  weights[3] =  PI2;
  weights[4] = -PI*PI;
  weights[5] =  ZERO;

  grid1_add = -1;
  /* pole_loop3 */
  for ( n = 0; n < grid1_size; ++n )
    if ( rg->grid1_area[n] < -THREE*PIH && rg->grid1_center_lat[n] < ZERO )
      {
	grid1_add = n;
#ifndef SX
	break;
#endif
      }

  grid2_add = -1;
  /* pole_loop4 */
  for ( n = 0; n < grid2_size; ++n )
    if ( rg->grid2_area[n] < -THREE*PIH && rg->grid2_center_lat[n] < ZERO )
      {
	grid2_add = n;
#ifndef SX
	break;
#endif
      }

  if ( grid1_add != -1 )
    {
      rg->grid1_area[grid1_add]     += weights[0];
      grid1_centroid_lat[grid1_add] += weights[1];
      grid1_centroid_lon[grid1_add] += weights[2];
    }

  if ( grid2_add != -1 )
    {
      rg->grid2_area[grid2_add]     += weights[3];
      grid2_centroid_lat[grid2_add] += weights[4];
      grid2_centroid_lon[grid2_add] += weights[5];
    }

  if ( grid1_add != -1 && grid2_add != -1 )
    {
      if ( rg->store_link_fast )
	store_link_cnsrv_fast(rv, grid1_add, grid2_add, weights, grid_store);
      else
	store_link_cnsrv(rv, grid1_add, grid2_add, weights, link_add1, link_add2);

      rg->grid1_frac[grid1_add] += weights[0];
      rg->grid2_frac[grid2_add] += weights[3];
    }

  if ( rg->store_link_fast )
    {
      grid_store_delete(grid_store);
      free(grid_store);
    }


  /* Finish centroid computation */

  for ( n = 0; n < grid1_size; ++n )
    if ( IS_NOT_EQUAL(rg->grid1_area[n], 0) )
      {
        grid1_centroid_lat[n] /= rg->grid1_area[n];
        grid1_centroid_lon[n] /= rg->grid1_area[n];
      }

  for ( n = 0; n < grid2_size; ++n )
    if ( IS_NOT_EQUAL(rg->grid2_area[n], 0) )
      {
        grid2_centroid_lat[n] /= rg->grid2_area[n];
        grid2_centroid_lon[n] /= rg->grid2_area[n];
      }

  /* 2010-10-08 Uwe Schulzweida: remove all links with weights < 0 */

  /* 
  if ( 1 )
    {
      num_links = rv->num_links;

      if ( cdoVerbose )
	for ( n = 0; n < num_links; n++ )
	  printf("wts1: %d %g\n", n, rv->wts[3*n]);

      for ( n = 0; n < num_links; n++ )
	{
	  if ( rv->wts[3*n] < 0 )
	    {
	      int i, n2, nd;
     
	      for ( n2 = n+1; n2 < num_links; n2++ )
		if ( rv->wts[3*n2] >= 0 ) break;

	      nd = n2-n;
	      num_links -= nd;
	      for ( i = n; i < num_links; i++ )
		{
		  rv->wts[3*i]   = rv->wts[3*(i+nd)];
		  rv->wts[3*i+1] = rv->wts[3*(i+nd)+1];
		  rv->wts[3*i+2] = rv->wts[3*(i+nd)+2];
		  
		  rv->grid1_add[i] = rv->grid1_add[i+nd];
		  rv->grid2_add[i] = rv->grid2_add[i+nd];
		}
	    }
	}

     if ( cdoVerbose ) cdoPrint("Removed number of links = %ld", rv->num_links - num_links);

      rv->num_links = num_links;
    }
  */

  /* Include centroids in weights and normalize using destination area if requested */

  num_links = rv->num_links;

  if ( rv->norm_opt == NORM_OPT_DESTAREA )
    {
#if defined(SX)
#pragma vdir nodep
#endif
#if defined(_OPENMP)
#pragma omp parallel for default(none) \
  shared(num_links, rv, rg, grid1_centroid_lat, grid1_centroid_lon)		\
  private(n, n3, grid1_add, grid2_add, weights, norm_factor)
#endif
      for ( n = 0; n < num_links; ++n )
	{
	  n3 = n*3;
	  grid1_add = rv->grid1_add[n]; grid2_add = rv->grid2_add[n];
	  weights[0] = rv->wts[n3]; weights[1] = rv->wts[n3+1]; weights[2] = rv->wts[n3+2];

          if ( IS_NOT_EQUAL(rg->grid2_area[grid2_add], 0) )
	    norm_factor = ONE/rg->grid2_area[grid2_add];
          else
            norm_factor = ZERO;

	  rv->wts[n3  ] =  weights[0]*norm_factor;
	  rv->wts[n3+1] = (weights[1] - weights[0]*grid1_centroid_lat[grid1_add])*norm_factor;
	  rv->wts[n3+2] = (weights[2] - weights[0]*grid1_centroid_lon[grid1_add])*norm_factor;
	}
    }
  else if ( rv->norm_opt == NORM_OPT_FRACAREA )
    {
#if defined(SX)
#pragma vdir nodep
#endif
#if defined(_OPENMP)
#pragma omp parallel for default(none) \
  shared(num_links, rv, rg, grid1_centroid_lat, grid1_centroid_lon)		\
  private(n, n3, grid1_add, grid2_add, weights, norm_factor)
#endif
      for ( n = 0; n < num_links; ++n )
	{
	  n3 = n*3;
	  grid1_add = rv->grid1_add[n]; grid2_add = rv->grid2_add[n];
	  weights[0] = rv->wts[n3]; weights[1] = rv->wts[n3+1]; weights[2] = rv->wts[n3+2];

          if ( IS_NOT_EQUAL(rg->grid2_frac[grid2_add], 0) )
	    norm_factor = ONE/rg->grid2_frac[grid2_add];
          else
            norm_factor = ZERO;

	  rv->wts[n3  ] =  weights[0]*norm_factor;
	  rv->wts[n3+1] = (weights[1] - weights[0]*grid1_centroid_lat[grid1_add])*norm_factor;
	  rv->wts[n3+2] = (weights[2] - weights[0]*grid1_centroid_lon[grid1_add])*norm_factor;
	}
    }
  else if ( rv->norm_opt == NORM_OPT_NONE )
    {
#if defined(SX)
#pragma vdir nodep
#endif
#if defined(_OPENMP)
#pragma omp parallel for default(none) \
  shared(num_links, rv, rg, grid1_centroid_lat, grid1_centroid_lon)	\
  private(n, n3, grid1_add, grid2_add, weights, norm_factor)
#endif
      for ( n = 0; n < num_links; ++n )
	{
	  n3 = n*3;
	  grid1_add = rv->grid1_add[n]; grid2_add = rv->grid2_add[n];
	  weights[0] = rv->wts[n3]; weights[1] = rv->wts[n3+1]; weights[2] = rv->wts[n3+2];

          norm_factor = ONE;

	  rv->wts[n3  ] =  weights[0]*norm_factor;
	  rv->wts[n3+1] = (weights[1] - weights[0]*grid1_centroid_lat[grid1_add])*norm_factor;
	  rv->wts[n3+2] = (weights[2] - weights[0]*grid1_centroid_lon[grid1_add])*norm_factor;
	}
    }

  if ( cdoVerbose )
    cdoPrint("Total number of links = %ld", rv->num_links);

  for ( n = 0; n < grid1_size; ++n )
    if ( IS_NOT_EQUAL(rg->grid1_area[n], 0) ) rg->grid1_frac[n] /= rg->grid1_area[n];

  for ( n = 0; n < grid2_size; ++n )
    if ( IS_NOT_EQUAL(rg->grid2_area[n], 0) ) rg->grid2_frac[n] /= rg->grid2_area[n];

  /* Perform some error checking on final weights  */

  if ( lcheck )
    {
      for ( n = 0; n < grid1_size; ++n )
	{
	  if ( rg->grid1_area[n] < -.01 )
	    cdoPrint("Grid 1 area error: %d %g", n, rg->grid1_area[n]);

	  if ( grid1_centroid_lat[n] < -PIH-.01 || grid1_centroid_lat[n] > PIH+.01 )
	    cdoPrint("Grid 1 centroid lat error: %d %g", n, grid1_centroid_lat[n]);

	  grid1_centroid_lat[n] = 0;
	  grid1_centroid_lon[n] = 0;
	}

      for ( n = 0; n < grid2_size; ++n )
	{
	  if ( rg->grid2_area[n] < -.01 )
	    cdoPrint("Grid 2 area error: %d %g", n, rg->grid2_area[n]);
	  if ( grid2_centroid_lat[n] < -PIH-.01 || grid2_centroid_lat[n] > PIH+.01 )
	    cdoPrint("Grid 2 centroid lat error: %d %g", n, grid2_centroid_lat[n]);

	  grid2_centroid_lat[n] = 0;
	  grid2_centroid_lon[n] = 0;
	}

      for ( n = 0; n < num_links; ++n )
	{
	  grid1_add = rv->grid1_add[n];
	  grid2_add = rv->grid2_add[n];

	  if ( rv->wts[3*n] < -0.01 )
	    cdoPrint("Map 1 weight < 0! grid1idx=%d grid2idx=%d nlink=%d wts=%g",
		     grid1_add, grid2_add, n, rv->wts[3*n]);

	  if ( rv->norm_opt != NORM_OPT_NONE && rv->wts[3*n] > 1.01 )
	    cdoPrint("Map 1 weight > 1! grid1idx=%d grid2idx=%d nlink=%d wts=%g",
		     grid1_add, grid2_add, n, rv->wts[3*n]);
	}

      for ( n = 0; n < num_links; ++n )
	{
	  grid2_add = rv->grid2_add[n];
	  grid2_centroid_lat[grid2_add] += rv->wts[3*n];
	}

      /* 2012-01-24 Uwe Schulzweida: changed [grid2_add] to [n] (bug fix) */
      for ( n = 0; n < grid2_size; ++n )
	{
	  if ( rv->norm_opt == NORM_OPT_DESTAREA )
	    norm_factor = rg->grid2_frac[n];
	  else if ( rv->norm_opt == NORM_OPT_FRACAREA )
	    norm_factor = ONE;
	  else if ( rv->norm_opt == NORM_OPT_NONE )
	    norm_factor = rg->grid2_area[n];
	    
	  if ( grid2_centroid_lat[n] > 0 && fabs(grid2_centroid_lat[n] - norm_factor) > .01 )
	    cdoPrint("Error: sum of wts for map1 %d %g %g", n, grid2_centroid_lat[n], norm_factor);
	}
    } // lcheck

  free(grid1_centroid_lat);
  free(grid1_centroid_lon);
  free(grid2_centroid_lat);
  free(grid2_centroid_lon);

  if ( ! rg->store_link_fast )
    {
      free(link_add1[0]);
      free(link_add1[1]);
      free(link_add2[0]);
      free(link_add2[1]);
    }

  if ( cdoTimer ) timer_stop(timer_remap_con);

} /* remap_conserv */

/*****************************************************************************/

void remap_stat(int remap_order, remapgrid_t rg, remapvars_t rv, const double *restrict array1, 
		const double *restrict array2, double missval)
{
  long n, ns, i;
  long idiff, imax, imin, icount;
  int *grid2_count;
  double minval, maxval, sum;
	  
  if ( remap_order == 2 )
    cdoPrint("Second order mapping from grid1 to grid2:");
  else
    cdoPrint("First order mapping from grid1 to grid2:");
  cdoPrint("----------------------------------------");

  ns = 0;
  sum = 0;
  minval =  DBL_MAX;
  maxval = -DBL_MAX;
  for ( n = 0; n < rg.grid1_size; ++n )
    {
      if ( !DBL_IS_EQUAL(array1[n], missval) )
	{
	  if ( array1[n] < minval ) minval = array1[n];
	  if ( array1[n] > maxval ) maxval = array1[n];
	  sum += array1[n];
	  ns++;
	}
    }
  if ( ns > 0 ) sum /= ns;
  cdoPrint("Grid1 min,mean,max: %g %g %g", minval, sum, maxval);

  ns = 0;
  sum = 0;
  minval =  DBL_MAX;
  maxval = -DBL_MAX;
  for ( n = 0; n < rg.grid2_size; ++n )
    {
      if ( !DBL_IS_EQUAL(array2[n], missval) )
	{
	  if ( array2[n] < minval ) minval = array2[n];
	  if ( array2[n] > maxval ) maxval = array2[n];
	  sum += array2[n];
	  ns++;
	}
    }
  if ( ns > 0 ) sum /= ns;
  cdoPrint("Grid2 min,mean,max: %g %g %g", minval, sum, maxval);

  /* Conservation Test */

  if ( rg.grid1_area )
    {
      cdoPrint("Conservation:");
      sum = 0;
      for ( n = 0; n < rg.grid1_size; ++n )
	if ( !DBL_IS_EQUAL(array1[n], missval) )
	  sum += array1[n]*rg.grid1_area[n]*rg.grid1_frac[n];
      cdoPrint("Grid1 Integral = %g", sum);

      sum = 0;
      for ( n = 0; n < rg.grid2_size; ++n )
	if ( !DBL_IS_EQUAL(array2[n], missval) )
	  sum += array2[n]*rg.grid2_area[n]*rg.grid2_frac[n];
      cdoPrint("Grid2 Integral = %g", sum);
      /*
      for ( n = 0; n < rg.grid1_size; n++ )
       fprintf(stderr, "1 %d %g %g %g\n", n, array1[n], rg.grid1_area[n], rg.grid1_frac[n]);
      for ( n = 0; n < rg.grid2_size; n++ )
	fprintf(stderr, "2 %d %g %g %g\n", n, array2[n], rg.grid2_area[n], rg.grid2_frac[n]);
      */
    }

  cdoPrint("number of sparse matrix entries %d", rv.num_links);
  cdoPrint("total number of dest cells %d", rg.grid2_size);

  grid2_count = (int *) malloc(rg.grid2_size*sizeof(int));

  for ( n = 0; n < rg.grid2_size; ++n ) grid2_count[n] = 0;

#if defined(SX)
#pragma vdir nodep
#endif
  for ( n = 0; n < rv.num_links; ++n ) grid2_count[rv.grid2_add[n]]++;

  imin = INT_MAX;
  imax = INT_MIN;
  for ( n = 0; n < rg.grid2_size; ++n )
    {
      if ( grid2_count[n] > 0 )
	if ( grid2_count[n] < imin ) imin = grid2_count[n];
      if ( grid2_count[n] > imax ) imax = grid2_count[n];
    }

  idiff =  (imax - imin)/10 + 1;
  icount = 0;
  for ( i = 0; i < rg.grid2_size; ++i )
    if ( grid2_count[i] > 0 ) icount++;

  cdoPrint("number of cells participating in remap %d", icount);

  if ( icount )
    {
      cdoPrint("min no of entries/row = %d", imin);
      cdoPrint("max no of entries/row = %d", imax);

      imax = imin + idiff;
      for ( n = 0; n < 10; ++n )
	{
	  icount = 0;
	  for ( i = 0; i < rg.grid2_size; ++i )
	    if ( grid2_count[i] >= imin && grid2_count[i] < imax ) icount++;

	  if ( icount )
	    cdoPrint("num of rows with entries between %d - %d  %d", imin, imax-1, icount);

	  imin = imin + idiff;
	  imax = imax + idiff;
	}
    }

  free(grid2_count);

} /* remap_stat */

/*****************************************************************************/

void remap_gradients(remapgrid_t rg, const double *restrict array, double *restrict grad1_lat,
		     double *restrict grad1_lon, double *restrict grad1_latlon)
{
  long n, nx, ny, grid1_size;
  long i, j, ip1, im1, jp1, jm1, in, is, ie, iw, ine, inw, ise, isw;
  double delew, delns;
  double grad1_lat_zero, grad1_lon_zero;

  if ( rg.grid1_rank != 2 )
    cdoAbort("Internal problem (remap_gradients), grid1 rank = %d!", rg.grid1_rank);

  grid1_size = rg.grid1_size;
  nx = rg.grid1_dims[0];
  ny = rg.grid1_dims[1];

#if defined(_OPENMP)
#pragma omp parallel for default(none)        \
  shared(grid1_size, grad1_lat, grad1_lon, grad1_latlon, rg, nx, ny, array) \
  private(n, i, j, ip1, im1, jp1, jm1, in, is, ie, iw, ine, inw, ise, isw, delew, delns, grad1_lat_zero, grad1_lon_zero)
#endif
  for ( n = 0; n < grid1_size; ++n )
    {
      grad1_lat[n] = ZERO;
      grad1_lon[n] = ZERO;
      grad1_latlon[n] = ZERO;

      if ( rg.grid1_mask[n] )
	{
	  delew = HALF;
	  delns = HALF;

	  j = n/nx + 1;
	  i = n - (j-1)*nx + 1;

	  ip1 = i + 1;
	  im1 = i - 1;
	  jp1 = j + 1;
	  jm1 = j - 1;

	  if ( ip1 > nx ) ip1 = ip1 - nx;
	  if ( im1 <  1 ) im1 = nx;
	  if ( jp1 > ny )
	    {
              jp1 = j;
              delns = ONE;
            }
	  if ( jm1 < 1 )
	    {
              jm1 = j;
              delns = ONE;
            }

	  in  = (jp1-1)*nx + i - 1;
	  is  = (jm1-1)*nx + i - 1;
	  ie  = (j  -1)*nx + ip1 - 1;
	  iw  = (j  -1)*nx + im1 - 1;

	  ine = (jp1-1)*nx + ip1 - 1;
	  inw = (jp1-1)*nx + im1 - 1;
	  ise = (jm1-1)*nx + ip1 - 1;
	  isw = (jm1-1)*nx + im1 - 1;

	  /* Compute i-gradient */

	  if ( ! rg.grid1_mask[ie] )
	    {
              ie = n;
              delew = ONE;
            }
	  if ( ! rg.grid1_mask[iw] )
	    {
              iw = n;
              delew = ONE;
            }
 
	  grad1_lat[n] = delew*(array[ie] - array[iw]);

	  /* Compute j-gradient */

	  if ( ! rg.grid1_mask[in] )
	    {
              in = n;
              delns = ONE;
            }
	  if ( ! rg.grid1_mask[is] )
	    {
              is = n;
              delns = ONE;
            }
 
	  grad1_lon[n] = delns*(array[in] - array[is]);

	  /* Compute ij-gradient */

	  delew = HALF;
	  if ( jp1 == j || jm1 == j )
	    delns = ONE;
	  else 
	    delns = HALF;

	  if ( ! rg.grid1_mask[ine] )
	    {
              if ( in != n )
		{
		  ine = in;
		  delew = ONE;
		}
              else if ( ie != n )
		{
		  ine = ie;
		  inw = iw;
		  if ( inw == n ) delew = ONE;
		  delns = ONE;
		}
              else
		{
		  ine = n;
		  inw = iw;
		  delew = ONE;
		  delns = ONE;
		}
	    }

	  if ( ! rg.grid1_mask[inw] )
	    {
              if ( in != n )
		{
		  inw = in;
		  delew = ONE;
		}
              else if ( iw != n )
		{
		  inw = iw;
		  ine = ie;
		  if ( ie == n ) delew = ONE;
		  delns = ONE;
		}
              else
		{
		  inw = n;
		  ine = ie;
		  delew = ONE;
		  delns = ONE;
		}
	    }

	  grad1_lat_zero = delew*(array[ine] - array[inw]);

	  if ( ! rg.grid1_mask[ise] )
	    {
              if ( is != n )
		{
		  ise = is;
		  delew = ONE;
		}
              else if ( ie != n )
		{
		  ise = ie;
		  isw = iw;
		  if ( isw == n ) delew = ONE;
		  delns = ONE;
		}
              else
		{
		  ise = n;
		  isw = iw;
		  delew = ONE;
		  delns = ONE;
		}
	    }

	  if ( ! rg.grid1_mask[isw] )
	    {
              if ( is != n )
		{
		  isw = is;
		  delew = ONE;
		}
              else if ( iw != n )
		{
		  isw = iw;
		  ise = ie;
		  if ( ie == n ) delew = ONE;
		  delns = ONE;
		}
              else
		{
		  isw = n;
		  ise = ie;
		  delew = ONE;
		  delns = ONE;
		}
	    }

	  grad1_lon_zero = delew*(array[ise] - array[isw]);

	  grad1_latlon[n] = delns*(grad1_lat_zero - grad1_lon_zero);
	}
    }
} /* remap_gradients */

/*****************************************************************************/

void reorder_links(remapvars_t *rv)
{
  long j, nval = 0, num_blks = 0;
  long lastval;
  long nlinks;
  long max_links = 0;
  long n;
  long num_links;

  num_links = rv->num_links;

  printf("reorder_links\n");
  printf("  num_links %ld\n", num_links);
  rv->links.option = TRUE;

  lastval = -1;
  for ( n = 0; n < num_links; n++ )
    {
      if ( rv->grid2_add[n] == lastval ) nval++;
      else
	{
	  if ( nval > num_blks ) num_blks = nval;
	  nval = 1;
	  max_links++;
	  lastval = rv->grid2_add[n];
	}
    }

  if ( num_blks )
    {
      rv->links.max_links = max_links;
      rv->links.num_blks  = num_blks;

      printf("num_links %ld  max_links %ld  num_blks %ld\n", rv->num_links, max_links, num_blks);

      rv->links.num_links = (int *)  malloc(num_blks*sizeof(int));
      rv->links.dst_add   = (int **) malloc(num_blks*sizeof(int *));
      rv->links.src_add   = (int **) malloc(num_blks*sizeof(int *));
      rv->links.w_index   = (int **) malloc(num_blks*sizeof(int *));
    }

  for ( j = 0; j < num_blks; j++ )
    {
      rv->links.dst_add[j] = (int *) malloc(max_links*sizeof(int));
      rv->links.src_add[j] = (int *) malloc(max_links*sizeof(int));
      rv->links.w_index[j] = (int *) malloc(max_links*sizeof(int));
    }

  for ( j = 0; j < num_blks; j++ )
    {
      nval = 0;
      lastval = -1;
      nlinks = 0;

      for ( n = 0; n < num_links; n++ )
	{
	  if ( rv->grid2_add[n] == lastval ) nval++;
	  else
	    {
	      nval = 1;
	      lastval = rv->grid2_add[n];
	    }
	  
	  if ( nval == j+1 )
	    {
	      rv->links.dst_add[j][nlinks] = rv->grid2_add[n];
	      rv->links.src_add[j][nlinks] = rv->grid1_add[n];
	      rv->links.w_index[j][nlinks] = n;
	      nlinks++;
	    }
	}

      rv->links.num_links[j] = nlinks;
      printf("loop %ld  nlinks %ld\n", j+1, nlinks);
    }
}