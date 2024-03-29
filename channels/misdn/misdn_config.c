/*
 * CallWeaver -- An open source telephony toolkit.
 * 
 * Copyright (C) 2005, Christian Richter
 *
 * Christian Richter <crich@beronet.com>
 *
 * See http://www.callweaver.org for more information about
 * the CallWeaver project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 *
 */

/*!
 * \file
 *
 * \brief chan_misdn configuration management
 * \author Christian Richter <crich@beronet.com>
 *
 * \ingroup channel_drivers
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "chan_misdn_config.h"

#include "callweaver/config.h"
#include "callweaver/channel.h"
#include "callweaver/logger.h"
#include "callweaver/lock.h"
#include "callweaver/pbx.h"
#include "callweaver/strings.h"
#include "callweaver/utils.h"

#define CW_LOAD_CFG cw_config_load
#define CW_DESTROY_CFG cw_config_destroy

#define NO_DEFAULT "<>"
#define NONE 0

#define GEN_CFG 1
#define PORT_CFG 2
#define NUM_GEN_ELEMENTS (sizeof(gen_spec) / sizeof(struct misdn_cfg_spec))
#define NUM_PORT_ELEMENTS (sizeof(port_spec) / sizeof(struct misdn_cfg_spec))

enum misdn_cfg_type {
	MISDN_CTYPE_STR,
	MISDN_CTYPE_INT,
	MISDN_CTYPE_BOOL,
	MISDN_CTYPE_BOOLINT,
	MISDN_CTYPE_MSNLIST,
	MISDN_CTYPE_ASTGROUP
};

struct msn_list {
	char *msn;
	struct msn_list *next;
};

union misdn_cfg_pt {
	char *str;
	int *num;
	struct msn_list *ml;
	cw_group_t *grp;
	void *any;
};

struct misdn_cfg_spec {
	char name[BUFFERSIZE];
	enum misdn_cfg_elements elem;
	enum misdn_cfg_type type;
	char def[BUFFERSIZE];
	int boolint_def;
	char desc[BUFFERSIZE];
};

static const char ports_description[] =
	"Define your ports, e.g. 1,2 (depends on mISDN-driver loading order).";

static const struct misdn_cfg_spec port_spec[] = {
	{ "name", MISDN_CFG_GROUPNAME, MISDN_CTYPE_STR, "default", NONE,
		"Name of the portgroup." },
	{ "allowed_bearers", MISDN_CFG_ALLOWED_BEARERS, MISDN_CTYPE_STR, "all", NONE,
		"Here you can define which bearers should be allowed." },
	{ "rxgain", MISDN_CFG_RXGAIN, MISDN_CTYPE_INT, "0", NONE,
		"Set this between -8 and 8 to change the RX Gain." },
	{ "txgain", MISDN_CFG_TXGAIN, MISDN_CTYPE_INT, "0", NONE,
		"Set this between -8 and 8 to change the TX Gain." },
	{ "te_choose_channel", MISDN_CFG_TE_CHOOSE_CHANNEL, MISDN_CTYPE_BOOL, "no", NONE,
		"Some telcos espacially in NL seem to need this set to yes,\n"
		"\talso in switzerland this seems to be important." },
	{ "far_alerting", MISDN_CFG_FAR_ALERTING, MISDN_CTYPE_BOOL, "no", NONE,
		"If we should generate ringing for chan_sip and others." },
	{ "pmp_l1_check", MISDN_CFG_PMP_L1_CHECK, MISDN_CTYPE_BOOL, "yes", NONE,
		"This option defines, if chan_misdn should check the L1 on a PMP\n"
		"\tbefore makeing a group call on it. The L1 may go down for PMP Ports\n"
		"\tso we might need this.\n"
		"\tBut be aware! a broken or plugged off cable might be used for a group call\n"
		"\tas well, since chan_misdn has no chance to distinguish if the L1 is down\n"
		"\tbecause of a lost Link or because the Provider shut it down..." },
	{ "block_on_alarm", MISDN_CFG_ALARM_BLOCK, MISDN_CTYPE_BOOL, "yes", NONE,
		"If the port should be blocked, whenever an Alarm comes up."},
	{ "hdlc", MISDN_CFG_HDLC, MISDN_CTYPE_BOOL, "no", NONE,
		"Set this to yes, if you want to bridge a mISDN data channel to\n"
		"\tanother channel type or to an application." },
	{ "context", MISDN_CFG_CONTEXT, MISDN_CTYPE_STR, "default", NONE,
		"Context to use for incoming calls." },
	{ "language", MISDN_CFG_LANGUAGE, MISDN_CTYPE_STR, "en", NONE,
		"Language." },
	{ "musicclass", MISDN_CFG_MUSICCLASS, MISDN_CTYPE_STR, "default", NONE,
		"Sets the musiconhold class." },
	{ "callerid", MISDN_CFG_CALLERID, MISDN_CTYPE_STR, "", NONE,
		"Sets the caller ID." },
	{ "method", MISDN_CFG_METHOD, MISDN_CTYPE_STR, "standard", NONE,
		"Sets the method to use for channel selection:\n"
		"\t  standard    - always choose the first free channel with the lowest number\n"
		"\t  round_robin - use the round robin algorithm to select a channel. use this\n"
		"\t                if you want to balance your load." },
	{ "dialplan", MISDN_CFG_DIALPLAN, MISDN_CTYPE_INT, "0", NONE,
		"Dialplan means Type Of Number in ISDN Terms (for outgoing calls)\n"
		"\n"
		"\tThere are different types of the dialplan:\n"
		"\n"
		"\tdialplan -> outgoing Number\n"
		"\tlocaldialplan -> callerid\n"
		"\tcpndialplan -> connected party number\n"
		"\n"
		"\tdialplan options:\n"
		"\n"
		"\t0 - unknown\n"
		"\t1 - International\n"
		"\t2 - National\n"
		"\t4 - Subscriber\n"
		"\n"
		"\tThis setting is used for outgoing calls." },
	{ "localdialplan", MISDN_CFG_LOCALDIALPLAN, MISDN_CTYPE_INT, "0", NONE,
		"Dialplan means Type Of Number in ISDN Terms (for outgoing calls)\n"
		"\n"
		"\tThere are different types of the dialplan:\n"
		"\n"
		"\tdialplan -> outgoing Number\n"
		"\tlocaldialplan -> callerid\n"
		"\tcpndialplan -> connected party number\n"
		"\n"
		"\tdialplan options:\n"
		"\n"
		"\t0 - unknown\n"
		"\t1 - International\n"
		"\t2 - National\n"
		"\t4 - Subscriber\n"
		"\n"
		"\tThis setting is used for outgoing calls" },
	{ "cpndialplan", MISDN_CFG_CPNDIALPLAN, MISDN_CTYPE_INT, "0", NONE,
		"Dialplan means Type Of Number in ISDN Terms (for outgoing calls)\n"
		"\n"
		"\tThere are different types of the dialplan:\n"
		"\n"
		"\tdialplan -> outgoing Number\n"
		"\tlocaldialplan -> callerid\n"
		"\tcpndialplan -> connected party number\n"
		"\n"
		"\tdialplan options:\n"
		"\n"
		"\t0 - unknown\n"
		"\t1 - International\n"
		"\t2 - National\n"
		"\t4 - Subscriber\n"
		"\n"
		"\tThis setting is used for outgoing calls." },
	{ "nationalprefix", MISDN_CFG_NATPREFIX, MISDN_CTYPE_STR, "0", NONE,
		"Prefix for national, this is put before the\n"
		"\toad if an according dialplan is set by the other end." },
	{ "internationalprefix", MISDN_CFG_INTERNATPREFIX, MISDN_CTYPE_STR, "00", NONE,
		"Prefix for international, this is put before the\n"
		"\toad if an according dialplan is set by the other end." },
	{ "presentation", MISDN_CFG_PRES, MISDN_CTYPE_INT, "-1", NONE,
		"These (presentation and screen) are the exact isdn screening and presentation\n"
		"\tindicators.\n"
		"\tIf -1 is given for both values, the presentation indicators are used from\n"
		"\tCallWeavers SetCallerPres application.\n"
		"\n"
		"\tscreen=0, presentation=0 -> callerid presented not screened\n"
		"\tscreen=1, presentation=1 -> callerid presented but screened (the remote end doesn't see it!)" },
	{ "screen", MISDN_CFG_SCREEN, MISDN_CTYPE_INT, "-1", NONE,
		"These (presentation and screen) are the exact isdn screening and presentation\n"
		"\tindicators.\n"
		"\tIf -1 is given for both values, the presentation indicators are used from\n"
		"\tCallWeavers SetCallerPres application.\n"
		"\n"
		"\tscreen=0, presentation=0 -> callerid presented not screened\n"
		"\tscreen=1, presentation=1 -> callerid presented but screened (the remote end doesn't see it!)" },
	{ "always_immediate", MISDN_CFG_ALWAYS_IMMEDIATE, MISDN_CTYPE_BOOL, "no", NONE,
		"Enable this to get into the s dialplan-extension.\n"
		"\tThere you can use DigitTimeout if you can't or don't want to use\n"
		"\tisdn overlap dial.\n"
		"\tNOTE: This will jump into the s extension for every exten!" },
	{ "nodialtone", MISDN_CFG_NODIALTONE, MISDN_CTYPE_BOOL, "no", NONE,
		"Enable this to prevent chan_misdn to generate the dialtone\n"
		"\tThis makes only sense together with the always_immediate=yes option\n"
		"\tto generate your own dialtone with Playtones or so."},
	{ "immediate", MISDN_CFG_IMMEDIATE, MISDN_CTYPE_BOOL, "no", NONE,
		"Enable this if you want callers which called exactly the base\n"
		"\tnumber (so no extension is set) to jump into the s extension.\n"
		"\tIf the user dials something more, it jumps to the correct extension\n"
		"\tinstead." },
	{ "senddtmf", MISDN_CFG_SENDDTMF, MISDN_CTYPE_BOOL, "no", NONE,
		"Enable this if we should produce DTMF Tones ourselves." },
	{ "hold_allowed", MISDN_CFG_HOLD_ALLOWED, MISDN_CTYPE_BOOL, "no", NONE,
		"Enable this to have support for hold and retrieve." },
	{ "early_bconnect", MISDN_CFG_EARLY_BCONNECT, MISDN_CTYPE_BOOL, "yes", NONE,
		"Disable this if you don't mind correct handling of Progress Indicators." },
	{ "incoming_early_audio", MISDN_CFG_INCOMING_EARLY_AUDIO, MISDN_CTYPE_BOOL, "no", NONE,
		"Turn this on if you like to send Tone Indications to a Incoming\n"
		"\tisdn channel on a TE Port. Rarely used, only if the Telco allows\n"
		"\tyou to send indications by yourself, normally the Telco sends the\n"
		"\tindications to the remote party." },
	{ "echocancel", MISDN_CFG_ECHOCANCEL, MISDN_CTYPE_BOOLINT, "0", 128,
		"This enables echocancellation, with the given number of taps.\n"
		"\tBe aware, move this setting only to outgoing portgroups!\n"
		"\tA value of zero turns echocancellation off.\n"
		"\n"
		"\tPossible values are: 0,32,64,128,256,yes(=128),no(=0)" },
	{ "echocancelwhenbridged", MISDN_CFG_ECHOCANCELWHENBRIDGED, MISDN_CTYPE_BOOL, "no", NONE,
		"This disables echocancellation when the call is bridged between\n"
		"\tmISDN channels" },
#ifdef WITH_BEROEC
	{ "bnechocancel", MISDN_CFG_BNECHOCANCEL, MISDN_CTYPE_BOOLINT, "yes", 64 ,
	""},
	{ "bnec_antihowl", MISDN_CFG_BNEC_ANTIHOWL, MISDN_CTYPE_INT, "0", NONE,
	""},
	{ "bnec_nlp", MISDN_CFG_BNEC_NLP, MISDN_CTYPE_BOOL, "yes", NONE ,
	""},
	{ "bnec_zerocoeff", MISDN_CFG_BNEC_ZEROCOEFF, MISDN_CTYPE_BOOL, "no", NONE,
	""},
	{ "bnec_tonedisabler", MISDN_CFG_BNEC_TD, MISDN_CTYPE_BOOL, "no", NONE,
	""},
	{ "bnec_adaption", MISDN_CFG_BNEC_ADAPT, MISDN_CTYPE_INT, "1", NONE,
	""},
#endif

#ifdef WITH_ECTRAIN
	{ "echotraining", MISDN_CFG_ECHOTRAINING, MISDN_CTYPE_BOOLINT, "0", 2000,
		"Set this to no to disable echotraining. You can enter a number > 10.\n"
		"\tThe value is a multiple of 0.125 ms.\n"
		"\n"
		"\tyes = 2000\n"
		"\tno = 0" },
#endif
	{ "need_more_infos", MISDN_CFG_NEED_MORE_INFOS, MISDN_CTYPE_BOOL, "0", NONE,
		"Send Setup_Acknowledge on incoming calls anyway (instead of PROCEEDING),\n"
		"\tthis requests additional Infos, so we can waitfordigits without much\n"
		"\tissues. This works only for PTP Ports" },
	{ "jitterbuffer", MISDN_CFG_JITTERBUFFER, MISDN_CTYPE_INT, "4000", NONE,
		"The jitterbuffer." },
	{ "jitterbuffer_upper_threshold", MISDN_CFG_JITTERBUFFER_UPPER_THRESHOLD, MISDN_CTYPE_INT, "0", NONE,
		"Change this threshold to enable dejitter functionality." },
	{ "callgroup", MISDN_CFG_CALLGROUP, MISDN_CTYPE_ASTGROUP, NO_DEFAULT, NONE,
		"Callgroup." },
	{ "pickupgroup", MISDN_CFG_PICKUPGROUP, MISDN_CTYPE_ASTGROUP, NO_DEFAULT, NONE,
		"Pickupgroup." },
	{ "max_incoming", MISDN_CFG_MAX_IN, MISDN_CTYPE_INT, "-1", NONE,
		"Defines the maximum amount of incoming calls per port for this group.\n"
		"\tCalls which exceed the maximum will be marked with the channel varible\n"
		"\tMAX_OVERFLOW. It will contain the amount of overflowed calls" },
	{ "max_outgoing", MISDN_CFG_MAX_OUT, MISDN_CTYPE_INT, "-1", NONE,
		"Defines the maximum amount of outgoing calls per port for this group\n"
		"\texceeding calls will be rejected" },
	{ "faxdetect", MISDN_CFG_FAXDETECT, MISDN_CTYPE_STR, "no", NONE,
		"Setup fax detection:\n"
		"\t    no        - no fax detection\n"
		"\t    incoming  - fax detection for incoming calls\n"
		"\t    outgoing  - fax detection for outgoing calls\n"
		"\t    both      - fax detection for incoming and outgoing calls\n"
		"\tAdd +nojump to your value (i.e. faxdetect=both+nojump) if you don't want to jump into the\n"
		"\tfax-extension but still want to detect the fax and prepare the channel for fax transfer." },
	{ "faxdetect_timeout", MISDN_CFG_FAXDETECT_TIMEOUT, MISDN_CTYPE_INT, "5", NONE,
		"Number of seconds the fax detection should do its job. After the given period of time,\n"
		"\twe assume that it's not a fax call and save some CPU time by turning off fax detection.\n"
		"\tSet this to 0 if you don't want a timeout (never stop detecting)." },
	{ "faxdetect_context", MISDN_CFG_FAXDETECT_CONTEXT, MISDN_CTYPE_STR, NO_DEFAULT, NONE,
		"Context to jump into if we detect a fax. Don't set this if you want to stay in the current context." },
	{ "l1watcher_timeout", MISDN_CFG_L1_TIMEOUT, MISDN_CTYPE_BOOLINT, "0", 4,
		"Watches the layer 1. If the layer 1 is down, it tries to\n"
		"\tget it up. The timeout is given in seconds. with 0 as value it\n"
		"\tdoes not watch the l1 at all\n"
		"\n"
		"\tThis option is only read at loading time of chan_misdn, which\n"
		"\tmeans you need to unload and load chan_misdn to change the value,\n"
		"\tan CallWeaver restart should do the trick." },
	{ "overlap_dial", MISDN_CFG_OVERLAP_DIAL, MISDN_CTYPE_BOOLINT, "0", 4,
		"Enables overlap dial for the given amount of seconds.\n"
		"\tPossible values are positive integers or:\n"
		"\t   yes (= 4 seconds)\n"
		"\t   no  (= 0 seconds = disabled)" },
	{ "msns", MISDN_CFG_MSNS, MISDN_CTYPE_MSNLIST, NO_DEFAULT, NONE,
		"MSN's for TE ports, listen on those numbers on the above ports, and\n"
		"\tindicate the incoming calls to CallWeaver.\n"
		"\tHere you can give a comma seperated list, or simply an '*' for any msn." },
};

static const struct misdn_cfg_spec gen_spec[] = {
	{ "debug", MISDN_GEN_DEBUG, MISDN_CTYPE_INT, "0", NONE,
		"Sets the debugging flag:\n"
		"\t0 - No Debug\n"
		"\t1 - mISDN Messages and * - Messages, and * - State changes\n"
		"\t2 - Messages + Message specific Informations (e.g. bearer capability)\n"
		"\t3 - very Verbose, the above + lots of Driver specific infos\n"
		"\t4 - even more Verbose than 3" },
	{ "misdn_init", MISDN_GEN_MISDN_INIT, MISDN_CTYPE_STR, "/etc/misdn-init.conf", NONE,
		"Set the path to the misdn-init.conf (for nt_ptp mode checking)." },
	{ "tracefile", MISDN_GEN_TRACEFILE, MISDN_CTYPE_STR, "/var/log/callweaver.org/misdn.log", NONE,
		"Set the path to the massively growing trace file, if you want that." },
	{ "bridging", MISDN_GEN_BRIDGING, MISDN_CTYPE_BOOL, "yes", NONE,
		"Set this to yes if you want mISDN_dsp to bridge the calls in HW." },
	{ "stop_tone_after_first_digit", MISDN_GEN_STOP_TONE, MISDN_CTYPE_BOOL, "yes", NONE,
		"Stops dialtone after getting first digit on NT Port." },
	{ "append_digits2exten", MISDN_GEN_APPEND_DIGITS2EXTEN, MISDN_CTYPE_BOOL, "yes", NONE,
		"Wether to append overlapdialed Digits to Extension or not." },
	{ "dynamic_crypt", MISDN_GEN_DYNAMIC_CRYPT, MISDN_CTYPE_BOOL, "no", NONE,
		"Wether to look out for dynamic crypting attempts." },
	{ "crypt_prefix", MISDN_GEN_CRYPT_PREFIX, MISDN_CTYPE_STR, NO_DEFAULT, NONE,
		"What is used for crypting Protocol." },
	{ "crypt_keys", MISDN_GEN_CRYPT_KEYS, MISDN_CTYPE_STR, NO_DEFAULT, NONE,
		"Keys for cryption, you reference them in the dialplan\n"
		"\tLater also in dynamic encr." },
	{ "ntdebugflags", MISDN_GEN_NTDEBUGFLAGS, MISDN_CTYPE_INT, "0", NONE,
		"No description yet." },
	{ "ntdebugfile", MISDN_GEN_NTDEBUGFILE, MISDN_CTYPE_STR, "/var/log/misdn-nt.log", NONE,
		"No description yet." }
};

/* array of port configs, default is at position 0. */
static union misdn_cfg_pt **port_cfg;
/* max number of available ports, is set on init */
static int max_ports;
/* general config */
static union misdn_cfg_pt *general_cfg;
/* storing the ptp flag separated to save memory */
static int *ptp;
/* maps enum config elements to array positions */
static int *map;

static cw_mutex_t config_mutex; 

#define CLI_ERROR(name, value, section) ({ \
	cw_log(LOG_WARNING, "misdn.conf: \"%s=%s\" (section: %s) invalid or out of range. " \
		"Please edit your misdn.conf and then do a \"misdn reload\".\n", name, value, section); \
})

static int _enum_array_map (void)
{
	int i, j, ok;

	for (i = MISDN_CFG_FIRST + 1; i < MISDN_CFG_LAST; ++i) {
		if (i == MISDN_CFG_PTP)
			continue;
		ok = 0;
		for (j = 0; j < NUM_PORT_ELEMENTS; ++j) {
			if (port_spec[j].elem == i) {
				map[i] = j;
				ok = 1;
				break;
			}
		}
		if (!ok) {
			cw_log(LOG_WARNING, "Enum element %d in misdn_cfg_elements (port section) has no corresponding element in the config struct!\n", i);
			return -1;
		}
	}
	for (i = MISDN_GEN_FIRST + 1; i < MISDN_GEN_LAST; ++i) {
		ok = 0;
		for (j = 0; j < NUM_GEN_ELEMENTS; ++j) {
			if (gen_spec[j].elem == i) {
				map[i] = j;
				ok = 1;
				break;
			}
		}
		if (!ok) {
			cw_log(LOG_WARNING, "Enum element %d in misdn_cfg_elements (general section) has no corresponding element in the config struct!\n", i);
			return -1;
		}
	}
	return 0;
}

static int get_cfg_position (char *name, int type)
{
	int i;

	switch (type) {
	case PORT_CFG:
		for (i = 0; i < NUM_PORT_ELEMENTS; ++i) {
			if (!strcasecmp(name, port_spec[i].name))
				return i;
		}
		break;
	case GEN_CFG:
		for (i = 0; i < NUM_GEN_ELEMENTS; ++i) {
			if (!strcasecmp(name, gen_spec[i].name))
				return i;
		}
	}

	return -1;
}

static inline void misdn_cfg_lock (void)
{
	cw_mutex_lock(&config_mutex);
}

static inline void misdn_cfg_unlock (void)
{
	cw_mutex_unlock(&config_mutex);
}

static void _free_msn_list (struct msn_list* iter)
{
	if (iter->next)
		_free_msn_list(iter->next);
	if (iter->msn)
		free(iter->msn);
	free(iter);
}

static void _free_port_cfg (void)
{
	int i, j;
	int gn = map[MISDN_CFG_GROUPNAME];
	union misdn_cfg_pt* free_list[max_ports + 2];
	
	memset(free_list, 0, sizeof(free_list));
	free_list[0] = port_cfg[0];
	for (i = 1; i <= max_ports; ++i) {
		if (port_cfg[i][gn].str) {
			/* we always have a groupname in the non-default case, so this is fine */
			for (j = 1; j <= max_ports; ++j) {
				if (free_list[j] && free_list[j][gn].str == port_cfg[i][gn].str)
					break;
				else if (!free_list[j]) {
					free_list[j] = port_cfg[i];
					break;
				}
			}
		}
	}
	for (j = 0; free_list[j]; ++j) {
		for (i = 0; i < NUM_PORT_ELEMENTS; ++i) {
			if (free_list[j][i].any) {
				if (port_spec[i].type == MISDN_CTYPE_MSNLIST)
					_free_msn_list(free_list[j][i].ml);
				else
					free(free_list[j][i].any);
			}
		}
	}
}

static void _free_general_cfg (void)
{
	int i;

	for (i = 0; i < NUM_GEN_ELEMENTS; i++) 
		if (general_cfg[i].any)
			free(general_cfg[i].any);
}

void misdn_cfg_get (int port, enum misdn_cfg_elements elem, void *buf, int bufsize)
{
	int place;

	if ((elem < MISDN_CFG_LAST) && !misdn_cfg_is_port_valid(port)) {
		memset(buf, 0, bufsize);
		cw_log(LOG_WARNING, "Invalid call to misdn_cfg_get! Port number %d is not valid.\n", port);
		return;
	}

	misdn_cfg_lock();
	if (elem == MISDN_CFG_PTP) {
		if (!memcpy(buf, &ptp[port], (bufsize > ptp[port]) ? sizeof(ptp[port]) : bufsize))
			memset(buf, 0, bufsize);
	} else {
		if ((place = map[elem]) < 0) {
			memset (buf, 0, bufsize);
			cw_log(LOG_WARNING, "Invalid call to misdn_cfg_get! Invalid element (%d) requested.\n", elem);
		} else {
			if (elem < MISDN_CFG_LAST) {
				switch (port_spec[place].type) {
				case MISDN_CTYPE_STR:
					if (port_cfg[port][place].str) {
						if (!memccpy(buf, port_cfg[port][place].str, 0, bufsize))
							memset(buf, 0, 1);
					} else if (port_cfg[0][place].str) {
						if (!memccpy(buf, port_cfg[0][place].str, 0, bufsize))
							memset(buf, 0, 1);
					}
					break;
				default:
					if (port_cfg[port][place].any)
						memcpy(buf, port_cfg[port][place].any, bufsize);
					else if (port_cfg[0][place].any)
						memcpy(buf, port_cfg[0][place].any, bufsize);
					else
						memset(buf, 0, bufsize);
				}
			} else {
				switch (gen_spec[place].type) {
				case MISDN_CTYPE_STR:
					if (!general_cfg[place].str || !memccpy(buf, general_cfg[place].str, 0, bufsize))
						memset(buf, 0, 1);
					break;
				default:
					if (general_cfg[place].any)
						memcpy(buf, general_cfg[place].any, bufsize);
					else
						memset(buf, 0, bufsize);
				}
			}
		}
	}
	misdn_cfg_unlock();
}

enum misdn_cfg_elements misdn_cfg_get_elem (char *name)
{
	int pos;

	/* here comes a hack to replace the (not existing) "name" elemet with the "ports" element */
	if (!strcmp(name, "ports"))
		return MISDN_CFG_GROUPNAME;
	if (!strcmp(name, "name"))
		return MISDN_CFG_FIRST;

	pos = get_cfg_position (name, PORT_CFG);
	if (pos >= 0)
		return port_spec[pos].elem;
	
	pos = get_cfg_position (name, GEN_CFG);
	if (pos >= 0)
		return gen_spec[pos].elem;
	
	return MISDN_CFG_FIRST;
}

void misdn_cfg_get_name (enum misdn_cfg_elements elem, void *buf, int bufsize)
{
	struct misdn_cfg_spec *spec = NULL;
	int place = map[elem];

	/* the ptp hack */
	if (elem == MISDN_CFG_PTP) {
		memset(buf, 0, 1);
		return;
	}
	
	/* here comes a hack to replace the (not existing) "name" elemet with the "ports" element */
	if (elem == MISDN_CFG_GROUPNAME) {
		if (!snprintf(buf, bufsize, "ports"))
			memset(buf, 0, 1);
		return;
	}
	
	if ((elem > MISDN_CFG_FIRST) && (elem < MISDN_CFG_LAST))
		spec = (struct misdn_cfg_spec *)port_spec;
	else if ((elem > MISDN_GEN_FIRST) && (elem < MISDN_GEN_LAST))
		spec = (struct misdn_cfg_spec *)gen_spec;

	if (!spec || !memccpy(buf, spec[place].name, 0, bufsize))
		memset(buf, 0, 1);
}

void misdn_cfg_get_desc (enum misdn_cfg_elements elem, void *buf, int bufsize, void *buf_default, int bufsize_default)
{
	int place = map[elem];
	struct misdn_cfg_spec *spec = NULL;

	/* here comes a hack to replace the (not existing) "name" elemet with the "ports" element */
	if (elem == MISDN_CFG_GROUPNAME) {
		if (!memccpy(buf, ports_description, 0, bufsize))
			memset(buf, 0, 1);
		if (buf_default && bufsize_default)
			memset(buf_default, 0, 1);
		return;
	}

	if ((elem > MISDN_CFG_FIRST) && (elem < MISDN_CFG_LAST))
		spec = (struct misdn_cfg_spec *)port_spec;
	else if ((elem > MISDN_GEN_FIRST) && (elem < MISDN_GEN_LAST))
		spec = (struct misdn_cfg_spec *)gen_spec;
		
	if (!spec || !spec[place].desc)
		memset(buf, 0, 1);
	else {
		if (!memccpy(buf, spec[place].desc, 0, bufsize))
			memset(buf, 0, 1);
		if (buf_default && bufsize) {
			if (!strcmp(spec[place].def, NO_DEFAULT))
				memset(buf_default, 0, 1);
			else if (!memccpy(buf_default, spec[place].def, 0, bufsize_default))
				memset(buf_default, 0, 1);
		}
	}
}

int misdn_cfg_is_msn_valid (int port, char* msn)
{
	int re = 0;
	struct msn_list *iter;

	if (!misdn_cfg_is_port_valid(port))
    {
		cw_log(LOG_WARNING, "Invalid call to misdn_cfg_is_msn_valid! Port number %d is not valid.\n", port);
		return 0;
	}

	misdn_cfg_lock();
	if (port_cfg[port][map[MISDN_CFG_MSNS]].ml)
		iter = port_cfg[port][map[MISDN_CFG_MSNS]].ml;
	else
		iter = port_cfg[0][map[MISDN_CFG_MSNS]].ml;
	for (  ;  iter;  iter = iter->next)
    {
		if (*(iter->msn) == '*')
        {
			re = 1;
		}
        else
        {
            switch (cw_extension_pattern_match(msn, iter->msn))
            {
            case EXTENSION_MATCH_EXACT:
            case EXTENSION_MATCH_STRETCHABLE:
            case EXTENSION_MATCH_POSSIBLE:
    			re = 1;
    			break;
            }
        }
        if (re)
            break;
	}
    misdn_cfg_unlock();

	return re;
}

int misdn_cfg_is_port_valid (int port)
{
	int gn = map[MISDN_CFG_GROUPNAME];

	return (port >= 1 && port <= max_ports && port_cfg[port][gn].str);
}

int misdn_cfg_is_group_method (char *group, enum misdn_cfg_method meth)
{
	int i, re = 0;
	char *method = NULL;

	misdn_cfg_lock();
	for (i = 1; i <= max_ports; i++) {
		if (port_cfg[i] && port_cfg[i][map[MISDN_CFG_GROUPNAME]].str) {
			if (!strcasecmp(port_cfg[i][map[MISDN_CFG_GROUPNAME]].str, group))
				method = (port_cfg[i][map[MISDN_CFG_METHOD]].str ? 
						  port_cfg[i][map[MISDN_CFG_METHOD]].str : port_cfg[0][map[MISDN_CFG_METHOD]].str);
		}
	}
	if (method) {
		switch (meth) {
		case METHOD_STANDARD:		re = !strcasecmp(method, "standard");
									break;
		case METHOD_ROUND_ROBIN:	re = !strcasecmp(method, "round_robin");
									break;
		}
	}
	misdn_cfg_unlock();

	return re;
}

void misdn_cfg_get_ports_string (char *ports)
{
	char tmp[16];
	int l, i;
	int gn = map[MISDN_CFG_GROUPNAME];

	*ports = 0;

	misdn_cfg_lock();
	for (i = 1; i <= max_ports; i++) {
		if (port_cfg[i][gn].str) {
			if (ptp[i])
				sprintf(tmp, "%dptp,", i);
			else
				sprintf(tmp, "%d,", i);
			strcat(ports, tmp);
		}
	}
	misdn_cfg_unlock();

	if ((l = strlen(ports)))
		ports[l-1] = 0;
}

void misdn_cfg_get_config_string (int port, enum misdn_cfg_elements elem, char* buf, int bufsize)
{
	int place;
	char tempbuf[BUFFERSIZE] = "";
	struct msn_list *iter;

	if ((elem < MISDN_CFG_LAST) && !misdn_cfg_is_port_valid(port)) {
		*buf = 0;
		cw_log(LOG_WARNING, "Invalid call to misdn_cfg_get_config_string! Port number %d is not valid.\n", port);
		return;
	}

	place = map[elem];

	misdn_cfg_lock();
	if (elem == MISDN_CFG_PTP) {
		snprintf(buf, bufsize, " -> ptp: %s", ptp[port] ? "yes" : "no");
	}
	else if (elem > MISDN_CFG_FIRST && elem < MISDN_CFG_LAST) {
		switch (port_spec[place].type) {
		case MISDN_CTYPE_INT:
		case MISDN_CTYPE_BOOLINT:
			if (port_cfg[port][place].num)
				snprintf(buf, bufsize, " -> %s: %d", port_spec[place].name, *port_cfg[port][place].num);
			else if (port_cfg[0][place].num)
				snprintf(buf, bufsize, " -> %s: %d", port_spec[place].name, *port_cfg[0][place].num);
			else
				snprintf(buf, bufsize, " -> %s:", port_spec[place].name);
			break;
		case MISDN_CTYPE_BOOL:
			if (port_cfg[port][place].num)
				snprintf(buf, bufsize, " -> %s: %s", port_spec[place].name, *port_cfg[port][place].num ? "yes" : "no");
			else if (port_cfg[0][place].num)
				snprintf(buf, bufsize, " -> %s: %s", port_spec[place].name, *port_cfg[0][place].num ? "yes" : "no");
			else
				snprintf(buf, bufsize, " -> %s:", port_spec[place].name);
			break;
		case MISDN_CTYPE_ASTGROUP:
			if (port_cfg[port][place].grp)
				snprintf(buf, bufsize, " -> %s: %s", port_spec[place].name, 
						 cw_print_group(tempbuf, sizeof(tempbuf), *port_cfg[port][place].grp));
			else if (port_cfg[0][place].grp)
				snprintf(buf, bufsize, " -> %s: %s", port_spec[place].name, 
						 cw_print_group(tempbuf, sizeof(tempbuf), *port_cfg[0][place].grp));
			else
				snprintf(buf, bufsize, " -> %s:", port_spec[place].name);
			break;
		case MISDN_CTYPE_MSNLIST:
			if (port_cfg[port][place].ml)
				iter = port_cfg[port][place].ml;
			else
				iter = port_cfg[0][place].ml;
			if (iter) {
				for (; iter; iter = iter->next)
					sprintf(tempbuf, "%s%s, ", tempbuf, iter->msn);
				tempbuf[strlen(tempbuf)-2] = 0;
			}
			snprintf(buf, bufsize, " -> msns: %s", *tempbuf ? tempbuf : "none");
			break;
		case MISDN_CTYPE_STR:
			if ( port_cfg[port][place].str) {
				snprintf(buf, bufsize, " -> %s: %s", port_spec[place].name, port_cfg[port][place].str);
			} else if (port_cfg[0][place].str) {
				snprintf(buf, bufsize, " -> %s: %s", port_spec[place].name, port_cfg[0][place].str);
			} else {
				snprintf(buf, bufsize, " -> %s:", port_spec[place].name);
			}
			break;
		}
	} else if (elem > MISDN_GEN_FIRST && elem < MISDN_GEN_LAST) {
		switch (gen_spec[place].type) {
		case MISDN_CTYPE_INT:
		case MISDN_CTYPE_BOOLINT:
			if (general_cfg[place].num)
				snprintf(buf, bufsize, " -> %s: %d", gen_spec[place].name, *general_cfg[place].num);
			else
				snprintf(buf, bufsize, " -> %s:", gen_spec[place].name);
			break;
		case MISDN_CTYPE_BOOL:
			if (general_cfg[place].num)
				snprintf(buf, bufsize, " -> %s: %s", gen_spec[place].name, *general_cfg[place].num ? "yes" : "no");
			else
				snprintf(buf, bufsize, " -> %s:", gen_spec[place].name);
			break;
		case MISDN_CTYPE_STR:
			if ( general_cfg[place].str) {
				snprintf(buf, bufsize, " -> %s: %s", gen_spec[place].name, general_cfg[place].str);
			} else {
				snprintf(buf, bufsize, " -> %s:", gen_spec[place].name);
			}
			break;
		default:
			snprintf(buf, bufsize, " -> type of %s not handled yet", gen_spec[place].name);
			break;
		}
	} else {
		*buf = 0;
		cw_log(LOG_WARNING, "Invalid call to misdn_cfg_get_config_string! Invalid config element (%d) requested.\n", elem);
	}
	misdn_cfg_unlock();
}

int misdn_cfg_get_next_port (int port)
{
	int p = -1;
	int gn = map[MISDN_CFG_GROUPNAME];
	
	misdn_cfg_lock();
	for (port++; port <= max_ports; port++) {
		if (port_cfg[port][gn].str) {
			p = port;
			break;
		}
	}
	misdn_cfg_unlock();

	return p;
}

int misdn_cfg_get_next_port_spin (int port)
{
	int p = misdn_cfg_get_next_port(port);
	return (p > 0) ? p : misdn_cfg_get_next_port(0);
}

static int _parse (union misdn_cfg_pt *dest, char *value, enum misdn_cfg_type type, int boolint_def)
{
	int re = 0;
	int len, tmp;
	char *valtmp;

	switch (type) {
	case MISDN_CTYPE_STR:
		if ((len = strlen(value))) {
			dest->str = (char *)malloc((len + 1) * sizeof(char));
			strncpy(dest->str, value, len);
			dest->str[len] = 0;
		} else {
			dest->str = (char *)malloc( sizeof(char));
			dest->str[0] = 0;
		}
		break;
	case MISDN_CTYPE_INT:
	{
		char *pat;
		if (strchr(value,'x')) 
			pat="%x";
		else
			pat="%d";
		if (sscanf(value, pat, &tmp)) {
			dest->num = (int *)malloc(sizeof(int));
			memcpy(dest->num, &tmp, sizeof(int));
		} else
			re = -1;
	}
		break;
	case MISDN_CTYPE_BOOL:
		dest->num = (int *)malloc(sizeof(int));
		*(dest->num) = (cw_true(value) ? 1 : 0);
		break;
	case MISDN_CTYPE_BOOLINT:
		dest->num = (int *)malloc(sizeof(int));
		if (sscanf(value, "%d", &tmp)) {
			memcpy(dest->num, &tmp, sizeof(int));
		} else {
			*(dest->num) = (cw_true(value) ? boolint_def : 0);
		}
		break;
	case MISDN_CTYPE_MSNLIST:
		for (valtmp = strsep(&value, ","); valtmp; valtmp = strsep(&value, ",")) {
			if ((len = strlen(valtmp))) {
				struct msn_list *ml = (struct msn_list *)malloc(sizeof(struct msn_list));
				ml->msn = (char *)calloc(len+1, sizeof(char));
				strncpy(ml->msn, valtmp, len);
				ml->next = dest->ml;
				dest->ml = ml;
			}
		}
		break;
	case MISDN_CTYPE_ASTGROUP:
		dest->grp = (cw_group_t *)malloc(sizeof(cw_group_t));
		*(dest->grp) = cw_get_group(value);
		break;
	}

	return re;
}

static void _build_general_config (struct cw_variable *v)
{
	int pos;

	for (; v; v = v->next) {
		if (((pos = get_cfg_position(v->name, GEN_CFG)) < 0) || 
			(_parse(&general_cfg[pos], v->value, gen_spec[pos].type, gen_spec[pos].boolint_def) < 0))
			CLI_ERROR(v->name, v->value, "general");
	}
}

static void _build_port_config (struct cw_variable *v, char *cat)
{
	int pos, i;
	union misdn_cfg_pt cfg_tmp[NUM_PORT_ELEMENTS];
	int cfg_for_ports[max_ports + 1];

	if (!v || !cat)
		return;

	memset(cfg_tmp, 0, sizeof(cfg_tmp));
	memset(cfg_for_ports, 0, sizeof(cfg_for_ports));

	if (!strcasecmp(cat, "default")) {
		cfg_for_ports[0] = 1;
	}

	if (((pos = get_cfg_position("name", PORT_CFG)) < 0) || 
		(_parse(&cfg_tmp[pos], cat, port_spec[pos].type, port_spec[pos].boolint_def) < 0)) {
		CLI_ERROR(v->name, v->value, cat);
		return;
	}

	for (; v; v = v->next) {
		if (!strcasecmp(v->name, "ports")) {
			char *token;
			char ptpbuf[BUFFERSIZE] = "";
			int start, end;
			for (token = strsep(&v->value, ","); token; token = strsep(&v->value, ","), *ptpbuf = 0) { 
				if (!*token)
					continue;
				if (sscanf(token, "%d-%d%s", &start, &end, ptpbuf) >= 2) {
					for (; start <= end; start++) {
						if (start <= max_ports && start > 0) {
							cfg_for_ports[start] = 1;
							ptp[start] = (strstr(ptpbuf, "ptp")) ? 1 : 0;
						} else
							CLI_ERROR(v->name, v->value, cat);
					}
				} else {
					if (sscanf(token, "%d%s", &start, ptpbuf)) {
						if (start <= max_ports && start > 0) {
							cfg_for_ports[start] = 1;
							ptp[start] = (strstr(ptpbuf, "ptp")) ? 1 : 0;
						} else
							CLI_ERROR(v->name, v->value, cat);
					} else
						CLI_ERROR(v->name, v->value, cat);
				}
			}
		} else {
			if (((pos = get_cfg_position(v->name, PORT_CFG)) < 0) || 
				(_parse(&cfg_tmp[pos], v->value, port_spec[pos].type, port_spec[pos].boolint_def) < 0))
				CLI_ERROR(v->name, v->value, cat);
		}
	}

	for (i = 0; i < (max_ports + 1); ++i) {
		if (cfg_for_ports[i]) {
			memcpy(port_cfg[i], cfg_tmp, sizeof(cfg_tmp));
		}
	}
}

void misdn_cfg_update_ptp (void)
{
	char misdn_init[BUFFERSIZE];
	char line[BUFFERSIZE];
	FILE *fp;
	char *tok, *p, *end;
	int port;

	misdn_cfg_get(0, MISDN_GEN_MISDN_INIT, &misdn_init, sizeof(misdn_init));

	if (misdn_init) {
		fp = fopen(misdn_init, "r");
		if (fp) {
			while(fgets(line, sizeof(line), fp)) {
				if (!strncmp(line, "nt_ptp", 6)) {
					for (tok = strtok_r(line,",=", &p);
						 tok;
						 tok = strtok_r(NULL,",=", &p)) {
						port = strtol(tok, &end, 10);
						if (end != tok && misdn_cfg_is_port_valid(port)) {
							misdn_cfg_lock();
							ptp[port] = 1;
							misdn_cfg_unlock();
						}
					}
				}
			}
			fclose(fp);
		} else {
			cw_log(LOG_WARNING,"Couldn't open %s: %s\n", misdn_init, strerror(errno));
		}
	}
}

static void _fill_defaults (void)
{
	int i;

	for (i = 0; i < NUM_PORT_ELEMENTS; ++i) {
		if (!port_cfg[0][i].any && strcasecmp(port_spec[i].def, NO_DEFAULT))
			_parse(&(port_cfg[0][i]), (char *)port_spec[i].def, port_spec[i].type, port_spec[i].boolint_def);
	}
	for (i = 0; i < NUM_GEN_ELEMENTS; ++i) {
		if (!general_cfg[i].any && strcasecmp(gen_spec[i].def, NO_DEFAULT))
			_parse(&(general_cfg[i]), (char *)gen_spec[i].def, gen_spec[i].type, gen_spec[i].boolint_def);
	}
}

void misdn_cfg_reload (void)
{
	misdn_cfg_init (0);
}

void misdn_cfg_destroy (void)
{
	misdn_cfg_lock();

	_free_port_cfg();
	_free_general_cfg();

	free(port_cfg);
	free(general_cfg);
	free(ptp);
	free(map);

	misdn_cfg_unlock();
	cw_mutex_destroy(&config_mutex);
}

int misdn_cfg_init (int this_max_ports)
{
	char config[] = "misdn.conf";
	char *cat, *p;
	int i;
	struct cw_config *cfg;
	struct cw_variable *v;

	if (!(cfg = CW_LOAD_CFG(config))) {
		cw_log(LOG_WARNING, "missing file: misdn.conf\n");
		return -1;
	}

	cw_mutex_init(&config_mutex);

	misdn_cfg_lock();

	if (this_max_ports) {
		/* this is the first run */
		max_ports = this_max_ports;
		map = (int *)calloc(MISDN_GEN_LAST + 1, sizeof(int));
		if (_enum_array_map())
			return -1;
		p = (char *)calloc(1, (max_ports + 1) * sizeof(union misdn_cfg_pt *)
						   + (max_ports + 1) * NUM_PORT_ELEMENTS * sizeof(union misdn_cfg_pt));
		port_cfg = (union misdn_cfg_pt **)p;
		p += (max_ports + 1) * sizeof(union misdn_cfg_pt *);
		for (i = 0; i <= max_ports; ++i) {
			port_cfg[i] = (union misdn_cfg_pt *)p;
			p += NUM_PORT_ELEMENTS * sizeof(union misdn_cfg_pt);
		}
		general_cfg = (union misdn_cfg_pt *)calloc(1, sizeof(union misdn_cfg_pt *) * NUM_GEN_ELEMENTS);
		ptp = (int *)calloc(max_ports + 1, sizeof(int));
	}
	else {
		/* misdn reload */
		_free_port_cfg();
		_free_general_cfg();
		memset(port_cfg[0], 0, NUM_PORT_ELEMENTS * sizeof(union misdn_cfg_pt) * (max_ports + 1));
		memset(general_cfg, 0, sizeof(union misdn_cfg_pt *) * NUM_GEN_ELEMENTS);
		memset(ptp, 0, sizeof(int) * (max_ports + 1));
	}

	cat = cw_category_browse(cfg, NULL);

	while(cat) {
		v = cw_variable_browse(cfg, cat);
		if (!strcasecmp(cat, "general")) {
			_build_general_config(v);
		} else {
			_build_port_config(v, cat);
		}
		cat = cw_category_browse(cfg, cat);
	}

	_fill_defaults();

	misdn_cfg_unlock();
	CW_DESTROY_CFG(cfg);

	return 0;
}


