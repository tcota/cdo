/*
  This file is part of CDO. CDO is a collection of Operators to
  manipulate and analyse Climate model Data.

  Copyright (C) 2003-2015 Uwe Schulzweida, <uwe.schulzweida AT mpimet.mpg.de>
  See COPYING file for copying and redistribution conditions.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
*/

#ifndef _PSTREAM_H
#define _PSTREAM_H

#include <sys/types.h> /* off_t */

#define  streamOpenWrite          pstreamOpenWrite
#define  streamOpenRead           pstreamOpenRead
#define  streamOpenAppend         pstreamOpenAppend
#define  streamClose              pstreamClose

#define  streamInqFiletype        pstreamInqFiletype
#define  streamInqByteorder       pstreamInqByteorder

#define  streamInqVlist           pstreamInqVlist
#define  streamDefVlist           pstreamDefVlist

#define  streamDefTimestep        pstreamDefTimestep
#define  streamInqTimestep        pstreamInqTimestep

#define  streamDefRecord          pstreamDefRecord
#define  streamInqRecord          pstreamInqRecord

#define  streamWriteRecord        pstreamWriteRecord
#define  streamWriteRecordF       pstreamWriteRecordF
#define  streamReadRecord         pstreamReadRecord

#define  streamCopyRecord         pstreamCopyRecord

#define  streamInqGRIBinfo        pstreamInqGRIBinfo

#define  vlistCopyFlag            cdoVlistCopyFlag


int     pstreamOpenWrite(const argument_t *argument, int filetype);
int     pstreamOpenRead(const argument_t *argument);
int     pstreamOpenAppend(const argument_t *argument);
void    pstreamClose(int pstreamID);

int     pstreamInqFiletype(int pstreamID);
int     pstreamInqByteorder(int pstreamID);

void    pstreamDefVlist(int pstreamID, int vlistID);
int     pstreamInqVlist(int pstreamID);

void    pstreamDefTimestep(int pstreamID, int tsID);
int     pstreamInqTimestep(int pstreamID, int tsID);

void    pstreamDefRecord(int pstreamID, int  varID, int  levelID);
int     pstreamInqRecord(int pstreamID, int *varID, int *levelID);

void    pstreamWriteRecord(int pstreamID, double *data, int nmiss);
void    pstreamWriteRecordF(int pstreamID, float *data, int nmiss);
void    pstreamReadRecord(int pstreamID, double *data, int *nmiss);
void    pstreamCopyRecord(int pstreamIDdest, int pstreamIDsrc);

void    pstreamInqGRIBinfo(int pstreamID, int *intnum, float *fltnum, off_t *bignum);

void    cdoVlistCopyFlag(int vlistID2, int vlistID1);



#endif  /* _PSTREAM_H */
