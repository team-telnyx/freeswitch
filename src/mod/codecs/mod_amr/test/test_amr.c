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
	int answer_octet_align;
	int stale_decode_failures;
	int framing_stale;
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

static const char *amr_offer_two_maps(switch_core_session_t *session, int version, const char *fmtp_96, const char *fmtp_97)
{
	return switch_core_session_sprintf(session,
		"v=0\r\n"
		"o=- 1 %d IN IP4 198.51.100.1\r\n"
		"s=-\r\n"
		"t=0 0\r\n"
		"m=audio 56210 RTP/AVP 96 97\r\n"
		"c=IN IP4 198.51.100.1\r\n"
		"a=rtpmap:96 AMR/8000\r\n"
		"a=fmtp:96 %s\r\n"
		"a=rtpmap:97 AMR/8000\r\n"
		"a=fmtp:97 %s\r\n"
		"a=sendrecv\r\n", version, fmtp_96, fmtp_97);
}

static int amr_answer_octet_align(switch_core_session_t *session)
{
	const char *sdp, *fmtp, *eol, *oa;

	switch_core_media_gen_local_sdp(session, SDP_ANSWER, "127.0.0.1", 12345, NULL, 1);

	if (!(sdp = switch_channel_get_variable(switch_core_session_get_channel(session), "rtp_local_sdp_str")) ||
		!(fmtp = strstr(sdp, "a=fmtp:"))) {
		return -1;
	}

	if (!(eol = strstr(fmtp, "\r\n"))) {
		eol = fmtp + strlen(fmtp);
	}

	if (!(oa = strstr(fmtp, "octet-align=")) || oa > eol) {
		return 0;
	}

	return atoi(oa + strlen("octet-align="));
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

static int amr_decode_failures(switch_codec_t *codec, switch_memory_pool_t *pool, const char *fmtp, int frames)
{
	switch_codec_t enc_codec = { 0 };
	switch_codec_settings_t codec_settings = {{ 0 }};
	unsigned char decoded[SWITCH_RECOMMENDED_BUFFER_SIZE];
	uint8_t pkt[64];
	int16_t pcm[160];
	uint32_t seed = 54321, pkt_len, decoded_len, rate, flags;
	int f, i, failures = 0;

	if (switch_core_codec_init(&enc_codec, "AMR", "mod_amr", fmtp, 8000, 20, 1,
							   SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, &codec_settings, pool) != SWITCH_STATUS_SUCCESS) {
		return -1;
	}

	for (f = 0; f < frames; f++) {
		for (i = 0; i < 160; i++) {
			seed = seed * 1103515245 + 12345;
			pcm[i] = (int16_t) ((int) ((seed >> 16) & 0x3fff) - 0x2000);
		}

		pkt_len = sizeof(pkt);
		rate = 8000;
		flags = 0;

		if (switch_core_codec_encode(&enc_codec, NULL, pcm, sizeof(pcm), 8000, pkt, &pkt_len, &rate, &flags) != SWITCH_STATUS_SUCCESS) {
			failures = -1;
			break;
		}

		decoded_len = sizeof(decoded);

		if (switch_core_codec_decode(codec, NULL, pkt, pkt_len, 8000, decoded, &decoded_len, &rate, &flags) != SWITCH_STATUS_SUCCESS) {
			failures++;
		}
	}

	switch_core_codec_destroy(&enc_codec);

	return failures;
}

static int amr_framing_changes(switch_codec_t *codec, const char *fmtp)
{
	switch_codec_control_type_t reply_type = SCCT_NONE;
	void *reply = NULL;

	return switch_core_codec_control(codec, SCC_CODEC_SPECIFIC, SCCT_STRING, (void *) "fmtp_changes_framing",
									 SCCT_STRING, (void *) fmtp, &reply_type, &reply) == SWITCH_STATUS_SUCCESS &&
		reply_type == SCCT_STRING && switch_true((const char *) reply);
}

static amr_reoffer_result_t amr_reoffer(switch_core_session_t *session, const char *strict,
										const char *first_fmtp, const char *second_offer, const char *frame_fmtp)
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

	if (switch_core_media_negotiate_sdp(session, second_offer, &p, SDP_OFFER) != 1 ||
		!(read_codec = switch_core_session_get_read_codec(session)) || !switch_core_codec_ready(read_codec)) {
		return result;
	}

	result.negotiated = 1;
	result.codec_reset = read_codec->private_info != initial_context;
	result.framing_stale = amr_framing_changes(read_codec, frame_fmtp);

	if (amr_encode_frame(pool, frame_fmtp, pkt, &pkt_len) == SWITCH_STATUS_SUCCESS && pkt_len > 20) {
		result.decoded = amr_decodes_as(read_codec, pool, frame_fmtp, pkt, pkt_len);
	}

	result.answer_octet_align = amr_answer_octet_align(session);
	result.stale_decode_failures = amr_decode_failures(read_codec, pool, first_fmtp, 50);

	return result;
}

static switch_xml_t amr_force_oa_config(const char *section, const char *tag_name, const char *key_name, const char *key_value,
										 switch_event_t *params, void *user_data)
{
	if (zstr(section) || strcmp(section, "configuration") || zstr(key_value) || strcmp(key_value, "amr.conf")) {
		return NULL;
	}

	return switch_xml_parse_str_dup(
		"<document type=\"freeswitch/xml\">"
		"<section name=\"configuration\">"
		"<configuration name=\"amr.conf\"><settings><param name=\"force-oa\" value=\"1\"/></settings></configuration>"
		"</section>"
		"</document>");
}

static switch_status_t amr_reload_module(void)
{
	const char *err = NULL;

	if (switch_loadable_module_unload_module(SWITCH_GLOBAL_dirs.mod_dir, "mod_amr", SWITCH_FALSE, &err) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	return switch_loadable_module_load_module(SWITCH_GLOBAL_dirs.mod_dir, "mod_amr", SWITCH_TRUE, &err);
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
			amr_reoffer_result_t result = amr_reoffer(fst_session, "true", AMR_FMTP_OA, amr_offer(fst_session, 2, AMR_FMTP_BE), AMR_FMTP_BE);

			fst_requires(result.negotiated);
			fst_xcheck(result.codec_reset, "a framing change resets the codec");
			fst_xcheck(result.decoded, "bandwidth efficient frames decode after the re-offer");
			fst_xcheck(result.answer_octet_align == 0, "the answer advertises bandwidth efficient framing");
			fst_xcheck(result.stale_decode_failures == 0, "octet aligned frames sent before the answer do not fail to decode");
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(amr_strict_reoffer_bandwidth_efficient_to_octet_aligned)
		{
			amr_reoffer_result_t result = amr_reoffer(fst_session, "true", AMR_FMTP_BE, amr_offer(fst_session, 2, AMR_FMTP_OA), AMR_FMTP_OA);

			fst_requires(result.negotiated);
			fst_xcheck(result.codec_reset, "a framing change resets the codec");
			fst_xcheck(result.decoded, "octet aligned frames decode after the re-offer");
			fst_xcheck(result.answer_octet_align == 1, "the answer advertises octet aligned framing");
			fst_xcheck(result.stale_decode_failures == 0, "bandwidth efficient frames sent before the answer do not fail to decode");
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(amr_strict_reoffer_mode_set_change_keeps_codec)
		{
			amr_reoffer_result_t result = amr_reoffer(fst_session, "true", AMR_FMTP_OA,
													  amr_offer(fst_session, 2, "octet-align=1; mode-set=4,5"), AMR_FMTP_OA);

			fst_requires(result.negotiated);
			fst_xcheck(!result.codec_reset, "a mode-set change alone does not reset the codec");
			fst_xcheck(result.decoded, "octet aligned frames still decode");
			fst_xcheck(result.answer_octet_align == 1, "the answer still advertises octet aligned framing");
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(amr_strict_reoffer_prefers_map_with_running_framing)
		{
			amr_reoffer_result_t result = amr_reoffer(fst_session, "true", AMR_FMTP_OA,
													  amr_offer_two_maps(fst_session, 2, AMR_FMTP_BE, AMR_FMTP_OA), AMR_FMTP_OA);

			fst_requires(result.negotiated);
			fst_xcheck(result.decoded, "octet aligned frames still decode");
			fst_xcheck(result.answer_octet_align == 1, "the answer keeps octet aligned framing");
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(amr_legacy_reoffer_keeps_stale_framing)
		{
			amr_reoffer_result_t result = amr_reoffer(fst_session, NULL, AMR_FMTP_OA, amr_offer(fst_session, 2, AMR_FMTP_BE), AMR_FMTP_BE);

			fst_requires(result.negotiated);
			fst_xcheck(!result.codec_reset, "without strict matching the codec is kept across the re-offer");
			fst_xcheck(result.framing_stale, "the kept codec still runs the old framing");
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

		FST_SESSION_BEGIN(amr_strict_reoffer_forced_framing_keeps_codec)
		{
			amr_reoffer_result_t result;

			switch_xml_bind_search_function(amr_force_oa_config, switch_xml_parse_section_string("configuration"), NULL);
			fst_requires(amr_reload_module() == SWITCH_STATUS_SUCCESS);

			result = amr_reoffer(fst_session, "true", AMR_FMTP_OA, amr_offer(fst_session, 2, AMR_FMTP_BE), AMR_FMTP_OA);
			switch_xml_unbind_search_function_ptr(amr_force_oa_config);

			fst_requires(result.negotiated);
			fst_xcheck(!result.codec_reset, "with force-oa an octet-align change does not reset the codec");
			fst_xcheck(result.decoded, "octet aligned frames still decode");
			fst_xcheck(result.answer_octet_align == 1, "the answer still advertises octet aligned framing");
		}
		FST_SESSION_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
#endif 
