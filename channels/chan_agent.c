/*
 * CallWeaver -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * See http://www.callweaver.org for more information about
 * the CallWeaver project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */


/**
 * Implementation of Agents
 *
 * @file chan_agent.c
 * @brief This file is the implementation of Agents modules.
 * It is a dynamic module that is loaded by CallWeaver. At load time, load_module is run.
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/signal.h>

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/channels/chan_agent.c $", "$Revision: 4723 $")

#include "callweaver/lock.h"
#include "callweaver/channel.h"
#include "callweaver/config.h"
#include "callweaver/logger.h"
#include "callweaver/module.h"
#include "callweaver/pbx.h"
#include "callweaver/options.h"
#include "callweaver/lock.h"
#include "callweaver/sched.h"
#include "callweaver/io.h"
#include "callweaver/acl.h"
#include "callweaver/phone_no_utils.h"
#include "callweaver/file.h"
#include "callweaver/cli.h"
#include "callweaver/app.h"
#include "callweaver/musiconhold.h"
#include "callweaver/manager.h"
#include "callweaver/features.h"
#include "callweaver/utils.h"
#include "callweaver/causes.h"
#include "callweaver/callweaver_db.h"
#include "callweaver/devicestate.h"

static const char desc[] = "Agent Proxy Channel";
static const char channeltype[] = "Agent";
static const char tdesc[] = "Call Agent Proxy Channel";
static const char config[] = "agents.conf";

static void *agentlogin_app;
static void *agentcallbacklogin_app;
static void *agentmonitoroutgoing_app;

static const char app[] = "AgentLogin";
static const char app2[] = "AgentCallbackLogin";
static const char app3[] = "AgentMonitorOutgoing";

static const char syntax[] = "AgentLogin([AgentNo[, options]])";
static const char syntax2[] = "AgentCallbackLogin([AgentNo[, options[, [exten]@context]]])";
static const char syntax3[] = "AgentMonitorOutgoing([options])";

static const char synopsis[] = "Call agent login";
static const char synopsis2[] = "Call agent callback login";
static const char synopsis3[] = "Record agent's outgoing call";

static const char descrip[] =
"Asks the agent to login to the system.  Always returns -1.  While\n"
"logged in, the agent can receive calls and will hear a 'beep'\n"
"when a new call comes in. The agent can dump the call by pressing\n"
"the star key.\n"
"The option string may contain zero or more of the following characters:\n"
"      's' -- silent login - do not announce the login ok segment after agent logged in/off\n";

static const char descrip2[] =
"Asks the agent to login to the system with callback.\n"
"The agent's callback extension is called (optionally with the specified\n"
"context).\n"
"The option string may contain zero or more of the following characters:\n"
"      's' -- silent login - do not announce the login ok segment agent logged in/off\n";

static const char descrip3[] =
"Tries to figure out the id of the agent who is placing outgoing call based on\n"
"comparision of the callerid of the current interface and the global variable \n"
"placed by the AgentCallbackLogin application. That's why it should be used only\n"
"with the AgentCallbackLogin app. Uses the monitoring functions in chan_agent \n"
"instead of Monitor application. That have to be configured in the agents.conf file.\n"
"If callerid or agent id aren't specified, or if other errors occur, set the\n"
"variable AGENTSTATUS to FAIL. Otherwise set this to SUCCESS. Always return 0\n"
"\nOptions:\n"
"	'd' - make the app return -1 if there is an error condition and there is\n"
"	      no extension n+101\n"
"	'c' - change the CDR so that the source of the call is 'Agent/agent_id'\n"
"	'n' - don't generate the warnings when there is no callerid or the\n"
"	      agentid is not known.\n"
"             It's handy if you want to have one context for agent and non-agent calls.\n";

static const char mandescr_agents[] =
"Description: Will list info about all possible agents.\n"
"Variables: NONE\n";

static const char mandescr_agent_logoff[] =
"Description: Sets an agent as no longer logged in.\n"
"Variables: (Names marked with * are required)\n"
"	*Agent: Agent ID of the agent to log off\n"
"	Soft: Set to 'true' to not hangup existing calls\n";

static const char mandescr_agent_callback_login[] =
"Description: Sets an agent as logged in with callback.\n"
"Variables: (Names marked with * are required)\n"
"	*Agent: Agent ID of the agent to login\n"
"	*Exten: Extension to use for callback\n"
"	Context: Context to use for callback\n"
"	AckCall: Set to 'true' to require an acknowledgement by '#' when agent is called back\n"
"	WrapupTime: the minimum amount of time after disconnecting before the caller can receive a new call\n";

static char moh[80] = "default";

#define CW_MAX_AGENT	80		/**< Agent ID or Password max length */
#define CW_MAX_BUF	256
#define CW_MAX_FILENAME_LEN	256

/** Persistent Agents cwdb family */
static const char pa_family[] = "/Agents";
/** The maximum lengh of each persistent member agent database entry */
#define PA_MAX_LEN 2048
/** queues.conf [general] option */
static int persistent_agents = 0;
static void dump_agents(void);

static cw_group_t group;
static int autologoff;
static int wrapuptime;
static int ackcall;

static int maxlogintries = 3;
static char agentgoodbye[CW_MAX_FILENAME_LEN] = "vm-goodbye";

static int usecnt =0;
CW_MUTEX_DEFINE_STATIC(usecnt_lock);

/* Protect the interface list (of pvt's) */
CW_MUTEX_DEFINE_STATIC(agentlock);

static int recordagentcalls = 0;
static char recordformat[CW_MAX_BUF] = "";
static char recordformatext[CW_MAX_BUF] = "";
static int createlink = 0;
static char urlprefix[CW_MAX_BUF] = "";
static char savecallsin[CW_MAX_BUF] = "";
static int updatecdr = 0;
static char beep[CW_MAX_BUF] = "beep";

#define GETAGENTBYCALLERID	"AGENTBYCALLERID"

/**
 * Structure representing an agent.
 */
struct agent_pvt {
	cw_mutex_t lock;              /**< Channel private lock */
	int dead;                      /**< Poised for destruction? */
	int pending;                   /**< Not a real agent -- just pending a match */
	int abouttograb;               /**< About to grab */
	int autologoff;                /**< Auto timeout time */
	int ackcall;                   /**< ackcall */
	time_t loginstart;             /**< When agent first logged in (0 when logged off) */
	time_t start;                  /**< When call started */
	struct timeval lastdisc;       /**< When last disconnected */
	int wrapuptime;                /**< Wrapup time in ms */
	cw_group_t group;             /**< Group memberships */
	int acknowledged;              /**< Acknowledged */
	char moh[80];                  /**< Which music on hold */
	char agent[CW_MAX_AGENT];     /**< Agent ID */
	char password[CW_MAX_AGENT];  /**< Password for Agent login */
	char name[CW_MAX_AGENT];
	cw_mutex_t app_lock;          /**< Synchronization between owning applications */
	volatile pthread_t owning_app; /**< Owning application thread id */
	volatile int app_sleep_cond;   /**< Sleep condition for the login app */
	struct cw_channel *owner;     /**< Agent */
	char loginchan[80];            /**< channel they logged in from */
	char logincallerid[80];        /**< Caller ID they had when they logged in */
	struct cw_channel *chan;      /**< Channel we use */
	struct agent_pvt *next;        /**< Next Agent in the linked list. */
};

static struct agent_pvt *agents = NULL;  /**< Holds the list of agents (loaded form agents.conf). */

#define CHECK_FORMATS(ast, p) do { \
	if (p->chan) {\
		if (ast->nativeformats != p->chan->nativeformats) { \
			cw_log(LOG_DEBUG, "Native formats changing from %d to %d\n", ast->nativeformats, p->chan->nativeformats); \
			/* Native formats changed, reset things */ \
			ast->nativeformats = p->chan->nativeformats; \
			cw_log(LOG_DEBUG, "Resetting read to %d and write to %d\n", ast->readformat, ast->writeformat);\
			cw_set_read_format(ast, ast->readformat); \
			cw_set_write_format(ast, ast->writeformat); \
		} \
		if (p->chan->readformat != ast->rawreadformat)  \
			cw_set_read_format(p->chan, ast->rawreadformat); \
		if (p->chan->writeformat != ast->rawwriteformat) \
			cw_set_write_format(p->chan, ast->rawwriteformat); \
	} \
} while(0)

/* Cleanup moves all the relevant FD's from the 2nd to the first, but retains things
   properly for a timingfd XXX This might need more work if agents were logged in as agents or other
   totally impractical combinations XXX */

#define CLEANUP(ast, p) do { \
	int x; \
	if (p->chan) { \
		for (x=0;x<CW_MAX_FDS;x++) {\
			if (x != CW_MAX_FDS - 2) \
				ast->fds[x] = p->chan->fds[x]; \
		} \
		ast->fds[CW_MAX_FDS - 3] = p->chan->fds[CW_MAX_FDS - 2]; \
	} \
} while(0)

static struct cw_channel *agent_request(const char *type, int format, void *data, int *cause);
static int agent_devicestate(void *data);
static int agent_digit(struct cw_channel *ast, char digit);
static int agent_call(struct cw_channel *ast, char *dest, int timeout);
static int agent_hangup(struct cw_channel *ast);
static int agent_answer(struct cw_channel *ast);
static struct cw_frame *agent_read(struct cw_channel *ast);
static int agent_write(struct cw_channel *ast, struct cw_frame *f);
static int agent_sendhtml(struct cw_channel *ast, int subclass, const char *data, int datalen);
static int agent_sendtext(struct cw_channel *ast, const char *text);
static int agent_indicate(struct cw_channel *ast, int condition);
static int agent_fixup(struct cw_channel *oldchan, struct cw_channel *newchan);
static struct cw_channel *agent_bridgedchannel(struct cw_channel *chan, struct cw_channel *bridge);

static const struct cw_channel_tech agent_tech = {
	.type = channeltype,
	.description = tdesc,
	.capabilities = -1,
	.requester = agent_request,
	.devicestate = agent_devicestate,
	.send_digit = agent_digit,
	.call = agent_call,
	.hangup = agent_hangup,
	.answer = agent_answer,
	.read = agent_read,
	.write = agent_write,
	.send_html = agent_sendhtml,
	.send_text = agent_sendtext,
	.exception = agent_read,
	.indicate = agent_indicate,
	.fixup = agent_fixup,
	.bridged_channel = agent_bridgedchannel,
};

/**
 * Unlink (that is, take outside of the linked list) an agent.
 *
 * @param agent Agent to be unlinked.
 */
static void agent_unlink(struct agent_pvt *agent)
{
	struct agent_pvt *p, *prev;
	prev = NULL;
	p = agents;
	// Iterate over all agents looking for the one.
	while(p) {
		if (p == agent) {
			// Once it wal found, check if it is the first one.
			if (prev)
				// If it is not, tell the previous agent that the next one is the next one of the current (jumping the current).
				prev->next = agent->next;
			else
				// If it is the first one, just change the general pointer to point to the second one.
				agents = agent->next;
			// We are done.
			break;
		}
		prev = p;
		p = p->next;
	}
}

/**
 * Adds an agent to the global list of agents.
 *
 * @param agent A string with the username, password and real name of an agent. As defined in agents.conf. Example: "13,169,John Smith"
 * @param pending If it is pending or not.
 * @return The just created agent.
 * @sa agent_pvt, agents.
 */
static struct agent_pvt *add_agent(char *agent, int pending)
{
	int argc;
	char *argv[3 + 1];
	char *args;
	char *password = NULL;
	char *name = NULL;
	char *agt = NULL;
	struct agent_pvt *p, *prev;

	args = cw_strdupa(agent);

	// Extract username (agt), password and name from agent (args).
	if ((argc = cw_separate_app_args(args, ',', arraysize(argv), argv))) {
		agt = argv[0];
		if (argc > 1) {
			password = argv[1];
			while (*password && *password < 33) password++;
		} 
		if (argc > 2) {
			name = argv[2];
			while (*name && *name < 33) name++;
		}
	} else {
		cw_log(LOG_WARNING, "A blank agent line!\n");
	}
	
	// Are we searching for the agent here ? to see if it exists already ?
	prev=NULL;
	p = agents;
	while(p) {
		if (!pending && !strcmp(p->agent, agt))
			break;
		prev = p;
		p = p->next;
	}
	if (!p) {
		// Build the agent.
		p = malloc(sizeof(struct agent_pvt));
		if (p) {
			memset(p, 0, sizeof(struct agent_pvt));
			cw_copy_string(p->agent, agt, sizeof(p->agent));
			cw_mutex_init(&p->lock);
			cw_mutex_init(&p->app_lock);
			p->owning_app = (pthread_t) -1;
			p->app_sleep_cond = 1;
			p->group = group;
			p->pending = pending;
			p->next = NULL;
			if (prev)
				prev->next = p;
			else
				agents = p;
			
		} else {
			return NULL;
		}
	}
	
	cw_copy_string(p->password, password ? password : "", sizeof(p->password));
	cw_copy_string(p->name, name ? name : "", sizeof(p->name));
	cw_copy_string(p->moh, moh, sizeof(p->moh));
	p->ackcall = ackcall;
	p->autologoff = autologoff;

	/* If someone reduces the wrapuptime and reloads, we want it
	 * to change the wrapuptime immediately on all calls */
	if (p->wrapuptime > wrapuptime) {
		struct timeval now = cw_tvnow();
		/* XXX check what is this exactly */

		/* We won't be pedantic and check the tv_usec val */
		if (p->lastdisc.tv_sec > (now.tv_sec + wrapuptime/1000)) {
			p->lastdisc.tv_sec = now.tv_sec + wrapuptime/1000;
			p->lastdisc.tv_usec = now.tv_usec;
		}
	}
	p->wrapuptime = wrapuptime;

	if (pending)
		p->dead = 1;
	else
		p->dead = 0;
	return p;
}

/**
 * Deletes an agent after doing some clean up.
 * Further documentation: How safe is this function ? What state should the agent be to be cleaned.
 * @param p Agent to be deleted.
 * @returns Always 0.
 */
static int agent_cleanup(struct agent_pvt *p)
{
	struct cw_channel *chan = p->owner;
	p->owner = NULL;
	chan->tech_pvt = NULL;
	p->app_sleep_cond = 1;
	/* Release ownership of the agent to other threads (presumably running the login app). */
	if (cw_strlen_zero(p->loginchan))
	    cw_mutex_unlock(&p->app_lock);
	if (chan)
		cw_channel_free(chan);
	if (p->dead) {
		cw_mutex_destroy(&p->lock);
		cw_mutex_destroy(&p->app_lock);
		free(p);
        }
	return 0;
}

static int check_availability(struct agent_pvt *newlyavailable, int needlock);

static int agent_answer(struct cw_channel *ast)
{
	cw_log(LOG_WARNING, "Huh?  Agent is being asked to answer?\n");
	return -1;
}

static int __agent_start_monitoring(struct cw_channel *ast, struct agent_pvt *p, int needlock)
{
	char tmp[CW_MAX_BUF],tmp2[CW_MAX_BUF], *pointer;
	char filename[CW_MAX_BUF];
	int res = -1;
	if (!p)
		return -1;
	if (!ast->monitor) {
		snprintf(filename, sizeof(filename), "agent-%s-%s",p->agent, ast->uniqueid);
		/* substitute . for - */
		if ((pointer = strchr(filename, '.')))
			*pointer = '-';
		snprintf(tmp, sizeof(tmp), "%s%s",savecallsin ? savecallsin : "", filename);
		cw_monitor_start(ast, recordformat, tmp, needlock);
		cw_monitor_setjoinfiles(ast, 1);
		snprintf(tmp2, sizeof(tmp2), "%s%s.%s", urlprefix ? urlprefix : "", filename, recordformatext);
#if 0
		cw_verbose("name is %s, link is %s\n",tmp, tmp2);
#endif
		if (!ast->cdr)
			ast->cdr = cw_cdr_alloc();
		cw_cdr_setuserfield(ast, tmp2);
		res = 0;
	} else
		cw_log(LOG_ERROR, "Recording already started on that call.\n");
	return res;
}

static int agent_start_monitoring(struct cw_channel *ast, int needlock)
{
	return __agent_start_monitoring(ast, ast->tech_pvt, needlock);
}

static struct cw_frame *agent_read(struct cw_channel *ast)
{
	struct agent_pvt *p = ast->tech_pvt;
	struct cw_frame *f = NULL;
	static struct cw_frame null_frame = { CW_FRAME_NULL, };
	static struct cw_frame answer_frame = { CW_FRAME_CONTROL, CW_CONTROL_ANSWER };
	cw_mutex_lock(&p->lock); 
	CHECK_FORMATS(ast, p);
	if (p->chan) {
		cw_copy_flags(p->chan, ast, CW_FLAG_EXCEPTION);
		if (ast->fdno == CW_MAX_FDS - 3)
			p->chan->fdno = CW_MAX_FDS - 2;
		else
			p->chan->fdno = ast->fdno;
		f = cw_read(p->chan);
	} else
		f = &null_frame;
	if (!f) {
		/* If there's a channel, hang it up (if it's on a callback) make it NULL */
		if (p->chan) {
			p->chan->_bridge = NULL;
			/* Note that we don't hangup if it's not a callback because CallWeaver will do it
			   for us when the PBX instance that called login finishes */
			if (!cw_strlen_zero(p->loginchan)) {
				if (p->chan)
					cw_log(LOG_DEBUG, "Bridge on '%s' being cleared (2)\n", p->chan->name);
				cw_hangup(p->chan);
				if (p->wrapuptime && p->acknowledged)
					p->lastdisc = cw_tvadd(cw_tvnow(), cw_samp2tv(p->wrapuptime, 1000));
			}
			p->chan = NULL;
			p->acknowledged = 0;
		}
 	} else {
 		/* if acknowledgement is not required, and the channel is up, we may have missed
 		   an CW_CONTROL_ANSWER (if there was one), so mark the call acknowledged anyway */
 		if (!p->ackcall && !p->acknowledged && p->chan && (p->chan->_state == CW_STATE_UP))
  			p->acknowledged = 1;
 		switch (f->frametype) {
 		case CW_FRAME_CONTROL:
 			if (f->subclass == CW_CONTROL_ANSWER) {
 				if (p->ackcall) {
 					if (option_verbose > 2)
 						cw_verbose(VERBOSE_PREFIX_3 "%s answered, waiting for '#' to acknowledge\n", p->chan->name);
 					/* Don't pass answer along */
 					cw_fr_free(f);
 					f = &null_frame;
 				} else {
 					p->acknowledged = 1;
 					/* Use the builtin answer frame for the 
					   recording start check below. */
 					cw_fr_free(f);
 					f = &answer_frame;
 				}
 			}
 			break;
 		case CW_FRAME_DTMF:
 			if (!p->acknowledged && (f->subclass == '#')) {
 				if (option_verbose > 2)
 					cw_verbose(VERBOSE_PREFIX_3 "%s acknowledged\n", p->chan->name);
 				p->acknowledged = 1;
 				cw_fr_free(f);
 				f = &answer_frame;
 			} else if (f->subclass == '*') {
 				/* terminates call */
 				cw_fr_free(f);
 				f = NULL;
 			}
 			break;
 		case CW_FRAME_VOICE:
 			/* don't pass voice until the call is acknowledged */
 			if (!p->acknowledged) {
 				cw_fr_free(f);
 				f = &null_frame;
 			}
 			break;
  		}
  	}

	CLEANUP(ast,p);
	if (p->chan && !p->chan->_bridge) {
		if (strcasecmp(p->chan->type, "Local")) {
			p->chan->_bridge = ast;
			if (p->chan)
				cw_log(LOG_DEBUG, "Bridge on '%s' being set to '%s' (3)\n", p->chan->name, p->chan->_bridge->name);
		}
	}
	cw_mutex_unlock(&p->lock);
	if (recordagentcalls && f == &answer_frame)
		agent_start_monitoring(ast,0);
	return f;
}

static int agent_sendhtml(struct cw_channel *ast, int subclass, const char *data, int datalen)
{
	struct agent_pvt *p = ast->tech_pvt;
	int res = -1;
	cw_mutex_lock(&p->lock);
	if (p->chan) 
		res = cw_channel_sendhtml(p->chan, subclass, data, datalen);
	cw_mutex_unlock(&p->lock);
	return res;
}

static int agent_sendtext(struct cw_channel *ast, const char *text)
{
	struct agent_pvt *p = ast->tech_pvt;
	int res = -1;
	cw_mutex_lock(&p->lock);
	if (p->chan) 
		res = cw_sendtext(p->chan, text);
	cw_mutex_unlock(&p->lock);
	return res;
}

static int agent_write(struct cw_channel *ast, struct cw_frame *f)
{
	struct agent_pvt *p = ast->tech_pvt;
	int res = -1;
	CHECK_FORMATS(ast, p);
	cw_mutex_lock(&p->lock);
	if (p->chan) {
		if ((f->frametype != CW_FRAME_VOICE) ||
		    (f->subclass == p->chan->writeformat)) {
			res = cw_write(p->chan, f);
		} else {
			cw_log(LOG_DEBUG, "Dropping one incompatible voice frame on '%s' to '%s'\n", ast->name, p->chan->name);
			res = 0;
		}
	} else
		res = 0;
	CLEANUP(ast, p);
	cw_mutex_unlock(&p->lock);
	return res;
}

static int agent_fixup(struct cw_channel *oldchan, struct cw_channel *newchan)
{
	struct agent_pvt *p = newchan->tech_pvt;
	cw_mutex_lock(&p->lock);
	if (p->owner != oldchan) {
		cw_log(LOG_WARNING, "old channel wasn't %p but was %p\n", oldchan, p->owner);
		cw_mutex_unlock(&p->lock);
		return -1;
	}
	p->owner = newchan;
	cw_mutex_unlock(&p->lock);
	return 0;
}

static int agent_indicate(struct cw_channel *ast, int condition)
{
	struct agent_pvt *p = ast->tech_pvt;
	int res = -1;
	cw_mutex_lock(&p->lock);
	if (p->chan)
		res = cw_indicate(p->chan, condition);
	else
		res = 0;
	cw_mutex_unlock(&p->lock);
	return res;
}

static int agent_digit(struct cw_channel *ast, char digit)
{
	struct agent_pvt *p = ast->tech_pvt;
	int res = -1;
	cw_mutex_lock(&p->lock);
	if (p->chan)
		res = p->chan->tech->send_digit(p->chan, digit);
	else
		res = 0;
	cw_mutex_unlock(&p->lock);
	return res;
}

static int agent_call(struct cw_channel *ast, char *dest, int timeout)
{
	struct agent_pvt *p = ast->tech_pvt;
	int res = -1;
	int newstate=0;
	cw_mutex_lock(&p->lock);
	p->acknowledged = 0;
	if (!p->chan) {
		if (p->pending) {
			cw_log(LOG_DEBUG, "Pretending to dial on pending agent\n");
			newstate = CW_STATE_DIALING;
			res = 0;
		} else {
			cw_log(LOG_NOTICE, "Whoa, they hung up between alloc and call...  what are the odds of that?\n");
			res = -1;
		}
		cw_mutex_unlock(&p->lock);
		if (newstate)
			cw_setstate(ast, newstate);
		return res;
	} else if (!cw_strlen_zero(p->loginchan)) {
		time(&p->start);
		/* Call on this agent */
		if (option_verbose > 2)
			cw_verbose(VERBOSE_PREFIX_3 "outgoing agentcall, to agent '%s', on '%s'\n", p->agent, p->chan->name);
		cw_set_callerid(p->chan, ast->cid.cid_num, ast->cid.cid_name, NULL);
		cw_channel_inherit_variables(ast, p->chan);
		res = cw_call(p->chan, p->loginchan, 0);
		CLEANUP(ast,p);
		cw_mutex_unlock(&p->lock);
		return res;
	}
	cw_verbose( VERBOSE_PREFIX_3 "agent_call, call to agent '%s' call on '%s'\n", p->agent, p->chan->name);
	cw_log( LOG_DEBUG, "Playing beep, lang '%s'\n", p->chan->language);
	res = cw_streamfile(p->chan, beep, p->chan->language);
	cw_log( LOG_DEBUG, "Played beep, result '%d'\n", res);
	if (!res) {
		res = cw_waitstream(p->chan, "");
		cw_log( LOG_DEBUG, "Waited for stream, result '%d'\n", res);
	}
	if (!res) {
		res = cw_set_read_format(p->chan, cw_best_codec(p->chan->nativeformats));
		cw_log( LOG_DEBUG, "Set read format, result '%d'\n", res);
		if (res)
			cw_log(LOG_WARNING, "Unable to set read format to %s\n", cw_getformatname(cw_best_codec(p->chan->nativeformats)));
	} else {
		/* Agent hung-up */
		p->chan = NULL;
	}

	if (!res) {
		cw_set_write_format(p->chan, cw_best_codec(p->chan->nativeformats));
		cw_log( LOG_DEBUG, "Set write format, result '%d'\n", res);
		if (res)
			cw_log(LOG_WARNING, "Unable to set write format to %s\n", cw_getformatname(cw_best_codec(p->chan->nativeformats)));
	}
	if( !res )
	{
		/* Call is immediately up, or might need ack */
		if (p->ackcall > 1)
			newstate = CW_STATE_RINGING;
		else {
			newstate = CW_STATE_UP;
			if (recordagentcalls)
				agent_start_monitoring(ast,0);
			p->acknowledged = 1;
		}
		res = 0;
	}
	CLEANUP(ast,p);
	cw_mutex_unlock(&p->lock);
	if (newstate)
		cw_setstate(ast, newstate);
	return res;
}

/* store/clear the global variable that stores agentid based on the callerid */
static void set_agentbycallerid(const char *callerid, const char *agent)
{
	char buf[CW_MAX_BUF];

	/* if there is no Caller ID, nothing to do */
	if (cw_strlen_zero(callerid))
		return;

	snprintf(buf, sizeof(buf), "%s_%s",GETAGENTBYCALLERID, callerid);
	pbx_builtin_setvar_helper(NULL, buf, agent);
}

static int agent_hangup(struct cw_channel *ast)
{
	struct agent_pvt *p = ast->tech_pvt;
	int howlong = 0;
	cw_mutex_lock(&p->lock);
	p->owner = NULL;
	ast->tech_pvt = NULL;
	p->app_sleep_cond = 1;
	p->acknowledged = 0;

	/* if they really are hung up then set start to 0 so the test
	 * later if we're called on an already downed channel
	 * doesn't cause an agent to be logged out like when
	 * agent_request() is followed immediately by agent_hangup()
	 * as in apps/app_chanisavail.c:chanavail_exec()
	 */

	cw_mutex_lock(&usecnt_lock);
	usecnt--;
	cw_mutex_unlock(&usecnt_lock);

	cw_log(LOG_DEBUG, "Hangup called for state %s\n", cw_state2str(ast->_state));
	if (p->start && (ast->_state != CW_STATE_UP)) {
		howlong = time(NULL) - p->start;
		p->start = 0;
	} else if (ast->_state == CW_STATE_RESERVED) {
		howlong = 0;
	} else
		p->start = 0; 
	if (p->chan) {
		p->chan->_bridge = NULL;
		/* If they're dead, go ahead and hang up on the agent now */
		if (!cw_strlen_zero(p->loginchan)) {
			/* Store last disconnect time */
			if (p->wrapuptime)
				p->lastdisc = cw_tvadd(cw_tvnow(), cw_samp2tv(p->wrapuptime, 1000));
			else
				p->lastdisc = cw_tv(0,0);
			if (p->chan) {
				/* Recognize the hangup and pass it along immediately */
				cw_hangup(p->chan);
				p->chan = NULL;
			}
			cw_log(LOG_DEBUG, "Hungup, howlong is %d, autologoff is %d\n", howlong, p->autologoff);
			if (howlong  && p->autologoff && (howlong > p->autologoff)) {
				char agent[CW_MAX_AGENT] = "";
				long logintime = time(NULL) - p->loginstart;
				p->loginstart = 0;
				cw_log(LOG_NOTICE, "Agent '%s' didn't answer/confirm within %d seconds (waited %d)\n", p->name, p->autologoff, howlong);
				manager_event(EVENT_FLAG_AGENT, "Agentcallbacklogoff",
					      "Agent: %s\r\n"
					      "Loginchan: %s\r\n"
					      "Logintime: %ld\r\n"
					      "Reason: Autologoff\r\n"
					      "Uniqueid: %s\r\n",
					      p->agent, p->loginchan, logintime, ast->uniqueid);
				snprintf(agent, sizeof(agent), "Agent/%s", p->agent);
				cw_queue_log("NONE", ast->uniqueid, agent, "AGENTCALLBACKLOGOFF", "%s|%ld|%s", p->loginchan, logintime, "Autologoff");
				set_agentbycallerid(p->logincallerid, NULL);
				cw_device_state_changed("Agent/%s", p->agent);
				p->loginchan[0] = '\0';
				p->logincallerid[0] = '\0';
				if (persistent_agents)
					dump_agents();
			}
		} else if (p->dead) {
			cw_mutex_lock(&p->chan->lock);
			cw_softhangup(p->chan, CW_SOFTHANGUP_EXPLICIT);
			cw_mutex_unlock(&p->chan->lock);
		} else if (p->loginstart) {
			cw_mutex_lock(&p->chan->lock);
			cw_moh_start(p->chan, p->moh);
			cw_mutex_unlock(&p->chan->lock);
		}
	}
	cw_mutex_unlock(&p->lock);
	/* Only register a device state change if the agent is still logged in */
	if (p->loginstart)
		cw_device_state_changed("Agent/%s", p->agent);

	if (p->pending) {
		cw_mutex_lock(&agentlock);
		agent_unlink(p);
		cw_mutex_unlock(&agentlock);
	}
	if (p->abouttograb) {
		/* Let the "about to grab" thread know this isn't valid anymore, and let it
		   kill it later */
		p->abouttograb = 0;
	} else if (p->dead) {
		cw_mutex_destroy(&p->lock);
		cw_mutex_destroy(&p->app_lock);
		free(p);
	} else {
		if (p->chan) {
			/* Not dead -- check availability now */
			cw_mutex_lock(&p->lock);
			/* Store last disconnect time */
			p->lastdisc = cw_tvnow();
			cw_mutex_unlock(&p->lock);
		}
		/* Release ownership of the agent to other threads (presumably running the login app). */
		if (cw_strlen_zero(p->loginchan))
			cw_mutex_unlock(&p->app_lock);
	}
	return 0;
}

static int agent_cont_sleep( void *data )
{
	struct agent_pvt *p;
	int res;

	p = (struct agent_pvt *)data;

	cw_mutex_lock(&p->lock);
	res = p->app_sleep_cond;
	if (p->lastdisc.tv_sec) {
		if (cw_tvdiff_ms(cw_tvnow(), p->lastdisc) > p->wrapuptime) 
			res = 1;
	}
	cw_mutex_unlock(&p->lock);
#if 0
	if( !res )
		cw_log( LOG_DEBUG, "agent_cont_sleep() returning %d\n", res );
#endif		
	return res;
}

static int agent_ack_sleep( void *data )
{
	struct agent_pvt *p;
	int res=0;
	int to = 1000;
	struct cw_frame *f;

	/* Wait a second and look for something */

	p = (struct agent_pvt *)data;
	if (p->chan) {
		for(;;) {
			to = cw_waitfor(p->chan, to);
			if (to < 0) {
				res = -1;
				break;
			}
			if (!to) {
				res = 0;
				break;
			}
			f = cw_read(p->chan);
			if (!f) {
				res = -1;
				break;
			}
			if (f->frametype == CW_FRAME_DTMF)
				res = f->subclass;
			else
				res = 0;
			cw_fr_free(f);
			cw_mutex_lock(&p->lock);
			if (!p->app_sleep_cond) {
				cw_mutex_unlock(&p->lock);
				res = 0;
				break;
			} else if (res == '#') {
				cw_mutex_unlock(&p->lock);
				res = 1;
				break;
			}
			cw_mutex_unlock(&p->lock);
			res = 0;
		}
	} else
		res = -1;
	return res;
}

static struct cw_channel *agent_bridgedchannel(struct cw_channel *chan, struct cw_channel *bridge)
{
	struct agent_pvt *p = bridge->tech_pvt;
	struct cw_channel *ret=NULL;
	

	if(p) {
		if (chan == p->chan)
			ret = bridge->_bridge;
		else if (chan == bridge->_bridge)
			ret = p->chan;
	}

	if (option_debug)
		cw_log(LOG_DEBUG, "Asked for bridged channel on '%s'/'%s', returning '%s'\n", chan->name, bridge->name, ret ? ret->name : "<none>");
	return ret;
}

/*--- agent_new: Create new agent channel ---*/
static struct cw_channel *agent_new(struct agent_pvt *p, int state)
{
	struct cw_channel *tmp;
	struct cw_frame null_frame = { CW_FRAME_NULL };
#if 0
	if (!p->chan) {
		cw_log(LOG_WARNING, "No channel? :(\n");
		return NULL;
	}
#endif	
	tmp = cw_channel_alloc(0);
	if (tmp) {
		tmp->tech = &agent_tech;
		if (p->chan) {
			tmp->nativeformats = p->chan->nativeformats;
			tmp->writeformat = p->chan->writeformat;
			tmp->rawwriteformat = p->chan->writeformat;
			tmp->readformat = p->chan->readformat;
			tmp->rawreadformat = p->chan->readformat;
			cw_copy_string(tmp->language, p->chan->language, sizeof(tmp->language));
			cw_copy_string(tmp->context, p->chan->context, sizeof(tmp->context));
			cw_copy_string(tmp->exten, p->chan->exten, sizeof(tmp->exten));
		} else {
			tmp->nativeformats = CW_FORMAT_SLINEAR;
			tmp->writeformat = CW_FORMAT_SLINEAR;
			tmp->rawwriteformat = CW_FORMAT_SLINEAR;
			tmp->readformat = CW_FORMAT_SLINEAR;
			tmp->rawreadformat = CW_FORMAT_SLINEAR;
		}
		if (p->pending)
			snprintf(tmp->name, sizeof(tmp->name), "Agent/P%s-%ld", p->agent, cw_random() & 0xffff);
		else
			snprintf(tmp->name, sizeof(tmp->name), "Agent/%s", p->agent);
		tmp->type = channeltype;
		/* Safe, agentlock already held */
		cw_setstate(tmp, state);
		tmp->tech_pvt = p;
		p->owner = tmp;
		cw_mutex_lock(&usecnt_lock);
		usecnt++;
		cw_mutex_unlock(&usecnt_lock);
		cw_update_use_count();
		tmp->priority = 1;
		/* Wake up and wait for other applications (by definition the login app)
		 * to release this channel). Takes ownership of the agent channel
		 * to this thread only.
		 * For signalling the other thread, cw_queue_frame is used until we
		 * can safely use signals for this purpose. The pselect() needs to be
		 * implemented in the kernel for this.
		 */
		p->app_sleep_cond = 0;
		if( cw_strlen_zero(p->loginchan) && cw_mutex_trylock(&p->app_lock) )
		{
			if (p->chan) {
				cw_queue_frame(p->chan, &null_frame);
				cw_mutex_unlock(&p->lock);	/* For other thread to read the condition. */
				cw_mutex_lock(&p->app_lock);
				cw_mutex_lock(&p->lock);
			}
			if( !p->chan )
			{
				cw_log(LOG_WARNING, "Agent disconnected while we were connecting the call\n");
				p->owner = NULL;
				tmp->tech_pvt = NULL;
				p->app_sleep_cond = 1;
				cw_channel_free( tmp );
				cw_mutex_unlock(&p->lock);	/* For other thread to read the condition. */
				cw_mutex_unlock(&p->app_lock);
				return NULL;
			}
		} else if (!cw_strlen_zero(p->loginchan)) {
			if (p->chan)
				cw_queue_frame(p->chan, &null_frame);
			if (!p->chan) {
				cw_log(LOG_WARNING, "Agent disconnected while we were connecting the call\n");
				p->owner = NULL;
				tmp->tech_pvt = NULL;
				p->app_sleep_cond = 1;
				cw_channel_free( tmp );
				cw_mutex_unlock(&p->lock);     /* For other thread to read the condition. */
                                return NULL;
			}	
		}
		p->owning_app = pthread_self();
		/* After the above step, there should not be any blockers. */
		if (p->chan) {
			if (cw_test_flag(p->chan, CW_FLAG_BLOCKING)) {
				cw_log( LOG_ERROR, "A blocker exists after agent channel ownership acquired\n" );
				CRASH;
			}
			cw_moh_stop(p->chan);
		}
	} else
		cw_log(LOG_WARNING, "Unable to allocate agent channel structure\n");
	return tmp;
}


/**
 * Read configuration data. The file named agents.conf.
 *
 * @returns Always 0, or so it seems.
 */
static int read_agent_config(void)
{
	struct cw_config *cfg;
	struct cw_variable *v;
	struct agent_pvt *p, *pl, *pn;
	char *general_val;

	group = 0;
	autologoff = 0;
	wrapuptime = 0;
	ackcall = 0;
	cfg = cw_config_load(config);
	if (!cfg) {
		cw_log(LOG_NOTICE, "No agent configuration found -- agent support disabled\n");
		return 0;
	}
	cw_mutex_lock(&agentlock);
	p = agents;
	while(p) {
		p->dead = 1;
		p = p->next;
	}
	strcpy(moh, "default");
	/* set the default recording values */
	recordagentcalls = 0;
	createlink = 0;
	strcpy(recordformat, "wav");
	strcpy(recordformatext, "wav");
	urlprefix[0] = '\0';
	savecallsin[0] = '\0';

	/* Read in [general] section for persistance */
	if ((general_val = cw_variable_retrieve(cfg, "general", "persistentagents")))
		persistent_agents = cw_true(general_val);

	/* Read in the [agents] section */
	v = cw_variable_browse(cfg, "agents");
	while(v) {
		/* Create the interface list */
		if (!strcasecmp(v->name, "agent")) {
			add_agent(v->value, 0);
		} else if (!strcasecmp(v->name, "group")) {
			group = cw_get_group(v->value);
		} else if (!strcasecmp(v->name, "autologoff")) {
			autologoff = atoi(v->value);
			if (autologoff < 0)
				autologoff = 0;
		} else if (!strcasecmp(v->name, "ackcall")) {
			if (!strcasecmp(v->value, "always"))
				ackcall = 2;
			else if (cw_true(v->value))
				ackcall = 1;
			else
				ackcall = 0;
		} else if (!strcasecmp(v->name, "wrapuptime")) {
			wrapuptime = atoi(v->value);
			if (wrapuptime < 0)
				wrapuptime = 0;
		} else if (!strcasecmp(v->name, "maxlogintries") && !cw_strlen_zero(v->value)) {
			maxlogintries = atoi(v->value);
			if (maxlogintries < 0)
				maxlogintries = 0;
		} else if (!strcasecmp(v->name, "goodbye") && !cw_strlen_zero(v->value)) {
			strcpy(agentgoodbye,v->value);
		} else if (!strcasecmp(v->name, "musiconhold")) {
			cw_copy_string(moh, v->value, sizeof(moh));
		} else if (!strcasecmp(v->name, "updatecdr")) {
			if (cw_true(v->value))
				updatecdr = 1;
			else
				updatecdr = 0;
		} else if (!strcasecmp(v->name, "recordagentcalls")) {
			recordagentcalls = cw_true(v->value);
		} else if (!strcasecmp(v->name, "createlink")) {
			createlink = cw_true(v->value);
		} else if (!strcasecmp(v->name, "recordformat")) {
			cw_copy_string(recordformat, v->value, sizeof(recordformat));
			if (!strcasecmp(v->value, "wav49"))
				strcpy(recordformatext, "WAV");
			else
				cw_copy_string(recordformatext, v->value, sizeof(recordformatext));
		} else if (!strcasecmp(v->name, "urlprefix")) {
			cw_copy_string(urlprefix, v->value, sizeof(urlprefix));
			if (urlprefix[strlen(urlprefix) - 1] != '/')
				strncat(urlprefix, "/", sizeof(urlprefix) - strlen(urlprefix) - 1);
		} else if (!strcasecmp(v->name, "savecallsin")) {
			if (v->value[0] == '/')
				cw_copy_string(savecallsin, v->value, sizeof(savecallsin));
			else
				snprintf(savecallsin, sizeof(savecallsin) - 2, "/%s", v->value);
			if (savecallsin[strlen(savecallsin) - 1] != '/')
				strncat(savecallsin, "/", sizeof(savecallsin) - strlen(savecallsin) - 1);
		} else if (!strcasecmp(v->name, "custom_beep")) {
			cw_copy_string(beep, v->value, sizeof(beep));
		}
		v = v->next;
	}
	p = agents;
	pl = NULL;
	while(p) {
		pn = p->next;
		if (p->dead) {
			/* Unlink */
			if (pl)
				pl->next = p->next;
			else
				agents = p->next;
			/* Destroy if  appropriate */
			if (!p->owner) {
				if (!p->chan) {
					cw_mutex_destroy(&p->lock);
					cw_mutex_destroy(&p->app_lock);
					free(p);
				} else {
					/* Cause them to hang up */
					cw_softhangup(p->chan, CW_SOFTHANGUP_EXPLICIT);
				}
			}
		} else
			pl = p;
		p = pn;
	}
	cw_mutex_unlock(&agentlock);
	cw_config_destroy(cfg);
	return 0;
}

static int check_availability(struct agent_pvt *newlyavailable, int needlock)
{
	struct cw_channel *chan=NULL, *parent=NULL;
	struct agent_pvt *p;
	int res;

	if (option_debug)
		cw_log(LOG_DEBUG, "Checking availability of '%s'\n", newlyavailable->agent);
	if (needlock)
		cw_mutex_lock(&agentlock);
	p = agents;
	while(p) {
		if (p == newlyavailable) {
			p = p->next;
			continue;
		}
		cw_mutex_lock(&p->lock);
		if (!p->abouttograb && p->pending && ((p->group && (newlyavailable->group & p->group)) || !strcmp(p->agent, newlyavailable->agent))) {
			if (option_debug)
				cw_log(LOG_DEBUG, "Call '%s' looks like a winner for agent '%s'\n", p->owner->name, newlyavailable->agent);
			/* We found a pending call, time to merge */
			chan = agent_new(newlyavailable, CW_STATE_DOWN);
			parent = p->owner;
			p->abouttograb = 1;
			cw_mutex_unlock(&p->lock);
			break;
		}
		cw_mutex_unlock(&p->lock);
		p = p->next;
	}
	if (needlock)
		cw_mutex_unlock(&agentlock);
	if (parent && chan)  {
		if (newlyavailable->ackcall > 1) {
			/* Don't do beep here */
			res = 0;
		} else {
			if (option_debug > 2)
				cw_log( LOG_DEBUG, "Playing beep, lang '%s'\n", newlyavailable->chan->language);
			res = cw_streamfile(newlyavailable->chan, beep, newlyavailable->chan->language);
			if (option_debug > 2)
				cw_log( LOG_DEBUG, "Played beep, result '%d'\n", res);
			if (!res) {
				res = cw_waitstream(newlyavailable->chan, "");
				cw_log( LOG_DEBUG, "Waited for stream, result '%d'\n", res);
			}
		}
		if (!res) {
			/* Note -- parent may have disappeared */
			if (p->abouttograb) {
				newlyavailable->acknowledged = 1;
				/* Safe -- agent lock already held */
				cw_setstate(parent, CW_STATE_UP);
				cw_setstate(chan, CW_STATE_UP);
				cw_copy_string(parent->context, chan->context, sizeof(parent->context));
				/* Go ahead and mark the channel as a zombie so that masquerade will
				   destroy it for us, and we need not call cw_hangup */
				cw_mutex_lock(&parent->lock);
				cw_set_flag(chan, CW_FLAG_ZOMBIE);
				cw_channel_masquerade(parent, chan);
				cw_mutex_unlock(&parent->lock);
				p->abouttograb = 0;
			} else {
				if (option_debug)
					cw_log(LOG_DEBUG, "Sneaky, parent disappeared in the mean time...\n");
				agent_cleanup(newlyavailable);
			}
		} else {
			if (option_debug)
				cw_log(LOG_DEBUG, "Ugh...  Agent hung up at exactly the wrong time\n");
			agent_cleanup(newlyavailable);
		}
	}
	return 0;
}

static int check_beep(struct agent_pvt *newlyavailable, int needlock)
{
	struct agent_pvt *p;
	int res=0;

	cw_log(LOG_DEBUG, "Checking beep availability of '%s'\n", newlyavailable->agent);
	if (needlock)
		cw_mutex_lock(&agentlock);
	p = agents;
	while(p) {
		if (p == newlyavailable) {
			p = p->next;
			continue;
		}
		cw_mutex_lock(&p->lock);
		if (!p->abouttograb && p->pending && ((p->group && (newlyavailable->group & p->group)) || !strcmp(p->agent, newlyavailable->agent))) {
			if (option_debug)
				cw_log(LOG_DEBUG, "Call '%s' looks like a would-be winner for agent '%s'\n", p->owner->name, newlyavailable->agent);
			cw_mutex_unlock(&p->lock);
			break;
		}
		cw_mutex_unlock(&p->lock);
		p = p->next;
	}
	if (needlock)
		cw_mutex_unlock(&agentlock);
	if (p) {
		cw_mutex_unlock(&newlyavailable->lock);
		if (option_debug > 2)
			cw_log( LOG_DEBUG, "Playing beep, lang '%s'\n", newlyavailable->chan->language);
		res = cw_streamfile(newlyavailable->chan, beep, newlyavailable->chan->language);
		if (option_debug > 2)
			cw_log( LOG_DEBUG, "Played beep, result '%d'\n", res);
		if (!res) {
			res = cw_waitstream(newlyavailable->chan, "");
			if (option_debug)
				cw_log( LOG_DEBUG, "Waited for stream, result '%d'\n", res);
		}
		cw_mutex_lock(&newlyavailable->lock);
	}
	return res;
}

/*--- agent_request: Part of the CallWeaver interface ---*/
static struct cw_channel *agent_request(const char *type, int format, void *data, int *cause)
{
	struct agent_pvt *p;
	struct cw_channel *chan = NULL;
	char *s;
	cw_group_t groupmatch;
	int groupoff;
	int waitforagent=0;
	int hasagent = 0;
	struct timeval tv;

	s = data;
	if ((s[0] == '@') && (sscanf(s + 1, "%d", &groupoff) == 1)) {
		groupmatch = (1 << groupoff);
	} else if ((s[0] == ':') && (sscanf(s + 1, "%d", &groupoff) == 1)) {
		groupmatch = (1 << groupoff);
		waitforagent = 1;
	} else {
		groupmatch = 0;
	}

	/* Check actual logged in agents first */
	cw_mutex_lock(&agentlock);
	p = agents;
	while(p) {
		cw_mutex_lock(&p->lock);
		if (!p->pending && ((groupmatch && (p->group & groupmatch)) || !strcmp(data, p->agent)) &&
		    cw_strlen_zero(p->loginchan)) {
			if (p->chan)
				hasagent++;
			if (!p->lastdisc.tv_sec) {
				/* Agent must be registered, but not have any active call, and not be in a waiting state */
				if (!p->owner && p->chan) {
					/* Fixed agent */
					chan = agent_new(p, CW_STATE_DOWN);
				}
				if (chan) {
					cw_mutex_unlock(&p->lock);
					break;
				}
			}
		}
		cw_mutex_unlock(&p->lock);
		p = p->next;
	}
	if (!p) {
		p = agents;
		while(p) {
			cw_mutex_lock(&p->lock);
			if (!p->pending && ((groupmatch && (p->group & groupmatch)) || !strcmp(data, p->agent))) {
				if (p->chan || !cw_strlen_zero(p->loginchan))
					hasagent++;
				tv = cw_tvnow();
#if 0
				cw_log(LOG_NOTICE, "Time now: %ld, Time of lastdisc: %ld\n", tv.tv_sec, p->lastdisc.tv_sec);
#endif
				if (!p->lastdisc.tv_sec || (tv.tv_sec > p->lastdisc.tv_sec)) {
					p->lastdisc = cw_tv(0, 0);
					/* Agent must be registered, but not have any active call, and not be in a waiting state */
					if (!p->owner && p->chan) {
						/* Could still get a fixed agent */
						chan = agent_new(p, CW_STATE_DOWN);
					} else if (!p->owner && !cw_strlen_zero(p->loginchan)) {
						/* Adjustable agent */
						p->chan = cw_request("Local", format, p->loginchan, cause);
						if (p->chan)
							chan = agent_new(p, CW_STATE_DOWN);
					}
					if (chan) {
						cw_mutex_unlock(&p->lock);
						break;
					}
				}
			}
			cw_mutex_unlock(&p->lock);
			p = p->next;
		}
	}

	if (!chan && waitforagent) {
		/* No agent available -- but we're requesting to wait for one.
		   Allocate a place holder */
		if (hasagent) {
			if (option_debug)
				cw_log(LOG_DEBUG, "Creating place holder for '%s'\n", s);
			p = add_agent(data, 1);
			p->group = groupmatch;
			chan = agent_new(p, CW_STATE_DOWN);
			if (!chan) {
				cw_log(LOG_WARNING, "Weird...  Fix this to drop the unused pending agent\n");
			}
		} else
			cw_log(LOG_DEBUG, "Not creating place holder for '%s' since nobody logged in\n", s);
	}
	if (hasagent)
		*cause = CW_CAUSE_BUSY;
	else
		*cause = CW_CAUSE_UNREGISTERED;
	cw_mutex_unlock(&agentlock);
	return chan;
}

static int powerof(unsigned int v)
{
	int x;
	for (x=0;x<32;x++) {
		if (v & (1 << x)) return x;
	}
	return 0;
}

/**
 * Lists agents and their status to the Manager API.
 * It is registered on load_module() and it gets called by the manager backend.
 * @param s
 * @param m
 * @returns 
 * @sa action_agent_logoff(), action_agent_callback_login(), load_module().
 */
static int action_agents(struct mansession *s, struct message *m)
{
	char *id = astman_get_header(m,"ActionID");
	char idText[256] = "";
	char chanbuf[256];
	struct agent_pvt *p;
	char *username = NULL;
	char *loginChan = NULL;
	char *talkingtoChan = NULL;
	char *status = NULL;

	if (!cw_strlen_zero(id))
		snprintf(idText, sizeof(idText) ,"ActionID: %s\r\n", id);
	astman_send_ack(s, m, "Agents will follow");
	cw_mutex_lock(&agentlock);
	p = agents;
	while(p) {
        	cw_mutex_lock(&p->lock);

		/* Status Values:
		   AGENT_LOGGEDOFF - Agent isn't logged in
		   AGENT_IDLE      - Agent is logged in, and waiting for call
		   AGENT_ONCALL    - Agent is logged in, and on a call
		   AGENT_UNKNOWN   - Don't know anything about agent. Shouldn't ever get this. */

		if(!cw_strlen_zero(p->name)) {
			username = p->name;
		} else {
			username = "None";
		}

		/* Set a default status. It 'should' get changed. */
		status = "AGENT_UNKNOWN";

		if (!cw_strlen_zero(p->loginchan) && !p->chan) {
			loginChan = p->loginchan;
			talkingtoChan = "n/a";
			status = "AGENT_IDLE";
			if (p->acknowledged) {
				snprintf(chanbuf, sizeof(chanbuf), " %s (Confirmed)", p->loginchan);
				loginChan = chanbuf;
			}
		} else if (p->chan) {
			loginChan = cw_strdupa(p->chan->name);
			if (p->owner && p->owner->_bridge) {
        			talkingtoChan = p->chan->cid.cid_num;
        			status = "AGENT_ONCALL";
			} else {
        			talkingtoChan = "n/a";
        			status = "AGENT_IDLE";
			}
		} else {
			loginChan = "n/a";
			talkingtoChan = "n/a";
			status = "AGENT_LOGGEDOFF";
		}

		cw_cli(s->fd, "Event: Agents\r\n"
			"Agent: %s\r\n"
			"Name: %s\r\n"
			"Status: %s\r\n"
			"LoggedInChan: %s\r\n"
			"LoggedInTime: %ld\r\n"
			"TalkingTo: %s\r\n"
			"%s"
			"\r\n",
			p->agent, username, status, loginChan, (long int)p->loginstart, talkingtoChan, idText);
		cw_mutex_unlock(&p->lock);
		p = p->next;
	}
	cw_mutex_unlock(&agentlock);
	cw_cli(s->fd, "Event: AgentsComplete\r\n"
		"%s"
		"\r\n",idText);
	return 0;
}

static int agent_logoff(char *agent, int soft)
{
	struct agent_pvt *p;
	long logintime;
	int ret = -1; /* Return -1 if no agent if found */

	for (p=agents; p; p=p->next) {
		if (!strcasecmp(p->agent, agent)) {
			if (!soft) {
				if (p->owner) {
					cw_softhangup(p->owner, CW_SOFTHANGUP_EXPLICIT);
				}
				if (p->chan) {
					cw_softhangup(p->chan, CW_SOFTHANGUP_EXPLICIT);
				}
			}
			ret = 0; /* found an agent => return 0 */
			logintime = time(NULL) - p->loginstart;
			p->loginstart = 0;
			
			manager_event(EVENT_FLAG_AGENT, "Agentcallbacklogoff",
				      "Agent: %s\r\n"
				      "Loginchan: %s\r\n"
				      "Logintime: %ld\r\n",
				      p->agent, p->loginchan, logintime);
			cw_queue_log("NONE", "NONE", agent, "AGENTCALLBACKLOGOFF", "%s|%ld|%s", p->loginchan, logintime, "CommandLogoff");
			set_agentbycallerid(p->logincallerid, NULL);
			p->loginchan[0] = '\0';
			p->logincallerid[0] = '\0';
			cw_device_state_changed("Agent/%s", p->agent);
			if (persistent_agents)
				dump_agents();
			break;
		}
	}

	return ret;
}

static int agent_logoff_cmd(int fd, int argc, char **argv)
{
	int ret;
	char *agent;

	if (argc < 3 || argc > 4)
		return RESULT_SHOWUSAGE;
	if (argc == 4 && strcasecmp(argv[3], "soft"))
		return RESULT_SHOWUSAGE;

	agent = argv[2] + 6;
	ret = agent_logoff(agent, argc == 4);
	if (ret == 0)
		cw_cli(fd, "Logging out %s\n", agent);

	return RESULT_SUCCESS;
}

/**
 * Sets an agent as no longer logged in in the Manager API.
 * It is registered on load_module() and it gets called by the manager backend.
 * @param s
 * @param m
 * @returns 
 * @sa action_agents(), action_agent_callback_login(), load_module().
 */
static int action_agent_logoff(struct mansession *s, struct message *m)
{
	char *agent = astman_get_header(m, "Agent");
	char *soft_s = astman_get_header(m, "Soft"); /* "true" is don't hangup */
	int soft;
	int ret; /* return value of agent_logoff */

	if (cw_strlen_zero(agent)) {
		astman_send_error(s, m, "No agent specified");
		return 0;
	}

	if (cw_true(soft_s))
		soft = 1;
	else
		soft = 0;

	ret = agent_logoff(agent, soft);
	if (ret == 0)
		astman_send_ack(s, m, "Agent logged out");
	else
		astman_send_error(s, m, "No such agent");

	return 0;
}

static char *complete_agent_logoff_cmd(char *line, char *word, int pos, int state)
{
	struct agent_pvt *p;
	char name[CW_MAX_AGENT];
	int which = 0;

	if (pos == 2) {
		for (p=agents; p; p=p->next) {
			snprintf(name, sizeof(name), "Agent/%s", p->agent);
			if (!strncasecmp(word, name, strlen(word))) {
				if (++which > state) {
					return strdup(name);
				}
			}
		}
	} else if (pos == 3 && state == 0) {
		return strdup("soft");
	}
	return NULL;
}

/**
 * Show agents in cli.
 */
static int agents_show(int fd, int argc, char **argv)
{
	struct agent_pvt *p;
	char username[CW_MAX_BUF];
	char location[CW_MAX_BUF] = "";
	char talkingto[CW_MAX_BUF] = "";
	char moh[CW_MAX_BUF];
	int count_agents = 0;		/* Number of agents configured */
	int online_agents = 0;		/* Number of online agents */
	int offline_agents = 0;		/* Number of offline agents */
	if (argc != 2)
		return RESULT_SHOWUSAGE;
	cw_mutex_lock(&agentlock);
	p = agents;
	while(p) {
		cw_mutex_lock(&p->lock);
		if (p->pending) {
			if (p->group)
				cw_cli(fd, "-- Pending call to group %d\n", powerof(p->group));
			else
				cw_cli(fd, "-- Pending call to agent %s\n", p->agent);
		} else {
			if (!cw_strlen_zero(p->name))
				snprintf(username, sizeof(username), "(%s) ", p->name);
			else
				username[0] = '\0';
			if (p->chan) {
				snprintf(location, sizeof(location), "logged in on %s", p->chan->name);
				if (p->owner && cw_bridged_channel(p->owner)) {
					snprintf(talkingto, sizeof(talkingto), " talking to %s", cw_bridged_channel(p->owner)->name);
				} else {
					strcpy(talkingto, " is idle");
				}
				online_agents++;
			} else if (!cw_strlen_zero(p->loginchan)) {
				if (cw_tvdiff_ms(cw_tvnow(), p->lastdisc) > 0 || !(p->lastdisc.tv_sec))
					snprintf(location, sizeof(location) - 20, "available at '%s'", p->loginchan);
				else
					snprintf(location, sizeof(location) - 20, "wrapping up at '%s'", p->loginchan);
				talkingto[0] = '\0';
				online_agents++;
				if (p->acknowledged)
					strncat(location, " (Confirmed)", sizeof(location) - strlen(location) - 1);
			} else {
				strcpy(location, "not logged in");
				talkingto[0] = '\0';
				offline_agents++;
			}
			if (!cw_strlen_zero(p->moh))
				snprintf(moh, sizeof(moh), " (musiconhold is '%s')", p->moh);
			cw_cli(fd, "%-12.12s %s%s%s%s\n", p->agent, 
				username, location, talkingto, moh);
			count_agents++;
		}
		cw_mutex_unlock(&p->lock);
		p = p->next;
	}
	cw_mutex_unlock(&agentlock);
	if ( !count_agents ) {
		cw_cli(fd, "No Agents are configured in %s\n",config);
	} else {
		cw_cli(fd, "%d agents configured [%d online , %d offline]\n",count_agents, online_agents, offline_agents);
	}
	cw_cli(fd, "\n");
	                
	return RESULT_SUCCESS;
}

static char show_agents_usage[] = 
"Usage: show agents\n"
"       Provides summary information on agents.\n";

static char agent_logoff_usage[] =
"Usage: agent logoff <channel> [soft]\n"
"       Sets an agent as no longer logged in.\n"
"       If 'soft' is specified, do not hangup existing calls.\n";

static struct cw_cli_entry cli_show_agents = {
	{ "show", "agents", NULL }, agents_show, 
	"Show status of agents", show_agents_usage, NULL };

static struct cw_cli_entry cli_agent_logoff = {
	{ "agent", "logoff", NULL }, agent_logoff_cmd, 
	"Sets an agent offline", agent_logoff_usage, complete_agent_logoff_cmd };

STANDARD_LOCAL_USER;
LOCAL_USER_DECL;

/**
 * Log in agent application.
 *
 * @param chan
 * @param data
 * @param callbackmode
 * @returns 
 */
static int __login_exec(struct cw_channel *chan, int argc, char **argv, int callbackmode)
{
	int res=0;
	int tries = 0;
	int max_login_tries = maxlogintries;
	struct agent_pvt *p;
	struct localuser *u;
	int login_state = 0;
	char user[CW_MAX_AGENT] = "";
	char pass[CW_MAX_AGENT];
	char agent[CW_MAX_AGENT] = "";
	char xpass[CW_MAX_AGENT] = "";
	char *errmsg;
	char *tmpoptions = NULL;
	char *context = NULL;
	int play_announcement = 1;
	char agent_goodbye[CW_MAX_FILENAME_LEN];
	int update_cdr = updatecdr;
	char *filename = "agent-loginok";

	LOCAL_USER_ADD(u);

	cw_copy_string(agent_goodbye, agentgoodbye, sizeof(agent_goodbye));

	/* Set Channel Specific Login Overrides */
	if (pbx_builtin_getvar_helper(chan, "AGENTLMAXLOGINTRIES") && strlen(pbx_builtin_getvar_helper(chan, "AGENTLMAXLOGINTRIES"))) {
		max_login_tries = atoi(pbx_builtin_getvar_helper(chan, "AGENTMAXLOGINTRIES"));
		if (max_login_tries < 0)
			max_login_tries = 0;
		tmpoptions=pbx_builtin_getvar_helper(chan, "AGENTMAXLOGINTRIES");
		if (option_verbose > 2)
			cw_verbose(VERBOSE_PREFIX_3 "Saw variable AGENTMAXLOGINTRIES=%s, setting max_login_tries to: %d on Channel '%s'.\n",tmpoptions,max_login_tries,chan->name);
	}
	if (pbx_builtin_getvar_helper(chan, "AGENTUPDATECDR") && !cw_strlen_zero(pbx_builtin_getvar_helper(chan, "AGENTUPDATECDR"))) {
		if (cw_true(pbx_builtin_getvar_helper(chan, "AGENTUPDATECDR")))
			update_cdr = 1;
		else
			update_cdr = 0;
		tmpoptions=pbx_builtin_getvar_helper(chan, "AGENTUPDATECDR");
		if (option_verbose > 2)
			cw_verbose(VERBOSE_PREFIX_3 "Saw variable AGENTUPDATECDR=%s, setting update_cdr to: %d on Channel '%s'.\n",tmpoptions,update_cdr,chan->name);
	}
	if (pbx_builtin_getvar_helper(chan, "AGENTGOODBYE") && !cw_strlen_zero(pbx_builtin_getvar_helper(chan, "AGENTGOODBYE"))) {
		strcpy(agent_goodbye, pbx_builtin_getvar_helper(chan, "AGENTGOODBYE"));
		tmpoptions=pbx_builtin_getvar_helper(chan, "AGENTGOODBYE");
		if (option_verbose > 2)
			cw_verbose(VERBOSE_PREFIX_3 "Saw variable AGENTGOODBYE=%s, setting agent_goodbye to: %s on Channel '%s'.\n",tmpoptions,agent_goodbye,chan->name);
	}
	/* End Channel Specific Login Overrides */
	
	if (callbackmode && argc > 2) {
		if ((context = strchr(argv[2], '@')))
			*(context++) = '\0';
	}

	if (argc > 1) {
		for (; argv[1][0]; argv[1]++) {
			switch (argv[1][0]) {
				case 's': play_announcement = 0; break;
			}
		}
	}

	if (chan->_state != CW_STATE_UP)
		res = cw_answer(chan);
	if (!res) {
		if (argc > 1 && argv[0][0])
			cw_copy_string(user, argv[0], CW_MAX_AGENT);
		else
			res = cw_app_getdata(chan, "agent-user", user, sizeof(user) - 1, 0);
	}
	while (!res && (max_login_tries==0 || tries < max_login_tries)) {
		tries++;
		/* Check for password */
		cw_mutex_lock(&agentlock);
		p = agents;
		while(p) {
			if (!strcmp(p->agent, user) && !p->pending)
				cw_copy_string(xpass, p->password, sizeof(xpass));
			p = p->next;
		}
		cw_mutex_unlock(&agentlock);
		if (!res) {
			if (!cw_strlen_zero(xpass))
				res = cw_app_getdata(chan, "agent-pass", pass, sizeof(pass) - 1, 0);
			else
				pass[0] = '\0';
		}
		errmsg = "agent-incorrect";

#if 0
		cw_log(LOG_NOTICE, "user: %s, pass: %s\n", user, pass);
#endif		

		/* Check again for accuracy */
		cw_mutex_lock(&agentlock);
		p = agents;
		while(p) {
			cw_mutex_lock(&p->lock);
			if (!strcmp(p->agent, user) &&
			    !strcmp(p->password, pass) && !p->pending) {
				login_state = 1; /* Successful Login */

				/* Ensure we can't be gotten until we're done */
				gettimeofday(&p->lastdisc, NULL);
				p->lastdisc.tv_sec++;

				/* Set Channel Specific Agent Overides */
				if (pbx_builtin_getvar_helper(chan, "AGENTACKCALL") && strlen(pbx_builtin_getvar_helper(chan, "AGENTACKCALL"))) {
					if (!strcasecmp(pbx_builtin_getvar_helper(chan, "AGENTACKCALL"), "always"))
						p->ackcall = 2;
					else if (cw_true(pbx_builtin_getvar_helper(chan, "AGENTACKCALL")))
						p->ackcall = 1;
					else
						p->ackcall = 0;
					tmpoptions=pbx_builtin_getvar_helper(chan, "AGENTACKCALL");
					if (option_verbose > 2)
						cw_verbose(VERBOSE_PREFIX_3 "Saw variable AGENTACKCALL=%s, setting ackcall to: %d for Agent '%s'.\n",tmpoptions,p->ackcall,p->agent);
				}
				if (pbx_builtin_getvar_helper(chan, "AGENTAUTOLOGOFF") && strlen(pbx_builtin_getvar_helper(chan, "AGENTAUTOLOGOFF"))) {
					p->autologoff = atoi(pbx_builtin_getvar_helper(chan, "AGENTAUTOLOGOFF"));
					if (p->autologoff < 0)
						p->autologoff = 0;
					tmpoptions=pbx_builtin_getvar_helper(chan, "AGENTAUTOLOGOFF");
					if (option_verbose > 2)
						cw_verbose(VERBOSE_PREFIX_3 "Saw variable AGENTAUTOLOGOFF=%s, setting autologff to: %d for Agent '%s'.\n",tmpoptions,p->autologoff,p->agent);
				}
				if (pbx_builtin_getvar_helper(chan, "AGENTWRAPUPTIME") && strlen(pbx_builtin_getvar_helper(chan, "AGENTWRAPUPTIME"))) {
					p->wrapuptime = atoi(pbx_builtin_getvar_helper(chan, "AGENTWRAPUPTIME"));
					if (p->wrapuptime < 0)
						p->wrapuptime = 0;
					tmpoptions=pbx_builtin_getvar_helper(chan, "AGENTWRAPUPTIME");
					if (option_verbose > 2)
						cw_verbose(VERBOSE_PREFIX_3 "Saw variable AGENTWRAPUPTIME=%s, setting wrapuptime to: %d for Agent '%s'.\n",tmpoptions,p->wrapuptime,p->agent);
				}
				/* End Channel Specific Agent Overides */
				if (!p->chan) {
					char last_loginchan[80] = "";
					long logintime;
					snprintf(agent, sizeof(agent), "Agent/%s", p->agent);

					if (callbackmode) {
						char tmpchan[CW_MAX_BUF] = "";
						char *extension = (argc > 2 ? argv[2] : NULL);
						int pos = 0;
						/* Retrieve login chan */
						while (!extension || extension[0] != '#') {
							if (!cw_strlen_zero(extension)) {
								cw_copy_string(tmpchan, extension, sizeof(tmpchan));
								res = 0;
							} else
								res = cw_app_getdata(chan, "agent-newlocation", tmpchan+pos, sizeof(tmpchan) - 2, 0);
							if (cw_strlen_zero(tmpchan) || cw_exists_extension(chan, !cw_strlen_zero(context) ? context : "default", tmpchan,
													     1, NULL))
								break;
							if (extension) {
								cw_log(LOG_WARNING, "Extension '%s' is not valid for automatic login of agent '%s'\n", extension, p->agent);
								extension = NULL;
								pos = 0;
							} else {
								cw_log(LOG_WARNING, "Extension '%s@%s' is not valid for automatic login of agent '%s'\n", tmpchan, !cw_strlen_zero(context) ? context : "default", p->agent);
								res = cw_streamfile(chan, "invalid", chan->language);
								if (!res)
									res = cw_waitstream(chan, CW_DIGIT_ANY);
								if (res > 0) {
									tmpchan[0] = res;
									tmpchan[1] = '\0';
									pos = 1;
								} else {
									tmpchan[0] = '\0';
									pos = 0;
								}
							}
						}
						extension = tmpchan;
						if (!res) {
							set_agentbycallerid(p->logincallerid, NULL);
							if (!cw_strlen_zero(context) && !cw_strlen_zero(tmpchan))
								snprintf(p->loginchan, sizeof(p->loginchan), "%s@%s", tmpchan, context);
							else {
								cw_copy_string(last_loginchan, p->loginchan, sizeof(last_loginchan));
								cw_copy_string(p->loginchan, tmpchan, sizeof(p->loginchan));
							}
							p->acknowledged = 0;
							if (cw_strlen_zero(p->loginchan)) {
								login_state = 2;
								filename = "agent-loggedoff";
							} else {
								if (chan->cid.cid_num) {
									cw_copy_string(p->logincallerid, chan->cid.cid_num, sizeof(p->logincallerid));
									set_agentbycallerid(p->logincallerid, p->agent);
								} else
									p->logincallerid[0] = '\0';
							}

							if(update_cdr && chan->cdr)
								snprintf(chan->cdr->channel, sizeof(chan->cdr->channel), "Agent/%s", p->agent);

						}
					} else {
						p->loginchan[0] = '\0';
						p->logincallerid[0] = '\0';
						p->acknowledged = 0;
					}
					cw_mutex_unlock(&p->lock);
					cw_mutex_unlock(&agentlock);
					if( !res && play_announcement==1 )
						res = cw_streamfile(chan, filename, chan->language);
					if (!res)
						cw_waitstream(chan, "");
					cw_mutex_lock(&agentlock);
					cw_mutex_lock(&p->lock);
					if (!res) {
						res = cw_set_read_format(chan, cw_best_codec(chan->nativeformats));
						if (res)
							cw_log(LOG_WARNING, "Unable to set read format to %d\n", cw_best_codec(chan->nativeformats));
					}
					if (!res) {
						res = cw_set_write_format(chan, cw_best_codec(chan->nativeformats));
						if (res)
							cw_log(LOG_WARNING, "Unable to set write format to %d\n", cw_best_codec(chan->nativeformats));
					}
					/* Check once more just in case */
					if (p->chan)
						res = -1;
					if (callbackmode && !res) {
						/* Just say goodbye and be done with it */
						if (!cw_strlen_zero(p->loginchan)) {
							if (p->loginstart == 0)
								time(&p->loginstart);
							manager_event(EVENT_FLAG_AGENT, "Agentcallbacklogin",
								      "Agent: %s\r\n"
								      "Loginchan: %s\r\n"
								      "Uniqueid: %s\r\n",
								      p->agent, p->loginchan, chan->uniqueid);
							cw_queue_log("NONE", chan->uniqueid, agent, "AGENTCALLBACKLOGIN", "%s", p->loginchan);
							if (option_verbose > 1)
								cw_verbose(VERBOSE_PREFIX_2 "Callback Agent '%s' logged in on %s\n", p->agent, p->loginchan);
							cw_device_state_changed("Agent/%s", p->agent);
						} else {
							logintime = time(NULL) - p->loginstart;
							p->loginstart = 0;
							manager_event(EVENT_FLAG_AGENT, "Agentcallbacklogoff",
								      "Agent: %s\r\n"
								      "Loginchan: %s\r\n"
								      "Logintime: %ld\r\n"
								      "Uniqueid: %s\r\n",
								      p->agent, last_loginchan, logintime, chan->uniqueid);
							cw_queue_log("NONE", chan->uniqueid, agent, "AGENTCALLBACKLOGOFF", "%s|%ld|", last_loginchan, logintime);
							if (option_verbose > 1)
								cw_verbose(VERBOSE_PREFIX_2 "Callback Agent '%s' logged out\n", p->agent);
							cw_device_state_changed("Agent/%s", p->agent);
						}
						cw_mutex_unlock(&agentlock);
						if (!res)
							res = cw_safe_sleep(chan, 500);
						cw_mutex_unlock(&p->lock);
						if (persistent_agents)
							dump_agents();
					} else if (!res) {
#ifdef HONOR_MUSIC_CLASS
						/* check if the moh class was changed with setmusiconhold */
						if (*(chan->musicclass))
							cw_copy_string(p->moh, chan->musicclass, sizeof(p->moh));
#endif								
						cw_moh_start(chan, p->moh);
						if (p->loginstart == 0)
							time(&p->loginstart);
						manager_event(EVENT_FLAG_AGENT, "Agentlogin",
							      "Agent: %s\r\n"
							      "Channel: %s\r\n"
							      "Uniqueid: %s\r\n",
							      p->agent, chan->name, chan->uniqueid);
						if (update_cdr && chan->cdr)
							snprintf(chan->cdr->channel, sizeof(chan->cdr->channel), "Agent/%s", p->agent);
						cw_queue_log("NONE", chan->uniqueid, agent, "AGENTLOGIN", "%s", chan->name);
						if (option_verbose > 1)
							cw_verbose(VERBOSE_PREFIX_2 "Agent '%s' logged in (format %s/%s)\n", p->agent,
								    cw_getformatname(chan->readformat), cw_getformatname(chan->writeformat));
						/* Login this channel and wait for it to
						   go away */
						p->chan = chan;
						if (p->ackcall > 1)
							check_beep(p, 0);
						else
							check_availability(p, 0);
						cw_mutex_unlock(&p->lock);
						cw_mutex_unlock(&agentlock);
						cw_device_state_changed("Agent/%s", p->agent);
						while (res >= 0) {
							cw_mutex_lock(&p->lock);
							if (p->chan != chan)
								res = -1;
							cw_mutex_unlock(&p->lock);
							/* Yield here so other interested threads can kick in. */
							sched_yield();
							if (res)
								break;

							cw_mutex_lock(&agentlock);
							cw_mutex_lock(&p->lock);
							if (p->lastdisc.tv_sec) {
								if (cw_tvdiff_ms(cw_tvnow(), p->lastdisc) > p->wrapuptime) {
									if (option_debug)
										cw_log(LOG_DEBUG, "Wrapup time for %s expired!\n", p->agent);
									p->lastdisc = cw_tv(0, 0);
									if (p->ackcall > 1)
										check_beep(p, 0);
									else
										check_availability(p, 0);
								}
							}
							cw_mutex_unlock(&p->lock);
							cw_mutex_unlock(&agentlock);
							/*	Synchronize channel ownership between call to agent and itself. */
							cw_mutex_lock( &p->app_lock );
							cw_mutex_lock(&p->lock);
							p->owning_app = pthread_self();
							cw_mutex_unlock(&p->lock);
							if (p->ackcall > 1) 
								res = agent_ack_sleep(p);
							else
								res = cw_safe_sleep_conditional( chan, 1000,
												  agent_cont_sleep, p );
							cw_mutex_unlock( &p->app_lock );
							if ((p->ackcall > 1)  && (res == 1)) {
								cw_mutex_lock(&agentlock);
								cw_mutex_lock(&p->lock);
								check_availability(p, 0);
								cw_mutex_unlock(&p->lock);
								cw_mutex_unlock(&agentlock);
								res = 0;
							}
							sched_yield();
						}
						cw_mutex_lock(&p->lock);
						if (res && p->owner) 
							cw_log(LOG_WARNING, "Huh?  We broke out when there was still an owner?\n");
						/* Log us off if appropriate */
						if (p->chan == chan)
							p->chan = NULL;
						p->acknowledged = 0;
						logintime = time(NULL) - p->loginstart;
						p->loginstart = 0;
						cw_mutex_unlock(&p->lock);
						manager_event(EVENT_FLAG_AGENT, "Agentlogoff",
							      "Agent: %s\r\n"
							      "Logintime: %ld\r\n"
							      "Uniqueid: %s\r\n",
							      p->agent, logintime, chan->uniqueid);
						cw_queue_log("NONE", chan->uniqueid, agent, "AGENTLOGOFF", "%s|%ld", chan->name, logintime);
						if (option_verbose > 1)
							cw_verbose(VERBOSE_PREFIX_2 "Agent '%s' logged out\n", p->agent);
						/* If there is no owner, go ahead and kill it now */
						cw_device_state_changed("Agent/%s", p->agent);
						if (p->dead && !p->owner) {
							cw_mutex_destroy(&p->lock);
							cw_mutex_destroy(&p->app_lock);
							free(p);
						}
					}
					else {
						cw_mutex_unlock(&p->lock);
						p = NULL;
					}
					res = -1;
				} else {
					cw_mutex_unlock(&p->lock);
					errmsg = "agent-alreadyon";
					p = NULL;
				}
				break;
			}
			cw_mutex_unlock(&p->lock);
			p = p->next;
		}
		if (!p)
			cw_mutex_unlock(&agentlock);

		if (!res && (max_login_tries==0 || tries < max_login_tries))
			res = cw_app_getdata(chan, errmsg, user, sizeof(user) - 1, 0);
	}
		
	if (!res)
		res = cw_safe_sleep(chan, 500);

	/* AgentLogin() exit */
	if (!callbackmode) {
		LOCAL_USER_REMOVE(u);
		return -1;
	}
	/* AgentCallbackLogin() exit*/
	else {
		/* Set variables */
		if (login_state > 0) {
			pbx_builtin_setvar_helper(chan, "AGENTNUMBER", user);
			if (login_state==1) {
				pbx_builtin_setvar_helper(chan, "AGENTSTATUS", "on");
				pbx_builtin_setvar_helper(chan, "AGENTEXTEN", argv[0]);
			}
			else {
				pbx_builtin_setvar_helper(chan, "AGENTSTATUS", "off");
			}
		}
		else {
			pbx_builtin_setvar_helper(chan, "AGENTSTATUS", "fail");
		}
		if (cw_exists_extension(chan, chan->context, chan->exten, chan->priority + 1, chan->cid.cid_num)) {
			LOCAL_USER_REMOVE(u);
			return 0;
		}
		/* Do we need to play agent-goodbye now that we will be hanging up? */
		if (play_announcement) {
			if (!res)
				res = cw_safe_sleep(chan, 1000);
			res = cw_streamfile(chan, agent_goodbye, chan->language);
			if (!res)
				res = cw_waitstream(chan, "");
			if (!res)
				res = cw_safe_sleep(chan, 1000);
		}
	}

	LOCAL_USER_REMOVE(u);
	
	/* We should never get here if next priority exists when in callbackmode */
 	return -1;
}

/**
 * Called by the AgentLogin application (from the dial plan).
 * 
 * @param chan
 * @param data
 * @returns
 * @sa callback_login_exec(), agentmonitoroutgoing_exec(), load_module().
 */
static int login_exec(struct cw_channel *chan, int argc, char **argv)
{
	return __login_exec(chan, argc, argv, 0);
}

/**
 *  Called by the AgentCallbackLogin application (from the dial plan).
 * 
 * @param chan
 * @param data
 * @returns
 * @sa login_exec(), agentmonitoroutgoing_exec(), load_module().
 */
static int callback_exec(struct cw_channel *chan, int argc, char **argv)
{
	return __login_exec(chan, argc, argv, 1);
}

/**
 * Sets an agent as logged in by callback in the Manager API.
 * It is registered on load_module() and it gets called by the manager backend.
 * @param s
 * @param m
 * @returns 
 * @sa action_agents(), action_agent_logoff(), load_module().
 */
static int action_agent_callback_login(struct mansession *s, struct message *m)
{
	char *agent = astman_get_header(m, "Agent");
	char *exten = astman_get_header(m, "Exten");
	char *context = astman_get_header(m, "Context");
	char *wrapuptime_s = astman_get_header(m, "WrapupTime");
	char *ackcall_s = astman_get_header(m, "AckCall");
	struct agent_pvt *p;
	int login_state = 0;

	if (cw_strlen_zero(agent)) {
		astman_send_error(s, m, "No agent specified");
		return 0;
	}

	if (cw_strlen_zero(exten)) {
		astman_send_error(s, m, "No extension specified");
		return 0;
	}

	cw_mutex_lock(&agentlock);
	p = agents;
	while(p) {
		if (strcmp(p->agent, agent) || p->pending) {
			p = p->next;
			continue;
		}
		if (p->chan) {
			login_state = 2; /* already logged in (and on the phone)*/
			break;
		}
		cw_mutex_lock(&p->lock);
		login_state = 1; /* Successful Login */
		
		if (cw_strlen_zero(context))
			cw_copy_string(p->loginchan, exten, sizeof(p->loginchan));
		else
			snprintf(p->loginchan, sizeof(p->loginchan), "%s@%s", exten, context);

		if (!cw_strlen_zero(wrapuptime_s)) {
			p->wrapuptime = atoi(wrapuptime_s);
			if (p->wrapuptime < 0)
				p->wrapuptime = 0;
		}

		if (cw_true(ackcall_s))
			p->ackcall = 1;
		else
			p->ackcall = 0;

		if (p->loginstart == 0)
			time(&p->loginstart);
		manager_event(EVENT_FLAG_AGENT, "Agentcallbacklogin",
			      "Agent: %s\r\n"
			      "Loginchan: %s\r\n",
			      p->agent, p->loginchan);
		cw_queue_log("NONE", "NONE", agent, "AGENTCALLBACKLOGIN", "%s", p->loginchan);
		if (option_verbose > 1)
			cw_verbose(VERBOSE_PREFIX_2 "Callback Agent '%s' logged in on %s\n", p->agent, p->loginchan);
		cw_device_state_changed("Agent/%s", p->agent);
		cw_mutex_unlock(&p->lock);
		p = p->next;
		if (persistent_agents)
			dump_agents();
	}
	cw_mutex_unlock(&agentlock);

	if (login_state == 1)
		astman_send_ack(s, m, "Agent logged in");
	else if (login_state == 0)
		astman_send_error(s, m, "No such agent");
	else if (login_state == 2)
		astman_send_error(s, m, "Agent already logged in");

	return 0;
}

/**
 *  Called by the AgentMonitorOutgoing application (from the dial plan).
 *
 * @param chan
 * @param data
 * @returns
 * @sa login_exec(), callback_login_exec(), load_module().
 */
static int agentmonitoroutgoing_exec(struct cw_channel *chan, int argc, char **argv)
{
	int exitifnoagentid = 0;
	int nowarnings = 0;
	int changeoutgoing = 0;
	int res = 0;
	char agent[CW_MAX_AGENT], *tmp;

	if (argc > 1) {
		for (; argv[0][0]; argv[0]++) {
			switch (argv[0][0]) {
				case 'c': changeoutgoing = 1; break;
				case 'd': exitifnoagentid = 1; break;
				case 'n': nowarnings = 1; break;
			}
		}
	}

	if (chan->cid.cid_num) {
		char agentvar[CW_MAX_BUF];
		snprintf(agentvar, sizeof(agentvar), "%s_%s", GETAGENTBYCALLERID, chan->cid.cid_num);
		if ((tmp = pbx_builtin_getvar_helper(NULL, agentvar))) {
			struct agent_pvt *p = agents;
			cw_copy_string(agent, tmp, sizeof(agent));
			cw_mutex_lock(&agentlock);
			while (p) {
				if (!strcasecmp(p->agent, tmp)) {
					if (changeoutgoing) snprintf(chan->cdr->channel, sizeof(chan->cdr->channel), "Agent/%s", p->agent);
					__agent_start_monitoring(chan, p, 1);
					break;
				}
				p = p->next;
			}
			cw_mutex_unlock(&agentlock);
			
		} else {
			res = -1;
			if (!nowarnings)
				cw_log(LOG_WARNING, "Couldn't find the global variable %s, so I can't figure out which agent (if it's an agent) is placing outgoing call.\n", agentvar);
		}
	} else {
		res = -1;
		if (!nowarnings)
			cw_log(LOG_WARNING, "There is no callerid on that call, so I can't figure out which agent (if it's an agent) is placing outgoing call.\n");
	}
	if (res) {
		pbx_builtin_setvar_helper(chan, "AGENTSTATUS", "FAIL");
	} else {
		pbx_builtin_setvar_helper(chan, "AGENTSTATUS", "SUCCESS");
	}
	return 0;
}

/**
 * Dump AgentCallbackLogin agents to the database for persistence
 */
static void dump_agents(void)
{
	struct agent_pvt *cur_agent = NULL;
	char buf[256];

	for (cur_agent = agents; cur_agent; cur_agent = cur_agent->next) {
		if (cur_agent->chan)
			continue;

		if (!cw_strlen_zero(cur_agent->loginchan)) {
			snprintf(buf, sizeof(buf), "%s;%s", cur_agent->loginchan, cur_agent->logincallerid);
			if (cw_db_put(pa_family, cur_agent->agent, buf))
				cw_log(LOG_WARNING, "failed to create persistent entry!\n");
			else if (option_debug)
				cw_log(LOG_DEBUG, "Saved Agent: %s on %s\n", cur_agent->agent, cur_agent->loginchan);
		} else {
			/* Delete -  no agent or there is an error */
			cw_db_del(pa_family, cur_agent->agent);
		}
	}
}

/**
 * Reload the persistent agents from cwdb.
 */
static void reload_agents(void)
{
	char *agent_num;
	struct cw_db_entry *db_tree;
	struct cw_db_entry *entry;
	struct agent_pvt *cur_agent;
	char agent_data[256];
	char *parse;
	char *agent_chan;
	char *agent_callerid;

	db_tree = cw_db_gettree(pa_family, NULL);

	cw_mutex_lock(&agentlock);
	for (entry = db_tree; entry; entry = entry->next) {
        	if (!strncmp(entry->key, pa_family, strlen(pa_family))){
                       agent_num = entry->key + strlen(pa_family) + 2;
               	} else {
                       agent_num = entry->key;
	        }
		cur_agent = agents;
		while (cur_agent) {
			cw_mutex_lock(&cur_agent->lock);
			if (strcmp(agent_num, cur_agent->agent) == 0)
				break;
			cw_mutex_unlock(&cur_agent->lock);
			cur_agent = cur_agent->next;
		}
		if (!cur_agent) {
			cw_db_del(pa_family, agent_num);
			continue;
		} else
			cw_mutex_unlock(&cur_agent->lock);
		if (!cw_db_get(pa_family, agent_num, agent_data, sizeof(agent_data)-1)) {
			if (option_debug)
				cw_log(LOG_DEBUG, "Reload Agent: %s on %s\n", cur_agent->agent, agent_data);
			parse = agent_data;
			agent_chan = strsep(&parse, ";");
			agent_callerid = strsep(&parse, ";");
			cw_copy_string(cur_agent->loginchan, agent_chan, sizeof(cur_agent->loginchan));
			if (agent_callerid) {
				cw_copy_string(cur_agent->logincallerid, agent_callerid, sizeof(cur_agent->logincallerid));
				set_agentbycallerid(cur_agent->logincallerid, cur_agent->agent);
			} else
				cur_agent->logincallerid[0] = '\0';
			if (cur_agent->loginstart == 0)
				time(&cur_agent->loginstart);
			cw_device_state_changed("Agent/%s", cur_agent->agent);	
		}
	}
	cw_mutex_unlock(&agentlock);
	if (db_tree) {
		cw_log(LOG_NOTICE, "Agents sucessfully reloaded from database.\n");
		cw_db_freetree(db_tree);
	}
}

/*--- agent_devicestate: Part of PBX channel interface ---*/
static int agent_devicestate(void *data)
{
	struct agent_pvt *p;
	char *s;
	cw_group_t groupmatch;
	int groupoff;
	int waitforagent=0;
	int res = CW_DEVICE_INVALID;
	
	s = data;
	if ((s[0] == '@') && (sscanf(s + 1, "%d", &groupoff) == 1)) {
		groupmatch = (1 << groupoff);
	} else if ((s[0] == ':') && (sscanf(s + 1, "%d", &groupoff) == 1)) {
		groupmatch = (1 << groupoff);
		waitforagent = 1;
	} else {
		groupmatch = 0;
	}

	/* Check actual logged in agents first */
	cw_mutex_lock(&agentlock);
	p = agents;
	while(p) {
		cw_mutex_lock(&p->lock);
		if (!p->pending && ((groupmatch && (p->group & groupmatch)) || !strcmp(data, p->agent))) {
			if (p->owner) {
				if (res != CW_DEVICE_INUSE)
					res = CW_DEVICE_BUSY;
			} else {
				if (res == CW_DEVICE_BUSY)
					res = CW_DEVICE_INUSE;
				if (p->chan || !cw_strlen_zero(p->loginchan)) {
					if (res == CW_DEVICE_INVALID)
						res = CW_DEVICE_UNKNOWN;
				} else if (res == CW_DEVICE_INVALID)	
					res = CW_DEVICE_UNAVAILABLE;
			}
			if (!strcmp(data, p->agent)) {
				cw_mutex_unlock(&p->lock);
				break;
			}
		}
		cw_mutex_unlock(&p->lock);
		p = p->next;
	}
	cw_mutex_unlock(&agentlock);
	return res;
}

/**
 * Initialize the Agents module.
 * This funcion is being called by CallWeaver when loading the module. Among other thing it registers applications, cli commands and reads the cofiguration file.
 *
 * @returns int Always 0.
 */
int load_module()
{
	/* Make sure we can register our agent channel type */
	if (cw_channel_register(&agent_tech)) {
		cw_log(LOG_ERROR, "Unable to register channel class %s\n", channeltype);
		return -1;
	}
	/* Dialplan applications */
	agentlogin_app = cw_register_application(app, login_exec, synopsis, syntax, descrip);
	agentcallbacklogin_app = cw_register_application(app2, callback_exec, synopsis2, syntax2, descrip2);
	agentmonitoroutgoing_app = cw_register_application(app3, agentmonitoroutgoing_exec, synopsis3, syntax3, descrip3);
	/* Manager commands */
	cw_manager_register2("Agents", EVENT_FLAG_AGENT, action_agents, "Lists agents and their status", mandescr_agents);
	cw_manager_register2("AgentLogoff", EVENT_FLAG_AGENT, action_agent_logoff, "Sets an agent as no longer logged in", mandescr_agent_logoff);
	cw_manager_register2("AgentCallbackLogin", EVENT_FLAG_AGENT, action_agent_callback_login, "Sets an agent as logged in by callback", mandescr_agent_callback_login);
	/* CLI Application */
	cw_cli_register(&cli_show_agents);
	cw_cli_register(&cli_agent_logoff);
	/* Read in the config */
	read_agent_config();
	if (persistent_agents)
		reload_agents();
	return 0;
}

int reload()
{
	read_agent_config();
	if (persistent_agents)
		reload_agents();
	return 0;
}

int unload_module()
{
	struct agent_pvt *p;
	int res = 0;

	/* First, take us out of the channel loop */
	/* Unregister CLI application */
	cw_cli_unregister(&cli_show_agents);
	cw_cli_unregister(&cli_agent_logoff);
	/* Unregister dialplan applications */
	res |= cw_unregister_application(agentlogin_app);
	res |= cw_unregister_application(agentcallbacklogin_app);
	res |= cw_unregister_application(agentmonitoroutgoing_app);
	/* Unregister manager command */
	cw_manager_unregister("Agents");
	cw_manager_unregister("AgentLogoff");
	cw_manager_unregister("AgentCallbackLogin");
	/* Unregister channel */
	cw_channel_unregister(&agent_tech);
	if (!cw_mutex_lock(&agentlock)) {
		/* Hangup all interfaces if they have an owner */
		p = agents;
		while(p) {
			if (p->owner)
				cw_softhangup(p->owner, CW_SOFTHANGUP_APPUNLOAD);
			p = p->next;
		}
		agents = NULL;
		cw_mutex_unlock(&agentlock);
	} else {
		cw_log(LOG_WARNING, "Unable to lock the monitor\n");
		return -1;
	}		
	return res;
}

int usecount()
{
	return usecnt;
}

char *description()
{
	return (char *) desc;
}

