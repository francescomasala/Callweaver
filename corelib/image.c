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
 * \brief Image Management
 * 
 */
#ifdef HAVE_CONFIG_H
#include "confdefs.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>

#include "openpbx.h"

OPENPBX_FILE_VERSION("$HeadURL$", "$Revision$")

#include "openpbx/sched.h"
#include "openpbx/options.h"
#include "openpbx/channel.h"
#include "openpbx/logger.h"
#include "openpbx/file.h"
#include "openpbx/image.h"
#include "openpbx/translate.h"
#include "openpbx/cli.h"
#include "openpbx/lock.h"

static struct opbx_imager *list;
OPBX_MUTEX_DEFINE_STATIC(listlock);

int opbx_image_register(struct opbx_imager *img)
{
	if (option_verbose > 1)
		opbx_verbose(VERBOSE_PREFIX_2 "Registered format '%s' (%s)\n", img->name, img->desc);
	opbx_mutex_lock(&listlock);
	img->next = list;
	list = img;
	opbx_mutex_unlock(&listlock);
	return 0;
}

void opbx_image_unregister(struct opbx_imager *img)
{
	struct opbx_imager *i, *prev = NULL;
	opbx_mutex_lock(&listlock);
	i = list;
	while(i) {
		if (i == img) {
			if (prev) 
				prev->next = i->next;
			else
				list = i->next;
			break;
		}
		prev = i;
		i = i->next;
	}
	opbx_mutex_unlock(&listlock);
	if (i && (option_verbose > 1))
		opbx_verbose(VERBOSE_PREFIX_2 "Unregistered format '%s' (%s)\n", img->name, img->desc);
}

int opbx_supports_images(struct opbx_channel *chan)
{
	if (!chan || !chan->tech)
		return 0;
	if (!chan->tech->send_image)
		return 0;
	return 1;
}

static int file_exists(char *filename)
{
	int res;
	struct stat st;
	res = stat(filename, &st);
	if (!res)
		return st.st_size;
	return 0;
}

static void make_filename(char *buf, int len, char *filename, char *preflang, char *ext)
{
	if (filename[0] == '/') {
		if (preflang && strlen(preflang))
			snprintf(buf, len, "%s-%s.%s", filename, preflang, ext);
		else
			snprintf(buf, len, "%s.%s", filename, ext);
	} else {
		if (preflang && strlen(preflang))
			snprintf(buf, len, "%s/%s/%s-%s.%s", opbx_config_OPBX_VAR_DIR, "images", filename, preflang, ext);
		else
			snprintf(buf, len, "%s/%s/%s.%s", opbx_config_OPBX_VAR_DIR, "images", filename, ext);
	}
}

struct opbx_frame *opbx_read_image(char *filename, char *preflang, int format)
{
	struct opbx_imager *i;
	char buf[256];
	char tmp[80];
	char *e;
	struct opbx_imager *found = NULL;
	int fd;
	int len=0;
	struct opbx_frame *f = NULL;
#if 0 /* We need to have some sort of read-only lock */
	opbx_mutex_lock(&listlock);
#endif	
	i = list;
	while(!found && i) {
		if (i->format & format) {
			char *stringp=NULL;
			strncpy(tmp, i->exts, sizeof(tmp)-1);
			stringp=tmp;
			e = strsep(&stringp, "|");
			while(e) {
				make_filename(buf, sizeof(buf), filename, preflang, e);
				if ((len = file_exists(buf))) {
					found = i;
					break;
				}
				make_filename(buf, sizeof(buf), filename, NULL, e);
				if ((len = file_exists(buf))) {
					found = i;
					break;
				}
				e = strsep(&stringp, "|");
			}
		}
		i = i->next;
	}
	if (found) {
		fd = open(buf, O_RDONLY);
		if (fd > -1) {
			if (!found->identify || found->identify(fd)) {
				/* Reset file pointer */
				lseek(fd, 0, SEEK_SET);
				f = found->read_image(fd,len); 
			} else
				opbx_log(LOG_WARNING, "%s does not appear to be a %s file\n", buf, i->name);
			close(fd);
		} else
			opbx_log(LOG_WARNING, "Unable to open '%s': %s\n", buf, strerror(errno));
	} else
		opbx_log(LOG_WARNING, "Image file '%s' not found\n", filename);
#if 0
	opbx_mutex_unlock(&listlock);
#endif	
	return f;
}


int opbx_send_image(struct opbx_channel *chan, char *filename)
{
	struct opbx_frame *f;
	int res = -1;
	if (chan->tech->send_image) {
		f = opbx_read_image(filename, chan->language, -1);
		if (f) {
			res = chan->tech->send_image(chan, f);
			opbx_frfree(f);
		}
	}
	return res;
}

static int show_image_formats(int fd, int argc, char *argv[])
{
#define FORMAT "%10s %10s %50s %10s\n"
#define FORMAT2 "%10s %10s %50s %10s\n"
	struct opbx_imager *i;
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	opbx_cli(fd, FORMAT, "Name", "Extensions", "Description", "Format");
	i = list;
	while(i) {
		opbx_cli(fd, FORMAT2, i->name, i->exts, i->desc, opbx_getformatname(i->format));
		i = i->next;
	};
	return RESULT_SUCCESS;
}

struct opbx_cli_entry show_images =
{
	{ "show", "image", "formats" },
	show_image_formats,
	"Displays image formats",
"Usage: show image formats\n"
"       displays currently registered image formats (if any)\n"
};


int opbx_image_init(void)
{
	opbx_cli_register(&show_images);
	return 0;
}

