include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_avmd

mod_LTLIBRARIES = mod_avmd.la
mod_avmd_la_SOURCES  = mod_avmd.c avmd_buffer.c avmd_candidate.c avmd_desa2_tweaked.c avmd_fast_acosf.c avmd_goertzel.c avmd_spectral.c prometheus_metrics.cpp
mod_avmd_la_CFLAGS   = $(AM_CFLAGS) $(AM_MOD_AVMD_CXXFLAGS)
mod_avmd_la_CPPFLAGS = $(AM_MOD_AVMD_CXXFLAGS) $(AM_CPPFLAGS)
mod_avmd_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_avmd_la_LDFLAGS  = -avoid-version -module -no-undefined -shared

noinst_PROGRAMS = test/test_avmd_spectral test/test_avmd_candidate test/avmd_spectral_scan
test_test_avmd_spectral_SOURCES = test/test_avmd_spectral.c avmd_candidate.c avmd_goertzel.c avmd_spectral.c
test_test_avmd_spectral_CFLAGS = $(AM_CFLAGS) -I./ -DAVMD_TEST_FIXTURE_DIR=\"$(abs_srcdir)/test/fixtures\"
test_test_avmd_spectral_LDADD = -lm
test_test_avmd_candidate_SOURCES = test/test_avmd_candidate.c avmd_candidate.c
test_test_avmd_candidate_CFLAGS = $(AM_CFLAGS) -I./
test_test_avmd_candidate_LDADD = -lm
test_avmd_spectral_scan_SOURCES = test/avmd_spectral_scan.c avmd_goertzel.c avmd_spectral.c
test_avmd_spectral_scan_CFLAGS = $(AM_CFLAGS) -I./
test_avmd_spectral_scan_LDADD = -lm
TESTS = test/test_avmd_spectral test/test_avmd_candidate test/test_avmd_pcap_extract_unit.py
EXTRA_DIST = test/fixtures/README.md test/fixtures/positive-voicemail-beep.ulaw test/fixtures/negative-transfer-ringback.ulaw test/fixtures/real-music-cc0.wav test/avmd_pcap_extract.py test/test_avmd_pcap_extract_unit.py test/avmd_corpus.json test/run_avmd_integration.py
