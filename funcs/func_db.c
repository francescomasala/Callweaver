/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 2005, Russell Bryant <russelb@clemson.edu> 
 *
 * func_db.c adapted from the old app_db.c, copyright by the following people 
 * Copyright (C) 2005, Mark Spencer <markster@digium.com>
 * Copyright (C) 2003, Jefferson Noxon <jeff@debian.org>
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
 * \brief Functions for interaction with the OpenPBX database
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <regex.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/logger.h"
#include "openpbx/options.h"
#include "openpbx/utils.h"
#include "openpbx/app.h"
#include "openpbx/opbxdb.h"

static char *function_db_read(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len)
{
	int argc;	
	char *args;
	char *argv[2];
	char *family;
	char *key;

	if (!data || opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "DB requires an argument, DB(<family>/<key>)\n");
		return buf;
	}

	args = opbx_strdupa(data);
	argc = opbx_separate_app_args(args, '/', argv, sizeof(argv) / sizeof(argv[0]));
	
	if (argc > 1) {
		family = argv[0];
		key = argv[1];
	} else {
		opbx_log(LOG_WARNING, "DB requires an argument, DB(<family>/<key>)\n");
		return buf;
	}

	if (opbx_db_get(family, key, buf, len-1)) {
		opbx_log(LOG_DEBUG, "DB: %s/%s not found in database.\n", family, key);
	} else
		pbx_builtin_setvar_helper(chan, "DB_RESULT", buf);

	
	return buf;
}

static void function_db_write(struct opbx_channel *chan, char *cmd, char *data, const char *value) 
{
	int argc;	
	char *args;
	char *argv[2];
	char *family;
	char *key;

	if (!data || opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "DB requires an argument, DB(<family>/<key>)=<value>\n");
		return;
	}

	args = opbx_strdupa(data);
	argc = opbx_separate_app_args(args, '/', argv, sizeof(argv) / sizeof(argv[0]));
	
	if (argc > 1) {
		family = argv[0];
		key = argv[1];
	} else {
		opbx_log(LOG_WARNING, "DB requires an argument, DB(<family>/<key>)=value\n");
		return;
	}

	if (opbx_db_put(family, key, (char*)value)) {
		opbx_log(LOG_WARNING, "DB: Error writing value to database.\n");
	}
}

static struct opbx_custom_function db_function = {
	.name = "DB",
	.synopsis = "Read or Write from/to the OpenPBX database",
	.syntax = "DB(<family>/<key>)",
	.desc = "This function will read or write a value from/to the OpenPBX database.\n"
		"DB(...) will read a value from the database, while DB(...)=value\n"
		"will write a value to the database.  On a read, this function\n"
		"returns the value from the database, or NULL if it does not exist.\n"
		"On a write, this function will always return NULL.  Reading a database value\n"
		"will also set the variable DB_RESULT.\n",
	.read = function_db_read,
	.write = function_db_write,
};

static char *function_db_exists(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len)
{
	int argc;	
	char *args;
	char *argv[2];
	char *family;
	char *key;

	if (!data || opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "DB_EXISTS requires an argument, DB(<family>/<key>)\n");
		return buf;
	}

	args = opbx_strdupa(data);
	argc = opbx_separate_app_args(args, '/', argv, sizeof(argv) / sizeof(argv[0]));
	
	if (argc > 1) {
		family = argv[0];
		key = argv[1];
	} else {
		opbx_log(LOG_WARNING, "DB_EXISTS requires an argument, DB(<family>/<key>)\n");
		return buf;
	}

	if (opbx_db_get(family, key, buf, len-1))
		opbx_copy_string(buf, "0", len);	
	else {
		pbx_builtin_setvar_helper(chan, "DB_RESULT", buf);
		opbx_copy_string(buf, "1", len);
	}
	
	return buf;
}

static struct opbx_custom_function db_exists_function = {
	.name = "DB_EXISTS",
	.synopsis = "Check to see if a key exists in the OpenPBX database",
	.syntax = "DB_EXISTS(<family>/<key>)",
	.desc = "This function will check to see if a key exists in the OpenPBX\n"
		"database. If it exists, the function will return \"1\". If not,\n"
		"it will return \"0\".  Checking for existence of a database key will\n"
		"also set the variable DB_RESULT to the key's value if it exists.\n",
	.read = function_db_exists,
};

static char *tdesc = "database functions";

int unload_module(void)
{
        int res = 0;

        if (opbx_custom_function_unregister(&db_exists_function) < 0)
                res = -1;

        if (opbx_custom_function_unregister(&db_function) < 0)
                res = -1;

        return res;
}

int load_module(void)
{
        int res = 0;

        if (opbx_custom_function_register(&db_exists_function) < 0)
                res = -1;

        if (opbx_custom_function_register(&db_function) < 0)
                res = -1;

        return res;
}

char *description(void)
{
	return tdesc;
}

int usecount(void)
{
	return 0;
}

/*
Local Variables:
mode: C
c-file-style: "linux"
indent-tabs-mode: nil
End:
*/
