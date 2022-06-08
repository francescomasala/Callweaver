/*
 * OpenPBX -- An open source telephony toolkit.
 *
 *
 * See http://www.openpbx.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <openpbx/features.h>


static int stub_opbx_park_call(struct opbx_channel *chan, struct opbx_channel *host, int timeout, int *extout)
{
	opbx_log(LOG_NOTICE, "res_features not loaded!\n");
	return -1;
}

static int stub_opbx_masq_park_call(struct opbx_channel *rchan, struct opbx_channel *host, int timeout, int *extout)
{
	opbx_log(LOG_NOTICE, "res_features not loaded!\n");
	return -1;
}

static char *stub_opbx_parking_ext(void)
{
	opbx_log(LOG_NOTICE, "res_features not loaded!\n");
	return NULL;
}

static char *stub_opbx_pickup_ext(void)
{
	opbx_log(LOG_NOTICE, "res_features not loaded!\n");
	return NULL;
}

static int stub_opbx_bridge_call(struct opbx_channel *chan, struct opbx_channel *peer,struct opbx_bridge_config *config)
{
	opbx_log(LOG_NOTICE, "res_features not loaded!\n");
	return -1;
}

static int stub_opbx_pickup_call(struct opbx_channel *chan)
{
	opbx_log(LOG_NOTICE, "res_features not loaded!\n");
	return -1;
}

static void stub_opbx_register_feature(struct opbx_call_feature *feature)
{
	opbx_log(LOG_NOTICE, "res_features not loaded!\n");
}

static void stub_opbx_unregister_feature(struct opbx_call_feature *feature)
{
	opbx_log(LOG_NOTICE, "res_features not loaded!\n");
}




int (*opbx_park_call)(struct opbx_channel *chan, struct opbx_channel *host, int timeout, int *extout) =
	stub_opbx_park_call;

int (*opbx_masq_park_call)(struct opbx_channel *rchan, struct opbx_channel *host, int timeout, int *extout) =
	stub_opbx_masq_park_call;

char *(*opbx_parking_ext)(void) =
	stub_opbx_parking_ext;

char *(*opbx_pickup_ext)(void) =
	stub_opbx_pickup_ext;

int (*opbx_bridge_call)(struct opbx_channel *chan, struct opbx_channel *peer,struct opbx_bridge_config *config) =
	stub_opbx_bridge_call;

int (*opbx_pickup_call)(struct opbx_channel *chan) =
	stub_opbx_pickup_call;

void (*opbx_register_feature)(struct opbx_call_feature *feature) =
	stub_opbx_register_feature;

void (*opbx_unregister_feature)(struct opbx_call_feature *feature) =
	stub_opbx_unregister_feature;

