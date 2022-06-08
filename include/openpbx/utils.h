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
 * \brief Utility functions
 */

#ifndef _OPENPBX_UTILS_H
#define _OPENPBX_UTILS_H

#include "confdefs.h"

#include <netinet/in.h>
#include <arpa/inet.h>	/* we want to override inet_ntoa */
#include <netdb.h>
#include <limits.h>
#include <openssl/evp.h>

#include "openpbx/lock.h"
#include "openpbx/time.h"
#include "openpbx/strings.h"

/*! \note
 \verbatim
   Note:
   It is very important to use only unsigned variables to hold
   bit flags, as otherwise you can fall prey to the compiler's
   sign-extension antics if you try to use the top two bits in
   your variable.

   The flag macros below use a set of compiler tricks to verify
   that the caller is using an "unsigned int" variable to hold
   the flags, and nothing else. If the caller uses any other
   type of variable, a warning message similar to this:

   warning: comparison of distinct pointer types lacks cast
   will be generated.

   The "dummy" variable below is used to make these comparisons.

   Also note that at -O2 or above, this type-safety checking
   does _not_ produce any additional object code at all.
 \endverbatim
*/

extern unsigned int __unsigned_int_flags_dummy;

#define opbx_test_flag(p,flag) 		({ \
					typeof ((p)->flags) __p = (p)->flags; \
					typeof (__unsigned_int_flags_dummy) __x = 0; \
					(void) (&__p == &__x); \
					((p)->flags & (flag)); \
					})

#define opbx_set_flag(p,flag) 		do { \
					typeof ((p)->flags) __p = (p)->flags; \
					typeof (__unsigned_int_flags_dummy) __x = 0; \
					(void) (&__p == &__x); \
					((p)->flags |= (flag)); \
					} while(0)

#define opbx_clear_flag(p,flag) 		do { \
					typeof ((p)->flags) __p = (p)->flags; \
					typeof (__unsigned_int_flags_dummy) __x = 0; \
					(void) (&__p == &__x); \
					((p)->flags &= ~(flag)); \
					} while(0)

#define opbx_copy_flags(dest,src,flagz)	do { \
					typeof ((dest)->flags) __d = (dest)->flags; \
					typeof ((src)->flags) __s = (src)->flags; \
					typeof (__unsigned_int_flags_dummy) __x = 0; \
					(void) (&__d == &__x); \
					(void) (&__s == &__x); \
					(dest)->flags &= ~(flagz); \
					(dest)->flags |= ((src)->flags & (flagz)); \
					} while (0)

#define opbx_set2_flag(p,value,flag)	do { \
					typeof ((p)->flags) __p = (p)->flags; \
					typeof (__unsigned_int_flags_dummy) __x = 0; \
					(void) (&__p == &__x); \
					if (value) \
						(p)->flags |= (flag); \
					else \
						(p)->flags &= ~(flag); \
					} while (0)

/* Non-type checking variations for non-unsigned int flags.  You
   should only use non-unsigned int flags where required by 
   protocol etc and if you know what you're doing :)  */
#define opbx_test_flag_nonstd(p,flag) 		({ \
					((p)->flags & (flag)); \
					})

#define opbx_set_flag_nonstd(p,flag) 		do { \
					((p)->flags |= (flag)); \
					} while(0)

#define opbx_clear_flag_nonstd(p,flag) 		do { \
					((p)->flags &= ~(flag)); \
					} while(0)

#define opbx_copy_flags_nonstd(dest,src,flagz)	do { \
					(dest)->flags &= ~(flagz); \
					(dest)->flags |= ((src)->flags & (flagz)); \
					} while (0)

#define opbx_set2_flag_nonstd(p,value,flag)	do { \
					if (value) \
						(p)->flags |= (flag); \
					else \
						(p)->flags &= ~(flag); \
					} while (0)

#define OPBX_FLAGS_ALL UINT_MAX

struct opbx_flags {
	unsigned int flags;
};

struct opbx_hostent {
	struct hostent hp;
	char buf[1024];
};

extern struct hostent *opbx_gethostbyname(const char *host, struct opbx_hostent *hp);

#define OPBX_MAX_BINARY_MD_SIZE EVP_MAX_MD_SIZE
#define OPBX_MAX_HEX_MD_SIZE ((EVP_MAX_MD_SIZE * 2) + 1)

/* opbx_hash_to_hex
   \brief Convert binary message digest to hex-encoded version. */
extern void opbx_hash_to_hex(char *output, unsigned char *md_value, unsigned int md_len);

extern int opbx_md5_hash_bin(unsigned char *md_value, unsigned char *input, unsigned int input_len);

/* opbx_md5_hash 
	\brief Produces MD5 hash based on input string */
extern void opbx_md5_hash(char *output, char *input);

extern int opbx_md5_hash_two_bin(unsigned char *output,
				 unsigned char *input1, unsigned int input1_len,
				 unsigned char *input2, unsigned int input2_len);

/* opbx_md5_hash_two 
	\brief Produces MD5 hash based on two input strings */
extern void opbx_md5_hash_two(char *output, char *input1, char *input2);

extern int opbx_base64encode(char *dst, const unsigned char *src, int srclen, int max);
extern int opbx_base64decode(unsigned char *dst, const char *src, int max);

/*! opbx_uri_encode
	\brief Turn text string to URI-encoded %XX version 
 	At this point, we're converting from ISO-8859-x (8-bit), not UTF8
	as in the SIP protocol spec 
	If doreserved == 1 we will convert reserved characters also.
	RFC 2396, section 2.4
	outbuf needs to have more memory allocated than the instring
	to have room for the expansion. Every char that is converted
	is replaced by three ASCII characters.
	\param string	String to be converted
	\param outbuf	Resulting encoded string
	\param buflen	Size of output buffer
	\param doreserved	Convert reserved characters
*/

char *opbx_uri_encode(char *string, char *outbuf, int buflen, int doreserved);

/*!	\brief Decode URI, URN, URL (overwrite string)
	\param s	String to be decoded 
 */
void opbx_uri_decode(char *s);

static inline void opbx_slinear_saturated_add(short *input, short *value)
{
	int res;

	res = (int) *input + *value;
	if (res > 32767)
		*input = 32767;
	else if (res < -32767)
		*input = -32767;
	else
		*input = (short) res;
}
	
static inline void opbx_slinear_saturated_multiply(short *input, short *value)
{
	int res;

	res = (int) *input * *value;
	if (res > 32767)
		*input = 32767;
	else if (res < -32767)
		*input = -32767;
	else
		*input = (short) res;
}

static inline void opbx_slinear_saturated_divide(short *input, short *value)
{
	*input /= *value;
}

extern int test_for_thread_safety(void);

extern const char *opbx_inet_ntoa(char *buf, int bufsiz, struct in_addr ia);

#ifdef inet_ntoa
#undef inet_ntoa
#endif
#define inet_ntoa __dont__use__inet_ntoa__use__opbx_inet_ntoa__instead__

extern int opbx_utils_init(void);
extern int opbx_wait_for_input(int fd, int ms);

/*! Compares the source address and port of two sockaddr_in */
static inline int inaddrcmp(const struct sockaddr_in *sin1, const struct sockaddr_in *sin2)
{
	return ((sin1->sin_addr.s_addr != sin2->sin_addr.s_addr) 
		|| (sin1->sin_port != sin2->sin_port));
}

/*! Atomically get value of "var" protected by "lock" */
#define OPBX_ATOMIC_GET(lock, var) \
	({ \
	 	typeof (var) value; \
		opbx_mutex_lock(&(lock)); \
		value = (var); \
		opbx_mutex_unlock(&(lock)); \
	 	value; \
	 })
 	
#define OPBX_STACKSIZE 256 * 1024
#define opbx_pthread_create(a,b,c,d) opbx_pthread_create_stack(a,b,c,d,0)
extern int opbx_pthread_create_stack(pthread_t *thread, pthread_attr_t *attr, void *(*start_routine)(void *), void *data, size_t stacksize);

#endif /* _OPENPBX_UTILS_H */

#ifdef linux
#define opbx_random random
#else
long int opbx_random(void);
#endif

/*!
  \brief Disable PMTU discovery on a socket
  \param sock The socket to manipulate
  \return Nothing

  On Linux, UDP sockets default to sending packets with the Dont Fragment (DF)
  bit set. This is supposedly done to allow the application to do PMTU
  discovery, but Asterisk does not do this.

  Because of this, UDP packets sent by Openpbx that are larger than the MTU
  of any hop in the path will be lost. This function can be called on a socket
  to ensure that the DF bit will not be set.
 */
void opbx_enable_packet_fragmentation(int sock);

