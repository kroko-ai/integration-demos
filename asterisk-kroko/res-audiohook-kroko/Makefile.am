MAINTAINERCLEANFILES          = Makefile.in

AM_CPPFLAGS                   = -I$(top_srcdir)/include $(ASTERISK_INCLUDES)
AM_CFLAGS                     = -DAST_MODULE_SELF_SYM="__internal_res_audiohook_kroko"

moddir                        = $(ASTERISK_MODDIR)
mod_LTLIBRARIES               = res_audiohook_kroko.la

res_audiohook_kroko_la_SOURCES = res_audiohook_kroko.c
res_audiohook_kroko_la_LDFLAGS = -avoid-version -no-undefined -module
res_audiohook_kroko_la_LIBADD  = $(kroko_LIBS) -lspeexdsp

load: 
	asterisk -rx "module load res_audiohook_kroko.so"

unload: 
	asterisk -rx "module unload res_audiohook_kroko.so"
