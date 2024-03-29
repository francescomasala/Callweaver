/*
 * CallWeaver -- An open source telephony toolkit.
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

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/apps/app_pipe.c $", "$Revision: 4723 $")

#include "callweaver/lock.h"
#include "callweaver/file.h"
#include "callweaver/logger.h"
#include "callweaver/channel.h"
#include "callweaver/frame.h"
#include "callweaver/pbx.h"
#include "callweaver/module.h"
#include "callweaver/translate.h"

static char *tdesc = "Pipe Raw Audio to and from an External Process";

static void *pipe_app;
static const char *pipe_name = "PIPE";
static const char *pipe_synopsis = "Pipe Raw Audio to and from an External Process";
static const char *pipe_syntax = "PIPE(1=in/0=out, program, argument)";
static const char *pipe_descrip = "Pipe Raw Audio to and from an External Process";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int pipeencode(char *filename, char *argument, int fdin, int fdout)
{
	int res;
	int x;
	res = fork();
	if (res < 0) 
		cw_log(LOG_WARNING, "Fork failed\n");
	if (res)
		return res;
	dup2(fdin, STDIN_FILENO);
	dup2(fdout, STDOUT_FILENO);
	for (x=0;x<256;x++) {
		if ((x != STDIN_FILENO && x != STDOUT_FILENO) || STDERR_FILENO == x)
			close(x);
	}
	cw_log(LOG_WARNING, "Launching '%s' '%s'\n", filename, argument);
	execlp(filename, "TEST", argument, (char *)NULL);
	cw_log(LOG_WARNING, "Execute of %s failed\n", filename);
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
		cw_log(LOG_NOTICE, "Poll timed out/errored out with %d\n", res);
		return -1;
	}
	return read(fd, data, datalen);

}

static int pipe_exec(struct cw_channel *chan, int argc, char **argv)
{
	int res=0;
	struct localuser *u;
	int fds[2];
	int ms = -1;
	int pid = -1;
	int owriteformat;
	int oreadformat;
	int timeout = 2000;
	struct timeval last;
	struct cw_frame *f;
	struct myframe {
		struct cw_frame f;
		char offset[CW_FRIENDLY_OFFSET];
		short frdata[160];
	} myf;

	last.tv_usec = 0;
	last.tv_sec = 0;

	if (argc < 2 || argc > 3) {
		cw_log(LOG_ERROR, "Syntax: %s\n", pipe_syntax);
		return -1;
	}

	LOCAL_USER_ADD(u);

	if (pipe(fds)) {
		cw_log(LOG_WARNING, "Unable to create pipe\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}

// MOC: Setting non blocking doesn't seem to change anything
//	flags = fcntl(fds[1], F_GETFL);
//	fcntl(fds[1], F_SETFL, flags | O_NONBLOCK);

//	flags = fcntl(fds[0], F_GETFL);
//	fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

	cw_stopstream(chan);

	if (chan->_state != CW_STATE_UP)
		res = cw_answer(chan);
		
	if (res) {
		close(fds[0]);
		close(fds[1]);
		cw_log(LOG_WARNING, "Answer failed!\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	oreadformat = chan->readformat;
	res = cw_set_read_format(chan, CW_FORMAT_SLINEAR);

	owriteformat = chan->writeformat;
	res += cw_set_write_format(chan, CW_FORMAT_SLINEAR);

	if (res < 0) {
		close(fds[0]);
		close(fds[1]);
		cw_log(LOG_WARNING, "Unable to set write format to signed linear\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	res = pipeencode(argv[1], argv[2], fds[0], fds[1]);

	if (res >= 0) {
	   last = cw_tvnow();
	   last.tv_sec += 1;

		pid = res;
		for (;;) {
			/* Wait for audio, and stream */
			if (argv[0][0] == '0') {
				/* START WRITE TO FD */
				ms = cw_waitfor(chan, 10);
				if (ms < 0) {
					cw_log(LOG_DEBUG, "Hangup detected\n");
					res = -1;
					break;
				} else if (ms > 0) {
					f = cw_read(chan);
					if (!f) {
						cw_log(LOG_DEBUG, "Null frame == hangup() detected\n");
						res = -1;
						break;
					}
					if (f->frametype == CW_FRAME_DTMF) {
						cw_log(LOG_DEBUG, "User pressed a key\n");
						cw_fr_free(f);
						res = 0;
						break;
					}
					if (f->frametype == CW_FRAME_VOICE) {
						res = write(fds[1], f->data, f->datalen);
						if (res < 0) {
							if (errno != EAGAIN) {
								cw_log(LOG_WARNING, "Write failed to pipe: %s\n", strerror(errno));
                                cw_fr_free(f);
								res = -1;
								break;
							}
						}
					}
					cw_fr_free(f);
				} /* END WRITE TO FD */
			} else {
				/* START WRITE CHANNEL */
				ms = cw_tvdiff_ms(last, cw_tvnow());
				if (ms <= 0) {
					res = timed_read(fds[0], myf.frdata, sizeof(myf.frdata), timeout);
					if (res > 0) {
                        cw_fr_init_ex(&myf.f, CW_FRAME_VOICE, CW_FORMAT_SLINEAR, __PRETTY_FUNCTION__);
						myf.f.datalen = res;
						myf.f.samples = res/sizeof(int16_t);
						myf.f.offset = CW_FRIENDLY_OFFSET;
						myf.f.data = myf.frdata;
						if (cw_write(chan, &myf.f) < 0)
                        {
							res = -1;
							break;
						}
					} else {
						cw_log(LOG_DEBUG, "No more stream\n");
						res = 0;
						break;
					}
					last = cw_tvadd(last, cw_samp2tv(myf.f.samples, 8000));
				} else {
					ms = cw_waitfor(chan, ms);
					if (ms < 0) {
						cw_log(LOG_DEBUG, "Hangup detected\n");
						res = -1;
						break;
					}
					if (ms) {
						f = cw_read(chan);
						if (!f) {
							cw_log(LOG_DEBUG, "Null frame == hangup() detected\n");
							res = -1;
							break;
						}
						if (f->frametype == CW_FRAME_DTMF) {
							cw_log(LOG_DEBUG, "User pressed a key\n");
							cw_fr_free(f);
							res = 0;
							break;
						}
						cw_fr_free(f);
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
		cw_set_read_format(chan, oreadformat);
	if (!res && owriteformat)
		cw_set_write_format(chan, owriteformat);

	return res;
}

int unload_module(void)
{
	int res = 0;
	res |= cw_unregister_application(pipe_app);
	STANDARD_HANGUP_LOCALUSERS;
	return res;
}

int load_module(void)
{
	pipe_app = cw_register_application(pipe_name, pipe_exec, pipe_synopsis, pipe_syntax, pipe_descrip);
	return 0;
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
