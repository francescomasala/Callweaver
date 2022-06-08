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
 * \brief Call Parking and Pickup API 
 * Includes code and algorithms from the Zapata library.
 */

#ifndef _OPBX_FEATURES_H
#define _OPBX_FEATURES_H

#define FEATURE_MAX_LEN		11
#define FEATURE_APP_LEN		64
#define FEATURE_APP_ARGS_LEN	256
#define FEATURE_SNAME_LEN	32
#define FEATURE_EXTEN_LEN	32

#include "openpbx/channel.h"
#include "openpbx/config.h"

/*! \brief main call feature structure */
struct opbx_call_feature {
	int feature_mask;
	char *fname;
	char sname[FEATURE_SNAME_LEN];
	char exten[FEATURE_MAX_LEN];
	char default_exten[FEATURE_MAX_LEN];
	int (*operation)(struct opbx_channel *chan, struct opbx_channel *peer, struct opbx_bridge_config *config, char *code, int sense);
	unsigned int flags;
	char app[FEATURE_APP_LEN];		
	char app_args[FEATURE_APP_ARGS_LEN];
	OPBX_LIST_ENTRY(opbx_call_feature) feature_entry;
};



/*! \brief Park a call and read back parked location 
 *  \param chan the channel to actually be parked
    \param host the channel which will have the parked location read to
	Park the channel chan, and read back the parked location to the
	host.  If the call is not picked up within a specified period of
	time, then the call will return to the last step that it was in 
	(in terms of exten, priority and context)
	\param timeout is a timeout in milliseconds
	\param extout is a parameter to an int that will hold the parked location, or NULL if you want
*/
extern int (*opbx_park_call)(struct opbx_channel *chan, struct opbx_channel *host, int timeout, int *extout);
/*! Park a call via a masqueraded channel
 *  \param rchan the real channel to be parked
    \param host the channel to have the parking read to
	Masquerade the channel rchan into a new, empty channel which is then
	parked with opbx_park_call
	\param timeout is a timeout in milliseconds
	\param extout is a parameter to an int that will hold the parked location, or NULL if you want
*/
extern int (*opbx_masq_park_call)(struct opbx_channel *rchan, struct opbx_channel *host, int timeout, int *extout);

/*! \brief Determine system parking extension 
 *  Returns the call parking extension for drivers that provide special
    call parking help */
extern char *(*opbx_parking_ext)(void);

/*! \brief Determine system call pickup extension */
extern char *(*opbx_pickup_ext)(void);


/*! \brief Bridge a call, optionally allowing redirection */
extern int (*opbx_bridge_call)(struct opbx_channel *chan, struct opbx_channel *peer,struct opbx_bridge_config *config);

/*! \brief Pickup a call */
extern int (*opbx_pickup_call)(struct opbx_channel *chan);

/*! \brief register new feature into feature_set 
   \param feature an opbx_call_feature object which contains a keysequence
   and a callback function which is called when this keysequence is pressed
   during a call. */
extern void (*opbx_register_feature)(struct opbx_call_feature *feature);

/*! \brief unregister feature from feature_set
    \param feature the opbx_call_feature object which was registered before*/
extern void (*opbx_unregister_feature)(struct opbx_call_feature *feature);

#endif /* _OPBX_FEATURES_H */
