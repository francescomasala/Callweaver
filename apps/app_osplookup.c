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
 * \brief Open Settlement Protocol Lookup
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/lock.h"
#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/options.h"
#include "openpbx/config.h"
#include "openpbx/module.h"
#include "openpbx/utils.h"
#include "openpbx/causes.h"
#include "openpbx/astosp.h"

static char *tdesc = "OSP Lookup";

static char *app = "OSPLookup";
static char *app2 = "OSPNext";
static char *app3 = "OSPFinish";

static char *synopsis = "Lookup number in OSP";
static char *synopsis2 = "Lookup next OSP entry";
static char *synopsis3 = "Record OSP entry";

static char *descrip = 
"  OSPLookup(exten[|provider[|options]]):  Looks up an extension via OSP and sets\n"
"the variables, where 'n' is the number of the result beginning with 1:\n"
" ${OSPTECH}:   The technology to use for the call\n"
" ${OSPDEST}:   The destination to use for the call\n"
" ${OSPTOKEN}:  The actual OSP token as a string\n"
" ${OSPHANDLE}: The OSP Handle for anything remaining\n"
" ${OSPRESULTS}: The number of OSP results total remaining\n"
"\n"
"If the lookup was *not* successful and there exists a priority n + 101,\n"
"then that priority will be taken next.\n" ;

static char *descrip2 = 
"  OSPNext:  Looks up the next OSP Destination for ${OSPHANDLE}\n"
"See OSPLookup for more information\n"
"\n"
"If the lookup was *not* successful and there exists a priority n + 101,\n"
"then that priority will be taken next.\n" ;

static char *descrip3 = 
"  OSPFinish(status):  Records call state for ${OSPHANDLE}, according to\n"
"status, which should be one of BUSY, CONGESTION, ANSWER, NOANSWER, or NOCHANAVAIL\n"
"or coincidentally, just what the Dial application stores in its ${DIALSTATUS}\n"
"\n"
"If the finishing was *not* successful and there exists a priority n + 101,\n"
"then that priority will be taken next.\n" ;

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int str2cause(char *cause)
{
	if (!strcasecmp(cause, "BUSY"))
		return OPBX_CAUSE_BUSY;
	if (!strcasecmp(cause, "CONGESTION"))
		return OPBX_CAUSE_CONGESTION;
	if (!strcasecmp(cause, "ANSWER"))
		return OPBX_CAUSE_NORMAL;
	if (!strcasecmp(cause, "CANCEL"))
		return OPBX_CAUSE_NORMAL;
	if (!strcasecmp(cause, "NOANSWER"))
		return OPBX_CAUSE_NOANSWER;
	if (!strcasecmp(cause, "NOCHANAVAIL"))
		return OPBX_CAUSE_CONGESTION;
	opbx_log(LOG_WARNING, "Unknown cause '%s', using NORMAL\n", cause);
	return OPBX_CAUSE_NORMAL;
}

static int osplookup_exec(struct opbx_channel *chan, void *data)
{
	int res=0;
	struct localuser *u;
	char *temp;
	char *provider, *opts=NULL;
	struct opbx_osp_result result;
	
	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "OSPLookup requires an argument (extension)\n");
		return -1;
	}

	LOCAL_USER_ADD(u);

	temp = opbx_strdupa(data);
	if (!temp) {
		opbx_log(LOG_ERROR, "Out of memory!\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	provider = strchr(temp, '|');
	if (provider) {
		*provider = '\0';
		provider++;
		opts = strchr(provider, '|');
		if (opts) {
			*opts = '\0';
			opts++;
		}
	}
	
	opbx_log(LOG_DEBUG, "Whoo hoo, looking up OSP on '%s' via '%s'\n", temp, provider ? provider : "<default>");
	if ((res = opbx_osp_lookup(chan, provider, temp, chan->cid.cid_num, &result)) > 0) {
		char tmp[80];
		snprintf(tmp, sizeof(tmp), "%d", result.handle);
		pbx_builtin_setvar_helper(chan, "_OSPHANDLE", tmp);
		pbx_builtin_setvar_helper(chan, "_OSPTECH", result.tech);
		pbx_builtin_setvar_helper(chan, "_OSPDEST", result.dest);
		pbx_builtin_setvar_helper(chan, "_OSPTOKEN", result.token);
		snprintf(tmp, sizeof(tmp), "%d", result.numresults);
		pbx_builtin_setvar_helper(chan, "_OSPRESULTS", tmp);

	} else {
		if (!res)
			opbx_log(LOG_NOTICE, "OSP Lookup failed for '%s' (provider '%s')\n", temp, provider ? provider : "<default>");
		else
			opbx_log(LOG_DEBUG, "Got hangup on '%s' while doing OSP Lookup for '%s' (provider '%s')!\n", chan->name, temp, provider ? provider : "<default>" );
	}
	if (!res) {
		/* Look for a "busy" place */
		opbx_goto_if_exists(chan, chan->context, chan->exten, chan->priority + 101);
	} else if (res > 0)
		res = 0;
	LOCAL_USER_REMOVE(u);
	return res;
}

static int ospnext_exec(struct opbx_channel *chan, void *data)
{
	int res=0;
	struct localuser *u;
	char *temp;
	int cause;
	struct opbx_osp_result result;

	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "OSPNext should have an argument (cause)\n");
		return -1;
	}
	
	LOCAL_USER_ADD(u);

	cause = str2cause((char *)data);
	temp = pbx_builtin_getvar_helper(chan, "OSPHANDLE");
	result.handle = -1;
	if (!opbx_strlen_zero(temp) && (sscanf(temp, "%d", &result.handle) == 1) && (result.handle > -1)) {
		if ((res = opbx_osp_next(&result, cause)) > 0) {
			char tmp[80];
			snprintf(tmp, sizeof(tmp), "%d", result.handle);
			pbx_builtin_setvar_helper(chan, "_OSPHANDLE", tmp);
			pbx_builtin_setvar_helper(chan, "_OSPTECH", result.tech);
			pbx_builtin_setvar_helper(chan, "_OSPDEST", result.dest);
			pbx_builtin_setvar_helper(chan, "_OSPTOKEN", result.token);
			snprintf(tmp, sizeof(tmp), "%d", result.numresults);
			pbx_builtin_setvar_helper(chan, "_OSPRESULTS", tmp);
		}
	} else {
		if (!res) {
			if (result.handle < 0)
				opbx_log(LOG_NOTICE, "OSP Lookup Next failed for handle '%d'\n", result.handle);
			else
				opbx_log(LOG_DEBUG, "No OSP handle specified\n");
		} else
			opbx_log(LOG_DEBUG, "Got hangup on '%s' while doing OSP Next!\n", chan->name);
	}
	if (!res) {
		/* Look for a "busy" place */
		opbx_goto_if_exists(chan, chan->context, chan->exten, chan->priority + 101);
	} else if (res > 0)
		res = 0;
	LOCAL_USER_REMOVE(u);
	return res;
}

static int ospfinished_exec(struct opbx_channel *chan, void *data)
{
	int res=0;
	struct localuser *u;
	char *temp;
	int cause;
	time_t start=0, duration=0;
	struct opbx_osp_result result;

	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "OSPFinish should have an argument (cause)\n");
		return -1;
	}

	LOCAL_USER_ADD(u);	

	if (chan->cdr) {
		start = chan->cdr->answer.tv_sec;
		if (start)
			duration = time(NULL) - start;
		else
			duration = 0;
	} else
		opbx_log(LOG_WARNING, "OSPFinish called on channel '%s' with no CDR!\n", chan->name);
	
	cause = str2cause((char *)data);
	temp = pbx_builtin_getvar_helper(chan, "OSPHANDLE");
	result.handle = -1;
	if (!opbx_strlen_zero(temp) && (sscanf(temp, "%d", &result.handle) == 1) && (result.handle > -1)) {
		if (!opbx_osp_terminate(result.handle, cause, start, duration)) {
			pbx_builtin_setvar_helper(chan, "_OSPHANDLE", "");
			res = 1;
		}
	} else {
		if (!res) {
			if (result.handle > -1)
				opbx_log(LOG_NOTICE, "OSP Finish failed for handle '%d'\n", result.handle);
			else
				opbx_log(LOG_DEBUG, "No OSP handle specified\n");
		} else
			opbx_log(LOG_DEBUG, "Got hangup on '%s' while doing OSP Terminate!\n", chan->name);
	}
	if (!res) {
		/* Look for a "busy" place */
		opbx_goto_if_exists(chan, chan->context, chan->exten, chan->priority + 101);
	} else if (res > 0)
		res = 0;
	LOCAL_USER_REMOVE(u);
	return res;
}


int unload_module(void)
{
	int res;
	STANDARD_HANGUP_LOCALUSERS;
	res = opbx_unregister_application(app3);
	res |= opbx_unregister_application(app2);
	res |= opbx_unregister_application(app);
	return res;
}

int load_module(void)
{
	int res;
	res = opbx_register_application(app, osplookup_exec, synopsis, descrip);
	if (res)
		return(res);
	res = opbx_register_application(app2, ospnext_exec, synopsis2, descrip2);
	if (res)
		return(res);
	res = opbx_register_application(app3, ospfinished_exec, synopsis3, descrip3);
	if (res)
		return(res);
	return(0);
}

int reload(void)
{
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



