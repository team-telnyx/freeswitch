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

static switch_bool_t amrwb_nonzero(const unsigned char *pcm, uint32_t len)
{
	uint32_t i;

	for (i = 0; i < len; i++) {
		if (pcm[i]) return SWITCH_TRUE;
	}
	return SWITCH_FALSE;
}

/* decode the same five frames of a 440 Hz tone on both decoders */
static void amrwb_prime(switch_codec_t *a, switch_codec_t *b, const char *fmtp, switch_memory_pool_t *pool)
{
	switch_codec_t encoder = { 0 };
	int16_t pcm[320];
	unsigned char frame[SWITCH_RECOMMENDED_BUFFER_SIZE], out[SWITCH_RECOMMENDED_BUFFER_SIZE];
	uint32_t frame_len, out_len, rate = 16000;
	unsigned int flag = 0;
	int f, i;

	amrwb_init(&encoder, fmtp, SWITCH_CODEC_FLAG_ENCODE, pool);
	for (f = 0; f < 5; f++) {
		for (i = 0; i < 320; i++) pcm[i] = (int16_t) (8000 * sin(2 * M_PI * 440 * (f * 320 + i) / 16000.0));
		frame_len = sizeof(frame);
		switch_core_codec_encode(&encoder, NULL, pcm, sizeof(pcm), 16000, frame, &frame_len, &rate, &flag);
		out_len = sizeof(out);
		switch_core_codec_decode(a, NULL, frame, frame_len, 16000, out, &out_len, &rate, &flag);
		out_len = sizeof(out);
		switch_core_codec_decode(b, NULL, frame, frame_len, 16000, out, &out_len, &rate, &flag);
	}
	switch_core_codec_destroy(&encoder);
}

/* after the same history, a decoder decodes payload exactly as it decodes a lost frame */
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
	/* same speech history first, so concealment output is not silence */
	amrwb_prime(&codec, &reference, fmtp, pool);
	status = switch_core_codec_decode(&codec, NULL, (void *) payload, len, 16000, out, &out_len, &rate, &flag);
	ref_status = switch_core_codec_decode(&reference, NULL, (void *) (oa ? lost_oa : lost_be), 2, 16000, ref, &ref_len, &rate, &flag);
	switch_core_codec_destroy(&codec);
	switch_core_codec_destroy(&reference);

	return (status == SWITCH_STATUS_SUCCESS && ref_status == SWITCH_STATUS_SUCCESS && out_len == 640 && out_len == ref_len &&
			!memcmp(out, ref, out_len) && amrwb_nonzero(ref, ref_len)) ? SWITCH_TRUE : SWITCH_FALSE;
}

static const int amrwb_test_frame_bits[] = {132, 177, 253, 285, 317, 365, 397, 461, 477, 40, 0, 0, 0, 0, 0, 0};

static void amrwb_put_bits(unsigned char *buf, uint32_t *pos, uint32_t value, int n)
{
	while (n--) {
		if ((value >> n) & 1) buf[*pos / 8] |= (unsigned char) (0x80 >> (*pos % 8));
		(*pos)++;
	}
}

/* one payload (RFC 4867 4.3/4.4) with the frames of octet-aligned single-frame payloads; its length */
static uint32_t amrwb_payload(unsigned char frames[][64], int n, switch_bool_t oa, unsigned char *out, uint32_t out_size)
{
	uint32_t pos = 0;
	int f, i, ft;

	memset(out, 0, out_size);
	amrwb_put_bits(out, &pos, 15, 4);
	if (oa) pos += 4;
	for (f = 0; f < n; f++) {
		amrwb_put_bits(out, &pos, ((f < n - 1) << 5) | ((frames[f][1] >> 2) & 0x1f), 6);
		if (oa) pos += 2;
	}
	for (f = 0; f < n; f++) {
		ft = (frames[f][1] >> 3) & 0x0f;
		for (i = 0; i < amrwb_test_frame_bits[ft]; i++) {
			amrwb_put_bits(out, &pos, (frames[f][2 + i / 8] >> (7 - i % 8)) & 1, 1);
		}
		if (oa) pos = (pos + 7) & ~7U;
	}

	return (pos + 7) / 8;
}

static void amrwb_adjust(switch_codec_t *codec, const char *how)
{
	switch_core_codec_control(codec, SCC_AUDIO_ADJUST_BITRATE, SCCT_STRING, (void *) how, SCCT_NONE, NULL, NULL, NULL);
}

/* two fresh decoders produce the same audio for two payloads */
static switch_bool_t amrwb_decode_same(const char *fmtp, const unsigned char *a, uint32_t a_len, const unsigned char *b, uint32_t b_len, switch_memory_pool_t *pool)
{
	switch_codec_t ca = { 0 }, cb = { 0 };
	unsigned char out_a[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 }, out_b[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
	uint32_t len_a = sizeof(out_a), len_b = sizeof(out_b), rate = 16000;
	unsigned int flag = 0;
	switch_status_t sa, sb;

	amrwb_init(&ca, fmtp, SWITCH_CODEC_FLAG_DECODE, pool);
	amrwb_init(&cb, fmtp, SWITCH_CODEC_FLAG_DECODE, pool);
	sa = switch_core_codec_decode(&ca, NULL, (void *) a, a_len, 16000, out_a, &len_a, &rate, &flag);
	sb = switch_core_codec_decode(&cb, NULL, (void *) b, b_len, 16000, out_b, &len_b, &rate, &flag);
	switch_core_codec_destroy(&ca);
	switch_core_codec_destroy(&cb);

	return (sa == SWITCH_STATUS_SUCCESS && sb == SWITCH_STATUS_SUCCESS && len_a == len_b && !memcmp(out_a, out_b, len_a)) ? SWITCH_TRUE : SWITCH_FALSE;
}

static int amrwb_warning_lines = 0;
static int amrwb_ft_ok_lines = 0, amrwb_ft_bad_lines = 0;
static volatile int amrwb_log_marks = 0;

#define AMRWB_LOG_MARK "amrwb test: log mark"

/* counts the module's concealment WARNINGs and the debug lines with the frame type of a SID */
static switch_status_t amrwb_watch_log(const switch_log_node_t *node, switch_log_level_t level)
{
	if (node->level <= SWITCH_LOG_WARNING && node->data && strstr(node->data, "undecodable payloads concealed")) amrwb_warning_lines++;
	if (node->data && strstr(node->data, "AMRWB decoder (OA): FT: [0x9]")) amrwb_ft_ok_lines++;
	if (node->data && strstr(node->data, "AMRWB decoder (OA): FT: [0x19]")) amrwb_ft_bad_lines++;
	if (node->data && strstr(node->data, AMRWB_LOG_MARK)) amrwb_log_marks++;
	return SWITCH_STATUS_SUCCESS;
}

/* the logger is asynchronous: wait until it delivered everything logged so far */
static void amrwb_log_sync(void)
{
	int want = amrwb_log_marks + 1, i;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, AMRWB_LOG_MARK "\n");
	for (i = 0; i < 1000 && amrwb_log_marks < want; i++) {
		switch_yield(10000);
	}
}

static void amrwb_log_watch_start(void)
{
	switch_log_bind_logger(amrwb_watch_log, SWITCH_LOG_DEBUG, SWITCH_FALSE);
	amrwb_log_sync();
	amrwb_warning_lines = amrwb_ft_ok_lines = amrwb_ft_bad_lines = 0;
}

static void amrwb_log_watch_stop(void)
{
	amrwb_log_sync();
	switch_log_unbind_logger(amrwb_watch_log);
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

/* encodes one frame: fst_check_int_equals evaluates its arguments twice */
#define amrwb_check_ft(codec, oa, expected) do { int ft_ = amrwb_encoded_ft((codec), (oa)); fst_check_int_equals(ft_, (expected)); } while (0)
struct amrwb_cmr_feeder {
	switch_codec_t *reader;
	int frames;
};

/* decodes frames with CMR 0, 1, 2 in turn on the session read codec */
static void *SWITCH_THREAD_FUNC amrwb_cmr_feeder_run(switch_thread_t *thread, void *obj)
{
	struct amrwb_cmr_feeder *feeder = (struct amrwb_cmr_feeder *) obj;
	unsigned char payload[2] = { 0x00, 0x7c };
	unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
	uint32_t out_len, rate = 16000;
	unsigned int flag = 0;
	int i;

	for (i = 0; i < feeder->frames; i++) {
		payload[0] = (unsigned char) ((i % 3) << 4);
		out_len = sizeof(out);
		switch_core_codec_decode(feeder->reader, NULL, payload, sizeof(payload), 16000, out, &out_len, &rate, &flag);
	}

	return NULL;
}

/* a session with its own AMR-WB read and write codecs */
static switch_core_session_t *amrwb_cmr_session(switch_codec_t *reader, switch_codec_t *writer, switch_memory_pool_t *pool)
{
	switch_core_session_t *session = NULL;
	switch_call_cause_t cause;

	if (switch_ivr_originate(NULL, &session, &cause, "null/amrwb-cmr-handoff", 0, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS) {
		return NULL;
	}
	amrwb_init(writer, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, pool);
	amrwb_init(reader, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, pool);
	switch_core_session_unset_read_codec(session);
	switch_core_session_unset_write_codec(session);
	switch_core_session_set_write_codec(session, writer);
	switch_core_session_set_read_codec(session, reader);
	writer->session = reader->session = session;

	return session;
}

/* codecs that used the session go before it */
static void amrwb_cmr_session_end(switch_core_session_t *session, switch_codec_t *a, switch_codec_t *b)
{
	switch_core_session_unset_read_codec(session);
	switch_core_session_unset_write_codec(session);
	switch_core_codec_destroy(a);
	switch_core_codec_destroy(b);
	switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
	switch_core_session_rwunlock(session);
}

/* amrwb_show contains text */
static switch_bool_t amrwb_show_has(const char *text)
{
	switch_stream_handle_t stream = { 0 };
	switch_bool_t found;

	SWITCH_STANDARD_STREAM(stream);
	switch_api_execute("amrwb_show", "", NULL, &stream);
	found = (stream.data && strstr((char *) stream.data, text)) ? SWITCH_TRUE : SWITCH_FALSE;
	switch_safe_free(stream.data);

	return found;
}

/* the codec's answer to "fmtp_changes_framing", NULL if none */
static const char *amrwb_changes_framing(switch_codec_t *codec, const char *fmtp)
{
	switch_codec_control_type_t rtype = SCCT_NONE;
	void *ret = NULL;

	switch_core_codec_control(codec, SCC_CODEC_SPECIFIC, SCCT_STRING, (void *) "fmtp_changes_framing", SCCT_STRING, (void *) fmtp, &rtype, &ret);
	return rtype == SCCT_STRING ? (const char *) ret : NULL;
}

static const char *amrwb_conf_settings;

static switch_xml_t amrwb_conf_lookup(const char *section, const char *tag_name, const char *key_name, const char *key_value, switch_event_t *params, void *user_data)
{
	char *xml;
	switch_xml_t conf;

	if (!key_value || strcmp(key_value, "amrwb.conf")) return NULL;
	xml = switch_mprintf("<document type=\"freeswitch/xml\"><section name=\"configuration\">"
						 "<configuration name=\"amrwb.conf\"><settings>%s</settings></configuration></section></document>", amrwb_conf_settings);
	conf = switch_xml_parse_str_dynamic(xml, SWITCH_FALSE);
	return conf;
}

/* reload mod_amrwb with the given settings, or with freeswitch.xml when NULL */
static switch_status_t amrwb_reload(const char *settings)
{
	switch_xml_binding_t *binding = NULL;
	const char *err = NULL;
	switch_status_t status;

	switch_loadable_module_unload_module(SWITCH_GLOBAL_dirs.mod_dir, "mod_amrwb", SWITCH_FALSE, &err);
	if (settings) {
		amrwb_conf_settings = settings;
		switch_xml_bind_search_function_ret(amrwb_conf_lookup, SWITCH_XML_SECTION_CONFIG, NULL, &binding);
	}
	status = switch_loadable_module_load_module(SWITCH_GLOBAL_dirs.mod_dir, "mod_amrwb", SWITCH_TRUE, &err);
	if (binding) {
		switch_xml_unbind_search_function(&binding);
	}
	return status;
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
			"octet-align=0",
			16000,
			20,
			1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
			&codec_settings, fst_pool);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			SWITCH_STANDARD_STREAM(stream);

			switch_api_execute("amrwb_debug", "on", NULL, &stream);

			switch_safe_free(stream.data);

			/* valid frame types are decoded, not concealed (no WARNING) */
			amrwb_log_watch_start();

			/*NO DATA = 0xf*/
			status = switch_core_codec_decode(&read_codec, NULL, &no_data, 2, 16000, &decbuf, &decoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(decoded_len, 640);

			/*SPEECH LOST = 0xe*/
			decoded_len = SWITCH_RECOMMENDED_BUFFER_SIZE;
			status = switch_core_codec_decode(&read_codec, NULL, &speech_lost, 2, 16000, &decbuf, &decoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(decoded_len, 640);

			amrwb_log_watch_stop();
			fst_check_int_equals(amrwb_warning_lines, 0);

			/*Invalid frame type*/
			amrwb_log_watch_start();
			decoded_len = SWITCH_RECOMMENDED_BUFFER_SIZE;
			status = switch_core_codec_decode(&read_codec, NULL, &fail, 2, 16000, &decbuf, &decoded_len, &rate, &flags);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(decoded_len, 640);
			amrwb_log_watch_stop();
			fst_check_int_equals(amrwb_warning_lines, 1);
			fst_check(amrwb_decodes_as_lost("octet-align=0", (unsigned char *) fail, 2, fst_pool));

			switch_core_codec_destroy(&read_codec);

			SWITCH_STANDARD_STREAM(stream);
			switch_api_execute("amrwb_debug", "off", NULL, &stream);
			switch_safe_free(stream.data);
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

		/* frames of one payload decode as the same frames in a payload each */
		FST_TEST_BEGIN(amrwb_decodes_multiple_frames)
		{
			int oa;

			for (oa = 0; oa < 2; oa++) {
				const char *fmtp = oa ? "mode-set=0,1,2;octet-align=1" : "mode-set=0,1,2;octet-align=0";
				switch_codec_t encoder = { 0 }, multi = { 0 }, single = { 0 };
				unsigned char frames[3][64], payload[512];
				unsigned char out_multi[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 }, out_single[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
				int16_t pcm[320];
				uint32_t len, out_len, single_len = 0, rate = 16000;
				unsigned int flag = 0;
				int f, i;

				fst_requires(amrwb_init(&encoder, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				for (f = 0; f < 3; f++) {
					for (i = 0; i < 320; i++) pcm[i] = (int16_t) (8000 * sin(2 * M_PI * 440 * (f * 320 + i) / 16000.0));
					len = sizeof(frames[f]);
					fst_requires(switch_core_codec_encode(&encoder, NULL, pcm, sizeof(pcm), 16000, frames[f], &len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				}
				switch_core_codec_destroy(&encoder);
				/* a NO_DATA frame between speech frames */
				frames[1][1] = (15 << 3) | 0x04;

				fst_requires(amrwb_init(&multi, fmtp, SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_requires(amrwb_init(&single, fmtp, SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);

				len = amrwb_payload(frames, 3, oa, payload, sizeof(payload));
				out_len = sizeof(out_multi);
				fst_check(switch_core_codec_decode(&multi, NULL, payload, len, 16000, out_multi, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				fst_check_int_equals(out_len, 3 * 640);

				for (f = 0; f < 3; f++) {
					uint32_t one = 640;

					len = amrwb_payload(&frames[f], 1, oa, payload, sizeof(payload));
					fst_check(switch_core_codec_decode(&single, NULL, payload, len, 16000, out_single + single_len, &one, &rate, &flag) == SWITCH_STATUS_SUCCESS);
					single_len += one;
				}
				fst_check_int_equals(single_len, 3 * 640);
				fst_check(!memcmp(out_multi, out_single, 3 * 640));
				fst_check(amrwb_nonzero(out_multi, 640) && amrwb_nonzero(out_multi + 1280, 640));

				/* no room for the frames: concealed */
				len = amrwb_payload(frames, 3, oa, payload, sizeof(payload));
				out_len = 2 * 640;
				fst_check(switch_core_codec_decode(&multi, NULL, payload, len, 16000, out_multi, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				fst_check_int_equals(out_len, 640);

				switch_core_codec_destroy(&single);
				switch_core_codec_destroy(&multi);
			}
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_decodes_two_sid_frames)
		{
			switch_codec_t codec = { 0 };
			static const unsigned char two_sid_oa[] = { 0xf0, 0xcc, 0x4c, 0xe3, 0xdf, 0x3d, 0xe0, 0x02, 0xe3, 0xdf, 0x3d, 0xe0, 0x02 };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len = sizeof(out), rate = 16000;
			unsigned int flag = 0;

			fst_requires(amrwb_init(&codec, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_codec_decode(&codec, NULL, (void *) two_sid_oa, sizeof(two_sid_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(out_len, 1280);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* a payload that is not a whole SID is not cached for relay: the encoder fed with its
		 * decoded PCM sends its own frame */
		FST_TEST_BEGIN(amrwb_truncated_sid_not_relayed)
		{
			static const unsigned char sid_be[] = { 0xf4, 0xf8, 0xf7, 0xcf, 0x78, 0x00, 0x80 };
			static const unsigned char sid_oa[] = { 0xf0, 0x4c, 0xe3, 0xdf, 0x3d, 0xe0, 0x02 };
			static const unsigned char short_payload[] = { 0xf0 };
			static const unsigned char reserved_oa[] = { 0xf0, 0x54 };
			static const unsigned char reserved_be_ft10[] = { 0xf5, 0x40 };
			const struct { const char *fmtp; const unsigned char *payload; uint32_t len; int sid; } cases[] = {
				{ "mode-set=0,1,2;octet-align=0", sid_be, sizeof(sid_be), 1 },
				{ "mode-set=0,1,2;octet-align=0", sid_be, sizeof(sid_be) - 1, 0 },
				{ "mode-set=0,1,2;octet-align=0", short_payload, sizeof(short_payload), 0 },
				{ "mode-set=0,1,2;octet-align=0", reserved_be_ft10, sizeof(reserved_be_ft10), 0 },
				{ "mode-set=0,1,2;octet-align=1", sid_oa, sizeof(sid_oa), 1 },
				{ "mode-set=0,1,2;octet-align=1", sid_oa, sizeof(sid_oa) - 1, 0 },
				{ "mode-set=0,1,2;octet-align=1", short_payload, sizeof(short_payload), 0 },
				{ "mode-set=0,1,2;octet-align=1", reserved_oa, sizeof(reserved_oa), 0 }
			};
			int i;

			for (i = 0; i < (int) (sizeof(cases) / sizeof(cases[0])); i++) {
				switch_codec_t source = { 0 }, target_oa = { 0 };
				unsigned char decoded[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 }, encoded[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
				uint32_t decoded_len = sizeof(decoded), encoded_len = sizeof(encoded), rate = 16000;
				unsigned int flag = 0;

				fst_requires(amrwb_init(&source, cases[i].fmtp, SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_requires(amrwb_init(&target_oa, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_core_codec_decode(&source, NULL, (void *) cases[i].payload, cases[i].len, 16000, decoded, &decoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_core_codec_encode(&target_oa, &source, decoded, decoded_len, 16000, encoded, &encoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				fst_check_int_equals(((encoded[1] >> 3) & 0x0f) == 9, cases[i].sid);
				switch_core_codec_destroy(&target_oa);
				switch_core_codec_destroy(&source);
			}
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
			amrwb_check_ft(&codec, SWITCH_TRUE, 8);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_mode_set_ignores_invalid_modes)
		{
			switch_codec_t codec = { 0 };

			/* no mode 0-8: no implementation matches */
			fst_check(amrwb_init(&codec, "mode-set=9,15;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) != SWITCH_STATUS_SUCCESS);
			memset(&codec, 0, sizeof(codec));

			fst_requires(amrwb_init(&codec, "mode-set=1,9;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "mode-set=1;"));
			amrwb_check_ft(&codec, SWITCH_TRUE, 1);
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
			/* second ToC: reserved FT 12 */
			static const unsigned char multi_be[] = { 0xfc, 0xf8, 0xf7, 0xcf, 0x78, 0x00, 0x80 };
			/* F set on every ToC: no last one */
			static const unsigned char unterminated_oa[] = { 0xf0, 0xcc, 0xcc, 0xcc };
			/* two SID ToCs, one SID */
			static const unsigned char short_multi_oa[] = { 0xf0, 0xcc, 0x4c, 0xe3, 0xdf, 0x3d, 0xe0, 0x02 };
			static const unsigned char truncated_sid_be[] = { 0xf4, 0xf8, 0xf7, 0xcf, 0x78, 0x00 };

			fst_check(amrwb_decodes_as_lost("octet-align=0", short_payload, sizeof(short_payload), fst_pool));
			fst_check(amrwb_decodes_as_lost("octet-align=0", reserved_be, sizeof(reserved_be), fst_pool));
			fst_check(amrwb_decodes_as_lost("octet-align=1", reserved_oa, sizeof(reserved_oa), fst_pool));
			fst_check(amrwb_decodes_as_lost("octet-align=0", multi_be, sizeof(multi_be), fst_pool));
			fst_check(amrwb_decodes_as_lost("octet-align=1", unterminated_oa, sizeof(unterminated_oa), fst_pool));
			fst_check(amrwb_decodes_as_lost("octet-align=1", short_multi_oa, sizeof(short_multi_oa), fst_pool));
			fst_check(amrwb_decodes_as_lost("octet-align=0", truncated_sid_be, sizeof(truncated_sid_be), fst_pool));
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_ignores_padding_after_the_frame)
		{
			const char *fmtps[] = { "mode-set=2;octet-align=1", "mode-set=2;octet-align=0", "mode-set=8;octet-align=1", "mode-set=8;octet-align=0" };
			int i;

			for (i = 0; i < 4; i++) {
				switch_codec_t encoder = { 0 };
				int16_t pcm[320];
				unsigned char frame[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
				uint32_t frame_len = sizeof(frame), rate = 16000;
				unsigned int flag = 0;
				int j;

				for (j = 0; j < 320; j++) pcm[j] = (int16_t) (8000 * sin(2 * M_PI * 440 * j / 16000.0));
				fst_requires(amrwb_init(&encoder, fmtps[i], SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_core_codec_encode(&encoder, NULL, pcm, sizeof(pcm), 16000, frame, &frame_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				switch_core_codec_destroy(&encoder);

				fst_check(amrwb_decode_same(fmtps[i], frame, frame_len, frame, frame_len + 3, fst_pool));
				fst_check(!amrwb_decodes_as_lost(fmtps[i], frame, frame_len, fst_pool));
				fst_check(!amrwb_decodes_as_lost(fmtps[i], frame, frame_len + 3, fst_pool));
				fst_check(amrwb_decodes_as_lost(fmtps[i], frame, frame_len + 4, fst_pool));
				fst_check(amrwb_decodes_as_lost(fmtps[i], frame, frame_len - 1, fst_pool));

				/* RTP padding (RFC 3550 5.1): any length, count in the last byte */
				frame[frame_len + 7] = 7;
				fst_check(amrwb_decodes_as_lost(fmtps[i], frame, frame_len + 8, fst_pool));
				frame[frame_len + 7] = 8;
				fst_check(amrwb_decode_same(fmtps[i], frame, frame_len, frame, frame_len + 8, fst_pool));
				fst_check(!amrwb_decodes_as_lost(fmtps[i], frame, frame_len + 8, fst_pool));
				frame[frame_len + 7] = 0;
				frame[frame_len + 39] = 40;
				fst_check(amrwb_decode_same(fmtps[i], frame, frame_len, frame, frame_len + 40, fst_pool));
				frame[frame_len + 39] = 0;
			}
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_conceals_octet_align_mismatch)
		{
			const char *fmtps[] = { "mode-set=2;octet-align=1", "mode-set=2;octet-align=0" };
			int i;

			for (i = 0; i < 2; i++) {
				switch_codec_t encoder = { 0 };
				int16_t pcm[320];
				unsigned char frame[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
				uint32_t frame_len = sizeof(frame), rate = 16000;
				unsigned int flag = 0;
				int j;

				for (j = 0; j < 320; j++) pcm[j] = (int16_t) (8000 * sin(2 * M_PI * 440 * j / 16000.0));
				fst_requires(amrwb_init(&encoder, fmtps[i], SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_core_codec_encode(&encoder, NULL, pcm, sizeof(pcm), 16000, frame, &frame_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				switch_core_codec_destroy(&encoder);

				fst_check(amrwb_decodes_as_lost(fmtps[1 - i], frame, frame_len, fst_pool));
			}
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
				/* a mode-set without a mode 0-8 */
				unsupported_matched += impl->matches_fmtp("octet-align=1;mode-set=9,15", impl->fmtp) == SWITCH_STATUS_SUCCESS;
				unsupported_matched += impl->matches_fmtp("octet-align=1;mode-set=abc", impl->fmtp) == SWITCH_STATUS_SUCCESS;
				matched += impl->matches_fmtp("octet-align=1;mode-set=0, 1 ,8,9", impl->fmtp) == SWITCH_STATUS_SUCCESS;
				matched += impl->matches_fmtp("octet-align=1;mode-set=", impl->fmtp) == SWITCH_STATUS_SUCCESS;
				matched += impl->matches_fmtp("octet-align=1;crc=0;robust-sorting=0;interleaving=0", impl->fmtp) == SWITCH_STATUS_SUCCESS;
			}
			UNPROTECT_INTERFACE(codec_interface);

			/* each accepted fmtp by the octet-aligned implementation */
			fst_check_int_equals(matched, 4);
			fst_check_int_equals(unsupported_matched, 0);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_negotiated_octet_align_wins_over_force)
		{
			const struct { const char *fmtp; uint32_t len; } cases[] = {
				{ "mode-set=2;octet-align=0", 33 },	/* stated: bandwidth-efficient despite force-oa */
				{ "mode-set=2;octet-align=1", 34 },
				{ "mode-set=2", 34 }			/* not stated: force-oa applies */
			};
			int i;

			for (i = 0; i < 3; i++) {
				switch_codec_t codec = { 0 };
				int16_t pcm[320] = { 0 };
				unsigned char encoded[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
				uint32_t encoded_len = sizeof(encoded), rate = 16000;
				unsigned int flag = 0;

				fst_requires(amrwb_init(&codec, cases[i].fmtp, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_core_codec_encode(&codec, NULL, pcm, sizeof(pcm), 16000, encoded, &encoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				fst_check_int_equals(encoded_len, cases[i].len);
				switch_core_codec_destroy(&codec);
			}
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

			fst_requires(amrwb_init(&codec, "a=1;b=1;c=1;d=1;e=1;f=1;g=1;h=1;i=1;j=1;k=1;mode-set=2;octet-align=0",
									SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "mode-set=2;"));
			fst_requires(switch_core_codec_encode(&codec, NULL, pcm, sizeof(pcm), 16000, encoded, &encoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(encoded_len, 33);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* Q=0 (damaged frame): decoded as a lost frame */
		FST_TEST_BEGIN(amrwb_q0_frame_is_decoded_as_lost)
		{
			switch_codec_t encoder = { 0 }, damaged = { 0 }, lost = { 0 };
			static const unsigned char lost_oa[] = { 0xf0, 0x74 };
			int16_t pcm[320];
			unsigned char frames[10][64];
			uint32_t frame_len[10];
			unsigned char out_damaged[SWITCH_RECOMMENDED_BUFFER_SIZE], out_lost[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t len_damaged = 0, len_lost = 0, rate = 16000;
			unsigned int flag = 0;
			int f, i, differ = 0;

			fst_requires(amrwb_init(&encoder, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(amrwb_init(&damaged, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(amrwb_init(&lost, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);

			for (f = 0; f < 10; f++) {
				for (i = 0; i < 320; i++) {
					pcm[i] = (int16_t) (8000 * sin(2 * M_PI * 440 * (f * 320 + i) / 16000.0));
				}
				frame_len[f] = sizeof(frames[f]);
				fst_requires(switch_core_codec_encode(&encoder, NULL, pcm, sizeof(pcm), 16000, frames[f], &frame_len[f], &rate, &flag) == SWITCH_STATUS_SUCCESS);
			}
			frames[5][1] &= ~0x04;

			/* same history, then the Q=0 frame vs a lost frame, then the same good frames */
			for (f = 0; f < 10; f++) {
				len_damaged = sizeof(out_damaged);
				fst_requires(switch_core_codec_decode(&damaged, NULL, frames[f], frame_len[f], 16000, out_damaged, &len_damaged, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				len_lost = sizeof(out_lost);
				if (f != 5) {
					fst_requires(switch_core_codec_decode(&lost, NULL, frames[f], frame_len[f], 16000, out_lost, &len_lost, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				} else {
					fst_requires(switch_core_codec_decode(&lost, NULL, (void *) lost_oa, sizeof(lost_oa), 16000, out_lost, &len_lost, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				}
				differ += f >= 5 && (len_damaged != len_lost || memcmp(out_damaged, out_lost, len_lost));
			}

			fst_check_int_equals(differ, 0);

			switch_core_codec_destroy(&lost);
			switch_core_codec_destroy(&damaged);
			switch_core_codec_destroy(&encoder);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_bitrate_adjustment_stays_in_mode_set)
		{
			switch_codec_t codec = { 0 };

			fst_requires(amrwb_init(&codec, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			amrwb_check_ft(&codec, SWITCH_TRUE, 2);
			amrwb_adjust(&codec, "increase");
			amrwb_check_ft(&codec, SWITCH_TRUE, 2);
			amrwb_adjust(&codec, "decrease");
			amrwb_check_ft(&codec, SWITCH_TRUE, 1);
			amrwb_adjust(&codec, "decrease");
			amrwb_check_ft(&codec, SWITCH_TRUE, 0);
			amrwb_adjust(&codec, "decrease");
			amrwb_check_ft(&codec, SWITCH_TRUE, 0);
			amrwb_adjust(&codec, "increase");
			amrwb_check_ft(&codec, SWITCH_TRUE, 1);
			amrwb_adjust(&codec, "default");
			amrwb_check_ft(&codec, SWITCH_TRUE, 2);
			amrwb_adjust(&codec, "minimum");
			amrwb_check_ft(&codec, SWITCH_TRUE, 0);
			switch_core_codec_destroy(&codec);

			fst_requires(amrwb_init(&codec, "mode-set=1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			amrwb_adjust(&codec, "minimum");
			amrwb_check_ft(&codec, SWITCH_TRUE, 1);
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
			static const unsigned char cmr5_oa[] = { 0x50, 0x7c };
			static const unsigned char cmr2_oa[] = { 0x20, 0x7c };
			static const unsigned char cmr0_oa[] = { 0x00, 0x7c };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len, rate = 16000;
			unsigned int flag = 0;

			fst_requires(amrwb_init(&codec, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			amrwb_check_ft(&codec, SWITCH_TRUE, 2);

			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr1_oa, sizeof(cmr1_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amrwb_check_ft(&codec, SWITCH_TRUE, 1);

			/* outside the mode-set: ignored, the previous request stays */
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr8_oa, sizeof(cmr8_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amrwb_check_ft(&codec, SWITCH_TRUE, 1);

			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr1_oa, sizeof(cmr1_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amrwb_check_ft(&codec, SWITCH_TRUE, 1);

			/* reserved CMR 9-14: ignored, the previous request stays */
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr9_oa, sizeof(cmr9_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amrwb_check_ft(&codec, SWITCH_TRUE, 1);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr12_oa, sizeof(cmr12_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amrwb_check_ft(&codec, SWITCH_TRUE, 1);

			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr15_oa, sizeof(cmr15_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amrwb_check_ft(&codec, SWITCH_TRUE, 2);
			switch_core_codec_destroy(&codec);

			/* non-contiguous mode-set: requests outside it are ignored */
			fst_requires(amrwb_init(&codec, "mode-set=0,2,8;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr5_oa, sizeof(cmr5_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amrwb_check_ft(&codec, SWITCH_TRUE, 8);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr2_oa, sizeof(cmr2_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amrwb_check_ft(&codec, SWITCH_TRUE, 2);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr1_oa, sizeof(cmr1_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amrwb_check_ft(&codec, SWITCH_TRUE, 2);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amrwb_check_ft(&codec, SWITCH_TRUE, 0);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_encoder_steps_to_received_cmr)
		{
			switch_codec_t codec = { 0 };
			static const unsigned char cmr0_oa[] = { 0x00, 0x7c };
			static const unsigned char cmr15_oa[] = { 0xf0, 0x7c };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len, rate = 16000;
			unsigned int flag = 0;
			int ft;

			/* neighbor, every 2nd frame */
			fst_requires(amrwb_init(&codec, "mode-set=0,1,2;mode-change-neighbor=1;mode-change-period=2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			ft = amrwb_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 2);
			ft = amrwb_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 2);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			ft = amrwb_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 1);
			ft = amrwb_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 1);
			ft = amrwb_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 0);
			ft = amrwb_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 0);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr15_oa, sizeof(cmr15_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			ft = amrwb_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 1);
			ft = amrwb_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 1);
			ft = amrwb_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 2);
			switch_core_codec_destroy(&codec);

			/* neighbor within a non-contiguous mode-set */
			fst_requires(amrwb_init(&codec, "mode-set=0,2,8;mode-change-neighbor=1;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			ft = amrwb_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 8);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			ft = amrwb_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 2);
			ft = amrwb_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 0);
			switch_core_codec_destroy(&codec);

			/* period only: changes at even frames */
			fst_requires(amrwb_init(&codec, "mode-set=0,1,2;mode-change-period=2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			ft = amrwb_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 2);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			ft = amrwb_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 2);
			ft = amrwb_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 0);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr15_oa, sizeof(cmr15_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			ft = amrwb_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 0);
			ft = amrwb_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 2);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_only_the_read_codec_passes_cmr_to_the_write_codec)
		{
			switch_core_session_t *session = NULL;
			switch_call_cause_t cause;
			switch_codec_t writer = { 0 }, reader = { 0 }, other = { 0 };
			static const unsigned char cmr0_oa[] = { 0x00, 0x7c };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len, rate = 16000;
			unsigned int flag = 0;
			int ft;

			fst_requires(switch_ivr_originate(NULL, &session, &cause, "null/amrwb-cmr", 0, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) == SWITCH_STATUS_SUCCESS);
			fst_requires(amrwb_init(&writer, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(amrwb_init(&reader, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(amrwb_init(&other, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			switch_core_session_unset_read_codec(session);
			switch_core_session_unset_write_codec(session);
			fst_requires(switch_core_session_set_write_codec(session, &writer) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_session_set_read_codec(session, &reader) == SWITCH_STATUS_SUCCESS);
			writer.session = reader.session = other.session = session;

			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&other, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			ft = amrwb_encoded_ft(&writer, SWITCH_TRUE);
			fst_check_int_equals(ft, 2);

			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&reader, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			ft = amrwb_encoded_ft(&writer, SWITCH_TRUE);
			fst_check_int_equals(ft, 0);

			switch_core_codec_destroy(&other);
			amrwb_cmr_session_end(session, &reader, &writer);
		}
		FST_TEST_END()

		/* the request ends with the read codec that received it (e.g. replaced on re-INVITE) */
		FST_TEST_BEGIN(amrwb_cmr_request_ends_with_the_read_codec)
		{
			switch_core_session_t *session;
			switch_codec_t writer = { 0 }, reader = { 0 }, reader2 = { 0 };
			static const unsigned char cmr0_oa[] = { 0x00, 0x7c };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len = sizeof(out), rate = 16000;
			unsigned int flag = 0;
			int ft;

			fst_requires((session = amrwb_cmr_session(&reader, &writer, fst_pool)));
			fst_requires(switch_core_codec_decode(&reader, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			ft = amrwb_encoded_ft(&writer, SWITCH_TRUE);
			fst_check_int_equals(ft, 0);

			switch_core_session_unset_read_codec(session);
			switch_core_codec_destroy(&reader);
			fst_requires(amrwb_init(&reader2, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_session_set_read_codec(session, &reader2) == SWITCH_STATUS_SUCCESS);
			reader2.session = session;
			ft = amrwb_encoded_ft(&writer, SWITCH_TRUE);
			fst_check_int_equals(ft, 2);

			amrwb_cmr_session_end(session, &reader2, &writer);
		}
		FST_TEST_END()

		/* decode on the session read codec while the session write codec encodes */
		FST_TEST_BEGIN(amrwb_cmr_handoff_with_concurrent_decode_and_encode)
		{
			switch_core_session_t *session;
			switch_codec_t writer = { 0 }, reader = { 0 };
			struct amrwb_cmr_feeder feeder = { 0 };
			switch_thread_t *thread = NULL;
			switch_threadattr_t *attr = NULL;
			switch_status_t retval;
			static const unsigned char cmr0_oa[] = { 0x00, 0x7c };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len = sizeof(out), rate = 16000;
			unsigned int flag = 0;
			int i, ft;

			fst_requires((session = amrwb_cmr_session(&reader, &writer, fst_pool)));
			feeder.reader = &reader;
			feeder.frames = 3000;
			switch_threadattr_create(&attr, fst_pool);
			fst_requires(switch_thread_create(&thread, attr, amrwb_cmr_feeder_run, &feeder, fst_pool) == SWITCH_STATUS_SUCCESS);
			for (i = 0; i < 1000; i++) {
				amrwb_encoded_ft(&writer, SWITCH_TRUE);
			}
			switch_thread_join(&retval, thread);

			fst_requires(switch_core_codec_decode(&reader, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			ft = amrwb_encoded_ft(&writer, SWITCH_TRUE);
			fst_check_int_equals(ft, 0);

			amrwb_cmr_session_end(session, &reader, &writer);
		}
		FST_TEST_END()

		/* re-INVITE: a new codec only when octet-align, the answered or encoded mode-set, or the mode-change parameters change */
		FST_TEST_BEGIN(amrwb_reports_fmtp_framing_changes)
		{
			switch_codec_t codec = { 0 };
			const char *r;

			fst_requires(amrwb_init(&codec, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			r = amrwb_changes_framing(&codec, "mode-set=0,1,2;octet-align=1");
			fst_check(r && !strcmp(r, "false"));
			r = amrwb_changes_framing(&codec, "octet-align=1; mode-set=2,1,0");
			fst_check(r && !strcmp(r, "false"));
			/* not stated: force-oa=1 in the test configuration */
			r = amrwb_changes_framing(&codec, "mode-set=0,1,2");
			fst_check(r && !strcmp(r, "false"));
			r = amrwb_changes_framing(&codec, "mode-set=0,1,2;octet-align=0");
			fst_check(r && !strcmp(r, "true"));
			r = amrwb_changes_framing(&codec, "mode-set=0,1;octet-align=1");
			fst_check(r && !strcmp(r, "true"));
			/* no mode-set: we answer and encode the configured 0,1,2, as before */
			r = amrwb_changes_framing(&codec, "octet-align=1");
			fst_check(r && !strcmp(r, "false"));
			/* period 1: no restriction, as before */
			r = amrwb_changes_framing(&codec, "mode-set=0,1,2;octet-align=1;mode-change-period=1");
			fst_check(r && !strcmp(r, "false"));
			r = amrwb_changes_framing(&codec, "mode-set=0,1,2;octet-align=1;mode-change-period=2");
			fst_check(r && !strcmp(r, "true"));
			r = amrwb_changes_framing(&codec, "mode-set=0,1,2;octet-align=1;mode-change-neighbor=1");
			fst_check(r && !strcmp(r, "true"));
			switch_core_codec_destroy(&codec);

			/* offer without mode-set, re-offer with the mode-set we answered */
			fst_requires(amrwb_init(&codec, "octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			r = amrwb_changes_framing(&codec, "mode-set=0,1,2;octet-align=1");
			fst_check(r && !strcmp(r, "false"));
			r = amrwb_changes_framing(&codec, "mode-set=0,1;octet-align=1");
			fst_check(r && !strcmp(r, "true"));
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* the debug log shows FT without the F bit */
		FST_TEST_BEGIN(amrwb_debug_log_shows_the_frame_type)
		{
			switch_codec_t codec = { 0 };
			switch_stream_handle_t stream = { 0 };
			static const unsigned char multi_oa[] = { 0xf0, 0xcc, 0x4c, 0xe3, 0xdf, 0x3d, 0xe0, 0x02, 0xe3, 0xdf, 0x3d, 0xe0, 0x02 };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len = sizeof(out), rate = 16000;
			unsigned int flag = 0;

			fst_requires(amrwb_init(&codec, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			SWITCH_STANDARD_STREAM(stream);
			switch_api_execute("amrwb_debug", "on", NULL, &stream);
			switch_safe_free(stream.data);
			amrwb_log_watch_start();
			switch_core_codec_decode(&codec, NULL, (void *) multi_oa, sizeof(multi_oa), 16000, out, &out_len, &rate, &flag);
			amrwb_log_watch_stop();
			SWITCH_STANDARD_STREAM(stream);
			switch_api_execute("amrwb_debug", "off", NULL, &stream);
			switch_safe_free(stream.data);
			switch_core_codec_destroy(&codec);

			fst_check_int_equals(amrwb_ft_ok_lines, 1);
			fst_check_int_equals(amrwb_ft_bad_lines, 0);
		}
		FST_TEST_END()

		/* a mode-change-period below 1 does not restrict mode changes */
		FST_TEST_BEGIN(amrwb_ignores_invalid_mode_change_period)
		{
			switch_codec_t codec = { 0 };
			static const unsigned char cmr0_oa[] = { 0x00, 0x7c };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len = sizeof(out), rate = 16000;
			unsigned int flag = 0;
			int ft;

			fst_requires(amrwb_init(&codec, "mode-set=0,1,2;mode-change-period=-1;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			ft = amrwb_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 2);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			ft = amrwb_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 0);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* spaces around "=" and after values in fmtp parameters */
		FST_TEST_BEGIN(amrwb_fmtp_keys_allow_spaces)
		{
			switch_codec_t codec = { 0 };
			switch_codec_interface_t *codec_interface;
			const switch_codec_implementation_t *impl;
			int16_t pcm[320] = { 0 };
			unsigned char encoded[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			uint32_t encoded_len = sizeof(encoded), rate = 16000;
			unsigned int flag = 0;
			int wrong = 0;

			fst_requires(amrwb_init(&codec, "mode-set = 2; octet-align = 0", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "mode-set=2;"));
			fst_requires(switch_core_codec_encode(&codec, NULL, pcm, sizeof(pcm), 16000, encoded, &encoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(encoded_len, 33);
			switch_core_codec_destroy(&codec);

			encoded_len = sizeof(encoded);
			fst_requires(amrwb_init(&codec, "octet-align=1 ;mode-set=2 ", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "mode-set=2;"));
			fst_requires(switch_core_codec_encode(&codec, NULL, pcm, sizeof(pcm), 16000, encoded, &encoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(encoded_len, 34);
			switch_core_codec_destroy(&codec);

			fst_requires((codec_interface = switch_loadable_module_get_codec_interface("AMR-WB", "mod_amrwb")));
			for (impl = codec_interface->implementations; impl; impl = impl->next) {
				switch_bool_t oa_impl = strstr(impl->fmtp, "octet-align=1") ? SWITCH_TRUE : SWITCH_FALSE;

				wrong += (impl->matches_fmtp("octet-align = 1", impl->fmtp) == SWITCH_STATUS_SUCCESS) != oa_impl;
				wrong += (impl->matches_fmtp("octet-align=1 ; mode-set=2", impl->fmtp) == SWITCH_STATUS_SUCCESS) != oa_impl;
			}
			UNPROTECT_INTERFACE(codec_interface);
			fst_check_int_equals(wrong, 0);
		}
		FST_TEST_END()

		/* a repeated octet-align: init and the matcher both use the last one */
		FST_TEST_BEGIN(amrwb_repeated_octet_align_uses_the_last)
		{
			switch_codec_t codec = { 0 };
			switch_codec_interface_t *codec_interface;
			const switch_codec_implementation_t *impl;
			int16_t pcm[320] = { 0 };
			unsigned char encoded[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			uint32_t encoded_len = sizeof(encoded), rate = 16000;
			unsigned int flag = 0;
			int wrong = 0;

			fst_requires(amrwb_init(&codec, "mode-set=2;octet-align=1;octet-align=0", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_codec_encode(&codec, NULL, pcm, sizeof(pcm), 16000, encoded, &encoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(encoded_len, 33);
			switch_core_codec_destroy(&codec);

			fst_requires((codec_interface = switch_loadable_module_get_codec_interface("AMR-WB", "mod_amrwb")));
			for (impl = codec_interface->implementations; impl; impl = impl->next) {
				switch_bool_t be_impl = strstr(impl->fmtp, "octet-align=0") ? SWITCH_TRUE : SWITCH_FALSE;

				wrong += (impl->matches_fmtp("octet-align=1;octet-align=0", impl->fmtp) == SWITCH_STATUS_SUCCESS) != be_impl;
			}
			UNPROTECT_INTERFACE(codec_interface);
			fst_check_int_equals(wrong, 0);
		}
		FST_TEST_END()

		/* destroying a replaced read codec keeps the request of the one that replaced it */
		FST_TEST_BEGIN(amrwb_cmr_request_survives_the_old_read_codec)
		{
			switch_core_session_t *session;
			switch_codec_t writer = { 0 }, reader = { 0 }, reader2 = { 0 };
			static const unsigned char cmr0_oa[] = { 0x00, 0x7c };
			static const unsigned char cmr1_oa[] = { 0x10, 0x7c };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len, rate = 16000;
			unsigned int flag = 0;
			int ft;

			fst_requires((session = amrwb_cmr_session(&reader, &writer, fst_pool)));
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&reader, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);

			fst_requires(amrwb_init(&reader2, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			switch_core_session_unset_read_codec(session);
			fst_requires(switch_core_session_set_read_codec(session, &reader2) == SWITCH_STATUS_SUCCESS);
			reader2.session = session;
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&reader2, NULL, (void *) cmr1_oa, sizeof(cmr1_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			switch_core_codec_destroy(&reader);
			ft = amrwb_encoded_ft(&writer, SWITCH_TRUE);
			fst_check_int_equals(ft, 1);

			amrwb_cmr_session_end(session, &reader2, &writer);
		}
		FST_TEST_END()

		/* suppress_cng on the encoder's session: no SID frames toward that peer */
		FST_TEST_BEGIN(amrwb_suppress_cng_session_does_not_relay_sid)
		{
			switch_core_session_t *session;
			switch_codec_t writer = { 0 }, reader = { 0 }, source = { 0 };
			static const unsigned char sid_oa[] = { 0xf0, 0x4c, 0xe3, 0xdf, 0x3d, 0xe0, 0x02 };
			unsigned char decoded[SWITCH_RECOMMENDED_BUFFER_SIZE], encoded[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			uint32_t decoded_len, encoded_len, rate = 16000;
			unsigned int flag = 0;

			fst_requires((session = amrwb_cmr_session(&reader, &writer, fst_pool)));
			fst_requires(amrwb_init(&source, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);

			decoded_len = sizeof(decoded);
			fst_requires(switch_core_codec_decode(&source, NULL, (void *) sid_oa, sizeof(sid_oa), 16000, decoded, &decoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			encoded_len = sizeof(encoded);
			fst_requires(switch_core_codec_encode(&writer, &source, decoded, decoded_len, 16000, encoded, &encoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals((encoded[1] >> 3) & 0x0f, 9);

			switch_channel_set_variable(switch_core_session_get_channel(session), "suppress_cng", "true");
			decoded_len = sizeof(decoded);
			fst_requires(switch_core_codec_decode(&source, NULL, (void *) sid_oa, sizeof(sid_oa), 16000, decoded, &decoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			encoded_len = sizeof(encoded);
			fst_requires(switch_core_codec_encode(&writer, &source, decoded, decoded_len, 16000, encoded, &encoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals((encoded[1] >> 3) & 0x0f, 2);

			switch_core_codec_destroy(&source);
			amrwb_cmr_session_end(session, &reader, &writer);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_encoder_mode_matches_answered_mode_set)
		{
			switch_codec_t codec = { 0 };

			/* offer without mode-set, mode-set-overwrite off: answer and encode within the configured mode-set */
			fst_requires(amrwb_init(&codec, "octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "mode-set=0,1,2;"));
			amrwb_check_ft(&codec, SWITCH_TRUE, 2);
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
			fst_check(switch_core_codec_control(&codec, SCC_DEBUG, SCCT_INT, &level, SCCT_NONE, NULL, NULL, NULL) == SWITCH_STATUS_FALSE);
			fst_check(switch_core_codec_control(&codec, SCC_AUDIO_ADJUST_BITRATE, SCCT_STRING, (void *) "increase", SCCT_NONE, NULL, NULL, NULL) == SWITCH_STATUS_FALSE);
			switch_core_codec_destroy(&codec);
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

		FST_TEST_BEGIN(amrwb_answered_mode_set_bounds_adjustments)
		{
			switch_codec_t codec = { 0 };

			/* offer without mode-set: the answered mode-set (configured 0,1,2) is binding */
			fst_requires(amrwb_init(&codec, "octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(codec.fmtp_out && strstr(codec.fmtp_out, "mode-set=0,1,2;"));
			amrwb_adjust(&codec, "increase");
			amrwb_check_ft(&codec, SWITCH_TRUE, 2);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_mode_set_ignores_non_numeric_entries)
		{
			switch_codec_t codec = { 0 };

			fst_requires(amrwb_init(&codec, "mode-set=2,,8;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "mode-set=2,8;"));
			switch_core_codec_destroy(&codec);

			fst_requires(amrwb_init(&codec, "mode-set=abc,1;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "mode-set=1;"));
			switch_core_codec_destroy(&codec);

			fst_requires(amrwb_init(&codec, "mode-set=0,1,2,3,4,5,6,7,1,8;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "mode-set=0,1,2,3,4,5,6,7,8;"));
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_concealment_does_not_flood_the_log)
		{
			switch_codec_t codec = { 0 };
			static const unsigned char reserved_oa[] = { 0xf0, 0x54 };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len, rate = 16000;
			unsigned int flag = 0;
			int i;

			fst_requires(amrwb_init(&codec, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			amrwb_log_watch_start();
			for (i = 0; i < 250; i++) {
				out_len = sizeof(out);
				switch_core_codec_decode(&codec, NULL, (void *) reserved_oa, sizeof(reserved_oa), 16000, out, &out_len, &rate, &flag);
			}
			amrwb_log_watch_stop();
			/* the 1st of 250 concealed payloads */
			fst_check_int_equals(amrwb_warning_lines, 1);

			amrwb_log_watch_start();
			out_len = sizeof(out);
			switch_core_codec_decode(&codec, NULL, (void *) reserved_oa, sizeof(reserved_oa), 16000, out, &out_len, &rate, &flag);
			amrwb_log_watch_stop();
			switch_core_codec_destroy(&codec);
			/* and the 251st */
			fst_check_int_equals(amrwb_warning_lines, 1);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_long_fmtp_extra_is_not_truncated)
		{
			switch_codec_t codec = { 0 };
			switch_codec_interface_t *codec_interface;
			const switch_codec_implementation_t *impl;
			int offers = 0, complete = 0;

			fst_requires(amrwb_init(&codec, "mode-set=0,1,2,3,4,5,6,7,8;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "x-tail=1"));
			switch_core_codec_destroy(&codec);

			fst_requires((codec_interface = switch_loadable_module_get_codec_interface("AMR-WB", "mod_amrwb")));
			for (impl = codec_interface->implementations; impl; impl = impl->next) {
				offers++;
				complete += impl->fmtp && strstr(impl->fmtp, "x-tail=1") != NULL;
			}
			UNPROTECT_INTERFACE(codec_interface);
			fst_check(offers > 0 && complete == offers);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_ignores_invalid_default_bitrate)
		{
			fst_requires(amrwb_reload("<param name=\"default-bitrate\" value=\"2\"/><param name=\"default-bitrate\" value=\"12\"/>") == SWITCH_STATUS_SUCCESS);
			fst_check(amrwb_show_has("default-bitrate: 2,"));
			fst_requires(amrwb_reload(NULL) == SWITCH_STATUS_SUCCESS);
			fst_check(amrwb_show_has("default-bitrate: 8,"));
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_overwrite_encodes_within_both_mode_sets)
		{
			switch_codec_t codec = { 0 };

			fst_requires(amrwb_reload("<param name=\"mode-set\" value=\"0,1,2\"/><param name=\"mode-set-overwrite\" value=\"1\"/>"
									  "<param name=\"mode-set-overwrite-with-default-bitrate\" value=\"0\"/>") == SWITCH_STATUS_SUCCESS);

			fst_requires(amrwb_init(&codec, "mode-set=0,1,2,3,4,5,6,7,8;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "mode-set=0,1,2;"));
			amrwb_check_ft(&codec, SWITCH_TRUE, 2);
			switch_core_codec_destroy(&codec);

			/* intersection */
			fst_requires(amrwb_init(&codec, "mode-set=1,8;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "mode-set=0,1,2;"));
			amrwb_check_ft(&codec, SWITCH_TRUE, 1);
			amrwb_adjust(&codec, "increase");
			amrwb_check_ft(&codec, SWITCH_TRUE, 1);
			switch_core_codec_destroy(&codec);

			/* no intersection: the offered mode-set */
			fst_requires(amrwb_init(&codec, "mode-set=7,8;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			amrwb_check_ft(&codec, SWITCH_TRUE, 8);
			amrwb_adjust(&codec, "decrease");
			amrwb_check_ft(&codec, SWITCH_TRUE, 7);
			amrwb_adjust(&codec, "decrease");
			amrwb_check_ft(&codec, SWITCH_TRUE, 7);
			switch_core_codec_destroy(&codec);

			fst_requires(amrwb_reload(NULL) == SWITCH_STATUS_SUCCESS);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_overwrite_offers_the_answered_mode_set)
		{
			switch_codec_interface_t *codec_interface;
			const switch_codec_implementation_t *impl;
			int offers = 0, configured = 0;

			fst_requires(amrwb_reload("<param name=\"mode-set\" value=\"0,1,2\"/><param name=\"mode-set-overwrite\" value=\"1\"/>"
									  "<param name=\"mode-set-overwrite-with-default-bitrate\" value=\"0\"/>") == SWITCH_STATUS_SUCCESS);
			fst_requires((codec_interface = switch_loadable_module_get_codec_interface("AMR-WB", "mod_amrwb")));
			for (impl = codec_interface->implementations; impl; impl = impl->next) {
				offers++;
				configured += impl->fmtp && strstr(impl->fmtp, "mode-set=0,1,2") != NULL;
			}
			UNPROTECT_INTERFACE(codec_interface);
			fst_check(offers > 0 && configured == offers);

			/* with mode-set-overwrite-with-default-bitrate: default-bitrate, not the mode-set */
			fst_requires(amrwb_reload("<param name=\"mode-set\" value=\"0,1,2\"/><param name=\"mode-set-overwrite\" value=\"1\"/>"
									  "<param name=\"default-bitrate\" value=\"1\"/>") == SWITCH_STATUS_SUCCESS);
			fst_requires((codec_interface = switch_loadable_module_get_codec_interface("AMR-WB", "mod_amrwb")));
			offers = configured = 0;
			for (impl = codec_interface->implementations; impl; impl = impl->next) {
				offers++;
				configured += impl->fmtp && strstr(impl->fmtp, "mode-set=1") && !strstr(impl->fmtp, "mode-set=0,1,2");
			}
			UNPROTECT_INTERFACE(codec_interface);
			fst_check(offers > 0 && configured == offers);
			fst_requires(amrwb_reload(NULL) == SWITCH_STATUS_SUCCESS);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_reload_keeps_the_configured_mode_set)
		{
			fst_requires(amrwb_reload(NULL) == SWITCH_STATUS_SUCCESS);
			fst_requires(amrwb_reload(NULL) == SWITCH_STATUS_SUCCESS);
			fst_check(amrwb_show_has("modes: [0,1,2],"));
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_boolean_params_accept_true_and_on)
		{
			fst_requires(amrwb_reload("<param name=\"mode-set-overwrite\" value=\"true\"/>"
									  "<param name=\"mode-set-overwrite-with-default-bitrate\" value=\"false\"/>"
									  "<param name=\"debug\" value=\"on\"/>") == SWITCH_STATUS_SUCCESS);
			fst_check(amrwb_show_has("mode-set-overwrite: 1, mode-set-overwrite-with-default-bitrate: 0,"));
			fst_check(amrwb_show_has("debug: 1,"));
			fst_requires(amrwb_reload(NULL) == SWITCH_STATUS_SUCCESS);
		}
		FST_TEST_END()

		/* silence-supp-off: no SID frames toward the peer, the encoder's own frames instead */
		FST_TEST_BEGIN(amrwb_silence_supp_off_does_not_relay_sid)
		{
			switch_codec_t source_oa = { 0 }, target_oa = { 0 };
			static const unsigned char sid_oa[] = { 0xf0, 0x4c, 0xe3, 0xdf, 0x3d, 0xe0, 0x02 };
			unsigned char decoded[SWITCH_RECOMMENDED_BUFFER_SIZE], encoded[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			uint32_t decoded_len = sizeof(decoded), encoded_len = sizeof(encoded), rate = 16000;
			unsigned int flag = 0;

			fst_requires(amrwb_reload("<param name=\"silence-supp-off\" value=\"true\"/><param name=\"mode-set\" value=\"0,1,2\"/>") == SWITCH_STATUS_SUCCESS);
			fst_requires(amrwb_init(&source_oa, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(amrwb_init(&target_oa, "mode-set=0,1;octet-align=1", SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_codec_decode(&source_oa, NULL, (void *) sid_oa, sizeof(sid_oa), 16000, decoded, &decoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_codec_encode(&target_oa, &source_oa, decoded, decoded_len, 16000, encoded, &encoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals((encoded[1] >> 3) & 0x0f, 1);
			switch_core_codec_destroy(&target_oa);
			switch_core_codec_destroy(&source_oa);
			fst_requires(amrwb_reload(NULL) == SWITCH_STATUS_SUCCESS);
		}
		FST_TEST_END()

		/* force-oa is for offers: on an outbound leg the fmtp answers our offer */
		FST_TEST_BEGIN(amrwb_force_oa_not_applied_to_answers)
		{
			switch_core_session_t *session = NULL;
			switch_call_cause_t cause;
			switch_codec_t codec = { 0 };
			int16_t pcm[320] = { 0 };
			unsigned char encoded[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			uint32_t encoded_len = sizeof(encoded), rate = 16000;
			unsigned int flag = 0;
			const char *r;

			fst_requires(switch_ivr_originate(NULL, &session, &cause, "null/amrwb-force", 0, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_channel_direction(switch_core_session_get_channel(session)) == SWITCH_CALL_DIRECTION_OUTBOUND);
			fst_requires(amrwb_init(&codec, "mode-set=2", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS);
			fst_requires(codec.session == session);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "octet-align=0"));
			fst_requires(switch_core_codec_encode(&codec, NULL, pcm, sizeof(pcm), 16000, encoded, &encoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(encoded_len, 33);
			r = amrwb_changes_framing(&codec, "mode-set=2;octet-align=0");
			fst_check(r && !strcmp(r, "false"));
			switch_core_codec_destroy(&codec);

			switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session);
		}
		FST_TEST_END()

		/* the CMR of a payload that is concealed is ignored (RFC 4867 4.3.2, 4.5.1: discarded) */
		FST_TEST_BEGIN(amrwb_concealed_payload_cmr_is_ignored)
		{
			switch_codec_t codec = { 0 };
			static const unsigned char cmr0_reserved_oa[] = { 0x00, 0x54 };
			static const unsigned char cmr1_reserved_be[] = { 0x15, 0x40 };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len, rate = 16000;
			unsigned int flag = 0;

			fst_requires(amrwb_init(&codec, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			amrwb_check_ft(&codec, SWITCH_TRUE, 2);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr0_reserved_oa, sizeof(cmr0_reserved_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amrwb_check_ft(&codec, SWITCH_TRUE, 2);
			switch_core_codec_destroy(&codec);

			fst_requires(amrwb_init(&codec, "mode-set=0,1,2;octet-align=0", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr1_reserved_be, sizeof(cmr1_reserved_be), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amrwb_check_ft(&codec, SWITCH_FALSE, 2);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* switch_core_codec_reset re-inits on the same pool: the context is reused, not allocated again */
		FST_TEST_BEGIN(amrwb_reset_reuses_the_context)
		{
			switch_codec_t codec = { 0 };
			void *context;
			static const unsigned char cmr0_oa[] = { 0x00, 0x7c };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len = sizeof(out), rate = 16000;
			unsigned int flag = 0;
			int i;

			fst_requires(amrwb_init(&codec, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amrwb_check_ft(&codec, SWITCH_TRUE, 0);
			context = codec.private_info;
			for (i = 0; i < 3; i++) {
				switch_core_codec_reset(&codec);
				fst_check(codec.private_info == context);
			}
			/* a fresh codec: no request, the highest mode */
			amrwb_check_ft(&codec, SWITCH_TRUE, 2);
			out_len = sizeof(out);
			fst_check(switch_core_codec_decode(&codec, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(out_len, 640);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* silence-supp-off: suppress_cng on the session of the codec */
		FST_TEST_BEGIN(amrwb_silence_supp_off_sets_suppress_cng)
		{
			switch_core_session_t *session = NULL;
			switch_call_cause_t cause;
			switch_codec_t codec = { 0 };

			fst_requires(amrwb_reload("<param name=\"silence-supp-off\" value=\"true\"/>") == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_ivr_originate(NULL, &session, &cause, "null/amrwb-cng", 0, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) == SWITCH_STATUS_SUCCESS);
			fst_check(!switch_channel_var_true(switch_core_session_get_channel(session), "suppress_cng"));
			fst_requires(amrwb_init(&codec, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_var_true(switch_core_session_get_channel(session), "suppress_cng"));
			switch_core_codec_destroy(&codec);
			switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session);
			fst_requires(amrwb_reload(NULL) == SWITCH_STATUS_SUCCESS);
		}
		FST_TEST_END()

		/* every mode: payload sizes of RFC 4867 4.3/4.4, the same frame in both formats, the same audio */
		FST_TEST_BEGIN(amrwb_encodes_every_mode_in_both_formats)
		{
			static const int bytes[] = { 17, 23, 32, 36, 40, 46, 50, 58, 60 };
			int m, i, wrong_len = 0, wrong_bits = 0, wrong_audio = 0;

			for (m = 0; m < 9; m++) {
				char fmtp_oa[64], fmtp_be[64];
				switch_codec_t enc_oa = { 0 }, enc_be = { 0 };
				int16_t pcm[320];
				unsigned char oa[1][64], be[SWITCH_RECOMMENDED_BUFFER_SIZE], expected[SWITCH_RECOMMENDED_BUFFER_SIZE];
				uint32_t oa_len = sizeof(oa[0]), be_len = sizeof(be), expected_len, rate = 16000;
				unsigned int flag = 0;

				switch_snprintf(fmtp_oa, sizeof(fmtp_oa), "mode-set=%d;octet-align=1", m);
				switch_snprintf(fmtp_be, sizeof(fmtp_be), "mode-set=%d;octet-align=0", m);
				for (i = 0; i < 320; i++) pcm[i] = (int16_t) (8000 * sin(2 * M_PI * 440 * i / 16000.0));
				fst_requires(amrwb_init(&enc_oa, fmtp_oa, SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_requires(amrwb_init(&enc_be, fmtp_be, SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_core_codec_encode(&enc_oa, NULL, pcm, sizeof(pcm), 16000, oa[0], &oa_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_core_codec_encode(&enc_be, NULL, pcm, sizeof(pcm), 16000, be, &be_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				switch_core_codec_destroy(&enc_be);
				switch_core_codec_destroy(&enc_oa);

				wrong_len += oa_len != (uint32_t) (2 + bytes[m]) || be_len != (uint32_t) (amrwb_test_frame_bits[m] + 10 + 7) / 8 || ((oa[0][1] >> 3) & 0x0f) != m;
				expected_len = amrwb_payload(oa, 1, SWITCH_FALSE, expected, sizeof(expected));
				wrong_bits += expected_len != be_len || memcmp(expected, be, be_len);
				{
					switch_codec_t dec_oa = { 0 }, dec_be = { 0 };
					unsigned char out_oa[SWITCH_RECOMMENDED_BUFFER_SIZE], out_be[SWITCH_RECOMMENDED_BUFFER_SIZE];
					uint32_t len_oa = sizeof(out_oa), len_be = sizeof(out_be);

					fst_requires(amrwb_init(&dec_oa, fmtp_oa, SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
					fst_requires(amrwb_init(&dec_be, fmtp_be, SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
					fst_requires(switch_core_codec_decode(&dec_oa, NULL, oa[0], oa_len, 16000, out_oa, &len_oa, &rate, &flag) == SWITCH_STATUS_SUCCESS);
					fst_requires(switch_core_codec_decode(&dec_be, NULL, be, be_len, 16000, out_be, &len_be, &rate, &flag) == SWITCH_STATUS_SUCCESS);
					wrong_audio += len_oa != 640 || len_be != 640 || memcmp(out_oa, out_be, 640) || !amrwb_nonzero(out_oa, 640);
					switch_core_codec_destroy(&dec_be);
					switch_core_codec_destroy(&dec_oa);
				}
			}

			fst_check_int_equals(wrong_len, 0);
			fst_check_int_equals(wrong_bits, 0);
			fst_check_int_equals(wrong_audio, 0);
		}
		FST_TEST_END()

		/* 12 frames per payload (240 ms) are decoded, 13 are not */
		FST_TEST_BEGIN(amrwb_decodes_at_most_12_frames)
		{
			switch_codec_t codec = { 0 };
			unsigned char payload[16], out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len, rate = 16000;
			unsigned int flag = 0;
			int n;

			fst_requires(amrwb_init(&codec, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			for (n = 12; n <= 13; n++) {
				/* CMR 15, then NO_DATA ToCs: F set on all but the last */
				payload[0] = 0xf0;
				memset(payload + 1, 0xfc, n - 1);
				payload[n] = 0x7c;
				out_len = sizeof(out);
				fst_requires(switch_core_codec_decode(&codec, NULL, payload, n + 1, 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				fst_check_int_equals(out_len, n == 12 ? 12 * 640 : 640);
			}
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* no more audio than the RTP timestamps advance */
		FST_TEST_BEGIN(amrwb_timestamps_bound_the_decoded_audio)
		{
			switch_codec_t codec = { 0 };
			switch_frame_t frame = { 0 };
			/* three NO_DATA frames: 60 ms */
			static const unsigned char three_oa[] = { 0xf0, 0xfc, 0xfc, 0x7c };
			const struct { uint32_t ts; uint32_t len; } cases[] = {
				{ 0, 3 * 640 },
				{ 320, 640 },			/* starts 40 ms before the previous one ends */
				{ 1280, 3 * 640 },		/* right after it */
				{ 1280, 640 },			/* again */
				{ 1920, 2 * 640 },		/* 20 ms before it ends */
				{ 100000, 3 * 640 },	/* a gap */
				{ 0, 640 },				/* back: one frame, then in step again */
				{ 960, 3 * 640 },
				{ 0xfffffc00, 640 },	/* back across the wrap */
				{ 0xffffffc0, 3 * 640 },
				{ 0x380, 3 * 640 }		/* in step across the wrap */
			};
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len, rate = 16000;
			unsigned int flag = 0;
			int i, wrong = 0;

			fst_requires(amrwb_init(&codec, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			codec.cur_frame = &frame;
			for (i = 0; i < (int) (sizeof(cases) / sizeof(cases[0])); i++) {
				frame.timestamp = cases[i].ts;
				out_len = sizeof(out);
				fst_requires(switch_core_codec_decode(&codec, NULL, (void *) three_oa, sizeof(three_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				wrong += out_len != cases[i].len;
			}
			/* without a frame (no timestamps): every frame */
			codec.cur_frame = NULL;
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) three_oa, sizeof(three_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(out_len, 3 * 640);
			fst_check_int_equals(wrong, 0);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* one frame per payload (ptime 20): from 40 ms of input the first 20 ms; less than 20 ms padded with silence */
		FST_TEST_BEGIN(amrwb_encodes_one_frame_per_payload)
		{
			switch_codec_t encoder = { 0 }, reference = { 0 };
			int16_t pcm[640], half[320] = { 0 };
			unsigned char encoded[SWITCH_RECOMMENDED_BUFFER_SIZE], expected[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t encoded_len, expected_len, rate = 16000;
			unsigned int flag = 0;
			int i;

			for (i = 0; i < 640; i++) pcm[i] = (int16_t) (8000 * sin(2 * M_PI * 440 * i / 16000.0));
			memcpy(half, pcm, 320);
			fst_requires(amrwb_init(&encoder, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(amrwb_init(&reference, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);

			encoded_len = sizeof(encoded);
			fst_requires(switch_core_codec_encode(&encoder, NULL, pcm, sizeof(pcm), 16000, encoded, &encoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			expected_len = sizeof(expected);
			fst_requires(switch_core_codec_encode(&reference, NULL, pcm, 640, 16000, expected, &expected_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(encoded_len, 1 + 1 + 32);
			fst_check(encoded_len == expected_len && !memcmp(encoded, expected, encoded_len));

			encoded_len = sizeof(encoded);
			fst_requires(switch_core_codec_encode(&encoder, NULL, pcm, 320, 16000, encoded, &encoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			expected_len = sizeof(expected);
			fst_requires(switch_core_codec_encode(&reference, NULL, half, sizeof(half), 16000, expected, &expected_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check(encoded_len == expected_len && !memcmp(encoded, expected, encoded_len));

			switch_core_codec_destroy(&reference);
			switch_core_codec_destroy(&encoder);
		}
		FST_TEST_END()

		/* a media bug decodes with a copy of the read codec, without a session: its CMR still reaches the write codec */
		FST_TEST_BEGIN(amrwb_media_bug_codec_passes_cmr)
		{
			switch_core_session_t *session;
			switch_codec_t writer = { 0 }, reader = { 0 }, bug = { 0 };
			switch_frame_t frame = { 0 };
			static const unsigned char cmr0_oa[] = { 0x00, 0x7c };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len = sizeof(out), rate = 16000;
			unsigned int flag = 0;
			int ft;

			fst_requires((session = amrwb_cmr_session(&reader, &writer, fst_pool)));
			fst_requires(switch_core_codec_copy(&reader, &bug, NULL, NULL) == SWITCH_STATUS_SUCCESS);
			fst_requires(!bug.session);
			frame.codec = &reader;
			bug.cur_frame = &frame;
			fst_requires(switch_core_codec_decode(&bug, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			bug.cur_frame = NULL;
			ft = amrwb_encoded_ft(&writer, SWITCH_TRUE);
			fst_check_int_equals(ft, 0);

			switch_core_codec_destroy(&bug);
			amrwb_cmr_session_end(session, &reader, &writer);
		}
		FST_TEST_END()

		/* the codec runs on the implementation (payload type) of its format, also with force-oa */
		FST_TEST_BEGIN(amrwb_runs_on_the_payload_type_of_its_format)
		{
			const struct { const char *fmtp; const char *impl; } cases[] = {
				{ "mode-set=2;octet-align=1", "octet-align=1" },
				{ "mode-set=2;octet-align=0", "octet-align=0" },
				{ "mode-set=2", "octet-align=1" }	/* force-oa=1 in the test configuration */
			};
			int i;

			for (i = 0; i < 3; i++) {
				switch_codec_t codec = { 0 };

				fst_requires(amrwb_init(&codec, cases[i].fmtp, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_check(codec.implementation && codec.implementation->fmtp && strstr(codec.implementation->fmtp, cases[i].impl));
				switch_core_codec_reset(&codec);
				fst_check(codec.implementation && codec.implementation->fmtp && strstr(codec.implementation->fmtp, cases[i].impl));
				switch_core_codec_destroy(&codec);
			}
		}
		FST_TEST_END()

		/* crc, robust-sorting and interleaving offered (as 0) are in the answer (RFC 4867 8.3.1) */
		FST_TEST_BEGIN(amrwb_answer_returns_offered_options)
		{
			switch_codec_t codec = { 0 };

			fst_requires(amrwb_init(&codec, "mode-set=2;octet-align=1;crc=0;robust-sorting=0;interleaving=0", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, ";crc=0") && strstr(codec.fmtp_out, ";robust-sorting=0") && strstr(codec.fmtp_out, ";interleaving=0"));
			switch_core_codec_destroy(&codec);

			fst_requires(amrwb_init(&codec, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && !strstr(codec.fmtp_out, "crc") && !strstr(codec.fmtp_out, "robust-sorting") && !strstr(codec.fmtp_out, "interleaving"));
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* a repeated mode-change-neighbor: the encoder and fmtp_changes_framing both use the last */
		FST_TEST_BEGIN(amrwb_repeated_mode_change_neighbor_uses_the_last)
		{
			switch_codec_t codec = { 0 };
			static const unsigned char cmr0_oa[] = { 0x00, 0x7c };
			const char *fmtp = "mode-set=0,1,2;mode-change-neighbor=1;mode-change-neighbor=0;octet-align=1";
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len = sizeof(out), rate = 16000;
			unsigned int flag = 0;
			const char *r;

			fst_requires(amrwb_init(&codec, fmtp, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			amrwb_check_ft(&codec, SWITCH_TRUE, 2);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 16000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			/* straight to 0, not by way of 1 */
			amrwb_check_ft(&codec, SWITCH_TRUE, 0);
			r = amrwb_changes_framing(&codec, fmtp);
			fst_check(r && !strcmp(r, "false"));
			r = amrwb_changes_framing(&codec, "mode-set=0,1,2;mode-change-neighbor=1;octet-align=1");
			fst_check(r && !strcmp(r, "true"));
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* a SID relayed to a bandwidth-efficient leg carries the mode of that leg in its last 4 bits */
		FST_TEST_BEGIN(amrwb_relayed_sid_carries_target_mode_in_be)
		{
			switch_codec_t source_oa = { 0 }, target_be = { 0 };
			static const unsigned char sid_oa[] = { 0xf0, 0x4c, 0xe3, 0xdf, 0x3d, 0xe0, 0x02 };
			unsigned char decoded[SWITCH_RECOMMENDED_BUFFER_SIZE], encoded[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			unsigned char frames[1][64] = { { 0 } }, expected[16];
			uint32_t decoded_len = sizeof(decoded), encoded_len = sizeof(encoded), expected_len, rate = 16000;
			unsigned int flag = 0;

			fst_requires(amrwb_init(&source_oa, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(amrwb_init(&target_be, "mode-set=0,1;octet-align=0", SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_codec_decode(&source_oa, NULL, (void *) sid_oa, sizeof(sid_oa), 16000, decoded, &decoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_codec_encode(&target_be, &source_oa, decoded, decoded_len, 16000, encoded, &encoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			/* the source SID with mode indication 1, bandwidth efficient */
			memcpy(frames[0], sid_oa, sizeof(sid_oa));
			frames[0][6] = (frames[0][6] & 0xf0) | 1;
			expected_len = amrwb_payload(frames, 1, SWITCH_FALSE, expected, sizeof(expected));
			fst_check(encoded_len == expected_len && !memcmp(encoded, expected, expected_len));
			switch_core_codec_destroy(&target_be);
			switch_core_codec_destroy(&source_oa);
		}
		FST_TEST_END()

	}
	FST_SUITE_END()
}
FST_CORE_END()
#endif 
