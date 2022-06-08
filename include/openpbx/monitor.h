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
 * \brief Channel monitoring
 */

#ifndef _OPENPBX_MONITOR_H
#define _OPENPBX_MONITOR_H

#include <stdio.h>

#include "openpbx/channel.h"

struct opbx_channel;

/*! Responsible for channel monitoring data */
struct opbx_channel_monitor
{
	struct opbx_filestream *read_stream;
	struct opbx_filestream *write_stream;
	char read_filename[ FILENAME_MAX ];
	char write_filename[ FILENAME_MAX ];
	char filename_base[ FILENAME_MAX ];
	int filename_changed;
	char *format;
	int joinfiles;
	int (*stop)( struct opbx_channel *chan, int need_lock);
};

/* Start monitoring a channel */
extern int (*opbx_monitor_start)(	struct opbx_channel *chan, const char *format_spec,
						const char *fname_base, int need_lock );

/* Stop monitoring a channel */
extern int (*opbx_monitor_stop)( struct opbx_channel *chan, int need_lock);

/* Change monitoring filename of a channel */
extern int (*opbx_monitor_change_fname)(	struct opbx_channel *chan,
								const char *fname_base, int need_lock );

extern void (*opbx_monitor_setjoinfiles)(struct opbx_channel *chan, int turnon);

#endif /* _OPENPBX_MONITOR_H */
