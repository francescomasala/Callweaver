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
 * \brief Trivial application to control playback of a sound file
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
#include "openpbx/app.h"
#include "openpbx/module.h"
#include "openpbx/translate.h"
#include "openpbx/utils.h"

static const char *tdesc = "Control Playback Application";

static const char *app = "ControlPlayback";

static const char *synopsis = "Play a file with fast forward and rewind";

static const char *descrip = 
"ControlPlayback(filename[|skipms[|ffchar[|rewchar[|stopchar[|pausechar[|restartchar]]]]]]):\n"
"  Plays  back  a  given  filename (do not put extension). Options may also\n"
"  be included following a pipe symbol.  You can use * and # to rewind and\n"
"  fast forward the playback specified. If 'stopchar' is added the file will\n"
"  terminate playback when 'stopchar' is pressed. If 'restartchar' is added, the file\n"
"  will restart when 'restartchar' is pressed. Returns -1 if the channel\n"
"  was hung up. if the file does not exist jumps to n+101 if it present.\n\n"
"  Example:  exten => 1234,1,ControlPlayback(file|4000|*|#|1|0|5)\n\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int is_on_phonepad(char key)
{
	return key == 35 || key == 42 || (key >= 48 && key <= 57);
}

static int controlplayback_exec(struct opbx_channel *chan, void *data)
{
	int res = 0;
	int skipms = 0;
	struct localuser *u;
	char *tmp;
	int argc;
	char *argv[7];
	enum arg_ids {
		arg_file = 0,
		arg_skip = 1,
		arg_fwd = 2,
		arg_rev = 3,
		arg_stop = 4,
		arg_pause = 5,
		arg_restart = 6,
	};
	
	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "ControlPlayback requires an argument (filename)\n");
		return -1;
	}

	LOCAL_USER_ADD(u);
	
	tmp = opbx_strdupa(data);
	memset(argv, 0, sizeof(argv));

	argc = opbx_separate_app_args(tmp, '|', argv, sizeof(argv) / sizeof(argv[0]));

	if (argc < 1) {
		opbx_log(LOG_WARNING, "ControlPlayback requires an argument (filename)\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	skipms = argv[arg_skip] ? atoi(argv[arg_skip]) : 3000;
	if (!skipms)
		skipms = 3000;

	if (!argv[arg_fwd] || !is_on_phonepad(*argv[arg_fwd]))
		argv[arg_fwd] = "#";
	if (!argv[arg_rev] || !is_on_phonepad(*argv[arg_rev]))
		argv[arg_rev] = "*";
	if (argv[arg_stop] && !is_on_phonepad(*argv[arg_stop]))
		argv[arg_stop] = NULL;
	if (argv[arg_pause] && !is_on_phonepad(*argv[arg_pause]))
		argv[arg_pause] = NULL;
	if (argv[arg_restart] && !is_on_phonepad(*argv[arg_restart]))
		argv[arg_restart] = NULL;

	res = opbx_control_streamfile(chan, argv[arg_file], argv[arg_fwd], argv[arg_rev], argv[arg_stop], argv[arg_pause], argv[arg_restart], skipms);

	/* If we stopped on one of our stop keys, return 0  */
	if (argv[arg_stop] && strchr(argv[arg_stop], res)) 
		res = 0;

	if (res < 0) {
		if (opbx_goto_if_exists(chan, chan->context, chan->exten, chan->priority + 101))
			res = 0;
	}

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
	return opbx_register_application(app, controlplayback_exec, synopsis, descrip);
}

char *description(void)
{
	return (char *) tdesc;
}

int usecount(void)
{
	int res;

	STANDARD_USECOUNT(res);
	return res;
}

