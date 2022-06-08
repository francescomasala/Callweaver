/*
 * OpenPBX -- A telephony toolkit for Linux.
 *
 * UDPTL support for T.38
 * 
 * Copyright (C) 2005, Steve Underood, partly based on RTP code which is
 * Copyright (C) 1999-2004, Digium, Inc.
 *
 * Steve Underood <steveu@coppice.org>
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
 *
 * $HeadURL$
 * $Revision$
 */

#ifndef _OPENPBX_UDPTL_H
#define _OPENPBX_UDPTL_H

#include "openpbx/frame.h"
#include "openpbx/io.h"
#include "openpbx/sched.h"
#include "openpbx/channel.h"

#include <netinet/in.h>

enum
{
    UDPTL_ERROR_CORRECTION_NONE,
    UDPTL_ERROR_CORRECTION_FEC,
    UDPTL_ERROR_CORRECTION_REDUNDANCY
};

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

struct opbx_udptl_protocol
{
	/* Get UDPTL struct, or NULL if unwilling to transfer */
	struct opbx_udptl *(*get_udptl_info)(struct opbx_channel *chan);
	/* Set UDPTL peer */
	int (* const set_udptl_peer)(struct opbx_channel *chan, struct opbx_udptl *peer);
	const char * const type;
	struct opbx_udptl_protocol *next;
};

struct opbx_udptl;

typedef int (*opbx_udptl_callback)(struct opbx_udptl *udptl, struct opbx_frame *f, void *data);

struct opbx_udptl *opbx_udptl_new_with_sock_info(struct sched_context *sched,
                                                 struct io_context *io,
                                                 int callbackmode,
                                                 udp_socket_info_t *sock_info);

int opbx_udptl_set_active(struct opbx_udptl *udptl, int active);

void opbx_udptl_set_peer(struct opbx_udptl *udptl, struct sockaddr_in *them);

void opbx_udptl_get_peer(struct opbx_udptl *udptl, struct sockaddr_in *them);

void opbx_udptl_get_us(struct opbx_udptl *udptl, struct sockaddr_in *us);

int opbx_udptl_get_stunstate(struct opbx_udptl *udptl);

void opbx_udptl_destroy(struct opbx_udptl *udptl);

void opbx_udptl_reset(struct opbx_udptl *udptl);

void opbx_udptl_set_callback(struct opbx_udptl *udptl, opbx_udptl_callback callback);

void opbx_udptl_set_data(struct opbx_udptl *udptl, void *data);

int opbx_udptl_write(struct opbx_udptl *udptl, struct opbx_frame *f);

struct opbx_frame *opbx_udptl_read(struct opbx_udptl *udptl);

int opbx_udptl_fd(struct opbx_udptl *udptl);

udp_socket_info_t *opbx_udptl_udp_socket(struct opbx_udptl *udptl,
                                         udp_socket_info_t *sock_info);

int opbx_udptl_settos(struct opbx_udptl *udptl, int tos);

void opbx_udptl_set_m_type(struct opbx_udptl* udptl, int pt);

void opbx_udptl_set_udptlmap_type(struct opbx_udptl *udptl, int pt,
									char *mimeType, char *mimeSubtype);

int opbx_udptl_lookup_code(struct opbx_udptl* udptl, int isAstFormat, int code);

void opbx_udptl_offered_from_local(struct opbx_udptl *udptl, int local);

int opbx_udptl_get_error_correction_scheme(struct opbx_udptl* udptl);

void opbx_udptl_set_error_correction_scheme(struct opbx_udptl* udptl, int ec);

int opbx_udptl_get_local_max_datagram(struct opbx_udptl* udptl);

void opbx_udptl_set_local_max_datagram(struct opbx_udptl* udptl, int max_datagram);

int opbx_udptl_get_far_max_datagram(struct opbx_udptl* udptl);

void opbx_udptl_set_far_max_datagram(struct opbx_udptl* udptl, int max_datagram);

void opbx_udptl_get_current_formats(struct opbx_udptl *udptl,
									int *astFormats, int *nonAstFormats);

void opbx_udptl_setnat(struct opbx_udptl *udptl, int nat);

enum opbx_bridge_result opbx_udptl_bridge(struct opbx_channel *c0, struct opbx_channel *c1, int flags, struct opbx_frame **fo, struct opbx_channel **rc);

int opbx_udptl_proto_register(struct opbx_udptl_protocol *proto);

void opbx_udptl_proto_unregister(struct opbx_udptl_protocol *proto);

void opbx_udptl_stop(struct opbx_udptl *udptl);

void opbx_udptl_init(void);

void opbx_udptl_reload(void);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif
