/*
 * (CAPI*)
 *
 * An implementation of Common ISDN API 2.0 for
 * Asterisk / OpenPBX.org
 *
 * Copyright (C) 2006 Cytronics & Melware
 *
 * Armin Schindler <armin@melware.de>
 * 
 * This program is free software and may be modified and 
 * distributed under the terms of the GNU Public License.
 */
 
#ifndef _PBX_CAPI_RTP_H
#define _PBX_CAPI_RTP_H

/*
 * prototypes
 */
extern int capi_alloc_rtp(struct capi_pvt *i);
extern void voice_over_ip_profile(struct cc_capi_controller *cp);
extern int capi_write_rtp(struct opbx_channel *c, struct opbx_frame *f);
extern struct opbx_frame *capi_read_rtp(struct capi_pvt *i, unsigned char *buf, int len);
extern _cstruct capi_rtp_ncpi(struct capi_pvt *i);

#endif
