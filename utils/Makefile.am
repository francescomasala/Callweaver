AUTOMAKE_OPTS = gnu

bin_PROGRAMS = streamplayer # check_expr 
streamplayer_SOURCES = streamplayer.c ${top_srcdir}/corelib/strcompat.c
# check_expr_SOURCES = check_expr.c ../opbx_expr2.c ../opbx_expr2f.c
# check_expr_CFLAGS  = -DNO_OPX_MM -D_GNU_SOURCE -DSTANDALONE $(AM_CFLAGS)

if USE_NEWT
    bin_PROGRAMS += opbxman
    opbxman_CFLAGS = $(AM_CFLAGS) @SSL_CFLAGS@
    opbxman_SOURCES = opbxman.c ${top_srcdir}/corelib/utils.c
    opbxman_LDADD = -lnewt @SSL_LIBS@
endif USE_NEWT

if WANT_SMSQ
    bin_PROGRAMS += smsq
    smsq_SOURCES = smsq.c
    smsq_LDADD = -lpopt
endif WANT_SMSQ
