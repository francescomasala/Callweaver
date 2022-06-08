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
 * \brief String manipulation functions
 */

#ifndef _OPENPBX_STRINGS_H
#define _OPENPBX_STRINGS_H

#include <string.h>
#include <stdarg.h>

#include "openpbx/inline_api.h"
#include "openpbx/compiler.h"
#include "confdefs.h"

static inline int opbx_strlen_zero(const char *s)
{
	return (!s || (*s == '\0'));
}

/*!
  \brief Gets a pointer to the first non-whitespace character in a string.
  \param str the input string
  \return a pointer to the first non-whitespace character
 */
OPBX_INLINE_API(
char *opbx_skip_blanks(char *str),
{
	while (*str && *str < 33)
		str++;
	return str;
}
)

/*!
  \brief Trims trailing whitespace characters from a string.
  \param str the input string
  \return a pointer to the NULL following the string
 */
OPBX_INLINE_API(
char *opbx_trim_blanks(char *str),
{
	char *work = str;

	if (work) {
		work += strlen(work) - 1;
		/* It's tempting to only want to erase after we exit this loop, 
		   but since opbx_trim_blanks *could* receive a constant string
		   (which we presumably wouldn't have to touch), we shouldn't
		   actually set anything unless we must, and it's easier just
		   to set each position to \0 than to keep track of a variable
		   for it */
		while ((work >= str) && *work < 33)
			*(work--) = '\0';
	}
	return str;
}
)

/*!
  \brief Gets a pointer to first whitespace character in a string.
  \param str the input string
  \return a pointer to the first whitespace character
 */
OPBX_INLINE_API(
char *opbx_skip_nonblanks(char *str),
{
	while (*str && *str > 32)
		str++;
	return str;
}
)
  
/*!
  \brief Strip leading/trailing whitespace from a string.
  \param s The string to be stripped (will be modified).
  \return The stripped string.

  This functions strips all leading and trailing whitespace
  characters from the input string, and returns a pointer to
  the resulting string. The string is modified in place.
*/
OPBX_INLINE_API(
char *opbx_strip(char *s),
{
	s = opbx_skip_blanks(s);
	if (s)
		opbx_trim_blanks(s);
	return s;
} 
)

/*!
  \brief Strip leading/trailing whitespace and quotes from a string.
  \param s The string to be stripped (will be modified).
  \param beg_quotes The list of possible beginning quote characters.
  \param end_quotes The list of matching ending quote characters.
  \return The stripped string.

  This functions strips all leading and trailing whitespace
  characters from the input string, and returns a pointer to
  the resulting string. The string is modified in place.

  It can also remove beginning and ending quote (or quote-like)
  characters, in matching pairs. If the first character of the
  string matches any character in beg_quotes, and the last
  character of the string is the matching character in
  end_quotes, then they are removed from the string.

  Examples:
  \code
  opbx_strip_quoted(buf, "\"", "\"");
  opbx_strip_quoted(buf, "'", "'");
  opbx_strip_quoted(buf, "[{(", "]})");
  \endcode
 */
char *opbx_strip_quoted(char *s, const char *beg_quotes, const char *end_quotes);

/*!
  \brief Size-limited null-terminating string copy.
  \param dst The destination buffer.
  \param src The source string
  \param size The size of the destination buffer
  \return Nothing.

  This is similar to \a strncpy, with two important differences:
    - the destination buffer will \b always be null-terminated
    - the destination buffer is not filled with zeros past the copied string length
  These differences make it slightly more efficient, and safer to use since it will
  not leave the destination buffer unterminated. There is no need to pass an artificially
  reduced buffer size to this function (unlike \a strncpy), and the buffer does not need
  to be initialized to zeroes prior to calling this function.
*/
OPBX_INLINE_API(
void opbx_copy_string(char *dst, const char *src, size_t size),
{
	while (*src && size) {
		*dst++ = *src++;
		size--;
	}
	if (__builtin_expect(!size, 0))
		dst--;
	*dst = '\0';
}
)

/*!
  \brief Build a string in a buffer, designed to be called repeatedly
  
  This is a wrapper for snprintf, that properly handles the buffer pointer
  and buffer space available.

  \return 0 on success, non-zero on failure.
  \param buffer current position in buffer to place string into (will be updated on return)
  \param space remaining space in buffer (will be updated on return)
  \param fmt printf-style format string
*/
int opbx_build_string(char **buffer, size_t *space, const char *fmt, ...) __attribute__ ((format (printf, 3, 4)));

/*!
  \brief Build a string in a buffer, designed to be called repeatedly
  
  This is a wrapper for snprintf, that properly handles the buffer pointer
  and buffer space available.

  \return 0 on success, non-zero on failure.
  \param buffer current position in buffer to place string into (will be updated on return)
  \param space remaining space in buffer (will be updated on return)
  \param fmt printf-style format string
  \param ap varargs list of arguments for format
*/
int opbx_build_string_va(char **buffer, size_t *space, const char *fmt, va_list ap);

/*! Make sure something is true */
/*!
 * Determine if a string containing a boolean value is "true".
 * This function checks to see whether a string passed to it is an indication of an "true" value.  It checks to see if the string is "yes", "true", "y", "t", "on" or "1".  
 *
 * Returns 0 if val is a NULL pointer, -1 if "true", and 0 otherwise.
 */
int opbx_true(const char *val);

/*! Make sure something is false */
/*!
 * Determine if a string containing a boolean value is "false".
 * This function checks to see whether a string passed to it is an indication of an "false" value.  It checks to see if the string is "no", "false", "n", "f", "off" or "0".  
 *
 * Returns 0 if val is a NULL pointer, -1 if "false", and 0 otherwise.
 */
int opbx_false(const char *val);

/* The realloca lets us opbx_restrdupa(), but you can't mix any other opbx_strdup calls! */

struct opbx_realloca {
	char *ptr;
	int alloclen;
};

#define opbx_restrdupa(ra, s) \
	({ \
		if ((ra)->ptr && strlen(s) + 1 < (ra)->alloclen) { \
			strcpy((ra)->ptr, s); \
		} else { \
			(ra)->ptr = alloca(strlen(s) + 1 - (ra)->alloclen); \
			if ((ra)->ptr) (ra)->alloclen = strlen(s) + 1; \
		} \
		(ra)->ptr; \
	})

#ifndef HAVE_STRCASESTR
char *strcasestr(const char *, const char *);
#endif

#if !defined(HAVE_STRNDUP) && !defined(__OPBX_DEBUG_MALLOC)
char *strndup(const char *, size_t);
#endif

#ifndef HAVE_STRNLEN
size_t strnlen(const char *, size_t);
#endif

#if !defined(HAVE_VASPRINTF) && !defined(__OPBX_DEBUG_MALLOC)
int vasprintf(char **strp, const char *fmt, va_list ap);
#endif

#ifndef HAVE_STRTOQ
uint64_t strtoq(const char *nptr, char **endptr, int base);
#endif

#ifndef HAVE_STRSEP
char* strsep(char** str, const char* delims);
#endif

#ifndef HAVE_SETENV
int setenv(const char *name, const char *value, int overwrite);
#endif

#ifndef HAVE_UNSETENV
int unsetenv(const char *name);   
#endif

#endif /* _OPENPBX_STRINGS_H */
