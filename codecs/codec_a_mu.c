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
 * \brief codec_a_mu.c - translate between alaw and ulaw directly
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

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/lock.h"
#include "openpbx/logger.h"
#include "openpbx/module.h"
#include "openpbx/translate.h"
#include "openpbx/channel.h"
#include "openpbx/alaw.h"
#include "openpbx/ulaw.h"

#define BUFFER_SIZE   8096	/* size for the translation buffers */

OPBX_MUTEX_DEFINE_STATIC(localuser_lock);
static int localusecnt = 0;

static char *tdesc = "A-law and Mulaw direct Coder/Decoder";

static unsigned char mu2a[256];
static unsigned char a2mu[256];

/* Sample frame data (Mu data is okay) */

#include "ulaw_slin_ex.h"

/*
 * Private workspace for translating signed linear signals to alaw.
 */

struct alaw_encoder_pvt
{
    struct opbx_frame f;
    uint8_t offset[OPBX_FRIENDLY_OFFSET];   /* Space to build offset */
    uint8_t outbuf[BUFFER_SIZE];  /* Encoded alaw, two nibbles to a word */
    int tail;
};

/*
 * Private workspace for translating laws.
 */

struct ulaw_encoder_pvt
{
    struct opbx_frame f;
    uint8_t offset[OPBX_FRIENDLY_OFFSET];	/* Space to build offset */
    uint8_t outbuf[BUFFER_SIZE];	/* Encoded ulaw values */
    int tail;
};

static struct opbx_translator_pvt *alawtoulaw_new(void)
{
    struct ulaw_encoder_pvt *tmp;

    if ((tmp = malloc (sizeof (struct ulaw_encoder_pvt))))
    {
	    memset(tmp, 0, sizeof(*tmp));
        tmp->tail = 0;
        localusecnt++;
        opbx_update_use_count();
    }
    return (struct opbx_translator_pvt *) tmp;
}

static struct opbx_translator_pvt *ulawtoalaw_new(void)
{
    struct alaw_encoder_pvt *tmp;
  
    if ((tmp = malloc (sizeof (struct alaw_encoder_pvt))))
    {
	    memset(tmp, 0, sizeof(*tmp));
        localusecnt++;
        opbx_update_use_count();
        tmp->tail = 0;
    }
    return (struct opbx_translator_pvt *) tmp;
}

static int alawtoulaw_framein(struct opbx_translator_pvt *pvt, struct opbx_frame *f)
{
    struct ulaw_encoder_pvt *tmp = (struct ulaw_encoder_pvt *) pvt;
    int x;
    uint8_t *b;

    if ((tmp->tail + f->datalen)> sizeof(tmp->outbuf))
    {
  	    opbx_log(LOG_WARNING, "Out of buffer space\n");
	    return -1;
    }

    /* Reset ssindex and signal to frame's specified values */
    b = f->data;
    for (x = 0;  x < f->datalen;  x++)
  	    tmp->outbuf[tmp->tail + x] = a2mu[b[x]];

    tmp->tail += f->datalen;
    return 0;
}

static struct opbx_frame *alawtoulaw_frameout(struct opbx_translator_pvt *pvt)
{
    struct ulaw_encoder_pvt *tmp = (struct ulaw_encoder_pvt *) pvt;

    if (!tmp->tail)
        return NULL;

    tmp->f.frametype = OPBX_FRAME_VOICE;
    tmp->f.subclass = OPBX_FORMAT_ULAW;
    tmp->f.datalen = tmp->tail;
    tmp->f.samples = tmp->tail;
    tmp->f.mallocd = 0;
    tmp->f.offset = OPBX_FRIENDLY_OFFSET;
    tmp->f.src = __PRETTY_FUNCTION__;
    tmp->f.data = tmp->outbuf;
    tmp->tail = 0;
    return &tmp->f;
}

static int ulawtoalaw_framein(struct opbx_translator_pvt *pvt, struct opbx_frame *f)
{
    struct alaw_encoder_pvt *tmp = (struct alaw_encoder_pvt *) pvt;
    int x;
    uint8_t *s;

    if (tmp->tail + f->datalen >= sizeof(tmp->outbuf))
    {
        opbx_log (LOG_WARNING, "Out of buffer space\n");
        return -1;
    }
    s = f->data;
    for (x = 0;  x < f->datalen;  x++) 
  	    tmp->outbuf[x+tmp->tail] = mu2a[s[x]];
    tmp->tail += f->datalen;
    return 0;
}

/*
 * LinToalaw_FrameOut
 *  Convert a buffer of raw 16-bit signed linear PCM to a buffer
 *  of 4-bit alaw packed two to a byte (Big Endian).
 *
 * Results:
 *  Foo
 *
 * Side effects:
 *  Leftover inbuf data gets packed, tail gets updated.
 */

static struct opbx_frame *ulawtoalaw_frameout(struct opbx_translator_pvt *pvt)
{
    struct alaw_encoder_pvt *tmp = (struct alaw_encoder_pvt *) pvt;
  
    if (tmp->tail)
    {
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

/*
 * alawToLin_Sample
 */

static struct opbx_frame *alawtoulaw_sample(void)
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

static struct opbx_frame *ulawtoalaw_sample(void)
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


/*
 * alaw_Destroy
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

/*
 * The complete translator for alawToLin.
 */

static struct opbx_translator alawtoulaw =
{
    "alawtoulaw",
    OPBX_FORMAT_ALAW,
    8000,
    OPBX_FORMAT_ULAW,
    8000,
    alawtoulaw_new,
    alawtoulaw_framein,
    alawtoulaw_frameout,
    alaw_destroy,
    /* NULL */
    alawtoulaw_sample
};

/*
 * The complete translator for LinToalaw.
 */

static struct opbx_translator ulawtoalaw =
{
    "ulawtoalaw",
    OPBX_FORMAT_ULAW,
    8000,
    OPBX_FORMAT_ALAW,
    8000,
    ulawtoalaw_new,
    ulawtoalaw_framein,
    ulawtoalaw_frameout,
    alaw_destroy,
    /* NULL */
    ulawtoalaw_sample
};

int unload_module(void)
{
    int res;
  
    opbx_mutex_lock(&localuser_lock);
    if ((res = opbx_unregister_translator(&ulawtoalaw)) == 0)
        res = opbx_unregister_translator(&alawtoulaw);
    if (localusecnt)
        res = -1;
    opbx_mutex_unlock(&localuser_lock);
    return res;
}

int load_module(void)
{
    int res;
    int x;

    for (x = 0;  x < 256;  x++)
    {
        mu2a[x] = ulaw_to_alaw(x);
        a2mu[x] = alaw_to_ulaw(x);
    }
    if ((res = opbx_register_translator(&alawtoulaw)) == 0)
        res = opbx_register_translator(&ulawtoalaw);
    else
        opbx_unregister_translator(&alawtoulaw);
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
