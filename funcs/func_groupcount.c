/*
 * CallWeaver -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
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
 * \brief Channel group related dialplan functions
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/funcs/func_groupcount.c $", "$Revision: 4723 $")

#include "callweaver/module.h"
#include "callweaver/channel.h"
#include "callweaver/pbx.h"
#include "callweaver/logger.h"
#include "callweaver/utils.h"
#include "callweaver/app.h"


static void *group_count_function;
static const char *group_count_func_name = "GROUP_COUNT";
static const char *group_count_func_syntax = "GROUP_COUNT([groupname][@category])";
static const char *group_count_func_synopsis = "Counts the number of channels in the specified group";
static const char *group_count_func_desc =
	"Calculates the group count for the specified group, or uses the\n"
	"channel's current group if not specifed (and non-empty).\n";

static void *group_match_count_function;
static const char *group_match_count_func_name = "GROUP_MATCH_COUNT";
static const char *group_match_count_func_syntax = "GROUP_MATCH_COUNT(groupmatch[@category])";
static const char *group_match_count_func_synopsis = "Counts the number of channels in the groups matching the specified pattern";
static const char *group_match_count_func_desc =
	"Calculates the group count for all groups that match the specified pattern.\n"
	"Uses standard regular expression matching (see regex(7)).\n";

static void *group_function;
static const char *group_func_name = "GROUP";
static const char *group_func_synopsis = "Gets or sets the channel group.";
static const char *group_func_syntax = "GROUP([category])";
static const char *group_func_desc = "Gets or sets the channel group.\n";

static void *group_list_function;
static const char *group_list_func_name = "GROUP_LIST";
static const char *group_list_func_synopsis = "Gets a list of the groups set on a channel.";
static const char *group_list_func_syntax = "GROUP_LIST()";
static const char *group_list_func_desc = "Gets a list of the groups set on a channel.\n";


static char *group_count_function_read(struct cw_channel *chan, int argc, char **argv, char *buf, size_t len) 
{
	int count;
	char group[80] = "";
	char category[80] = "";
	char *grp;

	cw_app_group_split_group(argv[0], group, sizeof(group), category, sizeof(category));

	if (cw_strlen_zero(group)) {
		if ((grp = pbx_builtin_getvar_helper(chan, category)))
			cw_copy_string(group, grp, sizeof(group));
		else
			cw_log(LOG_NOTICE, "No group could be found for channel '%s'\n", chan->name);	
	}

	count = cw_app_group_get_count(group, category);
	snprintf(buf, len, "%d", count);

	return buf;
}

static char *group_match_count_function_read(struct cw_channel *chan, int argc, char **argv, char *buf, size_t len) 
{
	int count;
	char group[80] = "";
	char category[80] = "";

	cw_app_group_split_group(argv[0], group, sizeof(group), category, sizeof(category));

	if (!cw_strlen_zero(group)) {
		count = cw_app_group_match_get_count(group, category);
		snprintf(buf, len, "%d", count);
	}

	return buf;
}

static char *group_function_read(struct cw_channel *chan, int argc, char **argv, char *buf, size_t len)
{
	char varname[256];
	char *group;

	if (argc > 0 && argv[0][0]) {
		snprintf(varname, sizeof(varname), "%s_%s", GROUP_CATEGORY_PREFIX, argv[0]);
	} else {
		cw_copy_string(varname, GROUP_CATEGORY_PREFIX, sizeof(varname));
	}

	group = pbx_builtin_getvar_helper(chan, varname);
	if (group)
		cw_copy_string(buf, group, len);

	return buf;
}

static void group_function_write(struct cw_channel *chan, int argc, char **argv, const char *value)
{
	char grpcat[256];

	if (argc > 0 && argv[0][0]) {
		snprintf(grpcat, sizeof(grpcat), "%s@%s", value, argv[0]);
	} else {
		cw_copy_string(grpcat, value, sizeof(grpcat));
	}

        if (cw_app_group_set_channel(chan, grpcat))
                cw_log(LOG_WARNING, "Setting a group requires an argument (group name)\n");
}

static char *group_list_function_read(struct cw_channel *chan, int argc, char **argv, char *buf, size_t len)
{
	struct cw_var_t *current;
	struct varshead *headp;
	char tmp1[1024] = "";
	char tmp2[1024] = "";

	headp=&chan->varshead;
	CW_LIST_TRAVERSE(headp,current,entries) {
		if (!strncmp(cw_var_name(current), GROUP_CATEGORY_PREFIX "_", strlen(GROUP_CATEGORY_PREFIX) + 1)) {
			if (!cw_strlen_zero(tmp1)) {
				cw_copy_string(tmp2, tmp1, sizeof(tmp2));
				snprintf(tmp1, sizeof(tmp1), "%s %s@%s", tmp2, cw_var_value(current), (cw_var_name(current) + strlen(GROUP_CATEGORY_PREFIX) + 1));
			} else {
				snprintf(tmp1, sizeof(tmp1), "%s@%s", cw_var_value(current), (cw_var_name(current) + strlen(GROUP_CATEGORY_PREFIX) + 1));
			}
		} else if (!strcmp(cw_var_name(current), GROUP_CATEGORY_PREFIX)) {
			if (!cw_strlen_zero(tmp1)) {
				cw_copy_string(tmp2, tmp1, sizeof(tmp2));
				snprintf(tmp1, sizeof(tmp1), "%s %s", tmp2, cw_var_value(current));
			} else {
				snprintf(tmp1, sizeof(tmp1), "%s", cw_var_value(current));
			}
		}
	}
	cw_copy_string(buf, tmp1, len);
	return buf;
}

static char *tdesc = "database functions";

int unload_module(void)
{
        int res = 0;

	res |= cw_unregister_function(group_count_function);
	res |= cw_unregister_function(group_match_count_function);
	res |= cw_unregister_function(group_function);
	res |= cw_unregister_function(group_list_function);
        return res;
}

int load_module(void)
{
	group_count_function = cw_register_function(group_count_func_name, group_count_function_read, NULL, group_count_func_synopsis, group_count_func_syntax, group_count_func_desc);
	group_match_count_function = cw_register_function(group_match_count_func_name, group_match_count_function_read, NULL, group_match_count_func_synopsis, group_match_count_func_syntax, group_match_count_func_desc);
	group_function = cw_register_function(group_func_name, group_function_read, group_function_write, group_func_synopsis, group_func_syntax, group_func_desc);
	group_list_function = cw_register_function(group_list_func_name, group_list_function_read, NULL, group_list_func_synopsis, group_list_func_syntax, group_list_func_desc);
        return 0;
}

char *description(void)
{
	return tdesc;
}

int usecount(void)
{
	return 0;
}

/*
Local Variables:
mode: C
c-file-style: "linux"
indent-tabs-mode: nil
End:
*/
