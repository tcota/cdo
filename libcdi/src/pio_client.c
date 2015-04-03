#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#ifdef USE_MPI
#include <ctype.h>

#include <yaxt.h>

#include "namespace.h"
#include "taxis.h"

#include "pio.h"
#include "pio_client.h"
#include "pio_comm.h"
#include "pio_interface.h"
#include "pio_rpc.h"
#include "pio_util.h"
#include "pio_serialize.h"

static int
cdiPioClientStreamOpen(const char *filename, const char *filemode,
                       int filetype, stream_t *streamptr,
                       int recordBufIsToBeCreated)
{
  union winHeaderEntry header;
  size_t filename_len;
  if ( tolower ( * filemode ) == 'w' )
    {
      statusCode nspStatus = namespaceInqResStatus ();
      switch ( nspStatus )
        {
        case STAGE_DEFINITION:
          break;
        case STAGE_TIMELOOP:
          filename_len = strlen(filename);
          xassert(filename_len > 0 && filename_len < MAXDATAFILENAME);
          header.funcCall
            = (struct funcCallDesc){
            .funcID = STREAMOPEN,
            .funcArgs.newFile = { .fnamelen = (int)filename_len,
                                  .filetype = filetype } };
          pioBufferFuncCall(header, filename, filename_len + 1);
          xdebug("WROTE FUNCTION CALL IN BUFFER OF WINS:  %s, filenamesz=%zu,"
                 " filename=%s, filetype=%d",
                 funcMap[(-1 - STREAMOPEN)], filename_len + 1, filename,
                 filetype);
          break;
        case STAGE_CLEANUP:
          xabort ( "TRANSITION TO IO PROCESSES ALREADY FINISHED." );
          break;
        default:
          xabort ( "INTERNAL ERROR" );
        }
    }
  else
    Error("cdiPIO read support not implemented");
  return 1;
}

static void
cdiPioClientStreamDefVlist_(int streamID, int vlistID)
{
  union winHeaderEntry header;
  statusCode nspStatus = namespaceInqResStatus ();
  switch ( nspStatus )
    {
    case STAGE_DEFINITION:
      break;
    case STAGE_TIMELOOP:
      header.funcCall
        = (struct funcCallDesc){
        .funcID = STREAMDEFVLIST,
        .funcArgs.streamChange = { streamID, vlistID } };
      pioBufferFuncCall(header, NULL, 0);
      xdebug("WROTE FUNCTION CALL IN BUFFER OF WINS:  %s, streamID=%d,"
             " vlistID=%d", funcMap[(-1 - STREAMDEFVLIST)], streamID, vlistID);
      break;
    case STAGE_CLEANUP:
      xabort ( "TRANSITION TO IO PROCESSES ALREADY FINISHED." );
      break;
    default:
      xabort ( "INTERNAL ERROR" );
    }
  cdiStreamDefVlist_(streamID, vlistID);
}

static void
cdiPioClientStreamWriteVar_(int streamID, int varID, int memtype,
                            const void *data, int nmiss)
{
  xabort("parallel writing requires explicit partition information,"
         " use streamWriteVarPart!");
}

static void
cdiPioClientStreamWriteVarChunk_(int streamID, int varID, int memtype,
                                 const int rect[][2],
                                 const void *data, int nmiss)
{
  int vlistID = streamInqVlist(streamID);
  int size = vlistInqVarSize(vlistID, varID),
    varShape[3];
  unsigned ndims = (unsigned)cdiPioQueryVarDims(varShape, vlistID, varID);
  Xt_int varShapeXt[3], chunkShape[3] = { 1, 1, 1 }, origin[3] = { 0, 0, 0 };
  /* FIXME: verify xt_int ranges are good enough */
  for (unsigned i = 0; i < 3; ++i)
    varShapeXt[i] = varShape[i];
  for (unsigned i = 0; i < ndims; ++i)
    chunkShape[i] = rect[i][1] - rect[i][0] + 1;
  int varSize = varShape[0] * varShape[1] * varShape[2];
  xassert(varSize == size);
  Xt_idxlist chunkDesc
    = xt_idxsection_new(0, ndims, varShapeXt, chunkShape, origin);
  pioBufferPartData(streamID, varID, data, nmiss, chunkDesc);
  xt_idxlist_delete(chunkDesc);
}

static void
cdiPioClientStreamWriteVarPart(int streamID, int varID, const void *data,
                               int nmiss, Xt_idxlist partDesc)
{
  switch (namespaceInqResStatus())
    {
    case STAGE_DEFINITION:
      xabort("DEFINITION STAGE: PARALLEL WRITING NOT POSSIBLE.");
      break;
    case STAGE_TIMELOOP:
      pioBufferPartData(streamID, varID, data, nmiss, partDesc);
      return;
    case STAGE_CLEANUP:
      xabort("CLEANUP STAGE: PARALLEL WRITING NOT POSSIBLE.");
      break;
    default:
      xabort("INTERNAL ERROR");
    }
}

#if defined HAVE_LIBNETCDF
static void
cdiPioCdfDefTimestepNOP(stream_t *streamptr, int tsID)
{
}
#endif

static void
cdiPioClientStreamNOP(stream_t *streamptr)
{
}


static void
cdiPioClientStreamClose(stream_t *streamptr, int recordBufIsToBeDeleted)
{
  union winHeaderEntry header;
  statusCode nspStatus = namespaceInqResStatus ();
  switch ( nspStatus )
    {
    case STAGE_DEFINITION:
      break;
    case STAGE_TIMELOOP:
      header.funcCall
        = (struct funcCallDesc){
        .funcID = STREAMCLOSE,
        .funcArgs.streamChange = { streamptr->self, CDI_UNDEFID } };
      pioBufferFuncCall(header, NULL, 0);
      xdebug("WROTE FUNCTION CALL IN BUFFER OF WINS:  %s, streamID=%d",
             funcMap[-1 - STREAMCLOSE], streamptr->self);
      break;
    case STAGE_CLEANUP:
      break;
    default:
      xabort ( "INTERNAL ERROR" );
    }
}

static int
cdiPioClientStreamDefTimestep_(stream_t *streamptr, int tsID)
{
  union winHeaderEntry header;
  statusCode nspStatus = namespaceInqResStatus ();
  int taxisID, buf_size, position;
  char *buf;
  MPI_Comm commCalc;
  switch ( nspStatus )
    {
    case STAGE_DEFINITION:
      break;
    case STAGE_TIMELOOP:
      position = 0;
      taxisID = vlistInqTaxis(streamptr->vlistID);
      header.funcCall
        = (struct funcCallDesc){
        .funcID = STREAMDEFTIMESTEP,
        .funcArgs.streamNewTimestep = { streamptr->self, tsID } };
      commCalc = commInqCommCalc();
      buf_size = reshResourceGetPackSize(taxisID, &taxisOps, &commCalc);
      buf = xmalloc((size_t)buf_size);
      reshPackResource(taxisID, &taxisOps, buf, buf_size, &position,
                       &commCalc);
      pioBufferFuncCall(header, buf, buf_size);
      free(buf);
      break;
    case STAGE_CLEANUP:
      break;
    default:
      xabort ( "INTERNAL ERROR" );
    }
  return cdiStreamDefTimestep_(streamptr, tsID);
}

void
cdiPioClientSetup(int *pioNamespace_, int *pioNamespace)
{
  commEvalPhysNodes ();
  commDefCommsIO ();
  *pioNamespace_ = *pioNamespace = namespaceNew();
  int callerCDINamespace = namespaceGetActive();
  pioNamespaceSetActive(*pioNamespace_);
  serializeSetMPI();
  namespaceSwitchSet(NSSWITCH_STREAM_OPEN_BACKEND,
                     NSSW_FUNC(cdiPioClientStreamOpen));
  namespaceSwitchSet(NSSWITCH_STREAM_DEF_VLIST_,
                     NSSW_FUNC(cdiPioClientStreamDefVlist_));
  namespaceSwitchSet(NSSWITCH_STREAM_WRITE_VAR_,
                     NSSW_FUNC(cdiPioClientStreamWriteVar_));
  namespaceSwitchSet(NSSWITCH_STREAM_WRITE_VAR_CHUNK_,
                     NSSW_FUNC(cdiPioClientStreamWriteVarChunk_));
  namespaceSwitchSet(NSSWITCH_STREAM_WRITE_VAR_PART_,
                     NSSW_FUNC(cdiPioClientStreamWriteVarPart));
  namespaceSwitchSet(NSSWITCH_STREAM_CLOSE_BACKEND,
                     NSSW_FUNC(cdiPioClientStreamClose));
  namespaceSwitchSet(NSSWITCH_STREAM_DEF_TIMESTEP_,
                     NSSW_FUNC(cdiPioClientStreamDefTimestep_));
  namespaceSwitchSet(NSSWITCH_STREAM_SYNC,
                     NSSW_FUNC(cdiPioClientStreamNOP));
#ifdef HAVE_LIBNETCDF
  namespaceSwitchSet(NSSWITCH_CDF_DEF_TIMESTEP,
                     NSSW_FUNC(cdiPioCdfDefTimestepNOP));
  namespaceSwitchSet(NSSWITCH_CDF_STREAM_SETUP,
                     NSSW_FUNC(cdiPioClientStreamNOP));
#endif
  pioNamespaceSetActive(callerCDINamespace);
}


#endif
/*
 * Local Variables:
 * c-file-style: "Java"
 * c-basic-offset: 2
 * indent-tabs-mode: nil
 * show-trailing-whitespace: t
 * require-trailing-newline: t
 * End:
 */