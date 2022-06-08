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
 * \brief Provide a directory of extensions
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h> 
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/lock.h"
#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/config.h"
#include "openpbx/say.h"
#include "openpbx/utils.h"

static char *tdesc = "Extension Directory";
static char *app = "Directory";

static char *synopsis = "Provide directory of voicemail extensions";
static char *descrip =
"  Directory(vm-context[|dial-context[|options]]): Presents the user with a directory\n"
"of extensions from which they  may  select  by name. The  list  of  names \n"
"and  extensions  is discovered from  voicemail.conf. The  vm-context  argument\n"
"is required, and specifies  the  context  of voicemail.conf to use.  The\n"
"dial-context is the context to use for dialing the users, and defaults to\n"
"the vm-context if unspecified. The 'f' option causes the directory to match\n"
"based on the first name in voicemail.conf instead of the last name.\n"
"Returns 0 unless the user hangs up. It  also sets up the channel on exit\n"
"to enter the extension the user selected.  If the user enters '0' and there\n"
"exists an extension 'o' in the current context, the directory will exit with 0\n"
"and call control will resume at that extension.  Entering '*' will exit similarly,\n"
"but to the 'a' extension, much like app_voicemail's behavior.\n";

/* For simplicity, I'm keeping the format compatible with the voicemail config,
   but i'm open to suggestions for isolating it */

#define VOICEMAIL_CONFIG "voicemail.conf"

/* How many digits to read in */
#define NUMDIGITS 3

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static char *convert(char *lastname)
{
	char *tmp;
	int lcount = 0;
	tmp = malloc(NUMDIGITS + 1);
	if (tmp) {
		while((*lastname > 32) && lcount < NUMDIGITS) {
			switch(toupper(*lastname)) {
			case '1':
				tmp[lcount++] = '1';
				break;
			case '2':
			case 'A':
			case 'B':
			case 'C':
				tmp[lcount++] = '2';
				break;
			case '3':
			case 'D':
			case 'E':
			case 'F':
				tmp[lcount++] = '3';
				break;
			case '4':
			case 'G':
			case 'H':
			case 'I':
				tmp[lcount++] = '4';
				break;
			case '5':
			case 'J':
			case 'K':
			case 'L':
				tmp[lcount++] = '5';
				break;
			case '6':
			case 'M':
			case 'N':
			case 'O':
				tmp[lcount++] = '6';
				break;
			case '7':
			case 'P':
			case 'Q':
			case 'R':
			case 'S':
				tmp[lcount++] = '7';
				break;
			case '8':
			case 'T':
			case 'U':
			case 'V':
				tmp[lcount++] = '8';
				break;
			case '9':
			case 'W':
			case 'X':
			case 'Y':
			case 'Z':
				tmp[lcount++] = '9';
				break;
			}
			lastname++;
		}
		tmp[lcount] = '\0';
	}
	return tmp;
}

/* play name of mailbox owner.
 * returns:  -1 for bad or missing extension
 *           '1' for selected entry from directory
 *           '*' for skipped entry from directory
 */
static int play_mailbox_owner(struct opbx_channel *chan, char *context, char *dialcontext, char *ext, char *name) {
	int res = 0;
	int loop = 3;
	char fn[256];
	char fn2[256];

	/* Check for the VoiceMail2 greeting first */
	snprintf(fn, sizeof(fn), "%s/voicemail/%s/%s/greet",
		(char *)opbx_config_OPBX_SPOOL_DIR, context, ext);

	/* Otherwise, check for an old-style Voicemail greeting */
	snprintf(fn2, sizeof(fn2), "%s/vm/%s/greet",
		(char *)opbx_config_OPBX_SPOOL_DIR, ext);

	if (opbx_fileexists(fn, NULL, chan->language) > 0) {
		res = opbx_streamfile(chan, fn, chan->language);
		if (!res) {
			res = opbx_waitstream(chan, OPBX_DIGIT_ANY);
		}
		opbx_stopstream(chan);
	} else if (opbx_fileexists(fn2, NULL, chan->language) > 0) {
		res = opbx_streamfile(chan, fn2, chan->language);
		if (!res) {
			res = opbx_waitstream(chan, OPBX_DIGIT_ANY);
		}
		opbx_stopstream(chan);
	} else {
		res = opbx_say_character_str(chan, !opbx_strlen_zero(name) ? name : ext,
					OPBX_DIGIT_ANY, chan->language);
	}

	while (loop) {
		if (!res) {
			res = opbx_streamfile(chan, "dir-instr", chan->language);
		}
		if (!res) {
			res = opbx_waitstream(chan, OPBX_DIGIT_ANY);
		}
		if (!res) {
			res = opbx_waitfordigit(chan, 3000);
		}
		opbx_stopstream(chan);
	
		if (res > -1) {
			switch (res) {
				case '1':
					/* Name selected */
					loop = 0;
					if (opbx_goto_if_exists(chan, dialcontext, ext, 1)) {
						opbx_log(LOG_WARNING,
							"Can't find extension '%s' in context '%s'.  "
							"Did you pass the wrong context to Directory?\n",
							ext, dialcontext);
						res = -1;
					}
					break;
	
				case '*':   
					/* Skip to next match in list */
					loop = 0;
					break;
	
				default:
					/* Not '1', or '*', so decrement number of tries */
					res = 0;
					loop--;
					break;
			} /* end switch */
		} /* end if */
		else {
			/* User hungup, so jump out now */
			loop = 0;
		}
	} /* end while */

	return(res);
}

static struct opbx_config *realtime_directory(char *context)
{
	struct opbx_config *cfg;
	struct opbx_config *rtdata;
	struct opbx_category *cat;
	struct opbx_variable *var;
	char *mailbox;
	char *fullname;
	char *hidefromdir;
	char tmp[100];

	/* Load flat file config. */
	cfg = opbx_config_load(VOICEMAIL_CONFIG);

	if (!cfg) {
		/* Loading config failed. */
		opbx_log(LOG_WARNING, "Loading config failed.\n");
		return NULL;
	}

	/* Get realtime entries, categorized by their mailbox number
	   and present in the requested context */
	rtdata = opbx_load_realtime_multientry("voicemail", "mailbox LIKE", "%", "context", context, NULL);

	/* if there are no results, just return the entries from the config file */
	if (!rtdata)
		return cfg;

	/* Does the context exist within the config file? If not, make one */
	cat = opbx_category_get(cfg, context);
	if (!cat) {
		cat = opbx_category_new(context);
		if (!cat) {
			opbx_log(LOG_WARNING, "Out of memory\n");
			opbx_config_destroy(cfg);
			return NULL;
		}
		opbx_category_append(cfg, cat);
	}

	mailbox = opbx_category_browse(rtdata, NULL);
	while (mailbox) {
		fullname = opbx_variable_retrieve(rtdata, mailbox, "fullname");
		hidefromdir = opbx_variable_retrieve(rtdata, mailbox, "hidefromdir");
		snprintf(tmp, sizeof(tmp), "no-password,%s,hidefromdir=%s",
			 fullname ? fullname : "",
			 hidefromdir ? hidefromdir : "no");
		var = opbx_variable_new(mailbox, tmp);
		if (var)
			opbx_variable_append(cat, var);
		else
			opbx_log(LOG_WARNING, "Out of memory adding mailbox '%s'\n", mailbox);
		mailbox = opbx_category_browse(rtdata, mailbox);
	}
	opbx_config_destroy(rtdata);

	return cfg;
}

static int do_directory(struct opbx_channel *chan, struct opbx_config *cfg, char *context, char *dialcontext, char digit, int last)
{
	/* Read in the first three digits..  "digit" is the first digit, already read */
	char ext[NUMDIGITS + 1];
	char name[80] = "";
	struct opbx_variable *v;
	int res;
	int found=0;
	int lastuserchoice = 0;
	char *start, *pos, *conv,*stringp=NULL;

	if (opbx_strlen_zero(context)) {
		opbx_log(LOG_WARNING,
			"Directory must be called with an argument "
			"(context in which to interpret extensions)\n");
		return -1;
	}
	if (digit == '0') {
		if (!opbx_goto_if_exists(chan, chan->context, "o", 1) ||
		    (!opbx_strlen_zero(chan->macrocontext) &&
		     !opbx_goto_if_exists(chan, chan->macrocontext, "o", 1))) {
			return 0;
		} else {
			opbx_log(LOG_WARNING, "Can't find extension 'o' in current context.  "
				"Not Exiting the Directory!\n");
			res = 0;
		}
	}	
	if (digit == '*') {
		if (!opbx_goto_if_exists(chan, chan->context, "a", 1) ||
		    (!opbx_strlen_zero(chan->macrocontext) &&
		     !opbx_goto_if_exists(chan, chan->macrocontext, "a", 1))) {
			return 0;
		} else {
			opbx_log(LOG_WARNING, "Can't find extension 'a' in current context.  "
				"Not Exiting the Directory!\n");
			res = 0;
		}
	}	
	memset(ext, 0, sizeof(ext));
	ext[0] = digit;
	res = 0;
	if (opbx_readstring(chan, ext + 1, NUMDIGITS - 1, 3000, 3000, "#") < 0) res = -1;
	if (!res) {
		/* Search for all names which start with those digits */
		v = opbx_variable_browse(cfg, context);
		while(v && !res) {
			/* Find all candidate extensions */
			while(v) {
				/* Find a candidate extension */
				start = strdup(v->value);
				if (start && !strcasestr(start, "hidefromdir=yes")) {
					stringp=start;
					strsep(&stringp, ",");
					pos = strsep(&stringp, ",");
					if (pos) {
						opbx_copy_string(name, pos, sizeof(name));
						/* Grab the last name */
						if (last && strrchr(pos,' '))
							pos = strrchr(pos, ' ') + 1;
						conv = convert(pos);
						if (conv) {
							if (!strcmp(conv, ext)) {
								/* Match! */
								found++;
								free(conv);
								free(start);
								break;
							}
							free(conv);
						}
					}
					free(start);
				}
				v = v->next;
			}

			if (v) {
				/* We have a match -- play a greeting if they have it */
				res = play_mailbox_owner(chan, context, dialcontext, v->name, name);
				switch (res) {
					case -1:
						/* user pressed '1' but extension does not exist, or
						 * user hungup
						 */
						lastuserchoice = 0;
						break;
					case '1':
						/* user pressed '1' and extensions exists;
						   play_mailbox_owner will already have done
						   a goto() on the channel
						 */
						lastuserchoice = res;
						break;
					case '*':
						/* user pressed '*' to skip something found */
						lastuserchoice = res;
						res = 0;
						break;
					default:
						break;
				}
				v = v->next;
			}
		}

		if (lastuserchoice != '1') {
			if (found) 
				res = opbx_streamfile(chan, "dir-nomore", chan->language);
			else
				res = opbx_streamfile(chan, "dir-nomatch", chan->language);
			if (!res)
				res = 1;
			return res;
		}
		return 0;
	}
	return res;
}

static int directory_exec(struct opbx_channel *chan, void *data)
{
	int res = 0;
	struct localuser *u;
	struct opbx_config *cfg;
	int last = 1;
	char *context, *dialcontext, *dirintro, *options;

	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "Directory requires an argument (context[,dialcontext])\n");
		return -1;
	}

	LOCAL_USER_ADD(u);

	context = opbx_strdupa(data);
	dialcontext = strchr(context, '|');
	if (dialcontext) {
		*dialcontext = '\0';
		dialcontext++;
		options = strchr(dialcontext, '|');
		if (options) {
			*options = '\0';
			options++; 
			if (strchr(options, 'f'))
				last = 0;
		}
	} else	
		dialcontext = context;

	cfg = realtime_directory(context);
	if (!cfg) {
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	dirintro = opbx_variable_retrieve(cfg, context, "directoryintro");
	if (opbx_strlen_zero(dirintro))
		dirintro = opbx_variable_retrieve(cfg, "general", "directoryintro");
	if (opbx_strlen_zero(dirintro)) {
		if (last)
			dirintro = "dir-intro";	
		else
			dirintro = "dir-intro-fn";
	}
	
	for (;;) {
		if (!res)
			res = opbx_streamfile(chan, dirintro, chan->language);
		if (!res)
			res = opbx_waitstream(chan, OPBX_DIGIT_ANY);
		opbx_stopstream(chan);
		if (!res)
			res = opbx_waitfordigit(chan, 5000);
		if (res >0) {
			res = do_directory(chan, cfg, context, dialcontext, res, last);
			if (res > 0){
				res = opbx_waitstream(chan, OPBX_DIGIT_ANY);
				opbx_stopstream(chan);
				if (res >= 0) {
					continue;
				}
			}
		}
		break;
	}
	opbx_config_destroy(cfg);
	LOCAL_USER_REMOVE(u);
	return res;
}

int unload_module(void)
{
	STANDARD_HANGUP_LOCALUSERS;
	return opbx_unregister_application(app);
}

int load_module(void)
{
	return opbx_register_application(app, directory_exec, synopsis, descrip);
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


