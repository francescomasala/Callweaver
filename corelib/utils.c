/*
 * CallWeaver -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
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
 * \brief Utility functions
 *
 * \note These are important for portability and security,
 * so please use them in favour of other routines.
 * Please consult the CODING GUIDELINES for more information.
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/evp.h>

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/corelib/utils.c $", "$Revision: 4741 $")

#include "callweaver/lock.h"
#include "callweaver/io.h"
#include "callweaver/logger.h"
#include "callweaver/options.h"
#include "callweaver/config.h"

#define CW_API_MODULE		/* ensure that inlinable API functions will be built in this module if required */
#include "callweaver/strings.h"

#define CW_API_MODULE		/* ensure that inlinable API functions will be built in this module if required */
#include "callweaver/time.h"

#define CW_API_MODULE		/* ensure that inlinable API functions will be built in this module if required */
#include "callweaver/utils.h"

static char base64[64];
static char b2a[256];

#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined( __NetBSD__ ) || defined(__APPLE__) || defined(__CYGWIN__)

/* duh? ERANGE value copied from web... */
#define ERANGE 34
#undef gethostbyname

CW_MUTEX_DEFINE_STATIC(__mutex);

/* Recursive replacement for gethostbyname for BSD-based systems.  This
routine is derived from code originally written and placed in the public 
domain by Enzo Michelangeli <em@em.no-ip.com> */

static int gethostbyname_r (const char *name, struct hostent *ret, char *buf,
				size_t buflen, struct hostent **result, 
				int *h_errnop) 
{
	int hsave;
	struct hostent *ph;
	cw_mutex_lock(&__mutex); /* begin critical area */
	hsave = h_errno;

	ph = gethostbyname(name);
	*h_errnop = h_errno; /* copy h_errno to *h_herrnop */
	if (ph == NULL) {
		*result = NULL;
	} else {
		char **p, **q;
		char *pbuf;
		int nbytes=0;
		int naddr=0, naliases=0;
		/* determine if we have enough space in buf */

		/* count how many addresses */
		for (p = ph->h_addr_list; *p != 0; p++) {
			nbytes += ph->h_length; /* addresses */
			nbytes += sizeof(*p); /* pointers */
			naddr++;
		}
		nbytes += sizeof(*p); /* one more for the terminating NULL */

		/* count how many aliases, and total length of strings */
		for (p = ph->h_aliases; *p != 0; p++) {
			nbytes += (strlen(*p)+1); /* aliases */
			nbytes += sizeof(*p);  /* pointers */
			naliases++;
		}
		nbytes += sizeof(*p); /* one more for the terminating NULL */

		/* here nbytes is the number of bytes required in buffer */
		/* as a terminator must be there, the minimum value is ph->h_length */
		if(nbytes > buflen) {
			*result = NULL;
			cw_mutex_unlock(&__mutex); /* end critical area */
			return ERANGE; /* not enough space in buf!! */
		}

		/* There is enough space. Now we need to do a deep copy! */
		/* Allocation in buffer:
			from [0] to [(naddr-1) * sizeof(*p)]:
			pointers to addresses
			at [naddr * sizeof(*p)]:
			NULL
			from [(naddr+1) * sizeof(*p)] to [(naddr+naliases) * sizeof(*p)] :
			pointers to aliases
			at [(naddr+naliases+1) * sizeof(*p)]:
			NULL
			then naddr addresses (fixed length), and naliases aliases (asciiz).
		*/

		*ret = *ph;   /* copy whole structure (not its address!) */

		/* copy addresses */
		q = (char **)buf; /* pointer to pointers area (type: char **) */
		ret->h_addr_list = q; /* update pointer to address list */
		pbuf = buf + ((naddr+naliases+2)*sizeof(*p)); /* skip that area */
		for (p = ph->h_addr_list; *p != 0; p++) {
			memcpy(pbuf, *p, ph->h_length); /* copy address bytes */
			*q++ = pbuf; /* the pointer is the one inside buf... */
			pbuf += ph->h_length; /* advance pbuf */
		}
		*q++ = NULL; /* address list terminator */

		/* copy aliases */
		ret->h_aliases = q; /* update pointer to aliases list */
		for (p = ph->h_aliases; *p != 0; p++) {
			strcpy(pbuf, *p); /* copy alias strings */
			*q++ = pbuf; /* the pointer is the one inside buf... */
			pbuf += strlen(*p); /* advance pbuf */
			*pbuf++ = 0; /* string terminator */
		}
		*q++ = NULL; /* terminator */

		strcpy(pbuf, ph->h_name); /* copy alias strings */
		ret->h_name = pbuf;
		pbuf += strlen(ph->h_name); /* advance pbuf */
		*pbuf++ = 0; /* string terminator */

		*result = ret;  /* and let *result point to structure */

	}
	h_errno = hsave;  /* restore h_errno */
	cw_mutex_unlock(&__mutex); /* end critical area */

	return (*result == NULL); /* return 0 on success, non-zero on error */
}


#endif

/*! \brief Re-entrant (thread safe) version of gethostbyname that replaces the 
   standard gethostbyname (which is not thread safe)
*/
struct hostent *cw_gethostbyname(const char *host, struct cw_hostent *hp)
{
	int res;
	int herrno;
	int dots=0;
	const char *s;
	struct hostent *result = NULL;
	/* Although it is perfectly legitimate to lookup a pure integer, for
	   the sake of the sanity of people who like to name their peers as
	   integers, we break with tradition and refuse to look up a
	   pure integer */
	s = host;
	res = 0;
	while(s && *s) {
		if (*s == '.')
			dots++;
		else if (!isdigit(*s))
			break;
		s++;
	}
	if (!s || !*s) {
		/* Forge a reply for IP's to avoid octal IP's being interpreted as octal */
		if (dots != 3)
			return NULL;
		memset(hp, 0, sizeof(struct cw_hostent));
		hp->hp.h_addr_list = (void *) hp->buf;
		hp->hp.h_addr = hp->buf + sizeof(void *);
		if (inet_pton(AF_INET, host, hp->hp.h_addr) > 0)
			return &hp->hp;
		return NULL;
		
	}
#ifdef SOLARIS
	result = gethostbyname_r(host, &hp->hp, hp->buf, sizeof(hp->buf), &herrno);

	if (!result || !hp->hp.h_addr_list || !hp->hp.h_addr_list[0])
		return NULL;
#else
	res = gethostbyname_r(host, &hp->hp, hp->buf, sizeof(hp->buf), &result, &herrno);

	if (res || !result || !hp->hp.h_addr_list || !hp->hp.h_addr_list[0])
		return NULL;
#endif
	return &hp->hp;
}



CW_MUTEX_DEFINE_STATIC(test_lock);
CW_MUTEX_DEFINE_STATIC(test_lock2);
static pthread_t test_thread; 
static int lock_count = 0;
static int test_errors = 0;

/*! \brief This is a regression test for recursive mutexes.
   test_for_thread_safety() will return 0 if recursive mutex locks are
   working properly, and non-zero if they are not working properly. */
static void *test_thread_body(void *data) 
{ 
	cw_mutex_lock(&test_lock);
	lock_count += 10;
	if (lock_count != 10) 
		test_errors++;
	cw_mutex_lock(&test_lock);
	lock_count += 10;
	if (lock_count != 20) 
		test_errors++;
	cw_mutex_lock(&test_lock2);
	cw_mutex_unlock(&test_lock);
	lock_count -= 10;
	if (lock_count != 10) 
		test_errors++;
	cw_mutex_unlock(&test_lock);
	lock_count -= 10;
	cw_mutex_unlock(&test_lock2);
	if (lock_count != 0) 
		test_errors++;
	return NULL;
} 

int test_for_thread_safety(void)
{ 
	cw_mutex_lock(&test_lock2);
	cw_mutex_lock(&test_lock);
	lock_count += 1;
	cw_mutex_lock(&test_lock);
	lock_count += 1;
	cw_pthread_create(&test_thread, NULL, test_thread_body, NULL); 
	usleep(100);
	if (lock_count != 2) 
		test_errors++;
	cw_mutex_unlock(&test_lock);
	lock_count -= 1;
	usleep(100); 
	if (lock_count != 1) 
		test_errors++;
	cw_mutex_unlock(&test_lock);
	lock_count -= 1;
	if (lock_count != 0) 
		test_errors++;
	cw_mutex_unlock(&test_lock2);
	usleep(100);
	if (lock_count != 0) 
		test_errors++;
	pthread_join(test_thread, NULL);
	return(test_errors);          /* return 0 on success. */
}

void cw_hash_to_hex(char *output, unsigned char *md_value, unsigned int md_len)
{
	int x;
	int len = 0;

	for (x = 0; x < md_len; x++)
		len += sprintf(output + len, "%2.2x", md_value[x]);
}

/*! \Brief cw_md5_hash_bin: Produce 16 char MD5 hash of value. ---*/
int cw_md5_hash_bin(unsigned char *md_value, unsigned char *input, unsigned int input_len)
{
	EVP_MD_CTX mdctx;
	unsigned int md_len;

	EVP_DigestInit(&mdctx, EVP_md5());
	EVP_DigestUpdate(&mdctx, input, input_len);
	EVP_DigestFinal(&mdctx, md_value, &md_len);

	return md_len;
}

void cw_md5_hash(char *output, char *input)
{
	unsigned int md_len;
	unsigned char md_value[CW_MAX_BINARY_MD_SIZE];
	
	md_len = cw_md5_hash_bin(md_value, (unsigned char *) input, strlen(input));

	cw_hash_to_hex(output, md_value, md_len);
}

int cw_md5_hash_two_bin(unsigned char *md_value,
			  unsigned char *input1, unsigned int input1_len,
			  unsigned char *input2, unsigned int input2_len)
{
	EVP_MD_CTX mdctx;
	unsigned int md_len;

	EVP_DigestInit(&mdctx, EVP_md5());
	EVP_DigestUpdate(&mdctx, input1, input1_len);
	EVP_DigestUpdate(&mdctx, input2, input2_len);
	EVP_DigestFinal(&mdctx, md_value, &md_len);
	
	return md_len;
}

void cw_md5_hash_two(char *output, char *input1, char *input2)
{
	unsigned int md_len;
	unsigned char md_value[CW_MAX_BINARY_MD_SIZE];
	
	md_len = cw_md5_hash_two_bin(md_value, (unsigned char *) input1, strlen(input1),
				       (unsigned char *) input2, strlen(input2));

	cw_hash_to_hex(output, md_value, md_len);
}

int cw_base64decode(unsigned char *dst, const char *src, int max)
{
	int cnt = 0;
	unsigned int byte = 0;
	unsigned int bits = 0;
	int incnt = 0;
#if 0
	unsigned char *odst = dst;
#endif
	while(*src && (cnt < max)) {
		/* Shift in 6 bits of input */
		byte <<= 6;
		byte |= (b2a[(int)(*src)]) & 0x3f;
		bits += 6;

		src++;
		incnt++;
		/* If we have at least 8 bits left over, take that character 
		   off the top */
		if (bits >= 8)  {
			bits -= 8;
			*dst = (byte >> bits) & 0xff;

			dst++;
			cnt++;
		}
	}
#if 0
	dump(odst, cnt);
#endif
	/* Dont worry about left over bits, they're extra anyway */
	return cnt;
}

int cw_base64encode(char *dst, const unsigned char *src, int srclen, int max)
{
	int cnt = 0;
	unsigned int byte = 0;
	int bits = 0;
	int index;
	int cntin = 0;
#if 0
	char *odst = dst;
	dump(src, srclen);
#endif
	/* Reserve one bit for end */
	max--;
	while((cntin < srclen) && (cnt < max)) {
		byte <<= 8;

		byte |= *(src++);
		bits += 8;
		cntin++;
		while((bits >= 6) && (cnt < max)) {
			bits -= 6;
			/* We want only the top */
			index = (byte >> bits) & 0x3f;
			*dst = base64[index];

			dst++;
			cnt++;
		}
	}
	if (bits && (cnt < max)) {
		/* Add one last character for the remaining bits, 
		   padding the rest with 0 */
		byte <<= (6 - bits);
		index = (byte) & 0x3f;
		*(dst++) = base64[index];
		cnt++;
	}
	*dst = '\0';
	return cnt;
}

static void base64_init(void)
{
	int x;
	memset(b2a, -1, sizeof(b2a));
	/* Initialize base-64 Conversion table */
	for (x=0;x<26;x++) {
		/* A-Z */
		base64[x] = 'A' + x;
		b2a['A' + x] = x;
		/* a-z */
		base64[x + 26] = 'a' + x;
		b2a['a' + x] = x + 26;
		/* 0-9 */
		if (x < 10) {
			base64[x + 52] = '0' + x;
			b2a['0' + x] = x + 52;
		}
	}
	base64[62] = '+';
	base64[63] = '/';
	b2a[(int)'+'] = 62;
	b2a[(int)'/'] = 63;
}

/*! \brief  cw_uri_encode: Turn text string to URI-encoded %XX version ---*/
/* 	At this point, we're converting from ISO-8859-x (8-bit), not UTF8
	as in the SIP protocol spec 
	If doreserved == 1 we will convert reserved characters also.
	RFC 2396, section 2.4
	outbuf needs to have more memory allocated than the instring
	to have room for the expansion. Every char that is converted
	is replaced by three ASCII characters.

	Note: The doreserved option is needed for replaces header in
	SIP transfers.
*/
char *cw_uri_encode(char *string, char *outbuf, int buflen, int doreserved) 
{
	char *reserved = ";/?:@&=+$,# ";	/* Reserved chars */

 	char *ptr  = string;	/* Start with the string */
	char *out = NULL;
	char *buf = NULL;

	strncpy(outbuf, string, buflen);

	/* If there's no characters to convert, just go through and don't do anything */
	while (*ptr) {
		if (((unsigned char) *ptr) > 127 || (doreserved && strchr(reserved, *ptr)) ) {
			/* Oops, we need to start working here */
			if (!buf) {
				buf = outbuf;
				out = buf + (ptr - string) ;	/* Set output ptr */
			}
			out += sprintf(out, "%%%02x", (unsigned char) *ptr);
		} else if (buf) {
			*out = *ptr;	/* Continue copying the string */
			out++;
		} 
		ptr++;
	}
	if (buf)
		*out = '\0';
	return outbuf;
}

/*! \brief  cw_uri_decode: Decode SIP URI, URN, URL (overwrite the string)  ---*/
void cw_uri_decode(char *s) 
{
	char *o;
	unsigned int tmp;

	for (o = s; *s; s++, o++) {
		if (*s == '%' && strlen(s) > 2 && sscanf(s + 1, "%2x", &tmp) == 1) {
			/* have '%', two chars and correct parsing */
			*o = tmp;
			s += 2;	/* Will be incremented once more when we break out */
		} else /* all other cases, just copy */
			*o = *s;
	}
	*o = '\0';
}

/*! \brief  cw_inet_ntoa: Recursive thread safe replacement of inet_ntoa */
const char *cw_inet_ntoa(char *buf, int bufsiz, struct in_addr ia)
{
	return inet_ntop(AF_INET, &ia, buf, bufsiz);
}

int cw_utils_init(void)
{
	base64_init();
	return 0;
}

#ifndef __linux__
#undef pthread_create /* For cw_pthread_create function only */
#endif /* !__linux__ */

int cw_pthread_create_stack(pthread_t *thread, pthread_attr_t *attr, void *(*start_routine)(void *), void *data, size_t stacksize)
{
	pthread_attr_t lattr;
	if (!attr) {
		pthread_attr_init(&lattr);
		attr = &lattr;
	}
#ifdef __linux__
	/* On Linux, pthread_attr_init() defaults to PTHREAD_EXPLICIT_SCHED,
	   which is kind of useless. Change this here to
	   PTHREAD_INHERIT_SCHED; that way the -p option to set realtime
	   priority will propagate down to new threads by default.
	   This does mean that callers cannot set a different priority using
	   PTHREAD_EXPLICIT_SCHED in the attr argument; instead they must set
	   the priority afterwards with pthread_setschedparam(). */
	errno = pthread_attr_setinheritsched(attr, PTHREAD_INHERIT_SCHED);
	if (errno)
		cw_log(LOG_WARNING, "pthread_attr_setinheritsched returned non-zero: %s\n", strerror(errno));
#endif

	if (!stacksize)
		stacksize = CW_STACKSIZE;
	errno = pthread_attr_setstacksize(attr, stacksize);
	if (errno)
		cw_log(LOG_WARNING, "pthread_attr_setstacksize returned non-zero: %s\n", strerror(errno));
	return pthread_create(thread, attr, start_routine, data); /* We're in cw_pthread_create, so it's okay */
}

int cw_wait_for_input(int fd, int ms)
{
	struct pollfd pfd[1];
	memset(pfd, 0, sizeof(pfd));
	pfd[0].fd = fd;
	pfd[0].events = POLLIN|POLLPRI;
	return poll(pfd, 1, ms);
}

int cw_build_string_va(char **buffer, size_t *space, const char *fmt, va_list ap)
{
	int result;

	if (!buffer || !*buffer || !space || !*space)
		return -1;

	result = vsnprintf(*buffer, *space, fmt, ap);

	if (result < 0)
		return -1;
	else if (result > *space)
		result = *space;

	*buffer += result;
	*space -= result;
	return 0;
}

int cw_build_string(char **buffer, size_t *space, const char *fmt, ...)
{
	va_list ap;
	int result;

	va_start(ap, fmt);
	result = cw_build_string_va(buffer, space, fmt, ap);
	va_end(ap);

	return result;
}

int cw_true(const char *s)
{
	if (cw_strlen_zero(s))
		return 0;

	/* Determine if this is a true value */
	if (!strcasecmp(s, "yes") ||
	    !strcasecmp(s, "true") ||
	    !strcasecmp(s, "y") ||
	    !strcasecmp(s, "t") ||
	    !strcasecmp(s, "1") ||
	    !strcasecmp(s, "on"))
		return -1;

	return 0;
}

int cw_false(const char *s)
{
	if (cw_strlen_zero(s))
		return 0;

	/* Determine if this is a false value */
	if (!strcasecmp(s, "no") ||
	    !strcasecmp(s, "false") ||
	    !strcasecmp(s, "n") ||
	    !strcasecmp(s, "f") ||
	    !strcasecmp(s, "0") ||
	    !strcasecmp(s, "off"))
		return -1;

	return 0;
}

#define ONE_MILLION	1000000
/*
 * put timeval in a valid range. usec is 0..999999
 * negative values are not allowed and truncated.
 */
static struct timeval tvfix(struct timeval a)
{
	if (a.tv_usec >= ONE_MILLION) {
		cw_log(LOG_WARNING, "warning too large timestamp %ld.%ld\n",
			a.tv_sec, (long int) a.tv_usec);
		a.tv_sec += a.tv_usec % ONE_MILLION;
		a.tv_usec %= ONE_MILLION;
	} else if (a.tv_usec < 0) {
		cw_log(LOG_WARNING, "warning negative timestamp %ld.%ld\n",
				a.tv_sec, (long int) a.tv_usec);
		a.tv_usec = 0;
	}
	return a;
}

struct timeval cw_tvadd(struct timeval a, struct timeval b)
{
	/* consistency checks to guarantee usec in 0..999999 */
	a = tvfix(a);
	b = tvfix(b);
	a.tv_sec += b.tv_sec;
	a.tv_usec += b.tv_usec;
	if (a.tv_usec >= ONE_MILLION) {
		a.tv_sec++;
		a.tv_usec -= ONE_MILLION;
	}
	return a;
}

struct timeval cw_tvsub(struct timeval a, struct timeval b)
{
	/* consistency checks to guarantee usec in 0..999999 */
	a = tvfix(a);
	b = tvfix(b);
	a.tv_sec -= b.tv_sec;
	a.tv_usec -= b.tv_usec;
	if (a.tv_usec < 0) {
		a.tv_sec-- ;
		a.tv_usec += ONE_MILLION;
	}
	return a;
}
#undef ONE_MILLION

#ifndef HAVE_STRCASESTR
static char *upper(const char *orig, char *buf, int bufsize)
{
	int i = 0;

	while (i < (bufsize - 1) && orig[i]) {
		buf[i] = toupper(orig[i]);
		i++;
	}

	buf[i] = '\0';

	return buf;
}

char *strcasestr(const char *haystack, const char *needle)
{
	char *offset;
	char *u1, *u2;
	int u1len = strlen(haystack) + 1, u2len = strlen(needle) + 1;

	if (u2len > u1len) {
		/* Needle bigger than haystack */
		return NULL;
	}
	u1 = alloca(u1len);
	u2 = alloca(u2len);
	offset = strstr(upper(haystack, u1, u1len), upper(needle, u2, u2len));
	if (offset) {
		/* Return the offset into the original string */
		return ((char *)((unsigned long)haystack + (unsigned long)(offset - u1)));
	} else {
		return NULL;
	}
}
#endif /* !HAVE_STRCASESTR */

#ifndef HAVE_STRNLEN
size_t strnlen(const char *s, size_t n)
{
	size_t len;

	for (len=0; len < n; len++)
		if (s[len] == '\0')
			break;

	return len;
}
#endif /* !HAVE_STRNLEN */

#if !defined(HAVE_STRNDUP) && !defined(__CW_DEBUG_MALLOC)
char *strndup(const char *s, size_t n)
{
	size_t len = strnlen(s, n);
	char *new = malloc(len + 1);

	if (!new)
		return NULL;

	new[len] = '\0';
	return memcpy(new, s, len);
}
#endif /* !defined(HAVE_STRNDUP) && !defined(__CW_DEBUG_MALLOC) */

#if !defined(HAVE_VASPRINTF) && !defined(__CW_DEBUG_MALLOC)
int vasprintf(char **strp, const char *fmt, va_list ap)
{
	int size;
	va_list ap2;
	char s;

	*strp = NULL;
	va_copy(ap2, ap);
	size = vsnprintf(&s, 1, fmt, ap2);
	va_end(ap2);
	*strp = malloc(size + 1);
	if (!*strp)
		return -1;
	vsnprintf(*strp, size + 1, fmt, ap);

	return size;
}
#endif /* !defined(HAVE_VASPRINTF) && !defined(__CW_DEBUG_MALLOC) */

#ifndef HAVE_STRTOQ
#ifndef LONG_MIN
#define LONG_MIN        (-9223372036854775807L-1L)
	                                 /* min value of a "long int" */
#endif
#ifndef LONG_MAX
#define LONG_MAX        9223372036854775807L
	                                 /* max value of a "long int" */
#endif

/*
 * Convert a string to a quad integer.
 *
 * Ignores `locale' stuff.  Assumes that the upper and lower case
 * alphabets and digits are each contiguous.
 */
uint64_t strtoq(const char *nptr, char **endptr, int base)
{
	 const char *s;
	 uint64_t acc;
	 unsigned char c;
	 uint64_t qbase, cutoff;
	 int neg, any, cutlim;

	 /*
	  * Skip white space and pick up leading +/- sign if any.
	  * If base is 0, allow 0x for hex and 0 for octal, else
	  * assume decimal; if base is already 16, allow 0x.
	  */
	 s = nptr;
	 do {
	         c = *s++;
	 } while (isspace(c));
	 if (c == '-') {
	         neg = 1;
	         c = *s++;
	 } else {
	         neg = 0;
	         if (c == '+')
	                 c = *s++;
	 }
	 if ((base == 0 || base == 16) &&
	     c == '\0' && (*s == 'x' || *s == 'X')) {
	         c = s[1];
	         s += 2;
	         base = 16;
	 }
	 if (base == 0)
	         base = c == '\0' ? 8 : 10;

	 /*
	  * Compute the cutoff value between legal numbers and illegal
	  * numbers.  That is the largest legal value, divided by the
	  * base.  An input number that is greater than this value, if
	  * followed by a legal input character, is too big.  One that
	  * is equal to this value may be valid or not; the limit
	  * between valid and invalid numbers is then based on the last
	  * digit.  For instance, if the range for quads is
	  * [-9223372036854775808..9223372036854775807] and the input base
	  * is 10, cutoff will be set to 922337203685477580 and cutlim to
	  * either 7 (neg==0) or 8 (neg==1), meaning that if we have
	  * accumulated a value > 922337203685477580, or equal but the
	  * next digit is > 7 (or 8), the number is too big, and we will
	  * return a range error.
	  *
	  * Set any if any `digits' consumed; make it negative to indicate
	  * overflow.
	  */
	 qbase = (unsigned)base;
	 cutoff = neg ? (uint64_t)-(LONG_MIN + LONG_MAX) + LONG_MAX : LONG_MAX;
	 cutlim = cutoff % qbase;
	 cutoff /= qbase;
	 for (acc = 0, any = 0;; c = *s++) {
	         if (!isascii(c))
	                 break;
	         if (isdigit(c))
	                 c -= '\0';
	         else if (isalpha(c))
	                 c -= isupper(c) ? 'A' - 10 : 'a' - 10;
	         else
	                 break;
	         if (c >= base)
	                 break;
	         if (any < 0 || acc > cutoff || (acc == cutoff && c > cutlim))
	                 any = -1;
	         else {
	                 any = 1;
	                 acc *= qbase;
	                 acc += c;
	         }
	 }
	 if (any < 0) {
	         acc = neg ? LONG_MIN : LONG_MAX;
	 } else if (neg)
	         acc = -acc;
	 if (endptr != 0)
	         *((const char **)endptr) = any ? s - 1 : nptr;
	 return acc;
}
#endif /* !HAVE_STRTOQ */

#if (!defined(getloadavg))
#ifdef linux
/* Alternative method of getting load avg on Linux only */
int getloadavg(double *list, int nelem)
{
	FILE *LOADAVG;
	double avg[3] = { 0.0, 0.0, 0.0 };
	int i, res = -1;

	if ((LOADAVG = fopen("/proc/loadavg", "r"))) {
		fscanf(LOADAVG, "%lf %lf %lf", &avg[0], &avg[1], &avg[2]);
		res = 0;
		fclose(LOADAVG);
	}

	for (i = 0; (i < nelem) && (i < 3); i++) {
		list[i] = avg[i];
	}

	return res;
}
#else /* !linux */
/* Return something that won't cancel the call, but still return -1, in case
 * we correct the implementation to check return value */
int getloadavg(double *list, int nelem)
{
	int i;

	for (i = 0; i < nelem; i++) {
		list[i] = 0.1;
	}
	return -1;
}
#endif /* linux */
#endif /* !defined(getloadavg) */

/*! \brief glibc puts a lock inside random(3), so that the results are thread-safe.
 * BSD libc (and others) do not. */
#ifndef linux

CW_MUTEX_DEFINE_STATIC(randomlock);

long int cw_random(void)
{
	long int res;
	cw_mutex_lock(&randomlock);
	res = random();
	cw_mutex_unlock(&randomlock);
	return res;
}
#endif

void cw_enable_packet_fragmentation(int sock)
{
#ifdef __linux__
	int val = IP_PMTUDISC_DONT;
	
	if (setsockopt(sock, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val)))
		cw_log(LOG_WARNING, "Unable to disable PMTU discovery. Large UDP packets may fail to be delivered when sent from this socket.\n");
#endif
}

