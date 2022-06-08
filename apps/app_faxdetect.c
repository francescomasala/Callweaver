/*
 * Openpbx.org -- A telephony toolkit for Linux.
 *
 * Fax detection application for all channel types. 
 * Copyright (C) 2004-2005, Newman Telecom, Inc. and Newman Ventures, Inc.
 *
 * Justin Newman <jnewman@newmantelecom.com>
 *
 * We would like to thank Newman Ventures, Inc. for funding this
 * Asterisk project.
 * Newman Ventures <info@newmanventures.com>
 *
 * Modified and ported to openpbx.org by
 * Massimo CtRiX Cetra <devel@navynet.it>
 * Thanks to Navynet SRL for funding this project
 *
 * Portions Copyright:
 * Copyright (C) 2001, Linux Support Services, Inc.
 * Copyright (C) 2004, Digium, Inc.
 *
 * Matthew Fredrickson <creslin@linux-support.net>
 * Mark Spencer <markster@digium.com>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */

#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "openpbx.h"

#include <openpbx/lock.h>
#include <openpbx/file.h>
#include <openpbx/logger.h>
#include <openpbx/channel.h>
#include <openpbx/pbx.h>
#include <openpbx/module.h>
#include <openpbx/translate.h>
#include <openpbx/dsp.h>
#include <openpbx/indications.h>
#include <openpbx/utils.h>

static char *tdesc = "Fax detection application";

static char *app = "FaxDetect";

static char *synopsis = "Detects fax sounds on all channel types (IAX and SIP too)";

static char *descrip = 
"  FaxDetect([waitdur[|tonestr[|options[|sildur[|mindur[|maxdur]]]]]]):\n"
"Parameters:\n"
"      waitdur:  Maximum number of seconds to wait (default=4)\n"
"      tonestr:  Indication to be used while detecting (example: ring)\n"
"      options:\n"
"        'n':     Attempt on-hook if unanswered (default=no)\n"
"        'x':     DTMF digits terminate without extension (default=no)\n"
"        'd':     Ignore DTMF digit detection (default=no)\n"
"        'D':     DTMF digit detection and (default=no)\n"
"                 jump do the dialled extension after pressing #\n"
"                 Disables single digit jumps#\n"
"        'f':     Ignore fax detection (default=no)\n"
"        't':     Ignore talk detection (default=no)\n"
"        'j':     To be used in OGI scripts. Does not jump to any extension.\n"
"        sildur:  Silence ms after mindur/maxdur before aborting (default=1000)\n"
"        mindur:  Minimum non-silence ms needed (default=100)\n"
"        maxdur:  Maximum non-silence ms allowed (default=0/forever)\n"
"\n"
"This application listens for fax tones for waitdur seconds of time.\n"
"Audio is only monitored in the receive direction.\n"
"It can play optional ringtone indicated by tonestr. \n"
"It can be interrupted by digits or non-silence (talk or fax tones).\n"
"if d option is specified, a single digit interrupt and must be the \n"
"start of a valid extension.\n"
"If D option is specified (overrides d), the application will wait for\n"
"the user to enter digits terminated by a # and jump to the corresponding\n"
"extension, if it exists.\n"
"If fax is detected (tones or T38 invite), it will jump to the 'fax' extension.\n"
"If a period of non-silence greater than 'mindur' ms, yet less than 'maxdur' ms\n"
"is followed by silence at least 'sildur' ms then the application jumps\n"
"to the 'talk' extension.\n"
"If all undetected, control will continue at the next priority.\n"
"The application, upon exit, will set the folloging channel variables: \n"
"   - DTMF_DETECTED : set to 1 when a DTMF digit would have caused a jump.\n"
"   - TALK_DETECTED : set to 1 when talk has been detected.\n"
"   - FAX_DETECTED  : set to 1 when fax tones detected.\n"
"   - FAXEXTEN      : original dialplan extension of this application\n"
"   - DTMF_DID      : digits dialled beforeexit (#excluded)\n"
"Returns -1 on hangup, and 0 on successful completion with no exit conditions.\n"
"\n";

#define CALLERID_FIELD cid.cid_num

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int detectfax_exec(struct opbx_channel *chan, void *data)
{
	int res = 0;
	struct localuser *u;
	char tmp[256] = "\0";
	char dtmf_did[256] = "\0";
	char *p = NULL;
	char *waitstr = NULL;
	char *tonestr = NULL;
	char *options = NULL;
	char *silstr = NULL;
	char *minstr = NULL;
	char *maxstr = NULL;
	int totalsilence;			// working vars
	int ms_silence=0,ms_talk=0;		// working vars
	struct opbx_frame *fr = NULL;
	struct opbx_frame *fr2 = NULL;
	struct opbx_frame *fr3 = NULL;
	int speaking = 0;
	int talkdetection_started = 0;
	struct timeval start_talk = {0, 0}, start_silence = {0, 0}, now = {0, 0};
	int waitdur = 4;
	int sildur = 1000;
	int mindur = 100;
	int maxdur = -1;
	int skipanswer = 0;
	int noextneeded = 0;
	int ignoredtmf = 0;
	int ignorefax = 0;
	int ignoretalk = 0;
	int ignorejump = 0;
	int longdtmf = 0;
	int x = 0;
	int origrformat = 0;
	int origwformat = 0;
	int features = 0;
	time_t timeout = 0;
	struct opbx_dsp *dsp = NULL;
	
	pbx_builtin_setvar_helper(chan, "FAX_DETECTED", "0");
	pbx_builtin_setvar_helper(chan, "FAXEXTEN", "unknown");
	pbx_builtin_setvar_helper(chan, "DTMF_DETECTED", "0");
	pbx_builtin_setvar_helper(chan, "TALK_DETECTED", "0");
        pbx_builtin_setvar_helper(chan, "DTMF_DID", "");

	if (data || !opbx_strlen_zero((char *)data)) {
		strncpy(tmp, (char *)data, sizeof(tmp)-1);
	}	
	
	p = tmp;
	
	waitstr = strsep(&p, "|");
	tonestr = strsep(&p, "|");
	options = strsep(&p, "|");
	silstr = strsep(&p, "|");
	minstr = strsep(&p, "|");	
	maxstr = strsep(&p, "|");	
	
	if (waitstr) {
		if ((sscanf(waitstr, "%d", &x) == 1) && (x > 0))
			waitdur = x;
	}

	if ( (tonestr != NULL) && !strlen(tonestr)) 
	    tonestr=NULL;
	
	if (options) {
		if (strchr(options, 'n'))
			skipanswer = 1;
		if (strchr(options, 'x'))
			noextneeded = 1;
		if (strchr(options, 'd'))
			ignoredtmf = 1;
		if (strchr(options, 'D')) {
			longdtmf = 1;
			ignoredtmf = 0;
		}
		if (strchr(options, 'f'))
			ignorefax = 1;
		if (strchr(options, 't'))
			ignoretalk = 1;
		if (strchr(options, 'j'))
			ignorejump = 1;
	}
	
	if (silstr) {
		if ((sscanf(silstr, "%d", &x) == 1) && (x > 0))
			sildur = x;
	}
	
	if (minstr) {
		if ((sscanf(minstr, "%d", &x) == 1) && (x > 0))
			mindur = x;
	}
	
	if (maxstr) {
		if ((sscanf(maxstr, "%d", &x) == 1) && (x > 0))
			maxdur = x;
	}
	
	opbx_log(LOG_DEBUG, "Preparing detect of fax (waitdur=%dms, sildur=%dms, mindur=%dms, maxdur=%dms)\n", 
						waitdur, sildur, mindur, maxdur);
						
	LOCAL_USER_ADD(u);
	if (chan->_state != OPBX_STATE_UP && !skipanswer) {
		/* Otherwise answer unless we're supposed to send this while on-hook */
		res = opbx_answer(chan);
	}
	if (!res) {
		origrformat = chan->readformat;
		if ((res = opbx_set_read_format(chan, OPBX_FORMAT_SLINEAR))) 
			opbx_log(LOG_WARNING, "Unable to set read format to linear!\n");
		origwformat = chan->writeformat;
		if ((res = opbx_set_write_format(chan, OPBX_FORMAT_SLINEAR))) 
			opbx_log(LOG_WARNING, "Unable to set write format to linear!\n");
	}
	if (!(dsp = opbx_dsp_new())) {
		opbx_log(LOG_WARNING, "Unable to allocate DSP!\n");
		res = -1;
	}
	
	if (dsp) {	
		if (!ignoretalk)
			; /* features |= DSP_FEATURE_SILENCE_SUPPRESS; */
		if (!ignorefax)
			features |= DSP_FEATURE_FAX_DETECT;

		features |= DSP_FEATURE_DTMF_DETECT;
			
		opbx_dsp_set_threshold(dsp, 256); 
		opbx_dsp_set_features(dsp, features | DSP_DIGITMODE_RELAXDTMF);
		opbx_dsp_digitmode(dsp, DSP_DIGITMODE_DTMF);
	}

	if (tonestr) {
	    struct tone_zone_sound *ts;
	    ts = opbx_get_indication_tone(chan->zone, tonestr);
	    if (ts && ts->data[0])
		res = opbx_playtones_start(chan, 0, ts->data, 0);
	    else
		res = opbx_playtones_start(chan, 0, tonestr, 0);
	    if (res)
		opbx_log(LOG_NOTICE,"Unable to start playtones\n");
	}



	if (!res) {
		if (waitdur > 0)
			timeout = time(NULL) + (time_t)waitdur;

		while(opbx_waitfor(chan, -1) > -1) {
			if (waitdur > 0 && time(NULL) > timeout) {
				res = 0;
				break;
			}

			fr = opbx_read(chan);
			if (!fr) {
				opbx_log(LOG_DEBUG, "Got hangup\n");
				res = -1;
				break;
			}

			/* Check for a T38 switchover */
			if (chan->t38mode_enabled==1 && !ignorefax) {
			    opbx_log(LOG_DEBUG, "Fax detected on %s. T38 switchover completed.\n", chan->name);
			    pbx_builtin_setvar_helper(chan, "FAX_DETECTED", "1");
			    pbx_builtin_setvar_helper(chan,"FAXEXTEN",chan->exten);
			    if (!ignorejump) {
				if (strcmp(chan->exten, "fax")) {
				    opbx_log(LOG_NOTICE, "Redirecting %s to fax extension [T38]\n", chan->name);
				    if (opbx_exists_extension(chan, chan->context, "fax", 1, chan->CALLERID_FIELD)) {
					/* Save the DID/DNIS when we transfer the fax call to a "fax" extension */
					strncpy(chan->exten, "fax", sizeof(chan->exten)-1);
					chan->priority = 0;									
				    } else
					opbx_log(LOG_WARNING, "Fax detected, but no fax extension\n");
				} else
			    	    opbx_log(LOG_WARNING, "Already in a fax extension, not redirecting\n");
			    }
			    res = 0;
			    opbx_frfree(fr);
			    break;
			}


			fr2 = opbx_dsp_process(chan, dsp, fr);
			if (!fr2) {
				opbx_log(LOG_WARNING, "Bad DSP received (what happened?)\n");
				fr2 = fr;
			} 

			if (fr2->frametype == OPBX_FRAME_DTMF) {
				if (fr2->subclass == 'f' && !ignorefax) {
					// Fax tone -- Handle and return NULL 
					opbx_log(LOG_DEBUG, "Fax detected on %s\n", chan->name);
					pbx_builtin_setvar_helper(chan, "FAX_DETECTED", "1");
					pbx_builtin_setvar_helper(chan,"FAXEXTEN",chan->exten);
					if (!ignorejump) {
					    if (strcmp(chan->exten, "fax")) {
						opbx_log(LOG_NOTICE, "Redirecting %s to fax extension [DTMF]\n", chan->name);
						    if (opbx_exists_extension(chan, chan->context, "fax", 1, chan->CALLERID_FIELD)) {
							// Save the DID/DNIS when we transfer the fax call to a "fax" extension
							strncpy(chan->exten, "fax", sizeof(chan->exten)-1);
							chan->priority = 0;
						    } else
						    opbx_log(LOG_WARNING, "Fax detected, but no fax extension\n");
					    } else
						opbx_log(LOG_WARNING, "Already in a fax extension, not redirecting\n");
					}
					res = 0;
					opbx_frfree(fr);
					break;
				} else if (!ignoredtmf) {
					char t[2];
					t[0] = fr2->subclass;
					t[1] = '\0';
					opbx_log(LOG_DEBUG, "DTMF detected on %s: %s\n", chan->name,t);
					if (
						( noextneeded || opbx_canmatch_extension(chan, chan->context, t, 1, chan->CALLERID_FIELD) )
						&& !longdtmf
					   ) {
						// They entered a valid extension, or might be anyhow 
						pbx_builtin_setvar_helper(chan, "DTMF_DETECTED", "1");
						if (noextneeded) {
							opbx_log(LOG_NOTICE, "DTMF received (not matching to exten)\n");
							res = 0;
						} else {
							opbx_log(LOG_NOTICE, "DTMF received (matching to exten)\n");
							res = fr2->subclass;
						}
						opbx_frfree(fr);
						break;
					} else {
						if (strcmp(t,"#") || !longdtmf) {
						    strncat(dtmf_did,t,sizeof(dtmf_did)-strlen(dtmf_did)-1 );
						} else {
						    pbx_builtin_setvar_helper(chan, "DTMF_DETECTED", "1");
						    pbx_builtin_setvar_helper(chan, "DTMF_DID", dtmf_did);
						    if (!ignorejump && opbx_canmatch_extension(chan, chan->context, dtmf_did, 1, chan->CALLERID_FIELD) ) {
							strncpy(chan->exten, dtmf_did, sizeof(chan->exten)-1);
							chan->priority = 0;
						    }
						    res=0;
						    opbx_frfree(fr);
						    break;
						}
						opbx_log(LOG_DEBUG, "Valid extension requested and DTMF did not match [%s]\n",t);
					}
				}
			} else if ((fr->frametype == OPBX_FRAME_VOICE) && (fr->subclass == OPBX_FORMAT_SLINEAR) && !ignoretalk) {

				// Let's do echo
    				if (!tonestr) {
    				    //The following piece of code enables this application
				    //to send empty frames. This solves fax detection problem
				    //When a fax gets in with RTP. 
				    //The CNG is detected, faxdetect gotos to fax extension
				    //and if we are on a SIP channel the T38 switchover is done.
    				    fr3=opbx_frdup(fr);
				    memset(fr3->data,0,fr3->datalen);
				    opbx_write(chan,fr3);
				    opbx_frfree(fr3);
				}


				res = opbx_dsp_silence(dsp, fr, &totalsilence);

				if (res) { // There is silence on the line.
				    gettimeofday(&now, NULL);
				    if (talkdetection_started && speaking) { // 1st iteration
					ms_talk =  (now.tv_sec  - start_talk.tv_sec ) * 1000;
					ms_talk += (now.tv_usec - start_talk.tv_usec) / 1000;

				    }
				    if (speaking)
					gettimeofday(&start_silence, NULL);

				    ms_silence =  (now.tv_sec  - start_silence.tv_sec ) * 1000;
				    ms_silence += (now.tv_usec - start_silence.tv_usec) / 1000;

				    //opbx_log(LOG_WARNING,"[%5d,%5d,%5d] MS_TALK: %6d MS_SILENCE %6d\n", 
				    //	mindur,maxdur,sildur, ms_talk, ms_silence);
				    speaking=0;

				    if (ms_silence>=sildur) {
					if ( (ms_talk>=mindur) && ( (maxdur<0) || (ms_talk<maxdur) ) ) {
					    // TALK Has been detected
					    char ms_str[64];
					    snprintf(ms_str, sizeof(ms_str), "%d", ms_talk);
					    pbx_builtin_setvar_helper(chan, "TALK_DETECTED", ms_str);
					    if (!ignorejump) {
					        opbx_log(LOG_NOTICE, "Redirecting %s to talk extension\n", chan->name);
						if (opbx_exists_extension(chan, chan->context, "talk", 1, chan->CALLERID_FIELD)) {
						    strncpy(chan->exten, "talk", sizeof(chan->exten) - 1);
						    chan->priority = 0;
						} else
						    opbx_log(LOG_WARNING, "Talk detected, but no talk extension\n");
					    } else {
						opbx_log(LOG_NOTICE, "Talk detected.\n");
					    }
					    res = 0;
					    opbx_frfree(fr);
					    break;
					}
				    }

				} else {
				    if (!talkdetection_started)
					talkdetection_started=1;
				    if (!speaking) {
					gettimeofday(&start_talk, NULL);
					opbx_log(LOG_DEBUG,"Start of voice token\n");
				    }
				    speaking=1;
				}

			}
			opbx_frfree(fr);
		}
	} else
		opbx_log(LOG_WARNING, "Could not answer channel '%s'\n", chan->name);
	
	if (res > -1) {
		if (origrformat && opbx_set_read_format(chan, origrformat)) {
			opbx_log(LOG_WARNING, "Failed to restore read format for %s to %s\n", 
				chan->name, opbx_getformatname(origrformat));
		}
		if (origwformat && opbx_set_write_format(chan, origwformat)) {
			opbx_log(LOG_WARNING, "Failed to restore write format for %s to %s\n", 
				chan->name, opbx_getformatname(origwformat));
		}
	}
	
	if (dsp)
		opbx_dsp_free(dsp);

	if (tonestr)
	    opbx_playtones_stop(chan);
	
	LOCAL_USER_REMOVE(u);
	
	return res;
}

int unload_module(void)
{
	STANDARD_HANGUP_LOCALUSERS;
	return opbx_unregister_application(app);
}

int load_module(void)
{
	return opbx_register_application(app, detectfax_exec, synopsis, descrip);
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

