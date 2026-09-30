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
 * test_amrwb.c -- tests mod_amrwb
 *
 */

#ifndef AMRWB_PASSTHROUGH
#include <switch.h>
#include <stdlib.h>
#include <math.h>

#include <test/switch_test.h>

static switch_status_t amrwb_init(switch_codec_t *codec, const char *fmtp, uint32_t flags, switch_memory_pool_t *pool)
{
	switch_codec_settings_t codec_settings = {{ 0 }};

	return switch_core_codec_init(codec, "AMR-WB", "mod_amrwb", fmtp, 16000, 20, 1, flags, &codec_settings, pool);
}

/* a fresh decoder decodes payload exactly as it decodes a lost frame */
static switch_bool_t amrwb_decodes_as_lost(const char *fmtp, const unsigned char *payload, uint32_t len, switch_memory_pool_t *pool)
{
	static const unsigned char lost_be[] = { 0xf7, 0x40 };
	static const unsigned char lost_oa[] = { 0xf0, 0x74 };
	switch_codec_t codec = { 0 }, reference = { 0 };
	unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 }, ref[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
	uint32_t out_len = sizeof(out), ref_len = sizeof(ref), rate = 16000;
	unsigned int flag = 0;
	switch_bool_t oa = strstr(fmtp, "octet-align=1") ? SWITCH_TRUE : SWITCH_FALSE;
	switch_status_t status, ref_status;

	amrwb_init(&codec, fmtp, SWITCH_CODEC_FLAG_DECODE, pool);
	amrwb_init(&reference, fmtp, SWITCH_CODEC_FLAG_DECODE, pool);
	status = switch_core_codec_decode(&codec, NULL, (void *) payload, len, 16000, out, &out_len, &rate, &flag);
	ref_status = switch_core_codec_decode(&reference, NULL, (void *) (oa ? lost_oa : lost_be), 2, 16000, ref, &ref_len, &rate, &flag);
	switch_core_codec_destroy(&codec);
	switch_core_codec_destroy(&reference);

	return (status == SWITCH_STATUS_SUCCESS && ref_status == SWITCH_STATUS_SUCCESS && out_len == 640 && out_len == ref_len &&
			!memcmp(out, ref, out_len)) ? SWITCH_TRUE : SWITCH_FALSE;
}

static void amrwb_adjust(switch_codec_t *codec, const char *how)
{
	switch_core_codec_control(codec, SCC_AUDIO_ADJUST_BITRATE, SCCT_STRING, (void *) how, SCCT_NONE, NULL, NULL, NULL);
}

/* frame type of the payload the encoder produces for 20 ms of silence */
static int amrwb_encoded_ft(switch_codec_t *codec, switch_bool_t octet_aligned)
{
	int16_t pcm[320] = { 0 };
	unsigned char encoded[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
	uint32_t encoded_len = sizeof(encoded), rate = 16000;
	unsigned int flag = 0;

	if (switch_core_codec_encode(codec, NULL, pcm, sizeof(pcm), 16000, encoded, &encoded_len, &rate, &flag) != SWITCH_STATUS_SUCCESS) {
		return -1;
	}

	return octet_aligned ? (encoded[1] >> 3) & 0x0f : ((encoded[0] & 0x07) << 1) | (encoded[1] >> 7);
}
FST_CORE_BEGIN(".")
{
	FST_SUITE_BEGIN(test_amrwb)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_loopback");
			fst_requires_module("mod_amrwb");
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		FST_TEST_BEGIN(amrwb_decode) 
		{
			switch_codec_t read_codec = { 0 };
			switch_status_t status;
			switch_codec_settings_t codec_settings = {{ 0 }};
			uint32_t flags = 0;
			uint32_t rate = 16000;
			/*amrwb frame types*/
			static char no_data[] = "\x77\xc0";
			static char speech_lost[] = "\x77\x00";
			static char fail[] = "\x76\xc0";
			/*decode*/
			uint32_t decoded_len = SWITCH_RECOMMENDED_BUFFER_SIZE;
			unsigned char decbuf[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			switch_stream_handle_t stream = { 0 };

			status = switch_core_codec_init(&read_codec,
			"AMR-WB",
			"mod_amrwb",
			NULL,
			16000,
			20,
			1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
			&codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			SWITCH_STANDARD_STREAM(stream);

			switch_api_execute("amrwb_debug", "on", NULL, &stream);

			switch_safe_free(stream.data);

			/*NO DATA = 0xf*/
			status = switch_core_codec_decode(&read_codec, NULL, &no_data, 2, 16000, &decbuf, &decoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			/*SPEECH LOST = 0xe*/
			status = switch_core_codec_decode(&read_codec, NULL, &speech_lost, 2, 16000, &decbuf, &decoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			/*Invalid frame type*/
			status = switch_core_codec_decode(&read_codec, NULL, &fail, 2, 16000, &decbuf, &decoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			switch_core_codec_destroy(&read_codec);
		}

		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_sid_transcode)
		{
			switch_codec_t source_be = { 0 };
			switch_codec_t source_oa = { 0 };
			switch_codec_t target_be = { 0 };
			switch_codec_t target_oa = { 0 };
			switch_codec_settings_t codec_settings = {{ 0 }};
			switch_status_t status;
			uint32_t flags = 0;
			uint32_t rate = 16000;
			unsigned char decoded[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			unsigned char encoded[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			unsigned char speech_oa[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			uint32_t decoded_len = sizeof(decoded);
			uint32_t encoded_len = sizeof(encoded);
			uint32_t speech_oa_len;
			static unsigned char sid_be[] = "\xf4\xf8\xf7\xcf\x78\x00\x80";
			static unsigned char sid_oa[] = "\xf0\x4c\xe3\xdf\x3d\xe0\x02";

			status = switch_core_codec_init(&source_be,
				"AMR-WB", "mod_amrwb", "mode-set=0,1,2;octet-align=0",
				16000, 20, 1, SWITCH_CODEC_FLAG_DECODE, &codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			status = switch_core_codec_init(&source_oa,
				"AMR-WB", "mod_amrwb", "mode-set=0,1,2;octet-align=1",
				16000, 20, 1, SWITCH_CODEC_FLAG_DECODE, &codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			status = switch_core_codec_init(&target_be,
				"AMR-WB", "mod_amrwb", "mode-set=0,1,2;octet-align=0",
				16000, 20, 1, SWITCH_CODEC_FLAG_ENCODE, &codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			status = switch_core_codec_init(&target_oa,
				"AMR-WB", "mod_amrwb", "mode-set=0,1,2;octet-align=1",
				16000, 20, 1, SWITCH_CODEC_FLAG_ENCODE, &codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			decoded_len = sizeof(decoded);
			status = switch_core_codec_decode(&source_be, NULL, sid_be, sizeof(sid_be) - 1,
				16000, decoded, &decoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			memset(encoded, 0, sizeof(encoded));
			encoded_len = sizeof(encoded);
			status = switch_core_codec_encode(&target_oa, &source_be, decoded, decoded_len,
				16000, encoded, &encoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check(encoded_len == sizeof(sid_oa) - 1);
			fst_check(!memcmp(encoded, sid_oa, sizeof(sid_oa) - 1));

			decoded[0] ^= 1;
			memset(encoded, 0, sizeof(encoded));
			encoded_len = sizeof(encoded);
			status = switch_core_codec_encode(&target_oa, &source_be, decoded, decoded_len,
				16000, encoded, &encoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check((encoded[1] >> 3 & 0x0f) != 9);
			decoded[0] ^= 1;
			memset(encoded, 0, sizeof(encoded));
			encoded_len = sizeof(encoded);
			status = switch_core_codec_encode(&target_be, &source_be, decoded, decoded_len,
				16000, encoded, &encoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check(encoded_len == sizeof(sid_be) - 1);
			fst_check(!memcmp(encoded, sid_be, sizeof(sid_be) - 1));

			decoded_len = sizeof(decoded);
			status = switch_core_codec_decode(&source_oa, NULL, sid_oa, sizeof(sid_oa) - 1,
				16000, decoded, &decoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			memset(encoded, 0, sizeof(encoded));
			encoded_len = sizeof(encoded);
			status = switch_core_codec_encode(&target_be, &source_oa, decoded, decoded_len,
				16000, encoded, &encoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check(encoded_len == sizeof(sid_be) - 1);
			fst_check(!memcmp(encoded, sid_be, sizeof(sid_be) - 1));

			speech_oa_len = sizeof(speech_oa);
			status = switch_core_codec_encode(&target_oa, NULL, decoded, decoded_len,
				16000, speech_oa, &speech_oa_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check((speech_oa[1] >> 3 & 0x0f) != 9);

			decoded_len = sizeof(decoded);
			status = switch_core_codec_decode(&source_oa, NULL, speech_oa, speech_oa_len,
				16000, decoded, &decoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			memset(encoded, 0, sizeof(encoded));
			encoded_len = sizeof(encoded);
			status = switch_core_codec_encode(&target_be, &source_oa, decoded, decoded_len,
				16000, encoded, &encoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check(encoded_len != sizeof(sid_be) - 1 || memcmp(encoded, sid_be, sizeof(sid_be) - 1));

			switch_core_codec_destroy(&target_oa);
			switch_core_codec_destroy(&target_be);
			switch_core_codec_destroy(&source_oa);
			switch_core_codec_destroy(&source_be);
		}

		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_conceals_multiple_frames)
		{
			switch_codec_t source_be = { 0 };
			switch_codec_t source_oa = { 0 };
			switch_codec_settings_t codec_settings = {{ 0 }};
			switch_status_t status;
			uint32_t flags = 0;
			uint32_t rate = 16000;
			unsigned char decoded[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			uint32_t decoded_len;
			static unsigned char multiframes_be[] = "\xfc\xf8\xf7\xcf\x78\x00\x80";
			static unsigned char multiframes_oa[] = "\xf0\xcc\x4c\xe3\xdf\x3d\xe0\x02\xe3\xdf\x3d\xe0\x02";

			status = switch_core_codec_init(&source_be,
				"AMR-WB", "mod_amrwb", "mode-set=0,1,2;octet-align=0",
				16000, 20, 1, SWITCH_CODEC_FLAG_DECODE, &codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			status = switch_core_codec_init(&source_oa,
				"AMR-WB", "mod_amrwb", "mode-set=0,1,2;octet-align=1",
				16000, 20, 1, SWITCH_CODEC_FLAG_DECODE, &codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			decoded_len = sizeof(decoded);
			status = switch_core_codec_decode(&source_be, NULL, multiframes_be, sizeof(multiframes_be) - 1,
				16000, decoded, &decoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			decoded_len = sizeof(decoded);
			status = switch_core_codec_decode(&source_oa, NULL, multiframes_oa, sizeof(multiframes_oa) - 1,
				16000, decoded, &decoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			switch_core_codec_destroy(&source_oa);
			switch_core_codec_destroy(&source_be);
		}

		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_truncated_sid_not_relayed)
		{
			switch_codec_t source_be = { 0 };
			switch_codec_t source_oa = { 0 };
			switch_codec_t target_oa = { 0 };
			switch_codec_settings_t codec_settings = {{ 0 }};
			switch_status_t status;
			uint32_t flags = 0;
			uint32_t rate = 16000;
			unsigned char decoded[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			unsigned char encoded[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			uint32_t decoded_len;
			uint32_t cached_pcm_len;
			uint32_t encoded_len;
			static unsigned char sid_be[] = "\xf4\xf8\xf7\xcf\x78\x00\x80";
			static unsigned char sid_oa[] = "\xf0\x4c\xe3\xdf\x3d\xe0\x02";
			static unsigned char short_payload[] = "\xf0";
			static unsigned char reserved_oa[] = "\xf0\x54";
			static unsigned char reserved_be_ft10[] = "\xf5\x40";

			status = switch_core_codec_init(&source_be,
				"AMR-WB", "mod_amrwb", "mode-set=0,1,2;octet-align=0",
				16000, 20, 1, SWITCH_CODEC_FLAG_DECODE, &codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			status = switch_core_codec_init(&source_oa,
				"AMR-WB", "mod_amrwb", "mode-set=0,1,2;octet-align=1",
				16000, 20, 1, SWITCH_CODEC_FLAG_DECODE, &codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			status = switch_core_codec_init(&target_oa,
				"AMR-WB", "mod_amrwb", "mode-set=0,1,2;octet-align=1",
				16000, 20, 1, SWITCH_CODEC_FLAG_ENCODE, &codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			decoded_len = sizeof(decoded);
			status = switch_core_codec_decode(&source_be, NULL, sid_be, sizeof(sid_be) - 1,
				16000, decoded, &decoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			cached_pcm_len = decoded_len;

			decoded_len = sizeof(decoded);
			status = switch_core_codec_decode(&source_be, NULL, sid_be, sizeof(sid_be) - 2,
				16000, decoded, &decoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			decoded_len = sizeof(decoded);
			status = switch_core_codec_decode(&source_be, NULL, short_payload, sizeof(short_payload) - 1,
				16000, decoded, &decoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			decoded_len = sizeof(decoded);
			status = switch_core_codec_decode(&source_be, NULL, reserved_be_ft10, sizeof(reserved_be_ft10) - 1,
				16000, decoded, &decoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			decoded_len = cached_pcm_len;
			encoded_len = sizeof(encoded);
			status = switch_core_codec_encode(&target_oa, &source_be, decoded, decoded_len,
				16000, encoded, &encoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check((encoded[1] >> 3 & 0x0f) != 9);

			decoded_len = sizeof(decoded);
			status = switch_core_codec_decode(&source_oa, NULL, sid_oa, sizeof(sid_oa) - 1,
				16000, decoded, &decoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			cached_pcm_len = decoded_len;

			decoded_len = sizeof(decoded);
			status = switch_core_codec_decode(&source_oa, NULL, sid_oa, sizeof(sid_oa) - 2,
				16000, decoded, &decoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			decoded_len = sizeof(decoded);
			status = switch_core_codec_decode(&source_oa, NULL, short_payload, sizeof(short_payload) - 1,
				16000, decoded, &decoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			decoded_len = sizeof(decoded);
			status = switch_core_codec_decode(&source_oa, NULL, reserved_oa, sizeof(reserved_oa) - 1,
				16000, decoded, &decoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			decoded_len = cached_pcm_len;
			encoded_len = sizeof(encoded);
			status = switch_core_codec_encode(&target_oa, &source_oa, decoded, decoded_len,
				16000, encoded, &encoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check((encoded[1] >> 3 & 0x0f) != 9);

			switch_core_codec_destroy(&target_oa);
			switch_core_codec_destroy(&source_oa);
			switch_core_codec_destroy(&source_be);
		}

		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_sid_advances_encoder_state)
		{
			switch_codec_t source_be = { 0 };
			switch_codec_t target_relay = { 0 };
			switch_codec_t target_control = { 0 };
			switch_codec_settings_t codec_settings = {{ 0 }};
			switch_status_t status;
			uint32_t flags = 0;
			uint32_t rate = 16000;
			unsigned char decoded[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			unsigned char relayed[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			unsigned char control[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			uint32_t decoded_len = sizeof(decoded);
			uint32_t relayed_len;
			uint32_t control_len;
			static unsigned char sid_be[] = "\xf4\xf8\xf7\xcf\x78\x00\x80";

			status = switch_core_codec_init(&source_be,
				"AMR-WB", "mod_amrwb", "mode-set=0,1,2;octet-align=0",
				16000, 20, 1, SWITCH_CODEC_FLAG_DECODE, &codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			status = switch_core_codec_init(&target_relay,
				"AMR-WB", "mod_amrwb", "mode-set=0,1,2;octet-align=1",
				16000, 20, 1, SWITCH_CODEC_FLAG_ENCODE, &codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			status = switch_core_codec_init(&target_control,
				"AMR-WB", "mod_amrwb", "mode-set=0,1,2;octet-align=1",
				16000, 20, 1, SWITCH_CODEC_FLAG_ENCODE, &codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			status = switch_core_codec_decode(&source_be, NULL, sid_be, sizeof(sid_be) - 1,
				16000, decoded, &decoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			relayed_len = sizeof(relayed);
			status = switch_core_codec_encode(&target_relay, &source_be, decoded, decoded_len,
				16000, relayed, &relayed_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check((relayed[1] >> 3 & 0x0f) == 9);
			control_len = sizeof(control);
			status = switch_core_codec_encode(&target_control, NULL, decoded, decoded_len,
				16000, control, &control_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			memset(decoded, 0, decoded_len);
			relayed_len = sizeof(relayed);
			control_len = sizeof(control);
			status = switch_core_codec_encode(&target_relay, NULL, decoded, decoded_len,
				16000, relayed, &relayed_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			status = switch_core_codec_encode(&target_control, NULL, decoded, decoded_len,
				16000, control, &control_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check(relayed_len == control_len);
			fst_check(!memcmp(relayed, control, control_len));

			switch_core_codec_destroy(&target_control);
			switch_core_codec_destroy(&target_relay);
			switch_core_codec_destroy(&source_be);
		}

		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_mode_set_keeps_mode_8)
		{
			switch_codec_t codec = { 0 };

			fst_requires(amrwb_init(&codec, "mode-set=2,8;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "mode-set=2,8"));
			fst_check_int_equals(amrwb_encoded_ft(&codec, SWITCH_TRUE), 8);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_mode_set_ignores_invalid_modes)
		{
			switch_codec_t codec = { 0 };

			fst_requires(amrwb_init(&codec, "mode-set=9,15;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && !strstr(codec.fmtp_out, "mode-set=;") && !strstr(codec.fmtp_out, "9") && !strstr(codec.fmtp_out, "15"));
			fst_check(amrwb_encoded_ft(&codec, SWITCH_TRUE) <= 8);
			switch_core_codec_destroy(&codec);

			fst_requires(amrwb_init(&codec, "mode-set=1,9;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "mode-set=1;"));
			fst_check_int_equals(amrwb_encoded_ft(&codec, SWITCH_TRUE), 1);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_answer_keeps_octet_align_with_fmtp_extra)
		{
			switch_codec_t codec = { 0 };

			fst_requires(amrwb_init(&codec, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "octet-align=1"));
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "x-extra=1"));
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_conceals_undecodable_payloads)
		{
			static const unsigned char short_payload[] = { 0xf0 };
			static const unsigned char reserved_be[] = { 0xf5, 0x40 };
			static const unsigned char reserved_oa[] = { 0xf0, 0x54 };
			static const unsigned char multi_be[] = { 0xfc, 0xf8, 0xf7, 0xcf, 0x78, 0x00, 0x80 };
			static const unsigned char multi_oa[] = { 0xf0, 0xcc, 0x4c, 0xe3, 0xdf, 0x3d, 0xe0, 0x02, 0xe3, 0xdf, 0x3d, 0xe0, 0x02 };
			static const unsigned char truncated_sid_be[] = { 0xf4, 0xf8, 0xf7, 0xcf, 0x78, 0x00 };
			unsigned char oversized[70] = { 0xf0, 0x44 };

			fst_check(amrwb_decodes_as_lost("octet-align=0", short_payload, sizeof(short_payload), fst_pool));
			fst_check(amrwb_decodes_as_lost("octet-align=0", reserved_be, sizeof(reserved_be), fst_pool));
			fst_check(amrwb_decodes_as_lost("octet-align=1", reserved_oa, sizeof(reserved_oa), fst_pool));
			fst_check(amrwb_decodes_as_lost("octet-align=0", multi_be, sizeof(multi_be), fst_pool));
			fst_check(amrwb_decodes_as_lost("octet-align=1", multi_oa, sizeof(multi_oa), fst_pool));
			fst_check(amrwb_decodes_as_lost("octet-align=0", truncated_sid_be, sizeof(truncated_sid_be), fst_pool));
			fst_check(amrwb_decodes_as_lost("octet-align=1", oversized, sizeof(oversized), fst_pool));
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_does_not_match_unsupported_payload_options)
		{
			switch_codec_interface_t *codec_interface = switch_loadable_module_get_codec_interface("AMR-WB", "mod_amrwb");
			const switch_codec_implementation_t *impl;
			int matched = 0, unsupported_matched = 0;

			fst_requires(codec_interface);
			for (impl = codec_interface->implementations; impl; impl = impl->next) {
				if (!impl->matches_fmtp) {
					continue;
				}
				matched += impl->matches_fmtp("octet-align=1", impl->fmtp) == SWITCH_STATUS_SUCCESS;
				unsupported_matched += impl->matches_fmtp("octet-align=1;crc=1", impl->fmtp) == SWITCH_STATUS_SUCCESS;
				unsupported_matched += impl->matches_fmtp("octet-align=1;robust-sorting=1", impl->fmtp) == SWITCH_STATUS_SUCCESS;
				unsupported_matched += impl->matches_fmtp("octet-align=1;interleaving=4", impl->fmtp) == SWITCH_STATUS_SUCCESS;
				unsupported_matched += impl->matches_fmtp("octet-align=1;channels=2", impl->fmtp) == SWITCH_STATUS_SUCCESS;
				unsupported_matched += impl->matches_fmtp(impl->fmtp, "octet-align=1;crc=1") == SWITCH_STATUS_SUCCESS;
				unsupported_matched += impl->matches_fmtp(impl->fmtp, "octet-align=1;interleaving=4") == SWITCH_STATUS_SUCCESS;
			}
			UNPROTECT_INTERFACE(codec_interface);

			fst_check(matched > 0);
			fst_check_int_equals(unsupported_matched, 0);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_negotiated_octet_align_wins_over_force)
		{
			switch_codec_t codec = { 0 };
			int16_t pcm[320] = { 0 };
			unsigned char encoded[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			uint32_t encoded_len = sizeof(encoded), rate = 16000;
			unsigned int flag = 0;

			fst_requires(amrwb_init(&codec, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "octet-align=1"));
			fst_requires(switch_core_codec_encode(&codec, NULL, pcm, sizeof(pcm), 16000, encoded, &encoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(encoded_len, 34);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_octet_align_parsed_like_the_matcher)
		{
			switch_codec_t codec = { 0 };
			int16_t pcm[320] = { 0 };
			unsigned char encoded[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			uint32_t encoded_len = sizeof(encoded), rate = 16000;
			unsigned int flag = 0;

			fst_requires(amrwb_init(&codec, "mode-set=2;octet-align=true", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_codec_encode(&codec, NULL, pcm, sizeof(pcm), 16000, encoded, &encoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(encoded_len, 34);
			switch_core_codec_destroy(&codec);

			encoded_len = sizeof(encoded);
			fst_requires(amrwb_init(&codec, "mode-set=2;octet-align= 1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_codec_encode(&codec, NULL, pcm, sizeof(pcm), 16000, encoded, &encoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(encoded_len, 34);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_parses_long_fmtp)
		{
			switch_codec_t codec = { 0 };
			int16_t pcm[320] = { 0 };
			unsigned char encoded[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			uint32_t encoded_len = sizeof(encoded), rate = 16000;
			unsigned int flag = 0;

			fst_requires(amrwb_init(&codec, "a=1;b=1;c=1;d=1;e=1;f=1;g=1;h=1;i=1;j=1;k=1;mode-set=2;octet-align=1",
									SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "mode-set=2;"));
			fst_requires(switch_core_codec_encode(&codec, NULL, pcm, sizeof(pcm), 16000, encoded, &encoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(encoded_len, 34);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_q0_frame_is_decoded_as_bad)
		{
			switch_codec_t encoder = { 0 }, damaged = { 0 }, lost = { 0 };
			static const unsigned char lost_oa[] = { 0xf0, 0x74 };
			int16_t pcm[320];
			unsigned char frames[6][64];
			uint32_t frame_len[6];
			unsigned char out_damaged[SWITCH_RECOMMENDED_BUFFER_SIZE], out_lost[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t len_damaged = 0, len_lost = 0, rate = 16000;
			unsigned int flag = 0;
			int f, i;

			fst_requires(amrwb_init(&encoder, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(amrwb_init(&damaged, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(amrwb_init(&lost, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);

			for (f = 0; f < 6; f++) {
				for (i = 0; i < 320; i++) {
					pcm[i] = (int16_t) (8000 * sin(2 * M_PI * 440 * (f * 320 + i) / 16000.0));
				}
				frame_len[f] = sizeof(frames[f]);
				fst_requires(switch_core_codec_encode(&encoder, NULL, pcm, sizeof(pcm), 16000, frames[f], &frame_len[f], &rate, &flag) == SWITCH_STATUS_SUCCESS);
			}
			frames[5][1] &= ~0x04;

			/* same history, then the Q=0 frame vs a lost frame */
			for (f = 0; f < 6; f++) {
				len_damaged = sizeof(out_damaged);
				fst_requires(switch_core_codec_decode(&damaged, NULL, frames[f], frame_len[f], 16000, out_damaged, &len_damaged, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				len_lost = sizeof(out_lost);
				if (f < 5) {
					fst_requires(switch_core_codec_decode(&lost, NULL, frames[f], frame_len[f], 16000, out_lost, &len_lost, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				} else {
					fst_requires(switch_core_codec_decode(&lost, NULL, (void *) lost_oa, sizeof(lost_oa), 16000, out_lost, &len_lost, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				}
			}

			fst_check(len_damaged == len_lost && !memcmp(out_damaged, out_lost, len_lost));

			switch_core_codec_destroy(&lost);
			switch_core_codec_destroy(&damaged);
			switch_core_codec_destroy(&encoder);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_bitrate_adjustment_stays_in_mode_set)
		{
			switch_codec_t codec = { 0 };

			fst_requires(amrwb_init(&codec, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(amrwb_encoded_ft(&codec, SWITCH_TRUE), 2);
			amrwb_adjust(&codec, "increase");
			fst_check_int_equals(amrwb_encoded_ft(&codec, SWITCH_TRUE), 2);
			amrwb_adjust(&codec, "decrease");
			fst_check_int_equals(amrwb_encoded_ft(&codec, SWITCH_TRUE), 1);
			amrwb_adjust(&codec, "decrease");
			fst_check_int_equals(amrwb_encoded_ft(&codec, SWITCH_TRUE), 0);
			amrwb_adjust(&codec, "decrease");
			fst_check_int_equals(amrwb_encoded_ft(&codec, SWITCH_TRUE), 0);
			amrwb_adjust(&codec, "increase");
			fst_check_int_equals(amrwb_encoded_ft(&codec, SWITCH_TRUE), 1);
			amrwb_adjust(&codec, "default");
			fst_check(amrwb_encoded_ft(&codec, SWITCH_TRUE) <= 2);
			amrwb_adjust(&codec, "minimum");
			fst_check_int_equals(amrwb_encoded_ft(&codec, SWITCH_TRUE), 0);
			switch_core_codec_destroy(&codec);

			fst_requires(amrwb_init(&codec, "mode-set=1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			amrwb_adjust(&codec, "minimum");
			fst_check_int_equals(amrwb_encoded_ft(&codec, SWITCH_TRUE), 1);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_encoder_follows_received_cmr)
		{
			switch_codec_t codec = { 0 };
			static const unsigned char cmr1_oa[] = { 0x10, 0x7c };
			static const unsigned char cmr15_oa[] = { 0xf0, 0x7c };
			static const unsigned char cmr8_oa[] = { 0x80, 0x7c };
			static const unsigned char cmr9_oa[] = { 0x90, 0x7c };
			static const unsigned char cmr12_oa[] = { 0xc0, 0x7c };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len, rate = 16000;
			unsigned int flag = 0;

			fst_requires(amrwb_init(&codec, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(amrwb_encoded_ft(&codec, SWITCH_TRUE), 2);

			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr1_oa, sizeof(cmr1_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(amrwb_encoded_ft(&codec, SWITCH_TRUE), 1);

			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr8_oa, sizeof(cmr8_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(amrwb_encoded_ft(&codec, SWITCH_TRUE), 2);

			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr1_oa, sizeof(cmr1_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(amrwb_encoded_ft(&codec, SWITCH_TRUE), 1);

			/* reserved CMR 9-14: ignored, the previous request stays */
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr9_oa, sizeof(cmr9_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(amrwb_encoded_ft(&codec, SWITCH_TRUE), 1);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr12_oa, sizeof(cmr12_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(amrwb_encoded_ft(&codec, SWITCH_TRUE), 1);

			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr15_oa, sizeof(cmr15_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(amrwb_encoded_ft(&codec, SWITCH_TRUE), 2);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_encoder_mode_matches_answered_mode_set)
		{
			switch_codec_t codec = { 0 };

			/* offer without mode-set, mode-set-overwrite off: answer and encode within the configured mode-set */
			fst_requires(amrwb_init(&codec, "octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "mode-set=0,1,2;"));
			fst_check_int_equals(amrwb_encoded_ft(&codec, SWITCH_TRUE), 2);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_relayed_sid_keeps_q_bit_in_be)
		{
			switch_codec_t source_oa = { 0 }, target_be = { 0 };
			static const unsigned char sid_oa_q0[] = { 0xf0, 0x48, 0xe3, 0xdf, 0x3d, 0xe0, 0x02 };
			unsigned char decoded[SWITCH_RECOMMENDED_BUFFER_SIZE], encoded[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			uint32_t decoded_len = sizeof(decoded), encoded_len = sizeof(encoded), rate = 16000;
			unsigned int flag = 0;

			fst_requires(amrwb_init(&source_oa, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(amrwb_init(&target_be, "mode-set=0,1,2;octet-align=0", SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_codec_decode(&source_oa, NULL, (void *) sid_oa_q0, sizeof(sid_oa_q0), 16000, decoded, &decoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_codec_encode(&target_be, &source_oa, decoded, decoded_len, 16000, encoded, &encoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(((encoded[0] & 0x07) << 1) | (encoded[1] >> 7), 9);
			fst_check_int_equals((encoded[1] >> 6) & 1, 0);
			switch_core_codec_destroy(&target_be);
			switch_core_codec_destroy(&source_oa);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_relayed_sid_carries_target_mode)
		{
			switch_codec_t source_oa = { 0 }, target_oa = { 0 };
			static const unsigned char sid_oa[] = { 0xf0, 0x4c, 0xe3, 0xdf, 0x3d, 0xe0, 0x02 };
			unsigned char decoded[SWITCH_RECOMMENDED_BUFFER_SIZE], encoded[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			uint32_t decoded_len = sizeof(decoded), encoded_len = sizeof(encoded), rate = 16000;
			unsigned int flag = 0;

			fst_requires(amrwb_init(&source_oa, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(amrwb_init(&target_oa, "mode-set=0,1;octet-align=1", SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_codec_decode(&source_oa, NULL, (void *) sid_oa, sizeof(sid_oa), 16000, decoded, &decoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_codec_encode(&target_oa, &source_oa, decoded, decoded_len, 16000, encoded, &encoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_requires(encoded_len == sizeof(sid_oa));
			fst_check_int_equals((encoded[1] >> 3) & 0x0f, 9);
			fst_check_int_equals(encoded[6] & 0x0f, 1);
			fst_check(!memcmp(encoded + 2, sid_oa + 2, 4) && (encoded[6] & 0xf0) == (sid_oa[6] & 0xf0));
			switch_core_codec_destroy(&target_oa);
			switch_core_codec_destroy(&source_oa);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_control_and_destroy_without_context)
		{
			switch_codec_t codec = { 0 };
			int32_t level = 1;

			amrwb_init(&codec, "octet-align=1", 0, fst_pool);
			switch_core_codec_control(&codec, SCC_DEBUG, SCCT_INT, &level, SCCT_NONE, NULL, NULL, NULL);
			amrwb_adjust(&codec, "increase");
			switch_core_codec_destroy(&codec);
			fst_check(1);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_show_prints_config_literally)
		{
			switch_stream_handle_t stream = { 0 };
			const char *literal = "x-pct=%s%s%s";

			SWITCH_STANDARD_STREAM(stream);
			switch_api_execute("amrwb_show", "", NULL, &stream);
			fst_check(stream.data && strstr((char *) stream.data, literal));
			switch_safe_free(stream.data);
		}
		FST_TEST_END()

	}
	FST_SUITE_END()
}
FST_CORE_END()
#endif 
