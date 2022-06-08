/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
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
 * \brief Environment related dialplan functions
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

static char *builtin_function_env_read(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	char *ret = "";

	if (data) {
		ret = getenv(data);
		if (!ret)
			ret = "";
	}
	opbx_copy_string(buf, ret, len);

	return buf;
}

static void builtin_function_env_write(struct opbx_channel *chan, char *cmd, char *data, const char *value) 
{
	if (data && !opbx_strlen_zero(data)) {
		if (value && !opbx_strlen_zero(value)) {
			setenv(data, value, 1);
		} else {
			unsetenv(data);
		}
	}
}

static struct opbx_custom_function env_function = {
	.name = "ENV",
	.synopsis = "Gets or sets the environment variable specified",
	.syntax = "ENV(<envname>)",
	.read = builtin_function_env_read,
	.write = builtin_function_env_write,
};

static char *tdesc = "Get or set environment variables.";

int unload_module(void)
{
       return opbx_custom_function_unregister(&env_function);
}

int load_module(void)
{
       return opbx_custom_function_register(&env_function);
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
