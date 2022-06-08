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
 * \brief App to transmit a text message
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
#include "openpbx/image.h"
#include "openpbx/options.h"

static const char *tdesc = "Send Text Applications";

static const char *app = "SendText";

static const char *synopsis = "Send a Text Message";

static const char *descrip = 
"  SendText(text): Sends text to current channel (callee).\n"
"Otherwise, execution will continue at the next priority level.\n"
"Result of transmission will be stored in the SENDTEXTSTATUS\n"
"channel variable:\n"
"      SUCCESS      Transmission succeeded\n"
"      FAILURE      Transmission failed\n"
"      UNSUPPORTED  Text transmission not supported by channel\n"
"\n"
"At this moment, text is supposed to be 7 bit ASCII in most channels.\n"
"Old deprecated behavior: \n"
" SendText only returns 0 if the text was sent correctly or if\n"
" the channel does not support text transport.\n"
" If the client does not support text transport, and there exists a\n"
" step with priority n + 101, then execution will continue at that step.\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int sendtext_exec(struct opbx_channel *chan, void *data)
{
	int res = 0;
	struct localuser *u;
	char *status = "UNSUPPORTED";
		
	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "SendText requires an argument (text)\n");
		return -1;
	}
	
	LOCAL_USER_ADD(u);

	opbx_mutex_lock(&chan->lock);
	if (!chan->tech->send_text) {
		opbx_mutex_unlock(&chan->lock);
		/* Does not support transport */
		if (option_priority_jumping)
			opbx_goto_if_exists(chan, chan->context, chan->exten, chan->priority + 101);
		LOCAL_USER_REMOVE(u);
		return 0;
	}
	status = "FAILURE";
	opbx_mutex_unlock(&chan->lock);
	res = opbx_sendtext(chan, (char *)data);
	if (!res)
		status = "SUCCESS";
	pbx_builtin_setvar_helper(chan, "SENDTEXTSTATUS", status);
	LOCAL_USER_REMOVE(u);
	return 0;
}

int unload_module(void)
{
	STANDARD_HANGUP_LOCALUSERS;

	return opbx_unregister_application(app);
}

int load_module(void)
{
	return opbx_register_application(app, sendtext_exec, synopsis, descrip);
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


