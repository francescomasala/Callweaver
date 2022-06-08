/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * UDPTL support for T.38
 * 
 * Copyright (C) 2005, Steve Underwood, partly based on RTP code which is
 * Copyright (C) 1999-2004, Digium, Inc.
 *
 * Steve Underwood <steveu@coppice.org>
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
#include <inttypes.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/udp.h"
#include "openpbx/udptl.h"
#include "openpbx/frame.h"
#include "openpbx/logger.h"
#include "openpbx/options.h"
#include "openpbx/channel.h"
#include "openpbx/acl.h"
#include "openpbx/channel.h"
#include "openpbx/config.h"
#include "openpbx/lock.h"
#include "openpbx/utils.h"
#include "openpbx/cli.h"
#include "openpbx/unaligned.h"
#include "openpbx/utils.h"
#include "openpbx/stun.h"

#define UDPTL_MTU        1200

#if !defined(FALSE)
#define FALSE 0
#endif
#if !defined(TRUE)
#define TRUE (!FALSE)
#endif

static int udptldebug = 0;                /* Are we debugging? */
static struct sockaddr_in udptldebugaddr;    /* Debug packets to/from this host */
static int nochecksums = 0;
static int udptlfectype = 0;
static int udptlfecentries = 0;
static int udptlfecspan = 0;
static int udptlmaxdatagram = 0;

#define LOCAL_FAX_MAX_DATAGRAM      400
#define MAX_FEC_ENTRIES             5
#define MAX_FEC_SPAN                5

#define UDPTL_BUF_MASK              15

typedef struct
{
    int buf_len;
    uint8_t buf[LOCAL_FAX_MAX_DATAGRAM];
} udptl_fec_tx_buffer_t;

typedef struct
{
    int buf_len;
    uint8_t buf[LOCAL_FAX_MAX_DATAGRAM];
    int fec_len[MAX_FEC_ENTRIES];
    uint8_t fec[MAX_FEC_ENTRIES][LOCAL_FAX_MAX_DATAGRAM];
    int fec_span;
    int fec_entries;
} udptl_fec_rx_buffer_t;

struct opbx_udptl
{
    udp_socket_info_t *udptl_sock_info;
    char resp;
    struct opbx_frame f[16];
    unsigned char rawdata[8192 + OPBX_FRIENDLY_OFFSET];
    unsigned int lasteventseqn;
    int nat;
    int flags;
    int *ioid;
    struct sched_context *sched;
    struct io_context *io;
    void *data;
    opbx_udptl_callback callback;
    int udptl_offered_from_local;

    int created_sock_info;
    
    /*! This option indicates the error correction scheme used in transmitted UDPTL
        packets. */
    int error_correction_scheme;

    /*! This option indicates the number of error correction entries transmitted in
        UDPTL packets. */
    int error_correction_entries;

    /*! This option indicates the span of the error correction entries in transmitted
        UDPTL packets (FEC only). */
    int error_correction_span;

    /*! This option indicates the maximum size of a UDPTL packet that can be accepted by
        the remote device. */
    int far_max_datagram_size;

    /*! This option indicates the maximum size of a UDPTL packet that we are prepared to
        accept. */
    int local_max_datagram_size;

    int verbose;

    struct sockaddr_in far;

    int tx_seq_no;
    int rx_seq_no;
    int rx_expected_seq_no;

    udptl_fec_tx_buffer_t tx[UDPTL_BUF_MASK + 1];
    udptl_fec_rx_buffer_t rx[UDPTL_BUF_MASK + 1];
};

static struct opbx_udptl_protocol *protos = NULL;

static int udptl_rx_packet(struct opbx_udptl *s, uint8_t *buf, int len);
static int udptl_build_packet(struct opbx_udptl *s, uint8_t *buf, uint8_t *msg, int msg_len);

static inline int udptl_debug_test_addr(const struct sockaddr_in *addr)
{
    if (udptldebug == 0)
        return 0;
    if (udptldebugaddr.sin_addr.s_addr)
    {
        if (((ntohs(udptldebugaddr.sin_port) != 0)  &&  (udptldebugaddr.sin_port != addr->sin_port))
            ||
            (udptldebugaddr.sin_addr.s_addr != addr->sin_addr.s_addr))
        {
            return 0;
        }
    }
    return 1;
}
/*- End of function --------------------------------------------------------*/

static int decode_length(const uint8_t *buf, int limit, int *len, int *pvalue)
{
    if ((buf[*len] & 0x80) == 0)
    {
        if (*len >= limit)
            return -1;
        *pvalue = buf[*len];
        (*len)++;
        return 0;
    }
    if ((buf[*len] & 0x40) == 0)
    {
        if (*len >= limit - 1)
            return -1;
        *pvalue = (buf[*len] & 0x3F) << 8;
        (*len)++;
        *pvalue |= buf[*len];
        (*len)++;
        return 0;
    }
    if (*len >= limit)
        return -1;
    *pvalue = (buf[*len] & 0x3F) << 14;
    (*len)++;
    /* Indicate we have a fragment */
    return 1;
}
/*- End of function --------------------------------------------------------*/

static int decode_open_type(const uint8_t *buf, int limit, int *len, const uint8_t **p_object, int *p_num_octets)
{
    int octet_cnt;
    int octet_idx;
    int stat;
    int i;
    const uint8_t **pbuf;

    for (octet_idx = 0, *p_num_octets = 0;  ;  octet_idx += octet_cnt)
    {
        if ((stat = decode_length(buf, limit, len, &octet_cnt)) < 0)
            return -1;
        if (octet_cnt > 0)
        {
            *p_num_octets += octet_cnt;

            pbuf = &p_object[octet_idx];
            i = 0;
            /* Make sure the buffer contains at least the number of bits requested */
            if ((*len + octet_cnt) > limit)
                return -1;

            *pbuf = &buf[*len];
            *len += octet_cnt;
        }
        if (stat == 0)
            break;
    }
    return 0;
}
/*- End of function --------------------------------------------------------*/

static int encode_length(uint8_t *buf, int *len, int value)
{
    int multiplier;

    if (value < 0x80)
    {
        /* 1 octet */
        buf[*len] = value;
        (*len)++;
        return value;
    }
    if (value < 0x4000)
    {
        /* 2 octets */
        /* Set the first bit of the first octet */
        buf[*len] = ((0x8000 | value) >> 8) & 0xFF;
        (*len)++;
        buf[*len] = value & 0xFF;
        (*len)++;
        return value;
    }
    /* Fragmentation */
    multiplier = (value < 0x10000)  ?  (value >> 14)  :  4;
    /* Set the first 2 bits of the octet */
    buf[*len] = 0xC0 | multiplier;
    (*len)++;
    return multiplier << 14;
}
/*- End of function --------------------------------------------------------*/

static int encode_open_type(uint8_t *buf, int *len, const uint8_t *data, int num_octets)
{
    int enclen;
    int octet_idx;
    uint8_t zero_byte;

    /* If open type is of zero length, add a single zero byte (10.1) */
    if (num_octets == 0)
    {
        zero_byte = 0;
        data = &zero_byte;
        num_octets = 1;
    }
    /* Encode the open type */
    for (octet_idx = 0;  ;  num_octets -= enclen, octet_idx += enclen)
    {
        if ((enclen = encode_length(buf, len, num_octets)) < 0)
            return -1;
        if (enclen > 0)
        {
            memcpy(&buf[*len], &data[octet_idx], enclen);
            *len += enclen;
        }
        if (enclen >= num_octets)
            break;
    }

    return 0;
}
/*- End of function --------------------------------------------------------*/

static int udptl_rx_packet(struct opbx_udptl *s, uint8_t *buf, int len)
{
    int stat;
    int stat2;
    int i;
    int j;
    int k;
    int l;
    int m;
    int x;
    int limit;
    int which;
    int ptr;
    int count;
    int total_count;
    int seq_no;
    const uint8_t *msg;
    const uint8_t *data;
    int msg_len;
    int repaired[16];
    const uint8_t *bufs[16];
    int lengths[16];
    int span;
    int entries;
    int msg_no;

    ptr = 0;
    msg_no = 0;
    s->f[0].prev = NULL;
    s->f[0].next = NULL;

    /* Decode seq_number */
    if (ptr + 2 > len)
        return -1;
    seq_no = (buf[0] << 8) | buf[1];
    ptr += 2;

    /* Break out the primary packet */
    if ((stat = decode_open_type(buf, len, &ptr, &msg, &msg_len)) != 0)
        return -1;
    /* Decode error_recovery */
    if (ptr + 1 > len)
        return -1;
    if ((buf[ptr++] & 0x80) == 0)
    {
        /* Secondary packet mode for error recovery */
        /* We might have the packet we want, but we need to check through
           the redundant stuff, and verify the integrity of the UDPTL.
           This greatly reduces our chances of accepting garbage. */
        total_count = 0;
        do
        {
            if ((stat2 = decode_length(buf, len, &ptr, &count)) < 0)
                return -1;
            for (i = 0;  i < count;  i++)
            {
                if ((stat = decode_open_type(buf, len, &ptr, &bufs[total_count + i], &lengths[total_count + i])) != 0)
                    return -1;
            }
            total_count += count;
        }
        while (stat2 > 0);
        /* We should now be exactly at the end of the packet. If not, this
           is a fault. */
        if (ptr != len)
            return -1;
        if (seq_no > s->rx_seq_no)
        {
            /* We received a later packet than we expected, so we need to check if we can fill in the gap from the
               secondary packets. */
            /* Step through in reverse order, so we go oldest to newest */
            for (i = total_count;  i > 0;  i--)
            {
                if (seq_no - i >= s->rx_seq_no)
                {
                    /* This one wasn't seen before */
                    /* Process the secondary packet */
                    //fprintf(stderr, "Secondary %d, len %d\n", seq_no - i, lengths[i - 1]);
                    s->f[msg_no].frametype = OPBX_FRAME_MODEM;
                    s->f[msg_no].subclass = OPBX_MODEM_T38;

                    s->f[msg_no].mallocd = 0;
                    s->f[msg_no].seq_no = seq_no - i;
                    s->f[msg_no].tx_copies = 1;
                    s->f[msg_no].datalen = lengths[i - 1];
                    s->f[msg_no].data = (uint8_t *) bufs[i - 1];
                    s->f[msg_no].offset = 0;
                    s->f[msg_no].src = "UDPTL";
                    if (msg_no > 0)
                    {
                        s->f[msg_no].prev = &s->f[msg_no - 1];
                        s->f[msg_no - 1].next = &s->f[msg_no];
                    }
                    s->f[msg_no].next = NULL;
                    msg_no++;
                }
            }
        }
    }
    else
    {
        /* FEC mode for error recovery */
        /* Our buffers cannot tolerate overlength packets in FEC mode */
        if (msg_len > LOCAL_FAX_MAX_DATAGRAM)
            return -1;
        /* Update any missed slots in the buffer */
        for (  ;  seq_no > s->rx_seq_no;  s->rx_seq_no++)
        {
            x = s->rx_seq_no & UDPTL_BUF_MASK;
            s->rx[x].buf_len = -1;
            s->rx[x].fec_len[0] = 0;
            s->rx[x].fec_span = 0;
            s->rx[x].fec_entries = 0;
        }

        x = seq_no & UDPTL_BUF_MASK;

        memset(repaired, 0, sizeof(repaired));

        /* Save the new packet */
        memcpy(s->rx[x].buf, msg, msg_len);
        s->rx[x].buf_len = msg_len;
        repaired[x] = TRUE;

        /* Decode the FEC packets */
        /* The span is defined as an unconstrained integer, but will never be more
           than a small value. */
        if (ptr + 2 > len)
            return -1;
        if (buf[ptr++] != 1)
            return -1;
        span = buf[ptr++];
        s->rx[x].fec_span = span;

        /* The number of entries is defined as a length, but will only ever be a small
           value. Treat it as such. */
        if (ptr + 1 > len)
            return -1;
        entries = buf[ptr++];
        s->rx[x].fec_entries = entries;

        /* Decode the elements */
        for (i = 0;  i < entries;  i++)
        {
            if ((stat = decode_open_type(buf, len, &ptr, &data, &s->rx[x].fec_len[i])) != 0)
                return -1;
            if (s->rx[x].fec_len[i] > LOCAL_FAX_MAX_DATAGRAM)
                return -1;

            /* Save the new FEC data */
            memcpy(s->rx[x].fec[i], data, s->rx[x].fec_len[i]);
#if 0
            fprintf(stderr, "FEC: ");
            for (j = 0;  j < s->rx[x].fec_len[i];  j++)
                fprintf(stderr, "%02X ", data[j]);
            fprintf(stderr, "\n");
#endif
        }
        /* We should now be exactly at the end of the packet. If not, this
           is a fault. */
        if (ptr != len)
            return -1;
        /* See if we can reconstruct anything which is missing */
        /* TODO: this does not comprehensively hunt back and repair everything that is possible */
        for (l = x;  l != ((x - (16 - span*entries)) & UDPTL_BUF_MASK);  l = (l - 1) & UDPTL_BUF_MASK)
        {
            if (s->rx[l].fec_len[0] <= 0)
                continue;
            for (m = 0;  m < s->rx[l].fec_entries;  m++)
            {
                limit = (l + m) & UDPTL_BUF_MASK;
                for (which = -1, k = (limit - s->rx[l].fec_span*s->rx[l].fec_entries) & UDPTL_BUF_MASK;  k != limit;  k = (k + s->rx[l].fec_entries) & UDPTL_BUF_MASK)
                {
                    if (s->rx[k].buf_len <= 0)
                        which = (which == -1)  ?  k  :  -2;
                }
                if (which >= 0)
                {
                    /* Repairable */
                    for (j = 0;  j < s->rx[l].fec_len[m];  j++)
                    {
                        s->rx[which].buf[j] = s->rx[l].fec[m][j];
                        for (k = (limit - s->rx[l].fec_span*s->rx[l].fec_entries) & UDPTL_BUF_MASK;  k != limit;  k = (k + s->rx[l].fec_entries) & UDPTL_BUF_MASK)
                            s->rx[which].buf[j] ^= (s->rx[k].buf_len > j)  ?  s->rx[k].buf[j]  :  0;
                    }
                    s->rx[which].buf_len = s->rx[l].fec_len[m];
                    repaired[which] = TRUE;
                }
            }
        }
        /* Now play any new packets forwards in time */
        for (l = (x + 1) & UDPTL_BUF_MASK, j = seq_no - UDPTL_BUF_MASK;  l != x;  l = (l + 1) & UDPTL_BUF_MASK, j++)
        {
            if (repaired[l])
            {
                /* Process the repaired packet */
                //fprintf(stderr, "Fixed packet %d, len %d\n", j, l);
                s->f[msg_no].frametype = OPBX_FRAME_MODEM;
                s->f[msg_no].subclass = OPBX_MODEM_T38;
            
                s->f[msg_no].mallocd = 0;
                s->f[msg_no].seq_no = j;
                s->f[msg_no].tx_copies = 1;
                s->f[msg_no].datalen = s->rx[l].buf_len;
                s->f[msg_no].data = s->rx[l].buf;
                s->f[msg_no].offset = 0;
                s->f[msg_no].src = "UDPTL";
                if (msg_no > 0)
                {
                    s->f[msg_no].prev = &s->f[msg_no - 1];
                    s->f[msg_no - 1].next = &s->f[msg_no];
                }
                s->f[msg_no].next = NULL;
                msg_no++;
            }
        }
    }
    /* If packets are received out of sequence, we may have already processed this packet
       from the error recovery information in a packet already received. */
    if (seq_no >= s->rx_seq_no)
    {
        /* Process the primary packet */
        //fprintf(stderr, "Primary %d, len %d\n", seq_no, msg_len);
        s->f[msg_no].frametype = OPBX_FRAME_MODEM;
        s->f[msg_no].subclass = OPBX_MODEM_T38;
            
        s->f[msg_no].mallocd = 0;
        s->f[msg_no].seq_no = seq_no;
        s->f[msg_no].tx_copies = 1;
        s->f[msg_no].datalen = msg_len;
        s->f[msg_no].data = (uint8_t *) msg;
        s->f[msg_no].offset = 0;
        s->f[msg_no].src = "UDPTL";
        if (msg_no > 0)
        {
            s->f[msg_no].prev = &s->f[msg_no - 1];
            s->f[msg_no - 1].next = &s->f[msg_no];
        }
        s->f[msg_no].next = NULL;
    }
    s->rx_seq_no = seq_no + 1;
    return 0;
}
/*- End of function --------------------------------------------------------*/

static int udptl_build_packet(struct opbx_udptl *s, uint8_t *buf, uint8_t *msg, int msg_len)
{
    uint8_t fec[LOCAL_FAX_MAX_DATAGRAM];
    int i;
    int j;
    int seq;
    int entry;
    int entries;
    int span;
    int m;
    int len;
    int limit;
    int high_tide;

    seq = s->tx_seq_no & 0xFFFF;

    /* Map the sequence number to an entry in the circular buffer */
    entry = seq & UDPTL_BUF_MASK;

    /* We save the message in a circular buffer, for generating FEC or
       redundancy sets later on. */
    s->tx[entry].buf_len = msg_len;
    memcpy(s->tx[entry].buf, msg, msg_len);
    
    /* Build the UDPTLPacket */

    len = 0;
    /* Encode the sequence number */
    buf[len++] = (seq >> 8) & 0xFF;
    buf[len++] = seq & 0xFF;

    /* Encode the primary packet */
    if (encode_open_type(buf, &len, msg, msg_len) < 0)
        return -1;

    /* Encode the appropriate type of error recovery information */
    switch (s->error_correction_scheme)
    {
    case UDPTL_ERROR_CORRECTION_NONE:
        /* Encode the error recovery type */
        buf[len++] = 0x00;
        /* The number of entries will always be zero, so it is pointless allowing
           for the fragmented case here. */
        if (encode_length(buf, &len, 0) < 0)
            return -1;
        break;
    case UDPTL_ERROR_CORRECTION_REDUNDANCY:
        /* Encode the error recovery type */
        buf[len++] = 0x00;
        if (s->tx_seq_no > s->error_correction_entries)
            entries = s->error_correction_entries;
        else
            entries = s->tx_seq_no;
        /* The number of entries will always be small, so it is pointless allowing
           for the fragmented case here. */
        if (encode_length(buf, &len, entries) < 0)
            return -1;
        /* Encode the elements */
        for (i = 0;  i < entries;  i++)
        {
            j = (entry - i - 1) & UDPTL_BUF_MASK;
            if (encode_open_type(buf, &len, s->tx[j].buf, s->tx[j].buf_len) < 0)
                return -1;
        }
        break;
    case UDPTL_ERROR_CORRECTION_FEC:
        span = s->error_correction_span;
        entries = s->error_correction_entries;
        if (seq < s->error_correction_span*s->error_correction_entries)
        {
            /* In the initial stages, wind up the FEC smoothly */
            entries = seq/s->error_correction_span;
            if (seq < s->error_correction_span)
                span = 0;
        }
        /* Encode the error recovery type */
        buf[len++] = 0x80;
        /* Span is defined as an inconstrained integer, which it dumb. It will only
           ever be a small value. Treat it as such. */
        buf[len++] = 1;
        buf[len++] = span;
        /* The number of entries is defined as a length, but will only ever be a small
           value. Treat it as such. */
        buf[len++] = entries;
        for (m = 0;  m < entries;  m++)
        {
            /* Make an XOR'ed entry the maximum length */
            limit = (entry + m) & UDPTL_BUF_MASK;
            high_tide = 0;
            for (i = (limit - span*entries) & UDPTL_BUF_MASK;  i != limit;  i = (i + entries) & UDPTL_BUF_MASK)
            {
                if (high_tide < s->tx[i].buf_len)
                {
                    for (j = 0;  j < high_tide;  j++)
                        fec[j] ^= s->tx[i].buf[j];
                    for (  ;  j < s->tx[i].buf_len;  j++)
                        fec[j] = s->tx[i].buf[j];
                    high_tide = s->tx[i].buf_len;
                }
                else
                {
                    for (j = 0;  j < s->tx[i].buf_len;  j++)
                        fec[j] ^= s->tx[i].buf[j];
                }
            }
            if (encode_open_type(buf, &len, fec, high_tide) < 0)
                return -1;
        }
        break;
    }

    if (s->verbose)
        fprintf(stderr, "\n");

    s->tx_seq_no++;
    return len;
}

int opbx_udptl_fd(struct opbx_udptl *udptl)
{
    return udp_socket_fd(udptl->udptl_sock_info);
}

udp_socket_info_t *opbx_udptl_udp_socket(struct opbx_udptl *udptl,
                                         udp_socket_info_t *sock_info)
{
    udp_socket_info_t *old;
    
    old = udptl->udptl_sock_info;
    if (sock_info)
        udptl->udptl_sock_info = sock_info;
    return old;
}

void opbx_udptl_set_data(struct opbx_udptl *udptl, void *data)
{
    udptl->data = data;
}

void opbx_udptl_set_callback(struct opbx_udptl *udptl, opbx_udptl_callback callback)
{
    udptl->callback = callback;
}

void opbx_udptl_setnat(struct opbx_udptl *udptl, int nat)
{
    udptl->nat = nat;
    udp_socket_set_nat(udptl->udptl_sock_info, nat);
}

static int udptlread(int *id, int fd, short events, void *cbdata)
{
    struct opbx_udptl *udptl = cbdata;
    struct opbx_frame *f;

    if ((f = opbx_udptl_read(udptl)))
    {
        if (udptl->callback)
            udptl->callback(udptl, f, udptl->data);
    }
    return 1;
}

struct opbx_frame *opbx_udptl_read(struct opbx_udptl *udptl)
{
    int res;
    int actions;
    struct sockaddr_in sin;
    socklen_t len;
    char iabuf[INET_ADDRSTRLEN];
    uint16_t *udptlheader;
    static struct opbx_frame null_frame = { OPBX_FRAME_NULL, };

    len = sizeof(sin);
    
    /* Cache where the header will go */
    res = udp_socket_recvfrom(udptl->udptl_sock_info,
                              udptl->rawdata + OPBX_FRIENDLY_OFFSET,
                              sizeof(udptl->rawdata) - OPBX_FRIENDLY_OFFSET,
                              0,
                              (struct sockaddr *) &sin,
                              &len,
                              &actions);
    udptlheader = (uint16_t *)(udptl->rawdata + OPBX_FRIENDLY_OFFSET);
    if (res < 0)
    {
        if (errno != EAGAIN)
            opbx_log(LOG_WARNING, "UDPTL read error: %s\n", strerror(errno));
        if (errno == EBADF)
            CRASH;
        return &null_frame;
    }
    if ((actions & 1))
    {
        if (option_debug || udptldebug)
            opbx_log(LOG_DEBUG, "UDPTL NAT: Using address %s:%d\n", opbx_inet_ntoa(iabuf, sizeof(iabuf), udp_socket_get_them(udptl->udptl_sock_info)->sin_addr), ntohs(udp_socket_get_them(udptl->udptl_sock_info)->sin_port));
    }

    if (udptl_debug_test_addr(&sin))
    {
        opbx_verbose("Got UDPTL packet from %s:%d (len %d)\n",
            opbx_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), ntohs(sin.sin_port), res);
    }
#if 0
    printf("Got UDPTL packet from %s:%d (len %d)\n", opbx_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), ntohs(sin.sin_port), res);
#endif
    udptl_rx_packet(udptl, udptl->rawdata + OPBX_FRIENDLY_OFFSET, res);

    return &udptl->f[0];
}

void opbx_udptl_offered_from_local(struct opbx_udptl *udptl, int local)
{
    if (udptl)
        udptl->udptl_offered_from_local = local;
    else
        opbx_log(LOG_WARNING, "udptl structure is null\n");
}

int opbx_udptl_get_error_correction_scheme(struct opbx_udptl *udptl)
{
    if (udptl)
        return udptl->error_correction_scheme;
    opbx_log(LOG_WARNING, "udptl structure is null\n");
    return -1;
}

void opbx_udptl_set_error_correction_scheme(struct opbx_udptl *udptl, int ec)
{
    if (udptl)
    {
        switch (ec)
        {
        case UDPTL_ERROR_CORRECTION_FEC:
            udptl->error_correction_scheme = UDPTL_ERROR_CORRECTION_FEC;
            break;
        case UDPTL_ERROR_CORRECTION_REDUNDANCY:
            udptl->error_correction_scheme = UDPTL_ERROR_CORRECTION_REDUNDANCY;
            break;
        case UDPTL_ERROR_CORRECTION_NONE:
            udptl->error_correction_scheme = UDPTL_ERROR_CORRECTION_NONE;
            break;
        default:
            opbx_log(LOG_WARNING, "error correction parameter invalid");
            break;
        }
    }
    else
    {
        opbx_log(LOG_WARNING, "udptl structure is null\n");
    }
}

int opbx_udptl_get_local_max_datagram(struct opbx_udptl *udptl)
{
    if (udptl)
        return udptl->local_max_datagram_size;
    opbx_log(LOG_WARNING, "udptl structure is null\n");
    return -1;
}

int opbx_udptl_get_far_max_datagram(struct opbx_udptl *udptl)
{
    if (udptl)
        return udptl->far_max_datagram_size;
    opbx_log(LOG_WARNING, "udptl structure is null\n");
    return -1;
}

void opbx_udptl_set_local_max_datagram(struct opbx_udptl *udptl, int max_datagram)
{
    if (udptl)
        udptl->local_max_datagram_size = max_datagram;
    else
        opbx_log(LOG_WARNING, "udptl structure is null\n");
}

void opbx_udptl_set_far_max_datagram(struct opbx_udptl *udptl, int max_datagram)
{
    if (udptl)
        udptl->far_max_datagram_size = max_datagram;
    else
        opbx_log(LOG_WARNING, "udptl structure is null\n");
}

struct opbx_udptl *opbx_udptl_new_with_sock_info(struct sched_context *sched,
                                                 struct io_context *io,
                                                 int callbackmode,
                                                 udp_socket_info_t *sock_info)
{
    struct opbx_udptl *udptl;
    int i;

    if ((udptl = malloc(sizeof(struct opbx_udptl))) == NULL)
        return NULL;
    memset(udptl, 0, sizeof(struct opbx_udptl));

    if (udptlfectype == 2)
        udptl->error_correction_scheme = UDPTL_ERROR_CORRECTION_FEC;
    else if (udptlfectype == 1)
        udptl->error_correction_scheme = UDPTL_ERROR_CORRECTION_REDUNDANCY;
    else
        udptl->error_correction_scheme = UDPTL_ERROR_CORRECTION_NONE;
    udptl->error_correction_span = udptlfecspan;
    udptl->error_correction_entries = udptlfecentries;
    
    udptl->far_max_datagram_size = udptlmaxdatagram;
    udptl->local_max_datagram_size = udptlmaxdatagram;

    memset(&udptl->rx, 0, sizeof(udptl->rx));
    memset(&udptl->tx, 0, sizeof(udptl->tx));
    for (i = 0;  i <= UDPTL_BUF_MASK;  i++)
    {
        udptl->rx[i].buf_len = -1;
        udptl->tx[i].buf_len = -1;
    }
    /* This sock_info should already be bound to an address */
    udptl->udptl_sock_info = sock_info;
    if (io  &&  sched  &&  callbackmode)
    {
        /* Operate this one in a callback mode */
        udptl->sched = sched;
        udptl->io = io;
        udptl->ioid = NULL;
    }
    udptl->created_sock_info = FALSE;
    return udptl;
}

int opbx_udptl_set_active(struct opbx_udptl *udptl, int active)
{
    if (udptl->sched  &&  udptl->io)
    {
        if (active)
        {
            if (udptl->ioid == NULL)
                udptl->ioid = opbx_io_add(udptl->io, udp_socket_fd(udptl->udptl_sock_info), udptlread, OPBX_IO_IN, udptl);
        }
        else
        {
            if (udptl->ioid)
            {
                opbx_io_remove(udptl->io, udptl->ioid);
                udptl->ioid = NULL;
            }
        }
    }
    return 0;
}

int opbx_udptl_settos(struct opbx_udptl *udptl, int tos)
{
    return udp_socket_set_tos(udptl->udptl_sock_info, tos);
}

void opbx_udptl_set_peer(struct opbx_udptl *udptl, struct sockaddr_in *them)
{
    udp_socket_set_them(udptl->udptl_sock_info, them);
}

void opbx_udptl_get_peer(struct opbx_udptl *udptl, struct sockaddr_in *them)
{
    memcpy(them, udp_socket_get_them(udptl->udptl_sock_info), sizeof(*them));
}

void opbx_udptl_get_us(struct opbx_udptl *udptl, struct sockaddr_in *us)
{
    memcpy(us, udp_socket_get_apparent_us(udptl->udptl_sock_info), sizeof(*us));
}

int opbx_udptl_get_stunstate(struct opbx_udptl *udptl)
{
    if (udptl)
        return udp_socket_get_stunstate(udptl->udptl_sock_info);
    return 0;
}

void opbx_udptl_stop(struct opbx_udptl *udptl)
{
    udp_socket_restart(udptl->udptl_sock_info);
}

void opbx_udptl_destroy(struct opbx_udptl *udptl)
{
    if (udptl->ioid)
        opbx_io_remove(udptl->io, udptl->ioid);
    //if (udptl->created_sock_info)
    //    udp_socket_destroy_group(udptl->udptl_sock_info);
    free(udptl);
}

int opbx_udptl_write(struct opbx_udptl *s, struct opbx_frame *f)
{
    int len;
    int res;
    int copies;
    int i;
    uint8_t buf[LOCAL_FAX_MAX_DATAGRAM];
    char iabuf[INET_ADDRSTRLEN];
    const struct sockaddr_in *them;

    them = udp_socket_get_them(s->udptl_sock_info);

    /* If we have no peer, return immediately */    
    if (them->sin_addr.s_addr == INADDR_ANY)
        return 0;

    /* If there is no data length, return immediately */
    if (f->datalen == 0)
        return 0;
    
    if (f->frametype != OPBX_FRAME_MODEM)
    {
        opbx_log(LOG_WARNING, "UDPTL can only send T.38 data\n");
        return -1;
    }
    /* Cook up the UDPTL packet, with the relevant EC info. */
    len = udptl_build_packet(s, buf, f->data, f->datalen);

    if (len > 0  &&  them->sin_port  &&  them->sin_addr.s_addr)
    {
#if 0
        printf("Sending %d copies of %d bytes of UDPTL data to %s:%d\n", f->tx_copies, len, opbx_inet_ntoa(iabuf, sizeof(iabuf), udptl->them.sin_addr), ntohs(udptl->them.sin_port));
#endif
        copies = (f->tx_copies > 0)  ?  f->tx_copies  :  1;

#if 1	
	opbx_log(LOG_DEBUG,
	    "Sending %d bytes, %d copies of UDPTL data to %s:%d\n", 
	    len, copies, 
	    opbx_inet_ntoa(iabuf, sizeof(iabuf), them->sin_addr), 
	    ntohs(them->sin_port)
	);
#endif

        for (i = 0;  i < copies;  i++)
        {
            if ((res = udp_socket_sendto(s->udptl_sock_info, buf, len, 0)) < 0)
                opbx_log(LOG_NOTICE, "UDPTL Transmission error to %s:%d: %s\n", opbx_inet_ntoa(iabuf, sizeof(iabuf), them->sin_addr), ntohs(them->sin_port), strerror(errno));
        }
#if 0
        printf("Sent %d bytes of UDPTL data to %s:%d\n", res, opbx_inet_ntoa(iabuf, sizeof(iabuf), 
	    them->sin_addr), ntohs(s->them.sin_port));
#endif
        if (udptl_debug_test_addr(them))
        {
            opbx_verbose("Sent UDPTL packet to %s:%d (seq %d, len %d)\n",
                    opbx_inet_ntoa(iabuf, sizeof(iabuf), them->sin_addr),
                    ntohs(them->sin_port), (s->tx_seq_no - 1) & 0xFFFF, len);
        }
    }
    return 0;
}

void opbx_udptl_proto_unregister(struct opbx_udptl_protocol *proto)
{
    struct opbx_udptl_protocol *cur;
    struct opbx_udptl_protocol *prev;

    for (cur = protos, prev = NULL;  cur;  prev = cur, cur = cur->next)
    {
        if (cur == proto)
        {
            if (prev)
                prev->next = proto->next;
            else
                protos = proto->next;
            return;
        }
    }
}

int opbx_udptl_proto_register(struct opbx_udptl_protocol *proto)
{
    struct opbx_udptl_protocol *cur;

    for (cur = protos;  cur;  cur = cur->next)
    {
        if (cur->type == proto->type)
        {
            opbx_log(LOG_WARNING, "Tried to register same protocol '%s' twice\n", cur->type);
            return -1;
        }
    }
    proto->next = protos;
    protos = proto;
    return 0;
}

static struct opbx_udptl_protocol *get_proto(struct opbx_channel *chan)
{
    struct opbx_udptl_protocol *cur;

    for (cur = protos;  cur;  cur = cur->next)
    {
        if (cur->type == chan->type)
            return cur;
    }
    return NULL;
}

enum opbx_bridge_result opbx_udptl_bridge(struct opbx_channel *c0, struct opbx_channel *c1, int flags, struct opbx_frame **fo, struct opbx_channel **rc)
{
    struct opbx_frame *f;
    struct opbx_channel *who;
    struct opbx_channel *cs[3];
    struct opbx_udptl *p0;
    struct opbx_udptl *p1;
    struct opbx_udptl_protocol *pr0;
    struct opbx_udptl_protocol *pr1;
    struct sockaddr_in ac0;
    struct sockaddr_in ac1;
    struct sockaddr_in t0;
    struct sockaddr_in t1;
    char iabuf[INET_ADDRSTRLEN];
    void *pvt0;
    void *pvt1;
    int to;
    
    opbx_mutex_lock(&c0->lock);
    while (opbx_mutex_trylock(&c1->lock))
    {
        opbx_mutex_unlock(&c0->lock);
        usleep(1);
        opbx_mutex_lock(&c0->lock);
    }
    pr0 = get_proto(c0);
    pr1 = get_proto(c1);
    if (!pr0)
    {
        opbx_log(LOG_WARNING, "Can't find native functions for channel '%s'\n", c0->name);
        opbx_mutex_unlock(&c0->lock);
        opbx_mutex_unlock(&c1->lock);
        return OPBX_BRIDGE_FAILED;
    }
    if (!pr1)
    {
        opbx_log(LOG_WARNING, "Can't find native functions for channel '%s'\n", c1->name);
        opbx_mutex_unlock(&c0->lock);
        opbx_mutex_unlock(&c1->lock);
        return OPBX_BRIDGE_FAILED;
    }
    pvt0 = c0->tech_pvt;
    pvt1 = c1->tech_pvt;
    p0 = pr0->get_udptl_info(c0);
    p1 = pr1->get_udptl_info(c1);
    if (!p0  ||  !p1)
    {
        /* Somebody doesn't want to play... */
        opbx_mutex_unlock(&c0->lock);
        opbx_mutex_unlock(&c1->lock);
        return OPBX_BRIDGE_FAILED_NOWARN;
    }
    if (pr0->set_udptl_peer(c0, p1))
    {
        opbx_log(LOG_WARNING, "Channel '%s' failed to talk to '%s'\n", c0->name, c1->name);
    }
    else
    {
        /* Store UDPTL peer */
        opbx_udptl_get_peer(p1, &ac1);
    }
    if (pr1->set_udptl_peer(c1, p0))
    {
        opbx_log(LOG_WARNING, "Channel '%s' failed to talk back to '%s'\n", c1->name, c0->name);
    }
    else
    {
        /* Store UDPTL peer */
        opbx_udptl_get_peer(p0, &ac0);
    }
    opbx_mutex_unlock(&c0->lock);
    opbx_mutex_unlock(&c1->lock);
    cs[0] = c0;
    cs[1] = c1;
    cs[2] = NULL;
    for (;;)
    {
        if ((c0->tech_pvt != pvt0)
            ||
            (c1->tech_pvt != pvt1)
            ||
            (c0->masq  ||  c0->masqr  ||  c1->masq  ||  c1->masqr))
        {
            opbx_log(LOG_DEBUG, "Oooh, something is weird, backing out\n");
            /* Tell it to try again later */
            return OPBX_BRIDGE_RETRY;
        }
        to = -1;
        opbx_udptl_get_peer(p1, &t1);
        opbx_udptl_get_peer(p0, &t0);
        if (inaddrcmp(&t1, &ac1))
        {
            opbx_log(LOG_DEBUG, "Oooh, '%s' changed end address to %s:%d\n", 
                c1->name, opbx_inet_ntoa(iabuf, sizeof(iabuf), t1.sin_addr), ntohs(t1.sin_port));
            opbx_log(LOG_DEBUG, "Oooh, '%s' was %s:%d\n", 
                c1->name, opbx_inet_ntoa(iabuf, sizeof(iabuf), ac1.sin_addr), ntohs(ac1.sin_port));
            memcpy(&ac1, &t1, sizeof(ac1));
        }
        if (inaddrcmp(&t0, &ac0))
        {
            opbx_log(LOG_DEBUG, "Oooh, '%s' changed end address to %s:%d\n", 
                c0->name, opbx_inet_ntoa(iabuf, sizeof(iabuf), t0.sin_addr), ntohs(t0.sin_port));
            opbx_log(LOG_DEBUG, "Oooh, '%s' was %s:%d\n", 
                c0->name, opbx_inet_ntoa(iabuf, sizeof(iabuf), ac0.sin_addr), ntohs(ac0.sin_port));
            memcpy(&ac0, &t0, sizeof(ac0));
        }
        if ((who = opbx_waitfor_n(cs, 2, &to)) == 0)
        {
            opbx_log(LOG_DEBUG, "Ooh, empty read...\n");
            /* Check for hangup / whentohangup */
            if (opbx_check_hangup(c0)  ||  opbx_check_hangup(c1))
                break;
            continue;
        }
        if ((f = opbx_read(who)) == 0)
        {
            *fo = f;
            *rc = who;
            opbx_log(LOG_DEBUG, "Oooh, got a %s\n", (f)  ?  "digit"  :  "hangup");
            /* That's all we needed */
            return OPBX_BRIDGE_COMPLETE;
        }
        if (f->frametype == OPBX_FRAME_MODEM)
        {
            /* Forward T.38 frames if they happen upon us */
            if (who == c0)
                opbx_write(c1, f);
            else if (who == c1)
                opbx_write(c0, f);
        }
        opbx_frfree(f);
        /* Swap priority. Not that it's a big deal at this point */
        cs[2] = cs[0];
        cs[0] = cs[1];
        cs[1] = cs[2];
    }
    return OPBX_BRIDGE_FAILED;
}

static int udptl_do_debug_ip(int fd, int argc, char *argv[])
{
    struct hostent *hp;
    struct opbx_hostent ahp;
    char iabuf[INET_ADDRSTRLEN];
    int port;
    char *p;
    char *arg;

    port = 0;
    if (argc != 4)
        return RESULT_SHOWUSAGE;
    arg = argv[3];
    p = strstr(arg, ":");
    if (p)
    {
        *p = '\0';
        p++;
        port = atoi(p);
    }
    hp = opbx_gethostbyname(arg, &ahp);
    if (hp == NULL)
        return RESULT_SHOWUSAGE;
    udptldebugaddr.sin_family = AF_INET;
    memcpy(&udptldebugaddr.sin_addr, hp->h_addr, sizeof(udptldebugaddr.sin_addr));
    udptldebugaddr.sin_port = htons(port);
    if (port == 0)
        opbx_cli(fd, "UDPTL Debugging Enabled for IP: %s\n", opbx_inet_ntoa(iabuf, sizeof(iabuf), udptldebugaddr.sin_addr));
    else
        opbx_cli(fd, "UDPTL Debugging Enabled for IP: %s:%d\n", opbx_inet_ntoa(iabuf, sizeof(iabuf), udptldebugaddr.sin_addr), port);
    udptldebug = 1;
    return RESULT_SUCCESS;
}

static int udptl_do_debug(int fd, int argc, char *argv[])
{
    if (argc != 2)
    {
        if (argc != 4)
            return RESULT_SHOWUSAGE;
        return udptl_do_debug_ip(fd, argc, argv);
    }
    udptldebug = 1;
    memset(&udptldebugaddr,0,sizeof(udptldebugaddr));
    opbx_cli(fd, "UDPTL Debugging Enabled\n");
    return RESULT_SUCCESS;
}
   
static int udptl_no_debug(int fd, int argc, char *argv[])
{
    if (argc !=3)
        return RESULT_SHOWUSAGE;
    udptldebug = 0;
    opbx_cli(fd,"UDPTL Debugging Disabled\n");
    return RESULT_SUCCESS;
}

static char debug_usage[] =
  "Usage: udptl debug [ip host[:port]]\n"
  "       Enable dumping of all UDPTL packets to and from host.\n";

static char no_debug_usage[] =
  "Usage: udptl no debug\n"
  "       Disable all UDPTL debugging\n";

static struct opbx_cli_entry  cli_debug_ip =
{{ "udptl", "debug", "ip", NULL } , udptl_do_debug, "Enable UDPTL debugging on IP", debug_usage };

static struct opbx_cli_entry  cli_debug =
{{ "udptl", "debug", NULL } , udptl_do_debug, "Enable UDPTL debugging", debug_usage };

static struct opbx_cli_entry  cli_no_debug =
{{ "udptl", "no", "debug", NULL } , udptl_no_debug, "Disable UDPTL debugging", no_debug_usage };

void opbx_udptl_reload(void)
{
    struct opbx_config *cfg;
    char *s;

    udptlfectype = 0;
    udptlfecentries = 0;
    udptlfecspan = 0;
    udptlmaxdatagram = 0;

    if ((cfg = opbx_config_load("udptl.conf")))
    {
        if ((s = opbx_variable_retrieve(cfg, "general", "udptlchecksums")))
        {
#ifdef SO_NO_CHECK
            if (opbx_false(s))
                nochecksums = 1;
            else
                nochecksums = 0;
#else
            if (opbx_false(s))
                opbx_log(LOG_WARNING, "Disabling UDPTL checksums is not supported on this operating system!\n");
#endif
        }
        if ((s = opbx_variable_retrieve(cfg, "general", "T38FaxUdpEC")))
        {
            if (strcmp(s, "t38UDPFEC") == 0)
                udptlfectype = 2;
            else if (strcmp(s, "t38UDPRedundancy") == 0)
                udptlfectype = 1;
        }
        if ((s = opbx_variable_retrieve(cfg, "general", "T38FaxMaxDatagram")))
        {
            udptlmaxdatagram = atoi(s);
            if (udptlmaxdatagram < 0)
                udptlmaxdatagram = 0;
            if (udptlmaxdatagram > LOCAL_FAX_MAX_DATAGRAM)
                udptlmaxdatagram = LOCAL_FAX_MAX_DATAGRAM;
        }
        if ((s = opbx_variable_retrieve(cfg, "general", "UDPTLFECentries")))
        {
            udptlfecentries = atoi(s);
            if (udptlfecentries < 0)
                udptlfecentries = 0;
            if (udptlfecentries > MAX_FEC_ENTRIES)
                udptlfecentries = MAX_FEC_ENTRIES;
        }
        if ((s = opbx_variable_retrieve(cfg, "general", "UDPTLFECspan")))
        {
            udptlfecspan = atoi(s);
            if (udptlfecspan < 0)
                udptlfecspan = 0;
            if (udptlfecspan > MAX_FEC_SPAN)
                udptlfecspan = MAX_FEC_SPAN;
        }
        opbx_config_destroy(cfg);
    }
}

void opbx_udptl_init(void)
{
    opbx_cli_register(&cli_debug);
    opbx_cli_register(&cli_debug_ip);
    opbx_cli_register(&cli_no_debug);
    opbx_udptl_reload();
}
