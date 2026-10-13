include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_spandsp

mod_LTLIBRARIES = mod_spandsp.la
mod_spandsp_la_SOURCES  = prometheus_metrics.cpp mod_spandsp.c udptl.c mod_spandsp_fax.c mod_spandsp_dsp.c mod_spandsp_codecs.c mod_spandsp_modem.c
mod_spandsp_la_CFLAGS   = $(AM_CFLAGS) 
mod_spandsp_la_CXXFLAGS = $(AM_CXXFLAGS)
mod_spandsp_la_CPPFLAGS = $(AM_CPPFLAGS) -I$(SPANDSP_DIR)/src -I$(SPANDSP_BUILDDIR)/src -I.
mod_spandsp_la_LIBADD   = $(switch_builddir)/libfreeswitch.la $(SPANDSP_LA) $(SPANDSP_LA_JBIG) $(SPANDSP_LA_LZMA) -ljpeg -lz -ltiff
mod_spandsp_la_LDFLAGS  = -avoid-version -module -no-undefined -shared

# Regression test for the T.38 gateway relay: the calling tone (CNG) must not
# hold back the messages of the calling terminal. Plain C against libspandsp,
# no libfreeswitch: it makes the same calls as mod_spandsp_fax.c in gateway
# mode. Declared with noinst_PROGRAMS so that `make all` builds it: the
# unit-test runner (tests/unit/run-tests.mk + test.sh) runs test binaries
# directly and never runs `make check`. The header of the test shows how to run
# it without autotools.
noinst_PROGRAMS = test/test_t38_gateway_cng
TESTS = test/test_t38_gateway_cng

test_test_t38_gateway_cng_SOURCES  = test/test_t38_gateway_cng.c
test_test_t38_gateway_cng_CPPFLAGS = $(SPANDSP_CFLAGS)
test_test_t38_gateway_cng_CFLAGS   = -Wall -Wextra
test_test_t38_gateway_cng_LDADD    = $(SPANDSP_LIBS) -lm
