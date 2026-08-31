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

#include <test/switch_test.h>
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
			fst_check(status != SWITCH_STATUS_SUCCESS);

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

		FST_TEST_BEGIN(amrwb_rejects_multiple_frames)
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
			fst_check(status != SWITCH_STATUS_SUCCESS);

			decoded_len = sizeof(decoded);
			status = switch_core_codec_decode(&source_oa, NULL, multiframes_oa, sizeof(multiframes_oa) - 1,
				16000, decoded, &decoded_len, &rate, &flags);
			fst_check(status != SWITCH_STATUS_SUCCESS);

			switch_core_codec_destroy(&source_oa);
			switch_core_codec_destroy(&source_be);
		}

		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_rejects_truncated_sid)
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
			fst_check(status != SWITCH_STATUS_SUCCESS);
			decoded_len = sizeof(decoded);
			status = switch_core_codec_decode(&source_be, NULL, short_payload, sizeof(short_payload) - 1,
				16000, decoded, &decoded_len, &rate, &flags);
			fst_check(status != SWITCH_STATUS_SUCCESS);
			decoded_len = sizeof(decoded);
			status = switch_core_codec_decode(&source_be, NULL, reserved_be_ft10, sizeof(reserved_be_ft10) - 1,
				16000, decoded, &decoded_len, &rate, &flags);
			fst_check(status != SWITCH_STATUS_SUCCESS);

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
			fst_check(status != SWITCH_STATUS_SUCCESS);
			decoded_len = sizeof(decoded);
			status = switch_core_codec_decode(&source_oa, NULL, short_payload, sizeof(short_payload) - 1,
				16000, decoded, &decoded_len, &rate, &flags);
			fst_check(status != SWITCH_STATUS_SUCCESS);
			decoded_len = sizeof(decoded);
			status = switch_core_codec_decode(&source_oa, NULL, reserved_oa, sizeof(reserved_oa) - 1,
				16000, decoded, &decoded_len, &rate, &flags);
			fst_check(status != SWITCH_STATUS_SUCCESS);

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

	}
	FST_SUITE_END()
}
FST_CORE_END()
#endif 
