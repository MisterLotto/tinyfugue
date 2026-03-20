/*************************************************************************
 *  TinyFugue - programmable mud client
 *  Copyright (C) 1993, 1994, 1995, 1996, 1997, 1998, 1999, 2002, 2003, 2004, 2005, 2006-2007 Ken Keys
 *
 *  TinyFugue (aka "tf") is protected under the terms of the GNU
 *  General Public License.  See the file "COPYING" for details.
 ************************************************************************/

#ifndef RESTART_H
#define RESTART_H

extern void restart_set_argv(int argc, char **argv);
extern int restart_resume(const char *path);
extern int restart_exec(void);

#endif /* RESTART_H */
