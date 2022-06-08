/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 2006
 * Bartek (eGnarF) Kania
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
 * \brief OpenPBX timer implementation
 */

#ifndef _OPENPBX_TIMER_H
#define _OPENPBX_TIMER_H

#include "confdefs.h"

#ifdef __linux__
#include <time.h>
#else
#include <sys/time.h>
#endif

#include "openpbx/lock.h"

typedef struct __opbx_timer_t opbx_timer_t;
typedef void (opbx_timer_func) (opbx_timer_t*, void*);

typedef enum {
	OPBX_TIMER_REPEATING,
	OPBX_TIMER_ONESHOT,
 	OPBX_TIMER_SIMPLE,
} opbx_timer_type_t;

struct __opbx_timer_t {
	int active;
	opbx_timer_type_t type;
#ifdef HAVE_POSIX_TIMERS
	timer_t timer_id;
	pthread_t *thread;
#endif /* HAVE_POSIX_TIMERS */
#if defined(USE_GENERIC_TIMERS) & !defined(HAVE_POSIX_TIMERS)
	pthread_t opbx_timer_thread;
#endif /* USE_GENERIC_TIMERS */
        unsigned long interval;
	opbx_timer_func *func;
	void *user_data;
};

/* Create a repeating timer with a firing interval of 'interval' microseconds
 * the user must provide a function that is called when the timer fires.
 * The function will be called within a thread of it's own and should return
 * fairly quickly
 *
 * Returns -1 on failure, 0 otherwise */
int opbx_repeating_timer_create(opbx_timer_t *t, unsigned long interval, 
				opbx_timer_func *func, void *user_data);

/* Create a one-shot timer that only fires once after it has been started.
 * Parameters are as above.
 * This timer can be started multiple times with opbx_timer_start(), so
 * it is not necessary to recreate it after use */
int opbx_oneshot_timer_create(opbx_timer_t *t, unsigned long interval, 
			      opbx_timer_func *func, void *user_data);

/* Stop and destroy a timer */
void opbx_timer_destroy(opbx_timer_t *t);

/* Start the specified timer. Repeating timers should only be started once
 * while one-shot timers may be started again after they have fired. */
int opbx_timer_start(opbx_timer_t *t);

/* Stop a running timer */
int opbx_timer_stop(opbx_timer_t *t);

/* Reset the firing interval on a timer. This will also automatically
 * restart the timer for you. */
int opbx_timer_newtime(opbx_timer_t *t, unsigned long interval);

/* Run a one-shot timer, that is created, fired and destroyed in one call
 * This is a shorthand for calling the create, start and destroy functions. */
int opbx_simple_timer(opbx_timer_t *t, unsigned long interval,
		      opbx_timer_func *func, void *user_data);

#endif /* _OPENPBX_TIMER_H */
