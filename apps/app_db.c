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
 */

/*! \file
 *
 * \brief Database access functions
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

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/options.h"
#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/opbxdb.h"
#include "openpbx/lock.h"

static char *tdesc = "Database access functions for OpenPBX extension logic";

static char *g_descrip =
	"  DBget(varname=family/key): Retrieves a value from the OpenPBX\n"
	"database and stores it in the given variable.  Always returns 0.  If the\n"
	"requested key is not found, jumps to priority n+101 if available.\n";

static char *p_descrip =
	"  DBput(family/key=value): Stores the given value in the OpenPBX\n"
	"database.  Always returns 0.\n";

static char *d_descrip =
	"  DBdel(family/key): Deletes a key from the OpenPBX database.  Always\n"
	"returns 0.\n";

static char *dt_descrip =
	"  DBdelTree(family[/keytree]): Deletes a family or keytree from the OpenPBX\n"
	"database.  Always returns 0.\n";

static char *g_app = "DBget";
static char *p_app = "DBput";
static char *d_app = "DBdel";
static char *dt_app = "DBdelTree";

static char *g_synopsis = "Retrieve a value from the database";
static char *p_synopsis = "Store a value in the database";
static char *d_synopsis = "Delete a key from the database";
static char *dt_synopsis = "Delete a family or keytree from the database";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int deltree_exec(struct opbx_channel *chan, void *data)
{
	char *argv, *family, *keytree;
	struct localuser *u;

	LOCAL_USER_ADD(u);

	argv = opbx_strdupa(data);
	if (!argv) {
		opbx_log(LOG_ERROR, "Memory allocation failed\n");
		LOCAL_USER_REMOVE(u);
		return 0;
	}

	if (strchr(argv, '/')) {
		family = strsep(&argv, "/");
		keytree = strsep(&argv, "\0");
			if (!family || !keytree) {
				opbx_log(LOG_DEBUG, "Ignoring; Syntax error in argument\n");
				LOCAL_USER_REMOVE(u);
				return 0;
			}
		if (opbx_strlen_zero(keytree))
			keytree = 0;
	} else {
		family = argv;
		keytree = 0;
	}

	if (option_verbose > 2)	{
		if (keytree)
			opbx_verbose(VERBOSE_PREFIX_3 "DBdeltree: family=%s, keytree=%s\n", family, keytree);
		else
			opbx_verbose(VERBOSE_PREFIX_3 "DBdeltree: family=%s\n", family);
	}

	if (opbx_db_deltree(family, keytree)) {
		if (option_verbose > 2)
			opbx_verbose(VERBOSE_PREFIX_3 "DBdeltree: Error deleting key from database.\n");
	}

	LOCAL_USER_REMOVE(u);

	return 0;
}

static int del_exec(struct opbx_channel *chan, void *data)
{
	char *argv, *family, *key;
	struct localuser *u;

	LOCAL_USER_ADD(u);

	argv = opbx_strdupa(data);
	if (!argv) {
		opbx_log (LOG_ERROR, "Memory allocation failed\n");
		LOCAL_USER_REMOVE(u);
		return 0;
	}

	if (strchr(argv, '/')) {
		family = strsep(&argv, "/");
		key = strsep(&argv, "\0");
		if (!family || !key) {
			opbx_log(LOG_DEBUG, "Ignoring; Syntax error in argument\n");
			LOCAL_USER_REMOVE(u);
			return 0;
		}
		if (option_verbose > 2)
			opbx_verbose(VERBOSE_PREFIX_3 "DBdel: family=%s, key=%s\n", family, key);
		if (opbx_db_del(family, key)) {
			if (option_verbose > 2)
				opbx_verbose(VERBOSE_PREFIX_3 "DBdel: Error deleting key from database.\n");
		}
	} else {
		opbx_log(LOG_DEBUG, "Ignoring, no parameters\n");
	}

	LOCAL_USER_REMOVE(u);
	
	return 0;
}

static int put_exec(struct opbx_channel *chan, void *data)
{
	char *argv, *value, *family, *key;
	static int dep_warning = 0;
	struct localuser *u;

	LOCAL_USER_ADD(u);

	if (!dep_warning) {
		opbx_log(LOG_WARNING, "This application has been deprecated, please use the ${DB(family/key)} function instead.\n");
		dep_warning = 1;
	}
	
	argv = opbx_strdupa(data);
	if (!argv) {
		opbx_log(LOG_ERROR, "Memory allocation failed\n");
		LOCAL_USER_REMOVE(u);
		return 0;
	}

	if (strchr(argv, '/') && strchr(argv, '=')) {
		family = strsep(&argv, "/");
		key = strsep(&argv, "=");
		value = strsep(&argv, "\0");
		if (!value || !family || !key) {
			opbx_log(LOG_DEBUG, "Ignoring; Syntax error in argument\n");
			LOCAL_USER_REMOVE(u);
			return 0;
		}
		if (option_verbose > 2)
			opbx_verbose(VERBOSE_PREFIX_3 "DBput: family=%s, key=%s, value=%s\n", family, key, value);
		if (opbx_db_put(family, key, value)) {
			if (option_verbose > 2)
				opbx_verbose(VERBOSE_PREFIX_3 "DBput: Error writing value to database.\n");
		}

	} else	{
		opbx_log (LOG_DEBUG, "Ignoring, no parameters\n");
	}

	LOCAL_USER_REMOVE(u);

	return 0;
}

static int get_exec(struct opbx_channel *chan, void *data)
{
	char *argv, *varname, *family, *key;
	char dbresult[256];
	static int dep_warning = 0;
	struct localuser *u;

	LOCAL_USER_ADD(u);

	if (!dep_warning) {
		opbx_log(LOG_WARNING, "This application has been deprecated, please use the ${DB(family/key)} function instead.\n");
		dep_warning = 1;
	}
	
	argv = opbx_strdupa(data);
	if (!argv) {
		opbx_log(LOG_ERROR, "Memory allocation failed\n");
		LOCAL_USER_REMOVE(u);
		return 0;
	}

	if (strchr(argv, '=') && strchr(argv, '/')) {
		varname = strsep(&argv, "=");
		family = strsep(&argv, "/");
		key = strsep(&argv, "\0");
		if (!varname || !family || !key) {
			opbx_log(LOG_DEBUG, "Ignoring; Syntax error in argument\n");
			LOCAL_USER_REMOVE(u);
			return 0;
		}
		if (option_verbose > 2)
			opbx_verbose(VERBOSE_PREFIX_3 "DBget: varname=%s, family=%s, key=%s\n", varname, family, key);
		if (!opbx_db_get(family, key, dbresult, sizeof (dbresult) - 1)) {
			pbx_builtin_setvar_helper(chan, varname, dbresult);
			if (option_verbose > 2)
				opbx_verbose(VERBOSE_PREFIX_3 "DBget: set variable %s to %s\n", varname, dbresult);
		} else {
			if (option_verbose > 2)
			opbx_verbose(VERBOSE_PREFIX_3 "DBget: Value not found in database.\n");
			/* Send the call to n+101 priority, where n is the current priority */
			opbx_goto_if_exists(chan, chan->context, chan->exten, chan->priority + 101);
		}
	} else {
		opbx_log(LOG_DEBUG, "Ignoring, no parameters\n");
	}

	LOCAL_USER_REMOVE(u);

	return 0;
}

int unload_module(void)
{
	int retval;

	STANDARD_HANGUP_LOCALUSERS;
	retval = opbx_unregister_application(dt_app);
	retval |= opbx_unregister_application(d_app);
	retval |= opbx_unregister_application(p_app);
	retval |= opbx_unregister_application(g_app);

	return retval;
}

int load_module(void)
{
	int retval;

	retval = opbx_register_application(g_app, get_exec, g_synopsis, g_descrip);
	retval |= opbx_register_application(p_app, put_exec, p_synopsis, p_descrip);
	retval |= opbx_register_application(d_app, del_exec, d_synopsis, d_descrip);
	retval |= opbx_register_application(dt_app, deltree_exec, dt_synopsis, dt_descrip);
	
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


