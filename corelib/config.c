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
 *
 * \brief Configuration File Parser
 *
 * Includes the OpenPBX Realtime API - ARA
 * See README.realtime
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#define OPBX_INCLUDE_GLOB 1
#ifdef OPBX_INCLUDE_GLOB
#include <glob.h>
#if defined(__CYGWIN__)
#define GLOB_ABORTED GLOB_ABEND
#endif
#endif

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/config.h"
#include "openpbx/cli.h"
#include "openpbx/lock.h"
#include "openpbx/options.h"
#include "openpbx/logger.h"
#include "openpbx/utils.h"
#include "openpbx/channel.h"
#include "openpbx/app.h"

#define MAX_NESTED_COMMENTS 128
#define COMMENT_START ";--"
#define COMMENT_END "--;"
#define COMMENT_META ';'
#define COMMENT_TAG '-'

static char *extconfig_conf = "extconfig.conf";

static struct opbx_config_map {
	struct opbx_config_map *next;
	char *name;
	char *driver;
	char *database;
	char *table;
	char stuff[0];
} *config_maps = NULL;

OPBX_MUTEX_DEFINE_STATIC(config_lock);
static struct opbx_config_engine *config_engine_list;

#define MAX_INCLUDE_LEVEL 10

struct opbx_comment {
	struct opbx_comment *next;
	char cmt[0];
};

struct opbx_category {
	char name[80];
	int ignored;			/* do not let user of the config see this category */
	struct opbx_variable *root;
	struct opbx_variable *last;
	struct opbx_category *next;
};

struct opbx_config {
	struct opbx_category *root;
	struct opbx_category *last;
	struct opbx_category *current;
	struct opbx_category *lopbx_browse;		/* used to cache the last category supplied via category_browse */
	int include_level;
	int max_include_level;
};

struct opbx_variable *opbx_variable_new(const char *name, const char *value) 
{
	struct opbx_variable *variable;

	int length = strlen(name) + strlen(value) + 2 + sizeof(struct opbx_variable);
	variable = malloc(length);
	if (variable) {
		memset(variable, 0, length);
		variable->name = variable->stuff;
		variable->value = variable->stuff + strlen(name) + 1;		
		strcpy(variable->name,name);
		strcpy(variable->value,value);
	}

	return variable;
}

void opbx_variable_append(struct opbx_category *category, struct opbx_variable *variable)
{
	if (category->last)
		category->last->next = variable;
	else
		category->root = variable;
	category->last = variable;
}

void opbx_variables_destroy(struct opbx_variable *v)
{
	struct opbx_variable *vn;

	while(v) {
		vn = v;
		v = v->next;
		free(vn);
	}
}

struct opbx_variable *opbx_variable_browse(const struct opbx_config *config, const char *category)
{
	struct opbx_category *cat = NULL;

	if (category && config->lopbx_browse && (config->lopbx_browse->name == category))
		cat = config->lopbx_browse;
	else
		cat = opbx_category_get(config, category);

	if (cat)
		return cat->root;
	else
		return NULL;
}

char *opbx_variable_retrieve(const struct opbx_config *config, const char *category, const char *variable)
{
	struct opbx_variable *v;

	if (category) {
		for (v = opbx_variable_browse(config, category); v; v = v->next) {
			if (!strcasecmp(variable, v->name))
				return v->value;
		}
	} else {
		struct opbx_category *cat;

		for (cat = config->root; cat; cat = cat->next)
			for (v = cat->root; v; v = v->next)
				if (!strcasecmp(variable, v->name))
					return v->value;
	}

	return NULL;
}

static struct opbx_variable *variable_clone(const struct opbx_variable *old)
{
	struct opbx_variable *new = opbx_variable_new(old->name, old->value);

	if (new) {
		new->lineno = old->lineno;
		new->object = old->object;
		new->blanklines = old->blanklines;
		/* TODO: clone comments? */
	}

	return new;
}
 
static void move_variables(struct opbx_category *old, struct opbx_category *new)
{
	struct opbx_variable *var;
	struct opbx_variable *next;

	next = old->root;
	old->root = NULL;
	for (var = next; var; var = next) {
		next = var->next;
		var->next = NULL;
		opbx_variable_append(new, var);
	}
}

struct opbx_category *opbx_category_new(const char *name) 
{
	struct opbx_category *category;

	category = malloc(sizeof(struct opbx_category));
	if (category) {
		memset(category, 0, sizeof(struct opbx_category));
		opbx_copy_string(category->name, name, sizeof(category->name));
	}

	return category;
}

static struct opbx_category *category_get(const struct opbx_config *config, const char *category_name, int ignored)
{
	struct opbx_category *cat;

	for (cat = config->root; cat; cat = cat->next) {
		if (cat->name == category_name && (ignored || !cat->ignored))
			return cat;
	}

	for (cat = config->root; cat; cat = cat->next) {
		if (!strcasecmp(cat->name, category_name) && (ignored || !cat->ignored))
			return cat;
	}

	return NULL;
}

struct opbx_category *opbx_category_get(const struct opbx_config *config, const char *category_name)
{
	return category_get(config, category_name, 0);
}

int opbx_category_exist(const struct opbx_config *config, const char *category_name)
{
	return !!opbx_category_get(config, category_name);
}

void opbx_category_append(struct opbx_config *config, struct opbx_category *category)
{
	if (config->last)
		config->last->next = category;
	else
		config->root = category;
	config->last = category;
	config->current = category;
}

void opbx_category_destroy(struct opbx_category *cat)
{
	opbx_variables_destroy(cat->root);
	free(cat);
}

static struct opbx_category *next_available_category(struct opbx_category *cat)
{
	for (; cat && cat->ignored; cat = cat->next);

	return cat;
}

char *opbx_category_browse(struct opbx_config *config, const char *prev)
{	
	struct opbx_category *cat = NULL;

	if (prev && config->lopbx_browse && (config->lopbx_browse->name == prev))
		cat = config->lopbx_browse->next;
	else if (!prev && config->root)
			cat = config->root;
	else if (prev) {
		for (cat = config->root; cat; cat = cat->next) {
			if (cat->name == prev) {
				cat = cat->next;
				break;
			}
		}
		if (!cat) {
			for (cat = config->root; cat; cat = cat->next) {
				if (!strcasecmp(cat->name, prev)) {
					cat = cat->next;
					break;
				}
			}
		}
	}
	
	if (cat)
		cat = next_available_category(cat);

	config->lopbx_browse = cat;
	if (cat)
		return cat->name;
	else
		return NULL;
}

struct opbx_variable *opbx_category_detach_variables(struct opbx_category *cat)
{
	struct opbx_variable *v;

	v = cat->root;
	cat->root = NULL;

	return v;
}

void opbx_category_rename(struct opbx_category *cat, const char *name)
{
	opbx_copy_string(cat->name, name, sizeof(cat->name));
}

static void inherit_category(struct opbx_category *new, const struct opbx_category *base)
{
	struct opbx_variable *var;

	for (var = base->root; var; var = var->next) {
		struct opbx_variable *v;
		
		v = variable_clone(var);
		if (v)
			opbx_variable_append(new, v);
	}
}

struct opbx_config *opbx_config_new(void) 
{
	struct opbx_config *config;

	config = malloc(sizeof(*config));
	if (config) {
		memset(config, 0, sizeof(*config));
		config->max_include_level = MAX_INCLUDE_LEVEL;
	}

	return config;
}

void opbx_config_destroy(struct opbx_config *cfg)
{
	struct opbx_category *cat, *catn;

	if (!cfg)
		return;

	cat = cfg->root;
	while(cat) {
		opbx_variables_destroy(cat->root);
		catn = cat;
		cat = cat->next;
		free(catn);
	}
	free(cfg);
}

struct opbx_category *opbx_config_get_current_category(const struct opbx_config *cfg)
{
	return cfg->current;
}

void opbx_config_set_current_category(struct opbx_config *cfg, const struct opbx_category *cat)
{
	/* cast below is just to silence compiler warning about dropping "const" */
	cfg->current = (struct opbx_category *) cat;
}

static int process_text_line(struct opbx_config *cfg, struct opbx_category **cat, char *buf, int lineno, const char *configfile)
{
	char *c;
	char *cur = buf;
	struct opbx_variable *v;
	char cmd[512], exec_file[512];
	int object, do_exec, do_include;

	/* Actually parse the entry */
	if (cur[0] == '[') {
		struct opbx_category *newcat = NULL;
		char *catname;

		/* A category header */
		c = strchr(cur, ']');
		if (!c) {
			opbx_log(LOG_WARNING, "parse error: no closing ']', line %d of %s\n", lineno, configfile);
			return -1;
		}
		*c++ = '\0';
		cur++;
 		if (*c++ != '(')
 			c = NULL;
		catname = cur;
		*cat = newcat = opbx_category_new(catname);
		if (!newcat) {
			opbx_log(LOG_WARNING, "Out of memory, line %d of %s\n", lineno, configfile);
			return -1;
		}
 		/* If there are options or categories to inherit from, process them now */
 		if (c) {
 			if (!(cur = strchr(c, ')'))) {
 				opbx_log(LOG_WARNING, "parse error: no closing ')', line %d of %s\n", lineno, configfile);
 				return -1;
 			}
 			*cur = '\0';
 			while ((cur = strsep(&c, ","))) {
				if (!strcasecmp(cur, "!")) {
					(*cat)->ignored = 1;
				} else if (!strcasecmp(cur, "+")) {
					*cat = category_get(cfg, catname, 1);
					if (!*cat) {
						opbx_config_destroy(cfg);
						if (newcat)
							opbx_category_destroy(newcat);
						opbx_log(LOG_WARNING, "Category addition requested, but category '%s' does not exist, line %d of %s\n", catname, lineno, configfile);
						return -1;
					}
					if (newcat) {
						move_variables(newcat, *cat);
						opbx_category_destroy(newcat);
						newcat = NULL;
					}
				} else {
					struct opbx_category *base;
 				
					base = category_get(cfg, cur, 1);
					if (!base) {
						opbx_log(LOG_WARNING, "Inheritance requested, but category '%s' does not exist, line %d of %s\n", cur, lineno, configfile);
						return -1;
					}
					inherit_category(*cat, base);
				}
 			}
 		}
		if (newcat)
			opbx_category_append(cfg, *cat);
	} else if (cur[0] == '#') {
		/* A directive */
		cur++;
		c = cur;
		while(*c && (*c > 32)) c++;
		if (*c) {
			*c = '\0';
			c++;
			/* Find real argument */
			while(*c  && (*c < 33)) c++;
			if (!*c)
				c = NULL;
		} else 
			c = NULL;
		do_include = !strcasecmp(cur, "include");
		if(!do_include)
			do_exec = !strcasecmp(cur, "exec");
		else
			do_exec = 0;
		if (do_exec && !option_exec_includes) {
			opbx_log(LOG_WARNING, "Cannot perform #exec unless execincludes option is enabled in openpbx.conf (options section)!\n");
			do_exec = 0;
		}
		if (do_include || do_exec) {
			if (c) {
				/* Strip off leading and trailing "'s and <>'s */
				while((*c == '<') || (*c == '>') || (*c == '\"')) c++;
				/* Get rid of leading mess */
				cur = c;
				while (!opbx_strlen_zero(cur)) {
					c = cur + strlen(cur) - 1;
					if ((*c == '>') || (*c == '<') || (*c == '\"'))
						*c = '\0';
					else
						break;
				}
				/* #exec </path/to/executable>
				   We create a tmp file, then we #include it, then we delete it. */
				if (do_exec) { 
					snprintf(exec_file, sizeof(exec_file), "/var/tmp/exec.%ld.%ld", time(NULL), (long)pthread_self());
					snprintf(cmd, sizeof(cmd), "%s > %s 2>&1", cur, exec_file);
					opbx_safe_system(cmd);
					cur = exec_file;
				} else
					exec_file[0] = '\0';
				/* A #include */
				do_include = opbx_config_internal_load(cur, cfg) ? 1 : 0;
				if(!opbx_strlen_zero(exec_file))
					unlink(exec_file);
				if(!do_include)
					return 0;

			} else {
				opbx_log(LOG_WARNING, "Directive '#%s' needs an argument (%s) at line %d of %s\n", 
						do_exec ? "exec" : "include",
						do_exec ? "/path/to/executable" : "filename",
						lineno,
						configfile);
			}
		}
		else 
			opbx_log(LOG_WARNING, "Unknown directive '%s' at line %d of %s\n", cur, lineno, configfile);
	} else {
		/* Just a line (variable = value) */
		if (!*cat) {
			opbx_log(LOG_WARNING,
				"parse error: No category context for line %d of %s\n", lineno, configfile);
			return -1;
		}
		c = strchr(cur, '=');
		if (c) {
			*c = 0;
			c++;
			/* Ignore > in => */
			if (*c== '>') {
				object = 1;
				c++;
			} else
				object = 0;
			v = opbx_variable_new(opbx_strip(cur), opbx_strip(c));
			if (v) {
				v->lineno = lineno;
				v->object = object;
				/* Put and reset comments */
				v->blanklines = 0;
				opbx_variable_append(*cat, v);
			} else {
				opbx_log(LOG_WARNING, "Out of memory, line %d\n", lineno);
				return -1;
			}
		} else {
			opbx_log(LOG_WARNING, "No '=' (equal sign) in line %d of %s\n", lineno, configfile);
		}

	}
	return 0;
}

static struct opbx_config *config_text_file_load(const char *database, const char *table, const char *filename, struct opbx_config *cfg)
{
	char fn[256];
	char buf[8192];
	char *new_buf, *comment_p, *process_buf;
	FILE *f;
	int lineno=0;
	int comment = 0, nest[MAX_NESTED_COMMENTS];
	struct opbx_category *cat = NULL;
	int count = 0;
	struct stat statbuf;
	
	cat = opbx_config_get_current_category(cfg);

	if (filename[0] == '/') {
		opbx_copy_string(fn, filename, sizeof(fn));
	} else {
		snprintf(fn, sizeof(fn), "%s/%s", (char *)opbx_config_OPBX_CONFIG_DIR, filename);
	}

#ifdef OPBX_INCLUDE_GLOB
	{
		int glob_ret;
		glob_t globbuf;
		globbuf.gl_offs = 0;	/* initialize it to silence gcc */
#ifdef SOLARIS
		glob_ret = glob(fn, GLOB_NOCHECK, NULL, &globbuf);
#else
		glob_ret = glob(fn, GLOB_NOMAGIC|GLOB_BRACE, NULL, &globbuf);
#endif
		if (glob_ret == GLOB_NOSPACE)
			opbx_log(LOG_WARNING,
				"Glob Expansion of pattern '%s' failed: Not enough memory\n", fn);
		else if (glob_ret  == GLOB_ABORTED)
			opbx_log(LOG_WARNING,
				"Glob Expansion of pattern '%s' failed: Read error\n", fn);
		else  {
			/* loop over expanded files */
			int i;
			for (i=0; i<globbuf.gl_pathc; i++) {
				opbx_copy_string(fn, globbuf.gl_pathv[i], sizeof(fn));
#endif
	do {
		if (stat(fn, &statbuf)) {
			opbx_log(LOG_WARNING, "Cannot stat() '%s', ignoring\n", fn);
			continue;
		}
		if (!S_ISREG(statbuf.st_mode)) {
			opbx_log(LOG_WARNING, "'%s' is not a regular file, ignoring\n", fn);
			continue;
		}
		if ((option_verbose > 3) && !option_debug) {
			opbx_verbose(VERBOSE_PREFIX_2 "Parsing '%s': ", fn);
			fflush(stdout);
		}
		if (!(f = fopen(fn, "r"))) {
			if (option_debug)
				opbx_log(LOG_DEBUG, "No file to parse: %s\n", fn);
			if (option_verbose > 1)
				opbx_verbose( "Not found (%s)\n", strerror(errno));
			continue;
		}
		count++;
		if (option_debug)
			opbx_log(LOG_DEBUG, "Parsing %s\n", fn);
		if (option_verbose > 1)
			opbx_verbose("Found\n");
		while(!feof(f)) {
			lineno++;
			if (fgets(buf, sizeof(buf), f)) {
				new_buf = buf;
				if (comment)
					process_buf = NULL;
				else
					process_buf = buf;
				while ((comment_p = strchr(new_buf, COMMENT_META))) {
					if ((comment_p > new_buf) && (*(comment_p-1) == '\\')) {
						/* Yuck, gotta memmove */
						memmove(comment_p - 1, comment_p, strlen(comment_p) + 1);
						new_buf = comment_p;
					} else if(comment_p[1] == COMMENT_TAG && comment_p[2] == COMMENT_TAG && (comment_p[3] != '-')) {
						/* Meta-Comment start detected ";--" */
						if (comment < MAX_NESTED_COMMENTS) {
							*comment_p = '\0';
							new_buf = comment_p + 3;
							comment++;
							nest[comment-1] = lineno;
						} else {
							opbx_log(LOG_ERROR, "Maximum nest limit of %d reached.\n", MAX_NESTED_COMMENTS);
						}
					} else if ((comment_p >= new_buf + 2) &&
						   (*(comment_p - 1) == COMMENT_TAG) &&
						   (*(comment_p - 2) == COMMENT_TAG)) {
						/* Meta-Comment end detected */
						comment--;
						new_buf = comment_p + 1;
						if (!comment) {
							/* Back to non-comment now */
							if (process_buf) {
								/* Actually have to move what's left over the top, then continue */
								char *oldptr;
								oldptr = process_buf + strlen(process_buf);
								memmove(oldptr, new_buf, strlen(new_buf) + 1);
								new_buf = oldptr;
							} else
								process_buf = new_buf;
						}
					} else {
						if (!comment) {
							/* If ; is found, and we are not nested in a comment, 
							   we immediately stop all comment processing */
							*comment_p = '\0'; 
							new_buf = comment_p;
						} else
							new_buf = comment_p + 1;
					}
				}
				if (process_buf) {
					char *buf = opbx_strip(process_buf);
					if (!opbx_strlen_zero(buf)) {
						if (process_text_line(cfg, &cat, buf, lineno, filename)) {
							cfg = NULL;
							break;
						}
					}
				}
			}
		}
		fclose(f);		
	} while(0);
	if (comment) {
		opbx_log(LOG_WARNING,"Unterminated comment detected beginning on line %d\n", nest[comment]);
	}
#ifdef OPBX_INCLUDE_GLOB
					if (!cfg)
						break;
				}
				globfree(&globbuf);
			}
		}
#endif
	if (count == 0)
		return NULL;

	return cfg;
}

int config_text_file_save(const char *configfile, const struct opbx_config *cfg, const char *generator)
{
	FILE *f;
	char fn[256];
	char date[256]="";
	time_t t;
	struct opbx_variable *var;
	struct opbx_category *cat;
	int blanklines = 0;

	if (configfile[0] == '/') {
		opbx_copy_string(fn, configfile, sizeof(fn));
	} else {
		snprintf(fn, sizeof(fn), "%s/%s", opbx_config_OPBX_CONFIG_DIR, configfile);
	}
	time(&t);
	opbx_copy_string(date, ctime(&t), sizeof(date));
#ifdef __CYGWIN__	
	if ((f = fopen(fn, "w+"))) {
#else
 	if ((f = fopen(fn, "w"))) {
#endif
		if ((option_verbose > 1) && !option_debug)
			opbx_verbose(  VERBOSE_PREFIX_2 "Saving '%s': ", fn);
		fprintf(f, ";!\n");
		fprintf(f, ";! Automatically generated configuration file\n");
		fprintf(f, ";! Filename: %s (%s)\n", configfile, fn);
		fprintf(f, ";! Generator: %s\n", generator);
		fprintf(f, ";! Creation Date: %s", date);
		fprintf(f, ";!\n");
		cat = cfg->root;
		while(cat) {
			/* Dump section with any appropriate comment */
			fprintf(f, "[%s]\n", cat->name);
			var = cat->root;
			while(var) {
				if (var->sameline) 
					fprintf(f, "%s %s %s  ; %s\n", var->name, (var->object ? "=>" : "="), var->value, var->sameline->cmt);
				else	
					fprintf(f, "%s %s %s\n", var->name, (var->object ? "=>" : "="), var->value);
				if (var->blanklines) {
					blanklines = var->blanklines;
					while (blanklines--)
						fprintf(f, "\n");
				}
					
				var = var->next;
			}
#if 0
			/* Put an empty line */
			fprintf(f, "\n");
#endif
			cat = cat->next;
		}
	} else {
		if (option_debug)
			printf("Unable to open for writing: %s\n", fn);
		else if (option_verbose > 1)
			printf( "Unable to write (%s)", strerror(errno));
		return -1;
	}
	fclose(f);
	return 0;
}

static void clear_config_maps(void) 
{
	struct opbx_config_map *map;

	opbx_mutex_lock(&config_lock);

	while (config_maps) {
		map = config_maps;
		config_maps = config_maps->next;
		free(map);
	}
		
	opbx_mutex_unlock(&config_lock);
}

static int append_mapping(char *name, char *driver, char *database, char *table)
{
	struct opbx_config_map *map;
	int length;

	length = sizeof(*map);
	length += strlen(name) + 1;
	length += strlen(driver) + 1;
	length += strlen(database) + 1;
	if (table)
		length += strlen(table) + 1;
	map = malloc(length);

	if (!map)
		return -1;

	memset(map, 0, length);
	map->name = map->stuff;
	strcpy(map->name, name);
	map->driver = map->name + strlen(map->name) + 1;
	strcpy(map->driver, driver);
	map->database = map->driver + strlen(map->driver) + 1;
	strcpy(map->database, database);
	if (table) {
		map->table = map->database + strlen(map->database) + 1;
		strcpy(map->table, table);
	}
	map->next = config_maps;

	if (option_verbose > 1)
		opbx_verbose(VERBOSE_PREFIX_2 "Binding %s to %s/%s/%s\n",
			    map->name, map->driver, map->database, map->table ? map->table : map->name);

	config_maps = map;
	return 0;
}

void read_config_maps(void) 
{
	struct opbx_config *config, *configtmp;
	struct opbx_variable *v;
	char *driver, *table, *database, *stringp;

	clear_config_maps();

	configtmp = opbx_config_new();
	configtmp->max_include_level = 1;
	config = opbx_config_internal_load(extconfig_conf, configtmp);
	if (!config) {
		opbx_config_destroy(configtmp);
		return;
	}

	for (v = opbx_variable_browse(config, "settings"); v; v = v->next) {
		stringp = v->value;
		driver = strsep(&stringp, ",");
		database = strsep(&stringp, ",");
		table = strsep(&stringp, ",");
			
		if (!strcmp(v->name, extconfig_conf)) {
			opbx_log(LOG_WARNING, "Cannot bind '%s'!\n", extconfig_conf);
			continue;
		}

		if (!strcmp(v->name, "openpbx.conf")) {
			opbx_log(LOG_WARNING, "Cannot bind 'openpbx.conf'!\n");
			continue;
		}

		if (!strcmp(v->name, "logger.conf")) {
			opbx_log(LOG_WARNING, "Cannot bind 'logger.conf'!\n");
			continue;
		}

		if (!driver || !database)
			continue;
		if (!strcasecmp(v->name, "sipfriends")) {
			opbx_log(LOG_WARNING, "The 'sipfriends' table is obsolete, update your config to use sipusers and sippeers, though they can point to the same table.\n");
			append_mapping("sipusers", driver, database, table ? table : "sipfriends");
			append_mapping("sippeers", driver, database, table ? table : "sipfriends");
		} else if (!strcasecmp(v->name, "iaxfriends")) {
			opbx_log(LOG_WARNING, "The 'iaxfriends' table is obsolete, update your config to use iaxusers and iaxpeers, though they can point to the same table.\n");
			append_mapping("iaxusers", driver, database, table ? table : "iaxfriends");
			append_mapping("iaxpeers", driver, database, table ? table : "iaxfriends");
		} else 
			append_mapping(v->name, driver, database, table);
	}
		
	opbx_config_destroy(config);
}

int opbx_config_engine_register(struct opbx_config_engine *new) 
{
	struct opbx_config_engine *ptr;

	opbx_mutex_lock(&config_lock);

	if (!config_engine_list) {
		config_engine_list = new;
	} else {
		for (ptr = config_engine_list; ptr->next; ptr=ptr->next);
		ptr->next = new;
	}

	opbx_mutex_unlock(&config_lock);
	opbx_log(LOG_NOTICE,"Registered Config Engine %s\n", new->name);

	return 1;
}

int opbx_config_engine_deregister(struct opbx_config_engine *del) 
{
	struct opbx_config_engine *ptr, *last=NULL;

	opbx_mutex_lock(&config_lock);

	for (ptr = config_engine_list; ptr; ptr=ptr->next) {
		if (ptr == del) {
			if (last)
				last->next = ptr->next;
			else
				config_engine_list = ptr->next;
			break;
		}
		last = ptr;
	}

	opbx_mutex_unlock(&config_lock);

	return 0;
}

/*--- find_engine: Find realtime engine for realtime family */
static struct opbx_config_engine *find_engine(const char *family, char *database, int dbsiz, char *table, int tabsiz) 
{
	struct opbx_config_engine *eng, *ret = NULL;
	struct opbx_config_map *map;

	opbx_mutex_lock(&config_lock);

	for (map = config_maps; map; map = map->next) {
		if (!strcasecmp(family, map->name)) {
			if (database)
				opbx_copy_string(database, map->database, dbsiz);
			if (table)
				opbx_copy_string(table, map->table ? map->table : family, tabsiz);
			break;
		}
	}

	/* Check if the required driver (engine) exist */
	if (map) {
		for (eng = config_engine_list; !ret && eng; eng = eng->next) {
			if (!strcasecmp(eng->name, map->driver))
				ret = eng;
		}
	}

	opbx_mutex_unlock(&config_lock);
	
	/* if we found a mapping, but the engine is not available, then issue a warning */
	if (map && !ret)
		opbx_log(LOG_WARNING, "Realtime mapping for '%s' found to engine '%s', but the engine is not available\n", map->name, map->driver);

	return ret;
}

static struct opbx_config_engine text_file_engine = {
	.name = "text",
	.load_func = config_text_file_load,
};

struct opbx_config *opbx_config_internal_load(const char *filename, struct opbx_config *cfg)
{
	char db[256];
	char table[256];
	struct opbx_config_engine *loader = &text_file_engine;
	struct opbx_config *result;

	if (cfg->include_level == cfg->max_include_level) {
		opbx_log(LOG_WARNING, "Maximum Include level (%d) exceeded\n", cfg->max_include_level);
		return NULL;
	}

	cfg->include_level++;

	if (strcmp(filename, extconfig_conf) && strcmp(filename, "openpbx.conf") && config_engine_list) {
		struct opbx_config_engine *eng;

		eng = find_engine(filename, db, sizeof(db), table, sizeof(table));


		if (eng && eng->load_func) {
			loader = eng;
		} else {
			eng = find_engine("global", db, sizeof(db), table, sizeof(table));
			if (eng && eng->load_func)
				loader = eng;
		}
	}

	result = loader->load_func(db, table, filename, cfg);

	if (result)
		result->include_level--;

	return result;
}

struct opbx_config *opbx_config_load(const char *filename)
{
	struct opbx_config *cfg;
	struct opbx_config *result;

	cfg = opbx_config_new();
	if (!cfg)
		return NULL;

	result = opbx_config_internal_load(filename, cfg);
	if (!result)
		opbx_config_destroy(cfg);

	return result;
}

struct opbx_variable *opbx_load_realtime(const char *family, ...)
{
	struct opbx_config_engine *eng;
	char db[256]="";
	char table[256]="";
	struct opbx_variable *res=NULL;
	va_list ap;

	va_start(ap, family);
	eng = find_engine(family, db, sizeof(db), table, sizeof(table));
	if (eng && eng->realtime_func) 
		res = eng->realtime_func(db, table, ap);
	va_end(ap);

	return res;
}

/*--- opbx_check_realtime: Check if realtime engine is configured for family */
int opbx_check_realtime(const char *family)
{
	struct opbx_config_engine *eng;

	eng = find_engine(family, NULL, 0, NULL, 0);
	if (eng)
		return 1;
	return 0;

}

struct opbx_config *opbx_load_realtime_multientry(const char *family, ...)
{
	struct opbx_config_engine *eng;
	char db[256]="";
	char table[256]="";
	struct opbx_config *res=NULL;
	va_list ap;

	va_start(ap, family);
	eng = find_engine(family, db, sizeof(db), table, sizeof(table));
	if (eng && eng->realtime_multi_func) 
		res = eng->realtime_multi_func(db, table, ap);
	va_end(ap);

	return res;
}

int opbx_update_realtime(const char *family, const char *keyfield, const char *lookup, ...)
{
	struct opbx_config_engine *eng;
	int res = -1;
	char db[256]="";
	char table[256]="";
	va_list ap;

	va_start(ap, lookup);
	eng = find_engine(family, db, sizeof(db), table, sizeof(table));
	if (eng && eng->update_func) 
		res = eng->update_func(db, table, keyfield, lookup, ap);
	va_end(ap);

	return res;
}

static int config_command(int fd, int argc, char **argv) 
{
	struct opbx_config_engine *eng;
	struct opbx_config_map *map;
	
	opbx_mutex_lock(&config_lock);

	opbx_cli(fd, "\n\n");
	for (eng = config_engine_list; eng; eng = eng->next) {
		opbx_cli(fd, "\nConfig Engine: %s\n", eng->name);
		for (map = config_maps; map; map = map->next)
			if (!strcasecmp(map->driver, eng->name)) {
				opbx_cli(fd, "===> %s (db=%s, table=%s)\n", map->name, map->database,
					map->table ? map->table : map->name);
			}
	}
	opbx_cli(fd,"\n\n");
	
	opbx_mutex_unlock(&config_lock);

	return 0;
}

static char show_config_help[] =
	"Usage: show config mappings\n"
	"	Shows the filenames to config engines.\n";

static struct opbx_cli_entry config_command_struct = {
	{ "show", "config", "mappings", NULL }, config_command, "Show Config mappings (file names to config engines)", show_config_help, NULL
};

int register_config_cli() 
{
	return opbx_cli_register(&config_command_struct);
}
