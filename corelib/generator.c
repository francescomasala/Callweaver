/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * Modifications by Carlos Antunes <cmantunes@gmail.com>
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

/*
 * Synthetic frame generation helper routines
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL: svn://svn.openpbx.org/openpbx/trunk/corelib/generator.c $", "$Revision: 878 $")

#include "openpbx/channel.h"	/* generator.h is included */
#include "openpbx/lock.h"

/* Needed declarations */
static void *opbx_generator_thread(void *data);
static int opbx_generator_start_thread(struct opbx_channel *pchan);

/*
 * ****************************************************************************
 *
 * Frame generation public interface
 *
 * ****************************************************************************
 */

/*
 * Stops the generator thread associated with the channel.
 * Called when the channel is being freed by opbx_channel_free.
 */
void opbx_generator_stop_thread(struct opbx_channel *pchan)
{
	opbx_mutex_lock(&pchan->gcd.lock);
	if (pchan->gcd.gen_req == gen_req_activate)
		pchan->gcd.gen_free(pchan, pchan->gcd.gen_data);
	if (pchan->gcd.pgenerator_thread) {
		pchan->gcd.gen_req = gen_req_shutdown;
		opbx_cond_signal(&pchan->gcd.gen_req_cond);
		opbx_mutex_unlock(&pchan->gcd.lock);
		pthread_join(*pchan->gcd.pgenerator_thread, NULL);
		free(pchan->gcd.pgenerator_thread);
		pchan->gcd.pgenerator_thread = NULL;
		opbx_cond_destroy(&pchan->gcd.gen_req_cond);
	} else {
		opbx_mutex_unlock(&pchan->gcd.lock);
	}
	opbx_mutex_destroy(&pchan->gcd.lock);
}

/* Activate channel generator */
int opbx_generator_activate(struct opbx_channel *chan, struct opbx_generator *gen, void *params)
{
	void *gen_data;

	/* Try to allocate new generator */
	gen_data = gen->alloc(chan, params);
	if (gen_data) {
		struct opbx_generator_channel_data *pgcd = &chan->gcd;

		/* We are going to play with new generator data structures */
		opbx_mutex_lock(&pgcd->lock);

		if(opbx_generator_start_thread(chan)) {
			/* Whoops! */
			opbx_mutex_unlock(&pgcd->lock);
			opbx_log(LOG_ERROR, "Generator activation failed: unable to start generator thread\n");
			return -1;
		}

		/* In case the generator thread hasn't yet processed a
		 * previous activation request, we need to release its data */
		if (pgcd->gen_req == gen_req_activate)
			pgcd->gen_free(chan, pgcd->gen_data);
		
		/* Setup new request */
		pgcd->gen_data = gen_data;
		pgcd->gen_func = gen->generate;
		pgcd->gen_samp = 160;
		pgcd->gen_free = gen->release;

		/* Signal generator thread to activate new generator */
		pgcd->gen_req = gen_req_activate;
		opbx_cond_signal(&pgcd->gen_req_cond);

		/* Our job is done */
		opbx_mutex_unlock(&pgcd->lock);
		return 0;
	} else {
		/* Whoops! */
		opbx_log(LOG_ERROR, "Generator activation failed\n");
		return -1;
	}
}

/* Deactivate channel generator */
void opbx_generator_deactivate(struct opbx_channel *chan)
{
        int i;
	struct opbx_generator_channel_data *pgcd = &chan->gcd;

	opbx_log(LOG_DEBUG, "Trying to deactivate generator in %s\n", 
		 chan->name);

	/* In case the generator thread hasn't yet processed a
	 * previous activation request, we need to release its data */
	opbx_mutex_lock(&pgcd->lock);
	if (pgcd->gen_req == gen_req_activate)
		pgcd->gen_free(chan, pgcd->gen_data);
		
	/* Current generator, if any, gets deactivated by signaling
	 * new request with request code being req_deactivate */
	pgcd->gen_req = gen_req_deactivate;

	/* Only signal the condition if we actually have a thread */
	if ( pgcd->pgenerator_thread )
    	    opbx_cond_signal(&pgcd->gen_req_cond);

	opbx_mutex_unlock(&pgcd->lock);

	/* Wait for the generator to deactivate */
	for (i = 0; i < GENERATOR_WAIT_ITERATIONS; i++) {
	    sched_yield();
	    if (!pgcd->gen_is_active)
		break;
	    usleep(10000);
	}
	if (pgcd->gen_is_active) {
	    opbx_log(LOG_ERROR, "Generator still active on %s!!!\n", 
		     chan->name);
	}

	opbx_log(LOG_DEBUG, "Generator on %s stopped after %d iterations\n",
		 chan->name, i);
}

/* Is channel generator active? */
int opbx_generator_is_active(struct opbx_channel *chan)
{
	struct opbx_generator_channel_data *pgcd = &chan->gcd;

	return OPBX_ATOMIC_GET(pgcd->lock, pgcd->gen_is_active);
}

/* Is the caller of this function running in the generator thread? */
int opbx_generator_is_self(struct opbx_channel *chan)
{
	struct opbx_generator_channel_data *pgcd = &chan->gcd;
	int res;

	if (pgcd->pgenerator_thread) {
		res = pthread_equal(*pgcd->pgenerator_thread, pthread_self());
	} else {
		res = 0;
	}
	return res;
}

/*
 * *****************************************************************************
 *
 * Frame generation private routines
 *
 * *****************************************************************************
 */

/* The mighty generator thread */
static void *opbx_generator_thread(void *data)
{
	struct opbx_channel *chan = data;
	struct opbx_generator_channel_data *pgcd = &chan->gcd;
	void *cur_gen_data;
	int cur_gen_samp;
	int (*cur_gen_func)(struct opbx_channel *chan, void *cur_gen_data, int cur_gen_samp);
	void (*cur_gen_free)(struct opbx_channel *chan, void *cur_gen_data);
	struct timeval tv;
	struct timespec ts;
	long sleep_interval_ns;
	int res;


	/* Loop continuously until shutdown request is received */
	opbx_mutex_lock(&pgcd->lock);
	opbx_log(LOG_DEBUG, "Generator thread started on %s\n",
		 chan->name);
	cur_gen_data = NULL;
	cur_gen_samp = 0;
	cur_gen_func = NULL;
	cur_gen_free = NULL;
	sleep_interval_ns = 0;
	for (;;) {
		/* If generator is active, wait for new request
		 * or generate after timeout. If generator is not
		 * active, just wait for new request. */
		if (pgcd->gen_is_active) {
			for (;;) {
				/* Sleep based on number of samples */
				ts.tv_nsec += sleep_interval_ns;
				if (ts.tv_nsec >= 1000000000L) {
					++ts.tv_sec;
					ts.tv_nsec -= 1000000000L;
				}
				res = opbx_cond_timedwait(&pgcd->gen_req_cond, &pgcd->lock, &ts);
				if (pgcd->gen_req) {
					/* Got new request */
					break;
				} else if (res == ETIMEDOUT) {
			 		/* We've got some generating to do. */

					/* Need to unlock generator lock prior
					 * to calling generate callback because
					 * it will try to acquire channel lock
					 * at least by opbx_write. This mean we
					 * can receive new request here */
					opbx_mutex_unlock(&pgcd->lock);
					res = cur_gen_func(chan, cur_gen_data, cur_gen_samp);
					opbx_mutex_lock(&pgcd->lock);
					if (res || pgcd->gen_req) {
						/* Got generator error or new
						 * request. Deactivate current
						 * generator */
						if (!pgcd->gen_req) {
							opbx_log(LOG_DEBUG, "Generator self-deactivating\n");
							pgcd->gen_req = gen_req_deactivate;
						}
						break;
					}
				}
			}
		} else {
			/* Just wait for new request */
			while (!pgcd->gen_req)
				opbx_cond_wait(&pgcd->gen_req_cond, &pgcd->lock);
		}

		/* If there is an activate generator, free its
		 * resources because its existence is over. */
		if (pgcd->gen_is_active) {
			cur_gen_free(chan, cur_gen_data);
			pgcd->gen_is_active = 0;
		}

		/* Process new request */
		if (pgcd->gen_req == gen_req_activate) {
			/* Activation request for a new generator. */

			/* Copy gen_* stuff to cur_gen_* stuff, set flag
			 * gen_is_active, calculate sleep interval and
			 * obtain current time using CLOCK_MONOTONIC. */
			cur_gen_data = pgcd->gen_data;
			cur_gen_samp = pgcd->gen_samp;
			cur_gen_func = pgcd->gen_func;
			cur_gen_free = pgcd->gen_free;
			pgcd->gen_is_active = -1;
			sleep_interval_ns = 1000000L * cur_gen_samp / 8;
			gettimeofday(&tv, NULL);
			ts.tv_sec = tv.tv_sec;
			ts.tv_nsec = 1000 * tv.tv_usec;

			/* CMANTUNES: is this prod thing really necessary? */
			/* Prod channel */
			opbx_prod(chan);
		} else if (pgcd->gen_req == gen_req_shutdown) {
			/* Shutdown requests. */

			/* Just break the loop */
			break;
		} else if (pgcd->gen_req != gen_req_deactivate) {
			opbx_log(LOG_DEBUG, "Unexpected generator request (%d).\n", pgcd->gen_req);
		}

		/* Reset request */
		pgcd->gen_req = gen_req_null;
	}

	/* Got request to shutdown. */
	opbx_log(LOG_DEBUG, "Generator thread shut down on %s\n", chan->name);
	opbx_mutex_unlock(&pgcd->lock);
	return NULL;
}

/* Starts the generator thread associated with the channel. */
static int opbx_generator_start_thread(struct opbx_channel *pchan)
{
	struct opbx_generator_channel_data *pgcd = &pchan->gcd;

	/* Just return if generator thread is running already */
	if (pgcd->pgenerator_thread) {
		return 0;
	}

	/* Create joinable generator thread */
	pgcd->pgenerator_thread = malloc(sizeof (pthread_t));
	if (!pgcd->pgenerator_thread) {
		return -1;
	}
	opbx_cond_init(&pgcd->gen_req_cond, NULL);
	if(opbx_pthread_create(pgcd->pgenerator_thread, NULL, opbx_generator_thread, pchan)) {
		free(pgcd->pgenerator_thread);
		pgcd->pgenerator_thread = NULL;
		opbx_cond_destroy(&pgcd->gen_req_cond);
		return -1;
	}

	/* All is good */
	return 0;
}

