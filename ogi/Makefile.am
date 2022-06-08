AUTOMAKE_OPTS 			= gnu
opbxogi_PROGRAMS 		= eogi-sphinx-test eogi-test
opbxogi_SCRIPTS 		= ogi-test.ogi fastogi-test
eogi_sphinx_test_SOURCES 	= eogi-sphinx-test.c
eogi_test_SOURCES 		= eogi-test.c

install-data-hook:
	if test -z "$(DESTDIR)"; \
	then \
		chown -R ${opbxrunuser}:${opbxrungroup} $(DESTDIR)${opbxogidir}; \
	fi; \
	chmod 0750 $(DESTDIR)${opbxogidir}

INCLUDES = -I$(top_srcdir)/include
