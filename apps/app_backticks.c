/*
 * CallWeaver -- An open source telephony toolkit.
 *
 * BackTicks Application For CallWeaver
 *
 * Copyright (C) 2005, Anthony Minessale II
 *
 * Anthony Minessale II <anthmct@yahoo.com>
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
 * \brief Execute a shell command and save the result as a variable
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "callweaver.h"

#include "callweaver/file.h"
#include "callweaver/logger.h"
#include "callweaver/channel.h"
#include "callweaver/pbx.h"
#include "callweaver/module.h"
#include "callweaver/lock.h"
#include "callweaver/app.h"
#include "callweaver/options.h"


static char *tdesc = "backticks";

static void *backticks_app;
static char *backticks_name = "BackTicks";
static char *backticks_synopsis = "Execute a shell command and save the result as a variable.";
static char *backticks_syntax = "BackTicks(varname, command)";
static char *backticks_descrip =
	"Be sure to include a full path!\n";

static void *backticks_function;
static const char *backticks_func_name = "BACKTICKS";
static const char *backticks_func_synopsis = "Executes a shell command.";
static const char *backticks_func_syntax = "BACKTICKS(command)";
static const char *backticks_func_descrip =
	"Executes a shell command and evaluates to the result.";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;


static char *do_backticks(char *command, char *buf, size_t len)
{
        int fds[2];
	pid_t pid = 0;
	int n;

        if (pipe(fds)) {
                cw_log(LOG_WARNING, "Pipe/Exec failed\n");
        } else {
                pid = fork();
                if (pid < 0) {
                        cw_log(LOG_WARNING, "Fork failed\n");
                        close(fds[0]);
                        close(fds[1]);
                } else if (pid) { /* parent */
                        close(fds[1]);
			/* Reserve the last for null */
			len--;
                        while (len && (n = read(fds[0], buf, len)) > 0) {
				buf += n;
				len -= n;
			}
                } else { /* child */
                        close(fds[0]);
                        dup2(fds[1], STDOUT_FILENO);

                        close(fds[0]);
                        dup2(fds[1], STDOUT_FILENO);

                        system(command);
                        cw_log(LOG_ERROR, "system(\"%s\") failed\n", command);
                        _exit(0);
                }
        }

	*buf = '\0';
        return buf;
}

static int backticks_exec(struct cw_channel *chan, int argc, char **argv)
{
	char buf[1024];
	struct localuser *u;
	int ret;

	if (argc != 2) {
		cw_log(LOG_ERROR, "Syntax: %s\n", backticks_syntax);
		return -1;
	}

	LOCAL_USER_ADD(u);

	ret = 0;
	if (do_backticks(argv[1], buf, sizeof(buf))) {
		pbx_builtin_setvar_helper(chan, argv[0], buf);
	} else {
		cw_log(LOG_WARNING, "No Data!\n");
		ret = -1;
	}

	LOCAL_USER_REMOVE(u);
	return ret;
}


static char *function_backticks(struct cw_channel *chan, int argc, char **argv, char *buf, size_t len)
{
        char *ret = NULL;

        if (argc > 0 && do_backticks(argv[0], buf, len))
                ret = buf;

        return ret;
}


int unload_module(void)
{
	int res = 0;
        STANDARD_HANGUP_LOCALUSERS;
        cw_unregister_function(backticks_function);
        res |= cw_unregister_application(backticks_app);
	return res;
}

int load_module(void)
{
        backticks_function = cw_register_function(backticks_func_name, function_backticks, NULL, backticks_func_synopsis, backticks_func_syntax, backticks_func_descrip);
        backticks_app = cw_register_application(backticks_name, backticks_exec, backticks_synopsis, backticks_syntax, backticks_descrip);
	return 0;
}

char *description(void)
{
        return tdesc;
}

int usecount(void)
{
        int res;
        STANDARD_USECOUNT(res);
        return res;
}
