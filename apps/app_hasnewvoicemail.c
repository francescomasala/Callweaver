/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Changes Copyright (c) 2004 - 2005 Todd Freeman <freeman@andrews.edu>
 * 
 * 95% based on HasNewVoicemail by:
 * 
 * Copyright (c) 2003 Tilghman Lesher.  All rights reserved.
 * 
 * Tilghman Lesher <openpbx-hasnewvoicemail-app@the-tilghman.com>
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

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/lock.h"
#include "openpbx/utils.h"

static char *tdesc = "Indicator for whether a voice mailbox has messages in a given folder.";
static char *app_hasvoicemail = "HasVoicemail";
static char *hasvoicemail_synopsis = "Conditionally branches to priority + 101";
static char *hasvoicemail_descrip =
"HasVoicemail(vmbox[/folder][@context][|varname])\n"
"  Branches to priority + 101, if there is voicemail in folder indicated."
"  Optionally sets <varname> to the number of messages in that folder."
"  Assumes folder of INBOX if not specified.\n";

static char *app_hasnewvoicemail = "HasNewVoicemail";
static char *hasnewvoicemail_synopsis = "Conditionally branches to priority + 101";
static char *hasnewvoicemail_descrip =
"HasNewVoicemail(vmbox[/folder][@context][|varname])\n"
"  Branches to priority + 101, if there is voicemail in folder 'folder' or INBOX.\n"
"if folder is not specified. Optionally sets <varname> to the number of messages\n" 
"in that folder.\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int hasvoicemail_internal(char *context, char *box, char *folder)
{
	char vmpath[256];
	DIR *vmdir;
	struct dirent *vment;
	int count=0;

	snprintf(vmpath,sizeof(vmpath), "%s/voicemail/%s/%s/%s", (char *)opbx_config_OPBX_SPOOL_DIR, context, box, folder);
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

static int hasvoicemail_exec(struct opbx_channel *chan, void *data)
{
	struct localuser *u;
	char *temps, *input, *varname = NULL, *vmbox, *context = "default";
	char *vmfolder;
	int vmcount = 0;
	static int dep_warning = 0;

	if (!dep_warning) {
		opbx_log(LOG_WARNING, "The applications HasVoicemail and HasNewVoicemail have been deprecated.  Please use the VMCOUNT() function instead.\n");
		dep_warning = 1;
	}
	
	if (!data) {
		opbx_log(LOG_WARNING, "HasVoicemail requires an argument (vm-box[/folder][@context]|varname)\n");
		return -1;
	}

	LOCAL_USER_ADD(u);

	input = opbx_strdupa((char *)data);
	if (! input) {
		opbx_log(LOG_ERROR, "Out of memory error\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	temps = input;
	if ((temps = strsep(&input, "|"))) {
		if (!opbx_strlen_zero(input))
			varname = input;
		input = temps;
	}

	if ((vmbox = strsep(&input, "@")))
		if (!opbx_strlen_zero(input))
			context = input;
	if (!vmbox)
		vmbox = input;

	vmfolder = strchr(vmbox, '/');
	if (vmfolder) {
		*vmfolder = '\0';
		vmfolder++;
	} else {
		vmfolder = "INBOX";
	}

	vmcount = hasvoicemail_internal(context, vmbox, vmfolder);
	/* Set the count in the channel variable */
	if (varname) {
		char tmp[12];
		snprintf(tmp, sizeof(tmp), "%d", vmcount);
		pbx_builtin_setvar_helper(chan, varname, tmp);
	}

	if (vmcount > 0) {
		/* Branch to the next extension */
		if (!opbx_goto_if_exists(chan, chan->context, chan->exten, chan->priority + 101)) 
			opbx_log(LOG_WARNING, "VM box %s@%s has new voicemail, but extension %s, priority %d doesn't exist\n", vmbox, context, chan->exten, chan->priority + 101);
	}

	LOCAL_USER_REMOVE(u);
	return 0;
}

static char *acf_vmcount_exec(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len)
{
	struct localuser *u;
	char *args, *context, *box, *folder;

	LOCAL_USER_ACF_ADD(u);

	buf[0] = '\0';

	args = opbx_strdupa(data);
	if (!args) {
		opbx_log(LOG_ERROR, "Out of memory");
		LOCAL_USER_REMOVE(u);
		return buf;
	}

	box = strsep(&args, "|");
	if (strchr(box, '@')) {
		context = box;
		box = strsep(&context, "@");
	} else {
		context = "default";
	}

	if (args) {
		folder = args;
	} else {
		folder = "INBOX";
	}

	snprintf(buf, len, "%d", hasvoicemail_internal(context, box, folder));

	LOCAL_USER_REMOVE(u);
	
	return buf;
}

struct opbx_custom_function acf_vmcount = {
	.name = "VMCOUNT",
	.synopsis = "Counts the voicemail in a specified mailbox",
	.syntax = "VMCOUNT(vmbox[@context][|folder])",
	.desc =
"  context - defaults to \"default\"\n"
"  folder  - defaults to \"INBOX\"\n",
	.read = acf_vmcount_exec,
};

int unload_module(void)
{
	int res;
	STANDARD_HANGUP_LOCALUSERS;
	res = opbx_custom_function_unregister(&acf_vmcount);
	res |= opbx_unregister_application(app_hasvoicemail);
	res |= opbx_unregister_application(app_hasnewvoicemail);
	return res;
}

int load_module(void)
{
	int res;
	res = opbx_custom_function_register(&acf_vmcount);
	res |= opbx_register_application(app_hasvoicemail, hasvoicemail_exec, hasvoicemail_synopsis, hasvoicemail_descrip);
	res |= opbx_register_application(app_hasnewvoicemail, hasvoicemail_exec, hasnewvoicemail_synopsis, hasnewvoicemail_descrip);
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


