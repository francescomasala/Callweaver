/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Implements the 32kbps DVI ADPCM codec, widely used for things like
 * voice mail and IVR, since it is the main codec used by Dialogic.
 *
 * Copyright (c) 2001 - 2005 Digium, Inc.
 * All rights reserved.
 *
 * Karl Sackett <krs@linux-support.net>, 2001-03-21
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
 * \brief codec_dvi_adpcm.c - translate between signed linear and IMA/DVI/Intel ADPCM
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

OPENPBX_FILE_VERSION("$HeadURL: svn://svn.openpbx.org/openpbx/trunk/codecs/codec_adpcm.c $", "$Revision: 1057 $")

#include "openpbx/lock.h"
#include "openpbx/logger.h"
#include "openpbx/module.h"
#include "openpbx/config.h"
#include "openpbx/options.h"
#include "openpbx/translate.h"
#include "openpbx/channel.h"

#define BUFFER_SIZE   8096	/* size for the translation buffers */

OPBX_MUTEX_DEFINE_STATIC(localuser_lock);

static int localusecnt = 0;

static char *tdesc = "DVI/IMA/Intel 32kbps ADPCM encoder/decoder";

static int useplc = 0;

/* Sample frame data */

#include "slin_adpcm_ex.h"
#include "adpcm_slin_ex.h"

/*
 * Private workspace for translating signed linear signals to ADPCM.
 */
struct dvi_adpcm_encoder_pvt
{
    struct opbx_frame f;
    char offset[OPBX_FRIENDLY_OFFSET];   /* Space to build offset */
    int16_t inbuf[BUFFER_SIZE];           /* Unencoded signed linear values */
    uint8_t outbuf[BUFFER_SIZE];  /* Encoded ADPCM, two nibbles to a word */
    ima_adpcm_state_t dvi_state;
    int tail;
};

/*
 * Private workspace for translating ADPCM signals to signed linear.
 */
struct dvi_adpcm_decoder_pvt
{
    struct opbx_frame f;
    char offset[OPBX_FRIENDLY_OFFSET];	/* Space to build offset */
    int16_t outbuf[BUFFER_SIZE];	/* Decoded signed linear values */
    ima_adpcm_state_t dvi_state;
    int tail;
    plc_state_t plc;
};

/*
 * dviadpcmtolin_new
 *  Create a new instance of adpcm_decoder_pvt.
 *
 * Results:
 *  Returns a pointer to the new instance.
 *
 * Side effects:
 *  None.
 */

static struct opbx_translator_pvt *dviadpcmtolin_new(void)
{
    struct dvi_adpcm_decoder_pvt *tmp;
  
    if ((tmp = malloc(sizeof(*tmp))))
    {
	    memset(tmp, 0, sizeof(*tmp));
        ima_adpcm_init(&tmp->dvi_state, 32000);
        tmp->tail = 0;
        plc_init(&tmp->plc);
        localusecnt++;
        opbx_update_use_count ();
    }
    return (struct opbx_translator_pvt *) tmp;
}

/*
 * lintodviadpcm_new
 *  Create a new instance of adpcm_encoder_pvt.
 *
 * Results:
 *  Returns a pointer to the new instance.
 *
 * Side effects:
 *  None.
 */
static struct opbx_translator_pvt *lintodviadpcm_new(void)
{
    struct dvi_adpcm_encoder_pvt *tmp;
  
    if ((tmp = malloc(sizeof(*tmp))))
    {
	    memset(tmp, 0, sizeof(*tmp));
        ima_adpcm_init(&tmp->dvi_state, 32000);
        tmp->tail = 0;
        localusecnt++;
        opbx_update_use_count ();
    }
    return (struct opbx_translator_pvt *) tmp;
}

/*
 * dviadpcmtolin_framein
 *  Take an input buffer with packed 4-bit ADPCM values and put decoded PCM in outbuf, 
 *  if there is room left.
 *
 * Results:
 *  Foo
 *
 * Side effects:
 *  tmp->tail is the number of packed values in the buffer.
 */

static int dviadpcmtolin_framein(struct opbx_translator_pvt *pvt, struct opbx_frame *f)
{
    struct dvi_adpcm_decoder_pvt *tmp = (struct dvi_adpcm_decoder_pvt *) pvt;
    int x;

    if (f->datalen == 0)
    {
        /* perform PLC with nominal framesize of 20ms/160 samples */
        if ((tmp->tail + 160) > sizeof(tmp->outbuf)/sizeof(int16_t))
        {
            opbx_log(LOG_WARNING, "Out of buffer space\n");
            return -1;
        }
        if (useplc)
        {
            plc_fillin(&tmp->plc, tmp->outbuf+tmp->tail, 160);
	        tmp->tail += 160;
	    }
        return 0;
    }

    if (f->datalen*4 + tmp->tail*2 > sizeof(tmp->outbuf))
    {
  	    opbx_log(LOG_WARNING, "Out of buffer space\n");
	    return -1;
    }

    tmp->tail += ima_adpcm_decode(&tmp->dvi_state, tmp->outbuf + tmp->tail, f->data, f->datalen);
    if (useplc)
        plc_rx(&tmp->plc, tmp->outbuf+tmp->tail-f->datalen*2, f->datalen*2);

    return 0;
}

/*
 * dviAdpcmToLin_FrameOut
 *  Convert 4-bit ADPCM encoded signals to 16-bit signed linear.
 *
 * Results:
 *  Converted signals are placed in tmp->f.data, tmp->f.datalen
 *  and tmp->f.samples are calculated.
 *
 * Side effects:
 *  None.
 */

static struct opbx_frame *dviadpcmtolin_frameout(struct opbx_translator_pvt *pvt)
{
    struct dvi_adpcm_decoder_pvt *tmp = (struct dvi_adpcm_decoder_pvt *) pvt;

    if (tmp->tail == 0)
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

/*
 * LinToAdpcm_FrameIn
 *  Fill an input buffer with 16-bit signed linear PCM values.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  tmp->tail is number of signal values in the input buffer.
 */

static int lintodviadpcm_framein(struct opbx_translator_pvt *pvt, struct opbx_frame *f)
{
    struct dvi_adpcm_encoder_pvt *tmp = (struct dvi_adpcm_encoder_pvt *) pvt;

    if ((tmp->tail + f->datalen/sizeof(int16_t)) < (sizeof (tmp->inbuf)/sizeof(int16_t)))
    {
        memcpy (&tmp->inbuf[tmp->tail], f->data, f->datalen);
        tmp->tail += f->datalen/sizeof(int16_t);
    }
    else
    {
        opbx_log (LOG_WARNING, "Out of buffer space\n");
        return -1;
    }
    return 0;
}

/*
 * LinToAdpcm_FrameOut
 *  Convert a buffer of raw 16-bit signed linear PCM to a buffer
 *  of 4-bit ADPCM packed two to a byte (Big Endian).
 *
 * Results:
 *  Foo
 *
 * Side effects:
 *  Leftover inbuf data gets packed, tail gets updated.
 */

static struct opbx_frame *lintodviadpcm_frameout(struct opbx_translator_pvt *pvt)
{
    struct dvi_adpcm_encoder_pvt *tmp = (struct dvi_adpcm_encoder_pvt *) pvt;
    int i_max;
    int i;
  
    if (tmp->tail < 2)
        return NULL;

    i_max = tmp->tail & ~1; /* atomic size is 2 samples */
    ima_adpcm_encode(&tmp->dvi_state, tmp->outbuf, tmp->inbuf, i_max);
    tmp->f.frametype = OPBX_FRAME_VOICE;
    tmp->f.subclass = OPBX_FORMAT_DVI_ADPCM;
    tmp->f.samples = i_max;
    tmp->f.mallocd = 0;
    tmp->f.offset = OPBX_FRIENDLY_OFFSET;
    tmp->f.src = __PRETTY_FUNCTION__;
    tmp->f.data = tmp->outbuf;
    tmp->f.datalen = i_max/2;

    /*
     * If there is a signal left over (there should be no more than
     * one) move it to the beginning of the input buffer.
     */
    if (tmp->tail == i_max)
    {
        tmp->tail = 0;
    }
    else
    {
        tmp->inbuf[0] = tmp->inbuf[tmp->tail];
        tmp->tail = 1;
    }
    return &tmp->f;
}


/*
 * AdpcmToLin_Sample
 */
static struct opbx_frame *dviadpcmtolin_sample(void)
{
    static struct opbx_frame f;
  
    f.frametype = OPBX_FRAME_VOICE;
    f.subclass = OPBX_FORMAT_DVI_ADPCM;
    f.datalen = sizeof (adpcm_slin_ex);
    f.samples = sizeof(adpcm_slin_ex)*sizeof(int16_t);
    f.mallocd = 0;
    f.offset = 0;
    f.src = __PRETTY_FUNCTION__;
    f.data = adpcm_slin_ex;
    return &f;
}

/*
 * LinToAdpcm_Sample
 */
static struct opbx_frame *lintodviadpcm_sample(void)
{
    static struct opbx_frame f;
  
    f.frametype = OPBX_FRAME_VOICE;
    f.subclass = OPBX_FORMAT_SLINEAR;
    f.datalen = sizeof (slin_adpcm_ex);
    /* Assume 8000 Hz */
    f.samples = sizeof (slin_adpcm_ex)/2;
    f.mallocd = 0;
    f.offset = 0;
    f.src = __PRETTY_FUNCTION__;
    f.data = slin_adpcm_ex;
    return &f;
}

/*
 * Adpcm_Destroy
 *  Destroys a private workspace.
 *
 * Results:
 *  It's gone!
 *
 * Side effects:
 *  None.
 */
static void dviadpcm_destroy(struct opbx_translator_pvt *pvt)
{
    free (pvt);
    localusecnt--;
    opbx_update_use_count ();
}

/*
 * The complete translator for ADPCMToLin.
 */
static struct opbx_translator dviadpcmtolin =
{
    "dviadpcmtolin",
    OPBX_FORMAT_DVI_ADPCM,
    8000,
    OPBX_FORMAT_SLINEAR,
    8000,
    dviadpcmtolin_new,
    dviadpcmtolin_framein,
    dviadpcmtolin_frameout,
    dviadpcm_destroy,
    /* NULL */
    dviadpcmtolin_sample
};

/*
 * The complete translator for LinToADPCM.
 */
static struct opbx_translator lintodviadpcm =
{
    "lintodviadpcm",
    OPBX_FORMAT_SLINEAR,
    8000,
    OPBX_FORMAT_DVI_ADPCM,
    8000,
    lintodviadpcm_new,
    lintodviadpcm_framein,
    lintodviadpcm_frameout,
    dviadpcm_destroy,
    /* NULL */
    lintodviadpcm_sample
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
                        opbx_verbose(VERBOSE_PREFIX_3 "codec_adpcm: %susing generic PLC\n", useplc ? "" : "not ");
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
    if ((res = opbx_unregister_translator(&lintodviadpcm)) == 0)
        res = opbx_unregister_translator(&dviadpcmtolin);
    if (localusecnt)
        res = -1;
    opbx_mutex_unlock(&localuser_lock);
    return res;
}

int load_module(void)
{
    int res;
  
    parse_config();
    if ((res = opbx_register_translator(&dviadpcmtolin)) == 0)
        res = opbx_register_translator(&lintodviadpcm);
    else
        opbx_unregister_translator(&dviadpcmtolin);
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
    
    STANDARD_USECOUNT (res);
    return res;
}
