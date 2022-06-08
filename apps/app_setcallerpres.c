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

/*! \royk
 *
 * \brief App to set caller presentation
 * 
 * \ingroup applications
 */

#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif
 
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION(__FILE__, "$Revision: 7221 $")

#include "openpbx/lock.h"
#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/translate.h"
#include "openpbx/image.h"
#include "openpbx/callerid.h"

static char *tdesc = "SetCallerPres Application";

static char *app = "SetCallerPres";

static char *synopsis = "Set CallerID Presentation";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static char *descrip = 
"  SetCallerPres(presentation): Set Caller*ID presentation on a call.\n"
"  Valid presentations are:\n"
"\n"
"      allowed_not_screened    : Presentation Allowed, Not Screened\n"
"      allowed_passed_screen   : Presentation Allowed, Passed Screen\n" 
"      allowed_failed_screen   : Presentation Allowed, Failed Screen\n" 
"      allowed                 : Presentation Allowed, Network Number\n"
"      prohib_not_screened     : Presentation Prohibited, Not Screened\n" 
"      prohib_passed_screen    : Presentation Prohibited, Passed Screen\n"
"      prohib_failed_screen    : Presentation Prohibited, Failed Screen\n"
"      prohib                  : Presentation Prohibited, Network Number\n"
"      unavailable             : Number Unavailable\n"
"\n"
;

static int setcallerid_pres_exec(struct opbx_channel *chan, void *data)
{
	struct localuser *u;
	int pres = -1;

	LOCAL_USER_ADD(u);
	
	pres = opbx_parse_caller_presentation(data);

	if (pres < 0) {
		opbx_log(LOG_WARNING, "'%s' is not a valid presentation (see 'show application SetCallerPres')\n",
			(char *) data);
		LOCAL_USER_REMOVE(u);
		return 0;
	}
	
	chan->cid.cid_pres = pres;
	LOCAL_USER_REMOVE(u);
	return 0;
}


int unload_module(void)
{
	int res;

	res = opbx_unregister_application(app);

	STANDARD_HANGUP_LOCALUSERS;

	return res;
}

int load_module(void)
{
	int res;
	
	res = opbx_register_application(app, setcallerid_pres_exec, synopsis, descrip);

	return res;
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

#if 0
char *key()
{
	return OPBX_GPL_KEY;
}
#endif
