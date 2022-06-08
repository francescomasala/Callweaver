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
 * \brief Group Manipulation Applications
 *
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <regex.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/options.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/utils.h"
#include "openpbx/cli.h"
#include "openpbx/app.h"

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int group_count_exec(struct opbx_channel *chan, void *data)
{
	int res = 0;
	int count;
	struct localuser *u;
	char group[80] = "";
	char category[80] = "";
	char ret[80] = "";
	char *grp;
	static int deprecation_warning = 0;

	LOCAL_USER_ADD(u);

	if (!deprecation_warning) {
	        opbx_log(LOG_WARNING, "The GetGroupCount application has been deprecated, please use the GROUP_COUNT function.\n");
		deprecation_warning = 1;
	}

	opbx_app_group_split_group(data, group, sizeof(group), category, sizeof(category));

	if (opbx_strlen_zero(group)) {
		grp = pbx_builtin_getvar_helper(chan, category);
		strncpy(group, grp, sizeof(group) - 1);
	}

	count = opbx_app_group_get_count(group, category);
	snprintf(ret, sizeof(ret), "%d", count);
	pbx_builtin_setvar_helper(chan, "GROUPCOUNT", ret);

	LOCAL_USER_REMOVE(u);

	return res;
}

static int group_match_count_exec(struct opbx_channel *chan, void *data)
{
	int res = 0;
	int count;
	struct localuser *u;
	char group[80] = "";
	char category[80] = "";
	char ret[80] = "";
	static int deprecation_warning = 0;

	LOCAL_USER_ADD(u);

	if (!deprecation_warning) {
	        opbx_log(LOG_WARNING, "The GetGroupMatchCount application has been deprecated, please use the GROUP_MATCH_COUNT function.\n");
		deprecation_warning = 1;
	}

	opbx_app_group_split_group(data, group, sizeof(group), category, sizeof(category));

	if (!opbx_strlen_zero(group)) {
		count = opbx_app_group_match_get_count(group, category);
		snprintf(ret, sizeof(ret), "%d", count);
		pbx_builtin_setvar_helper(chan, "GROUPCOUNT", ret);
	}

	LOCAL_USER_REMOVE(u);

	return res;
}

static int group_set_exec(struct opbx_channel *chan, void *data)
{
	int res = 0;
	struct localuser *u;
	static int deprecation_warning = 0;

	LOCAL_USER_ADD(u);
	
	if (!deprecation_warning) {
	        opbx_log(LOG_WARNING, "The SetGroup application has been deprecated, please use the GROUP() function.\n");
		deprecation_warning = 1;
	}

	if (opbx_app_group_set_channel(chan, data))
		opbx_log(LOG_WARNING, "SetGroup requires an argument (group name)\n");

	LOCAL_USER_REMOVE(u);
	return res;
}

static int group_check_exec(struct opbx_channel *chan, void *data)
{
	int res = 0;
	int max, count;
	struct localuser *u;
	char limit[80]="";
	char category[80]="";
	static int deprecation_warning = 0;

	if (!deprecation_warning) {
	        opbx_log(LOG_WARNING, "The CheckGroup application has been deprecated, please use a combination of the GotoIf application and the GROUP_COUNT() function.\n");
		deprecation_warning = 1;
	}

	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "CheckGroup requires an argument(max[@category])\n");
		return res;
	}

	LOCAL_USER_ADD(u);

  	opbx_app_group_split_group(data, limit, sizeof(limit), category, sizeof(category));

 	if ((sscanf(limit, "%d", &max) == 1) && (max > -1)) {
		count = opbx_app_group_get_count(pbx_builtin_getvar_helper(chan, category), category);
		if (count > max) {
			if (!opbx_goto_if_exists(chan, chan->context, chan->exten, chan->priority + 101))
				res = -1;
		}
	} else
		opbx_log(LOG_WARNING, "CheckGroup requires a positive integer argument (max)\n");

	LOCAL_USER_REMOVE(u);
	return res;
}

static int group_show_channels(int fd, int argc, char *argv[])
{
#define FORMAT_STRING  "%-25s  %-20s  %-20s\n"

	struct opbx_channel *c = NULL;
	int numchans = 0;
	struct opbx_var_t *current;
	struct varshead *headp;
	regex_t regexbuf;
	int havepattern = 0;

	if (argc < 3 || argc > 4)
		return RESULT_SHOWUSAGE;
	
	if (argc == 4) {
		if (regcomp(&regexbuf, argv[3], REG_EXTENDED | REG_NOSUB))
			return RESULT_SHOWUSAGE;
		havepattern = 1;
	}

	opbx_cli(fd, FORMAT_STRING, "Channel", "Group", "Category");
	while ( (c = opbx_channel_walk_locked(c)) != NULL) {
		headp=&c->varshead;
		OPBX_LIST_TRAVERSE(headp,current,entries) {
			if (!strncmp(opbx_var_name(current), GROUP_CATEGORY_PREFIX "_", strlen(GROUP_CATEGORY_PREFIX) + 1)) {
				if (!havepattern || !regexec(&regexbuf, opbx_var_value(current), 0, NULL, 0)) {
					opbx_cli(fd, FORMAT_STRING, c->name, opbx_var_value(current),
						(opbx_var_name(current) + strlen(GROUP_CATEGORY_PREFIX) + 1));
					numchans++;
				}
			} else if (!strcmp(opbx_var_name(current), GROUP_CATEGORY_PREFIX)) {
				if (!havepattern || !regexec(&regexbuf, opbx_var_value(current), 0, NULL, 0)) {
					opbx_cli(fd, FORMAT_STRING, c->name, opbx_var_value(current), "(default)");
					numchans++;
				}
			}
		}
		numchans++;
		opbx_mutex_unlock(&c->lock);
	}

	if (havepattern)
		regfree(&regexbuf);

	opbx_cli(fd, "%d active channel%s\n", numchans, (numchans != 1) ? "s" : "");
	return RESULT_SUCCESS;
#undef FORMAT_STRING
}

static char *tdesc = "Group Management Routines";

static char *app_group_count = "GetGroupCount";
static char *app_group_set = "SetGroup";
static char *app_group_check = "CheckGroup";
static char *app_group_match_count = "GetGroupMatchCount";

static char *group_count_synopsis = "Get the channel count of a group";
static char *group_set_synopsis = "Set the channel's group";
static char *group_check_synopsis = "Check the channel count of a group against a limit";
static char *group_match_count_synopsis = "Get the channel count of all groups that match a pattern";

static char *group_count_descrip =
"Usage: GetGroupCount([groupname][@category])\n"
"  Calculates the group count for the specified group, or uses\n"
"the current channel's group if not specifed (and non-empty).\n"
"Stores result in GROUPCOUNT.  Always returns 0.\n"
"This application has been deprecated, please use the function\n"
"GroupCount.\n";

static char *group_set_descrip =
"Usage: SetGroup(groupname[@category])\n"
"  Sets the channel group to the specified value.  Equivalent to\n"
"Set(GROUP=group).  Always returns 0.\n";

static char *group_check_descrip =
"Usage: CheckGroup(max[@category])\n"
"  Checks that the current number of total channels in the\n"
"current channel's group does not exceed 'max'.  If the number\n"
"does not exceed 'max', we continue to the next step. If the\n"
"number does in fact exceed max, if priority n+101 exists, then\n"
"execution continues at that step, otherwise -1 is returned.\n";

static char *group_match_count_descrip =
"Usage: GetGroupMatchCount(groupmatch[@category])\n"
"  Calculates the group count for all groups that match the specified\n"
"pattern. Uses standard regular expression matching (see regex(7)).\n"
"Stores result in GROUPCOUNT.  Always returns 0.\n"
"This application has been deprecated, please use the function\n"
"GroupMatchCount.\n";

static char show_channels_usage[] = 
"Usage: group show channels [pattern]\n"
"       Lists all currently active channels with channel group(s) specified.\n       Optional regular expression pattern is matched to group names for each channel.\n";

static struct opbx_cli_entry  cli_show_channels =
	{ { "group", "show", "channels", NULL }, group_show_channels, "Show active channels with group(s)", show_channels_usage};

int unload_module(void)
{
	int res;
	STANDARD_HANGUP_LOCALUSERS;
	opbx_cli_unregister(&cli_show_channels);
	res = opbx_unregister_application(app_group_count);
	res |= opbx_unregister_application(app_group_set);
	res |= opbx_unregister_application(app_group_check);
	res |= opbx_unregister_application(app_group_match_count);
	return res;
}

int load_module(void)
{
	int res;
	res = opbx_register_application(app_group_count, group_count_exec, group_count_synopsis, group_count_descrip);
	res |= opbx_register_application(app_group_set, group_set_exec, group_set_synopsis, group_set_descrip);
	res |= opbx_register_application(app_group_check, group_check_exec, group_check_synopsis, group_check_descrip);
	res |= opbx_register_application(app_group_match_count, group_match_count_exec, group_match_count_synopsis, group_match_count_descrip);
	opbx_cli_register(&cli_show_channels);
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

