/*
 * OpenPBX -- An open source telephony toolkit.
 *
 * Based on Asterisk written by Mark Spencer <markster@digium.com>
 *  Copyright (C) 1999 - 2005, Digium, Inc.
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
 * Very Top level source file for openpbx
 * 
 */
#include "openpbx.h"

int main(int argc, char *argv[])
{
	return openpbx_main(argc, argv);
}
