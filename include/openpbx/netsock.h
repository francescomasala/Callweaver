/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 * Kevin P. Fleming <kpfleming@digium.com>
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
 * \brief Network socket handling
 */

#ifndef _OPENPBX_NETSOCK_H
#define _OPENPBX_NETSOCK_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#include <netinet/in.h>
#include "openpbx/io.h"
#include "openpbx/astobj.h"

struct opbx_netsock;

struct opbx_netsock_list;

struct opbx_netsock_list *opbx_netsock_list_alloc(void);

int opbx_netsock_init(struct opbx_netsock_list *list);

struct opbx_netsock *opbx_netsock_bind(struct opbx_netsock_list *list, struct io_context *ioc,
				     const char *bindinfo, int defaultport, int tos, opbx_io_cb callback, void *data);

struct opbx_netsock *opbx_netsock_bindaddr(struct opbx_netsock_list *list, struct io_context *ioc,
					 struct sockaddr_in *bindaddr, int tos, opbx_io_cb callback, void *data);

int opbx_netsock_free(struct opbx_netsock_list *list, struct opbx_netsock *netsock);

int opbx_netsock_release(struct opbx_netsock_list *list);

struct opbx_netsock *opbx_netsock_find(struct opbx_netsock_list *list,
				     struct sockaddr_in *sa);

int opbx_netsock_sockfd(const struct opbx_netsock *ns);

const struct sockaddr_in *opbx_netsock_boundaddr(const struct opbx_netsock *ns);

void *opbx_netsock_data(const struct opbx_netsock *ns);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _OPENPBX_NETSOCK_H */
