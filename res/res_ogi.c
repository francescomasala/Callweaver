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
 * \brief OGI - the OpenPBX Gateway Interface
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <sys/types.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/time.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL: svn://ctrix@svn.openpbx.org/openpbx/trunk/res/res_ogi.c $", "$Revision: 1547 $")

#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/opbxdb.h"
#include "openpbx/phone_no_utils.h"
#include "openpbx/cli.h"
#include "openpbx/logger.h"
#include "openpbx/options.h"
#include "openpbx/image.h"
#include "openpbx/say.h"
#include "openpbx/app.h"
#include "openpbx/dsp.h"
#include "openpbx/musiconhold.h"
#include "openpbx/manager.h"
#include "openpbx/utils.h"
#include "openpbx/lock.h"
#include "openpbx/strings.h"
#include "openpbx/ogi.h"

#define MAX_ARGS 128
#define MAX_COMMANDS 128

/* Recycle some stuff from the CLI interface */
#define fdprintf ogi_debug_cli

static char *tdesc = "OpenPBX Gateway Interface (OGI)";

static char *app = "OGI";

static char *eapp = "EOGI";

static char *deadapp = "DeadOGI";

static char *synopsis = "Executes an OGI compliant application";
static char *esynopsis = "Executes an EOGI compliant application";
static char *deadsynopsis = "Executes OGI on a hungup channel";

static char *descrip =
"  [E|Dead]OGI(command|args): Executes an OpenPBX Gateway Interface compliant\n"
"program on a channel. OGI allows OpenPBX to launch external programs\n"
"written in any language to control a telephony channel, play audio,\n"
"read DTMF digits, etc. by communicating with the OGI protocol on stdin\n"
"and stdout.\n"
"Returns -1 on hangup (except for DeadOGI) or if application requested\n"
" hangup, or 0 on non-hangup exit. \n"
"Using 'EOGI' provides enhanced OGI, with incoming audio available out of band"
"on file descriptor 3\n\n"
"Use the CLI command 'show ogi' to list available ogi commands\n";

static int ogidebug = 0;

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;


#define TONE_BLOCK_SIZE 200

/* Max time to connect to an OGI remote host */
#define MAX_OGI_CONNECT 2000

#define OGI_PORT 4573

static void ogi_debug_cli(int fd, char *fmt, ...)
{
	char *stuff;
	int res = 0;

	va_list ap;
	va_start(ap, fmt);
	res = vasprintf(&stuff, fmt, ap);
	va_end(ap);
	if (res == -1) {
		opbx_log(LOG_ERROR, "Out of memory\n");
	} else {
		if (ogidebug)
			opbx_verbose("OGI Tx >> %s", stuff);
		opbx_carefulwrite(fd, stuff, strlen(stuff), 100);
		free(stuff);
	}
}

/* launch_netscript: The fastogi handler.
	FastOGI defaults to port 4573 */
static int launch_netscript(char *ogiurl, char *argv[], int *fds, int *efd, int *opid)
{
	int s;
	int flags;
	struct pollfd pfds[1];
	char *host;
	char *c; int port = OGI_PORT;
	char *script="";
	struct sockaddr_in sin;
	struct hostent *hp;
	struct opbx_hostent ahp;

	host = opbx_strdupa(ogiurl + 6);	/* Remove ogi:// */
	if (!host)
		return -1;
	/* Strip off any script name */
	if ((c = strchr(host, '/'))) {
		*c = '\0';
		c++;
		script = c;
	}
	if ((c = strchr(host, ':'))) {
		*c = '\0';
		c++;
		port = atoi(c);
	}
	if (efd) {
		opbx_log(LOG_WARNING, "OGI URI's don't support Enhanced OGI yet\n");
		return -1;
	}
	hp = opbx_gethostbyname(host, &ahp);
	if (!hp) {
		opbx_log(LOG_WARNING, "Unable to locate host '%s'\n", host);
		return -1;
	}
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		opbx_log(LOG_WARNING, "Unable to create socket: %s\n", strerror(errno));
		return -1;
	}
	flags = fcntl(s, F_GETFL);
	if (flags < 0) {
		opbx_log(LOG_WARNING, "Fcntl(F_GETFL) failed: %s\n", strerror(errno));
		close(s);
		return -1;
	}
	if (fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) {
		opbx_log(LOG_WARNING, "Fnctl(F_SETFL) failed: %s\n", strerror(errno));
		close(s);
		return -1;
	}
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	memcpy(&sin.sin_addr, hp->h_addr, sizeof(sin.sin_addr));
	if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) && (errno != EINPROGRESS)) {
		opbx_log(LOG_WARNING, "Connect failed with unexpected error: %s\n", strerror(errno));
		close(s);
		return -1;
	}
	pfds[0].fd = s;
	pfds[0].events = POLLOUT;
	if (poll(pfds, 1, MAX_OGI_CONNECT) != 1) {
		opbx_log(LOG_WARNING, "Connect to '%s' failed!\n", ogiurl);
		close(s);
		return -1;
	}
	if (write(s, "ogi_network: yes\n", strlen("ogi_network: yes\n")) < 0) {
		opbx_log(LOG_WARNING, "Connect to '%s' failed: %s\n", ogiurl, strerror(errno));
		close(s);
		return -1;
	}

	/* If we have a script parameter, relay it to the fastogi server */
	if (!opbx_strlen_zero(script))
		fdprintf(s, "ogi_network_script: %s\n", script);

	if (option_debug > 3)
		opbx_log(LOG_DEBUG, "Wow, connected!\n");
	fds[0] = s;
	fds[1] = s;
	*opid = -1;
	return 0;
}

static int launch_script(char *script, char *argv[], int *fds, int *efd, int *opid)
{
	char tmp[256];
	int pid;
	int toast[2];
	int fromast[2];
	int audio[2];
	int x;
	int res;
	sigset_t signal_set;
	
	if (!strncasecmp(script, "ogi://", 6))
		return launch_netscript(script, argv, fds, efd, opid);
	
	if (script[0] != '/') {
		snprintf(tmp, sizeof(tmp), "%s/%s", (char *)opbx_config_OPBX_OGI_DIR, script);
		script = tmp;
	}
	if (pipe(toast)) {
		opbx_log(LOG_WARNING, "Unable to create toast pipe: %s\n",strerror(errno));
		return -1;
	}
	if (pipe(fromast)) {
		opbx_log(LOG_WARNING, "unable to create fromast pipe: %s\n", strerror(errno));
		close(toast[0]);
		close(toast[1]);
		return -1;
	}
	if (efd) {
		if (pipe(audio)) {
			opbx_log(LOG_WARNING, "unable to create audio pipe: %s\n", strerror(errno));
			close(fromast[0]);
			close(fromast[1]);
			close(toast[0]);
			close(toast[1]);
			return -1;
		}
		res = fcntl(audio[1], F_GETFL);
		if (res > -1) 
			res = fcntl(audio[1], F_SETFL, res | O_NONBLOCK);
		if (res < 0) {
			opbx_log(LOG_WARNING, "unable to set audio pipe parameters: %s\n", strerror(errno));
			close(fromast[0]);
			close(fromast[1]);
			close(toast[0]);
			close(toast[1]);
			close(audio[0]);
			close(audio[1]);
			return -1;
		}
	}
	pid = fork();
	if (pid < 0) {
		opbx_log(LOG_WARNING, "Failed to fork(): %s\n", strerror(errno));
		return -1;
	}
	if (!pid) {
		/* Don't run OGI scripts with realtime priority -- it causes audio stutter */
		opbx_set_priority(0);

		/* Redirect stdin and out, provide enhanced audio channel if desired */
		dup2(fromast[0], STDIN_FILENO);
		dup2(toast[1], STDOUT_FILENO);
		if (efd) {
			dup2(audio[0], STDERR_FILENO + 1);
		} else {
			close(STDERR_FILENO + 1);
		}
		
		/* unblock important signal handlers */
		if (sigfillset(&signal_set) || pthread_sigmask(SIG_UNBLOCK, &signal_set, NULL)) {
			opbx_log(LOG_WARNING, "unable to unblock signals for OGI script: %s\n", strerror(errno));
			exit(1);
		}

		/* Close everything but stdin/out/error */
		for (x=STDERR_FILENO + 2;x<1024;x++) 
			close(x);

		/* Execute script */
		execv(script, argv);
		/* Can't use opbx_log since FD's are closed */
		fprintf(stderr, "Failed to execute '%s': %s\n", script, strerror(errno));
		exit(1);
	}
	if (option_verbose > 2) 
		opbx_verbose(VERBOSE_PREFIX_3 "Launched OGI Script %s\n", script);
	fds[0] = toast[0];
	fds[1] = fromast[1];
	if (efd) {
		*efd = audio[1];
	}
	/* close what we're not using in the parent */
	close(toast[1]);
	close(fromast[0]);

	if (efd) {
		/* [PHM 12/18/03] */
		close(audio[0]);
	}

	*opid = pid;
	return 0;
		
}

static void setup_env(struct opbx_channel *chan, char *request, int fd, int enhanced)
{
	/* Print initial environment, with ogi_request always being the first
	   thing */
	fdprintf(fd, "ogi_request: %s\n", request);
	fdprintf(fd, "ogi_channel: %s\n", chan->name);
	fdprintf(fd, "ogi_language: %s\n", chan->language);
	fdprintf(fd, "ogi_type: %s\n", chan->type);
	fdprintf(fd, "ogi_uniqueid: %s\n", chan->uniqueid);

	/* ANI/DNIS */
	fdprintf(fd, "ogi_callerid: %s\n", chan->cid.cid_num ? chan->cid.cid_num : "unknown");
	fdprintf(fd, "ogi_calleridname: %s\n", chan->cid.cid_name ? chan->cid.cid_name : "unknown");
	fdprintf(fd, "ogi_callingpres: %d\n", chan->cid.cid_pres);
	fdprintf(fd, "ogi_callingani2: %d\n", chan->cid.cid_ani2);
	fdprintf(fd, "ogi_callington: %d\n", chan->cid.cid_ton);
	fdprintf(fd, "ogi_callingtns: %d\n", chan->cid.cid_tns);
	fdprintf(fd, "ogi_dnid: %s\n", chan->cid.cid_dnid ? chan->cid.cid_dnid : "unknown");
	fdprintf(fd, "ogi_rdnis: %s\n", chan->cid.cid_rdnis ? chan->cid.cid_rdnis : "unknown");

	/* Context information */
	fdprintf(fd, "ogi_context: %s\n", chan->context);
	fdprintf(fd, "ogi_extension: %s\n", chan->exten);
	fdprintf(fd, "ogi_priority: %d\n", chan->priority);
	fdprintf(fd, "ogi_enhanced: %s\n", enhanced ? "1.0" : "0.0");

	/* User information */
	fdprintf(fd, "ogi_accountcode: %s\n", chan->accountcode ? chan->accountcode : "");
    
	/* End with empty return */
	fdprintf(fd, "\n");
}

static int handle_answer(struct opbx_channel *chan, OGI *ogi, int argc, char *argv[])
{
	int res;
	res = 0;
	if (chan->_state != OPBX_STATE_UP) {
		/* Answer the chan */
		res = opbx_answer(chan);
	}
	fdprintf(ogi->fd, "200 result=%d\n", res);
	if (res >= 0)
		return RESULT_SUCCESS;
	else
		return RESULT_FAILURE;
}

static int handle_waitfordigit(struct opbx_channel *chan, OGI *ogi, int argc, char *argv[])
{
	int res;
	int to;
	if (argc != 4)
		return RESULT_SHOWUSAGE;
	if (sscanf(argv[3], "%d", &to) != 1)
		return RESULT_SHOWUSAGE;
	res = opbx_waitfordigit_full(chan, to, ogi->audio, ogi->ctrl);
	fdprintf(ogi->fd, "200 result=%d\n", res);
	if (res >= 0)
		return RESULT_SUCCESS;
	else
		return RESULT_FAILURE;
}

static int handle_sendtext(struct opbx_channel *chan, OGI *ogi, int argc, char *argv[])
{
	int res;
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	/* At the moment, the parser (perhaps broken) returns with
	   the last argument PLUS the newline at the end of the input
	   buffer. This probably needs to be fixed, but I wont do that
	   because other stuff may break as a result. The right way
	   would probably be to strip off the trailing newline before
	   parsing, then here, add a newline at the end of the string
	   before sending it to opbx_sendtext --DUDE */
	res = opbx_sendtext(chan, argv[2]);
	fdprintf(ogi->fd, "200 result=%d\n", res);
	if (res >= 0)
		return RESULT_SUCCESS;
	else
		return RESULT_FAILURE;
}

static int handle_recvchar(struct opbx_channel *chan, OGI *ogi, int argc, char *argv[])
{
	int res;
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	res = opbx_recvchar(chan,atoi(argv[2]));
	if (res == 0) {
		fdprintf(ogi->fd, "200 result=%d (timeout)\n", res);
		return RESULT_SUCCESS;
	}
	if (res > 0) {
		fdprintf(ogi->fd, "200 result=%d\n", res);
		return RESULT_SUCCESS;
	}
	else {
		fdprintf(ogi->fd, "200 result=%d (hangup)\n", res);
		return RESULT_FAILURE;
	}
}

static int handle_recvtext(struct opbx_channel *chan, OGI *ogi, int argc, char *argv[])
{
	char *buf;
	
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	buf = opbx_recvtext(chan,atoi(argv[2]));
	if (buf) {
		fdprintf(ogi->fd, "200 result=1 (%s)\n", buf);
		free(buf);
	} else {	
		fdprintf(ogi->fd, "200 result=-1\n");
	}
	return RESULT_SUCCESS;
}

static int handle_tddmode(struct opbx_channel *chan, OGI *ogi, int argc, char *argv[])
{
	int res,x;
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	if (!strncasecmp(argv[2],"on",2)) 
		x = 1; 
	else 
		x = 0;
	if (!strncasecmp(argv[2],"mate",4)) 
		x = 2;
	if (!strncasecmp(argv[2],"tdd",3))
		x = 1;
	res = opbx_channel_setoption(chan, OPBX_OPTION_TDD, &x, sizeof(char), 0);
	if (res != RESULT_SUCCESS)
		fdprintf(ogi->fd, "200 result=0\n");
	else
		fdprintf(ogi->fd, "200 result=1\n");
	return RESULT_SUCCESS;
}

static int handle_sendimage(struct opbx_channel *chan, OGI *ogi, int argc, char *argv[])
{
	int res;
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	res = opbx_send_image(chan, argv[2]);
	if (!opbx_check_hangup(chan))
		res = 0;
	fdprintf(ogi->fd, "200 result=%d\n", res);
	if (res >= 0)
		return RESULT_SUCCESS;
	else
		return RESULT_FAILURE;
}

static int handle_controlstreamfile(struct opbx_channel *chan, OGI *ogi, int argc, char *argv[])
{
	int res = 0;
	int skipms = 3000;
	char *fwd = NULL;
	char *rev = NULL;
	char *pause = NULL;
	char *stop = NULL;

	if (argc < 5 || argc > 9)
		return RESULT_SHOWUSAGE;

	if (!opbx_strlen_zero(argv[4]))
		stop = argv[4];
	else
		stop = NULL;
	
	if ((argc > 5) && (sscanf(argv[5], "%d", &skipms) != 1))
		return RESULT_SHOWUSAGE;

	if (argc > 6 && !opbx_strlen_zero(argv[8]))
		fwd = argv[6];
	else
		fwd = "#";

	if (argc > 7 && !opbx_strlen_zero(argv[8]))
		rev = argv[7];
	else
		rev = "*";
	
	if (argc > 8 && !opbx_strlen_zero(argv[8]))
		pause = argv[8];
	else
		pause = NULL;
	
	res = opbx_control_streamfile(chan, argv[3], fwd, rev, stop, pause, NULL, skipms);
	
	fdprintf(ogi->fd, "200 result=%d\n", res);

	if (res >= 0)
		return RESULT_SUCCESS;
	else
		return RESULT_FAILURE;
}

static int handle_streamfile(struct opbx_channel *chan, OGI *ogi, int argc, char *argv[])
{
	int res;
	struct opbx_filestream *fs;
	long sample_offset = 0;
	long max_length;

	if (argc < 4)
		return RESULT_SHOWUSAGE;
	if (argc > 5)
		return RESULT_SHOWUSAGE;
	if ((argc > 4) && (sscanf(argv[4], "%ld", &sample_offset) != 1))
		return RESULT_SHOWUSAGE;
	
	fs = opbx_openstream(chan, argv[2], chan->language);
	if (!fs){
		fdprintf(ogi->fd, "200 result=%d endpos=%ld\n", 0, sample_offset);
		return RESULT_SUCCESS;
	}
	opbx_seekstream(fs, 0, SEEK_END);
	max_length = opbx_tellstream(fs);
	opbx_seekstream(fs, sample_offset, SEEK_SET);
	res = opbx_applystream(chan, fs);
	res = opbx_playstream(fs);
	if (res) {
		fdprintf(ogi->fd, "200 result=%d endpos=%ld\n", res, sample_offset);
		if (res >= 0)
			return RESULT_SHOWUSAGE;
		else
			return RESULT_FAILURE;
	}
	res = opbx_waitstream_full(chan, argv[3], ogi->audio, ogi->ctrl);
	/* this is to check for if opbx_waitstream closed the stream, we probably are at
	 * the end of the stream, return that amount, else check for the amount */
	sample_offset = (chan->stream) ? opbx_tellstream(fs) : max_length;
	opbx_stopstream(chan);
	if (res == 1) {
		/* Stop this command, don't print a result line, as there is a new command */
		return RESULT_SUCCESS;
	}
	fdprintf(ogi->fd, "200 result=%d endpos=%ld\n", res, sample_offset);
	if (res >= 0)
		return RESULT_SUCCESS;
	else
		return RESULT_FAILURE;
}

/* get option - really similar to the handle_streamfile, but with a timeout */
static int handle_getoption(struct opbx_channel *chan, OGI *ogi, int argc, char *argv[])
{
        int res;
        struct opbx_filestream *fs;
        long sample_offset = 0;
        long max_length;
	int timeout = 0;
	char *edigits = NULL;

	if ( argc < 4 || argc > 5 )
		return RESULT_SHOWUSAGE;

	if ( argv[3] ) 
		edigits = argv[3];

	if ( argc == 5 )
		timeout = atoi(argv[4]);
	else if (chan->pbx->dtimeout) {
		/* by default dtimeout is set to 5sec */
		timeout = chan->pbx->dtimeout * 1000; /* in msec */
	}

        fs = opbx_openstream(chan, argv[2], chan->language);
        if (!fs){
                fdprintf(ogi->fd, "200 result=%d endpos=%ld\n", 0, sample_offset);
                opbx_log(LOG_WARNING, "Unable to open %s\n", argv[2]);
		return RESULT_SUCCESS;
        }
	if (option_verbose > 2)
		opbx_verbose(VERBOSE_PREFIX_3 "Playing '%s' (escape_digits=%s) (timeout %d)\n", argv[2], edigits, timeout);

        opbx_seekstream(fs, 0, SEEK_END);
        max_length = opbx_tellstream(fs);
        opbx_seekstream(fs, sample_offset, SEEK_SET);
        res = opbx_applystream(chan, fs);
        res = opbx_playstream(fs);
        if (res) {
                fdprintf(ogi->fd, "200 result=%d endpos=%ld\n", res, sample_offset);
                if (res >= 0)
                        return RESULT_SHOWUSAGE;
                else
                        return RESULT_FAILURE;
        }
        res = opbx_waitstream_full(chan, argv[3], ogi->audio, ogi->ctrl);
        /* this is to check for if opbx_waitstream closed the stream, we probably are at
         * the end of the stream, return that amount, else check for the amount */
        sample_offset = (chan->stream)?opbx_tellstream(fs):max_length;
        opbx_stopstream(chan);
        if (res == 1) {
                /* Stop this command, don't print a result line, as there is a new command */
                return RESULT_SUCCESS;
        }

	/* If the user didnt press a key, wait for digitTimeout*/
	if (res == 0 ) {
		res = opbx_waitfordigit_full(chan, timeout, ogi->audio, ogi->ctrl);
		/* Make sure the new result is in the escape digits of the GET OPTION */
		if ( !strchr(edigits,res) )
                	res=0;
	}

        fdprintf(ogi->fd, "200 result=%d endpos=%ld\n", res, sample_offset);
        if (res >= 0)
                return RESULT_SUCCESS;
        else
                return RESULT_FAILURE;
}




/*--- handle_saynumber: Say number in various language syntaxes ---*/
/* Need to add option for gender here as well. Coders wanted */
/* While waiting, we're sending a (char *) NULL.  */
static int handle_saynumber(struct opbx_channel *chan, OGI *ogi, int argc, char *argv[])
{
	int res;
	int num;
	if (argc != 4)
		return RESULT_SHOWUSAGE;
	if (sscanf(argv[2], "%d", &num) != 1)
		return RESULT_SHOWUSAGE;
	res = opbx_say_number_full(chan, num, argv[3], chan->language, (char *) NULL, ogi->audio, ogi->ctrl);
	if (res == 1)
		return RESULT_SUCCESS;
	fdprintf(ogi->fd, "200 result=%d\n", res);
	if (res >= 0)
		return RESULT_SUCCESS;
	else
		return RESULT_FAILURE;
}

static int handle_saydigits(struct opbx_channel *chan, OGI *ogi, int argc, char *argv[])
{
	int res;
	int num;

	if (argc != 4)
		return RESULT_SHOWUSAGE;
	if (sscanf(argv[2], "%d", &num) != 1)
		return RESULT_SHOWUSAGE;

	res = opbx_say_digit_str_full(chan, argv[2], argv[3], chan->language, ogi->audio, ogi->ctrl);
	if (res == 1) /* New command */
		return RESULT_SUCCESS;
	fdprintf(ogi->fd, "200 result=%d\n", res);
	if (res >= 0)
		return RESULT_SUCCESS;
	else
		return RESULT_FAILURE;
}

static int handle_sayalpha(struct opbx_channel *chan, OGI *ogi, int argc, char *argv[])
{
	int res;

	if (argc != 4)
		return RESULT_SHOWUSAGE;

	res = opbx_say_character_str_full(chan, argv[2], argv[3], chan->language, ogi->audio, ogi->ctrl);
	if (res == 1) /* New command */
		return RESULT_SUCCESS;
	fdprintf(ogi->fd, "200 result=%d\n", res);
	if (res >= 0)
		return RESULT_SUCCESS;
	else
		return RESULT_FAILURE;
}

static int handle_saydate(struct opbx_channel *chan, OGI *ogi, int argc, char *argv[])
{
	int res;
	int num;
	if (argc != 4)
		return RESULT_SHOWUSAGE;
	if (sscanf(argv[2], "%d", &num) != 1)
		return RESULT_SHOWUSAGE;
	res = opbx_say_date(chan, num, argv[3], chan->language);
	if (res == 1)
		return RESULT_SUCCESS;
	fdprintf(ogi->fd, "200 result=%d\n", res);
	if (res >= 0)
		return RESULT_SUCCESS;
	else
		return RESULT_FAILURE;
}

static int handle_saytime(struct opbx_channel *chan, OGI *ogi, int argc, char *argv[])
{
	int res;
	int num;
	if (argc != 4)
		return RESULT_SHOWUSAGE;
	if (sscanf(argv[2], "%d", &num) != 1)
		return RESULT_SHOWUSAGE;
	res = opbx_say_time(chan, num, argv[3], chan->language);
	if (res == 1)
		return RESULT_SUCCESS;
	fdprintf(ogi->fd, "200 result=%d\n", res);
	if (res >= 0)
		return RESULT_SUCCESS;
	else
		return RESULT_FAILURE;
}

static int handle_saydatetime(struct opbx_channel *chan, OGI *ogi, int argc, char *argv[])
{
	int res=0;
	long unixtime;
	char *format, *zone=NULL;
	
	if (argc < 4)
		return RESULT_SHOWUSAGE;

	if (argc > 4) {
		format = argv[4];
	} else {
		if (!strcasecmp(chan->language, "de")) {
			format = "A dBY HMS";
		} else {
			format = "ABdY 'digits/at' IMp"; 
		}
	}

	if (argc > 5 && !opbx_strlen_zero(argv[5]))
		zone = argv[5];

	if (sscanf(argv[2], "%ld", &unixtime) != 1)
		return RESULT_SHOWUSAGE;

	res = opbx_say_date_with_format(chan, (time_t) unixtime, argv[3], chan->language, format, zone);
	if (res == 1)
		return RESULT_SUCCESS;

	fdprintf(ogi->fd, "200 result=%d\n", res);

	if (res >= 0)
		return RESULT_SUCCESS;
	else
		return RESULT_FAILURE;
}

static int handle_sayphonetic(struct opbx_channel *chan, OGI *ogi, int argc, char *argv[])
{
	int res;

	if (argc != 4)
		return RESULT_SHOWUSAGE;

	res = opbx_say_phonetic_str_full(chan, argv[2], argv[3], chan->language, ogi->audio, ogi->ctrl);
	if (res == 1) /* New command */
		return RESULT_SUCCESS;
	fdprintf(ogi->fd, "200 result=%d\n", res);
	if (res >= 0)
		return RESULT_SUCCESS;
	else
		return RESULT_FAILURE;
}

static int handle_getdata(struct opbx_channel *chan, OGI *ogi, int argc, char *argv[])
{
	int res;
	char data[1024];
	int max;
	int timeout;

	if (argc < 3)
		return RESULT_SHOWUSAGE;
	if (argc >= 4)
		timeout = atoi(argv[3]); 
	else
		timeout = 0;
	if (argc >= 5) 
		max = atoi(argv[4]); 
	else
		max = 1024;
	res = opbx_app_getdata_full(chan, argv[2], data, max, timeout, ogi->audio, ogi->ctrl);
	if (res == 2)			/* New command */
		return RESULT_SUCCESS;
	else if (res == 1)
		fdprintf(ogi->fd, "200 result=%s (timeout)\n", data);
	else if (res < 0 )
		fdprintf(ogi->fd, "200 result=-1\n");
	else
		fdprintf(ogi->fd, "200 result=%s\n", data);
	return RESULT_SUCCESS;
}

static int handle_setcontext(struct opbx_channel *chan, OGI *ogi, int argc, char *argv[])
{

	if (argc != 3)
		return RESULT_SHOWUSAGE;
	opbx_copy_string(chan->context, argv[2], sizeof(chan->context));
	fdprintf(ogi->fd, "200 result=0\n");
	return RESULT_SUCCESS;
}
	
static int handle_setextension(struct opbx_channel *chan, OGI *ogi, int argc, char **argv)
{
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	opbx_copy_string(chan->exten, argv[2], sizeof(chan->exten));
	fdprintf(ogi->fd, "200 result=0\n");
	return RESULT_SUCCESS;
}

static int handle_setpriority(struct opbx_channel *chan, OGI *ogi, int argc, char **argv)
{
	int pri;
	if (argc != 3)
		return RESULT_SHOWUSAGE;	

	if (sscanf(argv[2], "%d", &pri) != 1) {
		if ((pri = opbx_findlabel_extension(chan, chan->context, chan->exten, argv[2], chan->cid.cid_num)) < 1)
			return RESULT_SHOWUSAGE;
	}

	opbx_explicit_goto(chan, NULL, NULL, pri);
	fdprintf(ogi->fd, "200 result=0\n");
	return RESULT_SUCCESS;
}
		
static int handle_recordfile(struct opbx_channel *chan, OGI *ogi, int argc, char *argv[])
{
	struct opbx_filestream *fs;
	struct opbx_frame *f;
	struct timeval start;
	long sample_offset = 0;
	int res = 0;
	int ms;

        struct opbx_dsp *sildet=NULL;         /* silence detector dsp */
        int totalsilence = 0;
        int dspsilence = 0;
        int silence = 0;                /* amount of silence to allow */
        int gotsilence = 0;             /* did we timeout for silence? */
        char *silencestr=NULL;
        int rfmt=0;


	/* XXX EOGI FIXME XXX */

	if (argc < 6)
		return RESULT_SHOWUSAGE;
	if (sscanf(argv[5], "%d", &ms) != 1)
		return RESULT_SHOWUSAGE;

	if (argc > 6)
		silencestr = strchr(argv[6],'s');
	if ((argc > 7) && (!silencestr))
		silencestr = strchr(argv[7],'s');
	if ((argc > 8) && (!silencestr))
		silencestr = strchr(argv[8],'s');

	if (silencestr) {
		if (strlen(silencestr) > 2) {
			if ((silencestr[0] == 's') && (silencestr[1] == '=')) {
				silencestr++;
				silencestr++;
				if (silencestr)
	                		silence = atoi(silencestr);
        			if (silence > 0)
	                		silence *= 1000;
        		}
		}
	}

        if (silence > 0) {
        	rfmt = chan->readformat;
                res = opbx_set_read_format(chan, OPBX_FORMAT_SLINEAR);
                if (res < 0) {
                	opbx_log(LOG_WARNING, "Unable to set to linear mode, giving up\n");
                        return -1;
                }
               	sildet = opbx_dsp_new();
                if (!sildet) {
                	opbx_log(LOG_WARNING, "Unable to create silence detector :(\n");
                        return -1;
                }
               	opbx_dsp_set_threshold(sildet, 256);
      	}

	/* backward compatibility, if no offset given, arg[6] would have been
	 * caught below and taken to be a beep, else if it is a digit then it is a
	 * offset */
	if ((argc >6) && (sscanf(argv[6], "%ld", &sample_offset) != 1) && (!strchr(argv[6], '=')))
		res = opbx_streamfile(chan, "beep", chan->language);

	if ((argc > 7) && (!strchr(argv[7], '=')))
		res = opbx_streamfile(chan, "beep", chan->language);

	if (!res)
		res = opbx_waitstream(chan, argv[4]);
	if (res) {
		fdprintf(ogi->fd, "200 result=%d (randomerror) endpos=%ld\n", res, sample_offset);
	} else {
		fs = opbx_writefile(argv[2], argv[3], NULL, O_CREAT | O_WRONLY | (sample_offset ? O_APPEND : 0), 0, 0644);
		if (!fs) {
			res = -1;
			fdprintf(ogi->fd, "200 result=%d (writefile)\n", res);
			if (sildet)
				opbx_dsp_free(sildet);
			return RESULT_FAILURE;
		}
		
		chan->stream = fs;
		opbx_applystream(chan,fs);
		/* really should have checks */
		opbx_seekstream(fs, sample_offset, SEEK_SET);
		opbx_truncstream(fs);
		
		start = opbx_tvnow();
		while ((ms < 0) || opbx_tvdiff_ms(opbx_tvnow(), start) < ms) {
			res = opbx_waitfor(chan, -1);
			if (res < 0) {
				opbx_closestream(fs);
				fdprintf(ogi->fd, "200 result=%d (waitfor) endpos=%ld\n", res,sample_offset);
				if (sildet)
					opbx_dsp_free(sildet);
				return RESULT_FAILURE;
			}
			f = opbx_read(chan);
			if (!f) {
				fdprintf(ogi->fd, "200 result=%d (hangup) endpos=%ld\n", 0, sample_offset);
				opbx_closestream(fs);
				if (sildet)
					opbx_dsp_free(sildet);
				return RESULT_FAILURE;
			}
			switch(f->frametype) {
			case OPBX_FRAME_DTMF:
				if (strchr(argv[4], f->subclass)) {
					/* This is an interrupting chracter, so rewind to chop off any small
					   amount of DTMF that may have been recorded
					*/
					opbx_stream_rewind(fs, 200);
					opbx_truncstream(fs);
					sample_offset = opbx_tellstream(fs);
					fdprintf(ogi->fd, "200 result=%d (dtmf) endpos=%ld\n", f->subclass, sample_offset);
					opbx_closestream(fs);
					opbx_frfree(f);
					if (sildet)
						opbx_dsp_free(sildet);
					return RESULT_SUCCESS;
				}
				break;
			case OPBX_FRAME_VOICE:
				opbx_writestream(fs, f);
				/* this is a safe place to check progress since we know that fs
				 * is valid after a write, and it will then have our current
				 * location */
				sample_offset = opbx_tellstream(fs);
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
				break;
			}
			opbx_frfree(f);
			if (gotsilence)
				break;
        	}

              	if (gotsilence) {
                     	opbx_stream_rewind(fs, silence-1000);
                	opbx_truncstream(fs);
			sample_offset = opbx_tellstream(fs);
		}		
		fdprintf(ogi->fd, "200 result=%d (timeout) endpos=%ld\n", res, sample_offset);
		opbx_closestream(fs);
	}

        if (silence > 0) {
                res = opbx_set_read_format(chan, rfmt);
                if (res)
                        opbx_log(LOG_WARNING, "Unable to restore read format on '%s'\n", chan->name);
                opbx_dsp_free(sildet);
        }
	return RESULT_SUCCESS;
}

static int handle_autohangup(struct opbx_channel *chan, OGI *ogi, int argc, char *argv[])
{
	int timeout;

	if (argc != 3)
		return RESULT_SHOWUSAGE;
	if (sscanf(argv[2], "%d", &timeout) != 1)
		return RESULT_SHOWUSAGE;
	if (timeout < 0)
		timeout = 0;
	if (timeout)
		chan->whentohangup = time(NULL) + timeout;
	else
		chan->whentohangup = 0;
	fdprintf(ogi->fd, "200 result=0\n");
	return RESULT_SUCCESS;
}

static int handle_hangup(struct opbx_channel *chan, OGI *ogi, int argc, char **argv)
{
	struct opbx_channel *c;
	if (argc == 1) {
		/* no argument: hangup the current channel */
		opbx_softhangup(chan,OPBX_SOFTHANGUP_EXPLICIT);
		fdprintf(ogi->fd, "200 result=1\n");
		return RESULT_SUCCESS;
	} else if (argc == 2) {
		/* one argument: look for info on the specified channel */
		c = opbx_get_channel_by_name_locked(argv[1]);
		if (c) {
			/* we have a matching channel */
			opbx_softhangup(c,OPBX_SOFTHANGUP_EXPLICIT);
			fdprintf(ogi->fd, "200 result=1\n");
			opbx_mutex_unlock(&c->lock);
			return RESULT_SUCCESS;
		}
		/* if we get this far no channel name matched the argument given */
		fdprintf(ogi->fd, "200 result=-1\n");
		return RESULT_SUCCESS;
	} else {
		return RESULT_SHOWUSAGE;
	}
}

static int handle_exec(struct opbx_channel *chan, OGI *ogi, int argc, char **argv)
{
	int res;
	struct opbx_app *app;

	if (argc < 2)
		return RESULT_SHOWUSAGE;

	if (option_verbose > 2)
		opbx_verbose(VERBOSE_PREFIX_3 "OGI Script Executing Application: (%s) Options: (%s)\n", argv[1], argv[2]);

	app = pbx_findapp(argv[1]);

	if (app) {
		res = pbx_exec(chan, app, argv[2], 1);
	} else {
		opbx_log(LOG_WARNING, "Could not find application (%s)\n", argv[1]);
		res = -2;
	}
	fdprintf(ogi->fd, "200 result=%d\n", res);

	return res;
}

static int handle_setcallerid(struct opbx_channel *chan, OGI *ogi, int argc, char **argv)
{
	char tmp[256]="";
	char *l = NULL, *n = NULL;

	if (argv[2]) {
		opbx_copy_string(tmp, argv[2], sizeof(tmp));
		opbx_callerid_parse(tmp, &n, &l);
		if (l)
			opbx_shrink_phone_number(l);
		else
			l = "";
		if (!n)
			n = "";
		opbx_set_callerid(chan, l, n, NULL);
	}

	fdprintf(ogi->fd, "200 result=1\n");
	return RESULT_SUCCESS;
}

static int handle_channelstatus(struct opbx_channel *chan, OGI *ogi, int argc, char **argv)
{
	struct opbx_channel *c;
	if (argc == 2) {
		/* no argument: supply info on the current channel */
		fdprintf(ogi->fd, "200 result=%d\n", chan->_state);
		return RESULT_SUCCESS;
	} else if (argc == 3) {
		/* one argument: look for info on the specified channel */
		c = opbx_get_channel_by_name_locked(argv[2]);
		if (c) {
			fdprintf(ogi->fd, "200 result=%d\n", c->_state);
			opbx_mutex_unlock(&c->lock);
			return RESULT_SUCCESS;
		}
		/* if we get this far no channel name matched the argument given */
		fdprintf(ogi->fd, "200 result=-1\n");
		return RESULT_SUCCESS;
	} else {
		return RESULT_SHOWUSAGE;
	}
}

static int handle_setvariable(struct opbx_channel *chan, OGI *ogi, int argc, char **argv)
{
	if (argv[3])
		pbx_builtin_setvar_helper(chan, argv[2], argv[3]);

	fdprintf(ogi->fd, "200 result=1\n");
	return RESULT_SUCCESS;
}

static int handle_getvariable(struct opbx_channel *chan, OGI *ogi, int argc, char **argv)
{
	char *ret;
	char tempstr[1024];

	if (argc != 3)
		return RESULT_SHOWUSAGE;

	/* check if we want to execute an opbx_custom_function */
	if (!opbx_strlen_zero(argv[2]) && (argv[2][strlen(argv[2]) - 1] == ')')) {
		ret = opbx_func_read(chan, argv[2], tempstr, sizeof(tempstr));
	} else {
		pbx_retrieve_variable(chan, argv[2], &ret, tempstr, sizeof(tempstr), NULL);
	}

	if (ret)
		fdprintf(ogi->fd, "200 result=1 (%s)\n", ret);
	else
		fdprintf(ogi->fd, "200 result=0\n");

	return RESULT_SUCCESS;
}

static int handle_getvariablefull(struct opbx_channel *chan, OGI *ogi, int argc, char **argv)
{
	char tmp[4096];
	struct opbx_channel *chan2=NULL;

	if ((argc != 4) && (argc != 5))
		return RESULT_SHOWUSAGE;
	if (argc == 5) {
		chan2 = opbx_get_channel_by_name_locked(argv[4]);
	} else {
		chan2 = chan;
	}
	if (chan) { /* XXX isn't this chan2 ? */
		pbx_substitute_variables_helper(chan2, argv[3], tmp, sizeof(tmp) - 1);
		fdprintf(ogi->fd, "200 result=1 (%s)\n", tmp);
	} else {
		fdprintf(ogi->fd, "200 result=0\n");
	}
	if (chan2 && (chan2 != chan))
		opbx_mutex_unlock(&chan2->lock);
	return RESULT_SUCCESS;
}

static int handle_verbose(struct opbx_channel *chan, OGI *ogi, int argc, char **argv)
{
	int level = 0;
	char *prefix;

	if (argc < 2)
		return RESULT_SHOWUSAGE;

	if (argv[2])
		sscanf(argv[2], "%d", &level);

	switch (level) {
		case 4:
			prefix = VERBOSE_PREFIX_4;
			break;
		case 3:
			prefix = VERBOSE_PREFIX_3;
			break;
		case 2:
			prefix = VERBOSE_PREFIX_2;
			break;
		case 1:
		default:
			prefix = VERBOSE_PREFIX_1;
			break;
	}

	if (level <= option_verbose)
		opbx_verbose("%s %s: %s\n", prefix, chan->data, argv[1]);
	
	fdprintf(ogi->fd, "200 result=1\n");
	
	return RESULT_SUCCESS;
}

static int handle_dbget(struct opbx_channel *chan, OGI *ogi, int argc, char **argv)
{
	int res;
	char tmp[256];

	if (argc != 4)
		return RESULT_SHOWUSAGE;
	res = opbx_db_get(argv[2], argv[3], tmp, sizeof(tmp));
	if (res) 
		fdprintf(ogi->fd, "200 result=0\n");
	else
		fdprintf(ogi->fd, "200 result=1 (%s)\n", tmp);

	return RESULT_SUCCESS;
}

static int handle_dbput(struct opbx_channel *chan, OGI *ogi, int argc, char **argv)
{
	int res;

	if (argc != 5)
		return RESULT_SHOWUSAGE;
	res = opbx_db_put(argv[2], argv[3], argv[4]);
	if (res) 
		fdprintf(ogi->fd, "200 result=0\n");
	else
		fdprintf(ogi->fd, "200 result=1\n");

	return RESULT_SUCCESS;
}

static int handle_dbdel(struct opbx_channel *chan, OGI *ogi, int argc, char **argv)
{
	int res;

	if (argc != 4)
		return RESULT_SHOWUSAGE;
	res = opbx_db_del(argv[2], argv[3]);
	if (res) 
		fdprintf(ogi->fd, "200 result=0\n");
	else
		fdprintf(ogi->fd, "200 result=1\n");

	return RESULT_SUCCESS;
}

static int handle_dbdeltree(struct opbx_channel *chan, OGI *ogi, int argc, char **argv)
{
	int res;
	if ((argc < 3) || (argc > 4))
		return RESULT_SHOWUSAGE;
	if (argc == 4)
		res = opbx_db_deltree(argv[2], argv[3]);
	else
		res = opbx_db_deltree(argv[2], NULL);

	if (res) 
		fdprintf(ogi->fd, "200 result=0\n");
	else
		fdprintf(ogi->fd, "200 result=1\n");
	return RESULT_SUCCESS;
}

static char debug_usage[] = 
"Usage: ogi debug\n"
"       Enables dumping of OGI transactions for debugging purposes\n";

static char no_debug_usage[] = 
"Usage: ogi no debug\n"
"       Disables dumping of OGI transactions for debugging purposes\n";

static int ogi_do_debug(int fd, int argc, char *argv[])
{
	if (argc != 2)
		return RESULT_SHOWUSAGE;
	ogidebug = 1;
	opbx_cli(fd, "OGI Debugging Enabled\n");
	return RESULT_SUCCESS;
}

static int ogi_no_debug(int fd, int argc, char *argv[])
{
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	ogidebug = 0;
	opbx_cli(fd, "OGI Debugging Disabled\n");
	return RESULT_SUCCESS;
}

static struct opbx_cli_entry  cli_debug =
	{ { "ogi", "debug", NULL }, ogi_do_debug, "Enable OGI debugging", debug_usage };

static struct opbx_cli_entry  cli_no_debug =
	{ { "ogi", "no", "debug", NULL }, ogi_no_debug, "Disable OGI debugging", no_debug_usage };

static int handle_noop(struct opbx_channel *chan, OGI *ogi, int arg, char *argv[])
{
	fdprintf(ogi->fd, "200 result=0\n");
	return RESULT_SUCCESS;
}

static int handle_setmusic(struct opbx_channel *chan, OGI *ogi, int argc, char *argv[])
{
	if (!strncasecmp(argv[2],"on",2)) {
		if (argc > 3)
			opbx_moh_start(chan, argv[3]);
		else
			opbx_moh_start(chan, NULL);
	}
	if (!strncasecmp(argv[2],"off",3)) {
		opbx_moh_stop(chan);
	}
	fdprintf(ogi->fd, "200 result=0\n");
	return RESULT_SUCCESS;
}

static char usage_setmusic[] =
" Usage: SET MUSIC ON <on|off> <class>\n"
"	Enables/Disables the music on hold generator.  If <class> is\n"
" not specified, then the default music on hold class will be used.\n"
" Always returns 0.\n";

static char usage_dbput[] =
" Usage: DATABASE PUT <family> <key> <value>\n"
"	Adds or updates an entry in the OpenPBX database for a\n"
" given family, key, and value.\n"
" Returns 1 if successful, 0 otherwise.\n";

static char usage_dbget[] =
" Usage: DATABASE GET <family> <key>\n"
"	Retrieves an entry in the OpenPBX database for a\n"
" given family and key.\n"
" Returns 0 if <key> is not set.  Returns 1 if <key>\n"
" is set and returns the variable in parentheses.\n"
" Example return code: 200 result=1 (testvariable)\n";

static char usage_dbdel[] =
" Usage: DATABASE DEL <family> <key>\n"
"	Deletes an entry in the OpenPBX database for a\n"
" given family and key.\n"
" Returns 1 if successful, 0 otherwise.\n";

static char usage_dbdeltree[] =
" Usage: DATABASE DELTREE <family> [keytree]\n"
"	Deletes a family or specific keytree within a family\n"
" in the OpenPBX database.\n"
" Returns 1 if successful, 0 otherwise.\n";

static char usage_verbose[] =
" Usage: VERBOSE <message> <level>\n"
"	Sends <message> to the console via verbose message system.\n"
" <level> is the the verbose level (1-4)\n"
" Always returns 1.\n";

static char usage_getvariable[] =
" Usage: GET VARIABLE <variablename>\n"
"	Returns 0 if <variablename> is not set.  Returns 1 if <variablename>\n"
" is set and returns the variable in parentheses.\n"
" example return code: 200 result=1 (testvariable)\n";

static char usage_getvariablefull[] =
" Usage: GET FULL VARIABLE <variablename> [<channel name>]\n"
"	Returns 0 if <variablename> is not set or channel does not exist.  Returns 1\n"
"if <variablename>  is set and returns the variable in parenthesis.  Understands\n"
"complex variable names and builtin variables, unlike GET VARIABLE.\n"
" example return code: 200 result=1 (testvariable)\n";

static char usage_setvariable[] =
" Usage: SET VARIABLE <variablename> <value>\n";

static char usage_channelstatus[] =
" Usage: CHANNEL STATUS [<channelname>]\n"
"	Returns the status of the specified channel.\n" 
" If no channel name is given the returns the status of the\n"
" current channel.  Return values:\n"
"  0 Channel is down and available\n"
"  1 Channel is down, but reserved\n"
"  2 Channel is off hook\n"
"  3 Digits (or equivalent) have been dialed\n"
"  4 Line is ringing\n"
"  5 Remote end is ringing\n"
"  6 Line is up\n"
"  7 Line is busy\n";

static char usage_setcallerid[] =
" Usage: SET CALLERID <number>\n"
"	Changes the callerid of the current channel.\n";

static char usage_exec[] =
" Usage: EXEC <application> <options>\n"
"	Executes <application> with given <options>.\n"
" Returns whatever the application returns, or -2 on failure to find application\n";

static char usage_hangup[] =
" Usage: HANGUP [<channelname>]\n"
"	Hangs up the specified channel.\n"
" If no channel name is given, hangs up the current channel\n";

static char usage_answer[] = 
" Usage: ANSWER\n"
"	Answers channel if not already in answer state. Returns -1 on\n"
" channel failure, or 0 if successful.\n";

static char usage_waitfordigit[] = 
" Usage: WAIT FOR DIGIT <timeout>\n"
"	Waits up to 'timeout' milliseconds for channel to receive a DTMF digit.\n"
" Returns -1 on channel failure, 0 if no digit is received in the timeout, or\n"
" the numerical value of the ascii of the digit if one is received.  Use -1\n"
" for the timeout value if you desire the call to block indefinitely.\n";

static char usage_sendtext[] =
" Usage: SEND TEXT \"<text to send>\"\n"
"	Sends the given text on a channel. Most channels do not support the\n"
" transmission of text.  Returns 0 if text is sent, or if the channel does not\n"
" support text transmission.  Returns -1 only on error/hangup.  Text\n"
" consisting of greater than one word should be placed in quotes since the\n"
" command only accepts a single argument.\n";

static char usage_recvchar[] =
" Usage: RECEIVE CHAR <timeout>\n"
"	Receives a character of text on a channel. Specify timeout to be the\n"
" maximum time to wait for input in milliseconds, or 0 for infinite. Most channels\n"
" do not support the reception of text. Returns the decimal value of the character\n"
" if one is received, or 0 if the channel does not support text reception.  Returns\n"
" -1 only on error/hangup.\n";

static char usage_recvtext[] =
" Usage: RECEIVE TEXT <timeout>\n"
"	Receives a string of text on a channel. Specify timeout to be the\n"
" maximum time to wait for input in milliseconds, or 0 for infinite. Most channels\n"
" do not support the reception of text. Returns -1 for failure or 1 for success, and the string in parentheses.\n";

static char usage_tddmode[] =
" Usage: TDD MODE <on|off>\n"
"	Enable/Disable TDD transmission/reception on a channel. Returns 1 if\n"
" successful, or 0 if channel is not TDD-capable.\n";

static char usage_sendimage[] =
" Usage: SEND IMAGE <image>\n"
"	Sends the given image on a channel. Most channels do not support the\n"
" transmission of images. Returns 0 if image is sent, or if the channel does not\n"
" support image transmission.  Returns -1 only on error/hangup. Image names\n"
" should not include extensions.\n";

static char usage_streamfile[] =
" Usage: STREAM FILE <filename> <escape digits> [sample offset]\n"
"	Send the given file, allowing playback to be interrupted by the given\n"
" digits, if any. Use double quotes for the digits if you wish none to be\n"
" permitted. If sample offset is provided then the audio will seek to sample\n"
" offset before play starts.  Returns 0 if playback completes without a digit\n"
" being pressed, or the ASCII numerical value of the digit if one was pressed,\n"
" or -1 on error or if the channel was disconnected. Remember, the file\n"
" extension must not be included in the filename.\n";

static char usage_controlstreamfile[] =
" Usage: CONTROL STREAM FILE <filename> <escape digits> [skipms] [ffchar] [rewchr] [pausechr]\n"
"	Send the given file, allowing playback to be controled by the given\n"
" digits, if any. Use double quotes for the digits if you wish none to be\n"
" permitted.  Returns 0 if playback completes without a digit\n"
" being pressed, or the ASCII numerical value of the digit if one was pressed,\n"
" or -1 on error or if the channel was disconnected. Remember, the file\n"
" extension must not be included in the filename.\n\n"
" Note: ffchar and rewchar default to * and # respectively.\n";

static char usage_getoption[] = 
" Usage: GET OPTION <filename> <escape_digits> [timeout]\n"
"	Behaves similar to STREAM FILE but used with a timeout option.\n";

static char usage_saynumber[] =
" Usage: SAY NUMBER <number> <escape digits>\n"
"	Say a given number, returning early if any of the given DTMF digits\n"
" are received on the channel.  Returns 0 if playback completes without a digit\n"
" being pressed, or the ASCII numerical value of the digit if one was pressed or\n"
" -1 on error/hangup.\n";

static char usage_saydigits[] =
" Usage: SAY DIGITS <number> <escape digits>\n"
"	Say a given digit string, returning early if any of the given DTMF digits\n"
" are received on the channel. Returns 0 if playback completes without a digit\n"
" being pressed, or the ASCII numerical value of the digit if one was pressed or\n"
" -1 on error/hangup.\n";

static char usage_sayalpha[] =
" Usage: SAY ALPHA <number> <escape digits>\n"
"	Say a given character string, returning early if any of the given DTMF digits\n"
" are received on the channel. Returns 0 if playback completes without a digit\n"
" being pressed, or the ASCII numerical value of the digit if one was pressed or\n"
" -1 on error/hangup.\n";

static char usage_saydate[] =
" Usage: SAY DATE <date> <escape digits>\n"
"	Say a given date, returning early if any of the given DTMF digits are\n"
" received on the channel.  <date> is number of seconds elapsed since 00:00:00\n"
" on January 1, 1970, Coordinated Universal Time (UTC). Returns 0 if playback\n"
" completes without a digit being pressed, or the ASCII numerical value of the\n"
" digit if one was pressed or -1 on error/hangup.\n";

static char usage_saytime[] =
" Usage: SAY TIME <time> <escape digits>\n"
"	Say a given time, returning early if any of the given DTMF digits are\n"
" received on the channel.  <time> is number of seconds elapsed since 00:00:00\n"
" on January 1, 1970, Coordinated Universal Time (UTC). Returns 0 if playback\n"
" completes without a digit being pressed, or the ASCII numerical value of the\n"
" digit if one was pressed or -1 on error/hangup.\n";

static char usage_saydatetime[] =
" Usage: SAY DATETIME <time> <escape digits> [format] [timezone]\n"
"	Say a given time, returning early if any of the given DTMF digits are\n"
" received on the channel.  <time> is number of seconds elapsed since 00:00:00\n"
" on January 1, 1970, Coordinated Universal Time (UTC). [format] is the format\n"
" the time should be said in.  See voicemail.conf (defaults to \"ABdY\n"
" 'digits/at' IMp\").  Acceptable values for [timezone] can be found in\n"
" /usr/share/zoneinfo.  Defaults to machine default. Returns 0 if playback\n"
" completes without a digit being pressed, or the ASCII numerical value of the\n"
" digit if one was pressed or -1 on error/hangup.\n";

static char usage_sayphonetic[] =
" Usage: SAY PHONETIC <string> <escape digits>\n"
"	Say a given character string with phonetics, returning early if any of the\n"
" given DTMF digits are received on the channel. Returns 0 if playback\n"
" completes without a digit pressed, the ASCII numerical value of the digit\n"
" if one was pressed, or -1 on error/hangup.\n";

static char usage_getdata[] =
" Usage: GET DATA <file to be streamed> [timeout] [max digits]\n"
"	Stream the given file, and recieve DTMF data. Returns the digits received\n"
"from the channel at the other end.\n";

static char usage_setcontext[] =
" Usage: SET CONTEXT <desired context>\n"
"	Sets the context for continuation upon exiting the application.\n";

static char usage_setextension[] =
" Usage: SET EXTENSION <new extension>\n"
"	Changes the extension for continuation upon exiting the application.\n";

static char usage_setpriority[] =
" Usage: SET PRIORITY <priority>\n"
"	Changes the priority for continuation upon exiting the application.\n"
" The priority must be a valid priority or label.\n";

static char usage_recordfile[] =
" Usage: RECORD FILE <filename> <format> <escape digits> <timeout> \\\n"
"                                          [offset samples] [BEEP] [s=silence]\n"
"	Record to a file until a given dtmf digit in the sequence is received\n"
" Returns -1 on hangup or error.  The format will specify what kind of file\n"
" will be recorded.  The timeout is the maximum record time in milliseconds, or\n"
" -1 for no timeout. \"Offset samples\" is optional, and, if provided, will seek\n"
" to the offset without exceeding the end of the file.  \"silence\" is the number\n"
" of seconds of silence allowed before the function returns despite the\n"
" lack of dtmf digits or reaching timeout.  Silence value must be\n"
" preceeded by \"s=\" and is also optional.\n";

static char usage_autohangup[] =
" Usage: SET AUTOHANGUP <time>\n"
"	Cause the channel to automatically hangup at <time> seconds in the\n"
" future.  Of course it can be hungup before then as well. Setting to 0 will\n"
" cause the autohangup feature to be disabled on this channel.\n";

static char usage_noop[] =
" Usage: NoOp\n"
"	Does nothing.\n";

static ogi_command commands[MAX_COMMANDS] = {
	{ { "answer", NULL }, handle_answer, "Answer channel", usage_answer },
	{ { "channel", "status", NULL }, handle_channelstatus, "Returns status of the connected channel", usage_channelstatus },
	{ { "database", "del", NULL }, handle_dbdel, "Removes database key/value", usage_dbdel },
	{ { "database", "deltree", NULL }, handle_dbdeltree, "Removes database keytree/value", usage_dbdeltree },
	{ { "database", "get", NULL }, handle_dbget, "Gets database value", usage_dbget },
	{ { "database", "put", NULL }, handle_dbput, "Adds/updates database value", usage_dbput },
	{ { "exec", NULL }, handle_exec, "Executes a given Application", usage_exec },
	{ { "get", "data", NULL }, handle_getdata, "Prompts for DTMF on a channel", usage_getdata },
	{ { "get", "full", "variable", NULL }, handle_getvariablefull, "Evaluates a channel expression", usage_getvariablefull },
	{ { "get", "option", NULL }, handle_getoption, "Stream file, prompt for DTMF, with timeout", usage_getoption },
	{ { "get", "variable", NULL }, handle_getvariable, "Gets a channel variable", usage_getvariable },
	{ { "hangup", NULL }, handle_hangup, "Hangup the current channel", usage_hangup },
	{ { "noop", NULL }, handle_noop, "Does nothing", usage_noop },
	{ { "receive", "char", NULL }, handle_recvchar, "Receives one character from channels supporting it", usage_recvchar },
	{ { "receive", "text", NULL }, handle_recvtext, "Receives text from channels supporting it", usage_recvtext },
	{ { "record", "file", NULL }, handle_recordfile, "Records to a given file", usage_recordfile },
	{ { "say", "alpha", NULL }, handle_sayalpha, "Says a given character string", usage_sayalpha },
	{ { "say", "digits", NULL }, handle_saydigits, "Says a given digit string", usage_saydigits },
	{ { "say", "number", NULL }, handle_saynumber, "Says a given number", usage_saynumber },
	{ { "say", "phonetic", NULL }, handle_sayphonetic, "Says a given character string with phonetics", usage_sayphonetic },
	{ { "say", "date", NULL }, handle_saydate, "Says a given date", usage_saydate },
	{ { "say", "time", NULL }, handle_saytime, "Says a given time", usage_saytime },
	{ { "say", "datetime", NULL }, handle_saydatetime, "Says a given time as specfied by the format given", usage_saydatetime },
	{ { "send", "image", NULL }, handle_sendimage, "Sends images to channels supporting it", usage_sendimage },
	{ { "send", "text", NULL }, handle_sendtext, "Sends text to channels supporting it", usage_sendtext },
	{ { "set", "autohangup", NULL }, handle_autohangup, "Autohangup channel in some time", usage_autohangup },
	{ { "set", "callerid", NULL }, handle_setcallerid, "Sets callerid for the current channel", usage_setcallerid },
	{ { "set", "context", NULL }, handle_setcontext, "Sets channel context", usage_setcontext },
	{ { "set", "extension", NULL }, handle_setextension, "Changes channel extension", usage_setextension },
	{ { "set", "music", NULL }, handle_setmusic, "Enable/Disable Music on hold generator", usage_setmusic },
	{ { "set", "priority", NULL }, handle_setpriority, "Set channel dialplan priority", usage_setpriority },
	{ { "set", "variable", NULL }, handle_setvariable, "Sets a channel variable", usage_setvariable },
	{ { "stream", "file", NULL }, handle_streamfile, "Sends audio file on channel", usage_streamfile },
	{ { "control", "stream", "file", NULL }, handle_controlstreamfile, "Sends audio file on channel and allows the listner to control the stream", usage_controlstreamfile },
	{ { "tdd", "mode", NULL }, handle_tddmode, "Toggles TDD mode (for the deaf)", usage_tddmode },
	{ { "verbose", NULL }, handle_verbose, "Logs a message to the openpbx verbose log", usage_verbose },
	{ { "wait", "for", "digit", NULL }, handle_waitfordigit, "Waits for a digit to be pressed", usage_waitfordigit },
};

static void join(char *s, size_t len, char *w[])
{
	int x;

	/* Join words into a string */
	if (!s) {
		return;
	}
	s[0] = '\0';
	for (x=0; w[x]; x++) {
		if (x)
			strncat(s, " ", len - strlen(s) - 1);
		strncat(s, w[x], len - strlen(s) - 1);
	}
}

static int help_workhorse(int fd, char *match[])
{
	char fullcmd[80];
	char matchstr[80];
	int x;
	struct ogi_command *e;
	if (match)
		join(matchstr, sizeof(matchstr), match);
	for (x=0;x<sizeof(commands)/sizeof(commands[0]);x++) {
		if (!commands[x].cmda[0]) break;
		e = &commands[x]; 
		if (e)
			join(fullcmd, sizeof(fullcmd), e->cmda);
		/* Hide commands that start with '_' */
		if (fullcmd[0] == '_')
			continue;
		if (match) {
			if (strncasecmp(matchstr, fullcmd, strlen(matchstr))) {
				continue;
			}
		}
		opbx_cli(fd, "%20.20s   %s\n", fullcmd, e->summary);
	}
	return 0;
}

int ogi_register(ogi_command *ogi)
{
	int x;
	for (x=0; x<MAX_COMMANDS - 1; x++) {
		if (commands[x].cmda[0] == ogi->cmda[0]) {
			opbx_log(LOG_WARNING, "Command already registered!\n");
			return -1;
		}
	}
	for (x=0; x<MAX_COMMANDS - 1; x++) {
		if (!commands[x].cmda[0]) {
			commands[x] = *ogi;
			return 0;
		}
	}
	opbx_log(LOG_WARNING, "No more room for new commands!\n");
	return -1;
}

void ogi_unregister(ogi_command *ogi)
{
	int x;
	for (x=0; x<MAX_COMMANDS - 1; x++) {
		if (commands[x].cmda[0] == ogi->cmda[0]) {
			memset(&commands[x], 0, sizeof(ogi_command));
		}
	}
}

static ogi_command *find_command(char *cmds[], int exact)
{
	int x;
	int y;
	int match;

	for (x=0; x < sizeof(commands) / sizeof(commands[0]); x++) {
		if (!commands[x].cmda[0])
			break;
		/* start optimistic */
		match = 1;
		for (y=0; match && cmds[y]; y++) {
			/* If there are no more words in the command (and we're looking for
			   an exact match) or there is a difference between the two words,
			   then this is not a match */
			if (!commands[x].cmda[y] && !exact)
				break;
			/* don't segfault if the next part of a command doesn't exist */
			if (!commands[x].cmda[y])
				return NULL;
			if (strcasecmp(commands[x].cmda[y], cmds[y]))
				match = 0;
		}
		/* If more words are needed to complete the command then this is not
		   a candidate (unless we're looking for a really inexact answer  */
		if ((exact > -1) && commands[x].cmda[y])
			match = 0;
		if (match)
			return &commands[x];
	}
	return NULL;
}


static int parse_args(char *s, int *max, char *argv[])
{
	int x=0;
	int quoted=0;
	int escaped=0;
	int whitespace=1;
	char *cur;

	cur = s;
	while(*s) {
		switch(*s) {
		case '"':
			/* If it's escaped, put a literal quote */
			if (escaped) 
				goto normal;
			else 
				quoted = !quoted;
			if (quoted && whitespace) {
				/* If we're starting a quote, coming off white space start a new word, too */
				argv[x++] = cur;
				whitespace=0;
			}
			escaped = 0;
		break;
		case ' ':
		case '\t':
			if (!quoted && !escaped) {
				/* If we're not quoted, mark this as whitespace, and
				   end the previous argument */
				whitespace = 1;
				*(cur++) = '\0';
			} else
				/* Otherwise, just treat it as anything else */ 
				goto normal;
			break;
		case '\\':
			/* If we're escaped, print a literal, otherwise enable escaping */
			if (escaped) {
				goto normal;
			} else {
				escaped=1;
			}
			break;
		default:
normal:
			if (whitespace) {
				if (x >= MAX_ARGS -1) {
					opbx_log(LOG_WARNING, "Too many arguments, truncating\n");
					break;
				}
				/* Coming off of whitespace, start the next argument */
				argv[x++] = cur;
				whitespace=0;
			}
			*(cur++) = *s;
			escaped=0;
		}
		s++;
	}
	/* Null terminate */
	*(cur++) = '\0';
	argv[x] = NULL;
	*max = x;
	return 0;
}

static int ogi_handle_command(struct opbx_channel *chan, OGI *ogi, char *buf)
{
	char *argv[MAX_ARGS];
	int argc = 0;
	int res;
	ogi_command *c;
	argc = MAX_ARGS;

	parse_args(buf, &argc, argv);
	c = find_command(argv, 0);
	if (c) {
		res = c->handler(chan, ogi, argc, argv);
		switch(res) {
		case RESULT_SHOWUSAGE:
			fdprintf(ogi->fd, "520-Invalid command syntax.  Proper usage follows:\n");
			fdprintf(ogi->fd, c->usage);
			fdprintf(ogi->fd, "520 End of proper usage.\n");
			break;
		case OPBX_PBX_KEEPALIVE:
			/* We've been asked to keep alive, so do so */
			return OPBX_PBX_KEEPALIVE;
			break;
		case RESULT_FAILURE:
			/* They've already given the failure.  We've been hung up on so handle this
			   appropriately */
			return -1;
		}
	} else {
		fdprintf(ogi->fd, "510 Invalid or unknown command\n");
	}
	return 0;
}
#define RETRY	3
static int run_ogi(struct opbx_channel *chan, char *request, OGI *ogi, int pid, int dead)
{
	struct opbx_channel *c;
	int outfd;
	int ms;
	int returnstatus = 0;
	struct opbx_frame *f;
	char buf[2048];
	FILE *readf;
	/* how many times we'll retry if opbx_waitfor_nandfs will return without either 
	  channel or file descriptor in case select is interrupted by a system call (EINTR) */
	int retry = RETRY;

	if (!(readf = fdopen(ogi->ctrl, "r"))) {
		opbx_log(LOG_WARNING, "Unable to fdopen file descriptor\n");
		if (pid > -1)
			kill(pid, SIGHUP);
		close(ogi->ctrl);
		return -1;
	}
	setlinebuf(readf);
	setup_env(chan, request, ogi->fd, (ogi->audio > -1));
	for (;;) {
		ms = -1;
		c = opbx_waitfor_nandfds(&chan, dead ? 0 : 1, &ogi->ctrl, 1, NULL, &outfd, &ms);
		if (c) {
			retry = RETRY;
			/* Idle the channel until we get a command */
			f = opbx_read(c);
			if (!f) {
				opbx_log(LOG_DEBUG, "%s hungup\n", chan->name);
				returnstatus = -1;
				break;
			} else {
				/* If it's voice, write it to the audio pipe */
				if ((ogi->audio > -1) && (f->frametype == OPBX_FRAME_VOICE)) {
					/* Write, ignoring errors */
					write(ogi->audio, f->data, f->datalen);
				}
				opbx_frfree(f);
			}
		} else if (outfd > -1) {
			retry = RETRY;
			if (!fgets(buf, sizeof(buf), readf)) {
				/* Program terminated */
				if (returnstatus)
					returnstatus = -1;
				if (option_verbose > 2) 
					opbx_verbose(VERBOSE_PREFIX_3 "OGI Script %s completed, returning %d\n", request, returnstatus);
				/* No need to kill the pid anymore, since they closed us */
				pid = -1;
				break;
			}
			/* get rid of trailing newline, if any */
			if (*buf && buf[strlen(buf) - 1] == '\n')
				buf[strlen(buf) - 1] = 0;
			if (ogidebug)
				opbx_verbose("OGI Rx << %s\n", buf);
			returnstatus |= ogi_handle_command(chan, ogi, buf);
			/* If the handle_command returns -1, we need to stop */
			if ((returnstatus < 0) || (returnstatus == OPBX_PBX_KEEPALIVE)) {
				break;
			}
		} else {
			if (--retry <= 0) {
				opbx_log(LOG_WARNING, "No channel, no fd?\n");
				returnstatus = -1;
				break;
			}
		}
	}
	/* Notify process */
	if (pid > -1) {
		if (kill(pid, SIGHUP))
			opbx_log(LOG_WARNING, "unable to send SIGHUP to OGI process %d: %s\n", pid, strerror(errno));
	}
	fclose(readf);
	return returnstatus;
}

static int handle_showogi(int fd, int argc, char *argv[]) {
	struct ogi_command *e;
	char fullcmd[80];
	if ((argc < 2))
		return RESULT_SHOWUSAGE;
	if (argc > 2) {
		e = find_command(argv + 2, 1);
		if (e) 
			opbx_cli(fd, e->usage);
		else {
			if (find_command(argv + 2, -1)) {
				return help_workhorse(fd, argv + 1);
			} else {
				join(fullcmd, sizeof(fullcmd), argv+1);
				opbx_cli(fd, "No such command '%s'.\n", fullcmd);
			}
		}
	} else {
		return help_workhorse(fd, NULL);
	}
	return RESULT_SUCCESS;
}

static int handle_dumpogihtml(int fd, int argc, char *argv[]) {
	struct ogi_command *e;
	char fullcmd[80];
	char *tempstr;
	int x;
	FILE *htmlfile;

	if ((argc < 3))
		return RESULT_SHOWUSAGE;

	if (!(htmlfile = fopen(argv[2], "wt"))) {
		opbx_cli(fd, "Could not create file '%s'\n", argv[2]);
		return RESULT_SHOWUSAGE;
	}

	fprintf(htmlfile, "<HTML>\n<HEAD>\n<TITLE>OGI Commands</TITLE>\n</HEAD>\n");
	fprintf(htmlfile, "<BODY>\n<CENTER><B><H1>OGI Commands</H1></B></CENTER>\n\n");


	fprintf(htmlfile, "<TABLE BORDER=\"0\" CELLSPACING=\"10\">\n");

	for (x=0;x<sizeof(commands)/sizeof(commands[0]);x++) {
		char *stringp=NULL;
		if (!commands[x].cmda[0]) break;
		e = &commands[x]; 
		if (e)
			join(fullcmd, sizeof(fullcmd), e->cmda);
		/* Hide commands that start with '_' */
		if (fullcmd[0] == '_')
			continue;

		fprintf(htmlfile, "<TR><TD><TABLE BORDER=\"1\" CELLPADDING=\"5\" WIDTH=\"100%%\">\n");
		fprintf(htmlfile, "<TR><TH ALIGN=\"CENTER\"><B>%s - %s</B></TD></TR>\n", fullcmd,e->summary);


		stringp=e->usage;
		tempstr = strsep(&stringp, "\n");

		fprintf(htmlfile, "<TR><TD ALIGN=\"CENTER\">%s</TD></TR>\n", tempstr);
		
		fprintf(htmlfile, "<TR><TD ALIGN=\"CENTER\">\n");
		while ((tempstr = strsep(&stringp, "\n")) != NULL) {
		fprintf(htmlfile, "%s<BR>\n",tempstr);

		}
		fprintf(htmlfile, "</TD></TR>\n");
		fprintf(htmlfile, "</TABLE></TD></TR>\n\n");

	}

	fprintf(htmlfile, "</TABLE>\n</BODY>\n</HTML>\n");
	fclose(htmlfile);
	opbx_cli(fd, "OGI HTML Commands Dumped to: %s\n", argv[2]);
	return RESULT_SUCCESS;
}

static int ogi_exec_full(struct opbx_channel *chan, void *data, int enhanced, int dead)
{
	int res=0;
	struct localuser *u;
	char *argv[MAX_ARGS];
	char buf[2048]="";
	char *tmp = (char *)buf;
	int argc = 0;
	int fds[2];
	int efd = -1;
	int pid;
        char *stringp;
	OGI ogi;

	if (!data || opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "OGI requires an argument (script)\n");
		return -1;
	}
	opbx_copy_string(buf, data, sizeof(buf));

	memset(&ogi, 0, sizeof(ogi));
        while ((stringp = strsep(&tmp, "|"))) {
		argv[argc++] = stringp;
        }
	argv[argc] = NULL;

	LOCAL_USER_ADD(u);
#if 0
	 /* Answer if need be */
        if (chan->_state != OPBX_STATE_UP) {
		if (opbx_answer(chan)) {
			LOCAL_USER_REMOVE(u);
			return -1;
		}
	}
#endif
	res = launch_script(argv[0], argv, fds, enhanced ? &efd : NULL, &pid);
	if (!res) {
		ogi.fd = fds[1];
		ogi.ctrl = fds[0];
		ogi.audio = efd;
		res = run_ogi(chan, argv[0], &ogi, pid, dead);
		if (fds[1] != fds[0])
			close(fds[1]);
		if (efd > -1)
			close(efd);
	}
	LOCAL_USER_REMOVE(u);
	return res;
}

static int ogi_exec(struct opbx_channel *chan, void *data)
{
	if (chan->_softhangup)
		opbx_log(LOG_WARNING, "If you want to run OGI on hungup channels you should use DeadOGI!\n");
	return ogi_exec_full(chan, data, 0, 0);
}

static int eogi_exec(struct opbx_channel *chan, void *data)
{
	int readformat;
	int res;

	if (chan->_softhangup)
		opbx_log(LOG_WARNING, "If you want to run OGI on hungup channels you should use DeadOGI!\n");
	readformat = chan->readformat;
	if (opbx_set_read_format(chan, OPBX_FORMAT_SLINEAR)) {
		opbx_log(LOG_WARNING, "Unable to set channel '%s' to linear mode\n", chan->name);
		return -1;
	}
	res = ogi_exec_full(chan, data, 1, 0);
	if (!res) {
		if (opbx_set_read_format(chan, readformat)) {
			opbx_log(LOG_WARNING, "Unable to restore channel '%s' to format %s\n", chan->name, opbx_getformatname(readformat));
		}
	}
	return res;
}

static int deadogi_exec(struct opbx_channel *chan, void *data)
{
	return ogi_exec_full(chan, data, 0, 1);
}

static char showogi_help[] =
"Usage: show ogi [topic]\n"
"       When called with a topic as an argument, displays usage\n"
"       information on the given command.  If called without a\n"
"       topic, it provides a list of OGI commands.\n";


static char dumpogihtml_help[] =
"Usage: dump ogihtml <filename>\n"
"	Dumps the ogi command list in html format to given filename\n";

static struct opbx_cli_entry showogi = 
{ { "show", "ogi", NULL }, handle_showogi, "Show OGI commands or specific help", showogi_help };

static struct opbx_cli_entry dumpogihtml = 
{ { "dump", "ogihtml", NULL }, handle_dumpogihtml, "Dumps a list of ogi command in html format", dumpogihtml_help };

int unload_module(void)
{
	STANDARD_HANGUP_LOCALUSERS;
	opbx_cli_unregister(&showogi);
	opbx_cli_unregister(&dumpogihtml);
	opbx_cli_unregister(&cli_debug);
	opbx_cli_unregister(&cli_no_debug);
	opbx_unregister_application(eapp);
	opbx_unregister_application(deadapp);
	return opbx_unregister_application(app);
}

int load_module(void)
{
	opbx_cli_register(&showogi);
	opbx_cli_register(&dumpogihtml);
	opbx_cli_register(&cli_debug);
	opbx_cli_register(&cli_no_debug);
	opbx_register_application(deadapp, deadogi_exec, deadsynopsis, descrip);
	opbx_register_application(eapp, eogi_exec, esynopsis, descrip);
	return opbx_register_application(app, ogi_exec, synopsis, descrip);
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
