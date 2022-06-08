/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
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
 * \brief  Call Detail Record related dialplan functions
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

#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/logger.h"
#include "openpbx/utils.h"
#include "openpbx/app.h"
#include "openpbx/cdr.h"

static char *builtin_function_cdr_read(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	char *ret;
	char *mydata;
	int argc;
	char *argv[2];
	int recursive = 0;

	if (!data || opbx_strlen_zero(data))
		return NULL;
	
	if (!chan->cdr)
		return NULL;

	mydata = opbx_strdupa(data);
	argc = opbx_separate_app_args(mydata, '|', argv, sizeof(argv) / sizeof(argv[0]));

	/* check for a trailing flags argument */
	if (argc > 1) {
		argc--;
		if (strchr(argv[argc], 'r'))
			recursive = 1;
	}

	opbx_cdr_getvar(chan->cdr, argv[0], &ret, buf, len, recursive);

	return ret;
}

static void builtin_function_cdr_write(struct opbx_channel *chan, char *cmd, char *data, const char *value) 
{
	char *mydata;
	int argc;
	char *argv[2];
	int recursive = 0;

	if (!data || opbx_strlen_zero(data) || !value)
		return;
	
	mydata = opbx_strdupa(data);
	argc = opbx_separate_app_args(mydata, '|', argv, sizeof(argv) / sizeof(argv[0]));

	/* check for a trailing flags argument */
	if (argc > 1) {
		argc--;
		if (strchr(argv[argc], 'r'))
			recursive = 1;
	}

	if (!strcasecmp(argv[0], "accountcode"))
		opbx_cdr_setaccount(chan, value);
	else if (!strcasecmp(argv[0], "userfield"))
		opbx_cdr_setuserfield(chan, value);
	else if (chan->cdr)
		opbx_cdr_setvar(chan->cdr, argv[0], value, recursive);
}

static struct opbx_custom_function cdr_function = {
	.name = "CDR",
	.synopsis = "Gets or sets a CDR variable",
	.desc= "Option 'r' searches the entire stack of CDRs on the channel\n",
	.syntax = "CDR(<name>[|options])",
	.read = builtin_function_cdr_read,
	.write = builtin_function_cdr_write,
};

static char *tdesc = "CDR related dialplan function";

int unload_module(void)
{
        return opbx_custom_function_unregister(&cdr_function);
}

int load_module(void)
{
        return opbx_custom_function_register(&cdr_function);
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
