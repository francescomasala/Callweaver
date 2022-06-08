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
 * \brief App to lookup the caller ID number, and see if it is blacklisted
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/lock.h"
#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/options.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/translate.h"
#include "openpbx/image.h"
#include "openpbx/phone_no_utils.h"
#include "openpbx/opbxdb.h"

static char *tdesc = "Look up Caller*ID name/number from blacklist database";

static char *app = "LookupBlacklist";

static char *synopsis = "Look up Caller*ID name/number from blacklist database";

static char *descrip =
  "  LookupBlacklist: Looks up the Caller*ID number on the active\n"
  "channel in the OpenPBX database (family 'blacklist').  If the\n"
  "number is found, and if there exists a priority n + 101,\n"
  "where 'n' is the priority of the current instance, then  the\n"
  "channel  will  be  setup  to continue at that priority level.\n"
  "Otherwise, it returns 0.  Does nothing if no Caller*ID was received on the\n"
  "channel.\n"
  "Example: database put blacklist <name/number> 1\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int
lookupblacklist_exec (struct opbx_channel *chan, void *data)
{
	char blacklist[1];
	struct localuser *u;
	int bl = 0;

	LOCAL_USER_ADD (u);
	if (chan->cid.cid_num)
	{
		if (!opbx_db_get ("blacklist", chan->cid.cid_num, blacklist, sizeof (blacklist)))
		{
			if (option_verbose > 2)
				opbx_log(LOG_NOTICE, "Blacklisted number %s found\n",chan->cid.cid_num);
			bl = 1;
		}
	}
	if (chan->cid.cid_name) {
		if (!opbx_db_get ("blacklist", chan->cid.cid_name, blacklist, sizeof (blacklist))) 
		{
			if (option_verbose > 2)
				opbx_log (LOG_NOTICE,"Blacklisted name \"%s\" found\n",chan->cid.cid_name);
			bl = 1;
		}
	}
	
	if (bl)
		opbx_goto_if_exists(chan, chan->context, chan->exten, chan->priority + 101);

	LOCAL_USER_REMOVE (u);
	return 0;
}

int unload_module (void)
{
	STANDARD_HANGUP_LOCALUSERS;
	return opbx_unregister_application (app);
}

int load_module (void)
{
	return opbx_register_application (app, lookupblacklist_exec, synopsis,descrip);
}

char *description (void)
{
	return tdesc;
}

int usecount (void)
{
	int res;
	STANDARD_USECOUNT (res);
	return res;
}
