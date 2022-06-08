/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 * Copyright (C) 2003, Jefferson Noxon
 *
 * Mark Spencer <markster@digium.com>
 * Jefferson Noxon <jeff@debian.org>
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
 *
 * This application written by Massimo Cetra <devel@navynet.it>
 */

/*! \file
 *
 * \brief Get device state int 
 *
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL: svn://svn.openpbx.org/openpbx/trunk/apps/app_getdevstate.c $", "$Revision: 1055 $")

#include "openpbx/options.h"
#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/opbxdb.h"
#include "openpbx/lock.h"
#include "openpbx/devicestate.h"
#include "openpbx/cli.h"	//Needed to have RESULT_SUCCESS and RESULT_FAILURE

static char *tdesc = "Gets device state (show hints)";

static char *g_app = "GetDevState";

static char *g_descrip =
	"  GetDevState(device): \n"
	"Get the device state and saves it in DEVSTATE variable. Valid values are:\n"
	"0 = unknown, 1 = not inuse, 2 = inuse, 3 = busy, 4 = invalid, 5 = unavailable, 6 = ringing"
	"Example: GetDevState(SIP/715)\n";

static char *g_synopsis = "Gets the device state";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int get_devstate(struct opbx_channel *chan, void *data)
{
	char *argv;
	struct localuser *u;
	int res=-1;
	char resc[8]="-1";
	
	LOCAL_USER_ADD(u);

	argv = opbx_strdupa(data);
	if (!argv) {
		opbx_log(LOG_ERROR, "Memory allocation failed\n");
		LOCAL_USER_REMOVE(u);
		pbx_builtin_setvar_helper(chan, "DEVSTATE", resc );	
		return RESULT_FAILURE;
	}

	if ( strlen(argv) )
	{
		res=opbx_device_state(argv);	
	} else {
		opbx_log(LOG_DEBUG, "Ignoring, no parameters\n");
	}

        opbx_log(LOG_DEBUG, "app_getdevstate setting DEVSTATE to %d for device %s \n",
               res, argv);
	snprintf(resc,sizeof(resc),"%d",res);
	pbx_builtin_setvar_helper(chan, "DEVSTATE", resc );	

	LOCAL_USER_REMOVE(u);

	return RESULT_SUCCESS;
}

int unload_module(void)
{
	int retval;

	STANDARD_HANGUP_LOCALUSERS;
	retval = opbx_unregister_application(g_app);

	return retval;
}

int load_module(void)
{
	int retval;

	retval = opbx_register_application(g_app, get_devstate, g_synopsis, g_descrip);
	
	return retval;
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

