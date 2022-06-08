/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * Based on frompcm.c and topcm.c from the Emiliano MIPL browser/
 * interpreter.  See http://www.bsdtelephony.com.mx
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
 * \brief codec_g726.c - translate between signed linear and ITU G.726-32kbps
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

#define WANT_ASM
#include "log2comp.h"

#define BUFFER_SIZE   8096	/* size for the translation buffers */
#define BUF_SHIFT	5

OPBX_MUTEX_DEFINE_STATIC(localuser_lock);
static int localusecnt = 0;

static char *tdesc = "ITU G.726-32kbps G726 Transcoder";

static int useplc = 0;

/* Sample frame data */

/* 10ms of linear silence, at 8k samples/second */
static const int16_t slin_g726_ex[] =
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

/* 20ms of G.726 at 32kbps */
static const uint8_t g726_slin_ex[] =
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

/*
 * Private workspace for translating signed linear signals to G726.
 */
struct g726_encoder_pvt
{
    struct opbx_frame f;
    uint8_t offset[OPBX_FRIENDLY_OFFSET];   /* Space to build offset */
    uint8_t outbuf[BUFFER_SIZE];  /* Encoded G726, two nibbles to a word */
    uint8_t next_flag;
    g726_state_t g726_state;
    int tail;
};

/*
 * Private workspace for translating G726 signals to signed linear.
 */
struct g726_decoder_pvt
{
    struct opbx_frame f;
    uint8_t offset[OPBX_FRIENDLY_OFFSET];	/* Space to build offset */
    int16_t outbuf[BUFFER_SIZE];	/* Decoded signed linear values */
    g726_state_t g726_state;
    int tail;
    plc_state_t plc;
};

/*
 * G726ToLin_New
 *  Create a new instance of g726_decoder_pvt.
 *
 * Results:
 *  Returns a pointer to the new instance.
 *
 * Side effects:
 *  None.
 */

static struct opbx_translator_pvt *g726tolin_new(void)
{
    struct g726_decoder_pvt *tmp;
  
    if ((tmp = malloc(sizeof (struct g726_decoder_pvt))))
    {
	    memset(tmp, 0, sizeof(*tmp));
        localusecnt++;
        tmp->tail = 0;
        plc_init(&tmp->plc);
        g726_init(&(tmp->g726_state), 32000, G726_ENCODING_LINEAR, G726_PACKING_LEFT);
        opbx_update_use_count();
    }
    return (struct opbx_translator_pvt *) tmp;
}

/*
 * LinToG726_New
 *  Create a new instance of g726_encoder_pvt.
 *
 * Results:
 *  Returns a pointer to the new instance.
 *
 * Side effects:
 *  None.
 */

static struct opbx_translator_pvt *lintog726_new(void)
{
    struct g726_encoder_pvt *tmp;
  
    if ((tmp = malloc(sizeof (struct g726_encoder_pvt))))
    {
	    memset(tmp, 0, sizeof(*tmp));
        localusecnt++;
        tmp->tail = 0;
        g726_init(&(tmp->g726_state), 32000, G726_ENCODING_LINEAR, G726_PACKING_LEFT);
        opbx_update_use_count();
    }
    return (struct opbx_translator_pvt *) tmp;
}

/*
 * G726ToLin_FrameIn
 *  Fill an input buffer with packed 4-bit G726 values if there is room
 *  left.
 *
 * Results:
 *  Foo
 *
 * Side effects:
 *  tmp->tail is the number of packed values in the buffer.
 */

static int g726tolin_framein(struct opbx_translator_pvt *pvt, struct opbx_frame *f)
{
    struct g726_decoder_pvt *tmp = (struct g726_decoder_pvt *) pvt;

    if (f->datalen == 0)
    {
        /* Perform PLC with nominal framesize of 20ms/160 samples */
        if ((tmp->tail + 160) > BUFFER_SIZE)
        {
            opbx_log(LOG_WARNING, "Out of buffer space\n");
            return -1;
        }
        if (useplc)
        {
	        plc_fillin(&tmp->plc, tmp->outbuf + tmp->tail, 160);
	        tmp->tail += 160;
	    }
    }
    else
    {
        if ((tmp->tail + f->datalen*2) > BUFFER_SIZE)
        {
            opbx_log(LOG_WARNING, "Out of buffer space\n");
            return -1;
        }
        tmp->tail += g726_decode(&(tmp->g726_state),
                                 tmp->outbuf + tmp->tail,
                                 (const uint8_t *) f->data,
                                 f->datalen);
        if (useplc)
            plc_rx(&tmp->plc, tmp->outbuf + tmp->tail - f->datalen*2, f->datalen*2);
    }
    return 0;
}

/*
 * G726ToLin_FrameOut
 *  Convert 4-bit G726 encoded signals to 16-bit signed linear.
 *
 * Results:
 *  Converted signals are placed in tmp->f.data, tmp->f.datalen
 *  and tmp->f.samples are calculated.
 *
 * Side effects:
 *  None.
 */

static struct opbx_frame *g726tolin_frameout(struct opbx_translator_pvt *pvt)
{
    struct g726_decoder_pvt *tmp = (struct g726_decoder_pvt *) pvt;

    if (!tmp->tail)
        return NULL;

    tmp->f.frametype = OPBX_FRAME_VOICE;
    tmp->f.subclass = OPBX_FORMAT_SLINEAR;
    tmp->f.datalen = tmp->tail * 2;
    tmp->f.samples = tmp->tail;
    tmp->f.mallocd = 0;
    tmp->f.offset = OPBX_FRIENDLY_OFFSET;
    tmp->f.src = __PRETTY_FUNCTION__;
    tmp->f.data = tmp->outbuf;
    tmp->tail = 0;
    return &tmp->f;
}

/*
 * LinToG726_FrameIn
 *  Fill an input buffer with 16-bit signed linear PCM values.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  tmp->tail is number of signal values in the input buffer.
 */

static int lintog726_framein(struct opbx_translator_pvt *pvt, struct opbx_frame *f)
{
    struct g726_encoder_pvt *tmp = (struct g726_encoder_pvt *) pvt;
  
    if ((tmp->tail + f->datalen/(2*sizeof(int16_t)) + 1) > BUFFER_SIZE)
    {
        opbx_log(LOG_WARNING, "Out of buffer space\n");
        return -1;
    }
    tmp->tail += g726_encode(&(tmp->g726_state),
                             tmp->outbuf + tmp->tail,
                             f->data,
                             f->datalen/sizeof(int16_t));
    return 0;
}

/*
 * LinToG726_FrameOut
 *  Convert a buffer of raw 16-bit signed linear PCM to a buffer
 *  of 4-bit G726 packed two to a byte (Big Endian).
 *
 * Results:
 *  Foo
 *
 * Side effects:
 *  Leftover inbuf data gets packed, tail gets updated.
 */

static struct opbx_frame *lintog726_frameout(struct opbx_translator_pvt *pvt)
{
  struct g726_encoder_pvt *tmp = (struct g726_encoder_pvt *) pvt;
  
    if (!tmp->tail)
  	    return NULL;
    tmp->f.frametype = OPBX_FRAME_VOICE;
    tmp->f.subclass = OPBX_FORMAT_G726;
    tmp->f.samples = tmp->tail * 2;
    tmp->f.mallocd = 0;
    tmp->f.offset = OPBX_FRIENDLY_OFFSET;
    tmp->f.src = __PRETTY_FUNCTION__;
    tmp->f.data = tmp->outbuf;
    tmp->f.datalen = tmp->tail;

    tmp->tail = 0;
    return &tmp->f;
}


/*
 * G726ToLin_Sample
 */

static struct opbx_frame *g726tolin_sample(void)
{
    static struct opbx_frame f;
 
    f.frametype = OPBX_FRAME_VOICE;
    f.subclass = OPBX_FORMAT_G726;
    f.datalen = sizeof (g726_slin_ex);
    f.samples = sizeof(g726_slin_ex) * 2;
    f.mallocd = 0;
    f.offset = 0;
    f.src = __PRETTY_FUNCTION__;
    f.data = (uint8_t *) g726_slin_ex;
    return &f;
}

/*
 * LinToG726_Sample
 */

static struct opbx_frame *lintog726_sample(void)
{
    static struct opbx_frame f;
  
    f.frametype = OPBX_FRAME_VOICE;
    f.subclass = OPBX_FORMAT_SLINEAR;
    f.datalen = sizeof (slin_g726_ex);
    /* Assume 8000 Hz */
    f.samples = sizeof (slin_g726_ex) / 2;
    f.mallocd = 0;
    f.offset = 0;
    f.src = __PRETTY_FUNCTION__;
    f.data = (int16_t *) slin_g726_ex;
    return &f;
}

/*
 * G726_Destroy
 *  Destroys a private workspace.
 *
 * Results:
 *  It's gone!
 *
 * Side effects:
 *  None.
 */

static void g726_destroy(struct opbx_translator_pvt *pvt)
{
    free(pvt);
    localusecnt--;
    opbx_update_use_count();
}

/*
 * The complete translator for G726ToLin.
 */

static struct opbx_translator g726tolin =
{
    "g726tolin",
    OPBX_FORMAT_G726,
    8000,
    OPBX_FORMAT_SLINEAR,
    8000,
    g726tolin_new,
    g726tolin_framein,
    g726tolin_frameout,
    g726_destroy,
    /* NULL */
    g726tolin_sample
};

/*
 * The complete translator for LinToG726.
 */

static struct opbx_translator lintog726 =
{
    "lintog726",
    OPBX_FORMAT_SLINEAR,
    8000,
    OPBX_FORMAT_G726,
    8000,
    lintog726_new,
    lintog726_framein,
    lintog726_frameout,
    g726_destroy,
    /* NULL */
    lintog726_sample
};

static void parse_config(void)
{
    struct opbx_config *cfg;
    struct opbx_variable *var;
  
    if ((cfg = opbx_config_load("codecs.conf")))
    {
        if ((var = opbx_variable_browse(cfg, "plc")))
        {
            while (var)
            {
                if (!strcasecmp(var->name, "genericplc"))
                {
                    useplc = opbx_true(var->value) ? 1 : 0;
                    if (option_verbose > 2)
                        opbx_verbose(VERBOSE_PREFIX_3 "codec_g726: %susing generic PLC\n", useplc ? "" : "not ");
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
    if ((res = opbx_unregister_translator(&lintog726)) == 0)
        res = opbx_unregister_translator(&g726tolin);
    if (localusecnt)
        res = -1;
    opbx_mutex_unlock(&localuser_lock);
    return res;
}

int load_module(void)
{
    int res;
 
    parse_config();
    if ((res = opbx_register_translator(&g726tolin)) == 0)
        res = opbx_register_translator(&lintog726);
    else
        opbx_unregister_translator(&g726tolin);
    return res;
}

/*
 * Return a description of this module.
 */
char *description(void)
{
    return tdesc;
}

int usecount (void)
{
    int res;
    STANDARD_USECOUNT (res);
    return res;
}
