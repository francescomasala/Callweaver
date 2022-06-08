/*
 * ICD - Intelligent Call Distributor 
 *
 * Copyright (C) 2003, 2004, 2005
 *
 * Written by Anthony Minessale II <anthmct at yahoo dot com>
 * Written by Bruce Atherton <bruce at callenish dot com>
 * Additions, Changes and Support by Tim R. Clark <tclark at shaw dot ca>
 * Changed to adopt to jabber interaction and adjusted for OpenPBX.org by
 * Halo Kwadrat Sp. z o.o., Piotr Figurny and Michal Bielicki
 * 
 * This application is a part of:
 * 
 * OpenPBX -- An open source telephony toolkit.
 * Copyright (C) 1999 - 2005, Digium, Inc.
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
 
#ifndef ICD_COMMON_H

#define ICD_COMMON_H

/* Standard Includes */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <netinet/in.h>

/* Openpbx Includes */
#include "openpbx.h"

#include "openpbx/file.h"
#include "openpbx/say.h"
#include "openpbx/logger.h"
#include "openpbx/utils.h"
#include "openpbx/options.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/module.h"
#include "openpbx/musiconhold.h"
#include "openpbx/cli.h"
#include "openpbx/config.h"
#include "openpbx/manager.h"
#include "openpbx/features.h"

/* ICD Includes */
#include "openpbx/icd/icd_fieldset.h"
#include "openpbx/icd/icd_listeners.h"
#include "openpbx/icd/icd_event.h"
#include "openpbx/icd/voidhash.h"
#include "openpbx/icd/icd_config.h"
#include "openpbx/icd/icd_types.h"
#include "openpbx/icd/icd_globals.h"
#include "openpbx/icd/icd_plugable_fn.h"

/* 
   Support for pre/post 1.0 rendition of opbx_set_(read/write)_format.
   Add the CFLAG -DAST_POST_10 in make.conf to get the 3 arg version *default*
   or comment it to get the 2 arg version.
   This is obsolete as of 06/01/2004 do we nuke this macros 
*/

#ifdef AST_POST_10
#define icd_set_read_format(chan,fmt) opbx_set_read_format(chan,fmt,0);
#define icd_set_write_format(chan,fmt) opbx_set_write_format(chan,fmt,0);
#else
#define icd_set_read_format(chan,fmt) opbx_set_read_format(chan,fmt);
#define icd_set_write_format(chan,fmt) opbx_set_write_format(chan,fmt);
#endif
#endif


/* For Emacs:
 * Local Variables:
 * indent-tabs-mode:nil
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 expandtab:
 */

