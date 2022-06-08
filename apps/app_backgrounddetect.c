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
 * \brief Playback a file with audio detect
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h> 
#include <string.h>
#include <stdlib.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/lock.h"
#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/translate.h"
#include "openpbx/utils.h"
#include "openpbx/dsp.h"

static char *tdesc = "Playback with Talk and Fax Detection";

static char *app = "BackgroundDetect";

static char *synopsis = "Background a file with Talk and Fax detect";

static char *descrip = 
" BackgroundDetect(filename[|options[|sil[|min|[max]]]]): \n"
"Parameters:\n"
"      filename: File to play in the background.\n"
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
"Plays back a given filename, waiting for interruption from a given digit \n"
"(the digit must start the beginning of a valid extension, or it will be ignored).\n"
"If D option is specified (overrides d), the application will wait for\n"
"the user to enter digits terminated by a # and jump to the corresponding\n"
"extension, if it exists.\n"
"During the playback of the file, audio is monitored in the receive\n"
"direction, and if a period of non-silence which is greater than 'min' ms\n"
"yet less than 'max' ms is followed by silence for at least 'sil' ms then\n"
"the audio playback is aborted and processing jumps to the 'talk' extension\n"
"if available.  If unspecified, sil, min, and max default to 1000, 100, and\n"
"infinity respectively.\n"
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

static int background_detect_exec(struct opbx_channel *chan, void *data)
{
	int res = 0;
	struct localuser *u;
	char *tmp;
	char *options;
	char *parms;
	char *stringp;
	struct opbx_frame *fr=NULL, *fr2=NULL;
	int notsilent=0;
	struct timeval start = { 0, 0};
	int sil = 1000;
	int min = 100;
	int max = -1;
	int x;
	int origrformat=0;
	struct opbx_dsp *dsp;
	int skipanswer = 0;
	int ignoredtmf = 0;
	int ignorefax = 0;
	int ignoretalk = 0;
	int ignorejump = 0;
	int features = 0;
	int noextneeded=0;
	int longdtmf = 1;
	char dtmf_did[256] = "\0";

	pbx_builtin_setvar_helper(chan, "FAX_DETECTED", "0");
	pbx_builtin_setvar_helper(chan, "FAXEXTEN", "unknown");
	pbx_builtin_setvar_helper(chan, "DTMF_DETECTED", "0");
	pbx_builtin_setvar_helper(chan, "TALK_DETECTED", "0");
	pbx_builtin_setvar_helper(chan, "DTMF_DID", "");
	
	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "BackgroundDetect requires an argument (filename)\n");
		return -1;
	}

	LOCAL_USER_ADD(u);

	tmp = opbx_strdupa(data);
	if (!tmp) {
		opbx_log(LOG_ERROR, "Out of memory\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}	


	stringp=tmp;
	strsep(&stringp, "|");
	options = strsep(&stringp, "|");
	parms   = strsep(&stringp, "|");

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

	if (parms) {
		if ((sscanf(parms, "%d", &x) == 1) && (x > 0))
			sil = x;
		parms = strsep(&stringp, "|");
		if (parms) {
			if ((sscanf(parms, "%d", &x) == 1) && (x > 0))
				min = x;
			parms = strsep(&stringp, "|");
			if (parms) {
				if ((sscanf(parms, "%d", &x) == 1) && (x > 0))
					max = x;
			}
		}
	}

	opbx_log(LOG_DEBUG, "Preparing detect of '%s', sil=%d,min=%d,max=%d\n", 
						tmp, sil, min, max);
	if (chan->_state != OPBX_STATE_UP && !skipanswer) {
		// Otherwise answer unless we're supposed to send this while on-hook 
		res = opbx_answer(chan);
	}
	if (!res) {
		origrformat = chan->readformat;
		if ((res = opbx_set_read_format(chan, OPBX_FORMAT_SLINEAR))) 
			opbx_log(LOG_WARNING, "Unable to set read format to linear!\n");
	}
	if (!(dsp = opbx_dsp_new())) {
		opbx_log(LOG_WARNING, "Unable to allocate DSP!\n");
		res = -1;
	}

	if (dsp) {
		if (!ignoretalk)
			; // features |= DSP_FEATURE_SILENCE_SUPPRESS; 
		if (!ignorefax)
			features |= DSP_FEATURE_FAX_DETECT;
		//if (!ignoredtmf)
			features |= DSP_FEATURE_DTMF_DETECT;

		opbx_dsp_set_threshold(dsp, 256);
		opbx_dsp_set_features(dsp, features | DSP_DIGITMODE_RELAXDTMF);
		opbx_dsp_digitmode(dsp, DSP_DIGITMODE_DTMF);
	}

	if (!res) {
		opbx_stopstream(chan);
		res = opbx_streamfile(chan, tmp, chan->language);
		if (!res) {
			while(chan->stream) {
				res = opbx_sched_wait(chan->sched);
				if (res < 0) {
					res = 0;
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

				// NOW let's check for incoming RTP audio
				res = opbx_waitfor(chan, res);

				if (res < 0) {
					opbx_log(LOG_WARNING, "Waitfor failed on %s\n", chan->name);
					break;
				} else if (res > 0) {
					fr = opbx_read(chan);
					if (!fr) {
						opbx_log(LOG_DEBUG, "Got hangup\n");
						res = -1;
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
						    if (noextneeded) {
							opbx_log(LOG_NOTICE, "DTMF received (not matching to exten)\n");
							res = 0;
						    } else {
							opbx_log(LOG_NOTICE, "DTMF received (matching to exten)\n");
							res = fr2->subclass;
						    }
						    pbx_builtin_setvar_helper(chan, "DTMF_DETECTED", "1");
						    opbx_frfree(fr);
						    break;
						} else {
						    if (strcmp(t,"#") || !longdtmf) {
							strncat(dtmf_did,t,sizeof(dtmf_did)-strlen(dtmf_did)-1 );
						    } else {
							pbx_builtin_setvar_helper(chan, "DTMF_DID", dtmf_did);
							pbx_builtin_setvar_helper(chan, "DTMF_DETECTED", "1");
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
					} else if ((fr->frametype == OPBX_FRAME_VOICE) && (fr->subclass == OPBX_FORMAT_SLINEAR)) {
						int totalsilence;
						int ms;
						res = opbx_dsp_silence(dsp, fr, &totalsilence);
						if (res && (totalsilence > sil)) {
							/* We've been quiet a little while */
							if (notsilent) {
								/* We had heard some talking */
								ms = opbx_tvdiff_ms(opbx_tvnow(), start);
								ms -= sil;
								if (ms < 0)
									ms = 0;
								if ((ms > min) && ((max < 0) || (ms < max))) {
									char ms_str[10];
									opbx_log(LOG_DEBUG, "Found qualified token of %d ms\n", ms);

									/* Save detected talk time (in milliseconds) */ 
									sprintf(ms_str, "%d", ms );	
									pbx_builtin_setvar_helper(chan, "TALK_DETECTED", ms_str);
									
									opbx_goto_if_exists(chan, chan->context, "talk", 1);
									res = 0;
									opbx_frfree(fr);
									break;
								} else
									opbx_log(LOG_DEBUG, "Found unqualified token of %d ms\n", ms);
								notsilent = 0;
							}
						} else {
							if (!notsilent) {
								/* Heard some audio, mark the begining of the token */
								start = opbx_tvnow();
								opbx_log(LOG_DEBUG, "Start of voice token!\n");
								notsilent = 1;
							}
						}
						
					}
					opbx_frfree(fr);
				}
				opbx_sched_runq(chan->sched);
			}
			opbx_stopstream(chan);
		} else {
			opbx_log(LOG_WARNING, "opbx_streamfile failed on %s for %s\n", chan->name, (char *)data);
			res = 0;
		}
	}
	if (res > -1) {
		if (origrformat && opbx_set_read_format(chan, origrformat)) {
			opbx_log(LOG_WARNING, "Failed to restore read format for %s to %s\n", 
				chan->name, opbx_getformatname(origrformat));
		}
	}
	if (dsp)
		opbx_dsp_free(dsp);
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
	return opbx_register_application(app, background_detect_exec, synopsis, descrip);
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


