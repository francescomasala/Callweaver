AUTOMAKE_OPTS			= gnu

opbxmod_LTLIBRARIES 		=

SUBDIRS				= iax2 sccp fax misdn

if WANT_CHAN_ALSA
opbxmod_LTLIBRARIES		+= chan_alsa.la
chan_alsa_la_SOURCES		= chan_alsa.c
chan_alsa_la_CFLAGS		= $(AM_CFLAGS) @ALSA_CFLAGS@
chan_alsa_la_LIBADD		= @ALSA_LIBS@ ${top_builddir}/corelib/libopenpbx.la
chan_alsa_la_LDFLAGS            = -module -avoid-version -no-undefined
endif WANT_CHAN_ALSA

if WANT_CHAN_AGENT
opbxmod_LTLIBRARIES     	+= chan_agent.la
chan_agent_la_SOURCES		= chan_agent.c
chan_agent_la_LDFLAGS		= -module -avoid-version -no-undefined
chan_agent_la_LIBADD		= ${top_builddir}/corelib/libopenpbx.la  
endif WANT_CHAN_AGENT

if WANT_CHAN_CAPI
opbxmod_LTLIBRARIES  		+= chan_capi.la
chan_capi_la_SOURCES 		= chan_capi.c c20msg.c chan_capi_rtp.c
chan_capi_la_LIBADD  		= -lcapi20 ${top_builddir}/corelib/libopenpbx.la
chan_capi_la_LDFLAGS 		= -module -avoid-version -no-undefined
endif WANT_CHAN_CAPI

if WANT_CHAN_FEATURES
opbxmod_LTLIBRARIES		+= chan_features.la
chan_features_la_SOURCES  	= chan_features.c
chan_features_la_LDFLAGS 	= -module -avoid-version -no-undefined
chan_features_la_LIBADD         = ${top_builddir}/corelib/libopenpbx.la   
endif WANT_CHAN_FEATURES

if WANT_CHAN_LOCAL
opbxmod_LTLIBRARIES		+= chan_local.la
chan_local_la_SOURCES 		= chan_local.c
chan_local_la_LDFLAGS 		= -module -avoid-version -no-undefined
chan_local_la_LIBADD            = ${top_builddir}/corelib/libopenpbx.la   
endif WANT_CHAN_LOCAL

if WANT_CHAN_MGCP
opbxmod_LTLIBRARIES		+= chan_mgcp.la
chan_mgcp_la_SOURCES 		= chan_mgcp.c
chan_mgcp_la_LDFLAGS 		= -module -avoid-version -no-undefined
chan_mgcp_la_LIBADD             = ${top_builddir}/corelib/libopenpbx.la   
endif WANT_CHAN_MGCP

if WANT_CHAN_SIP
opbxmod_LTLIBRARIES		+= chan_sip.la
chan_sip_la_SOURCES 		= chan_sip.c
chan_sip_la_LDFLAGS 		= -module -avoid-version -no-undefined
chan_sip_la_LIBADD  		= ${top_builddir}/corelib/libopenpbx.la
endif WANT_CHAN_SIP

if WANT_CHAN_UNICALL
opbxmod_LTLIBRARIES     	+= chan_unicall.la
chan_unicall_la_SOURCES 	= chan_unicall.c
chan_unicall_la_LIBADD 		= -lunicall ${top_builddir}/corelib/libopenpbx.la
chan_unicall_la_LDFLAGS 	= -module -avoid-version -no-undefined
endif WANT_CHAN_UNICALL

if WANT_CHAN_VISDN
opbxmod_LTLIBRARIES		+= chan_visdn.la
chan_visdn_la_SOURCES		= chan_visdn.c
chan_visdn_la_LIBADD		= -lq931 ${top_builddir}/corelib/libopenpbx.la
chan_visdn_la_LDFLAGS		= -module -avoid-version -no-undefined
endif WANT_CHAN_VISDN

if WANT_CHAN_WOOMERA
opbxmod_LTLIBRARIES		+= chan_woomera.la
chan_woomera_la_SOURCES		= chan_woomera.c
chan_woomera_la_LIBADD		= ${top_builddir}/corelib/libopenpbx.la
chan_woomera_la_LDFLAGS		= -module -avoid-version -no-undefined
endif WANT_CHAN_WOOMERA

if WANT_CHAN_BLUETOOTH
opbxmod_LTLIBRARIES		+= chan_bluetooth.la
chan_bluetooth_la_SOURCES	= chan_bluetooth.c
chan_bluetooth_la_LIBADD	= ${top_builddir}/corelib/libopenpbx.la -lbluetooth
chan_bluetooth_la_LDFLAGS	= -module -avoid-version -no-undefined
endif WANT_CHAN_BLUETOOTH

if WANT_CHAN_ZAP
opbxmod_LTLIBRARIES		+= chan_zap.la
chan_zap_la_SOURCES		= chan_zap.c
chan_zap_la_CFLAGS		= $(AM_CFLAGS)
chan_zap_la_LIBADD 		= -ltonezone
if HAVE_LIBPRI
chan_zap_la_CFLAGS		+= -DZAPATA_PRI
chan_zap_la_LIBADD      	+= -lpri
endif HAVE_LIBPRI
chan_zap_la_LIBADD		+= ${top_builddir}/corelib/libopenpbx.la
chan_zap_la_LDFLAGS		= -module -avoid-version -no-undefined
endif WANT_CHAN_ZAP

INCLUDES = -I${top_srcdir}/corelib -I$(top_srcdir)/include
