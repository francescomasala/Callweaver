/*
 * CallWeaver -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * See http://www.callweaver.org for more information about
 * the CallWeaver project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 *
 * $HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/include/callweaver/rtp.h $
 * $Revision: 4723 $
 */

/*
 * \file rtp.h
 * \brief Supports RTP and RTCP with Symmetric RTP support for NAT traversal.
 *
 * RTP is defined in RFC 3550.
 *
 */

#ifndef _CALLWEAVER_RTP_H
#define _CALLWEAVER_RTP_H

#include "callweaver/frame.h"
#include "callweaver/io.h"
#include "callweaver/sched.h"
#include "callweaver/channel.h"
#include "callweaver/udp.h"

#include <netinet/in.h>

#ifdef ENABLE_SRTP
#include <srtp/srtp.h>
#endif

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

/* Codes for RTP-specific data - not defined by our CW_FORMAT codes */
/*! DTMF (RFC2833) */
#define CW_RTP_DTMF            (1 << 0)
/*! 'Comfort Noise' (RFC3389) */
#define CW_RTP_CN              (1 << 1)
/*! DTMF (Cisco Proprietary) */
#define CW_RTP_CISCO_DTMF      (1 << 2)
/*! Maximum RTP-specific code */
#define CW_RTP_MAX             CW_RTP_CISCO_DTMF

#define MAX_RTP_PT 256

struct cw_rtp_protocol
{
	/* Get RTP struct, or NULL if unwilling to transfer */
	struct cw_rtp *(* const get_rtp_info)(struct cw_channel *chan);
	/* Get RTP struct, or NULL if unwilling to transfer */
	struct cw_rtp *(* const get_vrtp_info)(struct cw_channel *chan);
	/* Set RTP peer */
	int (* const set_rtp_peer)(struct cw_channel *chan, struct cw_rtp *peer, struct cw_rtp *vpeer, int codecs, int nat_active);
	int (* const get_codec)(struct cw_channel *chan);
	const char * const type;
	struct cw_rtp_protocol *next;
};

typedef int (*cw_rtp_callback)(struct cw_rtp *rtp, struct cw_frame *f, void *data);

/* The value of each payload format mapping: */
struct rtpPayloadType
{
	int is_cw_format; 	/* whether the following code is an CW_FORMAT */
	int code;
};


struct cw_rtp *cw_rtp_new_with_bindaddr(struct sched_context *sched, struct io_context *io, int rtcpenable, int callbackmode, struct in_addr in);

void cw_rtp_set_peer(struct cw_rtp *rtp, struct sockaddr_in *them);

void cw_rtp_get_peer(struct cw_rtp *rtp, struct sockaddr_in *them);

void cw_rtp_get_us(struct cw_rtp *rtp, struct sockaddr_in *us);

int cw_rtp_get_stunstate(struct cw_rtp *rtp);

void cw_rtp_destroy(struct cw_rtp *rtp);

void cw_rtp_reset(struct cw_rtp *rtp);

void cw_rtp_set_callback(struct cw_rtp *rtp, cw_rtp_callback callback);

void cw_rtp_set_data(struct cw_rtp *rtp, void *data);

int cw_rtp_write(struct cw_rtp *rtp, struct cw_frame *f);

struct cw_frame *cw_rtp_read(struct cw_rtp *rtp);

struct cw_frame *cw_rtcp_read(struct cw_rtp *rtp);

int cw_rtp_fd(struct cw_rtp *rtp);

int cw_rtcp_fd(struct cw_rtp *rtp);

udp_socket_info_t *cw_rtp_udp_socket(struct cw_rtp *rtp,
                                       udp_socket_info_t *sock_info);

udp_socket_info_t *cw_rtcp_udp_socket(struct cw_rtp *rtp,
                                        udp_socket_info_t *sock_info);

int cw_rtp_sendevent(struct cw_rtp *const rtp, char event, uint16_t duration);

int cw_rtp_sendcng(struct cw_rtp *rtp, int level);

int cw_rtp_set_active(struct cw_rtp *rtp, int active);

int cw_rtp_settos(struct cw_rtp *rtp, int tos);

/*  Setting RTP payload types from lines in a SDP description: */
void cw_rtp_pt_clear(struct cw_rtp* rtp);
/* Set payload types to defaults */
void cw_rtp_pt_default(struct cw_rtp* rtp);
void cw_rtp_set_m_type(struct cw_rtp* rtp, int pt);
void cw_rtp_set_rtpmap_type(struct cw_rtp* rtp, int pt,
			 char* mimeType, char* mimeSubtype);

/*  Mapping between RTP payload format codes and CallWeaver codes: */
struct rtpPayloadType cw_rtp_lookup_pt(struct cw_rtp* rtp, int pt);
int cw_rtp_lookup_code(struct cw_rtp* rtp, int is_cw_format, int code);
void cw_rtp_offered_from_local(struct cw_rtp* rtp, int local);

void cw_rtp_get_current_formats(struct cw_rtp* rtp,
			     int* cw_formats, int* non_cw_formats);

/*  Mapping an CallWeaver code into a MIME subtype (string): */
char* cw_rtp_lookup_mime_subtype(int is_cw_format, int code);

/* Build a string of MIME subtype names from a capability list */
char *cw_rtp_lookup_mime_multiple(char *buf, int size, const int capability, const int is_cw_format);

void cw_rtp_setnat(struct cw_rtp *rtp, int nat);

int cw_rtp_bridge(struct cw_channel *c0, struct cw_channel *c1, int flags, struct cw_frame **fo, struct cw_channel **rc, int timeoutms);

int cw_rtp_proto_register(struct cw_rtp_protocol *proto);

void cw_rtp_proto_unregister(struct cw_rtp_protocol *proto);

void cw_rtp_stop(struct cw_rtp *rtp);

void cw_rtp_init(void);

void cw_rtp_reload(void);

int cw_rtp_set_framems(struct cw_rtp *rtp, int ms);

#ifdef ENABLE_SRTP

/* Crypto suites */
#define CW_AES_CM_128_HMAC_SHA1_80 1
#define CW_AES_CM_128_HMAC_SHA1_32 2
#define CW_F8_128_HMAC_SHA1_80     3

#define MIKEY_SRTP_EALG_NULL     0
#define MIKEY_SRTP_EALG_AESCM    1
#define MIKEY_SRTP_AALG_NULL     0
#define MIKEY_SRTP_AALG_SHA1HMAC 1

typedef struct cw_policy cw_policy_t;
typedef int (*rtp_generate_key_cb)(struct cw_rtp *rtp,
                                   uint32_t ssrc,
                                   void *data);

unsigned int cw_rtp_get_ssrc(struct cw_rtp *rtp);
void cw_rtp_set_generate_key_cb(struct cw_rtp *rtp,
				  rtp_generate_key_cb cb);
int cw_rtp_add_policy(struct cw_rtp *rtp, cw_policy_t *policy);
cw_policy_t *cw_policy_alloc(void);
int cw_policy_set_suite(cw_policy_t *policy, int suite);
int cw_policy_set_master_key(cw_policy_t *policy,
			      const unsigned char *key, size_t key_len,
			      const unsigned char *salt, size_t salt_len);
int cw_policy_set_encr_alg(cw_policy_t *policy, int ealg);
int cw_policy_set_auth_alg(cw_policy_t *policy, int aalg);
void cw_policy_set_encr_keylen(cw_policy_t *policy, int ekeyl);
void cw_policy_set_auth_keylen(cw_policy_t *policy, int akeyl);
void cw_policy_set_srtp_auth_taglen(cw_policy_t *policy, int autht);
void cw_policy_set_srtp_encr_enable(cw_policy_t *policy, int enable);
void cw_policy_set_srtcp_encr_enable(cw_policy_t *policy, int enable);
void cw_policy_set_srtp_auth_enable(cw_policy_t *policy, int enable);
void cw_policy_set_ssrc(cw_policy_t *policy,
                          struct cw_rtp *rtp, 
			              uint32_t ssrc,
                          int inbound);
    
void cw_policy_destroy(cw_policy_t *policy);
int cw_get_random(unsigned char *key, size_t len);

#endif	/* ENABLE_SRTP */

struct cw_rtp
{
    udp_socket_info_t *rtp_sock_info;
    udp_socket_info_t *rtcp_sock_info;
	struct cw_frame f;
	uint8_t rawdata[8192 + CW_FRIENDLY_OFFSET];
	uint32_t ssrc;
	uint32_t lastts;
	uint32_t lastrxts;
	uint32_t lastividtimestamp;
	uint32_t lastovidtimestamp;
	uint32_t lastevent_seqno;
	uint32_t lastevent_startts;
	uint16_t lastevent_duration;
	char lastevent_code;
	int lasttxformat;
	int lastrxformat;
	int dtmfcount;
	int sendevent;
	uint32_t sendevent_startts;
	uint32_t sendevent_rtphdr;
	uint32_t sendevent_payload;
	uint32_t sendevent_duration;
	uint16_t sendevent_seqno;
	int nat;
	unsigned int flags;
	int framems;
	int rtplen;
	struct timeval rxcore;
	struct timeval txcore;
	struct timeval dtmfmute;
	struct cw_smoother *smoother;
	int *ioid;
	uint16_t seqno;
	uint16_t rxseqno;
	struct sched_context *sched;
	struct io_context *io;
	void *data;
	cw_rtp_callback callback;
	struct rtpPayloadType current_RTP_PT[MAX_RTP_PT];
	int rtp_lookup_code_cache_is_cw_format;	/* a cache for the result of rtp_lookup_code(): */
	int rtp_lookup_code_cache_code;
	int rtp_lookup_code_cache_result;
	int rtp_offered_from_local;
#ifdef ENABLE_SRTP
	srtp_t srtp;
	rtp_generate_key_cb key_cb;
#endif
};


#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _CALLWEAVER_RTP_H */
