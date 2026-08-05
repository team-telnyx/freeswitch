#include <switch.h>
#include <switch_rtp.h>
#include <test/switch_test.h>
#include <private/switch_rtp_pvt.h>
#include "switch_telnyx.h"
#include <sofia-sip/sdp.h>

#define USE_SWITCH_RTP_NEW_IPPORT 1

extern char *fst_getenv_default(const char *, char *, switch_bool_t);
static void _silence_unused(void) { (void)fst_getenv_default; }
static const char *rx_host = "127.0.0.1";

typedef struct trickle_captured_s {
	int called;
	int last_mline;
	int last_eoc;
	char last_mid[64];
	switch_rtp_ice_cand_t last_cand;
} trickle_captured_t;

static void on_local_candidate_cb(void *user_data,
                                  const char *mid,
                                  int mline_index,
                                  const switch_rtp_ice_cand_t *cand,
                                  int end_of_candidates)
{
	trickle_captured_t *cap = (trickle_captured_t *)user_data;

	cap->called++;
	cap->last_mline = mline_index;
	cap->last_eoc   = end_of_candidates;
	switch_snprintf(cap->last_mid, sizeof(cap->last_mid), "%s", mid ? mid : "(null)");

	memset(&cap->last_cand, 0, sizeof(cap->last_cand));
	if (cand) {
		cap->last_cand.component_id = cand->component_id;
		switch_snprintf(cap->last_cand.ip, sizeof(cap->last_cand.ip), "%s", cand->ip);
		switch_snprintf(cap->last_cand.transport, sizeof(cap->last_cand.transport), "%s", cand->transport);
		cap->last_cand.port = cand->port;
		cap->last_cand.priority = cand->priority;
	}
}

/* A second callback to prove overwrite semantics */
static void on_local_candidate_cb_2(void *user_data,
                                    const char *mid,
                                    int mline_index,
                                    const switch_rtp_ice_cand_t *cand,
                                    int end_of_candidates)
{
	trickle_captured_t *cap = (trickle_captured_t *)user_data;
	/* Mark different values so we can tell which handler ran */
	cap->called += 10;
	cap->last_mline = mline_index + 100;
	cap->last_eoc   = end_of_candidates ? 99 : 98;
	switch_snprintf(cap->last_mid, sizeof(cap->last_mid), "cb2:%s", mid ? mid : "(null)");

	memset(&cap->last_cand, 0, sizeof(cap->last_cand));
	if (cand) {
		cap->last_cand.component_id = cand->component_id + 100;
		switch_snprintf(cap->last_cand.ip, sizeof(cap->last_cand.ip), "cb2-%s", cand->ip[0] ? cand->ip : "none");
		switch_snprintf(cap->last_cand.transport, sizeof(cap->last_cand.transport), "cb2-%s", cand->transport[0] ? cand->transport : "none");
		cap->last_cand.port = cand->port + 100;
		cap->last_cand.priority = cand->priority + 100;
	}
}

static switch_status_t make_real_rtp(switch_memory_pool_t *pool,
                                     switch_rtp_t **out_rtp,
                                     const char **out_err)
{

	switch_rtp_t *rtp;
	switch_payload_t payload = 0;        /* payload doesn't matter here */
	uint32_t spi = 160;                  /* 20ms @ 8kHz */
	uint32_t mpp = 20;                   /* 20ms packets */
	switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID] = { 0 };
	const char *dummy_err = NULL;
	const char **errp = out_err ? out_err : &dummy_err;

	rtp = switch_rtp_new("127.0.0.1", /* rx_host */
			1234,           /* rx_port => ephemeral */
			"127.0.0.1",        /* tx_host */
			5432,           /* tx_port */
			payload,
			spi,
			mpp,
			flags,      
			NULL,
			errp,
			pool);
	
	if (rtp) { *out_rtp = rtp; return SWITCH_STATUS_SUCCESS; }
	return SWITCH_STATUS_FALSE;
}


/* Declare the production trickle ICE function */
extern switch_status_t switch_core_media_trickle_remote_candidate_and_recheck(switch_core_session_t *session, switch_media_handle_t *smh, void *sdp_session, switch_sdp_type_t sdp_type, const char *mid, int mline_index, const char *cand_line, int end_of_candidates);

static void cleanup_rtp(switch_rtp_t **rtp)
{
	if (rtp && *rtp) {
		switch_rtp_destroy(rtp);
	}
}

static void cleanup_session_and_media(switch_core_session_t *session)
{
	switch_media_handle_t *smh = NULL;

	if (!session) return;

	/* Clean up media handle and RTP sessions to free DTLS contexts */
	smh = switch_core_session_get_media_handle(session);
	if (smh) {
		/* Deactivate RTP which will call switch_rtp_destroy internally */
		switch_core_media_deactivate_rtp(session);
	}

	switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
	switch_core_session_rwunlock(session);
}

static void cleanup_session_media_and_sdp(switch_core_session_t *session, void *sdp_session, sdp_parser_t *parser)
{
	/* Clean up SDP parser if allocated */
	if (parser) {
		sdp_parser_free(parser);
	}
	cleanup_session_and_media(session);
}

static switch_status_t make_session_and_rtp_with_sdp(switch_core_session_t **out_session,
                                                      switch_rtp_t **out_rtp,
                                                      void **out_sdp_session,
                                                      sdp_parser_t **out_parser)
{
	switch_status_t st;
	switch_call_cause_t cause = SWITCH_CAUSE_NONE, cancel = SWITCH_CAUSE_NONE;
	switch_core_session_t *session = NULL;
	switch_media_handle_t *media_handle;
	switch_core_media_params_t *mparams;
	switch_status_t status;
	switch_channel_t *chan = NULL;
	char *r_sdp;
	uint8_t match = 0, p = 0;

	const char *br = "{"
		"absolute_codec_string=PCMU,"
		"codec_string=PCMU,"
		"codec_ms=20,"
		"rtp_disable_crypto=true,"
		"rtp_enable_timer=false,"
		"rtp_timer_name=none,"
		"hangup_after_bridge=false,"
		"ignore_early_media=true,"
		"loopback_bowout=false,"
		"media_webrtc=false,"
		"rtp_trickle_ice=true"
		"}loopback/9999";

	st = switch_ivr_originate(
			NULL,                  /* a-leg session */
			&session,              /* out: b-leg */
			&cause,                /* out: cause */
			br,                    /* bridgeto */
			5,                     /* timeout (sec) */
			NULL, NULL, NULL, NULL,/* table, cid_name, cid_num, outbound_profile_uuid */
			NULL,                  /* ovars (NULL, since we inlined) */
			SOF_NONE,              /* flags */
			&cancel,               /* out: cancel cause */
			NULL);                 /* dial handle */

	if (st != SWITCH_STATUS_SUCCESS || !session) return SWITCH_STATUS_FALSE;

	chan = switch_core_session_get_channel(session);

	mparams = switch_core_session_alloc(session, sizeof(switch_core_media_params_t));
	mparams->inbound_codec_string = switch_core_session_strdup(session, "PCMU");
	mparams->outbound_codec_string = switch_core_session_strdup(session, "PCMU");
	mparams->rtpip = switch_core_session_strdup(session, (char *)rx_host);
	mparams->rtpip4 = switch_core_session_strdup(session, (char *)rx_host);

	status = switch_media_handle_create(&media_handle, session, mparams);
	if (status != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "switch_media_handle_create() failed\n");
		return SWITCH_STATUS_FALSE;
	}

	switch_channel_set_variable(chan, "absolute_codec_string", "PCMU");
	switch_channel_set_variable(chan, "send_silence_when_idle", "-1");
	switch_channel_set_variable(chan, "rtp_timer_name", "soft");
	switch_channel_set_variable(chan, "media_timeout", "1000");
	switch_channel_set_variable(chan, "rtp_trickle_ice", "true");

	r_sdp = switch_core_session_sprintf(session,
			"v=0\n"
			"o=- 1683118194 1683118195 IN IP4 0.0.0.0\n"
			"s=-\n"
			"t=0 0\n"
			"a=group:BUNDLE 0\n"
			"a=extmap-allow-mixed\n"
			"m=audio 9 UDP/TLS/RTP/SAVPF 0\n"
			"c=IN IP4 0.0.0.0\n"
			"a=ice-ufrag:aZJpsl00bYnjrOZtkCFMtKhFC/CHAfcv\n"
			"a=ice-pwd:aNniSnLLp43SSsJrz6TNPty1zPrxZNzh\n"
			"a=ice-options:trickle\n"
			"a=rtcp-mux\n"
			"a=setup:active\n"
			"a=rtpmap:0 PCMU/8000\n"
			"a=ssrc:2588681350 msid:user199999@host-a1132918 webrtctransceiver0\n"
			"a=ssrc:2588681350 cname:user19999@host-a1132918\n"
			"a=sendrecv\n"
			"a=fingerprint:sha-256 17:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
			"a=mid:0\n"
			"a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level\n"
			"a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time\n"
			"a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\n"
			"a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid\n"
			"a=msid:de61dc02-51d0-4164-9d7a-b74141a4548e 9dc86822-54a5-4506-8476-9be2238be778\n"
			"a=rtcp-rsize\n");

	if (switch_core_media_prepare_codecs(session, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to prepare codecs\n");
		goto fail;
	}

	match = switch_core_media_negotiate_sdp(session, r_sdp, &p, SDP_OFFER);

	if (match) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "SDP negotiation successful (match=%d, proceed=%d)\n", match, p);
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to negotiate SDP\n");
		goto fail;
	}

	/* Parse SDP for trickle ICE */
	if (out_sdp_session) {
		sdp_parser_t *parser = sdp_parse(NULL, r_sdp, strlen(r_sdp), 0);
		if (parser) {
			sdp_session_t *parsed_sdp = sdp_session(parser);
			if (parsed_sdp) {
				*out_sdp_session = (void*)parsed_sdp;
				if (out_parser) {
					*out_parser = parser;
				}
			} else {
				*out_sdp_session = NULL;
				sdp_parser_free(parser);
				if (out_parser) {
					*out_parser = NULL;
				}
			}
		} else {
			*out_sdp_session = NULL;
			if (out_parser) {
				*out_parser = NULL;
			}
		}
	}

	if (switch_core_media_choose_ports(session, SWITCH_TRUE, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to choose ports\n");
		goto fail;
	}
	if (switch_core_media_activate_rtp(session) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to activate RTP\n");
		goto fail;
	}

	*out_session = session;
	*out_rtp = switch_core_media_get_rtp_session(session, SWITCH_MEDIA_TYPE_AUDIO);
	if (!*out_rtp) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to get RTP session\n");
		goto fail;
	}

	return SWITCH_STATUS_SUCCESS;

fail:
	switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
	switch_core_session_rwunlock(session);
	return SWITCH_STATUS_FALSE;
}

static volatile int trickle_ev_seen = 0;
static char last_cand_line[512];

static void trickle_event_handler(switch_event_t *ev)
{
	const char *sub = switch_event_get_header(ev, "Event-Subclass");
	if (sub && !strcmp(sub, "sofia::trickle-ice")) {
		const char *cand = switch_event_get_header(ev, "a-candidate");
		if (cand) switch_snprintf(last_cand_line, sizeof(last_cand_line), "%s", cand);
		trickle_ev_seen = 1;
	}
}

FCT_BGN()
{
	FCT_FIXTURE_SUITE_BGN(switch_trickle_ice)
	{
		switch_memory_pool_t *pool = NULL;
		static switch_memory_pool_t *telnyx_pool = NULL;

		FCT_SETUP_BGN()
		{
			const char *confdir = "conf_trickle"; /* tests/unit/conf_trickle */
			_silence_unused();

			fct_req(switch_core_new_memory_pool(&telnyx_pool) == SWITCH_STATUS_SUCCESS);
			switch_telnyx_init(telnyx_pool);
			fst_init_core_and_modload(confdir, confdir, 0, 0 /* flags */);
			fct_req(switch_core_new_memory_pool(&pool) == SWITCH_STATUS_SUCCESS);
			fct_req(pool != NULL);
			do {
				int sps_total = 10000;
				switch_core_session_ctl(SCSC_SPS, &sps_total);
				switch_sleep(1000000); /* allow softtimer_runtime to apply SPS */
			} while(0);
		}
		FCT_SETUP_END();

		FCT_TEARDOWN_BGN()
		{
			if (pool) { switch_core_destroy_memory_pool(&pool); pool = NULL; }
			switch_telnyx_deinit();
			if (telnyx_pool) { switch_core_destroy_memory_pool(&telnyx_pool); telnyx_pool = NULL; }
		}
		FCT_TEARDOWN_END();

		FCT_TEST_BGN(restart_ice_rearms_unchanged_media)
		{
			switch_core_session_t *session = NULL;
			switch_channel_t *channel = NULL;
			switch_rtp_t *rtp = NULL;
			switch_status_t status;
			const char *err = NULL;
			char ice_user[256] = "";
			switch_bool_t has_addr = SWITCH_FALSE;
			ice_t old_ice;
			uint8_t match;
			uint8_t proceed = 0;
			const char *restart_sdp =
				"v=0\n"
				"o=- 1683118194 1683118196 IN IP4 0.0.0.0\n"
				"s=-\n"
				"t=0 0\n"
				"a=group:BUNDLE 0\n"
				"m=audio 9 UDP/TLS/RTP/SAVPF 0\n"
				"c=IN IP4 0.0.0.0\n"
				"a=ice-ufrag:restartRemoteUfrag\n"
				"a=ice-pwd:restartRemotePassword123456\n"
				"a=ice-options:trickle\n"
				"a=rtcp-mux\n"
				"a=setup:active\n"
				"a=rtpmap:0 PCMU/8000\n"
				"a=sendrecv\n"
				"a=fingerprint:sha-256 17:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=mid:0\n";

			memset(&old_ice, 0, sizeof(old_ice));
			old_ice.cands[0][IPR_RTP].priority = 1;
			old_ice.cands[0][IPR_RTP].con_addr = "0.0.0.0";
			old_ice.cands[0][IPR_RTP].con_port = 9;
			old_ice.cands[0][IPR_RTP].ready = 1;

			status = make_session_and_rtp_with_sdp(&session, &rtp, NULL, NULL);
			fst_requires(status == SWITCH_STATUS_SUCCESS && session && rtp);
			channel = switch_core_session_get_channel(session);
			fst_requires(channel != NULL);

			status = switch_rtp_set_remote_address(rtp, "0.0.0.0", 9, 0, SWITCH_FALSE, &err);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			status = switch_rtp_activate_ice(rtp, "oldRemoteUfrag", "oldLocalUfrag",
				"oldLocalPassword", "oldRemotePassword", IPR_RTP, ICE_VANILLA, &old_ice);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			status = switch_rtp_pvt_get_ice_state(rtp, IPR_RTP, ice_user, sizeof(ice_user), &has_addr);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(ice_user, "oldRemoteUfrag:oldLocalUfrag");
			fst_check(has_addr == SWITCH_TRUE);

			switch_core_media_clear_ice(session);
			switch_channel_set_flag(channel, CF_REINVITE);
			switch_channel_set_variable(channel, "rtp_ice_prflx_bootstrap", "true");
			match = switch_core_media_negotiate_sdp(session, restart_sdp, &proceed, SDP_OFFER);
			fst_requires(match != 0);
			status = switch_core_media_activate_rtp(session);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			memset(ice_user, 0, sizeof(ice_user));
			has_addr = SWITCH_TRUE;
			status = switch_rtp_pvt_get_ice_state(rtp, IPR_RTP, ice_user, sizeof(ice_user), &has_addr);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check(strstr(ice_user, "restartRemoteUfrag") != NULL);
			fst_check(has_addr == SWITCH_FALSE);

			cleanup_session_and_media(session);
		}
		FCT_TEST_END();

		/* 1) Registration toggles + candidate emit */
		FCT_TEST_BGN(registration_and_emit_basic)
		{
			switch_core_session_t *session = NULL;
			switch_rtp_t *rtp = NULL; trickle_captured_t cap; switch_rtp_ice_cand_t c;
			void *sdp_session = NULL;
			sdp_parser_t *parser = NULL;

			memset(&cap, 0, sizeof(cap));
			fct_req(make_session_and_rtp_with_sdp(&session, &rtp, &sdp_session, &parser) == SWITCH_STATUS_SUCCESS && rtp != NULL);

			/* make_session_and_rtp_with_sdp() auto-registers a callback because rtp_trickle_ice=true */
			fct_chk(switch_rtp_trickle_is_registered(rtp) == SWITCH_TRUE);

			/* unregister the auto-registered callback */
			switch_rtp_set_ice_candidate_cb(rtp, NULL, NULL);
			fct_chk(switch_rtp_trickle_is_registered(rtp) == SWITCH_FALSE);

			/* Now register our test callback */

			switch_rtp_set_ice_candidate_cb(rtp, on_local_candidate_cb, &cap);
			fct_chk(switch_rtp_trickle_is_registered(rtp) == SWITCH_TRUE);

			memset(&c, 0, sizeof(c));
			c.component_id = 1; switch_snprintf(c.ip, sizeof(c.ip), "%s", "192.168.2.1");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp"); c.port = 40000; c.priority = 100u;
			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 0);
			fct_chk(cap.called == 1 && strcmp(cap.last_mid, "audio") == 0 && cap.last_mline == 0 && cap.last_eoc == 0);
			fct_chk(cap.last_cand.component_id == 1 && strcmp(cap.last_cand.ip, "192.168.2.1") == 0);
			fct_chk(cap.last_cand.port == 40000 && cap.last_cand.priority == 100u);

			/* unregister by setting cb = NULL */
			switch_rtp_set_ice_candidate_cb(rtp, NULL, NULL);
			fct_chk(switch_rtp_trickle_is_registered(rtp) == SWITCH_FALSE);
			cleanup_session_media_and_sdp(session, sdp_session, parser);
		}
		FCT_TEST_END();

		/* 2) Overwrite callback: last registration wins */
		FCT_TEST_BGN(overwrite_callback_last_wins)
		{
			switch_rtp_t *rtp = NULL; trickle_captured_t cap; switch_rtp_ice_cand_t c; const char *err = NULL;
			memset(&cap, 0, sizeof(cap));
			fct_req(make_real_rtp(pool, &rtp, &err) == SWITCH_STATUS_SUCCESS && rtp != NULL);

			switch_rtp_set_ice_candidate_cb(rtp, on_local_candidate_cb, &cap);
			switch_rtp_set_ice_candidate_cb(rtp, on_local_candidate_cb_2, &cap); /* overwrite */

			memset(&c, 0, sizeof(c));
			c.component_id = 1; switch_snprintf(c.ip, sizeof(c.ip), "%s", "192.168.100.5");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp"); c.port = 50000; c.priority = 200u;
			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 1, 0);

			/* cb2 transforms values in a distinctive way (see implementation) */
			fct_chk(cap.called == 10);                /* +10 from cb2 */
			fct_chk(strncmp(cap.last_mid, "cb2:", 4) == 0);
			fct_chk(cap.last_mline == 101);          /* mline+100 */
			fct_chk(cap.last_eoc == 98);             /* not EOC => 98 */
			fct_chk(cap.last_cand.component_id == 101);
			fct_chk(strcmp(cap.last_cand.ip, "cb2-192.168.100.5") == 0);
			fct_chk(cap.last_cand.port == 50100);
			fct_chk(cap.last_cand.priority == 300u);
			cleanup_rtp(&rtp);
		}
		FCT_TEST_END();

		/* 3) Component 2 (RTCP) candidate */
		FCT_TEST_BGN(component_2_rtcp_candidate)
		{
			switch_rtp_t *rtp = NULL; trickle_captured_t cap; switch_rtp_ice_cand_t c; const char *err = NULL;
			memset(&cap, 0, sizeof(cap));
			fct_req(make_real_rtp(pool, &rtp, &err) == SWITCH_STATUS_SUCCESS && rtp != NULL);
			switch_rtp_set_ice_candidate_cb(rtp, on_local_candidate_cb, &cap);

			memset(&c, 0, sizeof(c));
			c.component_id = 2; switch_snprintf(c.ip, sizeof(c.ip), "%s", "192.168.113.9");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp"); c.port = 40002; c.priority = 999u;
			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 0);

			fct_chk(cap.called == 1);
			fct_chk(cap.last_cand.component_id == 2);
			fct_chk(strcmp(cap.last_cand.ip, "192.168.113.9") == 0);
			cleanup_rtp(&rtp);
		}
		FCT_TEST_END();

		/* 4) End-of-candidates only (NULL cand pointer) */
		FCT_TEST_BGN(end_of_candidates_only)
		{
			switch_rtp_t *rtp = NULL; trickle_captured_t cap; const char *err = NULL;
			memset(&cap, 0, sizeof(cap));
			fct_req(make_real_rtp(pool, &rtp, &err) == SWITCH_STATUS_SUCCESS && rtp != NULL);
			switch_rtp_set_ice_candidate_cb(rtp, on_local_candidate_cb, &cap);

			switch_rtp_trickle_emit_local_candidate(rtp, NULL, "video", 1, 1);
			fct_chk(cap.called == 1);
			fct_chk(strcmp(cap.last_mid, "video") == 0);
			fct_chk(cap.last_mline == 1);
			fct_chk(cap.last_eoc == 1);
			/* last_cand is zeroed */
			fct_chk(cap.last_cand.component_id == 0 && cap.last_cand.port == 0);
			cleanup_rtp(&rtp);
		}
		FCT_TEST_END();

		/* 5) Mixed EOC and candidates */
		FCT_TEST_BGN(mixed_emit_and_eoc_sequence)
		{
			switch_rtp_t *rtp = NULL; trickle_captured_t cap; switch_rtp_ice_cand_t c; const char *err = NULL;
			memset(&cap, 0, sizeof(cap));
			fct_req(make_real_rtp(pool, &rtp, &err) == SWITCH_STATUS_SUCCESS && rtp != NULL);
			switch_rtp_set_ice_candidate_cb(rtp, on_local_candidate_cb, &cap);

			memset(&c, 0, sizeof(c));
			c.component_id = 1; switch_snprintf(c.ip, sizeof(c.ip), "%s", "10.0.0.10");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp"); c.port = 10000; c.priority = 1u;

			switch_rtp_trickle_emit_local_candidate(rtp, NULL, "audio", 0, 1);
			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 0);

			fst_check(cap.called == 1);
			fst_check(cap.last_eoc == 1);
			fst_check(cap.last_cand.component_id == 0 && cap.last_cand.port == 0);
			
			fst_check(cap.last_cand.ip[0] == '\0');
			cleanup_rtp(&rtp);
		}
		FCT_TEST_END();

		/* 6) Different mids / mline indexes in one session */
		FCT_TEST_BGN(mids_and_mlines_variations)
		{
			switch_rtp_t *rtp = NULL; trickle_captured_t cap; switch_rtp_ice_cand_t c; const char *err = NULL;
			memset(&cap, 0, sizeof(cap));
			fct_req(make_real_rtp(pool, &rtp, &err) == SWITCH_STATUS_SUCCESS && rtp != NULL);
			switch_rtp_set_ice_candidate_cb(rtp, on_local_candidate_cb, &cap);

			memset(&c, 0, sizeof(c));
			c.component_id = 1; switch_snprintf(c.ip, sizeof(c.ip), "%s", "10.0.0.11");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp"); c.port = 10001; c.priority = 2u;

			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 0);
			fct_chk(cap.called == 1 && strcmp(cap.last_mid, "audio") == 0 && cap.last_mline == 0);

			switch_rtp_trickle_emit_local_candidate(rtp, &c, "video", 1, 0);
			fct_chk(cap.called == 2 && strcmp(cap.last_mid, "video") == 0 && cap.last_mline == 1);
			cleanup_rtp(&rtp);
		}
		FCT_TEST_END();

		/* 7) NULL mid tolerated (becomes "(null)" in our handler) */
		FCT_TEST_BGN(null_mid_tolerated)
		{
			switch_rtp_t *rtp = NULL; trickle_captured_t cap; switch_rtp_ice_cand_t c; const char *err = NULL;
			memset(&cap, 0, sizeof(cap));
			fct_req(make_real_rtp(pool, &rtp, &err) == SWITCH_STATUS_SUCCESS && rtp != NULL);
			switch_rtp_set_ice_candidate_cb(rtp, on_local_candidate_cb, &cap);

			memset(&c, 0, sizeof(c));
			c.component_id = 1; switch_snprintf(c.ip, sizeof(c.ip), "%s", "10.0.0.12");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp"); c.port = 10002; c.priority = 3u;

			switch_rtp_trickle_emit_local_candidate(rtp, &c, NULL, 2, 0);
			fct_chk(cap.called == 1);
			fct_chk(strcmp(cap.last_mid, "(null)") == 0);
			fct_chk(cap.last_mline == 2);
			cleanup_rtp(&rtp);
		}
		FCT_TEST_END();

		/* 8) Burst emission ordering & final state */
		FCT_TEST_BGN(burst_emission_ordering)
		{
			switch_rtp_t *rtp = NULL; trickle_captured_t cap; switch_rtp_ice_cand_t c; const char *err = NULL;
			int i;
			memset(&cap, 0, sizeof(cap));
			fct_req(make_real_rtp(pool, &rtp, &err) == SWITCH_STATUS_SUCCESS && rtp != NULL);
			switch_rtp_set_ice_candidate_cb(rtp, on_local_candidate_cb, &cap);

			memset(&c, 0, sizeof(c));
			c.component_id = 1; switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp"); c.priority = 777u;

			for (i = 0; i < 50; ++i) {
				switch_snprintf(c.ip, sizeof(c.ip), "198.168.0.%d", (i % 250) + 1);
				c.port = 20000 + i;
				switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 0);
			}
			fct_chk(cap.called == 50);
			fct_chk(strcmp(cap.last_cand.transport, "udp") == 0);
			fct_chk(cap.last_cand.port == 20000 + 49);
			cleanup_rtp(&rtp);
		}
		FCT_TEST_END();
		FCT_TEST_BGN(trickle_emit_fires_custom_event)
		{
			switch_core_session_t *session = NULL;
			switch_status_t st;
			switch_rtp_t *rtp;
			switch_rtp_ice_cand_t c;
			void *sdp_session = NULL;
			sdp_parser_t *parser = NULL;

			/* bind listener */
			switch_event_bind("trickle-ut", SWITCH_EVENT_CUSTOM, "sofia::trickle-ice", trickle_event_handler, NULL);

			st = make_session_and_rtp_with_sdp(&session, &rtp, &sdp_session, &parser);
			fst_requires(st == SWITCH_STATUS_SUCCESS && session && rtp);

			/* register trickle on audio */
			switch_core_media_trickle_register(session, "audio", 0);

			/* get the audio RTP and emit a fake local candidate */
			rtp = switch_core_media_get_rtp_session(session, SWITCH_MEDIA_TYPE_AUDIO);
			fct_req(rtp != NULL);

			memset(&c, 0, sizeof(c));
			switch_snprintf(c.foundation, sizeof(c.foundation), "%s", "1");
			c.component_id = 1;
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp");
			c.priority = 2122260223u;
			switch_snprintf(c.ip, sizeof(c.ip), "%s", "203.0.113.7");
			c.port = 40002;
			switch_snprintf(c.cand_type, sizeof(c.cand_type), "%s", "host");

			trickle_ev_seen = 0;
			memset(last_cand_line, 0, sizeof(last_cand_line));

			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 0);

			/* give the event loop a tick */
			switch_yield(100000);

			fct_chk(trickle_ev_seen == 1);
			fct_chk(strstr(last_cand_line, "a=candidate:1 1 udp") != NULL);

			switch_event_unbind_callback(trickle_event_handler);
			cleanup_session_media_and_sdp(session, sdp_session, parser);
		}
		FCT_TEST_END();
		FCT_TEST_BGN(unregister_stops_emission)
		{
			switch_rtp_t *rtp = NULL; trickle_captured_t cap; switch_rtp_ice_cand_t c; const char *err = NULL;
			memset(&cap, 0, sizeof(cap));
			fct_req(make_real_rtp(pool, &rtp, &err) == SWITCH_STATUS_SUCCESS && rtp != NULL);

			switch_rtp_set_ice_candidate_cb(rtp, on_local_candidate_cb, &cap);
			memset(&c, 0, sizeof(c));
			c.component_id = 1;
			switch_snprintf(c.ip, sizeof(c.ip), "%s", "10.10.10.10");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp");
			c.port = 30000; c.priority = 1234u;
			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 0);
			fct_chk(cap.called == 1);

			switch_rtp_set_ice_candidate_cb(rtp, NULL, NULL);
			fct_chk(switch_rtp_trickle_is_registered(rtp) == SWITCH_FALSE);

			memset(&c, 0, sizeof(c));
			c.component_id = 2;
			switch_snprintf(c.ip, sizeof(c.ip), "%s", "10.10.10.11");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp");
			c.port = 30002; c.priority = 5678u;
			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 0);

			fct_chk(cap.called == 1);

			cleanup_rtp(&rtp);
		}
		FCT_TEST_END();
		FCT_TEST_BGN(emit_local_candidates_EOC)
		{
			switch_core_session_t *session = NULL;
			switch_rtp_t *rtp = NULL;
			trickle_captured_t cap;
			switch_status_t st;
			switch_rtp_ice_cand_t c;
			void *sdp_session = NULL;
			sdp_parser_t *parser = NULL;

			memset(&cap, 0, sizeof(cap));
			memset(&c, 0, sizeof(c));

			st = make_session_and_rtp_with_sdp(&session, &rtp, &sdp_session, &parser);
			fst_requires(st == SWITCH_STATUS_SUCCESS && session && rtp);

			switch_rtp_set_ice_candidate_cb(rtp, on_local_candidate_cb, &cap);
			fst_check(switch_rtp_trickle_is_registered(rtp) == SWITCH_TRUE);

			c.component_id = 1;
			switch_snprintf(c.ip, sizeof(c.ip), "%s", "10.10.10.10");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp");
			c.port = 40000;
			c.priority = 100u;
			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 0);
			fst_check(cap.called == 1);

			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 1);
			fst_check(cap.called == 2);

			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 0);
			fst_xcheck(cap.called == 2, "EOC seal should prevent further emissions");

			switch_rtp_trickle_emit_local_candidate(rtp, NULL, "audio", 0, 1);
			fst_xcheck(cap.called == 2, "NULL-candidate EOC should be ignored after seal");

			switch_rtp_set_ice_candidate_cb(rtp, NULL, NULL);
			cleanup_session_media_and_sdp(session, sdp_session, parser);
		}
		FCT_TEST_END();
		FCT_TEST_BGN(emit_local_candidate_after_destroy)
		{
			switch_rtp_t *rtp1 = NULL, *rtp2 = NULL;
			trickle_captured_t cap1, cap2;
			switch_status_t st;
			switch_rtp_ice_cand_t c;

			memset(&cap1, 0, sizeof(cap1));
			memset(&cap2, 0, sizeof(cap2));
			memset(&c, 0, sizeof(c));

			st = make_real_rtp(pool, &rtp1, NULL);
			fst_requires(st == SWITCH_STATUS_SUCCESS && rtp1);

			switch_rtp_set_ice_candidate_cb(rtp1, on_local_candidate_cb, &cap1);
			fst_check(switch_rtp_trickle_is_registered(rtp1) == SWITCH_TRUE);

			c.component_id = 1;
			switch_snprintf(c.ip, sizeof(c.ip), "%s", "192.168.2.1");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp");
			c.port = 45000;
			c.priority = 200u;
			switch_rtp_trickle_emit_local_candidate(rtp1, &c, "audio", 0, 1);
			fst_check(cap1.called == 1);

			switch_rtp_set_ice_candidate_cb(rtp1, NULL, NULL);
			switch_rtp_destroy(&rtp1);
			rtp1 = NULL;

			st = make_real_rtp(pool, &rtp2, NULL);
			fst_requires(st == SWITCH_STATUS_SUCCESS && rtp2);

			switch_rtp_set_ice_candidate_cb(rtp2, on_local_candidate_cb, &cap2);
			fst_check(switch_rtp_trickle_is_registered(rtp2) == SWITCH_TRUE);

			memset(&c, 0, sizeof(c));
			c.component_id = 1;
			switch_snprintf(c.ip, sizeof(c.ip), "%s", "172.16.0.10");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp");
			c.port = 46000;
			c.priority = 300u;
			switch_rtp_trickle_emit_local_candidate(rtp2, &c, "audio", 0, 0);
			fst_xcheck(cap2.called == 1, "Fresh session must be able to emit new candidates");

			switch_rtp_set_ice_candidate_cb(rtp2, NULL, NULL);
			switch_rtp_destroy(&rtp2);
		}
		FCT_TEST_END();

	/* Test ACL filtering: private IP rejected, public IP chosen */
	FCT_TEST_BGN(test_acl_filtering_chooses_public_ip)
	{
		switch_core_session_t *session = NULL;
		switch_rtp_t *rtp = NULL;
		switch_status_t st;
		switch_media_handle_t *smh = NULL;
		void *sdp_session = NULL;
		sdp_parser_t *parser = NULL;
		/* Create a real session to simulate production environment */
		st = make_session_and_rtp_with_sdp(&session, &rtp, &sdp_session, &parser);
		fst_requires(st == SWITCH_STATUS_SUCCESS);
		fst_requires(session != NULL);
		fst_requires(rtp != NULL);

		/* Get the media handle from the session */
		smh = switch_core_session_get_media_handle(session);
		fst_requires(smh != NULL);

		/* Test the full production trickle ICE workflow */
		/* This should replicate the exact sequence from the production logs */

		/* First candidate: host candidate (private IP - should be rejected by ACL) */
		st = switch_core_media_trickle_remote_candidate_and_recheck(
			session, smh, sdp_session, SDP_TYPE_REQUEST,
			"0", 0,
			"candidate:2913552865 1 udp 24977407 192.168.0.1 49412 typ host raddr 162.120.214.184 rport 55197",
			0
		);
		fst_check(st == SWITCH_STATUS_SUCCESS);

		/* Second candidate: IPv6 srflx candidate (should be dropped - no network path) */
		st = switch_core_media_trickle_remote_candidate_and_recheck(
			session, smh, sdp_session, SDP_TYPE_REQUEST,
			"0", 0,
			"candidate:265031753 1 udp 1685921537 fd7a:115c:a1e0:ab12:4843:cd96:625d:273b 18215 typ srflx raddr 100.69.211.204 rport 54081",
			0
		);
		fst_check(st == SWITCH_STATUS_SUCCESS);

		/* Third candidate: .local hostname candidate (should be dropped - not an IP) */
		st = switch_core_media_trickle_remote_candidate_and_recheck(
			session, smh, sdp_session, SDP_TYPE_REQUEST,
			"0", 0,
			"candidate:265031753 1 udp 1685921535 490f301c-75b1-45ea-b4ef-259ba8aade9b.local 18215 typ host raddr 100.69.211.204 rport 54081",
			0
		);
		fst_check(st == SWITCH_STATUS_SUCCESS);

		/* Fourth candidate: public IP srflx candidate (THIS SHOULD BE CHOSEN!) */
		st = switch_core_media_trickle_remote_candidate_and_recheck(
			session, smh, sdp_session, SDP_TYPE_REQUEST,
			"0", 0,
			"candidate:265031753 1 udp 1685921533 50.114.144.39 18215 typ srflx raddr 100.69.211.204 rport 54081",
			0
		);
		fst_check(st == SWITCH_STATUS_SUCCESS);

		/* Test end-of-candidates marker */
		st = switch_core_media_trickle_remote_candidate_and_recheck(
			session, smh, sdp_session, SDP_TYPE_REQUEST,
			"0", 0,
			NULL,
			1  /* end_of_candidates */
		);
		fst_check(st == SWITCH_STATUS_SUCCESS);

		/* Verify the correct candidate was chosen using the new helper function */
		{
			char *chosen_addr = NULL;
			switch_port_t chosen_port = 0;

			st = switch_core_media_get_chosen_ice_candidate(session, SWITCH_MEDIA_TYPE_AUDIO, &chosen_addr, &chosen_port);
			fst_check(st == SWITCH_STATUS_SUCCESS);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
				"Chosen ICE candidate: %s:%d\n",
				chosen_addr ? chosen_addr : "(null)", chosen_port);

			/* Verify the correct candidate was chosen:
			 * - NOT the private IP 192.168.0.1:49412
			 * - SHOULD BE the public IP 50.114.144.39:18215 */
			fst_check_string_equals(chosen_addr, "50.114.144.39");
			fst_check(chosen_port == 18215);
		}

		switch_sleep(1000 * 1000);
		cleanup_session_media_and_sdp(session, sdp_session, parser);
	}
	FCT_TEST_END();
	
	}

	FCT_FIXTURE_SUITE_END();
}
FCT_END()
