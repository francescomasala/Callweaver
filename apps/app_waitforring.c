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
 * \brief Wait for Ring Application
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/options.h"
#include "openpbx/lock.h"

static char *synopsis = "Wait for Ring Application";

static char *tdesc = "Waits until first ring after time";

static char *desc = "  WaitForRing(timeout)\n"
"Returns 0 after waiting at least timeout seconds. and\n"
"only after the next ring has completed.  Returns 0 on\n"
"success or -1 on hangup\n";

static char *app = "WaitForRing";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int waitforring_exec(struct opbx_channel *chan, void *data)
{
	struct localuser *u;
	struct opbx_frame *f;
	int res = 0;
	int ms;

	if (!data || (sscanf(data, "%d", &ms) != 1)) {
                opbx_log(LOG_WARNING, "WaitForRing requires an argument (minimum seconds)\n");
		return 0;
	}

	LOCAL_USER_ADD(u);

	ms *= 1000;
	while(ms > 0) {
		ms = opbx_waitfor(chan, ms);
		if (ms < 0) {
			res = ms;
			break;
		}
		if (ms > 0) {
			f = opbx_read(chan);
			if (!f) {
				res = -1;
				break;
			}
			if ((f->frametype == OPBX_FRAME_CONTROL) && (f->subclass == OPBX_CONTROL_RING)) {
				if (option_verbose > 2)
					opbx_verbose(VERBOSE_PREFIX_3 "Got a ring but still waiting for timeout\n");
			}
			opbx_frfree(f);
		}
	}
	/* Now we're really ready for the ring */
	if (!res) {
		ms = 99999999;
		while(ms > 0) {
			ms = opbx_waitfor(chan, ms);
			if (ms < 0) {
				res = ms;
				break;
			}
			if (ms > 0) {
				f = opbx_read(chan);
				if (!f) {
					res = -1;
					break;
				}
				if ((f->frametype == OPBX_FRAME_CONTROL) && (f->subclass == OPBX_CONTROL_RING)) {
					if (option_verbose > 2)
						opbx_verbose(VERBOSE_PREFIX_3 "Got a ring after the timeout\n");
					opbx_frfree(f);
					break;
				}
				opbx_frfree(f);
			}
		}
	}
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
	return opbx_register_application(app, waitforring_exec, synopsis, desc);
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


