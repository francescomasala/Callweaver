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
 * \brief Privacy Routines
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/channel.h"
#include "openpbx/file.h"
#include "openpbx/app.h"
#include "openpbx/dsp.h"
#include "openpbx/logger.h"
#include "openpbx/options.h"
#include "openpbx/opbxdb.h"
#include "openpbx/phone_no_utils.h"
#include "openpbx/privacy.h"
#include "openpbx/utils.h"
#include "openpbx/lock.h"

int opbx_privacy_check(char *dest, char *cid)
{
	char tmp[256] = "";
	char *trimcid = "";
	char *n, *l;
	int res;
	char key[256], result[256];
	if (cid)
		opbx_copy_string(tmp, cid, sizeof(tmp));
	opbx_callerid_parse(tmp, &n, &l);
	if (l) {
		opbx_shrink_phone_number(l);
		trimcid = l;
	}
	snprintf(key, sizeof(key), "%s/%s", dest, trimcid);
	res = opbx_db_get("privacy", key, result, sizeof(result));
	if (!res) {
		if (!strcasecmp(result, "allow"))
			return OPBX_PRIVACY_ALLOW;
		if (!strcasecmp(result, "deny"))
			return OPBX_PRIVACY_DENY;
		if (!strcasecmp(result, "kill"))
			return OPBX_PRIVACY_KILL;
		if (!strcasecmp(result, "torture"))
			return OPBX_PRIVACY_TORTURE;
	}
	return OPBX_PRIVACY_UNKNOWN;
}

int opbx_privacy_reset(char *dest)
{
	if (!dest)
		return -1;
	return opbx_db_deltree("privacy", dest);
}

int opbx_privacy_set(char *dest, char *cid, int status)
{
	char tmp[256] = "";
	char *trimcid = "";
	char *n, *l;
	int res;
	char key[256];
	if (cid)
		opbx_copy_string(tmp, cid, sizeof(tmp));
	opbx_callerid_parse(tmp, &n, &l);
	if (l) {
		opbx_shrink_phone_number(l);
		trimcid = l;
	}
	if (opbx_strlen_zero(trimcid)) {
		/* Don't store anything for empty Caller*ID */
		return 0;
	}
	snprintf(key, sizeof(key), "%s/%s", dest, trimcid);
	if (status == OPBX_PRIVACY_UNKNOWN) 
		res = opbx_db_del("privacy", key);
	else if (status == OPBX_PRIVACY_ALLOW)
		res = opbx_db_put("privacy", key, "allow");
	else if (status == OPBX_PRIVACY_DENY)
		res = opbx_db_put("privacy", key, "deny");
	else if (status == OPBX_PRIVACY_KILL)
		res = opbx_db_put("privacy", key, "kill");
	else if (status == OPBX_PRIVACY_TORTURE)
		res = opbx_db_put("privacy", key, "torture");
	else
		res = -1;
	return res;
}
