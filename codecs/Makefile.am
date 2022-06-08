AUTOMAKE_OPTS		= gnu foreign

opbxmod_LTLIBRARIES	= codec_lpc10.la codec_dvi_adpcm.la codec_oki_adpcm.la \
			  codec_ulaw.la codec_alaw.la codec_a_mu.la \
			  codec_g726.la codec_gsm.la

codec_gsm_la_SOURCES	= codec_gsm.c slin_gsm_ex.h gsm_slin_ex.h
codec_gsm_la_CFLAGS     = $(AM_CFLAGS)
codec_gsm_la_LIBADD		= -lspandsp -ltiff ${top_builddir}/corelib/libopenpbx.la
codec_gsm_la_LDFLAGS	= -module -avoid-version -no-undefined
codec_lpc10_la_SOURCES	= codec_lpc10.c slin_lpc10_ex.h lpc10_slin_ex.h
codec_lpc10_la_CFLAGS   = $(AM_CFLAGS) -I${top_srcdir}/libs/lpc10
codec_lpc10_la_LIBADD	= -lspandsp -ltiff ${top_builddir}/corelib/libopenpbx.la -lm
codec_lpc10_la_LDFLAGS	= -module -avoid-version -no-undefined
codec_dvi_adpcm_la_SOURCES	= codec_dvi_adpcm.c slin_adpcm_ex.h adpcm_slin_ex.h
codec_dvi_adpcm_la_LDFLAGS	= -module -avoid-version -no-undefined
codec_dvi_adpcm_la_LIBADD	= -lspandsp -ltiff ${top_builddir}/corelib/libopenpbx.la 
codec_oki_adpcm_la_SOURCES	= codec_oki_adpcm.c slin_adpcm_ex.h adpcm_slin_ex.h
codec_oki_adpcm_la_LDFLAGS	= -module -avoid-version -no-undefined
codec_oki_adpcm_la_LIBADD	= -lspandsp -ltiff ${top_builddir}/corelib/libopenpbx.la 
codec_ulaw_la_SOURCES	= codec_ulaw.c slin_ulaw_ex.h ulaw_slin_ex.h
codec_ulaw_la_LDFLAGS	= -module -avoid-version -no-undefined
codec_ulaw_la_LIBADD	= -lspandsp -ltiff ${top_builddir}/corelib/libopenpbx.la 
codec_alaw_la_SOURCES	= codec_alaw.c slin_ulaw_ex.h ulaw_slin_ex.h
codec_alaw_la_LDFLAGS	= -module -avoid-version -no-undefined
codec_alaw_la_LIBADD	= -lspandsp -ltiff ${top_builddir}/corelib/libopenpbx.la 
codec_a_mu_la_SOURCES	= codec_a_mu.c ulaw_slin_ex.h
codec_a_mu_la_LDFLAGS	= -module -avoid-version -no-undefined
codec_a_mu_la_LIBADD	= -lspandsp -ltiff ${top_builddir}/corelib/libopenpbx.la 
codec_g726_la_SOURCES   = codec_g726.c g726_slin_ex.h slin_g726_ex.h log2comp.h
codec_g726_la_LDFLAGS	= -module -avoid-version -no-undefined
codec_g726_la_LIBADD	= -lspandsp -ltiff ${top_builddir}/corelib/libopenpbx.la

if WANT_CODEC_SPEEX
opbxmod_LTLIBRARIES	+= codec_speex.la
codec_speex_la_SOURCES	= codec_speex.c slin_speex_ex.h speex_slin_ex.h
codec_speex_la_LIBADD	= -lspeex ${top_builddir}/corelib/libopenpbx.la
codec_speex_la_CFLAGS	= $(AM_CFLAGS) @SPEEX_CFLAGS@
codec_speex_la_LDFLAGS  = -module -avoid-version -no-undefined
endif WANT_CODEC_SPEEX

INCLUDES = -I$(top_srcdir)/include
