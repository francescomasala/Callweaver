/*
 * generic_jb: common implementation-independent jitterbuffer stuff
 *
 * Copyright (C) 2005, Attractel OOD
 *
 * Contributors:
 * Slav Klenov <slav@securax.org>
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
 * \brief Common implementation-independent jitterbuffer stuff.
 * 
 */

#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/corelib/jitterbuffer/generic_jb.c $", "$Revision: 4723 $")

#include "callweaver/frame.h"
#include "callweaver/channel.h"
#include "callweaver/logger.h"
#include "callweaver/term.h"
#include "callweaver/options.h"
#include "callweaver/utils.h"

#include "callweaver/generic_jb.h"
#include "jitterbuf_scx.h"
#include "jitterbuf_stevek.h"
#include "jitterbuf_speakup.h"


/* Internal jb flags */
#define JB_USE (1 << 0)
#define JB_TIMEBASE_INITIALIZED (1 << 1)
#define JB_CREATED (1 << 2)


/* Hooks for the abstract jb implementation */

/* Create */
typedef void * (*jb_create_impl)(struct cw_jb_conf *general_config, long resynch_threshold);
/* Destroy */
typedef void (*jb_destroy_impl)(void *jb);
/* Put first frame */
typedef int (*jb_put_first_impl)(void *jb, struct cw_frame *fin, long now, int codec);
/* Put frame */
typedef int (*jb_put_impl)(void *jb, struct cw_frame *fin, long now, int codec);
/* Get frame for now */
typedef int (*jb_get_impl)(void *jb, struct cw_frame **fout, long now, long interpl);
/* Get next */
typedef long (*jb_next_impl)(void *jb);
/* Remove first frame */
typedef int (*jb_remove_impl)(void *jb, struct cw_frame **fout);
/* Force resynch */
typedef void (*jb_force_resynch_impl)(void *jb);
/* Get information */
typedef void (*jb_info_impl)(void *jb, cw_jb_info *info);

/*!
 * \brief Jitterbuffer implementation private struct.
 */
struct cw_jb_impl
{
	char name[CW_GENERIC_JB_IMPL_NAME_SIZE];
	jb_create_impl create;
	jb_destroy_impl destroy;
	jb_put_first_impl put_first;
	jb_put_impl put;
	jb_get_impl get;
	jb_next_impl next;
	jb_remove_impl remove;
	jb_force_resynch_impl force_resync;
	jb_info_impl info;
};

/* Implementation functions */
/* scx */
static void * jb_create_scx(struct cw_jb_conf *general_config, long resynch_threshold);
static void jb_destroy_scx(void *jb);
static int jb_put_first_scx(void *jb, struct cw_frame *fin, long now, int codec);
static int jb_put_scx(void *jb, struct cw_frame *fin, long now, int codec);
static int jb_get_scx(void *jb, struct cw_frame **fout, long now, long interpl);
static long jb_next_scx(void *jb);
static int jb_remove_scx(void *jb, struct cw_frame **fout);
static void jb_force_resynch_scx(void *jb);
static void jb_info_scx(void *jb, cw_jb_info *info);
/* stevek */
static void * jb_create_stevek(struct cw_jb_conf *general_config, long resynch_threshold);
static void jb_destroy_stevek(void *jb);
static int jb_put_first_stevek(void *jb, struct cw_frame *fin, long now, int codec);
static int jb_put_stevek(void *jb, struct cw_frame *fin, long now, int codec);
static int jb_get_stevek(void *jb, struct cw_frame **fout, long now, long interpl);
static long jb_next_stevek(void *jb);
static int jb_remove_stevek(void *jb, struct cw_frame **fout);
static void jb_force_resynch_stevek(void *jb);
static void jb_info_stevek(void *jb, cw_jb_info *info);
/* SpeakUp */
static void * jb_create_speakup(struct cw_jb_conf *general_config, long resynch_threshold);
static void jb_destroy_speakup(void *jb);
static int jb_put_first_speakup(void *jb, struct cw_frame *fin, long now, int codec);
static int jb_put_speakup(void *jb, struct cw_frame *fin, long now, int codec);
static int jb_get_speakup(void *jb, struct cw_frame **fout, long now, long interpl);
static long jb_next_speakup(void *jb);
static int jb_remove_speakup(void *jb, struct cw_frame **fout);
static void jb_force_resynch_speakup(void *jb);
static void jb_info_speakup(void *jb, cw_jb_info *info);

/* Available jb implementations */
static struct cw_jb_impl avail_impl[] = 
{
	{
		.name = "fixed",
		.create = jb_create_scx,
		.destroy = jb_destroy_scx,
		.put_first = jb_put_first_scx,
		.put = jb_put_scx,
		.get = jb_get_scx,
		.next = jb_next_scx,
		.remove = jb_remove_scx,
		.force_resync = jb_force_resynch_scx,
		.info = jb_info_scx
	},
	{
		.name = "adaptive",
		.create = jb_create_stevek,
		.destroy = jb_destroy_stevek,
		.put_first = jb_put_first_stevek,
		.put = jb_put_stevek,
		.get = jb_get_stevek,
		.next = jb_next_stevek,
		.remove = jb_remove_stevek,
		.force_resync = jb_force_resynch_stevek,
		.info = jb_info_stevek
	},
	{
		.name = "speakup",
		.create = jb_create_speakup,
		.destroy = jb_destroy_speakup,
		.put_first = jb_put_first_speakup,
		.put = jb_put_speakup,
		.get = jb_get_speakup,
		.next = jb_next_speakup,
		.remove = jb_remove_speakup,
		.force_resync = jb_force_resynch_speakup,
		.info = jb_info_speakup
	}
};

static int default_impl = 0;


/* Abstract return codes */
#define JB_IMPL_OK 0
#define JB_IMPL_DROP 1
#define JB_IMPL_INTERP 2
#define JB_IMPL_NOFRAME 3

/* Translations between impl and abstract return codes */
static int scx_to_abstract_code[] =
	{JB_IMPL_OK, JB_IMPL_DROP, JB_IMPL_INTERP, JB_IMPL_NOFRAME};
static int stevek_to_abstract_code[] =
	{JB_IMPL_OK, JB_IMPL_NOFRAME, JB_IMPL_NOFRAME, JB_IMPL_INTERP, JB_IMPL_DROP, JB_IMPL_OK};

/* JB_GET actions (used only for the frames log) */
static char *jb_get_actions[] = {"Delivered", "Dropped", "Interpolated", "No"};

/* Macros for JB logs */
/*#define jb_verbose(...) cw_verbose(VERBOSE_PREFIX_3 " ***[JB LOG]*** " __VA_ARGS__)*/
#define jb_verbose(...) if(1){\
	char tmp[192];\
	char msg[128];\
	snprintf(msg, sizeof(msg), VERBOSE_PREFIX_3 "***[JB LOG]*** " __VA_ARGS__);\
	cw_verbose("%s\n", cw_term_color(tmp, msg, COLOR_BRGREEN, 0, sizeof(tmp)));}

/* Macros for the frame log files */
#define jb_framelog(...) \
if(jb->logfile) \
{ \
	fprintf(jb->logfile, __VA_ARGS__); \
	fflush(jb->logfile); \
} \


/* Internal utility functions */
static void jb_choose_impl(struct cw_channel *chan);
static void jb_get_and_deliver(struct cw_channel *chan);
static int create_jb(struct cw_channel *chan, struct cw_frame *first_frame, int codec);
static long get_now(struct cw_jb *jb, struct timeval *tv);


/* Interface ast jb functions impl */


static void jb_choose_impl(struct cw_channel *chan)
{
	struct cw_jb *jb = &chan->jb;
	struct cw_jb_conf *jbconf = &jb->conf;
	struct cw_jb_impl *test_impl;
	int i, avail_impl_count = sizeof(avail_impl) / sizeof(avail_impl[0]);
	
	jb->impl = &avail_impl[default_impl];
	
	if(*jbconf->impl == '\0')
	{
		return;
	}
		
	for(i=0; i<avail_impl_count; i++)
	{
		test_impl = &avail_impl[i];
		if(strcmp(jbconf->impl, test_impl->name) == 0)
		{
			jb->impl = test_impl;
			return;
		}
	}
}


void cw_jb_do_usecheck(struct cw_channel *c0, struct cw_channel *c1)
{
	struct cw_jb *jb0 = &c0->jb;
	struct cw_jb *jb1 = &c1->jb;
	struct cw_jb_conf *conf0 = &jb0->conf;
	struct cw_jb_conf *conf1 = &jb1->conf;
	int c0_wants_jitter = c0->tech->properties & CW_CHAN_TP_WANTSJITTER;
	int c0_creates_jitter = c0->tech->properties & CW_CHAN_TP_CREATESJITTER;
	int c0_jb_enabled = cw_test_flag(conf0, CW_GENERIC_JB_ENABLED);
	int c0_force_jb = cw_test_flag(conf0, CW_GENERIC_JB_FORCED);
	int c0_jb_timebase_initialized = cw_test_flag(jb0, JB_TIMEBASE_INITIALIZED);
	int c0_jb_created = cw_test_flag(jb0, JB_CREATED);
	int c1_wants_jitter = c1->tech->properties & CW_CHAN_TP_WANTSJITTER;
	int c1_creates_jitter = c1->tech->properties & CW_CHAN_TP_CREATESJITTER;
	int c1_jb_enabled = cw_test_flag(conf1, CW_GENERIC_JB_ENABLED);
	int c1_force_jb = cw_test_flag(conf1, CW_GENERIC_JB_FORCED);
	int c1_jb_timebase_initialized = cw_test_flag(jb1, JB_TIMEBASE_INITIALIZED);
	int c1_jb_created = cw_test_flag(jb1, JB_CREATED);
	
	if(((!c0_wants_jitter && c1_creates_jitter) || c0_force_jb) && c0_jb_enabled)
	{
		cw_set_flag(jb0, JB_USE);
		if(!c0_jb_timebase_initialized)
		{
			if(c1_jb_timebase_initialized)
			{
				memcpy(&jb0->timebase, &jb1->timebase, sizeof(struct timeval));
			}
			else
			{
				gettimeofday(&jb0->timebase, NULL);
			}
			cw_set_flag(jb0, JB_TIMEBASE_INITIALIZED);
		}
		
		if(!c0_jb_created)
		{
			jb_choose_impl(c0);
		}
	}
	
	if(((!c1_wants_jitter && c0_creates_jitter) || c1_force_jb) && c1_jb_enabled)
	{
		cw_set_flag(jb1, JB_USE);
		if(!c1_jb_timebase_initialized)
		{
			if(c0_jb_timebase_initialized)
			{
				memcpy(&jb1->timebase, &jb0->timebase, sizeof(struct timeval));
			}
			else
			{
				gettimeofday(&jb1->timebase, NULL);
			}
			cw_set_flag(jb1, JB_TIMEBASE_INITIALIZED);
		}
		
		if(!c1_jb_created)
		{
			jb_choose_impl(c1);
		}
	}

}


int cw_jb_get_when_to_wakeup(struct cw_channel *c0, struct cw_channel *c1, int time_left)
{
	struct cw_jb *jb0 = &c0->jb;
	struct cw_jb *jb1 = &c1->jb;
	int c0_use_jb = cw_test_flag(jb0, JB_USE);
	int c0_jb_is_created = cw_test_flag(jb0, JB_CREATED);
	int c1_use_jb = cw_test_flag(jb1, JB_USE);
	int c1_jb_is_created = cw_test_flag(jb1, JB_CREATED);
	int wait, wait0, wait1;
	struct timeval tv_now;
	
	if(time_left == 0)
	{
		/* No time left - the bridge will be retried */
		/* TODO: Test disable this */
		/*return 0;*/
	}
	
	if(time_left < 0)
	{
		time_left = INT_MAX;
	}
	
	gettimeofday(&tv_now, NULL);
	
	wait0 = (c0_use_jb && c0_jb_is_created) ? jb0->next - get_now(jb0, &tv_now) : time_left;
	wait1 = (c1_use_jb && c1_jb_is_created) ? jb1->next - get_now(jb1, &tv_now) : time_left;
	
	wait = wait0 < wait1 ? wait0 : wait1;
	wait = wait < time_left ? wait : time_left;
	
	if(wait == INT_MAX)
	{
		wait = -1;
	}
	else if(wait < 1)
	{
		/* don't let wait=0, because this can cause the pbx thread to loop without any sleeping at all */
		wait = 1;
	}
	
	return wait;
}


int cw_jb_put(struct cw_channel *chan, struct cw_frame *f, int codec)
{
	struct cw_jb *jb = &chan->jb;
	struct cw_jb_impl *jbimpl = jb->impl;
	void *jbobj = jb->jbobj;
	struct cw_frame *frr;
	long now = 0;
	
	if(!cw_test_flag(jb, JB_USE))
	{
		return -1;
	}
	
	if(f->frametype != CW_FRAME_VOICE)
	{
		if(f->frametype == CW_FRAME_DTMF && cw_test_flag(jb, JB_CREATED))
		{
			jb_framelog("JB_PUT {now=%ld}: Received DTMF frame. Force resynching jb...\n", now);
			jbimpl->force_resync(jbobj);
		}
		
		return -1;
	}
	
	/* We consider an enabled jitterbuffer should receive frames with valid
	   timing info. */
	if(!f->has_timing_info || f->len < 2 || f->ts < 0)
	{
		return -1;
	}
	
	/* Get us our own copy of the frame.
	 * We dup it since frisolate makes it hard to 
	 * manage memory */
	frr = cw_frdup(f);
	if(frr == NULL)
	{
		cw_log(LOG_ERROR, "Failed to isolate frame for the jitterbuffer on channel '%s'\n", chan->name);
		return -1;
	}
	
	if(!cw_test_flag(jb, JB_CREATED))
	{
		if(create_jb(chan, frr, codec))
		{
			cw_fr_free(frr);
			/* Disable the jitterbuffer */
			cw_clear_flag(jb, JB_USE);
			return -1;
		}

		cw_set_flag(jb, JB_CREATED);
		return 0;
	}
	else
	{
		now = get_now(jb, NULL);
		if(jbimpl->put(jbobj, frr, now, codec) != JB_IMPL_OK)
		{
			jb_framelog("JB_PUT {now=%ld}: Dropped frame with ts=%ld and len=%ld\n", now, frr->ts, frr->len);
			cw_fr_free(frr);
			/*return -1;*/
			/* TODO: Check this fix - should return 0 here, because the dropped frame shouldn't 
			   be delivered at all */
			return 0;
		}
		
		jb->next = jbimpl->next(jbobj);

		jb_framelog("JB_PUT {now=%ld}: Queued frame with ts=%ld and len=%ld\n", now, frr->ts, frr->len);
		
		return 0;
	}
}


void cw_jb_get_and_deliver(struct cw_channel *c0, struct cw_channel *c1)
{
	struct cw_jb *jb0 = &c0->jb;
	struct cw_jb *jb1 = &c1->jb;
	int c0_use_jb = cw_test_flag(jb0, JB_USE);
	int c0_jb_is_created = cw_test_flag(jb0, JB_CREATED);
	int c1_use_jb = cw_test_flag(jb1, JB_USE);
	int c1_jb_is_created = cw_test_flag(jb1, JB_CREATED);
	
	if(c0_use_jb && c0_jb_is_created)
	{
		jb_get_and_deliver(c0);
	}
	
	if(c1_use_jb && c1_jb_is_created)
	{
		jb_get_and_deliver(c1);
	}
}


static void jb_get_and_deliver(struct cw_channel *chan)
{
	struct cw_jb *jb = &chan->jb;
	struct cw_jb_impl *jbimpl = jb->impl;
	void *jbobj = jb->jbobj;
	struct cw_frame *f, finterp;
	long now;
	int interpolation_len, res;
	
	now = get_now(jb, NULL);
	jb->next = jbimpl->next(jbobj);

	/* Nudge now a bit */
	if (now != jb->next && abs(now - jb->next) < 
	    jb->conf.timing_compensation) {
	    jb_framelog("\tJB_GET Nudget now=%ld to now=%ld\n", now, jb->next);
	    now = jb->next;
	}

	if(now < jb->next)
	{
		jb_framelog("\tJB_GET {now=%ld}: now < next=%ld\n", now, jb->next);
		return;
	}
	
	while(now >= jb->next)
	{
		interpolation_len = cw_codec_interp_len(jb->last_format);
		res = jbimpl->get(jbobj, &f, now, interpolation_len);
		
		switch(res)
		{
		case JB_IMPL_OK:
			/* deliver the frame */
			cw_write(chan, f);
		case JB_IMPL_DROP:
			jb_framelog("\tJB_GET {now=%ld, next=%ld}: %s frame"
				    "with ts=%ld and len=%ld\n",
				    now, jb->next, jb_get_actions[res], 
				    f->ts, f->len);
			jb->last_format = f->subclass;
			cw_fr_free(f);
			break;
		case JB_IMPL_INTERP:
			/* interpolate a frame */
			f = &finterp;
			cw_fr_init_ex(f, CW_FRAME_VOICE, jb->last_format, "JB interpolation");
			f->samples = interpolation_len * 8;
			f->delivery = cw_tvadd(jb->timebase, cw_samp2tv(jb->next, 1000));
			f->offset=CW_FRIENDLY_OFFSET;
			/* deliver the interpolated frame */
			cw_write(chan, f);
			jb_framelog("\tJB_GET {now=%ld}: Interpolated frame with len=%d\n", now, interpolation_len);
			break;
		case JB_IMPL_NOFRAME:
#ifdef DEBUG			
			cw_log(LOG_DEBUG,
				"JB_IMPL_NOFRAME is retuned from the %s jb when now=%ld >= next=%ld, jbnext=%ld!\n",
				jbimpl->name, now, jb->next, jbimpl->next(jbobj));
#endif
			jb_framelog("\tJB_GET {now=%ld}: No frame for now!?\n", now);
			return;
		default:
			cw_log(LOG_ERROR, "This should never happen!\n");
			CRASH;
			break;
		}
		
		jb->next = jbimpl->next(jbobj);
	}
}


static int create_jb(struct cw_channel *chan, struct cw_frame *frr, int codec)
{
	struct cw_jb *jb;
	struct cw_jb_conf *jbconf;
	struct cw_jb_impl *jbimpl;
	void *jbobj;
	struct cw_channel *bridged;
	long now;
	char logfile_pathname[20 + CW_GENERIC_JB_IMPL_NAME_SIZE + 2*CW_CHANNEL_NAME + 1];
	char name1[CW_CHANNEL_NAME], name2[CW_CHANNEL_NAME], *tmp;
	int res;

	if (chan) {
		jb = &chan->jb;	
		jbimpl = jb->impl;
		jbconf = &jb->conf;
	} else {
		cw_log(LOG_ERROR, "No channel provided!\n");
		return 0;
	}


	jbobj = jb->jbobj = jbimpl->create(jbconf, jbconf->resync_threshold);
	if(jbobj == NULL)
	{
		cw_log(LOG_WARNING, "Failed to create jitterbuffer on channel '%s'\n", chan->name);
		return -1;
	}
	
	now = get_now(jb, NULL);
	res = jbimpl->put_first(jbobj, frr, now, codec);
	
	/* The result of putting the first frame should not differ from OK. However, its possible
	   some implementations (i.e. stevek's when resynch_threshold is specified) to drop it. */
	if(res != JB_IMPL_OK)
	{
		cw_log(LOG_WARNING, "Failed to put first frame in the jitterbuffer on channel '%s'\n", chan->name);
		/*
		jbimpl->destroy(jbobj);
		return -1;
		*/
	}
	
	/* Init next */
	jb->next = jbimpl->next(jbobj);
	
	/* Init last format for a first time. */
	jb->last_format = frr->subclass;
	
	/* Create a frame log file */
	if(cw_test_flag(jbconf, CW_GENERIC_JB_LOG))
	{
		snprintf(name2, sizeof(name2), "%s", chan->name);
		tmp = strchr(name2, '/');
		if(tmp != NULL)
		{
			*tmp = '#';
		}
		bridged = cw_bridged_channel(chan);
		if(bridged == NULL)
		{
			/* We should always have bridged chan if a jitterbuffer is in use */
			CRASH;
		}
		snprintf(name1, sizeof(name1), "%s", bridged->name);
		tmp = strchr(name1, '/');
		if(tmp != NULL)
		{
			*tmp = '#';
		}
		snprintf(logfile_pathname, sizeof(logfile_pathname),
			"/tmp/cw_%s_jb_%s--%s.log", jbimpl->name, name1, name2);
		jb->logfile = fopen(logfile_pathname, "w+b");
		
		if(jb->logfile == NULL)
		{
			cw_log(LOG_WARNING, "Failed to create frame log file with pathname '%s'\n", logfile_pathname);
		}
		
		if(res == JB_IMPL_OK)
		{
			jb_framelog("JB_PUT_FIRST {now=%ld}: Queued frame with ts=%ld and len=%ld\n",
				now, frr->ts, frr->len);
		}
		else
		{
			jb_framelog("JB_PUT_FIRST {now=%ld}: Dropped frame with ts=%ld and len=%ld\n",
				now, frr->ts, frr->len);
		}
	}
	
	jb_verbose("%s jitterbuffer created on channel %s", jbimpl->name, chan->name);
	
	/* Free the frame if it has not been queued in the jb */
	if(res != JB_IMPL_OK)
	{
	    cw_fr_free(frr);
	}
	
	return 0;
}


void cw_jb_destroy(struct cw_channel *chan)
{
	struct cw_jb *jb;
	struct cw_jb_impl *jbimpl;
	void *jbobj;
	struct cw_frame *f;

	if (chan) {
		jb = &chan->jb;	
		jbimpl = jb->impl;
		jbobj = jb->jbobj;
	} else {
		cw_log(LOG_ERROR, "Channel/jitterbuffer data is broken!\n");
		return;
	}

	if(jb->logfile != NULL)
	{
		fclose(jb->logfile);
		jb->logfile = NULL;
	}
	
	if(cw_test_flag(jb, JB_CREATED))
	{
		/* Remove and free all frames still queued in jb */
		while(jbimpl->remove(jbobj, &f) == JB_IMPL_OK)
		{
			cw_fr_free(f);
		}
		
		jbimpl->destroy(jbobj);
		jb->jbobj = NULL;
		
		cw_clear_flag(jb, JB_CREATED);
		
		jb_verbose("%s jitterbuffer destroyed on channel %s", jbimpl->name, chan->name);
	}
}


static long get_now(struct cw_jb *jb, struct timeval *tv)
{
	struct timeval now;
	
	if(tv == NULL)
	{
		tv = &now;
		gettimeofday(tv, NULL);
	}
	
	return (long) ((tv->tv_sec - jb->timebase.tv_sec) * 1000) +
		(long) ((double) (tv->tv_usec - jb->timebase.tv_usec) / 1000.0);
	
	/* TODO: For callweaver complience, we should use: */
	/* return cw_tvdiff_ms(*tv, jb->timebase); */
}


int cw_jb_read_conf(struct cw_jb_conf *conf, char *varname, char *value)
{
	int prefixlen = sizeof(CW_GENERIC_JB_CONF_PREFIX) - 1;
	char *name;
	int tmp;
	
	if(memcmp(CW_GENERIC_JB_CONF_PREFIX, varname, prefixlen) != 0)
	{
		return -1;
	}
	
	name = varname + prefixlen;
	
	if(strcmp(name, CW_GENERIC_JB_CONF_ENABLE) == 0)
	{
		if(cw_true(value))
		{
			conf->flags |= CW_GENERIC_JB_ENABLED;
		}
	}
	else if(strcmp(name, CW_GENERIC_JB_CONF_FORCE) == 0)
	{
		if(cw_true(value))
		{
			conf->flags |= CW_GENERIC_JB_FORCED;
		}
	}
	else if(strcmp(name, CW_GENERIC_JB_CONF_MIN_SIZE) == 0)
	{
		if((tmp = atoi(value)) > 0)
		{
			conf->min_size = tmp;
		}
	}
	else if(strcmp(name, CW_GENERIC_JB_CONF_MAX_SIZE) == 0)
	{
		if((tmp = atoi(value)) > 0)
		{
			conf->max_size = tmp;
		}
	}
	else if(strcmp(name, CW_GENERIC_JB_CONF_RESYNCH_THRESHOLD) == 0)
	{
		if((tmp = atoi(value)) > 0)
		{
			conf->resync_threshold = tmp;
		}
	}
	else if(strcmp(name, CW_GENERIC_JB_CONF_IMPL) == 0)
	{
		if(*value)
		{
			snprintf(conf->impl, sizeof(conf->impl), "%s", value);
		}
	}
	else if(strcmp(name, CW_GENERIC_JB_CONF_LOG) == 0)
	{
		if(cw_true(value))
		{
			conf->flags |= CW_GENERIC_JB_LOG;
		}
	}
	else if(strcmp(name, CW_GENERIC_JB_CONF_TIMING_COMP) == 0)
	{
		conf->timing_compensation = atoi(value);
	}
	else
	{
		return -1;
	}
	
	return 0;
}

void cw_jb_default_config(struct cw_jb_conf *conf)
{
	if (conf) {
		conf->flags = 0;
		conf->min_size = 60;
		conf->max_size = -1;
		conf->resync_threshold = -1;
		conf->timing_compensation = 5;
		conf->impl[0] = 0;
	} else
		cw_log(LOG_ERROR, "No jitterbuffer conf struct provided!\n");
}

void cw_jb_configure(struct cw_channel *chan, struct cw_jb_conf *conf)
{
	struct cw_jb *jb;
	struct cw_jb_conf *jbconf;
	
	if (chan) {
		jb = &chan->jb;	
		jbconf = &jb->conf;
		memcpy(jbconf, conf, sizeof(struct cw_jb_conf));
	} else 
		cw_log(LOG_ERROR, "Channel/jitterbuffer data is broken!\n");
}


void cw_jb_get_config(struct cw_channel *chan, struct cw_jb_conf *conf)
{
	struct cw_jb *jb;
	struct cw_jb_conf *jbconf;

	if (chan) {
		jb = &chan->jb;	
		jbconf = &jb->conf;
		memcpy(conf, jbconf, sizeof(struct cw_jb_conf));
	} else 
		cw_log(LOG_ERROR, "Channel/jitterbuffer data is broken!\n");
}

void cw_jb_get_info(struct cw_channel *chan, cw_jb_info *info)
{
	struct cw_jb *jb;
	struct cw_jb_impl *jbimpl = NULL;
	void *jbobj = NULL;

	if (chan) {
		jb = &chan->jb;	
		jbimpl = jb->impl;
		jbobj = jb->jbobj;
	        jbimpl->info(jbobj, info);
	} else 
		cw_log(LOG_ERROR, "Channel/jitterbuffer data is broken!\n");

}

int cw_jb_is_active(struct cw_channel *chan)
{
	struct cw_jb *jb;

	if (chan) {
		jb = &chan->jb;
		return cw_test_flag(jb, JB_CREATED);
	} else {
		cw_log(LOG_ERROR, "Trying to retreive flag but structs are freed");	
		return 0;
	}
}


/* Implementation functions */

/* scx */

static void * jb_create_scx(struct cw_jb_conf *general_config, long resynch_threshold)
{
	struct scx_jb_conf conf;
	
	conf.jbsize = general_config->max_size;
	conf.resync_threshold = resynch_threshold;
	
	return scx_jb_new(&conf);
}


static void jb_destroy_scx(void *jb)
{
	struct scx_jb *scxjb = (struct scx_jb *) jb;
	
	/* destroy the jb */
	scx_jb_destroy(scxjb);
}


static int jb_put_first_scx(void *jb, struct cw_frame *fin, long now, int codec)
{
	struct scx_jb *scxjb = (struct scx_jb *) jb;
	int res;
	
	res = scx_jb_put_first(scxjb, fin, fin->len, fin->ts, now);
	
	return scx_to_abstract_code[res];
}


static int jb_put_scx(void *jb, struct cw_frame *fin, long now, int codec)
{
	struct scx_jb *scxjb = (struct scx_jb *) jb;
	int res;
	
	res = scx_jb_put(scxjb, fin, fin->len, fin->ts, now);
	
	return scx_to_abstract_code[res];
}


static int jb_get_scx(void *jb, struct cw_frame **fout, long now, long interpl)
{
	struct scx_jb *scxjb = (struct scx_jb *) jb;
	struct scx_jb_frame frame;
	int res;
	
	res = scx_jb_get(scxjb, &frame, now, interpl);
	*fout = frame.data;
	
	return scx_to_abstract_code[res];
}


static long jb_next_scx(void *jb)
{
	struct scx_jb *scxjb = (struct scx_jb *) jb;
	
	return scx_jb_next(scxjb);
}


static int jb_remove_scx(void *jb, struct cw_frame **fout)
{
	struct scx_jb *scxjb = (struct scx_jb *) jb;
	struct scx_jb_frame frame;
	int res;
	
	res = scx_jb_remove(scxjb, &frame);
	*fout = frame.data;
	
	return scx_to_abstract_code[res];
}


static void jb_force_resynch_scx(void *jb)
{
	struct scx_jb *scxjb = (struct scx_jb *) jb;
	
	scx_jb_set_force_resynch(scxjb);
}

static void jb_info_scx(void *jb, cw_jb_info *info)
{
	/* Not yet implemented */
	memset(info, 0, sizeof(cw_jb_info));
}


/* stevek */
static void stevek_error_output(const char *fmt, ...)
{
	va_list args;
	char buf[1024];

	va_start(args, fmt);
	vsnprintf(buf, 1024, fmt, args);
	va_end(args);

	cw_log(LOG_ERROR, buf);
}

static void stevek_warning_output(const char *fmt, ...)
{
	va_list args;
	char buf[1024];

	va_start(args, fmt);
	vsnprintf(buf, 1024, fmt, args);
	va_end(args);

	cw_log(LOG_WARNING, buf);
}

static void * jb_create_stevek(struct cw_jb_conf *general_config, long resynch_threshold)
{
	jb_conf jbconf;
	jitterbuf *stevekjb;

	/* Clear settings */
	memset(&jbconf, 0, sizeof(jbconf));

	stevekjb = jb_new();
	if(stevekjb != NULL)
	{
		jbconf.min_jitterbuf = general_config->min_size;
		jbconf.max_jitterbuf = general_config->max_size;
		jbconf.resync_threshold = general_config->resync_threshold;
		jbconf.max_contig_interp = 10;
		jb_setconf(stevekjb, &jbconf);
	}

	jb_setoutput(stevek_error_output, stevek_warning_output, NULL);
	
	return stevekjb;
}


static void jb_destroy_stevek(void *jb)
{
	jitterbuf *stevekjb = (jitterbuf *) jb;
	
	jb_destroy(stevekjb);
}


static int jb_put_first_stevek(void *jb, struct cw_frame *fin, long now, int codec)
{
	return jb_put_stevek(jb, fin, now, codec);
}


static int jb_put_stevek(void *jb, struct cw_frame *fin, long now, int codec)
{
	jitterbuf *stevekjb = (jitterbuf *) jb;
	int res;
	
	res = jb_put(stevekjb, fin, JB_TYPE_VOICE, fin->len, fin->ts, now);
	
	return stevek_to_abstract_code[res];
}


static int jb_get_stevek(void *jb, struct cw_frame **fout, long now, long interpl)
{
	jitterbuf *stevekjb = (jitterbuf *) jb;
	jb_frame frame;
	int res;
	
	res = jb_get(stevekjb, &frame, now, interpl);
	*fout = frame.data;
	
	return stevek_to_abstract_code[res];
}


static long jb_next_stevek(void *jb)
{
	jitterbuf *stevekjb = (jitterbuf *) jb;
	
	return jb_next(stevekjb);
}


static int jb_remove_stevek(void *jb, struct cw_frame **fout)
{
	jitterbuf *stevekjb = (jitterbuf *) jb;
	jb_frame frame;
	int res;
	
	res = jb_getall(stevekjb, &frame);
	*fout = frame.data;
	
	return stevek_to_abstract_code[res];
}


static void jb_force_resynch_stevek(void *jb)
{
	jitterbuf *stevekjb = (jitterbuf *) jb;
	stevekjb->force_resync = 1;
}

static void jb_info_stevek(void *jb, cw_jb_info *info)
{
	jitterbuf *stevekjb = (jitterbuf *) jb;
	jb_getinfo(stevekjb, info);
}


static void * jb_create_speakup(struct cw_jb_conf *general_config, long resynch_threshold)
{
	jb_speakup_settings jbconf;
	speakup_jitterbuffer *speakupjb;

	/* Clear settings structure */
	memset(&jbconf, 0, sizeof(jbconf));

	speakupjb = jb_speakup_new();
	if(speakupjb != NULL)
	{
		jbconf.min_jb = general_config->min_size;
		jbconf.max_jb = general_config->max_size;
		jbconf.max_successive_interp = 10;
		jb_speakup_set_settings(speakupjb, &jbconf);
	}

	jb_speakup_setoutput(stevek_error_output, stevek_warning_output, NULL);
	
	return speakupjb;
}


static void jb_destroy_speakup(void *jb)
{
	speakup_jitterbuffer *speakupjb = (speakup_jitterbuffer *) jb;
	
	jb_speakup_destroy(speakupjb);
}


static int jb_put_first_speakup(void *jb, struct cw_frame *fin, long now, int codec)
{
	return jb_put_speakup(jb, fin, now, codec);
}


static int jb_put_speakup(void *jb, struct cw_frame *fin, long now, int codec)
{
	speakup_jitterbuffer *speakupjb = (speakup_jitterbuffer *) jb;
	
	jb_speakup_put(speakupjb, fin, JB_TYPE_VOICE, fin->len, fin->ts, now,
		       codec);
	
	return JB_IMPL_OK;
}


static int jb_get_speakup(void *jb, struct cw_frame **fout, long now, long interpl)
{
	speakup_jitterbuffer *speakupjb = (speakup_jitterbuffer *) jb;
	int res;
	
	res = jb_speakup_get(speakupjb, (void**)fout, now, interpl);

	speakupjb->next = now + interpl;
	
	return stevek_to_abstract_code[res];
}


static long jb_next_speakup(void *jb)
{
	speakup_jitterbuffer *speakupjb = (speakup_jitterbuffer *) jb;

	return speakupjb->next_voice_time + speakupjb->current;
}


static int jb_remove_speakup(void *jb, struct cw_frame **fout)
{
	speakup_jitterbuffer *speakupjb = (speakup_jitterbuffer *) jb;
	int res;
	
	res = jb_speakup_get_all(speakupjb, (void**)fout);
	
	return stevek_to_abstract_code[res];
}


static void jb_force_resynch_speakup(void *jb)
{
}

static void jb_info_speakup(void *jb, cw_jb_info *info)
{
	speakup_jitterbuffer *speakupjb = (speakup_jitterbuffer *) jb;
	jb_speakup_get_info(speakupjb, info);
}


