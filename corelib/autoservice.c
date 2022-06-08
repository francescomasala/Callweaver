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
 * \brief Automatic channel service routines
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
#include <math.h>			/* For PI */

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/pbx.h"
#include "openpbx/frame.h"
#include "openpbx/sched.h"
#include "openpbx/options.h"
#include "openpbx/channel.h"
#include "openpbx/logger.h"
#include "openpbx/file.h"
#include "openpbx/translate.h"
#include "openpbx/manager.h"
#include "openpbx/chanvars.h"
#include "openpbx/linkedlists.h"
#include "openpbx/indications.h"
#include "openpbx/lock.h"
#include "openpbx/utils.h"

#define MAX_AUTOMONS 256

OPBX_MUTEX_DEFINE_STATIC(autolock);

struct asent {
	struct opbx_channel *chan;
	struct asent *next;
};

static struct asent *aslist = NULL;
static pthread_t asthread = OPBX_PTHREADT_NULL;

static void *autoservice_run(void *ign)
{
	struct opbx_channel *mons[MAX_AUTOMONS];
	int x;
	int ms;
	struct opbx_channel *chan;
	struct asent *as;
	struct opbx_frame *f;
	for(;;) {
		x = 0;
		opbx_mutex_lock(&autolock);
		as = aslist;
		while(as) {
			if (!as->chan->_softhangup) {
				if (x < MAX_AUTOMONS)
					mons[x++] = as->chan;
				else
					opbx_log(LOG_WARNING, "Exceeded maximum number of automatic monitoring events.  Fix autoservice.c\n");
			}
			as = as->next;
		}
		opbx_mutex_unlock(&autolock);

/* 		if (!aslist)
			break; */
		ms = 500;
		chan = opbx_waitfor_n(mons, x, &ms);
		if (chan) {
			/* Read and ignore anything that occurs */
			f = opbx_read(chan);
			if (f)
				opbx_frfree(f);
		}
	}
	asthread = OPBX_PTHREADT_NULL;
	return NULL;
}

int opbx_autoservice_start(struct opbx_channel *chan)
{
	int res = -1;
	struct asent *as;
	int needstart;
	opbx_mutex_lock(&autolock);
	needstart = (asthread == OPBX_PTHREADT_NULL) ? 1 : 0 /* aslist ? 0 : 1 */;
	as = aslist;
	while(as) {
		if (as->chan == chan)
			break;
		as = as->next;
	}
	if (!as) {
		as = malloc(sizeof(struct asent));
		if (as) {
			memset(as, 0, sizeof(struct asent));
			as->chan = chan;
			as->next = aslist;
			aslist = as;
			res = 0;
			if (needstart) {
				if (opbx_pthread_create(&asthread, NULL, autoservice_run, NULL)) {
					opbx_log(LOG_WARNING, "Unable to create autoservice thread :(\n");
					free(aslist);
					aslist = NULL;
					res = -1;
				} else
					pthread_kill(asthread, SIGURG);
			}
		}
	}
	opbx_mutex_unlock(&autolock);
	return res;
}

int opbx_autoservice_stop(struct opbx_channel *chan)
{
	int res = -1;
	struct asent *as, *prev;
	opbx_mutex_lock(&autolock);
	as = aslist;
	prev = NULL;
	while(as) {
		if (as->chan == chan)
			break;
		prev = as;
		as = as->next;
	}
	if (as) {
		if (prev)
			prev->next = as->next;
		else
			aslist = as->next;
		free(as);
		if (!chan->_softhangup)
			res = 0;
	}
	if (asthread != OPBX_PTHREADT_NULL) 
		pthread_kill(asthread, SIGURG);
	opbx_mutex_unlock(&autolock);
	/* Wait for it to un-block */
	while(opbx_test_flag(chan, OPBX_FLAG_BLOCKING))
		usleep(1000);
	return res;
}
