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

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/corelib/frame.c $", "$Revision: 4723 $")

#include "callweaver/lock.h"
#include "callweaver/frame.h"
#include "callweaver/logger.h"
#include "callweaver/options.h"
#include "callweaver/channel.h"
#include "callweaver/cli.h"
#include "callweaver/term.h"
#include "callweaver/utils.h"

#ifdef TRACE_FRAMES
static int headers = 0;
static struct cw_frame *headerlist = NULL;
CW_MUTEX_DEFINE_STATIC(framelock);
#endif

#define SMOOTHER_SIZE 8000

#define TYPE_HIGH       0x0
#define TYPE_LOW        0x1
#define TYPE_SILENCE    0x2
#define TYPE_DONTSEND   0x3
#define TYPE_MASK       0x3

struct cw_format_list_s
{
    int visible; /* Can we see this entry */
    int bits; /* bitmask value */
    char *name; /* short name */
    char *desc; /* Description */
    int sample_rate;
};

struct cw_smoother
{
    int size;
    int format;
    int readdata;
    int optimizablestream;
    int flags;
    float samplesperbyte;
    struct cw_frame f;
    struct timeval delivery;
    char data[SMOOTHER_SIZE];
    char framedata[SMOOTHER_SIZE + CW_FRIENDLY_OFFSET];
    struct cw_frame *opt;
    int len;
};

void cw_smoother_reset(struct cw_smoother *s, int size)
{
    memset(s, 0, sizeof(struct cw_smoother));
    s->size = size;
}

struct cw_smoother *cw_smoother_new(int size)
{
    struct cw_smoother *s;

    if (size < 1)
        return NULL;
    if ((s = malloc(sizeof(struct cw_smoother))))
        cw_smoother_reset(s, size);
    return s;
}

int cw_smoother_get_flags(struct cw_smoother *s)
{
    return s->flags;
}

void cw_smoother_set_flags(struct cw_smoother *s, int flags)
{
    s->flags = flags;
}

int cw_smoother_test_flag(struct cw_smoother *s, int flag)
{
    return (s->flags & flag);
}

int __cw_smoother_feed(struct cw_smoother *s, struct cw_frame *f, int swap)
{
    if (f->frametype != CW_FRAME_VOICE)
    {
        cw_log(LOG_WARNING, "Huh?  Can't smooth a non-voice frame!\n");
        return -1;
    }
    if (!s->format)
    {
        s->format = f->subclass;
        s->samplesperbyte = (float) f->samples/(float) f->datalen;
    }
    else if (s->format != f->subclass)
    {
        cw_log(LOG_WARNING, "Smoother was working on %d format frames, now trying to feed %d?\n", s->format, f->subclass);
        return -1;
    }
    if (s->len + f->datalen > SMOOTHER_SIZE)
    {
        cw_log(LOG_WARNING, "Out of smoother space\n");
        return -1;
    }
    if (((f->datalen == s->size) || ((f->datalen < 10) && (s->flags & CW_SMOOTHER_FLAG_G729)))
        &&
        !s->opt
        &&
        (f->offset >= CW_MIN_OFFSET))
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
    if (s->flags & CW_SMOOTHER_FLAG_G729)
    {
        if (s->len % 10)
        {
            cw_log(LOG_NOTICE, "Dropping extra frame of G.729 since we already have a VAD frame at the end\n");
            return 0;
        }
    }
    if (swap)
        cw_swapcopy_samples(s->data+s->len, f->data, f->samples);
    else
        memcpy(s->data + s->len, f->data, f->datalen);
    /* If either side is empty, reset the delivery time */
    if (!s->len || cw_tvzero(f->delivery) || cw_tvzero(s->delivery))    /* XXX really ? */
        s->delivery = f->delivery;
    s->len += f->datalen;
    return 0;
}

struct cw_frame *cw_smoother_read(struct cw_smoother *s)
{
    struct cw_frame *opt;
    int len;

    /* If we have an optimization frame, send it */
    if (s->opt)
    {
        if (s->opt->offset < CW_FRIENDLY_OFFSET)
        {
            cw_log(LOG_WARNING, "Returning a frame of inappropriate offset (%d).\n",
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
        if (!((s->flags & CW_SMOOTHER_FLAG_G729) && (s->size % 10)))
            return NULL;
    }
    len = s->size;
    if (len > s->len)
        len = s->len;
    /* Make frame */
    cw_fr_init_ex(&s->f, CW_FRAME_VOICE, s->format, NULL);
    s->f.data = s->framedata + CW_FRIENDLY_OFFSET;
    s->f.offset = CW_FRIENDLY_OFFSET;
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
           G.729 VAD, the next timestamp will take over anyway */
        memmove(s->data, s->data + len, s->len);
        if (!cw_tvzero(s->delivery))
        {
            /* If we have delivery time, increment it, otherwise, leave it at 0 */
            s->delivery = cw_tvadd(s->delivery, cw_samp2tv(s->f.samples, cw_codec_sample_rate(&s->f)));
        }
    }
    /* Return frame */
    return &s->f;
}

void cw_smoother_free(struct cw_smoother *s)
{
    free(s);
}

static struct cw_frame *cw_frame_header_new(void)
{
    struct cw_frame *f;

    if ((f = malloc(sizeof(struct cw_frame))))
        memset(f, 0, sizeof(struct cw_frame));
#ifdef TRACE_FRAMES
    if (f)
    {
        headers++;
        f->prev = NULL;
        cw_mutex_lock(&framelock);
        f->next = headerlist;
        if (headerlist)
            headerlist->prev = f;
        headerlist = f;
        cw_mutex_unlock(&framelock);
    }
#endif
    return f;
}

/*
 * Important: I should be made more efficient.  Frame headers should
 * most definitely be cached
 */

void cw_fr_init(struct cw_frame *fr)
{
    fr->frametype = CW_FRAME_NULL;
    fr->subclass = 0;
    fr->datalen = 0;
    fr->samples = 0;
    fr->samplerate = 8000;
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

void cw_fr_init_ex(struct cw_frame *fr,
                     int frame_type,
                     int sub_type,
                     const char *src)
{
    fr->frametype = frame_type;
    fr->subclass = sub_type;
    fr->datalen = 0;
    fr->samples = 0;
    fr->samplerate = 8000;
    fr->mallocd = 0;
    fr->offset = 0;
    fr->src = (src)  ?  src  :  "";
    fr->data = NULL;
    fr->delivery = cw_tv(0,0);
    fr->seq_no = 0;
    fr->prev = NULL;
    fr->next = NULL;
    fr->has_timing_info = 0;
    fr->ts = 0;
    fr->len = 0;
    fr->seq_no = 0;
    fr->tx_copies = 1;
}

void cw_fr_free(struct cw_frame *fr)
{
    if ((fr->mallocd & CW_MALLOCD_DATA))
    {
        if (fr->data)
            free(fr->data - fr->offset);
    }
    if ((fr->mallocd & CW_MALLOCD_SRC))
    {
        if (fr->src)
            free((char *) fr->src);
    }
    if ((fr->mallocd & CW_MALLOCD_HDR))
    {
#ifdef TRACE_FRAMES
        headers--;
        cw_mutex_lock(&framelock);
        if (fr->next)
            fr->next->prev = fr->prev;
        if (fr->prev)
            fr->prev->next = fr->next;
        else
            headerlist = fr->next;
        cw_mutex_unlock(&framelock);
#endif
        free(fr);
    }
}

/*
 * 'isolates' a frame by duplicating non-malloc'ed components
 * (header, src, data).
 * On return all components are malloc'ed
 */
struct cw_frame *cw_frisolate(struct cw_frame *fr)
{
    struct cw_frame *out;
    void *tmp;

    if (!(fr->mallocd & CW_MALLOCD_HDR))
    {
        /* Allocate a new header if needed */
        out = cw_frame_header_new();
        if (!out)
        {
            cw_log(LOG_WARNING, "Out of memory\n");
            return NULL;
        }
        cw_fr_init_ex(out, fr->frametype, fr->subclass, NULL);
        out->datalen = fr->datalen;
        out->samples = fr->samples;
        out->offset = fr->offset;
        out->data = fr->data;

        /* Copy the timing data */
        out->has_timing_info = fr->has_timing_info;
        if (fr->has_timing_info)
        {
            out->ts = fr->ts;
            out->len = fr->len;
            out->seq_no = fr->seq_no;
        }
    }
    else
    {
        out = fr;
    }
    if (!(fr->mallocd & CW_MALLOCD_SRC))
    {
        if (fr->src)
        {
            out->src = strdup(fr->src);
            if (!out->src)
            {
                if (out != fr)
                    free(out);
                cw_log(LOG_WARNING, "Out of memory\n");
                return NULL;
            }
        }
    }
    else
    {
        out->src = fr->src;
    }
    if (!(fr->mallocd & CW_MALLOCD_DATA))
    {
        tmp = fr->data;
        if ((out->data = malloc(fr->datalen + CW_FRIENDLY_OFFSET)) == NULL)
        {
            free(out);

            cw_log(LOG_WARNING, "Out of memory\n");
            return NULL;
        }
        out->data += CW_FRIENDLY_OFFSET;
        out->offset = CW_FRIENDLY_OFFSET;
        out->datalen = fr->datalen;
        memcpy(out->data, tmp, fr->datalen);
    }
    out->mallocd = CW_MALLOCD_HDR | CW_MALLOCD_SRC | CW_MALLOCD_DATA;
    return out;
}

struct cw_frame *cw_frdup(struct cw_frame *f)
{
    struct cw_frame *out;
    int len;
    int srclen;

    /* Its rather nasty if we are ever passed NULL, but lets try to be as
       benign as possible in how we traet this situation. */
    if (f == NULL)
        return NULL;
    srclen = 0;
    /* Start with standard stuff */
    len = sizeof(struct cw_frame) + CW_FRIENDLY_OFFSET + f->datalen;
    /* If we have a source, add space for it */
    /*
     * XXX Watch out here - if we receive a src which is not terminated
     * properly, we can easily be attacked. Should limit the size we deal with.
     */
    if (f->src)
        srclen = strlen(f->src);
    if (srclen > 0)
        len += srclen + 1;
    if ((out = (struct cw_frame *) malloc(len)) == NULL)
        return NULL;
    /* Set us as having malloc'd header only, so it will eventually
       get freed. */
    cw_fr_init_ex(out, f->frametype, f->subclass, NULL);
    out->datalen = f->datalen;
    out->samples = f->samples;
    out->delivery = f->delivery;
    out->mallocd = CW_MALLOCD_HDR;
    out->offset = CW_FRIENDLY_OFFSET;
    if (srclen > 0)
    {
        out->src = out->local_data + f->datalen;
        /* Must have space since we allocated for it */
        strcpy((char *) out->src, f->src);
    }
    else
    {
        out->src = NULL;
    }
    out->prev = NULL;
    out->next = NULL;
    if (f->data)
    {
        out->data = out->local_data;
        memcpy(out->data, f->data, out->datalen);
    }
    else
    {
        out->data = NULL;
    }
    out->has_timing_info = f->has_timing_info;
    if (f->has_timing_info)
    {
        out->ts = f->ts;
        out->len = f->len;
    }
    out->seq_no = f->seq_no;

    return out;
}

#if 0
/*
 * XXX
 * This function is badly broken - it does not handle correctly
 * partial reads on either header or body.
 * However is it never used anywhere so we leave it commented out
 */
struct cw_frame *cw_fr_fdread(int fd)
{
    char buf[65536];
    int res;
    int ttl = sizeof(struct cw_frame);
    struct cw_frame *f = (struct cw_frame *) buf;

    /* Read a frame directly from there.  They're always in the right format. */
    while (ttl)
    {
        res = read(fd, buf, ttl);
        if (res < 0)
        {
            cw_log(LOG_WARNING, "Bad read on %d: %s\n", fd, strerror(errno));
            return NULL;
        }
        ttl -= res;
    }

    /* read the frame header */
    f->mallocd = 0;
    /* Re-write data position */
    f->data = buf + sizeof(struct cw_frame);
    f->offset = 0;
    /* Forget about being mallocd */
    f->mallocd = 0;
    /* Re-write the source */
    f->src = (char *)__FUNCTION__;
    if (f->datalen > sizeof(buf) - sizeof(struct cw_frame))
    {
        /* Really bad read */
        cw_log(LOG_WARNING, "Strange read (%d bytes)\n", f->datalen);
        return NULL;
    }
    if (f->datalen)
    {
        if ((res = read(fd, f->data, f->datalen)) != f->datalen)
        {
            /* Bad read */
            cw_log(LOG_WARNING, "How very strange, expected %d, got %d\n", f->datalen, res);
            return NULL;
        }
    }
    if ((f->frametype == CW_FRAME_CONTROL) && (f->subclass == CW_CONTROL_HANGUP))
    {
        return NULL;
    }
    return cw_frisolate(f);
}

/* Some convenient routines for sending frames to/from stream or datagram
   sockets, pipes, etc (maybe even files) */

/*
 * XXX this function is also partly broken because it does not handle
 * partial writes. We comment it out too, and also the unique
 * client it has, cw_fr_fdhangup()
 */
int cw_fr_fdwrite(int fd, struct cw_frame *frame)
{
    /* Write the frame exactly */
    if (write(fd, frame, sizeof(struct cw_frame)) != sizeof(struct cw_frame))
    {
        cw_log(LOG_WARNING, "Write error: %s\n", strerror(errno));
        return -1;
    }
    if (write(fd, frame->data, frame->datalen) != frame->datalen)
    {
        cw_log(LOG_WARNING, "Write error: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int cw_fr_fdhangup(int fd)
{
    static const struct cw_frame hangup =
    {
        CW_FRAME_CONTROL,
        CW_CONTROL_HANGUP
    };
    return cw_fr_fdwrite(fd, &hangup);
}

#endif /* unused functions */

void cw_swapcopy_samples(void *dst, const void *src, int samples)
{
    int i;
    int16_t *dst_s = dst;
    const int16_t *src_s = src;

    for (i = 0;  i < samples;  i++)
        dst_s[i] = (src_s[i] << 8) | (src_s[i] >> 8);
}

static struct cw_format_list_s cw_format_list[] =
{
    { 1, CW_FORMAT_G723_1 , "g723" , "G.723.1", 8000},
    { 1, CW_FORMAT_GSM, "gsm" , "GSM", 8000},
    { 1, CW_FORMAT_ULAW, "ulaw", "G.711 u-law", 8000},
    { 1, CW_FORMAT_ALAW, "alaw", "G.711 A-law", 8000},
    { 1, CW_FORMAT_G726, "g726", "G.726", 8000},
    { 1, CW_FORMAT_DVI_ADPCM, "dvi" , "DVI-ADPCM", 8000},
    { 1, CW_FORMAT_SLINEAR, "slin",  "16 bit Signed Linear PCM", 8000},
    { 1, CW_FORMAT_LPC10, "lpc10", "LPC10", 8000},
    { 1, CW_FORMAT_G729A, "g729", "G.729A", 8000},
    { 1, CW_FORMAT_SPEEX, "speex", "SpeeX", 8000},
    { 1, CW_FORMAT_ILBC, "ilbc", "iLBC", 8000},
    { 1, CW_FORMAT_OKI_ADPCM, "oki", "OKI-ADPCM", 8000},
    { 1, CW_FORMAT_G722, "g722", "G.722", 16000},
    { 0, 0, "nothing", "undefined", 8000},
    { 0, 0, "nothing", "undefined", 8000},
    { 0, CW_FORMAT_MAX_AUDIO, "maxaudio", "Maximum audio format", 8000},
    { 1, CW_FORMAT_JPEG, "jpeg", "JPEG image", 90000},
    { 1, CW_FORMAT_PNG, "png", "PNG image", 90000},
    { 1, CW_FORMAT_H261, "h261", "H.261 Video", 90000},
    { 1, CW_FORMAT_H263, "h263", "H.263 Video", 90000},
    { 1, CW_FORMAT_H263_PLUS, "h263p", "H.263+ Video", 90000},
    { 1, CW_FORMAT_H264, "h264", "H.264 Video", 90000},      /*!< Passthrough support, see format_h263.c */
    { 0, 0, "nothing", "undefined", 90000},
    { 0, 0, "nothing", "undefined", 90000},
    { 0, 0, "nothing", "undefined", 90000},
    { 0, 0, "nothing", "undefined", 90000},
    { 0, CW_FORMAT_MAX_VIDEO, "maxvideo", "Maximum video format", 90000},
};

int cw_codec_sample_rate(struct cw_frame *f)
{
    int codec;

    if (f == NULL)
        return -1;
    if (f->frametype != CW_FRAME_VOICE)
        return -1;
    codec = f->subclass & CW_AUDIO_CODEC_MASK;
    if (codec == 0)
        return -1;
    return cw_format_list[top_bit(codec)].sample_rate;
}

#if 0
struct cw_format_list_s *cw_get_format_list_index(int index)
{
    return &cw_format_list[index];
}

struct cw_format_list_s *cw_get_format_list(size_t *size)
{
    *size = (sizeof(cw_format_list)/sizeof(struct cw_format_list_s));
    return cw_format_list;
}
#endif

char *cw_getformatname(int format)
{
    int x = 0;
    char *ret = "unknown";

    for (x = 0;  x < sizeof(cw_format_list)/sizeof(struct cw_format_list_s);  x++)
    {
        if (cw_format_list[x].visible  &&  cw_format_list[x].bits == format)
        {
            ret = cw_format_list[x].name;
            break;
        }
    }
    return ret;
}

char *cw_getformatname_multiple(char *buf, size_t size, int format)
{
    int x = 0;
    unsigned len;
    char *end = buf;
    char *start = buf;

    if (!size)
        return buf;
    snprintf(end, size, "0x%x (", format);
    len = strlen(end);
    end += len;
    size -= len;
    start = end;
    for (x = 0;  x < sizeof(cw_format_list)/sizeof(struct cw_format_list_s);  x++)
    {
        if (cw_format_list[x].visible && (cw_format_list[x].bits & format))
        {
            snprintf(end, size, "%s|", cw_format_list[x].name);
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

static struct cw_codec_alias_table
{
    char *alias;
    char *realname;
} cw_codec_alias_table[] =
{
    {"slinear", "slin"},
    {"g723.1", "g723"},
    {"g711u", "ulaw"},
    {"g711a", "alaw"},
};

static char *cw_expand_codec_alias(char *in)
{
    int x;

    for (x = 0;  x < sizeof(cw_codec_alias_table)/sizeof(struct cw_codec_alias_table);  x++)
    {
        if (!strcmp(in,cw_codec_alias_table[x].alias))
            return cw_codec_alias_table[x].realname;
    }
    return in;
}

int cw_getformatbyname(char *name)
{
    int x = 0;
    int all = 0;
    int format = 0;

    all = strcasecmp(name, "all")  ?  0  :  1;
    for (x = 0;  x < sizeof(cw_format_list)/sizeof(struct cw_format_list_s);  x++)
    {
        if (cw_format_list[x].visible
            &&
                (all
                ||
                !strcasecmp(cw_format_list[x].name,name)
                ||
                !strcasecmp(cw_format_list[x].name,cw_expand_codec_alias(name))))
        {
            format |= cw_format_list[x].bits;
            if (!all)
                break;
        }
    }

    return format;
}

char *cw_codec2str(int codec)
{
    int x = 0;
    char *ret = "unknown";

    for (x = 0;  x < sizeof(cw_format_list)/sizeof(struct cw_format_list_s);  x++)
    {
        if (cw_format_list[x].visible && cw_format_list[x].bits == codec)
        {
            ret = cw_format_list[x].desc;
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
        cw_cli(fd, "Disclaimer: this command is for informational purposes only.\n"
                 "\tIt does not indicate anything about your configuration.\n");

    cw_cli(fd, "%11s %9s %10s   TYPE   %5s   %s\n","INT","BINARY","HEX","NAME","DESC");
    cw_cli(fd, "--------------------------------------------------------------------------------\n");
    if ((argc == 2)  ||  (!strcasecmp(argv[1], "audio")))
    {
        found = 1;
        for (i = 0;  i <= 12;  i++)
        {
            snprintf(hex,25,"(0x%x)", 1 << i);
            cw_cli(fd, "%11u (1 << %2d) %10s  audio   %5s   (%s)\n", 1 << i, i, hex, cw_getformatname(1 << i), cw_codec2str(1 << i));
        }
    }

    if ((argc == 2)  ||  (!strcasecmp(argv[1], "image")))
    {
        found = 1;
        for (i = 16;  i < 18;  i++)
        {
            snprintf(hex, 25, "(0x%x)", 1 << i);
            cw_cli(fd, "%11u (1 << %2d) %10s  image   %5s   (%s)\n", 1 << i, i, hex,cw_getformatname(1 << i), cw_codec2str(1 << i));
        }
    }

    if ((argc == 2)  ||  (!strcasecmp(argv[1], "video")))
    {
        found = 1;
        for (i = 18;  i < 22;  i++)
        {
            snprintf(hex, 25, "(0x%x)", 1 << i);
            cw_cli(fd, "%11u (1 << %2d) %10s  video   %5s   (%s)\n", 1 << i, i, hex, cw_getformatname(1 << i), cw_codec2str(1 << i));
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
            cw_cli(fd, "%11u (1 << %2d)  %s\n", 1 << i, i, cw_codec2str(1 << i));
        }
    }
    if (!found)
        cw_cli(fd, "Codec %d not found\n", codec);

    return RESULT_SUCCESS;
}

static char frame_show_codec_n_usage[] =
    "Usage: show codec <number>\n"
    "       Displays codec mapping\n";

void cw_frame_dump(char *name, struct cw_frame *f, char *prefix)
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
    if (f == NULL)
    {
        cw_verbose("%s [ %s (NULL) ] [%s]\n",
                     cw_term_color(cp, prefix, COLOR_BRMAGENTA, COLOR_BLACK, sizeof(cp)),
                     cw_term_color(cft, "HANGUP", COLOR_BRRED, COLOR_BLACK, sizeof(cft)),
                     cw_term_color(cn, n, COLOR_YELLOW, COLOR_BLACK, sizeof(cn)));
        return;
    }
    /* XXX We should probably print one each of voice and video when the format changes XXX */
    if (f->frametype == CW_FRAME_VOICE)
        return;
    if (f->frametype == CW_FRAME_VIDEO)
        return;
    switch (f->frametype)
    {
    case CW_FRAME_DTMF:
        strcpy(ftype, "DTMF");
        subclass[0] = f->subclass;
        subclass[1] = '\0';
        break;
    case CW_FRAME_CONTROL:
        strcpy (ftype, "Control");
        switch(f->subclass)
        {
        case CW_CONTROL_HANGUP:
            strcpy(subclass, "Hangup");
            break;
        case CW_CONTROL_RING:
            strcpy(subclass, "Ring");
            break;
        case CW_CONTROL_RINGING:
            strcpy(subclass, "Ringing");
            break;
        case CW_CONTROL_ANSWER:
            strcpy(subclass, "Answer");
            break;
        case CW_CONTROL_BUSY:
            strcpy(subclass, "Busy");
            break;
        case CW_CONTROL_TAKEOFFHOOK:
            strcpy(subclass, "Take Off Hook");
            break;
        case CW_CONTROL_OFFHOOK:
            strcpy(subclass, "Line Off Hook");
            break;
        case CW_CONTROL_CONGESTION:
            strcpy(subclass, "Congestion");
            break;
        case CW_CONTROL_FLASH:
            strcpy(subclass, "Flash");
            break;
        case CW_CONTROL_WINK:
            strcpy(subclass, "Wink");
            break;
        case CW_CONTROL_OPTION:
            strcpy(subclass, "Option");
            break;
        case CW_CONTROL_RADIO_KEY:
            strcpy(subclass, "Key Radio");
            break;
        case CW_CONTROL_RADIO_UNKEY:
            strcpy(subclass, "Unkey Radio");
            break;
        case -1:
            strcpy(subclass, "Stop generators");
            break;
        default:
            snprintf(subclass, sizeof(subclass), "Unknown control '%d'", f->subclass);
        }
        break;
    case CW_FRAME_NULL:
        strcpy(ftype, "Null Frame");
        strcpy(subclass, "N/A");
        break;
    case CW_FRAME_IAX:
        /* Should never happen */
        strcpy(ftype, "IAX Specific");
        snprintf(subclass, sizeof(subclass), "IAX Frametype %d", f->subclass);
        break;
    case CW_FRAME_TEXT:
        strcpy(ftype, "Text");
        strcpy(subclass, "N/A");
        cw_copy_string(moreinfo, f->data, sizeof(moreinfo));
        break;
    case CW_FRAME_IMAGE:
        strcpy(ftype, "Image");
        snprintf(subclass, sizeof(subclass), "Image format %s\n", cw_getformatname(f->subclass));
        break;
    case CW_FRAME_HTML:
        strcpy(ftype, "HTML");
        switch (f->subclass)
        {
        case CW_HTML_URL:
            strcpy(subclass, "URL");
            cw_copy_string(moreinfo, f->data, sizeof(moreinfo));
            break;
        case CW_HTML_DATA:
            strcpy(subclass, "Data");
            break;
        case CW_HTML_BEGIN:
            strcpy(subclass, "Begin");
            break;
        case CW_HTML_END:
            strcpy(subclass, "End");
            break;
        case CW_HTML_LDCOMPLETE:
            strcpy(subclass, "Load Complete");
            break;
        case CW_HTML_NOSUPPORT:
            strcpy(subclass, "No Support");
            break;
        case CW_HTML_LINKURL:
            strcpy(subclass, "Link URL");
            cw_copy_string(moreinfo, f->data, sizeof(moreinfo));
            break;
        case CW_HTML_UNLINK:
            strcpy(subclass, "Unlink");
            break;
        case CW_HTML_LINKREJECT:
            strcpy(subclass, "Link Reject");
            break;
        default:
            snprintf(subclass, sizeof(subclass), "Unknown HTML frame '%d'\n", f->subclass);
            break;
        }
        break;
    default:
        snprintf(ftype, sizeof(ftype), "Unknown Frametype '%d'", f->frametype);
        break;
    }
    if (!cw_strlen_zero(moreinfo))
    {
        cw_verbose("%s [ TYPE: %s (%d) SUBCLASS: %s (%d) '%s' ] [%s]\n",
                     cw_term_color(cp, prefix, COLOR_BRMAGENTA, COLOR_BLACK, sizeof(cp)),
                     cw_term_color(cft, ftype, COLOR_BRRED, COLOR_BLACK, sizeof(cft)),
                     f->frametype,
                     cw_term_color(csub, subclass, COLOR_BRCYAN, COLOR_BLACK, sizeof(csub)),
                     f->subclass,
                     cw_term_color(cmn, moreinfo, COLOR_BRGREEN, COLOR_BLACK, sizeof(cmn)),
                     cw_term_color(cn, n, COLOR_YELLOW, COLOR_BLACK, sizeof(cn)));
    }
    else
    {
        cw_verbose("%s [ TYPE: %s (%d) SUBCLASS: %s (%d) ] [%s]\n",
                     cw_term_color(cp, prefix, COLOR_BRMAGENTA, COLOR_BLACK, sizeof(cp)),
                     cw_term_color(cft, ftype, COLOR_BRRED, COLOR_BLACK, sizeof(cft)),
                     f->frametype,
                     cw_term_color(csub, subclass, COLOR_BRCYAN, COLOR_BLACK, sizeof(csub)),
                     f->subclass,
                     cw_term_color(cn, n, COLOR_YELLOW, COLOR_BLACK, sizeof(cn)));
    }
}

#ifdef TRACE_FRAMES
static int show_frame_stats(int fd, int argc, char *argv[])
{
    struct cw_frame *f;
    int x = 1;

    if (argc != 3)
        return RESULT_SHOWUSAGE;
    cw_cli(fd, "     Framer Statistics     \n");
    cw_cli(fd, "---------------------------\n");
    cw_cli(fd, "Total allocated headers: %d\n", headers);
    cw_cli(fd, "Queue Dump:\n");
    cw_mutex_lock(&framelock);
    for (f = headerlist;  f;  f = f->next)
    {
        cw_cli(fd, "%d.  Type %d, subclass %d from %s\n", x++, f->frametype, f->subclass, f->src ? f->src : "<Unknown>");
    }
    cw_mutex_unlock(&framelock);
    return RESULT_SUCCESS;
}

static char frame_stats_usage[] =
    "Usage: show frame stats\n"
    "       Displays debugging statistics from framer\n";
#endif

/* XXX no unregister function here ??? */
static struct cw_cli_entry my_clis[] =
{
    {
        { "show", "codecs", NULL },
        show_codecs,
        "Shows codecs",
        frame_show_codecs_usage
    },
    {
        { "show", "audio", "codecs", NULL },
        show_codecs,
        "Shows audio codecs",
        frame_show_codecs_usage
    },
    {
        { "show", "video", "codecs", NULL },
        show_codecs,
        "Shows video codecs",
        frame_show_codecs_usage
    },
    {
        { "show", "image", "codecs", NULL },
        show_codecs,
        "Shows image codecs",
        frame_show_codecs_usage
    },
    {
        { "show", "codec", NULL },
        show_codec_n,
        "Shows a specific codec",
        frame_show_codec_n_usage
    },
#ifdef TRACE_FRAMES
    {
        { "show", "frame", "stats", NULL },
        show_frame_stats,
        "Shows frame statistics",
        frame_stats_usage
    },
#endif
};

int init_framer(void)
{
    cw_cli_register_multiple(my_clis, sizeof(my_clis)/sizeof(my_clis[0]));
    return 0;
}

void cw_codec_pref_convert(struct cw_codec_pref *pref, char *buf, size_t size, int right)
{
    int x = 0;
    int differential = (int) 'A';
    int mem = 0;
    char *from = NULL;
    char *to = NULL;

    if (right)
    {
        from = pref->order;
        to = buf;
        mem = size;
    }
    else
    {
        to = pref->order;
        from = buf;
        mem = 32;
    }

    memset(to, 0, mem);
    for (x = 0;  x < 32;  x++)
    {
        if (!from[x])
            break;
        to[x] = right ? (from[x] + differential) : (from[x] - differential);
    }
}

int cw_codec_pref_string(struct cw_codec_pref *pref, char *buf, size_t size)
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
        if (!(codec = cw_codec_pref_index(pref,x)))
            break;
        if ((formatname = cw_getformatname(codec)))
        {
            slen = strlen(formatname);
            if (slen > total_len)
                break;
            strncat(buf,formatname,total_len);
            total_len -= slen;
        }
        if (total_len  &&  x < 31  &&  cw_codec_pref_index(pref, x + 1))
        {
            strncat(buf, ",", total_len);
            total_len--;
        }
    }
    if (total_len)
    {
        strncat(buf, ")", total_len);
        total_len--;
    }

    return size - total_len;
}

int cw_codec_pref_index(struct cw_codec_pref *pref, int index)
{
    int slot = 0;

    if ((index >= 0)  &&  (index < sizeof(pref->order)))
        slot = pref->order[index];

    return slot  ?  cw_format_list[slot-1].bits  :  0;
}

/*--- cw_codec_pref_remove: Remove codec from pref list ---*/
void cw_codec_pref_remove(struct cw_codec_pref *pref, int format)
{
    struct cw_codec_pref oldorder;
    int x = 0;
    int y = 0;
    size_t size = 0;
    int slot = 0;

    if (!pref->order[0])
        return;

    size = sizeof(cw_format_list)/sizeof(struct cw_format_list_s);

    memcpy(&oldorder,pref,sizeof(struct cw_codec_pref));
    memset(pref,0,sizeof(struct cw_codec_pref));

    for (x = 0;  x < size;  x++)
    {
        slot = oldorder.order[x];
        if (! slot)
            break;
        if (cw_format_list[slot-1].bits != format)
            pref->order[y++] = slot;
    }
}

/*--- cw_codec_pref_append: Append codec to list ---*/
int cw_codec_pref_append(struct cw_codec_pref *pref, int format)
{
    size_t size = 0;
    int x = 0;
    int newindex = -1;

    cw_codec_pref_remove(pref, format);
    size = sizeof(cw_format_list)/sizeof(struct cw_format_list_s);

    for (x = 0;  x < size;  x++)
    {
        if (cw_format_list[x].bits == format)
        {
            newindex = x + 1;
            break;
        }
    }

    if (newindex)
    {
        for (x = 0;  x < size;  x++)
        {
            if (!pref->order[x])
            {
                pref->order[x] = newindex;
                break;
            }
        }
    }

    return x;
}

/*--- sip_codec_choose: Pick a codec ---*/
int cw_codec_choose(struct cw_codec_pref *pref, int formats, int find_best)
{
    size_t size = 0;
    int x = 0;
    int ret = 0;
    int slot = 0;

    size = sizeof(cw_format_list)/sizeof(struct cw_format_list_s);
    for (x = 0;  x < size;  x++)
    {
        slot = pref->order[x];

        if (!slot)
            break;
        if (formats  &  cw_format_list[slot - 1].bits)
        {
            ret = cw_format_list[slot - 1].bits;
            break;
        }
    }
    if (ret)
        return ret;

    return find_best ? cw_best_codec(formats) : 0;
}

void cw_parse_allow_disallow(struct cw_codec_pref *pref, int *mask, const char *list, int allowing)
{
    int format_i = 0;
    char *next_format = NULL;
    char *last_format = NULL;

    last_format = cw_strdupa(list);
    while (last_format)
    {
        if ((next_format = strchr(last_format, ',')))
        {
            *next_format = '\0';
            next_format++;
        }
        if ((format_i = cw_getformatbyname(last_format)) > 0)
        {
            if (mask)
            {
                if (allowing)
                    (*mask) |= format_i;
                else
                    (*mask) &= ~format_i;
            }
            /* can't consider 'all' a prefered codec*/
            if (pref  &&  strcasecmp(last_format, "all"))
            {
                if (allowing)
                    cw_codec_pref_append(pref, format_i);
                else
                    cw_codec_pref_remove(pref, format_i);
            }
            else if (!allowing) /* disallow all must clear your prefs or it makes no sense */
            {
                memset(pref, 0, sizeof(struct cw_codec_pref));
            }
        }
        else
        {
            cw_log(LOG_WARNING, "Cannot %s unknown format '%s'\n", allowing ? "allow" : "disallow", last_format);
        }
        last_format = next_format;
    }
}

static int g723_len(unsigned char buf)
{
    switch (buf & TYPE_MASK)
    {
    case TYPE_DONTSEND:
        return 0;
    case TYPE_SILENCE:
        return 4;
    case TYPE_HIGH:
        return 24;
    case TYPE_LOW:
        return 20;
    default:
        cw_log(LOG_WARNING, "Badly encoded frame (%d)\n", buf & TYPE_MASK);
        break;
    }
    return -1;
}

static int g723_samples(unsigned char *buf, int maxlen)
{
    int pos = 0;
    int samples = 0;
    int res;

    while (pos < maxlen)
    {
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
    int byte = bit/8;       /* byte containing first bit */
    int rem = 8 - (bit%8);  /* remaining bits in first byte */
    unsigned char ret = 0;

    if (n <= 0  ||  n > 8)
        return 0;

    if (rem < n)
    {
        ret = (data[byte] << (n - rem));
        ret |= (data[byte + 1] >> (8 - n + rem));
    }
    else
    {
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

    /* Skip up to two wideband frames */
    if (((len * 8 - off) >= 5)  &&  get_n_bits_at(data, 1, off))
    {
        c = get_n_bits_at(data, 3, off + 1);
        off += SpeexWBSubModeSz[c];

        if (((len * 8 - off) >= 5)  &&  get_n_bits_at(data, 1, off))
        {
            c = get_n_bits_at(data, 3, off + 1);
            off += SpeexWBSubModeSz[c];

            if (((len * 8 - off) >= 5)  &&  get_n_bits_at(data, 1, off))
            {
                cw_log(LOG_WARNING, "Encountered corrupt speex frame; too many wideband frames in a row.\n");
                return -1;
            }
        }
    }
    return off - bit;
}

static int speex_samples(unsigned char *data, int len)
{
    static int SpeexSubModeSz[] =
    {
        5, 43, 119, 160,
        220, 300, 364, 492,
        79, 0, 0, 0,
        0, 0, 0, 0
    };
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
            cw_log(LOG_WARNING, "Had error while reading wideband frames for speex samples\n");
            break;
        }
        bit += off;

        if ((len * 8 - bit) < 5)
        {
            cw_log(LOG_WARNING, "Not enough bits remaining after wide band for speex samples.\n");
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
        
        if (c == 14)
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

int cw_codec_get_samples(struct cw_frame *f)
{
    int samples = 0;

    switch (f->subclass)
    {
    case CW_FORMAT_SPEEX:
        samples = speex_samples(f->data, f->datalen);
        break;
    case CW_FORMAT_G723_1:
        samples = g723_samples(f->data, f->datalen);
        break;
    case CW_FORMAT_ILBC:
        samples = 240 * (f->datalen / 50);
        break;
    case CW_FORMAT_GSM:
        samples = 160 * (f->datalen / 33);
        break;
    case CW_FORMAT_G729A:
        samples = f->datalen * 8;
        break;
    case CW_FORMAT_SLINEAR:
        samples = f->datalen / 2;
        break;
    case CW_FORMAT_LPC10:
        /* assumes that the RTP packet contains one LPC10 frame */
        samples = 22 * 8;
        samples += (((char *)(f->data))[7] & 0x1) * 8;
        break;
    case CW_FORMAT_ULAW:
    case CW_FORMAT_ALAW:
        samples = f->datalen;
        break;
    case CW_FORMAT_DVI_ADPCM:
    case CW_FORMAT_G726:
        samples = f->datalen * 2;
        break;
    default:
        cw_log(LOG_WARNING, "Unable to calculate samples for format %s\n", cw_getformatname(f->subclass));
        break;
    }
    return samples;
}

int cw_codec_get_len(int format, int samples)
{
    int len = 0;

    /* XXX Still need speex, g723, and lpc10 XXX */
    switch (format)
    {
    case CW_FORMAT_ILBC:
        len = (samples/240)*50;
        break;
    case CW_FORMAT_GSM:
        len = (samples/160)*33;
        break;
    case CW_FORMAT_G729A:
        len = samples/8;
        break;
    case CW_FORMAT_SLINEAR:
        len = samples*sizeof(int16_t);
        break;
    case CW_FORMAT_ULAW:
    case CW_FORMAT_ALAW:
        len = samples;
        break;
    case CW_FORMAT_DVI_ADPCM:
    case CW_FORMAT_G726:
        len = samples/2;
        break;
    default:
        cw_log(LOG_WARNING, "Unable to calculate sample length for format %s\n", cw_getformatname(format));
        break;
    }

    return len;
}

int cw_frame_adjust_volume(struct cw_frame *f, int adjustment)
{
    int count;
    int16_t *fdata = f->data;
    int16_t adjust_value;

    if ((f->frametype != CW_FRAME_VOICE)  ||  (f->subclass != CW_FORMAT_SLINEAR))
        return -1;

    if (adjustment == 0)
        return 0;

    if (adjustment > 0)
        adjust_value = adjustment << 11;
    else
        adjust_value = (1 << 11)/(-adjustment);
    
    for (count = 0;  count < f->samples;  count++)
        fdata[count] = saturate(((int32_t) fdata[count]*(int32_t) adjust_value) >> 11);

    return 0;
}

int cw_frame_slinear_sum(struct cw_frame *f1, struct cw_frame *f2)
{
    int count;
    int16_t *data1;
    int16_t *data2;

    if ((f1->frametype != CW_FRAME_VOICE)  ||  (f1->subclass != CW_FORMAT_SLINEAR))
        return -1;

    if ((f2->frametype != CW_FRAME_VOICE)  ||  (f2->subclass != CW_FORMAT_SLINEAR))
        return -1;

    if (f1->samples != f2->samples)
        return -1;

    for (data1 = (int16_t *) f1->data, data2 = (int16_t *) f2->data, count = 0;  count < f1->samples;  count++)
        data1[count] = saturate((int) data1[count] + (int) data2[count]);
    return 0;
}
