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
 * \brief Get extension state int 
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

OPENPBX_FILE_VERSION("$HeadURL: svn://svn.openpbx.org/openpbx/trunk/apps/app_getextstate.c $", "$Revision: 1055 $")

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

static char *tdesc = "Get state for given extension in a context (show hints)";

static char *g_app = "GetExtState";

static char *g_descrip =
	"  GetExtState(extensions1[&extension2]|context): \n"
	"Report the extension state for given extension in a context and saves it in EXTSTATE variable. \n"
	"Valid EXTSTATE values are:\n"
	"0 = idle, 1 = inuse; 2 = busy, \n"
	"4 = unavail, 8 = ringing; -1 unknown; \n"
	"Example: GetExtState(715&523|default)\n";

static char *g_synopsis = "Get state for given extension in a context (show hints)";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int get_extstate(struct opbx_channel *chan, void *data)
{
	char *argv;
	struct localuser *u;

	int res=-1;
	char resc[8]="-1";

	char *exten,*ext;
	char *context;
	char hint[OPBX_MAX_EXTENSION] = "";
	char hints[1024] = "";

	char *cur, *rest;
	int allunavailable = 1, allbusy = 1, allfree = 1;
	int busy = 0, inuse = 0, ring = 0;
			
	LOCAL_USER_ADD(u);

	argv = opbx_strdupa(data);
	
	if (!argv) {
		opbx_log(LOG_ERROR, "Memory allocation failed\n");
		pbx_builtin_setvar_helper(chan, "EXTSTATE", resc );	
		LOCAL_USER_REMOVE(u);
		return RESULT_FAILURE;
	}

	exten   = argv;
	context = strchr(argv, '|');
	if (context) {
            *context = '\0';
	    context++;
	} else {
	    opbx_log(LOG_ERROR, "Ignoring, no context\n");
	    pbx_builtin_setvar_helper(chan, "EXTSTATE", resc );	
	    LOCAL_USER_REMOVE(u);
	    return RESULT_FAILURE;
	}

        if (opbx_strlen_zero(exten)) {
	    opbx_log(LOG_ERROR, "Ignoring, no extension\n");
	    pbx_builtin_setvar_helper(chan, "EXTSTATE", resc );	
	    LOCAL_USER_REMOVE(u);
	    return RESULT_FAILURE;
	}


	ext = opbx_strdupa(exten);
	if (!ext) {
		opbx_log(LOG_ERROR, "Memory allocation failed\n");
		pbx_builtin_setvar_helper(chan, "EXTSTATE", resc );	
		LOCAL_USER_REMOVE(u);
		return RESULT_FAILURE;
	}
	
	cur=ext;

	do {
		rest = strchr(cur, '&');
		if (rest) {
			*rest = 0;
			rest++;
		}
	    opbx_get_hint(hint, sizeof(hint) - 1, NULL, 0, NULL, context, cur);
	    //opbx_log(LOG_DEBUG,"HINT: %s Context: %s Exten: %s\n",hint,context,cur);
	    if (!opbx_strlen_zero(hint)) {
		//let's concat hints!
		if ( strlen(hint)+strlen(hints)+2<sizeof(hints) ) {
		    if ( strlen(hints) ) strcat(hints,"&");
		    strcat(hints,hint);
		}
	    }
	    //opbx_log(LOG_DEBUG,"HINTS: %s \n",hints);
	    cur=rest;
	} while (cur);
	//res=opbx_device_state(hint);
	//res=opbx_extension_state2(hint);

	cur=hints;
	do {
		rest = strchr(cur, '&');
		if (rest) {
			*rest = 0;
			rest++;
		}
	
		res = opbx_device_state(cur);
		//opbx_log(LOG_DEBUG,"Ext: %s State: %d \n",cur,res);
		switch (res) {
		case OPBX_DEVICE_NOT_INUSE:
			allunavailable = 0;
			allbusy = 0;
			break;
		case OPBX_DEVICE_INUSE:
			inuse = 1;
			allunavailable = 0;
			allfree = 0;
			break;
		case OPBX_DEVICE_RINGING:
			ring = 1;
			allunavailable = 0;
			allfree = 0;
			break;
		case OPBX_DEVICE_BUSY:
			allunavailable = 0;
			allfree = 0;
			busy = 1;
			break;
		case OPBX_DEVICE_UNAVAILABLE:
		case OPBX_DEVICE_INVALID:
			allbusy = 0;
			allfree = 0;
			break;
		default:
			allunavailable = 0;
			allbusy = 0;
			allfree = 0;
		}
		cur = rest;
	} while (cur);

        // 0-idle; 1-inuse; 2-busy; 4-unavail 8-ringing
	//opbx_log(LOG_VERBOSE, "allunavailable %d, allbusy %d, allfree %d, busy %d, inuse %d, ring %d \n", allunavailable, allbusy, allfree, busy, inuse, ring );
	if      (!inuse && ring)
		res=8;
	else if (inuse && ring)
		res=1;
	else if (inuse)
		res=1;
	else if (allfree)
		res=0;
	else if (allbusy)		
		res=2;
	else if (allunavailable)
		res=4;
	else if (busy) 
		res=2;
	else 	res=-1;
	
        opbx_log(LOG_DEBUG, "app_getextstate setting EXTSTATE to %d for extension %s in context %s\n",
               res, exten, context);
	snprintf(resc,sizeof(resc),"%d",res);
	pbx_builtin_setvar_helper(chan, "EXTSTATE", resc );	

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

	retval = opbx_register_application(g_app, get_extstate, g_synopsis, g_descrip);
	
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

