AUTOMAKE_OPTS = gnu
SAMPLES = adsi.conf.sample \
	adtranvofr.conf.sample \
	cdr.conf.sample \
	cdr_custom.conf.sample \
	cdr_manager.conf.sample \
	cdr_tds.conf.sample \
	codecs.conf.sample \
	dnsmgr.conf.sample \
	enum.conf.sample \
	extconfig.conf.sample \
	extensions.conf.sample \
	extensions.ael.sample \
	indications.conf.sample \
	logger.conf.sample \
	manager.conf.sample \
	meetme.conf.sample \
	modem.conf.sample \
	modules.conf.sample \
	musiconhold.conf.sample \
	muted.conf.sample \
	openpbx.adsi.sample \
	openpbx.conf.sample \
	osp.conf.sample \
	privacy.conf.sample \
	rpt.conf.sample \
	rtp.conf.sample \
	udptl.conf.sample

if WANT_APP_QUEUE
SAMPLES += queues.conf.sample
endif WANT_APP_QUEUE

if WANT_APP_VOICEMAIL
SAMPLES += voicemail.conf.sample
endif WANT_APP_VOICEMAIL

if WANT_CDR_MYSQL
SAMPLES	+= cdr_mysql.conf.sample
endif WANT_CDR_MYSQL

if WANT_CDR_ODBC
SAMPLES	+= cdr_odbc.conf.sample
endif WANT_CDR_ODBC

if WANT_CDR_PGSQL
SAMPLES	+= cdr_pgsql.conf.sample
endif WANT_CDR_PGSQL

if WANT_CHAN_AGENT
SAMPLES += agents.conf.sample
endif WANT_CHAN_AGENT

if WANT_CHAN_ALSA
SAMPLES	+= alsa.conf.sample
endif WANT_CHAN_ALSA

if WANT_CHAN_CAPI
SAMPLES += capi.conf.sample
endif WANT_CHAN_CAPI

if WANT_CHAN_FAX
SAMPLES += chan_fax.conf.sample
endif WANT_CHAN_FAX

if WANT_CHAN_FEATURES
SAMPLES += features.conf.sample
endif WANT_CHAN_FEATURES

if WANT_CHAN_IAX2
SAMPLES += iax.conf.sample
endif WANT_CHAN_IAX2

if WANT_CHAN_MGCP
SAMPLES += mgcp.conf.sample
endif WANT_CHAN_MGCP

if WANT_CHAN_MISDN
SAMPLES += misdn.conf.sample
endif WANT_CHAN_MISDN

if WANT_CHAN_SIP
SAMPLES += sip.conf.sample sip_notify.conf.sample
endif WANT_CHAN_SIP

if WANT_CHAN_SCCP
SAMPLES	+= sccp.conf.sample
endif WANT_CHAN_SCCP

if WANT_CHAN_UNICALL
SAMPLES += unicall.conf.sample
endif WANT_CHAN_UNICALL

if WANT_CHAN_VISDN
SAMPLES += chan_visdn.conf.sample
endif WANT_CHAN_VISDN

if WANT_CHAN_WOOMERA
SAMPLES += woomera.conf.sample
endif WANT_CHAN_WOOMERA

if WANT_CHAN_ZAP
SAMPLES += zapata.conf.sample
endif WANT_CHAN_ZAP

if WANT_PBX_DUNDI
SAMPLES += dundi.conf.sample
endif WANT_PBX_DUNDI

if WANT_RES_JABBER
SAMPLES += res_jabber.conf.sample
endif WANT_RES_JABBER

if WANT_RES_ODBC
SAMPLES += res_odbc.conf.sample
endif WANT_RES_ODBC

if WANT_RES_CONFIG_MYSQL
SAMPLES += res_mysql.conf.sample
endif WANT_RES_CONFIG_MYSQL

if WANT_RES_CONFIG_PGSQL
SAMPLES += res_pgsql.conf.sample
endif WANT_RES_CONFIG_PGSQL

if WANT_RES_CONFIG_CURL
SAMPLES += curl.conf.sample
endif WANT_RES_CONFIG_CURL

if WANT_RES_SQLITE
SAMPLES += res_sqlite.conf.sample
endif WANT_RES_SQLITE

if WANT_CHAN_BLUETOOTH
SAMPLES += chan_bluetooth.conf.sample
endif WANT_CHAN_BLUETOOTH

noinst_DATA = $(SAMPLES)

@substitute@

EXTRA_DIST = $(SAMPLES) openpbx.conf.sample.in logger.conf.sample.in 
CLEANFILES = openpbx.conf.sample logger.conf.sample musiconhold.conf.sample

openpbx.conf.sample: openpbx.conf.sample.in Makefile
	rm -f openpbx.conf.sample.tmp
	$(substitute) $(srcdir)/openpbx.conf.sample.in > openpbx.conf.sample.tmp
	mv openpbx.conf.sample.tmp openpbx.conf.sample

logger.conf.sample: logger.conf.sample.in Makefile
	rm -f logger.conf.sample.tmp
	$(substitute) $(srcdir)/logger.conf.sample.in > logger.conf.sample.tmp
	mv logger.conf.sample.tmp logger.conf.sample

musiconhold.conf.sample: musiconhold.conf.sample.in Makefile
	rm -f musiconhold.conf.sample.tmp
	$(substitute) $(srcdir)/musiconhold.conf.sample.in > musiconhold.conf.sample.tmp
	mv musiconhold.conf.sample.tmp musiconhold.conf.sample

install-data-local:
	mkdir -p $(DESTDIR)${opbxconfdir}; \
	if test -z "$(DESTDIR)"; \
	then \
		chown ${opbxrunuser}:${opbxrungroup} $(DESTDIR)${opbxconfdir}; \
	fi; \
	chmod 0750 $(DESTDIR)${opbxconfdir}; \
	for samplefile in $(SAMPLES); \
	do \
		configfile=`basename $${samplefile} .sample`; \
		if test ! -f $(DESTDIR)${opbxconfdir}/$${configfile}; \
		then \
			$(INSTALL) -m 0640 $${samplefile} $(DESTDIR)${opbxconfdir}/$${configfile}; \
			if test -z "$(DESTDIR)"; \
			then \
				chown ${opbxrunuser}:${opbxrungroup} $(DESTDIR)${opbxconfdir}/$${configfile}; \
			fi; \
		fi; \
	done
