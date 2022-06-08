/*
 * vISDN channel driver for Asterisk
 *
 * Copyright (C) 2004-2005 Daniele Orlandi
 *
 * Authors: Daniele "Vihai" Orlandi <daniele@orlandi.com>
 *
 * This program is free software and may be modified and distributed
 * under the terms and conditions of the GNU General Public License.
 *
 */

#ifdef HAVE_CONFIG_H
 #include "confdefs.h"
#endif

#include <openpbx/channel.h>
#include <libq931/list.h>

static const char visdn_channeltype[] = "VISDN";
static const char visdn_description[] = "VISDN Channel Driver for OpenPBX.org";

struct visdn_suspended_call
{
	struct list_head node;

	struct opbx_channel *opbx_chan;
	struct q931_channel *q931_chan;

	char call_identity[10];
	int call_identity_len;

	time_t old_when_to_hangup;
};

struct visdn_chan {
	struct opbx_channel *opbx_chan;
	struct q931_call *q931_call;
	struct visdn_suspended_call *suspended_call;

	char visdn_chanid[30];
	int is_voice;
	int channel_fd;

	char calling_number[21];
	int sending_complete;

	int may_send_digits;
	char queued_digits[21];
};

static int visdn_call(struct opbx_channel*, char *, int);
static struct opbx_frame *visdn_exception(struct opbx_channel *);
static int visdn_hangup(struct opbx_channel*);
static int visdn_answer(struct opbx_channel*);
static struct opbx_frame *visdn_read(struct opbx_channel*);
static int visdn_write(struct opbx_channel*, struct opbx_frame *);
static int visdn_indicate(struct opbx_channel*, int);
static int visdn_transfer(struct opbx_channel*, const char *);
static int visdn_fixup(struct opbx_channel*, struct opbx_channel *);
static int visdn_send_digit(struct opbx_channel*, char);
static int visdn_sendtext(struct opbx_channel*, const char *);
static int visdn_bridge(struct opbx_channel*, struct opbx_channel*, int, struct opbx_frame **, struct opbx_channel **, int);
static int visdn_setoption(struct opbx_channel*, int, void *, int);
static struct opbx_channel *visdn_request(const char *, int, void *, int *);

static const struct opbx_channel_tech visdn_tech = {
	.type = visdn_channeltype,
	.description = visdn_description,
	.exception = visdn_exception,
	.call = visdn_call,
	.hangup = visdn_hangup,
	.answer = visdn_answer,
	.read = visdn_read,
	.write = visdn_write,
	.indicate = visdn_indicate,
	.transfer = visdn_transfer,
	.fixup = visdn_fixup,
	.send_digit = visdn_send_digit,
	.send_text = visdn_sendtext,
	.bridge = visdn_bridge,
	.setoption = visdn_setoption,
	.capabilities = OPBX_FORMAT_ALAW,
	.requester = visdn_request,
};

