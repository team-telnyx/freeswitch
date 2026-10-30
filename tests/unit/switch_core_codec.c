/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2018, Anthony Minessale II <anthm@freeswitch.org>
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
 * Chris Rienzo <chris@signalwire.com>
 * Seven Du <dujinfang@gmail.com>
 * Dragos Oancea <dragos@signalwire.com>
 *
 * switch_core_codec.c -- tests codec core functions
 *
 */
#include <switch.h>
#include <stdlib.h>

#include <test/switch_test.h>

/* a codec with fmtp matching and several implementations, as AMR and AMR-WB register */
static switch_status_t fmtp_test_matches(const char *fmtp, const char *codec_fmtp)
{
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t fmtp_test_init(switch_codec_t *codec, switch_codec_flag_t flags, const switch_codec_settings_t *codec_settings)
{
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t fmtp_test_encode(switch_codec_t *codec, switch_codec_t *other_codec, void *decoded_data, uint32_t decoded_data_len,
										uint32_t decoded_rate, void *encoded_data, uint32_t *encoded_data_len, uint32_t *encoded_rate, unsigned int *flag)
{
	return SWITCH_STATUS_FALSE;
}

static switch_status_t fmtp_test_decode(switch_codec_t *codec, switch_codec_t *other_codec, void *encoded_data, uint32_t encoded_data_len,
										uint32_t encoded_rate, void *decoded_data, uint32_t *decoded_data_len, uint32_t *decoded_rate, unsigned int *flag)
{
	return SWITCH_STATUS_FALSE;
}

static switch_status_t fmtp_test_destroy(switch_codec_t *codec)
{
	return SWITCH_STATUS_SUCCESS;
}

/* plain codecs (no fmtp matching) with a single implementation: SLOW60 at 60 ms, FAST20 at 20 ms */
static switch_status_t ptime_test_load(switch_loadable_module_interface_t **module_interface, switch_memory_pool_t *pool)
{
	switch_codec_interface_t *codec_interface;

	*module_interface = switch_loadable_module_create_module_interface(pool, "mod_ptime_test");
	SWITCH_ADD_CODEC(codec_interface, "SLOW60");
	switch_core_codec_add_implementation(pool, codec_interface, SWITCH_CODEC_TYPE_AUDIO, 102, "SLOW60", NULL,
										 8000, 8000, 64000, 60000, 480, 960, 0, 1, 1,
										 fmtp_test_init, fmtp_test_encode, fmtp_test_decode, fmtp_test_destroy);
	SWITCH_ADD_CODEC(codec_interface, "FAST20");
	switch_core_codec_add_implementation(pool, codec_interface, SWITCH_CODEC_TYPE_AUDIO, 103, "FAST20", NULL,
										 8000, 8000, 64000, 20000, 160, 320, 0, 1, 1,
										 fmtp_test_init, fmtp_test_encode, fmtp_test_decode, fmtp_test_destroy);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t fmtp_test_load(switch_loadable_module_interface_t **module_interface, switch_memory_pool_t *pool)
{
	switch_codec_interface_t *codec_interface;
	int pt;

	*module_interface = switch_loadable_module_create_module_interface(pool, "mod_fmtp_test");
	SWITCH_ADD_CODEC(codec_interface, "FMTPTEST");
	for (pt = 96; pt < 100; pt++) {
		switch_core_codec_add_implementation(pool, codec_interface, SWITCH_CODEC_TYPE_AUDIO, pt, "FMTPTEST", NULL,
											 8000, 8000, 12200, 20000, 160, 320, 0, 1, 1,
											 fmtp_test_init, fmtp_test_encode, fmtp_test_decode, fmtp_test_destroy);
		codec_interface->implementations->matches_fmtp = fmtp_test_matches;
	}
	/* and a plain codec with one implementation */
	SWITCH_ADD_CODEC(codec_interface, "PLAINTEST");
	switch_core_codec_add_implementation(pool, codec_interface, SWITCH_CODEC_TYPE_AUDIO, 101, "PLAINTEST", NULL,
										 8000, 8000, 64000, 20000, 160, 320, 0, 1, 1,
										 fmtp_test_init, fmtp_test_encode, fmtp_test_decode, fmtp_test_destroy);
	return SWITCH_STATUS_SUCCESS;
}

FST_CORE_BEGIN("./conf")
{
	FST_SUITE_BEGIN(switch_core_codec)
	{
		FST_SETUP_BEGIN()
		{
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_opus");
			fst_requires_module("mod_spandsp");
		}
		FST_SETUP_END()

		FST_TEST_BEGIN(test_switch_core_codec_copy)
		{
			switch_codec_t orig_codec = { 0 };
			switch_codec_t new_codec = { 0 };
			switch_status_t status;
			switch_codec_settings_t codec_settings = {{ 0 }};
			status = switch_core_codec_init(&orig_codec,
			"OPUS",
			"mod_opus",
			NULL,
			48000,
			20,
			1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
			&codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			switch_core_codec_copy(&orig_codec, &new_codec, NULL, NULL);
			fst_check(orig_codec.implementation->samples_per_second == new_codec.implementation->samples_per_second);
			fst_check(orig_codec.implementation->actual_samples_per_second == new_codec.implementation->actual_samples_per_second);
			switch_core_codec_destroy(&orig_codec);
			switch_core_codec_destroy(&new_codec);

			status = switch_core_codec_init(&orig_codec,
			"OPUS",
			"mod_opus",
			NULL,
			16000,
			20,
			1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
			&codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			switch_core_codec_copy(&orig_codec, &new_codec, NULL, NULL);
			fst_check(orig_codec.implementation->samples_per_second == new_codec.implementation->samples_per_second);
			fst_check(orig_codec.implementation->actual_samples_per_second == new_codec.implementation->actual_samples_per_second);
			switch_core_codec_destroy(&orig_codec);
			switch_core_codec_destroy(&new_codec);

			status = switch_core_codec_init(&orig_codec,
			"OPUS",
			"mod_opus",
			NULL,
			8000,
			20,
			1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
			&codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			switch_core_codec_copy(&orig_codec, &new_codec, NULL, NULL);
			fst_check(orig_codec.implementation->samples_per_second == new_codec.implementation->samples_per_second);
			fst_check(orig_codec.implementation->actual_samples_per_second == new_codec.implementation->actual_samples_per_second);
			switch_core_codec_destroy(&orig_codec);
			switch_core_codec_destroy(&new_codec);
 
			status = switch_core_codec_init(&orig_codec,
			"G722",
			"mod_spandsp",
			NULL,
			8000,
			20,
			1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
			&codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			switch_core_codec_copy(&orig_codec, &new_codec, NULL, NULL);
			fst_check(orig_codec.implementation->samples_per_second == new_codec.implementation->samples_per_second);
			fst_check(orig_codec.implementation->actual_samples_per_second == new_codec.implementation->actual_samples_per_second);
			switch_core_codec_destroy(&orig_codec);
			switch_core_codec_destroy(&new_codec);

		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_mod_opus_switch_status_noop)
		{
			signed char outbuf[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			uint32_t decoded_len = 0;
			uint32_t decoded_rate = 48000;
			unsigned int flags = 0;
			switch_codec_t orig_codec = { 0 };
			switch_status_t status;
			switch_codec_settings_t codec_settings = { { 0 } };
			status = switch_core_codec_init(&orig_codec,
											"OPUS",
											"mod_opus",
											NULL,
											48000,
											20,
											1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
											&codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			status = switch_core_codec_decode(&orig_codec, NULL, "test", 5, 48000, outbuf, &decoded_len, &decoded_rate, &flags);
			// On ENGDESK-42254 we have change SWITCH_STATUS_FALSE to SWITCH_STATUS_NOOP to fix customer issue, so this test should check SWITCH_STATUS_NOOP
			fst_check_int_equals(status, SWITCH_STATUS_NOOP);
			switch_core_codec_destroy(&orig_codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_l24_codec_init)
		{
			switch_codec_t codec = { 0 };
			switch_status_t status;
			switch_codec_settings_t codec_settings = {{ 0 }};

			/* Test L24 at 48000 Hz */
			status = switch_core_codec_init(&codec,
											"L24",
											NULL,
											NULL,
											48000,
											20,
											1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
											&codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check(codec.implementation != NULL);
			fst_check(codec.implementation->ianacode == 96);
			fst_check(codec.implementation->actual_samples_per_second == 48000);
			fst_check(codec.implementation->decoded_bytes_per_packet == 1920); /* 960 samples * 2 bytes (16-bit internal) */
			fst_check(codec.implementation->encoded_bytes_per_packet == 2880); /* 960 samples * 3 bytes (24-bit wire) */
			switch_core_codec_destroy(&codec);

			/* Test L24 at 8000 Hz */
			status = switch_core_codec_init(&codec,
											"L24",
											NULL,
											NULL,
											8000,
											20,
											1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
											&codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check(codec.implementation->actual_samples_per_second == 8000);
			fst_check(codec.implementation->decoded_bytes_per_packet == 320); /* 160 samples * 2 bytes */
			fst_check(codec.implementation->encoded_bytes_per_packet == 480); /* 160 samples * 3 bytes */
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_l24_encode_decode)
		{
			switch_codec_t codec = { 0 };
			switch_status_t status;
			switch_codec_settings_t codec_settings = {{ 0 }};
			int16_t input_samples[960] = { 0 };
			uint8_t encoded_data[2880] = { 0 };
			int16_t decoded_samples[960] = { 0 };
			uint32_t encoded_len = sizeof(encoded_data);
			uint32_t decoded_len = sizeof(decoded_samples);
			uint32_t encoded_rate = 48000;
			uint32_t decoded_rate = 48000;
			unsigned int flag = 0;
			int i;

			/* Initialize codec */
			status = switch_core_codec_init(&codec,
											"L24",
											NULL,
											NULL,
											48000,
											20,
											1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
											&codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			/* Create test pattern: ascending 16-bit values */
			for (i = 0; i < 960; i++) {
				input_samples[i] = (int16_t)(i * 32);
			}

			/* Encode 16-bit samples to 24-bit */
			status = switch_core_codec_encode(&codec,
											  NULL,
											  input_samples,
											  1920, /* 960 samples * 2 bytes */
											  48000,
											  encoded_data,
											  &encoded_len,
											  &encoded_rate,
											  &flag);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check(encoded_len == 2880); /* 960 samples * 3 bytes */

			/* Verify 24-bit encoding (big-endian format) */
			/* First sample: input_samples[0] = 0x0000 -> 24-bit = 0x000000 */
			fst_check(encoded_data[0] == 0x00);
			fst_check(encoded_data[1] == 0x00);
			fst_check(encoded_data[2] == 0x00);

			/* Second sample: input_samples[1] = 0x0020 (32) -> 24-bit = 0x002000 */
			fst_check(encoded_data[3] == 0x00);
			fst_check(encoded_data[4] == 0x20);
			fst_check(encoded_data[5] == 0x00);

			/* Decode 24-bit samples back to 16-bit */
			status = switch_core_codec_decode(&codec,
											  NULL,
											  encoded_data,
											  encoded_len,
											  48000,
											  decoded_samples,
											  &decoded_len,
											  &decoded_rate,
											  &flag);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check(decoded_len == 1920); /* 960 samples * 2 bytes */

			/* Verify decoded samples match input */
			for (i = 0; i < 960; i++) {
				fst_check(decoded_samples[i] == input_samples[i]);
			}

			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_l24_negative_samples)
		{
			switch_codec_t codec = { 0 };
			switch_status_t status;
			switch_codec_settings_t codec_settings = {{ 0 }};
			int16_t input_samples[160] = { 0 };
			uint8_t encoded_data[480] = { 0 };
			int16_t decoded_samples[160] = { 0 };
			uint32_t encoded_len = sizeof(encoded_data);
			uint32_t decoded_len = sizeof(decoded_samples);
			uint32_t encoded_rate = 8000;
			uint32_t decoded_rate = 8000;
			unsigned int flag = 0;

			/* Initialize codec at 8kHz */
			status = switch_core_codec_init(&codec,
											"L24",
											NULL,
											NULL,
											8000,
											20,
											1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
											&codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			/* Test negative values and edge cases */
			input_samples[0] = 0x7FFF;   /* Max positive */
			input_samples[1] = -32768;   /* Min negative (0x8000) */
			input_samples[2] = -1;       /* 0xFFFF */
			input_samples[3] = 1000;
			input_samples[4] = -1000;

			/* Encode */
			status = switch_core_codec_encode(&codec,
											  NULL,
											  input_samples,
											  320,
											  8000,
											  encoded_data,
											  &encoded_len,
											  &encoded_rate,
											  &flag);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			/* Verify sign extension in 24-bit format */
			/* Max positive: 0x7FFF -> 0x7FFF00 */
			fst_check(encoded_data[0] == 0x7F);
			fst_check(encoded_data[1] == 0xFF);
			fst_check(encoded_data[2] == 0x00);

			/* Min negative: 0x8000 -> 0x800000 */
			fst_check(encoded_data[3] == 0x80);
			fst_check(encoded_data[4] == 0x00);
			fst_check(encoded_data[5] == 0x00);

			/* Decode */
			status = switch_core_codec_decode(&codec,
											  NULL,
											  encoded_data,
											  encoded_len,
											  8000,
											  decoded_samples,
											  &decoded_len,
											  &decoded_rate,
											  &flag);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			/* Verify decoded values */
			fst_check(decoded_samples[0] == 0x7FFF);
			fst_check(decoded_samples[1] == -32768);
			fst_check(decoded_samples[2] == -1);
			fst_check(decoded_samples[3] == 1000);
			fst_check(decoded_samples[4] == -1000);

			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* the near-match lookups pass a one-slot array: of a codec with fmtp matching and several
		 * implementations only as many as fit are returned */
		FST_TEST_BEGIN(test_get_codecs_sorted_stays_in_the_array)
		{
			struct {
				const switch_codec_implementation_t *search[1];
				const switch_codec_implementation_t *guard[4];
			} slots;
			char fmtp[SWITCH_MAX_CODECS][MAX_FMTP_LEN] = { { 0 } };
			char *prefs[1] = { "FMTPTEST" };
			char *plain_first[2] = { "PLAINTEST", "FMTPTEST" };
			const switch_codec_implementation_t *all[SWITCH_MAX_CODECS] = { 0 };
			const switch_codec_implementation_t *sentinel = (const switch_codec_implementation_t *) &slots;
			int num, g;

			fst_requires(switch_loadable_module_build_dynamic("mod_fmtp_test", fmtp_test_load, NULL, NULL, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS);

			for (g = 0; g < 4; g++) slots.guard[g] = sentinel;
			slots.search[0] = NULL;
			num = switch_loadable_module_get_codecs_sorted(slots.search, fmtp, 1, prefs, 1);
			fst_check_int_equals(num, 1);
			fst_check(slots.search[0] != NULL);
			for (g = 0; g < 4; g++) fst_check(slots.guard[g] == sentinel);

			/* two preferences, room for one: the first fills the array, the second must not be added */
			for (g = 0; g < 4; g++) slots.guard[g] = sentinel;
			num = switch_loadable_module_get_codecs_sorted(slots.search, fmtp, 1, plain_first, 2);
			fst_check_int_equals(num, 1);
			fst_check(slots.search[0] && !strcmp(slots.search[0]->iananame, "PLAINTEST"));
			for (g = 0; g < 4; g++) fst_check(slots.guard[g] == sentinel);
			/* with room, every implementation */
			num = switch_loadable_module_get_codecs_sorted(all, fmtp, SWITCH_MAX_CODECS, prefs, 1);
			fst_check_int_equals(num, 4);
		}
		FST_TEST_END()

		/* switch_loadable_module_get_codecs (every loaded codec) stays within the caller's array */
		FST_TEST_BEGIN(test_get_codecs_stays_in_the_array)
		{
			struct {
				const switch_codec_implementation_t *array[2];
				const switch_codec_implementation_t *guard[4];
			} slots;
			const switch_codec_implementation_t *sentinel = (const switch_codec_implementation_t *) &slots;
			int num, g;

			/* more codecs are loaded than that (PCMU, PCMA, L16, the test codecs ...) */
			for (g = 0; g < 4; g++) slots.guard[g] = sentinel;
			num = switch_loadable_module_get_codecs(slots.array, 2);
			fst_check_int_equals(num, 2);
			for (g = 0; g < 4; g++) fst_check(slots.guard[g] == sentinel);
		}
		FST_TEST_END()

		/* SDP validation (re-INVITEs) records at most MAX_MATCHES (30) near matches, as negotiation does. In
		 * validation ptime comes from a=maxptime (not a=ptime): with maxptime 40, 31 payload types of a codec
		 * that has only 60 ms are near matches that are then refused (60 > 40); a 32nd payload type of a 20 ms
		 * codec would be accepted, but is beyond the 30 recorded, so the SDP is refused, as in negotiation. */
		FST_TEST_BEGIN(test_validate_sdp_keeps_near_matches_in_the_array)
		{
			switch_core_session_t *session = NULL;
			switch_call_cause_t cause;
			switch_media_handle_t *media_handle = NULL;
			switch_core_media_params_t *mparams;
			switch_stream_handle_t sdp = { 0 };
			int pt;

			fst_requires(switch_loadable_module_exists("mod_ptime_test") == SWITCH_STATUS_SUCCESS ||
						 switch_loadable_module_build_dynamic("mod_ptime_test", ptime_test_load, NULL, NULL, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_ivr_originate(NULL, &session, &cause, "null/near-matches", 0, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) == SWITCH_STATUS_SUCCESS);
			mparams = switch_core_session_alloc(session, sizeof(*mparams));
			mparams->inbound_codec_string = switch_core_session_strdup(session, "SLOW60,FAST20");
			mparams->outbound_codec_string = switch_core_session_strdup(session, "SLOW60,FAST20");
			mparams->rtpip = switch_core_session_strdup(session, "127.0.0.1");
			if (switch_media_handle_create(&media_handle, session, mparams) == SWITCH_STATUS_SUCCESS &&
				switch_core_media_prepare_codecs(session, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS) {
				SWITCH_STANDARD_STREAM(sdp);
				sdp.write_function(&sdp, "v=0\r\no=- 1 1 IN IP4 198.51.100.1\r\ns=-\r\nt=0 0\r\nm=audio 56210 RTP/AVP");
				for (pt = 96; pt < 128; pt++) sdp.write_function(&sdp, " %d", pt);
				sdp.write_function(&sdp, "\r\nc=IN IP4 198.51.100.1\r\n");
				for (pt = 96; pt < 127; pt++) sdp.write_function(&sdp, "a=rtpmap:%d SLOW60/8000\r\n", pt);
				sdp.write_function(&sdp, "a=rtpmap:127 FAST20/8000\r\na=ptime:40\r\na=maxptime:40\r\na=sendrecv\r\n");
				fst_check(switch_core_media_validate_common_audio_sdp(session, (char *) sdp.data, SDP_OFFER) == 0);

				/* the 20 ms codec within the first 30 near matches: accepted */
				switch_safe_free(sdp.data);
				SWITCH_STANDARD_STREAM(sdp);
				sdp.write_function(&sdp, "v=0\r\no=- 1 1 IN IP4 198.51.100.1\r\ns=-\r\nt=0 0\r\nm=audio 56210 RTP/AVP 96 97\r\nc=IN IP4 198.51.100.1\r\n"
								   "a=rtpmap:96 SLOW60/8000\r\na=rtpmap:97 FAST20/8000\r\na=ptime:40\r\na=maxptime:40\r\na=sendrecv\r\n");
				fst_check(switch_core_media_validate_common_audio_sdp(session, (char *) sdp.data, SDP_OFFER) == 1);
				switch_safe_free(sdp.data);
			} else {
				fst_check(!"media handle");
			}
			switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
