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
 * switch_vbr_ptime_rtp.c -- VBR ptime autofix on the media read path, with crafted RTP into a jitter-buffered leg
 *
 */
#include <switch.h>
#include <test/switch_test.h>

#define VBR_TX_PORT 42090

typedef struct {
	switch_core_session_t *session;
	switch_channel_t *channel;
	switch_socket_t *sock;
	switch_sockaddr_t *to;
	uint32_t clock;
	uint32_t ts;
	uint16_t seq;
	uint32_t ssrc;
	int sent;
	int last_read_seq;
	int plc_frames;
	int alive;
} vbr_call_t;

static const char *vbr_sdp(switch_core_session_t *session, const char *media)
{
	return switch_core_session_sprintf(session,
									   "v=0\r\n"
									   "o=test 1 1 IN IP4 127.0.0.1\r\n"
									   "s=-\r\n"
									   "c=IN IP4 127.0.0.1\r\n"
									   "t=0 0\r\n"
									   "%s",
									   media);
}

static switch_status_t vbr_setup(vbr_call_t *c, const char *codecs, const char *media, uint32_t clock, const char *strict)
{
	switch_call_cause_t cause;
	switch_media_handle_t *media_handle = NULL;
	switch_core_media_params_t *mparams;
	switch_memory_pool_t *pool;
	switch_rtp_t *rtp;
	switch_sockaddr_t *local = NULL;
	const char *port, *err = NULL;
	uint8_t p = 0;

	memset(c, 0, sizeof(*c));
	c->clock = clock;
	c->ts = 160000;
	c->seq = 1000;
	c->ssrc = 0x5ca1ab1e;

	if (switch_ivr_originate(NULL, &c->session, &cause, "null/+15553330583", 2, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS || !c->session) {
		return SWITCH_STATUS_FALSE;
	}

	c->channel = switch_core_session_get_channel(c->session);
	pool = switch_core_session_get_pool(c->session);

	if (strict) {
		switch_channel_set_variable(c->channel, "telnyx-strict-codec-match", strict);
	}

	mparams = switch_core_session_alloc(c->session, sizeof(switch_core_media_params_t));
	mparams->inbound_codec_string = switch_core_session_strdup(c->session, codecs);
	mparams->outbound_codec_string = switch_core_session_strdup(c->session, codecs);
	mparams->rtpip = switch_core_session_strdup(c->session, "127.0.0.1");

	if (switch_media_handle_create(&media_handle, c->session, mparams) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	switch_media_handle_set_media_flag(media_handle, SCMF_AUTOFIX_TIMING);
	switch_channel_set_variable(c->channel, "absolute_codec_string", codecs);
	switch_channel_set_variable(c->channel, "rtp_timer_name", "soft");
	switch_channel_set_variable(c->channel, "jitterbuffer_msec", "60:200");
	switch_channel_set_variable(c->channel, "media_timeout", "0");
	switch_channel_set_variable(c->channel, SWITCH_LOCAL_MEDIA_IP_VARIABLE, "127.0.0.1");

	if (switch_core_media_prepare_codecs(c->session, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS ||
		switch_core_media_negotiate_sdp(c->session, vbr_sdp(c->session, media), &p, SDP_OFFER) != 1 ||
		switch_core_media_choose_ports(c->session, SWITCH_TRUE, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS ||
		switch_core_media_activate_rtp(c->session) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	if (!(rtp = switch_core_media_get_rtp_session(c->session, SWITCH_MEDIA_TYPE_AUDIO)) || !switch_rtp_get_jitter_buffer(rtp)) {
		return SWITCH_STATUS_FALSE;
	}

	switch_rtp_set_remote_address(rtp, "127.0.0.1", VBR_TX_PORT, 0, SWITCH_FALSE, &err);
	switch_rtp_clear_flag(rtp, SWITCH_RTP_FLAG_PAUSE);
	switch_rtp_clear_flag(rtp, SWITCH_RTP_FLAG_MUTE);

	if (!(port = switch_channel_get_variable(c->channel, SWITCH_LOCAL_MEDIA_PORT_VARIABLE)) ||
		switch_socket_create(&c->sock, AF_INET, SOCK_DGRAM, 0, pool) != SWITCH_STATUS_SUCCESS ||
		switch_sockaddr_new(&local, "127.0.0.1", VBR_TX_PORT, pool) != SWITCH_STATUS_SUCCESS ||
		switch_socket_opt_set(c->sock, SWITCH_SO_REUSEADDR, 1) != SWITCH_STATUS_SUCCESS ||
		switch_socket_bind(c->sock, local) != SWITCH_STATUS_SUCCESS ||
		switch_sockaddr_new(&c->to, "127.0.0.1", (switch_port_t) atoi(port), pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	c->alive = 1;

	return SWITCH_STATUS_SUCCESS;
}

static void vbr_end(vbr_call_t *c)
{
	if (c->sock) {
		switch_socket_close(c->sock);
		c->sock = NULL;
	}
	if (c->session) {
		switch_channel_hangup(c->channel, SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(c->session);
		c->session = NULL;
	}
}

static switch_status_t vbr_start(vbr_call_t *c, const char *codecs, const char *media, uint32_t clock, const char *strict)
{
	if (vbr_setup(c, codecs, media, clock, strict) != SWITCH_STATUS_SUCCESS) {
		vbr_end(c);
		return SWITCH_STATUS_FALSE;
	}

	return SWITCH_STATUS_SUCCESS;
}

/* One media read, as the endpoint's read thread does it. Returns the frame's sequence number, or -1 for no real frame. */
static int vbr_read(vbr_call_t *c)
{
	switch_frame_t *frame = NULL;

	if (!c->alive) {
		return -1;
	}

	if (switch_core_media_read_frame(c->session, &frame, SWITCH_IO_FLAG_NONE, 0, SWITCH_MEDIA_TYPE_AUDIO) != SWITCH_STATUS_SUCCESS ||
		!switch_channel_up(c->channel)) {
		if (!switch_channel_up(c->channel)) {
			c->alive = 0;
		}
		return -1;
	}

	if (frame && switch_test_flag(frame, SFF_PLC)) {
		c->plc_frames++;
	}

	if (!frame || switch_test_flag(frame, SFF_CNG) || switch_test_flag(frame, SFF_PLC) || !frame->datalen) {
		return -1;
	}

	c->last_read_seq = frame->seq;

	return frame->seq;
}

/* Send one packet whose timestamp advances step_ms (and sequence advances seq_step). Once three packets are in
 * flight, every send is followed by exactly one read, so the jitter buffer holds three frames and each read takes the
 * oldest one through the media read path. */
static void vbr_send(vbr_call_t *c, uint8_t pt, uint32_t step_ms, uint16_t seq_step, uint32_t len)
{
	uint8_t pkt[12 + 512] = { 0 };
	switch_size_t n;

	c->ts += step_ms * (c->clock / 1000);
	c->seq = (uint16_t) (c->seq + seq_step);

	pkt[0] = 0x80;
	pkt[1] = pt & 0x7f;
	pkt[2] = (uint8_t) (c->seq >> 8);
	pkt[3] = (uint8_t) c->seq;
	pkt[4] = (uint8_t) (c->ts >> 24);
	pkt[5] = (uint8_t) (c->ts >> 16);
	pkt[6] = (uint8_t) (c->ts >> 8);
	pkt[7] = (uint8_t) c->ts;
	pkt[8] = (uint8_t) (c->ssrc >> 24);
	pkt[9] = (uint8_t) (c->ssrc >> 16);
	pkt[10] = (uint8_t) (c->ssrc >> 8);
	pkt[11] = (uint8_t) c->ssrc;
	memset(pkt + 12, 0x5a, len);

	n = 12 + len;
	switch_socket_sendto(c->sock, c->to, 0, (const char *) pkt, &n);
	if (++c->sent > 3) {
		vbr_read(c);
	}
}

static void vbr_run(vbr_call_t *c, uint8_t pt, int count, uint32_t step_ms, uint32_t len)
{
	int i;

	for (i = 0; i < count && c->alive; i++) {
		vbr_send(c, pt, step_ms, 1, len);
	}
}

/* Keep sending fillers (same payload type, the given step), one read each, until the last packet sent before them (or
 * a later one) has gone through the read path, then do exactly one more read so a codec reset requested by that
 * packet runs. Returns 1 if that point was reached; a codec reset that flushes the jitter buffer can drop the packet. */
static int vbr_finish(vbr_call_t *c, uint8_t pt, uint32_t step_ms, uint32_t len)
{
	uint16_t target = c->seq;
	int seen = 0, i;

	for (i = 0; i < 40 && c->alive && !seen; i++) {
		vbr_send(c, pt, step_ms, 1, len);
		if (c->last_read_seq >= 0 && (int16_t) ((uint16_t) c->last_read_seq - target) >= 0) {
			seen = 1;
		}
	}
	vbr_read(c);

	return seen;
}

/* Send packets until one real frame goes through the read path. Returns the number of packets sent, 0 if none got
 * through within max. */
static int vbr_send_until_read(vbr_call_t *c, uint8_t pt, uint32_t step_ms, uint32_t len, int max)
{
	int i;

	for (i = 1; i <= max && c->alive; i++) {
		int before = c->last_read_seq;

		vbr_send(c, pt, step_ms, 1, len);
		if (c->last_read_seq != before) {
			return i;
		}
	}

	return 0;
}

static int vbr_ptime(vbr_call_t *c)
{
	switch_codec_implementation_t impl = { 0 };

	switch_core_session_get_read_impl(c->session, &impl);
	return (int) (impl.microseconds_per_packet / 1000);
}

static const char *vbr_codec_name(vbr_call_t *c)
{
	switch_codec_implementation_t impl = { 0 };

	switch_core_session_get_read_impl(c->session, &impl);
	return impl.iananame ? impl.iananame : "";
}

#define AMRWB_MEDIA "m=audio 9 RTP/AVP 96\r\na=rtpmap:96 AMR-WB/16000\r\na=fmtp:96 octet-align=0; mode-set=0,1,2\r\na=ptime:20\r\n"
#define OPUS_MEDIA(pt) "m=audio 9 RTP/AVP " #pt "\r\na=rtpmap:" #pt " opus/48000/2\r\na=fmtp:" #pt " useinbandfec=1; minptime=10; maxptime=40\r\na=ptime:20\r\n"

FST_CORE_DB_BEGIN("./conf_vbr")
{
	FST_SUITE_BEGIN(switch_vbr_ptime_rtp)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_amrwb");
			fst_requires_module("mod_amr");
			fst_requires_module("mod_opus");
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		FST_TEST_BEGIN(amrwb_speech_after_silence_keeps_the_call)
		{
			vbr_call_t c;
			uint32_t gap;

			fst_requires(vbr_start(&c, "AMR-WB", AMRWB_MEDIA, 16000, NULL) == SWITCH_STATUS_SUCCESS);
			vbr_run(&c, 96, 20, 20, 33);
			for (gap = 40; gap <= 120 && c.alive; gap += 20) {
				vbr_run(&c, 96, 10, 160, 7);
				vbr_run(&c, 96, 1, gap, 33);
				vbr_run(&c, 96, 20, 20, 33);
			}
			fst_check(vbr_finish(&c, 96, 20, 33));
			fst_check(c.alive);
			fst_check(vbr_ptime(&c) == 20);
			vbr_end(&c);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_unsupported_ptime_run_is_refused)
		{
			vbr_call_t c;

			fst_requires(vbr_start(&c, "AMR-WB", AMRWB_MEDIA, 16000, NULL) == SWITCH_STATUS_SUCCESS);
			vbr_run(&c, 96, 30, 20, 33);
			vbr_run(&c, 96, 10, 40, 61);
			fst_check(vbr_finish(&c, 96, 40, 61));
			fst_check(c.alive);
			fst_check(vbr_ptime(&c) == 20);
			vbr_end(&c);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(opus_mid_call_20_to_40_is_adopted)
		{
			vbr_call_t c;

			fst_requires(vbr_start(&c, "opus", OPUS_MEDIA(116), 48000, NULL) == SWITCH_STATUS_SUCCESS);
			vbr_run(&c, 116, 30, 20, 120);
			fst_check(vbr_ptime(&c) == 20);
			vbr_run(&c, 116, 10, 40, 240);
			vbr_finish(&c, 116, 40, 240);
			fst_check(c.alive);
			fst_check(vbr_ptime(&c) == 40);
			vbr_end(&c);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(opus_codec_rebuild_restarts_the_count)
		{
			vbr_call_t c;
			switch_codec_t *codec;
			void *before;
			uint8_t p = 0;
			int i;

			fst_requires(vbr_start(&c, "opus", OPUS_MEDIA(116), 48000, NULL) == SWITCH_STATUS_SUCCESS);
			vbr_run(&c, 116, 30, 20, 120);
			/* six 40 ms intervals reach the read path (count five); three more are still in flight */
			vbr_run(&c, 116, 9, 40, 240);
			fst_check(c.alive && vbr_ptime(&c) == 20);

			codec = switch_core_session_get_read_codec(c.session);
			before = codec ? codec->private_info : NULL;
			fst_check(switch_core_media_negotiate_sdp(c.session, vbr_sdp(c.session, OPUS_MEDIA(111)), &p, SDP_OFFER) == 1);
			codec = switch_core_session_get_read_codec(c.session);
			fst_check(codec && switch_core_codec_ready(codec) && codec->private_info != before);

			/* the first frame through the read path after the rebuild must not complete the old count */
			fst_check(vbr_send_until_read(&c, 111, 40, 240, 20) > 0);
			vbr_read(&c);
			fst_check(c.alive);
			fst_check(vbr_ptime(&c) == 20);

			/* seven more matching intervals after the rebuild are a genuine change */
			for (i = 0; i < 7 && c.alive; i++) {
				fst_check(vbr_send_until_read(&c, 111, 40, 240, 20) > 0);
			}
			vbr_read(&c);
			fst_check(c.alive);
			fst_check(vbr_ptime(&c) == 40);
			vbr_end(&c);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(opus_hard_mute_restarts_the_count)
		{
			vbr_call_t c;
			switch_core_session_message_t msg = { 0 };
			int i;

			fst_requires(vbr_start(&c, "opus", OPUS_MEDIA(116), 48000, NULL) == SWITCH_STATUS_SUCCESS);
			vbr_run(&c, 116, 30, 20, 120);
			/* six 40 ms intervals reach the read path; three more are still in flight */
			vbr_run(&c, 116, 9, 40, 240);
			fst_check(c.alive && vbr_ptime(&c) == 20);

			msg.message_id = SWITCH_MESSAGE_INDICATE_HARD_MUTE;
			msg.from = __FILE__;
			msg.numeric_arg = 1;
			fst_check(switch_core_media_receive_message(c.session, &msg) == SWITCH_STATUS_SUCCESS);
			msg.numeric_arg = 0;
			fst_check(switch_core_media_receive_message(c.session, &msg) == SWITCH_STATUS_SUCCESS);

			/* the first frame through the read path after the mute must not complete the old count */
			fst_check(vbr_send_until_read(&c, 116, 40, 240, 20) > 0);
			vbr_read(&c);
			fst_check(c.alive);
			fst_check(vbr_ptime(&c) == 20);

			for (i = 0; i < 7 && c.alive; i++) {
				fst_check(vbr_send_until_read(&c, 116, 40, 240, 20) > 0);
			}
			vbr_read(&c);
			fst_check(c.alive);
			fst_check(vbr_ptime(&c) == 40);
			vbr_end(&c);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amr_strict_framing_reoffer_keeps_the_stream)
		{
			vbr_call_t c;
			switch_codec_t *codec;
			void *before;
			uint8_t p = 0;
			const char *be = "m=audio 9 RTP/AVP 97\r\na=rtpmap:97 AMR/8000\r\na=fmtp:97 octet-align=0\r\na=ptime:20\r\n";
			const char *oa = "m=audio 9 RTP/AVP 97\r\na=rtpmap:97 AMR/8000\r\na=fmtp:97 octet-align=1\r\na=ptime:20\r\n";

			fst_requires(vbr_start(&c, "AMR", be, 8000, "true") == SWITCH_STATUS_SUCCESS);
			vbr_run(&c, 97, 20, 20, 14);
			vbr_run(&c, 97, 6, 40, 27);

			codec = switch_core_session_get_read_codec(c.session);
			before = codec ? codec->private_info : NULL;
			fst_check(switch_core_media_negotiate_sdp(c.session, vbr_sdp(c.session, oa), &p, SDP_OFFER) == 1);
			codec = switch_core_session_get_read_codec(c.session);
			fst_check(codec && switch_core_codec_ready(codec) && codec->private_info != before);

			vbr_run(&c, 97, 10, 40, 27);
			vbr_run(&c, 97, 10, 160, 7);
			vbr_run(&c, 97, 1, 60, 14);
			vbr_run(&c, 97, 20, 20, 14);
			fst_check(vbr_finish(&c, 97, 20, 14));
			fst_check(c.alive);
			fst_check(vbr_ptime(&c) == 20);
			vbr_end(&c);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(refused_candidate_frame_still_switches_payload)
		{
			vbr_call_t c;
			const char *media = "m=audio 9 RTP/AVP 96 0\r\na=rtpmap:96 AMR-WB/16000\r\na=fmtp:96 octet-align=0\r\na=rtpmap:0 PCMU/8000\r\na=ptime:20\r\n";

			fst_requires(vbr_start(&c, "AMR-WB,PCMU", media, 16000, NULL) == SWITCH_STATUS_SUCCESS);
			fst_check(!strcasecmp(vbr_codec_name(&c), "AMR-WB"));
			vbr_run(&c, 96, 30, 20, 33);
			vbr_run(&c, 96, 6, 40, 61);
			/* the seventh 40 ms interval is the refused candidate, and it carries the other negotiated payload type */
			vbr_run(&c, 0, 1, 40, 160);
			fst_check(vbr_finish(&c, 0, 20, 160));
			fst_check(c.alive);
			fst_check(!strcasecmp(vbr_codec_name(&c), "PCMU"));
			vbr_end(&c);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(oversized_gap_frame_still_switches_payload)
		{
			vbr_call_t c;
			const char *media = "m=audio 9 RTP/AVP 96 0\r\na=rtpmap:96 AMR-WB/16000\r\na=fmtp:96 octet-align=0\r\na=rtpmap:0 PCMU/8000\r\na=ptime:20\r\n";

			fst_requires(vbr_start(&c, "AMR-WB,PCMU", media, 16000, NULL) == SWITCH_STATUS_SUCCESS);
			vbr_run(&c, 96, 30, 20, 33);
			vbr_run(&c, 96, 6, 160, 7);
			vbr_run(&c, 0, 1, 160, 160);
			fst_check(vbr_finish(&c, 0, 20, 160));
			fst_check(c.alive);
			fst_check(!strcasecmp(vbr_codec_name(&c), "PCMU"));
			vbr_end(&c);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(opus_packet_loss_is_not_a_ptime_change)
		{
			vbr_call_t c;
			int i;

			fst_requires(vbr_start(&c, "opus", OPUS_MEDIA(116), 48000, NULL) == SWITCH_STATUS_SUCCESS);
			vbr_run(&c, 116, 30, 20, 120);
			for (i = 0; i < 20 && c.alive; i++) {
				vbr_send(&c, 116, 40, 2, 120);
			}
			vbr_run(&c, 116, 10, 20, 120);
			fst_check(vbr_finish(&c, 116, 20, 120));
			fst_check(c.plc_frames > 0);
			fst_check(c.alive);
			fst_check(vbr_ptime(&c) == 20);
			vbr_end(&c);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(opus_dtx_then_speech_keeps_ptime)
		{
			vbr_call_t c;

			fst_requires(vbr_start(&c, "opus", OPUS_MEDIA(116), 48000, NULL) == SWITCH_STATUS_SUCCESS);
			vbr_run(&c, 116, 30, 20, 120);
			vbr_run(&c, 116, 8, 400, 3);
			vbr_run(&c, 116, 1, 60, 120);
			vbr_run(&c, 116, 30, 20, 120);
			fst_check(vbr_finish(&c, 116, 20, 120));
			fst_check(c.alive);
			fst_check(vbr_ptime(&c) == 20);
			vbr_end(&c);
		}
		FST_TEST_END()
		FST_TEST_BEGIN(opus_dtx_then_supported_gap_keeps_ptime)
		{
			vbr_call_t c;

			fst_requires(vbr_start(&c, "opus", OPUS_MEDIA(116), 48000, NULL) == SWITCH_STATUS_SUCCESS);
			vbr_run(&c, 116, 30, 20, 120);
			vbr_run(&c, 116, 8, 400, 3);
			vbr_run(&c, 116, 1, 40, 120);
			fst_check(vbr_finish(&c, 116, 20, 120));
			fst_check(c.alive);
			fst_check(vbr_ptime(&c) == 20);
			vbr_end(&c);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
