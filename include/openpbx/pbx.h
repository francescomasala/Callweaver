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
 * \brief Core PBX routines and definitions.
 */

#ifndef _OPENPBX_PBX_H
#define _OPENPBX_PBX_H

#include "openpbx/sched.h"
#include "openpbx/channel.h"

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#define OPBX_PBX_KEEP    0
#define OPBX_PBX_REPLACE 1

/*! Max length of an application */
#define OPBX_MAX_APP	32

/*! Special return values from applications to the PBX */
#define OPBX_PBX_KEEPALIVE	10		/* Destroy the thread, but don't hang up the channel */
#define OPBX_PBX_NO_HANGUP_PEER       11

/*! Special Priority for an hint */
#define PRIORITY_HINT	-1

/*! Extension states */
enum opbx_extension_states
{
	/*! Extension removed */
	OPBX_EXTENSION_REMOVED = -2,
	/*! Extension hint removed */
	OPBX_EXTENSION_DEACTIVATED = -1,
	/*! No device INUSE or BUSY  */
	OPBX_EXTENSION_NOT_INUSE = 0,
	/*! One or more devices INUSE */
	OPBX_EXTENSION_INUSE = 1 << 0,
	/*! All devices BUSY */
	OPBX_EXTENSION_BUSY = 1 << 1,
	/*! All devices UNAVAILABLE/UNREGISTERED */
	OPBX_EXTENSION_UNAVAILABLE = 1 << 2,
	/*! All devices RINGING */
	OPBX_EXTENSION_RINGING = 1 << 3,
};

enum opbx_extension_match_conditions
{
    /*! The extension did not match. */
    EXTENSION_MATCH_FAILURE = 0,
    /*! The extension is an exact match for the pattern. */
    EXTENSION_MATCH_EXACT = 1,
    /*! The extension starts with an exact match for the pattern, but is longer than the pattern. */
    EXTENSION_MATCH_OVERLENGTH = 2,
    /*! The extension could match the pattern if more digits were added to the end, but is currently incomplete. */
    EXTENSION_MATCH_INCOMPLETE = 3,
    /*! The extension matches, but may be extended further as it matched a '.' or '~'. */
    EXTENSION_MATCH_STRETCHABLE = 4,
    /*! The extension is an 'optional' match for the pattern. i.e. the pattern has a '!' at the end. */
    EXTENSION_MATCH_POSSIBLE = 5
};

static const struct cfextension_states
{
	int extension_state;
	const char * const text;
} extension_states[] = {
	{ OPBX_EXTENSION_NOT_INUSE,                     "Idle" },
	{ OPBX_EXTENSION_INUSE,                         "InUse" },
	{ OPBX_EXTENSION_BUSY,                          "Busy" },
	{ OPBX_EXTENSION_UNAVAILABLE,                   "Unavailable" },
	{ OPBX_EXTENSION_RINGING,                       "Ringing" },
	{ OPBX_EXTENSION_INUSE | OPBX_EXTENSION_RINGING, "InUse&Ringing" }
};

struct opbx_context;
struct opbx_exten;     
struct opbx_include;
struct opbx_ignorepat;
struct opbx_sw;

typedef int (*opbx_state_cb_type)(char *context, char* id, enum opbx_extension_states state, void *data);

/*! Data structure associated with a custom function */
struct opbx_custom_function {
	char *name;
	char *synopsis;
	char *desc;
	char *syntax;
	char *(*read)(struct opbx_channel *, char *, char *, char *, size_t);
	void (*write)(struct opbx_channel *, char *, char *, const char *);
	struct opbx_custom_function *next;
};

/*! Data structure associated with an openpbx switch */
struct opbx_switch
{
	/*! NULL */
	struct opbx_switch *next;	
	/*! Name of the switch */
	const char *name;				
	/*! Description of the switch */
	const char *description;		
	
	int (*exists)(struct opbx_channel *chan, const char *context, const char *exten, int priority, const char *callerid, const char *data);
	
	int (*canmatch)(struct opbx_channel *chan, const char *context, const char *exten, int priority, const char *callerid, const char *data);
	
	int (*exec)(struct opbx_channel *chan, const char *context, const char *exten, int priority, const char *callerid, int newstack, const char *data);

	int (*matchmore)(struct opbx_channel *chan, const char *context, const char *exten, int priority, const char *callerid, const char *data);
};

struct opbx_timing
{
	int hastime;				/* If time construct exists */
	unsigned int monthmask;			/* Mask for month */
	unsigned int daymask;			/* Mask for date */
	unsigned int dowmask;			/* Mask for day of week (mon-sun) */
	unsigned int minmask[24];		/* Mask for minute */
};

extern int opbx_build_timing(struct opbx_timing *i, char *info);
extern int opbx_check_timing(struct opbx_timing *i);

struct opbx_pbx
{
        int dtimeout;                                   /* Timeout between digits (seconds) */
        int rtimeout;                                   /* Timeout for response
							   (seconds) */
};


/*! Register an alternative switch */
/*!
 * \param sw switch to register
 * This function registers a populated opbx_switch structure with the
 * openpbx switching architecture.
 * It returns 0 on success, and other than 0 on failure
 */
extern int opbx_register_switch(struct opbx_switch *sw);

/*! Unregister an alternative switch */
/*!
 * \param sw switch to unregister
 * Unregisters a switch from openpbx.
 * Returns nothing
 */
extern void opbx_unregister_switch(struct opbx_switch *sw);

/*! Look up an application */
/*!
 * \param app name of the app
 * This function searches for the opbx_app structure within
 * the apps that are registered for the one with the name
 * you passed in.
 * Returns the opbx_app structure that matches on success, or NULL on failure
 */
extern struct opbx_app *pbx_findapp(const char *app);

/*! executes an application */
/*!
 * \param c channel to execute on
 * \param app which app to execute
 * \param data the data passed into the app
 * \param newstack stack pointer
 * This application executes an application on a given channel.  It
 * saves the stack and executes the given appliation passing in
 * the given data.
 * It returns 0 on success, and -1 on failure
 */
int pbx_exec(struct opbx_channel *c, struct opbx_app *app, void *data, int newstack);

/*! Register a new context */
/*!
 * \param extcontexts pointer to the opbx_context structure pointer
 * \param name name of the new context
 * \param registrar registrar of the context
 * This will first search for a context with your name.  If it exists already, it will not
 * create a new one.  If it does not exist, it will create a new one with the given name
 * and registrar.
 * It returns NULL on failure, and an opbx_context structure on success
 */
struct opbx_context *opbx_context_create(struct opbx_context **extcontexts, const char *name, const char *registrar);

/*! Merge the temporary contexts into a global contexts list and delete from the global list the ones that are being added */
/*!
 * \param extcontexts pointer to the opbx_context structure pointer
 * \param registar of the context; if it's set the routine will delete all contexts that belong to that registrar; if NULL only the contexts that are specified in extcontexts
 */
void opbx_merge_contexts_and_delete(struct opbx_context **extcontexts, const char *registrar);

/*! Destroy a context (matches the specified context (or ANY context if NULL) */
/*!
 * \param con context to destroy
 * \param registrar who registered it
 * You can optionally leave out either parameter.  It will find it
 * based on either the opbx_context or the registrar name.
 * Returns nothing
 */
void opbx_context_destroy(struct opbx_context *con, const char *registrar);

/*! Find a context */
/*!
 * \param name name of the context to find
 * Will search for the context with the given name.
 * Returns the opbx_context on success, NULL on failure.
 */
struct opbx_context *opbx_context_find(const char *name);

enum opbx_pbx_result
{
	OPBX_PBX_SUCCESS = 0,
	OPBX_PBX_FAILED = -1,
	OPBX_PBX_CALL_LIMIT = -2,
};

/*! Create a new thread and start the PBX (or whatever) */
/*!
 * \param c channel to start the pbx on
 * \return Zero on success, non-zero on failure
 */
enum opbx_pbx_result opbx_pbx_start(struct opbx_channel *c);

/*! Execute the PBX in the current thread */
/*!
 * \param c channel to run the pbx on
 * \return Zero on success, non-zero on failure
 * This executes the PBX on a given channel. It allocates a new
 * PBX structure for the channel, and provides all PBX functionality.
 */
enum opbx_pbx_result opbx_pbx_run(struct opbx_channel *c);

/*! 
 * \param context context to add the extension to
 * \param replace
 * \param extension extension to add
 * \param priority priority level of extension addition
 * \param callerid callerid of extension
 * \param application application to run on the extension with that priority level
 * \param data data to pass to the application
 * \param datad
 * \param registrar who registered the extension
 * Add and extension to an extension context.  
 * Callerid is a pattern to match CallerID, or NULL to match any callerid
 * Returns 0 on success, -1 on failure
 */
int opbx_add_extension(const char *context, int replace, const char *extension, int priority, const char *label, const char *callerid,
	const char *application, void *data, void (*datad)(void *), const char *registrar);

/*! Add an extension to an extension context, this time with an opbx_context *.  CallerID is a pattern to match on callerid, or NULL to not care about callerid */
/*! 
 * For details about the arguements, check opbx_add_extension()
 */
int opbx_add_extension2(struct opbx_context *con,
				      int replace, const char *extension, int priority, const char *label, const char *callerid, 
					  const char *application, void *data, void (*datad)(void *),
					  const char *registrar);

/*! Add an application.  The function 'execute' should return non-zero if the line needs to be hung up.  */
/*!
  \param app Short name of the application
  \param execute a function callback to execute the application
  \param synopsis a short description of the application
  \param description long description of the application
   Include a one-line synopsis (e.g. 'hangs up a channel') and a more lengthy, multiline
   description with more detail, including under what conditions the application
   will return 0 or -1.
   This registers an application with openpbxs internal application list.  Please note:
   The individual applications themselves are responsible for registering and unregistering
   CLI commands.
   It returns 0 on success, -1 on failure.
*/
int opbx_register_application(const char *app, int (*execute)(struct opbx_channel *, void *),
			     const char *synopsis, const char *description);

/*! Remove an application */
/*!
 * \param app name of the application (does not have to be the same string as the one that was registered)
 * This unregisters an application from openpbx's internal registration mechanisms.
 * It returns 0 on success, and -1 on failure.
 */
int opbx_unregister_application(const char *app);

/*! Uses hint and devicestate callback to get the state of an extension */
/*!
 * \param c this is not important
 * \param context which context to look in
 * \param exten which extension to get state
 * Returns extension state !! = OPBX_EXTENSION_???
 */
int opbx_extension_state(struct opbx_channel *c, char *context, char *exten);

/*! Return string of the state of an extension */
/*!
 * \param extension_state is the numerical state delivered by opbx_extension_state
 * Returns the state of an extension as string
 */
const char *opbx_extension_state2str(int extension_state);

/*! Registers a state change callback */
/*!
 * \param context which context to look in
 * \param exten which extension to get state
 * \param callback callback to call if state changed
 * \param data to pass to callback
 * The callback is called if the state for extension is changed
 * Return -1 on failure, ID on success
 */ 
int opbx_extension_state_add(const char *context, const char *exten, 
			    opbx_state_cb_type callback, void *data);

/*! Deletes a registered state change callback by ID */
/*!
 * \param id of the callback to delete
 * Removes the callback from list of callbacks
 * Return 0 on success, -1 on failure
 */
int opbx_extension_state_del(int id, opbx_state_cb_type callback);

/*! If an extension exists, return non-zero */
/*!
 * \param hint buffer for hint
 * \param maxlen size of hint buffer
 * \param hint buffer for name portion of hint
 * \param maxlen size of name buffer
 * \param c this is not important
 * \param context which context to look in
 * \param exten which extension to search for
 * If an extension within the given context with the priority PRIORITY_HINT
 * is found a non zero value will be returned.
 * Otherwise, 0 is returned.
 */
int opbx_get_hint(char *hint, int maxlen, char *name, int maxnamelen, struct opbx_channel *c, const char *context, const char *exten);

/*! If an extension exists, return non-zero */
/*  work */
/*!
 * \param c this is not important
 * \param context which context to look in
 * \param exten which extension to search for
 * \param priority priority of the action within the extension
 * \param callerid callerid to search for
 * If an extension within the given context(or callerid) with the given priority is found a non zero value will be returned.
 * Otherwise, 0 is returned.
 */
int opbx_exists_extension(struct opbx_channel *c, const char *context, const char *exten, int priority, const char *callerid);

/*! If an extension exists, return non-zero */
/*  work */
/*!
 * \param c this is not important
 * \param context which context to look in
 * \param exten which extension to search for
 * \param labellabel of the action within the extension to match to priority
 * \param callerid callerid to search for
 * If an priority which matches given label in extension or -1 if not found.
\ */
int opbx_findlabel_extension(struct opbx_channel *c, const char *context, const char *exten, const char *label, const char *callerid);

int opbx_findlabel_extension2(struct opbx_channel *c, struct opbx_context *con, const char *exten, const char *label, const char *callerid);

/*! Looks for a valid matching extension */
/*!
  \param c not really important
  \param context context to serach within
  \param exten extension to check
  \param priority priority of extension path
  \param callerid callerid of extension being searched for
   If "exten" *could be* a valid extension in this context with or without
   some more digits, return non-zero.  Basically, when this returns 0, no matter
   what you add to exten, it's not going to be a valid extension anymore
*/
int opbx_canmatch_extension(struct opbx_channel *c, const char *context, const char *exten, int priority, const char *callerid);

/*! Looks to see if adding anything to this extension might match something. (exists ^ canmatch) */
/*!
  \param c not really important
  \param context context to serach within
  \param exten extension to check
  \param priority priority of extension path
  \param callerid callerid of extension being searched for
   If "exten" *could match* a valid extension in this context with
   some more digits, return non-zero.  Does NOT return non-zero if this is
   an exact-match only.  Basically, when this returns 0, no matter
   what you add to exten, it's not going to be a valid extension anymore
*/
int opbx_matchmore_extension(struct opbx_channel *c, const char *context, const char *exten, int priority, const char *callerid);

const char *opbx_extension_match_to_str(int match);

/*! Determine how well a given extension matches a given pattern (in NXX format) */
/*!
 * \param destinationm extension to check against the pattern.
 * \param pattern pattern to match
 * Checks the extent to which the given extension matches the given pattern.
 * \return one of the enum opbx_extension_match_conditions values
 */
int opbx_extension_pattern_match(const char *destination, const char *pattern);

/*! Launch a new extension (i.e. new stack) */
/*!
 * \param c not important
 * \param context which context to generate the extension within
 * \param exten new extension to add
 * \param priority priority of new extension
 * \param callerid callerid of extension
 * This adds a new extension to the openpbx extension list.
 * It returns 0 on success, -1 on failure.
 */
int opbx_spawn_extension(struct opbx_channel *c, const char *context, const char *exten, int priority, const char *callerid);

/*! Execute an extension. */
/*!
  \param c channel to execute upon
  \param context which context extension is in
  \param exten extension to execute
  \param priority priority to execute within the given extension
   If it's not available, do whatever you should do for
   default extensions and halt the thread if necessary.  This function does not
   return, except on error.
*/
int opbx_exec_extension(struct opbx_channel *c, const char *context, const char *exten, int priority, const char *callerid);

/*! Add an include */
/*!
  \param context context to add include to
  \param include new include to add
  \param registrar who's registering it
   Adds an include taking a char * string as the context parameter
   Returns 0 on success, -1 on error
*/
int opbx_context_add_include(const char *context, const char *include, const char *registrar);

/*! Add an include */
/*!
  \param con context to add the include to
  \param include include to add
  \param registrar who registered the context
   Adds an include taking a struct opbx_context as the first parameter
   Returns 0 on success, -1 on failure
*/
int opbx_context_add_include2(struct opbx_context *con, const char *include, const char *registrar);

/*! Removes an include */
/*!
 * See add_include
 */
int opbx_context_remove_include(const char *context, const char *include,const  char *registrar);
/*! Removes an include by an opbx_context structure */
/*!
 * See add_include2
 */
int opbx_context_remove_include2(struct opbx_context *con, const char *include, const char *registrar);

/*! Verifies includes in an opbx_contect structure */
/*!
 * \param con context in which to verify the includes
 * Returns 0 if no problems found, -1 if there were any missing context
 */
int opbx_context_verify_includes(struct opbx_context *con);
	  
/*! Add a switch */
/*!
 * \param context context to which to add the switch
 * \param sw switch to add
 * \param data data to pass to switch
 * \param eval whether to evaluate variables when running switch
 * \param registrar whoever registered the switch
 * This function registers a switch with the openpbx switch architecture
 * It returns 0 on success, -1 on failure
 */
int opbx_context_add_switch(const char *context, const char *sw, const char *data, int eval, const char *registrar);
/*! Adds a switch (first param is a opbx_context) */
/*!
 * See opbx_context_add_switch()
 */
int opbx_context_add_switch2(struct opbx_context *con, const char *sw, const char *data, int eval, const char *registrar);

/*! Remove a switch */
/*!
 * Removes a switch with the given parameters
 * Returns 0 on success, -1 on failure
 */
int opbx_context_remove_switch(const char *context, const char *sw, const char *data, const char *registrar);
int opbx_context_remove_switch2(struct opbx_context *con, const char *sw, const char *data, const char *registrar);

/*! Simply remove extension from context */
/*!
 * \param context context to remove extension from
 * \param extension which extension to remove
 * \param priority priority of extension to remove
 * \param registrar registrar of the extension
 * This function removes an extension from a given context.
 * Returns 0 on success, -1 on failure
 */
int opbx_context_remove_extension(const char *context, const char *extension, int priority,
	const char *registrar);
int opbx_context_remove_extension2(struct opbx_context *con, const char *extension,
	int priority, const char *registrar);

/*! Add an ignorepat */
/*!
 * \param context which context to add the ignorpattern to
 * \param ignorpat ignorepattern to set up for the extension
 * \param registrar registrar of the ignore pattern
 * Adds an ignore pattern to a particular context.
 * Returns 0 on success, -1 on failure
 */
int opbx_context_add_ignorepat(const char *context, const char *ignorepat, const char *registrar);
int opbx_context_add_ignorepat2(struct opbx_context *con, const char *ignorepat, const char *registrar);

/* Remove an ignorepat */
/*!
 * \param context context from which to remove the pattern
 * \param ignorepat the pattern to remove
 * \param registrar the registrar of the ignore pattern
 * This removes the given ignorepattern
 * Returns 0 on success, -1 on failure
 */
int opbx_context_remove_ignorepat(const char *context, const char *ignorepat, const char *registrar);
int opbx_context_remove_ignorepat2(struct opbx_context *con, const char *ignorepat, const char *registrar);

/*! Checks to see if a number should be ignored */
/*!
 * \param context context to search within
 * \param extension to check whether it should be ignored or not
 * Check if a number should be ignored with respect to dialtone cancellation.  
 * Returns 0 if the pattern should not be ignored, or non-zero if the pattern should be ignored 
 */
int opbx_ignore_pattern(const char *context, const char *pattern);

/* Locking functions for outer modules, especially for completion functions */
/*! Locks the contexts */
/*! Locks the context list
 * Returns 0 on success, -1 on error
 */
int opbx_lock_contexts(void);

/*! Unlocks contexts */
/*!
 * Returns 0 on success, -1 on failure
 */
int opbx_unlock_contexts(void);

/*! Locks a given context */
/*!
 * \param con context to lock
 * Locks the context.
 * Returns 0 on success, -1 on failure
 */
int opbx_lock_context(struct opbx_context *con);
/*! Unlocks the given context */
/*!
 * \param con context to unlock
 * Unlocks the given context
 * Returns 0 on success, -1 on failure
 */
int opbx_unlock_context(struct opbx_context *con);


int opbx_async_goto(struct opbx_channel *chan, const char *context, const char *exten, int priority);

int opbx_async_goto_by_name(const char *chan, const char *context, const char *exten, int priority);

/* Synchronously or asynchronously make an outbound call and send it to a
   particular extension */
int opbx_pbx_outgoing_exten(const char *type, int format, void *data, int timeout, const char *context, const char *exten, int priority, int *reason, int sync, const char *cid_num, const char *cid_name, struct opbx_variable *vars, struct opbx_channel **locked_channel);

/* Synchronously or asynchronously make an outbound call and send it to a
   particular application with given extension */
int opbx_pbx_outgoing_app(const char *type, int format, void *data, int timeout, const char *app, const char *appdata, int *reason, int sync, const char *cid_num, const char *cid_name, struct opbx_variable *vars, struct opbx_channel **locked_channel);

/* Functions for returning values from structures */
const char *opbx_get_context_name(struct opbx_context *con);
const char *opbx_get_extension_name(struct opbx_exten *exten);
const char *opbx_get_include_name(struct opbx_include *include);
const char *opbx_get_ignorepat_name(struct opbx_ignorepat *ip);
const char *opbx_get_switch_name(struct opbx_sw *sw);
const char *opbx_get_switch_data(struct opbx_sw *sw);

/* Other extension stuff */
int opbx_get_extension_priority(struct opbx_exten *exten);
int opbx_get_extension_matchcid(struct opbx_exten *e);
const char *opbx_get_extension_cidmatch(struct opbx_exten *e);
const char *opbx_get_extension_app(struct opbx_exten *e);
const char *opbx_get_extension_label(struct opbx_exten *e);
void *opbx_get_extension_app_data(struct opbx_exten *e);

/* Registrar info functions ... */
const char *opbx_get_context_registrar(struct opbx_context *c);
const char *opbx_get_extension_registrar(struct opbx_exten *e);
const char *opbx_get_include_registrar(struct opbx_include *i);
const char *opbx_get_ignorepat_registrar(struct opbx_ignorepat *ip);
const char *opbx_get_switch_registrar(struct opbx_sw *sw);

/* Walking functions ... */
struct opbx_context *opbx_walk_contexts(struct opbx_context *con);
struct opbx_exten *opbx_walk_context_extensions(struct opbx_context *con,
	struct opbx_exten *priority);
struct opbx_exten *opbx_walk_extension_priorities(struct opbx_exten *exten,
	struct opbx_exten *priority);
struct opbx_include *opbx_walk_context_includes(struct opbx_context *con,
	struct opbx_include *inc);
struct opbx_ignorepat *opbx_walk_context_ignorepats(struct opbx_context *con,
	struct opbx_ignorepat *ip);
struct opbx_sw *opbx_walk_context_switches(struct opbx_context *con, struct opbx_sw *sw);

int pbx_builtin_serialize_variables(struct opbx_channel *chan, char *buf, size_t size);
extern char *pbx_builtin_getvar_helper(struct opbx_channel *chan, const char *name);
extern void pbx_builtin_pushvar_helper(struct opbx_channel *chan, const char *name, const char *value);
extern void pbx_builtin_setvar_helper(struct opbx_channel *chan, const char *name, const char *value);
extern void pbx_retrieve_variable(struct opbx_channel *c, const char *var, char **ret, char *workspace, int workspacelen, struct varshead *headp);
extern void pbx_builtin_clear_globals(void);
extern int pbx_builtin_setvar(struct opbx_channel *chan, void *data);
extern void pbx_substitute_variables_helper(struct opbx_channel *c,const char *cp1,char *cp2,int count);
extern void pbx_substitute_variables_varshead(struct varshead *headp, const char *cp1, char *cp2, int count);

int opbx_extension_patmatch(const char *pattern, const char *data);

/* Set "autofallthrough" flag, if newval is <0, does not acutally set.  If
  set to 1, sets to auto fall through.  If newval set to 0, sets to no auto
  fall through (reads extension instead).  Returns previous value. */
extern int pbx_set_autofallthrough(int newval);
int opbx_goto_if_exists(struct opbx_channel *chan, char* context, char *exten, int priority);
/* I can find neither parsable nor parseable at dictionary.com, but google gives me 169000 hits for parseable and only 49,800 for parsable */
int opbx_parseable_goto(struct opbx_channel *chan, const char *goto_string);
int opbx_explicit_goto(struct opbx_channel *chan, const char *context, const char *exten, int priority);
int opbx_async_goto_if_exists(struct opbx_channel *chan, char* context, char *exten, int priority);

struct opbx_custom_function* opbx_custom_function_find(char *name);
int opbx_custom_function_unregister(struct opbx_custom_function *acf);
int opbx_custom_function_register(struct opbx_custom_function *acf);

/* Number of active calls */
int opbx_active_calls(void);
	
/*! executes a read operation on a function */
/*!
 * \param chan Channel to execute on
 * \param in Data containing the function call string
 * \param workspace A pointer to safe memory to use for a return value 
 * \param len the number of bytes in workspace
 * This application executes an function in read mode on a given channel.
 * It returns a pointer to workspace if the buffer contains any new data
 * or NULL if there was a problem.
 */
	
char *opbx_func_read(struct opbx_channel *chan, const char *in, char *workspace, size_t len);

/*! executes a write operation on a function */
/*!
 * \param chan Channel to execute on
 * \param in Data containing the function call string
 * \param value A value parameter to pass for writing
 * This application executes an function in write mode on a given channel.
 * It has no return value.
 */
void opbx_func_write(struct opbx_channel *chan, const char *in, const char *value);

void opbx_hint_state_changed(const char *device);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _OPENPBX_PBX_H */
