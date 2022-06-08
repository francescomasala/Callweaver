/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (c) 2004 - 2005 Tilghman Lesher.  All rights reserved.
 *
 * Tilghman Lesher <app_verbose_v001@the-tilghman.com>
 *
 * This code is released by the author with no restrictions on usage.
 *
 * See http://www.openpbx.org for more information about
 * the OpenPBX project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 */

/*! \file
 *
 * \brief Verbose logging application
 *
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/options.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"


static char *tdesc = "Send verbose output";

static char *app_verbose = "Verbose";

static char *verbose_synopsis = "Send arbitrary text to verbose output";

static char *verbose_descrip =
"Verbose([<level>|]<message>)\n"
"  level must be an integer value.  If not specified, defaults to 0."
"  Always returns 0.\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int verbose_exec(struct opbx_channel *chan, void *data)
{
	char *vtext;
	int vsize;
	struct localuser *u;

	LOCAL_USER_ADD(u);

	if (data) {
		vtext = opbx_strdupa((char *)data);
		if (vtext) {
			char *tmp = strsep(&vtext, "|,");
			if (vtext) {
				if (sscanf(tmp, "%d", &vsize) != 1) {
					vsize = 0;
					opbx_log(LOG_WARNING, "'%s' is not a verboser number\n", vtext);
				}
			} else {
				vtext = tmp;
				vsize = 0;
			}
			if (option_verbose >= vsize) {
				switch (vsize) {
				case 0:
					opbx_verbose("%s\n", vtext);
					break;
				case 1:
					opbx_verbose(VERBOSE_PREFIX_1 "%s\n", vtext);
					break;
				case 2:
					opbx_verbose(VERBOSE_PREFIX_2 "%s\n", vtext);
					break;
				case 3:
					opbx_verbose(VERBOSE_PREFIX_3 "%s\n", vtext);
					break;
				default:
					opbx_verbose(VERBOSE_PREFIX_4 "%s\n", vtext);
				}
			}
		} else {
			opbx_log(LOG_ERROR, "Out of memory\n");
		}
	}

	LOCAL_USER_REMOVE(u);

	return 0;
}

int unload_module(void)
{
	STANDARD_HANGUP_LOCALUSERS;
	return opbx_unregister_application(app_verbose);
}

int load_module(void)
{
	return opbx_register_application(app_verbose, verbose_exec, verbose_synopsis, verbose_descrip);
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


