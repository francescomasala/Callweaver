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
 * \brief Loopback PBX Module
 *
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

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/config.h"
#include "openpbx/options.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/frame.h"
#include "openpbx/file.h"
#include "openpbx/cli.h"
#include "openpbx/lock.h"
#include "openpbx/linkedlists.h"
#include "openpbx/chanvars.h"
#include "openpbx/sched.h"
#include "openpbx/io.h"
#include "openpbx/utils.h"
#include "openpbx/crypto.h"
#include "openpbx/opbxdb.h"

static char *tdesc = "Loopback Switch";

/* Loopback switch substitutes ${EXTEN}, ${CONTEXT}, and ${PRIORITY} into
   the data passed to it to try to get a string of the form:

	[exten]@context[:priority][/extramatch]
   
   Where exten, context, and priority are another extension, context, and priority
   to lookup and "extramatch" is an extra match restriction the *original* number 
   must fit if  specified.  The "extramatch" begins with _ like an exten pattern
   if it is specified.  Note that the search context MUST be a different context
   from the current context or the search will not succeed in an effort to reduce
   the likelihood of loops (they're still possible if you try hard, so be careful!)

*/


#define LOOPBACK_COMMON \
	char buf[1024]; \
	int res; \
	char *newexten=(char *)exten, *newcontext=(char *)context; \
	int newpriority=priority; \
	char *newpattern=NULL; \
	loopback_helper(buf, sizeof(buf), exten, context, priority, data); \
	loopback_subst(&newexten, &newcontext, &newpriority, &newpattern, buf); \
	opbx_log(LOG_DEBUG, "Parsed into %s @ %s priority %d\n", newexten, newcontext, newpriority); \
	if (!strcasecmp(newcontext, context)) return -1


static char *loopback_helper(char *buf, int buflen, const char *exten, const char *context, int priority, const char *data)
{
	struct opbx_var_t *newvariable;
	struct varshead headp;
	char tmp[80];

	snprintf(tmp, sizeof(tmp), "%d", priority);
	memset(buf, 0, buflen);
	OPBX_LIST_HEAD_INIT_NOLOCK(&headp);
	newvariable = opbx_var_assign("EXTEN", exten);
	OPBX_LIST_INSERT_HEAD(&headp, newvariable, entries);
	newvariable = opbx_var_assign("CONTEXT", context);
	OPBX_LIST_INSERT_HEAD(&headp, newvariable, entries);
	newvariable = opbx_var_assign("PRIORITY", tmp);
	OPBX_LIST_INSERT_HEAD(&headp, newvariable, entries);
	pbx_substitute_variables_varshead(&headp, data, buf, buflen);
	/* Substitute variables */
	while (!OPBX_LIST_EMPTY(&headp)) {           /* List Deletion. */
		newvariable = OPBX_LIST_REMOVE_HEAD(&headp, entries);
		opbx_var_delete(newvariable);
	}
	return buf;
}

static void loopback_subst(char **newexten, char **newcontext, int *priority, char **newpattern, char *buf)
{
	char *con;
	char *pri;
	*newpattern = strchr(buf, '/');
	if (*newpattern) {
		*(*newpattern) = '\0';
		(*newpattern)++;
	}
	con = strchr(buf, '@');
	if (con) {
		*con = '\0';
		con++;
		pri = strchr(con, ':');
	} else
		pri = strchr(buf, ':');
	if (!opbx_strlen_zero(buf))
		*newexten = buf;
	if (con && !opbx_strlen_zero(con))
		*newcontext = con;
	if (pri && !opbx_strlen_zero(pri))
		sscanf(pri, "%d", priority);
}

static int loopback_exists(struct opbx_channel *chan, const char *context, const char *exten, int priority, const char *callerid, const char *data)
{
	LOOPBACK_COMMON;

	res = opbx_exists_extension(chan, newcontext, newexten, newpriority, callerid);
	if (newpattern)
    {
        switch (opbx_extension_pattern_match(exten, newpattern))
        {
        case EXTENSION_MATCH_EXACT:
        case EXTENSION_MATCH_STRETCHABLE:
        case EXTENSION_MATCH_POSSIBLE:
            break;
        default:
            res = 0;
            break;
        }
	}
    return res;
}

static int loopback_canmatch(struct opbx_channel *chan, const char *context, const char *exten, int priority, const char *callerid, const char *data)
{
	LOOPBACK_COMMON;

	res = opbx_canmatch_extension(chan, newcontext, newexten, newpriority, callerid);
	if (newpattern)
    {
        switch (opbx_extension_pattern_match(exten, newpattern))
        {
        case EXTENSION_MATCH_EXACT:
        case EXTENSION_MATCH_STRETCHABLE:
        case EXTENSION_MATCH_POSSIBLE:
            break;
        default:
            res = 0;
            break;
        }
	}
	return res;
}

static int loopback_exec(struct opbx_channel *chan, const char *context, const char *exten, int priority, const char *callerid, int newstack, const char *data)
{
	LOOPBACK_COMMON;

	if (newstack)
		res = opbx_spawn_extension(chan, newcontext, newexten, newpriority, callerid);
	else
		res = opbx_exec_extension(chan, newcontext, newexten, newpriority, callerid);
	if (newpattern)
    {
        switch (opbx_extension_pattern_match(exten, newpattern))
        {
        case EXTENSION_MATCH_EXACT:
        case EXTENSION_MATCH_STRETCHABLE:
        case EXTENSION_MATCH_POSSIBLE:
            break;
        default:
            res = -1;
            break;
        }
	}
	return res;
}

static int loopback_matchmore(struct opbx_channel *chan, const char *context, const char *exten, int priority, const char *callerid, const char *data)
{
	LOOPBACK_COMMON;

	res = opbx_matchmore_extension(chan, newcontext, newexten, newpriority, callerid);
	if (newpattern)
    {
        switch (opbx_extension_pattern_match(exten, newpattern))
        {
        case EXTENSION_MATCH_EXACT:
        case EXTENSION_MATCH_STRETCHABLE:
        case EXTENSION_MATCH_POSSIBLE:
            break;
        default:
            res = 0;
            break;
        }
	}
	return res;
}

static struct opbx_switch loopback_switch =
{
        name:                   "Loopback",
        description:    		"Loopback Dialplan Switch",
        exists:                 loopback_exists,
        canmatch:               loopback_canmatch,
        exec:                   loopback_exec,
        matchmore:              loopback_matchmore,
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
	opbx_unregister_switch(&loopback_switch);
	return 0;
}

int load_module(void)
{
	opbx_register_switch(&loopback_switch);
	return 0;
}

