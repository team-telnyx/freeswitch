include $(top_srcdir)/build/modmake.rulesam

AM_CPPFLAGS += $(CARES_CFLAGS)
AM_CFLAGS   += $(CARES_CFLAGS)
AM_CXXFLAGS = -std=c++17
AM_LDFLAGS  += -avoid-version -no-undefined $(SWITCH_AM_LDFLAGS) $(openssl_LIBS)
AM_LDFLAGS  += $(FREESWITCH_LIBS) $(switch_builddir)/libfreeswitch.la $(CORE_LIBS) $(APR_LIBS)
AM_LDFLAGS  += $(CARES_LIBS) -lboost_system -lpthread

# Regression test for ZMQ wrapper timeouts (always built, no extra deps)
noinst_PROGRAMS = test_zmqsocket_timeouts
test_zmqsocket_timeouts_SOURCES = test_zmqsocket_timeouts.cpp
test_zmqsocket_timeouts_LDADD   = -lzmq -lstdc++

noinst_PROGRAMS += test_web_server_registry
test_web_server_registry_SOURCES = test_web_server_registry.cpp

if HAVE_CARES
noinst_PROGRAMS += test_resolver test_mod_resolver

# Dummy FS module — convenience lib for linking into the test binary,
# and shared module for FST_MODULE_BEGIN to dlopen
noinst_LTLIBRARIES = libmod_test_resolver.la
libmod_test_resolver_la_SOURCES = mod_test_resolver.cpp
libmod_test_resolver_la_CXXFLAGS = $(AM_CXXFLAGS)

mod_LTLIBRARIES = mod_test_resolver.la
mod_test_resolver_la_SOURCES =
mod_test_resolver_la_LIBADD = $(switch_builddir)/libfreeswitch.la libmod_test_resolver.la
mod_test_resolver_la_LDFLAGS = -avoid-version -module -no-undefined -shared -lboost_system $(CARES_LIBS) -lstdc++

# Standalone C++ test (no FS core needed)
test_resolver_SOURCES = test_resolver.cpp

# FST-based test that loads the module.
# SWITCH_TEST_BASE_DIR_OVERRIDE = abs_builddir/conf so that
# FST_MODULE_BEGIN looks in abs_builddir/conf/../.libs/ = abs_builddir/.libs/
test_mod_resolver_SOURCES = test_mod_resolver.c
test_mod_resolver_CFLAGS  = $(AM_CFLAGS) -I$(srcdir) \
	-DSWITCH_TEST_BASE_DIR_FOR_CONF=\"$(abs_builddir)/conf\" \
	-DSWITCH_TEST_BASE_DIR_OVERRIDE=\"$(abs_builddir)/conf\"
test_mod_resolver_LDADD   = libmod_test_resolver.la -lstdc++
endif

TESTS = $(noinst_PROGRAMS)

# Copy conf to build directory for out-of-tree builds
all-local:
	@if test "$(abs_srcdir)" != "$(abs_builddir)"; then \
		cp -rp $(srcdir)/conf $(abs_builddir)/; \
	fi
