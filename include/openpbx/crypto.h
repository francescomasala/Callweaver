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
 * \brief Provide cryptographic signature routines
 */

#ifndef _OPENPBX_CRYPTO_H
#define _OPENPBX_CRYPTO_H

#include "openpbx/channel.h"
#include "openpbx/file.h"

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#define OPBX_KEY_PUBLIC	(1 << 0)
#define OPBX_KEY_PRIVATE	(1 << 1)

struct opbx_key;

/*! \brief Retrieve a key 
 * \param name of the key we are retrieving
 * \param int type of key (OPBX_KEY_PUBLIC or OPBX_KEY_PRIVATE)
 *
 * Returns the key on success or NULL on failure
 */
extern struct opbx_key *(*opbx_key_get)(const char *key, int type);


/*! \brief Check the authenticity of a message signature using a given public key 
 * \param key a public key to use to verify
 * \param msg the message that has been signed
 * \param sig the proposed valid signature in mime64-like encoding
 *
 * Returns 0 if the signature is valid, or -1 otherwise
 *
 */
extern int (*opbx_check_signature)(struct opbx_key *key, const char *msg, const char *sig);

/*! \brief Check the authenticity of a message signature using a given public key 
 * \param key a public key to use to verify
 * \param msg the message that has been signed
 * \param sig the proposed valid signature in raw binary representation
 *
 * Returns 0 if the signature is valid, or -1 otherwise
 *
 */
extern int (*opbx_check_signature_bin)(struct opbx_key *key, const char *msg, int msglen, const unsigned char *sig);

/*!
 * \param key a private key to use to create the signature
 * \param msg the message to sign
 * \param sig a pointer to a buffer of at least 256 bytes in which the
 * mime64-like encoded signature will be stored
 *
 * Returns 0 on success or -1 on failure.
 *
 */
extern int (*opbx_sign)(struct opbx_key *key, char *msg, char *sig);

/*!
 * \param key a private key to use to create the signature
 * \param msg the message to sign
 * \param sig a pointer to a buffer of at least 128 bytes in which the
 * raw encoded signature will be stored
 *
 * Returns 0 on success or -1 on failure.
 *
 */
extern int (*opbx_sign_bin)(struct opbx_key *key, const char *msg, int msglen, unsigned char *sig);

/*!
 * \param key a private key to use to encrypt
 * \param src the message to encrypt
 * \param srclen the length of the message to encrypt
 * \param dst a pointer to a buffer of at least srclen * 1.5 bytes in which the encrypted
 * answer will be stored
 *
 * Returns length of encrypted data on success or -1 on failure.
 *
 */
extern int (*opbx_encrypt_bin)(unsigned char *dst, const unsigned char *src, int srclen, struct opbx_key *key);

/*!
 * \param key a private key to use to decrypt
 * \param src the message to decrypt
 * \param srclen the length of the message to decrypt
 * \param dst a pointer to a buffer of at least srclen bytes in which the decrypted
 * answer will be stored
 *
 * Returns length of decrypted data on success or -1 on failure.
 *
 */
extern int (*opbx_decrypt_bin)(unsigned char *dst, const unsigned char *src, int srclen, struct opbx_key *key);
#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _OPENPBX_CRYPTO_H */
