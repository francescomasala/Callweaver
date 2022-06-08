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
 * \brief Module Loader
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <dirent.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/module.h"
#include "openpbx/options.h"
#include "openpbx/config.h"
#include "openpbx/logger.h"
#include "openpbx/channel.h"
#include "openpbx/term.h"
#include "openpbx/manager.h"
#include "openpbx/cdr.h"
#include "openpbx/enum.h"
#include "openpbx/lock.h"
#include "openpbx/rtp.h"
#include "libltdl/ltdl.h"

#ifndef RTLD_NOW
#define RTLD_NOW 0
#endif

OPBX_MUTEX_DEFINE_STATIC(modlock);
OPBX_MUTEX_DEFINE_STATIC(reloadlock);

static struct module *module_list=NULL;
static int modlistver = 0;

struct module {
	int (*load_module)(void);
	int (*unload_module)(void);
	int (*usecount)(void);
	char *(*description)(void);
	int (*reload)(void);
	void *lib;
	char resource[256];
	struct module *next;
};

static struct loadupdate {
	int (*updater)(void);
	struct loadupdate *next;
} *updaters = NULL;

int opbx_unload_resource(const char *resource_name, int force)
{
	struct module *m, *ml = NULL;
	int res = -1;
	if (opbx_mutex_lock(&modlock))
		opbx_log(LOG_WARNING, "Failed to lock\n");
	m = module_list;
	while(m) {
		if (!strcasecmp(m->resource, resource_name)) {
			if ((res = m->usecount()) > 0)  {
				if (force) 
					opbx_log(LOG_WARNING, "Warning:  Forcing removal of module %s with use count %d\n", resource_name, res);
				else {
					opbx_log(LOG_WARNING, "Soft unload failed, '%s' has use count %d\n", resource_name, res);
					opbx_mutex_unlock(&modlock);
					return -1;
				}
			}
			res = m->unload_module();
			if (res) {
				opbx_log(LOG_WARNING, "Firm unload failed for %s\n", resource_name);
				if (force <= OPBX_FORCE_FIRM) {
					opbx_mutex_unlock(&modlock);
					return -1;
				} else
					opbx_log(LOG_WARNING, "** Dangerous **: Unloading resource anyway, at user request\n");
			}
			if (ml)
				ml->next = m->next;
			else
				module_list = m->next;
			lt_dlclose(m->lib);
			free(m);
			break;
		}
		ml = m;
		m = m->next;
	}
	modlistver = opbx_random();
	opbx_mutex_unlock(&modlock);
	opbx_update_use_count();
	return res;
}

char *opbx_module_helper(char *line, char *word, int pos, int state, int rpos, int needsreload)
{
	struct module *m;
	int which=0;
	char *ret;

	if (pos != rpos)
		return NULL;
	opbx_mutex_lock(&modlock);
	m = module_list;
	while(m) {
		if (!strncasecmp(word, m->resource, strlen(word)) && (m->reload || !needsreload)) {
			if (++which > state)
				break;
		}
		m = m->next;
	}
	if (m) {
		ret = strdup(m->resource);
	} else {
		ret = NULL;
		if (!strncasecmp(word, "extconfig", strlen(word))) {
			if (++which > state)
				ret = strdup("extconfig");
		} else if (!strncasecmp(word, "manager", strlen(word))) {
			if (++which > state)
				ret = strdup("manager");
		} else if (!strncasecmp(word, "enum", strlen(word))) {
			if (++which > state)
				ret = strdup("enum");
		} else if (!strncasecmp(word, "rtp", strlen(word))) {
			if (++which > state)
				ret = strdup("rtp");
		}
			
	}
	opbx_mutex_unlock(&modlock);
	return ret;
}

int opbx_module_reload(const char *name)
{
	struct module *m;
	int reloaded = 0;
	int oldversion;
	int (*reload)(void);
	/* We'll do the logger and manager the favor of calling its reload here first */

	if (opbx_mutex_trylock(&reloadlock)) {
		opbx_verbose("The previous reload command didn't finish yet\n");
		return -1;
	}
	if (!name || !strcasecmp(name, "extconfig")) {
		read_config_maps();
		reloaded = 2;
	}
	if (!name || !strcasecmp(name, "cdr")) {
		opbx_cdr_engine_reload();
		reloaded = 2;
	}
	if (!name || !strcasecmp(name, "enum")) {
		opbx_enum_reload();
		reloaded = 2;
	}
	if (!name || !strcasecmp(name, "rtp")) {
		opbx_rtp_reload();
		reloaded = 2;
	}
	if (!name || !strcasecmp(name, "dnsmgr")) {
		dnsmgr_reload();
		reloaded = 2;
	}
	time(&opbx_lastreloadtime);

	opbx_mutex_lock(&modlock);
	oldversion = modlistver;
	m = module_list;
	while(m) {
		if (!name || !strcasecmp(name, m->resource)) {
			if (reloaded < 1)
				reloaded = 1;
			reload = m->reload;
			opbx_mutex_unlock(&modlock);
			if (reload) {
				reloaded = 2;
				if (option_verbose > 2) 
					opbx_verbose(VERBOSE_PREFIX_3 "Reloading module '%s' (%s)\n", m->resource, m->description());
				reload();
			}
			opbx_mutex_lock(&modlock);
			if (oldversion != modlistver)
				break;
		}
		m = m->next;
	}
	opbx_mutex_unlock(&modlock);
	opbx_mutex_unlock(&reloadlock);
	return reloaded;
}

static int __load_resource(const char *resource_name, const struct opbx_config *cfg)
{
	static char fn[256];
	int errors=0;
	int res;
	struct module *m;
//	int flags=RTLD_NOW;
#ifdef RTLD_GLOBAL
	char *val;
#endif
	char tmp[80];

	if (strncasecmp(resource_name, "res_", 4)) {
#ifdef RTLD_GLOBAL
		if (cfg) {
			if ((val = opbx_variable_retrieve(cfg, "global", resource_name))
					&& opbx_true(val))
				// flags |= RTLD_GLOBAL;
		}
#endif
	} else {
		/* Resource modules are always loaded global and lazy */
#ifdef RTLD_GLOBAL
	// 	flags = (RTLD_GLOBAL | RTLD_LAZY);
#else
//		flags = RTLD_LAZY;
#endif
	}
	
	if (opbx_mutex_lock(&modlock))
		opbx_log(LOG_WARNING, "Failed to lock\n");
	m = module_list;
	while(m) {
		if (!strcasecmp(m->resource, resource_name)) {
			opbx_log(LOG_WARNING, "Module '%s' already exists\n", resource_name);
			opbx_mutex_unlock(&modlock);
			return -1;
		}
		m = m->next;
	}
	m = malloc(sizeof(struct module));	
	if (!m) {
		opbx_log(LOG_WARNING, "Out of memory\n");
		opbx_mutex_unlock(&modlock);
		return -1;
	}
	strncpy(m->resource, resource_name, sizeof(m->resource)-1);
	if (resource_name[0] == '/') {
		strncpy(fn, resource_name, sizeof(fn)-1);
	} else {
		snprintf(fn, sizeof(fn), "%s/%s", (char *)opbx_config_OPBX_MODULE_DIR, resource_name);
	}
	m->lib = lt_dlopen(fn); /* lt_dlopen takes only one argument, no flags ... we don't need no stinkin flags ... */
	if (!m->lib) {
		opbx_log(LOG_WARNING, "%s\n", lt_dlerror());
		free(m);
		opbx_mutex_unlock(&modlock);
		return -1;
	}
	m->load_module = lt_dlsym(m->lib, "load_module");
	if (m->load_module == NULL)
		m->load_module = lt_dlsym(m->lib, "_load_module");
	if (!m->load_module) {
		opbx_log(LOG_WARNING, "No load_module in module %s\n", fn);
		errors++;
	}
	m->unload_module = lt_dlsym(m->lib, "unload_module");
	if (m->unload_module == NULL)
		m->unload_module = lt_dlsym(m->lib, "_unload_module");
	if (!m->unload_module) {
		opbx_log(LOG_WARNING, "No unload_module in module %s\n", fn);
		errors++;
	}
	m->usecount = lt_dlsym(m->lib, "usecount");
	if (m->usecount == NULL)
		m->usecount = lt_dlsym(m->lib, "_usecount");
	if (!m->usecount) {
		opbx_log(LOG_WARNING, "No usecount in module %s\n", fn);
		errors++;
	}
	m->description = lt_dlsym(m->lib, "description");
	if (m->description == NULL)
		m->description = lt_dlsym(m->lib, "_description");
	if (!m->description) {
		opbx_log(LOG_WARNING, "No description in module %s\n", fn);
		errors++;
	}

	m->reload = lt_dlsym(m->lib, "reload");
	if (m->reload == NULL)
		m->reload = lt_dlsym(m->lib, "_reload");

	if (errors) {
		opbx_log(LOG_WARNING, "%d error%s loading module %s, aborted\n", errors, (errors != 1) ? "s" : "", fn);
		lt_dlclose(m->lib);
		free(m);
		opbx_mutex_unlock(&modlock);
		return -1;
	}
	if (!fully_booted) {
		if (option_verbose) 
			opbx_verbose( " => (%s)\n", opbx_term_color(tmp, m->description(), COLOR_BROWN, COLOR_BLACK, sizeof(tmp)));
		if (option_console && !option_verbose)
			opbx_verbose( ".");
	} else {
		if (option_verbose)
			opbx_verbose(VERBOSE_PREFIX_1 "Loaded %s => (%s)\n", fn, m->description());
	}

	/* add module 'm' to end of module_list chain
  	   so reload commands will be issued in same order modules were loaded */
	m->next = NULL;
	if (module_list == NULL) {
		/* empty list so far, add at front */
		module_list = m;
	}
	else {
		struct module *i;
		/* find end of chain, and add there */
		for (i = module_list; i->next; i = i->next)
			;
		i->next = m;
	}
	
	modlistver = opbx_random();
	opbx_mutex_unlock(&modlock);
	if ((res = m->load_module())) {
		opbx_log(LOG_WARNING, "%s: load_module failed, returning %d\n", m->resource, res);
		opbx_unload_resource(resource_name, 0);
		return -1;
	}
	opbx_update_use_count();
	return 0;
}

int opbx_load_resource(const char *resource_name)
{
	int o;
	struct opbx_config *cfg = NULL;
	int res;

	/* Keep the module file parsing silent */
	o = option_verbose;
	option_verbose = 0;
	cfg = opbx_config_load(OPBX_MODULE_CONFIG);
	option_verbose = o;
	res = __load_resource(resource_name, cfg);
	if (cfg)
		opbx_config_destroy(cfg);
	return res;
}	

static int opbx_resource_exists(char *resource)
{
	struct module *m;
	if (opbx_mutex_lock(&modlock))
		opbx_log(LOG_WARNING, "Failed to lock\n");
	m = module_list;
	while(m) {
		if (!strcasecmp(resource, m->resource))
			break;
		m = m->next;
	}
	opbx_mutex_unlock(&modlock);
	if (m)
		return -1;
	else
		return 0;
}

static const char *loadorder[] =
{
	"res_",
	"chan_",
	"pbx_",
	NULL,
};

int load_modules(const int preload_only)
{
	struct opbx_config *cfg;
	struct opbx_variable *v;
	char tmp[80];

	if (option_verbose) {
		if (preload_only)
			opbx_verbose("OpenPBX Dynamic Loader loading preload modules:\n");
		else
			opbx_verbose("OpenPBX Dynamic Loader Starting:\n");
	}

	cfg = opbx_config_load(OPBX_MODULE_CONFIG);
	if (cfg) {
		int doload;

		/* Load explicitly defined modules */
		for (v = opbx_variable_browse(cfg, "modules"); v; v = v->next) {
			doload = 0;

			if (preload_only)
				doload = !strcasecmp(v->name, "preload");
			else
				doload = !strcasecmp(v->name, "load");

		       if (doload) {
				if (option_debug && !option_verbose)
					opbx_log(LOG_DEBUG, "Loading module %s\n", v->value);
				if (option_verbose) {
					opbx_verbose(VERBOSE_PREFIX_1 "[%s]", opbx_term_color(tmp, v->value, COLOR_BRWHITE, 0, sizeof(tmp)));
					fflush(stdout);
				}
				if (__load_resource(v->value, cfg)) {
					opbx_log(LOG_WARNING, "Loading module %s failed!\n", v->value);
					opbx_config_destroy(cfg);
					return -1;
				}
			}
		}
	}

	if (preload_only) {
		opbx_config_destroy(cfg);
		return 0;
	}

	if (!cfg || opbx_true(opbx_variable_retrieve(cfg, "modules", "autoload"))) {
		/* Load all modules */
		DIR *mods;
		struct dirent *d;
		int x;

		/* Loop through each order */
		for (x=0; x<sizeof(loadorder) / sizeof(loadorder[0]); x++) {
			mods = opendir((char *)opbx_config_OPBX_MODULE_DIR);
			if (mods) {
				while((d = readdir(mods))) {
					/* Must end in .so to load it.  */
					if ((strlen(d->d_name) > 3) && 
					    (!loadorder[x] || !strncasecmp(d->d_name, loadorder[x], strlen(loadorder[x]))) && 
					    !strcasecmp(d->d_name + strlen(d->d_name) - 3, ".so") &&
						!opbx_resource_exists(d->d_name)) {
						/* It's a shared library -- Just be sure we're allowed to load it -- kinda
						   an inefficient way to do it, but oh well. */
						if (cfg) {
							v = opbx_variable_browse(cfg, "modules");
							while(v) {
								if (!strcasecmp(v->name, "noload") &&
								    !strcasecmp(v->value, d->d_name)) 
									break;
								v = v->next;
							}
							if (v) {
								if (option_verbose) {
									opbx_verbose( VERBOSE_PREFIX_1 "[skipping %s]\n", d->d_name);
									fflush(stdout);
								}
								continue;
							}
							
						}
						if (option_debug && !option_verbose)
							opbx_log(LOG_DEBUG, "Loading module %s\n", d->d_name);
						if (option_verbose) {
							opbx_verbose( VERBOSE_PREFIX_1 "[%s]", opbx_term_color(tmp, d->d_name, COLOR_BRWHITE, 0, sizeof(tmp)));
							fflush(stdout);
						}
						if (__load_resource(d->d_name, cfg)) {
							opbx_log(LOG_WARNING, "Loading module %s failed!\n", d->d_name);
							if (cfg)
								opbx_config_destroy(cfg);
							return -1;
						}
					}
				}
				closedir(mods);
			} else {
				if (!option_quiet)
					opbx_log(LOG_WARNING, "Unable to open modules directory %s.\n", (char *)opbx_config_OPBX_MODULE_DIR);
			}
		}
	} 
	opbx_config_destroy(cfg);
	return 0;
}

void opbx_update_use_count(void)
{
	/* Notify any module monitors that the use count for a 
	   resource has changed */
	struct loadupdate *m;
	if (opbx_mutex_lock(&modlock))
		opbx_log(LOG_WARNING, "Failed to lock\n");
	m = updaters;
	while(m) {
		m->updater();
		m = m->next;
	}
	opbx_mutex_unlock(&modlock);
	
}

int opbx_update_module_list(int (*modentry)(const char *module, const char *description, int usecnt, const char *like),
			   const char *like)
{
	struct module *m;
	int unlock = -1;
	int total_mod_loaded = 0;

	if (opbx_mutex_trylock(&modlock))
		unlock = 0;
	m = module_list;
	while (m) {
		total_mod_loaded += modentry(m->resource, m->description(), m->usecount(), like);
		m = m->next;
	}
	if (unlock)
		opbx_mutex_unlock(&modlock);

	return total_mod_loaded;
}

int opbx_loader_register(int (*v)(void)) 
{
	struct loadupdate *tmp;
	/* XXX Should be more flexible here, taking > 1 verboser XXX */
	if ((tmp = malloc(sizeof (struct loadupdate)))) {
		tmp->updater = v;
		if (opbx_mutex_lock(&modlock))
			opbx_log(LOG_WARNING, "Failed to lock\n");
		tmp->next = updaters;
		updaters = tmp;
		opbx_mutex_unlock(&modlock);
		return 0;
	}
	return -1;
}

int opbx_loader_unregister(int (*v)(void))
{
	int res = -1;
	struct loadupdate *tmp, *tmpl=NULL;
	if (opbx_mutex_lock(&modlock))
		opbx_log(LOG_WARNING, "Failed to lock\n");
	tmp = updaters;
	while(tmp) {
		if (tmp->updater == v)	{
			if (tmpl)
				tmpl->next = tmp->next;
			else
				updaters = tmp->next;
			break;
		}
		tmpl = tmp;
		tmp = tmp->next;
	}
	if (tmp)
		res = 0;
	opbx_mutex_unlock(&modlock);
	return res;
}

int opbx_loader_init(void)
{
	return lt_dlinit();
}

int opbx_loader_exit(void)
{
	return lt_dlexit();
}
