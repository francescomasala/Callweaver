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
 *
 * \brief Frame manipulation routines
 * 
 */

#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <spandsp.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/lock.h"
#include "openpbx/frame.h"
#include "openpbx/logger.h"
#include "openpbx/options.h"
#include "openpbx/channel.h"
#include "openpbx/cli.h"
#include "openpbx/term.h"
#include "openpbx/utils.h"

#ifdef TRACE_FRAMES
static int headers = 0;
static struct opbx_frame *headerlist = NULL;
OPBX_MUTEX_DEFINE_STATIC(framelock);
#endif

#define SMOOTHER_SIZE 8000

#define TYPE_HIGH     0x0
#define TYPE_LOW     0x1
#define TYPE_SILENCE     0x2
#define TYPE_DONTSEND     0x3
#define TYPE_MASK     0x3

struct opbx_format_list
{
    int visible; /* Can we see this entry */
    int bits; /* bitmask value */
    char *name; /* short name */
    char *desc; /* Description */
    int sample_rate;
};

struct opbx_smoother
{
    int size;
    int format;
    int readdata;
    int optimizablestream;
    int flags;
    float samplesperbyte;
    struct opbx_frame f;
    struct timeval delivery;
    char data[SMOOTHER_SIZE];
    char framedata[SMOOTHER_SIZE + OPBX_FRIENDLY_OFFSET];
    struct opbx_frame *opt;
    int len;
};

void opbx_smoother_reset(struct opbx_smoother *s, int size)
{
    memset(s, 0, sizeof(struct opbx_smoother));
    s->size = size;
}

struct opbx_smoother *opbx_smoother_new(int size)
{
    struct opbx_smoother *s;

    if (size < 1)
        return NULL;
    if ((s = malloc(sizeof(struct opbx_smoother))))
        opbx_smoother_reset(s, size);
    return s;
}

int opbx_smoother_get_flags(struct opbx_smoother *s)
{
    return s->flags;
}

void opbx_smoother_set_flags(struct opbx_smoother *s, int flags)
{
    s->flags = flags;
}

int opbx_smoother_test_flag(struct opbx_smoother *s, int flag)
{
    return (s->flags & flag);
}

int __opbx_smoother_feed(struct opbx_smoother *s, struct opbx_frame *f, int swap)
{
    if (f->frametype != OPBX_FRAME_VOICE)
    {
        opbx_log(LOG_WARNING, "Huh?  Can't smooth a non-voice frame!\n");
        return -1;
    }
    if (!s->format)
    {
        s->format = f->subclass;
        s->samplesperbyte = (float) f->samples/(float) f->datalen;
    }
    else if (s->format != f->subclass)
    {
        opbx_log(LOG_WARNING, "Smoother was working on %d format frames, now trying to feed %d?\n", s->format, f->subclass);
        return -1;
    }
    if (s->len + f->datalen > SMOOTHER_SIZE)
    {
        opbx_log(LOG_WARNING, "Out of smoother space\n");
        return -1;
    }
    if (((f->datalen == s->size) || ((f->datalen < 10) && (s->flags & OPBX_SMOOTHER_FLAG_G729)))
        &&
        !s->opt
        &&
        (f->offset >= OPBX_MIN_OFFSET))
    {
        if (!s->len)
        {
            /* Optimize by sending the frame we just got
               on the next read, thus eliminating the douple
               copy */
            s->opt = f;
            return 0;
        }
        else
        {
            s->optimizablestream++;
            if (s->optimizablestream > 10)
            {
                /* For the past 10 rounds, we have input and output
                   frames of the correct size for this smoother, yet
                   we were unable to optimize because there was still
                   some cruft left over.  Lets just drop the cruft so
                   we can move to a fully optimized path */
                s->len = 0;
                s->opt = f;
                return 0;
            }
        }
    }
    else 
    {
        s->optimizablestream = 0;
    }
    if (s->flags & OPBX_SMOOTHER_FLAG_G729)
    {
        if (s->len % 100)
        {
            opbx_log(LOG_NOTICE, "Dropping extra frame of G.729 since we already have a VAD frame at the end\n");
            return 0;
        }
    }
    if (swap)
        opbx_swapcopy_samples(s->data+s->len, f->data, f->samples);
    else
        memcpy(s->data + s->len, f->data, f->datalen);
    /* If either side is empty, reset the delivery time */
    if (!s->len || opbx_tvzero(f->delivery) || opbx_tvzero(s->delivery))    /* XXX really ? */
        s->delivery = f->delivery;
    s->len += f->datalen;
    return 0;
}

struct opbx_frame *opbx_smoother_read(struct opbx_smoother *s)
{
    struct opbx_frame *opt;
    int len;

    /* If we have an optimization frame, send it */
    if (s->opt)
    {
        if (s->opt->offset < OPBX_FRIENDLY_OFFSET)
        {
            opbx_log(LOG_WARNING, "Returning a frame of inappropriate offset (%d).\n",
                     s->opt->offset);
        }
        opt = s->opt;
        s->opt = NULL;
        return opt;
    }

    /* Make sure we have enough data */
    if (s->len < s->size)
    {
        /* Or, if this is a G.729 frame with VAD on it, send it immediately anyway */
        if (!((s->flags & OPBX_SMOOTHER_FLAG_G729) && (s->size % 10)))
            return NULL;
    }
    len = s->size;
    if (len > s->len)
        len = s->len;
    /* Make frame */
    s->f.frametype = OPBX_FRAME_VOICE;
    s->f.subclass = s->format;
    s->f.data = s->framedata + OPBX_FRIENDLY_OFFSET;
    s->f.offset = OPBX_FRIENDLY_OFFSET;
    s->f.datalen = len;
    /* Samples will be improper given VAD, but with VAD the concept really doesn't even exist */
    s->f.samples = len * s->samplesperbyte;    /* XXX rounding */
    s->f.delivery = s->delivery;
    /* Fill Data */
    memcpy(s->f.data, s->data, len);
    s->len -= len;
    /* Move remaining data to the front if applicable */
    if (s->len)
    {
        /* In principle this should all be fine because if we are sending
           G.729 VAD, the next timestamp will take over anyawy */
        memmove(s->data, s->data + len, s->len);
        if (!opbx_tvzero(s->delivery))
        {
            /* If we have delivery time, increment it, otherwise, leave it at 0 */
            s->delivery = opbx_tvadd(s->delivery, opbx_samp2tv(s->f.samples, opbx_codec_sample_rate(&s->f)));
        }
    }
    /* Return frame */
    return &s->f;
}

void opbx_smoother_free(struct opbx_smoother *s)
{
    free(s);
}

static struct opbx_frame *opbx_frame_header_new(void)
{
    struct opbx_frame *f;

    if ((f = malloc(sizeof(struct opbx_frame))))
        memset(f, 0, sizeof(struct opbx_frame));
#ifdef TRACE_FRAMES
    if (f)
    {
        headers++;
        f->prev = NULL;
        opbx_mutex_lock(&framelock);
        f->next = headerlist;
        if (headerlist)
            headerlist->prev = f;
        headerlist = f;
        opbx_mutex_unlock(&framelock);
    }
#endif    
    return f;
}

/*
 * Important: I should be made more efficient.  Frame headers should
 * most definitely be cached
 */

void opbx_fr_init(struct opbx_frame *fr)
{
	fr->frametype = OPBX_FRAME_NULL;
	fr->subclass = 0;
	fr->datalen = 0;
	fr->samples = 0;
	fr->mallocd = 0;
	fr->offset = 0;
	fr->src = "";
	fr->data = NULL;
	fr->delivery.tv_sec = 0;
	fr->delivery.tv_usec = 0;
	fr->seq_no = 0;
	fr->prev = NULL;
	fr->next = NULL;
	fr->has_timing_info = 0;
	fr->ts = 0;
	fr->len = 0;
	fr->seq_no = -1;
	fr->tx_copies = 1;
}

void opbx_fr_init_ex(struct opbx_frame *fr,
                     int frame_type,
                     int sub_type,
                     const char *src)
{
	fr->frametype = frame_type;
	fr->subclass = sub_type;
	fr->datalen = 0;
	fr->samples = 0;
	fr->mallocd = 0;
	fr->offset = 0;
	fr->src = (src)  ?  src  :  "";
	fr->data = NULL;
	fr->delivery = opbx_tv(0,0);
	fr->seq_no = 0;
	fr->prev = NULL;
	fr->next = NULL;
	fr->has_timing_info = 0;
	fr->ts = 0;
	fr->len = 0;
	fr->seq_no = 0;
	fr->tx_copies = 1;
}

void opbx_frfree(struct opbx_frame *fr)
{
    if (fr->mallocd & OPBX_MALLOCD_DATA)
    {
        if (fr->data) 
            free(fr->data - fr->offset);
    }
    if (fr->mallocd & OPBX_MALLOCD_SRC)
    {
        if (fr->src)
            free((char *)fr->src);
    }
    if (fr->mallocd & OPBX_MALLOCD_HDR)
    {
#ifdef TRACE_FRAMES
        headers--;
        opbx_mutex_lock(&framelock);
        if (fr->next)
            fr->next->prev = fr->prev;
        if (fr->prev)
            fr->prev->next = fr->next;
        else
            headerlist = fr->next;
        opbx_mutex_unlock(&framelock);
#endif            
        free(fr);
    }
}

/*
 * 'isolates' a frame by duplicating non-malloc'ed components
 * (header, src, data).
 * On return all components are malloc'ed
 */
struct opbx_frame *opbx_frisolate(struct opbx_frame *fr)
{
    struct opbx_frame *out;
    void *tmp;

    if (!(fr->mallocd & OPBX_MALLOCD_HDR))
    {
        /* Allocate a new header if needed */
        out = opbx_frame_header_new();
        if (!out)
        {
            opbx_log(LOG_WARNING, "Out of memory\n");
            return NULL;
        }
        out->frametype = fr->frametype;
        out->subclass = fr->subclass;
        out->datalen = fr->datalen;
        out->samples = fr->samples;
        out->offset = fr->offset;
        out->src = NULL;
        out->data = fr->data;

        /* Copy the timing data */
        out->has_timing_info = fr->has_timing_info;
        if(fr->has_timing_info)
        {
            out->ts = fr->ts;
            out->len = fr->len;
            out->seqno = fr->seqno;
        }
    }
    else
    {
        out = fr;
    }
    if (!(fr->mallocd & OPBX_MALLOCD_SRC))
    {
        if (fr->src)
        {
            out->src = strdup(fr->src);
            if (!out->src)
            {
                if (out != fr)
                    free(out);
                opbx_log(LOG_WARNING, "Out of memory\n");
                return NULL;
            }
        }
    }
    else
    {
        out->src = fr->src;
    }
    if (!(fr->mallocd & OPBX_MALLOCD_DATA))
    {
            tmp = fr->data;
        out->data = malloc(fr->datalen + OPBX_FRIENDLY_OFFSET);
        if (!out->data)
        {
            free(out);

            opbx_log(LOG_WARNING, "Out of memory\n");
            return NULL;
        }
        out->data += OPBX_FRIENDLY_OFFSET;
        out->offset = OPBX_FRIENDLY_OFFSET;
        out->datalen = fr->datalen;
        memcpy(out->data, tmp, fr->datalen);
    }
    out->mallocd = OPBX_MALLOCD_HDR | OPBX_MALLOCD_SRC | OPBX_MALLOCD_DATA;
    return out;
}

struct opbx_frame *opbx_frdup(struct opbx_frame *f)
{
    struct opbx_frame *out;
    int len, srclen = 0;
    void *buf;
    
    /* Start with standard stuff */
    len = sizeof(struct opbx_frame) + OPBX_FRIENDLY_OFFSET + f->datalen;
    /* If we have a source, add space for it */
    /*
     * XXX Watch out here - if we receive a src which is not terminated
     * properly, we can be easily attacked. Should limit the size we deal with.
     */
    if (f->src)
        srclen = strlen(f->src);
    if (srclen > 0)
        len += srclen + 1;
    if ((buf = malloc(len)) == NULL)
        return NULL;
    out = buf;
    /* Set us as having malloc'd header only, so it will eventually
       get freed. */
    out->frametype = f->frametype;
    out->subclass = f->subclass;
    out->datalen = f->datalen;
    out->samples = f->samples;
    out->delivery = f->delivery;
    out->mallocd = OPBX_MALLOCD_HDR;
    out->offset = OPBX_FRIENDLY_OFFSET;
    out->data = buf + sizeof(struct opbx_frame) + OPBX_FRIENDLY_OFFSET;
    if (srclen > 0)
    {
        out->src = out->data + f->datalen;
        /* Must have space since we allocated for it */
        strcpy((char *)out->src, f->src);
    }
    else
    {
        out->src = NULL;
    }
    out->prev = NULL;
    out->next = NULL;
    memcpy(out->data, f->data, out->datalen);

    out->has_timing_info = f->has_timing_info;
    if (f->has_timing_info)
    {
        out->ts = f->ts;
        out->len = f->len;
        out->seqno = f->seqno;
    }

    return out;
}

#if 0
/*
 * XXX
 * This function is badly broken - it does not handle correctly
 * partial reads on either header or body.
 * However is it never used anywhere so we leave it commented out
 */
struct opbx_frame *opbx_fr_fdread(int fd)
{
    char buf[65536];
    int res;
    int ttl = sizeof(struct opbx_frame);
    struct opbx_frame *f = (struct opbx_frame *)buf;
    /* Read a frame directly from there.  They're always in the
       right format. */
    
    while (ttl)
    {
        res = read(fd, buf, ttl);
        if (res < 0)
        {
            opbx_log(LOG_WARNING, "Bad read on %d: %s\n", fd, strerror(errno));
            return NULL;
        }
        ttl -= res;
    }
    
    /* read the frame header */
    f->mallocd = 0;
    /* Re-write data position */
    f->data = buf + sizeof(struct opbx_frame);
    f->offset = 0;
    /* Forget about being mallocd */
    f->mallocd = 0;
    /* Re-write the source */
    f->src = (char *)__FUNCTION__;
    if (f->datalen > sizeof(buf) - sizeof(struct opbx_frame))
    {
        /* Really bad read */
        opbx_log(LOG_WARNING, "Strange read (%d bytes)\n", f->datalen);
        return NULL;
    }
    if (f->datalen)
    {
        if ((res = read(fd, f->data, f->datalen)) != f->datalen)
        {
            /* Bad read */
            opbx_log(LOG_WARNING, "How very strange, expected %d, got %d\n", f->datalen, res);
            return NULL;
        }
    }
    if ((f->frametype == OPBX_FRAME_CONTROL) && (f->subclass == OPBX_CONTROL_HANGUP))
    {
        return NULL;
    }
    return opbx_frisolate(f);
}

/* Some convenient routines for sending frames to/from stream or datagram
   sockets, pipes, etc (maybe even files) */

/*
 * XXX this function is also partly broken because it does not handle
 * partial writes. We comment it out too, and also the unique
 * client it has, opbx_fr_fdhangup()
 */
int opbx_fr_fdwrite(int fd, struct opbx_frame *frame)
{
    /* Write the frame exactly */
    if (write(fd, frame, sizeof(struct opbx_frame)) != sizeof(struct opbx_frame))
    {
        opbx_log(LOG_WARNING, "Write error: %s\n", strerror(errno));
        return -1;
    }
    if (write(fd, frame->data, frame->datalen) != frame->datalen)
    {
        opbx_log(LOG_WARNING, "Write error: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int opbx_fr_fdhangup(int fd)
{
    struct opbx_frame hangup =
    {
        OPBX_FRAME_CONTROL,
        OPBX_CONTROL_HANGUP
    };
    return opbx_fr_fdwrite(fd, &hangup);
}

#endif /* unused functions */

void opbx_swapcopy_samples(void *dst, const void *src, int samples)
{
    int i;
    int16_t *dst_s = dst;
    const int16_t *src_s = src;

    for (i = 0;  i < samples;  i++)
        dst_s[i] = (src_s[i] << 8) | (src_s[i] >> 8);
}

static struct opbx_format_list OPBX_FORMAT_LIST[] =
{
    { 1, OPBX_FORMAT_G723_1 , "g723" , "G.723.1", 8000},
    { 1, OPBX_FORMAT_GSM, "gsm" , "GSM", 8000},
    { 1, OPBX_FORMAT_ULAW, "ulaw", "G.711 u-law", 8000},
    { 1, OPBX_FORMAT_ALAW, "alaw", "G.711 A-law", 8000},
    { 1, OPBX_FORMAT_G726, "g726", "G.726", 8000},
    { 1, OPBX_FORMAT_DVI_ADPCM, "adpcm" , "ADPCM", 8000},
    { 1, OPBX_FORMAT_SLINEAR, "slin",  "16 bit Signed Linear PCM", 8000},
    { 1, OPBX_FORMAT_LPC10, "lpc10", "LPC10", 8000},
    { 1, OPBX_FORMAT_G729A, "g729", "G.729A", 8000},
    { 1, OPBX_FORMAT_SPEEX, "speex", "SpeeX", 8000},
    { 1, OPBX_FORMAT_ILBC, "ilbc", "iLBC", 8000},
    { 0, 0, "nothing", "undefined", 8000},
    { 0, 0, "nothing", "undefined", 8000},
    { 0, 0, "nothing", "undefined", 8000},
    { 0, 0, "nothing", "undefined", 8000},
    { 0, OPBX_FORMAT_MAX_AUDIO, "maxaudio", "Maximum audio format", 8000},
    { 1, OPBX_FORMAT_JPEG, "jpeg", "JPEG image", 90000},
    { 1, OPBX_FORMAT_PNG, "png", "PNG image", 90000},
    { 1, OPBX_FORMAT_H261, "h261", "H.261 Video", 90000},
    { 1, OPBX_FORMAT_H263, "h263", "H.263 Video", 90000},
    { 1, OPBX_FORMAT_H263_PLUS, "h263p", "H.263+ Video", 90000},
    { 1, OPBX_FORMAT_H264, "h264", "H.264 Video", 90000},      /*!< Passthrough support, see format_h263.c */
    { 0, 0, "nothing", "undefined", 90000},
    { 0, 0, "nothing", "undefined", 90000},
    { 0, 0, "nothing", "undefined", 90000},
    { 0, 0, "nothing", "undefined", 90000},
    { 0, OPBX_FORMAT_MAX_VIDEO, "maxvideo", "Maximum video format", 90000},
};

int opbx_codec_sample_rate(struct opbx_frame *f)
{
	int codec;

	if (f == NULL)
		return -1;
	if (f->frametype != OPBX_FRAME_VOICE)
		return -1;
	codec = f->subclass & OPBX_AUDIO_CODEC_MASK;
	if (codec == 0)
		return -1;
	return OPBX_FORMAT_LIST[top_bit(codec)].sample_rate;
}

struct opbx_format_list *opbx_get_format_list_index(int index) 
{
    return &OPBX_FORMAT_LIST[index];
}

struct opbx_format_list *opbx_get_format_list(size_t *size) 
{
    *size = (sizeof(OPBX_FORMAT_LIST)/sizeof(struct opbx_format_list));
    return OPBX_FORMAT_LIST;
}

char *opbx_getformatname(int format)
{
    int x = 0;
    char *ret = "unknown";

    for (x = 0;  x < sizeof(OPBX_FORMAT_LIST)/sizeof(struct opbx_format_list);  x++)
    {
        if (OPBX_FORMAT_LIST[x].visible && OPBX_FORMAT_LIST[x].bits == format)
        {
            ret = OPBX_FORMAT_LIST[x].name;
            break;
        }
    }
    return ret;
}

char *opbx_getformatname_multiple(char *buf, size_t size, int format)
{

    int x = 0;
    unsigned len;
    char *end = buf;
    char *start = buf;
    if (!size) return buf;
    snprintf(end, size, "0x%x (", format);
    len = strlen(end);
    end += len;
    size -= len;
    start = end;
    for (x = 0 ; x < sizeof(OPBX_FORMAT_LIST) / sizeof(struct opbx_format_list) ; x++)
    {
        if (OPBX_FORMAT_LIST[x].visible && (OPBX_FORMAT_LIST[x].bits & format))
        {
            snprintf(end, size,"%s|",OPBX_FORMAT_LIST[x].name);
            len = strlen(end);
            end += len;
            size -= len;
        }
    }
    if (start == end)
        snprintf(start, size, "nothing)");
    else if (size > 1)
        *(end -1) = ')';
    return buf;
}

static struct opbx_codec_alias_table
{
    char *alias;
    char *realname;

} opbx_codec_alias_table[] =
{
    {"slinear", "slin"},
    {"g723.1", "g723"},
    {"g711u", "ulaw"},
    {"g711a", "alaw"},
};

static char *opbx_expand_codec_alias(char *in)
{
    int x = 0;

    for (x = 0; x < sizeof(opbx_codec_alias_table) / sizeof(struct opbx_codec_alias_table) ; x++) {
        if(!strcmp(in,opbx_codec_alias_table[x].alias))
            return opbx_codec_alias_table[x].realname;
    }
    return in;
}

int opbx_getformatbyname(char *name)
{
    int x = 0, all = 0, format = 0;

    all = strcasecmp(name, "all") ? 0 : 1;
    for (x = 0 ; x < sizeof(OPBX_FORMAT_LIST)/sizeof(struct opbx_format_list) ; x++)
    {
        if (OPBX_FORMAT_LIST[x].visible && (all || 
            !strcasecmp(OPBX_FORMAT_LIST[x].name,name) ||
            !strcasecmp(OPBX_FORMAT_LIST[x].name,opbx_expand_codec_alias(name))))
        {
            format |= OPBX_FORMAT_LIST[x].bits;
            if(!all)
                break;
        }
    }

    return format;
}

char *opbx_codec2str(int codec)
{
    int x = 0;
    char *ret = "unknown";

    for (x = 0 ; x < sizeof(OPBX_FORMAT_LIST)/sizeof(struct opbx_format_list) ; x++)
    {
        if (OPBX_FORMAT_LIST[x].visible && OPBX_FORMAT_LIST[x].bits == codec)
        {
            ret = OPBX_FORMAT_LIST[x].desc;
            break;
        }
    }
    return ret;
}

static int show_codecs(int fd, int argc, char *argv[])
{
    int i, found=0;
    char hex[25];
    
    if ((argc < 2)  ||  (argc > 3))
        return RESULT_SHOWUSAGE;

    if (!option_dontwarn)
        opbx_cli(fd, "Disclaimer: this command is for informational purposes only.\n"
                "\tIt does not indicate anything about your configuration.\n");

    opbx_cli(fd, "%11s %9s %10s   TYPE   %5s   %s\n","INT","BINARY","HEX","NAME","DESC");
    opbx_cli(fd, "--------------------------------------------------------------------------------\n");
    if ((argc == 2) || (!strcasecmp(argv[1],"audio")))
    {
        found = 1;
        for (i = 0;  i < 11;  i++)
        {
            snprintf(hex,25,"(0x%x)",1<<i);
            opbx_cli(fd, "%11u (1 << %2d) %10s  audio   %5s   (%s)\n",1 << i,i,hex,opbx_getformatname(1<<i),opbx_codec2str(1<<i));
        }
    }

    if ((argc == 2) || (!strcasecmp(argv[1],"image")))
    {
        found = 1;
        for (i=16;i<18;i++) {
            snprintf(hex,25,"(0x%x)",1<<i);
            opbx_cli(fd, "%11u (1 << %2d) %10s  image   %5s   (%s)\n",1 << i,i,hex,opbx_getformatname(1<<i),opbx_codec2str(1<<i));
        }
    }

    if ((argc == 2) || (!strcasecmp(argv[1],"video")))
    {
        found = 1;
        for (i=18;i<22;i++) {
            snprintf(hex,25,"(0x%x)",1<<i);
            opbx_cli(fd, "%11u (1 << %2d) %10s  video   %5s   (%s)\n",1 << i,i,hex,opbx_getformatname(1<<i),opbx_codec2str(1<<i));
        }
    }

    if (!found)
        return RESULT_SHOWUSAGE;
    else
        return RESULT_SUCCESS;
}

static char frame_show_codecs_usage[] =
"Usage: show [audio|video|image] codecs\n"
"       Displays codec mapping\n";

static int show_codec_n(int fd, int argc, char *argv[])
{
    int codec;
    int i;
    int found = 0;

    if (argc != 3)
        return RESULT_SHOWUSAGE;

    if (sscanf(argv[2],"%d",&codec) != 1)
        return RESULT_SHOWUSAGE;

    for (i = 0;  i < 32;  i++)
    {
        if (codec & (1 << i))
        {
            found = 1;
            opbx_cli(fd, "%11u (1 << %2d)  %s\n",1 << i,i,opbx_codec2str(1<<i));
        }
    }
    if (!found)
        opbx_cli(fd, "Codec %d not found\n", codec);

    return RESULT_SUCCESS;
}

static char frame_show_codec_n_usage[] =
"Usage: show codec <number>\n"
"       Displays codec mapping\n";

void opbx_frame_dump(char *name, struct opbx_frame *f, char *prefix)
{
    char *n = "unknown";
    char ftype[40] = "Unknown Frametype";
    char cft[80];
    char subclass[40] = "Unknown Subclass";
    char csub[80];
    char moreinfo[40] = "";
    char cn[60];
    char cp[40];
    char cmn[40];
    if (name)
        n = name;
    if (!f) {
        opbx_verbose("%s [ %s (NULL) ] [%s]\n", 
            opbx_term_color(cp, prefix, COLOR_BRMAGENTA, COLOR_BLACK, sizeof(cp)),
            opbx_term_color(cft, "HANGUP", COLOR_BRRED, COLOR_BLACK, sizeof(cft)), 
            opbx_term_color(cn, n, COLOR_YELLOW, COLOR_BLACK, sizeof(cn)));
        return;
    }
    /* XXX We should probably print one each of voice and video when the format changes XXX */
    if (f->frametype == OPBX_FRAME_VOICE)
        return;
    if (f->frametype == OPBX_FRAME_VIDEO)
        return;
    switch(f->frametype) {
    case OPBX_FRAME_DTMF:
        strcpy(ftype, "DTMF");
        subclass[0] = f->subclass;
        subclass[1] = '\0';
        break;
    case OPBX_FRAME_CONTROL:
        strcpy(ftype, "Control");
        switch(f->subclass) {
        case OPBX_CONTROL_HANGUP:
            strcpy(subclass, "Hangup");
            break;
        case OPBX_CONTROL_RING:
            strcpy(subclass, "Ring");
            break;
        case OPBX_CONTROL_RINGING:
            strcpy(subclass, "Ringing");
            break;
        case OPBX_CONTROL_ANSWER:
            strcpy(subclass, "Answer");
            break;
        case OPBX_CONTROL_BUSY:
            strcpy(subclass, "Busy");
            break;
        case OPBX_CONTROL_TAKEOFFHOOK:
            strcpy(subclass, "Take Off Hook");
            break;
        case OPBX_CONTROL_OFFHOOK:
            strcpy(subclass, "Line Off Hook");
            break;
        case OPBX_CONTROL_CONGESTION:
            strcpy(subclass, "Congestion");
            break;
        case OPBX_CONTROL_FLASH:
            strcpy(subclass, "Flash");
            break;
        case OPBX_CONTROL_WINK:
            strcpy(subclass, "Wink");
            break;
        case OPBX_CONTROL_OPTION:
            strcpy(subclass, "Option");
            break;
        case OPBX_CONTROL_RADIO_KEY:
            strcpy(subclass, "Key Radio");
            break;
        case OPBX_CONTROL_RADIO_UNKEY:
            strcpy(subclass, "Unkey Radio");
            break;
        case -1:
            strcpy(subclass, "Stop generators");
            break;
        default:
            snprintf(subclass, sizeof(subclass), "Unknown control '%d'", f->subclass);
        }
        break;
    case OPBX_FRAME_NULL:
        strcpy(ftype, "Null Frame");
        strcpy(subclass, "N/A");
        break;
    case OPBX_FRAME_IAX:
        /* Should never happen */
        strcpy(ftype, "IAX Specific");
        snprintf(subclass, sizeof(subclass), "IAX Frametype %d", f->subclass);
        break;
    case OPBX_FRAME_TEXT:
        strcpy(ftype, "Text");
        strcpy(subclass, "N/A");
        opbx_copy_string(moreinfo, f->data, sizeof(moreinfo));
        break;
    case OPBX_FRAME_IMAGE:
        strcpy(ftype, "Image");
        snprintf(subclass, sizeof(subclass), "Image format %s\n", opbx_getformatname(f->subclass));
        break;
    case OPBX_FRAME_HTML:
        strcpy(ftype, "HTML");
        switch(f->subclass) {
        case OPBX_HTML_URL:
            strcpy(subclass, "URL");
            opbx_copy_string(moreinfo, f->data, sizeof(moreinfo));
            break;
        case OPBX_HTML_DATA:
            strcpy(subclass, "Data");
            break;
        case OPBX_HTML_BEGIN:
            strcpy(subclass, "Begin");
            break;
        case OPBX_HTML_END:
            strcpy(subclass, "End");
            break;
        case OPBX_HTML_LDCOMPLETE:
            strcpy(subclass, "Load Complete");
            break;
        case OPBX_HTML_NOSUPPORT:
            strcpy(subclass, "No Support");
            break;
        case OPBX_HTML_LINKURL:
            strcpy(subclass, "Link URL");
            opbx_copy_string(moreinfo, f->data, sizeof(moreinfo));
            break;
        case OPBX_HTML_UNLINK:
            strcpy(subclass, "Unlink");
            break;
        case OPBX_HTML_LINKREJECT:
            strcpy(subclass, "Link Reject");
            break;
        default:
            snprintf(subclass, sizeof(subclass), "Unknown HTML frame '%d'\n", f->subclass);
            break;
        }
        break;
    default:
        snprintf(ftype, sizeof(ftype), "Unknown Frametype '%d'", f->frametype);
    }
    if (!opbx_strlen_zero(moreinfo))
        opbx_verbose("%s [ TYPE: %s (%d) SUBCLASS: %s (%d) '%s' ] [%s]\n",  
            opbx_term_color(cp, prefix, COLOR_BRMAGENTA, COLOR_BLACK, sizeof(cp)),
            opbx_term_color(cft, ftype, COLOR_BRRED, COLOR_BLACK, sizeof(cft)),
            f->frametype, 
            opbx_term_color(csub, subclass, COLOR_BRCYAN, COLOR_BLACK, sizeof(csub)),
            f->subclass, 
            opbx_term_color(cmn, moreinfo, COLOR_BRGREEN, COLOR_BLACK, sizeof(cmn)),
            opbx_term_color(cn, n, COLOR_YELLOW, COLOR_BLACK, sizeof(cn)));
    else
        opbx_verbose("%s [ TYPE: %s (%d) SUBCLASS: %s (%d) ] [%s]\n",  
            opbx_term_color(cp, prefix, COLOR_BRMAGENTA, COLOR_BLACK, sizeof(cp)),
            opbx_term_color(cft, ftype, COLOR_BRRED, COLOR_BLACK, sizeof(cft)),
            f->frametype, 
            opbx_term_color(csub, subclass, COLOR_BRCYAN, COLOR_BLACK, sizeof(csub)),
            f->subclass, 
            opbx_term_color(cn, n, COLOR_YELLOW, COLOR_BLACK, sizeof(cn)));

}

#ifdef TRACE_FRAMES
static int show_frame_stats(int fd, int argc, char *argv[])
{
    struct opbx_frame *f;
    int x = 1;

    if (argc != 3)
        return RESULT_SHOWUSAGE;
    opbx_cli(fd, "     Framer Statistics     \n");
    opbx_cli(fd, "---------------------------\n");
    opbx_cli(fd, "Total allocated headers: %d\n", headers);
    opbx_cli(fd, "Queue Dump:\n");
    opbx_mutex_lock(&framelock);
    for (f=headerlist;  f;  f = f->next)
    {
        opbx_cli(fd, "%d.  Type %d, subclass %d from %s\n", x++, f->frametype, f->subclass, f->src ? f->src : "<Unknown>");
    }
    opbx_mutex_unlock(&framelock);
    return RESULT_SUCCESS;
}

static char frame_stats_usage[] =
"Usage: show frame stats\n"
"       Displays debugging statistics from framer\n";
#endif

/* XXX no unregister function here ??? */
static struct opbx_cli_entry my_clis[] = {
{ { "show", "codecs", NULL }, show_codecs, "Shows codecs", frame_show_codecs_usage },
{ { "show", "audio", "codecs", NULL }, show_codecs, "Shows audio codecs", frame_show_codecs_usage },
{ { "show", "video", "codecs", NULL }, show_codecs, "Shows video codecs", frame_show_codecs_usage },
{ { "show", "image", "codecs", NULL }, show_codecs, "Shows image codecs", frame_show_codecs_usage },
{ { "show", "codec", NULL }, show_codec_n, "Shows a specific codec", frame_show_codec_n_usage },
#ifdef TRACE_FRAMES
{ { "show", "frame", "stats", NULL }, show_frame_stats, "Shows frame statistics", frame_stats_usage },
#endif
};

int init_framer(void)
{
    opbx_cli_register_multiple(my_clis, sizeof(my_clis)/sizeof(my_clis[0]) );
    return 0;    
}

void opbx_codec_pref_convert(struct opbx_codec_pref *pref, char *buf, size_t size, int right) 
{
    int x = 0, differential = (int) 'A', mem = 0;
    char *from = NULL, *to = NULL;

    if(right) {
        from = pref->order;
        to = buf;
        mem = size;
    } else {
        to = pref->order;
        from = buf;
        mem = 32;
    }

    memset(to, 0, mem);
    for (x = 0; x < 32 ; x++) {
        if(!from[x])
            break;
        to[x] = right ? (from[x] + differential) : (from[x] - differential);
    }
}

int opbx_codec_pref_string(struct opbx_codec_pref *pref, char *buf, size_t size) 
{
    int x = 0;
    int codec = 0; 
    size_t total_len = 0;
    size_t slen = 0;
    char *formatname = 0;
    
    memset(buf,0,size);
    total_len = size;
    buf[0] = '(';
    total_len--;
    for (x = 0;  x < 32;  x++)
    {
        if (total_len <= 0)
            break;
        if (!(codec = opbx_codec_pref_index(pref,x)))
            break;
        if ((formatname = opbx_getformatname(codec)))
        {
            slen = strlen(formatname);
            if (slen > total_len)
                break;
            strncat(buf,formatname,total_len);
            total_len -= slen;
        }
        if (total_len && x < 31 && opbx_codec_pref_index(pref , x + 1))
        {
            strncat(buf,"|",total_len);
            total_len--;
        }
    }
    if (total_len)
    {
        strncat(buf,")",total_len);
        total_len--;
    }

    return size - total_len;
}

int opbx_codec_pref_index(struct opbx_codec_pref *pref, int index) 
{
    int slot = 0;

    
    if ((index >= 0) && (index < sizeof(pref->order)))
        slot = pref->order[index];

    return slot ? OPBX_FORMAT_LIST[slot-1].bits : 0;
}

/*--- opbx_codec_pref_remove: Remove codec from pref list ---*/
void opbx_codec_pref_remove(struct opbx_codec_pref *pref, int format)
{
    struct opbx_codec_pref oldorder;
    int x=0, y=0;
    size_t size = 0;
    int slot = 0;

    if(!pref->order[0])
        return;

    size = sizeof(OPBX_FORMAT_LIST) / sizeof(struct opbx_format_list);

    memcpy(&oldorder,pref,sizeof(struct opbx_codec_pref));
    memset(pref,0,sizeof(struct opbx_codec_pref));

    for (x = 0; x < size; x++) {
        slot = oldorder.order[x];
        if(! slot)
            break;
        if(OPBX_FORMAT_LIST[slot-1].bits != format)
            pref->order[y++] = slot;
    }
    
}

/*--- opbx_codec_pref_append: Append codec to list ---*/
int opbx_codec_pref_append(struct opbx_codec_pref *pref, int format)
{
    size_t size = 0;
    int x = 0, newindex = -1;

    opbx_codec_pref_remove(pref, format);
    size = sizeof(OPBX_FORMAT_LIST) / sizeof(struct opbx_format_list);

    for (x = 0; x < size; x++) {
        if(OPBX_FORMAT_LIST[x].bits == format) {
            newindex = x + 1;
            break;
        }
    }

    if(newindex) {
        for (x = 0; x < size; x++) {
            if(!pref->order[x]) {
                pref->order[x] = newindex;
                break;
            }
        }
    }

    return x;
}


/*--- sip_codec_choose: Pick a codec ---*/
int opbx_codec_choose(struct opbx_codec_pref *pref, int formats, int find_best)
{
    size_t size = 0;
    int x = 0, ret = 0, slot = 0;

    size = sizeof(OPBX_FORMAT_LIST) / sizeof(struct opbx_format_list);
    for (x = 0; x < size; x++) {
        slot = pref->order[x];

        if(!slot)
            break;
        if ( formats & OPBX_FORMAT_LIST[slot-1].bits ) {
            ret = OPBX_FORMAT_LIST[slot-1].bits;
            break;
        }
    }
    if (ret)
        return ret;

    return find_best ? opbx_best_codec(formats) : 0;
}

void opbx_parse_allow_disallow(struct opbx_codec_pref *pref, int *mask, const char *list, int allowing)
{
    int format_i = 0;
    char *next_format = NULL, *lopbx_format = NULL;

    lopbx_format = opbx_strdupa(list);
    while (lopbx_format)
    {
        if((next_format = strchr(lopbx_format, ',')))
        {
            *next_format = '\0';
            next_format++;
        }
        if ((format_i = opbx_getformatbyname(lopbx_format)) > 0) {
            if (mask) {
                if (allowing)
                    (*mask) |= format_i;
                else
                    (*mask) &= ~format_i;
            }
            /* can't consider 'all' a prefered codec*/
            if(pref && strcasecmp(lopbx_format, "all")) {
                if(allowing)
                    opbx_codec_pref_append(pref, format_i);
                else
                    opbx_codec_pref_remove(pref, format_i);
            } else if(!allowing) /* disallow all must clear your prefs or it makes no sense */
                memset(pref, 0, sizeof(struct opbx_codec_pref));
        } else
            opbx_log(LOG_WARNING, "Cannot %s unknown format '%s'\n", allowing ? "allow" : "disallow", lopbx_format);

        lopbx_format = next_format;
    }
}

static int g723_len(unsigned char buf)
{
    switch (buf & TYPE_MASK)
    {
    case TYPE_DONTSEND:
        return 0;
        break;
    case TYPE_SILENCE:
        return 4;
        break;
    case TYPE_HIGH:
        return 24;
        break;
    case TYPE_LOW:
        return 20;
        break;
    default:
        opbx_log(LOG_WARNING, "Badly encoded frame (%d)\n", buf & TYPE_MASK);
    }
    return -1;
}

static int g723_samples(unsigned char *buf, int maxlen)
{
    int pos = 0;
    int samples = 0;
    int res;
    while(pos < maxlen) {
        res = g723_len(buf[pos]);
        if (res <= 0)
            break;
        samples += 240;
        pos += res;
    }
    return samples;
}

static unsigned char get_n_bits_at(unsigned char *data, int n, int bit)
{
    int byte = bit / 8;       /* byte containing first bit */
    int rem = 8 - (bit % 8);  /* remaining bits in first byte */
    unsigned char ret = 0;
    
    if (n <= 0 || n > 8)
        return 0;

    if (rem < n) {
        ret = (data[byte] << (n - rem));
        ret |= (data[byte + 1] >> (8 - n + rem));
    } else {
        ret = (data[byte] >> (rem - n));
    }

    return (ret & (0xff >> (8 - n)));
}

static int speex_get_wb_sz_at(unsigned char *data, int len, int bit)
{
    static int SpeexWBSubModeSz[] =
    {
        0, 36, 112, 192,
        352, 0, 0, 0
    };
    int off = bit;
    unsigned char c;

    /* skip up to two wideband frames */
    if (((len * 8 - off) >= 5) && 
        get_n_bits_at(data, 1, off))
    {
        c = get_n_bits_at(data, 3, off + 1);
        off += SpeexWBSubModeSz[c];

        if (((len * 8 - off) >= 5) && 
            get_n_bits_at(data, 1, off))
        {
            c = get_n_bits_at(data, 3, off + 1);
            off += SpeexWBSubModeSz[c];

            if (((len * 8 - off) >= 5) && 
                get_n_bits_at(data, 1, off))
            {
                opbx_log(LOG_WARNING, "Encountered corrupt speex frame; too many wideband frames in a row.\n");
                return -1;
            }
        }

    }
    return off - bit;
}

static int speex_samples(unsigned char *data, int len)
{
    static int SpeexSubModeSz[] = {
               5, 43, 119, 160,
        220, 300, 364, 492, 
        79, 0, 0, 0,
        0, 0, 0, 0 };
    static int SpeexInBandSz[] =
    { 
        1, 1, 4, 4,
        4, 4, 4, 4,
        8, 8, 16, 16,
        32, 32, 64, 64
    };
    int bit = 0;
    int cnt = 0;
    int off = 0;
    unsigned char c;

    while ((len * 8 - bit) >= 5)
    {
        /* skip wideband frames */
        off = speex_get_wb_sz_at(data, len, bit);
        if (off < 0)
        {
            opbx_log(LOG_WARNING, "Had error while reading wideband frames for speex samples\n");
            break;
        }
        bit += off;

        if ((len * 8 - bit) < 5)
        {
            opbx_log(LOG_WARNING, "Not enough bits remaining after wide band for speex samples.\n");
            break;
        }

        /* get control bits */
        c = get_n_bits_at(data, 5, bit);
        bit += 5;

        if (c == 15)
        {
            /* terminator */
            break; 
        }
        else if (c == 14)
        {
            /* in-band signal; next 4 bits contain signal id */
            c = get_n_bits_at(data, 4, bit);
            bit += 4;
            bit += SpeexInBandSz[c];
        }
        else if (c == 13)
        {
            /* user in-band; next 5 bits contain msg len */
            c = get_n_bits_at(data, 5, bit);
            bit += 5;
            bit += c * 8;
        }
        else if (c > 8)
        {
            /* unknown */
            break;
        }
        else
        {
            /* skip number bits for submode (less the 5 control bits) */
            bit += SpeexSubModeSz[c] - 5;
            cnt += 160; /* new frame */
        }
    }
    return cnt;
}

int opbx_codec_get_samples(struct opbx_frame *f)
{
    int samples = 0;

    switch (f->subclass)
    {
    case OPBX_FORMAT_SPEEX:
        samples = speex_samples(f->data, f->datalen);
        break;
    case OPBX_FORMAT_G723_1:
                samples = g723_samples(f->data, f->datalen);
        break;
    case OPBX_FORMAT_ILBC:
        samples = 240 * (f->datalen / 50);
        break;
    case OPBX_FORMAT_GSM:
        samples = 160 * (f->datalen / 33);
        break;
    case OPBX_FORMAT_G729A:
        samples = f->datalen * 8;
        break;
    case OPBX_FORMAT_SLINEAR:
        samples = f->datalen / 2;
        break;
    case OPBX_FORMAT_LPC10:
        /* assumes that the RTP packet contains one LPC10 frame */
        samples = 22 * 8;
        samples += (((char *)(f->data))[7] & 0x1) * 8;
        break;
    case OPBX_FORMAT_ULAW:
    case OPBX_FORMAT_ALAW:
        samples = f->datalen;
        break;
    case OPBX_FORMAT_DVI_ADPCM:
    case OPBX_FORMAT_G726:
        samples = f->datalen * 2;
        break;
    default:
        opbx_log(LOG_WARNING, "Unable to calculate samples for format %s\n", opbx_getformatname(f->subclass));
    }
    return samples;
}

int opbx_codec_get_len(int format, int samples)
{
    int len = 0;

    /* XXX Still need speex, g723, and lpc10 XXX */    
    switch (format)
    {
    case OPBX_FORMAT_ILBC:
        len = (samples / 240) * 50;
        break;
    case OPBX_FORMAT_GSM:
        len = (samples / 160) * 33;
        break;
    case OPBX_FORMAT_G729A:
        len = samples / 8;
        break;
    case OPBX_FORMAT_SLINEAR:
        len = samples * 2;
        break;
    case OPBX_FORMAT_ULAW:
    case OPBX_FORMAT_ALAW:
        len = samples;
        break;
    case OPBX_FORMAT_DVI_ADPCM:
    case OPBX_FORMAT_G726:
        len = samples / 2;
        break;
    default:
        opbx_log(LOG_WARNING, "Unable to calculate sample length for format %s\n", opbx_getformatname(format));
    }

    return len;
}

int opbx_frame_adjust_volume(struct opbx_frame *f, int adjustment)
{
    int count;
    short *fdata = f->data;
    short adjust_value = abs(adjustment);

    if ((f->frametype != OPBX_FRAME_VOICE) || (f->subclass != OPBX_FORMAT_SLINEAR))
        return -1;

    if (!adjustment)
        return 0;

    for (count = 0;  count < f->samples;  count++)
    {
        if (adjustment > 0)
            opbx_slinear_saturated_multiply(&fdata[count], &adjust_value);
        else if (adjustment < 0)
            opbx_slinear_saturated_divide(&fdata[count], &adjust_value);
    }

    return 0;
}

int opbx_frame_slinear_sum(struct opbx_frame *f1, struct opbx_frame *f2)
{
    int count;
    short *data1, *data2;

    if ((f1->frametype != OPBX_FRAME_VOICE) || (f1->subclass != OPBX_FORMAT_SLINEAR))
        return -1;

    if ((f2->frametype != OPBX_FRAME_VOICE) || (f2->subclass != OPBX_FORMAT_SLINEAR))
        return -1;

    if (f1->samples != f2->samples)
        return -1;

    for (count = 0, data1 = f1->data, data2 = f2->data;
         count < f1->samples;
         count++, data1++, data2++)
    {
        opbx_slinear_saturated_add(data1, data2);
    }
    return 0;
}
