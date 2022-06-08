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

/*
 *
 * Local Proxy Channel
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>
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

static const char desc[] = "Local Proxy Channel";
static const char type[] = "Local";
static const char tdesc[] = "Local Proxy Channel Driver";

static int usecnt =0;
OPBX_MUTEX_DEFINE_STATIC(usecnt_lock);

#define IS_OUTBOUND(a,b) (a == b->chan ? 1 : 0)

/* Protect the interface list (of sip_pvt's) */
OPBX_MUTEX_DEFINE_STATIC(locallock);

static struct opbx_channel *local_request(const char *type, int format, void *data, int *cause);
static int local_digit(struct opbx_channel *ast, char digit);
static int local_call(struct opbx_channel *ast, char *dest, int timeout);
static int local_hangup(struct opbx_channel *ast);
static int local_answer(struct opbx_channel *ast);
static struct opbx_frame *local_read(struct opbx_channel *ast);
static int local_write(struct opbx_channel *ast, struct opbx_frame *f);
static int local_indicate(struct opbx_channel *ast, int condition);
static int local_fixup(struct opbx_channel *oldchan, struct opbx_channel *newchan);
static int local_sendhtml(struct opbx_channel *ast, int subclass, const char *data, int datalen);

/* PBX interface structure for channel registration */
static const struct opbx_channel_tech local_tech = {
	.type = type,
	.description = tdesc,
	.capabilities = -1,
	.requester = local_request,
	.send_digit = local_digit,
	.call = local_call,
	.hangup = local_hangup,
	.answer = local_answer,
	.read = local_read,
	.write = local_write,
	.exception = local_read,
	.indicate = local_indicate,
	.fixup = local_fixup,
	.send_html = local_sendhtml,
};

static struct local_pvt {
	opbx_mutex_t lock;			/* Channel private lock */
	char context[OPBX_MAX_CONTEXT];		/* Context to call */
	char exten[OPBX_MAX_EXTENSION];		/* Extension to call */
	int reqformat;				/* Requested format */
	int glaredetect;			/* Detect glare on hangup */
	int cancelqueue;			/* Cancel queue */
	int alreadymasqed;			/* Already masqueraded */
	int launchedpbx;			/* Did we launch the PBX */
	int nooptimization;			/* Don't leave masq state */
	struct opbx_channel *owner;		/* Master Channel */
	struct opbx_channel *chan;		/* Outbound channel */
	struct local_pvt *next;			/* Next entity */
} *locals = NULL;

static int local_queue_frame(struct local_pvt *p, int isoutbound, struct opbx_frame *f, struct opbx_channel *us)
{
	struct opbx_channel *other;
retrylock:		
	/* Recalculate outbound channel */
	if (isoutbound) {
		other = p->owner;
	} else {
		other = p->chan;
	}
	/* Set glare detection */
	p->glaredetect = 1;
	if (p->cancelqueue) {
		/* We had a glare on the hangup.  Forget all this business,
		return and destroy p.  */
		opbx_mutex_unlock(&p->lock);
		opbx_mutex_destroy(&p->lock);
		free(p);
		return -1;
	}
	if (!other) {
		p->glaredetect = 0;
		return 0;
	}
	if (opbx_mutex_trylock(&other->lock)) {
		/* Failed to lock.  Release main lock and try again */
		opbx_mutex_unlock(&p->lock);
		if (us) {
			if (opbx_mutex_unlock(&us->lock)) {
				opbx_log(LOG_WARNING, "%s wasn't locked while sending %d/%d\n",
					us->name, f->frametype, f->subclass);
				us = NULL;
			}
		}
		/* Wait just a bit */
		usleep(1);
		/* Only we can destroy ourselves, so we can't disappear here */
		if (us)
			opbx_mutex_lock(&us->lock);
		opbx_mutex_lock(&p->lock);
		goto retrylock;
	}
	opbx_queue_frame(other, f);
	opbx_mutex_unlock(&other->lock);
	p->glaredetect = 0;
	return 0;
}

static int local_answer(struct opbx_channel *ast)
{
	struct local_pvt *p = ast->tech_pvt;
	int isoutbound;
	int res = -1;

	opbx_mutex_lock(&p->lock);
	isoutbound = IS_OUTBOUND(ast, p);
	if (isoutbound) {
		/* Pass along answer since somebody answered us */
		struct opbx_frame answer = { OPBX_FRAME_CONTROL, OPBX_CONTROL_ANSWER };
		res = local_queue_frame(p, isoutbound, &answer, ast);
	} else
		opbx_log(LOG_WARNING, "Huh?  Local is being asked to answer?\n");
	opbx_mutex_unlock(&p->lock);
	return res;
}

static void check_bridge(struct local_pvt *p, int isoutbound)
{
	if (p->alreadymasqed || p->nooptimization)
		return;
	if (!p->chan || !p->owner)
		return;

	/* only do the masquerade if we are being called on the outbound channel,
	   if it has been bridged to another channel and if there are no pending
	   frames on the owner channel (because they would be transferred to the
	   outbound channel during the masquerade)
	*/
	if (isoutbound && p->chan->_bridge /* Not opbx_bridged_channel!  Only go one step! */ && !p->owner->readq) {
		/* Masquerade bridged channel into owner */
		/* Lock everything we need, one by one, and give up if
		   we can't get everything.  Remember, we'll get another
		   chance in just a little bit */

		if (!opbx_mutex_trylock(&(p->chan->_bridge)->lock)) {
			if (!p->chan->_bridge->_softhangup) {
				if (!opbx_mutex_trylock(&p->owner->lock)) {
					if (!p->owner->_softhangup) {
						opbx_channel_masquerade(p->owner, p->chan->_bridge);
						p->alreadymasqed = 1;
					}
					opbx_mutex_unlock(&p->owner->lock);
				}
				opbx_mutex_unlock(&(p->chan->_bridge)->lock);
			}
		}
	/* We only allow masquerading in one 'direction'... it's important to preserve the state
	   (group variables, etc.) that live on p->chan->_bridge (and were put there by the dialplan)
	   when the local channels go away.
	*/
#if 0
	} else if (!isoutbound && p->owner && p->owner->_bridge && p->chan && !p->chan->readq) {
		/* Masquerade bridged channel into chan */
		if (!opbx_mutex_trylock(&(p->owner->_bridge)->lock)) {
			if (!p->owner->_bridge->_softhangup) {
				if (!opbx_mutex_trylock(&p->chan->lock)) {
					if (!p->chan->_softhangup) {
						opbx_channel_masquerade(p->chan, p->owner->_bridge);
						p->alreadymasqed = 1;
					}
					opbx_mutex_unlock(&p->chan->lock);
				}
			}
			opbx_mutex_unlock(&(p->owner->_bridge)->lock);
		}
#endif
	}
}

static struct opbx_frame  *local_read(struct opbx_channel *ast)
{
	static struct opbx_frame null = { OPBX_FRAME_NULL, };

	return &null;
}

static int local_write(struct opbx_channel *ast, struct opbx_frame *f)
{
	struct local_pvt *p = ast->tech_pvt;
	int res = -1;
	int isoutbound;

	/* Just queue for delivery to the other side */
	opbx_mutex_lock(&p->lock);
	isoutbound = IS_OUTBOUND(ast, p);
	if (f && (f->frametype == OPBX_FRAME_VOICE)) 
		check_bridge(p, isoutbound);
	if (!p->alreadymasqed)
		res = local_queue_frame(p, isoutbound, f, ast);
	else {
		opbx_log(LOG_DEBUG, "Not posting to queue since already masked on '%s'\n", ast->name);
		res = 0;
	}
	opbx_mutex_unlock(&p->lock);
	return res;
}

static int local_fixup(struct opbx_channel *oldchan, struct opbx_channel *newchan)
{
	struct local_pvt *p = newchan->tech_pvt;
	opbx_mutex_lock(&p->lock);

	if ((p->owner != oldchan) && (p->chan != oldchan)) {
		opbx_log(LOG_WARNING, "Old channel wasn't %p but was %p/%p\n", oldchan, p->owner, p->chan);
		opbx_mutex_unlock(&p->lock);
		return -1;
	}
	if (p->owner == oldchan)
		p->owner = newchan;
	else
		p->chan = newchan;
	opbx_mutex_unlock(&p->lock);
	return 0;
}

static int local_indicate(struct opbx_channel *ast, int condition)
{
	struct local_pvt *p = ast->tech_pvt;
	int res = -1;
	struct opbx_frame f = { OPBX_FRAME_CONTROL, };
	int isoutbound;

	/* Queue up a frame representing the indication as a control frame */
	opbx_mutex_lock(&p->lock);
	isoutbound = IS_OUTBOUND(ast, p);
	f.subclass = condition;
	res = local_queue_frame(p, isoutbound, &f, ast);
	opbx_mutex_unlock(&p->lock);
	return res;
}

static int local_digit(struct opbx_channel *ast, char digit)
{
	struct local_pvt *p = ast->tech_pvt;
	int res = -1;
	struct opbx_frame f = { OPBX_FRAME_DTMF, };
	int isoutbound;

	opbx_mutex_lock(&p->lock);
	isoutbound = IS_OUTBOUND(ast, p);
	f.subclass = digit;
	res = local_queue_frame(p, isoutbound, &f, ast);
	opbx_mutex_unlock(&p->lock);
	return res;
}

static int local_sendhtml(struct opbx_channel *ast, int subclass, const char *data, int datalen)
{
	struct local_pvt *p = ast->tech_pvt;
	int res = -1;
	struct opbx_frame f = { OPBX_FRAME_HTML, };
	int isoutbound;

	opbx_mutex_lock(&p->lock);
	isoutbound = IS_OUTBOUND(ast, p);
	f.subclass = subclass;
	f.data = (char *)data;
	f.datalen = datalen;
	res = local_queue_frame(p, isoutbound, &f, ast);
	opbx_mutex_unlock(&p->lock);
	return res;
}

/*--- local_call: Initiate new call, part of PBX interface */
/* 	dest is the dial string */
static int local_call(struct opbx_channel *ast, char *dest, int timeout)
{
	struct local_pvt *p = ast->tech_pvt;
	int res;
	
	opbx_mutex_lock(&p->lock);
	if (p->owner->cid.cid_num)
		p->chan->cid.cid_num = strdup(p->owner->cid.cid_num);
	else 
		p->chan->cid.cid_num = NULL;

	if (p->owner->cid.cid_name)
		p->chan->cid.cid_name = strdup(p->owner->cid.cid_name);
	else 
		p->chan->cid.cid_name = NULL;

	if (p->owner->cid.cid_rdnis)
		p->chan->cid.cid_rdnis = strdup(p->owner->cid.cid_rdnis);
	else
		p->chan->cid.cid_rdnis = NULL;

	if (p->owner->cid.cid_ani)
		p->chan->cid.cid_ani = strdup(p->owner->cid.cid_ani);
	else
		p->chan->cid.cid_ani = NULL;

	strncpy(p->chan->language, p->owner->language, sizeof(p->chan->language) - 1);
	strncpy(p->chan->accountcode, p->owner->accountcode, sizeof(p->chan->accountcode) - 1);
	p->chan->cdrflags = p->owner->cdrflags;
	opbx_channel_inherit_variables(p->owner, p->chan);
	p->launchedpbx = 1;

	/* Start switch on sub channel */
	res = opbx_pbx_start(p->chan);
	opbx_mutex_unlock(&p->lock);
	return res;
}

#if 0
static void local_destroy(struct local_pvt *p)
{
	struct local_pvt *cur, *prev = NULL;
	opbx_mutex_lock(&locallock);
	cur = locals;
	while(cur) {
		if (cur == p) {
			if (prev)
				prev->next = cur->next;
			else
				locals = cur->next;
			opbx_mutex_destroy(cur);
			free(cur);
			break;
		}
		prev = cur;
		cur = cur->next;
	}
	opbx_mutex_unlock(&locallock);
	if (!cur)
		opbx_log(LOG_WARNING, "Unable ot find local '%s@%s' in local list\n", p->exten, p->context);
}
#endif

/*--- local_hangup: Hangup a call through the local proxy channel */
static int local_hangup(struct opbx_channel *ast)
{
	struct local_pvt *p = ast->tech_pvt;
	int isoutbound;
	struct opbx_frame f = { OPBX_FRAME_CONTROL, OPBX_CONTROL_HANGUP };
	struct local_pvt *cur, *prev=NULL;
	struct opbx_channel *ochan = NULL;
	int glaredetect;

	opbx_mutex_lock(&p->lock);
	isoutbound = IS_OUTBOUND(ast, p);
	if (isoutbound) {
		p->chan = NULL;
		p->launchedpbx = 0;
	} else
		p->owner = NULL;
	ast->tech_pvt = NULL;
	
	opbx_mutex_lock(&usecnt_lock);
	usecnt--;
	opbx_mutex_unlock(&usecnt_lock);
	
	if (!p->owner && !p->chan) {
		/* Okay, done with the private part now, too. */
		glaredetect = p->glaredetect;
		/* If we have a queue holding, don't actually destroy p yet, but
		   let local_queue do it. */
		if (p->glaredetect)
			p->cancelqueue = 1;
		opbx_mutex_unlock(&p->lock);
		/* Remove from list */
		opbx_mutex_lock(&locallock);
		cur = locals;
		while(cur) {
			if (cur == p) {
				if (prev)
					prev->next = cur->next;
				else
					locals = cur->next;
				break;
			}
			prev = cur;
			cur = cur->next;
		}
		opbx_mutex_unlock(&locallock);
		/* Grab / release lock just in case */
		opbx_mutex_lock(&p->lock);
		opbx_mutex_unlock(&p->lock);
		/* And destroy */
		if (!glaredetect) {
			opbx_mutex_destroy(&p->lock);
			free(p);
		}
		return 0;
	}
	if (p->chan && !p->launchedpbx)
		/* Need to actually hangup since there is no PBX */
		ochan = p->chan;
	else
		local_queue_frame(p, isoutbound, &f, NULL);
	opbx_mutex_unlock(&p->lock);
	if (ochan)
		opbx_hangup(ochan);
	return 0;
}

/*--- local_alloc: Create a call structure */
static struct local_pvt *local_alloc(char *data, int format)
{
	struct local_pvt *tmp;
	char *c;
	char *opts;

	tmp = malloc(sizeof(struct local_pvt));
	if (tmp) {
		memset(tmp, 0, sizeof(struct local_pvt));
		opbx_mutex_init(&tmp->lock);
		strncpy(tmp->exten, data, sizeof(tmp->exten) - 1);
		opts = strchr(tmp->exten, '/');
		if (opts) {
			*opts='\0';
			opts++;
			if (strchr(opts, 'n'))
				tmp->nooptimization = 1;
		}
		c = strchr(tmp->exten, '@');
		if (c) {
			*c = '\0';
			c++;
			strncpy(tmp->context, c, sizeof(tmp->context) - 1);
		} else
			strncpy(tmp->context, "default", sizeof(tmp->context) - 1);
		tmp->reqformat = format;
		if (!opbx_exists_extension(NULL, tmp->context, tmp->exten, 1, NULL)) {
			opbx_log(LOG_NOTICE, "No such extension/context %s@%s creating local channel\n", tmp->exten, tmp->context);
			opbx_mutex_destroy(&tmp->lock);
			free(tmp);
			tmp = NULL;
		} else {
			/* Add to list */
			opbx_mutex_lock(&locallock);
			tmp->next = locals;
			locals = tmp;
			opbx_mutex_unlock(&locallock);
		}
		
	}
	return tmp;
}

/*--- local_new: Start new local channel */
static struct opbx_channel *local_new(struct local_pvt *p, int state)
{
	struct opbx_channel *tmp, *tmp2;
	int randnum = opbx_random() & 0xffff;
	int fmt=0;

	tmp = opbx_channel_alloc(1);
	tmp2 = opbx_channel_alloc(1);
	if (!tmp || !tmp2) {
		if (tmp)
			opbx_channel_free(tmp);
		if (tmp2)
			opbx_channel_free(tmp2);
		opbx_log(LOG_WARNING, "Unable to allocate channel structure(s)\n");
		return NULL;
	} 

	tmp2->tech = tmp->tech = &local_tech;
	tmp->nativeformats = p->reqformat;
	tmp2->nativeformats = p->reqformat;
	snprintf(tmp->name, sizeof(tmp->name), "Local/%s@%s-%04x,1", p->exten, p->context, randnum);
	snprintf(tmp2->name, sizeof(tmp2->name), "Local/%s@%s-%04x,2", p->exten, p->context, randnum);
	tmp->type = type;
	tmp2->type = type;
	opbx_setstate(tmp, state);
	opbx_setstate(tmp2, OPBX_STATE_RING);

	fmt = opbx_best_codec(p->reqformat);
	tmp->writeformat = fmt;
	tmp2->writeformat = fmt;
	tmp->rawwriteformat = fmt;
	tmp2->rawwriteformat = fmt;
	tmp->readformat = fmt;
	tmp2->readformat = fmt;
	tmp->rawreadformat = fmt;
	tmp2->rawreadformat = fmt;

	tmp->tech_pvt = p;
	tmp2->tech_pvt = p;
	p->owner = tmp;
	p->chan = tmp2;
	opbx_mutex_lock(&usecnt_lock);
	usecnt++;
	usecnt++;
	opbx_mutex_unlock(&usecnt_lock);
	opbx_update_use_count();
	opbx_copy_string(tmp->context, p->context, sizeof(tmp->context));
	opbx_copy_string(tmp2->context, p->context, sizeof(tmp2->context));
	opbx_copy_string(tmp2->exten, p->exten, sizeof(tmp->exten));
	tmp->priority = 1;
	tmp2->priority = 1;

	return tmp;
}


/*--- local_request: Part of PBX interface */
static struct opbx_channel *local_request(const char *type, int format, void *data, int *cause)
{
	struct local_pvt *p;
	struct opbx_channel *chan = NULL;

	p = local_alloc(data, format);
	if (p)
		chan = local_new(p, OPBX_STATE_DOWN);
	return chan;
}

/*--- locals_show: CLI command "local show channels" */
static int locals_show(int fd, int argc, char **argv)
{
	struct local_pvt *p;

	if (argc != 3)
		return RESULT_SHOWUSAGE;
	opbx_mutex_lock(&locallock);
	p = locals;
	while(p) {
		opbx_mutex_lock(&p->lock);
		opbx_cli(fd, "%s -- %s@%s\n", p->owner ? p->owner->name : "<unowned>", p->exten, p->context);
		opbx_mutex_unlock(&p->lock);
		p = p->next;
	}
	if (!locals)
		opbx_cli(fd, "No local channels in use\n");
	opbx_mutex_unlock(&locallock);
	return RESULT_SUCCESS;
}

static char show_locals_usage[] = 
"Usage: local show channels\n"
"       Provides summary information on active local proxy channels.\n";

static struct opbx_cli_entry cli_show_locals = {
	{ "local", "show", "channels", NULL }, locals_show, 
	"Show status of local channels", show_locals_usage, NULL };

/*--- load_module: Load module into PBX, register channel */
int load_module()
{
	/* Make sure we can register our channel type */
	if (opbx_channel_register(&local_tech)) {
		opbx_log(LOG_ERROR, "Unable to register channel class %s\n", type);
		return -1;
	}
	opbx_cli_register(&cli_show_locals);
	return 0;
}

/*--- reload: Reload module */
int reload()
{
	return 0;
}

/*--- unload_module: Unload the local proxy channel from OpenPBX */
int unload_module()
{
	struct local_pvt *p;

	/* First, take us out of the channel loop */
	opbx_cli_unregister(&cli_show_locals);
	opbx_channel_unregister(&local_tech);
	if (!opbx_mutex_lock(&locallock)) {
		/* Hangup all interfaces if they have an owner */
		p = locals;
		while(p) {
			if (p->owner)
				opbx_softhangup(p->owner, OPBX_SOFTHANGUP_APPUNLOAD);
			p = p->next;
		}
		locals = NULL;
		opbx_mutex_unlock(&locallock);
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

