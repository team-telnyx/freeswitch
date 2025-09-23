#include <switch.h>
#include <switch_rtp.h>
#include <test/switch_test.h>
#include "switch_telnyx.h"

#define USE_SWITCH_RTP_NEW_IPPORT 1

extern char *fst_getenv_default(const char *, char *, switch_bool_t);
static void _silence_unused(void) { (void)fst_getenv_default; }

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

static switch_rtp_t *wait_for_rtp_session(switch_core_session_t *session, switch_media_type_t type, uint32_t timeout_ms);
 
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


static switch_status_t make_session_and_rtp(switch_core_session_t **out_session,
                                            switch_rtp_t **out_rtp)
{
	switch_status_t st;
	switch_call_cause_t cause = SWITCH_CAUSE_NONE, cancel = SWITCH_CAUSE_NONE;
	switch_core_session_t *session = NULL;
	switch_rtp_t *rtp = NULL;

	 const char *br =        "{"
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
			10,                     /* timeout (sec) */
			NULL, NULL, NULL, NULL,/* table, cid_name, cid_num, outbound_profile_uuid */
			NULL,                  /* ovars (NULL, since we inlined) */
			SOF_NONE,              /* flags */
			&cancel,               /* out: cancel cause */
			NULL);                 /* dial handle */

	if (st != SWITCH_STATUS_SUCCESS || !session) return SWITCH_STATUS_FALSE;

	{
		switch_channel_t *chan = switch_core_session_get_channel(session);
		switch_channel_wait_for_state(chan, NULL, CS_CONSUME_MEDIA);
		switch_yield(100000);
	}

	if (switch_core_media_prepare_codecs(session, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS) goto fail;
	if (switch_core_media_choose_ports(session, SWITCH_TRUE, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS) goto fail;
	if (switch_core_media_activate_rtp(session) != SWITCH_STATUS_SUCCESS) goto fail;
	
	{
		switch_channel_t *channel = switch_core_session_get_channel(session);
		if (!channel) goto fail;

		{
			switch_time_t waited = 0;
			while (switch_channel_get_state(channel) != CS_EXECUTE &&
				switch_channel_get_state(channel) != CS_CONSUME_MEDIA &&
				waited < 3000000) {
				switch_yield(10000);
				waited += 10000;
			}
		}

		{
			switch_time_t waited = 0;
			while (!switch_channel_media_ready(channel) && waited < 3000000) {
				switch_yield(10000);
				waited += 10000;
			}
			if (!switch_channel_media_ready(channel)) goto fail;
		}
	}

	{
		switch_media_handle_t *smh = switch_core_session_get_media_handle(session);
		
		if (smh) {
			switch_core_media_prepare_codecs(session, SWITCH_FALSE);
			switch_core_media_choose_ports(session, SWITCH_TRUE, SWITCH_FALSE);
			switch_core_media_activate_rtp(session);
			rtp = wait_for_rtp_session(session, SWITCH_MEDIA_TYPE_AUDIO, 3000 /* ms */);
			if (!rtp) {
				switch_yield(100000);
				rtp = switch_core_media_get_rtp_session(session, SWITCH_MEDIA_TYPE_AUDIO);
				if (!rtp) goto fail;
			}
			rtp = switch_core_media_get_rtp_session(session, SWITCH_MEDIA_TYPE_AUDIO);
		}
	}
   
	if (!rtp) goto fail;
	
	*out_session = session;
	*out_rtp = switch_core_media_get_rtp_session(session, SWITCH_MEDIA_TYPE_AUDIO);
	if (!*out_rtp) {
		switch_channel_t *ch = switch_core_session_get_channel(session);
		const char *peer_uuid = ch ? switch_channel_get_partner_uuid(ch) : NULL;
		if (peer_uuid && *peer_uuid) {
			switch_core_session_t *peer = switch_core_session_locate(peer_uuid);
			if (peer) {
				if (switch_core_media_prepare_codecs(peer, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS &&
					switch_core_media_choose_ports(peer, SWITCH_TRUE, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS &&
					switch_core_media_activate_rtp(peer) == SWITCH_STATUS_SUCCESS) {
					*out_rtp = switch_core_media_get_rtp_session(peer, SWITCH_MEDIA_TYPE_AUDIO);
				}
				switch_core_session_rwunlock(peer);
			}
		}
		if (!*out_rtp) goto fail;
	}

	return SWITCH_STATUS_SUCCESS;

fail:
	switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
	switch_core_session_rwunlock(session);
	return SWITCH_STATUS_FALSE;
}

static switch_rtp_t *wait_for_rtp_session(switch_core_session_t *session, switch_media_type_t type, uint32_t timeout_ms)
{
    uint32_t waited = 0;
    switch_rtp_t *rtp = NULL;
	while (waited < timeout_ms) {
        rtp = switch_core_media_get_rtp_session(session, type);
        if (rtp) return rtp;
        switch_yield(10000);
        waited += 10000;
    }
    return NULL;
}

static void cleanup_session_and_media(switch_core_session_t *session)
{
	if (!session) return;
	switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
	switch_core_session_rwunlock(session);
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

		/* 1) Registration toggles + candidate emit */
		FCT_TEST_BGN(registration_and_emit_basic)
		{
			switch_core_session_t *session = NULL;
			switch_rtp_t *rtp = NULL; trickle_captured_t cap; switch_rtp_ice_cand_t c;

			memset(&cap, 0, sizeof(cap));
			fct_req(make_session_and_rtp(&session, &rtp) == SWITCH_STATUS_SUCCESS && rtp != NULL);
 

			fct_chk(switch_rtp_trickle_is_registered(rtp) == SWITCH_FALSE);

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
			cleanup_session_and_media(session);
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
		}
		FCT_TEST_END();
		FCT_TEST_BGN(trickle_emit_fires_custom_event)
		{
			switch_core_session_t *session = NULL;
			switch_status_t st;
			switch_rtp_t *rtp;
			switch_rtp_ice_cand_t c;

			/* bind listener */
			switch_event_bind("trickle-ut", SWITCH_EVENT_CUSTOM, "sofia::trickle-ice", trickle_event_handler, NULL);

			st = make_session_and_rtp(&session, &rtp);
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
			switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session);
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
			c.component_id = 2;+			switch_snprintf(c.ip, sizeof(c.ip), "%s", "10.10.10.11");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp");
			c.port = 30002; c.priority = 5678u;
			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 0);

			fct_chk(cap.called == 1);
		}
		FCT_TEST_END();
		FCT_TEST_BGN(emit_local_candidates_EOC)
		{
			switch_core_session_t *session = NULL;
			switch_rtp_t *rtp = NULL;
			trickle_captured_t cap;
			switch_status_t st;
			switch_rtp_ice_cand_t c;

			memset(&cap, 0, sizeof(cap));
			memset(&c, 0, sizeof(c));

			st = make_session_and_rtp(&session, &rtp);
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
			cleanup_session_and_media(session);
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
	}

	FCT_FIXTURE_SUITE_END();
}
FCT_END()
