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
 * \brief Music on hold handling
 */

#ifndef _OPENPBX_MOH_H
#define _OPENPBX_MOH_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

/*! Turn on music on hold on a given channel */
extern int opbx_moh_start(struct opbx_channel *chan, char *mclass);

/*! Turn off music on hold on a given channel */
extern void opbx_moh_stop(struct opbx_channel *chan);

extern void opbx_install_music_functions(int (*start_ptr)(struct opbx_channel *, char *),
										void (*stop_ptr)(struct opbx_channel *),
										void (*cleanup_ptr)(struct opbx_channel *));
	
extern void opbx_uninstall_music_functions(void);
void opbx_moh_cleanup(struct opbx_channel *chan);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _OPENPBX_MOH_H */
