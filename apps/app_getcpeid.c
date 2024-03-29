/*
 * CallWeaver -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * See http://www.callweaver.org for more information about
 * the CallWeaver project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Execute arbitrary system commands
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <spandsp.h>

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/apps/app_getcpeid.c $", "$Revision: 4723 $")

#include "callweaver/lock.h"
#include "callweaver/file.h"
#include "callweaver/logger.h"
#include "callweaver/channel.h"
#include "callweaver/pbx.h"
#include "callweaver/module.h"
#include "callweaver/adsi.h"
#include "callweaver/options.h"

static char *tdesc = "Get ADSI CPE ID";

static void *getcpeid_app;
static const char *getcpeid_name = "GetCPEID";
static const char *getcpeid_synopsis = "Get ADSI CPE ID";
static const char *getcpeid_syntax = "GetCPEID";
static const char *getcpeid_descrip =
"Obtains and displays ADSI CPE ID and other information in order\n"
"to properly setup zapata.conf for on-hook operations.\n"
"Returns -1 on hangup only.\n";


STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int cpeid_setstatus(struct cw_channel *chan, char *stuff[], int voice)
{
	int justify[5] = { ADSI_JUST_CENT, ADSI_JUST_LEFT, ADSI_JUST_LEFT, ADSI_JUST_LEFT };
	char *tmp[5];
	int x;
	for (x=0;x<4;x++)
		tmp[x] = stuff[x];
	tmp[4] = NULL;
	return adsi_print(chan, tmp, justify, voice);
}

static int cpeid_exec(struct cw_channel *chan, int argc, char **argv)
{
	char data[4][80];
	char *stuff[4];
	unsigned char cpeid[4];
	struct localuser *u;
	int gotgeometry = 0;
	int gotcpeid = 0;
	int width, height, buttons;
	int res=0;

	LOCAL_USER_ADD(u);
	stuff[0] = data[0];
	stuff[1] = data[1];
	stuff[2] = data[2];
	stuff[3] = data[3];
	memset(data, 0, sizeof(data));
	strncpy(stuff[0], "** CPE Info **", sizeof(data[0]) - 1);
	strncpy(stuff[1], "Identifying CPE...", sizeof(data[1]) - 1);
	strncpy(stuff[2], "Please wait...", sizeof(data[2]) - 1);
	res = adsi_load_session(chan, NULL, 0, 1);
	if (res > 0) {
		cpeid_setstatus(chan, stuff, 0);
		res = adsi_get_cpeid(chan, cpeid, 0);
		if (res > 0) {
			gotcpeid = 1;
			if (option_verbose > 2)
				cw_verbose(VERBOSE_PREFIX_3 "Got CPEID of '%02x:%02x:%02x:%02x' on '%s'\n", cpeid[0], cpeid[1], cpeid[2], cpeid[3], chan->name);
		}
		if (res > -1) {
			strncpy(stuff[1], "Measuring CPE...", sizeof(data[1]) - 1);
			strncpy(stuff[2], "Please wait...", sizeof(data[2]) - 1);
			cpeid_setstatus(chan, stuff, 0);
			res = adsi_get_cpeinfo(chan, &width, &height, &buttons, 0);
			if (res > -1) {
				if (option_verbose > 2)
					cw_verbose(VERBOSE_PREFIX_3 "CPE has %d lines, %d columns, and %d buttons on '%s'\n", height, width, buttons, chan->name);
				gotgeometry = 1;
			}
		}
		if (res > -1) {
			if (gotcpeid)
				snprintf(stuff[1], sizeof(data[1]), "CPEID: %02x:%02x:%02x:%02x", cpeid[0], cpeid[1], cpeid[2], cpeid[3]);
			else
				strncpy(stuff[1], "CPEID Unknown", sizeof(data[1]) - 1);
			if (gotgeometry) 
				snprintf(stuff[2], sizeof(data[2]), "Geom: %dx%d, %d buttons", width, height, buttons);
			else
				strncpy(stuff[2], "Geometry unknown", sizeof(data[2]) - 1);
			strncpy(stuff[3], "Press # to exit", sizeof(data[3]) - 1);
			cpeid_setstatus(chan, stuff, 1);
			for(;;) {
				res = cw_waitfordigit(chan, 1000);
				if (res < 0)
					break;
				if (res == '#') {
					res = 0;
					break;
				}
			}
			adsi_unload_session(chan);
		}
	}
	LOCAL_USER_REMOVE(u);
	return res;
}

int unload_module(void)
{
	int res = 0;
	STANDARD_HANGUP_LOCALUSERS;
	res |= cw_unregister_application(getcpeid_app);
	return res;
}

int load_module(void)
{
	getcpeid_app = cw_register_application(getcpeid_name, cpeid_exec, getcpeid_synopsis, getcpeid_syntax, getcpeid_descrip);
	return 0;
}

char *description(void)
{
	return tdesc;
}

int usecount(void)
{
	int res;
	STANDARD_USECOUNT(res);
	return res;
}


