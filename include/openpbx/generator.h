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
 * Synthetic frame generation definitions.
 * This only needs to be included by openpbx/channel.h
 */

#ifndef _OPENPBX_GENERATOR_H
#define _OPENPBX_GENERATOR_H

#define GENERATOR_WAIT_ITERATIONS 100

/*! Data structure used to register new generator */
struct opbx_generator {
	void *(*alloc)(struct opbx_channel *chan, void *params);
	void (*release)(struct opbx_channel *chan, void *data);
	int (*generate)(struct opbx_channel *chan, void *data, int samples);
};

/*! Requests sent to generator thread */
enum opbx_generator_requests {
	/* this really means 'no request' and MUST be zero*/
	gen_req_null = 0,
	/* deactivate current generator if any and activate new one */
	gen_req_activate,
	/* deactivate current generator, if any */
	gen_req_deactivate,
	/* deactivate current generator if any and terminate gen thread */
	gen_req_shutdown
};

/*! Generator channel data */
struct opbx_generator_channel_data {

	/*! Generator data structures mutex (those in here below).
	 * To avoid deadlocks, never acquire channel lock when
	 * generator data lock is acquired */
	opbx_mutex_t lock;

	/*! Generator thread for this channel */
	pthread_t *pgenerator_thread;

	/*! Non-zero if generator is currently active; zero otherwise */
	int gen_is_active;

	/*! Generator request condition gets signaled after
	 * changing gen_req to new value */
	opbx_cond_t gen_req_cond;

	/*! New generator request available flag */
	enum opbx_generator_requests gen_req;

	/*! New generator data */
	void *gen_data;

	/*! How many samples to generate each time*/
	int gen_samp;

	/*! New generator function */
	int (*gen_func)(struct opbx_channel *chan, void *gen_data, int gen_samp);

	/*! What to call to free (release) gen_data */
	void (*gen_free)(struct opbx_channel *chan, void *gen_data);
};

/*! Stop channel generator thread */
void opbx_generator_stop_thread(struct opbx_channel *pchan);

/*! Activate a given generator */
int opbx_generator_activate(struct opbx_channel *chan, struct opbx_generator *gen, void *params);

/*! Deactive an active generator */
void opbx_generator_deactivate(struct opbx_channel *chan);

/*! Is generator active on channel? */
inline int opbx_generator_is_active(struct opbx_channel *chan);

/*! Is the caller of this function running in the generator thread? */
inline int opbx_generator_is_self(struct opbx_channel *chan);

#endif /* _OPENPBX_GENERATOR_H */

