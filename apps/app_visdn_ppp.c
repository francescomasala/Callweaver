#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/apps/app_visdn_ppp.c $", "$Revision: 4723 $")

#include "callweaver/lock.h"
#include "callweaver/file.h"
#include "callweaver/logger.h"
#include "callweaver/channel.h"
#include "callweaver/pbx.h"
#include "callweaver/module.h"
#include "callweaver/options.h"
#include "callweaver/logger.h"

#include "../channels/chan_visdn.h"

static char *tdesc = "vISDN ppp RAS module";

static void *visdn_ppp_app;
static const char *visdn_ppp_name = "vISDNppp";
static const char *visdn_ppp_synopsis = "Runs pppd and connects channel to visdn-ppp gateway";
static const char *visdn_ppp_syntax = "vISDNppp(args)";
static const char *visdn_ppp_descrip = 
"Executes a RAS server using pppd on the given channel.\n"
"The channel must be a clear channel (i.e. PRI source) and a Zaptel\n"
"channel to be able to use this function (No modem emulation is included).\n"
"Your pppd must be patched to be zaptel aware. Arguments should be\n"
"separated by | characters.  Always returns -1.\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

#define PPP_MAX_ARGS	32
#define PPP_EXEC	"/usr/sbin/pppd"

static int get_max_fds(void)
{
#ifdef OPEN_MAX
	return OPEN_MAX;
#else
	int max;

	max = sysconf(_SC_OPEN_MAX);
	if (max <= 0)
		return 1024;

	return max;
#endif
}

static pid_t spawn_ppp(struct cw_channel *chan, const char *argv[])
{
	/* Start by forking */
	pid_t pid = fork();
	if (pid)
		return pid;

	dup2(chan->fds[0], STDIN_FILENO);

	int i;
	int max_fds = get_max_fds();
	for (i=STDERR_FILENO + 1; i < max_fds; i++)
		close(i);

	/* Restore original signal handlers */
	for (i=0; i<NSIG; i++)
		signal(i, SIG_DFL);

	/* Finally launch PPP */
	execv(PPP_EXEC, (char * const *)argv);
	fprintf(stderr, "Failed to exec pppd!: %s\n", strerror(errno));
	exit(1);
}


static int visdn_ppp_exec(struct cw_channel *chan, int argc, char **argv)
{
	struct visdn_chan *visdn_chan;
	const char **nargv;
	struct localuser *u;
	struct cw_frame *f;
	int res=-1;

	LOCAL_USER_ADD(u);

	if (chan->_state != CW_STATE_UP)
		cw_answer(chan);

	cw_mutex_lock(&chan->lock);

	if (strcmp(chan->type, "VISDN")) {
		cw_log(LOG_WARNING,
			"Only VISDN channels may be connected to"
			" this application\n");

		cw_mutex_unlock(&chan->lock);
		return -1;
	}

	visdn_chan = chan->tech_pvt;

	if (!strlen(visdn_chan->visdn_chanid)) {
		cw_log(LOG_WARNING,
			"vISDN crossconnector channel ID not present\n");
		cw_mutex_unlock(&chan->lock);
		return -1;
	}

	nargv = alloca((2 + argc + 3 + 1) * sizeof(nargv[0]));
	nargv[0] = PPP_EXEC;
	nargv[1] = "nodetach";
	memcpy(nargv + 2, argc, argv * sizeof(argv[0]));
	nargv[2 + argc + 0] = "plugin";
	nargv[2 + argc + 1] = "visdn.so";
	nargv[2 + argc + 2] = visdn_chan->visdn_chanid;
	nargv[2 + argc + 3] = NULL;

	cw_mutex_unlock(&chan->lock);

#if 0
	int i;
	for (i=0;i<argc;i++) {
		cw_log(LOG_NOTICE, "Arg %d: %s\n", i, argv[i]);
	}
#endif

	signal(SIGCHLD, SIG_DFL);

	pid_t pid = spawn_ppp(chan, nargv);
	if (pid < 0) {
		cw_log(LOG_WARNING, "Failed to spawn pppd\n");
		return -1;
	}

	while(cw_waitfor(chan, -1) > -1) {

		f = cw_read(chan);
		if (!f) {
			cw_log(LOG_NOTICE,
				"Channel '%s' hungup."
				" Signalling PPP at %d to die...\n",
				chan->name, pid);

			kill(pid, SIGTERM);

			break;
		}

		cw_fr_free(f);

		int status;
		res = wait4(pid, &status, WNOHANG, NULL);
		if (res < 0) {
			cw_log(LOG_WARNING,
				"wait4 returned %d: %s\n",
				res, strerror(errno));

			break;
		} else if (res > 0) {
			if (option_verbose > 2) {
				if (WIFEXITED(status)) {
					cw_verbose(VERBOSE_PREFIX_3
						"PPP on %s terminated with status %d\n",
						chan->name, WEXITSTATUS(status));
				} else if (WIFSIGNALED(status)) {
					cw_verbose(VERBOSE_PREFIX_3
						"PPP on %s terminated with signal %d\n", 
						chan->name, WTERMSIG(status));
				} else {
					cw_verbose(VERBOSE_PREFIX_3
						"PPP on %s terminated weirdly.\n", chan->name);
				}
			}

			break;
		}
	}

	LOCAL_USER_REMOVE(u);
	return res;
}

int unload_module(void)
{
	int res = 0;
	STANDARD_HANGUP_LOCALUSERS;
	res |= cw_unregister_application(visdn_ppp_app);
	return res;
}

int load_module(void)
{
	visdn_ppp_app = cw_register_application(visdn_ppp_name, visdn_ppp_exec, visdn_ppp_synopsis, visdn_ppp_syntax, visdn_ppp_descrip);
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
