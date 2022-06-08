/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 2004 - 2005, Anthony Minessale II.
 *
 * Anthony Minessale <anthmct@yahoo.com>
 *
 * disclaimed to Digium
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
 * \brief Application to dump channel variables
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
#include "openpbx/options.h"
#include "openpbx/utils.h"
#include "openpbx/lock.h"
#include "openpbx/utils.h"

static char *tdesc = "Dump Info About The Calling Channel";
static char *app = "DumpChan";
static char *synopsis = "Dump Info About The Calling Channel";
static char *desc = 
"   DumpChan([<min_verbose_level>])\n"
"Displays information on channel and listing of all channel\n"
"variables. If min_verbose_level is specified, output is only\n"
"displayed when the verbose level is currently set to that number\n"
"or greater. Always returns 0.\n\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int opbx_serialize_showchan(struct opbx_channel *c, char *buf, size_t size)
{
	struct timeval now;
	long elapsed_seconds=0;
	int hour=0, min=0, sec=0;
	char cgrp[256];
	char pgrp[256];
	
	now = opbx_tvnow();
	memset(buf,0,size);
	if (!c)
		return 0;

	if (c->cdr) {
		elapsed_seconds = now.tv_sec - c->cdr->start.tv_sec;
		hour = elapsed_seconds / 3600;
		min = (elapsed_seconds % 3600) / 60;
		sec = elapsed_seconds % 60;
	}

	snprintf(buf,size, 
			 "Name=               %s\n"
			 "Type=               %s\n"
			 "UniqueID=           %s\n"
			 "CallerID=           %s\n"
			 "CallerIDName=       %s\n"
			 "DNIDDigits=         %s\n"
			 "State=              %s (%d)\n"
			 "Rings=              %d\n"
			 "NativeFormat=       %d\n"
			 "WriteFormat=        %d\n"
			 "ReadFormat=         %d\n"
			 "1stFileDescriptor=  %d\n"
			 "Framesin=           %d %s\n"
			 "Framesout=          %d %s\n"
			 "TimetoHangup=       %ld\n"
			 "ElapsedTime=        %dh%dm%ds\n"
			 "Context=            %s\n"
			 "Extension=          %s\n"
			 "Priority=           %d\n"
			 "CallGroup=          %s\n"
			 "PickupGroup=        %s\n"
			 "Application=        %s\n"
			 "Data=               %s\n"
			 "Blocking_in=        %s\n",
			 c->name,
			 c->type,
			 c->uniqueid,
			 (c->cid.cid_num ? c->cid.cid_num : "(N/A)"),
			 (c->cid.cid_name ? c->cid.cid_name : "(N/A)"),
			 (c->cid.cid_dnid ? c->cid.cid_dnid : "(N/A)" ),
			 opbx_state2str(c->_state),
			 c->_state,
			 c->rings,
			 c->nativeformats,
			 c->writeformat,
			 c->readformat,
			 c->fds[0], c->fin & 0x7fffffff, (c->fin & 0x80000000) ? " (DEBUGGED)" : "",
			 c->fout & 0x7fffffff, (c->fout & 0x80000000) ? " (DEBUGGED)" : "", (long)c->whentohangup,
			 hour,
			 min,
			 sec,
			 c->context,
			 c->exten,
			 c->priority,
			 opbx_print_group(cgrp, sizeof(cgrp), c->callgroup),
			 opbx_print_group(pgrp, sizeof(pgrp), c->pickupgroup),
			 ( c->appl ? c->appl : "(N/A)" ),
			 ( c-> data ? (!opbx_strlen_zero(c->data) ? c->data : "(Empty)") : "(None)"),
			 (opbx_test_flag(c, OPBX_FLAG_BLOCKING) ? c->blockproc : "(Not Blocking)"));

	return 0;
}

static int dumpchan_exec(struct opbx_channel *chan, void *data)
{
	int res=0;
	struct localuser *u;
	char vars[1024];
	char info[1024];
	int level = 0;
	static char *line = "================================================================================";
	
	LOCAL_USER_ADD(u);

	if (!opbx_strlen_zero(data)) {
		level = atoi(data);
	}

	pbx_builtin_serialize_variables(chan, vars, sizeof(vars));
	opbx_serialize_showchan(chan, info, sizeof(info));
	if (option_verbose >= level)
		opbx_verbose("\nDumping Info For Channel: %s:\n%s\nInfo:\n%s\nVariables:\n%s%s\n",chan->name, line, info, vars, line);

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
	return opbx_register_application(app, dumpchan_exec, synopsis, desc);
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


