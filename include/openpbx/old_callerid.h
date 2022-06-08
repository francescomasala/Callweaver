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
 * CallerID (and other GR30) Generation support 
 * Includes code and algorithms from the Zapata library.
 */

#ifndef _OPENPBX_OLD_CALLERID_H
#define _OPENPBX_OLD_CALLERID_H

#define NCOLA 0x4000

typedef struct {
	float spb;	/* Samples / Bit */
	int nbit;	/* Number of Data Bits (5,7,8) */
	float nstop;	/* Number of Stop Bits 1,1.5,2  */
	int paridad;	/* Parity 0=none 1=even 2=odd */
	int hdlc;	/* Modo Packet */
	float x0;
	float x1;
	float x2;
	float cont;
	int bw;		/* Ancho de Banda */
	double fmxv[8],fmyv[8];	/* filter stuff for M filter */
	int	fmp;		/* pointer for M filter */
	double fsxv[8],fsyv[8];	/* filter stuff for S filter */
	int	fsp;		/* pointer for S filter */
	double flxv[8],flyv[8];	/* filter stuff for L filter */
	int	flp;		/* pointer for L filter */
	int f_mark_idx;	/* Indice de frecuencia de marca (f_M-500)/5 */
	int f_space_idx;/* Indice de frecuencia de espacio (f_S-500)/5 */
	int state;
	int pcola;	/* Puntero de las colas de datos */
	float cola_in[NCOLA];		/* Cola de muestras de entrada */
	float cola_filtro[NCOLA];	/* Cola de muestras tras filtros */
	float cola_demod[NCOLA];	/* Cola de muestras demoduladas */
} fsk_data;

#define MAX_CALLERID_SIZE 32000

#define CID_PRIVATE_NAME 		(1 << 0)
#define CID_PRIVATE_NUMBER		(1 << 1)
#define CID_UNKNOWN_NAME		(1 << 2)
#define CID_UNKNOWN_NUMBER		(1 << 3)

#define CID_SIG_BELL	1
#define CID_SIG_V23	2
#define CID_SIG_DTMF	3

#define CID_START_RING	1
#define CID_START_POLARITY 2


#define OPBX_LIN2X(a) ((codec == OPBX_FORMAT_ALAW) ? (OPBX_LIN2A(a)) : (OPBX_LIN2MU(a)))
#define OPBX_XLAW(a) ((codec == OPBX_FORMAT_ALAW) ? (OPBX_ALAW(a)) : (OPBX_MULAW(a)))

#define	END_SILENCE_LEN 400
#define	HEADER_MS 50
#define	TRAILER_MS 5
#define	HEADER_LEN ((HEADER_MS + TRAILER_MS) * 8)
#define	ASCII_BYTES_PER_CHAR 80

struct callerid_state;
typedef struct callerid_state CIDSTATE;

/*! CallerID Initialization */
/*!
 * Initializes the callerid system.  Mostly stuff for inverse FFT
 */
extern void callerid_init(void);

/*! Generates a CallerID FSK stream in ulaw format suitable for transmission. */
/*!
 * \param buf Buffer to use. If "buf" is supplied, it will use that buffer instead of allocating its own.  "buf" must be at least 32000 bytes in size of you want to be sure you don't have an overrun.
 * \param number Use NULL for no number or "P" for "private"
 * \param name name to be used
 * \param callwaiting callwaiting flag
 * \param codec -- either OPBX_FORMAT_ULAW or OPBX_FORMAT_ALAW
 * This function creates a stream of callerid (a callerid spill) data in ulaw format. It returns the size
 * (in bytes) of the data (if it returns a size of 0, there is probably an error)
*/
extern int callerid_generate(unsigned char *buf, char *number, char *name, int flags, int callwaiting, int codec);

/*! Create a callerID state machine */
/*!
 * \param cid_signalling Type of signalling in use
 *
 * This function returns a malloc'd instance of the callerid_state data structure.
 * Returns a pointer to a malloc'd callerid_state structure, or NULL on error.
 */
extern struct callerid_state *callerid_new(int cid_signalling);

/*! Read samples into the state machine. */
/*!
 * \param cid Which state machine to act upon
 * \param buffer containing your samples
 * \param samples number of samples contained within the buffer.
 * \param codec which codec (OPBX_FORMAT_ALAW or OPBX_FORMAT_ULAW)
 *
 * Send received audio to the Caller*ID demodulator.
 * Returns -1 on error, 0 for "needs more samples", 
 * and 1 if the CallerID spill reception is complete.
 */
extern int callerid_feed(struct callerid_state *cid, unsigned char *ubuf, int samples, int codec);

/*! Extract info out of callerID state machine.  Flags are listed above */
/*!
 * \param cid Callerid state machine to act upon
 * \param number Pass the address of a pointer-to-char (will contain the phone number)
 * \param name Pass the address of a pointer-to-char (will contain the name)
 * \param flags Pass the address of an int variable(will contain the various callerid flags)
 *
 * This function extracts a callerid string out of a callerid_state state machine.
 * If no number is found, *number will be set to NULL.  Likewise for the name.
 * Flags can contain any of the following:
 * 
 * Returns nothing.
 */
void callerid_get(struct callerid_state *cid, char **number, char **name, int *flags);

/*! Get and parse DTMF-based callerid  */
/*!
 * \param cidstring The actual transmitted string.
 * \param number The cid number is returned here.
 * \param flags The cid flags are returned here.
 * This function parses DTMF callerid.
 */
void callerid_get_dtmf(char *cidstring, char *number, int *flags);

/*! Free a callerID state */
/*!
 * \param cid This is the callerid_state state machine to free
 * This function frees callerid_state cid.
 */
extern void callerid_free(struct callerid_state *cid);

/*! Generate Caller-ID spill from the "callerid" field of openpbx (in e-mail address like format) */
/*!
 * \param buf buffer for output samples. See callerid_generate() for details regarding buffer.
 * \param astcid OpenPBX format callerid string, taken from the callerid field of openpbx.
 * \param codec OpenPBX codec (either OPBX_FORMAT_ALAW or OPBX_FORMAT_ULAW)
 *
 * Acts like callerid_generate except uses an openpbx format callerid string.
 */
extern int opbx_callerid_generate(uint8_t *buf, char *name, char *number, int codec);

/*! Generate message waiting indicator  */
extern int vmwi_generate(uint8_t *buf, int active, int mdmf, int codec);

extern int adsi_generate(uint8_t *buf, int msgtype, unsigned char *msg, int msglen, int msgnum, int last, int codec);

extern int mate_generate(uint8_t *buf, char *msg, int codec);

/*! Generate Caller-ID spill but in a format suitable for Call Waiting(tm)'s Caller*ID(tm) */
/*!
 * See opbx_callerid_generate for other details
 */
extern int opbx_callerid_callwaiting_generate(uint8_t *buf, char *name, char *number, int codec);

/*! Generate a CAS (CPE Alert Signal) tone for 'n' samples */
/*!
 * \param outbuf Allocated buffer for data.  Must be at least 2400 bytes unless no SAS is desired
 * \param sas Non-zero if CAS should be preceeded by SAS
 * \param len How many samples to generate.
 * \param codec Which codec (OPBX_FORMAT_ALAW or OPBX_FORMAT_ULAW)
 * Returns -1 on error (if len is less than 2400), 0 on success.
 */
extern int opbx_gen_cas(unsigned char *outbuf, int sas, int len, int codec);

/*
 * Caller*ID and other GR-30 compatible generation
 * routines (used by ADSI for example)
 */

#define	TDD_SAMPLES_PER_CHAR	2700

struct tdd_state;

int opbx_tdd_gen_ecdisa(int codec, uint8_t *outbuf, int len);

int tdd_generate(struct tdd_state *tdd, int codec, uint8_t *buf, const char *str);

int tdd_feed(struct tdd_state *tdd, int codec, uint8_t *ubuf, int len);

struct tdd_state *tdd_new(void);

void tdd_free(struct tdd_state *tdd);

#endif /* _OPENPBX_OLD_CALLERID_H */
