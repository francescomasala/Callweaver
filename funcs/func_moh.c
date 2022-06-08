/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Russell Bryant <russelb@clemson.edu> 
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
 * \brief Functions for reading or setting the MusicOnHold class
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL: svn+ssh://svn@svn.openpbx.org/openpbx/trunk/funcs/func_md5.c $", "$Revision$")

#include "openpbx/module.h"
#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/utils.h"

static char *function_moh_read(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len)
{
	opbx_copy_string(buf, chan->musicclass, len);

	return buf;
}

static void function_moh_write(struct opbx_channel *chan, char *cmd, char *data, const char *value) 
{
	opbx_copy_string(chan->musicclass, value, MAX_MUSICCLASS);
}

static struct opbx_custom_function moh_function = {
	.name = "MUSICCLASS",
	.synopsis = "Read or Set the MusicOnHold class",
	.syntax = "MUSICCLASS()",
	.desc = "This function will read or set the music on hold class for a channel.\n",
	.read = function_moh_read,
	.write = function_moh_write,
};

static char *tdesc = "MOH functions";

int unload_module(void)
{
        return opbx_custom_function_unregister(&moh_function);
}

int load_module(void)
{
        return opbx_custom_function_register(&moh_function);
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
