/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 * James Golovich <james@gnuinter.net>
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
 * \brief Check if Channel is Available
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/lock.h"
#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/app.h"
#include "openpbx/devicestate.h"

static char *tdesc = "Check if channel is available";

static char *app = "ChanIsAvail";

static char *synopsis = "Check if channel is available";

static char *descrip = 
"  ChanIsAvail(Technology/resource[&Technology2/resource2...][|option]): \n"
"Checks is any of the requested channels are available.  If none\n"
"of the requested channels are available the new priority will be\n"
"n+101 (unless such a priority does not exist or on error, in which\n"
"case ChanIsAvail will return -1).\n"
"If any of the requested channels are available, the next priority will be n+1,\n"
"the channel variable ${AVAILCHAN} will be set to the name of the available channel\n"
"and the ChanIsAvail app will return 0.\n"
"${AVAILORIGCHAN} is the canonical channel name that was used to create the channel.\n"
"${AVAILSTATUS} is the status code for the channel.\n"
"If the option 's' is specified (state), will consider channel unavailable\n"
"when the channel is in use at all, even if it can take another call.\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int chanavail_exec(struct opbx_channel *chan, void *data)
{
	int res=-1, inuse=-1, option_state=0;
	int status;
	struct localuser *u;
	char *info, tmp[512], trychan[512], *peers, *tech, *number, *rest, *cur, *options, *stringp;
	struct opbx_channel *tempchan;

	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "ChanIsAvail requires an argument (Zap/1&Zap/2)\n");
		return -1;
	}

	LOCAL_USER_ADD(u);

	info = opbx_strdupa(data); 
	stringp = info;
	strsep(&stringp, "|");
	options = strsep(&stringp, "|");
	if (options && *options == 's')
		option_state = 1;
	peers = info;
	if (peers) {
		cur = peers;
		do {
			/* remember where to start next time */
			rest = strchr(cur, '&');
			if (rest) {
				*rest = 0;
				rest++;
			}
			tech = cur;
			number = strchr(tech, '/');
			if (!number) {
				opbx_log(LOG_WARNING, "ChanIsAvail argument takes format ([technology]/[device])\n");
				LOCAL_USER_REMOVE(u);
				return -1;
			}
			*number = '\0';
			number++;
			
			if (option_state) {
				/* If the pbx says in use then don't bother trying further.
				   This is to permit testing if someone's on a call, even if the 
	 			   channel can permit more calls (ie callwaiting, sip calls, etc).  */
                               
				snprintf(trychan, sizeof(trychan), "%s/%s",cur,number);
				status = inuse = opbx_device_state(trychan);
			}
			if ((inuse <= 1) && (tempchan = opbx_request(tech, chan->nativeformats, number, &status))) {
					pbx_builtin_setvar_helper(chan, "AVAILCHAN", tempchan->name);
					/* Store the originally used channel too */
					snprintf(tmp, sizeof(tmp), "%s/%s", tech, number);
					pbx_builtin_setvar_helper(chan, "AVAILORIGCHAN", tmp);
					snprintf(tmp, sizeof(tmp), "%d", status);
					pbx_builtin_setvar_helper(chan, "AVAILSTATUS", tmp);
					opbx_hangup(tempchan);
					tempchan = NULL;
					res = 1;
					break;
			} else {
				snprintf(tmp, sizeof(tmp), "%d", status);
				pbx_builtin_setvar_helper(chan, "AVAILSTATUS", tmp);
			}
			cur = rest;
		} while (cur);
	}
	if (res < 1) {
		pbx_builtin_setvar_helper(chan, "AVAILCHAN", "");
		pbx_builtin_setvar_helper(chan, "AVAILORIGCHAN", "");
		if (opbx_goto_if_exists(chan, chan->context, chan->exten, chan->priority + 101)) {
			LOCAL_USER_REMOVE(u);
			return -1;
		}
	}

	LOCAL_USER_REMOVE(u);
	return 0;
}

int unload_module(void)
{
	STANDARD_HANGUP_LOCALUSERS;
	return opbx_unregister_application(app);
}

int load_module(void)
{
	return opbx_register_application(app, chanavail_exec, synopsis, descrip);
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

