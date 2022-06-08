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
 *
 * $HeadURL$
 * $Revision$
 */

/*
 * \file rtp.h
 * \brief Supports RTP and RTCP with Symmetric RTP support for NAT traversal.
 *
 * RTP is defined in RFC 3550.
 *
 */

#ifndef _OPENPBX_RTP_H
#define _OPENPBX_RTP_H

#include "openpbx/frame.h"
#include "openpbx/io.h"
#include "openpbx/sched.h"
#include "openpbx/channel.h"
#include "openpbx/udp.h"

#include <netinet/in.h>

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

/* Codes for RTP-specific data - not defined by our OPBX_FORMAT codes */
/*! DTMF (RFC2833) */
#define OPBX_RTP_DTMF            (1 << 0)
/*! 'Comfort Noise' (RFC3389) */
#define OPBX_RTP_CN              (1 << 1)
/*! DTMF (Cisco Proprietary) */
#define OPBX_RTP_CISCO_DTMF      (1 << 2)
/*! Maximum RTP-specific code */
#define OPBX_RTP_MAX             OPBX_RTP_CISCO_DTMF

#define MAX_RTP_PT 256

struct opbx_rtp_protocol
{
	/* Get RTP struct, or NULL if unwilling to transfer */
	struct opbx_rtp *(* const get_rtp_info)(struct opbx_channel *chan);
	/* Get RTP struct, or NULL if unwilling to transfer */
	struct opbx_rtp *(* const get_vrtp_info)(struct opbx_channel *chan);
	/* Set RTP peer */
	int (* const set_rtp_peer)(struct opbx_channel *chan, struct opbx_rtp *peer, struct opbx_rtp *vpeer, int codecs, int nat_active);
	int (* const get_codec)(struct opbx_channel *chan);
	const char * const type;
	struct opbx_rtp_protocol *next;
};

typedef int (*opbx_rtp_callback)(struct opbx_rtp *rtp, struct opbx_frame *f, void *data);

/* The value of each payload format mapping: */
struct rtpPayloadType
{
	int is_opbx_format; 	/* whether the following code is an OPBX_FORMAT */
	int code;
};

struct opbx_rtp
{
    udp_socket_info_t *rtp_sock_info;
    udp_socket_info_t *rtcp_sock_info;
	char resp;
	struct opbx_frame f;
	uint8_t rawdata[8192 + OPBX_FRIENDLY_OFFSET];
	uint32_t ssrc;
	uint32_t lastts;
	uint32_t lastdigitts;
	uint32_t lastrxts;
	uint32_t lastividtimestamp;
	uint32_t lastovidtimestamp;
	uint32_t lasteventseqn;
	uint32_t lasteventendseqn;
	int lasttxformat;
	int lastrxformat;
	int dtmfcount;
	unsigned int dtmfduration;
	int nat;
	unsigned int flags;
	int framems;
	int rtplen;
	struct timeval rxcore;
	struct timeval txcore;
	struct timeval dtmfmute;
	struct opbx_smoother *smoother;
	int *ioid;
	uint16_t seqno;
	uint16_t rxseqno;
	struct sched_context *sched;
	struct io_context *io;
	void *data;
	opbx_rtp_callback callback;
	struct rtpPayloadType current_RTP_PT[MAX_RTP_PT];
	int rtp_lookup_code_cache_is_opbx_format;	/* a cache for the result of rtp_lookup_code(): */
	int rtp_lookup_code_cache_code;
	int rtp_lookup_code_cache_result;
	int rtp_offered_from_local;
#ifdef ENABLE_SRTP
	srtp_t srtp;
	rtp_generate_key_cb key_cb;
#endif
};


struct opbx_rtp *opbx_rtp_new_with_bindaddr(struct sched_context *sched, struct io_context *io, int rtcpenable, int callbackmode, struct in_addr in);

void opbx_rtp_set_peer(struct opbx_rtp *rtp, struct sockaddr_in *them);

void opbx_rtp_get_peer(struct opbx_rtp *rtp, struct sockaddr_in *them);

void opbx_rtp_get_us(struct opbx_rtp *rtp, struct sockaddr_in *us);

int opbx_rtp_get_stunstate(struct opbx_rtp *rtp);

void opbx_rtp_destroy(struct opbx_rtp *rtp);

void opbx_rtp_reset(struct opbx_rtp *rtp);

void opbx_rtp_set_callback(struct opbx_rtp *rtp, opbx_rtp_callback callback);

void opbx_rtp_set_data(struct opbx_rtp *rtp, void *data);

int opbx_rtp_write(struct opbx_rtp *rtp, struct opbx_frame *f);

struct opbx_frame *opbx_rtp_read(struct opbx_rtp *rtp);

struct opbx_frame *opbx_rtcp_read(struct opbx_rtp *rtp);

int opbx_rtp_fd(struct opbx_rtp *rtp);

int opbx_rtcp_fd(struct opbx_rtp *rtp);

udp_socket_info_t *opbx_rtp_udp_socket(struct opbx_rtp *rtp,
                                       udp_socket_info_t *sock_info);

udp_socket_info_t *opbx_rtcp_udp_socket(struct opbx_rtp *rtp,
                                        udp_socket_info_t *sock_info);

int opbx_rtp_senddigit(struct opbx_rtp *rtp, char digit);

int opbx_rtp_sendcng(struct opbx_rtp *rtp, int level);

int opbx_rtp_set_active(struct opbx_rtp *rtp, int active);

int opbx_rtp_settos(struct opbx_rtp *rtp, int tos);

/*  Setting RTP payload types from lines in a SDP description: */
void opbx_rtp_pt_clear(struct opbx_rtp* rtp);
/* Set payload types to defaults */
void opbx_rtp_pt_default(struct opbx_rtp* rtp);
void opbx_rtp_set_m_type(struct opbx_rtp* rtp, int pt);
void opbx_rtp_set_rtpmap_type(struct opbx_rtp* rtp, int pt,
			 char* mimeType, char* mimeSubtype);

/*  Mapping between RTP payload format codes and OpenPBX codes: */
struct rtpPayloadType opbx_rtp_lookup_pt(struct opbx_rtp* rtp, int pt);
int opbx_rtp_lookup_code(struct opbx_rtp* rtp, int is_opbx_format, int code);
void opbx_rtp_offered_from_local(struct opbx_rtp* rtp, int local);

void opbx_rtp_get_current_formats(struct opbx_rtp* rtp,
			     int* astFormats, int* nonAstFormats);

/*  Mapping an OpenPBX code into a MIME subtype (string): */
char* opbx_rtp_lookup_mime_subtype(int is_opbx_format, int code);

/* Build a string of MIME subtype names from a capability list */
char *opbx_rtp_lookup_mime_multiple(char *buf, int size, const int capability, const int is_opbx_format);

void opbx_rtp_setnat(struct opbx_rtp *rtp, int nat);

int opbx_rtp_bridge(struct opbx_channel *c0, struct opbx_channel *c1, int flags, struct opbx_frame **fo, struct opbx_channel **rc, int timeoutms);

int opbx_rtp_proto_register(struct opbx_rtp_protocol *proto);

void opbx_rtp_proto_unregister(struct opbx_rtp_protocol *proto);

void opbx_rtp_stop(struct opbx_rtp *rtp);

void opbx_rtp_init(void);

void opbx_rtp_reload(void);

int opbx_rtp_set_framems(struct opbx_rtp *rtp, int ms);

#ifdef ENABLE_SRTP

/* Crypto suites */
#define OPBX_AES_CM_128_HMAC_SHA1_80 1
#define OPBX_AES_CM_128_HMAC_SHA1_32 2
#define OPBX_F8_128_HMAC_SHA1_80     3

#define MIKEY_SRTP_EALG_NULL     0
#define MIKEY_SRTP_EALG_AESCM    1
#define MIKEY_SRTP_AALG_NULL     0
#define MIKEY_SRTP_AALG_SHA1HMAC 1

typedef struct opbx_policy opbx_policy_t;
typedef int (*rtp_generate_key_cb)(struct opbx_rtp *rtp, unsigned long ssrc,
				   void *data);

unsigned int opbx_rtp_get_ssrc(struct opbx_rtp *rtp);
void opbx_rtp_set_generate_key_cb(struct opbx_rtp *rtp,
				  rtp_generate_key_cb cb);
int opbx_rtp_add_policy(struct opbx_rtp *rtp, opbx_policy_t *policy);
opbx_policy_t *opbx_policy_alloc(void);
int opbx_policy_set_suite(opbx_policy_t *policy, int suite);
int opbx_policy_set_master_key(opbx_policy_t *policy,
			      const unsigned char *key, size_t key_len,
			      const unsigned char *salt, size_t salt_len);
int opbx_policy_set_encr_alg(opbx_policy_t *policy, int ealg);
int opbx_policy_set_auth_alg(opbx_policy_t *policy, int aalg);
void opbx_policy_set_encr_keylen(opbx_policy_t *policy, int ekeyl);
void opbx_policy_set_auth_keylen(opbx_policy_t *policy, int akeyl);
void opbx_policy_set_srtp_auth_taglen(opbx_policy_t *policy, int autht);
void opbx_policy_set_srtp_encr_enable(opbx_policy_t *policy, int enable);
void opbx_policy_set_srtcp_encr_enable(opbx_policy_t *policy, int enable);
void opbx_policy_set_srtp_auth_enable(opbx_policy_t *policy, int enable);
void opbx_policy_set_ssrc(opbx_policy_t *policy, struct opbx_rtp *rtp, 
			  unsigned long ssrc, int inbound);
    
void opbx_policy_destroy(opbx_policy_t *policy);
int opbx_get_random(unsigned char *key, size_t len);

#endif	/* ENABLE_SRTP */

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _OPENPBX_RTP_H */
