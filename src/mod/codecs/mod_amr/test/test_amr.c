/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2021, Anthony Minessale II <anthm@freeswitch.org>
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
 * Dragos Oancea <dragos@signalwire.com>
 *
 *
 * test_amr.c -- tests mod_amr
 *
 */

#ifndef AMR_PASSTHROUGH
#include <switch.h>
#include <stdlib.h>

#include <test/switch_test.h>

#define AMR_FMTP_OA "octet-align=1; mode-set=5"
#define AMR_FMTP_BE "mode-set=5"

typedef struct {
	int negotiated;
	int decoded;
	int codec_reset;
} amr_reoffer_result_t;

static const char *amr_offer(switch_core_session_t *session, int version, const char *fmtp)
{
	return switch_core_session_sprintf(session,
		"v=0\r\n"
		"o=- 1 %d IN IP4 198.51.100.1\r\n"
		"s=-\r\n"
		"t=0 0\r\n"
		"m=audio 56210 RTP/AVP 96\r\n"
		"c=IN IP4 198.51.100.1\r\n"
		"a=rtpmap:96 AMR/8000\r\n"
		"a=fmtp:96 %s\r\n"
		"a=sendrecv\r\n", version, fmtp);
}

static switch_status_t amr_encode_frame(switch_memory_pool_t *pool, const char *fmtp, uint8_t *pkt, uint32_t *pkt_len)
{
	switch_codec_t codec = { 0 };
	switch_codec_settings_t codec_settings = {{ 0 }};
	int16_t pcm[160];
	uint32_t seed = 12345, rate = 8000, flags = 0;
	switch_status_t status;
	int i;

	for (i = 0; i < 160; i++) {
		seed = seed * 1103515245 + 12345;
		pcm[i] = (int16_t) ((int) ((seed >> 16) & 0x3fff) - 0x2000);
	}

	if (switch_core_codec_init(&codec, "AMR", "mod_amr", fmtp, 8000, 20, 1,
							   SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, &codec_settings, pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	status = switch_core_codec_encode(&codec, NULL, pcm, sizeof(pcm), 8000, pkt, pkt_len, &rate, &flags);
	switch_core_codec_destroy(&codec);

	return status;
}

static int amr_decodes_as(switch_codec_t *codec, switch_memory_pool_t *pool, const char *fmtp, uint8_t *pkt, uint32_t pkt_len)
{
	switch_codec_t ref_codec = { 0 };
	switch_codec_settings_t codec_settings = {{ 0 }};
	unsigned char ref[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
	unsigned char got[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
	uint32_t ref_len = sizeof(ref), got_len = sizeof(got), rate = 8000, flags = 0;
	switch_status_t ref_status, got_status;

	if (switch_core_codec_init(&ref_codec, "AMR", "mod_amr", fmtp, 8000, 20, 1,
							   SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, &codec_settings, pool) != SWITCH_STATUS_SUCCESS) {
		return 0;
	}

	ref_status = switch_core_codec_decode(&ref_codec, NULL, pkt, pkt_len, 8000, ref, &ref_len, &rate, &flags);
	switch_core_codec_destroy(&ref_codec);

	got_status = switch_core_codec_decode(codec, NULL, pkt, pkt_len, 8000, got, &got_len, &rate, &flags);

	return ref_status == SWITCH_STATUS_SUCCESS && got_status == SWITCH_STATUS_SUCCESS &&
		ref_len == 320 && got_len == ref_len && !memcmp(ref, got, ref_len);
}

static amr_reoffer_result_t amr_reoffer(switch_core_session_t *session, const char *strict,
										const char *first_fmtp, const char *second_fmtp)
{
	amr_reoffer_result_t result = { 0 };
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_memory_pool_t *pool = switch_core_session_get_pool(session);
	switch_media_handle_t *media_handle = NULL;
	switch_core_media_params_t *mparams;
	switch_codec_t *read_codec;
	void *initial_context;
	uint8_t pkt[64] = { 0 };
	uint32_t pkt_len = sizeof(pkt);
	uint8_t p = 0;

	if (strict) {
		switch_channel_set_variable(channel, "telnyx-strict-codec-match", strict);
	}

	mparams = switch_core_session_alloc(session, sizeof(switch_core_media_params_t));
	mparams->inbound_codec_string = switch_core_session_strdup(session, "AMR");
	mparams->outbound_codec_string = switch_core_session_strdup(session, "AMR");
	mparams->rtpip = switch_core_session_strdup(session, "127.0.0.1");

	if (switch_media_handle_create(&media_handle, session, mparams) != SWITCH_STATUS_SUCCESS ||
		switch_core_media_prepare_codecs(session, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS ||
		switch_core_media_negotiate_sdp(session, amr_offer(session, 1, first_fmtp), &p, SDP_OFFER) != 1 ||
		!(read_codec = switch_core_session_get_read_codec(session)) || !switch_core_codec_ready(read_codec)) {
		return result;
	}

	initial_context = read_codec->private_info;

	if (switch_core_media_negotiate_sdp(session, amr_offer(session, 2, second_fmtp), &p, SDP_OFFER) != 1 ||
		!(read_codec = switch_core_session_get_read_codec(session)) || !switch_core_codec_ready(read_codec)) {
		return result;
	}

	result.negotiated = 1;
	result.codec_reset = read_codec->private_info != initial_context;

	if (amr_encode_frame(pool, second_fmtp, pkt, &pkt_len) == SWITCH_STATUS_SUCCESS && pkt_len > 20) {
		result.decoded = amr_decodes_as(read_codec, pool, second_fmtp, pkt, pkt_len);
	}

	return result;
}

FST_CORE_BEGIN(".")
{
	FST_SUITE_BEGIN(test_amr)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_loopback");
			fst_requires_module("mod_amr");
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		FST_SESSION_BEGIN(amr_strict_reoffer_octet_aligned_to_bandwidth_efficient)
		{
			amr_reoffer_result_t result = amr_reoffer(fst_session, "true", AMR_FMTP_OA, AMR_FMTP_BE);

			fst_requires(result.negotiated);
			fst_xcheck(result.codec_reset, "a framing change resets the codec");
			fst_xcheck(result.decoded, "bandwidth efficient frames decode after the re-offer");
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(amr_strict_reoffer_bandwidth_efficient_to_octet_aligned)
		{
			amr_reoffer_result_t result = amr_reoffer(fst_session, "true", AMR_FMTP_BE, AMR_FMTP_OA);

			fst_requires(result.negotiated);
			fst_xcheck(result.codec_reset, "a framing change resets the codec");
			fst_xcheck(result.decoded, "octet aligned frames decode after the re-offer");
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(amr_strict_reoffer_mode_set_change_keeps_codec)
		{
			amr_reoffer_result_t result = amr_reoffer(fst_session, "true", AMR_FMTP_OA, "octet-align=1; mode-set=4,5");

			fst_requires(result.negotiated);
			fst_xcheck(!result.codec_reset, "a mode-set change alone does not reset the codec");
			fst_xcheck(result.decoded, "octet aligned frames still decode");
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(amr_legacy_reoffer_keeps_codec)
		{
			amr_reoffer_result_t result = amr_reoffer(fst_session, NULL, AMR_FMTP_OA, AMR_FMTP_BE);

			fst_requires(result.negotiated);
			fst_xcheck(!result.codec_reset, "without strict matching the codec is kept across the re-offer");
		}
		FST_SESSION_END()

		FST_TEST_BEGIN(amr_decode) 
		{
			switch_codec_t read_codec = { 0 };
			switch_status_t status;
			switch_codec_settings_t codec_settings = {{ 0 }};
			uint32_t flags = 0;
			uint32_t rate;
			/*amr frame types*/
			static char no_data[] = "\x77\xc0";
			static char fail[] = "\x76\xc0";
			/*decode*/
			uint32_t decoded_len;
			unsigned char decbuf[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			switch_stream_handle_t stream = { 0 };

			status = switch_core_codec_init(&read_codec,
			"AMR",
			"mod_amr",
			NULL,
			8000,
			20,
			1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
			&codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			SWITCH_STANDARD_STREAM(stream);

			switch_api_execute("amr_debug", "on", NULL, &stream);

			switch_safe_free(stream.data);

			/*NO DATA = 0xf*/
			status = switch_core_codec_decode(&read_codec, NULL, &no_data, 2, 8000, &decbuf, &decoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			/*Invalid frame type*/
			status = switch_core_codec_decode(&read_codec, NULL, &fail, 2, 8000, &decbuf, &decoded_len, &rate, &flags);
			fst_check(status != SWITCH_STATUS_SUCCESS);

			switch_core_codec_destroy(&read_codec);
		}

		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
#endif 
