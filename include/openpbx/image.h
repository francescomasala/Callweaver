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
 * \brief General OpenPBX channel definitions for image handling
 */

#ifndef _OPENPBX_IMAGE_H
#define _OPENPBX_IMAGE_H

/*! \brief structure associated with registering an image format */
struct opbx_imager {
	/*! Name */
	char *name;						
	/*! Description */
	char *desc;						
	/*! Extension(s) (separated by '|' ) */
	char *exts;						
	/*! Image format */
	int format;						
	/*! Read an image from a file descriptor */
	struct opbx_frame *(*read_image)(int fd, int len);	
	/*! Identify if this is that type of file */
	int (*identify)(int fd);				
	/*! Returns length written */
	int (*write_image)(int fd, struct opbx_frame *frame); 	
	/*! For linked list */
	struct opbx_imager *next;
};

/*! Check for image support on a channel */
/*! 
 * \param chan channel to check
 * Checks the channel to see if it supports the transmission of images
 * Returns non-zero if image transmission is supported
 */
extern int opbx_supports_images(struct opbx_channel *chan);

/*! Sends an image */
/*!
 * \param chan channel to send image on
 * \param filename filename of image to send (minus extension)
 * Sends an image on the given channel.
 * Returns 0 on success, -1 on error
 */
extern int opbx_send_image(struct opbx_channel *chan, char *filename);

/*! Make an image */
/*! 
 * \param filename filename of image to prepare
 * \param preflang preferred language to get the image...?
 * \param format the format of the file
 * Make an image from a filename ??? No estoy positivo
 * Returns an opbx_frame on success, NULL on failure
 */
extern struct opbx_frame *opbx_read_image(char *filename, char *preflang, int format);

/*! Register image format */
/*! 
 * \param imgdrv Populated opbx_imager structure with info to register
 * Registers an image format
 * Returns 0 regardless
 */
extern int opbx_image_register(struct opbx_imager *imgdrv);

/*! Unregister an image format */
/*!
 * \param imgdrv pointer to the opbx_imager structure you wish to unregister
 * Unregisters the image format passed in
 * Returns nothing
 */
extern void opbx_image_unregister(struct opbx_imager *imgdrv);

/*! Initialize image stuff */
/*!
 * Initializes all the various image stuff.  Basically just registers the cli stuff
 * Returns 0 all the time
 */
extern int opbx_image_init(void);

#endif /* _OPENPBX_IMAGE_H */