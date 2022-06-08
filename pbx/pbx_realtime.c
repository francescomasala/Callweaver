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
 * \brief Realtime PBX Module
 *
 * \arg See also: \ref AstARA
 */


#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>


#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision: 46 $")

#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/config.h"
#include "openpbx/options.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/frame.h"
#include "openpbx/term.h"
#include "openpbx/manager.h"
#include "openpbx/cli.h"
#include "openpbx/linkedlists.h"
#include "openpbx/chanvars.h"
#include "openpbx/sched.h"
#include "openpbx/io.h"
#include "openpbx/utils.h"
#include "openpbx/crypto.h"
#include "openpbx/opbxdb.h"

#define MODE_MATCH 		0
#define MODE_MATCHMORE 	1
#define MODE_CANMATCH 	2

#define EXT_DATA_SIZE 256

static char *tdesc = "Realtime Switch";

/* Realtime switch looks up extensions in the supplied realtime table.

	[context@][realtimetable][/options]

	If the realtimetable is omitted it is assumed to be "extensions".  If no context is 
	specified the context is assumed to be whatever is the container.

	The realtime table should have entries for context,exten,priority,app,args
	
	The realtime table currently does not support callerid fields.

*/


#define REALTIME_COMMON(mode) \
	char *buf; \
	char *opts; \
	const char *cxt; \
	char *table; \
	int res=-1; \
	struct opbx_variable *var=NULL; \
	buf = opbx_strdupa(data); \
	if (buf) { \
		opts = strchr(buf, '/'); \
		if (opts) { \
			*opts='\0'; \
			opts++; \
		} else \
			opts=""; \
		table = strchr(buf, '@'); \
		if (table) { \
			*table = '\0'; \
			table++;\
			cxt = buf; \
		} else cxt = NULL; \
		if (!cxt || opbx_strlen_zero(cxt)) \
			cxt = context;\
		if (!table || opbx_strlen_zero(table)) \
			table = "extensions"; \
		var = realtime_switch_common(table, cxt, exten, priority, mode); \
	} else \
		res = -1; 

static struct opbx_variable *realtime_switch_common(const char *table, const char *context, const char *exten, int priority, int mode)
{
	struct opbx_variable *var;
	struct opbx_config *cfg;
	char pri[20];
	char *ematch;
	char rexten[OPBX_MAX_EXTENSION + 20]="";
	int match;

	snprintf(pri, sizeof(pri), "%d", priority);
	switch (mode)
    {
	case MODE_MATCHMORE:
		ematch = "exten LIKE";
		snprintf(rexten, sizeof(rexten), "%s_%%", exten);
		break;
	case MODE_CANMATCH:
		ematch = "exten LIKE";
		snprintf(rexten, sizeof(rexten), "%s%%", exten);
		break;
	case MODE_MATCH:
	default:
		ematch = "exten";
		strncpy(rexten, exten, sizeof(rexten) - 1);
	}
	var = opbx_load_realtime(table, ematch, rexten, "context", context, "priority", pri, NULL);
	if (!var)
    {
		cfg = opbx_load_realtime_multientry(table, "exten LIKE", "\\_%", "context", context, "priority", pri, NULL);	
		if (cfg)
        {
			char *cat = opbx_category_browse(cfg, NULL);

			while (cat)
            {
                match = opbx_extension_pattern_match(exten, cat);
				switch (mode)
                {
				case MODE_MATCHMORE:
                    match = (match == EXTENSION_MATCH_STRETCHABLE  ||  match == EXTENSION_MATCH_INCOMPLETE  ||  match == EXTENSION_MATCH_POSSIBLE);
					break;
				case MODE_CANMATCH:
                    match = (match == EXTENSION_MATCH_EXACT  ||  match == EXTENSION_MATCH_STRETCHABLE  ||  match == EXTENSION_MATCH_INCOMPLETE  ||  match == EXTENSION_MATCH_POSSIBLE);
					break;
				case MODE_MATCH:
				default:
                    match = (match == EXTENSION_MATCH_EXACT  ||  match == EXTENSION_MATCH_STRETCHABLE  ||  match == EXTENSION_MATCH_POSSIBLE);
				}
				if (match)
                {
					var = opbx_category_detach_variables(opbx_category_get(cfg, cat));
					break;
				}
				cat = opbx_category_browse(cfg, cat);
			}
			opbx_config_destroy(cfg);
		}
	}
	return var;
}

static int realtime_exists(struct opbx_channel *chan, const char *context, const char *exten, int priority, const char *callerid, const char *data)
{
	REALTIME_COMMON(MODE_MATCH);
	if (var) opbx_variables_destroy(var);
	if (var)
		res = 1;
	return res > 0 ? res : 0;
}

static int realtime_canmatch(struct opbx_channel *chan, const char *context, const char *exten, int priority, const char *callerid, const char *data)
{
	REALTIME_COMMON(MODE_CANMATCH);
	if (var) opbx_variables_destroy(var);
	if (var)
		res = 1;
	return res > 0 ? res : 0;
}

static int realtime_exec(struct opbx_channel *chan, const char *context, const char *exten, int priority, const char *callerid, int newstack, const char *data)
{
	char app[256];
	char appdata[512]="";
	char *tmp="";
    char tmp1[80];
    char tmp2[80];
    char tmp3[EXT_DATA_SIZE];
	struct opbx_app *a;
	struct opbx_variable *v;
	REALTIME_COMMON(MODE_MATCH);
	if (var) {
		v = var;
		while(v) {
			if (!strcasecmp(v->name, "app"))
				strncpy(app, v->value, sizeof(app) -1 );
			else if (!strcasecmp(v->name, "appdata"))
				tmp = opbx_strdupa(v->value);
			v = v->next;
		}
		opbx_variables_destroy(var);
		if (!opbx_strlen_zero(app)) {
			a = pbx_findapp(app);
			if (a) {
				if(!opbx_strlen_zero(tmp))
				   pbx_substitute_variables_helper(chan, tmp, appdata, sizeof(appdata) - 1);
                if (option_verbose > 2)
					opbx_verbose( VERBOSE_PREFIX_3 "Executing %s(\"%s\", \"%s\")\n",
					    opbx_term_color(tmp1, app, COLOR_BRCYAN, 0, sizeof(tmp1)),
					    opbx_term_color(tmp2, chan->name, COLOR_BRMAGENTA, 0, sizeof(tmp2)),
					    opbx_term_color(tmp3, (!opbx_strlen_zero(appdata) ? (char *)appdata : ""), COLOR_BRMAGENTA, 0, sizeof(tmp3)));
                manager_event(EVENT_FLAG_CALL, "Newexten",
							  "Channel: %s\r\n"
							  "Context: %s\r\n"
							  "Extension: %s\r\n"
							  "Priority: %d\r\n"
							  "Application: %s\r\n"
							  "AppData: %s\r\n"
							  "Uniqueid: %s\r\n",
							  chan->name, chan->context, chan->exten, chan->priority, app, appdata ? appdata : "(NULL)", chan->uniqueid);
				
				res = pbx_exec(chan, a, appdata, newstack);
			} else
				opbx_log(LOG_NOTICE, "No such application '%s' for extension '%s' in context '%s'\n", app, exten, context);
		}
	}
	return res;
}

static int realtime_matchmore(struct opbx_channel *chan, const char *context, const char *exten, int priority, const char *callerid, const char *data)
{
	REALTIME_COMMON(MODE_MATCHMORE);
	if (var) opbx_variables_destroy(var);
	if (var)
		res = 1;
	return res > 0 ? res : 0;
}

static struct opbx_switch realtime_switch =
{
        name:                   "Realtime",
        description:    		"Realtime Dialplan Switch",
        exists:                 realtime_exists,
        canmatch:               realtime_canmatch,
        exec:                   realtime_exec,
        matchmore:              realtime_matchmore,
};

char *description(void)
{
	return tdesc;
}

int usecount(void)
{
	return 1;
}

int unload_module(void)
{
	opbx_unregister_switch(&realtime_switch);
	return 0;
}

int load_module(void)
{
	opbx_register_switch(&realtime_switch);
	return 0;
}

