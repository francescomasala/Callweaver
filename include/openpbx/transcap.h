/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Matthew Fredrickson <creslin@digium.com>
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
 * \brief General OpenPBX channel transcoding definitions.
 */

#ifndef _OPENPBX_TRANSCAP_H
#define _OPENPBX_TRANSCAP_H

/* These definitions are taken directly out of libpri.h and used here.
 * DO NOT change them as it will cause unexpected behavior in channels
 * that utilize these fields.
 */

#define OPBX_TRANS_CAP_SPEECH				0x0
#define OPBX_TRANS_CAP_DIGITAL				0x08
#define OPBX_TRANS_CAP_RESTRICTED_DIGITAL		0x09
#define OPBX_TRANS_CAP_3_1K_AUDIO			0x10
#define OPBX_TRANS_CAP_7K_AUDIO				0x11	/* Depriciated ITU Q.931 (05/1998)*/
#define OPBX_TRANS_CAP_DIGITAL_W_TONES			0x11
#define OPBX_TRANS_CAP_VIDEO				0x18

#define IS_DIGITAL(cap)\
	(cap) & OPBX_TRANS_CAP_DIGITAL ? 1 : 0

#endif /* _OPENPBX_TRANSCAP_H */
