#
#	Main Automake file for OpenPBX
#
#	Author: Michael "cypromis" Bielicki <michal.bielickie@halo2.pl>
#	Copyright <c> 2005 Halo Kwadrat Sp. z o.o.
#
#	This program is free software; you can redistribute it and/or
#	modify it under the terms of the GNU General Public License
#	as published by the Free Software Foundation; either version
#	2 of the License, or (at your option) any later version.


AUTOMAKE_OPTS = gnu foreign
ACLOCAL_AMFLAGS = -I acmacros
SH = @SH@
SHELL = @SHELL@
AWK = @AWK@
AR = @AR@
GREP = @GREP@
SED = @SED@
M4 = @M4@
SVN = @SVN@

sbin_PROGRAMS = openpbx 

openpbx_SOURCES 	= main.c
openpbx_LDADD		= ${top_builddir}/corelib/libopenpbx.la $(LIBLTDL)
openpbx_LDADD		+= -lreadline

openpbx_CFLAGS 	=  -fomit-frame-pointer -Wall -Wstrict-prototypes 
openpbx_CFLAGS	+= -Wmissing-prototypes -Wmissing-declarations -I$(top_srcdir)/include $(AM_CFLAGS)

man_MANS = openpbx.8

EXTRA_DIST = openpbx.sgml $(man_MANS) substitute.mak

SUBDIRS =   libltdl libs
SUBDIRS +=  stdtime @SQLITE3_SUBDIR@ include corelib . pbx funcs
SUBDIRS +=  cdr apps codecs formats channels ogi utils res configs

INSTALL_DIRS = $(opbxdbdir) $(opbxdocdir) $(opbximagesdir)		\
$(opbxkeydir) $(opbxlogdir) $(opbxlogdir)/cdr-custom			\
$(opbxlogdir)/cdr-csv $(opbxmohdir) $(opbxrundir) $(opbxspooldir)	\
$(opbxspooldir)/voicemail $(opbxspooldir)/meetme			\
$(opbxspooldir)/outgoing $(opbxsqlitedir) $(opbxvardir) $(opbxicdmoddir)

install-data-hook:
	for install_dir in $(INSTALL_DIRS); \
	do \
		mkdir -p $(DESTDIR)$${install_dir}; \
		if test -z "$(DESTDIR)"; \
		then \
			chown ${opbxrunuser}:${opbxrungroup} $(DESTDIR)$${install_dir}; \
		fi; \
		chmod 0750 $(DESTDIR)$${install_dir}; \
	done
