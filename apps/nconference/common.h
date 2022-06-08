/*
 * app_nconference
 *
 * NConference
 * A channel independent conference application for Openpbx
 *
 * Copyright (C) 2002, 2003 Navynet SRL
 * http://www.navynet.it
 *
 * Massimo "CtRiX" Cetra - ctrix (at) navynet.it
 *
 * This program may be modified and distributed under the 
 * terms of the GNU Public License V2.
 *
 */

#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#ifndef _NCONFERENCE_COMMON_H
#define NCONFERENCE_COMMON_H

/* openpbx includes */
#include "openpbx.h"
#include "openpbx/lock.h"
#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/config.h"
#include "openpbx/app.h"
#include "openpbx/dsp.h"
#include "openpbx/musiconhold.h"
#include "openpbx/manager.h"
#include "openpbx/options.h"
#include "openpbx/cli.h"
#include "openpbx/say.h"
#include "openpbx/utils.h"
#include "openpbx/translate.h"
#include "openpbx/frame.h"
#include "openpbx/features.h"

/* standard includes */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <pthread.h>


extern opbx_mutex_t conflist_lock;


// debug logging level
#define APP_NCONFERENCE_DEBUG	0

#if (APP_NCONFERENCE_DEBUG == 0)
#define OPBX_CONF_DEBUG 	LOG_DEBUG
#else
#define OPBX_CONF_DEBUG 	LOG_NOTICE
#endif



#define APP_CONFERENCE_NAME     "NConference"
#define APP_CONFERENCE_MANID	"NConference-"

//
// feature defines
//
#define ENABLE_VAD		1
#define ENABLE AUTOGAIN		0	// Not used yet

// sample information for OPBX_FORMAT_SLINEAR format
#define OPBX_CONF_SAMPLE_RATE_8K 	8000
#define OPBX_CONF_SAMPLE_RATE_16K 	16000
#define OPBX_CONF_SAMPLE_RATE 		OPBX_CONF_SAMPLE_RATE_8K

// Time to wait while reading a channel
#define OPBX_CONF_WAITFOR_TIME 40 

// Time to destroy empty conferences (seconds)
#define OPBX_CONF_DESTROY_TIME 300

// -----------------------------------------------
#define OPBX_CONF_SKIP_MS_AFTER_VOICE_DETECTION 	210
#define OPBX_CONF_SKIP_MS_WHEN_SILENT     		90

#define OPBX_CONF_CBUFFER_8K_SIZE 3072


// Timelog functions


#if 1

#define TIMELOG(func,min,message) \
	do { \
		struct timeval t1, t2; \
		int diff; \
		gettimeofday(&t1,NULL); \
		func; \
		gettimeofday(&t2,NULL); \
		if((diff = usecdiff(&t2, &t1)) > min) \
			opbx_log( OPBX_CONF_DEBUG, "TimeLog: %s: %d ms\n", message, diff); \
	} while (0)

#else

#define TIMELOG(func,min,message) func

#endif

#endif // _NCONFERENCE_COMMON_H
