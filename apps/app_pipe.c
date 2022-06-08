/*
 * OpenPBX -- A telephony toolkit for Linux.
 *
 * PIPE Standard in or out of a call
 * 
 * Copyright (C) 1999, Mark Spencer
 *
 * Mark Spencer <markster@linux-support.net>
 *
 * Adaptated by Tony 
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 *
 * 
 * /usr/local/bin/mpg123.bin -q -s --mono -r 8000 -f 4096 $1
 * lame -x -r -s 4 --bitwidth 16 -b 256 --resample 44100 - - | ices -c /tmp/test.conf
 * ices 0.4 required. Config is simple, normal config (no rencode necesarry, lame does it), 
 *   and set only a dash '-' in the playlist file so it STDIN 
 *
 */
 
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <errno.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION(__FILE__, "$Revision: 1 $")

#include "openpbx/lock.h"
#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/frame.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/translate.h"

static char *tdesc = "Pipe Raw Audio to and from an External Process";

static char *app = "PIPE";

static char *synopsis = "Pipe Raw Audio to and from an External Process";

static char *descrip = 
	"  PIPE(1=in/0=out|program|argument) Pipe Raw Audio to and from an External Process";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int pipeencode(char *filename, char *argument, int fdin, int fdout)
{
	int res;
	int x;
	res = fork();
	if (res < 0) 
		opbx_log(LOG_WARNING, "Fork failed\n");
	if (res)
		return res;
	dup2(fdin, STDIN_FILENO);
	dup2(fdout, STDOUT_FILENO);
	for (x=0;x<256;x++) {
		if ((x != STDIN_FILENO && x != STDOUT_FILENO) || STDERR_FILENO == x)
			close(x);
	}
	opbx_log(LOG_WARNING, "Launching '%s' '%s'\n", filename, argument);
	execlp(filename, "TEST", argument, (char *)NULL);
	opbx_log(LOG_WARNING, "Execute of %s failed\n", filename);
	return -1;
}

static int timed_read(int fd, void *data, int datalen, int timeout)
{
	int res;
	struct pollfd fds[1];
	fds[0].fd = fd;
	fds[0].events = POLLIN;
	res = poll(fds, 1, timeout);
	if (res < 1) {
		opbx_log(LOG_NOTICE, "Poll timed out/errored out with %d\n", res);
		return -1;
	}
	return read(fd, data, datalen);

}

static int pipe_exec(struct opbx_channel *chan, void *data)
{
	int res=0;
	struct localuser *u;
	int fds[2];
	int ms = -1;
	int pid = -1;
	int flags;
	int owriteformat;
	int oreadformat;
	int timeout = 2000;
	int stdinout = -1;
	struct timeval last;
	struct opbx_frame *f;
	char filename[256]="";
	char argument[256]="";
	char *c;
	struct myframe {
		struct opbx_frame f;
		char offset[OPBX_FRIENDLY_OFFSET];
		short frdata[160];
	} myf;

	last.tv_usec = 0;
	last.tv_sec = 0;

	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "PIPE requires an argument (filename)\n");
		return -1;
	}
	if (!opbx_strlen_zero(data)) {
		char *tmp;
		int argc;
		char *argv[3];

		tmp = opbx_strdupa(data);
		argc = opbx_separate_app_args(tmp, '|', argv, sizeof(argv) / sizeof(argv[0]));

		if (argc >= 2) {
			if (!opbx_strlen_zero(argv[0])) {
				switch (argv[0][0]) {
					case '1':
						stdinout = 1;
						break;
					case '0':
						stdinout = 0;
						break;
				}
			}

			opbx_log(LOG_WARNING, "SELECTED %d\n",stdinout);
			strncpy(filename, argv[1], sizeof(filename) - 1);
		}
		if (argc == 3) {
			strncpy(argument, argv[2], sizeof(argument) - 1);
		}
	}
	if (stdinout == -1) {
		opbx_log(LOG_WARNING, "Arguments are invalid\n");
		return -1;
	}

	LOCAL_USER_ADD(u);

	if (pipe(fds)) {
		opbx_log(LOG_WARNING, "Unable to create pipe\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}

// MOC: Setting non blocking doesn't seem to change anything
//	flags = fcntl(fds[1], F_GETFL);
//	fcntl(fds[1], F_SETFL, flags | O_NONBLOCK);

//	flags = fcntl(fds[0], F_GETFL);
//	fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

	opbx_stopstream(chan);

	if (chan->_state != OPBX_STATE_UP)
		res = opbx_answer(chan);
		
	if (res) {
		close(fds[0]);
		close(fds[1]);
		opbx_log(LOG_WARNING, "Answer failed!\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	oreadformat = chan->readformat;
	res = opbx_set_read_format(chan, OPBX_FORMAT_SLINEAR);

	owriteformat = chan->writeformat;
	res += opbx_set_write_format(chan, OPBX_FORMAT_SLINEAR);

	if (res < 0) {
		close(fds[0]);
		close(fds[1]);
		opbx_log(LOG_WARNING, "Unable to set write format to signed linear\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	res = pipeencode(filename, argument, fds[0], fds[1]);

	if (res >= 0) {
	   last = opbx_tvnow();
	   last.tv_sec += 1;

		pid = res;
		for (;;) {
			/* Wait for audio, and stream */
			if (stdinout == 0) {
				/* START WRITE TO FD */
				ms = opbx_waitfor(chan, 10);
				if (ms < 0) {
					opbx_log(LOG_DEBUG, "Hangup detected\n");
					res = -1;
					break;
				} else if (ms > 0) {
					f = opbx_read(chan);
					if (!f) {
						opbx_log(LOG_DEBUG, "Null frame == hangup() detected\n");
						res = -1;
						break;
					}
					if (f->frametype == OPBX_FRAME_DTMF) {
						opbx_log(LOG_DEBUG, "User pressed a key\n");
						opbx_frfree(f);
						res = 0;
						break;
					}
					if (f->frametype == OPBX_FRAME_VOICE) {
						res = write(fds[1], f->data, f->datalen);
						if (res < 0) {
							if (errno != EAGAIN) {
								opbx_log(LOG_WARNING, "Write failed to pipe: %s\n", strerror(errno));
								res = -1;
								break;
							}
						}
					}
					opbx_frfree(f);
				} /* END WRITE TO FD */
			}
			if (stdinout == 1) {
				/* START WRITE CHANNEL */
				ms = opbx_tvdiff_ms(last, opbx_tvnow());
				if (ms <= 0) {
					res = timed_read(fds[0], myf.frdata, sizeof(myf.frdata), timeout);
					if (res > 0) {
						myf.f.frametype = OPBX_FRAME_VOICE;
						myf.f.subclass = OPBX_FORMAT_SLINEAR;
						myf.f.datalen = res;
						myf.f.samples = res / 2;
						myf.f.mallocd = 0;
						myf.f.offset = OPBX_FRIENDLY_OFFSET;
						myf.f.src = __PRETTY_FUNCTION__;
						myf.f.delivery.tv_sec = 0;
						myf.f.delivery.tv_usec = 0;
						myf.f.data = myf.frdata;
						if (opbx_write(chan, &myf.f) < 0) {
							res = -1;
							break;
						}
					} else {
						opbx_log(LOG_DEBUG, "No more stream\n");
						res = 0;
						break;
					}
					last = opbx_tvadd(last, opbx_samp2tv(myf.f.samples, 8000));
				} else {
					ms = opbx_waitfor(chan, ms);
					if (ms < 0) {
						opbx_log(LOG_DEBUG, "Hangup detected\n");
						res = -1;
						break;
					}
					if (ms) {
						f = opbx_read(chan);
						if (!f) {
							opbx_log(LOG_DEBUG, "Null frame == hangup() detected\n");
							res = -1;
							break;
						}
						if (f->frametype == OPBX_FRAME_DTMF) {
							opbx_log(LOG_DEBUG, "User pressed a key\n");
							opbx_frfree(f);
							res = 0;
							break;
						}
						opbx_frfree(f);
					}
				}
				/* END WRITE CHANNEL */
			}
		}
	}
	close(fds[0]);
	close(fds[1]);

	LOCAL_USER_REMOVE(u);
	if (pid > -1)
		kill(pid, SIGKILL);
	if (!res && oreadformat)
		opbx_set_read_format(chan, oreadformat);
	if (!res && owriteformat)
		opbx_set_write_format(chan, owriteformat);

	return res;
}

int unload_module(void)
{
	int res;

	res = opbx_unregister_application(app);

	STANDARD_HANGUP_LOCALUSERS;
	
	return res;
}

int load_module(void)
{
	return opbx_register_application(app, pipe_exec, synopsis, descrip);
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
