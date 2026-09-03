include $(top_srcdir)/build/modmake.rulesam

noinst_PROGRAMS = switch_event switch_hash switch_ivr_originate switch_utils switch_core switch_console switch_vpx switch_core_file \
			   switch_ivr_play_say switch_core_codec switch_rtp switch_xml
noinst_PROGRAMS += switch_core_video switch_core_db switch_vad switch_packetizer switch_core_session test_sofia switch_ivr_async switch_core_asr switch_log switch_stun switch_trickle_ice switch_bundle
noinst_PROGRAMS += test_tts_format
noinst_PROGRAMS += switch_core_media
noinst_PROGRAMS += test_mod_verto
noinst_PROGRAMS += test_mod_event_socket
noinst_PROGRAMS += switch_timer

noinst_PROGRAMS += switch_hold switch_sip switch_mod_telnyx switch_mod_gstt switch_3pcc test_inuse_race switch_jb_deadlock switch_cjson switch_sockaddr test_rtp_set_flag_null_crash test_write_frame_foreign_codec test_video_bridge_engine_race switch_apr_cleanup test_dtmf_sent_event

# switch_mod_lumenvox is intentionally NOT built/run in CI yet: LumenVox servers
# are not reachable from CI, so the ASR tests cannot run there. Re-enable by
# appending switch_mod_lumenvox to the line above. See TELCORE-249.

if HAVE_SOFIA_SIP
noinst_PROGRAMS += switch_sdp
switch_sdp_SOURCES = switch_sdp.c
switch_sdp_CFLAGS  = $(AM_CFLAGS) $(SOFIA_SIP_CFLAGS)
switch_sdp_LDADD   = $(switch_builddir)/libfreeswitch.la $(SOFIA_SIP_LIBS)

switch_trickle_ice_SOURCES = switch_trickle_ice.c
switch_trickle_ice_CFLAGS  = $(AM_CFLAGS) $(SOFIA_SIP_CFLAGS)
switch_trickle_ice_LDADD   = $(switch_builddir)/libfreeswitch.la $(SOFIA_SIP_LIBS)
endif

if HAVE_PCAP
noinst_PROGRAMS += switch_rtp_pcap
AM_LDFLAGS += $(PCAP_LIBS)
endif 

AM_LDFLAGS += -avoid-version -no-undefined $(SWITCH_AM_LDFLAGS) $(openssl_LIBS)
AM_LDFLAGS += $(FREESWITCH_LIBS) $(switch_builddir)/libfreeswitch.la $(CORE_LIBS) $(APR_LIBS)

# "make check" will not run these.
examples = switch_eavesdrop

if HAVE_FVAD
AM_CFLAGS += -DSWITCH_HAVE_FVAD
endif

TESTS = $(noinst_PROGRAMS)

bin_PROGRAMS = $(examples)
