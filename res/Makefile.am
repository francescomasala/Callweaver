AUTOMAKE_OPTS 			= gnu
opbxmod_LTLIBRARIES		= res_crypto.la res_musiconhold.la \
				  res_indications.la res_ogi.la  res_monitor.la \
				  res_features.la res_adsi.la

res_adsi_la_SOURCES             = res_adsi.c
res_adsi_la_LDFLAGS		= -module -avoid-version -no-undefined
res_adsi_la_LIBADD              = ${top_builddir}/corelib/libopenpbx.la    

res_features_la_SOURCES		= res_features.c
res_features_la_LDFLAGS 	= -module -avoid-version -no-undefined
res_features_la_LIBADD		= ${top_builddir}/corelib/libopenpbx.la

res_crypto_la_SOURCES		= res_crypto.c
res_crypto_la_CFLAGS		= $(AM_CFLAGS) @SSL_CFLAGS@
res_crypto_la_LDFLAGS 		= -module -avoid-version -no-undefined
res_crypto_la_LIBADD		= @SSL_LIBS@ ${top_builddir}/corelib/libopenpbx.la     

res_musiconhold_la_SOURCES	= res_musiconhold.c
res_musiconhold_la_LDFLAGS 	= -module -avoid-version -no-undefined
res_musiconhold_la_LIBADD	= ${top_builddir}/corelib/libopenpbx.la     

res_indications_la_SOURCES 	= res_indications.c
res_indications_la_LDFLAGS 	= -module -avoid-version -no-undefined
res_indications_la_LIBADD	= ${top_builddir}/corelib/libopenpbx.la

res_monitor_la_SOURCES 		= res_monitor.c
res_monitor_la_LDFLAGS 		= -module -avoid-version -no-undefined
res_monitor_la_LIBADD		= ${top_builddir}/corelib/libopenpbx.la  

res_ogi_la_SOURCES 		= res_ogi.c
res_ogi_la_LDFLAGS 		= -module -avoid-version -no-undefined
res_ogi_la_LIBADD		= ${top_builddir}/corelib/libopenpbx.la  

if WANT_RES_SQLITE
opbxmod_LTLIBRARIES	+= res_sqlite.la
res_sqlite_la_SOURCES	= res_sqlite.c
res_sqlite_la_LDFLAGS	= -module -avoid-version -no-undefined
res_sqlite_la_LIBADD	= @SQLITE3_THREADSAFE_LIBS@ ${top_builddir}/corelib/libopenpbx.la  
res_sqlite_la_CFLAGS	= $(AM_CFLAGS) @SQLITE3_THREADSAFE_CFLAGS@ -I${top_srcdir}/sqlite3-embedded/src
endif WANT_RES_SQLITE

if WANT_RES_JABBER
opbxmod_LTLIBRARIES   	+= res_jabber.la
res_jabber_la_SOURCES 	= res_jabber.c
res_jabber_la_LDFLAGS  	= -module -avoid-version -no-undefined
res_jabber_la_CFLAGS   	= $(AM_CFLAGS) @LOUDMOUTH_CFLAGS@
res_jabber_la_LIBADD   	= @LOUDMOUTH_LIBS@ ${top_builddir}/corelib/libopenpbx.la  
endif WANT_RES_JABBER

if WANT_RES_CONFIG_MYSQL
opbxmod_LTLIBRARIES     	+= res_config_mysql.la
res_config_mysql_la_SOURCES   = res_config_mysql.c
res_config_mysql_la_LDFLAGS   = -module -avoid-version -no-undefined
res_config_mysql_la_CFLAGS    = $(AM_CFLAGS) @MYSQL_CFLAGS@
res_config_mysql_la_LIBADD    = @MYSQL_LIBS@ ${top_builddir}/corelib/libopenpbx.la  
endif WANT_RES_CONFIG_MYSQL

if WANT_RES_CONFIG_PGSQL
opbxmod_LTLIBRARIES     	+= res_config_pgsql.la
res_config_pgsql_la_SOURCES   = res_config_pgsql.c
res_config_pgsql_la_LDFLAGS   = -module -avoid-version -no-undefined
res_config_pgsql_la_CFLAGS    = $(AM_CFLAGS) @PQINCPATH@
res_config_pgsql_la_LIBADD    = @PQLIBPATH@ -lpq ${top_builddir}/corelib/libopenpbx.la  
endif WANT_RES_CONFIG_PGSQL

if WANT_RES_ODBC
opbxmod_LTLIBRARIES 		+= res_odbc.la
res_odbc_la_SOURCES		= res_odbc.c
res_odbc_la_LDFLAGS		= -module -avoid-version -no-undefined
res_odbc_la_CFLAGS		= $(AM_CFLAGS) @ODBC_CFLAGS@
res_odbc_la_LIBADD		= @ODBC_LIBS@ ${top_builddir}/corelib/libopenpbx.la  
endif WANT_RES_ODBC

if WANT_RES_CONFIG_ODBC
opbxmod_LTLIBRARIES		+= res_config_odbc.la
res_config_odbc_la_SOURCES	= res_config_odbc.c
res_config_odbc_la_LDFLAGS	= -module -avoid-version -no-undefined
res_config_odbc_la_CFLAGS	= $(AM_CFLAGS) @ODBC_CFLAGS@
res_config_odbc_la_LIBADD	= @ODBC_LIBS@ ${top_builddir}/corelib/libopenpbx.la
#res_config_odbc_la_LIBADD	= @ODBC_LIBS@ ${top_builddir}/corelib/libopenpbx.la res_odbc.la
endif WANT_RES_CONFIG_ODBC  

if WANT_OSP
opbxmod_LTLIBRARIES	+= res_osp.la
res_osp_la_SOURCES	= res_osp.c
res_osp_la_LDFLAGS	= -module -avoid-version -no-undefined
res_osp_la_CFLAGS	= $(AM_CFLAGS) @OSP_CFLAGS@ @SSL_CFLAGS@
res_osp_la_LIBADD	= @OSP_LIBS@ @SSL_LIBS@ ${top_builddir}/corelib/libopenpbx.la  
endif WANT_OSP

if WANT_RES_JS
opbxmod_LTLIBRARIES	+= res_js.la
res_js_la_SOURCES	= res_js.c
res_js_la_CFLAGS	= $(AM_CLFAGS) @JS_CFLAGS@ @NSPR_CFLAGS@
res_js_la_LIBADD	= @JS_LDFLAGS@ @NSPR_LDFLAGS@
res_js_la_LDFLAGS	= -module -avoid-version -no-undefined
endif WANT_RES_JS

if WANT_RES_CONFIG_CURL
opbxmod_LTLIBRARIES		+= res_config_curl.la
res_config_curl_la_SOURCES	= res_config_curl.c
res_config_curl_la_LDFLAGS	= -module -avoid-version -no-undefined
res_config_curl_la_CFLAGS	= $(AM_CFLAGS) @ODBC_CFLAGS@
res_config_curl_la_LIBADD	= @ODBC_LIBS@ ${top_builddir}/corelib/libopenpbx.la
endif WANT_RES_CONFIG_CURL

INCLUDES = -I$(top_srcdir)/include
