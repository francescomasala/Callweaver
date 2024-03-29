/*
 * CallWeaver -- An open source telephony toolkit.
 *
 * V.110 application -- accept incoming v.110 call and spawn a login pty
 * 
 * Copyright (C) 2005-2006, David Woodhouse
 *
 * David Woodhouse <dwmw2@infradead.org>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */

#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include "callweaver/lock.h"
#include "callweaver/file.h"
#include "callweaver/logger.h"
#include "callweaver/channel.h"
#include "callweaver/pbx.h"
#include "callweaver/module.h"
#include "callweaver/transcap.h"

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>  
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdio.h>


static char *tdesc = "v.110 dialin Application";

static void *v110_app;
static const char *v110_name = "V110";
static const char *v110_synopsis = "Accept v.110 dialin connection.";
static const char *v110_syntax = "V110()";
static const char *v110_descrip = 
	"Check the incoming call type. For v.110 data calls on an mISDN\n"
	"channel, answer the call, assign a pseudotty and start a login process.\n"
	"For non-v.110 calls, the V110() application does nothing, and immediately\n"
	"passes on to the next item in the dialplan.\n";


STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

#define URATE_EBITS	0 /* In E-bits or negotiated in-band */
#define URATE_600	1
#define URATE_1200	2
#define URATE_2400	3
#define URATE_3600	4
#define URATE_4800	5
#define URATE_7200	6
#define URATE_8000	7
#define URATE_9600	8
#define URATE_14400	9	/* isdn4linux abuses this for 38400 */
#define URATE_16000	10
#define URATE_19200	11
#define URATE_32000	12

#define URATE_48000	14
#define URATE_56000	15
#define URATE_135	21 /* 134.5 */
#define URATE_100	22
#define URATE_75_1200	23 /* 75 forward, 1200 back */
#define URATE_1200_75	24 /* 1200 forward, 75 back */
#define URATE_50	25
#define URATE_75	26
#define URATE_110	27
#define URATE_150	28
#define URATE_200	29
#define URATE_300	30
#define URATE_12000	31



#define IBUF_LEN 8192
#define OBUF_LEN 1024
#define OBUF_THRESH 16

struct v110_state {
	/* Input v.110 frame buffer */
	unsigned char vframe_in[10];
	unsigned vframe_in_len;

	/* Input data buffer */
	unsigned char ibuf[IBUF_LEN];
	unsigned ibufend;
	unsigned ibufstart;
	unsigned nextibit;

	/* Output data buffer */
	unsigned char obuf[OBUF_LEN];
	unsigned obufend;
	unsigned obufstart;
	unsigned nextobit;

	/* Output v.110 frame buffer */
	unsigned nextoline;

	int bufwarning;
	int ptyfd;
	unsigned char cts, rts, sbit;
	int synccount;
	struct cw_frame f;
	unsigned char friendly[CW_FRIENDLY_OFFSET];
	unsigned char fdata[4096];
	
	void (*input_frame)(struct v110_state *, struct cw_frame *);
	void (*fill_outframe)(struct v110_state *, int);	
};

void v110_input_frame_x4(struct v110_state *vs, struct cw_frame *);
void v110_input_frame_x2(struct v110_state *vs, struct cw_frame *);
void v110_input_frame_x1(struct v110_state *vs, struct cw_frame *);
void v110_fill_outframe_x4(struct v110_state *vs, int);
void v110_fill_outframe_x2(struct v110_state *vs, int);
void v110_fill_outframe_x1(struct v110_state *vs, int);

int loginpty(char *);

static void *v110_gen_alloc (struct cw_channel *chan, void *params)
{
	return params;
}

static void v110_gen_release (struct cw_channel *chan, void *data)
{
	return;
}

static int v110_generate(struct cw_channel *chan, void *data, int want)
{
	struct v110_state *vs = data;

	vs->fill_outframe(vs, want);
	return cw_write(chan, &vs->f);
}

static struct cw_generator v110_gen = {
	.alloc = v110_gen_alloc,
	.release = v110_gen_release,
	.generate = v110_generate,
};


static int login_v110(struct cw_channel *chan, int argc, char **argv)
{
	int res=-1;
	struct localuser *u;
	struct cw_frame *f;
	struct v110_state *vs;
	int urate=-1;
	int primelen;
	const char *tmp=NULL;
	
	LOCAL_USER_ADD(u);

	/* FIXME: We can probably do this on all ISDN channels, if we
	   just fix the MISDN_URATE thing somehow. */
	if (strcasecmp(chan->tech->type, "mISDN")) {
		cw_log(LOG_DEBUG, "V.110: Not mISDN channel\n");
		LOCAL_USER_REMOVE(u);
		return 0;
	}

	if (chan->transfercapability != CW_TRANS_CAP_DIGITAL) {
		cw_log(LOG_NOTICE, "V.110: Not an async V.110 call cap[%d]\n",
			chan->transfercapability );
		LOCAL_USER_REMOVE(u);
		return 0;
	}

	tmp=pbx_builtin_getvar_helper(chan,"MISDN_URATE");

	if (tmp) {
		urate=atoi(tmp);
	}
	if (urate == -1)
		urate = URATE_9600;
	
	vs = malloc(sizeof(*vs));
	if (!vs) {
		cw_log(LOG_NOTICE, "Allocation of v.110 data structure failed\n");
		LOCAL_USER_REMOVE(u);
		return -ENOMEM;
	}

	/* FIXME: Waste bits between characters instead of relying on flow control */
	switch (urate) {
	case URATE_1200:
	case URATE_2400:
	case URATE_4800:
	case URATE_7200:
	case URATE_8000:
	case URATE_9600:
		vs->input_frame = v110_input_frame_x4;
		vs->fill_outframe = v110_fill_outframe_x4;
		primelen = 400;
		break;

	case URATE_12000:
	case URATE_16000:
	case URATE_19200:
		vs->input_frame = v110_input_frame_x2;
		vs->fill_outframe = v110_fill_outframe_x2;
		primelen = 400;
		break;

	case URATE_14400: /* NB. isdn4linux 38400 */
	case URATE_32000:
		vs->input_frame = v110_input_frame_x1;
		vs->fill_outframe = v110_fill_outframe_x1;
		primelen = 400;
		break;

	default:
		cw_log(LOG_NOTICE, "V.110 call at rate %d not supported\n",
			urate);
		free(vs);
		LOCAL_USER_REMOVE(u);
		return -EINVAL;
	}

	vs->vframe_in_len = 0;
	vs->ibufstart = vs->ibufend = 0;
	vs->nextibit = vs->nextobit = 0;
	vs->obufstart = vs->obufend = 0;
	vs->nextoline = 10;
	vs->rts = 0x80;
	vs->sbit = 0x80;
	vs->cts = 1;
	vs->synccount = 5;
	vs->bufwarning = 5;
	vs->ptyfd = loginpty(chan->cid.cid_num);
	if (vs->ptyfd < 0) {
		free(vs);
		LOCAL_USER_REMOVE(u);
		return -EIO;
	}
	
	/* Set transparent mode before we answer */
	cw_log(LOG_NOTICE, "Accepting V.110 call at rate %d\n", urate);
	
	pbx_builtin_setvar_helper(chan,"MISDN_DIGITAL_TRANS","1");

	cw_answer(chan);
	cw_set_write_format(chan, cw_best_codec(chan->nativeformats));
	cw_set_read_format(chan, cw_best_codec(chan->nativeformats));

    cw_fr_init_ex(&vs->f, CW_FRAME_VOICE, chan->readformat, NULL);
	vs->f.data = vs->fdata;
	vs->f.offset = CW_FRIENDLY_OFFSET;

	v110_generate(chan, vs, primelen);

	if (cw_generator_activate(chan, &v110_gen, vs) < 0) {
		cw_log (LOG_ERROR, "Failed to activate generator on '%s'\n", chan->name);
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	while(cw_waitfor(chan, -1) > -1) {
		f = cw_read(chan);
		if (!f)
			break;
		f->delivery.tv_sec = 0;
		f->delivery.tv_usec = 0;
		if (f->frametype == CW_FRAME_VOICE) {
			int r, want;

			vs->input_frame(vs, f);

			/* Flush v.110 incoming buffer (to pty) */
			if (vs->ibufend > vs->ibufstart)
				want = vs->ibufend - vs->ibufstart;
			else if (vs->ibufend < vs->ibufstart)
				want = IBUF_LEN - vs->ibufstart;
			else
				want = 0;
			if (want) {
				r = write(vs->ptyfd, &vs->ibuf[vs->ibufstart], want);
				if (r < 0 && errno == EAGAIN)
					r = 0;
				if (r < 0) {
					cw_log(LOG_NOTICE, "Error writing to pty: %s\n", strerror(errno));
					vs->sbit = 0x80;
					cw_softhangup(chan, CW_SOFTHANGUP_SHUTDOWN);
					goto out;
				}
				vs->ibufstart += r;
				if (vs->ibufstart == IBUF_LEN)
					vs->ibufstart = 0;

				/* Set flow control state. */
				if (r < want)
					vs->rts = 0x80;
				else 
					vs->rts = 0;
			}
			/* Replenish v.110 outgoing buffer (from pty) */
			if (vs->obufend >= vs->obufstart) {
				if (vs->obufend - vs->obufstart < OBUF_THRESH)
					want = OBUF_LEN - vs->obufend - !vs->obufstart;
				else
					want = 0;
			} else {
				if (vs->obufstart + OBUF_LEN - vs->obufend < OBUF_THRESH)
					want = vs->obufstart - vs->obufend - 1;
				else
					want = 0;
			}
			if (want) {
				r = read(vs->ptyfd, &vs->obuf[vs->obufend], want);
				if (r < 0 && errno == EAGAIN)
					r = 0;
				if (r < 0) {
					/* It's expected that we get -EIO when the user logs out */
					cw_log(LOG_DEBUG, "Error reading from pty: %s\n", strerror(errno));
					vs->sbit = 0x80;
					cw_softhangup(chan, CW_SOFTHANGUP_SHUTDOWN);
					goto out;
				}
				vs->obufend += r;
				if (vs->obufend == OBUF_LEN)
					vs->obufend = 0;
				
				if (0 && r) {
					vs->obuf[vs->obufend] = 0;
					cw_log(LOG_NOTICE, "pty: \"%s\"\n", vs->obuf+vs->obufend-r);
				}
			}

		}
		cw_fr_free(f);
	}
 out:
	/* In the error case we can get here with a frame to free */
	if (f)
		cw_fr_free(f);
	close(vs->ptyfd);
	LOCAL_USER_REMOVE(u);
	return res;
}

static void v110_process_frame(struct v110_state *vs) 
{
	int octet;


	if (0) cw_log(LOG_NOTICE, "frame %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
		vs->vframe_in[0], vs->vframe_in[1], vs->vframe_in[2],
		vs->vframe_in[3], vs->vframe_in[4], vs->vframe_in[5],
		vs->vframe_in[6], vs->vframe_in[7], vs->vframe_in[8], 
		vs->vframe_in[9]);

	/* Check that line 5 (E-bits) starts '1011'. */
	if ((vs->vframe_in[5] & 0xf) != 0xd) 
		return;

	/* Check that each other octet starts with '1' */
	if (!(vs->vframe_in[1] & vs->vframe_in[2] & vs->vframe_in[3] & 
	      vs->vframe_in[4] & vs->vframe_in[6] & vs->vframe_in[7] & 
	      vs->vframe_in[8] & vs->vframe_in[9] & 0x01))
		return;
		
	/* Extract flow control signal from last octet */
	if (vs->synccount) {
		if (!--vs->synccount) {
			cw_log(LOG_NOTICE, "V.110 synchronisation achieved\n");
			vs->sbit = 0;
			vs->rts = 0;
		}
	} else
		vs->cts = vs->vframe_in[7] & 0x80;

	for (octet = 1; octet < 10; octet++) {
		unsigned char tmp;

		/* Skip E-bits in line 5 */
		if (octet == 5)
			continue;

		tmp = vs->vframe_in[octet] & 0x7e;

		/* Search for start bit if not yet found */
		if (!vs->nextibit) {

			/* First check for no zero bits. This will be common */
			if (tmp == 0x7e)
				continue;

			/* Check for start bit being last in the octet */
			if (tmp == 0x3e) {
				vs->nextibit = 1; /* Expecting first data bit now */
				vs->ibuf[vs->ibufend] = 0;
				continue;
			}
			
			/* Scan for the start bit, copy the data bits (of which
			   there will be at least one) into the next byte of ibuf */
			vs->nextibit = 7;
			do {
				tmp >>= 1;
				vs->nextibit--;
			} while (tmp & 1);

			/* Start bit is now (host's) LSB */
			vs->ibuf[vs->ibufend] = tmp >> 1;
			continue;
		}

		tmp >>= 1;

		if (vs->nextibit < 9) {
			/* Add next bits of incoming byte to ibuf */
			vs->ibuf[vs->ibufend] |= tmp << (vs->nextibit-1);

			
			if (vs->nextibit <= 3) {
				/* Haven't finished this byte (including stop) yet */
				vs->nextibit += 6;
				continue;
			}

			tmp >>= (9 - vs->nextibit);
		}

		/* Check for stop bit */
		if (tmp & 1) {
			unsigned newend = (vs->ibufend + 1) & (IBUF_LEN-1);

			if (newend == vs->ibufstart) {
				/* Buffer full. This shouldn't happen because we should
				   have asserted flow control long ago */
				if (vs->bufwarning) {
					vs->bufwarning--;
					cw_log(LOG_NOTICE, "incoming buffer full\n");
				}
				continue;
			} else
				vs->ibufend = newend;
		} else {
			cw_log(LOG_NOTICE, "No stop bit\n");
		}
		
		/* Now, scan for next start bit */
		tmp >>= 1;
		vs->nextibit -= 4;
		while (vs->nextibit && (tmp & 1)) {
			tmp >>= 1;
			vs->nextibit--;
		}
		if (vs->nextibit > 1)
			vs->ibuf[vs->ibufend] = tmp >> 1;
			
	}

}

/* We don't handle multiple multiplexed channels. Nobody really does */
void v110_input_frame_x4(struct v110_state *vs, struct cw_frame *f)
{
	int datalen = f->datalen;
	unsigned char *frame_data = f->data;

	while (datalen) {
		if (vs->vframe_in_len < 4) {
			/* Find zero octet in buffer */
			if ( (*frame_data) & 3 ) {
				vs->vframe_in_len = 0;
				frame_data++;
				datalen--;
				continue;
			}
			/* Found a suitable byte. Add it. */
			if (++vs->vframe_in_len == 4)
				memset(vs->vframe_in, 0, 10);
			frame_data++;
			datalen--;
			continue;
		}
		/* Add in these two bits */
		vs->vframe_in[vs->vframe_in_len/4] |= 
			((*frame_data) & 3) << ((vs->vframe_in_len & 3) * 2);

		vs->vframe_in_len++;
		frame_data++;
		datalen--;

		if (vs->vframe_in_len == 40) {
			v110_process_frame(vs);
			vs->vframe_in_len = 0;
		}
	}
}

void v110_input_frame_x2(struct v110_state *vs, struct cw_frame *f)
{
	int datalen = f->datalen;
	unsigned char *frame_data = f->data;

	while (datalen) {
		if (vs->vframe_in_len < 2) {
			/* Find zero octet in buffer */
			if ( (*frame_data) & 7 ) {
				vs->vframe_in_len = 0;
				frame_data++;
				datalen--;
				continue;
			}
			/* Found a suitable byte. Add it. */
			if (++vs->vframe_in_len == 2)
				memset(vs->vframe_in, 0, 10);
			frame_data++;
			datalen--;
			continue;
		}
		/* Add in these four bits */
		vs->vframe_in[vs->vframe_in_len/2] |= 
			((*frame_data) & 15) << ((vs->vframe_in_len & 1) * 4);

		vs->vframe_in_len++;
		frame_data++;
		datalen--;

		if (vs->vframe_in_len == 20) {
			v110_process_frame(vs);
			vs->vframe_in_len = 0;
		}
	}
}

void v110_input_frame_x1(struct v110_state *vs, struct cw_frame *f)
{
	int datalen = f->datalen;
	unsigned char *frame_data = f->data;

	while (datalen) {
		if (!vs->vframe_in_len) {
			/* Find zero octet in buffer */
			if ( (*frame_data)) {
				vs->vframe_in_len = 0;
				frame_data++;
				datalen--;
				continue;
			}
			/* Found a suitable byte. Add it. */
			vs->vframe_in_len++;
			memset(vs->vframe_in, 0, 10);
			frame_data++;
			datalen--;
			continue;
		}
		/* Add byte to frame */
		vs->vframe_in[vs->vframe_in_len] = *frame_data;

		vs->vframe_in_len++;
		frame_data++;
		datalen--;

		if (vs->vframe_in_len == 10) {
			v110_process_frame(vs);
			vs->vframe_in_len = 0;
		}
	}
}

/* Some bitmasks to ease calculation. */
static unsigned char helper1[] = { 0x81, 0x81, 0x81, 0xc1, 0xe1, 0xf1, 0xf9, 0xfd, 0xff };
static unsigned char helper2[] = { 0x81, 0x83, 0x87, 0x8f, 0x9f, 0xbf };

static unsigned char v110_getline(struct v110_state *vs)
{
	unsigned char octet;
	int line = vs->nextoline++;
	int place = 2;

	if (line == 10) {
		vs->nextoline = 1;
		return 0x00; /* Header */
	} else if (line == 5) {
		return 0xfd; /* E-bits. 10111111 (reversed) */
	} else if (line == 2 || line == 7) {
		octet = 0x7f | vs->rts;
	} else {
		octet = 0x7f | vs->sbit;
	}

	/* If we're already sending a byte, finish it */
	if (vs->nextobit) {
		unsigned char tmp;

		/* Shift the data byte so that the bit we want is in bit 1 */
		tmp = vs->obuf[vs->obufstart] >> (vs->nextobit - 2);

		/* Mask in the bits we don't want to touch and the stop bit */
		tmp |= helper1[vs->nextobit - 1];

		/* Clear bits in the generated octet to match */
		octet &= tmp;

		if (vs->nextobit < 4) {
			/* There's some of this byte left; possibly just the stop bit */
			vs->nextobit += 6;
			return octet;
		}

		/* We've finished this byte */
		vs->obufstart++;
		if (vs->obufstart == OBUF_LEN)
			vs->obufstart = 0;

		if (vs->nextobit < 5) {
			/* But there's still no room in this octet for any more */
			vs->nextobit = 0;
			return octet;
		}
		/* Work out where to put the next data byte */
		place = 12 - vs->nextobit;
		vs->nextobit = 0;
	} else {
		/* Nothing to follow; start bit of new byte at bit 1 */
		place = 2;
	}

	/* Honour flow control when starting new characters */
	if (vs->cts || vs->obufstart == vs->obufend)
		return octet;

	/* 'place' is the location within the octet to start the new
	   data byte. It'll be 2 unless we've already got the tail of
	   a previous data byte in this octet. If you're staring at it
	   and think there's an off-by-one error, remember the start bit
	   which is zero, and in bit (place-1). */
	octet &= (vs->obuf[vs->obufstart] << place) | helper2[place-2];
	vs->nextobit = 8 - place;

	return octet;
}

void v110_fill_outframe_x4(struct v110_state *vs, int datalen)
{
	unsigned char *pos = vs->f.data;

	if (datalen & 3)
		datalen = (datalen + 3) & ~3;

	vs->f.datalen = vs->f.samples = datalen;

	while (datalen) {
		unsigned char tmp = v110_getline(vs);
		pos[0] = 0xfc | (tmp & 3);
		tmp >>= 2;
		pos[1] = 0xfc | (tmp & 3);
		tmp >>= 2;
		pos[2] = 0xfc | (tmp & 3);
		tmp >>= 2;
		pos[3] = 0xfc | tmp;
		pos += 4;
		datalen -= 4;
	}
}

void v110_fill_outframe_x2(struct v110_state *vs, int datalen)
{
	unsigned char *pos = vs->f.data;

	if (datalen & 1)
		vs->f.datalen = datalen = datalen + 1;

	vs->f.datalen = vs->f.samples = datalen;

	while (datalen) {
		unsigned char tmp = v110_getline(vs);
		pos[0] = 0xf0 | (tmp & 15);
		tmp >>= 4;
		pos[1] = 0xf0 | tmp;
		pos += 2;
		datalen -= 2;
	}
}

void v110_fill_outframe_x1(struct v110_state *vs, int datalen)
{
	unsigned char *pos = vs->f.data;

	vs->f.datalen = vs->f.samples = datalen;

	while (datalen) {
		*pos = v110_getline(vs);
		pos++;
		datalen--;
	}
}

/* This really ought to be a single syscall. It's slow */
static void closeall(int base)
{
	int i = sysconf(_SC_OPEN_MAX);

	while(i >= base)
		close(i--);
}

int loginpty(char *source)
{
	int master = getpt();
	int slave;
	int pid;
	char *name;
	int flags;
	sigset_t set;
	char *logincmdtext[] = { "/usr/bin/sudo", "/bin/login", "-h", NULL, NULL};
	char **logincmd = logincmdtext;

	if (master < 0) {
		cw_log(LOG_NOTICE, "Failed to allocate pty: %s\n", strerror(errno));
		return -1;
	}

	if (grantpt(master)) {
		cw_log(LOG_NOTICE, "grantpt() failed: %s\n", strerror(errno));
		close(master);
		return -1;
	}

	if (unlockpt(master)) {
		cw_log(LOG_NOTICE, "unlockpt() failed: %s\n", strerror(errno));
		close(master);
		return -1;
	}

	flags = fcntl(master, F_GETFL);
	fcntl(master, F_SETFL, flags | O_NONBLOCK);

	name = ptsname(master);
	if (!name) {
		cw_log(LOG_NOTICE, "ptsname() failed\n");
		close(master);
		return -1;
	}
	pid = fork();
	if (pid == -1) {
		cw_log(LOG_NOTICE, "fork() failed: %s\n", strerror(errno));
		close(master);
		return -1;
	}
	if (pid) {
		/* We are the parent. Wait for the child. */
		waitpid(pid, NULL, 0);
		return master;
	}

	/* We are the child. Fork again to become an orphan */
	pid = fork();
	if (pid)
		exit(1);

	sleep(2);

	/* We are the grandchild. Close everything, start a new session, and
	   open our shiny new controlling tty */
	closeall(0);
	setsid();
	slave = open(name, O_RDWR); /* Now our controlling tty */
	if (slave < 0)
		exit(1); /* Eep! */

	dup2(0, 1);
	dup2(0, 2);

	setenv("TERM", "vt100", 1);

	/* Unblock signals. */
	sigfillset(&set);
	sigprocmask(SIG_UNBLOCK, &set, NULL);

	/* Fill in the host argument, or drop it entirely */
	if (source)
		logincmd[3] = source;
	else
		logincmd[2] = NULL;

	/* Skip over the 'sudo' bit if we're already running as root */
	if (!getuid())
		logincmd++;

	execv(logincmd[0], logincmd);

	exit(1);
}

int unload_module(void)
{
	int res = 0;
	STANDARD_HANGUP_LOCALUSERS;
	res |= cw_unregister_application(v110_app);
	return res;
}

int load_module(void)
{
	v110_app = cw_register_application(v110_name, login_v110, v110_synopsis, v110_syntax, v110_descrip);
	return 0;
}

char *description(void)
{
	return tdesc;
}

int usecount(void)
{
	int res;
	STANDARD_USECOUNT(res);
	return res;
}
