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
 * \brief Scheduler Routines (from cheops-NG)
 *
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

//#undef DEBUG_SCHED

#ifdef DEBUG_SCHEDULER
#define DEBUG_LOG(a) DEBUG_M(a)
#else
#define DEBUG_LOG(a) 
#endif

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/sched.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/lock.h"
#include "openpbx/utils.h"
#include <openpbx/timer.h>

/* Determine if a is sooner than b */
#define SOONER(a,b) (((b).tv_sec > (a).tv_sec) || \
					 (((b).tv_sec == (a).tv_sec) && ((b).tv_usec > (a).tv_usec)))

struct sched {
	struct sched *next;		/* Next event in the list */
	int id; 			/* ID number of event */
	struct timeval when;		/* Absolute time event should take place */
	int resched;			/* When to reschedule */
	int variable;		/* Use return value from callback to reschedule */
	void *data; 			/* Data */
	opbx_sched_cb callback;		/* Callback */
};

struct sched_context {
	opbx_mutex_t lock;
	opbx_mutex_t del_lock;
	/* Number of events processed */
	int eventcnt;

	/* Number of outstanding schedule events */
	int schedcnt;

	/* Schedule entry and main queue */
 	struct sched *schedq;

	/* The scheduling timer */
	int usetimer;
	opbx_timer_t timer;

#ifdef SCHED_MAX_CACHE
	/* Cache of unused schedule structures and how many */
	struct sched *schedc;
	int schedccnt;
#endif
};

static void timer_set(struct sched_context *con)
{
	long ms;
	int res;

        if (con->usetimer && con->schedq) {
                ms = opbx_tvdiff_ms(con->schedq->when, opbx_tvnow());

		/* If the event has bassed start a timer that triggers almost
		 * immediately */
                if (ms <= 0)
                        ms = 1;

#ifdef DEBUG_SCHED
		DEBUG_LOG(opbx_log(LOG_DEBUG, "Setting timer 0x%lx to %ld ms\n", 
			       (unsigned long)&con->timer, ms));
#endif
		res = opbx_timer_newtime(&con->timer, ms * 1000);

		if (res == -1)
			opbx_log(LOG_ERROR, "Failed starting timer!\n");
        }
}

static void timer_func(opbx_timer_t *t, void *user_data)
{
	struct sched_context *con = (struct sched_context *) user_data;

#ifdef DEBUG_SCHED
	DEBUG_LOG(opbx_log(LOG_DEBUG, "Scheduler timer 0x%lx fired!\n", 
		       (unsigned long)t));
#endif
	opbx_sched_runq(con);

	/* Reset the timer for the next task
	 * We only need to reset if something actually happened */
	timer_set(con);
}

static struct sched_context *context_create(void)
{
	struct sched_context *tmp;
	tmp = malloc(sizeof(struct sched_context));
	if (tmp) {
          	memset(tmp, 0, sizeof(struct sched_context));
		opbx_mutex_init(&tmp->lock);
		opbx_mutex_init(&tmp->del_lock);
		tmp->eventcnt = 1;
		tmp->schedcnt = 0;
		tmp->schedq = NULL;
#ifdef SCHED_MAX_CACHE
		tmp->schedc = NULL;
		tmp->schedccnt = 0;
#endif
	}

	return tmp;
}

struct sched_context *sched_context_create(void)
{
	struct sched_context *tmp;

	tmp = context_create();

	/* Create timer */
	tmp->usetimer = 1;
	opbx_oneshot_timer_create(&tmp->timer, 0, timer_func, tmp);

	return tmp;
}

struct sched_context *sched_manual_context_create(void)
{
	struct sched_context *tmp;

	tmp = context_create();

	/* Disable timer */
	tmp->usetimer = 0;

	return tmp;
}

void sched_context_destroy(struct sched_context *con)
{
	struct sched *s, *sl;
	opbx_mutex_lock(&con->lock);

	/* Kill the timer */
	if (con->usetimer)
		opbx_timer_destroy(&con->timer);
	con->usetimer = 0;

#ifdef SCHED_MAX_CACHE
	/* Eliminate the cache */
	s = con->schedc;
	while(s) {
		sl = s;
		s = s->next;
		free(sl);
	}
#endif
	/* And the queue */
	s = con->schedq;
	while(s) {
		sl = s;
		s = s->next;
		free(sl);
	}
	/* And the context */
	opbx_mutex_unlock(&con->lock);
	opbx_mutex_destroy(&con->lock);
	free(con);
}

static struct sched *sched_alloc(struct sched_context *con)
{
	/*
	 * We keep a small cache of schedule entries
	 * to minimize the number of necessary malloc()'s
	 */
	struct sched *tmp;
#ifdef SCHED_MAX_CACHE
	if (con->schedc) {
		tmp = con->schedc;
		con->schedc = con->schedc->next;
		con->schedccnt--;
	} else
#endif
		tmp = malloc(sizeof(struct sched));
	return tmp;
}

static void sched_release(struct sched_context *con, struct sched *tmp)
{
	/*
	 * Add to the cache, or just free() if we
	 * already have too many cache entries
	 */

#ifdef SCHED_MAX_CACHE	 
	if (con->schedccnt < SCHED_MAX_CACHE) {
		tmp->next = con->schedc;
		con->schedc = tmp;
		con->schedccnt++;
	} else
#endif
	{
	    tmp->id = -1;
	    free(tmp);
	    tmp = NULL;
	}
}

int opbx_sched_wait(struct sched_context *con)
{
	/*
	 * Return the number of milliseconds 
	 * until the next scheduled event
	 */
	int ms;
#ifdef DEBUG_SCHED
	DEBUG_LOG(opbx_log(LOG_DEBUG, "opbx_sched_wait()\n"));
#endif
	opbx_mutex_lock(&con->lock);
	if (!con->schedq) {
		ms = -1;
	} else {
		ms = opbx_tvdiff_ms(con->schedq->when, opbx_tvnow());
		if (ms < 0)
			ms = 0;
	}
	opbx_mutex_unlock(&con->lock);
	return ms;
	
}


static void schedule(struct sched_context *con, struct sched *s)
{
	/*
	 * Take a sched structure and put it in the
	 * queue, such that the soonest event is
	 * first in the list. 
	 */
	 
	struct sched *last=NULL;
	struct sched *current=con->schedq;
	while(current) {
		if (SOONER(s->when, current->when))
			break;
		last = current;
		current = current->next;
	}
	/* Insert this event into the schedule */
	s->next = current;
	if (last) 
		last->next = s;
	else
		con->schedq = s;
	con->schedcnt++;
}

/*
 * given the last event *tv and the offset in milliseconds 'when',
 * computes the next value,
 */
static int sched_settime(struct timeval *tv, int when)
{
	struct timeval now = opbx_tvnow();

	/*opbx_log(LOG_DEBUG, "TV -> %lu,%lu\n", tv->tv_sec, tv->tv_usec);*/
	if (opbx_tvzero(*tv))	/* not supplied, default to now */
		*tv = now;
	*tv = opbx_tvadd(*tv, opbx_samp2tv(when, 1000));
	if (opbx_tvcmp(*tv, now) < 0) {
		opbx_log(LOG_DEBUG, "Request to schedule in the past?!?!\n");
		*tv = now;
	}
	return 0;
}


int opbx_sched_add_variable(struct sched_context *con, int when, opbx_sched_cb callback, void *data, int variable)
{
	/*
	 * Schedule callback(data) to happen when ms into the future
	 */
	struct sched *tmp;
	int res = -1;

#ifdef DEBUG_SCHED
	DEBUG_LOG(opbx_log(LOG_DEBUG, "opbx_sched_add_variable()\n"));
#endif
	if (!when) {
		opbx_log(LOG_NOTICE, "Scheduled event in 0 ms?\n");
		return -1;
	}
	opbx_mutex_lock(&con->lock);
	if ((tmp = sched_alloc(con))) {
		tmp->id = con->eventcnt++;
		tmp->callback = callback;
		tmp->data = data;
		tmp->resched = when;
		tmp->variable = variable;
		tmp->when = opbx_tv(0, 0);
		if (sched_settime(&tmp->when, when)) {
			sched_release(con, tmp);
		} else {
			schedule(con, tmp);
			res = tmp->id;
		}
	}
#ifdef DUMP_SCHEDULER
	/* Dump contents of the context while we have the lock so nothing gets screwed up by accident. */
	opbx_sched_dump(con);
#endif

	/* Set a timer for the first task */
	if (con->usetimer)
		timer_set(con);

	opbx_mutex_unlock(&con->lock);
	return res;
}

int opbx_sched_add(struct sched_context *con, int when, opbx_sched_cb callback, void *data)
{
	return opbx_sched_add_variable(con, when, callback, data, 0);
}

int opbx_sched_del(struct sched_context *con, int id)
{
	/*
	 * Delete the schedule entry with number
	 * "id".  It's nearly impossible that there
	 * would be two or more in the list with that
	 * id.
	 */
	struct sched *last=NULL, *s;
	int deleted = 0;
#ifdef DEBUG_SCHED
	DEBUG_LOG(opbx_log(LOG_DEBUG, "opbx_sched_del()\n"));
#endif
	opbx_mutex_lock(&con->lock);
	s = con->schedq;
	while(s) {
		if (s->id == id) {
			if (last)
				last->next = s->next;
			else
				con->schedq = s->next;
			con->schedcnt--;
			sched_release(con, s);
			deleted = 1;
			break;
		}
		last = s;
		s = s->next;
	}

	/* Reset the timer for the next task */
	if (con->usetimer)
		timer_set(con);

#ifdef DUMP_SCHEDULER
	/* Dump contents of the context while we have the lock so nothing gets screwed up by accident. */
	opbx_sched_dump(con);
#endif
	opbx_mutex_unlock(&con->lock);
	if (!deleted) {
		opbx_log(LOG_NOTICE, "Attempted to delete nonexistent schedule entry %d!\n", id);
#ifdef DO_CRASH
		CRASH;
#endif
		return -1;
	} else
		return 0;
}


int opbx_sched_del_with_lock(struct sched_context *con, int id)
{
	int res;	
	
	opbx_mutex_lock(&con->del_lock);
	res = opbx_sched_del(con, id);
	opbx_mutex_unlock(&con->del_lock);

	return res;

}



void opbx_sched_dump(const struct sched_context *con)
{
	/*
	 * Dump the contents of the scheduler to
	 * stderr
	 */
	struct sched *q;
	struct timeval tv = opbx_tvnow();
#ifdef SCHED_MAX_CACHE
	opbx_log(LOG_DEBUG, "OpenPBX Schedule Dump (%d in Q, %d Total, %d Cache)\n", con->schedcnt, con->eventcnt - 1, con->schedccnt);
#else
	opbx_log(LOG_DEBUG, "OpenPBX Schedule Dump (%d in Q, %d Total)\n", con->schedcnt, con->eventcnt - 1);
#endif

	opbx_log(LOG_DEBUG, "=============================================================\n");
	opbx_log(LOG_DEBUG, "|ID    Callback          Data              Time  (sec:ms)   |\n");
	opbx_log(LOG_DEBUG, "+-----+-----------------+-----------------+-----------------+\n");
 	for (q = con->schedq; q; q = q->next) {
 		struct timeval delta =  opbx_tvsub(q->when, tv);

		opbx_log(LOG_DEBUG, "|%.4d | %-15p | %-15p | %.6ld : %.6ld |\n", 
			q->id,
			q->callback,
			q->data,
			delta.tv_sec,
			(long int)delta.tv_usec);
	}
	opbx_log(LOG_DEBUG, "=============================================================\n");
	
}

int opbx_sched_runq(struct sched_context *con)
{
	/*
	 * Launch all events which need to be run at this time.
	 */
	struct sched *current;
	struct timeval tv;
	int x=0;
	int res;
#ifdef DEBUG_SCHED
	DEBUG_LOG(opbx_log(LOG_DEBUG, "opbx_sched_runq()\n"));
#endif		

	opbx_mutex_lock(&con->del_lock);

	opbx_mutex_lock(&con->lock);
	for(;;) {
		if (!con->schedq)
			break;
		
		/* schedule all events which are going to expire within 1ms.
		 * We only care about millisecond accuracy anyway, so this will
		 * help us get more than one event at one time if they are very
		 * close together.
		 */
		tv = opbx_tvadd(opbx_tvnow(), opbx_tv(0, 1000));
		if (SOONER(con->schedq->when, tv)) {
			current = con->schedq;
			con->schedq = con->schedq->next;
			con->schedcnt--;

			/*
			 * At this point, the schedule queue is still intact.  We
			 * have removed the first event and the rest is still there,
			 * so it's permissible for the callback to add new events, but
			 * trying to delete itself won't work because it isn't in
			 * the schedule queue.  If that's what it wants to do, it 
			 * should return 0.
			 */
			
			opbx_mutex_unlock(&con->lock);
			res = current->callback(current->data);
			opbx_mutex_lock(&con->lock);
			
			if (res) {
			 	/*
				 * If they return non-zero, we should schedule them to be
				 * run again.
				 */
				if (sched_settime(&current->when, current->variable? res : current->resched)) {
					sched_release(con, current);
				} else
					schedule(con, current);
			} else {
				/* No longer needed, so release it */
			 	sched_release(con, current);
			}
			x++;
		} else
			break;
	}

	opbx_mutex_unlock(&con->lock);

	opbx_mutex_unlock(&con->del_lock);

	return x;
}

long opbx_sched_when(struct sched_context *con,int id)
{
	struct sched *s;
	long secs;
#ifdef DEBUG_SCHED
	DEBUG_LOG(opbx_log(LOG_DEBUG, "opbx_sched_when()\n"));
#endif
	opbx_mutex_lock(&con->lock);
	s=con->schedq;
	while (s!=NULL) {
		if (s->id==id) break;
		s=s->next;
	}
	secs=-1;
	if (s!=NULL) {
		struct timeval now = opbx_tvnow();
		secs=s->when.tv_sec-now.tv_sec;
	}
	opbx_mutex_unlock(&con->lock);
	return secs;
}
