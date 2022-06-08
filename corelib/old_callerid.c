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

/*
 *
 * CallerID Generation support 
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <math.h>
#include <ctype.h>
#include <spandsp.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL: svn://svn.openpbx.org/openpbx/trunk/corelib/callerid.c $", "$Revision: 922 $")

#include "openpbx/ulaw.h"
#include "openpbx/alaw.h"
#include "openpbx/frame.h"
#include "openpbx/channel.h"
#include "openpbx/phone_no_utils.h"
#include "openpbx/logger.h"
#include "openpbx/utils.h"
#include "openpbx/old_callerid.h"

struct tdd_state {
    adsi_rx_state_t rx;
    adsi_tx_state_t tx;
    char rx_msg[256];
};

#define PARITY_NONE		0
#define PARITY_EVEN		1
#define PARITY_ODD		2

/* \brief Retrieve a serial byte into outbyte.
   Buffer is a pointer into a series of 
   shorts and len records the number of bytes in the buffer.  len will be 
   overwritten with the number of bytes left that were not consumed, and the
   return value is as follows:
     0: Still looking for something...  
	 1: An output byte was received and stored in outbyte
	 -1: An error occured in the transmission
   He must be called with at least 80 bytes of buffer. */
static int fsk_serie(fsk_data *fskd, short *buffer, int *len, int *outbyte);

struct callerid_state {
    adsi_rx_state_t rx;
    int current_standard;

	fsk_data fskd;
	char rawdata[256];
	short oldstuff[160];
	int oldlen;
	int pos;
	int type;
	int cksum;
	char name[64];
	char number[64];
	int flags;
	int sawflag;
	int len;
};


float cid_dr[4], cid_di[4];
float clidsb = 8000.0/1200.0;

#define CALLERID_SPACE	2200.0		/* 2200 hz for "0" */
#define CALLERID_MARK	1200.0		/* 1200 hz for "1" */
#define SAS_FREQ		 440.0
#define CAS_FREQ1		2130.0
#define CAS_FREQ2		2750.0

#define NBW	2
#define BWLIST	{75,800}
#define	NF	6

#define	FLIST {1400,1800,1200,2200,1300,2100}

#define STATE_SEARCH_STARTBIT	0
#define STATE_SEARCH_STARTBIT2	1
#define STATE_SEARCH_STARTBIT3	2
#define STATE_GET_BYTE			3

static inline float get_sample(short **buffer, int *len)
{
	float retval;
	retval = (float) **buffer / 256;
	(*buffer)++;
	(*len)--;
	return retval;
}

#define GET_SAMPLE get_sample(&buffer, len)

/* Coeficientes para filtros de entrada					*/
/* Tabla de coeficientes, generada a partir del programa "mkfilter"	*/
/* Formato: coef[IDX_FREC][IDX_BW][IDX_COEF]				*/
/* IDX_COEF=0	=>	1/GAIN						*/
/* IDX_COEF=1-6	=>	Coeficientes y[n]				*/

static double coef_in[NF][NBW][8]={
 {  { 1.8229206611e-04,-7.8997325866e-01,2.2401819940e+00,-4.6751353581e+00,5.5080745712e+00,-5.0571565772e+00,2.6215820004e+00,0.0000000000e+00,
 },  { 9.8532175289e-02,-5.6297236492e-02,3.3146713415e-01,-9.2239200436e-01,1.4844365184e+00,-2.0183258642e+00,2.0074154497e+00,0.0000000000e+00,
 },  },  {  { 1.8229206610e-04,-7.8997325866e-01,7.7191410839e-01,-2.8075643964e+00,1.6948618347e+00,-3.0367273700e+00,9.0333559408e-01,0.0000000000e+00,
 },  { 9.8531161839e-02,-5.6297236492e-02,1.1421579050e-01,-4.8122536483e-01,4.0121072432e-01,-7.4834487567e-01,6.9170822332e-01,0.0000000000e+00,
 },  },  {  { 1.8229206611e-04,-7.8997325866e-01,2.9003821430e+00,-6.1082779024e+00,7.7169345751e+00,-6.6075999680e+00,3.3941838836e+00,0.0000000000e+00,
 },  { 9.8539686961e-02,-5.6297236492e-02,4.2915323820e-01,-1.2609358633e+00,2.2399213250e+00,-2.9928879142e+00,2.5990173742e+00,0.0000000000e+00,
 },  },  {  { 1.8229206610e-04,-7.8997325866e-01,-7.7191410839e-01,-2.8075643964e+00,-1.6948618347e+00,-3.0367273700e+00,-9.0333559408e-01,0.0000000000e+00,
 },  { 9.8531161839e-02,-5.6297236492e-02,-1.1421579050e-01,-4.8122536483e-01,-4.0121072432e-01,-7.4834487567e-01,-6.9170822332e-01,0.0000000000e+00,
 },  },  {  { 1.8229206611e-04,-7.8997325866e-01,2.5782298908e+00,-5.3629717478e+00,6.5890882172e+00,-5.8012914776e+00,3.0171839130e+00,0.0000000000e+00,
 },  { 9.8534230718e-02,-5.6297236492e-02,3.8148618075e-01,-1.0848760410e+00,1.8441165168e+00,-2.4860666655e+00,2.3103384142e+00,0.0000000000e+00,
 },  },  {  { 1.8229206610e-04,-7.8997325866e-01,-3.8715051001e-01,-2.6192408538e+00,-8.3977994034e-01,-2.8329897913e+00,-4.5306444352e-01,0.0000000000e+00,
 },  { 9.8531160936e-02,-5.6297236492e-02,-5.7284484199e-02,-4.3673866734e-01,-1.9564766257e-01,-6.2028156584e-01,-3.4692356122e-01,0.0000000000e+00,
 },  }, 
};

/* Coeficientes para filtro de salida					*/
/* Tabla de coeficientes, generada a partir del programa "mkfilter"	*/
/* Formato: coef[IDX_BW][IDX_COEF]					*/
/* IDX_COEF=0	=>	1/GAIN						*/
/* IDX_COEF=1-6	=>	Coeficientes y[n]				*/

static double coef_out[NBW][8]={
 { 1.3868644653e-08,-6.3283665042e-01,4.0895057217e+00,-1.1020074592e+01,1.5850766191e+01,-1.2835109292e+01,5.5477477340e+00,0.0000000000e+00,
 },  { 3.1262119724e-03,-7.8390522307e-03,8.5209627801e-02,-4.0804129163e-01,1.1157139955e+00,-1.8767603680e+00,1.8916395224e+00,0.0000000000e+00 
 }, 
};


extern float cid_dr[4];
extern float cid_di[4];
extern float clidsb;

static inline float callerid_getcarrier(float *cr, float *ci, int bit)
{
	/* Move along.  There's nothing to see here... */
	float t;
	t = *cr * cid_dr[bit] - *ci * cid_di[bit];
	*ci = *cr * cid_di[bit] + *ci * cid_dr[bit];
	*cr = t;
	
	t = 2.0 - (*cr * *cr + *ci * *ci);
	*cr *= t;
	*ci *= t;
	return *cr;
}	

#define PUT_BYTE(a) do { \
	*(buf++) = (a); \
	bytes++; \
} while(0)

#define PUT_AUDIO_SAMPLE(y) do { \
	int index = (short)(rint(8192.0 * (y))); \
	*(buf++) = OPBX_LIN2X(index); \
	bytes++; \
} while(0)
	
#define PUT_CLID_MARKMS do { \
	int x; \
	for (x=0;x<8;x++) \
		PUT_AUDIO_SAMPLE(callerid_getcarrier(&cr, &ci, 1)); \
} while(0)

#define PUT_CLID_BAUD(bit) do { \
	while(scont < clidsb) { \
		PUT_AUDIO_SAMPLE(callerid_getcarrier(&cr, &ci, bit)); \
		scont += 1.0; \
	} \
	scont -= clidsb; \
} while(0)


#define PUT_CLID(byte) do { \
	int z; \
	unsigned char b = (byte); \
	PUT_CLID_BAUD(0); 	/* Start bit */ \
	for (z=0;z<8;z++) { \
		PUT_CLID_BAUD(b & 1); \
		b >>= 1; \
	} \
	PUT_CLID_BAUD(1);	/* Stop bit */ \
} while(0);	

/* Filtro pasa-banda para frecuencia de MARCA */
static inline float filtroM(fsk_data *fskd,float in)
{
	int i,j;
	double s;
	double *pc;
	
	pc=&coef_in[fskd->f_mark_idx][fskd->bw][0];
	fskd->fmxv[(fskd->fmp+6)&7]=in*(*pc++);
	
	s=(fskd->fmxv[(fskd->fmp+6)&7] - fskd->fmxv[fskd->fmp]) + 3 * (fskd->fmxv[(fskd->fmp+2)&7] - fskd->fmxv[(fskd->fmp+4)&7]);
	for (i=0,j=fskd->fmp;i<6;i++,j++) s+=fskd->fmyv[j&7]*(*pc++);
	fskd->fmyv[j&7]=s;
	fskd->fmp++; fskd->fmp&=7;
	return s;
}

/* Filtro pasa-banda para frecuencia de ESPACIO */
static inline float filtroS(fsk_data *fskd,float in)
{
	int i,j;
	double s;
	double *pc;
	
	pc=&coef_in[fskd->f_space_idx][fskd->bw][0];
	fskd->fsxv[(fskd->fsp+6)&7]=in*(*pc++);
	
	s=(fskd->fsxv[(fskd->fsp+6)&7] - fskd->fsxv[fskd->fsp]) + 3 * (fskd->fsxv[(fskd->fsp+2)&7] - fskd->fsxv[(fskd->fsp+4)&7]);
	for (i=0,j=fskd->fsp;i<6;i++,j++) s+=fskd->fsyv[j&7]*(*pc++);
	fskd->fsyv[j&7]=s;
	fskd->fsp++; fskd->fsp&=7;
	return s;
}

/* Filtro pasa-bajos para datos demodulados */
static inline float filtroL(fsk_data *fskd,float in)
{
	int i,j;
	double s;
	double *pc;
	
	pc=&coef_out[fskd->bw][0];
	fskd->flxv[(fskd->flp + 6) & 7]=in * (*pc++); 
	
	s=     (fskd->flxv[fskd->flp]       + fskd->flxv[(fskd->flp+6)&7]) +
	  6  * (fskd->flxv[(fskd->flp+1)&7] + fskd->flxv[(fskd->flp+5)&7]) +
	  15 * (fskd->flxv[(fskd->flp+2)&7] + fskd->flxv[(fskd->flp+4)&7]) +
	  20 *  fskd->flxv[(fskd->flp+3)&7]; 
	
	for (i=0,j=fskd->flp;i<6;i++,j++) s+=fskd->flyv[j&7]*(*pc++);
	fskd->flyv[j&7]=s;
	fskd->flp++; fskd->flp&=7;
	return s;
}

static inline int demodulador(fsk_data *fskd, float *retval, float x)
{
	float xS,xM;

	fskd->cola_in[fskd->pcola]=x;
	
	xS=filtroS(fskd,x);
	xM=filtroM(fskd,x);

	fskd->cola_filtro[fskd->pcola]=xM-xS;

	x=filtroL(fskd,xM*xM - xS*xS);
	
	fskd->cola_demod[fskd->pcola++]=x;
	fskd->pcola &= (NCOLA-1);

	*retval = x;
	return(0);
}

static int get_bit_raw(fsk_data *fskd, short *buffer, int *len)
{
	/* Esta funcion implementa un DPLL para sincronizarse con los bits */
	float x,spb,spb2,ds;
	int f;

	spb=fskd->spb; 
	if (fskd->spb == 7) spb = 8000.0 / 1200.0;
	ds=spb/32.;
	spb2=spb/2.;

	for (f=0;;){
		if (demodulador(fskd,&x, GET_SAMPLE)) return(-1);
		if ((x*fskd->x0)<0) {	/* Transicion */
			if (!f) {
				if (fskd->cont<(spb2)) fskd->cont+=ds; else fskd->cont-=ds;
				f=1;
			}
		}
		fskd->x0=x;
		fskd->cont+=1.;
		if (fskd->cont>spb) {
			fskd->cont-=spb;
			break;
		}
	}
	f=(x>0)?0x80:0;
	return(f);
}

static int fsk_serie(fsk_data *fskd, short *buffer, int *len, int *outbyte)
{
	int a;
	int i,j,n1,r;
	int samples=0;
	int olen;
	switch(fskd->state) {
		/* Pick up where we left off */
	case STATE_SEARCH_STARTBIT2:
		goto search_startbit2;
	case STATE_SEARCH_STARTBIT3:
		goto search_startbit3;
	case STATE_GET_BYTE:
		goto getbyte;
	}
	/* Esperamos bit de start	*/
	do {
/* this was jesus's nice, reasonable, working (at least with RTTY) code
to look for the beginning of the start bit. Unfortunately, since TTY/TDD's
just start sending a start bit with nothing preceding it at the beginning
of a transmission (what a LOSING design), we cant do it this elegantly */
/*
		if (demodulador(zap,&x1)) return(-1);
		for(;;) {
			if (demodulador(zap,&x2)) return(-1);
			if (x1>0 && x2<0) break;
			x1=x2;
		}
*/
/* this is now the imprecise, losing, but functional code to detect the
beginning of a start bit in the TDD sceanario. It just looks for sufficient
level to maybe, perhaps, guess, maybe that its maybe the beginning of
a start bit, perhaps. This whole thing stinks! */
		if (demodulador(fskd,&fskd->x1,GET_SAMPLE)) return(-1);
		samples++;
		for(;;)
		   {
search_startbit2:		   
			if (!*len) {
				fskd->state = STATE_SEARCH_STARTBIT2;
				return 0;
			}
			samples++;
			if (demodulador(fskd,&fskd->x2,GET_SAMPLE)) return(-1);
#if 0
			printf("x2 = %5.5f ", fskd->x2);
#endif			
			if (fskd->x2 < -0.5) break; 
		   }
search_startbit3:		   
		/* Esperamos 0.5 bits antes de usar DPLL */
		i=fskd->spb/2;
		if (*len < i) {
			fskd->state = STATE_SEARCH_STARTBIT3;
			return 0;
		}
		for(;i;i--) { if (demodulador(fskd,&fskd->x1,GET_SAMPLE)) return(-1); 
#if 0
			printf("x1 = %5.5f ", fskd->x1);
#endif			
	samples++; }

		/* x1 debe ser negativo (confirmación del bit de start) */

	} while (fskd->x1>0);
	fskd->state = STATE_GET_BYTE;

getbyte:

	/* Need at least 80 samples (for 1200) or
		1320 (for 45.5) to be sure we'll have a byte */
	if (fskd->nbit < 8) {
		if (*len < 1320)
			return 0;
	} else {
		if (*len < 80)
			return 0;
	}
	/* Leemos ahora los bits de datos */
	j=fskd->nbit;
	for (a=n1=0;j;j--) {
		olen = *len;
		i=get_bit_raw(fskd, buffer, len);
		buffer += (olen - *len);
		if (i == -1) return(-1);
		if (i) n1++;
		a>>=1; a|=i;
	}
	j=8-fskd->nbit;
	a>>=j;

	/* Leemos bit de paridad (si existe) y la comprobamos */
	if (fskd->paridad) {
		olen = *len;
		i=get_bit_raw(fskd, buffer, len); 
		buffer += (olen - *len);
		if (i == -1) return(-1);
		if (i) n1++;
		if (fskd->paridad==1) {	/* paridad=1 (par) */
			if (n1&1) a|=0x100;		/* error */
		} else {			/* paridad=2 (impar) */
			if (!(n1&1)) a|=0x100;	/* error */
		}
	}
	
	/* Leemos bits de STOP. Todos deben ser 1 */
	
	for (j=fskd->nstop;j;j--) {
		r = get_bit_raw(fskd, buffer, len);
		if (r == -1) return(-1);
		if (!r) a|=0x200;
	}

	/* Por fin retornamos  */
	/* Bit 8 : Error de paridad */
	/* Bit 9 : Error de Framming */

	*outbyte = a;
	fskd->state = STATE_SEARCH_STARTBIT;
	return 1;
}

static inline void gen_tones(unsigned char *buf, int len, int codec, float ddr1, float ddi1, float ddr2, float ddi2, float *cr1, float *ci1, float *cr2, float *ci2)
{
	int x;
	float t;
	for (x=0;x<len;x++) {
		t = *cr1 * ddr1 - *ci1 * ddi1;
		*ci1 = *cr1 * ddi1 + *ci1 * ddr1;
		*cr1 = t;
		t = 2.0 - (*cr1 * *cr1 + *ci1 * *ci1);
		*cr1 *= t;
		*ci1 *= t; 	

		t = *cr2 * ddr2 - *ci2 * ddi2;
		*ci2 = *cr2 * ddi2 + *ci2 * ddr2;
		*cr2 = t;
		t = 2.0 - (*cr2 * *cr2 + *ci2 * *ci2);
		*cr2 *= t;
		*ci2 *= t; 	
		buf[x] = OPBX_LIN2X((*cr1 + *cr2) * 2048.0);
	}
}

static inline void gen_tone(unsigned char *buf, int len, int codec, float ddr1, float ddi1, float *cr1, float *ci1)
{
	int x;
	float t;
	for (x=0;x<len;x++) {
		t = *cr1 * ddr1 - *ci1 * ddi1;
		*ci1 = *cr1 * ddi1 + *ci1 * ddr1;
		*cr1 = t;
		t = 2.0 - (*cr1 * *cr1 + *ci1 * *ci1);
		*cr1 *= t;
		*ci1 *= t; 	
		buf[x] = OPBX_LIN2X(*cr1 * 8192.0);
	}
}

int opbx_gen_cas(unsigned char *outbuf, int sendsas, int len, int codec)
{
    uint32_t phase_acc1;
    int32_t phase_rate1;
    uint32_t phase_acc2;
    int32_t phase_rate2;
    int i;
	int pos = 0;
	int saslen = 2400;

	if (sendsas) {
		if (len < saslen)
			return -1;
        phase_rate1 = dds_phase_rate(SAS_FREQ);
        phase_acc1 = 0;
        for (i = 0;  i < saslen;  i++)
            outbuf[i] = OPBX_LIN2X(dds(&phase_acc1, phase_rate1) >> 3);
		pos += saslen;
	}
    phase_rate1 = dds_phase_rate(CAS_FREQ1);
    phase_acc1 = 0;
    phase_rate2 = dds_phase_rate(CAS_FREQ2);
    phase_acc2 = 0;
    for (i = pos;  i < len;  i++)
        outbuf[i] = OPBX_LIN2X((dds(&phase_acc1, phase_rate1) >> 3) + (dds(&phase_acc2, phase_rate2) >> 3));
	return 0;
}

void callerid_init(void)
{
	/* Initialize stuff for inverse FFT */
	cid_dr[0] = cos(CALLERID_SPACE * 2.0 * M_PI / 8000.0);
	cid_di[0] = sin(CALLERID_SPACE * 2.0 * M_PI / 8000.0);
	cid_dr[1] = cos(CALLERID_MARK * 2.0 * M_PI / 8000.0);
	cid_di[1] = sin(CALLERID_MARK * 2.0 * M_PI / 8000.0);
}

static void put_adsi_msg(void *user_data, const uint8_t *msg, int len)
{
    struct callerid_state *cid;
    int l;
    uint8_t field_type;
    const uint8_t *field_body;
    int field_len;
    int message_type;
    uint8_t body[256];
    
    cid = (struct callerid_state *) user_data;
    l = -1;
    message_type = -1;
    do
    {
        l = adsi_next_field(&cid->rx, msg, len, l, &field_type, &field_body, &field_len);
        if (l > 0)
        {
            if (field_body)
            {
                memcpy(body, field_body, field_len);
                body[field_len] = '\0';
                printf("Field type 0x%x, len %d, '%s' - ", field_type, field_len, body);
                switch (cid->current_standard)
                {
                case ADSI_STANDARD_CLASS:
                    switch (message_type)
                    {
                    case CLASS_SDMF_CALLERID:
                        break;
                    case CLASS_MDMF_CALLERID:
                        switch (field_type)
                        {
                        case MCLASS_DATETIME:
                            printf("Date and time (MMDDHHMM)");
                            break;
                        case MCLASS_CALLER_NUMBER:
                            printf("Caller's number");
                            break;
                        case MCLASS_DIALED_NUMBER:
                            printf("Dialed number");
                            break;
                        case MCLASS_ABSENCE1:
                            printf("Caller's number absent: 'O' or 'P'");
                            break;
                        case MCLASS_REDIRECT:
                            printf("Call forward: universal ('0'), on busy ('1'), or on unanswered ('2')");
                            break;
                        case MCLASS_QUALIFIER:
                            printf("Long distance: 'L'");
                            break;
                        case MCLASS_CALLER_NAME:
                            printf("Caller's name");
                            break;
                        case MCLASS_ABSENCE2:
                            printf("Caller's name absent: 'O' or 'P'");
                            break;
                        }
                        break;
                    case CLASS_SDMF_MSG_WAITING:
                        break;
                    case CLASS_MDMF_MSG_WAITING:
                        switch (field_type)
                        {
                        case MCLASS_VISUAL_INDICATOR:
                            printf("Message waiting/not waiting");
                            break;
                        }
                        break;
                    }
                    break;
                case ADSI_STANDARD_CLIP:
                    switch (message_type)
                    {
                    case CLIP_MDMF_CALLERID:
                    case CLIP_MDMF_MSG_WAITING:
                    case CLIP_MDMF_CHARGE_INFO:
                    case CLIP_MDMF_SMS:
                        switch (field_type)
                        {
                        case CLIP_DATETIME:
                            printf("Date and time (MMDDHHMM)");
                            break;
                        case CLIP_CALLER_NUMBER:
                            printf("Caller's number");
                            break;
                        case CLIP_DIALED_NUMBER:
                            printf("Dialed number");
                            break;
                        case CLIP_ABSENCE1:
                            printf("Caller's number absent");
                            break;
                        case CLIP_CALLER_NAME:
                            printf("Caller's name");
                            break;
                        case CLIP_ABSENCE2:
                            printf("Caller's name absent");
                            break;
                        case CLIP_VISUAL_INDICATOR:
                            printf("Visual indicator");
                            break;
                        case CLIP_MESSAGE_ID:
                            printf("Message ID");
                            break;
                        case CLIP_CALLTYPE:
                            printf("Voice call, ring-back-when-free call, or msg waiting call");
                            break;
                        case CLIP_NUM_MSG:
                            printf("Number of messages");
                            break;
#if 0
                        case CLIP_REDIR_NUMBER:
                            printf("Redirecting number");
                            break;
#endif
                        case CLIP_CHARGE:
                            printf("Charge");
                            break;
                        case CLIP_DURATION:
                            printf("Duration of the call");
                            break;
                        case CLIP_ADD_CHARGE:
                            printf("Additional charge");
                            break;
                        case CLIP_DISPLAY_INFO:
                            printf("Display information");
                            break;
                        case CLIP_SERVICE_INFO:
                            printf("Service information");
                            break;
                        }
                        break;
                    }
                    break;
                case ADSI_STANDARD_ACLIP:
                    switch (message_type)
                    {
                    case ACLIP_SDMF_CALLERID:
                        break;
                    case ACLIP_MDMF_CALLERID:
                        switch (field_type)
                        {
                        case ACLIP_DATETIME:
                            printf("Date and time (MMDDHHMM)");
                            break;
                        case ACLIP_CALLER_NUMBER:
                            printf("Caller's number");
                            break;
                        case ACLIP_DIALED_NUMBER:
                            printf("Dialed number");
                            break;
                        case ACLIP_ABSENCE1:
                            printf("Caller's number absent: 'O' or 'P'");
                            break;
                        case ACLIP_REDIRECT:
                            printf("Call forward: universal, on busy, or on unanswered");
                            break;
                        case ACLIP_QUALIFIER:
                            printf("Long distance call: 'L'");
                            break;
                        case ACLIP_CALLER_NAME:
                            printf("Caller's name");
                            break;
                        case ACLIP_ABSENCE2:
                            printf("Caller's name absent: 'O' or 'P'");
                            break;
                        }
                        break;
                    }
                    break;
                case ADSI_STANDARD_JCLIP:
                    switch (message_type)
                    {
                    case JCLIP_MDMF_CALLERID:
                        switch (field_type)
                        {
                        case JCLIP_CALLER_NUMBER:
                            printf("Caller's number");
                            break;
                        case JCLIP_CALLER_NUM_DES:
                            printf("Caller's number data extension signal");
                            break;
                        case JCLIP_DIALED_NUMBER:
                            printf("Dialed number");
                            break;
                        case JCLIP_DIALED_NUM_DES:
                            printf("Dialed number data extension signal");
                            break;
                        case JCLIP_ABSENCE:
                            printf("Caller's number absent: 'C', 'O', 'P' or 'S'");
                            break;
                        }
                        break;
                    }
                    break;
                case ADSI_STANDARD_CLIP_DTMF:
                    switch (message_type)
                    {
                    case CLIP_DTMF_CALLER_NUMBER:
                        printf("Caller's number");
                        break;
                    case CLIP_DTMF_ABSENCE1:
                        printf("Caller's number absent: private (1), overseas (2) or not available (3)");
                        break;
                    }
                    break;
                case ADSI_STANDARD_TDD:
                    break;
                }
            }
            else
            {
                printf("Message type 0x%x - ", field_type);
                message_type = field_type;
                switch (cid->current_standard)
                {
                case ADSI_STANDARD_CLASS:
                    switch (message_type)
                    {
                    case CLASS_SDMF_CALLERID:
                        printf("Single data message caller ID");
                        break;
                    case CLASS_MDMF_CALLERID:
                        printf("Multiple data message caller ID");
                        break;
                    case CLASS_SDMF_MSG_WAITING:
                        printf("Single data message message waiting");
                        break;
                    case CLASS_MDMF_MSG_WAITING:
                        printf("Multiple data message message waiting");
                        break;
                    default:
                        printf("Unknown");
                        break;
                    }
                    break;
                case ADSI_STANDARD_CLIP:
                    switch (message_type)
                    {
                    case CLIP_MDMF_CALLERID:
                        printf("Multiple data message caller ID");
                        break;
                    case CLIP_MDMF_MSG_WAITING:
                        printf("Multiple data message message waiting");
                        break;
                    case CLIP_MDMF_CHARGE_INFO:
                        printf("Multiple data message charge info");
                        break;
                    case CLIP_MDMF_SMS:
                        printf("Multiple data message SMS");
                        break;
                    default:
                        printf("Unknown");
                        break;
                    }
                    break;
                case ADSI_STANDARD_ACLIP:
                    switch (message_type)
                    {
                    case ACLIP_SDMF_CALLERID:
                        printf("Single data message caller ID frame");
                        break;
                    case ACLIP_MDMF_CALLERID:
                        printf("Multiple data message caller ID frame");
                        break;
                    default:
                        printf("Unknown");
                        break;
                    }
                    break;
                case ADSI_STANDARD_JCLIP:
                    switch (message_type)
                    {
                    case JCLIP_MDMF_CALLERID:
                        printf("Multiple data message caller ID frame");
                        break;
                    default:
                        printf("Unknown");
                        break;
                    }
                    break;
                case ADSI_STANDARD_CLIP_DTMF:
                    switch (message_type)
                    {
                    case CLIP_DTMF_CALLER_NUMBER:
                        printf("Caller's number");
                        break;
                    case CLIP_DTMF_ABSENCE1:
                        printf("Caller's number absent");
                        break;
                    default:
                        printf("Unknown");
                        break;
                    }
                    break;
                }
            }
            printf("\n");
        }
    }
    while (l > 0);
}

struct callerid_state *callerid_new(int cid_signalling)
{
	struct callerid_state *cid;
	cid = malloc(sizeof(struct callerid_state));
	if (cid) {
		memset(cid, 0, sizeof(struct callerid_state));
		if (cid_signalling == 2)
            cid->current_standard = ADSI_STANDARD_CLIP;
        else
            cid->current_standard = ADSI_STANDARD_CLASS;
        adsi_rx_init(&cid->rx, cid->current_standard, put_adsi_msg, cid);
		memset(cid->name, 0, sizeof(cid->name));
		memset(cid->number, 0, sizeof(cid->number));
		cid->flags = CID_UNKNOWN_NAME | CID_UNKNOWN_NUMBER;

		cid->fskd.spb = 7;		/* 1200 baud */
		cid->fskd.hdlc = 0;		/* Async */
		cid->fskd.nbit = 8;		/* 8 bits */
		cid->fskd.nstop = 1;	/* 1 stop bit */
		cid->fskd.paridad = 0;	/* No parity */
		cid->fskd.bw=1;			/* Filter 800 Hz */
		if (cid_signalling == 2) { /* v23 signalling */
			cid->fskd.f_mark_idx =  4;	/* 1300 Hz */
			cid->fskd.f_space_idx = 5;	/* 2100 Hz */
		} else { /* Bell 202 signalling as default */ 
			cid->fskd.f_mark_idx =  2;	/* 1200 Hz */
			cid->fskd.f_space_idx = 3;	/* 2200 Hz */
		}
		cid->fskd.pcola = 0;		/* No clue */
		cid->fskd.cont = 0;			/* Digital PLL reset */
		cid->fskd.x0 = 0.0;
		cid->fskd.state = 0;

		cid->pos = 0;
	} else
		opbx_log(LOG_WARNING, "Out of memory\n");
	return cid;
}

void callerid_get(struct callerid_state *cid, char **name, char **number, int *flags)
{
	*flags = cid->flags;
	if (cid->flags & (CID_UNKNOWN_NAME | CID_PRIVATE_NUMBER))
		*name = NULL;
	else
		*name = cid->name;
	if (cid->flags & (CID_UNKNOWN_NUMBER | CID_PRIVATE_NUMBER))
		*number = NULL;
	else
		*number = cid->number;
}

void callerid_get_dtmf(char *cidstring, char *number, int *flags)
{
	int i;
	int code;

	/* "Clear" the number-buffer. */
	number[0] = 0;

	if (strlen(cidstring) < 2) {
		opbx_log(LOG_DEBUG, "No cid detected\n");
		*flags = CID_UNKNOWN_NUMBER;
		return;
	}
	
	/* Detect protocol and special types */
	if (cidstring[0] == 'B') {
		/* Handle special codes */
		code = atoi(&cidstring[1]);
		if (code == 0)
			*flags = CID_UNKNOWN_NUMBER;
		else if (code == 10) 
			*flags = CID_PRIVATE_NUMBER;
		else
			opbx_log(LOG_DEBUG, "Unknown DTMF code %d\n", code);
	} else if (cidstring[0] == 'D' && cidstring[2] == '#') {
		/* .DK special code */
		if (cidstring[1] == '1')
			*flags = CID_PRIVATE_NUMBER;
		if (cidstring[1] == '2' || cidstring[1] == '3')
			*flags = CID_UNKNOWN_NUMBER;
	} else if (cidstring[0] == 'D' || cidstring[0] == 'A') {
		/* "Standard" callerid */
		for (i = 1; i < strlen(cidstring); i++ ) {
			if (cidstring[i] == 'C' || cidstring[i] == '#')
				break;
			if (isdigit(cidstring[i]))
				number[i-1] = cidstring[i];
			else
				opbx_log(LOG_DEBUG, "Unknown CID digit '%c'\n",
					cidstring[i]);
		}
		number[i-1] = 0;
	} else if (isdigit(cidstring[0])) {
		/* It begins with a digit, so we parse it as a number and hope
		 * for the best */
		opbx_log(LOG_WARNING, "Couldn't detect start-character. CID "
			"parsing might be unreliable\n");
		for (i = 0; i < strlen(cidstring); i++) {
			if (isdigit(cidstring[i]))
                                number[i] = cidstring[i];
			else
				break;
		}
		number[i] = 0;
	} else {
		opbx_log(LOG_DEBUG, "Unknown CID protocol, start digit '%c'\n", 
			cidstring[0]);
		*flags = CID_UNKNOWN_NUMBER;
	}
}

int callerid_feed(struct callerid_state *cid, unsigned char *ubuf, int len, int codec)
{
	int mylen = len;
	int olen;
	int b = 'X';
	int res;
	int x;
	short *buf = malloc(2 * len + cid->oldlen);
	short *obuf = buf;
	if (!buf) {
		opbx_log(LOG_WARNING, "Out of memory\n");
		return -1;
	}
	memset(buf, 0, 2 * len + cid->oldlen);
	memcpy(buf, cid->oldstuff, cid->oldlen);
	mylen += cid->oldlen/2;
	for (x=0;x<len;x++) 
		buf[x+cid->oldlen/2] = OPBX_XLAW(ubuf[x]);
	while(mylen >= 160) {
		olen = mylen;
		res = fsk_serie(&cid->fskd, buf, &mylen, &b);
		if (mylen < 0) {
			opbx_log(LOG_ERROR, "fsk_serie made mylen < 0 (%d)\n", mylen);
			free(obuf);
			return -1;
		}
		buf += (olen - mylen);
		if (res < 0) {
			opbx_log(LOG_NOTICE, "fsk_serie failed\n");
			return -1;
		}
		if (res == 1) {
			/* Ignore invalid bytes */
			if (b > 0xff)
				continue;
			switch(cid->sawflag) {
			case 0: /* Look for flag */
				if (b == 'U')
					cid->sawflag = 2;
				break;
			case 2: /* Get lead-in */
				if ((b == 0x04) || (b == 0x80)) {
					cid->type = b;
					cid->sawflag = 3;
					cid->cksum = b;
				}
				break;
			case 3:	/* Get length */
				/* Not a lead in.  We're ready  */
				cid->sawflag = 4;
				cid->len = b;
				cid->pos = 0;
				cid->cksum += b;
				break;
			case 4: /* Retrieve message */
				if (cid->pos >= 128) {
					opbx_log(LOG_WARNING, "Caller ID too long???\n");
					free(obuf);
					return -1;
				}
				cid->rawdata[cid->pos++] = b;
				cid->len--;
				cid->cksum += b;
				if (!cid->len) {
					cid->rawdata[cid->pos] = '\0';
					cid->sawflag = 5;
				}
				break;
			case 5: /* Check checksum */
				if (b != (256 - (cid->cksum & 0xff))) {
					opbx_log(LOG_NOTICE, "Caller*ID failed checksum\n");
					/* Try again */
					cid->sawflag = 0;
					break;
				}
		
				cid->number[0] = '\0';
				cid->name[0] = '\0';
				/* If we get this far we're fine.  */
				if (cid->type == 0x80) {
					/* MDMF */
					/* Go through each element and process */
					for (x=0;x< cid->pos;) {
						switch(cid->rawdata[x++]) {
						case 1:
							/* Date */
							break;
						case 2: /* Number */
						case 3: /* Number (for Zebble) */
						case 4: /* Number */
							res = cid->rawdata[x];
							if (res > 32) {
								opbx_log(LOG_NOTICE, "Truncating long caller ID number from %d bytes to 32\n", cid->rawdata[x]);
								res = 32; 
							}
							if (opbx_strlen_zero(cid->number)) {
								memcpy(cid->number, cid->rawdata + x + 1, res);
								/* Null terminate */
								cid->number[res] = '\0';
							}
							break;
						case 6: /* Stentor Call Qualifier (ie. Long Distance call) */
							break;
						case 7: /* Name */
						case 8: /* Name */
							res = cid->rawdata[x];
							if (res > 32) {
								opbx_log(LOG_NOTICE, "Truncating long caller ID name from %d bytes to 32\n", cid->rawdata[x]);
								res = 32; 
							}
							memcpy(cid->name, cid->rawdata + x + 1, res);
							cid->name[res] = '\0';
							break;
						case 17: /* UK: Call type, 1=Voice Call, 2=Ringback when free, 129=Message waiting  */
						case 19: /* UK: Network message system status (Number of messages waiting) */
						case 22: /* Something French */
							break;
						default:
							opbx_log(LOG_NOTICE, "Unknown IE %d\n", cid->rawdata[x-1]);
						}
						x += cid->rawdata[x];
						x++;
					}
				} else {
					/* SDMF */
					opbx_copy_string(cid->number, cid->rawdata + 8, sizeof(cid->number));
				}
				/* Update flags */
				cid->flags = 0;
				if (!strcmp(cid->number, "P")) {
					strcpy(cid->number, "");
					cid->flags |= CID_PRIVATE_NUMBER;
				} else if (!strcmp(cid->number, "O") || opbx_strlen_zero(cid->number)) {
					strcpy(cid->number, "");
					cid->flags |= CID_UNKNOWN_NUMBER;
				}
				if (!strcmp(cid->name, "P")) {
					strcpy(cid->name, "");
					cid->flags |= CID_PRIVATE_NAME;
				} else if (!strcmp(cid->name, "O") || opbx_strlen_zero(cid->name)) {
					strcpy(cid->name, "");
					cid->flags |= CID_UNKNOWN_NAME;
				}
				free(obuf);
				return 1;
				break;
			default:
				opbx_log(LOG_ERROR, "Dunno what to do with a digit in sawflag %d\n", cid->sawflag);
			}
		}
	}
	if (mylen) {
		memcpy(cid->oldstuff, buf, mylen * 2);
		cid->oldlen = mylen * 2;
	} else
		cid->oldlen = 0;
	free(obuf);
	return 0;
}

void callerid_free(struct callerid_state *cid)
{
	free(cid);
}

static int callerid_genmsg(char *msg, int size, char *number, char *name, int flags)
{
	time_t t;
	struct tm tm;
	char *ptr;
	int res;
	int i;
	int x;

	/* Get the time */
	time(&t);
	localtime_r(&t,&tm);
	
	ptr = msg;
	
	/* Format time and message header */
	res = snprintf(ptr, size, "\001\010%02d%02d%02d%02d", tm.tm_mon + 1,
				tm.tm_mday, tm.tm_hour, tm.tm_min);
	size -= res;
	ptr += res;
	if (!number || opbx_strlen_zero(number) || (flags & CID_UNKNOWN_NUMBER)) {
		/* Indicate number not known */
		res = snprintf(ptr, size, "\004\001O");
		size -= res;
		ptr += res;
	} else if (flags & CID_PRIVATE_NUMBER) {
		/* Indicate number is private */
		res = snprintf(ptr, size, "\004\001P");
		size -= res;
		ptr += res;
	} else {
		/* Send up to 16 digits of number MAX */
		i = strlen(number);
		if (i > 16) i = 16;
		res = snprintf(ptr, size, "\002%c", i);
		size -= res;
		ptr += res;
		for (x = 0;  x < i;  x++)
			ptr[x] = number[x];
		ptr[i] = '\0';
		ptr += i;
		size -= i;
	}

	if (!name || opbx_strlen_zero(name) || (flags & CID_UNKNOWN_NAME)) {
		/* Indicate name not known */
		res = snprintf(ptr, size, "\010\001O");
		size -= res;
		ptr += res;
	} else if (flags & CID_PRIVATE_NAME) {
		/* Indicate name is private */
		res = snprintf(ptr, size, "\010\001P");
		size -= res;
		ptr += res;
	} else {
		/* Send up to 16 digits of name MAX */
		i = strlen(name);
		if (i > 16) i = 16;
		res = snprintf(ptr, size, "\007%c", i);
		size -= res;
		ptr += res;
		for (x=0;x<i;x++)
			ptr[x] = name[x];
		ptr[i] = '\0';
		ptr += i;
		size -= i;
	}
	return (ptr - msg);
}

int mate_generate(uint8_t *buf, char *msg, int codec)
{
	int x;
	int bytes=0;
	float cr = 1.0;
	float ci = 0.0;
	float scont = 0.0;

	for (x = 0;  x < HEADER_MS;  x++) {	/* 50 ms of Mark */
		PUT_CLID_MARKMS;
	}
	/* Put actual message */
	for (x = 0;  msg[x];  x++)  {
		PUT_CLID(msg[x]);
	}
	for (x = 0;  x < TRAILER_MS;  x++) {	/* 5 ms of Mark */
		PUT_CLID_MARKMS;
	}
	return bytes;
}

int adsi_generate(uint8_t *buf, int msgtype, unsigned char *msg, int msglen, int msgnum, int last, int codec)
{
	int sum;
	int x;	
	int bytes=0;
	/* Initial carrier (imaginary) */
	float cr = 1.0;
	float ci = 0.0;
	float scont = 0.0;

	if (msglen > 255)
		msglen = 255;

	/* If first message, Send 150ms of MARK's */
	if (msgnum == 1) {
		for (x=0;x<150;x++)	/* was 150 */
			PUT_CLID_MARKMS;
	}
	/* Put message type */
	PUT_CLID(msgtype);
	sum = msgtype;

	/* Put message length (plus one  for the message number) */
	PUT_CLID(msglen + 1);
	sum += msglen + 1;

	/* Put message number */
	PUT_CLID(msgnum);
	sum += msgnum;

	/* Put actual message */
	for (x=0;x<msglen;x++) {
		PUT_CLID(msg[x]);
		sum += msg[x];
	}

	/* Put 2's compliment of sum */
	PUT_CLID(256-(sum & 0xff));

#if 0
	if (last) {
		/* Put trailing marks */
		for (x=0;x<50;x++)
			PUT_CLID_MARKMS;
	}
#endif
	return bytes;

}

int vmwi_generate(uint8_t *buf, int active, int mdmf, int codec)
{
	unsigned char msg[256];
	int len=0;
	int sum;
	int x;
	int bytes = 0;
	float cr = 1.0;
	float ci = 0.0;
	float scont = 0.0;
	if (mdmf) {
		/* MDMF Message waiting */
		msg[len++] = 0x82;
		/* Length is 3 */
		msg[len++] = 3;
		/* IE is "Message Waiting Parameter" */
		msg[len++] = 0xb;
		/* Length of IE is one */
		msg[len++] = 1;
		/* Active or not */
		if (active)
			msg[len++] = 0xff;
		else
			msg[len++] = 0x00;
	} else {
		/* SDMF Message waiting */
		msg[len++] = 0x6;
		/* Length is 3 */
		msg[len++] = 3;
		if (active) {
			msg[len++] = 0x42;
			msg[len++] = 0x42;
			msg[len++] = 0x42;
		} else {
			msg[len++] = 0x6f;
			msg[len++] = 0x6f;
			msg[len++] = 0x6f;
		}
	}
	sum = 0;
	for (x=0;x<len;x++)
		sum += msg[x];
	sum = (256 - (sum & 255));
	msg[len++] = sum;
	/* Wait a half a second */
	for (x=0;x<4000;x++)
		PUT_BYTE(0x7f);
	/* Transmit 30 0x55's (looks like a square wave) for channel seizure */
	for (x=0;x<30;x++)
		PUT_CLID(0x55);
	/* Send 170ms of callerid marks */
	for (x=0;x<170;x++)
		PUT_CLID_MARKMS;
	for (x=0;x<len;x++) {
		PUT_CLID(msg[x]);
	}
	/* Send 50 more ms of marks */
	for (x=0;x<50;x++)
		PUT_CLID_MARKMS;
	return bytes;
}

int callerid_generate(uint8_t *buf, char *number, char *name, int flags, int callwaiting, int codec)
{
	int bytes=0;
	int x, sum;
	int len;
	/* Initial carriers (real/imaginary) */
	float cr = 1.0;
	float ci = 0.0;
	float scont = 0.0;
	char msg[256];

	len = callerid_genmsg(msg, sizeof(msg), number, name, flags);
	if (!callwaiting) {
		/* Wait a half a second */
		for (x=0;x<4000;x++)
			PUT_BYTE(0x7f);
		/* Transmit 30 0x55's (looks like a square wave) for channel seizure */
		for (x=0;x<30;x++)
			PUT_CLID(0x55);
	}
	/* Send 150ms of callerid marks */
	for (x=0;x<150;x++)
		PUT_CLID_MARKMS;
	/* Send 0x80 indicating MDMF format */
	PUT_CLID(0x80);
	/* Put length of whole message */
	PUT_CLID(len);
	sum = 0x80 + strlen(msg);
	/* Put each character of message and update checksum */
	for (x=0;x<len; x++) {
		PUT_CLID(msg[x]);
		sum += msg[x];
	}
	/* Send 2's compliment of sum */
	PUT_CLID(256 - (sum & 255));

	/* Send 50 more ms of marks */
	for (x=0;x<50;x++)
		PUT_CLID_MARKMS;
	
	return bytes;
}

static int __opbx_callerid_generate(unsigned char *buf, char *name, char *number, int callwaiting, int codec)
{
	if (name && opbx_strlen_zero(name))
		name = NULL;
	if (number && opbx_strlen_zero(number))
		number = NULL;
	return callerid_generate(buf, number, name, 0, callwaiting, codec);
}

int opbx_callerid_generate(unsigned char *buf, char *name, char *number, int codec)
{
	return __opbx_callerid_generate(buf, name, number, 0, codec);
}

int opbx_callerid_callwaiting_generate(unsigned char *buf, char *name, char *number, int codec)
{
	return __opbx_callerid_generate(buf, name, number, 1, codec);
}

int opbx_tdd_gen_ecdisa(int codec, uint8_t *outbuf, int len)
{
    uint32_t phase_acc;
    int32_t phase_rate;
    int i;
    
    phase_rate = dds_phase_rate(2100.0f);
    phase_acc = 0;
    for (i = 0;  i < len;  i++)
        outbuf[i] = OPBX_LIN2MU(dds(&phase_acc, phase_rate) >> 3);
	return 0;
}

int tdd_generate(struct tdd_state *tdd, int codec, uint8_t *buf, const char *str)
{
    int16_t amp[160];
    uint8_t adsi_msg[256 + 42];
    int adsi_msg_len;
    int total_len;
    int j;
    int len;

    adsi_msg_len = adsi_add_field(&tdd->tx, adsi_msg, -1, 0, (uint8_t *) str, strlen(str));
    adsi_put_message(&tdd->tx, adsi_msg, adsi_msg_len);
    total_len = 0;
    while ((len = adsi_tx(&tdd->tx, amp, 160)) > 0)
    {
        for (j = 0;  j < len;  j++)
            buf[total_len++] = OPBX_LIN2MU(amp[j]);
    }
	return total_len;
}

static void put_tdd_msg(void *user_data, const uint8_t *msg, int len)
{
    struct tdd_state *tdd;
    
    tdd = (struct tdd_state *) user_data;
    if (len < 256)
        memcpy(tdd->rx_msg, msg, len);
}

int tdd_feed(struct tdd_state *tdd, int codec, uint8_t *ubuf, int len)
{
    int16_t bufit[160];
    int c;
    int i;
    int j;
    int max;

    for (i = 0;  i < len;  i += 160)
    {
        max = (i + 160 > len)  ?  (len - i)  :  160;
        for (j = 0;  j < max;  j++)
    		bufit[j] = OPBX_MULAW(ubuf[160*i + j]);
        adsi_rx(&tdd->rx, bufit, max);
    }
    if (tdd->rx_msg[0])
    {
        c = tdd->rx_msg[0];
        tdd->rx_msg[0] = '\0';
        return c;
    }
    return 0;
}

struct tdd_state *tdd_new(void)
{
	struct tdd_state *tdd;

	if ((tdd = malloc(sizeof(struct tdd_state)))) {
		memset(tdd, 0, sizeof(struct tdd_state));
		adsi_tx_init(&tdd->tx, ADSI_STANDARD_TDD);
		adsi_rx_init(&tdd->rx, ADSI_STANDARD_TDD, put_tdd_msg, tdd);
	} else
		opbx_log(LOG_WARNING, "Out of memory\n");
	return tdd;
}

void tdd_free(struct tdd_state *tdd)
{
	free(tdd);
}
