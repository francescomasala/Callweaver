/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Matthew Fredrickson <creslin@digium.com>
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
 * \brief Trivial application to record a sound file
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
#include "openpbx/dsp.h"
#include "openpbx/utils.h"
#include "openpbx/app.h"

static char *tdesc = "Trivial Record Application";

static char *app = "Record";

static char *synopsis = "Record to a file";

static char *descrip = 
"  Record(filename.format|silence[|maxduration][|options])\n\n"
"Records from the channel into a given filename. If the file exists it will\n"
"be overwritten.\n"
"- 'format' is the format of the file type to be recorded (wav, gsm, etc).\n"
"- 'silence' is the number of seconds of silence to allow before returning.\n"
"- 'maxduration' is the maximum recording duration in seconds. If missing\n"
"or 0 there is no maximum.\n"
"- 'options' may contain any of the following letters:\n"
"     's' : skip recording if the line is not yet answered\n"
"     'n' : do not answer, but record anyway if line not yet answered\n"
"     'a' : append to existing recording rather than replacing\n"
"     't' : use alternate '*' terminator key instead of default '#'\n"
"     'q' : quiet (do not play a beep tone)\n"
"\n"
"If filename contains '%d', these characters will be replaced with a number\n"
"incremented by one each time the file is recorded. \n\n"
"Use 'show file formats' to see the available formats on your system\n\n"
"User can press '#' to terminate the recording and continue to the next priority.\n\n"
"Returns -1 when the user hangs up.\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int record_exec(struct opbx_channel *chan, void *data)
{
	int res = 0;
	int count = 0;
	int percentflag = 0;
	char *filename, *ext = NULL, *silstr, *maxstr, *options;
	char *vdata, *p;
	int i = 0;
	char tmp[256];

	struct opbx_filestream *s = '\0';
	struct localuser *u;
	struct opbx_frame *f = NULL;
	
	struct opbx_dsp *sildet = NULL;   	/* silence detector dsp */
	int totalsilence = 0;
	int dspsilence = 0;
	int silence = 0;		/* amount of silence to allow */
	int gotsilence = 0;		/* did we timeout for silence? */
	int maxduration = 0;		/* max duration of recording in milliseconds */
	int gottimeout = 0;		/* did we timeout for maxduration exceeded? */
	int option_skip = 0;
	int option_noanswer = 0;
	int option_append = 0;
	int terminator = '#';
	int option_quiet = 0;
	int rfmt = 0;
	int flags;
	
	/* The next few lines of code parse out the filename and header from the input string */
	if (opbx_strlen_zero(data)) { /* no data implies no filename or anything is present */
		opbx_log(LOG_WARNING, "Record requires an argument (filename)\n");
		return -1;
	}

	LOCAL_USER_ADD(u);

	/* Yay for strsep being easy */
	vdata = opbx_strdupa(data);
	if (!vdata) {
		opbx_log(LOG_ERROR, "Out of memory\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	p = vdata;
	filename = strsep(&p, "|");
	silstr = strsep(&p, "|");
	maxstr = strsep(&p, "|");	
	options = strsep(&p, "|");
	
	if (filename) {
		if (strstr(filename, "%d"))
			percentflag = 1;
		ext = strrchr(filename, '.'); /* to support filename with a . in the filename, not format */
		if (!ext)
			ext = strchr(filename, ':');
		if (ext) {
			*ext = '\0';
			ext++;
		}
	}
	if (!ext) {
		opbx_log(LOG_WARNING, "No extension specified to filename!\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}
	if (silstr) {
		if ((sscanf(silstr, "%d", &i) == 1) && (i > -1)) {
			silence = i * 1000;
		} else if (!opbx_strlen_zero(silstr)) {
			opbx_log(LOG_WARNING, "'%s' is not a valid silence duration\n", silstr);
		}
	}
	
	if (maxstr) {
		if ((sscanf(maxstr, "%d", &i) == 1) && (i > -1))
			/* Convert duration to milliseconds */
			maxduration = i * 1000;
		else if (!opbx_strlen_zero(maxstr))
			opbx_log(LOG_WARNING, "'%s' is not a valid maximum duration\n", maxstr);
	}
	if (options) {
		/* Retain backwards compatibility with old style options */
		if (!strcasecmp(options, "skip"))
			option_skip = 1;
		else if (!strcasecmp(options, "noanswer"))
			option_noanswer = 1;
		else {
			if (strchr(options, 's'))
				option_skip = 1;
			if (strchr(options, 'n'))
				option_noanswer = 1;
			if (strchr(options, 'a'))
				option_append = 1;
			if (strchr(options, 't'))
				terminator = '*';
			if (strchr(options, 'q'))
				option_quiet = 1;
		}
	}
	
	/* done parsing */
	
	/* these are to allow the use of the %d in the config file for a wild card of sort to
	  create a new file with the inputed name scheme */
	if (percentflag) {

		OPBX_DECLARE_APP_ARGS(fname, 
		    OPBX_APP_ARG(piece)[100];
		);
		char *tmp2 = opbx_strdupa(filename);
		char countstring[15];
		int i;

		/* Separate each piece out by the format specifier */
		/* OPBX_NONSTANDARD_APP_ARGS(fname, tmp2, '%'); */
		fname.argc = opbx_separate_app_args(tmp2, '%', fname.argv, 
				    (sizeof(fname) - sizeof(fname.argc)) / sizeof(fname.argv[0]));

		do {
//			snprintf(tmp, sizeof(tmp), filename, count);

			int tmplen;
			/* First piece has no leading percent, so it's copied verbatim */
			opbx_copy_string(tmp, fname.piece[0], sizeof(tmp));
			tmplen = strlen(tmp);
			for (i = 1; i < fname.argc; i++) {
				if (fname.piece[i][0] == 'd') {
					/* Substitute the count */
					snprintf(countstring, sizeof(countstring), "%d", count);
					opbx_copy_string(tmp + tmplen, countstring, sizeof(tmp) - tmplen);
					tmplen += strlen(countstring);
				} else if (tmplen + 2 < sizeof(tmp)) {
					/* Unknown format specifier - just copy it verbatim */
					tmp[tmplen++] = '%';
					tmp[tmplen++] = fname.piece[i][0];
				}
				/* Copy the remaining portion of the piece */
				opbx_copy_string(tmp + tmplen, &(fname.piece[i][1]), sizeof(tmp) - tmplen);
			}


			count++;
		} while ( opbx_fileexists(tmp, ext, chan->language) != -1 );
		pbx_builtin_setvar_helper(chan, "RECORDED_FILE", tmp);
	} else
		strncpy(tmp, filename, sizeof(tmp)-1);
	/* end of routine mentioned */
	
	
	
	if (chan->_state != OPBX_STATE_UP) {
		if (option_skip) {
			/* At the user's option, skip if the line is not up */
			LOCAL_USER_REMOVE(u);
			return 0;
		} else if (!option_noanswer) {
			/* Otherwise answer unless we're supposed to record while on-hook */
			res = opbx_answer(chan);
		}
	}
	
	if (!res) {
	
		if (!option_quiet) {
			/* Some code to play a nice little beep to signify the start of the record operation */
			res = opbx_streamfile(chan, "beep", chan->language);
			if (!res) {
				res = opbx_waitstream(chan, "");
			} else {
				opbx_log(LOG_WARNING, "opbx_streamfile failed on %s\n", chan->name);
			}
			opbx_stopstream(chan);
		}
		
		/* The end of beep code.  Now the recording starts */
		
		if (silence > 0) {
			rfmt = chan->readformat;
			res = opbx_set_read_format(chan, OPBX_FORMAT_SLINEAR);
			if (res < 0) {
				opbx_log(LOG_WARNING, "Unable to set to linear mode, giving up\n");
				LOCAL_USER_REMOVE(u);
				return -1;
			}
			sildet = opbx_dsp_new();
			if (!sildet) {
				opbx_log(LOG_WARNING, "Unable to create silence detector :(\n");
				LOCAL_USER_REMOVE(u);
				return -1;
			}
			opbx_dsp_set_threshold(sildet, 256);
		} 
		
		
		flags = option_append ? O_CREAT|O_APPEND|O_WRONLY : O_CREAT|O_TRUNC|O_WRONLY;
		s = opbx_writefile( tmp, ext, NULL, flags , 0, 0644);
		
		if (s) {
			int waitres;

			/* Request a video update */
			opbx_indicate(chan, OPBX_CONTROL_VIDUPDATE);

			if (maxduration <= 0)
				maxduration = -1;
			
			while ((waitres = opbx_waitfor(chan, maxduration)) > -1) {
				if (maxduration > 0) {
					if (waitres == 0) {
						gottimeout = 1;
						break;
					}
					maxduration = waitres;
  				}
				
				f = opbx_read(chan);
				if (!f) {
					res = -1;
					break;
				}
				if (f->frametype == OPBX_FRAME_VOICE) {
					res = opbx_writestream(s, f);
					
					if (res) {
						opbx_log(LOG_WARNING, "Problem writing frame\n");
						break;
					}
					
					if (silence > 0) {
						dspsilence = 0;
						opbx_dsp_silence(sildet, f, &dspsilence);
						if (dspsilence) {
							totalsilence = dspsilence;
						} else {
							totalsilence = 0;
						}
						if (totalsilence > silence) {
							/* Ended happily with silence */
							opbx_frfree(f);
							gotsilence = 1;
							break;
						}
					}
				}
				if (f->frametype == OPBX_FRAME_VIDEO) {
					res = opbx_writestream(s, f);
					
					if (res) {
						opbx_log(LOG_WARNING, "Problem writing frame\n");
						break;
					}
				}
				if ((f->frametype == OPBX_FRAME_DTMF) &&
					(f->subclass == terminator)) {
					opbx_frfree(f);
					break;
				}
				opbx_frfree(f);
			}
			if (!f) {
				opbx_log(LOG_DEBUG, "Got hangup\n");
				res = -1;
			}
			
			if (gotsilence) {
				opbx_stream_rewind(s, silence-1000);
				opbx_truncstream(s);
			} else if (!gottimeout) {
				/* Strip off the last 1/4 second of it */
				opbx_stream_rewind(s, 250);
				opbx_truncstream(s);
			}
			opbx_closestream(s);
		} else			
			opbx_log(LOG_WARNING, "Could not create file %s\n", filename);
	} else
		opbx_log(LOG_WARNING, "Could not answer channel '%s'\n", chan->name);
	
	if ((silence > 0) && rfmt) {
		res = opbx_set_read_format(chan, rfmt);
		if (res)
			opbx_log(LOG_WARNING, "Unable to restore read format on '%s'\n", chan->name);
		if (sildet)
			opbx_dsp_free(sildet);
	}

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
	return opbx_register_application(app, record_exec, synopsis, descrip);
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


