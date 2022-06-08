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
 * \brief Playback the special information tone to get rid of telemarketers
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
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/translate.h"

static char *tdesc = "Block Telemarketers with Special Information Tone";

static char *app = "Zapateller";

static char *synopsis = "Block telemarketers with SIT";

static char *descrip = 
"  Zapateller(options):  Generates special information tone to block\n"
"telemarketers from calling you.  Returns 0 normally or -1 on hangup.\n"
"Options is a pipe-delimited list of options.  The following options\n"
"are available: 'answer' causes the line to be answered before playing\n"
"the tone, 'nocallerid' causes Zapateller to only play the tone if there\n"
"is no callerid information available.  Options should be separated by |\n"
"characters\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int zapateller_exec(struct opbx_channel *chan, void *data)
{
	int res = 0;
	struct localuser *u;
	int answer = 0, nocallerid = 0;
	char *c;
	char *stringp=NULL;
	
	LOCAL_USER_ADD(u);

	stringp=data;
        c = strsep(&stringp, "|");
        while(!opbx_strlen_zero(c)) {
		if (!strcasecmp(c, "answer"))
			answer = 1;
		else if (!strcasecmp(c, "nocallerid"))
			nocallerid = 1;

                c = strsep(&stringp, "|");
        }

	opbx_stopstream(chan);
	if (chan->_state != OPBX_STATE_UP) {

		if (answer) 
			res = opbx_answer(chan);
		if (!res) {
			res = opbx_safe_sleep(chan, 500);
		}
	}
	if (chan->cid.cid_num && nocallerid) {
		LOCAL_USER_REMOVE(u);
		return res;
	} 
	if (!res) 
		res = opbx_tonepair(chan, 950, 0, 330, 0);
	if (!res) 
		res = opbx_tonepair(chan, 1400, 0, 330, 0);
	if (!res) 
		res = opbx_tonepair(chan, 1800, 0, 330, 0);
	if (!res) 
		res = opbx_tonepair(chan, 0, 0, 1000, 0);
	LOCAL_USER_REMOVE(u);
	return res;
}

int unload_module(void)
{
	STANDARD_HANGUP_LOCALUSERS;
	return opbx_unregister_application(app);
}

int load_module(void)
{
	return opbx_register_application(app, zapateller_exec, synopsis, descrip);
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


