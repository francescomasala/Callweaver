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
 * \brief Tone detection routines
 *
 */

#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <stdio.h>
#include <spandsp.h>

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/corelib/dsp.c $", "$Revision: 4723 $")

#include "callweaver/frame.h"
#include "callweaver/channel.h"
#include "callweaver/logger.h"
#include "callweaver/dsp.h"
#include "callweaver/ulaw.h"
#include "callweaver/alaw.h"

/* Number of goertzels for progress detect */
#define GSAMP_SIZE_NA       183     /* North America - 350, 440, 480, 620, 950, 1400, 1800 Hz */
#define GSAMP_SIZE_CR       188     /* Costa Rica, Brazil - Only care about 425 Hz */
#define GSAMP_SIZE_UK       160     /* UK disconnect goertzel feed - should trigger 400Hz */

#define PROG_MODE_NA        0
#define PROG_MODE_CR        1
#define PROG_MODE_UK        2

/* For US modes */
#define HZ_350  0
#define HZ_440  1
#define HZ_480  2
#define HZ_620  3
#define HZ_950  4
#define HZ_1400 5
#define HZ_1800 6

/* For CR/BR modes */
#define HZ_425  0

/* For UK mode */
#define HZ_400  0

static struct progalias
{
    char *name;
    int mode;
} aliases[] =
{
    { "us", PROG_MODE_NA },
    { "ca", PROG_MODE_NA },
    { "cr", PROG_MODE_CR },
    { "br", PROG_MODE_CR },
    { "uk", PROG_MODE_UK },
};

static struct progress
{
    int size;
    int freqs[7];
} modes[] =
{
    { GSAMP_SIZE_NA, { 350, 440, 480, 620, 950, 1400, 1800 } },    /* North America */
    { GSAMP_SIZE_CR, { 425 } },
    { GSAMP_SIZE_UK, { 400 } },
};

#define DEFAULT_THRESHOLD    512

#define BUSY_PERCENT        10      /* The percentage difference between the two last silence periods */
#define BUSY_PAT_PERCENT    7       /* The percentage difference between measured and actual pattern */
#define BUSY_THRESHOLD      100     /* Max number of ms difference between max and min times in busy */
#define BUSY_MIN            75      /* Busy must be at least 80 ms in half-cadence */
#define BUSY_MAX            3100    /* Busy can't be longer than 3100 ms in half-cadence */

/* Remember last 15 units */
#define DSP_HISTORY         15

#define TONE_THRESH         10.0f   /* How much louder the tone should be than channel energy */
#define TONE_MIN_THRESH     1.0e8f  /* How much tone there should be at least to attempt */
#define COUNT_THRESH        3       /* Need at least 50ms of stuff to count it */
#define UK_HANGUP_THRESH    60      /* This is the threshold for the UK */

#define BUSYDETECT

#if !defined(BUSYDETECT_MARTIN)  &&  !defined(BUSYDETECT)  &&  !defined(BUSYDETECT_COMPARE_TONE_AND_SILENCE)
#define BUSYDETECT_MARTIN
#endif

struct cw_dsp
{
    struct cw_frame f;
    int threshold;
    int totalsilence;
    int totalnoise;
    int features;
    int busy_maybe;
    int busycount;
    int busy_tonelength;
    int busy_quietlength;
    int historicnoise[DSP_HISTORY];
    int historicsilence[DSP_HISTORY];
    goertzel_state_t freqs[7];
    int freqcount;
    int gsamps;
    int gsamp_size;
    int progmode;
    int tstate;
    int tcount;
    int digitmode;
    int thinkdigit;
    int mute_lag;
    float genergy;
    dtmf_rx_state_t dtmf_rx;
    modem_connect_tones_rx_state_t fax_ced_rx;
    modem_connect_tones_rx_state_t fax_cng_rx;
    bell_mf_rx_state_t bell_mf_rx;
};

static inline int pair_there(float p1, float p2, float i1, float i2, float e)
{
    /* See if p1 and p2 are there, relative to i1 and i2 and total energy */
    /* Make sure absolute levels are high enough */
    if ((p1 < TONE_MIN_THRESH) || (p2 < TONE_MIN_THRESH))
        return 0;
    /* Amplify ignored stuff */
    i2 *= TONE_THRESH;
    i1 *= TONE_THRESH;
    e *= TONE_THRESH;
    /* Check first tone */
    if ((p1 < i1)  ||  (p1 < i2)  ||  (p1 < e))
        return 0;
    /* And second */
    if ((p2 < i1)  ||  (p2 < i2)  ||  (p2 < e))
        return 0;
    /* Guess it's there... */
    return 1;
}

static int __cw_dsp_call_progress(struct cw_dsp *dsp, int16_t *s, int len)
{
    int x;
    int y;
    int pass;
    int newstate;
    int res;
    int thresh;

    newstate = DSP_TONE_STATE_SILENCE;
    res = 0;
    thresh = (dsp->progmode == PROG_MODE_UK)  ?  UK_HANGUP_THRESH  :  COUNT_THRESH;
    while (len)
    {
        /* Take the lesser of the number of samples we need and what we have */
        pass = len;
        if (pass > dsp->gsamp_size - dsp->gsamps) 
            pass = dsp->gsamp_size - dsp->gsamps;
        for (x = 0;  x < pass;  x++)
        {
            for (y = 0;  y < dsp->freqcount;  y++) 
                goertzel_sample(&dsp->freqs[y], s[x]);
            dsp->genergy += s[x] * s[x];
        }
        s += pass;
        dsp->gsamps += pass;
        len -= pass;
        if (dsp->gsamps == dsp->gsamp_size)
        {
            float hz[7];
            for (y = 0;  y < 7;  y++)
                hz[y] = goertzel_result(&dsp->freqs[y]);
#if 0
            printf("\n350:     425:     440:     480:     620:     950:     1400:    1800:    Energy:   \n");
            printf("%.2e %.2e %.2e %.2e %.2e %.2e %.2e %.2e %.2e\n", 
                hz[HZ_350], hz[HZ_425], hz[HZ_440], hz[HZ_480], hz[HZ_620], hz[HZ_950], hz[HZ_1400], hz[HZ_1800], dsp->genergy);
#endif
            switch (dsp->progmode)
            {
            case PROG_MODE_NA:
                if (pair_there(hz[HZ_480], hz[HZ_620], hz[HZ_350], hz[HZ_440], dsp->genergy))
                {
                    newstate = DSP_TONE_STATE_BUSY;
                }
                else if (pair_there(hz[HZ_440], hz[HZ_480], hz[HZ_350], hz[HZ_620], dsp->genergy))
                {
                    newstate = DSP_TONE_STATE_RINGING;
                }
                else if (pair_there(hz[HZ_350], hz[HZ_440], hz[HZ_480], hz[HZ_620], dsp->genergy))
                {
                    newstate = DSP_TONE_STATE_DIALTONE;
                }
                else if (hz[HZ_950] > TONE_MIN_THRESH * TONE_THRESH)
                {
                    newstate = DSP_TONE_STATE_SPECIAL1;
                }
                else if (hz[HZ_1400] > TONE_MIN_THRESH * TONE_THRESH)
                {
                    if (dsp->tstate == DSP_TONE_STATE_SPECIAL1)
                        newstate = DSP_TONE_STATE_SPECIAL2;
                }
                else if (hz[HZ_1800] > TONE_MIN_THRESH * TONE_THRESH)
                {
                    if (dsp->tstate == DSP_TONE_STATE_SPECIAL2)
                        newstate = DSP_TONE_STATE_SPECIAL3;
                }
                else if (dsp->genergy > TONE_MIN_THRESH * TONE_THRESH)
                {
                    newstate = DSP_TONE_STATE_TALKING;
                }
                else
                {
                    newstate = DSP_TONE_STATE_SILENCE;
                }
                break;
            case PROG_MODE_CR:
                if (hz[HZ_425] > TONE_MIN_THRESH * TONE_THRESH)
                    newstate = DSP_TONE_STATE_RINGING;
                else if (dsp->genergy > TONE_MIN_THRESH * TONE_THRESH)
                    newstate = DSP_TONE_STATE_TALKING;
                else
                    newstate = DSP_TONE_STATE_SILENCE;
                break;
            case PROG_MODE_UK:
                if (hz[HZ_400] > TONE_MIN_THRESH * TONE_THRESH)
                    newstate = DSP_TONE_STATE_HUNGUP;
                break;
            default:
                cw_log(LOG_WARNING, "Can't process in unknown prog mode '%d'\n", dsp->progmode);
            }
            if (newstate == dsp->tstate)
            {
                dsp->tcount++;
                if (dsp->tcount == thresh)
                {
                    if ((dsp->features & DSP_PROGRESS_BUSY)
                            && 
                        dsp->tstate == DSP_TONE_STATE_BUSY)
                    {
                        res = CW_CONTROL_BUSY;
                        dsp->features &= ~DSP_FEATURE_CALL_PROGRESS;
                    }
                    else if ((dsp->features & DSP_PROGRESS_TALK)
                             && 
                             dsp->tstate == DSP_TONE_STATE_TALKING)
                    {
                        res = CW_CONTROL_ANSWER;
                        dsp->features &= ~DSP_FEATURE_CALL_PROGRESS;
                    }
                    else if ((dsp->features & DSP_PROGRESS_RINGING)
                             && 
                             dsp->tstate == DSP_TONE_STATE_RINGING)
                    {
                        res = CW_CONTROL_RINGING;
                    }
                    else if ((dsp->features & DSP_PROGRESS_CONGESTION)
                             && 
                             dsp->tstate == DSP_TONE_STATE_SPECIAL3)
                    {
                        res = CW_CONTROL_CONGESTION;
                        dsp->features &= ~DSP_FEATURE_CALL_PROGRESS;
                    }
                    else if ((dsp->features & DSP_FEATURE_CALL_PROGRESS)
                             &&
                             dsp->tstate == DSP_TONE_STATE_HUNGUP)
                    {
                        res = CW_CONTROL_HANGUP;
                        dsp->features &= ~DSP_FEATURE_CALL_PROGRESS;
                    }
                }
            }
            else
            {
                dsp->tstate = newstate;
                dsp->tcount = 1;
            }
            
            /* Reset goertzel */                        
            for (x = 0;  x < 7;  x++)
                goertzel_reset(&dsp->freqs[x]);
            dsp->gsamps = 0;
            dsp->genergy = 0.0f;
        }
    }
    return res;
}

int cw_dsp_call_progress(struct cw_dsp *dsp, struct cw_frame *inf)
{
    if (inf->frametype != CW_FRAME_VOICE)
    {
        cw_log(LOG_WARNING, "Can't check call progress of non-voice frames\n");
        return 0;
    }
    if (inf->subclass != CW_FORMAT_SLINEAR)
    {
        cw_log(LOG_WARNING, "Can only check call progress in signed-linear frames\n");
        return 0;
    }
    return __cw_dsp_call_progress(dsp, inf->data, inf->datalen / 2);
}

static int __cw_dsp_silence(struct cw_dsp *dsp, int16_t amp[], int len, int *totalsilence)
{
    int accum;
    int x;
    int res;

    if (!len)
        return 0;
    accum = 0;
    for (x = 0;  x < len;  x++) 
        accum += abs(amp[x]);
    accum /= len;
    if (accum < dsp->threshold)
    {
        /* Silent */
        dsp->totalsilence += len/8;
        if (dsp->totalnoise)
        {
            /* Move and save history */
            memmove(dsp->historicnoise + DSP_HISTORY - dsp->busycount, dsp->historicnoise + DSP_HISTORY - dsp->busycount +1, dsp->busycount*sizeof(dsp->historicnoise[0]));
            dsp->historicnoise[DSP_HISTORY - 1] = dsp->totalnoise;
            /* We don't want to check for busydetect that frequently */
        }
        dsp->totalnoise = 0;
        res = TRUE;
    }
    else
    {
        /* Not silent */
        dsp->totalnoise += len/8;
        if (dsp->totalsilence)
        {
            int silence1 = dsp->historicsilence[DSP_HISTORY - 1];
            int silence2 = dsp->historicsilence[DSP_HISTORY - 2];
            /* Move and save history */
            memmove(dsp->historicsilence + DSP_HISTORY - dsp->busycount, dsp->historicsilence + DSP_HISTORY - dsp->busycount + 1, dsp->busycount*sizeof(dsp->historicsilence[0]));
            dsp->historicsilence[DSP_HISTORY - 1] = dsp->totalsilence;
            /* check if the previous sample differs only by BUSY_PERCENT from the one before it */
            if (silence1 < silence2)
                dsp->busy_maybe = (silence1 + silence1*BUSY_PERCENT/100 >= silence2);
            else
                dsp->busy_maybe = (silence1 - silence1*BUSY_PERCENT/100 <= silence2);
        }
        dsp->totalsilence = 0;
        res = FALSE;
    }
    if (totalsilence)
        *totalsilence = dsp->totalsilence;
    return res;
}

#ifdef BUSYDETECT_MARTIN
int cw_dsp_busydetect(struct cw_dsp *dsp)
{
    int res = 0;
    int x;
    int avgsilence = 0;
    int hitsilence = 0;
    int avgtone = 0;
    int hittone = 0;

    if (!dsp->busy_maybe)
        return res;
    for (x = DSP_HISTORY - dsp->busycount;  x < DSP_HISTORY;  x++)
    {
        avgsilence += dsp->historicsilence[x];
        avgtone += dsp->historicnoise[x];
    }
    avgsilence /= dsp->busycount;
    avgtone /= dsp->busycount;
    for (x = DSP_HISTORY - dsp->busycount;  x < DSP_HISTORY;  x++)
    {
        if (avgsilence > dsp->historicsilence[x])
        {
            if (avgsilence - (avgsilence*BUSY_PERCENT/100) <= dsp->historicsilence[x])
                hitsilence++;
        }
        else
        {
            if (avgsilence + (avgsilence*BUSY_PERCENT/100) >= dsp->historicsilence[x])
                hitsilence++;
        }
        if (avgtone > dsp->historicnoise[x])
        {
            if (avgtone - (avgtone*BUSY_PERCENT/100) <= dsp->historicnoise[x])
                hittone++;
        }
        else
        {
            if (avgtone + (avgtone*BUSY_PERCENT/100) >= dsp->historicnoise[x])
                hittone++;
        }
    }
    if ((hittone >= dsp->busycount - 1)
        &&
        (hitsilence >= dsp->busycount - 1)
        && 
        (avgtone >= BUSY_MIN  &&  avgtone <= BUSY_MAX)
        && 
        (avgsilence >= BUSY_MIN  &&  avgsilence <= BUSY_MAX))
    {
#ifdef BUSYDETECT_COMPARE_TONE_AND_SILENCE
        if (avgtone > avgsilence)
        {
            if (avgtone - avgtone*BUSY_PERCENT/100 <= avgsilence)
                res = 1;
        }
        else
        {
            if (avgtone + avgtone*BUSY_PERCENT/100 >= avgsilence)
                res = 1;
        }
#else
        res = 1;
#endif
    }
    /* If we know the expected busy tone length, check we are in the range */
    if (res && (dsp->busy_tonelength > 0))
    {
        if (abs(avgtone - dsp->busy_tonelength) > (dsp->busy_tonelength*BUSY_PAT_PERCENT/100))
            res = 0;
    }
    /* If we know the expected busy tone silent-period length, check we are in the range */
    if (res && (dsp->busy_quietlength > 0))
    {
        if (abs(avgsilence - dsp->busy_quietlength) > (dsp->busy_quietlength*BUSY_PAT_PERCENT/100))
            res = 0;
    }
#if 1
    if (res)
        cw_log(LOG_DEBUG, "cw_dsp_busydetect detected busy, avgtone: %d, avgsilence %d\n", avgtone, avgsilence);
#endif
    return res;
}
#endif

#ifdef BUSYDETECT
int cw_dsp_busydetect(struct cw_dsp *dsp)
{
    int x;
    int res;
    int max;
    int min;

    res = 0;
    if (dsp->busy_maybe)
    {
        dsp->busy_maybe = FALSE;
        min = 9999;
        max = 0;
        for (x = DSP_HISTORY - dsp->busycount;  x < DSP_HISTORY;  x++)
        {
            if (dsp->historicsilence[x] < min)
                min = dsp->historicsilence[x];
            if (dsp->historicnoise[x] < min)
                min = dsp->historicnoise[x];
            if (dsp->historicsilence[x] > max)
                max = dsp->historicsilence[x];
            if (dsp->historicnoise[x] > max)
                max = dsp->historicnoise[x];
        }
        if ((max - min < BUSY_THRESHOLD) && (max < BUSY_MAX) && (min > BUSY_MIN))
            res = 1;
    }
    return res;
}
#endif

int cw_dsp_silence(struct cw_dsp *dsp, struct cw_frame *f, int *totalsilence)
{
    int16_t *amp;
    uint8_t *data;
    int len = 0;
    int x = 0;

    if (f->frametype != CW_FRAME_VOICE)
    {
        cw_log(LOG_WARNING, "Can't calculate silence on a non-voice frame\n");
        return 0;
    }
    data = f->data;
    switch (f->subclass)
    {
    case CW_FORMAT_SLINEAR:
        amp = f->data;
        len = f->datalen / sizeof(int16_t);
        break;
    case CW_FORMAT_ULAW:
        amp = alloca(f->datalen*sizeof(int16_t));
        len = f->datalen;
        for (x = 0;  x < len;  x++) 
            amp[x] = CW_MULAW(data[x]);
        break;
    case CW_FORMAT_ALAW:
        amp = alloca(f->datalen*sizeof(int16_t));
        len = f->datalen;
        for (x = 0;  x < len;  x++) 
            amp[x] = CW_ALAW(data[x]);
        break;
    default:
        cw_log(LOG_WARNING, "Silence detection is not supported on codec %s. Use RFC2833\n", cw_getformatname(f->subclass));
        return 0;
    }
    return __cw_dsp_silence(dsp, amp, len, totalsilence);
}

#define FIX_INF(inf) \
do \
{ \
    if (writeback) \
    { \
        switch(inf->subclass) \
        { \
        case CW_FORMAT_ULAW: \
            for (x = 0;  x < len;  x++) \
                odata[x] = CW_LIN2MU(amp[x]); \
            break; \
        case CW_FORMAT_ALAW: \
            for (x = 0;  x < len;  x++) \
                odata[x] = CW_LIN2A(amp[x]); \
            break; \
        } \
    } \
} \
while(0) 

struct cw_frame *cw_dsp_process(struct cw_channel *chan, struct cw_dsp *dsp, struct cw_frame *af)
{
    int silence;
    int res;
    int x;
    int16_t *amp;
    uint8_t *odata;
    int len;
    int writeback = FALSE;
    char digit_buf[10];
    char buf[2];

    if (af == NULL)
        return NULL;
    if (af->frametype != CW_FRAME_VOICE)
        return af;
    odata = af->data;
    len = af->datalen;
    /* Make sure we have short data */
    switch (af->subclass)
    {
    case CW_FORMAT_SLINEAR:
        amp = af->data;
        len = af->datalen/2;
        break;
    case CW_FORMAT_ULAW:
        amp = alloca(af->datalen*sizeof(int16_t));
        for (x = 0;  x < len;  x++) 
            amp[x] = CW_MULAW(odata[x]);
        break;
    case CW_FORMAT_ALAW:
        amp = alloca(af->datalen*sizeof(int16_t));
        for (x = 0;  x < len;  x++) 
            amp[x] = CW_ALAW(odata[x]);
        break;
    default:
        cw_log(LOG_WARNING, "Tone detection is not supported on codec %s. Use RFC2833\n", cw_getformatname(af->subclass));
        return af;
    }
    silence = __cw_dsp_silence(dsp, amp, len, NULL);
    if ((dsp->features & DSP_FEATURE_SILENCE_SUPPRESS)  &&  silence)
    {
        cw_fr_init(&dsp->f);
        dsp->f.frametype = CW_FRAME_NULL;
        return &dsp->f;
    }
    if ((dsp->features & DSP_FEATURE_BUSY_DETECT)  &&  cw_dsp_busydetect(dsp))
    {
        chan->_softhangup |= CW_SOFTHANGUP_DEV;
        cw_fr_init_ex(&dsp->f, CW_FRAME_CONTROL, CW_CONTROL_BUSY, NULL);
        cw_log(LOG_DEBUG, "Requesting Hangup because the busy tone was detected on channel %s\n", chan->name);
        return &dsp->f;
    }
    if ((dsp->features & DSP_FEATURE_DTMF_DETECT))
    {
        if ((dsp->digitmode & DSP_DIGITMODE_MF))
        {
            bell_mf_rx(&dsp->bell_mf_rx, amp, len);
            if (bell_mf_rx_get(&dsp->bell_mf_rx, digit_buf, 1))
            {
                cw_fr_init_ex(&dsp->f, CW_FRAME_DTMF, digit_buf[0], NULL);
                if (chan)
                    cw_queue_frame(chan, af);
                cw_fr_free(af);
                return &dsp->f;
            }
        }
        else
        {
            dtmf_rx(&dsp->dtmf_rx, amp, len);
            /* TODO: Avoid probing inside the DTMF rx object */
            if (dsp->dtmf_rx.in_digit)
                dsp->mute_lag = 5;
            if (dsp->mute_lag  &&  --dsp->mute_lag)
            {
                memset(amp, 0, sizeof(int16_t)*len);
                writeback = TRUE;
            }
            /* TODO: Avoid probing inside the DTMF rx object */
            if ((dsp->digitmode & (DSP_DIGITMODE_MUTECONF | DSP_DIGITMODE_MUTEMAX)))
            {
                if (dsp->thinkdigit == 0)
                {
                    if (dsp->dtmf_rx.last_hit)
                    {
                        /* Looks like we might have something.  
                           Request a conference mute for the moment */
                        dsp->thinkdigit = 'x';
                        cw_fr_init_ex(&dsp->f, CW_FRAME_DTMF, 'm', NULL);
                        FIX_INF(af);
                        if (chan)
                            cw_queue_frame(chan, af);
                        cw_fr_free(af);
                        return &dsp->f;
                    }
                }
                else
                {
                    if (dsp->dtmf_rx.in_digit)
                    {
                        /* Thought we saw one last time. It is now confirmed. */
                        if (dsp->thinkdigit)
                        {
                            if ((dsp->thinkdigit != 'x')  &&  (dsp->thinkdigit != dsp->dtmf_rx.in_digit))
                            {
                                /* If we found a digit, and we're changing digits, go
                                   ahead and send this one, but DON'T stop confmute because
                                   we're detecting something else, too... */
                                cw_fr_init_ex(&dsp->f, CW_FRAME_DTMF, dsp->thinkdigit, NULL);
                                FIX_INF(af);
                                if (chan)
                                    cw_queue_frame(chan, af);
                                cw_fr_free(af);
                            }
                            dsp->thinkdigit = dsp->dtmf_rx.in_digit;
                            return &dsp->f;
                        }
                        dsp->thinkdigit = dsp->dtmf_rx.in_digit;
                    }
                    else
                    {
                        if (dsp->thinkdigit)
                        {
                            cw_fr_init(&dsp->f);
                            if (dsp->thinkdigit != 'x')
                            {
                                /* If we found a digit, send it now */
                                dsp->f.frametype = CW_FRAME_DTMF;
                                dsp->f.subclass = dsp->thinkdigit;
                                dsp->thinkdigit = 0;
                            }
                            else
                            {
                                dsp->f.frametype = CW_FRAME_DTMF;
                                dsp->f.subclass = 'u';
                                dsp->thinkdigit = 0;
                            }
                            FIX_INF(af);
                            if (chan)
                                cw_queue_frame(chan, af);
                            cw_fr_free(af);
                            return &dsp->f;
                        }
                    }
                }
            }
            if (dtmf_rx_get(&dsp->dtmf_rx, buf, 1))
            {
                cw_fr_init_ex(&dsp->f, CW_FRAME_DTMF, buf[0], NULL);
                FIX_INF(af);
                if (chan)
                    cw_queue_frame(chan, af);
                cw_fr_free(af);
                return &dsp->f;
            }
        }
    }
    if ((dsp->features & DSP_FEATURE_FAX_CNG_DETECT))
    {
        modem_connect_tones_rx(&dsp->fax_cng_rx, amp, len);
        if (modem_connect_tones_rx_get(&dsp->fax_cng_rx))
        {
            cw_fr_init_ex(&dsp->f, CW_FRAME_DTMF, 'f', NULL);
            FIX_INF(af);
            if (chan)
                cw_queue_frame(chan, af);
            cw_fr_free(af);
            return &dsp->f;
        }
    }
    if ((dsp->features & DSP_FEATURE_FAX_CED_DETECT))
    {
        modem_connect_tones_rx(&dsp->fax_ced_rx, amp, len);
        if (modem_connect_tones_rx_get(&dsp->fax_ced_rx))
        {
            cw_fr_init_ex(&dsp->f, CW_FRAME_DTMF, 'F', NULL);
            FIX_INF(af);
            if (chan)
                cw_queue_frame(chan, af);
            cw_fr_free(af);
            return &dsp->f;
        }
    }
    if ((dsp->features & DSP_FEATURE_CALL_PROGRESS))
    {
        if ((res = __cw_dsp_call_progress(dsp, amp, len)))
        {
            switch (res)
            {
            case CW_CONTROL_ANSWER:
            case CW_CONTROL_BUSY:
            case CW_CONTROL_RINGING:
            case CW_CONTROL_CONGESTION:
            case CW_CONTROL_HANGUP:
                cw_fr_init_ex(&dsp->f, CW_FRAME_CONTROL, res, "dsp_progress");
                if (chan) 
                    cw_queue_frame(chan, &dsp->f);
                break;
            default:
                cw_log(LOG_WARNING, "Don't know how to represent call progress message %d\n", res);
                break;
            }
        }
    }
    FIX_INF(af);
    return af;
}

static void cw_dsp_prog_reset(struct cw_dsp *dsp)
{
    goertzel_descriptor_t desc;
    int max = 0;
    int x;
    
    dsp->gsamp_size = modes[dsp->progmode].size;
    dsp->gsamps = 0;
    for (x = 0;  x < sizeof(modes[dsp->progmode].freqs)/sizeof(modes[dsp->progmode].freqs[0]);  x++)
    {
        if (modes[dsp->progmode].freqs[x])
        {
            make_goertzel_descriptor(&desc, (float) modes[dsp->progmode].freqs[x], dsp->gsamp_size);
            goertzel_init(&dsp->freqs[x], &desc);
            max = x + 1;
        }
    }
    dsp->freqcount = max;
}

struct cw_dsp *cw_dsp_new(void)
{
    struct cw_dsp *dsp;

    if ((dsp = malloc(sizeof(*dsp))))
    {
        memset(dsp, 0, sizeof(*dsp));
        dsp->threshold = DEFAULT_THRESHOLD;
        dsp->features = DSP_FEATURE_SILENCE_SUPPRESS;
        dsp->busycount = DSP_HISTORY;

        dtmf_rx_init(&dsp->dtmf_rx, NULL, NULL);
        dsp->mute_lag = 0;

        modem_connect_tones_rx_init(&dsp->fax_cng_rx,
                                    MODEM_CONNECT_TONES_FAX_CNG,
                                    NULL,
                                    NULL);
        modem_connect_tones_rx_init(&dsp->fax_ced_rx,
                                    MODEM_CONNECT_TONES_FAX_CED,
                                    NULL,
                                    NULL);

        /* Initialize initial DSP progress detect parameters */
        cw_dsp_prog_reset(dsp);
    }
    return dsp;
}

void cw_dsp_set_features(struct cw_dsp *dsp, int features)
{
    dsp->features = features;
}

void cw_dsp_free(struct cw_dsp *dsp)
{
    free(dsp);
}

void cw_dsp_set_threshold(struct cw_dsp *dsp, int threshold)
{
    dsp->threshold = threshold;
}

void cw_dsp_set_busy_count(struct cw_dsp *dsp, int cadences)
{
    if (cadences < 4)
        cadences = 4;
    if (cadences > DSP_HISTORY)
        cadences = DSP_HISTORY;
    dsp->busycount = cadences;
}

void cw_dsp_set_busy_pattern(struct cw_dsp *dsp, int tonelength, int quietlength)
{
    dsp->busy_tonelength = tonelength;
    dsp->busy_quietlength = quietlength;
    cw_log(LOG_DEBUG, "dsp busy pattern set to %d,%d\n", tonelength, quietlength);
}

void cw_dsp_digitreset(struct cw_dsp *dsp)
{
    dsp->thinkdigit = 0;
    if (dsp->digitmode & DSP_DIGITMODE_MF)
        bell_mf_rx_init(&dsp->bell_mf_rx, NULL, NULL);
    else
        dtmf_rx_init(&dsp->dtmf_rx, NULL, NULL);
    dsp->mute_lag = 0;
    modem_connect_tones_rx_init(&dsp->fax_cng_rx,
                                MODEM_CONNECT_TONES_FAX_CNG,
                                NULL,
                                NULL);
    modem_connect_tones_rx_init(&dsp->fax_ced_rx,
                                MODEM_CONNECT_TONES_FAX_CED,
                                NULL,
                                NULL);
}

void cw_dsp_reset(struct cw_dsp *dsp)
{
    int x;
    
    dsp->totalsilence = 0;
    dsp->gsamps = 0;
    for (x = 0;  x < 4;  x++)
        goertzel_reset(&dsp->freqs[x]);
    memset(dsp->historicsilence, 0, sizeof(dsp->historicsilence));
    memset(dsp->historicnoise, 0, sizeof(dsp->historicnoise));    
}

int cw_dsp_digitmode(struct cw_dsp *dsp, int digitmode)
{
    int new_mode;
    int old_mode;
    
    old_mode = dsp->digitmode & (DSP_DIGITMODE_DTMF | DSP_DIGITMODE_MF | DSP_DIGITMODE_MUTECONF | DSP_DIGITMODE_MUTEMAX);
    new_mode = digitmode & (DSP_DIGITMODE_DTMF | DSP_DIGITMODE_MF | DSP_DIGITMODE_MUTECONF | DSP_DIGITMODE_MUTEMAX);
    if (old_mode != new_mode)
    {
        /* Must initialize structures if switching from MF to DTMF or vice-versa */
        if ((new_mode & DSP_DIGITMODE_MF))
            bell_mf_rx_init(&dsp->bell_mf_rx, NULL, NULL);
        else
            dtmf_rx_init(&dsp->dtmf_rx, NULL, NULL);
        dsp->mute_lag = 0;
        modem_connect_tones_rx_init(&dsp->fax_cng_rx,
                                    MODEM_CONNECT_TONES_FAX_CNG,
                                    NULL,
                                    NULL);
        modem_connect_tones_rx_init(&dsp->fax_ced_rx,
                                    MODEM_CONNECT_TONES_FAX_CED,
                                    NULL,
                                    NULL);
    }
    if ((digitmode & DSP_DIGITMODE_RELAXDTMF))
        dtmf_rx_parms(&dsp->dtmf_rx, FALSE, 8, 8);
    else
        dtmf_rx_parms(&dsp->dtmf_rx, FALSE, 8, 4);
    dsp->digitmode = digitmode;
    return 0;
}

int cw_dsp_set_call_progress_zone(struct cw_dsp *dsp, char *zone)
{
    int x;
    
    for (x = 0;  x < sizeof(aliases)/sizeof(aliases[0]);  x++)
    {
        if (!strcasecmp(aliases[x].name, zone))
        {
            dsp->progmode = aliases[x].mode;
            cw_dsp_prog_reset(dsp);
            return 0;
        }
    }
    return -1;
}
