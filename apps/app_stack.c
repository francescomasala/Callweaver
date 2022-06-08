/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (c) 2004-2005 Tilghman Lesher <app_stack_v002@the-tilghman.com>.
 *
 * This code is released by the author with no restrictions on usage.
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
 * \brief Stack applications Gosub, Return, etc.
 * 
 * \ingroup applications
 */

#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "openpbx.h"

#include "openpbx/options.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/chanvars.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/config.h"

#define STACKVAR	"~GOSUB~STACK~"

static const char *tdesc = "Stack Routines";

static const char *app_gosub = "Gosub";
static const char *app_gosubif = "GosubIf";
static const char *app_return = "Return";
static const char *app_pop = "StackPop";

static const char *gosub_synopsis = "Jump to label, saving return address";
static const char *gosubif_synopsis = "Jump to label, saving return address";
static const char *return_synopsis = "Return from gosub routine";
static const char *pop_synopsis = "Remove one address from gosub stack";

static const char *gosub_descrip =
"Gosub([[context|]exten|]priority)\n"
"  Jumps to the label specified, saving the return address.\n"
"  Returns 0 if the label exists or -1 otherwise.\n";
static const char *gosubif_descrip =
"Gosub(condition?labeliftrue[:labeliffalse])\n"
"  If the condition is true, then jump to labeliftrue.  If false, jumps to\n"
"labeliffalse, if specified.  In either case, a jump saves the return point\n"
"in the dialplan, to be returned to with a Return.\n"
"  Returns 0 if the label exists or -1 otherwise.\n";
static const char *return_descrip =
"Return()\n"
"  Jumps to the last label in the stack, removing it.\n"
"  Returns 0 if there's a label in the stack or -1 otherwise.\n";
static const char *pop_descrip =
"StackPop()\n"
"  Removes last label in the stack, discarding it.\n"
"  Always returns 0, even if the stack is empty.\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int pop_exec(struct opbx_channel *chan, void *data)
{
	pbx_builtin_setvar_helper(chan, STACKVAR, NULL);

	return 0;
}

static int return_exec(struct opbx_channel *chan, void *data)
{
	char *label = pbx_builtin_getvar_helper(chan, STACKVAR);

	if (opbx_strlen_zero(label)) {
		opbx_log(LOG_ERROR, "Return without Gosub: stack is empty\n");
		return -1;
	} else if (opbx_parseable_goto(chan, label)) {
		opbx_log(LOG_WARNING, "No next statement after Gosub?\n");
		return -1;
	}

	pbx_builtin_setvar_helper(chan, STACKVAR, NULL);
	return 0;
}

static int gosub_exec(struct opbx_channel *chan, void *data)
{
	char newlabel[OPBX_MAX_EXTENSION * 2 + 3 + 11];
	struct localuser *u;

	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_ERROR, "%s requires an argument: %s([[context|]exten|]priority)\n", app_gosub, app_gosub);
		return -1;
	}

	LOCAL_USER_ADD(u);
	snprintf(newlabel, sizeof(newlabel), "%s|%s|%d", chan->context, chan->exten, chan->priority + 1);

	if (opbx_parseable_goto(chan, data)) {
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	pbx_builtin_pushvar_helper(chan, STACKVAR, newlabel);
	LOCAL_USER_REMOVE(u);

	return 0;
}

static int gosubif_exec(struct opbx_channel *chan, void *data)
{
	struct localuser *u;
	char *condition="", *label1, *label2, *args;
	int res=0;

	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "GosubIf requires an argument\n");
		return 0;
	}

	args = opbx_strdupa((char *)data);
	if (!args) {
		opbx_log(LOG_ERROR, "Out of memory\n");
		return -1;
	}

	LOCAL_USER_ADD(u);

	condition = strsep(&args, "?");
	label1 = strsep(&args, ":");
	label2 = args;

	if (opbx_true(condition)) {
		if (label1) {
			res = gosub_exec(chan, label1);
		}
	} else if (label2) {
		res = gosub_exec(chan, label2);
	}

	LOCAL_USER_REMOVE(u);
	return res;
}

int unload_module(void)
{
	opbx_unregister_application(app_return);
	opbx_unregister_application(app_pop);
	opbx_unregister_application(app_gosubif);
	opbx_unregister_application(app_gosub);

	STANDARD_HANGUP_LOCALUSERS;

	return 0;
}

int load_module(void)
{
	opbx_register_application(app_pop, pop_exec, pop_synopsis, pop_descrip);
	opbx_register_application(app_return, return_exec, return_synopsis, return_descrip);
	opbx_register_application(app_gosubif, gosubif_exec, gosubif_synopsis, gosubif_descrip);
	opbx_register_application(app_gosub, gosub_exec, gosub_synopsis, gosub_descrip);

	return 0;
}

char *description(void)
{
	return (char *) tdesc;
}

int usecount(void)
{
	int res;

	STANDARD_USECOUNT(res);

	return res;
}
