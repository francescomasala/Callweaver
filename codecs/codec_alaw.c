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

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/lock.h"
#include "openpbx/logger.h"
#include "openpbx/module.h"
#include "openpbx/config.h"
#include "openpbx/options.h"
#include "openpbx/translate.h"
#include "openpbx/channel.h"
#include "openpbx/alaw.h"

#define BUFFER_SIZE   8096    /* size for the translation buffers */

OPBX_MUTEX_DEFINE_STATIC(localuser_lock);
static int localusecnt = 0;

static char *tdesc = "A-law Coder/Decoder";

static int useplc = 0;

/* Sample frame data (re-using the Mu data is okay) */

#include "slin_ulaw_ex.h"
#include "ulaw_slin_ex.h"

/*!
 * \brief Private workspace for translating signed linear signals to alaw.
 */
struct alaw_encoder_pvt
{
    struct opbx_frame f;
    char offset[OPBX_FRIENDLY_OFFSET];   /*!< Space to build offset */
    unsigned char outbuf[BUFFER_SIZE];  /*!< Encoded alaw, two nibbles to a word */
    int tail;
};

/*!
 * \brief Private workspace for translating alaw signals to signed linear.
 */
struct alaw_decoder_pvt
{
    struct opbx_frame f;
    char offset[OPBX_FRIENDLY_OFFSET];    /*!< Space to build offset */
    short outbuf[BUFFER_SIZE];    /*!< Decoded signed linear values */
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
static struct opbx_translator_pvt *alawtolin_new(void)
{
    struct alaw_decoder_pvt *tmp;
  
    if ((tmp = malloc(sizeof(struct alaw_decoder_pvt))))
    {
        memset(tmp, 0, sizeof(*tmp));
        tmp->tail = 0;
        plc_init(&tmp->plc);
        localusecnt++;
        opbx_update_use_count();
    }
    return (struct opbx_translator_pvt *) tmp;
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
static struct opbx_translator_pvt *lintoalaw_new(void)
{
    struct alaw_encoder_pvt *tmp;
  
    if ((tmp = malloc(sizeof(struct alaw_encoder_pvt))))
    {
        memset(tmp, 0, sizeof(*tmp));
        localusecnt++;
        opbx_update_use_count();
        tmp->tail = 0;
    }
    return (struct opbx_translator_pvt *) tmp;
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
static int alawtolin_framein(struct opbx_translator_pvt *pvt, struct opbx_frame *f)
{
    struct alaw_decoder_pvt *tmp = (struct alaw_decoder_pvt *) pvt;
    int x;
    unsigned char *b;

    if (f->datalen == 0) {
        /* perform PLC with nominal framesize of 20ms/160 samples */
        if ((tmp->tail + 160)*sizeof(int16_t) > sizeof(tmp->outbuf)) {
            opbx_log(LOG_WARNING, "Out of buffer space\n");
            return -1;
        }
        if (useplc) {
            plc_fillin(&tmp->plc, tmp->outbuf+tmp->tail, 160);
            tmp->tail += 160;
        }
        return 0;
    }

    if ((tmp->tail + f->datalen)*sizeof(int16_t) > sizeof(tmp->outbuf)) {
        opbx_log(LOG_WARNING, "Out of buffer space\n");
        return -1;
    }

    /* Reset ssindex and signal to frame's specified values */
    b = f->data;
    for (x = 0;  x < f->datalen;  x++)
        tmp->outbuf[tmp->tail + x] = OPBX_ALAW(b[x]);

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
static struct opbx_frame *alawtolin_frameout(struct opbx_translator_pvt *pvt)
{
    struct alaw_decoder_pvt *tmp = (struct alaw_decoder_pvt *) pvt;

    if (!tmp->tail)
        return NULL;

    tmp->f.frametype = OPBX_FRAME_VOICE;
    tmp->f.subclass = OPBX_FORMAT_SLINEAR;
    tmp->f.datalen = tmp->tail*sizeof(int16_t);
    tmp->f.samples = tmp->tail;
    tmp->f.mallocd = 0;
    tmp->f.offset = OPBX_FRIENDLY_OFFSET;
    tmp->f.src = __PRETTY_FUNCTION__;
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
static int lintoalaw_framein(struct opbx_translator_pvt *pvt, struct opbx_frame *f)
{
    struct alaw_encoder_pvt *tmp = (struct alaw_encoder_pvt *) pvt;
    int x;
    short *s;
  
    if (tmp->tail + f->datalen/sizeof(int16_t) >= sizeof(tmp->outbuf))
    {
        opbx_log (LOG_WARNING, "Out of buffer space\n");
        return -1;
    }
    s = f->data;
    for (x = 0;  x < f->datalen/sizeof(int16_t);  x++) 
        tmp->outbuf[x + tmp->tail] = OPBX_LIN2A(s[x]);
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
static struct opbx_frame *lintoalaw_frameout(struct opbx_translator_pvt *pvt)
{
    struct alaw_encoder_pvt *tmp = (struct alaw_encoder_pvt *) pvt;
  
    if (tmp->tail) {
        tmp->f.frametype = OPBX_FRAME_VOICE;
        tmp->f.subclass = OPBX_FORMAT_ALAW;
        tmp->f.samples = tmp->tail;
        tmp->f.mallocd = 0;
        tmp->f.offset = OPBX_FRIENDLY_OFFSET;
        tmp->f.src = __PRETTY_FUNCTION__;
        tmp->f.data = tmp->outbuf;
        tmp->f.datalen = tmp->tail;
        tmp->tail = 0;
        return &tmp->f;
    }
    return NULL;
}

/*!
 * \brief alawtolin_sample
 */
static struct opbx_frame *alawtolin_sample(void)
{
    static struct opbx_frame f;
  
    f.frametype = OPBX_FRAME_VOICE;
    f.subclass = OPBX_FORMAT_ALAW;
    f.datalen = sizeof (ulaw_slin_ex);
    f.samples = sizeof(ulaw_slin_ex);
    f.mallocd = 0;
    f.offset = 0;
    f.src = __PRETTY_FUNCTION__;
    f.data = ulaw_slin_ex;
    return &f;
}

/*!
 * \brief lintoalaw_sample
 */
static struct opbx_frame *lintoalaw_sample(void)
{
    static struct opbx_frame f;
  
    f.frametype = OPBX_FRAME_VOICE;
    f.subclass = OPBX_FORMAT_SLINEAR;
    f.datalen = sizeof(slin_ulaw_ex);
    /* Assume 8000 Hz */
    f.samples = sizeof(slin_ulaw_ex)/sizeof(int16_t);
    f.mallocd = 0;
    f.offset = 0;
    f.src = __PRETTY_FUNCTION__;
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
static void alaw_destroy(struct opbx_translator_pvt *pvt)
{
    free(pvt);
    localusecnt--;
    opbx_update_use_count();
}

/*!
 * \brief The complete translator for alawtolin.
 */
static struct opbx_translator alawtolin = {
    "alawtolin",
    OPBX_FORMAT_ALAW,
    8000,
    OPBX_FORMAT_SLINEAR,
    8000,
    alawtolin_new,
    alawtolin_framein,
    alawtolin_frameout,
    alaw_destroy,
    /* NULL */
    alawtolin_sample
};

/*!
 * \brief The complete translator for lintoalaw.
 */
static struct opbx_translator lintoalaw = {
    "lintoalaw",
    OPBX_FORMAT_SLINEAR,
    8000,
    OPBX_FORMAT_ALAW,
    8000,
    lintoalaw_new,
    lintoalaw_framein,
    lintoalaw_frameout,
    alaw_destroy,
    /* NULL */
    lintoalaw_sample
};

static void parse_config(void)
{
    struct opbx_config *cfg;
    struct opbx_variable *var;

    if ((cfg = opbx_config_load("codecs.conf"))) {
        if ((var = opbx_variable_browse(cfg, "plc"))) {
            while (var) {
                if (!strcasecmp(var->name, "genericplc")) {
                    useplc = opbx_true(var->value) ? 1 : 0;
                    if (option_verbose > 2)
                        opbx_verbose(VERBOSE_PREFIX_3 "codec_alaw: %susing generic PLC\n", useplc  ?  ""  :  "not ");
                }
                var = var->next;
            }
        }
        opbx_config_destroy(cfg);
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

    opbx_mutex_lock(&localuser_lock);
    res = opbx_unregister_translator(&lintoalaw);
    if (!res)
        res = opbx_unregister_translator(&alawtolin);
    if (localusecnt)
        res = -1;
    opbx_mutex_unlock(&localuser_lock);
    return res;
}

int load_module(void)
{
    int res;

    parse_config();
    res = opbx_register_translator(&alawtolin);
    if (!res)
        res = opbx_register_translator(&lintoalaw);
    else
        opbx_unregister_translator(&alawtolin);
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
