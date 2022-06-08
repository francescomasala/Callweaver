/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 2005, Anthony Minessale II
 *
 * Anthony Minessale <anthmct@yahoo.com>
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
 * \brief A machine to gather up arbitrary frames and convert them
 * to raw slinear on demand.
 */

#ifndef _OPENPBX_SLINFACTORY_H
#define _OPENPBX_SLINFACTORY_H
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "openpbx/lock.h"



#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

struct opbx_slinfactory {
	struct opbx_frame *queue;
	struct opbx_trans_pvt *trans;
	short hold[1280];
	short *offset;
	size_t holdlen;
	int size;
	int format;
	opbx_mutex_t lock;

};

void opbx_slinfactory_init(struct opbx_slinfactory *sf);
void opbx_slinfactory_destroy(struct opbx_slinfactory *sf);
int opbx_slinfactory_feed(struct opbx_slinfactory *sf, struct opbx_frame *f);
int opbx_slinfactory_read(struct opbx_slinfactory *sf, short *buf, size_t bytes);
		 


#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _OPENPBX_SLINFACTORY_H */
