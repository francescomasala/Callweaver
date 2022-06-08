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
#include "sccp_cli.h"
#include "sccp_indicate.h"
#include "sccp_utils.h"
#include "sccp_device.h"
#include <openpbx/utils.h>
#include <openpbx/cli.h>
#include <openpbx/opbxdb.h>

/* ------------------------------------------------------------ */

static int sccp_reset_restart(int fd, int argc, char * argv[]) {
  sccp_moo_t * r;
  sccp_device_t * d;

  if (argc != 3)
	return RESULT_SHOWUSAGE;

  d = sccp_device_find_byid(argv[2]);

  if (!d) {
	opbx_cli(fd, "Can't find device %s\n", argv[2]);
	return RESULT_SUCCESS;
  }

  REQ(r, Reset);
  r->msg.Reset.lel_resetType = htolel((!strcasecmp(argv[1], "reset")) ? SKINNY_DEVICE_RESET : SKINNY_DEVICE_RESTART);
  sccp_dev_send(d, r);

  opbx_cli(fd, "%s: Reset request sent to the device\n", argv[2]);
  return RESULT_SUCCESS;

}

/* ------------------------------------------------------------ */

static char *sccp_print_group(char *buf, int buflen, opbx_group_t group) {
	unsigned int i;
	int first=1;
	char num[3];
	uint8_t max = (sizeof(opbx_group_t) * 8) - 1;

	buf[0] = '\0';
	
	if (!group)
		return(buf);

	for (i=0; i<=max; i++) {
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

static int sccp_show_globals(int fd, int argc, char * argv[]) {
	char pref_buf[128];
	char cap_buf[512];
	char buf[256];
	char iabuf[INET_ADDRSTRLEN];

	opbx_mutex_lock(&GLOB(lock));
	opbx_codec_pref_string(&GLOB(global_codecs), pref_buf, sizeof(pref_buf) - 1);
	opbx_getformatname_multiple(cap_buf, sizeof(cap_buf), GLOB(global_capability)),

	opbx_cli(fd, "SCCP channel driver global settings\n");
	opbx_cli(fd, "------------------------------------\n\n");
#if SCCP_PLATFORM_BYTE_ORDER == SCCP_LITTLE_ENDIAN
	opbx_cli(fd, "Platform byte order   : LITTLE ENDIAN\n");
#else
	opbx_cli(fd, "Platform byte order   : BIG ENDIAN\n");
#endif
	opbx_cli(fd, "Protocol Version      : %d\n", GLOB(protocolversion));
	opbx_cli(fd, "Server Name           : %s\n", GLOB(servername));
	opbx_cli(fd, "Bind Address          : %s:%d\n", opbx_inet_ntoa(iabuf, sizeof(iabuf), GLOB(bindaddr.sin_addr)), ntohs(GLOB(bindaddr.sin_port)));
	opbx_cli(fd, "Keepalive             : %d\n", GLOB(keepalive));
	opbx_cli(fd, "Debug level           : %d\n", GLOB(debug));
	opbx_cli(fd, "Date format           : %s\n", GLOB(date_format));
	opbx_cli(fd, "First digit timeout   : %d\n", GLOB(firstdigittimeout));
	opbx_cli(fd, "Digit timeout         : %d\n", GLOB(digittimeout));
	opbx_cli(fd, "RTP tos               : %d\n", GLOB(rtptos));
	opbx_cli(fd, "Context               : %s\n", GLOB(context));
	opbx_cli(fd, "Language              : %s\n", GLOB(language));
	opbx_cli(fd, "Accountcode           : %s\n", GLOB(accountcode));
	opbx_cli(fd, "Musicclass            : %s\n", GLOB(musicclass));
	opbx_cli(fd, "AMA flags             : %d - %s\n", GLOB(amaflags), opbx_cdr_flags2str(GLOB(amaflags)));
	opbx_cli(fd, "Callgroup             : %s\n", sccp_print_group(buf, sizeof(buf), GLOB(callgroup)));
#ifdef CS_SCCP_PICKUP
	opbx_cli(fd, "Pickupgroup           : %s\n", sccp_print_group(buf, sizeof(buf), GLOB(pickupgroup)));
#endif
	opbx_cli(fd, "Capabilities          : %s\n", cap_buf);
	opbx_cli(fd, "Codecs preference     : %s\n", pref_buf);
	opbx_cli(fd, "DND                   : %s\n", GLOB(dndmode) ? sccp_dndmode2str(GLOB(dndmode)) : "Disabled");
#ifdef CS_SCCP_PARK
	opbx_cli(fd, "Park                  : Enabled\n");
#else
	opbx_cli(fd, "Park                  : Disabled\n");
#endif
	opbx_cli(fd, "Private softkey       : %s\n", GLOB(private) ? "Enabled" : "Disabled");
	opbx_cli(fd, "Echo cancel           : %s\n", GLOB(echocancel) ? "Enabled" : "Disabled");
	opbx_cli(fd, "Silence suppression   : %s\n", GLOB(silencesuppression) ? "Enabled" : "Disabled");
	opbx_cli(fd, "Trust phone ip        : %s\n", GLOB(trustphoneip) ? "Yes" : "No");
	opbx_cli(fd, "Early RTP             : %s\n", GLOB(earlyrtp) ? "Yes" : "No");
	opbx_cli(fd, "AutoAnswer ringtime   : %d\n", GLOB(autoanswer_ring_time));
	opbx_cli(fd, "AutoAnswer tone       : %d\n", GLOB(autoanswer_tone));
	opbx_cli(fd, "RemoteHangup tone     : %d\n", GLOB(remotehangup_tone));
	opbx_cli(fd, "Transfer tone         : %d\n", GLOB(transfer_tone));
	opbx_cli(fd, "CallWaiting tone      : %d\n", GLOB(callwaiting_tone));
	opbx_mutex_unlock(&GLOB(lock));
	return RESULT_SUCCESS;
}

static struct opbx_cli_entry cli_show_globals = {
  { "sccp", "show", "globals", NULL },
  sccp_show_globals,
  "Show SCCP global settings",
  "Usage: sccp show globals\n"
};

/* ------------------------------------------------------------ */

static int sccp_show_device(int fd, int argc, char * argv[]) {
	sccp_device_t * d;
	sccp_speed_t * k;
	sccp_line_t * l;
	char pref_buf[128];
	char cap_buf[512];

	if (argc != 4)
		return RESULT_SHOWUSAGE;

	d = sccp_device_find_byid(argv[3]);
	if (!d) {
		opbx_cli(fd, "Can't find settings for device %s\n", argv[3]);
		return RESULT_SUCCESS;
	}
	opbx_mutex_lock(&d->lock);
	opbx_codec_pref_string(&d->codecs, pref_buf, sizeof(pref_buf) - 1);
	opbx_getformatname_multiple(cap_buf, sizeof(cap_buf), d->capability),

	opbx_cli(fd, "Current settings for selected Device\n");
	opbx_cli(fd, "------------------------------------\n\n");
	opbx_cli(fd, "MAC-Address        : %s\n", d->id);
	opbx_cli(fd, "Protocol Version   : phone=%d, channel=%d\n", d->protocolversion, GLOB(protocolversion));
	opbx_cli(fd, "Registration state : %s(%d)\n", skinny_registrationstate2str(d->registrationState), d->registrationState);
	opbx_cli(fd, "State              : %s(%d)\n", skinny_devicestate2str(d->state), d->state);
	opbx_cli(fd, "MWI handset light  : %s\n", (d->mwilight) ? "ON" : "OFF");
	opbx_cli(fd, "Description        : %s\n", d->description);
	opbx_cli(fd, "Config Phone Type  : %s\n", d->config_type);
	opbx_cli(fd, "Skinny Phone Type  : %s(%d)\n", skinny_devicetype2str(d->skinny_type), d->skinny_type);
	opbx_cli(fd, "Softkey support    : %s\n", (d->softkeysupport) ? "Yes" : "No");
	opbx_cli(fd, "Autologin          : %s\n", d->autologin);
	opbx_cli(fd, "Image Version      : %s\n", d->imageversion);
	opbx_cli(fd, "Timezone Offset    : %d\n", d->tz_offset);
	opbx_cli(fd, "Capabilities       : %s\n", cap_buf);
	opbx_cli(fd, "Codecs preference  : %s\n", pref_buf);
	opbx_cli(fd, "Can DND            : %s\n", (d->dndmode) ? sccp_dndmode2str(d->dndmode) : "Disabled");
	opbx_cli(fd, "Can Transfer       : %s\n", (d->transfer) ? "Yes" : "No");
	opbx_cli(fd, "Can Park           : %s\n", (d->park) ? "Yes" : "No");
	opbx_cli(fd, "Private softkey    : %s\n", d->private ? "Enabled" : "Disabled");
	opbx_cli(fd, "Can CFWDALL        : %s\n", (d->cfwdall) ? "Yes" : "No");
	opbx_cli(fd, "Can CFWBUSY        : %s\n", (d->cfwdbusy) ? "Yes" : "No");
	opbx_cli(fd, "Dtmf mode          : %s\n", (d->dtmfmode) ? "Out-of-Band" : "In-Band");
	opbx_cli(fd, "Trust phone ip     : %s\n", (d->trustphoneip) ? "Yes" : "No");
	opbx_cli(fd, "Early RTP          : %s\n", (d->earlyrtp) ? "Yes" : "No");

	l = d->lines;
	if (l) {
		opbx_cli(fd, "\nLines\n");
		opbx_cli(fd, "%-4s: %-20s %-20s\n", "id", "name" , "label");
		opbx_cli(fd, "------------------------------------\n");
		while (l) {
			opbx_cli(fd, "%4d: %-20s %-20s\n", l->instance, l->name , l->label);
			l = l->next_on_device;
		}
	}
	k = d->speed_dials;
	if (k) {
		opbx_cli(fd, "\nSpeedials\n");
		opbx_cli(fd, "%-4s: %-20s %-20s\n", "id", "name" , "number");
		opbx_cli(fd, "------------------------------------\n");
		while (k) {
			opbx_cli(fd, "%4d: %-20s %-20s\n", k->instance, k->name , k->ext);
			k = k->next;
		}
	}
	opbx_mutex_unlock(&d->lock);
	return RESULT_SUCCESS;
}

static struct opbx_cli_entry cli_show_device = {
  { "sccp", "show", "device", NULL },
  sccp_show_device,
  "Show SCCP Device Information",
  "Usage: sccp show device <deviceId>\n"
};

/* ------------------------------------------------------------ */


static struct opbx_cli_entry cli_reset = {
  { "sccp", "reset", NULL },
  sccp_reset_restart,
  "Reset an SCCP device",
  "Usage: sccp reset <deviceId>\n"
};

static struct opbx_cli_entry cli_restart = {
	{ "sccp", "restart", NULL },
	sccp_reset_restart,
	"Reset an SCCP device",
	"Usage: sccp restart <deviceId>\n"
};

/* ------------------------------------------------------------ */

static int sccp_show_channels(int fd, int argc, char * argv[]) {
	sccp_channel_t * c;

	opbx_cli(fd, "\n%-5s %-10s %-16s %-16s %-16s %-10s\n", "ID","LINE","DEVICE","AST STATE","SCCP STATE","CALLED");
	opbx_cli(fd, "===== ========== ================ ================ ================ ========== \n");

	opbx_mutex_lock(&GLOB(channels_lock));
	c = GLOB(channels);
	while(c) {
		opbx_cli(fd, "%.5d %-10s %-16s %-16s %-16s %-10s\n",
			c->callid,
			c->line->name,
			c->line->device->description,
			(c->owner) ? opbx_state2str(c->owner->_state) : "No channel",
			sccp_indicate2str(c->state),
			c->calledPartyNumber);
		c = c->next;
	}
	opbx_mutex_unlock(&GLOB(channels_lock));
	return RESULT_SUCCESS;
}

static struct opbx_cli_entry cli_show_channels = {
	{ "sccp", "show", "channels", NULL },
	sccp_show_channels,
	"Show all SCCP channels",
	"Usage: sccp show channel\n",
};

/* ------------------------------------------------------------ */

static int sccp_show_devices(int fd, int argc, char * argv[]) {
	sccp_device_t * d;
	char iabuf[INET_ADDRSTRLEN];

	opbx_cli(fd, "\n%-16s %-15s %-16s %-10s\n", "NAME","ADDRESS","MAC","Reg. State");
	opbx_cli(fd, "================ =============== ================ ==========\n");

	opbx_mutex_lock(&GLOB(devices_lock));
	d = GLOB(devices);
	while (d) {
		opbx_cli(fd, "%-16s %-15s %-16s %-10s\n",// %-10s %-16s %c%c %-10s\n",
			d->description,
			(d->session) ? opbx_inet_ntoa(iabuf, sizeof(iabuf), d->session->sin.sin_addr) : "--",
			d->id,
			skinny_registrationstate2str(d->registrationState)
		);
		d = d->next;
	}
	opbx_mutex_unlock(&GLOB(devices_lock));
	return RESULT_SUCCESS;
}

static struct opbx_cli_entry cli_show_devices = {
	{ "sccp", "show", "devices", NULL },
	sccp_show_devices,
	"Show all SCCP Devices",
	"Usage: sccp show devices\n"
};

static int sccp_message_devices(int fd, int argc, char * argv[]) {
	sccp_device_t * d;
	int msgtimeout=10;

	if (argc < 4)
		return RESULT_SHOWUSAGE;
		
	if (opbx_strlen_zero(argv[3]))
		return RESULT_SHOWUSAGE;
		
	if (argc == 5 && sscanf(argv[4], "%d", &msgtimeout) != 1) {
		msgtimeout=10;
	}

	opbx_mutex_lock(&GLOB(devices_lock));
	d = GLOB(devices);
	while (d) {
		sccp_dev_displaynotify(d,argv[3],msgtimeout);
		d = d->next;
	}
	opbx_mutex_unlock(&GLOB(devices_lock));
	return RESULT_SUCCESS;
}

static struct opbx_cli_entry cli_message_devices = {
  { "sccp", "message", "devices", NULL },
  sccp_message_devices,
  "Send a message to all SCCP Devices",
  "Usage: sccp messages devices <message text> <timeout>\n"
};



/* ------------------------------------------------------------ */

static int sccp_show_lines(int fd, int argc, char * argv[]) {
	sccp_line_t * l = NULL;
	sccp_channel_t * c = NULL;
	sccp_device_t * d = NULL;
	char cap_buf[512];

	opbx_cli(fd, "\n%-16s %-16s %-4s %-4s %-16s\n", "NAME","DEVICE","MWI","Chs","Active Channel");
	opbx_cli(fd, "================ ================ ==== ==== =================================================\n");

	opbx_mutex_lock(&GLOB(lines_lock));
	l = GLOB(lines);

	while (l) {
		opbx_mutex_lock(&l->lock);
		c = NULL;
		d = l->device;
		if (d) {
			opbx_mutex_lock(&d->lock);
			c = d->active_channel;
			opbx_mutex_unlock(&d->lock);
		}

		if (!c || (c->line != l))
		c = NULL;
		memset(&cap_buf, 0, sizeof(cap_buf));
		if (c && c->owner)
			opbx_getformatname_multiple(cap_buf, sizeof(cap_buf),  c->owner->nativeformats);

		opbx_cli(fd, "%-16s %-16s %-4s %-4d %-10s %-10s %-16s %-10s\n",
			l->name,
			(l->device) ? l->device->id : "--",
			(l->mwilight) ? "ON" : "OFF",
			l->channelCount,
			(c) ? sccp_indicate2str(c->state) : "--",
			(c) ? skinny_calltype2str(c->calltype) : "",
			(c) ? ( (c->calltype == SKINNY_CALLTYPE_OUTBOUND) ? c->calledPartyName : c->callingPartyName ) : "",
			cap_buf);

		opbx_mutex_unlock(&l->lock);
		l = l->next;
  }

  opbx_mutex_unlock(&GLOB(lines_lock));
  return RESULT_SUCCESS;
}

static struct opbx_cli_entry cli_show_lines = {
  { "sccp", "show", "lines", NULL },
  sccp_show_lines,
  "Show All SCCP Lines",
  "Usage: sccp show lines\n"
};

/* ------------------------------------------------------------ */

static int sccp_show_sessions(int fd, int argc, char * argv[]) {
	sccp_session_t * s = NULL;
	sccp_device_t * d = NULL;
	char iabuf[INET_ADDRSTRLEN];

	opbx_cli(fd, "%-10s %-15s %-4s %-15s %-15s %-15s\n", "Socket", "IP", "KA", "DEVICE", "STATE", "TYPE");
	opbx_cli(fd, "========== =============== ==== =============== =============== ===============\n");

	opbx_mutex_lock(&GLOB(sessions_lock));
	s = GLOB(sessions);

	while (s) {
		opbx_mutex_lock(&s->lock);
		d = s->device;
		if (d)
			opbx_mutex_lock(&d->lock);
		opbx_cli(fd, "%-10d %-15s %-4d %-15s %-15s %-15s\n",
			s->fd,
			opbx_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr),
			(uint32_t)(time(0) - s->lastKeepAlive),
			(d) ? d->id : "--",
			(d) ? skinny_devicestate2str(d->state) : "--",
			(d) ? skinny_devicetype2str(d->skinny_type) : "--");
		if (d)
			opbx_mutex_unlock(&d->lock);
		opbx_mutex_unlock(&s->lock);
		s = s->next;
	}
	opbx_mutex_unlock(&GLOB(sessions_lock));
	return RESULT_SUCCESS;
}

static struct opbx_cli_entry cli_show_sessions = {
  { "sccp", "show", "sessions", NULL },
  sccp_show_sessions,
  "Show All SCCP Sessions",
  "Usage: sccp show sessions\n"
};

/* ------------------------------------------------------------ */
static int sccp_system_message(int fd, int argc, char * argv[]) {
	int res;
	int timeout = 0;
	if ((argc < 3) || (argc > 5))
		return RESULT_SHOWUSAGE;

	if (argc == 3) {
		res = opbx_db_deltree("SCCP", "message");
		if (res) {
			opbx_cli(fd, "Failed to delete the SCCP system message!\n");
			return RESULT_FAILURE;
		}
		opbx_cli(fd, "SCCP system message deleted!\n");
		return RESULT_SUCCESS;
	}

	if (opbx_strlen_zero(argv[3]))
		return RESULT_SHOWUSAGE;

	res = opbx_db_put("SCCP/message", "text", argv[3]);
	if (res) {
		opbx_cli(fd, "Failed to store the SCCP system message text\n");
	} else {
		opbx_cli(fd, "SCCP system message text stored successfully\n");
	}
	if (argc == 5) {
		if (sscanf(argv[4], "%d", &timeout) != 1)
			return RESULT_SHOWUSAGE;
		res = opbx_db_put("SCCP/message", "timeout", argv[4]);
		if (res) {
			opbx_cli(fd, "Failed to store the SCCP system message timeout\n");
		} else {
			opbx_cli(fd, "SCCP system message timeout stored successfully\n");
		}
	} else {
		opbx_db_del("SCCP/message", "timeout");
	}
	return RESULT_SUCCESS;
}

static struct opbx_cli_entry cli_system_message = {
   { "sccp", "system", "message", NULL },
   sccp_system_message,
   "Set the SCCP system message",
   "Usage: sccp system message \"<message text>\" <timeout>\nThe default optional timeout is 0 (forever)\nExample: sccp system message \"The boss is gone. Let's have some fun!\"  10\n"
};

/* ------------------------------------------------------------ */

static char debug_usage[] =
"Usage: SCCP debug <level>\n"
"		Set the debug level of the sccp protocol from none (0) to high (10)\n";

static char no_debug_usage[] =
"Usage: SCCP no debug\n"
"		Disables dumping of SCCP packets for debugging purposes\n";

static int sccp_do_debug(int fd, int argc, char *argv[]) {
	int new_debug = 10;

	if ((argc < 2) || (argc > 3))
		return RESULT_SHOWUSAGE;

	if (argc == 3) {
		if (sscanf(argv[2], "%d", &new_debug) != 1)
			return RESULT_SHOWUSAGE;
		new_debug = (new_debug > 10) ? 10 : new_debug;
		new_debug = (new_debug < 0) ? 0 : new_debug;
	}

	opbx_cli(fd, "SCCP debug level was %d now %d\n", GLOB(debug), new_debug);
	GLOB(debug) = new_debug;
	return RESULT_SUCCESS;
}

static struct opbx_cli_entry cli_do_debug = {
  { "sccp", "debug", NULL },
  sccp_do_debug,
  "Enable SCCP debugging",
  debug_usage };

static int sccp_no_debug(int fd, int argc, char *argv[]) {
	if (argc != 3)
		return RESULT_SHOWUSAGE;

	GLOB(debug) = 0;
	opbx_cli(fd, "SCCP Debugging Disabled\n");
	return RESULT_SUCCESS;
}

static struct opbx_cli_entry cli_no_debug = {
  { "sccp", "no", "debug", NULL },
  sccp_no_debug,
  "Disable SCCP debugging",
  no_debug_usage };

static int sccp_do_reload(int fd, int argc, char *argv[]) {
	opbx_cli(fd, "SCCP configuration reload not implemented yet! use unload and load.\n");
	return RESULT_SUCCESS;
}

static char reload_usage[] =
"Usage: sccp reload\n"
"		Reloads SCCP configuration from sccp.conf (It will close all active connections)\n";

static struct opbx_cli_entry cli_reload = {
  { "sccp", "reload", NULL },
  sccp_do_reload,
  "SCCP module reload",
  reload_usage };

static char version_usage[] =
"Usage: SCCP show version\n"
"		Show the SCCP channel version\n";

static int sccp_show_version(int fd, int argc, char *argv[]) {
	opbx_cli(fd, "SCCP channel version: %s\n", SCCP_VERSION);
	return RESULT_SUCCESS;
}

static struct opbx_cli_entry cli_show_version = {
  { "sccp", "show", "version", NULL },
  sccp_show_version,
  "SCCP show version",
  version_usage };

void sccp_register_cli(void) {
  opbx_cli_register(&cli_show_channels);
  opbx_cli_register(&cli_show_devices);
  opbx_cli_register(&cli_show_lines);
  opbx_cli_register(&cli_show_sessions);
  opbx_cli_register(&cli_show_device);
  opbx_cli_register(&cli_show_version);
  opbx_cli_register(&cli_reload);
  opbx_cli_register(&cli_restart);
  opbx_cli_register(&cli_reset);
  opbx_cli_register(&cli_do_debug);
  opbx_cli_register(&cli_no_debug);
  opbx_cli_register(&cli_system_message);
  opbx_cli_register(&cli_show_globals);
  opbx_cli_register(&cli_message_devices);

}

void sccp_unregister_cli(void) {
  opbx_cli_unregister(&cli_show_channels);
  opbx_cli_unregister(&cli_show_devices);
  opbx_cli_unregister(&cli_show_lines);
  opbx_cli_unregister(&cli_show_sessions);
  opbx_cli_unregister(&cli_show_device);
  opbx_cli_unregister(&cli_show_version);
  opbx_cli_unregister(&cli_reload);
  opbx_cli_unregister(&cli_restart);
  opbx_cli_unregister(&cli_reset);
  opbx_cli_unregister(&cli_do_debug);
  opbx_cli_unregister(&cli_no_debug);
  opbx_cli_unregister(&cli_system_message);
  opbx_cli_unregister(&cli_show_globals);
  opbx_cli_unregister(&cli_message_devices);
}
