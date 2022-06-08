AUTOMAKE_OPTS			= gnu

opbxmod_LTLIBRARIES 		=

if WANT_CHAN_IAX2
opbxmod_LTLIBRARIES		+= chan_iax2.la
chan_iax2_la_SOURCES            = chan_iax2.c iax2-parser.h iax2-parser.c iax2-provision.h iax2-provision.c iax2.h
chan_iax2_la_CFLAGS		= $(AM_CFLAGS) -I${top_srcdir}/corelib
chan_iax2_la_LDFLAGS 		= -module -avoid-version -no-undefined
chan_iax2_la_LIBADD             = ${top_builddir}/corelib/libopenpbx.la
endif WANT_CHAN_IAX2
