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
 * \brief JPEG File format support.
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif
 
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <sys/time.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/channel.h"
#include "openpbx/file.h"
#include "openpbx/logger.h"
#include "openpbx/sched.h"
#include "openpbx/module.h"
#include "openpbx/image.h"
#include "openpbx/lock.h"
#include "confdefs.h"

static char *desc = "JPEG (Joint Picture Experts Group) Image Format";


static struct opbx_frame *jpeg_read_image(int fd, int len)
{
	struct opbx_frame fr;
	int res;
	char buf[65536];
	if (len > sizeof(buf) || len < 0) {
		opbx_log(LOG_WARNING, "JPEG image too large to read\n");
		return NULL;
	}
	res = read(fd, buf, len);
	if (res < len) {
		opbx_log(LOG_WARNING, "Only read %d of %d bytes: %s\n", res, len, strerror(errno));
	}
	memset(&fr, 0, sizeof(fr));
	fr.frametype = OPBX_FRAME_IMAGE;
	fr.subclass = OPBX_FORMAT_JPEG;
	fr.data = buf;
	fr.src = "JPEG Read";
	fr.datalen = len;
	return opbx_frisolate(&fr);
}

static int jpeg_identify(int fd)
{
	char buf[10];
	int res;
	res = read(fd, buf, sizeof(buf));
	if (res < sizeof(buf))
		return 0;
	if (memcmp(buf + 6, "JFIF", 4))
		return 0;
	return 1;
}

static int jpeg_write_image(int fd, struct opbx_frame *fr)
{
	int res=0;
	if (fr->frametype != OPBX_FRAME_IMAGE) {
		opbx_log(LOG_WARNING, "Not an image\n");
		return -1;
	}
	if (fr->subclass != OPBX_FORMAT_JPEG) {
		opbx_log(LOG_WARNING, "Not a jpeg image\n");
		return -1;
	}
	if (fr->datalen) {
		res = write(fd, fr->data, fr->datalen);
		if (res != fr->datalen) {
			opbx_log(LOG_WARNING, "Only wrote %d of %d bytes: %s\n", res, fr->datalen, strerror(errno));
			return -1;
		}
	}
	return res;
}

static struct opbx_imager jpeg_format = {
	"jpg",
	"JPEG (Joint Picture Experts Group)",
	"jpg|jpeg",
	OPBX_FORMAT_JPEG,
	jpeg_read_image,
	jpeg_identify,
	jpeg_write_image,
};

int load_module()
{
	return opbx_image_register(&jpeg_format);
}

int unload_module()
{
	opbx_image_unregister(&jpeg_format);
	return 0;
}	

int usecount()
{
	/* We never really have any users */
	return 0;
}

char *description()
{
	return desc;
}



