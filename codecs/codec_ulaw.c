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
 * \brief codec_ulaw.c - translate between signed linear and ulaw
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
#include "openpbx/ulaw.h"

#define BUFFER_SIZE   8096    /* size for the translation buffers */

OPBX_MUTEX_DEFINE_STATIC(localuser_lock);
static int localusecnt = 0;

static char *tdesc = "Mu-law Coder/Decoder";

static int useplc = 0;

/* Sample frame data */

#include "slin_ulaw_ex.h"
#include "ulaw_slin_ex.h"

/*!
 * \brief Private workspace for translating signed linear signals to ulaw.
 */
struct ulaw_encoder_pvt
{
    struct opbx_frame f;
    char offset[OPBX_FRIENDLY_OFFSET];   /*!< Space to build offset */
    unsigned char outbuf[BUFFER_SIZE];  /*!< Encoded ulaw, two nibbles to a word */
    int tail;
};

/*!
 * \brief Private workspace for translating ulaw signals to signed linear.
 */
struct ulaw_decoder_pvt
{
    struct opbx_frame f;
    char offset[OPBX_FRIENDLY_OFFSET];    /*!< Space to build offset */
    short outbuf[BUFFER_SIZE];    /*!< Decoded signed linear values */
    int tail;
    plc_state_t plc;
};

/*!
 * \brief ulawtolin_new
 *  Create a new instance of ulaw_decoder_pvt.
 *
 * Results:
 *  Returns a pointer to the new instance.
 *
 * Side effects:
 *  None.
 */

static struct opbx_translator_pvt *ulawtolin_new(void)
{
    struct ulaw_decoder_pvt *tmp;
  
    if ((tmp = malloc(sizeof(struct ulaw_decoder_pvt))))
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
 * \brief lintoulaw_new
 *  Create a new instance of ulaw_encoder_pvt.
 *
 * Results:
 *  Returns a pointer to the new instance.
 *
 * Side effects:
 *  None.
 */
static struct opbx_translator_pvt *lintoulaw_new(void)
{
    struct ulaw_encoder_pvt *tmp;
  
    if ((tmp = malloc(sizeof(struct ulaw_encoder_pvt))))
    {
        memset(tmp, 0, sizeof(*tmp));
        localusecnt++;
        opbx_update_use_count();
        tmp->tail = 0;
    }
    return (struct opbx_translator_pvt *) tmp;
}

/*!
 * \brief ulawtolin_framein
 *  Fill an input buffer with packed 4-bit ulaw values if there is room
 *  left.
 *
 * Results:
 *  Foo
 *
 * Side effects:
 *  tmp->tail is the number of packed values in the buffer.
 */
static int ulawtolin_framein(struct opbx_translator_pvt *pvt, struct opbx_frame *f)
{
    struct ulaw_decoder_pvt *tmp = (struct ulaw_decoder_pvt *) pvt;
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
        tmp->outbuf[tmp->tail + x] = OPBX_MULAW(b[x]);

    if (useplc)
        plc_rx(&tmp->plc, tmp->outbuf+tmp->tail, f->datalen);

    tmp->tail += f->datalen;
    return 0;
}

/*!
 * \brief ulawtolin_frameout
 *  Convert 4-bit ulaw encoded signals to 16-bit signed linear.
 *
 * Results:
 *  Converted signals are placed in tmp->f.data, tmp->f.datalen
 *  and tmp->f.samples are calculated.
 *
 * Side effects:
 *  None.
 */
static struct opbx_frame *ulawtolin_frameout(struct opbx_translator_pvt *pvt)
{
    struct ulaw_decoder_pvt *tmp = (struct ulaw_decoder_pvt *) pvt;

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
 * \brief lintoulaw_framein
 *  Fill an input buffer with 16-bit signed linear PCM values.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  tmp->tail is number of signal values in the input buffer.
 */
static int lintoulaw_framein(struct opbx_translator_pvt *pvt, struct opbx_frame *f)
{
    struct ulaw_encoder_pvt *tmp = (struct ulaw_encoder_pvt *) pvt;
    int x;
    short *s;
  
    if (tmp->tail + f->datalen/sizeof(int16_t) >= sizeof(tmp->outbuf))
    {
        opbx_log (LOG_WARNING, "Out of buffer space\n");
        return -1;
    }
    s = f->data;
    for (x = 0;  x < f->datalen/sizeof(int16_t);  x++) 
        tmp->outbuf[x + tmp->tail] = OPBX_LIN2MU(s[x]);
    tmp->tail += f->datalen/sizeof(int16_t);
    return 0;
}

/*!
 * \brief lintoulaw_frameout
 *  Convert a buffer of raw 16-bit signed linear PCM to a buffer
 *  of 4-bit ulaw packed two to a byte (Big Endian).
 *
 * Results:
 *  Foo
 *
 * Side effects:
 *  Leftover inbuf data gets packed, tail gets updated.
 */
static struct opbx_frame *lintoulaw_frameout(struct opbx_translator_pvt *pvt)
{
    struct ulaw_encoder_pvt *tmp = (struct ulaw_encoder_pvt *) pvt;
  
    if (tmp->tail) {
        tmp->f.frametype = OPBX_FRAME_VOICE;
        tmp->f.subclass = OPBX_FORMAT_ULAW;
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
 * \brief ulawtolin_sample
 */
static struct opbx_frame *ulawtolin_sample(void)
{
    static struct opbx_frame f;
  
    f.frametype = OPBX_FRAME_VOICE;
    f.subclass = OPBX_FORMAT_ULAW;
    f.datalen = sizeof (ulaw_slin_ex);
    f.samples = sizeof(ulaw_slin_ex);
    f.mallocd = 0;
    f.offset = 0;
    f.src = __PRETTY_FUNCTION__;
    f.data = ulaw_slin_ex;
    return &f;
}

/*!
 * \brief lintoulaw_sample
 */
static struct opbx_frame *lintoulaw_sample(void)
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
 * \brief ulaw_destroy
 *  Destroys a private workspace.
 *
 * Results:
 *  It's gone!
 *
 * Side effects:
 *  None.
 */
static void ulaw_destroy(struct opbx_translator_pvt *pvt)
{
    free(pvt);
    localusecnt--;
    opbx_update_use_count();
}

/*!
 * \brief The complete translator for ulawtolin.
 */
static struct opbx_translator ulawtolin = {
    "ulawtolin",
    OPBX_FORMAT_ULAW,
    8000,
    OPBX_FORMAT_SLINEAR,
    8000,
    ulawtolin_new,
    ulawtolin_framein,
    ulawtolin_frameout,
    ulaw_destroy,
    /* NULL */
    ulawtolin_sample
};

/*!
 * \brief The complete translator for lintoulaw.
 */
static struct opbx_translator lintoulaw = {
    "lintoulaw",
    OPBX_FORMAT_SLINEAR,
    8000,
    OPBX_FORMAT_ULAW,
    8000,
    lintoulaw_new,
    lintoulaw_framein,
    lintoulaw_frameout,
    ulaw_destroy,
    /* NULL */
    lintoulaw_sample
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
                        opbx_verbose(VERBOSE_PREFIX_3 "codec_ulaw: %susing generic PLC\n", useplc  ?  ""  :  "not ");
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
    res = opbx_unregister_translator(&lintoulaw);
    if (!res)
        res = opbx_unregister_translator(&ulawtolin);
    if (localusecnt)
        res = -1;
    opbx_mutex_unlock(&localuser_lock);
    return res;
}

int load_module(void)
{
    int res;

    parse_config();
    res = opbx_register_translator(&ulawtolin);
    if (!res)
        res = opbx_register_translator(&lintoulaw);
    else
        opbx_unregister_translator(&ulawtolin);
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
