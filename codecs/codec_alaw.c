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
 * \brief codec_alaw.c - translate between signed linear and alaw
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <spandsp.h>

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/codecs/codec_alaw.c $", "$Revision: 4723 $")

#include "callweaver/lock.h"
#include "callweaver/logger.h"
#include "callweaver/module.h"
#include "callweaver/config.h"
#include "callweaver/options.h"
#include "callweaver/translate.h"
#include "callweaver/channel.h"
#include "callweaver/alaw.h"

#define BUFFER_SIZE   8096    /* size for the translation buffers */

CW_MUTEX_DEFINE_STATIC(localuser_lock);
static int localusecnt = 0;

static char *tdesc = "A-law to/from PCM16 translator";

static int useplc = 0;

/* Sample frame data (re-using the Mu data is okay) */

/* Sample 10ms of linear frame data */
static int16_t slin_ulaw_ex[] =
{
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000
};

/* Sample 10ms of alaw frame data */
static uint8_t alaw_slin_ex[] =
{
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/*!
 * \brief Private workspace for translating signed linear signals to alaw.
 */
struct alaw_encoder_pvt
{
    struct cw_frame f;
    char offset[CW_FRIENDLY_OFFSET];   /*!< Space to build offset */
    unsigned char outbuf[BUFFER_SIZE];  /*!< Encoded alaw, two nibbles to a word */
    int tail;
};

/*!
 * \brief Private workspace for translating alaw signals to signed linear.
 */
struct alaw_decoder_pvt
{
    struct cw_frame f;
    char offset[CW_FRIENDLY_OFFSET];    /*!< Space to build offset */
    int16_t outbuf[BUFFER_SIZE];    /*!< Decoded signed linear values */
    int tail;
    plc_state_t plc;
};

/*!
 * \brief alawtolin_new
 *  Create a new instance of alaw_decoder_pvt.
 *
 * Results:
 *  Returns a pointer to the new instance.
 *
 * Side effects:
 *  None.
 */
static struct cw_translator_pvt *alawtolin_new(void)
{
    struct alaw_decoder_pvt *tmp;
  
    if ((tmp = malloc(sizeof(struct alaw_decoder_pvt))) == NULL)
        return NULL;
    memset(tmp, 0, sizeof(*tmp));
    tmp->tail = 0;
    plc_init(&tmp->plc);
    localusecnt++;
    cw_update_use_count();
    return (struct cw_translator_pvt *) tmp;
}

/*!
 * \brief lintoalaw_new
 *  Create a new instance of alaw_encoder_pvt.
 *
 * Results:
 *  Returns a pointer to the new instance.
 *
 * Side effects:
 *  None.
 */
static struct cw_translator_pvt *lintoalaw_new(void)
{
    struct alaw_encoder_pvt *tmp;
  
    if ((tmp = malloc(sizeof(struct alaw_encoder_pvt))) == NULL)
        return NULL;
    memset(tmp, 0, sizeof(*tmp));
    localusecnt++;
    cw_update_use_count();
    return (struct cw_translator_pvt *) tmp;
}

/*!
 * \brief alawtolin_framein
 *  Fill an input buffer with packed 4-bit alaw values if there is room
 *  left.
 *
 * Results:
 *  Foo
 *
 * Side effects:
 *  tmp->tail is the number of packed values in the buffer.
 */
static int alawtolin_framein(struct cw_translator_pvt *pvt, struct cw_frame *f)
{
    struct alaw_decoder_pvt *tmp = (struct alaw_decoder_pvt *) pvt;
    int x;
    unsigned char *b;

    if (f->datalen == 0) {
        /* perform PLC with nominal framesize of 20ms/160 samples */
        if ((tmp->tail + 160)*sizeof(int16_t) > sizeof(tmp->outbuf)) {
            cw_log(LOG_WARNING, "Out of buffer space\n");
            return -1;
        }
        if (useplc) {
            plc_fillin(&tmp->plc, tmp->outbuf+tmp->tail, 160);
            tmp->tail += 160;
        }
        return 0;
    }

    if ((tmp->tail + f->datalen)*sizeof(int16_t) > sizeof(tmp->outbuf)) {
        cw_log(LOG_WARNING, "Out of buffer space\n");
        return -1;
    }

    /* Reset ssindex and signal to frame's specified values */
    b = f->data;
    for (x = 0;  x < f->datalen;  x++)
        tmp->outbuf[tmp->tail + x] = CW_ALAW(b[x]);

    if (useplc)
        plc_rx(&tmp->plc, tmp->outbuf+tmp->tail, f->datalen);

    tmp->tail += f->datalen;
    return 0;
}

/*!
 * \brief alawtolin_frameout
 *  Convert 4-bit alaw encoded signals to 16-bit signed linear.
 *
 * Results:
 *  Converted signals are placed in tmp->f.data, tmp->f.datalen
 *  and tmp->f.samples are calculated.
 *
 * Side effects:
 *  None.
 */
static struct cw_frame *alawtolin_frameout(struct cw_translator_pvt *pvt)
{
    struct alaw_decoder_pvt *tmp = (struct alaw_decoder_pvt *) pvt;

    if (tmp->tail == 0)
        return NULL;

    cw_fr_init_ex(&tmp->f, CW_FRAME_VOICE, CW_FORMAT_SLINEAR, __PRETTY_FUNCTION__);
    tmp->f.datalen = tmp->tail*sizeof(int16_t);
    tmp->f.samples = tmp->tail;
    tmp->f.offset = CW_FRIENDLY_OFFSET;
    tmp->f.data = tmp->outbuf;

    tmp->tail = 0;
    return &tmp->f;
}

/*!
 * \brief lintoalaw_framein
 *  Fill an input buffer with 16-bit signed linear PCM values.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  tmp->tail is number of signal values in the input buffer.
 */
static int lintoalaw_framein(struct cw_translator_pvt *pvt, struct cw_frame *f)
{
    struct alaw_encoder_pvt *tmp = (struct alaw_encoder_pvt *) pvt;
    int x;
    int16_t *s;
  
    if (tmp->tail + f->datalen/sizeof(int16_t) >= sizeof(tmp->outbuf))
    {
        cw_log (LOG_WARNING, "Out of buffer space\n");
        return -1;
    }
    s = f->data;
    for (x = 0;  x < f->datalen/sizeof(int16_t);  x++) 
        tmp->outbuf[x + tmp->tail] = CW_LIN2A(s[x]);
    tmp->tail += f->datalen/sizeof(int16_t);
    return 0;
}

/*!
 * \brief lintoalaw_frameout
 *  Convert a buffer of raw 16-bit signed linear PCM to a buffer
 *  of 4-bit alaw packed two to a byte (Big Endian).
 *
 * Results:
 *  Foo
 *
 * Side effects:
 *  Leftover inbuf data gets packed, tail gets updated.
 */
static struct cw_frame *lintoalaw_frameout(struct cw_translator_pvt *pvt)
{
    struct alaw_encoder_pvt *tmp = (struct alaw_encoder_pvt *) pvt;
  
    if (tmp->tail == 0)
        return NULL;

    cw_fr_init_ex(&tmp->f, CW_FRAME_VOICE, CW_FORMAT_ALAW, __PRETTY_FUNCTION__);
    tmp->f.samples = tmp->tail;
    tmp->f.offset = CW_FRIENDLY_OFFSET;
    tmp->f.data = tmp->outbuf;
    tmp->f.datalen = tmp->tail;

    tmp->tail = 0;
    return &tmp->f;
}

/*!
 * \brief alawtolin_sample
 */
static struct cw_frame *alawtolin_sample(void)
{
    static struct cw_frame f;
  
    cw_fr_init_ex(&f, CW_FRAME_VOICE, CW_FORMAT_ALAW, __PRETTY_FUNCTION__);
    f.datalen = sizeof(alaw_slin_ex);
    f.samples = sizeof(alaw_slin_ex);
    f.data = alaw_slin_ex;
    return &f;
}

/*!
 * \brief lintoalaw_sample
 */
static struct cw_frame *lintoalaw_sample(void)
{
    static struct cw_frame f;
  
    cw_fr_init_ex(&f, CW_FRAME_VOICE, CW_FORMAT_SLINEAR, __PRETTY_FUNCTION__);
    f.datalen = sizeof(slin_ulaw_ex);
    /* Assume 8000 Hz */
    f.samples = sizeof(slin_ulaw_ex)/sizeof(int16_t);
    f.data = slin_ulaw_ex;
    return &f;
}

/*!
 * \brief alaw_destroy
 *  Destroys a private workspace.
 *
 * Results:
 *  It's gone!
 *
 * Side effects:
 *  None.
 */
static void alaw_destroy(struct cw_translator_pvt *pvt)
{
    free(pvt);
    localusecnt--;
    cw_update_use_count();
}

/*!
 * \brief The complete translator for alawtolin.
 */
static struct cw_translator alawtolin =
{
    "alawtolin",
    CW_FORMAT_ALAW,
    8000,
    CW_FORMAT_SLINEAR,
    8000,
    alawtolin_new,
    alawtolin_framein,
    alawtolin_frameout,
    alaw_destroy,
    alawtolin_sample
};

/*!
 * \brief The complete translator for lintoalaw.
 */
static struct cw_translator lintoalaw =
{
    "lintoalaw",
    CW_FORMAT_SLINEAR,
    8000,
    CW_FORMAT_ALAW,
    8000,
    lintoalaw_new,
    lintoalaw_framein,
    lintoalaw_frameout,
    alaw_destroy,
    lintoalaw_sample
};

static void parse_config(void)
{
    struct cw_config *cfg;
    struct cw_variable *var;

    if ((cfg = cw_config_load("codecs.conf")))
    {
        if ((var = cw_variable_browse(cfg, "plc")))
        {
            while (var)
            {
                if (!strcasecmp(var->name, "genericplc"))
                {
                    useplc = cw_true(var->value)  ?  1  :  0;
                    if (option_verbose > 2)
                        cw_verbose(VERBOSE_PREFIX_3 "codec_alaw: %susing generic PLC\n", useplc  ?  ""  :  "not ");
                }
                var = var->next;
            }
        }
        cw_config_destroy(cfg);
    }
}

int reload(void)
{
    parse_config();
    return 0;
}

int unload_module(void)
{
    int res;

    cw_mutex_lock(&localuser_lock);
    res = cw_unregister_translator(&lintoalaw);
    if (!res)
        res = cw_unregister_translator(&alawtolin);
    if (localusecnt)
        res = -1;
    cw_mutex_unlock(&localuser_lock);
    return res;
}

int load_module(void)
{
    int res;

    parse_config();
    res = cw_register_translator(&alawtolin);
    if (!res)
        res = cw_register_translator(&lintoalaw);
    else
        cw_unregister_translator(&alawtolin);
    return res;
}

/*
 * Return a description of this module.
 */
char *description(void)
{
    return tdesc;
}

int usecount(void)
{
    int res;
  
    STANDARD_USECOUNT(res);
    return res;
}
