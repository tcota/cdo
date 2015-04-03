#if defined (HAVE_CONFIG_H)
#  include "config.h"
#endif

#include <string.h>
#include <math.h>

#include "cdi.h"
#include "cdi_int.h"
#include "dmemory.h"
#include "varscan.h"
#include "vlist.h"
#include "grid.h"
#include "zaxis.h"


extern void zaxisGetIndexList ( int, int * );


#undef  UNDEFID
#define UNDEFID -1

static size_t Vctsize = 0;
static double *Vct = NULL;

static int numberOfVerticalLevels = 0;
static int numberOfVerticalGrid = 0;
static char uuidVGrid[17];

typedef struct
{
  int      level1;
  int      level2;
  int      recID;
  int      lindex;
}
leveltable_t;

typedef struct
{
  int           param;
  int           prec;
  int           tsteptype;
  int           timave;
  int           timaccu;
  int           gridID;
  int           zaxistype;
  int           ltype;     /* GRIB level type */
  int           lbounds;
  int           level_sf;
  int           level_unit;
  int           zaxisID;
  int           nlevels;
  int           levelTableSize;
  leveltable_t *levelTable;
  int           instID;
  int           modelID;
  int           tableID;
  int           comptype;       // compression type
  int           complevel;      // compression level
  int           lmissval;
  double        missval;
  char         *name;
  char         *stdname;
  char         *longname;
  char         *units;
  ensinfo_t    *ensdata;
  int           typeOfGeneratingProcess;
#if  defined  (HAVE_LIBGRIB_API)
  /* (Optional) list of keyword/double value pairs */
  int           opt_grib_dbl_nentries;
  char         *opt_grib_dbl_keyword[MAX_OPT_GRIB_ENTRIES];
  double        opt_grib_dbl_val[MAX_OPT_GRIB_ENTRIES];
  /* (Optional) list of keyword/integer value pairs */
  int           opt_grib_int_nentries;
  char         *opt_grib_int_keyword[MAX_OPT_GRIB_ENTRIES];
  int           opt_grib_int_val[MAX_OPT_GRIB_ENTRIES];
#endif
}
vartable_t;


int vartableInit = 0;
vartable_t *vartable;
static int varTablesize = 0;
int nvars = 0;


static
void paramInitEntry(int varID, int param)
{
  vartable[varID].param          = param;
  vartable[varID].prec           = 0;
  vartable[varID].tsteptype      = TSTEP_INSTANT;
  vartable[varID].timave         = 0;
  vartable[varID].timaccu        = 0;
  vartable[varID].gridID         = UNDEFID;
  vartable[varID].zaxistype      = 0;
  vartable[varID].ltype          = 0;
  vartable[varID].lbounds        = 0;
  vartable[varID].level_sf       = 0;
  vartable[varID].level_unit     = 0;
  vartable[varID].levelTable     = NULL;
  vartable[varID].levelTableSize = 0;
  vartable[varID].nlevels        = 0;
  vartable[varID].instID         = UNDEFID;
  vartable[varID].modelID        = UNDEFID;
  vartable[varID].tableID        = UNDEFID;
  vartable[varID].typeOfGeneratingProcess  = UNDEFID;
  vartable[varID].comptype       = COMPRESS_NONE;
  vartable[varID].complevel      = 1;
  vartable[varID].lmissval       = 0;
  vartable[varID].missval        = 0;
  vartable[varID].name           = NULL;
  vartable[varID].stdname        = NULL;
  vartable[varID].longname       = NULL;
  vartable[varID].units          = NULL;
  vartable[varID].ensdata        = NULL;
}

static
int varGetEntry(int param, int zaxistype, int ltype, const char *name)
{
  int varID;

  for ( varID = 0; varID < varTablesize; varID++ )
    {
      if ( vartable[varID].param     == param     &&
	   vartable[varID].zaxistype == zaxistype &&
	   vartable[varID].ltype     == ltype )
        {
          if ( name && name[0] && vartable[varID].name && vartable[varID].name[0] )
            {
              if ( strcmp(name, vartable[varID].name) == 0 ) return (varID);
            }
          else
            {
              return (varID);
            }
        }
    }

  return (UNDEFID);
}

static
void varFree(void)
{
  int varID;

  for ( varID = 0; varID < nvars; varID++ )
    {
      if ( vartable[varID].levelTable )
	free(vartable[varID].levelTable);

      if ( vartable[varID].name )     free(vartable[varID].name);
      if ( vartable[varID].stdname )  free(vartable[varID].stdname);
      if ( vartable[varID].longname ) free(vartable[varID].longname);
      if ( vartable[varID].units )    free(vartable[varID].units);
      if ( vartable[varID].ensdata )  free(vartable[varID].ensdata);
    }

  if ( vartable )
    free(vartable);

  vartable = NULL;
  varTablesize = 0;
  nvars = 0;

  if ( Vct )
    free(Vct);

  Vct = NULL;
  Vctsize = 0;
}

static
int levelNewEntry(int varID, int level1, int level2)
{
  int levelID = 0;
  int levelTableSize;
  leveltable_t *levelTable;

  levelTableSize = vartable[varID].levelTableSize;
  levelTable     = vartable[varID].levelTable;

  /*
    Look for a free slot in levelTable.
    (Create the table the first time through).
  */
  if ( ! levelTableSize )
    {
      int i;

      levelTableSize = 2;
      levelTable = (leveltable_t *) malloc(levelTableSize*sizeof(leveltable_t));
      if( levelTable == NULL )
	{
          Message("levelTableSize = %d", levelTableSize);
	  SysError("Allocation of leveltable failed!");
	}

      for( i = 0; i < levelTableSize; i++ )
	levelTable[i].recID = UNDEFID;
    }
  else
    {
      while( levelID < levelTableSize )
	{
	  if ( levelTable[levelID].recID == UNDEFID ) break;
	  levelID++;
	}
    }
  /*
    If the table overflows, double its size.
  */
  if( levelID == levelTableSize )
    {
      int i;

      levelTableSize = 2*levelTableSize;
      levelTable = (leveltable_t *) realloc(levelTable, levelTableSize*sizeof(leveltable_t));
      if( levelTable == NULL )
	{
          Message("levelTableSize = %d", levelTableSize);
	  SysError("Reallocation of leveltable failed");
	}
      levelID = levelTableSize/2;

      for( i = levelID; i < levelTableSize; i++ )
	levelTable[i].recID = UNDEFID;
    }

  levelTable[levelID].level1   = level1;
  levelTable[levelID].level2   = level2;
  levelTable[levelID].lindex   = levelID;

  vartable[varID].nlevels = levelID+1;
  vartable[varID].levelTableSize = levelTableSize;
  vartable[varID].levelTable = levelTable;

  return (levelID);
}

#define  UNDEF_PARAM  -4711

static
int paramNewEntry(int param)
{
  int varID = 0;

  /*
    Look for a free slot in vartable.
    (Create the table the first time through).
  */
  if ( ! varTablesize )
    {
      int i;

      varTablesize = 2;
      vartable = (vartable_t *) malloc(varTablesize*sizeof(vartable_t));
      if( vartable == NULL )
	{
          Message("varTablesize = %d", varTablesize);
	  SysError("Allocation of vartable failed");
	}

      for( i = 0; i < varTablesize; i++ )
	{
	  vartable[i].param = UNDEF_PARAM;
#if  defined  (HAVE_LIBGRIB_API)
	  vartable[i].opt_grib_int_nentries = 0;
	  vartable[i].opt_grib_dbl_nentries = 0;
#endif
	}
    }
  else
    {
      while( varID < varTablesize )
	{
	  if ( vartable[varID].param == UNDEF_PARAM ) break;
	  varID++;
	}
    }
  /*
    If the table overflows, double its size.
  */
  if ( varID == varTablesize )
    {
      int i;

      varTablesize = 2*varTablesize;
      vartable = (vartable_t *) realloc(vartable, varTablesize*sizeof(vartable_t));
      if( vartable == NULL )
	{
          Message("varTablesize = %d", varTablesize);
	  SysError("Reallocation of vartable failed!");
	}
      varID = varTablesize/2;

      for( i = varID; i < varTablesize; i++ )
	{
	  vartable[i].param = UNDEF_PARAM;
#if  defined  (HAVE_LIBGRIB_API)
	  vartable[i].opt_grib_int_nentries = 0;
	  vartable[i].opt_grib_dbl_nentries = 0;
#endif
	}
    }

  paramInitEntry(varID, param);

  return (varID);
}


void varAddRecord(int recID, int param, int gridID, int zaxistype, int lbounds,
		  int level1, int level2, int level_sf, int level_unit, int prec,
		  int *pvarID, int *plevelID, int tsteptype, int numavg, int ltype,
		  const char *name, const char *stdname, const char *longname, const char *units)
{
  int varID = UNDEFID;
  int levelID = -1;

  if ( ! (cdiSplitLtype105 == 1 && zaxistype == ZAXIS_HEIGHT) )
    varID = varGetEntry(param, zaxistype, ltype, name);

  if ( varID == UNDEFID )
    {
      nvars++;
      varID = paramNewEntry(param);
      vartable[varID].gridID    = gridID;
      vartable[varID].zaxistype = zaxistype;
      vartable[varID].ltype     = ltype;
      vartable[varID].lbounds   = lbounds;
      vartable[varID].level_sf  = level_sf;
      vartable[varID].level_unit = level_unit;
      if ( tsteptype != UNDEFID ) vartable[varID].tsteptype = tsteptype;
      if ( numavg ) vartable[varID].timave = 1;

      if ( name )     if ( name[0] )     vartable[varID].name     = strdup(name);
      if ( stdname )  if ( stdname[0] )  vartable[varID].stdname  = strdup(stdname);
      if ( longname ) if ( longname[0] ) vartable[varID].longname = strdup(longname);
      if ( units )    if ( units[0] )    vartable[varID].units    = strdup(units);
    }
  else
    {
      char paramstr[32];
      cdiParamToString(param, paramstr, sizeof(paramstr));

      if ( vartable[varID].gridID != gridID )
	{
	  Message("param = %s gridID = %d", paramstr, gridID);
	  Error("horizontal grid must not change for same param!");
	}
      if ( vartable[varID].zaxistype != zaxistype )
	{
	  Message("param = %s zaxistype = %d", paramstr, zaxistype);
	  Error("zaxistype must not change for same param!");
	}
    }

  if ( prec > vartable[varID].prec ) vartable[varID].prec = prec;

  levelID = levelNewEntry(varID, level1, level2);
  vartable[varID].levelTable[levelID].recID = recID;

  *pvarID   = varID;
  *plevelID = levelID;
}
/*
static
int dblcmp(const void *s1, const void *s2)
{
  int cmp = 0;

  if      ( *((double *) s1) < *((double *) s2) ) cmp = -1;
  else if ( *((double *) s1) > *((double *) s2) ) cmp =  1;

  return (cmp);
}
*/
static
int cmpLevelTable(const void* s1, const void* s2)
{
  int cmp = 0;
  const leveltable_t* x = (const leveltable_t*) s1;
  const leveltable_t* y = (const leveltable_t*) s2;
  /*
  printf("%g %g  %d %d\n", x->leve11, y->level1, x, y);
  */
  if      ( x->level1 < y->level1 ) cmp = -1;
  else if ( x->level1 > y->level1 ) cmp =  1;

  return (cmp);
}

static
int cmpLevelTableInv(const void* s1, const void* s2)
{
  int cmp = 0;
  const leveltable_t* x = (const leveltable_t*) s1;
  const leveltable_t* y = (const leveltable_t*) s2;
  /*
  printf("%g %g  %d %d\n", x->leve11, y->level1, x, y);
  */
  if      ( x->level1 < y->level1 ) cmp =  1;
  else if ( x->level1 > y->level1 ) cmp = -1;

  return (cmp);
}


typedef struct
{
  int      varid;
  int      param;
  int      ltype;
}
param_t;


static
int cmpparam(const void* s1, const void* s2)
{
  const param_t* x = (const param_t*) s1;
  const param_t* y = (const param_t*) s2;

  int cmp = (( x->param > y->param ) - ( x->param < y->param )) * 2
           + ( x->ltype > y->ltype ) - ( x->ltype < y->ltype );

  return (cmp);
}


void cdi_generate_vars(stream_t *streamptr)
{
  int varID, gridID, zaxisID, levelID;
  int instID, modelID, tableID;
  int param, nlevels, zaxistype, lindex, ltype;
  int prec;
  int tsteptype;
  int timave, timaccu;
  int lbounds;
  int comptype;
  char name[CDI_MAX_NAME], longname[CDI_MAX_NAME], units[CDI_MAX_NAME];
  double *dlevels = NULL;
  double *dlevels1 = NULL;
  double *dlevels2 = NULL;
  int vlistID;
  int *varids, index, varid;
  double level_sf = 1;

  vlistID =  streamptr->vlistID;

  varids = (int *) malloc(nvars*sizeof(int));
  for ( varID = 0; varID < nvars; varID++ ) varids[varID] = varID;

  if ( streamptr->sortname )
    {
      int index;
      param_t **varInfo;
      varInfo    = (param_t **) malloc(nvars*sizeof(param_t *));
      varInfo[0] = (param_t *)  malloc(nvars*sizeof(param_t));

      for ( index = 1; index < nvars; index++ )
	varInfo[index] = varInfo[0] + index;

      for ( varid = 0; varid < nvars; varid++ )
	{
	  varInfo[varid]->varid = varids[varid];
	  varInfo[varid]->param = vartable[varid].param;
	  varInfo[varid]->ltype = vartable[varid].ltype;
	}
      qsort(varInfo[0], nvars, sizeof(param_t), cmpparam);
      for ( varid = 0; varid < nvars; varid++ )
	{
	  varids[varid] = varInfo[varid]->varid;
	}
      free(varInfo[0]);
      free(varInfo);
    }

  for ( index = 0; index < nvars; index++ )
    {
      varid     = varids[index];

      gridID    = vartable[varid].gridID;
      param     = vartable[varid].param;
      nlevels   = vartable[varid].nlevels;
      ltype     = vartable[varid].ltype;
      zaxistype = vartable[varid].zaxistype;
      if ( ltype == 0 && zaxistype == ZAXIS_GENERIC && cdiDefaultLeveltype != -1 )
	zaxistype = cdiDefaultLeveltype;
      lbounds   = vartable[varid].lbounds;
      prec      = vartable[varid].prec;
      instID    = vartable[varid].instID;
      modelID   = vartable[varid].modelID;
      tableID   = vartable[varid].tableID;
      tsteptype = vartable[varid].tsteptype;
      timave    = vartable[varid].timave;
      timaccu   = vartable[varid].timaccu;
      comptype  = vartable[varid].comptype;

      level_sf  = 1;
      if ( vartable[varid].level_sf != 0 ) level_sf = 1./vartable[varid].level_sf;

      zaxisID = UNDEFID;

      if ( ltype == 0 && zaxistype == ZAXIS_GENERIC && nlevels == 1 &&
	   ! (fabs(vartable[varid].levelTable[0].level1)>0) )
	zaxistype = ZAXIS_SURFACE;

      dlevels = (double *) malloc(nlevels*sizeof(double));

      if ( lbounds && zaxistype != ZAXIS_HYBRID && zaxistype != ZAXIS_HYBRID_HALF )
	for ( levelID = 0; levelID < nlevels; levelID++ )
	  dlevels[levelID] = (level_sf*vartable[varid].levelTable[levelID].level1 +
	                      level_sf*vartable[varid].levelTable[levelID].level2)/2;
      else
	for ( levelID = 0; levelID < nlevels; levelID++ )
	  dlevels[levelID] = level_sf*vartable[varid].levelTable[levelID].level1;

      if ( nlevels > 1 )
	{
          bool linc = true, ldec = true, lsort = false;
          for ( levelID = 1; levelID < nlevels; levelID++ )
            {
              /* check increasing of levels */
              linc &= (dlevels[levelID] > dlevels[levelID-1]);
              /* check decreasing of levels */
              ldec &= (dlevels[levelID] < dlevels[levelID-1]);
            }
          /*
           * always sort pressure z-axis to ensure
           * vartable[varid].levelTable[levelID1].level1 < vartable[varid].levelTable[levelID2].level1 <=> levelID1 > levelID2
           * unless already sorted in decreasing order
           */
          if ( !ldec && zaxistype == ZAXIS_PRESSURE )
            {
              qsort(vartable[varid].levelTable, (size_t)nlevels, sizeof(leveltable_t), cmpLevelTableInv);
              lsort = true;
            }
          /*
           * always sort hybrid and depth-below-land z-axis to ensure
           * vartable[varid].levelTable[levelID1].level1 < vartable[varid].levelTable[levelID2].level1 <=> levelID1 < levelID2
           * unless already sorted in increasing order
           */
          else if ( (!linc && !ldec) ||
                    zaxistype == ZAXIS_HYBRID ||
                    zaxistype == ZAXIS_DEPTH_BELOW_LAND )
            {
              qsort(vartable[varid].levelTable, (size_t)nlevels, sizeof(leveltable_t), cmpLevelTable);
              lsort = true;
            }

          if ( lsort )
            {
              if ( lbounds && zaxistype != ZAXIS_HYBRID && zaxistype != ZAXIS_HYBRID_HALF )
                for ( levelID = 0; levelID < nlevels; levelID++ )
                  dlevels[levelID] = (level_sf*vartable[varid].levelTable[levelID].level1 +
                                      level_sf*vartable[varid].levelTable[levelID].level2)/2.;
              else
                for ( levelID = 0; levelID < nlevels; levelID++ )
                  dlevels[levelID] = level_sf*vartable[varid].levelTable[levelID].level1;
            }
	}

      if ( lbounds )
	{
	  dlevels1 = (double *) malloc(nlevels*sizeof(double));
	  for ( levelID = 0; levelID < nlevels; levelID++ )
	    dlevels1[levelID] = level_sf*vartable[varid].levelTable[levelID].level1;
	  dlevels2 = (double *) malloc(nlevels*sizeof(double));
	  for ( levelID = 0; levelID < nlevels; levelID++ )
	    dlevels2[levelID] = level_sf*vartable[varid].levelTable[levelID].level2;
        }

      char *unitptr = cdiUnitNamePtr(vartable[varid].level_unit);
      zaxisID = varDefZaxis(vlistID, zaxistype, nlevels, dlevels, lbounds, dlevels1, dlevels2,
                            Vctsize, Vct, NULL, NULL, unitptr, 0, 0, ltype);

      if ( zaxisInqType(zaxisID) == ZAXIS_REFERENCE )
        {
          if ( numberOfVerticalLevels > 0 ) zaxisDefNlevRef(zaxisID, numberOfVerticalLevels);
          if ( numberOfVerticalGrid > 0 ) zaxisDefNumber(zaxisID, numberOfVerticalGrid);
          if ( uuidVGrid[0] != 0 ) zaxisDefUUID(zaxisID, uuidVGrid);
        }

      if ( lbounds ) free(dlevels1);
      if ( lbounds ) free(dlevels2);
      free(dlevels);

      varID = stream_new_var(streamptr, gridID, zaxisID);
      varID = vlistDefVar(vlistID, gridID, zaxisID, tsteptype);

      vlistDefVarParam(vlistID, varID, param);
      vlistDefVarDatatype(vlistID, varID, prec);
      vlistDefVarTimave(vlistID, varID, timave);
      vlistDefVarTimaccu(vlistID, varID, timaccu);
      vlistDefVarCompType(vlistID, varID, comptype);

      if ( vartable[varid].typeOfGeneratingProcess != UNDEFID )
        vlistDefVarTypeOfGeneratingProcess(vlistID, varID, vartable[varid].typeOfGeneratingProcess);

      if ( vartable[varid].lmissval ) vlistDefVarMissval(vlistID, varID, vartable[varid].missval);

      if ( vartable[varid].name )     vlistDefVarName(vlistID, varID, vartable[varid].name);
      if ( vartable[varid].stdname )  vlistDefVarStdname(vlistID, varID, vartable[varid].stdname);
      if ( vartable[varid].longname ) vlistDefVarLongname(vlistID, varID, vartable[varid].longname);
      if ( vartable[varid].units )    vlistDefVarUnits(vlistID, varID, vartable[varid].units);

      if ( vartable[varid].ensdata )  vlistDefVarEnsemble(vlistID, varID, vartable[varid].ensdata->ens_index,
	                                                  vartable[varid].ensdata->ens_count,
							  vartable[varid].ensdata->forecast_init_type);

#if  defined  (HAVE_LIBGRIB_API)
      /* ---------------------------------- */
      /* Local change: 2013-04-23, FP (DWD) */
      /* ---------------------------------- */

      int    i;
      vlist_t *vlistptr;
      vlistptr = vlist_to_pointer(vlistID);
      for (i=0; i<vartable[varid].opt_grib_int_nentries; i++)
        {
          int idx = vlistptr->vars[varID].opt_grib_int_nentries;
          vlistptr->vars[varID].opt_grib_int_nentries++;
          if ( idx >= MAX_OPT_GRIB_ENTRIES ) Error("Too many optional keyword/integer value pairs!");
          vlistptr->vars[varID].opt_grib_int_val[idx] = vartable[varid].opt_grib_int_val[idx];
          vlistptr->vars[varID].opt_grib_int_keyword[idx] = strdupx(vartable[varid].opt_grib_int_keyword[idx]);
        }
      for (i=0; i<vartable[varid].opt_grib_dbl_nentries; i++)
        {
          int idx = vlistptr->vars[varID].opt_grib_dbl_nentries;
          vlistptr->vars[varID].opt_grib_dbl_nentries++;
          if ( idx >= MAX_OPT_GRIB_ENTRIES ) Error("Too many optional keyword/double value pairs!");
          vlistptr->vars[varID].opt_grib_dbl_val[idx] = vartable[varid].opt_grib_dbl_val[idx];
          vlistptr->vars[varID].opt_grib_dbl_keyword[idx] = strdupx(vartable[varid].opt_grib_dbl_keyword[idx]);
        }
      /* note: if the key is not defined, we do not throw an error! */
#endif

      if ( cdiDefaultTableID != UNDEFID )
	{
	  int pdis, pcat, pnum;
	  cdiDecodeParam(param, &pnum, &pcat, &pdis);
	  if ( tableInqParNamePtr(cdiDefaultTableID, pnum) )
	    {
	      if ( tableID != UNDEFID )
		{
		  strcpy(name, tableInqParNamePtr(cdiDefaultTableID, pnum));
		  vlistDefVarName(vlistID, varID, name);
		  if ( tableInqParLongnamePtr(cdiDefaultTableID, pnum) )
		    {
		      strcpy(longname, tableInqParLongnamePtr(cdiDefaultTableID, pnum));
		      vlistDefVarLongname(vlistID, varID, longname);
		    }
		  if ( tableInqParUnitsPtr(cdiDefaultTableID, pnum) )
		    {
		      strcpy(units, tableInqParUnitsPtr(cdiDefaultTableID, pnum));
		      vlistDefVarUnits(vlistID, varID, units);
		    }
		}
	      else
		tableID = cdiDefaultTableID;
	    }
	  if ( cdiDefaultModelID != UNDEFID ) modelID = cdiDefaultModelID;
	  if ( cdiDefaultInstID  != UNDEFID )  instID = cdiDefaultInstID;
	}

      if ( instID  != UNDEFID ) vlistDefVarInstitut(vlistID, varID, instID);
      if ( modelID != UNDEFID ) vlistDefVarModel(vlistID, varID, modelID);
      if ( tableID != UNDEFID ) vlistDefVarTable(vlistID, varID, tableID);
    }

  for ( index = 0; index < nvars; index++ )
    {
      varID     = index;
      varid     = varids[index];

      nlevels   = vartable[varid].nlevels;
      /*
      for ( levelID = 0; levelID < nlevels; levelID++ )
	{
	  lindex = vartable[varid].levelTable[levelID].lindex;
	  printf("%d %d %d %d %d\n", varID, levelID,
		 vartable[varid].levelTable[levelID].lindex,
		 vartable[varid].levelTable[levelID].recID,
		 vartable[varid].levelTable[levelID].level1);
	}
      */
      for ( levelID = 0; levelID < nlevels; levelID++ )
	{
	  streamptr->vars[varID].level[levelID] = vartable[varid].levelTable[levelID].recID;
	  for ( lindex = 0; lindex < nlevels; lindex++ )
	    if ( levelID == vartable[varid].levelTable[lindex].lindex ) break;

	  if ( lindex == nlevels )
	    Error("Internal problem! lindex not found.");

	  streamptr->vars[varID].lindex[levelID] = lindex;
	}
    }

  free(varids);

  varFree();
}


void varDefVCT(size_t vctsize, double *vctptr)
{
  if ( Vct == NULL && vctptr != NULL && vctsize > 0 )
    {
      Vctsize = vctsize;
      Vct = (double *) malloc(vctsize*sizeof(double));
      memcpy(Vct, vctptr, vctsize*sizeof(double));
    }
}


void varDefZAxisReference(int nhlev, int nvgrid, char *uuid)
{
  numberOfVerticalLevels = nhlev;
  numberOfVerticalGrid = nvgrid;
  memcpy(uuidVGrid, uuid, 16);
}


int varDefGrid(int vlistID, grid_t grid, int mode)
{
  /*
    mode: 0 search in vlist and grid table
          1 search in grid table
   */
  int gridglobdefined = FALSE;
  int griddefined;
  int ngrids;
  int gridID = UNDEFID;
  int index;
  vlist_t *vlistptr;
  int * gridIndexList, i;

  vlistptr = vlist_to_pointer(vlistID);

  griddefined = FALSE;
  ngrids = vlistptr->ngrids;

  if ( mode == 0 )
    for ( index = 0; index < ngrids; index++ )
      {
	gridID = vlistptr->gridIDs[index];
	if ( gridID == UNDEFID )
	  Error("Internal problem: undefined gridID %d!", gridID);

	if ( gridCompare(gridID, grid) == 0 )
	  {
	    griddefined = TRUE;
	    break;
	  }
      }

  if ( ! griddefined )
    {
      ngrids = gridSize();
      if ( ngrids > 0 )
        {
          gridIndexList = (int*) malloc(ngrids*sizeof(int));
          gridGetIndexList ( ngrids, gridIndexList );
          for ( i = 0; i < ngrids; i++ )
            {
              gridID = gridIndexList[i];
              if ( gridCompare(gridID, grid) == 0 )
                {
                  gridglobdefined = TRUE;
                  break;
                }
            }
          if ( gridIndexList ) free ( gridIndexList );
        }

      ngrids = vlistptr->ngrids;
      if ( mode == 1 )
	for ( index = 0; index < ngrids; index++ )
	  if ( vlistptr->gridIDs[index] == gridID )
	    {
	      gridglobdefined = FALSE;
	      break;
	    }
    }

  if ( ! griddefined )
    {
      if ( ! gridglobdefined ) gridID = gridGenerate(grid);
      ngrids = vlistptr->ngrids;
      vlistptr->gridIDs[ngrids] = gridID;
      vlistptr->ngrids++;
    }

  return (gridID);
}


int zaxisCompare(int zaxisID, int zaxistype, int nlevels, int lbounds, double *levels, char *longname, char *units, int ltype)
{
  int differ = 1;
  int levelID;
  int zlbounds = 0;
  int ltype_is_equal = FALSE;

  if ( ltype == zaxisInqLtype(zaxisID) ) ltype_is_equal = TRUE;

  if ( ltype_is_equal && (zaxistype == zaxisInqType(zaxisID) || zaxistype == ZAXIS_GENERIC) )
    {
      if ( zaxisInqLbounds(zaxisID, NULL) > 0 ) zlbounds = 1;
      if ( nlevels == zaxisInqSize(zaxisID) && zlbounds == lbounds )
	{
	  const double *dlevels;
	  char zlongname[CDI_MAX_NAME];
	  char zunits[CDI_MAX_NAME];

	  dlevels = zaxisInqLevelsPtr(zaxisID);
	  for ( levelID = 0; levelID < nlevels; levelID++ )
	    {
	      if ( fabs(dlevels[levelID] - levels[levelID]) > 1.e-9 )
		break;
	    }

	  if ( levelID == nlevels ) differ = 0;

	  if ( ! differ )
	    {
	      zaxisInqLongname(zaxisID, zlongname);
	      zaxisInqUnits(zaxisID, zunits);
	      if ( longname && zlongname[0] )
		{
		  if ( strcmp(longname, zlongname) != 0 ) differ = 1;
		}
	      if ( units && zunits[0] )
		{
		  if ( strcmp(units, zunits) != 0 ) differ = 1;
		}
	    }
	}
    }

  return (differ);
}


int varDefZaxis(int vlistID, int zaxistype, int nlevels, double *levels, int lbounds,
		double *levels1, double *levels2, int vctsize, double *vct, char *name,
		char *longname, char *units, int prec, int mode, int ltype)
{
  /*
    mode: 0 search in vlist and zaxis table
          1 search in zaxis table
   */
  int zaxisdefined;
  int nzaxis;
  int zaxisID = UNDEFID;
  int index;
  int zaxisglobdefined = 0;
  vlist_t *vlistptr;
  int i;

  vlistptr = vlist_to_pointer(vlistID);

  zaxisdefined = 0;
  nzaxis = vlistptr->nzaxis;

  if ( mode == 0 )
    for ( index = 0; index < nzaxis; index++ )
      {
	zaxisID = vlistptr->zaxisIDs[index];

	if ( zaxisCompare(zaxisID, zaxistype, nlevels, lbounds, levels, longname, units, ltype) == 0 )
	  {
	    zaxisdefined = 1;
	    break;
	  }
      }

  if ( ! zaxisdefined )
    {
      nzaxis = zaxisSize();
      if ( nzaxis > 0 )
        {
          int *zaxisIndexList;
          zaxisIndexList = (int *) malloc ( nzaxis * sizeof ( int ));
          zaxisGetIndexList ( nzaxis, zaxisIndexList );
          for ( i = 0; i < nzaxis; i++ )
            {
              zaxisID = zaxisIndexList[i];
              if ( zaxisCompare(zaxisID, zaxistype, nlevels, lbounds, levels, longname, units, ltype) == 0 )
                {
                  zaxisglobdefined = 1;
                  break;
                }
            }
          if ( zaxisIndexList ) free ( zaxisIndexList );
        }

      nzaxis = vlistptr->nzaxis;
      if ( mode == 1 )
	for ( index = 0; index < nzaxis; index++ )
	  if ( vlistptr->zaxisIDs[index] == zaxisID )
	    {
	      zaxisglobdefined = FALSE;
	      break;
	    }
    }

  if ( ! zaxisdefined )
    {
      if ( ! zaxisglobdefined )
	{
	  zaxisID = zaxisCreate(zaxistype, nlevels);
	  zaxisDefLevels(zaxisID, levels);
	  if ( lbounds )
	    {
	      zaxisDefLbounds(zaxisID, levels1);
	      zaxisDefUbounds(zaxisID, levels2);
	    }

	  if ( zaxistype == ZAXIS_HYBRID || zaxistype == ZAXIS_HYBRID_HALF )
	    {
	      /* if ( vctsize > 0 && vctsize >= 2*(nlevels+1)) */
	      /* if ( vctsize > 0 && vctsize >= 2*(nlevels)) */
	      if ( vctsize > 0 )
		zaxisDefVct(zaxisID, vctsize, vct);
	      else
		Warning("VCT missing");
	    }

	  zaxisDefName(zaxisID, name);
	  zaxisDefLongname(zaxisID, longname);
	  zaxisDefUnits(zaxisID, units);
	  zaxisDefPrec(zaxisID, prec);
	  zaxisDefLtype(zaxisID, ltype);
	}

      nzaxis = vlistptr->nzaxis;
      vlistptr->zaxisIDs[nzaxis] = zaxisID;
      vlistptr->nzaxis++;
    }

  return (zaxisID);
}


void varDefMissval(int varID, double missval)
{
  vartable[varID].lmissval = 1;
  vartable[varID].missval = missval;
}


void varDefCompType(int varID, int comptype)
{
  if ( vartable[varID].comptype == COMPRESS_NONE )
    vartable[varID].comptype = comptype;
}


void varDefCompLevel(int varID, int complevel)
{
  vartable[varID].complevel = complevel;
}


int varInqInst(int varID)
{
  return (vartable[varID].instID);
}


void varDefInst(int varID, int instID)
{
  vartable[varID].instID = instID;
}


int varInqModel(int varID)
{
  return (vartable[varID].modelID);
}


void varDefModel(int varID, int modelID)
{
  vartable[varID].modelID = modelID;
}


int varInqTable(int varID)
{
  return (vartable[varID].tableID);
}


void varDefTable(int varID, int tableID)
{
  vartable[varID].tableID = tableID;
}


void varDefEnsembleInfo(int varID, int ens_idx, int ens_count, int forecast_type)
{
  if ( vartable[varID].ensdata == NULL )
      vartable[varID].ensdata = (ensinfo_t *) malloc( sizeof( ensinfo_t ) );

  vartable[varID].ensdata->ens_index = ens_idx;
  vartable[varID].ensdata->ens_count = ens_count;
  vartable[varID].ensdata->forecast_init_type = forecast_type;
}


void varDefTypeOfGeneratingProcess(int varID, int typeOfGeneratingProcess)
{
  vartable[varID].typeOfGeneratingProcess = typeOfGeneratingProcess;
}


void varDefOptGribInt(int varID, long lval, const char *keyword)
{
#if  defined  (HAVE_LIBGRIB_API)
  int idx = vartable[varID].opt_grib_int_nentries;
  vartable[varID].opt_grib_int_nentries++;
  if ( idx >= MAX_OPT_GRIB_ENTRIES ) Error("Too many optional keyword/integer value pairs!");
  vartable[varID].opt_grib_int_val[idx] = (int) lval;
  vartable[varID].opt_grib_int_keyword[idx] = strdupx(keyword);
#endif
}


void varDefOptGribDbl(int varID, double dval, const char *keyword)
{
#if  defined  (HAVE_LIBGRIB_API)
  int idx = vartable[varID].opt_grib_dbl_nentries;
  vartable[varID].opt_grib_dbl_nentries++;
  if ( idx >= MAX_OPT_GRIB_ENTRIES ) Error("Too many optional keyword/double value pairs!");
  vartable[varID].opt_grib_dbl_val[idx] = dval;
  vartable[varID].opt_grib_dbl_keyword[idx] = strdupx(keyword);
#endif
}


int varOptGribNentries(int varID)
{
  int nentries = 0;
#if  defined  (HAVE_LIBGRIB_API)
  nentries = vartable[varID].opt_grib_int_nentries + vartable[varID].opt_grib_dbl_nentries;
#endif
  return (nentries);
}

/*
 * Local Variables:
 * c-file-style: "Java"
 * c-basic-offset: 2
 * indent-tabs-mode: nil
 * show-trailing-whitespace: t
 * require-trailing-newline: t
 * End:
 */