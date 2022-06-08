/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 2005, Kevin P. Fleming
 *
 * Kevin P. Fleming <kpfleming@digium.com>
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
 * \brief Background DNS update manager
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <resolv.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <regex.h>
#include <signal.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/dnsmgr.h"
#include "openpbx/linkedlists.h"
#include "openpbx/utils.h"
#include "openpbx/config.h"
#include "openpbx/logger.h"
#include "openpbx/sched.h"
#include "openpbx/options.h"
#include "openpbx/cli.h"

static struct sched_context *sched;
static int refresh_sched = -1;
static pthread_t refresh_thread = OPBX_PTHREADT_NULL;

struct opbx_dnsmgr_entry {
	struct in_addr *result;
	OPBX_LIST_ENTRY(opbx_dnsmgr_entry) list;
	char name[1];
};

static OPBX_LIST_HEAD(entry_list, opbx_dnsmgr_entry) entry_list;

OPBX_MUTEX_DEFINE_STATIC(refresh_lock);

#define REFRESH_DEFAULT 300

static int enabled = 0;
static int refresh_interval;

struct refresh_info {
	struct entry_list *entries;
	int verbose;
	unsigned int regex_present:1;
	regex_t filter;
};

static struct refresh_info master_refresh_info = {
	.entries = &entry_list,
	.verbose = 0,
};

struct opbx_dnsmgr_entry *opbx_dnsmgr_get(const char *name, struct in_addr *result)
{
	struct opbx_dnsmgr_entry *entry;

	if (!result || opbx_strlen_zero(name))
		return NULL;

	entry = calloc(1, sizeof(*entry) + strlen(name));
	if (!entry)
		return NULL;

	entry->result = result;
	strcpy(entry->name, name);

	OPBX_LIST_LOCK(&entry_list);
	OPBX_LIST_INSERT_HEAD(&entry_list, entry, list);
	OPBX_LIST_UNLOCK(&entry_list);

	return entry;
}

void opbx_dnsmgr_release(struct opbx_dnsmgr_entry *entry)
{
	if (!entry)
		return;

	OPBX_LIST_LOCK(&entry_list);
	OPBX_LIST_REMOVE(&entry_list, entry, list);
	OPBX_LIST_UNLOCK(&entry_list);
	free(entry);
}

int opbx_dnsmgr_lookup(const char *name, struct in_addr *result, struct opbx_dnsmgr_entry **dnsmgr)
{
	if (opbx_strlen_zero(name) || !result || !dnsmgr)
		return -1;

	if (*dnsmgr && !strcasecmp((*dnsmgr)->name, name))
		return 0;

	if (option_verbose > 3)
		opbx_verbose(VERBOSE_PREFIX_3 "doing lookup for '%s'\n", name);

	/* if it's actually an IP address and not a name,
	   there's no need for a managed lookup */
	if (inet_aton(name, result))
		return 0;

	/* if the manager is disabled, do a direct lookup and return the result,
	   otherwise register a managed lookup for the name */
	if (!enabled) {
		struct opbx_hostent ahp;
		struct hostent *hp;

		if ((hp = opbx_gethostbyname(name, &ahp)))
			memcpy(result, hp->h_addr, sizeof(result));
		return 0;
	} else {
		if (option_verbose > 2)
			opbx_verbose(VERBOSE_PREFIX_2 "adding manager for '%s'\n", name);
		*dnsmgr = opbx_dnsmgr_get(name, result);
		return !*dnsmgr;
	}
}

static int refresh_list(void *data)
{
	struct refresh_info *info = data;
	struct opbx_dnsmgr_entry *entry;
	struct opbx_hostent ahp;
	struct hostent *hp;

	/* if a refresh or reload is already in progress, exit now */
	if (opbx_mutex_trylock(&refresh_lock)) {
		if (info->verbose)
			opbx_log(LOG_WARNING, "DNS Manager refresh already in progress.\n");
		return -1;
	}

	if (option_verbose > 2)
		opbx_verbose(VERBOSE_PREFIX_2 "Refreshing DNS lookups.\n");
	OPBX_LIST_LOCK(info->entries);
	OPBX_LIST_TRAVERSE(info->entries, entry, list) {
		if (info->regex_present && regexec(&info->filter, entry->name, 0, NULL, 0))
		    continue;

		if (info->verbose && (option_verbose > 2))
			opbx_verbose(VERBOSE_PREFIX_2 "refreshing '%s'\n", entry->name);

		if ((hp = opbx_gethostbyname(entry->name, &ahp))) {
			/* check to see if it has changed, do callback if requested */
			memcpy(entry->result, hp->h_addr, sizeof(entry->result));
		}
	}
	OPBX_LIST_UNLOCK(info->entries);

	opbx_mutex_unlock(&refresh_lock);

	/* automatically reschedule */
	return -1;
}

static int do_reload(int loading);

static int handle_cli_reload(int fd, int argc, char *argv[])
{
	if (argc > 2)
		return RESULT_SHOWUSAGE;

	do_reload(0);
	return 0;
}

static int handle_cli_refresh(int fd, int argc, char *argv[])
{
	struct refresh_info info = {
		.entries = &entry_list,
		.verbose = 1,
	};

	if (argc > 3)
		return RESULT_SHOWUSAGE;

	if (argc == 3) {
		if (regcomp(&info.filter, argv[2], REG_EXTENDED | REG_NOSUB))
			return RESULT_SHOWUSAGE;
		else
			info.regex_present = 1;
	}

	refresh_list(&info);

	if (info.regex_present)
		regfree(&info.filter);

	return 0;
}

static int handle_cli_status(int fd, int argc, char *argv[])
{
	int count = 0;
	struct opbx_dnsmgr_entry *entry;

	if (argc > 2)
		return RESULT_SHOWUSAGE;

	opbx_cli(fd, "DNS Manager: %s\n", enabled ? "enabled" : "disabled");
	opbx_cli(fd, "Refresh Interval: %d seconds\n", refresh_interval);
	OPBX_LIST_LOCK(&entry_list);
	OPBX_LIST_TRAVERSE(&entry_list, entry, list)
		count++;
	OPBX_LIST_UNLOCK(&entry_list);
	opbx_cli(fd, "Number of entries: %d\n", count);

	return 0;
}

static struct opbx_cli_entry cli_reload = {
	.cmda = { "dnsmgr", "reload", NULL },
	.handler = handle_cli_reload,
	.summary = "Reloads the DNS manager configuration",
	.usage = 
	"Usage: dnsmgr reload\n"
	"       Reloads the DNS manager configuration.\n"
};

static struct opbx_cli_entry cli_refresh = {
	.cmda = { "dnsmgr", "refresh", NULL },
	.handler = handle_cli_refresh,
	.summary = "Performs an immediate refresh",
	.usage = 
	"Usage: dnsmgr refresh [pattern]\n"
	"       Peforms an immediate refresh of the managed DNS entries.\n"
	"       Optional regular expression pattern is used to filter the entries to refresh.\n",
};

static struct opbx_cli_entry cli_status = {
	.cmda = { "dnsmgr", "status", NULL },
	.handler = handle_cli_status,
	.summary = "Display the DNS manager status",
	.usage =
	"Usage: dnsmgr status\n"
	"       Displays the DNS manager status.\n"
};

int dnsmgr_init(void)
{
	sched = sched_context_create();
	if (!sched) {
		opbx_log(LOG_ERROR, "Unable to create schedule context.\n");
		return -1;
	}
	OPBX_LIST_HEAD_INIT(&entry_list);
	opbx_cli_register(&cli_reload);
	opbx_cli_register(&cli_status);
	return do_reload(1);
}

void dnsmgr_reload(void)
{
	do_reload(0);
}

static int do_reload(int loading)
{
	struct opbx_config *config;
	const char *interval_value;
	const char *enabled_value;
	int interval;
	int was_enabled;
	int res = -1;

	/* ensure that no refresh cycles run while the reload is in progress */
	opbx_mutex_lock(&refresh_lock);

	/* reset defaults in preparation for reading config file */
	refresh_interval = REFRESH_DEFAULT;
	was_enabled = enabled;
	enabled = 0;

	if (refresh_sched > -1)
		opbx_sched_del(sched, refresh_sched);

	if ((config = opbx_config_load("dnsmgr.conf"))) {
		if ((enabled_value = opbx_variable_retrieve(config, "general", "enable"))) {
			enabled = opbx_true(enabled_value);
		}
		if ((interval_value = opbx_variable_retrieve(config, "general", "refreshinterval"))) {
			if (sscanf(interval_value, "%d", &interval) < 1)
				opbx_log(LOG_WARNING, "Unable to convert '%s' to a numeric value.\n", interval_value);
			else if (interval < 0)
				opbx_log(LOG_WARNING, "Invalid refresh interval '%d' specified, using default\n", interval);
			else
				refresh_interval = interval;
		}
		opbx_config_destroy(config);
	}

	if (enabled && refresh_interval) {
		refresh_sched = opbx_sched_add(sched, refresh_interval * 1000, refresh_list, &master_refresh_info);
		opbx_log(LOG_NOTICE, "Managed DNS entries will be refreshed every %d seconds.\n", refresh_interval);
	}

	/* if this reload enabled the manager, create the background thread
	   if it does not exist 
	   This no longer uses a thread since the scheduler has its own
	   timers now */
	if (enabled && !was_enabled && (refresh_thread == OPBX_PTHREADT_NULL)) {
		opbx_cli_register(&cli_refresh);
		res = 0;
	}
	/* if this reload disabled the manager and there is a background thread,
	   kill it */
	else if (!enabled && was_enabled && (refresh_thread != OPBX_PTHREADT_NULL)) {
		/* wake up the thread so it will exit */
		pthread_cancel(refresh_thread);
		pthread_kill(refresh_thread, SIGURG);
		pthread_join(refresh_thread, NULL);
		refresh_thread = OPBX_PTHREADT_NULL;
		opbx_cli_unregister(&cli_refresh);
		res = 0;
	}
	else
		res = 0;

	opbx_mutex_unlock(&refresh_lock);

	return res;
}
