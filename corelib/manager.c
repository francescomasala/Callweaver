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

/*
 *
 * The CallWeaver Management Interface
 *
 * Channel Management and more
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/corelib/manager.c $", "$Revision: 4723 $")

#include "callweaver/channel.h"
#include "callweaver/file.h"
#include "callweaver/module.h"
#include "callweaver/manager.h"
#include "callweaver/config.h"
#include "callweaver/phone_no_utils.h"
#include "callweaver/lock.h"
#include "callweaver/logger.h"
#include "callweaver/options.h"
#include "callweaver/cli.h"
#include "callweaver/app.h"
#include "callweaver/pbx.h"
#include "callweaver/acl.h"
#include "callweaver/utils.h"

struct fast_originate_helper {
	char tech[256];
	char data[256];
	int timeout;
	char app[256];
	char appdata[256];
	char cid_name[256];
	char cid_num[256];
	char context[256];
	char exten[256];
	char idtext[256];
	int priority;
	struct cw_variable *vars;
};

static int enabled = 0;
static int portno = DEFAULT_MANAGER_PORT;
static int asock = -1;
static int displayconnects = 1;

static pthread_t t;
CW_MUTEX_DEFINE_STATIC(sessionlock);
static int block_sockets = 0;

static struct permalias {
	int num;
	char *label;
} perms[] = {
	{ EVENT_FLAG_SYSTEM, "system" },
	{ EVENT_FLAG_CALL, "call" },
	{ EVENT_FLAG_LOG, "log" },
	{ EVENT_FLAG_VERBOSE, "verbose" },
	{ EVENT_FLAG_COMMAND, "command" },
	{ EVENT_FLAG_AGENT, "agent" },
	{ EVENT_FLAG_USER, "user" },
	{ -1, "all" },
	{ 0, "none" },
};

static struct mansession *sessions = NULL;
static struct manager_action *first_action = NULL;
CW_MUTEX_DEFINE_STATIC(actionlock);
CW_MUTEX_DEFINE_STATIC(hooklock);

static struct manager_custom_hook *manager_hooks = NULL;


void add_manager_hook(struct manager_custom_hook *hook)
{
	cw_mutex_lock(&hooklock);
	if (hook) {
		hook->next = manager_hooks;
		manager_hooks = hook;
	}
	cw_mutex_unlock(&hooklock);
}

void del_manager_hook(struct manager_custom_hook *hook)
{
	struct manager_custom_hook *hookp, *lasthook = NULL;

	cw_mutex_lock(&hooklock);
	for (hookp = manager_hooks; hookp ; hookp = hookp->next) {
		if (hookp == hook) {
			if (lasthook) {
				lasthook->next = hookp->next;
			} else {
				manager_hooks = hookp->next;
			}
		}
		lasthook = hookp;
	}
	cw_mutex_unlock(&hooklock);

}

/*! authority_to_str: Convert authority code to string with serveral options */
static char *authority_to_str(int authority, char *res, int reslen)
{
	int running_total = 0, i;
	memset(res, 0, reslen);
	for (i=0; i<sizeof(perms) / sizeof(perms[0]) - 1; i++) {
		if (authority & perms[i].num) {
			if (*res) {
				strncat(res, ",", (reslen > running_total) ? reslen - running_total : 0);
				running_total++;
			}
			strncat(res, perms[i].label, (reslen > running_total) ? reslen - running_total : 0);
			running_total += strlen(perms[i].label);
		}
	}
	if (cw_strlen_zero(res)) {
		cw_copy_string(res, "<none>", reslen);
	}
	return res;
}

static char *complete_show_mancmd(char *line, char *word, int pos, int state)
{
	struct manager_action *cur = first_action;
	int which = 0;

	cw_mutex_lock(&actionlock);
	while (cur) { /* Walk the list of actions */
		if (!strncasecmp(word, cur->action, strlen(word))) {
			if (++which > state) {
				char *ret = strdup(cur->action);
				cw_mutex_unlock(&actionlock);
				return ret;
			}
		}
		cur = cur->next;
	}
	cw_mutex_unlock(&actionlock);
	return NULL;
}

static int handle_showmancmd(int fd, int argc, char *argv[])
{
	struct manager_action *cur = first_action;
	char authority[80];
	int num;

	if (argc != 4)
		return RESULT_SHOWUSAGE;
	cw_mutex_lock(&actionlock);
	while (cur) { /* Walk the list of actions */
		for (num = 3; num < argc; num++) {
			if (!strcasecmp(cur->action, argv[num])) {
				cw_cli(fd, "Action: %s\nSynopsis: %s\nPrivilege: %s\n%s\n", cur->action, cur->synopsis, authority_to_str(cur->authority, authority, sizeof(authority) -1), cur->description ? cur->description : "");
			}
		}
		cur = cur->next;
	}

	cw_mutex_unlock(&actionlock);
	return RESULT_SUCCESS;
}

/*! \brief  handle_showmancmds: CLI command */
/* Should change to "manager show commands" */
static int handle_showmancmds(int fd, int argc, char *argv[])
{
	struct manager_action *cur = first_action;
	char authority[80];
	char *format = "  %-15.15s  %-15.15s  %-55.55s\n";

	cw_mutex_lock(&actionlock);
	cw_cli(fd, format, "Action", "Privilege", "Synopsis");
	cw_cli(fd, format, "------", "---------", "--------");
	while (cur) { /* Walk the list of actions */
		cw_cli(fd, format, cur->action, authority_to_str(cur->authority, authority, sizeof(authority) -1), cur->synopsis);
		cur = cur->next;
	}

	cw_mutex_unlock(&actionlock);
	return RESULT_SUCCESS;
}

/*! \brief  handle_showmanconn: CLI command show manager connected */
/* Should change to "manager show connected" */
static int handle_showmanconn(int fd, int argc, char *argv[])
{
	struct mansession *s;
	char iabuf[INET_ADDRSTRLEN];
	char *format = "  %-15.15s  %-15.15s\n";
	cw_mutex_lock(&sessionlock);
	s = sessions;
	cw_cli(fd, format, "Username", "IP Address");
	while (s) {
		cw_cli(fd, format,s->username, cw_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr));
		s = s->next;
	}

	cw_mutex_unlock(&sessionlock);
	return RESULT_SUCCESS;
}

static char showmancmd_help[] = 
"Usage: show manager command <actionname>\n"
"	Shows the detailed description for a specific CallWeaver manager interface command.\n";

static char showmancmds_help[] = 
"Usage: show manager commands\n"
"	Prints a listing of all the available CallWeaver manager interface commands.\n";

static char showmanconn_help[] = 
"Usage: show manager connected\n"
"	Prints a listing of the users that are currently connected to the\n"
"CallWeaver manager interface.\n";

static struct cw_cli_entry show_mancmd_cli =
	{ { "show", "manager", "command", NULL },
	handle_showmancmd, "Show a manager interface command", showmancmd_help, complete_show_mancmd };

static struct cw_cli_entry show_mancmds_cli =
	{ { "show", "manager", "commands", NULL },
	handle_showmancmds, "List manager interface commands", showmancmds_help };

static struct cw_cli_entry show_manconn_cli =
	{ { "show", "manager", "connected", NULL },
	handle_showmanconn, "Show connected manager interface users", showmanconn_help };

static void free_session(struct mansession *s)
{
	struct eventqent *eqe;

	if (s->fd > -1)
		close(s->fd);
	cw_mutex_destroy(&s->__lock);
	while (s->eventq)
    {
		eqe = s->eventq;
		s->eventq = s->eventq->next;
		free(eqe);
	}
	free(s);
}

static void destroy_session(struct mansession *s)
{
	struct mansession *cur;
	struct mansession *prev = NULL;
	
    cw_mutex_lock(&sessionlock);
	cur = sessions;
	while (cur)
    {
		if (cur == s)
			break;
		prev = cur;
		cur = cur->next;
	}
	if (cur)
    {
		if (prev)
			prev->next = cur->next;
		else
			sessions = cur->next;
		free_session(s);
	}
    else
	{
    	cw_log(LOG_WARNING, "Trying to delete nonexistent session %p?\n", s);
	}
    cw_mutex_unlock(&sessionlock);
}

char *astman_get_header(struct message *m, char *var)
{
	char cmp[80];
	int x;

	snprintf(cmp, sizeof(cmp), "%s: ", var);
	for (x = 0;  x < m->hdrcount;  x++)
    {
		if (!strncasecmp(cmp, m->headers[x], strlen(cmp)))
			return m->headers[x] + strlen(cmp);
	}
    return "";
}

struct cw_variable *astman_get_variables(struct message *m)
{
	int varlen, x;
	struct cw_variable *head = NULL, *cur;
	char *var, *val;
	
	varlen = strlen("Variable: ");	

	for (x = 0;  x < m->hdrcount;  x++)
    {
		if (!strncasecmp("Variable: ", m->headers[x], varlen))
        {
			var = val = cw_strdupa(m->headers[x] + varlen);
			strsep(&val, "=");
			if (!val || cw_strlen_zero(var))
				continue;
			cur = cw_variable_new(var, val);
			if (head)
            {
				cur->next = head;
				head = cur;
			}
            else
            {
				head = cur;
            }
		}
	}

	return head;
}

/*! NOTE:
   Callers of astman_send_error(), astman_send_response() or astman_send_ack() must EITHER
   hold the session lock _or_ be running in an action callback (in which case s->busy will
   be non-zero). In either of these cases, there is no need to lock-protect the session's
   fd, since no other output will be sent (events will be queued), and no input will
   be read until either the current action finishes or get_input() obtains the session
   lock.
 */
void astman_send_error(struct mansession *s, struct message *m, char *error)
{
	char *id = astman_get_header(m,"ActionID");

	cw_cli(s->fd, "Response: Error\r\n");
	if (!cw_strlen_zero(id))
		cw_cli(s->fd, "ActionID: %s\r\n",id);
	cw_cli(s->fd, "Message: %s\r\n\r\n", error);
}

void astman_send_response(struct mansession *s, struct message *m, char *resp, char *msg)
{
	char *id = astman_get_header(m,"ActionID");

	cw_cli(s->fd, "Response: %s\r\n", resp);
	if (!cw_strlen_zero(id))
		cw_cli(s->fd, "ActionID: %s\r\n",id);
	if (msg)
		cw_cli(s->fd, "Message: %s\r\n\r\n", msg);
	else
		cw_cli(s->fd, "\r\n");
}

void astman_send_ack(struct mansession *s, struct message *m, char *msg)
{
	astman_send_response(s, m, "Success", msg);
}

/*! Tells you if smallstr exists inside bigstr
   which is delim by delim and uses no buf or stringsep
   cw_instring("this|that|more","this",',') == 1;

   feel free to move this to app.c -anthm */
static int cw_instring(char *bigstr, char *smallstr, char delim) 
{
	char *val = bigstr, *next;

	do {
		if ((next = strchr(val, delim))) {
			if (!strncmp(val, smallstr, (next - val)))
				return 1;
			else
				continue;
		} else
			return !strcmp(smallstr, val);

	} while (*(val = (next + 1)));

	return 0;
}

static int get_perm(char *instr)
{
	int x = 0, ret = 0;

	if (!instr)
		return 0;

	for (x=0; x<sizeof(perms) / sizeof(perms[0]); x++)
		if (cw_instring(instr, perms[x].label, ','))
			ret |= perms[x].num;
	
	return ret;
}

static int cw_is_number(char *string) 
{
	int ret = 1, x = 0;

	if (!string)
		return 0;

	for (x=0; x < strlen(string); x++) {
		if (!(string[x] >= 48 && string[x] <= 57)) {
			ret = 0;
			break;
		}
	}
	
	return ret ? atoi(string) : 0;
}

static int cw_strings_to_mask(char *string) 
{
	int x, ret = -1;
	
	x = cw_is_number(string);
	if (x)
    {
		ret = x;
	}
    else if (cw_strlen_zero(string))
    {
		ret = -1;
	}
    else if (cw_false(string))
    {
		ret = 0;
	}
    else if (cw_true(string))
    {
		ret = 0;
		for (x = 0;  x < sizeof(perms) / sizeof(perms[0]);  x++)
			ret |= perms[x].num;		
	}
    else
    {
		ret = 0;
		for (x = 0;  x < sizeof(perms) / sizeof(perms[0]);  x++)
        {
			if (cw_instring(string, perms[x].label, ',')) 
				ret |= perms[x].num;		
		}
	}

	return ret;
}

/*! 
   Rather than braindead on,off this now can also accept a specific int mask value 
   or a ',' delim list of mask strings (the same as manager.conf) -anthm
*/

static int set_eventmask(struct mansession *s, char *eventmask)
{
	int maskint = cw_strings_to_mask(eventmask);

	cw_mutex_lock(&s->__lock);
	if (maskint >= 0)	
		s->send_events = maskint;
	cw_mutex_unlock(&s->__lock);
	
	return maskint;
}

static int authenticate(struct mansession *s, struct message *m)
{
	struct cw_config *cfg;
	char iabuf[INET_ADDRSTRLEN];
	char *cat;
	char *user = astman_get_header(m, "Username");
	char *pass = astman_get_header(m, "Secret");
	char *authtype = astman_get_header(m, "AuthType");
	char *key = astman_get_header(m, "Key");
	char *events = astman_get_header(m, "Events");
	
	cfg = cw_config_load("manager.conf");
	if (!cfg)
		return -1;
	cat = cw_category_browse(cfg, NULL);
	while (cat)
    {
		if (strcasecmp(cat, "general"))
        {
			/* This is a user */
			if (!strcasecmp(cat, user))
            {
				struct cw_variable *v;
				struct cw_ha *ha = NULL;
				char *password = NULL;

				v = cw_variable_browse(cfg, cat);
				while (v)
                {
					if (!strcasecmp(v->name, "secret"))
                    {
						password = v->value;
					}
                    else if (!strcasecmp(v->name, "permit")
                             ||
						     !strcasecmp(v->name, "deny"))
                    {
						ha = cw_append_ha(v->name, v->value, ha);
					}
                    else if (!strcasecmp(v->name, "writetimeout"))
                    {
						int val = atoi(v->value);

						if (val < 100)
							cw_log(LOG_WARNING, "Invalid writetimeout value '%s' at line %d\n", v->value, v->lineno);
						else
							s->writetimeout = val;
					}
				    		
					v = v->next;
				}
				if (ha && !cw_apply_ha(ha, &(s->sin)))
                {
					cw_log(LOG_NOTICE, "%s failed to pass IP ACL as '%s'\n", cw_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr), user);
					cw_free_ha(ha);
					cw_config_destroy(cfg);
					return -1;
				}
                else if (ha)
				{
                	cw_free_ha(ha);
				}
                if (!strcasecmp(authtype, "MD5"))
                {
                    if (!cw_strlen_zero(key)
                        && 
                        !cw_strlen_zero(s->challenge)
                        &&
                        !cw_strlen_zero(password))
                    {
						char md5key[256] = "";
						cw_md5_hash_two(md5key, s->challenge, password);
						if (!strcmp(md5key, key))
							break;
						cw_config_destroy(cfg);
						return -1;
					}
				}
                else if (password && !strcasecmp(password, pass))
                {
					break;
				}
                else
                {
					cw_log(LOG_NOTICE, "%s failed to authenticate as '%s'\n", cw_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr), user);
					cw_config_destroy(cfg);
					return -1;
				}	
			}
		}
		cat = cw_category_browse(cfg, cat);
	}
	if (cat)
    {
		cw_copy_string(s->username, cat, sizeof(s->username));
		s->readperm = get_perm(cw_variable_retrieve(cfg, cat, "read"));
		s->writeperm = get_perm(cw_variable_retrieve(cfg, cat, "write"));
		cw_config_destroy(cfg);
		if (events)
			set_eventmask(s, events);
		return 0;
	}
	cw_log(LOG_NOTICE, "%s tried to authenticate with nonexistent user '%s'\n", cw_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr), user);
	cw_config_destroy(cfg);
	return -1;
}

static char mandescr_ping[] = 
"Description: A 'Ping' action will ellicit a 'Pong' response.  Used to keep the "
"  manager connection open.\n"
"Variables: NONE\n";

static int action_ping(struct mansession *s, struct message *m)
{
	astman_send_response(s, m, "Pong", NULL);
	return 0;
}

static char mandescr_listcommands[] = 
"Description: Returns the action name and synopsis for every\n"
"  action that is available to the user\n"
"Variables: NONE\n";

static int action_listcommands(struct mansession *s, struct message *m)
{
	struct manager_action *cur = first_action;
	char idText[256] = "";
	char temp[BUFSIZ];
	char *id = astman_get_header(m,"ActionID");

	if (!cw_strlen_zero(id))
		snprintf(idText,256,"ActionID: %s\r\n",id);
	cw_cli(s->fd, "Response: Success\r\n%s", idText);
	cw_mutex_lock(&actionlock);
	while (cur) { /* Walk the list of actions */
		if ((s->writeperm & cur->authority) == cur->authority)
			cw_cli(s->fd, "%s: %s (Priv: %s)\r\n", cur->action, cur->synopsis, authority_to_str(cur->authority, temp, sizeof(temp)) );
		cur = cur->next;
	}
	cw_mutex_unlock(&actionlock);
	cw_cli(s->fd, "\r\n");

	return 0;
}

static char mandescr_events[] = 
"Description: Enable/Disable sending of events to this manager\n"
"  client.\n"
"Variables:\n"
"	EventMask: 'on' if all events should be sent,\n"
"		'off' if no events should be sent,\n"
"		'system,call,log' to select which flags events should have to be sent.\n";

static int action_events(struct mansession *s, struct message *m)
{
	char *mask = astman_get_header(m, "EventMask");
	int res;

	res = set_eventmask(s, mask);
	if (res > 0)
		astman_send_response(s, m, "Events On", NULL);
	else if (res == 0)
		astman_send_response(s, m, "Events Off", NULL);

	return 0;
}

static char mandescr_logoff[] = 
"Description: Logoff this manager session\n"
"Variables: NONE\n";

static int action_logoff(struct mansession *s, struct message *m)
{
	astman_send_response(s, m, "Goodbye", "Thanks for all the fish.");
	return -1;
}

static char mandescr_hangup[] = 
"Description: Hangup a channel\n"
"Variables: \n"
"	Channel: The channel name to be hungup\n";

static int action_hangup(struct mansession *s, struct message *m)
{
	struct cw_channel *c = NULL;
	char *name = astman_get_header(m, "Channel");

	if (cw_strlen_zero(name)) {
		astman_send_error(s, m, "No channel specified");
		return 0;
	}
	c = cw_get_channel_by_name_locked(name);
	if (!c)
    {
		astman_send_error(s, m, "No such channel");
		return 0;
	}
	cw_softhangup(c, CW_SOFTHANGUP_EXPLICIT);
	cw_mutex_unlock(&c->lock);
	astman_send_ack(s, m, "Channel Hungup");
	return 0;
}

static char mandescr_setvar[] = 
"Description: Set a local channel variable.\n"
"Variables: (Names marked with * are required)\n"
"	*Channel: Channel to set variable for\n"
"	*Variable: Variable name\n"
"	*Value: Value\n";

static int action_setvar(struct mansession *s, struct message *m)
{
        struct cw_channel *c = NULL;
        char *name = astman_get_header(m, "Channel");
        char *varname = astman_get_header(m, "Variable");
        char *varval = astman_get_header(m, "Value");
	
	if (!strlen(name)) {
		astman_send_error(s, m, "No channel specified");
		return 0;
	}
	if (!strlen(varname)) {
		astman_send_error(s, m, "No variable specified");
		return 0;
	}

	c = cw_get_channel_by_name_locked(name);
	if (!c) {
		astman_send_error(s, m, "No such channel");
		return 0;
	}
	
	pbx_builtin_setvar_helper(c,varname,varval);
	  
	cw_mutex_unlock(&c->lock);
	astman_send_ack(s, m, "Variable Set");
	return 0;
}

static char mandescr_getvar[] = 
"Description: Get the value of a local channel variable.\n"
"Variables: (Names marked with * are required)\n"
"	*Channel: Channel to read variable from\n"
"	*Variable: Variable name\n"
"	ActionID: Optional Action id for message matching.\n";

static int action_getvar(struct mansession *s, struct message *m)
{
        struct cw_channel *c = NULL;
        char *name = astman_get_header(m, "Channel");
        char *varname = astman_get_header(m, "Variable");
	char *id = astman_get_header(m,"ActionID");
	char *varval;
	char *varval2=NULL;

	if (!strlen(name)) {
		astman_send_error(s, m, "No channel specified");
		return 0;
	}
	if (!strlen(varname)) {
		astman_send_error(s, m, "No variable specified");
		return 0;
	}

	c = cw_get_channel_by_name_locked(name);
	if (!c) {
		astman_send_error(s, m, "No such channel");
		return 0;
	}
	
	varval=pbx_builtin_getvar_helper(c,varname);
	if (varval)
		varval2 = cw_strdupa(varval);
	if (!varval2)
		varval2 = "";
	cw_mutex_unlock(&c->lock);
	cw_cli(s->fd, "Response: Success\r\n"
		"Variable: %s\r\nValue: %s\r\n" ,varname,varval2);
	if (!cw_strlen_zero(id))
		cw_cli(s->fd, "ActionID: %s\r\n",id);
	cw_cli(s->fd, "\r\n");

	return 0;
}

/*! \brief  action_status: Manager "status" command to show channels */
/* Needs documentation... */
static int action_status(struct mansession *s, struct message *m)
{
	char *id = astman_get_header(m,"ActionID");
  	char *name = astman_get_header(m,"Channel");
	char idText[256] = "";
	struct cw_channel *c;
	char bridge[256];
	struct timeval now = cw_tvnow();
	long elapsed_seconds=0;
	long billable_seconds=0;
	int all = cw_strlen_zero(name); /* set if we want all channels */

	astman_send_ack(s, m, "Channel status will follow");
    if (!cw_strlen_zero(id))
        snprintf(idText, 256, "ActionID: %s\r\n", id);
	if (all)
	{
    	c = cw_channel_walk_locked(NULL);
	}
    else
    {
		c = cw_get_channel_by_name_locked(name);
		if (!c)
        {
			astman_send_error(s, m, "No such channel");
			return 0;
		}
	}
	/* if we look by name, we break after the first iteration */
	while (c)
    {
		if (c->_bridge)
			snprintf(bridge, sizeof(bridge), "Link: %s\r\n", c->_bridge->name);
		else
			bridge[0] = '\0';
		if (c->pbx)
        {
			if (c->cdr)
				elapsed_seconds = now.tv_sec - c->cdr->start.tv_sec;
				if (c->cdr->answer.tv_sec > 0)
					billable_seconds = now.tv_sec - c->cdr->answer.tv_sec;
			cw_cli(s->fd,
        			"Event: Status\r\n"
        			"Privilege: Call\r\n"
        			"Channel: %s\r\n"
        			"CallerID: %s\r\n"
        			"CallerIDName: %s\r\n"
        			"Account: %s\r\n"
        			"State: %s\r\n"
        			"Context: %s\r\n"
        			"Extension: %s\r\n"
        			"Priority: %d\r\n"
        			"Seconds: %ld\r\n"
				"BillableSeconds: %ld\r\n"
        			"%s"
        			"Uniqueid: %s\r\n"
        			"%s"
        			"\r\n",
        			c->name, 
        			c->cid.cid_num ? c->cid.cid_num : "<unknown>", 
        			c->cid.cid_name ? c->cid.cid_name : "<unknown>", 
        			c->accountcode,
        			cw_state2str(c->_state), c->context,
        			c->exten, c->priority, (long)elapsed_seconds, (long)billable_seconds,
				bridge, c->uniqueid, idText);
		}
        else
        {
    		cw_cli(s->fd,
        			"Event: Status\r\n"
        			"Privilege: Call\r\n"
        			"Channel: %s\r\n"
        			"CallerID: %s\r\n"
        			"CallerIDName: %s\r\n"
        			"Account: %s\r\n"
        			"State: %s\r\n"
        			"%s"
        			"Uniqueid: %s\r\n"
        			"%s"
        			"\r\n",
        			c->name, 
        			c->cid.cid_num ? c->cid.cid_num : "<unknown>", 
        			c->cid.cid_name ? c->cid.cid_name : "<unknown>", 
        			c->accountcode,
        			cw_state2str(c->_state), bridge, c->uniqueid, idText);
        }
		cw_mutex_unlock(&c->lock);
		if (!all)
			break;
		c = cw_channel_walk_locked(c);
	}
	cw_cli(s->fd,
         	 "Event: StatusComplete\r\n"
        	 "%s"
        	 "\r\n",idText);
	return 0;
}

static char mandescr_redirect[] = 
"Description: Redirect (transfer) a call.\n"
"Variables: (Names marked with * are required)\n"
"	*Channel: Channel to redirect\n"
"	ExtraChannel: Second call leg to transfer (optional)\n"
"	*Exten: Extension to transfer to\n"
"	*Context: Context to transfer to\n"
"	*Priority: Priority to transfer to\n"
"	ActionID: Optional Action id for message matching.\n";

/*! \brief  action_redirect: The redirect manager command */
static int action_redirect(struct mansession *s, struct message *m)
{
	char *name = astman_get_header(m, "Channel");
	char *name2 = astman_get_header(m, "ExtraChannel");
	char *exten = astman_get_header(m, "Exten");
	char *context = astman_get_header(m, "Context");
	char *priority = astman_get_header(m, "Priority");
	struct cw_channel *chan, *chan2 = NULL;
	int pi = 0;
	int res;

	if (cw_strlen_zero(name))
    {
		astman_send_error(s, m, "Channel not specified");
		return 0;
	}
	if (!cw_strlen_zero(priority) && (sscanf(priority, "%d", &pi) != 1))
    {
		astman_send_error(s, m, "Invalid priority\n");
		return 0;
	}
	chan = cw_get_channel_by_name_locked(name);
	if (!chan)
    {
		char buf[BUFSIZ];

		snprintf(buf, sizeof(buf), "Channel does not exist: %s", name);
		astman_send_error(s, m, buf);
		return 0;
	}
	if (!cw_strlen_zero(name2))
		chan2 = cw_get_channel_by_name_locked(name2);
	res = cw_async_goto(chan, context, exten, pi);
	if (!res)
    {
		if (!cw_strlen_zero(name2))
        {
			if (chan2)
				res = cw_async_goto(chan2, context, exten, pi);
			else
				res = -1;
			if (!res)
				astman_send_ack(s, m, "Dual Redirect successful");
			else
				astman_send_error(s, m, "Secondary redirect failed");
		}
        else
			astman_send_ack(s, m, "Redirect successful");
	}
    else
		astman_send_error(s, m, "Redirect failed");
	if (chan)
		cw_mutex_unlock(&chan->lock);
	if (chan2)
		cw_mutex_unlock(&chan2->lock);
	return 0;
}

static char mandescr_command[] = 
"Description: Run a CLI command.\n"
"Variables: (Names marked with * are required)\n"
"	*Command: CallWeaver CLI command to run\n"
"	ActionID: Optional Action id for message matching.\n";

/*! \brief  action_command: Manager command "command" - execute CLI command */
static int action_command(struct mansession *s, struct message *m)
{
	char *cmd = astman_get_header(m, "Command");
	char *id = astman_get_header(m, "ActionID");

	cw_cli(s->fd, "Response: Follows\r\nPrivilege: Command\r\n");
	if (!cw_strlen_zero(id))
		cw_cli(s->fd, "ActionID: %s\r\n", id);
	/* FIXME: Wedge a ActionID response in here, waiting for later changes */
	cw_cli_command(s->fd, cmd);
	cw_cli(s->fd, "--END COMMAND--\r\n\r\n");
	return 0;
}

static void *fast_originate(void *data)
{
	struct fast_originate_helper *in = data;
	int res;
	int reason = 0;
	struct cw_channel *chan = NULL;

	if (!cw_strlen_zero(in->app)) {
		res = cw_pbx_outgoing_app(in->tech, CW_FORMAT_SLINEAR, in->data, in->timeout, in->app, in->appdata, &reason, 1, 
			!cw_strlen_zero(in->cid_num) ? in->cid_num : NULL, 
			!cw_strlen_zero(in->cid_name) ? in->cid_name : NULL,
			in->vars, &chan);
	} else {
		res = cw_pbx_outgoing_exten(in->tech, CW_FORMAT_SLINEAR, in->data, in->timeout, in->context, in->exten, in->priority, &reason, 1, 
			!cw_strlen_zero(in->cid_num) ? in->cid_num : NULL, 
			!cw_strlen_zero(in->cid_name) ? in->cid_name : NULL,
			in->vars, &chan);
	}   
	if (!res)
    {
		manager_event(EVENT_FLAG_CALL,
			"OriginateSuccess",
			"%s"
			"Channel: %s/%s\r\n"
			"Context: %s\r\n"
			"Exten: %s\r\n"
			"Reason: %d\r\n"
			"Uniqueid: %s\r\n",
			in->idtext,
            in->tech,
            in->data,
            in->context,
            in->exten,
            reason,
            chan ? chan->uniqueid : "<null>");
	}
    else
	{
    	manager_event(EVENT_FLAG_CALL,
			"OriginateFailure",
			"%s"
			"Channel: %s/%s\r\n"
			"Context: %s\r\n"
			"Exten: %s\r\n"
			"Reason: %d\r\n"
			"Uniqueid: %s\r\n",
			in->idtext,
            in->tech,
            in->data,
            in->context,
            in->exten,
            reason,
            chan ? chan->uniqueid : "<null>");
    }
	/* Locked by cw_pbx_outgoing_exten or cw_pbx_outgoing_app */
	if (chan)
		cw_mutex_unlock(&chan->lock);
	free(in);
	return NULL;
}

static char mandescr_originate[] = 
"Description: Generates an outgoing call to a Extension/Context/Priority or\n"
"  Application/Data\n"
"Variables: (Names marked with * are required)\n"
"	*Channel: Channel name to call\n"
"	Exten: Extension to use (requires 'Context' and 'Priority')\n"
"	Context: Context to use (requires 'Exten' and 'Priority')\n"
"	Priority: Priority to use (requires 'Exten' and 'Context')\n"
"	Application: Application to use\n"
"	Data: Data to use (requires 'Application')\n"
"	Timeout: How long to wait for call to be answered (in ms)\n"
"	CallerID: Caller ID to be set on the outgoing channel\n"
"	Variable: Channel variable to set, multiple Variable: headers are allowed\n"
"	Account: Account code\n"
"	Async: Set to 'true' for fast origination\n";

static int action_originate(struct mansession *s, struct message *m)
{
	char *name = astman_get_header(m, "Channel");
	char *exten = astman_get_header(m, "Exten");
	char *context = astman_get_header(m, "Context");
	char *priority = astman_get_header(m, "Priority");
	char *timeout = astman_get_header(m, "Timeout");
	char *callerid = astman_get_header(m, "CallerID");
	char *account = astman_get_header(m, "Account");
	char *app = astman_get_header(m, "Application");
	char *appdata = astman_get_header(m, "Data");
	char *async = astman_get_header(m, "Async");
	char *id = astman_get_header(m, "ActionID");
	struct cw_variable *vars = astman_get_variables(m);
	char *tech, *data;
	char *l=NULL, *n=NULL;
	int pi = 0;
	int res;
	int to = 30000;
	int reason = 0;
	char tmp[256];
	char tmp2[256];
	
	pthread_t th;
	pthread_attr_t attr;
	
    if (!name)
    {
		astman_send_error(s, m, "Channel not specified");
		return 0;
	}
	if (!cw_strlen_zero(priority) && (sscanf(priority, "%d", &pi) != 1))
    {
		astman_send_error(s, m, "Invalid priority\n");
		return 0;
	}
	if (!cw_strlen_zero(timeout) && (sscanf(timeout, "%d", &to) != 1))
    {
		astman_send_error(s, m, "Invalid timeout\n");
		return 0;
	}
	cw_copy_string(tmp, name, sizeof(tmp));
	tech = tmp;
	data = strchr(tmp, '/');
	if (!data)
    {
		astman_send_error(s, m, "Invalid channel\n");
		return 0;
	}
	*data++ = '\0';
	cw_copy_string(tmp2, callerid, sizeof(tmp2));
	cw_callerid_parse(tmp2, &n, &l);
	if (n)
    {
		if (cw_strlen_zero(n))
			n = NULL;
	}
	if (l)
    {
		cw_shrink_phone_number(l);
		if (cw_strlen_zero(l))
			l = NULL;
	}
	if (account)
    {
		struct cw_variable *newvar;
		newvar = cw_variable_new("CDR(accountcode|r)", account);
		newvar->next = vars;
		vars = newvar;
	}
	if (cw_true(async))
    {
		struct fast_originate_helper *fast = malloc(sizeof(struct fast_originate_helper));

		if (!fast)
        {
			res = -1;
		}
        else
        {
			memset(fast, 0, sizeof(struct fast_originate_helper));
			if (!cw_strlen_zero(id))
				snprintf(fast->idtext, sizeof(fast->idtext), "ActionID: %s\r\n", id);
			cw_copy_string(fast->tech, tech, sizeof(fast->tech));
   			cw_copy_string(fast->data, data, sizeof(fast->data));
			cw_copy_string(fast->app, app, sizeof(fast->app));
			cw_copy_string(fast->appdata, appdata, sizeof(fast->appdata));
			if (l)
				cw_copy_string(fast->cid_num, l, sizeof(fast->cid_num));
			if (n)
				cw_copy_string(fast->cid_name, n, sizeof(fast->cid_name));
			fast->vars = vars;	
			cw_copy_string(fast->context, context, sizeof(fast->context));
			cw_copy_string(fast->exten, exten, sizeof(fast->exten));
			fast->timeout = to;
			fast->priority = pi;
			pthread_attr_init(&attr);
			pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
			if (cw_pthread_create(&th, &attr, fast_originate, fast))
            {
				free(fast);
				res = -1;
			}
            else
            {
				res = 0;
			}
		}
	}
    else if (!cw_strlen_zero(app))
    {
      	res = cw_pbx_outgoing_app(tech, CW_FORMAT_SLINEAR, data, to, app, appdata, &reason, 1, l, n, vars, NULL);
    }
    else
    {
		if (exten && context && pi)
	    {
           	res = cw_pbx_outgoing_exten(tech, CW_FORMAT_SLINEAR, data, to, context, exten, pi, &reason, 1, l, n, vars, NULL);
		}
        else
        {
			astman_send_error(s, m, "Originate with 'Exten' requires 'Context' and 'Priority'");
			return 0;
		}
	}   
	if (!res)
		astman_send_ack(s, m, "Originate successfully queued");
	else
		astman_send_error(s, m, "Originate failed");
	return 0;
}

static char mandescr_mailboxstatus[] = 
"Description: Checks a voicemail account for status.\n"
"Variables: (Names marked with * are required)\n"
"	*Mailbox: Full mailbox ID <mailbox>@<vm-context>\n"
"	ActionID: Optional ActionID for message matching.\n"
"Returns number of messages.\n"
"	Message: Mailbox Status\n"
"	Mailbox: <mailboxid>\n"
"	Waiting: <count>\n"
"\n";
static int action_mailboxstatus(struct mansession *s, struct message *m)
{
	char *mailbox = astman_get_header(m, "Mailbox");
	char *id = astman_get_header(m,"ActionID");
	char idText[256] = "";
	int ret;

	if (cw_strlen_zero(mailbox))
    {
		astman_send_error(s, m, "Mailbox not specified");
		return 0;
	}
    if (!cw_strlen_zero(id))
        snprintf(idText, 256, "ActionID: %s\r\n", id);
	ret = cw_app_has_voicemail(mailbox, NULL);
	cw_cli(s->fd, "Response: Success\r\n"
				   "%s"
				   "Message: Mailbox Status\r\n"
				   "Mailbox: %s\r\n"
		 		   "Waiting: %d\r\n\r\n", idText, mailbox, ret);
	return 0;
}

static char mandescr_mailboxcount[] = 
"Description: Checks a voicemail account for new messages.\n"
"Variables: (Names marked with * are required)\n"
"	*Mailbox: Full mailbox ID <mailbox>@<vm-context>\n"
"	ActionID: Optional ActionID for message matching.\n"
"Returns number of new and old messages.\n"
"	Message: Mailbox Message Count\n"
"	Mailbox: <mailboxid>\n"
"	NewMessages: <count>\n"
"	OldMessages: <count>\n"
"\n";
static int action_mailboxcount(struct mansession *s, struct message *m)
{
	char *mailbox = astman_get_header(m, "Mailbox");
	char *id = astman_get_header(m,"ActionID");
	char idText[256] = "";
	int newmsgs = 0, oldmsgs = 0;
	
    if (cw_strlen_zero(mailbox))
    {
		astman_send_error(s, m, "Mailbox not specified");
		return 0;
	}
	cw_app_messagecount(mailbox, &newmsgs, &oldmsgs);
	if (!cw_strlen_zero(id))
		snprintf(idText, 256, "ActionID: %s\r\n", id);
	cw_cli(s->fd, "Response: Success\r\n"
				   "%s"
				   "Message: Mailbox Message Count\r\n"
				   "Mailbox: %s\r\n"
		 		   "NewMessages: %d\r\n"
				   "OldMessages: %d\r\n" 
				   "\r\n",
				    idText,mailbox, newmsgs, oldmsgs);
	return 0;
}

static char mandescr_extensionstate[] = 
"Description: Report the extension state for given extension.\n"
"  If the extension has a hint, will use devicestate to check\n"
"  the status of the device connected to the extension.\n"
"Variables: (Names marked with * are required)\n"
"	*Exten: Extension to check state on\n"
"	*Context: Context for extension\n"
"	ActionId: Optional ID for this transaction\n"
"Will return an \"Extension Status\" message.\n"
"The response will include the hint for the extension and the status.\n";

static int action_extensionstate(struct mansession *s, struct message *m)
{
	char *exten = astman_get_header(m, "Exten");
	char *context = astman_get_header(m, "Context");
	char *id = astman_get_header(m,"ActionID");
	char idText[256] = "";
	char hint[256] = "";
	int status;
	
    if (cw_strlen_zero(exten))
    {
		astman_send_error(s, m, "Extension not specified");
		return 0;
	}
	if (cw_strlen_zero(context))
		context = "default";
	status = cw_extension_state(NULL, context, exten);
	cw_get_hint(hint, sizeof(hint) - 1, NULL, 0, NULL, context, exten);
    if (!cw_strlen_zero(id))
        snprintf(idText, 256, "ActionID: %s\r\n", id);
	cw_cli(s->fd, "Response: Success\r\n"
			           "%s"
				   "Message: Extension Status\r\n"
				   "Exten: %s\r\n"
				   "Context: %s\r\n"
				   "Hint: %s\r\n"
		 		   "Status: %d\r\n\r\n",
				   idText,exten, context, hint, status);
	return 0;
}

static char mandescr_timeout[] = 
"Description: Hangup a channel after a certain time.\n"
"Variables: (Names marked with * are required)\n"
"	*Channel: Channel name to hangup\n"
"	*Timeout: Maximum duration of the call (sec)\n"
"Acknowledges set time with 'Timeout Set' message\n";

static int action_timeout(struct mansession *s, struct message *m)
{
	struct cw_channel *c = NULL;
	char *name = astman_get_header(m, "Channel");
	int timeout = atoi(astman_get_header(m, "Timeout"));
	if (cw_strlen_zero(name))
    {
		astman_send_error(s, m, "No channel specified");
		return 0;
	}
	if (!timeout)
    {
		astman_send_error(s, m, "No timeout specified");
		return 0;
	}
	c = cw_get_channel_by_name_locked(name);
	if (!c)
    {
		astman_send_error(s, m, "No such channel");
		return 0;
	}
	cw_channel_setwhentohangup(c, timeout);
	cw_mutex_unlock(&c->lock);
	astman_send_ack(s, m, "Timeout Set");
	return 0;
}

static int process_message(struct mansession *s, struct message *m)
{
	char action[80] = "";
	struct manager_action *tmp = first_action;
	char *id = astman_get_header(m,"ActionID");
	char idText[256] = "";
	char iabuf[INET_ADDRSTRLEN];

	cw_copy_string(action, astman_get_header(m, "Action"), sizeof(action));
	cw_log(LOG_DEBUG, "Manager received command '%s'\n", action);

	if (cw_strlen_zero(action))
    {
		astman_send_error(s, m, "Missing action in request");
		return 0;
	}
    if (!cw_strlen_zero(id))
        snprintf(idText, 256, "ActionID: %s\r\n", id);
	if (!s->authenticated)
    {
		if (!strcasecmp(action, "Challenge"))
        {
			char *authtype;

			authtype = astman_get_header(m, "AuthType");
			if (!strcasecmp(authtype, "MD5"))
            {
				if (cw_strlen_zero(s->challenge))
					snprintf(s->challenge, sizeof(s->challenge), "%d", rand());
				cw_mutex_lock(&s->__lock);
				cw_cli(s->fd, "Response: Success\r\n"
						"%s"
						"Challenge: %s\r\n\r\n",
						idText,s->challenge);
				cw_mutex_unlock(&s->__lock);
			}
            else
            {
				astman_send_error(s, m, "Must specify AuthType");
			}
		    return 0;
		}
        else if (!strcasecmp(action, "Login"))
        {
			if (authenticate(s, m))
            {
				sleep(1);
				astman_send_error(s, m, "Authentication failed");
				return -1;
			}
            else
            {
				s->authenticated = 1;
				if (option_verbose > 3)
                {
					if (displayconnects)
						cw_verbose(VERBOSE_PREFIX_2 "Manager '%s' logged on from %s\n", s->username, cw_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr));
				}
				cw_log(LOG_EVENT, "Manager '%s' logged on from %s\n", s->username, cw_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr));
				astman_send_ack(s, m, "Authentication accepted");
			}
		}
        else if (!strcasecmp(action, "Logoff"))
        {
			astman_send_ack(s, m, "See ya");
			return -1;
		}
        else
			astman_send_error(s, m, "Authentication Required");
	}
    else
    {
		int ret = 0;
		struct eventqent *eqe;

		cw_mutex_lock(&s->__lock);
		s->busy = 1;
		cw_mutex_unlock(&s->__lock);
		while (tmp)
        {
			if (!strcasecmp(action, tmp->action))
            {
				if ((s->writeperm & tmp->authority) == tmp->authority)
                {
					if (tmp->func(s, m))
						ret = -1;
				}
                else
                {
					astman_send_error(s, m, "Permission denied");
				}
				break;
			}
			tmp = tmp->next;
		}
		if (!tmp)
			astman_send_error(s, m, "Invalid/unknown command");
		cw_mutex_lock(&s->__lock);
		s->busy = 0;
		while (s->eventq)
        {
			if (cw_carefulwrite(s->fd, s->eventq->eventdata, strlen(s->eventq->eventdata), s->writetimeout) < 0)
            {
				ret = -1;
				break;
			}
			eqe = s->eventq;
			s->eventq = s->eventq->next;
			free(eqe);
		}
		cw_mutex_unlock(&s->__lock);
		return ret;
	}
	return 0;
}

static int get_input(struct mansession *s, char *output)
{
	/* output must have at least sizeof(s->inbuf) space */
	int res;
	int x;
	struct pollfd fds[1];
	char iabuf[INET_ADDRSTRLEN];

	for (x = 1;  x < s->inlen;  x++)
    {
		if ((s->inbuf[x] == '\n') && (s->inbuf[x-1] == '\r'))
        {
			/* Copy output data up to and including \r\n */
			memcpy(output, s->inbuf, x + 1);
			/* Add trailing \0 */
			output[x+1] = '\0';
			/* Move remaining data back to the front */
			memmove(s->inbuf, s->inbuf + x + 1, s->inlen - x);
			s->inlen -= (x + 1);
			return 1;
		}
	} 
	if (s->inlen >= sizeof(s->inbuf) - 1)
    {
		cw_log(LOG_WARNING, "Dumping long line with no return from %s: %s\n", cw_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr), s->inbuf);
		s->inlen = 0;
	}
	fds[0].fd = s->fd;
	fds[0].events = POLLIN;
	do
    {
		res = poll(fds, 1, -1);
		if (res < 0)
        {
			if (errno == EINTR)
            {
				if (s->dead)
					return -1;
				continue;
			}
			cw_log(LOG_WARNING, "Select returned error: %s\n", strerror(errno));
	 		return -1;
		}
        else if (res > 0)
        {
			cw_mutex_lock(&s->__lock);
			res = read(s->fd, s->inbuf + s->inlen, sizeof(s->inbuf) - 1 - s->inlen);
			cw_mutex_unlock(&s->__lock);
			if (res < 1)
				return -1;
			break;
		}
	} while (1);
	s->inlen += res;
	s->inbuf[s->inlen] = '\0';
	return 0;
}

static void *session_do(void *data)
{
	struct mansession *s = data;
	struct message m;
	char iabuf[INET_ADDRSTRLEN];
	int res;
	
	cw_mutex_lock(&s->__lock);
	cw_cli(s->fd, "CallWeaver Call Manager/1.0\r\n");
	cw_mutex_unlock(&s->__lock);
	memset(&m, 0, sizeof(m));
	for (;;)
    {
		res = get_input(s, m.headers[m.hdrcount]);
		if (res > 0)
        {
			/* Strip trailing \r\n */
			if (strlen(m.headers[m.hdrcount]) < 2)
				continue;
			m.headers[m.hdrcount][strlen(m.headers[m.hdrcount]) - 2] = '\0';
			if (cw_strlen_zero(m.headers[m.hdrcount]))
            {
				if (process_message(s, &m))
					break;
				memset(&m, 0, sizeof(m));
			}
            else if (m.hdrcount < MAX_HEADERS - 1)
            {
				m.hdrcount++;
            }
		}
        else if (res < 0)
        {
			break;
        }
	}
	if (s->authenticated)
    {
		if (option_verbose > 3)
        {
			if (displayconnects) 
				cw_verbose(VERBOSE_PREFIX_2 "Manager '%s' logged off from %s\n", s->username, cw_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr));    
		}
		cw_log(LOG_EVENT, "Manager '%s' logged off from %s\n", s->username, cw_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr));
	}
    else
    {
		if (option_verbose > 2)
        {
			if (displayconnects)
				cw_verbose(VERBOSE_PREFIX_2 "Connect attempt from '%s' unable to authenticate\n", cw_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr));
		}
		cw_log(LOG_EVENT, "Failed attempt from %s\n", cw_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr));
	}
	destroy_session(s);
	return NULL;
}

static void *accept_thread(void *ignore)
{
	int as;
	struct sockaddr_in sin;
	socklen_t sinlen;
	struct mansession *s;
	struct protoent *p;
	int arg = 1;
	int flags;
	pthread_attr_t attr;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	for (;;)
    {
		sinlen = sizeof(sin);
		as = accept(asock, (struct sockaddr *)&sin, &sinlen);
		if (as < 0)
        {
			cw_log(LOG_NOTICE, "Accept returned -1: %s\n", strerror(errno));
			continue;
		}
		p = getprotobyname("tcp");
		if (p)
        {
			if (setsockopt(as, p->p_proto, TCP_NODELAY, (char *)&arg, sizeof(arg)) < 0)
				cw_log(LOG_WARNING, "Failed to set manager tcp connection to TCP_NODELAY mode: %s\n", strerror(errno));
		}
		if ((s = malloc(sizeof(struct mansession))) == NULL)
		{
			cw_log(LOG_WARNING, "Failed to allocate management session: %s\n", strerror(errno));
			continue;
		} 
		memset(s, 0, sizeof(struct mansession));
		memcpy(&s->sin, &sin, sizeof(sin));
		s->writetimeout = 100;

		if (!block_sockets)
        {
			/* For safety, make sure socket is non-blocking */
			flags = fcntl(as, F_GETFL);
			fcntl(as, F_SETFL, flags | O_NONBLOCK);
		}
		cw_mutex_init(&s->__lock);
		s->fd = as;
		s->send_events = -1;
		cw_mutex_lock(&sessionlock);
		s->next = sessions;
		sessions = s;
		cw_mutex_unlock(&sessionlock);
		if (cw_pthread_create(&s->t, &attr, session_do, s))
			destroy_session(s);
	}
	pthread_attr_destroy(&attr);
	return NULL;
}

static int append_event(struct mansession *s, const char *str)
{
	struct eventqent *tmp;
    struct eventqent *prev = NULL;

	if ((tmp = malloc(sizeof(struct eventqent) + strlen(str))) == NULL)
        return -1;
    tmp->next = NULL;
	strcpy(tmp->eventdata, str);
	if (s->eventq)
    {
		for (prev = s->eventq;  prev->next;  prev = prev->next) 
			;
		prev->next = tmp;
	}
    else
    {
	    s->eventq = tmp;
	}
	return 0;
}

/*! \brief  manager_event: Send AMI event to client */
int manager_event(int category, char *event, char *fmt, ...)
{
	struct mansession *s;
	char auth[80];
	char tmp[4096] = "";
	char *tmp_next = tmp;
	size_t tmp_left = sizeof(tmp) - 2;
	va_list ap;

	cw_mutex_lock(&sessionlock);
	for (s = sessions;  s;  s = s->next)
    {
		if ((s->readperm & category) != category)
			continue;

		if ((s->send_events & category) != category)
			continue;

		if (cw_strlen_zero(tmp))
        {
			cw_build_string(&tmp_next, &tmp_left, "Event: %s\r\nPrivilege: %s\r\n",
					 event, authority_to_str(category, auth, sizeof(auth)-1));
			va_start(ap, fmt);
			cw_build_string_va(&tmp_next, &tmp_left, fmt, ap);
			va_end(ap);
			*tmp_next++ = '\r';
			*tmp_next++ = '\n';
			*tmp_next = '\0';
		}

		cw_mutex_lock(&s->__lock);
		if (s->busy)
        {
			append_event(s, tmp);
		}
        else if (cw_carefulwrite(s->fd, tmp, tmp_next - tmp, s->writetimeout) < 0)
        {
			cw_log(LOG_WARNING, "Disconnecting slow (or gone) manager session!\n");
			s->dead = 1;
			pthread_kill(s->t, SIGURG);
		}
		cw_mutex_unlock(&s->__lock);
	}
	cw_mutex_unlock(&sessionlock);

	if (manager_hooks)
    {
		struct manager_custom_hook *hookp;
		char *p;
		int len;

		cw_mutex_lock(&hooklock);
		snprintf(tmp, sizeof(tmp)-1, "Event: %s\r\nPrivilege: %s\r\n", event, authority_to_str(category, auth, sizeof(auth)-1));
		len = strlen(tmp);
		p = tmp + len;
		va_start(ap, fmt);
		vsnprintf(p, sizeof(tmp) - len - 1, fmt, ap);
		va_end(ap);
		for (hookp = manager_hooks ;  hookp;  hookp = hookp->next)
			hookp->helper(category, event, tmp);
		cw_mutex_unlock(&hooklock);
	}

	return 0;
}

int cw_manager_unregister(char *action) 
{
	struct manager_action *cur;
	struct manager_action *prev;

	cw_mutex_lock(&actionlock);
	for (cur = prev = first_action;  cur;  prev = cur, cur = cur->next)
    {
		if (!strcasecmp(action, cur->action))
        {
			prev->next = cur->next;
			free(cur);
			if (option_verbose > 2) 
				cw_verbose(VERBOSE_PREFIX_2 "Manager unregistered action %s\n", action);
			cw_mutex_unlock(&actionlock);
			return 0;
		}
	}
	cw_mutex_unlock(&actionlock);
	return 0;
}

static int manager_state_cb(char *context, char *exten, int state, void *data)
{
	/* Notify managers of change */
	manager_event(EVENT_FLAG_CALL, "ExtensionStatus", "Exten: %s\r\nContext: %s\r\nStatus: %d\r\n", exten, context, state);
	return 0;
}

static int cw_manager_register_struct(struct manager_action *act)
{
	struct manager_action *cur = first_action, *prev = NULL;
	int ret;

	cw_mutex_lock(&actionlock);
	while (cur)
    {
        /* Walk the list of actions */
		ret = strcasecmp(cur->action, act->action);
		if (ret == 0)
        {
			cw_log(LOG_WARNING, "Manager: Action '%s' already registered\n", act->action);
			cw_mutex_unlock(&actionlock);
			return -1;
		}
        else if (ret > 0)
        {
			/* Insert these alphabetically */
			if (prev)
            {
				act->next = prev->next;
				prev->next = act;
			}
            else
            {
				act->next = first_action;
				first_action = act;
			}
			break;
		}
		prev = cur; 
		cur = cur->next;
	}
	
	if (!cur)
    {
		if (prev)
			prev->next = act;
		else
			first_action = act;
		act->next = NULL;
	}

	if (option_verbose > 2) 
		cw_verbose(VERBOSE_PREFIX_2 "Manager registered action %s\n", act->action);
	cw_mutex_unlock(&actionlock);
	return 0;
}

int cw_manager_register2(const char *action, int auth, int (*func)(struct mansession *s, struct message *m), const char *synopsis, const char *description)
{
	struct manager_action *cur;

	if ((cur = malloc(sizeof(struct manager_action))) == NULL)
    {
		cw_log(LOG_WARNING, "Manager: out of memory trying to register action\n");
		cw_mutex_unlock(&actionlock);
		return -1;
	}
	cur->action = action;
	cur->authority = auth;
	cur->func = func;
	cur->synopsis = synopsis;
	cur->description = description;
	cur->next = NULL;

	cw_manager_register_struct(cur);

	return 0;
}

static int registered = 0;

int init_manager(void)
{
	struct cw_config *cfg;
	char *val;
	int oldportno = portno;
	static struct sockaddr_in ba;
	int x = 1;
	
    if (!registered)
    {
		/* Register default actions */
		cw_manager_register2("Ping", 0, action_ping, "Keepalive command", mandescr_ping);
		cw_manager_register2("Events", 0, action_events, "Control Event Flow", mandescr_events);
		cw_manager_register2("Logoff", 0, action_logoff, "Logoff Manager", mandescr_logoff);
		cw_manager_register2("Hangup", EVENT_FLAG_CALL, action_hangup, "Hangup Channel", mandescr_hangup);
		cw_manager_register("Status", EVENT_FLAG_CALL, action_status, "Lists channel status" );
		cw_manager_register2("Setvar", EVENT_FLAG_CALL, action_setvar, "Set Channel Variable", mandescr_setvar );
		cw_manager_register2("Getvar", EVENT_FLAG_CALL, action_getvar, "Gets a Channel Variable", mandescr_getvar );
		cw_manager_register2("Redirect", EVENT_FLAG_CALL, action_redirect, "Redirect (transfer) a call", mandescr_redirect );
		cw_manager_register2("Originate", EVENT_FLAG_CALL, action_originate, "Originate Call", mandescr_originate);
		cw_manager_register2("Command", EVENT_FLAG_COMMAND, action_command, "Execute CallWeaver CLI Command", mandescr_command );
		cw_manager_register2("ExtensionState", EVENT_FLAG_CALL, action_extensionstate, "Check Extension Status", mandescr_extensionstate );
		cw_manager_register2("AbsoluteTimeout", EVENT_FLAG_CALL, action_timeout, "Set Absolute Timeout", mandescr_timeout );
		cw_manager_register2("MailboxStatus", EVENT_FLAG_CALL, action_mailboxstatus, "Check Mailbox", mandescr_mailboxstatus );
		cw_manager_register2("MailboxCount", EVENT_FLAG_CALL, action_mailboxcount, "Check Mailbox Message Count", mandescr_mailboxcount );
		cw_manager_register2("ListCommands", 0, action_listcommands, "List available manager commands", mandescr_listcommands);

		cw_cli_register(&show_mancmd_cli);
		cw_cli_register(&show_mancmds_cli);
		cw_cli_register(&show_manconn_cli);
		cw_extension_state_add(NULL, NULL, manager_state_cb, NULL);
		registered = 1;
	}
	portno = DEFAULT_MANAGER_PORT;
	displayconnects = 1;
	cfg = cw_config_load("manager.conf");
	if (!cfg)
    {
		cw_log(LOG_NOTICE, "Unable to open management configuration manager.conf.  Call management disabled.\n");
		return 0;
	}
	memset(&ba, 0, sizeof(ba));
	val = cw_variable_retrieve(cfg, "general", "enabled");
	if (val)
		enabled = cw_true(val);

	val = cw_variable_retrieve(cfg, "general", "block-sockets");
	if (val)
		block_sockets = cw_true(val);

	if ((val = cw_variable_retrieve(cfg, "general", "port")))
    {
		if (sscanf(val, "%d", &portno) != 1)
        {
			cw_log(LOG_WARNING, "Invalid port number '%s'\n", val);
			portno = DEFAULT_MANAGER_PORT;
		}
	}
    else if ((val = cw_variable_retrieve(cfg, "general", "portno")))
    {
		if (sscanf(val, "%d", &portno) != 1)
        {
			cw_log(LOG_WARNING, "Invalid port number '%s'\n", val);
			portno = DEFAULT_MANAGER_PORT;
		}
		cw_log(LOG_NOTICE, "Use of portno in manager.conf deprecated.  Please use 'port=%s' instead.\n", val);
	}
	/* Parsing the displayconnects */
	if ((val = cw_variable_retrieve(cfg, "general", "displayconnects")))
		displayconnects = cw_true(val);
				
	
	ba.sin_family = AF_INET;
	ba.sin_port = htons(portno);
	memset(&ba.sin_addr, 0, sizeof(ba.sin_addr));
	
	if ((val = cw_variable_retrieve(cfg, "general", "bindaddr")))
    {
		if (!inet_aton(val, &ba.sin_addr)) { 
			cw_log(LOG_WARNING, "Invalid address '%s' specified, using 0.0.0.0\n", val);
			memset(&ba.sin_addr, 0, sizeof(ba.sin_addr));
		}
	}
	
	if ((asock > -1)  &&  ((portno != oldportno) || !enabled))
    {
#if 0
		/* Can't be done yet */
		close(asock);
		asock = -1;
#else
		cw_log(LOG_WARNING, "Unable to change management port / enabled\n");
#endif
	}
	cw_config_destroy(cfg);
	
	/* If not enabled, do nothing */
	if (!enabled)
		return 0;
	if (asock < 0)
    {
		if ((asock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        {
			cw_log(LOG_WARNING, "Unable to create socket: %s\n", strerror(errno));
			return -1;
		}
		setsockopt(asock, SOL_SOCKET, SO_REUSEADDR, &x, sizeof(x));
		if (bind(asock, (struct sockaddr *) &ba, sizeof(ba)))
        {
			cw_log(LOG_WARNING, "Unable to bind socket: %s\n", strerror(errno));
			close(asock);
			asock = -1;
			return -1;
		}
		if (listen(asock, 2))
        {
			cw_log(LOG_WARNING, "Unable to listen on socket: %s\n", strerror(errno));
			close(asock);
			asock = -1;
			return -1;
		}
		if (option_verbose)
			cw_verbose("CallWeaver Management interface listening on port %d\n", portno);
		cw_pthread_create(&t, NULL, accept_thread, NULL);
	}
	return 0;
}
