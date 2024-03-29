/*
 * CallWeaver -- An open source telephony toolkit.
 *
 * Copyright (C) 2004 - 2005, Adrian Kennard, rights assigned to Digium
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
 */

/*! \file
 *
 * \brief SMS application - ETSI ES 201 912 protocol 1 implimentation
 * \ingroup applications
 *
 * \author Adrian Kennard
 */

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/apps/app_sms.c $", "$Revision: 4739 $")

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <errno.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <spandsp.h>

#include "callweaver/lock.h"
#include "callweaver/file.h"
#include "callweaver/logger.h"
#include "callweaver/options.h"
#include "callweaver/channel.h"
#include "callweaver/pbx.h"
#include "callweaver/module.h"
#include "callweaver/alaw.h"
#include "callweaver/callerid.h"

#include "dll_sms.h"

/* ToDo */
/* Add full VP support */
/* Handle status report messages (generation and reception) */
/* Time zones on time stamps */
/* user ref field */

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static volatile unsigned char message_ref;      /* arbitary message ref */
static volatile unsigned int seq;       /* arbitrary message sequence number for unqiue files */

static char log_file[255];
static char spool_dir[255];

static void *sms_app;
static const char *sms_name = "SMS";
static const char *sms_synopsis = "Communicates with SMS service centres and SMS capable analogue phones";
static const char *sms_syntax = "SMS(name, [a][s])";
static const char *sms_descrip =
    "SMS handles exchange of SMS data with a call to/from SMS capabale\n"
    "phone or SMS PSTN service center. Can send and/or receive SMS messages.\n"
    "Works to ETSI ES 201 912 compatible with BT SMS PSTN service in UK\n"
    "Typical usage is to use to handle called from the SMS service centre CLI,\n"
    "or to set up a call using 'outgoing' or manager interface to connect\n"
    "service centre to SMS()\n"
    "name is the name of the queue used in /var/spool/callweaver.org/sms\n"
    "Arguments:\n"
    " a: answer, i.e. send initial FSK packet.\n"
    " s: act as service centre talking to a phone.\n"
    "Messages are processed as per text file message queues.\n" 
    "smsq (a separate software) is a command to generate message\n"
    "queues and send messages.\n";

static signed short wave[] =
{
    0, 392, 782, 1167, 1545, 1913, 2270, 2612, 2939, 3247, 3536, 3802, 4045, 4263, 4455, 4619, 4755, 4862, 4938, 4985,
    5000, 4985, 4938, 4862, 4755, 4619, 4455, 4263, 4045, 3802, 3536, 3247, 2939, 2612, 2270, 1913, 1545, 1167, 782, 392,
    0, -392, -782, -1167,
     -1545, -1913, -2270, -2612, -2939, -3247, -3536, -3802, -4045, -4263, -4455, -4619, -4755, -4862, -4938, -4985, -5000,
    -4985, -4938, -4862,
    -4755, -4619, -4455, -4263, -4045, -3802, -3536, -3247, -2939, -2612, -2270, -1913, -1545, -1167, -782, -392
};

/* SMS 7 bit character mapping to UCS-2 */
static const unsigned short defaultalphabet[] =
{
    0x0040, 0x00A3, 0x0024, 0x00A5, 0x00E8, 0x00E9, 0x00F9, 0x00EC,
    0x00F2, 0x00E7, 0x000A, 0x00D8, 0x00F8, 0x000D, 0x00C5, 0x00E5,
    0x0394, 0x005F, 0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8,
    0x03A3, 0x0398, 0x039E, 0x00A0, 0x00C6, 0x00E6, 0x00DF, 0x00C9,
    ' ', '!', '"', '#', 164, '%', '&', 39, '(', ')', '*', '+', ',', '-', '.', '/',
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ';', '<', '=', '>', '?',
    161, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
    'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 196, 214, 209, 220, 167,
    191, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
    'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', 228, 246, 241, 252, 224,
};

static const unsigned short escapes[] =
{
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x000C, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0x005E, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0x007B, 0x007D, 0, 0, 0, 0, 0, 0x005C,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x005B, 0x007E, 0x005D, 0,
    0x007C, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0x20AC, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

#define SMSLEN 160              /* max SMS length */

typedef struct sms_s
{
    unsigned char hangup;        /* we are done... */
    unsigned char err;           /* set for any errors */
    unsigned char smsc:1;        /* we are SMSC */
    unsigned char rx:1;          /* this is a received message */
    char queue[30];              /* queue name */
    char oa[20];                 /* originating address */
    char da[20];                 /* destination address */
    time_t scts;                 /* time stamp, UTC */
    unsigned char pid;           /* protocol ID */
    unsigned char dcs;           /* data coding scheme */
    short mr;                    /* message reference - actually a byte, but usde -1 for not set */
    int udl;                     /* user data length */
    int udhl;                    /* user data header length */
    unsigned char srr:1;         /* Status Report request */
    unsigned char udhi:1;        /* User Data Header required, even if length 0 */
    unsigned char rp:1;          /* Reply Path */
    unsigned int vp;             /* validity period in minutes, 0 for not set */
    unsigned short ud[SMSLEN];   /* user data (message), UCS-2 coded */
    unsigned char udh[SMSLEN];   /* user data header */
    char cli[20];                /* caller ID */
    unsigned char ophase;        /* phase (0-79) for 0 and 1 frequencies (1300Hz and 2100Hz) */
    unsigned char ophasep;       /* phase (0-79) for 1200 bps */
    unsigned char obyte;         /* byte being sent */
    unsigned int opause;         /* silent pause before sending (in sample periods) */
    unsigned char obitp;         /* bit in byte */
    unsigned char osync;         /* sync bits to send */
    unsigned char obytep;        /* byte in data */
    unsigned char obyten;        /* bytes in data */
    unsigned char omsg[256];     /* data buffer (out) */
    unsigned char imsg[200];     /* data buffer (in) */
    signed long long ims0,
        imc0,
        ims1,
        imc1;                      /* magnitude averages sin/cos 0/1 */
    unsigned int idle;
    unsigned short imag;         /* signal level */
    unsigned char ips0,
        ips1,
        ipc0,
        ipc1;                      /* phase sin/cos 0/1 */
    unsigned char ibitl;         /* last bit */
    unsigned char ibitc;         /* bit run length count */
    unsigned char iphasep;       /* bit phase (0-79) for 1200 bps */
    unsigned char ibitn;         /* bit number in byte being received */
    unsigned char ibytev;        /* byte value being received */
    unsigned char ibytep;        /* byte pointer in messafe */
    unsigned char ibytec;        /* byte checksum for message */
    unsigned char ierr;          /* error flag */
    unsigned char ibith;         /* history of last bits */
    unsigned char ibitt;         /* total of 1's in last 3 bites */
    /* more to go here */
} sms_t;

/* different types of encoding */
#define is7bit(dcs) (((dcs)&0xC0)?(!((dcs)&4)):(!((dcs)&12)))
#define is8bit(dcs) (((dcs)&0xC0)?(((dcs)&4)):(((dcs)&12)==4))
#define is16bit(dcs) (((dcs)&0xC0)?0:(((dcs)&12)==8))


#if 0
/* Code for protocol 2, which needs properly integrating */
#define BLOCK_SIZE 160

adsi_tx_state_t tx_adsi;

static int adsi_create_message(adsi_tx_state_t *s, uint8_t *msg)
{
    int len;

    len = adsi_add_field(s, msg, -1, CLIP_MDMF_SMS, NULL, 0);
    /* Active */
    len = adsi_add_field(s, msg, len, CLIP_DISPLAY_INFO, "\00ABC", 4);

    return len;
}
/*- End of function --------------------------------------------------------*/

int adsi_tl_next_field(adsi_rx_state_t *s, const uint8_t *msg, int msg_len, int pos, uint8_t *field_type, uint8_t const **field_body, int *field_len)
{
    int len;

    if (pos >= msg_len)
        return -1;
    /* These standards all use "IE" type fields - type, length, body - and similar headers */
    if (pos <= 0)
    {
        /* Return the message type */
        *field_type = msg[0];
        *field_len = 0;
        *field_body = NULL;
        pos = 2;
    }
    else
    {
        *field_type = msg[pos++];
        len = msg[pos++];
        len |= (msg[pos++] << 8);
        *field_len = len;
        *field_body = msg + pos;
        pos += len;
    }
    return pos;
}
/*- End of function --------------------------------------------------------*/

int adsi_tl_add_field(adsi_tx_state_t *s, uint8_t *msg, int len, uint8_t field_type, uint8_t const *field_body, int field_len)
{
    /* These standards all use "IE" type fields - type, length, body - and similar headers */
    if (len <= 0)
    {
        /* Initialise a new message. The field type is actually the message type. */
        msg[0] = field_type;
        msg[1] = 0;
        len = 2;
    }
    else
    {
        /* Add to a message in progress. */
        if (field_type)
        {
            msg[len] = field_type;
            msg[len + 1] = field_len & 0xFF;
            msg[len + 2] = (field_len >> 8) & 0xFF;
            memcpy(msg + len + 3, field_body, field_len);
            len += (field_len + 3);
        }
        else
        {
            /* No field type or length, for restricted single message formats */
            memcpy(msg + len, field_body, field_len);
            len += field_len;
        }
    }
    return len;
}
/*- End of function --------------------------------------------------------*/

int adsi_tl_add_tm(adsi_tx_state_t *s, char *msg)
{
    uint8_t tx_msg[256];
    int tx_len;
    int l;
    
    l = -1;
    tx_len = adsi_tl_add_field(&tx_adsi, tx_msg, -1, DLL_SMS_P2_INFO_MT, NULL, 0);
    tx_len += 2;
    tx_len = adsi_tl_add_field(&tx_adsi, tx_msg, tx_len, DLL_PARM_PROVIDER_ID, "CW", 3);
    tx_len = adsi_tl_add_field(&tx_adsi, tx_msg, tx_len, DLL_PARM_DESTINATION, "98765432", 8);
    tx_len = adsi_tl_add_field(&tx_adsi, tx_msg, tx_len, DLL_PARM_DISPLAY_INFO, msg, strlen(msg));
    tx_msg[2] = tx_len - 4;
    tx_msg[3] = 0;
    adsi_tx_put_message(s, tx_msg, tx_len);
}
/*- End of function --------------------------------------------------------*/

static void put_adsi_msg(void *user_data, const uint8_t *msg, int len)
{
    int i;
    int l;
    uint8_t field_type;
    const uint8_t *field_body;
    int field_len;
    uint8_t body[256];
    adsi_rx_state_t *rx_adsi;
    uint8_t tx_msg[256];
    int tx_len;
    int file;
    
    printf("Good message received (%d bytes)\n", len);

    rx_adsi = (adsi_rx_state_t *) user_data;
    for (i = 0;  i < len;  i++)
    {
        printf("%2x ", msg[i]);
        if ((i & 0xf) == 0xf)
            printf("\n");
    }
    printf("\n");
    l = -1;
    do
    {
        l = adsi_tl_next_field(rx_adsi, msg, len, l, &field_type, &field_body, &field_len);
        if (l > 0)
        {
            if (field_body)
            {
                memcpy(body, field_body, field_len);
                body[field_len] = '\0';
                printf("Type %x, len %d, '%s'\n", field_type, field_len, body);
            }
            else
            {
                printf("Message type %x\n", field_type);
                switch (field_type)
                {
                case DLL_SMS_P2_INFO_MO:
                    file = open("/tmp/pdus", O_WRONLY | O_CREAT, 0666);
                    if (file >= 0)
                    {
                        write(file, msg + 2, msg[1]);
                        close(file);
                    }
                    tx_len = adsi_tl_add_field(&tx_adsi, tx_msg, -1, DLL_SMS_P2_ACK0, NULL, 0);
                    adsi_tx_put_message(&tx_adsi, tx_msg, tx_len);
                    /* Skip the TL length */
                    l += 2;
                    break;
                case DLL_SMS_P2_INFO_MT:
                    tx_len = adsi_tl_add_field(&tx_adsi, tx_msg, -1, DLL_SMS_P2_ACK0, NULL, 0);
                    adsi_tx_put_message(&tx_adsi, tx_msg, tx_len);
                    /* Skip the TL length */
                    l += 2;
                    break;
                case DLL_SMS_P2_INFO_STA:
                    l += 2;
                    tx_len = adsi_tl_add_field(&tx_adsi, tx_msg, -1, DLL_SMS_P2_ACK0, NULL, 0);
                    adsi_tx_put_message(&tx_adsi, tx_msg, tx_len);
                    break;
                case DLL_SMS_P2_NACK:
                    adsi_tl_add_tm(&tx_adsi, "This is the message");
                    l += 2;
                    break;
                case DLL_SMS_P2_ACK0:
                    l += 2;
                    tx_len = adsi_tl_add_field(&tx_adsi, tx_msg, -1, DLL_SMS_P2_ACK0, NULL, 0);
                    adsi_tx_put_message(&tx_adsi, tx_msg, tx_len);
                    break;
                case DLL_SMS_P2_ACK1:
                    l += 2;
                    adsi_tl_add_tm(&tx_adsi, "This is the message");
                    break;
                case DLL_SMS_P2_ENQ:
                    l += 2;
                    tx_len = adsi_tl_add_field(&tx_adsi, tx_msg, -1, DLL_SMS_P2_ACK0, NULL, 0);
                    adsi_tx_put_message(&tx_adsi, tx_msg, tx_len);
                    break;
                case DLL_SMS_P2_REL:
                    l += 2;
                    tx_len = adsi_tl_add_field(&tx_adsi, tx_msg, -1, DLL_SMS_P2_ACK0, NULL, 0);
                    adsi_tx_put_message(&tx_adsi, tx_msg, tx_len);
                    break;
                }
            }
        }
    }
    while (l > 0);
    printf("\n");
}
/*- End of function --------------------------------------------------------*/

static int sms_protocol2_exec(struct cw_channel *chan, void *data)
{
    int res = 0;
    int count = 0;
    int percentflag = 0;
    char msg[256];
    char *vdata;
    char *localident;
    int i;
    adsi_rx_state_t rx_adsi;
    uint8_t adsi_msg[256];
    int adsi_msg_len;

    struct localuser *u;
    struct cw_frame *inf = NULL;
    struct cw_frame outf;

    int original_read_fmt;
    int original_write_fmt;
    
    uint8_t __buf[sizeof(uint16_t)*BLOCK_SIZE + 2*CW_FRIENDLY_OFFSET];
    uint8_t *buf = __buf + CW_FRIENDLY_OFFSET;
    int len;
    extern int errno;

    if (chan == NULL)
    {
        cw_log(LOG_WARNING, "Channel is NULL. Giving up.\n");
        return -1;
    }
    vdata = data;

    /* The next few lines of code parse out the message and header from the input string */
    if (!vdata)
    {
        /* No data implies no filename or anything is present */
        cw_log(LOG_WARNING, "SMS requires an argument (message)\n");
        return -1;
    }
    
    for (i = 0;  vdata[i]  &&  (vdata[i] != ':')  &&  (vdata[i] != ',');  i++)
    {
        if ((vdata[i] == '%')  &&  (vdata[i + 1] == 'd'))
            percentflag = 1;                      /* the wildcard is used */
        
        if (i == strlen(vdata))
        {
            cw_log(LOG_WARNING, "No extension found\n");
            return -1;
        }
        msg[i] = vdata[i];
    }
    msg[i] = '\0';

printf("Message is '%s'\n", msg);
    if (vdata[i] == ',')
    {
        i++;
    }
    /* Done parsing */

    LOCAL_USER_ADD(u);

    if (chan->_state != CW_STATE_UP)
    {
        /* Shouldn't need this, but checking to see if channel is already answered
         * Theoretically cw should already have answered before running the app */
        res = cw_answer(chan);
    }
    
    if (!res)
    {
        original_read_fmt = chan->readformat;
        if (original_read_fmt != CW_FORMAT_SLINEAR)
        {
            res = cw_set_read_format(chan, CW_FORMAT_SLINEAR);
            if (res < 0)
            {
                cw_log(LOG_WARNING, "Unable to set to linear read mode, giving up\n");
                return -1;
            }
        }
        original_write_fmt = chan->writeformat;
        if (original_write_fmt != CW_FORMAT_SLINEAR)
        {
            res = cw_set_write_format(chan, CW_FORMAT_SLINEAR);
            if (res < 0)
            {
                cw_log(LOG_WARNING, "Unable to set to linear write mode, giving up\n");
                res = cw_set_read_format(chan, original_read_fmt);
                if (res)
                    cw_log(LOG_WARNING, "Unable to restore read format on '%s'\n", chan->name);
                return -1;
            }
        }
        adsi_tx_init(&tx_adsi, ADSI_STANDARD_CLIP);
        adsi_rx_init(&rx_adsi, ADSI_STANDARD_CLIP, put_adsi_msg, &rx_adsi);
        adsi_msg_len = adsi_create_message(&tx_adsi, adsi_msg);
        if (msg[0] == '\0')
            adsi_tx_put_message(&tx_adsi, adsi_msg, adsi_msg_len);

        while (cw_waitfor(chan, -1) > -1)
        {
            inf = cw_read(chan);
            if (inf == NULL)
            {
                res = -1;
                break;
            }
            if (inf->frametype == CW_FRAME_VOICE)
            {
                adsi_rx(&rx_adsi, inf->data, inf->samples);
                len = adsi_tx(&tx_adsi, (int16_t *) &buf[CW_FRIENDLY_OFFSET], inf->samples);
                if (len)
                {
                    memset(&outf, 0, sizeof(outf));
                    outf.frametype = CW_FRAME_VOICE;
                    outf.subclass = CW_FORMAT_SLINEAR;
                    outf.datalen = len*sizeof(int16_t);
                    outf.samples = len;
                    outf.data = &buf[CW_FRIENDLY_OFFSET];
                    outf.offset = CW_FRIENDLY_OFFSET;
                    if (cw_write(chan, &outf) < 0)
                    {
                        cw_log(LOG_WARNING, "Unable to write frame to channel; %s\n", strerror(errno));
                        break;
                    }
                }
            }
            cw_fr_free(inf);
        }
        if (inf == NULL)
        {
            cw_log(LOG_DEBUG, "Got hangup\n");
            res = -1;
        }
        if (original_read_fmt != CW_FORMAT_SLINEAR)
        {
            res = cw_set_read_format(chan, original_read_fmt);
            if (res)
                cw_log(LOG_WARNING, "Unable to restore read format on '%s'\n", chan->name);
        }
        if (original_write_fmt != CW_FORMAT_SLINEAR)
        {
            res = cw_set_write_format(chan, original_write_fmt);
            if (res)
                cw_log(LOG_WARNING, "Unable to restore write format on '%s'\n", chan->name);
        }
    }
    else
    {
        cw_log(LOG_WARNING, "Could not answer channel '%s'\n", chan->name);
    }
    LOCAL_USER_REMOVE(u);
    return res;
}
/*- End of function --------------------------------------------------------*/

#endif

static void *sms_alloc(struct cw_channel *chan, void *params)
{
    return params;
}

static void sms_release(struct cw_channel *chan, void *data)
{
    return;
}

static void sms_messagetx(sms_t *h);

/*! \brief copy number, skipping non digits apart from leading + */
static void numcpy(char *d, char *s)
{
    if (*s == '+')
        *d++ = *s++;
    while (*s)
    {
          if (isdigit (*s))
            *d++ = *s;
        s++;
    }
    *d = 0;
}

/*! \brief static, return a date/time in ISO format */
static char * isodate (time_t t)
{
    static char date[20];
    strftime (date, sizeof (date), "%Y-%m-%dT%H:%M:%S", localtime (&t));
    return date;
}

/*! \brief reads next UCS character from null terminated UTF-8 string and advanced pointer */
/* for non valid UTF-8 sequences, returns character as is */
/* Does not advance pointer for null termination */
static long utf8decode(unsigned char **pp)
{
    unsigned char *p = *pp;

    if (!*p)
        return 0;                 /* null termination of string */
    (*pp)++;
    if (*p < 0xC0)
        return *p;                /* ascii or continuation character */
    if (*p < 0xE0) {
        if (*p < 0xC2 || (p[1] & 0xC0) != 0x80)
            return *p;             /* not valid UTF-8 */
        (*pp)++;
        return ((*p & 0x1F) << 6) + (p[1] & 0x3F);
       }
    if (*p < 0xF0) {
        if ((*p == 0xE0 && p[1] < 0xA0) || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80)
             return *p;             /* not valid UTF-8 */
        (*pp) += 2;
        return ((*p & 0x0F) << 12) + ((p[1] & 0x3F) << 6) + (p[2] & 0x3F);
    }
    if (*p < 0xF8) {
        if ((*p == 0xF0 && p[1] < 0x90) || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80)
            return *p;             /* not valid UTF-8 */
        (*pp) += 3;
        return ((*p & 0x07) << 18) + ((p[1] & 0x3F) << 12) + ((p[2] & 0x3F) << 6) + (p[3] & 0x3F);
    }
    if (*p < 0xFC) {
        if ((*p == 0xF8 && p[1] < 0x88) || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80
            || (p[4] & 0xC0) != 0x80)
            return *p;             /* not valid UTF-8 */
        (*pp) += 4;
        return ((*p & 0x03) << 24) + ((p[1] & 0x3F) << 18) + ((p[2] & 0x3F) << 12) + ((p[3] & 0x3F) << 6) + (p[4] & 0x3F);
    }
    if (*p < 0xFE) {
        if ((*p == 0xFC && p[1] < 0x84) || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80
            || (p[4] & 0xC0) != 0x80 || (p[5] & 0xC0) != 0x80)
            return *p;             /* not valid UTF-8 */
        (*pp) += 5;
        return ((*p & 0x01) << 30) + ((p[1] & 0x3F) << 24) + ((p[2] & 0x3F) << 18) + ((p[3] & 0x3F) << 12) + ((p[4] & 0x3F) << 6) + (p[5] & 0x3F);
    }
    return *p;                   /* not sensible */
}

/*! \brief takes a binary header (udhl bytes at udh) and UCS-2 message (udl characters at ud) and packs in to o using SMS 7 bit character codes */
/* The return value is the number of septets packed in to o, which is internally limited to SMSLEN */
/* o can be null, in which case this is used to validate or count only */
/* if the input contains invalid characters then the return value is -1 */
static int packsms7(unsigned char *o, int udhl, unsigned char *udh, int udl, unsigned short *ud)
{
     unsigned char p = 0, b = 0, n = 0;

    if (udhl) {                            /* header */
        if (o)
            o[p++] = udhl;
        b = 1;
        n = 1;
        while (udhl--) {
            if (o)
                o[p++] = *udh++;
            b += 8;
            while (b >= 7) {
                b -= 7;
                n++;
            }
            if (n >= SMSLEN)
                return n;
        }
        if (b) {
            b = 7 - b;
            if (++n >= SMSLEN)
                return n;
            };    /* filling to septet boundary */
        }
        if (o)
            o[p] = 0;
        /* message */
        while (udl--) {
            long u;
            unsigned char v;
            u = *ud++;
            for (v = 0; v < 128 && defaultalphabet[v] != u; v++);
            if (v == 128 && u && n + 1 < SMSLEN) {
                for (v = 0; v < 128 && escapes[v] != u; v++);
                if (v < 128) {    /* escaped sequence */
                if (o)
                    o[p] |= (27 << b);
                b += 7;
                if (b >= 8) {
                    b -= 8;
                    p++;
                    if (o)
                        o[p] = (27 >> (7 - b));
                }
                n++;
            }
        }
        if (v == 128)
            return -1;             /* invalid character */
        if (o)
            o[p] |= (v << b);
        b += 7;
        if (b >= 8) {
            b -= 8;
            p++;
            if (o)
                o[p] = (v >> (7 - b));
        }
        if (++n >= SMSLEN)
            return n;
    }
    return n;
}

/*! \brief takes a binary header (udhl bytes at udh) and UCS-2 message (udl characters at ud) and packs in to o using 8 bit character codes */
/* The return value is the number of bytes packed in to o, which is internally limited to 140 */
/* o can be null, in which case this is used to validate or count only */
/* if the input contains invalid characters then the return value is -1 */
static int packsms8(unsigned char *o, int udhl, unsigned char *udh, int udl, unsigned short *ud)
{
    unsigned char p = 0;

    /* header - no encoding */
    if (udhl) {
        if (o)
            o[p++] = udhl;
        while (udhl--) {
            if (o)
                o[p++] = *udh++;
            if (p >= 140)
                return p;
        }
    }
    while (udl--) {
        long u;
        u = *ud++;
        if (u < 0 || u > 0xFF)
            return -1;             /* not valid */
        if (o)
            o[p++] = u;
        if (p >= 140)
            return p;
    }
    return p;
}

/*! \brief takes a binary header (udhl bytes at udh) and UCS-2 
    message (udl characters at ud) and packs in to o using 16 bit 
    UCS-2 character codes 
    The return value is the number of bytes packed in to o, which is 
    internally limited to 140 
    o can be null, in which case this is used to validate or count 
    only if the input contains invalid characters then 
    the return value is -1 */
static int packsms16(unsigned char *o, int udhl, unsigned char *udh, int udl, unsigned short *ud)
{
    unsigned char p = 0;
    /* header - no encoding */
    if (udhl) {
        if (o)
            o[p++] = udhl;
        while (udhl--) {
            if (o)
                o[p++] = *udh++;
            if (p >= 140)
                return p;
        }
    }
    while (udl--) {
        long u;
        u = *ud++;
        if (o)
            o[p++] = (u >> 8);
        if (p >= 140)
            return p - 1;          /* could not fit last character */
        if (o)
            o[p++] = u;
        if (p >= 140)
            return p;
    }
    return p;
}

/*! \brief general pack, with length and data, 
    returns number of bytes of target used */
static int packsms(unsigned char dcs, unsigned char *base, unsigned int udhl, unsigned char *udh, int udl, unsigned short *ud)
{
    unsigned char *p = base;
    if (udl) {
        int l = 0;
        if (is7bit (dcs)) {         /* 7 bit */
            l = packsms7 (p + 1, udhl, udh, udl, ud);
            if (l < 0)
                l = 0;
            *p++ = l;
            p += (l * 7 + 7) / 8;
        } else if (is8bit (dcs)) {                                 /* 8 bit */
            l = packsms8 (p + 1, udhl, udh, udl, ud);
            if (l < 0)
                l = 0;
            *p++ = l;
            p += l;
        } else {             /* UCS-2 */
            l = packsms16 (p + 1, udhl, udh, udl, ud);
            if (l < 0)
                l = 0;
            *p++ = l;
            p += l;
        }
    } else
        *p++ = 0;              /* no user data */
    return p - base;
}


/*! \brief pack a date and return */
static void packdate(unsigned char *o, time_t w)
{
    struct tm *t = localtime (&w);
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined( __NetBSD__ ) || defined(__APPLE__)
    int z = -t->tm_gmtoff / 60 / 15;
#else
    int z = timezone / 60 / 15;
#endif
    *o++ = ((t->tm_year % 10) << 4) + (t->tm_year % 100) / 10;
    *o++ = (((t->tm_mon + 1) % 10) << 4) + (t->tm_mon + 1) / 10;
    *o++ = ((t->tm_mday % 10) << 4) + t->tm_mday / 10;
    *o++ = ((t->tm_hour % 10) << 4) + t->tm_hour / 10;
    *o++ = ((t->tm_min % 10) << 4) + t->tm_min / 10;
    *o++ = ((t->tm_sec % 10) << 4) + t->tm_sec / 10;
    if (z < 0)
        *o++ = (((-z) % 10) << 4) + (-z) / 10 + 0x08;
    else
        *o++ = ((z % 10) << 4) + z / 10;
}

/*! \brief unpack a date and return */
static time_t unpackdate(unsigned char *i)
{
    struct tm t;
    t.tm_year = 100 + (i[0] & 0xF) * 10 + (i[0] >> 4);
    t.tm_mon = (i[1] & 0xF) * 10 + (i[1] >> 4) - 1;
    t.tm_mday = (i[2] & 0xF) * 10 + (i[2] >> 4);
    t.tm_hour = (i[3] & 0xF) * 10 + (i[3] >> 4);
    t.tm_min = (i[4] & 0xF) * 10 + (i[4] >> 4);
    t.tm_sec = (i[5] & 0xF) * 10 + (i[5] >> 4);
    t.tm_isdst = 0;
    if (i[6] & 0x08)
        t.tm_min += 15 * ((i[6] & 0x7) * 10 + (i[6] >> 4));
    else
        t.tm_min -= 15 * ((i[6] & 0x7) * 10 + (i[6] >> 4));
    return mktime (&t);
}

/*! \brief unpacks bytes (7 bit encoding) at i, len l septets, 
    and places in udh and ud setting udhl and udl. udh not used 
    if udhi not set */
static void unpacksms7(unsigned char *i, unsigned char l, unsigned char *udh, int *udhl, unsigned short *ud, int *udl, char udhi)
{
    unsigned char b = 0, p = 0;
    unsigned short *o = ud;

    *udhl = 0;
    if (udhi && l) {         /* header */
        int h = i[p];
        *udhl = h;
        if (h) {
            b = 1;
            p++;
            l--;
            while (h-- && l) {
                *udh++ = i[p++];
                b += 8;
                while (b >= 7) {
                    b -= 7;
                    l--;
                    if (!l)
                        break;
                }
            }
            /* adjust for fill, septets */
            if (b) {
                b = 7 - b;
                l--;
            }
        }
    }
    while (l--) {
        unsigned char v;
        if (b < 2)
            v = ((i[p] >> b) & 0x7F);
        else
            v = ((((i[p] >> b) + (i[p + 1] << (8 - b)))) & 0x7F);
        b += 7;
        if (b >= 8) {
            b -= 8;
            p++;
        }
        if (o > ud && o[-1] == 0x00A0 && escapes[v])
            o[-1] = escapes[v];
        else
            *o++ = defaultalphabet[v];
    }
    *udl = (o - ud);
}

/*! \brief unpacks bytes (8 bit encoding) at i, len l septets, 
      and places in udh and ud setting udhl and udl. udh not used 
      if udhi not set */
static void unpacksms8(unsigned char *i, unsigned char l, unsigned char *udh, int *udhl, unsigned short *ud, int *udl, char udhi)
{
    unsigned short *o = ud;
    *udhl = 0;
    if (udhi) {
        int n = *i;
        *udhl = n;
        if (n) {
            i++;
            l--;
            while (l && n) {
                l--;
                n--;
                *udh++ = *i++;
            }
        }
    }
    while (l--)
        *o++ = *i++;      /* not to UTF-8 as explicitely 8 bit coding in DCS */
    *udl = (o - ud);
}

/*! \brief unpacks bytes (16 bit encoding) at i, len l septets,
     and places in udh and ud setting udhl and udl. 
    udh not used if udhi not set */
static void unpacksms16(unsigned char *i, unsigned char l, unsigned char *udh, int *udhl, unsigned short *ud, int *udl, char udhi)
{
    unsigned short *o = ud;
    *udhl = 0;
    if (udhi) {
        int n = *i;
        *udhl = n;
        if (n) {
            i++;
            l--;
            while (l && n) {
                l--;
                n--;
                *udh++ = *i++;
            }
        }
    }
    while (l--) {
        int v = *i++;
        if (l--)
            v = (v << 8) + *i++;
        *o++ = v;
    }
    *udl = (o - ud);
}

/*! \brief general unpack - starts with length byte (octet or septet) and returns number of bytes used, inc length */
static int unpacksms(unsigned char dcs, unsigned char *i, unsigned char *udh, int *udhl, unsigned short *ud, int *udl, char udhi)
{
    int l = *i++;
    if (is7bit (dcs)) {
        unpacksms7 (i, l, udh, udhl, ud, udl, udhi);
        l = (l * 7 + 7) / 8;        /* adjust length to return */
    } else if (is8bit (dcs))
        unpacksms8 (i, l, udh, udhl, ud, udl, udhi);
    else
        unpacksms16 (i, l, udh, udhl, ud, udl, udhi);
    return l + 1;
}

/*! \brief unpack an address from i, return byte length, unpack to o */
static unsigned char unpackaddress(char *o, unsigned char *i)
{
    unsigned char l = i[0];
    unsigned char p;

    if (i[1] == (0x80 | DLL_SMS_P1_DATA))
        *o++ = '+';
    for (p = 0; p < l; p++) {
        if (p & 1)
            *o++ = (i[2 + p / 2] >> 4) + '0';
        else
            *o++ = (i[2 + p / 2] & 0xF) + '0';
    }
    *o = 0;
    return (l + 5) / 2;
}

/*! \brief store an address at o, and return number of bytes used */
static unsigned char packaddress(unsigned char *o, char *i)
{
    unsigned char p = 2;

    o[0] = 0;
    if (*i == '+') {
        i++;
        o[1] = 0x80 | DLL_SMS_P1_DATA;
    } else
        o[1] = 0x81;
    while (*i)
        if (isdigit (*i)) {
            if (o[0] & 1)
                o[p++] |= ((*i & 0xF) << 4);
            else
                o[p] = (*i & 0xF);
            o[0]++;
            i++;
        } else
            i++;
    if (o[0] & 1)
        o[p++] |= 0xF0;              /* pad */
    return p;
}

/*! \brief Log the output, and remove file */
static void sms_log(sms_t * h, char status)
{
    if (*h->oa || *h->da) {
        int o = open (log_file, O_CREAT | O_APPEND | O_WRONLY, 0666);
        if (o >= 0) {
            char line[1000], mrs[3] = "", *p;
            unsigned char n;

            if (h->mr >= 0)
                snprintf (mrs, sizeof (mrs), "%02X", h->mr);
            snprintf (line, sizeof (line), "%s %c%c%c%s %s %s %s ",
                 isodate (time (0)), status, h->rx ? 'I' : 'O', h->smsc ? 'S' : 'M', mrs, h->queue, *h->oa ? h->oa : "-",
                 *h->da ? h->da : "-");
            p = line + strlen (line);
            for (n = 0; n < h->udl; n++)
                if (h->ud[n] == '\\') {
                    *p++ = '\\';
                    *p++ = '\\';
                } else if (h->ud[n] == '\n') {
                    *p++ = '\\';
                    *p++ = 'n';
                } else if (h->ud[n] == '\r') {
                    *p++ = '\\';
                    *p++ = 'r';
                } else if (h->ud[n] < 32 || h->ud[n] == 127)
                    *p++ = 191;
                else
                    *p++ = h->ud[n];
            *p++ = '\n';
            *p = 0;
            write(o, line, strlen (line));
            close(o);
        }
        *h->oa = *h->da = h->udl = 0;
    }
}

/*! \brief parse and delete a file */
static void sms_readfile(sms_t * h, char *fn)
{
    char line[1000];
    FILE *s;
    char dcsset = 0;                 /* if DSC set */
    cw_log (LOG_EVENT, "Sending %s\n", fn);
    h->rx = h->udl = *h->oa = *h->da = h->pid = h->srr = h->udhi = h->rp = h->vp = h->udhl = 0;
    h->mr = -1;
    h->dcs = 0xF1;                    /* normal messages class 1 */
    h->scts = time (0);
    s = fopen (fn, "r");
    if (s)
    {
        if (unlink (fn))
        {                                 /* concurrent access, we lost */
            fclose (s);
            return;
        }
        while (fgets (line, sizeof (line), s))
        {                                 /* process line in file */
            char *p;
            void *pp = &p;
            for (p = line; *p && *p != '\n' && *p != '\r'; p++);
            *p = 0;                     /* strip eoln */
            p = line;
            if (!*p || *p == ';')
                continue;              /* blank line or comment, ignore */
            while (isalnum (*p))
            {
                *p = tolower (*p);
                p++;
            }
            while (isspace (*p))
                *p++ = 0;
            if (*p == '=')
            {
                *p++ = 0;
                if (!strcmp (line, "ud"))
                {                         /* parse message (UTF-8) */
                    unsigned char o = 0;
                    while (*p && o < SMSLEN)
                        h->ud[o++] = utf8decode(pp);
                    h->udl = o;
                    if (*p)
                        cw_log (LOG_WARNING, "UD too long in %s\n", fn);
                } else
                {
                    while (isspace (*p))
                        p++;
                    if (!strcmp (line, "oa") && strlen (p) < sizeof (h->oa))
                        numcpy (h->oa, p);
                    else if (!strcmp (line, "da") && strlen (p) < sizeof (h->oa))
                        numcpy (h->da, p);
                    else if (!strcmp (line, "pid"))
                        h->pid = atoi (p);
                    else if (!strcmp (line, "dcs"))
                    {
                        h->dcs = atoi (p);
                        dcsset = 1;
                    } else if (!strcmp (line, "mr"))
                        h->mr = atoi (p);
                    else if (!strcmp (line, "srr"))
                        h->srr = (atoi (p) ? 1 : 0);
                    else if (!strcmp (line, "vp"))
                        h->vp = atoi (p);
                    else if (!strcmp (line, "rp"))
                        h->rp = (atoi (p) ? 1 : 0);
                    else if (!strcmp (line, "scts"))
                    {                     /* get date/time */
                        int Y,
                          m,
                          d,
                          H,
                          M,
                          S;
                        if (sscanf (p, "%d-%d-%dT%d:%d:%d", &Y, &m, &d, &H, &M, &S) == 6)
                        {
                            struct tm t;
                            t.tm_year = Y - 1900;
                            t.tm_mon = m - 1;
                            t.tm_mday = d;
                            t.tm_hour = H;
                            t.tm_min = M;
                            t.tm_sec = S;
                            t.tm_isdst = -1;
                            h->scts = mktime (&t);
                            if (h->scts == (time_t) - 1)
                                cw_log (LOG_WARNING, "Bad date/timein %s: %s", fn, p);
                        }
                    } else
                        cw_log (LOG_WARNING, "Cannot parse in %s: %s=%si\n", fn, line, p);
                }
            } else if (*p == '#')
            {                             /* raw hex format */
                *p++ = 0;
                if (*p == '#')
                {
                    p++;
                    if (!strcmp (line, "ud"))
                    {                     /* user data */
                        int o = 0;
                        while (*p && o < SMSLEN)
                        {
                            if (!isxdigit(p[0])  ||  !isxdigit(p[1])  ||  !isxdigit(p[2])  ||  !isxdigit(p[3]))
                                break;
                            h->ud[o++] = (((isalpha(p[0])  ?  9  :  0) + (p[0] & 0xF)) << 12)
                                       + (((isalpha(p[1])  ?  9  :  0) + (p[1] & 0xF)) << 8)
                                       + (((isalpha(p[2])  ?  9  :  0) + (p[2] & 0xF)) << 4)
                                       + ((isalpha(p[3])  ?  9  :  0) + (p[3] & 0xF));
                            p += 4;
                        }
                        h->udl = o;
                        if (*p)
                            cw_log (LOG_WARNING, "UD too long / invalid UCS-2 hex in %s\n", fn);
                    } else
                        cw_log (LOG_WARNING, "Only ud can use ## format, %s\n", fn);
                } else if (!strcmp (line, "ud"))
                {                         /* user data */
                    int o = 0;
                    while (*p && o < SMSLEN)
                    {
                        if (isxdigit (*p) && isxdigit (p[1]))
                        {
                            h->ud[o++] = (((isalpha (*p) ? 9 : 0) + (*p & 0xF)) << 4) + ((isalpha (p[1]) ? 9 : 0) + (p[1] & 0xF));
                            p += 2;
                        } else
                            break;
                    }
                    h->udl = o;
                    if (*p)
                        cw_log (LOG_WARNING, "UD too long / invalid UCS-1 hex in %s\n", fn);
                } else if (!strcmp (line, "udh"))
                {                         /* user data header */
                    unsigned char o = 0;
                    h->udhi = 1;
                    while (*p && o < SMSLEN)
                    {
                        if (isxdigit (*p) && isxdigit (p[1]))
                        {
                            h->udh[o] = (((isalpha (*p) ? 9 : 0) + (*p & 0xF)) << 4) + ((isalpha (p[1]) ? 9 : 0) + (p[1] & 0xF));
                            o++;
                            p += 2;
                        } else
                            break;
                    }
                    h->udhl = o;
                    if (*p)
                        cw_log (LOG_WARNING, "UDH too long / invalid hex in %s\n", fn);
                } else
                    cw_log (LOG_WARNING, "Only ud and udh can use # format, %s\n", fn);
            } else
                cw_log (LOG_WARNING, "Cannot parse in %s: %s\n", fn, line);
        }
        fclose (s);
        if (!dcsset && packsms7 (0, h->udhl, h->udh, h->udl, h->ud) < 0)
        {
            if (packsms8 (0, h->udhl, h->udh, h->udl, h->ud) < 0)
            {
                if (packsms16 (0, h->udhl, h->udh, h->udl, h->ud) < 0)
                    cw_log (LOG_WARNING, "Invalid UTF-8 message even for UCS-2 (%s)\n", fn);
                else
                {
                    h->dcs = 0x08;    /* default to 16 bit */
                    cw_log (LOG_WARNING, "Sending in 16 bit format (%s)\n", fn);
                }
            } else
            {
                h->dcs = 0xF5;        /* default to 8 bit */
                cw_log (LOG_WARNING, "Sending in 8 bit format (%s)\n", fn);
            }
        }
        if (is7bit (h->dcs) && packsms7 (0, h->udhl, h->udh, h->udl, h->ud) < 0)
            cw_log (LOG_WARNING, "Invalid 7 bit GSM data %s\n", fn);
        if (is8bit (h->dcs) && packsms8 (0, h->udhl, h->udh, h->udl, h->ud) < 0)
            cw_log (LOG_WARNING, "Invalid 8 bit data %s\n", fn);
        if (is16bit (h->dcs) && packsms16 (0, h->udhl, h->udh, h->udl, h->ud) < 0)
            cw_log (LOG_WARNING, "Invalid 16 bit data %s\n", fn);
    }
}

/*! \brief white a received text message to a file */
static void sms_writefile(sms_t * h)
{
    char fn[200] = "";
    char fn2[200] = "";
    FILE *o;

    cw_copy_string(fn, spool_dir, sizeof (fn));
    mkdir(fn, 0777);            /* ensure it exists */
    snprintf(fn + strlen (fn), sizeof (fn) - strlen (fn), "/%s", h->smsc ? h->rx ? "morx" : "mttx" : h->rx ? "mtrx" : "motx");
    mkdir(fn, 0777);            /* ensure it exists */
    cw_copy_string(fn2, fn, sizeof (fn2));
    snprintf(fn2 + strlen (fn2), sizeof (fn2) - strlen (fn2), "/%s.%s-%d", h->queue, isodate (h->scts), seq++);
    snprintf(fn + strlen (fn), sizeof (fn) - strlen (fn), "/.%s", fn2 + strlen (fn) + 1);
    o = fopen(fn, "w");
    if (o) {
        if (*h->oa)
            fprintf (o, "oa=%s\n", h->oa);
        if (*h->da)
            fprintf (o, "da=%s\n", h->da);
        if (h->udhi) {
            unsigned int p;
            fprintf (o, "udh#");
            for (p = 0; p < h->udhl; p++)
                fprintf (o, "%02X", h->udh[p]);
            fprintf (o, "\n");
        }
        if (h->udl) {
            unsigned int p;
            for (p = 0; p < h->udl && h->ud[p] >= ' '; p++);
            if (p < h->udl)
                fputc (';', o);      /* cannot use ud=, but include as a comment for human readable */
            fprintf (o, "ud=");
            for (p = 0; p < h->udl; p++) {
                unsigned short v = h->ud[p];
                if (v < 32)
                    fputc (191, o);
                else if (v < 0x80)
                    fputc(v, o);
                else if (v < 0x800)
                {
                    fputc(0xC0 + (v >> 6), o);
                    fputc(0x80 + (v & 0x3F), o);
                } else
                {
                    fputc(0xE0 + (v >> 12), o);
                    fputc(0x80 + ((v >> 6) & 0x3F), o);
                    fputc(0x80 + (v & 0x3F), o);
                }
            }
            fprintf (o, "\n");
            for (p = 0; p < h->udl && h->ud[p] >= ' '; p++);
            if (p < h->udl) {
                for (p = 0; p < h->udl && h->ud[p] < 0x100; p++);
                if (p == h->udl) {                         /* can write in ucs-1 hex */
                    fprintf (o, "ud#");
                    for (p = 0; p < h->udl; p++)
                        fprintf (o, "%02X", h->ud[p]);
                    fprintf (o, "\n");
                } else {                         /* write in UCS-2 */
                    fprintf (o, "ud##");
                    for (p = 0; p < h->udl; p++)
                        fprintf (o, "%04X", h->ud[p]);
                    fprintf (o, "\n");
                }
            }
        }
        if (h->scts)
            fprintf (o, "scts=%s\n", isodate (h->scts));
        if (h->pid)
            fprintf (o, "pid=%d\n", h->pid);
        if (h->dcs != 0xF1)
            fprintf (o, "dcs=%d\n", h->dcs);
        if (h->vp)
            fprintf (o, "vp=%d\n", h->vp);
        if (h->srr)
            fprintf (o, "srr=1\n");
        if (h->mr >= 0)
            fprintf (o, "mr=%d\n", h->mr);
        if (h->rp)
            fprintf (o, "rp=1\n");
        fclose (o);
        if (rename (fn, fn2))
            unlink (fn);
        else
            cw_log (LOG_EVENT, "Received to %s\n", fn2);
    }
}

/*! \brief read dir skipping dot files... */
static struct dirent *readdirqueue(DIR * d, char *queue)
{
   struct dirent *f;
   do {
      f = readdir (d);
   } while (f && (*f->d_name == '.' || strncmp (f->d_name, queue, strlen (queue)) || f->d_name[strlen (queue)] != '.'));
   return f;
}

/*! \brief handle the incoming message */
static unsigned char sms_handleincoming(sms_t * h)
{
    unsigned char p = 3;

    if (h->smsc) {                                     /* SMSC */
        if ((h->imsg[2] & 3) == 1) {                /* SMS-SUBMIT */
            h->udhl = h->udl = 0;
            h->vp = 0;
            h->srr = ((h->imsg[2] & 0x20) ? 1 : 0);
            h->udhi = ((h->imsg[2] & 0x40) ? 1 : 0);
            h->rp = ((h->imsg[2] & 0x80) ? 1 : 0);
            cw_copy_string (h->oa, h->cli, sizeof (h->oa));
            h->scts = time (0);
            h->mr = h->imsg[p++];
            p += unpackaddress (h->da, h->imsg + p);
            h->pid = h->imsg[p++];
            h->dcs = h->imsg[p++];
            if ((h->imsg[2] & 0x18) == 0x10) {                             /* relative VP */
                if (h->imsg[p] < 144)
                    h->vp = (h->imsg[p] + 1) * 5;
                else if (h->imsg[p] < 168)
                    h->vp = 720 + (h->imsg[p] - 143) * 30;
                else if (h->imsg[p] < 197)
                    h->vp = (h->imsg[p] - 166) * 1440;
                else
                    h->vp = (h->imsg[p] - 192) * 10080;
                p++;
            } else if (h->imsg[2] & 0x18)
                p += 7;                 /* ignore enhanced / absolute VP */
            p += unpacksms (h->dcs, h->imsg + p, h->udh, &h->udhl, h->ud, &h->udl, h->udhi);
            h->rx = 1;                 /* received message */
            sms_writefile(h);      /* write the file */
            if (p != h->imsg[1] + 2) {
                cw_log (LOG_WARNING, "Mismatch receive unpacking %d/%d\n", p, h->imsg[1] + 2);
                return 0xFF;          /* duh! */
            }
        } else {
            cw_log (LOG_WARNING, "Unknown message type %02X\n", h->imsg[2]);
            return 0xFF;
        }
    } else {                                     /* client */
        if (!(h->imsg[2] & 3)) {                                 /* SMS-DELIVER */
            *h->da = h->srr = h->rp = h->vp = h->udhi = h->udhl = h->udl = 0;
            h->srr = ((h->imsg[2] & 0x20) ? 1 : 0);
            h->udhi = ((h->imsg[2] & 0x40) ? 1 : 0);
            h->rp = ((h->imsg[2] & 0x80) ? 1 : 0);
            h->mr = -1;
            p += unpackaddress (h->oa, h->imsg + p);
            h->pid = h->imsg[p++];
            h->dcs = h->imsg[p++];
            h->scts = unpackdate(h->imsg + p);
            p += 7;
            p += unpacksms (h->dcs, h->imsg + p, h->udh, &h->udhl, h->ud, &h->udl, h->udhi);
            h->rx = 1;                 /* received message */
            sms_writefile(h);      /* write the file */
            if (p != h->imsg[1] + 2) {
                cw_log (LOG_WARNING, "Mismatch receive unpacking %d/%d\n", p, h->imsg[1] + 2);
                return 0xFF;          /* duh! */
            }
        } else {
            cw_log (LOG_WARNING, "Unknown message type %02X\n", h->imsg[2]);
            return 0xFF;
        }
    }
    return 0;                          /* no error */
}

#ifdef SOLARIS
#define NAME_MAX 1024
#endif

/*! \brief find and fill in next message, or send a REL if none waiting */
static void sms_nextoutgoing(sms_t * h)
{          
    char fn[100 + NAME_MAX] = "";
    DIR *d;
    char more = 0;

    cw_copy_string (fn, spool_dir, sizeof (fn));
    mkdir (fn, 0777);                /* ensure it exists */
    h->rx = 0;                         /* outgoing message */
    snprintf (fn + strlen (fn), sizeof (fn) - strlen (fn), "/%s", h->smsc ? "mttx" : "motx");
    mkdir (fn, 0777);                /* ensure it exists */
    d = opendir (fn);
    if (d) {
        struct dirent *f = readdirqueue (d, h->queue);
        if (f) {
            snprintf (fn + strlen (fn), sizeof (fn) - strlen (fn), "/%s", f->d_name);
            sms_readfile (h, fn);
            if (readdirqueue (d, h->queue))
                more = 1;              /* more to send */
        }
        closedir (d);
    }
    if (*h->da || *h->oa) {                                     /* message to send */
        unsigned char p = 2;
        h->omsg[0] = 0x80 | DLL_SMS_P1_DATA;
        if (h->smsc) {             /* deliver */
            h->omsg[p++] = (more ? 4 : 0);
            p += packaddress (h->omsg + p, h->oa);
            h->omsg[p++] = h->pid;
            h->omsg[p++] = h->dcs;
            packdate (h->omsg + p, h->scts);
            p += 7;
            p += packsms (h->dcs, h->omsg + p, h->udhl, h->udh, h->udl, h->ud);
        } else {             /* submit */
            h->omsg[p++] =
                0x01 + (more ? 4 : 0) + (h->srr ? 0x20 : 0) + (h->rp ? 0x80 : 0) + (h->vp ? 0x10 : 0) + (h->udhi ? 0x40 : 0);
            if (h->mr < 0)
                h->mr = message_ref++;
            h->omsg[p++] = h->mr;
            p += packaddress (h->omsg + p, h->da);
            h->omsg[p++] = h->pid;
            h->omsg[p++] = h->dcs;
            if (h->vp) {         /* relative VP */
                if (h->vp < 720)
                    h->omsg[p++] = (h->vp + 4) / 5 - 1;
                else if (h->vp < 1440)
                    h->omsg[p++] = (h->vp - 720 + 29) / 30 + 143;
                else if (h->vp < 43200)
                    h->omsg[p++] = (h->vp + 1439) / 1440 + 166;
                else if (h->vp < 635040)
                    h->omsg[p++] = (h->vp + 10079) / 10080 + 192;
                else
                    h->omsg[p++] = 255;        /* max */
            }
            p += packsms (h->dcs, h->omsg + p, h->udhl, h->udh, h->udl, h->ud);
        }
        h->omsg[1] = p - 2;
        sms_messagetx (h);
    }
    else
    {
        /* no message */
        h->omsg[0] = 0x80 | DLL_SMS_P1_REL;
        h->omsg[1] = 0;
        sms_messagetx (h);
    }
}

static void sms_debug(char *dir, unsigned char *msg)
{
    char txt[259 * 3 + 1];
    char *p = txt;                         /* always long enough */
    int n = msg[1] + 3;
    int    q = 0;

    while (q < n && q < 30)
    {
        sprintf (p, " %02X", msg[q++]);
        p += 3;
    }
    if (q < n)
        sprintf (p, "...");
    if (option_verbose > 2)
        cw_verbose (VERBOSE_PREFIX_3 "SMS %s%s\n", dir, txt);
}

static void sms_messagerx(sms_t * h)
{
    sms_debug("RX", h->imsg);
    /* testing */
    switch (h->imsg[0])
    {
    case 0x80 | DLL_SMS_P1_DATA:
        {
            unsigned char cause = sms_handleincoming (h);
            if (!cause) {
                sms_log (h, 'Y');
                h->omsg[0] = 0x80 | DLL_SMS_P1_ACK;
                h->omsg[1] = 0x02;
                h->omsg[2] = 0x00;  /* deliver report */
                h->omsg[3] = 0x00;  /* no parameters */
            } else {                             /* NACK */
                sms_log (h, 'N');
                h->omsg[0] = 0x80 | DLL_SMS_P1_NACK;
                h->omsg[1] = 3;
                h->omsg[2] = 0;      /* delivery report */
                h->omsg[3] = cause; /* cause */
                h->omsg[4] = 0;      /* no parameters */
            }
            sms_messagetx (h);
        }
        break;
    case 0x80 | DLL_SMS_P1_ERROR:
        h->err = 1;
        sms_messagetx (h);          /* send whatever we sent again */
        break;
    case 0x80 | DLL_SMS_P1_EST:
        sms_nextoutgoing (h);
        break;
    case 0x80 | DLL_SMS_P1_REL:
        h->hangup = 1;                /* hangup */
        break;
    case 0x80 | DLL_SMS_P1_ACK:
        sms_log (h, 'Y');
        sms_nextoutgoing (h);
        break;
    case 0x80 | DLL_SMS_P1_NACK:
        h->err = 1;
        sms_log (h, 'N');
        sms_nextoutgoing (h);
        break;
    default:                          /* Unknown */
        h->omsg[0] = 0x80 | DLL_SMS_P1_ERROR;
        h->omsg[1] = 1;
        h->omsg[2] = DLL_SMS_ERROR_UNKNOWN_MESSAGE_TYPE;
        sms_messagetx (h);
        break;
    }
}

static void sms_messagetx(sms_t * h)
{
    unsigned char c = 0, p;

    for (p = 0;  p < h->omsg[1] + 2;  p++)
        c += h->omsg[p];
    h->omsg[h->omsg[1] + 2] = 0 - c;
    sms_debug ("TX", h->omsg);
    h->obyte = 1;
    h->opause = 200;
    if (h->omsg[0] == (0x80 | DLL_SMS_P1_EST))
        h->opause = 2400;            /* initial message delay 300ms (for BT) */
    h->obytep = 0;
    h->obitp = 0;
    h->osync = 80;
    h->obyten = h->omsg[1] + 3;
}

static int sms_generate(struct cw_channel *chan, void *data, int samples)
{
#define MAXSAMPLES (800)
    int len;
    struct cw_frame f = { 0 };
    int16_t *buf;
    sms_t *h = data;
    int i;

    if (samples > MAXSAMPLES) {
        cw_log (LOG_WARNING, "Only doing %d samples (%d requested)\n",
             MAXSAMPLES, samples);
        samples = MAXSAMPLES;
    }
    len = samples*sizeof(int16_t) + CW_FRIENDLY_OFFSET;
    buf = alloca(len);

    cw_fr_init_ex(&f, CW_FRAME_VOICE, CW_FORMAT_SLINEAR, "app_sms");
    f.datalen = samples*sizeof(int16_t);
    f.offset = CW_FRIENDLY_OFFSET;
    f.data = ((char *) buf) + CW_FRIENDLY_OFFSET;
    f.samples = samples;

    /* Create a buffer containing the digital sms pattern */
    for (i = 0;  i < samples;  i++)
    {
        buf[i + CW_FRIENDLY_OFFSET/2] = wave[0];
        if (h->opause)
            h->opause--;
        else if (h->obyten || h->osync) {                                 /* sending data */
            buf[i + CW_FRIENDLY_OFFSET/2] = wave[h->ophase];
            if ((h->ophase += ((h->obyte & 1) ? 13 : 21)) >= 80)
                h->ophase -= 80;
            if ((h->ophasep += 12) >= 80) {                             /* next bit */
                h->ophasep -= 80;
                if (h->osync)
                    h->osync--;        /* sending sync bits */
                else {
                    h->obyte >>= 1;
                    h->obitp++;
                    if (h->obitp == 1)
                        h->obyte = 0; /* start bit; */
                    else if (h->obitp == 2)
                        h->obyte = h->omsg[h->obytep];
                    else if (h->obitp == 10) {
                        h->obyte = 1; /* stop bit */
                        h->obitp = 0;
                        h->obytep++;
                        if (h->obytep == h->obyten) {
                            h->obytep = h->obyten = 0; /* sent */
                            h->osync = 10;      /* trailing marks */
                        }
                    }
                }
            }
        }
    }
    if (cw_write(chan, &f) < 0)
    {
        cw_log (LOG_WARNING, "Failed to write frame to '%s': %s\n", chan->name, strerror (errno));
        return -1;
    }
    return 0;
#undef SAMPLE2LEN
#undef MAXSAMPLES
}

static void sms_process(sms_t *h, int samples, signed short *data)
{
    if (h->obyten  ||  h->osync)
        return;                         /* sending */
    while (samples--) {
        unsigned long long m0, m1;
        if (abs (*data) > h->imag)
            h->imag = abs (*data);
        else
            h->imag = h->imag * 7 / 8;
        if (h->imag > 500) {
            h->idle = 0;
            h->ims0 = (h->ims0 * 6 + *data * wave[h->ips0]) / 7;
            h->imc0 = (h->imc0 * 6 + *data * wave[h->ipc0]) / 7;
            h->ims1 = (h->ims1 * 6 + *data * wave[h->ips1]) / 7;
            h->imc1 = (h->imc1 * 6 + *data * wave[h->ipc1]) / 7;
            m0 = h->ims0 * h->ims0 + h->imc0 * h->imc0;
            m1 = h->ims1 * h->ims1 + h->imc1 * h->imc1;
            if ((h->ips0 += 21) >= 80)
                h->ips0 -= 80;
            if ((h->ipc0 += 21) >= 80)
                h->ipc0 -= 80;
            if ((h->ips1 += 13) >= 80)
                h->ips1 -= 80;
            if ((h->ipc1 += 13) >= 80)
                h->ipc1 -= 80;
            {
                char bit;
                h->ibith <<= 1;
                if (m1 > m0)
                    h->ibith |= 1;
                if (h->ibith & 8)
                    h->ibitt--;
                if (h->ibith & 1)
                    h->ibitt++;
                bit = ((h->ibitt > 1) ? 1 : 0);
                if (bit != h->ibitl)
                    h->ibitc = 1;
                else
                    h->ibitc++;
                h->ibitl = bit;
                if (!h->ibitn  &&  h->ibitc == 4  &&  !bit)
                {
                    h->ibitn = 1;
                    h->iphasep = 0;
                }
                if (bit && h->ibitc == 200) {                         /* sync, restart message */
                    h->ierr =
                    h->ibitn =
                    h->ibytep =
                    h->ibytec = 0;
                }
                if (h->ibitn) {
                    h->iphasep += 12;
                    if (h->iphasep >= 80) {                     /* next bit */
                        h->iphasep -= 80;
                        if (h->ibitn++ == 9) {                 /* end of byte */
                            if (!bit)  /* bad stop bit */
                                h->ierr = DLL_SMS_ERROR_UNSPECIFIED_ERROR;
                            else {
                                if (h->ibytep < sizeof (h->imsg)) {
                                    h->imsg[h->ibytep] = h->ibytev;
                                    h->ibytec += h->ibytev;
                                    h->ibytep++;
                                } else if (h->ibytep == sizeof (h->imsg))
                                    h->ierr = DLL_SMS_ERROR_WRONG_MESSAGE_LEN;
                                if (h->ibytep > 1 && h->ibytep == 3 + h->imsg[1] && !h->ierr) {
                                    if (!h->ibytec)
                                        sms_messagerx(h);
                                    else
                                        h->ierr = DLL_SMS_ERROR_WRONG_CHECKSUM;
                                }
                            }
                            h->ibitn = 0;
                        }
                        h->ibytev = (h->ibytev >> 1) + (bit ? 0x80 : 0);
                    }
                }
            }
        }
        else
        {
            /* lost carrier */
            if (h->idle++ == 80000)
            {
                /* nothing happening */
                cw_log (LOG_EVENT, "No data, hanging up\n");
                h->hangup = 1;
                h->err = 1;
            }
            if (h->ierr)
            {
                /* error */
                h->err = DLL_SMS_ERROR_WRONG_CHECKSUM;
                h->omsg[0] = 0x80 | DLL_SMS_P1_ERROR;
                h->omsg[1] = 1;
                h->omsg[2] = h->ierr;
                sms_messagetx(h);  /* send error */
            }
            h->ierr =
            h->ibitn =
            h->ibytep =
            h->ibytec = 0;
        }
        data++;
    }
}

static struct cw_generator smsgen =
{
    alloc:sms_alloc,
    release:sms_release,
    generate:sms_generate,
};

static int sms_exec(struct cw_channel *chan, int argc, char **argv)
{
    sms_t h = { 0 };
    int res = -1;
    struct localuser *u;
    struct cw_frame *f;
    char *d;
    int answer;
    int original_read_fmt;
    int original_write_fmt;

    if (argc < 1 || argc > 2) {
    	cw_log(LOG_ERROR, "Syntax: %s\n", sms_syntax);
	return -1;
    }

    LOCAL_USER_ADD(u);

    h.ipc0 = h.ipc1 = 20;          /* phase for cosine */
    h.dcs = 0xF1;                     /* default */

    if (chan->cid.cid_num)
        cw_copy_string (h.cli, chan->cid.cid_num, sizeof (h.cli));

    answer = 0;

    if (strlen(argv[0]) >= sizeof (h.queue)) {
        cw_log (LOG_ERROR, "Queue name too long\n");
        LOCAL_USER_REMOVE(u);
        return -1;
    }
    strcpy (h.queue, argv[0]);
    for (d = h.queue; *d; d++) {
        if (!isalnum (*d))
            *d = '-';              /* make very safe for filenames */
    }

    if (argc > 1) {
        for (d = argv[1]; *d; d++)
        {
            switch (*d)
            {
            case 'a':                 /* we have to send the initial FSK sequence */
                answer = 1;
                break;
            case 's':                 /* we are acting as a service centre talking to a phone */
                h.smsc = 1;
                break;
                /* the following apply if there is an arg3/4 and apply to the created message file */
            case 'r':
                h.srr = 1;
                break;
            case 'o':
                h.dcs |= 4;            /* octets */
                break;
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':                 /* set the pid for saved local message */
                h.pid = 0x40 + (*d & 0xF);
                break;
            }
        }
    }

    if (argc > 2)
    {
        char *p;
        /* submitting a message, not taking call. */
        /* deprecated, use smsq instead */
        d = argv[2];
        h.scts = time (0);
        for (p = d; *p && *p != ','; p++);
        if (*p)
            *p++ = 0;
        if (strlen ((char *)d) >= sizeof (h.oa))
        {
            cw_log (LOG_ERROR, "Address too long %s\n", d);
            return 0;
        }
        if (h.smsc)
            cw_copy_string(h.oa, (char *) d, sizeof(h.oa));
        else
            cw_copy_string(h.da, (char *) d, sizeof(h.da));
        if (!h.smsc)
            cw_copy_string(h.oa, h.cli, sizeof (h.oa));
        d = p;
        h.udl = 0;
        while (*p && h.udl < SMSLEN)
            h.ud[h.udl++] = utf8decode((unsigned char **)&p);
        if (is7bit (h.dcs) && packsms7 (0, h.udhl, h.udh, h.udl, h.ud) < 0)
            cw_log (LOG_WARNING, "Invalid 7 bit GSM data\n");
        if (is8bit (h.dcs) && packsms8 (0, h.udhl, h.udh, h.udl, h.ud) < 0)
            cw_log (LOG_WARNING, "Invalid 8 bit data\n");
        if (is16bit (h.dcs) && packsms16 (0, h.udhl, h.udh, h.udl, h.ud) < 0)
            cw_log (LOG_WARNING, "Invalid 16 bit data\n");
        h.rx = 0;                  /* sent message */
        h.mr = -1;
        sms_writefile (&h);
        LOCAL_USER_REMOVE(u);
        return 0;
    }

    if (answer)
    {
        /* Set up SMS_EST initial message */
        h.omsg[0] = 0x80 | DLL_SMS_P1_EST;
        h.omsg[1] = 0;
        sms_messagetx (&h);
    }

    if (chan->_state != CW_STATE_UP)
        cw_answer (chan);

    original_read_fmt = chan->readformat;
    res = 0;
    if (original_read_fmt != CW_FORMAT_SLINEAR)
    {
        if ((res = cw_set_read_format(chan, CW_FORMAT_SLINEAR)) < 0)
        {
            cw_log(LOG_WARNING, "Unable to set to linear read mode, giving up\n");
            return -1;
        }
    }
    original_write_fmt = chan->writeformat;
    if (original_write_fmt != CW_FORMAT_SLINEAR)
    {
        if ((res = cw_set_write_format(chan, CW_FORMAT_SLINEAR)) < 0)
        {
            cw_log(LOG_WARNING, "Unable to set to linear write mode, giving up\n");
            if ((res = cw_set_read_format(chan, original_read_fmt)))
                cw_log(LOG_WARNING, "Unable to restore read format on '%s'\n", chan->name);
            return -1;
        }
    }

    if (res < 0)
    {
        cw_log (LOG_ERROR, "Unable to set to linear mode, giving up\n");
        LOCAL_USER_REMOVE(u);
        return -1;
    }

    if (cw_generator_activate(chan, &smsgen, &h) < 0)
    {
        cw_log (LOG_ERROR, "Failed to activate generator on '%s'\n", chan->name);
        LOCAL_USER_REMOVE(u);
        return -1;
    }

    while (cw_waitfor(chan, -1) > -1 && !h.hangup)
    {
        if ((f = cw_read(chan)) == NULL)
            break;
        if (f->frametype == CW_FRAME_VOICE)
            sms_process(&h, f->samples, f->data);
        cw_fr_free(f);
    }
    if (original_read_fmt != CW_FORMAT_SLINEAR)
    {
        if ((res = cw_set_read_format(chan, original_read_fmt)))
            cw_log(LOG_WARNING, "Unable to restore read format on '%s'\n", chan->name);
    }
    if (original_write_fmt != CW_FORMAT_SLINEAR)
    {
        if ((res = cw_set_write_format(chan, original_write_fmt)))
            cw_log(LOG_WARNING, "Unable to restore write format on '%s'\n", chan->name);
    }

    sms_log(&h, '?');              /* log incomplete message */

    LOCAL_USER_REMOVE(u);
    return (h.err);
}

int unload_module(void)
{
    int res = 0;
    
    STANDARD_HANGUP_LOCALUSERS;
    res |= cw_unregister_application(sms_app);
    return res;    
}

int load_module(void)
{
    snprintf(log_file, sizeof (log_file), "%s/sms", cw_config_CW_LOG_DIR);
    snprintf(spool_dir, sizeof (spool_dir), "%s/sms", cw_config_CW_SPOOL_DIR);
    sms_app = cw_register_application(sms_name, sms_exec, sms_synopsis, sms_syntax, sms_descrip);
    return 0;
}

char *description(void)
{
    return (char *)sms_synopsis;
}

int usecount(void)
{
    int res;

    STANDARD_USECOUNT(res);
    return res;
}
