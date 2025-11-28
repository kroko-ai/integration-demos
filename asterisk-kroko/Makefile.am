AUTOMAKE_OPTIONS     = foreign
MAINTAINERCLEANFILES = Makefile.in aclocal.m4 configure
ACLOCAL_AMFLAGS      = -I m4

SUBDIRS              = res-speech-kroko res-audiohook-kroko

EXTRA_DIST           = bootstrap res-speech-kroko/res_speech_kroko.conf

install-data-local:
	test -d $(DESTDIR)$(ASTERISK_CONF_DIR) || $(mkinstalldirs) $(DESTDIR)$(ASTERISK_CONF_DIR)
	test -f $(DESTDIR)$(ASTERISK_CONF_DIR)/res_speech_kroko.conf || $(INSTALL) -m 644 $(top_srcdir)/res-speech-kroko/res_speech_kroko.conf $(DESTDIR)$(ASTERISK_CONF_DIR)
