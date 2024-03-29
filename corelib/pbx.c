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

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/corelib/pbx.c $", "$Revision: 4723 $")

#include "callweaver/lock.h"
#include "callweaver/cli.h"
#include "callweaver/pbx.h"
#include "callweaver/channel.h"
#include "callweaver/options.h"
#include "callweaver/logger.h"
#include "callweaver/file.h"
#include "callweaver/phone_no_utils.h"
#include "callweaver/cdr.h"
#include "callweaver/config.h"
#include "callweaver/term.h"
#include "callweaver/manager.h"
#include "callweaver/callweaver_expr.h"
#include "callweaver/linkedlists.h"
#include "callweaver/say.h"
#include "callweaver/utils.h"
#include "callweaver/causes.h"
#include "callweaver/musiconhold.h"
#include "callweaver/app.h"
#include "callweaver/devicestate.h"
#include "callweaver/callweaver_hash.h"
#include "callweaver/callweaver_keywords.h"

/*!
 * \note I M P O R T A N T :
 *
 *        The speed of extension handling will likely be among the most important
 * aspects of this PBX.  The switching scheme as it exists right now isn't
 * terribly bad (it's O(N+M), where N is the # of extensions and M is the avg #
 * of priorities, but a constant search time here would be great ;-) 
 *
 */

/*!
 * \note I M P O R T A N T  V . 2 :
 *
 *        This file has been converted towards a hash code based system to
 * recognise identifiers, which is precisely what the original author should
 * have done to address their concern stated in the above IMPORTANT note.
 *
 *        As a result of the change to the hash code based system, application
 * and variable names are no longer case insensitive. If the old behaviour is
 * desired, this file should be compiled with the following macros defined:
 *
 *        o  CW_USE_CASE_INSENSITIVE_APP_NAMES
 *        o  CW_USE_CASE_INSENSITIVE_VAR_NAMES
 *
 */

#ifdef CW_USE_CASE_INSENSITIVE_APP_NAMES
#define cw_hash_app_name(x)    cw_hash_string_toupper(x)
#define CW_CASE_INFO_STRING_FOR_APP_NAMES    "insensitive"
#else
#define cw_hash_app_name(x)    cw_hash_string(x)
#define CW_CASE_INFO_STRING_FOR_APP_NAMES    "sensitive"
#endif

#ifdef CW_USE_CASE_INSENSITIVE_VAR_NAMES
#define cw_hash_var_name(x)    cw_hash_string_toupper(x)
#define CW_CASE_INFO_STRING_FOR_VAR_NAMES    "insensitive"
#else
#define cw_hash_var_name(x)    cw_hash_string(x)
#define CW_CASE_INFO_STRING_FOR_VAR_NAMES    "sensitive"
#endif

#ifdef LOW_MEMORY
#define EXT_DATA_SIZE 256
#else
#define EXT_DATA_SIZE 8192
#endif

#define SWITCH_DATA_LENGTH 256

#define VAR_BUF_SIZE 4096

#define    VAR_NORMAL        1
#define    VAR_SOFTTRAN    2
#define    VAR_HARDTRAN    3

#define BACKGROUND_SKIP        (1 << 0)
#define BACKGROUND_NOANSWER    (1 << 1)
#define BACKGROUND_MATCHEXTEN    (1 << 2)
#define BACKGROUND_PLAYBACK    (1 << 3)

CW_DECLARE_OPTIONS(background_opts,{
    ['s'] = { BACKGROUND_SKIP },
    ['n'] = { BACKGROUND_NOANSWER },
    ['m'] = { BACKGROUND_MATCHEXTEN },
    ['p'] = { BACKGROUND_PLAYBACK },
});

#define WAITEXTEN_MOH        (1 << 0)

CW_DECLARE_OPTIONS(waitexten_opts,{
    ['m'] = { WAITEXTEN_MOH, 1 },
});

struct cw_context;

/* cw_exten: An extension */
struct cw_exten
{
    char *exten;                /* Extension name -- shouldn't this be called "ident" ? */
    unsigned int hash;            /* Hashed identifier */
    int matchcid;                /* Match caller id ? */
    char *cidmatch;                /* Caller id to match for this extension */
    int priority;                /* Priority */
    char *label;                /* Label */
    struct cw_context *parent;    /* The context this extension belongs to  */
    char *app;                    /* Application to execute */
    void *data;                    /* Data to use (arguments) */
    void (*datad)(void *);        /* Data destructor */
    struct cw_exten *peer;    /* Next higher priority with our extension */
    const char *registrar;        /* Registrar */
    struct cw_exten *next;    /* Extension with a greater ID */
    char stuff[0];
};

/* cw_include: include= support in extensions.conf */
struct cw_include
{
    char *name;        
    char *rname;                /* Context to include */
    const char *registrar;        /* Registrar */
    int hastime;                /* If time construct exists */
    struct cw_timing timing;    /* time construct */
    struct cw_include *next;    /* Link them together */
    char stuff[0];
};

/* cw_sw: Switch statement in extensions.conf */
struct cw_sw
{
    char *name;
    const char *registrar;        /* Registrar */
    char *data;                    /* Data load */
    int eval;
    struct cw_sw *next;        /* Link them together */
    char *tmpdata;
    char stuff[0];
};

struct cw_ignorepat
{
    const char *registrar;
    struct cw_ignorepat *next;
    char pattern[0];
};

/* cw_context: An extension context */
struct cw_context
{
    cw_mutex_t lock;             /* A lock to prevent multiple threads from clobbering the context */
    unsigned int hash;            /* Hashed context name */
    struct cw_exten *root;    /* The root of the list of extensions */
    struct cw_context *next;    /* Link them together */
    struct cw_include *includes;    /* Include other contexts */
    struct cw_ignorepat *ignorepats;    /* Patterns for which to continue playing dialtone */
    const char *registrar;        /* Registrar */
    struct cw_sw *alts;        /* Alternative switches */
    char name[0];                /* Name of the context */
};

/* cw_app: An application */
struct cw_app
{
    struct cw_app *next;        /* Next app in list */
    unsigned int hash;            /* Hashed application name */
    int (*execute)(struct cw_channel *chan, int argc, char **argv);
    const char *name;             /* Name of the application */
    const char *synopsis;         /* Synopsis text for 'show applications' */
    const char *syntax;           /* Syntax text for 'show applications' */
    const char *description;      /* Description (help text) for 'show application <name>' */
};

/* cw_func: A function */
struct cw_func {
	struct cw_func *next;
	unsigned int hash;
	char *(*read)(struct cw_channel *chan, int argc, char **argv, char *buf, size_t len);
	void (*write)(struct cw_channel *chan, int argc, char **argv, const char *value);
	const char *name;
	const char *synopsis;
	const char *syntax;
	const char *desc;
};

/* cw_state_cb: An extension state notify */
struct cw_state_cb
{
    int id;
    void *data;
    cw_state_cb_type callback;
    struct cw_state_cb *next;
};
        
/* Hints are pointers from an extension in the dialplan to one or more devices (tech/name) */
struct cw_hint
{
    struct cw_exten *exten;    /* Extension */
    int laststate;                /* Last known state */
    struct cw_state_cb *callbacks;    /* Callback list for this extension */
    struct cw_hint *next;        /* Pointer to next hint in list */
};

int cw_pbx_outgoing_cdr_failed(void);

static int pbx_builtin_prefix(struct cw_channel *, int, char **);
static int pbx_builtin_suffix(struct cw_channel *, int, char **);
static int pbx_builtin_stripmsd(struct cw_channel *, int, char **);
static int pbx_builtin_answer(struct cw_channel *, int, char **);
static int pbx_builtin_goto(struct cw_channel *, int, char **);
static int pbx_builtin_hangup(struct cw_channel *, int, char **);
static int pbx_builtin_background(struct cw_channel *, int, char **);
static int pbx_builtin_dtimeout(struct cw_channel *, int, char **);
static int pbx_builtin_rtimeout(struct cw_channel *, int, char **);
static int pbx_builtin_atimeout(struct cw_channel *, int, char **);
static int pbx_builtin_wait(struct cw_channel *, int, char **);
static int pbx_builtin_waitexten(struct cw_channel *, int, char **);
static int pbx_builtin_setlanguage(struct cw_channel *, int, char **);
static int pbx_builtin_resetcdr(struct cw_channel *, int, char **);
static int pbx_builtin_setaccount(struct cw_channel *, int, char **);
static int pbx_builtin_setamaflags(struct cw_channel *, int, char **);
static int pbx_builtin_ringing(struct cw_channel *, int, char **);
static int pbx_builtin_progress(struct cw_channel *, int, char **);
static int pbx_builtin_congestion(struct cw_channel *, int, char **);
static int pbx_builtin_busy(struct cw_channel *, int, char **);
static int pbx_builtin_setglobalvar(struct cw_channel *, int, char **);
static int pbx_builtin_noop(struct cw_channel *, int, char **);
static int pbx_builtin_gotoif(struct cw_channel *, int, char **);
static int pbx_builtin_gotoiftime(struct cw_channel *, int, char **);
static int pbx_builtin_execiftime(struct cw_channel *, int, char **);
static int pbx_builtin_saynumber(struct cw_channel *, int, char **);
static int pbx_builtin_saydigits(struct cw_channel *, int, char **);
static int pbx_builtin_saycharacters(struct cw_channel *, int, char **);
static int pbx_builtin_sayphonetic(struct cw_channel *, int, char **);
static int pbx_builtin_setvar_old(struct cw_channel *, int, char **);
static int pbx_builtin_setvar(struct cw_channel *, int, char **);
static int pbx_builtin_importvar(struct cw_channel *, int, char **);

CW_MUTEX_DEFINE_STATIC(globalslock);
static struct varshead globals;

static int autofallthrough = 0;

CW_MUTEX_DEFINE_STATIC(maxcalllock);
static int countcalls = 0;

CW_MUTEX_DEFINE_STATIC(funcs_lock);         /* Lock for the custom function list */
static struct cw_func *funcs_head = NULL;

static struct pbx_builtin {
    char *name;
    int (*execute)(struct cw_channel *chan, int argc, char **argv);
    char *synopsis;
    char *syntax;
    char *description;
} builtins[] = 
{
    /* These applications are built into the PBX core and do not
       need separate modules */

    { "AbsoluteTimeout", pbx_builtin_atimeout,
    "Set absolute maximum time of call",
    "AbsoluteTimeout(seconds)",
    "Set the absolute maximum amount of time permitted for a call.\n"
    "A setting of 0 disables the timeout.  Always returns 0.\n" 
    "AbsoluteTimeout has been deprecated in favor of Set(TIMEOUT(absolute)=timeout)\n"
    },

    { "Answer", pbx_builtin_answer, 
    "Answer a channel if ringing", 
    "Answer([delay])",
    "If the channel is ringing, answer it, otherwise do nothing. \n"
    "If delay is specified, callweaver will pause execution for the specified amount\n"
    "of milliseconds if an answer is required, in order to give audio a chance to\n"
    "become ready. Returns 0 unless it tries to answer the channel and fails.\n"   
    },

    { "Background", pbx_builtin_background,
    "Play a file while awaiting extension",
    "Background(filename1[&filename2...][, options[, langoverride][, context]])",
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
    "         extension in the destination context\n"
    },

    { "Busy", pbx_builtin_busy,
    "Indicate busy condition and stop",
    "Busy([timeout])",
    "Requests that the channel indicate busy condition and then waits\n"
    "for the user to hang up or the optional timeout to expire.\n"
    "Always returns -1." 
    },

    { "Congestion", pbx_builtin_congestion,
    "Indicate congestion and stop",
    "Congestion([timeout])",
    "Requests that the channel indicate congestion and then waits for\n"
    "the user to hang up or for the optional timeout to expire.\n"
    "Always returns -1." 
    },

    { "DigitTimeout", pbx_builtin_dtimeout,
    "Set maximum timeout between digits",
    "DigitTimeout(seconds)",
    "Set the maximum amount of time permitted between digits when the\n"
    "user is typing in an extension. When this timeout expires,\n"
    "after the user has started to type in an extension, the extension will be\n"
    "considered complete, and will be interpreted. Note that if an extension\n"
    "typed in is valid, it will not have to timeout to be tested, so typically\n"
    "at the expiry of this timeout, the extension will be considered invalid\n"
    "(and thus control would be passed to the 'i' extension, or if it doesn't\n"
    "exist the call would be terminated). The default timeout is 5 seconds.\n"
    "Always returns 0.\n" 
    "DigitTimeout has been deprecated in favor of Set(TIMEOUT(digit)=timeout)\n"
    },

    { "ExecIfTime", pbx_builtin_execiftime,
    "Conditional application execution on current time",
    "ExecIfTime(times, weekdays, mdays, months ? appname[, arg, ...])",
    "If the current time matches the specified time, then execute the specified\n"
    "application. Each of the elements may be specified either as '*' (for always)\n"
    "or as a range. See the 'include' syntax for details. It will return whatever\n"
    "<appname> returns, or a non-zero value if the application is not found.\n"
    },

    { "Goto", pbx_builtin_goto, 
    "Goto a particular priority, extension, or context",
    "Goto([[context, ]extension, ]priority)",
    "Set the  priority to the specified\n"
    "value, optionally setting the extension and optionally the context as well.\n"
    "The extension BYEXTENSION is special in that it uses the current extension,\n"
    "thus  permitting you to go to a different context, without specifying a\n"
    "specific extension. Always returns 0, even if the given context, extension,\n"
    "or priority is invalid.\n" 
    },

    { "GotoIf", pbx_builtin_gotoif,
    "Conditional goto",
    "GotoIf(condition ? [context, [exten, ]]priority|label [: [context, [exten, ]]priority|label])",
    "Go to label 1 if condition is\n"
    "true, to label2 if condition is false. Either label1 or label2 may be\n"
    "omitted (in that case, we just don't take the particular branch) but not\n"
    "both. Look for the condition syntax in examples or documentation." 
    },

    { "GotoIfTime", pbx_builtin_gotoiftime,
    "Conditional goto on current time",
    "GotoIfTime(times, weekdays, mdays, months ? [[context, ]extension, ]priority|label)",
    "If the current time matches the specified time, then branch to the specified\n"
    "extension. Each of the elements may be specified either as '*' (for always)\n"
    "or as a range. See the 'include' syntax for details." 
    },

    { "Hangup", pbx_builtin_hangup,
    "Unconditional hangup",
    "Hangup()",
    "Unconditionally hangs up a given channel by returning -1 always.\n" 
    },

    { "ImportVar", pbx_builtin_importvar,
    "Import a variable from a channel into a new variable",
    "ImportVar(newvar=channelname, variable)",
    "This application imports a\n"
    "variable from the specified channel (as opposed to the current one)\n"
    "and stores it as a variable in the current channel (the channel that\n"
    "is calling this application). If the new variable name is prefixed by\n"
    "a single underscore \"_\", then it will be inherited into any channels\n"
    "created from this one. If it is prefixed with two underscores,then\n"
    "the variable will have infinite inheritance, meaning that it will be\n"
    "present in any descendent channel of this one.\n"
    },

    { "NoOp", pbx_builtin_noop,
    "No operation",
    "NoOp()",
    "No-operation; Does nothing except relaxing the dialplan and \n"
    "re-scheduling over threads. It's necessary and very useful in tight loops." 
    },

    { "Prefix", pbx_builtin_prefix, 
    "Prepend leading digits",
    "Prefix(digits)",
    "Prepends the digit string specified by digits to the\n"
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
    "Progress()",
    "Request that the channel indicate in-band progress is \n"
    "available to the user.\nAlways returns 0.\n" 
    },

    { "ResetCDR", pbx_builtin_resetcdr,
    "Resets the Call Data Record",
    "ResetCDR([options])",
    "Causes the Call Data Record to be reset, optionally\n"
    "storing the current CDR before zeroing it out\b"
    " - if 'w' option is specified record will be stored.\n"
    " - if 'a' option is specified any stacked records will be stored.\n"
    " - if 'v' option is specified any variables will be saved.\n"
    "Always returns 0.\n"  
    },

    { "ResponseTimeout", pbx_builtin_rtimeout,
    "Set maximum timeout awaiting response",
    "ResponseTimeout(seconds)",
    "Set the maximum amount of time permitted after\n"
    "falling through a series of priorities for a channel in which the user may\n"
    "begin typing an extension. If the user does not type an extension in this\n"
    "amount of time, control will pass to the 't' extension if it exists, and\n"
    "if not the call would be terminated. The default timeout is 10 seconds.\n"
    "Always returns 0.\n"  
    "ResponseTimeout has been deprecated in favor of Set(TIMEOUT(response)=timeout)\n"
    },

    { "Ringing", pbx_builtin_ringing,
    "Indicate ringing tone",
    "Ringing()",
    "Request that the channel indicate ringing tone to the user.\n"
    "Always returns 0.\n" 
    },

    { "SayAlpha", pbx_builtin_saycharacters,
    "Say Alpha",
    "SayAlpha(string)",
    "Spells the passed string\n" 
    },

    { "SayDigits", pbx_builtin_saydigits,
    "Say Digits",
    "SayDigits(digits)",
    "Says the passed digits. SayDigits is using the\n" 
    "current language setting for the channel. (See app setLanguage)\n"
    },

    { "SayNumber", pbx_builtin_saynumber,
    "Say Number",
    "SayNumber(digits[, gender])",
    "Says the passed number. SayNumber is using\n" 
    "the current language setting for the channel. (See app SetLanguage).\n"
    },

    { "SayPhonetic", pbx_builtin_sayphonetic,
    "Say Phonetic",
    "SayPhonetic(string)",
    "Spells the passed string with phonetic alphabet\n" 
    },

    { "Set", pbx_builtin_setvar,
      "Set channel variable(s) or function value(s)",
      "Set(name1=value1, name2=value2, ...[, options])",
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

    { "SetAccount", pbx_builtin_setaccount,
    "Sets account code",
    "SetAccount([account])",
    "Set the channel account code for billing\n"
    "purposes. Always returns 0.\n"
    },

    { "SetAMAFlags", pbx_builtin_setamaflags,
    "Sets AMA Flags",
    "SetAMAFlags([flag])",
    "Set the channel AMA Flags for billing\n"
    "purposes. Always returns 0.\n"
    },

    { "SetGlobalVar", pbx_builtin_setglobalvar,
    "Set global variable to value",
    "SetGlobalVar(#n=value)",
    "Sets global variable n to value. Global\n" 
    "variable are available across channels.\n"
    },

    { "SetLanguage", pbx_builtin_setlanguage,
    "Sets channel language",
    "SetLanguage(language)",
    "Set the channel language to 'language'. This\n"
    "information is used for the syntax in generation of numbers, and to choose\n"
    "a natural language file when available.\n"
    "  For example, if language is set to 'fr' and the file 'demo-congrats' is \n"
    "requested to be played, if the file 'fr/demo-congrats' exists, then\n"
    "it will play that file, and if not will play the normal 'demo-congrats'.\n"
    "For some language codes, SetLanguage also changes the syntax of some\n"
    "CallWeaver functions, like SayNumber.\n"
    "Always returns 0.\n"
    "SetLanguage has been deprecated in favor of Set(LANGUAGE()=language)\n"
    },

    { "SetVar", pbx_builtin_setvar_old,
      "Set channel variable(s)",
      "SetVar(name1=value1, name2=value2, ...[, options])",
      "SetVar has been deprecated in favor of Set.\n"
    },

    { "StripMSD", pbx_builtin_stripmsd,
    "Strip leading digits",
    "StripMSD(count)",
    "Strips the leading 'count' digits from the channel's\n"
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
    "Suffix(digits)",
    "Appends the digit string specified by digits to the\n"
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
    "Wait(seconds)",
    "Waits for a specified number of seconds, then returns 0.\n"
    "seconds can be passed with fractions of a second. (eg: 1.5 = 1.5 seconds)\n" 
    },

    { "WaitExten", pbx_builtin_waitexten, 
    "Waits for an extension to be entered", 
    "WaitExten([seconds][, options])",
    "Waits for the user to enter a new extension for the \n"
    "specified number of seconds, then returns 0. Seconds can be passed with\n"
    "fractions of a seconds (eg: 1.5 = 1.5 seconds) or if unspecified the\n"
    "default extension timeout will be used.\n"
    "  Options:\n"
    "    'm[(x)]' - Provide music on hold to the caller while waiting for an extension.\n"
    "               Optionally, specify the class for music on hold within parenthesis.\n"
    },
};


static struct cw_context *contexts = NULL;
CW_MUTEX_DEFINE_STATIC(conlock);         /* Lock for the cw_context list */
static struct cw_app *apps_head = NULL;
CW_MUTEX_DEFINE_STATIC(apps_lock);         /* Lock for the application list */

struct cw_switch *switches = NULL;
CW_MUTEX_DEFINE_STATIC(switchlock);        /* Lock for switches */

CW_MUTEX_DEFINE_STATIC(hintlock);        /* Lock for extension state notifys */
static int stateid = 1;
struct cw_hint *hints = NULL;
struct cw_state_cb *statecbs = NULL;

int pbx_exec_argv(struct cw_channel *c, struct cw_app *app, int argc, char **argv)
{
	const char *saved_c_appl;
	int res;

	/* save channel values - for the sake of debug output from DumpChan and the CLI <bleurgh> */
	saved_c_appl= c->appl;
	c->appl = app->name;

	res = (*app->execute)(c, argc, argv);

	/* restore channel values */
	c->appl= saved_c_appl;

	return res;
}

int pbx_exec(struct cw_channel *c, struct cw_app *app, void *data)
{
	char *argv[100]; /* No app can take more than 100 args unless it parses them itself */
	const char *saved_c_appl;
	int res;
    
	if (c->cdr && !cw_check_hangup(c))
		cw_cdr_setapp(c->cdr, app->name, data);

	/* save channel values - for the sake of debug output from DumpChan and the CLI <bleurgh> */
	saved_c_appl= c->appl;
	c->appl = app->name;

	res = (*app->execute)(c, cw_separate_app_args(data, ',', arraysize(argv), argv), argv);

	/* restore channel values */
	c->appl= saved_c_appl;

	return res;
}


/* Go no deeper than this through includes (not counting loops) */
#define CW_PBX_MAX_STACK    128

#define HELPER_EXISTS 0
#define HELPER_EXEC 1
#define HELPER_CANMATCH 2
#define HELPER_MATCHMORE 3
#define HELPER_FINDLABEL 4

struct cw_app *pbx_findapp(const char *app) 
{
	struct cw_app *tmp;
	unsigned int hash = cw_hash_app_name(app);

	if (cw_mutex_lock(&apps_lock)) {
		cw_log(LOG_WARNING, "Unable to obtain application lock\n");
		return NULL;
	}

	for (tmp = apps_head; tmp && hash != tmp->hash; tmp = tmp->next);

	cw_mutex_unlock(&apps_lock);
	return tmp;
}

static struct cw_switch *pbx_findswitch(const char *sw)
{
    struct cw_switch *asw;

    if (cw_mutex_lock(&switchlock))
    {
        cw_log(LOG_WARNING, "Unable to obtain switch lock\n");
        return NULL;
    }
    asw = switches;
    while (asw)
    {
        if (!strcasecmp(asw->name, sw))
            break;
        asw = asw->next;
    }
    cw_mutex_unlock(&switchlock);
    return asw;
}

static inline int include_valid(struct cw_include *i)
{
    if (!i->hastime)
        return 1;

    return cw_check_timing(&(i->timing));
}

static void pbx_destroy(struct cw_pbx *p)
{
    free(p);
}

const char *cw_extension_match_to_str(int match)
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

int cw_extension_pattern_match(const char *destination, const char *pattern)
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
                cw_log(LOG_WARNING, "Bad usage of [] in extension pattern '%s'", pattern);
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

static int cw_extension_match(const char *pattern, const char *data)
{
    int match;

    match = cw_extension_pattern_match(data, pattern);
    if (match == EXTENSION_MATCH_POSSIBLE)
        return 2;
    return (match == EXTENSION_MATCH_EXACT  ||  match == EXTENSION_MATCH_STRETCHABLE)  ?  1  :  0;
}

struct cw_context *cw_context_find(const char *name)
{
    struct cw_context *tmp;
    unsigned int hash = cw_hash_string(name);
    
    cw_mutex_lock(&conlock);
    if (name)
    {
        tmp = contexts;
        while (tmp)
        {
            if (hash == tmp->hash)
                break;
            tmp = tmp->next;
        }
    }
    else
    {
        tmp = contexts;
    }
    cw_mutex_unlock(&conlock);
    return tmp;
}

#define STATUS_NO_CONTEXT    1
#define STATUS_NO_EXTENSION    2
#define STATUS_NO_PRIORITY    3
#define STATUS_NO_LABEL        4
#define STATUS_SUCCESS        5

static int matchcid(const char *cidpattern, const char *callerid)
{
    /* If the Caller*ID pattern is empty, then we're matching NO Caller*ID, so
       failing to get a number should count as a match, otherwise not */

    if (callerid == NULL)
        return (cidpattern[0])  ?  0  :  1;

    switch (cw_extension_pattern_match(callerid, cidpattern))
    {
    case EXTENSION_MATCH_EXACT:
    case EXTENSION_MATCH_STRETCHABLE:
    case EXTENSION_MATCH_POSSIBLE:
        return 1;
    }
    return 0;
}

static struct cw_exten *pbx_find_extension(struct cw_channel *chan, struct cw_context *bypass, const char *context, const char *exten, int priority, const char *label, const char *callerid, int action, char *incstack[], int *stacklen, int *status, struct cw_switch **swo, char **data, const char **foundcontext)
{
    int x, res;
    struct cw_context *tmp;
    struct cw_exten *e, *eroot;
    struct cw_include *i;
    struct cw_sw *sw;
    struct cw_switch *asw;
    unsigned int hash = cw_hash_string(context);

    /* Initialize status if appropriate */
    if (!*stacklen)
    {
        *status = STATUS_NO_CONTEXT;
        *swo = NULL;
        *data = NULL;
    }
    /* Check for stack overflow */
    if (*stacklen >= CW_PBX_MAX_STACK)
    {
        cw_log(LOG_WARNING, "Maximum PBX stack exceeded\n");
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
            struct cw_exten *earlymatch = NULL;

            if (*status < STATUS_NO_EXTENSION)
                *status = STATUS_NO_EXTENSION;
            for (eroot = tmp->root;  eroot;  eroot = eroot->next)
            {
                int match = 0;
                int res = 0;

                /* Match extension */
                match = cw_extension_pattern_match(exten, eroot->exten);
                res = 0;
		if (!(eroot->matchcid  &&  !matchcid(eroot->cidmatch, callerid)))
		{
                    switch (action)
                    {
                        case HELPER_EXISTS:
                        case HELPER_EXEC:
                        case HELPER_FINDLABEL:
                    	    /* We are only interested in exact matches */
                    	    res = (match == EXTENSION_MATCH_POSSIBLE  ||  match == EXTENSION_MATCH_EXACT  ||  match == EXTENSION_MATCH_STRETCHABLE);
                    	    break;
                        case HELPER_CANMATCH:
                            /* We are interested in exact or incomplete matches */
                            res = (match == EXTENSION_MATCH_POSSIBLE  ||  match == EXTENSION_MATCH_EXACT  ||  match == EXTENSION_MATCH_STRETCHABLE  ||  match == EXTENSION_MATCH_INCOMPLETE);
                    	    break;
                        case HELPER_MATCHMORE:
                    	    /* We are only interested in incomplete matches */
                    	    if (match == EXTENSION_MATCH_POSSIBLE  &&  earlymatch == NULL) 
			    {
                               /* It matched an extension ending in a '!' wildcard
                               So just record it for now, unless there's a better match */
                               earlymatch = eroot;
                               res = 0;
                               break;
                            }
                            res = (match == EXTENSION_MATCH_STRETCHABLE  ||  match == EXTENSION_MATCH_INCOMPLETE)  ?  1  :  0;
                            break;
                    }
		}
                if (res)
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
                             if (label  &&  e->label  &&  !strcmp(label, e->label))
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
                        pbx_substitute_variables_helper(chan, sw->data, sw->tmpdata, SWITCH_DATA_LENGTH);
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
                    cw_log(LOG_WARNING, "No such switch '%s'\n", sw->name);
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

/*! \brief  pbx_retrieve_variable: Support for CallWeaver built-in variables and
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

void pbx_retrieve_variable(struct cw_channel *c, const char *var, char **ret, char *workspace, int workspacelen, struct varshead *headp)
{
    char *first, *second;
    char tmpvar[80];
    time_t thistime;
    struct tm brokentime;
    int offset, offset2;
    struct cw_var_t *variables;
    int no_match_yet = 0; // start optimistic
    unsigned int hash = cw_hash_var_name(var);

    // warnings for (potentially) unsafe pre-conditions
    // TODO: these cases really ought to be safeguarded against
        
    if (ret == NULL)
        cw_log(LOG_WARNING, "NULL passed in parameter 'ret' in function 'pbx_retrieve_variable'\n");

    if (workspace == NULL)
        cw_log(LOG_WARNING, "NULL passed in parameter 'workspace' in function 'pbx_retrieve_variable'\n");
    
    if (workspacelen == 0)
        cw_log(LOG_WARNING, "Zero passed in parameter 'workspacelen' in function 'pbx_retrieve_variable'\n");

    if (workspacelen > VAR_BUF_SIZE)
        cw_log(LOG_WARNING, "VAR_BUF_SIZE exceeded by parameter 'workspacelen' in function 'pbx_retrieve_variable'\n");

    // actual work starts here
    
    if /* channel exists */ (c) 
        headp = &c->varshead;
    
    *ret = NULL;
    
    // check for slicing modifier
    if /* sliced */ ((first=strchr(var,':')))
    {
        // remove characters counting from end or start of string */
        cw_copy_string(tmpvar, var, sizeof(tmpvar));
        first = strchr(tmpvar, ':');
        if (!first)
            first = tmpvar + strlen(tmpvar);
        *first='\0';
        pbx_retrieve_variable(c,tmpvar,ret,workspace,workspacelen - 1, headp);
        if (!(*ret)) 
            return;
        offset = atoi(first + 1);    /* The number of characters, 
                       positive: remove # of chars from start
                       negative: keep # of chars from end */
                        
         if ((second = strchr(first + 1, ':')))
        {    
            *second='\0';
            offset2 = atoi(second+1);        /* Number of chars to copy */
        }
        else if (offset >= 0)
        {
            offset2 = strlen(*ret)-offset;    /* Rest of string */
        }
        else
        {
            offset2 = abs(offset);
        }

        if (abs(offset) > strlen(*ret))
        {
            /* Offset beyond string */
            if (offset >= 0) 
                offset = strlen(*ret);
            else 
                offset =- strlen(*ret);
        }

        if ((offset < 0  &&  offset2 > -offset)  ||  (offset >= 0  &&  offset + offset2 > strlen(*ret)))
        {
            if (offset >= 0) 
                offset2 = strlen(*ret) - offset;
            else 
                offset2 = strlen(*ret) + offset;
        }
        if (offset >= 0)
            *ret += offset;
        else
            *ret += strlen(*ret)+offset;
        (*ret)[offset2] = '\0';        /* Cut at offset2 position */
    }
    else /* not sliced */
    {
        if /* channel exists */ (c)
        {
            // ----------------------------------------------
            // search builtin channel variables (scenario #1)
            // ----------------------------------------------
                        
            if /* CALLERID */(hash == CW_KEYWORD_CALLERID)
            {
                if (c->cid.cid_num)
                {
                    if (c->cid.cid_name)
                        snprintf(workspace, workspacelen, "\"%s\" <%s>", c->cid.cid_name, c->cid.cid_num);
                    else
                        cw_copy_string(workspace, c->cid.cid_num, workspacelen);
                    *ret = workspace;
                }
                else if (c->cid.cid_name)
                {
                    cw_copy_string(workspace, c->cid.cid_name, workspacelen);
                    *ret = workspace;
                }
                else
                {
                    *ret = NULL;
                }
            }
            else if /* CALLERIDNUM */ (hash == CW_KEYWORD_CALLERIDNUM)
            {
                if (c->cid.cid_num)
                {
                    cw_copy_string(workspace, c->cid.cid_num, workspacelen);
                    *ret = workspace;
                }
                else
                {
                    *ret = NULL;
                }
            }
            else if /* CALLERIDNAME */ (hash == CW_KEYWORD_CALLERIDNAME)
            {
                if (c->cid.cid_name)
                {
                    cw_copy_string(workspace, c->cid.cid_name, workspacelen);
                    *ret = workspace;
                }
                else
                    *ret = NULL;
            }
            else if /* CALLERANI */ (hash == CW_KEYWORD_CALLERANI)
            {
                if (c->cid.cid_ani)
                {
                    cw_copy_string(workspace, c->cid.cid_ani, workspacelen);
                    *ret = workspace;
                }
                else
                    *ret = NULL;
            }            
            else if /* CALLINGPRES */ (hash == CW_KEYWORD_CALLINGPRES)
            {
                snprintf(workspace, workspacelen, "%d", c->cid.cid_pres);
                *ret = workspace;
            }            
            else if /* CALLINGANI2 */ (hash == CW_KEYWORD_CALLINGANI2)
            {
                snprintf(workspace, workspacelen, "%d", c->cid.cid_ani2);
                *ret = workspace;
            }            
            else if /* CALLINGTON */ (hash == CW_KEYWORD_CALLINGTON)
            {
                snprintf(workspace, workspacelen, "%d", c->cid.cid_ton);
                *ret = workspace;
            }            
            else if /* CALLINGTNS */ (hash == CW_KEYWORD_CALLINGTNS)
            {
                snprintf(workspace, workspacelen, "%d", c->cid.cid_tns);
                *ret = workspace;
            }            
            else if /* DNID */ (hash == CW_KEYWORD_DNID)
            {
                if (c->cid.cid_dnid)
                {
                    cw_copy_string(workspace, c->cid.cid_dnid, workspacelen);
                    *ret = workspace;
                }
                else
                {
                    *ret = NULL;
                }
            }            
            else if /* HINT */ (hash == CW_KEYWORD_HINT)
            {
                if (!cw_get_hint(workspace, workspacelen, NULL, 0, c, c->context, c->exten))
                    *ret = NULL;
                else
                    *ret = workspace;
            }
            else if /* HINTNAME */ (hash == CW_KEYWORD_HINTNAME)
            {
                if (!cw_get_hint(NULL, 0, workspace, workspacelen, c, c->context, c->exten))
                    *ret = NULL;
                else
                    *ret = workspace;
            }
            else if /* EXTEN */ (hash == CW_KEYWORD_EXTEN)
            {
                cw_copy_string(workspace, c->exten, workspacelen);
                *ret = workspace;
            }
            else if /* RDNIS */ (hash == CW_KEYWORD_RDNIS)
            {
                if (c->cid.cid_rdnis)
                {
                    cw_copy_string(workspace, c->cid.cid_rdnis, workspacelen);
                    *ret = workspace;
                }
                else
                {
                    *ret = NULL;
                }
            }
            else if /* CONTEXT */ (hash == CW_KEYWORD_CONTEXT)
            {
                cw_copy_string(workspace, c->context, workspacelen);
                *ret = workspace;
            }
            else if /* PRIORITY */ (hash == CW_KEYWORD_PRIORITY)
            {
                snprintf(workspace, workspacelen, "%d", c->priority);
                *ret = workspace;
            }
            else if /* CHANNEL */ (hash == CW_KEYWORD_CHANNEL)
            {
                cw_copy_string(workspace, c->name, workspacelen);
                *ret = workspace;
            }
            else if /* UNIQUEID */ (hash == CW_KEYWORD_UNIQUEID)
            {
                snprintf(workspace, workspacelen, "%s", c->uniqueid);
                *ret = workspace;
            }
            else if /* HANGUPCAUSE */ (hash == CW_KEYWORD_HANGUPCAUSE)
            {
                snprintf(workspace, workspacelen, "%d", c->hangupcause);
                *ret = workspace;
            }
            else if /* ACCOUNTCODE */ (hash == CW_KEYWORD_ACCOUNTCODE)
            {
                cw_copy_string(workspace, c->accountcode, workspacelen);
                *ret = workspace;
            }
            else if /* LANGUAGE */ (hash == CW_KEYWORD_LANGUAGE)
            {
                cw_copy_string(workspace, c->language, workspacelen);
                *ret = workspace;
            }
	    else if /* SYSTEMNAME */ (hash == CW_KEYWORD_SYSTEMNAME)
	    {
		cw_copy_string(workspace, cw_config_CW_SYSTEM_NAME, workspacelen);
		*ret = workspace;
	    }	
            else if /* user defined channel variables exist */ (&c->varshead)
            {
                no_match_yet = 1;
                
                // ---------------------------------------------------
                // search user defined channel variables (scenario #2)
                // ---------------------------------------------------
                
                CW_LIST_TRAVERSE(&c->varshead, variables, entries) {
#if 0
                    cw_log(LOG_WARNING, "Comparing variable '%s' with '%s' in channel '%s'\n",
                             var, cw_var_name(variables), c->name);
#endif
                    if (strcasecmp(cw_var_name(variables),var) == 0)
                    {
                        *ret = cw_var_value(variables);
                        if (*ret)
                        {
                            cw_copy_string(workspace, *ret, workspacelen);
                            *ret = workspace;
                        }
                        no_match_yet = 0; // remember that we found a match
                        break;
                    }
                }
            }            
            else /* not a channel variable, neither built-in nor user-defined */
            {
                no_match_yet = 1;
            }        
        }
        else /* channel does not exist */
        {
            no_match_yet = 1;
            
            // -------------------------------------------------------------------------
            // search for user defined variables not bound to this channel (scenario #3)
            // -------------------------------------------------------------------------
            
            if /* parameter headp points to an address other than NULL */ (headp)
            {
            
                CW_LIST_TRAVERSE(headp, variables, entries) {
#if 0
                    cw_log(LOG_WARNING,"Comparing variable '%s' with '%s'\n",var,cw_var_name(variables));
#endif
                    if (strcasecmp(cw_var_name(variables), var) == 0)
                    {
                        *ret = cw_var_value(variables);
                        if (*ret)
                        {
                            cw_copy_string(workspace, *ret, workspacelen);
                            *ret = workspace;
                        }
                        no_match_yet = 0; // remember that we found a match
                        break;
                    }
                }
            }
            
        }        
        if /* no match yet */ (no_match_yet)
        {
            // ------------------------------------
            // search builtin globals (scenario #4)
            // ------------------------------------
            if /* EPOCH */ (hash == CW_KEYWORD_EPOCH)
            {
                snprintf(workspace, workspacelen, "%u",(int)time(NULL));
                *ret = workspace;
            }
            else if /* DATETIME */ (hash == CW_KEYWORD_DATETIME)
            {
                thistime = time(NULL);
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
            }
            else if /* TIMESTAMP */ (hash == CW_KEYWORD_TIMESTAMP)
            {
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
            }
            else if (!(*ret))
            {
                // -----------------------------------------
                // search user defined globals (scenario #5)
                // -----------------------------------------
                
                if /* globals variable list exists, not NULL */ (&globals)
                {
                    cw_mutex_lock(&globalslock);
                    CW_LIST_TRAVERSE(&globals, variables, entries)
                    {
#if 0
                        cw_log(LOG_WARNING,"Comparing variable '%s' with '%s' in globals\n",
                                 var, cw_var_name(variables));
#endif
                        if (hash == cw_var_hash(variables))
                        {
                            *ret = cw_var_value(variables);
                            if (*ret)
                            {
                                cw_copy_string(workspace, *ret, workspacelen);
                                *ret = workspace;
                            }
                        }
                    }
                    cw_mutex_unlock(&globalslock);
                }
            }
        }
    }
}

static int handle_show_functions(int fd, int argc, char *argv[])
{
    struct cw_func *acf;
    int count_acf = 0;

    cw_cli(fd, "Installed Custom Functions:\n--------------------------------------------------------------------------------\n");
    for (acf = funcs_head;  acf;  acf = acf->next)
    {
        cw_cli(fd,
                 "%-20.20s  %-35.35s  %s\n",
                 (acf->name)  ?  acf->name  :  "N/A",
                 (acf->syntax)  ?  acf->syntax  :  "N/A",
                 (acf->synopsis)  ?  acf->synopsis  :  "N/A");
        count_acf++;
    }
    cw_cli(fd, "%d custom functions installed.\n", count_acf);
    return 0;
}

static int handle_show_function(int fd, int argc, char *argv[])
{
    struct cw_func *acf;
    /* Maximum number of characters added by terminal coloring is 22 */
    char infotitle[64 + CW_MAX_APP + 22], syntitle[40], destitle[40];
    char info[64 + CW_MAX_APP], *synopsis = NULL, *description = NULL;
    char stxtitle[40], *syntax = NULL;
    int synopsis_size, description_size, syntax_size;

    if (argc < 3) return RESULT_SHOWUSAGE;

    if (!(acf = cw_function_find(argv[2])))
    {
        cw_cli(fd, "No function by that name registered.\n");
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

    snprintf(info, 64 + CW_MAX_APP, "\n  -= Info about function '%s' =- \n\n", acf->name);
    cw_term_color(infotitle, info, COLOR_MAGENTA, 0, 64 + CW_MAX_APP + 22);
    cw_term_color(stxtitle, "[Syntax]\n", COLOR_MAGENTA, 0, 40);
    cw_term_color(syntitle, "[Synopsis]\n", COLOR_MAGENTA, 0, 40);
    cw_term_color(destitle, "[Description]\n", COLOR_MAGENTA, 0, 40);
    cw_term_color(syntax,
           acf->syntax ? acf->syntax : "Not available",
           COLOR_CYAN, 0, syntax_size);
    cw_term_color(synopsis,
           acf->synopsis ? acf->synopsis : "Not available",
           COLOR_CYAN, 0, synopsis_size);
    cw_term_color(description,
           acf->desc ? acf->desc : "Not available",
           COLOR_CYAN, 0, description_size);
    
    cw_cli(fd,"%s%s%s\n\n%s%s\n\n%s%s\n", infotitle, stxtitle, syntax, syntitle, synopsis, destitle, description);

    return RESULT_SUCCESS;
}

static char *complete_show_function(char *line, char *word, int pos, int state)
{
    struct cw_func *acf;
    int which = 0;

    /* try to lock functions list ... */
    if (cw_mutex_lock(&funcs_lock))
    {
        cw_log(LOG_ERROR, "Unable to lock function list\n");
        return NULL;
    }

    acf = funcs_head;
    while (acf)
    {
        if (!strncasecmp(word, acf->name, strlen(word)))
        {
            if (++which > state)
            {
                char *ret = strdup(acf->name);
                cw_mutex_unlock(&funcs_lock);
                return ret;
            }
        }
        acf = acf->next; 
    }

    cw_mutex_unlock(&funcs_lock);
    return NULL; 
}

struct cw_func* cw_function_find(const char *name) 
{
	struct cw_func *p;
	unsigned int hash = cw_hash_app_name(name);

	if (cw_mutex_lock(&funcs_lock)) {
		cw_log(LOG_ERROR, "Unable to lock function list\n");
		return NULL;
	}

	for (p = funcs_head; p; p = p->next) {
		if (p->hash == hash)
			break;
	}

	cw_mutex_unlock(&funcs_lock);
	return p;
}

int cw_unregister_function(void *func) 
{
	struct cw_func **p;
	int ret;
    
	if (!func)
		return 0;

	if (cw_mutex_lock(&funcs_lock)) {
		cw_log(LOG_ERROR, "Unable to lock function list\n");
		return -1;
	}

	ret = -1;
	for (p = &funcs_head; *p; p = &((*p)->next)) {
		if (*p == func) {
			*p = (*p)->next;
			ret = 0;
			break;
		}
	}

	cw_mutex_unlock(&funcs_lock);

	if (!ret) {
		if (option_verbose > 1)
			cw_verbose(VERBOSE_PREFIX_2 "Unregistered custom function %s\n", ((struct cw_func *)func)->name);
		free(func);
	}

	return ret;
}

void *cw_register_function(const char *name,
	char *(*read)(struct cw_channel *chan, int argc, char **argv, char *buf, size_t len),
	void (*write)(struct cw_channel *chan, int argc, char **argv, const char *value),
	const char *synopsis, const char *syntax, const char *description)
{
	char tmps[80];
	struct cw_func *p;
	unsigned int hash;
 
	if (cw_mutex_lock(&funcs_lock)) {
		cw_log(LOG_ERROR, "Unable to lock function list. Failed registering function %s\n", name);
		return NULL;
	}

	hash = cw_hash_app_name(name);

	for (p = funcs_head; p; p = p->next) {
		if (!strcmp(p->name, name)) {
			cw_log(LOG_ERROR, "Function %s already registered.\n", name);
			cw_mutex_unlock(&funcs_lock);
			return NULL;
		}
		if (p->hash == hash) {
			cw_log(LOG_ERROR, "Hash for function %s collides with %s.\n", name, p->name);
			cw_mutex_unlock(&funcs_lock);
			return NULL;
		}
	}

	if (!(p = malloc(sizeof(*p)))) {
		cw_log(LOG_ERROR, "malloc: %s\n", strerror(errno));
		cw_mutex_unlock(&funcs_lock);
		return NULL;
	}

	p->hash = hash;
	p->read = read;
	p->write = write;
	p->name = name;
	p->synopsis = synopsis;
	p->syntax = syntax;
	p->desc = description;
	p->next = funcs_head;
	funcs_head = p;

	cw_mutex_unlock(&funcs_lock);

	if (option_verbose > 1)
		cw_verbose(VERBOSE_PREFIX_2 "Registered custom function '%s'\n", cw_term_color(tmps, name, COLOR_BRCYAN, 0, sizeof(tmps)));

	return p;
}


char *cw_func_read(struct cw_channel *chan, const char *in, char *workspace, size_t len)
{
	char *argv[100]; /* No function can take more than 100 args unless it parses them itself */
	char *args = NULL, *function, *p;
	char *ret = "0";
	struct cw_func *acfptr;

	function = cw_strdupa(in);

	if ((args = strchr(function, '('))) {
		*(args++) = '\0';
		if ((p = strrchr(args, ')')))
			*p = '\0';
		else
			cw_log(LOG_WARNING, "Can't find trailing parenthesis in \"%s\"?\n", args);
	} else {
		cw_log(LOG_WARNING, "Function doesn't contain parentheses.  Assuming null argument.\n");
	}

	if ((acfptr = cw_function_find(function))) {
        	if (acfptr->read)
			return (*acfptr->read)(chan, cw_separate_app_args(args, ',', arraysize(argv), argv), argv, workspace, len);
		cw_log(LOG_ERROR, "Function %s cannot be read\n", function);
	} else {
		cw_log(LOG_ERROR, "Function %s not registered\n", function);
	}

	return ret;
}

void cw_func_write(struct cw_channel *chan, const char *in, const char *value)
{
	char *argv[100]; /* No function can take more than 100 args unless it parses them itself */
	char *args = NULL, *function, *p;
	struct cw_func *acfptr;

	/* FIXME: unnecessary dup? */
	function = cw_strdupa(in);

	if ((args = strchr(function, '('))) {
		*(args++) = '\0';
		if ((p = strrchr(args, ')')))
			*p = '\0';
		else
			cw_log(LOG_WARNING, "Can't find trailing parenthesis?\n");
	} else {
		cw_log(LOG_WARNING, "Function doesn't contain parentheses.  Assuming null argument.\n");
	}

	if ((acfptr = cw_function_find(function))) {
		if (acfptr->write) {
			(*acfptr->write)(chan, cw_separate_app_args(args, ',', arraysize(argv), argv), argv, value);
			return;
		}
		cw_log(LOG_ERROR, "Function %s is read-only, it cannot be written to\n", function);
	} else {
		cw_log(LOG_ERROR, "Function %s not registered\n", function);
	}
}

static void pbx_substitute_variables_helper_full(struct cw_channel *c, struct varshead *headp, const char *cp1, char *cp2, int count)
{
    char *cp4 = 0;
    const char *tmp, *whereweare;
    int length;
    char *workspace = NULL;
    char *ltmp = NULL, *var = NULL;
    char *nextvar, *nextexp, *nextthing;
    char *vars, *vare;
    int pos, brackets, needsub, len;
    
    /* Substitutes variables into cp2, based on string cp1 */

    /* Save the last byte for a terminating '\0' */
    count--;

    whereweare =
    tmp = cp1;
    while (!cw_strlen_zero(whereweare)  &&  count)
    {
        /* Assume we're copying the whole remaining string */
        pos = strlen(whereweare);
        nextvar = NULL;
        nextexp = NULL;
        nextthing = strchr(whereweare, '$');
        if (nextthing)
        {
            switch (nextthing[1])
            {
            case '{':
                nextvar = nextthing;
                pos = nextvar - whereweare;
                break;
            case '[':
                nextexp = nextthing;
                pos = nextexp - whereweare;
                break;
            default:
                pos = nextthing - whereweare + 1;
                break;
            }
        }

        if (pos)
        {
            /* Can't copy more than 'count' bytes */
            if (pos > count)
                pos = count;
            
            /* Copy that many bytes */
            memcpy(cp2, whereweare, pos);
            
            count -= pos;
            cp2 += pos;
            whereweare += pos;
        }
        
        if (nextvar)
        {
            /* We have a variable.  Find the start and end, and determine
               if we are going to have to recursively call ourselves on the
               contents */
            vars = vare = nextvar + 2;
            brackets = 1;
            needsub = 0;

            /* Find the end of it */
            while (brackets  &&  *vare)
            {
                if ((vare[0] == '$')  &&  (vare[1] == '{'))
                {
                    needsub++;
                }
                else if (vare[0] == '{')
                {
                    brackets++;
                }
                else if (vare[0] == '}')
                {
                    brackets--;
                }
                else if ((vare[0] == '$')  &&  (vare[1] == '['))
                {
                    needsub++;
                }
                vare++;
            }
            if (brackets)
                cw_log(LOG_NOTICE, "Error in extension logic (missing '}')\n");
            len = vare - vars - 1;

            /* Skip totally over variable string */
            whereweare += (len + 3);

            if (!var)
                var = alloca(VAR_BUF_SIZE);

            /* Store variable name (and truncate) */
            cw_copy_string(var, vars, len + 1);

            /* Substitute if necessary */
            if (needsub)
            {
                if (!ltmp)
                    ltmp = alloca(VAR_BUF_SIZE);

                pbx_substitute_variables_helper_full(c, headp, var, ltmp, VAR_BUF_SIZE);
                vars = ltmp;
            }
            else
            {
                vars = var;
            }

            if (!workspace)
                workspace = alloca(VAR_BUF_SIZE);

            workspace[0] = '\0';

            if ((cp4 = strrchr(vars, ')')))
            {
                /* Evaluate function */
                pos = 0;
                length = VAR_BUF_SIZE;

                sscanf(cp4, "):%d:%d", &pos, &length);
                cp4[1] = '\0';
                cp4 = cw_func_read(c, vars, workspace, VAR_BUF_SIZE);

                if (cp4) {
                    char *p = cp4 + strlen(cp4);
                    if (pos < 0) {
                        cp4 = (p + pos <= cp4 ? cp4 : p + pos);
                    } else {
                        cp4 = (cp4 + pos > p ? p : cp4 + pos);
                    }
                    if (cp4 + length < p)
                        cp4[length] = '\0';
                }

                if (option_debug && option_verbose > 5)
            	    cw_log(LOG_DEBUG, "Function result is '%s'\n", cp4 ? cp4 : "(null)");
            }
            else
            {
                /* Retrieve variable value */
                pbx_retrieve_variable(c, vars, &cp4, workspace, VAR_BUF_SIZE, headp);
            }
            if (cp4)
            {
                length = strlen(cp4);
                if (length > count)
                    length = count;
                memcpy(cp2, cp4, length);
                count -= length;
                cp2 += length;
            }
        }
        else if (nextexp)
        {
            /* We have an expression.  Find the start and end, and determine
               if we are going to have to recursively call ourselves on the
               contents */
            vars = vare = nextexp + 2;
            brackets = 1;
            needsub = 0;

            /* Find the end of it */
            while (brackets  &&  *vare)
            {
                if ((vare[0] == '$') && (vare[1] == '['))
                {
                    needsub++;
                    brackets++;
                    vare++;
                }
                else if (vare[0] == '[')
                {
                    brackets++;
                }
                else if (vare[0] == ']')
                {
                    brackets--;
                }
                else if ((vare[0] == '$')  &&  (vare[1] == '{'))
                {
                    needsub++;
                    vare++;
                }
                vare++;
            }
            if (brackets)
                cw_log(LOG_NOTICE, "Error in extension logic (missing ']')\n");
            len = vare - vars - 1;
            
            /* Skip totally over expression */
            whereweare += (len + 3);
            
            if (!var)
                var = alloca(VAR_BUF_SIZE);

            /* Store variable name (and truncate) */
            cw_copy_string(var, vars, len + 1);
            
            /* Substitute if necessary */
            if (needsub)
            {
                if (!ltmp)
                    ltmp = alloca(VAR_BUF_SIZE);

                pbx_substitute_variables_helper_full(c, headp, var, ltmp, VAR_BUF_SIZE - 1);
                vars = ltmp;
            }
            else
            {
                vars = var;
            }

            length = cw_expr(vars, cp2, count);

            if (length)
            {
                cw_log(LOG_DEBUG, "Expression result is '%s'\n", cp2);
                count -= length;
                cp2 += length;
            }
        }
    }
    *cp2 = '\0';
}

void pbx_substitute_variables_helper(struct cw_channel *c, const char *cp1, char *cp2, int count)
{
    pbx_substitute_variables_helper_full(c, (c) ? &c->varshead : NULL, cp1, cp2, count);
}

void pbx_substitute_variables_varshead(struct varshead *headp, const char *cp1, char *cp2, int count)
{
    pbx_substitute_variables_helper_full(NULL, headp, cp1, cp2, count);
}

static void pbx_substitute_variables(char *passdata, int datalen, struct cw_channel *c, struct cw_exten *e)
{
    /* No variables or expressions in e->data, so why scan it? */
    if (!strchr(e->data, '$') && !strstr(e->data,"${") && !strstr(e->data,"$[") && !strstr(e->data,"$(")) {
        cw_copy_string(passdata, e->data, datalen);
        return;
    }
    
    pbx_substitute_variables_helper(c, e->data, passdata, datalen);
}                                                        

static int pbx_extension_helper(struct cw_channel *c, struct cw_context *con, const char *context, const char *exten, int priority, const char *label, const char *callerid, int action) 
{
    struct cw_exten *e;
    struct cw_app *app;
    struct cw_switch *sw;
    char *data;
    const char *foundcontext=NULL;
    int res;
    int status = 0;
    char *incstack[CW_PBX_MAX_STACK];
    char passdata[EXT_DATA_SIZE];
    int stacklen = 0;
    char tmp[80];
    char tmp2[80];
    char tmp3[EXT_DATA_SIZE];

    if (cw_mutex_lock(&conlock))
    {
        cw_log(LOG_WARNING, "Unable to obtain lock\n");
        if ((action == HELPER_EXISTS) || (action == HELPER_CANMATCH) || (action == HELPER_MATCHMORE))
            return 0;
        else
            return -1;
    }
    e = pbx_find_extension(c, con, context, exten, priority, label, callerid, action, incstack, &stacklen, &status, &sw, &data, &foundcontext);
    if (e)
    {
        switch (action)
        {
        case HELPER_CANMATCH:
            cw_mutex_unlock(&conlock);
            return -1;
        case HELPER_EXISTS:
            cw_mutex_unlock(&conlock);
            return -1;
        case HELPER_FINDLABEL:
            res = e->priority;
            cw_mutex_unlock(&conlock);
            return res;
        case HELPER_MATCHMORE:
            cw_mutex_unlock(&conlock);
            return -1;
        case HELPER_EXEC:
            app = pbx_findapp(e->app);
            cw_mutex_unlock(&conlock);
            if (app)
            {
                if (c->context != context)
                    cw_copy_string(c->context, context, sizeof(c->context));
                if (c->exten != exten)
                    cw_copy_string(c->exten, exten, sizeof(c->exten));
                c->priority = priority;
                pbx_substitute_variables(passdata, sizeof(passdata), c, e);
                if (option_verbose > 2)
                        cw_verbose( VERBOSE_PREFIX_3 "Executing [%s@%s:%d] %s(\"%s\", \"%s\")\n", 
                                exten, context, priority,
                                cw_term_color(tmp, app->name, COLOR_BRCYAN, 0, sizeof(tmp)),
                                cw_term_color(tmp2, c->name, COLOR_BRMAGENTA, 0, sizeof(tmp2)),
                                cw_term_color(tmp3, (!cw_strlen_zero(passdata) ? (char *)passdata : ""), COLOR_BRMAGENTA, 0, sizeof(tmp3)));
                manager_event(EVENT_FLAG_CALL, "Newexten", 
                    "Channel: %s\r\n"
                    "Context: %s\r\n"
                    "Extension: %s\r\n"
                    "Priority: %d\r\n"
                    "Application: %s\r\n"
                    "AppData: %s\r\n"
                    "Uniqueid: %s\r\n",
                    c->name, c->context, c->exten, c->priority, app->name, passdata ? passdata : "(NULL)", c->uniqueid);
                res = pbx_exec(c, app, passdata);
                return res;
            }
            cw_log(LOG_WARNING, "No application '%s' for extension (%s, %s, %d)\n", e->app, context, exten, priority);
            return -1;
        default:
            cw_log(LOG_WARNING, "Huh (%d)?\n", action);
            return -1;
        }
    }
    else if (sw)
    {
        switch (action)
        {
        case HELPER_CANMATCH:
            cw_mutex_unlock(&conlock);
            return -1;
        case HELPER_EXISTS:
            cw_mutex_unlock(&conlock);
            return -1;
        case HELPER_MATCHMORE:
            cw_mutex_unlock(&conlock);
            return -1;
        case HELPER_FINDLABEL:
            cw_mutex_unlock(&conlock);
            return -1;
        case HELPER_EXEC:
            cw_mutex_unlock(&conlock);
            if (sw->exec)
            {
                res = sw->exec(c, foundcontext ? foundcontext : context, exten, priority, callerid, data);
            }
            else
            {
                cw_log(LOG_WARNING, "No execution engine for switch %s\n", sw->name);
                res = -1;
            }
            return res;
        default:
            cw_log(LOG_WARNING, "Huh (%d)?\n", action);
            return -1;
        }
    }
    else
    {
        cw_mutex_unlock(&conlock);
        switch (status)
        {
        case STATUS_NO_CONTEXT:
            if ((action != HELPER_EXISTS) && (action != HELPER_MATCHMORE))
                cw_log(LOG_NOTICE, "Cannot find extension context '%s'\n", context);
            break;
        case STATUS_NO_EXTENSION:
            if ((action != HELPER_EXISTS) && (action !=  HELPER_CANMATCH) && (action != HELPER_MATCHMORE))
                cw_log(LOG_NOTICE, "Cannot find extension '%s' in context '%s'\n", exten, context);
            break;
        case STATUS_NO_PRIORITY:
            if ((action != HELPER_EXISTS) && (action !=  HELPER_CANMATCH) && (action != HELPER_MATCHMORE))
                cw_log(LOG_NOTICE, "No such priority %d in extension '%s' in context '%s'\n", priority, exten, context);
            break;
        case STATUS_NO_LABEL:
            if (context)
                cw_log(LOG_NOTICE, "No such label '%s' in extension '%s' in context '%s'\n", label, exten, context);
            break;
        default:
            cw_log(LOG_DEBUG, "Shouldn't happen!\n");
        }
        
        if ((action != HELPER_EXISTS) && (action != HELPER_CANMATCH) && (action != HELPER_MATCHMORE))
            return -1;
        else
            return 0;
    }

}

/*! \brief  cw_hint_extension: Find hint for given extension in context */
static struct cw_exten *cw_hint_extension(struct cw_channel *c, const char *context, const char *exten)
{
    struct cw_exten *e;
    struct cw_switch *sw;
    char *data;
    const char *foundcontext = NULL;
    int status = 0;
    char *incstack[CW_PBX_MAX_STACK];
    int stacklen = 0;

    if (cw_mutex_lock(&conlock))
    {
        cw_log(LOG_WARNING, "Unable to obtain lock\n");
        return NULL;
    }
    e = pbx_find_extension(c, NULL, context, exten, PRIORITY_HINT, NULL, "", HELPER_EXISTS, incstack, &stacklen, &status, &sw, &data, &foundcontext);
    cw_mutex_unlock(&conlock);    
    return e;
}

/*! \brief  cw_extensions_state2: Check state of extension by using hints */
static int cw_extension_state2(struct cw_exten *e)
{
    char hint[CW_MAX_EXTENSION] = "";    
    char *cur, *rest;
    int res = -1;
    int allunavailable = 1, allbusy = 1, allfree = 1;
    int busy = 0, inuse = 0, ring = 0;

    if (!e)
        return -1;

    cw_copy_string(hint, cw_get_extension_app(e), sizeof(hint));

    cur = hint;        /* On or more devices separated with a & character */
    do
    {
        rest = strchr(cur, '&');
        if (rest)
        {
            *rest = 0;
            rest++;
        }
    
        res = cw_device_state(cur);
        switch (res)
        {
        case CW_DEVICE_NOT_INUSE:
            allunavailable = 0;
            allbusy = 0;
            break;
        case CW_DEVICE_INUSE:
            inuse = 1;
            allunavailable = 0;
            allfree = 0;
            break;
        case CW_DEVICE_RINGING:
            ring = 1;
            allunavailable = 0;
            allfree = 0;
            break;
        case CW_DEVICE_BUSY:
            allunavailable = 0;
            allfree = 0;
            busy = 1;
            break;
        case CW_DEVICE_UNAVAILABLE:
        case CW_DEVICE_INVALID:
            allbusy = 0;
            allfree = 0;
            break;
        default:
            allunavailable = 0;
            allbusy = 0;
            allfree = 0;
        }
        cur = rest;
    }
    while (cur);

    if (!inuse && ring)
        return CW_EXTENSION_RINGING;
    if (inuse && ring)
        return (CW_EXTENSION_INUSE | CW_EXTENSION_RINGING);
    if (inuse)
        return CW_EXTENSION_INUSE;
    if (allfree)
        return CW_EXTENSION_NOT_INUSE;
    if (allbusy)        
        return CW_EXTENSION_BUSY;
    if (allunavailable)
        return CW_EXTENSION_UNAVAILABLE;
    if (busy) 
        return CW_EXTENSION_INUSE;
    
    return CW_EXTENSION_NOT_INUSE;
}

/*! \brief  cw_extension_state2str: Return extension_state as string */
const char *cw_extension_state2str(int extension_state)
{
    int i;

    for (i = 0;  (i < (sizeof(extension_states)/sizeof(extension_states[0])));  i++)
    {
        if (extension_states[i].extension_state == extension_state)
            return extension_states[i].text;
    }
    return "Unknown";    
}

/*! \brief  cw_extension_state: Check extension state for an extension by using hint */
int cw_extension_state(struct cw_channel *c, char *context, char *exten)
{
    struct cw_exten *e;

    e = cw_hint_extension(c, context, exten);    /* Do we have a hint for this extension ? */ 
    if (!e) 
        return -1;                /* No hint, return -1 */

    return cw_extension_state2(e);            /* Check all devices in the hint */
}

void cw_hint_state_changed(const char *device)
{
    struct cw_hint *hint;
    struct cw_state_cb *cblist;
    char buf[CW_MAX_EXTENSION];
    char *parse;
    char *cur;
    int state;

    cw_mutex_lock(&hintlock);

    for (hint = hints; hint; hint = hint->next)
    {
        cw_copy_string(buf, cw_get_extension_app(hint->exten), sizeof(buf));
        parse = buf;
        for (cur = strsep(&parse, "&"); cur; cur = strsep(&parse, "&"))
        {
            if (strcmp(cur, device))
                continue;

            /* Get device state for this hint */
            state = cw_extension_state2(hint->exten);
            
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

    cw_mutex_unlock(&hintlock);
}
            
/*! \brief  cw_extension_state_add: Add watcher for extension states */
int cw_extension_state_add(const char *context, const char *exten, 
                cw_state_cb_type callback, void *data)
{
    struct cw_hint *list;
    struct cw_state_cb *cblist;
    struct cw_exten *e;

    /* If there's no context and extension:  add callback to statecbs list */
    if (!context  &&  !exten)
    {
        cw_mutex_lock(&hintlock);

        cblist = statecbs;
        while (cblist)
        {
            if (cblist->callback == callback)
            {
                cblist->data = data;
                cw_mutex_unlock(&hintlock);
                return 0;
            }
            cblist = cblist->next;
        }
    
        /* Now insert the callback */
        if ((cblist = malloc(sizeof(struct cw_state_cb))) == NULL)
        {
            cw_mutex_unlock(&hintlock);
            return -1;
        }
        memset(cblist, 0, sizeof(struct cw_state_cb));
        cblist->id = 0;
        cblist->callback = callback;
        cblist->data = data;
    
        cblist->next = statecbs;
        statecbs = cblist;

        cw_mutex_unlock(&hintlock);
        return 0;
    }

    if (!context  ||  !exten)
        return -1;

    /* This callback type is for only one hint, so get the hint */
    e = cw_hint_extension(NULL, context, exten);    
    if (!e)
        return -1;

    /* Find the hint in the list of hints */
    cw_mutex_lock(&hintlock);
    list = hints;        

    while (list)
    {
        if (list->exten == e)
            break;        
        list = list->next;    
    }

    if (!list)
    {
        /* We have no hint, sorry */
        cw_mutex_unlock(&hintlock);
        return -1;
    }

    /* Now insert the callback in the callback list  */
    if ((cblist = malloc(sizeof(struct cw_state_cb))) == NULL)
    {
        cw_mutex_unlock(&hintlock);
        return -1;
    }
    memset(cblist, 0, sizeof(struct cw_state_cb));
    cblist->id = stateid++;        /* Unique ID for this callback */
    cblist->callback = callback;    /* Pointer to callback routine */
    cblist->data = data;        /* Data for the callback */

    cblist->next = list->callbacks;
    list->callbacks = cblist;

    cw_mutex_unlock(&hintlock);
    return cblist->id;
}

/*! \brief  cw_extension_state_del: Remove a watcher from the callback list */
int cw_extension_state_del(int id, cw_state_cb_type callback)
{
    struct cw_hint *list;
    struct cw_state_cb *cblist, *cbprev;

    if (!id && !callback)
        return -1;

    cw_mutex_lock(&hintlock);

    /* id is zero is a callback without extension */
    if (!id)
    {
        cbprev = NULL;
        cblist = statecbs;
        while (cblist)
        {
            if (cblist->callback == callback)
            {
                if (!cbprev)
                    statecbs = cblist->next;
                else
                    cbprev->next = cblist->next;

                free(cblist);

                cw_mutex_unlock(&hintlock);
                return 0;
            }
            cbprev = cblist;
            cblist = cblist->next;
        }

        cw_mutex_lock(&hintlock);
        return -1;
    }

    /* id greater than zero is a callback with extension */
    /* Find the callback based on ID */
    list = hints;
    while (list)
    {
        cblist = list->callbacks;
        cbprev = NULL;
        while (cblist)
        {
            if (cblist->id == id)
            {
                if (!cbprev)
                    list->callbacks = cblist->next;        
                else
                    cbprev->next = cblist->next;
                free(cblist);
        
                cw_mutex_unlock(&hintlock);
                return 0;        
            }        
            cbprev = cblist;                
            cblist = cblist->next;
        }
        list = list->next;
    }

    cw_mutex_unlock(&hintlock);
    return -1;
}

/*! \brief  cw_add_hint: Add hint to hint list, check initial extension state */
static int cw_add_hint(struct cw_exten *e)
{
    struct cw_hint *list;

    if (!e) 
        return -1;

    cw_mutex_lock(&hintlock);
    list = hints;        

    /* Search if hint exists, do nothing */
    while (list)
    {
        if (list->exten == e)
        {
            cw_mutex_unlock(&hintlock);
            if (option_debug > 1)
                cw_log(LOG_DEBUG, "HINTS: Not re-adding existing hint %s: %s\n", cw_get_extension_name(e), cw_get_extension_app(e));
            return -1;
        }
        list = list->next;    
    }

    if (option_debug > 1)
        cw_log(LOG_DEBUG, "HINTS: Adding hint %s: %s\n", cw_get_extension_name(e), cw_get_extension_app(e));

    if ((list = malloc(sizeof(struct cw_hint))) == NULL)
    {
        cw_mutex_unlock(&hintlock);
        if (option_debug > 1)
            cw_log(LOG_DEBUG, "HINTS: Out of memory...\n");
        return -1;
    }
    /* Initialize and insert new item at the top */
    memset(list, 0, sizeof(struct cw_hint));
    list->exten = e;
    list->laststate = cw_extension_state2(e);
    list->next = hints;
    hints = list;

    cw_mutex_unlock(&hintlock);
    return 0;
}

/*! \brief  cw_change_hint: Change hint for an extension */
static int cw_change_hint(struct cw_exten *oe, struct cw_exten *ne)
{ 
    struct cw_hint *list;

    cw_mutex_lock(&hintlock);
    list = hints;

    while (list)
    {
        if (list->exten == oe)
        {
                list->exten = ne;
            cw_mutex_unlock(&hintlock);    
            return 0;
        }
        list = list->next;
    }
    cw_mutex_unlock(&hintlock);

    return -1;
}

/*! \brief  cw_remove_hint: Remove hint from extension */
static int cw_remove_hint(struct cw_exten *e)
{
    /* Cleanup the Notifys if hint is removed */
    struct cw_hint *list, *prev = NULL;
    struct cw_state_cb *cblist, *cbprev;

    if (!e) 
        return -1;

    cw_mutex_lock(&hintlock);

    list = hints;    
    while (list)
    {
        if (list->exten == e)
        {
            cbprev = NULL;
            cblist = list->callbacks;
            while (cblist)
            {
                /* Notify with -1 and remove all callbacks */
                cbprev = cblist;        
                cblist = cblist->next;
                cbprev->callback(list->exten->parent->name, list->exten->exten, CW_EXTENSION_DEACTIVATED, cbprev->data);
                free(cbprev);
                }
                list->callbacks = NULL;

                if (!prev)
                hints = list->next;
                else
                prev->next = list->next;
                free(list);
        
            cw_mutex_unlock(&hintlock);
            return 0;
        }
        prev = list;
        list = list->next;    
    }

    cw_mutex_unlock(&hintlock);
    return -1;
}


/*! \brief  cw_get_hint: Get hint for channel */
int cw_get_hint(char *hint, int hintsize, char *name, int namesize, struct cw_channel *c, const char *context, const char *exten)
{
    struct cw_exten *e;
    void *tmp;

    e = cw_hint_extension(c, context, exten);
    if (e)
    {
        if (hint) 
            cw_copy_string(hint, cw_get_extension_app(e), hintsize);
        if (name)
        {
            tmp = cw_get_extension_app_data(e);
            if (tmp)
                cw_copy_string(name, (char *) tmp, namesize);
        }
        return -1;
    }
    return 0;    
}

int cw_exists_extension(struct cw_channel *c, const char *context, const char *exten, int priority, const char *callerid) 
{
    return pbx_extension_helper(c, NULL, context, exten, priority, NULL, callerid, HELPER_EXISTS);
}

int cw_findlabel_extension(struct cw_channel *c, const char *context, const char *exten, const char *label, const char *callerid) 
{
    return pbx_extension_helper(c, NULL, context, exten, 0, label, callerid, HELPER_FINDLABEL);
}

int cw_findlabel_extension2(struct cw_channel *c, struct cw_context *con, const char *exten, const char *label, const char *callerid) 
{
    return pbx_extension_helper(c, con, NULL, exten, 0, label, callerid, HELPER_FINDLABEL);
}

int cw_canmatch_extension(struct cw_channel *c, const char *context, const char *exten, int priority, const char *callerid)
{
    return pbx_extension_helper(c, NULL, context, exten, priority, NULL, callerid, HELPER_CANMATCH);
}

int cw_matchmore_extension(struct cw_channel *c, const char *context, const char *exten, int priority, const char *callerid)
{
    return pbx_extension_helper(c, NULL, context, exten, priority, NULL, callerid, HELPER_MATCHMORE);
}

int cw_exec_extension(struct cw_channel *c, const char *context, const char *exten, int priority, const char *callerid) 
{
    return pbx_extension_helper(c, NULL, context, exten, priority, NULL, callerid, HELPER_EXEC);
}

static int __cw_pbx_run(struct cw_channel *c)
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
        cw_log(LOG_WARNING, "%s already has PBX structure??\n", c->name);
    if ((c->pbx = malloc(sizeof(struct cw_pbx))) == NULL)
    {
        cw_log(LOG_ERROR, "Out of memory\n");
        return -1;
    }
    if (c->amaflags)
    {
        if (!c->cdr)
        {
            c->cdr = cw_cdr_alloc();
            if (!c->cdr)
            {
                cw_log(LOG_WARNING, "Unable to create Call Detail Record\n");
                free(c->pbx);
                return -1;
            }
            cw_cdr_init(c->cdr, c);
        }
    }
    memset(c->pbx, 0, sizeof(struct cw_pbx));
    /* Set reasonable defaults */
    c->pbx->rtimeout = 10;
    c->pbx->dtimeout = 5;

    autoloopflag = cw_test_flag(c, CW_FLAG_IN_AUTOLOOP);
    cw_set_flag(c, CW_FLAG_IN_AUTOLOOP);

    /* Start by trying whatever the channel is set to */
    if (!cw_exists_extension(c, c->context, c->exten, c->priority, c->cid.cid_num))
    {
        /* If not successful fall back to 's' */
        if (option_verbose > 1)
            cw_verbose( VERBOSE_PREFIX_2 "Starting %s at %s,%s,%d failed so falling back to exten 's'\n", c->name, c->context, c->exten, c->priority);
        cw_copy_string(c->exten, "s", sizeof(c->exten));
        if (!cw_exists_extension(c, c->context, c->exten, c->priority, c->cid.cid_num))
        {
            /* JK02: And finally back to default if everything else failed */
            if (option_verbose > 1)
                cw_verbose( VERBOSE_PREFIX_2 "Starting %s at %s,%s,%d still failed so falling back to context 'default'\n", c->name, c->context, c->exten, c->priority);
            cw_copy_string(c->context, "default", sizeof(c->context));
        }
        c->priority = 1;
    }
    if (c->cdr  &&  !c->cdr->start.tv_sec  &&  !c->cdr->start.tv_usec)
        cw_cdr_start(c->cdr);
    for(;;)
    {
        pos = 0;
        digit = 0;
        while (cw_exists_extension(c, c->context, c->exten, c->priority, c->cid.cid_num))
        {
            memset(exten, 0, sizeof(exten));
            if ((res = cw_exec_extension(c, c->context, c->exten, c->priority, c->cid.cid_num)))
            {
                /* Something bad happened, or a hangup has been requested. */
                if (((res >= '0') && (res <= '9')) || ((res >= 'A') && (res <= 'F')) ||
                    (res == '*') || (res == '#'))
                {
                    cw_log(LOG_DEBUG, "Oooh, got something to jump out with ('%c')!\n", res);
                    memset(exten, 0, sizeof(exten));
                    pos = 0;
                    exten[pos++] = digit = res;
                    break;
                }
                switch (res)
                {
                case CW_PBX_KEEPALIVE:
                    if (option_debug)
                        cw_log(LOG_DEBUG, "Spawn extension (%s,%s,%d) exited KEEPALIVE on '%s'\n", c->context, c->exten, c->priority, c->name);
                    if (option_verbose > 1)
                        cw_verbose( VERBOSE_PREFIX_2 "Spawn extension (%s, %s, %d) exited KEEPALIVE on '%s'\n", c->context, c->exten, c->priority, c->name);
                    goto out;
                    break;
                default:
                    if (option_debug)
                        cw_log(LOG_DEBUG, "Spawn extension (%s,%s,%d) exited non-zero on '%s'\n", c->context, c->exten, c->priority, c->name);
                    if (option_verbose > 1)
                        cw_verbose( VERBOSE_PREFIX_2 "Spawn extension (%s, %s, %d) exited non-zero on '%s'\n", c->context, c->exten, c->priority, c->name);
                    if (c->_softhangup == CW_SOFTHANGUP_ASYNCGOTO)
                    {
                        c->_softhangup =0;
                        break;
                    }
                    /* atimeout */
                    if (c->_softhangup == CW_SOFTHANGUP_TIMEOUT)
                    {
                        break;
                    }

                    if (c->cdr)
                    {
                        cw_cdr_update(c);
                    }
                    goto out;
                }
            }
            if ((c->_softhangup == CW_SOFTHANGUP_TIMEOUT) && (cw_exists_extension(c,c->context,"T",1,c->cid.cid_num)))
            {
                cw_copy_string(c->exten, "T", sizeof(c->exten));
                /* If the AbsoluteTimeout is not reset to 0, we'll get an infinite loop */
                c->whentohangup = 0;
                c->priority = 0;
                c->_softhangup &= ~CW_SOFTHANGUP_TIMEOUT;
            }
            else if (c->_softhangup)
            {
                cw_log(LOG_DEBUG, "Extension %s, priority %d returned normally even though call was hung up\n",
                    c->exten, c->priority);
                goto out;
            }
            firstpass = 0;
            c->priority++;
        }
        if (!cw_exists_extension(c, c->context, c->exten, 1, c->cid.cid_num))
        {
            /* It's not a valid extension anymore */
            if (cw_exists_extension(c, c->context, "i", 1, c->cid.cid_num))
            {
                if (option_verbose > 2)
                    cw_verbose(VERBOSE_PREFIX_3 "Sent into invalid extension '%s' in context '%s' on %s\n", c->exten, c->context, c->name);
                pbx_builtin_setvar_helper(c, "INVALID_EXTEN", c->exten);
                cw_copy_string(c->exten, "i", sizeof(c->exten));
                c->priority = 1;
            }
            else
            {
                cw_log(LOG_WARNING, "Channel '%s' sent into invalid extension '%s' in context '%s', but no invalid handler\n",
                    c->name, c->exten, c->context);
                goto out;
            }
        }
        else if (c->_softhangup == CW_SOFTHANGUP_TIMEOUT)
        {
            /* If we get this far with CW_SOFTHANGUP_TIMEOUT, then we know that the "T" extension is next. */
            c->_softhangup = 0;
        }
        else
        {
            /* Done, wait for an extension */
            waittime = 0;
            if (digit)
                waittime = c->pbx->dtimeout;
            else if (!autofallthrough)
                waittime = c->pbx->rtimeout;
            if (waittime)
            {
                while (cw_matchmore_extension(c, c->context, exten, 1, c->cid.cid_num))
                {
                    /* As long as we're willing to wait, and as long as it's not defined, 
                       keep reading digits until we can't possibly get a right answer anymore.  */
                    digit = cw_waitfordigit(c, waittime * 1000);
                    if (c->_softhangup == CW_SOFTHANGUP_ASYNCGOTO)
                    {
                        c->_softhangup = 0;
                    }
                    else
                    {
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
                if (cw_exists_extension(c, c->context, exten, 1, c->cid.cid_num)) {
                    /* Prepare the next cycle */
                    cw_copy_string(c->exten, exten, sizeof(c->exten));
                    c->priority = 1;
                }
                else
                {
                    /* No such extension */
                    if (!cw_strlen_zero(exten))
                    {
                        /* An invalid extension */
                        if (cw_exists_extension(c, c->context, "i", 1, c->cid.cid_num))
                        {
                            if (option_verbose > 2)
                                cw_verbose( VERBOSE_PREFIX_3 "Invalid extension '%s' in context '%s' on %s\n", exten, c->context, c->name);
                            pbx_builtin_setvar_helper(c, "INVALID_EXTEN", exten);
                            cw_copy_string(c->exten, "i", sizeof(c->exten));
                            c->priority = 1;
                        }
                        else
                        {
                            cw_log(LOG_WARNING, "Invalid extension '%s', but no rule 'i' in context '%s'\n", exten, c->context);
                            goto out;
                        }
                    }
                    else
                    {
                        /* A simple timeout */
                        if (cw_exists_extension(c, c->context, "t", 1, c->cid.cid_num))
                        {
                            if (option_verbose > 2)
                                cw_verbose( VERBOSE_PREFIX_3 "Timeout on %s\n", c->name);
                            cw_copy_string(c->exten, "t", sizeof(c->exten));
                            c->priority = 1;
                        }
                        else
                        {
                            cw_log(LOG_WARNING, "Timeout, but no rule 't' in context '%s'\n", c->context);
                            goto out;
                        }
                    }    
                }
                if (c->cdr)
                {
                    if (option_verbose > 2)
                        cw_verbose(VERBOSE_PREFIX_2 "CDR updated on %s\n",c->name);    
                    cw_cdr_update(c);
                }
            }
            else
            {
                char *status;

                // this should really use c->hangupcause instead of dialstatus
                // let's go along with it for now but we should revisit it later
                
                status = pbx_builtin_getvar_helper(c, "DIALSTATUS");
                if (!status)
                {
                    hash = 0;
                    status = "UNKNOWN";
                }
                else
                {
                    hash = cw_hash_var_name(status);
                }
                if (option_verbose > 2)
                    cw_verbose(VERBOSE_PREFIX_2 "Auto fallthrough, channel '%s' status is '%s'\n", c->name, status);

		status = "10";
                if (hash == CW_KEYWORD_BUSY)
                    res = pbx_builtin_busy(c, 1, &status);
                else if (hash == CW_KEYWORD_CHANUNAVAIL)
                    res = pbx_builtin_congestion(c, 1, &status);
                else if (hash == CW_KEYWORD_CONGESTION)
                    res = pbx_builtin_congestion(c, 1, &status);
                goto out;
            }
        }
    }
    if (firstpass) 
        cw_log(LOG_WARNING, "Don't know what to do with '%s'\n", c->name);
out:
    if ((res != CW_PBX_KEEPALIVE) && cw_exists_extension(c, c->context, "h", 1, c->cid.cid_num))
    {
		if (c->cdr && cw_end_cdr_before_h_exten)
			cw_cdr_end(c->cdr);

        c->exten[0] = 'h';
        c->exten[1] = '\0';
        c->priority = 1;
        while (cw_exists_extension(c, c->context, c->exten, c->priority, c->cid.cid_num))
        {
            if ((res = cw_exec_extension(c, c->context, c->exten, c->priority, c->cid.cid_num)))
            {
                /* Something bad happened, or a hangup has been requested. */
                if (option_debug)
                    cw_log(LOG_DEBUG, "Spawn extension (%s,%s,%d) exited non-zero on '%s'\n", c->context, c->exten, c->priority, c->name);
                if (option_verbose > 1)
                    cw_verbose( VERBOSE_PREFIX_2 "Spawn extension (%s, %s, %d) exited non-zero on '%s'\n", c->context, c->exten, c->priority, c->name);
                break;
            }
            c->priority++;
        }
    }
    cw_set2_flag(c, autoloopflag, CW_FLAG_IN_AUTOLOOP);

    pbx_destroy(c->pbx);
    c->pbx = NULL;
    if (res != CW_PBX_KEEPALIVE)
        cw_hangup(c);
    return 0;
}

/* Returns 0 on success, non-zero if call limit was reached */
static int increase_call_count(const struct cw_channel *c)
{
    int failed = 0;
    double curloadavg;

    cw_mutex_lock(&maxcalllock);
    if (option_maxcalls)
    {
        if (countcalls >= option_maxcalls)
        {
            cw_log(LOG_NOTICE, "Maximum call limit of %d calls exceeded by '%s'!\n", option_maxcalls, c->name);
            failed = -1;
        }
    }
    if (option_maxload)
    {
        getloadavg(&curloadavg, 1);
        if (curloadavg >= option_maxload)
        {
            cw_log(LOG_NOTICE, "Maximum loadavg limit of %lf load exceeded by '%s' (currently %f)!\n", option_maxload, c->name, curloadavg);
            failed = -1;
        }
    }
    if (!failed)
        countcalls++;    
    cw_mutex_unlock(&maxcalllock);

    return failed;
}

static void decrease_call_count(void)
{
    cw_mutex_lock(&maxcalllock);
    if (countcalls > 0)
        countcalls--;
    cw_mutex_unlock(&maxcalllock);
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
    struct cw_channel *c = data;

    __cw_pbx_run(c);
    decrease_call_count();

    pthread_exit(NULL);

    return NULL;
}

enum cw_pbx_result cw_pbx_start(struct cw_channel *c)
{
    pthread_t t;
    pthread_attr_t attr;

    if (!c)
    {
        cw_log(LOG_WARNING, "Asked to start thread on NULL channel?\n");
        return CW_PBX_FAILED;
    }
       
    if (increase_call_count(c))
        return CW_PBX_CALL_LIMIT;

    /* Start a new thread, and get something handling this channel. */
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (cw_pthread_create(&t, &attr, pbx_thread, c))
    {
        cw_log(LOG_WARNING, "Failed to create new channel thread\n");
        return CW_PBX_FAILED;
    }

    return CW_PBX_SUCCESS;
}

enum cw_pbx_result cw_pbx_run(struct cw_channel *c)
{
    enum cw_pbx_result res = CW_PBX_SUCCESS;

    if (increase_call_count(c))
        return CW_PBX_CALL_LIMIT;

    res = __cw_pbx_run(c);
    decrease_call_count();

    return res;
}

int cw_active_calls(void)
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
 * structure, leave context list locked and call cw_context_remove_include2
 * which removes include, unlock contexts list and return ...
 */
int cw_context_remove_include(const char *context, const char *include, const char *registrar)
{
    struct cw_context *c;
    unsigned int hash = cw_hash_string(context);

    if (cw_lock_contexts())
        return -1;

    /* walk contexts and search for the right one ...*/
    c = cw_walk_contexts(NULL);
    while (c)
    {
        /* we found one ... */
        if (hash == c->hash)
        {
            int ret;
            /* remove include from this context ... */    
            ret = cw_context_remove_include2(c, include, registrar);

            cw_unlock_contexts();

            /* ... return results */
            return ret;
        }
        c = cw_walk_contexts(c);
    }

    /* we can't find the right one context */
    cw_unlock_contexts();
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
int cw_context_remove_include2(struct cw_context *con, const char *include, const char *registrar)
{
    struct cw_include *i, *pi = NULL;

    if (cw_mutex_lock(&con->lock))
        return -1;

    /* walk includes */
    i = con->includes;
    while (i)
    {
        /* find our include */
        if (!strcmp(i->name, include)
            && 
            (!registrar || !strcmp(i->registrar, registrar)))
        {
            /* remove from list */
            if (pi)
                pi->next = i->next;
            else
                con->includes = i->next;
            /* free include and return */
            free(i);
            cw_mutex_unlock(&con->lock);
            return 0;
        }
        pi = i;
        i = i->next;
    }

    /* we can't find the right include */
    cw_mutex_unlock(&con->lock);
    return -1;
}

/*
 * This function locks contexts list by &conlist, search for the rigt context
 * structure, leave context list locked and call cw_context_remove_switch2
 * which removes switch, unlock contexts list and return ...
 */
int cw_context_remove_switch(const char *context, const char *sw, const char *data, const char *registrar)
{
    struct cw_context *c;
    unsigned int hash = cw_hash_string(context);

    if (cw_lock_contexts())
        return -1;

    /* walk contexts and search for the right one ...*/
    c = cw_walk_contexts(NULL);
    while (c)
    {
        /* we found one ... */
        if (hash == c->hash)
        {
            int ret;
            /* remove switch from this context ... */    
            ret = cw_context_remove_switch2(c, sw, data, registrar);

            cw_unlock_contexts();

            /* ... return results */
            return ret;
        }
        c = cw_walk_contexts(c);
    }

    /* we can't find the right one context */
    cw_unlock_contexts();
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
int cw_context_remove_switch2(struct cw_context *con, const char *sw, const char *data, const char *registrar)
{
    struct cw_sw *i, *pi = NULL;

    if (cw_mutex_lock(&con->lock))
        return -1;

    /* walk switches */
    i = con->alts;
    while (i)
    {
        /* find our switch */
        if (!strcmp(i->name, sw) && !strcmp(i->data, data)
            && 
            (!registrar || !strcmp(i->registrar, registrar)))
        {
            /* remove from list */
            if (pi)
                pi->next = i->next;
            else
                con->alts = i->next;
            /* free switch and return */
            free(i);
            cw_mutex_unlock(&con->lock);
            return 0;
        }
        pi = i;
        i = i->next;
    }

    /* we can't find the right switch */
    cw_mutex_unlock(&con->lock);
    return -1;
}

/*
 * This functions lock contexts list, search for the right context,
 * call cw_context_remove_extension2, unlock contexts list and return.
 * In this function we are using
 */
int cw_context_remove_extension(const char *context, const char *extension, int priority, const char *registrar)
{
    struct cw_context *c;
    unsigned int hash = cw_hash_string(context);

    if (cw_lock_contexts())
        return -1;

    /* walk contexts ... */
    c = cw_walk_contexts(NULL);
    while (c)
    {
        /* ... search for the right one ... */
        if (hash == c->hash)
        {
            /* ... remove extension ... */
            int ret = cw_context_remove_extension2(c, extension, priority,
                registrar);
            /* ... unlock contexts list and return */
            cw_unlock_contexts();
            return ret;
        }
        c = cw_walk_contexts(c);
    }

    /* we can't find the right context */
    cw_unlock_contexts();
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
int cw_context_remove_extension2(struct cw_context *con, const char *extension, int priority, const char *registrar)
{
    struct cw_exten *exten, *prev_exten = NULL;

    if (cw_mutex_lock(&con->lock))
        return -1;

    /* go through all extensions in context and search the right one ... */
    exten = con->root;
    while (exten)
    {
        /* look for right extension */
        if (!strcmp(exten->exten, extension)
            &&
            (!registrar || !strcmp(exten->registrar, registrar)))
        {
            struct cw_exten *peer;

            /* should we free all peers in this extension? (priority == 0)? */
            if (priority == 0)
            {
                /* remove this extension from context list */
                if (prev_exten)
                    prev_exten->next = exten->next;
                else
                    con->root = exten->next;

                /* fire out all peers */
                peer = exten; 
                while (peer)
                {
                    exten = peer->peer;
                    
                    if (!peer->priority == PRIORITY_HINT) 
                        cw_remove_hint(peer);

                    peer->datad(peer->data);
                    free(peer);

                    peer = exten;
                }

                cw_mutex_unlock(&con->lock);
                return 0;
            }
            else
            {
                /* remove only extension with exten->priority == priority */
                struct cw_exten *previous_peer = NULL;

                peer = exten;
                while (peer)
                {
                    /* is this our extension? */
                    if (peer->priority == priority
                        &&
                        (!registrar || !strcmp(peer->registrar, registrar)))
                    {
                        /* we are first priority extension? */
                        if (!previous_peer)
                        {
                            /* exists previous extension here? */
                            if (prev_exten)
                            {
                                /* yes, so we must change next pointer in
                                 * previous connection to next peer
                                 */
                                if (peer->peer)
                                {
                                    prev_exten->next = peer->peer;
                                    peer->peer->next = exten->next;
                                }
                                else
                                {
                                    prev_exten->next = exten->next;
                                }
                            }
                            else
                            {
                                /* no previous extension, we are first
                                 * extension, so change con->root ...
                                 */
                                if (peer->peer)
                                    con->root = peer->peer;
                                else
                                    con->root = exten->next; 
                            }
                        }
                        else
                        {
                            /* we are not first priority in extension */
                            previous_peer->peer = peer->peer;
                        }

                        /* now, free whole priority extension */
                        if (peer->priority==PRIORITY_HINT)
                            cw_remove_hint(peer);
                        peer->datad(peer->data);
                        free(peer);

                        cw_mutex_unlock(&con->lock);
                        return 0;
                    }
                    /* this is not right extension, skip to next peer */
                    previous_peer = peer;
                    peer = peer->peer;
                }

                cw_mutex_unlock(&con->lock);
                return -1;
            }
        }

        prev_exten = exten;
        exten = exten->next;
    }

    /* we can't find right extension */
    cw_mutex_unlock(&con->lock);
    return -1;
}


void *cw_register_application(const char *name, int (*execute)(struct cw_channel *, int, char **), const char *synopsis, const char *syntax, const char *description)
{
	char tmps[80];
	struct cw_app *p, **q;
	unsigned int hash;
    
	if (cw_mutex_lock(&apps_lock)) {
		cw_log(LOG_ERROR, "Unable to lock application list\n");
		return NULL;
	}

	hash = cw_hash_app_name(name);

	for (p = apps_head; p; p = p->next) {
		if (!strcmp(p->name, name)) {
			cw_log(LOG_WARNING, "Application '%s' already registered\n", name);
			cw_mutex_unlock(&apps_lock);
			return NULL;
		}
		if (p->hash == hash) {
			cw_log(LOG_WARNING, "Hash for application '%s' collides with %s\n", name, p->name);
			cw_mutex_unlock(&apps_lock);
			return NULL;
		}
	}

	if (!(p = malloc(sizeof(*p)))) {
		cw_log(LOG_ERROR, "Out of memory\n");
		cw_mutex_unlock(&apps_lock);
		return NULL;
	}

	p->execute = execute;
	p->hash = hash;
	p->name = name;
	p->synopsis = synopsis;
	p->syntax = syntax;
	p->description = description;
 
	/* Store in alphabetical order */

	// One more reason why the CLI should be removed from the daemon
	// and moved instead into a separate standalone command line utility
	// Alphabetic order is only needed for CLI output and this slows down
	// the daemon's performance unneccessarily, need to revisit later
	for (q = &apps_head; ; q = &((*q)->next)) {
		if (!*q || strcmp(name, (*q)->name) < 0) {
			p->next = *q;
			*q = p;
			break;
		}
	}

	if (option_verbose > 1)
		cw_verbose(VERBOSE_PREFIX_2 "Registered application '%s'\n", cw_term_color(tmps, name, COLOR_BRCYAN, 0, sizeof(tmps)));

	cw_mutex_unlock(&apps_lock);
	return p;
}


int cw_unregister_application(void *app) 
{
	struct cw_app **p;
	int ret;
    
	if (!app)
		return 0;

	if (cw_mutex_lock(&apps_lock)) {
		cw_log(LOG_ERROR, "Unable to lock application list\n");
		return -1;
	}

	ret = -1;
	for (p = &apps_head; *p; p = &((*p)->next)) {
		if (*p == app) {
			*p = (*p)->next;
			ret = 0;
			break;
		}
	}

	cw_mutex_unlock(&apps_lock);

	if (!ret) {
		if (option_verbose > 1)
			cw_verbose(VERBOSE_PREFIX_2 "Unregistered application %s\n", ((struct cw_app *)app)->name);
		free(app);
	}

	return ret;
}


int cw_register_switch(struct cw_switch *sw)
{
    struct cw_switch *tmp, *prev = NULL;
    
    if (cw_mutex_lock(&switchlock))
    {
        cw_log(LOG_ERROR, "Unable to lock switch lock\n");
        return -1;
    }
    tmp = switches;
    while (tmp)
    {
        if (!strcasecmp(tmp->name, sw->name))
            break;
        prev = tmp;
        tmp = tmp->next;
    }
    if (tmp)
    {
        cw_mutex_unlock(&switchlock);
        cw_log(LOG_WARNING, "Switch '%s' already found\n", sw->name);
        return -1;
    }
    sw->next = NULL;
    if (prev) 
        prev->next = sw;
    else
        switches = sw;
    cw_mutex_unlock(&switchlock);
    return 0;
}

void cw_unregister_switch(struct cw_switch *sw)
{
    struct cw_switch *tmp, *prev = NULL;

    if (cw_mutex_lock(&switchlock))
    {
        cw_log(LOG_ERROR, "Unable to lock switch lock\n");
        return;
    }
    tmp = switches;
    while (tmp)
    {
        if (tmp == sw)
        {
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
    cw_mutex_unlock(&switchlock);
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

static char show_globals_help[] =
"Usage: show globals\n"
"       List current global dialplan variables and their values\n";

static char set_global_help[] =
"Usage: set global <name> <value>\n"
"       Set global dialplan variable <name> to <value>\n";


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
    struct cw_app *a;
    int which = 0;

    /* try to lock applications list ... */
    if (cw_mutex_lock(&apps_lock))
    {
        cw_log(LOG_ERROR, "Unable to lock application list\n");
        return NULL;
    }

    /* ... walk all applications ... */
    a = apps_head; 
    while (a)
    {
        /* ... check if word matches this application ... */
        if (!strncasecmp(word, a->name, strlen(word)))
        {
            /* ... if this is right app serve it ... */
            if (++which > state)
            {
                char *ret = strdup(a->name);
                cw_mutex_unlock(&apps_lock);
                return ret;
            }
        }
        a = a->next; 
    }

    /* no application match */
    cw_mutex_unlock(&apps_lock);
    return NULL; 
}

static int handle_show_application(int fd, int argc, char *argv[])
{
    struct cw_app *a;
    int app, no_registered_app = 1;

    if (argc < 3) return RESULT_SHOWUSAGE;

    /* try to lock applications list ... */
    if (cw_mutex_lock(&apps_lock))
    {
        cw_log(LOG_ERROR, "Unable to lock application list\n");
        return -1;
    }

    /* ... go through all applications ... */
    a = apps_head; 
    while (a)
    {
        /* ... compare this application name with all arguments given
         * to 'show application' command ... */
        for (app = 2;  app < argc;  app++)
        {
            if (!strcasecmp(a->name, argv[app]))
            {
                /* Maximum number of characters added by terminal coloring is 22 */
                char infotitle[64 + CW_MAX_APP + 22], synopsistitle[40], syntaxtitle[40], destitle[40];
                char info[64 + CW_MAX_APP], *synopsis = NULL, *syntax = NULL, *description = NULL;
                int synopsis_size, syntax_size, description_size;

                no_registered_app = 0;

                if (a->synopsis)
                    synopsis_size = strlen(a->synopsis) + 23;
                else
                    synopsis_size = strlen("Not available") + 23;
                synopsis = alloca(synopsis_size);

                if (a->syntax)
                    syntax_size = strlen(a->syntax) + 23;
                else
                    syntax_size = strlen("Not available") + 23;
                syntax = alloca(syntax_size);

                if (a->description)
                    description_size = strlen(a->description) + 23;
                else
                    description_size = strlen("Not available") + 23;
                description = alloca(description_size);

                snprintf(info, 64 + CW_MAX_APP, "\n  -= Info about application '%s' =- \n\n", a->name);
                cw_term_color(infotitle, info, COLOR_MAGENTA, 0, 64 + CW_MAX_APP + 22);
                cw_term_color(synopsistitle, "[Synopsis]\n", COLOR_MAGENTA, 0, 40);
                cw_term_color(syntaxtitle, "[Syntax]\n", COLOR_MAGENTA, 0, 40);
                cw_term_color(destitle, "[Description]\n", COLOR_MAGENTA, 0, 40);
                cw_term_color(synopsis,
                                a->synopsis ? a->synopsis : "Not available",
                                COLOR_CYAN, 0, synopsis_size);
                cw_term_color(syntax,
                                a->syntax ? a->syntax : "Not available",
                                COLOR_CYAN, 0, syntax_size);
                cw_term_color(description,
                                a->description ? a->description : "Not available",
                                COLOR_CYAN, 0, description_size);

                cw_cli(fd,"%s%s%s\n\n%s%s\n\n%s%s\n", infotitle,
				synopsistitle, synopsis,
				syntaxtitle, syntax,
				destitle, description);
            }
        }
        a = a->next; 
    }

    cw_mutex_unlock(&apps_lock);

    /* we found at least one app? no? */
    if (no_registered_app) {
        cw_cli(fd, "Your application(s) is (are) not registered\n");
        return RESULT_FAILURE;
    }

    return RESULT_SUCCESS;
}

/*! \brief  handle_show_hints: CLI support for listing registred dial plan hints */
static int handle_show_hints(int fd, int argc, char *argv[])
{
    struct cw_hint *hint;
    int num = 0;
    int watchers;
    struct cw_state_cb *watcher;

    if (!hints)
    {
        cw_cli(fd, "There are no registered dialplan hints\n");
        return RESULT_SUCCESS;
    }
    /* ... we have hints ... */
    cw_cli(fd, "\n    -== Registered CallWeaver Dial Plan Hints ==-\n");
    if (cw_mutex_lock(&hintlock))
    {
        cw_log(LOG_ERROR, "Unable to lock hints\n");
        return -1;
    }
    hint = hints;
    while (hint)
    {
        watchers = 0;
        for (watcher = hint->callbacks; watcher; watcher = watcher->next)
            watchers++;
        cw_cli(fd, "   %-20.20s: %-20.20s  State:%-15.15s Watchers %2d\n",
            cw_get_extension_name(hint->exten), cw_get_extension_app(hint->exten),
            cw_extension_state2str(hint->laststate), watchers);
        num++;
        hint = hint->next;
    }
    cw_cli(fd, "----------------\n");
    cw_cli(fd, "- %d hints registered\n", num);
    cw_mutex_unlock(&hintlock);
    return RESULT_SUCCESS;
}

/*! \brief  handle_show_globals: CLI support for listing global variables */
static int handle_show_globals(int fd, int argc, char *argv[])
{
    struct cw_var_t *variable;
    int count = 0;

    cw_mutex_lock(&globalslock);
    CW_LIST_TRAVERSE(&globals, variable, entries)
    {
        cw_cli(fd, "  %s=%s\n", cw_var_name(variable), cw_var_value(variable));
        ++count;
    }
    cw_mutex_unlock(&globalslock);

    cw_cli(fd, "\n    -- %d variables\n", count);
    return RESULT_SUCCESS;
}

/*! \brief  CLI support for setting global variables */
static int handle_set_global(int fd, int argc, char *argv[])
{
    if (argc != 4)
        return RESULT_SHOWUSAGE;
    pbx_builtin_setvar_helper(NULL, argv[2], argv[3]);
    cw_cli(fd, "\n    -- Global variable %s set to %s\n", argv[2], argv[3]);
    return RESULT_SUCCESS;
}

/*! \brief  handle_show_switches: CLI support for listing registred dial plan switches */
static int handle_show_switches(int fd, int argc, char *argv[])
{
    struct cw_switch *sw;
    
    if (!switches)
    {
        cw_cli(fd, "There are no registered alternative switches\n");
        return RESULT_SUCCESS;
    }
    /* ... we have applications ... */
    cw_cli(fd, "\n    -= Registered CallWeaver Alternative Switches =-\n");
    if (cw_mutex_lock(&switchlock))
    {
        cw_log(LOG_ERROR, "Unable to lock switches\n");
        return -1;
    }
    sw = switches;
    while (sw)
    {
        cw_cli(fd, "%s: %s\n", sw->name, sw->description);
        sw = sw->next;
    }
    cw_mutex_unlock(&switchlock);
    return RESULT_SUCCESS;
}

/*
 * 'show applications' CLI command implementation functions ...
 */
static int handle_show_applications(int fd, int argc, char *argv[])
{
    struct cw_app *a;
    int like=0, describing=0;
    int total_match = 0;     /* Number of matches in like clause */
    int total_apps = 0;     /* Number of apps registered */
    
    /* try to lock applications list ... */
    if (cw_mutex_lock(&apps_lock))
    {
        cw_log(LOG_ERROR, "Unable to lock application list\n");
        return -1;
    }

    /* ... have we got at least one application (first)? no? */
    if (!apps_head)
    {
        cw_cli(fd, "There are no registered applications\n");
        cw_mutex_unlock(&apps_lock);
        return -1;
    }

    /* show applications like <keyword> */
    if ((argc == 4) && (!strcmp(argv[2], "like")))
    {
        like = 1;
    }
    else if ((argc > 3) && (!strcmp(argv[2], "describing")))
    {
        describing = 1;
    }

    /* show applications describing <keyword1> [<keyword2>] [...] */
    if ((!like) && (!describing))
    {
        cw_cli(fd, "    -= Registered CallWeaver Applications =-\n");
    }
    else
    {
        cw_cli(fd, "    -= Matching CallWeaver Applications =-\n");
    }

    /* ... go through all applications ... */
    for (a = apps_head;  a;  a = a->next)
    {
        /* ... show information about applications ... */
        int printapp=0;

        total_apps++;
        if (like)
        {
            if (strcasestr(a->name, argv[3]))
            {
                printapp = 1;
                total_match++;
            }
        }
        else if (describing)
        {
            if (a->description)
            {
                /* Match all words on command line */
                int i;
                printapp = 1;
                for (i = 3;  i < argc;  i++)
                {
                    if (!strcasestr(a->description, argv[i]))
                        printapp = 0;
                    else
                        total_match++;
                }
            }
        }
        else
        {
            printapp = 1;
        }

        if (printapp)
        {
            cw_cli(fd,"  %20s (%#x): %s\n", a->name, a->hash,
                     a->synopsis ? a->synopsis : "<Synopsis not available>");
        }
    }
    if ((!like)  &&  (!describing))
        cw_cli(fd, "    -= %d Applications Registered =-\n", total_apps);
    else
        cw_cli(fd, "    -= %d Applications Matching =-\n", total_match);
    
    /* ... unlock and return */
    cw_mutex_unlock(&apps_lock);

    return RESULT_SUCCESS;
}

static char *complete_show_applications(char *line, char *word, int pos, int state)
{
    if (pos == 2)
    {
        if (cw_strlen_zero(word))
        {
            switch (state)
            {
            case 0:
                return strdup("like");
            case 1:
                return strdup("describing");
            default:
                return NULL;
            }
        }
        else if (! strncasecmp(word, "like", strlen(word)))
        {
            if (state == 0)
                return strdup("like");
            return NULL;
        }
        else if (! strncasecmp(word, "describing", strlen(word)))
        {
            if (state == 0)
                return strdup("describing");
            return NULL;
        }
    }
    return NULL;
}

/*
 * 'show dialplan' CLI command implementation functions ...
 */
static char *complete_show_dialplan_context(char *line, char *word, int pos, int state)
{
    struct cw_context *c;
    int which = 0;

    /* we are do completion of [exten@]context on second position only */
    if (pos != 2) return NULL;

    /* try to lock contexts list ... */
    if (cw_lock_contexts())
    {
        cw_log(LOG_ERROR, "Unable to lock context list\n");
        return NULL;
    }

    /* ... walk through all contexts ... */
    c = cw_walk_contexts(NULL);
    while (c)
    {
        /* ... word matches context name? yes? ... */
        if (!strncasecmp(word, cw_get_context_name(c), strlen(word)))
        {
            /* ... for serve? ... */
            if (++which > state)
            {
                /* ... yes, serve this context name ... */
                char *ret = strdup(cw_get_context_name(c));
                cw_unlock_contexts();
                return ret;
            }
        }
        c = cw_walk_contexts(c);
    }

    /* ... unlock and return */
    cw_unlock_contexts();
    return NULL;
}

struct dialplan_counters
{
    int total_context;
    int total_exten;
    int total_prio;
    int context_existence;
    int extension_existence;
};

static int show_dialplan_helper(int fd, char *context, char *exten, struct dialplan_counters *dpc, struct cw_include *rinclude, int includecount, char *includes[])
{
    struct cw_context *c;
    int res=0, old_total_exten = dpc->total_exten;

    /* try to lock contexts */
    if (cw_lock_contexts())
    {
        cw_log(LOG_WARNING, "Failed to lock contexts list\n");
        return -1;
    }

    /* walk all contexts ... */
    for (c = cw_walk_contexts(NULL); c ; c = cw_walk_contexts(c))
    {
        /* show this context? */
        if (!context  ||  !strcmp(cw_get_context_name(c), context))
        {
            dpc->context_existence = 1;

            /* try to lock context before walking in ... */
            if (!cw_lock_context(c))
            {
                struct cw_exten *e;
                struct cw_include *i;
                struct cw_ignorepat *ip;
                struct cw_sw *sw;
                char buf[256], buf2[256];
                int context_info_printed = 0;

                /* are we looking for exten too? if yes, we print context
                 * if we our extension only
                 */
                if (!exten)
                {
                    dpc->total_context++;
                    cw_cli(fd, "[ Context '%s' (%#x) created by '%s' ]\n",
                        cw_get_context_name(c), c->hash, cw_get_context_registrar(c));
                    context_info_printed = 1;
                }

                /* walk extensions ... */
                for (e = cw_walk_context_extensions(c, NULL);  e;  e = cw_walk_context_extensions(c, e))
                {
                    struct cw_exten *p;
                    int prio;

                    /* looking for extension? is this our extension? */
                    if (exten
                        &&
                        !cw_extension_match(cw_get_extension_name(e), exten))
                    {
                        /* we are looking for extension and it's not our
                          * extension, so skip to next extension */
                        continue;
                    }

                    dpc->extension_existence = 1;

                    /* may we print context info? */    
                    if (!context_info_printed)
                    {
                        dpc->total_context++;
                        if (rinclude)
                        {
                            /* TODO Print more info about rinclude */
                            cw_cli(fd, "[ Included context '%s' (%#x) created by '%s' ]\n",
                                cw_get_context_name(c), c->hash,
                                cw_get_context_registrar(c));
                        }
                        else
                        {
                            cw_cli(fd, "[ Context '%s' (%#x) created by '%s' ]\n",
                                cw_get_context_name(c), c->hash,
                                cw_get_context_registrar(c));
                        }
                        context_info_printed = 1;
                    }
                    dpc->total_prio++;

                    /* write extension name and first peer */    
                    bzero(buf, sizeof(buf));        
                    snprintf(buf, sizeof(buf), "'%s' =>",
                        cw_get_extension_name(e));

                    prio = cw_get_extension_priority(e);
                    if (prio == PRIORITY_HINT)
                    {
                        snprintf(buf2, sizeof(buf2),
                            "hint: %s",
                            cw_get_extension_app(e));
                    }
                    else
                    {
                        snprintf(buf2, sizeof(buf2),
                            "%d. %s(%s)",
                            prio,
                            cw_get_extension_app(e),
                            (char *)cw_get_extension_app_data(e));
                    }

                    cw_cli(fd, "  %-17s %-45s [%s]\n", buf, buf2,
                        cw_get_extension_registrar(e));

                    dpc->total_exten++;
                    /* walk next extension peers */
                    for (p = cw_walk_extension_priorities(e, e);  p;  p = cw_walk_extension_priorities(e, p))
                    {
                        dpc->total_prio++;
                        bzero((void *) buf2, sizeof(buf2));
                        bzero((void *) buf, sizeof(buf));
                        if (cw_get_extension_label(p))
                            snprintf(buf, sizeof(buf), "   [%s]", cw_get_extension_label(p));
                        prio = cw_get_extension_priority(p);
                        if (prio == PRIORITY_HINT)
                        {
                            snprintf(buf2, sizeof(buf2),
                                "hint: %s",
                                cw_get_extension_app(p));
                        }
                        else
                        {
                            snprintf(buf2, sizeof(buf2),
                                "%d. %s(%s)",
                                prio,
                                cw_get_extension_app(p),
                                (char *)cw_get_extension_app_data(p));
                        }

                        cw_cli(fd,"  %-17s %-45s [%s]\n",
                            buf, buf2,
                            cw_get_extension_registrar(p));
                    }
                }

                /* walk included and write info ... */
                for (i = cw_walk_context_includes(c, NULL);  i;  i = cw_walk_context_includes(c, i))
                {
                    bzero(buf, sizeof(buf));
                    snprintf(buf, sizeof(buf), "'%s'",
                        cw_get_include_name(i));
                    if (exten)
                    {
                        /* Check all includes for the requested extension */
                        if (includecount >= CW_PBX_MAX_STACK)
                        {
                            cw_log(LOG_NOTICE, "Maximum include depth exceeded!\n");
                        }
                        else
                        {
                            int dupe=0;
                            int x;

                            for (x = 0;  x < includecount;  x++)
                            {
                                if (!strcasecmp(includes[x], cw_get_include_name(i)))
                                {
                                    dupe++;
                                    break;
                                }
                            }
                            if (!dupe)
                            {
                                includes[includecount] = (char *)cw_get_include_name(i);
                                show_dialplan_helper(fd, (char *)cw_get_include_name(i),
                                                    exten, dpc, i, includecount + 1, includes);
                            }
                            else
                            {
                                cw_log(LOG_WARNING, "Avoiding circular include of %s within %s (%#x)\n",
                                         cw_get_include_name(i), context, c->hash);
                            }
                        }
                    }
                    else
                    {
                        cw_cli(fd, "  Include =>        %-45s [%s]\n",
                                 buf, cw_get_include_registrar(i));
                    }
                }

                /* walk ignore patterns and write info ... */
                for (ip = cw_walk_context_ignorepats(c, NULL);  ip;  ip = cw_walk_context_ignorepats(c, ip))
                {
                    const char *ipname = cw_get_ignorepat_name(ip);
                    char ignorepat[CW_MAX_EXTENSION];

                    snprintf(buf, sizeof(buf), "'%s'", ipname);
                    snprintf(ignorepat, sizeof(ignorepat), "_%s.", ipname);
                    if ((!exten)  ||  cw_extension_match(ignorepat, exten))
                    {
                        cw_cli(fd, "  Ignore pattern => %-45s [%s]\n",
                            buf, cw_get_ignorepat_registrar(ip));
                    }
                }
                if (!rinclude)
                {
                    for (sw = cw_walk_context_switches(c, NULL);  sw;  sw = cw_walk_context_switches(c, sw))
                    {
                        snprintf(buf, sizeof(buf), "'%s/%s'",
                            cw_get_switch_name(sw),
                            cw_get_switch_data(sw));
                        cw_cli(fd, "  Alt. Switch =>    %-45s [%s]\n",
                            buf, cw_get_switch_registrar(sw));    
                    }
                }
    
                cw_unlock_context(c);

                /* if we print something in context, make an empty line */
                if (context_info_printed) cw_cli(fd, "\r\n");
            }
        }
    }
    cw_unlock_contexts();

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
    char *incstack[CW_PBX_MAX_STACK];
    memset(&counters, 0, sizeof(counters));

    if (argc != 2  &&  argc != 3) 
        return RESULT_SHOWUSAGE;

    /* we obtain [exten@]context? if yes, split them ... */
    if (argc == 3)
    {
        char *splitter = cw_strdupa(argv[2]);
        /* is there a '@' character? */
        if (strchr(argv[2], '@'))
        {
            /* yes, split into exten & context ... */
            exten   = strsep(&splitter, "@");
            context = splitter;

            /* check for length and change to NULL if cw_strlen_zero() */
            if (cw_strlen_zero(exten))
                exten = NULL;
            if (cw_strlen_zero(context))
                context = NULL;
            show_dialplan_helper(fd, context, exten, &counters, NULL, 0, incstack);
        }
        else
        {
            /* no '@' char, only context given */
            context = argv[2];
            if (cw_strlen_zero(context))
                context = NULL;
            show_dialplan_helper(fd, context, exten, &counters, NULL, 0, incstack);
        }
    }
    else
    {
        /* Show complete dial plan */
        show_dialplan_helper(fd, NULL, NULL, &counters, NULL, 0, incstack);
    }

    /* check for input failure and throw some error messages */
    if (context  &&  !counters.context_existence)
    {
        cw_cli(fd, "No such context '%s'\n", context);
        return RESULT_FAILURE;
    }

    if (exten  &&  !counters.extension_existence)
    {
        if (context)
            cw_cli(fd,
                     "No such extension %s in context %s\n",
                     exten,
                     context);
        else
            cw_cli(fd,
                     "No such extension '%s' extension in any context\n",
                     exten);
        return RESULT_FAILURE;
    }

    cw_cli(fd,"-= %d %s (%d %s) in %d %s. =-\n",
                counters.total_exten, counters.total_exten == 1 ? "extension" : "extensions",
                counters.total_prio, counters.total_prio == 1 ? "priority" : "priorities",
                counters.total_context, counters.total_context == 1 ? "context" : "contexts");

    /* everything ok */
    return RESULT_SUCCESS;
}

/*
 * CLI entries for upper commands ...
 */
static struct cw_cli_entry pbx_cli[] = {
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
    { { "show", "switches", NULL },    handle_show_switches,
      "Show alternative switches", show_switches_help },
    { { "show", "hints", NULL }, handle_show_hints,
      "Show dialplan hints", show_hints_help },
    { { "show", "globals", NULL }, handle_show_globals,
      "Show global dialplan variables", show_globals_help },
    { { "set", "global", NULL }, handle_set_global,
      "Set global dialplan variable", set_global_help },
};


struct cw_context *cw_context_create(struct cw_context **extcontexts, const char *name, const char *registrar)
{
    struct cw_context *tmp, **local_contexts;
    unsigned int hash = cw_hash_string(name);
    int length;
    
    length = sizeof(struct cw_context);
    length += strlen(name) + 1;
    if (!extcontexts)
    {
        local_contexts = &contexts;
        cw_mutex_lock(&conlock);
    }
    else
    {
        local_contexts = extcontexts;
    }
    tmp = *local_contexts;
    while (tmp)
    {
        if (hash == tmp->hash)
        {
            cw_mutex_unlock(&conlock);
            cw_log(LOG_WARNING, "Failed to register context '%s' because it is already in use\n", name);
            if (!extcontexts)
                cw_mutex_unlock(&conlock);
            return NULL;
        }
        tmp = tmp->next;
    }
    if ((tmp = malloc(length)))
    {
        memset(tmp, 0, length);
        cw_mutex_init(&tmp->lock);
        tmp->hash = hash;
        strcpy(tmp->name, name);
        tmp->root = NULL;
        tmp->registrar = registrar;
        tmp->next = *local_contexts;
        tmp->includes = NULL;
        tmp->ignorepats = NULL;
        *local_contexts = tmp;
        if (option_debug)
            cw_log(LOG_DEBUG, "Registered context '%s' (%#x)\n", tmp->name, tmp->hash);
        else if (option_verbose > 2)
            cw_verbose( VERBOSE_PREFIX_3 "Registered extension context '%s' (%#x)\n", tmp->name, tmp->hash);
    }
    else
    {
        cw_log(LOG_ERROR, "Out of memory\n");
    }
    
    if (!extcontexts)
        cw_mutex_unlock(&conlock);
    return tmp;
}

void __cw_context_destroy(struct cw_context *con, const char *registrar);

struct store_hint
{
    char *context;
    char *exten;
    struct cw_state_cb *callbacks;
    int laststate;
    CW_LIST_ENTRY(store_hint) list;
    char data[1];
};

CW_LIST_HEAD(store_hints, store_hint);

void cw_merge_contexts_and_delete(struct cw_context **extcontexts, const char *registrar)
{
    struct cw_context *tmp, *lasttmp = NULL;
    struct store_hints store;
    struct store_hint *this;
    struct cw_hint *hint;
    struct cw_exten *exten;
    int length;
    struct cw_state_cb *thiscb, *prevcb;

    /* preserve all watchers for hints associated with this registrar */
    CW_LIST_HEAD_INIT(&store);
    cw_mutex_lock(&hintlock);
    for (hint = hints;  hint;  hint = hint->next)
    {
        if (hint->callbacks  &&  !strcmp(registrar, hint->exten->parent->registrar))
        {
            length = strlen(hint->exten->exten) + strlen(hint->exten->parent->name) + 2 + sizeof(*this);
            if ((this = calloc(1, length)) == NULL)
            {
                cw_log(LOG_WARNING, "Could not allocate memory to preserve hint\n");
                continue;
            }
            this->callbacks = hint->callbacks;
            hint->callbacks = NULL;
            this->laststate = hint->laststate;
            this->context = this->data;
            strcpy(this->data, hint->exten->parent->name);
            this->exten = this->data + strlen(this->context) + 1;
            strcpy(this->exten, hint->exten->exten);
            CW_LIST_INSERT_HEAD(&store, this, list);
        }
    }
    cw_mutex_unlock(&hintlock);

    tmp = *extcontexts;
    cw_mutex_lock(&conlock);
    if (registrar)
    {
        __cw_context_destroy(NULL,registrar);
        while (tmp)
        {
            lasttmp = tmp;
            tmp = tmp->next;
        }
    }
    else
    {
        while (tmp)
        {
            __cw_context_destroy(tmp,tmp->registrar);
            lasttmp = tmp;
            tmp = tmp->next;
        }
    }
    if (lasttmp)
    {
        lasttmp->next = contexts;
        contexts = *extcontexts;
        *extcontexts = NULL;
    }
    else 
    {
        cw_log(LOG_WARNING, "Requested contexts could not be merged\n");
    }
    cw_mutex_unlock(&conlock);

    /* restore the watchers for hints that can be found; notify those that
       cannot be restored
    */
    while ((this = CW_LIST_REMOVE_HEAD(&store, list)))
    {
        exten = cw_hint_extension(NULL, this->context, this->exten);
        /* Find the hint in the list of hints */
        cw_mutex_lock(&hintlock);
        for (hint = hints;  hint;  hint = hint->next)
        {
            if (hint->exten == exten)
                break;
        }
        if (!exten  ||  !hint)
        {
            /* this hint has been removed, notify the watchers */
            prevcb = NULL;
            thiscb = this->callbacks;
            while (thiscb)
            {
                prevcb = thiscb;        
                thiscb = thiscb->next;
                prevcb->callback(this->context, this->exten, CW_EXTENSION_REMOVED, prevcb->data);
                free(prevcb);
            }
        }
        else
        {
            thiscb = this->callbacks;
            while (thiscb->next)
                thiscb = thiscb->next;
            thiscb->next = hint->callbacks;
            hint->callbacks = this->callbacks;
            hint->laststate = this->laststate;
        }
        cw_mutex_unlock(&hintlock);
        free(this);
    }
}

/*
 * errno values
 *  EBUSY  - can't lock
 *  ENOENT - no existence of context
 */
int cw_context_add_include(const char *context, const char *include, const char *registrar)
{
    struct cw_context *c;
    unsigned int hash = cw_hash_string(context);

    if (cw_lock_contexts())
    {
        errno = EBUSY;
        return -1;
    }

    /* walk contexts ... */
    c = cw_walk_contexts(NULL);
    while (c)
    {
        /* ... search for the right one ... */
        if (hash == c->hash)
        {
            int ret = cw_context_add_include2(c, include, registrar);
            /* ... unlock contexts list and return */
            cw_unlock_contexts();
            return ret;
        }
        c = cw_walk_contexts(c);
    }

    /* we can't find the right context */
    cw_unlock_contexts();
    errno = ENOENT;
    return -1;
}

#define FIND_NEXT \
do { \
    c = info; \
    while(*c && (*c != ',')) c++; \
    if (*c) { *c = '\0'; c++; } else c = NULL; \
} while(0)

static void get_timerange(struct cw_timing *i, char *times)
{
    char *e;
    int x;
    int s1, s2;
    int e1, e2;
    /*    int cth, ctm; */

    /* start disabling all times, fill the fields with 0's, as they may contain garbage */
    memset(i->minmask, 0, sizeof(i->minmask));
    
    /* Star is all times */
    if (cw_strlen_zero(times)  ||  !strcmp(times, "*"))
    {
        for (x=0; x<24; x++)
            i->minmask[x] = (1 << 30) - 1;
        return;
    }
    /* Otherwise expect a range */
    e = strchr(times, '-');
    if (!e)
    {
        cw_log(LOG_WARNING, "Time range is not valid. Assuming no restrictions based on time.\n");
        return;
    }
    *e = '\0';
    e++;
    while (*e  &&  !isdigit(*e)) 
        e++;
    if (!*e)
    {
        cw_log(LOG_WARNING, "Invalid time range.  Assuming no restrictions based on time.\n");
        return;
    }
    if (sscanf(times, "%d:%d", &s1, &s2) != 2)
    {
        cw_log(LOG_WARNING, "%s isn't a time.  Assuming no restrictions based on time.\n", times);
        return;
    }
    if (sscanf(e, "%d:%d", &e1, &e2) != 2)
    {
        cw_log(LOG_WARNING, "%s isn't a time.  Assuming no restrictions based on time.\n", e);
        return;
    }

#if 1
    s1 = s1 * 30 + s2/2;
    if ((s1 < 0) || (s1 >= 24*30))
    {
        cw_log(LOG_WARNING, "%s isn't a valid start time. Assuming no time.\n", times);
        return;
    }
    e1 = e1 * 30 + e2/2;
    if ((e1 < 0)  ||  (e1 >= 24*30))
    {
        cw_log(LOG_WARNING, "%s isn't a valid end time. Assuming no time.\n", e);
        return;
    }
    /* Go through the time and enable each appropriate bit */
    for (x = s1;  x != e1;  x = (x + 1) % (24 * 30))
    {
        i->minmask[x/30] |= (1 << (x % 30));
    }
    /* Do the last one */
    i->minmask[x/30] |= (1 << (x % 30));
#else
    for (cth = 0;  cth < 24;  cth++)
    {
        /* Initialize masks to blank */
        i->minmask[cth] = 0;
        for (ctm = 0;  ctm < 30;  ctm++)
        {
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
    if (cw_strlen_zero(dow)  ||  !strcmp(dow, "*"))
        return (1 << 7) - 1;
    /* Get start and ending days */
    c = strchr(dow, '-');
    if (c)
    {
        *c = '\0';
        c++;
    }
    else
    {
        c = NULL;
    }
    /* Find the start */
    s = 0;
    while ((s < 7)  &&  strcasecmp(dow, days[s]))
        s++;
    if (s >= 7)
    {
        cw_log(LOG_WARNING, "Invalid day '%s', assuming none\n", dow);
        return 0;
    }
    if (c)
    {
        e = 0;
        while ((e < 7)  &&  strcasecmp(c, days[e]))
            e++;
        if (e >= 7)
        {
            cw_log(LOG_WARNING, "Invalid day '%s', assuming none\n", c);
            return 0;
        }
    }
    else
    {
        e = s;
    }
    mask = 0;
    for (x = s;  x != e;  x = (x + 1)%7)
        mask |= (1 << x);
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
    if (cw_strlen_zero(day)  ||  !strcmp(day, "*"))
    {
        mask = (1 << 30)  + ((1 << 30) - 1);
        return mask;
    }
    /* Get start and ending days */
    c = strchr(day, '-');
    if (c)
    {
        *c = '\0';
        c++;
    }
    /* Find the start */
    if (sscanf(day, "%d", &s) != 1)
    {
        cw_log(LOG_WARNING, "Invalid day '%s', assuming none\n", day);
        return 0;
    }
    if ((s < 1)  ||  (s > 31))
    {
        cw_log(LOG_WARNING, "Invalid day '%s', assuming none\n", day);
        return 0;
    }
    s--;
    if (c)
    {
        if (sscanf(c, "%d", &e) != 1)
        {
            cw_log(LOG_WARNING, "Invalid day '%s', assuming none\n", c);
            return 0;
        }
        if ((e < 1) || (e > 31))
        {
            cw_log(LOG_WARNING, "Invalid day '%s', assuming none\n", c);
            return 0;
        }
        e--;
    }
    else
    {
        e = s;
    }
    mask = 0;
    for (x = s;  x != e;  x = (x + 1)%31)
        mask |= (1 << x);
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
    if (cw_strlen_zero(mon) || !strcmp(mon, "*")) 
        return (1 << 12) - 1;
    /* Get start and ending days */
    c = strchr(mon, '-');
    if (c)
    {
        *c = '\0';
        c++;
    }
    /* Find the start */
    s = 0;
    while((s < 12) && strcasecmp(mon, months[s]))
        s++;
    if (s >= 12)
    {
        cw_log(LOG_WARNING, "Invalid month '%s', assuming none\n", mon);
        return 0;
    }
    if (c)
    {
        e = 0;
        while ((e < 12)  &&  strcasecmp(mon, months[e]))
            e++;
        if (e >= 12)
        {
            cw_log(LOG_WARNING, "Invalid month '%s', assuming none\n", c);
            return 0;
        }
    }
    else
    {
        e = s;
    }
    mask = 0;
    for (x = s;  x != e;  x = (x + 1)%12)
    {
        mask |= (1 << x);
    }
    /* One last one */
    mask |= (1 << x);
    return mask;
}

int cw_build_timing(struct cw_timing *i, char *info_in)
{
    char info_save[256];
    char *info;
    char *c;

    /* Check for empty just in case */
    if (cw_strlen_zero(info_in))
        return 0;
    /* make a copy just in case we were passed a static string */
    cw_copy_string(info_save, info_in, sizeof(info_save));
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

int cw_check_timing(struct cw_timing *i)
{
    struct tm tm;
    time_t t;

    time(&t);
    localtime_r(&t,&tm);

    /* If it's not the right month, return */
    if (!(i->monthmask & (1 << tm.tm_mon)))
    {
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
    if ((tm.tm_hour < 0)  ||  (tm.tm_hour > 23))
    {
        cw_log(LOG_WARNING, "Insane time...\n");
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
int cw_context_add_include2(struct cw_context *con,
                              const char *value,
                              const char *registrar)
{
    struct cw_include *new_include;
    char *c;
    struct cw_include *i, *il = NULL; /* include, include_last */
    int length;
    char *p;
    
    length = sizeof(struct cw_include);
    length += 2 * (strlen(value) + 1);

    /* allocate new include structure ... */
    if (!(new_include = malloc(length)))
    {
        cw_log(LOG_ERROR, "Out of memory\n");
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
    while (*c  &&  (*c != ',')) 
        c++; 
    /* Process if it's there */
    if (*c)
    {
            new_include->hastime = cw_build_timing(&(new_include->timing), c+1);
        *c = '\0';
    }
    new_include->next      = NULL;
    new_include->registrar = registrar;

    /* ... try to lock this context ... */
    if (cw_mutex_lock(&con->lock))
    {
        free(new_include);
        errno = EBUSY;
        return -1;
    }

    /* ... go to last include and check if context is already included too... */
    i = con->includes;
    while (i)
    {
        if (!strcasecmp(i->name, new_include->name))
        {
            free(new_include);
            cw_mutex_unlock(&con->lock);
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
        cw_verbose(VERBOSE_PREFIX_3 "Including context '%s' in context '%s'\n", new_include->name, cw_get_context_name(con)); 
    cw_mutex_unlock(&con->lock);

    return 0;
}

/*
 * errno values
 *  EBUSY  - can't lock
 *  ENOENT - no existence of context
 */
int cw_context_add_switch(const char *context, const char *sw, const char *data, int eval, const char *registrar)
{
    struct cw_context *c;
    unsigned int hash = cw_hash_string(context);
    
    if (cw_lock_contexts())
    {
        errno = EBUSY;
        return -1;
    }

    /* walk contexts ... */
    c = cw_walk_contexts(NULL);
    while (c)
    {
        /* ... search for the right one ... */
        if (hash == c->hash)
        {
            int ret = cw_context_add_switch2(c, sw, data, eval, registrar);
            /* ... unlock contexts list and return */
            cw_unlock_contexts();
            return ret;
        }
        c = cw_walk_contexts(c);
    }

    /* we can't find the right context */
    cw_unlock_contexts();
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
int cw_context_add_switch2(struct cw_context *con, const char *value,
    const char *data, int eval, const char *registrar)
{
    struct cw_sw *new_sw;
    struct cw_sw *i, *il = NULL; /* sw, sw_last */
    int length;
    char *p;
    
    length = sizeof(struct cw_sw);
    length += strlen(value) + 1;
    if (data)
        length += strlen(data);
    length++;
    if (eval)
    {
        /* Create buffer for evaluation of variables */
        length += SWITCH_DATA_LENGTH;
        length++;
    }

    /* allocate new sw structure ... */
    if (!(new_sw = malloc(length)))
    {
        cw_log(LOG_ERROR, "Out of memory\n");
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
    if (data)
    {
        strcpy(new_sw->data, data);
        p += strlen(data) + 1;
    }
    else
    {
        strcpy(new_sw->data, "");
        p++;
    }
    if (eval) 
        new_sw->tmpdata = p;
    new_sw->next      = NULL;
    new_sw->eval      = eval;
    new_sw->registrar = registrar;

    /* ... try to lock this context ... */
    if (cw_mutex_lock(&con->lock))
    {
        free(new_sw);
        errno = EBUSY;
        return -1;
    }

    /* ... go to last sw and check if context is already swd too... */
    i = con->alts;
    while (i)
    {
        if (!strcasecmp(i->name, new_sw->name) && !strcasecmp(i->data, new_sw->data))
        {
            free(new_sw);
            cw_mutex_unlock(&con->lock);
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
        cw_verbose(VERBOSE_PREFIX_3 "Including switch '%s/%s' in context '%s'\n", new_sw->name, new_sw->data, cw_get_context_name(con)); 
    cw_mutex_unlock(&con->lock);

    return 0;
}

/*
 * EBUSY  - can't lock
 * ENOENT - there is not context existence
 */
int cw_context_remove_ignorepat(const char *context, const char *ignorepat, const char *registrar)
{
    struct cw_context *c;
    unsigned int hash = cw_hash_string(context);

    if (cw_lock_contexts())
    {
        errno = EBUSY;
        return -1;
    }

    c = cw_walk_contexts(NULL);
    while (c)
    {
        if (hash == c->hash)
        {
            int ret = cw_context_remove_ignorepat2(c, ignorepat, registrar);
            cw_unlock_contexts();
            return ret;
        }
        c = cw_walk_contexts(c);
    }

    cw_unlock_contexts();
    errno = ENOENT;
    return -1;
}

int cw_context_remove_ignorepat2(struct cw_context *con, const char *ignorepat, const char *registrar)
{
    struct cw_ignorepat *ip, *ipl = NULL;

    if (cw_mutex_lock(&con->lock))
    {
        errno = EBUSY;
        return -1;
    }

    ip = con->ignorepats;
    while (ip)
    {
        if (!strcmp(ip->pattern, ignorepat)
            &&
            (!registrar || (registrar == ip->registrar)))
        {
            if (ipl)
            {
                ipl->next = ip->next;
                free(ip);
            }
            else
            {
                con->ignorepats = ip->next;
                free(ip);
            }
            cw_mutex_unlock(&con->lock);
            return 0;
        }
        ipl = ip;
        ip = ip->next;
    }

    cw_mutex_unlock(&con->lock);
    errno = EINVAL;
    return -1;
}

/*
 * EBUSY - can't lock
 * ENOENT - there is no existence of context
 */
int cw_context_add_ignorepat(const char *con, const char *value, const char *registrar)
{
    struct cw_context *c;
    unsigned int hash = cw_hash_string(con);

    if (cw_lock_contexts())
    {
        errno = EBUSY;
        return -1;
    }

    c = cw_walk_contexts(NULL);
    while (c)
    {
        if (hash == c->hash)
        {
            int ret = cw_context_add_ignorepat2(c, value, registrar);
            cw_unlock_contexts();
            return ret;
        } 
        c = cw_walk_contexts(c);
    }

    cw_unlock_contexts();
    errno = ENOENT;
    return -1;
}

int cw_context_add_ignorepat2(struct cw_context *con, const char *value, const char *registrar)
{
    struct cw_ignorepat *ignorepat, *ignorepatc, *ignorepatl = NULL;
    int length;

    length = sizeof(struct cw_ignorepat);
    length += strlen(value) + 1;
    if ((ignorepat = malloc(length)) == NULL)
    {
        cw_log(LOG_ERROR, "Out of memory\n");
        errno = ENOMEM;
        return -1;
    }
    memset(ignorepat, 0, length);
    strcpy(ignorepat->pattern, value);
    ignorepat->next = NULL;
    ignorepat->registrar = registrar;
    cw_mutex_lock(&con->lock);
    ignorepatc = con->ignorepats;
    while (ignorepatc)
    {
        ignorepatl = ignorepatc;
        if (!strcasecmp(ignorepatc->pattern, value))
        {
            /* Already there */
            cw_mutex_unlock(&con->lock);
            errno = EEXIST;
            return -1;
        }
        ignorepatc = ignorepatc->next;
    }
    if (ignorepatl) 
        ignorepatl->next = ignorepat;
    else
        con->ignorepats = ignorepat;
    cw_mutex_unlock(&con->lock);
    return 0;
    
}

int cw_ignore_pattern(const char *context, const char *pattern)
{
    struct cw_context *con;
    struct cw_ignorepat *pat;

    con = cw_context_find(context);
    if (con)
    {
        pat = con->ignorepats;
        while (pat)
        {
            switch (cw_extension_pattern_match(pattern, pat->pattern))
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
int cw_add_extension(const char *context, int replace, const char *extension, int priority, const char *label, const char *callerid,
    const char *application, void *data, void (*datad)(void *), const char *registrar)
{
    struct cw_context *c;
    unsigned int hash = cw_hash_string(context);

    if (cw_lock_contexts())
    {
        errno = EBUSY;
        return -1;
    }

    c = cw_walk_contexts(NULL);
    while (c)
    {
        if (hash == c->hash)
        {
            int ret = cw_add_extension2(c, replace, extension, priority, label, callerid,
                application, data, datad, registrar);
            cw_unlock_contexts();
            return ret;
        }
        c = cw_walk_contexts(c);
    }

    cw_unlock_contexts();
    errno = ENOENT;
    return -1;
}

int cw_explicit_goto(struct cw_channel *chan, const char *context, const char *exten, int priority)
{
    if (!chan)
        return -1;

    if (!cw_strlen_zero(context))
        cw_copy_string(chan->context, context, sizeof(chan->context));
    if (!cw_strlen_zero(exten))
        cw_copy_string(chan->exten, exten, sizeof(chan->exten));
    if (priority > -1)
    {
        chan->priority = priority;
        /* see flag description in channel.h for explanation */
        if (cw_test_flag(chan, CW_FLAG_IN_AUTOLOOP))
            chan->priority--;
    }
    
    return 0;
}

int cw_explicit_gotolabel(struct cw_channel *chan, const char *context, const char *exten, const char *priority)
{
    int npriority;
    
    if (!chan || !priority || !*priority)
        return -1;

    if (exten && (!*exten || cw_hash_app_name(exten) == CW_KEYWORD_BYEXTENSION))
        exten = NULL;

    if (isdigit(*priority) || ((*priority == '+' || *priority == '-') && isdigit(priority[1]))) {
        switch (*priority) {
	    case '-':
		    npriority = chan->priority - atoi(priority+1);
		    break;
	    case '+':
		    npriority = chan->priority + atoi(priority+1);
		    break;
	    default:
		    npriority = atoi(priority);
		    break;
        }
    } else {
        if ((npriority = cw_findlabel_extension(chan,
		((context && *context) ?  context  :  chan->context),
		((exten && *exten) ? exten : chan->exten),
		priority, chan->cid.cid_num)) < 1
	) {
            cw_log(LOG_WARNING, "Priority '%s' must be [+-]number, or a valid label\n", priority);
            return -1;
        }
    }

    return cw_explicit_goto(chan, context, exten, npriority);
}

int cw_async_goto(struct cw_channel *chan, const char *context, const char *exten, int priority)
{
    int res = 0;

    cw_mutex_lock(&chan->lock);

    if (chan->pbx)
    {
        /* This channel is currently in the PBX */
        cw_explicit_goto(chan, context, exten, priority);
        cw_softhangup_nolock(chan, CW_SOFTHANGUP_ASYNCGOTO);
    }
    else
    {
        /* In order to do it when the channel doesn't really exist within
           the PBX, we have to make a new channel, masquerade, and start the PBX
           at the new location */
        struct cw_channel *tmpchan;
        
        tmpchan = cw_channel_alloc(0);
        if (tmpchan)
        {
            snprintf(tmpchan->name, sizeof(tmpchan->name), "AsyncGoto/%s", chan->name);
            cw_setstate(tmpchan, chan->_state);
            /* Make formats okay */
            tmpchan->readformat = chan->readformat;
            tmpchan->writeformat = chan->writeformat;
            /* Setup proper location */
            cw_explicit_goto(tmpchan,
                               (!cw_strlen_zero(context)) ? context : chan->context,
                               (!cw_strlen_zero(exten)) ? exten : chan->exten,
                               priority);

            /* Masquerade into temp channel */
            cw_channel_masquerade(tmpchan, chan);
        
            /* Grab the locks and get going */
            cw_mutex_lock(&tmpchan->lock);
            cw_do_masquerade(tmpchan);
            cw_mutex_unlock(&tmpchan->lock);
            /* Start the PBX going on our stolen channel */
            if (cw_pbx_start(tmpchan))
            {
                cw_log(LOG_WARNING, "Unable to start PBX on %s\n", tmpchan->name);
                cw_hangup(tmpchan);
                res = -1;
            }
        }
        else
        {
            res = -1;
        }
    }
    cw_mutex_unlock(&chan->lock);
    return res;
}

int cw_async_goto_by_name(const char *channame, const char *context, const char *exten, int priority)
{
    struct cw_channel *chan;
    int res = -1;

    chan = cw_get_channel_by_name_locked(channame);
    if (chan)
    {
        res = cw_async_goto(chan, context, exten, priority);
        cw_mutex_unlock(&chan->lock);
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
            /*    otherwise exten => [a-b],1,... doesn't work */
            /*        case '-': */
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
int cw_add_extension2(struct cw_context *con,
                        int replace, const char *extension, int priority, const char *label, const char *callerid,
                        const char *application, void *data, void (*datad)(void *),
                        const char *registrar)
{

#define LOG do {     if (option_debug) {\
        if (tmp->matchcid) { \
            cw_log(LOG_DEBUG, "Added extension '%s' priority %d (CID match '%s') to %s\n", tmp->exten, tmp->priority, tmp->cidmatch, con->name); \
        } else { \
            cw_log(LOG_DEBUG, "Added extension '%s' priority %d to %s\n", tmp->exten, tmp->priority, con->name); \
        } \
    } else if (option_verbose > 2) { \
        if (tmp->matchcid) { \
            cw_verbose( VERBOSE_PREFIX_3 "Added extension '%s' priority %d (CID match '%s')to %s\n", tmp->exten, tmp->priority, tmp->cidmatch, con->name); \
        } else {  \
            cw_verbose( VERBOSE_PREFIX_3 "Added extension '%s' priority %d to %s\n", tmp->exten, tmp->priority, con->name); \
        } \
    } } while(0)

    /*
     * This is a fairly complex routine.  Different extensions are kept
     * in order by the extension number.  Then, extensions of different
     * priorities (same extension) are kept in a list, according to the
     * peer pointer.
     */
    struct cw_exten *tmp, *e, *el = NULL, *ep = NULL;
    int res;
    int length;
    char *p;
    unsigned int hash = cw_hash_string(extension);

    length = sizeof(struct cw_exten);
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
    if ((tmp = malloc(length)))
    {
        memset(tmp, 0, length);
        tmp->hash = hash;
        p = tmp->stuff;
        if (label)
        {
            tmp->label = p;
            strcpy(tmp->label, label);
            p += strlen(label) + 1;
        }
        tmp->exten = p;
        p += ext_strncpy(tmp->exten, extension, strlen(extension) + 1) + 1;
        tmp->priority = priority;
        tmp->cidmatch = p;
        if (callerid)
        {
            p += ext_strncpy(tmp->cidmatch, callerid, strlen(callerid) + 1) + 1;
            tmp->matchcid = 1;
        }
        else
        {
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
    }
    else
    {
        cw_log(LOG_ERROR, "Out of memory\n");
        errno = ENOMEM;
        return -1;
    }
    if (cw_mutex_lock(&con->lock))
    {
        free(tmp);
        /* And properly destroy the data */
        datad(data);
        cw_log(LOG_WARNING, "Failed to lock context '%s' (%#x)\n", con->name, con->hash);
        errno = EBUSY;
        return -1;
    }
    e = con->root;
    while (e)
    {
        /* Make sure patterns are always last! */
        if ((e->exten[0] != '_') && (extension[0] == '_'))
            res = -1;
        else if ((e->exten[0] == '_') && (extension[0] != '_'))
            res = 1;
        else
            res= strcmp(e->exten, extension);
        if (!res)
        {
            if (!e->matchcid  &&  !tmp->matchcid)
                res = 0;
            else if (tmp->matchcid  &&  !e->matchcid)
                res = 1;
            else if (e->matchcid  &&  !tmp->matchcid)
                res = -1;
            else
                res = strcasecmp(e->cidmatch, tmp->cidmatch);
        }
        if (res == 0)
        {
            /* We have an exact match, now we find where we are
               and be sure there's no duplicates */
            while (e)
            {
                if (e->priority == tmp->priority)
                {
                    /* Can't have something exactly the same.  Is this a
                       replacement?  If so, replace, otherwise, bonk. */
                    if (replace)
                    {
                        if (ep)
                        {
                            /* We're in the peer list, insert ourselves */
                            ep->peer = tmp;
                            tmp->peer = e->peer;
                        }
                        else if (el)
                        {
                            /* We're the first extension. Take over e's functions */
                            el->next = tmp;
                            tmp->next = e->next;
                            tmp->peer = e->peer;
                        }
                        else
                        {
                            /* We're the very first extension.  */
                            con->root = tmp;
                            tmp->next = e->next;
                            tmp->peer = e->peer;
                        }
                        if (tmp->priority == PRIORITY_HINT)
                            cw_change_hint(e,tmp);
                        /* Destroy the old one */
                        e->datad(e->data);
                        free(e);
                        cw_mutex_unlock(&con->lock);
                        if (tmp->priority == PRIORITY_HINT)
                            cw_change_hint(e, tmp);
                        /* And immediately return success. */
                        LOG;
                        return 0;
                    }
                    else
                    {
                        cw_log(LOG_WARNING, "Unable to register extension '%s', priority %d in '%s' (%#x), already in use\n",
                                 tmp->exten, tmp->priority, con->name, con->hash);
                        tmp->datad(tmp->data);
                        free(tmp);
                        cw_mutex_unlock(&con->lock);
                        errno = EEXIST;
                        return -1;
                    }
                }
                else if (e->priority > tmp->priority)
                {
                    /* Slip ourselves in just before e */
                    if (ep)
                    {
                        /* Easy enough, we're just in the peer list */
                        ep->peer = tmp;
                        tmp->peer = e;
                    }
                    else if (el)
                    {
                        /* We're the first extension in this peer list */
                        el->next = tmp;
                        tmp->next = e->next;
                        e->next = NULL;
                        tmp->peer = e;
                    }
                    else
                    {
                        /* We're the very first extension altogether */
                        tmp->next = con->root->next;
                        /* Con->root must always exist or we couldn't get here */
                        tmp->peer = con->root;
                        con->root = tmp;
                    }
                    cw_mutex_unlock(&con->lock);

                    /* And immediately return success. */
                    if (tmp->priority == PRIORITY_HINT)
                         cw_add_hint(tmp);
                    
                    LOG;
                    return 0;
                }
                ep = e;
                e = e->peer;
            }
            /* If we make it here, then it's time for us to go at the very end.
               ep *must* be defined or we couldn't have gotten here. */
            ep->peer = tmp;
            cw_mutex_unlock(&con->lock);
            if (tmp->priority == PRIORITY_HINT)
                cw_add_hint(tmp);
            
            /* And immediately return success. */
            LOG;
            return 0;
        }
        else if (res > 0)
        {
            /* Insert ourselves just before 'e'.  We're the first extension of
               this kind */
            tmp->next = e;
            if (el)
            {
                /* We're in the list somewhere */
                el->next = tmp;
            }
            else
            {
                /* We're at the top of the list */
                con->root = tmp;
            }
            cw_mutex_unlock(&con->lock);
            if (tmp->priority == PRIORITY_HINT)
                cw_add_hint(tmp);

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
    cw_mutex_unlock(&con->lock);
    if (tmp->priority == PRIORITY_HINT)
        cw_add_hint(tmp);
    LOG;
    return 0;    
}

struct async_stat
{
    pthread_t p;
    struct cw_channel *chan;
    char context[CW_MAX_CONTEXT];
    char exten[CW_MAX_EXTENSION];
    int priority;
    int timeout;
    char app[CW_MAX_EXTENSION];
    char appdata[1024];
};

static void *async_wait(void *data) 
{
    struct async_stat *as = data;
    struct cw_channel *chan = as->chan;
    int timeout = as->timeout;
    int res;
    struct cw_frame *f;
    struct cw_app *app;
    
    while (timeout  &&  (chan->_state != CW_STATE_UP))
    {
        res = cw_waitfor(chan, timeout);
        if (res < 1) 
            break;
        if (timeout > -1)
            timeout = res;
        f = cw_read(chan);
        if (!f)
            break;
        if (f->frametype == CW_FRAME_CONTROL)
        {
            if ((f->subclass == CW_CONTROL_BUSY)
                ||
                (f->subclass == CW_CONTROL_CONGESTION))
            {
                break;
            }
        }
        cw_fr_free(f);
    }
    if (chan->_state == CW_STATE_UP)
    {
        if (!cw_strlen_zero(as->app))
        {
            app = pbx_findapp(as->app);
            if (app)
            {
                if (option_verbose > 2)
                    cw_verbose(VERBOSE_PREFIX_3 "Launching %s(%s) on %s\n", as->app, as->appdata, chan->name);
                pbx_exec(chan, app, as->appdata);
            }
            else
            {
                cw_log(LOG_WARNING, "No such application '%s'\n", as->app);
                
            }
        }
        else
        {
            if (!cw_strlen_zero(as->context))
                cw_copy_string(chan->context, as->context, sizeof(chan->context));
            if (!cw_strlen_zero(as->exten))
                cw_copy_string(chan->exten, as->exten, sizeof(chan->exten));
            if (as->priority > 0)
                chan->priority = as->priority;
            /* Run the PBX */
            if (cw_pbx_run(chan))
            {
                cw_log(LOG_ERROR, "Failed to start PBX on %s\n", chan->name);
            }
            else
            {
                /* PBX will have taken care of this */
                chan = NULL;
            }
        }
            
    }
    free(as);
    if (chan)
        cw_hangup(chan);
    return NULL;
}

/*! Function to update the cdr after a spool call fails.
 *
 *  This function updates the cdr for a failed spool call
 *  and takes the channel of the failed call as an argument.
 *
 * \param chan the channel for the failed call.
 */
int cw_pbx_outgoing_cdr_failed(void)
{
    /* allocate a channel */
    struct cw_channel *chan = cw_channel_alloc(0);

    if (!chan)
    {
        /* allocation of the channel failed, let some peeps know */
        cw_log(LOG_WARNING, "Unable to allocate channel structure for CDR record\n");
        return -1;  /* failure */
    }

    chan->cdr = cw_cdr_alloc();   /* allocate a cdr for the channel */

    if (!chan->cdr)
    {
        /* allocation of the cdr failed */
        cw_log(LOG_WARNING, "Unable to create Call Detail Record\n");
        cw_channel_free(chan);   /* free the channel */
        return -1;                /* return failure */
    }
    
    /* allocation of the cdr was successful */
    cw_cdr_init(chan->cdr, chan);  /* initilize our channel's cdr */
    cw_cdr_start(chan->cdr);       /* record the start and stop time */
    cw_cdr_end(chan->cdr);
    cw_cdr_failed(chan->cdr);      /* set the status to failed */
    cw_cdr_detach(chan->cdr);      /* post and free the record */
    cw_channel_free(chan);         /* free the channel */
    
    return 0;  /* success */
}

int cw_pbx_outgoing_exten(const char *type, int format, void *data, int timeout, const char *context, const char *exten, int priority, int *reason, int sync, const char *cid_num, const char *cid_name, struct cw_variable *vars, struct cw_channel **channel)
{
    struct cw_channel *chan;
    struct async_stat *as;
    int res = -1, cdr_res = -1;
    struct outgoing_helper oh;
    pthread_attr_t attr;

    if (sync)
    {
        LOAD_OH(oh);
        chan = __cw_request_and_dial(type, format, data, timeout, reason, cid_num, cid_name, &oh);
        if (channel)
        {
            *channel = chan;
            if (chan)
                cw_mutex_lock(&chan->lock);
        }
        if (chan)
        {
            if (chan->cdr)
            {
                /* check if the channel already has a cdr record, if not give it one */
                cw_log(LOG_WARNING, "%s already has a call record??\n", chan->name);
            }
            else
            {
                chan->cdr = cw_cdr_alloc();   /* allocate a cdr for the channel */
                if (!chan->cdr)
                {
                    /* allocation of the cdr failed */
                    cw_log(LOG_WARNING, "Unable to create Call Detail Record\n");
                    free(chan->pbx);
                    cw_variables_destroy(vars);
                    return -1;
                }
                /* allocation of the cdr was successful */
                cw_cdr_init(chan->cdr, chan);  /* initilize our channel's cdr */
                cw_cdr_start(chan->cdr);
            }
            if (chan->_state == CW_STATE_UP)
            {
                res = 0;
                if (option_verbose > 3)
                    cw_verbose(VERBOSE_PREFIX_4 "Channel %s was answered.\n", chan->name);

                if (sync > 1)
                {
                    if (channel)
                        cw_mutex_unlock(&chan->lock);
                    if (cw_pbx_run(chan))
                    {
                        cw_log(LOG_ERROR, "Unable to run PBX on %s\n", chan->name);
                        if (channel)
                            *channel = NULL;
                        cw_hangup(chan);
                        res = -1;
                    }
                }
                else
                {
                    if (cw_pbx_start(chan))
                    {
                        cw_log(LOG_ERROR, "Unable to start PBX on %s\n", chan->name);
                        if (channel)
                            *channel = NULL;
                        cw_hangup(chan);
                        res = -1;
                    } 
                }
            }
            else
            {
                if (option_verbose > 3)
                    cw_verbose(VERBOSE_PREFIX_4 "Channel %s was never answered.\n", chan->name);

                if (chan->cdr)
                {
                    /* update the cdr */
                    /* here we update the status of the call, which sould be busy.
                     * if that fails then we set the status to failed */
                    if (cw_cdr_disposition(chan->cdr, chan->hangupcause))
                        cw_cdr_failed(chan->cdr);
                }
            
                if (channel)
                    *channel = NULL;
                cw_hangup(chan);
            }
        }

        if (res < 0)
        {
            /* the call failed for some reason */
            if (*reason == 0)
            {
                /* if the call failed (not busy or no answer)
                 * update the cdr with the failed message */
                cdr_res = cw_pbx_outgoing_cdr_failed();
                if (cdr_res != 0)
                {
                    cw_variables_destroy(vars);
                    return cdr_res;
                }
            }
            
            /* create a fake channel and execute the "failed" extension (if it exists) within the requested context */
            /* check if "failed" exists */
            if (cw_exists_extension(chan, context, "failed", 1, NULL))
            {
                chan = cw_channel_alloc(0);
                if (chan)
                {
                    cw_copy_string(chan->name, "OutgoingSpoolFailed", sizeof(chan->name));
                    if (!cw_strlen_zero(context))
                        cw_copy_string(chan->context, context, sizeof(chan->context));
                    cw_copy_string(chan->exten, "failed", sizeof(chan->exten));
                    chan->priority = 1;
                    cw_set_variables(chan, vars);
                    cw_pbx_run(chan);    
                }
                else
                {
                    cw_log(LOG_WARNING, "Can't allocate the channel structure, skipping execution of extension 'failed'\n");
                }
            }
        }
    }
    else
    {
        if ((as = malloc(sizeof(struct async_stat))) == NULL)
        {
            cw_variables_destroy(vars);
            return -1;
        }    
        memset(as, 0, sizeof(struct async_stat));
        chan = cw_request_and_dial(type, format, data, timeout, reason, cid_num, cid_name);
        if (channel)
        {
            *channel = chan;
            if (chan)
                cw_mutex_lock(&chan->lock);
        }
        if (!chan)
        {
            free(as);
            cw_variables_destroy(vars);
            return -1;
        }
        as->chan = chan;
        cw_copy_string(as->context, context, sizeof(as->context));
        cw_copy_string(as->exten,  exten, sizeof(as->exten));
        as->priority = priority;
        as->timeout = timeout;
        cw_set_variables(chan, vars);
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (cw_pthread_create(&as->p, &attr, async_wait, as))
        {
            cw_log(LOG_WARNING, "Failed to start async wait\n");
            free(as);
            if (channel)
                *channel = NULL;
            cw_hangup(chan);
            cw_variables_destroy(vars);
            return -1;
        }
        res = 0;
    }
    cw_variables_destroy(vars);
    return res;
}

struct app_tmp
{
    char app[256];
    char data[256];
    struct cw_channel *chan;
    pthread_t t;
};

static void *cw_pbx_run_app(void *data)
{
    struct app_tmp *tmp = data;
    struct cw_app *app;

    app = pbx_findapp(tmp->app);
    if (app)
    {
        if (option_verbose > 3)
            cw_verbose(VERBOSE_PREFIX_4 "Launching %s(%s) on %s\n", tmp->app, tmp->data, tmp->chan->name);
        pbx_exec(tmp->chan, app, tmp->data);
    }
    else
    {
        cw_log(LOG_WARNING, "No such application '%s'\n", tmp->app);
    }
    cw_hangup(tmp->chan);
    free(tmp);
    return NULL;
}

int cw_pbx_outgoing_app(const char *type, int format, void *data, int timeout, const char *app, const char *appdata, int *reason, int sync, const char *cid_num, const char *cid_name, struct cw_variable *vars, struct cw_channel **locked_channel)
{
    struct cw_channel *chan;
    struct async_stat *as;
    struct app_tmp *tmp;
    int res = -1, cdr_res = -1;
    struct outgoing_helper oh;
    pthread_attr_t attr;
    
    memset(&oh, 0, sizeof(oh));
    oh.vars = vars;    

    if (locked_channel) 
        *locked_channel = NULL;
    if (cw_strlen_zero(app))
    {
           cw_variables_destroy(vars);
        return -1;
    }
    if (sync)
    {
        chan = __cw_request_and_dial(type, format, data, timeout, reason, cid_num, cid_name, &oh);
        if (chan)
        {
            if (chan->cdr)
            {
                /* check if the channel already has a cdr record, if not give it one */
                cw_log(LOG_WARNING, "%s already has a call record??\n", chan->name);
            }
            else
            {
                chan->cdr = cw_cdr_alloc();   /* allocate a cdr for the channel */
                if (!chan->cdr)
                {
                    /* allocation of the cdr failed */
                    cw_log(LOG_WARNING, "Unable to create Call Detail Record\n");
                    free(chan->pbx);
                       cw_variables_destroy(vars);
                    return -1;
                }
                /* allocation of the cdr was successful */
                cw_cdr_init(chan->cdr, chan);  /* initilize our channel's cdr */
                cw_cdr_start(chan->cdr);
            }
            cw_set_variables(chan, vars);
            if (chan->_state == CW_STATE_UP)
            {
                res = 0;
                if (option_verbose > 3)
                    cw_verbose(VERBOSE_PREFIX_4 "Channel %s was answered.\n", chan->name);
                if ((tmp = malloc(sizeof(struct app_tmp))))
                {
                    memset(tmp, 0, sizeof(struct app_tmp));
                    cw_copy_string(tmp->app, app, sizeof(tmp->app));
                    if (appdata)
                        cw_copy_string(tmp->data, appdata, sizeof(tmp->data));
                    tmp->chan = chan;
                    if (sync > 1)
                    {
                        if (locked_channel)
                            cw_mutex_unlock(&chan->lock);
                        cw_pbx_run_app(tmp);
                    }
                    else
                    {
                        pthread_attr_init(&attr);
                        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                        if (locked_channel) 
                            cw_mutex_lock(&chan->lock);
                        if (cw_pthread_create(&tmp->t, &attr, cw_pbx_run_app, tmp))
                        {
                            cw_log(LOG_WARNING, "Unable to spawn execute thread on %s: %s\n", chan->name, strerror(errno));
                            free(tmp);
                            if (locked_channel) 
                                cw_mutex_unlock(&chan->lock);
                            cw_hangup(chan);
                            res = -1;
                        }
                        else
                        {
                            if (locked_channel) 
                                *locked_channel = chan;
                        }
                    }
                }
                else
                {
                    cw_log(LOG_ERROR, "Out of memory :(\n");
                    res = -1;
                }
            }
            else
            {
                if (option_verbose > 3)
                    cw_verbose(VERBOSE_PREFIX_4 "Channel %s was never answered.\n", chan->name);
                if (chan->cdr)
                {
                    /* update the cdr */
                    /* here we update the status of the call, which sould be busy.
                     * if that fails then we set the status to failed */
                    if (cw_cdr_disposition(chan->cdr, chan->hangupcause))
                        cw_cdr_failed(chan->cdr);
                }
                cw_hangup(chan);
            }
        }
        
        if (res < 0)
        {
            /* the call failed for some reason */
            if (*reason == 0)
            {
                /* if the call failed (not busy or no answer)
                 * update the cdr with the failed message */
                cdr_res = cw_pbx_outgoing_cdr_failed();
                if (cdr_res != 0)
                {
                    cw_variables_destroy(vars);
                    return cdr_res;
                }
            }
        }

    }
    else
    {
        if ((as = malloc(sizeof(struct async_stat))) == NULL)
        {
            cw_variables_destroy(vars);
            return -1;
        }
        memset(as, 0, sizeof(struct async_stat));
        chan = cw_request_and_dial(type, format, data, timeout, reason, cid_num, cid_name);
        if (!chan)
        {
            free(as);
            cw_variables_destroy(vars);
            return -1;
        }
        as->chan = chan;
        cw_copy_string(as->app, app, sizeof(as->app));
        if (appdata)
            cw_copy_string(as->appdata,  appdata, sizeof(as->appdata));
        as->timeout = timeout;
        cw_set_variables(chan, vars);
        /* Start a new thread, and get something handling this channel. */
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (locked_channel) 
            cw_mutex_lock(&chan->lock);
        if (cw_pthread_create(&as->p, &attr, async_wait, as))
        {
            cw_log(LOG_WARNING, "Failed to start async wait\n");
            free(as);
            if (locked_channel) 
                cw_mutex_unlock(&chan->lock);
            cw_hangup(chan);
            cw_variables_destroy(vars);
            return -1;
        }
        if (locked_channel)
            *locked_channel = chan;
        res = 0;
    }
    cw_variables_destroy(vars);
    return res;
}

static void destroy_exten(struct cw_exten *e)
{
    if (e->priority == PRIORITY_HINT)
        cw_remove_hint(e);

    if (e->datad)
        e->datad(e->data);
    free(e);
}

void __cw_context_destroy(struct cw_context *con, const char *registrar)
{
    struct cw_context *tmp, *tmpl=NULL;
    struct cw_include *tmpi, *tmpil= NULL;
    struct cw_sw *sw, *swl= NULL;
    struct cw_exten *e, *el, *en;
    struct cw_ignorepat *ipi, *ipl = NULL;

    cw_mutex_lock(&conlock);
    tmp = contexts;
    while (tmp)
    {
        if (((con  &&  (tmp->hash == con->hash))  ||  !con)
            &&
            (!registrar ||  !strcasecmp(registrar, tmp->registrar)))
        {
            /* Okay, let's lock the structure to be sure nobody else
               is searching through it. */
            if (cw_mutex_lock(&tmp->lock))
            {
                cw_log(LOG_WARNING, "Unable to lock context lock\n");
                return;
            }
            if (tmpl)
                tmpl->next = tmp->next;
            else
                contexts = tmp->next;
            /* Okay, now we're safe to let it go -- in a sense, we were
               ready to let it go as soon as we locked it. */
            cw_mutex_unlock(&tmp->lock);
            for (tmpi = tmp->includes;  tmpi;  )
            {
                /* Free includes */
                tmpil = tmpi;
                tmpi = tmpi->next;
                free(tmpil);
            }
            for (ipi = tmp->ignorepats;  ipi;  )
            {
                /* Free ignorepats */
                ipl = ipi;
                ipi = ipi->next;
                free(ipl);
            }
            for (sw = tmp->alts;  sw;  )
            {
                /* Free switches */
                swl = sw;
                sw = sw->next;
                free(swl);
                swl = sw;
            }
            for (e = tmp->root;  e;  )
            {
                for (en = e->peer;  en;  )
                {
                    el = en;
                    en = en->peer;
                    destroy_exten(el);
                }
                el = e;
                e = e->next;
                destroy_exten(el);
            }
            cw_mutex_destroy(&tmp->lock);
            free(tmp);
            if (!con)
            {
                /* Might need to get another one -- restart */
                tmp = contexts;
                tmpl = NULL;
                tmpil = NULL;
                continue;
            }
            cw_mutex_unlock(&conlock);
            return;
        }
        tmpl = tmp;
        tmp = tmp->next;
    }
    cw_mutex_unlock(&conlock);
}

void cw_context_destroy(struct cw_context *con, const char *registrar)
{
    __cw_context_destroy(con,registrar);
}

static void wait_for_hangup(struct cw_channel *chan, void *data)
{
    int res;
    struct cw_frame *f;
    int waittime;
    
    if (!data || !strlen(data) || (sscanf(data, "%d", &waittime) != 1) || (waittime < 0))
        waittime = -1;
    if (waittime > -1)
    {
        cw_safe_sleep(chan, waittime * 1000);
    }
    else
    {
        do
        {
            res = cw_waitfor(chan, -1);
            if (res < 0)
                return;
            f = cw_read(chan);
            if (f)
                cw_fr_free(f);
        }
        while(f);
    }
}

static int pbx_builtin_progress(struct cw_channel *chan, int argc, char **argv)
{
    cw_indicate(chan, CW_CONTROL_PROGRESS);
    return 0;
}

static int pbx_builtin_ringing(struct cw_channel *chan, int argc, char **argv)
{
    cw_indicate(chan, CW_CONTROL_RINGING);
    return 0;
}

static int pbx_builtin_busy(struct cw_channel *chan, int argc, char **argv)
{
    cw_indicate(chan, CW_CONTROL_BUSY);        
    if (chan->_state != CW_STATE_UP)
        cw_setstate(chan, CW_STATE_BUSY);
    wait_for_hangup(chan, (argc > 0 ? argv[0] : NULL));
    return -1;
}

static int pbx_builtin_congestion(struct cw_channel *chan, int argc, char **argv)
{
    cw_indicate(chan, CW_CONTROL_CONGESTION);
    if (chan->_state != CW_STATE_UP)
        cw_setstate(chan, CW_STATE_BUSY);
    wait_for_hangup(chan, (argc > 0 ? argv[0] : NULL));
    return -1;
}

static int pbx_builtin_answer(struct cw_channel *chan, int argc, char **argv)
{
    int delay = (argc > 0 ? atoi(argv[0]) : 0);
    int res;
    
    if (chan->_state == CW_STATE_UP)
        delay = 0;
    res = cw_answer(chan);
    if (res)
        return res;
    if (delay)
        res = cw_safe_sleep(chan, delay);
    return res;
}

static int pbx_builtin_setlanguage(struct cw_channel *chan, int argc, char **argv)
{
    static int deprecation_warning = 0;

    if (!deprecation_warning)
    {
        cw_log(LOG_WARNING, "SetLanguage is deprecated, please use Set(LANGUAGE()=language) instead.\n");
        deprecation_warning = 1;
    }

    /* Copy the language as specified */
    if (argc > 0)
        cw_copy_string(chan->language, argv[0], sizeof(chan->language));

    return 0;
}

static int pbx_builtin_resetcdr(struct cw_channel *chan, int argc, char **argv)
{
	char *p;
	int flags = 0;

	for (; argc; argv++, argc--) {
		for (p = argv[0]; *p; p++) {
			switch (*p) {
				case 'a':
					flags |= CW_CDR_FLAG_LOCKED;
					break;
				case 'v':
					flags |= CW_CDR_FLAG_KEEP_VARS;
					break;
				case 'w':
					flags |= CW_CDR_FLAG_POSTED;
					break;
			}
		}
	}

	cw_cdr_reset(chan->cdr, flags);
	return 0;
}

static int pbx_builtin_setaccount(struct cw_channel *chan, int argc, char **argv)
{
	cw_cdr_setaccount(chan, (argc > 0 ? argv[0] : ""));
	return 0;
}

static int pbx_builtin_setamaflags(struct cw_channel *chan, int argc, char **argv)
{
	cw_cdr_setamaflags(chan, (argc > 0 ? argv[0] : ""));
	return 0;
}

static int pbx_builtin_hangup(struct cw_channel *chan, int argc, char **argv)
{
    int n;
    if (argc > 0 && (n = atoi(argv[0])) > 0)
        chan->hangupcause = n;
    /* Just return non-zero and it will hang up */
    return -1;
}

static int pbx_builtin_stripmsd(struct cw_channel *chan, int argc, char **argv)
{
	int n;

	if (argc != 1 || !(n = atoi(argv[0])) || n >= sizeof(chan->exten)) {
		cw_log(LOG_WARNING, "Syntax: StripMSD(n) where 0 < n < %u\n", sizeof(chan->exten));
		return 0;
	}

	memmove(chan->exten, chan->exten + n, sizeof(chan->exten) - n);

	if (option_verbose > 2)
		cw_verbose(VERBOSE_PREFIX_3 "Stripped %d, new extension is %s\n", n, chan->exten);

	return 0;
}

static int pbx_builtin_prefix(struct cw_channel *chan, int argc, char **argv)
{
	for (; argc; argv++, argc--) {
		int n = strlen(argv[0]);
		memmove(chan->exten + n, chan->exten, sizeof(chan->exten) - n - 1);
		memcpy(chan->exten, argv[0], n);
		if (option_verbose > 2)
			cw_verbose(VERBOSE_PREFIX_3 "Prepended prefix, new extension is %s\n", chan->exten);
	}
	return 0;
}

static int pbx_builtin_suffix(struct cw_channel *chan, int argc, char **argv)
{
	int l = strlen(chan->exten);

	for (; argc; argv++, argc--) {
		int n = strlen(argv[0]);
		if (n > sizeof(chan->exten) - l - 1)
			n = sizeof(chan->exten) - l - 1;
		memcpy(chan->exten + l, argv[0], n);
		if (option_verbose > 2)
			cw_verbose(VERBOSE_PREFIX_3 "Appended suffix, new extension is %s\n", chan->exten);
	}
	return 0;
}

static int pbx_builtin_gotoiftime(struct cw_channel *chan, int argc, char **argv)
{
    struct cw_timing timing;
    char *s, *q;

    if (argc < 4 || argc > 6 || !(s = strchr(argv[3], '?'))) {
        cw_log(LOG_WARNING, "GotoIfTime requires an argument:\n  <time range>,<days of week>,<days of month>,<months>?[[context,]extension,]priority\n");
        return -1;
    }

    /* Trim trailing space from the timespec */
    q = s;
    do { *(q--) = '\0'; } while (q >= argv[3] && isspace(*q));

    get_timerange(&timing, argv[0]);
    timing.dowmask = get_dow(argv[1]);
    timing.daymask = get_day(argv[2]);
    timing.monthmask = get_month(argv[3]);

    if (cw_check_timing(&timing)) {
        do { *(s++) = '\0'; } while (isspace(*s));
    	argv[3] = s;
	argv += 3;
    	argc -= 3;
	return pbx_builtin_goto(chan, argc, argv);
    }

    return 0;
}

static int pbx_builtin_execiftime(struct cw_channel *chan, int argc, char **argv)
{
    struct cw_timing timing;
    char *s, *q;

    if (argc < 4 || !(s = strchr(argv[3], '?'))) {
        cw_log(LOG_WARNING, "ExecIfTime requires an argument:\n  <time range>,<days of week>,<days of month>,<months>?<appname>[(<args>)]\n");
        return -1;
    }

    /* Trim trailing space from the timespec */
    q = s;
    do { *(q--) = '\0'; } while (q >= argv[3] && isspace(*q));

    get_timerange(&timing, argv[0]);
    timing.dowmask = get_dow(argv[1]);
    timing.daymask = get_day(argv[2]);
    timing.monthmask = get_month(argv[3]);

    if (cw_check_timing(&timing)) {
        struct cw_app *app;
        do { *(s++) = '\0'; } while (isspace(*s));
        app = pbx_findapp(s);
	if (app) {
		if ((s = strchr(s, '('))) {
			argv[0] = s + 1;
			if ((s = strrchr(s + 1, ')')))
				*s = '\0';
			return pbx_exec(chan, app, argv[0]);
		} else {
			return pbx_exec_argv(chan, app, argc - 4, argv + 4);
		}
	} else {
		cw_log(LOG_WARNING, "Cannot locate application %s\n", s);
		return -1;
	}
    }

    return 0;
}

static int pbx_builtin_wait(struct cw_channel *chan, int argc, char **argv)
{
    double ms;

    /* Wait for "n" seconds */
    if (argc > 0 && (ms = atof(argv[0])))
        return cw_safe_sleep(chan, (int)(ms * 1000.0));
    return 0;
}

static int pbx_builtin_waitexten(struct cw_channel *chan, int argc, char **argv)
{
    struct cw_flags flags = {0};
    char *mohclass = NULL;
    int ms, res;

    if (argc > 1) {
        char *opts[1];

        cw_parseoptions(waitexten_opts, &flags, opts, argv[1]);
        if (cw_test_flag(&flags, WAITEXTEN_MOH))
            mohclass = opts[0];
    }
    
    if (cw_test_flag(&flags, WAITEXTEN_MOH))
        cw_moh_start(chan, mohclass);

    /* Wait for "n" seconds */
    if (argc < 1 || !(ms = (atof(argv[0]) * 1000.0))) 
        ms = (chan->pbx ? chan->pbx->rtimeout * 1000 : 10000);

    res = cw_waitfordigit(chan, ms);
    if (!res)
    {
        if (cw_exists_extension(chan, chan->context, chan->exten, chan->priority + 1, chan->cid.cid_num))
        {
            if (option_verbose > 2)
                cw_verbose(VERBOSE_PREFIX_3 "Timeout on %s, continuing...\n", chan->name);
        }
        else if (cw_exists_extension(chan, chan->context, "t", 1, chan->cid.cid_num))
        {
            if (option_verbose > 2)
                cw_verbose(VERBOSE_PREFIX_3 "Timeout on %s, going to 't'\n", chan->name);
            cw_copy_string(chan->exten, "t", sizeof(chan->exten));
            chan->priority = 0;
        }
        else
        {
            cw_log(LOG_WARNING, "Timeout but no rule 't' in context '%s'\n", chan->context);
            res = -1;
        }
    }

    if (cw_test_flag(&flags, WAITEXTEN_MOH))
        cw_moh_stop(chan);

    return res;
}

static int pbx_builtin_background(struct cw_channel *chan, int argc, char **argv)
{
    int res = 0;
    char *options = NULL; 
    char *filename = NULL;
    char *front = NULL, *back = NULL;
    char *lang = NULL;
    char *context = NULL;
    struct cw_flags flags = {0};
    unsigned int hash = 0;

    switch (argc)
    {
    case 4:
        context = argv[3];
    case 3:
        lang = argv[2];
    case 2:
        options = argv[1];
        hash = cw_hash_app_name(options);
    case 1:
        filename = argv[0];
        break;
    default:
        cw_log(LOG_WARNING, "Background requires an argument (filename)\n");
        return -1;
    }

    if (!lang)
        lang = chan->language;

    if (!context)
        context = chan->context;

    if (options)
    {
        if (hash == CW_KEYWORD_SKIP)
            flags.flags = BACKGROUND_SKIP;
        else if (hash == CW_KEYWORD_NOANSWER)
            flags.flags = BACKGROUND_NOANSWER;
        else
            cw_parseoptions(background_opts, &flags, NULL, options);
    }

    /* Answer if need be */
    if (chan->_state != CW_STATE_UP)
    {
        if (cw_test_flag(&flags, BACKGROUND_SKIP))
            return 0;
        if (!cw_test_flag(&flags, BACKGROUND_NOANSWER))
            res = cw_answer(chan);
    }

    if (!res)
    {
        /* Stop anything playing */
        cw_stopstream(chan);
        /* Stream a file */
        front = filename;
        while (!res  &&  front)
        {
            if ((back = strchr(front, '&')))
            {
                *back = '\0';
                back++;
            }
            res = cw_streamfile(chan, front, lang);
            if (!res)
            {
                if (cw_test_flag(&flags, BACKGROUND_PLAYBACK))
                {
                    res = cw_waitstream(chan, "");
                }
                else
                {
                    if (cw_test_flag(&flags, BACKGROUND_MATCHEXTEN))
                        res = cw_waitstream_exten(chan, context);
                    else
                        res = cw_waitstream(chan, CW_DIGIT_ANY);
                }
                cw_stopstream(chan);
            }
            else
            {
                cw_log(LOG_WARNING, "cw_streamfile failed on %s for %s, %s, %s, %s\n", chan->name, argv[0], argv[1], argv[2], argv[3]);
                res = 0;
                break;
            }
            front = back;
        }
    }
    if (context != chan->context  &&  res)
    {
        snprintf(chan->exten, sizeof(chan->exten), "%c", res);
        cw_copy_string(chan->context, context, sizeof(chan->context));
        chan->priority = 0;
        return 0;
    }
    return res;
}

static int pbx_builtin_atimeout(struct cw_channel *chan, int argc, char **argv)
{
    static int deprecation_warning = 0;
    int x = (argc > 0 ? atoi(argv[0]) : 0);

    if (!deprecation_warning)
    {
        cw_log(LOG_WARNING, "AbsoluteTimeout is deprecated, please use Set(TIMEOUT(absolute)=timeout) instead.\n");
        deprecation_warning = 1;
    }
            
    /* Set the absolute maximum time how long a call can be connected */
    cw_channel_setwhentohangup(chan, x);
    if (option_verbose > 2)
        cw_verbose( VERBOSE_PREFIX_3 "Set Absolute Timeout to %d\n", x);
    return 0;
}

static int pbx_builtin_rtimeout(struct cw_channel *chan, int argc, char **argv)
{
    static int deprecation_warning = 0;

    if (!deprecation_warning)
    {
        cw_log(LOG_WARNING, "ResponseTimeout is deprecated, please use Set(TIMEOUT(response)=timeout) instead.\n");
        deprecation_warning = 1;
    }

    /* If the channel is not in a PBX, return now */
    if (!chan->pbx)
        return 0;

    /* Set the timeout for how long to wait between digits */
    chan->pbx->rtimeout = atoi(argv[0]);
    if (option_verbose > 2)
        cw_verbose( VERBOSE_PREFIX_3 "Set Response Timeout to %d\n", chan->pbx->rtimeout);
    return 0;
}

static int pbx_builtin_dtimeout(struct cw_channel *chan, int argc, char **argv)
{
    static int deprecation_warning = 0;

    if (!deprecation_warning)
    {
        cw_log(LOG_WARNING, "DigitTimeout is deprecated, please use Set(TIMEOUT(digit)=timeout) instead.\n");
        deprecation_warning = 1;
    }

    /* If the channel is not in a PBX, return now */
    if (!chan->pbx)
        return 0;

    /* Set the timeout for how long to wait between digits */
    chan->pbx->dtimeout = atoi(argv[0]);
    if (option_verbose > 2)
        cw_verbose( VERBOSE_PREFIX_3 "Set Digit Timeout to %d\n", chan->pbx->dtimeout);
    return 0;
}

static int pbx_builtin_goto(struct cw_channel *chan, int argc, char **argv)
{
	char *context, *exten;
	int res;

	context = exten = NULL;
	if (argc > 2) context = (argv++)[0];
	if (argc > 1) exten = (argv++)[0];
	res = cw_explicit_gotolabel(chan, context, exten, argv[0]);
	if (!res && option_verbose > 2)
		cw_verbose(VERBOSE_PREFIX_3 "Goto (%s, %s, %d)\n", chan->context, chan->exten, chan->priority + 1);
	return res;
}


int pbx_builtin_serialize_variables(struct cw_channel *chan, char *buf, size_t size) 
{
    struct cw_var_t *variables;
    char *var;
    char *val;
    int total = 0;

    if (!chan)
        return 0;

    memset(buf, 0, size);

    CW_LIST_TRAVERSE(&chan->varshead, variables, entries)
    {
        if ((var = cw_var_name(variables))  &&  (val = cw_var_value(variables)))
        {
            if (cw_build_string(&buf, &size, "%s=%s\n", var, val))
            {
                cw_log(LOG_ERROR, "Data Buffer Size Exceeded!\n");
                break;
            }
            total++;
        }
        else
        {
            break;
        }
    }
    
    return total;
}

char *pbx_builtin_getvar_helper(struct cw_channel *chan, const char *name) 
{
    struct cw_var_t *variables;
    struct varshead *headp;
    unsigned int hash = cw_hash_var_name(name);
    char *ret = NULL;

    if (chan)
        headp = &chan->varshead;
    else
        headp = &globals;

    if (name)
    {
        if (headp == &globals)
            cw_mutex_lock(&globalslock);
        CW_LIST_TRAVERSE(headp,variables,entries)
        {
            if (hash == cw_var_hash(variables))
            {
                ret = cw_var_value(variables);
                break;
            }
        }
        if (headp == &globals)
            cw_mutex_unlock(&globalslock);
        if (ret == NULL && headp != &globals)
        {
            /* Check global variables if we haven't already */
            headp = &globals;
            cw_mutex_lock(&globalslock);
            CW_LIST_TRAVERSE(headp,variables,entries)
            {
                if (hash == cw_var_hash(variables))
                {
                    ret = cw_var_value(variables);
                    break;
                }
            }
            cw_mutex_unlock(&globalslock);
        }
    }
    return ret;
}

void pbx_builtin_pushvar_helper(struct cw_channel *chan, const char *name, const char *value)
{
    struct cw_var_t *newvariable;
    struct varshead *headp;

    if (name[strlen(name)-1] == ')')
    {
        cw_log(LOG_WARNING, "Cannot push a value onto a function\n");
        return cw_func_write(chan, name, value);
    }

    headp = (chan) ? &chan->varshead : &globals;

    if (value)
    {
        if ((option_verbose > 1) && (headp == &globals))
            cw_verbose(VERBOSE_PREFIX_2 "Setting global variable '%s' to '%s'\n", name, value);
        newvariable = cw_var_assign(name, value);      
        if (headp == &globals)
            cw_mutex_lock(&globalslock);
        CW_LIST_INSERT_HEAD(headp, newvariable, entries);
        if (headp == &globals)
            cw_mutex_unlock(&globalslock);
    }
}


void pbx_builtin_setvar_helper(struct cw_channel *chan, const char *name, const char *value)
{
    struct cw_var_t *newvariable;
    struct varshead *headp;
    const char *nametail = name;
    unsigned int hash;

    if (name[strlen(name)-1] == ')')
        return cw_func_write(chan, name, value);

    headp = (chan) ? &chan->varshead : &globals;

    /* For comparison purposes, we have to strip leading underscores */
    if (*nametail == '_')
    {
        nametail++;
        if (*nametail == '_') 
            nametail++;
    }
    
    hash = cw_hash_var_name(nametail);

    if (headp == &globals)
        cw_mutex_lock(&globalslock);

    CW_LIST_TRAVERSE (headp, newvariable, entries)
    {
        if (hash == cw_var_hash(newvariable))
        {
            /* there is already such a variable, delete it */
            CW_LIST_REMOVE(headp, newvariable, entries);
            cw_var_delete(newvariable);
            break;
        }
    } 

    if (value)
    {
        if ((option_verbose > 1) && (headp == &globals))
            cw_verbose(VERBOSE_PREFIX_2 "Setting global variable '%s' to '%s'\n", name, value);
        newvariable = cw_var_assign(name, value);    
        CW_LIST_INSERT_HEAD(headp, newvariable, entries);
    }

    if (headp == &globals)
        cw_mutex_unlock(&globalslock);
}

static int pbx_builtin_setvar_old(struct cw_channel *chan, int argc, char **argv)
{
    static int deprecation_warning = 0;

    if (!deprecation_warning)
    {
        cw_log(LOG_WARNING, "SetVar is deprecated, please use Set instead.\n");
        deprecation_warning = 1;
    }

    return pbx_builtin_setvar(chan, argc, argv);
}

static int pbx_builtin_setvar(struct cw_channel *chan, int argc, char **argv)
{
	if (argc < 1) {
		cw_log(LOG_WARNING, "Set requires at least one variable name/value pair.\n");
		return 0;
	}

	/* check for a trailing flags argument */
	if ((argc > 1)  &&  !strchr(argv[argc-1], '=')) {
		argc--;
		if (strchr(argv[argc], 'g'))
			chan = NULL;
	}

	for (; argc; argv++, argc--) {
 		char *value;
		if ((value = strchr(argv[0], '='))) {
			*(value++) = '\0';
			pbx_builtin_setvar_helper(chan, argv[0], value);
		} else {
			cw_log(LOG_WARNING, "Ignoring entry '%s' with no '=' (and not last 'options' entry)\n", argv[0]);
		}
	}

	return 0;
}

int pbx_builtin_importvar(struct cw_channel *chan, int argc, char **argv)
{
	char tmp[VAR_BUF_SIZE];
	struct cw_channel *chan2;
	char *channel, *s;

	if (argc != 2 || !(channel = strchr(argv[0], '='))) {
		cw_log(LOG_WARNING, "Syntax: ImportVar(newvar=channelname,variable)\n");
		return 0;
	}

	s = channel;
	do { *(s--) = '\0'; } while (isspace(*s));
	do { channel++; } while (isspace(*channel));

	tmp[0] = '\0';
	chan2 = cw_get_channel_by_name_locked(channel);
	if (chan2) {
		if ((s = alloca(strlen(argv[1]) + 4))) {
			sprintf(s, "${%s}", argv[1]);
			pbx_substitute_variables_helper(chan2, s, tmp, sizeof(tmp));
		}
		cw_mutex_unlock(&chan2->lock);
	}
	pbx_builtin_setvar_helper(chan, argv[0], tmp);

	return(0);
}

static int pbx_builtin_setglobalvar(struct cw_channel *chan, int argc, char **argv)
{
	for (; argc; argv++, argc--) {
 		char *value;
		if ((value = strchr(argv[0], '='))) {
			*(value++) = '\0';
			pbx_builtin_setvar_helper(NULL, argv[0], value);
		} else {
			cw_log(LOG_WARNING, "Ignoring entry '%s' with no '='\n", argv[0]);
		}
	}

	return(0);
}

static int pbx_builtin_noop(struct cw_channel *chan, int argc, char **argv)
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
    struct cw_var_t *vardata;
    
    cw_mutex_lock(&globalslock);
    while (!CW_LIST_EMPTY(&globals))
    {
        vardata = CW_LIST_REMOVE_HEAD(&globals, entries);
        cw_var_delete(vardata);
    }
    cw_mutex_unlock(&globalslock);
}

int pbx_checkcondition(char *condition) 
{
    if (condition)
    {
        if (*condition == '\0')
        {
            /* Empty strings are false */
            return 0;
        }
        if (*condition >= '0' && *condition <= '9')
        {
            /* Numbers are evaluated for truth */
            return atoi(condition);
        }
        /* Strings are true */
        return 1;
    }
    /* NULL is also false */
    return 0;
}

static int pbx_builtin_gotoif(struct cw_channel *chan, int argc, char **argv)
{
	char *s, *q;
	int i;

	/* First argument is "<condition ? ..." */
	if (argc > 0) {
		q = s = strchr(argv[0], '?');
		if (s) {
			/* Trim trailing space from the condition */
			do { *(q--) = '\0'; } while (q >= argv[0] && isspace(*q));

			do { *(s++) = '\0'; } while (isspace(*s));

			if (pbx_checkcondition(argv[0])) {
				/* True: we want everything between '?' and ':' */
				argv[0] = s;
				for (i = 0; i < argc; i++) {
					if ((s = strchr(argv[i], ':'))) {
						do { *(s--) = '\0'; } while (s >= argv[i] && isspace(*s));
						argc = i + 1;
						break;
					}
				}
				return (argc != 1 || argv[0][0] ? pbx_builtin_goto(chan, argc, argv) : 0);
			} else {
				/* False: we want everything after ':' (if anything) */
				argv[0] = s;
				for (i = 0; i < argc; i++) {
					if ((s = strchr(argv[i], ':'))) {
						do { *(s++) = '\0'; } while (isspace(*s));
						argv[i] = s;
						return (argc - i != 1 || s[0] ? pbx_builtin_goto(chan, argc - i, argv + i) : 0);
					}
				}
				/* No ": ..." so we just drop through */
				return 0;
			}
		}
	}
    
	cw_log(LOG_WARNING, "Syntax: GotoIf(boolean ? [[[context,]exten,]priority] [: [[context,]exten,]priority])\n");
	return 0;
}           

static int pbx_builtin_saynumber(struct cw_channel *chan, int argc, char **argv)
{
    if (argc < 1) {
        cw_log(LOG_WARNING, "SayNumber requires an argument (number)\n");
        return -1;
    }
    if (argc > 1) { 
        argv[1][0] = tolower(argv[1][0]);
        if (!strchr("fmcn", argv[1][0])) {
            cw_log(LOG_WARNING, "SayNumber gender option is either 'f', 'm', 'c' or 'n'\n");
            return -1;
        }
    }
    return cw_say_number(chan, atoi(argv[0]), "", chan->language, (argc > 1 ? argv[1] : NULL));
}

static int pbx_builtin_saydigits(struct cw_channel *chan, int argc, char **argv)
{
    int res = 0;

    for (; !res && argc; argv++, argc--)
        res = cw_say_digit_str(chan, argv[0], "", chan->language);
    return res;
}
    
static int pbx_builtin_saycharacters(struct cw_channel *chan, int argc, char **argv)
{
    int res = 0;

    for (; !res && argc; argv++, argc--)
        res = cw_say_character_str(chan, argv[0], "", chan->language);
    return res;
}
    
static int pbx_builtin_sayphonetic(struct cw_channel *chan, int argc, char **argv)
{
    int res = 0;

    for (; !res && argc; argv++, argc--)
        res = cw_say_phonetic_str(chan, argv[0], "", chan->language);
    return res;
}
    
int load_pbx(void)
{
    int x;

    /* Initialize the PBX */
    if (option_verbose)
    {
        cw_verbose( "CallWeaver Core Initializing\n");
        cw_verbose( "Registering builtin applications:\n");
    }
    CW_LIST_HEAD_INIT_NOLOCK(&globals);
    cw_cli_register_multiple(pbx_cli, sizeof(pbx_cli) / sizeof(pbx_cli[0]));

    /* Register builtin applications */
    for (x = 0;  x < arraysize(builtins);  x++) {
        if (option_verbose)
            cw_verbose( VERBOSE_PREFIX_1 "[%s]\n", builtins[x].name);
        if (!cw_register_application(builtins[x].name, builtins[x].execute, builtins[x].synopsis, builtins[x].syntax, builtins[x].description)) {
            cw_log(LOG_ERROR, "Unable to register builtin application '%s'\n", builtins[x].name);
            return -1;
        }
    }

    return 0;
}

/*
 * Lock context list functions ...
 */
int cw_lock_contexts()
{
    return cw_mutex_lock(&conlock);
}

int cw_unlock_contexts()
{
    return cw_mutex_unlock(&conlock);
}

/*
 * Lock context ...
 */
int cw_lock_context(struct cw_context *con)
{
    return cw_mutex_lock(&con->lock);
}

int cw_unlock_context(struct cw_context *con)
{
    return cw_mutex_unlock(&con->lock);
}

/*
 * Name functions ...
 */
const char *cw_get_context_name(struct cw_context *con)
{
    return con ? con->name : NULL;
}

const char *cw_get_extension_name(struct cw_exten *exten)
{
    return exten ? exten->exten : NULL;
}

const char *cw_get_extension_label(struct cw_exten *exten)
{
    return exten ? exten->label : NULL;
}

const char *cw_get_include_name(struct cw_include *inc)
{
    return inc ? inc->name : NULL;
}

const char *cw_get_ignorepat_name(struct cw_ignorepat *ip)
{
    return ip ? ip->pattern : NULL;
}

int cw_get_extension_priority(struct cw_exten *exten)
{
    return exten ? exten->priority : -1;
}

/*
 * Registrar info functions ...
 */
const char *cw_get_context_registrar(struct cw_context *c)
{
    return c ? c->registrar : NULL;
}

const char *cw_get_extension_registrar(struct cw_exten *e)
{
    return e ? e->registrar : NULL;
}

const char *cw_get_include_registrar(struct cw_include *i)
{
    return i ? i->registrar : NULL;
}

const char *cw_get_ignorepat_registrar(struct cw_ignorepat *ip)
{
    return ip ? ip->registrar : NULL;
}

int cw_get_extension_matchcid(struct cw_exten *e)
{
    return e ? e->matchcid : 0;
}

const char *cw_get_extension_cidmatch(struct cw_exten *e)
{
    return e ? e->cidmatch : NULL;
}

const char *cw_get_extension_app(struct cw_exten *e)
{
    return e ? e->app : NULL;
}

void *cw_get_extension_app_data(struct cw_exten *e)
{
    return e ? e->data : NULL;
}

const char *cw_get_switch_name(struct cw_sw *sw)
{
    return sw ? sw->name : NULL;
}

const char *cw_get_switch_data(struct cw_sw *sw)
{
    return sw ? sw->data : NULL;
}

const char *cw_get_switch_registrar(struct cw_sw *sw)
{
    return sw ? sw->registrar : NULL;
}

/*
 * Walking functions ...
 */
struct cw_context *cw_walk_contexts(struct cw_context *con)
{
    if (!con)
        return contexts;
    else
        return con->next;
}

struct cw_exten *cw_walk_context_extensions(struct cw_context *con, struct cw_exten *exten)
{
    if (!exten)
        return con ? con->root : NULL;
    else
        return exten->next;
}

struct cw_sw *cw_walk_context_switches(struct cw_context *con, struct cw_sw *sw)
{
    if (!sw)
        return con ? con->alts : NULL;
    else
        return sw->next;
}

struct cw_exten *cw_walk_extension_priorities(struct cw_exten *exten, struct cw_exten *priority)
{
    if (!priority)
        return exten;
    else
        return priority->peer;
}

struct cw_include *cw_walk_context_includes(struct cw_context *con, struct cw_include *inc)
{
    if (!inc)
        return con ? con->includes : NULL;
    else
        return inc->next;
}

struct cw_ignorepat *cw_walk_context_ignorepats(struct cw_context *con, struct cw_ignorepat *ip)
{
    if (!ip)
        return con ? con->ignorepats : NULL;
    else
        return ip->next;
}

int cw_context_verify_includes(struct cw_context *con)
{
    struct cw_include *inc;
    int res = 0;

    for (inc = cw_walk_context_includes(con, NULL);  inc;  inc = cw_walk_context_includes(con, inc))
    {
        if (!cw_context_find(inc->rname))
        {
            res = -1;
            cw_log(LOG_WARNING, "Attempt to include nonexistent context '%s' in context '%s' (%#x)\n",
                     cw_get_context_name(con), inc->rname, con->hash);
        }
    }
    return res;
}


static int __cw_goto_if_exists(struct cw_channel *chan, char *context, char *exten, int priority, int async) 
{
    int (*goto_func)(struct cw_channel *chan, const char *context, const char *exten, int priority);

    if (!chan)
        return -2;

    goto_func = (async) ? cw_async_goto : cw_explicit_goto;
    if (cw_exists_extension(chan, context ? context : chan->context,
                 exten ? exten : chan->exten, priority,
                 chan->cid.cid_num))
        return goto_func(chan, context ? context : chan->context,
                 exten ? exten : chan->exten, priority);
    else 
        return -3;
}

int cw_goto_if_exists(struct cw_channel *chan, char* context, char *exten, int priority)
{
    return __cw_goto_if_exists(chan, context, exten, priority, 0);
}

int cw_async_goto_if_exists(struct cw_channel *chan, char* context, char *exten, int priority)
{
    return __cw_goto_if_exists(chan, context, exten, priority, 1);
}


int cw_parseable_goto(struct cw_channel *chan, const char *goto_string) 
{
	char *argv[3 + 1];
	char *context = NULL, *exten = NULL, *prio = NULL;
	int argc;
	int ipri, mode = 0;

	if (!goto_string || !(prio = cw_strdupa(goto_string))
	|| (argc = cw_separate_app_args(prio, ',', arraysize(argv), argv)) < 1 || argc > 3) {
		cw_log(LOG_ERROR, "Syntax: Goto([[context,]extension,]priority)\n");
		return -1;
	}

	prio = argv[argc - 1];
	exten = (argc > 1 ? argv[argc - 2] : NULL);
	context = (argc > 2 ? argv[0] : NULL);

	if (exten && cw_hash_app_name(exten) == CW_KEYWORD_BYEXTENSION) {
 		cw_log(LOG_WARNING, "Use of BYEXTENSTION in Goto is deprecated. Use ${EXTEN} instead\n");
		exten = chan->exten;
	}

	if (*prio == '+') {
		mode = 1;
		prio++;
	} else if (*prio == '-') {
		mode = -1;
		prio++;
	}
    
	if (sscanf(prio, "%d", &ipri) != 1) {
		ipri = cw_findlabel_extension(chan,
			(context ? context : chan->context),
			(exten ? exten : chan->exten),
			prio, chan->cid.cid_num);
		if (ipri < 1) {
			cw_log(LOG_ERROR, "Priority '%s' must be a number > 0, or valid label\n", prio);
			return -1;
		}
		mode = 0;
	}
    
	if (mode) 
		ipri = chan->priority + (ipri * mode);

	cw_explicit_goto(chan, context, exten, ipri);
	cw_cdr_update(chan);

	return 0;
}
