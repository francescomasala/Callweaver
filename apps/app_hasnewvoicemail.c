/*
 * CallWeaver -- An open source telephony toolkit.
 *
 * Changes Copyright (c) 2004 - 2005 Todd Freeman <freeman@andrews.edu>
 * 
 * 95% based on HasNewVoicemail by:
 * 
 * Copyright (c) 2003 Tilghman Lesher.  All rights reserved.
 * 
 * Tilghman Lesher <asterisk-hasnewvoicemail-app@the-tilghman.com>
 *
 * See http://www.callweaver.org for more information about
 * the CallWeaver project. Please do not directly contact
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
 * \brief HasVoicemail application
 *
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/apps/app_hasnewvoicemail.c $", "$Revision: 4723 $")

#include "callweaver/file.h"
#include "callweaver/logger.h"
#include "callweaver/channel.h"
#include "callweaver/pbx.h"
#include "callweaver/module.h"
#include "callweaver/lock.h"
#include "callweaver/utils.h"


static void *vmcount_function;
static const char *vmcount_func_name = "VMCOUNT";
static const char *vmcount_func_synopsis = "Counts the voicemail in a specified mailbox";
static const char *vmcount_func_syntax = "VMCOUNT(vmbox[@context][, folder])";
static const char *vmcount_func_desc =
	"  context - defaults to \"default\"\n"
	"  folder  - defaults to \"INBOX\"\n";

static char *tdesc = "Indicator for whether a voice mailbox has messages in a given folder.";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int hasvoicemail_internal(char *context, char *box, char *folder)
{
	char vmpath[256];
	DIR *vmdir;
	struct dirent *vment;
	int count=0;

	snprintf(vmpath,sizeof(vmpath), "%s/voicemail/%s/%s/%s", (char *)cw_config_CW_SPOOL_DIR, context, box, folder);
	if ((vmdir = opendir(vmpath))) {
		/* No matter what the format of VM, there will always be a .txt file for each message. */
		while ((vment = readdir(vmdir))) {
			if (!strncmp(vment->d_name + 7, ".txt", 4)) {
				count++;
				break;
			}
		}
		closedir(vmdir);
	}
	return count;
}

static char *acf_vmcount_exec(struct cw_channel *chan, int argc, char **argv, char *buf, size_t len)
{
	struct localuser *u;
	char *context;

	if (argc < 1 || argc > 2 || !argv[0][0]) {
		cw_log(LOG_ERROR, "Syntax: %s\n", vmcount_func_syntax);
		return NULL;
	}

	LOCAL_USER_ACF_ADD(u);

	buf[0] = '\0';

	if ((context = strchr(argv[0], '@')))
		*(context++) = '\0';
	else
		context = "default";

	snprintf(buf, len, "%d", hasvoicemail_internal(context, argv[0], (argc > 1 && argv[1][0] ? argv[1] : "INBOX")));

	LOCAL_USER_REMOVE(u);
	return buf;
}


int unload_module(void)
{
	int res = 0;
	STANDARD_HANGUP_LOCALUSERS;
	res |= cw_unregister_function(vmcount_function);
	return res;
}

int load_module(void)
{
	vmcount_function = cw_register_function(vmcount_func_name, acf_vmcount_exec, NULL, vmcount_func_synopsis, vmcount_func_syntax, vmcount_func_desc);
	return 0;
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


