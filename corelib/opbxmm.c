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
 *
 * Memory Management
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#ifdef __OPBX_DEBUG_MALLOC

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/cli.h"
#include "openpbx/logger.h"
#include "openpbx/options.h"
#include "openpbx/lock.h"
#include "openpbx/strings.h"

#define SOME_PRIME 563

#define FUNC_CALLOC	1
#define FUNC_MALLOC	2
#define FUNC_REALLOC	3
#define FUNC_STRDUP	4
#define FUNC_STRNDUP	5
#define FUNC_VASPRINTF	6

/* Undefine all our macros */
#undef malloc
#undef calloc
#undef realloc
#undef strdup
#undef strndup
#undef free
#undef vasprintf

#define FENCE_MAGIC 0xdeadbeef

static FILE *mmlog;

static struct opbx_region {
	struct opbx_region *next;
	char file[40];
	char func[40];
	int lineno;
	int which;
	size_t len;
	unsigned int fence;
	unsigned char data[0];
} *regions[SOME_PRIME];

#define HASH(a) \
	(((unsigned long)(a)) % SOME_PRIME)
	
OPBX_MUTEX_DEFINE_STATIC(reglock);
OPBX_MUTEX_DEFINE_STATIC(showmemorylock);

static inline void *__opbx_alloc_region(size_t size, int which, const char *file, int lineno, const char *func)
{
	struct opbx_region *reg;
	void *ptr = NULL;
	unsigned int *fence;
	int hash;
	reg = malloc(size + sizeof(struct opbx_region) + sizeof(unsigned int));
	opbx_mutex_lock(&reglock);
	if (reg) {
		opbx_copy_string(reg->file, file, sizeof(reg->file));
		reg->file[sizeof(reg->file) - 1] = '\0';
		opbx_copy_string(reg->func, func, sizeof(reg->func));
		reg->func[sizeof(reg->func) - 1] = '\0';
		reg->lineno = lineno;
		reg->len = size;
		reg->which = which;
		ptr = reg->data;
		hash = HASH(ptr);
		reg->next = regions[hash];
		regions[hash] = reg;
		reg->fence = FENCE_MAGIC;
		fence = (ptr + reg->len);
		*fence = FENCE_MAGIC;
	}
	opbx_mutex_unlock(&reglock);
	if (!reg) {
		fprintf(stderr, "Out of memory :(\n");
		if (mmlog) {
			fprintf(mmlog, "%ld - Out of memory\n", time(NULL));
			fflush(mmlog);
		}
	}
	return ptr;
}

static inline size_t __opbx_sizeof_region(void *ptr)
{
	int hash = HASH(ptr);
	struct opbx_region *reg;
	size_t len = 0;
	
	opbx_mutex_lock(&reglock);
	reg = regions[hash];
	while (reg) {
		if (reg->data == ptr) {
			len = reg->len;
			break;
		}
		reg = reg->next;
	}
	opbx_mutex_unlock(&reglock);
	return len;
}

static void __opbx_free_region(void *ptr, const char *file, int lineno, const char *func)
{
	int hash = HASH(ptr);
	struct opbx_region *reg, *prev = NULL;
	unsigned int *fence;

	opbx_mutex_lock(&reglock);
	reg = regions[hash];
	while (reg) {
		if (reg->data == ptr) {
			if (prev) {
				prev->next = reg->next;
			} else {
				regions[hash] = reg->next;
			}
			break;
		}
		prev = reg;
		reg = reg->next;
	}
	opbx_mutex_unlock(&reglock);
	if (reg) {
		fence = (unsigned int *)(reg->data + reg->len);
		if (reg->fence != FENCE_MAGIC) {
			fprintf(stderr, "WARNING: Low fence violation at %p, in %s of %s, line %d\n", reg->data, reg->func, reg->file, reg->lineno);
			if (mmlog) {
				fprintf(mmlog, "%ld - WARNING: Low fence violation at %p, in %s of %s, line %d\n", time(NULL), reg->data, reg->func, reg->file, reg->lineno);
				fflush(mmlog);
			}
		}
		if (*fence != FENCE_MAGIC) {
			fprintf(stderr, "WARNING: High fence violation at %p, in %s of %s, line %d\n", reg->data, reg->func, reg->file, reg->lineno);
			if (mmlog) {
				fprintf(mmlog, "%ld - WARNING: High fence violation at %p, in %s of %s, line %d\n", time(NULL), reg->data, reg->func, reg->file, reg->lineno);
				fflush(mmlog);
			}
		}
		free(reg);
	} else {
		fprintf(stderr, "WARNING: Freeing unused memory at %p, in %s of %s, line %d\n",	ptr, func, file, lineno);
		if (mmlog) {
			fprintf(mmlog, "%ld - WARNING: Freeing unused memory at %p, in %s of %s, line %d\n", time(NULL), ptr, func, file, lineno);
			fflush(mmlog);
		}
	}
}

void *__opbx_calloc(size_t nmemb, size_t size, const char *file, int lineno, const char *func) 
{
	void *ptr;
	ptr = __opbx_alloc_region(size * nmemb, FUNC_CALLOC, file, lineno, func);
	if (ptr) 
		memset(ptr, 0, size * nmemb);
	return ptr;
}

void *__opbx_malloc(size_t size, const char *file, int lineno, const char *func) 
{
	return __opbx_alloc_region(size, FUNC_MALLOC, file, lineno, func);
}

void __opbx_free(void *ptr, const char *file, int lineno, const char *func) 
{
	__opbx_free_region(ptr, file, lineno, func);
}

void *__opbx_realloc(void *ptr, size_t size, const char *file, int lineno, const char *func) 
{
	void *tmp;
	size_t len = 0;
	if (ptr) {
		len = __opbx_sizeof_region(ptr);
		if (!len) {
			fprintf(stderr, "WARNING: Realloc of unalloced memory at %p, in %s of %s, line %d\n", ptr, func, file, lineno);
			if (mmlog) {
				fprintf(mmlog, "%ld - WARNING: Realloc of unalloced memory at %p, in %s of %s, line %d\n", time(NULL), ptr, func, file, lineno);
				fflush(mmlog);
			}
			return NULL;
		}
	}
	tmp = __opbx_alloc_region(size, FUNC_REALLOC, file, lineno, func);
	if (tmp) {
		if (len > size)
			len = size;
		if (ptr) {
			memcpy(tmp, ptr, len);
			__opbx_free_region(ptr, file, lineno, func);
		}
	}
	return tmp;
}

char *__opbx_strdup(const char *s, const char *file, int lineno, const char *func) 
{
	size_t len;
	void *ptr;
	if (!s)
		return NULL;
	len = strlen(s) + 1;
	ptr = __opbx_alloc_region(len, FUNC_STRDUP, file, lineno, func);
	if (ptr)
		strcpy(ptr, s);
	return ptr;
}

char *__opbx_strndup(const char *s, size_t n, const char *file, int lineno, const char *func) 
{
	size_t len;
	void *ptr;
	if (!s)
		return NULL;
	len = strlen(s) + 1;
	if (len > n)
		len = n;
	ptr = __opbx_alloc_region(len, FUNC_STRNDUP, file, lineno, func);
	if (ptr)
		strcpy(ptr, s);
	return ptr;
}

int __opbx_vasprintf(char **strp, const char *fmt, va_list ap, const char *file, int lineno, const char *func) 
{
	int n, size = strlen(fmt) + 1;
	if ((*strp = __opbx_alloc_region(size, FUNC_VASPRINTF, file, lineno, func)) == NULL)
		return -1; 
	for (;;) {
		n = vsnprintf(*strp, size, fmt, ap);
		if (n > -1 && n < size)
			return n;
		if (n > -1) {	/* glibc 2.1 */
			size = n+1;
		} else {	/* glibc 2.0 */
			size *= 2;
		}
		if ((*strp = __opbx_realloc(*strp, size, file, lineno, func)) == NULL)
			return -1;
	}
}

static int handle_show_memory(int fd, int argc, char *argv[])
{
	char *fn = NULL;
	int x;
	struct opbx_region *reg;
	unsigned int len = 0;
	int count = 0;
	unsigned int *fence;
	if (argc > 3) 
		fn = argv[3];

	/* try to lock applications list ... */
	opbx_mutex_lock(&showmemorylock);

	for (x = 0; x < SOME_PRIME; x++) {
		reg = regions[x];
		while (reg) {
			if (!fn || !strcasecmp(fn, reg->file) || !strcasecmp(fn, "anomolies")) {
				fence = (unsigned int *)(reg->data + reg->len);
				if (reg->fence != FENCE_MAGIC) {
					fprintf(stderr, "WARNING: Low fence violation at %p, in %s of %s, line %d\n", reg->data, reg->func, reg->file, reg->lineno);
					if (mmlog) {
						fprintf(mmlog, "%ld - WARNING: Low fence violation at %p, in %s of %s, line %d\n", time(NULL), reg->data, reg->func, reg-> file, reg->lineno);
						fflush(mmlog);
					}
				}
				if (*fence != FENCE_MAGIC) {
					fprintf(stderr, "WARNING: High fence violation at %p, in %s of %s, line %d\n", reg->data, reg->func, reg->file, reg->lineno);
					if (mmlog) {
						fprintf(mmlog, "%ld - WARNING: High fence violation at %p, in %s of %s, line %d\n", time(NULL), reg->data, reg->func, reg->file, reg->lineno);
						fflush(mmlog);
					}
				}
			}
			if (!fn || !strcasecmp(fn, reg->file)) {
				opbx_cli(fd, "%10d bytes allocated in %20s at line %5d of %s\n", (int) reg->len, reg->func, reg->lineno, reg->file);
				len += reg->len;
				count++;
			}
			reg = reg->next;
		}
	}
	opbx_cli(fd, "%d bytes allocated %d units total\n", len, count);
	opbx_mutex_unlock(&showmemorylock);
	return RESULT_SUCCESS;
}

struct file_summary {
	char fn[80];
	int len;
	int count;
	struct file_summary *next;
};

static int handle_show_memory_summary(int fd, int argc, char *argv[])
{
	char *fn = NULL;
	int x;
	struct opbx_region *reg;
	unsigned int len = 0;
	int count = 0;
	struct file_summary *list = NULL, *cur;
	
	if (argc > 3) 
		fn = argv[3];

	/* try to lock applications list ... */
	opbx_mutex_lock(&reglock);

	for (x = 0; x < SOME_PRIME; x++) {
		reg = regions[x];
		while (reg) {
			if (!fn || !strcasecmp(fn, reg->file)) {
				cur = list;
				while (cur) {
					if ((!fn && !strcmp(cur->fn, reg->file)) || (fn && !strcmp(cur->fn, reg->func)))
						break;
					cur = cur->next;
				}
				if (!cur) {
					cur = alloca(sizeof(struct file_summary));
					memset(cur, 0, sizeof(struct file_summary));
					opbx_copy_string(cur->fn, fn ? reg->func : reg->file, sizeof(cur->fn));
					cur->next = list;
					list = cur;
				}
				cur->len += reg->len;
				cur->count++;
			}
			reg = reg->next;
		}
	}
	opbx_mutex_unlock(&reglock);
	
	/* Dump the whole list */
	while (list) {
		cur = list;
		len += list->len;
		count += list->count;
		if (fn) {
			opbx_cli(fd, "%10d bytes in %5d allocations in function '%s' of '%s'\n", list->len, list->count, list->fn, fn);
		} else {
			opbx_cli(fd, "%10d bytes in %5d allocations in file '%s'\n", list->len, list->count, list->fn);
		}
		list = list->next;
#if 0
		free(cur);
#endif		
	}
	opbx_cli(fd, "%d bytes allocated %d units total\n", len, count);
	return RESULT_SUCCESS;
}

static char show_memory_help[] = 
"Usage: show memory allocations [<file>]\n"
"       Dumps a list of all segments of allocated memory, optionally\n"
"limited to those from a specific file\n";

static char show_memory_summary_help[] = 
"Usage: show memory summary [<file>]\n"
"       Summarizes heap memory allocations by file, or optionally\n"
"by function, if a file is specified\n";

static struct opbx_cli_entry show_memory_allocations_cli = 
	{ { "show", "memory", "allocations", NULL }, 
	handle_show_memory, "Display outstanding memory allocations",
	show_memory_help };

static struct opbx_cli_entry show_memory_summary_cli = 
	{ { "show", "memory", "summary", NULL }, 
	handle_show_memory_summary, "Summarize outstanding memory allocations",
	show_memory_summary_help };

void __opbx_mm_init(void)
{
	char filename[80] = "";
	opbx_cli_register(&show_memory_allocations_cli);
	opbx_cli_register(&show_memory_summary_cli);
	
	snprintf(filename, sizeof(filename), "%s/mmlog", (char *)opbx_config_OPBX_LOG_DIR);
	mmlog = fopen(filename, "a+");
	if (option_verbose)
		opbx_verbose("OpenPBX Malloc Debugger Started (see %s))\n", filename);
	if (mmlog) {
		fprintf(mmlog, "%ld - New session\n", time(NULL));
		fflush(mmlog);
	}
}

#endif
