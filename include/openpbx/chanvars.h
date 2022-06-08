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
 * \brief Channel Variables
 */

#ifndef _OPENPBX_CHANVARS_H
#define _OPENPBX_CHANVARS_H

#include "openpbx/linkedlists.h"

struct opbx_var_t {
	OPBX_LIST_ENTRY(opbx_var_t) entries;
	// added 'hash' to accommodate hash based system to recognise identifiers
	unsigned int hash;
	char *value;
	char name[0];
};

OPBX_LIST_HEAD_NOLOCK(varshead, opbx_var_t);

struct opbx_var_t *opbx_var_assign(const char *name, const char *value);
void opbx_var_delete(struct opbx_var_t *var);
char *opbx_var_name(struct opbx_var_t *var);
char *opbx_var_full_name(struct opbx_var_t *var);
char *opbx_var_value(struct opbx_var_t *var);
#define opbx_var_hash(v) (v ? v->hash : 0)

#endif /* _OPENPBX_CHANVARS_H */
