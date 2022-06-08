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
 * \brief Translate between signed linear and GSM 06.10
 *
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <string.h>
#include <stdio.h>
#include <spandsp.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/lock.h"
#include "openpbx/translate.h"
#include "openpbx/config.h"
#include "openpbx/options.h"
#include "openpbx/module.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"

#include "../formats/msgsm.h"

/* Sample frame data */
#include "slin_gsm_ex.h"
#include "gsm_slin_ex.h"

OPBX_MUTEX_DEFINE_STATIC(localuser_lock);
static int localusecnt=0;

static char *tdesc = "GSM06.10/PCM16 (signed linear) Codec Translator";

static int useplc = 0;

struct opbx_translator_pvt {
	gsm0610_state_t *gsm;
	struct opbx_frame f;
	/* Space to build offset */
	char offset[OPBX_FRIENDLY_OFFSET];
	/* Buffer for our outgoing frame */
	int16_t outbuf[8000];
	/* Enough to store a full second */
	int16_t buf[8000];
	int tail;
	plc_state_t plc;
};

#define gsm_coder_pvt opbx_translator_pvt

static struct opbx_translator_pvt *gsm_new(void)
{
	struct gsm_coder_pvt *tmp;

	if ((tmp = malloc(sizeof(struct gsm_coder_pvt)))) {
		if ((tmp->gsm = gsm0610_init(NULL, GSM0610_PACKING_VOIP)) == NULL) {
			free(tmp);
			tmp = NULL;
		}
		tmp->tail = 0;
		plc_init(&tmp->plc);
		localusecnt++;
	}
	return tmp;
}

static struct opbx_frame *lintogsm_sample(void)
{
	static struct opbx_frame f;
	f.frametype = OPBX_FRAME_VOICE;
	f.subclass = OPBX_FORMAT_SLINEAR;
	f.datalen = sizeof(slin_gsm_ex);
	/* Assume 8000 Hz */
	f.samples = sizeof(slin_gsm_ex)/2;
	f.mallocd = 0;
	f.offset = 0;
	f.src = __PRETTY_FUNCTION__;
	f.data = slin_gsm_ex;
	return &f;
}

static struct opbx_frame *gsmtolin_sample(void)
{
	static struct opbx_frame f;
	f.frametype = OPBX_FRAME_VOICE;
	f.subclass = OPBX_FORMAT_GSM;
	f.datalen = sizeof(gsm_slin_ex);
	/* All frames are 20 ms long */
	f.samples = 160;
	f.mallocd = 0;
	f.offset = 0;
	f.src = __PRETTY_FUNCTION__;
	f.data = gsm_slin_ex;
	return &f;
}

static struct opbx_frame *gsmtolin_frameout(struct opbx_translator_pvt *tmp)
{
	if (!tmp->tail)
		return NULL;
	/* Signed linear is no particular frame size, so just send whatever
	   we have in the buffer in one lump sum */
	tmp->f.frametype = OPBX_FRAME_VOICE;
	tmp->f.subclass = OPBX_FORMAT_SLINEAR;
	tmp->f.datalen = tmp->tail*sizeof(int16_t);
	/* Assume 8000 Hz */
	tmp->f.samples = tmp->tail;
	tmp->f.mallocd = 0;
	tmp->f.offset = OPBX_FRIENDLY_OFFSET;
	tmp->f.src = __PRETTY_FUNCTION__;
	tmp->f.data = tmp->buf;
	/* Reset tail pointer */
	tmp->tail = 0;

	return &tmp->f;	
}

static int gsmtolin_framein(struct opbx_translator_pvt *tmp, struct opbx_frame *f)
{
	/* Assuming there's space left, decode into the current buffer at
	   the tail location.  Read in as many frames as there are */
	int x;
	uint8_t data[66];
	int msgsm = 0;
	
	if (f->datalen == 0) {
        /* perform PLC with nominal framesize of 20ms/160 samples */
	    if((tmp->tail + 160) > sizeof(tmp->buf) / 2) {
		    opbx_log(LOG_WARNING, "Out of buffer space\n");
		    return -1;
	    }
	    if(useplc) {
		    plc_fillin(&tmp->plc, tmp->buf+tmp->tail, 160);
		    tmp->tail += 160;
	    }
	    return 0;
	}

	if ((f->datalen % 33)  &&  (f->datalen % 65)) {
		opbx_log(LOG_WARNING, "Huh?  A GSM frame that isn't a multiple of 33 or 65 bytes long from %s (%d)?\n", f->src, f->datalen);
		return -1;
	}
	
	if (f->datalen % 65 == 0) 
		msgsm = 1;
		
	for (x = 0;  x < f->datalen;  x += (msgsm  ?  65  :  33)) {
		if (msgsm) {
			/* Translate MSGSM format to Real GSM format before feeding in */
			conv65(f->data + x, data);
			if (tmp->tail + 320 < sizeof(tmp->buf)/2) {	
				if (gsm0610_decode(tmp->gsm, tmp->buf + tmp->tail, data, 1) != 160) {
					opbx_log(LOG_WARNING, "Invalid GSM data (1)\n");
					return -1;
				}
				tmp->tail += 160;
				if (gsm0610_decode(tmp->gsm, tmp->buf + tmp->tail, data + 33, 1) != 160) {
					opbx_log(LOG_WARNING, "Invalid GSM data (2)\n");
					return -1;
				}
				tmp->tail += 160;
			} else {
				opbx_log(LOG_WARNING, "Out of (MS) buffer space\n");
				return -1;
			}
		} else {
			if (tmp->tail + 160 < sizeof(tmp->buf)/sizeof(int16_t)) {	
				if (gsm0610_decode(tmp->gsm, tmp->buf + tmp->tail, f->data + x, 1) != 160) {
					opbx_log(LOG_WARNING, "Invalid GSM data\n");
					return -1;
				}
				tmp->tail += 160;
			} else {
				opbx_log(LOG_WARNING, "Out of buffer space\n");
				return -1;
			}
		}
	}

	/* just add the last 20ms frame; there must have been at least one */
	if(useplc) plc_rx(&tmp->plc, tmp->buf+tmp->tail-160, 160);

	return 0;
}

static int lintogsm_framein(struct opbx_translator_pvt *tmp, struct opbx_frame *f)
{
	/* Just add the frames to our stream */
	/* XXX We should look at how old the rest of our stream is, and if it
	   is too old, then we should overwrite it entirely, otherwise we can
	   get artifacts of earlier talk that do not belong */
	if (tmp->tail + f->datalen/2 < sizeof(tmp->buf) / 2) {
		memcpy((tmp->buf + tmp->tail), f->data, f->datalen);
		tmp->tail += f->datalen/2;
	} else {
		opbx_log(LOG_WARNING, "Out of buffer space\n");
		return -1;
	}
	return 0;
}

static struct opbx_frame *lintogsm_frameout(struct opbx_translator_pvt *tmp)
{
	int x = 0;

	/* We can't work on anything less than a frame in size */
	if (tmp->tail < 160)
		return NULL;
	tmp->f.frametype = OPBX_FRAME_VOICE;
	tmp->f.subclass = OPBX_FORMAT_GSM;
	tmp->f.mallocd = 0;
	tmp->f.offset = OPBX_FRIENDLY_OFFSET;
	tmp->f.src = __PRETTY_FUNCTION__;
	tmp->f.data = tmp->outbuf;
	while (tmp->tail >= 160) {
		if ((x + 1)*33 >= sizeof(tmp->outbuf)) {
			opbx_log(LOG_WARNING, "Out of buffer space\n");
			break;
		}
		/* Encode a frame of data */
		gsm0610_encode(tmp->gsm, ((uint8_t *) tmp->outbuf) + (x * 33), tmp->buf, 1);
		/* Assume 8000 Hz -- 20 ms */
		tmp->tail -= 160;
		/* Move the data at the end of the buffer to the front */
		if (tmp->tail)
			memmove(tmp->buf, tmp->buf + 160, tmp->tail*sizeof(int16_t));
		x++;
	}
	tmp->f.datalen = x*33;
	tmp->f.samples = x*160;
	return &tmp->f;	
}

static void gsm_destroy_stuff(struct opbx_translator_pvt *pvt)
{
	if (pvt->gsm)
		gsm0610_release(pvt->gsm);
	free(pvt);
	localusecnt--;
}

static struct opbx_translator gsmtolin =
{
	"gsmtolin", 
	OPBX_FORMAT_GSM,
    8000,
    OPBX_FORMAT_SLINEAR,
    8000,
	gsm_new,
	gsmtolin_framein,
	gsmtolin_frameout,
	gsm_destroy_stuff,
	gsmtolin_sample
};

static struct opbx_translator lintogsm =
{
    "lintogsm", 
	OPBX_FORMAT_SLINEAR,
    8000,
    OPBX_FORMAT_GSM,
    8000,
	gsm_new,
	lintogsm_framein,
	lintogsm_frameout,
	gsm_destroy_stuff,
	lintogsm_sample
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
				       opbx_verbose(VERBOSE_PREFIX_3 "codec_gsm: %susing generic PLC\n", useplc ? "" : "not ");
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
	res = opbx_unregister_translator(&lintogsm);
	if (!res)
		res = opbx_unregister_translator(&gsmtolin);
	if (localusecnt)
		res = -1;
	opbx_mutex_unlock(&localuser_lock);
	return res;
}

int load_module(void)
{
	int res;

	parse_config();
	res = opbx_register_translator(&gsmtolin);
	if (!res) 
		res=opbx_register_translator(&lintogsm);
	else
		opbx_unregister_translator(&gsmtolin);
	return res;
}

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
