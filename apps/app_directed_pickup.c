/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 2005, Joshua Colp
 *
 * Joshua Colp <jcolp@asterlink.com>
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
 * \brief Directed Call Pickup Support
 *
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/lock.h"
#include "openpbx/app.h"

static const char *tdesc = "Directed Call Pickup Application";
static const char *app = "Pickup";
static const char *synopsis = "Directed Call Pickup application.";
static const char *descrip =
" Pickup(extension@context):\n"
"Steals any calls to a specified extension that are in a ringing state and bridges them to the current channel. Context is an optional argument.\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int pickup_exec(struct opbx_channel *chan, void *data)
{
	int res = 0, locked = 0;
	struct localuser *u = NULL;
	struct opbx_channel *origin = NULL, *target = NULL;
	char *tmp = NULL, *exten = NULL, *context = NULL;
	char workspace[256] = "";

	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "Pickup requires an argument (extension) !\n");
		return -1;	
	}

	LOCAL_USER_ADD(u);
	
	/* Get the extension and context if present */
	exten = data;
	context = strchr(data, '@');
	if (context) {
		*context = '\0';
		context++;
	}

	/* Find a channel to pickup */
	origin = opbx_get_channel_by_exten_locked(exten, context);
	if (origin) {
		opbx_cdr_getvar(origin->cdr, "dstchannel", &tmp, workspace,
			       sizeof(workspace), 0);
		if (tmp) {
			/* We have a possible channel... now we need to find it! */
			target = opbx_get_channel_by_name_locked(tmp);
			if (target)
			  locked = 1;
		} else {
			opbx_log(LOG_DEBUG, "No target channel found.\n");
			res = -1;
		}
		opbx_mutex_unlock(&origin->lock);
	} else {
		opbx_log(LOG_DEBUG, "No originating channel found.\n");
	}
	
	if (res)
		goto out;

	if (target && (!target->pbx) && ((target->_state == OPBX_STATE_RINGING) || (target->_state == OPBX_STATE_RING))) {
		opbx_log(LOG_DEBUG, "Call pickup on chan '%s' by '%s'\n", target->name,
			chan->name);
		res = opbx_answer(chan);
		if (res) {
			opbx_log(LOG_WARNING, "Unable to answer '%s'\n", chan->name);
			res = -1;
			goto out;
		}
		res = opbx_queue_control(chan, OPBX_CONTROL_ANSWER);
		if (res) {
			opbx_log(LOG_WARNING, "Unable to queue answer on '%s'\n",
				chan->name);
			res = -1;
			goto out;
		}
		res = opbx_channel_masquerade(target, chan);
		if (res) {
			opbx_log(LOG_WARNING, "Unable to masquerade '%s' into '%s'\n", chan->name, target->name);
			res = -1;
			goto out;
		}
	} else {
		opbx_log(LOG_DEBUG, "No call pickup possible...\n");
		res = -1;
	}
	
 out:
	if (target)
		opbx_mutex_unlock(&target->lock);

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
	return opbx_register_application(app, pickup_exec, synopsis, descrip);
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


