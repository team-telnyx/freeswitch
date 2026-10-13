/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2025, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * Tuan Nguyen <tuan@telnyx.com>
 *
 * switch_3pcc.c -- tests 3PCC (Third Party Call Control) scenarios
 *
 */

#include <switch.h>
#include <test/switch_test.h>

/* SDP templates with placeholders for session_id and session_version */
static const char *sdp_audio_template =
	"v=0\r\n"
	"o=FreeSWITCH %u %u IN IP4 192.168.1.100\r\n"
	"s=FreeSWITCH\r\n"
	"c=IN IP4 192.168.1.100\r\n"
	"t=0 0\r\n"
	"m=audio 5004 RTP/AVP 0 8 101\r\n"
	"a=rtpmap:0 PCMU/8000\r\n"
	"a=rtpmap:8 PCMA/8000\r\n"
	"a=rtpmap:101 telephone-event/8000\r\n"
	"a=fmtp:101 0-16\r\n";

static const char *sdp_srtp_template =
	"v=0\r\n"
	"o=FreeSWITCH %u %u IN IP4 192.168.1.100\r\n"
	"s=FreeSWITCH\r\n"
	"c=IN IP4 192.168.1.100\r\n"
	"t=0 0\r\n"
	"m=audio 5004 RTP/SAVP 0 8 101\r\n"
	"a=rtpmap:0 PCMU/8000\r\n"
	"a=rtpmap:8 PCMA/8000\r\n"
	"a=rtpmap:101 telephone-event/8000\r\n"
	"a=fmtp:101 0-16\r\n"
	"a=crypto:1 AES_CM_128_HMAC_SHA1_80 inline:WnD7c1ksDGs+dq5PI6rV/1Pj8+eSRCZQ8JWaGaQA\r\n";

static const char *sdp_srtp_key2_template =
	"v=0\r\n"
	"o=FreeSWITCH %u %u IN IP4 192.168.1.100\r\n"
	"s=FreeSWITCH\r\n"
	"c=IN IP4 192.168.1.100\r\n"
	"t=0 0\r\n"
	"m=audio 5004 RTP/SAVP 0 8 101\r\n"
	"a=rtpmap:0 PCMU/8000\r\n"
	"a=rtpmap:8 PCMA/8000\r\n"
	"a=rtpmap:101 telephone-event/8000\r\n"
	"a=fmtp:101 0-16\r\n"
	"a=crypto:1 AES_CM_128_HMAC_SHA1_80 inline:XoE8d2ltEHt+er6QJ7sW/2Qk9+fTSDaR9KXbHbRB\r\n";

/* Buffers to hold dynamically generated SDP strings */
static char sample_sdp_audio[1024];
static char sample_sdp_srtp[1024];
static char sample_sdp_srtp_key2[1024];

/* Generate dynamic SDP strings with unique session IDs */
static void generate_sdp_samples(void)
{
	uint32_t session_id;
	uint32_t session_version;

	/* Generate session ID */
	session_id = (uint32_t)switch_epoch_time_now(NULL) - (rand() % 64000 + 1);
	session_version = session_id + 1;

	/* Generate audio SDP */
	snprintf(sample_sdp_audio, sizeof(sample_sdp_audio), sdp_audio_template,
		session_id, session_version);

	/* Generate SRTP SDP with same session ID, same version */
	snprintf(sample_sdp_srtp, sizeof(sample_sdp_srtp), sdp_srtp_template,
		session_id, session_version);

	/* Generate SRTP SDP with same session ID, incremented version */
	snprintf(sample_sdp_srtp_key2, sizeof(sample_sdp_srtp_key2), sdp_srtp_key2_template,
		session_id, session_version + 1);
}

/* Helper to simulate receiving INVITE with SDP */
static void simulate_invite_with_sdp(switch_core_session_t *session, const char *sdp)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);

	if (!channel) return;

	switch_channel_set_flag(channel, CF_3PCC);
	switch_channel_set_variable(channel, SWITCH_R_SDP_VARIABLE, sdp);

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
		"Simulated INVITE with SDP\n");
}

/* Helper to simulate receiving INVITE without SDP */
static void simulate_invite_no_sdp(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);

	if (!channel) return;

	switch_channel_set_flag(channel, CF_3PCC);

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
		"Simulated INVITE without SDP (delayed offer)\n");
}

/* Helper to simulate RE-INVITE without SDP */
static void simulate_reinvite_no_sdp(switch_core_session_t *session, int sequence)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	char seq_marker[64];

	if (!channel) return;

	snprintf(seq_marker, sizeof(seq_marker), "reinvite_no_sdp_%d", sequence);
	switch_channel_set_variable(channel, seq_marker, "true");

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
		"Simulated RE-INVITE without SDP (sequence %d)\n", sequence);
}

/* Helper to set local SDP for UAS scenarios */
static void set_local_sdp(switch_core_session_t *session, const char *sdp)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);

	if (!channel) return;

	switch_channel_set_variable(channel, SWITCH_L_SDP_VARIABLE, sdp);
}

/* Helper to validate crypto in SDP */
static switch_bool_t has_crypto(const char *sdp, const char *crypto_key)
{
	char search_str[256];

	if (!sdp) return SWITCH_FALSE;
	if (!strstr(sdp, "RTP/SAVP")) return SWITCH_FALSE;
	if (!strstr(sdp, "a=crypto:")) return SWITCH_FALSE;

	if (crypto_key) {
		snprintf(search_str, sizeof(search_str), "inline:%s", crypto_key);
		return (strstr(sdp, search_str) != NULL) ? SWITCH_TRUE : SWITCH_FALSE;
	}

	return SWITCH_TRUE;
}

/* Count the occurrences of a string in an SDP */
static int count_str(const char *sdp, const char *str)
{
	int count = 0;

	while (sdp && (sdp = strstr(sdp, str))) {
		count++;
		sdp++;
	}

	return count;
}

/* Count the audio m-lines in an SDP */
static int count_audio_mlines(const char *sdp)
{
	return count_str(sdp, "m=audio ");
}

/* Helper to create a PCMU media handle on loopback */
static switch_status_t create_media_handle(switch_core_session_t *session)
{
	switch_core_media_params_t *mparams;
	switch_media_handle_t *media_handle;

	mparams = switch_core_session_alloc(session, sizeof(switch_core_media_params_t));
	mparams->num_codecs = 1;
	mparams->inbound_codec_string = switch_core_session_strdup(session, "PCMU");
	mparams->outbound_codec_string = switch_core_session_strdup(session, "PCMU");
	mparams->rtpip = switch_core_session_strdup(session, "127.0.0.1");

	return switch_media_handle_create(&media_handle, session, mparams);
}

/* Helper to generate a local SDP offer on a fresh media handle */
static const char *gen_offer(switch_core_session_t *session, switch_call_direction_t direction, switch_channel_flag_t flag)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);

	switch_channel_set_direction(channel, direction);

	if (flag) {
		switch_channel_set_flag(channel, flag);
	}

	if (create_media_handle(session) != SWITCH_STATUS_SUCCESS) {
		return NULL;
	}

	switch_channel_set_variable(channel, "absolute_codec_string", "PCMU");

	switch_core_media_choose_port(session, SWITCH_MEDIA_TYPE_AUDIO, 0);
	switch_core_media_prepare_codecs(session, SWITCH_TRUE);
	switch_core_media_gen_local_sdp(session, SDP_OFFER, NULL, 0, NULL, 1);

	return switch_channel_get_variable(channel, "rtp_local_sdp_str");
}

/* Helper to generate the local offer an inbound proxy 3PCC call answers a late-offer INVITE with */
static const char *gen_proxy_3pcc_offer(switch_core_session_t *session, const char *var, const char *val, const char *opt_in)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);

	if (var) {
		switch_channel_set_variable(channel, var, val);
	}

	if (opt_in) {
		switch_channel_set_variable(channel, "rtp_secure_media_3pcc_proxy_offer", opt_in);
	}

	return gen_offer(session, SWITCH_CALL_DIRECTION_INBOUND, CF_3PCC_PROXY);
}

/* Copy the first a=crypto line of an SDP, without the "a=crypto:" prefix and line end */
static switch_bool_t first_crypto(const char *sdp, char *buf, size_t len)
{
	const char *p, *e;

	if (!sdp || !(p = strstr(sdp, "a=crypto:"))) {
		return SWITCH_FALSE;
	}

	p += strlen("a=crypto:");
	e = p + strcspn(p, "\r\n");

	if ((size_t)(e - p) >= len) {
		return SWITCH_FALSE;
	}

	memcpy(buf, p, e - p);
	buf[e - p] = '\0';

	return SWITCH_TRUE;
}

/* Build an SRTP answer to a local offer, reusing its first crypto tag and suite with the peer's own key */
static void build_srtp_answer(const char *offer_crypto, char *buf, size_t len)
{
	char tag_suite[128];
	const char *inline_at = strstr(offer_crypto, " inline:");
	size_t n = inline_at ? (size_t)(inline_at - offer_crypto) : 0;

	if (n >= sizeof(tag_suite)) {
		n = 0;
	}

	memcpy(tag_suite, offer_crypto, n);
	tag_suite[n] = '\0';

	snprintf(buf, len,
		"v=0\r\n"
		"o=peer 1 1 IN IP4 127.0.0.1\r\n"
		"s=peer\r\n"
		"c=IN IP4 127.0.0.1\r\n"
		"t=0 0\r\n"
		"m=audio 5006 RTP/SAVP 0\r\n"
		"a=rtpmap:0 PCMU/8000\r\n"
		"a=crypto:%s inline:WnD7c1ksDGs+dq5PI6rV/1Pj8+eSRCZQ8JWaGaQA\r\n",
		tag_suite);
}

/* True when the SDP offers SRTP only: one audio m-line, RTP/SAVP with crypto */
static switch_bool_t srtp_only(const char *sdp)
{
	return has_crypto(sdp, NULL) && count_audio_mlines(sdp) == 1 && !strstr(sdp, "RTP/AVP ");
}

/* True when the SDP offers plain RTP only: one audio m-line, RTP/AVP without crypto */
static switch_bool_t rtp_only(const char *sdp)
{
	return sdp && !strstr(sdp, "a=crypto:") && !strstr(sdp, "RTP/SAVP") &&
		count_audio_mlines(sdp) == 1 && strstr(sdp, "RTP/AVP ") != NULL;
}

/* Negotiate SRTP on a proxy 3PCC late offer: opt-in offer, SRTP answer in the ACK, RTP activated */
static switch_bool_t negotiate_proxy_srtp(switch_core_session_t *session)
{
	const char *local_sdp;
	char offer_crypto[256];
	char answer[1024];
	uint8_t p = 0;

	local_sdp = gen_proxy_3pcc_offer(session, "rtp_secure_media", "true:AES_CM_128_HMAC_SHA1_80", "true");

	if (!first_crypto(local_sdp, offer_crypto, sizeof(offer_crypto))) {
		return SWITCH_FALSE;
	}

	build_srtp_answer(offer_crypto, answer, sizeof(answer));

	if (!switch_core_media_negotiate_sdp(session, answer, &p, SDP_ANSWER)) {
		return SWITCH_FALSE;
	}

	switch_core_media_choose_port(session, SWITCH_MEDIA_TYPE_AUDIO, 0);

	return switch_core_media_activate_rtp(session) == SWITCH_STATUS_SUCCESS;
}

FST_CORE_BEGIN("./conf")
{
	FST_SUITE_BEGIN(switch_3pcc)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_loopback");
			generate_sdp_samples();
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		/* ========== UAC SCENARIOS ========== */

		/* Test Case 1: UAC - Receive INVITE with SDP, then 2x RE-INVITE without SDP */
		FST_SESSION_BEGIN(uac_invite_with_sdp_reinvite_no_sdp)
		{
			switch_channel_t *channel;
			const char *remote_sdp;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			/* Step 1: Receive INVITE with SDP */
			simulate_invite_with_sdp(fst_session, sample_sdp_audio);

			fst_check(switch_channel_test_flag(channel, CF_3PCC));
			remote_sdp = switch_channel_get_variable(channel, SWITCH_R_SDP_VARIABLE);
			fst_check(remote_sdp != NULL);
			fst_check(strstr(remote_sdp, "m=audio") != NULL);
			fst_check(strstr(remote_sdp, "RTP/AVP") != NULL);

			/* Step 2 & 3: Simulate 2x RE-INVITE without SDP */
			simulate_reinvite_no_sdp(fst_session, 1);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_1") != NULL);

			simulate_reinvite_no_sdp(fst_session, 2);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_2") != NULL);

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO,
				"UAC Test 1 PASS: INVITE with SDP + 2x RE-INVITE without SDP\n");
		}
		FST_SESSION_END()

		/* Test Case 2: UAC - Receive INVITE without SDP, then 2x RE-INVITE without SDP */
		FST_SESSION_BEGIN(uac_invite_no_sdp_reinvite_no_sdp)
		{
			switch_channel_t *channel;
			const char *remote_sdp;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			/* Step 1: Receive INVITE without SDP */
			simulate_invite_no_sdp(fst_session);

			fst_check(switch_channel_test_flag(channel, CF_3PCC));
			remote_sdp = switch_channel_get_variable(channel, SWITCH_R_SDP_VARIABLE);
			fst_check(remote_sdp == NULL);

			/* We would send our SDP in 200 OK */
			set_local_sdp(fst_session, sample_sdp_audio);

			/* Step 2 & 3: Simulate 2x RE-INVITE without SDP */
			simulate_reinvite_no_sdp(fst_session, 1);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_1") != NULL);

			simulate_reinvite_no_sdp(fst_session, 2);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_2") != NULL);

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO,
				"UAC Test 2 PASS: INVITE without SDP + 2x RE-INVITE without SDP\n");
		}
		FST_SESSION_END()

		/* Test Case 3: UAC - Receive INVITE with SDP (SRTP), then 2x RE-INVITE without SDP */
		FST_SESSION_BEGIN(uac_invite_srtp_reinvite_no_sdp_same_key)
		{
			switch_channel_t *channel;
			const char *remote_sdp;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			/* Step 1: Receive INVITE with SDP (SRTP) */
			simulate_invite_with_sdp(fst_session, sample_sdp_srtp);

			fst_check(switch_channel_test_flag(channel, CF_3PCC));
			remote_sdp = switch_channel_get_variable(channel, SWITCH_R_SDP_VARIABLE);
			fst_check(remote_sdp != NULL);
			fst_check(strstr(remote_sdp, "RTP/SAVP") != NULL);
			fst_check(has_crypto(remote_sdp, NULL));

			/* Step 2 & 3: Simulate 2x RE-INVITE without SDP (same crypto key) */
			simulate_reinvite_no_sdp(fst_session, 1);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_1") != NULL);

			simulate_reinvite_no_sdp(fst_session, 2);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_2") != NULL);

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO,
				"UAC Test 3 PASS: INVITE with SRTP + 2x RE-INVITE without SDP (same key)\n");
		}
		FST_SESSION_END()

		/* Test Case 4: UAC - Receive INVITE with SDP (SRTP), crypto key changes */
		FST_SESSION_BEGIN(uac_invite_srtp_reinvite_no_sdp_diff_key)
		{
			switch_channel_t *channel;
			const char *remote_sdp;
			const char *crypto_key1;
			const char *crypto_key2;

			crypto_key1 = "WnD7c1ksDGs+dq5PI6rV/1Pj8+eSRCZQ8JWaGaQA";
			crypto_key2 = "XoE8d2ltEHt+er6QJ7sW/2Qk9+fTSDaR9KXbHbRB";

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			/* Step 1: Receive INVITE with SDP (SRTP) with key 1 */
			simulate_invite_with_sdp(fst_session, sample_sdp_srtp);

			remote_sdp = switch_channel_get_variable(channel, SWITCH_R_SDP_VARIABLE);
			fst_check(has_crypto(remote_sdp, crypto_key1));

			/* Step 2: Simulate crypto key change in new offer */
			switch_channel_set_variable(channel, SWITCH_R_SDP_VARIABLE, sample_sdp_srtp_key2);
			remote_sdp = switch_channel_get_variable(channel, SWITCH_R_SDP_VARIABLE);
			fst_check(has_crypto(remote_sdp, crypto_key2));
			fst_check(!has_crypto(remote_sdp, crypto_key1));

			/* Step 3 & 4: Simulate 2x RE-INVITE without SDP */
			simulate_reinvite_no_sdp(fst_session, 1);
			simulate_reinvite_no_sdp(fst_session, 2);

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO,
				"UAC Test 4 PASS: INVITE with SRTP + crypto key change + 2x RE-INVITE\n");
		}
		FST_SESSION_END()

		/* Test Case 5: UAC - INVITE without SDP, then offer SRTP, same key */
		FST_SESSION_BEGIN(uac_invite_no_sdp_reinvite_no_sdp_same_key)
		{
			switch_channel_t *channel;
			const char *local_sdp;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			/* Step 1: Receive INVITE without SDP */
			simulate_invite_no_sdp(fst_session);

			/* Step 2: We offer SRTP in our answer */
			set_local_sdp(fst_session, sample_sdp_srtp);
			local_sdp = switch_channel_get_variable(channel, SWITCH_L_SDP_VARIABLE);
			fst_check(has_crypto(local_sdp, NULL));

			/* Step 3 & 4: Simulate 2x RE-INVITE without SDP */
			simulate_reinvite_no_sdp(fst_session, 1);
			simulate_reinvite_no_sdp(fst_session, 2);

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO,
				"UAC Test 5 PASS: INVITE without SDP + offer SRTP + 2x RE-INVITE (same key)\n");
		}
		FST_SESSION_END()

		/* Test Case 6: UAC - INVITE without SDP, then offer SRTP, key change */
		FST_SESSION_BEGIN(uac_invite_no_sdp_reinvite_no_sdp_diff_key)
		{
			switch_channel_t *channel;
			const char *local_sdp;
			const char *crypto_key1;
			const char *crypto_key2;

			crypto_key1 = "WnD7c1ksDGs+dq5PI6rV/1Pj8+eSRCZQ8JWaGaQA";
			crypto_key2 = "XoE8d2ltEHt+er6QJ7sW/2Qk9+fTSDaR9KXbHbRB";

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			/* Step 1: Receive INVITE without SDP */
			simulate_invite_no_sdp(fst_session);

			/* Step 2: We offer SRTP with key 1 */
			set_local_sdp(fst_session, sample_sdp_srtp);
			local_sdp = switch_channel_get_variable(channel, SWITCH_L_SDP_VARIABLE);
			fst_check(has_crypto(local_sdp, crypto_key1));

			/* Step 3: Simulate key change */
			set_local_sdp(fst_session, sample_sdp_srtp_key2);
			local_sdp = switch_channel_get_variable(channel, SWITCH_L_SDP_VARIABLE);
			fst_check(has_crypto(local_sdp, crypto_key2));

			/* Step 4 & 5: Simulate 2x RE-INVITE without SDP */
			simulate_reinvite_no_sdp(fst_session, 1);
			simulate_reinvite_no_sdp(fst_session, 2);

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO,
				"UAC Test 6 PASS: INVITE without SDP + offer SRTP + key change + 2x RE-INVITE\n");
		}
		FST_SESSION_END()

		/* ========== UAS SCENARIOS ========== */

		/* Test Case 7: UAS - Send INVITE with SDP, receive 2x RE-INVITE without SDP */
		FST_SESSION_BEGIN(uas_send_invite_with_sdp_receive_reinvite_no_sdp)
		{
			switch_channel_t *channel;
			const char *local_sdp;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			/* Step 1: Send INVITE with SDP (UAS sending initial offer) */
			switch_channel_set_flag(channel, CF_3PCC_PROXY);
			set_local_sdp(fst_session, sample_sdp_audio);

			local_sdp = switch_channel_get_variable(channel, SWITCH_L_SDP_VARIABLE);
			fst_check(local_sdp != NULL);
			fst_check(strstr(local_sdp, "m=audio") != NULL);

			/* Step 2 & 3: Receive 2x RE-INVITE without SDP */
			simulate_reinvite_no_sdp(fst_session, 1);
			simulate_reinvite_no_sdp(fst_session, 2);

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO,
				"UAS Test 7 PASS: Send INVITE with SDP + receive 2x RE-INVITE without SDP\n");
		}
		FST_SESSION_END()

		/* Test Case 8: UAS - Send INVITE with SDP (SRTP), same crypto */
		FST_SESSION_BEGIN(uas_send_invite_srtp_receive_reinvite_no_sdp_same_key)
		{
			switch_channel_t *channel;
			const char *local_sdp;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			/* Step 1: Send INVITE with SDP (SRTP) */
			switch_channel_set_flag(channel, CF_3PCC_PROXY);
			set_local_sdp(fst_session, sample_sdp_srtp);

			local_sdp = switch_channel_get_variable(channel, SWITCH_L_SDP_VARIABLE);
			fst_check(has_crypto(local_sdp, NULL));

			/* Step 2 & 3: Receive 2x RE-INVITE without SDP */
			simulate_reinvite_no_sdp(fst_session, 1);
			simulate_reinvite_no_sdp(fst_session, 2);

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO,
				"UAS Test 8 PASS: Send INVITE with SRTP + receive 2x RE-INVITE (same key)\n");
		}
		FST_SESSION_END()

		/* Test Case 9: UAS - Send INVITE with SDP (SRTP), crypto key change */
		FST_SESSION_BEGIN(uas_send_invite_srtp_receive_reinvite_no_sdp_diff_key)
		{
			switch_channel_t *channel;
			const char *local_sdp;
			const char *crypto_key1;
			const char *crypto_key2;

			crypto_key1 = "WnD7c1ksDGs+dq5PI6rV/1Pj8+eSRCZQ8JWaGaQA";
			crypto_key2 = "XoE8d2ltEHt+er6QJ7sW/2Qk9+fTSDaR9KXbHbRB";

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			/* Step 1: Send INVITE with SDP (SRTP) with key 1 */
			switch_channel_set_flag(channel, CF_3PCC_PROXY);
			set_local_sdp(fst_session, sample_sdp_srtp);

			local_sdp = switch_channel_get_variable(channel, SWITCH_L_SDP_VARIABLE);
			fst_check(has_crypto(local_sdp, crypto_key1));

			/* Step 2: Simulate crypto key change */
			set_local_sdp(fst_session, sample_sdp_srtp_key2);
			local_sdp = switch_channel_get_variable(channel, SWITCH_L_SDP_VARIABLE);
			fst_check(has_crypto(local_sdp, crypto_key2));

			/* Step 3 & 4: Receive 2x RE-INVITE without SDP */
			simulate_reinvite_no_sdp(fst_session, 1);
			simulate_reinvite_no_sdp(fst_session, 2);

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO,
				"UAS Test 9 PASS: Send INVITE with SRTP + key change + 2x RE-INVITE\n");
		}
		FST_SESSION_END()

		/* Test Case 10: UAS - Send INVITE without SDP, then receive SDP with SRTP (same key) */
		FST_SESSION_BEGIN(uas_send_invite_no_sdp_srtp_receive_reinvite_no_sdp_same_key)
		{
			switch_channel_t *channel;
			const char *remote_sdp;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			/* Step 1: Send INVITE without SDP (UAS delayed offer) */
			switch_channel_set_flag(channel, CF_3PCC_PROXY);

			/* Step 2: Receive answer with SRTP */
			simulate_invite_with_sdp(fst_session, sample_sdp_srtp);
			remote_sdp = switch_channel_get_variable(channel, SWITCH_R_SDP_VARIABLE);
			fst_check(has_crypto(remote_sdp, NULL));

			/* Step 3 & 4: Receive 2x RE-INVITE without SDP (same crypto key) */
			simulate_reinvite_no_sdp(fst_session, 1);
			simulate_reinvite_no_sdp(fst_session, 2);

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO,
				"UAS Test 10 PASS: Send INVITE without SDP + receive SRTP answer + 2x RE-INVITE (same key)\n");
		}
		FST_SESSION_END()

		/* Test Case 11: UAS - Send INVITE without SDP, then receive SDP with SRTP (diff key) */
		FST_SESSION_BEGIN(uas_send_invite_no_sdp_srtp_receive_reinvite_no_sdp_diff_key)
		{
			switch_channel_t *channel;
			const char *remote_sdp;
			const char *crypto_key1;
			const char *crypto_key2;

			crypto_key1 = "WnD7c1ksDGs+dq5PI6rV/1Pj8+eSRCZQ8JWaGaQA";
			crypto_key2 = "XoE8d2ltEHt+er6QJ7sW/2Qk9+fTSDaR9KXbHbRB";

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			/* Step 1: Send INVITE without SDP (UAS delayed offer) */
			switch_channel_set_flag(channel, CF_3PCC_PROXY);

			/* Step 2: Receive answer with SRTP (key 1) */
			simulate_invite_with_sdp(fst_session, sample_sdp_srtp);
			remote_sdp = switch_channel_get_variable(channel, SWITCH_R_SDP_VARIABLE);
			fst_check(has_crypto(remote_sdp, crypto_key1));

			/* Step 3: Simulate crypto key change in remote SDP */
			switch_channel_set_variable(channel, SWITCH_R_SDP_VARIABLE, sample_sdp_srtp_key2);
			remote_sdp = switch_channel_get_variable(channel, SWITCH_R_SDP_VARIABLE);
			fst_check(has_crypto(remote_sdp, crypto_key2));
			fst_check(!has_crypto(remote_sdp, crypto_key1));

			/* Step 4 & 5: Receive 2x RE-INVITE without SDP */
			simulate_reinvite_no_sdp(fst_session, 1);
			simulate_reinvite_no_sdp(fst_session, 2);

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO,
				"UAS Test 11 PASS: Send INVITE without SDP + receive SRTP answer + key change + 2x RE-INVITE\n");
		}
		FST_SESSION_END()

		/* ========== INTEGRATION TESTS - ACTUAL SRTP ACTIVATION ========== */

		/* Test Case 12: Integration - UAC with actual SRTP media activation */
		FST_SESSION_BEGIN(integration_uac_srtp_media_activation)
		{
			switch_channel_t *channel;
			switch_core_media_params_t *mparams;
			switch_media_handle_t *media_handle;
			switch_status_t status;
			switch_rtp_t *rtp_session;
			const char *crypto_type;
			uint8_t match;
			uint8_t p;

			match = 0;
			p = 0;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			/* Enable SRTP */
			switch_channel_set_variable(channel, "rtp_secure_media", "true");
			switch_channel_set_flag(channel, CF_SECURE);
			switch_channel_set_flag(channel, CF_3PCC);

			/* Setup media params */
			mparams = switch_core_session_alloc(fst_session, sizeof(switch_core_media_params_t));
			mparams->num_codecs = 1;
			mparams->inbound_codec_string = switch_core_session_strdup(fst_session, "PCMU");
			mparams->outbound_codec_string = switch_core_session_strdup(fst_session, "PCMU");
			mparams->rtpip = switch_core_session_strdup(fst_session, "127.0.0.1");

			status = switch_media_handle_create(&media_handle, fst_session, mparams);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			switch_channel_set_variable(channel, "absolute_codec_string", "PCMU");

			/* Prepare codecs */
			switch_core_media_prepare_codecs(fst_session, SWITCH_FALSE);

			/* Negotiate SRTP SDP */
			match = switch_core_media_negotiate_sdp(fst_session, sample_sdp_srtp, &p, SDP_OFFER);
			fst_check(match > 0);

			/* Choose ports */
			status = switch_core_media_choose_ports(fst_session, SWITCH_TRUE, SWITCH_FALSE);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			/* Activate RTP with SRTP */
			status = switch_core_media_activate_rtp(fst_session);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			/* ========== COMPREHENSIVE SRTP VALIDATION ========== */

			/* 1. Verify CF_SECURE flag is still set */
			fst_check(switch_channel_test_flag(channel, CF_SECURE));

			/* 2. Get RTP session */
			rtp_session = switch_core_media_get_rtp_session(fst_session, SWITCH_MEDIA_TYPE_AUDIO);
			fst_requires(rtp_session != NULL);

			/* 3. Verify SRTP flags are set on RTP session */
			fst_check(switch_rtp_test_flag(rtp_session, SWITCH_RTP_FLAG_SECURE_SEND));
			fst_check(switch_rtp_test_flag(rtp_session, SWITCH_RTP_FLAG_SECURE_RECV));

			/* 4. Verify crypto type variable is set */
			crypto_type = switch_channel_get_variable(channel, "rtp_has_crypto");
			fst_check(crypto_type != NULL);
			fst_check(strstr(crypto_type, "AES_CM_128_HMAC_SHA1") != NULL);

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO,
				"Integration Test 12 PASS: UAC with actual SRTP media activation - ALL VALIDATIONS PASSED\n");

			/* Cleanup */
			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 13: Integration - UAS 3PCC with actual SRTP media activation */
		FST_SESSION_BEGIN(integration_uas_3pcc_srtp_media_activation)
		{
			switch_channel_t *channel;
			switch_core_media_params_t *mparams;
			switch_media_handle_t *media_handle;
			switch_status_t status;
			switch_rtp_t *rtp_session;
			const char *crypto_type;
			uint8_t match;
			uint8_t p;

			match = 0;
			p = 0;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			/* Enable SRTP for UAS 3PCC mode */
			switch_channel_set_variable(channel, "rtp_secure_media", "true");
			switch_channel_set_flag(channel, CF_SECURE);
			switch_channel_set_flag(channel, CF_3PCC_PROXY);

			/* Setup media params */
			mparams = switch_core_session_alloc(fst_session, sizeof(switch_core_media_params_t));
			mparams->num_codecs = 1;
			mparams->inbound_codec_string = switch_core_session_strdup(fst_session, "PCMU");
			mparams->outbound_codec_string = switch_core_session_strdup(fst_session, "PCMU");
			mparams->rtpip = switch_core_session_strdup(fst_session, "127.0.0.1");

			status = switch_media_handle_create(&media_handle, fst_session, mparams);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			switch_channel_set_variable(channel, "absolute_codec_string", "PCMU");

			/* Prepare codecs */
			switch_core_media_prepare_codecs(fst_session, SWITCH_FALSE);

			/* Negotiate SRTP SDP */
			match = switch_core_media_negotiate_sdp(fst_session, sample_sdp_srtp, &p, SDP_OFFER);
			fst_check(match > 0);

			status = switch_core_media_choose_ports(fst_session, SWITCH_TRUE, SWITCH_FALSE);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			status = switch_core_media_activate_rtp(fst_session);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			/* ========== COMPREHENSIVE SRTP VALIDATION ========== */

			/* 1. Verify both 3PCC_PROXY and SECURE flags are set */
			fst_check(switch_channel_test_flag(channel, CF_3PCC_PROXY));
			fst_check(switch_channel_test_flag(channel, CF_SECURE));

			/* 2. Get RTP session */
			rtp_session = switch_core_media_get_rtp_session(fst_session, SWITCH_MEDIA_TYPE_AUDIO);
			fst_requires(rtp_session != NULL);

			/* 3. Verify SRTP flags are set on RTP session */
			fst_check(switch_rtp_test_flag(rtp_session, SWITCH_RTP_FLAG_SECURE_SEND));
			fst_check(switch_rtp_test_flag(rtp_session, SWITCH_RTP_FLAG_SECURE_RECV));

			/* 4. Verify crypto type variable is set */
			crypto_type = switch_channel_get_variable(channel, "rtp_has_crypto");
			fst_check(crypto_type != NULL);
			fst_check(strstr(crypto_type, "AES_CM_128_HMAC_SHA1") != NULL);

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO,
				"Integration Test 13 PASS: UAS 3PCC with actual SRTP media activation - ALL VALIDATIONS PASSED\n");

			/* Cleanup */
			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 14: 3PCC flag validation */
		FST_SESSION_BEGIN(test_3pcc_flags)
		{
			switch_channel_t *channel;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			/* Test CF_3PCC flag */
			fst_check(!switch_channel_test_flag(channel, CF_3PCC));
			switch_channel_set_flag(channel, CF_3PCC);
			fst_check(switch_channel_test_flag(channel, CF_3PCC));

			/* Test CF_3PCC_PROXY flag */
			fst_check(!switch_channel_test_flag(channel, CF_3PCC_PROXY));
			switch_channel_set_flag(channel, CF_3PCC_PROXY);
			fst_check(switch_channel_test_flag(channel, CF_3PCC_PROXY));

			/* Both flags can be set simultaneously */
			fst_check(switch_channel_test_flag(channel, CF_3PCC));
			fst_check(switch_channel_test_flag(channel, CF_3PCC_PROXY));

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO,
				"3PCC flag validation test PASS\n");
		}
		FST_SESSION_END()

		/* Test Case 15: SDP and crypto validation helpers */
		FST_TEST_BEGIN(test_sdp_crypto_validation)
		{
			const char *crypto_key1;
			const char *crypto_key2;

			crypto_key1 = "WnD7c1ksDGs+dq5PI6rV/1Pj8+eSRCZQ8JWaGaQA";
			crypto_key2 = "XoE8d2ltEHt+er6QJ7sW/2Qk9+fTSDaR9KXbHbRB";

			/* Test RTP SDP */
			fst_check(strstr(sample_sdp_audio, "m=audio") != NULL);
			fst_check(strstr(sample_sdp_audio, "RTP/AVP") != NULL);
			fst_check(!has_crypto(sample_sdp_audio, NULL));

			/* Test SRTP SDP with key 1 */
			fst_check(strstr(sample_sdp_srtp, "RTP/SAVP") != NULL);
			fst_check(has_crypto(sample_sdp_srtp, NULL));
			fst_check(has_crypto(sample_sdp_srtp, crypto_key1));
			fst_check(!has_crypto(sample_sdp_srtp, crypto_key2));

			/* Test SRTP SDP with key 2 */
			fst_check(has_crypto(sample_sdp_srtp_key2, crypto_key2));
			fst_check(!has_crypto(sample_sdp_srtp_key2, crypto_key1));

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
				"SDP and crypto validation test PASS\n");
		}
		FST_TEST_END()

		/* ========== PROXY 3PCC LATE OFFER ========== */

		/* Test Case 16: proxy 3PCC late offer, rtp_secure_media=true and opt-in: SRTP offer only */
		FST_SESSION_BEGIN(proxy_3pcc_late_offer_secure_opt_in)
		{
			switch_channel_t *channel;
			const char *local_sdp;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			local_sdp = gen_proxy_3pcc_offer(fst_session, "rtp_secure_media", "true", "true");
			fst_requires(local_sdp != NULL);

			fst_check(!switch_channel_test_flag(channel, CF_3PCC));
			fst_check(has_crypto(local_sdp, NULL));
			fst_check(count_audio_mlines(local_sdp) == 1);
			fst_check(strstr(local_sdp, "RTP/AVP ") == NULL);
			fst_check(switch_channel_test_flag(channel, CF_SECURE));

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 17: proxy 3PCC late offer, rtp_secure_media_inbound=mandatory and opt-in: SRTP offer only */
		FST_SESSION_BEGIN(proxy_3pcc_late_offer_inbound_mandatory_opt_in)
		{
			const char *local_sdp;

			local_sdp = gen_proxy_3pcc_offer(fst_session, "rtp_secure_media_inbound", "mandatory", "true");
			fst_requires(local_sdp != NULL);

			fst_check(has_crypto(local_sdp, NULL));
			fst_check(count_audio_mlines(local_sdp) == 1);
			fst_check(strstr(local_sdp, "RTP/AVP ") == NULL);

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 18: proxy 3PCC late offer, rtp_secure_media=true without opt-in: plain RTP offer */
		FST_SESSION_BEGIN(proxy_3pcc_late_offer_secure_no_opt_in)
		{
			const char *local_sdp;

			local_sdp = gen_proxy_3pcc_offer(fst_session, "rtp_secure_media", "true", NULL);
			fst_requires(local_sdp != NULL);

			fst_check(strstr(local_sdp, "a=crypto:") == NULL);
			fst_check(strstr(local_sdp, "RTP/SAVP") == NULL);
			fst_check(count_audio_mlines(local_sdp) == 1);
			fst_check(strstr(local_sdp, "RTP/AVP ") != NULL);

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 19: proxy 3PCC late offer, rtp_secure_media=true with opt-in false: plain RTP offer */
		FST_SESSION_BEGIN(proxy_3pcc_late_offer_secure_opt_in_false)
		{
			const char *local_sdp;

			local_sdp = gen_proxy_3pcc_offer(fst_session, "rtp_secure_media", "true", "false");
			fst_requires(local_sdp != NULL);

			fst_check(strstr(local_sdp, "a=crypto:") == NULL);
			fst_check(strstr(local_sdp, "RTP/SAVP") == NULL);
			fst_check(count_audio_mlines(local_sdp) == 1);
			fst_check(strstr(local_sdp, "RTP/AVP ") != NULL);

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 20: proxy 3PCC late offer, opt-in without a secure media preference: single plain RTP offer */
		FST_SESSION_BEGIN(proxy_3pcc_late_offer_opt_in_no_preference)
		{
			const char *local_sdp;

			local_sdp = gen_proxy_3pcc_offer(fst_session, NULL, NULL, "true");
			fst_requires(local_sdp != NULL);

			fst_check(strstr(local_sdp, "a=crypto:") == NULL);
			fst_check(strstr(local_sdp, "RTP/SAVP") == NULL);
			fst_check(count_audio_mlines(local_sdp) == 1);
			fst_check(strstr(local_sdp, "RTP/AVP ") != NULL);

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 21: proxy 3PCC late offer, explicit rtp_secure_media=optional and opt-in: SRTP and RTP offered */
		FST_SESSION_BEGIN(proxy_3pcc_late_offer_optional_opt_in)
		{
			const char *local_sdp;

			local_sdp = gen_proxy_3pcc_offer(fst_session, "rtp_secure_media", "optional", "true");
			fst_requires(local_sdp != NULL);

			fst_check(count_audio_mlines(local_sdp) == 2);
			fst_check(strstr(local_sdp, "RTP/SAVP") != NULL);
			fst_check(strstr(local_sdp, "a=crypto:") != NULL);
			fst_check(strstr(local_sdp, "RTP/AVP ") != NULL);

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 22: proxy 3PCC late offer, opt-in, no preference, offer_both: SRTP and RTP offered */
		FST_SESSION_BEGIN(proxy_3pcc_late_offer_offer_both_opt_in)
		{
			switch_channel_t *channel;
			const char *local_sdp;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			switch_channel_set_variable(channel, "rtp_secure_media_3pcc_offer_both", "true");
			local_sdp = gen_proxy_3pcc_offer(fst_session, NULL, NULL, "true");
			fst_requires(local_sdp != NULL);

			fst_check(count_audio_mlines(local_sdp) == 2);
			fst_check(strstr(local_sdp, "a=crypto:") != NULL);

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 23: proxy 3PCC late offer, rtp_secure_media=forbidden and opt-in: plain RTP offer */
		FST_SESSION_BEGIN(proxy_3pcc_late_offer_forbidden_opt_in)
		{
			const char *local_sdp;

			local_sdp = gen_proxy_3pcc_offer(fst_session, "rtp_secure_media", "forbidden", "true");
			fst_requires(local_sdp != NULL);

			fst_check(rtp_only(local_sdp));

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 24: proxy 3PCC late offer, inbound preference wins over rtp_secure_media */
		FST_SESSION_BEGIN(proxy_3pcc_late_offer_inbound_preference_wins)
		{
			switch_channel_t *channel;
			const char *local_sdp;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			switch_channel_set_variable(channel, "rtp_secure_media", "true");
			local_sdp = gen_proxy_3pcc_offer(fst_session, "rtp_secure_media_inbound", "forbidden", "true");
			fst_requires(local_sdp != NULL);

			fst_check(rtp_only(local_sdp));

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 25: proxy 3PCC late offer with proxy media and opt-in: no crypto prepared */
		FST_SESSION_BEGIN(proxy_3pcc_late_offer_proxy_media_excluded)
		{
			switch_channel_t *channel;
			const char *local_sdp;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			switch_channel_set_flag(channel, CF_PROXY_MEDIA);
			local_sdp = gen_proxy_3pcc_offer(fst_session, "rtp_secure_media", "true", "true");

			fst_check(!local_sdp || !strstr(local_sdp, "a=crypto:"));
			fst_check(!switch_channel_test_flag(channel, CF_SECURE));

			switch_channel_clear_flag(channel, CF_PROXY_MEDIA);
			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 26: proxy 3PCC late offer with rtp_use_dtls and opt-in: no SDES crypto prepared */
		FST_SESSION_BEGIN(proxy_3pcc_late_offer_dtls_excluded)
		{
			switch_channel_t *channel;
			const char *local_sdp;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			switch_channel_set_variable(channel, "rtp_use_dtls", "true");
			local_sdp = gen_proxy_3pcc_offer(fst_session, "rtp_secure_media", "true", "true");
			fst_requires(local_sdp != NULL);

			fst_check(strstr(local_sdp, "a=crypto:") == NULL);
			fst_check(!switch_channel_test_flag(channel, CF_SECURE));

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 27: opt-in set as a global variable applies when the channel doesn't set it */
		FST_SESSION_BEGIN(proxy_3pcc_late_offer_global_opt_in)
		{
			const char *local_sdp;

			switch_core_set_variable("rtp_secure_media_3pcc_proxy_offer", "true");
			local_sdp = gen_proxy_3pcc_offer(fst_session, "rtp_secure_media", "true", NULL);
			switch_core_set_variable("rtp_secure_media_3pcc_proxy_offer", NULL);
			fst_requires(local_sdp != NULL);

			fst_check(srtp_only(local_sdp));

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 28: channel opt-in false overrides a global true */
		FST_SESSION_BEGIN(proxy_3pcc_late_offer_channel_overrides_global_true)
		{
			const char *local_sdp;

			switch_core_set_variable("rtp_secure_media_3pcc_proxy_offer", "true");
			local_sdp = gen_proxy_3pcc_offer(fst_session, "rtp_secure_media", "true", "false");
			switch_core_set_variable("rtp_secure_media_3pcc_proxy_offer", NULL);
			fst_requires(local_sdp != NULL);

			fst_check(rtp_only(local_sdp));

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 29: channel opt-in true overrides a global false */
		FST_SESSION_BEGIN(proxy_3pcc_late_offer_channel_overrides_global_false)
		{
			const char *local_sdp;

			switch_core_set_variable("rtp_secure_media_3pcc_proxy_offer", "false");
			local_sdp = gen_proxy_3pcc_offer(fst_session, "rtp_secure_media", "true", "true");
			switch_core_set_variable("rtp_secure_media_3pcc_proxy_offer", NULL);
			fst_requires(local_sdp != NULL);

			fst_check(srtp_only(local_sdp));

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 30: outbound offer with rtp_secure_media=true is unchanged: SRTP offer only */
		FST_SESSION_BEGIN(outbound_offer_secure_unchanged)
		{
			switch_channel_t *channel;
			const char *local_sdp;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			switch_channel_set_variable(channel, "rtp_secure_media", "true");
			local_sdp = gen_offer(fst_session, SWITCH_CALL_DIRECTION_OUTBOUND, 0);
			fst_requires(local_sdp != NULL);

			fst_check(srtp_only(local_sdp));

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 31: inbound 3PCC offer with rtp_secure_media=true is unchanged: SRTP offer only */
		FST_SESSION_BEGIN(inbound_3pcc_offer_secure_unchanged)
		{
			switch_channel_t *channel;
			const char *local_sdp;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			switch_channel_set_variable(channel, "rtp_secure_media", "true");
			local_sdp = gen_offer(fst_session, SWITCH_CALL_DIRECTION_INBOUND, CF_3PCC);
			fst_requires(local_sdp != NULL);

			fst_check(srtp_only(local_sdp));

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 32: inbound 3PCC offer without a preference is unchanged: single plain RTP offer */
		FST_SESSION_BEGIN(inbound_3pcc_offer_no_preference_unchanged)
		{
			const char *local_sdp;

			local_sdp = gen_offer(fst_session, SWITCH_CALL_DIRECTION_INBOUND, CF_3PCC);
			fst_requires(local_sdp != NULL);

			fst_check(rtp_only(local_sdp));

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 33: proxy 3PCC late offer, SRTP answer in the ACK activates SRTP with the offered key */
		FST_SESSION_BEGIN(proxy_3pcc_late_offer_ack_answer_activates_srtp)
		{
			switch_channel_t *channel;
			switch_rtp_t *rtp_session;
			const char *local_sdp;
			const char *local_key;
			const char *remote_key;
			const char *crypto_type;
			char offer_crypto[256];
			char answer[1024];
			uint8_t match;
			uint8_t p = 0;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			local_sdp = gen_proxy_3pcc_offer(fst_session, "rtp_secure_media", "true:AES_CM_128_HMAC_SHA1_80", "true");
			fst_requires(local_sdp != NULL);
			fst_requires(first_crypto(local_sdp, offer_crypto, sizeof(offer_crypto)));
			fst_requires(strstr(offer_crypto, " AES_CM_128_HMAC_SHA1_80 ") != NULL);
			local_sdp = switch_core_session_strdup(fst_session, local_sdp);

			build_srtp_answer(offer_crypto, answer, sizeof(answer));
			match = switch_core_media_negotiate_sdp(fst_session, answer, &p, SDP_ANSWER);
			fst_check(match > 0);

			fst_check(switch_core_media_choose_port(fst_session, SWITCH_MEDIA_TYPE_AUDIO, 0) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_media_activate_rtp(fst_session) == SWITCH_STATUS_SUCCESS);

			fst_check(switch_channel_test_flag(channel, CF_SECURE));

			local_key = switch_core_session_local_crypto_key(fst_session, SWITCH_MEDIA_TYPE_AUDIO);
			fst_requires(local_key != NULL);
			fst_check(strstr(local_sdp, local_key) != NULL);

			rtp_session = switch_core_media_get_rtp_session(fst_session, SWITCH_MEDIA_TYPE_AUDIO);
			fst_requires(rtp_session != NULL);
			fst_check(switch_rtp_test_flag(rtp_session, SWITCH_RTP_FLAG_SECURE_SEND));
			fst_check(switch_rtp_test_flag(rtp_session, SWITCH_RTP_FLAG_SECURE_RECV));

			crypto_type = switch_channel_get_variable(channel, "rtp_has_crypto");
			fst_requires(crypto_type != NULL);
			fst_check(strstr(offer_crypto, crypto_type) != NULL);

			remote_key = switch_channel_get_variable(channel, "srtp_remote_audio_crypto_key");
			fst_requires(remote_key != NULL);
			fst_check(strstr(remote_key, "inline:WnD7c1ksDGs+dq5PI6rV/1Pj8+eSRCZQ8JWaGaQA") != NULL);

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 34: proxy 3PCC late offer, mandatory SRTP and a plain RTP answer in the ACK: not negotiated */
		FST_SESSION_BEGIN(proxy_3pcc_late_offer_ack_plain_answer_rejected)
		{
			const char *local_sdp;
			uint8_t match;
			uint8_t p = 0;

			local_sdp = gen_proxy_3pcc_offer(fst_session, "rtp_secure_media", "true", "true");
			fst_requires(local_sdp != NULL);
			fst_requires(srtp_only(local_sdp));

			match = switch_core_media_negotiate_sdp(fst_session, sample_sdp_audio, &p, SDP_ANSWER);
			fst_check(match == 0);

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 35: proxy 3PCC late offer generated again before the ACK keeps its keys */
		FST_SESSION_BEGIN(proxy_3pcc_late_offer_regenerated_keeps_keys)
		{
			const char *local_sdp;
			char first[256];
			char second[256];

			local_sdp = gen_proxy_3pcc_offer(fst_session, "rtp_secure_media", "true", "true");
			fst_requires(first_crypto(local_sdp, first, sizeof(first)));

			switch_core_media_gen_local_sdp(fst_session, SDP_OFFER, NULL, 0, NULL, 1);
			local_sdp = switch_channel_get_variable(switch_core_session_get_channel(fst_session), "rtp_local_sdp_str");
			fst_requires(first_crypto(local_sdp, second, sizeof(second)));

			fst_check(srtp_only(local_sdp));
			fst_check_string_equals(first, second);

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 36: once the ACK clears CF_3PCC_PROXY, a later inbound offer is no longer prepared by it */
		FST_SESSION_BEGIN(proxy_3pcc_offer_after_ack_not_prepared)
		{
			switch_channel_t *channel;
			const char *local_sdp;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			local_sdp = gen_proxy_3pcc_offer(fst_session, NULL, NULL, "true");
			fst_requires(rtp_only(local_sdp));

			switch_channel_clear_flag(channel, CF_3PCC_PROXY);
			switch_channel_set_variable(channel, "rtp_secure_media", "true");
			switch_core_media_gen_local_sdp(fst_session, SDP_OFFER, NULL, 0, NULL, 1);
			local_sdp = switch_channel_get_variable(channel, "rtp_local_sdp_str");

			fst_check(rtp_only(local_sdp));
			fst_check(!switch_channel_test_flag(channel, CF_SECURE));

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 37: an SRTP proxy 3PCC call recovered with the opt-in on restores its negotiated keys only */
		FST_SESSION_BEGIN(proxy_3pcc_recovery_opt_in_on)
		{
			switch_channel_t *channel;
			switch_rtp_t *rtp_session;
			const char *local_sdp;
			const char *local_key;
			const char *var;
			const char *negotiated_key;
			char *negotiated;
			char *remote;
			char offer_crypto[256];

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);
			fst_requires(negotiate_proxy_srtp(fst_session));

			var = switch_channel_get_variable(channel, "srtp_local_audio_crypto_key_negotiated");
			fst_requires(var != NULL && strstr(var, "inline:") != NULL);
			negotiated = switch_core_session_strdup(fst_session, strstr(var, "inline:"));
			var = switch_channel_get_variable(channel, "srtp_remote_audio_crypto_key");
			fst_requires(var != NULL);
			remote = switch_core_session_strdup(fst_session, var);

			switch_media_handle_destroy(fst_session);
			fst_requires(create_media_handle(fst_session) == SWITCH_STATUS_SUCCESS);

			switch_channel_set_variable(channel, "rtp_secure_media", "true");
			switch_channel_set_variable(channel, "rtp_secure_media_3pcc_proxy_offer", "true");
			switch_channel_set_flag(channel, CF_RECOVERING);
			switch_core_media_recover_session(fst_session, NULL, NULL, SWITCH_TRUE);

			local_sdp = switch_channel_get_variable(channel, "rtp_local_sdp_str");
			fst_requires(local_sdp != NULL);
			fst_check(count_str(local_sdp, "a=crypto:") == 1);
			fst_requires(first_crypto(local_sdp, offer_crypto, sizeof(offer_crypto)));
			fst_check(strstr(offer_crypto, negotiated) != NULL);

			local_key = switch_core_session_local_crypto_key(fst_session, SWITCH_MEDIA_TYPE_AUDIO);
			fst_requires(local_key != NULL);
			fst_check(strstr(local_key, negotiated) != NULL);

			negotiated_key = switch_channel_get_variable(channel, "srtp_remote_audio_crypto_key");
			fst_requires(negotiated_key != NULL);
			fst_check_string_equals(remote, negotiated_key);

			rtp_session = switch_core_media_get_rtp_session(fst_session, SWITCH_MEDIA_TYPE_AUDIO);
			fst_requires(rtp_session != NULL);
			fst_check(switch_rtp_test_flag(rtp_session, SWITCH_RTP_FLAG_SECURE_SEND));
			fst_check(switch_rtp_test_flag(rtp_session, SWITCH_RTP_FLAG_SECURE_RECV));

			switch_channel_clear_flag(channel, CF_RECOVERING);
			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 37b: an SRTP proxy 3PCC call recovered with the opt-in off restores its negotiated keys only */
		FST_SESSION_BEGIN(proxy_3pcc_recovery_opt_in_off)
		{
			switch_channel_t *channel;
			switch_rtp_t *rtp_session;
			const char *local_sdp;
			const char *local_key;
			const char *var;
			const char *negotiated_key;
			char *negotiated;
			char *remote;
			char offer_crypto[256];

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);
			fst_requires(negotiate_proxy_srtp(fst_session));

			var = switch_channel_get_variable(channel, "srtp_local_audio_crypto_key_negotiated");
			fst_requires(var != NULL && strstr(var, "inline:") != NULL);
			negotiated = switch_core_session_strdup(fst_session, strstr(var, "inline:"));
			var = switch_channel_get_variable(channel, "srtp_remote_audio_crypto_key");
			fst_requires(var != NULL);
			remote = switch_core_session_strdup(fst_session, var);

			switch_media_handle_destroy(fst_session);
			fst_requires(create_media_handle(fst_session) == SWITCH_STATUS_SUCCESS);

			switch_channel_set_variable(channel, "rtp_secure_media", "true");
			switch_channel_set_variable(channel, "rtp_secure_media_3pcc_proxy_offer", "false");
			switch_channel_set_flag(channel, CF_RECOVERING);
			switch_core_media_recover_session(fst_session, NULL, NULL, SWITCH_TRUE);

			local_sdp = switch_channel_get_variable(channel, "rtp_local_sdp_str");
			fst_requires(local_sdp != NULL);
			fst_check(count_str(local_sdp, "a=crypto:") == 1);
			fst_requires(first_crypto(local_sdp, offer_crypto, sizeof(offer_crypto)));
			fst_check(strstr(offer_crypto, negotiated) != NULL);

			local_key = switch_core_session_local_crypto_key(fst_session, SWITCH_MEDIA_TYPE_AUDIO);
			fst_requires(local_key != NULL);
			fst_check(strstr(local_key, negotiated) != NULL);

			negotiated_key = switch_channel_get_variable(channel, "srtp_remote_audio_crypto_key");
			fst_requires(negotiated_key != NULL);
			fst_check_string_equals(remote, negotiated_key);

			rtp_session = switch_core_media_get_rtp_session(fst_session, SWITCH_MEDIA_TYPE_AUDIO);
			fst_requires(rtp_session != NULL);
			fst_check(switch_rtp_test_flag(rtp_session, SWITCH_RTP_FLAG_SECURE_SEND));
			fst_check(switch_rtp_test_flag(rtp_session, SWITCH_RTP_FLAG_SECURE_RECV));

			switch_channel_clear_flag(channel, CF_RECOVERING);
			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 37c: mandatory SRTP and an answer with a suite that wasn't offered: not negotiated */
		FST_SESSION_BEGIN(proxy_3pcc_late_offer_ack_unoffered_suite_rejected)
		{
			const char *local_sdp;
			char answer[1024];
			uint8_t match;
			uint8_t p = 0;

			local_sdp = gen_proxy_3pcc_offer(fst_session, "rtp_secure_media", "true:AES_CM_128_HMAC_SHA1_80", "true");
			fst_requires(srtp_only(local_sdp));
			fst_requires(strstr(local_sdp, "AES_CM_128_HMAC_SHA1_32") == NULL);

			build_srtp_answer("1 AES_CM_128_HMAC_SHA1_32 inline:x", answer, sizeof(answer));
			match = switch_core_media_negotiate_sdp(fst_session, answer, &p, SDP_ANSWER);
			fst_check(match == 0);

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 38: proxy 3PCC late offer honours a restricted suite list */
		FST_SESSION_BEGIN(proxy_3pcc_late_offer_suite_restricted)
		{
			const char *local_sdp;

			local_sdp = gen_proxy_3pcc_offer(fst_session, "rtp_secure_media", "true:AES_CM_128_HMAC_SHA1_32", "true");
			fst_requires(local_sdp != NULL);

			fst_check(srtp_only(local_sdp));
			fst_check(strstr(local_sdp, "AES_CM_128_HMAC_SHA1_32") != NULL);
			fst_check(strstr(local_sdp, "AES_CM_128_HMAC_SHA1_80") == NULL);

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		/* Test Case 39: proxy mode, AVPF, DTLS and media_webrtc are excluded from SDES preparation */
		FST_SESSION_BEGIN(proxy_3pcc_late_offer_proxy_mode_excluded)
		{
			switch_channel_t *channel;
			const char *local_sdp;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			switch_channel_set_flag(channel, CF_PROXY_MODE);
			local_sdp = gen_proxy_3pcc_offer(fst_session, "rtp_secure_media", "true", "true");

			fst_check(!local_sdp || !strstr(local_sdp, "a=crypto:"));
			fst_check(!switch_channel_test_flag(channel, CF_SECURE));

			switch_channel_clear_flag(channel, CF_PROXY_MODE);
			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(proxy_3pcc_late_offer_avpf_excluded)
		{
			switch_channel_t *channel;
			const char *local_sdp;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			switch_channel_set_flag(channel, CF_AVPF);
			local_sdp = gen_proxy_3pcc_offer(fst_session, "rtp_secure_media", "true", "true");

			fst_check(!local_sdp || !strstr(local_sdp, "a=crypto:"));
			fst_check(!switch_channel_test_flag(channel, CF_SECURE));

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(proxy_3pcc_late_offer_dtls_flag_excluded)
		{
			switch_channel_t *channel;
			const char *local_sdp;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			switch_channel_set_flag(channel, CF_DTLS);
			local_sdp = gen_proxy_3pcc_offer(fst_session, "rtp_secure_media", "true", "true");

			fst_check(!local_sdp || !strstr(local_sdp, "a=crypto:"));
			fst_check(!switch_channel_test_flag(channel, CF_SECURE));

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(proxy_3pcc_late_offer_media_webrtc_excluded)
		{
			switch_channel_t *channel;
			const char *local_sdp;

			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);

			switch_channel_set_variable(channel, "media_webrtc", "true");
			local_sdp = gen_proxy_3pcc_offer(fst_session, "rtp_secure_media", "true", "true");

			fst_check(!local_sdp || !strstr(local_sdp, "a=crypto:"));
			fst_check(!switch_channel_test_flag(channel, CF_SECURE));

			switch_media_handle_destroy(fst_session);
		}
		FST_SESSION_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
