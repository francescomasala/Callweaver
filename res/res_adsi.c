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
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * Includes code and algorithms from the Zapata library.
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

/*! \file
 *
 * \brief ADSI support 
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
#include <errno.h>
#include <spandsp.h>
#include <spandsp/adsi.h>

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/res/res_adsi.c $", "$Revision: 4723 $")

#include "callweaver/ulaw.h"
#include "callweaver/alaw.h"
#include "callweaver/phone_no_utils.h"
#include "callweaver/logger.h"
#include "callweaver/channel.h"
#include "callweaver/adsi.h"
#include "callweaver/module.h"
#include "callweaver/config.h"
#include "callweaver/file.h"

#define DEFAULT_ADSI_MAX_RETRIES 3

#define ADSI_MAX_INTRO 20
#define ADSI_MAX_SPEED_DIAL 6

#define ADSI_FLAG_DATAMODE	(1 << 8)

static int maxretries = DEFAULT_ADSI_MAX_RETRIES;

/* CallWeaver ADSI button definitions */
#define ADSI_SPEED_DIAL		10	/* 10-15 are reserved for speed dial */

static char intro[ADSI_MAX_INTRO][20];
static int aligns[ADSI_MAX_INTRO];

static char speeddial[ADSI_MAX_SPEED_DIAL][3][20];

static int alignment = 0;

/* Predeclare all statics to make GCC 4.x happy */
static int __adsi_transmit_message_full(struct cw_channel *, unsigned char *, int, int, int);
static int __adsi_download_connect(unsigned char *, char *,  unsigned char *, unsigned char *, int);
static int __adsi_data_mode(unsigned char *);
static int __adsi_voice_mode(unsigned char *, int);
static int __adsi_download_disconnect(unsigned char *);

static int adsi_careful_send(struct cw_channel *chan, unsigned char *buf, int len, int *remainder)
{
	/* Sends carefully on a full duplex channel by using reading for
	   timing */
	struct cw_frame *inf, outf;
	int amt;

	/* Zero out our outgoing frame */
	memset(&outf, 0, sizeof(outf));

	if (remainder && *remainder) {
		amt = len;

		/* Send remainder if provided */
		if (amt > *remainder)
			amt = *remainder;
		else
			*remainder = *remainder - amt;
        cw_fr_init_ex(&outf, CW_FRAME_VOICE, CW_FORMAT_ULAW, NULL);
		outf.data = buf;
		outf.datalen = amt;
		outf.samples = amt;
		if (cw_write(chan, &outf))
        {
			cw_log(LOG_WARNING, "Failed to carefully write frame\n");
			return -1;
		}
		/* Update pointers and lengths */
		buf += amt;
		len -= amt;
	}

	while(len) {
		amt = len;
		/* If we don't get anything at all back in a second, forget
		   about it */
		if (cw_waitfor(chan, 1000) < 1)
			return -1;
		inf = cw_read(chan);
		/* Detect hangup */
		if (!inf)
			return -1;
		if (inf->frametype == CW_FRAME_VOICE)
        {
			/* Read a voice frame */
			if (inf->subclass != CW_FORMAT_ULAW) {
				cw_log(LOG_WARNING, "Channel not in ulaw?\n");
				return -1;
			}
			/* Send no more than they sent us */
			if (amt > inf->datalen)
				amt = inf->datalen;
			else if (remainder)
				*remainder = inf->datalen - amt;
            cw_fr_init_ex(&outf, CW_FRAME_VOICE, CW_FORMAT_ULAW, NULL);
			outf.data = buf;
			outf.datalen = amt;
			outf.samples = amt;
			if (cw_write(chan, &outf))
            {
				cw_log(LOG_WARNING, "Failed to carefully write frame\n");
				return -1;
			}
			/* Update pointers and lengths */
			buf += amt;
			len -= amt;
		}
		cw_fr_free(inf);
	}
	return 0;
}

static int __adsi_transmit_messages(struct cw_channel *chan, unsigned char **msg, int *msglen, int *msgtype)
{
	/* msglen must be no more than 256 bits, each */
	uint8_t cas_buf[MAX_CALLERID_SIZE]; /* Actually only need enough for CAS - <250ms */
	adsi_tx_state_t adsi;
	void *mem = NULL;
	uint8_t *buf;
	int16_t *lin;
	int pos = 0, res;
	int x;
	int start=0;
	int retries = 0;
	char ack[3];
	/* Wait up to 500 ms for initial ACK */
	int waittime;
	struct cw_frame *f;
	int rem = 0;
	int def;

	if (chan->adsicpe == CW_ADSI_UNAVAILABLE) {
		/* Don't bother if we know they don't support ADSI */
		errno = ENOSYS;
		return -1;
	}

	while(retries < maxretries) {
		if (!(chan->adsicpe & ADSI_FLAG_DATAMODE)) {
			/* Generate CAS (no SAS) */
			cw_gen_cas(cas_buf, 680, 0, CW_FORMAT_ULAW);
		
			/* Send CAS */
			if (adsi_careful_send(chan, cas_buf, 680, NULL)) {
				cw_log(LOG_WARNING, "Unable to send CAS\n");
			}
			/* Wait For DTMF result */
			waittime = 500;
			for(;;) {
				if (((res = cw_waitfor(chan, waittime)) < 1)) {
					/* Didn't get back DTMF A in time */
					cw_log(LOG_DEBUG, "No ADSI CPE detected (%d)\n", res);
					if (!chan->adsicpe)
						chan->adsicpe = CW_ADSI_UNAVAILABLE;
					errno = ENOSYS;
					return -1;
				}
				waittime = res;
				f = cw_read(chan);
				if (!f) {
					cw_log(LOG_DEBUG, "Hangup in ADSI\n");
					return -1;
				}
				if (f->frametype == CW_FRAME_DTMF)
                {
					if (f->subclass == 'A') {
						/* Okay, this is an ADSI CPE.  Note this for future reference, too */
						if (!chan->adsicpe)
							chan->adsicpe = CW_ADSI_AVAILABLE;
						break;
					} else {
						if (f->subclass == 'D')  {
							cw_log(LOG_DEBUG, "Off-hook capable CPE only, not ADSI\n");
						} else
							cw_log(LOG_WARNING, "Unknown ADSI response '%c'\n", f->subclass);
						if (!chan->adsicpe)
							chan->adsicpe = CW_ADSI_UNAVAILABLE;
						errno =	ENOSYS;
						return -1;
					}
				}
				cw_fr_free(f);
			}

			cw_log(LOG_DEBUG, "ADSI Compatible CPE Detected\n");
		} else
			cw_log(LOG_DEBUG, "Already in data mode\n");

		if (!mem) {
			mem = malloc(24000 * 5 * sizeof(uint16_t) + 24000 * 5 * sizeof(int8_t));
			if (mem) {
				lin = mem;
				buf = mem + 24000 * 5 * sizeof(uint16_t);
			} else {
				cw_log(LOG_ERROR, "Out of memory!\n");
				return -1;
			}
		}

		x = 0;
#if 1
		def= cw_channel_defer_dtmf(chan);
#endif
		adsi_tx_init(&adsi, ADSI_STANDARD_CLASS);
		pos = 0;
		while((x < 6) && msg[x]) {
			buf[0] = msgtype[x];
			buf[1] = msglen[x] + 1;
			buf[2] = x + 1 - start;
			memcpy(buf+3, msg[x], msglen[x]);
			adsi_tx_put_message(&adsi, buf, 3 + msglen[x]);
			if (x + 1 - start != 1)
                adsi_tx_set_preamble(&adsi, 0, 0, -1, -1);
            else
                adsi_tx_set_preamble(&adsi, 0, -1, -1, -1);
			/* We should suppress the trailing marks as well except for
			 * the last message but this isn't possible. Is it a problem?
			 */
			pos += adsi_tx(&adsi, lin + pos, sizeof(lin)/sizeof(lin[0]) - pos);
			//if (option_debug)
				cw_log(LOG_DEBUG, "Message %d, of %d input bytes, %d output bytes\n", x + 1, msglen[x], pos);
			x++;
		}

		for (x = 0; x < pos; x++)
			buf[x] = CW_LIN2MU(lin[x]);

		rem = 0;
		res = adsi_careful_send(chan, buf, pos, &rem); 
		if (!def)
			cw_channel_undefer_dtmf(chan);
		if (res) {
			free(mem);
			return -1;
		}

		cw_log(LOG_DEBUG, "Sent total spill of %d bytes\n", pos);

		memset(ack, 0, sizeof(ack));
		/* Get real result */
		res = cw_readstring(chan, ack, 2, 1000, 1000, "");
		/* Check for hangup */
		if (res < 0) {
			free(mem);
			return -1;
		}
		if (ack[0] == 'D') {
			cw_log(LOG_DEBUG, "Acked up to message %d\n", atoi(ack + 1));
			start += atoi(ack + 1);
			if (start >= x)
				break;
			else {
				retries++;
				cw_log(LOG_DEBUG, "Retransmitting (%d), from %d\n", retries, start + 1);
			}
		} else {
			retries++;
			cw_log(LOG_WARNING, "Unexpected response to ack: %s (retry %d)\n", ack, retries);
		} 
	}
	if (retries >= maxretries) {
		cw_log(LOG_WARNING, "Maximum ADSI Retries (%d) exceeded\n", maxretries);
		free(mem);
		errno = ETIMEDOUT;
		return -1;
	}
	free(mem);
	return 0;
	
}

static int __adsi_begin_download(struct cw_channel *chan, char *service, unsigned char *fdn, unsigned char *sec, int version)
{
	int bytes;
	unsigned char buf[256];
	char ack[2];
	bytes = 0;
	/* Setup the resident soft key stuff, a piece at a time */
	/* Upload what scripts we can for voicemail ahead of time */
	bytes += __adsi_download_connect(buf + bytes, service, fdn, sec, version);
	if (__adsi_transmit_message_full(chan, buf, bytes, ADSI_MSG_DOWNLOAD, 0))
		return -1;
	if (cw_readstring(chan, ack, 1, 10000, 10000, ""))
		return -1;
	if (ack[0] == 'B')
		return 0;
	cw_log(LOG_DEBUG, "Download was denied by CPE\n");
	return -1;
}

static int __adsi_end_download(struct cw_channel *chan)
{
	int bytes;
	unsigned char buf[256];
        bytes = 0;
        /* Setup the resident soft key stuff, a piece at a time */
        /* Upload what scripts we can for voicemail ahead of time */
        bytes += __adsi_download_disconnect(buf + bytes);
	if (__adsi_transmit_message_full(chan, buf, bytes, ADSI_MSG_DOWNLOAD, 0))
		return -1;
	return 0;
}

static int __adsi_transmit_message_full(struct cw_channel *chan, unsigned char *msg, int msglen, int msgtype, int dowait)
{
	unsigned char *msgs[5] = { NULL, NULL, NULL, NULL, NULL };
	int msglens[5];
	int msgtypes[5];
	int newdatamode;
	int res;
	int x;
	int writeformat, readformat;
	int waitforswitch = 0;

	writeformat = chan->writeformat;
	readformat = chan->readformat;

	newdatamode = chan->adsicpe & ADSI_FLAG_DATAMODE;

	for (x=0;x<msglen;x+=(msg[x+1]+2)) {
		if (msg[x] == ADSI_SWITCH_TO_DATA) {
			cw_log(LOG_DEBUG, "Switch to data is sent!\n");
			waitforswitch++;
			newdatamode = ADSI_FLAG_DATAMODE;
		}
		
		if (msg[x] == ADSI_SWITCH_TO_VOICE) {
			cw_log(LOG_DEBUG, "Switch to voice is sent!\n");
			waitforswitch++;
			newdatamode = 0;
		}
	}
	msgs[0] = msg;

	msglens[0] = msglen;
	msgtypes[0] = msgtype;

	if (msglen > 253) {
		cw_log(LOG_WARNING, "Can't send ADSI message of %d bytes, too large\n", msglen);
		return -1;
	}

	cw_stopstream(chan);

	if (cw_set_write_format(chan, CW_FORMAT_ULAW)) {
		cw_log(LOG_WARNING, "Unable to set write format to ULAW\n");
		return -1;
	}

	if (cw_set_read_format(chan, CW_FORMAT_ULAW)) {
		cw_log(LOG_WARNING, "Unable to set read format to ULAW\n");
		if (writeformat) {
			if (cw_set_write_format(chan, writeformat)) 
				cw_log(LOG_WARNING, "Unable to restore write format to %d\n", writeformat);
		}
		return -1;
	}
	res = __adsi_transmit_messages(chan, msgs, msglens, msgtypes);

	if (dowait) {
		cw_log(LOG_DEBUG, "Wait for switch is '%d'\n", waitforswitch);
		while(waitforswitch-- && ((res = cw_waitfordigit(chan, 1000)) > 0)) { res = 0; cw_log(LOG_DEBUG, "Waiting for 'B'...\n"); }
	}
	
	if (!res)
		chan->adsicpe = (chan->adsicpe & ~ADSI_FLAG_DATAMODE) | newdatamode;

	if (writeformat)
		cw_set_write_format(chan, writeformat);
	if (readformat)
		cw_set_read_format(chan, readformat);

	if (!res)
		res = cw_safe_sleep(chan, 100 );
	return res;
}

static int __adsi_transmit_message(struct cw_channel *chan, unsigned char *msg, int msglen, int msgtype)
{
	return __adsi_transmit_message_full(chan, msg, msglen, msgtype, 1);
}

static inline int ccopy(unsigned char *dst, unsigned char *src, int max)
{
	int x=0;
	/* Carefully copy the requested data */
	while ((x < max) && src[x] && (src[x] != 0xff)) {
		dst[x] = src[x];
		x++;
	}
	return x;
}

static int __adsi_load_soft_key(unsigned char *buf, int key, char *llabel, char *slabel, char *ret, int data)
{
	int bytes=0;

	/* Abort if invalid key specified */
	if ((key < 2) || (key > 33))
		return -1;
	buf[bytes++] = ADSI_LOAD_SOFTKEY;
	/* Reserve for length */
	bytes++;
	/* Which key */
	buf[bytes++] = key;

	/* Carefully copy long label */
	bytes += ccopy(buf + bytes, (unsigned char *)llabel, 18);

	/* Place delimiter */
	buf[bytes++] = 0xff;

	/* Short label */
	bytes += ccopy(buf + bytes, (unsigned char *)slabel, 7);


	/* If specified, copy return string */
	if (ret) {
		/* Place delimiter */
		buf[bytes++] = 0xff;
		if (data)
			buf[bytes++] = ADSI_SWITCH_TO_DATA2;
		/* Carefully copy return string */
		bytes += ccopy(buf + bytes, (unsigned char *)ret, 20);

	}
	/* Replace parameter length */
	buf[1] = bytes - 2;
	return bytes;
	
}

static int __adsi_connect_session(unsigned char *buf, unsigned char *fdn, int ver)
{
	int bytes=0;
	int x;

	/* Message type */
	buf[bytes++] = ADSI_CONNECT_SESSION;

	/* Reserve space for length */
	bytes++;

	if (fdn) {
		for (x=0;x<4;x++)
			buf[bytes++] = fdn[x];
		if (ver > -1)
			buf[bytes++] = ver & 0xff;
	}

	buf[1] = bytes - 2;
	return bytes;

}

static int __adsi_download_connect(unsigned char *buf, char *service,  unsigned char *fdn, unsigned char *sec, int ver)
{
	int bytes=0;
	int x;

	/* Message type */
	buf[bytes++] = ADSI_DOWNLOAD_CONNECT;

	/* Reserve space for length */
	bytes++;

	/* Primary column */
	bytes+= ccopy(buf + bytes, (unsigned char *)service, 18);

	/* Delimiter */
	buf[bytes++] = 0xff;
	
	for (x=0;x<4;x++) {
		buf[bytes++] = fdn[x];
	}
	for (x=0;x<4;x++)
		buf[bytes++] = sec[x];
	buf[bytes++] = ver & 0xff;

	buf[1] = bytes - 2;

	return bytes;

}

static int __adsi_disconnect_session(unsigned char *buf)
{
	int bytes=0;

	/* Message type */
	buf[bytes++] = ADSI_DISC_SESSION;

	/* Reserve space for length */
	bytes++;

	buf[1] = bytes - 2;
	return bytes;

}

static int __adsi_query_cpeid(unsigned char *buf)
{
	int bytes = 0;
	buf[bytes++] = ADSI_QUERY_CPEID;
	/* Reserve space for length */
	bytes++;
	buf[1] = bytes - 2;
	return bytes;
}

static int __adsi_query_cpeinfo(unsigned char *buf)
{
	int bytes = 0;
	buf[bytes++] = ADSI_QUERY_CONFIG;
	/* Reserve space for length */
	bytes++;
	buf[1] = bytes - 2;
	return bytes;
}

static int __adsi_read_encoded_dtmf(struct cw_channel *chan, unsigned char *buf, int maxlen)
{
	int bytes = 0;
	int res;
	unsigned char current = 0;
	int gotstar = 0;
	int pos = 0;
	memset(buf, 0, sizeof(buf));
	while(bytes <= maxlen) {
		/* Wait up to a second for a digit */
		res = cw_waitfordigit(chan, 1000);
		if (!res)
			break;
		if (res == '*') {
			gotstar = 1;	
			continue;
		}
		/* Ignore anything other than a digit */
		if ((res < '0') || (res > '9'))
			continue;
		res -= '0';
		if (gotstar)
			res += 9;
		if (pos)  {
			pos = 0;
			buf[bytes++] = (res << 4) | current;
		} else {
			pos = 1;
			current = res;
		}
		gotstar = 0;
	}
	return bytes;
}

static int __adsi_get_cpeid(struct cw_channel *chan, unsigned char *cpeid, int voice)
{
	unsigned char buf[256];
	int bytes = 0;
	int res;
	bytes += __adsi_data_mode(buf);
	__adsi_transmit_message_full(chan, buf, bytes, ADSI_MSG_DISPLAY, 0);

	bytes = 0;
	bytes += __adsi_query_cpeid(buf);
	__adsi_transmit_message_full(chan, buf, bytes, ADSI_MSG_DISPLAY, 0);

	/* Get response */
	memset(buf, 0, sizeof(buf));
	res = __adsi_read_encoded_dtmf(chan, cpeid, 4);
	if (res != 4) {
		cw_log(LOG_WARNING, "Got %d bytes back of encoded DTMF, expecting 4\n", res);
		res = 0;
	} else {
		res = 1;
	}

	if (voice) {
		bytes = 0;
		bytes += __adsi_voice_mode(buf, 0);
		__adsi_transmit_message_full(chan, buf, bytes, ADSI_MSG_DISPLAY, 0);
		/* Ignore the resulting DTMF B announcing it's in voice mode */
		cw_waitfordigit(chan, 1000);
	}
	return res;
}

static int __adsi_get_cpeinfo(struct cw_channel *chan, int *width, int *height, int *buttons, int voice)
{
	unsigned char buf[256];
	int bytes = 0;
	int res;
	bytes += __adsi_data_mode(buf);
	__adsi_transmit_message_full(chan, buf, bytes, ADSI_MSG_DISPLAY, 0);

	bytes = 0;
	bytes += __adsi_query_cpeinfo(buf);
	__adsi_transmit_message_full(chan, buf, bytes, ADSI_MSG_DISPLAY, 0);

	/* Get width */
	memset(buf, 0, sizeof(buf));
	res = cw_readstring(chan, (char *)buf, 2, 1000, 500, "");
	if (res < 0)
		return res;
	if (strlen((char *)buf) != 2) {
		cw_log(LOG_WARNING, "Got %d bytes of width, expecting 2\n", res);
		res = 0;
	} else {
		res = 1;
	}
	if (width)
		*width = atoi((char *)buf);
	/* Get height */
	memset(buf, 0, sizeof(buf));
	if (res) {
		res = cw_readstring(chan, (char *)buf, 2, 1000, 500, "");
		if (res < 0)
			return res;
		if (strlen((char *)buf) != 2) {
			cw_log(LOG_WARNING, "Got %d bytes of height, expecting 2\n", res);
			res = 0;
		} else {
			res = 1;
		}	
		if (height)
			*height= atoi((char *)buf);
	}
	/* Get buttons */
	memset(buf, 0, sizeof(buf));
	if (res) {
		res = cw_readstring(chan, (char *)buf, 1, 1000, 500, "");
		if (res < 0)
			return res;
		if (strlen((char *)buf) != 1) {
			cw_log(LOG_WARNING, "Got %d bytes of buttons, expecting 1\n", res);
			res = 0;
		} else {
			res = 1;
		}	
		if (buttons)
			*buttons = atoi((char *)buf);
	}
	if (voice) {
		bytes = 0;
		bytes += __adsi_voice_mode(buf, 0);
		__adsi_transmit_message_full(chan, buf, bytes, ADSI_MSG_DISPLAY, 0);
		/* Ignore the resulting DTMF B announcing it's in voice mode */
		cw_waitfordigit(chan, 1000);
	}
	return res;
}

static int __adsi_data_mode(unsigned char *buf)
{
	int bytes=0;

	/* Message type */
	buf[bytes++] = ADSI_SWITCH_TO_DATA;

	/* Reserve space for length */
	bytes++;

	buf[1] = bytes - 2;
	return bytes;

}

static int __adsi_clear_soft_keys(unsigned char *buf)
{
	int bytes=0;

	/* Message type */
	buf[bytes++] = ADSI_CLEAR_SOFTKEY;

	/* Reserve space for length */
	bytes++;

	buf[1] = bytes - 2;
	return bytes;

}

static int __adsi_clear_screen(unsigned char *buf)
{
	int bytes=0;

	/* Message type */
	buf[bytes++] = ADSI_CLEAR_SCREEN;

	/* Reserve space for length */
	bytes++;

	buf[1] = bytes - 2;
	return bytes;

}

static int __adsi_voice_mode(unsigned char *buf, int when)
{
	int bytes=0;

	/* Message type */
	buf[bytes++] = ADSI_SWITCH_TO_VOICE;

	/* Reserve space for length */
	bytes++;

	buf[bytes++] = when & 0x7f;

	buf[1] = bytes - 2;
	return bytes;

}

static int __adsi_available(struct cw_channel *chan)
{
	int cpe = chan->adsicpe & 0xff;
	if ((cpe == CW_ADSI_AVAILABLE) ||
	    (cpe == CW_ADSI_UNKNOWN))
		return 1;
	return 0;
}

static int __adsi_download_disconnect(unsigned char *buf)
{
	int bytes=0;

	/* Message type */
	buf[bytes++] = ADSI_DOWNLOAD_DISC;

	/* Reserve space for length */
	bytes++;

	buf[1] = bytes - 2;
	return bytes;

}

static int __adsi_display(unsigned char *buf, int page, int line, int just, int wrap, 
		 char *col1, char *col2)
{
	int bytes=0;

	/* Sanity check line number */

	if (page) {
		if (line > 4) return -1;
	} else {
		if (line > 33) return -1;
	}

	if (line < 1)
		return -1;
	/* Parameter type */
	buf[bytes++] = ADSI_LOAD_VIRTUAL_DISP;
	
	/* Reserve space for size */
	bytes++;

	/* Page and wrap indicator */
	buf[bytes++] = ((page & 0x1) << 7) | ((wrap & 0x1) << 6) | (line & 0x3f);

	/* Justification */
	buf[bytes++] = (just & 0x3) << 5;

	/* Omit highlight mode definition */
	buf[bytes++] = 0xff;

	/* Primary column */
	bytes+= ccopy(buf + bytes, (unsigned char *)col1, 20);

	/* Delimiter */
	buf[bytes++] = 0xff;
	
	/* Secondary column */
	bytes += ccopy(buf + bytes, (unsigned char *)col2, 20);

	/* Update length */
	buf[1] = bytes - 2;
	
	return bytes;

}

static int __adsi_input_control(unsigned char *buf, int page, int line, int display, int format, int just)
{
	int bytes=0;

	if (page) {
		if (line > 4) return -1;
	} else {
		if (line > 33) return -1;
	}

	if (line < 1)
		return -1;

	buf[bytes++] = ADSI_INPUT_CONTROL;
	bytes++;
	buf[bytes++] = ((page & 1) << 7) | (line & 0x3f);
	buf[bytes++] = ((display & 1) << 7) | ((just & 0x3) << 4) | (format & 0x7);
	
	buf[1] = bytes - 2;
	return bytes;

}

static int __adsi_input_format(unsigned char *buf, int num, int dir, int wrap, char *format1, char *format2)
{
	int bytes = 0;

	if (!strlen((char *)format1))
		return -1;

	buf[bytes++] = ADSI_INPUT_FORMAT;
	bytes++;
	buf[bytes++] = ((dir & 1) << 7) | ((wrap & 1) << 6) | (num & 0x7);
	bytes += ccopy(buf + bytes, (unsigned char *)format1, 20);
	buf[bytes++] = 0xff;
	if (format2 && strlen((char *)format2)) {
		bytes += ccopy(buf + bytes, (unsigned char *)format2, 20);
	}
	buf[1] = bytes - 2;
	return bytes;
}

static int __adsi_set_keys(unsigned char *buf, unsigned char *keys)
{
	int bytes=0;
	int x;
	/* Message type */
	buf[bytes++] = ADSI_INIT_SOFTKEY_LINE;
	/* Space for size */
	bytes++;
	/* Key definitions */
	for (x=0;x<6;x++)
		buf[bytes++] = (keys[x] & 0x3f) ? keys[x] : (keys[x] | 0x1);
	buf[1] = bytes - 2;
	return bytes;
}

static int __adsi_set_line(unsigned char *buf, int page, int line)
{
	int bytes=0;

	/* Sanity check line number */

	if (page) {
		if (line > 4) return -1;
	} else {
		if (line > 33) return -1;
	}

	if (line < 1)
		return -1;
	/* Parameter type */
	buf[bytes++] = ADSI_LINE_CONTROL;
	
	/* Reserve space for size */
	bytes++;

	/* Page and line */
	buf[bytes++] = ((page & 0x1) << 7) | (line & 0x3f);

	buf[1] = bytes - 2;
	return bytes;

};

static int total = 0;
static int speeds = 0;

static int __adsi_channel_restore(struct cw_channel *chan)
{
	unsigned char dsp[256];
	int bytes;
	int x;
	unsigned char keyd[6];

	memset(dsp, 0, sizeof(dsp));

	/* Start with initial display setup */
	bytes = 0;
	bytes += __adsi_set_line(dsp + bytes, ADSI_INFO_PAGE, 1);

	/* Prepare key setup messages */

	if (speeds) {
		memset(keyd, 0, sizeof(keyd));
		for (x=0;x<speeds;x++) {
			keyd[x] = ADSI_SPEED_DIAL + x;
		}
		bytes += __adsi_set_keys(dsp + bytes, keyd);
	}
	__adsi_transmit_message_full(chan, dsp, bytes, ADSI_MSG_DISPLAY, 0);
	return 0;

}

static int __adsi_print(struct cw_channel *chan, char **lines, int *aligns, int voice)
{
	unsigned char buf[4096];
	int bytes=0;
	int res;
	int x;
	for(x=0;lines[x];x++) 
		bytes += __adsi_display(buf + bytes, ADSI_INFO_PAGE, x+1, aligns[x], 0, lines[x], "");
	bytes += __adsi_set_line(buf + bytes, ADSI_INFO_PAGE, 1);
	if (voice) {
		bytes += __adsi_voice_mode(buf + bytes, 0);
	}
	res = __adsi_transmit_message_full(chan, buf, bytes, ADSI_MSG_DISPLAY, 0);
	if (voice) {
		/* Ignore the resulting DTMF B announcing it's in voice mode */
		cw_waitfordigit(chan, 1000);
	}
	return res;
}

static int __adsi_load_session(struct cw_channel *chan, unsigned char *app, int ver, int data)
{
	unsigned char dsp[256];
	int bytes;
	int res;
	char resp[2];

	memset(dsp, 0, sizeof(dsp));

	/* Connect to session */
	bytes = 0;
	bytes += __adsi_connect_session(dsp + bytes, app, ver);

	if (data)
		bytes += __adsi_data_mode(dsp + bytes);

	/* Prepare key setup messages */
	if (__adsi_transmit_message_full(chan, dsp, bytes, ADSI_MSG_DISPLAY, 0))
		return -1;
	if (app) {
		res = cw_readstring(chan, resp, 1, 1200, 1200, "");
		if (res < 0)
			return -1;
		if (res) {
			cw_log(LOG_DEBUG, "No response from CPE about version.  Assuming not there.\n");
			return 0;
		}
		if (!strcmp(resp, "B")) {
			cw_log(LOG_DEBUG, "CPE has script '%s' version %d already loaded\n", app, ver);
			return 1;
		} else if (!strcmp(resp, "A")) {
			cw_log(LOG_DEBUG, "CPE hasn't script '%s' version %d already loaded\n", app, ver);
		} else {
			cw_log(LOG_WARNING, "Unexpected CPE response to script query: %s\n", resp);
		}
	} else
		return 1;
	return 0;

}

static int __adsi_unload_session(struct cw_channel *chan)
{
	unsigned char dsp[256];
	int bytes;

	memset(dsp, 0, sizeof(dsp));

	/* Connect to session */
	bytes = 0;
	bytes += __adsi_disconnect_session(dsp + bytes);
	bytes += __adsi_voice_mode(dsp + bytes, 0);

	/* Prepare key setup messages */
	if (__adsi_transmit_message_full(chan, dsp, bytes, ADSI_MSG_DISPLAY, 0))
		return -1;
	return 0;
}

static int str2align(char *s)
{
	if (!strncasecmp(s, "l", 1))
		return ADSI_JUST_LEFT;
	else if (!strncasecmp(s, "r", 1))
		return ADSI_JUST_RIGHT;
	else if (!strncasecmp(s, "i", 1))
		return ADSI_JUST_IND;
	else
		return ADSI_JUST_CENT;
}

static void init_state(void)
{
	int x;

	for (x=0;x<ADSI_MAX_INTRO;x++)
		aligns[x] = ADSI_JUST_CENT;
	strncpy(intro[0], "Welcome to the", sizeof(intro[0]) - 1);
	strncpy(intro[1], "CallWeaver", sizeof(intro[1]) - 1);
	strncpy(intro[2], "Open Source PBX", sizeof(intro[2]) - 1);
	total = 3;
	speeds = 0;
	for (x=3;x<ADSI_MAX_INTRO;x++)
		intro[x][0] = '\0';
	memset(speeddial, 0, sizeof(speeddial));
	alignment = ADSI_JUST_CENT;
}

static void adsi_load(void)
{
	int x;
	struct cw_config *conf;
	struct cw_variable *v;
	char *name, *sname;
	init_state();
	conf = cw_config_load("adsi.conf");
	if (conf) {
		x=0;
		v = cw_variable_browse(conf, "intro");
		while(v) {
			if (!strcasecmp(v->name, "alignment"))
				alignment = str2align(v->value);
			else if (!strcasecmp(v->name, "greeting")) {
				if (x < ADSI_MAX_INTRO) {
					aligns[x] = alignment;
					strncpy(intro[x], v->value, sizeof(intro[x]) - 1);
					intro[x][sizeof(intro[x]) - 1] = '\0';
					x++;
				}
			} else if (!strcasecmp(v->name, "maxretries")) {
				if (atoi(v->value) > 0)
					maxretries = atoi(v->value);
			}
			v = v->next;
		}
		v = cw_variable_browse(conf, "speeddial");
		if (x)
			total = x;
		x = 0;
		while(v) {
			char *stringp=NULL;
			stringp=v->value;
			name = strsep(&stringp, ",");
			sname = strsep(&stringp, ",");
			if (!sname) 
				sname = name;
			if (x < ADSI_MAX_SPEED_DIAL) {
				/* Up to 20 digits */
				strncpy(speeddial[x][0], v->name, sizeof(speeddial[x][0]) - 1);
				strncpy(speeddial[x][1], name, 18);
				strncpy(speeddial[x][2], sname, 7);
				x++;
			}
			v = v->next;
				
		}
		if (x)
			speeds = x;
		cw_config_destroy(conf);
	}
}

int reload(void)
{
	adsi_load();
	return 0;
}

int load_module(void)
{
	adsi_load();
	adsi_begin_download = __adsi_begin_download;
	adsi_end_download = __adsi_end_download;
	adsi_channel_restore = __adsi_channel_restore;
	adsi_print = __adsi_print;
	adsi_load_session = __adsi_load_session;
	adsi_unload_session = __adsi_unload_session;
	adsi_transmit_messages = __adsi_transmit_messages;
	adsi_transmit_message = __adsi_transmit_message;
	adsi_transmit_message_full = __adsi_transmit_message_full;
	adsi_read_encoded_dtmf = __adsi_read_encoded_dtmf;
	adsi_connect_session = __adsi_connect_session;
	adsi_query_cpeid = __adsi_query_cpeid;
	adsi_query_cpeinfo = __adsi_query_cpeinfo;
	adsi_get_cpeid = __adsi_get_cpeid;
	adsi_get_cpeinfo = __adsi_get_cpeinfo;
	adsi_download_connect = __adsi_download_connect;
	adsi_disconnect_session = __adsi_disconnect_session;
	adsi_download_disconnect = __adsi_download_disconnect;
	adsi_data_mode = __adsi_data_mode;
	adsi_clear_soft_keys = __adsi_clear_soft_keys;
	adsi_clear_screen = __adsi_clear_screen;
	adsi_voice_mode = __adsi_voice_mode;
	adsi_available = __adsi_available;
	adsi_display = __adsi_display;
	adsi_set_line = __adsi_set_line;
	adsi_load_soft_key = __adsi_load_soft_key;
	adsi_set_keys = __adsi_set_keys;
	adsi_input_control = __adsi_input_control;
	adsi_input_format = __adsi_input_format;
	return 0;
}

int unload_module(void)
{
	/* Can't unload this once we're loaded */
	return -1;
}

char *description(void)
{
	return "ADSI Resource";
}

int usecount(void)
{
	/* We should never be unloaded */
	return 1;
}
