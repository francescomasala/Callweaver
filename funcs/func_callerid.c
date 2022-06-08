/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
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

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/module.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/logger.h"
#include "openpbx/utils.h"
#include "openpbx/app.h"
#include "openpbx/options.h"
#include "openpbx/old_callerid.h"

static char *callerid_read(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	if (!strncasecmp("all", data, 3)) {
		snprintf(buf, len, "\"%s\" <%s>", chan->cid.cid_name ? chan->cid.cid_name : "", chan->cid.cid_num ? chan->cid.cid_num : "");	
	} else if (!strncasecmp("name", data, 4)) {
		if (chan->cid.cid_name) {
			opbx_copy_string(buf, chan->cid.cid_name, len);
		}
	} else if (!strncasecmp("num", data, 3) || !strncasecmp("number", data, 6)) {
		if (chan->cid.cid_num) {
			opbx_copy_string(buf, chan->cid.cid_num, len);
		}
	} else if (!strncasecmp("ani", data, 3)) {
		if (chan->cid.cid_ani) {
			opbx_copy_string(buf, chan->cid.cid_ani, len);
		}
	} else if (!strncasecmp("dnid", data, 4)) {
		if (chan->cid.cid_dnid) {
			opbx_copy_string(buf, chan->cid.cid_dnid, len);
		}
	} else if (!strncasecmp("rdnis", data, 5)) {
		if (chan->cid.cid_rdnis) {
			opbx_copy_string(buf, chan->cid.cid_rdnis, len);
		}
	} else {
		opbx_log(LOG_ERROR, "Unknown callerid data type.\n");
	}

	return buf;
}

static void callerid_write(struct opbx_channel *chan, char *cmd, char *data, const char *value) 
{
	if (!value)
                return;
	
	if (!strncasecmp("all", data, 3)) {
		char name[256];
		char num[256];
		if (!opbx_callerid_split(value, name, sizeof(name), num, sizeof(num)))
			opbx_set_callerid(chan, num, name, num);	
        } else if (!strncasecmp("name", data, 4)) {
                opbx_set_callerid(chan, NULL, value, NULL);
        } else if (!strncasecmp("num", data, 3) || !strncasecmp("number", data, 6)) {
                opbx_set_callerid(chan, value, NULL, NULL);
        } else if (!strncasecmp("ani", data, 3)) {
                opbx_set_callerid(chan, NULL, NULL, value);
        } else if (!strncasecmp("dnid", data, 4)) {
                /* do we need to lock chan here? */
                if (chan->cid.cid_dnid)
                        free(chan->cid.cid_dnid);
                chan->cid.cid_dnid = opbx_strlen_zero(value) ? NULL : strdup(value);
        } else if (!strncasecmp("rdnis", data, 5)) {
                /* do we need to lock chan here? */
                if (chan->cid.cid_rdnis)
                        free(chan->cid.cid_rdnis);
                chan->cid.cid_rdnis = opbx_strlen_zero(value) ? NULL : strdup(value);
        } else {
                opbx_log(LOG_ERROR, "Unknown callerid data type.\n");
        }
}

static struct opbx_custom_function callerid_function = {
	.name = "CALLERID",
	.synopsis = "Gets or sets Caller*ID data on the channel.",
	.syntax = "CALLERID(datatype)",
	.desc = "Gets or sets Caller*ID data on the channel.  The allowable datatypes\n"
	"are \"all\", \"name\", \"num\", \"ANI\", \"DNID\", \"RDNIS\".\n",
	.read = callerid_read,
	.write = callerid_write,
};

static char *tdesc = "Caller ID related dialplan function";

int unload_module(void)
{
        return opbx_custom_function_unregister(&callerid_function);
}

int load_module(void)
{
        return opbx_custom_function_register(&callerid_function);
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
