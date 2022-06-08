/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 2005 Anthony Minessale II (anthmct@yahoo.com)
 *
 * Disclaimed to Digium
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
 * \brief muxmon() - record a call natively
 */

#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>


#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/slinfactory.h"
#include "openpbx/app.h"
#include "openpbx/pbx.h"
#include "openpbx/translate.h"
#include "openpbx/module.h"
#include "openpbx/lock.h"
#include "openpbx/cli.h"
#include "openpbx/options.h"

#define get_volfactor(x) x ? ((x > 0) ? (1 << x) : ((1 << abs(x)) * -1)) : 0
#define minmax(x,y) x ? (x > y) ? y : ((x < (y * -1)) ? (y * -1) : x) : 0 

static char *tdesc = "Native Channel Monitoring Module";
static char *app = "MuxMon";
static char *synopsis = "Record A Call Natively";
static char *desc = ""
"  MuxMon(<file>.<ext>[|<options>[|<command>]])\n\n"
"Records The audio on the current channel to the specified file.\n\n"
"Valid Options:\n"
" b    - Only save audio to the file while the channel is bridged. *does not include conferences*\n"
" a    - Append to the file instead of overwriting it.\n"
" v(<x>) - Adjust the heard volume by a factor of <x> -4/4.\n"	
" V(<x>) - Adjust the spoken volume by a factor of <x> -4/4.\n"	
" W(<x>) - Adjust the overall volume by a factor of <x> -4/4.\n\n"	
"<command> will be executed when the recording is over\n"
"Any strings matching ^{X} will be unescaped to ${X} and \n"
"all variables will be evaluated at that time.\n"
"The variable MUXMON_FILENAME will be present as well.\n"
"";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

OPBX_MUTEX_DEFINE_STATIC(modlock);

struct muxmon {
	struct opbx_channel *chan;
	char *filename;
	char *post_process;
	unsigned int flags;
	int readvol;
	int writevol;
};

typedef enum {
    MUXFLAG_RUNNING = (1 << 0),
    MUXFLAG_APPEND = (1 << 1),
    MUXFLAG_BRIDGED = (1 << 2),
    MUXFLAG_VOLUME = (1 << 3),
    MUXFLAG_READVOLUME = (1 << 4),
    MUXFLAG_WRITEVOLUME = (1 << 5)
} muxflags;


OPBX_DECLARE_OPTIONS(muxmon_opts,{
    ['a'] = { MUXFLAG_APPEND },
	['b'] = { MUXFLAG_BRIDGED },
	['v'] = { MUXFLAG_READVOLUME, 1 },
	['V'] = { MUXFLAG_WRITEVOLUME, 2 },
	['W'] = { MUXFLAG_VOLUME, 3 },
});


static void stopmon(struct opbx_channel *chan, struct opbx_channel_spy *spy) 
{
	struct opbx_channel_spy *cptr=NULL, *prev=NULL;
	int count = 0;

	if (chan) {
		if(chan->spiers == NULL)
		   return;
		while(opbx_mutex_trylock(&chan->lock)) {
/*			if (chan->spiers == spy) {
				chan->spiers = NULL;
				return;
			}
*/			
			count++;
			if (count > 10) {
			    opbx_log(LOG_ERROR, "Muxmon - unable to lock channel to stopmon \n");
			    chan->spiers = NULL;
				return;
			}
			sched_yield();
		}
		
		for(cptr=chan->spiers; cptr; cptr=cptr->next) {
			if (cptr == spy) {
				if (prev) {
					prev->next = cptr->next;
					cptr->next = NULL;
				} else
					chan->spiers = cptr->next;
					cptr->next = NULL;
			}
			prev = cptr;
		}

		opbx_mutex_unlock(&chan->lock);
	}
}

static void startmon(struct opbx_channel *chan, struct opbx_channel_spy *spy) 
{

	struct opbx_channel_spy *cptr=NULL;
	struct opbx_channel *peer;

	if (chan) {
		opbx_mutex_lock(&chan->lock);
		if (chan->spiers) {
			for(cptr=chan->spiers;cptr && cptr->next;cptr=cptr->next);
			cptr->next = spy;
		} else {
			chan->spiers = spy;
		}
		opbx_mutex_unlock(&chan->lock);
		
		if ( opbx_test_flag(chan, OPBX_FLAG_NBRIDGE) && (peer = opbx_bridged_channel(chan))) {
			opbx_softhangup(peer, OPBX_SOFTHANGUP_UNBRIDGE);	
		}
	}
}

static int spy_queue_translate(struct opbx_channel_spy *spy,
							   struct opbx_slinfactory *slinfactory0,
							   struct opbx_slinfactory *slinfactory1)
{
	int res = 0;
	struct opbx_frame *f;
	
	opbx_mutex_lock(&spy->lock);
	while((f = spy->queue[0])) {
		spy->queue[0] = f->next;
		opbx_slinfactory_feed(slinfactory0, f);
		opbx_frfree(f);
	}
	opbx_mutex_unlock(&spy->lock);
	opbx_mutex_lock(&spy->lock);
	while((f = spy->queue[1])) {
		spy->queue[1] = f->next;
		opbx_slinfactory_feed(slinfactory1, f);
		opbx_frfree(f);
	}
	opbx_mutex_unlock(&spy->lock);
	return res;
}

static void *muxmon_thread(void *obj) 
{

	int len0 = 0, len1 = 0, samp0 = 0, samp1 = 0, framelen, maxsamp = 0, x = 0;
	short buf0[1280], buf1[1280], buf[1280];
	struct opbx_frame frame;
	struct muxmon *muxmon = obj;
	struct opbx_channel_spy spy;
	struct opbx_filestream *fs = NULL;
	char *ext, *name;
	unsigned int oflags;
	struct opbx_slinfactory slinfactory[2];
	char post_process[1024] = "";
	
	name = opbx_strdupa(muxmon->chan->name);

	framelen = 320;
	frame.frametype = OPBX_FRAME_VOICE;
	frame.subclass = OPBX_FORMAT_SLINEAR;
	frame.data = buf;
	opbx_set_flag(muxmon, MUXFLAG_RUNNING);
	oflags = O_CREAT|O_WRONLY;
	opbx_slinfactory_init(&slinfactory[0]);
	opbx_slinfactory_init(&slinfactory[1]);
	


	/* for efficiency, use a flag to bypass volume logic when it's not needed */
	if (muxmon->readvol || muxmon->writevol) {
		opbx_set_flag(muxmon, MUXFLAG_VOLUME);
	}

	if ((ext = strchr(muxmon->filename, '.'))) {
		*(ext++) = '\0';
	} else {
		ext = "raw";
	}

	memset(&spy, 0, sizeof(spy));
	spy.status = CHANSPY_RUNNING;
	spy.next = NULL;
	opbx_mutex_init(&spy.lock);
	startmon(muxmon->chan, &spy);
	if (opbx_test_flag(muxmon, MUXFLAG_RUNNING)) {
		if (option_verbose > 1) {
			opbx_verbose(VERBOSE_PREFIX_2 "Begin Recording %s\n", name);
		}

		oflags |= opbx_test_flag(muxmon, MUXFLAG_APPEND) ? O_APPEND : O_TRUNC;
		
		if (!(fs = opbx_writefile(muxmon->filename, ext, NULL, oflags, 0, 0644))) {
			opbx_log(LOG_ERROR, "Cannot open %s\n", muxmon->filename);
			spy.status = CHANSPY_DONE;
		}  else {

			if (opbx_test_flag(muxmon, MUXFLAG_APPEND)) {
				opbx_seekstream(fs, 0, SEEK_END);
			}

			while (opbx_test_flag(muxmon, MUXFLAG_RUNNING)) {
				samp0 = samp1 = len0 = len1 = 0;
                                /*In case of hapgup it is safer to start from testing this */  
				if (spy.status != CHANSPY_RUNNING) {
					opbx_clear_flag(muxmon, MUXFLAG_RUNNING);
					break;
				}
				if (opbx_check_hangup(muxmon->chan)) {
					opbx_clear_flag(muxmon, MUXFLAG_RUNNING);
					break;
				}

				if (opbx_test_flag(muxmon, MUXFLAG_BRIDGED) && !opbx_bridged_channel(muxmon->chan)) {
					usleep(1000);
					sched_yield();
					continue;
				}
				
				spy_queue_translate(&spy, &slinfactory[0], &slinfactory[1]);
				
				if (slinfactory[0].size < framelen || slinfactory[1].size < framelen) {
					usleep(1000);
					sched_yield();
					continue;
				}

				if ((len0 = opbx_slinfactory_read(&slinfactory[0], buf0, framelen))) {
					samp0 = len0 / 2;
				}
				if((len1 = opbx_slinfactory_read(&slinfactory[1], buf1, framelen))) {
					samp1 = len1 / 2;
				}
				
				if (opbx_test_flag(muxmon, MUXFLAG_VOLUME)) {
					if (samp0 && muxmon->readvol > 0) {
						for(x=0; x < samp0 / 2; x++) {
							buf0[x] *= muxmon->readvol;
						}
					} else if (samp0 && muxmon->readvol < 0) {
						for(x=0; x < samp0 / 2; x++) {
							buf0[x] /= muxmon->readvol;
						}
					}
					if (samp1 && muxmon->writevol > 0) {
						for(x=0; x < samp1 / 2; x++) {
							buf1[x] *= muxmon->writevol;
						}
					} else if (muxmon->writevol < 0) {
						for(x=0; x < samp1 / 2; x++) {
							buf1[x] /= muxmon->writevol;
						}
					}
				}
				
				maxsamp = (samp0 > samp1) ? samp0 : samp1;

				if (samp0 && samp1) {
					for(x=0; x < maxsamp; x++) {
						if (x < samp0 && x < samp1) {
							buf[x] = buf0[x] + buf1[x];
						} else if (x < samp0) {
							buf[x] = buf0[x];
						} else if (x < samp1) {
							buf[x] = buf1[x];
						}
					}
				} else if(samp0) {
					memcpy(buf, buf0, len0);
					x = samp0;
				} else if(samp1) {
					memcpy(buf, buf1, len1);
					x = samp1;
				}

				frame.samples = x;
				frame.datalen = x * 2;
				opbx_writestream(fs, &frame);
		
				usleep(1000);
				sched_yield();
			}
		}
	}

	if (muxmon->post_process) {
		char *p;
		for(p = muxmon->post_process; *p ; p++) {
			if (*p == '^' && *(p+1) == '{') {
				*p = '$';
			}
		}
		pbx_substitute_variables_helper(muxmon->chan, muxmon->post_process, post_process, sizeof(post_process) - 1);
		free(muxmon->post_process);
		muxmon->post_process = NULL;
	}
/* In case of channel hangup - this is dangerous. Cli stop command do clearing */
	if(spy.status == CHANSPY_RUNNING){
    	 stopmon(muxmon->chan, &spy);  
	}
	if (option_verbose > 1) {
		opbx_verbose(VERBOSE_PREFIX_2 "Finished Recording %s\n", name);
	}
	opbx_mutex_destroy(&spy.lock);
	
	if(fs) {
		opbx_closestream(fs);
	}
	
	opbx_slinfactory_destroy(&slinfactory[0]);
	opbx_slinfactory_destroy(&slinfactory[1]);

	if (muxmon) {
		if (muxmon->filename) {
			free(muxmon->filename);
		}
		free(muxmon);
	}

	if (!opbx_strlen_zero(post_process)) {
		if (option_verbose > 2) {
			opbx_verbose(VERBOSE_PREFIX_2 "Executing [%s]\n", post_process);
		}
		opbx_safe_system(post_process);
	}

	return NULL;
}

static void launch_monitor_thread(struct opbx_channel *chan, char *filename, unsigned int flags, int readvol , int writevol, char *post_process) 
{
	pthread_attr_t attr;
	int result = 0;
	pthread_t thread;
	struct muxmon *muxmon;


	if (!(muxmon = malloc(sizeof(struct muxmon)))) {
		opbx_log(LOG_ERROR, "Memory Error!\n");
		return;
	}

	memset(muxmon, 0, sizeof(struct muxmon));
	muxmon->chan = chan;
	muxmon->filename = strdup(filename);
	if(post_process) {
		muxmon->post_process = strdup(post_process);
	}
	muxmon->readvol = readvol;
	muxmon->writevol = writevol;
	muxmon->flags = flags;

	result = pthread_attr_init(&attr);
	pthread_attr_setschedpolicy(&attr, SCHED_RR);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	result = opbx_pthread_create(&thread, &attr, muxmon_thread, muxmon);
	result = pthread_attr_destroy(&attr);
}


static int muxmon_exec(struct opbx_channel *chan, void *data)
{
	int res = 0, x = 0, readvol = 0, writevol = 0;
	struct localuser *u;
	struct opbx_flags flags = {0};
	int argc;
	char *options = NULL,
		*args,
		*argv[3],
		*filename = NULL,
		*post_process = NULL;
	
	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "muxmon requires an argument\n");
		return -1;
	}

	LOCAL_USER_ADD(u);

	args = opbx_strdupa(data);
	if (!args) {
		opbx_log(LOG_WARNING, "Memory Error!\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}
	
	if ((argc = opbx_separate_app_args(args, '|', argv, sizeof(argv) / sizeof(argv[0])))) {
		filename = argv[0];
		if (argc > 1) {
			options = argv[1];
		}
		if (argc > 2) {
			post_process = argv[2];
		}
	}
	
	if (options) {
		char *opts[3] = {};
		opbx_parseoptions(muxmon_opts, &flags, opts, options);

		if (opbx_test_flag(&flags, MUXFLAG_READVOLUME) && opts[0]) {
			if (sscanf(opts[0], "%d", &x) != 1)
				opbx_log(LOG_NOTICE, "volume must be a number between -4 and 4\n");
			else {
				readvol = minmax(x, 4);
				x = get_volfactor(readvol);
				readvol = minmax(x, 16);
			}
		}
		
		if (opbx_test_flag(&flags, MUXFLAG_WRITEVOLUME) && opts[1]) {
			if (sscanf(opts[1], "%d", &x) != 1)
				opbx_log(LOG_NOTICE, "volume must be a number between -4 and 4\n");
			else {
				writevol = minmax(x, 4);
				x = get_volfactor(writevol);
				writevol = minmax(x, 16);
			}
		}

		if (opbx_test_flag(&flags, MUXFLAG_VOLUME) && opts[2]) {
			if (sscanf(opts[2], "%d", &x) != 1)
				opbx_log(LOG_NOTICE, "volume must be a number between -4 and 4\n");
			else {
				readvol = writevol = minmax(x, 4);
				x = get_volfactor(readvol);
				readvol = minmax(x, 16);
				x = get_volfactor(writevol);
				writevol = minmax(x, 16);
			}
		}
	}
	pbx_builtin_setvar_helper(chan, "MUXMON_FILENAME", filename);

	if (filename) {
		pbx_builtin_setvar_helper(chan, "MUXMON_FILENAME", filename);
		launch_monitor_thread(chan, filename, flags.flags, readvol, writevol, post_process);
	} else {
		opbx_log(LOG_WARNING, "muxmon requires an argument\n");
        return -1;
	}
	LOCAL_USER_REMOVE(u);
	return res;
}


static struct opbx_channel *local_channel_walk(struct opbx_channel *chan) 
{
	struct opbx_channel *ret;
	opbx_mutex_lock(&modlock);	
	if ((ret = opbx_channel_walk_locked(chan))) {
		opbx_mutex_unlock(&ret->lock);
	}
	opbx_mutex_unlock(&modlock);			
	return ret;
}

static struct opbx_channel *local_get_channel_begin_name(char *name) 
{
	struct opbx_channel *chan, *ret = NULL;
	opbx_mutex_lock(&modlock);
	chan = local_channel_walk(NULL);
	while (chan) {
		if (!strncmp(chan->name, name, strlen(name))) {
			ret = chan;
			break;
		}
		chan = local_channel_walk(chan);
	}
	opbx_mutex_unlock(&modlock);
	
	return ret;
}

static int muxmon_cli(int fd, int argc, char **argv) 
{
	char *op, *chan_name = NULL, *args = NULL;
	struct opbx_channel *chan;

	if (argc > 2) {
		op = argv[1];
		chan_name = argv[2];

		if (argv[3]) {
			args = argv[3];
		}

		if (!(chan = local_get_channel_begin_name(chan_name))) {
			opbx_cli(fd, "Invalid Channel!\n");
			return -1;
		}

		if (!strcasecmp(op, "start")) {
			muxmon_exec(chan, args);
		} else if (!strcasecmp(op, "stop")) {
			struct opbx_channel_spy *cptr=NULL;
			int count=0;
			
			while(opbx_mutex_trylock(&chan->lock)) {
				count++;
				if (count > 10) {
					opbx_cli(fd, "Cannot Lock Channel!\n");
					return -1;
				}
				usleep(1000);
				sched_yield();
			}

			for(cptr=chan->spiers; cptr; cptr=cptr->next) {
				cptr->status = CHANSPY_DONE;
			}
			chan->spiers = NULL;
			opbx_mutex_unlock(&chan->lock);
		}
		return 0;
	}

	opbx_cli(fd, "Usage: muxmon <start|stop> <chan_name> <args>\n");
	return -1;
}


static struct opbx_cli_entry cli_muxmon = {
	{ "muxmon", NULL, NULL }, muxmon_cli, 
	"Execute a monitor command", "muxmon <start|stop> <chan_name> <args>"};


int unload_module(void)
{
	STANDARD_HANGUP_LOCALUSERS;
	opbx_cli_unregister(&cli_muxmon);
	return opbx_unregister_application(app);
}

int load_module(void)
{
	opbx_cli_register(&cli_muxmon);
	return opbx_register_application(app, muxmon_exec, synopsis, desc);
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
