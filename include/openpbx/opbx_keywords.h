/* API to use string hashes for keywords in place of strcmp()
 *
 *  opbx_keywords.h
 *  OpenPBX Keywords
 *
 * Keyword hash codes
 *
 * Author: Benjamin Kowarsch <benjamin at sunrise dash tel dot com>
 *
 * (C) 2006 Sunrise Telephone Systems Ltd. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software. Where software
 * packages making use of this software have a command line interface, the
 * above copyright notice and a reference to the license terms must be
 * displayed whenever the command line interface is invoked. Where software
 * packages making use of this software have a graphical user interface, the
 * above copyright notice and a reference to the license terms must be
 * displayed in the application's "about this software" window or equivalent.
 * Installers which install this software must display the above copyright
 * notice and these license terms in full.
 *
 * Under no circumstances is it permitted for any licensee to take credit for
 * the creation of this software, to claim authorship or ownership in this
 * software or in any other way give the impression that they have created
 * this software. Credit to the true authors and rights holders of this
 * software is absolutely mandatory and must be given at all times.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * In countries and territories where the above no-warranty disclaimer is
 * not permissible by applicable law, the following terms apply:
 *
 * NO PERMISSION TO USE THE SOFTWARE IS GRANTED AND THE SOFTWARE MUST NOT BE
 * USED AT ALL IN SUCH COUNTRIES AND TERRITORIES WHERE THE ABOVE NO-WARRANTY
 * DISCLAIMER IS NOT PERMISSIBLE AND INVALIDATED BY APPLICABLE LAW. HOWEVER,
 * THE COPYRIGHT HOLDERS HEREBY WAIVE THEIR RIGHT TO PURSUE OFFENDERS AS LONG
 * AS THEY OTHERWISE ABIDE BY THE TERMS OF THE LICENSE AS APPLICABLE FOR USE
 * OF THE SOFTWARE IN COUNTRIES AND TERRITORIES WHERE THE ABOVE NO-WARRANTY
 * DISCLAIMER IS PERMITTED BY APPLICABLE LAW. THIS WAIVER DOES NOT CONSTITUTE
 * A LICENSE TO USE THE SOFTWARE IN COUNTRIES AND TERRITORIES WHERE THE ABOVE
 * NO-WARRANTY DISCLAIMER IS NOT PERMISSIBLE AND INVALIDATED BY APPLICABLE
 * LAW. ANY LIABILITY OF ANY KIND IS CATEGORICALLY RULED OUT AT ALL TIMES.
 */


#ifndef _OPBX_KEYWORDS_H
#define _OPBX_KEYWORDS_H


// ---------------------------------------------------------------------------
// Built-in channel variable names used in pbx.c
// ---------------------------------------------------------------------------
//
// UPPERCASE only. Use function opbx_hash_string() to obtain a hash value for
// case sensitive comparisons, use function opbx_hash_string_toupper() to
// obtain a hash value for case insensitive comparisons.
//

#define OPBX_KEYWORD_ANSWERED			0x2146E99D
#define OPBX_KEYWORD_CALLERID			0x00E173C6
#define OPBX_KEYWORD_CALLERIDNUM		0x2AA80F40
#define OPBX_KEYWORD_CALLERIDNAME		0x04C28AF1
#define OPBX_KEYWORD_CALLERANI			0x275B0471
#define OPBX_KEYWORD_CALLINGPRES		0x53038954
#define OPBX_KEYWORD_CALLINGANI2		0x172910FA
#define OPBX_KEYWORD_CALLINGTON			0x16FA640F
#define OPBX_KEYWORD_CALLINGTNS			0x16F963D5
#define OPBX_KEYWORD_DIALSTATUS			0x3D7F96A2
#define OPBX_KEYWORD_DNID				0x02813E45
#define OPBX_KEYWORD_HINT				0x3A2B3507
#define OPBX_KEYWORD_HINTNAME			0x47AF6B32
#define OPBX_KEYWORD_EXTEN				0x76AF848A
#define OPBX_KEYWORD_RDNIS				0x4437ACA6
#define OPBX_KEYWORD_CONTEXT			0x7D8222EF
#define OPBX_KEYWORD_PRIORITY			0x63565B44
#define OPBX_KEYWORD_CHANNEL			0x5C7D4983
#define OPBX_KEYWORD_UNIQUEID			0x4B6D096C
#define OPBX_KEYWORD_HANGUPCAUSE		0x3926FB5C
#define OPBX_KEYWORD_NEWDESTNUM			0x275D7824
#define OPBX_KEYWORD_ACCOUNTCODE		0x047D129A
#define OPBX_KEYWORD_LANGUAGE			0x75479ED8


// ---------------------------------------------------------------------------
// Values for the DIALSTATUS channel variable
// ---------------------------------------------------------------------------
//
// UPPERCASE only. Use function opbx_hash_string() to obtain a hash value for
// case sensitive comparisons, use function opbx_hash_string_toupper() to
// obtain a hash value for case insensitive comparisons.
//

#define OPBX_KEYWORD_ANSWER				0x141BA75E
#define OPBX_KEYWORD_BARRED				0x064A173E
#define OPBX_KEYWORD_BUSY				0x28F00BD9
#define OPBX_KEYWORD_CANCEL				0x5AF84CFA
#define OPBX_KEYWORD_CHANUNAVAIL		0x61B1E464
#define OPBX_KEYWORD_CONGESTION			0x0C09C847
#define OPBX_KEYWORD_NOANSWER			0x5ABF695F
#define OPBX_KEYWORD_NUMBERCHANGED		0x3A9A762B
#define OPBX_KEYWORD_REJECTED			0x03B1B8FE
#define OPBX_KEYWORD_UNALLOCATED		0x4920440E


// ---------------------------------------------------------------------------
// Built-in global variable names used in pbx.c
// ---------------------------------------------------------------------------
//
// UPPERCASE only. Use function opbx_hash_string() to obtain a hash value for
// case sensitive comparisons, use function opbx_hash_string_toupper() to
// obtain a hash value for case insensitive comparisons.
//

#define OPBX_KEYWORD_EPOCH				0x0000B089
#define OPBX_KEYWORD_DATETIME			0x2F5B471B
#define OPBX_KEYWORD_TIMESTAMP			0x138C4996


// ---------------------------------------------------------------------------
// Modifiers used in pbx.c
// ---------------------------------------------------------------------------
//
// UPPERCASE only. Use function opbx_hash_string() to obtain a hash value for
// case sensitive comparisons, use function opbx_hash_string_toupper() to
// obtain a hash value for case insensitive comparisons.
//

#define OPBX_KEYWORD_SKIP				0x3AED4AFF
#define OPBX_KEYWORD_NOANSWER			0x5ABF695F
#define OPBX_KEYWORD_BYEXTENSION		0x3A9C6B28


#endif

// END OF FILE
