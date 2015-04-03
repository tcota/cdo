/*
  This file is part of CDO. CDO is a collection of Operators to
  manipulate and analyse Climate model Data.

  Copyright (C) 2003-2014 Uwe Schulzweida, <uwe.schulzweida AT mpimet.mpg.de>
  See COPYING file for copying and redistribution conditions.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
*/

#ifndef _UTIL_H
#define _UTIL_H

typedef struct {
  int    argc;
  int    argl;
  char **argv;
  char  *args;
} argument_t;

argument_t *file_argument_new(const char *filename);
void        file_argument_free(argument_t *argument);
argument_t *argument_new(size_t argc, size_t len);
void        argument_free(argument_t *argument);
void        argument_fill(argument_t *argument, int argc, char *argv[]);

char *getProgname(char *string);
char *getOperator(const char *argument);
char *getOperatorName(const char *xoperator);

argument_t makeArgument(int argc, char *argv[]);
char *getFileArg(char *argument);

enum {START_DEC, START_JAN};
int get_season_start(void);
void get_season_name(const char *seas_name[]);

void init_is_tty(void);

void progressInit(void);
void progressStatus(double offset, double refval, double curval);

int fileExists(const char *filename);
int userFileOverwrite(const char *filename);

/* convert a CDI datatype to string */
int datatype2str(int datatype, char *datatypestr);
int str2datatype(const char *datatypestr);

/* filename manipulation */
const char *filetypeext(int filetype);
void rm_filetypeext(char *file, const char *ext);
void repl_filetypeext(char file[], const char *oldext, const char *newext);


/* moved here from cdo.h */
void    cdiOpenError(int cdiErrno, const char *fmt, const char *path);
void    cdoAbort(const char *fmt, ...);
void    cdoWarning(const char *fmt, ...);
void    cdoPrint(const char *fmt, ...);

int  timer_new(const char *text);
void timer_report(void);
void timer_start(int it);
void timer_stop(int it);
double timer_val(int it);


void    operatorInputArg(const char *enter);
int     operatorArgc(void);
char  **operatorArgv(void);
void    operatorCheckArgc(int numargs);

const argument_t *cdoStreamName(int cnt);

void    cdoInitialize(void *argument);
void    cdoFinish(void);

int     cdoStreamNumber(void);
int     cdoStreamCnt(void);
int     cdoOperatorAdd(const char *name, int func, int intval, const char *enter);
int     cdoOperatorID(void);
int     cdoOperatorF1(int operID);
int     cdoOperatorF2(int operID);
const char *cdoOperatorName(int operID);
const char *cdoOperatorEnter(int operID);

int     cdoFiletype(void);

void    cdoInqHistory(int fileID);
void    cdoDefHistory(int fileID, char *histstring);

int     cdoDefineGrid(const char *gridfile);
int     cdoDefineZaxis(const char *zaxisfile);

int     vlistInqNWPV(int vlistID, int varID);
int     vlistIsSzipped(int vlistID);

void cdoGenFileSuffix(char *filesuffix, size_t maxlen, int filetype, int vlistID, const char *refname);

void writeNCgrid(const char *gridfile, int gridID, int *imask);
void defineZaxis(const char *zaxisarg);
void cdiDefTableID(int tableID);

int gridFromName(const char *gridname);
int zaxisFromName(const char *zaxisname);

#endif  /* _UTIL_H */
