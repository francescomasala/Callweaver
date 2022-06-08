/*
 * (SCCP*)
 *
 * An implementation of Skinny Client Control Protocol (SCCP)
 *
 * Sergio Chersovani (mlists@c-net.it)
 *
 * Reworked, but based on chan_sccp code.
 * The original chan_sccp driver that was made by Zozo which itself was derived from the chan_skinny driver.
 * Modified by Jan Czmok and Julien Goodwin
 *
 * This program is free software and may be modified and
 * distributed under the terms of the GNU Public License.
 */

#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include "chan_sccp.h"
#include "sccp_pbx.h"
#include "sccp_utils.h"
#include "sccp_device.h"
#include "sccp_channel.h"
#include "sccp_indicate.h"

#include <openpbx/pbx.h>
#include <openpbx/old_callerid.h>
#include <openpbx/utils.h>
#include <openpbx/causes.h>

static struct opbx_frame * sccp_rtp_read(sccp_channel_t * c) {
	/* Retrieve audio/etc from channel.  Assumes c->lock is already held. */
	struct opbx_frame * f;
	/* if c->rtp not null */
	if (!c->rtp)
		return NULL;

	f = opbx_rtp_read(c->rtp);

	if (c->owner) {
		/* We already hold the channel lock */
		if (f->frametype == OPBX_FRAME_VOICE) {
			if (f->subclass != c->owner->nativeformats) {
				sccp_log(10)(VERBOSE_PREFIX_3 "%s: Oooh, format changed to %d from %d on channel %d\n", c->device->id, f->subclass, c->owner->nativeformats, c->callid);
				c->owner->nativeformats = f->subclass;
				opbx_set_read_format(c->owner, c->owner->readformat);
				opbx_set_write_format(c->owner, c->owner->writeformat);
			}
		}
	}
	return f;
}

static void * sccp_pbx_call_autoanswer_thread(void *data) {
	uint32_t *tmp = data;
	uint32_t callid = *tmp;
	sccp_channel_t * c;

	sleep(GLOB(autoanswer_ring_time));
	pthread_testcancel();
	c = sccp_channel_find_byid(callid);
	if (!c || !c->device)
		return NULL;
	if (c->state != SCCP_CHANNELSTATE_RINGIN)
		return NULL;

	sccp_channel_answer(c);
	if (GLOB(autoanswer_tone) != SKINNY_TONE_SILENCE && GLOB(autoanswer_tone) != SKINNY_TONE_NOTONE)
		sccp_dev_starttone(c->device, GLOB(autoanswer_tone), c->line->instance, c->callid, 0);
	if (c->autoanswer_type == SCCP_AUTOANSWER_1W)
		sccp_dev_set_microphone(c->device, SKINNY_STATIONMIC_OFF);

	return NULL;
}


/* this is for incoming calls openpbx sccp_request */
static int sccp_pbx_call(struct opbx_channel *ast, char *dest, int timeout) {
	sccp_line_t	 * l;
	sccp_device_t  * d;
	sccp_session_t * s;
	sccp_channel_t * c;
	char * ringermode = NULL;
	pthread_attr_t attr;
	pthread_t t;

	c = CS_OPBX_CHANNEL_PVT(ast);

	if (!c) {
		opbx_log(LOG_WARNING, "SCCP: Asterisk request to call %s channel: %s, but we don't have this channel!\n", dest, ast->name);
		return -1;
	}

	l = c->line;
	d = l->device;
	s = d->session;

	if (!l || !d || !s) {
		opbx_log(LOG_WARNING, "SCCP: weird error. The channel %d has no line or device or session\n", (c ? c->callid : 0) );
		return -1;
	}

	sccp_log(1)(VERBOSE_PREFIX_3 "%s: Asterisk request to call %s\n", d->id, ast->name);

	opbx_mutex_lock(&d->lock);

	if (d->dnd) {
		if (d->dndmode == SCCP_DNDMODE_REJECT) {
			opbx_mutex_unlock(&d->lock);
			opbx_setstate(ast, OPBX_STATE_BUSY);
			opbx_queue_control(ast, OPBX_CONTROL_BUSY);
			sccp_log(1)(VERBOSE_PREFIX_3 "%s: DND is on. Call %s rejected\n", d->id, ast->name);
			return 0;
		} else if (d->dndmode == SCCP_DNDMODE_SILENT) {
			/* disable the ringer and autoanswer options */
			c->ringermode = SKINNY_STATION_SILENTRING;
			c->autoanswer_type = SCCP_AUTOANSWER_NONE;
			sccp_log(1)(VERBOSE_PREFIX_3 "%s: DND (silent) is on. Set the ringer = silent and autoanswer = off for %s\n", d->id, ast->name);
		}
	}

	/* if incoming call limit is reached send BUSY */
	opbx_mutex_lock(&l->lock);
	if ( l->channelCount > l->incominglimit ) { /* >= just to be sure :-) */
		sccp_log(1)(VERBOSE_PREFIX_3 "Incoming calls limit (%d) reached on SCCP/%s... sending busy\n", l->incominglimit, l->name);
		opbx_mutex_unlock(&l->lock);
		opbx_mutex_unlock(&d->lock);
		opbx_setstate(ast, OPBX_STATE_BUSY);
		opbx_queue_control(ast, OPBX_CONTROL_BUSY);
		return 0;
	}
	opbx_mutex_unlock(&l->lock);

	/* autoanswer check */
	if (c->autoanswer_type) {
		if (d->channelCount > 1) {
			sccp_log(1)(VERBOSE_PREFIX_3 "%s: Autoanswer requested, but the device is in use\n", d->id);
			c->autoanswer_type = SCCP_AUTOANSWER_NONE;
			if (c->autoanswer_cause) {
				switch (c->autoanswer_cause) {
					case OPBX_CAUSE_CONGESTION:
						opbx_queue_control(ast, OPBX_CONTROL_CONGESTION);
						break;
					default:
						opbx_queue_control(ast, OPBX_CONTROL_BUSY);
						break;
				}
				opbx_mutex_unlock(&d->lock);
				return 0;
			}
		} else {
			sccp_log(1)(VERBOSE_PREFIX_3 "%s: Autoanswer requested and activated %s\n", d->id, (c->autoanswer_type == SCCP_AUTOANSWER_1W) ? "with MIC OFF" : "with MIC ON");
		}
	}
	opbx_mutex_unlock(&d->lock);

  /* Set the channel callingParty Name and Number */
	sccp_channel_set_callingparty(c, ast->cid.cid_name, ast->cid.cid_num);

  /* Set the channel calledParty Name and Number 7910 compatibility*/
	sccp_channel_set_calledparty(c, l->cid_name, l->cid_num);

	if (!c->ringermode) {
		c->ringermode = SKINNY_STATION_OUTSIDERING;
		ringermode = pbx_builtin_getvar_helper(ast, "ALERT_INFO");
	}

	if ( ringermode && !opbx_strlen_zero(ringermode) ) {
		sccp_log(1)(VERBOSE_PREFIX_3 "%s: Found ALERT_INFO=%s\n", d->id, ringermode);
		if (strcasecmp(ringermode, "inside") == 0)
			c->ringermode = SKINNY_STATION_INSIDERING;
		else if (strcasecmp(ringermode, "feature") == 0)
			c->ringermode = SKINNY_STATION_FEATURERING;
		else if (strcasecmp(ringermode, "silent") == 0)
			c->ringermode = SKINNY_STATION_SILENTRING;
		else if (strcasecmp(ringermode, "urgent") == 0)
			c->ringermode = SKINNY_STATION_URGENTRING;
	}

	/* release the openpbx lock */
	opbx_mutex_unlock(&ast->lock);
	if ( sccp_channel_get_active(d) ) {
		sccp_indicate_lock(c, SCCP_CHANNELSTATE_CALLWAITING);
		opbx_queue_control(ast, OPBX_CONTROL_RINGING);
	} else {
		sccp_indicate_lock(c, SCCP_CHANNELSTATE_RINGIN);
		opbx_queue_control(ast, OPBX_CONTROL_RINGING);
		if (c->autoanswer_type) {
			sccp_log(1)(VERBOSE_PREFIX_3 "%s: Running the autoanswer thread on %s\n", d->id, ast->name);
			pthread_attr_init(&attr);
			pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
			if (opbx_pthread_create(&t, &attr, sccp_pbx_call_autoanswer_thread, &c->callid)) {
				opbx_log(LOG_WARNING, "%s: Unable to create switch thread for channel (%s-%d) %s\n", d->id, l->name, c->callid, strerror(errno));
			}
		}
	}
	return 0;
}


static int sccp_pbx_hangup(struct opbx_channel * ast) {
	sccp_channel_t * c;
	sccp_line_t    * l;
	sccp_device_t  * d;
	int res = 0;

	c = CS_OPBX_CHANNEL_PVT(ast);

	opbx_mutex_lock(&GLOB(usecnt_lock));
	GLOB(usecnt)--;
	opbx_mutex_unlock(&GLOB(usecnt_lock));

	opbx_update_use_count();

	if (!c) {
		sccp_log(10)(VERBOSE_PREFIX_3 "SCCP: Asked to hangup channel %s. SCCP channel already hangup\n", ast->name);
		return 0;
	}

	opbx_mutex_lock(&c->lock);
	CS_OPBX_CHANNEL_PVT(ast) = NULL;
	c->owner = NULL;
	l = c->line;
	d = l->device;

	sccp_log(1)(VERBOSE_PREFIX_3 "SCCP: Asterisk request to hangup %s channel %s\n", skinny_calltype2str(c->calltype), ast->name);

	if (c->rtp) {
		sccp_channel_closereceivechannel(c);
		sccp_channel_stop_rtp(c);
	}

	sccp_log(10)(VERBOSE_PREFIX_3 "%s: Current channel %s-%d state %s(%d)\n", DEV_ID_LOG(d), l ? l->name : "(null)", c->callid, sccp_indicate2str(c->state), c->state);

	if ( c->state != SCCP_CHANNELSTATE_DOWN) {
		/* we are in a passive hangup */
		if (GLOB(remotehangup_tone) && d->state == SCCP_DEVICESTATE_OFFHOOK && c == sccp_channel_get_active(d))
			sccp_dev_starttone(d, GLOB(remotehangup_tone), 0, 0, 10);
		sccp_indicate_nolock(c, SCCP_CHANNELSTATE_ONHOOK);
	}

	if (c->calltype == SKINNY_CALLTYPE_OUTBOUND && !c->hangupok) {
		opbx_mutex_unlock(&c->lock);
		sccp_log(10)(VERBOSE_PREFIX_3 "%s: Waiting for the dialing thread to go down on channel %s\n", DEV_ID_LOG(d), ast->name);
		do {
			usleep(10000);
			opbx_mutex_lock(&c->lock);
			res = c->hangupok;
			opbx_mutex_unlock(&c->lock);
		} while (!res);
	} else {
		opbx_mutex_unlock(&c->lock);
	}

	opbx_mutex_lock(&GLOB(channels_lock));
	sccp_channel_delete(c);
	opbx_mutex_unlock(&GLOB(channels_lock));

	if (d && d->session) {
		opbx_mutex_lock(&d->session->lock);
		d->session->needcheckringback = 1;
		opbx_mutex_unlock(&d->session->lock);
	}

	return 0;
}

static int sccp_pbx_answer(struct opbx_channel *ast) {
	sccp_channel_t * c = CS_OPBX_CHANNEL_PVT(ast);

	if (!c || !c->device || !c->line) {
		opbx_log(LOG_ERROR, "SCCP: Answered %s but no SCCP channel\n", ast->name);
		return -1;
	}

	sccp_log(1)(VERBOSE_PREFIX_3 "SCCP: Outgoing call has been answered %s on %s@%s-%d\n", ast->name, c->line->name, c->device->id, c->callid);
	sccp_indicate_lock(c, SCCP_CHANNELSTATE_CONNECTED);
	return 0;
}


static struct opbx_frame * sccp_pbx_read(struct opbx_channel *ast) {
	sccp_channel_t * c = CS_OPBX_CHANNEL_PVT(ast);
	if (!c)
		return NULL;
	struct opbx_frame *fr;
	opbx_mutex_lock(&c->lock);
	fr = sccp_rtp_read(c);
	opbx_mutex_unlock(&c->lock);
	return fr;
}

static int sccp_pbx_write(struct opbx_channel *ast, struct opbx_frame *frame) {
	sccp_channel_t * c = CS_OPBX_CHANNEL_PVT(ast);

	if (!c)
		return 0;

	int res = 0;
	if (frame->frametype != OPBX_FRAME_VOICE) {
		if (frame->frametype == OPBX_FRAME_IMAGE)
			return 0;
		else {
			opbx_log(LOG_WARNING, "%s: Can't send %d type frames with SCCP write on channel %d\n", DEV_ID_LOG(c->device), frame->frametype, (c) ? c->callid : 0);
			return 0;
		}
	} else {
		if (!(frame->subclass & ast->nativeformats)) {
			opbx_log(LOG_WARNING, "%s: Asked to transmit frame type %d, while native formats is %d (read/write = %d/%d)\n",
			DEV_ID_LOG(c->device), frame->subclass, ast->nativeformats, ast->readformat, ast->writeformat);
			return -1;
		}
	}
	if (c) {
		opbx_mutex_lock(&c->lock);
		if (c->rtp) {
			res =  opbx_rtp_write(c->rtp, frame);
		}
		opbx_mutex_unlock(&c->lock);
	}
	return res;
}

static char *sccp_control2str(int state) {
		switch(state) {
		case OPBX_CONTROL_HANGUP:
				return "Hangup";
		case OPBX_CONTROL_RING:
				return "Ring";
		case OPBX_CONTROL_RINGING:
				return "Ringing";
		case OPBX_CONTROL_ANSWER:
				return "Answer";
		case OPBX_CONTROL_BUSY:
				return "Busy";
		case OPBX_CONTROL_TAKEOFFHOOK:
				return "TakeOffHook";
		case OPBX_CONTROL_OFFHOOK:
				return "OffHook";
		case OPBX_CONTROL_CONGESTION:
				return "Congestion";
		case OPBX_CONTROL_FLASH:
				return "Flash";
		case OPBX_CONTROL_WINK:
				return "Wink";
		case OPBX_CONTROL_OPTION:
				return "Option";
		case OPBX_CONTROL_RADIO_KEY:
				return "RadioKey";
		case OPBX_CONTROL_RADIO_UNKEY:
				return "RadioUnKey";
		case OPBX_CONTROL_PROGRESS:
				return "Progress";
		case OPBX_CONTROL_PROCEEDING:
				return "Proceeding";
		case OPBX_CONTROL_HOLD:
				return "Hold";
		case OPBX_CONTROL_UNHOLD:
				return "UnHold";
		default:
				return "Unknown";
		}
}

static int sccp_pbx_indicate(struct opbx_channel *ast, int ind) {
	sccp_channel_t * c = CS_OPBX_CHANNEL_PVT(ast);
	int res = 0;
	if (!c)
		return -1;

	opbx_mutex_lock(&c->lock);
	sccp_log(10)(VERBOSE_PREFIX_3 "%s: Asterisk indicate '%d' (%s) condition on channel %s\n", DEV_ID_LOG(c->device), ind, sccp_control2str(ind), ast->name);
	if (c->state == SCCP_CHANNELSTATE_CONNECTED) {
		/* let's openpbx emulate it */
		opbx_mutex_unlock(&c->lock);
		return -1;

	}
	
	/* when the rtp media stream is open we will let openpbx emulate the tones */
	if (c->rtp)
		res = -1;

	switch(ind) {

	case OPBX_CONTROL_RINGING:
		sccp_indicate_nolock(c, SCCP_CHANNELSTATE_RINGOUT);
		break;

	case OPBX_CONTROL_BUSY:
		sccp_indicate_nolock(c, SCCP_CHANNELSTATE_BUSY);
		break;

	case OPBX_CONTROL_CONGESTION:
		sccp_indicate_nolock(c, SCCP_CHANNELSTATE_CONGESTION);
		break;

	case OPBX_CONTROL_PROGRESS:
	case OPBX_CONTROL_PROCEEDING:
		sccp_indicate_nolock(c, SCCP_CHANNELSTATE_PROCEED);
		res = 0;
		break;

/* when the bridged channel hold/unhold the call we are notified here */
	case OPBX_CONTROL_HOLD:
		res = 0;
		break;
	case OPBX_CONTROL_UNHOLD:
		res = 0;
		break;

	case -1:
		break;

	default:
	  opbx_log(LOG_WARNING, "SCCP: Don't know how to indicate condition %d\n", ind);
	  res = -1;
	}

	opbx_mutex_unlock(&c->lock);
	return res;
}

static int sccp_pbx_fixup(struct opbx_channel *oldchan, struct opbx_channel *newchan) {
	sccp_channel_t * c = CS_OPBX_CHANNEL_PVT(newchan);
	if (!c) {
		opbx_log(LOG_WARNING, "sccp_pbx_fixup(old: %s(%p), new: %s(%p)). no SCCP channel to fix\n", oldchan->name, oldchan, newchan->name, newchan);
		return -1;
	}
	opbx_mutex_lock(&c->lock);
	if (c->owner != oldchan) {
		opbx_log(LOG_WARNING, "old channel wasn't %p but was %p\n", oldchan, c->owner);
		opbx_mutex_unlock(&c->lock);
		return -1;
	}
	c->owner = newchan;
	opbx_mutex_unlock(&c->lock);
	return 0;
}

static int sccp_pbx_recvdigit(struct opbx_channel *ast, char digit) {
	sccp_channel_t * c = CS_OPBX_CHANNEL_PVT(ast);
	sccp_device_t * d = NULL;

	if (!c || !c->device)
		return -1;

	d = c->device;

	sccp_log(1)(VERBOSE_PREFIX_3 "SCCP: Asterisk asked to send dtmf '%d' to channel %s. Trying to send it %s\n", digit, ast->name, (d->dtmfmode) ? "outofband" : "inband");

	if (c->state != SCCP_CHANNELSTATE_CONNECTED) {
		sccp_log(1)(VERBOSE_PREFIX_3 "%s: Can't send the dtmf '%d' %c to a not connected channel %s\n", d->id, digit, digit, ast->name);
		return -1;
	}

	if (d->dtmfmode == SCCP_DTMFMODE_INBAND)
		return -1 ;

	if (digit == '*') {
		digit = 0xe; /* See the definition of tone_list in chan_protocol.h for more info */
	} else if (digit == '#') {
		digit = 0xf;
	} else if (digit == '0') {
		digit = 0xa; /* 0 is not 0 for cisco :-) */
	} else {
		digit -= '0';
	}

	if (digit > 16) {
		sccp_log(1)(VERBOSE_PREFIX_3 "%s: Cisco phones can't play this type of dtmf. Sinding it inband\n", d->id);
		return -1;
	}
	sccp_dev_starttone(c->device, (uint8_t) digit, c->line->instance, c->callid, 0);
	return 0;
}

static int sccp_pbx_sendtext(struct opbx_channel *ast, const char *text) {
	sccp_channel_t * c = CS_OPBX_CHANNEL_PVT(ast);
	sccp_device_t * d;

	if (!c || !c->device)
		return -1;

	d = c->device;
	if (!text || opbx_strlen_zero(text))
		return 0;

	sccp_log(1)(VERBOSE_PREFIX_3 "%s: Sending text %s on %s\n", d->id, text, ast->name);
	sccp_dev_displayprompt(d, c->line->instance, c->callid, (char *)text, 10);
	return 0;
}

const struct opbx_channel_tech sccp_tech = {
	.type = "SCCP",
	.description = "Skinny Client Control Protocol (SCCP)",
	.capabilities = OPBX_FORMAT_ALAW|OPBX_FORMAT_ULAW|OPBX_FORMAT_G729A,
	.requester = sccp_request,
	.devicestate = sccp_devicestate,
	.call = sccp_pbx_call,
	.hangup = sccp_pbx_hangup,
	.answer = sccp_pbx_answer,
	.read = sccp_pbx_read,
	.write = sccp_pbx_write,
	.indicate = sccp_pbx_indicate,
	.fixup = sccp_pbx_fixup,
	.send_digit = sccp_pbx_recvdigit,
	.send_text = sccp_pbx_sendtext,
/*	.bridge = opbx_rtp_bridge */
};

uint8_t sccp_pbx_channel_allocate(sccp_channel_t * c) {
	sccp_device_t * d = c->device;
	struct opbx_channel * tmp;
	sccp_line_t * l = c->line;
	int fmt;

	if (!l || !d || !d->session) {
		opbx_log(LOG_ERROR, "SCCP: Unable to allocate asterisk channel\n");
		return 0;
	}

	tmp = opbx_channel_alloc(1);
	if (!tmp) {
		opbx_log(LOG_ERROR, "%s: Unable to allocate openpbx channel on line %s\n", d->id, l->name);
		return 0;
	}

	/* need to reset the exten, otherwise it would be set to s */
	memset(&tmp->exten,0,sizeof(tmp->exten));

	/* let's connect the ast channel to the sccp channel */
	opbx_mutex_lock(&c->lock);
	c->owner = tmp;
	opbx_mutex_unlock(&c->lock);
	sccp_log(10)(VERBOSE_PREFIX_3 "%s: Global Capabilities: %d\n", d->id, GLOB(global_capability));

	opbx_mutex_lock(&l->lock);
	opbx_mutex_lock(&d->lock);
	tmp->nativeformats = (d->capability ? d->capability : GLOB(global_capability));
	if (tmp->nativeformats & c->format) {
		fmt = c->format;
	} else {
		fmt = opbx_codec_choose(&d->codecs, tmp->nativeformats, 1);
		c->format = fmt;
	}
	opbx_mutex_unlock(&l->lock);
	opbx_mutex_unlock(&d->lock);
	sccp_log(2)(VERBOSE_PREFIX_3 "%s: format request: %d/%d\n", d->id, tmp->nativeformats, c->format);

	snprintf(tmp->name, sizeof(tmp->name), "SCCP/%s-%08x", l->name, c->callid);

	if (GLOB(debug) > 2) {
	  const unsigned slen=512;
	  char s1[slen];
	  char s2[slen];
	  sccp_log(2)(VERBOSE_PREFIX_3 "%s: Channel %s, capabilities: DEVICE %s NATIVE %s BEST %d (%s)\n",
		d->id,
		c->owner->name,
		opbx_getformatname_multiple(s1, slen, d->capability),
		opbx_getformatname_multiple(s2, slen, tmp->nativeformats),
		fmt, opbx_getformatname(fmt));
	}

	tmp->type = "SCCP";
	tmp->nativeformats		= fmt;
	tmp->writeformat		= fmt;
	tmp->readformat 		= fmt;

	tmp->tech				 = &sccp_tech;
	tmp->tech_pvt			 = c;

	tmp->adsicpe		 = OPBX_ADSI_UNAVAILABLE;

	// XXX: Bridge?
	// XXX: Transfer?

	opbx_mutex_lock(&GLOB(usecnt_lock));
	GLOB(usecnt)++;
	opbx_mutex_unlock(&GLOB(usecnt_lock));

	opbx_update_use_count();

	if (l->cid_num)
	  tmp->cid.cid_num = strdup(l->cid_num);
	if (l->cid_name)
	  tmp->cid.cid_name = strdup(l->cid_name);

	opbx_copy_string(tmp->context, l->context, sizeof(tmp->context));
	if (!opbx_strlen_zero(l->language))
		opbx_copy_string(tmp->language, l->language, sizeof(tmp->language));
	if (!opbx_strlen_zero(l->accountcode))
		opbx_copy_string(tmp->accountcode, l->accountcode, sizeof(tmp->accountcode));
	if (!opbx_strlen_zero(l->musicclass))
		opbx_copy_string(tmp->musicclass, l->musicclass, sizeof(tmp->musicclass));
	tmp->amaflags = l->amaflags;
	tmp->callgroup = l->callgroup;
#ifdef CS_SCCP_PICKUP
	tmp->pickupgroup = l->pickupgroup;
#endif
	tmp->priority = 1;
	sccp_log(10)(VERBOSE_PREFIX_3 "%s: Allocated openpbx channel %s-%d\n", d->id, l->name, c->callid);
	return 1;
}

void * sccp_pbx_startchannel(void *data) {
	struct opbx_channel * chan = data;
	sccp_channel_t * c;
	sccp_line_t * l;
	sccp_device_t * d;
	uint8_t res_exten = 0, res_wait = 0, res_timeout = 0;

	c = CS_OPBX_CHANNEL_PVT(chan);
	if ( !c || !(l = c->line) || !(d = c->device) ) {
		opbx_hangup(chan);
		return NULL;
	}

	sccp_log(1)( VERBOSE_PREFIX_3 "%s: New call on line %s\n", d->id, l->name);

	opbx_mutex_lock(&c->lock);
	c->calltype = SKINNY_CALLTYPE_OUTBOUND;
	c->hangupok = 0;
	opbx_mutex_unlock(&c->lock);
	
	sccp_channel_set_callingparty(c, l->cid_name, l->cid_num);

	if (!opbx_strlen_zero(c->dialedNumber)) {
		/* we have a number to dial. Let's do it */
		sccp_log(10)( VERBOSE_PREFIX_3 "%s: Dialing %s on channel %s-%d\n", l->device->id, c->dialedNumber, l->name, c->callid);
		sccp_channel_set_calledparty(c, c->dialedNumber, c->dialedNumber);
		sccp_indicate_lock(c, SCCP_CHANNELSTATE_DIALING);
		goto dial;
	}

	/* we have to collect the number */
	/* the phone is on TsOffHook state */
	sccp_log(10)( VERBOSE_PREFIX_3 "%s: Waiting for the number to dial on channel %s-%d\n", l->device->id, l->name, c->callid);
	/* let's use the keypad to collect digits */
	opbx_mutex_lock(&c->lock);
	c->digittimeout = time(0)+GLOB(firstdigittimeout);
	opbx_mutex_unlock(&c->lock);

	res_exten = 1;

	do {
		pthread_testcancel();
		usleep(100);
		opbx_mutex_lock(&c->lock);
		if (!opbx_strlen_zero(c->dialedNumber)) {
			res_exten = (c->dialedNumber[0] == '*' || opbx_matchmore_extension(chan, chan->context, c->dialedNumber, 1, l->cid_num));
		}
		if (! (res_wait = ( c->state == SCCP_CHANNELSTATE_DOWN || chan->_state == OPBX_STATE_DOWN
							|| chan->_softhangup || c->calltype == SKINNY_CALLTYPE_INBOUND)) ) {
			if (CS_OPBX_CHANNEL_PVT(chan)) {
				res_timeout = (time(0) < c->digittimeout);
			} else
				res_timeout = 0;
		}
		opbx_mutex_unlock(&c->lock);
	} while ( (res_wait == 0) && res_exten &&  res_timeout);

	if (res_wait != 0) {
		/* OPBX_STATE_DOWN or softhangup */
		sccp_log(10)(VERBOSE_PREFIX_3 "%s: return from the startchannel for DOWN, HANGUP or PICKUP cause\n", l->device->id);
		opbx_mutex_lock(&c->lock);
		c->hangupok = 1;
		opbx_mutex_unlock(&c->lock);
		return NULL;
	}

dial:

	opbx_mutex_lock(&c->lock);
	opbx_copy_string(chan->exten, c->dialedNumber, sizeof(chan->exten));
	opbx_copy_string(d->lastNumber, c->dialedNumber, sizeof(d->lastNumber));
	sccp_channel_set_calledparty(c, c->dialedNumber, c->dialedNumber);
	/* proceed call state is needed to display the called number.
	The phone will not display callinfo in offhook state */
	sccp_channel_set_callstate(c, SKINNY_CALLSTATE_PROCEED);
	sccp_channel_send_callinfo(c);
	sccp_dev_clearprompt(d,c->line->instance, c->callid);
	sccp_dev_displayprompt(d, c->line->instance, c->callid, SKINNY_DISP_CALL_PROCEED, 0);
	c->hangupok = 1;
	opbx_mutex_unlock(&c->lock);

	if ( !opbx_strlen_zero(c->dialedNumber)
			&& opbx_exists_extension(chan, chan->context, c->dialedNumber, 1, l->cid_num) ) {
		/* found an extension, let's dial it */
		sccp_log(10)(VERBOSE_PREFIX_3 "%s: channel %s-%d is dialing number %s\n", l->device->id, l->name, c->callid, c->dialedNumber);
		/* Answer dialplan command works only when in RINGING OR RING opbx_state */
		sccp_opbx_setstate(c, OPBX_STATE_RING);
		if (opbx_pbx_run(chan)) {
			sccp_indicate_lock(c, SCCP_CHANNELSTATE_INVALIDNUMBER);
		}
	} else {
		/* timeout and no extension match */
		sccp_indicate_lock(c, SCCP_CHANNELSTATE_INVALIDNUMBER);
	}
	sccp_log(10)(VERBOSE_PREFIX_3 "%s: return from the startchannel on exit\n", l->device->id);
	return NULL;
}

void sccp_pbx_senddigit(sccp_channel_t * c, char digit) {
	struct opbx_frame f = { OPBX_FRAME_DTMF, };

	f.src = "SCCP";
	f.subclass = digit;

	opbx_mutex_lock(&c->lock);
	opbx_queue_frame(c->owner, &f);
	opbx_mutex_unlock(&c->lock);
}

void sccp_pbx_senddigits(sccp_channel_t * c, char digits[OPBX_MAX_EXTENSION]) {
	int i;
	struct opbx_frame f = { OPBX_FRAME_DTMF, 0};
	sccp_log(10)(VERBOSE_PREFIX_3 "%s: Sending digits %s\n", DEV_ID_LOG(c->device), digits);
	// We don't just call sccp_pbx_senddigit due to potential overhead, and issues with locking
	f.src = "SCCP";
	f.offset = 0;
	f.data = NULL;
	f.datalen = 0;
	opbx_mutex_lock(&c->lock);
	for (i = 0; digits[i] != '\0'; i++) {
		f.subclass = digits[i];
		sccp_log(10)(VERBOSE_PREFIX_3 "%s: Sending digit %c\n", DEV_ID_LOG(c->device), digits[i]);
		opbx_queue_frame(c->owner, &f);
	}
	opbx_mutex_unlock(&c->lock);
}