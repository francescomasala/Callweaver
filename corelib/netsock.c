/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Kevin P. Fleming <kpfleming@digium.com>
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
 *
 * \brief Network socket handling
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <sys/ioctl.h>

#if defined(__OpenBSD__) || defined(__NetBSD__) || defined(__FreeBSD__)
#include <fcntl.h>
#include <net/route.h>
#endif

#if defined (SOLARIS)
#include <sys/sockio.h>
#endif

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/netsock.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/options.h"
#include "openpbx/utils.h"
#include "openpbx/lock.h"
#include "openpbx/srv.h"

struct opbx_netsock {
	ASTOBJ_COMPONENTS(struct opbx_netsock);
	struct sockaddr_in bindaddr;
	int sockfd;
	int *ioref;
	struct io_context *ioc;
	void *data;
};

struct opbx_netsock_list {
	ASTOBJ_CONTAINER_COMPONENTS(struct opbx_netsock);
	struct io_context *ioc;
};

static void opbx_netsock_destroy(struct opbx_netsock *netsock)
{
	opbx_io_remove(netsock->ioc, netsock->ioref);
	close(netsock->sockfd);
	free(netsock);
}

struct opbx_netsock_list *opbx_netsock_list_alloc(void)
{
	struct opbx_netsock_list *res;

	res = calloc(1, sizeof(*res));

	return res;
}

int opbx_netsock_init(struct opbx_netsock_list *list)
{
	memset(list, 0, sizeof(*list));
	ASTOBJ_CONTAINER_INIT(list);

	return 0;
}

int opbx_netsock_release(struct opbx_netsock_list *list)
{
	ASTOBJ_CONTAINER_DESTROYALL(list, opbx_netsock_destroy);
	ASTOBJ_CONTAINER_DESTROY(list);

	return 0;
}

struct opbx_netsock *opbx_netsock_find(struct opbx_netsock_list *list,
				     struct sockaddr_in *sa)
{
	struct opbx_netsock *sock = NULL;

	ASTOBJ_CONTAINER_TRAVERSE(list, !sock, {
		ASTOBJ_RDLOCK(iterator);
		if (!inaddrcmp(&iterator->bindaddr, sa))
			sock = iterator;
		ASTOBJ_UNLOCK(iterator);
	});

	return sock;
}

struct opbx_netsock *opbx_netsock_bindaddr(struct opbx_netsock_list *list, struct io_context *ioc, struct sockaddr_in *bindaddr, int tos, opbx_io_cb callback, void *data)
{
	int netsocket = -1;
	int *ioref;
	char iabuf[INET_ADDRSTRLEN];
	
	struct opbx_netsock *ns;
	
	/* Make a UDP socket */
	netsocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
	
	if (netsocket < 0) {
		opbx_log(LOG_ERROR, "Unable to create network socket: %s\n", strerror(errno));
		return NULL;
	}
	if (bind(netsocket,(struct sockaddr *)bindaddr, sizeof(struct sockaddr_in))) {
		opbx_log(LOG_ERROR, "Unable to bind to %s port %d: %s\n", opbx_inet_ntoa(iabuf, sizeof(iabuf), bindaddr->sin_addr), ntohs(bindaddr->sin_port), strerror(errno));
		close(netsocket);
		return NULL;
	}
	if (option_verbose > 1)
		opbx_verbose(VERBOSE_PREFIX_2 "Using TOS bits %d\n", tos);

	if (setsockopt(netsocket, IPPROTO_IP, IP_TOS, &tos, sizeof(tos))) 
		opbx_log(LOG_WARNING, "Unable to set TOS to %d\n", tos);

	opbx_enable_packet_fragmentation(netsocket);

	ns = malloc(sizeof(struct opbx_netsock));
	if (ns) {
		/* Establish I/O callback for socket read */
		ioref = opbx_io_add(ioc, netsocket, callback, OPBX_IO_IN, ns);
		if (!ioref) {
			opbx_log(LOG_WARNING, "Out of memory!\n");
			close(netsocket);
			free(ns);
			return NULL;
		}	
		ASTOBJ_INIT(ns);
		ns->ioref = ioref;
		ns->ioc = ioc;
		ns->sockfd = netsocket;
		ns->data = data;
		memcpy(&ns->bindaddr, bindaddr, sizeof(ns->bindaddr));
		ASTOBJ_CONTAINER_LINK(list, ns);
	} else {
		opbx_log(LOG_WARNING, "Out of memory!\n");
		close(netsocket);
	}

	return ns;
}

struct opbx_netsock *opbx_netsock_bind(struct opbx_netsock_list *list, struct io_context *ioc, const char *bindinfo, int defaultport, int tos, opbx_io_cb callback, void *data)
{
	struct sockaddr_in sin;
	char *tmp;
	char *host;
	char *port;
	int portno;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(defaultport);
	tmp = opbx_strdupa(bindinfo);
	if (!tmp) {
		opbx_log(LOG_WARNING, "Out of memory!\n");
		return NULL;
	}

	host = strsep(&tmp, ":");
	port = tmp;

	if (port && ((portno = atoi(port)) > 0))
		sin.sin_port = htons(portno);

	inet_aton(host, &sin.sin_addr);

	return opbx_netsock_bindaddr(list, ioc, &sin, tos, callback, data);
}

int opbx_netsock_sockfd(const struct opbx_netsock *ns)
{
	return ns ? ns-> sockfd : -1;
}

const struct sockaddr_in *opbx_netsock_boundaddr(const struct opbx_netsock *ns)
{
	return &(ns->bindaddr);
}

void *opbx_netsock_data(const struct opbx_netsock *ns)
{
	return ns->data;
}
