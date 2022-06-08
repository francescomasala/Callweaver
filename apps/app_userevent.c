/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
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
 * \brief UserEvent application -- send manager event
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
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
#include "openpbx/manager.h"

static char *tdesc = "Custom User Event Application";

static char *app = "UserEvent";

static char *synopsis = "Send an arbitrary event to the manager interface";

static char *descrip = 
"  UserEvent(eventname[|body]): Sends an arbitrary event to the\n"
"manager interface, with an optional body representing additional\n"
"arguments.  The format of the event will be:\n"
"    Event: UserEvent<specified event name>\n"
"    Channel: <channel name>\n"
"    Uniqueid: <call uniqueid>\n"
"    [body]\n"
"If the body is not specified, only Event, Channel, and Uniqueid fields\n"
"will be present.  Returns 0.";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int userevent_exec(struct opbx_channel *chan, void *data)
{
	struct localuser *u;
	char *info;
	char eventname[512];
	char *eventbody;

	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "UserEvent requires an argument (eventname|optional event body)\n");
		return -1;
	}

	LOCAL_USER_ADD(u);

	info = opbx_strdupa(data);
	if (!info) {
		opbx_log(LOG_ERROR, "Out of memory\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	snprintf(eventname, sizeof(eventname), "UserEvent%s", info);
	eventbody = strchr(eventname, '|');
	if (eventbody) {
		*eventbody = '\0';
		eventbody++;
	}
	
	if(eventbody) {
            opbx_log(LOG_DEBUG, "Sending user event: %s, %s\n", eventname, eventbody);
            manager_event(EVENT_FLAG_USER, eventname, 
			"Channel: %s\r\nUniqueid: %s\r\n%s\r\n",
			chan->name, chan->uniqueid, eventbody);
	} else {
            opbx_log(LOG_DEBUG, "Sending user event: %s\n", eventname);
            manager_event(EVENT_FLAG_USER, eventname, 
			"Channel: %s\r\nUniqueid: %s\r\n", chan->name, chan->uniqueid);
	}

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
	return opbx_register_application(app, userevent_exec, synopsis, descrip);
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


