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
 * \brief Generic File Format Support.
 */

#ifndef _OPENPBX_FILE_H
#define _OPENPBX_FILE_H

#include "openpbx/channel.h"
#include "openpbx/frame.h"
#include <fcntl.h>


#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif


/*! Convenient for waiting */
#define OPBX_DIGIT_ANY "0123456789#*ABCD"
#define OPBX_DIGIT_ANYNUM "0123456789"

#define SEEK_FORCECUR	10
	
/* Defined by individual formats.  First item MUST be a
   pointer for use by the stream manager */
struct opbx_filestream;

/*! Registers a new file format */
/*! Register a new file format capability
 * Adds a format to openpbx's format abilities.  Fill in the fields, and it will work. For examples, look at some of the various format code.
 * returns 0 on success, -1 on failure
 */
int opbx_format_register(const char *name, const char *exts, int format,
						struct opbx_filestream * (*open)(FILE *f),
						struct opbx_filestream * (*rewrite)(FILE *f, const char *comment),
						int (*write)(struct opbx_filestream *, struct opbx_frame *),
						int (*seek)(struct opbx_filestream *, long offset, int whence),
						int (*trunc)(struct opbx_filestream *),
						long (*tell)(struct opbx_filestream *),
						struct opbx_frame * (*read)(struct opbx_filestream *, int *timetonext),
						void (*close)(struct opbx_filestream *),
						char * (*getcomment)(struct opbx_filestream *));
	
/*! Unregisters a file format */
/*!
 * \param name the name of the format you wish to unregister
 * Unregisters a format based on the name of the format.
 * Returns 0 on success, -1 on failure to unregister
 */
int opbx_format_unregister(const char *name);

/*! Streams a file */
/*!
 * \param c channel to stream the file to
 * \param filename the name of the file you wish to stream, minus the extension
 * \param preflang the preferred language you wish to have the file streamed to you in
 * Prepares a channel for the streaming of a file.  To start the stream, afterward do a opbx_waitstream() on the channel
 * Also, it will stop any existing streams on the channel.
 * Returns 0 on success, or -1 on failure.
 */
int opbx_streamfile(struct opbx_channel *c, const char *filename, const char *preflang);

/*! Stops a stream */
/*!
 * \param c The channel you wish to stop playback on
 * Stop playback of a stream 
 * Returns 0 regardless
 */
int opbx_stopstream(struct opbx_channel *c);

/*! Checks for the existence of a given file */
/*!
 * \param filename name of the file you wish to check, minus the extension
 * \param fmt the format you wish to check (the extension)
 * \param preflang (the preferred language you wisht to find the file in)
 * See if a given file exists in a given format.  If fmt is NULL,  any format is accepted.
 * Returns -1 if file does not exist, non-zero positive otherwise.
 */
int opbx_fileexists(const char *filename, const char *fmt, const char *preflang);

/*! Renames a file */
/*! 
 * \param oldname the name of the file you wish to act upon (minus the extension)
 * \param newname the name you wish to rename the file to (minus the extension)
 * \param fmt the format of the file
 * Rename a given file in a given format, or if fmt is NULL, then do so for all 
 * Returns -1 on failure
 */
int opbx_filerename(const char *oldname, const char *newname, const char *fmt);

/*! Deletes a file */
/*! 
 * \param filename name of the file you wish to delete (minus the extension)
 * \param format of the file
 * Delete a given file in a given format, or if fmt is NULL, then do so for all 
 */
int opbx_filedelete(const char *filename, const char *fmt);

/*! Copies a file */
/*!
 * \param oldname name of the file you wish to copy (minus extension)
 * \param newname name you wish the file to be copied to (minus extension)
 * \param fmt the format of the file
 * Copy a given file in a given format, or if fmt is NULL, then do so for all 
 */
int opbx_filecopy(const char *oldname, const char *newname, const char *fmt);

/*! Waits for a stream to stop or digit to be pressed */
/*!
 * \param c channel to waitstram on
 * \param breakon string of DTMF digits to break upon
 * Begins playback of a stream...
 * Wait for a stream to stop or for any one of a given digit to arrive,  Returns 0 
 * if the stream finishes, the character if it was interrupted, and -1 on error 
 */
int opbx_waitstream(struct opbx_channel *c, const char *breakon);

/*! Waits for a stream to stop or digit matching a valid one digit exten to be pressed */
/*!
 * \param c channel to waitstram on
 * \param context string of context to match digits to break upon
 * Begins playback of a stream...
 * Wait for a stream to stop or for any one of a valid extension digit to arrive,  Returns 0 
 * if the stream finishes, the character if it was interrupted, and -1 on error 
 */
int opbx_waitstream_exten(struct opbx_channel *c, const char *context);

/*! Same as waitstream but allows stream to be forwarded or rewound */
/*!
 * \param c channel to waitstram on
 * \param breakon string of DTMF digits to break upon
 * \param forward DTMF digit to fast forward upon
 * \param rewind DTMF digit to rewind upon
 * \param ms How many miliseconds to skip forward/back
 * Begins playback of a stream...
 * Wait for a stream to stop or for any one of a given digit to arrive,  Returns 0 
 * if the stream finishes, the character if it was interrupted, and -1 on error 
 */
int opbx_waitstream_fr(struct opbx_channel *c, const char *breakon, const char *forward, const char *rewind, int ms);

/* Same as waitstream, but with audio output to fd and monitored fd checking.  Returns
   1 if monfd is ready for reading */
int opbx_waitstream_full(struct opbx_channel *c, const char *breakon, int audiofd, int monfd);

/*! Starts reading from a file */
/*!
 * \param filename the name of the file to read from
 * \param type format of file you wish to read from
 * \param comment comment to go with
 * \param flags file flags
 * \param check (unimplemented, hence negligible)
 * \param mode Open mode
 * Open an incoming file stream.  flags are flags for the open() command, and 
 * if check is non-zero, then it will not read a file if there are any files that 
 * start with that name and have an extension
 * Please note, this is a blocking function.  Program execution will not return until opbx_waitstream completes it's execution.
 * Returns a struct opbx_filestream on success, NULL on failure
 */
struct opbx_filestream *opbx_readfile(const char *filename, const char *type, const char *comment, int flags, int check, mode_t mode);

/*! Starts writing a file */
/*!
 * \param filename the name of the file to write to
 * \param type format of file you wish to write out to
 * \param comment comment to go with
 * \param oflags output file flags
 * \param check (unimplemented, hence negligible)
 * \param mode Open mode
 * Create an outgoing file stream.  oflags are flags for the open() command, and 
 * if check is non-zero, then it will not write a file if there are any files that 
 * start with that name and have an extension
 * Please note, this is a blocking function.  Program execution will not return until opbx_waitstream completes it's execution.
 * Returns a struct opbx_filestream on success, NULL on failure
 */
struct opbx_filestream *opbx_writefile(const char *filename, const char *type, const char *comment, int flags, int check, mode_t mode);

/*! Writes a frame to a stream */
/*! 
 * \param fs filestream to write to
 * \param f frame to write to the filestream
 * Send a frame to a filestream -- note: does NOT free the frame, call opbx_frfree manually
 * Returns 0 on success, -1 on failure.
 */
int opbx_writestream(struct opbx_filestream *fs, struct opbx_frame *f);

/*! Closes a stream */
/*!
 * \param f filestream to close
 * Close a playback or recording stream
 * Returns 0 on success, -1 on failure
 */
int opbx_closestream(struct opbx_filestream *f);

/*! Opens stream for use in seeking, playing */
/*!
 * \param chan channel to work with
 * \param filename to use
 * \param preflang prefered language to use
 * Returns a opbx_filestream pointer if it opens the file, NULL on error
 */
struct opbx_filestream *opbx_openstream(struct opbx_channel *chan, const char *filename, const char *preflang);

/*! Opens stream for use in seeking, playing */
/*!
 * \param chan channel to work with
 * \param filename to use
 * \param preflang prefered language to use
 * \param asis if set, don't clear generators
 * Returns a opbx_filestream pointer if it opens the file, NULL on error
 */
struct opbx_filestream *opbx_openstream_full(struct opbx_channel *chan, const char *filename, const char *preflang, int asis);
/*! Opens stream for use in seeking, playing */
/*!
 * \param chan channel to work with
 * \param filename to use
 * \param preflang prefered language to use
 * Returns a opbx_filestream pointer if it opens the file, NULL on error
 */
struct opbx_filestream *opbx_openvstream(struct opbx_channel *chan, const char *filename, const char *preflang);

/*! Applys a open stream to a channel. */
/*!
 * \param chan channel to work
 * \param opbx_filestream s to apply
 * Returns 0 for success, -1 on failure
 */
int opbx_applystream(struct opbx_channel *chan, struct opbx_filestream *s);

/*! play a open stream on a channel. */
/*!
 * \param opbx_filestream s to play
 * Returns 0 for success, -1 on failure
 */
int opbx_playstream(struct opbx_filestream *s);

/*! Seeks into stream */
/*!
 * \param opbx_filestream to perform seek on
 * \param sample_offset numbers of samples to seek
 * \param whence SEEK_SET, SEEK_CUR, SEEK_END 
 * Returns 0 for success, or -1 for error
 */
int opbx_seekstream(struct opbx_filestream *fs, long sample_offset, int whence);

/*! Trunc stream at current location */
/*!
 * \param opbx_filestream fs 
 * Returns 0 for success, or -1 for error
 */
int opbx_truncstream(struct opbx_filestream *fs);

/*! Fast forward stream ms */
/*!
 * \param opbx_filestream fs filestream to act on
 * \param ms milliseconds to move
 * Returns 0 for success, or -1 for error
 */
int opbx_stream_fastforward(struct opbx_filestream *fs, long ms);

/*! Rewind stream ms */
/*!
 * \param opbx_filestream fs filestream to act on
 * \param ms milliseconds to move
 * Returns 0 for success, or -1 for error
 */
int opbx_stream_rewind(struct opbx_filestream *fs, long ms);

/*! Fast forward stream ms */
/*!
 * \param opbx_filestream fs filestream to act on
 * \param ms milliseconds to move
 * Returns 0 for success, or -1 for error
 */
int opbx_stream_fastforward(struct opbx_filestream *fs, long ms);

/*! Rewind stream ms */
/*!
 * \param opbx_filestream fs filestream to act on
 * \param ms milliseconds to move
 * Returns 0 for success, or -1 for error
 */
int opbx_stream_rewind(struct opbx_filestream *fs, long ms);

/*! Tell where we are in a stream */
/*!
 * \param opbx_filestream fs to act on
 * Returns a long as a sample offset into stream
 */
long opbx_tellstream(struct opbx_filestream *fs);

/*! Read a frame from a filestream */
/*!
 * \param opbx_filestream fs to act on
 * Returns a frame or NULL if read failed
 */ 
struct opbx_frame *opbx_readframe(struct opbx_filestream *s);

/*! Initialize file stuff */
/*!
 * Initializes all the various file stuff.  Basically just registers the cli stuff
 * Returns 0 all the time
 */
extern int opbx_file_init(void);


#define OPBX_RESERVED_POINTERS 20

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _OPENPBX_FILE_H */
