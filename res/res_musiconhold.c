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
 * \brief Routines implementing music on hold
 *
 * \arg See also \ref Config_moh
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <sys/signal.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/lock.h"
#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/options.h"
#include "openpbx/module.h"
#include "openpbx/translate.h"
#include "openpbx/say.h"
#include "openpbx/musiconhold.h"
#include "openpbx/config.h"
#include "openpbx/utils.h"
#include "openpbx/cli.h"

#define MAX_MOHFILES 512
#define MAX_MOHFILE_LEN 128

static char *app0 = "MusicOnHold";
static char *app1 = "WaitMusicOnHold";
static char *app2 = "SetMusicOnHold";
static char *app3 = "StartMusicOnHold";
static char *app4 = "StopMusicOnHold";

static char *synopsis0 = "Play Music On Hold indefinitely";
static char *synopsis1 = "Wait, playing Music On Hold";
static char *synopsis2 = "Set default Music On Hold class";
static char *synopsis3 = "Play Music On Hold";
static char *synopsis4 = "Stop Playing Music On Hold";

static char *descrip0 = "MusicOnHold(class): "
"Plays hold music specified by class.  If omitted, the default\n"
"music source for the channel will be used. Set the default \n"
"class with the SetMusicOnHold() application.\n"
"Returns -1 on hangup.\n"
"Never returns otherwise.\n";

static char *descrip1 = "WaitMusicOnHold(delay): "
"Plays hold music specified number of seconds.  Returns 0 when\n"
"done, or -1 on hangup.  If no hold music is available, the delay will\n"
"still occur with no sound.\n";

static char *descrip2 = "SetMusicOnHold(class): "
"Sets the default class for music on hold for a given channel.  When\n"
"music on hold is activated, this class will be used to select which\n"
"music is played.\n";

static char *descrip3 = "StartMusicOnHold(class): "
"Starts playing music on hold, uses default music class for channel.\n"
"Starts playing music specified by class.  If omitted, the default\n"
"music source for the channel will be used.  Always returns 0.\n";

static char *descrip4 = "StopMusicOnHold: "
"Stops playing music on hold.\n";

static int respawn_time = 20;

struct moh_files_state {
	struct mohclass *class;
	int origwfmt;
	int samples;
	int sample_queue;
	unsigned char pos;
	unsigned char save_pos;
};

#define MOH_CUSTOM		(1 << 0)
#define MOH_RANDOMIZE		(1 << 1)

struct mohclass {
	char name[MAX_MUSICCLASS];
	char dir[256];
	char args[256];
	char mode[80];
	char filearray[MAX_MOHFILES][MAX_MOHFILE_LEN];
	unsigned int flags;
	int total_files;
	int format;
	int pid;		/* PID of custom command */
	time_t start;
	pthread_t thread;
	struct mohdata *members;
	/* Source of audio */
	int srcfd;
	struct mohclass *next;
};

struct mohdata {
	int pipe[2];
	int origwfmt;
	struct mohclass *parent;
	struct mohdata *next;
};

static struct mohclass *mohclasses;

OPBX_MUTEX_DEFINE_STATIC(moh_lock);

static void opbx_moh_free_class(struct mohclass **class) 
{
	struct mohdata *members, *mtmp;
	
	members = (*class)->members;
	while(members) {
		mtmp = members;
		members = members->next;
		free(mtmp);
	}
	free(*class);
	*class = NULL;
}


static void moh_files_release(struct opbx_channel *chan, void *data)
{
	struct moh_files_state *state = chan->music_state;

	if (chan && state) {
		if (option_verbose > 2)
			opbx_verbose(VERBOSE_PREFIX_3 "Stopped music on hold on %s\n", chan->name);

		if (state->origwfmt && opbx_set_write_format(chan, state->origwfmt)) {
			opbx_log(LOG_WARNING, "Unable to restore channel '%s' to format '%d'\n", chan->name, state->origwfmt);
		}
		state->save_pos = state->pos + 1;
	}
}


static int opbx_moh_files_next(struct opbx_channel *chan) 
{
	struct moh_files_state *state = chan->music_state;
	int tries;

	if (state->save_pos) {
		state->pos = state->save_pos - 1;
		state->save_pos = 0;
	} else {
		/* Try 20 times to find something good */
		for (tries=0;tries < 20;tries++) {
			state->samples = 0;
			if (chan->stream) {
				opbx_closestream(chan->stream);
				chan->stream = NULL;
				state->pos++;
			}

			if (opbx_test_flag(state->class, MOH_RANDOMIZE))
				state->pos = rand();

			state->pos %= state->class->total_files;

			/* check to see if this file's format can be opened */
			if (opbx_fileexists(state->class->filearray[state->pos], NULL, NULL) > 0)
				break;

		}
	}

	state->pos = state->pos % state->class->total_files;
/* Check it	
	if (opbx_set_write_format(chan, OPBX_FORMAT_SLINEAR)) {
		opbx_log(LOG_WARNING, "Unable to set '%s' to linear format (write)\n", chan->name);
		return -1;
	}
*/
	if (!opbx_openstream_full(chan, state->class->filearray[state->pos], chan->language, 1)) {
		opbx_log(LOG_WARNING, "Unable to open file '%s': %s\n", state->class->filearray[state->pos], strerror(errno));
		state->pos++;
		return -1;
	}

	if (option_debug)
		opbx_log(LOG_DEBUG, "%s Opened file %d '%s'\n", chan->name, state->pos, state->class->filearray[state->pos]);

	if (state->samples)
		opbx_seekstream(chan->stream, state->samples, SEEK_SET);

	return 0;
}


static struct opbx_frame *moh_files_readframe(struct opbx_channel *chan) 
{
	struct opbx_frame *f = NULL;
	
	if (!opbx_test_flag(chan, OPBX_FLAG_ZOMBIE)) {
		if (!(chan->stream && (f = opbx_readframe(chan->stream)))) {
			if (!opbx_moh_files_next(chan))
    				f = opbx_readframe(chan->stream);
               	}
        }

	return f;
}

static int moh_files_generator(struct opbx_channel *chan, void *data, int samples)
{
	struct moh_files_state *state = chan->music_state;
	struct opbx_frame *f = NULL;
	int res = 0;

	state->sample_queue += samples;

	while (state->sample_queue > 0) {
		if ((f = moh_files_readframe(chan))) {
			state->samples += f->samples;
			res = opbx_write(chan, f);
			state->sample_queue -= f->samples;
			opbx_frfree(f);
			if (res < 0) {
				return -1;
			}
		} else
			return -1;	
	}
	return res;
}


static void *moh_files_alloc(struct opbx_channel *chan, void *params)
{
	struct moh_files_state *state;
	struct mohclass *class = params;
	int allocated = 0;

	if (!chan->music_state && (state = malloc(sizeof(struct moh_files_state)))) {
		chan->music_state = state;
		allocated = 1;
	} else 
		state = chan->music_state;

	if (state) {
		if (allocated || state->class != class) {
			/* initialize */
			memset(state, 0, sizeof(struct moh_files_state));
			state->class = class;
		}

		state->origwfmt = chan->writeformat;
/*
		if (opbx_set_write_format(chan, OPBX_FORMAT_SLINEAR)) {
			opbx_log(LOG_WARNING, "Unable to set '%s' to linear format (write)\n", chan->name);
			free(chan->music_state);
			chan->music_state = NULL;
		} else {
			if (option_verbose > 2)
				opbx_verbose(VERBOSE_PREFIX_3 "Started music on hold, class '%s', on %s\n", class->name, chan->name);
		}
*/
		if (option_verbose > 2)
			opbx_verbose(VERBOSE_PREFIX_3 "Started music on hold, class '%s', on %s\n", class->name, chan->name);

	}
	
	return chan->music_state;
}

static struct opbx_generator moh_file_stream = 
{
	alloc: moh_files_alloc,
	release: moh_files_release,
	generate: moh_files_generator,
};

static int spawn_custom_command(struct mohclass *class)
{
	int fds[2];
	int files = 0;
	char fns[MAX_MOHFILES][MAX_MOHFILE_LEN];
	char *argv[MAX_MOHFILES + 50];
	char xargs[256];
	char *argptr;
	int argc = 0;
	DIR *dir = NULL;
	struct dirent *de;

	
	if (!strcasecmp(class->dir, "nodir")) {
		files = 1;
	} else {
		dir = opendir(class->dir);
		if (!dir) {
			opbx_log(LOG_WARNING, "%s is not a valid directory\n", class->dir);
			return -1;
		}
	}

	/* Format arguments for argv vector */
	strncpy(xargs, class->args, sizeof(xargs) - 1);
	argptr = xargs;
	while (argptr && !opbx_strlen_zero(argptr)) {
		argv[argc++] = argptr;
		argptr = strchr(argptr, ' ');
		if (argptr) {
			*argptr = '\0';
			argptr++;
		}
	}

	if (dir) {
		while ((de = readdir(dir)) && (files < MAX_MOHFILES)) {
			if (de->d_name[0] != '.') {
				strncpy(fns[files], de->d_name, sizeof(fns[files]) - 1);
				argv[argc++] = fns[files];
				files++;
			}
		}
	}
	argv[argc] = NULL;
	if (dir) {
		closedir(dir);
	}
	if (!files) {
		opbx_log(LOG_WARNING, "Found no files in '%s'\n", class->dir);
		return -1;
	}
	if (pipe(fds)) {	
		opbx_log(LOG_WARNING, "Pipe failed\n");
		return -1;
	}
#if 0
	printf("%d files total, %d args total\n", files, argc);
	{
		int x;
		for (x=0;argv[x];x++)
			printf("arg%d: %s\n", x, argv[x]);
	}
#endif	
	if (time(NULL) - class->start < respawn_time) {
		sleep(respawn_time - (time(NULL) - class->start));
	}
	time(&class->start);
	class->pid = fork();
	if (class->pid < 0) {
		close(fds[0]);
		close(fds[1]);
		opbx_log(LOG_WARNING, "Fork failed: %s\n", strerror(errno));
		return -1;
	}
	if (!class->pid) {
		int x;
		close(fds[0]);
		/* Stdout goes to pipe */
		dup2(fds[1], STDOUT_FILENO);
		/* Close unused file descriptors */
		for (x=3;x<8192;x++) {
			if (-1 != fcntl(x, F_GETFL)) {
				close(x);
			}
		}
		/* Child */
		chdir(class->dir);
		execv(argv[0], argv);
		opbx_log(LOG_WARNING, "Exec failed: %s\n", strerror(errno));
		close(fds[1]);
		exit(1);
	} else {
		/* Parent */
		close(fds[1]);
	}
	return fds[0];
}

static void *monitor_custom_command(void *data)
{
#define	MOH_MS_INTERVAL		100

	struct mohclass *class = data;
	struct mohdata *moh;
	short sbuf[8192];
	int res, res2;
	int len;
	struct timeval tv, tv_tmp;
	long delta;

	tv.tv_sec = 0;
	tv.tv_usec = 0;
	for(;/* ever */;) {
		/* Spawn custom command if it's not there */
		if (class->srcfd < 0) {
			if ((class->srcfd = spawn_custom_command(class)) < 0) {
				opbx_log(LOG_WARNING, "Unable to spawn custom command\n");
				/* Try again later */
				sleep(500);
			}
		}

		/* Reliable sleep */
		tv_tmp = opbx_tvnow();
		if (opbx_tvzero(tv))
			tv = tv_tmp;
		delta = opbx_tvdiff_ms(tv_tmp, tv);
		if (delta < MOH_MS_INTERVAL) {	/* too early */
			tv = opbx_tvadd(tv, opbx_samp2tv(MOH_MS_INTERVAL, 1000));	/* next deadline */
			usleep(1000 * (MOH_MS_INTERVAL - delta));
		} else {
			opbx_log(LOG_NOTICE, "Request to schedule in the past?!?!\n");
			tv = tv_tmp;
		}
		res = 8 * MOH_MS_INTERVAL;	/* 8 samples per millisecond */
	
		if (!class->members)
			continue;
		/* Read audio */
		len = opbx_codec_get_len(class->format, res);
		
		if ((res2 = read(class->srcfd, sbuf, len)) != len) {
			if (!res2) {
				close(class->srcfd);
				class->srcfd = -1;
				if (class->pid) {
					kill(class->pid, SIGKILL);
					class->pid = 0;
				}
			} else
				opbx_log(LOG_DEBUG, "Read %d bytes of audio while expecting %d\n", res2, len);
			continue;
		}
		opbx_mutex_lock(&moh_lock);
		moh = class->members;
		while (moh) {
			/* Write data */
			if ((res = write(moh->pipe[1], sbuf, res2)) != res2) 
				if (option_debug)
					opbx_log(LOG_DEBUG, "Only wrote %d of %d bytes to pipe\n", res, res2);
			moh = moh->next;
		}
		opbx_mutex_unlock(&moh_lock);
	}
	return NULL;
}

static int moh0_exec(struct opbx_channel *chan, void *data)
{
	if (opbx_moh_start(chan, data)) {
		opbx_log(LOG_WARNING, "Unable to start music on hold (class '%s') on channel %s\n", (char *)data, chan->name);
		return -1;
	}
	while (!opbx_safe_sleep(chan, 10000));
	opbx_moh_stop(chan);
	return -1;
}

static int moh1_exec(struct opbx_channel *chan, void *data)
{
	int res;
	if (!data || !atoi(data)) {
		opbx_log(LOG_WARNING, "WaitMusicOnHold requires an argument (number of seconds to wait)\n");
		return -1;
	}
	if (opbx_moh_start(chan, NULL)) {
		opbx_log(LOG_WARNING, "Unable to start music on hold for %d seconds on channel %s\n", atoi(data), chan->name);
		return -1;
	}
	res = opbx_safe_sleep(chan, atoi(data) * 1000);
	opbx_moh_stop(chan);
	return res;
}

static int moh2_exec(struct opbx_channel *chan, void *data)
{
	if (!data || opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "SetMusicOnHold requires an argument (class)\n");
		return -1;
	}
	strncpy(chan->musicclass, data, sizeof(chan->musicclass) - 1);
	return 0;
}

static int moh3_exec(struct opbx_channel *chan, void *data)
{
	char *class = NULL;
	if (data && strlen(data))
		class = data;
	if (opbx_moh_start(chan, class)) 
		opbx_log(LOG_NOTICE, "Unable to start music on hold class '%s' on channel %s\n", class ? class : "default", chan->name);

	return 0;
}

static int moh4_exec(struct opbx_channel *chan, void *data)
{
	opbx_moh_stop(chan);

	return 0;
}

static struct mohclass *get_mohbyname(char *name)
{
	struct mohclass *moh;
	moh = mohclasses;
	while (moh) {
		if (!strcasecmp(name, moh->name))
			return moh;
		moh = moh->next;
	}
	return NULL;
}

static struct mohdata *mohalloc(struct mohclass *cl)
{
	struct mohdata *moh;
	long flags;
	moh = malloc(sizeof(struct mohdata));
	if (!moh)
		return NULL;
	memset(moh, 0, sizeof(struct mohdata));
	if (pipe(moh->pipe)) {
		opbx_log(LOG_WARNING, "Failed to create pipe: %s\n", strerror(errno));
		free(moh);
		return NULL;
	}
	/* Make entirely non-blocking */
	flags = fcntl(moh->pipe[0], F_GETFL);
	fcntl(moh->pipe[0], F_SETFL, flags | O_NONBLOCK);
	flags = fcntl(moh->pipe[1], F_GETFL);
	fcntl(moh->pipe[1], F_SETFL, flags | O_NONBLOCK);
	moh->parent = cl;
	moh->next = cl->members;
	cl->members = moh;
	return moh;
}

static void moh_release(struct opbx_channel *chan, void *data)
{
	struct mohdata *moh = data, *prev, *cur;
	int oldwfmt;
	opbx_mutex_lock(&moh_lock);
	/* Unlink */
	prev = NULL;
	cur = moh->parent->members;
	while (cur) {
		if (cur == moh) {
			if (prev)
				prev->next = cur->next;
			else
				moh->parent->members = cur->next;
			break;
		}
		prev = cur;
		cur = cur->next;
	}
	opbx_mutex_unlock(&moh_lock);
	close(moh->pipe[0]);
	close(moh->pipe[1]);
	oldwfmt = moh->origwfmt;
	free(moh);
	if (chan) {
		if (oldwfmt && opbx_set_write_format(chan, oldwfmt)) 
			opbx_log(LOG_WARNING, "Unable to restore channel '%s' to format %s\n", chan->name, opbx_getformatname(oldwfmt));
		if (option_verbose > 2)
			opbx_verbose(VERBOSE_PREFIX_3 "Stopped music on hold on %s\n", chan->name);
	}
}

static void *moh_alloc(struct opbx_channel *chan, void *params)
{
	struct mohdata *res;
	struct mohclass *class = params;

	res = mohalloc(class);
	if (res) {
		res->origwfmt = chan->writeformat;
		if (opbx_set_write_format(chan, class->format)) {
			opbx_log(LOG_WARNING, "Unable to set channel '%s' to format '%s'\n", chan->name, opbx_codec2str(class->format));
			moh_release(NULL, res);
			res = NULL;
		}
		if (option_verbose > 2)
			opbx_verbose(VERBOSE_PREFIX_3 "Started music on hold, class '%s', on channel '%s'\n", class->name, chan->name);
	}
	return res;
}

static int moh_generate(struct opbx_channel *chan, void *data, int samples)
{
	struct opbx_frame f;
	struct mohdata *moh = data;
	short buf[1280 + OPBX_FRIENDLY_OFFSET / 2];
	int len, res;

	if (!moh->parent->pid)
		return -1;

	len = opbx_codec_get_len(moh->parent->format, samples);

	if (len > sizeof(buf) - OPBX_FRIENDLY_OFFSET) {
		opbx_log(LOG_WARNING, "Only doing %d of %d requested bytes on %s\n", (int)sizeof(buf), len, chan->name);
		len = sizeof(buf) - OPBX_FRIENDLY_OFFSET;
	}
	res = read(moh->pipe[0], buf + OPBX_FRIENDLY_OFFSET/2, len);
#if 0
	if (res != len) {
		opbx_log(LOG_WARNING, "Read only %d of %d bytes: %s\n", res, len, strerror(errno));
	}
#endif
	if (res <= 0)
		return 0;

	memset(&f, 0, sizeof(f));
	
	f.frametype = OPBX_FRAME_VOICE;
	f.subclass = moh->parent->format;
	f.mallocd = 0;
	f.datalen = res;
	f.data = buf + OPBX_FRIENDLY_OFFSET / 2;
	f.offset = OPBX_FRIENDLY_OFFSET;
	f.samples = opbx_codec_get_samples(&f);

	if (opbx_write(chan, &f) < 0) {
		opbx_log(LOG_WARNING, "Failed to write frame to '%s': %s\n", chan->name, strerror(errno));
		return -1;
	}

	return 0;
}

static struct opbx_generator mohgen = 
{
	alloc: moh_alloc,
	release: moh_release,
	generate: moh_generate,
};

static int moh_scan_files(struct mohclass *class) {

	DIR *files_DIR;
	struct dirent *files_dirent;
	char path[512];
	char filepath[MAX_MOHFILE_LEN];
	char *ext;
	struct stat statbuf;
	int dirnamelen;
	int i;
	
	files_DIR = opendir(class->dir);
	if (!files_DIR) {
		opbx_log(LOG_WARNING, "Cannot open dir %s or dir does not exist\n", class->dir);
		return -1;
	}

	class->total_files = 0;
	dirnamelen = strlen(class->dir) + 2;
	getcwd(path, 512);
	chdir(class->dir);
	memset(class->filearray, 0, MAX_MOHFILES*MAX_MOHFILE_LEN);
	while ((files_dirent = readdir(files_DIR))) {
		if ((files_dirent->d_name[0] == '.') || ((strlen(files_dirent->d_name) + dirnamelen) >= MAX_MOHFILE_LEN))
			continue;

		snprintf(filepath, MAX_MOHFILE_LEN, "%s/%s", class->dir, files_dirent->d_name);

		if (stat(filepath, &statbuf))
			continue;

		if (!S_ISREG(statbuf.st_mode))
			continue;

		if ((ext = strrchr(filepath, '.'))) {
			*ext = '\0';
			ext++;
		}

		/* if the file is present in multiple formats, ensure we only put it into the list once */
		for (i = 0; i < class->total_files; i++)
			if (!strcmp(filepath, class->filearray[i]))
				break;

		if (i == class->total_files)
			strcpy(class->filearray[class->total_files++], filepath);
	}

	closedir(files_DIR);
	chdir(path);
	return class->total_files;
}

static int moh_register(struct mohclass *moh)
{
	opbx_mutex_lock(&moh_lock);
	if (get_mohbyname(moh->name)) {
		opbx_log(LOG_WARNING, "Music on Hold class '%s' already exists\n", moh->name);
		free(moh);	
		opbx_mutex_unlock(&moh_lock);
		return -1;
	}
	opbx_mutex_unlock(&moh_lock);

	time(&moh->start);
	moh->start -= respawn_time;
	
	if (!strcasecmp(moh->mode, "files")) {
		if (!moh_scan_files(moh)) {
			opbx_moh_free_class(&moh);
			return -1;
		}
		if (strchr(moh->args, 'r'))
			opbx_set_flag(moh, MOH_RANDOMIZE);
	} else if (!strcasecmp(moh->mode, "custom")) {
		
		opbx_set_flag(moh, MOH_CUSTOM);
		
		moh->srcfd = -1;
		if (opbx_pthread_create(&moh->thread, NULL, monitor_custom_command, moh)) {
			opbx_log(LOG_WARNING, "Unable to create moh...\n");
			opbx_moh_free_class(&moh);
			return -1;
		}
	} else {
		opbx_log(LOG_WARNING, "Don't know how to do a mode '%s' music on hold\n", moh->mode);
		opbx_moh_free_class(&moh);
		return -1;
	}
	opbx_mutex_lock(&moh_lock);
	moh->next = mohclasses;
	mohclasses = moh;
	opbx_mutex_unlock(&moh_lock);
	return 0;
}

static void local_opbx_moh_cleanup(struct opbx_channel *chan)
{
	if (chan->music_state) {
		free(chan->music_state);
		chan->music_state = NULL;
	}
}

static int local_opbx_moh_start(struct opbx_channel *chan, char *class)
{
	struct mohclass *mohclass;

	if (!class || opbx_strlen_zero(class))
		class = chan->musicclass;
	if (!class || opbx_strlen_zero(class))
		class = "default";
	opbx_mutex_lock(&moh_lock);
	mohclass = get_mohbyname(class);
	opbx_mutex_unlock(&moh_lock);

	if (!mohclass) {
		opbx_log(LOG_WARNING, "No class: %s\n", (char *)class);
		return -1;
	}

	/* Stop any generators that might be running */
	opbx_generator_deactivate(chan);

	opbx_set_flag(chan, OPBX_FLAG_MOH);
	if (mohclass->total_files) {
		return opbx_generator_activate(chan, &moh_file_stream, mohclass);
	} else
		return opbx_generator_activate(chan, &mohgen, mohclass);
}

static void local_opbx_moh_stop(struct opbx_channel *chan)
{
	opbx_clear_flag(chan, OPBX_FLAG_MOH);
	opbx_generator_deactivate(chan);

	if (chan->music_state) {
		if (chan->stream) {
			opbx_closestream(chan->stream);
			chan->stream = NULL;
		}
	}
}

static struct mohclass *moh_class_malloc(void)
{
	struct mohclass *class;

	class = malloc(sizeof(struct mohclass));

	if (!class)
		return NULL;

	memset(class, 0, sizeof(struct mohclass));

	class->format = OPBX_FORMAT_SLINEAR;

	return class;
}

static int load_moh_classes(void)
{
	struct opbx_config *cfg;
	struct opbx_variable *var;
	struct mohclass *class;	
	char *data;
	char *args;
	char *cat;
	int numclasses = 0;
	static int dep_warning = 0;

	cfg = opbx_config_load("musiconhold.conf");

	if (!cfg)
		return 0;

	cat = opbx_category_browse(cfg, NULL);
	for (; cat; cat = opbx_category_browse(cfg, cat)) {
		if (strcasecmp(cat, "classes") && strcasecmp(cat, "moh_files")) {
			class = moh_class_malloc();
			if (!class) {
				opbx_log(LOG_WARNING, "Out of memory!\n");
				break;
			}				
			opbx_copy_string(class->name, cat, sizeof(class->name));	
			var = opbx_variable_browse(cfg, cat);
			while (var) {
				if (!strcasecmp(var->name, "mode"))
					opbx_copy_string(class->mode, var->value, sizeof(class->name)); 
				else if (!strcasecmp(var->name, "directory"))
					opbx_copy_string(class->dir, var->value, sizeof(class->dir));
				else if (!strcasecmp(var->name, "application"))
					opbx_copy_string(class->args, var->value, sizeof(class->args));
				else if (!strcasecmp(var->name, "random"))
					opbx_set2_flag(class, opbx_true(var->value), MOH_RANDOMIZE);
				else if (!strcasecmp(var->name, "format")) {
					class->format = opbx_getformatbyname(var->value);
					if (!class->format) {
						opbx_log(LOG_WARNING, "Unknown format '%s' -- defaulting to SLIN\n", var->value);
						class->format = OPBX_FORMAT_SLINEAR;
					}
				}
					var = var->next;
			}

			if (opbx_strlen_zero(class->dir)) {
				if (!strcasecmp(class->mode, "custom")) {
					strcpy(class->dir, "nodir");
				} else {
					opbx_log(LOG_WARNING, "A directory must be specified for class '%s'!\n", class->name);
					free(class);
					continue;
				}
			}
			if (opbx_strlen_zero(class->mode)) {
				opbx_log(LOG_WARNING, "A mode must be specified for class '%s'!\n", class->name);
				free(class);
				continue;
			}
			if (opbx_strlen_zero(class->args) && !strcasecmp(class->mode, "custom")) {
				opbx_log(LOG_WARNING, "An application must be specified for class '%s'!\n", class->name);
				free(class);
				continue;
			}

			moh_register(class);
			numclasses++;
		}
	}
	

	/* Deprecated Old-School Configuration */
	var = opbx_variable_browse(cfg, "classes");
	while (var) {
		if (!dep_warning) {
			opbx_log(LOG_WARNING, "The old musiconhold.conf syntax has been deprecated!  Please refer to the sample configuration for information on the new syntax.\n");
			dep_warning = 1;
		}
		data = strchr(var->value, ':');
		if (data) {
			*data++ = '\0';
			args = strchr(data, ',');
			if (args)
				*args++ = '\0';
			if (!(get_mohbyname(var->name))) {
				class = moh_class_malloc();
				if (!class) {
					opbx_log(LOG_WARNING, "Out of memory!\n");
					return numclasses;
				}
				
				opbx_copy_string(class->name, var->name, sizeof(class->name));
				opbx_copy_string(class->dir, data, sizeof(class->dir));
				opbx_copy_string(class->mode, var->value, sizeof(class->mode));
				if (args)
					opbx_copy_string(class->args, args, sizeof(class->args));
				
				moh_register(class);
				numclasses++;
			}
		}
		var = var->next;
	}
	var = opbx_variable_browse(cfg, "moh_files");
	while (var) {
		if (!dep_warning) {
			opbx_log(LOG_WARNING, "The old musiconhold.conf syntax has been deprecated!  Please refer to the sample configuration for information on the new syntax.\n");
			dep_warning = 1;
		}
		if (!(get_mohbyname(var->name))) {
			args = strchr(var->value, ',');
			if (args)
				*args++ = '\0';
			class = moh_class_malloc();
			if (!class) {
				opbx_log(LOG_WARNING, "Out of memory!\n");
				return numclasses;
			}
			
			opbx_copy_string(class->name, var->name, sizeof(class->name));
			opbx_copy_string(class->dir, var->value, sizeof(class->dir));
			strcpy(class->mode, "files");
			if (args)	
				opbx_copy_string(class->args, args, sizeof(class->args));
			
			moh_register(class);
			numclasses++;
		}
		var = var->next;
	}

	opbx_config_destroy(cfg);

	return numclasses;
}

static void opbx_moh_destroy(void)
{
	struct mohclass *moh, *tmp;
	char buff[8192];
	int bytes, tbytes=0, stime = 0, pid = 0;

	if (option_verbose > 1)
		opbx_verbose(VERBOSE_PREFIX_2 "Destroying musiconhold processes\n");
	opbx_mutex_lock(&moh_lock);
	moh = mohclasses;

	while (moh) {
/*
		// This Seems to kill openpbx Whenever a reload is done.
		// Someone should look into it. In the meanwhile, it's better to
		// comment it out to prevent malfunctions.

		if (moh->thread) 
			pthread_kill(moh->thread, SIGKILL); 
*/
		if (moh->pid) {
			opbx_log(LOG_DEBUG, "killing %d!\n", moh->pid);
			stime = time(NULL) + 2;
			pid = moh->pid;
			moh->pid = 0;
			kill(pid, SIGKILL);
			while ((opbx_wait_for_input(moh->srcfd, 100) > 0) && (bytes = read(moh->srcfd, buff, 8192)) && time(NULL) < stime) {
				tbytes = tbytes + bytes;
			}
			opbx_log(LOG_DEBUG, "Custom command pid %d and child died after %d bytes read\n", pid, tbytes);
			close(moh->srcfd);
		}
		tmp = moh;
		moh = moh->next;
		opbx_moh_free_class(&tmp);
	}
	mohclasses = NULL;
	opbx_mutex_unlock(&moh_lock);
}

static void moh_on_off(int on)
{
	struct opbx_channel *chan = NULL;

	while ( (chan = opbx_channel_walk_locked(chan)) != NULL) {
		if (opbx_test_flag(chan, OPBX_FLAG_MOH)) {
			if (on)
				local_opbx_moh_start(chan, NULL);
			else
				opbx_generator_deactivate(chan);
		}
		opbx_mutex_unlock(&chan->lock);
	}
}

static int moh_cli(int fd, int argc, char *argv[]) 
{
	int x;

	moh_on_off(0);
	opbx_moh_destroy();
	x = load_moh_classes();
	moh_on_off(1);
	opbx_cli(fd, "\n%d class%s reloaded.\n", x, x == 1 ? "" : "es");
	return 0;
}

static int cli_files_show(int fd, int argc, char *argv[])
{
	int i;
	struct mohclass *class;

	opbx_mutex_lock(&moh_lock);
	for (class = mohclasses; class; class = class->next) {
		if (!class->total_files)
			continue;

		opbx_cli(fd, "Class: %s\n", class->name);
		for (i = 0; i < class->total_files; i++)
			opbx_cli(fd, "\tFile: %s\n", class->filearray[i]);
	}
	opbx_mutex_unlock(&moh_lock);

	return 0;
}

static int moh_classes_show(int fd, int argc, char *argv[])
{
	struct mohclass *class;

	opbx_mutex_lock(&moh_lock);
	for (class = mohclasses; class; class = class->next) {
		opbx_cli(fd, "Class: %s\n", class->name);
		opbx_cli(fd, "\tMode: %s\n", opbx_strlen_zero(class->mode) ? "<none>" : class->mode);
		opbx_cli(fd, "\tDirectory: %s\n", opbx_strlen_zero(class->dir) ? "<none>" : class->dir);
		if (opbx_test_flag(class, MOH_CUSTOM))
			opbx_cli(fd, "\tApplication: %s\n", opbx_strlen_zero(class->args) ? "<none>" : class->args);
		opbx_cli(fd, "\tFormat: %s\n", opbx_getformatname(class->format));
	}
	opbx_mutex_unlock(&moh_lock);

	return 0;
}

static struct opbx_cli_entry  cli_moh = { { "moh", "reload"}, moh_cli, "Music On Hold", "Music On Hold", NULL};

static struct opbx_cli_entry  cli_moh_classes_show = { { "moh", "classes", "show"}, moh_classes_show, "List MOH classes", "Lists all MOH classes", NULL};

static struct opbx_cli_entry  cli_moh_files_show = { { "moh", "files", "show"}, cli_files_show, "List MOH file-based classes", "Lists all loaded file-based MOH classes and their files", NULL};

static int init_classes(void) 
{
	struct mohclass *moh;
    
	if (!load_moh_classes()) 		/* Load classes from config */
		return 0;			/* Return if nothing is found */
	moh = mohclasses;
	while (moh) {
		if (moh->total_files)
			moh_scan_files(moh);
		moh = moh->next;
	}
	return 1;
}

int load_module(void)
{
	int res;

	res = opbx_register_application(app0, moh0_exec, synopsis0, descrip0);
	opbx_register_atexit(opbx_moh_destroy);
	opbx_cli_register(&cli_moh);
	opbx_cli_register(&cli_moh_files_show);
	opbx_cli_register(&cli_moh_classes_show);
	if (!res)
		res = opbx_register_application(app1, moh1_exec, synopsis1, descrip1);
	if (!res)
		res = opbx_register_application(app2, moh2_exec, synopsis2, descrip2);
	if (!res)
		res = opbx_register_application(app3, moh3_exec, synopsis3, descrip3);
	if (!res)
		res = opbx_register_application(app4, moh4_exec, synopsis4, descrip4);

	if (!init_classes()) { 	/* No music classes configured, so skip it */
		opbx_log(LOG_WARNING, "No music on hold classes configured, disabling music on hold.\n");
	} else {
		opbx_install_music_functions(local_opbx_moh_start, local_opbx_moh_stop, local_opbx_moh_cleanup);
	}

	return 0;
}

int reload(void)
{
	if (init_classes())
		opbx_install_music_functions(local_opbx_moh_start, local_opbx_moh_stop, local_opbx_moh_cleanup);

	return 0;
}

int unload_module(void)
{
	return -1;
}

char *description(void)
{
	return "Music On Hold Resource";
}

int usecount(void)
{
	/* Never allow Music On Hold to be unloaded
	   unresolve needed symbols in the dialer */
#if 0
	int res;
	STANDARD_USECOUNT(res);
	return res;
#else
	return 1;
#endif
}
