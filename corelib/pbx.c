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
 * \brief Core PBX routines.
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <sys/types.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/lock.h"
#include "openpbx/cli.h"
#include "openpbx/pbx.h"
#include "openpbx/channel.h"
#include "openpbx/options.h"
#include "openpbx/logger.h"
#include "openpbx/file.h"
#include "openpbx/phone_no_utils.h"
#include "openpbx/cdr.h"
#include "openpbx/config.h"
#include "openpbx/term.h"
#include "openpbx/manager.h"
#include "openpbx/opbx_expr.h"
#include "openpbx/linkedlists.h"
#include "openpbx/say.h"
#include "openpbx/utils.h"
#include "openpbx/causes.h"
#include "openpbx/musiconhold.h"
#include "openpbx/app.h"
#include "openpbx/devicestate.h"
#include "openpbx/opbx_hash.h"
#include "openpbx/opbx_keywords.h"

/*!
 * \note I M P O R T A N T :
 *
 *		The speed of extension handling will likely be among the most important
 * aspects of this PBX.  The switching scheme as it exists right now isn't
 * terribly bad (it's O(N+M), where N is the # of extensions and M is the avg #
 * of priorities, but a constant search time here would be great ;-) 
 *
 */

/*!
 * \note I M P O R T A N T  V . 2 :
 *
 *		This file has been converted towards a hash code based system to
 * recognise identifiers, which is precisely what the original author should
 * have done to address their concern stated in the above IMPORTANT note.
 *
 *		As a result of the change to the hash code based system, application
 * and variable names are no longer case insensitive. If the old behaviour is
 * desired, this file should be compiled with the following macros defined:
 *
 *		o  OPBX_USE_CASE_INSENSITIVE_APP_NAMES
 *		o  OPBX_USE_CASE_INSENSITIVE_VAR_NAMES
 *
 */

#ifdef OPBX_USE_CASE_INSENSITIVE_APP_NAMES
#define opbx_hash_app_name(x)	opbx_hash_string_toupper(x)
#define OPBX_CASE_INFO_STRING_FOR_APP_NAMES	"insensitive"
#else
#define opbx_hash_app_name(x)	opbx_hash_string(x)
#define OPBX_CASE_INFO_STRING_FOR_APP_NAMES	"sensitive"
#endif

#ifdef OPBX_USE_CASE_INSENSITIVE_VAR_NAMES
#define opbx_hash_var_name(x)	opbx_hash_string_toupper(x)
#define OPBX_CASE_INFO_STRING_FOR_VAR_NAMES	"insensitive"
#else
#define opbx_hash_var_name(x)	opbx_hash_string(x)
#define OPBX_CASE_INFO_STRING_FOR_VAR_NAMES	"sensitive"
#endif

#ifdef LOW_MEMORY
#define EXT_DATA_SIZE 256
#else
#define EXT_DATA_SIZE 8192
#endif

#define SWITCH_DATA_LENGTH 256

#define VAR_BUF_SIZE 4096

#define	VAR_NORMAL		1
#define	VAR_SOFTTRAN	2
#define	VAR_HARDTRAN	3

#define BACKGROUND_SKIP		(1 << 0)
#define BACKGROUND_NOANSWER	(1 << 1)
#define BACKGROUND_MATCHEXTEN	(1 << 2)
#define BACKGROUND_PLAYBACK	(1 << 3)

OPBX_DECLARE_OPTIONS(background_opts,{
	['s'] = { BACKGROUND_SKIP },
	['n'] = { BACKGROUND_NOANSWER },
	['m'] = { BACKGROUND_MATCHEXTEN },
	['p'] = { BACKGROUND_PLAYBACK },
});

#define WAITEXTEN_MOH		(1 << 0)

OPBX_DECLARE_OPTIONS(waitexten_opts,{
	['m'] = { WAITEXTEN_MOH, 1 },
});

struct opbx_context;

/* opbx_exten: An extension */
struct opbx_exten
{
	char *exten;				/* Extension name -- shouldn't this be called "ident" ? */
	unsigned int hash;			/* Hashed identifier */
	int matchcid;				/* Match caller id ? */
	char *cidmatch;				/* Caller id to match for this extension */
	int priority;				/* Priority */
	char *label;				/* Label */
	struct opbx_context *parent;	/* The context this extension belongs to  */
	char *app;					/* Application to execute */
	void *data;					/* Data to use (arguments) */
	void (*datad)(void *);		/* Data destructor */
	struct opbx_exten *peer;	/* Next higher priority with our extension */
	const char *registrar;		/* Registrar */
	struct opbx_exten *next;	/* Extension with a greater ID */
	char stuff[0];
};

/* opbx_include: include= support in extensions.conf */
struct opbx_include
{
	char *name;		
	char *rname;				/* Context to include */
	const char *registrar;		/* Registrar */
	int hastime;				/* If time construct exists */
	struct opbx_timing timing;	/* time construct */
	struct opbx_include *next;	/* Link them together */
	char stuff[0];
};

/* opbx_sw: Switch statement in extensions.conf */
struct opbx_sw
{
	char *name;
	const char *registrar;		/* Registrar */
	char *data;					/* Data load */
	int eval;
	struct opbx_sw *next;		/* Link them together */
	char *tmpdata;
	char stuff[0];
};

struct opbx_ignorepat
{
	const char *registrar;
	struct opbx_ignorepat *next;
	char pattern[0];
};

/* opbx_context: An extension context */
struct opbx_context
{
	opbx_mutex_t lock; 			/* A lock to prevent multiple threads from clobbering the context */
	unsigned int hash;			/* Hashed context name */
	struct opbx_exten *root;	/* The root of the list of extensions */
	struct opbx_context *next;	/* Link them together */
	struct opbx_include *includes;	/* Include other contexts */
	struct opbx_ignorepat *ignorepats;	/* Patterns for which to continue playing dialtone */
	const char *registrar;		/* Registrar */
	struct opbx_sw *alts;		/* Alternative switches */
	char name[0];				/* Name of the context */
};

/* opbx_app: An application */
struct opbx_app
{
	int (*execute)(struct opbx_channel *chan, void *data);
	unsigned int hash;			/* Hashed application name */
	const char *synopsis;		/* Synopsis text for 'show applications' */
	const char *description;	/* Description (help text) for 'show application <name>' */
	struct opbx_app *next;		/* Next app in list */
	char name[0];				/* Name of the application */
};

/* opbx_state_cb: An extension state notify */
struct opbx_state_cb
{
	int id;
	void *data;
	opbx_state_cb_type callback;
	struct opbx_state_cb *next;
};
	    
/* Hints are pointers from an extension in the dialplan to one or more devices (tech/name) */
struct opbx_hint
{
	struct opbx_exten *exten;	/* Extension */
	int laststate;				/* Last known state */
	struct opbx_state_cb *callbacks;	/* Callback list for this extension */
	struct opbx_hint *next;		/* Pointer to next hint in list */
};

int opbx_pbx_outgoing_cdr_failed(void);

static int pbx_builtin_prefix(struct opbx_channel *, void *);
static int pbx_builtin_suffix(struct opbx_channel *, void *);
static int pbx_builtin_stripmsd(struct opbx_channel *, void *);
static int pbx_builtin_answer(struct opbx_channel *, void *);
static int pbx_builtin_goto(struct opbx_channel *, void *);
static int pbx_builtin_hangup(struct opbx_channel *, void *);
static int pbx_builtin_background(struct opbx_channel *, void *);
static int pbx_builtin_dtimeout(struct opbx_channel *, void *);
static int pbx_builtin_rtimeout(struct opbx_channel *, void *);
static int pbx_builtin_atimeout(struct opbx_channel *, void *);
static int pbx_builtin_wait(struct opbx_channel *, void *);
static int pbx_builtin_waitexten(struct opbx_channel *, void *);
static int pbx_builtin_setlanguage(struct opbx_channel *, void *);
static int pbx_builtin_resetcdr(struct opbx_channel *, void *);
static int pbx_builtin_setaccount(struct opbx_channel *, void *);
static int pbx_builtin_setamaflags(struct opbx_channel *, void *);
static int pbx_builtin_ringing(struct opbx_channel *, void *);
static int pbx_builtin_progress(struct opbx_channel *, void *);
static int pbx_builtin_congestion(struct opbx_channel *, void *);
static int pbx_builtin_busy(struct opbx_channel *, void *);
static int pbx_builtin_setglobalvar(struct opbx_channel *, void *);
static int pbx_builtin_noop(struct opbx_channel *, void *);
static int pbx_builtin_gotoif(struct opbx_channel *, void *);
static int pbx_builtin_gotoiftime(struct opbx_channel *, void *);
static int pbx_builtin_execiftime(struct opbx_channel *, void *);
static int pbx_builtin_saynumber(struct opbx_channel *, void *);
static int pbx_builtin_saydigits(struct opbx_channel *, void *);
static int pbx_builtin_saycharacters(struct opbx_channel *, void *);
static int pbx_builtin_sayphonetic(struct opbx_channel *, void *);
static int pbx_builtin_setvar_old(struct opbx_channel *, void *);
int pbx_builtin_setvar(struct opbx_channel *, void *);
static int pbx_builtin_importvar(struct opbx_channel *, void *);

static struct varshead globals;

static int autofallthrough = 0;

OPBX_MUTEX_DEFINE_STATIC(maxcalllock);
static int countcalls = 0;

OPBX_MUTEX_DEFINE_STATIC(acflock); 		/* Lock for the custom function list */
static struct opbx_custom_function *acf_root = NULL;

static struct pbx_builtin {
	char name[OPBX_MAX_APP];
	int (*execute)(struct opbx_channel *chan, void *data);
	char *synopsis;
	char *description;
} builtins[] = 
{
	/* These applications are built into the PBX core and do not
	   need separate modules */

	{ "AbsoluteTimeout", pbx_builtin_atimeout,
	"Set absolute maximum time of call",
	"  AbsoluteTimeout(seconds): Set the absolute maximum amount of time permitted\n"
	"for a call.  A setting of 0 disables the timeout.  Always returns 0.\n" 
	"AbsoluteTimeout has been deprecated in favor of Set(TIMEOUT(absolute)=timeout)\n"
	},

	{ "Answer", pbx_builtin_answer, 
	"Answer a channel if ringing", 
	"  Answer([delay]): If the channel is ringing, answer it, otherwise do nothing. \n"
	"If delay is specified, openpbx will pause execution for the specified amount\n"
	"of milliseconds if an answer is required, in order to give audio a chance to\n"
	"become ready. Returns 0 unless it tries to answer the channel and fails.\n"   
	},

	{ "Background", pbx_builtin_background,
	"Play a file while awaiting extension",
	"  Background(filename1[&filename2...][|options[|langoverride][|context]]):\n"
	"Plays given files, while simultaneously waiting for the user to begin typing\n"
	"an extension. The timeouts do not count until the last BackGround\n"
	"application has ended. Options may also be included following a pipe \n"
	"symbol. The 'langoverride' may be a language to use for playing the prompt\n"
	"which differs from the current language of the channel.  The optional\n"
	"'context' can be used to specify an optional context to exit into.\n"
	"Returns -1 if thhe channel was hung up, or if the file does not exist./n"
	"Returns 0 otherwise.\n\n"
	"  Options:\n"
	"    's' - causes the playback of the message to be skipped\n"
	"          if the channel is not in the 'up' state (i.e. it\n"
	"          hasn't been answered yet.) If this happens, the\n"
	"          application will return immediately.\n"
	"    'n' - don't answer the channel before playing the files\n"
	"    'm' - only break if a digit hit matches a one digit\n"
	"		 extension in the destination context\n"
	},

	{ "Busy", pbx_builtin_busy,
	"Indicate busy condition and stop",
	"  Busy([timeout]): Requests that the channel indicate busy condition and\n"
	"then waits for the user to hang up or the optional timeout to expire.\n"
	"Always returns -1." 
	},

	{ "Congestion", pbx_builtin_congestion,
	"Indicate congestion and stop",
	"  Congestion([timeout]): Requests that the channel indicate congestion\n"
	"and then waits for the user to hang up or for the optional timeout to\n"
	"expire.  Always returns -1." 
	},

	{ "DigitTimeout", pbx_builtin_dtimeout,
	"Set maximum timeout between digits",
	"  DigitTimeout(seconds): Set the maximum amount of time permitted between\n"
	"digits when the user is typing in an extension. When this timeout expires,\n"
	"after the user has started to type in an extension, the extension will be\n"
	"considered complete, and will be interpreted. Note that if an extension\n"
	"typed in is valid, it will not have to timeout to be tested, so typically\n"
	"at the expiry of this timeout, the extension will be considered invalid\n"
	"(and thus control would be passed to the 'i' extension, or if it doesn't\n"
	"exist the call would be terminated). The default timeout is 5 seconds.\n"
	"Always returns 0.\n" 
	"DigitTimeout has been deprecated in favor of Set(TIMEOUT(digit)=timeout)\n"
	},

	{ "Goto", pbx_builtin_goto, 
	"Goto a particular priority, extension, or context",
	"  Goto([[context|]extension|]priority):  Set the  priority to the specified\n"
	"value, optionally setting the extension and optionally the context as well.\n"
	"The extension BYEXTENSION is special in that it uses the current extension,\n"
	"thus  permitting you to go to a different context, without specifying a\n"
	"specific extension. Always returns 0, even if the given context, extension,\n"
	"or priority is invalid.\n" 
	},

	{ "GotoIf", pbx_builtin_gotoif,
	"Conditional goto",
	"  GotoIf(Condition?label1:label2): Go to label 1 if condition is\n"
	"true, to label2 if condition is false. Either label1 or label2 may be\n"
	"omitted (in that case, we just don't take the particular branch) but not\n"
	"both. Look for the condition syntax in examples or documentation." 
	},

	{ "GotoIfTime", pbx_builtin_gotoiftime,
	"Conditional goto on current time",
	"  GotoIfTime(<times>|<weekdays>|<mdays>|<months>?[[context|]extension|]pri):\n"
	"If the current time matches the specified time, then branch to the specified\n"
	"extension. Each of the elements may be specified either as '*' (for always)\n"
	"or as a range. See the 'include' syntax for details." 
	},

	{ "ExecIfTime", pbx_builtin_execiftime,
	"Conditional application execution on current time",
	"  ExecIfTime(<times>|<weekdays>|<mdays>|<months>?<appname>[|<appdata>]):\n"
	"If the current time matches the specified time, then execute the specified\n"
	"application. Each of the elements may be specified either as '*' (for always)\n"
	"or as a range. See the 'include' syntax for details. It will return whatever\n"
	"<appname> returns, or a non-zero value if the application is not found.\n"
	},
	
	{ "Hangup", pbx_builtin_hangup,
	"Unconditional hangup",
	"  Hangup(): Unconditionally hangs up a given channel by returning -1 always.\n" 
	},

	{ "NoOp", pbx_builtin_noop,
	"No operation",
        "  NoOp(): No-operation; Does nothing except relaxing the dialplan and \n"
        "re-scheduling over threads. It's necessary and very useful in tight loops." 
	},

	{ "Prefix", pbx_builtin_prefix, 
	"Prepend leading digits",
	"  Prefix(digits): Prepends the digit string specified by digits to the\n"
	"channel's associated extension. For example, the number 1212 when prefixed\n"
	"with '555' will become 5551212. This app always returns 0, and the PBX will\n"
	"continue processing at the next priority for the *new* extension.\n"
	"  So, for example, if priority  3  of 1212 is  Prefix  555, the next step\n"
	"executed will be priority 4 of 5551212. If you switch into an extension\n"
	"which has no first step, the PBX will treat it as though the user dialed an\n"
	"invalid extension.\n" 
	},

	{ "Progress", pbx_builtin_progress,
	"Indicate progress",
	"  Progress(): Request that the channel indicate in-band progress is \n"
	"available to the user.\nAlways returns 0.\n" 
	},

	{ "ResetCDR", pbx_builtin_resetcdr,
	"Resets the Call Data Record",
	"  ResetCDR([options]):  Causes the Call Data Record to be reset, optionally\n"
	"storing the current CDR before zeroing it out\b"
	" - if 'w' option is specified record will be stored.\n"
	" - if 'a' option is specified any stacked records will be stored.\n"
	" - if 'v' option is specified any variables will be saved.\n"
	"Always returns 0.\n"  
	},

	{ "ResponseTimeout", pbx_builtin_rtimeout,
	"Set maximum timeout awaiting response",
	"  ResponseTimeout(seconds): Set the maximum amount of time permitted after\n"
	"falling through a series of priorities for a channel in which the user may\n"
	"begin typing an extension. If the user does not type an extension in this\n"
	"amount of time, control will pass to the 't' extension if it exists, and\n"
	"if not the call would be terminated. The default timeout is 10 seconds.\n"
	"Always returns 0.\n"  
	"ResponseTimeout has been deprecated in favor of Set(TIMEOUT(response)=timeout)\n"
	},

	{ "Ringing", pbx_builtin_ringing,
	"Indicate ringing tone",
	"  Ringing(): Request that the channel indicate ringing tone to the user.\n"
	"Always returns 0.\n" 
	},

	{ "SayNumber", pbx_builtin_saynumber,
	"Say Number",
	"  SayNumber(digits[,gender]): Says the passed number. SayNumber is using\n" 
	"the current language setting for the channel. (See app SetLanguage).\n"
	},

	{ "SayDigits", pbx_builtin_saydigits,
	"Say Digits",
	"  SayDigits(digits): Says the passed digits. SayDigits is using the\n" 
	"current language setting for the channel. (See app setLanguage)\n"
	},

	{ "SayAlpha", pbx_builtin_saycharacters,
	"Say Alpha",
	"  SayAlpha(string): Spells the passed string\n" 
	},

	{ "SayPhonetic", pbx_builtin_sayphonetic,
	"Say Phonetic",
	"  SayPhonetic(string): Spells the passed string with phonetic alphabet\n" 
	},

	{ "SetAccount", pbx_builtin_setaccount,
	"Sets account code",
	"  SetAccount([account]): Set the channel account code for billing\n"
	"purposes. Always returns 0.\n"
	},

	{ "SetAMAFlags", pbx_builtin_setamaflags,
	"Sets AMA Flags",
	"  SetAMAFlags([flag]): Set the channel AMA Flags for billing\n"
	"purposes. Always returns 0.\n"
	},

	{ "SetGlobalVar", pbx_builtin_setglobalvar,
	"Set global variable to value",
	"  SetGlobalVar(#n=value): Sets global variable n to value. Global\n" 
	"variable are available across channels.\n"
	},

	{ "SetLanguage", pbx_builtin_setlanguage,
	"Sets channel language",
	"  SetLanguage(language): Set the channel language to 'language'. This\n"
	"information is used for the syntax in generation of numbers, and to choose\n"
	"a natural language file when available.\n"
	"  For example, if language is set to 'fr' and the file 'demo-congrats' is \n"
	"requested to be played, if the file 'fr/demo-congrats' exists, then\n"
	"it will play that file, and if not will play the normal 'demo-congrats'.\n"
	"For some language codes, SetLanguage also changes the syntax of some\n"
	"OpenPBX functions, like SayNumber.\n"
	"Always returns 0.\n"
	"SetLanguage has been deprecated in favor of Set(LANGUAGE()=language)\n"
	},

	{ "Set", pbx_builtin_setvar,
	  "Set channel variable(s) or function value(s)",
	  "  Set(name1=value1|name2=value2|..[|options])\n"
	  "This function can be used to set the value of channel variables\n"
	  "or dialplan functions. It will accept up to 24 name/value pairs.\n"
	  "When setting variables, if the variable name is prefixed with _,\n"
	  "the variable will be inherited into channels created from the\n"
	  "current channel. If the variable name is prefixed with __,\n"
	  "the variable will be inherited into channels created from the\n"
	  "current channel and all child channels.\n"
	  "The last argument, if it does not contain '=', is interpreted\n"
	  "as a string of options. The valid options are:\n"
	  "  g - Set variable globally instead of on the channel\n"
	  "      (applies only to variables, not functions)\n"
	},

	{ "SetVar", pbx_builtin_setvar_old,
	  "Set channel variable(s)",
	  "  SetVar(name1=value1|name2=value2|..[|options])\n"
	  "SetVar has been deprecated in favor of Set.\n"
	},

	{ "ImportVar", pbx_builtin_importvar,
	"Import a variable from a channel into a new variable",
	"  ImportVar(newvar=channelname|variable): This application imports a\n"
	"variable from the specified channel (as opposed to the current one)\n"
	"and stores it as a variable in the current channel (the channel that\n"
	"is calling this application). If the new variable name is prefixed by\n"
	"a single underscore \"_\", then it will be inherited into any channels\n"
	"created from this one. If it is prefixed with two underscores,then\n"
	"the variable will have infinite inheritance, meaning that it will be\n"
	"present in any descendent channel of this one.\n"
	},

	{ "StripMSD", pbx_builtin_stripmsd,
	"Strip leading digits",
	"  StripMSD(count): Strips the leading 'count' digits from the channel's\n"
	"associated extension. For example, the number 5551212 when stripped with a\n"
	"count of 3 would be changed to 1212. This app always returns 0, and the PBX\n"
	"will continue processing at the next priority for the *new* extension.\n"
	"  So, for example, if priority 3 of 5551212 is StripMSD 3, the next step\n"
	"executed will be priority 4 of 1212. If you switch into an extension which\n"
	"has no first step, the PBX will treat it as though the user dialed an\n"
	"invalid extension.\n" 
	},

	{ "Suffix", pbx_builtin_suffix, 
	"Append trailing digits",
	"  Suffix(digits): Appends the digit string specified by digits to the\n"
	"channel's associated extension. For example, the number 555 when suffixed\n"
	"with '1212' will become 5551212. This app always returns 0, and the PBX will\n"
	"continue processing at the next priority for the *new* extension.\n"
	"  So, for example, if priority 3 of 555 is Suffix 1212, the next step\n"
	"executed will be priority 4 of 5551212. If you switch into an extension\n"
	"which has no first step, the PBX will treat it as though the user dialed an\n"
	"invalid extension.\n" 
	},

	{ "Wait", pbx_builtin_wait, 
	"Waits for some time", 
	"  Wait(seconds): Waits for a specified number of seconds, then returns 0.\n"
	"seconds can be passed with fractions of a second. (eg: 1.5 = 1.5 seconds)\n" 
	},

	{ "WaitExten", pbx_builtin_waitexten, 
	"Waits for an extension to be entered", 
	"  WaitExten([seconds][|options]): Waits for the user to enter a new extension for the \n"
	"specified number of seconds, then returns 0. Seconds can be passed with\n"
	"fractions of a seconds (eg: 1.5 = 1.5 seconds) or if unspecified the\n"
	"default extension timeout will be used.\n"
	"  Options:\n"
	"    'm[(x)]' - Provide music on hold to the caller while waiting for an extension.\n"
	"               Optionally, specify the class for music on hold within parenthesis.\n"
	},

};

static struct opbx_context *contexts = NULL;
OPBX_MUTEX_DEFINE_STATIC(conlock); 		/* Lock for the opbx_context list */
static struct opbx_app *apps = NULL;
OPBX_MUTEX_DEFINE_STATIC(applock); 		/* Lock for the application list */

struct opbx_switch *switches = NULL;
OPBX_MUTEX_DEFINE_STATIC(switchlock);		/* Lock for switches */

OPBX_MUTEX_DEFINE_STATIC(hintlock);		/* Lock for extension state notifys */
static int stateid = 1;
struct opbx_hint *hints = NULL;
struct opbx_state_cb *statecbs = NULL;

int pbx_exec(struct opbx_channel *c, 		/* Channel */
		struct opbx_app *app,		/* Application */
		void *data,			/* Data for execution */
		int newstack)			/* Force stack increment */
{
	/* This function is special. It saves the stack so that no matter
	   how many times it is called, it returns to the same place */
	int res;
	
	char *saved_c_appl;
	char *saved_c_data;
	
	int (*execute)(struct opbx_channel *chan, void *data) = app->execute; 

	if (newstack)
    {
		if (c->cdr)
			opbx_cdr_setapp(c->cdr, app->name, data);

		/* save channel values */
		saved_c_appl= c->appl;
		saved_c_data= c->data;

		c->appl = app->name;
		c->data = data;		
		res = execute(c, data);
		/* restore channel values */
		c->appl= saved_c_appl;
		c->data= saved_c_data;
		return res;
	}
    else
	{
    	opbx_log(LOG_WARNING, "You really didn't want to call this function with newstack set to 0\n");
	}
    return -1;
}


/* Go no deeper than this through includes (not counting loops) */
#define OPBX_PBX_MAX_STACK	128

#define HELPER_EXISTS 0
#define HELPER_SPAWN 1
#define HELPER_EXEC 2
#define HELPER_CANMATCH 3
#define HELPER_MATCHMORE 4
#define HELPER_FINDLABEL 5

struct opbx_app *pbx_findapp(const char *app) 
{
	struct opbx_app *tmp;
	unsigned int hash = opbx_hash_app_name(app);

	if (opbx_mutex_lock(&applock))
    {
		opbx_log(LOG_WARNING, "Unable to obtain application lock\n");
		return NULL;
	}
	tmp = apps;
	while (tmp)
    {
		if (hash == tmp->hash)
			break;
		tmp = tmp->next;
	}
	opbx_mutex_unlock(&applock);
	return tmp;
}

static struct opbx_switch *pbx_findswitch(const char *sw)
{
	struct opbx_switch *asw;

	if (opbx_mutex_lock(&switchlock))
    {
		opbx_log(LOG_WARNING, "Unable to obtain application lock\n");
		return NULL;
	}
	asw = switches;
	while (asw)
    {
		if (!strcasecmp(asw->name, sw))
			break;
		asw = asw->next;
	}
	opbx_mutex_unlock(&switchlock);
	return asw;
}

static inline int include_valid(struct opbx_include *i)
{
	if (!i->hastime)
		return 1;

	return opbx_check_timing(&(i->timing));
}

static void pbx_destroy(struct opbx_pbx *p)
{
	free(p);
}

const char *opbx_extension_match_to_str(int match)
{
    switch (match)
    {
    case EXTENSION_MATCH_FAILURE:
        return "Failure";
    case EXTENSION_MATCH_EXACT:
        return "Exact";
    case EXTENSION_MATCH_OVERLENGTH:
        return "Overlength";
    case EXTENSION_MATCH_INCOMPLETE:
        return "Incomplete";
    case EXTENSION_MATCH_STRETCHABLE:
        return "Stretchable";
    case EXTENSION_MATCH_POSSIBLE:
        return "Possible";
    }
    return "???";
}

int opbx_extension_pattern_match(const char *destination, const char *pattern)
{
    unsigned int pattern_len;
    unsigned int destination_len;
    int i;
    int limit;
    char *where;
    const char *d;
    const char *p;

    /* If there is nothing to match, we consider the match incomplete */
    if (destination[0] == '\0')
    {
        /* A blank pattern is an odd thing to have, but let's be comprehensive and
           allow for it. */
        if (pattern[0] == '\0')
            return EXTENSION_MATCH_EXACT;
        return EXTENSION_MATCH_INCOMPLETE;
    }
    /* All patterns begin with _ */
    if (pattern[0] != '_')
    {
        /* Its not really a pattern. We need a solid partial/full match. */
        pattern_len = strlen(pattern);
        destination_len = strlen(destination);
        if (pattern_len > destination_len)
        {
            if (memcmp(pattern, destination, destination_len))
                return EXTENSION_MATCH_FAILURE;
            return EXTENSION_MATCH_INCOMPLETE;
        }
        else
        {
            if (memcmp(pattern, destination, pattern_len))
                return EXTENSION_MATCH_FAILURE;
            if (pattern_len == destination_len)
                return EXTENSION_MATCH_EXACT;
            return EXTENSION_MATCH_OVERLENGTH;
        }
        return EXTENSION_MATCH_INCOMPLETE;
    }

    d = destination;
    p = pattern;
    /* Skip the initial '_' */
    p++;
    while (*d == '-')
        d++;
    if (*d == '\0')
        return EXTENSION_MATCH_INCOMPLETE;
    while (*d  &&  *p  &&  *p != '/')
    {
        while (*d == '-')
            d++;
        if (*d == '\0')
            break;
        switch (toupper(*p))
        {
        case '[':
            if ((where = strchr(++p, ']')) == NULL)
            {
                opbx_log(LOG_WARNING, "Bad usage of [] in extension pattern '%s'", pattern);
                return EXTENSION_MATCH_FAILURE;
            }
            limit = (int) (where - p);
            for (i = 0;  i < limit;  i++)
            {
                if (i < limit - 2)
                {
                    if (p[i + 1] == '-')
                    {
                        if (*d >= p[i]  &&  *d <= p[i + 2])
                            break;
                        i += 2;
                        continue;
                    }
                }
                if (*d == p[i])
                    break;
            }
            if (i >= limit)
                return EXTENSION_MATCH_FAILURE;
            p += limit;
            break;
        case 'X':
            if (*d < '0'  ||  *d > '9')
                return EXTENSION_MATCH_FAILURE;
            break;
        case 'Z':
            if (*d < '1'  ||  *d > '9')
                return EXTENSION_MATCH_FAILURE;
            break;
        case 'N':
            if (*d < '2'  ||  *d > '9')
                return EXTENSION_MATCH_FAILURE;
            break;
        case '.':
        case '~':
            /* A hard match - can be relied upon. */
            return EXTENSION_MATCH_STRETCHABLE;
        case '!':
            /* A soft match - acceptable, might there might be a better match. */
            return EXTENSION_MATCH_POSSIBLE;
        case ' ':
        case '-':
            /* Ignore these characters */
            d--;
            break;
        default:
            if (*d != *p)
                return EXTENSION_MATCH_FAILURE;
            break;
        }
        d++;
        p++;
    }
    /* If we ran off the end of the destination and the pattern ends in '!', match */
    if (*d == '\0')
    {
        if (*p == '!')
            return EXTENSION_MATCH_POSSIBLE;
        if (*p == '\0'  ||  *p == '/')
            return EXTENSION_MATCH_EXACT;
        return EXTENSION_MATCH_INCOMPLETE;
    }
    if (*p == '\0'  ||  *p == '/')
        return EXTENSION_MATCH_OVERLENGTH;
    return EXTENSION_MATCH_FAILURE;
}

static int opbx_extension_match(const char *pattern, const char *data)
{
	int match;

    match = opbx_extension_pattern_match(data, pattern);
    if (match == EXTENSION_MATCH_POSSIBLE)
        return 2;
    return (match == EXTENSION_MATCH_EXACT  ||  match == EXTENSION_MATCH_STRETCHABLE)  ?  1  :  0;
}

static int opbx_extension_close(const char *pattern, const char *data, int needmore)
{
	int match;

    match = opbx_extension_pattern_match(data, pattern);
    if (match == EXTENSION_MATCH_POSSIBLE)
        return 2;
    if (needmore)
        return (match == EXTENSION_MATCH_STRETCHABLE  ||  match == EXTENSION_MATCH_INCOMPLETE)  ?  1  :  0;
    return (match == EXTENSION_MATCH_EXACT  ||  match == EXTENSION_MATCH_STRETCHABLE  ||  match == EXTENSION_MATCH_INCOMPLETE)  ?  1  :  0;
}

struct opbx_context *opbx_context_find(const char *name)
{
	struct opbx_context *tmp;
	unsigned int hash = opbx_hash_string(name);
	
	opbx_mutex_lock(&conlock);
	if (name) {
		tmp = contexts;
		while(tmp) {
			if (hash == tmp->hash)
				break;
			tmp = tmp->next;
		}
	} else
		tmp = contexts;
	opbx_mutex_unlock(&conlock);
	return tmp;
}

#define STATUS_NO_CONTEXT	1
#define STATUS_NO_EXTENSION	2
#define STATUS_NO_PRIORITY	3
#define STATUS_NO_LABEL		4
#define STATUS_SUCCESS		5

static int matchcid(const char *cidpattern, const char *callerid)
{
	/* If the Caller*ID pattern is empty, then we're matching NO Caller*ID, so
	   failing to get a number should count as a match, otherwise not */

	if (callerid == NULL)
		return (cidpattern[0])  ?  0  :  1;

    switch (opbx_extension_pattern_match(callerid, cidpattern))
    {
    case EXTENSION_MATCH_EXACT:
    case EXTENSION_MATCH_STRETCHABLE:
    case EXTENSION_MATCH_POSSIBLE:
	    return 1;
    }
    return 0;
}

static struct opbx_exten *pbx_find_extension(struct opbx_channel *chan, struct opbx_context *bypass, const char *context, const char *exten, int priority, const char *label, const char *callerid, int action, char *incstack[], int *stacklen, int *status, struct opbx_switch **swo, char **data, const char **foundcontext)
{
	int x, res;
	struct opbx_context *tmp;
	struct opbx_exten *e, *eroot;
	struct opbx_include *i;
	struct opbx_sw *sw;
	struct opbx_switch *asw;
	unsigned int hash = opbx_hash_string(context);

	/* Initialize status if appropriate */
	if (!*stacklen)
    {
		*status = STATUS_NO_CONTEXT;
		*swo = NULL;
		*data = NULL;
	}
	/* Check for stack overflow */
	if (*stacklen >= OPBX_PBX_MAX_STACK)
    {
		opbx_log(LOG_WARNING, "Maximum PBX stack exceeded\n");
		return NULL;
	}
	/* Check first to see if we've already been checked */
	for (x = 0;  x < *stacklen;  x++)
    {
		if (!strcasecmp(incstack[x], context))
			return NULL;
	}
	if (bypass)
		tmp = bypass;
	else
		tmp = contexts;
	while (tmp)
    {
		/* Match context */
		if (bypass || (hash == tmp->hash))
        {
			struct opbx_exten *earlymatch = NULL;

			if (*status < STATUS_NO_EXTENSION)
				*status = STATUS_NO_EXTENSION;
			for (eroot = tmp->root;  eroot;  eroot = eroot->next)
            {
				int match = 0;
				/* Match extension */
				if ((((action != HELPER_MATCHMORE) && opbx_extension_match(eroot->exten, exten))
                    ||
				    ((action == HELPER_CANMATCH) && (opbx_extension_close(eroot->exten, exten, 0)))
                    ||
    				   ((action == HELPER_MATCHMORE) && (match = opbx_extension_close(eroot->exten, exten, 1))))
                        &&
				        (!eroot->matchcid  ||  matchcid(eroot->cidmatch, callerid)))
                {

					if (action == HELPER_MATCHMORE  &&  match == 2  &&  !earlymatch)
                    {
						/* It matched an extension ending in a '!' wildcard
						   So ignore it for now, unless there's a better match */
						earlymatch = eroot;
					}
                    else
                    {
						e = eroot;
						if (*status < STATUS_NO_PRIORITY)
							*status = STATUS_NO_PRIORITY;
						while (e)
                        {
							/* Match priority */
							if (action == HELPER_FINDLABEL)
                            {
								if (*status < STATUS_NO_LABEL)
									*status = STATUS_NO_LABEL;
							 	if (label && e->label && !strcmp(label, e->label))
                                {
									*status = STATUS_SUCCESS;
									*foundcontext = context;
									return e;
								}
							}
                            else if (e->priority == priority)
                            {
								*status = STATUS_SUCCESS;
								*foundcontext = context;
								return e;
							}
							e = e->peer;
						}
					}
				}
			}
			if (earlymatch)
            {
				/* Bizarre logic for HELPER_MATCHMORE. We return zero to break out 
				   of the loop waiting for more digits, and _then_ match (normally)
				   the extension we ended up with. We got an early-matching wildcard
				   pattern, so return NULL to break out of the loop. */
				return NULL;
			}
			/* Check alternative switches */
			sw = tmp->alts;
			while (sw)
            {
				if ((asw = pbx_findswitch(sw->name)))
                {
					/* Substitute variables now */
					if (sw->eval) 
						pbx_substitute_variables_helper(chan, sw->data, sw->tmpdata, SWITCH_DATA_LENGTH - 1);
					if (action == HELPER_CANMATCH)
						res = asw->canmatch ? asw->canmatch(chan, context, exten, priority, callerid, sw->eval ? sw->tmpdata : sw->data) : 0;
					else if (action == HELPER_MATCHMORE)
						res = asw->matchmore ? asw->matchmore(chan, context, exten, priority, callerid, sw->eval ? sw->tmpdata : sw->data) : 0;
					else
						res = asw->exists ? asw->exists(chan, context, exten, priority, callerid, sw->eval ? sw->tmpdata : sw->data) : 0;
					if (res)
                    {
						/* Got a match */
						*swo = asw;
						*data = sw->eval ? sw->tmpdata : sw->data;
						*foundcontext = context;
						return NULL;
					}
				}
                else
                {
					opbx_log(LOG_WARNING, "No such switch '%s'\n", sw->name);
				}
				sw = sw->next;
			}
			/* Setup the stack */
			incstack[*stacklen] = tmp->name;
			(*stacklen)++;
			/* Now try any includes we have in this context */
			i = tmp->includes;
			while (i)
            {
				if (include_valid(i))
                {
					if ((e = pbx_find_extension(chan, bypass, i->rname, exten, priority, label, callerid, action, incstack, stacklen, status, swo, data, foundcontext))) 
						return e;
					if (*swo) 
						return NULL;
				}
				i = i->next;
			}
			break;
		}
		tmp = tmp->next;
	}
	return NULL;
}

/*! \brief  pbx_retrieve_variable: Support for Asterisk built-in variables and
      functions in the dialplan
  ---*/

// There are 5 different scenarios to be covered:
// 
// 1) built-in variables living in channel c's variable list
// 2) user defined variables living in channel c's variable list
// 3) user defined variables not living in channel c's variable list
// 4) built-in global variables, that is globally visible, not bound to any channel
// 5) user defined variables living in the global dialplan variable list (&globals)
//
// This function is safeguarded against the following cases:
//
// 1) if channel c doesn't exist (is NULL), scenario #1 and #2 searches are skipped
// 2) if channel c's variable list doesn't exist (is NULL), scenario #2 search is skipped
// 3) if NULL is passed in for parameter headp, scenario #3 search is skipped
// 4) global dialplan variable list doesn't exist (&globals is NULL), scenario #5 search is skipped
//
// This function is known NOT to be safeguarded against the following cases:
//
// 1) ret is NULL
// 2) workspace is NULL
// 3) workspacelen is larger than the actual buffer size of workspace
// 4) workspacelen is larger than VAR_BUF_SIZE
//
// NOTE: There may be further unsafeguarded cases not yet documented here!

void pbx_retrieve_variable(struct opbx_channel *c, const char *var, char **ret, char *workspace, int workspacelen, struct varshead *headp)
{
	char *first, *second;
	char tmpvar[80];
	time_t thistime;
	struct tm brokentime;
	int offset, offset2;
	struct opbx_var_t *variables;
	int no_match_yet = 0; // start optimistic
	unsigned int hash = opbx_hash_var_name(var);

	// warnings for (potentially) unsafe pre-conditions
	// TODO: these cases really ought to be safeguarded against
		
	if (ret == NULL)
		opbx_log(LOG_WARNING, "NULL passed in parameter 'ret' in function 'pbx_retrieve_variable'\n");

	if (workspace == NULL)
		opbx_log(LOG_WARNING, "NULL passed in parameter 'workspace' in function 'pbx_retrieve_variable'\n");
	
	if (workspacelen == 0)
		opbx_log(LOG_WARNING, "Zero passed in parameter 'workspacelen' in function 'pbx_retrieve_variable'\n");

	if (workspacelen > VAR_BUF_SIZE)
		opbx_log(LOG_WARNING, "VAR_BUF_SIZE exceeded by parameter 'workspacelen' in function 'pbx_retrieve_variable'\n");

	// actual work starts here
	
	if /* channel exists */ (c) 
		headp = &c->varshead;
	
	*ret=NULL;
	
	// check for slicing modifier
	if /* sliced */ ((first=strchr(var,':'))) {
		
		// remove characters counting from end or start of string */
		opbx_copy_string(tmpvar, var, sizeof(tmpvar));
		first = strchr(tmpvar, ':');
		if (!first)
			first = tmpvar + strlen(tmpvar);
		*first='\0';
		pbx_retrieve_variable(c,tmpvar,ret,workspace,workspacelen - 1, headp);
		if (!(*ret)) 
			return;
		offset=atoi(first+1);	/* The number of characters, 
					   positive: remove # of chars from start
					   negative: keep # of chars from end */
						
	 	if ((second=strchr(first+1,':'))) {	
			*second='\0';
			offset2 = atoi(second+1);		/* Number of chars to copy */
		} else if (offset >= 0) {
			offset2 = strlen(*ret)-offset;	/* Rest of string */
		} else {
			offset2 = abs(offset);
		} // end if

		if (abs(offset) > strlen(*ret)) {	/* Offset beyond string */
			if (offset >= 0) 
				offset = strlen(*ret);
			else 
				offset =- strlen(*ret);
		} // end if

		if ((offset < 0 && offset2 > -offset) || (offset >= 0 && offset+offset2 > strlen(*ret))) {
			if (offset >= 0) 
				offset2 = strlen(*ret)-offset;
			else 
				offset2 = strlen(*ret)+offset;
		} // end if

		if (offset >= 0)
			*ret += offset;
		else
			*ret += strlen(*ret)+offset;
		(*ret)[offset2] = '\0';		/* Cut at offset2 position */
	}
	else /* not sliced */ {
		
		if /* channel exists */ (c) {
			
			// ----------------------------------------------
			// search builtin channel variables (scenario #1)
			// ----------------------------------------------
						
			if /* CALLERID */(hash == OPBX_KEYWORD_CALLERID) {
				if (c->cid.cid_num) {
					if (c->cid.cid_name) {
						snprintf(workspace, workspacelen, "\"%s\" <%s>", c->cid.cid_name, c->cid.cid_num);
					} else {
						opbx_copy_string(workspace, c->cid.cid_num, workspacelen);
					}
					*ret = workspace;
				} else if (c->cid.cid_name) {
					opbx_copy_string(workspace, c->cid.cid_name, workspacelen);
					*ret = workspace;
				} else
					*ret = NULL;
			} // end if CALLERID
			
			else if /* CALLERIDNUM */ (hash == OPBX_KEYWORD_CALLERIDNUM) {
				if (c->cid.cid_num) {
					opbx_copy_string(workspace, c->cid.cid_num, workspacelen);
					*ret = workspace;
				} else
					*ret = NULL;
			} // end if CALLERIDNUM
			
			else if /* CALLERIDNAME */ (hash == OPBX_KEYWORD_CALLERIDNAME) {
				if (c->cid.cid_name) {
					opbx_copy_string(workspace, c->cid.cid_name, workspacelen);
					*ret = workspace;
				} else
					*ret = NULL;
			} // end if CALLERIDNAME
			
			else if /* CALLERANI */ (hash == OPBX_KEYWORD_CALLERANI) {
				if (c->cid.cid_ani) {
					opbx_copy_string(workspace, c->cid.cid_ani, workspacelen);
					*ret = workspace;
				} else
					*ret = NULL;
			} // end if CALLERANI
			
			else if /* CALLINGPRES */ (hash == OPBX_KEYWORD_CALLINGPRES) {
				snprintf(workspace, workspacelen, "%d", c->cid.cid_pres);
				*ret = workspace;
			} // end if CALLINGPRES
			
			else if /* CALLINGANI2 */ (hash == OPBX_KEYWORD_CALLINGANI2) {
				snprintf(workspace, workspacelen, "%d", c->cid.cid_ani2);
				*ret = workspace;
			} // end if CALLINGANI2
			
			else if /* CALLINGTON */ (hash == OPBX_KEYWORD_CALLINGTON) {
				snprintf(workspace, workspacelen, "%d", c->cid.cid_ton);
				*ret = workspace;
			} // end if CALLINGTON
			
			else if /* CALLINGTNS */ (hash == OPBX_KEYWORD_CALLINGTNS) {
				snprintf(workspace, workspacelen, "%d", c->cid.cid_tns);
				*ret = workspace;
			} // end if CALLINGTNS
			
			else if /* DNID */ (hash == OPBX_KEYWORD_DNID) {
				if (c->cid.cid_dnid) {
					opbx_copy_string(workspace, c->cid.cid_dnid, workspacelen);
					*ret = workspace;
				} else
					*ret = NULL;
			} // end if DNID
			
			else if /* HINT */ (hash == OPBX_KEYWORD_HINT) {
				if (!opbx_get_hint(workspace, workspacelen, NULL, 0, c, c->context, c->exten))
					*ret = NULL;
				else
					*ret = workspace;
			} // end if HINT
			
			else if /* HINTNAME */ (hash == OPBX_KEYWORD_HINTNAME) {
				if (!opbx_get_hint(NULL, 0, workspace, workspacelen, c, c->context, c->exten))
					*ret = NULL;
				else
					*ret = workspace;
			} // end if HINTNAME
			
			else if /* EXTEN */ (hash == OPBX_KEYWORD_EXTEN) {
				opbx_copy_string(workspace, c->exten, workspacelen);
				*ret = workspace;
			} // end if EXTEN
			
			else if /* RDNIS */ (hash == OPBX_KEYWORD_RDNIS) {
				if (c->cid.cid_rdnis) {
					opbx_copy_string(workspace, c->cid.cid_rdnis, workspacelen);
					*ret = workspace;
				} else
					*ret = NULL;
			} // end if RDNIS
			
			else if /* CONTEXT */ (hash == OPBX_KEYWORD_CONTEXT) {
				opbx_copy_string(workspace, c->context, workspacelen);
				*ret = workspace;
			} // end if CONTEXT
			
			else if /* PRIORITY */ (hash == OPBX_KEYWORD_PRIORITY) {
				snprintf(workspace, workspacelen, "%d", c->priority);
				*ret = workspace;
			} // end if PRIORITY
			
			else if /* CHANNEL */ (hash == OPBX_KEYWORD_CHANNEL) {
				opbx_copy_string(workspace, c->name, workspacelen);
				*ret = workspace;
			} // end if CHANNEL
			
			else if /* UNIQUEID */ (hash == OPBX_KEYWORD_UNIQUEID) {
				snprintf(workspace, workspacelen, "%s", c->uniqueid);
				*ret = workspace;
			} // end if UNIQUEID
			
			else if /* HANGUPCAUSE */ (hash == OPBX_KEYWORD_HANGUPCAUSE) {
				snprintf(workspace, workspacelen, "%d", c->hangupcause);
				*ret = workspace;
			} // end if HANGUPCAUSE
			
			else if /* ACCOUNTCODE */ (hash == OPBX_KEYWORD_ACCOUNTCODE) {
				opbx_copy_string(workspace, c->accountcode, workspacelen);
				*ret = workspace;
			} // end if ACCOUNTCODE
			
			else if /* LANGUAGE */ (hash == OPBX_KEYWORD_LANGUAGE) {
				opbx_copy_string(workspace, c->language, workspacelen);
				*ret = workspace;
			} // end if LANGUAGE
			
			else if /* user defined channel variables exist */ (&c->varshead) {
				no_match_yet = 1;
				
				// ---------------------------------------------------
				// search user defined channel variables (scenario #2)
				// ---------------------------------------------------
				
				OPBX_LIST_TRAVERSE(&c->varshead, variables, entries) {
#if 0
					opbx_log(LOG_WARNING, "Comparing variable '%s' with '%s' in channel '%s'\n",
							 var, opbx_var_name(variables), c->name);
#endif
					if (strcasecmp(opbx_var_name(variables),var)==0) {
						*ret=opbx_var_value(variables);
						if (*ret) {
							opbx_copy_string(workspace, *ret, workspacelen);
							*ret = workspace;
						} // end if
						no_match_yet = 0; // remember that we found a match
						break;
					} // end if
				} // end OPBX_LIST_TRAVERSE
			} // end if user defined chanvars exist
			
			else /* not a channel variable, neither built-in nor user-defined */ {
				no_match_yet = 1;
			} // end search chanvars
		
		}
		else /* channel does not exist */ {
			no_match_yet = 1;
			
			// -------------------------------------------------------------------------
			// search for user defined variables not bound to this channel (scenario #3)
			// -------------------------------------------------------------------------
			
			if /* parameter headp points to an address other than NULL */ (headp) {
			
				OPBX_LIST_TRAVERSE(headp, variables, entries) {
#if 0
					opbx_log(LOG_WARNING,"Comparing variable '%s' with '%s'\n",var,opbx_var_name(variables));
#endif
					if (strcasecmp(opbx_var_name(variables),var)==0) {
						*ret=opbx_var_value(variables);
						if (*ret) {
							opbx_copy_string(workspace, *ret, workspacelen);
							*ret = workspace;
						} // end if
						no_match_yet = 0; // remember that we found a match
						break;
					} // end if
				} // end OPBX_LIST_TRAVERSE
			} // end if headp not NULL
			
		} // end if channel exists
		
		if /* no match yet */ (no_match_yet) {
			
			// ------------------------------------
			// search builtin globals (scenario #4)
			// ------------------------------------
		
			if /* EPOCH */ (hash == OPBX_KEYWORD_EPOCH) {
				snprintf(workspace, workspacelen, "%u",(int)time(NULL));
				*ret = workspace;
			} // end if EPOCH
			
			else if /* DATETIME */ (hash == OPBX_KEYWORD_DATETIME) {
				thistime=time(NULL);
				localtime_r(&thistime, &brokentime);
				snprintf(workspace, workspacelen, "%02d%02d%04d-%02d:%02d:%02d",
						 brokentime.tm_mday,
						 brokentime.tm_mon+1,
						 brokentime.tm_year+1900,
						 brokentime.tm_hour,
						 brokentime.tm_min,
						 brokentime.tm_sec
						 );
				*ret = workspace;
			} // end if DATETIME
			
			else if /* TIMESTAMP */ (hash == OPBX_KEYWORD_TIMESTAMP) {
				thistime=time(NULL);
				localtime_r(&thistime, &brokentime);
				/* 20031130-150612 */
				snprintf(workspace, workspacelen, "%04d%02d%02d-%02d%02d%02d",
						 brokentime.tm_year+1900,
						 brokentime.tm_mon+1,
						 brokentime.tm_mday,
						 brokentime.tm_hour,
						 brokentime.tm_min,
						 brokentime.tm_sec
						 );
				*ret = workspace;
			} // end if TIMESTAMP
			
			else if (!(*ret)) {
				
				// -----------------------------------------
				// search user defined globals (scenario #5)
				// -----------------------------------------
				
				if /* globals variable list exists, not NULL */ (&globals) {
					
					OPBX_LIST_TRAVERSE(&globals, variables, entries) {
#if 0
						opbx_log(LOG_WARNING,"Comparing variable '%s' with '%s' in globals\n",
								 var, opbx_var_name(variables));
#endif
						if (hash == opbx_var_hash(variables)) {
							*ret = opbx_var_value(variables);
							if (*ret) {
								opbx_copy_string(workspace, *ret, workspacelen);
								*ret = workspace;
							} // end if
						} // end if
					} // end OPBX_LIST_TRAVERSE
				} // end if globals exists
			} // end search builtin globals
		} // end if no match

	} // end if variable sliced

} // end pbx_retrieve_variable

static int handle_show_functions(int fd, int argc, char *argv[])
{
	struct opbx_custom_function *acf;
	int count_acf = 0;

	opbx_cli(fd, "Installed Custom Functions:\n--------------------------------------------------------------------------------\n");
	for (acf = acf_root ; acf; acf = acf->next) {
		opbx_cli(fd, "%-20.20s  %-35.35s  %s\n", acf->name, acf->syntax, acf->synopsis);
		count_acf++;
	}
	opbx_cli(fd, "%d custom functions installed.\n", count_acf);
	return 0;
}

static int handle_show_function(int fd, int argc, char *argv[])
{
	struct opbx_custom_function *acf;
	/* Maximum number of characters added by terminal coloring is 22 */
	char infotitle[64 + OPBX_MAX_APP + 22], syntitle[40], destitle[40];
	char info[64 + OPBX_MAX_APP], *synopsis = NULL, *description = NULL;
	char stxtitle[40], *syntax = NULL;
	int synopsis_size, description_size, syntax_size;

	if (argc < 3) return RESULT_SHOWUSAGE;

	if (!(acf = opbx_custom_function_find(argv[2]))) {
		opbx_cli(fd, "No function by that name registered.\n");
		return RESULT_FAILURE;

	}

	if (acf->synopsis)
		synopsis_size = strlen(acf->synopsis) + 23;
	else
		synopsis_size = strlen("Not available") + 23;
	synopsis = alloca(synopsis_size);
	
	if (acf->desc)
		description_size = strlen(acf->desc) + 23;
	else
		description_size = strlen("Not available") + 23;
	description = alloca(description_size);

	if (acf->syntax)
		syntax_size = strlen(acf->syntax) + 23;
	else
		syntax_size = strlen("Not available") + 23;
	syntax = alloca(syntax_size);

	snprintf(info, 64 + OPBX_MAX_APP, "\n  -= Info about function '%s' =- \n\n", acf->name);
	opbx_term_color(infotitle, info, COLOR_MAGENTA, 0, 64 + OPBX_MAX_APP + 22);
	opbx_term_color(stxtitle, "[Syntax]\n", COLOR_MAGENTA, 0, 40);
	opbx_term_color(syntitle, "[Synopsis]\n", COLOR_MAGENTA, 0, 40);
	opbx_term_color(destitle, "[Description]\n", COLOR_MAGENTA, 0, 40);
	opbx_term_color(syntax,
		   acf->syntax ? acf->syntax : "Not available",
		   COLOR_CYAN, 0, syntax_size);
	opbx_term_color(synopsis,
		   acf->synopsis ? acf->synopsis : "Not available",
		   COLOR_CYAN, 0, synopsis_size);
	opbx_term_color(description,
		   acf->desc ? acf->desc : "Not available",
		   COLOR_CYAN, 0, description_size);
	
	opbx_cli(fd,"%s%s%s\n\n%s%s\n\n%s%s\n", infotitle, stxtitle, syntax, syntitle, synopsis, destitle, description);

	return RESULT_SUCCESS;
}

static char *complete_show_function(char *line, char *word, int pos, int state)
{
	struct opbx_custom_function *acf;
	int which = 0;

	/* try to lock functions list ... */
	if (opbx_mutex_lock(&acflock)) {
		opbx_log(LOG_ERROR, "Unable to lock function list\n");
		return NULL;
	}

	acf = acf_root;
	while (acf) {
		if (!strncasecmp(word, acf->name, strlen(word))) {
			if (++which > state) {
				char *ret = strdup(acf->name);
				opbx_mutex_unlock(&acflock);
				return ret;
			}
		}
		acf = acf->next; 
	}

	opbx_mutex_unlock(&acflock);
	return NULL; 
}

struct opbx_custom_function* opbx_custom_function_find(char *name) 
{
	struct opbx_custom_function *acfptr;

	/* try to lock functions list ... */
	if (opbx_mutex_lock(&acflock)) {
		opbx_log(LOG_ERROR, "Unable to lock function list\n");
		return NULL;
	}

	for (acfptr = acf_root; acfptr; acfptr = acfptr->next) {
		if (!strcmp(name, acfptr->name)) {
			break;
		}
	}

	opbx_mutex_unlock(&acflock);
	
	return acfptr;
}

int opbx_custom_function_unregister(struct opbx_custom_function *acf) 
{
	struct opbx_custom_function *acfptr, *lastacf = NULL;
	int res = -1;

	if (!acf)
		return -1;

	/* try to lock functions list ... */
	if (opbx_mutex_lock(&acflock)) {
		opbx_log(LOG_ERROR, "Unable to lock function list\n");
		return -1;
	}

	for (acfptr = acf_root; acfptr; acfptr = acfptr->next) {
		if (acfptr == acf) {
			if (lastacf) {
				lastacf->next = acf->next;
			} else {
				acf_root = acf->next;
			}
			res = 0;
			break;
		}
		lastacf = acfptr;
	}

	opbx_mutex_unlock(&acflock);

	if (!res && (option_verbose > 1))
		opbx_verbose(VERBOSE_PREFIX_2 "Unregistered custom function %s\n", acf->name);

	return res;
}

int opbx_custom_function_register(struct opbx_custom_function *acf) 
{
	if (!acf)
		return -1;

	/* try to lock functions list ... */
	if (opbx_mutex_lock(&acflock)) {
		opbx_log(LOG_ERROR, "Unable to lock function list. Failed registering function %s\n", acf->name);
		return -1;
	}

	if (opbx_custom_function_find(acf->name)) {
		opbx_log(LOG_ERROR, "Function %s already registered.\n", acf->name);
		opbx_mutex_unlock(&acflock);
		return -1;
	}

	acf->next = acf_root;
	acf_root = acf;

	opbx_mutex_unlock(&acflock);

	if (option_verbose > 1)
		opbx_verbose(VERBOSE_PREFIX_2 "Registered custom function %s\n", acf->name);

	return 0;
}

char *opbx_func_read(struct opbx_channel *chan, const char *in, char *workspace, size_t len)
{
	char *args = NULL, *function, *p;
	char *ret = "0";
	struct opbx_custom_function *acfptr;

	function = opbx_strdupa(in);
	if (!function) {
		opbx_log(LOG_ERROR, "Out of memory\n");
		return ret;
	}
	if ((args = strchr(function, '('))) {
		*args = '\0';
		args++;
		if ((p = strrchr(args, ')'))) {
			*p = '\0';
		} else {
			opbx_log(LOG_WARNING, "Can't find trailing parenthesis?\n");
		}
	} else {
		opbx_log(LOG_WARNING, "Function doesn't contain parentheses.  Assuming null argument.\n");
	}

	if ((acfptr = opbx_custom_function_find(function))) {
		/* run the custom function */
		if (acfptr->read) {
			return acfptr->read(chan, function, args, workspace, len);
		} else {
			opbx_log(LOG_ERROR, "Function %s cannot be read\n", function);
		}
	} else {
		opbx_log(LOG_ERROR, "Function %s not registered\n", function);
	}
	return ret;
}

void opbx_func_write(struct opbx_channel *chan, const char *in, const char *value)
{
	char *args = NULL, *function, *p;
	struct opbx_custom_function *acfptr;

	function = opbx_strdupa(in);
	if (!function) {
		opbx_log(LOG_ERROR, "Out of memory\n");
		return;
	}
	if ((args = strchr(function, '('))) {
		*args = '\0';
		args++;
		if ((p = strrchr(args, ')'))) {
			*p = '\0';
		} else {
			opbx_log(LOG_WARNING, "Can't find trailing parenthesis?\n");
		}
	} else {
		opbx_log(LOG_WARNING, "Function doesn't contain parentheses.  Assuming null argument.\n");
	}

	if ((acfptr = opbx_custom_function_find(function))) {
		/* run the custom function */
		if (acfptr->write) {
			acfptr->write(chan, function, args, value);
		} else {
			opbx_log(LOG_ERROR, "Function %s is read-only, it cannot be written to\n", function);
		}
	} else {
		opbx_log(LOG_ERROR, "Function %s not registered\n", function);
	}
}

static void pbx_substitute_variables_helper_full(struct opbx_channel *c, struct varshead *headp, const char *cp1, char *cp2, int count)
{
	char *cp4 = 0;
	const char *tmp, *whereweare;
	int length;
	char *workspace = NULL;
	char *ltmp = NULL, *var = NULL;
	char *nextvar, *nextexp, *nextthing;
	char *vars, *vare;
	int pos, brackets, needsub, len;
	
	/* Substitutes variables into cp2, based on string cp1, and assuming cp2 to be
	   zero-filled */
	whereweare=tmp=cp1;
	while(!opbx_strlen_zero(whereweare) && count) {
		/* Assume we're copying the whole remaining string */
		pos = strlen(whereweare);
		nextvar = NULL;
		nextexp = NULL;
		nextthing = strchr(whereweare, '$');
		if (nextthing) {
			switch(nextthing[1]) {
			case '{':
				nextvar = nextthing;
				pos = nextvar - whereweare;
				break;
			case '[':
				nextexp = nextthing;
				pos = nextexp - whereweare;
				break;
			}
		}

		if (pos) {
			/* Can't copy more than 'count' bytes */
			if (pos > count)
				pos = count;
			
			/* Copy that many bytes */
			memcpy(cp2, whereweare, pos);
			
			count -= pos;
			cp2 += pos;
			whereweare += pos;
		}
		
		if (nextvar) {
			/* We have a variable.  Find the start and end, and determine
			   if we are going to have to recursively call ourselves on the
			   contents */
			vars = vare = nextvar + 2;
			brackets = 1;
			needsub = 0;

			/* Find the end of it */
			while(brackets && *vare) {
				if ((vare[0] == '$') && (vare[1] == '{')) {
					needsub++;
				} else if (vare[0] == '{') {
					brackets++;
				} else if (vare[0] == '}') {
					brackets--;
				} else if ((vare[0] == '$') && (vare[1] == '['))
					needsub++;
				vare++;
			}
			if (brackets)
				opbx_log(LOG_NOTICE, "Error in extension logic (missing '}')\n");
			len = vare - vars - 1;

			/* Skip totally over variable string */
			whereweare += (len + 3);

			if (!var)
				var = alloca(VAR_BUF_SIZE);

			/* Store variable name (and truncate) */
			opbx_copy_string(var, vars, len + 1);

			/* Substitute if necessary */
			if (needsub) {
				if (!ltmp)
					ltmp = alloca(VAR_BUF_SIZE);

				memset(ltmp, 0, VAR_BUF_SIZE);
				pbx_substitute_variables_helper_full(c, headp, var, ltmp, VAR_BUF_SIZE - 1);
				vars = ltmp;
			} else {
				vars = var;
			}

			if (!workspace)
				workspace = alloca(VAR_BUF_SIZE);

			workspace[0] = '\0';

			if (var[len - 1] == ')') {
				/* Evaluate function */
				cp4 = opbx_func_read(c, vars, workspace, VAR_BUF_SIZE);

				opbx_log(LOG_DEBUG, "Function result is '%s'\n", cp4 ? cp4 : "(null)");
			} else {
				/* Retrieve variable value */
				pbx_retrieve_variable(c, vars, &cp4, workspace, VAR_BUF_SIZE, headp);
			}
			if (cp4) {
				length = strlen(cp4);
				if (length > count)
					length = count;
				memcpy(cp2, cp4, length);
				count -= length;
				cp2 += length;
			}
		} else if (nextexp) {
			/* We have an expression.  Find the start and end, and determine
			   if we are going to have to recursively call ourselves on the
			   contents */
			vars = vare = nextexp + 2;
			brackets = 1;
			needsub = 0;

			/* Find the end of it */
			while(brackets && *vare) {
				if ((vare[0] == '$') && (vare[1] == '[')) {
					needsub++;
					brackets++;
					vare++;
				} else if (vare[0] == '[') {
					brackets++;
				} else if (vare[0] == ']') {
					brackets--;
				} else if ((vare[0] == '$') && (vare[1] == '{')) {
					needsub++;
					vare++;
				}
				vare++;
			}
			if (brackets)
				opbx_log(LOG_NOTICE, "Error in extension logic (missing ']')\n");
			len = vare - vars - 1;
			
			/* Skip totally over expression */
			whereweare += (len + 3);
			
			if (!var)
				var = alloca(VAR_BUF_SIZE);

			/* Store variable name (and truncate) */
			opbx_copy_string(var, vars, len + 1);
			
			/* Substitute if necessary */
			if (needsub) {
				if (!ltmp)
					ltmp = alloca(VAR_BUF_SIZE);

				memset(ltmp, 0, VAR_BUF_SIZE);
				pbx_substitute_variables_helper_full(c, headp, var, ltmp, VAR_BUF_SIZE - 1);
				vars = ltmp;
			} else {
				vars = var;
			}

			length = opbx_expr(vars, cp2, count);

			if (length) {
				opbx_log(LOG_DEBUG, "Expression result is '%s'\n", cp2);
				count -= length;
				cp2 += length;
			}
		} else
			break;
	}
}

void pbx_substitute_variables_helper(struct opbx_channel *c, const char *cp1, char *cp2, int count)
{
	pbx_substitute_variables_helper_full(c, (c) ? &c->varshead : NULL, cp1, cp2, count);
}

void pbx_substitute_variables_varshead(struct varshead *headp, const char *cp1, char *cp2, int count)
{
	pbx_substitute_variables_helper_full(NULL, headp, cp1, cp2, count);
}

static void pbx_substitute_variables(char *passdata, int datalen, struct opbx_channel *c, struct opbx_exten *e)
{
	memset(passdata, 0, datalen);
		
	/* No variables or expressions in e->data, so why scan it? */
	if (!strchr(e->data, '$') && !strstr(e->data,"${") && !strstr(e->data,"$[") && !strstr(e->data,"$(")) {
		opbx_copy_string(passdata, e->data, datalen);
		return;
	}
	
	pbx_substitute_variables_helper(c, e->data, passdata, datalen - 1);
}		                                                

static int pbx_extension_helper(struct opbx_channel *c, struct opbx_context *con, const char *context, const char *exten, int priority, const char *label, const char *callerid, int action) 
{
	struct opbx_exten *e;
	struct opbx_app *app;
	struct opbx_switch *sw;
	char *data;
	const char *foundcontext=NULL;
	int newstack = 0;
	int res;
	int status = 0;
	char *incstack[OPBX_PBX_MAX_STACK];
	char passdata[EXT_DATA_SIZE];
	int stacklen = 0;
	char tmp[80];
	char tmp2[80];
	char tmp3[EXT_DATA_SIZE];
	char atmp[80];
	char atmp2[EXT_DATA_SIZE+100];

	if (opbx_mutex_lock(&conlock)) {
		opbx_log(LOG_WARNING, "Unable to obtain lock\n");
		if ((action == HELPER_EXISTS) || (action == HELPER_CANMATCH) || (action == HELPER_MATCHMORE))
			return 0;
		else
			return -1;
	}
	e = pbx_find_extension(c, con, context, exten, priority, label, callerid, action, incstack, &stacklen, &status, &sw, &data, &foundcontext);
	if (e) {
		switch(action) {
		case HELPER_CANMATCH:
			opbx_mutex_unlock(&conlock);
			return -1;
		case HELPER_EXISTS:
			opbx_mutex_unlock(&conlock);
			return -1;
		case HELPER_FINDLABEL:
			res = e->priority;
			opbx_mutex_unlock(&conlock);
			return res;
		case HELPER_MATCHMORE:
			opbx_mutex_unlock(&conlock);
			return -1;
		case HELPER_SPAWN:
			newstack++;
			/* Fall through */
		case HELPER_EXEC:
			app = pbx_findapp(e->app);
			opbx_mutex_unlock(&conlock);
			if (app) {
				if (c->context != context)
					opbx_copy_string(c->context, context, sizeof(c->context));
				if (c->exten != exten)
					opbx_copy_string(c->exten, exten, sizeof(c->exten));
				c->priority = priority;
				pbx_substitute_variables(passdata, sizeof(passdata), c, e);
				if (option_debug) {
						opbx_log(LOG_DEBUG, "Launching '%s'\n", app->name);
						snprintf(atmp, 80, "STACK-%s-%s-%d", context, exten, priority);
						snprintf(atmp2, EXT_DATA_SIZE+100, "%s(\"%s\", \"%s\") %s", app->name, c->name, (!opbx_strlen_zero(passdata) ? (char *)passdata : ""), (newstack ? "in new stack" : "in same stack"));
						pbx_builtin_setvar_helper(c, atmp, atmp2);
				}
				if (option_verbose > 2)
						opbx_verbose( VERBOSE_PREFIX_3 "Executing %s(\"%s\", \"%s\") %s\n", 
								opbx_term_color(tmp, app->name, COLOR_BRCYAN, 0, sizeof(tmp)),
								opbx_term_color(tmp2, c->name, COLOR_BRMAGENTA, 0, sizeof(tmp2)),
								opbx_term_color(tmp3, (!opbx_strlen_zero(passdata) ? (char *)passdata : ""), COLOR_BRMAGENTA, 0, sizeof(tmp3)),
								(newstack ? "in new stack" : "in same stack"));
				manager_event(EVENT_FLAG_CALL, "Newexten", 
					"Channel: %s\r\n"
					"Context: %s\r\n"
					"Extension: %s\r\n"
					"Priority: %d\r\n"
					"Application: %s\r\n"
					"AppData: %s\r\n"
					"Uniqueid: %s\r\n",
					c->name, c->context, c->exten, c->priority, app->name, passdata ? passdata : "(NULL)", c->uniqueid);
				res = pbx_exec(c, app, passdata, newstack);
				return res;
			} else {
				opbx_log(LOG_WARNING, "No application '%s' for extension (%s, %s, %d)\n", e->app, context, exten, priority);
				return -1;
			}
		default:
			opbx_log(LOG_WARNING, "Huh (%d)?\n", action);			return -1;
		}
	} else if (sw) {
		switch(action) {
		case HELPER_CANMATCH:
			opbx_mutex_unlock(&conlock);
			return -1;
		case HELPER_EXISTS:
			opbx_mutex_unlock(&conlock);
			return -1;
		case HELPER_MATCHMORE:
			opbx_mutex_unlock(&conlock);
			return -1;
		case HELPER_FINDLABEL:
			opbx_mutex_unlock(&conlock);
			return -1;
		case HELPER_SPAWN:
			newstack++;
			/* Fall through */
		case HELPER_EXEC:
			opbx_mutex_unlock(&conlock);
			if (sw->exec)
				res = sw->exec(c, foundcontext ? foundcontext : context, exten, priority, callerid, newstack, data);
			else {
				opbx_log(LOG_WARNING, "No execution engine for switch %s\n", sw->name);
				res = -1;
			}
			return res;
		default:
			opbx_log(LOG_WARNING, "Huh (%d)?\n", action);
			return -1;
		}
	} else {
		opbx_mutex_unlock(&conlock);
		switch(status) {
		case STATUS_NO_CONTEXT:
			if ((action != HELPER_EXISTS) && (action != HELPER_MATCHMORE))
				opbx_log(LOG_NOTICE, "Cannot find extension context '%s'\n", context);
			break;
		case STATUS_NO_EXTENSION:
			if ((action != HELPER_EXISTS) && (action !=  HELPER_CANMATCH) && (action != HELPER_MATCHMORE))
				opbx_log(LOG_NOTICE, "Cannot find extension '%s' in context '%s'\n", exten, context);
			break;
		case STATUS_NO_PRIORITY:
			if ((action != HELPER_EXISTS) && (action !=  HELPER_CANMATCH) && (action != HELPER_MATCHMORE))
				opbx_log(LOG_NOTICE, "No such priority %d in extension '%s' in context '%s'\n", priority, exten, context);
			break;
		case STATUS_NO_LABEL:
			if (context)
				opbx_log(LOG_NOTICE, "No such label '%s' in extension '%s' in context '%s'\n", label, exten, context);
			break;
		default:
			opbx_log(LOG_DEBUG, "Shouldn't happen!\n");
		}
		
		if ((action != HELPER_EXISTS) && (action != HELPER_CANMATCH) && (action != HELPER_MATCHMORE))
			return -1;
		else
			return 0;
	}

}

/*! \brief  opbx_hint_extension: Find hint for given extension in context */
static struct opbx_exten *opbx_hint_extension(struct opbx_channel *c, const char *context, const char *exten)
{
	struct opbx_exten *e;
	struct opbx_switch *sw;
	char *data;
	const char *foundcontext = NULL;
	int status = 0;
	char *incstack[OPBX_PBX_MAX_STACK];
	int stacklen = 0;

	if (opbx_mutex_lock(&conlock)) {
		opbx_log(LOG_WARNING, "Unable to obtain lock\n");
		return NULL;
	}
	e = pbx_find_extension(c, NULL, context, exten, PRIORITY_HINT, NULL, "", HELPER_EXISTS, incstack, &stacklen, &status, &sw, &data, &foundcontext);
	opbx_mutex_unlock(&conlock);	
	return e;
}

/*! \brief  opbx_extensions_state2: Check state of extension by using hints */
static int opbx_extension_state2(struct opbx_exten *e)
{
	char hint[OPBX_MAX_EXTENSION] = "";    
	char *cur, *rest;
	int res = -1;
	int allunavailable = 1, allbusy = 1, allfree = 1;
	int busy = 0, inuse = 0, ring = 0;

	if (!e)
		return -1;

	opbx_copy_string(hint, opbx_get_extension_app(e), sizeof(hint));

	cur = hint;    	/* On or more devices separated with a & character */
	do {
		rest = strchr(cur, '&');
		if (rest) {
			*rest = 0;
			rest++;
		}
	
		res = opbx_device_state(cur);
		switch (res) {
		case OPBX_DEVICE_NOT_INUSE:
			allunavailable = 0;
			allbusy = 0;
			break;
		case OPBX_DEVICE_INUSE:
			inuse = 1;
			allunavailable = 0;
			allfree = 0;
			break;
		case OPBX_DEVICE_RINGING:
			ring = 1;
			allunavailable = 0;
			allfree = 0;
			break;
		case OPBX_DEVICE_BUSY:
			allunavailable = 0;
			allfree = 0;
			busy = 1;
			break;
		case OPBX_DEVICE_UNAVAILABLE:
		case OPBX_DEVICE_INVALID:
			allbusy = 0;
			allfree = 0;
			break;
		default:
			allunavailable = 0;
			allbusy = 0;
			allfree = 0;
		}
		cur = rest;
	} while (cur);

	if (!inuse && ring)
		return OPBX_EXTENSION_RINGING;
	if (inuse && ring)
		return (OPBX_EXTENSION_INUSE | OPBX_EXTENSION_RINGING);
	if (inuse)
		return OPBX_EXTENSION_INUSE;
	if (allfree)
		return OPBX_EXTENSION_NOT_INUSE;
	if (allbusy)		
		return OPBX_EXTENSION_BUSY;
	if (allunavailable)
		return OPBX_EXTENSION_UNAVAILABLE;
	if (busy) 
		return OPBX_EXTENSION_INUSE;
	
	return OPBX_EXTENSION_NOT_INUSE;
}

/*! \brief  opbx_extension_state2str: Return extension_state as string */
const char *opbx_extension_state2str(int extension_state)
{
	int i;

	for (i = 0; (i < (sizeof(extension_states) / sizeof(extension_states[0]))); i++) {
		if (extension_states[i].extension_state == extension_state) {
			return extension_states[i].text;
		}
	}
	return "Unknown";	
}

/*! \brief  opbx_extension_state: Check extension state for an extension by using hint */
int opbx_extension_state(struct opbx_channel *c, char *context, char *exten)
{
	struct opbx_exten *e;

	e = opbx_hint_extension(c, context, exten);	/* Do we have a hint for this extension ? */ 
	if (!e) 
		return -1;				/* No hint, return -1 */

	return opbx_extension_state2(e);    		/* Check all devices in the hint */
}

void opbx_hint_state_changed(const char *device)
{
	struct opbx_hint *hint;
	struct opbx_state_cb *cblist;
	char buf[OPBX_MAX_EXTENSION];
	char *parse;
	char *cur;
	int state;

	opbx_mutex_lock(&hintlock);

	for (hint = hints; hint; hint = hint->next) {
		opbx_copy_string(buf, opbx_get_extension_app(hint->exten), sizeof(buf));
		parse = buf;
		for (cur = strsep(&parse, "&"); cur; cur = strsep(&parse, "&")) {
			if (strcmp(cur, device))
				continue;

			/* Get device state for this hint */
			state = opbx_extension_state2(hint->exten);
			
			if ((state == -1) || (state == hint->laststate))
				continue;

			/* Device state changed since last check - notify the watchers */
			
			/* For general callbacks */
			for (cblist = statecbs; cblist; cblist = cblist->next)
				cblist->callback(hint->exten->parent->name, hint->exten->exten, state, cblist->data);
			
			/* For extension callbacks */
			for (cblist = hint->callbacks; cblist; cblist = cblist->next)
				cblist->callback(hint->exten->parent->name, hint->exten->exten, state, cblist->data);
			
			hint->laststate = state;
			break;
		}
	}

	opbx_mutex_unlock(&hintlock);
}
			
/*! \brief  opbx_extension_state_add: Add watcher for extension states */
int opbx_extension_state_add(const char *context, const char *exten, 
			    opbx_state_cb_type callback, void *data)
{
	struct opbx_hint *list;
	struct opbx_state_cb *cblist;
	struct opbx_exten *e;

	/* If there's no context and extension:  add callback to statecbs list */
	if (!context && !exten) {
		opbx_mutex_lock(&hintlock);

		cblist = statecbs;
		while (cblist) {
			if (cblist->callback == callback) {
				cblist->data = data;
				opbx_mutex_unlock(&hintlock);
				return 0;
			}
			cblist = cblist->next;
		}
	
		/* Now insert the callback */
		cblist = malloc(sizeof(struct opbx_state_cb));
		if (!cblist) {
			opbx_mutex_unlock(&hintlock);
			return -1;
		}
		memset(cblist, 0, sizeof(struct opbx_state_cb));
		cblist->id = 0;
		cblist->callback = callback;
		cblist->data = data;
	
		cblist->next = statecbs;
		statecbs = cblist;

		opbx_mutex_unlock(&hintlock);
		return 0;
	}

	if (!context || !exten)
		return -1;

	/* This callback type is for only one hint, so get the hint */
	e = opbx_hint_extension(NULL, context, exten);    
	if (!e) {
		return -1;
	}

	/* Find the hint in the list of hints */
	opbx_mutex_lock(&hintlock);
	list = hints;        

	while (list) {
		if (list->exten == e)
			break;	    
		list = list->next;    
	}

	if (!list) {
		/* We have no hint, sorry */
		opbx_mutex_unlock(&hintlock);
		return -1;
	}

	/* Now insert the callback in the callback list  */
	cblist = malloc(sizeof(struct opbx_state_cb));
	if (!cblist) {
		opbx_mutex_unlock(&hintlock);
		return -1;
	}
	memset(cblist, 0, sizeof(struct opbx_state_cb));
	cblist->id = stateid++;		/* Unique ID for this callback */
	cblist->callback = callback;	/* Pointer to callback routine */
	cblist->data = data;		/* Data for the callback */

	cblist->next = list->callbacks;
	list->callbacks = cblist;

	opbx_mutex_unlock(&hintlock);
	return cblist->id;
}

/*! \brief  opbx_extension_state_del: Remove a watcher from the callback list */
int opbx_extension_state_del(int id, opbx_state_cb_type callback)
{
	struct opbx_hint *list;
	struct opbx_state_cb *cblist, *cbprev;

	if (!id && !callback)
		return -1;

	opbx_mutex_lock(&hintlock);

	/* id is zero is a callback without extension */
	if (!id) {
		cbprev = NULL;
		cblist = statecbs;
		while (cblist) {
	 		if (cblist->callback == callback) {
				if (!cbprev)
		    			statecbs = cblist->next;
				else
		    			cbprev->next = cblist->next;

				free(cblist);

	        		opbx_mutex_unlock(&hintlock);
				return 0;
	    		}
	    		cbprev = cblist;
	    		cblist = cblist->next;
		}

		opbx_mutex_lock(&hintlock);
		return -1;
	}

	/* id greater than zero is a callback with extension */
	/* Find the callback based on ID */
	list = hints;
	while (list) {
		cblist = list->callbacks;
		cbprev = NULL;
		while (cblist) {
	    		if (cblist->id==id) {
				if (!cbprev)
		    			list->callbacks = cblist->next;		
				else
		    			cbprev->next = cblist->next;
		
				free(cblist);
		
				opbx_mutex_unlock(&hintlock);
				return 0;		
	    		}		
	    		cbprev = cblist;				
	    		cblist = cblist->next;
		}
		list = list->next;
	}

	opbx_mutex_unlock(&hintlock);
	return -1;
}

/*! \brief  opbx_add_hint: Add hint to hint list, check initial extension state */
static int opbx_add_hint(struct opbx_exten *e)
{
	struct opbx_hint *list;

	if (!e) 
		return -1;

	opbx_mutex_lock(&hintlock);
	list = hints;        

	/* Search if hint exists, do nothing */
	while (list) {
		if (list->exten == e) {
			opbx_mutex_unlock(&hintlock);
			if (option_debug > 1)
				opbx_log(LOG_DEBUG, "HINTS: Not re-adding existing hint %s: %s\n", opbx_get_extension_name(e), opbx_get_extension_app(e));
			return -1;
		}
		list = list->next;    
	}

	if (option_debug > 1)
		opbx_log(LOG_DEBUG, "HINTS: Adding hint %s: %s\n", opbx_get_extension_name(e), opbx_get_extension_app(e));

	list = malloc(sizeof(struct opbx_hint));
	if (!list) {
		opbx_mutex_unlock(&hintlock);
		if (option_debug > 1)
			opbx_log(LOG_DEBUG, "HINTS: Out of memory...\n");
		return -1;
	}
	/* Initialize and insert new item at the top */
	memset(list, 0, sizeof(struct opbx_hint));
	list->exten = e;
	list->laststate = opbx_extension_state2(e);
	list->next = hints;
	hints = list;

	opbx_mutex_unlock(&hintlock);
	return 0;
}

/*! \brief  opbx_change_hint: Change hint for an extension */
static int opbx_change_hint(struct opbx_exten *oe, struct opbx_exten *ne)
{ 
	struct opbx_hint *list;

	opbx_mutex_lock(&hintlock);
	list = hints;

	while(list) {
		if (list->exten == oe) {
	    		list->exten = ne;
			opbx_mutex_unlock(&hintlock);	
			return 0;
		}
		list = list->next;
	}
	opbx_mutex_unlock(&hintlock);

	return -1;
}

/*! \brief  opbx_remove_hint: Remove hint from extension */
static int opbx_remove_hint(struct opbx_exten *e)
{
	/* Cleanup the Notifys if hint is removed */
	struct opbx_hint *list, *prev = NULL;
	struct opbx_state_cb *cblist, *cbprev;

	if (!e) 
		return -1;

	opbx_mutex_lock(&hintlock);

	list = hints;    
	while(list) {
		if (list->exten==e) {
			cbprev = NULL;
			cblist = list->callbacks;
			while (cblist) {
				/* Notify with -1 and remove all callbacks */
				cbprev = cblist;	    
				cblist = cblist->next;
				cbprev->callback(list->exten->parent->name, list->exten->exten, OPBX_EXTENSION_DEACTIVATED, cbprev->data);
				free(cbprev);
	    		}
	    		list->callbacks = NULL;

	    		if (!prev)
				hints = list->next;
	    		else
				prev->next = list->next;
	    		free(list);
	    
			opbx_mutex_unlock(&hintlock);
			return 0;
		} else {
			prev = list;
			list = list->next;    
		}
	}

	opbx_mutex_unlock(&hintlock);
	return -1;
}


/*! \brief  opbx_get_hint: Get hint for channel */
int opbx_get_hint(char *hint, int hintsize, char *name, int namesize, struct opbx_channel *c, const char *context, const char *exten)
{
	struct opbx_exten *e;
	void *tmp;

	e = opbx_hint_extension(c, context, exten);
	if (e) {
		if (hint) 
		    opbx_copy_string(hint, opbx_get_extension_app(e), hintsize);
		if (name) {
			tmp = opbx_get_extension_app_data(e);
			if (tmp)
				opbx_copy_string(name, (char *) tmp, namesize);
		}
	    return -1;
	}
	return 0;	
}

int opbx_exists_extension(struct opbx_channel *c, const char *context, const char *exten, int priority, const char *callerid) 
{
	return pbx_extension_helper(c, NULL, context, exten, priority, NULL, callerid, HELPER_EXISTS);
}

int opbx_findlabel_extension(struct opbx_channel *c, const char *context, const char *exten, const char *label, const char *callerid) 
{
	return pbx_extension_helper(c, NULL, context, exten, 0, label, callerid, HELPER_FINDLABEL);
}

int opbx_findlabel_extension2(struct opbx_channel *c, struct opbx_context *con, const char *exten, const char *label, const char *callerid) 
{
	return pbx_extension_helper(c, con, NULL, exten, 0, label, callerid, HELPER_FINDLABEL);
}

int opbx_canmatch_extension(struct opbx_channel *c, const char *context, const char *exten, int priority, const char *callerid)
{
	return pbx_extension_helper(c, NULL, context, exten, priority, NULL, callerid, HELPER_CANMATCH);
}

int opbx_matchmore_extension(struct opbx_channel *c, const char *context, const char *exten, int priority, const char *callerid)
{
	return pbx_extension_helper(c, NULL, context, exten, priority, NULL, callerid, HELPER_MATCHMORE);
}

int opbx_spawn_extension(struct opbx_channel *c, const char *context, const char *exten, int priority, const char *callerid) 
{
	return pbx_extension_helper(c, NULL, context, exten, priority, NULL, callerid, HELPER_SPAWN);
}

int opbx_exec_extension(struct opbx_channel *c, const char *context, const char *exten, int priority, const char *callerid) 
{
	return pbx_extension_helper(c, NULL, context, exten, priority, NULL, callerid, HELPER_EXEC);
}

static int __opbx_pbx_run(struct opbx_channel *c)
{
	int firstpass = 1;
	int digit;
	char exten[256];
	int pos;
	int waittime;
	int res=0;
	int autoloopflag;
	unsigned int hash;

	/* A little initial setup here */
	if (c->pbx)
		opbx_log(LOG_WARNING, "%s already has PBX structure??\n", c->name);
	c->pbx = malloc(sizeof(struct opbx_pbx));
	if (!c->pbx) {
		opbx_log(LOG_ERROR, "Out of memory\n");
		return -1;
	}
	if (c->amaflags) {
		if (!c->cdr) {
			c->cdr = opbx_cdr_alloc();
			if (!c->cdr) {
				opbx_log(LOG_WARNING, "Unable to create Call Detail Record\n");
				free(c->pbx);
				return -1;
			}
			opbx_cdr_init(c->cdr, c);
		}
	}
	memset(c->pbx, 0, sizeof(struct opbx_pbx));
	/* Set reasonable defaults */
	c->pbx->rtimeout = 10;
	c->pbx->dtimeout = 5;

	autoloopflag = opbx_test_flag(c, OPBX_FLAG_IN_AUTOLOOP);
	opbx_set_flag(c, OPBX_FLAG_IN_AUTOLOOP);

	/* Start by trying whatever the channel is set to */
	if (!opbx_exists_extension(c, c->context, c->exten, c->priority, c->cid.cid_num)) {
		/* If not successful fall back to 's' */
		if (option_verbose > 1)
			opbx_verbose( VERBOSE_PREFIX_2 "Starting %s at %s,%s,%d failed so falling back to exten 's'\n", c->name, c->context, c->exten, c->priority);
		opbx_copy_string(c->exten, "s", sizeof(c->exten));
		if (!opbx_exists_extension(c, c->context, c->exten, c->priority, c->cid.cid_num)) {
			/* JK02: And finally back to default if everything else failed */
			if (option_verbose > 1)
				opbx_verbose( VERBOSE_PREFIX_2 "Starting %s at %s,%s,%d still failed so falling back to context 'default'\n", c->name, c->context, c->exten, c->priority);
			opbx_copy_string(c->context, "default", sizeof(c->context));
		}
		c->priority = 1;
	}
	if (c->cdr && !c->cdr->start.tv_sec && !c->cdr->start.tv_usec)
		opbx_cdr_start(c->cdr);
	for(;;) {
		pos = 0;
		digit = 0;
		while(opbx_exists_extension(c, c->context, c->exten, c->priority, c->cid.cid_num)) {
			memset(exten, 0, sizeof(exten));
			if ((res = opbx_spawn_extension(c, c->context, c->exten, c->priority, c->cid.cid_num))) {
				/* Something bad happened, or a hangup has been requested. */
				if (((res >= '0') && (res <= '9')) || ((res >= 'A') && (res <= 'F')) ||
					(res == '*') || (res == '#')) {
					opbx_log(LOG_DEBUG, "Oooh, got something to jump out with ('%c')!\n", res);
					memset(exten, 0, sizeof(exten));
					pos = 0;
					exten[pos++] = digit = res;
					break;
				}
				switch(res) {
				case OPBX_PBX_KEEPALIVE:
					if (option_debug)
						opbx_log(LOG_DEBUG, "Spawn extension (%s,%s,%d) exited KEEPALIVE on '%s'\n", c->context, c->exten, c->priority, c->name);
					if (option_verbose > 1)
						opbx_verbose( VERBOSE_PREFIX_2 "Spawn extension (%s, %s, %d) exited KEEPALIVE on '%s'\n", c->context, c->exten, c->priority, c->name);
					goto out;
					break;
				default:
					if (option_debug)
						opbx_log(LOG_DEBUG, "Spawn extension (%s,%s,%d) exited non-zero on '%s'\n", c->context, c->exten, c->priority, c->name);
					if (option_verbose > 1)
						opbx_verbose( VERBOSE_PREFIX_2 "Spawn extension (%s, %s, %d) exited non-zero on '%s'\n", c->context, c->exten, c->priority, c->name);
					if (c->_softhangup == OPBX_SOFTHANGUP_ASYNCGOTO) {
						c->_softhangup =0;
						break;
					}
					/* atimeout */
					if (c->_softhangup == OPBX_SOFTHANGUP_TIMEOUT) {
						break;
					}

					if (c->cdr) {
						opbx_cdr_update(c);
					}
					goto out;
				}
			}
			if ((c->_softhangup == OPBX_SOFTHANGUP_TIMEOUT) && (opbx_exists_extension(c,c->context,"T",1,c->cid.cid_num))) {
				opbx_copy_string(c->exten, "T", sizeof(c->exten));
				/* If the AbsoluteTimeout is not reset to 0, we'll get an infinite loop */
				c->whentohangup = 0;
				c->priority = 0;
				c->_softhangup &= ~OPBX_SOFTHANGUP_TIMEOUT;
			} else if (c->_softhangup) {
				opbx_log(LOG_DEBUG, "Extension %s, priority %d returned normally even though call was hung up\n",
					c->exten, c->priority);
				goto out;
			}
			firstpass = 0;
			c->priority++;
		}
		if (!opbx_exists_extension(c, c->context, c->exten, 1, c->cid.cid_num)) {
			/* It's not a valid extension anymore */
			if (opbx_exists_extension(c, c->context, "i", 1, c->cid.cid_num)) {
				if (option_verbose > 2)
					opbx_verbose(VERBOSE_PREFIX_3 "Sent into invalid extension '%s' in context '%s' on %s\n", c->exten, c->context, c->name);
				pbx_builtin_setvar_helper(c, "INVALID_EXTEN", c->exten);
				opbx_copy_string(c->exten, "i", sizeof(c->exten));
				c->priority = 1;
			} else {
				opbx_log(LOG_WARNING, "Channel '%s' sent into invalid extension '%s' in context '%s', but no invalid handler\n",
					c->name, c->exten, c->context);
				goto out;
			}
		} else if (c->_softhangup == OPBX_SOFTHANGUP_TIMEOUT) {
			/* If we get this far with OPBX_SOFTHANGUP_TIMEOUT, then we know that the "T" extension is next. */
			c->_softhangup = 0;
		} else {
			/* Done, wait for an extension */
			waittime = 0;
			if (digit)
				waittime = c->pbx->dtimeout;
			else if (!autofallthrough)
				waittime = c->pbx->rtimeout;
			if (waittime) {
				while (opbx_matchmore_extension(c, c->context, exten, 1, c->cid.cid_num)) {
					/* As long as we're willing to wait, and as long as it's not defined, 
					   keep reading digits until we can't possibly get a right answer anymore.  */
					digit = opbx_waitfordigit(c, waittime * 1000);
					if (c->_softhangup == OPBX_SOFTHANGUP_ASYNCGOTO) {
						c->_softhangup = 0;
					} else {
						if (!digit)
							/* No entry */
							break;
						if (digit < 0)
							/* Error, maybe a  hangup */
							goto out;
						exten[pos++] = digit;
						waittime = c->pbx->dtimeout;
					}
				}
				if (opbx_exists_extension(c, c->context, exten, 1, c->cid.cid_num)) {
					/* Prepare the next cycle */
					opbx_copy_string(c->exten, exten, sizeof(c->exten));
					c->priority = 1;
				} else {
					/* No such extension */
					if (!opbx_strlen_zero(exten)) {
						/* An invalid extension */
						if (opbx_exists_extension(c, c->context, "i", 1, c->cid.cid_num)) {
							if (option_verbose > 2)
								opbx_verbose( VERBOSE_PREFIX_3 "Invalid extension '%s' in context '%s' on %s\n", exten, c->context, c->name);
							pbx_builtin_setvar_helper(c, "INVALID_EXTEN", exten);
							opbx_copy_string(c->exten, "i", sizeof(c->exten));
							c->priority = 1;
						} else {
							opbx_log(LOG_WARNING, "Invalid extension '%s', but no rule 'i' in context '%s'\n", exten, c->context);
							goto out;
						}
					} else {
						/* A simple timeout */
						if (opbx_exists_extension(c, c->context, "t", 1, c->cid.cid_num)) {
							if (option_verbose > 2)
								opbx_verbose( VERBOSE_PREFIX_3 "Timeout on %s\n", c->name);
							opbx_copy_string(c->exten, "t", sizeof(c->exten));
							c->priority = 1;
						} else {
							opbx_log(LOG_WARNING, "Timeout, but no rule 't' in context '%s'\n", c->context);
							goto out;
						}
					}	
				}
				if (c->cdr) {
					if (option_verbose > 2)
						opbx_verbose(VERBOSE_PREFIX_2 "CDR updated on %s\n",c->name);	
					opbx_cdr_update(c);
			    }
			} else {
				char *status;

				// this should really use c->hangupcause instead of dialstatus
				// let's go along with it for now but we should revisit it later
				
				status = pbx_builtin_getvar_helper(c, "DIALSTATUS");
				if (!status) {
					hash = 0;
					status = "UNKNOWN";
				}
				else
					hash = opbx_hash_var_name(status);

				if (option_verbose > 2)
					opbx_verbose(VERBOSE_PREFIX_2 "Auto fallthrough, channel '%s' status is '%s'\n", c->name, status);

				if (hash == OPBX_KEYWORD_BUSY)
					res = pbx_builtin_busy(c, "10");
				else if (hash == OPBX_KEYWORD_CHANUNAVAIL)
					res = pbx_builtin_congestion(c, "10");
				else if (hash == OPBX_KEYWORD_CONGESTION)
					res = pbx_builtin_congestion(c, "10");
				goto out;
			}
		}
	}
	if (firstpass) 
		opbx_log(LOG_WARNING, "Don't know what to do with '%s'\n", c->name);
out:
	if ((res != OPBX_PBX_KEEPALIVE) && opbx_exists_extension(c, c->context, "h", 1, c->cid.cid_num)) {
		c->exten[0] = 'h';
		c->exten[1] = '\0';
		c->priority = 1;
		while(opbx_exists_extension(c, c->context, c->exten, c->priority, c->cid.cid_num)) {
			if ((res = opbx_spawn_extension(c, c->context, c->exten, c->priority, c->cid.cid_num))) {
				/* Something bad happened, or a hangup has been requested. */
				if (option_debug)
					opbx_log(LOG_DEBUG, "Spawn extension (%s,%s,%d) exited non-zero on '%s'\n", c->context, c->exten, c->priority, c->name);
				if (option_verbose > 1)
					opbx_verbose( VERBOSE_PREFIX_2 "Spawn extension (%s, %s, %d) exited non-zero on '%s'\n", c->context, c->exten, c->priority, c->name);
				break;
			}
			c->priority++;
		}
	}
	opbx_set2_flag(c, autoloopflag, OPBX_FLAG_IN_AUTOLOOP);

	pbx_destroy(c->pbx);
	c->pbx = NULL;
	if (res != OPBX_PBX_KEEPALIVE)
		opbx_hangup(c);
	return 0;
}

/* Returns 0 on success, non-zero if call limit was reached */
static int increase_call_count(const struct opbx_channel *c)
{
	int failed = 0;
	double curloadavg;
	opbx_mutex_lock(&maxcalllock);
	if (option_maxcalls) {
		if (countcalls >= option_maxcalls) {
			opbx_log(LOG_NOTICE, "Maximum call limit of %d calls exceeded by '%s'!\n", option_maxcalls, c->name);
			failed = -1;
		}
	}
	if (option_maxload) {
		getloadavg(&curloadavg, 1);
		if (curloadavg >= option_maxload) {
			opbx_log(LOG_NOTICE, "Maximum loadavg limit of %lf load exceeded by '%s' (currently %f)!\n", option_maxload, c->name, curloadavg);
			failed = -1;
		}
	}
	if (!failed)
		countcalls++;	
	opbx_mutex_unlock(&maxcalllock);

	return failed;
}

static void decrease_call_count(void)
{
	opbx_mutex_lock(&maxcalllock);
	if (countcalls > 0)
		countcalls--;
	opbx_mutex_unlock(&maxcalllock);
}

static void *pbx_thread(void *data)
{
	/* Oh joyeous kernel, we're a new thread, with nothing to do but
	   answer this channel and get it going.
	*/
	/* NOTE:
	   The launcher of this function _MUST_ increment 'countcalls'
	   before invoking the function; it will be decremented when the
	   PBX has finished running on the channel
	 */
	struct opbx_channel *c = data;

	__opbx_pbx_run(c);
	decrease_call_count();

	pthread_exit(NULL);

	return NULL;
}

enum opbx_pbx_result opbx_pbx_start(struct opbx_channel *c)
{
	pthread_t t;
	pthread_attr_t attr;

	if (!c) {
		opbx_log(LOG_WARNING, "Asked to start thread on NULL channel?\n");
		return OPBX_PBX_FAILED;
	}
	   
	if (increase_call_count(c))
		return OPBX_PBX_CALL_LIMIT;

	/* Start a new thread, and get something handling this channel. */
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (opbx_pthread_create(&t, &attr, pbx_thread, c)) {
		opbx_log(LOG_WARNING, "Failed to create new channel thread\n");
		return OPBX_PBX_FAILED;
	}

	return OPBX_PBX_SUCCESS;
}

enum opbx_pbx_result opbx_pbx_run(struct opbx_channel *c)
{
	enum opbx_pbx_result res = OPBX_PBX_SUCCESS;

	if (increase_call_count(c))
		return OPBX_PBX_CALL_LIMIT;

	res = __opbx_pbx_run(c);
	decrease_call_count();

	return res;
}

int opbx_active_calls(void)
{
	return countcalls;
}

int pbx_set_autofallthrough(int newval)
{
	int oldval;
	oldval = autofallthrough;
	if (oldval != newval)
		autofallthrough = newval;
	return oldval;
}

/*
 * This function locks contexts list by &conlist, search for the right context
 * structure, leave context list locked and call opbx_context_remove_include2
 * which removes include, unlock contexts list and return ...
 */
int opbx_context_remove_include(const char *context, const char *include, const char *registrar)
{
	struct opbx_context *c;
	unsigned int hash = opbx_hash_string(context);

	if (opbx_lock_contexts()) return -1;

	/* walk contexts and search for the right one ...*/
	c = opbx_walk_contexts(NULL);
	while (c) {
		/* we found one ... */
		if (hash == c->hash) {
			int ret;
			/* remove include from this context ... */	
			ret = opbx_context_remove_include2(c, include, registrar);

			opbx_unlock_contexts();

			/* ... return results */
			return ret;
		}
		c = opbx_walk_contexts(c);
	}

	/* we can't find the right one context */
	opbx_unlock_contexts();
	return -1;
}

/*
 * When we call this function, &conlock lock must be locked, because when
 * we giving *con argument, some process can remove/change this context
 * and after that there can be segfault.
 *
 * This function locks given context, removes include, unlock context and
 * return.
 */
int opbx_context_remove_include2(struct opbx_context *con, const char *include, const char *registrar)
{
	struct opbx_include *i, *pi = NULL;

	if (opbx_mutex_lock(&con->lock)) return -1;

	/* walk includes */
	i = con->includes;
	while (i) {
		/* find our include */
		if (!strcmp(i->name, include) && 
			(!registrar || !strcmp(i->registrar, registrar))) {
			/* remove from list */
			if (pi)
				pi->next = i->next;
			else
				con->includes = i->next;
			/* free include and return */
			free(i);
			opbx_mutex_unlock(&con->lock);
			return 0;
		}
		pi = i;
		i = i->next;
	}

	/* we can't find the right include */
	opbx_mutex_unlock(&con->lock);
	return -1;
}

/*
 * This function locks contexts list by &conlist, search for the rigt context
 * structure, leave context list locked and call opbx_context_remove_switch2
 * which removes switch, unlock contexts list and return ...
 */
int opbx_context_remove_switch(const char *context, const char *sw, const char *data, const char *registrar)
{
	struct opbx_context *c;
	unsigned int hash = opbx_hash_string(context);

	if (opbx_lock_contexts()) return -1;

	/* walk contexts and search for the right one ...*/
	c = opbx_walk_contexts(NULL);
	while (c) {
		/* we found one ... */
		if (hash == c->hash) {
			int ret;
			/* remove switch from this context ... */	
			ret = opbx_context_remove_switch2(c, sw, data, registrar);

			opbx_unlock_contexts();

			/* ... return results */
			return ret;
		}
		c = opbx_walk_contexts(c);
	}

	/* we can't find the right one context */
	opbx_unlock_contexts();
	return -1;
}

/*
 * When we call this function, &conlock lock must be locked, because when
 * we giving *con argument, some process can remove/change this context
 * and after that there can be segfault.
 *
 * This function locks given context, removes switch, unlock context and
 * return.
 */
int opbx_context_remove_switch2(struct opbx_context *con, const char *sw, const char *data, const char *registrar)
{
	struct opbx_sw *i, *pi = NULL;

	if (opbx_mutex_lock(&con->lock)) return -1;

	/* walk switches */
	i = con->alts;
	while (i) {
		/* find our switch */
		if (!strcmp(i->name, sw) && !strcmp(i->data, data) && 
			(!registrar || !strcmp(i->registrar, registrar))) {
			/* remove from list */
			if (pi)
				pi->next = i->next;
			else
				con->alts = i->next;
			/* free switch and return */
			free(i);
			opbx_mutex_unlock(&con->lock);
			return 0;
		}
		pi = i;
		i = i->next;
	}

	/* we can't find the right switch */
	opbx_mutex_unlock(&con->lock);
	return -1;
}

/*
 * This functions lock contexts list, search for the right context,
 * call opbx_context_remove_extension2, unlock contexts list and return.
 * In this function we are using
 */
int opbx_context_remove_extension(const char *context, const char *extension, int priority, const char *registrar)
{
	struct opbx_context *c;
	unsigned int hash = opbx_hash_string(context);

	if (opbx_lock_contexts()) return -1;

	/* walk contexts ... */
	c = opbx_walk_contexts(NULL);
	while (c) {
		/* ... search for the right one ... */
		if (hash == c->hash) {
			/* ... remove extension ... */
			int ret = opbx_context_remove_extension2(c, extension, priority,
				registrar);
			/* ... unlock contexts list and return */
			opbx_unlock_contexts();
			return ret;
		}
		c = opbx_walk_contexts(c);
	}

	/* we can't find the right context */
	opbx_unlock_contexts();
	return -1;
}

/*
 * When do you want to call this function, make sure that &conlock is locked,
 * because some process can handle with your *con context before you lock
 * it.
 *
 * This functionc locks given context, search for the right extension and
 * fires out all peer in this extensions with given priority. If priority
 * is set to 0, all peers are removed. After that, unlock context and
 * return.
 */
int opbx_context_remove_extension2(struct opbx_context *con, const char *extension, int priority, const char *registrar)
{
	struct opbx_exten *exten, *prev_exten = NULL;

	if (opbx_mutex_lock(&con->lock)) return -1;

	/* go through all extensions in context and search the right one ... */
	exten = con->root;
	while (exten) {

		/* look for right extension */
		if (!strcmp(exten->exten, extension) &&
			(!registrar || !strcmp(exten->registrar, registrar))) {
			struct opbx_exten *peer;

			/* should we free all peers in this extension? (priority == 0)? */
			if (priority == 0) {
				/* remove this extension from context list */
				if (prev_exten)
					prev_exten->next = exten->next;
				else
					con->root = exten->next;

				/* fire out all peers */
				peer = exten; 
				while (peer) {
					exten = peer->peer;
					
					if (!peer->priority==PRIORITY_HINT) 
					    opbx_remove_hint(peer);

					peer->datad(peer->data);
					free(peer);

					peer = exten;
				}

				opbx_mutex_unlock(&con->lock);
				return 0;
			} else {
				/* remove only extension with exten->priority == priority */
				struct opbx_exten *previous_peer = NULL;

				peer = exten;
				while (peer) {
					/* is this our extension? */
					if (peer->priority == priority &&
						(!registrar || !strcmp(peer->registrar, registrar) )) {
						/* we are first priority extension? */
						if (!previous_peer) {
							/* exists previous extension here? */
							if (prev_exten) {
								/* yes, so we must change next pointer in
								 * previous connection to next peer
								 */
								if (peer->peer) {
									prev_exten->next = peer->peer;
									peer->peer->next = exten->next;
								} else
									prev_exten->next = exten->next;
							} else {
								/* no previous extension, we are first
								 * extension, so change con->root ...
								 */
								if (peer->peer)
									con->root = peer->peer;
								else
									con->root = exten->next; 
							}
						} else {
							/* we are not first priority in extension */
							previous_peer->peer = peer->peer;
						}

						/* now, free whole priority extension */
						if (peer->priority==PRIORITY_HINT)
						    opbx_remove_hint(peer);
						peer->datad(peer->data);
						free(peer);

						opbx_mutex_unlock(&con->lock);
						return 0;
					} else {
						/* this is not right extension, skip to next peer */
						previous_peer = peer;
						peer = peer->peer;
					}
				}

				opbx_mutex_unlock(&con->lock);
				return -1;
			}
		}

		prev_exten = exten;
		exten = exten->next;
	}

	/* we can't find right extension */
	opbx_mutex_unlock(&con->lock);
	return -1;
}


int opbx_register_application(const char *app, int (*execute)(struct opbx_channel *, void *), const char *synopsis, const char *description)
{
	struct opbx_app *tmp, *prev, *cur;
	char tmps[80];
	int length;
	unsigned int hash = opbx_hash_app_name(app);
	
	length = sizeof(struct opbx_app);
	length += strlen(app) + 1;
	if (opbx_mutex_lock(&applock)) {
		opbx_log(LOG_ERROR, "Unable to lock application list\n");
		return -1;
	}
	tmp = apps;
	while(tmp) {
		if (hash == tmp->hash) {
			opbx_log(LOG_WARNING, "Application '%s' already exists\n", app);
			opbx_mutex_unlock(&applock);
			return -1;
		}
		tmp = tmp->next;
	}
	tmp = malloc(length);
	if (tmp) {
		memset(tmp, 0, length);
		tmp->hash = hash;
		strcpy(tmp->name, app);
		tmp->execute = execute;
		tmp->synopsis = synopsis;
		tmp->description = description;
		
		/* Store in alphabetical order */
		
		// One more reason why the CLI should be removed from the daemon
		// and moved instead into a separate standalone command line utility
		// Alphabetic order is only needed for CLI output and this slows down
		// the daemon's performance unneccessarily, need to revisit later
		
		cur = apps;
		prev = NULL;
		while(cur) {
#ifdef OPBX_USE_CASE_INSENSITIVE_APP_NAMES
			if (strcasecmp(tmp->name, cur->name) < 0)
				break;
#else
			if (strcmp(tmp->name, cur->name) < 0)
				break;
#endif
			prev = cur;
			cur = cur->next;
		}
		if (prev) {
			tmp->next = prev->next;
			prev->next = tmp;
		} else {
			tmp->next = apps;
			apps = tmp;
		}
	} else {
		opbx_log(LOG_ERROR, "Out of memory\n");
		opbx_mutex_unlock(&applock);
		return -1;
	}
	if (option_verbose > 1)
		opbx_verbose( VERBOSE_PREFIX_2 "Registered application '%s'\n", opbx_term_color(tmps, tmp->name, COLOR_BRCYAN, 0, sizeof(tmps)));
	opbx_mutex_unlock(&applock);
	return 0;
}

int opbx_register_switch(struct opbx_switch *sw)
{
	struct opbx_switch *tmp, *prev=NULL;
	if (opbx_mutex_lock(&switchlock)) {
		opbx_log(LOG_ERROR, "Unable to lock switch lock\n");
		return -1;
	}
	tmp = switches;
	while(tmp) {
		if (!strcasecmp(tmp->name, sw->name))
			break;
		prev = tmp;
		tmp = tmp->next;
	}
	if (tmp) {	
		opbx_mutex_unlock(&switchlock);
		opbx_log(LOG_WARNING, "Switch '%s' already found\n", sw->name);
		return -1;
	}
	sw->next = NULL;
	if (prev) 
		prev->next = sw;
	else
		switches = sw;
	opbx_mutex_unlock(&switchlock);
	return 0;
}

void opbx_unregister_switch(struct opbx_switch *sw)
{
	struct opbx_switch *tmp, *prev=NULL;
	if (opbx_mutex_lock(&switchlock)) {
		opbx_log(LOG_ERROR, "Unable to lock switch lock\n");
		return;
	}
	tmp = switches;
	while(tmp) {
		if (tmp == sw) {
			if (prev)
				prev->next = tmp->next;
			else
				switches = tmp->next;
			tmp->next = NULL;
			break;			
		}
		prev = tmp;
		tmp = tmp->next;
	}
	opbx_mutex_unlock(&switchlock);
}

/*
 * Help for CLI commands ...
 */
static char show_application_help[] = 
"Usage: show application <application> [<application> [<application> [...]]]\n"
"       Describes a particular application.\n";

static char show_functions_help[] =
"Usage: show functions\n"
"       List builtin functions accessable as $(function args)\n";

static char show_function_help[] =
"Usage: show function <function>\n"
"       Describe a particular dialplan function.\n";

static char show_applications_help[] =
"Usage: show applications [{like|describing} <text>]\n"
"       List applications which are currently available.\n"
"       If 'like', <text> will be a substring of the app name\n"
"       If 'describing', <text> will be a substring of the description\n";

static char show_dialplan_help[] =
"Usage: show dialplan [exten@][context]\n"
"       Show dialplan\n";

static char show_switches_help[] = 
"Usage: show switches\n"
"       Show registered switches\n";

static char show_hints_help[] = 
"Usage: show hints\n"
"       Show registered hints\n";


/*
 * IMPLEMENTATION OF CLI FUNCTIONS IS IN THE SAME ORDER AS COMMANDS HELPS
 *
 */

/*
 * 'show application' CLI command implementation functions ...
 */

/*
 * There is a possibility to show informations about more than one
 * application at one time. You can type 'show application Dial Echo' and
 * you will see informations about these two applications ...
 */
static char *complete_show_application(char *line, char *word,
	int pos, int state)
{
	struct opbx_app *a;
	int which = 0;

	/* try to lock applications list ... */
	if (opbx_mutex_lock(&applock)) {
		opbx_log(LOG_ERROR, "Unable to lock application list\n");
		return NULL;
	}

	/* ... walk all applications ... */
	a = apps; 
	while (a) {
		/* ... check if word matches this application ... */
		if (!strncasecmp(word, a->name, strlen(word))) {
			/* ... if this is right app serve it ... */
			if (++which > state) {
				char *ret = strdup(a->name);
				opbx_mutex_unlock(&applock);
				return ret;
			}
		}
		a = a->next; 
	}

	/* no application match */
	opbx_mutex_unlock(&applock);
	return NULL; 
}

static int handle_show_application(int fd, int argc, char *argv[])
{
	struct opbx_app *a;
	int app, no_registered_app = 1;

	if (argc < 3) return RESULT_SHOWUSAGE;

	/* try to lock applications list ... */
	if (opbx_mutex_lock(&applock)) {
		opbx_log(LOG_ERROR, "Unable to lock application list\n");
		return -1;
	}

	/* ... go through all applications ... */
	a = apps; 
	while (a) {
		/* ... compare this application name with all arguments given
		 * to 'show application' command ... */
		for (app = 2; app < argc; app++) {
			if (!strcasecmp(a->name, argv[app])) {
				/* Maximum number of characters added by terminal coloring is 22 */
				char infotitle[64 + OPBX_MAX_APP + 22], syntitle[40], destitle[40];
				char info[64 + OPBX_MAX_APP], *synopsis = NULL, *description = NULL;
				int synopsis_size, description_size;

				no_registered_app = 0;

				if (a->synopsis)
					synopsis_size = strlen(a->synopsis) + 23;
				else
					synopsis_size = strlen("Not available") + 23;
				synopsis = alloca(synopsis_size);

				if (a->description)
					description_size = strlen(a->description) + 23;
				else
					description_size = strlen("Not available") + 23;
				description = alloca(description_size);

				if (synopsis && description) {
					snprintf(info, 64 + OPBX_MAX_APP, "\n  -= Info about application '%s' =- \n\n", a->name);
					opbx_term_color(infotitle, info, COLOR_MAGENTA, 0, 64 + OPBX_MAX_APP + 22);
					opbx_term_color(syntitle, "[Synopsis]\n", COLOR_MAGENTA, 0, 40);
					opbx_term_color(destitle, "[Description]\n", COLOR_MAGENTA, 0, 40);
					opbx_term_color(synopsis,
									a->synopsis ? a->synopsis : "Not available",
									COLOR_CYAN, 0, synopsis_size);
					opbx_term_color(description,
									a->description ? a->description : "Not available",
									COLOR_CYAN, 0, description_size);

					opbx_cli(fd,"%s%s%s\n\n%s%s\n", infotitle, syntitle, synopsis, destitle, description);
				} else {
					/* ... one of our applications, show info ...*/
					opbx_cli(fd,"\n  -= Info about application '%s' =- \n\n"
						"[Synopsis]\n  %s\n\n"
						"[Description]\n%s\n",
						a->name,
						a->synopsis ? a->synopsis : "Not available",
						a->description ? a->description : "Not available");
				}
			}
		}
		a = a->next; 
	}

	opbx_mutex_unlock(&applock);

	/* we found at least one app? no? */
	if (no_registered_app) {
		opbx_cli(fd, "Your application(s) is (are) not registered\n");
		return RESULT_FAILURE;
	}

	return RESULT_SUCCESS;
}

/*! \brief  handle_show_hints: CLI support for listing registred dial plan hints */
static int handle_show_hints(int fd, int argc, char *argv[])
{
	struct opbx_hint *hint;
	int num = 0;
	int watchers;
	struct opbx_state_cb *watcher;

	if (!hints) {
		opbx_cli(fd, "There are no registered dialplan hints\n");
		return RESULT_SUCCESS;
	}
	/* ... we have hints ... */
	opbx_cli(fd, "\n    -== Registered OpenPBX Dial Plan Hints ==-\n");
	if (opbx_mutex_lock(&hintlock)) {
		opbx_log(LOG_ERROR, "Unable to lock hints\n");
		return -1;
	}
	hint = hints;
	while (hint) {
		watchers = 0;
		for (watcher = hint->callbacks; watcher; watcher = watcher->next)
			watchers++;
		opbx_cli(fd, "   %-20.20s: %-20.20s  State:%-15.15s Watchers %2d\n",
			opbx_get_extension_name(hint->exten), opbx_get_extension_app(hint->exten),
			opbx_extension_state2str(hint->laststate), watchers);
		num++;
		hint = hint->next;
	}
	opbx_cli(fd, "----------------\n");
	opbx_cli(fd, "- %d hints registered\n", num);
	opbx_mutex_unlock(&hintlock);
	return RESULT_SUCCESS;
}

/*! \brief  handle_show_switches: CLI support for listing registred dial plan switches */
static int handle_show_switches(int fd, int argc, char *argv[])
{
	struct opbx_switch *sw;
	if (!switches) {
		opbx_cli(fd, "There are no registered alternative switches\n");
		return RESULT_SUCCESS;
	}
	/* ... we have applications ... */
	opbx_cli(fd, "\n    -= Registered OpenPBX Alternative Switches =-\n");
	if (opbx_mutex_lock(&switchlock)) {
		opbx_log(LOG_ERROR, "Unable to lock switches\n");
		return -1;
	}
	sw = switches;
	while (sw) {
		opbx_cli(fd, "%s: %s\n", sw->name, sw->description);
		sw = sw->next;
	}
	opbx_mutex_unlock(&switchlock);
	return RESULT_SUCCESS;
}

/*
 * 'show applications' CLI command implementation functions ...
 */
static int handle_show_applications(int fd, int argc, char *argv[])
{
	struct opbx_app *a;
	int like=0, describing=0;
	int total_match = 0; 	/* Number of matches in like clause */
	int total_apps = 0; 	/* Number of apps registered */
	
	/* try to lock applications list ... */
	if (opbx_mutex_lock(&applock)) {
		opbx_log(LOG_ERROR, "Unable to lock application list\n");
		return -1;
	}

	/* ... have we got at least one application (first)? no? */
	if (!apps) {
		opbx_cli(fd, "There are no registered applications\n");
		opbx_mutex_unlock(&applock);
		return -1;
	}

	/* show applications like <keyword> */
	if ((argc == 4) && (!strcmp(argv[2], "like"))) {
		like = 1;
	} else if ((argc > 3) && (!strcmp(argv[2], "describing"))) {
		describing = 1;
	}

	/* show applications describing <keyword1> [<keyword2>] [...] */
	if ((!like) && (!describing)) {
		opbx_cli(fd, "    -= Registered OpenPBX Applications =-\n");
	} else {
		opbx_cli(fd, "    -= Matching OpenPBX Applications =-\n");
	}

	/* ... go through all applications ... */
	for (a = apps; a; a = a->next) {
		/* ... show information about applications ... */
		int printapp=0;
		total_apps++;
		if (like) {
			if (strcasestr(a->name, argv[3])) {
				printapp = 1;
				total_match++;
			}
		} else if (describing) {
			if (a->description) {
				/* Match all words on command line */
				int i;
				printapp = 1;
				for (i=3; i<argc; i++) {
					if (!strcasestr(a->description, argv[i])) {
						printapp = 0;
					} else {
						total_match++;
					}
				}
			}
		} else {
			printapp = 1;
		}

		if (printapp) {
			opbx_cli(fd,"  %20s (%#x): %s\n", a->name, a->hash,
					 a->synopsis ? a->synopsis : "<Synopsis not available>");
		}
	}
	if ((!like) && (!describing)) {
		opbx_cli(fd, "    -= %d Applications Registered =-\n", total_apps);
	} else {
		opbx_cli(fd, "    -= %d Applications Matching =-\n", total_match);
	}
	
	/* ... unlock and return */
	opbx_mutex_unlock(&applock);

	return RESULT_SUCCESS;
}

static char *complete_show_applications(char *line, char *word, int pos, int state)
{
	if (pos == 2) {
		if (opbx_strlen_zero(word)) {
			switch (state) {
			case 0:
				return strdup("like");
			case 1:
				return strdup("describing");
			default:
				return NULL;
			}
		} else if (! strncasecmp(word, "like", strlen(word))) {
			if (state == 0) {
				return strdup("like");
			} else {
				return NULL;
			}
		} else if (! strncasecmp(word, "describing", strlen(word))) {
			if (state == 0) {
				return strdup("describing");
			} else {
				return NULL;
			}
		}
	}
	return NULL;
}

/*
 * 'show dialplan' CLI command implementation functions ...
 */
static char *complete_show_dialplan_context(char *line, char *word, int pos,
	int state)
{
	struct opbx_context *c;
	int which = 0;

	/* we are do completion of [exten@]context on second position only */
	if (pos != 2) return NULL;

	/* try to lock contexts list ... */
	if (opbx_lock_contexts()) {
		opbx_log(LOG_ERROR, "Unable to lock context list\n");
		return NULL;
	}

	/* ... walk through all contexts ... */
	c = opbx_walk_contexts(NULL);
	while(c) {
		/* ... word matches context name? yes? ... */
		if (!strncasecmp(word, opbx_get_context_name(c), strlen(word))) {
			/* ... for serve? ... */
			if (++which > state) {
				/* ... yes, serve this context name ... */
				char *ret = strdup(opbx_get_context_name(c));
				opbx_unlock_contexts();
				return ret;
			}
		}
		c = opbx_walk_contexts(c);
	}

	/* ... unlock and return */
	opbx_unlock_contexts();
	return NULL;
}

struct dialplan_counters {
	int total_context;
	int total_exten;
	int total_prio;
	int context_existence;
	int extension_existence;
};

static int show_dialplan_helper(int fd, char *context, char *exten, struct dialplan_counters *dpc, struct opbx_include *rinclude, int includecount, char *includes[])
{
	struct opbx_context *c;
	int res=0, old_total_exten = dpc->total_exten;

	/* try to lock contexts */
	if (opbx_lock_contexts()) {
		opbx_log(LOG_WARNING, "Failed to lock contexts list\n");
		return -1;
	}

	/* walk all contexts ... */
	for (c = opbx_walk_contexts(NULL); c ; c = opbx_walk_contexts(c)) {
		/* show this context? */
		if (!context ||
			!strcmp(opbx_get_context_name(c), context)) {
			dpc->context_existence = 1;

			/* try to lock context before walking in ... */
			if (!opbx_lock_context(c)) {
				struct opbx_exten *e;
				struct opbx_include *i;
				struct opbx_ignorepat *ip;
				struct opbx_sw *sw;
				char buf[256], buf2[256];
				int context_info_printed = 0;

				/* are we looking for exten too? if yes, we print context
				 * if we our extension only
				 */
				if (!exten) {
					dpc->total_context++;
					opbx_cli(fd, "[ Context '%s' (%#x) created by '%s' ]\n",
						opbx_get_context_name(c), c->hash, opbx_get_context_registrar(c));
					context_info_printed = 1;
				}

				/* walk extensions ... */
				for (e = opbx_walk_context_extensions(c, NULL); e; e = opbx_walk_context_extensions(c, e)) {
					struct opbx_exten *p;
					int prio;

					/* looking for extension? is this our extension? */
					if (exten
                        &&
						!opbx_extension_match(opbx_get_extension_name(e), exten))
					{
						/* we are looking for extension and it's not our
 						 * extension, so skip to next extension */
						continue;
					}

					dpc->extension_existence = 1;

					/* may we print context info? */	
					if (!context_info_printed) {
						dpc->total_context++;
						if (rinclude) {
							/* TODO Print more info about rinclude */
							opbx_cli(fd, "[ Included context '%s' (%#x) created by '%s' ]\n",
								opbx_get_context_name(c), c->hash,
								opbx_get_context_registrar(c));
						} else {
							opbx_cli(fd, "[ Context '%s' (%#x) created by '%s' ]\n",
								opbx_get_context_name(c), c->hash,
								opbx_get_context_registrar(c));
						}
						context_info_printed = 1;
					}
					dpc->total_prio++;

					/* write extension name and first peer */	
					bzero(buf, sizeof(buf));		
					snprintf(buf, sizeof(buf), "'%s' =>",
						opbx_get_extension_name(e));

					prio = opbx_get_extension_priority(e);
					if (prio == PRIORITY_HINT) {
						snprintf(buf2, sizeof(buf2),
							"hint: %s",
							opbx_get_extension_app(e));
					} else {
						snprintf(buf2, sizeof(buf2),
							"%d. %s(%s)",
							prio,
							opbx_get_extension_app(e),
							(char *)opbx_get_extension_app_data(e));
					}

					opbx_cli(fd, "  %-17s %-45s [%s]\n", buf, buf2,
						opbx_get_extension_registrar(e));

					dpc->total_exten++;
					/* walk next extension peers */
					for (p=opbx_walk_extension_priorities(e, e); p; p=opbx_walk_extension_priorities(e, p)) {
						dpc->total_prio++;
						bzero((void *)buf2, sizeof(buf2));
						bzero((void *)buf, sizeof(buf));
						if (opbx_get_extension_label(p))
							snprintf(buf, sizeof(buf), "   [%s]", opbx_get_extension_label(p));
						prio = opbx_get_extension_priority(p);
						if (prio == PRIORITY_HINT) {
							snprintf(buf2, sizeof(buf2),
								"hint: %s",
								opbx_get_extension_app(p));
						} else {
							snprintf(buf2, sizeof(buf2),
								"%d. %s(%s)",
								prio,
								opbx_get_extension_app(p),
								(char *)opbx_get_extension_app_data(p));
						}

						opbx_cli(fd,"  %-17s %-45s [%s]\n",
							buf, buf2,
							opbx_get_extension_registrar(p));
					}
				}

				/* walk included and write info ... */
				for (i = opbx_walk_context_includes(c, NULL); i; i = opbx_walk_context_includes(c, i)) {
					bzero(buf, sizeof(buf));
					snprintf(buf, sizeof(buf), "'%s'",
						opbx_get_include_name(i));
					if (exten) {
						/* Check all includes for the requested extension */
						if (includecount >= OPBX_PBX_MAX_STACK) {
							opbx_log(LOG_NOTICE, "Maximum include depth exceeded!\n");
						} else {
							int dupe=0;
							int x;
							for (x=0;x<includecount;x++) {
								if (!strcasecmp(includes[x], opbx_get_include_name(i))) {
									dupe++;
									break;
								}
							}
							if (!dupe) {
								includes[includecount] = (char *)opbx_get_include_name(i);
								show_dialplan_helper(fd, (char *)opbx_get_include_name(i),
													exten, dpc, i, includecount + 1, includes);
							} else {
								opbx_log(LOG_WARNING, "Avoiding circular include of %s within %s (%#x)\n",
										 opbx_get_include_name(i), context, c->hash);
							}
						}
					} else {
						opbx_cli(fd, "  Include =>        %-45s [%s]\n",
							buf, opbx_get_include_registrar(i));
					}
				}

				/* walk ignore patterns and write info ... */
				for (ip = opbx_walk_context_ignorepats(c, NULL);  ip;  ip = opbx_walk_context_ignorepats(c, ip))
                {
					const char *ipname = opbx_get_ignorepat_name(ip);
					char ignorepat[OPBX_MAX_EXTENSION];

					snprintf(buf, sizeof(buf), "'%s'", ipname);
					snprintf(ignorepat, sizeof(ignorepat), "_%s.", ipname);
					if ((!exten)  ||  opbx_extension_match(ignorepat, exten))
                    {
						opbx_cli(fd, "  Ignore pattern => %-45s [%s]\n",
							buf, opbx_get_ignorepat_registrar(ip));
					}
				}
				if (!rinclude)
                {
					for (sw = opbx_walk_context_switches(c, NULL);  sw;  sw = opbx_walk_context_switches(c, sw))
                    {
						snprintf(buf, sizeof(buf), "'%s/%s'",
							opbx_get_switch_name(sw),
							opbx_get_switch_data(sw));
						opbx_cli(fd, "  Alt. Switch =>    %-45s [%s]\n",
							buf, opbx_get_switch_registrar(sw));	
					}
				}
	
				opbx_unlock_context(c);

				/* if we print something in context, make an empty line */
				if (context_info_printed) opbx_cli(fd, "\r\n");
			}
		}
	}
	opbx_unlock_contexts();

	if (dpc->total_exten == old_total_exten)
    {
		/* Nothing new under the sun */
		return -1;
	}
    return res;
}

static int handle_show_dialplan(int fd, int argc, char *argv[])
{
	char *exten = NULL, *context = NULL;
	/* Variables used for different counters */
	struct dialplan_counters counters;
	char *incstack[OPBX_PBX_MAX_STACK];
	memset(&counters, 0, sizeof(counters));

	if (argc != 2 && argc != 3) 
		return RESULT_SHOWUSAGE;

	/* we obtain [exten@]context? if yes, split them ... */
	if (argc == 3) {
		char *splitter = opbx_strdupa(argv[2]);
		/* is there a '@' character? */
		if (splitter && strchr(argv[2], '@')) {
			/* yes, split into exten & context ... */
			exten   = strsep(&splitter, "@");
			context = splitter;

			/* check for length and change to NULL if opbx_strlen_zero() */
			if (opbx_strlen_zero(exten))
				exten = NULL;
			if (opbx_strlen_zero(context))
				context = NULL;
			show_dialplan_helper(fd, context, exten, &counters, NULL, 0, incstack);
		} else {
			/* no '@' char, only context given */
			context = argv[2];
			if (opbx_strlen_zero(context))
				context = NULL;
			show_dialplan_helper(fd, context, exten, &counters, NULL, 0, incstack);
		}
	} else {
		/* Show complete dial plan */
		show_dialplan_helper(fd, NULL, NULL, &counters, NULL, 0, incstack);
	}

	/* check for input failure and throw some error messages */
	if (context && !counters.context_existence) {
		opbx_cli(fd, "No such context '%s'\n", context);
		return RESULT_FAILURE;
	}

	if (exten && !counters.extension_existence) {
		if (context)
			opbx_cli(fd, "No such extension %s in context %s\n",
				exten, context);
		else
			opbx_cli(fd,
				"No such extension '%s' extension in any context\n",
				exten);
		return RESULT_FAILURE;
	}

	opbx_cli(fd,"-= %d %s (%d %s) in %d %s. =-\n",
				counters.total_exten, counters.total_exten == 1 ? "extension" : "extensions",
				counters.total_prio, counters.total_prio == 1 ? "priority" : "priorities",
				counters.total_context, counters.total_context == 1 ? "context" : "contexts");

	/* everything ok */
	return RESULT_SUCCESS;
}

/*
 * CLI entries for upper commands ...
 */
static struct opbx_cli_entry pbx_cli[] = {
	{ { "show", "applications", NULL }, handle_show_applications,
	  "Shows registered dialplan applications", show_applications_help, complete_show_applications },
	{ { "show", "functions", NULL }, handle_show_functions,
	  "Shows registered dialplan functions", show_functions_help },
	{ { "show" , "function", NULL }, handle_show_function,
	  "Describe a specific dialplan function", show_function_help, complete_show_function },
	{ { "show", "application", NULL }, handle_show_application,
	  "Describe a specific dialplan application", show_application_help, complete_show_application },
	{ { "show", "dialplan", NULL }, handle_show_dialplan,
	  "Show dialplan", show_dialplan_help, complete_show_dialplan_context },
	{ { "show", "switches", NULL },	handle_show_switches,
	  "Show alternative switches", show_switches_help },
	{ { "show", "hints", NULL }, handle_show_hints,
	  "Show dialplan hints", show_hints_help },
};

int opbx_unregister_application(const char *app) 
{
	struct opbx_app *tmp, *tmpl = NULL;
	unsigned int hash = opbx_hash_app_name(app);
	
	if (opbx_mutex_lock(&applock)) {
		opbx_log(LOG_ERROR, "Unable to lock application list\n");
		return -1;
	}
	tmp = apps;
	while(tmp) {
		if (hash == tmp->hash) {
			if (tmpl)
				tmpl->next = tmp->next;
			else
				apps = tmp->next;
			if (option_verbose > 1)
				opbx_verbose( VERBOSE_PREFIX_2 "Unregistered application '%s' (%#x)\n", tmp->name, tmp->hash);
			free(tmp);
			opbx_mutex_unlock(&applock);
			return 0;
		}
		tmpl = tmp;
		tmp = tmp->next;
	}
	opbx_mutex_unlock(&applock);
	return -1;
}

struct opbx_context *opbx_context_create(struct opbx_context **extcontexts, const char *name, const char *registrar)
{
	struct opbx_context *tmp, **local_contexts;
	unsigned int hash = opbx_hash_string(name);
	int length;
	
	length = sizeof(struct opbx_context);
	length += strlen(name) + 1;
	if (!extcontexts) {
		local_contexts = &contexts;
		opbx_mutex_lock(&conlock);
	} else
		local_contexts = extcontexts;

	tmp = *local_contexts;
	while(tmp) {
		if (hash == tmp->hash) {
			opbx_mutex_unlock(&conlock);
			opbx_log(LOG_WARNING, "Failed to register context '%s' because it is already in use\n", name);
			if (!extcontexts)
				opbx_mutex_unlock(&conlock);
			return NULL;
		}
		tmp = tmp->next;
	}
	tmp = malloc(length);
	if (tmp) {
		memset(tmp, 0, length);
		opbx_mutex_init(&tmp->lock);
		tmp->hash = hash;
		strcpy(tmp->name, name);
		tmp->root = NULL;
		tmp->registrar = registrar;
		tmp->next = *local_contexts;
		tmp->includes = NULL;
		tmp->ignorepats = NULL;
		*local_contexts = tmp;
		if (option_debug)
			opbx_log(LOG_DEBUG, "Registered context '%s' (%#x)\n", tmp->name, tmp->hash);
		else if (option_verbose > 2)
			opbx_verbose( VERBOSE_PREFIX_3 "Registered extension context '%s' (%#x)\n", tmp->name, tmp->hash);
	} else
		opbx_log(LOG_ERROR, "Out of memory\n");
	
	if (!extcontexts)
		opbx_mutex_unlock(&conlock);
	return tmp;
}

void __opbx_context_destroy(struct opbx_context *con, const char *registrar);

struct store_hint {
	char *context;
	char *exten;
	struct opbx_state_cb *callbacks;
	int laststate;
	OPBX_LIST_ENTRY(store_hint) list;
	char data[1];
};

OPBX_LIST_HEAD(store_hints, store_hint);

void opbx_merge_contexts_and_delete(struct opbx_context **extcontexts, const char *registrar)
{
	struct opbx_context *tmp, *lasttmp = NULL;
	struct store_hints store;
	struct store_hint *this;
	struct opbx_hint *hint;
	struct opbx_exten *exten;
	int length;
	struct opbx_state_cb *thiscb, *prevcb;

	/* preserve all watchers for hints associated with this registrar */
	OPBX_LIST_HEAD_INIT(&store);
	opbx_mutex_lock(&hintlock);
	for (hint = hints; hint; hint = hint->next) {
		if (hint->callbacks && !strcmp(registrar, hint->exten->parent->registrar)) {
			length = strlen(hint->exten->exten) + strlen(hint->exten->parent->name) + 2 + sizeof(*this);
			this = calloc(1, length);
			if (!this) {
				opbx_log(LOG_WARNING, "Could not allocate memory to preserve hint\n");
				continue;
			}
			this->callbacks = hint->callbacks;
			hint->callbacks = NULL;
			this->laststate = hint->laststate;
			this->context = this->data;
			strcpy(this->data, hint->exten->parent->name);
			this->exten = this->data + strlen(this->context) + 1;
			strcpy(this->exten, hint->exten->exten);
			OPBX_LIST_INSERT_HEAD(&store, this, list);
		}
	}
	opbx_mutex_unlock(&hintlock);

	tmp = *extcontexts;
	opbx_mutex_lock(&conlock);
	if (registrar) {
		__opbx_context_destroy(NULL,registrar);
		while (tmp) {
			lasttmp = tmp;
			tmp = tmp->next;
		}
	} else {
		while (tmp) {
			__opbx_context_destroy(tmp,tmp->registrar);
			lasttmp = tmp;
			tmp = tmp->next;
		}
	}
	if (lasttmp) {
		lasttmp->next = contexts;
		contexts = *extcontexts;
		*extcontexts = NULL;
	} else 
		opbx_log(LOG_WARNING, "Requested contexts could not be merged\n");
	opbx_mutex_unlock(&conlock);

	/* restore the watchers for hints that can be found; notify those that
	   cannot be restored
	*/
	while ((this = OPBX_LIST_REMOVE_HEAD(&store, list))) {
		exten = opbx_hint_extension(NULL, this->context, this->exten);
		/* Find the hint in the list of hints */
		opbx_mutex_lock(&hintlock);
		for (hint = hints; hint; hint = hint->next) {
			if (hint->exten == exten)
				break;
		}
		if (!exten || !hint) {
			/* this hint has been removed, notify the watchers */
			prevcb = NULL;
			thiscb = this->callbacks;
			while (thiscb) {
				prevcb = thiscb;	    
				thiscb = thiscb->next;
				prevcb->callback(this->context, this->exten, OPBX_EXTENSION_REMOVED, prevcb->data);
				free(prevcb);
	    		}
		} else {
			thiscb = this->callbacks;
			while (thiscb->next)
				thiscb = thiscb->next;
			thiscb->next = hint->callbacks;
			hint->callbacks = this->callbacks;
			hint->laststate = this->laststate;
		}
		opbx_mutex_unlock(&hintlock);
		free(this);
	}

	return;	
}

/*
 * errno values
 *  EBUSY  - can't lock
 *  ENOENT - no existence of context
 */
int opbx_context_add_include(const char *context, const char *include, const char *registrar)
{
	struct opbx_context *c;
	unsigned int hash = opbx_hash_string(context);

	if (opbx_lock_contexts()) {
		errno = EBUSY;
		return -1;
	}

	/* walk contexts ... */
	c = opbx_walk_contexts(NULL);
	while (c) {
		/* ... search for the right one ... */
		if (hash == c->hash) {
			int ret = opbx_context_add_include2(c, include, registrar);
			/* ... unlock contexts list and return */
			opbx_unlock_contexts();
			return ret;
		}
		c = opbx_walk_contexts(c);
	}

	/* we can't find the right context */
	opbx_unlock_contexts();
	errno = ENOENT;
	return -1;
}

#define FIND_NEXT \
do { \
	c = info; \
	while(*c && (*c != '|')) c++; \
	if (*c) { *c = '\0'; c++; } else c = NULL; \
} while(0)

static void get_timerange(struct opbx_timing *i, char *times)
{
	char *e;
	int x;
	int s1, s2;
	int e1, e2;
	/*	int cth, ctm; */

	/* start disabling all times, fill the fields with 0's, as they may contain garbage */
	memset(i->minmask, 0, sizeof(i->minmask));
	
	/* Star is all times */
	if (opbx_strlen_zero(times) || !strcmp(times, "*")) {
		for (x=0; x<24; x++)
			i->minmask[x] = (1 << 30) - 1;
		return;
	}
	/* Otherwise expect a range */
	e = strchr(times, '-');
	if (!e) {
		opbx_log(LOG_WARNING, "Time range is not valid. Assuming no restrictions based on time.\n");
		return;
	}
	*e = '\0';
	e++;
	while(*e && !isdigit(*e)) 
		e++;
	if (!*e) {
		opbx_log(LOG_WARNING, "Invalid time range.  Assuming no restrictions based on time.\n");
		return;
	}
	if (sscanf(times, "%d:%d", &s1, &s2) != 2) {
		opbx_log(LOG_WARNING, "%s isn't a time.  Assuming no restrictions based on time.\n", times);
		return;
	}
	if (sscanf(e, "%d:%d", &e1, &e2) != 2) {
		opbx_log(LOG_WARNING, "%s isn't a time.  Assuming no restrictions based on time.\n", e);
		return;
	}

#if 1
	s1 = s1 * 30 + s2/2;
	if ((s1 < 0) || (s1 >= 24*30)) {
		opbx_log(LOG_WARNING, "%s isn't a valid start time. Assuming no time.\n", times);
		return;
	}
	e1 = e1 * 30 + e2/2;
	if ((e1 < 0) || (e1 >= 24*30)) {
		opbx_log(LOG_WARNING, "%s isn't a valid end time. Assuming no time.\n", e);
		return;
	}
	/* Go through the time and enable each appropriate bit */
	for (x=s1;x != e1;x = (x + 1) % (24 * 30)) {
		i->minmask[x/30] |= (1 << (x % 30));
	}
	/* Do the last one */
	i->minmask[x/30] |= (1 << (x % 30));
#else
	for (cth=0; cth<24; cth++) {
		/* Initialize masks to blank */
		i->minmask[cth] = 0;
		for (ctm=0; ctm<30; ctm++) {
			if (
			/* First hour with more than one hour */
			      (((cth == s1) && (ctm >= s2)) &&
			       ((cth < e1)))
			/* Only one hour */
			||    (((cth == s1) && (ctm >= s2)) &&
			       ((cth == e1) && (ctm <= e2)))
			/* In between first and last hours (more than 2 hours) */
			||    ((cth > s1) &&
			       (cth < e1))
			/* Last hour with more than one hour */
			||    ((cth > s1) &&
			       ((cth == e1) && (ctm <= e2)))
			)
				i->minmask[cth] |= (1 << (ctm / 2));
		}
	}
#endif
	/* All done */
	return;
}

static char *days[] =
{
	"sun",
	"mon",
	"tue",
	"wed",
	"thu",
	"fri",
	"sat",
};

/*! \brief  get_dow: Get day of week */
static unsigned int get_dow(char *dow)
{
	char *c;
	/* The following line is coincidence, really! */
	int s, e, x;
	unsigned int mask;

	/* Check for all days */
	if (opbx_strlen_zero(dow) || !strcmp(dow, "*"))
		return (1 << 7) - 1;
	/* Get start and ending days */
	c = strchr(dow, '-');
	if (c) {
		*c = '\0';
		c++;
	} else
		c = NULL;
	/* Find the start */
	s = 0;
	while((s < 7) && strcasecmp(dow, days[s])) s++;
	if (s >= 7) {
		opbx_log(LOG_WARNING, "Invalid day '%s', assuming none\n", dow);
		return 0;
	}
	if (c) {
		e = 0;
		while((e < 7) && strcasecmp(c, days[e])) e++;
		if (e >= 7) {
			opbx_log(LOG_WARNING, "Invalid day '%s', assuming none\n", c);
			return 0;
		}
	} else
		e = s;
	mask = 0;
	for (x=s; x != e; x = (x + 1) % 7) {
		mask |= (1 << x);
	}
	/* One last one */
	mask |= (1 << x);
	return mask;
}

static unsigned int get_day(char *day)
{
	char *c;
	/* The following line is coincidence, really! */
	int s, e, x;
	unsigned int mask;

	/* Check for all days */
	if (opbx_strlen_zero(day) || !strcmp(day, "*")) {
		mask = (1 << 30)  + ((1 << 30) - 1);
		return mask;
	}
	/* Get start and ending days */
	c = strchr(day, '-');
	if (c) {
		*c = '\0';
		c++;
	}
	/* Find the start */
	if (sscanf(day, "%d", &s) != 1) {
		opbx_log(LOG_WARNING, "Invalid day '%s', assuming none\n", day);
		return 0;
	}
	if ((s < 1) || (s > 31)) {
		opbx_log(LOG_WARNING, "Invalid day '%s', assuming none\n", day);
		return 0;
	}
	s--;
	if (c) {
		if (sscanf(c, "%d", &e) != 1) {
			opbx_log(LOG_WARNING, "Invalid day '%s', assuming none\n", c);
			return 0;
		}
		if ((e < 1) || (e > 31)) {
			opbx_log(LOG_WARNING, "Invalid day '%s', assuming none\n", c);
			return 0;
		}
		e--;
	} else
		e = s;
	mask = 0;
	for (x=s; x!=e; x = (x + 1) % 31) {
		mask |= (1 << x);
	}
	mask |= (1 << x);
	return mask;
}

static char *months[] =
{
	"jan",
	"feb",
	"mar",
	"apr",
	"may",
	"jun",
	"jul",
	"aug",
	"sep",
	"oct",
	"nov",
	"dec",
};

static unsigned int get_month(char *mon)
{
	char *c;
	/* The following line is coincidence, really! */
	int s, e, x;
	unsigned int mask;

	/* Check for all days */
	if (opbx_strlen_zero(mon) || !strcmp(mon, "*")) 
		return (1 << 12) - 1;
	/* Get start and ending days */
	c = strchr(mon, '-');
	if (c) {
		*c = '\0';
		c++;
	}
	/* Find the start */
	s = 0;
	while((s < 12) && strcasecmp(mon, months[s])) s++;
	if (s >= 12) {
		opbx_log(LOG_WARNING, "Invalid month '%s', assuming none\n", mon);
		return 0;
	}
	if (c) {
		e = 0;
		while((e < 12) && strcasecmp(mon, months[e])) e++;
		if (e >= 12) {
			opbx_log(LOG_WARNING, "Invalid month '%s', assuming none\n", c);
			return 0;
		}
	} else
		e = s;
	mask = 0;
	for (x=s; x!=e; x = (x + 1) % 12) {
		mask |= (1 << x);
	}
	/* One last one */
	mask |= (1 << x);
	return mask;
}

int opbx_build_timing(struct opbx_timing *i, char *info_in)
{
	char info_save[256];
	char *info;
	char *c;

	/* Check for empty just in case */
	if (opbx_strlen_zero(info_in))
		return 0;
	/* make a copy just in case we were passed a static string */
	opbx_copy_string(info_save, info_in, sizeof(info_save));
	info = info_save;
	/* Assume everything except time */
	i->monthmask = (1 << 12) - 1;
	i->daymask = (1 << 30) - 1 + (1 << 30);
	i->dowmask = (1 << 7) - 1;
	/* Avoid using str tok */
	FIND_NEXT;
	/* Info has the time range, start with that */
	get_timerange(i, info);
	info = c;
	if (!info)
		return 1;
	FIND_NEXT;
	/* Now check for day of week */
	i->dowmask = get_dow(info);

	info = c;
	if (!info)
		return 1;
	FIND_NEXT;
	/* Now check for the day of the month */
	i->daymask = get_day(info);
	info = c;
	if (!info)
		return 1;
	FIND_NEXT;
	/* And finally go for the month */
	i->monthmask = get_month(info);

	return 1;
}

int opbx_check_timing(struct opbx_timing *i)
{
	struct tm tm;
	time_t t;

	time(&t);
	localtime_r(&t,&tm);

	/* If it's not the right month, return */
	if (!(i->monthmask & (1 << tm.tm_mon))) {
		return 0;
	}

	/* If it's not that time of the month.... */
	/* Warning, tm_mday has range 1..31! */
	if (!(i->daymask & (1 << (tm.tm_mday-1))))
		return 0;

	/* If it's not the right day of the week */
	if (!(i->dowmask & (1 << tm.tm_wday)))
		return 0;

	/* Sanity check the hour just to be safe */
	if ((tm.tm_hour < 0) || (tm.tm_hour > 23)) {
		opbx_log(LOG_WARNING, "Insane time...\n");
		return 0;
	}

	/* Now the tough part, we calculate if it fits
	   in the right time based on min/hour */
	if (!(i->minmask[tm.tm_hour] & (1 << (tm.tm_min / 2))))
		return 0;

	/* If we got this far, then we're good */
	return 1;
}

/*
 * errno values
 *  ENOMEM - out of memory
 *  EBUSY  - can't lock
 *  EEXIST - already included
 *  EINVAL - there is no existence of context for inclusion
 */
int opbx_context_add_include2(struct opbx_context *con, const char *value,
	const char *registrar)
{
	struct opbx_include *new_include;
	char *c;
	struct opbx_include *i, *il = NULL; /* include, include_last */
	int length;
	char *p;
	
	length = sizeof(struct opbx_include);
	length += 2 * (strlen(value) + 1);

	/* allocate new include structure ... */
	if (!(new_include = malloc(length))) {
		opbx_log(LOG_ERROR, "Out of memory\n");
		errno = ENOMEM;
		return -1;
	}
	
	/* ... fill in this structure ... */
	memset(new_include, 0, length);
	p = new_include->stuff;
	new_include->name = p;
	strcpy(new_include->name, value);
	p += strlen(value) + 1;
	new_include->rname = p;
	strcpy(new_include->rname, value);
	c = new_include->rname;
	/* Strip off timing info */
	while(*c && (*c != '|')) 
		c++; 
	/* Process if it's there */
	if (*c) {
	        new_include->hastime = opbx_build_timing(&(new_include->timing), c+1);
		*c = '\0';
	}
	new_include->next      = NULL;
	new_include->registrar = registrar;

	/* ... try to lock this context ... */
	if (opbx_mutex_lock(&con->lock)) {
		free(new_include);
		errno = EBUSY;
		return -1;
	}

	/* ... go to last include and check if context is already included too... */
	i = con->includes;
	while (i) {
		if (!strcasecmp(i->name, new_include->name)) {
			free(new_include);
			opbx_mutex_unlock(&con->lock);
			errno = EEXIST;
			return -1;
		}
		il = i;
		i = i->next;
	}

	/* ... include new context into context list, unlock, return */
	if (il)
		il->next = new_include;
	else
		con->includes = new_include;
	if (option_verbose > 2)
		opbx_verbose(VERBOSE_PREFIX_3 "Including context '%s' in context '%s'\n", new_include->name, opbx_get_context_name(con)); 
	opbx_mutex_unlock(&con->lock);

	return 0;
}

/*
 * errno values
 *  EBUSY  - can't lock
 *  ENOENT - no existence of context
 */
int opbx_context_add_switch(const char *context, const char *sw, const char *data, int eval, const char *registrar)
{
	struct opbx_context *c;
	unsigned int hash = opbx_hash_string(context);
	
	if (opbx_lock_contexts()) {
		errno = EBUSY;
		return -1;
	}

	/* walk contexts ... */
	c = opbx_walk_contexts(NULL);
	while (c) {
		/* ... search for the right one ... */
		if (hash == c->hash) {
			int ret = opbx_context_add_switch2(c, sw, data, eval, registrar);
			/* ... unlock contexts list and return */
			opbx_unlock_contexts();
			return ret;
		}
		c = opbx_walk_contexts(c);
	}

	/* we can't find the right context */
	opbx_unlock_contexts();
	errno = ENOENT;
	return -1;
}

/*
 * errno values
 *  ENOMEM - out of memory
 *  EBUSY  - can't lock
 *  EEXIST - already included
 *  EINVAL - there is no existence of context for inclusion
 */
int opbx_context_add_switch2(struct opbx_context *con, const char *value,
	const char *data, int eval, const char *registrar)
{
	struct opbx_sw *new_sw;
	struct opbx_sw *i, *il = NULL; /* sw, sw_last */
	int length;
	char *p;
	
	length = sizeof(struct opbx_sw);
	length += strlen(value) + 1;
	if (data)
		length += strlen(data);
	length++;
	if (eval) {
		/* Create buffer for evaluation of variables */
		length += SWITCH_DATA_LENGTH;
		length++;
	}

	/* allocate new sw structure ... */
	if (!(new_sw = malloc(length))) {
		opbx_log(LOG_ERROR, "Out of memory\n");
		errno = ENOMEM;
		return -1;
	}
	
	/* ... fill in this structure ... */
	memset(new_sw, 0, length);
	p = new_sw->stuff;
	new_sw->name = p;
	strcpy(new_sw->name, value);
	p += strlen(value) + 1;
	new_sw->data = p;
	if (data) {
		strcpy(new_sw->data, data);
		p += strlen(data) + 1;
	} else {
		strcpy(new_sw->data, "");
		p++;
	}
	if (eval) 
		new_sw->tmpdata = p;
	new_sw->next      = NULL;
	new_sw->eval	  = eval;
	new_sw->registrar = registrar;

	/* ... try to lock this context ... */
	if (opbx_mutex_lock(&con->lock)) {
		free(new_sw);
		errno = EBUSY;
		return -1;
	}

	/* ... go to last sw and check if context is already swd too... */
	i = con->alts;
	while (i) {
		if (!strcasecmp(i->name, new_sw->name) && !strcasecmp(i->data, new_sw->data)) {
			free(new_sw);
			opbx_mutex_unlock(&con->lock);
			errno = EEXIST;
			return -1;
		}
		il = i;
		i = i->next;
	}

	/* ... sw new context into context list, unlock, return */
	if (il)
		il->next = new_sw;
	else
		con->alts = new_sw;
	if (option_verbose > 2)
		opbx_verbose(VERBOSE_PREFIX_3 "Including switch '%s/%s' in context '%s'\n", new_sw->name, new_sw->data, opbx_get_context_name(con)); 
	opbx_mutex_unlock(&con->lock);

	return 0;
}

/*
 * EBUSY  - can't lock
 * ENOENT - there is not context existence
 */
int opbx_context_remove_ignorepat(const char *context, const char *ignorepat, const char *registrar)
{
	struct opbx_context *c;
	unsigned int hash = opbx_hash_string(context);

	if (opbx_lock_contexts()) {
		errno = EBUSY;
		return -1;
	}

	c = opbx_walk_contexts(NULL);
	while (c) {
		if (hash == c->hash) {
			int ret = opbx_context_remove_ignorepat2(c, ignorepat, registrar);
			opbx_unlock_contexts();
			return ret;
		}
		c = opbx_walk_contexts(c);
	}

	opbx_unlock_contexts();
	errno = ENOENT;
	return -1;
}

int opbx_context_remove_ignorepat2(struct opbx_context *con, const char *ignorepat, const char *registrar)
{
	struct opbx_ignorepat *ip, *ipl = NULL;

	if (opbx_mutex_lock(&con->lock)) {
		errno = EBUSY;
		return -1;
	}

	ip = con->ignorepats;
	while (ip) {
		if (!strcmp(ip->pattern, ignorepat) &&
			(!registrar || (registrar == ip->registrar))) {
			if (ipl) {
				ipl->next = ip->next;
				free(ip);
			} else {
				con->ignorepats = ip->next;
				free(ip);
			}
			opbx_mutex_unlock(&con->lock);
			return 0;
		}
		ipl = ip; ip = ip->next;
	}

	opbx_mutex_unlock(&con->lock);
	errno = EINVAL;
	return -1;
}

/*
 * EBUSY - can't lock
 * ENOENT - there is no existence of context
 */
int opbx_context_add_ignorepat(const char *con, const char *value, const char *registrar)
{
	struct opbx_context *c;
	unsigned int hash = opbx_hash_string(con);

	if (opbx_lock_contexts()) {
		errno = EBUSY;
		return -1;
	}

	c = opbx_walk_contexts(NULL);
	while (c) {
		if (hash == c->hash) {
			int ret = opbx_context_add_ignorepat2(c, value, registrar);
			opbx_unlock_contexts();
			return ret;
		} 
		c = opbx_walk_contexts(c);
	}

	opbx_unlock_contexts();
	errno = ENOENT;
	return -1;
}

int opbx_context_add_ignorepat2(struct opbx_context *con, const char *value, const char *registrar)
{
	struct opbx_ignorepat *ignorepat, *ignorepatc, *ignorepatl = NULL;
	int length;
	length = sizeof(struct opbx_ignorepat);
	length += strlen(value) + 1;
	ignorepat = malloc(length);
	if (!ignorepat) {
		opbx_log(LOG_ERROR, "Out of memory\n");
		errno = ENOMEM;
		return -1;
	}
	memset(ignorepat, 0, length);
	strcpy(ignorepat->pattern, value);
	ignorepat->next = NULL;
	ignorepat->registrar = registrar;
	opbx_mutex_lock(&con->lock);
	ignorepatc = con->ignorepats;
	while(ignorepatc) {
		ignorepatl = ignorepatc;
		if (!strcasecmp(ignorepatc->pattern, value)) {
			/* Already there */
			opbx_mutex_unlock(&con->lock);
			errno = EEXIST;
			return -1;
		}
		ignorepatc = ignorepatc->next;
	}
	if (ignorepatl) 
		ignorepatl->next = ignorepat;
	else
		con->ignorepats = ignorepat;
	opbx_mutex_unlock(&con->lock);
	return 0;
	
}

int opbx_ignore_pattern(const char *context, const char *pattern)
{
	struct opbx_context *con;
	struct opbx_ignorepat *pat;

	con = opbx_context_find(context);
	if (con)
    {
		pat = con->ignorepats;
		while (pat)
        {
            switch (opbx_extension_pattern_match(pattern, pat->pattern))
            {
            case EXTENSION_MATCH_EXACT:
            case EXTENSION_MATCH_STRETCHABLE:
            case EXTENSION_MATCH_POSSIBLE:
				return 1;
            }
			pat = pat->next;
		}
	} 
	return 0;
}

/*
 * EBUSY   - can't lock
 * ENOENT  - no existence of context
 *
 */
int opbx_add_extension(const char *context, int replace, const char *extension, int priority, const char *label, const char *callerid,
	const char *application, void *data, void (*datad)(void *), const char *registrar)
{
	struct opbx_context *c;
	unsigned int hash = opbx_hash_string(context);

	if (opbx_lock_contexts()) {
		errno = EBUSY;
		return -1;
	}

	c = opbx_walk_contexts(NULL);
	while (c) {
		if (hash == c->hash) {
			int ret = opbx_add_extension2(c, replace, extension, priority, label, callerid,
				application, data, datad, registrar);
			opbx_unlock_contexts();
			return ret;
		}
		c = opbx_walk_contexts(c);
	}

	opbx_unlock_contexts();
	errno = ENOENT;
	return -1;
}

int opbx_explicit_goto(struct opbx_channel *chan, const char *context, const char *exten, int priority)
{
	if (!chan)
		return -1;

	if (!opbx_strlen_zero(context))
		opbx_copy_string(chan->context, context, sizeof(chan->context));
	if (!opbx_strlen_zero(exten))
		opbx_copy_string(chan->exten, exten, sizeof(chan->exten));
	if (priority > -1) {
		chan->priority = priority;
		/* see flag description in channel.h for explanation */
		if (opbx_test_flag(chan, OPBX_FLAG_IN_AUTOLOOP))
			chan->priority--;
	}
	
	return 0;
}

int opbx_async_goto(struct opbx_channel *chan, const char *context, const char *exten, int priority)
{
	int res = 0;

	opbx_mutex_lock(&chan->lock);

	if (chan->pbx) {
		/* This channel is currently in the PBX */
		opbx_explicit_goto(chan, context, exten, priority);
		opbx_softhangup_nolock(chan, OPBX_SOFTHANGUP_ASYNCGOTO);
	} else {
		/* In order to do it when the channel doesn't really exist within
		   the PBX, we have to make a new channel, masquerade, and start the PBX
		   at the new location */
		struct opbx_channel *tmpchan;
		tmpchan = opbx_channel_alloc(0);
		if (tmpchan) {
			snprintf(tmpchan->name, sizeof(tmpchan->name), "AsyncGoto/%s", chan->name);
			opbx_setstate(tmpchan, chan->_state);
			/* Make formats okay */
			tmpchan->readformat = chan->readformat;
			tmpchan->writeformat = chan->writeformat;
			/* Setup proper location */
			opbx_explicit_goto(tmpchan,
					  (!opbx_strlen_zero(context)) ? context : chan->context,
					  (!opbx_strlen_zero(exten)) ? exten : chan->exten,
					  priority);

			/* Masquerade into temp channel */
			opbx_channel_masquerade(tmpchan, chan);
		
			/* Grab the locks and get going */
			opbx_mutex_lock(&tmpchan->lock);
			opbx_do_masquerade(tmpchan);
			opbx_mutex_unlock(&tmpchan->lock);
			/* Start the PBX going on our stolen channel */
			if (opbx_pbx_start(tmpchan)) {
				opbx_log(LOG_WARNING, "Unable to start PBX on %s\n", tmpchan->name);
				opbx_hangup(tmpchan);
				res = -1;
			}
		} else {
			res = -1;
		}
	}
	opbx_mutex_unlock(&chan->lock);
	return res;
}

int opbx_async_goto_by_name(const char *channame, const char *context, const char *exten, int priority)
{
	struct opbx_channel *chan;
	int res = -1;

	chan = opbx_get_channel_by_name_locked(channame);
	if (chan) {
		res = opbx_async_goto(chan, context, exten, priority);
		opbx_mutex_unlock(&chan->lock);
	}
	return res;
}

static int ext_strncpy(char *dst, const char *src, int len)
{
	int count=0;

	while (*src  &&  (count < len - 1))
    {
		switch (*src)
        {
		case ' ':
			/*	otherwise exten => [a-b],1,... doesn't work */
			/*		case '-': */
			/* Ignore */
			break;
		default:
			*dst = *src;
			dst++;
		}
		src++;
		count++;
	}
	*dst = '\0';
	return count;
}

static void null_datad(void *foo)
{
}

/*
 * EBUSY - can't lock
 * EEXIST - extension with the same priority exist and no replace is set
 *
 */
int opbx_add_extension2(struct opbx_context *con,
					  int replace, const char *extension, int priority, const char *label, const char *callerid,
					  const char *application, void *data, void (*datad)(void *),
					  const char *registrar)
{

#define LOG do { 	if (option_debug) {\
		if (tmp->matchcid) { \
			opbx_log(LOG_DEBUG, "Added extension '%s' priority %d (CID match '%s') to %s\n", tmp->exten, tmp->priority, tmp->cidmatch, con->name); \
		} else { \
			opbx_log(LOG_DEBUG, "Added extension '%s' priority %d to %s\n", tmp->exten, tmp->priority, con->name); \
		} \
	} else if (option_verbose > 2) { \
		if (tmp->matchcid) { \
			opbx_verbose( VERBOSE_PREFIX_3 "Added extension '%s' priority %d (CID match '%s')to %s\n", tmp->exten, tmp->priority, tmp->cidmatch, con->name); \
		} else {  \
			opbx_verbose( VERBOSE_PREFIX_3 "Added extension '%s' priority %d to %s\n", tmp->exten, tmp->priority, con->name); \
		} \
	} } while(0)

	/*
	 * This is a fairly complex routine.  Different extensions are kept
	 * in order by the extension number.  Then, extensions of different
	 * priorities (same extension) are kept in a list, according to the
	 * peer pointer.
	 */
	struct opbx_exten *tmp, *e, *el = NULL, *ep = NULL;
	int res;
	int length;
	char *p;
	unsigned int hash = opbx_hash_string(extension);

	length = sizeof(struct opbx_exten);
	length += strlen(extension) + 1;
	length += strlen(application) + 1;
	if (label)
		length += strlen(label) + 1;
	if (callerid)
		length += strlen(callerid) + 1;
	else
		length ++;

	/* Be optimistic:  Build the extension structure first */
	if (datad == NULL)
		datad = null_datad;
	tmp = malloc(length);
	if (tmp) {
		memset(tmp, 0, length);
		tmp->hash = hash;
		p = tmp->stuff;
		if (label) {
			tmp->label = p;
			strcpy(tmp->label, label);
			p += strlen(label) + 1;
		}
		tmp->exten = p;
		p += ext_strncpy(tmp->exten, extension, strlen(extension) + 1) + 1;
		tmp->priority = priority;
		tmp->cidmatch = p;
		if (callerid) {
			p += ext_strncpy(tmp->cidmatch, callerid, strlen(callerid) + 1) + 1;
			tmp->matchcid = 1;
		} else {
			tmp->cidmatch[0] = '\0';
			tmp->matchcid = 0;
			p++;
		}
		tmp->app = p;
		strcpy(tmp->app, application);
		tmp->parent = con;
		tmp->data = data;
		tmp->datad = datad;
		tmp->registrar = registrar;
		tmp->peer = NULL;
		tmp->next =  NULL;
	} else {
		opbx_log(LOG_ERROR, "Out of memory\n");
		errno = ENOMEM;
		return -1;
	}
	if (opbx_mutex_lock(&con->lock)) {
		free(tmp);
		/* And properly destroy the data */
		datad(data);
		opbx_log(LOG_WARNING, "Failed to lock context '%s' (%#x)\n", con->name, con->hash);
		errno = EBUSY;
		return -1;
	}
	e = con->root;
	while(e) {
		/* Make sure patterns are always last! */
		if ((e->exten[0] != '_') && (extension[0] == '_'))
			res = -1;
		else if ((e->exten[0] == '_') && (extension[0] != '_'))
			res = 1;
		else
			res= strcmp(e->exten, extension);
		if (!res) {
			if (!e->matchcid && !tmp->matchcid)
				res = 0;
			else if (tmp->matchcid && !e->matchcid)
				res = 1;
			else if (e->matchcid && !tmp->matchcid)
				res = -1;
			else
				res = strcasecmp(e->cidmatch, tmp->cidmatch);
		}
		if (res == 0) {
			/* We have an exact match, now we find where we are
			   and be sure there's no duplicates */
			while(e) {
				if (e->priority == tmp->priority) {
					/* Can't have something exactly the same.  Is this a
					   replacement?  If so, replace, otherwise, bonk. */
					if (replace) {
						if (ep) {
							/* We're in the peer list, insert ourselves */
							ep->peer = tmp;
							tmp->peer = e->peer;
						} else if (el) {
							/* We're the first extension. Take over e's functions */
							el->next = tmp;
							tmp->next = e->next;
							tmp->peer = e->peer;
						} else {
							/* We're the very first extension.  */
							con->root = tmp;
							tmp->next = e->next;
							tmp->peer = e->peer;
						}
						if (tmp->priority == PRIORITY_HINT)
						    opbx_change_hint(e,tmp);
						/* Destroy the old one */
						e->datad(e->data);
						free(e);
						opbx_mutex_unlock(&con->lock);
						if (tmp->priority == PRIORITY_HINT)
						    opbx_change_hint(e, tmp);
						/* And immediately return success. */
						LOG;
						return 0;
					} else {
						opbx_log(LOG_WARNING, "Unable to register extension '%s', priority %d in '%s' (%#x), already in use\n",
								 tmp->exten, tmp->priority, con->name, con->hash);
						tmp->datad(tmp->data);
						free(tmp);
						opbx_mutex_unlock(&con->lock);
						errno = EEXIST;
						return -1;
					}
				} else if (e->priority > tmp->priority) {
					/* Slip ourselves in just before e */
					if (ep) {
						/* Easy enough, we're just in the peer list */
						ep->peer = tmp;
						tmp->peer = e;
					} else if (el) {
						/* We're the first extension in this peer list */
						el->next = tmp;
						tmp->next = e->next;
						e->next = NULL;
						tmp->peer = e;
					} else {
						/* We're the very first extension altogether */
						tmp->next = con->root->next;
						/* Con->root must always exist or we couldn't get here */
						tmp->peer = con->root;
						con->root = tmp;
					}
					opbx_mutex_unlock(&con->lock);

					/* And immediately return success. */
					if (tmp->priority == PRIORITY_HINT)
						 opbx_add_hint(tmp);
					
					LOG;
					return 0;
				}
				ep = e;
				e = e->peer;
			}
			/* If we make it here, then it's time for us to go at the very end.
			   ep *must* be defined or we couldn't have gotten here. */
			ep->peer = tmp;
			opbx_mutex_unlock(&con->lock);
			if (tmp->priority == PRIORITY_HINT)
				opbx_add_hint(tmp);
			
			/* And immediately return success. */
			LOG;
			return 0;
				
		} else if (res > 0) {
			/* Insert ourselves just before 'e'.  We're the first extension of
			   this kind */
			tmp->next = e;
			if (el) {
				/* We're in the list somewhere */
				el->next = tmp;
			} else {
				/* We're at the top of the list */
				con->root = tmp;
			}
			opbx_mutex_unlock(&con->lock);
			if (tmp->priority == PRIORITY_HINT)
				opbx_add_hint(tmp);

			/* And immediately return success. */
			LOG;
			return 0;
		}			
			
		el = e;
		e = e->next;
	}
	/* If we fall all the way through to here, then we need to be on the end. */
	if (el)
		el->next = tmp;
	else
		con->root = tmp;
	opbx_mutex_unlock(&con->lock);
	if (tmp->priority == PRIORITY_HINT)
		opbx_add_hint(tmp);
	LOG;
	return 0;	
}

struct async_stat {
	pthread_t p;
	struct opbx_channel *chan;
	char context[OPBX_MAX_CONTEXT];
	char exten[OPBX_MAX_EXTENSION];
	int priority;
	int timeout;
	char app[OPBX_MAX_EXTENSION];
	char appdata[1024];
};

static void *async_wait(void *data) 
{
	struct async_stat *as = data;
	struct opbx_channel *chan = as->chan;
	int timeout = as->timeout;
	int res;
	struct opbx_frame *f;
	struct opbx_app *app;
	
	while(timeout && (chan->_state != OPBX_STATE_UP)) {
		res = opbx_waitfor(chan, timeout);
		if (res < 1) 
			break;
		if (timeout > -1)
			timeout = res;
		f = opbx_read(chan);
		if (!f)
			break;
		if (f->frametype == OPBX_FRAME_CONTROL) {
			if ((f->subclass == OPBX_CONTROL_BUSY)  ||
				(f->subclass == OPBX_CONTROL_CONGESTION) )
					break;
		}
		opbx_frfree(f);
	}
	if (chan->_state == OPBX_STATE_UP) {
		if (!opbx_strlen_zero(as->app)) {
			app = pbx_findapp(as->app);
			if (app) {
				if (option_verbose > 2)
					opbx_verbose(VERBOSE_PREFIX_3 "Launching %s(%s) on %s\n", as->app, as->appdata, chan->name);
				pbx_exec(chan, app, as->appdata, 1);
			} else
				opbx_log(LOG_WARNING, "No such application '%s'\n", as->app);
		} else {
			if (!opbx_strlen_zero(as->context))
				opbx_copy_string(chan->context, as->context, sizeof(chan->context));
			if (!opbx_strlen_zero(as->exten))
				opbx_copy_string(chan->exten, as->exten, sizeof(chan->exten));
			if (as->priority > 0)
				chan->priority = as->priority;
			/* Run the PBX */
			if (opbx_pbx_run(chan)) {
				opbx_log(LOG_ERROR, "Failed to start PBX on %s\n", chan->name);
			} else {
				/* PBX will have taken care of this */
				chan = NULL;
			}
		}
			
	}
	free(as);
	if (chan)
		opbx_hangup(chan);
	return NULL;
}

/*! Function to update the cdr after a spool call fails.
 *
 *  This function updates the cdr for a failed spool call
 *  and takes the channel of the failed call as an argument.
 *
 * \param chan the channel for the failed call.
 */
int opbx_pbx_outgoing_cdr_failed(void)
{
	/* allocate a channel */
	struct opbx_channel *chan = opbx_channel_alloc(0);
	if(!chan) {
		/* allocation of the channel failed, let some peeps know */
		opbx_log(LOG_WARNING, "Unable to allocate channel structure for CDR record\n");
		return -1;  /* failure */
	}

	chan->cdr = opbx_cdr_alloc();   /* allocate a cdr for the channel */

	if(!chan->cdr) {
		/* allocation of the cdr failed */
		opbx_log(LOG_WARNING, "Unable to create Call Detail Record\n");
		opbx_channel_free(chan);   /* free the channel */
		return -1;                /* return failure */
	}
	
	/* allocation of the cdr was successful */
	opbx_cdr_init(chan->cdr, chan);  /* initilize our channel's cdr */
	opbx_cdr_start(chan->cdr);       /* record the start and stop time */
	opbx_cdr_end(chan->cdr);
	opbx_cdr_failed(chan->cdr);      /* set the status to failed */
	opbx_cdr_detach(chan->cdr);      /* post and free the record */
	opbx_channel_free(chan);         /* free the channel */
	
	return 0;  /* success */
}

int opbx_pbx_outgoing_exten(const char *type, int format, void *data, int timeout, const char *context, const char *exten, int priority, int *reason, int sync, const char *cid_num, const char *cid_name, struct opbx_variable *vars, struct opbx_channel **channel)
{
	struct opbx_channel *chan;
	struct async_stat *as;
	int res = -1, cdr_res = -1;
	struct outgoing_helper oh;
	pthread_attr_t attr;

	if (sync) {
		LOAD_OH(oh);
		chan = __opbx_request_and_dial(type, format, data, timeout, reason, cid_num, cid_name, &oh);
		if (channel) {
			*channel = chan;
			if (chan)
				opbx_mutex_lock(&chan->lock);
		}
		if (chan) {
			if(chan->cdr) { /* check if the channel already has a cdr record, if not give it one */
				opbx_log(LOG_WARNING, "%s already has a call record??\n", chan->name);
			} else {
				chan->cdr = opbx_cdr_alloc();   /* allocate a cdr for the channel */
				if (!chan->cdr) {
					/* allocation of the cdr failed */
					opbx_log(LOG_WARNING, "Unable to create Call Detail Record\n");
					free(chan->pbx);
					res = -1;
					goto outgoing_exten_cleanup;
				}
				/* allocation of the cdr was successful */
				opbx_cdr_init(chan->cdr, chan);  /* initilize our channel's cdr */
				opbx_cdr_start(chan->cdr);
			}
			if (chan->_state == OPBX_STATE_UP) {
					res = 0;
				if (option_verbose > 3)
					opbx_verbose(VERBOSE_PREFIX_4 "Channel %s was answered.\n", chan->name);

				if (sync > 1) {
					if (channel)
						opbx_mutex_unlock(&chan->lock);
					if (opbx_pbx_run(chan)) {
						opbx_log(LOG_ERROR, "Unable to run PBX on %s\n", chan->name);
						if (channel)
							*channel = NULL;
						opbx_hangup(chan);
						res = -1;
					}
				} else {
					if (opbx_pbx_start(chan)) {
						opbx_log(LOG_ERROR, "Unable to start PBX on %s\n", chan->name);
						if (channel)
							*channel = NULL;
						opbx_hangup(chan);
						res = -1;
					} 
				}
			} else {
				if (option_verbose > 3)
					opbx_verbose(VERBOSE_PREFIX_4 "Channel %s was never answered.\n", chan->name);

				if(chan->cdr) { /* update the cdr */
					/* here we update the status of the call, which sould be busy.
					 * if that fails then we set the status to failed */
					if (opbx_cdr_disposition(chan->cdr, chan->hangupcause))
						opbx_cdr_failed(chan->cdr);
				}
			
				if (channel)
					*channel = NULL;
				opbx_hangup(chan);
			}
		}

		if(res < 0) { /* the call failed for some reason */
			if (*reason == 0) { /* if the call failed (not busy or no answer)
				            * update the cdr with the failed message */
				cdr_res = opbx_pbx_outgoing_cdr_failed();
				if (cdr_res != 0) {
					res = cdr_res;
					goto outgoing_exten_cleanup;
				}
			}
			
			/* create a fake channel and execute the "failed" extension (if it exists) within the requested context */
			/* check if "failed" exists */
			if (opbx_exists_extension(chan, context, "failed", 1, NULL)) {
				chan = opbx_channel_alloc(0);
				if (chan) {
					opbx_copy_string(chan->name, "OutgoingSpoolFailed", sizeof(chan->name));
					if (!opbx_strlen_zero(context))
						opbx_copy_string(chan->context, context, sizeof(chan->context));
					opbx_copy_string(chan->exten, "failed", sizeof(chan->exten));
					chan->priority = 1;
					opbx_set_variables(chan, vars);
					opbx_pbx_run(chan);	
				} else 
					opbx_log(LOG_WARNING, "Can't allocate the channel structure, skipping execution of extension 'failed'\n");
			}
		}
	} else {
		as = malloc(sizeof(struct async_stat));
		if (!as) {
			res = -1;
			goto outgoing_exten_cleanup;
		}	
		memset(as, 0, sizeof(struct async_stat));
		chan = opbx_request_and_dial(type, format, data, timeout, reason, cid_num, cid_name);
		if (channel) {
			*channel = chan;
			if (chan)
				opbx_mutex_lock(&chan->lock);
		}
		if (!chan) {
			free(as);
			res = -1;
			goto outgoing_exten_cleanup;
		}
		as->chan = chan;
		opbx_copy_string(as->context, context, sizeof(as->context));
		opbx_copy_string(as->exten,  exten, sizeof(as->exten));
		as->priority = priority;
		as->timeout = timeout;
		opbx_set_variables(chan, vars);
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		if (opbx_pthread_create(&as->p, &attr, async_wait, as)) {
			opbx_log(LOG_WARNING, "Failed to start async wait\n");
			free(as);
			if (channel)
				*channel = NULL;
			opbx_hangup(chan);
			res = -1;
			goto outgoing_exten_cleanup;
		}
		res = 0;
	}
outgoing_exten_cleanup:
	opbx_variables_destroy(vars);
	return res;
}

struct app_tmp {
	char app[256];
	char data[256];
	struct opbx_channel *chan;
	pthread_t t;
};

static void *opbx_pbx_run_app(void *data)
{
	struct app_tmp *tmp = data;
	struct opbx_app *app;
	app = pbx_findapp(tmp->app);
	if (app) {
		if (option_verbose > 3)
			opbx_verbose(VERBOSE_PREFIX_4 "Launching %s(%s) on %s\n", tmp->app, tmp->data, tmp->chan->name);
		pbx_exec(tmp->chan, app, tmp->data, 1);
	} else
		opbx_log(LOG_WARNING, "No such application '%s'\n", tmp->app);
	opbx_hangup(tmp->chan);
	free(tmp);
	return NULL;
}

int opbx_pbx_outgoing_app(const char *type, int format, void *data, int timeout, const char *app, const char *appdata, int *reason, int sync, const char *cid_num, const char *cid_name, struct opbx_variable *vars, struct opbx_channel **locked_channel)
{
	struct opbx_channel *chan;
	struct async_stat *as;
	struct app_tmp *tmp;
	int res = -1, cdr_res = -1;
	struct outgoing_helper oh;
	pthread_attr_t attr;
	
	memset(&oh, 0, sizeof(oh));
	oh.vars = vars;	

	if (locked_channel) 
		*locked_channel = NULL;
	if (opbx_strlen_zero(app)) {
		res = -1;
		goto outgoing_app_cleanup;	
	}
	if (sync) {
		chan = __opbx_request_and_dial(type, format, data, timeout, reason, cid_num, cid_name, &oh);
		if (chan) {
			if (chan->cdr) { /* check if the channel already has a cdr record, if not give it one */
				opbx_log(LOG_WARNING, "%s already has a call record??\n", chan->name);
			} else {
				chan->cdr = opbx_cdr_alloc();   /* allocate a cdr for the channel */
				if(!chan->cdr) {
					/* allocation of the cdr failed */
					opbx_log(LOG_WARNING, "Unable to create Call Detail Record\n");
					free(chan->pbx);
					res = -1;
					goto outgoing_app_cleanup;
				}
				/* allocation of the cdr was successful */
				opbx_cdr_init(chan->cdr, chan);  /* initilize our channel's cdr */
				opbx_cdr_start(chan->cdr);
			}
			opbx_set_variables(chan, vars);
			if (chan->_state == OPBX_STATE_UP) {
				res = 0;
				if (option_verbose > 3)
					opbx_verbose(VERBOSE_PREFIX_4 "Channel %s was answered.\n", chan->name);
				tmp = malloc(sizeof(struct app_tmp));
				if (tmp) {
					memset(tmp, 0, sizeof(struct app_tmp));
					opbx_copy_string(tmp->app, app, sizeof(tmp->app));
					if (appdata)
						opbx_copy_string(tmp->data, appdata, sizeof(tmp->data));
					tmp->chan = chan;
					if (sync > 1) {
						if (locked_channel)
							opbx_mutex_unlock(&chan->lock);
						opbx_pbx_run_app(tmp);
					} else {
						pthread_attr_init(&attr);
						pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
						if (locked_channel) 
							opbx_mutex_lock(&chan->lock);
						if (opbx_pthread_create(&tmp->t, &attr, opbx_pbx_run_app, tmp)) {
							opbx_log(LOG_WARNING, "Unable to spawn execute thread on %s: %s\n", chan->name, strerror(errno));
							free(tmp);
							if (locked_channel) 
								opbx_mutex_unlock(&chan->lock);
							opbx_hangup(chan);
							res = -1;
						} else {
							if (locked_channel) 
								*locked_channel = chan;
						}
					}
				} else {
					opbx_log(LOG_ERROR, "Out of memory :(\n");
					res = -1;
				}
			} else {
				if (option_verbose > 3)
					opbx_verbose(VERBOSE_PREFIX_4 "Channel %s was never answered.\n", chan->name);
				if (chan->cdr) { /* update the cdr */
					/* here we update the status of the call, which sould be busy.
					 * if that fails then we set the status to failed */
					if (opbx_cdr_disposition(chan->cdr, chan->hangupcause))
						opbx_cdr_failed(chan->cdr);
				}
				opbx_hangup(chan);
			}
		}
		
		if (res < 0) { /* the call failed for some reason */
			if (*reason == 0) { /* if the call failed (not busy or no answer)
				            * update the cdr with the failed message */
				cdr_res = opbx_pbx_outgoing_cdr_failed();
				if (cdr_res != 0) {
					res = cdr_res;
					goto outgoing_app_cleanup;
				}
			}
		}

	} else {
		as = malloc(sizeof(struct async_stat));
		if (!as) {
			res = -1;
			goto outgoing_app_cleanup;
		}
		memset(as, 0, sizeof(struct async_stat));
		chan = opbx_request_and_dial(type, format, data, timeout, reason, cid_num, cid_name);
		if (!chan) {
			free(as);
			res = -1;
			goto outgoing_app_cleanup;
		}
		as->chan = chan;
		opbx_copy_string(as->app, app, sizeof(as->app));
		if (appdata)
			opbx_copy_string(as->appdata,  appdata, sizeof(as->appdata));
		as->timeout = timeout;
		opbx_set_variables(chan, vars);
		/* Start a new thread, and get something handling this channel. */
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		if (locked_channel) 
			opbx_mutex_lock(&chan->lock);
		if (opbx_pthread_create(&as->p, &attr, async_wait, as)) {
			opbx_log(LOG_WARNING, "Failed to start async wait\n");
			free(as);
			if (locked_channel) 
				opbx_mutex_unlock(&chan->lock);
			opbx_hangup(chan);
			res = -1;
			goto outgoing_app_cleanup;
		} else {
			if (locked_channel)
				*locked_channel = chan;
		}
		res = 0;
	}
outgoing_app_cleanup:
	opbx_variables_destroy(vars);
	return res;
}

static void destroy_exten(struct opbx_exten *e)
{
	if (e->priority == PRIORITY_HINT)
		opbx_remove_hint(e);

	if (e->datad)
		e->datad(e->data);
	free(e);
}

void __opbx_context_destroy(struct opbx_context *con, const char *registrar)
{
	struct opbx_context *tmp, *tmpl=NULL;
	struct opbx_include *tmpi, *tmpil= NULL;
	struct opbx_sw *sw, *swl= NULL;
	struct opbx_exten *e, *el, *en;
	struct opbx_ignorepat *ipi, *ipl = NULL;

	opbx_mutex_lock(&conlock);
	tmp = contexts;
	while(tmp) {
		if (((con && (tmp->hash == con->hash)) || !con) &&
		    (!registrar || !strcasecmp(registrar, tmp->registrar))) {
			/* Okay, let's lock the structure to be sure nobody else
			   is searching through it. */
			if (opbx_mutex_lock(&tmp->lock)) {
				opbx_log(LOG_WARNING, "Unable to lock context lock\n");
				return;
			}
			if (tmpl)
				tmpl->next = tmp->next;
			else
				contexts = tmp->next;
			/* Okay, now we're safe to let it go -- in a sense, we were
			   ready to let it go as soon as we locked it. */
			opbx_mutex_unlock(&tmp->lock);
			for (tmpi = tmp->includes; tmpi; ) {
				/* Free includes */
				tmpil = tmpi;
				tmpi = tmpi->next;
				free(tmpil);
			}
			for (ipi = tmp->ignorepats; ipi; ) {
				/* Free ignorepats */
				ipl = ipi;
				ipi = ipi->next;
				free(ipl);
			}
			for (sw = tmp->alts; sw; ) {
				/* Free switches */
				swl = sw;
				sw = sw->next;
				free(swl);
				swl = sw;
			}
			for (e = tmp->root; e;) {
				for (en = e->peer; en;) {
					el = en;
					en = en->peer;
					destroy_exten(el);
				}
				el = e;
				e = e->next;
				destroy_exten(el);
			}
			opbx_mutex_destroy(&tmp->lock);
			free(tmp);
			if (!con) {
				/* Might need to get another one -- restart */
				tmp = contexts;
				tmpl = NULL;
				tmpil = NULL;
				continue;
			}
			opbx_mutex_unlock(&conlock);
			return;
		}
		tmpl = tmp;
		tmp = tmp->next;
	}
	opbx_mutex_unlock(&conlock);
}

void opbx_context_destroy(struct opbx_context *con, const char *registrar)
{
	__opbx_context_destroy(con,registrar);
}

static void wait_for_hangup(struct opbx_channel *chan, void *data)
{
	int res;
	struct opbx_frame *f;
	int waittime;
	
	if (!data || !strlen(data) || (sscanf(data, "%d", &waittime) != 1) || (waittime < 0))
		waittime = -1;
	if (waittime > -1) {
		opbx_safe_sleep(chan, waittime * 1000);
	} else do {
		res = opbx_waitfor(chan, -1);
		if (res < 0)
			return;
		f = opbx_read(chan);
		if (f)
			opbx_frfree(f);
	} while(f);
}

static int pbx_builtin_progress(struct opbx_channel *chan, void *data)
{
	opbx_indicate(chan, OPBX_CONTROL_PROGRESS);
	return 0;
}

static int pbx_builtin_ringing(struct opbx_channel *chan, void *data)
{
	opbx_indicate(chan, OPBX_CONTROL_RINGING);
	return 0;
}

static int pbx_builtin_busy(struct opbx_channel *chan, void *data)
{
	opbx_indicate(chan, OPBX_CONTROL_BUSY);		
	if (chan->_state != OPBX_STATE_UP)
		opbx_setstate(chan, OPBX_STATE_BUSY);
	wait_for_hangup(chan, data);
	return -1;
}

static int pbx_builtin_congestion(struct opbx_channel *chan, void *data)
{
	opbx_indicate(chan, OPBX_CONTROL_CONGESTION);
	if (chan->_state != OPBX_STATE_UP)
		opbx_setstate(chan, OPBX_STATE_BUSY);
	wait_for_hangup(chan, data);
	return -1;
}

static int pbx_builtin_answer(struct opbx_channel *chan, void *data)
{
	int delay = atoi(data);
	int res;
	if (chan->_state == OPBX_STATE_UP)
		delay = 0;
	res = opbx_answer(chan);
	if (res)
		return res;
	if (delay)
		res = opbx_safe_sleep(chan, delay);
	return res;
}

static int pbx_builtin_setlanguage(struct opbx_channel *chan, void *data)
{
	static int deprecation_warning = 0;

	if (!deprecation_warning) {
		opbx_log(LOG_WARNING, "SetLanguage is deprecated, please use Set(LANGUAGE()=language) instead.\n");
		deprecation_warning = 1;
	}

	/* Copy the language as specified */
	if (data)
		opbx_copy_string(chan->language, (char *) data, sizeof(chan->language));

	return 0;
}

static int pbx_builtin_resetcdr(struct opbx_channel *chan, void *data)
{
	int flags = 0;
	/* Reset the CDR as specified */
	if(data) {
		if(strchr((char *)data, 'w'))
			flags |= OPBX_CDR_FLAG_POSTED;
		if(strchr((char *)data, 'a'))
			flags |= OPBX_CDR_FLAG_LOCKED;
		if(strchr((char *)data, 'v'))
			flags |= OPBX_CDR_FLAG_KEEP_VARS;
	}

	opbx_cdr_reset(chan->cdr, flags);
	return 0;
}

static int pbx_builtin_setaccount(struct opbx_channel *chan, void *data)
{
	/* Copy the account code  as specified */
	if (data)
		opbx_cdr_setaccount(chan, (char *)data);
	else
		opbx_cdr_setaccount(chan, "");
	return 0;
}

static int pbx_builtin_setamaflags(struct opbx_channel *chan, void *data)
{
	/* Copy the AMA Flags as specified */
	if (data)
		opbx_cdr_setamaflags(chan, (char *)data);
	else
		opbx_cdr_setamaflags(chan, "");
	return 0;
}

static int pbx_builtin_hangup(struct opbx_channel *chan, void *data)
{
        if (data && atoi(data) > 0)
	       chan->hangupcause = atoi(data);
	/* Just return non-zero and it will hang up */
	return -1;
}

static int pbx_builtin_stripmsd(struct opbx_channel *chan, void *data)
{
	char newexten[OPBX_MAX_EXTENSION] = "";

	if (!data || !atoi(data)) {
		opbx_log(LOG_DEBUG, "Ignoring, since number of digits to strip is 0\n");
		return 0;
	}
	if (strlen(chan->exten) > atoi(data)) {
		opbx_copy_string(newexten, chan->exten + atoi(data), sizeof(newexten));
	}
	opbx_copy_string(chan->exten, newexten, sizeof(chan->exten));
	return 0;
}

static int pbx_builtin_prefix(struct opbx_channel *chan, void *data)
{
	char newexten[OPBX_MAX_EXTENSION];

	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_DEBUG, "Ignoring, since there is no prefix to add\n");
		return 0;
	}
	snprintf(newexten, sizeof(newexten), "%s%s", (char *)data, chan->exten);
	opbx_copy_string(chan->exten, newexten, sizeof(chan->exten));
	if (option_verbose > 2)
		opbx_verbose(VERBOSE_PREFIX_3 "Prepended prefix, new extension is %s\n", chan->exten);
	return 0;
}

static int pbx_builtin_suffix(struct opbx_channel *chan, void *data)
{
	char newexten[OPBX_MAX_EXTENSION];

	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_DEBUG, "Ignoring, since there is no suffix to add\n");
		return 0;
	}
	snprintf(newexten, sizeof(newexten), "%s%s", chan->exten, (char *)data);
	opbx_copy_string(chan->exten, newexten, sizeof(chan->exten));
	if (option_verbose > 2)
		opbx_verbose(VERBOSE_PREFIX_3 "Appended suffix, new extension is %s\n", chan->exten);
	return 0;
}

static int pbx_builtin_gotoiftime(struct opbx_channel *chan, void *data)
{
	int res=0;
	char *s, *ts;
	struct opbx_timing timing;

	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "GotoIfTime requires an argument:\n  <time range>|<days of week>|<days of month>|<months>?[[context|]extension|]priority\n");
		return -1;
	}

	if ((s = opbx_strdupa((char *) data))) {
		ts = s;

		/* Separate the Goto path */
		strsep(&ts,"?");

		/* struct opbx_include include contained garbage here, fixed by zeroing it on get_timerange */
		if (opbx_build_timing(&timing, s) && opbx_check_timing(&timing))
			res = pbx_builtin_goto(chan, (void *)ts);
	} else {
		opbx_log(LOG_ERROR, "Memory Error!\n");
	}
	return res;
}

static int pbx_builtin_execiftime(struct opbx_channel *chan, void *data)
{
	int res = 0;
	char *ptr1, *ptr2;
	struct opbx_timing timing;
	struct opbx_app *app;
	const char *usage = "ExecIfTime requires an argument:\n  <time range>|<days of week>|<days of month>|<months>?<appname>[|<appargs>]";

	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "%s\n", usage);	
		return -1;
	}

	ptr1 = opbx_strdupa(data);

	if (!ptr1) {
		opbx_log(LOG_ERROR, "Out of Memory!\n");
		return -1;	
	}

	ptr2 = ptr1;
	/* Separate the Application data ptr1 is the time spec ptr2 is the app|data */
	strsep(&ptr2,"?");
	if(!opbx_build_timing(&timing, ptr1)) {
		opbx_log(LOG_WARNING, "Invalid Time Spec: %s\nCorrect usage: %s\n", ptr1, usage);
		res = -1;
	}
		
	if (!res && opbx_check_timing(&timing)) {
		if (!ptr2) {
			opbx_log(LOG_WARNING, "%s\n", usage);
		}

		/* ptr2 is now the app name 
		   we're done with ptr1 now so recycle it and use it to point to the app args */
		if((ptr1 = strchr(ptr2, '|'))) {
			*ptr1 = '\0';
			ptr1++;
		}
		
		if ((app = pbx_findapp(ptr2))) {
			res = pbx_exec(chan, app, ptr1 ? ptr1 : "", 1);
		} else {
			opbx_log(LOG_WARNING, "Cannot locate application %s\n", ptr2);
			res = -1;
		}
	}
	
	return res;
}

static int pbx_builtin_wait(struct opbx_channel *chan, void *data)
{
	int ms;

	/* Wait for "n" seconds */
	if (data && atof((char *)data)) {
		ms = atof((char *)data) * 1000;
		return opbx_safe_sleep(chan, ms);
	}
	return 0;
}

static int pbx_builtin_waitexten(struct opbx_channel *chan, void *data)
{
	int ms, res, argc;
	char *args;
	char *argv[2];
	char *options = NULL; 
	char *mohclass = NULL;
	char *timeout = NULL;
	struct opbx_flags flags = {0};

	args = opbx_strdupa(data);

	if ((argc = opbx_separate_app_args(args, '|', argv, sizeof(argv) / sizeof(argv[0])))) {
		if (argc > 0) {
			timeout = argv[0];
			if (argc > 1)
				options = argv[1];
		}
	}

	if (options) {
		char *opts[1];
		opbx_parseoptions(waitexten_opts, &flags, opts, options);
		if (opbx_test_flag(&flags, WAITEXTEN_MOH)) {
			mohclass = opts[0];
		}
	}
	
	if (opbx_test_flag(&flags, WAITEXTEN_MOH))
		opbx_moh_start(chan, mohclass);

	/* Wait for "n" seconds */
	if (timeout && atof((char *)timeout)) 
		ms = atof((char *)timeout) * 1000;
	else if (chan->pbx)
		ms = chan->pbx->rtimeout * 1000;
	else
		ms = 10000;
	res = opbx_waitfordigit(chan, ms);
	if (!res) {
		if (opbx_exists_extension(chan, chan->context, chan->exten, chan->priority + 1, chan->cid.cid_num)) {
			if (option_verbose > 2)
				opbx_verbose(VERBOSE_PREFIX_3 "Timeout on %s, continuing...\n", chan->name);
		} else if (opbx_exists_extension(chan, chan->context, "t", 1, chan->cid.cid_num)) {
			if (option_verbose > 2)
				opbx_verbose(VERBOSE_PREFIX_3 "Timeout on %s, going to 't'\n", chan->name);
			opbx_copy_string(chan->exten, "t", sizeof(chan->exten));
			chan->priority = 0;
		} else {
			opbx_log(LOG_WARNING, "Timeout but no rule 't' in context '%s'\n", chan->context);
			res = -1;
		}
	}

	if (opbx_test_flag(&flags, WAITEXTEN_MOH))
		opbx_moh_stop(chan);

	return res;
}

static int pbx_builtin_background(struct opbx_channel *chan, void *data)
{
	int res = 0;
	int argc;
	char *parse;
	char *argv[4];
	char *options = NULL; 
	char *filename = NULL;
	char *front = NULL, *back = NULL;
	char *lang = NULL;
	char *context = NULL;
	struct opbx_flags flags = {0};
	unsigned int hash=0;

	parse = opbx_strdupa(data);

	if ((argc = opbx_separate_app_args(parse, '|', argv, sizeof(argv) / sizeof(argv[0])))) {
		switch (argc) {
		case 4:
			context = argv[3];
		case 3:
			lang = argv[2];
		case 2:
			options = argv[1];
			hash = opbx_hash_app_name(options);
		case 1:
			filename = argv[0];
			break;
		default:
			opbx_log(LOG_WARNING, "Background requires an argument (filename)\n");
			return -1;
		}
	}

	if (!lang)
		lang = chan->language;

	if (!context)
		context = chan->context;

	if (options) {
		if (hash == OPBX_KEYWORD_SKIP)
			flags.flags = BACKGROUND_SKIP;
		else if (hash == OPBX_KEYWORD_NOANSWER)
			flags.flags = BACKGROUND_NOANSWER;
		else
			opbx_parseoptions(background_opts, &flags, NULL, options);
	}

	/* Answer if need be */
	if (chan->_state != OPBX_STATE_UP) {
		if (opbx_test_flag(&flags, BACKGROUND_SKIP)) {
			return 0;
		} else if (!opbx_test_flag(&flags, BACKGROUND_NOANSWER)) {
			res = opbx_answer(chan);
		}
	}

	if (!res) {
		/* Stop anything playing */
		opbx_stopstream(chan);
		/* Stream a file */
		front = filename;
		while(!res && front) {
			if((back = strchr(front, '&'))) {
				*back = '\0';
				back++;
			}
			res = opbx_streamfile(chan, front, lang);
			if (!res) {
				if (opbx_test_flag(&flags, BACKGROUND_PLAYBACK)) {
					res = opbx_waitstream(chan, "");
				} else {
					if (opbx_test_flag(&flags, BACKGROUND_MATCHEXTEN)) {
						res = opbx_waitstream_exten(chan, context);
					} else {
						res = opbx_waitstream(chan, OPBX_DIGIT_ANY);
					}
				}
				opbx_stopstream(chan);
			} else {
				opbx_log(LOG_WARNING, "opbx_streamfile failed on %s for %s\n", chan->name, (char*)data);
				res = 0;
				break;
			}
			front = back;
		}
	}
	if (context != chan->context && res) {
		snprintf(chan->exten, sizeof(chan->exten), "%c", res);
		opbx_copy_string(chan->context, context, sizeof(chan->context));
		chan->priority = 0;
		return 0;
	} else {
		return res;
	}
}

static int pbx_builtin_atimeout(struct opbx_channel *chan, void *data)
{
	static int deprecation_warning = 0;
	int x = atoi((char *) data);

	if (!deprecation_warning) {
		opbx_log(LOG_WARNING, "AbsoluteTimeout is deprecated, please use Set(TIMEOUT(absolute)=timeout) instead.\n");
		deprecation_warning = 1;
	}
			
	/* Set the absolute maximum time how long a call can be connected */
	opbx_channel_setwhentohangup(chan,x);
	if (option_verbose > 2)
		opbx_verbose( VERBOSE_PREFIX_3 "Set Absolute Timeout to %d\n", x);
	return 0;
}

static int pbx_builtin_rtimeout(struct opbx_channel *chan, void *data)
{
	static int deprecation_warning = 0;

	if (!deprecation_warning) {
		opbx_log(LOG_WARNING, "ResponseTimeout is deprecated, please use Set(TIMEOUT(response)=timeout) instead.\n");
		deprecation_warning = 1;
	}

	/* If the channel is not in a PBX, return now */
	if (!chan->pbx)
		return 0;

	/* Set the timeout for how long to wait between digits */
	chan->pbx->rtimeout = atoi((char *)data);
	if (option_verbose > 2)
		opbx_verbose( VERBOSE_PREFIX_3 "Set Response Timeout to %d\n", chan->pbx->rtimeout);
	return 0;
}

static int pbx_builtin_dtimeout(struct opbx_channel *chan, void *data)
{
	static int deprecation_warning = 0;

	if (!deprecation_warning) {
		opbx_log(LOG_WARNING, "DigitTimeout is deprecated, please use Set(TIMEOUT(digit)=timeout) instead.\n");
		deprecation_warning = 1;
	}

	/* If the channel is not in a PBX, return now */
	if (!chan->pbx)
		return 0;

	/* Set the timeout for how long to wait between digits */
	chan->pbx->dtimeout = atoi((char *)data);
	if (option_verbose > 2)
		opbx_verbose( VERBOSE_PREFIX_3 "Set Digit Timeout to %d\n", chan->pbx->dtimeout);
	return 0;
}

static int pbx_builtin_goto(struct opbx_channel *chan, void *data)
{
	int res;
	res = opbx_parseable_goto(chan, (const char *) data);
	if (!res && (option_verbose > 2))
		opbx_verbose( VERBOSE_PREFIX_3 "Goto (%s,%s,%d)\n", chan->context,chan->exten, chan->priority+1);
	return res;
}


int pbx_builtin_serialize_variables(struct opbx_channel *chan, char *buf, size_t size) 
{
	struct opbx_var_t *variables;
	char *var, *val;
	int total = 0;

	if (!chan)
		return 0;

	memset(buf, 0, size);

	OPBX_LIST_TRAVERSE(&chan->varshead, variables, entries) {
//TODO: TO BE REMOVED LATER
//		if(variables &&
//		   (var=opbx_var_name(variables)) && (val=opbx_var_value(variables)) &&
//		   !opbx_strlen_zero(var) && !opbx_strlen_zero(val)) {
		if(
		   (var=opbx_var_name(variables)) && (val=opbx_var_value(variables))
		  ) {
			if (opbx_build_string(&buf, &size, "%s=%s\n", var, val)) {
				opbx_log(LOG_ERROR, "Data Buffer Size Exceeded!\n");
				break;
			} else
				total++;
		} else 
			break;
	}
	
	return total;
}

char *pbx_builtin_getvar_helper(struct opbx_channel *chan, const char *name) 
{
	struct opbx_var_t *variables;
	struct varshead *headp;
	unsigned int hash = opbx_hash_var_name(name);

	if (chan)
		headp = &chan->varshead;
	else
		headp = &globals;

	if (name) {
		OPBX_LIST_TRAVERSE(headp,variables,entries) {
			if (hash == opbx_var_hash(variables))
				return opbx_var_value(variables);
		}
		if (headp != &globals) {
			/* Check global variables if we haven't already */
			headp = &globals;
			OPBX_LIST_TRAVERSE(headp,variables,entries) {
				if (hash == opbx_var_hash(variables))
					return opbx_var_value(variables);
			}
		}
	}
	return NULL;
}

void pbx_builtin_pushvar_helper(struct opbx_channel *chan, const char *name, const char *value)
{
	struct opbx_var_t *newvariable;
	struct varshead *headp;

	if (name[strlen(name)-1] == ')') {
		opbx_log(LOG_WARNING, "Cannot push a value onto a function\n");
		return opbx_func_write(chan, name, value);
																        }

	headp = (chan) ? &chan->varshead : &globals;

	if (value) {
		if ((option_verbose > 1) && (headp == &globals))
			opbx_verbose(VERBOSE_PREFIX_2 "Setting global variable '%s' to '%s'\n", name, value);
			newvariable = opbx_var_assign(name, value);      
			OPBX_LIST_INSERT_HEAD(headp, newvariable, entries);
	}
}


void pbx_builtin_setvar_helper(struct opbx_channel *chan, const char *name, const char *value)
{
	struct opbx_var_t *newvariable;
	struct varshead *headp;
	const char *nametail = name;
	unsigned int hash;

	if (name[strlen(name)-1] == ')')
		return opbx_func_write(chan, name, value);

	headp = (chan) ? &chan->varshead : &globals;

	/* For comparison purposes, we have to strip leading underscores */
	if (*nametail == '_') {
		nametail++;
		if (*nametail == '_') 
			nametail++;
	}
	
	hash = opbx_hash_var_name(nametail);

	OPBX_LIST_TRAVERSE (headp, newvariable, entries) {
		if (hash == opbx_var_hash(newvariable)) {
			/* there is already such a variable, delete it */
			OPBX_LIST_REMOVE(headp, newvariable, entries);
			opbx_var_delete(newvariable);
			break;
		}
	} 

	if (value) {
		if ((option_verbose > 1) && (headp == &globals))
			opbx_verbose(VERBOSE_PREFIX_2 "Setting global variable '%s' to '%s'\n", name, value);
		newvariable = opbx_var_assign(name, value);	
		OPBX_LIST_INSERT_HEAD(headp, newvariable, entries);
	}
}

int pbx_builtin_setvar_old(struct opbx_channel *chan, void *data)
{
	static int deprecation_warning = 0;

	if (!deprecation_warning) {
		opbx_log(LOG_WARNING, "SetVar is deprecated, please use Set instead.\n");
		deprecation_warning = 1;
	}

	return pbx_builtin_setvar(chan, data);
}

int pbx_builtin_setvar(struct opbx_channel *chan, void *data)
{
	char *name, *value, *mydata;
	int argc;
	char *argv[24];		/* this will only support a maximum of 24 variables being set in a single operation */
	int global = 0;
	int x;

	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "Set requires at least one variable name/value pair.\n");
		return 0;
	}

	mydata = opbx_strdupa(data);
	argc = opbx_separate_app_args(mydata, '|', argv, sizeof(argv) / sizeof(argv[0]));

	/* check for a trailing flags argument */
	if ((argc > 1) && !strchr(argv[argc-1], '=')) {
		argc--;
		if (strchr(argv[argc], 'g'))
			global = 1;
	}

	for (x = 0; x < argc; x++) {
		name = argv[x];
		if ((value = strchr(name, '='))) {
			*value = '\0';
			value++;
			pbx_builtin_setvar_helper((global) ? NULL : chan, name, value);
		} else
			opbx_log(LOG_WARNING, "Ignoring entry '%s' with no = (and not last 'options' entry)\n", name);
	}

	return(0);
}

int pbx_builtin_importvar(struct opbx_channel *chan, void *data)
{
	char *name;
	char *value;
	char *stringp=NULL;
	char *channel;
	struct opbx_channel *chan2;
	char tmp[VAR_BUF_SIZE]="";
	char *s;

	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "Ignoring, since there is no variable to set\n");
		return 0;
	}

	stringp = opbx_strdupa(data);
	name = strsep(&stringp,"=");
	channel = strsep(&stringp,"|"); 
	value = strsep(&stringp,"\0");
	if (channel && value && name) {
		chan2 = opbx_get_channel_by_name_locked(channel);
		if (chan2) {
			s = alloca(strlen(value) + 4);
			if (s) {
				sprintf(s, "${%s}", value);
				pbx_substitute_variables_helper(chan2, s, tmp, sizeof(tmp) - 1);
			}
			opbx_mutex_unlock(&chan2->lock);
		}
		pbx_builtin_setvar_helper(chan, name, tmp);
	}

	return(0);
}

static int pbx_builtin_setglobalvar(struct opbx_channel *chan, void *data)
{
	char *name;
	char *value;
	char *stringp = NULL;

	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "Ignoring, since there is no variable to set\n");
		return 0;
	}

	stringp = data;
	name = strsep(&stringp, "=");
	value = strsep(&stringp, "\0"); 

	pbx_builtin_setvar_helper(NULL, name, value);

	return(0);
}

static int pbx_builtin_noop(struct opbx_channel *chan, void *data)
{
    // The following is added to relax dialplan execution.
    // When doing small loops with lots of iteration, this
    // allows other threads to re-schedule smoothly.
    // This will for sure dramatically slow down benchmarks but
    // will improve performance under load or in particular circumstances.
    // sched_yield(); // This doesn't seem to have the effect we want.
    usleep(1);

    return 0;
}


void pbx_builtin_clear_globals(void)
{
	struct opbx_var_t *vardata;
	while (!OPBX_LIST_EMPTY(&globals)) {
		vardata = OPBX_LIST_REMOVE_HEAD(&globals, entries);
		opbx_var_delete(vardata);
	}
}

static int pbx_checkcondition(char *condition) 
{
	if (condition) {
		if (*condition == '\0') {
			/* Empty strings are false */
			return 0;
		} else if (*condition >= '0' && *condition <= '9') {
			/* Numbers are evaluated for truth */
			return atoi(condition);
		} else {
			/* Strings are true */
			return 1;
		}
	} else {
		/* NULL is also false */
		return 0;
	}
}

static int pbx_builtin_gotoif(struct opbx_channel *chan, void *data)
{
	char *condition,*branch1,*branch2,*branch;
	char *s;
	int rc;
	char *stringp=NULL;

	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "Ignoring, since there is no variable to check\n");
		return 0;
	}
	
	s=opbx_strdupa(data);
	stringp=s;
	condition=strsep(&stringp,"?");
	branch1=strsep(&stringp,":");
	branch2=strsep(&stringp,"");
	branch = pbx_checkcondition(condition) ? branch1 : branch2;
	
	if (opbx_strlen_zero(branch)) {
		opbx_log(LOG_DEBUG, "Not taking any branch\n");
		return(0);
	}
	
	rc = pbx_builtin_goto(chan, branch);

	return(rc);
}           

static int pbx_builtin_saynumber(struct opbx_channel *chan, void *data)
{
	int res = 0;
	char tmp[256];
	char *number = (char *) NULL;
	char *options = (char *) NULL;

	
	if (opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "SayNumber requires an argument (number)\n");
		return -1;
	}
	opbx_copy_string(tmp, (char *) data, sizeof(tmp));
	number=tmp;
	strsep(&number, "|");
	options = strsep(&number, "|");
	if (options) { 
		if ( strcasecmp(options, "f") && strcasecmp(options,"m") && 
			strcasecmp(options, "c") && strcasecmp(options, "n") ) {
			opbx_log(LOG_WARNING, "SayNumber gender option is either 'f', 'm', 'c' or 'n'\n");
			return -1;
		}
	}
	return res = opbx_say_number(chan, atoi((char *) tmp), "", chan->language, options);
}

static int pbx_builtin_saydigits(struct opbx_channel *chan, void *data)
{
	int res = 0;

	if (data)
		res = opbx_say_digit_str(chan, (char *)data, "", chan->language);
	return res;
}
	
static int pbx_builtin_saycharacters(struct opbx_channel *chan, void *data)
{
	int res = 0;

	if (data)
		res = opbx_say_character_str(chan, (char *)data, "", chan->language);
	return res;
}
	
static int pbx_builtin_sayphonetic(struct opbx_channel *chan, void *data)
{
	int res = 0;

	if (data)
		res = opbx_say_phonetic_str(chan, (char *)data, "", chan->language);
	return res;
}
	
int load_pbx(void)
{
	int x;

	/* Initialize the PBX */
	if (option_verbose) {
		opbx_verbose( "OpenPBX Core Initializing\n");
		opbx_verbose( "Registering builtin applications:\n");
	}
	OPBX_LIST_HEAD_INIT_NOLOCK(&globals);
	opbx_cli_register_multiple(pbx_cli, sizeof(pbx_cli) / sizeof(pbx_cli[0]));

	/* Register builtin applications */
	for (x=0; x<sizeof(builtins) / sizeof(struct pbx_builtin); x++) {
		if (option_verbose)
			opbx_verbose( VERBOSE_PREFIX_1 "[%s]\n", builtins[x].name);
		if (opbx_register_application(builtins[x].name, builtins[x].execute, builtins[x].synopsis, builtins[x].description)) {
			opbx_log(LOG_ERROR, "Unable to register builtin application '%s'\n", builtins[x].name);
			return -1;
		}
	}
	return 0;
}

/*
 * Lock context list functions ...
 */
int opbx_lock_contexts()
{
	return opbx_mutex_lock(&conlock);
}

int opbx_unlock_contexts()
{
	return opbx_mutex_unlock(&conlock);
}

/*
 * Lock context ...
 */
int opbx_lock_context(struct opbx_context *con)
{
	return opbx_mutex_lock(&con->lock);
}

int opbx_unlock_context(struct opbx_context *con)
{
	return opbx_mutex_unlock(&con->lock);
}

/*
 * Name functions ...
 */
const char *opbx_get_context_name(struct opbx_context *con)
{
	return con ? con->name : NULL;
}

const char *opbx_get_extension_name(struct opbx_exten *exten)
{
	return exten ? exten->exten : NULL;
}

const char *opbx_get_extension_label(struct opbx_exten *exten)
{
	return exten ? exten->label : NULL;
}

const char *opbx_get_include_name(struct opbx_include *inc)
{
	return inc ? inc->name : NULL;
}

const char *opbx_get_ignorepat_name(struct opbx_ignorepat *ip)
{
	return ip ? ip->pattern : NULL;
}

int opbx_get_extension_priority(struct opbx_exten *exten)
{
	return exten ? exten->priority : -1;
}

/*
 * Registrar info functions ...
 */
const char *opbx_get_context_registrar(struct opbx_context *c)
{
	return c ? c->registrar : NULL;
}

const char *opbx_get_extension_registrar(struct opbx_exten *e)
{
	return e ? e->registrar : NULL;
}

const char *opbx_get_include_registrar(struct opbx_include *i)
{
	return i ? i->registrar : NULL;
}

const char *opbx_get_ignorepat_registrar(struct opbx_ignorepat *ip)
{
	return ip ? ip->registrar : NULL;
}

int opbx_get_extension_matchcid(struct opbx_exten *e)
{
	return e ? e->matchcid : 0;
}

const char *opbx_get_extension_cidmatch(struct opbx_exten *e)
{
	return e ? e->cidmatch : NULL;
}

const char *opbx_get_extension_app(struct opbx_exten *e)
{
	return e ? e->app : NULL;
}

void *opbx_get_extension_app_data(struct opbx_exten *e)
{
	return e ? e->data : NULL;
}

const char *opbx_get_switch_name(struct opbx_sw *sw)
{
	return sw ? sw->name : NULL;
}

const char *opbx_get_switch_data(struct opbx_sw *sw)
{
	return sw ? sw->data : NULL;
}

const char *opbx_get_switch_registrar(struct opbx_sw *sw)
{
	return sw ? sw->registrar : NULL;
}

/*
 * Walking functions ...
 */
struct opbx_context *opbx_walk_contexts(struct opbx_context *con)
{
	if (!con)
		return contexts;
	else
		return con->next;
}

struct opbx_exten *opbx_walk_context_extensions(struct opbx_context *con,
	struct opbx_exten *exten)
{
	if (!exten)
		return con ? con->root : NULL;
	else
		return exten->next;
}

struct opbx_sw *opbx_walk_context_switches(struct opbx_context *con,
	struct opbx_sw *sw)
{
	if (!sw)
		return con ? con->alts : NULL;
	else
		return sw->next;
}

struct opbx_exten *opbx_walk_extension_priorities(struct opbx_exten *exten,
	struct opbx_exten *priority)
{
	if (!priority)
		return exten;
	else
		return priority->peer;
}

struct opbx_include *opbx_walk_context_includes(struct opbx_context *con,
	struct opbx_include *inc)
{
	if (!inc)
		return con ? con->includes : NULL;
	else
		return inc->next;
}

struct opbx_ignorepat *opbx_walk_context_ignorepats(struct opbx_context *con,
	struct opbx_ignorepat *ip)
{
	if (!ip)
		return con ? con->ignorepats : NULL;
	else
		return ip->next;
}

int opbx_context_verify_includes(struct opbx_context *con)
{
	struct opbx_include *inc;
	int res = 0;

	for (inc = opbx_walk_context_includes(con, NULL); inc; inc = opbx_walk_context_includes(con, inc))
		if (!opbx_context_find(inc->rname)) {
			res = -1;
			opbx_log(LOG_WARNING, "Attempt to include nonexistent context '%s' in context '%s' (%#x)\n",
			    opbx_get_context_name(con), inc->rname, con->hash);
		}
	return res;
}


static int __opbx_goto_if_exists(struct opbx_channel *chan, char *context, char *exten, int priority, int async) 
{
	int (*goto_func)(struct opbx_channel *chan, const char *context, const char *exten, int priority);

	if (!chan)
		return -2;

	goto_func = (async) ? opbx_async_goto : opbx_explicit_goto;
	if (opbx_exists_extension(chan, context ? context : chan->context,
				 exten ? exten : chan->exten, priority,
				 chan->cid.cid_num))
		return goto_func(chan, context ? context : chan->context,
				 exten ? exten : chan->exten, priority);
	else 
		return -3;
}

int opbx_goto_if_exists(struct opbx_channel *chan, char* context, char *exten, int priority) {
	return __opbx_goto_if_exists(chan, context, exten, priority, 0);
}

int opbx_async_goto_if_exists(struct opbx_channel *chan, char* context, char *exten, int priority) {
	return __opbx_goto_if_exists(chan, context, exten, priority, 1);
}


// this function parses the arguments of a Goto(), it removes leading and
// trailing spaces for each argument and it ignores any arguments after the
// maximum number of arguments (GOTO_MAX_ARGS). If no arguments were passed
// to the Goto(), it will return -1 which will lead to a dialplan execution
// error.

#define GOTO_MAX_ARGS 3 /* maximum number of arguments for Goto() */

int opbx_parseable_goto(struct opbx_channel *chan, const char *goto_string) 
{
	int argc = 0;
	char *argv = NULL;
	char *arg[GOTO_MAX_ARGS];
	char *context = NULL, *exten = NULL, *prio = NULL;
	int ipri, mode = 0;
	unsigned int hash = 0;

	if (opbx_strlen_zero(goto_string)) {
		opbx_log(LOG_WARNING, "Goto requires an argument, usage is Goto([[context|]extension|]priority)\n");
		return -1;
	} // end if
	
	argv = opbx_strdupa(goto_string);
	
	// parse argument string and obtain all its arguments
	
	while ((*argv != 0) && (argc < GOTO_MAX_ARGS)) {
		
		// skip over any leading spaces
		while (*argv == 32)
			argv++;
		
		// remember start of this substring
		arg[argc] = argv;
		
		// find end of this substring
		while ((*argv != 32) && (*argv != '|') && (*argv != 0))
			argv++;
		
		if (*argv == '|') {
			// terminate this substring
			*argv = 0;
			// move past this delimiter
			argv++;
		}
		else if (*argv == 32) {
			// terminate this substring
			*argv = 0;
			// skip over any trailing spaces and following characters
			// until next delimiter or end of string has been reached
			while ((*argv != '|') && (*argv != 0))
				argv++;
			if (*argv == '|')
				argv++;
		} // end if
				
		// increase argument counter
		argc++;
		
	} // end while
	
	switch(argc) {
		case 1 :
			prio = arg[0];
			break;
		case 2 :
			exten = arg[0];
			prio = arg[1];
			hash = opbx_hash_app_name(exten);
			break;
		case 3 :
			context = arg[0];
			exten = arg[1];
			prio = arg[2];
			hash = opbx_hash_app_name(exten);
			break;
		default :
			opbx_log(LOG_WARNING, "invalid number of arguments in Goto statement\n");
			return -1;
	} // end switch
	
	if (*prio == '+') {
		mode = 1;
		prio++;
	}
	else if (*prio == '-') {
		mode = -1;
		prio++;
	} // end if
	
	if (sscanf(prio, "%d", &ipri) != 1) {
		if ((ipri = opbx_findlabel_extension(chan, context ? context : chan->context,
						(exten && (hash != OPBX_KEYWORD_BYEXTENSION)) ? exten : chan->exten,
						prio, chan->cid.cid_num)) < 1) {
			opbx_log(LOG_WARNING, "Priority '%s' must be a number > 0, or valid label\n", prio);
			return -1;
		}
		else
			mode = 0;
	} // end if
	
	/* At this point we have a priority and maybe an extension and a context */

	if (exten && (hash == OPBX_KEYWORD_BYEXTENSION))
		exten = NULL;

	if (mode) 
		ipri = chan->priority + (ipri * mode);

	opbx_explicit_goto(chan, context, exten, ipri);
	opbx_cdr_update(chan);

	return 0;

} // end opbx_parseable_goto


// END OF FILE
