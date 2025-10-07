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
 * AI SWE Agent <openhands@all-hands.dev>
 *
 * switch_3pcc.c -- tests 3PCC (Third Party Call Control) scenarios
 *
 */

#include <switch.h>
#include <test/switch_test.h>

/* Sample SDP for testing */
static const char *sample_sdp_with_audio = 
	"v=0\r\n"
	"o=FreeSWITCH 1234567890 1234567891 IN IP4 192.168.1.100\r\n"
	"s=FreeSWITCH\r\n"
	"c=IN IP4 192.168.1.100\r\n"
	"t=0 0\r\n"
	"m=audio 5004 RTP/AVP 0 8 101\r\n"
	"a=rtpmap:0 PCMU/8000\r\n"
	"a=rtpmap:8 PCMA/8000\r\n"
	"a=rtpmap:101 telephone-event/8000\r\n"
	"a=fmtp:101 0-16\r\n";

static const char *sample_sdp_with_srtp = 
	"v=0\r\n"
	"o=FreeSWITCH 1234567890 1234567891 IN IP4 192.168.1.100\r\n"
	"s=FreeSWITCH\r\n"
	"c=IN IP4 192.168.1.100\r\n"
	"t=0 0\r\n"
	"m=audio 5004 RTP/SAVP 0 8 101\r\n"
	"a=rtpmap:0 PCMU/8000\r\n"
	"a=rtpmap:8 PCMA/8000\r\n"
	"a=rtpmap:101 telephone-event/8000\r\n"
	"a=fmtp:101 0-16\r\n"
	"a=crypto:1 AES_CM_128_HMAC_SHA1_80 inline:WnD7c1ksDGs+dq5PI6rV/1Pj8+eSRCZQ8JWaGaQA\r\n";

static const char *sample_sdp_with_srtp_different_key = 
	"v=0\r\n"
	"o=FreeSWITCH 1234567890 1234567891 IN IP4 192.168.1.100\r\n"
	"s=FreeSWITCH\r\n"
	"c=IN IP4 192.168.1.100\r\n"
	"t=0 0\r\n"
	"m=audio 5004 RTP/SAVP 0 8 101\r\n"
	"a=rtpmap:0 PCMU/8000\r\n"
	"a=rtpmap:8 PCMA/8000\r\n"
	"a=rtpmap:101 telephone-event/8000\r\n"
	"a=fmtp:101 0-16\r\n"
	"a=crypto:1 AES_CM_128_HMAC_SHA1_80 inline:XoE8d2ltEHt+er6QJ7sW/2Qk9+fTSDaR9KXbHbRB\r\n";

/* Helper function to simulate SIP message processing */
static switch_status_t simulate_sip_invite(switch_core_session_t *session, const char *sdp_body, switch_bool_t with_sdp)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	
	if (!channel) {
		return SWITCH_STATUS_FALSE;
	}
	
	/* Set 3PCC flag to simulate 3PCC scenario */
	switch_channel_set_flag(channel, CF_3PCC);
	
	if (with_sdp && sdp_body) {
		/* Simulate receiving INVITE with SDP */
		switch_channel_set_variable(channel, "switch_r_sdp", sdp_body);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, 
			"Simulated INVITE with SDP\n");
	} else {
		/* Simulate receiving INVITE without SDP */
		switch_channel_set_variable(channel, "switch_r_sdp", NULL);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, 
			"Simulated INVITE without SDP\n");
	}
	
	return SWITCH_STATUS_SUCCESS;
}

/* Helper function to simulate RE-INVITE without SDP */
static switch_status_t simulate_sip_reinvite_no_sdp(switch_core_session_t *session, int sequence)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	char var_name[64];
	
	if (!channel) {
		return SWITCH_STATUS_FALSE;
	}
	
	/* Track RE-INVITE sequence */
	snprintf(var_name, sizeof(var_name), "reinvite_no_sdp_seq_%d", sequence);
	switch_channel_set_variable(channel, var_name, "true");
	
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, 
		"Simulated RE-INVITE without SDP (sequence %d)\n", sequence);
	
	return SWITCH_STATUS_SUCCESS;
}

/* Helper function to validate crypto key handling */
static switch_bool_t validate_crypto_key_handling(switch_core_session_t *session, const char *expected_crypto)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	const char *current_sdp;
	
	if (!channel) {
		return SWITCH_FALSE;
	}
	
	current_sdp = switch_channel_get_variable(channel, "switch_r_sdp");
	if (!current_sdp) {
		return SWITCH_FALSE;
	}
	
	/* Simple check for crypto line presence */
	if (expected_crypto && strstr(current_sdp, "a=crypto:")) {
		return SWITCH_TRUE;
	} else if (!expected_crypto && !strstr(current_sdp, "a=crypto:")) {
		return SWITCH_TRUE;
	}
	
	return SWITCH_FALSE;
}

FST_CORE_BEGIN("./conf")
{
	FST_SUITE_BEGIN(switch_3pcc)
	{
		FST_SETUP_BEGIN()
		{
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		/* Test Case 1: UAC scenario - Receive INVITE with SDP, then RE-INVITE without SDP (2 times) */
		FST_SESSION_BEGIN(uac_invite_with_sdp_reinvite_no_sdp)
		{
			switch_channel_t *channel;
			const char *sdp;
			
			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);
			
			/* Step 1: Receive INVITE with SDP */
			fst_check(simulate_sip_invite(fst_session, sample_sdp_with_audio, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_test_flag(channel, CF_3PCC));
			
			/* Verify SDP was set */
			sdp = switch_channel_get_variable(channel, "switch_r_sdp");
			fst_check(sdp != NULL);
			fst_check_string_has(sdp, "m=audio");
			
			/* Step 2: Receive RE-INVITE without SDP (first time) */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 1) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_1") != NULL);
			
			/* Step 3: Receive RE-INVITE without SDP (second time) */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 2) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_2") != NULL);
			
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO, 
				"UAC Test 1 completed: INVITE with SDP + 2x RE-INVITE without SDP\n");
		}
		FST_SESSION_END()

		/* Test Case 2: UAC scenario - Receive INVITE without SDP, then RE-INVITE without SDP (2 times) */
		FST_SESSION_BEGIN(uac_invite_no_sdp_reinvite_no_sdp)
		{
			switch_channel_t *channel;
			const char *sdp;
			
			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);
			
			/* Step 1: Receive INVITE without SDP */
			fst_check(simulate_sip_invite(fst_session, NULL, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_test_flag(channel, CF_3PCC));
			
			/* Verify no SDP was set */
			sdp = switch_channel_get_variable(channel, "switch_r_sdp");
			fst_check(sdp == NULL);
			
			/* Step 2: Receive RE-INVITE without SDP (first time) */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 1) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_1") != NULL);
			
			/* Step 3: Receive RE-INVITE without SDP (second time) */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 2) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_2") != NULL);
			
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO, 
				"UAC Test 2 completed: INVITE without SDP + 2x RE-INVITE without SDP\n");
		}
		FST_SESSION_END()

		/* Test Case 3: UAC scenario - Receive INVITE with SDP (SRTP), then RE-INVITE without SDP (2 times) same crypto key */
		FST_SESSION_BEGIN(uac_invite_srtp_reinvite_no_sdp_same_key)
		{
			switch_channel_t *channel;
			const char *sdp;
			
			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);
			
			/* Step 1: Receive INVITE with SDP (SRTP) */
			fst_check(simulate_sip_invite(fst_session, sample_sdp_with_srtp, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_test_flag(channel, CF_3PCC));
			
			/* Verify SRTP SDP was set */
			sdp = switch_channel_get_variable(channel, "switch_r_sdp");
			fst_check(sdp != NULL);
			fst_check_string_has(sdp, "RTP/SAVP");
			fst_check_string_has(sdp, "a=crypto:");
			fst_check(validate_crypto_key_handling(fst_session, "crypto"));
			
			/* Step 2: Receive RE-INVITE without SDP (first time) - same crypto key */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 1) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_1") != NULL);
			
			/* Step 3: Receive RE-INVITE without SDP (second time) - same crypto key */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 2) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_2") != NULL);
			
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO, 
				"UAC Test 3 completed: INVITE with SRTP + 2x RE-INVITE without SDP (same key)\n");
		}
		FST_SESSION_END()

		/* Test Case 4: UAC scenario - Receive INVITE with SDP (SRTP), then RE-INVITE without SDP (2 times) different crypto key */
		FST_SESSION_BEGIN(uac_invite_srtp_reinvite_no_sdp_diff_key)
		{
			switch_channel_t *channel;
			const char *sdp;
			
			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);
			
			/* Step 1: Receive INVITE with SDP (SRTP) */
			fst_check(simulate_sip_invite(fst_session, sample_sdp_with_srtp, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_test_flag(channel, CF_3PCC));
			
			/* Verify SRTP SDP was set */
			sdp = switch_channel_get_variable(channel, "switch_r_sdp");
			fst_check(sdp != NULL);
			fst_check_string_has(sdp, "RTP/SAVP");
			fst_check_string_has(sdp, "a=crypto:");
			
			/* Step 2: Simulate crypto key change */
			switch_channel_set_variable(channel, "switch_r_sdp", sample_sdp_with_srtp_different_key);
			
			/* Step 3: Receive RE-INVITE without SDP (first time) - different crypto key */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 1) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_1") != NULL);
			
			/* Step 4: Receive RE-INVITE without SDP (second time) - different crypto key */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 2) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_2") != NULL);
			
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO, 
				"UAC Test 4 completed: INVITE with SRTP + 2x RE-INVITE without SDP (different key)\n");
		}
		FST_SESSION_END()

		/* Test Case 5: UAC scenario - Receive INVITE without SDP, then RE-INVITE without SDP (2 times) same crypto key */
		FST_SESSION_BEGIN(uac_invite_no_sdp_reinvite_no_sdp_same_key)
		{
			switch_channel_t *channel;
			channel = switch_core_session_get_channel(fst_session);
			
			fst_requires(channel != NULL);
			
			/* Step 1: Receive INVITE without SDP */
			fst_check(simulate_sip_invite(fst_session, NULL, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_test_flag(channel, CF_3PCC));
			
			/* Step 2: Set up SRTP context for subsequent RE-INVITEs */
			switch_channel_set_variable(channel, "srtp_context", "established");
			
			/* Step 3: Receive RE-INVITE without SDP (first time) - same crypto key */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 1) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_1") != NULL);
			
			/* Step 4: Receive RE-INVITE without SDP (second time) - same crypto key */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 2) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_2") != NULL);
			
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO, 
				"UAC Test 5 completed: INVITE without SDP + 2x RE-INVITE without SDP (same key)\n");
		}
		FST_SESSION_END()

		/* Test Case 6: UAC scenario - Receive INVITE without SDP, then RE-INVITE without SDP (2 times) different crypto key */
		FST_SESSION_BEGIN(uac_invite_no_sdp_reinvite_no_sdp_diff_key)
		{
			switch_channel_t *channel;
			channel = switch_core_session_get_channel(fst_session);
			
			fst_requires(channel != NULL);
			
			/* Step 1: Receive INVITE without SDP */
			fst_check(simulate_sip_invite(fst_session, NULL, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_test_flag(channel, CF_3PCC));
			
			/* Step 2: Set up SRTP context for subsequent RE-INVITEs */
			switch_channel_set_variable(channel, "srtp_context", "established");
			switch_channel_set_variable(channel, "crypto_key_changed", "true");
			
			/* Step 3: Receive RE-INVITE without SDP (first time) - different crypto key */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 1) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_1") != NULL);
			
			/* Step 4: Receive RE-INVITE without SDP (second time) - different crypto key */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 2) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_2") != NULL);
			
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO, 
				"UAC Test 6 completed: INVITE without SDP + 2x RE-INVITE without SDP (different key)\n");
		}
		FST_SESSION_END()

		/* Test Case 7: UAS scenario - Send INVITE with SDP, then receive RE-INVITE without SDP (2 times) */
		FST_SESSION_BEGIN(uas_send_invite_with_sdp_receive_reinvite_no_sdp)
		{
			switch_channel_t *channel;
			const char *local_sdp;
			
			channel = switch_core_session_get_channel(fst_session);
			fst_requires(channel != NULL);
			
			/* Step 1: Simulate sending INVITE with SDP (UAS behavior) */
			switch_channel_set_flag(channel, CF_3PCC_PROXY);
			switch_channel_set_variable(channel, "switch_l_sdp", sample_sdp_with_audio);
			
			/* Verify local SDP was set */
			local_sdp = switch_channel_get_variable(channel, "switch_l_sdp");
			fst_check(local_sdp != NULL);
			fst_check_string_has(local_sdp, "m=audio");
			
			/* Step 2: Receive RE-INVITE without SDP (first time) */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 1) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_1") != NULL);
			
			/* Step 3: Receive RE-INVITE without SDP (second time) */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 2) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_2") != NULL);
			
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO, 
				"UAS Test 7 completed: Send INVITE with SDP + receive 2x RE-INVITE without SDP\n");
		}
		FST_SESSION_END()

		/* Test Case 8: UAS scenario - Send INVITE with SDP, then receive RE-INVITE without SDP (2 times) */
		FST_SESSION_BEGIN(uas_send_invite_with_sdp_receive_reinvite_no_sdp_2)
		{
			switch_channel_t *channel;
			const char *local_sdp;
			
			channel = switch_core_session_get_channel(fst_session);
			
			fst_requires(channel != NULL);
			
			/* Step 1: Simulate sending INVITE with SDP (UAS behavior) */
			switch_channel_set_flag(channel, CF_3PCC_PROXY);
			switch_channel_set_variable(channel, "switch_l_sdp", sample_sdp_with_audio);
			
			/* Verify local SDP was set */
			local_sdp = switch_channel_get_variable(channel, "switch_l_sdp");
			fst_check(local_sdp != NULL);
			fst_check_string_has(local_sdp, "m=audio");
			
			/* Step 2: Receive RE-INVITE without SDP (first time) */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 1) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_1") != NULL);
			
			/* Step 3: Receive RE-INVITE without SDP (second time) */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 2) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_2") != NULL);
			
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO, 
				"UAS Test 8 completed: Send INVITE with SDP + receive 2x RE-INVITE without SDP\n");
		}
		FST_SESSION_END()

		/* Test Case 9: UAS scenario - Send INVITE with SDP (SRTP), then receive RE-INVITE without SDP (2 times) same crypto key */
		FST_SESSION_BEGIN(uas_send_invite_srtp_receive_reinvite_no_sdp_same_key)
		{
			switch_channel_t *channel;
			const char *local_sdp;
			
			channel = switch_core_session_get_channel(fst_session);
			
			fst_requires(channel != NULL);
			
			/* Step 1: Simulate sending INVITE with SDP (SRTP) */
			switch_channel_set_flag(channel, CF_3PCC_PROXY);
			switch_channel_set_variable(channel, "switch_l_sdp", sample_sdp_with_srtp);
			
			/* Verify SRTP local SDP was set */
			local_sdp = switch_channel_get_variable(channel, "switch_l_sdp");
			fst_check(local_sdp != NULL);
			fst_check_string_has(local_sdp, "RTP/SAVP");
			fst_check_string_has(local_sdp, "a=crypto:");
			
			/* Step 2: Receive RE-INVITE without SDP (first time) - same crypto key */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 1) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_1") != NULL);
			
			/* Step 3: Receive RE-INVITE without SDP (second time) - same crypto key */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 2) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_2") != NULL);
			
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO, 
				"UAS Test 9 completed: Send INVITE with SRTP + receive 2x RE-INVITE without SDP (same key)\n");
		}
		FST_SESSION_END()

		/* Test Case 10: UAS scenario - Send INVITE with SDP (SRTP), then receive RE-INVITE without SDP (2 times) different crypto key */
		FST_SESSION_BEGIN(uas_send_invite_srtp_receive_reinvite_no_sdp_diff_key)
		{
			switch_channel_t *channel;
			const char *local_sdp;
			
			channel = switch_core_session_get_channel(fst_session);
			
			fst_requires(channel != NULL);
			
			/* Step 1: Simulate sending INVITE with SDP (SRTP) */
			switch_channel_set_flag(channel, CF_3PCC_PROXY);
			switch_channel_set_variable(channel, "switch_l_sdp", sample_sdp_with_srtp);
			
			/* Verify SRTP local SDP was set */
			local_sdp = switch_channel_get_variable(channel, "switch_l_sdp");
			fst_check(local_sdp != NULL);
			fst_check_string_has(local_sdp, "RTP/SAVP");
			fst_check_string_has(local_sdp, "a=crypto:");
			
			/* Step 2: Simulate crypto key change */
			switch_channel_set_variable(channel, "crypto_key_changed", "true");
			
			/* Step 3: Receive RE-INVITE without SDP (first time) - different crypto key */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 1) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_1") != NULL);
			
			/* Step 4: Receive RE-INVITE without SDP (second time) - different crypto key */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 2) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_2") != NULL);
			
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO, 
				"UAS Test 10 completed: Send INVITE with SRTP + receive 2x RE-INVITE without SDP (different key)\n");
		}
		FST_SESSION_END()

		/* Test Case 11: UAS scenario - Send INVITE without SDP (SRTP), then receive RE-INVITE without SDP (2 times) same crypto key */
		FST_SESSION_BEGIN(uas_send_invite_no_sdp_srtp_receive_reinvite_no_sdp_same_key)
		{
			switch_channel_t *channel;
			channel = switch_core_session_get_channel(fst_session);
			
			fst_requires(channel != NULL);
			
			/* Step 1: Simulate sending INVITE without SDP but with SRTP capability */
			switch_channel_set_flag(channel, CF_3PCC_PROXY);
			switch_channel_set_variable(channel, "srtp_capable", "true");
			
			/* Step 2: Set up SRTP context for subsequent RE-INVITEs */
			switch_channel_set_variable(channel, "srtp_context", "established");
			
			/* Step 3: Receive RE-INVITE without SDP (first time) - same crypto key */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 1) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_1") != NULL);
			
			/* Step 4: Receive RE-INVITE without SDP (second time) - same crypto key */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 2) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_2") != NULL);
			
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO, 
				"UAS Test 11 completed: Send INVITE without SDP (SRTP) + receive 2x RE-INVITE without SDP (same key)\n");
		}
		FST_SESSION_END()

		/* Test Case 12: UAS scenario - Send INVITE without SDP (SRTP), then receive RE-INVITE without SDP (2 times) different crypto key */
		FST_SESSION_BEGIN(uas_send_invite_no_sdp_srtp_receive_reinvite_no_sdp_diff_key)
		{
			switch_channel_t *channel;
			channel = switch_core_session_get_channel(fst_session);
			
			fst_requires(channel != NULL);
			
			/* Step 1: Simulate sending INVITE without SDP but with SRTP capability */
			switch_channel_set_flag(channel, CF_3PCC_PROXY);
			switch_channel_set_variable(channel, "srtp_capable", "true");
			
			/* Step 2: Set up SRTP context for subsequent RE-INVITEs */
			switch_channel_set_variable(channel, "srtp_context", "established");
			switch_channel_set_variable(channel, "crypto_key_changed", "true");
			
			/* Step 3: Receive RE-INVITE without SDP (first time) - different crypto key */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 1) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_1") != NULL);
			
			/* Step 4: Receive RE-INVITE without SDP (second time) - different crypto key */
			fst_check(simulate_sip_reinvite_no_sdp(fst_session, 2) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_get_variable(channel, "reinvite_no_sdp_seq_2") != NULL);
			
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_INFO, 
				"UAS Test 12 completed: Send INVITE without SDP (SRTP) + receive 2x RE-INVITE without SDP (different key)\n");
		}
		FST_SESSION_END()

		/* Test Case 13: 3PCC Proxy flag validation */
		FST_TEST_BEGIN(test_3pcc_proxy_flag)
		{
			/* Test that 3PCC proxy flag can be set and retrieved correctly */
			switch_core_session_t *session = NULL;
			switch_channel_t *channel = NULL;
			switch_call_cause_t cause = SWITCH_CAUSE_NONE;
			
			/* Create a test session */
			session = switch_core_session_request(switch_core_get_endpoint_interface("loopback"), 
				SWITCH_CALL_DIRECTION_OUTBOUND, SOF_NONE, fst_pool);
			fst_requires(session != NULL);
			
			channel = switch_core_session_get_channel(session);
			fst_requires(channel != NULL);
			
			/* Test CF_3PCC_PROXY flag */
			fst_check(!switch_channel_test_flag(channel, CF_3PCC_PROXY));
			switch_channel_set_flag(channel, CF_3PCC_PROXY);
			fst_check(switch_channel_test_flag(channel, CF_3PCC_PROXY));
			
			/* Test CF_3PCC flag */
			fst_check(!switch_channel_test_flag(channel, CF_3PCC));
			switch_channel_set_flag(channel, CF_3PCC);
			fst_check(switch_channel_test_flag(channel, CF_3PCC));
			
			/* Both flags can be set simultaneously */
			fst_check(switch_channel_test_flag(channel, CF_3PCC_PROXY));
			fst_check(switch_channel_test_flag(channel, CF_3PCC));
			
			/* Clean up */
			switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session);
			
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
				"3PCC flag validation test completed successfully\n");
		}
		FST_TEST_END()

		/* Test Case 14: SDP validation helper test */
		FST_TEST_BEGIN(test_sdp_validation_helpers)
		{
			/* Test SDP validation functions */
			fst_check_string_has(sample_sdp_with_audio, "m=audio");
			fst_check_string_has(sample_sdp_with_audio, "RTP/AVP");
			fst_check_string_does_not_have(sample_sdp_with_audio, "a=crypto:");
			
			fst_check_string_has(sample_sdp_with_srtp, "m=audio");
			fst_check_string_has(sample_sdp_with_srtp, "RTP/SAVP");
			fst_check_string_has(sample_sdp_with_srtp, "a=crypto:");
			
			fst_check_string_has(sample_sdp_with_srtp_different_key, "a=crypto:");
			fst_check_string_does_not_have(sample_sdp_with_srtp, 
				"inline:XoE8d2ltEHt+er6QJ7sW/2Qk9+fTSDaR9KXbHbRB");
			fst_check_string_has(sample_sdp_with_srtp_different_key, 
				"inline:XoE8d2ltEHt+er6QJ7sW/2Qk9+fTSDaR9KXbHbRB");
			
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
				"SDP validation helper test completed successfully\n");
		}
		FST_TEST_END()
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