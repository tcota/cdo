#if defined (HAVE_CONFIG_H)
#  include "config.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef USE_MPI
#include <mpi.h>
#include <yaxt.h>
#else
typedef int MPI_Comm;
#endif

#include "cdi.h"
#ifdef USE_MPI
#include "cdipio.h"
#include "pio_util.h"
#ifdef HAVE_PPM_CORE
#include <ppm/ppm_uniform_partition.h>
#endif
#endif

#include "cksum.h"
#include "dmemory.h"
#include "error.h"
#include "pio_write.h"

#include "simple_model_helper.h"

enum {
  ntfiles     = 2,
};


static void
modelRegionCompute(double region[], size_t offset, size_t len,
                   int nlev, int nlat, int nlon,
                   int tsID, const double lons[], const double lats[],
                   double mscale, double mrscale)
{
  size_t local_pos;
  for (local_pos = 0; local_pos < len; ++local_pos)
    {
      size_t global_pos = offset + local_pos;
      int k = global_pos / (nlon * nlat);
      int j = (global_pos % (nlon * nlat))/ nlon;
      int i = global_pos % nlon;
      region[local_pos]
        = sign_flat(round((cos(2.0 * M_PI * (lons[(i + tsID)%nlon] - lons[0])
                               / (lons[nlon-1] - lons[0]))
                           * sin(2.0 * M_PI * (lats[(j + k)%nlat] - lats[0])
                                 / (lats[nlat-1] - lats[0]))
                           ) * mscale)) * mrscale;
    }
}

void
modelRun(struct model_config setup, MPI_Comm comm)
{
  static const char * const fname_prefix        = "example";

  struct
  {
    size_t size;
    int nlev, zaxisID, id, code;
    uint32_t checksum_state;
#if USE_MPI
    int chunkSize, start;
    Xt_idxlist partDesc;
#endif
  } *varDesc;
  int gridID, taxisID, vlistID, streamID, tsID, tfID = 0;
  int i, nmiss = 0;
  double *lons, *lats, *levs;
  double *var = NULL, *varslice = NULL;
  double mscale, mrscale;
  time_t current_time;
  int vdate = 19850101, vtime = 120000;
  int rank = 0;
  char filename[1024];
  int nlon = setup.nlon, nlat = setup.nlat;
  int nVars = setup.nvars;
  size_t varslice_size = 0;
#if USE_MPI
  int *chunks = NULL, *displs = NULL, comm_size = 1;
#endif

#if USE_MPI
  xmpi ( MPI_Comm_rank ( comm, &rank ));
  xmpi ( MPI_Comm_size ( comm, &comm_size ));
  if (rank == 0 && setup.compute_checksum)
    {
      chunks = xmalloc(comm_size * sizeof (chunks[0]));
      displs = xmalloc(comm_size * sizeof (displs[0]));
      var = xmalloc((size_t)nlon * (size_t)nlat
                    * (size_t)setup.max_nlev * sizeof(var[0]));
    }
#endif

  var_scale(setup.datatype, &mscale, &mrscale);

  gridID = gridCreate ( GRID_LONLAT, nlon*nlat );
  gridDefXsize ( gridID, nlon );
  gridDefYsize ( gridID, nlat );
  lons = xmalloc(nlon * sizeof (lons[0]));
  for (i = 0; i < nlon; ++i)
    lons[i] = ((double)(i * 360))/nlon;
  lats = xmalloc(nlat * sizeof (lats[0]));
  for (i = 0; i < nlat; ++i)
    lats[i] = ((double)(i * 180))/nlat - 90.0;
  gridDefXvals ( gridID, lons );
  gridDefYvals ( gridID, lats );

  levs = xmalloc(setup.max_nlev * sizeof (levs[0]));
  for (i = 0; i < setup.max_nlev; ++i)
    levs[i] = 101300.0
      - 3940.3 * (exp(1.3579 * (double)(i)/(setup.max_nlev - 1)) - 1.0);

  vlistID = vlistCreate ();

  varDesc = xmalloc(nVars * sizeof (varDesc[0]));
  for (int varIdx = 0; varIdx < nVars; varIdx++ )
    {
      int varLevs = random()%4;
      switch (varLevs)
        {
        case 1:
          varLevs = setup.max_nlev / 3;
          break;
        case 2:
          varLevs = setup.max_nlev >= 11 ? 11 : setup.max_nlev / 2;
          break;
        case 3:
          varLevs = setup.max_nlev - 1;
          break;
        }
      ++varLevs;
      varDesc[varIdx].nlev = varLevs;
      for (i = 0; i < varIdx; ++i)
        if (varDesc[i].nlev == varLevs)
          {
            varDesc[varIdx].zaxisID = varDesc[i].zaxisID;
            goto zaxisIDset;
          }
      varDesc[varIdx].zaxisID
        = zaxisCreate(ZAXIS_PRESSURE, varDesc[varIdx].nlev);
      zaxisDefLevels(varDesc[varIdx].zaxisID, levs);
      zaxisIDset:
      varDesc[varIdx].id = vlistDefVar(vlistID, gridID, varDesc[varIdx].zaxisID,
                                       TIME_VARIABLE);
      varDesc[varIdx].size = nlon * nlat * varDesc[varIdx].nlev;
#ifdef USE_MPI
      {
        struct PPM_extent range
          = PPM_uniform_partition((struct PPM_extent){ 0,
                (int32_t)varDesc[varIdx].size }, comm_size, rank);
        int start = range.first;
        int chunkSize = range.size;
         fprintf(stderr, "%d: start=%d, chunkSize = %d\n", rank,
                 start, chunkSize);
         Xt_idxlist idxlist
           = xt_idxstripes_new(&(struct Xt_stripe){ .start = start,
                 .nstrides = chunkSize, .stride = 1 }, 1);
         varDesc[varIdx].start = start;
         varDesc[varIdx].chunkSize = chunkSize;
         varDesc[varIdx].partDesc = idxlist;
      }
#endif
      varDesc[varIdx].code = 129 + varIdx;
      vlistDefVarCode(vlistID, varDesc[varIdx].id, varDesc[varIdx].code);
      vlistDefVarDatatype(vlistID, varDesc[varIdx].id, setup.datatype);
    }

  taxisID = taxisCreate ( TAXIS_ABSOLUTE );
  vlistDefTaxis ( vlistID, taxisID );

  sprintf ( &filename[0], "%s_%d.%s", fname_prefix, tfID, setup.suffix );
  streamID = streamOpenWrite ( filename, setup.filetype );
  xassert ( streamID >= 0 );
  streamDefVlist ( streamID, vlistID);

#ifdef USE_MPI
  pioEndDef ();
#endif

  for ( tfID = 0; tfID < ntfiles; tfID++ )
    {
      for (int varIdx = 0; varIdx < nVars; ++varIdx)
        varDesc[varIdx].checksum_state = 0;
      if ( tfID > 0 )
	{
	  streamClose ( streamID );
	  sprintf ( &filename[0], "%s_%d.%s", fname_prefix, tfID, setup.suffix );
	  streamID = streamOpenWrite ( filename, setup.filetype );
	  xassert ( streamID >= 0 );
	  streamDefVlist ( streamID, vlistID );
	}
      vdate = 19850101;
      vtime = 120000;
      current_time = cditime2time_t(vdate, vtime);
      for ( tsID = 0; tsID < setup.nts; tsID++ )
	{
          time_t2cditime(current_time, &vdate, &vtime);
	  taxisDefVdate ( taxisID, vdate );
	  taxisDefVtime ( taxisID, vtime );
	  streamDefTimestep ( streamID, tsID );
	  for (int varID = 0; varID < nVars; ++varID)
	    {
#ifdef USE_MPI
              int start = varDesc[varID].start;
              int chunk = varDesc[varID].chunkSize;
#else
              int chunk = varDesc[varID].size;
              int start = 0;
#endif
              if (varslice_size < chunk)
                {
                  varslice = xrealloc(varslice, chunk * sizeof (var[0]));
                  varslice_size = chunk;
                }
              modelRegionCompute(varslice, start, chunk,
                                 varDesc[varID].nlev, nlat, nlon,
                                 tsID, lons, lats,
                                 mscale, mrscale);
              if (setup.compute_checksum)
                {
#if USE_MPI
                  xmpi(MPI_Gather(&chunk, 1, MPI_INT,
                                  chunks, 1, MPI_INT, 0, comm));
                  if (rank == 0)
                    {
                      displs[0] = 0;
                      for (i = 1; i < comm_size; ++i)
                        displs[i] = displs[i - 1] + chunks[i - 1];
                    }
                  xmpi(MPI_Gatherv(varslice, chunk, MPI_DOUBLE,
                                   var, chunks, displs, MPI_DOUBLE, 0, comm));
#else
                  var = varslice;
#endif
                }
              if (rank == 0 && setup.compute_checksum)
                {
                  memcrc_r(&varDesc[varID].checksum_state,
                           (const unsigned char *)var,
                           varDesc[varID].size * sizeof (var[0]));
                }

#ifdef USE_MPI
	      streamWriteVarPart(streamID, varDesc[varID].id, varslice, nmiss,
                                 varDesc[varID].partDesc);
#else
	      streamWriteVar(streamID, varDesc[varID].id, varslice, nmiss);
#endif
	    }
          current_time += 86400;
#ifdef USE_MPI
	  pioWriteTimestep ( tsID, vdate, vtime );
#endif
	}
      if (rank == 0 && setup.compute_checksum)
        {
          FILE *tablefp;
          {
            sprintf(filename, "%s_%d.cksum", fname_prefix, tfID);
            if (!(tablefp = fopen(filename, "w")))
              {
                perror("failed to open table file");
                exit(EXIT_FAILURE);
              }
            for (i = 0; i < nVars; ++i)
              {
                uint32_t cksum;
                int code;
                cksum = memcrc_finish(&varDesc[i].checksum_state,
                                      (off_t)varDesc[i].size
                                      * sizeof (var[0]) * setup.nts);
                code = vlistInqVarCode(vlistID, varDesc[i].id);
                if (fprintf(tablefp, "%08lx %d\n", (unsigned long)cksum,
                            code) < 0)
                  {
                    perror("failed to write table file");
                    exit(EXIT_FAILURE);
                  }
              }
            fclose(tablefp);
          }
        }
    }
  free(varslice);
#ifdef USE_MPI
  pioEndTimestepping ();
#endif
  streamClose ( streamID );
  vlistDestroy ( vlistID );
  taxisDestroy ( taxisID );
  for ( i = 0; i < nVars; i++ )
    {
      int zID = varDesc[i].zaxisID;
      if (zID != CDI_UNDEFID)
        {
          zaxisDestroy(zID);
          for (int j = i + 1; j < nVars; ++j)
            if (zID == varDesc[j].zaxisID)
              varDesc[j].zaxisID = CDI_UNDEFID;
        }
    }
  gridDestroy ( gridID );
#if USE_MPI
  for (int varID = 0; varID < nVars; ++varID)
    xt_idxlist_delete(varDesc[varID].partDesc);
  free(displs);
  free(chunks);
  free(var);
#endif
  free(varDesc);
  free(levs);
  free(lats);
  free(lons);
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