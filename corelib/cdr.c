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
 * \brief Call Detail Record API 
 * 
 * Includes code and algorithms from the Zapata library.
 *
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/lock.h"
#include "openpbx/channel.h"
#include "openpbx/cdr.h"
#include "openpbx/logger.h"
#include "openpbx/phone_no_utils.h"
#include "openpbx/causes.h"
#include "openpbx/options.h"
#include "openpbx/linkedlists.h"
#include "openpbx/utils.h"
#include "openpbx/sched.h"
#include "openpbx/config.h"
#include "openpbx/cli.h"
#include "openpbx/module.h"

int opbx_default_amaflags = OPBX_CDR_DOCUMENTATION;
char opbx_default_accountcode[OPBX_MAX_ACCOUNT_CODE] = "";

struct opbx_cdr_beitem {
	char name[20];
	char desc[80];
	opbx_cdrbe be;
	OPBX_LIST_ENTRY(opbx_cdr_beitem) list;
};

static OPBX_LIST_HEAD_STATIC(be_list, opbx_cdr_beitem);

struct opbx_cdr_batch_item {
	struct opbx_cdr *cdr;
	struct opbx_cdr_batch_item *next;
};

static struct opbx_cdr_batch {
	int size;
	struct opbx_cdr_batch_item *head;
	struct opbx_cdr_batch_item *tail;
} *batch = NULL;

static struct sched_context *sched;
static int cdr_sched = -1;
static pthread_t cdr_thread = OPBX_PTHREADT_NULL;

#define BATCH_SIZE_DEFAULT 100
#define BATCH_TIME_DEFAULT 300
#define BATCH_SCHEDULER_ONLY_DEFAULT 0
#define BATCH_SAFE_SHUTDOWN_DEFAULT 1

static int enabled;
static int batchmode;
static int batchsize;
static int batchtime;
static int batchscheduleronly;
static int batchsafeshutdown;

OPBX_MUTEX_DEFINE_STATIC(cdr_batch_lock);

/* these are used to wake up the CDR thread when there's work to do */
OPBX_MUTEX_DEFINE_STATIC(cdr_pending_lock);

/*
 * We do a lot of checking here in the CDR code to try to be sure we don't ever let a CDR slip
 * through our fingers somehow.  If someone allocates a CDR, it must be completely handled normally
 * or a WARNING shall be logged, so that we can best keep track of any escape condition where the CDR
 * isn't properly generated and posted.
 */

int opbx_cdr_register(char *name, char *desc, opbx_cdrbe be)
{
	struct opbx_cdr_beitem *i;

	if (!name)
		return -1;
	if (!be) {
		opbx_log(LOG_WARNING, "CDR engine '%s' lacks backend\n", name);
		return -1;
	}

	OPBX_LIST_LOCK(&be_list);
	OPBX_LIST_TRAVERSE(&be_list, i, list) {
		if (!strcasecmp(name, i->name))
			break;
	}
	OPBX_LIST_UNLOCK(&be_list);

	if (i) {
		opbx_log(LOG_WARNING, "Already have a CDR backend called '%s'\n", name);
		return -1;
	}

	i = malloc(sizeof(*i));
	if (!i) 	
		return -1;

	memset(i, 0, sizeof(*i));
	i->be = be;
	opbx_copy_string(i->name, name, sizeof(i->name));
	opbx_copy_string(i->desc, desc, sizeof(i->desc));

	OPBX_LIST_LOCK(&be_list);
	OPBX_LIST_INSERT_HEAD(&be_list, i, list);
	OPBX_LIST_UNLOCK(&be_list);

	return 0;
}

void opbx_cdr_unregister(char *name)
{
	struct opbx_cdr_beitem *i = NULL;

	OPBX_LIST_LOCK(&be_list);
	OPBX_LIST_TRAVERSE_SAFE_BEGIN(&be_list, i, list) {
		if (!strcasecmp(name, i->name)) {
			OPBX_LIST_REMOVE_CURRENT(&be_list, list);
			if (option_verbose > 1)
				opbx_verbose(VERBOSE_PREFIX_2 "Unregistered '%s' CDR backend\n", name);
			free(i);
			break;
		}
	}
	OPBX_LIST_TRAVERSE_SAFE_END;
	OPBX_LIST_UNLOCK(&be_list);
}

struct opbx_cdr *opbx_cdr_dup(struct opbx_cdr *cdr) 
{
	struct opbx_cdr *newcdr;

	if (!(newcdr = opbx_cdr_alloc())) {
		opbx_log(LOG_ERROR, "Memory Error!\n");
		return NULL;
	}

	memcpy(newcdr, cdr, sizeof(*newcdr));
	/* The varshead is unusable, volatile even, after the memcpy so we take care of that here */
	memset(&newcdr->varshead, 0, sizeof(newcdr->varshead));
	opbx_cdr_copy_vars(newcdr, cdr);
	newcdr->next = NULL;

	return newcdr;
}

static const char *opbx_cdr_getvar_internal(struct opbx_cdr *cdr, const char *name, int recur) 
{
	struct opbx_var_t *variables;
	struct varshead *headp;

	if (opbx_strlen_zero(name))
		return NULL;

	while (cdr) {
		headp = &cdr->varshead;
		OPBX_LIST_TRAVERSE(headp, variables, entries) {
			if (!strcasecmp(name, opbx_var_name(variables)))
				return opbx_var_value(variables);
		}
		if (!recur)
			break;
		cdr = cdr->next;
	}

	return NULL;
}

void opbx_cdr_getvar(struct opbx_cdr *cdr, const char *name, char **ret, char *workspace, int workspacelen, int recur) 
{
	struct tm tm;
	time_t t;
	const char *fmt = "%Y-%m-%d %T";
	const char *varbuf;

	*ret = NULL;
	/* special vars (the ones from the struct opbx_cdr when requested by name) 
	   I'd almost say we should convert all the stringed vals to vars */

	if (!strcasecmp(name, "clid"))
		opbx_copy_string(workspace, cdr->clid, workspacelen);
	else if (!strcasecmp(name, "src"))
		opbx_copy_string(workspace, cdr->src, workspacelen);
	else if (!strcasecmp(name, "dst"))
		opbx_copy_string(workspace, cdr->dst, workspacelen);
	else if (!strcasecmp(name, "dcontext"))
		opbx_copy_string(workspace, cdr->dcontext, workspacelen);
	else if (!strcasecmp(name, "channel"))
		opbx_copy_string(workspace, cdr->channel, workspacelen);
	else if (!strcasecmp(name, "dstchannel"))
		opbx_copy_string(workspace, cdr->dstchannel, workspacelen);
	else if (!strcasecmp(name, "lastapp"))
		opbx_copy_string(workspace, cdr->lastapp, workspacelen);
	else if (!strcasecmp(name, "lastdata"))
		opbx_copy_string(workspace, cdr->lastdata, workspacelen);
	else if (!strcasecmp(name, "start")) {
		t = cdr->start.tv_sec;
		if (t) {
			localtime_r(&t, &tm);
			strftime(workspace, workspacelen, fmt, &tm);
		}
	} else if (!strcasecmp(name, "answer")) {
		t = cdr->answer.tv_sec;
		if (t) {
			localtime_r(&t, &tm);
			strftime(workspace, workspacelen, fmt, &tm);
		}
	} else if (!strcasecmp(name, "end")) {
		t = cdr->end.tv_sec;
		if (t) {
			localtime_r(&t, &tm);
			strftime(workspace, workspacelen, fmt, &tm);
		}
	} else if (!strcasecmp(name, "duration"))
		snprintf(workspace, workspacelen, "%d", cdr->duration);
	else if (!strcasecmp(name, "billsec"))
		snprintf(workspace, workspacelen, "%d", cdr->billsec);
	else if (!strcasecmp(name, "disposition"))
		opbx_copy_string(workspace, opbx_cdr_disp2str(cdr->disposition), workspacelen);
	else if (!strcasecmp(name, "amaflags"))
		opbx_copy_string(workspace, opbx_cdr_flags2str(cdr->amaflags), workspacelen);
	else if (!strcasecmp(name, "accountcode"))
		opbx_copy_string(workspace, cdr->accountcode, workspacelen);
	else if (!strcasecmp(name, "uniqueid"))
		opbx_copy_string(workspace, cdr->uniqueid, workspacelen);
	else if (!strcasecmp(name, "userfield"))
		opbx_copy_string(workspace, cdr->userfield, workspacelen);
	else if ((varbuf = opbx_cdr_getvar_internal(cdr, name, recur)))
		opbx_copy_string(workspace, varbuf, workspacelen);

	if (!opbx_strlen_zero(workspace))
		*ret = workspace;
}

int opbx_cdr_setvar(struct opbx_cdr *cdr, const char *name, const char *value, int recur) 
{
	struct opbx_var_t *newvariable;
	struct varshead *headp;
	const char *read_only[] = { "clid", "src", "dst", "dcontext", "channel", "dstchannel",
				    "lastapp", "lastdata", "start", "answer", "end", "duration",
				    "billsec", "disposition", "amaflags", "accountcode", "uniqueid",
				    "userfield", NULL };
	int x;
	
	for(x = 0; read_only[x]; x++) {
		if (!strcasecmp(name, read_only[x])) {
			opbx_log(LOG_ERROR, "Attempt to set a read-only variable!.\n");
			return -1;
		}
	}

	if (!cdr) {
		opbx_log(LOG_ERROR, "Attempt to set a variable on a nonexistent CDR record.\n");
		return -1;
	}

	while (cdr) {
		headp = &cdr->varshead;
		OPBX_LIST_TRAVERSE_SAFE_BEGIN(headp, newvariable, entries) {
			if (!strcasecmp(opbx_var_name(newvariable), name)) {
				/* there is already such a variable, delete it */
				OPBX_LIST_REMOVE_CURRENT(headp, entries);
				opbx_var_delete(newvariable);
				break;
			}
		}
		OPBX_LIST_TRAVERSE_SAFE_END;

		if (value) {
			newvariable = opbx_var_assign(name, value);
			OPBX_LIST_INSERT_HEAD(headp, newvariable, entries);
		}

		if (!recur) {
			break;
		}

		cdr = cdr->next;
	}

	return 0;
}

int opbx_cdr_copy_vars(struct opbx_cdr *to_cdr, struct opbx_cdr *from_cdr)
{
	struct opbx_var_t *variables, *newvariable = NULL;
	struct varshead *headpa, *headpb;
	char *var, *val;
	int x = 0;

	headpa = &from_cdr->varshead;
	headpb = &to_cdr->varshead;

	OPBX_LIST_TRAVERSE(headpa,variables,entries) {
		if (variables &&
		    (var = opbx_var_name(variables)) && (val = opbx_var_value(variables)) &&
		    !opbx_strlen_zero(var) && !opbx_strlen_zero(val)) {
			newvariable = opbx_var_assign(var, val);
			OPBX_LIST_INSERT_HEAD(headpb, newvariable, entries);
			x++;
		}
	}

	return x;
}

int opbx_cdr_serialize_variables(struct opbx_cdr *cdr, char *buf, size_t size, char delim, char sep, int recur) 
{
	struct opbx_var_t *variables;
	char *var, *val;
	char *tmp;
	char workspace[256];
	int total = 0, x = 0, i;
	const char *cdrcols[] = { 
		"clid",
		"src",
		"dst",
		"dcontext",
		"channel",
		"dstchannel",
		"lastapp",
		"lastdata",
		"start",
		"answer",
		"end",
		"duration",
		"billsec",
		"disposition",
		"amaflags",
		"accountcode",
		"uniqueid",
		"userfield"
	};

	memset(buf, 0, size);

	for (; cdr; cdr = recur ? cdr->next : NULL) {
		if (++x > 1)
			opbx_build_string(&buf, &size, "\n");

		OPBX_LIST_TRAVERSE(&cdr->varshead, variables, entries) {
			if (variables &&
			    (var = opbx_var_name(variables)) && (val = opbx_var_value(variables)) &&
			    !opbx_strlen_zero(var) && !opbx_strlen_zero(val)) {
				if (opbx_build_string(&buf, &size, "level %d: %s%c%s%c", x, var, delim, val, sep)) {
 					opbx_log(LOG_ERROR, "Data Buffer Size Exceeded!\n");
 					break;
				} else
					total++;
			} else 
				break;
		}

		for (i = 0; i < (sizeof(cdrcols) / sizeof(cdrcols[0])); i++) {
			opbx_cdr_getvar(cdr, cdrcols[i], &tmp, workspace, sizeof(workspace), 0);
			if (!tmp)
				continue;
			
			if (opbx_build_string(&buf, &size, "level %d: %s%c%s%c", x, cdrcols[i], delim, tmp, sep)) {
				opbx_log(LOG_ERROR, "Data Buffer Size Exceeded!\n");
				break;
			} else
				total++;
		}
	}

	return total;
}


void opbx_cdr_free_vars(struct opbx_cdr *cdr, int recur)
{
	struct varshead *headp;
	struct opbx_var_t *vardata;

	/* clear variables */
	while (cdr) {
		headp = &cdr->varshead;
		while (!OPBX_LIST_EMPTY(headp)) {
			vardata = OPBX_LIST_REMOVE_HEAD(headp, entries);
			opbx_var_delete(vardata);
		}

		if (!recur) {
			break;
		}

		cdr = cdr->next;
	}
}

void opbx_cdr_free(struct opbx_cdr *cdr)
{
	char *chan;
	struct opbx_cdr *next; 

	while (cdr) {
		next = cdr->next;
		chan = !opbx_strlen_zero(cdr->channel) ? cdr->channel : "<unknown>";
		if (!opbx_test_flag(cdr, OPBX_CDR_FLAG_POSTED) && !opbx_test_flag(cdr, OPBX_CDR_FLAG_POST_DISABLED))
			opbx_log(LOG_WARNING, "CDR on channel '%s' not posted\n", chan);
		if (opbx_tvzero(cdr->end))
			opbx_log(LOG_WARNING, "CDR on channel '%s' lacks end\n", chan);
		if (opbx_tvzero(cdr->start))
			opbx_log(LOG_WARNING, "CDR on channel '%s' lacks start\n", chan);

		opbx_cdr_free_vars(cdr, 0);
		free(cdr);
		cdr = next;
	}
}

struct opbx_cdr *opbx_cdr_alloc(void)
{
	struct opbx_cdr *cdr;

	cdr = malloc(sizeof(*cdr));
	if (cdr)
		memset(cdr, 0, sizeof(*cdr));

	return cdr;
}

void opbx_cdr_start(struct opbx_cdr *cdr)
{
	char *chan; 

	while (cdr) {
		if (!opbx_test_flag(cdr, OPBX_CDR_FLAG_LOCKED)) {
			chan = !opbx_strlen_zero(cdr->channel) ? cdr->channel : "<unknown>";
			if (opbx_test_flag(cdr, OPBX_CDR_FLAG_POSTED))
				opbx_log(LOG_WARNING, "CDR on channel '%s' already posted\n", chan);
			if (!opbx_tvzero(cdr->start))
				opbx_log(LOG_WARNING, "CDR on channel '%s' already started\n", chan);
			cdr->start = opbx_tvnow();
		}
		cdr = cdr->next;
	}
}

void opbx_cdr_answer(struct opbx_cdr *cdr)
{
	char *chan; 

	while (cdr) {
		chan = !opbx_strlen_zero(cdr->channel) ? cdr->channel : "<unknown>";
		if (opbx_test_flag(cdr, OPBX_CDR_FLAG_POSTED))
			opbx_log(LOG_WARNING, "CDR on channel '%s' already posted\n", chan);
		if (cdr->disposition < OPBX_CDR_ANSWERED)
			cdr->disposition = OPBX_CDR_ANSWERED;
		if (opbx_tvzero(cdr->answer))
			cdr->answer = opbx_tvnow();
		cdr = cdr->next;
	}
}

void opbx_cdr_busy(struct opbx_cdr *cdr)
{
	char *chan; 

	while (cdr) {
		if (!opbx_test_flag(cdr, OPBX_CDR_FLAG_LOCKED)) {
			chan = !opbx_strlen_zero(cdr->channel) ? cdr->channel : "<unknown>";
			if (opbx_test_flag(cdr, OPBX_CDR_FLAG_POSTED))
				opbx_log(LOG_WARNING, "CDR on channel '%s' already posted\n", chan);
			if (cdr->disposition < OPBX_CDR_BUSY)
				cdr->disposition = OPBX_CDR_BUSY;
		}
		cdr = cdr->next;
	}
}

void opbx_cdr_failed(struct opbx_cdr *cdr)
{
	char *chan; 

	while (cdr) {
		chan = !opbx_strlen_zero(cdr->channel) ? cdr->channel : "<unknown>";
		if (opbx_test_flag(cdr, OPBX_CDR_FLAG_POSTED))
			opbx_log(LOG_WARNING, "CDR on channel '%s' already posted\n", chan);
		if (!opbx_test_flag(cdr, OPBX_CDR_FLAG_LOCKED))
			cdr->disposition = OPBX_CDR_FAILED;
		cdr = cdr->next;
	}
}

int opbx_cdr_disposition(struct opbx_cdr *cdr, int cause)
{
	int res = 0;

	while (cdr) {
		switch(cause) {
		case OPBX_CAUSE_BUSY:
			opbx_cdr_busy(cdr);
			break;
		case OPBX_CAUSE_FAILURE:
			opbx_cdr_failed(cdr);
			break;
		case OPBX_CAUSE_NORMAL:
			break;
		case OPBX_CAUSE_NOTDEFINED:
			res = -1;
			break;
		default:
			res = -1;
			opbx_log(LOG_WARNING, "Cause not handled\n");
		}
		cdr = cdr->next;
	}
	return res;
}

void opbx_cdr_setdestchan(struct opbx_cdr *cdr, char *chann)
{
	char *chan; 

	while (cdr) {
		chan = !opbx_strlen_zero(cdr->channel) ? cdr->channel : "<unknown>";
		if (opbx_test_flag(cdr, OPBX_CDR_FLAG_POSTED))
			opbx_log(LOG_WARNING, "CDR on channel '%s' already posted\n", chan);
		if (!opbx_test_flag(cdr, OPBX_CDR_FLAG_LOCKED))
			opbx_copy_string(cdr->dstchannel, chann, sizeof(cdr->dstchannel));
		cdr = cdr->next;
	}
}

void opbx_cdr_setapp(struct opbx_cdr *cdr, char *app, char *data)
{
	char *chan; 

	while (cdr) {
		if (!opbx_test_flag(cdr, OPBX_CDR_FLAG_LOCKED)) {
			chan = !opbx_strlen_zero(cdr->channel) ? cdr->channel : "<unknown>";
			if (opbx_test_flag(cdr, OPBX_CDR_FLAG_POSTED))
				opbx_log(LOG_WARNING, "CDR on channel '%s' already posted\n", chan);
			if (!app)
				app = "";
			opbx_copy_string(cdr->lastapp, app, sizeof(cdr->lastapp));
			if (!data)
				data = "";
			opbx_copy_string(cdr->lastdata, data, sizeof(cdr->lastdata));
		}
		cdr = cdr->next;
	}
}

int opbx_cdr_setcid(struct opbx_cdr *cdr, struct opbx_channel *c)
{
	char tmp[OPBX_MAX_EXTENSION] = "";
	char *num;

	while (cdr) {
		if (!opbx_test_flag(cdr, OPBX_CDR_FLAG_LOCKED)) {
			/* Grab source from ANI or normal Caller*ID */
			num = c->cid.cid_ani ? c->cid.cid_ani : c->cid.cid_num;
			
			if (c->cid.cid_name && num)
				snprintf(tmp, sizeof(tmp), "\"%s\" <%s>", c->cid.cid_name, num);
			else if (c->cid.cid_name)
				opbx_copy_string(tmp, c->cid.cid_name, sizeof(tmp));
			else if (num)
				opbx_copy_string(tmp, num, sizeof(tmp));
			opbx_copy_string(cdr->clid, tmp, sizeof(cdr->clid));
			opbx_copy_string(cdr->src, num ? num : "", sizeof(cdr->src));
		}
		cdr = cdr->next;
	}

	return 0;
}


int opbx_cdr_init(struct opbx_cdr *cdr, struct opbx_channel *c)
{
	char *chan;
	char *num;
	char tmp[OPBX_MAX_EXTENSION] = "";

	while (cdr) {
		if (!opbx_test_flag(cdr, OPBX_CDR_FLAG_LOCKED)) {
			chan = !opbx_strlen_zero(cdr->channel) ? cdr->channel : "<unknown>";
			if (!opbx_strlen_zero(cdr->channel)) 
				opbx_log(LOG_WARNING, "CDR already initialized on '%s'\n", chan); 
			opbx_copy_string(cdr->channel, c->name, sizeof(cdr->channel));
			/* Grab source from ANI or normal Caller*ID */
			num = c->cid.cid_ani ? c->cid.cid_ani : c->cid.cid_num;
			
			if (c->cid.cid_name && num)
				snprintf(tmp, sizeof(tmp), "\"%s\" <%s>", c->cid.cid_name, num);
			else if (c->cid.cid_name)
				opbx_copy_string(tmp, c->cid.cid_name, sizeof(tmp));
			else if (num)
				opbx_copy_string(tmp, num, sizeof(tmp));
			opbx_copy_string(cdr->clid, tmp, sizeof(cdr->clid));
			opbx_copy_string(cdr->src, num ? num : "", sizeof(cdr->src));

			cdr->disposition = (c->_state == OPBX_STATE_UP) ?  OPBX_CDR_ANSWERED : OPBX_CDR_NOANSWER;
			cdr->amaflags = c->amaflags ? c->amaflags :  opbx_default_amaflags;
			opbx_copy_string(cdr->accountcode, c->accountcode, sizeof(cdr->accountcode));
			/* Destination information */
			opbx_copy_string(cdr->dst, c->exten, sizeof(cdr->dst));
			opbx_copy_string(cdr->dcontext, c->context, sizeof(cdr->dcontext));
			/* Unique call identifier */
			opbx_copy_string(cdr->uniqueid, c->uniqueid, sizeof(cdr->uniqueid));
		}
		cdr = cdr->next;
	}
	return 0;
}

void opbx_cdr_end(struct opbx_cdr *cdr)
{
	char *chan;

	while (cdr) {
		chan = !opbx_strlen_zero(cdr->channel) ? cdr->channel : "<unknown>";
		if (opbx_test_flag(cdr, OPBX_CDR_FLAG_POSTED))
			opbx_log(LOG_WARNING, "CDR on channel '%s' already posted\n", chan);
		if (opbx_tvzero(cdr->start))
			opbx_log(LOG_WARNING, "CDR on channel '%s' has not started\n", chan);
		if (opbx_tvzero(cdr->end))
			cdr->end = opbx_tvnow();
		cdr = cdr->next;
	}
}

char *opbx_cdr_disp2str(int disposition)
{
	switch (disposition) {
	case OPBX_CDR_NOANSWER:
		return "NO ANSWER";
	case OPBX_CDR_FAILED:
		return "FAILED";		
	case OPBX_CDR_BUSY:
		return "BUSY";		
	case OPBX_CDR_ANSWERED:
		return "ANSWERED";
	}
	return "UNKNOWN";
}

char *opbx_cdr_flags2str(int flag)
{
	switch(flag) {
	case OPBX_CDR_OMIT:
		return "OMIT";
	case OPBX_CDR_BILLING:
		return "BILLING";
	case OPBX_CDR_DOCUMENTATION:
		return "DOCUMENTATION";
	}
	return "Unknown";
}

int opbx_cdr_setaccount(struct opbx_channel *chan, const char *account)
{
	struct opbx_cdr *cdr = chan->cdr;

	opbx_copy_string(chan->accountcode, account, sizeof(chan->accountcode));
	while (cdr) {
		if (!opbx_test_flag(cdr, OPBX_CDR_FLAG_LOCKED))
			opbx_copy_string(cdr->accountcode, chan->accountcode, sizeof(cdr->accountcode));
		cdr = cdr->next;
	}
	return 0;
}

int opbx_cdr_setamaflags(struct opbx_channel *chan, const char *flag)
{
	struct opbx_cdr *cdr = chan->cdr;
	int newflag;

	newflag = opbx_cdr_amaflags2int(flag);
	if (newflag)
		cdr->amaflags = newflag;

	return 0;
}

int opbx_cdr_setuserfield(struct opbx_channel *chan, const char *userfield)
{
	struct opbx_cdr *cdr = chan->cdr;

	while (cdr) {
		if (!opbx_test_flag(cdr, OPBX_CDR_FLAG_LOCKED)) 
			opbx_copy_string(cdr->userfield, userfield, sizeof(cdr->userfield));
		cdr = cdr->next;
	}

	return 0;
}

int opbx_cdr_appenduserfield(struct opbx_channel *chan, const char *userfield)
{
	struct opbx_cdr *cdr = chan->cdr;

	while (cdr) {
		int len = strlen(cdr->userfield);

		if (!opbx_test_flag(cdr, OPBX_CDR_FLAG_LOCKED))
			strncpy(cdr->userfield+len, userfield, sizeof(cdr->userfield) - len - 1);

		cdr = cdr->next;
	}

	return 0;
}

int opbx_cdr_update(struct opbx_channel *c)
{
	struct opbx_cdr *cdr = c->cdr;
	char *num;
	char tmp[OPBX_MAX_EXTENSION] = "";

	while (cdr) {
		if (!opbx_test_flag(cdr, OPBX_CDR_FLAG_LOCKED)) {
			num = c->cid.cid_ani ? c->cid.cid_ani : c->cid.cid_num;
			
			if (c->cid.cid_name && num)
				snprintf(tmp, sizeof(tmp), "\"%s\" <%s>", c->cid.cid_name, num);
			else if (c->cid.cid_name)
				opbx_copy_string(tmp, c->cid.cid_name, sizeof(tmp));
			else if (num)
				opbx_copy_string(tmp, num, sizeof(tmp));
			opbx_copy_string(cdr->clid, tmp, sizeof(cdr->clid));
			opbx_copy_string(cdr->src, num ? num : "", sizeof(cdr->src));

			/* Copy account code et-al */	
			opbx_copy_string(cdr->accountcode, c->accountcode, sizeof(cdr->accountcode));
			/* Destination information */
			opbx_copy_string(cdr->dst, (opbx_strlen_zero(c->macroexten)) ? c->exten : c->macroexten, sizeof(cdr->dst));
			opbx_copy_string(cdr->dcontext, (opbx_strlen_zero(c->macrocontext)) ? c->context : c->macrocontext, sizeof(cdr->dcontext));
		}
		cdr = cdr->next;
	}

	return 0;
}

int opbx_cdr_amaflags2int(const char *flag)
{
	if (!strcasecmp(flag, "default"))
		return 0;
	if (!strcasecmp(flag, "omit"))
		return OPBX_CDR_OMIT;
	if (!strcasecmp(flag, "billing"))
		return OPBX_CDR_BILLING;
	if (!strcasecmp(flag, "documentation"))
		return OPBX_CDR_DOCUMENTATION;
	return -1;
}

static void post_cdr(struct opbx_cdr *cdr)
{
	char *chan;
	struct opbx_cdr_beitem *i;

	while (cdr) {
		chan = !opbx_strlen_zero(cdr->channel) ? cdr->channel : "<unknown>";
		if (opbx_test_flag(cdr, OPBX_CDR_FLAG_POSTED))
			opbx_log(LOG_WARNING, "CDR on channel '%s' already posted\n", chan);
		if (opbx_tvzero(cdr->end))
			opbx_log(LOG_WARNING, "CDR on channel '%s' lacks end\n", chan);
		if (opbx_tvzero(cdr->start))
			opbx_log(LOG_WARNING, "CDR on channel '%s' lacks start\n", chan);
		cdr->duration = cdr->end.tv_sec - cdr->start.tv_sec + (cdr->end.tv_usec - cdr->start.tv_usec) / 1000000;
		if (!opbx_tvzero(cdr->answer))
			cdr->billsec = cdr->end.tv_sec - cdr->answer.tv_sec + (cdr->end.tv_usec - cdr->answer.tv_usec) / 1000000;
		else
			cdr->billsec = 0;
		opbx_set_flag(cdr, OPBX_CDR_FLAG_POSTED);
		OPBX_LIST_LOCK(&be_list);
		OPBX_LIST_TRAVERSE(&be_list, i, list) {
			i->be(cdr);
		}
		OPBX_LIST_UNLOCK(&be_list);
		cdr = cdr->next;
	}
}

void opbx_cdr_reset(struct opbx_cdr *cdr, int flags)
{
	struct opbx_flags tmp = {flags};
	struct opbx_cdr *dup;


	while (cdr) {
		/* Detach if post is requested */
		if (opbx_test_flag(&tmp, OPBX_CDR_FLAG_LOCKED) || !opbx_test_flag(cdr, OPBX_CDR_FLAG_LOCKED)) {
			if (opbx_test_flag(&tmp, OPBX_CDR_FLAG_POSTED)) {
				opbx_cdr_end(cdr);
				if ((dup = opbx_cdr_dup(cdr))) {
					opbx_cdr_detach(dup);
				}
				opbx_set_flag(cdr, OPBX_CDR_FLAG_POSTED);
			}

			/* clear variables */
			if (!opbx_test_flag(&tmp, OPBX_CDR_FLAG_KEEP_VARS)) {
				opbx_cdr_free_vars(cdr, 0);
			}

			/* Reset to initial state */
			opbx_clear_flag(cdr, OPBX_FLAGS_ALL);	
			memset(&cdr->start, 0, sizeof(cdr->start));
			memset(&cdr->end, 0, sizeof(cdr->end));
			memset(&cdr->answer, 0, sizeof(cdr->answer));
			cdr->billsec = 0;
			cdr->duration = 0;
			opbx_cdr_start(cdr);
			cdr->disposition = OPBX_CDR_NOANSWER;
		}
			
		cdr = cdr->next;
	}
}

struct opbx_cdr *opbx_cdr_append(struct opbx_cdr *cdr, struct opbx_cdr *newcdr) 
{
	struct opbx_cdr *ret;

	if (cdr) {
		ret = cdr;

		while (cdr->next)
			cdr = cdr->next;
		cdr->next = newcdr;
	} else {
		ret = newcdr;
	}

	return ret;
}

/* Don't call without cdr_batch_lock */
static void reset_batch(void)
{
	batch->size = 0;
	batch->head = NULL;
	batch->tail = NULL;
}

/* Don't call without cdr_batch_lock */
static int init_batch(void)
{
	/* This is the single meta-batch used to keep track of all CDRs during the entire life of the program */
	batch = malloc(sizeof(*batch));
	if (!batch) {
		opbx_log(LOG_WARNING, "CDR: out of memory while trying to handle batched records, data will most likely be lost\n");
		return -1;
	}

	reset_batch();

	return 0;
}

static void *do_batch_backend_process(void *data)
{
	struct opbx_cdr_batch_item *processeditem;
	struct opbx_cdr_batch_item *batchitem = data;

	/* Push each CDR into storage mechanism(s) and free all the memory */
	while (batchitem) {
		post_cdr(batchitem->cdr);
		opbx_cdr_free(batchitem->cdr);
		processeditem = batchitem;
		batchitem = batchitem->next;
		free(processeditem);
	}

	return NULL;
}

void opbx_cdr_submit_batch(int shutdown)
{
	struct opbx_cdr_batch_item *oldbatchitems = NULL;
	pthread_attr_t attr;
	pthread_t batch_post_thread = OPBX_PTHREADT_NULL;

	/* if there's no batch, or no CDRs in the batch, then there's nothing to do */
	if (!batch || !batch->head)
		return;

	/* move the old CDRs aside, and prepare a new CDR batch */
	opbx_mutex_lock(&cdr_batch_lock);
	oldbatchitems = batch->head;
	reset_batch();
	opbx_mutex_unlock(&cdr_batch_lock);

	/* if configured, spawn a new thread to post these CDRs,
	   also try to save as much as possible if we are shutting down safely */
	if (batchscheduleronly || shutdown) {
		if (option_debug)
			opbx_log(LOG_DEBUG, "CDR single-threaded batch processing begins now\n");
		do_batch_backend_process(oldbatchitems);
	} else {
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		if (opbx_pthread_create(&batch_post_thread, &attr, do_batch_backend_process, oldbatchitems)) {
			opbx_log(LOG_WARNING, "CDR processing thread could not detach, now trying in this thread\n");
			do_batch_backend_process(oldbatchitems);
		} else {
			if (option_debug)
				opbx_log(LOG_DEBUG, "CDR multi-threaded batch processing begins now\n");
		}
	}
}

static int submit_scheduled_batch(void *data)
{
	opbx_mutex_lock(&cdr_pending_lock);
	opbx_cdr_submit_batch(0);
	opbx_mutex_unlock(&cdr_pending_lock);

	/* manually reschedule from this point in time */
	cdr_sched = opbx_sched_add(sched, batchtime * 1000, submit_scheduled_batch, NULL);
	/* returning zero so the scheduler does not automatically reschedule */
	return 0;
}

static void submit_unscheduled_batch(void)
{
	/* this is okay since we are not being called from within the scheduler */
	if (cdr_sched > -1)
		opbx_sched_del(sched, cdr_sched);
	/* schedule the submission to occur ASAP (1 ms) */
	cdr_sched = opbx_sched_add(sched, 1, submit_scheduled_batch, NULL);
}

void opbx_cdr_detach(struct opbx_cdr *cdr)
{
	struct opbx_cdr_batch_item *newtail;
	int curr;

	/* maybe they disabled CDR stuff completely, so just drop it */
	if (!enabled) {
		if (option_debug)
			opbx_log(LOG_DEBUG, "Dropping CDR !\n");
		opbx_set_flag(cdr, OPBX_CDR_FLAG_POST_DISABLED);
		opbx_cdr_free(cdr);
		return;
	}

	/* post stuff immediately if we are not in batch mode, this is legacy behaviour */
	if (!batchmode) {
		post_cdr(cdr);
		opbx_cdr_free(cdr);
		return;
	}

	/* otherwise, each CDR gets put into a batch list (at the end) */
	if (option_debug)
		opbx_log(LOG_DEBUG, "CDR detaching from this thread\n");

	/* we'll need a new tail for every CDR */
	newtail = malloc(sizeof(*newtail));
	if (!newtail) {
		opbx_log(LOG_WARNING, "CDR: out of memory while trying to detach, will try in this thread instead\n");
		post_cdr(cdr);
		opbx_cdr_free(cdr);
		return;
	}
	memset(newtail, 0, sizeof(*newtail));

	/* don't traverse a whole list (just keep track of the tail) */
	opbx_mutex_lock(&cdr_batch_lock);
	if (!batch)
		init_batch();
	if (!batch->head) {
		/* new batch is empty, so point the head at the new tail */
		batch->head = newtail;
	} else {
		/* already got a batch with something in it, so just append a new tail */
		batch->tail->next = newtail;
	}
	newtail->cdr = cdr;
	batch->tail = newtail;
	curr = batch->size++;
	opbx_mutex_unlock(&cdr_batch_lock);

	/* if we have enough stuff to post, then do it */
	if (curr >= (batchsize - 1))
		submit_unscheduled_batch();
}


static int handle_cli_status(int fd, int argc, char *argv[])
{
	struct opbx_cdr_beitem *beitem=NULL;
	int cnt=0;
	long nextbatchtime=0;

	if (argc > 2)
		return RESULT_SHOWUSAGE;

	opbx_cli(fd, "CDR logging: %s\n", enabled ? "enabled" : "disabled");
	opbx_cli(fd, "CDR mode: %s\n", batchmode ? "batch" : "simple");
	if (enabled) {
		if (batchmode) {
			if (batch)
				cnt = batch->size;
			if (cdr_sched > -1)
				nextbatchtime = opbx_sched_when(sched, cdr_sched);
			opbx_cli(fd, "CDR safe shut down: %s\n", batchsafeshutdown ? "enabled" : "disabled");
			opbx_cli(fd, "CDR batch threading model: %s\n", batchscheduleronly ? "scheduler only" : "scheduler plus separate threads");
			opbx_cli(fd, "CDR current batch size: %d record%s\n", cnt, (cnt != 1) ? "s" : "");
			opbx_cli(fd, "CDR maximum batch size: %d record%s\n", batchsize, (batchsize != 1) ? "s" : "");
			opbx_cli(fd, "CDR maximum batch time: %d second%s\n", batchtime, (batchtime != 1) ? "s" : "");
			opbx_cli(fd, "CDR next scheduled batch processing time: %ld second%s\n", nextbatchtime, (nextbatchtime != 1) ? "s" : "");
		}
		OPBX_LIST_LOCK(&be_list);
		OPBX_LIST_TRAVERSE(&be_list, beitem, list) {
			opbx_cli(fd, "CDR registered backend: %s\n", beitem->name);
		}
		OPBX_LIST_UNLOCK(&be_list);
	}

	return 0;
}

static int handle_cli_submit(int fd, int argc, char *argv[])
{
	if (argc > 2)
		return RESULT_SHOWUSAGE;

	submit_unscheduled_batch();
	opbx_cli(fd, "Submitted CDRs to backend engines for processing.  This may take a while.\n");

	return 0;
}

static struct opbx_cli_entry cli_submit = {
	.cmda = { "cdr", "submit", NULL },
	.handler = handle_cli_submit,
	.summary = "Posts all pending batched CDR data",
	.usage =
	"Usage: cdr submit\n"
	"       Posts all pending batched CDR data to the configured CDR backend engine modules.\n"
};

static struct opbx_cli_entry cli_status = {
	.cmda = { "cdr", "status", NULL },
	.handler = handle_cli_status,
	.summary = "Display the CDR status",
	.usage =
	"Usage: cdr status\n"
	"	Displays the Call Detail Record engine system status.\n"
};

static int do_reload(void)
{
	struct opbx_config *config;
	const char *enabled_value;
	const char *batched_value;
	const char *scheduleronly_value;
	const char *batchsafeshutdown_value;
	const char *size_value;
	const char *time_value;
	int cfg_size;
	int cfg_time;
	int was_enabled;
	int was_batchmode;
	int res=0;

	opbx_mutex_lock(&cdr_batch_lock);

	batchsize = BATCH_SIZE_DEFAULT;
	batchtime = BATCH_TIME_DEFAULT;
	batchscheduleronly = BATCH_SCHEDULER_ONLY_DEFAULT;
	batchsafeshutdown = BATCH_SAFE_SHUTDOWN_DEFAULT;
	was_enabled = enabled;
	was_batchmode = batchmode;
	enabled = 1;
	batchmode = 0;

	/* don't run the next scheduled CDR posting while reloading */
	if (cdr_sched > -1)
		opbx_sched_del(sched, cdr_sched);

	if ((config = opbx_config_load("cdr.conf"))) {
		if ((enabled_value = opbx_variable_retrieve(config, "general", "enable"))) {
			enabled = opbx_true(enabled_value);
		}
		if ((batched_value = opbx_variable_retrieve(config, "general", "batch"))) {
			batchmode = opbx_true(batched_value);
		}
		if ((scheduleronly_value = opbx_variable_retrieve(config, "general", "scheduleronly"))) {
			batchscheduleronly = opbx_true(scheduleronly_value);
		}
		if ((batchsafeshutdown_value = opbx_variable_retrieve(config, "general", "safeshutdown"))) {
			batchsafeshutdown = opbx_true(batchsafeshutdown_value);
		}
		if ((size_value = opbx_variable_retrieve(config, "general", "size"))) {
			if (sscanf(size_value, "%d", &cfg_size) < 1)
				opbx_log(LOG_WARNING, "Unable to convert '%s' to a numeric value.\n", size_value);
			else if (size_value < 0)
				opbx_log(LOG_WARNING, "Invalid maximum batch size '%d' specified, using default\n", cfg_size);
			else
				batchsize = cfg_size;
		}
		if ((time_value = opbx_variable_retrieve(config, "general", "time"))) {
			if (sscanf(time_value, "%d", &cfg_time) < 1)
				opbx_log(LOG_WARNING, "Unable to convert '%s' to a numeric value.\n", time_value);
			else if (time_value < 0)
				opbx_log(LOG_WARNING, "Invalid maximum batch time '%d' specified, using default\n", cfg_time);
			else
				batchtime = cfg_time;
		}
	}

	if (enabled && !batchmode) {
		opbx_log(LOG_NOTICE, "CDR simple logging enabled.\n");
	} else if (enabled && batchmode) {
		cdr_sched = opbx_sched_add(sched, batchtime * 1000, submit_scheduled_batch, NULL);
		opbx_log(LOG_NOTICE, "CDR batch mode logging enabled, first of either size %d or time %d seconds.\n", batchsize, batchtime);
	} else {
		opbx_log(LOG_NOTICE, "CDR logging disabled, data will be lost.\n");
	}

	/* if this reload enabled the CDR batch mode, create the background thread
	   if it does not exist */
	if (enabled && batchmode && (!was_enabled || !was_batchmode) && (cdr_thread == OPBX_PTHREADT_NULL)) {
		opbx_cli_register(&cli_submit);
		opbx_register_atexit(opbx_cdr_engine_term);
		res = 0;
	/* if this reload disabled the CDR and/or batch mode and there is a background thread,
	   kill it */
	}else if (((!enabled && was_enabled) || (!batchmode && was_batchmode)) && (cdr_thread != OPBX_PTHREADT_NULL)) {
		cdr_thread = OPBX_PTHREADT_NULL;
		opbx_cli_unregister(&cli_submit);
		opbx_unregister_atexit(opbx_cdr_engine_term);
		res = 0;
		/* if leaving batch mode, then post the CDRs in the batch,
		   and don't reschedule, since we are stopping CDR logging */
		if (!batchmode && was_batchmode) {
			opbx_cdr_engine_term();
		}
	} else {
		res = 0;
	}

	opbx_mutex_unlock(&cdr_batch_lock);
	opbx_config_destroy(config);

	return res;
}

int opbx_cdr_engine_init(void)
{
	int res;

	sched = sched_context_create();
	if (!sched) {
		opbx_log(LOG_ERROR, "Unable to create schedule context.\n");
		return -1;
	}

	opbx_cli_register(&cli_status);

	res = do_reload();
	if (res) {
		opbx_mutex_lock(&cdr_batch_lock);
		res = init_batch();
		opbx_mutex_unlock(&cdr_batch_lock);
	}

	return res;
}

/* This actually gets called a couple of times at shutdown.  Once, before we start
   hanging up channels, and then again, after the channel hangup timeout expires */
void opbx_cdr_engine_term(void)
{
	opbx_cdr_submit_batch(batchsafeshutdown);
}

void opbx_cdr_engine_reload(void)
{
	do_reload();
}

