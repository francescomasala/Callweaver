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
 * \brief Dial plan proc Implementation
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL: svn://ctrix@svn.openpbx.org/openpbx/trunk/apps/app_proc.c $", "$Revision: 1055 $")

#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/options.h"
#include "openpbx/config.h"
#include "openpbx/utils.h"
#include "openpbx/lock.h"

#define MAX_ARGS 80

/* special result value used to force proc exit */
#define MACRO_EXIT_RESULT 1024

static char *tdesc = "Extension Procs";

static char *descrip =
"  Proc(procname|arg1|arg2...): Executes a procedure using the context\n"
"'proc-<procname>', jumping to the 's' extension of that context and\n"
"executing each step, then returning when the steps end. \n"
"The calling extension, context, and priority are stored in ${PROC_EXTEN}, \n"
"${PROC_CONTEXT} and ${PROC_PRIORITY} respectively.  Arguments become\n"
"${ARG1}, ${ARG2}, etc in the proc context.\n"
"If you Goto out of the Proc context, the Proc will terminate and control\n"
"will be returned at the location of the Goto.\n"
"Proc returns -1 if any step in the proc returns -1, and 0 otherwise.\n" 
"If ${PROC_OFFSET} is set at termination, Proc will attempt to continue\n"
"at priority PROC_OFFSET + N + 1 if such a step exists, and N + 1 otherwise.\n";

static char *if_descrip =
"  ProcIf(<expr>?procname_a[|arg1][:procname_b[|arg1]])\n"
"Executes proc defined in <procname_a> if <expr> is true\n"
"(otherwise <procname_b> if provided)\n"
"Arguments and return values as in application proc()\n";

static char *exit_descrip =
"  ProcExit():\n"
"Causes the currently running proc to exit as if it had\n"
"ended normally by running out of priorities to execute.\n"
"If used outside a proc, will likely cause unexpected\n"
"behavior.\n";

static char *app = "Proc";
static char *if_app = "ProcIf";
static char *exit_app = "ProcExit";

static char *synopsis = "Proc Implementation";
static char *if_synopsis = "Conditional Proc Implementation";
static char *exit_synopsis = "Exit From Proc";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int proc_exec(struct opbx_channel *chan, void *data)
{
	char *tmp;
	char *cur, *rest;
	char *macro;
	char fullmacro[80];
	char varname[80];
	char *oldargs[MAX_ARGS + 1] = { NULL, };
	int argc, x;
	int res=0;
	char oldexten[256]="";
	int oldpriority;
	char pc[80], depthc[12];
	char oldcontext[OPBX_MAX_CONTEXT] = "";
	char *offsets;
	int offset, depth;
	int setmacrocontext=0;
	int autoloopflag;
  
	char *save_macro_exten;
	char *save_macro_context;
	char *save_macro_priority;
	char *save_macro_offset;
	struct localuser *u;
 
	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "Proc() requires arguments. See \"show application Proc\" for help.\n");
		return -1;
	}

	LOCAL_USER_ADD(u);

	/* Count how many levels deep the rabbit hole goes */
	tmp = pbx_builtin_getvar_helper(chan, "PROC_DEPTH");
	if (tmp) {
		sscanf(tmp, "%d", &depth);
	} else {
		depth = 0;
	}

	if (depth >= 7) {
		opbx_log(LOG_ERROR, "Proc():  possible infinite loop detected.  Returning early.\n");
		LOCAL_USER_REMOVE(u);
		return 0;
	}
	snprintf(depthc, sizeof(depthc), "%d", depth + 1);
	pbx_builtin_setvar_helper(chan, "PROC_DEPTH", depthc);

	tmp = opbx_strdupa(data);
	rest = tmp;
	macro = strsep(&rest, "|");
	if (opbx_strlen_zero(macro)) {
		opbx_log(LOG_WARNING, "Invalid proc name specified\n");
		LOCAL_USER_REMOVE(u);
		return 0;
	}
	snprintf(fullmacro, sizeof(fullmacro), "proc-%s", macro);
	if (!opbx_exists_extension(chan, fullmacro, "s", 1, chan->cid.cid_num)) {
  		if (!opbx_context_find(fullmacro)) 
			opbx_log(LOG_WARNING, "No such context '%s' for proc '%s'\n", fullmacro, macro);
		else
	  		opbx_log(LOG_WARNING, "Context '%s' for proc '%s' lacks 's' extension, priority 1\n", fullmacro, macro);
		LOCAL_USER_REMOVE(u);
		return 0;
	}
	
	/* Save old info */
	oldpriority = chan->priority;
	opbx_copy_string(oldexten, chan->exten, sizeof(oldexten));
	opbx_copy_string(oldcontext, chan->context, sizeof(oldcontext));
	if (opbx_strlen_zero(chan->macrocontext)) {
		opbx_copy_string(chan->macrocontext, chan->context, sizeof(chan->macrocontext));
		opbx_copy_string(chan->macroexten, chan->exten, sizeof(chan->macroexten));
		chan->macropriority = chan->priority;
		setmacrocontext=1;
	}
	argc = 1;
	/* Save old macro variables */
	save_macro_exten = pbx_builtin_getvar_helper(chan, "PROC_EXTEN");
	if (save_macro_exten) 
		save_macro_exten = strdup(save_macro_exten);
	pbx_builtin_setvar_helper(chan, "PROC_EXTEN", oldexten);

	save_macro_context = pbx_builtin_getvar_helper(chan, "PROC_CONTEXT");
	if (save_macro_context)
		save_macro_context = strdup(save_macro_context);
	pbx_builtin_setvar_helper(chan, "PROC_CONTEXT", oldcontext);

	save_macro_priority = pbx_builtin_getvar_helper(chan, "PROC_PRIORITY");
	if (save_macro_priority) 
		save_macro_priority = strdup(save_macro_priority);
	snprintf(pc, sizeof(pc), "%d", oldpriority);
	pbx_builtin_setvar_helper(chan, "PROC_PRIORITY", pc);
  
	save_macro_offset = pbx_builtin_getvar_helper(chan, "PROC_OFFSET");
	if (save_macro_offset) 
		save_macro_offset = strdup(save_macro_offset);
	pbx_builtin_setvar_helper(chan, "PROC_OFFSET", NULL);

	/* Setup environment for new run */
	chan->exten[0] = 's';
	chan->exten[1] = '\0';
	opbx_copy_string(chan->context, fullmacro, sizeof(chan->context));
	chan->priority = 1;

	while((cur = strsep(&rest, "|")) && (argc < MAX_ARGS)) {
  		/* Save copy of old arguments if we're overwriting some, otherwise
	   	let them pass through to the other macro */
  		snprintf(varname, sizeof(varname), "ARG%d", argc);
		oldargs[argc] = pbx_builtin_getvar_helper(chan, varname);
		if (oldargs[argc])
			oldargs[argc] = strdup(oldargs[argc]);
		pbx_builtin_setvar_helper(chan, varname, cur);
		argc++;
	}
	autoloopflag = opbx_test_flag(chan, OPBX_FLAG_IN_AUTOLOOP);
	opbx_set_flag(chan, OPBX_FLAG_IN_AUTOLOOP);
	while(opbx_exists_extension(chan, chan->context, chan->exten, chan->priority, chan->cid.cid_num)) {
		/* Reset the macro depth, if it was changed in the last iteration */
		pbx_builtin_setvar_helper(chan, "PROC_DEPTH", depthc);
		if ((res = opbx_spawn_extension(chan, chan->context, chan->exten, chan->priority, chan->cid.cid_num))) {
			/* Something bad happened, or a hangup has been requested. */
			if (((res >= '0') && (res <= '9')) || ((res >= 'A') && (res <= 'F')) ||
		    	(res == '*') || (res == '#')) {
				/* Just return result as to the previous application as if it had been dialed */
				opbx_log(LOG_DEBUG, "Oooh, got something to jump out with ('%c')!\n", res);
				break;
			}
			switch(res) {
	        	case MACRO_EXIT_RESULT:
                        	res = 0;
				goto out;
			case OPBX_PBX_KEEPALIVE:
				if (option_debug)
					opbx_log(LOG_DEBUG, "Spawn extension (%s,%s,%d) exited KEEPALIVE in proc %s on '%s'\n", chan->context, chan->exten, chan->priority, macro, chan->name);
				if (option_verbose > 1)
					opbx_verbose( VERBOSE_PREFIX_2 "Spawn extension (%s, %s, %d) exited KEEPALIVE in proc '%s' on '%s'\n", chan->context, chan->exten, chan->priority, macro, chan->name);
				goto out;
				break;
			default:
				if (option_debug)
					opbx_log(LOG_DEBUG, "Spawn extension (%s,%s,%d) exited non-zero on '%s' in proc '%s'\n", chan->context, chan->exten, chan->priority, chan->name, macro);
				if (option_verbose > 1)
					opbx_verbose( VERBOSE_PREFIX_2 "Spawn extension (%s, %s, %d) exited non-zero on '%s' in proc '%s'\n", chan->context, chan->exten, chan->priority, chan->name, macro);
				goto out;
			}
		}
		if (strcasecmp(chan->context, fullmacro)) {
			if (option_verbose > 1)
				opbx_verbose(VERBOSE_PREFIX_2 "Channel '%s' jumping out of proc '%s'\n", chan->name, macro);
			break;
		}
		/* don't stop executing extensions when we're in "h" */
		if (chan->_softhangup && strcasecmp(oldexten,"h")) {
			opbx_log(LOG_DEBUG, "Extension %s, priority %d returned normally even though call was hung up\n",
				chan->exten, chan->priority);
			goto out;
		}
		chan->priority++;
  	}
	out:
	/* Reset the depth back to what it was when the routine was entered (like if we called Macro recursively) */
	snprintf(depthc, sizeof(depthc), "%d", depth);
	pbx_builtin_setvar_helper(chan, "PROC_DEPTH", depthc);

	opbx_set2_flag(chan, autoloopflag, OPBX_FLAG_IN_AUTOLOOP);
  	for (x=1; x<argc; x++) {
  		/* Restore old arguments and delete ours */
		snprintf(varname, sizeof(varname), "ARG%d", x);
  		if (oldargs[x]) {
			pbx_builtin_setvar_helper(chan, varname, oldargs[x]);
			free(oldargs[x]);
		} else {
			pbx_builtin_setvar_helper(chan, varname, NULL);
		}
  	}

	/* Restore macro variables */
	pbx_builtin_setvar_helper(chan, "PROC_EXTEN", save_macro_exten);
	if (save_macro_exten)
		free(save_macro_exten);
	pbx_builtin_setvar_helper(chan, "PROC_CONTEXT", save_macro_context);
	if (save_macro_context)
		free(save_macro_context);
	pbx_builtin_setvar_helper(chan, "PROC_PRIORITY", save_macro_priority);
	if (save_macro_priority)
		free(save_macro_priority);
	if (setmacrocontext) {
		chan->macrocontext[0] = '\0';
		chan->macroexten[0] = '\0';
		chan->macropriority = 0;
	}

	if (!strcasecmp(chan->context, fullmacro)) {
  		/* If we're leaving the macro normally, restore original information */
		chan->priority = oldpriority;
		opbx_copy_string(chan->context, oldcontext, sizeof(chan->context));
		if (!(chan->_softhangup & OPBX_SOFTHANGUP_ASYNCGOTO)) {
			/* Copy the extension, so long as we're not in softhangup, where we could be given an asyncgoto */
			opbx_copy_string(chan->exten, oldexten, sizeof(chan->exten));
			if ((offsets = pbx_builtin_getvar_helper(chan, "PROC_OFFSET"))) {
				/* Handle macro offset if it's set by checking the availability of step n + offset + 1, otherwise continue
			   	normally if there is any problem */
				if (sscanf(offsets, "%d", &offset) == 1) {
					if (opbx_exists_extension(chan, chan->context, chan->exten, chan->priority + offset + 1, chan->cid.cid_num)) {
						chan->priority += offset;
					}
				}
			}
		}
	}

	pbx_builtin_setvar_helper(chan, "PROC_OFFSET", save_macro_offset);
	if (save_macro_offset)
		free(save_macro_offset);
	LOCAL_USER_REMOVE(u);
	return res;
}

static int procif_exec(struct opbx_channel *chan, void *data) 
{
	char *expr = NULL, *label_a = NULL, *label_b = NULL;
	int res = 0;
	struct localuser *u;

	LOCAL_USER_ADD(u);

	expr = opbx_strdupa(data);
	if (!expr) {
		opbx_log(LOG_ERROR, "Out of Memory!\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	if ((label_a = strchr(expr, '?'))) {
		*label_a = '\0';
		label_a++;
		if ((label_b = strchr(label_a, ':'))) {
			*label_b = '\0';
			label_b++;
		}
		if (opbx_true(expr))
			proc_exec(chan, label_a);
		else if (label_b) 
			proc_exec(chan, label_b);
	} else
		opbx_log(LOG_WARNING, "Invalid Syntax.\n");

	LOCAL_USER_REMOVE(u);

	return res;
}
			
static int proc_exit_exec(struct opbx_channel *chan, void *data)
{
	return MACRO_EXIT_RESULT;
}

int unload_module(void)
{
	STANDARD_HANGUP_LOCALUSERS;
	opbx_unregister_application(if_app);
	opbx_unregister_application(exit_app);
	return opbx_unregister_application(app);
}

int load_module(void)
{
	opbx_register_application(exit_app, proc_exit_exec, exit_synopsis, exit_descrip);
	opbx_register_application(if_app, procif_exec, if_synopsis, if_descrip);
	return opbx_register_application(app, proc_exec, synopsis, descrip);
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


