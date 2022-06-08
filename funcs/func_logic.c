/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 * Portions Copyright (C) 2005, Anthony Minessale II
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
 * \brief Conditional logic dialplan functions
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/module.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/logger.h"
#include "openpbx/utils.h"
#include "openpbx/app.h"
#include "openpbx/config.h"		/* for opbx_true */

static char *builtin_function_isnull(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	return data && *data ? "0" : "1";
}

static char *builtin_function_exists(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	return data && *data ? "1" : "0";
}

static char *builtin_function_iftime(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	struct opbx_timing timing;
	char *ret;
	char *expr;
	char *iftrue;
	char *iffalse;

	if (!(data = opbx_strdupa(data))) {
		opbx_log(LOG_WARNING, "Memory Error!\n");
		return NULL;
	}

	data = opbx_strip_quoted(data, "\"", "\"");
	expr = strsep(&data, "?");
	iftrue = strsep(&data, ":");
	iffalse = data;

	if (!expr || opbx_strlen_zero(expr) || !(iftrue || iffalse)) {
		opbx_log(LOG_WARNING, "Syntax IFTIME(<timespec>?[<true>][:<false>])\n");
		return NULL;
	}

	if (!opbx_build_timing(&timing, expr)) {
		opbx_log(LOG_WARNING, "Invalid Time Spec.\n");
		return NULL;
	}

	if (iftrue)
		iftrue = opbx_strip_quoted(iftrue, "\"", "\"");
	if (iffalse)
		iffalse = opbx_strip_quoted(iffalse, "\"", "\"");

	if ((ret = opbx_check_timing(&timing) ? iftrue : iffalse)) {
		opbx_copy_string(buf, ret, len);
		ret = buf;
	} 
	
	return ret;
}

static char *builtin_function_if(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	char *ret;
	char *expr;
	char *iftrue;
	char *iffalse;

	if (!(data = opbx_strdupa(data))) {
		opbx_log(LOG_WARNING, "Memory Error!\n");
		return NULL;
	}

	data = opbx_strip_quoted(data, "\"", "\"");
	expr = strsep(&data, "?");
	iftrue = strsep(&data, ":");
	iffalse = data;

	if (!expr || opbx_strlen_zero(expr) || !(iftrue || iffalse)) {
		opbx_log(LOG_WARNING, "Syntax IF(<expr>?[<true>][:<false>])\n");
		return NULL;
	}

	expr = opbx_strip(expr);
	if (iftrue)
		iftrue = opbx_strip_quoted(iftrue, "\"", "\"");
	if (iffalse)
		iffalse = opbx_strip_quoted(iffalse, "\"", "\"");

	if ((ret = opbx_true(expr) ? iftrue : iffalse)) {
		opbx_copy_string(buf, ret, len);
		ret = buf;
	} 
	
	return ret;
}

static char *builtin_function_set(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	char *varname;
	char *val;

	if (!(data = opbx_strdupa(data))) {
		opbx_log(LOG_WARNING, "Memory Error!\n");
		return NULL;
	}

	varname = strsep(&data, "=");
	val = data;

	if (!varname || opbx_strlen_zero(varname) || !val) {
		opbx_log(LOG_WARNING, "Syntax SET(<varname>=[<value>])\n");
		return NULL;
	}

	varname = opbx_strip(varname);
	val = opbx_strip(val);
	pbx_builtin_setvar_helper(chan, varname, val);
	opbx_copy_string(buf, val, len);

	return buf;
}

static struct opbx_custom_function isnull_function = {
	.name = "ISNULL",
	.synopsis = "NULL Test: Returns 1 if NULL or 0 otherwise",
	.syntax = "ISNULL(<data>)",
	.read = builtin_function_isnull,
};

static struct opbx_custom_function set_function = {
	.name = "SET",
	.synopsis = "SET assigns a value to a channel variable",
	.syntax = "SET(<varname>=[<value>])",
	.read = builtin_function_set,
};

static struct opbx_custom_function exists_function = {
	.name = "EXISTS",
	.synopsis = "Existence Test: Returns 1 if exists, 0 otherwise",
	.syntax = "EXISTS(<data>)",
	.read = builtin_function_exists,
};

static struct opbx_custom_function if_function = {
	.name = "IF",
	.synopsis = "Conditional: Returns the data following '?' if true else the data following ':'",
	.syntax = "IF(<expr>?[<true>][:<false>])",
	.read = builtin_function_if,
};

static struct opbx_custom_function if_time_function = {
	.name = "IFTIME",
	.synopsis = "Temporal Conditional: Returns the data following '?' if true else the data following ':'",
	.syntax = "IFTIME(<timespec>?[<true>][:<false>])",
	.read = builtin_function_iftime,
};

static char *tdesc = "logic functions";

int unload_module(void)
{
        int res = 0;

        if (opbx_custom_function_unregister(&isnull_function) < 0)
                res = -1;

        if (opbx_custom_function_unregister(&set_function) < 0)
                res = -1;

        if (opbx_custom_function_unregister(&exists_function) < 0)
                res = -1;

        if (opbx_custom_function_unregister(&if_function) < 0)
                res = -1;

        if (opbx_custom_function_unregister(&if_time_function) < 0)
                res = -1;

        return res;
}

int load_module(void)
{
        int res = 0;

        if (opbx_custom_function_register(&isnull_function) < 0)
                res = -1;

        if (opbx_custom_function_register(&set_function) < 0)
                res = -1;

        if (opbx_custom_function_register(&exists_function) < 0)
                res = -1;

        if (opbx_custom_function_register(&if_function) < 0)
                res = -1;

        if (opbx_custom_function_register(&if_time_function) < 0)
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
