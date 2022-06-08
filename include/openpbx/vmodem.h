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
 * \brief Voice Modem Definitions
 */

#ifndef _OPENPBX_VMODEM_H
#define _OPENPBX_VMODEM_H

#include "openpbx/frame.h"
#include "openpbx/channel.h"

#define CHAR_DLE		0x10
#define CHAR_ETX		0x03
#define CHAR_DC4		0x14

#define MODEM_DEV_TELCO		0
#define MODEM_DEV_TELCO_SPK	4
#define MODEM_DEV_SPKRPHONE	6
#define MODEM_DEV_HANDSET	9

#define MODEM_DTMF_NONE		(1 << 0)
#define MODEM_DTMF_AST		(1 << 1)
#define MODEM_DTMF_I4L		(1 << 2)

/* Thirty millisecond sections */
#define MODEM_MAX_LEN		30
#define MODEM_MAX_BUF		MODEM_MAX_LEN * 16

#define OPBX_MAX_INIT_STR	256

struct opbx_modem_pvt;

struct opbx_modem_driver {
	char *name;
	char **idents;
	int formats;
	int fullduplex;
	void (*incusecnt)(void);
	void (*decusecnt)(void);
	char * (*identify)(struct opbx_modem_pvt *);
	int (*init)(struct opbx_modem_pvt *);
	int (*setdev)(struct opbx_modem_pvt *, int dev);
	struct opbx_frame * (*read)(struct opbx_modem_pvt *);
	int (*write)(struct opbx_modem_pvt *, struct opbx_frame *fr);
	int (*dial)(struct opbx_modem_pvt *, char *);
	int (*answer)(struct opbx_modem_pvt *);
	int (*hangup)(struct opbx_modem_pvt *);
	int (*startrec)(struct opbx_modem_pvt *);
	int (*stoprec)(struct opbx_modem_pvt *);
	int (*startpb)(struct opbx_modem_pvt *);
	int (*stoppb)(struct opbx_modem_pvt *);
	int (*setsilence)(struct opbx_modem_pvt *, int onoff);
	int (*dialdigit)(struct opbx_modem_pvt *, char digit);
	struct opbx_modem_driver *next;
};

#define MODEM_MODE_IMMEDIATE 		0
#define MODEM_MODE_WAIT_RING		1
#define MODEM_MODE_WAIT_ANSWER		2

/*! Private data that needs to be filled by modem driver */
struct opbx_modem_pvt {
	/*! Raw file descriptor for this device */
	int fd;							
	/*! FILE * representation of device */
	FILE *f;						
	/*! Channel we belong to, possibly NULL */
	struct opbx_channel *owner;		
	/* Device name */
	char dev[256];					
	/*! Frame */
	struct opbx_frame fr;			
	
	char offset[OPBX_FRIENDLY_OFFSET];
	/*! Outgoing buffer */
	char obuf[MODEM_MAX_BUF];		
	
	int tail;
	/*! Pulse or tone dialling */
	char dialtype;					
	/*! Time to wait for dial timeout */
	char dialtimeout;				
	
	int obuflen;
	/*! Immediate, or wait for an answer */
	int mode;						
	/*! State of modem in miniature */
	int ministate;					
	/*! Digits to strip on outgoing numbers */
	int stripmsd;					
	/*! Is the last thing we saw an escape */
	int escape;					
	/*! flag to say if has caller*id yet*/
	int gotclid;				
	/* Has a fax tone already been handled? */
	int faxhandled;
	/*! ringer timeout */
	int ringt;				
	/*! actual time of last ring */
	time_t lastring;			
	/*! dtmf receive state/data */
	char dtmfrx;				
	
	char context[OPBX_MAX_EXTENSION];
	/*! Multiple Subscriber Number */
	char msn[OPBX_MAX_EXTENSION];	
	/*! Multiple Subscriber Number we listen to (; separated list) */
	char incomingmsn[OPBX_MAX_EXTENSION];	
	/*! Multiple Subscriber Number we accept for outgoing calls (; separated list) */
	char outgoingmsn[OPBX_MAX_EXTENSION];	
	/*! Group(s) we belong to if available */
	opbx_group_t group;
	/*! Caller ID if available */
	char cid_name[OPBX_MAX_EXTENSION];	
	/*! Caller ID if available */
	char cid_num[OPBX_MAX_EXTENSION];	
	/*! DTMF-detection mode (i4l/openpbx) */
	int dtmfmode;
	/*! DTMF-generation mode (i4l (outband) / openpbx (inband) */
	int dtmfmodegen;
	/*! DSP for DTMF detection */
	struct opbx_dsp *dsp;
	/*! Dialed Number if available */
	char dnid[OPBX_MAX_EXTENSION];	
	/*! Modem initialization String */
	char initstr[OPBX_MAX_INIT_STR];	
	/*! default language */
	char language[MAX_LANGUAGE];	
	/*! Static response buffer */
	char response[256];				
	/*! Modem Capability */
	struct opbx_modem_driver *mc;	
	/*! Next channel in list */
	struct opbx_modem_pvt *next;			
};


/*! Register a modem driver */
/*! Register a driver */
extern int opbx_register_modem_driver(struct opbx_modem_driver *mc);

/*! Unregisters a modem driver */
/*! Unregister a driver */
extern int opbx_unregister_modem_driver(struct opbx_modem_driver *mc);

/*! Sends command */
/*! Send the command cmd (length len, or 0 if pure ascii) on modem */
extern int opbx_modem_send(struct opbx_modem_pvt *p, char *cmd, int len);

/*! Waits for result */
/*! Wait for result to occur.  Return non-zero if times out or error, last
   response is stored in p->response  */
extern int opbx_modem_expect(struct opbx_modem_pvt *p,  char *result, int timeout);

/*! Waits for result */
/*! Wait for result to occur.    response is stored in p->response  */
extern int opbx_modem_read_response(struct opbx_modem_pvt *p,  int timeout);

/*! Used to start up the PBX on a RING */
/*! Used by modem drivers to start up the PBX on a RING */
extern struct opbx_channel *opbx_modem_new(struct opbx_modem_pvt *i, int state);

/*! Trim string of trailing stuff */
/*! Trim off trailing mess */
extern void opbx_modem_trim(char *s);

#endif /* _OPENPBX_VMODEM_H */
