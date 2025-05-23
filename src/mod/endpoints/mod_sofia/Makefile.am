include $(top_srcdir)/build/modmake.rulesam

MODNAME=mod_sofia

noinst_LTLIBRARIES = libsofiamod.la
libsofiamod_la_SOURCES   =  mod_sofia.c sofia.c sofia_json_api.c sofia_glue.c sofia_presence.c sofia_reg.c sofia_media.c sip-dig.c rtp.c mod_sofia.h sip-dig.h
libsofiamod_la_LDFLAGS   = -lstdc++ -static
libsofiamod_la_CFLAGS  = $(AM_CFLAGS) -I. $(SOFIA_CMD_LINE_CFLAGS) $(SOFIA_SIP_CFLAGS) $(STIRSHAKEN_CFLAGS)
if HAVE_STIRSHAKEN
libsofiamod_la_CFLAGS += -DHAVE_STIRSHAKEN
endif

mod_LTLIBRARIES = mod_sofia.la
mod_sofia_la_SOURCES = prometheus_metrics.cpp prometheus_metrics.h
mod_sofia_la_LIBADD = $(switch_builddir)/libfreeswitch.la libsofiamod.la
mod_sofia_la_LDFLAGS = -avoid-version -module -no-undefined -shared -lstdc++ $(SOFIA_SIP_LIBS) $(STIRSHAKEN_LIBS)

noinst_PROGRAMS = test/test_sofia_funcs test/test_nuafail test/sipp-based-tests

test_test_sofia_funcs_SOURCES = test/test_sofia_funcs.c
test_test_sofia_funcs_CFLAGS = $(AM_CFLAGS) $(SOFIA_SIP_CFLAGS) $(STIRSHAKEN_CFLAGS) -DSWITCH_TEST_BASE_DIR_FOR_CONF=\"${abs_builddir}/test\" -DSWITCH_TEST_BASE_DIR_OVERRIDE=\"${abs_builddir}/test\"
if HAVE_STIRSHAKEN
test_test_sofia_funcs_CFLAGS += -DHAVE_STIRSHAKEN
endif
test_test_sofia_funcs_LDFLAGS = $(AM_LDFLAGS) -avoid-version -no-undefined $(freeswitch_LDFLAGS) $(switch_builddir)/libfreeswitch.la $(CORE_LIBS) $(APR_LIBS) $(STIRSHAKEN_LIBS)
test_test_sofia_funcs_LDADD = libsofiamod.la mod_sofia.la $(SOFIA_SIP_LIBS) $(STIRSHAKEN_LIBS)

test_test_nuafail_SOURCES = test/test_nuafail.c
test_test_nuafail_CFLAGS = $(AM_CFLAGS) $(SOFIA_SIP_CFLAGS) $(STIRSHAKEN_CFLAGS) -DSWITCH_TEST_BASE_DIR_FOR_CONF=\"${abs_builddir}/test\" -DSWITCH_TEST_BASE_DIR_OVERRIDE=\"${abs_builddir}/test\"
if HAVE_STIRSHAKEN
test_test_nuafail_CFLAGS += -DHAVE_STIRSHAKEN
endif
test_test_nuafail_LDFLAGS = $(AM_LDFLAGS) -avoid-version -no-undefined $(freeswitch_LDFLAGS) -lstdc++ $(switch_builddir)/libfreeswitch.la $(CORE_LIBS) $(APR_LIBS) $(STIRSHAKEN_LIBS)
test_test_nuafail_LDADD = libsofiamod.la $(SOFIA_SIP_LIBS)

test_sipp_based_tests_SOURCES = test/sipp-based-tests.c
test_sipp_based_tests_CFLAGS = $(AM_CFLAGS) $(SOFIA_SIP_CFLAGS) -DSWITCH_TEST_BASE_DIR_FOR_CONF=\"${abs_builddir}/test\" -DSWITCH_TEST_BASE_DIR_OVERRIDE=\"${abs_builddir}/test\"
test_sipp_based_tests_LDFLAGS = $(AM_LDFLAGS) -avoid-version -no-undefined $(freeswitch_LDFLAGS) -lstdc++ $(switch_builddir)/libfreeswitch.la $(CORE_LIBS) $(APR_LIBS)
test_sipp_based_tests_LDADD = libsofiamod.la $(SOFIA_SIP_LIBS)

TESTS = test/test_sofia_funcs.sh test/test_nuafail test/test_run_sipp.sh

if ISMAC
mod_sofia_la_LDFLAGS += -framework CoreFoundation -framework SystemConfiguration
test_test_sofia_funcs_LDFLAGS += -framework CoreFoundation -framework SystemConfiguration
test_test_nuafail_LDFLAGS += -framework CoreFoundation -framework SystemConfiguration
test_sipp_based_tests_LDFLAGS += -framework CoreFoundation -framework SystemConfiguration
endif

