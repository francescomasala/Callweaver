/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 * Tilghman Lesher <tlesher@vcch.com>
 *
 * See http://www.openpbx.org for more information about
 * the OpenPBX project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 * \brief Custom localtime functions for multiple timezones
 */

#ifndef _OPENPBX_LOCALTIME_H
#define _OPENPBX_LOCALTIME_H
#include "confdefs.h"
extern int opbx_tzsetwall(void);
extern void opbx_tzset(const char *name);
extern struct tm *opbx_localtime(const time_t *timep, struct tm *p_tm, const char *zone);
extern time_t opbx_mktime(struct tm * const tmp, const char *zone);
extern char *opbx_ctime(const time_t * const timep);
extern char *opbx_ctime_r(const time_t * const timep, char *buf);

#endif /* _OPENPBX_LOCALTIME_H */
