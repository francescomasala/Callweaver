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
#include "sccp_actions.h"
#include "sccp_utils.h"
#include "sccp_device.h"
#include "sccp_channel.h"
#include "sccp_cli.h"
#include "sccp_line.h"
#include "sccp_socket.h"
#include "sccp_pbx.h"
#include "sccp_indicate.h"

#include <ctype.h>
#include <unistd.h>
#include <openpbx/pbx.h>
#include <openpbx/old_callerid.h>
#include <openpbx/utils.h>
#include <openpbx/causes.h>
#include <openpbx/devicestate.h>
#include <openpbx/translate.h>


static pthread_t socket_thread;

/* ---------------------------------------------------- */

struct opbx_channel *sccp_request(const char *type, int format, void *data, int *cause) {

	sccp_line_t * l = NULL;
	sccp_channel_t * c = NULL;
	char *options = NULL, *datadup = NULL;
	int optc = 0;
	char *optv[2];
	int opti = 0;
	int res = 0;
	int oldformat = format;

	*cause = OPBX_CAUSE_NOTDEFINED;

	if (!type) {
		opbx_log(LOG_NOTICE, "Attempt to call the wrong type of channel\n");
		*cause = OPBX_CAUSE_REQUESTED_CHAN_UNAVAIL;
		goto OUT;
	}

	if (!data) {
		opbx_log(LOG_NOTICE, "Attempt to call SCCP/ failed\n");
		*cause = OPBX_CAUSE_REQUESTED_CHAN_UNAVAIL;
		goto OUT;
	}

	/* we leave the data unchanged */
	datadup = strdup(data);
	if ((options = strchr(datadup, '/'))) {
		*options = '\0';
		options++;
	}

	sccp_log(10)(VERBOSE_PREFIX_3 "SCCP: Asterisk asked to create a channel type=%s, format=%d, data=%s, options=%s\n", type, format, datadup, (options) ? options : "");

	l = sccp_line_find_byname(datadup);

	if (!l) {
		sccp_log(1)(VERBOSE_PREFIX_3 "SCCP/%s does not exist!\n", datadup);
		*cause = OPBX_CAUSE_REQUESTED_CHAN_UNAVAIL;
		goto OUT;
	}

	if (!l->device) {
		sccp_log(10)(VERBOSE_PREFIX_3 "SCCP/%s isn't currently registered anywhere.\n", l->name);
		*cause = OPBX_CAUSE_REQUESTED_CHAN_UNAVAIL;
		goto OUT;
	}

	if (!l->device->session) {
		sccp_log(10)(VERBOSE_PREFIX_3 "SCCP/%s device has no active session.\n", l->name);
		*cause = OPBX_CAUSE_REQUESTED_CHAN_UNAVAIL;
		goto OUT;
	}


	format &= l->device->capability;
	if (!format) {
		format = oldformat;
		res = opbx_translator_best_choice(&format, &l->device->capability);
		if (res < 0) {
			opbx_log(LOG_NOTICE, "Asked to get a channel of unsupported format '%d'\n", oldformat);
			*cause = OPBX_CAUSE_CHANNEL_UNACCEPTABLE;
			goto OUT;
		}
	}
	// Allocate a new SCCP channel.
	/* on multiline phone we set the line when answering or switching lines */
	 c = sccp_channel_allocate(l);
	 if (!c) {
		*cause = OPBX_CAUSE_REQUESTED_CHAN_UNAVAIL;
	 	goto OUT;
	 }

	 c->format = oldformat;
	if (!sccp_pbx_channel_allocate(c)) {
		*cause = OPBX_CAUSE_REQUESTED_CHAN_UNAVAIL;
		sccp_channel_delete(c);
		c = NULL;
		goto OUT;
	}

	/* call forward check */
	opbx_mutex_lock(&l->lock);
	if (l->cfwd_type == SCCP_CFWD_ALL) {
		sccp_log(1)(VERBOSE_PREFIX_3 "%s: Call forward (all) to %s\n", l->device->id, l->cfwd_num);
		opbx_copy_string(c->owner->call_forward, l->cfwd_num, sizeof(c->owner->call_forward));
	} else if (l->cfwd_type == SCCP_CFWD_BUSY && l->channelCount > 1) {
		sccp_log(1)(VERBOSE_PREFIX_3 "%s: Call forward (busy) to %s\n", l->device->id, l->cfwd_num);
		opbx_copy_string(c->owner->call_forward, l->cfwd_num, sizeof(c->owner->call_forward));
	}
	opbx_mutex_unlock(&l->lock);
	
	/* we don't need to parse any options when we have a call forward status */
	if (!opbx_strlen_zero(c->owner->call_forward))
		goto OUT;

	/* check for the channel params */
	if (options && (optc = sccp_app_separate_args(options, '/', optv, sizeof(optv) / sizeof(optv[0])))) {
		for (opti = 0; opti < optc; opti++) {
			if (!strncasecmp(optv[opti], "aa", 2)) {
				/* let's use the old style auto answer aa1w and aa2w */
				if (!strncasecmp(optv[opti], "aa1w", 4)) {
						c->autoanswer_type = SCCP_AUTOANSWER_1W;
						optv[opti]+=4;
				} else if (!strncasecmp(optv[opti], "aa2w", 4)) {
						c->autoanswer_type = SCCP_AUTOANSWER_2W;
						optv[opti]+=4;
				} else if (!strncasecmp(optv[opti], "aa=", 3)) {
					optv[opti] += 3;
					if (!strncasecmp(optv[opti], "1w", 2)) {
						c->autoanswer_type = SCCP_AUTOANSWER_1W;
						optv[opti]+=2;
					} else if (!strncasecmp(optv[opti], "2w", 2)) {
						c->autoanswer_type = SCCP_AUTOANSWER_2W;
						optv[opti]+=2;
					}
				}

				/* since the pbx ignores autoanswer_cause unless channelCount > 1, it is safe to set it if provided */
				if (!opbx_strlen_zero(optv[opti]) && (c->autoanswer_type)) {
					if (!strcasecmp(optv[opti], "b"))
						c->autoanswer_cause = OPBX_CAUSE_BUSY;
					else if (!strcasecmp(optv[opti], "u"))
						c->autoanswer_cause = OPBX_CAUSE_REQUESTED_CHAN_UNAVAIL;
					else if (!strcasecmp(optv[opti], "c"))
						c->autoanswer_cause = OPBX_CAUSE_CONGESTION;
				}
				if (c->autoanswer_cause)
					*cause = c->autoanswer_cause;
			/* check for ringer options */
			} else if (!strncasecmp(optv[opti], "ringer=", 7)) {
				optv[opti] += 7;
				if (!strcasecmp(optv[opti], "inside"))
					c->ringermode = SKINNY_STATION_INSIDERING;
				else if (!strcasecmp(optv[opti], "feature"))
					c->ringermode = SKINNY_STATION_FEATURERING;
				else if (!strcasecmp(optv[opti], "silent"))
					c->ringermode = SKINNY_STATION_SILENTRING;
				else if (!strcasecmp(optv[opti], "urgent"))
					c->ringermode = SKINNY_STATION_URGENTRING;
				else
					c->ringermode = SKINNY_STATION_OUTSIDERING;
			} else {
				opbx_log(LOG_WARNING, "%s: Wrong option %s\n", l->device->id, optv[opti]);
			}
		}
	}

OUT:
	if (datadup)
		free(datadup);

	return (c && c->owner ? c->owner : NULL);
}


int sccp_devicestate(void * data) {
	sccp_line_t * l =  NULL;
	int res = OPBX_DEVICE_UNKNOWN;

	l = sccp_line_find_byname((char*)data);
	if (!l)
		res = OPBX_DEVICE_INVALID;
	else if (!l->device)
		res = OPBX_DEVICE_UNAVAILABLE;
	else if (l->device->dnd && l->device->dndmode == SCCP_DNDMODE_REJECT)
		res = OPBX_DEVICE_BUSY;
	else if (!l->channelCount)
		res = OPBX_DEVICE_NOT_INUSE;
	else if (sccp_channel_find_bystate_on_device(l->device, SCCP_CHANNELSTATE_RINGIN))
		res = OPBX_DEVICE_RINGING;
	else
		res = OPBX_DEVICE_INUSE;

	sccp_log(10)(VERBOSE_PREFIX_3 "SCCP: Asterisk asked for the state (%d) of the line %s\n", res, (char *)data);

	return res;
}


sccp_hint_t * sccp_hint_make(sccp_device_t *d, uint8_t instance) {
	sccp_hint_t *h = NULL;
	if (!d)
		return NULL;
	h = malloc(sizeof(sccp_hint_t));

	if (!h)
		return NULL;
	h->device = d;
	h->instance = instance;
	return h;
}

void sccp_hint_notify_devicestate(sccp_device_t * d, uint8_t state) {
	sccp_line_t * l;
	if (!d || !d->session)
		return;
	opbx_mutex_lock(&d->lock);
	l = d->lines;
	while (l) {
		sccp_hint_notify_linestate(l, state, NULL);
		l = l->next_on_device;
	}
	opbx_mutex_unlock(&d->lock);
}

void sccp_hint_notify_linestate(sccp_line_t * l, uint8_t state, sccp_device_t * onedevice) {
	sccp_moo_t * r;
	sccp_hint_t * h;
	sccp_device_t * d;
	char tmp[256] = "";
	uint8_t lamp = SKINNY_LAMP_OFF;

	/* let's go for internal hint system */

	if (!l || !l->hints) {
		if (!onedevice)
			opbx_device_state_changed("SCCP/%s", l->name);
		return;
	}

	h = l->hints;

	while (h) {
		d = h->device;
		if (!d || !d->session) {
			h = h->next;
			continue;
		}

		/* this is for polling line status after device registration */
		if (onedevice && d != onedevice) {
			h = h->next;
			continue;
		}

		sccp_log(10)(VERBOSE_PREFIX_3 "%s: HINT notify state %d of the line '%s'\n", d->id, state, l->name);
		if (state == SCCP_DEVICESTATE_ONHOOK) {
			sccp_dev_set_lamp(d, SKINNY_STIMULUS_LINE, h->instance, SKINNY_LAMP_OFF);
			sccp_channel_set_callstate_full(d, h->instance, 0, SKINNY_CALLSTATE_ONHOOK);
			h = h->next;
			continue;
		}

		REQ(r, CallInfoMessage);
		switch (state) {
		case SCCP_DEVICESTATE_UNAVAILABLE:
			lamp = SKINNY_LAMP_ON;
			opbx_copy_string(r->msg.CallInfoMessage.callingPartyName, SKINNY_DISP_TEMP_FAIL, sizeof(r->msg.CallInfoMessage.callingPartyName));
			opbx_copy_string(r->msg.CallInfoMessage.calledPartyName, SKINNY_DISP_TEMP_FAIL, sizeof(r->msg.CallInfoMessage.calledPartyName));
			break;
		case SCCP_DEVICESTATE_DND:
			lamp = SKINNY_LAMP_ON;
			opbx_copy_string(r->msg.CallInfoMessage.callingPartyName, SKINNY_DISP_DND, sizeof(r->msg.CallInfoMessage.callingPartyName));
			opbx_copy_string(r->msg.CallInfoMessage.calledPartyName, SKINNY_DISP_DND, sizeof(r->msg.CallInfoMessage.calledPartyName));
			break;
		case SCCP_DEVICESTATE_FWDALL:
			lamp = SKINNY_LAMP_ON;
			if (l->cfwd_type == SCCP_CFWD_ALL) {
				strcat(tmp, SKINNY_DISP_FORWARDED_TO " ");
				strcat(tmp, l->cfwd_num);
				opbx_copy_string(r->msg.CallInfoMessage.callingPartyName, tmp, sizeof(r->msg.CallInfoMessage.callingPartyName));
				opbx_copy_string(r->msg.CallInfoMessage.calledPartyName, tmp, sizeof(r->msg.CallInfoMessage.calledPartyName));
			}
			break;
		default:
			/* nothing to send */
			free(r);
			h = h->next;
			continue;
		}
		sccp_dev_set_lamp(d, SKINNY_STIMULUS_LINE, h->instance, lamp);
		sccp_channel_set_callstate_full(d, h->instance, 0, SKINNY_CALLSTATE_CALLREMOTEMULTILINE);
		/* sending CallInfoMessage */
		r->msg.CallInfoMessage.lel_lineId = htolel(h->instance);
		r->msg.CallInfoMessage.lel_callType = htolel(SKINNY_CALLTYPE_OUTBOUND);
		sccp_dev_send(d, r);
		sccp_dev_set_keyset(d, h->instance, 0, KEYMODE_MYST);

		h = h->next;
	}

	/* notify the openpbx hint system when we are not in a postregistration state (onedevice) */
	if (!onedevice)
		opbx_device_state_changed("SCCP/%s", l->name);
}

void sccp_hint_notify_channelstate(sccp_device_t *d, uint8_t instance, sccp_channel_t * c) {
	sccp_moo_t * r;
	uint8_t lamp = SKINNY_LAMP_OFF;

	if (!d)
		d = c->device;
	if (!d)
		return;

	sccp_log(10)(VERBOSE_PREFIX_3 "%s: HINT notify state %s (%d) of the channel %d \n", d->id, sccp_callstate2str(c->callstate), c->callstate, c->callid);

	if (c->callstate == SKINNY_CALLSTATE_ONHOOK) {
		sccp_dev_set_lamp(d, SKINNY_STIMULUS_LINE, instance, SKINNY_LAMP_OFF);
		sccp_channel_set_callstate_full(d, instance, c->callid, SKINNY_CALLSTATE_ONHOOK);
		return;
	}

	REQ(r, CallInfoMessage);
	switch (c->callstate) {
	case SKINNY_CALLSTATE_CONNECTED:
		lamp = SKINNY_LAMP_ON;
		opbx_copy_string(r->msg.CallInfoMessage.callingPartyName, c->callingPartyName, sizeof(r->msg.CallInfoMessage.callingPartyName));
		opbx_copy_string(r->msg.CallInfoMessage.callingParty, c->callingPartyNumber, sizeof(r->msg.CallInfoMessage.callingParty));
		opbx_copy_string(r->msg.CallInfoMessage.calledPartyName, c->calledPartyName, sizeof(r->msg.CallInfoMessage.calledPartyName));
		opbx_copy_string(r->msg.CallInfoMessage.calledParty, c->calledPartyNumber, sizeof(r->msg.CallInfoMessage.calledParty));
		break;
	case SKINNY_CALLSTATE_OFFHOOK:
		lamp = SKINNY_LAMP_ON;
		opbx_copy_string(r->msg.CallInfoMessage.callingPartyName, SKINNY_DISP_OFF_HOOK, sizeof(r->msg.CallInfoMessage.callingPartyName));
		opbx_copy_string(r->msg.CallInfoMessage.calledPartyName, SKINNY_DISP_OFF_HOOK, sizeof(r->msg.CallInfoMessage.calledPartyName));
		break;
	case SKINNY_CALLSTATE_RINGOUT:
		lamp = SKINNY_LAMP_ON;
		opbx_copy_string(r->msg.CallInfoMessage.callingPartyName, c->callingPartyName, sizeof(r->msg.CallInfoMessage.callingPartyName));
		opbx_copy_string(r->msg.CallInfoMessage.callingParty, c->callingPartyNumber, sizeof(r->msg.CallInfoMessage.callingParty));
		opbx_copy_string(r->msg.CallInfoMessage.calledPartyName, c->calledPartyName, sizeof(r->msg.CallInfoMessage.calledPartyName));
		opbx_copy_string(r->msg.CallInfoMessage.calledParty, c->calledPartyNumber, sizeof(r->msg.CallInfoMessage.calledParty));
		break;
	case SKINNY_CALLSTATE_RINGIN:
		lamp = SKINNY_LAMP_BLINK;
		opbx_copy_string(r->msg.CallInfoMessage.callingPartyName, c->callingPartyName, sizeof(r->msg.CallInfoMessage.callingPartyName));
		opbx_copy_string(r->msg.CallInfoMessage.callingParty, c->callingPartyNumber, sizeof(r->msg.CallInfoMessage.callingParty));
		opbx_copy_string(r->msg.CallInfoMessage.calledPartyName, c->calledPartyName, sizeof(r->msg.CallInfoMessage.calledPartyName));
		opbx_copy_string(r->msg.CallInfoMessage.calledParty, c->calledPartyNumber, sizeof(r->msg.CallInfoMessage.calledParty));
		break;
	default:
		/* nothing to send */
		free(r);
		return;
	}
	sccp_dev_set_lamp(d, SKINNY_STIMULUS_LINE, instance, SKINNY_LAMP_ON);
	sccp_channel_set_callstate_full(d, instance, c->callid, SKINNY_CALLSTATE_CALLREMOTEMULTILINE);
	/* sending CallInfoMessage */
	r->msg.CallInfoMessage.lel_lineId   = htolel(instance);
	r->msg.CallInfoMessage.lel_callRef  = htolel(c->callid);
	r->msg.CallInfoMessage.lel_callType = htolel(c->calltype);

	sccp_dev_send(d, r);
//			sccp_dev_sendmsg(d, DeactivateCallPlaneMessage);
	sccp_dev_set_keyset(d, instance, c->callid, KEYMODE_MYST);
}


void sccp_hint_notify(sccp_channel_t * c, sccp_device_t * onedevice) {
	sccp_hint_t *h;
	sccp_line_t *l = c->line;
	sccp_device_t *d;

	h = l->hints;
	if (!h)
		return;

	sccp_log(10)(VERBOSE_PREFIX_3 "SCCP: HINT notify the state of the line %s \n", l->name);

	while (h) {
		d = h->device;
		if (!d || !d->session) {
			h = h->next;
			continue;
		}
		if (onedevice && d == onedevice) {
			sccp_hint_notify_channelstate(d, h->instance, c);
			break;
		} else
			sccp_hint_notify_channelstate(d, h->instance, c);
		h = h->next;
	}
}

/* openpbx hint wrapper */
int sccp_hint_state(char *context, char* exten, int state, void *data) {
	sccp_hint_t * h = data;
	sccp_device_t *d;
	sccp_moo_t * r;
	if (state == -1 || !h || !h->device || !h->device->session)
		return 0;

	d = h->device;
	REQ(r, CallInfoMessage);

	sccp_log(10)(VERBOSE_PREFIX_3 "%s: HINT notify state %s (%d), instance %d\n", d->id, sccp_extensionstate2str(state), state, h->instance);
	switch(state) {
		case OPBX_EXTENSION_NOT_INUSE:
			sccp_dev_set_lamp(d, SKINNY_STIMULUS_LINE, h->instance, SKINNY_LAMP_OFF);
			sccp_channel_set_callstate_full(d, h->instance, 0, SKINNY_CALLSTATE_ONHOOK);
			return 0;
		case OPBX_EXTENSION_INUSE:
			sccp_dev_set_lamp(d, SKINNY_STIMULUS_LINE, h->instance, SKINNY_LAMP_ON);
			sccp_channel_set_callstate_full(d, h->instance, 0, SKINNY_CALLSTATE_CALLREMOTEMULTILINE);
			opbx_copy_string(r->msg.CallInfoMessage.callingPartyName, SKINNY_DISP_LINE_IN_USE, sizeof(r->msg.CallInfoMessage.callingPartyName));
			opbx_copy_string(r->msg.CallInfoMessage.calledPartyName, SKINNY_DISP_LINE_IN_USE, sizeof(r->msg.CallInfoMessage.calledPartyName));
			break;
		case OPBX_EXTENSION_BUSY:
			sccp_dev_set_lamp(d, SKINNY_STIMULUS_LINE, h->instance, SKINNY_LAMP_ON);
			sccp_channel_set_callstate_full(d, h->instance, 0, SKINNY_CALLSTATE_CALLREMOTEMULTILINE);
			opbx_copy_string(r->msg.CallInfoMessage.callingPartyName, SKINNY_DISP_BUSY, sizeof(r->msg.CallInfoMessage.callingPartyName));
			opbx_copy_string(r->msg.CallInfoMessage.calledPartyName, SKINNY_DISP_BUSY, sizeof(r->msg.CallInfoMessage.calledPartyName));
			break;
		case OPBX_EXTENSION_UNAVAILABLE:
			sccp_dev_set_lamp(d, SKINNY_STIMULUS_LINE, h->instance, SKINNY_LAMP_FLASH);
			sccp_channel_set_callstate_full(d, h->instance, 0, SKINNY_CALLSTATE_CALLREMOTEMULTILINE);
			opbx_copy_string(r->msg.CallInfoMessage.callingPartyName, SKINNY_DISP_TEMP_FAIL, sizeof(r->msg.CallInfoMessage.callingPartyName));
			opbx_copy_string(r->msg.CallInfoMessage.calledPartyName, SKINNY_DISP_TEMP_FAIL, sizeof(r->msg.CallInfoMessage.calledPartyName));
			break;
		case OPBX_EXTENSION_RINGING:
			sccp_dev_set_lamp(d, SKINNY_STIMULUS_LINE, h->instance, SKINNY_LAMP_FLASH);
			sccp_channel_set_callstate_full(d, h->instance, 0, SKINNY_CALLSTATE_CALLREMOTEMULTILINE);
			opbx_copy_string(r->msg.CallInfoMessage.callingPartyName, SKINNY_DISP_LINE_IN_USE, sizeof(r->msg.CallInfoMessage.callingPartyName));
			opbx_copy_string(r->msg.CallInfoMessage.calledPartyName, SKINNY_DISP_LINE_IN_USE, sizeof(r->msg.CallInfoMessage.calledPartyName));
			break;
		default:
			sccp_dev_set_lamp(d, SKINNY_STIMULUS_LINE, h->instance, SKINNY_LAMP_FLASH);
			sccp_channel_set_callstate_full(d, h->instance, 0, SKINNY_CALLSTATE_CALLREMOTEMULTILINE);
			opbx_copy_string(r->msg.CallInfoMessage.callingPartyName, SKINNY_DISP_TEMP_FAIL, sizeof(r->msg.CallInfoMessage.callingPartyName));
			opbx_copy_string(r->msg.CallInfoMessage.calledPartyName, SKINNY_DISP_TEMP_FAIL, sizeof(r->msg.CallInfoMessage.calledPartyName));
	}

	opbx_copy_string(r->msg.CallInfoMessage.callingParty, "", sizeof(r->msg.CallInfoMessage.callingParty));
	opbx_copy_string(r->msg.CallInfoMessage.calledParty, "", sizeof(r->msg.CallInfoMessage.calledParty));

	r->msg.CallInfoMessage.lel_lineId   = htolel(h->instance);
	r->msg.CallInfoMessage.lel_callRef  = htolel(0);
	r->msg.CallInfoMessage.lel_callType = htolel(SKINNY_CALLTYPE_OUTBOUND);
	sccp_dev_send(d, r);
	return 0;
}

uint8_t sccp_handle_message(sccp_moo_t * r, sccp_session_t * s) {
  uint32_t  mid = letohl(r->lel_messageId);
  s->lastKeepAlive = time(0); /* always update keepalive */

  if ( (!s->device) && (mid != RegisterMessage && mid != UnregisterMessage && mid != RegisterTokenReq && mid != AlarmMessage && mid != KeepAliveMessage && mid != IpPortMessage)) {
	opbx_log(LOG_WARNING, "Client sent %s without first registering.\n", sccpmsg2str(mid));
	free(r);
	return 0;
  }

  if (mid != KeepAliveMessage) {
	if (s && s->device) {
		sccp_log(10)(VERBOSE_PREFIX_3 "%s: >> Got message %s\n", s->device->id, sccpmsg2str(mid));
	} else {
		sccp_log(10)(VERBOSE_PREFIX_3 "SCCP: >> Got message %s\n", sccpmsg2str(mid));
	}
  }

  switch (mid) {

  case AlarmMessage:
	sccp_handle_alarm(s, r);
	break;
	case RegisterMessage:
	case RegisterTokenReq:
	sccp_handle_register(s, r);
	break;
  case UnregisterMessage:
	sccp_handle_unregister(s, r);
	break;
  case VersionReqMessage:
	sccp_handle_version(s, r);
	break;
  case CapabilitiesResMessage:
	sccp_handle_capabilities_res(s, r);
	break;
  case ButtonTemplateReqMessage:
	sccp_handle_button_template_req(s, r);
	break;
  case SoftKeyTemplateReqMessage:
	sccp_handle_soft_key_template_req(s, r);
	break;
  case SoftKeySetReqMessage:
	sccp_handle_soft_key_set_req(s, r);
	break;
  case LineStatReqMessage:
	sccp_handle_line_number(s, r);
	break;
  case SpeedDialStatReqMessage:
	sccp_handle_speed_dial_stat_req(s, r);
	break;
  case StimulusMessage:
	sccp_handle_stimulus(s, r);
	break;
  case OffHookMessage:
	sccp_handle_offhook(s, r);
	break;
  case OnHookMessage:
	sccp_handle_onhook(s, r);
	break;
  case HeadsetStatusMessage:
	sccp_handle_headset(s, r);
	break;
  case TimeDateReqMessage:
	sccp_handle_time_date_req(s, r);
	break;
  case KeypadButtonMessage:
	sccp_handle_keypad_button(s, r);
	break;
  case SoftKeyEventMessage:
	sccp_handle_soft_key_event(s, r);
	break;
  case KeepAliveMessage:
	sccp_session_sendmsg(s, KeepAliveAckMessage);
	sccp_dev_check_mwi(s->device);
	break;
  case IpPortMessage:
  	/* obsolete message */
	s->rtpPort = letohs(r->msg.IpPortMessage.les_rtpMediaPort);
	break;
  case OpenReceiveChannelAck:
	sccp_handle_open_receive_channel_ack(s, r);
	break;
  case ConnectionStatisticsRes:
	sccp_handle_ConnectionStatistics(s,r);
	break;
  case ServerReqMessage:
	sccp_handle_ServerResMessage(s,r);
	break;
  case ConfigStatReqMessage:
	sccp_handle_ConfigStatMessage(s,r);
	break;
  case EnblocCallMessage:
	sccp_handle_EnblocCallMessage(s,r);
	break;
  case RegisterAvailableLinesMessage:
	if (s->device)
		sccp_dev_set_registered(s->device, SKINNY_DEVICE_RS_OK);
	break;
  case ForwardStatReqMessage:
	sccp_handle_forward_stat_req(s,r);
	break;
  case FeatureStatReqMessage:
	sccp_handle_feature_stat_req(s,r);
	break;
  default:
	if (GLOB(debug))
		opbx_log(LOG_WARNING, "Unhandled SCCP Message: %d - %s with length %d\n", mid, sccpmsg2str(mid), r->length);
  }

  free(r);
  return 1;
}

static sccp_line_t * build_line(void) {
	sccp_line_t * l = malloc(sizeof(sccp_line_t));
	if (!l) {
		sccp_log(0)(VERBOSE_PREFIX_3 "Unable to allocate memory for a line\n");
		return NULL;
	}
	memset(l, 0, sizeof(sccp_line_t));
	opbx_mutex_init(&l->lock);
	l->instance = -1;
	l->incominglimit = 3; /* default value */
	l->echocancel = GLOB(echocancel); /* default value */
	l->silencesuppression = GLOB(silencesuppression); /* default value */
	l->rtptos = GLOB(rtptos); /* default value */
	l->transfer = 1; /* default value. on if the device transfer is on*/
	l->secondary_dialtone_tone = SKINNY_TONE_OUTSIDEDIALTONE;

	opbx_copy_string(l->context, GLOB(context), sizeof(l->context));
	opbx_copy_string(l->language, GLOB(language), sizeof(l->language));
	opbx_copy_string(l->accountcode, GLOB(accountcode), sizeof(l->accountcode));
	opbx_copy_string(l->musicclass, GLOB(musicclass), sizeof(l->musicclass));
	l->amaflags = GLOB(amaflags);
	l->callgroup = GLOB(callgroup);
#ifdef CS_SCCP_PICKUP
	l->pickupgroup = GLOB(pickupgroup);
#endif

  return l;
}

static sccp_device_t * build_device(void) {
	sccp_device_t * d = malloc(sizeof(sccp_device_t));
	if (!d) {
		sccp_log(0)(VERBOSE_PREFIX_3 "Unable to allocate memory for a device\n");
		return NULL;
	}
	memset(d, 0, sizeof(sccp_device_t));
	opbx_mutex_init(&d->lock);

	d->tz_offset = 0;
	d->capability = GLOB(global_capability);
	d->codecs = GLOB(global_codecs);
	d->transfer = 1;
	d->state = SCCP_DEVICESTATE_ONHOOK;
	d->ringermode = SKINNY_STATION_RINGOFF;
	d->dndmode = GLOB(dndmode);
	d->trustphoneip = GLOB(trustphoneip);
	d->private = GLOB(private);
	d->earlyrtp = GLOB(earlyrtp);
	d->mwilamp = GLOB(mwilamp);
	d->mwioncall = GLOB(mwioncall);

#ifdef CS_SCCP_PARK
	d->park = 1;
#else
	d->park = 0;
#endif

  return d;
}

static int reload_config(void) {
	struct opbx_config	*cfg;
	struct opbx_variable	*v;
	int					oldport	= ntohs(GLOB(bindaddr.sin_port));
	int					on		= 1;
	int					tos		= 0;
	char				pref_buf[128];
	char				iabuf[INET_ADDRSTRLEN];
	struct opbx_hostent	ahp;
	struct hostent		*hp;
	struct opbx_ha 		*na;
	sccp_hostname_t 	*permithost;
	sccp_device_t		*d;
	sccp_line_t			*l;
	sccp_speed_t		*k = NULL, *k_last = NULL;
	char 				*splitter, *k_exten = NULL, *k_name = NULL, *k_hint = NULL;
	char k_speed[256];
	uint8_t 			speeddial_index = 1;
	int firstdigittimeout = 0;
	int digittimeout = 0;
	int autoanswer_ring_time = 0;
	int autoanswer_tone = 0;
	int remotehangup_tone = 0;
	int secondary_dialtone_tone = 0;
	int transfer_tone = 0;
	int callwaiting_tone = 0;
	int amaflags = 0;
	int protocolversion = 0;

	memset(&GLOB(global_codecs), 0, sizeof(GLOB(global_codecs)));
	memset(&GLOB(bindaddr), 0, sizeof(GLOB(bindaddr)));

#if SCCP_PLATFORM_BYTE_ORDER == SCCP_LITTLE_ENDIAN
	sccp_log(0)(VERBOSE_PREFIX_2 "Platform byte order   : LITTLE ENDIAN\n");
#else
	sccp_log(0)(VERBOSE_PREFIX_2 "Platform byte order   : BIG ENDIAN\n");
#endif

	cfg = opbx_config_load("sccp.conf");
	if (!cfg) {
		opbx_log(LOG_WARNING, "Unable to load config file sccp.conf, SCCP disabled\n");
		return 0;
	}
	/* read the general section */
	v = opbx_variable_browse(cfg, "general");
	if (!v) {
		opbx_log(LOG_WARNING, "Missing [general] section, SCCP disabled\n");
		return 0;
	}

	while (v) {
		if (!strcasecmp(v->name, "protocolversion")) {
			if (sscanf(v->value, "%i", &protocolversion) == 1) {
				if (protocolversion < 2 || protocolversion > 6)
					GLOB(protocolversion) = 3;
				else
					GLOB(protocolversion) = protocolversion;
			} else {
				opbx_log(LOG_WARNING, "Invalid protocolversion number '%s' at line %d of SCCP.CONF\n", v->value, v->lineno);
			}
		} else if (!strcasecmp(v->name, "servername")) {
			opbx_copy_string(GLOB(servername), v->value, sizeof(GLOB(servername)));
		} else if (!strcasecmp(v->name, "bindaddr")) {
			if (!(hp = opbx_gethostbyname(v->value, &ahp))) {
				opbx_log(LOG_WARNING, "Invalid address: %s. SCCP disabled\n", v->value);
				return 0;
			} else {
				memcpy(&GLOB(bindaddr.sin_addr), hp->h_addr, sizeof(GLOB(bindaddr.sin_addr)));
			}
		} else if (!strcasecmp(v->name, "permit") || !strcasecmp(v->name, "deny")) {
			GLOB(ha) = opbx_append_ha(v->name, v->value, GLOB(ha));
		} else if (!strcasecmp(v->name, "localnet")) {
			if (!(na = opbx_append_ha("d", v->value, GLOB(localaddr))))
				opbx_log(LOG_WARNING, "Invalid localnet value: %s\n", v->value);
			else
				GLOB(localaddr) = na;
		} else if (!strcasecmp(v->name, "externip")) {
			if (!(hp = opbx_gethostbyname(v->value, &ahp)))
				opbx_log(LOG_WARNING, "Invalid address for externip keyword: %s\n", v->value);
			else
				memcpy(&GLOB(externip.sin_addr), hp->h_addr, sizeof(GLOB(externip.sin_addr)));
			GLOB(externexpire) = 0;
		} else if (!strcasecmp(v->name, "externhost")) {
			opbx_copy_string(GLOB(externhost), v->value, sizeof(GLOB(externhost)));
			if (!(hp = opbx_gethostbyname(GLOB(externhost), &ahp)))
				opbx_log(LOG_WARNING, "Invalid address resolution for externhost keyword: %s\n", GLOB(externhost));
			else
				memcpy(&GLOB(externip.sin_addr), hp->h_addr, sizeof(GLOB(externip.sin_addr)));
			time(&GLOB(externexpire));
		} else if (!strcasecmp(v->name, "externrefresh")) {
			if (sscanf(v->value, "%d", &GLOB(externrefresh)) != 1) {
				opbx_log(LOG_WARNING, "Invalid externrefresh value '%s', must be an integer >0 at line %d\n", v->value, v->lineno);
				GLOB(externrefresh) = 10;
			}
		} else if (!strcasecmp(v->name, "keepalive")) {
			GLOB(keepalive) = atoi(v->value);
		} else if (!strcasecmp(v->name, "context")) {
			opbx_copy_string(GLOB(context), v->value, sizeof(GLOB(context)));
		} else if (!strcasecmp(v->name, "language")) {
			opbx_copy_string(GLOB(language), v->value, sizeof(GLOB(language)));
		} else if (!strcasecmp(v->name, "accountcode")) {
			opbx_copy_string(GLOB(accountcode), v->value, sizeof(GLOB(accountcode)));
		} else if (!strcasecmp(v->name, "amaflags")) {
			amaflags = opbx_cdr_amaflags2int(v->value);
			if (amaflags < 0) {
				opbx_log(LOG_WARNING, "Invalid AMA Flags: %s at line %d\n", v->value, v->lineno);
			} else {
				GLOB(amaflags) = amaflags;
			}
		} else if (!strcasecmp(v->name, "musicclass")) {
			opbx_copy_string(GLOB(musicclass), v->value, sizeof(GLOB(musicclass)));
		} else if (!strcasecmp(v->name, "callgroup")) {
			GLOB(callgroup) = opbx_get_group(v->value);
#ifdef CS_SCCP_PICKUP
		} else if (!strcasecmp(v->name, "pickupgroup")) {
			GLOB(pickupgroup) = opbx_get_group(v->value);
#endif
		} else if (!strcasecmp(v->name, "dateformat")) {
			opbx_copy_string (GLOB(date_format), v->value, sizeof(GLOB(date_format)));
		} else if (!strcasecmp(v->name, "port")) {
			if (sscanf(v->value, "%i", &GLOB(ourport)) == 1) {
				GLOB(bindaddr.sin_port) = htons(GLOB(ourport));
			} else {
				opbx_log(LOG_WARNING, "Invalid port number '%s' at line %d of SCCP.CONF\n", v->value, v->lineno);
			}
		} else if (!strcasecmp(v->name, "firstdigittimeout")) {
			if (sscanf(v->value, "%i", &firstdigittimeout) == 1) {
				if (firstdigittimeout > 0 && firstdigittimeout < 255)
					GLOB(firstdigittimeout) = firstdigittimeout;
			} else {
				opbx_log(LOG_WARNING, "Invalid firstdigittimeout number '%s' at line %d of SCCP.CONF\n", v->value, v->lineno);
			}
		} else if (!strcasecmp(v->name, "digittimeout")) {
			if (sscanf(v->value, "%i", &digittimeout) == 1) {
				if (digittimeout > 0 && digittimeout < 255)
					GLOB(digittimeout) = digittimeout;
			} else {
				opbx_log(LOG_WARNING, "Invalid firstdigittimeout number '%s' at line %d of SCCP.CONF\n", v->value, v->lineno);
			}
		} else if (!strcasecmp(v->name, "digittimeoutchar")) {
			GLOB(digittimeoutchar) = v->value[0];
		} else if (!strcasecmp(v->name, "debug")) {
			GLOB(debug) = atoi(v->value);
		} else if (!strcasecmp(v->name, "allow")) {
			opbx_parse_allow_disallow(&GLOB(global_codecs), &GLOB(global_capability), v->value, 1);
		} else if (!strcasecmp(v->name, "disallow")) {
			opbx_parse_allow_disallow(&GLOB(global_codecs), &GLOB(global_capability), v->value, 0);
		} else if (!strcasecmp(v->name, "dnd")) {
			if (!strcasecmp(v->value, "reject")) {
				GLOB(dndmode) = SCCP_DNDMODE_REJECT;
			} else if (!strcasecmp(v->value, "silent")) {
				GLOB(dndmode) = SCCP_DNDMODE_SILENT;
			} else {
				/* 0 is off and 1 (on) is reject */
				GLOB(dndmode) = sccp_true(v->value);
			}
		} else if (!strcasecmp(v->name, "echocancel")) {
			GLOB(echocancel) = sccp_true(v->value);
		} else if (!strcasecmp(v->name, "silencesuppression")) {
			GLOB(silencesuppression) = sccp_true(v->value);
		} else if (!strcasecmp(v->name, "trustphoneip")) {
			GLOB(trustphoneip) = sccp_true(v->value);
		} else if (!strcasecmp(v->name, "private")) {
			GLOB(private) = sccp_true(v->value);
		} else if (!strcasecmp(v->name, "earlyrtp")) {
			if (!strcasecmp(v->value, "none"))
				GLOB(earlyrtp) = 0;
			else if (!strcasecmp(v->value, "offhook"))
				GLOB(earlyrtp) = SCCP_CHANNELSTATE_OFFHOOK;
			else if (!strcasecmp(v->value, "dial"))
				GLOB(earlyrtp) = SCCP_CHANNELSTATE_DIALING;
			else if (!strcasecmp(v->value, "ringout"))
				GLOB(earlyrtp) = SCCP_CHANNELSTATE_RINGOUT;
			else
				opbx_log(LOG_WARNING, "Invalid earlyrtp state value at line %d, should be 'none', 'offhook', 'dial', 'ringout'\n", v->lineno);
		} else if (!strcasecmp(v->name, "tos")) {
			if (sscanf(v->value, "%d", &tos) == 1)
				GLOB(tos) = tos & 0xff;
			else if (!strcasecmp(v->value, "lowdelay"))
				GLOB(tos) = IPTOS_LOWDELAY;
			else if (!strcasecmp(v->value, "throughput"))
				GLOB(tos) = IPTOS_THROUGHPUT;
			else if (!strcasecmp(v->value, "reliability"))
				GLOB(tos) = IPTOS_RELIABILITY;
			#if !defined(__NetBSD__) && !defined(__OpenBSD__) && !defined(SOLARIS)
			else if (!strcasecmp(v->value, "mincost"))
				GLOB(tos) = IPTOS_MINCOST;
			#endif
			else if (!strcasecmp(v->value, "none"))
				GLOB(tos) = 0;
			else
			#if !defined(__NetBSD__) && !defined(__OpenBSD__) && !defined(SOLARIS)
				opbx_log(LOG_WARNING, "Invalid tos value at line %d, should be 'lowdelay', 'throughput', 'reliability', 'mincost', or 'none'\n", v->lineno);
			#else
				opbx_log(LOG_WARNING, "Invalid tos value at line %d, should be 'lowdelay', 'throughput', 'reliability', or 'none'\n", v->lineno);
			#endif
		} else if (!strcasecmp(v->name, "rtptos")) {
			if (sscanf(v->value, "%d", &GLOB(rtptos)) == 1)
				GLOB(rtptos) &= 0xff;
		} else if (!strcasecmp(v->name, "autoanswer_ring_time")) {
			if (sscanf(v->value, "%i", &autoanswer_ring_time) == 1) {
				if (autoanswer_ring_time >= 0 && autoanswer_ring_time <= 255)
					GLOB(autoanswer_ring_time) = autoanswer_ring_time;
			} else {
				opbx_log(LOG_WARNING, "Invalid autoanswer_ring_time value '%s' at line %d of SCCP.CONF. Default is 0\n", v->value, v->lineno);
			}
		} else if (!strcasecmp(v->name, "autoanswer_tone")) {
			if (sscanf(v->value, "%i", &autoanswer_tone) == 1) {
				if (autoanswer_tone >= 0 && autoanswer_tone <= 255)
					GLOB(autoanswer_tone) = autoanswer_tone;
			} else {
				opbx_log(LOG_WARNING, "Invalid autoanswer_tone value '%s' at line %d of SCCP.CONF. Default is 0\n", v->value, v->lineno);
			}
		} else if (!strcasecmp(v->name, "remotehangup_tone")) {
			if (sscanf(v->value, "%i", &remotehangup_tone) == 1) {
				if (remotehangup_tone >= 0 && remotehangup_tone <= 255)
					GLOB(remotehangup_tone) = remotehangup_tone;
			} else {
				opbx_log(LOG_WARNING, "Invalid remotehangup_tone value '%s' at line %d of SCCP.CONF. Default is 0\n", v->value, v->lineno);
			}
		} else if (!strcasecmp(v->name, "transfer_tone")) {
			if (sscanf(v->value, "%i", &transfer_tone) == 1) {
				if (transfer_tone >= 0 && transfer_tone <= 255)
					GLOB(transfer_tone) = transfer_tone;
			} else {
				opbx_log(LOG_WARNING, "Invalid transfer_tone value '%s' at line %d of SCCP.CONF. Default is 0\n", v->value, v->lineno);
			}
		} else if (!strcasecmp(v->name, "callwaiting_tone")) {
			if (sscanf(v->value, "%i", &callwaiting_tone) == 1) {
				if (callwaiting_tone >= 0 && callwaiting_tone <= 255)
					GLOB(callwaiting_tone) = callwaiting_tone;
			} else {
				opbx_log(LOG_WARNING, "Invalid callwaiting_tone value '%s' at line %d of SCCP.CONF. Default is 0\n", v->value, v->lineno);
			}
		} else if (!strcasecmp(v->name, "mwilamp")) {
			if (!strcasecmp(v->value, "off"))
				GLOB(mwilamp) = SKINNY_LAMP_OFF;
			else if (!strcasecmp(v->value, "on"))
				GLOB(mwilamp) = SKINNY_LAMP_ON;
			else if (!strcasecmp(v->value, "wink"))
				GLOB(mwilamp) = SKINNY_LAMP_WINK;
			else if (!strcasecmp(v->value, "flash"))
				GLOB(mwilamp) = SKINNY_LAMP_FLASH;
			else if (!strcasecmp(v->value, "blink"))
				GLOB(mwilamp) = SKINNY_LAMP_BLINK;
			else
				opbx_log(LOG_WARNING, "Invalid mwilamp value at line %d, should be 'off', 'on', 'wink', 'flash' or 'blink'\n", v->lineno);
		} else if (!strcasecmp(v->name, "mwioncall")) {
				GLOB(mwioncall) = sccp_true(v->value);
		} else if (!strcasecmp(v->name, "blindtransferindication")) {
			if (!strcasecmp(v->value, "moh"))
				GLOB(blindtransferindication) = SCCP_BLINDTRANSFER_MOH;
			else if (!strcasecmp(v->value, "ring"))
				GLOB(blindtransferindication) = SCCP_BLINDTRANSFER_RING;
			else
				opbx_log(LOG_WARNING, "Invalid blindtransferindication value at line %d, should be 'moh' or 'ring'\n", v->lineno);
		} else {
			opbx_log(LOG_WARNING, "Unknown param at line %d: %s = %s\n", v->lineno, v->name, v->value);
		}
		v = v->next;
	}

	opbx_codec_pref_string(&GLOB(global_codecs), pref_buf, sizeof(pref_buf) - 1);
	opbx_verbose(VERBOSE_PREFIX_3 "GLOBAL: Preferred capability %s\n", pref_buf);

	if (!ntohs(GLOB(bindaddr.sin_port))) {
		GLOB(bindaddr.sin_port) = ntohs(DEFAULT_SCCP_PORT);
	}
	GLOB(bindaddr.sin_family) = AF_INET;

	v = opbx_variable_browse(cfg, "devices");
	if (!v) {
		opbx_log(LOG_WARNING, "Missing [devices] section, SCCP disabled\n");
		return 0;
	}
	d = build_device();
	/* read devices section */
	while (v) {
		if (!strcasecmp(v->name, "device")) {
			if ( (strlen(v->value) == 15) && ((strncmp(v->value, "SEP",3) == 0) || (strncmp(v->value, "ATA",3)==0)) ) {
				opbx_copy_string(d->id, v->value, sizeof(d->id));
				opbx_verbose(VERBOSE_PREFIX_3 "Added device '%s' (%s)\n", d->id, d->config_type);
				opbx_mutex_lock(&GLOB(devices_lock));
				d->next = GLOB(devices);
				GLOB(devices) = d;
				opbx_mutex_unlock(&GLOB(devices_lock));
			} else {
				opbx_log(LOG_WARNING, "Wrong device param: %s => %s\n", v->name, v->value);
				sccp_dev_clean(d);
				free(d);
			}
			d = build_device();
			speeddial_index = 1;
			k_last = NULL;
		} else if (!strcasecmp(v->name, "permit") || !strcasecmp(v->name, "deny")) {
			d->ha = opbx_append_ha(v->name, v->value, d->ha);
		} else if (!strcasecmp(v->name, "permithost")) {
			if ((permithost = malloc(sizeof(sccp_hostname_t)))) {
				opbx_copy_string(permithost->name, v->value, sizeof(permithost->name));
				permithost->next = d->permithost;
				d->permithost = permithost;
			} else {
				opbx_log(LOG_WARNING, "Error adding the permithost = %s to the list\n", v->value);
			}
		} else if (!strcasecmp(v->name, "type")) {
			opbx_copy_string(d->config_type, v->value, sizeof(d->config_type));
		} else if (!strcasecmp(v->name, "tzoffset")) {
			/* timezone offset */
			d->tz_offset = atoi(v->value);
		} else if (!strcasecmp(v->name, "autologin")) {
			opbx_copy_string(d->autologin, v->value, sizeof(d->autologin));
		} else if (!strcasecmp(v->name, "description")) {
			opbx_copy_string(d->description, v->value, sizeof(d->description));
		} else if (!strcasecmp(v->name, "imageversion")) {
			opbx_copy_string(d->imageversion, v->value, sizeof(d->imageversion));
		} else if (!strcasecmp(v->name, "allow")) {
			opbx_parse_allow_disallow(&d->codecs, &d->capability, v->value, 1);
		} else if (!strcasecmp(v->name, "disallow")) {
			opbx_parse_allow_disallow(&d->codecs, &d->capability, v->value, 0);
		} else if (!strcasecmp(v->name, "transfer")) {
			d->transfer = sccp_true(v->value);
		} else if (!strcasecmp(v->name, "cfwdall")) {
			d->cfwdall = sccp_true(v->value);
		} else if (!strcasecmp(v->name, "cfwdbusy")) {
			d->cfwdbusy = sccp_true(v->value);
		} else if (!strcasecmp(v->name, "dnd")) {
			if (!strcasecmp(v->value, "reject")) {
				d->dndmode = SCCP_DNDMODE_REJECT;
			} else if (!strcasecmp(v->value, "silent")) {
				d->dndmode = SCCP_DNDMODE_SILENT;
			} else {
				/* 0 is off and 1 (on) is reject */
				d->dndmode = sccp_true(v->value);
			}
		} else if (!strcasecmp(v->name, "trustphoneip")) {
			d->trustphoneip = sccp_true(v->value);
		} else if (!strcasecmp(v->name, "private")) {
			d->private = sccp_true(v->value);
		} else if (!strcasecmp(v->name, "earlyrtp")) {
			if (!strcasecmp(v->value, "none"))
				d->earlyrtp = 0;
			else if (!strcasecmp(v->value, "offhook"))
				d->earlyrtp = SCCP_CHANNELSTATE_OFFHOOK;
			else if (!strcasecmp(v->value, "dial"))
				d->earlyrtp = SCCP_CHANNELSTATE_DIALING;
			else if (!strcasecmp(v->value, "ringout"))
				d->earlyrtp = SCCP_CHANNELSTATE_RINGOUT;
			else
				opbx_log(LOG_WARNING, "Invalid earlyrtp state value at line %d, should be 'none', 'offhook', 'dial', 'ringout'\n", v->lineno);
		} else if (!strcasecmp(v->name, "dtmfmode")) {
			if (!strcasecmp(v->value, "outofband"))
				d->dtmfmode = SCCP_DTMFMODE_OUTOFBAND;
	#ifdef CS_SCCP_PARK
		} else if (!strcasecmp(v->name, "park")) {
			d->park = sccp_true(v->value);
	#endif
		} else if (!strcasecmp(v->name, "speeddial")) {
			if (opbx_strlen_zero(v->value)) {
				speeddial_index++;
				opbx_verbose(VERBOSE_PREFIX_3 "Added empty speeddial\n");
			} else {
				opbx_copy_string(k_speed, v->value, sizeof(k_speed));
				splitter = k_speed;
				k_exten = strsep(&splitter, ",");
				k_name = strsep(&splitter, ",");
				k_hint = splitter;
				if (k_exten)
					opbx_strip(k_exten);
				if (k_name)
					opbx_strip(k_name);
				if (k_hint)
					opbx_strip(k_hint);
				if (k_exten && !opbx_strlen_zero(k_exten)) {
					if (!k_name)
						k_name = k_exten;
					k = malloc(sizeof(sccp_speed_t));
					if (!k)
						opbx_log(LOG_WARNING, "Error allocating speedial %s => %s\n", v->name, v->value);
					else {
						memset(k, 0, sizeof(sccp_speed_t));
						opbx_copy_string(k->name, k_name, sizeof(k->name));
						opbx_copy_string(k->ext, k_exten, sizeof(k->ext));
						if (k_hint)
							opbx_copy_string(k->hint, k_hint, sizeof(k->hint));
						k->config_instance = speeddial_index++;
						if (!d->speed_dials)
							d->speed_dials = k;
						if (!k_last)
							k_last = k;
						else {
							k_last->next = k;
							k_last = k;
						}
						opbx_verbose(VERBOSE_PREFIX_3 "Added speeddial %d: %s (%s)\n", k->config_instance, k->name, k->ext);
					}
				} else
					opbx_log(LOG_WARNING, "Wrong speedial syntax: %s => %s\n", v->name, v->value);
			}
		} else if (!strcasecmp(v->name, "mwilamp")) {
			if (!strcasecmp(v->value, "off"))
				d->mwilamp = SKINNY_LAMP_OFF;
			else if (!strcasecmp(v->value, "on"))
				d->mwilamp = SKINNY_LAMP_ON;
			else if (!strcasecmp(v->value, "wink"))
				d->mwilamp = SKINNY_LAMP_WINK;
			else if (!strcasecmp(v->value, "flash"))
				d->mwilamp = SKINNY_LAMP_FLASH;
			else if (!strcasecmp(v->value, "blink"))
				d->mwilamp = SKINNY_LAMP_BLINK;
			else
				opbx_log(LOG_WARNING, "Invalid mwilamp value at line %d, should be 'off', 'on', 'wink', 'flash' or 'blink'\n", v->lineno);
		} else if (!strcasecmp(v->name, "mwioncall")) {
				d->mwioncall = sccp_true(v->value);
		} else {
			opbx_log(LOG_WARNING, "Unknown param at line %d: %s = %s\n", v->lineno, v->name, v->value);
		}
		v = v->next;
	}

	if (d) {
		sccp_dev_clean(d);
		free(d);
		d = NULL;
	}

	v = opbx_variable_browse(cfg, "lines");
	if (!v) {
		opbx_log(LOG_WARNING, "Missing [lines] section, SCCP disabled\n");
		return 0;
	}

	l = build_line();
	while(v) {
		if (!strcasecmp(v->name, "line")) {
			if ( !opbx_strlen_zero(v->value) ) {
				opbx_copy_string(l->name, opbx_strip(v->value), sizeof(l->name));
				if (sccp_line_find_byname(v->value)) {
					opbx_log(LOG_WARNING, "The line %s already exists\n", l->name);
					free(l);
				} else {
					opbx_verbose(VERBOSE_PREFIX_3 "Added line '%s'\n", l->name);
					opbx_mutex_lock(&GLOB(lines_lock));
					l->next = GLOB(lines);
					if (l->next)
						l->next->prev = l;
					GLOB(lines) = l;
					opbx_mutex_unlock(&GLOB(lines_lock));
				}
			} else {
				opbx_log(LOG_WARNING, "Wrong line param: %s => %s\n", v->name, v->value);
				free(l);
			}
			l = build_line();
		} else if (!strcasecmp(v->name, "id")) {
			opbx_copy_string(l->id, v->value, sizeof(l->id));
		} else if (!strcasecmp(v->name, "pin")) {
			opbx_copy_string(l->pin, v->value, sizeof(l->pin));
		} else if (!strcasecmp(v->name, "label")) {
			opbx_copy_string(l->label, v->value, sizeof(l->label));
		} else if (!strcasecmp(v->name, "description")) {
			opbx_copy_string(l->description, v->value, sizeof(l->description));
		} else if (!strcasecmp(v->name, "context")) {
			opbx_copy_string(l->context, v->value, sizeof(l->context));
		} else if (!strcasecmp(v->name, "cid_name")) {
			opbx_copy_string(l->cid_name, v->value, sizeof(l->cid_name));
		} else if (!strcasecmp(v->name, "cid_num")) {
			opbx_copy_string(l->cid_num, v->value, sizeof(l->cid_num));
		} else if (!strcasecmp(v->name, "callerid")) {
			opbx_log(LOG_WARNING, "obsolete callerid param. Use cid_num and cid_name\n");
		} else if (!strcasecmp(v->name, "mailbox")) {
			opbx_copy_string(l->mailbox, v->value, sizeof(l->mailbox));
		} else if (!strcasecmp(v->name, "vmnum")) {
			opbx_copy_string(l->vmnum, v->value, sizeof(l->vmnum));
		} else if (!strcasecmp(v->name, "transfer")) {
			l->transfer = sccp_true(v->value);
		} else if (!strcasecmp(v->name, "incominglimit")) {
			l->incominglimit = atoi(v->value);
			if (l->incominglimit < 1)
			l->incominglimit = 1;
			/* this is the max call phone limits. Just a guess. It's not important */
			if (l->incominglimit > 10)
			l->incominglimit = 10;
		} else if (!strcasecmp(v->name, "echocancel")) {
				l->echocancel = sccp_true(v->value);
		} else if (!strcasecmp(v->name, "silencesuppression")) {
			l->silencesuppression = sccp_true(v->value);
		} else if (!strcasecmp(v->name, "rtptos")) {
			if (sscanf(v->value, "%d", &l->rtptos) == 1)
				l->rtptos &= 0xff;
		} else if (!strcasecmp(v->name, "language")) {
			opbx_copy_string(l->language, v->value, sizeof(l->language));
		} else if (!strcasecmp(v->name, "musicclass")) {
			opbx_copy_string(l->musicclass, v->value, sizeof(l->musicclass));
		} else if (!strcasecmp(v->name, "accountcode")) {
			opbx_copy_string(l->accountcode, v->value, sizeof(l->accountcode));
		} else if (!strcasecmp(v->name, "amaflags")) {
			amaflags = opbx_cdr_amaflags2int(v->value);
			if (amaflags < 0) {
				opbx_log(LOG_WARNING, "Invalid AMA Flags: %s at line %d\n", v->value, v->lineno);
			} else {
				l->amaflags = amaflags;
			}
		} else if (!strcasecmp(v->name, "callgroup")) {
			l->callgroup = opbx_get_group(v->value);
#ifdef CS_SCCP_PICKUP
		} else if (!strcasecmp(v->name, "pickupgroup")) {
			l->pickupgroup = opbx_get_group(v->value);
#endif
		} else if (!strcasecmp(v->name, "trnsfvm")) {
			if (!opbx_strlen_zero(v->value)) {
				if (opbx_exists_extension(NULL, l->context, v->value, 1, l->cid_num)) {
					l->trnsfvm = strdup(v->value);
				} else {
					opbx_log(LOG_WARNING, "trnsfvm: %s is not a valid extension. Disabled!\n", v->value);
				}
			}
		} else if (!strcasecmp(v->name, "secondary_dialtone_digits")) {
			if (strlen(v->value) > 9)
				opbx_log(LOG_WARNING, "secondary_dialtone_digits value '%s' is too long at line %d of SCCP.CONF. Max 9 digits\n", v->value, v->lineno);
			opbx_copy_string(l->secondary_dialtone_digits, v->value, sizeof(l->secondary_dialtone_digits));
		} else if (!strcasecmp(v->name, "secondary_dialtone_tone")) {
			if (sscanf(v->value, "%i", &secondary_dialtone_tone) == 1) {
				if (secondary_dialtone_tone >= 0 && secondary_dialtone_tone <= 255)
					l->secondary_dialtone_tone = secondary_dialtone_tone;
				else
					l->secondary_dialtone_tone = SKINNY_TONE_OUTSIDEDIALTONE;
			} else {
				opbx_log(LOG_WARNING, "Invalid secondary_dialtone_tone value '%s' at line %d of SCCP.CONF. Default is OutsideDialtone (0x22)\n", v->value, v->lineno);
			}
		} else {
			opbx_log(LOG_WARNING, "Unknown param at line %d: %s = %s\n", v->lineno, v->name, v->value);
		}
		v = v->next;
	}

	if (l) {
		free(l);
	}

	/* ok the config parse is done */

	if ((GLOB(descriptor) > -1) && (ntohs(GLOB(bindaddr.sin_port)) != oldport)) {
		close(GLOB(descriptor));
		GLOB(descriptor) = -1;
	}

  if (GLOB(descriptor) < 0) {

	GLOB(descriptor) = socket(AF_INET, SOCK_STREAM, 0);

	on = 1;
	if (setsockopt(GLOB(descriptor), SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
		opbx_log(LOG_WARNING, "Failed to set SCCP socket to SO_REUSEADDR mode: %s\n", strerror(errno));
	if (setsockopt(GLOB(descriptor), IPPROTO_IP, IP_TOS, &GLOB(tos), sizeof(GLOB(tos))) < 0)
		opbx_log(LOG_WARNING, "Failed to set SCCP socket TOS to IPTOS_LOWDELAY: %s\n", strerror(errno));
	if (setsockopt(GLOB(descriptor), IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) < 0)
		opbx_log(LOG_WARNING, "Failed to set SCCP socket to TCP_NODELAY: %s\n", strerror(errno));

	if (GLOB(descriptor) < 0) {

		opbx_log(LOG_WARNING, "Unable to create SCCP socket: %s\n", strerror(errno));

	} else {
		if (bind(GLOB(descriptor), (struct sockaddr *)&GLOB(bindaddr), sizeof(GLOB(bindaddr))) < 0) {
			opbx_log(LOG_WARNING, "Failed to bind to %s:%d: %s!\n",
			opbx_inet_ntoa(iabuf, sizeof(iabuf), GLOB(bindaddr.sin_addr)), ntohs(GLOB(bindaddr.sin_port)),
			strerror(errno));
			close(GLOB(descriptor));
			GLOB(descriptor) = -1;
			return 0;
		}
		opbx_verbose(VERBOSE_PREFIX_3 "SCCP channel driver up and running on %s:%d\n",
		opbx_inet_ntoa(iabuf, sizeof(iabuf), GLOB(bindaddr.sin_addr)), ntohs(GLOB(bindaddr.sin_port)));

		if (listen(GLOB(descriptor), DEFAULT_SCCP_BACKLOG)) {
			opbx_log(LOG_WARNING, "Failed to start listening to %s:%d: %s\n",
			opbx_inet_ntoa(iabuf, sizeof(iabuf), GLOB(bindaddr.sin_addr)), ntohs(GLOB(bindaddr.sin_port)),
			strerror(errno));
			close(GLOB(descriptor));
			GLOB(descriptor) = -1;
			return 0;
		}

		sccp_log(0)(VERBOSE_PREFIX_3 "SCCP listening on %s:%d\n", opbx_inet_ntoa(iabuf, sizeof(iabuf), GLOB(bindaddr.sin_addr)), ntohs(GLOB(bindaddr.sin_port)));
		opbx_pthread_create(&socket_thread,NULL, sccp_socket_thread, NULL);
	}
  }

  opbx_config_destroy(cfg);
  sccp_dev_dbclean();
  return 0;
}

static char *sccp_setcalledparty_descrip = "  SetCalledParty(\"Name\" <ext>) sets the name and number of the called party for use with chan_sccp\n";

static int sccp_setcalledparty_exec(struct opbx_channel *chan, void *data) {
  char tmp[256] = "";
  char * num, * name;
  sccp_channel_t * c = CS_OPBX_CHANNEL_PVT(chan);

  if (strcasecmp(chan->type, "SCCP") != 0)
	return 0;

  if (!data || !c)
	return 0;

  opbx_copy_string(tmp, (char *)data, sizeof(tmp));
  opbx_callerid_parse(tmp, &name, &num);

  sccp_channel_set_calledparty(c, name, num);

  return 0;
}

int reload(void) {
	return 0;
}

int load_module() {

	/* make globals */
	sccp_globals = malloc(sizeof(struct sccp_global_vars));
	if (!sccp_globals) {
		opbx_log(LOG_ERROR, "No free mamory for SCCP global vars. SCCP channel type disabled\n");
		return -1;
	}
	memset(sccp_globals,0,sizeof(struct sccp_global_vars));
	opbx_mutex_init(&GLOB(lock));
	opbx_mutex_init(&GLOB(sessions_lock));
	opbx_mutex_init(&GLOB(devices_lock));
	opbx_mutex_init(&GLOB(lines_lock));
	opbx_mutex_init(&GLOB(channels_lock));
	opbx_mutex_init(&GLOB(usecnt_lock));

	/* GLOB() is a macro for sccp_globals-> */

	GLOB(descriptor) = -1;
	GLOB(ourport) = 2000;
	GLOB(externrefresh) = 60;
	GLOB(keepalive)  = SCCP_KEEPALIVE;
	opbx_copy_string(GLOB(date_format), "D/M/YA", sizeof(GLOB(date_format)));
	opbx_copy_string(GLOB(context), "default", sizeof(GLOB(context)));
	opbx_copy_string(GLOB(servername), "Asterisk", sizeof(GLOB(servername)));

	/* Wait up to 16 seconds for first digit */
	GLOB(firstdigittimeout) = 16;
	/* How long to wait for following digits */
	GLOB(digittimeout) = 8;
	/* Yes, these are all that the phone supports (except it's own 'Wideband 256k') */
	GLOB(global_capability) = OPBX_FORMAT_ALAW|OPBX_FORMAT_ULAW|OPBX_FORMAT_G729A;

	GLOB(debug) = 1;
	GLOB(tos) = (0x68 & 0xff);
	GLOB(rtptos) = (184 & 0xff);
	GLOB(echocancel) = 1;
	GLOB(silencesuppression) = 0;
	GLOB(dndmode) = SCCP_DNDMODE_REJECT;
	GLOB(autoanswer_tone) = SKINNY_TONE_ZIP;
	GLOB(remotehangup_tone) = SKINNY_TONE_ZIP;
	GLOB(callwaiting_tone) = SKINNY_TONE_CALLWAITINGTONE;
	GLOB(private) = 1; /* permit private function */
	GLOB(mwilamp) = SKINNY_LAMP_ON;
	GLOB(protocolversion) = 3;

	if (!reload_config()) {
		if (opbx_channel_register(&sccp_tech)) {
			opbx_log(LOG_ERROR, "Unable to register channel class SCCP\n");
			return -1;
		}
	}

	sccp_register_cli();
	opbx_register_application("SetCalledParty", sccp_setcalledparty_exec, "Sets the name of the called party", sccp_setcalledparty_descrip);
	return 0;
}

int unload_module() {
	sccp_line_t * l;
	sccp_device_t * d;
	sccp_session_t * s;
	sccp_hint_t *h;
	char iabuf[INET_ADDRSTRLEN];
	
	opbx_channel_unregister(&sccp_tech);
	opbx_unregister_application("SetCalledParty");
	sccp_unregister_cli();

	opbx_mutex_lock(&GLOB(channels_lock));
	while (GLOB(channels))
		sccp_channel_delete(GLOB(channels));
	opbx_mutex_unlock(&GLOB(channels_lock));

	opbx_mutex_lock(&GLOB(lines_lock));
	while (GLOB(lines)) {
		l = GLOB(lines);
		GLOB(lines) = l->next;
		sccp_log(10)(VERBOSE_PREFIX_3 "SCCP: Removing line %s\n", l->name);
		while (l->hints) {
			h = l->hints;
			l->hints = l->hints->next;
			free(h);
		}
		if (l->cfwd_num)
			free(l->cfwd_num);
		if (l->trnsfvm)
			free(l->trnsfvm);
		free(l);
	}
	opbx_mutex_unlock(&GLOB(lines_lock));

	opbx_mutex_lock(&GLOB(devices_lock));
	while (GLOB(devices)) {
		d = GLOB(devices);
		GLOB(devices) = d->next;
		sccp_log(10)(VERBOSE_PREFIX_3 "SCCP: Removing device %s\n", d->id);
		sccp_dev_clean(d);
		free(d);
	}
	opbx_mutex_unlock(&GLOB(devices_lock));

	opbx_mutex_lock(&GLOB(sessions_lock));
	while (GLOB(sessions)) {
		s = GLOB(sessions);
		GLOB(sessions) = s->next;
		sccp_log(10)(VERBOSE_PREFIX_3 "SCCP: Removing session %s\n", opbx_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr));
		if (s->fd > -1)
			close(s->fd);
		free(s);
	}
	opbx_mutex_unlock(&GLOB(sessions_lock));
	close(GLOB(descriptor));
	GLOB(descriptor) = -1;

	if (!opbx_mutex_lock(&GLOB(socket_lock))) {
		if (socket_thread && (socket_thread != OPBX_PTHREADT_STOP)) {
			pthread_cancel(socket_thread);
			pthread_kill(socket_thread, SIGURG);
			pthread_join(socket_thread, NULL);
		}
		socket_thread = OPBX_PTHREADT_STOP;
		opbx_mutex_unlock(&GLOB(socket_lock));
	} else {
		opbx_log(LOG_WARNING, "SCCP: Unable to lock the socket\n");
		return -1;
	}

	if (GLOB(ha))
		opbx_free_ha(GLOB(ha));

	if (GLOB(localaddr))
		opbx_free_ha(GLOB(localaddr));

	free(sccp_globals);

	return 0;
}

int usecount() {
	int res;
	opbx_mutex_lock(&GLOB(usecnt_lock));
	res = GLOB(usecnt);
	opbx_mutex_unlock(&GLOB(usecnt_lock));
	return res;
}

char *description() {
	return ("Skinny Client Control Protocol (SCCP). Release: " SCCP_VERSION);
}
