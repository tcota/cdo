#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#ifdef USE_MPI

#include <inttypes.h>
#include <stdlib.h>

#include "pio_comm.h"
#include "pio_impl.h"
#include "pio_util.h"

extern char *command2charP[];
extern double accumWait;
extern char *token;
extern long initial_buffersize;

typedef struct
{
  size_t size;
  struct dBuffer *db1;
  struct dBuffer *db2;
  struct dBuffer *db;
  IO_Server_command command;
  MPI_Request request;
  int tsID, fileID;
  char name[];
} remoteFileBuf;

static listSet * bibRemoteFileBuf;

static int
fileIDTest(void *a, void *fileID)
{
  return ((remoteFileBuf *)a)->fileID == (int)(intptr_t)fileID;
}

static remoteFileBuf *
initRemoteFileBuf(const char *filename, size_t bs)
{
  remoteFileBuf *afp;
  size_t len;
  int iret;

  xdebug ( "filename=%s, buffersize=%zu, in", filename, bs );

  len = strlen(filename);
  afp = xmalloc(sizeof (remoteFileBuf) + len + 1);
  strcpy(afp->name, filename);
  afp->size = bs;
  afp->tsID = 0;

  /* init output buffer */

  xdebug ( "filename=%s, init output buffer",  afp->name );

  iret = dbuffer_init ( &( afp->db1 ), afp->size );
  iret += dbuffer_init ( &( afp->db2 ), afp->size );

  if ( iret > 0 ) xabort ( "dbuffer_init did not succeed" );

  afp->db = afp->db1;

  afp->command = IO_Open_file;
  afp->request = MPI_REQUEST_NULL;

  xdebug ( "added name=%s, return", afp->name );
  return afp;
}

static int
destroyRemoteFileBuf(void *v)
{
  remoteFileBuf *afp = ( remoteFileBuf * ) v;
  MPI_Status status;

  xdebug ( "filename=%s, cleanup, in", afp->name );

  xmpiStat(MPI_Wait(&afp->request, &status), &status);
  dbuffer_cleanup(&afp->db1);
  dbuffer_cleanup(&afp->db2);

  free(afp);

  xdebug("%s", "cleaned up, return");

  return 0;
}

static bool
compareNames(void *v1, void *v2)
{
  remoteFileBuf *afd1 = v1, *afd2 = v2;

  return !strcmp(afd1->name, afd2->name);
}

/***************************************************************/
/* send buffer to writer and swap buffer for filling */
static void
sendP(remoteFileBuf *afd, int id)
{
  int tag;
  size_t amount;
  MPI_Status status;
  double startTime;

  amount = dbuffer_data_size ( afd->db );
  tag = encodeFileOpTag(id, afd->command);

  xdebug("send buffer for %s, size: %zu bytes, command=%s, in",
         afd->name, amount, command2charP[afd->command]);

  if ( ddebug ) startTime = MPI_Wtime ();

  xmpiStat(MPI_Wait(&(afd->request), &status), &status);

  if ( ddebug ) accumWait +=  ( MPI_Wtime () - startTime );

  xmpi(MPI_Issend(afd->db->buffer, amount, MPI_CHAR, commInqSpecialRankNode(),
                  tag, commInqCommNode(), &( afd->request )));

  /* change outputBuffer */
  dbuffer_reset ( afd->db );
  if ( afd->db == afd->db1 )
    {
      xdebug("%s", "Change to buffer 2 ...");
      afd->db =  afd->db2;
    }
  else
    {
      xdebug("%s", "Change to buffer 1 ...");
      afd->db =  afd->db1;
    }
  afd->command = IO_Send_buffer;

  return;
}

static void
defTimestep(remoteFileBuf *afd, int tsID)
{
  if ( afd == NULL || tsID != afd->tsID + 1 )
    xabort ( " defTimestepPA () didn't succeed." );
  afd->tsID = tsID;
}

static void
flushOp(remoteFileBuf *fb, int tsID)
{
  sendP(fb, fb->fileID);
  defTimestep(fb, (int)(intptr_t)tsID);
}


size_t
pioSendWrite(int id, int tsID, const void *buffer, size_t len)
{
  int error = 0;
  int flush = 0;
  int filled;
  remoteFileBuf *afd;

  afd = listSetGet(bibRemoteFileBuf, fileIDTest, (void *)(intptr_t)id);

  flush = tsID != afd->tsID;

  if ( flush )
    {
      xdebug("tsID = %d, flush buffer for fileID=%d", tsID, afd->fileID);

      flushOp(afd, tsID);
      {
        double startTime;
        MPI_Status status;
        if (ddebug) startTime = MPI_Wtime();
        xmpiStat(MPI_Wait(&(afd->request), &status), &status);
        if (ddebug) accumWait +=  MPI_Wtime() - startTime;
      }
      xmpi(MPI_Barrier(commInqCommColl()));
    }

  filled = dbuffer_push(afd->db, buffer, len);

  xdebug ( "id = %d, tsID = %d, pushed %lu byte data on buffer, filled = %d",
           id, tsID, len, filled);

  if (filled == 1)
    {
      if ( flush )
        error = filled;
      else
        {
          sendP(afd, id);
          error = dbuffer_push(afd->db, buffer, len);
        }
    }

  if ( error == 1 )
    xabort("did not succeed filling output buffer, id=%d", id);

  return len;
}


int
pioSendClose(int id)
{
  double accumWaitMax;
  remoteFileBuf *afd;
  int iret;

  xdebug ( "fileID %d: send buffer, close file and cleanup",id );

  afd = listSetGet(bibRemoteFileBuf, fileIDTest, (void *)(intptr_t)id);

  afd->command = IO_Close_file;

  sendP(afd, id);
  /* wait for other collectors to also close the file
   * this prevents one process from re-using the file ID before
   * another has sent the close */
  xmpi(MPI_Barrier(commInqCommColl()));

  /* remove file element */
  iret = listSetRemove(bibRemoteFileBuf, fileIDTest, (void *)(intptr_t)id);

  /* timer output */
  if ( ddebug )
    {
      enum { root = 0 };
      xmpi(MPI_Reduce(&accumWait, &accumWaitMax,
                      1, MPI_DOUBLE, MPI_MAX, root, commInqCommColl()));
      xdebug ( "Wait time %15.10lf s", accumWait );
      if ( commInqRankColl () == root )
	xdebug ( "Max wait time %15.10lf s", accumWaitMax );
    }
  return iret;
}

int
pioSendOpen(const char *filename)
{
  remoteFileBuf *afd;
  static long buffersize = 0;
  int root = 0, id, iret, messageLength = 32;
  char message[messageLength];
  MPI_Comm commCollectors = commInqCommColl();

  /* broadcast buffersize to collectors */
  if (!buffersize)
    {
      if  (commInqRankColl() == root)
	{
	  if (getenv("BUFSIZE") != NULL)
	    buffersize = atol(getenv("BUFSIZE"));
	  if (buffersize < initial_buffersize)
	    buffersize = initial_buffersize;
          xdebug("filename=%s, broadcast buffersize=%ld to collectors ...",
                 filename, buffersize);
	}
      xmpi(MPI_Bcast(&buffersize, 1, MPI_LONG, root, commCollectors));
    }

  /* init and add remoteFileBuf */
  afd = initRemoteFileBuf(filename, buffersize);
  if ((id = listSetAdd(bibRemoteFileBuf, afd)) < 0)
    xabort("filename %s is not unique", afd->name);
  afd->fileID = id;

  xdebug("filename=%s, init and added remoteFileBuf, return id = %d",
         filename, id);

  /* put filename, id and buffersize on buffer */
  iret = dbuffer_push ( afd->db, filename, strlen ( filename ));
  xassert(iret == 0);
  iret = dbuffer_push ( afd->db, token, 1);
  xassert(iret == 0);
  sprintf ( message,"%lX", buffersize);
  iret = dbuffer_push ( afd->db, message, strlen ( message ));
  xassert(iret == 0);
  iret = dbuffer_push ( afd->db, token, 1);
  xassert(iret == 0);

  if ( ddebug )
    {
      size_t l = strlen(filename) + strlen(message) + 2;
      char *temp = xmalloc(l + 1);
      strncpy(temp, (char *)afd->db->buffer, l);
      temp[l] = '\0';
      xdebug("filename=%s, put Open file message on buffer:\n%s,\t return",
             filename, temp);
      free(temp);
    }
  sendP(afd, afd->fileID);
  xmpi(MPI_Barrier(commCollectors));
  return id;
}

void
pioSendFinalize(void)
{
  int buffer = 0, tag, specialRank = commInqSpecialRankNode ();
  MPI_Comm commNode = commInqCommNode ();

  tag = encodeFileOpTag(0, IO_Finalize);

  xmpi(MPI_Send(&buffer, 1, MPI_INT, specialRank, tag, commNode));
  xdebug("%s", "SENT MESSAGE WITH TAG \"IO_FINALIZE\" TO SPECIAL PROCESS");

  if (!listSetIsEmpty(bibRemoteFileBuf))
    xabort("set bibRemoteFileBuf not empty.");
  else
    {
      xdebug("%s", "destroy set");
      listSetDelete(bibRemoteFileBuf);
    }
}

void
pioSendInitialize(void)
{
  if (commInqSizeNode() < 2)
    xabort ( "USAGE: # IO PROCESSES ON A PHYSICAL NODE >= 2" );


  if ( commInqRankNode () == commInqSpecialRankNode ())
    {
      commDefCommColl ( 0 );
      commSendNodeInfo ();
      commRecvNodeMap ();
      commDefCommsIO ();
      switch ( commInqIOMode ())
        {
        case PIO_WRITER:
          pioWriterStdIO();
          break;
        case PIO_ASYNCH:
          pioWriterAIO();
          break;
        }
    }
  else
    {
      commDefCommColl ( 1 );
      commSendNodeInfo ();
      commRecvNodeMap ();
      commDefCommsIO ();
      bibRemoteFileBuf = listSetNew(destroyRemoteFileBuf, compareNames);
    }
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
