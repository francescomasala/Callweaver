/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Created by Olle E. Johansson, Edvina.net 
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
 * \brief URI encoding / decoding
 * 
 * \note For now this code only supports 8 bit characters, not unicode,
         which we ultimately will need to support.
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/channel.h"
#include "openpbx/pbx.h"
#include "openpbx/logger.h"
#include "openpbx/utils.h"
#include "openpbx/app.h"
#include "openpbx/module.h"

/*! \brief builtin_function_uriencode: Encode URL according to RFC 2396 */
static char *builtin_function_uriencode(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	char uri[BUFSIZ];

	if (!data || opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "Syntax: URIENCODE(<data>) - missing argument!\n");
		return NULL;
	}

	opbx_uri_encode(data, uri, sizeof(uri), 1);
	opbx_copy_string(buf, uri, len);

	return buf;
}

/*!\brief builtin_function_uridecode: Decode URI according to RFC 2396 */
static char *builtin_function_uridecode(struct opbx_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	if (!data || opbx_strlen_zero(data)) {
		opbx_log(LOG_WARNING, "Syntax: URIDECODE(<data>) - missing argument!\n");
		return NULL;
	}
	
	opbx_copy_string(buf, data, len);
	opbx_uri_decode(buf);
	return buf;
}

static struct opbx_custom_function urldecode_function = {
	.name = "URIDECODE",
	.synopsis = "Decodes an URI-encoded string.",
	.syntax = "URIDECODE(<data>)",
	.read = builtin_function_uridecode,
};

static struct opbx_custom_function urlencode_function = {
	.name = "URIENCODE",
	.synopsis = "Encodes a string to URI-safe encoding.",
	.syntax = "URIENCODE(<data>)",
	.read = builtin_function_uriencode,
};

static char *tdesc = "URI encode/decode functions";

int unload_module(void)
{
        return opbx_custom_function_unregister(&urldecode_function) || opbx_custom_function_unregister(&urlencode_function);
}

int load_module(void)
{
        return opbx_custom_function_register(&urldecode_function) || opbx_custom_function_register(&urlencode_function);
}

char *description(void)
{
	return tdesc;
}

int usecount(void)
{
	return 0;
}
