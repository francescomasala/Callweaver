/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * Goertzel routines are borrowed from Steve Underwood's tremendous work on the
 * DTMF detector.
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
 * \brief Convenience Signal Processing routines
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

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/frame.h"
#include "openpbx/channel.h"
#include "openpbx/logger.h"
#include "openpbx/dsp.h"
#include "openpbx/ulaw.h"
#include "openpbx/alaw.h"

/* Number of goertzels for progress detect */
#define GSAMP_SIZE_NA 183			/* North America - 350, 440, 480, 620, 950, 1400, 1800 Hz */
#define GSAMP_SIZE_CR 188			/* Costa Rica, Brazil - Only care about 425 Hz */
#define GSAMP_SIZE_UK 160			/* UK disconnect goertzel feed - should trigger 400hz */

#define PROG_MODE_NA		0
#define PROG_MODE_CR		1	
#define PROG_MODE_UK		2	

/* For US modes */
#define HZ_350  0
#define HZ_440  1
#define HZ_480  2
#define HZ_620  3
#define HZ_950  4
#define HZ_1400 5
#define HZ_1800 6

/* For CR/BR modes */
#define HZ_425	0

/* For UK mode */
#define HZ_400	0

static struct progalias {
	char *name;
	int mode;
} aliases[] = {
	{ "us", PROG_MODE_NA },
	{ "ca", PROG_MODE_NA },
	{ "cr", PROG_MODE_CR },
	{ "br", PROG_MODE_CR },
	{ "uk", PROG_MODE_UK },
};

static struct progress {
	int size;
	int freqs[7];
} modes[] = {
	{ GSAMP_SIZE_NA, { 350, 440, 480, 620, 950, 1400, 1800 } },	/* North America */
	{ GSAMP_SIZE_CR, { 425 } },
	{ GSAMP_SIZE_UK, { 400 } },
};

#define DEFAULT_THRESHOLD	512

#define BUSY_PERCENT		10      /* The percentage difference between the two last silence periods */
#define BUSY_PAT_PERCENT	7       /* The percentage difference between measured and actual pattern */
#define BUSY_THRESHOLD		100     /* Max number of ms difference between max and min times in busy */
#define BUSY_MIN            75      /* Busy must be at least 80 ms in half-cadence */
#define BUSY_MAX            3100    /* Busy can't be longer than 3100 ms in half-cadence */

/* Remember last 15 units */
#define DSP_HISTORY 		15

/* Define if you want the fax detector -- NOT RECOMMENDED IN -STABLE */
#define FAX_DETECT

#define TONE_THRESH	        10.0    /* How much louder the tone should be than channel energy */
#define TONE_MIN_THRESH 	1e8	    /* How much tone there should be at least to attempt */
#define COUNT_THRESH		3	    /* Need at least 50ms of stuff to count it */
#define UK_HANGUP_THRESH	60	    /* This is the threshold for the UK */


#define	MAX_DTMF_DIGITS		128

/* Basic DTMF specs:
 *
 * Minimum tone on = 40ms
 * Minimum tone off = 50ms
 * Maximum digit rate = 10 per second
 * Normal twist <= 8dB accepted
 * Reverse twist <= 4dB accepted
 * S/N >= 15dB will detect OK
 * Attenuation <= 26dB will detect OK
 * Frequency tolerance +- 1.5% will detect, +-3.5% will reject
 */

#define DTMF_THRESHOLD		    8.0e7
#define FAX_THRESHOLD		    7.0e7
#define DTMF_NORMAL_TWIST	    6.3     /* 8dB */
#ifdef	RADIO_RELAX
#define DTMF_REVERSE_TWIST      ((digitmode & DSP_DIGITMODE_RELAXDTMF) ? 6.3 : 2.5)     /* 8dB relaxed, 4dB normal */
#else
#define DTMF_REVERSE_TWIST      ((digitmode & DSP_DIGITMODE_RELAXDTMF) ? 4.0 : 2.5)     /* 6dB relaxed, 4dB normal */
#endif
#define DTMF_RELATIVE_PEAK_ROW	6.3     /* 8dB */
#define DTMF_RELATIVE_PEAK_COL	6.3     /* 8dB */
#define DTMF_TO_TOTAL_ENERGY	42.0

#if !defined(BUSYDETECT_MARTIN)  &&  !defined(BUSYDETECT)  &&  !defined(BUSYDETECT_TONEONLY)  &&  !defined(BUSYDETECT_COMPARE_TONE_AND_SILENCE)
#define BUSYDETECT_MARTIN
#endif

typedef struct
{
	goertzel_state_t row_out[4];
	goertzel_state_t col_out[4];
	int hits[3];
	int mhit;
	float energy;
	int current_sample;

	char digits[MAX_DTMF_DIGITS + 1];
	
	int current_digits;
	int detected_digits;
	int lost_digits;
	int digit_hits[16];
#ifdef FAX_DETECT
	goertzel_state_t fax_tone;
	int fax_hits;
#endif
} dtmf_detect_state_t;

static float dtmf_row[] =
{
	697.0,  770.0,  852.0,  941.0
};
static float dtmf_col[] =
{
	1209.0, 1336.0, 1477.0, 1633.0
};

#ifdef FAX_DETECT
static float fax_freq = 1100.0;
#endif

static char dtmf_positions[] = "123A" "456B" "789C" "*0#D";

struct opbx_dsp
{
	struct opbx_frame f;
	int threshold;
	int totalsilence;
	int totalnoise;
	int features;
	int busymaybe;
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
	float genergy;
	union
    {
		dtmf_detect_state_t dtmf;
		bell_mf_rx_state_t bell_mf;
	} td;
};

static void opbx_dtmf_detect_init(dtmf_detect_state_t *s)
{
	int i;
    goertzel_descriptor_t desc;

	s->hits[0] = s->hits[1] = s->hits[2] = 0;
	for (i = 0;  i < 4;  i++)
    {
		make_goertzel_descriptor(&desc, dtmf_row[i], 102);
		goertzel_init(&s->row_out[i], &desc);
		make_goertzel_descriptor(&desc, dtmf_col[i], 102);
		goertzel_init(&s->col_out[i], &desc);
		s->energy = 0.0;
	}
#ifdef FAX_DETECT
	/* Same for the fax detector */
	make_goertzel_descriptor(&desc, fax_freq, 102);
	goertzel_init(&s->fax_tone, &desc);
#endif /* FAX_DETECT */
	s->current_sample = 0;
	s->detected_digits = 0;
	s->current_digits = 0;
	memset(&s->digits, 0, sizeof(s->digits));
	s->lost_digits = 0;
	s->digits[0] = '\0';
}

static int dtmf_detect(dtmf_detect_state_t *s, int16_t amp[], int samples, 
		 int digitmode, int *writeback, int faxdetect)
{
	float row_energy[4];
	float col_energy[4];
#ifdef FAX_DETECT
	float fax_energy;
#endif /* FAX_DETECT */
	float famp;
	float v1;
	int i;
	int j;
	int sample;
	int best_row;
	int best_col;
	int hit;
	int limit;

	hit = 0;
	for (sample = 0;  sample < samples;  sample = limit)
    {
		/* 102 is optimised to meet the DTMF specs. */
		if ((samples - sample) >= (102 - s->current_sample))
			limit = sample + (102 - s->current_sample);
		else
			limit = samples;
		/* The following unrolled loop takes only 35% (rough estimate) of the 
		   time of a rolled loop on the machine on which it was developed */
		for (j = sample;  j < limit;  j++)
        {
			famp = amp[j];
			s->energy += famp*famp;
			/* With GCC 2.95, the following unrolled code seems to take about 35%
			   (rough estimate) as long as a neat little 0-3 loop */
			v1 = s->row_out[0].v2;
			s->row_out[0].v2 = s->row_out[0].v3;
			s->row_out[0].v3 = s->row_out[0].fac*s->row_out[0].v2 - v1 + famp;
			v1 = s->col_out[0].v2;
			s->col_out[0].v2 = s->col_out[0].v3;
			s->col_out[0].v3 = s->col_out[0].fac*s->col_out[0].v2 - v1 + famp;
			v1 = s->row_out[1].v2;
			s->row_out[1].v2 = s->row_out[1].v3;
			s->row_out[1].v3 = s->row_out[1].fac*s->row_out[1].v2 - v1 + famp;
			v1 = s->col_out[1].v2;
			s->col_out[1].v2 = s->col_out[1].v3;
			s->col_out[1].v3 = s->col_out[1].fac*s->col_out[1].v2 - v1 + famp;
			v1 = s->row_out[2].v2;
			s->row_out[2].v2 = s->row_out[2].v3;
			s->row_out[2].v3 = s->row_out[2].fac*s->row_out[2].v2 - v1 + famp;
			v1 = s->col_out[2].v2;
			s->col_out[2].v2 = s->col_out[2].v3;
			s->col_out[2].v3 = s->col_out[2].fac*s->col_out[2].v2 - v1 + famp;
			v1 = s->row_out[3].v2;
			s->row_out[3].v2 = s->row_out[3].v3;
			s->row_out[3].v3 = s->row_out[3].fac*s->row_out[3].v2 - v1 + famp;
			v1 = s->col_out[3].v2;
			s->col_out[3].v2 = s->col_out[3].v3;
			s->col_out[3].v3 = s->col_out[3].fac*s->col_out[3].v2 - v1 + famp;
#ifdef FAX_DETECT
			/* Update fax tone */
			v1 = s->fax_tone.v2;
			s->fax_tone.v2 = s->fax_tone.v3;
			s->fax_tone.v3 = s->fax_tone.fac*s->fax_tone.v2 - v1 + famp;
#endif /* FAX_DETECT */
		}
		s->current_sample += (limit - sample);
		if (s->current_sample < 102)
        {
			if (hit && !((digitmode & DSP_DIGITMODE_NOQUELCH)))
            {
				/* If we had a hit last time, go ahead and clear this out since likely it
				   will be another hit */
				for (i = sample;  i < limit;  i++) 
					amp[i] = 0;
				*writeback = 1;
			}
			continue;
		}
#ifdef FAX_DETECT
		/* Detect the fax energy, too */
		fax_energy = goertzel_result(&s->fax_tone);
#endif
		/* We are at the end of a DTMF detection block */
		/* Find the peak row and the peak column */
		row_energy[0] = goertzel_result(&s->row_out[0]);
		col_energy[0] = goertzel_result(&s->col_out[0]);

		for (best_row = best_col = 0, i = 1;  i < 4;  i++) {
			row_energy[i] = goertzel_result(&s->row_out[i]);
			if (row_energy[i] > row_energy[best_row])
				best_row = i;
			col_energy[i] = goertzel_result(&s->col_out[i]);
			if (col_energy[i] > col_energy[best_col])
				best_col = i;
		}
		hit = 0;
		/* Basic signal level test and the twist test */
		if (row_energy[best_row] >= DTMF_THRESHOLD && 
		    col_energy[best_col] >= DTMF_THRESHOLD &&
		    col_energy[best_col] < row_energy[best_row]*DTMF_REVERSE_TWIST &&
		    col_energy[best_col]*DTMF_NORMAL_TWIST > row_energy[best_row]) {
			/* Relative peak test */
			for (i = 0;  i < 4;  i++)
            {
				if ((i != best_col
                    &&
				    col_energy[i]*DTMF_RELATIVE_PEAK_COL > col_energy[best_col]) ||
				    (i != best_row 
				     && row_energy[i]*DTMF_RELATIVE_PEAK_ROW > row_energy[best_row]))
                {
					break;
				}
			}
			/* ... and fraction of total energy test */
			if (i >= 4
                &&
			    (row_energy[best_row] + col_energy[best_col]) > DTMF_TO_TOTAL_ENERGY*s->energy)
            {
				/* Got a hit */
				hit = dtmf_positions[(best_row << 2) + best_col];
				if (!(digitmode & DSP_DIGITMODE_NOQUELCH))
                {
					/* Zero out frame data if this is part DTMF */
					for (i = sample;  i < limit;  i++) 
						amp[i] = 0;
					*writeback = 1;
				}
				/* Look for two successive similar results */
				/* The logic in the next test is:
				   We need two successive identical clean detects, with
				   something different preceeding it. This can work with
				   back to back differing digits. More importantly, it
				   can work with nasty phones that give a very wobbly start
				   to a digit */
				if (hit == s->hits[2]  &&  hit != s->hits[1]  &&  hit != s->hits[0])
                {
					s->mhit = hit;
					s->digit_hits[(best_row << 2) + best_col]++;
					s->detected_digits++;
					if (s->current_digits < MAX_DTMF_DIGITS)
                    {
						s->digits[s->current_digits++] = hit;
						s->digits[s->current_digits] = '\0';
					}
                    else
                    {
						s->lost_digits++;
					}
				}
			}
		} 
#ifdef FAX_DETECT
		if (!hit && (fax_energy >= FAX_THRESHOLD) && 
			(fax_energy >= DTMF_TO_TOTAL_ENERGY*s->energy) &&
			(faxdetect))
        {
			/* XXX Probably need better checking than just this the energy XXX */
			hit = 'f';
			s->fax_hits++;
		}
        else
        {
			if (s->fax_hits > 5)
            {
				hit = 'f';
				s->mhit = 'f';
				s->detected_digits++;
				if (s->current_digits < MAX_DTMF_DIGITS)
                {
					s->digits[s->current_digits++] = hit;
					s->digits[s->current_digits] = '\0';
				}
                else
                {
					s->lost_digits++;
				}
			}
			s->fax_hits = 0;
		}
#endif /* FAX_DETECT */
		s->hits[0] = s->hits[1];
		s->hits[1] = s->hits[2];
		s->hits[2] = hit;
		/* Reinitialise the detector for the next block */
		for (i = 0;  i < 4;  i++)
        {
			goertzel_reset(&s->row_out[i]);
			goertzel_reset(&s->col_out[i]);
		}
#ifdef FAX_DETECT
		goertzel_reset(&s->fax_tone);
#endif
		s->energy = 0.0;
		s->current_sample = 0;
	}
	if ((!s->mhit) || (s->mhit != hit))
    {
		s->mhit = 0;
		return  0;
	}
	return hit;
}

static int __opbx_dsp_digitdetect(struct opbx_dsp *dsp, int16_t amp[], int len, int *writeback)
{
	int res;
    char buf[2];
	
	if (dsp->digitmode & DSP_DIGITMODE_MF)
    {
        /* This is kinda dirty, since it assumes a maximum of one digit detected per block. In
           practice this should be OK. */
		bell_mf_rx(&dsp->td.bell_mf, amp, len);
		bell_mf_rx_get(&dsp->td.bell_mf, buf, 1);
		res = buf[0];
	}
    else
    {
		res = dtmf_detect(&dsp->td.dtmf, amp, len, dsp->digitmode & DSP_DIGITMODE_RELAXDTMF, writeback, dsp->features & DSP_FEATURE_FAX_DETECT);
	}
	if (res)
    {
		memset(amp, 0, sizeof(int16_t)*len);
        *writeback = TRUE;
	}
    return res;
}

int opbx_dsp_digitdetect(struct opbx_dsp *dsp, struct opbx_frame *inf)
{
	short *s;
	int len;
	int ign=0;

	if (inf->frametype != OPBX_FRAME_VOICE)
    {
		opbx_log(LOG_WARNING, "Can't check call progress of non-voice frames\n");
		return 0;
	}
	if (inf->subclass != OPBX_FORMAT_SLINEAR)
    {
		opbx_log(LOG_WARNING, "Can only check call progress in signed-linear frames\n");
		return 0;
	}
	s = inf->data;
	len = inf->datalen / 2;
	return __opbx_dsp_digitdetect(dsp, s, len, &ign);
}

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
	if ((p1 < i1) || (p1 < i2) || (p1 < e))
		return 0;
	/* And second */
	if ((p2 < i1) || (p2 < i2) || (p2 < e))
		return 0;
	/* Guess it's there... */
	return 1;
}

int opbx_dsp_getdigits(struct opbx_dsp *dsp, char *buf, int max)
{
	if (dsp->digitmode & DSP_DIGITMODE_MF)
		return bell_mf_rx_get(&dsp->td.bell_mf, buf, max);
	if (max > dsp->td.dtmf.current_digits)
		max = dsp->td.dtmf.current_digits;
	if (max > 0)
    {
		memcpy (buf, dsp->td.dtmf.digits, max);
		memmove (dsp->td.dtmf.digits, dsp->td.dtmf.digits + max, dsp->td.dtmf.current_digits - max);
		dsp->td.dtmf.current_digits -= max;
	}
	buf[max] = '\0';
	return  max;
}

static int __opbx_dsp_call_progress(struct opbx_dsp *dsp, short *s, int len)
{
	int x;
	int y;
	int pass;
	int newstate = DSP_TONE_STATE_SILENCE;
	int res = 0;
	int thresh = (dsp->progmode == PROG_MODE_UK) ? UK_HANGUP_THRESH : COUNT_THRESH;

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
					newstate = DSP_TONE_STATE_SILENCE;
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
				opbx_log(LOG_WARNING, "Can't process in unknown prog mode '%d'\n", dsp->progmode);
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
						res = OPBX_CONTROL_BUSY;
						dsp->features &= ~DSP_FEATURE_CALL_PROGRESS;
					}
                    else if ((dsp->features & DSP_PROGRESS_TALK)
                             && 
						     dsp->tstate == DSP_TONE_STATE_TALKING)
                    {
						res = OPBX_CONTROL_ANSWER;
						dsp->features &= ~DSP_FEATURE_CALL_PROGRESS;
					}
                    else if ((dsp->features & DSP_PROGRESS_RINGING)
                             && 
						     dsp->tstate == DSP_TONE_STATE_RINGING)
					{
                    	res = OPBX_CONTROL_RINGING;
					}
                    else if ((dsp->features & DSP_PROGRESS_CONGESTION)
                             && 
						     dsp->tstate == DSP_TONE_STATE_SPECIAL3)
                    {
						res = OPBX_CONTROL_CONGESTION;
						dsp->features &= ~DSP_FEATURE_CALL_PROGRESS;
					}
                    else if ((dsp->features & DSP_FEATURE_CALL_PROGRESS)
                             &&
					         dsp->tstate == DSP_TONE_STATE_HUNGUP)
                    {
						res = OPBX_CONTROL_HANGUP;
						dsp->features &= ~DSP_FEATURE_CALL_PROGRESS;
					}
				}
			}
            else
            {
#if 0
				printf("Newstate: %d\n", newstate);
#endif
				dsp->tstate = newstate;
				dsp->tcount = 1;
			}
			
			/* Reset goertzel */						
			for (x = 0;  x < 7;  x++)
				goertzel_reset(&dsp->freqs[x]);
			dsp->gsamps = 0;
			dsp->genergy = 0.0;
		}
	}
#if 0
	if (res)
		printf("Returning %d\n", res);
#endif		
	return res;
}

int opbx_dsp_call_progress(struct opbx_dsp *dsp, struct opbx_frame *inf)
{
	if (inf->frametype != OPBX_FRAME_VOICE)
    {
		opbx_log(LOG_WARNING, "Can't check call progress of non-voice frames\n");
		return 0;
	}
	if (inf->subclass != OPBX_FORMAT_SLINEAR)
    {
		opbx_log(LOG_WARNING, "Can only check call progress in signed-linear frames\n");
		return 0;
	}
	return __opbx_dsp_call_progress(dsp, inf->data, inf->datalen / 2);
}

static int __opbx_dsp_silence(struct opbx_dsp *dsp, short *s, int len, int *totalsilence)
{
	int accum;
	int x;
	int res = 0;

	if (!len)
		return 0;
	accum = 0;
	for (x = 0;  x < len;  x++) 
		accum += abs(s[x]);
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
/* we don't want to check for busydetect that frequently */
#if 0
			dsp->busymaybe = 1;
#endif
		}
		dsp->totalnoise = 0;
		res = 1;
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
            {
				if (silence1 + silence1*BUSY_PERCENT/100 >= silence2)
					dsp->busymaybe = 1;
				else 
					dsp->busymaybe = 0;
			}
            else
            {
				if (silence1 - silence1*BUSY_PERCENT/100 <= silence2)
					dsp->busymaybe = 1;
				else 
					dsp->busymaybe = 0;
			}
		}
		dsp->totalsilence = 0;
	}
	if (totalsilence)
		*totalsilence = dsp->totalsilence;
	return res;
}

#ifdef BUSYDETECT_MARTIN
int opbx_dsp_busydetect(struct opbx_dsp *dsp)
{
	int res = 0, x;
#ifndef BUSYDETECT_TONEONLY
	int avgsilence = 0, hitsilence = 0;
#endif
	int avgtone = 0, hittone = 0;

	if (!dsp->busymaybe)
		return res;
	for (x = DSP_HISTORY - dsp->busycount;  x < DSP_HISTORY;  x++)
    {
#ifndef BUSYDETECT_TONEONLY
		avgsilence += dsp->historicsilence[x];
#endif
		avgtone += dsp->historicnoise[x];
	}
#ifndef BUSYDETECT_TONEONLY
	avgsilence /= dsp->busycount;
#endif
	avgtone /= dsp->busycount;
	for (x = DSP_HISTORY - dsp->busycount;  x < DSP_HISTORY;  x++)
    {
#ifndef BUSYDETECT_TONEONLY
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
#endif
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
#ifndef BUSYDETECT_TONEONLY
	if ((hittone >= dsp->busycount - 1) && (hitsilence >= dsp->busycount - 1) && 
	    (avgtone >= BUSY_MIN && avgtone <= BUSY_MAX) && 
	    (avgsilence >= BUSY_MIN && avgsilence <= BUSY_MAX))
    {
#else
	if ((hittone >= dsp->busycount - 1) && (avgtone >= BUSY_MIN && avgtone <= BUSY_MAX))
    {
#endif
#ifdef BUSYDETECT_COMPARE_TONE_AND_SILENCE
#ifdef BUSYDETECT_TONEONLY
#error You cant use BUSYDETECT_TONEONLY together with BUSYDETECT_COMPARE_TONE_AND_SILENCE
#endif
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
        {
#if 0
			opbx_log(LOG_NOTICE, "busy detector: avgtone of %d not close enough to desired %d\n",
						avgtone, dsp->busy_tonelength);
#endif
			res = 0;
		}
	}
	/* If we know the expected busy tone silent-period length, check we are in the range */
	if (res && (dsp->busy_quietlength > 0))
    {
		if (abs(avgsilence - dsp->busy_quietlength) > (dsp->busy_quietlength*BUSY_PAT_PERCENT/100))
        {
#if 0
			opbx_log(LOG_NOTICE, "busy detector: avgsilence of %d not close enough to desired %d\n",
						avgsilence, dsp->busy_quietlength);
#endif
			res = 0;
		}
	}
#if 1
	if (res)
		opbx_log(LOG_DEBUG, "opbx_dsp_busydetect detected busy, avgtone: %d, avgsilence %d\n", avgtone, avgsilence);
#endif
	return res;
}
#endif

#ifdef BUSYDETECT
int opbx_dsp_busydetect(struct opbx_dsp *dsp)
{
	int x;
	int res = 0;
	int max, min;

#if 0
	if (dsp->busy_hits > 5);
	return 0;
#endif
	if (dsp->busymaybe)
    {
#if 0
		printf("Maybe busy!\n");
#endif		
		dsp->busymaybe = 0;
		min = 9999;
		max = 0;
		for (x = DSP_HISTORY - dsp->busycount;  x < DSP_HISTORY;  x++)
        {
#if 0
			printf("Silence: %d, Noise: %d\n", dsp->historicsilence[x], dsp->historicnoise[x]);
#endif			
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
        {
#if 0
			printf("Busy!\n");
#endif			
			res = 1;
		}
#if 0
		printf("Min: %d, max: %d\n", min, max);
#endif		
	}
	return res;
}
#endif

int opbx_dsp_silence(struct opbx_dsp *dsp, struct opbx_frame *f, int *totalsilence)
{
	short *s;
	int len;
	
	if (f->frametype != OPBX_FRAME_VOICE)
    {
		opbx_log(LOG_WARNING, "Can't calculate silence on a non-voice frame\n");
		return 0;
	}
	if (f->subclass != OPBX_FORMAT_SLINEAR)
    {
		opbx_log(LOG_WARNING, "Can only calculate silence on signed-linear frames :(\n");
		return 0;
	}
	s = f->data;
	len = f->datalen/2;
	return __opbx_dsp_silence(dsp, s, len, totalsilence);
}

struct opbx_frame *opbx_dsp_process(struct opbx_channel *chan, struct opbx_dsp *dsp, struct opbx_frame *af)
{
	int silence;
	int res;
	int digit;
	int x;
	int16_t *shortdata;
	unsigned char *odata;
	int len;
	int writeback = 0;
    char digit_buf[10];

#define FIX_INF(inf) do { \
		if (writeback) { \
			switch(inf->subclass) { \
			case OPBX_FORMAT_SLINEAR: \
				break; \
			case OPBX_FORMAT_ULAW: \
				for (x=0;x<len;x++) \
					odata[x] = OPBX_LIN2MU((unsigned short)shortdata[x]); \
				break; \
			case OPBX_FORMAT_ALAW: \
				for (x=0;x<len;x++) \
					odata[x] = OPBX_LIN2A((unsigned short)shortdata[x]); \
				break; \
			} \
		} \
	} while(0) 

	if (!af)
		return NULL;
	if (af->frametype != OPBX_FRAME_VOICE)
		return af;
	odata = af->data;
	len = af->datalen;
	/* Make sure we have short data */
	switch (af->subclass)
    {
	case OPBX_FORMAT_SLINEAR:
		shortdata = af->data;
		len = af->datalen / 2;
		break;
	case OPBX_FORMAT_ULAW:
		shortdata = alloca(af->datalen*sizeof(int16_t));
		if (!shortdata) {
			opbx_log(LOG_WARNING, "Unable to allocate stack space for data: %s\n", strerror(errno));
			return af;
		}
		for (x = 0;  x < len;  x++) 
			shortdata[x] = OPBX_MULAW(odata[x]);
		break;
	case OPBX_FORMAT_ALAW:
		shortdata = alloca(af->datalen*sizeof(int16_t));
		if (!shortdata)
        {
			opbx_log(LOG_WARNING, "Unable to allocate stack space for data: %s\n", strerror(errno));
			return af;
		}
		for (x = 0;  x < len;  x++) 
			shortdata[x] = OPBX_ALAW(odata[x]);
		break;
	default:
		opbx_log(LOG_WARNING, "Inband DTMF is not supported on codec %s. Use RFC2833\n", opbx_getformatname(af->subclass));
		return af;
	}
	silence = __opbx_dsp_silence(dsp, shortdata, len, NULL);
	if ((dsp->features & DSP_FEATURE_SILENCE_SUPPRESS) && silence)
    {
		memset(&dsp->f, 0, sizeof(dsp->f));
		dsp->f.frametype = OPBX_FRAME_NULL;
		return &dsp->f;
	}
	if ((dsp->features & DSP_FEATURE_BUSY_DETECT) && opbx_dsp_busydetect(dsp))
    {
		chan->_softhangup |= OPBX_SOFTHANGUP_DEV;
		memset(&dsp->f, 0, sizeof(dsp->f));
		dsp->f.frametype = OPBX_FRAME_CONTROL;
		dsp->f.subclass = OPBX_CONTROL_BUSY;
		opbx_log(LOG_DEBUG, "Requesting Hangup because the busy tone was detected on channel %s\n", chan->name);
		return &dsp->f;
	}
	if ((dsp->features & DSP_FEATURE_DTMF_DETECT))
    {
		digit = __opbx_dsp_digitdetect(dsp, shortdata, len, &writeback);
		if (dsp->digitmode & (DSP_DIGITMODE_MUTECONF | DSP_DIGITMODE_MUTEMAX))
        {
			if (!dsp->thinkdigit)
            {
				if (digit)
                {
					/* Looks like we might have something.  
					 * Request a conference mute for the moment */
					memset(&dsp->f, 0, sizeof(dsp->f));
					dsp->f.frametype = OPBX_FRAME_DTMF;
					dsp->f.subclass = 'm';
					dsp->thinkdigit = 'x';
					FIX_INF(af);
					if (chan)
						opbx_queue_frame(chan, af);
					opbx_frfree(af);
					return &dsp->f;
				}
			}
            else
            {
				if (digit)
                {
					/* Thought we saw one last time.  Pretty sure we really have now */
					if (dsp->thinkdigit)
                    {
						if ((dsp->thinkdigit != 'x') && (dsp->thinkdigit != digit))
                        {
							/* If we found a digit, and we're changing digits, go
							   ahead and send this one, but DON'T stop confmute because
							   we're detecting something else, too... */
							memset(&dsp->f, 0, sizeof(dsp->f));
							dsp->f.frametype = OPBX_FRAME_DTMF;
							dsp->f.subclass = dsp->thinkdigit;
							FIX_INF(af);
							if (chan)
								opbx_queue_frame(chan, af);
							opbx_frfree(af);
						}
						dsp->thinkdigit = digit;
						return &dsp->f;
					}
					dsp->thinkdigit = digit;
				}
                else
                {
					if (dsp->thinkdigit)
                    {
						memset(&dsp->f, 0, sizeof(dsp->f));
						if (dsp->thinkdigit != 'x')
                        {
							/* If we found a digit, send it now */
							dsp->f.frametype = OPBX_FRAME_DTMF;
							dsp->f.subclass = dsp->thinkdigit;
							dsp->thinkdigit = 0;
						}
                        else
                        {
							dsp->f.frametype = OPBX_FRAME_DTMF;
							dsp->f.subclass = 'u';
							dsp->thinkdigit = 0;
						}
						FIX_INF(af);
						if (chan)
							opbx_queue_frame(chan, af);
						opbx_frfree(af);
						return &dsp->f;
					}
				}
			}
		}
        else if (!digit)
        {
			/* Only check when there is *not* a hit... */
			if (dsp->digitmode & DSP_DIGITMODE_MF)
            {
                if (bell_mf_rx_get(&dsp->td.bell_mf, digit_buf, 1))
                {
					memset(&dsp->f, 0, sizeof(dsp->f));
					dsp->f.frametype = OPBX_FRAME_DTMF;
					dsp->f.subclass = digit_buf[0];
					FIX_INF(af);
					if (chan)
						opbx_queue_frame(chan, af);
					opbx_frfree(af);
					return &dsp->f;
				}
			}
            else
            {
				if (dsp->td.dtmf.current_digits)
                {
					memset(&dsp->f, 0, sizeof(dsp->f));
					dsp->f.frametype = OPBX_FRAME_DTMF;
					dsp->f.subclass = dsp->td.dtmf.digits[0];
					memmove(dsp->td.dtmf.digits, dsp->td.dtmf.digits + 1, dsp->td.dtmf.current_digits);
					dsp->td.dtmf.current_digits--;
					FIX_INF(af);
					if (chan)
						opbx_queue_frame(chan, af);
					opbx_frfree(af);
					return &dsp->f;
				}
			}
		}
	}
	if ((dsp->features & DSP_FEATURE_CALL_PROGRESS))
    {
		res = __opbx_dsp_call_progress(dsp, shortdata, len);
		if (res)
        {
			switch (res)
            {
			case OPBX_CONTROL_ANSWER:
			case OPBX_CONTROL_BUSY:
			case OPBX_CONTROL_RINGING:
			case OPBX_CONTROL_CONGESTION:
			case OPBX_CONTROL_HANGUP:
				memset(&dsp->f, 0, sizeof(dsp->f));
				dsp->f.frametype = OPBX_FRAME_CONTROL;
				dsp->f.subclass = res;
				dsp->f.src = "dsp_progress";
				if (chan) 
					opbx_queue_frame(chan, &dsp->f);
				break;
			default:
				opbx_log(LOG_WARNING, "Don't know how to represent call progress message %d\n", res);
                break;
			}
		}
	}
	FIX_INF(af);
	return af;
}

static void opbx_dsp_prog_reset(struct opbx_dsp *dsp)
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

struct opbx_dsp *opbx_dsp_new(void)
{
	struct opbx_dsp *dsp;

	if ((dsp = malloc(sizeof(struct opbx_dsp))))
    {
		memset(dsp, 0, sizeof(*dsp));
		dsp->threshold = DEFAULT_THRESHOLD;
		dsp->features = DSP_FEATURE_SILENCE_SUPPRESS;
		dsp->busycount = DSP_HISTORY;
		/* Initialize DTMF detector */
		opbx_dtmf_detect_init(&dsp->td.dtmf);
		/* Initialize initial DSP progress detect parameters */
		opbx_dsp_prog_reset(dsp);
	}
	return dsp;
}

void opbx_dsp_set_features(struct opbx_dsp *dsp, int features)
{
	dsp->features = features;
}

void opbx_dsp_free(struct opbx_dsp *dsp)
{
	free(dsp);
}

void opbx_dsp_set_threshold(struct opbx_dsp *dsp, int threshold)
{
	dsp->threshold = threshold;
}

void opbx_dsp_set_busy_count(struct opbx_dsp *dsp, int cadences)
{
	if (cadences < 4)
		cadences = 4;
	if (cadences > DSP_HISTORY)
		cadences = DSP_HISTORY;
	dsp->busycount = cadences;
}

void opbx_dsp_set_busy_pattern(struct opbx_dsp *dsp, int tonelength, int quietlength)
{
	dsp->busy_tonelength = tonelength;
	dsp->busy_quietlength = quietlength;
	opbx_log(LOG_DEBUG, "dsp busy pattern set to %d,%d\n", tonelength, quietlength);
}

void opbx_dsp_digitreset(struct opbx_dsp *dsp)
{
	int i;
	
	dsp->thinkdigit = 0;
	if (dsp->digitmode & DSP_DIGITMODE_MF)
    {
		bell_mf_rx_init(&dsp->td.bell_mf, NULL, NULL);
	}
    else
    {
		memset(dsp->td.dtmf.digits, 0, sizeof(dsp->td.dtmf.digits));
		dsp->td.dtmf.current_digits = 0;
		/* Reinitialise the detector for the next block */
		for (i = 0;  i < 4;  i++)
        {
			goertzel_reset(&dsp->td.dtmf.row_out[i]);
			goertzel_reset(&dsp->td.dtmf.col_out[i]);
		}
#ifdef FAX_DETECT
		goertzel_reset(&dsp->td.dtmf.fax_tone);
#endif
		dsp->td.dtmf.hits[2] = dsp->td.dtmf.hits[1] = dsp->td.dtmf.hits[0] =  dsp->td.dtmf.mhit = 0;
		dsp->td.dtmf.energy = 0.0;
		dsp->td.dtmf.current_sample = 0;
	}
}

void opbx_dsp_reset(struct opbx_dsp *dsp)
{
	int x;
	
	dsp->totalsilence = 0;
	dsp->gsamps = 0;
	for (x = 0;  x < 4;  x++)
		goertzel_reset(&dsp->freqs[x]);
	memset(dsp->historicsilence, 0, sizeof(dsp->historicsilence));
	memset(dsp->historicnoise, 0, sizeof(dsp->historicnoise));	
}

int opbx_dsp_digitmode(struct opbx_dsp *dsp, int digitmode)
{
	int new;
	int old;
	
	old = dsp->digitmode & (DSP_DIGITMODE_DTMF | DSP_DIGITMODE_MF | DSP_DIGITMODE_MUTECONF | DSP_DIGITMODE_MUTEMAX);
	new = digitmode & (DSP_DIGITMODE_DTMF | DSP_DIGITMODE_MF | DSP_DIGITMODE_MUTECONF | DSP_DIGITMODE_MUTEMAX);
	if (old != new)
    {
		/* Must initialize structures if switching from MF to DTMF or vice-versa */
		if (new & DSP_DIGITMODE_MF)
			bell_mf_rx_init(&dsp->td.bell_mf, NULL, NULL);
		else
			opbx_dtmf_detect_init(&dsp->td.dtmf);
	}
	dsp->digitmode = digitmode;
	return 0;
}

int opbx_dsp_set_call_progress_zone(struct opbx_dsp *dsp, char *zone)
{
	int x;
	
	for (x = 0;  x < sizeof(aliases)/sizeof(aliases[0]);  x++)
    {
		if (!strcasecmp(aliases[x].name, zone))
        {
			dsp->progmode = aliases[x].mode;
			opbx_dsp_prog_reset(dsp);
			return 0;
		}
	}
	return -1;
}

