/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) <Year>, <Your Name Here>
 *
 * <Your Name Here> <<You Email Here>>
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
 * \brief Skeleton application
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/lock.h"
#include "openpbx/app.h"

static char *tdesc = "Trivial skeleton Application";
static char *app = "Skel";
static char *synopsis = 
"Skeleton application.";
static char *descrip = "This application is a template to build other applications from.\n"
 " It shows you the basic structure to create your own OpenPBX applications.\n";

#define OPTION_A	(1 << 0)	/* Option A */
#define OPTION_B	(1 << 1)	/* Option B(n) */
#define OPTION_C	(1 << 2)	/* Option C(str) */
#define OPTION_NULL	(1 << 3)	/* Dummy Termination */

OPBX_DECLARE_OPTIONS(app_opts,{
	['a'] = { OPTION_A },
	['b'] = { OPTION_B, 1 },
	['c'] = { OPTION_C, 2 }
});

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int app_exec(struct opbx_channel *chan, void *data)
{
	int res = 0;
	struct opbx_flags flags;
	struct localuser *u;
	char *options=NULL;
	char *dummy = NULL;
	char *args;
	int argc = 0;
	char *opts[2];
	char *argv[2];

	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "%s requires an argument (dummy|[options])\n",app);
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	LOCAL_USER_ADD(u);
	
	/* Do our thing here */

	/* We need to make a copy of the input string if we are going to modify it! */
	args = opbx_strdupa(data);	
	if (!args) {
		opbx_log(LOG_ERROR, "Out of memory!\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}
	
	if ((argc = opbx_separate_app_args(args, '|', argv, sizeof(argv) / sizeof(argv[0])))) {
		dummy = argv[0];
		options = argv[1];
		opbx_parseoptions(app_opts, &flags, opts, options);
	}

	if (!opbx_strlen_zero(dummy)) 
		opbx_log(LOG_NOTICE, "Dummy value is : %s\n", dummy);

	if (opbx_test_flag(&flags, OPTION_A))
		opbx_log(LOG_NOTICE, "Option A is set\n");

	if (opbx_test_flag(&flags, OPTION_B))
		opbx_log(LOG_NOTICE,"Option B is set with : %s\n", opts[0] ? opts[0] : "<unspecified>");

	if (opbx_test_flag(&flags, OPTION_C))
		opbx_log(LOG_NOTICE,"Option C is set with : %s\n", opts[1] ? opts[1] : "<unspecified>");

	LOCAL_USER_REMOVE(u);
	
	return res;
}

int unload_module(void)
{
	STANDARD_HANGUP_LOCALUSERS;
	return opbx_unregister_application(app);
}

int load_module(void)
{
	return opbx_register_application(app, app_exec, synopsis, descrip);
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


