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
 * \brief u-Law to Signed linear conversion
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdlib.h>
#include <inttypes.h>
#include <time.h>
#if defined(HAVE_TGMATH_H)
#include <tgmath.h>
#endif
#if defined(HAVE_MATH_H)
#include <math.h>
#endif
#include <spandsp.h>

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/corelib/alaw.c $", "$Revision: 4723 $")

#include "callweaver/alaw.h"

uint8_t __cw_lin2a[8192];
int16_t __cw_alaw[256];

void cw_alaw_init(void)
{
	int i;
	
    /* Set up A-law conversion table */
	for (i = 0;  i < 256;  i++)
		__cw_alaw[i] = alaw_to_linear(i);
	/* Set up the reverse (A-law) conversion table */
	for(i = -32768; i < 32768; i++)
		__cw_lin2a[((unsigned short)i) >> 3] = linear_to_alaw(i);
}

