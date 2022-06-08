/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (c) 2004 - 2005, Tilghman Lesher.  All rights reserved.
 *
 * Tilghman Lesher <app_eval__v001@the-tilghman.com>
 *
 * This code is released by the author with no restrictions on usage.
 *
 * See http://www.openpbx.org for more information about
 * the OpenPBX project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 */

/*! \file
 * \brief Eval application
 *
 * \author Tilghman Lesher <app_eval__v001 at the-tilghman.com>
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/options.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"

/* Maximum length of any variable */
#define MAXRESULT	1024

static char *tdesc = "Reevaluates strings";

static char *app_eval = "Eval";

static char *eval_synopsis = "Evaluates a string";

static char *eval_descrip =
"Usage: Eval(newvar=somestring)\n"
"  Normally OpenPBX evaluates variables inline.  But what if you want to\n"
"store variable offsets in a database, to be evaluated later?  Eval is\n"
"the answer, by allowing a string to be evaluated twice in the dialplan,\n"
"the first time as part of the normal dialplan, and the second using Eval.\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int eval_exec(struct opbx_channel *chan, void *data)
{
	int res=0;
	struct localuser *u;
	char *s, *newvar=NULL, tmp[MAXRESULT];
	static int dep_warning = 0;

	LOCAL_USER_ADD(u);
	
	if (!dep_warning) {
		opbx_log(LOG_WARNING, "This application has been deprecated in favor of the dialplan function, EVAL\n");
		dep_warning = 1;
	}

	/* Check and parse arguments */
	if (data) {
		s = opbx_strdupa((char *)data);
		if (s) {
			newvar = strsep(&s, "=");
			if (newvar && (newvar[0] != '\0')) {
				memset(tmp, 0, MAXRESULT);
				pbx_substitute_variables_helper(chan, s, tmp, MAXRESULT - 1);
				pbx_builtin_setvar_helper(chan, newvar, tmp);
			}
		} else {
			opbx_log(LOG_ERROR, "Out of memory\n");
			res = -1;
		}
	}

	LOCAL_USER_REMOVE(u);
	return res;
}

int unload_module(void)
{
	STANDARD_HANGUP_LOCALUSERS;
	return opbx_unregister_application(app_eval);
}

int load_module(void)
{
	return opbx_register_application(app_eval, eval_exec, eval_synopsis, eval_descrip);
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


