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
 * \brief Device state management
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/channel.h"
#include "openpbx/utils.h"
#include "openpbx/lock.h"
#include "openpbx/linkedlists.h"
#include "openpbx/logger.h"
#include "openpbx/devicestate.h"
#include "openpbx/pbx.h"
#include "openpbx/options.h"

static const char *devstatestring[] = {
	/* 0 OPBX_DEVICE_UNKNOWN */	"Unknown",	/* Valid, but unknown state */
	/* 1 OPBX_DEVICE_NOT_INUSE */	"Not in use",	/* Not used */
	/* 2 OPBX_DEVICE IN USE */	"In use",	/* In use */
	/* 3 OPBX_DEVICE_BUSY */	"Busy",		/* Busy */
	/* 4 OPBX_DEVICE_INVALID */	"Invalid",	/* Invalid - not known to OpenPBX */
	/* 5 OPBX_DEVICE_UNAVAILABLE */	"Unavailable",	/* Unavailable (not registred) */
	/* 6 OPBX_DEVICE_RINGING */	"Ringing"	/* Ring, ring, ring */
};

/* opbx_devstate_cb: A device state watcher (callback) */
struct devstate_cb {
	void *data;
	opbx_devstate_cb_type callback;
	OPBX_LIST_ENTRY(devstate_cb) list;
};

static OPBX_LIST_HEAD_STATIC(devstate_cbs, devstate_cb);

struct state_change {
	OPBX_LIST_ENTRY(state_change) list;
	char device[1];
};

static OPBX_LIST_HEAD_STATIC(state_changes, state_change);

static pthread_t change_thread = OPBX_PTHREADT_NULL;
static opbx_cond_t change_pending;

/*--- devstate2str: Find devicestate as text message for output */
const char *devstate2str(int devstate) 
{
	return devstatestring[devstate];
}

/*--- opbx_parse_device_state: Find out if device is active in a call or not */
int opbx_parse_device_state(const char *device)
{
	struct opbx_channel *chan;
	char match[OPBX_CHANNEL_NAME] = "";
	int res;

	opbx_copy_string(match, device, sizeof(match)-1);
	strcat(match, "-");
	chan = opbx_get_channel_by_name_prefix_locked(match, strlen(match));

	if (!chan)
		return OPBX_DEVICE_UNKNOWN;

	if (chan->_state == OPBX_STATE_RINGING)
		res = OPBX_DEVICE_RINGING;
	else
		res = OPBX_DEVICE_INUSE;
	
	opbx_mutex_unlock(&chan->lock);

	return res;
}

/*--- opbx_device_state: Check device state through channel specific function or generic function */
int opbx_device_state(const char *device)
{
	char *buf;
	char *tech;
	char *number;
	const struct opbx_channel_tech *chan_tech;
	int res = 0;
	
	buf = opbx_strdupa(device);
	tech = strsep(&buf, "/");
	number = buf;
	if (!number)
		return OPBX_DEVICE_INVALID;
		
	chan_tech = opbx_get_channel_tech(tech);
	if (!chan_tech)
		return OPBX_DEVICE_INVALID;

	if (!chan_tech->devicestate) 	/* Does the channel driver support device state notification? */
		return opbx_parse_device_state(device);	/* No, try the generic function */
	else {
		res = chan_tech->devicestate(number);	/* Ask the channel driver for device state */
		if (res == OPBX_DEVICE_UNKNOWN) {
			res = opbx_parse_device_state(device);
			/* at this point we know the device exists, but the channel driver
			   could not give us a state; if there is no channel state available,
			   it must be 'not in use'
			*/
			if (res == OPBX_DEVICE_UNKNOWN)
				res = OPBX_DEVICE_NOT_INUSE;
			return res;
		} else
			return res;
	}
}

/*--- opbx_devstate_add: Add device state watcher */
int opbx_devstate_add(opbx_devstate_cb_type callback, void *data)
{
	struct devstate_cb *devcb;

	if (!callback)
		return -1;

	devcb = calloc(1, sizeof(*devcb));
	if (!devcb)
		return -1;

	devcb->data = data;
	devcb->callback = callback;

	OPBX_LIST_LOCK(&devstate_cbs);
	OPBX_LIST_INSERT_HEAD(&devstate_cbs, devcb, list);
	OPBX_LIST_UNLOCK(&devstate_cbs);

	return 0;
}

/*--- opbx_devstate_del: Remove device state watcher */
void opbx_devstate_del(opbx_devstate_cb_type callback, void *data)
{
	struct devstate_cb *devcb;

	OPBX_LIST_LOCK(&devstate_cbs);
	OPBX_LIST_TRAVERSE_SAFE_BEGIN(&devstate_cbs, devcb, list) {
		if ((devcb->callback == callback) && (devcb->data == data)) {
			OPBX_LIST_REMOVE_CURRENT(&devstate_cbs, list);
			free(devcb);
			break;
		}
	}
	OPBX_LIST_TRAVERSE_SAFE_END;
	OPBX_LIST_UNLOCK(&devstate_cbs);
}

/*--- do_state_change: Notify callback watchers of change, and notify PBX core for hint updates */
static inline void do_state_change(const char *device)
{
	int state;
	struct devstate_cb *devcb;

	state = opbx_device_state(device);
	if (option_debug > 2)
		opbx_log(LOG_DEBUG, "Changing state for %s - state %d (%s)\n", device, state, devstate2str(state));

	OPBX_LIST_LOCK(&devstate_cbs);
	OPBX_LIST_TRAVERSE(&devstate_cbs, devcb, list)
		devcb->callback(device, state, devcb->data);
	OPBX_LIST_UNLOCK(&devstate_cbs);

	opbx_hint_state_changed(device);
}

static int __opbx_device_state_changed_literal(char *buf)
{
	char *device, *tmp;
	struct state_change *change = NULL;

	device = buf;
	tmp = strrchr(device, '-');
	if (tmp)
		*tmp = '\0';
	if (change_thread != OPBX_PTHREADT_NULL)
		change = calloc(1, sizeof(*change) + strlen(device));

	if (!change) {
		/* we could not allocate a change struct, or */
		/* there is no background thread, so process the change now */
		do_state_change(device);
	} else {
		/* queue the change */
		strcpy(change->device, device);
		OPBX_LIST_LOCK(&state_changes);
		OPBX_LIST_INSERT_TAIL(&state_changes, change, list);
		if (OPBX_LIST_FIRST(&state_changes) == change)
			/* the list was empty, signal the thread */
			opbx_cond_signal(&change_pending);
		OPBX_LIST_UNLOCK(&state_changes);
	}

	return 1;
}

int opbx_device_state_changed_literal(const char *dev)
{
	char *buf;
	buf = opbx_strdupa(dev);
	return __opbx_device_state_changed_literal(buf);
}

/*--- opbx_device_state_changed: Accept change notification, add it to change queue */
int opbx_device_state_changed(const char *fmt, ...) 
{
	char buf[OPBX_MAX_EXTENSION];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return __opbx_device_state_changed_literal(buf);
}

/*--- do_devstate_changes: Go through the dev state change queue and update changes in the dev state thread */
static void *do_devstate_changes(void *data)
{
	struct state_change *cur=NULL;

	OPBX_LIST_LOCK(&state_changes);
	for(;;) {
		/* the list lock will _always_ be held at this point in the loop */
		cur = OPBX_LIST_REMOVE_HEAD(&state_changes, list);
		if (cur) {
			/* we got an entry, so unlock the list while we process it */
			OPBX_LIST_UNLOCK(&state_changes);
			do_state_change(cur->device);
			free(cur);
			OPBX_LIST_LOCK(&state_changes);
		} else {
			/* there was no entry, so atomically unlock the list and wait for
			   the condition to be signalled (returns with the lock held) */
			opbx_cond_wait(&change_pending, &state_changes.lock);
		}
	}

	return NULL;
}

/*--- opbx_device_state_engine_init: Initialize the device state engine in separate thread */
int opbx_device_state_engine_init(void)
{
	pthread_attr_t attr;

	opbx_cond_init(&change_pending, NULL);
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (opbx_pthread_create(&change_thread, &attr, do_devstate_changes, NULL) < 0) {
		opbx_log(LOG_ERROR, "Unable to start device state change thread.\n");
		return -1;
	}

	return 0;
}
