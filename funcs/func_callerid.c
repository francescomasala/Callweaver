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
 * \brief Caller ID related dialplan functions
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
#include <spandsp.h>

#include "callweaver.h"

CALLWEAVER_FILE_VERSION("$HeadURL: https://svn.callweaver.org/callweaver/branches/rel/1.2/funcs/func_callerid.c $", "$Revision: 4723 $")

#include "callweaver/module.h"
#include "callweaver/channel.h"
#include "callweaver/pbx.h"
#include "callweaver/logger.h"
#include "callweaver/utils.h"
#include "callweaver/app.h"
#include "callweaver/options.h"
#include "callweaver/callerid.h"
#include "callweaver/phone_no_utils.h"

static void *callerid_function;
static const char *callerid_func_name = "CALLERID";
static const char *callerid_func_synopsis = "Gets or sets Caller*ID data on the channel.";
static const char *callerid_func_syntax = "CALLERID(datatype)";
static const char *callerid_func_desc =
	"Gets or sets Caller*ID data on the channel.  The allowable datatypes\n"
	"are \"all\", \"name\", \"num\", \"ANI\", \"DNID\", \"RDNIS\".\n";


static char *callerid_read(struct cw_channel *chan, int argc, char **argv, char *buf, size_t len) 
{
	if (!strncasecmp("all", argv[0], 3)) {
		snprintf(buf, len, "\"%s\" <%s>", chan->cid.cid_name ? chan->cid.cid_name : "", chan->cid.cid_num ? chan->cid.cid_num : "");	
	} else if (!strncasecmp("name", argv[0], 4)) {
		if (chan->cid.cid_name) {
			cw_copy_string(buf, chan->cid.cid_name, len);
		}
	} else if (!strncasecmp("num", argv[0], 3) || !strncasecmp("number", argv[0], 6)) {
		if (chan->cid.cid_num) {
			cw_copy_string(buf, chan->cid.cid_num, len);
		}
	} else if (!strncasecmp("ani", argv[0], 3)) {
		if (chan->cid.cid_ani) {
			cw_copy_string(buf, chan->cid.cid_ani, len);
		}
	} else if (!strncasecmp("dnid", argv[0], 4)) {
		if (chan->cid.cid_dnid) {
			cw_copy_string(buf, chan->cid.cid_dnid, len);
		}
	} else if (!strncasecmp("rdnis", argv[0], 5)) {
		if (chan->cid.cid_rdnis) {
			cw_copy_string(buf, chan->cid.cid_rdnis, len);
		}
	} else {
		cw_log(LOG_ERROR, "Unknown callerid data type.\n");
	}

	return buf;
}

static void callerid_write(struct cw_channel *chan, int argc, char **argv, const char *value) 
{
	if (!value)
                return;
	
	if (!strncasecmp("all", argv[0], 3)) {
		char name[256];
		char num[256];
		if (!cw_callerid_split(value, name, sizeof(name), num, sizeof(num)))
			cw_set_callerid(chan, num, name, num);	
        } else if (!strncasecmp("name", argv[0], 4)) {
                cw_set_callerid(chan, NULL, value, NULL);
        } else if (!strncasecmp("num", argv[0], 3) || !strncasecmp("number", argv[0], 6)) {
                cw_set_callerid(chan, value, NULL, NULL);
        } else if (!strncasecmp("ani", argv[0], 3)) {
                cw_set_callerid(chan, NULL, NULL, value);
        } else if (!strncasecmp("dnid", argv[0], 4)) {
                /* do we need to lock chan here? */
                if (chan->cid.cid_dnid)
                        free(chan->cid.cid_dnid);
                chan->cid.cid_dnid = cw_strlen_zero(value) ? NULL : strdup(value);
        } else if (!strncasecmp("rdnis", argv[0], 5)) {
                /* do we need to lock chan here? */
                if (chan->cid.cid_rdnis)
                        free(chan->cid.cid_rdnis);
                chan->cid.cid_rdnis = cw_strlen_zero(value) ? NULL : strdup(value);
        } else {
                cw_log(LOG_ERROR, "Unknown callerid data type.\n");
        }
}


static char *tdesc = "Caller ID related dialplan function";

int unload_module(void)
{
        return cw_unregister_function(callerid_function);
}

int load_module(void)
{
        callerid_function = cw_register_function(callerid_func_name, callerid_read, callerid_write, callerid_func_synopsis, callerid_func_syntax, callerid_func_desc);
	return 0;
}

char *description(void)
{
	return tdesc;
}

int usecount(void)
{
	return 0;
}

/*
Local Variables:
mode: C
c-file-style: "linux"
indent-tabs-mode: nil
End:
*/
