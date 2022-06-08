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
 * \brief Access Control of various sorts
 */

#ifndef _OPENPBX_ACL_H
#define _OPENPBX_ACL_H


#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#include <netinet/in.h>
#include "openpbx/io.h"

#define OPBX_SENSE_DENY                  0
#define OPBX_SENSE_ALLOW                 1

/* Host based access control */

struct opbx_ha;

extern void opbx_free_ha(struct opbx_ha *ha);
extern struct opbx_ha *opbx_append_ha(char *sense, char *stuff, struct opbx_ha *path);
extern int opbx_apply_ha(struct opbx_ha *ha, struct sockaddr_in *sin);
extern int opbx_get_ip(struct sockaddr_in *sin, const char *value);
extern int opbx_get_ip_or_srv(struct sockaddr_in *sin, const char *value, const char *service);
extern int opbx_ouraddrfor(struct in_addr *them, struct in_addr *us);
extern int opbx_lookup_iface(char *iface, struct in_addr *address);
extern struct opbx_ha *opbx_duplicate_ha_list(struct opbx_ha *original);
extern int opbx_find_ourip(struct in_addr *ourip, struct sockaddr_in bindaddr);
extern int opbx_str2tos(const char *value, int *tos);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _OPENPBX_ACL_H */