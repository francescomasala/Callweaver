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
 * \brief Trivial application to playback a sound file
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
#include "openpbx/utils.h"

static char *tdesc = "Sound File Playback Application";

static char *app = "Playback";

static char *synopsis = "Play a file";

static char *descrip = 
"  Playback(filename[&filename2...][|option]):  Plays back given filenames (do not put\n"
"extension). Options may also be  included following a pipe symbol. The 'skip'\n"
"option causes the playback of the message to  be  skipped  if  the  channel\n"
"is not in the 'up' state (i.e. it hasn't been  answered  yet. If 'skip' is \n"
"specified, the application will return immediately should the channel not be\n"
"off hook.  Otherwise, unless 'noanswer' is specified, the channel channel will\n"
"be answered before the sound is played. Not all channels support playing\n"
"messages while still hook. Returns -1 if the channel was hung up.  If the\n"
"file does not exist, will jump to priority n+101 if present."
"The channel variable PLAYBACKSTATUS is set to SUCCESS or FAILED on termination."
"\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int playback_exec(struct opbx_channel *chan, void *data)
{
	int res = 0, mres=0;
	struct localuser *u;
	char *tmp = NULL;
	char *options = NULL;
	int option_skip=0;
	int option_noanswer = 0;
	char *stringp = NULL;
	char *front = NULL, *back = NULL;
	
	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "Playback requires an argument (filename)\n");
		return -1;
	}

	LOCAL_USER_ADD(u);

        pbx_builtin_setvar_helper(chan, "PLAYBACKSTATUS", "");

	tmp = opbx_strdupa(data);
	if (!tmp) {
		opbx_log(LOG_ERROR, "Out of memory!\n");
		LOCAL_USER_REMOVE(u);
		return -1;	
	}

	stringp = tmp;
	strsep(&stringp, "|");
	options = strsep(&stringp, "|");
	if (options && !strcasecmp(options, "skip"))
		option_skip = 1;
	if (options && !strcasecmp(options, "noanswer"))
		option_noanswer = 1;
	
	if (chan->_state != OPBX_STATE_UP) {
		if (option_skip) {
			/* At the user's option, skip if the line is not up */
                        pbx_builtin_setvar_helper(chan, "PLAYBACKSTATUS", "SUCCESS");
			LOCAL_USER_REMOVE(u);
			return 0;
		} else if (!option_noanswer)
			/* Otherwise answer unless we're supposed to send this while on-hook */
			res = opbx_answer(chan);
	}
	if (!res) {
		opbx_stopstream(chan);
		front = tmp;
		while (!res && front) {
			if ((back = strchr(front, '&'))) {
				*back = '\0';
				back++;
			}
			res = opbx_streamfile(chan, front, chan->language);
			if (!res) { 
				res = opbx_waitstream(chan, "");	
				opbx_stopstream(chan);
			} else {
				opbx_log(LOG_WARNING, "opbx_streamfile failed on %s for %s\n", chan->name, (char *)data);
				opbx_goto_if_exists(chan, chan->context, chan->exten, chan->priority + 101);
				res = 0;
				mres=1;
			}
			front = back;
		}
	}
	if (mres)
		pbx_builtin_setvar_helper(chan, "PLAYBACKSTATUS", "FAILED");
	else
		pbx_builtin_setvar_helper(chan, "PLAYBACKSTATUS", "SUCCESS");
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
	return opbx_register_application(app, playback_exec, synopsis, descrip);
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


