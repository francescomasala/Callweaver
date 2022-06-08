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

/*
 * OpenPBX memory usage debugging
 */

#ifndef NO_OPBX_MM
#ifndef _OPENPBX_ASTMM_H
#define _OPENPBX_ASTMM_H

#define __OPBX_DEBUG_MALLOC

/* Include these now to prevent them from being needed later */
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Undefine any macros */
#undef malloc
#undef calloc
#undef realloc
#undef strdup
#undef strndup
#undef vasprintf

void *__opbx_calloc(size_t nmemb, size_t size, const char *file, int lineno, const char *func);
void *__opbx_malloc(size_t size, const char *file, int lineno, const char *func);
void __opbx_free(void *ptr, const char *file, int lineno, const char *func);
void *__opbx_realloc(void *ptr, size_t size, const char *file, int lineno, const char *func);
char *__opbx_strdup(const char *s, const char *file, int lineno, const char *func);
char *__opbx_strndup(const char *s, size_t n, const char *file, int lineno, const char *func);
int __opbx_vasprintf(char **strp, const char *format, va_list ap, const char *file, int lineno, const char *func);

void __opbx_mm_init(void);


/* Provide our own definitions */
#define calloc(a,b) \
	__opbx_calloc(a,b,__FILE__, __LINE__, __PRETTY_FUNCTION__)

#define malloc(a) \
	__opbx_malloc(a,__FILE__, __LINE__, __PRETTY_FUNCTION__)

#define free(a) \
	__opbx_free(a,__FILE__, __LINE__, __PRETTY_FUNCTION__)

#define realloc(a,b) \
	__opbx_realloc(a,b,__FILE__, __LINE__, __PRETTY_FUNCTION__)

#define strdup(a) \
	__opbx_strdup(a,__FILE__, __LINE__, __PRETTY_FUNCTION__)

#define strndup(a,b) \
	__opbx_strndup(a,b,__FILE__, __LINE__, __PRETTY_FUNCTION__)

#define vasprintf(a,b,c) \
	__opbx_vasprintf(a,b,c,__FILE__, __LINE__, __PRETTY_FUNCTION__)

#else
#error "NEVER INCLUDE astmm.h DIRECTLY!!"
#endif /* _OPENPBX_ASTMM_H */
#endif
