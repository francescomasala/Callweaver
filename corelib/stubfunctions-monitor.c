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

#include <openpbx/monitor.h>


static int stub_opbx_monitor_start( struct opbx_channel *chan, const char *format_spec, const char *fname_base, int need_lock )
{
	opbx_log(LOG_NOTICE, "res_monitor not loaded!\n");
	return -1;
}

static int stub_opbx_monitor_stop( struct opbx_channel *chan, int need_lock)
{
	opbx_log(LOG_NOTICE, "res_monitor not loaded!\n");
	return -1;
}

static int stub_opbx_monitor_change_fname( struct opbx_channel *chan, const char *fname_base, int need_lock )
{
	opbx_log(LOG_NOTICE, "res_monitor not loaded!\n");
	return -1;
}

static void stub_opbx_monitor_setjoinfiles(struct opbx_channel *chan, int turnon)
{
	opbx_log(LOG_NOTICE, "res_monitor not loaded!\n");
}



int (*opbx_monitor_start)( struct opbx_channel *chan, const char *format_spec, const char *fname_base, int need_lock ) =
	stub_opbx_monitor_start;

int (*opbx_monitor_stop)( struct opbx_channel *chan, int need_lock) =
	stub_opbx_monitor_stop;

int (*opbx_monitor_change_fname)( struct opbx_channel *chan, const char *fname_base, int need_lock ) =
	stub_opbx_monitor_change_fname;

void (*opbx_monitor_setjoinfiles)(struct opbx_channel *chan, int turnon) =
	stub_opbx_monitor_setjoinfiles;

