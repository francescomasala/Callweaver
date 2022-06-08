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


/**
 * Implementation of Agents
 *
 * @file chan_agent.c
 * @brief This file is the implementation of Agents modules.
 * It is a dynamic module that is loaded by OpenPBX. At load time, load_module is run.
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

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/lock.h"
#include "openpbx/channel.h"
#include "openpbx/config.h"
#include "openpbx/logger.h"
#include "openpbx/module.h"
#include "openpbx/pbx.h"
#include "openpbx/options.h"
#include "openpbx/lock.h"
#include "openpbx/sched.h"
#include "openpbx/io.h"
#include "openpbx/acl.h"
#include "openpbx/phone_no_utils.h"
#include "openpbx/file.h"
#include "openpbx/cli.h"
#include "openpbx/app.h"
#include "openpbx/musiconhold.h"
#include "openpbx/manager.h"
#include "openpbx/features.h"
#include "openpbx/utils.h"
#include "openpbx/causes.h"
#include "openpbx/opbxdb.h"
#include "openpbx/devicestate.h"

static const char desc[] = "Agent Proxy Channel";
static const char channeltype[] = "Agent";
static const char tdesc[] = "Call Agent Proxy Channel";
static const char config[] = "agents.conf";

static const char app[] = "AgentLogin";
static const char app2[] = "AgentCallbackLogin";
static const char app3[] = "AgentMonitorOutgoing";

static const char synopsis[] = "Call agent login";
static const char synopsis2[] = "Call agent callback login";
static const char synopsis3[] = "Record agent's outgoing call";

static const char descrip[] =
"  AgentLogin([AgentNo][|options]):\n"
"Asks the agent to login to the system.  Always returns -1.  While\n"
"logged in, the agent can receive calls and will hear a 'beep'\n"
"when a new call comes in. The agent can dump the call by pressing\n"
"the star key.\n"
"The option string may contain zero or more of the following characters:\n"
"      's' -- silent login - do not announce the login ok segment after agent logged in/off\n";

static const char descrip2[] =
"  AgentCallbackLogin([AgentNo][|[options][|[exten]@context]]):\n"
"Asks the agent to login to the system with callback.\n"
"The agent's callback extension is called (optionally with the specified\n"
"context).\n"
"The option string may contain zero or more of the following characters:\n"
"      's' -- silent login - do not announce the login ok segment agent logged in/off\n";

static const char descrip3[] =
"  AgentMonitorOutgoing([options]):\n"
"Tries to figure out the id of the agent who is placing outgoing call based on\n"
"comparision of the callerid of the current interface and the global variable \n"
"placed by the AgentCallbackLogin application. That's why it should be used only\n"
"with the AgentCallbackLogin app. Uses the monitoring functions in chan_agent \n"
"instead of Monitor application. That have to be configured in the agents.conf file.\n"
"\nReturn value:\n"
"Normally the app returns 0 unless the options are passed. Also if the callerid or\n"
"the agentid are not specified it'll look for n+101 priority.\n"
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
"	*Extension: Extension to use for callback\n"
"	Context: Context to use for callback\n"
"	AckCall: Set to 'true' to require an acknowledgement by '#' when agent is called back\n"
"	WrapupTime: the minimum amount of time after disconnecting before the caller can receive a new call\n";

static char moh[80] = "default";

#define OPBX_MAX_AGENT	80		/**< Agent ID or Password max length */
#define OPBX_MAX_BUF	256
#define OPBX_MAX_FILENAME_LEN	256

/** Persistent Agents opbxdb family */
static const char pa_family[] = "/Agents";
/** The maximum lengh of each persistent member agent database entry */
#define PA_MAX_LEN 2048
/** queues.conf [general] option */
static int persistent_agents = 0;
static void dump_agents(void);

static opbx_group_t group;
static int autologoff;
static int wrapuptime;
static int ackcall;

static int maxlogintries = 3;
static char agentgoodbye[OPBX_MAX_FILENAME_LEN] = "vm-goodbye";

static int usecnt =0;
OPBX_MUTEX_DEFINE_STATIC(usecnt_lock);

/* Protect the interface list (of pvt's) */
OPBX_MUTEX_DEFINE_STATIC(agentlock);

static int recordagentcalls = 0;
static char recordformat[OPBX_MAX_BUF] = "";
static char recordformatext[OPBX_MAX_BUF] = "";
static int createlink = 0;
static char urlprefix[OPBX_MAX_BUF] = "";
static char savecallsin[OPBX_MAX_BUF] = "";
static int updatecdr = 0;
static char beep[OPBX_MAX_BUF] = "beep";

#define GETAGENTBYCALLERID	"AGENTBYCALLERID"

/**
 * Structure representing an agent.
 */
struct agent_pvt {
	opbx_mutex_t lock;              /**< Channel private lock */
	int dead;                      /**< Poised for destruction? */
	int pending;                   /**< Not a real agent -- just pending a match */
	int abouttograb;               /**< About to grab */
	int autologoff;                /**< Auto timeout time */
	int ackcall;                   /**< ackcall */
	time_t loginstart;             /**< When agent first logged in (0 when logged off) */
	time_t start;                  /**< When call started */
	struct timeval lastdisc;       /**< When last disconnected */
	int wrapuptime;                /**< Wrapup time in ms */
	opbx_group_t group;             /**< Group memberships */
	int acknowledged;              /**< Acknowledged */
	char moh[80];                  /**< Which music on hold */
	char agent[OPBX_MAX_AGENT];     /**< Agent ID */
	char password[OPBX_MAX_AGENT];  /**< Password for Agent login */
	char name[OPBX_MAX_AGENT];
	opbx_mutex_t app_lock;          /**< Synchronization between owning applications */
	volatile pthread_t owning_app; /**< Owning application thread id */
	volatile int app_sleep_cond;   /**< Sleep condition for the login app */
	struct opbx_channel *owner;     /**< Agent */
	char loginchan[80];            /**< channel they logged in from */
	char logincallerid[80];        /**< Caller ID they had when they logged in */
	struct opbx_channel *chan;      /**< Channel we use */
	struct agent_pvt *next;        /**< Next Agent in the linked list. */
};

static struct agent_pvt *agents = NULL;  /**< Holds the list of agents (loaded form agents.conf). */

#define CHECK_FORMATS(ast, p) do { \
	if (p->chan) {\
		if (ast->nativeformats != p->chan->nativeformats) { \
			opbx_log(LOG_DEBUG, "Native formats changing from %d to %d\n", ast->nativeformats, p->chan->nativeformats); \
			/* Native formats changed, reset things */ \
			ast->nativeformats = p->chan->nativeformats; \
			opbx_log(LOG_DEBUG, "Resetting read to %d and write to %d\n", ast->readformat, ast->writeformat);\
			opbx_set_read_format(ast, ast->readformat); \
			opbx_set_write_format(ast, ast->writeformat); \
		} \
		if (p->chan->readformat != ast->rawreadformat)  \
			opbx_set_read_format(p->chan, ast->rawreadformat); \
		if (p->chan->writeformat != ast->rawwriteformat) \
			opbx_set_write_format(p->chan, ast->rawwriteformat); \
	} \
} while(0)

/* Cleanup moves all the relevant FD's from the 2nd to the first, but retains things
   properly for a timingfd XXX This might need more work if agents were logged in as agents or other
   totally impractical combinations XXX */

#define CLEANUP(ast, p) do { \
	int x; \
	if (p->chan) { \
		for (x=0;x<OPBX_MAX_FDS;x++) {\
			if (x != OPBX_MAX_FDS - 2) \
				ast->fds[x] = p->chan->fds[x]; \
		} \
		ast->fds[OPBX_MAX_FDS - 3] = p->chan->fds[OPBX_MAX_FDS - 2]; \
	} \
} while(0)

static struct opbx_channel *agent_request(const char *type, int format, void *data, int *cause);
static int agent_devicestate(void *data);
static int agent_digit(struct opbx_channel *ast, char digit);
static int agent_call(struct opbx_channel *ast, char *dest, int timeout);
static int agent_hangup(struct opbx_channel *ast);
static int agent_answer(struct opbx_channel *ast);
static struct opbx_frame *agent_read(struct opbx_channel *ast);
static int agent_write(struct opbx_channel *ast, struct opbx_frame *f);
static int agent_sendhtml(struct opbx_channel *ast, int subclass, const char *data, int datalen);
static int agent_sendtext(struct opbx_channel *ast, const char *text);
static int agent_indicate(struct opbx_channel *ast, int condition);
static int agent_fixup(struct opbx_channel *oldchan, struct opbx_channel *newchan);
static struct opbx_channel *agent_bridgedchannel(struct opbx_channel *chan, struct opbx_channel *bridge);

static const struct opbx_channel_tech agent_tech = {
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
	char *argv[3];
	char *args;
	char *password = NULL;
	char *name = NULL;
	char *agt = NULL;
	struct agent_pvt *p, *prev;

	args = opbx_strdupa(agent);

	// Extract username (agt), password and name from agent (args).
	if ((argc = opbx_separate_app_args(args, ',', argv, sizeof(argv) / sizeof(argv[0])))) {
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
		opbx_log(LOG_WARNING, "A blank agent line!\n");
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
			opbx_copy_string(p->agent, agt, sizeof(p->agent));
			opbx_mutex_init(&p->lock);
			opbx_mutex_init(&p->app_lock);
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
	
	opbx_copy_string(p->password, password ? password : "", sizeof(p->password));
	opbx_copy_string(p->name, name ? name : "", sizeof(p->name));
	opbx_copy_string(p->moh, moh, sizeof(p->moh));
	p->ackcall = ackcall;
	p->autologoff = autologoff;

	/* If someone reduces the wrapuptime and reloads, we want it
	 * to change the wrapuptime immediately on all calls */
	if (p->wrapuptime > wrapuptime) {
		struct timeval now = opbx_tvnow();
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
	struct opbx_channel *chan = p->owner;
	p->owner = NULL;
	chan->tech_pvt = NULL;
	p->app_sleep_cond = 1;
	/* Release ownership of the agent to other threads (presumably running the login app). */
	if (opbx_strlen_zero(p->loginchan))
	    opbx_mutex_unlock(&p->app_lock);
	if (chan)
		opbx_channel_free(chan);
	if (p->dead) {
		opbx_mutex_destroy(&p->lock);
		opbx_mutex_destroy(&p->app_lock);
		free(p);
        }
	return 0;
}

static int check_availability(struct agent_pvt *newlyavailable, int needlock);

static int agent_answer(struct opbx_channel *ast)
{
	opbx_log(LOG_WARNING, "Huh?  Agent is being asked to answer?\n");
	return -1;
}

static int __agent_start_monitoring(struct opbx_channel *ast, struct agent_pvt *p, int needlock)
{
	char tmp[OPBX_MAX_BUF],tmp2[OPBX_MAX_BUF], *pointer;
	char filename[OPBX_MAX_BUF];
	int res = -1;
	if (!p)
		return -1;
	if (!ast->monitor) {
		snprintf(filename, sizeof(filename), "agent-%s-%s",p->agent, ast->uniqueid);
		/* substitute . for - */
		if ((pointer = strchr(filename, '.')))
			*pointer = '-';
		snprintf(tmp, sizeof(tmp), "%s%s",savecallsin ? savecallsin : "", filename);
		opbx_monitor_start(ast, recordformat, tmp, needlock);
		opbx_monitor_setjoinfiles(ast, 1);
		snprintf(tmp2, sizeof(tmp2), "%s%s.%s", urlprefix ? urlprefix : "", filename, recordformatext);
#if 0
		opbx_verbose("name is %s, link is %s\n",tmp, tmp2);
#endif
		if (!ast->cdr)
			ast->cdr = opbx_cdr_alloc();
		opbx_cdr_setuserfield(ast, tmp2);
		res = 0;
	} else
		opbx_log(LOG_ERROR, "Recording already started on that call.\n");
	return res;
}

static int agent_start_monitoring(struct opbx_channel *ast, int needlock)
{
	return __agent_start_monitoring(ast, ast->tech_pvt, needlock);
}

static struct opbx_frame *agent_read(struct opbx_channel *ast)
{
	struct agent_pvt *p = ast->tech_pvt;
	struct opbx_frame *f = NULL;
	static struct opbx_frame null_frame = { OPBX_FRAME_NULL, };
	static struct opbx_frame answer_frame = { OPBX_FRAME_CONTROL, OPBX_CONTROL_ANSWER };
	opbx_mutex_lock(&p->lock); 
	CHECK_FORMATS(ast, p);
	if (p->chan) {
		opbx_copy_flags(p->chan, ast, OPBX_FLAG_EXCEPTION);
		if (ast->fdno == OPBX_MAX_FDS - 3)
			p->chan->fdno = OPBX_MAX_FDS - 2;
		else
			p->chan->fdno = ast->fdno;
		f = opbx_read(p->chan);
	} else
		f = &null_frame;
	if (!f) {
		/* If there's a channel, hang it up (if it's on a callback) make it NULL */
		if (p->chan) {
			p->chan->_bridge = NULL;
			/* Note that we don't hangup if it's not a callback because OpenPBX will do it
			   for us when the PBX instance that called login finishes */
			if (!opbx_strlen_zero(p->loginchan)) {
				if (p->chan)
					opbx_log(LOG_DEBUG, "Bridge on '%s' being cleared (2)\n", p->chan->name);
				opbx_hangup(p->chan);
				if (p->wrapuptime && p->acknowledged)
					p->lastdisc = opbx_tvadd(opbx_tvnow(), opbx_samp2tv(p->wrapuptime, 1000));
			}
			p->chan = NULL;
			p->acknowledged = 0;
		}
 	} else {
 		/* if acknowledgement is not required, and the channel is up, we may have missed
 		   an OPBX_CONTROL_ANSWER (if there was one), so mark the call acknowledged anyway */
 		if (!p->ackcall && !p->acknowledged && p->chan && (p->chan->_state == OPBX_STATE_UP))
  			p->acknowledged = 1;
 		switch (f->frametype) {
 		case OPBX_FRAME_CONTROL:
 			if (f->subclass == OPBX_CONTROL_ANSWER) {
 				if (p->ackcall) {
 					if (option_verbose > 2)
 						opbx_verbose(VERBOSE_PREFIX_3 "%s answered, waiting for '#' to acknowledge\n", p->chan->name);
 					/* Don't pass answer along */
 					opbx_frfree(f);
 					f = &null_frame;
 				} else {
 					p->acknowledged = 1;
 					/* Use the builtin answer frame for the 
					   recording start check below. */
 					opbx_frfree(f);
 					f = &answer_frame;
 				}
 			}
 			break;
 		case OPBX_FRAME_DTMF:
 			if (!p->acknowledged && (f->subclass == '#')) {
 				if (option_verbose > 2)
 					opbx_verbose(VERBOSE_PREFIX_3 "%s acknowledged\n", p->chan->name);
 				p->acknowledged = 1;
 				opbx_frfree(f);
 				f = &answer_frame;
 			} else if (f->subclass == '*') {
 				/* terminates call */
 				opbx_frfree(f);
 				f = NULL;
 			}
 			break;
 		case OPBX_FRAME_VOICE:
 			/* don't pass voice until the call is acknowledged */
 			if (!p->acknowledged) {
 				opbx_frfree(f);
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
				opbx_log(LOG_DEBUG, "Bridge on '%s' being set to '%s' (3)\n", p->chan->name, p->chan->_bridge->name);
		}
	}
	opbx_mutex_unlock(&p->lock);
	if (recordagentcalls && f == &answer_frame)
		agent_start_monitoring(ast,0);
	return f;
}

static int agent_sendhtml(struct opbx_channel *ast, int subclass, const char *data, int datalen)
{
	struct agent_pvt *p = ast->tech_pvt;
	int res = -1;
	opbx_mutex_lock(&p->lock);
	if (p->chan) 
		res = opbx_channel_sendhtml(p->chan, subclass, data, datalen);
	opbx_mutex_unlock(&p->lock);
	return res;
}

static int agent_sendtext(struct opbx_channel *ast, const char *text)
{
	struct agent_pvt *p = ast->tech_pvt;
	int res = -1;
	opbx_mutex_lock(&p->lock);
	if (p->chan) 
		res = opbx_sendtext(p->chan, text);
	opbx_mutex_unlock(&p->lock);
	return res;
}

static int agent_write(struct opbx_channel *ast, struct opbx_frame *f)
{
	struct agent_pvt *p = ast->tech_pvt;
	int res = -1;
	CHECK_FORMATS(ast, p);
	opbx_mutex_lock(&p->lock);
	if (p->chan) {
		if ((f->frametype != OPBX_FRAME_VOICE) ||
		    (f->subclass == p->chan->writeformat)) {
			res = opbx_write(p->chan, f);
		} else {
			opbx_log(LOG_DEBUG, "Dropping one incompatible voice frame on '%s' to '%s'\n", ast->name, p->chan->name);
			res = 0;
		}
	} else
		res = 0;
	CLEANUP(ast, p);
	opbx_mutex_unlock(&p->lock);
	return res;
}

static int agent_fixup(struct opbx_channel *oldchan, struct opbx_channel *newchan)
{
	struct agent_pvt *p = newchan->tech_pvt;
	opbx_mutex_lock(&p->lock);
	if (p->owner != oldchan) {
		opbx_log(LOG_WARNING, "old channel wasn't %p but was %p\n", oldchan, p->owner);
		opbx_mutex_unlock(&p->lock);
		return -1;
	}
	p->owner = newchan;
	opbx_mutex_unlock(&p->lock);
	return 0;
}

static int agent_indicate(struct opbx_channel *ast, int condition)
{
	struct agent_pvt *p = ast->tech_pvt;
	int res = -1;
	opbx_mutex_lock(&p->lock);
	if (p->chan)
		res = opbx_indicate(p->chan, condition);
	else
		res = 0;
	opbx_mutex_unlock(&p->lock);
	return res;
}

static int agent_digit(struct opbx_channel *ast, char digit)
{
	struct agent_pvt *p = ast->tech_pvt;
	int res = -1;
	opbx_mutex_lock(&p->lock);
	if (p->chan)
		res = p->chan->tech->send_digit(p->chan, digit);
	else
		res = 0;
	opbx_mutex_unlock(&p->lock);
	return res;
}

static int agent_call(struct opbx_channel *ast, char *dest, int timeout)
{
	struct agent_pvt *p = ast->tech_pvt;
	int res = -1;
	int newstate=0;
	opbx_mutex_lock(&p->lock);
	p->acknowledged = 0;
	if (!p->chan) {
		if (p->pending) {
			opbx_log(LOG_DEBUG, "Pretending to dial on pending agent\n");
			newstate = OPBX_STATE_DIALING;
			res = 0;
		} else {
			opbx_log(LOG_NOTICE, "Whoa, they hung up between alloc and call...  what are the odds of that?\n");
			res = -1;
		}
		opbx_mutex_unlock(&p->lock);
		if (newstate)
			opbx_setstate(ast, newstate);
		return res;
	} else if (!opbx_strlen_zero(p->loginchan)) {
		time(&p->start);
		/* Call on this agent */
		if (option_verbose > 2)
			opbx_verbose(VERBOSE_PREFIX_3 "outgoing agentcall, to agent '%s', on '%s'\n", p->agent, p->chan->name);
		opbx_set_callerid(p->chan, ast->cid.cid_num, ast->cid.cid_name, NULL);
		opbx_channel_inherit_variables(ast, p->chan);
		res = opbx_call(p->chan, p->loginchan, 0);
		CLEANUP(ast,p);
		opbx_mutex_unlock(&p->lock);
		return res;
	}
	opbx_verbose( VERBOSE_PREFIX_3 "agent_call, call to agent '%s' call on '%s'\n", p->agent, p->chan->name);
	opbx_log( LOG_DEBUG, "Playing beep, lang '%s'\n", p->chan->language);
	res = opbx_streamfile(p->chan, beep, p->chan->language);
	opbx_log( LOG_DEBUG, "Played beep, result '%d'\n", res);
	if (!res) {
		res = opbx_waitstream(p->chan, "");
		opbx_log( LOG_DEBUG, "Waited for stream, result '%d'\n", res);
	}
	if (!res) {
		res = opbx_set_read_format(p->chan, opbx_best_codec(p->chan->nativeformats));
		opbx_log( LOG_DEBUG, "Set read format, result '%d'\n", res);
		if (res)
			opbx_log(LOG_WARNING, "Unable to set read format to %s\n", opbx_getformatname(opbx_best_codec(p->chan->nativeformats)));
	} else {
		/* Agent hung-up */
		p->chan = NULL;
	}

	if (!res) {
		opbx_set_write_format(p->chan, opbx_best_codec(p->chan->nativeformats));
		opbx_log( LOG_DEBUG, "Set write format, result '%d'\n", res);
		if (res)
			opbx_log(LOG_WARNING, "Unable to set write format to %s\n", opbx_getformatname(opbx_best_codec(p->chan->nativeformats)));
	}
	if( !res )
	{
		/* Call is immediately up, or might need ack */
		if (p->ackcall > 1)
			newstate = OPBX_STATE_RINGING;
		else {
			newstate = OPBX_STATE_UP;
			if (recordagentcalls)
				agent_start_monitoring(ast,0);
			p->acknowledged = 1;
		}
		res = 0;
	}
	CLEANUP(ast,p);
	opbx_mutex_unlock(&p->lock);
	if (newstate)
		opbx_setstate(ast, newstate);
	return res;
}

/* store/clear the global variable that stores agentid based on the callerid */
static void set_agentbycallerid(const char *callerid, const char *agent)
{
	char buf[OPBX_MAX_BUF];

	/* if there is no Caller ID, nothing to do */
	if (opbx_strlen_zero(callerid))
		return;

	snprintf(buf, sizeof(buf), "%s_%s",GETAGENTBYCALLERID, callerid);
	pbx_builtin_setvar_helper(NULL, buf, agent);
}

static int agent_hangup(struct opbx_channel *ast)
{
	struct agent_pvt *p = ast->tech_pvt;
	int howlong = 0;
	opbx_mutex_lock(&p->lock);
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

	opbx_mutex_lock(&usecnt_lock);
	usecnt--;
	opbx_mutex_unlock(&usecnt_lock);

	opbx_log(LOG_DEBUG, "Hangup called for state %s\n", opbx_state2str(ast->_state));
	if (p->start && (ast->_state != OPBX_STATE_UP)) {
		howlong = time(NULL) - p->start;
		p->start = 0;
	} else if (ast->_state == OPBX_STATE_RESERVED) {
		howlong = 0;
	} else
		p->start = 0; 
	if (p->chan) {
		p->chan->_bridge = NULL;
		/* If they're dead, go ahead and hang up on the agent now */
		if (!opbx_strlen_zero(p->loginchan)) {
			/* Store last disconnect time */
			if (p->wrapuptime)
				p->lastdisc = opbx_tvadd(opbx_tvnow(), opbx_samp2tv(p->wrapuptime, 1000));
			else
				p->lastdisc = opbx_tv(0,0);
			if (p->chan) {
				/* Recognize the hangup and pass it along immediately */
				opbx_hangup(p->chan);
				p->chan = NULL;
			}
			opbx_log(LOG_DEBUG, "Hungup, howlong is %d, autologoff is %d\n", howlong, p->autologoff);
			if (howlong  && p->autologoff && (howlong > p->autologoff)) {
				char agent[OPBX_MAX_AGENT] = "";
				long logintime = time(NULL) - p->loginstart;
				p->loginstart = 0;
				opbx_log(LOG_NOTICE, "Agent '%s' didn't answer/confirm within %d seconds (waited %d)\n", p->name, p->autologoff, howlong);
				manager_event(EVENT_FLAG_AGENT, "Agentcallbacklogoff",
					      "Agent: %s\r\n"
					      "Loginchan: %s\r\n"
					      "Logintime: %ld\r\n"
					      "Reason: Autologoff\r\n"
					      "Uniqueid: %s\r\n",
					      p->agent, p->loginchan, logintime, ast->uniqueid);
				snprintf(agent, sizeof(agent), "Agent/%s", p->agent);
				opbx_queue_log("NONE", ast->uniqueid, agent, "AGENTCALLBACKLOGOFF", "%s|%ld|%s", p->loginchan, logintime, "Autologoff");
				set_agentbycallerid(p->logincallerid, NULL);
				opbx_device_state_changed("Agent/%s", p->agent);
				p->loginchan[0] = '\0';
				p->logincallerid[0] = '\0';
				if (persistent_agents)
					dump_agents();
			}
		} else if (p->dead) {
			opbx_mutex_lock(&p->chan->lock);
			opbx_softhangup(p->chan, OPBX_SOFTHANGUP_EXPLICIT);
			opbx_mutex_unlock(&p->chan->lock);
		} else if (p->loginstart) {
			opbx_mutex_lock(&p->chan->lock);
			opbx_moh_start(p->chan, p->moh);
			opbx_mutex_unlock(&p->chan->lock);
		}
	}
	opbx_mutex_unlock(&p->lock);
	/* Only register a device state change if the agent is still logged in */
	if (p->loginstart)
		opbx_device_state_changed("Agent/%s", p->agent);

	if (p->pending) {
		opbx_mutex_lock(&agentlock);
		agent_unlink(p);
		opbx_mutex_unlock(&agentlock);
	}
	if (p->abouttograb) {
		/* Let the "about to grab" thread know this isn't valid anymore, and let it
		   kill it later */
		p->abouttograb = 0;
	} else if (p->dead) {
		opbx_mutex_destroy(&p->lock);
		opbx_mutex_destroy(&p->app_lock);
		free(p);
	} else {
		if (p->chan) {
			/* Not dead -- check availability now */
			opbx_mutex_lock(&p->lock);
			/* Store last disconnect time */
			p->lastdisc = opbx_tvnow();
			opbx_mutex_unlock(&p->lock);
		}
		/* Release ownership of the agent to other threads (presumably running the login app). */
		opbx_mutex_unlock(&p->app_lock);
	}
	return 0;
}

static int agent_cont_sleep( void *data )
{
	struct agent_pvt *p;
	int res;

	p = (struct agent_pvt *)data;

	opbx_mutex_lock(&p->lock);
	res = p->app_sleep_cond;
	if (p->lastdisc.tv_sec) {
		if (opbx_tvdiff_ms(opbx_tvnow(), p->lastdisc) > p->wrapuptime) 
			res = 1;
	}
	opbx_mutex_unlock(&p->lock);
#if 0
	if( !res )
		opbx_log( LOG_DEBUG, "agent_cont_sleep() returning %d\n", res );
#endif		
	return res;
}

static int agent_ack_sleep( void *data )
{
	struct agent_pvt *p;
	int res=0;
	int to = 1000;
	struct opbx_frame *f;

	/* Wait a second and look for something */

	p = (struct agent_pvt *)data;
	if (p->chan) {
		for(;;) {
			to = opbx_waitfor(p->chan, to);
			if (to < 0) {
				res = -1;
				break;
			}
			if (!to) {
				res = 0;
				break;
			}
			f = opbx_read(p->chan);
			if (!f) {
				res = -1;
				break;
			}
			if (f->frametype == OPBX_FRAME_DTMF)
				res = f->subclass;
			else
				res = 0;
			opbx_frfree(f);
			opbx_mutex_lock(&p->lock);
			if (!p->app_sleep_cond) {
				opbx_mutex_unlock(&p->lock);
				res = 0;
				break;
			} else if (res == '#') {
				opbx_mutex_unlock(&p->lock);
				res = 1;
				break;
			}
			opbx_mutex_unlock(&p->lock);
			res = 0;
		}
	} else
		res = -1;
	return res;
}

static struct opbx_channel *agent_bridgedchannel(struct opbx_channel *chan, struct opbx_channel *bridge)
{
	struct agent_pvt *p = bridge->tech_pvt;
	struct opbx_channel *ret=NULL;
	

	if(p) {
		if (chan == p->chan)
			ret = bridge->_bridge;
		else if (chan == bridge->_bridge)
			ret = p->chan;
	}

	if (option_debug)
		opbx_log(LOG_DEBUG, "Asked for bridged channel on '%s'/'%s', returning '%s'\n", chan->name, bridge->name, ret ? ret->name : "<none>");
	return ret;
}

/*--- agent_new: Create new agent channel ---*/
static struct opbx_channel *agent_new(struct agent_pvt *p, int state)
{
	struct opbx_channel *tmp;
	struct opbx_frame null_frame = { OPBX_FRAME_NULL };
#if 0
	if (!p->chan) {
		opbx_log(LOG_WARNING, "No channel? :(\n");
		return NULL;
	}
#endif	
	tmp = opbx_channel_alloc(0);
	if (tmp) {
		tmp->tech = &agent_tech;
		if (p->chan) {
			tmp->nativeformats = p->chan->nativeformats;
			tmp->writeformat = p->chan->writeformat;
			tmp->rawwriteformat = p->chan->writeformat;
			tmp->readformat = p->chan->readformat;
			tmp->rawreadformat = p->chan->readformat;
			opbx_copy_string(tmp->language, p->chan->language, sizeof(tmp->language));
			opbx_copy_string(tmp->context, p->chan->context, sizeof(tmp->context));
			opbx_copy_string(tmp->exten, p->chan->exten, sizeof(tmp->exten));
		} else {
			tmp->nativeformats = OPBX_FORMAT_SLINEAR;
			tmp->writeformat = OPBX_FORMAT_SLINEAR;
			tmp->rawwriteformat = OPBX_FORMAT_SLINEAR;
			tmp->readformat = OPBX_FORMAT_SLINEAR;
			tmp->rawreadformat = OPBX_FORMAT_SLINEAR;
		}
		if (p->pending)
			snprintf(tmp->name, sizeof(tmp->name), "Agent/P%s-%ld", p->agent, opbx_random() & 0xffff);
		else
			snprintf(tmp->name, sizeof(tmp->name), "Agent/%s", p->agent);
		tmp->type = channeltype;
		/* Safe, agentlock already held */
		opbx_setstate(tmp, state);
		tmp->tech_pvt = p;
		p->owner = tmp;
		opbx_mutex_lock(&usecnt_lock);
		usecnt++;
		opbx_mutex_unlock(&usecnt_lock);
		opbx_update_use_count();
		tmp->priority = 1;
		/* Wake up and wait for other applications (by definition the login app)
		 * to release this channel). Takes ownership of the agent channel
		 * to this thread only.
		 * For signalling the other thread, opbx_queue_frame is used until we
		 * can safely use signals for this purpose. The pselect() needs to be
		 * implemented in the kernel for this.
		 */
		p->app_sleep_cond = 0;
		if( opbx_strlen_zero(p->loginchan) && opbx_mutex_trylock(&p->app_lock) )
		{
			if (p->chan) {
				opbx_queue_frame(p->chan, &null_frame);
				opbx_mutex_unlock(&p->lock);	/* For other thread to read the condition. */
				opbx_mutex_lock(&p->app_lock);
				opbx_mutex_lock(&p->lock);
			}
			if( !p->chan )
			{
				opbx_log(LOG_WARNING, "Agent disconnected while we were connecting the call\n");
				p->owner = NULL;
				tmp->tech_pvt = NULL;
				p->app_sleep_cond = 1;
				opbx_channel_free( tmp );
				opbx_mutex_unlock(&p->lock);	/* For other thread to read the condition. */
				opbx_mutex_unlock(&p->app_lock);
				return NULL;
			}
		} else if (!opbx_strlen_zero(p->loginchan)) {
			if (p->chan)
				opbx_queue_frame(p->chan, &null_frame);
			if (!p->chan) {
				opbx_log(LOG_WARNING, "Agent disconnected while we were connecting the call\n");
				p->owner = NULL;
				tmp->tech_pvt = NULL;
				p->app_sleep_cond = 1;
				opbx_channel_free( tmp );
				opbx_mutex_unlock(&p->lock);     /* For other thread to read the condition. */
                                return NULL;
			}	
		}
		p->owning_app = pthread_self();
		/* After the above step, there should not be any blockers. */
		if (p->chan) {
			if (opbx_test_flag(p->chan, OPBX_FLAG_BLOCKING)) {
				opbx_log( LOG_ERROR, "A blocker exists after agent channel ownership acquired\n" );
				CRASH;
			}
			opbx_moh_stop(p->chan);
		}
	} else
		opbx_log(LOG_WARNING, "Unable to allocate agent channel structure\n");
	return tmp;
}


/**
 * Read configuration data. The file named agents.conf.
 *
 * @returns Always 0, or so it seems.
 */
static int read_agent_config(void)
{
	struct opbx_config *cfg;
	struct opbx_variable *v;
	struct agent_pvt *p, *pl, *pn;
	char *general_val;

	group = 0;
	autologoff = 0;
	wrapuptime = 0;
	ackcall = 0;
	cfg = opbx_config_load(config);
	if (!cfg) {
		opbx_log(LOG_NOTICE, "No agent configuration found -- agent support disabled\n");
		return 0;
	}
	opbx_mutex_lock(&agentlock);
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
	if ((general_val = opbx_variable_retrieve(cfg, "general", "persistentagents")))
		persistent_agents = opbx_true(general_val);

	/* Read in the [agents] section */
	v = opbx_variable_browse(cfg, "agents");
	while(v) {
		/* Create the interface list */
		if (!strcasecmp(v->name, "agent")) {
			add_agent(v->value, 0);
		} else if (!strcasecmp(v->name, "group")) {
			group = opbx_get_group(v->value);
		} else if (!strcasecmp(v->name, "autologoff")) {
			autologoff = atoi(v->value);
			if (autologoff < 0)
				autologoff = 0;
		} else if (!strcasecmp(v->name, "ackcall")) {
			if (!strcasecmp(v->value, "always"))
				ackcall = 2;
			else if (opbx_true(v->value))
				ackcall = 1;
			else
				ackcall = 0;
		} else if (!strcasecmp(v->name, "wrapuptime")) {
			wrapuptime = atoi(v->value);
			if (wrapuptime < 0)
				wrapuptime = 0;
		} else if (!strcasecmp(v->name, "maxlogintries") && !opbx_strlen_zero(v->value)) {
			maxlogintries = atoi(v->value);
			if (maxlogintries < 0)
				maxlogintries = 0;
		} else if (!strcasecmp(v->name, "goodbye") && !opbx_strlen_zero(v->value)) {
			strcpy(agentgoodbye,v->value);
		} else if (!strcasecmp(v->name, "musiconhold")) {
			opbx_copy_string(moh, v->value, sizeof(moh));
		} else if (!strcasecmp(v->name, "updatecdr")) {
			if (opbx_true(v->value))
				updatecdr = 1;
			else
				updatecdr = 0;
		} else if (!strcasecmp(v->name, "recordagentcalls")) {
			recordagentcalls = opbx_true(v->value);
		} else if (!strcasecmp(v->name, "createlink")) {
			createlink = opbx_true(v->value);
		} else if (!strcasecmp(v->name, "recordformat")) {
			opbx_copy_string(recordformat, v->value, sizeof(recordformat));
			if (!strcasecmp(v->value, "wav49"))
				strcpy(recordformatext, "WAV");
			else
				opbx_copy_string(recordformatext, v->value, sizeof(recordformatext));
		} else if (!strcasecmp(v->name, "urlprefix")) {
			opbx_copy_string(urlprefix, v->value, sizeof(urlprefix));
			if (urlprefix[strlen(urlprefix) - 1] != '/')
				strncat(urlprefix, "/", sizeof(urlprefix) - strlen(urlprefix) - 1);
		} else if (!strcasecmp(v->name, "savecallsin")) {
			if (v->value[0] == '/')
				opbx_copy_string(savecallsin, v->value, sizeof(savecallsin));
			else
				snprintf(savecallsin, sizeof(savecallsin) - 2, "/%s", v->value);
			if (savecallsin[strlen(savecallsin) - 1] != '/')
				strncat(savecallsin, "/", sizeof(savecallsin) - strlen(savecallsin) - 1);
		} else if (!strcasecmp(v->name, "custom_beep")) {
			opbx_copy_string(beep, v->value, sizeof(beep));
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
					opbx_mutex_destroy(&p->lock);
					opbx_mutex_destroy(&p->app_lock);
					free(p);
				} else {
					/* Cause them to hang up */
					opbx_softhangup(p->chan, OPBX_SOFTHANGUP_EXPLICIT);
				}
			}
		} else
			pl = p;
		p = pn;
	}
	opbx_mutex_unlock(&agentlock);
	opbx_config_destroy(cfg);
	return 0;
}

static int check_availability(struct agent_pvt *newlyavailable, int needlock)
{
	struct opbx_channel *chan=NULL, *parent=NULL;
	struct agent_pvt *p;
	int res;

	if (option_debug)
		opbx_log(LOG_DEBUG, "Checking availability of '%s'\n", newlyavailable->agent);
	if (needlock)
		opbx_mutex_lock(&agentlock);
	p = agents;
	while(p) {
		if (p == newlyavailable) {
			p = p->next;
			continue;
		}
		opbx_mutex_lock(&p->lock);
		if (!p->abouttograb && p->pending && ((p->group && (newlyavailable->group & p->group)) || !strcmp(p->agent, newlyavailable->agent))) {
			if (option_debug)
				opbx_log(LOG_DEBUG, "Call '%s' looks like a winner for agent '%s'\n", p->owner->name, newlyavailable->agent);
			/* We found a pending call, time to merge */
			chan = agent_new(newlyavailable, OPBX_STATE_DOWN);
			parent = p->owner;
			p->abouttograb = 1;
			opbx_mutex_unlock(&p->lock);
			break;
		}
		opbx_mutex_unlock(&p->lock);
		p = p->next;
	}
	if (needlock)
		opbx_mutex_unlock(&agentlock);
	if (parent && chan)  {
		if (newlyavailable->ackcall > 1) {
			/* Don't do beep here */
			res = 0;
		} else {
			if (option_debug > 2)
				opbx_log( LOG_DEBUG, "Playing beep, lang '%s'\n", newlyavailable->chan->language);
			res = opbx_streamfile(newlyavailable->chan, beep, newlyavailable->chan->language);
			if (option_debug > 2)
				opbx_log( LOG_DEBUG, "Played beep, result '%d'\n", res);
			if (!res) {
				res = opbx_waitstream(newlyavailable->chan, "");
				opbx_log( LOG_DEBUG, "Waited for stream, result '%d'\n", res);
			}
		}
		if (!res) {
			/* Note -- parent may have disappeared */
			if (p->abouttograb) {
				newlyavailable->acknowledged = 1;
				/* Safe -- agent lock already held */
				opbx_setstate(parent, OPBX_STATE_UP);
				opbx_setstate(chan, OPBX_STATE_UP);
				opbx_copy_string(parent->context, chan->context, sizeof(parent->context));
				/* Go ahead and mark the channel as a zombie so that masquerade will
				   destroy it for us, and we need not call opbx_hangup */
				opbx_mutex_lock(&parent->lock);
				opbx_set_flag(chan, OPBX_FLAG_ZOMBIE);
				opbx_channel_masquerade(parent, chan);
				opbx_mutex_unlock(&parent->lock);
				p->abouttograb = 0;
			} else {
				if (option_debug)
					opbx_log(LOG_DEBUG, "Sneaky, parent disappeared in the mean time...\n");
				agent_cleanup(newlyavailable);
			}
		} else {
			if (option_debug)
				opbx_log(LOG_DEBUG, "Ugh...  Agent hung up at exactly the wrong time\n");
			agent_cleanup(newlyavailable);
		}
	}
	return 0;
}

static int check_beep(struct agent_pvt *newlyavailable, int needlock)
{
	struct agent_pvt *p;
	int res=0;

	opbx_log(LOG_DEBUG, "Checking beep availability of '%s'\n", newlyavailable->agent);
	if (needlock)
		opbx_mutex_lock(&agentlock);
	p = agents;
	while(p) {
		if (p == newlyavailable) {
			p = p->next;
			continue;
		}
		opbx_mutex_lock(&p->lock);
		if (!p->abouttograb && p->pending && ((p->group && (newlyavailable->group & p->group)) || !strcmp(p->agent, newlyavailable->agent))) {
			if (option_debug)
				opbx_log(LOG_DEBUG, "Call '%s' looks like a would-be winner for agent '%s'\n", p->owner->name, newlyavailable->agent);
			opbx_mutex_unlock(&p->lock);
			break;
		}
		opbx_mutex_unlock(&p->lock);
		p = p->next;
	}
	if (needlock)
		opbx_mutex_unlock(&agentlock);
	if (p) {
		opbx_mutex_unlock(&newlyavailable->lock);
		if (option_debug > 2)
			opbx_log( LOG_DEBUG, "Playing beep, lang '%s'\n", newlyavailable->chan->language);
		res = opbx_streamfile(newlyavailable->chan, beep, newlyavailable->chan->language);
		if (option_debug > 2)
			opbx_log( LOG_DEBUG, "Played beep, result '%d'\n", res);
		if (!res) {
			res = opbx_waitstream(newlyavailable->chan, "");
			if (option_debug)
				opbx_log( LOG_DEBUG, "Waited for stream, result '%d'\n", res);
		}
		opbx_mutex_lock(&newlyavailable->lock);
	}
	return res;
}

/*--- agent_request: Part of the OpenPBX interface ---*/
static struct opbx_channel *agent_request(const char *type, int format, void *data, int *cause)
{
	struct agent_pvt *p;
	struct opbx_channel *chan = NULL;
	char *s;
	opbx_group_t groupmatch;
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
	opbx_mutex_lock(&agentlock);
	p = agents;
	while(p) {
		opbx_mutex_lock(&p->lock);
		if (!p->pending && ((groupmatch && (p->group & groupmatch)) || !strcmp(data, p->agent)) &&
		    opbx_strlen_zero(p->loginchan)) {
			if (p->chan)
				hasagent++;
			if (!p->lastdisc.tv_sec) {
				/* Agent must be registered, but not have any active call, and not be in a waiting state */
				if (!p->owner && p->chan) {
					/* Fixed agent */
					chan = agent_new(p, OPBX_STATE_DOWN);
				}
				if (chan) {
					opbx_mutex_unlock(&p->lock);
					break;
				}
			}
		}
		opbx_mutex_unlock(&p->lock);
		p = p->next;
	}
	if (!p) {
		p = agents;
		while(p) {
			opbx_mutex_lock(&p->lock);
			if (!p->pending && ((groupmatch && (p->group & groupmatch)) || !strcmp(data, p->agent))) {
				if (p->chan || !opbx_strlen_zero(p->loginchan))
					hasagent++;
				tv = opbx_tvnow();
#if 0
				opbx_log(LOG_NOTICE, "Time now: %ld, Time of lastdisc: %ld\n", tv.tv_sec, p->lastdisc.tv_sec);
#endif
				if (!p->lastdisc.tv_sec || (tv.tv_sec > p->lastdisc.tv_sec)) {
					p->lastdisc = opbx_tv(0, 0);
					/* Agent must be registered, but not have any active call, and not be in a waiting state */
					if (!p->owner && p->chan) {
						/* Could still get a fixed agent */
						chan = agent_new(p, OPBX_STATE_DOWN);
					} else if (!p->owner && !opbx_strlen_zero(p->loginchan)) {
						/* Adjustable agent */
						p->chan = opbx_request("Local", format, p->loginchan, cause);
						if (p->chan)
							chan = agent_new(p, OPBX_STATE_DOWN);
					}
					if (chan) {
						opbx_mutex_unlock(&p->lock);
						break;
					}
				}
			}
			opbx_mutex_unlock(&p->lock);
			p = p->next;
		}
	}

	if (!chan && waitforagent) {
		/* No agent available -- but we're requesting to wait for one.
		   Allocate a place holder */
		if (hasagent) {
			if (option_debug)
				opbx_log(LOG_DEBUG, "Creating place holder for '%s'\n", s);
			p = add_agent(data, 1);
			p->group = groupmatch;
			chan = agent_new(p, OPBX_STATE_DOWN);
			if (!chan) {
				opbx_log(LOG_WARNING, "Weird...  Fix this to drop the unused pending agent\n");
			}
		} else
			opbx_log(LOG_DEBUG, "Not creating place holder for '%s' since nobody logged in\n", s);
	}
	if (hasagent)
		*cause = OPBX_CAUSE_BUSY;
	else
		*cause = OPBX_CAUSE_UNREGISTERED;
	opbx_mutex_unlock(&agentlock);
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

	if (!opbx_strlen_zero(id))
		snprintf(idText, sizeof(idText) ,"ActionID: %s\r\n", id);
	astman_send_ack(s, m, "Agents will follow");
	opbx_mutex_lock(&agentlock);
	p = agents;
	while(p) {
        	opbx_mutex_lock(&p->lock);

		/* Status Values:
		   AGENT_LOGGEDOFF - Agent isn't logged in
		   AGENT_IDLE      - Agent is logged in, and waiting for call
		   AGENT_ONCALL    - Agent is logged in, and on a call
		   AGENT_UNKNOWN   - Don't know anything about agent. Shouldn't ever get this. */

		if(!opbx_strlen_zero(p->name)) {
			username = p->name;
		} else {
			username = "None";
		}

		/* Set a default status. It 'should' get changed. */
		status = "AGENT_UNKNOWN";

		if (!opbx_strlen_zero(p->loginchan) && !p->chan) {
			loginChan = p->loginchan;
			talkingtoChan = "n/a";
			status = "AGENT_IDLE";
			if (p->acknowledged) {
				snprintf(chanbuf, sizeof(chanbuf), " %s (Confirmed)", p->loginchan);
				loginChan = chanbuf;
			}
		} else if (p->chan) {
			loginChan = opbx_strdupa(p->chan->name);
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

		opbx_cli(s->fd, "Event: Agents\r\n"
			"Agent: %s\r\n"
			"Name: %s\r\n"
			"Status: %s\r\n"
			"LoggedInChan: %s\r\n"
			"LoggedInTime: %ld\r\n"
			"TalkingTo: %s\r\n"
			"%s"
			"\r\n",
			p->agent, username, status, loginChan, (long int)p->loginstart, talkingtoChan, idText);
		opbx_mutex_unlock(&p->lock);
		p = p->next;
	}
	opbx_mutex_unlock(&agentlock);
	opbx_cli(s->fd, "Event: AgentsComplete\r\n"
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
					opbx_softhangup(p->owner, OPBX_SOFTHANGUP_EXPLICIT);
				}
				if (p->chan) {
					opbx_softhangup(p->chan, OPBX_SOFTHANGUP_EXPLICIT);
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
			opbx_queue_log("NONE", "NONE", agent, "AGENTCALLBACKLOGOFF", "%s|%ld|%s", p->loginchan, logintime, "CommandLogoff");
			set_agentbycallerid(p->logincallerid, NULL);
			p->loginchan[0] = '\0';
			p->logincallerid[0] = '\0';
			opbx_device_state_changed("Agent/%s", p->agent);
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
		opbx_cli(fd, "Logging out %s\n", agent);

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

	if (opbx_strlen_zero(agent)) {
		astman_send_error(s, m, "No agent specified");
		return 0;
	}

	if (opbx_true(soft_s))
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
	char name[OPBX_MAX_AGENT];
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
	char username[OPBX_MAX_BUF];
	char location[OPBX_MAX_BUF] = "";
	char talkingto[OPBX_MAX_BUF] = "";
	char moh[OPBX_MAX_BUF];
	int count_agents = 0;		/* Number of agents configured */
	int online_agents = 0;		/* Number of online agents */
	int offline_agents = 0;		/* Number of offline agents */
	if (argc != 2)
		return RESULT_SHOWUSAGE;
	opbx_mutex_lock(&agentlock);
	p = agents;
	while(p) {
		opbx_mutex_lock(&p->lock);
		if (p->pending) {
			if (p->group)
				opbx_cli(fd, "-- Pending call to group %d\n", powerof(p->group));
			else
				opbx_cli(fd, "-- Pending call to agent %s\n", p->agent);
		} else {
			if (!opbx_strlen_zero(p->name))
				snprintf(username, sizeof(username), "(%s) ", p->name);
			else
				username[0] = '\0';
			if (p->chan) {
				snprintf(location, sizeof(location), "logged in on %s", p->chan->name);
				if (p->owner && opbx_bridged_channel(p->owner)) {
					snprintf(talkingto, sizeof(talkingto), " talking to %s", opbx_bridged_channel(p->owner)->name);
				} else {
					strcpy(talkingto, " is idle");
				}
				online_agents++;
			} else if (!opbx_strlen_zero(p->loginchan)) {
				if (opbx_tvdiff_ms(opbx_tvnow(), p->lastdisc) > 0 || !(p->lastdisc.tv_sec))
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
			if (!opbx_strlen_zero(p->moh))
				snprintf(moh, sizeof(moh), " (musiconhold is '%s')", p->moh);
			opbx_cli(fd, "%-12.12s %s%s%s%s\n", p->agent, 
				username, location, talkingto, moh);
			count_agents++;
		}
		opbx_mutex_unlock(&p->lock);
		p = p->next;
	}
	opbx_mutex_unlock(&agentlock);
	if ( !count_agents ) {
		opbx_cli(fd, "No Agents are configured in %s\n",config);
	} else {
		opbx_cli(fd, "%d agents configured [%d online , %d offline]\n",count_agents, online_agents, offline_agents);
	}
	opbx_cli(fd, "\n");
	                
	return RESULT_SUCCESS;
}

static char show_agents_usage[] = 
"Usage: show agents\n"
"       Provides summary information on agents.\n";

static char agent_logoff_usage[] =
"Usage: agent logoff <channel> [soft]\n"
"       Sets an agent as no longer logged in.\n"
"       If 'soft' is specified, do not hangup existing calls.\n";

static struct opbx_cli_entry cli_show_agents = {
	{ "show", "agents", NULL }, agents_show, 
	"Show status of agents", show_agents_usage, NULL };

static struct opbx_cli_entry cli_agent_logoff = {
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
static int __login_exec(struct opbx_channel *chan, void *data, int callbackmode)
{
	int res=0;
	int tries = 0;
	int max_login_tries = maxlogintries;
	struct agent_pvt *p;
	struct localuser *u;
	int login_state = 0;
	char user[OPBX_MAX_AGENT] = "";
	char pass[OPBX_MAX_AGENT];
	char agent[OPBX_MAX_AGENT] = "";
	char xpass[OPBX_MAX_AGENT] = "";
	char *errmsg;
	char *parse;
	char *opts;
	OPBX_DECLARE_APP_ARGS(args,
			     OPBX_APP_ARG(agent_id);
			     OPBX_APP_ARG(options);
			     OPBX_APP_ARG(extension);
		);
	char *tmpoptions = NULL;
	char *context = NULL;
	int play_announcement = 1;
	char agent_goodbye[OPBX_MAX_FILENAME_LEN];
	int update_cdr = updatecdr;
	char *filename = "agent-loginok";

	LOCAL_USER_ADD(u);

	if (!(parse = opbx_strdupa(data))) {
		opbx_log(LOG_ERROR, "Out of memory!\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	OPBX_STANDARD_APP_ARGS(args, parse);

	opbx_copy_string(agent_goodbye, agentgoodbye, sizeof(agent_goodbye));

	/* Set Channel Specific Login Overrides */
	if (pbx_builtin_getvar_helper(chan, "AGENTLMAXLOGINTRIES") && strlen(pbx_builtin_getvar_helper(chan, "AGENTLMAXLOGINTRIES"))) {
		max_login_tries = atoi(pbx_builtin_getvar_helper(chan, "AGENTMAXLOGINTRIES"));
		if (max_login_tries < 0)
			max_login_tries = 0;
		tmpoptions=pbx_builtin_getvar_helper(chan, "AGENTMAXLOGINTRIES");
		if (option_verbose > 2)
			opbx_verbose(VERBOSE_PREFIX_3 "Saw variable AGENTMAXLOGINTRIES=%s, setting max_login_tries to: %d on Channel '%s'.\n",tmpoptions,max_login_tries,chan->name);
	}
	if (pbx_builtin_getvar_helper(chan, "AGENTUPDATECDR") && !opbx_strlen_zero(pbx_builtin_getvar_helper(chan, "AGENTUPDATECDR"))) {
		if (opbx_true(pbx_builtin_getvar_helper(chan, "AGENTUPDATECDR")))
			update_cdr = 1;
		else
			update_cdr = 0;
		tmpoptions=pbx_builtin_getvar_helper(chan, "AGENTUPDATECDR");
		if (option_verbose > 2)
			opbx_verbose(VERBOSE_PREFIX_3 "Saw variable AGENTUPDATECDR=%s, setting update_cdr to: %d on Channel '%s'.\n",tmpoptions,update_cdr,chan->name);
	}
	if (pbx_builtin_getvar_helper(chan, "AGENTGOODBYE") && !opbx_strlen_zero(pbx_builtin_getvar_helper(chan, "AGENTGOODBYE"))) {
		strcpy(agent_goodbye, pbx_builtin_getvar_helper(chan, "AGENTGOODBYE"));
		tmpoptions=pbx_builtin_getvar_helper(chan, "AGENTGOODBYE");
		if (option_verbose > 2)
			opbx_verbose(VERBOSE_PREFIX_3 "Saw variable AGENTGOODBYE=%s, setting agent_goodbye to: %s on Channel '%s'.\n",tmpoptions,agent_goodbye,chan->name);
	}
	/* End Channel Specific Login Overrides */
	
	if (callbackmode && args.extension) {
		parse = args.extension;
		args.extension = strsep(&parse, "@");
		context = parse;
	}

	opts = args.options;
	if (!opbx_strlen_zero(opts)) {
		if (strchr(opts, 's')) {
			play_announcement = 0;
		}
	}

	if (chan->_state != OPBX_STATE_UP)
		res = opbx_answer(chan);
	if (!res) {
		if (!opbx_strlen_zero(args.agent_id))
			opbx_copy_string(user, args.agent_id, OPBX_MAX_AGENT);
		else
			res = opbx_app_getdata(chan, "agent-user", user, sizeof(user) - 1, 0);
	}
	while (!res && (max_login_tries==0 || tries < max_login_tries)) {
		tries++;
		/* Check for password */
		opbx_mutex_lock(&agentlock);
		p = agents;
		while(p) {
			if (!strcmp(p->agent, user) && !p->pending)
				opbx_copy_string(xpass, p->password, sizeof(xpass));
			p = p->next;
		}
		opbx_mutex_unlock(&agentlock);
		if (!res) {
			if (!opbx_strlen_zero(xpass))
				res = opbx_app_getdata(chan, "agent-pass", pass, sizeof(pass) - 1, 0);
			else
				pass[0] = '\0';
		}
		errmsg = "agent-incorrect";

#if 0
		opbx_log(LOG_NOTICE, "user: %s, pass: %s\n", user, pass);
#endif		

		/* Check again for accuracy */
		opbx_mutex_lock(&agentlock);
		p = agents;
		while(p) {
			opbx_mutex_lock(&p->lock);
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
					else if (opbx_true(pbx_builtin_getvar_helper(chan, "AGENTACKCALL")))
						p->ackcall = 1;
					else
						p->ackcall = 0;
					tmpoptions=pbx_builtin_getvar_helper(chan, "AGENTACKCALL");
					if (option_verbose > 2)
						opbx_verbose(VERBOSE_PREFIX_3 "Saw variable AGENTACKCALL=%s, setting ackcall to: %d for Agent '%s'.\n",tmpoptions,p->ackcall,p->agent);
				}
				if (pbx_builtin_getvar_helper(chan, "AGENTAUTOLOGOFF") && strlen(pbx_builtin_getvar_helper(chan, "AGENTAUTOLOGOFF"))) {
					p->autologoff = atoi(pbx_builtin_getvar_helper(chan, "AGENTAUTOLOGOFF"));
					if (p->autologoff < 0)
						p->autologoff = 0;
					tmpoptions=pbx_builtin_getvar_helper(chan, "AGENTAUTOLOGOFF");
					if (option_verbose > 2)
						opbx_verbose(VERBOSE_PREFIX_3 "Saw variable AGENTAUTOLOGOFF=%s, setting autologff to: %d for Agent '%s'.\n",tmpoptions,p->autologoff,p->agent);
				}
				if (pbx_builtin_getvar_helper(chan, "AGENTWRAPUPTIME") && strlen(pbx_builtin_getvar_helper(chan, "AGENTWRAPUPTIME"))) {
					p->wrapuptime = atoi(pbx_builtin_getvar_helper(chan, "AGENTWRAPUPTIME"));
					if (p->wrapuptime < 0)
						p->wrapuptime = 0;
					tmpoptions=pbx_builtin_getvar_helper(chan, "AGENTWRAPUPTIME");
					if (option_verbose > 2)
						opbx_verbose(VERBOSE_PREFIX_3 "Saw variable AGENTWRAPUPTIME=%s, setting wrapuptime to: %d for Agent '%s'.\n",tmpoptions,p->wrapuptime,p->agent);
				}
				/* End Channel Specific Agent Overides */
				if (!p->chan) {
					char lopbx_loginchan[80] = "";
					long logintime;
					snprintf(agent, sizeof(agent), "Agent/%s", p->agent);

					if (callbackmode) {
						char tmpchan[OPBX_MAX_BUF] = "";
						int pos = 0;
						/* Retrieve login chan */
						while (!args.extension || args.extension[0] != '#') {
							if (!opbx_strlen_zero(args.extension)) {
								opbx_copy_string(tmpchan, args.extension, sizeof(tmpchan));
								res = 0;
							} else
								res = opbx_app_getdata(chan, "agent-newlocation", tmpchan+pos, sizeof(tmpchan) - 2, 0);
							if (opbx_strlen_zero(tmpchan) || opbx_exists_extension(chan, !opbx_strlen_zero(context) ? context : "default", tmpchan,
													     1, NULL))
								break;
							if (args.extension) {
								opbx_log(LOG_WARNING, "Extension '%s' is not valid for automatic login of agent '%s'\n", args.extension, p->agent);
								args.extension = NULL;
								pos = 0;
							} else {
								opbx_log(LOG_WARNING, "Extension '%s@%s' is not valid for automatic login of agent '%s'\n", tmpchan, !opbx_strlen_zero(context) ? context : "default", p->agent);
								res = opbx_streamfile(chan, "invalid", chan->language);
								if (!res)
									res = opbx_waitstream(chan, OPBX_DIGIT_ANY);
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
						args.extension = tmpchan;
						if (!res) {
							set_agentbycallerid(p->logincallerid, NULL);
							if (!opbx_strlen_zero(context) && !opbx_strlen_zero(tmpchan))
								snprintf(p->loginchan, sizeof(p->loginchan), "%s@%s", tmpchan, context);
							else {
								opbx_copy_string(lopbx_loginchan, p->loginchan, sizeof(lopbx_loginchan));
								opbx_copy_string(p->loginchan, tmpchan, sizeof(p->loginchan));
							}
							p->acknowledged = 0;
							if (opbx_strlen_zero(p->loginchan)) {
								login_state = 2;
								filename = "agent-loggedoff";
							} else {
								if (chan->cid.cid_num) {
									opbx_copy_string(p->logincallerid, chan->cid.cid_num, sizeof(p->logincallerid));
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
					opbx_mutex_unlock(&p->lock);
					opbx_mutex_unlock(&agentlock);
					if( !res && play_announcement==1 )
						res = opbx_streamfile(chan, filename, chan->language);
					if (!res)
						opbx_waitstream(chan, "");
					opbx_mutex_lock(&agentlock);
					opbx_mutex_lock(&p->lock);
					if (!res) {
						res = opbx_set_read_format(chan, opbx_best_codec(chan->nativeformats));
						if (res)
							opbx_log(LOG_WARNING, "Unable to set read format to %d\n", opbx_best_codec(chan->nativeformats));
					}
					if (!res) {
						res = opbx_set_write_format(chan, opbx_best_codec(chan->nativeformats));
						if (res)
							opbx_log(LOG_WARNING, "Unable to set write format to %d\n", opbx_best_codec(chan->nativeformats));
					}
					/* Check once more just in case */
					if (p->chan)
						res = -1;
					if (callbackmode && !res) {
						/* Just say goodbye and be done with it */
						if (!opbx_strlen_zero(p->loginchan)) {
							if (p->loginstart == 0)
								time(&p->loginstart);
							manager_event(EVENT_FLAG_AGENT, "Agentcallbacklogin",
								      "Agent: %s\r\n"
								      "Loginchan: %s\r\n"
								      "Uniqueid: %s\r\n",
								      p->agent, p->loginchan, chan->uniqueid);
							opbx_queue_log("NONE", chan->uniqueid, agent, "AGENTCALLBACKLOGIN", "%s", p->loginchan);
							if (option_verbose > 1)
								opbx_verbose(VERBOSE_PREFIX_2 "Callback Agent '%s' logged in on %s\n", p->agent, p->loginchan);
							opbx_device_state_changed("Agent/%s", p->agent);
						} else {
							logintime = time(NULL) - p->loginstart;
							p->loginstart = 0;
							manager_event(EVENT_FLAG_AGENT, "Agentcallbacklogoff",
								      "Agent: %s\r\n"
								      "Loginchan: %s\r\n"
								      "Logintime: %ld\r\n"
								      "Uniqueid: %s\r\n",
								      p->agent, lopbx_loginchan, logintime, chan->uniqueid);
							opbx_queue_log("NONE", chan->uniqueid, agent, "AGENTCALLBACKLOGOFF", "%s|%ld|", lopbx_loginchan, logintime);
							if (option_verbose > 1)
								opbx_verbose(VERBOSE_PREFIX_2 "Callback Agent '%s' logged out\n", p->agent);
							opbx_device_state_changed("Agent/%s", p->agent);
						}
						opbx_mutex_unlock(&agentlock);
						if (!res)
							res = opbx_safe_sleep(chan, 500);
						opbx_mutex_unlock(&p->lock);
						if (persistent_agents)
							dump_agents();
					} else if (!res) {
#ifdef HONOR_MUSIC_CLASS
						/* check if the moh class was changed with setmusiconhold */
						if (*(chan->musicclass))
							opbx_copy_string(p->moh, chan->musicclass, sizeof(p->moh));
#endif								
						opbx_moh_start(chan, p->moh);
						if (p->loginstart == 0)
							time(&p->loginstart);
						manager_event(EVENT_FLAG_AGENT, "Agentlogin",
							      "Agent: %s\r\n"
							      "Channel: %s\r\n"
							      "Uniqueid: %s\r\n",
							      p->agent, chan->name, chan->uniqueid);
						if (update_cdr && chan->cdr)
							snprintf(chan->cdr->channel, sizeof(chan->cdr->channel), "Agent/%s", p->agent);
						opbx_queue_log("NONE", chan->uniqueid, agent, "AGENTLOGIN", "%s", chan->name);
						if (option_verbose > 1)
							opbx_verbose(VERBOSE_PREFIX_2 "Agent '%s' logged in (format %s/%s)\n", p->agent,
								    opbx_getformatname(chan->readformat), opbx_getformatname(chan->writeformat));
						/* Login this channel and wait for it to
						   go away */
						p->chan = chan;
						if (p->ackcall > 1)
							check_beep(p, 0);
						else
							check_availability(p, 0);
						opbx_mutex_unlock(&p->lock);
						opbx_mutex_unlock(&agentlock);
						opbx_device_state_changed("Agent/%s", p->agent);
						while (res >= 0) {
							opbx_mutex_lock(&p->lock);
							if (p->chan != chan)
								res = -1;
							opbx_mutex_unlock(&p->lock);
							/* Yield here so other interested threads can kick in. */
							sched_yield();
							if (res)
								break;

							opbx_mutex_lock(&agentlock);
							opbx_mutex_lock(&p->lock);
							if (p->lastdisc.tv_sec) {
								if (opbx_tvdiff_ms(opbx_tvnow(), p->lastdisc) > p->wrapuptime) {
									if (option_debug)
										opbx_log(LOG_DEBUG, "Wrapup time for %s expired!\n", p->agent);
									p->lastdisc = opbx_tv(0, 0);
									if (p->ackcall > 1)
										check_beep(p, 0);
									else
										check_availability(p, 0);
								}
							}
							opbx_mutex_unlock(&p->lock);
							opbx_mutex_unlock(&agentlock);
							/*	Synchronize channel ownership between call to agent and itself. */
							opbx_mutex_lock( &p->app_lock );
							opbx_mutex_lock(&p->lock);
							p->owning_app = pthread_self();
							opbx_mutex_unlock(&p->lock);
							if (p->ackcall > 1) 
								res = agent_ack_sleep(p);
							else
								res = opbx_safe_sleep_conditional( chan, 1000,
												  agent_cont_sleep, p );
							opbx_mutex_unlock( &p->app_lock );
							if ((p->ackcall > 1)  && (res == 1)) {
								opbx_mutex_lock(&agentlock);
								opbx_mutex_lock(&p->lock);
								check_availability(p, 0);
								opbx_mutex_unlock(&p->lock);
								opbx_mutex_unlock(&agentlock);
								res = 0;
							}
							sched_yield();
						}
						opbx_mutex_lock(&p->lock);
						if (res && p->owner) 
							opbx_log(LOG_WARNING, "Huh?  We broke out when there was still an owner?\n");
						/* Log us off if appropriate */
						if (p->chan == chan)
							p->chan = NULL;
						p->acknowledged = 0;
						logintime = time(NULL) - p->loginstart;
						p->loginstart = 0;
						opbx_mutex_unlock(&p->lock);
						manager_event(EVENT_FLAG_AGENT, "Agentlogoff",
							      "Agent: %s\r\n"
							      "Logintime: %ld\r\n"
							      "Uniqueid: %s\r\n",
							      p->agent, logintime, chan->uniqueid);
						opbx_queue_log("NONE", chan->uniqueid, agent, "AGENTLOGOFF", "%s|%ld", chan->name, logintime);
						if (option_verbose > 1)
							opbx_verbose(VERBOSE_PREFIX_2 "Agent '%s' logged out\n", p->agent);
						/* If there is no owner, go ahead and kill it now */
						opbx_device_state_changed("Agent/%s", p->agent);
						if (p->dead && !p->owner) {
							opbx_mutex_destroy(&p->lock);
							opbx_mutex_destroy(&p->app_lock);
							free(p);
						}
					}
					else {
						opbx_mutex_unlock(&p->lock);
						p = NULL;
					}
					res = -1;
				} else {
					opbx_mutex_unlock(&p->lock);
					errmsg = "agent-alreadyon";
					p = NULL;
				}
				break;
			}
			opbx_mutex_unlock(&p->lock);
			p = p->next;
		}
		if (!p)
			opbx_mutex_unlock(&agentlock);

		if (!res && (max_login_tries==0 || tries < max_login_tries))
			res = opbx_app_getdata(chan, errmsg, user, sizeof(user) - 1, 0);
	}
		
	if (!res)
		res = opbx_safe_sleep(chan, 500);

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
				pbx_builtin_setvar_helper(chan, "AGENTEXTEN", args.extension);
			}
			else {
				pbx_builtin_setvar_helper(chan, "AGENTSTATUS", "off");
			}
		}
		else {
			pbx_builtin_setvar_helper(chan, "AGENTSTATUS", "fail");
		}
		if (opbx_exists_extension(chan, chan->context, chan->exten, chan->priority + 1, chan->cid.cid_num)) {
			LOCAL_USER_REMOVE(u);
			return 0;
		}
		/* Do we need to play agent-goodbye now that we will be hanging up? */
		if (play_announcement) {
			if (!res)
				res = opbx_safe_sleep(chan, 1000);
			res = opbx_streamfile(chan, agent_goodbye, chan->language);
			if (!res)
				res = opbx_waitstream(chan, "");
			if (!res)
				res = opbx_safe_sleep(chan, 1000);
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
static int login_exec(struct opbx_channel *chan, void *data)
{
	return __login_exec(chan, data, 0);
}

/**
 *  Called by the AgentCallbackLogin application (from the dial plan).
 * 
 * @param chan
 * @param data
 * @returns
 * @sa login_exec(), agentmonitoroutgoing_exec(), load_module().
 */
static int callback_exec(struct opbx_channel *chan, void *data)
{
	return __login_exec(chan, data, 1);
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

	if (opbx_strlen_zero(agent)) {
		astman_send_error(s, m, "No agent specified");
		return 0;
	}

	if (opbx_strlen_zero(exten)) {
		astman_send_error(s, m, "No extension specified");
		return 0;
	}

	opbx_mutex_lock(&agentlock);
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
		opbx_mutex_lock(&p->lock);
		login_state = 1; /* Successful Login */
		
		if (opbx_strlen_zero(context))
			opbx_copy_string(p->loginchan, exten, sizeof(p->loginchan));
		else
			snprintf(p->loginchan, sizeof(p->loginchan), "%s@%s", exten, context);

		if (!opbx_strlen_zero(wrapuptime_s)) {
			p->wrapuptime = atoi(wrapuptime_s);
			if (p->wrapuptime < 0)
				p->wrapuptime = 0;
		}

		if (opbx_true(ackcall_s))
			p->ackcall = 1;
		else
			p->ackcall = 0;

		if (p->loginstart == 0)
			time(&p->loginstart);
		manager_event(EVENT_FLAG_AGENT, "Agentcallbacklogin",
			      "Agent: %s\r\n"
			      "Loginchan: %s\r\n",
			      p->agent, p->loginchan);
		opbx_queue_log("NONE", "NONE", agent, "AGENTCALLBACKLOGIN", "%s", p->loginchan);
		if (option_verbose > 1)
			opbx_verbose(VERBOSE_PREFIX_2 "Callback Agent '%s' logged in on %s\n", p->agent, p->loginchan);
		opbx_device_state_changed("Agent/%s", p->agent);
		opbx_mutex_unlock(&p->lock);
		p = p->next;
		if (persistent_agents)
			dump_agents();
	}
	opbx_mutex_unlock(&agentlock);

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
static int agentmonitoroutgoing_exec(struct opbx_channel *chan, void *data)
{
	int exitifnoagentid = 0;
	int nowarnings = 0;
	int changeoutgoing = 0;
	int res = 0;
	char agent[OPBX_MAX_AGENT], *tmp;

	if (data) {
		if (strchr(data, 'd'))
			exitifnoagentid = 1;
		if (strchr(data, 'n'))
			nowarnings = 1;
		if (strchr(data, 'c'))
			changeoutgoing = 1;
	}
	if (chan->cid.cid_num) {
		char agentvar[OPBX_MAX_BUF];
		snprintf(agentvar, sizeof(agentvar), "%s_%s", GETAGENTBYCALLERID, chan->cid.cid_num);
		if ((tmp = pbx_builtin_getvar_helper(NULL, agentvar))) {
			struct agent_pvt *p = agents;
			opbx_copy_string(agent, tmp, sizeof(agent));
			opbx_mutex_lock(&agentlock);
			while (p) {
				if (!strcasecmp(p->agent, tmp)) {
					if (changeoutgoing) snprintf(chan->cdr->channel, sizeof(chan->cdr->channel), "Agent/%s", p->agent);
					__agent_start_monitoring(chan, p, 1);
					break;
				}
				p = p->next;
			}
			opbx_mutex_unlock(&agentlock);
			
		} else {
			res = -1;
			if (!nowarnings)
				opbx_log(LOG_WARNING, "Couldn't find the global variable %s, so I can't figure out which agent (if it's an agent) is placing outgoing call.\n", agentvar);
		}
	} else {
		res = -1;
		if (!nowarnings)
			opbx_log(LOG_WARNING, "There is no callerid on that call, so I can't figure out which agent (if it's an agent) is placing outgoing call.\n");
	}
	/* check if there is n + 101 priority */
	if (res) {
		if (opbx_exists_extension(chan, chan->context, chan->exten, chan->priority + 101, chan->cid.cid_num)) {
			chan->priority+=100;
			if (option_verbose > 2)
				opbx_verbose(VERBOSE_PREFIX_3 "Going to %d priority because there is no callerid or the agentid cannot be found.\n",chan->priority);
		}
		else if (exitifnoagentid)
			return res;
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

		if (!opbx_strlen_zero(cur_agent->loginchan)) {
			snprintf(buf, sizeof(buf), "%s;%s", cur_agent->loginchan, cur_agent->logincallerid);
			if (opbx_db_put(pa_family, cur_agent->agent, buf))
				opbx_log(LOG_WARNING, "failed to create persistent entry!\n");
			else if (option_debug)
				opbx_log(LOG_DEBUG, "Saved Agent: %s on %s\n", cur_agent->agent, cur_agent->loginchan);
		} else {
			/* Delete -  no agent or there is an error */
			opbx_db_del(pa_family, cur_agent->agent);
		}
	}
}

/**
 * Reload the persistent agents from opbxdb.
 */
static void reload_agents(void)
{
	char *agent_num;
	struct opbx_db_entry *db_tree;
	struct opbx_db_entry *entry;
	struct agent_pvt *cur_agent;
	char agent_data[256];
	char *parse;
	char *agent_chan;
	char *agent_callerid;

	db_tree = opbx_db_gettree(pa_family, NULL);

	opbx_mutex_lock(&agentlock);
	for (entry = db_tree; entry; entry = entry->next) {
        	if (!strncmp(entry->key, pa_family, strlen(pa_family))){
                       agent_num = entry->key + strlen(pa_family) + 2;
               	} else {
                       agent_num = entry->key;
	        }
		cur_agent = agents;
		while (cur_agent) {
			opbx_mutex_lock(&cur_agent->lock);
			if (strcmp(agent_num, cur_agent->agent) == 0)
				break;
			opbx_mutex_unlock(&cur_agent->lock);
			cur_agent = cur_agent->next;
		}
		if (!cur_agent) {
			opbx_db_del(pa_family, agent_num);
			continue;
		} else
			opbx_mutex_unlock(&cur_agent->lock);
		if (!opbx_db_get(pa_family, agent_num, agent_data, sizeof(agent_data)-1)) {
			if (option_debug)
				opbx_log(LOG_DEBUG, "Reload Agent: %s on %s\n", cur_agent->agent, agent_data);
			parse = agent_data;
			agent_chan = strsep(&parse, ";");
			agent_callerid = strsep(&parse, ";");
			opbx_copy_string(cur_agent->loginchan, agent_chan, sizeof(cur_agent->loginchan));
			if (agent_callerid) {
				opbx_copy_string(cur_agent->logincallerid, agent_callerid, sizeof(cur_agent->logincallerid));
				set_agentbycallerid(cur_agent->logincallerid, cur_agent->agent);
			} else
				cur_agent->logincallerid[0] = '\0';
			if (cur_agent->loginstart == 0)
				time(&cur_agent->loginstart);
			opbx_device_state_changed("Agent/%s", cur_agent->agent);	
		}
	}
	opbx_mutex_unlock(&agentlock);
	if (db_tree) {
		opbx_log(LOG_NOTICE, "Agents sucessfully reloaded from database.\n");
		opbx_db_freetree(db_tree);
	}
}

/*--- agent_devicestate: Part of PBX channel interface ---*/
static int agent_devicestate(void *data)
{
	struct agent_pvt *p;
	char *s;
	opbx_group_t groupmatch;
	int groupoff;
	int waitforagent=0;
	int res = OPBX_DEVICE_INVALID;
	
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
	opbx_mutex_lock(&agentlock);
	p = agents;
	while(p) {
		opbx_mutex_lock(&p->lock);
		if (!p->pending && ((groupmatch && (p->group & groupmatch)) || !strcmp(data, p->agent))) {
			if (p->owner) {
				if (res != OPBX_DEVICE_INUSE)
					res = OPBX_DEVICE_BUSY;
			} else {
				if (res == OPBX_DEVICE_BUSY)
					res = OPBX_DEVICE_INUSE;
				if (p->chan || !opbx_strlen_zero(p->loginchan)) {
					if (res == OPBX_DEVICE_INVALID)
						res = OPBX_DEVICE_UNKNOWN;
				} else if (res == OPBX_DEVICE_INVALID)	
					res = OPBX_DEVICE_UNAVAILABLE;
			}
			if (!strcmp(data, p->agent)) {
				opbx_mutex_unlock(&p->lock);
				break;
			}
		}
		opbx_mutex_unlock(&p->lock);
		p = p->next;
	}
	opbx_mutex_unlock(&agentlock);
	return res;
}

/**
 * Initialize the Agents module.
 * This funcion is being called by OpenPBX when loading the module. Among other thing it registers applications, cli commands and reads the cofiguration file.
 *
 * @returns int Always 0.
 */
int load_module()
{
	/* Make sure we can register our agent channel type */
	if (opbx_channel_register(&agent_tech)) {
		opbx_log(LOG_ERROR, "Unable to register channel class %s\n", channeltype);
		return -1;
	}
	/* Dialplan applications */
	opbx_register_application(app, login_exec, synopsis, descrip);
	opbx_register_application(app2, callback_exec, synopsis2, descrip2);
	opbx_register_application(app3, agentmonitoroutgoing_exec, synopsis3, descrip3);
	/* Manager commands */
	opbx_manager_register2("Agents", EVENT_FLAG_AGENT, action_agents, "Lists agents and their status", mandescr_agents);
	opbx_manager_register2("AgentLogoff", EVENT_FLAG_AGENT, action_agent_logoff, "Sets an agent as no longer logged in", mandescr_agent_logoff);
	opbx_manager_register2("AgentCallbackLogin", EVENT_FLAG_AGENT, action_agent_callback_login, "Sets an agent as logged in by callback", mandescr_agent_callback_login);
	/* CLI Application */
	opbx_cli_register(&cli_show_agents);
	opbx_cli_register(&cli_agent_logoff);
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
	/* First, take us out of the channel loop */
	/* Unregister CLI application */
	opbx_cli_unregister(&cli_show_agents);
	opbx_cli_unregister(&cli_agent_logoff);
	/* Unregister dialplan applications */
	opbx_unregister_application(app);
	opbx_unregister_application(app2);
	opbx_unregister_application(app3);
	/* Unregister manager command */
	opbx_manager_unregister("Agents");
	opbx_manager_unregister("AgentLogoff");
	opbx_manager_unregister("AgentCallbackLogin");
	/* Unregister channel */
	opbx_channel_unregister(&agent_tech);
	if (!opbx_mutex_lock(&agentlock)) {
		/* Hangup all interfaces if they have an owner */
		p = agents;
		while(p) {
			if (p->owner)
				opbx_softhangup(p->owner, OPBX_SOFTHANGUP_APPUNLOAD);
			p = p->next;
		}
		agents = NULL;
		opbx_mutex_unlock(&agentlock);
	} else {
		opbx_log(LOG_WARNING, "Unable to lock the monitor\n");
		return -1;
	}		
	return 0;
}

int usecount()
{
	return usecnt;
}

char *description()
{
	return (char *) desc;
}

