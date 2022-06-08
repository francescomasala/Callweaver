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
 * \brief App to send DTMF digits
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
#include "openpbx/options.h"
#include "openpbx/utils.h"
#include "openpbx/app.h"

static char *tdesc = "Send DTMF digits Application";

static char *app = "SendDTMF";

static char *synopsis = "Sends arbitrary DTMF digits";

static char *descrip = 
"  SendDTMF(digits[|timeout_ms]): Sends DTMF digits on a channel. \n"
"  Accepted digits: 0-9, *#abcd\n"
" Returns 0 on success or -1 on a hangup.\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int senddtmf_exec(struct opbx_channel *chan, void *data)
{
	int res = 0;
	struct localuser *u;
	char *digits = NULL, *to = NULL;
	int timeout = 250;

	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "SendDTMF requires an argument (digits or *#aAbBcCdD)\n");
		return 0;
	}

	LOCAL_USER_ADD(u);

	digits = opbx_strdupa(data);
	if (!digits) {
		opbx_log(LOG_ERROR, "Out of Memory!\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	if ((to = strchr(digits,'|'))) {
		*to = '\0';
		to++;
		timeout = atoi(to);
	}
		
	if(timeout <= 0)
		timeout = 250;

	res = opbx_dtmf_stream(chan,NULL,digits,timeout);
		
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
	return opbx_register_application(app, senddtmf_exec, synopsis, descrip);
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


