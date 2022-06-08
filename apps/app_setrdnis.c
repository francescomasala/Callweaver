/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 * Oliver Daudey <traveler@xs4all.nl>
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
 * \brief App to set rdnis
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
#include "openpbx/phone_no_utils.h"
#include "openpbx/utils.h"

static char *tdesc = "Set RDNIS Number";

static char *app = "SetRDNIS";

static char *synopsis = "Set RDNIS Number";

static char *descrip = 
"  SetRDNIS(cnum): Set RDNIS Number on a call to a new\n"
"value.  Always returns 0\n"
"SetRDNIS has been deprecated in favor of the function\n"
"CALLERID(rdnis)\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int setrdnis_exec(struct opbx_channel *chan, void *data)
{
	struct localuser *u;
	char *opt, *n, *l;
	char *tmp = NULL;
	static int deprecation_warning = 0;

	LOCAL_USER_ADD(u);
	
	if (!deprecation_warning) {
		opbx_log(LOG_WARNING, "SetRDNIS is deprecated, please use Set(CALLERID(rdnis)=value) instead.\n");
		deprecation_warning = 1;
	}

	if (data)
		tmp = opbx_strdupa(data);
	else
		tmp = "";	

	opt = strchr(tmp, '|');
	if (opt)
		*opt = '\0';
	
	n = l = NULL;
	opbx_callerid_parse(tmp, &n, &l);
	if (l) {
		opbx_shrink_phone_number(l);
		opbx_mutex_lock(&chan->lock);
		if (chan->cid.cid_rdnis)
			free(chan->cid.cid_rdnis);
		chan->cid.cid_rdnis = (l[0]) ? strdup(l) : NULL;
		opbx_mutex_unlock(&chan->lock);
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
	return opbx_register_application(app, setrdnis_exec, synopsis, descrip);
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


