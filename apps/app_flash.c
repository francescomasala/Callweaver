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
 * \brief App to flash a zap trunk
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h> 
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include ZAPTEL_H

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
#include "openpbx/options.h"

static char *tdesc = "Flash zap trunk application";

static char *app = "Flash";

static char *synopsis = "Flashes a Zap Trunk";

static char *descrip = 
"  Flash(): Sends a flash on a zap trunk.  This is only a hack for\n"
"people who want to perform transfers and such via OGI and is generally\n"
"quite useless otherwise.  Returns 0 on success or -1 if this is not\n"
"a zap trunk\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static inline int zt_wait_event(int fd)
{
	/* Avoid the silly zt_waitevent which ignores a bunch of events */
	int i,j=0;
	i = ZT_IOMUX_SIGEVENT;
	if (ioctl(fd, ZT_IOMUX, &i) == -1) return -1;
	if (ioctl(fd, ZT_GETEVENT, &j) == -1) return -1;
	return j;
}

static int flash_exec(struct opbx_channel *chan, void *data)
{
	int res = -1;
	int x;
	struct localuser *u;
	struct zt_params ztp;
	LOCAL_USER_ADD(u);
	if (!strcasecmp(chan->type, "Zap")) {
		memset(&ztp, 0, sizeof(ztp));
		res = ioctl(chan->fds[0], ZT_GET_PARAMS, &ztp);
		if (!res) {
			if (ztp.sigtype & __ZT_SIG_FXS) {
				x = ZT_FLASH;
				res = ioctl(chan->fds[0], ZT_HOOK, &x);
				if (!res || (errno == EINPROGRESS)) {
					if (res) {
						/* Wait for the event to finish */
						zt_wait_event(chan->fds[0]);
					}
					res = opbx_safe_sleep(chan, 1000);
					if (option_verbose > 2)
						opbx_verbose(VERBOSE_PREFIX_3 "Flashed channel %s\n", chan->name);
				} else
					opbx_log(LOG_WARNING, "Unable to flash channel %s: %s\n", chan->name, strerror(errno));
			} else
				opbx_log(LOG_WARNING, "%s is not an FXO Channel\n", chan->name);
		} else
			opbx_log(LOG_WARNING, "Unable to get parameters of %s: %s\n", chan->name, strerror(errno));
	} else
		opbx_log(LOG_WARNING, "%s is not a Zap channel\n", chan->name);
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
	return opbx_register_application(app, flash_exec, synopsis, descrip);
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


