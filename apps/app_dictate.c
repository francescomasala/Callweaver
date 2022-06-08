/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 2005, Anthony Minessale II
 *
 * Anthony Minessale II <anthmct@yahoo.com>
 *
 * Donated by Sangoma Technologies <http://www.samgoma.com>
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
 * \brief Virtual Dictation Machine Application For OpenPBX
 *
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>	/* for mkdir */

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/say.h"
#include "openpbx/lock.h"
#include "openpbx/app.h"

static char *tdesc = "Virtual Dictation Machine";
static char *app = "Dictate";
static char *synopsis = "Virtual Dictation Machine";
static char *desc = "  Dictate([<base_dir>])\n"
"Start dictation machine using optional base dir for files.\n";


STANDARD_LOCAL_USER;
LOCAL_USER_DECL;

typedef enum {
	DFLAG_RECORD = (1 << 0),
	DFLAG_PLAY = (1 << 1),
	DFLAG_TRUNC = (1 << 2),
	DFLAG_PAUSE = (1 << 3),
} dflags;

typedef enum {
	DMODE_INIT,
	DMODE_RECORD,
	DMODE_PLAY
} dmodes;

#define opbx_toggle_flag(it,flag) if(opbx_test_flag(it, flag)) opbx_clear_flag(it, flag); else opbx_set_flag(it, flag)

static int play_and_wait(struct opbx_channel *chan, char *file, char *digits) 
{
	int res = -1;
	if (!opbx_streamfile(chan, file, chan->language)) {
		res = opbx_waitstream(chan, digits);
	}
	return res;
}

static int dictate_exec(struct opbx_channel *chan, void *data)
{
	char *mydata, *argv[2], *path = NULL, filein[256];
	char dftbase[256];
	char *base;
	struct opbx_flags flags = {0};
	struct opbx_filestream *fs;
	struct opbx_frame *f = NULL;
	struct localuser *u;
	int ffactor = 320 * 80,
		res = 0,
		argc = 0,
		done = 0,
		oldr = 0,
		lastop = 0,
		samples = 0,
		speed = 1,
		digit = 0,
		len = 0,
		maxlen = 0,
		mode = 0;
		
	LOCAL_USER_ADD(u);
	
	snprintf(dftbase, sizeof(dftbase), "%s/dictate", opbx_config_OPBX_SPOOL_DIR);
	if (!opbx_strlen_zero(data) && (mydata = opbx_strdupa(data))) {
		argc = opbx_separate_app_args(mydata, '|', argv, sizeof(argv) / sizeof(argv[0]));
	}
	
	if (argc) {
		base = argv[0];
	} else {
		base = dftbase;
	}

	oldr = chan->readformat;
	if ((res = opbx_set_read_format(chan, OPBX_FORMAT_SLINEAR)) < 0) {
		opbx_log(LOG_WARNING, "Unable to set to linear mode.\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	opbx_answer(chan);
	opbx_safe_sleep(chan, 200);
	for(res = 0; !res;) {
		if (opbx_app_getdata(chan, "dictate/enter_filename", filein, sizeof(filein), 0) || 
			opbx_strlen_zero(filein)) {
			res = -1;
			break;
		}
		
		mkdir(base, 0755);
		len = strlen(base) + strlen(filein) + 2;
		if (!path || len > maxlen) {
			path = alloca(len);
			memset(path, 0, len);
			maxlen = len;
		} else {
			memset(path, 0, maxlen);
		}

		snprintf(path, len, "%s/%s", base, filein);
		fs = opbx_writefile(path, "wav", NULL, O_CREAT|O_APPEND, 0, 0700);
		mode = DMODE_PLAY;
		memset(&flags, 0, sizeof(flags));
		opbx_set_flag(&flags, DFLAG_PAUSE);
		digit = play_and_wait(chan, "dictate/forhelp", OPBX_DIGIT_ANY);
		done = 0;
		speed = 1;
		res = 0;
		lastop = 0;
		samples = 0;
		while (!done && ((res = opbx_waitfor(chan, -1)) > -1) && fs && (f = opbx_read(chan))) {
			if (digit) {
				struct opbx_frame fr = {OPBX_FRAME_DTMF, digit};
				opbx_queue_frame(chan, &fr);
				digit = 0;
			}
			if ((f->frametype == OPBX_FRAME_DTMF)) {
				int got = 1;
				switch(mode) {
				case DMODE_PLAY:
					switch(f->subclass) {
					case '1':
						opbx_set_flag(&flags, DFLAG_PAUSE);
						mode = DMODE_RECORD;
						break;
					case '2':
						speed++;
						if (speed > 4) {
							speed = 1;
						}
						res = opbx_say_number(chan, speed, OPBX_DIGIT_ANY, chan->language, (char *) NULL);
						break;
					case '7':
						samples -= ffactor;
						if(samples < 0) {
							samples = 0;
						}
						opbx_seekstream(fs, samples, SEEK_SET);
						break;
					case '8':
						samples += ffactor;
						opbx_seekstream(fs, samples, SEEK_SET);
						break;
						
					default:
						got = 0;
					}
					break;
				case DMODE_RECORD:
					switch(f->subclass) {
					case '1':
						opbx_set_flag(&flags, DFLAG_PAUSE);
						mode = DMODE_PLAY;
						break;
					case '8':
						opbx_toggle_flag(&flags, DFLAG_TRUNC);
						lastop = 0;
						break;
					default:
						got = 0;
					}
					break;
				default:
					got = 0;
				}
				if (!got) {
					switch(f->subclass) {
					case '#':
						done = 1;
						continue;
						break;
					case '*':
						opbx_toggle_flag(&flags, DFLAG_PAUSE);
						if (opbx_test_flag(&flags, DFLAG_PAUSE)) {
							digit = play_and_wait(chan, "dictate/pause", OPBX_DIGIT_ANY);
						} else {
							digit = play_and_wait(chan, mode == DMODE_PLAY ? "dictate/playback" : "dictate/record", OPBX_DIGIT_ANY);
						}
						break;
					case '0':
						opbx_set_flag(&flags, DFLAG_PAUSE);
						digit = play_and_wait(chan, "dictate/paused", OPBX_DIGIT_ANY);
						switch(mode) {
						case DMODE_PLAY:
							digit = play_and_wait(chan, "dictate/play_help", OPBX_DIGIT_ANY);
							break;
						case DMODE_RECORD:
							digit = play_and_wait(chan, "dictate/record_help", OPBX_DIGIT_ANY);
							break;
						}
						if (digit == 0) {
							digit = play_and_wait(chan, "dictate/both_help", OPBX_DIGIT_ANY);
						} else if (digit < 0) {
							done = 1;
							break;
						}
						break;
					}
				}
				
			} else if (f->frametype == OPBX_FRAME_VOICE) {
				switch(mode) {
					struct opbx_frame *fr;
					int x;
				case DMODE_PLAY:
					if (lastop != DMODE_PLAY) {
						if (opbx_test_flag(&flags, DFLAG_PAUSE)) {
							digit = play_and_wait(chan, "dictate/playback_mode", OPBX_DIGIT_ANY);
							if (digit == 0) {
								digit = play_and_wait(chan, "dictate/paused", OPBX_DIGIT_ANY);
							} else if (digit < 0) {
								break;
							}
						}
						if (lastop != DFLAG_PLAY) {
							lastop = DFLAG_PLAY;
							opbx_closestream(fs);
							fs = opbx_openstream(chan, path, chan->language);
							opbx_seekstream(fs, samples, SEEK_SET);
							chan->stream = NULL;
						}
						lastop = DMODE_PLAY;
					}

					if (!opbx_test_flag(&flags, DFLAG_PAUSE)) {
						for (x = 0; x < speed; x++) {
							if ((fr = opbx_readframe(fs))) {
								opbx_write(chan, fr);
								samples += fr->samples;
								opbx_frfree(fr);
								fr = NULL;
							} else {
								samples = 0;
								opbx_seekstream(fs, 0, SEEK_SET);
							}
						}
					}
					break;
				case DMODE_RECORD:
					if (lastop != DMODE_RECORD) {
						int oflags = O_CREAT | O_WRONLY;
						if (opbx_test_flag(&flags, DFLAG_PAUSE)) {						
							digit = play_and_wait(chan, "dictate/record_mode", OPBX_DIGIT_ANY);
							if (digit == 0) {
								digit = play_and_wait(chan, "dictate/paused", OPBX_DIGIT_ANY);
							} else if (digit < 0) {
								break;
							}
						}
						lastop = DMODE_RECORD;
						opbx_closestream(fs);
						if ( opbx_test_flag(&flags, DFLAG_TRUNC)) {
							oflags |= O_TRUNC;
							digit = play_and_wait(chan, "dictate/truncating_audio", OPBX_DIGIT_ANY);
						} else {
							oflags |= O_APPEND;
						}
						fs = opbx_writefile(path, "wav", NULL, oflags, 0, 0700);
						if (opbx_test_flag(&flags, DFLAG_TRUNC)) {
							opbx_seekstream(fs, 0, SEEK_SET);
							opbx_clear_flag(&flags, DFLAG_TRUNC);
						} else {
							opbx_seekstream(fs, 0, SEEK_END);
						}
					}
					if (!opbx_test_flag(&flags, DFLAG_PAUSE)) {
						res = opbx_writestream(fs, f);
					}
					break;
				}
				
			}

			opbx_frfree(f);
		}
	}
	if (oldr) {
		opbx_set_read_format(chan, oldr);
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
	return opbx_register_application(app, dictate_exec, synopsis, desc);
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



