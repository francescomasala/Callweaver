/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (c) 2003 - 2005 Tilghman Lesher.  All rights reserved.
 *
 * Tilghman Lesher <openpbx__app_random__200508@the-tilghman.com>
 *
 * This code is released by the author with no restrictions on usage or distribution.
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
 * \brief Random application
 *
 * \author Tilghman Lesher <openpbx__app_random__200508 at the-tilghman.com>
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

#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/options.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"

static char *tdesc = "Random goto";

static char *app_random = "Random";

static char *random_synopsis = "Conditionally branches, based upon a probability";

static char *random_descrip =
"Random([probability]:[[context|]extension|]priority)\n"
"  probability := INTEGER in the range 1 to 100\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static char random_state[256];

static int random_exec(struct opbx_channel *chan, void *data)
{
	int res=0;
	struct localuser *u;

	char *s;
	char *prob;
	int probint;
	
	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "Random requires an argument ([probability]:[[context|]extension|]priority)\n");
		return -1;
	}
	
	LOCAL_USER_ADD(u);

	s = opbx_strdupa(data);
	if (!s) {
		opbx_log(LOG_ERROR, "Out of memory!\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	prob = strsep(&s,":");
	if ((!prob) || (sscanf(prob, "%d", &probint) != 1))
		probint = 0;

	if ((random() % 100) + probint > 100) {
		res = opbx_parseable_goto(chan, s);
		if (option_verbose > 2)
			opbx_verbose( VERBOSE_PREFIX_3 "Random branches to (%s,%s,%d)\n",
				chan->context,chan->exten, chan->priority+1);
	}
	LOCAL_USER_REMOVE(u);
	return res;
}

int unload_module(void)
{
	STANDARD_HANGUP_LOCALUSERS;
	return opbx_unregister_application(app_random);
}

int load_module(void)
{
	initstate((getppid() * 65535 + getpid()) % RAND_MAX, random_state, 256);
	return opbx_register_application(app_random, random_exec, random_synopsis, random_descrip);
}

char *description(void)
{
	return tdesc;
}

int usecount(void)
{
	/* Don't allow unload, since rand(3) depends upon this module being here. */
	return 1;
//	int res;
//	STANDARD_USECOUNT(res);
//	return res;
}


