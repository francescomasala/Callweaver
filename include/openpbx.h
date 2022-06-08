/*
 * OpenPBX -- A telephony toolkit for Linux.
 *
 * General Definitions for OpenPBX top level program
 * 
 * Copyright (C) 1999-2005, Mark Spencer
 *
 * Mark Spencer <markster@digium.com>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */

/*! \file
 * \brief OpenPBX main include file. File version handling, generic pbx functions.
*/
#ifndef _OPENPBX_H
#define _OPENPBX_H

//#define RELEASE_TARBALL 1

#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#define DEFAULT_LANGUAGE "en"

#define OPBX_CONFIG_MAX_PATH 255

#ifndef RELEASE_TARBALL
#define OPBX_VERSION_INFO PACKAGE_STRING " SVN-" SVN_VERSION " built on " BUILD_HOSTNAME \
        ",  a " BUILD_MACHINE " running " BUILD_OS " on " BUILD_DATE
#else
#define OPBX_VERSION_INFO PACKAGE_STRING " built on " BUILD_HOSTNAME \
        ",  a " BUILD_MACHINE " running " BUILD_OS " on " BUILD_DATE
#endif


/* provided in openpbx.c */
extern int openpbx_main(int argc, char *argv[]);
extern char opbx_config_OPBX_CONFIG_DIR[OPBX_CONFIG_MAX_PATH];
extern char opbx_config_OPBX_CONFIG_FILE[OPBX_CONFIG_MAX_PATH];
extern char opbx_config_OPBX_MODULE_DIR[OPBX_CONFIG_MAX_PATH];
extern char opbx_config_OPBX_SPOOL_DIR[OPBX_CONFIG_MAX_PATH];
extern char opbx_config_OPBX_MONITOR_DIR[OPBX_CONFIG_MAX_PATH];
extern char opbx_config_OPBX_VAR_DIR[OPBX_CONFIG_MAX_PATH];
extern char opbx_config_OPBX_LOG_DIR[OPBX_CONFIG_MAX_PATH];
extern char opbx_config_OPBX_OGI_DIR[OPBX_CONFIG_MAX_PATH];
extern char opbx_config_OPBX_DB[OPBX_CONFIG_MAX_PATH];
extern char opbx_config_OPBX_DB_DIR[OPBX_CONFIG_MAX_PATH];
extern char opbx_config_OPBX_KEY_DIR[OPBX_CONFIG_MAX_PATH];
extern char opbx_config_OPBX_PID[OPBX_CONFIG_MAX_PATH];
extern char opbx_config_OPBX_SOCKET[OPBX_CONFIG_MAX_PATH];
extern char opbx_config_OPBX_RUN_DIR[OPBX_CONFIG_MAX_PATH];
extern char opbx_config_OPBX_CTL_PERMISSIONS[OPBX_CONFIG_MAX_PATH];
extern char opbx_config_OPBX_CTL_OWNER[OPBX_CONFIG_MAX_PATH];
extern char opbx_config_OPBX_CTL_GROUP[OPBX_CONFIG_MAX_PATH];
extern char opbx_config_OPBX_CTL[OPBX_CONFIG_MAX_PATH];
extern char opbx_config_OPBX_SOUNDS_DIR[OPBX_CONFIG_MAX_PATH];

/* Provided by openpbx.c */
extern int opbx_set_priority(int);
/* Provided by module.c */
extern int load_modules(const int preload_only);
/* Provided by pbx.c */
extern int load_pbx(void);
/* Provided by logger.c */
extern int init_logger(void);
extern void close_logger(void);
/* Provided by frame.c */
extern int init_framer(void);
/* Provided by logger.c */
extern int reload_logger(int);
/* Provided by term.c */
extern int term_init(void);
/* Provided by db.c */
extern int opbxdb_init(void);
/* Provided by channel.c */
extern void opbx_channels_init(void);
/* Provided by dnsmgr.c */
extern int dnsmgr_init(void);
extern void dnsmgr_reload(void);

/*!
 * \brief Register the version of a source code file with the core.
 * \param file the source file name
 * \param version the version string (typically a CVS revision keyword string)
 * \return nothing
 *
 * This function should not be called directly, but instead the
 * OPENPBX_FILE_VERSION macro should be used to register a file with the core.
 */
void opbx_register_file_version(const char *file, const char *version);

/*!
 * \brief Unregister a source code file from the core.
 * \param file the source file name
 * \return nothing
 *
 * This function should not be called directly, but instead the
 * OPENPBX_FILE_VERSION macro should be used to automatically unregister
 * the file when the module is unloaded.
 */
void opbx_unregister_file_version(const char *file);

/*!
 * \brief Register/unregister a source code file with the core.
 * \param file the source file name
 * \param version the version string (typically a CVS revision keyword string)
 *
 * This macro will place a file-scope constructor and destructor into the
 * source of the module using it; this will cause the version of this file
 * to registered with the OpenPBX core (and unregistered) at the appropriate
 * times.
 *
 * Example:
 *
 * \code
 * OPENPBX_FILE_VERSION("\$HeadURL\$", "\$Revision\$")
 * \endcode
 *
 * \note The dollar signs above have been protected with backslashes to keep
 * CVS from modifying them in this file; under normal circumstances they would
 * not be present and CVS would expand the Revision keyword into the file's
 * revision number.
 */
#if defined(__GNUC__) && !defined(LOW_MEMORY)
#define OPENPBX_FILE_VERSION(file, version) \
	static void __attribute__((constructor)) __register_file_version(void) \
	{ \
		opbx_register_file_version(file, version); \
	} \
	static void __attribute__((destructor)) __unregister_file_version(void) \
	{ \
		opbx_unregister_file_version(file); \
	}
#elif !defined(LOW_MEMORY) /* ! __GNUC__  && ! LOW_MEMORY*/
#define OPENPBX_FILE_VERSION(file, x) static const char __file_version[] = x;
#else /* LOW_MEMORY */
#define OPENPBX_FILE_VERSION(file, x)
#endif /* __GNUC__ */

#endif /* _OPENPBX_H */
