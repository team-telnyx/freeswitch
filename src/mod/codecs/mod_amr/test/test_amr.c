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
#include <math.h>

#include <test/switch_test.h>

#define AMR_FMTP_OA "octet-align=1; mode-set=2"
#define AMR_FMTP_BE "mode-set=2"

/* AMR-NB bits per frame type (3GPP TS 26.101): modes 0-7, SID */
static const int amr_test_frame_bits[] = {95, 103, 118, 134, 148, 159, 204, 244, 39, 0, 0, 0, 0, 0, 0, 0};
static const int amr_test_frame_bytes[] = {12, 13, 15, 17, 19, 20, 26, 31, 5};

static switch_status_t amr_init(switch_codec_t *codec, const char *fmtp, uint32_t flags, switch_memory_pool_t *pool)
{
	switch_codec_settings_t codec_settings = {{ 0 }};

	return switch_core_codec_init(codec, "AMR", "mod_amr", fmtp, 8000, 20, 1, flags, &codec_settings, pool);
}

static switch_bool_t amr_nonzero(const unsigned char *pcm, uint32_t len)
{
	uint32_t i;

	for (i = 0; i < len; i++) {
		if (pcm[i]) return SWITCH_TRUE;
	}
	return SWITCH_FALSE;
}

/* 20 ms of a 440 Hz tone, frame f of a continuous signal */
static void amr_tone(int16_t *pcm, int f)
{
	int i;

	for (i = 0; i < 160; i++) pcm[i] = (int16_t) (8000 * sin(2 * M_PI * 440 * (f * 160 + i) / 8000.0));
}

/* 20 ms of noise */
static void amr_noise(int16_t *pcm, uint32_t *seed)
{
	int i;

	for (i = 0; i < 160; i++) {
		*seed = *seed * 1103515245 + 12345;
		pcm[i] = (int16_t) ((int) ((*seed >> 16) & 0x3fff) - 0x2000);
	}
}

/* one payload, encoded from 20 ms of input; its length, 0 if encoding failed */
static uint32_t amr_encode(switch_codec_t *encoder, const int16_t *input, unsigned char *out, uint32_t out_size)
{
	int16_t pcm[160];
	uint32_t len = out_size, rate = 8000;
	unsigned int flag = 0;

	memcpy(pcm, input, sizeof(pcm));
	return switch_core_codec_encode(encoder, NULL, pcm, sizeof(pcm), 8000, out, &len, &rate, &flag) == SWITCH_STATUS_SUCCESS ? len : 0;
}

/* decode the same five frames of a 440 Hz tone on both decoders */
static void amr_prime(switch_codec_t *a, switch_codec_t *b, const char *fmtp, switch_memory_pool_t *pool)
{
	switch_codec_t encoder = { 0 };
	int16_t pcm[160];
	unsigned char frame[SWITCH_RECOMMENDED_BUFFER_SIZE], out[SWITCH_RECOMMENDED_BUFFER_SIZE];
	uint32_t frame_len, out_len, rate = 8000;
	unsigned int flag = 0;
	int f;

	if (amr_init(&encoder, fmtp, SWITCH_CODEC_FLAG_ENCODE, pool) != SWITCH_STATUS_SUCCESS) {
		return;
	}
	for (f = 0; f < 5; f++) {
		amr_tone(pcm, f);
		frame_len = amr_encode(&encoder, pcm, frame, sizeof(frame));
		out_len = sizeof(out);
		switch_core_codec_decode(a, NULL, frame, frame_len, 8000, out, &out_len, &rate, &flag);
		out_len = sizeof(out);
		switch_core_codec_decode(b, NULL, frame, frame_len, 8000, out, &out_len, &rate, &flag);
	}
	switch_core_codec_destroy(&encoder);
}

/* after the same history, a decoder decodes payload exactly as it decodes a NO_DATA frame */
static switch_bool_t amr_decodes_as_lost(const char *fmtp, const unsigned char *payload, uint32_t len, switch_memory_pool_t *pool)
{
	static const unsigned char lost_be[] = { 0xf7, 0xc0 };
	static const unsigned char lost_oa[] = { 0xf0, 0x7c };
	switch_codec_t codec = { 0 }, reference = { 0 };
	unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 }, ref[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
	uint32_t out_len = sizeof(out), ref_len = sizeof(ref), rate = 8000;
	unsigned int flag = 0;
	switch_bool_t oa = strstr(fmtp, "octet-align=1") ? SWITCH_TRUE : SWITCH_FALSE;
	switch_status_t status, ref_status;

	if (amr_init(&codec, fmtp, SWITCH_CODEC_FLAG_DECODE, pool) != SWITCH_STATUS_SUCCESS ||
		amr_init(&reference, fmtp, SWITCH_CODEC_FLAG_DECODE, pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_FALSE;
	}
	/* same speech history first, so concealment output is not silence */
	amr_prime(&codec, &reference, fmtp, pool);
	status = switch_core_codec_decode(&codec, NULL, (void *) payload, len, 8000, out, &out_len, &rate, &flag);
	ref_status = switch_core_codec_decode(&reference, NULL, (void *) (oa ? lost_oa : lost_be), 2, 8000, ref, &ref_len, &rate, &flag);
	switch_core_codec_destroy(&codec);
	switch_core_codec_destroy(&reference);

	return (status == SWITCH_STATUS_SUCCESS && ref_status == SWITCH_STATUS_SUCCESS && out_len == 320 && out_len == ref_len &&
			!memcmp(out, ref, out_len) && amr_nonzero(ref, ref_len)) ? SWITCH_TRUE : SWITCH_FALSE;
}

static void amr_put_bits(unsigned char *buf, uint32_t *pos, uint32_t value, int n)
{
	while (n--) {
		if ((value >> n) & 1) buf[*pos / 8] |= (unsigned char) (0x80 >> (*pos % 8));
		(*pos)++;
	}
}

/* one payload (RFC 4867 4.3/4.4) with the frames of octet-aligned single-frame payloads; its length.
 * Independent of the module's packer. */
static uint32_t amr_payload(unsigned char frames[][64], int n, switch_bool_t oa, unsigned char *out, uint32_t out_size)
{
	uint32_t pos = 0;
	int f, i, ft;

	memset(out, 0, out_size);
	amr_put_bits(out, &pos, 15, 4);
	if (oa) pos += 4;
	for (f = 0; f < n; f++) {
		amr_put_bits(out, &pos, ((f < n - 1) << 5) | ((frames[f][1] >> 2) & 0x1f), 6);
		if (oa) pos += 2;
	}
	for (f = 0; f < n; f++) {
		ft = (frames[f][1] >> 3) & 0x0f;
		for (i = 0; i < amr_test_frame_bits[ft]; i++) {
			amr_put_bits(out, &pos, (frames[f][2 + i / 8] >> (7 - i % 8)) & 1, 1);
		}
		if (oa) pos = (pos + 7) & ~7U;
	}

	return (pos + 7) / 8;
}

static void amr_adjust(switch_codec_t *codec, const char *how)
{
	switch_core_codec_control(codec, SCC_AUDIO_ADJUST_BITRATE, SCCT_STRING, (void *) how, SCCT_NONE, NULL, NULL, NULL);
}

/* two fresh decoders produce the same audio for two payloads */
static switch_bool_t amr_decode_same(const char *fmtp_a, const unsigned char *a, uint32_t a_len, const char *fmtp_b, const unsigned char *b, uint32_t b_len,
									 switch_memory_pool_t *pool)
{
	switch_codec_t ca = { 0 }, cb = { 0 };
	unsigned char out_a[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 }, out_b[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
	uint32_t len_a = sizeof(out_a), len_b = sizeof(out_b), rate = 8000;
	unsigned int flag = 0;
	switch_status_t sa, sb;

	if (amr_init(&ca, fmtp_a, SWITCH_CODEC_FLAG_DECODE, pool) != SWITCH_STATUS_SUCCESS ||
		amr_init(&cb, fmtp_b, SWITCH_CODEC_FLAG_DECODE, pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_FALSE;
	}
	sa = switch_core_codec_decode(&ca, NULL, (void *) a, a_len, 8000, out_a, &len_a, &rate, &flag);
	sb = switch_core_codec_decode(&cb, NULL, (void *) b, b_len, 8000, out_b, &len_b, &rate, &flag);
	switch_core_codec_destroy(&ca);
	switch_core_codec_destroy(&cb);

	return (sa == SWITCH_STATUS_SUCCESS && sb == SWITCH_STATUS_SUCCESS && len_a == len_b && !memcmp(out_a, out_b, len_a)) ? SWITCH_TRUE : SWITCH_FALSE;
}

static int amr_warning_lines = 0;
static volatile int amr_log_marks = 0;

#define AMR_LOG_MARK "amr test: log mark"

/* counts the module's concealment WARNINGs */
static switch_status_t amr_watch_log(const switch_log_node_t *node, switch_log_level_t level)
{
	if (node->level <= SWITCH_LOG_WARNING && node->data && strstr(node->data, "undecodable payloads concealed")) amr_warning_lines++;
	if (node->data && strstr(node->data, AMR_LOG_MARK)) amr_log_marks++;
	return SWITCH_STATUS_SUCCESS;
}

/* the logger is asynchronous: wait until it delivered everything logged so far */
static void amr_log_sync(void)
{
	int want = amr_log_marks + 1, i;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, AMR_LOG_MARK "\n");
	for (i = 0; i < 1000 && amr_log_marks < want; i++) {
		switch_yield(10000);
	}
}

static void amr_log_watch_start(void)
{
	switch_log_bind_logger(amr_watch_log, SWITCH_LOG_DEBUG, SWITCH_FALSE);
	amr_log_sync();
	amr_warning_lines = 0;
}

static void amr_log_watch_stop(void)
{
	amr_log_sync();
	switch_log_unbind_logger(amr_watch_log);
}

/* frame type of the payload the encoder produces for 20 ms of silence */
static int amr_encoded_ft(switch_codec_t *codec, switch_bool_t octet_aligned)
{
	int16_t pcm[160] = { 0 };
	unsigned char encoded[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };

	if (!amr_encode(codec, pcm, encoded, sizeof(encoded))) {
		return -1;
	}

	return octet_aligned ? (encoded[1] >> 3) & 0x0f : ((encoded[0] & 0x07) << 1) | (encoded[1] >> 7);
}

/* encodes one frame: fst_check_int_equals evaluates its arguments twice */
#define amr_check_ft(codec, oa, expected) do { int ft_ = amr_encoded_ft((codec), (oa)); fst_check_int_equals(ft_, (expected)); } while (0)

/* a session with its own AMR read and write codecs */
static switch_core_session_t *amr_cmr_session(switch_codec_t *reader, switch_codec_t *writer, switch_memory_pool_t *pool)
{
	switch_core_session_t *session = NULL;
	switch_call_cause_t cause;

	if (switch_ivr_originate(NULL, &session, &cause, "null/amr-cmr-handoff", 0, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS) {
		return NULL;
	}
	amr_init(writer, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, pool);
	amr_init(reader, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, pool);
	switch_core_session_unset_read_codec(session);
	switch_core_session_unset_write_codec(session);
	switch_core_session_set_write_codec(session, writer);
	switch_core_session_set_read_codec(session, reader);
	writer->session = reader->session = session;

	return session;
}

/* codecs that used the session go before it */
static void amr_cmr_session_end(switch_core_session_t *session, switch_codec_t *a, switch_codec_t *b)
{
	switch_core_session_unset_read_codec(session);
	switch_core_session_unset_write_codec(session);
	switch_core_codec_destroy(a);
	switch_core_codec_destroy(b);
	switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
	switch_core_session_rwunlock(session);
}

/* amr_show contains text */
static switch_bool_t amr_show_has(const char *text)
{
	switch_stream_handle_t stream = { 0 };
	switch_bool_t found;

	SWITCH_STANDARD_STREAM(stream);
	switch_api_execute("amr_show", "", NULL, &stream);
	found = (stream.data && strstr((char *) stream.data, text)) ? SWITCH_TRUE : SWITCH_FALSE;
	switch_safe_free(stream.data);

	return found;
}

/* the codec's answer to "fmtp_changes_framing", NULL if none */
static const char *amr_changes_framing(switch_codec_t *codec, const char *fmtp)
{
	switch_codec_control_type_t rtype = SCCT_NONE;
	void *ret = NULL;

	switch_core_codec_control(codec, SCC_CODEC_SPECIFIC, SCCT_STRING, (void *) "fmtp_changes_framing", SCCT_STRING, (void *) fmtp, &rtype, &ret);
	return rtype == SCCT_STRING ? (const char *) ret : NULL;
}

static const char *amr_conf_settings;

static switch_xml_t amr_conf_lookup(const char *section, const char *tag_name, const char *key_name, const char *key_value, switch_event_t *params, void *user_data)
{
	char *xml;
	switch_xml_t conf;

	if (!key_value || strcmp(key_value, "amr.conf")) return NULL;
	xml = switch_mprintf("<document type=\"freeswitch/xml\"><section name=\"configuration\">"
						 "<configuration name=\"amr.conf\"><settings>%s</settings></configuration></section></document>", amr_conf_settings);
	conf = switch_xml_parse_str_dynamic(xml, SWITCH_FALSE);
	return conf;
}

/* reload mod_amr with the given settings, or with freeswitch.xml when NULL */
static switch_status_t amr_reload(const char *settings)
{
	switch_xml_binding_t *binding = NULL;
	const char *err = NULL;
	switch_status_t status;

	switch_loadable_module_unload_module(SWITCH_GLOBAL_dirs.mod_dir, "mod_amr", SWITCH_FALSE, &err);
	if (settings) {
		amr_conf_settings = settings;
		switch_xml_bind_search_function_ret(amr_conf_lookup, SWITCH_XML_SECTION_CONFIG, NULL, &binding);
	}
	status = switch_loadable_module_load_module(SWITCH_GLOBAL_dirs.mod_dir, "mod_amr", SWITCH_TRUE, &err);
	if (binding) {
		switch_xml_unbind_search_function(&binding);
	}
	return status;
}

/* SDP offers to a session */

typedef struct {
	int negotiated;
	int decoded;
	int codec_reset;
	int stale_decoded;
	int framing_stale;
	char answer[256];
} amr_reoffer_result_t;

/* an offer with one AMR payload type; fmtp NULL: none */
static const char *amr_offer_pt(switch_core_session_t *session, int version, int pt, const char *fmtp)
{
	return switch_core_session_sprintf(session,
		"v=0\r\n"
		"o=- 1 %d IN IP4 198.51.100.1\r\n"
		"s=-\r\n"
		"t=0 0\r\n"
		"m=audio 56210 RTP/AVP %d\r\n"
		"c=IN IP4 198.51.100.1\r\n"
		"a=rtpmap:%d AMR/8000\r\n"
		"%s"
		"a=sendrecv\r\n", version, pt, pt, fmtp ? switch_core_session_sprintf(session, "a=fmtp:%d %s\r\n", pt, fmtp) : "");
}

static const char *amr_offer(switch_core_session_t *session, int version, const char *fmtp)
{
	return amr_offer_pt(session, version, 96, fmtp);
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

/* the a=fmtp line of the answer (without "a=fmtp:"), empty if there is none */
static void amr_answer_fmtp(switch_core_session_t *session, char *answer, size_t answer_size)
{
	const char *sdp, *fmtp, *eol;

	*answer = '\0';
	switch_core_media_gen_local_sdp(session, SDP_ANSWER, "127.0.0.1", 12345, NULL, 1);

	if (!(sdp = switch_channel_get_variable(switch_core_session_get_channel(session), "rtp_local_sdp_str")) ||
		!(fmtp = strstr(sdp, "a=fmtp:"))) {
		return;
	}
	fmtp += strlen("a=fmtp:");
	if (!(eol = strstr(fmtp, "\r\n"))) {
		eol = fmtp + strlen(fmtp);
	}
	switch_snprintf(answer, answer_size, "%.*s", (int) (eol - fmtp), fmtp);
}

/* the media handle and codecs of a session for SDP tests */
static switch_status_t amr_session_media(switch_core_session_t *session, const char *strict)
{
	switch_media_handle_t *media_handle = NULL;
	switch_core_media_params_t *mparams;

	if (strict) {
		switch_channel_set_variable(switch_core_session_get_channel(session), "telnyx-strict-codec-match", strict);
	}

	mparams = switch_core_session_alloc(session, sizeof(switch_core_media_params_t));
	mparams->inbound_codec_string = switch_core_session_strdup(session, "AMR");
	mparams->outbound_codec_string = switch_core_session_strdup(session, "AMR");
	mparams->rtpip = switch_core_session_strdup(session, "127.0.0.1");

	if (switch_media_handle_create(&media_handle, session, mparams) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}
	return switch_core_media_prepare_codecs(session, SWITCH_FALSE);
}

/* frames that a fresh encoder produced with fmtp decode on codec exactly as on a fresh decoder with that fmtp */
static int amr_frames_decode_as(switch_codec_t *codec, switch_memory_pool_t *pool, const char *fmtp, int frames)
{
	switch_codec_t encoder = { 0 }, reference = { 0 };
	unsigned char pkt[64], got[SWITCH_RECOMMENDED_BUFFER_SIZE], ref[SWITCH_RECOMMENDED_BUFFER_SIZE];
	int16_t pcm[160];
	uint32_t seed = 54321, pkt_len, got_len, ref_len, rate = 8000;
	unsigned int flag = 0;
	int f, same = 1;

	if (amr_init(&encoder, fmtp, SWITCH_CODEC_FLAG_ENCODE, pool) != SWITCH_STATUS_SUCCESS ||
		amr_init(&reference, fmtp, SWITCH_CODEC_FLAG_DECODE, pool) != SWITCH_STATUS_SUCCESS) {
		return 0;
	}
	for (f = 0; f < frames && same; f++) {
		amr_noise(pcm, &seed);
		pkt_len = amr_encode(&encoder, pcm, pkt, sizeof(pkt));
		got_len = sizeof(got);
		ref_len = sizeof(ref);
		same = pkt_len && switch_core_codec_decode(codec, NULL, pkt, pkt_len, 8000, got, &got_len, &rate, &flag) == SWITCH_STATUS_SUCCESS &&
			switch_core_codec_decode(&reference, NULL, pkt, pkt_len, 8000, ref, &ref_len, &rate, &flag) == SWITCH_STATUS_SUCCESS &&
			got_len == 320 && got_len == ref_len && !memcmp(got, ref, got_len);
	}
	switch_core_codec_destroy(&reference);
	switch_core_codec_destroy(&encoder);

	return same;
}

static int amr_framing_changes(switch_codec_t *codec, const char *fmtp)
{
	const char *r = amr_changes_framing(codec, fmtp);

	return r && switch_true(r);
}

/* first an offer of first_fmtp at payload type first_pt, then second_offer */
static amr_reoffer_result_t amr_reoffer_pt(switch_core_session_t *session, const char *strict, int first_pt,
										   const char *first_fmtp, const char *second_offer, const char *frame_fmtp)
{
	amr_reoffer_result_t result = { 0 };
	switch_memory_pool_t *pool = switch_core_session_get_pool(session);
	switch_codec_t *read_codec;
	void *initial_context;
	uint8_t p = 0;

	if (amr_session_media(session, strict) != SWITCH_STATUS_SUCCESS ||
		switch_core_media_negotiate_sdp(session, amr_offer_pt(session, 1, first_pt, first_fmtp), &p, SDP_OFFER) != 1 ||
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
	result.decoded = amr_frames_decode_as(read_codec, pool, frame_fmtp, 20);
	/* frames the far end may still send in the old format right after the re-offer, on a decoder
	 * without history as the reference */
	switch_core_codec_reset(read_codec);
	result.stale_decoded = amr_frames_decode_as(read_codec, pool, first_fmtp, 20);
	amr_answer_fmtp(session, result.answer, sizeof(result.answer));

	return result;
}

static amr_reoffer_result_t amr_reoffer(switch_core_session_t *session, const char *strict,
										const char *first_fmtp, const char *second_offer, const char *frame_fmtp)
{
	return amr_reoffer_pt(session, strict, 96, first_fmtp, second_offer, frame_fmtp);
}

/* the answer to a single offer */
static int amr_answer_to(switch_core_session_t *session, const char *offer, char *answer, size_t answer_size)
{
	uint8_t p = 0;

	*answer = '\0';
	if (amr_session_media(session, NULL) != SWITCH_STATUS_SUCCESS || switch_core_media_negotiate_sdp(session, offer, &p, SDP_OFFER) != 1) {
		return 0;
	}
	amr_answer_fmtp(session, answer, answer_size);
	return 1;
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

		/* re-INVITE with strict codec matching: a framing change resets the codec */
		FST_SESSION_BEGIN(amr_strict_reoffer_octet_aligned_to_bandwidth_efficient)
		{
			amr_reoffer_result_t result = amr_reoffer(fst_session, "true", AMR_FMTP_OA, amr_offer(fst_session, 2, AMR_FMTP_BE), AMR_FMTP_BE);

			fst_requires(result.negotiated);
			fst_xcheck(result.codec_reset, "a framing change resets the codec");
			fst_xcheck(!result.framing_stale, "the new codec runs the new framing");
			fst_xcheck(result.decoded, "bandwidth efficient frames decode after the re-offer");
			fst_xcheck(result.stale_decoded, "octet aligned frames sent before the answer still decode");
			fst_xcheck(strstr(result.answer, "octet-align=0") != NULL, "the answer advertises bandwidth efficient framing");
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(amr_strict_reoffer_bandwidth_efficient_to_octet_aligned)
		{
			amr_reoffer_result_t result = amr_reoffer(fst_session, "true", AMR_FMTP_BE, amr_offer(fst_session, 2, AMR_FMTP_OA), AMR_FMTP_OA);

			fst_requires(result.negotiated);
			fst_xcheck(result.codec_reset, "a framing change resets the codec");
			fst_xcheck(!result.framing_stale, "the new codec runs the new framing");
			fst_xcheck(result.decoded, "octet aligned frames decode after the re-offer");
			fst_xcheck(result.stale_decoded, "bandwidth efficient frames sent before the answer still decode");
			fst_xcheck(strstr(result.answer, "octet-align=1") != NULL, "the answer advertises octet aligned framing");
		}
		FST_SESSION_END()

		/* a mode-set change re-initialises the codec and is answered (RFC 4867 8.3.1) */
		FST_SESSION_BEGIN(amr_strict_reoffer_mode_set_change_answers_the_new_mode_set)
		{
			amr_reoffer_result_t result = amr_reoffer(fst_session, "true", AMR_FMTP_OA,
													  amr_offer(fst_session, 2, "octet-align=1; mode-set=1,2"), "octet-align=1; mode-set=1,2");

			fst_requires(result.negotiated);
			fst_xcheck(result.codec_reset, "a mode-set change re-initialises the codec");
			fst_xcheck(strstr(result.answer, "mode-set=1,2;") != NULL, "the answer returns the new mode-set");
			fst_xcheck(strstr(result.answer, "octet-align=1") != NULL, "the answer still advertises octet aligned framing");
			fst_xcheck(result.decoded, "octet aligned frames still decode");
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(amr_strict_reoffer_prefers_map_with_running_framing)
		{
			/* the same payload type: a payload type change resets the codec in any case */
			amr_reoffer_result_t result = amr_reoffer_pt(fst_session, "true", 97, AMR_FMTP_OA,
														 amr_offer_two_maps(fst_session, 2, AMR_FMTP_BE, AMR_FMTP_OA), AMR_FMTP_OA);

			fst_requires(result.negotiated);
			fst_xcheck(!result.codec_reset, "the codec is kept: the offer still has its framing");
			fst_xcheck(result.decoded, "octet aligned frames still decode");
			fst_xcheck(strstr(result.answer, "octet-align=1") != NULL, "the answer keeps octet aligned framing");
		}
		FST_SESSION_END()

		/* without strict codec matching the core keeps the codec across a re-offer (core behaviour) */
		FST_SESSION_BEGIN(amr_legacy_reoffer_keeps_stale_framing)
		{
			amr_reoffer_result_t result = amr_reoffer(fst_session, NULL, AMR_FMTP_OA, amr_offer(fst_session, 2, AMR_FMTP_BE), AMR_FMTP_BE);

			fst_requires(result.negotiated);
			fst_xcheck(!result.codec_reset, "without strict matching the codec is kept across the re-offer");
			fst_xcheck(result.framing_stale, "the kept codec still runs the old framing");
		}
		FST_SESSION_END()

		/* an offer without mode-set: the answer states default-bitrate, never an empty mode-set */
		FST_SESSION_BEGIN(amr_answer_to_offer_without_mode_set)
		{
			char answer[256];

			fst_requires(amr_answer_to(fst_session, amr_offer(fst_session, 1, "octet-align=1"), answer, sizeof(answer)));
			fst_check(strstr(answer, "mode-set=7;") != NULL);
			fst_check(strstr(answer, "octet-align=1") != NULL);
			fst_check(strstr(answer, "mode-set=;") == NULL);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(amr_answer_to_offer_without_fmtp)
		{
			char answer[256];

			fst_requires(amr_answer_to(fst_session, amr_offer(fst_session, 1, NULL), answer, sizeof(answer)));
			fst_check(strstr(answer, "mode-set=7;") != NULL);
			fst_check(strstr(answer, "octet-align=0") != NULL);
		}
		FST_SESSION_END()

		/* crc is not supported: that payload type is not matched */
		FST_SESSION_BEGIN(amr_offer_with_crc_is_not_matched)
		{
			uint8_t p = 0;

			fst_requires(amr_session_media(fst_session, NULL) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_media_negotiate_sdp(fst_session, amr_offer(fst_session, 1, "octet-align=1; crc=1"), &p, SDP_OFFER) == 0);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(amr_offer_with_crc_and_plain_payload_types_takes_the_plain_one)
		{
			char answer[256];

			fst_requires(amr_answer_to(fst_session, amr_offer_two_maps(fst_session, 1, "octet-align=1; crc=1", "mode-set=2"), answer, sizeof(answer)));
			fst_check(strstr(answer, "octet-align=0") != NULL);
			fst_check(strstr(answer, "crc") == NULL);
		}
		FST_SESSION_END()

		/* every mode, both formats: sizes of RFC 4867 4.3/4.4, the same bits as an independent packer, the same audio */
		FST_TEST_BEGIN(amr_encodes_every_mode_in_both_formats)
		{
			int m, f, wrong_len = 0, wrong_bits = 0, wrong_audio = 0;

			for (m = 0; m < 8; m++) {
				char fmtp_oa[64], fmtp_be[64];
				switch_codec_t enc_oa = { 0 }, enc_be = { 0 }, dec_oa = { 0 }, dec_be = { 0 };
				int16_t pcm[160];
				unsigned char oa[1][64], be[SWITCH_RECOMMENDED_BUFFER_SIZE], expected[SWITCH_RECOMMENDED_BUFFER_SIZE];
				unsigned char out_oa[SWITCH_RECOMMENDED_BUFFER_SIZE], out_be[SWITCH_RECOMMENDED_BUFFER_SIZE];
				uint32_t oa_len, be_len, expected_len, len_oa, len_be, rate = 8000, seed = 4242 + m;
				unsigned int flag = 0;

				switch_snprintf(fmtp_oa, sizeof(fmtp_oa), "mode-set=%d;octet-align=1", m);
				switch_snprintf(fmtp_be, sizeof(fmtp_be), "mode-set=%d;octet-align=0", m);
				fst_requires(amr_init(&enc_oa, fmtp_oa, SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_requires(amr_init(&enc_be, fmtp_be, SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_requires(amr_init(&dec_oa, fmtp_oa, SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_requires(amr_init(&dec_be, fmtp_be, SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);

				/* digital silence, a tone and noise: the last speech bit is set in some frames, not in others */
				for (f = 0; f < 200; f++) {
					if (f < 20) {
						memset(pcm, 0, sizeof(pcm));
					} else if (f < 100) {
						amr_tone(pcm, f);
					} else {
						amr_noise(pcm, &seed);
					}
					oa_len = amr_encode(&enc_oa, pcm, oa[0], sizeof(oa[0]));
					be_len = amr_encode(&enc_be, pcm, be, sizeof(be));

					wrong_len += oa_len != (uint32_t) (2 + amr_test_frame_bytes[m]) || be_len != (uint32_t) (amr_test_frame_bits[m] + 10 + 7) / 8 ||
						((oa[0][1] >> 3) & 0x0f) != m;
					expected_len = amr_payload(oa, 1, SWITCH_FALSE, expected, sizeof(expected));
					wrong_bits += expected_len != be_len || memcmp(expected, be, be_len);

					len_oa = sizeof(out_oa);
					len_be = sizeof(out_be);
					wrong_audio += switch_core_codec_decode(&dec_oa, NULL, oa[0], oa_len, 8000, out_oa, &len_oa, &rate, &flag) != SWITCH_STATUS_SUCCESS ||
						switch_core_codec_decode(&dec_be, NULL, be, be_len, 8000, out_be, &len_be, &rate, &flag) != SWITCH_STATUS_SUCCESS ||
						len_oa != 320 || len_be != 320 || memcmp(out_oa, out_be, 320) || (f >= 20 && !amr_nonzero(out_oa, 320));
				}

				switch_core_codec_destroy(&dec_be);
				switch_core_codec_destroy(&dec_oa);
				switch_core_codec_destroy(&enc_be);
				switch_core_codec_destroy(&enc_oa);
			}

			fst_check_int_equals(wrong_len, 0);
			fst_check_int_equals(wrong_bits, 0);
			fst_check_int_equals(wrong_audio, 0);
		}
		FST_TEST_END()

		/* the encoder filters its input in place: the caller's audio must not change */
		FST_TEST_BEGIN(amr_encoder_keeps_the_callers_audio)
		{
			switch_codec_t encoder = { 0 };
			int16_t pcm[160], copy[160];
			unsigned char encoded[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t encoded_len = sizeof(encoded), rate = 8000;
			unsigned int flag = 0;

			amr_tone(pcm, 3);
			memcpy(copy, pcm, sizeof(pcm));
			fst_requires(amr_init(&encoder, "mode-set=7;octet-align=1", SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_codec_encode(&encoder, NULL, pcm, sizeof(pcm), 8000, encoded, &encoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check(!memcmp(pcm, copy, sizeof(pcm)));
			switch_core_codec_destroy(&encoder);
		}
		FST_TEST_END()

		/* one frame per payload: from 40 ms of input the first 20 ms; less than 20 ms padded with silence */
		FST_TEST_BEGIN(amr_encodes_one_frame_per_payload)
		{
			switch_codec_t encoder = { 0 }, reference = { 0 };
			int16_t pcm[320], half[160] = { 0 };
			unsigned char encoded[SWITCH_RECOMMENDED_BUFFER_SIZE], expected[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t encoded_len, expected_len, rate = 8000;
			unsigned int flag = 0;

			amr_tone(pcm, 0);
			amr_tone(pcm + 160, 1);
			memcpy(half, pcm, 160);
			fst_requires(amr_init(&encoder, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(amr_init(&reference, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);

			encoded_len = sizeof(encoded);
			fst_requires(switch_core_codec_encode(&encoder, NULL, pcm, sizeof(pcm), 8000, encoded, &encoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			expected_len = amr_encode(&reference, pcm, expected, sizeof(expected));
			fst_check_int_equals(encoded_len, 2 + 15);
			fst_check(encoded_len == expected_len && !memcmp(encoded, expected, encoded_len));

			encoded_len = sizeof(encoded);
			fst_requires(switch_core_codec_encode(&encoder, NULL, pcm, 160, 8000, encoded, &encoded_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			expected_len = amr_encode(&reference, half, expected, sizeof(expected));
			fst_check(encoded_len == expected_len && !memcmp(encoded, expected, encoded_len));

			switch_core_codec_destroy(&reference);
			switch_core_codec_destroy(&encoder);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amr_decode)
		{
			switch_codec_t read_codec = { 0 };
			/* CMR 7, NO_DATA: bandwidth efficient, then octet aligned */
			static const unsigned char no_data_be[] = { 0x77, 0xc0 };
			static const unsigned char no_data_oa[] = { 0x70, 0x7c };
			/* a SID: octet aligned, 39 bits */
			static const unsigned char sid_oa[] = { 0xf0, 0x44, 0x2a, 0x51, 0x93, 0x7c, 0xe0 };
			unsigned char decbuf[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			uint32_t decoded_len, rate = 8000;
			unsigned int flags = 0;

			fst_requires(amr_init(&read_codec, NULL, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);

			/* valid frame types are decoded, not concealed (no WARNING) */
			amr_log_watch_start();
			decoded_len = sizeof(decbuf);
			fst_check(switch_core_codec_decode(&read_codec, NULL, (void *) no_data_be, sizeof(no_data_be), 8000, decbuf, &decoded_len, &rate, &flags) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(decoded_len, 320);
			amr_log_watch_stop();
			fst_check_int_equals(amr_warning_lines, 0);
			switch_core_codec_destroy(&read_codec);

			fst_requires(amr_init(&read_codec, "octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			amr_log_watch_start();
			decoded_len = sizeof(decbuf);
			fst_check(switch_core_codec_decode(&read_codec, NULL, (void *) no_data_oa, sizeof(no_data_oa), 8000, decbuf, &decoded_len, &rate, &flags) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(decoded_len, 320);
			decoded_len = sizeof(decbuf);
			fst_check(switch_core_codec_decode(&read_codec, NULL, (void *) sid_oa, sizeof(sid_oa), 8000, decbuf, &decoded_len, &rate, &flags) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(decoded_len, 320);
			amr_log_watch_stop();
			fst_check_int_equals(amr_warning_lines, 0);
			switch_core_codec_destroy(&read_codec);
		}
		FST_TEST_END()

		/* FT 9-14 are not AMR: the payload is concealed, the call goes on (RFC 4867 4.3.2) */
		FST_TEST_BEGIN(amr_conceals_invalid_frame_types)
		{
			int ft, wrong = 0;

			for (ft = 9; ft <= 14; ft++) {
				unsigned char oa[8] = { 0xf0, 0 }, be[8] = { 0 };
				uint32_t pos = 0;

				oa[1] = (unsigned char) ((ft << 3) | 0x04);
				amr_put_bits(be, &pos, 15, 4);
				amr_put_bits(be, &pos, (unsigned) ((ft << 1) | 1), 6);
				wrong += !amr_decodes_as_lost("octet-align=1", oa, sizeof(oa), fst_pool);
				wrong += !amr_decodes_as_lost("octet-align=0", be, sizeof(be), fst_pool);
			}
			fst_check_int_equals(wrong, 0);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amr_conceals_undecodable_payloads)
		{
			static const unsigned char short_payload[] = { 0xf0 };
			/* F set on every ToC: no last one */
			static const unsigned char unterminated_oa[] = { 0xf0, 0xbc, 0xbc, 0xbc };
			/* second ToC: FT 12 */
			static const unsigned char multi_reserved_oa[] = { 0xf0, 0xc4, 0x64, 0x2a, 0x51, 0x93, 0x7c, 0xe0 };
			switch_codec_t encoder = { 0 };
			int16_t pcm[160];
			unsigned char frame[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t frame_len;
			int oa;

			fst_check(amr_decodes_as_lost("octet-align=0", short_payload, sizeof(short_payload), fst_pool));
			fst_check(amr_decodes_as_lost("octet-align=1", short_payload, sizeof(short_payload), fst_pool));
			fst_check(amr_decodes_as_lost("octet-align=1", unterminated_oa, sizeof(unterminated_oa), fst_pool));
			fst_check(amr_decodes_as_lost("octet-align=1", multi_reserved_oa, sizeof(multi_reserved_oa), fst_pool));

			/* a mode 7 frame one byte short, in both formats */
			for (oa = 0; oa < 2; oa++) {
				const char *fmtp = oa ? "mode-set=7;octet-align=1" : "mode-set=7;octet-align=0";

				amr_noise(pcm, &(uint32_t) { 99 });
				fst_requires(amr_init(&encoder, fmtp, SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				frame_len = amr_encode(&encoder, pcm, frame, sizeof(frame));
				switch_core_codec_destroy(&encoder);
				fst_requires(frame_len > 2);
				fst_check(amr_decodes_as_lost(fmtp, frame, frame_len - 1, fst_pool));
			}
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amr_ignores_padding_after_the_frame)
		{
			const char *fmtps[] = { "mode-set=2;octet-align=1", "mode-set=2;octet-align=0", "mode-set=7;octet-align=1", "mode-set=7;octet-align=0" };
			int i;

			for (i = 0; i < 4; i++) {
				switch_codec_t encoder = { 0 };
				int16_t pcm[160];
				unsigned char frame[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
				uint32_t frame_len;

				amr_tone(pcm, 0);
				fst_requires(amr_init(&encoder, fmtps[i], SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				frame_len = amr_encode(&encoder, pcm, frame, sizeof(frame));
				switch_core_codec_destroy(&encoder);
				fst_requires(frame_len);

				fst_check(amr_decode_same(fmtps[i], frame, frame_len, fmtps[i], frame, frame_len + 3, fst_pool));
				fst_check(!amr_decodes_as_lost(fmtps[i], frame, frame_len, fst_pool));
				fst_check(!amr_decodes_as_lost(fmtps[i], frame, frame_len + 3, fst_pool));
				fst_check(amr_decodes_as_lost(fmtps[i], frame, frame_len + 4, fst_pool));

				/* RTP padding (RFC 3550 5.1): any length, count in the last byte */
				frame[frame_len + 7] = 7;
				fst_check(amr_decodes_as_lost(fmtps[i], frame, frame_len + 8, fst_pool));
				frame[frame_len + 7] = 8;
				fst_check(amr_decode_same(fmtps[i], frame, frame_len, fmtps[i], frame, frame_len + 8, fst_pool));
				frame[frame_len + 7] = 0;
			}
		}
		FST_TEST_END()

		/* a payload in the other format decodes as such: the far end may still send it after a re-INVITE */
		FST_TEST_BEGIN(amr_decodes_the_other_format)
		{
			int m;

			for (m = 0; m < 8; m++) {
				char fmtp_oa[64], fmtp_be[64];
				switch_codec_t enc_oa = { 0 }, enc_be = { 0 };
				int16_t pcm[160];
				unsigned char oa[SWITCH_RECOMMENDED_BUFFER_SIZE], be[SWITCH_RECOMMENDED_BUFFER_SIZE];
				uint32_t oa_len, be_len;

				switch_snprintf(fmtp_oa, sizeof(fmtp_oa), "mode-set=%d;octet-align=1", m);
				switch_snprintf(fmtp_be, sizeof(fmtp_be), "mode-set=%d;octet-align=0", m);
				amr_tone(pcm, 2);
				fst_requires(amr_init(&enc_oa, fmtp_oa, SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_requires(amr_init(&enc_be, fmtp_be, SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				oa_len = amr_encode(&enc_oa, pcm, oa, sizeof(oa));
				be_len = amr_encode(&enc_be, pcm, be, sizeof(be));
				switch_core_codec_destroy(&enc_be);
				switch_core_codec_destroy(&enc_oa);

				fst_check(amr_decode_same(fmtp_be, oa, oa_len, fmtp_oa, oa, oa_len, fst_pool));
				fst_check(amr_decode_same(fmtp_oa, be, be_len, fmtp_be, be, be_len, fst_pool));
			}
		}
		FST_TEST_END()

		/* frames of one payload decode as the same frames in a payload each */
		FST_TEST_BEGIN(amr_decodes_multiple_frames)
		{
			int oa;

			for (oa = 0; oa < 2; oa++) {
				const char *fmtp = oa ? "mode-set=0,1,2;octet-align=1" : "mode-set=0,1,2;octet-align=0";
				switch_codec_t encoder = { 0 }, multi = { 0 }, single = { 0 };
				unsigned char frames[3][64], payload[512];
				unsigned char out_multi[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 }, out_single[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
				int16_t pcm[160];
				uint32_t len, out_len, single_len = 0, rate = 8000;
				unsigned int flag = 0;
				int f;

				fst_requires(amr_init(&encoder, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				for (f = 0; f < 3; f++) {
					amr_tone(pcm, f);
					fst_requires(amr_encode(&encoder, pcm, frames[f], sizeof(frames[f])));
				}
				switch_core_codec_destroy(&encoder);
				/* a NO_DATA frame between speech frames */
				frames[1][1] = (15 << 3) | 0x04;

				fst_requires(amr_init(&multi, fmtp, SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_requires(amr_init(&single, fmtp, SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);

				len = amr_payload(frames, 3, oa, payload, sizeof(payload));
				out_len = sizeof(out_multi);
				fst_check(switch_core_codec_decode(&multi, NULL, payload, len, 8000, out_multi, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				fst_check_int_equals(out_len, 3 * 320);

				for (f = 0; f < 3; f++) {
					uint32_t one = 320;

					len = amr_payload(&frames[f], 1, oa, payload, sizeof(payload));
					fst_check(switch_core_codec_decode(&single, NULL, payload, len, 8000, out_single + single_len, &one, &rate, &flag) == SWITCH_STATUS_SUCCESS);
					single_len += one;
				}
				fst_check_int_equals(single_len, 3 * 320);
				fst_check(!memcmp(out_multi, out_single, 3 * 320));
				fst_check(amr_nonzero(out_multi, 320) && amr_nonzero(out_multi + 640, 320));

				switch_core_codec_destroy(&single);
				switch_core_codec_destroy(&multi);
			}
		}
		FST_TEST_END()

		/* 12 frames per payload (240 ms) are decoded, 13 are not */
		FST_TEST_BEGIN(amr_decodes_at_most_12_frames)
		{
			switch_codec_t codec = { 0 };
			unsigned char payload[16], out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len, rate = 8000;
			unsigned int flag = 0;
			int n;

			fst_requires(amr_init(&codec, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			for (n = 12; n <= 13; n++) {
				/* CMR 15, then NO_DATA ToCs: F set on all but the last */
				payload[0] = 0xf0;
				memset(payload + 1, 0xfc, n - 1);
				payload[n] = 0x7c;
				out_len = sizeof(out);
				fst_requires(switch_core_codec_decode(&codec, NULL, payload, n + 1, 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				fst_check_int_equals(out_len, n == 12 ? 12 * 320 : 320);
			}
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* no more audio than the RTP timestamps advance */
		FST_TEST_BEGIN(amr_timestamps_bound_the_decoded_audio)
		{
			switch_codec_t codec = { 0 };
			switch_frame_t frame = { 0 };
			/* three NO_DATA frames: 60 ms */
			static const unsigned char three_oa[] = { 0xf0, 0xfc, 0xfc, 0x7c };
			const struct { uint32_t ts; uint32_t len; } cases[] = {
				{ 0, 3 * 320 },
				{ 160, 320 },			/* starts 40 ms before the previous one ends */
				{ 640, 3 * 320 },		/* right after it */
				{ 640, 320 },			/* again */
				{ 960, 2 * 320 },		/* 20 ms before it ends */
				{ 100000, 3 * 320 },	/* a gap */
				{ 0, 320 },				/* back: one frame, then in step again */
				{ 480, 3 * 320 },
				{ 0xfffffe00, 320 },	/* back across the wrap */
				{ 0xffffffe0, 3 * 320 },
				{ 0x1c0, 3 * 320 }		/* in step across the wrap */
			};
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len, rate = 8000;
			unsigned int flag = 0;
			int i, wrong = 0;

			fst_requires(amr_init(&codec, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			codec.cur_frame = &frame;
			for (i = 0; i < (int) (sizeof(cases) / sizeof(cases[0])); i++) {
				frame.timestamp = cases[i].ts;
				out_len = sizeof(out);
				fst_requires(switch_core_codec_decode(&codec, NULL, (void *) three_oa, sizeof(three_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				wrong += out_len != cases[i].len;
			}
			/* without a frame (no timestamps): every frame */
			codec.cur_frame = NULL;
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) three_oa, sizeof(three_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(out_len, 3 * 320);
			fst_check_int_equals(wrong, 0);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* Q=0 (damaged frame): decoded as a lost frame */
		FST_TEST_BEGIN(amr_q0_frame_is_decoded_as_lost)
		{
			switch_codec_t encoder = { 0 }, damaged = { 0 }, lost = { 0 };
			static const unsigned char lost_oa[] = { 0xf0, 0x7c };
			int16_t pcm[160];
			unsigned char frames[10][64];
			uint32_t frame_len[10];
			unsigned char out_damaged[SWITCH_RECOMMENDED_BUFFER_SIZE], out_lost[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t len_damaged, len_lost, rate = 8000;
			unsigned int flag = 0;
			int f, differ = 0;

			fst_requires(amr_init(&encoder, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(amr_init(&damaged, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(amr_init(&lost, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);

			for (f = 0; f < 10; f++) {
				amr_tone(pcm, f);
				fst_requires((frame_len[f] = amr_encode(&encoder, pcm, frames[f], sizeof(frames[f]))));
			}
			frames[5][1] &= ~0x04;

			/* same history, then the Q=0 frame vs a lost frame, then the same good frames */
			for (f = 0; f < 10; f++) {
				len_damaged = sizeof(out_damaged);
				fst_requires(switch_core_codec_decode(&damaged, NULL, frames[f], frame_len[f], 8000, out_damaged, &len_damaged, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				len_lost = sizeof(out_lost);
				if (f != 5) {
					fst_requires(switch_core_codec_decode(&lost, NULL, frames[f], frame_len[f], 8000, out_lost, &len_lost, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				} else {
					fst_requires(switch_core_codec_decode(&lost, NULL, (void *) lost_oa, sizeof(lost_oa), 8000, out_lost, &len_lost, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				}
				differ += f >= 5 && (len_damaged != len_lost || memcmp(out_damaged, out_lost, len_lost));
			}

			fst_check_int_equals(differ, 0);

			switch_core_codec_destroy(&lost);
			switch_core_codec_destroy(&damaged);
			switch_core_codec_destroy(&encoder);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amr_concealment_does_not_flood_the_log)
		{
			switch_codec_t codec = { 0 };
			static const unsigned char reserved_oa[] = { 0xf0, 0x54 };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len, rate = 8000;
			unsigned int flag = 0;
			int i;

			fst_requires(amr_init(&codec, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			amr_log_watch_start();
			for (i = 0; i < 250; i++) {
				out_len = sizeof(out);
				switch_core_codec_decode(&codec, NULL, (void *) reserved_oa, sizeof(reserved_oa), 8000, out, &out_len, &rate, &flag);
			}
			amr_log_watch_stop();
			/* the 1st of 250 concealed payloads */
			fst_check_int_equals(amr_warning_lines, 1);

			amr_log_watch_start();
			out_len = sizeof(out);
			switch_core_codec_decode(&codec, NULL, (void *) reserved_oa, sizeof(reserved_oa), 8000, out, &out_len, &rate, &flag);
			amr_log_watch_stop();
			switch_core_codec_destroy(&codec);
			/* and the 251st */
			fst_check_int_equals(amr_warning_lines, 1);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amr_encoder_follows_received_cmr)
		{
			switch_codec_t codec = { 0 };
			static const unsigned char cmr1_oa[] = { 0x10, 0x7c };
			static const unsigned char cmr15_oa[] = { 0xf0, 0x7c };
			static const unsigned char cmr7_oa[] = { 0x70, 0x7c };
			static const unsigned char cmr9_oa[] = { 0x90, 0x7c };
			static const unsigned char cmr4_oa[] = { 0x40, 0x7c };
			static const unsigned char cmr2_oa[] = { 0x20, 0x7c };
			static const unsigned char cmr0_oa[] = { 0x00, 0x7c };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len, rate = 8000;
			unsigned int flag = 0;

			fst_requires(amr_init(&codec, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 2);

			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr1_oa, sizeof(cmr1_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 1);

			/* outside the mode-set: ignored, the previous request stays */
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr7_oa, sizeof(cmr7_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 1);

			/* reserved CMR: ignored */
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr9_oa, sizeof(cmr9_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 1);

			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr15_oa, sizeof(cmr15_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 2);
			switch_core_codec_destroy(&codec);

			/* non-contiguous mode-set: requests outside it are ignored */
			fst_requires(amr_init(&codec, "mode-set=0,2,7;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr4_oa, sizeof(cmr4_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 7);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr2_oa, sizeof(cmr2_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 2);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 0);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amr_encoder_steps_to_received_cmr)
		{
			switch_codec_t codec = { 0 };
			static const unsigned char cmr0_oa[] = { 0x00, 0x7c };
			static const unsigned char cmr15_oa[] = { 0xf0, 0x7c };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len, rate = 8000;
			unsigned int flag = 0;
			int ft;

			/* neighbor, every 2nd frame */
			fst_requires(amr_init(&codec, "mode-set=0,1,2;mode-change-neighbor=1;mode-change-period=2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			ft = amr_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 2);
			ft = amr_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 2);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			ft = amr_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 1);
			ft = amr_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 1);
			ft = amr_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 0);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr15_oa, sizeof(cmr15_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			ft = amr_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 0);
			ft = amr_encoded_ft(&codec, SWITCH_TRUE);
			fst_check_int_equals(ft, 1);
			switch_core_codec_destroy(&codec);

			/* a repeated mode-change-neighbor: the last one, also for fmtp_changes_framing */
			fst_requires(amr_init(&codec, "mode-set=0,1,2;mode-change-neighbor=1;mode-change-neighbor=0;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 2);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 0);
			fst_check(!amr_framing_changes(&codec, "mode-set=0,1,2;mode-change-neighbor=1;mode-change-neighbor=0;octet-align=1"));
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* the CMR of a payload that is concealed is ignored (RFC 4867 4.3.2, 4.5.1: discarded) */
		FST_TEST_BEGIN(amr_concealed_payload_cmr_is_ignored)
		{
			switch_codec_t codec = { 0 };
			static const unsigned char cmr0_reserved_oa[] = { 0x00, 0x54 };
			/* CMR 0, FT 12, then zeros */
			static const unsigned char cmr0_reserved_be[] = { 0x06, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00 };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len = sizeof(out), rate = 8000;
			unsigned int flag = 0;

			fst_requires(amr_init(&codec, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr0_reserved_oa, sizeof(cmr0_reserved_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 2);
			switch_core_codec_destroy(&codec);

			/* bandwidth efficient, FT 12: it reads as an octet aligned payload only with reserved bits set
			 * (not one a sender makes): concealed as well */
			fst_requires(amr_init(&codec, "mode-set=0,1,2;octet-align=0", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr0_reserved_be, sizeof(cmr0_reserved_be), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(out_len, 320);
			amr_check_ft(&codec, SWITCH_FALSE, 2);
			switch_core_codec_destroy(&codec);
			fst_check(amr_decodes_as_lost("octet-align=0", cmr0_reserved_be, sizeof(cmr0_reserved_be), fst_pool));
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amr_only_the_read_codec_passes_cmr_to_the_write_codec)
		{
			switch_core_session_t *session;
			switch_codec_t writer = { 0 }, reader = { 0 }, other = { 0 };
			static const unsigned char cmr0_oa[] = { 0x00, 0x7c };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len, rate = 8000;
			unsigned int flag = 0;
			int ft;

			fst_requires((session = amr_cmr_session(&reader, &writer, fst_pool)));
			fst_requires(amr_init(&other, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			other.session = session;

			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&other, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			ft = amr_encoded_ft(&writer, SWITCH_TRUE);
			fst_check_int_equals(ft, 2);

			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&reader, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			ft = amr_encoded_ft(&writer, SWITCH_TRUE);
			fst_check_int_equals(ft, 0);

			switch_core_codec_destroy(&other);
			amr_cmr_session_end(session, &reader, &writer);
		}
		FST_TEST_END()

		/* the request ends with the read codec that received it (e.g. replaced on re-INVITE) */
		FST_TEST_BEGIN(amr_cmr_request_ends_with_the_read_codec)
		{
			switch_core_session_t *session;
			switch_codec_t writer = { 0 }, reader = { 0 }, reader2 = { 0 };
			static const unsigned char cmr0_oa[] = { 0x00, 0x7c };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len = sizeof(out), rate = 8000;
			unsigned int flag = 0;
			int ft;

			fst_requires((session = amr_cmr_session(&reader, &writer, fst_pool)));
			fst_requires(switch_core_codec_decode(&reader, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			ft = amr_encoded_ft(&writer, SWITCH_TRUE);
			fst_check_int_equals(ft, 0);

			switch_core_session_unset_read_codec(session);
			switch_core_codec_destroy(&reader);
			fst_requires(amr_init(&reader2, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_session_set_read_codec(session, &reader2) == SWITCH_STATUS_SUCCESS);
			reader2.session = session;
			ft = amr_encoded_ft(&writer, SWITCH_TRUE);
			fst_check_int_equals(ft, 2);

			amr_cmr_session_end(session, &reader2, &writer);
		}
		FST_TEST_END()

		/* a media bug decodes with a copy of the read codec, without a session: its CMR still reaches the write codec */
		FST_TEST_BEGIN(amr_media_bug_codec_passes_cmr)
		{
			switch_core_session_t *session;
			switch_codec_t writer = { 0 }, reader = { 0 }, bug = { 0 };
			switch_frame_t frame = { 0 };
			static const unsigned char cmr0_oa[] = { 0x00, 0x7c };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len = sizeof(out), rate = 8000;
			unsigned int flag = 0;
			int ft;

			fst_requires((session = amr_cmr_session(&reader, &writer, fst_pool)));
			fst_requires(switch_core_codec_copy(&reader, &bug, NULL, NULL) == SWITCH_STATUS_SUCCESS);
			fst_requires(!bug.session);
			frame.codec = &reader;
			bug.cur_frame = &frame;
			fst_requires(switch_core_codec_decode(&bug, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			bug.cur_frame = NULL;
			ft = amr_encoded_ft(&writer, SWITCH_TRUE);
			fst_check_int_equals(ft, 0);

			switch_core_codec_destroy(&bug);
			amr_cmr_session_end(session, &reader, &writer);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amr_bitrate_adjustment_stays_in_mode_set)
		{
			switch_codec_t codec = { 0 };

			fst_requires(amr_init(&codec, "mode-set=0,2,5;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 5);
			amr_adjust(&codec, "increase");
			amr_check_ft(&codec, SWITCH_TRUE, 5);
			amr_adjust(&codec, "decrease");
			amr_check_ft(&codec, SWITCH_TRUE, 2);
			amr_adjust(&codec, "decrease");
			amr_check_ft(&codec, SWITCH_TRUE, 0);
			amr_adjust(&codec, "decrease");
			amr_check_ft(&codec, SWITCH_TRUE, 0);
			amr_adjust(&codec, "increase");
			amr_check_ft(&codec, SWITCH_TRUE, 2);
			/* default-bitrate 7 is not in the mode-set: the highest mode below it */
			amr_adjust(&codec, "default");
			amr_check_ft(&codec, SWITCH_TRUE, 5);
			amr_adjust(&codec, "minimum");
			amr_check_ft(&codec, SWITCH_TRUE, 0);
			switch_core_codec_destroy(&codec);

			fst_requires(amr_init(&codec, "mode-set=1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			amr_adjust(&codec, "minimum");
			amr_check_ft(&codec, SWITCH_TRUE, 1);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* re-INVITE: a new codec only when octet-align, the answered or encoded mode-set, or the mode-change parameters change */
		FST_TEST_BEGIN(amr_reports_fmtp_framing_changes)
		{
			switch_codec_t codec = { 0 };
			const char *r;

			fst_requires(amr_init(&codec, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			r = amr_changes_framing(&codec, "mode-set=0,1,2;octet-align=1");
			fst_check(r && !strcmp(r, "false"));
			r = amr_changes_framing(&codec, "octet-align=1; mode-set=2,1,0");
			fst_check(r && !strcmp(r, "false"));
			r = amr_changes_framing(&codec, "mode-set=0,1,2");
			fst_check(r && !strcmp(r, "true"));
			r = amr_changes_framing(&codec, "mode-set=0,1;octet-align=1");
			fst_check(r && !strcmp(r, "true"));
			r = amr_changes_framing(&codec, "mode-set=0,1,2;octet-align=1;mode-change-period=1");
			fst_check(r && !strcmp(r, "false"));
			r = amr_changes_framing(&codec, "mode-set=0,1,2;octet-align=1;mode-change-period=2");
			fst_check(r && !strcmp(r, "true"));
			r = amr_changes_framing(&codec, "mode-set=0,1,2;octet-align=1;mode-change-neighbor=1");
			fst_check(r && !strcmp(r, "true"));
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amr_mode_set_answers)
		{
			switch_codec_t codec = { 0 };

			fst_requires(amr_init(&codec, "mode-set=2,7;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "mode-set=2,7;"));
			amr_check_ft(&codec, SWITCH_TRUE, 7);
			switch_core_codec_destroy(&codec);

			/* invalid and non-numeric entries are dropped; none left: as if there were no mode-set */
			fst_requires(amr_init(&codec, "mode-set=1,9;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "mode-set=1;"));
			amr_check_ft(&codec, SWITCH_TRUE, 1);
			switch_core_codec_destroy(&codec);

			fst_requires(amr_init(&codec, "mode-set=abc,2,,5;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "mode-set=2,5;"));
			switch_core_codec_destroy(&codec);

			/* no mode-set offered, none configured: default-bitrate */
			fst_requires(amr_init(&codec, "octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "mode-set=7;"));
			amr_check_ft(&codec, SWITCH_TRUE, 7);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* fmtp-extra is appended, it does not replace octet-align */
		FST_TEST_BEGIN(amr_answer_keeps_octet_align_with_fmtp_extra)
		{
			switch_codec_t codec = { 0 };

			fst_requires(amr_init(&codec, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "octet-align=1"));
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "x-extra=1"));
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* spaces around "=" and after values, upper case keys, a repeated octet-align, many parameters */
		FST_TEST_BEGIN(amr_parses_fmtp)
		{
			const struct { const char *fmtp; const char *answer; uint32_t len; } cases[] = {
				{ "mode-set = 2; octet-align = 0", "mode-set=2;", 16 },
				{ "OCTET-ALIGN=1 ;Mode-Set=2 ", "mode-set=2;", 17 },
				{ "mode-set=2;octet-align=1;octet-align=0", "mode-set=2;", 16 },
				{ "mode-set=2;octet-align=true", "mode-set=2;", 17 },
				{ "a=1;b=1;c=1;d=1;e=1;f=1;g=1;h=1;i=1;j=1;k=1;mode-set=2;octet-align=1", "mode-set=2;", 17 }
			};
			int i;

			for (i = 0; i < (int) (sizeof(cases) / sizeof(cases[0])); i++) {
				switch_codec_t codec = { 0 };
				int16_t pcm[160] = { 0 };
				unsigned char encoded[SWITCH_RECOMMENDED_BUFFER_SIZE];

				fst_requires(amr_init(&codec, cases[i].fmtp, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_check(codec.fmtp_out && strstr(codec.fmtp_out, cases[i].answer));
				fst_check_int_equals(amr_encode(&codec, pcm, encoded, sizeof(encoded)), cases[i].len);
				switch_core_codec_destroy(&codec);
			}
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amr_does_not_match_unsupported_payload_options)
		{
			switch_codec_interface_t *codec_interface = switch_loadable_module_get_codec_interface("AMR", "mod_amr");
			const switch_codec_implementation_t *impl;
			int matched = 0, unsupported_matched = 0;

			fst_requires(codec_interface);
			for (impl = codec_interface->implementations; impl; impl = impl->next) {
				fst_requires(impl->matches_fmtp);
				matched += impl->matches_fmtp("octet-align=1", impl->fmtp) == SWITCH_STATUS_SUCCESS;
				matched += impl->matches_fmtp("octet-align=1;mode-set=0, 1 ,7,9", impl->fmtp) == SWITCH_STATUS_SUCCESS;
				matched += impl->matches_fmtp("octet-align=1;crc=0;robust-sorting=0;interleaving=0", impl->fmtp) == SWITCH_STATUS_SUCCESS;
				unsupported_matched += impl->matches_fmtp("octet-align=1;crc=1", impl->fmtp) == SWITCH_STATUS_SUCCESS;
				unsupported_matched += impl->matches_fmtp("octet-align=1;robust-sorting=1", impl->fmtp) == SWITCH_STATUS_SUCCESS;
				unsupported_matched += impl->matches_fmtp("octet-align=1;interleaving=4", impl->fmtp) == SWITCH_STATUS_SUCCESS;
				unsupported_matched += impl->matches_fmtp("octet-align=1;mode-set=8,9", impl->fmtp) == SWITCH_STATUS_SUCCESS;
				unsupported_matched += impl->matches_fmtp("octet-align=1;channels=2", impl->fmtp) == SWITCH_STATUS_SUCCESS;
			}
			UNPROTECT_INTERFACE(codec_interface);

			/* each accepted fmtp by the octet-aligned implementation */
			fst_check_int_equals(matched, 3);
			fst_check_int_equals(unsupported_matched, 0);
		}
		FST_TEST_END()

		/* crc, robust-sorting and interleaving offered (as 0) are in the answer (RFC 4867 8.3.1) */
		FST_TEST_BEGIN(amr_answer_returns_offered_options)
		{
			switch_codec_t codec = { 0 };

			fst_requires(amr_init(&codec, "mode-set=2;octet-align=1;crc=0;robust-sorting=0;interleaving=0", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, ";crc=0") && strstr(codec.fmtp_out, ";robust-sorting=0") && strstr(codec.fmtp_out, ";interleaving=0"));
			switch_core_codec_destroy(&codec);

			fst_requires(amr_init(&codec, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && !strstr(codec.fmtp_out, "crc") && !strstr(codec.fmtp_out, "robust-sorting") && !strstr(codec.fmtp_out, "interleaving"));
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* both payload types are offered, each with its framing and the mode-set */
		FST_TEST_BEGIN(amr_offers_both_payload_types)
		{
			switch_codec_interface_t *codec_interface = switch_loadable_module_get_codec_interface("AMR", "mod_amr");
			const switch_codec_implementation_t *impl;
			int oa = 0, be = 0;

			fst_requires(codec_interface);
			for (impl = codec_interface->implementations; impl; impl = impl->next) {
				fst_check(impl->fmtp && strstr(impl->fmtp, "mode-set=7"));
				oa += impl->ianacode == 96 && impl->fmtp && strstr(impl->fmtp, "octet-align=1") != NULL;
				be += impl->ianacode == 97 && impl->fmtp && strstr(impl->fmtp, "octet-align=0") != NULL;
			}
			UNPROTECT_INTERFACE(codec_interface);
			fst_check_int_equals(oa, 1);
			fst_check_int_equals(be, 1);
		}
		FST_TEST_END()

		/* the codec runs on the implementation (payload type) of its format */
		FST_TEST_BEGIN(amr_runs_on_the_payload_type_of_its_format)
		{
			const struct { const char *fmtp; const char *impl; } cases[] = {
				{ "mode-set=2;octet-align=1", "octet-align=1" },
				{ "mode-set=2;octet-align=0", "octet-align=0" },
				{ "mode-set=2", "octet-align=0" }
			};
			int i;

			for (i = 0; i < 3; i++) {
				switch_codec_t codec = { 0 };

				fst_requires(amr_init(&codec, cases[i].fmtp, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_check(codec.implementation && codec.implementation->fmtp && strstr(codec.implementation->fmtp, cases[i].impl));
				switch_core_codec_reset(&codec);
				fst_check(codec.implementation && codec.implementation->fmtp && strstr(codec.implementation->fmtp, cases[i].impl));
				switch_core_codec_destroy(&codec);
			}
		}
		FST_TEST_END()

		/* force-oa: for an offer that does not state octet-align (a codec without a session takes the fmtp as an
		 * offer); a stated octet-align wins; not for answers (outbound legs); the payload type follows */
		FST_TEST_BEGIN(amr_force_oa_applies_to_offers_without_octet_align)
		{
			switch_core_session_t *session = NULL;
			switch_call_cause_t cause;
			const struct { const char *fmtp; uint32_t len; } cases[] = {
				{ "mode-set=2;octet-align=0", 16 },
				{ "mode-set=2;octet-align=1", 17 },
				{ "mode-set=2", 17 }
			};
			int16_t pcm[160] = { 0 };
			unsigned char encoded[SWITCH_RECOMMENDED_BUFFER_SIZE];
			switch_codec_t codec = { 0 };
			int i;

			fst_requires(amr_reload("<param name=\"force-oa\" value=\"true\"/><param name=\"fmtp-extra\" value=\"x-extra=1\"/>") == SWITCH_STATUS_SUCCESS);
			for (i = 0; i < 3; i++) {
				fst_requires(amr_init(&codec, cases[i].fmtp, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_check_int_equals(amr_encode(&codec, pcm, encoded, sizeof(encoded)), cases[i].len);
				fst_check(codec.implementation && strstr(codec.implementation->fmtp, cases[i].len == 17 ? "octet-align=1" : "octet-align=0"));
				switch_core_codec_destroy(&codec);
			}

			fst_requires(switch_ivr_originate(NULL, &session, &cause, "null/amr-force", 0, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_channel_direction(switch_core_session_get_channel(session)) == SWITCH_CALL_DIRECTION_OUTBOUND);
			fst_requires(amr_init(&codec, "mode-set=2", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS);
			fst_requires(codec.session == session);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "octet-align=0"));
			fst_check_int_equals(amr_encode(&codec, pcm, encoded, sizeof(encoded)), 16);
			switch_core_codec_destroy(&codec);
			switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session);

			fst_requires(amr_reload(NULL) == SWITCH_STATUS_SUCCESS);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amr_config)
		{
			/* fst_check prints its expression as a format: the literal stays out of it */
			const char *literal = "fmtp-extra: [x-pct=%s%s%s]";
			const char *settings = "<param name=\"default-bitrate\" value=\"2\"/><param name=\"default-bitrate\" value=\"8\"/>"
				"<param name=\"mode-set\" value=\"0,2,9\"/>"
				"<param name=\"volte\" value=\"on\"/><param name=\"adjust-bitrate\" value=\"yes\"/>"
				"<param name=\"silence-supp-off\" value=\"true\"/>"
				"<param name=\"fmtp-extra\" value=\"x-pct=%s%s%s\"/>";

			fst_requires(amr_reload(settings) == SWITCH_STATUS_SUCCESS);
			fst_check(amr_show_has("default-bitrate: 2,"));
			fst_check(amr_show_has("modes: [0,2],"));
			fst_check(amr_show_has("volte: 1, adjust-bitrate: 1,"));
			fst_check(amr_show_has("silence-supp-off: 1"));
			fst_check(amr_show_has(literal));
			fst_requires(amr_reload(NULL) == SWITCH_STATUS_SUCCESS);
			fst_check(amr_show_has("default-bitrate: 7,"));
			fst_check(amr_show_has("modes: [],"));
		}
		FST_TEST_END()

		/* mode-set-overwrite: answer our own mode-set; encode within both */
		FST_TEST_BEGIN(amr_mode_set_overwrite)
		{
			switch_codec_t codec = { 0 };

			fst_requires(amr_reload("<param name=\"mode-set\" value=\"0,1,2\"/><param name=\"mode-set-overwrite\" value=\"1\"/>"
									"<param name=\"mode-set-overwrite-with-default-bitrate\" value=\"0\"/>") == SWITCH_STATUS_SUCCESS);
			fst_requires(amr_init(&codec, "mode-set=1,7;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "mode-set=0,1,2;"));
			amr_check_ft(&codec, SWITCH_TRUE, 1);
			switch_core_codec_destroy(&codec);
			fst_requires(amr_reload(NULL) == SWITCH_STATUS_SUCCESS);
		}
		FST_TEST_END()

		/* volte: the VoLTE parameters in answers, octet-align kept */
		FST_TEST_BEGIN(amr_volte_answer)
		{
			switch_codec_t codec = { 0 };

			fst_requires(amr_reload("<param name=\"volte\" value=\"1\"/><param name=\"fmtp-extra\" value=\"x-extra=1\"/>") == SWITCH_STATUS_SUCCESS);
			fst_requires(amr_init(&codec, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "octet-align=1;max-red=0;mode-change-capability=2"));
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "x-extra=1"));
			switch_core_codec_destroy(&codec);
			fst_requires(amr_reload(NULL) == SWITCH_STATUS_SUCCESS);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amr_silence_supp_off_sets_suppress_cng)
		{
			switch_core_session_t *session = NULL;
			switch_call_cause_t cause;
			switch_codec_t codec = { 0 };

			fst_requires(amr_reload("<param name=\"silence-supp-off\" value=\"true\"/>") == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_ivr_originate(NULL, &session, &cause, "null/amr-cng", 0, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) == SWITCH_STATUS_SUCCESS);
			fst_check(!switch_channel_var_true(switch_core_session_get_channel(session), "suppress_cng"));
			fst_requires(amr_init(&codec, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_var_true(switch_core_session_get_channel(session), "suppress_cng"));
			switch_core_codec_destroy(&codec);
			switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session);
			fst_requires(amr_reload(NULL) == SWITCH_STATUS_SUCCESS);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amr_control_and_destroy_without_context)
		{
			switch_codec_t codec = { 0 };
			int32_t level = 1;

			amr_init(&codec, "octet-align=1", 0, fst_pool);
			fst_check(switch_core_codec_control(&codec, SCC_DEBUG, SCCT_INT, &level, SCCT_NONE, NULL, NULL, NULL) == SWITCH_STATUS_FALSE);
			fst_check(switch_core_codec_control(&codec, SCC_AUDIO_ADJUST_BITRATE, SCCT_STRING, (void *) "increase", SCCT_NONE, NULL, NULL, NULL) == SWITCH_STATUS_FALSE);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* switch_core_codec_reset re-inits on the same pool: the context is reused, not allocated again */
		FST_TEST_BEGIN(amr_reset_reuses_the_context)
		{
			switch_codec_t codec = { 0 };
			void *context;
			static const unsigned char cmr0_oa[] = { 0x00, 0x7c };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len = sizeof(out), rate = 8000;
			unsigned int flag = 0;
			int i;

			fst_requires(amr_init(&codec, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 0);
			context = codec.private_info;
			for (i = 0; i < 3; i++) {
				switch_core_codec_reset(&codec);
				fst_check(codec.private_info == context);
			}
			/* a fresh codec: no request, the highest mode */
			amr_check_ft(&codec, SWITCH_TRUE, 2);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
#endif
