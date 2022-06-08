/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
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
#include <stdlib.h>
#include <errno.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/lock.h"
#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/app.h"
#include "openpbx/options.h"

static char *tdesc = "Generic System() application";

static char *app = "System";

static char *app2 = "TrySystem";

static char *synopsis = "Execute a system command";

static char *synopsis2 = "Try executing a system command";

static char *chanvar = "SYSTEMSTATUS";

static char *descrip =
"  System(command): Executes a command  by  using  system(). Returns -1 on\n"
"failure to execute the specified command. \n"
"Result of execution is returned in the SYSTEMSTATUS channel variable:\n"
"   FAILURE	Could not execute the specified command\n"
"   SUCCESS	Specified command successfully executed\n"
"\n"
"Old behaviour:\n"
"If the command itself executes but is in error, and if there exists\n"
"a priority n + 101, where 'n' is the priority of the current instance,\n"
"then  the  channel  will  be  setup to continue at that priority level.\n"
"Note that this jump functionality has been deprecated and will only occur\n"
"if the global priority jumping option is enabled in extensions.conf.\n"
" Otherwise, System returns 0.\n";

static char *descrip2 =
"  TrySystem(command): Executes a command  by  using  system(). Returns 0\n"
"on any situation.\n"
"Result of execution is returned in the SYSTEMSTATUS channel variable:\n"
"   FAILURE	Could not execute the specified command\n"
"   SUCCESS	Specified command successfully executed\n"
"   APPERROR	Specified command successfully executed, but returned error code\n"
"\n"
"Old behaviour:\nIf  the command itself executes but is in error, and if\n"
"there exists a priority n + 101, where 'n' is the priority of the current\n"
"instance, then  the  channel  will  be  setup  to continue at that\n"
"priority level.  Otherwise, System returns 0.\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int system_exec_helper(struct opbx_channel *chan, void *data, int failmode)
{
	int res=0;
	struct localuser *u;
	
	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "System requires an argument(command)\n");
		pbx_builtin_setvar_helper(chan, chanvar, "FAILURE");
		return failmode;
	}

	LOCAL_USER_ADD(u);

	/* Do our thing here */
	res = opbx_safe_system((char *)data);
	if ((res < 0) && (errno != ECHILD)) {
		opbx_log(LOG_WARNING, "Unable to execute '%s'\n", (char *)data);
		pbx_builtin_setvar_helper(chan, chanvar, "FAILURE");
		res = failmode;
	} else if (res == 127) {
		opbx_log(LOG_WARNING, "Unable to execute '%s'\n", (char *)data);
		pbx_builtin_setvar_helper(chan, chanvar, "FAILURE");
		res = failmode;
	} else {
		if (res < 0) 
			res = 0;
		if (option_priority_jumping && res)
			opbx_goto_if_exists(chan, chan->context, chan->exten, chan->priority + 101);

		if (res != 0)
			pbx_builtin_setvar_helper(chan, chanvar, "APPERROR");
		else
			pbx_builtin_setvar_helper(chan, chanvar, "SUCCESS");
		res = 0;
	} 

	LOCAL_USER_REMOVE(u);

	return res;
}

static int system_exec(struct opbx_channel *chan, void *data)
{
	return system_exec_helper(chan, data, -1);
}

static int trysystem_exec(struct opbx_channel *chan, void *data)
{
	return system_exec_helper(chan, data, 0);
}

int unload_module(void)
{
	STANDARD_HANGUP_LOCALUSERS;
	opbx_unregister_application(app2);
	return opbx_unregister_application(app);
}

int load_module(void)
{
	opbx_register_application(app2, trysystem_exec, synopsis2, descrip2);
	return opbx_register_application(app, system_exec, synopsis, descrip);
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


