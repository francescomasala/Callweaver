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
 * \brief A-Law to Signed linear conversion
 */

#ifndef _OPENPBX_ALAW_H
#define _OPENPBX_ALAW_H

/*! Init the ulaw conversion stuff */
/*!
 * To init the ulaw to slinear conversion stuff, this needs to be run.
 */
extern void opbx_alaw_init(void);

/*! converts signed linear to alaw */
extern uint8_t __opbx_lin2a[8192];

/*! converts alaw to signed linear */
extern int16_t __opbx_alaw[256];

#define OPBX_LIN2A(a) (__opbx_lin2a[((unsigned short)(a)) >> 3])
#define OPBX_ALAW(a) (__opbx_alaw[(int)(a)])

#endif /* _OPENPBX_ALAW_H */
