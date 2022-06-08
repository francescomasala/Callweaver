/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Martin Pycko <martinp@digium.com>
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
 * \brief Applications connected with CDR engine
 *
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <sys/types.h>
#include <stdlib.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/channel.h"
#include "openpbx/module.h"
#include "openpbx/pbx.h"


static char *tdesc = "Make sure openpbx doesn't save CDR for a certain call";

static char *nocdr_descrip = "NoCDR(): makes sure there won't be any CDR written for a certain call";
static char *nocdr_app = "NoCDR";
static char *nocdr_synopsis = "Make sure openpbx doesn't save CDR for a certain call";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int nocdr_exec(struct opbx_channel *chan, void *data)
{
	struct localuser *u;
	
	LOCAL_USER_ADD(u);

	if (chan->cdr) {
		opbx_cdr_free(chan->cdr);
		chan->cdr = NULL;
	}

	LOCAL_USER_REMOVE(u);

	return 0;
}

int unload_module(void)
{
	STANDARD_HANGUP_LOCALUSERS;
	return opbx_unregister_application(nocdr_app);
}

int load_module(void)
{
	return opbx_register_application(nocdr_app, nocdr_exec, nocdr_synopsis, nocdr_descrip);
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


