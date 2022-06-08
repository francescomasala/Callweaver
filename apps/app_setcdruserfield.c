/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Justin Huff <jjhuff@mspin.net>
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
 * \brief Applictions connected with CDR engine
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/channel.h"
#include "openpbx/cdr.h"
#include "openpbx/module.h"
#include "openpbx/pbx.h"
#include "openpbx/logger.h"
#include "openpbx/config.h"
#include "openpbx/manager.h"
#include "openpbx/utils.h"


static char *tdesc = "CDR user field apps";

static char *setcdruserfield_descrip = 
               "[Synopsis]\n"
               "SetCDRUserField(value)\n\n"
               "[Description]\n"
               "SetCDRUserField(value): Set the CDR 'user field' to value\n"
               "       The Call Data Record (CDR) user field is an extra field you\n"
               "       can use for data not stored anywhere else in the record.\n"
               "       CDR records can be used for billing or storing other arbitrary data\n"
               "       (I.E. telephone survey responses)\n"
               "       Also see AppendCDRUserField().\n"
               "       Always returns 0\n";

		
static char *setcdruserfield_app = "SetCDRUserField";
static char *setcdruserfield_synopsis = "Set the CDR user field";

static char *appendcdruserfield_descrip = 
               "[Synopsis]\n"
               "AppendCDRUserField(value)\n\n"
               "[Description]\n"
               "AppendCDRUserField(value): Append value to the CDR user field\n"
               "       The Call Data Record (CDR) user field is an extra field you\n"
               "       can use for data not stored anywhere else in the record.\n"
               "       CDR records can be used for billing or storing other arbitrary data\n"
               "       (I.E. telephone survey responses)\n"
               "       Also see SetCDRUserField().\n"
               "       Always returns 0\n";
		
static char *appendcdruserfield_app = "AppendCDRUserField";
static char *appendcdruserfield_synopsis = "Append to the CDR user field";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int action_setcdruserfield(struct mansession *s, struct message *m)
{
	struct opbx_channel *c = NULL;
	char *userfield = astman_get_header(m, "UserField");
	char *channel = astman_get_header(m, "Channel");
	char *append = astman_get_header(m, "Append");

	if (opbx_strlen_zero(channel)) {
		astman_send_error(s, m, "No Channel specified");
		return 0;
	}
	if (opbx_strlen_zero(userfield)) {
		astman_send_error(s, m, "No UserField specified");
		return 0;
	}
	c = opbx_get_channel_by_name_locked(channel);
	if (!c) {
		astman_send_error(s, m, "No such channel");
		return 0;
	}
	if (opbx_true(append))
		opbx_cdr_appenduserfield(c, userfield);
	else
		opbx_cdr_setuserfield(c, userfield);
	opbx_mutex_unlock(&c->lock);
	astman_send_ack(s, m, "CDR Userfield Set");
	return 0;
}

static int setcdruserfield_exec(struct opbx_channel *chan, void *data)
{
	struct localuser *u;
	int res = 0;
	
	LOCAL_USER_ADD(u);

	if (chan->cdr && data) {
		opbx_cdr_setuserfield(chan, (char*)data);
	}

	LOCAL_USER_REMOVE(u);
	
	return res;
}

static int appendcdruserfield_exec(struct opbx_channel *chan, void *data)
{
	struct localuser *u;
	int res = 0;
	
	LOCAL_USER_ADD(u);

	if (chan->cdr && data) {
		opbx_cdr_appenduserfield(chan, (char*)data);
	}

	LOCAL_USER_REMOVE(u);
	
	return res;
}

int unload_module(void)
{
	int res;
	STANDARD_HANGUP_LOCALUSERS;
	res = opbx_unregister_application(setcdruserfield_app);
	res |= opbx_unregister_application(appendcdruserfield_app);
	opbx_manager_unregister("SetCDRUserField");
	return res;
}

int load_module(void)
{
	int res;
	res = opbx_register_application(setcdruserfield_app, setcdruserfield_exec, setcdruserfield_synopsis, setcdruserfield_descrip);
	res |= opbx_register_application(appendcdruserfield_app, appendcdruserfield_exec, appendcdruserfield_synopsis, appendcdruserfield_descrip);
	opbx_manager_register("SetCDRUserField", EVENT_FLAG_CALL, action_setcdruserfield, "Set the CDR UserField");
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

