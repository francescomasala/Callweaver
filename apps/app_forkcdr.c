/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Anthony Minessale anthmct@yahoo.com
 * Development of this app Sponsered/Funded  by TAAN Softworks Corp
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
 * \brief Fork CDR application
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/cdr.h"
#include "openpbx/module.h"

static char *tdesc = "Fork The CDR into 2 separate entities.";
static char *app = "ForkCDR";
static char *synopsis = 
"Forks the Call Data Record";
static char *descrip = 
"  ForkCDR([options]):  Causes the Call Data Record to fork an additional\n"
	"cdr record starting from the time of the fork call\n"
"If the option 'v' is passed all cdr variables will be passed along also.\n"
"";


STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static void opbx_cdr_fork(struct opbx_channel *chan) 
{
	struct opbx_cdr *cdr;
	struct opbx_cdr *newcdr;
	if (!chan || !(cdr = chan->cdr))
		return;
	while (cdr->next)
		cdr = cdr->next;
	if (!(newcdr = opbx_cdr_dup(cdr)))
		return;
	opbx_cdr_append(cdr, newcdr);
	opbx_cdr_reset(newcdr, OPBX_CDR_FLAG_KEEP_VARS);
	if (!opbx_test_flag(cdr, OPBX_CDR_FLAG_KEEP_VARS))
		opbx_cdr_free_vars(cdr, 0);
	opbx_set_flag(cdr, OPBX_CDR_FLAG_CHILD | OPBX_CDR_FLAG_LOCKED);
}

static int forkcdr_exec(struct opbx_channel *chan, void *data)
{
	int res=0;
	struct localuser *u;
	LOCAL_USER_ADD(u);
	if (!opbx_strlen_zero(data))
		opbx_set2_flag(chan->cdr, strchr((char *)data, 'v'), OPBX_CDR_FLAG_KEEP_VARS);
	
	opbx_cdr_fork(chan);

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
	return opbx_register_application(app, forkcdr_exec, synopsis, descrip);
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


