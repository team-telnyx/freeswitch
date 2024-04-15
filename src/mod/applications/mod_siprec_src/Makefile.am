include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_siprec_src

mod_LTLIBRARIES = mod_siprec_src.la
mod_siprec_src_la_SOURCES  = mod_siprec_src.c
mod_siprec_src_la_CFLAGS   = $(AM_CFLAGS)
mod_siprec_src_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_siprec_src_la_LDFLAGS  = -avoid-version -module -no-undefined -shared
