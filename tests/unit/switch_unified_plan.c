#include <switch.h>
#include <switch_rtp.h>
#include <test/switch_test.h>
#include "switch_telnyx.h"


#define TEST_HAVE_HDREXT_MAP

#define USE_SWITCH_RTP_NEW_IPPORT 1

#define TEST_AUDIO_PT       0      /* PCMU */
#define TEST_PTIME_MS       20     /* 20ms */
#define TEST_SRATE          8000   /* 8kHz */
#define TEST_MID_EXT_ID     10     /* extmap id for urn:ietf:params:rtp-hdrext:sdes:mid */

#ifndef STRINGIZE
#  define STRINGIZE2(x) #x
#  define STRINGIZE(x) STRINGIZE2(x)
#endif

static void silence_fst_unused(void) {
	(void)fst_getenv_default("__never_set__", NULL, SWITCH_FALSE);
}

static const char *SAMPLE_SDP_BUNDLE =
	"v=0\n"
	"o=- 12345 2 IN IP4 127.0.0.1\n"
	"s=-\n"
	"t=0 0\n"
	"a=group:BUNDLE 0 1\n"
	"a=msid-semantic: WMS stream\n"
	"m=audio 40000 RTP/AVP 0\n"
	"c=IN IP4 127.0.0.1\n"
	"a=rtpmap:0 PCMU/8000\n"
	"a=extmap:" STRINGIZE(TEST_MID_EXT_ID) " urn:ietf:params:rtp-hdrext:sdes:mid\n"
	"a=mid:0\n"
	"m=video 40002 RTP/AVP 96\n"
	"c=IN IP4 127.0.0.1\n"
	"a=rtpmap:96 VP8/90000\n"
	"a=extmap:" STRINGIZE(TEST_MID_EXT_ID) " urn:ietf:params:rtp-hdrext:sdes:mid\n"
	"a=mid:1\n";

static switch_status_t make_real_rtp(switch_memory_pool_t *pool,
                                     switch_rtp_t **out_rtp,
                                     const char **out_err)
{
	switch_rtp_t *rtp = NULL;
	switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID];
	const char *err = NULL;
	uint32_t samples_per_interval = (TEST_SRATE / 1000) * TEST_PTIME_MS; /* 160 */
	switch_status_t st;

	if (!out_rtp) return SWITCH_STATUS_FALSE;
	*out_rtp = NULL;
	if (out_err) *out_err = NULL;

	st = switch_rtp_create(&rtp,
										TEST_AUDIO_PT,
										samples_per_interval,
										TEST_PTIME_MS,
										flags,
										"soft",
										&err,
										pool,
										5);
	if (st != SWITCH_STATUS_SUCCESS) {
		if (out_err) *out_err = err ? err : "switch_rtp_create failed";
		return st;
	}

	*out_rtp = rtp;
	return SWITCH_STATUS_SUCCESS;
}

static void cleanup_rtp(switch_rtp_t **rtp)
{
	if (rtp && *rtp) {
		switch_rtp_destroy(rtp);
	}
}

static switch_status_t make_session_and_rtp(switch_core_session_t **out_session,
                                            switch_rtp_t **out_rtp)
{
	switch_status_t st;
	switch_call_cause_t cause = SWITCH_CAUSE_NONE, cancel = SWITCH_CAUSE_NONE;
	switch_core_session_t *session = NULL;

	const char *br = "{absolute_codec_string=L16@8000h@20i,rtp_disable_crypto=true}loopback/9999";


	 st = switch_ivr_originate(
	 NULL,                  /* a-leg session */
	 &session,              /* out: b-leg */
	 &cause,                /* out: cause */
	 br,                    /* bridgeto */
	 2,                     /* timeout (sec) */
	 NULL, NULL, NULL, NULL,/* table, cid_name, cid_num, outbound_profile_uuid */
	 NULL,                  /* ovars (NULL, since we inlined) */
	 SOF_NONE,              /* flags */
	 &cancel,               /* out: cancel cause */
	 NULL);                 /* dial handle */


	if (st != SWITCH_STATUS_SUCCESS || !session) {
        return SWITCH_STATUS_FALSE;
	}

	if (out_rtp) {
		const char *err = NULL;
		switch_rtp_t *rtp = NULL;
		if (make_real_rtp(switch_core_session_get_pool(session), &rtp, &err) == SWITCH_STATUS_SUCCESS) {
			*out_rtp = rtp;
		} else {
			*out_rtp = NULL;
		}
	}

	if (out_session) *out_session = session;
	return SWITCH_STATUS_SUCCESS;
}

static void cleanup_session_and_media(switch_core_session_t *session)
{
	if (session) {
		switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
	}
}

FCT_BGN()
{
	FCT_FIXTURE_SUITE_BGN(switch_unified_plan)
	{
		switch_memory_pool_t *pool = NULL;
		switch_memory_pool_t *telnyx_pool = NULL;

		FCT_SETUP_BGN()
		{
			const char *confdir = "conf_unified_plan";
			silence_fst_unused();
				fct_req(switch_core_new_memory_pool(&telnyx_pool) == SWITCH_STATUS_SUCCESS);
			switch_telnyx_init(telnyx_pool);
			fst_init_core_and_modload(confdir, confdir, 0, 0);
			fct_req(switch_core_new_memory_pool(&pool) == SWITCH_STATUS_SUCCESS);
			fct_req(pool != NULL);
		}
		FCT_SETUP_END();

		FCT_TEARDOWN_BGN()
		{
			if (pool) { switch_core_destroy_memory_pool(&pool); pool = NULL; }
			switch_telnyx_deinit();
			if (telnyx_pool) { switch_core_destroy_memory_pool(&telnyx_pool); telnyx_pool = NULL; }
		}
		FCT_TEARDOWN_END();

		FCT_TEST_BGN(sdp_bundle_and_mid_presence)
		{
			const char *sdp = SAMPLE_SDP_BUNDLE;
			fct_chk(strstr(sdp, "a=group:BUNDLE") != NULL);
			fct_chk(strstr(sdp, "a=mid:0") != NULL);
			fct_chk(strstr(sdp, "a=mid:1") != NULL);
			fct_chk(strstr(sdp, "urn:ietf:params:rtp-hdrext:sdes:mid") != NULL);
			{
				char needle[32];
				switch_snprintf(needle, sizeof(needle), "a=extmap:%d ", TEST_MID_EXT_ID);
				fct_chk(strstr(sdp, needle) != NULL);
			}
		}
		FCT_TEST_END();
		FCT_TEST_BGN(bundle_grouping_respects_channel_var)
		{
			switch_core_session_t *session = NULL;
			char *s;
			switch_rtp_t *rtp = NULL;

			fct_req(make_session_and_rtp(&session, &rtp) == SWITCH_STATUS_SUCCESS && session != NULL);
			/* Set channel vars for BUNDLE + MIDs */
			switch_channel_set_variable(switch_core_session_get_channel(session), "rtp_use_bundle", "true");
			switch_channel_set_variable(switch_core_session_get_channel(session), "rtp_audio_mid", "0");
			switch_channel_set_variable(switch_core_session_get_channel(session), "rtp_video_mid", "1");


			/* Generate local SDP with core API */
			switch_core_media_gen_local_sdp(session, SDP_OFFER, "127.0.0.1", 40000, NULL, 1);
			s = (char *)switch_channel_get_variable(switch_core_session_get_channel(session), "rtp_local_sdp_str");
			fct_req(s != NULL);
			fct_chk(strstr(s, "a=group:BUNDLE 0 1") != NULL);


			/* Now disable bundle and ensure the line disappears */
			switch_channel_set_variable(switch_core_session_get_channel(session), "rtp_use_bundle", "false");
			switch_core_media_gen_local_sdp(session, SDP_OFFER, "127.0.0.1", 40000, NULL, 1);
			s = (char *)switch_channel_get_variable(switch_core_session_get_channel(session), "rtp_local_sdp_str");
			fct_req(s != NULL);
			fct_chk(strstr(s, "a=group:BUNDLE") == NULL);


			/* Cleanup session */
			switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session);
			}
		FCT_TEST_END();

		FCT_TEST_BGN(session_and_rtp_smoke)
		{
			switch_core_session_t *session = NULL; switch_rtp_t *rtp = NULL;
			fct_req(make_session_and_rtp(&session, &rtp) == SWITCH_STATUS_SUCCESS && session != NULL);
			fct_chk(rtp != NULL);
			cleanup_rtp(&rtp);
			cleanup_session_and_media(session);
		}
		FCT_TEST_END();
	}
	FCT_FIXTURE_SUITE_END()
}
FCT_END()
