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
 * \brief Channel timeout related dialplan functions
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

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/module.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/logger.h"
#include "openpbx/utils.h"
#include "openpbx/app.h"
#include "openpbx/options.h"

static char *builtin_function_timeout_read(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	time_t myt;

	if (!data) {
		opbx_log(LOG_ERROR, "Must specify type of timeout to get.\n");
                return NULL;
	}
	
	switch(*data) {
	case 'a':
	case 'A':
		if (chan->whentohangup == 0) {
			opbx_copy_string(buf, "0", len);
		} else {
			time(&myt);
			snprintf(buf, len, "%d", (int) (chan->whentohangup - myt));
		}
		break;

	case 'r':
	case 'R':
		if (chan->pbx) {
			snprintf(buf, len, "%d", chan->pbx->rtimeout);
		}
		break;

	case 'd':
	case 'D':
		if (chan->pbx) {
			snprintf(buf, len, "%d", chan->pbx->dtimeout);
		}
		break;

	default:
		opbx_log(LOG_ERROR, "Unknown timeout type specified.\n");
		break;
	}

	return buf;
}

static void builtin_function_timeout_write(struct opbx_channel *chan, char *cmd, char *data, const char *value) 
{
	int x;
	char timestr[64];
	struct tm myt;

        if (!chan) {
                opbx_log(LOG_ERROR, "No channel! Timeout only works on "
                         "channels\n");
                return;
        }

	if (!data) {
		opbx_log(LOG_ERROR, "Must specify type of timeout to set.\n");
		return;
	}        
	
	if (!value)
		return;

	x = atoi(value);

	switch(*data) {
	case 'a':
	case 'A':
		opbx_channel_setwhentohangup(chan, x);
		if (option_verbose > 2) {
			if (chan->whentohangup) {
				strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S UTC", gmtime_r(&chan->whentohangup, &myt));
				opbx_verbose( VERBOSE_PREFIX_3 "Channel will hangup at %s.\n", timestr);
			} else {
				opbx_verbose( VERBOSE_PREFIX_3 "Channel hangup cancelled.\n");
			} 
		}
		break;

	case 'r':
	case 'R':
		if (chan->pbx) {
			chan->pbx->rtimeout = x;
			if (option_verbose > 2)
				opbx_verbose( VERBOSE_PREFIX_3 "Response timeout set to %d\n", chan->pbx->rtimeout);
		}
		break;

	case 'd':
	case 'D':
		if (chan->pbx) {
			chan->pbx->dtimeout = x;
			if (option_verbose > 2)
				opbx_verbose( VERBOSE_PREFIX_3 "Digit timeout set to %d\n", chan->pbx->dtimeout);
		}
		break;

	default:
		opbx_log(LOG_ERROR, "Unknown timeout type specified.\n");
		break;
	}
}

static struct opbx_custom_function timeout_function = {
	.name = "TIMEOUT",
	.synopsis = "Gets or sets timeouts on the channel.",
	.syntax = "TIMEOUT(timeouttype)",
	.desc = "Gets or sets various channel timeouts. The timeouts that can be\n"
	"manipulated are:\n"
	"\n"
	"absolute: The absolute maximum amount of time permitted for a call.  A\n"
	"	   setting of 0 disables the timeout.\n"
	"\n"
	"digit:    The maximum amount of time permitted between digits when the\n"
	"          user is typing in an extension.  When this timeout expires,\n"
	"          after the user has started to type in an extension, the\n"
	"          extension will be considered complete, and will be\n"
	"          interpreted.  Note that if an extension typed in is valid,\n"
	"          it will not have to timeout to be tested, so typically at\n"
	"          the expiry of this timeout, the extension will be considered\n"
	"          invalid (and thus control would be passed to the 'i'\n"
	"          extension, or if it doesn't exist the call would be\n"
	"          terminated).  The default timeout is 5 seconds.\n"
	"\n"
	"response: The maximum amount of time permitted after falling through a\n"
	"	   series of priorities for a channel in which the user may\n"
	"	   begin typing an extension.  If the user does not type an\n"
	"	   extension in this amount of time, control will pass to the\n"
	"	   't' extension if it exists, and if not the call would be\n"
	"	   terminated.  The default timeout is 10 seconds.\n",
	.read = builtin_function_timeout_read,
	.write = builtin_function_timeout_write,
};

static char *tdesc = "string functions";

int unload_module(void)
{
        return opbx_custom_function_unregister(&timeout_function);
}

int load_module(void)
{
        return opbx_custom_function_register(&timeout_function);
}

char *description(void)
{
	return tdesc;
}

int usecount(void)
{
	return 0;
}

/*
Local Variables:
mode: C
c-file-style: "linux"
indent-tabs-mode: nil
End:
*/