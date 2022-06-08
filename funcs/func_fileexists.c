/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 2005, Reisig Consulting
 *
 * Boris Reisig <boris@boris.ca>
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

/*
 *
 * Check's if a file exists function.
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

OPENPBX_FILE_VERSION("$HeadURL: svn://svn.openpbx.org/openpbx/trunk/funcs/func_fileexists.c $", "$Revision: 594 $")

#ifndef BUILTIN_FUNC
#include "openpbx/module.h"
#endif /* BUILTIN_FUNC */
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/logger.h"
#include "openpbx/utils.h"
#include "openpbx/app.h"
#include "openpbx/cdr.h"

static char *builtin_function_fileexists(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	char *ret;
	char *args;
	int argc;
	char *argv[2];
	int FileStatus = 0;

	if (!data || opbx_strlen_zero(data))
		return NULL;

	/* Parse some options */	
	args = opbx_strdupa(data);
	argc = opbx_separate_app_args(args, '|', argv, sizeof(argv) / sizeof(argv[0]));

	/* Attempt to check if the file already exists. */
	FileStatus = access(argv[0],F_OK);

	/* If the file does exists, Proceed with the following */
	if ( FileStatus == 0 ) {

		/* The file does exist so let's set the system variable. */
		ret="EXISTS";

	} else {

		/* The file does not exist. */
		ret="NONEXISTENT";
	}

	/* Returns the File Status */
	return ret;
}

#ifndef BUILTIN_FUNC
static
#endif
struct opbx_custom_function fileexists_function = {
	.name = "FILEEXISTS",
	.synopsis = "Checks if a file exists",
	.desc= "Returns the file status. Results are 'EXISTS' if the file exists and 'NONEXISTENT' if the file does not exist.\n",
	.syntax = "FILEEXISTS(<filename>)",
	.read = builtin_function_fileexists,
};

#ifndef BUILTIN_FUNC
static char *tdesc = "file existence dialplan function";

int unload_module(void)
{
        return opbx_custom_function_unregister(&fileexists_function);
}

int load_module(void)
{
        return opbx_custom_function_register(&fileexists_function);
}

char *description(void)
{
	return tdesc;
}

int usecount(void)
{
	return 0;
}


#endif /* BUILTIN_FUNC */

/*
Local Variables:
mode: C
c-file-style: "linux"
indent-tabs-mode: nil
End:
*/
