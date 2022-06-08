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
 * \brief App to set caller ID name from database, based on directory number
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

static char *tdesc = "Look up CallerID Name from local database";

static char *app = "LookupCIDName";

static char *synopsis = "Look up CallerID Name from local database";

static char *descrip =
  "  LookupCIDName: Looks up the Caller*ID number on the active\n"
  "channel in the OpenPBX database (family 'cidname') and sets the\n"
  "Caller*ID name.  Does nothing if no Caller*ID was received on the\n"
  "channel.  This is useful if you do not subscribe to Caller*ID\n"
  "name delivery, or if you want to change the names on some incoming\n"
  "calls.  Always returns 0.\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int
lookupcidname_exec (struct opbx_channel *chan, void *data)
{
  char dbname[64];
  struct localuser *u;

  LOCAL_USER_ADD (u);
  if (chan->cid.cid_num) {
	if (!opbx_db_get ("cidname", chan->cid.cid_num, dbname, sizeof (dbname))) {
		opbx_set_callerid (chan, NULL, dbname, NULL);
		  if (option_verbose > 2)
		    opbx_verbose (VERBOSE_PREFIX_3 "Changed Caller*ID name to %s\n",
				 dbname);
	}
  }
  LOCAL_USER_REMOVE (u);
  return 0;
}

int
unload_module (void)
{
  STANDARD_HANGUP_LOCALUSERS;
  return opbx_unregister_application (app);
}

int
load_module (void)
{
  return opbx_register_application (app, lookupcidname_exec, synopsis,
				   descrip);
}

char *
description (void)
{
  return tdesc;
}

int
usecount (void)
{
  int res;
  STANDARD_USECOUNT (res);
  return res;
}
