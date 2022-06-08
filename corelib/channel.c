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
 * \brief Channel Management
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
#include <math.h>
#include <spandsp.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/pbx.h"
#include "openpbx/frame.h"
#include "openpbx/sched.h"
#include "openpbx/options.h"
#include "openpbx/channel.h"
#include "openpbx/musiconhold.h"
#include "openpbx/logger.h"
#include "openpbx/say.h"
#include "openpbx/file.h"
#include "openpbx/cli.h"
#include "openpbx/translate.h"
#include "openpbx/manager.h"
#include "openpbx/chanvars.h"
#include "openpbx/linkedlists.h"
#include "openpbx/indications.h"
#include "openpbx/monitor.h"
#include "openpbx/causes.h"
#include "openpbx/phone_no_utils.h"
#include "openpbx/utils.h"
#include "openpbx/lock.h"
#include "openpbx/app.h"
#include "openpbx/transcap.h"
#include "openpbx/devicestate.h"

/* uncomment if you have problems with 'monitoring' synchronized files */
#if 0
#define MONITOR_CONSTANT_DELAY
#define MONITOR_DELAY	150 * 8		/* 150 ms of MONITORING DELAY */
#endif

/*
 * Prevent new channel allocation if shutting down.
 */
static int shutting_down = 0;

static int uniqueint = 0;

unsigned long global_fin = 0, global_fout = 0;

/* XXX Lock appropriately in more functions XXX */

struct chanlist {
	const struct opbx_channel_tech *tech;
	struct chanlist *next;
};

static struct chanlist *backends = NULL;

/*
 * the list of channels we have
 */
static struct opbx_channel *channels = NULL;

/* Protect the channel list, both backends and channels.
 */
OPBX_MUTEX_DEFINE_STATIC(chlock);

const struct opbx_cause {
	int cause;
	const char *desc;
} causes[] = {
	{ OPBX_CAUSE_UNALLOCATED, "Unallocated (unassigned) number" },
	{ OPBX_CAUSE_NO_ROUTE_TRANSIT_NET, "No route to specified transmit network" },
	{ OPBX_CAUSE_NO_ROUTE_DESTINATION, "No route to destination" },
	{ OPBX_CAUSE_CHANNEL_UNACCEPTABLE, "Channel unacceptable" },
	{ OPBX_CAUSE_CALL_AWARDED_DELIVERED, "Call awarded and being delivered in an established channel" },
	{ OPBX_CAUSE_NORMAL_CLEARING, "Normal Clearing" },
	{ OPBX_CAUSE_USER_BUSY, "User busy" },
	{ OPBX_CAUSE_NO_USER_RESPONSE, "No user responding" },
	{ OPBX_CAUSE_NO_ANSWER, "User alerting, no answer" },
	{ OPBX_CAUSE_CALL_REJECTED, "Call Rejected" },
	{ OPBX_CAUSE_NUMBER_CHANGED, "Number changed" },
	{ OPBX_CAUSE_DESTINATION_OUT_OF_ORDER, "Destination out of order" },
	{ OPBX_CAUSE_INVALID_NUMBER_FORMAT, "Invalid number format" },
	{ OPBX_CAUSE_FACILITY_REJECTED, "Facility rejected" },
	{ OPBX_CAUSE_RESPONSE_TO_STATUS_ENQUIRY, "Response to STATus ENQuiry" },
	{ OPBX_CAUSE_NORMAL_UNSPECIFIED, "Normal, unspecified" },
	{ OPBX_CAUSE_NORMAL_CIRCUIT_CONGESTION, "Circuit/channel congestion" },
	{ OPBX_CAUSE_NETWORK_OUT_OF_ORDER, "Network out of order" },
	{ OPBX_CAUSE_NORMAL_TEMPORARY_FAILURE, "Temporary failure" },
	{ OPBX_CAUSE_SWITCH_CONGESTION, "Switching equipment congestion" },
	{ OPBX_CAUSE_ACCESS_INFO_DISCARDED, "Access information discarded" },
	{ OPBX_CAUSE_REQUESTED_CHAN_UNAVAIL, "Requested channel not available" },
	{ OPBX_CAUSE_PRE_EMPTED, "Pre-empted" },
	{ OPBX_CAUSE_FACILITY_NOT_SUBSCRIBED, "Facility not subscribed" },
	{ OPBX_CAUSE_OUTGOING_CALL_BARRED, "Outgoing call barred" },
	{ OPBX_CAUSE_INCOMING_CALL_BARRED, "Incoming call barred" },
	{ OPBX_CAUSE_BEARERCAPABILITY_NOTAUTH, "Bearer capability not authorized" },
	{ OPBX_CAUSE_BEARERCAPABILITY_NOTAVAIL, "Bearer capability not available" },
	{ OPBX_CAUSE_BEARERCAPABILITY_NOTIMPL, "Bearer capability not implemented" },
	{ OPBX_CAUSE_CHAN_NOT_IMPLEMENTED, "Channel not implemented" },
	{ OPBX_CAUSE_FACILITY_NOT_IMPLEMENTED, "Facility not implemented" },
	{ OPBX_CAUSE_INVALID_CALL_REFERENCE, "Invalid call reference value" },
	{ OPBX_CAUSE_INCOMPATIBLE_DESTINATION, "Incompatible destination" },
	{ OPBX_CAUSE_INVALID_MSG_UNSPECIFIED, "Invalid message unspecified" },
	{ OPBX_CAUSE_MANDATORY_IE_MISSING, "Mandatory information element is missing" },
	{ OPBX_CAUSE_MESSAGE_TYPE_NONEXIST, "Message type nonexist." },
	{ OPBX_CAUSE_WRONG_MESSAGE, "Wrong message" },
	{ OPBX_CAUSE_IE_NONEXIST, "Info. element nonexist or not implemented" },
	{ OPBX_CAUSE_INVALID_IE_CONTENTS, "Invalid information element contents" },
	{ OPBX_CAUSE_WRONG_CALL_STATE, "Message not compatible with call state" },
	{ OPBX_CAUSE_RECOVERY_ON_TIMER_EXPIRE, "Recover on timer expiry" },
	{ OPBX_CAUSE_MANDATORY_IE_LENGTH_ERROR, "Mandatory IE length error" },
	{ OPBX_CAUSE_PROTOCOL_ERROR, "Protocol error, unspecified" },
	{ OPBX_CAUSE_INTERWORKING, "Interworking, unspecified" },
};

/* Control frame types */
const struct opbx_control {
	int control;
	const char *desc;
} controles[] = {
	{OPBX_CONTROL_HANGUP, "Other end has hungup"},
	{OPBX_CONTROL_RING, "Local ring"},
	{OPBX_CONTROL_RINGING, "Remote end is ringing"},
	{OPBX_CONTROL_ANSWER,"Remote end has answered"},
	{OPBX_CONTROL_BUSY, "Remote end is busy"},
	{OPBX_CONTROL_TAKEOFFHOOK, "Make it go off hook"},
	{OPBX_CONTROL_OFFHOOK, "Line is off hook"},
	{OPBX_CONTROL_CONGESTION, "Congestion (circuits busy"},
	{OPBX_CONTROL_FLASH, "Flash hook"},
	{OPBX_CONTROL_WINK, "Wink"},
	{OPBX_CONTROL_OPTION, "Set a low-level option"},
	{OPBX_CONTROL_RADIO_KEY, "Key Radio"},
	{OPBX_CONTROL_RADIO_UNKEY, "Un-Key Radio"},
	{OPBX_CONTROL_PROGRESS, "Indicate PROGRESS"},
	{OPBX_CONTROL_PROCEEDING, "Indicate CALL PROCEEDING"},
	{OPBX_CONTROL_HOLD, "Indicate call is placed on hold"},
	{OPBX_CONTROL_UNHOLD, "Indicate call is left from hold"},
	{OPBX_CONTROL_VIDUPDATE, "Indicate video frame update"},
};

static int show_channeltypes(int fd, int argc, char *argv[])
{
#define FORMAT  "%-10.10s  %-30.30s %-12.12s %-12.12s %-12.12s\n"
	struct chanlist *cl;
	opbx_cli(fd, FORMAT, "Type", "Description",       "Devicestate", "Indications", "Transfer");
	opbx_cli(fd, FORMAT, "----------", "-----------", "-----------", "-----------", "--------");
	if (opbx_mutex_lock(&chlock)) {
		opbx_log(LOG_WARNING, "Unable to lock channel list\n");
		return -1;
	}
	cl = backends;
	while (cl) {
		opbx_cli(fd, FORMAT, cl->tech->type, cl->tech->description, 
			(cl->tech->devicestate) ? "yes" : "no", 
			(cl->tech->indicate) ? "yes" : "no",
			(cl->tech->transfer) ? "yes" : "no");
		cl = cl->next;
	}
	opbx_mutex_unlock(&chlock);
	return RESULT_SUCCESS;

#undef FORMAT

}

static char show_channeltypes_usage[] = 
"Usage: show channeltypes\n"
"       Shows available channel types registered in your OpenPBX server.\n";

static struct opbx_cli_entry cli_show_channeltypes = 
	{ { "show", "channeltypes", NULL }, show_channeltypes, "Show available channel types", show_channeltypes_usage };

/*--- opbx_check_hangup: Checks to see if a channel is needing hang up */
int opbx_check_hangup(struct opbx_channel *chan)
{
	time_t	myt;

	/* if soft hangup flag, return true */
	if (chan->_softhangup) 
		return 1;
	/* if no technology private data, return true */
	if (!chan->tech_pvt) 
		return 1;
	/* if no hangup scheduled, just return here */
	if (!chan->whentohangup) 
		return 0;
	time(&myt); /* get current time */
	/* return, if not yet */
	if (chan->whentohangup > myt) 
		return 0;
	chan->_softhangup |= OPBX_SOFTHANGUP_TIMEOUT;
	return 1;
}

static int opbx_check_hangup_locked(struct opbx_channel *chan)
{
	int res;
	opbx_mutex_lock(&chan->lock);
	res = opbx_check_hangup(chan);
	opbx_mutex_unlock(&chan->lock);
	return res;
}

/*--- opbx_begin_shutdown: Initiate system shutdown */
void opbx_begin_shutdown(int hangup)
{
	struct opbx_channel *c;
	shutting_down = 1;
	if (hangup) {
		opbx_mutex_lock(&chlock);
		c = channels;
		while(c) {
			opbx_softhangup(c, OPBX_SOFTHANGUP_SHUTDOWN);
			c = c->next;
		}
		opbx_mutex_unlock(&chlock);
	}
}

/*--- opbx_active_channels: returns number of active/allocated channels */
int opbx_active_channels(void)
{
	struct opbx_channel *c;
	int cnt = 0;
	opbx_mutex_lock(&chlock);
	c = channels;
	while(c) {
		cnt++;
		c = c->next;
	}
	opbx_mutex_unlock(&chlock);
	return cnt;
}

/*--- opbx_cancel_shutdown: Cancel a shutdown in progress */
void opbx_cancel_shutdown(void)
{
	shutting_down = 0;
}

/*--- opbx_shutting_down: Returns non-zero if OpenPBX is being shut down */
int opbx_shutting_down(void)
{
	return shutting_down;
}

/*--- opbx_channel_setwhentohangup: Set when to hangup channel */
void opbx_channel_setwhentohangup(struct opbx_channel *chan, time_t offset)
{
	time_t	myt;
	struct opbx_frame fr = { OPBX_FRAME_NULL, };

	time(&myt);
	if (offset)
		chan->whentohangup = myt + offset;
	else
		chan->whentohangup = 0;
	opbx_queue_frame(chan, &fr);
	return;
}
/*--- opbx_channel_cmpwhentohangup: Compare a offset with when to hangup channel */
int opbx_channel_cmpwhentohangup(struct opbx_channel *chan, time_t offset)
{
	time_t whentohangup;

	if (chan->whentohangup == 0) {
		if (offset == 0)
			return (0);
		else
			return (-1);
	} else { 
		if (offset == 0)
			return (1);
		else {
			whentohangup = offset + time (NULL);
			if (chan->whentohangup < whentohangup)
				return (1);
			else if (chan->whentohangup == whentohangup)
				return (0);
			else
				return (-1);
		}
	}
}

/*--- opbx_channel_register: Register a new telephony channel in OpenPBX */
int opbx_channel_register(const struct opbx_channel_tech *tech)
{
	struct chanlist *chan;

	opbx_mutex_lock(&chlock);

	chan = backends;
	while (chan) {
		if (!strcasecmp(tech->type, chan->tech->type)) {
			opbx_log(LOG_WARNING, "Already have a handler for type '%s'\n", tech->type);
			opbx_mutex_unlock(&chlock);
			return -1;
		}
		chan = chan->next;
	}

	chan = malloc(sizeof(*chan));
	if (!chan) {
		opbx_log(LOG_WARNING, "Out of memory\n");
		opbx_mutex_unlock(&chlock);
		return -1;
	}
	chan->tech = tech;
	chan->next = backends;
	backends = chan;

	if (option_debug)
		opbx_log(LOG_DEBUG, "Registered handler for '%s' (%s)\n", chan->tech->type, chan->tech->description);

	if (option_verbose > 1)
		opbx_verbose(VERBOSE_PREFIX_2 "Registered channel type '%s' (%s)\n", chan->tech->type,
			    chan->tech->description);

	opbx_mutex_unlock(&chlock);
	return 0;
}

void opbx_channel_unregister(const struct opbx_channel_tech *tech)
{
	struct chanlist *chan, *last=NULL;

	if (option_debug)
		opbx_log(LOG_DEBUG, "Unregistering channel type '%s'\n", tech->type);

	opbx_mutex_lock(&chlock);

	chan = backends;
	while (chan) {
		if (chan->tech == tech) {
			if (last)
				last->next = chan->next;
			else
				backends = backends->next;
			free(chan);
			opbx_mutex_unlock(&chlock);

			if (option_verbose > 1)
				opbx_verbose( VERBOSE_PREFIX_2 "Unregistered channel type '%s'\n", tech->type);

			return;
		}
		last = chan;
		chan = chan->next;
	}

	opbx_mutex_unlock(&chlock);
}

const struct opbx_channel_tech *opbx_get_channel_tech(const char *name)
{
	struct chanlist *chanls;

	if (opbx_mutex_lock(&chlock)) {
		opbx_log(LOG_WARNING, "Unable to lock channel tech list\n");
		return NULL;
	}

	for (chanls = backends; chanls; chanls = chanls->next) {
		if (strcasecmp(name, chanls->tech->type))
			continue;

		opbx_mutex_unlock(&chlock);
		return chanls->tech;
	}

	opbx_mutex_unlock(&chlock);
	return NULL;
}

/*--- opbx_cause2str: Gives the string form of a given hangup cause */
const char *opbx_cause2str(int cause)
{
	int x;

	for (x=0; x < sizeof(causes) / sizeof(causes[0]); x++) 
		if (causes[x].cause == cause)
			return causes[x].desc;

	return "Unknown";
}

/*--- opbx_control2str: Gives the string form of a given control frame */
const char *opbx_control2str(int control)
{
	int x;

	for (x=0; x < sizeof(controles) / sizeof(controles[0]); x++) 
		if (controles[x].control == control)
			return controles[x].desc;

	return "Unknown";
}

/*--- opbx_state2str: Gives the string form of a given channel state */
char *opbx_state2str(int state)
{
	/* XXX Not reentrant XXX */
	static char localtmp[256];
	switch(state) {
	case OPBX_STATE_DOWN:
		return "Down";
	case OPBX_STATE_RESERVED:
		return "Rsrvd";
	case OPBX_STATE_OFFHOOK:
		return "OffHook";
	case OPBX_STATE_DIALING:
		return "Dialing";
	case OPBX_STATE_RING:
		return "Ring";
	case OPBX_STATE_RINGING:
		return "Ringing";
	case OPBX_STATE_UP:
		return "Up";
	case OPBX_STATE_BUSY:
		return "Busy";
	default:
		snprintf(localtmp, sizeof(localtmp), "Unknown (%d)\n", state);
		return localtmp;
	}
}

/*--- opbx_transfercapability2str: Gives the string form of a given transfer capability */
char *opbx_transfercapability2str(int transfercapability)
{
	switch(transfercapability) {
	case OPBX_TRANS_CAP_SPEECH:
		return "SPEECH";
	case OPBX_TRANS_CAP_DIGITAL:
		return "DIGITAL";
	case OPBX_TRANS_CAP_RESTRICTED_DIGITAL:
		return "RESTRICTED_DIGITAL";
	case OPBX_TRANS_CAP_3_1K_AUDIO:
		return "3K1AUDIO";
	case OPBX_TRANS_CAP_DIGITAL_W_TONES:
		return "DIGITAL_W_TONES";
	case OPBX_TRANS_CAP_VIDEO:
		return "VIDEO";
	default:
		return "UNKNOWN";
	}
}

/*--- opbx_best_codec: Pick the best codec */
int opbx_best_codec(int fmts)
{
	/* This just our opinion, expressed in code.  We are asked to choose
	   the best codec to use, given no information */
	int x;
	static const int prefs[] = 
	{
		/* Okay, ulaw is used by all telephony equipment, so start with it */
		OPBX_FORMAT_ULAW,
		/* Unless of course, you're a European, so then prefer ALAW */
		OPBX_FORMAT_ALAW,
		/* Okay, well, signed linear is easy to translate into other stuff */
		OPBX_FORMAT_SLINEAR,
		/* G.726 is standard ADPCM */
		OPBX_FORMAT_G726,
		/* ADPCM has great sound quality and is still pretty easy to translate */
		OPBX_FORMAT_DVI_ADPCM,
		/* Okay, we're down to vocoders now, so pick GSM because it's small and easier to
		   translate and sounds pretty good */
		OPBX_FORMAT_GSM,
		/* iLBC is not too bad */
		OPBX_FORMAT_ILBC,
		/* Speex is free, but computationally more expensive than GSM */
		OPBX_FORMAT_SPEEX,
		/* Ick, LPC10 sounds terrible, but at least we have code for it, if you're tacky enough
		   to use it */
		OPBX_FORMAT_LPC10,
		/* G.729a is faster than 723 and slightly less expensive */
		OPBX_FORMAT_G729A,
		/* Down to G.723.1 which is proprietary but at least designed for voice */
		OPBX_FORMAT_G723_1,
        OPBX_FORMAT_OKI_ADPCM,
	};

	/* Find the first prefered codec in the format given */
	for (x=0; x < (sizeof(prefs) / sizeof(prefs[0]) ); x++)
		if (fmts & prefs[x])
			return prefs[x];
	opbx_log(LOG_WARNING, "Don't know any of 0x%x formats\n", fmts);
	return 0;
}

static const struct opbx_channel_tech null_tech = {
	.type = "NULL",
	.description = "Null channel (should not see this)",
};

/*--- opbx_channel_alloc: Create a new channel structure */
struct opbx_channel *opbx_channel_alloc(int needqueue)
{
	struct opbx_channel *tmp;
	int x;
	int flags;
	struct varshead *headp;        
	        

	/* If shutting down, don't allocate any new channels */
	if (shutting_down) {
		opbx_log(LOG_NOTICE, "Refusing channel allocation due to active shutdown\n");
		return NULL;
	}

	tmp = malloc(sizeof(struct opbx_channel));
	if (!tmp) {
		opbx_log(LOG_ERROR, "Channel allocation failed: Out of memory\n");
		return NULL;
	}

	memset(tmp, 0, sizeof(struct opbx_channel));
	tmp->sched = sched_manual_context_create();
	if (!tmp->sched) {
		opbx_log(LOG_ERROR, "Channel allocation failed: Unable to create schedule context\n");
		free(tmp);
		return NULL;
	}
	
	for (x=0; x<OPBX_MAX_FDS - 1; x++)
		tmp->fds[x] = -1;

	if (needqueue) {
		if (pipe(tmp->alertpipe)) {
			opbx_log(LOG_WARNING, "Channel allocation failed: Can't create alert pipe!\n");
			free(tmp);
			return NULL;
		} else {
			flags = fcntl(tmp->alertpipe[0], F_GETFL);
			fcntl(tmp->alertpipe[0], F_SETFL, flags | O_NONBLOCK);
			flags = fcntl(tmp->alertpipe[1], F_GETFL);
			fcntl(tmp->alertpipe[1], F_SETFL, flags | O_NONBLOCK);
		}
	} else 
		/* Make sure we've got it done right if they don't */
		tmp->alertpipe[0] = tmp->alertpipe[1] = -1;

	/* Init channel generator data struct lock */
	opbx_mutex_init(&tmp->gcd.lock);

	/* Always watch the alertpipe */
	tmp->fds[OPBX_MAX_FDS-1] = tmp->alertpipe[0];
	strcpy(tmp->name, "**Unknown**");
	/* Initial state */
	tmp->_state = OPBX_STATE_DOWN;
	tmp->appl = NULL;
	tmp->data = NULL;
	tmp->fin = global_fin;
	tmp->fout = global_fout;
	snprintf(tmp->uniqueid, sizeof(tmp->uniqueid), "%li.%d", (long) time(NULL), uniqueint++);
	headp = &tmp->varshead;
	opbx_mutex_init(&tmp->lock);
	OPBX_LIST_HEAD_INIT_NOLOCK(headp);
	strcpy(tmp->context, "default");
	opbx_copy_string(tmp->language, defaultlanguage, sizeof(tmp->language));
	strcpy(tmp->exten, "s");
	tmp->priority = 1;
	tmp->amaflags = opbx_default_amaflags;
	opbx_copy_string(tmp->accountcode, opbx_default_accountcode, sizeof(tmp->accountcode));

	tmp->tech = &null_tech;
	tmp->t38mode_enabled=0;

	opbx_mutex_lock(&chlock);
	tmp->next = channels;
	channels = tmp;

	opbx_mutex_unlock(&chlock);
	return tmp;
}

/*--- opbx_queue_frame: Queue an outgoing media frame */
int opbx_queue_frame(struct opbx_channel *chan, struct opbx_frame *fin)
{
	struct opbx_frame *f;
	struct opbx_frame *prev, *cur;
	int blah = 1;
	int qlen = 0;

	/* Build us a copy and free the original one */
	f = opbx_frdup(fin);
	if (!f) {
		opbx_log(LOG_WARNING, "Unable to duplicate frame\n");
		return -1;
	}
	opbx_mutex_lock(&chan->lock);
	prev = NULL;
	cur = chan->readq;
	while(cur) {
		if ((cur->frametype == OPBX_FRAME_CONTROL) && (cur->subclass == OPBX_CONTROL_HANGUP)) {
			/* Don't bother actually queueing anything after a hangup */
			opbx_frfree(f);
			opbx_mutex_unlock(&chan->lock);
			return 0;
		}
		prev = cur;
		cur = cur->next;
		qlen++;
	}
	/* Allow up to 96 voice frames outstanding, and up to 128 total frames */
	if (((fin->frametype == OPBX_FRAME_VOICE) && (qlen > 96)) || (qlen  > 128)) {
		if (fin->frametype != OPBX_FRAME_VOICE) {
			opbx_log(LOG_WARNING, "Exceptionally long queue length queuing to %s\n", chan->name);
			CRASH;
		} else {
			opbx_log(LOG_DEBUG, "Dropping voice to exceptionally long queue on %s\n", chan->name);
			opbx_frfree(f);
			opbx_mutex_unlock(&chan->lock);
			return 0;
		}
	}
	if (prev)
		prev->next = f;
	else
		chan->readq = f;
	if (chan->alertpipe[1] > -1) {
		if (write(chan->alertpipe[1], &blah, sizeof(blah)) != sizeof(blah))
			opbx_log(LOG_WARNING, "Unable to write to alert pipe on %s, frametype/subclass %d/%d (qlen = %d): %s!\n",
				chan->name, f->frametype, f->subclass, qlen, strerror(errno));
	} else if (opbx_test_flag(chan, OPBX_FLAG_BLOCKING)) {
		pthread_kill(chan->blocker, SIGURG);
	}
	opbx_mutex_unlock(&chan->lock);
	return 0;
}

/*--- opbx_queue_hangup: Queue a hangup frame for channel */
int opbx_queue_hangup(struct opbx_channel *chan)
{
	struct opbx_frame f = { OPBX_FRAME_CONTROL, OPBX_CONTROL_HANGUP };
	chan->_softhangup |= OPBX_SOFTHANGUP_DEV;
	return opbx_queue_frame(chan, &f);
}

/*--- opbx_queue_control: Queue a control frame */
int opbx_queue_control(struct opbx_channel *chan, int control)
{
	struct opbx_frame f = { OPBX_FRAME_CONTROL, };
	f.subclass = control;
	return opbx_queue_frame(chan, &f);
}

/*--- opbx_channel_defer_dtmf: Set defer DTMF flag on channel */
int opbx_channel_defer_dtmf(struct opbx_channel *chan)
{
	int pre = 0;

	if (chan) {
		pre = opbx_test_flag(chan, OPBX_FLAG_DEFER_DTMF);
		opbx_set_flag(chan, OPBX_FLAG_DEFER_DTMF);
	}
	return pre;
}

/*--- opbx_channel_undefer_dtmf: Unset defer DTMF flag on channel */
void opbx_channel_undefer_dtmf(struct opbx_channel *chan)
{
	if (chan)
		opbx_clear_flag(chan, OPBX_FLAG_DEFER_DTMF);
}

/*
 * Helper function to find channels. It supports these modes:
 *
 * prev != NULL : get channel next in list after prev
 * name != NULL : get channel with matching name
 * name != NULL && namelen != 0 : get channel whose name starts with prefix
 * exten != NULL : get channel whose exten or macroexten matches
 * context != NULL && exten != NULL : get channel whose context or macrocontext
 *                                    
 * It returns with the channel's lock held. If getting the individual lock fails,
 * unlock and retry quickly up to 10 times, then give up.
 * 
 * XXX Note that this code has cost O(N) because of the need to verify
 * that the object is still on the global list.
 *
 * XXX also note that accessing fields (e.g. c->name in opbx_log())
 * can only be done with the lock held or someone could delete the
 * object while we work on it. This causes some ugliness in the code.
 * Note that removing the first opbx_log() may be harmful, as it would
 * shorten the retry period and possibly cause failures.
 * We should definitely go for a better scheme that is deadlock-free.
 */
static struct opbx_channel *channel_find_locked(const struct opbx_channel *prev,
					       const char *name, const int namelen,
					       const char *context, const char *exten)
{
	const char *msg = prev ? "deadlock" : "initial deadlock";
	int retries, done;
	struct opbx_channel *c;

	for (retries = 0; retries < 10; retries++) {
		opbx_mutex_lock(&chlock);
		for (c = channels; c; c = c->next) {
			if (!prev) {
				/* want head of list */
				if (!name && !exten)
					break;
				if (name) {
					/* want match by full name */
					if (!namelen) {
						if (!strcasecmp(c->name, name))
							break;
						else
							continue;
					}
					/* want match by name prefix */
					if (!strncasecmp(c->name, name, namelen))
						break;
				} else if (exten) {
					/* want match by context and exten */
					if (context && (strcasecmp(c->context, context) &&
							strcasecmp(c->macrocontext, context)))
						continue;
					/* match by exten */
					if (strcasecmp(c->exten, exten) &&
					    strcasecmp(c->macroexten, exten))
						continue;
					else
						break;
				}
			} else if (c == prev) { /* found, return c->next */
				c = c->next;
				break;
			}
		}
		/* exit if chan not found or mutex acquired successfully */
		done = (c == NULL) || (opbx_mutex_trylock(&c->lock) == 0);
		/* this is slightly unsafe, as we _should_ hold the lock to access c->name */
		if (!done && c)
			opbx_log(LOG_DEBUG, "Avoiding %s for '%s'\n", msg, c->name);
		opbx_mutex_unlock(&chlock);
		if (done)
			return c;
		usleep(1);
	}
	/*
 	 * c is surely not null, but we don't have the lock so cannot
	 * access c->name
	 */
	opbx_log(LOG_WARNING, "Avoided %s for '%p', %d retries!\n",
		msg, c, retries);

	return NULL;
}

/*--- opbx_channel_walk_locked: Browse channels in use */
struct opbx_channel *opbx_channel_walk_locked(const struct opbx_channel *prev)
{
	return channel_find_locked(prev, NULL, 0, NULL, NULL);
}

/*--- opbx_get_channel_by_name_locked: Get channel by name and lock it */
struct opbx_channel *opbx_get_channel_by_name_locked(const char *name)
{
	return channel_find_locked(NULL, name, 0, NULL, NULL);
}

/*--- opbx_get_channel_by_name_prefix_locked: Get channel by name prefix and lock it */
struct opbx_channel *opbx_get_channel_by_name_prefix_locked(const char *name, const int namelen)
{
	return channel_find_locked(NULL, name, namelen, NULL, NULL);
}

/*--- opbx_walk_channel_by_name_prefix_locked: Get next channel by name prefix and lock it */
struct opbx_channel *opbx_walk_channel_by_name_prefix_locked(struct opbx_channel *chan, const char *name, const int namelen)
{
	return channel_find_locked(chan, name, namelen, NULL, NULL);
}

/*--- opbx_get_channel_by_exten_locked: Get channel by exten (and optionally context) and lock it */
struct opbx_channel *opbx_get_channel_by_exten_locked(const char *exten, const char *context)
{
	return channel_find_locked(NULL, NULL, 0, context, exten);
}

/*--- opbx_safe_sleep_conditional: Wait, look for hangups and condition arg */
int opbx_safe_sleep_conditional(	struct opbx_channel *chan, int ms,
	int (*cond)(void*), void *data )
{
	struct opbx_frame *f;

	while(ms > 0) {
		if( cond && ((*cond)(data) == 0 ) )
			return 0;
		ms = opbx_waitfor(chan, ms);
		if (ms <0)
			return -1;
		if (ms > 0) {
			f = opbx_read(chan);
			if (!f)
				return -1;
			opbx_frfree(f);
		}
	}
	return 0;
}

/*--- opbx_safe_sleep: Wait, look for hangups */
int opbx_safe_sleep(struct opbx_channel *chan, int ms)
{
	struct opbx_frame *f;
	while(ms > 0) {
		ms = opbx_waitfor(chan, ms);
		if (ms < 0)
			return -1;
		if (ms > 0) {
			f = opbx_read(chan);
			if (!f)
				return -1;
			opbx_frfree(f);
		}
	}
	return 0;
}

static void free_cid(struct opbx_callerid *cid)
{
	if (cid->cid_dnid)
		free(cid->cid_dnid);
	if (cid->cid_num)
		free(cid->cid_num);	
	if (cid->cid_name)
		free(cid->cid_name);	
	if (cid->cid_ani)
		free(cid->cid_ani);
	if (cid->cid_rdnis)
		free(cid->cid_rdnis);
}

/*--- opbx_channel_free: Free a channel structure */
void opbx_channel_free(struct opbx_channel *chan)
{
	struct opbx_channel *last=NULL, *cur;
	int fd;
	struct opbx_var_t *vardata;
	struct opbx_frame *f, *fp;
	struct varshead *headp;
	char name[OPBX_CHANNEL_NAME];
	
	headp=&chan->varshead;
	
	opbx_mutex_lock(&chlock);
	cur = channels;
	while(cur) {
		if (cur == chan) {
			if (last)
				last->next = cur->next;
			else
				channels = cur->next;
			break;
		}
		last = cur;
		cur = cur->next;
	}
	if (!cur)
		opbx_log(LOG_WARNING, "Unable to find channel in list\n");
	else {
		/* Lock and unlock the channel just to be sure nobody
		   has it locked still */
		opbx_mutex_lock(&cur->lock);
		opbx_mutex_unlock(&cur->lock);
	}
	if (chan->tech_pvt)
    {
		opbx_log(LOG_WARNING, "Channel '%s' may not have been hung up properly\n", chan->name);
		free(chan->tech_pvt);
	}

	opbx_copy_string(name, chan->name, sizeof(name));

	/* Stop generator thread */
	opbx_generator_stop_thread(chan);

	/* Stop monitoring */
	if (chan->monitor)
		chan->monitor->stop(chan, 0);

	/* If there is native format music-on-hold state, free it */
	if (chan->music_state)
		opbx_moh_cleanup(chan);

	/* Free translators */
	if (chan->readtrans)
		opbx_translator_free_path(chan->readtrans);
	if (chan->writetrans)
		opbx_translator_free_path(chan->writetrans);
	if (chan->pbx) 
		opbx_log(LOG_WARNING, "PBX may not have been terminated properly on '%s'\n", chan->name);
	free_cid(&chan->cid);
	opbx_mutex_destroy(&chan->lock);
	/* Close pipes if appropriate */
	if ((fd = chan->alertpipe[0]) > -1)
		close(fd);
	if ((fd = chan->alertpipe[1]) > -1)
		close(fd);
	f = chan->readq;
	chan->readq = NULL;
	while (f)
    {
		fp = f;
		f = f->next;
		opbx_frfree(fp);
	}
	
	/* loop over the variables list, freeing all data and deleting list items */
	/* no need to lock the list, as the channel is already locked */
	
	while (!OPBX_LIST_EMPTY(headp))
    {
        /* List Deletion. */
	    vardata = OPBX_LIST_REMOVE_HEAD(headp, entries);
	    opbx_var_delete(vardata);
	}

	/* Destroy the jitterbuffer */
	opbx_jb_destroy(chan);

	free(chan);
	opbx_mutex_unlock(&chlock);

	opbx_device_state_changed_literal(name);
}

static void opbx_spy_detach(struct opbx_channel *chan) 
{
	struct opbx_channel_spy *chanspy;

	/* Marking the spies as done is sufficient.  Chanspy or spy users will get the picture. */
	for (chanspy = chan->spiers;  chanspy;  chanspy = chanspy->next)
    {
		if (chanspy->status == CHANSPY_RUNNING)
			chanspy->status = CHANSPY_DONE;
	}
	chan->spiers = NULL;
}

/*--- opbx_softhangup_nolock: Softly hangup a channel, don't lock */
int opbx_softhangup_nolock(struct opbx_channel *chan, int cause)
{
	int res = 0;
	struct opbx_frame f = { OPBX_FRAME_NULL };

	if (option_debug)
		opbx_log(LOG_DEBUG, "Soft-Hanging up channel '%s'\n", chan->name);
	/* Inform channel driver that we need to be hung up, if it cares */
	chan->_softhangup |= cause;
	opbx_queue_frame(chan, &f);
	/* Interrupt any poll call or such */
	if (opbx_test_flag(chan, OPBX_FLAG_BLOCKING))
		pthread_kill(chan->blocker, SIGURG);
	return res;
}

/*--- opbx_softhangup_nolock: Softly hangup a channel, lock */
int opbx_softhangup(struct opbx_channel *chan, int cause)
{
	int res;

	opbx_mutex_lock(&chan->lock);
	res = opbx_softhangup_nolock(chan, cause);
	opbx_mutex_unlock(&chan->lock);
	return res;
}

static void opbx_queue_spy_frame(struct opbx_channel_spy *spy, struct opbx_frame *f, int pos) 
{
	struct opbx_frame *tmpf = NULL;
	int count = 0;

	opbx_mutex_lock(&spy->lock);
	for (tmpf = spy->queue[pos];  tmpf  &&  tmpf->next;  tmpf = tmpf->next)
		count++;

	if (count > 1000)
    {
		struct opbx_frame *freef, *headf;

		opbx_log(LOG_ERROR, "Too many frames queued at once, flushing cache.\n");
		headf = spy->queue[pos];
		/* deref the queue right away so it looks empty */
		spy->queue[pos] = NULL;
		tmpf = headf;
		/* Free the wasted frames */
		while (tmpf)
        {
			freef = tmpf;
			tmpf = tmpf->next;
			opbx_frfree(freef);
		}
		opbx_mutex_unlock(&spy->lock);
		return;
	}

	if (tmpf)
		tmpf->next = opbx_frdup(f);
	else
		spy->queue[pos] = opbx_frdup(f);
	opbx_mutex_unlock(&spy->lock);
}

static void free_translation(struct opbx_channel *clone)
{
	if (clone->writetrans)
		opbx_translator_free_path(clone->writetrans);
	if (clone->readtrans)
		opbx_translator_free_path(clone->readtrans);
	clone->writetrans = NULL;
	clone->readtrans = NULL;
	clone->rawwriteformat = clone->nativeformats;
	clone->rawreadformat = clone->nativeformats;
}

/*--- opbx_hangup: Hangup a channel */
int opbx_hangup(struct opbx_channel *chan)
{
	int res = 0;

	/* Don't actually hang up a channel that will masquerade as someone else, or
	   if someone is going to masquerade as us */
	opbx_mutex_lock(&chan->lock);

	opbx_spy_detach(chan);		/* get rid of spies */

	if (chan->masq)
    {
		if (opbx_do_masquerade(chan)) 
			opbx_log(LOG_WARNING, "Failed to perform masquerade\n");
	}

	if (chan->masq)
    {
		opbx_log(LOG_WARNING, "%s getting hung up, but someone is trying to masq into us?!?\n", chan->name);
		opbx_mutex_unlock(&chan->lock);
		return 0;
	}
	/* If this channel is one which will be masqueraded into something, 
	   mark it as a zombie already, so we know to free it later */
	if (chan->masqr)
    {
		opbx_set_flag(chan, OPBX_FLAG_ZOMBIE);
		opbx_mutex_unlock(&chan->lock);
		return 0;
	}
	free_translation(chan);
	if (chan->stream) 		/* Close audio stream */
		opbx_closestream(chan->stream);
	if (chan->vstream)		/* Close video stream */
		opbx_closestream(chan->vstream);
	if (chan->sched)
		sched_context_destroy(chan->sched);
	
	if (chan->cdr)
    {
        /* End the CDR if it hasn't already */ 
		opbx_cdr_end(chan->cdr);
		opbx_cdr_detach(chan->cdr);	/* Post and Free the CDR */ 
		chan->cdr = NULL;
	}
	if (opbx_test_flag(chan, OPBX_FLAG_BLOCKING))
    {
		opbx_log(LOG_WARNING, "Hard hangup called by thread %ld on %s, while fd "
					"is blocked by thread %ld in procedure %s!  Expect a failure\n",
					(long)pthread_self(), chan->name, (long)chan->blocker, chan->blockproc);
		CRASH;
	}
	if (!opbx_test_flag(chan, OPBX_FLAG_ZOMBIE))
    {
		if (option_debug)
			opbx_log(LOG_DEBUG, "Hanging up channel '%s'\n", chan->name);
		if (chan->tech->hangup)
			res = chan->tech->hangup(chan);
	}
    else
    {
		if (option_debug)
			opbx_log(LOG_DEBUG, "Hanging up zombie '%s'\n", chan->name);
	}
			
	opbx_mutex_unlock(&chan->lock);
	
	/** opbx_generator_deactivate after mutex_unlock*/
	if (option_debug)
	    opbx_log(LOG_DEBUG, "Generator : deactivate after channel unlock (hangup function)\n");
	opbx_generator_deactivate(chan);

	manager_event(EVENT_FLAG_CALL, "Hangup", 
			"Channel: %s\r\n"
			"Uniqueid: %s\r\n"
			"Cause: %d\r\n"
			"Cause-txt: %s\r\n",
			chan->name, 
			chan->uniqueid, 
			chan->hangupcause,
			opbx_cause2str(chan->hangupcause)
			);
	opbx_channel_free(chan);
	return res;
}

int opbx_answer(struct opbx_channel *chan)
{
	int res = 0;

	opbx_mutex_lock(&chan->lock);
	/* Stop if we're a zombie or need a soft hangup */
	if (opbx_test_flag(chan, OPBX_FLAG_ZOMBIE) || opbx_check_hangup(chan))
    {
		opbx_mutex_unlock(&chan->lock);
		return -1;
	}
	switch (chan->_state)
    {
	case OPBX_STATE_RINGING:
	case OPBX_STATE_RING:
		if (chan->tech->answer)
			res = chan->tech->answer(chan);
		opbx_setstate(chan, OPBX_STATE_UP);
		if (chan->cdr)
			opbx_cdr_answer(chan->cdr);
		opbx_mutex_unlock(&chan->lock);
		return res;
		break;
	case OPBX_STATE_UP:
		if (chan->cdr)
			opbx_cdr_answer(chan->cdr);
		break;
	}
	opbx_mutex_unlock(&chan->lock);
	return 0;
}

/*--- opbx_waitfor_n_fd: Wait for x amount of time on a file descriptor to have input.  */
int opbx_waitfor_n_fd(int *fds, int n, int *ms, int *exception)
{
	struct timeval start = { 0 , 0 };
	int res;
	int x, y;
	int winner = -1;
	int spoint;
	struct pollfd *pfds;
	
	if ((pfds = alloca(sizeof(struct pollfd) * n)) == NULL)
    {
		opbx_log(LOG_ERROR, "Out of memory\n");
		return -1;
	}
	if (*ms > 0)
		start = opbx_tvnow();
	y = 0;
	for (x = 0;  x < n;  x++)
    {
		if (fds[x] > -1)
        {
			pfds[y].fd = fds[x];
			pfds[y].events = POLLIN | POLLPRI;
			y++;
		}
	}
	res = poll(pfds, y, *ms);
	if (res < 0)
    {
		/* Simulate a timeout if we were interrupted */
		*ms = (errno != EINTR)  ?  -1  :  0;
		return -1;
	}
	spoint = 0;
	for (x = 0;  x < n;  x++)
    {
		if (fds[x] > -1)
        {
			if ((res = opbx_fdisset(pfds, fds[x], y, &spoint)))
            {
				winner = fds[x];
				if (exception)
					*exception = (res & POLLPRI)?  -1  :  0;
			}
		}
	}
	if (*ms > 0)
    {
		*ms -= opbx_tvdiff_ms(opbx_tvnow(), start);
		if (*ms < 0)
			*ms = 0;
	}
	return winner;
}

/*--- opbx_waitfor_nanfds: Wait for x amount of time on a file descriptor to have input.  */
struct opbx_channel *opbx_waitfor_nandfds(struct opbx_channel **c, int n, int *fds, int nfds, 
	int *exception, int *outfd, int *ms)
{
	struct timeval start = { 0 , 0 };
	struct pollfd *pfds;
	int res;
	long rms;
	int x, y, max;
	int spoint;
	time_t now = 0;
	long whentohangup = 0, havewhen = 0, diff;
	struct opbx_channel *winner = NULL;

	if ((pfds = alloca(sizeof(struct pollfd) * (n * OPBX_MAX_FDS + nfds))) == NULL)
    {
        opbx_log(LOG_ERROR, "Out of memory\n");
		*outfd = -1;
		return NULL;
	}

	if (outfd)
		*outfd = -99999;
	if (exception)
		*exception = 0;
	
	/* Perform any pending masquerades */
	for (x = 0;  x < n;  x++)
    {
		opbx_mutex_lock(&c[x]->lock);
		if (c[x]->whentohangup)
        {
			if (!havewhen)
				time(&now);
			diff = c[x]->whentohangup - now;
			if (!havewhen || (diff < whentohangup))
            {
				havewhen++;
				whentohangup = diff;
			}
		}
		if (c[x]->masq)
        {
			if (opbx_do_masquerade(c[x]))
            {
				opbx_log(LOG_WARNING, "Masquerade failed\n");
				*ms = -1;
				opbx_mutex_unlock(&c[x]->lock);
				return NULL;
			}
		}
		opbx_mutex_unlock(&c[x]->lock);
	}

	rms = *ms;
	
	if (havewhen)
    {
		if ((*ms < 0) || (whentohangup * 1000 < *ms))
			rms = whentohangup * 1000;
	}
	max = 0;
	for (x = 0;  x < n;  x++)
    {
		for (y = 0;  y < OPBX_MAX_FDS;  y++)
        {
			if (c[x]->fds[y] > -1)
            {
				pfds[max].fd = c[x]->fds[y];
				pfds[max].events = POLLIN | POLLPRI;
				pfds[max].revents = 0;
				max++;
			}
		}
		CHECK_BLOCKING(c[x]);
	}
	for (x = 0;  x < nfds;  x++)
    {
		if (fds[x] > -1)
        {
			pfds[max].fd = fds[x];
			pfds[max].events = POLLIN | POLLPRI;
			pfds[max].revents = 0;
			max++;
		}
	}
	if (*ms > 0) 
		start = opbx_tvnow();
	
	if (sizeof(int) == 4)
    {
		do
        {
			int kbrms = rms;
			if (kbrms > 600000)
				kbrms = 600000;
			res = poll(pfds, max, kbrms);
			if (!res)
				rms -= kbrms;
		}
        while (!res && (rms > 0));
	}
    else
    {
		res = poll(pfds, max, rms);
	}
	
	if (res < 0)
    {
		for (x = 0;  x < n;  x++) 
			opbx_clear_flag(c[x], OPBX_FLAG_BLOCKING);
		/* Simulate a timeout if we were interrupted */
		if (errno != EINTR)
			*ms = -1;
		else
        {
			/* Just an interrupt */
#if 0
			*ms = 0;
#endif			
		}
		return NULL;
    }
    else
    {
       	/* If no fds signalled, then timeout. So set ms = 0
		   since we may not have an exact timeout. */
		if (res == 0)
			*ms = 0;
	}

	if (havewhen)
		time(&now);
	spoint = 0;
	for (x = 0;  x < n;  x++)
    {
		opbx_clear_flag(c[x], OPBX_FLAG_BLOCKING);
		if (havewhen && c[x]->whentohangup && (now > c[x]->whentohangup))
        {
			c[x]->_softhangup |= OPBX_SOFTHANGUP_TIMEOUT;
			if (!winner)
				winner = c[x];
		}
		for (y = 0;  y < OPBX_MAX_FDS;  y++)
        {
			if (c[x]->fds[y] > -1)
            {
				if ((res = opbx_fdisset(pfds, c[x]->fds[y], max, &spoint)))
                {
					if (res & POLLPRI)
						opbx_set_flag(c[x], OPBX_FLAG_EXCEPTION);
					else
						opbx_clear_flag(c[x], OPBX_FLAG_EXCEPTION);
					c[x]->fdno = y;
					winner = c[x];
				}
			}
		}
	}
	for (x = 0;  x < nfds;  x++)
    {
		if (fds[x] > -1)
        {
			if ((res = opbx_fdisset(pfds, fds[x], max, &spoint)))
            {
				if (outfd)
					*outfd = fds[x];
				if (exception)
                {	
					if (res & POLLPRI) 
						*exception = -1;
					else
						*exception = 0;
				}
				winner = NULL;
			}
		}	
	}
	if (*ms > 0)
    {
		*ms -= opbx_tvdiff_ms(opbx_tvnow(), start);
		if (*ms < 0)
			*ms = 0;
	}
	return winner;
}

struct opbx_channel *opbx_waitfor_n(struct opbx_channel **c, int n, int *ms)
{
	return opbx_waitfor_nandfds(c, n, NULL, 0, NULL, NULL, ms);
}

int opbx_waitfor(struct opbx_channel *c, int ms)
{
	struct opbx_channel *chan;
	int oldms = ms;

	chan = opbx_waitfor_n(&c, 1, &ms);
	if (ms < 0)
    {
		if (oldms < 0)
			return 0;
		else
			return -1;
	}
	return ms;
}

int opbx_waitfordigit(struct opbx_channel *c, int ms)
{
	/* XXX Should I be merged with waitfordigit_full XXX */
	struct opbx_frame *f;
	int result = 0;

	/* Stop if we're a zombie or need a soft hangup */
	if (opbx_test_flag(c, OPBX_FLAG_ZOMBIE) || opbx_check_hangup(c)) 
		return -1;

	/* Wait for a digit, no more than ms milliseconds total. */
	while (ms && !result)
    {
		ms = opbx_waitfor(c, ms);
		if (ms < 0) /* Error */
			result = -1; 
		else if (ms > 0)
        {
			/* Read something */
			f = opbx_read(c);
			if (f)
            {
				if (f->frametype == OPBX_FRAME_DTMF) 
					result = f->subclass;
				opbx_frfree(f);
			}
            else
            {
				result = -1;
            }
		}
	}
	return result;
}

int opbx_waitfordigit_full(struct opbx_channel *c, int ms, int audiofd, int cmdfd)
{
	struct opbx_frame *f;
	struct opbx_channel *rchan;
	int outfd;
	int res;

	/* Stop if we're a zombie or need a soft hangup */
	if (opbx_test_flag(c, OPBX_FLAG_ZOMBIE) || opbx_check_hangup(c)) 
		return -1;
	/* Wait for a digit, no more than ms milliseconds total. */
	while (ms)
    {
		errno = 0;
		rchan = opbx_waitfor_nandfds(&c, 1, &cmdfd, (cmdfd > -1) ? 1 : 0, NULL, &outfd, &ms);
		if ((!rchan) && (outfd < 0) && (ms))
        { 
			if (errno == 0 || errno == EINTR)
				continue;
			opbx_log(LOG_WARNING, "Wait failed (%s)\n", strerror(errno));
			return -1;
		}
        else if (outfd > -1)
        {
			/* The FD we were watching has something waiting */
			return 1;
		}
        else if (rchan)
        {
			if ((f = opbx_read(c)) == 0)
				return -1;
			switch (f->frametype)
            {
			case OPBX_FRAME_DTMF:
				res = f->subclass;
				opbx_frfree(f);
				return res;
			case OPBX_FRAME_CONTROL:
				switch(f->subclass)
                {
				case OPBX_CONTROL_HANGUP:
					opbx_frfree(f);
					return -1;
				case OPBX_CONTROL_RINGING:
				case OPBX_CONTROL_ANSWER:
					/* Unimportant */
					break;
				default:
					opbx_log(LOG_WARNING, "Unexpected control subclass '%d'\n", f->subclass);
				}
			case OPBX_FRAME_VOICE:
				/* Write audio if appropriate */
				if (audiofd > -1)
					write(audiofd, f->data, f->datalen);
			}
			/* Ignore */
			opbx_frfree(f);
		}
	}
	return 0; /* Time is up */
}

struct opbx_frame *opbx_read(struct opbx_channel *chan)
{
	struct opbx_frame *f = NULL;
	int blah;
	int prestate;
	static struct opbx_frame null_frame =
    {
		OPBX_FRAME_NULL,
	};
	
	opbx_mutex_lock(&chan->lock);
	if (chan->masq)
    {
		if (opbx_do_masquerade(chan))
        {
			opbx_log(LOG_WARNING, "Failed to perform masquerade\n");
			f = NULL;
		}
        else
        {
			f =  &null_frame;
		}
        opbx_mutex_unlock(&chan->lock);
		return f;
	}

	/* Stop if we're a zombie or need a soft hangup */
	if (opbx_test_flag(chan, OPBX_FLAG_ZOMBIE)  ||  opbx_check_hangup(chan))
    {
		opbx_mutex_unlock(&chan->lock);
		opbx_generator_deactivate(chan);
		return NULL;
	}
	prestate = chan->_state;

	if (!opbx_test_flag(chan, OPBX_FLAG_DEFER_DTMF) && !opbx_strlen_zero(chan->dtmfq))
    {
		/* We have DTMF that has been deferred.  Return it now */
		chan->dtmff.frametype = OPBX_FRAME_DTMF;
		chan->dtmff.subclass = chan->dtmfq[0];
		/* Drop first digit */
		memmove(chan->dtmfq, chan->dtmfq + 1, sizeof(chan->dtmfq) - 1);
		opbx_mutex_unlock(&chan->lock);
		return &chan->dtmff;
	}
	
	/* Read and ignore anything on the alertpipe, but read only
	   one sizeof(blah) per frame that we send from it */
	if (chan->alertpipe[0] > -1)
		read(chan->alertpipe[0], &blah, sizeof(blah));

	/* Check for pending read queue */
	if (chan->readq)
    {
		f = chan->readq;
		chan->readq = f->next;
		/* Interpret hangup and return NULL */
		if ((f->frametype == OPBX_FRAME_CONTROL) && (f->subclass == OPBX_CONTROL_HANGUP))
        {
			opbx_frfree(f);
			f = NULL;
		}
	}
    else
    {
		chan->blocker = pthread_self();
		if (opbx_test_flag(chan, OPBX_FLAG_EXCEPTION))
        {
			if (chan->tech->exception) 
			{
            	f = chan->tech->exception(chan);
			}
            else
            {
				opbx_log(LOG_WARNING, "Exception flag set on '%s', but no exception handler\n", chan->name);
				f = &null_frame;
			}
			/* Clear the exception flag */
			opbx_clear_flag(chan, OPBX_FLAG_EXCEPTION);
		} else {
			if (chan->tech->read)
				f = chan->tech->read(chan);
			else
				opbx_log(LOG_WARNING, "No read routine on channel %s\n", chan->name);
		}
	}


	if (f  &&  (f->frametype == OPBX_FRAME_VOICE))
    {
		if (!(f->subclass & chan->nativeformats))
        {
			/* This frame can't be from the current native formats -- drop it on the
			   floor */
			opbx_log(LOG_NOTICE, "Dropping incompatible voice frame on %s of format %s since our native format has changed to %s\n", chan->name, opbx_getformatname(f->subclass), opbx_getformatname(chan->nativeformats));
			opbx_frfree(f);
			f = &null_frame;
		}
        else
        {
			if (chan->spiers)
            {
				struct opbx_channel_spy *spying;

				for (spying = chan->spiers;  spying;  spying = spying->next)
					opbx_queue_spy_frame(spying, f, 0);
			}
			if (chan->monitor && chan->monitor->read_stream)
            {
#ifndef MONITOR_CONSTANT_DELAY
				int jump = chan->outsmpl - chan->insmpl - 2 * f->samples;

				if (jump >= 0)
                {
					if (opbx_seekstream(chan->monitor->read_stream, jump + f->samples, SEEK_FORCECUR) == -1)
						opbx_log(LOG_WARNING, "Failed to perform seek in monitoring read stream, synchronization between the files may be broken\n");
					chan->insmpl += jump + 2 * f->samples;
				}
                else
                {
					chan->insmpl+= f->samples;
                }
#else
				int jump = chan->outsmpl - chan->insmpl;

				if (jump - MONITOR_DELAY >= 0)
                {
					if (opbx_seekstream(chan->monitor->read_stream, jump - f->samples, SEEK_FORCECUR) == -1)
						opbx_log(LOG_WARNING, "Failed to perform seek in monitoring read stream, synchronization between the files may be broken\n");
					chan->insmpl += jump;
				}
                else
                {
					chan->insmpl += f->samples;
                }
#endif
				if (opbx_writestream(chan->monitor->read_stream, f) < 0)
					opbx_log(LOG_WARNING, "Failed to write data to channel monitor read stream\n");
			}
			if (chan->readtrans)
            {
				if ((f = opbx_translate(chan->readtrans, f, 1)) == NULL)
					f = &null_frame;
			}
		}
	}

	/* Make sure we always return NULL in the future */
	if (!f)
    {
		chan->_softhangup |= OPBX_SOFTHANGUP_DEV;
		opbx_generator_deactivate(chan);
		/* End the CDR if appropriate */
		if (chan->cdr)
			opbx_cdr_end(chan->cdr);
	}
    else if (opbx_test_flag(chan, OPBX_FLAG_DEFER_DTMF) && f->frametype == OPBX_FRAME_DTMF)
    {
		if (strlen(chan->dtmfq) < sizeof(chan->dtmfq) - 2)
			chan->dtmfq[strlen(chan->dtmfq)] = f->subclass;
		else
			opbx_log(LOG_WARNING, "Dropping deferred DTMF digits on %s\n", chan->name);
		f = &null_frame;
	}
    else if ((f->frametype == OPBX_FRAME_CONTROL) && (f->subclass == OPBX_CONTROL_ANSWER))
    {
		if (prestate == OPBX_STATE_UP)
        {
			opbx_log(LOG_DEBUG, "Dropping duplicate answer!\n");
			f = &null_frame;
		}
		/* Answer the CDR */
		opbx_setstate(chan, OPBX_STATE_UP);
		opbx_cdr_answer(chan->cdr);
    }

	/* High bit prints debugging */
	if (chan->fin & 0x80000000)
		opbx_frame_dump(chan->name, f, "<<");
	if ((chan->fin & 0x7fffffff) == 0x7fffffff)
		chan->fin &= 0x80000000;
	else
		chan->fin++;
		
	
	opbx_mutex_unlock(&chan->lock);
	/** generator deactivate after channel unlock */
	if (f == NULL  &&  opbx_generator_is_active(chan))
    {
	    if (option_debug)
	    	opbx_log(LOG_DEBUG, "Generator not finished in previous deactivate attempt - trying deactivate after channel unlock (opbx_read function)\n");
	    opbx_generator_deactivate(chan);
	}	
	
	return f;
}

int opbx_indicate(struct opbx_channel *chan, int condition)
{
	int res = -1;

	/* Stop if we're a zombie or need a soft hangup */
	if (opbx_test_flag(chan, OPBX_FLAG_ZOMBIE) || opbx_check_hangup(chan)) 
		return -1;
	opbx_mutex_lock(&chan->lock);
	if (chan->tech->indicate)
		res = chan->tech->indicate(chan, condition);
	opbx_mutex_unlock(&chan->lock);
	if (!chan->tech->indicate  ||  res)
    {
		/*
		 * Device does not support (that) indication, lets fake
		 * it by doing our own tone generation. (PM2002)
		 */
		if (condition >= 0)
        {
			const struct tone_zone_sound *ts = NULL;
			switch (condition)
            {
			case OPBX_CONTROL_RINGING:
				ts = opbx_get_indication_tone(chan->zone, "ring");
				break;
			case OPBX_CONTROL_BUSY:
				ts = opbx_get_indication_tone(chan->zone, "busy");
				break;
			case OPBX_CONTROL_CONGESTION:
				ts = opbx_get_indication_tone(chan->zone, "congestion");
				break;
			}
			if (ts  &&  ts->data[0])
            {
				opbx_log(LOG_DEBUG, "Driver for channel '%s' does not support indication %d, emulating it\n", chan->name, condition);
				opbx_playtones_start(chan,0,ts->data, 1);
				res = 0;
			}
            else if (condition == OPBX_CONTROL_PROGRESS)
            {
				/* opbx_playtones_stop(chan); */
			}
            else if (condition == OPBX_CONTROL_PROCEEDING)
            {
				/* Do nothing, really */
			}
            else if (condition == OPBX_CONTROL_HOLD)
            {
				/* Do nothing.... */
			}
            else if (condition == OPBX_CONTROL_UNHOLD)
            {
				/* Do nothing.... */
			}
            else if (condition == OPBX_CONTROL_VIDUPDATE)
            {
				/* Do nothing.... */
			}
            else
            {
				/* not handled */
				opbx_log(LOG_WARNING, "Unable to handle indication %d for '%s'\n", condition, chan->name);
				res = -1;
			}
		}
		else
        {
            opbx_playtones_stop(chan);
        }
	}
	return res;
}

int opbx_recvchar(struct opbx_channel *chan, int timeout)
{
	int c;
	char *buf = opbx_recvtext(chan, timeout);

	if (buf == NULL)
		return -1;	/* error or timeout */
	c = *(unsigned char *) buf;
	free(buf);
	return c;
}

char *opbx_recvtext(struct opbx_channel *chan, int timeout)
{
	int res, done = 0;
	char *buf = NULL;
	
	while (!done)
    {
		struct opbx_frame *f;

		if (opbx_check_hangup(chan))
			break;
		if ((res = opbx_waitfor(chan, timeout)) <= 0) /* timeout or error */
			break;
		timeout = res;	/* update timeout */
		if ((f = opbx_read(chan)) == NULL)
			break; /* no frame */
		if (f->frametype == OPBX_FRAME_CONTROL  &&  f->subclass == OPBX_CONTROL_HANGUP)
		{
        	done = 1;	/* force a break */
		}
        else if (f->frametype == OPBX_FRAME_TEXT)
        {
        	/* what we want */
			buf = strndup((char *) f->data, f->datalen);	/* dup and break */
			done = 1;
		}
		opbx_frfree(f);
	}
	return buf;
}

int opbx_sendtext(struct opbx_channel *chan, const char *text)
{
	int res = 0;

	/* Stop if we're a zombie or need a soft hangup */
	if (opbx_test_flag(chan, OPBX_FLAG_ZOMBIE)  ||  opbx_check_hangup(chan)) 
		return -1;
	CHECK_BLOCKING(chan);
	if (chan->tech->send_text)
		res = chan->tech->send_text(chan, text);
	opbx_clear_flag(chan, OPBX_FLAG_BLOCKING);
	return res;
}

static int do_senddigit(struct opbx_channel *chan, char digit)
{
	int res = -1;

	if (chan->tech->send_digit)
		res = chan->tech->send_digit(chan, digit);
	if (!chan->tech->send_digit || res)
    {
		/*
		 * Device does not support DTMF tones, lets fake
		 * it by doing our own generation. (PM2002)
		 */
		static const char* dtmf_tones[] =
        {
			"!941+1336/100,!0/100",	/* 0 */
			"!697+1209/100,!0/100",	/* 1 */
			"!697+1336/100,!0/100",	/* 2 */
			"!697+1477/100,!0/100",	/* 3 */
			"!770+1209/100,!0/100",	/* 4 */
			"!770+1336/100,!0/100",	/* 5 */
			"!770+1477/100,!0/100",	/* 6 */
			"!852+1209/100,!0/100",	/* 7 */
			"!852+1336/100,!0/100",	/* 8 */
			"!852+1477/100,!0/100",	/* 9 */
			"!697+1633/100,!0/100",	/* A */
			"!770+1633/100,!0/100",	/* B */
			"!852+1633/100,!0/100",	/* C */
			"!941+1633/100,!0/100",	/* D */
			"!941+1209/100,!0/100",	/* * */
			"!941+1477/100,!0/100"	/* # */
        };
		if (digit >= '0'  &&  digit <='9')
			opbx_playtones_start(chan, 0, dtmf_tones[digit - '0'], 0);
		else if (digit >= 'A' && digit <= 'D')
			opbx_playtones_start(chan, 0, dtmf_tones[digit - 'A' + 10], 0);
		else if (digit == '*')
			opbx_playtones_start(chan, 0, dtmf_tones[14], 0);
		else if (digit == '#')
			opbx_playtones_start(chan, 0, dtmf_tones[15], 0);
		else
        {
			/* not handled */
			opbx_log(LOG_DEBUG, "Unable to generate DTMF tone '%c' for '%s'\n", digit, chan->name);
		}
	}
	return 0;
}

int opbx_senddigit(struct opbx_channel *chan, char digit)
{
	return do_senddigit(chan, digit);
}

int opbx_prod(struct opbx_channel *chan)
{
	struct opbx_frame a = { OPBX_FRAME_VOICE };
	char nothing[128];

	/* Send an empty audio frame to get things moving */
	if (chan->_state != OPBX_STATE_UP)
    {
		opbx_log(LOG_DEBUG, "Prodding channel '%s'\n", chan->name);
		a.subclass = chan->rawwriteformat;
		a.data = nothing + OPBX_FRIENDLY_OFFSET;
		a.src = "opbx_prod";
		if (opbx_write(chan, &a))
			opbx_log(LOG_WARNING, "Prodding channel '%s' failed\n", chan->name);
	}
	return 0;
}

int opbx_write_video(struct opbx_channel *chan, struct opbx_frame *fr)
{
	int res;

	if (!chan->tech->write_video)
		return 0;
	if ((res = opbx_write(chan, fr)) == 0)
		res = 1;
	return res;
}

int opbx_write(struct opbx_channel *chan, struct opbx_frame *fr)
{
	int res = -1;
	struct opbx_frame *f = NULL;

	/* Stop if we're a zombie or need a soft hangup */
	opbx_mutex_lock(&chan->lock);
	if (opbx_test_flag(chan, OPBX_FLAG_ZOMBIE) || opbx_check_hangup(chan))
    {
		opbx_mutex_unlock(&chan->lock);
		return -1;
	}
	/* Handle any pending masquerades */
	if (chan->masq)
    {
		if (opbx_do_masquerade(chan))
        {
			opbx_log(LOG_WARNING, "Failed to perform masquerade\n");
			opbx_mutex_unlock(&chan->lock);
			return -1;
		}
	}
	if (chan->masqr)
    {
		opbx_mutex_unlock(&chan->lock);
		return 0;
	}

	/* A write by a non channel generator thread may or may not
	 * deactivate a running channel generator depending on
	 * whether the OPBX_FLAG_WRITE_INT is set or not for the
	 * channel. If OPBX_FLAG_WRITE_INT is set, channel generator
	 * is deactivated. Otherwise, the write is simply ignored. */
	if (!opbx_generator_is_self(chan)  &&  opbx_generator_is_active(chan))
	{
		/* We weren't called by the generator
		 * thread and channel generator is active */
		if (opbx_test_flag(chan, OPBX_FLAG_WRITE_INT))
        {
			/* Deactivate generator */

			/** unlock & lock added - testing if this caused crashes*/
			opbx_mutex_unlock(&chan->lock);
			if (option_debug)
			    opbx_log(LOG_DEBUG, "trying deactivate generator with unlock/lock channel (opbx_write function)\n");
			opbx_generator_deactivate(chan);
			opbx_mutex_lock(&chan->lock);
		}
        else
        {
			/* Write doesn't interrupt generator.
			 * Write gets ignored instead */
			opbx_mutex_unlock(&chan->lock);
			return 0;
		}

	}

	/* High bit prints debugging */
	if (chan->fout & 0x80000000)
		opbx_frame_dump(chan->name, fr, ">>");
#if 0
	/* CMANTUNES: Do we really need this CHECK_BLOCKING thing in here?
	 * I no longer think we do because we can now be reading and writing
	 * at the same time. Writing is no longer tied to reading as before */
	CHECK_BLOCKING(chan);
#endif
	switch (fr->frametype)
    {
	case OPBX_FRAME_CONTROL:
		/* XXX Interpret control frames XXX */
		opbx_log(LOG_WARNING, "Don't know how to handle control frames yet\n");
		break;
	case OPBX_FRAME_DTMF:
		opbx_clear_flag(chan, OPBX_FLAG_BLOCKING);
		opbx_mutex_unlock(&chan->lock);
		res = do_senddigit(chan,fr->subclass);
		opbx_mutex_lock(&chan->lock);
		CHECK_BLOCKING(chan);
		break;
	case OPBX_FRAME_TEXT:
		if (chan->tech->send_text)
			res = chan->tech->send_text(chan, (char *) fr->data);
		else
			res = 0;
		break;
	case OPBX_FRAME_HTML:
		if (chan->tech->send_html)
			res = chan->tech->send_html(chan, fr->subclass, (char *) fr->data, fr->datalen);
		else
			res = 0;
		break;
	case OPBX_FRAME_VIDEO:
		/* XXX Handle translation of video codecs one day XXX */
		if (chan->tech->write_video)
			res = chan->tech->write_video(chan, fr);
		else
			res = 0;
		break;
	case OPBX_FRAME_MODEM:
		if (chan->tech->write)
			res = chan->tech->write(chan, fr);
		else
			res = 0;
		break;
	default:
		if (chan->tech->write)
        {
			if (chan->writetrans) 
				f = opbx_translate(chan->writetrans, fr, 0);
			else
				f = fr;
			if (f)
            {
				/* CMANTUNES: Instead of writing directly here,
				 * we could insert frame into output queue and
				 * let the channel driver use a writer thread
				 * to actually write the stuff, for example. */

				if (f->frametype == OPBX_FRAME_VOICE  &&  chan->spiers)
                {
					struct opbx_channel_spy *spying;

					for (spying = chan->spiers;  spying;  spying = spying->next)
						opbx_queue_spy_frame(spying, f, 1);
				}

				if( chan->monitor && chan->monitor->write_stream &&
						f && ( f->frametype == OPBX_FRAME_VOICE ) ) {
#ifndef MONITOR_CONSTANT_DELAY
					int jump = chan->insmpl - chan->outsmpl - 2 * f->samples;
					if (jump >= 0)
                    {
						if (opbx_seekstream(chan->monitor->write_stream, jump + f->samples, SEEK_FORCECUR) == -1)
							opbx_log(LOG_WARNING, "Failed to perform seek in monitoring write stream, synchronization between the files may be broken\n");
						chan->outsmpl += jump + 2 * f->samples;
					}
                    else
						chan->outsmpl += f->samples;
#else
					int jump = chan->insmpl - chan->outsmpl;
					if (jump - MONITOR_DELAY >= 0)
                    {
						if (opbx_seekstream(chan->monitor->write_stream, jump - f->samples, SEEK_FORCECUR) == -1)
							opbx_log(LOG_WARNING, "Failed to perform seek in monitoring write stream, synchronization between the files may be broken\n");
						chan->outsmpl += jump;
					}
                    else
						chan->outsmpl += f->samples;
#endif
					if (opbx_writestream(chan->monitor->write_stream, f) < 0)
						opbx_log(LOG_WARNING, "Failed to write data to channel monitor write stream\n");
				}

				res = chan->tech->write(chan, f);
			}
            else
            {
				res = 0;
            }
		}
	}

	/* It's possible this is a translated frame */
	if (f && f->frametype == OPBX_FRAME_DTMF)
		opbx_log(LOG_DTMF, "%s : %c\n", chan->name, f->subclass);
    else if (fr->frametype == OPBX_FRAME_DTMF)
		opbx_log(LOG_DTMF, "%s : %c\n", chan->name, fr->subclass);

	if (f && (f != fr))
		opbx_frfree(f);
	opbx_clear_flag(chan, OPBX_FLAG_BLOCKING);
	/* Consider a write failure to force a soft hangup */
	if (res < 0)
		chan->_softhangup |= OPBX_SOFTHANGUP_DEV;
	else
    {
		if ((chan->fout & 0x7fffffff) == 0x7fffffff)
			chan->fout &= 0x80000000;
		else
			chan->fout++;
	}
	opbx_mutex_unlock(&chan->lock);
	return res;
}

static int set_format(struct opbx_channel *chan, int fmt, int *rawformat, int *format,
		      struct opbx_trans_pvt **trans, const int direction)
{
	int native;
	int res;
	
	native = chan->nativeformats;
	/* Find a translation path from the native format to one of the desired formats */
	if (!direction)
		/* reading */
		res = opbx_translator_best_choice(&fmt, &native);
	else
		/* writing */
		res = opbx_translator_best_choice(&native, &fmt);

	if (res < 0)
    {
		opbx_log(LOG_WARNING, "Unable to find a codec translation path from %s to %s\n",
			opbx_getformatname(native), opbx_getformatname(fmt));
		return -1;
	}
	
	/* Now we have a good choice for both. */
	opbx_mutex_lock(&chan->lock);

	if ((*rawformat == native) && (*format == fmt) && ((*rawformat == *format) || (*trans))) {
		/* the channel is already in these formats, so nothing to do */
		opbx_mutex_unlock(&chan->lock);
		return 0;
	}

	*rawformat = native;
	/* User perspective is fmt */
	*format = fmt;
	/* Free any read translation we have right now */
	if (*trans)
		opbx_translator_free_path(*trans);
	/* Build a translation path from the raw format to the desired format */
	if (!direction)
		/* reading */
		*trans = opbx_translator_build_path(*format, *rawformat);
	else
		/* writing */
		*trans = opbx_translator_build_path(*rawformat, *format);
	opbx_mutex_unlock(&chan->lock);
	if (option_debug)
    {
		opbx_log(LOG_DEBUG, "Set channel %s to %s format %s\n", chan->name,
			direction ? "write" : "read", opbx_getformatname(fmt));
	}
    return 0;
}

int opbx_set_read_format(struct opbx_channel *chan, int fmt)
{
	return set_format(chan, fmt, &chan->rawreadformat, &chan->readformat,
			  &chan->readtrans, 0);
}

int opbx_set_write_format(struct opbx_channel *chan, int fmt)
{
	return set_format(chan, fmt, &chan->rawwriteformat, &chan->writeformat,
			  &chan->writetrans, 1);
}

struct opbx_channel *__opbx_request_and_dial(const char *type, int format, void *data, int timeout, int *outstate, const char *cid_num, const char *cid_name, struct outgoing_helper *oh)
{
	int state = 0;
	int cause = 0;
	struct opbx_channel *chan;
	struct opbx_frame *f;
	int res = 0;
	
	chan = opbx_request(type, format, data, &cause);
	if (chan)
    {
		if (oh)
        {
			opbx_set_variables(chan, oh->vars);
			opbx_set_callerid(chan, oh->cid_num, oh->cid_name, oh->cid_num);
		}
		opbx_set_callerid(chan, cid_num, cid_name, cid_num);

		if (!opbx_call(chan, data, 0))
        {
			while(timeout && (chan->_state != OPBX_STATE_UP))
            {
				if ((res = opbx_waitfor(chan, timeout)) < 0)
                {
					/* Something not cool, or timed out */
					break;
				}
				/* If done, break out */
				if (!res)
					break;
				if (timeout > -1)
					timeout = res;
				if ((f = opbx_read(chan)) == 0)
				{
					state = OPBX_CONTROL_HANGUP;
					res = 0;
					break;
				}
				if (f->frametype == OPBX_FRAME_CONTROL)
                {
					if (f->subclass == OPBX_CONTROL_RINGING)
                    {
						state = OPBX_CONTROL_RINGING;
                    }
					else if ((f->subclass == OPBX_CONTROL_BUSY)  ||  (f->subclass == OPBX_CONTROL_CONGESTION))
                    {
						state = f->subclass;
						opbx_frfree(f);
						break;
					}
                    else if (f->subclass == OPBX_CONTROL_ANSWER)
                    {
						state = f->subclass;
						opbx_frfree(f);
						break;
					}
                    else if (f->subclass == OPBX_CONTROL_PROGRESS)
                    {
						/* Ignore */
					}
                    else if (f->subclass == -1)
                    {
						/* Ignore -- just stopping indications */
					}
                    else
                    {
						opbx_log(LOG_NOTICE, "Don't know what to do with control frame %d\n", f->subclass);
					}
				}
				opbx_frfree(f);
			}
		}
        else
			opbx_log(LOG_NOTICE, "Unable to call channel %s/%s\n", type, (char *)data);
	}
    else
    {
		opbx_log(LOG_NOTICE, "Unable to request channel %s/%s\n", type, (char *)data);
		switch(cause)
        {
		case OPBX_CAUSE_BUSY:
			state = OPBX_CONTROL_BUSY;
			break;
		case OPBX_CAUSE_CONGESTION:
			state = OPBX_CONTROL_CONGESTION;
			break;
		}
	}
	if (chan)
    {
		/* Final fixups */
		if (oh)
        {
			if (oh->context && *oh->context)
				opbx_copy_string(chan->context, oh->context, sizeof(chan->context));
			if (oh->exten && *oh->exten)
				opbx_copy_string(chan->exten, oh->exten, sizeof(chan->exten));
			if (oh->priority)	
				chan->priority = oh->priority;
		}
		if (chan->_state == OPBX_STATE_UP) 
			state = OPBX_CONTROL_ANSWER;
	}
	if (outstate)
		*outstate = state;
	if (chan  &&  res <= 0)
    {
		if (!chan->cdr)
        {
			chan->cdr = opbx_cdr_alloc();
			if (chan->cdr)
				opbx_cdr_init(chan->cdr, chan);
		}
		if (chan->cdr)
        {
			char tmp[256];
			snprintf(tmp, 256, "%s/%s", type, (char *)data);
			opbx_cdr_setapp(chan->cdr,"Dial",tmp);
			opbx_cdr_update(chan);
			opbx_cdr_start(chan->cdr);
			opbx_cdr_end(chan->cdr);
			/* If the cause wasn't handled properly */
			if (opbx_cdr_disposition(chan->cdr,chan->hangupcause))
				opbx_cdr_failed(chan->cdr);
		}
        else 
		{
        	opbx_log(LOG_WARNING, "Unable to create Call Detail Record\n");
		}
        opbx_hangup(chan);
		chan = NULL;
	}
	return chan;
}

struct opbx_channel *opbx_request_and_dial(const char *type, int format, void *data, int timeout, int *outstate, const char *cidnum, const char *cidname)
{
	return __opbx_request_and_dial(type, format, data, timeout, outstate, cidnum, cidname, NULL);
}

struct opbx_channel *opbx_request(const char *type, int format, void *data, int *cause)
{
	struct chanlist *chan;
	struct opbx_channel *c = NULL;
	int capabilities;
	int fmt;
	int res;
	int foo;

	if (!cause)
		cause = &foo;
	*cause = OPBX_CAUSE_NOTDEFINED;
	if (opbx_mutex_lock(&chlock))
    {
		opbx_log(LOG_WARNING, "Unable to lock channel list\n");
		return NULL;
	}
	chan = backends;
	while (chan)
    {
		if (!strcasecmp(type, chan->tech->type))
        {
			capabilities = chan->tech->capabilities;
			fmt = format;
			res = opbx_translator_best_choice(&fmt, &capabilities);
			if (res < 0)
            {
				opbx_log(LOG_WARNING, "No translator path exists for channel type %s (native %d) to %d\n", type, chan->tech->capabilities, format);
				opbx_mutex_unlock(&chlock);
				return NULL;
			}
			opbx_mutex_unlock(&chlock);
			if (chan->tech->requester)
				c = chan->tech->requester(type, capabilities, data, cause);
			if (c)
            {
				if (c->_state == OPBX_STATE_DOWN)
                {
					manager_event(EVENT_FLAG_CALL, "Newchannel",
					"Channel: %s\r\n"
					"State: %s\r\n"
					"CallerID: %s\r\n"
					"CallerIDName: %s\r\n"
					"Uniqueid: %s\r\n"
					"Type: %s\r\n"
					"Dialstring: %s\r\n",					
					c->name, opbx_state2str(c->_state), c->cid.cid_num ? c->cid.cid_num : "<unknown>", c->cid.cid_name ? c->cid.cid_name : "<unknown>",c->uniqueid,type,(char *)data);
				}
			}
			return c;
		}
		chan = chan->next;
	}
	if (!chan)
    {
		opbx_log(LOG_WARNING, "No channel type registered for '%s'\n", type);
		*cause = OPBX_CAUSE_NOSUCHDRIVER;
	}
	opbx_mutex_unlock(&chlock);
	return c;
}

int opbx_call(struct opbx_channel *chan, char *addr, int timeout) 
{
	/* Place an outgoing call, but don't wait any longer than timeout ms before returning. 
	   If the remote end does not answer within the timeout, then do NOT hang up, but 
	   return anyway.  */
	int res = -1;

	/* Stop if we're a zombie or need a soft hangup */
	opbx_mutex_lock(&chan->lock);
	if (!opbx_test_flag(chan, OPBX_FLAG_ZOMBIE)  &&  !opbx_check_hangup(chan))
    {
		if (chan->tech->call)
			res = chan->tech->call(chan, addr, timeout);
	}
    opbx_mutex_unlock(&chan->lock);
	return res;
}

/*--- opbx_transfer: Transfer a call to dest, if the channel supports transfer */
/*	called by app_transfer or the manager interface */
int opbx_transfer(struct opbx_channel *chan, char *dest) 
{
	int res = -1;

	/* Stop if we're a zombie or need a soft hangup */
	opbx_mutex_lock(&chan->lock);
	if (!opbx_test_flag(chan, OPBX_FLAG_ZOMBIE) && !opbx_check_hangup(chan))
    {
		if (chan->tech->transfer)
        {
			res = chan->tech->transfer(chan, dest);
			if (!res)
				res = 1;
		}
        else
        {
			res = 0;
        }
	}
	opbx_mutex_unlock(&chan->lock);
	return res;
}

int opbx_readstring(struct opbx_channel *c, char *s, int len, int timeout, int ftimeout, char *enders)
{
	int pos = 0;
	int to = ftimeout;
	int d;

	/* XXX Merge with full version? XXX */
	/* Stop if we're a zombie or need a soft hangup */
	if (opbx_test_flag(c, OPBX_FLAG_ZOMBIE)  ||  opbx_check_hangup(c)) 
		return -1;
	if (!len)
		return -1;
	for (;;)
    {
		if (c->stream)
        {
			d = opbx_waitstream(c, OPBX_DIGIT_ANY);
			opbx_stopstream(c);
			usleep(1000);
			if (!d)
				d = opbx_waitfordigit(c, to);
		}
        else
        {
			d = opbx_waitfordigit(c, to);
		}
		if (d < 0)
			return -1;
		if (d == 0)
        {
			s[pos]='\0';
			return 1;
		}
		if (!strchr(enders, d))
			s[pos++] = d;
		if (strchr(enders, d)  ||  (pos >= len))
        {
			s[pos]='\0';
			return 0;
		}
		to = timeout;
	}
	/* Never reached */
	return 0;
}

int opbx_readstring_full(struct opbx_channel *c, char *s, int len, int timeout, int ftimeout, char *enders, int audiofd, int ctrlfd)
{
	int pos = 0;
	int to = ftimeout;
	int d;

	/* Stop if we're a zombie or need a soft hangup */
	if (opbx_test_flag(c, OPBX_FLAG_ZOMBIE)  ||  opbx_check_hangup(c)) 
		return -1;
	if (!len)
		return -1;
	for (;;)
    {
		if (c->stream)
        {
			d = opbx_waitstream_full(c, OPBX_DIGIT_ANY, audiofd, ctrlfd);
			opbx_stopstream(c);
			usleep(1000);
			if (!d)
				d = opbx_waitfordigit_full(c, to, audiofd, ctrlfd);
		}
        else
        {
			d = opbx_waitfordigit_full(c, to, audiofd, ctrlfd);
		}
		if (d < 0)
			return -1;
		if (d == 0)
        {
			s[pos]='\0';
			return 1;
		}
		if (d == 1)
        {
			s[pos]='\0';
			return 2;
		}
		if (!strchr(enders, d))
			s[pos++] = d;
		if (strchr(enders, d)  ||  (pos >= len))
        {
			s[pos]='\0';
			return 0;
		}
		to = timeout;
	}
	/* Never reached */
	return 0;
}

int opbx_channel_supports_html(struct opbx_channel *chan)
{
	if (chan->tech->send_html)
		return 1;
	return 0;
}

int opbx_channel_sendhtml(struct opbx_channel *chan, int subclass, const char *data, int datalen)
{
	if (chan->tech->send_html)
		return chan->tech->send_html(chan, subclass, data, datalen);
	return -1;
}

int opbx_channel_sendurl(struct opbx_channel *chan, const char *url)
{
	if (chan->tech->send_html)
		return chan->tech->send_html(chan, OPBX_HTML_URL, url, strlen(url) + 1);
	return -1;
}

int opbx_channel_make_compatible(struct opbx_channel *chan, struct opbx_channel *peer)
{
	int src;
	int dst;

	/* Set up translation from the chan to the peer */
	src = chan->nativeformats;
	dst = peer->nativeformats;
	if (opbx_translator_best_choice(&dst, &src) < 0)
    {
		opbx_log(LOG_WARNING, "No path to translate from %s(%d) to %s(%d)\n", chan->name, src, peer->name, dst);
		return -1;
	}

	/* if the best path is not 'pass through', then
	   transcoding is needed; if desired, force transcode path
	   to use SLINEAR between channels */
	if ((src != dst) && option_transcode_slin)
		dst = OPBX_FORMAT_SLINEAR;
	if (opbx_set_read_format(chan, dst) < 0)
    {
		opbx_log(LOG_WARNING, "Unable to set read format on channel %s to %d\n", chan->name, dst);
		return -1;
	}
	if (opbx_set_write_format(peer, dst) < 0)
    {
		opbx_log(LOG_WARNING, "Unable to set write format on channel %s to %d\n", peer->name, dst);
		return -1;
	}

	/* Set up translation from the peer to the chan */
	src = peer->nativeformats;
	dst = chan->nativeformats;
	if (opbx_translator_best_choice(&dst, &src) < 0)
    {
		opbx_log(LOG_WARNING, "No path to translate from %s(%d) to %s(%d)\n", peer->name, src, chan->name, dst);
		return -1;
	}
	/* if the best path is not 'pass through', then
	   transcoding is needed; if desired, force transcode path
	   to use SLINEAR between channels */
	if ((src != dst) && option_transcode_slin)
		dst = OPBX_FORMAT_SLINEAR;
	if (opbx_set_read_format(peer, dst) < 0)
    {
		opbx_log(LOG_WARNING, "Unable to set read format on channel %s to %d\n", peer->name, dst);
		return -1;
	}
	if (opbx_set_write_format(chan, dst) < 0)
    {
		opbx_log(LOG_WARNING, "Unable to set write format on channel %s to %d\n", chan->name, dst);
		return -1;
	}
	return 0;
}

int opbx_channel_masquerade(struct opbx_channel *original, struct opbx_channel *clone)
{
    struct opbx_frame null = { OPBX_FRAME_NULL, };
    int res = -1;

	/* each of these channels may be sitting behind a channel proxy (i.e. chan_agent)
	   and if so, we don't really want to masquerade it, but its proxy */
	if (original->_bridge && (original->_bridge != opbx_bridged_channel(original)))
		original = original->_bridge;

	if (clone->_bridge && (clone->_bridge != opbx_bridged_channel(clone)))
		clone = clone->_bridge;


    if (original == clone)
    {
		opbx_log(LOG_WARNING, "Can't masquerade channel '%s' into itself!\n", original->name);
		return -1;
    }

    opbx_mutex_lock(&original->lock);
    while (opbx_mutex_trylock(&clone->lock))
    {
		opbx_mutex_unlock(&original->lock);
		usleep(1);
		opbx_mutex_lock(&original->lock);
    }
    opbx_log(LOG_DEBUG, "Planning to masquerade channel %s into the structure of %s\n",
    	clone->name, original->name);

    if (original->masq)
    {
	opbx_log(LOG_WARNING, "%s is already going to masquerade as %s\n", 
    	original->masq->name, original->name);
    }
    else if (clone->masqr)
    {
		opbx_log(LOG_WARNING, "%s is already going to masquerade as %s\n", 
			clone->name, clone->masqr->name);
    }
    else
    {
		original->masq = clone;
		clone->masqr = original;
		opbx_queue_frame(original, &null);
		opbx_queue_frame(clone, &null);
		opbx_log(LOG_DEBUG, "Done planning to masquerade channel %s into the structure of %s\n", clone->name, original->name);
		res = 0;
    }
    opbx_mutex_unlock(&clone->lock);
    opbx_mutex_unlock(&original->lock);
    return res;
}

void opbx_change_name(struct opbx_channel *chan, char *newname)
{
	char tmp[256];
	opbx_copy_string(tmp, chan->name, sizeof(tmp));
	opbx_copy_string(chan->name, newname, sizeof(chan->name));
	manager_event(EVENT_FLAG_CALL, "Rename", "Oldname: %s\r\nNewname: %s\r\nUniqueid: %s\r\n", tmp, chan->name, chan->uniqueid);
}

void opbx_channel_inherit_variables(const struct opbx_channel *parent, struct opbx_channel *child)
{
	struct opbx_var_t *current, *newvar;
	char *varname;

	OPBX_LIST_TRAVERSE(&parent->varshead, current, entries)
    {
		int vartype = 0;

		varname = opbx_var_full_name(current);
		if (!varname)
			continue;

		if (varname[0] == '_')
        {
			vartype = 1;
			if (varname[1] == '_')
				vartype = 2;
		}

		switch (vartype)
        {
		case 1:
			newvar = opbx_var_assign(&varname[1], opbx_var_value(current));
			if (newvar)
            {
				OPBX_LIST_INSERT_TAIL(&child->varshead, newvar, entries);
				if (option_debug)
					opbx_log(LOG_DEBUG, "Copying soft-transferable variable %s.\n", opbx_var_name(newvar));
			}
			break;
		case 2:
			newvar = opbx_var_assign(opbx_var_full_name(current), opbx_var_value(current));
			if (newvar)
            {
				OPBX_LIST_INSERT_TAIL(&child->varshead, newvar, entries);
				if (option_debug)
					opbx_log(LOG_DEBUG, "Copying hard-transferable variable %s.\n", opbx_var_name(newvar));
			}
			break;
		default:
			if (option_debug)
				opbx_log(LOG_DEBUG, "Not copying variable %s.\n", opbx_var_name(current));
			break;
		}
	}
}

/* Clone channel variables from 'clone' channel into 'original' channel
   All variables except those related to app_groupcount are cloned
   Variables are actually _removed_ from 'clone' channel, presumably
   because it will subsequently be destroyed.
   Assumes locks will be in place on both channels when called.
*/
   
static void clone_variables(struct opbx_channel *original, struct opbx_channel *clone)
{
	struct opbx_var_t *varptr;

	/* we need to remove all app_groupcount related variables from the original
	   channel before merging in the clone's variables; any groups assigned to the
	   original channel should be released, only those assigned to the clone
	   should remain
	*/

	OPBX_LIST_TRAVERSE_SAFE_BEGIN(&original->varshead, varptr, entries)
    {
		if (!strncmp(opbx_var_name(varptr), GROUP_CATEGORY_PREFIX, strlen(GROUP_CATEGORY_PREFIX)))
        {
			OPBX_LIST_REMOVE(&original->varshead, varptr, entries);
			opbx_var_delete(varptr);
		}
	}
	OPBX_LIST_TRAVERSE_SAFE_END;

	/* Append variables from clone channel into original channel */
	/* XXX Is this always correct?  We have to in order to keep MACROS working XXX */
	if (OPBX_LIST_FIRST(&clone->varshead))
		OPBX_LIST_INSERT_TAIL(&original->varshead, OPBX_LIST_FIRST(&clone->varshead), entries);
}

/*--- opbx_do_masquerade: Masquerade a channel */
/* Assumes channel will be locked when called */
int opbx_do_masquerade(struct opbx_channel *original)
{
	int x,i;
	int res=0;
	int origstate;
	struct opbx_frame *cur, *prev;
	const struct opbx_channel_tech *t;
	void *t_pvt;
	struct opbx_callerid tmpcid;
	struct opbx_channel *clone = original->masq;
	int rformat = original->readformat;
	int wformat = original->writeformat;
	char newn[100];
	char orig[100];
	char masqn[100];
	char zombn[100];

	if (option_debug > 3)
		opbx_log(LOG_DEBUG, "Actually Masquerading %s(%d) into the structure of %s(%d)\n",
			clone->name, clone->_state, original->name, original->_state);

	/* XXX This is a seriously wacked out operation.  We're essentially putting the guts of
	   the clone channel into the original channel.  Start by killing off the original
	   channel's backend.   I'm not sure we're going to keep this function, because 
	   while the features are nice, the cost is very high in terms of pure nastiness. XXX */

	/* We need the clone's lock, too */
	opbx_mutex_lock(&clone->lock);

	opbx_log(LOG_DEBUG, "Got clone lock for masquerade on '%s' at %p\n", clone->name, &clone->lock);

	/* Having remembered the original read/write formats, we turn off any translation on either
	   one */
	free_translation(clone);
	free_translation(original);


	/* Unlink the masquerade */
	original->masq = NULL;
	clone->masqr = NULL;
	
	/* Save the original name */
	opbx_copy_string(orig, original->name, sizeof(orig));
	/* Save the new name */
	opbx_copy_string(newn, clone->name, sizeof(newn));
	/* Create the masq name */
	snprintf(masqn, sizeof(masqn), "%s<MASQ>", newn);
		
	/* Copy the name from the clone channel */
	opbx_copy_string(original->name, newn, sizeof(original->name));

	/* Mangle the name of the clone channel */
	opbx_copy_string(clone->name, masqn, sizeof(clone->name));
	
	/* Notify any managers of the change, first the masq then the other */
	manager_event(EVENT_FLAG_CALL, "Rename", "Oldname: %s\r\nNewname: %s\r\nUniqueid: %s\r\n", newn, masqn, clone->uniqueid);
	manager_event(EVENT_FLAG_CALL, "Rename", "Oldname: %s\r\nNewname: %s\r\nUniqueid: %s\r\n", orig, newn, original->uniqueid);

	/* Swap the technlogies */	
	t = original->tech;
	original->tech = clone->tech;
	clone->tech = t;

	t_pvt = original->tech_pvt;
	original->tech_pvt = clone->tech_pvt;
	clone->tech_pvt = t_pvt;

	/* Swap the readq's */
	cur = original->readq;
	original->readq = clone->readq;
	clone->readq = cur;

	/* Swap the alertpipes */
	for (i = 0;  i < 2;  i++)
    {
		x = original->alertpipe[i];
		original->alertpipe[i] = clone->alertpipe[i];
		clone->alertpipe[i] = x;
	}

	/* Swap the raw formats */
	x = original->rawreadformat;
	original->rawreadformat = clone->rawreadformat;
	clone->rawreadformat = x;
	x = original->rawwriteformat;
	original->rawwriteformat = clone->rawwriteformat;
	clone->rawwriteformat = x;

	/* Save any pending frames on both sides.  Start by counting
	 * how many we're going to need... */
	prev = NULL;
	cur = clone->readq;
	x = 0;
	while (cur)
    {
		x++;
		prev = cur;
		cur = cur->next;
	}
	/* If we had any, prepend them to the ones already in the queue, and 
	 * load up the alertpipe */
	if (prev)
    {
		prev->next = original->readq;
		original->readq = clone->readq;
		clone->readq = NULL;
		if (original->alertpipe[1] > -1)
        {
			for (i = 0;  i < x;  i++)
				write(original->alertpipe[1], &x, sizeof(x));
		}
	}
	clone->_softhangup = OPBX_SOFTHANGUP_DEV;


	/* And of course, so does our current state.  Note we need not
	   call opbx_setstate since the event manager doesn't really consider
	   these separate.  We do this early so that the clone has the proper
	   state of the original channel. */
	origstate = original->_state;
	original->_state = clone->_state;
	clone->_state = origstate;

	if (clone->tech->fixup)
    {
		if ((res = clone->tech->fixup(original, clone)))
			opbx_log(LOG_WARNING, "Fixup failed on channel %s, strange things may happen.\n", clone->name);
	}

	/* Start by disconnecting the original's physical side */
	if (clone->tech->hangup)
		res = clone->tech->hangup(clone);
	if (res)
    {
		opbx_log(LOG_WARNING, "Hangup failed!  Strange things may happen!\n");
		opbx_mutex_unlock(&clone->lock);
		return -1;
	}
	
	snprintf(zombn, sizeof(zombn), "%s<ZOMBIE>", orig);
	/* Mangle the name of the clone channel */
	opbx_copy_string(clone->name, zombn, sizeof(clone->name));
	manager_event(EVENT_FLAG_CALL, "Rename", "Oldname: %s\r\nNewname: %s\r\nUniqueid: %s\r\n", masqn, zombn, clone->uniqueid);

	/* Update the type. */
	original->type = clone->type;
	t_pvt = original->monitor;
	original->monitor = clone->monitor;
	clone->monitor = t_pvt;
	
	/* Keep the same language.  */
	opbx_copy_string(original->language, clone->language, sizeof(original->language));
	/* Copy the FD's */
	for (x = 0;  x < OPBX_MAX_FDS;  x++)
		original->fds[x] = clone->fds[x];
	clone_variables(original, clone);
	OPBX_LIST_HEAD_INIT_NOLOCK(&clone->varshead);
	/* Presense of ADSI capable CPE follows clone */
	original->adsicpe = clone->adsicpe;
	/* Bridge remains the same */
	/* CDR fields remain the same */
	/* XXX What about blocking, softhangup, blocker, and lock and blockproc? XXX */
	/* Application and data remain the same */
	/* Clone exception  becomes real one, as with fdno */
	opbx_copy_flags(original, clone, OPBX_FLAG_EXCEPTION);
	original->fdno = clone->fdno;
	/* Schedule context remains the same */
	/* Stream stuff stays the same */
	/* Keep the original state.  The fixup code will need to work with it most likely */

	/* Just swap the whole structures, nevermind the allocations, they'll work themselves
	   out. */
	tmpcid = original->cid;
	original->cid = clone->cid;
	clone->cid = tmpcid;
	
	/* Our native formats are different now */
	original->nativeformats = clone->nativeformats;
	
	/* Context, extension, priority, app data, jump table,  remain the same */
	/* pvt switches.  pbx stays the same, as does next */
	
	/* Set the write format */
	opbx_set_write_format(original, wformat);

	/* Set the read format */
	opbx_set_read_format(original, rformat);

	/* Copy the music class */
	opbx_copy_string(original->musicclass, clone->musicclass, sizeof(original->musicclass));

	opbx_log(LOG_DEBUG, "Putting channel %s in %d/%d formats\n", original->name, wformat, rformat);

	/* Okay.  Last thing is to let the channel driver know about all this mess, so he
	   can fix up everything as best as possible */
	if (original->tech->fixup)
    {
		res = original->tech->fixup(clone, original);
		if (res)
        {
			opbx_log(LOG_WARNING, "Channel for type '%s' could not fixup channel %s\n",
				original->type, original->name);
			opbx_mutex_unlock(&clone->lock);
			return -1;
		}
	}
    else
    {
		opbx_log(LOG_WARNING, "Channel type '%s' does not have a fixup routine (for %s)!  Bad things may happen.\n",
                 original->type, original->name);
	}

	/* Now, at this point, the "clone" channel is totally F'd up.  We mark it as
	   a zombie so nothing tries to touch it.  If it's already been marked as a
	   zombie, then free it now (since it already is considered invalid). */
	if (opbx_test_flag(clone, OPBX_FLAG_ZOMBIE))
    {
		opbx_log(LOG_DEBUG, "Destroying channel clone '%s'\n", clone->name);
		opbx_mutex_unlock(&clone->lock);
		opbx_channel_free(clone);
		manager_event(EVENT_FLAG_CALL, "Hangup", 
			"Channel: %s\r\n"
			"Uniqueid: %s\r\n"
			"Cause: %d\r\n"
			"Cause-txt: %s\r\n",
			clone->name, 
			clone->uniqueid, 
			clone->hangupcause,
			opbx_cause2str(clone->hangupcause)
			);
	}
    else
    {
		struct opbx_frame null_frame = { OPBX_FRAME_NULL, };
		opbx_log(LOG_DEBUG, "Released clone lock on '%s'\n", clone->name);
		opbx_set_flag(clone, OPBX_FLAG_ZOMBIE);
		opbx_queue_frame(clone, &null_frame);
		opbx_mutex_unlock(&clone->lock);
	}
	
	/* Signal any blocker */
	if (opbx_test_flag(original, OPBX_FLAG_BLOCKING))
		pthread_kill(original->blocker, SIGURG);
	opbx_log(LOG_DEBUG, "Done Masquerading %s (%d)\n", original->name, original->_state);
	return 0;
}

void opbx_set_callerid(struct opbx_channel *chan, const char *callerid, const char *calleridname, const char *ani)
{
	if (callerid)
    {
		if (chan->cid.cid_num)
			free(chan->cid.cid_num);
		if (opbx_strlen_zero(callerid))
			chan->cid.cid_num = NULL;
		else
			chan->cid.cid_num = strdup(callerid);
	}
	if (calleridname)
    {
		if (chan->cid.cid_name)
			free(chan->cid.cid_name);
		if (opbx_strlen_zero(calleridname))
			chan->cid.cid_name = NULL;
		else
			chan->cid.cid_name = strdup(calleridname);
	}
	if (ani)
    {
		if (chan->cid.cid_ani)
			free(chan->cid.cid_ani);
		if (opbx_strlen_zero(ani))
			chan->cid.cid_ani = NULL;
		else
			chan->cid.cid_ani = strdup(ani);
	}
	if (chan->cdr)
		opbx_cdr_setcid(chan->cdr, chan);
	manager_event(EVENT_FLAG_CALL, "Newcallerid", 
				"Channel: %s\r\n"
				"CallerID: %s\r\n"
				"CallerIDName: %s\r\n"
				"Uniqueid: %s\r\n"
				"CID-CallingPres: %d (%s)\r\n",
				chan->name, chan->cid.cid_num ? 
				chan->cid.cid_num : "<Unknown>",
				chan->cid.cid_name ? 
				chan->cid.cid_name : "<Unknown>",
				chan->uniqueid,
				chan->cid.cid_pres,
				opbx_describe_caller_presentation(chan->cid.cid_pres)
				);
}

int opbx_setstate(struct opbx_channel *chan, int state)
{
	int oldstate = chan->_state;

	if (oldstate == state)
		return 0;

	chan->_state = state;
	opbx_device_state_changed_literal(chan->name);
	manager_event(EVENT_FLAG_CALL,
		      (oldstate == OPBX_STATE_DOWN) ? "Newchannel" : "Newstate",
		      "Channel: %s\r\n"
		      "State: %s\r\n"
		      "CallerID: %s\r\n"
		      "CallerIDName: %s\r\n"
		      "Uniqueid: %s\r\n",
		      chan->name, opbx_state2str(chan->_state), 
		      chan->cid.cid_num ? chan->cid.cid_num : "<unknown>", 
		      chan->cid.cid_name ? chan->cid.cid_name : "<unknown>", 
		      chan->uniqueid);

	return 0;
}

/*--- Find bridged channel */
struct opbx_channel *opbx_bridged_channel(struct opbx_channel *chan)
{
	struct opbx_channel *bridged;
	bridged = chan->_bridge;
	if (bridged && bridged->tech->bridged_channel) 
		bridged = bridged->tech->bridged_channel(chan, bridged);
	return bridged;
}

static void bridge_playfile(struct opbx_channel *chan, struct opbx_channel *peer, char *sound, int remain) 
{
	int res=0, min=0, sec=0,check=0;

	check = opbx_autoservice_start(peer);
	if(check) 
		return;

	if (remain > 0)
    {
		if (remain / 60 > 1)
        {
			min = remain / 60;
			sec = remain % 60;
		}
        else
        {
			sec = remain;
		}
	}
	
	if (!strcmp(sound, "timeleft"))
    {
    	/* Queue support */
		res = opbx_streamfile(chan, "vm-youhave", chan->language);
		res = opbx_waitstream(chan, "");
		if (min)
        {
			res = opbx_say_number(chan, min, OPBX_DIGIT_ANY, chan->language, (char *) NULL);
			res = opbx_streamfile(chan, "queue-minutes", chan->language);
			res = opbx_waitstream(chan, "");
		}
		if (sec)
        {
			res = opbx_say_number(chan, sec, OPBX_DIGIT_ANY, chan->language, (char *) NULL);
			res = opbx_streamfile(chan, "queue-seconds", chan->language);
			res = opbx_waitstream(chan, "");
		}
	}
    else
    {
		res = opbx_streamfile(chan, sound, chan->language);
		res = opbx_waitstream(chan, "");
	}

	check = opbx_autoservice_stop(peer);
}

static enum opbx_bridge_result opbx_generic_bridge(struct opbx_channel *c0, struct opbx_channel *c1,
			      struct opbx_bridge_config *config, struct opbx_frame **fo, struct opbx_channel **rc, int toms)
{
	/* Copy voice back and forth between the two channels. */
	struct opbx_channel *cs[3];
	struct opbx_frame *f;
	struct opbx_channel *who = NULL;
	enum opbx_bridge_result res = OPBX_BRIDGE_COMPLETE;
	int o0nativeformats;
	int o1nativeformats;
	int watch_c0_dtmf;
	int watch_c1_dtmf;
	void *pvt0, *pvt1;
	
	/* Indicates whether a frame was queued into a jitterbuffer */
	int frame_put_in_jb;

	cs[0] = c0;
	cs[1] = c1;
	pvt0 = c0->tech_pvt;
	pvt1 = c1->tech_pvt;
	o0nativeformats = c0->nativeformats;
	o1nativeformats = c1->nativeformats;
	watch_c0_dtmf = config->flags & OPBX_BRIDGE_DTMF_CHANNEL_0;
	watch_c1_dtmf = config->flags & OPBX_BRIDGE_DTMF_CHANNEL_1;

	/* Check the need of a jitterbuffer for each channel */
	opbx_jb_do_usecheck(c0, c1);

	for (;;) {
		if ((c0->tech_pvt != pvt0) || (c1->tech_pvt != pvt1) ||
		    (o0nativeformats != c0->nativeformats) ||
		    (o1nativeformats != c1->nativeformats)) {
			/* Check for Masquerade, codec changes, etc */
			res = OPBX_BRIDGE_RETRY;
			break;
		}

		/* Calculate the appropriate max sleep interval - 
		 * in general, this is the time,
		 * left to the closest jb delivery moment */
		toms = opbx_jb_get_when_to_wakeup(c0, c1, toms);

		who = opbx_waitfor_n(cs, 2, &toms);
		if (!who) {
			if (!toms) {
				res = OPBX_BRIDGE_RETRY;
				break;
			}

			/* No frame received within the specified timeout - 
			 * check if we have to deliver now */
			opbx_jb_get_and_deliver(c0, c1);

			opbx_log(LOG_DEBUG, "Nobody there, continuing...\n"); 
			if (c0->_softhangup == OPBX_SOFTHANGUP_UNBRIDGE || c1->_softhangup == OPBX_SOFTHANGUP_UNBRIDGE) {
				if (c0->_softhangup == OPBX_SOFTHANGUP_UNBRIDGE)
					c0->_softhangup = 0;
				if (c1->_softhangup == OPBX_SOFTHANGUP_UNBRIDGE)
					c1->_softhangup = 0;
				c0->_bridge = c1;
				c1->_bridge = c0;
			}
			continue;
		}
		f = opbx_read(who);
		if (!f) {
			*fo = NULL;
			*rc = who;
			res = OPBX_BRIDGE_COMPLETE;
			opbx_log(LOG_DEBUG, "Didn't get a frame from channel: %s\n",who->name);
			break;
		}

		/* Try add the frame info the who's bridged channel jitterbuff */
		frame_put_in_jb = !opbx_jb_put((who == c0) ? c1 : c0, f, f->subclass);

		if ((f->frametype == OPBX_FRAME_CONTROL) && !(config->flags & OPBX_BRIDGE_IGNORE_SIGS)) {
			if ((f->subclass == OPBX_CONTROL_HOLD) || (f->subclass == OPBX_CONTROL_UNHOLD) ||
			    (f->subclass == OPBX_CONTROL_VIDUPDATE)) {
				opbx_indicate(who == c0 ? c1 : c0, f->subclass);
			} else {
				*fo = f;
				*rc = who;
				res =  OPBX_BRIDGE_COMPLETE;
				opbx_log(LOG_DEBUG, "Got a FRAME_CONTROL (%d) frame on channel %s\n", f->subclass, who->name);
				break;
			}
		}
		if ((f->frametype == OPBX_FRAME_VOICE) ||
		    (f->frametype == OPBX_FRAME_DTMF) ||
		    (f->frametype == OPBX_FRAME_VIDEO) || 
		    (f->frametype == OPBX_FRAME_IMAGE) ||
		    (f->frametype == OPBX_FRAME_HTML) ||
		    (f->frametype == OPBX_FRAME_MODEM) ||
		    (f->frametype == OPBX_FRAME_TEXT)) {
			if (f->frametype == OPBX_FRAME_DTMF) {
				if (((who == c0) && watch_c0_dtmf) ||
				    ((who == c1) && watch_c1_dtmf)) {
					*rc = who;
					*fo = f;
					res = OPBX_BRIDGE_COMPLETE;
					opbx_log(LOG_DEBUG, "Got DTMF on channel (%s)\n", who->name);
					break;
				} else {
					goto tackygoto;
				}
			} else {
#if 0
				opbx_log(LOG_DEBUG, "Read from %s\n", who->name);
				if (who == last) 
					opbx_log(LOG_DEBUG, "Servicing channel %s twice in a row?\n", last->name);
				last = who;
#endif
tackygoto:
				/* Write immediately frames, not passed through jb */
				if (!frame_put_in_jb)
					opbx_write((who == c0)  ?  c1  :  c0, f);
				
				/* Check if we have to deliver now */
				opbx_jb_get_and_deliver(c0, c1);
			}
		}
		opbx_frfree(f);

		/* Swap who gets priority */
		cs[2] = cs[0];
		cs[0] = cs[1];
		cs[1] = cs[2];
	}
	return res;
}

/*--- opbx_channel_bridge: Bridge two channels together */
enum opbx_bridge_result opbx_channel_bridge(struct opbx_channel *c0, struct opbx_channel *c1, struct opbx_bridge_config *config, struct opbx_frame **fo, struct opbx_channel **rc) 
{
	struct opbx_channel *who = NULL;
	enum opbx_bridge_result res = OPBX_BRIDGE_COMPLETE;
	int nativefailed=0;
	int firstpass;
	int o0nativeformats;
	int o1nativeformats;
	long time_left_ms=0;
	struct timeval nexteventts = { 0, };
	char caller_warning = 0;
	char callee_warning = 0;
	int to;

	if (c0->_bridge) {
		opbx_log(LOG_WARNING, "%s is already in a bridge with %s\n", 
			c0->name, c0->_bridge->name);
		return -1;
	}
	if (c1->_bridge) {
		opbx_log(LOG_WARNING, "%s is already in a bridge with %s\n", 
			c1->name, c1->_bridge->name);
		return -1;
	}
	
	/* Stop if we're a zombie or need a soft hangup */
	if (opbx_test_flag(c0, OPBX_FLAG_ZOMBIE) || opbx_check_hangup_locked(c0) ||
	    opbx_test_flag(c1, OPBX_FLAG_ZOMBIE) || opbx_check_hangup_locked(c1)) 
		return -1;

	*fo = NULL;
	firstpass = config->firstpass;
	config->firstpass = 0;

	if (opbx_tvzero(config->start_time))
		config->start_time = opbx_tvnow();
	time_left_ms = config->timelimit;

	caller_warning = opbx_test_flag(&config->features_caller, OPBX_FEATURE_PLAY_WARNING);
	callee_warning = opbx_test_flag(&config->features_callee, OPBX_FEATURE_PLAY_WARNING);

	if (config->start_sound && firstpass) {
		if (caller_warning)
			bridge_playfile(c0, c1, config->start_sound, time_left_ms / 1000);
		if (callee_warning)
			bridge_playfile(c1, c0, config->start_sound, time_left_ms / 1000);
	}

	/* Keep track of bridge */
	c0->_bridge = c1;
	c1->_bridge = c0;
	
	manager_event(EVENT_FLAG_CALL, "Link", 
		      "Channel1: %s\r\n"
		      "Channel2: %s\r\n"
		      "Uniqueid1: %s\r\n"
		      "Uniqueid2: %s\r\n"
		      "CallerID1: %s\r\n"
		      "CallerID2: %s\r\n",
		      c0->name, c1->name, c0->uniqueid, c1->uniqueid, c0->cid.cid_num, c1->cid.cid_num);
                                                                        
	o0nativeformats = c0->nativeformats;
	o1nativeformats = c1->nativeformats;

	if (config->timelimit) {
		nexteventts = opbx_tvadd(config->start_time, opbx_samp2tv(config->timelimit, 1000));
		if (caller_warning || callee_warning)
			nexteventts = opbx_tvsub(nexteventts, opbx_samp2tv(config->play_warning, 1000));
	}

	for (/* ever */;;) {
		to = -1;
		if (config->timelimit) {
			struct timeval now;
			now = opbx_tvnow();
			to = opbx_tvdiff_ms(nexteventts, now);
			if (to < 0)
				to = 0;
			time_left_ms = config->timelimit - opbx_tvdiff_ms(now, config->start_time);
			if (time_left_ms < to)
				to = time_left_ms;

			if (time_left_ms <= 0) {
				if (caller_warning && config->end_sound)
					bridge_playfile(c0, c1, config->end_sound, 0);
				if (callee_warning && config->end_sound)
					bridge_playfile(c1, c0, config->end_sound, 0);
				*fo = NULL;
				if (who) 
					*rc = who;
				res = 0;
				break;
			}
			
			if (!to) {
				if (time_left_ms >= 5000) {
					if (caller_warning && config->warning_sound && config->play_warning)
						bridge_playfile(c0, c1, config->warning_sound, time_left_ms / 1000);
					if (callee_warning && config->warning_sound && config->play_warning)
						bridge_playfile(c1, c0, config->warning_sound, time_left_ms / 1000);
				}
				if (config->warning_freq) {
					nexteventts = opbx_tvadd(nexteventts, opbx_samp2tv(config->warning_freq, 1000));
				} else
					nexteventts = opbx_tvadd(config->start_time, opbx_samp2tv(config->timelimit, 1000));
			}
		}

		if (c0->_softhangup == OPBX_SOFTHANGUP_UNBRIDGE || c1->_softhangup == OPBX_SOFTHANGUP_UNBRIDGE) {
			if (c0->_softhangup == OPBX_SOFTHANGUP_UNBRIDGE)
				c0->_softhangup = 0;
			if (c1->_softhangup == OPBX_SOFTHANGUP_UNBRIDGE)
				c1->_softhangup = 0;
			c0->_bridge = c1;
			c1->_bridge = c0;
			opbx_log(LOG_DEBUG, "Unbridge signal received. Ending native bridge.\n");
			continue;
		}
		
		/* Stop if we're a zombie or need a soft hangup */
		if (opbx_test_flag(c0, OPBX_FLAG_ZOMBIE) || opbx_check_hangup_locked(c0) ||
		    opbx_test_flag(c1, OPBX_FLAG_ZOMBIE) || opbx_check_hangup_locked(c1)) {
			*fo = NULL;
			if (who)
				*rc = who;
			res = 0;
			opbx_log(LOG_DEBUG, "Bridge stops because we're zombie or need a soft hangup: c0=%s, c1=%s, flags: %s,%s,%s,%s\n",
				c0->name, c1->name,
				opbx_test_flag(c0, OPBX_FLAG_ZOMBIE) ? "Yes" : "No",
				opbx_check_hangup(c0) ? "Yes" : "No",
				opbx_test_flag(c1, OPBX_FLAG_ZOMBIE) ? "Yes" : "No",
				opbx_check_hangup(c1) ? "Yes" : "No");
			break;
		}

		if (c0->tech->bridge &&
		    (config->timelimit == 0) &&
		    (c0->tech->bridge == c1->tech->bridge) &&
		    !nativefailed && !c0->monitor && !c1->monitor && !c0->spiers && !c1->spiers) {
			/* Looks like they share a bridge method */
			if (option_verbose > 2) 
				opbx_verbose(VERBOSE_PREFIX_3 "Attempting native bridge of %s and %s\n", c0->name, c1->name);
			opbx_set_flag(c0, OPBX_FLAG_NBRIDGE);
			opbx_set_flag(c1, OPBX_FLAG_NBRIDGE);
			if ((res = c0->tech->bridge(c0, c1, config->flags, fo, rc, to)) == OPBX_BRIDGE_COMPLETE) {
				manager_event(EVENT_FLAG_CALL, "Unlink", 
					      "Channel1: %s\r\n"
					      "Channel2: %s\r\n"
					      "Uniqueid1: %s\r\n"
					      "Uniqueid2: %s\r\n"
					      "CallerID1: %s\r\n"
					      "CallerID2: %s\r\n",
					      c0->name, c1->name, c0->uniqueid, c1->uniqueid, c0->cid.cid_num, c1->cid.cid_num);
				opbx_log(LOG_DEBUG, "Returning from native bridge, channels: %s, %s\n", c0->name, c1->name);

				opbx_clear_flag(c0, OPBX_FLAG_NBRIDGE);
				opbx_clear_flag(c1, OPBX_FLAG_NBRIDGE);

				if (c0->_softhangup == OPBX_SOFTHANGUP_UNBRIDGE || c1->_softhangup == OPBX_SOFTHANGUP_UNBRIDGE)
					continue;

				c0->_bridge = NULL;
				c1->_bridge = NULL;

				return res;
			} else {
				opbx_clear_flag(c0, OPBX_FLAG_NBRIDGE);
				opbx_clear_flag(c1, OPBX_FLAG_NBRIDGE);
			}
			if (res == OPBX_BRIDGE_RETRY)
				continue;
			switch (res) {
			case OPBX_BRIDGE_RETRY:
/*				continue; */
				break;
			default:
				opbx_log(LOG_WARNING, "Private bridge between %s and %s failed\n", c0->name, c1->name);
				/* fallthrough */
			case OPBX_BRIDGE_FAILED_NOWARN:
				nativefailed++;
				break;
			}
		}
	
		if (((c0->writeformat != c1->readformat) || (c0->readformat != c1->writeformat) ||
		    (c0->nativeformats != o0nativeformats) || (c1->nativeformats != o1nativeformats)) &&
		    !(opbx_generator_is_active(c0) || opbx_generator_is_active(c1))) {
			if (opbx_channel_make_compatible(c0, c1)) {
				opbx_log(LOG_WARNING, "Can't make %s and %s compatible\n", c0->name, c1->name);
                                manager_event(EVENT_FLAG_CALL, "Unlink",
					      "Channel1: %s\r\n"
					      "Channel2: %s\r\n"
					      "Uniqueid1: %s\r\n"
					      "Uniqueid2: %s\r\n"
					      "CallerID1: %s\r\n"
					      "CallerID2: %s\r\n",
					      c0->name, c1->name, c0->uniqueid, c1->uniqueid, c0->cid.cid_num, c1->cid.cid_num);
				return OPBX_BRIDGE_FAILED;
			}
			o0nativeformats = c0->nativeformats;
			o1nativeformats = c1->nativeformats;
		}
		res = opbx_generic_bridge(c0, c1, config, fo, rc, to);
		if (res != OPBX_BRIDGE_RETRY)
			break;
	}

	c0->_bridge = NULL;
	c1->_bridge = NULL;

	manager_event(EVENT_FLAG_CALL, "Unlink",
		      "Channel1: %s\r\n"
		      "Channel2: %s\r\n"
		      "Uniqueid1: %s\r\n"
		      "Uniqueid2: %s\r\n"
		      "CallerID1: %s\r\n"
		      "CallerID2: %s\r\n",
		      c0->name, c1->name, c0->uniqueid, c1->uniqueid, c0->cid.cid_num, c1->cid.cid_num);
	opbx_log(LOG_DEBUG, "Bridge stops bridging channels %s and %s\n", c0->name, c1->name);

	return res;
}

/*--- opbx_channel_setoption: Sets an option on a channel */
int opbx_channel_setoption(struct opbx_channel *chan, int option, void *data, int datalen, int block)
{
	int res;

	if (chan->tech->setoption) {
		res = chan->tech->setoption(chan, option, data, datalen);
		if (res < 0)
			return res;
	} else {
		errno = ENOSYS;
		return -1;
	}
	if (block) {
		/* XXX Implement blocking -- just wait for our option frame reply, discarding
		   intermediate packets. XXX */
		opbx_log(LOG_ERROR, "XXX Blocking not implemented yet XXX\n");
		return -1;
	}
	return 0;
}

struct tonepair_def {
	int freq1;
	int freq2;
	int duration;
	int vol;
};

struct tonepair_state {
	float freq1;
	float freq2;
	float vol;
	int duration;
	int pos;
	int origwfmt;
	struct opbx_frame f;
	unsigned char offset[OPBX_FRIENDLY_OFFSET];
	short data[4000];
};

static void tonepair_release(struct opbx_channel *chan, void *params)
{
	struct tonepair_state *ts = params;

	if (chan) {
		opbx_set_write_format(chan, ts->origwfmt);
	}
	free(ts);
}

static void *tonepair_alloc(struct opbx_channel *chan, void *params)
{
	struct tonepair_state *ts;
	struct tonepair_def *td = params;

	ts = malloc(sizeof(struct tonepair_state));
	if (!ts)
		return NULL;
	memset(ts, 0, sizeof(struct tonepair_state));
	ts->origwfmt = chan->writeformat;
	if (opbx_set_write_format(chan, OPBX_FORMAT_SLINEAR)) {
		opbx_log(LOG_WARNING, "Unable to set '%s' to signed linear format (write)\n", chan->name);
		tonepair_release(NULL, ts);
		ts = NULL;
	} else {
		ts->freq1 = td->freq1;
		ts->freq2 = td->freq2;
		ts->duration = td->duration;
		ts->vol = td->vol;
	}
	/* Let interrupts interrupt :) */
	opbx_set_flag(chan, OPBX_FLAG_WRITE_INT);
	return ts;
}

static int tonepair_generate(struct opbx_channel *chan, void *data, int samples)
{
	struct tonepair_state *ts = data;
	int len, x;

	/* we need to prepare a frame with 16 * timelen samples as we're 
	 * generating SLIN audio
	 */
	len = samples * 2;

	if (len > sizeof(ts->data) / 2 - 1) {
		opbx_log(LOG_WARNING, "Can't generate that much data!\n");
		return -1;
	}
	memset(&ts->f, 0, sizeof(ts->f));
	for (x = 0; x < (len / 2); x++) {
		ts->data[x] = ts->vol * (
				sin((ts->freq1 * 2.0 * M_PI / 8000.0) * (ts->pos + x)) +
				sin((ts->freq2 * 2.0 * M_PI / 8000.0) * (ts->pos + x))
			);
	}
	ts->f.frametype = OPBX_FRAME_VOICE;
	ts->f.subclass = OPBX_FORMAT_SLINEAR;
	ts->f.datalen = len;
	ts->f.samples = samples;
	ts->f.offset = OPBX_FRIENDLY_OFFSET;
	ts->f.data = ts->data;
	opbx_write(chan, &ts->f);
	ts->pos += x;
	if (ts->duration > 0) {
		if (ts->pos >= ts->duration * 8)
			return -1;
	}
	return 0;
}

static struct opbx_generator tonepair = {
	alloc: tonepair_alloc,
	release: tonepair_release,
	generate: tonepair_generate,
};

int opbx_tonepair_start(struct opbx_channel *chan, int freq1, int freq2, int duration, int vol)
{
	struct tonepair_def d = { 0, };

	d.freq1 = freq1;
	d.freq2 = freq2;
	d.duration = duration;
	if (vol < 1)
		d.vol = 8192;
	else
		d.vol = vol;
	if (opbx_generator_activate(chan, &tonepair, &d))
		return -1;
	return 0;
}

void opbx_tonepair_stop(struct opbx_channel *chan)
{
	opbx_generator_deactivate(chan);
}

int opbx_tonepair(struct opbx_channel *chan, int freq1, int freq2, int duration, int vol)
{
	int res;

	if ((res = opbx_tonepair_start(chan, freq1, freq2, duration, vol)))
		return res;

	/* Don't return to caller until after duration has passed */
	return opbx_safe_sleep(chan, duration);
}

opbx_group_t opbx_get_group(char *s)
{
	char *copy;
	char *piece;
	char *c=NULL;
	int start=0, finish=0, x;
	opbx_group_t group = 0;

	copy = opbx_strdupa(s);
	if (!copy) {
		opbx_log(LOG_ERROR, "Out of memory\n");
		return 0;
	}
	c = copy;
	
	while((piece = strsep(&c, ","))) {
		if (sscanf(piece, "%d-%d", &start, &finish) == 2) {
			/* Range */
		} else if (sscanf(piece, "%d", &start)) {
			/* Just one */
			finish = start;
		} else {
			opbx_log(LOG_ERROR, "Syntax error parsing group configuration '%s' at '%s'. Ignoring.\n", s, piece);
			continue;
		}
		for (x = start; x <= finish; x++) {
			if ((x > 63) || (x < 0)) {
				opbx_log(LOG_WARNING, "Ignoring invalid group %d (maximum group is 63)\n", x);
			} else
				group |= ((opbx_group_t) 1 << x);
		}
	}
	return group;
}

static int (*opbx_moh_start_ptr)(struct opbx_channel *, char *) = NULL;
static void (*opbx_moh_stop_ptr)(struct opbx_channel *) = NULL;
static void (*opbx_moh_cleanup_ptr)(struct opbx_channel *) = NULL;


void opbx_install_music_functions(int (*start_ptr)(struct opbx_channel *, char *),
								 void (*stop_ptr)(struct opbx_channel *),
								 void (*cleanup_ptr)(struct opbx_channel *)
								 ) 
{
	opbx_moh_start_ptr = start_ptr;
	opbx_moh_stop_ptr = stop_ptr;
	opbx_moh_cleanup_ptr = cleanup_ptr;
}

void opbx_uninstall_music_functions(void) 
{
	opbx_moh_start_ptr = NULL;
	opbx_moh_stop_ptr = NULL;
	opbx_moh_cleanup_ptr = NULL;
}

/*! Turn on music on hold on a given channel */
int opbx_moh_start(struct opbx_channel *chan, char *mclass) 
{
	if (opbx_moh_start_ptr)
		return opbx_moh_start_ptr(chan, mclass);

	if (option_verbose > 2)
		opbx_verbose(VERBOSE_PREFIX_3 "Music class %s requested but no musiconhold loaded.\n", mclass ? mclass : "default");
	
	return 0;
}

/*! Turn off music on hold on a given channel */
void opbx_moh_stop(struct opbx_channel *chan) 
{
	if(opbx_moh_stop_ptr)
		opbx_moh_stop_ptr(chan);
}

void opbx_moh_cleanup(struct opbx_channel *chan) 
{
	if(opbx_moh_cleanup_ptr)
        opbx_moh_cleanup_ptr(chan);
}

void opbx_channels_init(void)
{
	opbx_cli_register(&cli_show_channeltypes);
}

/*--- opbx_print_group: Print call group and pickup group ---*/
char *opbx_print_group(char *buf, int buflen, opbx_group_t group) 
{
	unsigned int i;
	int first=1;
	char num[3];

	buf[0] = '\0';
	
	if (!group)	/* Return empty string if no group */
		return(buf);

	for (i=0; i<=63; i++) {	/* Max group is 63 */
		if (group & ((opbx_group_t) 1 << i)) {
	   		if (!first) {
				strncat(buf, ", ", buflen);
			} else {
				first=0;
	  		}
			snprintf(num, sizeof(num), "%u", i);
			strncat(buf, num, buflen);
		}
	}
	return(buf);
}

void opbx_set_variables(struct opbx_channel *chan, struct opbx_variable *vars)
{
	struct opbx_variable *cur;

	for (cur = vars; cur; cur = cur->next)
		pbx_builtin_setvar_helper(chan, cur->name, cur->value);	
}

/* If you are calling carefulwrite, it is assumed that you are calling
   it on a file descriptor that _DOES_ have NONBLOCK set.  This way,
   there is only one system call made to do a write, unless we actually
   have a need to wait.  This way, we get better performance. */
int opbx_carefulwrite(int fd, char *s, int len, int timeoutms) 
{
	/* Try to write string, but wait no more than ms milliseconds
	   before timing out */
	int res=0;
	struct pollfd fds[1];
	while(len) {
		res = write(fd, s, len);
		if ((res < 0) && (errno != EAGAIN)) {
			return -1;
		}
		if (res < 0) res = 0;
		len -= res;
		s += res;
		fds[0].fd = fd;
		fds[0].events = POLLOUT;
		/* Wait until writable again */
		res = poll(fds, 1, timeoutms);
		if (res < 1)
			return -1;
	}
	return res;
}
