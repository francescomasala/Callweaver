/* Temporary things, until everyone is using the latest spandsp */
#if !defined(CLIP_DTMF_C_TERMINATED)
    #define CLIP_DTMF_C_TERMINATED 'C'
#endif
#if !defined(CLIP_DTMF_HASH_TERMINATED)
    #define CLIP_DTMF_HASH_TERMINATED '#'
#endif
#if !defined(CLIP_DTMF_C_CALLER_NUMBER)
    #define CLIP_DTMF_C_CALLER_NUMBER CLIP_DTMF_CALLER_NUMBER
    #define adsi_tx_set_preamble(a,b,c,d,e) /**/
#endif

/*
 * CallWeaver -- An open source telephony toolkit.
 *
 * Copyright (C) 2007, Eris Associates Limited, UK
 *
 * Mike Jagdis <mjagdis@eris-associates.co.uk>
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
 */

/*
 *
 * CallerID Generation support 
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <math.h>
#include <ctype.h>
#include <spandsp.h>

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/corelib/callerid.c $", "$Revision: 4723 $")

#include "callweaver/ulaw.h"
#include "callweaver/alaw.h"
#include "callweaver/frame.h"
#include "callweaver/channel.h"
#include "callweaver/options.h"
#include "callweaver/phone_no_utils.h"
#include "callweaver/logger.h"
#include "callweaver/utils.h"
#include "callweaver/callerid.h"

struct tdd_state {
    adsi_rx_state_t rx;
    char rx_msg[256];
};


static inline int lin2xlaw(int codec, int16_t *lin, int slen, uint8_t *xlaw, int xmax)
{
	int i;

	if (slen > xmax)
		slen = xmax;

	if (codec == CW_FORMAT_ULAW) {
		for (i = 0; i < slen; i++)
			xlaw[i] = CW_LIN2MU(lin[i]);
	} else {
		for (i = 0; i < slen; i++)
			xlaw[i] = CW_LIN2A(lin[i]);
	}

	return slen;
}


int cw_gen_ecdisa(uint8_t *outbuf, int outlen, int codec)
{
	int16_t lin[MAX_CALLERID_SIZE];
	tone_gen_descriptor_t tone_desc;
	tone_gen_state_t tone_state;
	int slen;

	/* A 2100Hz monotone (commonly known as a fax tone) may be used to cause
	 * most switches (with the possible exceptio of some ancient pre-fax
	 * switches maybe?) to disable echo cancelling and provide a clean channel.
	 */
	make_tone_gen_descriptor(&tone_desc, 2100, -13, 0, 0, outlen/8, 0, 0, 0, FALSE);
	tone_gen_init(&tone_state, &tone_desc);
	slen = tone_gen(&tone_state, lin, arraysize(lin));
	return lin2xlaw(codec, lin, slen, outbuf, outlen);
}

int cw_gen_cas(uint8_t *outbuf, int outlen, int sendsas, int codec)
{
	int16_t lin[MAX_CALLERID_SIZE];
	tone_gen_descriptor_t tone_desc;
	tone_gen_state_t tone_state;
	int slen;

	slen = 0;

	if (sendsas) {
		/* SAS - Subscriber Alerting Signal
		 * BT SIN227 makes no mention of SAS and says the speech path is disabled
		 * _after_ CAS. i.e. under the BT system the CPE alert tone also serves
		 * as a subscriber alert tone. It won't be a problem to add SAS in front
		 * of it but people using HW that strictly complies with SIN227 may
		 * experience a "double squawk" that they're not familiar with.
		 * Note: we only handle single burst SAS. If you need to add double and
		 * triple burst they use bursts of 440Hz tone at -16dBm0 for 100ms with
		 * 100ms silence in between each burst.
		 */
		make_tone_gen_descriptor(&tone_desc, 440, -16, 0, 0, 300, 0, 0, 0, FALSE);
		tone_gen_init(&tone_state, &tone_desc);
		slen += tone_gen(&tone_state, lin + slen, arraysize(lin) - slen);
	}

	/* CAS - CPE Alerting Signal
	 * BT SIN227 says the idle state tone alert is 88-110ms at -2dBV to -40dBV followed
	 * by >=45ms silence. The loop state tone alert (which is what we're doing here) is
	 * 80-85ms at -2dBV to -40dBV followed by <= 100ms silence before the ACK is looked
	 * for a a total of <= 275ms before the message should be in flight.
	 * ADSI/US implementation notes on the net say tone for 82ms at -15dBm0 followed by 160ms silence.
	 * Spandsp's adsi.c uses 110ms tone at -13dBm0 followed by 60ms silence. It seems
	 * spandsp's adsi.c implements BT's idle state tone alert. Be warned if you want to
	 * try and remove cw_gen_case - the idle state alert is _almost_ like the loop
	 * state alert but not quite!
	 */
	make_tone_gen_descriptor(&tone_desc, 2130, -13, 2750, -13, 85, 0, 0, 0, FALSE);
	tone_gen_init(&tone_state, &tone_desc);
	slen += tone_gen(&tone_state, lin + slen, arraysize(lin) - slen);

	return lin2xlaw(codec, lin, slen, outbuf, outlen);
}

int mate_generate(uint8_t *outbuf, int outlen, const char *msg, int codec)
{
	int16_t lin[MAX_CALLERID_SIZE];
	int slen;

	adsi_tx_state_t adsi;
	adsi_tx_init(&adsi, ADSI_STANDARD_CLASS);

	/* FIXME: mate formatting should really be implemented in spandsp
	 * rather than mucking around under the hood here
	 */
	for (adsi.msg_len = 0; msg[adsi.msg_len]; adsi.msg_len++)
		adsi.msg[adsi.msg_len] = msg[adsi.msg_len];
    adsi_tx_set_preamble(&adsi, 0, 80, -1, -1);

	slen = adsi_tx(&adsi, lin, sizeof(lin)/sizeof(lin[0]));
	return lin2xlaw(codec, lin, slen, outbuf, outlen);
}

int vmwi_generate(uint8_t *outbuf, int outlen, int active, int mdmf, int codec)
{
	const int init_silence = 2000;
	int16_t lin[MAX_CALLERID_SIZE];
	uint8_t msg[256];
	adsi_tx_state_t adsi;
	int len, slen;

	/* We always use Bell202 - V23 receivers should be able to handle it as if it were V23 */
	adsi_tx_init(&adsi, ADSI_STANDARD_CLASS);

	if (mdmf) {
		len = adsi_add_field(&adsi, msg, -1, CLASS_MDMF_MSG_WAITING, NULL, 0);
		len = adsi_add_field(&adsi, msg, len, MCLASS_VISUAL_INDICATOR, (active)  ?  (const uint8_t *) "\377"  :  (const uint8_t *) "\000", 1);
	} else {
		len = adsi_add_field(&adsi, msg, -1, CLASS_SDMF_MSG_WAITING, NULL, 0);
		len = adsi_add_field(&adsi, msg, len, 0, (active)  ?  (const uint8_t *) "\102\102\102"  :  (const uint8_t *) "\157\157\157", 3);
	}

	adsi_tx_put_message(&adsi, msg, len);

	slen = init_silence + adsi_tx(&adsi, lin+init_silence, sizeof(lin)/sizeof(lin[0]) - init_silence);
	return lin2xlaw(codec, lin, slen, outbuf, outlen);
}


int callerid_get(adsi_rx_state_t *adsi, struct cw_channel *chan, const uint8_t *msg, int len)
{
	uint8_t field_type;
	uint8_t *field_body;
	int field_len;
	int l;
	int message_type = ADSI_STANDARD_NONE;
	uint8_t *name, *number, *dialled;
	int name_len, number_len, dialled_len;

	name = number = dialled = NULL;
	name_len = number_len = dialled_len = -1;

	if (adsi->standard == ADSI_STANDARD_CLIP_DTMF) {
		if (option_debug)
			cw_log(LOG_DEBUG, "%s: CID-IN: DTMF: ALL \"%.*s\"\n", chan->name, len, msg);

		/* Spandsp only handles the Dutch/Danish system.
		 * For Finland/Denmark/Iceland/Netherlands/India/Belgium/Sweden/Brazil/Saudi Arabia/Uruguay/?
		 * we need to parse "AnnnnBnnnDnnnnC" where: A is start of calling
		 * party number, B is start of absence info, C is the end code for
		 * info transfer and D is the start code for the diverting party
		 * number in cas of call diversion. The absence codes are decimal
		 * "00" for caller number unavailable and "10" for caller number
		 * restricted.
		 * Taiwan also has DTMF (as well as V23). For Taiwan the
		 * format is "DnnnnnC"
		 */

		while (len) {
			field_type = msg[0];
			field_body = (uint8_t *)(++msg);
			len--;
			field_len = 0;
			while (len && *msg >= '0' && *msg <= '9')
				len--, msg++, field_len++;

			/* If CID detection was triggered by a device driver level
			 * DTMF event we've lost the first field code. If the field
			 * length is 2 or less we'll assume an absence code otherwise
			 * we'll take it as the caller number. We should be right
			 * most of the time.
			 */
			if (field_type >= '0' && field_type <= '9') {
				msg--;
				field_len++;
				switch (field_len) {
					case 1: field_type = (uint8_t) 'D'; break;
					case 2: field_type = (uint8_t) 'B'; break;
					default: field_type = (uint8_t) 'A'; break;
				}
			}

			if (option_debug)
				cw_log(LOG_DEBUG, "%s: CID-IN: DTMF: '%c' \"%.*s\"\n", chan->name, field_type, field_len, field_body);
			switch (field_type) {
				case 'D':
					if (field_len == 1) {
						/* CLIP-DTMF absence code */
						switch (field_body[0]) {
							case '1': name = (uint8_t *) "Withheld"; break;
							case '2': name = (uint8_t *) "International"; break;
							case '3': name = (uint8_t *) "Unknown"; break;
						}
					} else if (number) {
						/* Finland/Denmark/etc. diverting number */
						break;
					}
					/* Taiwanese caller number - fall through */
				case 'A':
					number = field_body;
					number_len = field_len;
					break;
				case 'B':
					if (field_len == 2 && field_body[1] == '0') {
						switch (field_body[0]) {
							case '0': name = (uint8_t *) "Unknown"; break;
							case '1': name = (uint8_t *) "Withheld"; break;
						}
					}
					break;
			}
		}
	} else {
		l = adsi_next_field(adsi, msg, len, -1, &field_type, (uint8_t const **)&field_body, &field_len);
		while (l > 0) {
			if (!field_body) {
				if (option_debug)
					cw_log(LOG_DEBUG, "%s: CID-IN: %s: Message Type: 0x%02x\n", chan->name, adsi_standard_to_str(adsi->standard), field_type);
				message_type = field_type;
			} else {
				if (option_debug)
					cw_log(LOG_DEBUG, "%s: CID-IN: %s: Field: 0x%02x \"%.*s\"\n", chan->name, adsi_standard_to_str(adsi->standard), field_type, field_len, field_body);
				/* CLASS, CLIP, and ACLIP use identical message codes.
				 * JCLIP is different but MDMF CALLERID and ABSENCE match the rest
				 * and there is nothing that conflicts with the standard DIALLED_NUMBER.
				 * (Nor is there anything in standard that conflicts with the JCLIP
				 * DIALLED NUMBER but we don't need that)
				 */
				switch (message_type) {
					case CLASS_SDMF_CALLERID:
						if (field_len >= 8) {
							number = field_body + 8;
							number_len = field_len - 8;
						}
						break;

					case CLASS_MDMF_CALLERID:
						switch (field_type) {
							case MCLASS_CALLER_NUMBER:
								number = field_body;
								number_len = field_len;
								break;

							case MCLASS_ABSENCE1:
								number = (uint8_t *) "";
								if (name)
									break;
								/* Fall through */
							case MCLASS_ABSENCE2:
								if (field_len == 1) {
									if (field_body[0] == 'P') {
										name = (uint8_t *) "Withheld";
										break;
									} else if (field_body[0] == 'O') {
										name = (uint8_t *) "Unknown";
										break;
									} else if (field_body[0] == 'I') { /* Taiwan */
										name = (uint8_t *) "International";
										break;
									} else if (field_body[0] == 'C') { /* Taiwan */
										name = (uint8_t *) "Coin/Public";
										break;
									}
								}
								cw_log(LOG_DEBUG, "%s: CID-IN: unknown absence code \"%.*s\"\n", chan->name, field_len, field_body);
								name = (uint8_t *) "Unknown";
								break;
							case MCLASS_CALLER_NAME:
								name = field_body;
								name_len = field_len;
								break;

							case MCLASS_DIALED_NUMBER:
								dialled = field_body;
								dialled_len = field_len;
								break;
						}
						break;
				}
			}
			l = adsi_next_field(adsi, msg, len, l, &field_type, (uint8_t const **) &field_body, &field_len);
		}
	}

	/* If we don't have a caller number but do have a dialled number we may
	 * be connected to a Canadian switch using Stentor signalling. This replaces
	 * "dialled number" with "dialable directory number" and uses it in place
	 * of the usual caller number field.
	 */
	if (!number && dialled) {
		number = dialled;
		number_len = dialled_len;
		dialled = NULL;
		dialled_len = 0;
	}

	if (number || name) {
		if (number_len >= 0) {
			number[number_len] = '\0';
			/* BT SIN227 says we aren't supposed to do this. We do anyway
			 * because propagating strange characters into the pbx dialplan
			 * may have unwanted side-effects and may break existing user
			 * configurations.
			 */
			cw_shrink_phone_number((char *) number);
		}

		if (name_len >= 0)
			name[name_len] = '\0';

		/* The last argument should be ANI. If we have ANI (unlikely but technically
		 * possible) we should use it, no?
		 */
		cw_log(LOG_DEBUG, "%s: CID-IN: number=\"%s\", name=\"%s\"\n", chan->name, number, name);
		cw_set_callerid(chan, (char *) number, (char *) name, (char *) number);

		return 0;
	}

	return -1;
}

int cw_callerid_generate(int sig, uint8_t *outbuf, int outlen, int pres, char *number, char *name, int callwaiting, int codec)
{
	const int init_silence = 2000;
	int16_t lin[MAX_CALLERID_SIZE];
	uint8_t msg[256];
	adsi_tx_state_t adsi;
	struct tm tm;
	char datetime[9];
	time_t t;
	int len = 0;
	int i, slen;

	adsi_tx_init(&adsi, sig);

	switch (sig) {
		case ADSI_STANDARD_CLASS:
		case ADSI_STANDARD_CLIP:
		case ADSI_STANDARD_ACLIP:
			/* These are identical down to the {MCLASS,CLIP}_* constants except that
			 * CLIP includes a call type field (which isn't strictly necessary to
			 * include as it defaults to voice if not present)
			 */
			len = adsi_add_field(&adsi, msg, -1, CLASS_MDMF_CALLERID, NULL, 0);

			if (sig == ADSI_STANDARD_CLIP)
				len = adsi_add_field(&adsi, msg, len, CLIP_CALLTYPE, (uint8_t *)"\0x81", 1);

			time(&t);
			localtime_r(&t, &tm);
			sprintf(datetime, "%02d%02d%02d%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
			len = adsi_add_field(&adsi, msg, len, MCLASS_DATETIME, (uint8_t *)datetime, 8);

			if ((pres & CW_PRES_RESTRICTION) == CW_PRES_ALLOWED && number && *number) {
				i = strlen(number);
				len = adsi_add_field(&adsi, msg, len, MCLASS_CALLER_NUMBER, (uint8_t *)number, (i > 16 ? 16 : i));
			} else {
				len = adsi_add_field(&adsi, msg, len, MCLASS_ABSENCE1,
					((pres & CW_PRES_RESTRICTION) == CW_PRES_RESTRICTED
					 	? (uint8_t *)"P" : (uint8_t *)"O"),
					1);
			}

			if ((pres & CW_PRES_RESTRICTION) == CW_PRES_ALLOWED && name && *name) {
				i = strlen(name);
				len = adsi_add_field(&adsi, msg, len, MCLASS_CALLER_NAME, (uint8_t *)name, (i > 16 ? 16 : i));
			} else {
				len = adsi_add_field(&adsi, msg, len, MCLASS_ABSENCE2,
					((pres & CW_PRES_RESTRICTION) == CW_PRES_RESTRICTED
					 	? (uint8_t *)"P" : (uint8_t *)"O"),
					1);
			}
			break;
		case ADSI_STANDARD_JCLIP:
			len = adsi_add_field(&adsi, msg, -1, JCLIP_MDMF_CALLERID, NULL, 0);
			if (number && *number)
				len = adsi_add_field(&adsi, msg, len, JCLIP_CALLER_NUMBER, (uint8_t *)number, strlen(number));
			break;
		case ADSI_STANDARD_CLIP_DTMF:
			/* Spandsp only offers Annnnn# with Dnn# for absence indication.
			 * Other possibilities are AnnnnnnC with BnnC for absence or a
			 * full AnnnnnBnnnnDnnnnnC. Available docs (well, rumours and
			 * hearsay) suggest Annnnn# should work for most variants but
			 * AnnnnnnC might be rquired for some. A world tour is needed.
			 * Sponsors?
			 */
			if (number  &&  *number)
            {
				len = adsi_add_field(&adsi, msg, -1, CLIP_DTMF_C_TERMINATED, NULL, 0);
				len = adsi_add_field(&adsi, msg, len, CLIP_DTMF_C_CALLER_NUMBER, (uint8_t *) number, strlen(number));
			}
            break;
		default:
			cw_log(LOG_ERROR, "Bad signalling type %d\n", sig);
			break;
	}

	adsi_tx_put_message(&adsi, msg, len);

	/* Anything that goes after a pre-ring polarity reversal needs some
	 * leading silence for the CPE to get ready.
	 * BT doc. SIN227 says >=100ms
	 * This should maybe be handled in the caller's FSM, not here
	 */
	if (!callwaiting && sig != ADSI_STANDARD_CLASS) {
		memset(lin, '\0', init_silence*sizeof(lin[0]));
		slen = init_silence;
	} else {
		slen = 0;
	}

	/* BT doc. SIN227 says we are required to start with 100ms silence followed
	 * by an alert tone in both idle and loop state. The loop state alert is
	 * handled elsewhere as part of the call waiting set up, but the idle state
	 * alert has to be inserted here.
	 * Yes, BT-CID HW _does_ require the alert tone.
	 */
	if (!callwaiting && sig == ADSI_STANDARD_CLIP) {
		adsi_tx_send_alert_tone(&adsi);
		/* Strictly speaking (SIN227 again) there should be 43ms silence
		 * after the alert tone. Spandsp gives us no way to tweak this
		 * stuff but the seizure length it uses is sufficient that
		 * just about everything should work.
		 */
	}

	slen += adsi_tx(&adsi, lin + slen, sizeof(lin)/sizeof(lin[0]) - slen);
	return lin2xlaw(codec, lin, slen, outbuf, outlen);
}


int tdd_generate(struct tdd_state *tdd, uint8_t *outbuf, int outlen, const char *msg, int codec)
{
	int16_t lin[MAX_CALLERID_SIZE * 3];
	adsi_tx_state_t adsi;
	int slen;

	adsi_tx_init(&adsi, ADSI_STANDARD_TDD);

	adsi_tx_put_message(&adsi, (uint8_t *) msg, strlen(msg));
    adsi_tx_set_preamble(&adsi, 0, -1, -1, -1);

	slen = adsi_tx(&adsi, lin, sizeof(lin)/sizeof(lin[0]));
	return lin2xlaw(codec, lin, slen, outbuf, outlen);
}

static void put_tdd_msg(void *user_data, const uint8_t *msg, int len)
{
	struct tdd_state *tdd;
    
	tdd = (struct tdd_state *) user_data;
	if (len < 256)
		memcpy(tdd->rx_msg, msg, len);
}

int tdd_feed(struct tdd_state *tdd, uint8_t *xlaw, int len, int codec)
{
	int16_t lin[160];
	int i, j, c;

	if (codec == CW_FORMAT_ULAW) {
		for (i = j = 0; i < len; i++) {
			lin[j++] = CW_MULAW(xlaw[i]);
			if (j == sizeof(lin)/sizeof(lin[0])) {
				adsi_rx(&tdd->rx, lin, sizeof(lin)/sizeof(lin[0]));
				j = 0;
			}
		}
	} else {
		for (i = j = 0; i < len; i++) {
			lin[j++] = CW_ALAW(xlaw[i]);
			if (j == sizeof(lin)/sizeof(lin[0])) {
				adsi_rx(&tdd->rx, lin, sizeof(lin)/sizeof(lin[0]));
				j = 0;
			}
		}
	}
	adsi_rx(&tdd->rx, lin, j);

	if (tdd->rx_msg[0]) {
		c = tdd->rx_msg[0];
		tdd->rx_msg[0] = '\0';
		return c;
	}
	return 0;
}

struct tdd_state *tdd_new(void)
{
	struct tdd_state *tdd;

	if ((tdd = malloc(sizeof(struct tdd_state)))) {
		memset(tdd, 0, sizeof(struct tdd_state));
		adsi_rx_init(&tdd->rx, ADSI_STANDARD_TDD, put_tdd_msg, tdd);
	} else
		cw_log(LOG_WARNING, "Out of memory\n");
	return tdd;
}

void tdd_free(struct tdd_state *tdd)
{
	free(tdd);
}
