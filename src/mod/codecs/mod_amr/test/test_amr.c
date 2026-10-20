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
#include "opencore-amrnb/interf_enc.h"

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

/* decode the same five frames of a 440 Hz tone on both decoders; FALSE if that failed */
static switch_bool_t amr_prime(switch_codec_t *a, switch_codec_t *b, const char *fmtp, switch_memory_pool_t *pool)
{
	switch_codec_t encoder = { 0 };
	int16_t pcm[160];
	unsigned char frame[SWITCH_RECOMMENDED_BUFFER_SIZE], out[SWITCH_RECOMMENDED_BUFFER_SIZE];
	uint32_t frame_len, out_len, rate = 8000;
	unsigned int flag = 0;
	int f, ok = 1;

	if (amr_init(&encoder, fmtp, SWITCH_CODEC_FLAG_ENCODE, pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_FALSE;
	}
	for (f = 0; f < 5 && ok; f++) {
		amr_tone(pcm, f);
		frame_len = amr_encode(&encoder, pcm, frame, sizeof(frame));
		out_len = sizeof(out);
		ok = frame_len && switch_core_codec_decode(a, NULL, frame, frame_len, 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS;
		out_len = sizeof(out);
		ok = ok && switch_core_codec_decode(b, NULL, frame, frame_len, 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS;
	}
	switch_core_codec_destroy(&encoder);
	return ok ? SWITCH_TRUE : SWITCH_FALSE;
}

/* after the same history, a decoder decodes payload exactly as it decodes a NO_DATA frame: 1,
 * differently: 0, the test could not run: -1 */
static int amr_lost_compare(const char *fmtp, const unsigned char *payload, uint32_t len, switch_memory_pool_t *pool)
{
	static const unsigned char lost_be[] = { 0xf7, 0xc0 };
	static const unsigned char lost_oa[] = { 0xf0, 0x7c };
	switch_codec_t codec = { 0 }, reference = { 0 };
	unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 }, ref[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
	uint32_t out_len = sizeof(out), ref_len = sizeof(ref), rate = 8000;
	unsigned int flag = 0;
	switch_bool_t oa = strstr(fmtp, "octet-align=1") ? SWITCH_TRUE : SWITCH_FALSE;
	switch_status_t status, ref_status;

	switch_bool_t primed;

	if (amr_init(&codec, fmtp, SWITCH_CODEC_FLAG_DECODE, pool) != SWITCH_STATUS_SUCCESS ||
		amr_init(&reference, fmtp, SWITCH_CODEC_FLAG_DECODE, pool) != SWITCH_STATUS_SUCCESS) {
		return -1;
	}
	/* same speech history first, so concealment output is not silence */
	primed = amr_prime(&codec, &reference, fmtp, pool);
	status = switch_core_codec_decode(&codec, NULL, (void *) payload, len, 8000, out, &out_len, &rate, &flag);
	ref_status = switch_core_codec_decode(&reference, NULL, (void *) (oa ? lost_oa : lost_be), 2, 8000, ref, &ref_len, &rate, &flag);
	switch_core_codec_destroy(&codec);
	switch_core_codec_destroy(&reference);

	if (!primed || status != SWITCH_STATUS_SUCCESS || ref_status != SWITCH_STATUS_SUCCESS || ref_len != 320 || !amr_nonzero(ref, ref_len)) {
		return -1;
	}
	return (out_len == ref_len && !memcmp(out, ref, out_len)) ? 1 : 0;
}

static switch_bool_t amr_decodes_as_lost(const char *fmtp, const unsigned char *payload, uint32_t len, switch_memory_pool_t *pool)
{
	return amr_lost_compare(fmtp, payload, len, pool) == 1 ? SWITCH_TRUE : SWITCH_FALSE;
}

/* decoded, not concealed (and the test could run) */
static switch_bool_t amr_decodes_not_as_lost(const char *fmtp, const unsigned char *payload, uint32_t len, switch_memory_pool_t *pool)
{
	return amr_lost_compare(fmtp, payload, len, pool) == 0 ? SWITCH_TRUE : SWITCH_FALSE;
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
static int amr_debug_lines = 0;
static volatile int amr_log_marks = 0;

#define AMR_LOG_MARK "amr test: log mark"

/* counts the module's concealment WARNINGs */
static switch_status_t amr_watch_log(const switch_log_node_t *node, switch_log_level_t level)
{
	if (node->level <= SWITCH_LOG_WARNING && node->data && strstr(node->data, "undecodable payloads concealed")) amr_warning_lines++;
	if (node->data && (strstr(node->data, "AMR decoder (") || strstr(node->data, "AMR encoder (") || strstr(node->data, "undecodable payload of"))) amr_debug_lines++;
	if (node->data && strstr(node->data, AMR_LOG_MARK)) amr_log_marks++;
	return SWITCH_STATUS_SUCCESS;
}

/* the logger is asynchronous: wait until it delivered everything logged so far; FALSE if it did not */
static switch_bool_t amr_log_sync(void)
{
	int want = amr_log_marks + 1, i;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, AMR_LOG_MARK "\n");
	for (i = 0; i < 1000 && amr_log_marks < want; i++) {
		switch_yield(10000);
	}
	return amr_log_marks >= want ? SWITCH_TRUE : SWITCH_FALSE;
}

static switch_bool_t amr_log_watch_start(void)
{
	switch_bool_t synced;

	/* a test that failed before amr_log_watch_stop left it bound */
	switch_log_unbind_logger(amr_watch_log);
	switch_log_bind_logger(amr_watch_log, SWITCH_LOG_DEBUG, SWITCH_FALSE);
	synced = amr_log_sync();
	amr_warning_lines = 0;
	amr_debug_lines = 0;
	return synced;
}

static switch_bool_t amr_log_watch_stop(void)
{
	switch_bool_t synced = amr_log_sync();

	switch_log_unbind_logger(amr_watch_log);
	return synced;
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

/* any value with side effects (an encode, a negotiation): evaluated once */
#define amr_check_int(expr, expected) do { int v_ = (int) (expr); fst_check_int_equals(v_, (expected)); } while (0)

/* a session with its own AMR read and write codecs */
static switch_core_session_t *amr_cmr_session(switch_codec_t *reader, switch_codec_t *writer, switch_memory_pool_t *pool)
{
	switch_core_session_t *session = NULL;
	switch_call_cause_t cause;

	if (switch_ivr_originate(NULL, &session, &cause, "null/amr-cmr-handoff", 0, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS) {
		return NULL;
	}
	if (amr_init(writer, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, pool) != SWITCH_STATUS_SUCCESS ||
		amr_init(reader, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, pool) != SWITCH_STATUS_SUCCESS) {
		switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
		return NULL;
	}
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

static switch_bool_t amr_conf_changed = SWITCH_FALSE;

/* reload mod_amr with the given settings, or with freeswitch.xml when NULL (the teardown does that
 * after a test that changed it, also when it failed); waits for sessions that still use it */
static switch_status_t amr_reload(const char *settings)
{
	switch_xml_binding_t *binding = NULL;
	const char *err = NULL;
	switch_status_t status;
	int i;

	for (i = 0; i < 1000 && switch_loadable_module_unload_module(SWITCH_GLOBAL_dirs.mod_dir, "mod_amr", SWITCH_FALSE, &err) != SWITCH_STATUS_SUCCESS; i++) {
		switch_yield(10000);
	}
	if (settings) {
		amr_conf_settings = settings;
		switch_xml_bind_search_function_ret(amr_conf_lookup, SWITCH_XML_SECTION_CONFIG, NULL, &binding);
	}
	status = switch_loadable_module_load_module(SWITCH_GLOBAL_dirs.mod_dir, "mod_amr", SWITCH_TRUE, &err);
	if (binding) {
		switch_xml_unbind_search_function(&binding);
	}
	/* not reloaded: what was loaded stays, and may not be the default */
	if (status == SWITCH_STATUS_SUCCESS) {
		amr_conf_changed = settings ? SWITCH_TRUE : SWITCH_FALSE;
	} else {
		amr_conf_changed = SWITCH_TRUE;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "amr test: reloading mod_amr failed: %s\n", switch_str_nil(err));
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

/* an offer with a=ptime */
static const char *amr_offer_ptime(switch_core_session_t *session, int version, int pt, int ptime, const char *fmtp)
{
	return switch_core_session_sprintf(session,
		"v=0\r\n"
		"o=- 1 %d IN IP4 198.51.100.1\r\n"
		"s=-\r\n"
		"t=0 0\r\n"
		"m=audio 56210 RTP/AVP %d\r\n"
		"c=IN IP4 198.51.100.1\r\n"
		"a=rtpmap:%d AMR/8000\r\n"
		"a=fmtp:%d %s\r\n"
		"a=ptime:%d\r\n"
		"a=sendrecv\r\n", version, pt, pt, pt, fmtp, ptime);
}

static switch_status_t amr_session_media(switch_core_session_t *session, const char *strict);

/* the far end's answer on payload type pt (answer_fmtp: its fmtp, NULL none), negotiated as sdp_type (early media
 * comes as SDP_OFFER); the octet-align the read codec runs, -1 if negotiation failed */
static int amr_answer_on(switch_core_session_t *session, int pt, const char *answer_fmtp, switch_sdp_type_t sdp_type)
{
	char answer[512];
	uint8_t proceed = 0;
	switch_codec_t *read_codec;

	switch_snprintf(answer, sizeof(answer),
		"v=0\r\n"
		"o=- 2 1 IN IP4 198.51.100.2\r\n"
		"s=-\r\n"
		"t=0 0\r\n"
		"m=audio 56220 RTP/AVP %d\r\n"
		"c=IN IP4 198.51.100.2\r\n"
		"a=rtpmap:%d AMR/8000\r\n"
		"%s"
		"a=sendrecv\r\n", pt, pt, answer_fmtp ? switch_core_session_sprintf(session, "a=fmtp:%d %s\r\n", pt, answer_fmtp) : "");
	if (switch_core_media_negotiate_sdp(session, answer, &proceed, sdp_type) != 1 ||
		!(read_codec = switch_core_session_get_read_codec(session)) || !switch_core_codec_ready(read_codec) || !read_codec->implementation) {
		return -1;
	}
	return strstr(read_codec->implementation->fmtp, "octet-align=1") ? 1 : 0;
}

/* the payload type of our last SDP whose a=fmtp states want, -1 if none */
static int amr_local_pt(switch_core_session_t *session, const char *want)
{
	const char *sdp, *p;

	if (!(sdp = switch_channel_get_variable(switch_core_session_get_channel(session), "rtp_local_sdp_str"))) {
		return -1;
	}
	for (p = sdp; (p = strstr(p, "a=fmtp:")); p++) {
		if (strstr(p, want) && strstr(p, want) < strstr(p, "\r\n")) {
			return atoi(p + strlen("a=fmtp:"));
		}
	}
	return -1;
}

/* our offer, then the far end's answer on our payload type of that framing (answer_fmtp: its fmtp, NULL none);
 * the octet-align the read codec runs, -1 if negotiation failed */
static int amr_answer_framing(switch_core_session_t *session, switch_bool_t pick_oa, const char *answer_fmtp)
{
	int pt;

	if (amr_session_media(session, NULL) != SWITCH_STATUS_SUCCESS) {
		return -1;
	}
	switch_core_media_gen_local_sdp(session, SDP_OFFER, "127.0.0.1", 12345, NULL, 1);
	if ((pt = amr_local_pt(session, pick_oa ? "octet-align=1" : "octet-align=0")) < 0) {
		return -1;
	}
	return amr_answer_on(session, pt, answer_fmtp, SDP_ANSWER);
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
static const char *amr_codec_negotiation = NULL;

static switch_status_t amr_session_media(switch_core_session_t *session, const char *strict)
{
	switch_media_handle_t *media_handle = NULL;
	switch_core_media_params_t *mparams;

	if (strict) {
		switch_channel_set_variable(switch_core_session_get_channel(session), "telnyx-strict-codec-match", strict);
	}
	if (amr_codec_negotiation) {
		switch_channel_set_variable(switch_core_session_get_channel(session), "rtp_codec_negotiation", amr_codec_negotiation);
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

/* an offer with two AMR payload types; NULL fmtp: none */
static const char *amr_offer_maps(switch_core_session_t *session, const char *fmtp_96, const char *fmtp_97)
{
	return switch_core_session_sprintf(session,
		"v=0\r\n"
		"o=- 1 1 IN IP4 198.51.100.1\r\n"
		"s=-\r\n"
		"t=0 0\r\n"
		"m=audio 56210 RTP/AVP 96 97\r\n"
		"c=IN IP4 198.51.100.1\r\n"
		"a=rtpmap:96 AMR/8000\r\n"
		"%s"
		"a=rtpmap:97 AMR/8000\r\n"
		"%s"
		"a=sendrecv\r\n",
		fmtp_96 ? switch_core_session_sprintf(session, "a=fmtp:96 %s\r\n", fmtp_96) : "",
		fmtp_97 ? switch_core_session_sprintf(session, "a=fmtp:97 %s\r\n", fmtp_97) : "");
}

/* a session of a test that reloads the module first (FST_SESSION sessions exist before the test) */
static switch_core_session_t *amr_new_session(void)
{
	switch_core_session_t *session = NULL;
	switch_call_cause_t cause;

	if (switch_ivr_originate(NULL, &session, &cause, "null/amr-sdp", 0, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS) {
		return NULL;
	}
	return session;
}

static void amr_end_session(switch_core_session_t *session)
{
	if (session) {
		switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
	}
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

static const char *amr_passes_to(switch_codec_t *codec, switch_codec_t *other)
{
	switch_codec_control_type_t rtype = SCCT_NONE;
	void *ret = NULL;

	switch_core_codec_control(codec, SCC_CODEC_SPECIFIC, SCCT_STRING, (void *) "passes_to", SCCT_NONE, other, &rtype, &ret);
	return rtype == SCCT_STRING ? (const char *) ret : NULL;
}

static const char *amr_transcodes_dtx_to(switch_codec_t *codec, switch_codec_t *other)
{
	switch_codec_control_type_t rtype = SCCT_NONE;
	void *ret = NULL;

	switch_core_codec_control(codec, SCC_CODEC_SPECIFIC, SCCT_STRING, (void *) "transcodes_dtx_to", SCCT_NONE, other, &rtype, &ret);
	return rtype == SCCT_STRING ? (const char *) ret : NULL;
}

static switch_io_write_frame_t amr_null_write = NULL;
static unsigned char amr_written[SWITCH_RECOMMENDED_BUFFER_SIZE];
static uint32_t amr_written_len = 0;
/* frames per payload amr_forwarded_ft() writes, and the writes the session made */
static int amr_payload_frames = 1, amr_writes = 0;

static switch_status_t amr_capture_write(switch_core_session_t *session, switch_frame_t *frame, switch_io_flag_t flags, int stream_id)
{
	amr_writes++;
	amr_written_len = frame->datalen <= sizeof(amr_written) ? frame->datalen : 0;
	memcpy(amr_written, frame->data, amr_written_len);
	return SWITCH_STATUS_SUCCESS;
}

/* writes a mode 7 payload of a leg answered mode-set=7 to the session, its write codec on to_fmtp (octet aligned);
 * the FT sent, -1 if nothing was; same: sent unchanged */
static int amr_forwarded_ft(switch_core_session_t *session, switch_memory_pool_t *pool, const char *to_fmtp, switch_bool_t *same)
{
	switch_endpoint_interface_t *endpoint = switch_loadable_module_get_endpoint_interface("null");
	switch_codec_t encoder = { 0 }, from = { 0 }, to = { 0 };
	unsigned char payload[SWITCH_RECOMMENDED_BUFFER_SIZE], frames[12][64];
	switch_frame_t frame = { 0 };
	int16_t pcm[160];
	int ft = -1, f;

	*same = SWITCH_FALSE;
	amr_written_len = 0;
	amr_writes = 0;
	amr_tone(pcm, 0);
	if (endpoint && amr_init(&encoder, "octet-align=1; mode-set=7", SWITCH_CODEC_FLAG_ENCODE, pool) == SWITCH_STATUS_SUCCESS &&
		amr_init(&from, "octet-align=1; mode-set=7", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, pool) == SWITCH_STATUS_SUCCESS &&
		amr_init(&to, to_fmtp, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS &&
		switch_core_session_set_write_codec(session, &to) == SWITCH_STATUS_SUCCESS) {
		frame.codec = &from;
		frame.data = payload;
		if (amr_payload_frames == 1) {
			frame.datalen = amr_encode(&encoder, pcm, payload, sizeof(payload));
		} else {
			for (f = 0; f < amr_payload_frames; f++) {
				amr_encode(&encoder, pcm, frames[f], sizeof(frames[f]));
			}
			frame.datalen = amr_payload(frames, amr_payload_frames, SWITCH_TRUE, payload, sizeof(payload));
		}
		frame.buflen = sizeof(payload);
		frame.samples = 160 * amr_payload_frames;
		frame.rate = 8000;
		frame.channels = 1;
		amr_null_write = endpoint->io_routines->write_frame;
		endpoint->io_routines->write_frame = amr_capture_write;
		switch_core_session_write_frame(session, &frame, SWITCH_IO_FLAG_NONE, 0);
		endpoint->io_routines->write_frame = amr_null_write;
		switch_core_session_set_write_codec(session, NULL);
		if (amr_written_len > 1) {
			ft = (amr_written[1] >> 3) & 0x0f;
			*same = amr_written_len == frame.datalen && !memcmp(amr_written, payload, frame.datalen);
		}
	}
	if (switch_core_codec_ready(&to)) switch_core_codec_destroy(&to);
	if (switch_core_codec_ready(&from)) switch_core_codec_destroy(&from);
	if (switch_core_codec_ready(&encoder)) switch_core_codec_destroy(&encoder);
	if (endpoint) UNPROTECT_INTERFACE(endpoint);
	return ft;
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
			if (amr_conf_changed) {
				fst_check(amr_reload(NULL) == SWITCH_STATUS_SUCCESS);
			}
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

			/* documents the core without strict-codec-match (not RFC 4867 8.3.1): fails once the core changes it */
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
			fst_requires(amr_log_watch_start());
			decoded_len = sizeof(decbuf);
			fst_check(switch_core_codec_decode(&read_codec, NULL, (void *) no_data_be, sizeof(no_data_be), 8000, decbuf, &decoded_len, &rate, &flags) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(decoded_len, 320);
			fst_requires(amr_log_watch_stop());
			fst_check_int_equals(amr_warning_lines, 0);
			switch_core_codec_destroy(&read_codec);

			fst_requires(amr_init(&read_codec, "octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(amr_log_watch_start());
			decoded_len = sizeof(decbuf);
			fst_check(switch_core_codec_decode(&read_codec, NULL, (void *) no_data_oa, sizeof(no_data_oa), 8000, decbuf, &decoded_len, &rate, &flags) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(decoded_len, 320);
			decoded_len = sizeof(decbuf);
			fst_check(switch_core_codec_decode(&read_codec, NULL, (void *) sid_oa, sizeof(sid_oa), 8000, decbuf, &decoded_len, &rate, &flags) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(decoded_len, 320);
			fst_requires(amr_log_watch_stop());
			fst_check_int_equals(amr_warning_lines, 0);
			switch_core_codec_destroy(&read_codec);
		}
		FST_TEST_END()

		/* FT 9-14 are not AMR: the payload is concealed, the call goes on (RFC 4867 4.3.2) */
		FST_TEST_BEGIN(amr_conceals_invalid_frame_types)
		{
			int ft, wrong = 0;

			/* exactly a CMR and the ToC, as for a NO_DATA frame: only the frame type is wrong */
			for (ft = 9; ft <= 14; ft++) {
				unsigned char oa[2] = { 0xf0, 0 }, be[2] = { 0 };
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
				fst_check(amr_decodes_not_as_lost(fmtps[i], frame, frame_len, fst_pool));
				fst_check(amr_decodes_not_as_lost(fmtps[i], frame, frame_len + 3, fst_pool));
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
				if (!(frame_len[f] = amr_encode(&encoder, pcm, frames[f], sizeof(frames[f])))) break;
			}
			fst_requires(f == 10);
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
			fst_requires(amr_log_watch_start());
			for (i = 0; i < 250; i++) {
				out_len = sizeof(out);
				switch_core_codec_decode(&codec, NULL, (void *) reserved_oa, sizeof(reserved_oa), 8000, out, &out_len, &rate, &flag);
			}
			fst_requires(amr_log_watch_stop());
			/* the 1st of 250 concealed payloads */
			fst_check_int_equals(amr_warning_lines, 1);

			fst_requires(amr_log_watch_start());
			out_len = sizeof(out);
			switch_core_codec_decode(&codec, NULL, (void *) reserved_oa, sizeof(reserved_oa), 8000, out, &out_len, &rate, &flag);
			fst_requires(amr_log_watch_stop());
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

			/* reserved CMR: ignored, the previous request stays */
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr9_oa, sizeof(cmr9_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 1);

			/* outside the mode-set: ignored, the previous request stays (RFC 4867 4.3.1) */
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr7_oa, sizeof(cmr7_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 1);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr1_oa, sizeof(cmr1_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 1);

			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr15_oa, sizeof(cmr15_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 2);
			switch_core_codec_destroy(&codec);

			/* non-contiguous mode-set: requests between its modes are ignored */
			fst_requires(amr_init(&codec, "mode-set=0,2,7;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr4_oa, sizeof(cmr4_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 7);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr2_oa, sizeof(cmr2_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 2);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr4_oa, sizeof(cmr4_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 2);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr7_oa, sizeof(cmr7_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
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
			fst_check(amr_init(&other, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			other.session = session;

			out_len = sizeof(out);
			fst_check(switch_core_codec_decode(&other, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			ft = amr_encoded_ft(&writer, SWITCH_TRUE);
			fst_check_int_equals(ft, 2);

			out_len = sizeof(out);
			fst_check(switch_core_codec_decode(&reader, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
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
			fst_check(switch_core_codec_decode(&reader, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			ft = amr_encoded_ft(&writer, SWITCH_TRUE);
			fst_check_int_equals(ft, 0);

			switch_core_session_unset_read_codec(session);
			switch_core_codec_destroy(&reader);
			fst_check(amr_init(&reader2, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_session_set_read_codec(session, &reader2) == SWITCH_STATUS_SUCCESS);
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
			fst_check(switch_core_codec_copy(&reader, &bug, NULL, NULL) == SWITCH_STATUS_SUCCESS);
			fst_check(!bug.session);
			frame.codec = &reader;
			bug.cur_frame = &frame;
			fst_check(switch_core_codec_decode(&bug, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
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
			amr_adjust(&codec, "increase");
			/* unknown: no change */
			amr_adjust(&codec, "bogus");
			amr_check_ft(&codec, SWITCH_TRUE, 2);
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
			/* the answer would return crc=0 now */
			r = amr_changes_framing(&codec, "mode-set=0,1,2;octet-align=1;crc=0");
			fst_check(r && !strcmp(r, "true"));
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* the same answered set, but now the peer signals it: CMRs outside it are ignored from then on */
		FST_TEST_BEGIN(amr_reports_a_newly_signalled_mode_set)
		{
			switch_codec_t codec = { 0 };
			const char *r;

			fst_requires(amr_reload("<param name=\"mode-set\" value=\"0,2,7\"/>") == SWITCH_STATUS_SUCCESS);
			fst_requires(amr_init(&codec, "octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			r = amr_changes_framing(&codec, "octet-align=1");
			fst_check(r && !strcmp(r, "false"));
			r = amr_changes_framing(&codec, "mode-set=0,2,7;octet-align=1");
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
				amr_check_int(amr_encode(&codec, pcm, encoded, sizeof(encoded)), cases[i].len);
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
				matched += impl->matches_fmtp("octet-align=1;crc=0;robust-sorting=0", impl->fmtp) == SWITCH_STATUS_SUCCESS;
				unsupported_matched += impl->matches_fmtp("octet-align=1;crc=1", impl->fmtp) == SWITCH_STATUS_SUCCESS;
				unsupported_matched += impl->matches_fmtp("octet-align=1;robust-sorting=1", impl->fmtp) == SWITCH_STATUS_SUCCESS;
				unsupported_matched += impl->matches_fmtp("octet-align=1;interleaving=4", impl->fmtp) == SWITCH_STATUS_SUCCESS;
				/* interleaving present means interleaving (RFC 4867 8.1) */
				unsupported_matched += impl->matches_fmtp("octet-align=1;interleaving=0", impl->fmtp) == SWITCH_STATUS_SUCCESS;
				unsupported_matched += impl->matches_fmtp("octet-align=1;mode-set=8,9", impl->fmtp) == SWITCH_STATUS_SUCCESS;
				unsupported_matched += impl->matches_fmtp("octet-align=1;channels=2", impl->fmtp) == SWITCH_STATUS_SUCCESS;
			}
			UNPROTECT_INTERFACE(codec_interface);

			/* each accepted fmtp by the octet-aligned implementation */
			fst_check_int_equals(matched, 3);
			fst_check_int_equals(unsupported_matched, 0);
		}
		FST_TEST_END()

		/* crc and robust-sorting offered (as 0) are in the answer (RFC 4867 8.3.1) */
		FST_TEST_BEGIN(amr_answer_returns_offered_options)
		{
			switch_codec_t codec = { 0 };

			fst_requires(amr_init(&codec, "mode-set=2;octet-align=1;crc=0;robust-sorting=0", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, ";crc=0") && strstr(codec.fmtp_out, ";robust-sorting=0") && !strstr(codec.fmtp_out, "interleaving"));
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

		/* force-oa: whenever the far end's fmtp does not state octet-align, offer or answer; a stated
		 * octet-align wins; the implementation follows */
		FST_TEST_BEGIN(amr_force_oa_applies_when_octet_align_is_not_stated)
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
				amr_check_int(amr_encode(&codec, pcm, encoded, sizeof(encoded)), cases[i].len);
				fst_check(codec.implementation && strstr(codec.implementation->fmtp, cases[i].len == 17 ? "octet-align=1" : "octet-align=0"));
				switch_core_codec_destroy(&codec);
			}

			fst_requires(switch_ivr_originate(NULL, &session, &cause, "null/amr-force", 0, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_direction(switch_core_session_get_channel(session)) == SWITCH_CALL_DIRECTION_OUTBOUND);
			/* a codec of a session without our offer's fmtp (the core fills in octet-align for answers to our offer) */
			if (amr_init(&codec, "mode-set=2", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS) {
				switch_codec_t copy = { 0 };

				fst_check(codec.session == session);
				fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "octet-align=1"));
				amr_check_int(amr_encode(&codec, pcm, encoded, sizeof(encoded)), 17);
				/* the media bug copy (no session) runs the same framing */
				if (switch_core_codec_copy(&codec, &copy, NULL, NULL) == SWITCH_STATUS_SUCCESS) {
					fst_check(!copy.session);
					amr_check_int(amr_encode(&copy, pcm, encoded, sizeof(encoded)), 17);
					switch_core_codec_destroy(&copy);
				} else {
					fst_check(!"codec copy");
				}
				switch_core_codec_destroy(&codec);
			} else {
				fst_check(!"codec init");
			}
			switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session);

			/* the core matches the octet-aligned implementation itself */
			{
				switch_codec_interface_t *codec_interface = switch_loadable_module_get_codec_interface("AMR", "mod_amr");
				const switch_codec_implementation_t *impl;
				int oa_matched = 0, be_matched = 0;

				fst_requires(codec_interface);
				for (impl = codec_interface->implementations; impl; impl = impl->next) {
					/* both argument orders: the core calls it both ways */
					if (impl->matches_fmtp("mode-set=2", impl->fmtp) == SWITCH_STATUS_SUCCESS &&
						impl->matches_fmtp(impl->fmtp, "mode-set=2") == SWITCH_STATUS_SUCCESS) {
						oa_matched += strstr(impl->fmtp, "octet-align=1") != NULL;
						be_matched += strstr(impl->fmtp, "octet-align=0") != NULL;
					}
				}
				UNPROTECT_INTERFACE(codec_interface);
				fst_check_int_equals(oa_matched, 1);
				fst_check_int_equals(be_matched, 0);
			}
		}
		FST_TEST_END()

		/* force-be cancels force-oa: an fmtp without octet-align is bandwidth efficient again */
		FST_TEST_BEGIN(amr_force_be_cancels_force_oa)
		{
			switch_codec_interface_t *codec_interface;
			const switch_codec_implementation_t *impl;
			switch_codec_t codec = { 0 };
			int16_t pcm[160] = { 0 };
			unsigned char encoded[SWITCH_RECOMMENDED_BUFFER_SIZE];
			int oa_matched = 0, be_matched = 0;

			fst_requires(amr_reload("<param name=\"force-oa\" value=\"true\"/><param name=\"force-be\" value=\"true\"/>") == SWITCH_STATUS_SUCCESS);
			fst_requires(amr_init(&codec, "mode-set=2", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			amr_check_int(amr_encode(&codec, pcm, encoded, sizeof(encoded)), 16);
			switch_core_codec_destroy(&codec);
			fst_requires((codec_interface = switch_loadable_module_get_codec_interface("AMR", "mod_amr")));
			for (impl = codec_interface->implementations; impl; impl = impl->next) {
				/* both argument orders: the core calls it both ways */
				if (impl->matches_fmtp("mode-set=2", impl->fmtp) == SWITCH_STATUS_SUCCESS && impl->matches_fmtp(impl->fmtp, "mode-set=2") == SWITCH_STATUS_SUCCESS) {
					oa_matched += strstr(impl->fmtp, "octet-align=1") != NULL;
					be_matched += strstr(impl->fmtp, "octet-align=0") != NULL;
				}
			}
			UNPROTECT_INTERFACE(codec_interface);
			fst_check_int_equals(oa_matched, 0);
			fst_check_int_equals(be_matched, 1);
		}
		FST_TEST_END()

		/* force-oa in SDP: an offer without octet-align is answered octet aligned, and the same offer
		 * again keeps the codec (the matched implementation is the running one) */
		FST_TEST_BEGIN(amr_force_oa_answer_and_reoffer)
		{
			switch_core_session_t *session;
			amr_reoffer_result_t result;

			fst_requires(amr_reload("<param name=\"force-oa\" value=\"true\"/>") == SWITCH_STATUS_SUCCESS);
			fst_requires((session = amr_new_session()));
			result = amr_reoffer(session, "true", "mode-set=2", amr_offer(session, 2, "mode-set=2"), "octet-align=1; mode-set=2");
			fst_check(result.negotiated);
			fst_check(strstr(result.answer, "octet-align=1") != NULL);
			fst_check(!result.codec_reset);
			fst_check(!result.framing_stale);
			fst_check(result.decoded);
			amr_end_session(session);
		}
		FST_TEST_END()

		/* invite-prefer-oa/-be: of several AMR payload types the preferred framing is answered,
		 * also over one without an fmtp */
		FST_TEST_BEGIN(amr_invite_prefer_picks_the_framing)
		{
			const struct { const char *param; const char *negotiation; const char *fmtp_96; const char *fmtp_97; const char *answer; const char *framing; } cases[] = {
				{ "invite-prefer-oa", NULL, "mode-set=2", "octet-align=1; mode-set=2", "97 ", "octet-align=1" },
				{ "invite-prefer-oa", NULL, NULL, "octet-align=1; mode-set=2", "97 ", "octet-align=1" },
				{ "invite-prefer-be", NULL, "octet-align=1; mode-set=2", "mode-set=2", "97 ", "octet-align=0" },
				{ "invite-prefer-be", NULL, "octet-align=1; mode-set=2", NULL, "97 ", "octet-align=0" },
				/* the preferred one first: the codec says which framing it is (spaces around "=") */
				{ "invite-prefer-oa", NULL, "octet-align = 1; mode-set=2", "mode-set=2", "96 ", "octet-align=1" },
				{ "invite-prefer-be", NULL, "mode-set=2", "octet-align=1; mode-set=2", "96 ", "octet-align=0" },
				/* greedy (our order: bandwidth efficient first): a later other one is skipped once one matched */
				{ "invite-prefer-oa", "greedy", "octet-align=1; mode-set=2", "mode-set=2", "96 ", "octet-align=1" },
				/* none of the preferred framing: nothing is skipped, the first one is taken */
				{ "invite-prefer-oa", NULL, "mode-set=2", "mode-set=7", "96 ", "octet-align=0" },
				/* the preferred one is not usable (crc): the other one is not skipped */
				{ "invite-prefer-oa", NULL, "octet-align=0; mode-set=2", "octet-align=1; crc=1; mode-set=2", "96 ", "octet-align=0" }
			};
			int i;

			for (i = 0; i < (int) (sizeof(cases) / sizeof(cases[0])); i++) {
				switch_core_session_t *session;
				char settings[128], answer[256];

				switch_snprintf(settings, sizeof(settings), "<param name=\"%s\" value=\"true\"/>", cases[i].param);
				fst_requires(amr_reload(settings) == SWITCH_STATUS_SUCCESS);
				fst_requires((session = amr_new_session()));
				amr_codec_negotiation = cases[i].negotiation;
				fst_check(amr_answer_to(session, amr_offer_maps(session, cases[i].fmtp_96, cases[i].fmtp_97), answer, sizeof(answer)));
				amr_codec_negotiation = NULL;
				fst_check(!strncmp(answer, cases[i].answer, strlen(cases[i].answer)));
				fst_check(strstr(answer, cases[i].framing) != NULL);
				amr_end_session(session);
			}
		}
		FST_TEST_END()

		/* invite-prefer with three AMR payload types: once one matched, later ones of the other framing are skipped;
		 * none of the preferred framing: the first one */
		FST_TEST_BEGIN(amr_invite_prefer_with_three_payload_types)
		{
			const struct { const char *negotiation; const char *f96, *f97, *f98; const char *answer; } cases[] = {
				{ "greedy", "octet-align=1; mode-set=2", "mode-set=2", "mode-set=2", "96 " },
				{ NULL, "mode-set=2", "mode-set=2", "mode-set=2", "96 " }
			};
			int i;

			fst_requires(amr_reload("<param name=\"invite-prefer-oa\" value=\"true\"/>") == SWITCH_STATUS_SUCCESS);
			for (i = 0; i < 2; i++) {
				switch_core_session_t *session;
				char answer[256];
				const char *offer;

				fst_requires((session = amr_new_session()));
				offer = switch_core_session_sprintf(session,
					"v=0\r\no=- 1 1 IN IP4 198.51.100.1\r\ns=-\r\nt=0 0\r\nm=audio 56210 RTP/AVP 96 97 98\r\nc=IN IP4 198.51.100.1\r\n"
					"a=rtpmap:96 AMR/8000\r\na=fmtp:96 %s\r\na=rtpmap:97 AMR/8000\r\na=fmtp:97 %s\r\na=rtpmap:98 AMR/8000\r\na=fmtp:98 %s\r\n"
					"a=sendrecv\r\n", cases[i].f96, cases[i].f97, cases[i].f98);
				amr_codec_negotiation = cases[i].negotiation;
				fst_check(amr_answer_to(session, offer, answer, sizeof(answer)));
				amr_codec_negotiation = NULL;
				fst_check(!strncmp(answer, cases[i].answer, strlen(cases[i].answer)));
				amr_end_session(session);
			}
		}
		FST_TEST_END()

		/* invite-prefer counts the preferred framing as the match sees it: an answer on our octet-aligned payload type
		 * that leaves out octet-align is octet aligned (as offered); a payload type at another clock rate does not
		 * match, so it does not skip a usable one of the other framing */
		FST_TEST_BEGIN(amr_invite_prefer_counts_what_the_match_sees)
		{
			switch_core_session_t *session;
			char answer[256];
			const char *fmtps[] = { "mode-set=7", NULL };
			int i;

			fst_requires(amr_reload("<param name=\"invite-prefer-be\" value=\"true\"/>") == SWITCH_STATUS_SUCCESS);
			for (i = 0; i < 2; i++) {
				fst_requires((session = amr_new_session()));
				amr_check_int(amr_answer_framing(session, SWITCH_TRUE, fmtps[i]), 1);
				amr_end_session(session);
			}

			fst_requires(amr_reload("<param name=\"invite-prefer-oa\" value=\"true\"/>") == SWITCH_STATUS_SUCCESS);
			fst_requires((session = amr_new_session()));
			fst_check(amr_answer_to(session, switch_core_session_sprintf(session,
				"v=0\r\no=- 1 1 IN IP4 198.51.100.1\r\ns=-\r\nt=0 0\r\nm=audio 56210 RTP/AVP 96 97\r\nc=IN IP4 198.51.100.1\r\n"
				"a=rtpmap:96 AMR/16000\r\na=fmtp:96 octet-align=1; mode-set=2\r\na=rtpmap:97 AMR/8000\r\na=fmtp:97 mode-set=2\r\n"
				"a=sendrecv\r\n"), answer, sizeof(answer)));
			fst_check(!strncmp(answer, "97 ", 3));
			fst_check(strstr(answer, "octet-align=0") != NULL);
			amr_end_session(session);
		}
		FST_TEST_END()

		/* without invite-prefer the first matching payload type is answered */
		FST_SESSION_BEGIN(amr_offer_order_decides_without_invite_prefer)
		{
			char answer[256];

			fst_requires(amr_answer_to(fst_session, amr_offer_maps(fst_session, "mode-set=2", "octet-align=1; mode-set=2"), answer, sizeof(answer)));
			fst_check(!strncmp(answer, "96 ", 3));
			fst_check(strstr(answer, "octet-align=0") != NULL);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(amr_offer_order_decides_without_invite_prefer_reversed)
		{
			char answer[256];

			fst_requires(amr_answer_to(fst_session, amr_offer_maps(fst_session, "octet-align=1; mode-set=2", "mode-set=2"), answer, sizeof(answer)));
			fst_check(!strncmp(answer, "96 ", 3));
			fst_check(strstr(answer, "octet-align=1") != NULL);
		}
		FST_SESSION_END()

		/* a dynamic payload type below 96 (RFC 3551 does not limit them to 96-127), as for AMR-WB */
		FST_SESSION_BEGIN(amr_offer_on_a_low_dynamic_payload_type)
		{
			char answer[256];

			fst_requires(amr_answer_to(fst_session, amr_offer_pt(fst_session, 1, 63, "octet-align=1; mode-set=2"), answer, sizeof(answer)));
			fst_check(!strncmp(answer, "63 ", 3));
			fst_check(strstr(answer, "octet-align=1") != NULL);
		}
		FST_SESSION_END()

		/* an offer with ptime 40 only near-matches (20 ms implementations): the implementation of the offered
		 * framing is kept, so the same offer again keeps the codec */
		FST_SESSION_BEGIN(amr_near_match_keeps_the_framing)
		{
			switch_codec_t *read_codec;
			void *context;
			uint8_t p = 0;
			char answer[256];

			fst_requires(amr_session_media(fst_session, "true") == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_media_negotiate_sdp(fst_session, amr_offer_ptime(fst_session, 1, 96, 40, "octet-align=1; mode-set=2"), &p, SDP_OFFER) == 1);
			fst_requires((read_codec = switch_core_session_get_read_codec(fst_session)) && switch_core_codec_ready(read_codec));
			fst_check(strstr(read_codec->implementation->fmtp, "octet-align=1") != NULL);
			context = read_codec->private_info;
			fst_requires(switch_core_media_negotiate_sdp(fst_session, amr_offer_ptime(fst_session, 2, 96, 40, "octet-align=1; mode-set=2"), &p, SDP_OFFER) == 1);
			fst_requires((read_codec = switch_core_session_get_read_codec(fst_session)) && switch_core_codec_ready(read_codec));
			fst_check(read_codec->private_info == context);
			amr_answer_fmtp(fst_session, answer, sizeof(answer));
			fst_check(strstr(answer, "octet-align=1") != NULL);
		}
		FST_SESSION_END()

		/* an answer to our offer that leaves out octet-align runs the framing we offered on its payload type */
		FST_SESSION_BEGIN(amr_answer_without_octet_align_runs_the_offered_framing)
		{
			amr_check_int(amr_answer_framing(fst_session, SWITCH_TRUE, "mode-set=7"), 1);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(amr_answer_without_fmtp_runs_the_offered_framing)
		{
			amr_check_int(amr_answer_framing(fst_session, SWITCH_TRUE, NULL), 1);
		}
		FST_SESSION_END()

		/* an answer that states octet-align is followed, also against what we offered on its payload type */
		FST_SESSION_BEGIN(amr_answer_stating_octet_align_is_followed)
		{
			amr_check_int(amr_answer_framing(fst_session, SWITCH_TRUE, "octet-align=0; mode-set=7"), 0);
		}
		FST_SESSION_END()

		/* an answer on a payload type we did not offer: nothing to fill in, the RFC 4867 default */
		FST_SESSION_BEGIN(amr_answer_on_a_payload_type_we_did_not_offer)
		{
			fst_requires(amr_session_media(fst_session, NULL) == SWITCH_STATUS_SUCCESS);
			switch_core_media_gen_local_sdp(fst_session, SDP_OFFER, "127.0.0.1", 12345, NULL, 1);
			fst_requires(amr_local_pt(fst_session, "octet-align=1") != 120 && amr_local_pt(fst_session, "octet-align=0") != 120);
			amr_check_int(amr_answer_on(fst_session, 120, "mode-set=7", SDP_ANSWER), 0);
		}
		FST_SESSION_END()

		/* early media: a 18x answer to our INVITE is negotiated as SDP_OFFER; before the leg is answered it gets
		 * the octet-align we offered too */
		FST_SESSION_BEGIN(amr_early_media_answer_runs_the_offered_framing)
		{
			int pt;

			fst_requires(switch_channel_direction(switch_core_session_get_channel(fst_session)) == SWITCH_CALL_DIRECTION_OUTBOUND);
			/* the fst session is answered already: back to the early phase */
			switch_channel_clear_flag(switch_core_session_get_channel(fst_session), CF_ANSWERED);
			fst_requires(amr_session_media(fst_session, NULL) == SWITCH_STATUS_SUCCESS);
			switch_core_media_gen_local_sdp(fst_session, SDP_OFFER, "127.0.0.1", 12345, NULL, 1);
			fst_requires((pt = amr_local_pt(fst_session, "octet-align=1")) > 0);
			amr_check_int(amr_answer_on(fst_session, pt, "mode-set=7", SDP_OFFER), 1);
		}
		FST_SESSION_END()

		/* a re-INVITE we send on an established call (one payload type, the negotiated one): an answer without
		 * octet-align keeps the framing we offered there, not the one of the implementation registered on that
		 * payload type number */
		FST_SESSION_BEGIN(amr_answer_to_our_reinvite_keeps_the_framing)
		{
			uint8_t p = 0;
			switch_codec_t *read_codec;

			/* strict codec matching: a wrong framing in the answer re-creates the codec (else it is kept as is) */
			fst_requires(amr_session_media(fst_session, "true") == SWITCH_STATUS_SUCCESS);
			/* the far end offered octet aligned on payload type 97 (our bandwidth-efficient implementation's
			 * registered number), we answered */
			fst_requires(switch_core_media_negotiate_sdp(fst_session, amr_offer_pt(fst_session, 1, 97, "octet-align=1; mode-set=7"), &p, SDP_OFFER) == 1);
			switch_core_media_gen_local_sdp(fst_session, SDP_ANSWER, "127.0.0.1", 12345, NULL, 1);
			switch_channel_mark_answered(switch_core_session_get_channel(fst_session));
			fst_requires((read_codec = switch_core_session_get_read_codec(fst_session)) && strstr(read_codec->implementation->fmtp, "octet-align=1"));
			/* our re-INVITE, and its answer without octet-align */
			switch_core_media_gen_local_sdp(fst_session, SDP_OFFER, "127.0.0.1", 12345, NULL, 1);
			fst_requires(amr_local_pt(fst_session, "octet-align=1") == 97);
			amr_check_int(amr_answer_on(fst_session, 97, "mode-set=7", SDP_ANSWER), 1);
		}
		FST_SESSION_END()

		/* force-oa is for offers: a compliant answer on our bandwidth-efficient payload type stays bandwidth efficient */
		FST_TEST_BEGIN(amr_force_oa_keeps_the_answer_framing)
		{
			switch_core_session_t *session;

			fst_requires(amr_reload("<param name=\"force-oa\" value=\"true\"/>") == SWITCH_STATUS_SUCCESS);
			fst_requires((session = amr_new_session()));
			amr_check_int(amr_answer_framing(session, SWITCH_FALSE, "mode-set=7"), 0);
			amr_end_session(session);
			fst_requires((session = amr_new_session()));
			amr_check_int(amr_answer_framing(session, SWITCH_TRUE, "mode-set=7"), 1);
			amr_end_session(session);
		}
		FST_TEST_END()

		/* our SDP offer carries both payload formats */
		FST_SESSION_BEGIN(amr_sdp_offer_has_both_payload_types)
		{
			const char *sdp, *p;
			int amr_maps = 0;

			fst_requires(amr_session_media(fst_session, NULL) == SWITCH_STATUS_SUCCESS);
			switch_core_media_gen_local_sdp(fst_session, SDP_OFFER, "127.0.0.1", 12345, NULL, 1);
			fst_requires((sdp = switch_channel_get_variable(switch_core_session_get_channel(fst_session), "rtp_local_sdp_str")));
			for (p = sdp; (p = strstr(p, " AMR/8000")); p++) amr_maps++;
			fst_check_int_equals(amr_maps, 2);
			fst_check(strstr(sdp, "octet-align=1") != NULL);
			fst_check(strstr(sdp, "octet-align=0") != NULL);
		}
		FST_SESSION_END()

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
			{
				switch_codec_t codec = { 0 };

				fst_requires(amr_init(&codec, "octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_check(switch_test_flag(&codec, SWITCH_CODEC_FLAG_HAS_ADJ_BITRATE));
				switch_core_codec_destroy(&codec);
			}
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
		/* mode-set-overwrite: the offer has 0,1,2, we answer 0,2,4,7 and encode within 0,2. A request
		 * the peer may send (its own mode-set) is followed within ours, others are ignored */
		FST_TEST_BEGIN(amr_overwrite_follows_cmr_of_the_offered_mode_set)
		{
			switch_codec_t codec = { 0 };
			static const unsigned char cmr1_oa[] = { 0x10, 0x7c };
			static const unsigned char cmr2_oa[] = { 0x20, 0x7c };
			static const unsigned char cmr7_oa[] = { 0x70, 0x7c };
			static const unsigned char cmr15_oa[] = { 0xf0, 0x7c };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len, rate = 8000;
			unsigned int flag = 0;

			fst_requires(amr_reload("<param name=\"mode-set\" value=\"0,2,4,7\"/><param name=\"mode-set-overwrite\" value=\"1\"/>"
									"<param name=\"mode-set-overwrite-with-default-bitrate\" value=\"0\"/>") == SWITCH_STATUS_SUCCESS);

			fst_requires(amr_init(&codec, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(codec.fmtp_out && strstr(codec.fmtp_out, "mode-set=0,2,4,7;"));
			amr_check_ft(&codec, SWITCH_TRUE, 2);

			/* in the offered mode-set, not encoded: the allowed mode below it */
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr1_oa, sizeof(cmr1_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 0);

			/* in our answer only, outside the offered mode-set: ignored */
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr7_oa, sizeof(cmr7_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 0);

			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr2_oa, sizeof(cmr2_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 2);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr1_oa, sizeof(cmr1_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 0);
			out_len = sizeof(out);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr15_oa, sizeof(cmr15_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 2);
			switch_core_codec_destroy(&codec);

			fst_requires(amr_reload(NULL) == SWITCH_STATUS_SUCCESS);
		}
		FST_TEST_END()

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
			if (amr_init(&codec, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS) {
				fst_check(switch_channel_var_true(switch_core_session_get_channel(session), "suppress_cng"));
				switch_core_codec_destroy(&codec);
			} else {
				fst_check(!"codec init");
			}
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
			/* the request stays until the next one (RFC 4867 4.3.1) */
			amr_check_ft(&codec, SWITCH_TRUE, 0);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* a reset (e.g. when the bridged leg stops transcoding) keeps the encoder's mode stepping and
		 * an adjust-bitrate mode */
		FST_TEST_BEGIN(amr_reset_keeps_the_mode_state)
		{
			switch_codec_t codec = { 0 };
			static const unsigned char cmr0_oa[] = { 0x00, 0x7c };
			unsigned char out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t out_len = sizeof(out), rate = 8000;
			unsigned int flag = 0;

			/* neighbor, every 2nd frame: 7 7 | CMR 0 | 6 6 5 | reset | 5 4: the step after the reset comes at the
			 * 2nd frame, as the period counts on (with the count lost it would come at once: 4 4) */
			fst_requires(amr_init(&codec, "mode-set=0,1,2,3,4,5,6,7;mode-change-neighbor=1;mode-change-period=2;octet-align=1",
								  SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 7);
			amr_check_ft(&codec, SWITCH_TRUE, 7);
			fst_requires(switch_core_codec_decode(&codec, NULL, (void *) cmr0_oa, sizeof(cmr0_oa), 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			amr_check_ft(&codec, SWITCH_TRUE, 6);
			amr_check_ft(&codec, SWITCH_TRUE, 6);
			amr_check_ft(&codec, SWITCH_TRUE, 5);
			switch_core_codec_reset(&codec);
			amr_check_ft(&codec, SWITCH_TRUE, 5);
			amr_check_ft(&codec, SWITCH_TRUE, 4);
			switch_core_codec_destroy(&codec);

			fst_requires(amr_init(&codec, "mode-set=0,2,5;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			amr_adjust(&codec, "decrease");
			amr_check_ft(&codec, SWITCH_TRUE, 2);
			switch_core_codec_reset(&codec);
			amr_check_ft(&codec, SWITCH_TRUE, 2);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* SFF_PLC: the payload that comes with a jitter buffer loss is not used (here a valid frame); every loss is
		 * decoded as a lost frame, which fades out; also as the first packet */
		FST_TEST_BEGIN(amr_jitter_buffer_losses_fade_out)
		{
			switch_codec_t encoder = { 0 }, codec = { 0 }, reference = { 0 }, fresh = { 0 };
			static const unsigned char lost_oa[] = { 0xf0, 0x7c };
			unsigned char frames[10][64], out[SWITCH_RECOMMENDED_BUFFER_SIZE], ref[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t frame_len[10], out_len, ref_len, rate = 8000;
			unsigned int flag = 0, plc;
			int16_t pcm[160];
			double energy[4] = { 0 };
			int f, i, same = 0, ok = 1;

			fst_requires(amr_init(&encoder, "mode-set=7;octet-align=1", SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(amr_init(&codec, "mode-set=7;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(amr_init(&reference, "mode-set=7;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			for (f = 0; f < 10 && ok; f++) {
				amr_tone(pcm, f);
				ok = (frame_len[f] = amr_encode(&encoder, pcm, frames[f], sizeof(frames[f]))) != 0;
				out_len = sizeof(out);
				ok = ok && switch_core_codec_decode(&codec, NULL, frames[f], frame_len[f], 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS;
				ref_len = sizeof(ref);
				ok = ok && switch_core_codec_decode(&reference, NULL, frames[f], frame_len[f], 8000, ref, &ref_len, &rate, &flag) == SWITCH_STATUS_SUCCESS;
			}
			fst_check(ok);

			for (i = 0; i < 4 && ok; i++) {
				out_len = sizeof(out);
				plc = SFF_PLC;
				ok = switch_core_codec_decode(&codec, NULL, frames[0], frame_len[0], 8000, out, &out_len, &rate, &plc) == SWITCH_STATUS_SUCCESS;
				ref_len = sizeof(ref);
				ok = ok && switch_core_codec_decode(&reference, NULL, (void *) lost_oa, sizeof(lost_oa), 8000, ref, &ref_len, &rate, &flag) == SWITCH_STATUS_SUCCESS;
				same += out_len == 320 && ref_len == 320 && !memcmp(out, ref, 320);
				for (f = 0; f < 160; f++) energy[i] += (double) ((int16_t *) out)[f] * ((int16_t *) out)[f];
			}
			fst_check_int_equals(same, 4);
			fst_check(energy[3] < energy[0] && energy[0] > 0);

			/* the first packet of a call */
			fst_requires(amr_init(&fresh, "mode-set=7;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			out_len = sizeof(out);
			plc = SFF_PLC;
			fst_check(switch_core_codec_decode(&fresh, NULL, frames[0], frame_len[0], 8000, out, &out_len, &rate, &plc) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(out_len, 320);

			switch_core_codec_destroy(&fresh);
			switch_core_codec_destroy(&reference);
			switch_core_codec_destroy(&codec);
			switch_core_codec_destroy(&encoder);
		}
		FST_TEST_END()

		/* a payload concealed for want of output room: its CMR is ignored too; no payload: a lost frame, no WARNING */
		FST_TEST_BEGIN(amr_conceals_without_side_effects)
		{
			switch_codec_t encoder = { 0 }, codec = { 0 };
			unsigned char frames[3][64], payload[512], out[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t len, out_len, rate = 8000;
			unsigned int flag = 0;
			int16_t pcm[160];
			int f;

			fst_requires(amr_init(&encoder, "mode-set=2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			for (f = 0; f < 3; f++) {
				amr_tone(pcm, f);
				fst_requires(amr_encode(&encoder, pcm, frames[f], sizeof(frames[f])));
			}
			switch_core_codec_destroy(&encoder);
			len = amr_payload(frames, 3, SWITCH_TRUE, payload, sizeof(payload));
			payload[0] = (unsigned char) (payload[0] & 0x0f); /* CMR 0 */

			fst_requires(amr_init(&codec, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			out_len = 320;
			fst_requires(switch_core_codec_decode(&codec, NULL, payload, len, 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(out_len, 320);
			amr_check_ft(&codec, SWITCH_TRUE, 2);

			switch_core_codec_destroy(&codec);

			/* no payload, on a fresh codec (a concealment would log its 1st WARNING): a lost frame, no WARNING */
			fst_requires(amr_init(&codec, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(amr_log_watch_start());
			out_len = sizeof(out);
			fst_check(switch_core_codec_decode(&codec, NULL, payload, 0, 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
			fst_check_int_equals(out_len, 320);
			fst_requires(amr_log_watch_stop());
			fst_check_int_equals(amr_warning_lines, 0);
			switch_core_codec_destroy(&codec);
			/* an empty payload decodes as a lost frame */
			fst_check(amr_decodes_as_lost("mode-set=2;octet-align=1", payload, 0, fst_pool));

			/* the concealed payload does not move the timestamp bound: 1 frame at 0, the 3 frames concealed at 10000
			 * (no room), the 3 frames at 160 all decoded (overlap 0, not 10320) */
			{
				switch_frame_t frame = { 0 };
				unsigned char one[64];
				uint32_t one_len = amr_payload(frames, 1, SWITCH_TRUE, one, sizeof(one));

				fst_requires(amr_init(&codec, "mode-set=0,1,2;octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				codec.cur_frame = &frame;
				frame.timestamp = 0;
				out_len = sizeof(out);
				fst_check(switch_core_codec_decode(&codec, NULL, one, one_len, 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				frame.timestamp = 10000;
				out_len = 320;
				fst_check(switch_core_codec_decode(&codec, NULL, payload, len, 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				frame.timestamp = 160;
				out_len = sizeof(out);
				fst_check(switch_core_codec_decode(&codec, NULL, payload, len, 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				fst_check_int_equals(out_len, 3 * 320);
				codec.cur_frame = NULL;
				switch_core_codec_destroy(&codec);
			}
		}
		FST_TEST_END()

		/* amr_debug on: payloads are logged, decoding and encoding are unchanged */
		FST_TEST_BEGIN(amr_debug_logging)
		{
			switch_stream_handle_t stream = { 0 };
			static const unsigned char junk[] = { 0xf0, 0x54, 0x01 };
			int oa, f, same = 0;

			for (oa = 0; oa < 2; oa++) {
				const char *fmtp = oa ? "mode-set=7;octet-align=1" : "mode-set=7;octet-align=0";
				switch_codec_t enc_plain = { 0 }, enc_logged = { 0 }, plain = { 0 }, logged = { 0 };
				unsigned char frame_plain[64], frame_logged[64], out_plain[SWITCH_RECOMMENDED_BUFFER_SIZE], out_logged[SWITCH_RECOMMENDED_BUFFER_SIZE];
				uint32_t len_plain, len_logged, flen_plain, flen_logged, rate = 8000;
				unsigned int flag = 0;
				int16_t pcm[160];

				fst_requires(amr_init(&enc_plain, fmtp, SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_requires(amr_init(&enc_logged, fmtp, SWITCH_CODEC_FLAG_ENCODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_requires(amr_init(&plain, fmtp, SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_requires(amr_init(&logged, fmtp, SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				for (f = 0; f < 5; f++) {
					amr_tone(pcm, f);
					flen_plain = amr_encode(&enc_plain, pcm, frame_plain, sizeof(frame_plain));
					len_plain = sizeof(out_plain);
					switch_core_codec_decode(&plain, NULL, frame_plain, flen_plain, 8000, out_plain, &len_plain, &rate, &flag);

					fst_requires(amr_log_watch_start());
					SWITCH_STANDARD_STREAM(stream);
					switch_api_execute("amr_debug", "on", NULL, &stream);
					switch_safe_free(stream.data);
					flen_logged = amr_encode(&enc_logged, pcm, frame_logged, sizeof(frame_logged));
					len_logged = sizeof(out_logged);
					switch_core_codec_decode(&logged, NULL, frame_logged, flen_logged, 8000, out_logged, &len_logged, &rate, &flag);
					SWITCH_STANDARD_STREAM(stream);
					switch_api_execute("amr_debug", "off", NULL, &stream);
					switch_safe_free(stream.data);
					fst_requires(amr_log_watch_stop());
					/* the encoder's and the decoder's lines at least */
					fst_check(amr_debug_lines >= 2);

					same += flen_plain && flen_plain == flen_logged && !memcmp(frame_plain, frame_logged, flen_plain) &&
						len_plain == 320 && len_logged == 320 && !memcmp(out_plain, out_logged, 320);

					/* an undecodable payload in between, on both, logged */
					len_plain = sizeof(out_plain);
					switch_core_codec_decode(&plain, NULL, (void *) junk, sizeof(junk), 8000, out_plain, &len_plain, &rate, &flag);
					SWITCH_STANDARD_STREAM(stream);
					switch_api_execute("amr_debug", "on", NULL, &stream);
					switch_safe_free(stream.data);
					len_logged = sizeof(out_logged);
					switch_core_codec_decode(&logged, NULL, (void *) junk, sizeof(junk), 8000, out_logged, &len_logged, &rate, &flag);
					SWITCH_STANDARD_STREAM(stream);
					switch_api_execute("amr_debug", "off", NULL, &stream);
					switch_safe_free(stream.data);
				}
				switch_core_codec_destroy(&logged);
				switch_core_codec_destroy(&plain);
				switch_core_codec_destroy(&enc_logged);
				switch_core_codec_destroy(&enc_plain);
			}
			fst_check_int_equals(same, 10);
		}
		FST_TEST_END()

		/* known answer: the octet-aligned payload is CMR 15 and opencore's own frame, byte for byte */
		FST_TEST_BEGIN(amr_octet_aligned_payload_is_the_opencore_frame)
		{
			int m, f, wrong = 0;

			for (m = 0; m < 8; m++) {
				char fmtp[64];
				switch_codec_t codec = { 0 };
				void *state;
				uint32_t seed = 777 + m;

				switch_snprintf(fmtp, sizeof(fmtp), "mode-set=%d;octet-align=1", m);
				if (amr_init(&codec, fmtp, SWITCH_CODEC_FLAG_ENCODE, fst_pool) != SWITCH_STATUS_SUCCESS) {
					wrong++;
					continue;
				}
				if (!(state = Encoder_Interface_init(0))) {
					switch_core_codec_destroy(&codec);
					wrong++;
					continue;
				}
				for (f = 0; f < 50; f++) {
					int16_t pcm[160], copy[160];
					unsigned char payload[SWITCH_RECOMMENDED_BUFFER_SIZE], storage[64];
					uint32_t len;
					int n;

					amr_noise(pcm, &seed);
					memcpy(copy, pcm, sizeof(pcm));
					len = amr_encode(&codec, pcm, payload, sizeof(payload));
					n = Encoder_Interface_Encode(state, (enum Mode) m, copy, storage, 0);
					wrong += n <= 0 || len != (uint32_t) n + 1 || payload[0] != 0xf0 || memcmp(payload + 1, storage, n);
				}
				switch_core_codec_destroy(&codec);
				Encoder_Interface_exit(state);
			}
			fst_check_int_equals(wrong, 0);
		}
		FST_TEST_END()

		/* payloads pass to a leg of the same implementation only within its mode-set */
		FST_TEST_BEGIN(amr_passes_only_within_the_other_legs_mode_set)
		{
			switch_codec_t from = { 0 }, to = { 0 };
			const char *to_fmtps[] = { "octet-align=1; mode-set=2", "octet-align=1; mode-set=2,7", "octet-align=1" };
			const char *expected[] = { "false", "true", "true" };
			int i;

			fst_requires(amr_init(&from, "octet-align=1; mode-set=7", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_test_flag(&from, SWITCH_CODEC_FLAG_CHECK_PASSTHROUGH));
			for (i = 0; i < 3; i++) {
				fst_requires(amr_init(&to, to_fmtps[i], SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_check_string_equals(amr_passes_to(&from, &to), expected[i]);
				switch_core_codec_destroy(&to);
			}
			switch_core_codec_destroy(&from);
		}
		FST_TEST_END()

		FST_SESSION_BEGIN(amr_passthrough_is_transcoded_to_the_other_legs_mode_set)
		{
			switch_bool_t same;

			amr_check_int(amr_forwarded_ft(fst_session, fst_pool, "octet-align=1; mode-set=2", &same), 2);
			fst_check(!same);
			amr_check_int(amr_forwarded_ft(fst_session, fst_pool, "octet-align=1; mode-set=2,7", &same), 7);
			fst_check(same);
			amr_check_int(amr_forwarded_ft(fst_session, fst_pool, "octet-align=1", &same), 7);
			fst_check(same);

			/* every frame of a refused payload is re-encoded, not only the first */
			amr_payload_frames = 3;
			amr_check_int(amr_forwarded_ft(fst_session, fst_pool, "octet-align=1; mode-set=2", &same), 2);
			amr_check_int(amr_writes, 3);
			amr_payload_frames = 1;
		}
		FST_SESSION_END()

		/* no SID frames to a leg that takes none from a leg that may send them */
		FST_SESSION_BEGIN(amr_passes_no_sid_to_a_leg_without_silence_suppression)
		{
			switch_codec_t from = { 0 }, to = { 0 };
			int i;

			fst_requires(amr_init(&from, "octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(amr_init(&to, "octet-align=1", SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
								  switch_core_session_get_pool(fst_session)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(amr_passes_to(&from, &to), "true");
			fst_check_string_equals(amr_transcodes_dtx_to(&from, &to), "false");
			switch_channel_set_variable(switch_core_session_get_channel(fst_session), "suppress_cng", "true");
			/* read again within 50 calls, about a second of frames */
			for (i = 0; i < 50 && !strcmp(switch_str_nil(amr_passes_to(&from, &to)), "true"); i++);
			fst_check(i < 50);
			fst_check_string_equals(amr_passes_to(&from, &to), "false");
			fst_check_string_equals(amr_transcodes_dtx_to(&from, &to), "false");
			fst_check_string_equals(amr_passes_to(&to, &from), "true");
			switch_channel_set_variable(switch_core_session_get_channel(fst_session), "suppress_cng", NULL);
			switch_core_codec_destroy(&to);
			switch_core_codec_destroy(&from);
		}
		FST_SESSION_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
#endif
