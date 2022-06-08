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
 * \brief General OpenPBX channel locking definitions.
 */

#ifndef _OPENPBX_LOCK_H
#define _OPENPBX_LOCK_H

#include <pthread.h>
#include <netdb.h>
#include <time.h>
#include <sys/param.h>

#include "openpbx/logger.h"

#define OPBX_PTHREADT_NULL (pthread_t) -1
#define OPBX_PTHREADT_STOP (pthread_t) -2

#ifdef __APPLE__
/* Provide the Linux initializers for MacOS X */
#define PTHREAD_MUTEX_RECURSIVE_NP			PTHREAD_MUTEX_RECURSIVE
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP		 { 0x4d555458, \
													   { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, \
														 0x20 } }
#endif

#ifdef __CYGWIN__
#define PTHREAD_MUTEX_RECURSIVE_NP					PTHREAD_MUTEX_RECURSIVE
#endif

#ifdef BSD
#ifdef __GNUC__
#define OPBX_MUTEX_INIT_W_CONSTRUCTORS
#else
#define OPBX_MUTEX_INIT_ON_FIRST_USE
#endif
#endif /* BSD */

/* From now on, OpenPBX REQUIRES Recursive (not error checking) mutexes
   and will not run without them. */
#ifdef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define PTHREAD_MUTEX_INIT_VALUE	PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define OPBX_MUTEX_KIND			PTHREAD_MUTEX_RECURSIVE_NP
#else
#define PTHREAD_MUTEX_INIT_VALUE	PTHREAD_MUTEX_INITIALIZER
#define OPBX_MUTEX_KIND			PTHREAD_MUTEX_RECURSIVE
#endif /* PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP */

#ifdef SOLARIS
#define OPBX_MUTEX_INIT_W_CONSTRUCTORS
#endif

#ifdef DEBUG_THREADS

#define __opbx_mutex_logger(...) { if (canlog) opbx_log(LOG_ERROR, __VA_ARGS__); else fprintf(stderr, __VA_ARGS__); }

#ifdef THREAD_CRASH
#define DO_THREAD_CRASH do { *((int *)(0)) = 1; } while(0)
#endif

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#define OPBX_MUTEX_INIT_VALUE { PTHREAD_MUTEX_INIT_VALUE, { NULL }, { 0 }, 0, { NULL }, { 0 } }

#define OPBX_MAX_REENTRANCY 10

struct opbx_mutex_info {
	pthread_mutex_t mutex;
	const char *file[OPBX_MAX_REENTRANCY];
	int lineno[OPBX_MAX_REENTRANCY];
	int reentrancy;
	const char *func[OPBX_MAX_REENTRANCY];
	pthread_t thread[OPBX_MAX_REENTRANCY];
};

typedef struct opbx_mutex_info opbx_mutex_t;

typedef pthread_cond_t opbx_cond_t;

static inline int __opbx_pthread_mutex_init_attr(const char *filename, int lineno, const char *func,
						const char *mutex_name, opbx_mutex_t *t,
						pthread_mutexattr_t *attr) 
{
#ifdef OPBX_MUTEX_INIT_W_CONSTRUCTORS
	int canlog = strcmp(filename, "logger.c");

	if ((t->mutex) != ((pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER)) {
		__opbx_mutex_logger("%s line %d (%s): Error: mutex '%s' is already initialized.\n",
				   filename, lineno, func, mutex_name);
		__opbx_mutex_logger("%s line %d (%s): Error: previously initialization of mutex '%s'.\n",
				   t->file, t->lineno, t->func, mutex_name);
#ifdef THREAD_CRASH
		DO_THREAD_CRASH;
#endif
		return 0;
	}
#endif

	t->file[0] = filename;
	t->lineno[0] = lineno;
	t->func[0] = func;
	t->thread[0]  = 0;
	t->reentrancy = 0;

	return pthread_mutex_init(&t->mutex, attr);
}

static inline int __opbx_pthread_mutex_init(const char *filename, int lineno, const char *func,
					   const char *mutex_name, opbx_mutex_t *t)
{
	static pthread_mutexattr_t  attr;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, OPBX_MUTEX_KIND);

	return __opbx_pthread_mutex_init_attr(filename, lineno, func, mutex_name, t, &attr);
}

static inline int __opbx_pthread_mutex_destroy(const char *filename, int lineno, const char *func,
						const char *mutex_name, opbx_mutex_t *t)
{
	int res;
	int canlog = strcmp(filename, "logger.c");

#ifdef OPBX_MUTEX_INIT_W_CONSTRUCTORS
	if ((t->mutex) == ((pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER)) {
		__opbx_mutex_logger("%s line %d (%s): Error: mutex '%s' is uninitialized.\n",
				   filename, lineno, func, mutex_name);
	}
#endif

	res = pthread_mutex_trylock(&t->mutex);
	switch (res) {
	case 0:
		pthread_mutex_unlock(&t->mutex);
		break;
	case EINVAL:
		__opbx_mutex_logger("%s line %d (%s): Error: attempt to destroy invalid mutex '%s'.\n",
				  filename, lineno, func, mutex_name);
		break;
	case EBUSY:
		__opbx_mutex_logger("%s line %d (%s): Error: attempt to destroy locked mutex '%s'.\n",
				   filename, lineno, func, mutex_name);
		__opbx_mutex_logger("%s line %d (%s): Error: '%s' was locked here.\n",
				   t->file[t->reentrancy-1], t->lineno[t->reentrancy-1], t->func[t->reentrancy-1], mutex_name);
		break;
	}

	if ((res = pthread_mutex_destroy(&t->mutex)))
		__opbx_mutex_logger("%s line %d (%s): Error destroying mutex: %s\n",
				   filename, lineno, func, strerror(res));
#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
	else
		t->mutex = PTHREAD_MUTEX_INIT_VALUE;
#endif
	t->file[0] = filename;
	t->lineno[0] = lineno;
	t->func[0] = func;

	return res;
}

#if defined(OPBX_MUTEX_INIT_W_CONSTRUCTORS)
/* if OPBX_MUTEX_INIT_W_CONSTRUCTORS is defined, use file scope
 constrictors/destructors to create/destroy mutexes.  */
#define __OPBX_MUTEX_DEFINE(scope,mutex) \
	scope opbx_mutex_t mutex = OPBX_MUTEX_INIT_VALUE; \
static void  __attribute__ ((constructor)) init_##mutex(void) \
{ \
	opbx_mutex_init(&mutex); \
} \
static void  __attribute__ ((destructor)) fini_##mutex(void) \
{ \
	opbx_mutex_destroy(&mutex); \
}
#elif defined(OPBX_MUTEX_INIT_ON_FIRST_USE)
/* if OPBX_MUTEX_INIT_ON_FIRST_USE is defined, mutexes are created on
 first use.  The performance impact on FreeBSD should be small since
 the pthreads library does this itself to initialize errror checking
 (defaulty type) mutexes.  If nither is defined, the pthreads librariy
 does the initialization itself on first use. */ 
#define __OPBX_MUTEX_DEFINE(scope,mutex) \
	scope opbx_mutex_t mutex = OPBX_MUTEX_INIT_VALUE
#else /* OPBX_MUTEX_INIT_W_CONSTRUCTORS */
/* By default, use static initialization of mutexes.*/ 
#define __OPBX_MUTEX_DEFINE(scope,mutex) \
	scope opbx_mutex_t mutex = OPBX_MUTEX_INIT_VALUE
#endif /* OPBX_MUTEX_INIT_W_CONSTRUCTORS */

static inline int __opbx_pthread_mutex_lock(const char *filename, int lineno, const char *func,
                                           const char* mutex_name, opbx_mutex_t *t)
{
	int res;
	int canlog = strcmp(filename, "logger.c");

#if defined(OPBX_MUTEX_INIT_W_CONSTRUCTORS) || defined(OPBX_MUTEX_INIT_ON_FIRST_USE)
	if ((t->mutex) == ((pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER)) {
#ifdef OPBX_MUTEX_INIT_W_CONSTRUCTORS
		opbx_mutex_logger("%s line %d (%s): Error: mutex '%s' is uninitialized.\n",
				 filename, lineno, func, mutex_name);
#endif
		opbx_mutex_init(t);
	}
#endif /* defined(OPBX_MUTEX_INIT_W_CONSTRUCTORS) || defined(OPBX_MUTEX_INIT_ON_FIRST_USE) */

#ifdef DETECT_DEADLOCKS
	{
		time_t seconds = time(NULL);
		time_t current;
		do {
			res = pthread_mutex_trylock(&t->mutex);
			if (res == EBUSY) {
				current = time(NULL);
				if ((current - seconds) && (!((current - seconds) % 5))) {
					__opbx_mutex_logger("%s line %d (%s): Deadlock? waited %d sec for mutex '%s'?\n",
							   filename, lineno, func, (int)(current - seconds), mutex_name);
					__opbx_mutex_logger("%s line %d (%s): '%s' was locked here.\n",
							   *t->file, *t->lineno, *t->func, mutex_name);
				}
				usleep(200);
			}
		} while (res == EBUSY);
	}
#else
	res = pthread_mutex_lock(&t->mutex);
#endif /* DETECT_DEADLOCKS */

	if (!res) {
		if (t->reentrancy < OPBX_MAX_REENTRANCY) {
			t->file[t->reentrancy] = filename;
			t->lineno[t->reentrancy] = lineno;
			t->func[t->reentrancy] = func;
			t->thread[t->reentrancy] = pthread_self();
			t->reentrancy++;
		} else {
			__opbx_mutex_logger("%s line %d (%s): '%s' really deep reentrancy!\n",
							   filename, lineno, func, mutex_name);
		}
	} else {
		__opbx_mutex_logger("%s line %d (%s): Error obtaining mutex: %s\n",
				   filename, lineno, func, strerror(errno));
#ifdef THREAD_CRASH
		DO_THREAD_CRASH;
#endif
	}

	return res;
}

static inline int __opbx_pthread_mutex_trylock(const char *filename, int lineno, const char *func,
                                              const char* mutex_name, opbx_mutex_t *t)
{
	int res;
	int canlog = strcmp(filename, "logger.c");

#if defined(OPBX_MUTEX_INIT_W_CONSTRUCTORS) || defined(OPBX_MUTEX_INIT_ON_FIRST_USE)
	if ((t->mutex) == ((pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER)) {
#ifdef OPBX_MUTEX_INIT_W_CONSTRUCTORS

		__opbx_mutex_logger("%s line %d (%s): Error: mutex '%s' is uninitialized.\n",
				   filename, lineno, func, mutex_name);
#endif
		opbx_mutex_init(t);
	}
#endif /* defined(OPBX_MUTEX_INIT_W_CONSTRUCTORS) || defined(OPBX_MUTEX_INIT_ON_FIRST_USE) */

	if (!(res = pthread_mutex_trylock(&t->mutex))) {
		if (t->reentrancy < OPBX_MAX_REENTRANCY) {
			t->file[t->reentrancy] = filename;
			t->lineno[t->reentrancy] = lineno;
			t->func[t->reentrancy] = func;
			t->thread[t->reentrancy] = pthread_self();
			t->reentrancy++;
		} else {
			__opbx_mutex_logger("%s line %d (%s): '%s' really deep reentrancy!\n",
							   filename, lineno, func, mutex_name);
		}
	}

	return res;
}

static inline int __opbx_pthread_mutex_unlock(const char *filename, int lineno, const char *func,
					     const char *mutex_name, opbx_mutex_t *t)
{
	int res;
	int canlog = strcmp(filename, "logger.c");

#ifdef OPBX_MUTEX_INIT_W_CONSTRUCTORS
	if ((t->mutex) == ((pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER)) {
		__opbx_mutex_logger("%s line %d (%s): Error: mutex '%s' is uninitialized.\n",
				   filename, lineno, func, mutex_name);
	}
#endif

	if (t->reentrancy && (t->thread[t->reentrancy-1] != pthread_self())) {
		__opbx_mutex_logger("%s line %d (%s): attempted unlock mutex '%s' without owning it!\n",
				   filename, lineno, func, mutex_name);
		__opbx_mutex_logger("%s line %d (%s): '%s' was locked here.\n",
				   t->file[t->reentrancy-1], t->lineno[t->reentrancy-1], t->func[t->reentrancy-1], mutex_name);
#ifdef THREAD_CRASH
		DO_THREAD_CRASH;
#endif
	}

	if (--t->reentrancy < 0) {
		__opbx_mutex_logger("%s line %d (%s): mutex '%s' freed more times than we've locked!\n",
				   filename, lineno, func, mutex_name);
		t->reentrancy = 0;
	}

	if (t->reentrancy < OPBX_MAX_REENTRANCY) {
		t->file[t->reentrancy] = NULL;
		t->lineno[t->reentrancy] = 0;
		t->func[t->reentrancy] = NULL;
		t->thread[t->reentrancy] = 0;
	}

	if ((res = pthread_mutex_unlock(&t->mutex))) {
		__opbx_mutex_logger("%s line %d (%s): Error releasing mutex: %s\n", 
				   filename, lineno, func, strerror(res));
#ifdef THREAD_CRASH
		DO_THREAD_CRASH;
#endif
	}

	return res;
}

static inline int __opbx_cond_init(const char *filename, int lineno, const char *func,
				  const char *cond_name, opbx_cond_t *cond, pthread_condattr_t *cond_attr)
{
	return pthread_cond_init(cond, cond_attr);
}

static inline int __opbx_cond_signal(const char *filename, int lineno, const char *func,
				    const char *cond_name, opbx_cond_t *cond)
{
	return pthread_cond_signal(cond);
}

static inline int __opbx_cond_broadcast(const char *filename, int lineno, const char *func,
				       const char *cond_name, opbx_cond_t *cond)
{
	return pthread_cond_broadcast(cond);
}

static inline int __opbx_cond_destroy(const char *filename, int lineno, const char *func,
				     const char *cond_name, opbx_cond_t *cond)
{
	return pthread_cond_destroy(cond);
}

static inline int __opbx_cond_wait(const char *filename, int lineno, const char *func,
				  const char *cond_name, const char *mutex_name,
				  opbx_cond_t *cond, opbx_mutex_t *t)
{
	int res;
	int canlog = strcmp(filename, "logger.c");

#ifdef OPBX_MUTEX_INIT_W_CONSTRUCTORS
	if ((t->mutex) == ((pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER)) {
		__opbx_mutex_logger("%s line %d (%s): Error: mutex '%s' is uninitialized.\n",
				   filename, lineno, func, mutex_name);
	}
#endif

	if (t->reentrancy && (t->thread[t->reentrancy-1] != pthread_self())) {
		__opbx_mutex_logger("%s line %d (%s): attempted unlock mutex '%s' without owning it!\n",
				   filename, lineno, func, mutex_name);
		__opbx_mutex_logger("%s line %d (%s): '%s' was locked here.\n",
				   t->file[t->reentrancy-1], t->lineno[t->reentrancy-1], t->func[t->reentrancy-1], mutex_name);
#ifdef THREAD_CRASH
		DO_THREAD_CRASH;
#endif
	}

	if (--t->reentrancy < 0) {
		__opbx_mutex_logger("%s line %d (%s): mutex '%s' freed more times than we've locked!\n",
				   filename, lineno, func, mutex_name);
		t->reentrancy = 0;
	}

	if (t->reentrancy < OPBX_MAX_REENTRANCY) {
		t->file[t->reentrancy] = NULL;
		t->lineno[t->reentrancy] = 0;
		t->func[t->reentrancy] = NULL;
		t->thread[t->reentrancy] = 0;
	}

	if ((res = pthread_cond_wait(cond, &t->mutex))) {
		__opbx_mutex_logger("%s line %d (%s): Error waiting on condition mutex '%s'\n", 
				   filename, lineno, func, strerror(res));
#ifdef THREAD_CRASH
		DO_THREAD_CRASH;
#endif
	} else {
		if (t->reentrancy < OPBX_MAX_REENTRANCY) {
			t->file[t->reentrancy] = filename;
			t->lineno[t->reentrancy] = lineno;
			t->func[t->reentrancy] = func;
			t->thread[t->reentrancy] = pthread_self();
			t->reentrancy++;
		} else {
			__opbx_mutex_logger("%s line %d (%s): '%s' really deep reentrancy!\n",
							   filename, lineno, func, mutex_name);
		}
	}

	return res;
}

static inline int __opbx_cond_timedwait(const char *filename, int lineno, const char *func,
				       const char *cond_name, const char *mutex_name, opbx_cond_t *cond,
				       opbx_mutex_t *t, const struct timespec *abstime)
{
	int res;
	int canlog = strcmp(filename, "logger.c");

#ifdef OPBX_MUTEX_INIT_W_CONSTRUCTORS
	if ((t->mutex) == ((pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER)) {
		__opbx_mutex_logger("%s line %d (%s): Error: mutex '%s' is uninitialized.\n",
				   filename, lineno, func, mutex_name);
	}
#endif

	if (t->reentrancy && (t->thread[t->reentrancy-1] != pthread_self())) {
		__opbx_mutex_logger("%s line %d (%s): attempted unlock mutex '%s' without owning it!\n",
				   filename, lineno, func, mutex_name);
		__opbx_mutex_logger("%s line %d (%s): '%s' was locked here.\n",
				   t->file[t->reentrancy-1], t->lineno[t->reentrancy-1], t->func[t->reentrancy-1], mutex_name);
#ifdef THREAD_CRASH
		DO_THREAD_CRASH;
#endif
	}

	if (--t->reentrancy < 0) {
		__opbx_mutex_logger("%s line %d (%s): mutex '%s' freed more times than we've locked!\n",
				   filename, lineno, func, mutex_name);
		t->reentrancy = 0;
	}

	if (t->reentrancy < OPBX_MAX_REENTRANCY) {
		t->file[t->reentrancy] = NULL;
		t->lineno[t->reentrancy] = 0;
		t->func[t->reentrancy] = NULL;
		t->thread[t->reentrancy] = 0;
	}

	res = pthread_cond_timedwait(cond, &t->mutex, abstime);
	if (res && res != ETIMEDOUT) {
		__opbx_mutex_logger("%s line %d (%s): Error waiting on condition mutex '%s'\n", 
				   filename, lineno, func, strerror(res));
#ifdef THREAD_CRASH
		DO_THREAD_CRASH;
#endif
	} else {
		if (t->reentrancy < OPBX_MAX_REENTRANCY) {
			t->file[t->reentrancy] = filename;
			t->lineno[t->reentrancy] = lineno;
			t->func[t->reentrancy] = func;
			t->thread[t->reentrancy] = pthread_self();
			t->reentrancy++;
		} else {
			__opbx_mutex_logger("%s line %d (%s): '%s' really deep reentrancy!\n",
							   filename, lineno, func, mutex_name);
		}
	}

	return res;
}


#define opbx_mutex_init(pmutex) __opbx_pthread_mutex_init(__FILE__, __LINE__, __PRETTY_FUNCTION__, #pmutex, pmutex)
#define opbx_mutex_destroy(a) __opbx_pthread_mutex_destroy(__FILE__, __LINE__, __PRETTY_FUNCTION__, #a, a)
#define opbx_mutex_lock(a) __opbx_pthread_mutex_lock(__FILE__, __LINE__, __PRETTY_FUNCTION__, #a, a)
#define opbx_mutex_unlock(a) __opbx_pthread_mutex_unlock(__FILE__, __LINE__, __PRETTY_FUNCTION__, #a, a)
#define opbx_mutex_trylock(a) __opbx_pthread_mutex_trylock(__FILE__, __LINE__, __PRETTY_FUNCTION__, #a, a)
#define opbx_cond_init(cond, attr) __opbx_cond_init(__FILE__, __LINE__, __PRETTY_FUNCTION__, #cond, cond, attr)
#define opbx_cond_destroy(cond) __opbx_cond_destroy(__FILE__, __LINE__, __PRETTY_FUNCTION__, #cond, cond)
#define opbx_cond_signal(cond) __opbx_cond_signal(__FILE__, __LINE__, __PRETTY_FUNCTION__, #cond, cond)
#define opbx_cond_broadcast(cond) __opbx_cond_broadcast(__FILE__, __LINE__, __PRETTY_FUNCTION__, #cond, cond)
#define opbx_cond_wait(cond, mutex) __opbx_cond_wait(__FILE__, __LINE__, __PRETTY_FUNCTION__, #cond, #mutex, cond, mutex)
#define opbx_cond_timedwait(cond, mutex, time) __opbx_cond_timedwait(__FILE__, __LINE__, __PRETTY_FUNCTION__, #cond, #mutex, cond, mutex, time)

#else /* !DEBUG_THREADS */


#define OPBX_MUTEX_INIT_VALUE	PTHREAD_MUTEX_INIT_VALUE


typedef pthread_mutex_t opbx_mutex_t;

static inline int opbx_mutex_init(opbx_mutex_t *pmutex)
{
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, OPBX_MUTEX_KIND);
	return pthread_mutex_init(pmutex, &attr);
}

#define opbx_pthread_mutex_init(pmutex,a) pthread_mutex_init(pmutex,a)

static inline int opbx_mutex_unlock(opbx_mutex_t *pmutex)
{
	return pthread_mutex_unlock(pmutex);
}

static inline int opbx_mutex_destroy(opbx_mutex_t *pmutex)
{
	return pthread_mutex_destroy(pmutex);
}

#if defined(OPBX_MUTEX_INIT_W_CONSTRUCTORS)
/* if OPBX_MUTEX_INIT_W_CONSTRUCTORS is defined, use file scope
 constrictors/destructors to create/destroy mutexes.  */ 
#define __OPBX_MUTEX_DEFINE(scope,mutex) \
	scope opbx_mutex_t mutex = OPBX_MUTEX_INIT_VALUE; \
static void  __attribute__ ((constructor)) init_##mutex(void) \
{ \
	opbx_mutex_init(&mutex); \
} \
static void  __attribute__ ((destructor)) fini_##mutex(void) \
{ \
	opbx_mutex_destroy(&mutex); \
}

static inline int opbx_mutex_lock(opbx_mutex_t *pmutex)
{
	return pthread_mutex_lock(pmutex);
}

static inline int opbx_mutex_trylock(opbx_mutex_t *pmutex)
{
	return pthread_mutex_trylock(pmutex);
}

#elif defined(OPBX_MUTEX_INIT_ON_FIRST_USE)
/* if OPBX_MUTEX_INIT_ON_FIRST_USE is defined, mutexes are created on
 first use.  The performance impact on FreeBSD should be small since
 the pthreads library does this itself to initialize errror checking
 (defaulty type) mutexes.*/ 
#define __OPBX_MUTEX_DEFINE(scope,mutex) \
	scope opbx_mutex_t mutex = OPBX_MUTEX_INIT_VALUE

static inline int opbx_mutex_lock(opbx_mutex_t *pmutex)
{
	if (*pmutex == (opbx_mutex_t)OPBX_MUTEX_KIND)
		opbx_mutex_init(pmutex);
	return pthread_mutex_lock(pmutex);
}
static inline int opbx_mutex_trylock(opbx_mutex_t *pmutex)
{
	if (*pmutex == (opbx_mutex_t)OPBX_MUTEX_KIND)
		opbx_mutex_init(pmutex);
	return pthread_mutex_trylock(pmutex);
}
#else
/* By default, use static initialization of mutexes.*/ 
#define __OPBX_MUTEX_DEFINE(scope,mutex) \
	scope opbx_mutex_t mutex = OPBX_MUTEX_INIT_VALUE

static inline int opbx_mutex_lock(opbx_mutex_t *pmutex)
{
	return pthread_mutex_lock(pmutex);
}

static inline int opbx_mutex_trylock(opbx_mutex_t *pmutex)
{
	return pthread_mutex_trylock(pmutex);
}

#endif /* OPBX_MUTEX_INIT_W_CONSTRUCTORS */

typedef pthread_cond_t opbx_cond_t;

static inline int opbx_cond_init(opbx_cond_t *cond, pthread_condattr_t *cond_attr)
{
	return pthread_cond_init(cond, cond_attr);
}

static inline int opbx_cond_signal(opbx_cond_t *cond)
{
	return pthread_cond_signal(cond);
}

static inline int opbx_cond_broadcast(opbx_cond_t *cond)
{
	return pthread_cond_broadcast(cond);
}

static inline int opbx_cond_destroy(opbx_cond_t *cond)
{
	return pthread_cond_destroy(cond);
}

static inline int opbx_cond_wait(opbx_cond_t *cond, opbx_mutex_t *t)
{
	return pthread_cond_wait(cond, t);
}

static inline int opbx_cond_timedwait(opbx_cond_t *cond, opbx_mutex_t *t, const struct timespec *abstime)
{
	return pthread_cond_timedwait(cond, t, abstime);
}

#endif /* !DEBUG_THREADS */

//Maybe it's needed even by other OSs
#if defined(__NetBSD__)

#ifdef pthread_mutex_t
#undef pthread_mutex_t
#endif
#ifdef pthread_mutex_lock
#undef pthread_mutex_lock
#endif
#ifdef pthread_mutex_unlock
#undef pthread_mutex_unlock
#endif
#ifdef pthread_mutex_trylock
#undef pthread_mutex_trylock
#endif
#ifdef pthread_mutex_init
#undef pthread_mutex_init
#endif
#ifdef pthread_mutex_destroy
#undef pthread_mutex_destroy
#endif

#ifdef pthread_cond_t
#undef pthread_cond_t
#endif
#ifdef pthread_cond_init
#undef pthread_cond_init
#endif
#ifdef pthread_cond_destroy
#undef pthread_cond_destroy
#endif
#ifdef pthread_cond_signal
#undef pthread_cond_signal
#endif
#ifdef pthread_cond_broadcast
#undef pthread_cond_broadcast
#endif
#ifdef pthread_cond_wait
#undef pthread_cond_wait
#endif
#ifdef pthread_cond_timedwait
#undef pthread_cond_timedwait
#endif

#endif


#define pthread_mutex_t use_opbx_mutex_t_instead_of_pthread_mutex_t
#define pthread_mutex_lock use_opbx_mutex_lock_instead_of_pthread_mutex_lock
#define pthread_mutex_unlock use_opbx_mutex_unlock_instead_of_pthread_mutex_unlock
#define pthread_mutex_trylock use_opbx_mutex_trylock_instead_of_pthread_mutex_trylock
#define pthread_mutex_init use_opbx_mutex_init_instead_of_pthread_mutex_init
#define pthread_mutex_destroy use_opbx_mutex_destroy_instead_of_pthread_mutex_destroy
#define pthread_cond_t use_opbx_cond_t_instead_of_pthread_cond_t
#define pthread_cond_init use_opbx_cond_init_instead_of_pthread_cond_init
#define pthread_cond_destroy use_opbx_cond_destroy_instead_of_pthread_cond_destroy
#define pthread_cond_signal use_opbx_cond_signal_instead_of_pthread_cond_signal
#define pthread_cond_broadcast use_opbx_cond_broadcopbx_instead_of_pthread_cond_broadcast
#define pthread_cond_wait use_opbx_cond_wait_instead_of_pthread_cond_wait
#define pthread_cond_timedwait use_opbx_cond_wait_instead_of_pthread_cond_timedwait

#define OPBX_MUTEX_DEFINE_STATIC(mutex) __OPBX_MUTEX_DEFINE(static,mutex)
#define OPBX_MUTEX_DEFINE_EXPORTED(mutex) __OPBX_MUTEX_DEFINE(/**/,mutex)

#define OPBX_MUTEX_INITIALIZER __use_OPBX_MUTEX_DEFINE_STATIC_rather_than_OPBX_MUTEX_INITIALIZER__

#define gethostbyname __gethostbyname__is__not__reentrant__use__opbx_gethostbyname__instead__
#ifndef __linux__
#define pthread_create __use_opbx_pthread_create_instead__
#endif

#endif /* _OPENPBX_LOCK_H */
