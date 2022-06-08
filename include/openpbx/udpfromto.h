#ifndef UDPFROMTO_H
#define UDPFROMTO_H

#include <sys/socket.h>

int opbx_udpfromto_init(int s);
int opbx_recvfromto(int s, void *buf, size_t len, int flags,
	struct sockaddr *from, socklen_t *fromlen,
	struct sockaddr *to, socklen_t *tolen);
int opbx_sendfromto(int s, void *buf, size_t len, int flags,
	struct sockaddr *from, socklen_t fromlen,
	struct sockaddr *to, socklen_t tolen);

#endif
