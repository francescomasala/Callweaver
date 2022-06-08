/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright 2004 - 2005, Anthony Minessale <anthmct@yahoo.com>
 *
 * Anthony Minessale <anthmct@yahoo.com>
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
 * \brief While Loop and ExecIf Implementations
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
#include "openpbx/utils.h"
#include "openpbx/config.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/lock.h"
#include "openpbx/options.h"

#define ALL_DONE(u,ret) {LOCAL_USER_REMOVE(u); return ret;}


static char *exec_app = "ExecIf";
static char *exec_desc = 
"Usage:  ExecIF (<expr>|<app>|<data>)\n"
"If <expr> is true, execute and return the result of <app>(<data>).\n"
"If <expr> is true, but <app> is not found, then the application\n"
"will return a non-zero value.";
static char *exec_synopsis = "Conditional exec";

static char *start_app = "While";
static char *start_desc = 
"Usage:  While(<expr>)\n"
"Start a While Loop.  Execution will return to this point when\n"
"EndWhile is called until expr is no longer true.\n";

static char *start_synopsis = "Start A While Loop";


static char *stop_app = "EndWhile";
static char *stop_desc = 
"Usage:  EndWhile()\n"
"Return to the previous called While\n\n";

static char *stop_synopsis = "End A While Loop";

static char *tdesc = "While Loops and Conditional Execution";



STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int execif_exec(struct opbx_channel *chan, void *data) {
	int res=0;
	struct localuser *u;
	char *myapp = NULL;
	char *mydata = NULL;
	char *expr = NULL;
	struct opbx_app *app = NULL;

	LOCAL_USER_ADD(u);

	expr = opbx_strdupa(data);
	if (!expr) {
		opbx_log(LOG_ERROR, "Out of memory\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	if ((myapp = strchr(expr,'|'))) {
		*myapp = '\0';
		myapp++;
		if ((mydata = strchr(myapp,'|'))) {
			*mydata = '\0';
			mydata++;
		} else
			mydata = "";

		if (opbx_true(expr)) { 
			if ((app = pbx_findapp(myapp))) {
				res = pbx_exec(chan, app, mydata, 1);
			} else {
				opbx_log(LOG_WARNING, "Count not find application! (%s)\n", myapp);
				res = -1;
			}
		}
	} else {
		opbx_log(LOG_ERROR,"Invalid Syntax.\n");
		res = -1;
	}
		
	ALL_DONE(u,res);
}

#define VAR_SIZE 64


static char *get_index(struct opbx_channel *chan, const char *prefix, int index) {
	char varname[VAR_SIZE];

	snprintf(varname, VAR_SIZE, "%s_%d", prefix, index);
	return pbx_builtin_getvar_helper(chan, varname);
}

static struct opbx_exten *find_matching_priority(struct opbx_context *c, const char *exten, int priority, const char *callerid)
{
	struct opbx_exten *e;
	struct opbx_include *i;
	struct opbx_context *c2;
	struct opbx_exten *p;
    int hit;

	for (e = opbx_walk_context_extensions(c, NULL);  e;  e = opbx_walk_context_extensions(c, e))
    {
        switch (opbx_extension_pattern_match(exten, opbx_get_extension_name(e)))
        {
        case EXTENSION_MATCH_EXACT:
        case EXTENSION_MATCH_STRETCHABLE:
        case EXTENSION_MATCH_POSSIBLE:
			if (opbx_get_extension_matchcid(e))
            {
                switch (opbx_extension_pattern_match(callerid, opbx_get_extension_cidmatch(e)))
                {
                case EXTENSION_MATCH_EXACT:
                case EXTENSION_MATCH_STRETCHABLE:
                case EXTENSION_MATCH_POSSIBLE:
                    hit = 1;
                    break;
                default:
                    hit = 0;
                    break;
                }
            }
            else
            {
                hit = 1;
            }
			if (hit)
            {
				/* This is the matching extension we want */
				for (p = opbx_walk_extension_priorities(e, NULL);  p;  p = opbx_walk_extension_priorities(e, p))
                {
					if (priority != opbx_get_extension_priority(p))
						continue;
					return p;
				}
			}
            break;
		}
	}

	/* No match; run through includes */
	for (i = opbx_walk_context_includes(c, NULL);  i;  i = opbx_walk_context_includes(c, i))
    {
		for (c2 = opbx_walk_contexts(NULL);  c2;  c2 = opbx_walk_contexts(c2))
        {
			if (!strcmp(opbx_get_context_name(c2), opbx_get_include_name(i)))
            {
				if ((e = find_matching_priority(c2, exten, priority, callerid)))
					return e;
			}
		}
	}
	return NULL;
}

static int find_matching_endwhile(struct opbx_channel *chan)
{
	struct opbx_context *c;
	int res = -1;

	if (opbx_lock_contexts())
    {
		opbx_log(LOG_ERROR, "Failed to lock contexts list\n");
		return -1;
	}

	for (c=opbx_walk_contexts(NULL); c; c=opbx_walk_contexts(c)) {
		struct opbx_exten *e;

		if (!opbx_lock_context(c)) {
			if (!strcmp(opbx_get_context_name(c), chan->context)) {
				/* This is the matching context we want */
				int cur_priority = chan->priority + 1, level=1;

				for (e = find_matching_priority(c, chan->exten, cur_priority, chan->cid.cid_num); e; e = find_matching_priority(c, chan->exten, ++cur_priority, chan->cid.cid_num)) {
					if (!strcasecmp(opbx_get_extension_app(e), "WHILE")) {
						level++;
					} else if (!strcasecmp(opbx_get_extension_app(e), "ENDWHILE")) {
						level--;
					}

					if (level == 0) {
						res = cur_priority;
						break;
					}
				}
			}
			opbx_unlock_context(c);
			if (res > 0) {
				break;
			}
		}
	}
	opbx_unlock_contexts();
	return res;
}

static int _while_exec(struct opbx_channel *chan, void *data, int end)
{
	int res=0;
	struct localuser *u;
	char *while_pri = NULL;
	char *goto_str = NULL, *my_name = NULL;
	char *condition = NULL, *label = NULL;
	char varname[VAR_SIZE], end_varname[VAR_SIZE];
	const char *prefix = "WHILE";
	size_t size=0;
	int used_index_i = -1, x=0;
	char used_index[VAR_SIZE] = "0", new_index[VAR_SIZE] = "0";
	
	if (!chan) {
		/* huh ? */
		return -1;
	}

	LOCAL_USER_ADD(u);

	/* dont want run away loops if the chan isn't even up
	   this is up for debate since it slows things down a tad ......
	*/
	if (opbx_waitfordigit(chan,1) < 0)
		ALL_DONE(u,-1);


	for (x=0;;x++) {
		if (get_index(chan, prefix, x)) {
			used_index_i = x;
		} else 
			break;
	}
	
	snprintf(used_index, VAR_SIZE, "%d", used_index_i);
	snprintf(new_index, VAR_SIZE, "%d", used_index_i + 1);
	
	if (!end) {
		condition = opbx_strdupa((char *) data);
	}

	size = strlen(chan->context) + strlen(chan->exten) + 32;
	my_name = alloca(size);
	memset(my_name, 0, size);
	snprintf(my_name, size, "%s_%s_%d", chan->context, chan->exten, chan->priority);
	
	if (opbx_strlen_zero(label)) {
		if (end) 
			label = used_index;
		else if (!(label = pbx_builtin_getvar_helper(chan, my_name))) {
			label = new_index;
			pbx_builtin_setvar_helper(chan, my_name, label);
		}
		
	}
	
	snprintf(varname, VAR_SIZE, "%s_%s", prefix, label);
	while_pri = pbx_builtin_getvar_helper(chan, varname);
	
	if ((while_pri = pbx_builtin_getvar_helper(chan, varname)) && !end) {
		snprintf(end_varname,VAR_SIZE,"END_%s",varname);
	}
	

	if (!end && !opbx_true(condition)) {
		/* Condition Met (clean up helper vars) */
		pbx_builtin_setvar_helper(chan, varname, NULL);
		pbx_builtin_setvar_helper(chan, my_name, NULL);
        snprintf(end_varname,VAR_SIZE,"END_%s",varname);
		if ((goto_str=pbx_builtin_getvar_helper(chan, end_varname))) {
			pbx_builtin_setvar_helper(chan, end_varname, NULL);
			opbx_parseable_goto(chan, goto_str);
		} else {
			int pri = find_matching_endwhile(chan);
			if (pri > 0) {
				if (option_verbose > 2)
					opbx_verbose(VERBOSE_PREFIX_3 "Jumping to priority %d\n", pri);
				chan->priority = pri;
			} else {
				opbx_log(LOG_WARNING, "Couldn't find matching EndWhile? (While at %s@%s priority %d)\n", chan->context, chan->exten, chan->priority);
			}
		}
		ALL_DONE(u,res);
	}

	if (!end && !while_pri) {
		size = strlen(chan->context) + strlen(chan->exten) + 32;
		goto_str = alloca(size);
		memset(goto_str, 0, size);
		snprintf(goto_str, size, "%s|%s|%d", chan->context, chan->exten, chan->priority);
		pbx_builtin_setvar_helper(chan, varname, goto_str);
	} 

	else if (end && while_pri) {
		/* END of loop */
		snprintf(end_varname, VAR_SIZE, "END_%s", varname);
		if (! pbx_builtin_getvar_helper(chan, end_varname)) {
			size = strlen(chan->context) + strlen(chan->exten) + 32;
			goto_str = alloca(size);
			memset(goto_str, 0, size);
			snprintf(goto_str, size, "%s|%s|%d", chan->context, chan->exten, chan->priority+1);
			pbx_builtin_setvar_helper(chan, end_varname, goto_str);
		}
		opbx_parseable_goto(chan, while_pri);
	}
	



	ALL_DONE(u, res);
}

static int while_start_exec(struct opbx_channel *chan, void *data) {
	return _while_exec(chan, data, 0);
}

static int while_end_exec(struct opbx_channel *chan, void *data) {
	return _while_exec(chan, data, 1);
}


int unload_module(void)
{
	STANDARD_HANGUP_LOCALUSERS;
	opbx_unregister_application(start_app);
	opbx_unregister_application(exec_app);
	return opbx_unregister_application(stop_app);
}

int load_module(void)
{
	opbx_register_application(start_app, while_start_exec, start_synopsis, start_desc);
	opbx_register_application(exec_app, execif_exec, exec_synopsis, exec_desc);
	return opbx_register_application(stop_app, while_end_exec, stop_synopsis, stop_desc);
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



