/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * See http://www.openpbx.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif  

#include <stdio.h>
#include <openpbx/crypto.h>


/* Hrm, I wonder if the compiler is smart enough to only create two functions
   for all these...  I could force it to only make two, but those would be some
   really nasty looking casts. */

static struct opbx_key *stub_opbx_key_get(const char *kname, int ktype)
{
	opbx_log(LOG_NOTICE, "Crypto support not loaded!\n");
	return NULL;
}

static int stub_opbx_check_signature(struct opbx_key *key, const char *msg, const char *sig)
{
	opbx_log(LOG_NOTICE, "Crypto support not loaded!\n");
	return -1;
}

static int stub_opbx_check_signature_bin(struct opbx_key *key, const char *msg, int msglen, const unsigned char *sig)
{
	opbx_log(LOG_NOTICE, "Crypto support not loaded!\n");
	return -1;
}

static int stub_opbx_sign(struct opbx_key *key, char *msg, char *sig) 
{
	opbx_log(LOG_NOTICE, "Crypto support not loaded!\n");
	return -1;
}

static int stub_opbx_sign_bin(struct opbx_key *key, const char *msg, int msglen, unsigned char *sig)
{
	opbx_log(LOG_NOTICE, "Crypto support not loaded!\n");
	return -1;
}

static int stub_opbx_encdec_bin(unsigned char *dst, const unsigned char *src, int srclen, struct opbx_key *key)
{
	opbx_log(LOG_NOTICE, "Crypto support not loaded!\n");
	return -1;
}

struct opbx_key *(*opbx_key_get)(const char *key, int type) = 
	stub_opbx_key_get;

int (*opbx_check_signature)(struct opbx_key *key, const char *msg, const char *sig) =
	stub_opbx_check_signature;
	
int (*opbx_check_signature_bin)(struct opbx_key *key, const char *msg, int msglen, const unsigned char *sig) =
	stub_opbx_check_signature_bin;
	
int (*opbx_sign)(struct opbx_key *key, char *msg, char *sig) = 
	stub_opbx_sign;

int (*opbx_sign_bin)(struct opbx_key *key, const char *msg, int msglen, unsigned char *sig) =
	stub_opbx_sign_bin;
	
int (*opbx_encrypt_bin)(unsigned char *dst, const unsigned char *src, int srclen, struct opbx_key *key) =
	stub_opbx_encdec_bin;

int (*opbx_decrypt_bin)(unsigned char *dst, const unsigned char *src, int srclen, struct opbx_key *key) =
	stub_opbx_encdec_bin;
