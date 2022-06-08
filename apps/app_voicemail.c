/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * 12-16-2004 : Support for Greek added by InAccess Networks (work funded by HOL, www.hol.gr)
 *				 George Konstantoulakis <gkon@inaccessnetworks.com>
 *
 * 05-10-2005 : Support for Swedish and Norwegian added by Daniel Nylander, http://www.danielnylander.se/
 *
 * 05-11-2005 : An option for maximum number of messsages per mailbox added by GDS Partners (www.gdspartners.com)
 *				 Stojan Sljivic <stojan.sljivic@gdspartners.com>
 *
 * 07-11-2005 : An issue with voicemail synchronization has been fixed by GDS Partners (www.gdspartners.com)
 *				 Stojan Sljivic <stojan.sljivic@gdspartners.com>
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
 * \brief Comedian Mail - Voicemail System
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <time.h>
#include <dirent.h>
#include <spandsp.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/lock.h"
#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/options.h"
#include "openpbx/config.h"
#include "openpbx/say.h"
#include "openpbx/module.h"
#include "openpbx/adsi.h"
#include "openpbx/app.h"
#include "openpbx/manager.h"
#include "openpbx/dsp.h"
#include "openpbx/localtime.h"
#include "openpbx/cli.h"
#include "openpbx/utils.h"
#include "openpbx/phone_no_utils.h"

#ifdef USE_ODBC_STORAGE
#include "openpbx/res_odbc.h"
#endif

#define COMMAND_TIMEOUT 5000

#define VOICEMAIL_CONFIG "voicemail.conf"
#define OPENPBX_USERNAME "openpbx"

/* Default mail command to mail voicemail. Change it with the
    mailcmd= command in voicemail.conf */
#define SENDMAIL "/usr/sbin/sendmail -t"

#define INTRO "vm-intro"

#define MAXMSG 100
#define MAXMSGLIMIT 9999

#define BASEMAXINLINE 256
#define BASELINELEN 72
#define BASEMAXINLINE 256
#define eol "\r\n"

#define MAX_DATETIME_FORMAT	512
#define MAX_NUM_CID_CONTEXTS 10

#define VM_REVIEW		(1 << 0)
#define VM_OPERATOR		(1 << 1)
#define VM_SAYCID		(1 << 2)
#define VM_SVMAIL		(1 << 3)
#define VM_ENVELOPE		(1 << 4)
#define VM_SAYDURATION		(1 << 5)
#define VM_SKIPAFTERCMD 	(1 << 6)
#define VM_FORCENAME		(1 << 7)	/* Have new users record their name */
#define VM_FORCEGREET		(1 << 8)	/* Have new users record their greetings */
#define VM_PBXSKIP		(1 << 9)
#define VM_DIRECFORWARD 	(1 << 10)	/* directory_forward */
#define VM_ATTACH		(1 << 11)
#define VM_DELETE		(1 << 12)
#define VM_ALLOCED		(1 << 13)

#define ERROR_LOCK_PATH		-100

#define OPT_SILENT 		(1 << 0)
#define OPT_BUSY_GREETING	(1 << 1)
#define OPT_UNAVAIL_GREETING	(1 << 2)
#define OPT_RECORDGAIN		(1 << 3)
#define OPT_PREPEND_MAILBOX	(1 << 4)

#define OPT_ARG_RECORDGAIN	0
#define OPT_ARG_ARRAY_SIZE	1

OPBX_DECLARE_OPTIONS(vm_app_options, {
	['s'] = { .flag = OPT_SILENT },
	['b'] = { .flag = OPT_BUSY_GREETING },
	['u'] = { .flag = OPT_UNAVAIL_GREETING },
	['g'] = { .flag = OPT_RECORDGAIN, .arg_index = OPT_ARG_RECORDGAIN + 1},
	['p'] = { .flag = OPT_PREPEND_MAILBOX },
});

static int load_config(void);

/* Syntaxes supported, not really language codes.
	en - English
	de - German
	es - Spanish
	fr - French
	it = Italian
	nl - Dutch
	pt - Portuguese
	gr - Greek
	no - Norwegian
	se - Swedish

German requires the following additional soundfile:
1F	einE (feminine)

Spanish requires the following additional soundfile:
1M      un (masculine)

Dutch, Portuguese & Spanish require the following additional soundfiles:
vm-INBOXs	singular of 'new'
vm-Olds		singular of 'old/heard/read'

NB these are plural:
vm-INBOX	nieuwe (nl)
vm-Old		oude (nl)

Swedish uses:
vm-nytt		singular of 'new'
vm-nya		plural of 'new'
vm-gammalt	singular of 'old'
vm-gamla	plural of 'old'
digits/ett	'one', not always same as 'digits/1'

Norwegian uses:
vm-ny		singular of 'new'
vm-nye		plural of 'new'
vm-gammel	singular of 'old'
vm-gamle	plural of 'old'

Dutch also uses:
nl-om		'at'?

Spanish also uses:
vm-youhaveno

Italian requires the following additional soundfile:

For vm_intro_it:
vm-nuovo	new
vm-nuovi	new plural
vm-vecchio	old
vm-vecchi	old plural
Don't use vm-INBOX or vm-Old, because they are the name of the INBOX and Old folders,
spelled among others when you have to change folder. For the above reasons, vm-INBOX
and vm-Old are spelled plural, to make them sound more as folder name than an adjective.

*/

struct baseio {
	int iocp;
	int iolen;
	int linelength;
	int ateof;
	unsigned char iobuf[BASEMAXINLINE];
};

/* Structure for linked list of users */
struct opbx_vm_user {
	char context[OPBX_MAX_CONTEXT];	/* Voicemail context */
	char mailbox[OPBX_MAX_EXTENSION];/* Mailbox id, unique within vm context */
	char password[80];		/* Secret pin code, numbers only */
	char fullname[80];		/* Full name, for directory app */
	char email[80];			/* E-mail address */
	char pager[80];			/* E-mail address to pager (no attachment) */
	char serveremail[80];		/* From: Mail address */
	char mailcmd[160];		/* Configurable mail command */
	char language[MAX_LANGUAGE];    /* Config: Language setting */
	char zonetag[80];		/* Time zone */
	char callback[80];
	char dialout[80];
	char uniqueid[20];		/* Unique integer identifier */
	char exit[80];
	unsigned int flags;		/* VM_ flags */	
	int saydurationm;
	int maxmsg;			/* Maximum number of msgs per folder for this mailbox */
	struct opbx_vm_user *next;
};

struct vm_zone {
	char name[80];
	char timezone[80];
	char msg_format[512];
	struct vm_zone *next;
};

struct vm_state {
	char curbox[80];
	char username[80];
	char curdir[256];
	char vmbox[256];
	char fn[256];
	char fn2[256];
	int *deleted;
	int *heard;
	int curmsg;
	int lastmsg;
	int newmessages;
	int oldmessages;
	int starting;
	int repeats;
};
static int advanced_options(struct opbx_channel *chan, struct opbx_vm_user *vmu, struct vm_state *vms, int msg,
			    int option, signed char record_gain);
static int dialout(struct opbx_channel *chan, struct opbx_vm_user *vmu, char *num, char *outgoing_context);
static int play_record_review(struct opbx_channel *chan, char *playfile, char *recordfile, int maxtime,
			      char *fmt, int outsidecaller, struct opbx_vm_user *vmu, int *duration, const char *unlockdir,
			      signed char record_gain);
static int vm_tempgreeting(struct opbx_channel *chan, struct opbx_vm_user *vmu, struct vm_state *vms, char *fmtc, signed char record_gain);
static int vm_play_folder_name(struct opbx_channel *chan, char *mbox);

static void apply_options(struct opbx_vm_user *vmu, const char *options);

#ifdef USE_ODBC_STORAGE
static char odbc_database[80];
static char odbc_table[80];
#define RETRIEVE(a,b) retrieve_file(a,b)
#define DISPOSE(a,b) remove_file(a,b)
#define STORE(a,b,c,d) store_file(a,b,c,d)
#define EXISTS(a,b,c,d) (message_exists(a,b))
#define RENAME(a,b,c,d,e,f,g,h) (rename_file(a,b,c,d,e,f))
#define COPY(a,b,c,d,e,f,g,h) (copy_file(a,b,c,d,e,f))
#define DELETE(a,b,c) (delete_file(a,b))
#else
#define RETRIEVE(a,b)
#define DISPOSE(a,b)
#define STORE(a,b,c,d)
#define EXISTS(a,b,c,d) (opbx_fileexists(c,NULL,d) > 0)
#define RENAME(a,b,c,d,e,f,g,h) (rename_file(g,h));
#define COPY(a,b,c,d,e,f,g,h) (copy_file(g,h));
#define DELETE(a,b,c) (vm_delete(c))
#endif

static char VM_SPOOL_DIR[OPBX_CONFIG_MAX_PATH];

static char ext_pass_cmd[128];

static char *tdesc = "Comedian Mail (Voicemail System)";

static char *addesc = "Comedian Mail";

static char *synopsis_vm =
"Leave a voicemail message";

static char *descrip_vm =
"  VoiceMail(mailbox[@context][&mailbox[@context]][...][|options]):  Leaves"
"voicemail for a given mailbox (must be configured in voicemail.conf).\n"
" If the options contain: \n"
"* 's'    instructions for leaving the message will be skipped.\n"
"* 'u'    the \"unavailable\" greeting will be played.\n"
"* 'b'    the \"busy\" greeting will be played.\n"
"* 'g(#)' the specified amount of gain will be requested during message\n"
"         recording (units are whole-number decibels (dB))\n"
"If the caller presses '0' (zero) during the prompt, the call jumps to\n"
"extension 'o' in the current context.\n"
"If the caller presses '*' during the prompt, the call jumps to\n"
"extension 'a' in the current context.\n"
"If the requested mailbox does not exist, and there exists a priority\n"
"n + 101, then that priority will be taken next.\n"
"If an error occur in the voicemail application resulting in that the message cannot be left,\n" 
"and there exists a priority n + 101, then that priority will be taken next.\n"
"When multiple mailboxes are specified, the unavailable or busy message\n"
"will be taken from the first mailbox specified.\n"
"Returns -1 on error or mailbox not found, or if the user hangs up.\n"
"Otherwise, it returns 0.\n";

static char *synopsis_vmain =
"Enter voicemail system";

static char *descrip_vmain =
"  VoiceMailMain([mailbox][@context][|options]): Enters the main voicemail system\n"
"for the checking of voicemail. The mailbox can be passed in,\n"
"which will stop the voicemail system from prompting the user for the mailbox.\n"
"If the options contain: \n"
"* 's'    the password check will be skipped.\n"
"* 'p'    the supplied mailbox is prepended to the user's entry and\n"
"         the resulting string is used as the mailbox number. This can\n"
"         be useful for virtual hosting of voicemail boxes.\n"
"* 'g(#)' the specified amount of gain will be requested during message\n"
"         recording (units are whole-number decibels (dB))\n"
"If a context is specified, mailboxes are considered in that voicemail context only.\n"
"Returns -1 if the user hangs up or 0 otherwise.\n";

static char *synopsis_vm_box_exists =
"Check if vmbox exists";

static char *descrip_vm_box_exists =
"  MailboxExists(mailbox[@context]): Conditionally branches to priority n+101\n"
"if the specified voice mailbox exists.\n";

static char *synopsis_vmauthenticate =
"Authenticate off voicemail passwords";

static char *descrip_vmauthenticate =
"  VMAuthenticate([mailbox][@context][|options]): Behaves identically to\n"
"the Authenticate application, with the exception that the passwords are\n"
"taken from voicemail.conf.\n"
"  If the mailbox is specified, only that mailbox's password will be considered\n"
"valid. If the mailbox is not specified, the channel variable AUTH_MAILBOX will\n"
"be set with the authenticated mailbox.\n"
"If the options contain 's' then no initial prompts will be played.\n";

/* Leave a message */
static char *app = "VoiceMail";

/* Check mail, control, etc */
static char *app2 = "VoiceMailMain";

static char *app3 = "MailboxExists";
static char *app4 = "VMAuthenticate";

OPBX_MUTEX_DEFINE_STATIC(vmlock);
struct opbx_vm_user *users;
struct opbx_vm_user *usersl;
struct vm_zone *zones = NULL;
struct vm_zone *zonesl = NULL;
static int maxsilence;
static int maxmsg;
static int silencethreshold = 128;
static char serveremail[80];
static char mailcmd[160];	/* Configurable mail cmd */
static char externnotify[160]; 

static char vmfmts[80];
static int vmminmessage;
static int vmmaxmessage;
static int maxgreet;
static int skipms;
static int maxlogins;

static struct opbx_flags globalflags = {0};

static int saydurationminfo;

static char dialcontext[OPBX_MAX_CONTEXT];
static char callcontext[OPBX_MAX_CONTEXT];
static char exitcontext[OPBX_MAX_CONTEXT];

static char cidinternalcontexts[MAX_NUM_CID_CONTEXTS][64];


static char *emailbody = NULL;
static char *emailsubject = NULL;
static char *pagerbody = NULL;
static char *pagersubject = NULL;
static char fromstring[100];
static char pagerfromstring[100];
static char emailtitle[100];
static char charset[32] = "ISO-8859-1";

static unsigned char adsifdn[4] = "\x00\x00\x00\x0F";
static unsigned char adsisec[4] = "\x9B\xDB\xF7\xAC";
static int adsiver = 1;
static char emaildateformat[32] = "%A, %B %d, %Y at %r";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static void populate_defaults(struct opbx_vm_user *vmu)
{
	opbx_copy_flags(vmu, (&globalflags), OPBX_FLAGS_ALL);	
	if (saydurationminfo)
		vmu->saydurationm = saydurationminfo;
	if (callcontext)
		opbx_copy_string(vmu->callback, callcontext, sizeof(vmu->callback));
	if (dialcontext)
		opbx_copy_string(vmu->dialout, dialcontext, sizeof(vmu->dialout));
	if (exitcontext)
		opbx_copy_string(vmu->exit, exitcontext, sizeof(vmu->exit));
	if (maxmsg)
		vmu->maxmsg = maxmsg;
}

static void apply_option(struct opbx_vm_user *vmu, const char *var, const char *value)
{
	int x;
	if (!strcasecmp(var, "attach")) {
		opbx_set2_flag(vmu, opbx_true(value), VM_ATTACH);	
	} else if (!strcasecmp(var, "serveremail")) {
		opbx_copy_string(vmu->serveremail, value, sizeof(vmu->serveremail));
	} else if (!strcasecmp(var, "language")) {
		opbx_copy_string(vmu->language, value, sizeof(vmu->language));
	} else if (!strcasecmp(var, "tz")) {
		opbx_copy_string(vmu->zonetag, value, sizeof(vmu->zonetag));
	} else if (!strcasecmp(var, "delete")) {
		opbx_set2_flag(vmu, opbx_true(value), VM_DELETE);	
	} else if (!strcasecmp(var, "saycid")){
		opbx_set2_flag(vmu, opbx_true(value), VM_SAYCID);	
	} else if (!strcasecmp(var,"sendvoicemail")){
		opbx_set2_flag(vmu, opbx_true(value), VM_SVMAIL);	
	} else if (!strcasecmp(var, "review")){
		opbx_set2_flag(vmu, opbx_true(value), VM_REVIEW);	
	} else if (!strcasecmp(var, "operator")){
		opbx_set2_flag(vmu, opbx_true(value), VM_OPERATOR);	
	} else if (!strcasecmp(var, "envelope")){
		opbx_set2_flag(vmu, opbx_true(value), VM_ENVELOPE);	
	} else if (!strcasecmp(var, "sayduration")){
		opbx_set2_flag(vmu, opbx_true(value), VM_SAYDURATION);	
	} else if (!strcasecmp(var, "saydurationm")){
		if (sscanf(value, "%d", &x) == 1) {
			vmu->saydurationm = x;
		} else {
			opbx_log(LOG_WARNING, "Invalid min duration for say duration\n");
		}
	} else if (!strcasecmp(var, "forcename")){
		opbx_set2_flag(vmu, opbx_true(value), VM_FORCENAME);	
	} else if (!strcasecmp(var, "forcegreetings")){
		opbx_set2_flag(vmu, opbx_true(value), VM_FORCEGREET);	
	} else if (!strcasecmp(var, "callback")) {
		opbx_copy_string(vmu->callback, value, sizeof(vmu->callback));
	} else if (!strcasecmp(var, "dialout")) {
		opbx_copy_string(vmu->dialout, value, sizeof(vmu->dialout));
	} else if (!strcasecmp(var, "exitcontext")) {
		opbx_copy_string(vmu->exit, value, sizeof(vmu->exit));
	} else if (!strcasecmp(var, "maxmsg")) {
		vmu->maxmsg = atoi(value);
 		if (vmu->maxmsg <= 0) {
			opbx_log(LOG_WARNING, "Invalid number of messages per folder maxmsg=%s. Using default value %i\n", value, MAXMSG);
			vmu->maxmsg = MAXMSG;
		} else if (vmu->maxmsg > MAXMSGLIMIT) {
			opbx_log(LOG_WARNING, "Maximum number of messages per folder is %i. Cannot accept value maxmsg=%s\n", MAXMSGLIMIT, value);
			vmu->maxmsg = MAXMSGLIMIT;
		}
	} else if (!strcasecmp(var, "options")) {
		apply_options(vmu, value);
	}
}

static int change_password_realtime(struct opbx_vm_user *vmu, const char *password)
{
	int res;
	if (!opbx_strlen_zero(vmu->uniqueid)) {
		res = opbx_update_realtime("voicemail", "uniqueid", vmu->uniqueid, "password", password, NULL);
		if (res > 0) {
			opbx_copy_string(vmu->password, password, sizeof(vmu->password));
			res = 0;
		} else if (!res) {
			res = -1;
		}
		return res;
	}
	return -1;
}

static void apply_options(struct opbx_vm_user *vmu, const char *options)
{	/* Destructively Parse options and apply */
	char *stringp;
	char *s;
	char *var, *value;
	stringp = opbx_strdupa(options);
	while ((s = strsep(&stringp, "|"))) {
		value = s;
		if ((var = strsep(&value, "=")) && value) {
			apply_option(vmu, var, value);
		}
	}	
}

static struct opbx_vm_user *find_user_realtime(struct opbx_vm_user *ivm, const char *context, const char *mailbox)
{
	struct opbx_variable *var, *tmp;
	struct opbx_vm_user *retval;

	if (ivm)
		retval=ivm;
	else
		retval=malloc(sizeof(struct opbx_vm_user));

	if (retval) {
		memset(retval, 0, sizeof(struct opbx_vm_user));
		if (!ivm)
			opbx_set_flag(retval, VM_ALLOCED);	
		if (mailbox) 
			opbx_copy_string(retval->mailbox, mailbox, sizeof(retval->mailbox));
		if (context) 
			opbx_copy_string(retval->context, context, sizeof(retval->context));
		else
			strcpy(retval->context, "default");
		populate_defaults(retval);
		var = opbx_load_realtime("voicemail", "mailbox", mailbox, "context", retval->context, NULL);
		if (var) {
			tmp = var;
			while(tmp) {
				printf("%s => %s\n", tmp->name, tmp->value);
				if (!strcasecmp(tmp->name, "password")) {
					opbx_copy_string(retval->password, tmp->value, sizeof(retval->password));
				} else if (!strcasecmp(tmp->name, "uniqueid")) {
					opbx_copy_string(retval->uniqueid, tmp->value, sizeof(retval->uniqueid));
				} else if (!strcasecmp(tmp->name, "pager")) {
					opbx_copy_string(retval->pager, tmp->value, sizeof(retval->pager));
				} else if (!strcasecmp(tmp->name, "email")) {
					opbx_copy_string(retval->email, tmp->value, sizeof(retval->email));
				} else if (!strcasecmp(tmp->name, "fullname")) {
					opbx_copy_string(retval->fullname, tmp->value, sizeof(retval->fullname));
				} else
					apply_option(retval, tmp->name, tmp->value);
				tmp = tmp->next;
			} 
		} else { 
			if (!ivm) 
				free(retval);
			retval = NULL;
		}	
	} 
	return retval;
}

static struct opbx_vm_user *find_user(struct opbx_vm_user *ivm, const char *context, const char *mailbox)
{
	/* This function could be made to generate one from a database, too */
	struct opbx_vm_user *vmu=NULL, *cur;
	opbx_mutex_lock(&vmlock);
	cur = users;
	while (cur) {
		if ((!context || !strcasecmp(context, cur->context)) &&
			(!strcasecmp(mailbox, cur->mailbox)))
				break;
		cur=cur->next;
	}
	if (cur) {
		if (ivm)
			vmu = ivm;
		else
			/* Make a copy, so that on a reload, we have no race */
			vmu = malloc(sizeof(struct opbx_vm_user));
		if (vmu) {
			memcpy(vmu, cur, sizeof(struct opbx_vm_user));
			opbx_set2_flag(vmu, !ivm, VM_ALLOCED);	
			vmu->next = NULL;
		}
	} else
		vmu = find_user_realtime(ivm, context, mailbox);
	opbx_mutex_unlock(&vmlock);
	return vmu;
}

static int reset_user_pw(const char *context, const char *mailbox, const char *newpass)
{
	/* This function could be made to generate one from a database, too */
	struct opbx_vm_user *cur;
	int res = -1;
	opbx_mutex_lock(&vmlock);
	cur = users;
	while (cur) {
		if ((!context || !strcasecmp(context, cur->context)) &&
			(!strcasecmp(mailbox, cur->mailbox)))
				break;
		cur=cur->next;
	}
	if (cur) {
		opbx_copy_string(cur->password, newpass, sizeof(cur->password));
		res = 0;
	}
	opbx_mutex_unlock(&vmlock);
	return res;
}

static void vm_change_password(struct opbx_vm_user *vmu, const char *newpassword)
{
	/*  There's probably a better way of doing this. */
	/*  That's why I've put the password change in a separate function. */
	/*  This could also be done with a database function */
	
	FILE *configin;
	FILE *configout;
	int linenum=0;
	char inbuf[256];
	char orig[256];
	char currcontext[256] ="";
	char tmpin[OPBX_CONFIG_MAX_PATH];
	char tmpout[OPBX_CONFIG_MAX_PATH];
	char *user, *pass, *rest, *trim, *tempcontext;
	struct stat statbuf;

	if (!change_password_realtime(vmu, newpassword))
		return;

	tempcontext = NULL;
	snprintf(tmpin, sizeof(tmpin), "%s/voicemail.conf", opbx_config_OPBX_CONFIG_DIR);
	snprintf(tmpout, sizeof(tmpout), "%s/voicemail.conf.new", opbx_config_OPBX_CONFIG_DIR);
	configin = fopen(tmpin,"r");
	if (configin)
		configout = fopen(tmpout,"w+");
	else
		configout = NULL;
	if (!configin || !configout) {
		if (configin)
			fclose(configin);
		else
			opbx_log(LOG_WARNING, "Warning: Unable to open '%s' for reading: %s\n", tmpin, strerror(errno));
		if (configout)
			fclose(configout);
		else
			opbx_log(LOG_WARNING, "Warning: Unable to open '%s' for writing: %s\n", tmpout, strerror(errno));
			return;
	}

	while (!feof(configin)) {
		/* Read in the line */
		fgets(inbuf, sizeof(inbuf), configin);
		linenum++;
		if (!feof(configin)) {
			/* Make a backup of it */
			memcpy(orig, inbuf, sizeof(orig));
			/* Strip trailing \n and comment */
			inbuf[strlen(inbuf) - 1] = '\0';
			user = strchr(inbuf, ';');
			if (user)
				*user = '\0';
			user=inbuf;
			while (*user < 33)
				user++;
			/* check for '[' (opening of context name ) */
			tempcontext = strchr(user, '[');
			if (tempcontext) {
				opbx_copy_string(currcontext, tempcontext +1, sizeof(currcontext));
				/* now check for ']' */
				tempcontext = strchr(currcontext, ']');
				if (tempcontext) 
					*tempcontext = '\0';
				else
					currcontext[0] = '\0';
			}
			pass = strchr(user, '=');
			if (pass > user) {
				trim = pass - 1;
				while (*trim && *trim < 33) {
					*trim = '\0';
					trim--;
				}
			}
			if (pass) {
				*pass = '\0';
				pass++;
				if (*pass == '>')
					pass++;
				while (*pass && *pass < 33)
					pass++;
			}
			if (pass) {
				rest = strchr(pass,',');
				if (rest) {
					*rest = '\0';
					rest++;
				}
			} else
				rest = NULL;

			/* Compare user, pass AND context */
			if (user && *user && !strcmp(user, vmu->mailbox) &&
				 pass && !strcmp(pass, vmu->password) &&
				 currcontext && *currcontext && !strcmp(currcontext, vmu->context)) {
				/* This is the line */
				if (rest) {
					fprintf(configout, "%s => %s,%s\n", vmu->mailbox,newpassword,rest);
				} else {
					fprintf(configout, "%s => %s\n", vmu->mailbox,newpassword);
				}
			} else {
				/* Put it back like it was */
				fprintf(configout, "%s", orig);
			}
		}
	}
	fclose(configin);
	fclose(configout);

	stat((char *)tmpin, &statbuf);
	chmod((char *)tmpout, statbuf.st_mode);
	chown((char *)tmpout, statbuf.st_uid, statbuf.st_gid);
	unlink((char *)tmpin);
	rename((char *)tmpout,(char *)tmpin);
	reset_user_pw(vmu->context, vmu->mailbox, newpassword);
	opbx_copy_string(vmu->password, newpassword, sizeof(vmu->password));
}

static void vm_change_password_shell(struct opbx_vm_user *vmu, char *newpassword)
{
	char buf[255];
	snprintf(buf,255,"%s %s %s %s",ext_pass_cmd,vmu->context,vmu->mailbox,newpassword);
	if (!opbx_safe_system(buf))
		opbx_copy_string(vmu->password, newpassword, sizeof(vmu->password));
}

static int make_dir(char *dest, int len, char *context, char *ext, char *mailbox)
{
	return snprintf(dest, len, "%s%s/%s/%s", VM_SPOOL_DIR, context, ext, mailbox);
}

static int make_file(char *dest, int len, char *dir, int num)
{
	return snprintf(dest, len, "%s/msg%04d", dir, num);
}

/* only return failure if opbx_lock_path returns 'timeout',
   not if the path does not exist or any other reason
*/
static int vm_lock_path(const char *path)
{
	switch (opbx_lock_path(path)) {
	case OPBX_LOCK_TIMEOUT:
		return -1;
	default:
		return 0;
	}
}


#ifdef USE_ODBC_STORAGE
static int retrieve_file(char *dir, int msgnum)
{
	int x = 0;
	int res;
	int fd=-1;
	size_t fdlen = 0;
	void *fdm=NULL;
	SQLSMALLINT colcount=0;
	SQLHSTMT stmt;
	char sql[256];
	char fmt[80]="";
	char *c;
	char coltitle[256];
	SQLSMALLINT collen;
	SQLSMALLINT datatype;
	SQLSMALLINT decimaldigits;
	SQLSMALLINT nullable;
	SQLULEN colsize;
	FILE *f=NULL;
	char rowdata[80];
	char fn[256];
	char full_fn[256];
	char msgnums[80];
	
	odbc_obj *obj;
	obj = fetch_odbc_obj(odbc_database, 0);
	if (obj) {
		opbx_copy_string(fmt, vmfmts, sizeof(fmt));
		c = strchr(fmt, '|');
		if (c)
			*c = '\0';
		if (!strcasecmp(fmt, "wav49"))
			strcpy(fmt, "WAV");
		snprintf(msgnums, sizeof(msgnums),"%d", msgnum);
		if (msgnum > -1)
			make_file(fn, sizeof(fn), dir, msgnum);
		else
			opbx_copy_string(fn, dir, sizeof(fn));
		snprintf(full_fn, sizeof(full_fn), "%s.txt", fn);
		f = fopen(full_fn, "w+");
		snprintf(full_fn, sizeof(full_fn), "%s.%s", fn, fmt);
		res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
			goto yuck;
		}
		snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE dir=? AND msgnum=?",odbc_table);
		res = SQLPrepare(stmt, sql, SQL_NTS);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Prepare failed![%s]\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(dir), 0, (void *)dir, 0, NULL);
		SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(msgnums), 0, (void *)msgnums, 0, NULL);
		res = odbc_smart_execute(obj, stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if (res == SQL_NO_DATA) {
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		else if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		fd = open(full_fn, O_RDWR | O_CREAT | O_TRUNC);
		if (fd < 0) {
			opbx_log(LOG_WARNING, "Failed to write '%s': %s\n", full_fn, strerror(errno));
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		res = SQLNumResultCols(stmt, &colcount);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {	
			opbx_log(LOG_WARNING, "SQL Column Count error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		if (f) 
			fprintf(f, "[message]\n");
		for (x=0;x<colcount;x++) {
			rowdata[0] = '\0';
			collen = sizeof(coltitle);
			res = SQLDescribeCol(stmt, x + 1, coltitle, sizeof(coltitle), &collen, 
						&datatype, &colsize, &decimaldigits, &nullable);
			if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				opbx_log(LOG_WARNING, "SQL Describe Column error!\n[%s]\n\n", sql);
				SQLFreeHandle (SQL_HANDLE_STMT, stmt);
				goto yuck;
			}
			if (!strcasecmp(coltitle, "recording")) {
				res = SQLGetData(stmt, x + 1, SQL_BINARY, NULL, 0, &colsize);
				fdlen = colsize;
				fd = open(full_fn, O_RDWR | O_TRUNC | O_CREAT, 0770);
				if (fd > -1) {
					char tmp[1]="";
					lseek(fd, fdlen - 1, SEEK_SET);
					if (write(fd, tmp, 1) != 1) {
						close(fd);
						fd = -1;
					}
					if (fd > -1)
						fdm = mmap(NULL, fdlen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
				}
				if (fdm) {
					memset(fdm, 0, fdlen);
					res = SQLGetData(stmt, x + 1, SQL_BINARY, fdm, fdlen, &colsize);
					if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
						opbx_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
						SQLFreeHandle (SQL_HANDLE_STMT, stmt);
						goto yuck;
					}
				}
			} else {
				res = SQLGetData(stmt, x + 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
				if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
					opbx_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
					SQLFreeHandle (SQL_HANDLE_STMT, stmt);
					goto yuck;
				}
				if (strcasecmp(coltitle, "msgnum") && strcasecmp(coltitle, "dir") && f)
					fprintf(f, "%s=%s\n", coltitle, rowdata);
			}
		}
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
	} else
		opbx_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
yuck:	
	if (f)
		fclose(f);
	if (fdm)
		munmap(fdm, fdlen);
	if (fd > -1)
		close(fd);
	return x - 1;
}

static int remove_file(char *dir, int msgnum)
{
	char fn[256];
	char full_fn[256];
	char msgnums[80];
	
	if (msgnum > -1) {
		snprintf(msgnums, sizeof(msgnums), "%d", msgnum);
		make_file(fn, sizeof(fn), dir, msgnum);
	} else
		opbx_copy_string(fn, dir, sizeof(fn));
	opbx_filedelete(fn, NULL);	
	snprintf(full_fn, sizeof(full_fn), "%s.txt", fn);
	unlink(full_fn);
	return 0;
}

static int lopbx_message_index(struct opbx_vm_user *vmu, char *dir)
{
	int x = 0;
	int res;
	SQLHSTMT stmt;
	char sql[256];
	char rowdata[20];
	
	odbc_obj *obj;
	obj = fetch_odbc_obj(odbc_database, 0);
	if (obj) {
		res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
			goto yuck;
		}
		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir=?",odbc_table);
		res = SQLPrepare(stmt, sql, SQL_NTS);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Prepare failed![%s]\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(dir), 0, (void *)dir, 0, NULL);
		res = odbc_smart_execute(obj, stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		if (sscanf(rowdata, "%d", &x) != 1)
			opbx_log(LOG_WARNING, "Failed to read message count!\n");
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
	} else
		opbx_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
yuck:	
	return x - 1;
}

static int message_exists(char *dir, int msgnum)
{
	int x = 0;
	int res;
	SQLHSTMT stmt;
	char sql[256];
	char rowdata[20];
	char msgnums[20];
	
	odbc_obj *obj;
	obj = fetch_odbc_obj(odbc_database, 0);
	if (obj) {
		snprintf(msgnums, sizeof(msgnums), "%d", msgnum);
		res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
			goto yuck;
		}
		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir=? AND msgnum=?",odbc_table);
		res = SQLPrepare(stmt, sql, SQL_NTS);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Prepare failed![%s]\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(dir), 0, (void *)dir, 0, NULL);
		SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(msgnums), 0, (void *)msgnums, 0, NULL);
		res = odbc_smart_execute(obj, stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		if (sscanf(rowdata, "%d", &x) != 1)
			opbx_log(LOG_WARNING, "Failed to read message count!\n");
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
	} else
		opbx_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
yuck:	
	return x;
}

static int count_messages(struct opbx_vm_user *vmu, char *dir)
{
	return lopbx_message_index(vmu, dir) + 1;
}

static void delete_file(char *sdir, int smsg)
{
	int res;
	SQLHSTMT stmt;
	char sql[256];
	char msgnums[20];
	
	odbc_obj *obj;
	obj = fetch_odbc_obj(odbc_database, 0);
	if (obj) {
		snprintf(msgnums, sizeof(msgnums), "%d", smsg);
		res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
			goto yuck;
		}
		snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE dir=? AND msgnum=?",odbc_table);
		res = SQLPrepare(stmt, sql, SQL_NTS);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Prepare failed![%s]\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(sdir), 0, (void *)sdir, 0, NULL);
		SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(msgnums), 0, (void *)msgnums, 0, NULL);
		res = odbc_smart_execute(obj, stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
	} else
		opbx_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
yuck:
	return;	
}

static void copy_file(char *sdir, int smsg, char *ddir, int dmsg, char *dmailboxuser, char *dmailboxcontext)
{
	int res;
	SQLHSTMT stmt;
	char sql[512];
	char msgnums[20];
	char msgnumd[20];
	odbc_obj *obj;

	delete_file(ddir, dmsg);
	obj = fetch_odbc_obj(odbc_database, 0);
	if (obj) {
		snprintf(msgnums, sizeof(msgnums), "%d", smsg);
		snprintf(msgnumd, sizeof(msgnumd), "%d", dmsg);
		res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
			goto yuck;
		}
#ifdef EXTENDED_ODBC_STORAGE
		snprintf(sql, sizeof(sql), "INSERT INTO %s (dir, msgnum, context, macrocontext, callerid, origtime, duration, recording, mailboxuser, mailboxcontext) SELECT ?,?,context,macrocontext,callerid,origtime,duration,recording,?,? FROM %s WHERE dir=? AND msgnum=?",odbc_table,odbc_table); 
#else
 		snprintf(sql, sizeof(sql), "INSERT INTO %s (dir, msgnum, context, macrocontext, callerid, origtime, duration, recording) SELECT ?,?,context,macrocontext,callerid,origtime,duration,recording FROM %s WHERE dir=? AND msgnum=?",odbc_table,odbc_table); 
#endif
		res = SQLPrepare(stmt, sql, SQL_NTS);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Prepare failed![%s]\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(ddir), 0, (void *)ddir, 0, NULL);
		SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(msgnumd), 0, (void *)msgnumd, 0, NULL);
#ifdef EXTENDED_ODBC_STORAGE
		SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(dmailboxuser), 0, (void *)dmailboxuser, 0, NULL);
		SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(dmailboxcontext), 0, (void *)dmailboxcontext, 0, NULL);
		SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(sdir), 0, (void *)sdir, 0, NULL);
		SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(msgnums), 0, (void *)msgnums, 0, NULL);
#else
 		SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(sdir), 0, (void *)sdir, 0, NULL);
 		SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(msgnums), 0, (void *)msgnums, 0, NULL);
#endif		 
		res = odbc_smart_execute(obj, stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Execute error!\n[%s] (You probably don't have MySQL 4.1 or later installed)\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
	} else
		opbx_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
yuck:
	return;	
}

static int store_file(char *dir, char *mailboxuser, char *mailboxcontext, int msgnum)
{
	int x = 0;
	int res;
	int fd = -1;
	void *fdm=NULL;
	size_t fdlen = -1;
	SQLHSTMT stmt;
	SQLINTEGER len;
	char sql[256];
	char msgnums[20];
	char fn[256];
	char full_fn[256];
	char fmt[80]="";
	char *c;
	char *context="", *macrocontext="", *callerid="", *origtime="", *duration="";
	char *category = "";
	struct opbx_config *cfg=NULL;
	odbc_obj *obj;

	delete_file(dir, msgnum);
	obj = fetch_odbc_obj(odbc_database, 0);
	if (obj) {
		opbx_copy_string(fmt, vmfmts, sizeof(fmt));
		c = strchr(fmt, '|');
		if (c)
			*c = '\0';
		if (!strcasecmp(fmt, "wav49"))
			strcpy(fmt, "WAV");
		snprintf(msgnums, sizeof(msgnums),"%d", msgnum);
		if (msgnum > -1)
			make_file(fn, sizeof(fn), dir, msgnum);
		else
			opbx_copy_string(fn, dir, sizeof(fn));
		snprintf(full_fn, sizeof(full_fn), "%s.txt", fn);
		cfg = opbx_config_load(full_fn);
		snprintf(full_fn, sizeof(full_fn), "%s.%s", fn, fmt);
		fd = open(full_fn, O_RDWR);
		if (fd < 0) {
			opbx_log(LOG_WARNING, "Open of sound file '%s' failed: %s\n", full_fn, strerror(errno));
			goto yuck;
		}
		if (cfg) {
			context = opbx_variable_retrieve(cfg, "message", "context");
			if (!context) context = "";
			macrocontext = opbx_variable_retrieve(cfg, "message", "macrocontext");
			if (!macrocontext) macrocontext = "";
			callerid = opbx_variable_retrieve(cfg, "message", "callerid");
			if (!callerid) callerid = "";
			origtime = opbx_variable_retrieve(cfg, "message", "origtime");
			if (!origtime) origtime = "";
			duration = opbx_variable_retrieve(cfg, "message", "duration");
			if (!duration) duration = "";
			category = opbx_variable_retrieve(cfg, "message", "category");
			if (!category) category = "";
		}
		fdlen = lseek(fd, 0, SEEK_END);
		lseek(fd, 0, SEEK_SET);
		printf("Length is %d\n", fdlen);
		fdm = mmap(NULL, fdlen, PROT_READ | PROT_WRITE, MAP_SHARED,fd, 0);
		if (!fdm) {
			opbx_log(LOG_WARNING, "Memory map failed!\n");
			goto yuck;
		} 
		res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
			goto yuck;
		}
 		if (!opbx_strlen_zero(category)) 
#ifdef EXTENDED_ODBC_STORAGE
			snprintf(sql, sizeof(sql), "INSERT INTO %s (dir,msgnum,recording,context,macrocontext,callerid,origtime,duration,mailboxuser,mailboxcontext,category) VALUES (?,?,?,?,?,?,?,?,?,?,?)",odbc_table); 
#else
 			snprintf(sql, sizeof(sql), "INSERT INTO %s (dir,msgnum,recording,context,macrocontext,callerid,origtime,duration,category) VALUES (?,?,?,?,?,?,?,?,?)",odbc_table);
#endif
 		else
#ifdef EXTENDED_ODBC_STORAGE
			snprintf(sql, sizeof(sql), "INSERT INTO %s (dir,msgnum,recording,context,macrocontext,callerid,origtime,duration,mailboxuser,mailboxcontext) VALUES (?,?,?,?,?,?,?,?,?,?)",odbc_table);
#else
 			snprintf(sql, sizeof(sql), "INSERT INTO %s (dir,msgnum,recording,context,macrocontext,callerid,origtime,duration) VALUES (?,?,?,?,?,?,?,?)",odbc_table);
#endif
		res = SQLPrepare(stmt, sql, SQL_NTS);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Prepare failed![%s]\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		len = fdlen; /* SQL_LEN_DATA_AT_EXEC(fdlen); */
		SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(dir), 0, (void *)dir, 0, NULL);
		SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(msgnums), 0, (void *)msgnums, 0, NULL);
		SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_BINARY, SQL_BINARY, fdlen, 0, (void *)fdm, fdlen, &len);
		SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(context), 0, (void *)context, 0, NULL);
		SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(macrocontext), 0, (void *)macrocontext, 0, NULL);
		SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(callerid), 0, (void *)callerid, 0, NULL);
		SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(origtime), 0, (void *)origtime, 0, NULL);
		SQLBindParameter(stmt, 8, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(duration), 0, (void *)duration, 0, NULL);
#ifdef EXTENDED_ODBC_STORAGE
		SQLBindParameter(stmt, 9, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(mailboxuser), 0, (void *)mailboxuser, 0, NULL);
		SQLBindParameter(stmt, 10, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(mailboxcontext), 0, (void *)mailboxcontext, 0, NULL);
		if (!opbx_strlen_zero(category))
			SQLBindParameter(stmt, 11, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(category), 0, (void *)category, 0, NULL);
#else
 		if (!opbx_strlen_zero(category))
 			SQLBindParameter(stmt, 9, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(category), 0, (void *)category, 0, NULL);
#endif
		res = odbc_smart_execute(obj, stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
	} else
		opbx_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
yuck:	
	if (cfg)
		opbx_config_destroy(cfg);
	if (fdm)
		munmap(fdm, fdlen);
	if (fd > -1)
		close(fd);
	return x;
}

static void rename_file(char *sdir, int smsg, char *mailboxuser, char *mailboxcontext, char *ddir, int dmsg)
{
	int res;
	SQLHSTMT stmt;
	char sql[256];
	char msgnums[20];
	char msgnumd[20];
	odbc_obj *obj;

	delete_file(ddir, dmsg);
	obj = fetch_odbc_obj(odbc_database, 0);
	if (obj) {
		snprintf(msgnums, sizeof(msgnums), "%d", smsg);
		snprintf(msgnumd, sizeof(msgnumd), "%d", dmsg);
		res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
			goto yuck;
		}
#ifdef EXTENDED_ODBC_STORAGE
		snprintf(sql, sizeof(sql), "UPDATE %s SET dir=?, msgnum=?, mailboxuser=?, mailboxcontext=? WHERE dir=? AND msgnum=?",odbc_table);
#else
 		snprintf(sql, sizeof(sql), "UPDATE %s SET dir=?, msgnum=? WHERE dir=? AND msgnum=?",odbc_table);
#endif
		res = SQLPrepare(stmt, sql, SQL_NTS);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Prepare failed![%s]\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(ddir), 0, (void *)ddir, 0, NULL);
		SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(msgnumd), 0, (void *)msgnumd, 0, NULL);
#ifdef EXTENDED_ODBC_STORAGE
		SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(mailboxuser), 0, (void *)mailboxuser, 0, NULL);
		SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(mailboxcontext), 0, (void *)mailboxcontext, 0, NULL);
		SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(sdir), 0, (void *)sdir, 0, NULL);
		SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(msgnums), 0, (void *)msgnums, 0, NULL);
#else
 		SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(sdir), 0, (void *)sdir, 0, NULL);
 		SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(msgnums), 0, (void *)msgnums, 0, NULL);
#endif		 
		res = odbc_smart_execute(obj, stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
	} else
		opbx_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
yuck:
	return;	
}

#else

static int count_messages(struct opbx_vm_user *vmu, char *dir)
{
	/* Find all .txt files - even if they are not in sequence from 0000 */

	int vmcount = 0;
	DIR *vmdir = NULL;
	struct dirent *vment = NULL;

	if (vm_lock_path(dir))
		return ERROR_LOCK_PATH;

	if ((vmdir = opendir(dir))) {
		while ((vment = readdir(vmdir))) {
			if (strlen(vment->d_name) > 7 && !strncmp(vment->d_name + 7, ".txt", 4)) 
				vmcount++;
		}
		closedir(vmdir);
	}
	opbx_unlock_path(dir);
	
	return vmcount;
}

static void rename_file(char *sfn, char *dfn)
{
	char stxt[256];
	char dtxt[256];
	opbx_filerename(sfn,dfn,NULL);
	snprintf(stxt, sizeof(stxt), "%s.txt", sfn);
	snprintf(dtxt, sizeof(dtxt), "%s.txt", dfn);
	rename(stxt, dtxt);
}

static int copy(char *infile, char *outfile)
{
	int ifd;
	int ofd;
	int res;
	int len;
	char buf[4096];

#ifdef HARDLINK_WHEN_POSSIBLE
	/* Hard link if possible; saves disk space & is faster */
	if (link(infile, outfile)) {
#endif
		if ((ifd = open(infile, O_RDONLY)) < 0) {
			opbx_log(LOG_WARNING, "Unable to open %s in read-only mode\n", infile);
			return -1;
		}
		if ((ofd = open(outfile, O_WRONLY | O_TRUNC | O_CREAT, 0600)) < 0) {
			opbx_log(LOG_WARNING, "Unable to open %s in write-only mode\n", outfile);
			close(ifd);
			return -1;
		}
		do {
			len = read(ifd, buf, sizeof(buf));
			if (len < 0) {
				opbx_log(LOG_WARNING, "Read failed on %s: %s\n", infile, strerror(errno));
				close(ifd);
				close(ofd);
				unlink(outfile);
			}
			if (len) {
				res = write(ofd, buf, len);
				if (errno == ENOMEM || errno == ENOSPC || res != len) {
					opbx_log(LOG_WARNING, "Write failed on %s (%d of %d): %s\n", outfile, res, len, strerror(errno));
					close(ifd);
					close(ofd);
					unlink(outfile);
				}
			}
		} while (len);
		close(ifd);
		close(ofd);
		return 0;
#ifdef HARDLINK_WHEN_POSSIBLE
	} else {
		/* Hard link succeeded */
		return 0;
	}
#endif
}

static void copy_file(char *frompath, char *topath)
{
	char frompath2[256],topath2[256];
	opbx_filecopy(frompath, topath, NULL);
	snprintf(frompath2, sizeof(frompath2), "%s.txt", frompath);
	snprintf(topath2, sizeof(topath2), "%s.txt", topath);
	copy(frompath2, topath2);
}

/*
 * A negative return value indicates an error.
 */
static int lopbx_message_index(struct opbx_vm_user *vmu, char *dir)
{
	int x;
	char fn[256];

	if (vm_lock_path(dir))
		return ERROR_LOCK_PATH;

	for (x = 0; x < vmu->maxmsg; x++) {
		make_file(fn, sizeof(fn), dir, x);
		if (opbx_fileexists(fn, NULL, NULL) < 1)
			break;
	}
	opbx_unlock_path(dir);

	return x - 1;
}

static int vm_delete(char *file)
{
	char *txt;
	int txtsize = 0;

	txtsize = (strlen(file) + 5)*sizeof(char);
	txt = (char *)alloca(txtsize);
	/* Sprintf here would safe because we alloca'd exactly the right length,
	 * but trying to eliminate all sprintf's anyhow
	 */
	snprintf(txt, txtsize, "%s.txt", file);
	unlink(txt);
	return opbx_filedelete(file, NULL);
}


#endif
static int
inbuf(struct baseio *bio, FILE *fi)
{
	int l;

	if (bio->ateof)
		return 0;

	if ((l = fread(bio->iobuf,1,BASEMAXINLINE,fi)) <= 0) {
		if (ferror(fi))
			return -1;

		bio->ateof = 1;
		return 0;
	}

	bio->iolen= l;
	bio->iocp= 0;

	return 1;
}

static int 
inchar(struct baseio *bio, FILE *fi)
{
	if (bio->iocp>=bio->iolen) {
		if (!inbuf(bio, fi))
			return EOF;
	}

	return bio->iobuf[bio->iocp++];
}

static int
ochar(struct baseio *bio, int c, FILE *so)
{
	if (bio->linelength>=BASELINELEN) {
		if (fputs(eol,so)==EOF)
			return -1;

		bio->linelength= 0;
	}

	if (putc(((unsigned char)c),so)==EOF)
		return -1;

	bio->linelength++;

	return 1;
}

static int base_encode(char *filename, FILE *so)
{
	unsigned char dtable[BASEMAXINLINE];
	int i,hiteof= 0;
	FILE *fi;
	struct baseio bio;

	memset(&bio, 0, sizeof(bio));
	bio.iocp = BASEMAXINLINE;

	if (!(fi = fopen(filename, "rb"))) {
		opbx_log(LOG_WARNING, "Failed to open log file: %s: %s\n", filename, strerror(errno));
		return -1;
	}

	for (i= 0;i<9;i++) {
		dtable[i]= 'A'+i;
		dtable[i+9]= 'J'+i;
		dtable[26+i]= 'a'+i;
		dtable[26+i+9]= 'j'+i;
	}
	for (i= 0;i<8;i++) {
		dtable[i+18]= 'S'+i;
		dtable[26+i+18]= 's'+i;
	}
	for (i= 0;i<10;i++) {
		dtable[52+i]= '0'+i;
	}
	dtable[62]= '+';
	dtable[63]= '/';

	while (!hiteof){
		unsigned char igroup[3],ogroup[4];
		int c,n;

		igroup[0]= igroup[1]= igroup[2]= 0;

		for (n= 0;n<3;n++) {
			if ((c = inchar(&bio, fi)) == EOF) {
				hiteof= 1;
				break;
			}

			igroup[n]= (unsigned char)c;
		}

		if (n> 0) {
			ogroup[0]= dtable[igroup[0]>>2];
			ogroup[1]= dtable[((igroup[0]&3)<<4)|(igroup[1]>>4)];
			ogroup[2]= dtable[((igroup[1]&0xF)<<2)|(igroup[2]>>6)];
			ogroup[3]= dtable[igroup[2]&0x3F];

			if (n<3) {
				ogroup[3]= '=';

				if (n<2)
					ogroup[2]= '=';
			}

			for (i= 0;i<4;i++)
				ochar(&bio, ogroup[i], so);
		}
	}

	if (fputs(eol,so)==EOF)
		return 0;

	fclose(fi);

	return 1;
}

static void prep_email_sub_vars(struct opbx_channel *ast, struct opbx_vm_user *vmu, int msgnum, char *context, char *mailbox, char *cidnum, char *cidname, char *dur, char *date, char *passdata, size_t passdatasize)
{
	char callerid[256];
	/* Prepare variables for substition in email body and subject */
	pbx_builtin_setvar_helper(ast, "VM_NAME", vmu->fullname);
	pbx_builtin_setvar_helper(ast, "VM_DUR", dur);
	snprintf(passdata, passdatasize, "%d", msgnum);
	pbx_builtin_setvar_helper(ast, "VM_MSGNUM", passdata);
	pbx_builtin_setvar_helper(ast, "VM_CONTEXT", context);
	pbx_builtin_setvar_helper(ast, "VM_MAILBOX", mailbox);
	pbx_builtin_setvar_helper(ast, "VM_CALLERID", opbx_callerid_merge(callerid, sizeof(callerid), cidname, cidnum, "Unknown Caller"));
	pbx_builtin_setvar_helper(ast, "VM_CIDNAME", (cidname ? cidname : "an unknown caller"));
	pbx_builtin_setvar_helper(ast, "VM_CIDNUM", (cidnum ? cidnum : "an unknown caller"));
	pbx_builtin_setvar_helper(ast, "VM_DATE", date);
}

static int sendmail(char *srcemail, struct opbx_vm_user *vmu, int msgnum, char *context, char *mailbox, char *cidnum, char *cidname, char *attach, char *format, int duration, int attach_user_voicemail)
{
	FILE *p=NULL;
	int pfd;
	char date[256];
	char host[MAXHOSTNAMELEN] = "";
	char who[256];
	char bound[256];
	char fname[256];
	char dur[256];
	char tmp[80] = "/tmp/astmail-XXXXXX";
	char tmp2[256];
	time_t t;
	struct tm tm;
	struct vm_zone *the_zone = NULL;
	if (vmu && opbx_strlen_zero(vmu->email)) {
		opbx_log(LOG_WARNING, "E-mail address missing for mailbox [%s].  E-mail will not be sent.\n", vmu->mailbox);
		return(0);
	}
	if (!strcmp(format, "wav49"))
		format = "WAV";
	opbx_log(LOG_DEBUG, "Attaching file '%s', format '%s', uservm is '%d', global is %d\n", attach, format, attach_user_voicemail, opbx_test_flag((&globalflags), VM_ATTACH));
	/* Make a temporary file instead of piping directly to sendmail, in case the mail
	   command hangs */
	pfd = mkstemp(tmp);
	if (pfd > -1) {
		p = fdopen(pfd, "w");
		if (!p) {
			close(pfd);
			pfd = -1;
		}
	}
	if (p) {
		gethostname(host, sizeof(host)-1);
		if (strchr(srcemail, '@'))
			opbx_copy_string(who, srcemail, sizeof(who));
		else {
			snprintf(who, sizeof(who), "%s@%s", srcemail, host);
		}
		snprintf(dur, sizeof(dur), "%d:%02d", duration / 60, duration % 60);
		time(&t);

		/* Does this user have a timezone specified? */
		if (!opbx_strlen_zero(vmu->zonetag)) {
			/* Find the zone in the list */
			struct vm_zone *z;
			z = zones;
			while (z) {
				if (!strcmp(z->name, vmu->zonetag)) {
					the_zone = z;
					break;
				}
				z = z->next;
			}
		}

		if (the_zone)
			opbx_localtime(&t,&tm,the_zone->timezone);
		else
			opbx_localtime(&t,&tm,NULL);
		strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S %z", &tm);
		fprintf(p, "Date: %s\n", date);

		/* Set date format for voicemail mail */
		strftime(date, sizeof(date), emaildateformat, &tm);

		if (*fromstring) {
			struct opbx_channel *ast = opbx_channel_alloc(0);
			if (ast) {
				char *passdata;
				int vmlen = strlen(fromstring)*3 + 200;
				if ((passdata = alloca(vmlen))) {
					memset(passdata, 0, vmlen);
					prep_email_sub_vars(ast,vmu,msgnum + 1,context,mailbox,cidnum, cidname,dur,date,passdata, vmlen);
					pbx_substitute_variables_helper(ast,fromstring,passdata,vmlen);
					fprintf(p, "From: %s <%s>\n",passdata,who);
				} else opbx_log(LOG_WARNING, "Cannot allocate workspace for variable substitution\n");
				opbx_channel_free(ast);
			} else opbx_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
		} else
			fprintf(p, "From: OpenPBX <%s>\n", who);
		fprintf(p, "To: %s <%s>\n", vmu->fullname, vmu->email);

		if (emailsubject) {
			struct opbx_channel *ast = opbx_channel_alloc(0);
			if (ast) {
				char *passdata;
				int vmlen = strlen(emailsubject)*3 + 200;
				if ((passdata = alloca(vmlen))) {
					memset(passdata, 0, vmlen);
					prep_email_sub_vars(ast,vmu,msgnum + 1,context,mailbox,cidnum, cidname,dur,date,passdata, vmlen);
					pbx_substitute_variables_helper(ast,emailsubject,passdata,vmlen);
					fprintf(p, "Subject: %s\n",passdata);
				} else opbx_log(LOG_WARNING, "Cannot allocate workspace for variable substitution\n");
				opbx_channel_free(ast);
			} else opbx_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
		} else
		if (*emailtitle) {
			fprintf(p, emailtitle, msgnum + 1, mailbox) ;
			fprintf(p,"\n") ;
		} else if (opbx_test_flag((&globalflags), VM_PBXSKIP))
			fprintf(p, "Subject: New message %d in mailbox %s\n", msgnum + 1, mailbox);
		else
			fprintf(p, "Subject: [PBX]: New message %d in mailbox %s\n", msgnum + 1, mailbox);
		fprintf(p, "Message-ID: <OpenPBX-%d-%d-%s-%d@%s>\n", msgnum, (unsigned int)opbx_random(), mailbox, getpid(), host);
		fprintf(p, "MIME-Version: 1.0\n");
		if (attach_user_voicemail) {
			/* Something unique. */
			snprintf(bound, sizeof(bound), "voicemail_%d%s%d%d", msgnum, mailbox, getpid(), (unsigned int)opbx_random());

			fprintf(p, "Content-Type: multipart/mixed; boundary=\"%s\"\n\n\n", bound);

			fprintf(p, "--%s\n", bound);
		}
		fprintf(p, "Content-Type: text/plain; charset=%s\nContent-Transfer-Encoding: 8bit\n\n", charset);
		if (emailbody) {
			struct opbx_channel *ast = opbx_channel_alloc(0);
			if (ast) {
				char *passdata;
				int vmlen = strlen(emailbody)*3 + 200;
				if ((passdata = alloca(vmlen))) {
					memset(passdata, 0, vmlen);
					prep_email_sub_vars(ast,vmu,msgnum + 1,context,mailbox,cidnum, cidname,dur,date,passdata, vmlen);
					pbx_substitute_variables_helper(ast,emailbody,passdata,vmlen);
					fprintf(p, "%s\n",passdata);
				} else opbx_log(LOG_WARNING, "Cannot allocate workspace for variable substitution\n");
				opbx_channel_free(ast);
			} else opbx_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
		} else {
			fprintf(p, "Dear %s:\n\n\tJust wanted to let you know you were just left a %s long message (number %d)\n"

			"in mailbox %s from %s, on %s so you might\n"
			"want to check it when you get a chance.  Thanks!\n\n\t\t\t\t--OpenPBX\n\n", vmu->fullname, 
			dur, msgnum + 1, mailbox, (cidname ? cidname : (cidnum ? cidnum : "an unknown caller")), date);
		}
		if (attach_user_voicemail) {
			/* Eww. We want formats to tell us their own MIME type */
			char *ctype = "audio/x-";
			if (!strcasecmp(format, "ogg"))
				ctype = "application/";
		
			fprintf(p, "--%s\n", bound);
			fprintf(p, "Content-Type: %s%s; name=\"msg%04d.%s\"\n", ctype, format, msgnum, format);
			fprintf(p, "Content-Transfer-Encoding: base64\n");
			fprintf(p, "Content-Description: Voicemail sound attachment.\n");
			fprintf(p, "Content-Disposition: attachment; filename=\"msg%04d.%s\"\n\n", msgnum, format);

			snprintf(fname, sizeof(fname), "%s.%s", attach, format);
			base_encode(fname, p);
			fprintf(p, "\n\n--%s--\n.\n", bound);
		}
		fclose(p);
		snprintf(tmp2, sizeof(tmp2), "( %s < %s ; rm -f %s ) &", mailcmd, tmp, tmp);
		opbx_safe_system(tmp2);
		opbx_log(LOG_DEBUG, "Sent mail to %s with command '%s'\n", vmu->email, mailcmd);
	} else {
		opbx_log(LOG_WARNING, "Unable to launch '%s'\n", mailcmd);
		return -1;
	}
	return 0;
}

static int sendpage(char *srcemail, char *pager, int msgnum, char *context, char *mailbox, char *cidnum, char *cidname, int duration, struct opbx_vm_user *vmu)
{
	FILE *p=NULL;
	int pfd;
	char date[256];
	char host[MAXHOSTNAMELEN]="";
	char who[256];
	char dur[256];
	char tmp[80] = "/tmp/astmail-XXXXXX";
	char tmp2[256];
	time_t t;
	struct tm tm;
	struct vm_zone *the_zone = NULL;
	pfd = mkstemp(tmp);

	if (pfd > -1) {
		p = fdopen(pfd, "w");
		if (!p) {
			close(pfd);
			pfd = -1;
		}
	}

	if (p) {
		gethostname(host, sizeof(host)-1);
		if (strchr(srcemail, '@'))
			opbx_copy_string(who, srcemail, sizeof(who));
		else {
			snprintf(who, sizeof(who), "%s@%s", srcemail, host);
		}
		snprintf(dur, sizeof(dur), "%d:%02d", duration / 60, duration % 60);
		time(&t);

		/* Does this user have a timezone specified? */
		if (!opbx_strlen_zero(vmu->zonetag)) {
			/* Find the zone in the list */
			struct vm_zone *z;
			z = zones;
			while (z) {
				if (!strcmp(z->name, vmu->zonetag)) {
					the_zone = z;
					break;
				}
				z = z->next;
			}
		}

		if (the_zone)
			opbx_localtime(&t,&tm,the_zone->timezone);
		else
			opbx_localtime(&t,&tm,NULL);

		strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S %z", &tm);
		fprintf(p, "Date: %s\n", date);

		if (*pagerfromstring) {
			struct opbx_channel *ast = opbx_channel_alloc(0);
			if (ast) {
				char *passdata;
				int vmlen = strlen(fromstring)*3 + 200;
				if ((passdata = alloca(vmlen))) {
					memset(passdata, 0, vmlen);
					prep_email_sub_vars(ast,vmu,msgnum + 1,context,mailbox,cidnum, cidname,dur,date,passdata, vmlen);
					pbx_substitute_variables_helper(ast,pagerfromstring,passdata,vmlen);
					fprintf(p, "From: %s <%s>\n",passdata,who);
				} else 
					opbx_log(LOG_WARNING, "Cannot allocate workspace for variable substitution\n");
				opbx_channel_free(ast);
			} else opbx_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
		} else
			fprintf(p, "From: OpenPBX <%s>\n", who);
		fprintf(p, "To: %s\n", pager);
               if (pagersubject) {
                       struct opbx_channel *ast = opbx_channel_alloc(0);
                       if (ast) {
                               char *passdata;
                               int vmlen = strlen(pagersubject)*3 + 200;
                               if ((passdata = alloca(vmlen))) {
                                       memset(passdata, 0, vmlen);
                                       prep_email_sub_vars(ast,vmu,msgnum + 1,context,mailbox,cidnum, cidname,dur,date,passdata, vmlen);
                                       pbx_substitute_variables_helper(ast,pagersubject,passdata,vmlen);
                                       fprintf(p, "Subject: %s\n\n",passdata);
                               } else opbx_log(LOG_WARNING, "Cannot allocate workspace for variable substitution\n");
                               opbx_channel_free(ast);
                       } else opbx_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
               } else
                       fprintf(p, "Subject: New VM\n\n");
		strftime(date, sizeof(date), "%A, %B %d, %Y at %r", &tm);
               if (pagerbody) {
                       struct opbx_channel *ast = opbx_channel_alloc(0);
                       if (ast) {
                               char *passdata;
                               int vmlen = strlen(pagerbody)*3 + 200;
                               if ((passdata = alloca(vmlen))) {
                                       memset(passdata, 0, vmlen);
                                       prep_email_sub_vars(ast,vmu,msgnum + 1,context,mailbox,cidnum, cidname,dur,date,passdata, vmlen);
                                       pbx_substitute_variables_helper(ast,pagerbody,passdata,vmlen);
                                       fprintf(p, "%s\n",passdata);
                               } else opbx_log(LOG_WARNING, "Cannot allocate workspace for variable substitution\n");
                               opbx_channel_free(ast);
                       } else opbx_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
               } else {
                       fprintf(p, "New %s long msg in box %s\n"
                                       "from %s, on %s", dur, mailbox, (cidname ? cidname : (cidnum ? cidnum : "unknown")), date);
               }
		fclose(p);
		snprintf(tmp2, sizeof(tmp2), "( %s < %s ; rm -f %s ) &", mailcmd, tmp, tmp);
		opbx_safe_system(tmp2);
		opbx_log(LOG_DEBUG, "Sent page to %s with command '%s'\n", pager, mailcmd);
	} else {
		opbx_log(LOG_WARNING, "Unable to launch '%s'\n", mailcmd);
		return -1;
	}
	return 0;
}

static int get_date(char *s, int len)
{
	struct tm tm;
	time_t t;
	t = time(0);
	localtime_r(&t,&tm);
	return strftime(s, len, "%a %b %e %r %Z %Y", &tm);
}

static int invent_message(struct opbx_channel *chan, char *context, char *ext, int busy, char *ecodes)
{
	int res;
	char fn[256];
	snprintf(fn, sizeof(fn), "%s%s/%s/greet", VM_SPOOL_DIR, context, ext);
	RETRIEVE(fn, -1);
	if (opbx_fileexists(fn, NULL, NULL) > 0) {
		res = opbx_streamfile(chan, fn, chan->language);
		if (res) {
			DISPOSE(fn, -1);
			return -1;
		}
		res = opbx_waitstream(chan, ecodes);
		if (res) {
			DISPOSE(fn, -1);
			return res;
		}
	} else {
		/* Dispose just in case */
		DISPOSE(fn, -1);
		res = opbx_streamfile(chan, "vm-theperson", chan->language);
		if (res)
			return -1;
		res = opbx_waitstream(chan, ecodes);
		if (res)
			return res;
		res = opbx_say_digit_str(chan, ext, ecodes, chan->language);
		if (res)
			return res;
	}
	if (busy)
		res = opbx_streamfile(chan, "vm-isonphone", chan->language);
	else
		res = opbx_streamfile(chan, "vm-isunavail", chan->language);
	if (res)
		return -1;
	res = opbx_waitstream(chan, ecodes);
	return res;
}

static void free_user(struct opbx_vm_user *vmu)
{
	if (opbx_test_flag(vmu, VM_ALLOCED))
		free(vmu);
}

static void free_zone(struct vm_zone *z)
{
	free(z);
}

static char *mbox(int id)
{
	switch(id) {
	case 0:
		return "INBOX";
	case 1:
		return "Old";
	case 2:
		return "Work";
	case 3:
		return "Family";
	case 4:
		return "Friends";
	case 5:
		return "Cust1";
	case 6:
		return "Cust2";
	case 7:
		return "Cust3";
	case 8:
		return "Cust4";
	case 9:
		return "Cust5";
	default:
		return "Unknown";
	}
}

#ifdef USE_ODBC_STORAGE
static int messagecount(const char *mailbox, int *newmsgs, int *oldmsgs)
{
	int x = 0;
	int res;
	SQLHSTMT stmt;
	char sql[256];
	char rowdata[20];
	char tmp[256]="";
        char *context;

        if (newmsgs)
                *newmsgs = 0;
        if (oldmsgs)
                *oldmsgs = 0;
        /* If no mailbox, return immediately */
        if (opbx_strlen_zero(mailbox))
                return 0;

        opbx_copy_string(tmp, mailbox, sizeof(tmp));
        
	context = strchr(tmp, '@');
        if (context) {   
                *context = '\0';
                context++;
        } else  
                context = "default";
	
	odbc_obj *obj;
	obj = fetch_odbc_obj(odbc_database, 0);
	if (obj) {
		res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
			goto yuck;
		}
		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir like \"%%%s/%s/%s\"%c", odbc_table, context, tmp, "INBOX", '\0');
		res = SQLPrepare(stmt, sql, SQL_NTS);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Prepare failed![%s]\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		res = odbc_smart_execute(obj, stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		*newmsgs = atoi(rowdata);
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);

		res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
			goto yuck;
		}
		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir like \"%%%s/%s/%s\"%c", odbc_table, context, tmp, "Old", '\0');
		res = SQLPrepare(stmt, sql, SQL_NTS);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Prepare failed![%s]\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		res = odbc_smart_execute(obj, stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			opbx_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		*oldmsgs = atoi(rowdata);
		x = 1;
	} else
		opbx_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
		
yuck:	
	return x;
}

static int has_voicemail(const char *mailbox, const char *folder)
{
	int nummsgs = 0;
        int res;
        SQLHSTMT stmt;
        char sql[256];
        char rowdata[20];
        char tmp[256]="";
        char *context;
	if (!folder)
                folder = "INBOX";
	/* If no mailbox, return immediately */
        if (opbx_strlen_zero(mailbox))
                return 0;

	opbx_copy_string(tmp, mailbox, sizeof(tmp));
                        
        context = strchr(tmp, '@');
        if (context) {
                *context = '\0';
                context++;
        } else
                context = "default";

        odbc_obj *obj;
        obj = fetch_odbc_obj(odbc_database, 0);
        if (obj) {
                res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
                if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
                        opbx_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
                        goto yuck;
                }
		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir like \"%%%s/%s/%s\"%c", odbc_table, context, tmp, "INBOX", '\0');
                res = SQLPrepare(stmt, sql, SQL_NTS);
                if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {  
                        opbx_log(LOG_WARNING, "SQL Prepare failed![%s]\n", sql);
                        SQLFreeHandle (SQL_HANDLE_STMT, stmt);
                        goto yuck;
                }
                res = odbc_smart_execute(obj, stmt);
                if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
                        opbx_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
                        SQLFreeHandle (SQL_HANDLE_STMT, stmt);
                        goto yuck;
                }
                res = SQLFetch(stmt);
                if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
                        opbx_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
                        SQLFreeHandle (SQL_HANDLE_STMT, stmt);
                        goto yuck;
                }
                res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
                if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
                        opbx_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
                        SQLFreeHandle (SQL_HANDLE_STMT, stmt);
                        goto yuck;
                }
                nummsgs = atoi(rowdata);
                SQLFreeHandle (SQL_HANDLE_STMT, stmt);
       } else
                opbx_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);

yuck:
	if (nummsgs>=1)
		return 1;
	else
		return 0;
}

#else

static int has_voicemail(const char *mailbox, const char *folder)
{
	DIR *dir;
	struct dirent *de;
	char fn[256];
	char tmp[256]="";
	char *mb, *cur;
	char *context;
	int ret;
	if (!folder)
		folder = "INBOX";
	/* If no mailbox, return immediately */
	if (opbx_strlen_zero(mailbox))
		return 0;
	if (strchr(mailbox, ',')) {
		opbx_copy_string(tmp, mailbox, sizeof(tmp));
		mb = tmp;
		ret = 0;
		while((cur = strsep(&mb, ","))) {
			if (!opbx_strlen_zero(cur)) {
				if (has_voicemail(cur, folder))
					return 1; 
			}
		}
		return 0;
	}
	opbx_copy_string(tmp, mailbox, sizeof(tmp));
	context = strchr(tmp, '@');
	if (context) {
		*context = '\0';
		context++;
	} else
		context = "default";
	snprintf(fn, sizeof(fn), "%s/%s/%s/%s", VM_SPOOL_DIR, context, tmp, folder);
	dir = opendir(fn);
	if (!dir)
		return 0;
	while ((de = readdir(dir))) {
		if (!strncasecmp(de->d_name, "msg", 3))
			break;
	}
	closedir(dir);
	if (de)
		return 1;
	return 0;
}


static int messagecount(const char *mailbox, int *newmsgs, int *oldmsgs)
{
	DIR *dir;
	struct dirent *de;
	char fn[256];
	char tmp[256]="";
	char *mb, *cur;
	char *context;
	int ret;
	if (newmsgs)
		*newmsgs = 0;
	if (oldmsgs)
		*oldmsgs = 0;
	/* If no mailbox, return immediately */
	if (opbx_strlen_zero(mailbox))
		return 0;
	if (strchr(mailbox, ',')) {
		int tmpnew, tmpold;
		opbx_copy_string(tmp, mailbox, sizeof(tmp));
		mb = tmp;
		ret = 0;
		while((cur = strsep(&mb, ", "))) {
			if (!opbx_strlen_zero(cur)) {
				if (messagecount(cur, newmsgs ? &tmpnew : NULL, oldmsgs ? &tmpold : NULL))
					return -1;
				else {
					if (newmsgs)
						*newmsgs += tmpnew; 
					if (oldmsgs)
						*oldmsgs += tmpold;
				}
			}
		}
		return 0;
	}
	opbx_copy_string(tmp, mailbox, sizeof(tmp));
	context = strchr(tmp, '@');
	if (context) {
		*context = '\0';
		context++;
	} else
		context = "default";
	if (newmsgs) {
		snprintf(fn, sizeof(fn), "%s/%s/%s/INBOX", VM_SPOOL_DIR, context, tmp);
		dir = opendir(fn);
		if (dir) {
			while ((de = readdir(dir))) {
				if ((strlen(de->d_name) > 3) && !strncasecmp(de->d_name, "msg", 3) &&
					!strcasecmp(de->d_name + strlen(de->d_name) - 3, "txt"))
						(*newmsgs)++;
					
			}
			closedir(dir);
		}
	}
	if (oldmsgs) {
		snprintf(fn, sizeof(fn), "%s/%s/%s/Old", VM_SPOOL_DIR, context, tmp);
		dir = opendir(fn);
		if (dir) {
			while ((de = readdir(dir))) {
				if ((strlen(de->d_name) > 3) && !strncasecmp(de->d_name, "msg", 3) &&
					!strcasecmp(de->d_name + strlen(de->d_name) - 3, "txt"))
						(*oldmsgs)++;
					
			}
			closedir(dir);
		}
	}
	return 0;
}

#endif

static int notify_new_message(struct opbx_channel *chan, struct opbx_vm_user *vmu, int msgnum, long duration, char *fmt, char *cidnum, char *cidname);

static int copy_message(struct opbx_channel *chan, struct opbx_vm_user *vmu, int imbox, int msgnum, long duration, struct opbx_vm_user *recip, char *fmt)
{
	char fromdir[256], todir[256], frompath[256], topath[256];
	char *frombox = mbox(imbox);
	int recipmsgnum;

	opbx_log(LOG_NOTICE, "Copying message from %s@%s to %s@%s\n", vmu->mailbox, vmu->context, recip->mailbox, recip->context);

	make_dir(todir, sizeof(todir), recip->context, "", "");
	/* It's easier just to try to make it than to check for its existence */
	if (mkdir(todir, 0700) && (errno != EEXIST))
		opbx_log(LOG_WARNING, "mkdir '%s' failed: %s\n", todir, strerror(errno));
	make_dir(todir, sizeof(todir), recip->context, recip->mailbox, "");
	/* It's easier just to try to make it than to check for its existence */
	if (mkdir(todir, 0700) && (errno != EEXIST))
		opbx_log(LOG_WARNING, "mkdir '%s' failed: %s\n", todir, strerror(errno));
	make_dir(todir, sizeof(todir), recip->context, recip->mailbox, "INBOX");
	if (mkdir(todir, 0700) && (errno != EEXIST))
		opbx_log(LOG_WARNING, "mkdir '%s' failed: %s\n", todir, strerror(errno));

	make_dir(fromdir, sizeof(fromdir), vmu->context, vmu->mailbox, frombox);
	make_file(frompath, sizeof(frompath), fromdir, msgnum);

	if (vm_lock_path(topath))
		return ERROR_LOCK_PATH;

	recipmsgnum = 0;
	do {
		make_file(topath, sizeof(topath), todir, recipmsgnum);
		if (!EXISTS(todir, recipmsgnum, topath, chan->language))
			break;
		recipmsgnum++;
	} while (recipmsgnum < recip->maxmsg);
	if (recipmsgnum < recip->maxmsg) {
		COPY(fromdir, msgnum, todir, recipmsgnum, recip->mailbox, recip->context, frompath, topath);
	} else {
		opbx_log(LOG_ERROR, "Recipient mailbox %s@%s is full\n", recip->mailbox, recip->context);
	}
	opbx_unlock_path(topath);
	notify_new_message(chan, recip, recipmsgnum, duration, fmt, chan->cid.cid_num, chan->cid.cid_name);
	
	return 0;
}

static void run_externnotify(char *context, char *extension)
{
	char arguments[255];
	char ext_context[256] = "";
	int newvoicemails = 0, oldvoicemails = 0;

	if (!opbx_strlen_zero(context))
		snprintf(ext_context, sizeof(ext_context), "%s@%s", extension, context);
	else
		opbx_copy_string(ext_context, extension, sizeof(ext_context));

	if (!opbx_strlen_zero(externnotify)) {
		if (messagecount(ext_context, &newvoicemails, &oldvoicemails)) {
			opbx_log(LOG_ERROR, "Problem in calculating number of voicemail messages available for extension %s\n", extension);
		} else {
			snprintf(arguments, sizeof(arguments), "%s %s %s %d&", externnotify, context, extension, newvoicemails);
			opbx_log(LOG_DEBUG, "Executing %s\n", arguments);
	  		opbx_safe_system(arguments);
		}
	}
}

struct leave_vm_options {
	unsigned int flags;
	signed char record_gain;
};

static int leave_voicemail(struct opbx_channel *chan, char *ext, struct leave_vm_options *options)
{
	char txtfile[256];
	char callerid[256];
	FILE *txt;
	int res = 0;
	int msgnum;
	int duration = 0;
	int ausemacro = 0;
	int ousemacro = 0;
	char date[256];
	char dir[256];
	char fn[256];
	char prefile[256]="";
	char tempfile[256]="";
	char ext_context[256] = "";
	char fmt[80];
	char *context;
	char ecodes[16] = "#";
	char tmp[256] = "", *tmpptr;
	struct opbx_vm_user *vmu;
	struct opbx_vm_user svm;
	char *category = NULL;

	opbx_copy_string(tmp, ext, sizeof(tmp));
	ext = tmp;
	context = strchr(tmp, '@');
	if (context) {
		*context = '\0';
		context++;
		tmpptr = strchr(context, '&');
	} else {
		tmpptr = strchr(ext, '&');
	}

	if (tmpptr) {
		*tmpptr = '\0';
		tmpptr++;
	}

	category = pbx_builtin_getvar_helper(chan, "VM_CATEGORY");

	if (!(vmu = find_user(&svm, context, ext))) {
		opbx_log(LOG_WARNING, "No entry in voicemail config file for '%s'\n", ext);
		opbx_goto_if_exists(chan, chan->context, chan->exten, chan->priority + 101);
		return res;
	}

	/* Setup pre-file if appropriate */
	if (strcmp(vmu->context, "default"))
		snprintf(ext_context, sizeof(ext_context), "%s@%s", ext, vmu->context);
	else
		opbx_copy_string(ext_context, vmu->context, sizeof(ext_context));
	if (opbx_test_flag(options, OPT_BUSY_GREETING))
		snprintf(prefile, sizeof(prefile), "%s%s/%s/busy", VM_SPOOL_DIR, vmu->context, ext);
	else if (opbx_test_flag(options, OPT_UNAVAIL_GREETING))
		snprintf(prefile, sizeof(prefile), "%s%s/%s/unavail", VM_SPOOL_DIR, vmu->context, ext);
	snprintf(tempfile, sizeof(tempfile), "%s%s/%s/temp", VM_SPOOL_DIR, vmu->context, ext);
	RETRIEVE(tempfile, -1);
	if (opbx_fileexists(tempfile, NULL, NULL) > 0)
		opbx_copy_string(prefile, tempfile, sizeof(prefile));
	DISPOSE(tempfile, -1);
	make_dir(dir, sizeof(dir), vmu->context, "", "");
	/* It's easier just to try to make it than to check for its existence */
	if (mkdir(dir, 0700) && (errno != EEXIST))
		opbx_log(LOG_WARNING, "mkdir '%s' failed: %s\n", dir, strerror(errno));
	make_dir(dir, sizeof(dir), vmu->context, ext, "");
	/* It's easier just to try to make it than to check for its existence */
	if (mkdir(dir, 0700) && (errno != EEXIST))
		opbx_log(LOG_WARNING, "mkdir '%s' failed: %s\n", dir, strerror(errno));
	make_dir(dir, sizeof(dir), vmu->context, ext, "INBOX");
	if (mkdir(dir, 0700) && (errno != EEXIST))
		opbx_log(LOG_WARNING, "mkdir '%s' failed: %s\n", dir, strerror(errno));

	/* Check current or macro-calling context for special extensions */
	if (!opbx_strlen_zero(vmu->exit)) {
		if (opbx_exists_extension(chan, vmu->exit, "o", 1, chan->cid.cid_num))
			strncat(ecodes, "0", sizeof(ecodes) - strlen(ecodes) - 1);
	} else if (opbx_exists_extension(chan, chan->context, "o", 1, chan->cid.cid_num))
		strncat(ecodes, "0", sizeof(ecodes) - strlen(ecodes) - 1);
	else if (!opbx_strlen_zero(chan->macrocontext) && opbx_exists_extension(chan, chan->macrocontext, "o", 1, chan->cid.cid_num)) {
		strncat(ecodes, "0", sizeof(ecodes) - strlen(ecodes) - 1);
		ousemacro = 1;
	}

	if (!opbx_strlen_zero(vmu->exit)) {
		if (opbx_exists_extension(chan, vmu->exit, "a", 1, chan->cid.cid_num))
			strncat(ecodes, "*", sizeof(ecodes) -  strlen(ecodes) - 1);
	} else if (opbx_exists_extension(chan, chan->context, "a", 1, chan->cid.cid_num))
		strncat(ecodes, "*", sizeof(ecodes) -  strlen(ecodes) - 1);
	else if (!opbx_strlen_zero(chan->macrocontext) && opbx_exists_extension(chan, chan->macrocontext, "a", 1, chan->cid.cid_num)) {
		strncat(ecodes, "*", sizeof(ecodes) -  strlen(ecodes) - 1);
		ausemacro = 1;
	}

	/* Play the beginning intro if desired */
	if (!opbx_strlen_zero(prefile)) {
		RETRIEVE(prefile, -1);
		if (opbx_fileexists(prefile, NULL, NULL) > 0) {
			if (opbx_streamfile(chan, prefile, chan->language) > -1) 
				res = opbx_waitstream(chan, ecodes);
		} else {
			opbx_log(LOG_DEBUG, "%s doesn't exist, doing what we can\n", prefile);
			res = invent_message(chan, vmu->context, ext, opbx_test_flag(options, OPT_BUSY_GREETING), ecodes);
		}
		DISPOSE(prefile, -1);
		if (res < 0) {
			opbx_log(LOG_DEBUG, "Hang up during prefile playback\n");
			free_user(vmu);
			return -1;
		}
	}
	if (res == '#') {
		/* On a '#' we skip the instructions */
		opbx_set_flag(options, OPT_SILENT);
		res = 0;
	}
	if (!res && !opbx_test_flag(options, OPT_SILENT)) {
		res = opbx_streamfile(chan, INTRO, chan->language);
		if (!res)
			res = opbx_waitstream(chan, ecodes);
		if (res == '#') {
			opbx_set_flag(options, OPT_SILENT);
			res = 0;
		}
	}
	if (res > 0)
		opbx_stopstream(chan);
	/* Check for a '*' here in case the caller wants to escape from voicemail to something
	   other than the operator -- an automated attendant or mailbox login for example */
	if (res == '*') {
		chan->exten[0] = 'a';
		chan->exten[1] = '\0';
		if (!opbx_strlen_zero(vmu->exit)) {
			opbx_copy_string(chan->context, vmu->exit, sizeof(chan->context));
		} else if (ausemacro && !opbx_strlen_zero(chan->macrocontext)) {
			opbx_copy_string(chan->context, chan->macrocontext, sizeof(chan->context));
		}
		chan->priority = 0;
		free_user(vmu);
		return 0;
	}
	/* Check for a '0' here */
	if (res == '0') {
	transfer:
		if (opbx_test_flag(vmu, VM_OPERATOR)) {
			chan->exten[0] = 'o';
			chan->exten[1] = '\0';
			if (!opbx_strlen_zero(vmu->exit)) {
				opbx_copy_string(chan->context, vmu->exit, sizeof(chan->context));
			} else if (ousemacro && !opbx_strlen_zero(chan->macrocontext)) {
				opbx_copy_string(chan->context, chan->macrocontext, sizeof(chan->context));
			}
			opbx_play_and_wait(chan, "transfer");
			chan->priority = 0;
			free_user(vmu);
			return 0;
		} else {
			opbx_play_and_wait(chan, "vm-sorry");
			return 0;
		}
	}
	if (res < 0) {
		free_user(vmu);
		return -1;
	}
	/* The meat of recording the message...  All the announcements and beeps have been played*/
	opbx_copy_string(fmt, vmfmts, sizeof(fmt));
	if (!opbx_strlen_zero(fmt)) {
		msgnum = 0;

		if (vm_lock_path(dir)) {
			free_user(vmu);
			return ERROR_LOCK_PATH;
		}

		/* 
		 * This operation can be very expensive if done say over NFS or if the mailbox has 100+ messages
		 * in the mailbox.  So we should get this first so we don't cut off the first few seconds of the 
		 * message.  
		 */
		do {
			make_file(fn, sizeof(fn), dir, msgnum);
			if (!EXISTS(dir,msgnum,fn,chan->language))
				break;
			msgnum++;
		} while (msgnum < vmu->maxmsg);

		/* Now play the beep once we have the message number for our next message. */
		if (res >= 0) {
			/* Unless we're *really* silent, try to send the beep */
			res = opbx_streamfile(chan, "beep", chan->language);
			if (!res)
				res = opbx_waitstream(chan, "");
		}
		if (msgnum < vmu->maxmsg) {
			/* assign a variable with the name of the voicemail file */	  
			pbx_builtin_setvar_helper(chan, "VM_MESSAGEFILE", fn);
				
			/* Store information */
			snprintf(txtfile, sizeof(txtfile), "%s.txt", fn);
			txt = fopen(txtfile, "w+");
			if (txt) {
				get_date(date, sizeof(date));
				fprintf(txt, 
					";\n"
					"; Message Information file\n"
					";\n"
					"[message]\n"
					"origmailbox=%s\n"
					"context=%s\n"
					"macrocontext=%s\n"
					"exten=%s\n"
					"priority=%d\n"
					"callerchan=%s\n"
					"callerid=%s\n"
					"origdate=%s\n"
					"origtime=%ld\n"
					"category=%s\n",
					ext,
					chan->context,
					chan->macrocontext, 
					chan->exten,
					chan->priority,
					chan->name,
					opbx_callerid_merge(callerid, sizeof(callerid), chan->cid.cid_name, chan->cid.cid_num, "Unknown"),
					date, (long)time(NULL),
					category ? category : ""); 
			} else
				opbx_log(LOG_WARNING, "Error opening text file for output\n");
			res = play_record_review(chan, NULL, fn, vmmaxmessage, fmt, 1, vmu, &duration, dir, options->record_gain);
			if (res == '0') {
				if (txt)
					fclose(txt);
				goto transfer;
			}
			if (res > 0)
				res = 0;
			if (txt) {
				fprintf(txt, "duration=%d\n", duration);
				fclose(txt);
			}
				
			if (duration < vmminmessage) {
				if (option_verbose > 2) 
					opbx_verbose( VERBOSE_PREFIX_3 "Recording was %d seconds long but needs to be at least %d - abandoning\n", duration, vmminmessage);
				DELETE(dir,msgnum,fn);
				/* XXX We should really give a prompt too short/option start again, with leave_vm_out called only after a timeout XXX */
				goto leave_vm_out;
			}
			/* Are there to be more recipients of this message? */
			while (tmpptr) {
				struct opbx_vm_user recipu, *recip;
				char *exten, *context;
					
				exten = strsep(&tmpptr, "&");
				context = strchr(exten, '@');
				if (context) {
					*context = '\0';
					context++;
				}
				if ((recip = find_user(&recipu, context, exten))) {
					copy_message(chan, vmu, 0, msgnum, duration, recip, fmt);
					free_user(recip);
				}
			}
			if (opbx_fileexists(fn, NULL, NULL)) {
				notify_new_message(chan, vmu, msgnum, duration, fmt, chan->cid.cid_num, chan->cid.cid_name);
				STORE(dir, vmu->mailbox, vmu->context, msgnum);
				DISPOSE(dir, msgnum);
			}
		} else {
			opbx_unlock_path(dir);
			res = opbx_streamfile(chan, "vm-mailboxfull", chan->language);
			if (!res)
				res = opbx_waitstream(chan, "");
			opbx_log(LOG_WARNING, "No more messages possible\n");
		}
	} else
		opbx_log(LOG_WARNING, "No format for saving voicemail?\n");
 leave_vm_out:
	free_user(vmu);

	return res;
}

static int resequence_mailbox(struct opbx_vm_user *vmu, char *dir)
{
	/* we know max messages, so stop process when number is hit */

	int x,dest;
	char sfn[256];
	char dfn[256];

	if (vm_lock_path(dir))
		return ERROR_LOCK_PATH;

	for (x = 0, dest = 0; x < vmu->maxmsg; x++) {
		make_file(sfn, sizeof(sfn), dir, x);
		if (EXISTS(dir, x, sfn, NULL)) {
			
			if(x != dest) {
				make_file(dfn, sizeof(dfn), dir, dest);
				RENAME(dir, x, vmu->mailbox, vmu->context, dir, dest, sfn, dfn);
			}
			
			dest++;
		}
	}
	opbx_unlock_path(dir);

	return 0;
}


static int say_and_wait(struct opbx_channel *chan, int num, char *language)
{
	int d;
	d = opbx_say_number(chan, num, OPBX_DIGIT_ANY, language, (char *) NULL);
	return d;
}

static int save_to_folder(struct opbx_vm_user *vmu, char *dir, int msg, char *context, char *username, int box)
{
	char sfn[256];
	char dfn[256];
	char ddir[256];
	char *dbox = mbox(box);
	int x;
	make_file(sfn, sizeof(sfn), dir, msg);
	make_dir(ddir, sizeof(ddir), context, username, dbox);
	mkdir(ddir, 0700);

	if (vm_lock_path(ddir))
		return ERROR_LOCK_PATH;

	for (x = 0; x < vmu->maxmsg; x++) {
		make_file(dfn, sizeof(dfn), ddir, x);
		if (!EXISTS(ddir, x, dfn, NULL))
			break;
	}
	if (x >= vmu->maxmsg) {
		opbx_unlock_path(ddir);
		return -1;
	}
	if (strcmp(sfn, dfn)) {
		COPY(dir, msg, ddir, x, username, context, sfn, dfn);
	}
	opbx_unlock_path(ddir);
	
	return 0;
}

static int adsi_logo(unsigned char *buf)
{
	int bytes = 0;
	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 1, ADSI_JUST_CENT, 0, "Comedian Mail", "");
	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 2, ADSI_JUST_CENT, 0, "(C)2002 LSS, Inc.", "");
	return bytes;
}

static int adsi_load_vmail(struct opbx_channel *chan, int *useadsi)
{
	unsigned char buf[256];
	int bytes=0;
	int x;
	char num[5];

	*useadsi = 0;
	bytes += adsi_data_mode(buf + bytes);
 	adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);

	bytes = 0;
	bytes += adsi_logo(buf);
	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Downloading Scripts", "");
#ifdef DISPLAY
	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, "   .", "");
#endif
	bytes += adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
	bytes += adsi_data_mode(buf + bytes);
 	adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);

	if (adsi_begin_download(chan, addesc, adsifdn, adsisec, adsiver)) {
		bytes = 0;
		bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Load Cancelled.", "");
		bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "ADSI Unavailable", "");
		bytes += adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += adsi_voice_mode(buf + bytes, 0);
		adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
		return 0;
	}

#ifdef DISPLAY
	/* Add a dot */
	bytes = 0;
	bytes += adsi_logo(buf);
	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Downloading Scripts", "");
	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, "   ..", "");
	bytes += adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
 	adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
#endif
	bytes = 0;
	bytes += adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 0, "Listen", "Listen", "1", 1);
	bytes += adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 1, "Folder", "Folder", "2", 1);
	bytes += adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 2, "Advanced", "Advnced", "3", 1);
	bytes += adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 3, "Options", "Options", "0", 1);
	bytes += adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 4, "Help", "Help", "*", 1);
	bytes += adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 5, "Exit", "Exit", "#", 1);
 	adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DOWNLOAD);

#ifdef DISPLAY
	/* Add another dot */
	bytes = 0;
	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, "   ...", "");
	bytes += adsi_voice_mode(buf + bytes, 0);

	bytes += adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
 	adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
#endif

	bytes = 0;
	/* These buttons we load but don't use yet */
	bytes += adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 6, "Previous", "Prev", "4", 1);
	bytes += adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 8, "Repeat", "Repeat", "5", 1);
	bytes += adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 7, "Delete", "Delete", "7", 1);
	bytes += adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 9, "Next", "Next", "6", 1);
	bytes += adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 10, "Save", "Save", "9", 1);
	bytes += adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 11, "Undelete", "Restore", "7", 1);
 	adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DOWNLOAD);

#ifdef DISPLAY
	/* Add another dot */
	bytes = 0;
	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, "   ....", "");
	bytes += adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
 	adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
#endif

	bytes = 0;
	for (x=0;x<5;x++) {
		snprintf(num, sizeof(num), "%d", x);
		bytes += adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 12 + x, mbox(x), mbox(x), num, 1);
	}
	bytes += adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 12 + 5, "Cancel", "Cancel", "#", 1);
 	adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DOWNLOAD);

#ifdef DISPLAY
	/* Add another dot */
	bytes = 0;
	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, "   .....", "");
	bytes += adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
 	adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
#endif

	if (adsi_end_download(chan)) {
		bytes = 0;
		bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Download Unsuccessful.", "");
		bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "ADSI Unavailable", "");
		bytes += adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += adsi_voice_mode(buf + bytes, 0);
		adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
		return 0;
	}
	bytes = 0;
	bytes += adsi_download_disconnect(buf + bytes);
	bytes += adsi_voice_mode(buf + bytes, 0);
 	adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DOWNLOAD);

	opbx_log(LOG_DEBUG, "Done downloading scripts...\n");

#ifdef DISPLAY
	/* Add last dot */
	bytes = 0;
	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "   ......", "");
	bytes += adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
#endif
	opbx_log(LOG_DEBUG, "Restarting session...\n");

	bytes = 0;
	/* Load the session now */
	if (adsi_load_session(chan, adsifdn, adsiver, 1) == 1) {
		*useadsi = 1;
		bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Scripts Loaded!", "");
	} else
		bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Load Failed!", "");

 	adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	return 0;
}

static void adsi_begin(struct opbx_channel *chan, int *useadsi)
{
	int x;
	if (!adsi_available(chan))
		return;
	x = adsi_load_session(chan, adsifdn, adsiver, 1);
	if (x < 0)
		return;
	if (!x) {
		if (adsi_load_vmail(chan, useadsi)) {
			opbx_log(LOG_WARNING, "Unable to upload voicemail scripts\n");
			return;
		}
	} else
		*useadsi = 1;
}

static void adsi_login(struct opbx_channel *chan)
{
	unsigned char buf[256];
	int bytes=0;
	unsigned char keys[8];
	int x;
	if (!adsi_available(chan))
		return;

	for (x=0;x<8;x++)
		keys[x] = 0;
	/* Set one key for next */
	keys[3] = ADSI_KEY_APPS + 3;

	bytes += adsi_logo(buf + bytes);
	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, " ", "");
	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, " ", "");
	bytes += adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
	bytes += adsi_input_format(buf + bytes, 1, ADSI_DIR_FROM_LEFT, 0, "Mailbox: ******", "");
	bytes += adsi_input_control(buf + bytes, ADSI_COMM_PAGE, 4, 1, 1, ADSI_JUST_LEFT);
	bytes += adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 3, "Enter", "Enter", "#", 1);
	bytes += adsi_set_keys(buf + bytes, keys);
	bytes += adsi_voice_mode(buf + bytes, 0);
 	adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
}

static void adsi_password(struct opbx_channel *chan)
{
	unsigned char buf[256];
	int bytes=0;
	unsigned char keys[8];
	int x;
	if (!adsi_available(chan))
		return;

	for (x=0;x<8;x++)
		keys[x] = 0;
	/* Set one key for next */
	keys[3] = ADSI_KEY_APPS + 3;

	bytes += adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
	bytes += adsi_input_format(buf + bytes, 1, ADSI_DIR_FROM_LEFT, 0, "Password: ******", "");
	bytes += adsi_input_control(buf + bytes, ADSI_COMM_PAGE, 4, 0, 1, ADSI_JUST_LEFT);
	bytes += adsi_set_keys(buf + bytes, keys);
	bytes += adsi_voice_mode(buf + bytes, 0);
	adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
}

static void adsi_folders(struct opbx_channel *chan, int start, char *label)
{
	unsigned char buf[256];
	int bytes=0;
	unsigned char keys[8];
	int x,y;

	if (!adsi_available(chan))
		return;

	for (x=0;x<5;x++) {
		y = ADSI_KEY_APPS + 12 + start + x;
		if (y > ADSI_KEY_APPS + 12 + 4)
			y = 0;
		keys[x] = ADSI_KEY_SKT | y;
	}
	keys[5] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 17);
	keys[6] = 0;
	keys[7] = 0;

	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 1, ADSI_JUST_CENT, 0, label, "");
	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 2, ADSI_JUST_CENT, 0, " ", "");
	bytes += adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
	bytes += adsi_set_keys(buf + bytes, keys);
	bytes += adsi_voice_mode(buf + bytes, 0);

	adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
}

static void adsi_message(struct opbx_channel *chan, struct vm_state *vms)
{
	int bytes=0;
	unsigned char buf[256]; 
	char buf1[256], buf2[256];
	char fn2[256];

	char cid[256]="";
	char *val;
	char *name, *num;
	char datetime[21]="";
	FILE *f;

	unsigned char keys[8];

	int x;

	if (!adsi_available(chan))
		return;

	/* Retrieve important info */
	snprintf(fn2, sizeof(fn2), "%s.txt", vms->fn);
	f = fopen(fn2, "r");
	if (f) {
		while (!feof(f)) {	
			fgets((char *)buf, sizeof(buf), f);
			if (!feof(f)) {
				char *stringp=NULL;
				stringp = (char *)buf;
				strsep(&stringp, "=");
				val = strsep(&stringp, "=");
				if (!opbx_strlen_zero(val)) {
					if (!strcmp((char *)buf, "callerid"))
						opbx_copy_string(cid, val, sizeof(cid));
					if (!strcmp((char *)buf, "origdate"))
						opbx_copy_string(datetime, val, sizeof(datetime));
				}
			}
		}
		fclose(f);
	}
	/* New meaning for keys */
	for (x=0;x<5;x++)
		keys[x] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 6 + x);
	keys[6] = 0x0;
	keys[7] = 0x0;

	if (!vms->curmsg) {
		/* No prev key, provide "Folder" instead */
		keys[0] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 1);
	}
	if (vms->curmsg >= vms->lastmsg) {
		/* If last message ... */
		if (vms->curmsg) {
			/* but not only message, provide "Folder" instead */
			keys[3] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 1);
			bytes += adsi_voice_mode(buf + bytes, 0);

		} else {
			/* Otherwise if only message, leave blank */
			keys[3] = 1;
		}
	}

	if (!opbx_strlen_zero(cid)) {
		opbx_callerid_parse(cid, &name, &num);
		if (!name)
			name = num;
	} else
		name = "Unknown Caller";

	/* If deleted, show "undeleted" */

	if (vms->deleted[vms->curmsg])
		keys[1] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 11);

	/* Except "Exit" */
	keys[5] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 5);
	snprintf(buf1, sizeof(buf1), "%s%s", vms->curbox,
		strcasecmp(vms->curbox, "INBOX") ? " Messages" : "");
 	snprintf(buf2, sizeof(buf2), "Message %d of %d", vms->curmsg + 1, vms->lastmsg + 1);

 	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 1, ADSI_JUST_LEFT, 0, buf1, "");
	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 2, ADSI_JUST_LEFT, 0, buf2, "");
	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_LEFT, 0, name, "");
	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, datetime, "");
	bytes += adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
	bytes += adsi_set_keys(buf + bytes, keys);
	bytes += adsi_voice_mode(buf + bytes, 0);

	adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
}

static void adsi_delete(struct opbx_channel *chan, struct vm_state *vms)
{
	int bytes=0;
	unsigned char buf[256];
	unsigned char keys[8];

	int x;

	if (!adsi_available(chan))
		return;

	/* New meaning for keys */
	for (x=0;x<5;x++)
		keys[x] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 6 + x);

	keys[6] = 0x0;
	keys[7] = 0x0;

	if (!vms->curmsg) {
		/* No prev key, provide "Folder" instead */
		keys[0] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 1);
	}
	if (vms->curmsg >= vms->lastmsg) {
		/* If last message ... */
		if (vms->curmsg) {
			/* but not only message, provide "Folder" instead */
			keys[3] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 1);
		} else {
			/* Otherwise if only message, leave blank */
			keys[3] = 1;
		}
	}

	/* If deleted, show "undeleted" */
	if (vms->deleted[vms->curmsg]) 
		keys[1] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 11);

	/* Except "Exit" */
	keys[5] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 5);
	bytes += adsi_set_keys(buf + bytes, keys);
	bytes += adsi_voice_mode(buf + bytes, 0);

	adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
}

static void adsi_status(struct opbx_channel *chan, struct vm_state *vms)
{
	unsigned char buf[256] = "";
	char buf1[256] = "", buf2[256] = "";
	int bytes=0;
	unsigned char keys[8];
	int x;

	char *newm = (vms->newmessages == 1) ? "message" : "messages";
	char *oldm = (vms->oldmessages == 1) ? "message" : "messages";
	if (!adsi_available(chan))
		return;
	if (vms->newmessages) {
		snprintf(buf1, sizeof(buf1), "You have %d new", vms->newmessages);
		if (vms->oldmessages) {
			strncat(buf1, " and", sizeof(buf1) - strlen(buf1) - 1);
			snprintf(buf2, sizeof(buf2), "%d old %s.", vms->oldmessages, oldm);
		} else {
			snprintf(buf2, sizeof(buf2), "%s.", newm);
		}
	} else if (vms->oldmessages) {
		snprintf(buf1, sizeof(buf1), "You have %d old", vms->oldmessages);
		snprintf(buf2, sizeof(buf2), "%s.", oldm);
	} else {
		strcpy(buf1, "You have no messages.");
		buf2[0] = ' ';
		buf2[1] = '\0';
	}
 	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 1, ADSI_JUST_LEFT, 0, buf1, "");
	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 2, ADSI_JUST_LEFT, 0, buf2, "");
	bytes += adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);

	for (x=0;x<6;x++)
		keys[x] = ADSI_KEY_SKT | (ADSI_KEY_APPS + x);
	keys[6] = 0;
	keys[7] = 0;

	/* Don't let them listen if there are none */
	if (vms->lastmsg < 0)
		keys[0] = 1;
	bytes += adsi_set_keys(buf + bytes, keys);

	bytes += adsi_voice_mode(buf + bytes, 0);

	adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
}

static void adsi_status2(struct opbx_channel *chan, struct vm_state *vms)
{
	unsigned char buf[256] = "";
	char buf1[256] = "", buf2[256] = "";
	int bytes=0;
	unsigned char keys[8];
	int x;

	char *mess = (vms->lastmsg == 0) ? "message" : "messages";

	if (!adsi_available(chan))
		return;

	/* Original command keys */
	for (x=0;x<6;x++)
		keys[x] = ADSI_KEY_SKT | (ADSI_KEY_APPS + x);

	keys[6] = 0;
	keys[7] = 0;

	if ((vms->lastmsg + 1) < 1)
		keys[0] = 0;

	snprintf(buf1, sizeof(buf1), "%s%s has", vms->curbox,
		strcasecmp(vms->curbox, "INBOX") ? " folder" : "");

	if (vms->lastmsg + 1)
		snprintf(buf2, sizeof(buf2), "%d %s.", vms->lastmsg + 1, mess);
	else
		strcpy(buf2, "no messages.");
 	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 1, ADSI_JUST_LEFT, 0, buf1, "");
	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 2, ADSI_JUST_LEFT, 0, buf2, "");
	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_LEFT, 0, "", "");
	bytes += adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
	bytes += adsi_set_keys(buf + bytes, keys);

	bytes += adsi_voice_mode(buf + bytes, 0);

	adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	
}

/*
static void adsi_clear(struct opbx_channel *chan)
{
	char buf[256];
	int bytes=0;
	if (!adsi_available(chan))
		return;
	bytes += adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
	bytes += adsi_voice_mode(buf + bytes, 0);

	adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
}
*/

static void adsi_goodbye(struct opbx_channel *chan)
{
	unsigned char buf[256];
	int bytes=0;

	if (!adsi_available(chan))
		return;
	bytes += adsi_logo(buf + bytes);
	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_LEFT, 0, " ", "");
	bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "Goodbye", "");
	bytes += adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
	bytes += adsi_voice_mode(buf + bytes, 0);

	adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
}

/*--- get_folder: Folder menu ---*/
/* Plays "press 1 for INBOX messages" etc
   Should possibly be internationalized
 */
static int get_folder(struct opbx_channel *chan, int start)
{
	int x;
	int d;
	char fn[256];
	d = opbx_play_and_wait(chan, "vm-press");	/* "Press" */
	if (d)
		return d;
	for (x = start; x< 5; x++) {	/* For all folders */
		if ((d = opbx_say_number(chan, x, OPBX_DIGIT_ANY, chan->language, (char *) NULL)))
			return d;
		d = opbx_play_and_wait(chan, "vm-for");	/* "for" */
		if (d)
			return d;
		snprintf(fn, sizeof(fn), "vm-%s", mbox(x));	/* Folder name */
		d = vm_play_folder_name(chan, fn);
		if (d)
			return d;
		d = opbx_waitfordigit(chan, 500);
		if (d)
			return d;
	}
	d = opbx_play_and_wait(chan, "vm-tocancel"); /* "or pound to cancel" */
	if (d)
		return d;
	d = opbx_waitfordigit(chan, 4000);
	return d;
}

static int get_folder2(struct opbx_channel *chan, char *fn, int start)
{
	int res = 0;
	res = opbx_play_and_wait(chan, fn);	/* Folder name */
	while (((res < '0') || (res > '9')) &&
			(res != '#') && (res >= 0)) {
		res = get_folder(chan, 0);
	}
	return res;
}

static int vm_forwardoptions(struct opbx_channel *chan, struct opbx_vm_user *vmu, char *curdir, int curmsg, char *vmfts,
			     char *context, signed char record_gain)
{
	int cmd = 0;
	int retries = 0;
	int duration = 0;
	signed char zero_gain = 0;

	while ((cmd >= 0) && (cmd != 't') && (cmd != '*')) {
		if (cmd)
			retries = 0;
		switch (cmd) {
		case '1': 
			/* prepend a message to the current message and return */
		{
			char file[200];
			snprintf(file, sizeof(file), "%s/msg%04d", curdir, curmsg);
			if (record_gain)
				opbx_channel_setoption(chan, OPBX_OPTION_RXGAIN, &record_gain, sizeof(record_gain), 0);
			cmd = opbx_play_and_prepend(chan, NULL, file, 0, vmfmts, &duration, 1, silencethreshold, maxsilence);
			if (record_gain)
				opbx_channel_setoption(chan, OPBX_OPTION_RXGAIN, &zero_gain, sizeof(zero_gain), 0);
			break;
		}
		case '2': 
			cmd = 't';
			break;
		case '*':
			cmd = '*';
			break;
		default: 
			cmd = opbx_play_and_wait(chan,"vm-forwardoptions");
				/* "Press 1 to prepend a message or 2 to forward the message without prepending" */
			if (!cmd)
				cmd = opbx_play_and_wait(chan,"vm-starmain");
				/* "press star to return to the main menu" */
			if (!cmd)
				cmd = opbx_waitfordigit(chan,6000);
			if (!cmd)
				retries++;
			if (retries > 3)
				cmd = 't';
		 }
	}
	if (cmd == 't')
		cmd = 0;
	return cmd;
}

static int notify_new_message(struct opbx_channel *chan, struct opbx_vm_user *vmu, int msgnum, long duration, char *fmt, char *cidnum, char *cidname)
{
	char todir[256], fn[256], ext_context[256], *stringp;
	int newmsgs = 0, oldmsgs = 0;

	make_dir(todir, sizeof(todir), vmu->context, vmu->mailbox, "INBOX");
	make_file(fn, sizeof(fn), todir, msgnum);
	snprintf(ext_context, sizeof(ext_context), "%s@%s", vmu->mailbox, vmu->context);

	/* Attach only the first format */
	fmt = opbx_strdupa(fmt);
	if (fmt) {
		stringp = fmt;
		strsep(&stringp, "|");

		if (!opbx_strlen_zero(vmu->email)) {
			int attach_user_voicemail = opbx_test_flag((&globalflags), VM_ATTACH);
			char *myserveremail = serveremail;
			attach_user_voicemail = opbx_test_flag(vmu, VM_ATTACH);
			if (!opbx_strlen_zero(vmu->serveremail))
				myserveremail = vmu->serveremail;
			sendmail(myserveremail, vmu, msgnum, vmu->context, vmu->mailbox, cidnum, cidname, fn, fmt, duration, attach_user_voicemail);
		}

		if (!opbx_strlen_zero(vmu->pager)) {
			char *myserveremail = serveremail;
			if (!opbx_strlen_zero(vmu->serveremail))
				myserveremail = vmu->serveremail;
			sendpage(myserveremail, vmu->pager, msgnum, vmu->context, vmu->mailbox, cidnum, cidname, duration, vmu);
		}
	} else {
		opbx_log(LOG_ERROR, "Out of memory\n");
	}

	if (opbx_test_flag(vmu, VM_DELETE)) {
		DELETE(todir, msgnum, fn);
	}

	/* Leave voicemail for someone */
	if (opbx_app_has_voicemail(ext_context, NULL)) {
		opbx_app_messagecount(ext_context, &newmsgs, &oldmsgs);
	}
	manager_event(EVENT_FLAG_CALL, "MessageWaiting", "Mailbox: %s@%s\r\nWaiting: %d\r\nNew: %d\r\nOld: %d\r\n", vmu->mailbox, vmu->context, opbx_app_has_voicemail(ext_context, NULL), newmsgs, oldmsgs);
	run_externnotify(vmu->context, vmu->mailbox);
	return 0;
}

static int forward_message(struct opbx_channel *chan, char *context, char *dir, int curmsg, struct opbx_vm_user *sender,
			   char *fmt, int flag, signed char record_gain)
{
	char username[70]="";
	char sys[256];
	char todir[256];
	int todircount=0;
	int duration;
	struct opbx_config *mif;
	char miffile[256];
	char fn[256];
	char callerid[512];
	char ext_context[256]="";
	int res = 0, cmd = 0;
	struct opbx_vm_user *receiver = NULL, *extensions = NULL, *vmtmp = NULL, *vmfree;
	char tmp[256];
	char *stringp, *s;
	int saved_messages = 0, found = 0;
	int valid_extensions = 0;
	
	while (!res && !valid_extensions) {
		int use_directory = 0;
		if(opbx_test_flag((&globalflags), VM_DIRECFORWARD)) {
			int done = 0;
			int retries = 0;
			cmd=0;
			while((cmd >= 0) && !done ){
				if (cmd)
					retries = 0;
				switch (cmd) {
				case '1': 
					use_directory = 0;
					done = 1;
					break;
				case '2': 
					use_directory = 1;
					done=1;
					break;
				case '*': 
					cmd = 't';
					done = 1;
					break;
				default: 
					/* Press 1 to enter an extension press 2 to use the directory */
					cmd = opbx_play_and_wait(chan,"vm-forward");
					if (!cmd)
						cmd = opbx_waitfordigit(chan,3000);
					if (!cmd)
						retries++;
					if (retries > 3)
					{
						cmd = 't';
						done = 1;
					}
					
				 }
			}
			if( cmd<0 || cmd=='t' )
				break;
		}
		
		if( use_directory ) {
			/* use app_directory */
			
			char old_context[sizeof(chan->context)];
			char old_exten[sizeof(chan->exten)];
			int old_priority;
			struct opbx_app* app;

			
			app = pbx_findapp("Directory");
			if (app) {
				/* make mackup copies */
				memcpy(old_context, chan->context, sizeof(chan->context));
				memcpy(old_exten, chan->exten, sizeof(chan->exten));
				old_priority = chan->priority;
				
				/* call the the Directory, changes the channel */
				res = pbx_exec(chan, app, ((context)?context:chan->context), 1);
				
				opbx_copy_string(username, chan->exten, sizeof(username));
				
				/* restore the old context, exten, and priority */
				memcpy(chan->context, old_context, sizeof(chan->context));
				memcpy(chan->exten, old_exten, sizeof(chan->exten));
				chan->priority = old_priority;
				
			} else {
				opbx_log(LOG_WARNING, "Could not find the Directory application, disabling directory_forward\n");
				opbx_clear_flag((&globalflags), VM_DIRECFORWARD);	
			}
		} else 	{
			/* Ask for an extension */
			res = opbx_streamfile(chan, "vm-extension", chan->language);	/* "extension" */
			if (res)
				break;
			if ((res = opbx_readstring(chan, username, sizeof(username) - 1, 2000, 10000, "#") < 0))
				break;
		}
		
		/* start all over if no username */
		if (opbx_strlen_zero(username))
			continue;
		stringp = username;
		s = strsep(&stringp, "*");
		/* start optimistic */
		valid_extensions = 1;
		while (s) {
			/* find_user is going to malloc since we have a NULL as first argument */
			if ((receiver = find_user(NULL, context, s))) {
				if (!extensions)
					vmtmp = extensions = receiver;
				else {
					vmtmp->next = receiver;
					vmtmp = receiver;
				}
				found++;
			} else {
				valid_extensions = 0;
				break;
			}
			s = strsep(&stringp, "*");
		}
		/* break from the loop of reading the extensions */
		if (valid_extensions)
			break;
		/* "I am sorry, that's not a valid extension.  Please try again." */
		res = opbx_play_and_wait(chan, "pbx-invalid");
	}
	/* check if we're clear to proceed */
	if (!extensions || !valid_extensions)
		return res;
	vmtmp = extensions;
	if (flag==1) {
		struct leave_vm_options leave_options;

		/* Send VoiceMail */
		memset(&leave_options, 0, sizeof(leave_options));
		leave_options.record_gain = record_gain;
		cmd = leave_voicemail(chan, username, &leave_options);
	} else {
		/* Forward VoiceMail */
		RETRIEVE(dir, curmsg);
		cmd = vm_forwardoptions(chan, sender, dir, curmsg, vmfmts, context, record_gain);
		if (!cmd) {
			while (!res && vmtmp) {
				/* if (opbx_play_and_wait(chan, "vm-savedto"))
					break;
				*/
				snprintf(todir, sizeof(todir), "%s%s/%s/INBOX",  VM_SPOOL_DIR, vmtmp->context, vmtmp->mailbox);
				snprintf(sys, sizeof(sys), "mkdir -p %s\n", todir);
				snprintf(ext_context, sizeof(ext_context), "%s@%s", vmtmp->mailbox, vmtmp->context);
				opbx_log(LOG_DEBUG, "%s", sys);
				opbx_safe_system(sys);
		
				if ( (res = count_messages(receiver, todir)) )
					break;
				else
					todircount = res;
				opbx_copy_string(tmp, fmt, sizeof(tmp));
				stringp = tmp;
				while ((s = strsep(&stringp, "|"))) {
					/* XXX This is a hack -- we should use build_filename or similar XXX */
					if (!strcasecmp(s, "wav49"))
						s = "WAV";
					snprintf(sys, sizeof(sys), "cp %s/msg%04d.%s %s/msg%04d.%s\n", dir, curmsg, s, todir, todircount, s);
					opbx_log(LOG_DEBUG, "%s", sys);
					opbx_safe_system(sys);
				}
				snprintf(sys, sizeof(sys), "cp %s/msg%04d.txt %s/msg%04d.txt\n", dir, curmsg, todir, todircount);
				opbx_log(LOG_DEBUG, "%s", sys);
				opbx_safe_system(sys);
				snprintf(fn, sizeof(fn), "%s/msg%04d", todir,todircount);
	
				STORE(todir, vmtmp->mailbox, vmtmp->context, todircount);

				/* load the information on the source message so we can send an e-mail like a new message */
				snprintf(miffile, sizeof(miffile), "%s/msg%04d.txt", dir, curmsg);
				if ((mif=opbx_config_load(miffile))) {
	
					/* set callerid and duration variables */
					snprintf(callerid, sizeof(callerid), "FWD from: %s from %s", sender->fullname, opbx_variable_retrieve(mif, NULL, "callerid"));
					s = opbx_variable_retrieve(mif, NULL, "duration");
					if (s)
						duration = atoi(s);
					else
						duration = 0;
					if (!opbx_strlen_zero(vmtmp->email)) {
						int attach_user_voicemail = opbx_test_flag((&globalflags), VM_ATTACH);
						char *myserveremail = serveremail;
						attach_user_voicemail = opbx_test_flag(vmtmp, VM_ATTACH);
						if (!opbx_strlen_zero(vmtmp->serveremail))
							myserveremail = vmtmp->serveremail;
						sendmail(myserveremail, vmtmp, todircount, vmtmp->context, vmtmp->mailbox, chan->cid.cid_num, chan->cid.cid_name, fn, tmp, duration, attach_user_voicemail);
					}

					if (!opbx_strlen_zero(vmtmp->pager)) {
						char *myserveremail = serveremail;
						if (!opbx_strlen_zero(vmtmp->serveremail))
							myserveremail = vmtmp->serveremail;
						sendpage(myserveremail, vmtmp->pager, todircount, vmtmp->context, vmtmp->mailbox, chan->cid.cid_num, chan->cid.cid_name, duration, vmtmp);
					}
				  
					opbx_config_destroy(mif); /* or here */
				}
				/* Leave voicemail for someone */
				manager_event(EVENT_FLAG_CALL, "MessageWaiting", "Mailbox: %s\r\nWaiting: %d\r\n", ext_context, has_voicemail(ext_context, NULL));
				run_externnotify(vmtmp->context, vmtmp->mailbox);
	
				saved_messages++;
				vmfree = vmtmp;
				vmtmp = vmtmp->next;
				free_user(vmfree);
			}
			if (saved_messages > 0) {
				/* give confirmation that the message was saved */
				/* commented out since we can't forward batches yet
				if (saved_messages == 1)
					res = opbx_play_and_wait(chan, "vm-message");
				else
					res = opbx_play_and_wait(chan, "vm-messages");
				if (!res)
					res = opbx_play_and_wait(chan, "vm-saved"); */
				if (!res)
					res = opbx_play_and_wait(chan, "vm-msgsaved");
			}	
		}
	}
	return res ? res : cmd;
}

static int wait_file2(struct opbx_channel *chan, struct vm_state *vms, char *file)
{
	int res;
	if ((res = opbx_streamfile(chan, file, chan->language))) 
		opbx_log(LOG_WARNING, "Unable to play message %s\n", file); 
	if (!res)
		res = opbx_waitstream(chan, OPBX_DIGIT_ANY);
	return res;
}

static int wait_file(struct opbx_channel *chan, struct vm_state *vms, char *file) 
{
	return opbx_control_streamfile(chan, file, "#", "*", "1456789", "0", "2", skipms);
}

static int play_message_category(struct opbx_channel *chan, char *category)
{
	int res = 0;

	if (!opbx_strlen_zero(category))
		res = opbx_play_and_wait(chan, category);

	return res;
}

static int play_message_datetime(struct opbx_channel *chan, struct opbx_vm_user *vmu, char *origtime, char *filename)
{
	int res = 0;
	struct vm_zone *the_zone = NULL;
	time_t t;
	long tin;

	if (sscanf(origtime,"%ld",&tin) < 1) {
		opbx_log(LOG_WARNING, "Couldn't find origtime in %s\n", filename);
		return 0;
	}
	t = tin;

	/* Does this user have a timezone specified? */
	if (!opbx_strlen_zero(vmu->zonetag)) {
		/* Find the zone in the list */
		struct vm_zone *z;
		z = zones;
		while (z) {
			if (!strcmp(z->name, vmu->zonetag)) {
				the_zone = z;
				break;
			}
			z = z->next;
		}
	}

/* No internal variable parsing for now, so we'll comment it out for the time being */
#if 0
	/* Set the DIFF_* variables */
	localtime_r(&t, &time_now);
	tv_now = opbx_tvnow();
	tnow = tv_now.tv_sec;
	localtime_r(&tnow,&time_then);

	/* Day difference */
	if (time_now.tm_year == time_then.tm_year)
		snprintf(temp,sizeof(temp),"%d",time_now.tm_yday);
	else
		snprintf(temp,sizeof(temp),"%d",(time_now.tm_year - time_then.tm_year) * 365 + (time_now.tm_yday - time_then.tm_yday));
	pbx_builtin_setvar_helper(chan, "DIFF_DAY", temp);

	/* Can't think of how other diffs might be helpful, but I'm sure somebody will think of something. */
#endif
	if (the_zone)
		res = opbx_say_date_with_format(chan, t, OPBX_DIGIT_ANY, chan->language, the_zone->msg_format, the_zone->timezone);
       else if(!strcasecmp(chan->language,"se"))       /* SWEDISH syntax */
               res = opbx_say_date_with_format(chan, t, OPBX_DIGIT_ANY, chan->language, "'vm-received' dB 'digits/at' k 'and' M", NULL);
       else if(!strcasecmp(chan->language,"no"))       /* NORWEGIAN syntax */
               res = opbx_say_date_with_format(chan, t, OPBX_DIGIT_ANY, chan->language, "'vm-received' Q 'digits/at' HM", NULL);
	else if(!strcasecmp(chan->language,"de"))	/* GERMAN syntax */
		res = opbx_say_date_with_format(chan, t, OPBX_DIGIT_ANY, chan->language, "'vm-received' Q 'digits/at' HM", NULL);
	else if (!strcasecmp(chan->language,"nl"))	/* DUTCH syntax */
		res = opbx_say_date_with_format(chan, t, OPBX_DIGIT_ANY, chan->language, "'vm-received' q 'digits/nl-om' HM", NULL);
 	else if (!strcasecmp(chan->language,"it"))      /* ITALIAN syntax */
		res = opbx_say_date_with_format(chan, t, OPBX_DIGIT_ANY, chan->language, "'vm-received' q 'digits/at' 'digits/hours' k 'digits/e' M 'digits/minutes'", NULL);
	else if (!strcasecmp(chan->language,"gr"))
		res = opbx_say_date_with_format(chan, t, OPBX_DIGIT_ANY, chan->language, "'vm-received' q  H 'digits/kai' M ", NULL);
	else
		res = opbx_say_date_with_format(chan, t, OPBX_DIGIT_ANY, chan->language, "'vm-received' q 'digits/at' IMp", NULL);
#if 0
	pbx_builtin_setvar_helper(chan, "DIFF_DAY", NULL);
#endif
	return res;
}



static int play_message_callerid(struct opbx_channel *chan, struct vm_state *vms, char *cid, char *context, int callback)
{
	int res = 0;
	int i;
	char *callerid, *name;
	char prefile[256]="";
	

	/* If voicemail cid is not enabled, or we didn't get cid or context from the attribute file, leave now. */
	/* BB: Still need to change this so that if this function is called by the message envelope (and someone is explicitly requesting to hear the CID), it does not check to see if CID is enabled in the config file */
	if ((cid == NULL)||(context == NULL))
		return res;

	/* Strip off caller ID number from name */
	opbx_log(LOG_DEBUG, "VM-CID: composite caller ID received: %s, context: %s\n", cid, context);
	opbx_callerid_parse(cid, &name, &callerid);
	if ((callerid != NULL)&&(!res)&&(!opbx_strlen_zero(callerid))){
		/* Check for internal contexts and only */
		/* say extension when the call didn't come from an internal context in the list */
		for (i = 0 ; i < MAX_NUM_CID_CONTEXTS ; i++){
			opbx_log(LOG_DEBUG, "VM-CID: comparing internalcontext: %s\n", cidinternalcontexts[i]);
			if ((strcmp(cidinternalcontexts[i], context) == 0))
				break;
		}
		if (i != MAX_NUM_CID_CONTEXTS){ /* internal context? */
			if (!res) {
				snprintf(prefile, sizeof(prefile), "%s%s/%s/greet", VM_SPOOL_DIR, context, callerid);
				if (!opbx_strlen_zero(prefile)) {
				/* See if we can find a recorded name for this person instead of their extension number */
					if (opbx_fileexists(prefile, NULL, NULL) > 0) {
						opbx_verbose(VERBOSE_PREFIX_3 "Playing envelope info: CID number '%s' matches mailbox number, playing recorded name\n", callerid);
						if (!callback)
							res = wait_file2(chan, vms, "vm-from");
						res = opbx_streamfile(chan, prefile, chan->language) > -1;
						res = opbx_waitstream(chan, "");
					} else {
						opbx_verbose(VERBOSE_PREFIX_3 "Playing envelope info: message from '%s'\n", callerid);
						/* BB: Say "from extension" as one saying to sound smoother */
						if (!callback)
							res = wait_file2(chan, vms, "vm-from-extension");
						res = opbx_say_digit_str(chan, callerid, "", chan->language);
					}
				}
			}
		}

		else if (!res){
			opbx_log(LOG_DEBUG, "VM-CID: Numeric caller id: (%s)\n",callerid);
			/* BB: Since this is all nicely figured out, why not say "from phone number" in this case" */
			if (!callback)
				res = wait_file2(chan, vms, "vm-from-phonenumber");
			res = opbx_say_digit_str(chan, callerid, OPBX_DIGIT_ANY, chan->language);
		}
	} else {
		/* Number unknown */
		opbx_log(LOG_DEBUG, "VM-CID: From an unknown number\n");
		if (!res)
			/* BB: Say "from an unknown caller" as one phrase - it is already recorded by "the voice" anyhow */
			res = wait_file2(chan, vms, "vm-unknown-caller");
	}
	return res;
}

static int play_message_duration(struct opbx_channel *chan, struct vm_state *vms, char *duration, int minduration)
{
	int res = 0;
	int durationm;
	int durations;
	/* Verify that we have a duration for the message */
	if((duration == NULL))
		return res;

	/* Convert from seconds to minutes */
	durations=atoi(duration);
	durationm=(durations / 60);

	opbx_log(LOG_DEBUG, "VM-Duration: duration is: %d seconds converted to: %d minutes\n", durations, durationm);

	if((!res)&&(durationm>=minduration)) {
		res = opbx_say_number(chan, durationm, OPBX_DIGIT_ANY, chan->language, (char *) NULL);
		res = wait_file2(chan, vms, "vm-minutes");
	}
	return res;
}

static int play_message(struct opbx_channel *chan, struct opbx_vm_user *vmu, struct vm_state *vms)
{
	int res = 0;
	char filename[256],*origtime, *cid, *context, *duration;
	char *category;
	struct opbx_config *msg_cfg;

	vms->starting = 0; 
	make_file(vms->fn, sizeof(vms->fn), vms->curdir, vms->curmsg);
	adsi_message(chan, vms);
	if (!vms->curmsg)
		res = wait_file2(chan, vms, "vm-first");	/* "First" */
	else if (vms->curmsg == vms->lastmsg)
		res = wait_file2(chan, vms, "vm-last");		/* "last" */
	if (!res) {
               if (!strcasecmp(chan->language, "se")) {             /* SWEDISH syntax */
                       res = wait_file2(chan, vms, "vm-meddelandet");  /* "message" */
               }
               else {
                       res = wait_file2(chan, vms, "vm-message");      /* "message" */
               }
		if (vms->curmsg && (vms->curmsg != vms->lastmsg)) {
			if (!res)
				res = opbx_say_number(chan, vms->curmsg + 1, OPBX_DIGIT_ANY, chan->language, (char *) NULL);
		}
	}

	/* Retrieve info from VM attribute file */
	make_file(vms->fn2, sizeof(vms->fn2), vms->curdir, vms->curmsg);
	snprintf(filename,sizeof(filename), "%s.txt", vms->fn2);
	RETRIEVE(vms->curdir, vms->curmsg);
	msg_cfg = opbx_config_load(filename);
	if (!msg_cfg) {
		opbx_log(LOG_WARNING, "No message attribute file?!! (%s)\n", filename);
		return 0;
	}
																									
	if (!(origtime = opbx_variable_retrieve(msg_cfg, "message", "origtime"))) {
		opbx_log(LOG_WARNING, "No origtime?!\n");
		DISPOSE(vms->curdir, vms->curmsg);
		opbx_config_destroy(msg_cfg);
		return 0;
	}

	cid = opbx_variable_retrieve(msg_cfg, "message", "callerid");
	duration = opbx_variable_retrieve(msg_cfg, "message", "duration");
	category = opbx_variable_retrieve(msg_cfg, "message", "category");

	context = opbx_variable_retrieve(msg_cfg, "message", "context");
	if (!strncasecmp("macro",context,5)) /* Macro names in contexts are useless for our needs */
		context = opbx_variable_retrieve(msg_cfg, "message","macrocontext");

	if (!res)
		res = play_message_category(chan, category);
	if ((!res) && (opbx_test_flag(vmu, VM_ENVELOPE)))
		res = play_message_datetime(chan, vmu, origtime, filename);
	if ((!res) && (opbx_test_flag(vmu, VM_SAYCID)))
		res = play_message_callerid(chan, vms, cid, context, 0);
        if ((!res) && (opbx_test_flag(vmu, VM_SAYDURATION)))
                res = play_message_duration(chan, vms, duration, vmu->saydurationm);
	/* Allow pressing '1' to skip envelope / callerid */
	if (res == '1')
		res = 0;
	opbx_config_destroy(msg_cfg);

	if (!res) {
		make_file(vms->fn, sizeof(vms->fn), vms->curdir, vms->curmsg);
		vms->heard[vms->curmsg] = 1;
		printf("yay!\n");
		res = wait_file(chan, vms, vms->fn);
	}
	DISPOSE(vms->curdir, vms->curmsg);
	return res;
}

static int open_mailbox(struct vm_state *vms, struct opbx_vm_user *vmu,int box)
{
	int res = 0;
	int count_msg, lopbx_msg;

	opbx_copy_string(vms->curbox, mbox(box), sizeof(vms->curbox));
	
	/* Rename the member vmbox HERE so that we don't try to return before
	 * we know what's going on.
	 */
	snprintf(vms->vmbox, sizeof(vms->vmbox), "vm-%s", vms->curbox);
	
	make_dir(vms->curdir, sizeof(vms->curdir), vmu->context, vms->username, vms->curbox);
	count_msg = count_messages(vmu, vms->curdir);
	if (count_msg < 0)
		return count_msg;
	else
		vms->lastmsg = count_msg - 1;

	/*
	The following test is needed in case sequencing gets messed up.
	There appears to be more than one way to mess up sequence, so
	we will not try to find all of the root causes--just fix it when
	detected.
	*/

	lopbx_msg = lopbx_message_index(vmu, vms->curdir);
	if (lopbx_msg < 0)
		return lopbx_msg;
	else if(vms->lastmsg != lopbx_msg)
	{
		opbx_log(LOG_NOTICE, "Resequencing Mailbox: %s\n", vms->curdir);
		res = resequence_mailbox(vmu, vms->curdir);
		if (res)
			return res;
	}

	return 0;
}

static int close_mailbox(struct vm_state *vms, struct opbx_vm_user *vmu)
{
	int x;
	int res = 0;

	if (vms->lastmsg <= -1)
		goto done;

	/* Get the deleted messages fixed */ 
	if (vm_lock_path(vms->curdir))
		return ERROR_LOCK_PATH;
	
	vms->curmsg = -1; 
	for (x=0;x < vmu->maxmsg;x++) { 
		if (!vms->deleted[x] && (strcasecmp(vms->curbox, "INBOX") || !vms->heard[x])) { 
			/* Save this message.  It's not in INBOX or hasn't been heard */ 
			make_file(vms->fn, sizeof(vms->fn), vms->curdir, x); 
			if (!EXISTS(vms->curdir, x, vms->fn, NULL)) 
				break;
			vms->curmsg++; 
			make_file(vms->fn2, sizeof(vms->fn2), vms->curdir, vms->curmsg); 
			if (strcmp(vms->fn, vms->fn2)) { 
				RENAME(vms->curdir, x, vmu->mailbox,vmu->context, vms->curdir, vms->curmsg, vms->fn, vms->fn2);
			} 
		} else if (!strcasecmp(vms->curbox, "INBOX") && vms->heard[x] && !vms->deleted[x]) { 
			/* Move to old folder before deleting */ 
			res = save_to_folder(vmu, vms->curdir, x, vmu->context, vms->username, 1);
			if (res == ERROR_LOCK_PATH) {
				/* If save failed do not delete the message */
				vms->deleted[x] = 0;
				vms->heard[x] = 0;
				--x;
			} 
		} 
	} 
	for (x = vms->curmsg + 1; x <= vmu->maxmsg; x++) { 
		make_file(vms->fn, sizeof(vms->fn), vms->curdir, x); 
		if (!EXISTS(vms->curdir, x, vms->fn, NULL)) 
			break;
		DELETE(vms->curdir, x, vms->fn);
	} 
	opbx_unlock_path(vms->curdir);

done:
	if (vms->deleted)
		memset(vms->deleted, 0, sizeof(vms->deleted)); 
	if (vms->heard)
		memset(vms->heard, 0, sizeof(vms->heard)); 

	return 0;
}

/* In Greek even though we CAN use a syntax like "friends messages"
 * ("filika mynhmata") it is not elegant. This also goes for "work/family messages"
 * ("ergasiaka/oikogeniaka mynhmata"). Therefore it is better to use a reversed 
 * syntax for the above three categories which is more elegant. 
*/

static int vm_play_folder_name_gr(struct opbx_channel *chan, char *mbox)
{
	int cmd;
	char buf[sizeof(mbox)+1]; 

	memset(buf, '\0', sizeof(char)*(sizeof(buf)));
	strcpy(buf, mbox);
	strcat(buf,"s");

	if (!strcasecmp(mbox, "vm-INBOX") || !strcasecmp(mbox, "vm-Old")){
		cmd = opbx_play_and_wait(chan, buf); /* "NEA / PALIA" */
		if (cmd)
		return cmd;
		return opbx_play_and_wait(chan, "vm-messages"); /* "messages" -> "MYNHMATA" */
	} else {
		cmd = opbx_play_and_wait(chan, "vm-messages"); /* "messages" -> "MYNHMATA" */
	  	if (cmd)
			return cmd;
	  	return opbx_play_and_wait(chan, mbox); /* friends/family/work... -> "FILWN"/"OIKOGENIAS"/"DOULEIAS"*/
	}
}

static int vm_play_folder_name(struct opbx_channel *chan, char *mbox)
{
	int cmd;

	if (!strcasecmp(chan->language, "it") || !strcasecmp(chan->language, "es") || !strcasecmp(chan->language, "fr") || !strcasecmp(chan->language, "pt")) { /* Italian, Spanish, French or Portuguese syntax */
		cmd = opbx_play_and_wait(chan, "vm-messages"); /* "messages */
		if (cmd)
			return cmd;
		return opbx_play_and_wait(chan, mbox);
	} else if (!strcasecmp(chan->language, "gr")){
		return vm_play_folder_name_gr(chan, mbox);
	} else {  /* Default English */
		cmd = opbx_play_and_wait(chan, mbox);
		if (cmd)
			return cmd;
		return opbx_play_and_wait(chan, "vm-messages"); /* "messages */
	}
}

 /* GREEK SYNTAX 
	In greek the plural for old/new is
	different so we need the following files	 
	We also need vm-denExeteMynhmata because 
	this syntax is different.
	
	-> vm-Olds.wav	: "Palia"
	-> vm-INBOXs.wav : "Nea"
	-> vm-denExeteMynhmata : "den exete mynhmata"
 */
					
	
static int vm_intro_gr(struct opbx_channel *chan, struct vm_state *vms)
{
	int res = 0;

	if (vms->newmessages) {
		res = opbx_play_and_wait(chan, "vm-youhave");
		if (!res) 
			res = opbx_say_number(chan, vms->newmessages, OPBX_DIGIT_ANY, chan->language, NULL);
		if (!res) {
			if ((vms->newmessages == 1)) {
				res = opbx_play_and_wait(chan, "vm-INBOX");
				if (!res)
			 		res = opbx_play_and_wait(chan, "vm-message");
		 	} else {
				res = opbx_play_and_wait(chan, "vm-INBOXs");
				if (!res)
					res = opbx_play_and_wait(chan, "vm-messages");
		 	}
		}	  
	} else if (vms->oldmessages){
		res = opbx_play_and_wait(chan, "vm-youhave");
		if (!res)
			res = opbx_say_number(chan, vms->oldmessages, OPBX_DIGIT_ANY, chan->language, NULL);
		if ((vms->oldmessages == 1)){
			res = opbx_play_and_wait(chan, "vm-Old");
			if (!res)
				res = opbx_play_and_wait(chan, "vm-message");
		} else {
			res = opbx_play_and_wait(chan, "vm-Olds");
		 	if (!res)
				res = opbx_play_and_wait(chan, "vm-messages");
		}
	 } else if (!vms->oldmessages && !vms->newmessages) 
			res = opbx_play_and_wait(chan, "vm-denExeteMynhmata"); 
	 return res;
}
	
/* Default English syntax */
static int vm_intro_en(struct opbx_channel *chan,struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	res = opbx_play_and_wait(chan, "vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			res = say_and_wait(chan, vms->newmessages, chan->language);
			if (!res)
				res = opbx_play_and_wait(chan, "vm-INBOX");
			if (vms->oldmessages && !res)
				res = opbx_play_and_wait(chan, "vm-and");
			else if (!res) {
				if ((vms->newmessages == 1))
					res = opbx_play_and_wait(chan, "vm-message");
				else
					res = opbx_play_and_wait(chan, "vm-messages");
			}
				
		}
		if (!res && vms->oldmessages) {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			if (!res)
				res = opbx_play_and_wait(chan, "vm-Old");
			if (!res) {
				if (vms->oldmessages == 1)
					res = opbx_play_and_wait(chan, "vm-message");
				else
					res = opbx_play_and_wait(chan, "vm-messages");
			}
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = opbx_play_and_wait(chan, "vm-no");
				if (!res)
					res = opbx_play_and_wait(chan, "vm-messages");
			}
		}
	}
	return res;
}

/* ITALIAN syntax */
static int vm_intro_it(struct opbx_channel *chan, struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	if (!vms->oldmessages && !vms->newmessages)
		res =	opbx_play_and_wait(chan, "vm-no") ||
			opbx_play_and_wait(chan, "vm-message");
	else
		res =	opbx_play_and_wait(chan, "vm-youhave");
	if (!res && vms->newmessages) {
		res = (vms->newmessages == 1) ?
			opbx_play_and_wait(chan, "digits/un") ||
			opbx_play_and_wait(chan, "vm-nuovo") ||
			opbx_play_and_wait(chan, "vm-message") :
			/* 2 or more new messages */
			say_and_wait(chan, vms->newmessages, chan->language) ||
			opbx_play_and_wait(chan, "vm-nuovi") ||
			opbx_play_and_wait(chan, "vm-messages");
		if (!res && vms->oldmessages)
			res =	opbx_play_and_wait(chan, "vm-and");
	}
	if (!res && vms->oldmessages) {
		res = (vms->oldmessages == 1) ?
			opbx_play_and_wait(chan, "digits/un") ||
			opbx_play_and_wait(chan, "vm-vecchio") ||
			opbx_play_and_wait(chan, "vm-message") :
			/* 2 or more old messages */
			say_and_wait(chan, vms->oldmessages, chan->language) ||
			opbx_play_and_wait(chan, "vm-vecchi") ||
			opbx_play_and_wait(chan, "vm-messages");
	}
	return res ? -1 : 0;
}

/* SWEDISH syntax */
static int vm_intro_se(struct opbx_channel *chan, struct vm_state *vms)
{
        /* Introduce messages they have */
        int res;

	res = opbx_play_and_wait(chan, "vm-youhave");
	if (res)
		return res;

        if (!vms->oldmessages && !vms->newmessages) {
		res = opbx_play_and_wait(chan, "vm-no");
		res = res ? res : opbx_play_and_wait(chan, "vm-messages");
		return res;
        }

	if (vms->newmessages) {
		if ((vms->newmessages == 1)) {
			res = opbx_play_and_wait(chan, "digits/ett");
			res = res ? res : opbx_play_and_wait(chan, "vm-nytt");
			res = res ? res : opbx_play_and_wait(chan, "vm-message");
		} else {
			res = say_and_wait(chan, vms->newmessages, chan->language);
			res = res ? res : opbx_play_and_wait(chan, "vm-nya");
			res = res ? res : opbx_play_and_wait(chan, "vm-messages");
		}
		if (!res && vms->oldmessages)
			res = opbx_play_and_wait(chan, "vm-and");
	}
	if (!res && vms->oldmessages) {
		if (vms->oldmessages == 1) {
			res = opbx_play_and_wait(chan, "digits/ett");
			res = res ? res : opbx_play_and_wait(chan, "vm-gammalt");
			res = res ? res : opbx_play_and_wait(chan, "vm-message");
		} else {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			res = res ? res : opbx_play_and_wait(chan, "vm-gamla");
			res = res ? res : opbx_play_and_wait(chan, "vm-messages");
		}
	}

	return res;
}

/* NORWEGIAN syntax */
static int vm_intro_no(struct opbx_channel *chan,struct vm_state *vms)
{
        /* Introduce messages they have */
        int res;

	res = opbx_play_and_wait(chan, "vm-youhave");
	if (res)
		return res;

        if (!vms->oldmessages && !vms->newmessages) {
		res = opbx_play_and_wait(chan, "vm-no");
		res = res ? res : opbx_play_and_wait(chan, "vm-messages");
		return res;
        }

	if (vms->newmessages) {
		if ((vms->newmessages == 1)) {
			res = opbx_play_and_wait(chan, "digits/1");
			res = res ? res : opbx_play_and_wait(chan, "vm-ny");
			res = res ? res : opbx_play_and_wait(chan, "vm-message");
		} else {
			res = say_and_wait(chan, vms->newmessages, chan->language);
			res = res ? res : opbx_play_and_wait(chan, "vm-nye");
			res = res ? res : opbx_play_and_wait(chan, "vm-messages");
		}
		if (!res && vms->oldmessages)
			res = opbx_play_and_wait(chan, "vm-and");
	}
	if (!res && vms->oldmessages) {
		if (vms->oldmessages == 1) {
			res = opbx_play_and_wait(chan, "digits/1");
			res = res ? res : opbx_play_and_wait(chan, "vm-gamel");
			res = res ? res : opbx_play_and_wait(chan, "vm-message");
		} else {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			res = res ? res : opbx_play_and_wait(chan, "vm-gamle");
			res = res ? res : opbx_play_and_wait(chan, "vm-messages");
		}
	}

	return res;
}

/* GERMAN syntax */
static int vm_intro_de(struct opbx_channel *chan,struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	res = opbx_play_and_wait(chan, "vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			if ((vms->newmessages == 1))
				res = opbx_play_and_wait(chan, "digits/1F");
			else
				res = say_and_wait(chan, vms->newmessages, chan->language);
			if (!res)
				res = opbx_play_and_wait(chan, "vm-INBOX");
			if (vms->oldmessages && !res)
				res = opbx_play_and_wait(chan, "vm-and");
			else if (!res) {
				if ((vms->newmessages == 1))
					res = opbx_play_and_wait(chan, "vm-message");
				else
					res = opbx_play_and_wait(chan, "vm-messages");
			}
				
		}
		if (!res && vms->oldmessages) {
			if (vms->oldmessages == 1)
				res = opbx_play_and_wait(chan, "digits/1F");
			else
				res = say_and_wait(chan, vms->oldmessages, chan->language);
			if (!res)
				res = opbx_play_and_wait(chan, "vm-Old");
			if (!res) {
				if (vms->oldmessages == 1)
					res = opbx_play_and_wait(chan, "vm-message");
				else
					res = opbx_play_and_wait(chan, "vm-messages");
			}
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = opbx_play_and_wait(chan, "vm-no");
				if (!res)
					res = opbx_play_and_wait(chan, "vm-messages");
			}
		}
	}
	return res;
}

/* SPANISH syntax */
static int vm_intro_es(struct opbx_channel *chan,struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	if (!vms->oldmessages && !vms->newmessages) {
		res = opbx_play_and_wait(chan, "vm-youhaveno");
		if (!res)
			res = opbx_play_and_wait(chan, "vm-messages");
	} else {
		res = opbx_play_and_wait(chan, "vm-youhave");
	}
	if (!res) {
		if (vms->newmessages) {
			if (!res) {
				if ((vms->newmessages == 1)) {
					res = opbx_play_and_wait(chan, "digits/1M");
					if (!res)
						res = opbx_play_and_wait(chan, "vm-message");
					if (!res)
						res = opbx_play_and_wait(chan, "vm-INBOXs");
				} else {
					res = say_and_wait(chan, vms->newmessages, chan->language);
					if (!res)
						res = opbx_play_and_wait(chan, "vm-messages");
					if (!res)
						res = opbx_play_and_wait(chan, "vm-INBOX");
				}
			}
			if (vms->oldmessages && !res)
				res = opbx_play_and_wait(chan, "vm-and");
		}
		if (vms->oldmessages) {
			if (!res) {
				if (vms->oldmessages == 1) {
					res = opbx_play_and_wait(chan, "digits/1M");
					if (!res)
						res = opbx_play_and_wait(chan, "vm-message");
					if (!res)
						res = opbx_play_and_wait(chan, "vm-Olds");
				} else {
					res = say_and_wait(chan, vms->oldmessages, chan->language);
					if (!res)
						res = opbx_play_and_wait(chan, "vm-messages");
					if (!res)
						res = opbx_play_and_wait(chan, "vm-Old");
				}
			}
		}
	}
return res;
}

/* FRENCH syntax */
static int vm_intro_fr(struct opbx_channel *chan,struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	res = opbx_play_and_wait(chan, "vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			res = say_and_wait(chan, vms->newmessages, chan->language);
			if (!res)
				res = opbx_play_and_wait(chan, "vm-INBOX");
			if (vms->oldmessages && !res)
				res = opbx_play_and_wait(chan, "vm-and");
			else if (!res) {
				if ((vms->newmessages == 1))
					res = opbx_play_and_wait(chan, "vm-message");
				else
					res = opbx_play_and_wait(chan, "vm-messages");
			}
				
		}
		if (!res && vms->oldmessages) {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			if (!res) {
				if (vms->oldmessages == 1)
					res = opbx_play_and_wait(chan, "vm-message");
				else
					res = opbx_play_and_wait(chan, "vm-messages");
			}
			if (!res)
				res = opbx_play_and_wait(chan, "vm-Old");
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = opbx_play_and_wait(chan, "vm-no");
				if (!res)
					res = opbx_play_and_wait(chan, "vm-messages");
			}
		}
	}
	return res;
}

/* DUTCH syntax */
static int vm_intro_nl(struct opbx_channel *chan,struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	res = opbx_play_and_wait(chan, "vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			res = say_and_wait(chan, vms->newmessages, chan->language);
			if (!res) {
				if (vms->oldmessages == 1)
					res = opbx_play_and_wait(chan, "vm-INBOXs");
				else
					res = opbx_play_and_wait(chan, "vm-INBOX");
			}
			if (vms->oldmessages && !res)
				res = opbx_play_and_wait(chan, "vm-and");
			else if (!res) {
				if ((vms->newmessages == 1))
					res = opbx_play_and_wait(chan, "vm-message");
				else
					res = opbx_play_and_wait(chan, "vm-messages");
			}
				
		}
		if (!res && vms->oldmessages) {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			if (!res) {
				if (vms->oldmessages == 1)
					res = opbx_play_and_wait(chan, "vm-Olds");
				else
					res = opbx_play_and_wait(chan, "vm-Old");
			}
			if (!res) {
				if (vms->oldmessages == 1)
					res = opbx_play_and_wait(chan, "vm-message");
				else
					res = opbx_play_and_wait(chan, "vm-messages");
			}
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = opbx_play_and_wait(chan, "vm-no");
				if (!res)
					res = opbx_play_and_wait(chan, "vm-messages");
			}
		}
	}
	return res;
}

/* PORTUGUESE syntax */
static int vm_intro_pt(struct opbx_channel *chan,struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	res = opbx_play_and_wait(chan, "vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			res = opbx_say_number(chan, vms->newmessages, OPBX_DIGIT_ANY, chan->language, "f");
			if (!res) {
				if ((vms->newmessages == 1)) {
					res = opbx_play_and_wait(chan, "vm-message");
					if (!res)
						res = opbx_play_and_wait(chan, "vm-INBOXs");
				} else {
					res = opbx_play_and_wait(chan, "vm-messages");
					if (!res)
						res = opbx_play_and_wait(chan, "vm-INBOX");
				}
			}
			if (vms->oldmessages && !res)
				res = opbx_play_and_wait(chan, "vm-and");
		}
		if (!res && vms->oldmessages) {
			res = opbx_say_number(chan, vms->oldmessages, OPBX_DIGIT_ANY, chan->language, "f");
			if (!res) {
				if (vms->oldmessages == 1) {
					res = opbx_play_and_wait(chan, "vm-message");
					if (!res)
						res = opbx_play_and_wait(chan, "vm-Olds");
				} else {
					res = opbx_play_and_wait(chan, "vm-messages");
					if (!res)
						res = opbx_play_and_wait(chan, "vm-Old");
				}
			}
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = opbx_play_and_wait(chan, "vm-no");
				if (!res)
					res = opbx_play_and_wait(chan, "vm-messages");
			}
		}
	}
	return res;
}


/* CZECH syntax */
/* in czech there must be declension of word new and message
 * czech   	: english 	   : czech	: english
 * --------------------------------------------------------
 * vm-youhave 	: you have 
 * vm-novou 	: one new 	   : vm-zpravu 	: message
 * vm-nove 	: 2-4 new	   : vm-zpravy 	: messages
 * vm-novych 	: 5-infinite new   : vm-zprav 	: messages
 * vm-starou	: one old
 * vm-stare	: 2-4 old 
 * vm-starych	: 5-infinite old
 * jednu	: one	- falling 4. 
 * vm-no	: no  ( no messages )
 */

static int vm_intro_cz(struct opbx_channel *chan,struct vm_state *vms)
{
	int res;
	res = opbx_play_and_wait(chan, "vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			if (vms->newmessages == 1) {
				res = opbx_play_and_wait(chan, "digits/jednu");
			} else {
				res = say_and_wait(chan, vms->newmessages, chan->language);
			}
			if (!res) {
				if ((vms->newmessages == 1))
					res = opbx_play_and_wait(chan, "vm-novou");
				if ((vms->newmessages) > 1 && (vms->newmessages < 5))
					res = opbx_play_and_wait(chan, "vm-nove");
				if (vms->newmessages > 4)
					res = opbx_play_and_wait(chan, "vm-novych");
			}
			if (vms->oldmessages && !res)
				res = opbx_play_and_wait(chan, "vm-and");
			else if (!res) {
				if ((vms->newmessages == 1))
					res = opbx_play_and_wait(chan, "vm-zpravu");
				if ((vms->newmessages) > 1 && (vms->newmessages < 5))
					res = opbx_play_and_wait(chan, "vm-zpravy");
				if (vms->newmessages > 4)
					res = opbx_play_and_wait(chan, "vm-zprav");
			}
		}
		if (!res && vms->oldmessages) {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			if (!res) {
				if ((vms->oldmessages == 1))
					res = opbx_play_and_wait(chan, "vm-starou");
				if ((vms->oldmessages) > 1 && (vms->oldmessages < 5))
					res = opbx_play_and_wait(chan, "vm-stare");
				if (vms->oldmessages > 4)
					res = opbx_play_and_wait(chan, "vm-starych");
			}
			if (!res) {
				if ((vms->oldmessages == 1))
					res = opbx_play_and_wait(chan, "vm-zpravu");
				if ((vms->oldmessages) > 1 && (vms->oldmessages < 5))
					res = opbx_play_and_wait(chan, "vm-zpravy");
				if (vms->oldmessages > 4)
					res = opbx_play_and_wait(chan, "vm-zprav");
			}
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = opbx_play_and_wait(chan, "vm-no");
				if (!res)
					res = opbx_play_and_wait(chan, "vm-zpravy");
			}
		}
	}
	return res;
}

static int vm_intro(struct opbx_channel *chan,struct vm_state *vms)
{
	/* Play voicemail intro - syntax is different for different languages */
	if (!strcasecmp(chan->language, "de")) {	/* GERMAN syntax */
		return vm_intro_de(chan, vms);
	} else if (!strcasecmp(chan->language, "es")) { /* SPANISH syntax */
		return vm_intro_es(chan, vms);
	} else if (!strcasecmp(chan->language, "it")) { /* ITALIAN syntax */
		return vm_intro_it(chan, vms);
	} else if (!strcasecmp(chan->language, "fr")) {	/* FRENCH syntax */
		return vm_intro_fr(chan, vms);
	} else if (!strcasecmp(chan->language, "nl")) {	/* DUTCH syntax */
		return vm_intro_nl(chan, vms);
	} else if (!strcasecmp(chan->language, "pt")) {	/* PORTUGUESE syntax */
		return vm_intro_pt(chan, vms);
	} else if (!strcasecmp(chan->language, "cz")) {	/* CZECH syntax */
		return vm_intro_cz(chan, vms);
	} else if (!strcasecmp(chan->language, "gr")) {	/* GREEK syntax */
		return vm_intro_gr(chan, vms);
	} else if (!strcasecmp(chan->language, "se")) {	/* SWEDISH syntax */
		return vm_intro_se(chan, vms);
	} else if (!strcasecmp(chan->language, "no")) {	/* NORWEGIAN syntax */
		return vm_intro_no(chan, vms);
	} else {					/* Default to ENGLISH */
		return vm_intro_en(chan, vms);
	}
}

static int vm_instructions(struct opbx_channel *chan, struct vm_state *vms, int skipadvanced)
{
	int res = 0;
	/* Play instructions and wait for new command */
	while (!res) {
		if (vms->starting) {
			if (vms->lastmsg > -1) {
				res = opbx_play_and_wait(chan, "vm-onefor");
				if (!res)
					res = vm_play_folder_name(chan, vms->vmbox);
			}
			if (!res)
				res = opbx_play_and_wait(chan, "vm-opts");
		} else {
			if (vms->curmsg)
				res = opbx_play_and_wait(chan, "vm-prev");
			if (!res && !skipadvanced)
				res = opbx_play_and_wait(chan, "vm-advopts");
			if (!res)
				res = opbx_play_and_wait(chan, "vm-repeat");
			if (!res && (vms->curmsg != vms->lastmsg))
				res = opbx_play_and_wait(chan, "vm-next");
			if (!res) {
				if (!vms->deleted[vms->curmsg])
					res = opbx_play_and_wait(chan, "vm-delete");
				else
					res = opbx_play_and_wait(chan, "vm-undelete");
				if (!res)
					res = opbx_play_and_wait(chan, "vm-toforward");
				if (!res)
					res = opbx_play_and_wait(chan, "vm-savemessage");
			}
		}
		if (!res)
			res = opbx_play_and_wait(chan, "vm-helpexit");
		if (!res)
			res = opbx_waitfordigit(chan, 6000);
		if (!res) {
			vms->repeats++;
			if (vms->repeats > 2) {
				res = 't';
			}
		}
	}
	return res;
}

static int vm_newuser(struct opbx_channel *chan, struct opbx_vm_user *vmu, struct vm_state *vms, char *fmtc, signed char record_gain)
{
	int cmd = 0;
	int duration = 0;
	char newpassword[80] = "";
	char newpassword2[80] = "";
	char prefile[256]="";
	unsigned char buf[256];
	int bytes=0;

	if (adsi_available(chan)) {
		bytes += adsi_logo(buf + bytes);
		bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "New User Setup", "");
		bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "Not Done", "");
		bytes += adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += adsi_voice_mode(buf + bytes, 0);
		adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}

	/* First, have the user change their password 
	   so they won't get here again */
	newpassword[1] = '\0';
	newpassword[0] = cmd = opbx_play_and_wait(chan,"vm-newpassword");
	if (cmd == '#')
		newpassword[0] = '\0';
	if (cmd < 0 || cmd == 't' || cmd == '#')
		return cmd;
	cmd = opbx_readstring(chan,newpassword + strlen(newpassword),sizeof(newpassword)-1,2000,10000,"#");
	if (cmd < 0 || cmd == 't' || cmd == '#')
		return cmd;
	newpassword2[1] = '\0';
	newpassword2[0] = cmd = opbx_play_and_wait(chan,"vm-reenterpassword");
	if (cmd == '#')
		newpassword2[0] = '\0';
	if (cmd < 0 || cmd == 't' || cmd == '#')
		return cmd;
	cmd = opbx_readstring(chan,newpassword2 + strlen(newpassword2),sizeof(newpassword2)-1,2000,10000,"#");
	if (cmd < 0 || cmd == 't' || cmd == '#')
		return cmd;
	if (strcmp(newpassword, newpassword2)) {
		opbx_log(LOG_NOTICE,"Password mismatch for user %s (%s != %s)\n", vms->username, newpassword, newpassword2);
		cmd = opbx_play_and_wait(chan, "vm-mismatch");
	}
	if (opbx_strlen_zero(ext_pass_cmd)) 
		vm_change_password(vmu,newpassword);
	else 
		vm_change_password_shell(vmu,newpassword);
	opbx_log(LOG_DEBUG,"User %s set password to %s of length %d\n",vms->username,newpassword,(int)strlen(newpassword));
	cmd = opbx_play_and_wait(chan,"vm-passchanged");

	/* If forcename is set, have the user record their name */	
	if (opbx_test_flag(vmu, VM_FORCENAME)) {
		snprintf(prefile,sizeof(prefile), "%s%s/%s/greet", VM_SPOOL_DIR, vmu->context, vms->username);
		cmd = play_record_review(chan,"vm-rec-name",prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain);
		if (cmd < 0 || cmd == 't' || cmd == '#')
			return cmd;
	}

	/* If forcegreetings is set, have the user record their greetings */
	if (opbx_test_flag(vmu, VM_FORCEGREET)) {
		snprintf(prefile,sizeof(prefile), "%s%s/%s/unavail", VM_SPOOL_DIR, vmu->context, vms->username);
		cmd = play_record_review(chan,"vm-rec-unv",prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain);
		if (cmd < 0 || cmd == 't' || cmd == '#')
			return cmd;
		snprintf(prefile,sizeof(prefile), "%s%s/%s/busy", VM_SPOOL_DIR, vmu->context, vms->username);
		cmd = play_record_review(chan,"vm-rec-busy",prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain);
		if (cmd < 0 || cmd == 't' || cmd == '#')
			return cmd;
	}

	return cmd;
}

static int vm_options(struct opbx_channel *chan, struct opbx_vm_user *vmu, struct vm_state *vms, char *fmtc, signed char record_gain)
{
	int cmd = 0;
	int retries = 0;
	int duration = 0;
	char newpassword[80] = "";
	char newpassword2[80] = "";
	char prefile[256]="";
	unsigned char buf[256];
	int bytes=0;

	if (adsi_available(chan))
	{
		bytes += adsi_logo(buf + bytes);
		bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Options Menu", "");
		bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "Not Done", "");
		bytes += adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += adsi_voice_mode(buf + bytes, 0);
		adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}
	while ((cmd >= 0) && (cmd != 't')) {
		if (cmd)
			retries = 0;
		switch (cmd) {
		case '1':
			snprintf(prefile,sizeof(prefile), "%s%s/%s/unavail", VM_SPOOL_DIR, vmu->context, vms->username);
			cmd = play_record_review(chan,"vm-rec-unv",prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain);
			break;
		case '2': 
			snprintf(prefile,sizeof(prefile), "%s%s/%s/busy", VM_SPOOL_DIR, vmu->context, vms->username);
			cmd = play_record_review(chan,"vm-rec-busy",prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain);
			break;
		case '3': 
			snprintf(prefile,sizeof(prefile), "%s%s/%s/greet", VM_SPOOL_DIR, vmu->context, vms->username);
			cmd = play_record_review(chan,"vm-rec-name",prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain);
			break;
		case '4': 
			cmd = vm_tempgreeting(chan, vmu, vms, fmtc, record_gain);
			break;
		case '5':
			if (vmu->password[0] == '-') {
				cmd = opbx_play_and_wait(chan, "vm-no");
				break;
			}
			newpassword[1] = '\0';
			newpassword[0] = cmd = opbx_play_and_wait(chan,"vm-newpassword");
			if (cmd == '#')
				newpassword[0] = '\0';
			else {
				if (cmd < 0)
					break;
				if ((cmd = opbx_readstring(chan,newpassword + strlen(newpassword),sizeof(newpassword)-1,2000,10000,"#")) < 0) {
					break;
				}
			}
			newpassword2[1] = '\0';
			newpassword2[0] = cmd = opbx_play_and_wait(chan,"vm-reenterpassword");
			if (cmd == '#')
				newpassword2[0] = '\0';
			else {
				if (cmd < 0)
					break;

				if ((cmd = opbx_readstring(chan,newpassword2 + strlen(newpassword2),sizeof(newpassword2)-1,2000,10000,"#"))) {
					break;
				}
			}
			if (strcmp(newpassword, newpassword2)) {
				opbx_log(LOG_NOTICE,"Password mismatch for user %s (%s != %s)\n", vms->username, newpassword, newpassword2);
				cmd = opbx_play_and_wait(chan, "vm-mismatch");
				break;
			}
			if (opbx_strlen_zero(ext_pass_cmd)) 
				vm_change_password(vmu,newpassword);
			else 
				vm_change_password_shell(vmu,newpassword);
			opbx_log(LOG_DEBUG,"User %s set password to %s of length %d\n",vms->username,newpassword,(int)strlen(newpassword));
			cmd = opbx_play_and_wait(chan,"vm-passchanged");
			break;
		case '*': 
			cmd = 't';
			break;
		default: 
			cmd = opbx_play_and_wait(chan,"vm-options");
			if (!cmd)
				cmd = opbx_waitfordigit(chan,6000);
			if (!cmd)
				retries++;
			if (retries > 3)
				cmd = 't';
		 }
	}
	if (cmd == 't')
		cmd = 0;
	return cmd;
}

static int vm_tempgreeting(struct opbx_channel *chan, struct opbx_vm_user *vmu, struct vm_state *vms, char *fmtc, signed char record_gain)
{
	int cmd = 0;
	int retries = 0;
	int duration = 0;
	char prefile[256]="";
	unsigned char buf[256];
	int bytes=0;

	if (adsi_available(chan))
	{
		bytes += adsi_logo(buf + bytes);
		bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Temp Greeting Menu", "");
		bytes += adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "Not Done", "");
		bytes += adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += adsi_voice_mode(buf + bytes, 0);
		adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}
	snprintf(prefile,sizeof(prefile), "%s%s/%s/temp", VM_SPOOL_DIR, vmu->context, vms->username);
	while((cmd >= 0) && (cmd != 't')) {
		if (cmd)
			retries = 0;
		if (opbx_fileexists(prefile, NULL, NULL) > 0) {
			switch (cmd) {
			case '1':
				cmd = play_record_review(chan,"vm-rec-temp",prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain);
				break;
			case '2':
				opbx_filedelete(prefile, NULL);
				opbx_play_and_wait(chan,"vm-tempremoved");
				cmd = 't';	
				break;
			case '*': 
				cmd = 't';
				break;
			default:
				if (opbx_fileexists(prefile, NULL, NULL) > 0) {
					cmd = opbx_play_and_wait(chan,"vm-tempgreeting2");
				} else {
					cmd = opbx_play_and_wait(chan,"vm-tempgreeting");
				} if (!cmd) {
					cmd = opbx_waitfordigit(chan,6000);
				} if (!cmd) {
					retries++;
				} if (retries > 3) {
					cmd = 't';
				}
			}
		} else {
			play_record_review(chan,"vm-rec-temp",prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain);
			cmd = 't';	
		}
	}
	if (cmd == 't')
		cmd = 0;
	return cmd;
}

/* GREEK SYNTAX */
	
static int vm_browse_messages_gr(struct opbx_channel *chan, struct vm_state *vms, struct opbx_vm_user *vmu)
{
	int cmd=0;

	if (vms->lastmsg > -1) {
		cmd = play_message(chan, vmu, vms);
	} else {
	 	cmd = opbx_play_and_wait(chan, "vm-youhaveno");
	 	if (!strcasecmp(vms->vmbox, "vm-INBOX") ||!strcasecmp(vms->vmbox, "vm-Old")){
			if (!cmd) {
		 		snprintf(vms->fn, sizeof(vms->fn), "vm-%ss", vms->curbox);
		 		cmd = opbx_play_and_wait(chan, vms->fn);
			}
			if (!cmd)
		 		cmd = opbx_play_and_wait(chan, "vm-messages");
	 	} else {
		 	if (!cmd)
				cmd = opbx_play_and_wait(chan, "vm-messages");
			if (!cmd) {
			 	snprintf(vms->fn, sizeof(vms->fn), "vm-%s", vms->curbox);
			 	cmd = opbx_play_and_wait(chan, vms->fn);
			}
		}
	} 
	return cmd;
}

/* Default English syntax */
static int vm_browse_messages_en(struct opbx_channel *chan, struct vm_state *vms, struct opbx_vm_user *vmu)
{
	int cmd=0;

	if (vms->lastmsg > -1) {
		cmd = play_message(chan, vmu, vms);
	} else {
		cmd = opbx_play_and_wait(chan, "vm-youhave");
		if (!cmd) 
			cmd = opbx_play_and_wait(chan, "vm-no");
		if (!cmd) {
			snprintf(vms->fn, sizeof(vms->fn), "vm-%s", vms->curbox);
			cmd = opbx_play_and_wait(chan, vms->fn);
		}
		if (!cmd)
			cmd = opbx_play_and_wait(chan, "vm-messages");
	}
	return cmd;
}

/* ITALIAN syntax */
static int vm_browse_messages_it(struct opbx_channel *chan, struct vm_state *vms, struct opbx_vm_user *vmu)
{
        int cmd=0;

        if (vms->lastmsg > -1) {
                cmd = play_message(chan, vmu, vms);
        } else {
                cmd = opbx_play_and_wait(chan, "vm-no");
                if (!cmd)
                        cmd = opbx_play_and_wait(chan, "vm-message");
                if (!cmd) {
                        snprintf(vms->fn, sizeof(vms->fn), "vm-%s", vms->curbox);
                        cmd = opbx_play_and_wait(chan, vms->fn);
                }
        }
        return cmd;
}

/* SPANISH syntax */
static int vm_browse_messages_es(struct opbx_channel *chan, struct vm_state *vms, struct opbx_vm_user *vmu)
{
	int cmd=0;

	if (vms->lastmsg > -1) {
		cmd = play_message(chan, vmu, vms);
	} else {
		cmd = opbx_play_and_wait(chan, "vm-youhaveno");
		if (!cmd)
			cmd = opbx_play_and_wait(chan, "vm-messages");
		if (!cmd) {
			snprintf(vms->fn, sizeof(vms->fn), "vm-%s", vms->curbox);
			cmd = opbx_play_and_wait(chan, vms->fn);
		}
	}
	return cmd;
}

/* PORTUGUESE syntax */
static int vm_browse_messages_pt(struct opbx_channel *chan, struct vm_state *vms, struct opbx_vm_user *vmu)
{
	int cmd=0;

	if (vms->lastmsg > -1) {
		cmd = play_message(chan, vmu, vms);
	} else {
		cmd = opbx_play_and_wait(chan, "vm-no");
		if (!cmd) {
			snprintf(vms->fn, sizeof(vms->fn), "vm-%s", vms->curbox);
			cmd = opbx_play_and_wait(chan, vms->fn);
		}
		if (!cmd)
			cmd = opbx_play_and_wait(chan, "vm-messages");
	}
	return cmd;
}

static int vm_browse_messages(struct opbx_channel *chan, struct vm_state *vms, struct opbx_vm_user *vmu)
{
	if (!strcasecmp(chan->language, "es")) {	/* SPANISH */
		return vm_browse_messages_es(chan, vms, vmu);
	} else if (!strcasecmp(chan->language, "it")) { /* ITALIAN */
		return vm_browse_messages_it(chan, vms, vmu);
	} else if (!strcasecmp(chan->language, "pt")) {	/* PORTUGUESE */
		return vm_browse_messages_pt(chan, vms, vmu);
	} else if (!strcasecmp(chan->language, "gr")){
		return vm_browse_messages_gr(chan, vms, vmu);   /* GREEK */
	} else {	/* Default to English syntax */
		return vm_browse_messages_en(chan, vms, vmu);
	}
}

static int vm_authenticate(struct opbx_channel *chan, char *mailbox, int mailbox_size,
			   struct opbx_vm_user *res_vmu, const char *context, const char *prefix,
			   int skipuser, int maxlogins, int silent)
{
	int useadsi, valid=0, logretries=0;
	char password[OPBX_MAX_EXTENSION]="", *passptr;
	struct opbx_vm_user vmus, *vmu = NULL;

	/* If ADSI is supported, setup login screen */
	adsi_begin(chan, &useadsi);
	if (!skipuser && useadsi)
		adsi_login(chan);
	if (!silent && !skipuser && opbx_streamfile(chan, "vm-login", chan->language)) {
		opbx_log(LOG_WARNING, "Couldn't stream login file\n");
		return -1;
	}
	
	/* Authenticate them and get their mailbox/password */
	
	while (!valid && (logretries < maxlogins)) {
		/* Prompt for, and read in the username */
		if (!skipuser && opbx_readstring(chan, mailbox, mailbox_size - 1, 2000, 10000, "#") < 0) {
			opbx_log(LOG_WARNING, "Couldn't read username\n");
			return -1;
		}
		if (opbx_strlen_zero(mailbox)) {
			if (chan->cid.cid_num) {
				opbx_copy_string(mailbox, chan->cid.cid_num, mailbox_size);
			} else {
				if (option_verbose > 2)
					opbx_verbose(VERBOSE_PREFIX_3 "Username not entered\n");	
				return -1;
			}
		}
		if (useadsi)
			adsi_password(chan);

		if (!opbx_strlen_zero(prefix)) {
			char fullusername[80] = "";
			opbx_copy_string(fullusername, prefix, sizeof(fullusername));
			strncat(fullusername, mailbox, sizeof(fullusername) - 1 - strlen(fullusername));
			opbx_copy_string(mailbox, fullusername, mailbox_size);
		}

		vmu = find_user(&vmus, context, mailbox);
		if (vmu && (vmu->password[0] == '\0' || (vmu->password[0] == '-' && vmu->password[1] == '\0'))) {
			/* saved password is blank, so don't bother asking */
			password[0] = '\0';
		} else {
			if (opbx_streamfile(chan, "vm-password", chan->language)) {
				opbx_log(LOG_WARNING, "Unable to stream password file\n");
				return -1;
			}
			if (opbx_readstring(chan, password, sizeof(password) - 1, 2000, 10000, "#") < 0) {
				opbx_log(LOG_WARNING, "Unable to read password\n");
				return -1;
			}
		}

		if (vmu) {
			passptr = vmu->password;
			if (passptr[0] == '-') passptr++;
		}
		if (vmu && !strcmp(passptr, password))
			valid++;
		else {
			if (option_verbose > 2)
				opbx_verbose( VERBOSE_PREFIX_3 "Incorrect password '%s' for user '%s' (context = %s)\n", password, mailbox, context ? context : "<any>");
			if (!opbx_strlen_zero(prefix))
				mailbox[0] = '\0';
		}
		logretries++;
		if (!valid) {
			if (skipuser || logretries >= maxlogins) {
				if (opbx_streamfile(chan, "vm-incorrect", chan->language)) {
					opbx_log(LOG_WARNING, "Unable to stream incorrect message\n");
					return -1;
				}
			} else {
				if (useadsi)
					adsi_login(chan);
				if (opbx_streamfile(chan, "vm-incorrect-mailbox", chan->language)) {
					opbx_log(LOG_WARNING, "Unable to stream incorrect mailbox message\n");
					return -1;
				}
			}
			if (opbx_waitstream(chan, ""))	/* Channel is hung up */
				return -1;
		}
	}
	if (!valid && (logretries >= maxlogins)) {
		opbx_stopstream(chan);
		opbx_play_and_wait(chan, "vm-goodbye");
		return -1;
	}
	if (vmu && !skipuser) {
		memcpy(res_vmu, vmu, sizeof(struct opbx_vm_user));
	}
	return 0;
}

static int vm_execmain(struct opbx_channel *chan, void *data)
{
	/* XXX This is, admittedly, some pretty horrendus code.  For some
	   reason it just seemed a lot easier to do with GOTO's.  I feel
	   like I'm back in my GWBASIC days. XXX */
	int res=-1;
	int cmd=0;
	int valid = 0;
	struct localuser *u;
	char prefixstr[80] ="";
	char ext_context[256]="";
	int box;
	int useadsi = 0;
	int skipuser = 0;
	struct vm_state vms;
	struct opbx_vm_user *vmu = NULL, vmus;
	char *context=NULL;
	int silentexit = 0;
	struct opbx_flags flags = { 0 };
	signed char record_gain = 0;

	LOCAL_USER_ADD(u);

	memset(&vms, 0, sizeof(vms));
	vms.lastmsg = -1;

	memset(&vmus, 0, sizeof(vmus));

	if (chan->_state != OPBX_STATE_UP)
		opbx_answer(chan);

	if (!opbx_strlen_zero(data)) {
		char *tmp;
		int argc;
		char *argv[2];
		char *opts[OPT_ARG_ARRAY_SIZE];

		tmp = opbx_strdupa(data);
		argc = opbx_separate_app_args(tmp, '|', argv, sizeof(argv) / sizeof(argv[0]));
		if (argc == 2) {
			if (opbx_parseoptions(vm_app_options, &flags, opts, argv[1])) {
				LOCAL_USER_REMOVE(u);
				return -1;
			}
			if (opbx_test_flag(&flags, OPT_RECORDGAIN)) {
				int gain;

				if (sscanf(opts[OPT_ARG_RECORDGAIN], "%d", &gain) != 1) {
					opbx_log(LOG_WARNING, "Invalid value '%s' provided for record gain option\n", opts[OPT_ARG_RECORDGAIN]);
					LOCAL_USER_REMOVE(u);
					return -1;
				} else {
					record_gain = (signed char) gain;
				}
			}
		} else {
			/* old style options parsing */
			while (*argv[0]) {
				if (*argv[0] == 's') {
					opbx_set_flag(&flags, OPT_SILENT);
					argv[0]++;
				} else if (*argv[0] == 'p') {
					opbx_set_flag(&flags, OPT_PREPEND_MAILBOX);
					argv[0]++;
				} else 
					break;
			}

		}

		valid = opbx_test_flag(&flags, OPT_SILENT);

		if ((context = strchr(argv[0], '@')))
			*context++ = '\0';

		if (opbx_test_flag(&flags, OPT_PREPEND_MAILBOX))
			opbx_copy_string(prefixstr, argv[0], sizeof(prefixstr));
		else
			opbx_copy_string(vms.username, argv[0], sizeof(vms.username));

		if (!opbx_strlen_zero(vms.username) && (vmu = find_user(&vmus, context ,vms.username)))
			skipuser++;
		else {
			if (!opbx_strlen_zero(vms.username))
				opbx_log(LOG_NOTICE, "Specified user '%s%s%s' not found (check voicemail.conf and/or realtime config).  Falling back to authentication mode.\n", vms.username, context ? "@" : "", context ? context : "");
			valid = 0;
		}
	}

	if (!valid)
		res = vm_authenticate(chan, vms.username, sizeof(vms.username), &vmus, context, prefixstr, skipuser, maxlogins, 0);

	if (!res) {
		valid = 1;
		if (!skipuser)
			vmu = &vmus;
	} else {
		res = 0;
	}

	/* If ADSI is supported, setup login screen */
	adsi_begin(chan, &useadsi);

	if (!valid)
		goto out;

	vms.deleted = calloc(vmu->maxmsg, sizeof(int));
	vms.heard = calloc(vmu->maxmsg, sizeof(int));
	
	/* Set language from config to override channel language */
	if (!opbx_strlen_zero(vmu->language))
		opbx_copy_string(chan->language, vmu->language, sizeof(chan->language));
	snprintf(vms.curdir, sizeof(vms.curdir), "%s/%s", VM_SPOOL_DIR, vmu->context);
	mkdir(vms.curdir, 0700);
	snprintf(vms.curdir, sizeof(vms.curdir), "%s/%s/%s", VM_SPOOL_DIR, vmu->context, vms.username);
	mkdir(vms.curdir, 0700);
	/* Retrieve old and new message counts */
	res = open_mailbox(&vms, vmu, 1);
	if (res == ERROR_LOCK_PATH)
		goto out;
	vms.oldmessages = vms.lastmsg + 1;
	/* Start in INBOX */
	res = open_mailbox(&vms, vmu, 0);
	if (res == ERROR_LOCK_PATH)
		goto out;
	vms.newmessages = vms.lastmsg + 1;
		
	/* Select proper mailbox FIRST!! */
	if (!vms.newmessages && vms.oldmessages) {
		/* If we only have old messages start here */
		res = open_mailbox(&vms, vmu, 1);
		if (res == ERROR_LOCK_PATH)
			goto out;
	}

	if (useadsi)
		adsi_status(chan, &vms);
	res = 0;

	/* Check to see if this is a new user */
	if (!strcasecmp(vmu->mailbox, vmu->password) && 
	    (opbx_test_flag(vmu, VM_FORCENAME | VM_FORCEGREET))) {
		if (opbx_play_and_wait(chan, "vm-newuser") == -1)
			opbx_log(LOG_WARNING, "Couldn't stream new user file\n");
		cmd = vm_newuser(chan, vmu, &vms, vmfmts, record_gain);
		if ((cmd == 't') || (cmd == '#')) {
			/* Timeout */
			res = 0;
			goto out;
		} else if (cmd < 0) {
			/* Hangup */
			res = -1;
			goto out;
		}
	}

	cmd = vm_intro(chan, &vms);

	vms.repeats = 0;
	vms.starting = 1;
	while ((cmd > -1) && (cmd != 't') && (cmd != '#')) {
		/* Run main menu */
		switch(cmd) {
		case '1':
			vms.curmsg = 0;
			/* Fall through */
		case '5':
			cmd = vm_browse_messages(chan, &vms, vmu);
			break;
		case '2': /* Change folders */
			if (useadsi)
				adsi_folders(chan, 0, "Change to folder...");
			cmd = get_folder2(chan, "vm-changeto", 0);
			if (cmd == '#') {
				cmd = 0;
			} else if (cmd > 0) {
				cmd = cmd - '0';
				res = close_mailbox(&vms, vmu);
				if (res == ERROR_LOCK_PATH)
					goto out;
				res = open_mailbox(&vms, vmu, cmd);
				if (res == ERROR_LOCK_PATH)
					goto out;
				cmd = 0;
			}
			if (useadsi)
				adsi_status2(chan, &vms);
				
			if (!cmd)
				cmd = vm_play_folder_name(chan, vms.vmbox);

			vms.starting = 1;
			break;
		case '3': /* Advanced options */
			cmd = 0;
			vms.repeats = 0;
			while ((cmd > -1) && (cmd != 't') && (cmd != '#')) {
				switch(cmd) {
				case '1': /* Reply */
					if (vms.lastmsg > -1) {
						cmd = advanced_options(chan, vmu, &vms, vms.curmsg, 1, record_gain);
						if (cmd == ERROR_LOCK_PATH) {
							res = cmd;
							goto out;
						}
					} else
						cmd = opbx_play_and_wait(chan, "vm-sorry");
					cmd = 't';
					break;
				case '2': /* Callback */
					opbx_verbose( VERBOSE_PREFIX_3 "Callback Requested\n");
					if (!opbx_strlen_zero(vmu->callback) && vms.lastmsg > -1) {
						cmd = advanced_options(chan, vmu, &vms, vms.curmsg, 2, record_gain);
						if (cmd == 9) {
							silentexit = 1;
							goto out;
						} else if (cmd == ERROR_LOCK_PATH) {
							res = cmd;
							goto out;
						}
					}
					else 
						cmd = opbx_play_and_wait(chan, "vm-sorry");
					cmd = 't';
					break;
				case '3': /* Envelope */
					if (vms.lastmsg > -1) {
						cmd = advanced_options(chan, vmu, &vms, vms.curmsg, 3, record_gain);
						if (cmd == ERROR_LOCK_PATH) {
							res = cmd;
							goto out;
						}
					} else
						cmd = opbx_play_and_wait(chan, "vm-sorry");
					cmd = 't';
					break;
				case '4': /* Dialout */
					if (!opbx_strlen_zero(vmu->dialout)) {
						cmd = dialout(chan, vmu, NULL, vmu->dialout);
						if (cmd == 9) {
							silentexit = 1;
							goto out;
						}
					}
					else 
						cmd = opbx_play_and_wait(chan, "vm-sorry");
					cmd = 't';
					break;

				case '5': /* Leave VoiceMail */
					if (opbx_test_flag(vmu, VM_SVMAIL)) {
						cmd = forward_message(chan, context, vms.curdir, vms.curmsg, vmu, vmfmts, 1, record_gain);
						if (cmd == ERROR_LOCK_PATH) {
							res = cmd;
							goto out;
						}
					} else
						cmd = opbx_play_and_wait(chan,"vm-sorry");
					cmd='t';
					break;
					
				case '*': /* Return to main menu */
					cmd = 't';
					break;

				default:
					cmd = 0;
					if (!vms.starting) {
						cmd = opbx_play_and_wait(chan, "vm-toreply");
					}
					if (!opbx_strlen_zero(vmu->callback) && !vms.starting && !cmd) {
						cmd = opbx_play_and_wait(chan, "vm-tocallback");
					}
					if (!cmd && !vms.starting) {
						cmd = opbx_play_and_wait(chan, "vm-tohearenv");
					}
					if (!opbx_strlen_zero(vmu->dialout) && !cmd) {
						cmd = opbx_play_and_wait(chan, "vm-tomakecall");
					}
					if (opbx_test_flag(vmu, VM_SVMAIL) && !cmd)
						cmd=opbx_play_and_wait(chan, "vm-leavemsg");
					if (!cmd)
						cmd = opbx_play_and_wait(chan, "vm-starmain");
					if (!cmd)
						cmd = opbx_waitfordigit(chan,6000);
					if (!cmd)
						vms.repeats++;
					if (vms.repeats > 3)
						cmd = 't';
				}
			}
			if (cmd == 't') {
				cmd = 0;
				vms.repeats = 0;
			}
			break;
		case '4':
			if (vms.curmsg) {
				vms.curmsg--;
				cmd = play_message(chan, vmu, &vms);
			} else {
				cmd = opbx_play_and_wait(chan, "vm-nomore");
			}
			break;
		case '6':
			if (vms.curmsg < vms.lastmsg) {
				vms.curmsg++;
				cmd = play_message(chan, vmu, &vms);
			} else {
				cmd = opbx_play_and_wait(chan, "vm-nomore");
			}
			break;
		case '7':
			vms.deleted[vms.curmsg] = !vms.deleted[vms.curmsg];
			if (useadsi)
				adsi_delete(chan, &vms);
			if (vms.deleted[vms.curmsg]) 
				cmd = opbx_play_and_wait(chan, "vm-deleted");
			else
				cmd = opbx_play_and_wait(chan, "vm-undeleted");
			if (opbx_test_flag((&globalflags), VM_SKIPAFTERCMD)) {
				if (vms.curmsg < vms.lastmsg) {
					vms.curmsg++;
					cmd = play_message(chan, vmu, &vms);
				} else {
					cmd = opbx_play_and_wait(chan, "vm-nomore");
				}
			}
			break;
	
		case '8':
			if (vms.lastmsg > -1) {
				cmd = forward_message(chan, context, vms.curdir, vms.curmsg, vmu, vmfmts, 0, record_gain);
				if (cmd == ERROR_LOCK_PATH) {
					res = cmd;
					goto out;
				}
			} else
				cmd = opbx_play_and_wait(chan, "vm-nomore");
			break;
		case '9':
			if (useadsi)
				adsi_folders(chan, 1, "Save to folder...");
			cmd = get_folder2(chan, "vm-savefolder", 1);
			box = 0;	/* Shut up compiler */
			if (cmd == '#') {
				cmd = 0;
				break;
			} else if (cmd > 0) {
				box = cmd = cmd - '0';
				cmd = save_to_folder(vmu, vms.curdir, vms.curmsg, vmu->context, vms.username, cmd);
				if (cmd == ERROR_LOCK_PATH) {
					res = cmd;
					goto out;
				} else if (!cmd) {
					vms.deleted[vms.curmsg] = 1;
				} else {
					vms.deleted[vms.curmsg] = 0;
					vms.heard[vms.curmsg] = 0;
				}
			}
			make_file(vms.fn, sizeof(vms.fn), vms.curdir, vms.curmsg);
			if (useadsi)
				adsi_message(chan, &vms);
			snprintf(vms.fn, sizeof(vms.fn), "vm-%s", mbox(box));
			if (!cmd) {
				cmd = opbx_play_and_wait(chan, "vm-message");
				if (!cmd)
					cmd = say_and_wait(chan, vms.curmsg + 1, chan->language);
				if (!cmd)
					cmd = opbx_play_and_wait(chan, "vm-savedto");
				if (!cmd)
					cmd = vm_play_folder_name(chan, vms.fn);
			} else {
				cmd = opbx_play_and_wait(chan, "vm-mailboxfull");
			}
			if (opbx_test_flag((&globalflags), VM_SKIPAFTERCMD)) {
				if (vms.curmsg < vms.lastmsg) {
					vms.curmsg++;
					cmd = play_message(chan, vmu, &vms);
				} else {
					cmd = opbx_play_and_wait(chan, "vm-nomore");
				}
			}
			break;
		case '*':
			if (!vms.starting) {
				cmd = opbx_play_and_wait(chan, "vm-onefor");
				if (!cmd)
					cmd = vm_play_folder_name(chan, vms.vmbox);
				if (!cmd)
					cmd = opbx_play_and_wait(chan, "vm-opts");
				if (!cmd)
					cmd = vm_instructions(chan, &vms, 1);
			} else
				cmd = 0;
			break;
		case '0':
			cmd = vm_options(chan, vmu, &vms, vmfmts, record_gain);
			if (useadsi)
				adsi_status(chan, &vms);
			break;
		default:	/* Nothing */
			cmd = vm_instructions(chan, &vms, 0);
			break;
		}
	}
	if ((cmd == 't') || (cmd == '#')) {
		/* Timeout */
		res = 0;
	} else {
		/* Hangup */
		res = -1;
	}

out:
	if (res > -1) {
		opbx_stopstream(chan);
		adsi_goodbye(chan);
		if (valid) {
			if (silentexit)
				res = opbx_play_and_wait(chan, "vm-dialout");
			else 
				res = opbx_play_and_wait(chan, "vm-goodbye");
			if (res > 0)
				res = 0;
		}
		if (useadsi)
			adsi_unload_session(chan);
	}
	if (vmu)
		close_mailbox(&vms, vmu);
	if (valid) {
		snprintf(ext_context, sizeof(ext_context), "%s@%s", vms.username, vmu->context);
		manager_event(EVENT_FLAG_CALL, "MessageWaiting", "Mailbox: %s\r\nWaiting: %d\r\n", ext_context, has_voicemail(ext_context, NULL));
		run_externnotify(vmu->context, vmu->mailbox);
	}
	if (vmu)
		free_user(vmu);
	if (vms.deleted)
		free(vms.deleted);
	if (vms.heard)
		free(vms.heard);
	LOCAL_USER_REMOVE(u);

	return res;
}

static int vm_exec(struct opbx_channel *chan, void *data)
{
	int res = 0;
	struct localuser *u;
	char tmp[256];
	struct leave_vm_options leave_options;
	int argc;
	char *argv[2];
	struct opbx_flags flags = { 0 };
	char *opts[OPT_ARG_ARRAY_SIZE];
	
	LOCAL_USER_ADD(u);
	
	memset(&leave_options, 0, sizeof(leave_options));

	if (chan->_state != OPBX_STATE_UP)
		opbx_answer(chan);

	if (!opbx_strlen_zero(data)) {
		opbx_copy_string(tmp, data, sizeof(tmp));
		argc = opbx_separate_app_args(tmp, '|', argv, sizeof(argv) / sizeof(argv[0]));
		if (argc == 2) {
			if (opbx_parseoptions(vm_app_options, &flags, opts, argv[1])) {
				LOCAL_USER_REMOVE(u);
				return -1;
			}
			opbx_copy_flags(&leave_options, &flags, OPT_SILENT | OPT_BUSY_GREETING | OPT_UNAVAIL_GREETING);
			if (opbx_test_flag(&flags, OPT_RECORDGAIN)) {
				int gain;

				if (sscanf(opts[OPT_ARG_RECORDGAIN], "%d", &gain) != 1) {
					opbx_log(LOG_WARNING, "Invalid value '%s' provided for record gain option\n", opts[OPT_ARG_RECORDGAIN]);
					LOCAL_USER_REMOVE(u);
					return -1;
				} else {
					leave_options.record_gain = (signed char) gain;
				}
			}
		} else {
			/* old style options parsing */
			while (*argv[0]) {
				if (*argv[0] == 's') {
					opbx_set_flag(&leave_options, OPT_SILENT);
					argv[0]++;
				} else if (*argv[0] == 'b') {
					opbx_set_flag(&leave_options, OPT_BUSY_GREETING);
					argv[0]++;
				} else if (*argv[0] == 'u') {
					opbx_set_flag(&leave_options, OPT_UNAVAIL_GREETING);
					argv[0]++;
				} else 
					break;
			}
		}
	} else {
		res = opbx_app_getdata(chan, "vm-whichbox", tmp, sizeof(tmp) - 1, 0);
		if (res < 0) {
			LOCAL_USER_REMOVE(u);
			return res;
		}
		if (opbx_strlen_zero(tmp)) {
			LOCAL_USER_REMOVE(u);
			return 0;
		}	
	}

	res = leave_voicemail(chan, argv[0], &leave_options);

	if (res == ERROR_LOCK_PATH) {
		opbx_log(LOG_ERROR, "Could not leave voicemail. The path is already locked.\n");
		/*Send the call to n+101 priority, where n is the current priority*/
		if (opbx_goto_if_exists(chan, chan->context, chan->exten, chan->priority + 101))
			opbx_log(LOG_WARNING, "Extension %s, priority %d doesn't exist.\n", chan->exten, chan->priority + 101);
		res = 0;
	}
	
	LOCAL_USER_REMOVE(u);

	return res;
}

static int append_mailbox(char *context, char *mbox, char *data)
{
	/* Assumes lock is already held */
	char tmp[256] = "";
	char *stringp;
	char *s;
	struct opbx_vm_user *vmu;

	opbx_copy_string(tmp, data, sizeof(tmp));
	vmu = malloc(sizeof(struct opbx_vm_user));
	if (vmu) {
		memset(vmu, 0, sizeof(struct opbx_vm_user));
		opbx_copy_string(vmu->context, context, sizeof(vmu->context));
		opbx_copy_string(vmu->mailbox, mbox, sizeof(vmu->mailbox));

		populate_defaults(vmu);

		stringp = tmp;
		if ((s = strsep(&stringp, ","))) 
			opbx_copy_string(vmu->password, s, sizeof(vmu->password));
		if (stringp && (s = strsep(&stringp, ","))) 
			opbx_copy_string(vmu->fullname, s, sizeof(vmu->fullname));
		if (stringp && (s = strsep(&stringp, ","))) 
			opbx_copy_string(vmu->email, s, sizeof(vmu->email));
		if (stringp && (s = strsep(&stringp, ","))) 
			opbx_copy_string(vmu->pager, s, sizeof(vmu->pager));
		if (stringp && (s = strsep(&stringp, ","))) 
			apply_options(vmu, s);
		
		vmu->next = NULL;
		if (usersl)
			usersl->next = vmu;
		else
			users = vmu;
		usersl = vmu;
	}
	return 0;
}

static int vm_box_exists(struct opbx_channel *chan, void *data) 
{
	struct localuser *u;
	struct opbx_vm_user svm;
	char *context, *box;

	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_ERROR, "MailboxExists requires an argument: (vmbox[@context])\n");
		return -1;
	}

	LOCAL_USER_ADD(u);

	box = opbx_strdupa(data);
	if (!box) {
		opbx_log(LOG_ERROR, "Out of memory\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	if ((context = strchr(box, '@'))) {
		*context = '\0';
		context++;
	}

	if (find_user(&svm, context, box)) {
		if (opbx_goto_if_exists(chan, chan->context, chan->exten, chan->priority + 101)) 
			opbx_log(LOG_WARNING, "VM box %s@%s exists, but extension %s, priority %d doesn't exist\n", box, context, chan->exten, chan->priority + 101);
	}
	LOCAL_USER_REMOVE(u);
	return 0;
}

static int vmauthenticate(struct opbx_channel *chan, void *data)
{
	struct localuser *u;
	char *s = data, *user=NULL, *context=NULL, mailbox[OPBX_MAX_EXTENSION];
	struct opbx_vm_user vmus;
	char *options = NULL;
	int silent = 0;
	int res = -1;

	LOCAL_USER_ADD(u);
	
	if (s) {
		s = opbx_strdupa(s);
		if (!s) {
			opbx_log(LOG_ERROR, "Out of memory\n");
			return -1;
		}
		user = strsep(&s, "|");
		options = strsep(&s, "|");
		if (user) {
			s = user;
			user = strsep(&s, "@");
			context = strsep(&s, "");
		}
	}

	if (options) {
		silent = (strchr(options, 's')) != NULL;
	}

	if (!vm_authenticate(chan, mailbox, sizeof(mailbox), &vmus, context, NULL, 0, 3, silent)) {
		pbx_builtin_setvar_helper(chan, "AUTH_MAILBOX", mailbox);
		pbx_builtin_setvar_helper(chan, "AUTH_CONTEXT", vmus.context);
		res = 0;
	}

	LOCAL_USER_REMOVE(u);
	return res;
}

static char show_voicemail_users_help[] =
"Usage: show voicemail users [for <context>]\n"
"       Lists all mailboxes currently set up\n";

static char show_voicemail_zones_help[] =
"Usage: show voicemail zones\n"
"       Lists zone message formats\n";

static int handle_show_voicemail_users(int fd, int argc, char *argv[])
{
	struct opbx_vm_user *vmu = users;
	char *output_format = "%-10s %-5s %-25s %-10s %6s\n";

	if ((argc < 3) || (argc > 5) || (argc == 4)) return RESULT_SHOWUSAGE;
	else if ((argc == 5) && strcmp(argv[3],"for")) return RESULT_SHOWUSAGE;

	if (vmu) {
		if (argc == 3)
			opbx_cli(fd, output_format, "Context", "Mbox", "User", "Zone", "NewMsg");
		else {
			int count = 0;
			while (vmu) {
				if (!strcmp(argv[4],vmu->context))
					count++;
				vmu = vmu->next;
			}
			if (count) {
				vmu = users;
				opbx_cli(fd, output_format, "Context", "Mbox", "User", "Zone", "NewMsg");
			} else {
				opbx_cli(fd, "No such voicemail context \"%s\"\n", argv[4]);
				return RESULT_FAILURE;
			}
		}
		while (vmu) {
			char dirname[256];
			DIR *vmdir;
			struct dirent *vment;
			int vmcount = 0;
			char count[12];

			if ((argc == 3) || ((argc == 5) && !strcmp(argv[4],vmu->context))) {
				make_dir(dirname, 255, vmu->context, vmu->mailbox, "INBOX");
				if ((vmdir = opendir(dirname))) {
					/* No matter what the format of VM, there will always be a .txt file for each message. */
					while ((vment = readdir(vmdir)))
						if (strlen(vment->d_name) > 7 && !strncmp(vment->d_name + 7,".txt",4))
							vmcount++;
					closedir(vmdir);
				}
				snprintf(count,sizeof(count),"%d",vmcount);
				opbx_cli(fd, output_format, vmu->context, vmu->mailbox, vmu->fullname, vmu->zonetag, count);
			}
			vmu = vmu->next;
		}
	} else {
		opbx_cli(fd, "There are no voicemail users currently defined\n");
		return RESULT_FAILURE;
	}
	return RESULT_SUCCESS;
}

static int handle_show_voicemail_zones(int fd, int argc, char *argv[])
{
	struct vm_zone *zone = zones;
	char *output_format = "%-15s %-20s %-45s\n";

	if (argc != 3) return RESULT_SHOWUSAGE;

	if (zone) {
		opbx_cli(fd, output_format, "Zone", "Timezone", "Message Format");
		while (zone) {
			opbx_cli(fd, output_format, zone->name, zone->timezone, zone->msg_format);
			zone = zone->next;
		}
	} else {
		opbx_cli(fd, "There are no voicemail zones currently defined\n");
		return RESULT_FAILURE;
	}
	return RESULT_SUCCESS;
}

static char *complete_show_voicemail_users(char *line, char *word, int pos, int state)
{
	int which = 0;
	struct opbx_vm_user *vmu = users;
	char *context = "";

	/* 0 - show; 1 - voicemail; 2 - users; 3 - for; 4 - <context> */
	if (pos > 4)
		return NULL;
	if (pos == 3) {
		if (state == 0)
			return strdup("for");
		else
			return NULL;
	}
	while (vmu) {
		if (!strncasecmp(word, vmu->context, strlen(word))) {
			if (context && strcmp(context, vmu->context)) {
				if (++which > state) {
					return strdup(vmu->context);
				}
				context = vmu->context;
			}
		}
		vmu = vmu->next;
	}
	return NULL;
}

static struct opbx_cli_entry show_voicemail_users_cli =
	{ { "show", "voicemail", "users", NULL },
	handle_show_voicemail_users, "List defined voicemail boxes",
	show_voicemail_users_help, complete_show_voicemail_users };

static struct opbx_cli_entry show_voicemail_zones_cli =
	{ { "show", "voicemail", "zones", NULL },
	handle_show_voicemail_zones, "List zone message formats",
	show_voicemail_zones_help, NULL };

static int load_config(void)
{
	struct opbx_vm_user *cur, *l;
	struct vm_zone *zcur, *zl;
	struct opbx_config *cfg;
	char *cat;
	struct opbx_variable *var;
	char *notifystr = NULL;
	char *astattach;
	char *astsaycid;
	char *send_voicemail;
	char *astcallop;
	char *astreview;
	char *astskipcmd;
	char *asthearenv;
	char *astsaydurationinfo;
	char *astsaydurationminfo;
	char *silencestr;
	char *maxmsgstr;
	char *astdirfwd;
	char *thresholdstr;
	char *fmt;
	char *astemail;
 	char *astmailcmd = SENDMAIL;
	char *s,*q,*stringp;
	char *dialoutcxt = NULL;
	char *callbackcxt = NULL;	
	char *exitcxt = NULL;	
	char *extpc;
	char *emaildateformatstr;
	int x;
	int tmpadsi[4];

	cfg = opbx_config_load(VOICEMAIL_CONFIG);
	opbx_mutex_lock(&vmlock);
	cur = users;
	while (cur) {
		l = cur;
		cur = cur->next;
		opbx_set_flag(l, VM_ALLOCED);	
		free_user(l);
	}
	zcur = zones;
	while (zcur) {
		zl = zcur;
		zcur = zcur->next;
		free_zone(zl);
	}
	zones = NULL;
	zonesl = NULL;
	users = NULL;
	usersl = NULL;
	memset(ext_pass_cmd, 0, sizeof(ext_pass_cmd));

	if (cfg) {
		/* General settings */

		/* Attach voice message to mail message ? */
		if (!(astattach = opbx_variable_retrieve(cfg, "general", "attach"))) 
			astattach = "yes";
		opbx_set2_flag((&globalflags), opbx_true(astattach), VM_ATTACH);	

#ifdef USE_ODBC_STORAGE
		strcpy(odbc_database, "openpbx");
		if ((thresholdstr = opbx_variable_retrieve(cfg, "general", "odbcstorage"))) {
			opbx_copy_string(odbc_database, thresholdstr, sizeof(odbc_database));
		}
		strcpy(odbc_table, "voicemessages");
                if ((thresholdstr = opbx_variable_retrieve(cfg, "general", "odbctable"))) {
                        opbx_copy_string(odbc_table, thresholdstr, sizeof(odbc_table));
                }
#endif		
		/* Mail command */
		strcpy(mailcmd, SENDMAIL);
		if ((astmailcmd = opbx_variable_retrieve(cfg, "general", "mailcmd")))
			opbx_copy_string(mailcmd, astmailcmd, sizeof(mailcmd)); /* User setting */

		maxsilence = 0;
		if ((silencestr = opbx_variable_retrieve(cfg, "general", "maxsilence"))) {
			maxsilence = atoi(silencestr);
			if (maxsilence > 0)
				maxsilence *= 1000;
		}
		
		if (!(maxmsgstr = opbx_variable_retrieve(cfg, "general", "maxmsg"))) {
			maxmsg = MAXMSG;
		} else {
			maxmsg = atoi(maxmsgstr);
			if (maxmsg <= 0) {
				opbx_log(LOG_WARNING, "Invalid number of messages per folder '%s'. Using default value %i\n", maxmsgstr, MAXMSG);
				maxmsg = MAXMSG;
			} else if (maxmsg > MAXMSGLIMIT) {
				opbx_log(LOG_WARNING, "Maximum number of messages per folder is %i. Cannot accept value '%s'\n", MAXMSGLIMIT, maxmsgstr);
				maxmsg = MAXMSGLIMIT;
			}
		}

		/* Load date format config for voicemail mail */
		if ((emaildateformatstr = opbx_variable_retrieve(cfg, "general", "emaildateformat"))) {
			opbx_copy_string(emaildateformat, emaildateformatstr, sizeof(emaildateformat));
		}

		/* External password changing command */
		if ((extpc = opbx_variable_retrieve(cfg, "general", "externpass"))) {
			opbx_copy_string(ext_pass_cmd,extpc,sizeof(ext_pass_cmd));
		}

		/* External voicemail notify application */
		
		if ((notifystr = opbx_variable_retrieve(cfg, "general", "externnotify"))) {
			opbx_copy_string(externnotify, notifystr, sizeof(externnotify));
			opbx_log(LOG_DEBUG, "found externnotify: %s\n", externnotify);
		} else {
			externnotify[0] = '\0';
		}

		/* Silence treshold */
		silencethreshold = 256;
		if ((thresholdstr = opbx_variable_retrieve(cfg, "general", "silencethreshold")))
			silencethreshold = atoi(thresholdstr);
		
		if (!(astemail = opbx_variable_retrieve(cfg, "general", "serveremail"))) 
			astemail = OPENPBX_USERNAME;
		opbx_copy_string(serveremail, astemail, sizeof(serveremail));
		
		vmmaxmessage = 0;
		if ((s = opbx_variable_retrieve(cfg, "general", "maxmessage"))) {
			if (sscanf(s, "%d", &x) == 1) {
				vmmaxmessage = x;
			} else {
				opbx_log(LOG_WARNING, "Invalid max message time length\n");
			}
		}

		vmminmessage = 0;
		if ((s = opbx_variable_retrieve(cfg, "general", "minmessage"))) {
			if (sscanf(s, "%d", &x) == 1) {
				vmminmessage = x;
				if (maxsilence <= vmminmessage)
					opbx_log(LOG_WARNING, "maxsilence should be less than minmessage or you may get empty messages\n");
			} else {
				opbx_log(LOG_WARNING, "Invalid min message time length\n");
			}
		}
		fmt = opbx_variable_retrieve(cfg, "general", "format");
		if (!fmt)
			fmt = "wav";	
		opbx_copy_string(vmfmts, fmt, sizeof(vmfmts));

		skipms = 3000;
		if ((s = opbx_variable_retrieve(cfg, "general", "maxgreet"))) {
			if (sscanf(s, "%d", &x) == 1) {
				maxgreet = x;
			} else {
				opbx_log(LOG_WARNING, "Invalid max message greeting length\n");
			}
		}

		if ((s = opbx_variable_retrieve(cfg, "general", "skipms"))) {
			if (sscanf(s, "%d", &x) == 1) {
				skipms = x;
			} else {
				opbx_log(LOG_WARNING, "Invalid skipms value\n");
			}
		}

		maxlogins = 3;
		if ((s = opbx_variable_retrieve(cfg, "general", "maxlogins"))) {
			if (sscanf(s, "%d", &x) == 1) {
				maxlogins = x;
			} else {
				opbx_log(LOG_WARNING, "Invalid max failed login attempts\n");
			}
		}

		/* Force new user to record name ? */
		if (!(astattach = opbx_variable_retrieve(cfg, "general", "forcename"))) 
			astattach = "no";
		opbx_set2_flag((&globalflags), opbx_true(astattach), VM_FORCENAME);

		/* Force new user to record greetings ? */
		if (!(astattach = opbx_variable_retrieve(cfg, "general", "forcegreetings"))) 
			astattach = "no";
		opbx_set2_flag((&globalflags), opbx_true(astattach), VM_FORCEGREET);

		if ((s = opbx_variable_retrieve(cfg, "general", "cidinternalcontexts"))){
			opbx_log(LOG_DEBUG,"VM_CID Internal context string: %s\n",s);
			stringp = opbx_strdupa(s);
			for (x = 0 ; x < MAX_NUM_CID_CONTEXTS ; x++){
				if (!opbx_strlen_zero(stringp)) {
					q = strsep(&stringp,",");
					while ((*q == ' ')||(*q == '\t')) /* Eat white space between contexts */
						q++;
					opbx_copy_string(cidinternalcontexts[x], q, sizeof(cidinternalcontexts[x]));
					opbx_log(LOG_DEBUG,"VM_CID Internal context %d: %s\n", x, cidinternalcontexts[x]);
				} else {
					cidinternalcontexts[x][0] = '\0';
				}
			}
		}
		if (!(astreview = opbx_variable_retrieve(cfg, "general", "review"))){
			opbx_log(LOG_DEBUG,"VM Review Option disabled globally\n");
			astreview = "no";
		}
		opbx_set2_flag((&globalflags), opbx_true(astreview), VM_REVIEW);	

		if (!(astcallop = opbx_variable_retrieve(cfg, "general", "operator"))){
			opbx_log(LOG_DEBUG,"VM Operator break disabled globally\n");
			astcallop = "no";
		}
		opbx_set2_flag((&globalflags), opbx_true(astcallop), VM_OPERATOR);	

		if (!(astsaycid = opbx_variable_retrieve(cfg, "general", "saycid"))) {
			opbx_log(LOG_DEBUG,"VM CID Info before msg disabled globally\n");
			astsaycid = "no";
		} 
		opbx_set2_flag((&globalflags), opbx_true(astsaycid), VM_SAYCID);	

		if (!(send_voicemail = opbx_variable_retrieve(cfg,"general", "sendvoicemail"))){
			opbx_log(LOG_DEBUG,"Send Voicemail msg disabled globally\n");
			send_voicemail = "no";
		}
		opbx_set2_flag((&globalflags), opbx_true(send_voicemail), VM_SVMAIL);
	
		if (!(asthearenv = opbx_variable_retrieve(cfg, "general", "envelope"))) {
			opbx_log(LOG_DEBUG,"ENVELOPE before msg enabled globally\n");
			asthearenv = "yes";
		}
		opbx_set2_flag((&globalflags), opbx_true(asthearenv), VM_ENVELOPE);	

		if (!(astsaydurationinfo = opbx_variable_retrieve(cfg, "general", "sayduration"))) {
			opbx_log(LOG_DEBUG,"Duration info before msg enabled globally\n");
			astsaydurationinfo = "yes";
		}
		opbx_set2_flag((&globalflags), opbx_true(astsaydurationinfo), VM_SAYDURATION);	

		saydurationminfo = 2;
		if ((astsaydurationminfo = opbx_variable_retrieve(cfg, "general", "saydurationm"))) {
			if (sscanf(astsaydurationminfo, "%d", &x) == 1) {
				saydurationminfo = x;
			} else {
				opbx_log(LOG_WARNING, "Invalid min duration for say duration\n");
			}
		}

		if (!(astskipcmd = opbx_variable_retrieve(cfg, "general", "nextaftercmd"))) {
			opbx_log(LOG_DEBUG,"We are not going to skip to the next msg after save/delete\n");
			astskipcmd = "no";
		}
		opbx_set2_flag((&globalflags), opbx_true(astskipcmd), VM_SKIPAFTERCMD);

		if ((dialoutcxt = opbx_variable_retrieve(cfg, "general", "dialout"))) {
			opbx_copy_string(dialcontext, dialoutcxt, sizeof(dialcontext));
			opbx_log(LOG_DEBUG, "found dialout context: %s\n", dialcontext);
		} else {
			dialcontext[0] = '\0';	
		}
		
		if ((callbackcxt = opbx_variable_retrieve(cfg, "general", "callback"))) {
			opbx_copy_string(callcontext, callbackcxt, sizeof(callcontext));
			opbx_log(LOG_DEBUG, "found callback context: %s\n", callcontext);
		} else {
			callcontext[0] = '\0';
		}

		if ((exitcxt = opbx_variable_retrieve(cfg, "general", "exitcontext"))) {
			opbx_copy_string(exitcontext, exitcxt, sizeof(exitcontext));
			opbx_log(LOG_DEBUG, "found operator context: %s\n", exitcontext);
		} else {
			exitcontext[0] = '\0';
		}

		if (!(astdirfwd = opbx_variable_retrieve(cfg, "general", "usedirectory"))) 
			astdirfwd = "no";
		opbx_set2_flag((&globalflags), opbx_true(astdirfwd), VM_DIRECFORWARD);	
		cat = opbx_category_browse(cfg, NULL);
		while (cat) {
			if (strcasecmp(cat, "general")) {
				var = opbx_variable_browse(cfg, cat);
				if (strcasecmp(cat, "zonemessages")) {
					/* Process mailboxes in this context */
					while (var) {
						append_mailbox(cat, var->name, var->value);
						var = var->next;
					}
				} else {
					/* Timezones in this context */
					while (var) {
						struct vm_zone *z;
						z = malloc(sizeof(struct vm_zone));
						if (z != NULL) {
							char *msg_format, *timezone;
							msg_format = opbx_strdupa(var->value);
							if (msg_format != NULL) {
								timezone = strsep(&msg_format, "|");
								if (msg_format) {
									opbx_copy_string(z->name, var->name, sizeof(z->name));
									opbx_copy_string(z->timezone, timezone, sizeof(z->timezone));
									opbx_copy_string(z->msg_format, msg_format, sizeof(z->msg_format));
									z->next = NULL;
									if (zones) {
										zonesl->next = z;
										zonesl = z;
									} else {
										zones = z;
										zonesl = z;
									}
								} else {
									opbx_log(LOG_WARNING, "Invalid timezone definition at line %d\n", var->lineno);
									free(z);
								}
							} else {
								opbx_log(LOG_WARNING, "Out of memory while reading voicemail config\n");
								free(z);
								return -1;
							}
						} else {
							opbx_log(LOG_WARNING, "Out of memory while reading voicemail config\n");
							return -1;
						}
						var = var->next;
					}
				}
			}
			cat = opbx_category_browse(cfg, cat);
		}
		memset(fromstring,0,sizeof(fromstring));
		memset(pagerfromstring,0,sizeof(pagerfromstring));
		memset(emailtitle,0,sizeof(emailtitle));
		strcpy(charset, "ISO-8859-1");
		if (emailbody) {
			free(emailbody);
			emailbody = NULL;
		}
		if (emailsubject) {
			free(emailsubject);
			emailsubject = NULL;
		}
               if (pagerbody) {
                       free(pagerbody);
                       pagerbody = NULL;
               }
               if (pagersubject) {
                       free(pagersubject);
                       pagersubject = NULL;
               }
		if ((s=opbx_variable_retrieve(cfg, "general", "pbxskip")))
			opbx_set2_flag((&globalflags), opbx_true(s), VM_PBXSKIP);
		if ((s=opbx_variable_retrieve(cfg, "general", "fromstring")))
			opbx_copy_string(fromstring,s,sizeof(fromstring));
		if ((s=opbx_variable_retrieve(cfg, "general", "pagerfromstring")))
			opbx_copy_string(pagerfromstring,s,sizeof(pagerfromstring));
		if ((s=opbx_variable_retrieve(cfg, "general", "charset")))
			opbx_copy_string(charset,s,sizeof(charset));
		if ((s=opbx_variable_retrieve(cfg, "general", "adsifdn"))) {
			sscanf(s, "%2x%2x%2x%2x", &tmpadsi[0], &tmpadsi[1], &tmpadsi[2], &tmpadsi[3]);
			for (x=0; x<4; x++) {
				memcpy(&adsifdn[x], &tmpadsi[x], 1);
			}
		}
		if ((s=opbx_variable_retrieve(cfg, "general", "adsisec"))) {
			sscanf(s, "%2x%2x%2x%2x", &tmpadsi[0], &tmpadsi[1], &tmpadsi[2], &tmpadsi[3]);
			for (x=0; x<4; x++) {
				memcpy(&adsisec[x], &tmpadsi[x], 1);
			}
		}
		if ((s=opbx_variable_retrieve(cfg, "general", "adsiver")))
			if (atoi(s)) {
				adsiver = atoi(s);
			}
		if ((s=opbx_variable_retrieve(cfg, "general", "emailtitle"))) {
			opbx_log(LOG_NOTICE, "Keyword 'emailtitle' is DEPRECATED, please use 'emailsubject' instead.\n");
			opbx_copy_string(emailtitle,s,sizeof(emailtitle));
		}
		if ((s=opbx_variable_retrieve(cfg, "general", "emailsubject")))
			emailsubject = strdup(s);
		if ((s=opbx_variable_retrieve(cfg, "general", "emailbody"))) {
			char *tmpread, *tmpwrite;
			emailbody = strdup(s);

			/* substitute strings \t and \n into the apropriate characters */
			tmpread = tmpwrite = emailbody;
                       while ((tmpwrite = strchr(tmpread,'\\'))) {
                               int len = strlen("\n");
                               switch (tmpwrite[1]) {
                                       case 'n':
                                               strncpy(tmpwrite+len,tmpwrite+2,strlen(tmpwrite+2)+1);
                                               strncpy(tmpwrite,"\n",len);
                                               break;
                                       case 't':
                                               strncpy(tmpwrite+len,tmpwrite+2,strlen(tmpwrite+2)+1);
                                               strncpy(tmpwrite,"\t",len);
                                               break;
                                       default:
                                               opbx_log(LOG_NOTICE, "Substitution routine does not support this character: %c\n",tmpwrite[1]);
                               }
                               tmpread = tmpwrite+len;
                       }
               }
               if ((s=opbx_variable_retrieve(cfg, "general", "pagersubject")))
                       pagersubject = strdup(s);
               if ((s=opbx_variable_retrieve(cfg, "general", "pagerbody"))) {
                       char *tmpread, *tmpwrite;
                       pagerbody = strdup(s);

                       /* substitute strings \t and \n into the apropriate characters */
                       tmpread = tmpwrite = pagerbody;
			while ((tmpwrite = strchr(tmpread,'\\'))) {
				int len = strlen("\n");
				switch (tmpwrite[1]) {
					case 'n':
						strncpy(tmpwrite+len,tmpwrite+2,strlen(tmpwrite+2)+1);
						strncpy(tmpwrite,"\n",len);
						break;
					case 't':
						strncpy(tmpwrite+len,tmpwrite+2,strlen(tmpwrite+2)+1);
						strncpy(tmpwrite,"\t",len);
						break;
					default:
						opbx_log(LOG_NOTICE, "Substitution routine does not support this character: %c\n",tmpwrite[1]);
				}
				tmpread = tmpwrite+len;
			}
		}
		opbx_mutex_unlock(&vmlock);
		opbx_config_destroy(cfg);
		return 0;
	} else {
		opbx_mutex_unlock(&vmlock);
		opbx_log(LOG_WARNING, "Error reading voicemail config\n");
		return -1;
	}
}

int reload(void)
{
	return(load_config());
}

int unload_module(void)
{
	int res;
	STANDARD_HANGUP_LOCALUSERS;
	res = opbx_unregister_application(app);
	res |= opbx_unregister_application(app2);
	res |= opbx_unregister_application(app3);
	res |= opbx_unregister_application(app4);
	opbx_cli_unregister(&show_voicemail_users_cli);
	opbx_cli_unregister(&show_voicemail_zones_cli);
	opbx_uninstall_vm_functions();
	return res;
}

int load_module(void)
{
	int res;
	res = opbx_register_application(app, vm_exec, synopsis_vm, descrip_vm);
	res |= opbx_register_application(app2, vm_execmain, synopsis_vmain, descrip_vmain);
	res |= opbx_register_application(app3, vm_box_exists, synopsis_vm_box_exists, descrip_vm_box_exists);
	res |= opbx_register_application(app4, vmauthenticate, synopsis_vmauthenticate, descrip_vmauthenticate);
	if (res)
		return(res);

	if ((res=load_config())) {
		return(res);
	}

	opbx_cli_register(&show_voicemail_users_cli);
	opbx_cli_register(&show_voicemail_zones_cli);

	/* compute the location of the voicemail spool directory */
	snprintf(VM_SPOOL_DIR, sizeof(VM_SPOOL_DIR), "%s/voicemail/", opbx_config_OPBX_SPOOL_DIR);

	opbx_install_vm_functions(has_voicemail, messagecount);

#if defined(USE_ODBC_STORAGE) && !defined(EXTENDED_ODBC_STORAGE)
	opbx_log(LOG_WARNING, "The current ODBC storage table format will be changed soon."
				"Please update your tables as per the README and edit the apps/Makefile "
				"and uncomment the line containing EXTENDED_ODBC_STORAGE to enable the "
				"new table format.\n");
#endif

	return res;
}

char *description(void)
{
	return tdesc;
}

static int dialout(struct opbx_channel *chan, struct opbx_vm_user *vmu, char *num, char *outgoing_context) 
{
	int cmd = 0;
	char destination[80] = "";
	int retries = 0;

	if (!num) {
		opbx_verbose( VERBOSE_PREFIX_3 "Destination number will be entered manually\n");
		while (retries < 3 && cmd != 't') {
			destination[1] = '\0';
			destination[0] = cmd = opbx_play_and_wait(chan,"vm-enter-num-to-call");
			if (!cmd)
				destination[0] = cmd = opbx_play_and_wait(chan, "vm-then-pound");
			if (!cmd)
				destination[0] = cmd = opbx_play_and_wait(chan, "vm-star-cancel");
			if (!cmd) {
				cmd = opbx_waitfordigit(chan, 6000);
				if (cmd)
					destination[0] = cmd;
			}
			if (!cmd) {
				retries++;
			} else {

				if (cmd < 0)
					return 0;
				if (cmd == '*') {
					opbx_verbose( VERBOSE_PREFIX_3 "User hit '*' to cancel outgoing call\n");
					return 0;
				}
				if ((cmd = opbx_readstring(chan,destination + strlen(destination),sizeof(destination)-1,6000,10000,"#")) < 0) 
					retries++;
				else
					cmd = 't';
			}
		}
		if (retries >= 3) {
			return 0;
		}
		
	} else {
		opbx_verbose( VERBOSE_PREFIX_3 "Destination number is CID number '%s'\n", num);
		opbx_copy_string(destination, num, sizeof(destination));
	}

	if (!opbx_strlen_zero(destination)) {
		if (destination[strlen(destination) -1 ] == '*')
			return 0; 
		opbx_verbose( VERBOSE_PREFIX_3 "Placing outgoing call to extension '%s' in context '%s' from context '%s'\n", destination, outgoing_context, chan->context);
		opbx_copy_string(chan->exten, destination, sizeof(chan->exten));
		opbx_copy_string(chan->context, outgoing_context, sizeof(chan->context));
		chan->priority = 0;
		return 9;
	}
	return 0;
}

static int advanced_options(struct opbx_channel *chan, struct opbx_vm_user *vmu, struct vm_state *vms, int msg,
			    int option, signed char record_gain)
{
	int res = 0;
	char filename[256],*origtime, *cid, *context, *name, *num;
	struct opbx_config *msg_cfg;
	int retries = 0;

	vms->starting = 0; 
	make_file(vms->fn, sizeof(vms->fn), vms->curdir, msg);

	/* Retrieve info from VM attribute file */

	make_file(vms->fn2, sizeof(vms->fn2), vms->curdir, vms->curmsg);
	snprintf(filename,sizeof(filename), "%s.txt", vms->fn2);
	RETRIEVE(vms->curdir, vms->curmsg);
	msg_cfg = opbx_config_load(filename);
	DISPOSE(vms->curdir, vms->curmsg);
	if (!msg_cfg) {
		opbx_log(LOG_WARNING, "No message attribute file?!! (%s)\n", filename);
		return 0;
	}

	if (!(origtime = opbx_variable_retrieve(msg_cfg, "message", "origtime")))
		return 0;

	cid = opbx_variable_retrieve(msg_cfg, "message", "callerid");

	context = opbx_variable_retrieve(msg_cfg, "message", "context");
	if (!strncasecmp("macro",context,5)) /* Macro names in contexts are useless for our needs */
		context = opbx_variable_retrieve(msg_cfg, "message","macrocontext");

	if (option == 3) {

		if (!res)
			res = play_message_datetime(chan, vmu, origtime, filename);
		if (!res)
			res = play_message_callerid(chan, vms, cid, context, 0);
	} else if (option == 2) { /* Call back */

		if (!opbx_strlen_zero(cid)) {
			opbx_callerid_parse(cid, &name, &num);
			while ((res > -1) && (res != 't')) {
				switch(res) {
					case '1':
						if (num) {
							/* Dial the CID number */
							res = dialout(chan, vmu, num, vmu->callback);
							if (res)
								return 9;
						} else {
							res = '2';
						}
						break;

					case '2':
						/* Want to enter a different number, can only do this if there's a dialout context for this user */
						if (!opbx_strlen_zero(vmu->dialout)) {
							res = dialout(chan, vmu, NULL, vmu->dialout);
							if (res)
								return 9;
						} else {
							opbx_verbose( VERBOSE_PREFIX_3 "Caller can not specify callback number - no dialout context available\n");
							res = opbx_play_and_wait(chan, "vm-sorry");
						}
						return res;
					case '*':
						res = 't';
						break;
					case '3':
					case '4':
					case '5':
					case '6':
					case '7':
					case '8':
					case '9':
					case '0':

						res = opbx_play_and_wait(chan, "vm-sorry");
						retries++;
						break;
					default:
						if (num) {
							opbx_verbose( VERBOSE_PREFIX_3 "Confirm CID number '%s' is number to use for callback\n", num);
							res = opbx_play_and_wait(chan, "vm-num-i-have");
							if (!res)
								res = play_message_callerid(chan, vms, num, vmu->context, 1);
							if (!res)
								res = opbx_play_and_wait(chan, "vm-tocallnum");
							/* Only prompt for a caller-specified number if there is a dialout context specified */
							if (!opbx_strlen_zero(vmu->dialout)) {
								if (!res)
									res = opbx_play_and_wait(chan, "vm-calldiffnum");
							}
						} else {
							res = opbx_play_and_wait(chan, "vm-nonumber");
							if (!opbx_strlen_zero(vmu->dialout)) {
								if (!res)
									res = opbx_play_and_wait(chan, "vm-toenternumber");
							}
						}
						if (!res)
							res = opbx_play_and_wait(chan, "vm-star-cancel");
						if (!res)
							res = opbx_waitfordigit(chan, 6000);
						if (!res)
							retries++;
						if (retries > 3)
							res = 't';
							break; 

						}
					if (res == 't')
						res = 0;
					else if (res == '*')
						res = -1;
				}
			}

	}
	else if (option == 1) { /* Reply */
		/* Send reply directly to sender */
		if (!opbx_strlen_zero(cid)) {
			opbx_callerid_parse(cid, &name, &num);
			if (!num) {
				opbx_verbose(VERBOSE_PREFIX_3 "No CID number available, no reply sent\n");
				if (!res)
					res = opbx_play_and_wait(chan, "vm-nonumber");
				return res;
			} else {
				if (find_user(NULL, vmu->context, num)) {
					struct leave_vm_options leave_options;

					opbx_verbose(VERBOSE_PREFIX_3 "Leaving voicemail for '%s' in context '%s'\n", num, vmu->context);
					
					memset(&leave_options, 0, sizeof(leave_options));
					leave_options.record_gain = record_gain;
					res = leave_voicemail(chan, num, &leave_options);
					if (!res)
						res = 't';
					return res;
				} else {
					/* Sender has no mailbox, can't reply */
					opbx_verbose( VERBOSE_PREFIX_3 "No mailbox number '%s' in context '%s', no reply sent\n", num, vmu->context);
					opbx_play_and_wait(chan, "vm-nobox");
					res = 't';
					return res;
				}
			} 
			res = 0;
		}
	}

	opbx_config_destroy(msg_cfg);

	if (!res) {
		make_file(vms->fn, sizeof(vms->fn), vms->curdir, msg);
		vms->heard[msg] = 1;
		res = wait_file(chan, vms, vms->fn);
	}
	return res;
}
 
static int play_record_review(struct opbx_channel *chan, char *playfile, char *recordfile, int maxtime, char *fmt,
			      int outsidecaller, struct opbx_vm_user *vmu, int *duration, const char *unlockdir,
			      signed char record_gain)
{
	/* Record message & let caller review or re-record it, or set options if applicable */
 	int res = 0;
 	int cmd = 0;
 	int max_attempts = 3;
 	int attempts = 0;
 	int recorded = 0;
 	int message_exists = 0;
	signed char zero_gain = 0;
 	/* Note that urgent and private are for flagging messages as such in the future */
 
	/* barf if no pointer passed to store duration in */
	if (duration == NULL) {
		opbx_log(LOG_WARNING, "Error play_record_review called without duration pointer\n");
		return -1;
	}

 	cmd = '3';	 /* Want to start by recording */
 
	while ((cmd >= 0) && (cmd != 't')) {
		switch (cmd) {
		case '1':
			if (!message_exists) {
 				/* In this case, 1 is to record a message */
 				cmd = '3';
 				break;
 			} else {
 				/* Otherwise 1 is to save the existing message */
 				opbx_verbose(VERBOSE_PREFIX_3 "Saving message as is\n");
 				opbx_streamfile(chan, "vm-msgsaved", chan->language);
 				opbx_waitstream(chan, "");
				STORE(recordfile, vmu->mailbox, vmu->context, -1);
				DISPOSE(recordfile, -1);
 				cmd = 't';
 				return res;
 			}
 		case '2':
 			/* Review */
 			opbx_verbose(VERBOSE_PREFIX_3 "Reviewing the message\n");
 			opbx_streamfile(chan, recordfile, chan->language);
 			cmd = opbx_waitstream(chan, OPBX_DIGIT_ANY);
 			break;
 		case '3':
 			message_exists = 0;
 			/* Record */
 			if (recorded == 1)
				opbx_verbose(VERBOSE_PREFIX_3 "Re-recording the message\n");
 			else	
				opbx_verbose(VERBOSE_PREFIX_3 "Recording the message\n");
			if (recorded && outsidecaller) {
 				cmd = opbx_play_and_wait(chan, INTRO);
 				cmd = opbx_play_and_wait(chan, "beep");
 			}
 			recorded = 1;
 			/* After an attempt has been made to record message, we have to take care of INTRO and beep for incoming messages, but not for greetings */
			if (record_gain)
				opbx_channel_setoption(chan, OPBX_OPTION_RXGAIN, &record_gain, sizeof(record_gain), 0);
			cmd = opbx_play_and_record(chan, playfile, recordfile, maxtime, fmt, duration, silencethreshold, maxsilence, unlockdir);
			if (record_gain)
				opbx_channel_setoption(chan, OPBX_OPTION_RXGAIN, &zero_gain, sizeof(zero_gain), 0);
 			if (cmd == -1) {
 			/* User has hung up, no options to give */
 				return cmd;
			}
 			if (cmd == '0') {
 				break;
 			} else if (cmd == '*') {
 				break;
 			} 
#if 0			
 			else if (vmu->review && (*duration < 5)) {
 				/* Message is too short */
 				opbx_verbose(VERBOSE_PREFIX_3 "Message too short\n");
				cmd = opbx_play_and_wait(chan, "vm-tooshort");
 				cmd = vm_delete(recordfile);
 				break;
 			}
 			else if (vmu->review && (cmd == 2 && *duration < (maxsilence + 3))) {
 				/* Message is all silence */
 				opbx_verbose(VERBOSE_PREFIX_3 "Nothing recorded\n");
 				cmd = vm_delete(recordfile);
				cmd = opbx_play_and_wait(chan, "vm-nothingrecorded");
				if (!cmd)
 					cmd = opbx_play_and_wait(chan, "vm-speakup");
 				break;
 			}
#endif
 			else {
 				/* If all is well, a message exists */
 				message_exists = 1;
				cmd = 0;
 			}
 			break;
 		case '4':
 		case '5':
 		case '6':
 		case '7':
 		case '8':
 		case '9':
		case '*':
		case '#':
 			cmd = opbx_play_and_wait(chan, "vm-sorry");
 			break;
#if 0 
/*  XXX Commented out for the moment because of the dangers of deleting
    a message while recording (can put the message numbers out of sync) */
 		case '*':
 			/* Cancel recording, delete message, offer to take another message*/
 			cmd = opbx_play_and_wait(chan, "vm-deleted");
 			cmd = vm_delete(recordfile);
 			if (outsidecaller) {
 				res = vm_exec(chan, NULL);
 				return res;
 			}
 			else
 				return 1;
#endif
 		case '0':
			if (message_exists || recorded) {
				cmd = opbx_play_and_wait(chan, "vm-saveoper");
				if (!cmd)
					cmd = opbx_waitfordigit(chan, 3000);
				if (cmd == '1') {
					opbx_play_and_wait(chan, "vm-msgsaved");
					cmd = '0';
				} else {
					opbx_play_and_wait(chan, "vm-deleted");
					DELETE(recordfile, -1, recordfile);
					cmd = '0';
				}
			}
			return cmd;
 		default:
			/* If the caller is an ouside caller, and the review option is enabled,
			   allow them to review the message, but let the owner of the box review
			   their OGM's */
			if (outsidecaller && !opbx_test_flag(vmu, VM_REVIEW))
				return cmd;
 			if (message_exists) {
 				cmd = opbx_play_and_wait(chan, "vm-review");
 			}
 			else {
 				cmd = opbx_play_and_wait(chan, "vm-torerecord");
 				if (!cmd)
 					cmd = opbx_waitfordigit(chan, 600);
 			}
 			
 			if (!cmd && outsidecaller && opbx_test_flag(vmu, VM_OPERATOR)) {
 				cmd = opbx_play_and_wait(chan, "vm-reachoper");
 				if (!cmd)
 					cmd = opbx_waitfordigit(chan, 600);
 			}
#if 0
			if (!cmd)
 				cmd = opbx_play_and_wait(chan, "vm-tocancelmsg");
#endif
 			if (!cmd)
 				cmd = opbx_waitfordigit(chan, 6000);
 			if (!cmd) {
 				attempts++;
 			}
 			if (attempts > max_attempts) {
 				cmd = 't';
 			}
 		}
 	}
 	if (outsidecaller)  
		opbx_play_and_wait(chan, "vm-goodbye");
 	if (cmd == 't')
 		cmd = 0;
 	return cmd;
 }
 

int usecount(void)
{
	int res;
	STANDARD_USECOUNT(res);
	return res;
}



