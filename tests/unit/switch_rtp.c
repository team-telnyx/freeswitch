
#include <switch.h>
#include <test/switch_test.h>
#include "../../libs/srtp/include/srtp.h"

#pragma weak srtp_create
#pragma weak srtp_unprotect
#pragma weak srtp_dealloc
#pragma weak srtp_crypto_policy_set_rtp_default

#ifndef MSG_CONFIRM
#define MSG_CONFIRM 0
#endif

static const char *rx_host = "127.0.0.1";
static switch_port_t rx_port = 1234;
static const char *tx_host = "127.0.0.1";
static switch_port_t tx_port = 54320;
static switch_memory_pool_t *pool = NULL;
static switch_rtp_t *rtp_session = NULL;
static switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID] = {0};
const char *err = NULL;
static const switch_payload_t TEST_PT = 8;
switch_rtp_packet_t rtp_packet;
switch_frame_flag_t *frame_flags;
switch_io_flag_t io_flags;
switch_payload_t read_pt;
int send_rtcp_test_success = 0;

static int make_udp_sink(switch_port_t *port)
{
	struct sockaddr_in addr;
	socklen_t addr_len = sizeof(addr);
	int fd;

	if (!port) return -1;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
		getsockname(fd, (struct sockaddr *)&addr, &addr_len) < 0) {
		close(fd);
		return -1;
	}

	*port = ntohs(addr.sin_port);
	return fd;
}

static switch_bool_t recv_rtp_pt_ts(int fd, switch_payload_t pt, uint32_t *ts, int timeout_ms)
{
	fd_set read_fds;
	struct timeval timeout;
	uint8_t packet[SWITCH_RTP_MAX_PACKET_LEN];
	ssize_t bytes;

	if (fd < 0 || !ts || timeout_ms < 0) return SWITCH_FALSE;

	FD_ZERO(&read_fds);
	FD_SET(fd, &read_fds);
	timeout.tv_sec = timeout_ms / 1000;
	timeout.tv_usec = (timeout_ms % 1000) * 1000;
	if (select(fd + 1, &read_fds, NULL, NULL, &timeout) != 1) return SWITCH_FALSE;

	bytes = recvfrom(fd, packet, sizeof(packet), 0, NULL, NULL);
	if (bytes < SWITCH_RTP_HEADER_LEN) return SWITCH_FALSE;
	if ((packet[1] & 0x7f) != pt) return recv_rtp_pt_ts(fd, pt, ts, timeout_ms);

	*ts = ((uint32_t)packet[4] << 24) | ((uint32_t)packet[5] << 16) | ((uint32_t)packet[6] << 8) | packet[7];
	return SWITCH_TRUE;
}

static switch_bool_t recv_rtp_ts(int fd, uint32_t *ts, int timeout_ms)
{
	return recv_rtp_pt_ts(fd, TEST_PT, ts, timeout_ms);
}

static switch_bool_t raw_write_event_ts(switch_rtp_t *raw_rtp, int sink_fd, uint32_t src_ts, uint32_t *out_ts)
{
	switch_rtp_packet_t packet;
	switch_frame_t frame = { 0 };

	memset(&packet, 0, sizeof(packet));
	packet.header.version = 2;
	packet.header.pt = 101;
	packet.header.ts = htonl(src_ts);
	packet.header.ssrc = htonl(0x11223344);
	packet.body[0] = 1;
	packet.body[1] = 10;

	frame.packet = &packet;
	frame.packetlen = SWITCH_RTP_HEADER_LEN + 4;
	frame.data = packet.body;
	frame.datalen = 4;
	frame.payload = 101;
	frame.timestamp = src_ts;
	frame.flags = SFF_RAW_RTP | SFF_EXTERNAL | SFF_RFC2833;

	if (switch_rtp_write_frame(raw_rtp, &frame) <= 0) return SWITCH_FALSE;

	return recv_rtp_pt_ts(sink_fd, 101, out_ts, 1000);
}

static switch_bool_t raw_write_srtp_ts(switch_rtp_t *raw_rtp, int sink_fd, srtp_t srtp_rx, uint32_t src_ts, uint32_t *out_ts, uint16_t *out_seq)
{
	switch_rtp_packet_t packet;
	switch_frame_t frame = { 0 };
	uint8_t buf[SWITCH_RTP_MAX_PACKET_LEN];
	struct timeval timeout = { 1, 0 };
	fd_set read_fds;
	ssize_t bytes;
	int len;

	memset(&packet, 0, sizeof(packet));
	packet.header.version = 2;
	packet.header.pt = TEST_PT;
	packet.header.ts = htonl(src_ts);
	packet.header.ssrc = htonl(0x11223344);
	packet.body[0] = (char)0xaa;

	frame.packet = &packet;
	frame.packetlen = SWITCH_RTP_HEADER_LEN + 1;
	frame.data = packet.body;
	frame.datalen = 1;
	frame.payload = TEST_PT;
	frame.timestamp = src_ts;
	frame.flags = SFF_RAW_RTP | SFF_EXTERNAL;

	if (switch_rtp_write_frame(raw_rtp, &frame) <= 0) return SWITCH_FALSE;

	FD_ZERO(&read_fds);
	FD_SET(sink_fd, &read_fds);
	if (select(sink_fd + 1, &read_fds, NULL, NULL, &timeout) != 1) return SWITCH_FALSE;
	bytes = recvfrom(sink_fd, buf, sizeof(buf), 0, NULL, NULL);
	if (bytes < SWITCH_RTP_HEADER_LEN) return SWITCH_FALSE;

	len = (int) bytes;
	if (srtp_unprotect(srtp_rx, buf, &len) != srtp_err_status_ok) return SWITCH_FALSE;

	*out_seq = (uint16_t)(((uint16_t)buf[2] << 8) | buf[3]);
	*out_ts = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 8) | buf[7];
	return SWITCH_TRUE;
}

static switch_rtp_t *new_raw_write_rtp(switch_memory_pool_t *test_pool, switch_port_t sink_port, switch_bool_t rebase);

static switch_bool_t run_srtp_rebase_case(const char **why)
{
	switch_memory_pool_t *test_pool = NULL;
	switch_core_session_t *session = NULL;
	switch_call_cause_t cause;
	switch_rtp_t *raw_rtp = NULL;
	switch_port_t sink_port = 0;
	switch_secure_settings_t ssec;
	srtp_policy_t policy;
	srtp_t srtp_rx = NULL;
	switch_time_t sent_at;
	uint32_t out = 0, step;
	uint16_t seq = 0, prev_seq = 0;
	int sink_fd = -1, i;
	switch_bool_t ok = SWITCH_FALSE;

	*why = "setup";
	if (switch_ivr_originate(NULL, &session, &cause, "null/+15553334444", 2, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS || !session) goto end;
	if (switch_core_new_memory_pool(&test_pool) != SWITCH_STATUS_SUCCESS) goto end;
	switch_core_memory_pool_set_data(test_pool, "__session", session);
	if ((sink_fd = make_udp_sink(&sink_port)) < 0) goto end;
	if (!(raw_rtp = new_raw_write_rtp(test_pool, sink_port, SWITCH_TRUE))) goto end;

	memset(&ssec, 0, sizeof(ssec));
	ssec.crypto_type = AES_CM_128_HMAC_SHA1_80;
	for (i = 0; i < 30; i++) {
		ssec.local_raw_key[i] = (unsigned char)(i * 7 + 1);
	}
	*why = "send key";
	if (switch_rtp_add_crypto_key(raw_rtp, SWITCH_RTP_CRYPTO_SEND, 1, &ssec) != SWITCH_STATUS_SUCCESS) goto end;

	memset(&policy, 0, sizeof(policy));
	srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtp);
	srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtcp);
	policy.ssrc.type = ssrc_any_inbound;
	policy.key = ssec.local_raw_key;
	policy.window_size = 1024;
	*why = "receive context";
	if (srtp_create(&srtp_rx, &policy) != srtp_err_status_ok) goto end;

	*why = "first packet";
	if (!raw_write_srtp_ts(raw_rtp, sink_fd, srtp_rx, 1000, &out, &prev_seq) || out != 1000) goto end;
	*why = "inconsistent jump not corrected once";
	if (!raw_write_srtp_ts(raw_rtp, sink_fd, srtp_rx, 1000 + 48000, &out, &seq) || out != 1320 || seq != (uint16_t)(prev_seq + 1)) goto end;
	prev_seq = seq;
	*why = "source spacing after the correction";
	if (!raw_write_srtp_ts(raw_rtp, sink_fd, srtp_rx, 1000 + 48000 + 2560, &out, &seq) || out != 3880 || seq != (uint16_t)(prev_seq + 1)) goto end;
	prev_seq = seq;
	sent_at = switch_time_ref();

	switch_yield(2500000);
	step = (uint32_t) ((switch_time_ref() - sent_at) * 16000 / 1000000);
	*why = "real pause not kept";
	if (step <= 32000) goto end;
	if (!raw_write_srtp_ts(raw_rtp, sink_fd, srtp_rx, 1000 + 48000 + 2560 + step, &out, &seq) || out != 3880 + step || seq != (uint16_t)(prev_seq + 1)) goto end;

	ok = SWITCH_TRUE;
	*why = NULL;

 end:
	if (srtp_rx) srtp_dealloc(srtp_rx);
	if (raw_rtp) switch_rtp_destroy(&raw_rtp);
	if (sink_fd >= 0) close(sink_fd);
	if (test_pool) switch_core_destroy_memory_pool(&test_pool);
	if (session) {
		switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
	}
	return ok;
}

static switch_bool_t payload_write_ts(switch_rtp_t *raw_rtp, int sink_fd, uint32_t src_ts, uint32_t *out_ts)
{
	uint8_t data = 0xaa;
	switch_frame_t frame = { 0 };

	frame.data = &data;
	frame.datalen = 1;
	frame.payload = TEST_PT;
	frame.timestamp = src_ts;

	if (switch_rtp_write_frame(raw_rtp, &frame) <= 0) return SWITCH_FALSE;

	return recv_rtp_ts(sink_fd, out_ts, 1000);
}

static switch_rtp_t *new_raw_write_rtp(switch_memory_pool_t *test_pool, switch_port_t sink_port, switch_bool_t rebase)
{
	switch_rtp_flag_t raw_flags[SWITCH_RTP_FLAG_INVALID] = { 0 };
	const char *raw_err = NULL;
	switch_port_t local_port = switch_rtp_request_port(rx_host);
	switch_rtp_t *raw_rtp;

	if (!local_port) return NULL;

	raw_flags[SWITCH_RTP_FLAG_RAW_WRITE] = 1;
	if (rebase) {
		raw_flags[SWITCH_RTP_FLAG_REBASE_TS_ON_JUMP] = 1;
	}
	raw_rtp = switch_rtp_new(rx_host, local_port, tx_host, sink_port, TEST_PT, 320, 20 * 1000,
		raw_flags, "soft", &raw_err, test_pool);
	if (raw_rtp) {
		switch_rtp_clear_flag(raw_rtp, SWITCH_RTP_FLAG_PAUSE);
	}

	return raw_rtp;
}

static switch_bool_t raw_write_ts(switch_rtp_t *raw_rtp, int sink_fd, uint32_t src_ts, uint32_t *out_ts)
{
	switch_rtp_packet_t packet;
	switch_frame_t frame = { 0 };

	memset(&packet, 0, sizeof(packet));
	packet.header.version = 2;
	packet.header.pt = TEST_PT;
	packet.header.ts = htonl(src_ts);
	packet.header.ssrc = htonl(0x11223344);
	packet.body[0] = (char)0xaa;

	frame.packet = &packet;
	frame.packetlen = SWITCH_RTP_HEADER_LEN + 1;
	frame.data = packet.body;
	frame.datalen = 1;
	frame.payload = TEST_PT;
	frame.timestamp = src_ts;
	frame.flags = SFF_RAW_RTP | SFF_EXTERNAL;

	if (switch_rtp_write_frame(raw_rtp, &frame) <= 0) return SWITCH_FALSE;

	return recv_rtp_ts(sink_fd, out_ts, 1000);
}

static void show_event(switch_event_t *event) {
	char *str;
	/*print the event*/
	switch_event_serialize_json(event, &str);
	if (str) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s\n", str);
		switch_safe_free(str);
	}
}

static void send_rtcp_event_handler(switch_event_t *event) 
{
	const char *new_ev = switch_event_get_header(event, "Event-Name");

	if (new_ev && !strcmp(new_ev, "SEND_RTCP_MESSAGE")) { 
		send_rtcp_test_success = 1;
	}

	show_event(event);
}

FST_CORE_BEGIN("./conf")
{
FST_SUITE_BEGIN(switch_rtp)
{
FST_SETUP_BEGIN()
{
	fst_requires_module("mod_loopback");
}
FST_SETUP_END()

FST_TEARDOWN_BEGIN()
{
}
FST_TEARDOWN_END()
	FST_TEST_BEGIN(test_rtp)
	{
		switch_rtp_stats_t *stats;
		switch_core_new_memory_pool(&pool);
		
		rtp_session = switch_rtp_new(rx_host, rx_port, tx_host, tx_port, TEST_PT, 8000, 20 * 1000, flags, "soft", &err, pool);
		fst_xcheck(rtp_session != NULL, "get RTP session");
		fst_requires(rtp_session);
		fst_requires(switch_rtp_ready(rtp_session));
		switch_rtp_activate_rtcp(rtp_session, 5, rx_port + 1, 0);
		switch_rtp_set_default_payload(rtp_session, TEST_PT);
		fst_xcheck(switch_rtp_get_default_payload(rtp_session) == TEST_PT, "get Payload Type");
		switch_rtp_set_ssrc(rtp_session, 0xabcd);
		switch_rtp_set_remote_ssrc(rtp_session, 0xcdef);
		fst_xcheck(switch_rtp_get_ssrc(rtp_session) == 0xabcd, "get SSRC");
		stats = switch_rtp_get_stats(rtp_session, pool);
		fst_requires(stats);
		switch_rtp_destroy(&rtp_session);

		switch_core_destroy_memory_pool(&pool);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_session_with_rtp)
	{
		switch_core_session_t *session = NULL;
		switch_channel_t *channel = NULL;
		switch_status_t status;
		switch_call_cause_t cause;

		switch_core_new_memory_pool(&pool);

		status = switch_ivr_originate(NULL, &session, &cause, "null/+15553334444", 2, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
		fst_requires(session);
		fst_check(status == SWITCH_STATUS_SUCCESS);

		channel = switch_core_session_get_channel(session);
		fst_requires(channel);

		switch_core_memory_pool_set_data(pool, "__session", session);
		session = switch_core_memory_pool_get_data(pool, "__session");
		fst_requires(session);
		rtp_session = switch_rtp_new(rx_host, rx_port, tx_host, tx_port, TEST_PT, 8000, 20 * 1000, flags, "soft", &err, pool);
		fst_xcheck(rtp_session != NULL, "switch_rtp_new()");
		fst_requires(switch_rtp_ready(rtp_session));
		switch_rtp_activate_rtcp(rtp_session, 5, rx_port + 1, 0);
		switch_rtp_set_default_payload(rtp_session, TEST_PT);
		switch_core_media_set_rtp_session(session, SWITCH_MEDIA_TYPE_AUDIO, rtp_session);
		channel = switch_core_session_get_channel(session);
		fst_requires(channel);
		session = switch_rtp_get_core_session(rtp_session);
		fst_requires(session);
		status = switch_rtp_activate_jitter_buffer(rtp_session, 1, 10, 80, 8000);
		fst_xcheck(status == SWITCH_STATUS_SUCCESS, "switch_rtp_activate_jitter_buffer()");
		status = switch_rtp_debug_jitter_buffer(rtp_session, "debug");
		fst_xcheck(status == SWITCH_STATUS_SUCCESS, "switch_rtp_debug_jitter_buffer()");
		fst_requires(switch_rtp_get_jitter_buffer(rtp_session));
		status = switch_rtp_pause_jitter_buffer(rtp_session, SWITCH_TRUE);
		fst_xcheck(status == SWITCH_STATUS_SUCCESS, "switch_rtp_pause_jitter_buffer()");
		status = switch_rtp_deactivate_jitter_buffer(rtp_session);
		fst_xcheck(status == SWITCH_STATUS_SUCCESS, "switch_rtp_deactivate_jitter_buffer()");

		switch_rtp_destroy(&rtp_session);
		switch_core_session_rwunlock(session);
		switch_core_destroy_memory_pool(&pool);
	}
	FST_TEST_END()
	FST_TEST_BEGIN(test_send_rtcp_event_audio)
	{
		switch_core_session_t *session = NULL;
		switch_channel_t *channel = NULL;
		switch_status_t status;
		switch_call_cause_t cause;
		switch_stream_handle_t stream = { 0 };
		const unsigned char packet[]="\x80\x00\xcd\x15\xfd\x86\x00\x00\x61\x5a\xe1\x37";
		uint32_t plen = 12;
		char rpacket[SWITCH_RECOMMENDED_BUFFER_SIZE];
		switch_payload_t pt = { 0 };
		switch_frame_flag_t frameflags = { 0 };
		static switch_port_t audio_rx_port = 1234;
		switch_media_handle_t *media_handle;
		switch_core_media_params_t *mparams;
		char *r_sdp;
		uint8_t match = 0, p = 0;
		struct sockaddr_in sin;
		socklen_t len = sizeof(sin);
		int x;
		struct sockaddr_in servaddr_rtp; 
		int sockfd_rtp;
		struct hostent *server;
		int ret;
		switch_frame_t *read_frame, *write_frame;

		switch_event_bind("", SWITCH_EVENT_ALL, SWITCH_EVENT_SUBCLASS_ANY, send_rtcp_event_handler, NULL);

		status = switch_ivr_originate(NULL, &session, &cause, "null/+15553334444", 2, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
		fst_requires(session);
		fst_check(status == SWITCH_STATUS_SUCCESS);

		channel = switch_core_session_get_channel(session);
		fst_requires(channel);
		mparams  = switch_core_session_alloc(session, sizeof(switch_core_media_params_t));
		mparams->num_codecs = 1;
		mparams->inbound_codec_string = switch_core_session_strdup(session, "PCMU");
		mparams->outbound_codec_string = switch_core_session_strdup(session, "PCMU");
		mparams->rtpip = switch_core_session_strdup(session, (char *)rx_host);

		status = switch_media_handle_create(&media_handle, session, mparams);
		fst_requires(status == SWITCH_STATUS_SUCCESS);

		switch_channel_set_variable(channel, "absolute_codec_string", "PCMU");
		switch_channel_set_variable(channel, "fire_rtcp_events", "true");
		switch_channel_set_variable(channel, "send_silence_when_idle", "-1");

		switch_channel_set_variable(channel, SWITCH_LOCAL_MEDIA_IP_VARIABLE, rx_host);
		switch_channel_set_variable_printf(channel, SWITCH_LOCAL_MEDIA_PORT_VARIABLE, "%d", audio_rx_port);

		r_sdp = switch_core_session_sprintf(session,
		"v=0\n"
		"o=FreeSWITCH 1632033305 1632033306 IN IP4 %s\n"
		"s=-\n"
		"c=IN IP4 %s\n"
		"t=0 0\n"
		"m=audio 11114 RTP/AVP 0 101\n"
		"a=rtpmap:0 PCMU/8000\n"
		"a=rtpmap:101 telephone-event/8000\n"
		"a=rtcp:11115\n",
		tx_host, tx_host);
		 
		switch_core_media_prepare_codecs(session, SWITCH_FALSE);
		   
		match = switch_core_media_negotiate_sdp(session, r_sdp, &p, SDP_OFFER);
		fst_requires(match == 1);

		status = switch_core_media_choose_ports(session, SWITCH_TRUE, SWITCH_FALSE);
		fst_requires(status == SWITCH_STATUS_SUCCESS);

		status = switch_core_media_activate_rtp(session);
		fst_requires(status == SWITCH_STATUS_SUCCESS);

		switch_core_media_set_rtp_flag(session, SWITCH_MEDIA_TYPE_AUDIO, SWITCH_RTP_FLAG_DEBUG_RTP_READ);
		switch_core_media_set_rtp_flag(session, SWITCH_MEDIA_TYPE_AUDIO, SWITCH_RTP_FLAG_DEBUG_RTP_WRITE);
		switch_core_media_set_rtp_flag(session, SWITCH_MEDIA_TYPE_AUDIO, SWITCH_RTP_FLAG_AUDIO_FIRE_SEND_RTCP_EVENT);
		switch_core_media_set_rtp_flag(session, SWITCH_MEDIA_TYPE_AUDIO, SWITCH_RTP_FLAG_ENABLE_RTCP);


		switch_frame_alloc(&write_frame, SWITCH_RECOMMENDED_BUFFER_SIZE);
		write_frame->codec = switch_core_session_get_write_codec(session);

		SWITCH_STANDARD_STREAM(stream);
		switch_api_execute("fsctl", "debug_level 9", session, &stream);
		switch_safe_free(stream.data);

		if ((sockfd_rtp = socket(AF_INET, SOCK_DGRAM, 0)) < 0) { 
			perror("socket creation failed"); 
			fst_requires(0); /*exit*/ 
		}

		memset(&servaddr_rtp, 0, sizeof(servaddr_rtp)); 
		                                    
		servaddr_rtp.sin_family = AF_INET; 
		servaddr_rtp.sin_port = htons(audio_rx_port); 
		server = gethostbyname(rx_host);
		bcopy((char *)server->h_addr, (char *)&servaddr_rtp.sin_addr.s_addr, server->h_length);

		/*get local UDP port (tx side) to trick FS into accepting our packets*/
		ret = sendto(sockfd_rtp, NULL, 0, MSG_CONFIRM, (const struct sockaddr *) &servaddr_rtp, sizeof(servaddr_rtp)); 
		if (ret < 0){
			perror("sendto");
			fst_requires(0);
		}

		rtp_session = switch_core_media_get_rtp_session(session, SWITCH_MEDIA_TYPE_AUDIO);
		len = sizeof(sin);
		if (getsockname(sockfd_rtp, (struct sockaddr *)&sin, &len) == -1) {
			perror("getsockname");
			fst_requires(0);
		} else {
			switch_rtp_set_remote_address(rtp_session, tx_host, ntohs(sin.sin_port), 0, SWITCH_FALSE, &err);
			switch_rtp_reset(rtp_session);
		}

		write_frame->datalen = plen;
		memcpy(write_frame->data, &packet, plen);

		switch_rtp_clear_flag(rtp_session, SWITCH_RTP_FLAG_PAUSE);

		for (x = 0; x < 3; x++) {

			switch_rtp_write_frame(rtp_session, write_frame);  /* rtp_session->stats.rtcp.sent_pkt_count++; */

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Sent RTP. Packet size = [%u]\n", plen);
			ret = sendto(sockfd_rtp, (const char *) &packet, plen, MSG_CONFIRM, (const struct sockaddr *) &servaddr_rtp, sizeof(servaddr_rtp));
			if (ret < 0){
				perror("sendto");
				fst_requires(0);
			}

			status = switch_rtp_read(rtp_session, (void *)&rpacket, &plen, &pt, &frameflags, io_flags);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			plen = 12;
			if (pt == SWITCH_RTP_CNG_PAYLOAD /*timeout*/) continue;

			status = switch_core_session_read_frame(session, &read_frame, frameflags, 0);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
		}
		switch_sleep(3000 * 1000);
		
		fst_requires(send_rtcp_test_success);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);

		if (write_frame) switch_frame_free(&write_frame);

		switch_rtp_destroy(&rtp_session);

		switch_media_handle_destroy(session);

		switch_core_session_rwunlock(session);
	}
	FST_TEST_END()


	FST_TEST_BEGIN(test_raw_write_ts_jump_is_rebased_once)
	{
		switch_memory_pool_t *test_pool = NULL;
		switch_rtp_t *raw_rtp = NULL;
		switch_port_t sink_port = 0;
		uint32_t out = 0;
		int sink_fd;

		fst_requires(switch_core_new_memory_pool(&test_pool) == SWITCH_STATUS_SUCCESS);
		sink_fd = make_udp_sink(&sink_port);
		fst_requires(sink_fd >= 0);
		raw_rtp = new_raw_write_rtp(test_pool, sink_port, SWITCH_TRUE);
		fst_requires(raw_rtp);

		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1000, &out));
		fst_check(out == 1000);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1320, &out));
		fst_check(out == 1320);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1320 + 48000, &out));
		fst_check(out == 1640);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1320 + 48000 + 320, &out));
		fst_check(out == 1960);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1320 + 48000 + 320 + 2560, &out));
		fst_check(out == 4520);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1320 + 48000 + 320 + 2560 + 320, &out));
		fst_check(out == 4840);

		switch_rtp_destroy(&raw_rtp);
		close(sink_fd);
		switch_core_destroy_memory_pool(&test_pool);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_raw_write_ts_backward_jump_is_rebased_once)
	{
		switch_memory_pool_t *test_pool = NULL;
		switch_rtp_t *raw_rtp = NULL;
		switch_port_t sink_port = 0;
		uint32_t out = 0;
		int sink_fd;

		fst_requires(switch_core_new_memory_pool(&test_pool) == SWITCH_STATUS_SUCCESS);
		sink_fd = make_udp_sink(&sink_port);
		fst_requires(sink_fd >= 0);
		raw_rtp = new_raw_write_rtp(test_pool, sink_port, SWITCH_TRUE);
		fst_requires(raw_rtp);

		fst_requires(raw_write_ts(raw_rtp, sink_fd, 30000, &out));
		fst_check(out == 30000);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, (uint32_t)(30000 - 48000), &out));
		fst_check(out == 30320);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, (uint32_t)(30000 - 48000 + 2560), &out));
		fst_check(out == 32880);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, (uint32_t)(30000 - 48000 + 2560 + 320), &out));
		fst_check(out == 33200);

		switch_rtp_destroy(&raw_rtp);
		close(sink_fd);
		switch_core_destroy_memory_pool(&test_pool);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_raw_write_ts_real_pause_is_kept)
	{
		switch_memory_pool_t *test_pool = NULL;
		switch_rtp_t *raw_rtp = NULL;
		switch_port_t sink_port = 0;
		uint32_t out = 0;
		switch_time_t sent_at;
		uint32_t step;
		int sink_fd;

		fst_requires(switch_core_new_memory_pool(&test_pool) == SWITCH_STATUS_SUCCESS);
		sink_fd = make_udp_sink(&sink_port);
		fst_requires(sink_fd >= 0);
		raw_rtp = new_raw_write_rtp(test_pool, sink_port, SWITCH_TRUE);
		fst_requires(raw_rtp);

		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1000, &out));
		fst_check(out == 1000);
		sent_at = switch_time_ref();
		switch_yield(2500000);
		step = (uint32_t) ((switch_time_ref() - sent_at) * 16000 / 1000000);
		fst_requires(step > 32000);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1000 + step, &out));
		fst_check(out == 1000 + step);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1000 + step + 2560, &out));
		fst_check(out == 1000 + step + 2560);

		switch_rtp_destroy(&raw_rtp);
		close(sink_fd);
		switch_core_destroy_memory_pool(&test_pool);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_raw_write_ts_two_second_boundary_and_wrap)
	{
		switch_memory_pool_t *test_pool = NULL;
		switch_rtp_t *raw_rtp = NULL;
		switch_port_t sink_port = 0;
		uint32_t out = 0;
		int sink_fd;

		fst_requires(switch_core_new_memory_pool(&test_pool) == SWITCH_STATUS_SUCCESS);
		sink_fd = make_udp_sink(&sink_port);
		fst_requires(sink_fd >= 0);
		raw_rtp = new_raw_write_rtp(test_pool, sink_port, SWITCH_TRUE);
		fst_requires(raw_rtp);

		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1000, &out));
		fst_check(out == 1000);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1000 + 32000, &out));
		fst_check(out == 33000);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 33000 + 32001, &out));
		fst_check(out == 33320);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 33000 + 32001 + 320, &out));
		fst_check(out == 33640);

		switch_rtp_destroy(&raw_rtp);

		raw_rtp = new_raw_write_rtp(test_pool, sink_port, SWITCH_TRUE);
		fst_requires(raw_rtp);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 0xffffff00, &out));
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 0xffffff00 + 320, &out));
		fst_check(out == 0x40);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 0x40 + 320, &out));
		fst_check(out == 0x180);

		switch_rtp_destroy(&raw_rtp);
		close(sink_fd);
		switch_core_destroy_memory_pool(&test_pool);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_raw_write_ts_jump_without_rebase_keeps_legacy_clamp)
	{
		switch_memory_pool_t *test_pool = NULL;
		switch_rtp_t *raw_rtp = NULL;
		switch_port_t sink_port = 0;
		uint32_t out = 0;
		int sink_fd;

		fst_requires(switch_core_new_memory_pool(&test_pool) == SWITCH_STATUS_SUCCESS);
		sink_fd = make_udp_sink(&sink_port);
		fst_requires(sink_fd >= 0);
		raw_rtp = new_raw_write_rtp(test_pool, sink_port, SWITCH_FALSE);
		fst_requires(raw_rtp);

		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1000, &out));
		fst_check(out == 1000);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1000 + 48000, &out));
		fst_check(out == 1320);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1000 + 48000 + 2560, &out));
		fst_check(out == 1640);

		switch_rtp_destroy(&raw_rtp);
		close(sink_fd);
		switch_core_destroy_memory_pool(&test_pool);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_raw_write_ts_rebase_applies_to_payload_frames)
	{
		switch_memory_pool_t *test_pool = NULL;
		switch_rtp_t *raw_rtp = NULL;
		switch_port_t sink_port = 0;
		uint32_t out = 0;
		int sink_fd;

		fst_requires(switch_core_new_memory_pool(&test_pool) == SWITCH_STATUS_SUCCESS);
		sink_fd = make_udp_sink(&sink_port);
		fst_requires(sink_fd >= 0);
		raw_rtp = new_raw_write_rtp(test_pool, sink_port, SWITCH_TRUE);
		fst_requires(raw_rtp);

		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1000, &out));
		fst_check(out == 1000);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1000 + 48000, &out));
		fst_check(out == 1320);
		fst_requires(payload_write_ts(raw_rtp, sink_fd, 1000 + 48000 + 320, &out));
		fst_check(out == 1640);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1000 + 48000 + 640, &out));
		fst_check(out == 1960);
		fst_requires(payload_write_ts(raw_rtp, sink_fd, 1000 + 48000 + 640 + 48000, &out));
		fst_check(out == 2280);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1000 + 48000 + 640 + 48000 + 2560, &out));
		fst_check(out == 4840);

		switch_rtp_destroy(&raw_rtp);
		close(sink_fd);
		switch_core_destroy_memory_pool(&test_pool);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_raw_write_ts_real_pause_after_generated_dtmf_is_kept)
	{
		switch_memory_pool_t *test_pool = NULL;
		switch_rtp_t *raw_rtp = NULL;
		switch_port_t sink_port = 0;
		switch_dtmf_t dtmf = { '1', 16000, 0, SWITCH_DTMF_RTP };
		switch_time_t sent_at;
		uint32_t out = 0, src;
		int sink_fd, i;

		fst_requires(switch_core_new_memory_pool(&test_pool) == SWITCH_STATUS_SUCCESS);
		sink_fd = make_udp_sink(&sink_port);
		fst_requires(sink_fd >= 0);
		raw_rtp = new_raw_write_rtp(test_pool, sink_port, SWITCH_TRUE);
		fst_requires(raw_rtp);
		switch_rtp_set_telephony_event(raw_rtp, 101);

		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1000, &out));
		fst_check(out == 1000);
		sent_at = switch_time_ref();

		fst_requires(switch_rtp_queue_rfc2833(raw_rtp, &dtmf) == SWITCH_STATUS_SUCCESS);
		for (i = 0; i < 70; i++) {
			do_2833(raw_rtp);
			switch_yield(20000);
		}
		switch_yield(4000000 - (switch_time_ref() - sent_at));

		src = 1000 + (uint32_t) ((switch_time_ref() - sent_at) * 16000 / 1000000);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, src, &out));
		fst_check(out == src);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, src + 2560, &out));
		fst_check(out == src + 2560);

		switch_rtp_destroy(&raw_rtp);
		close(sink_fd);
		switch_core_destroy_memory_pool(&test_pool);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_raw_write_ts_rate_change_drops_old_mapping)
	{
		switch_memory_pool_t *test_pool = NULL;
		switch_rtp_t *raw_rtp = NULL;
		switch_port_t sink_port = 0;
		uint32_t out = 0;
		int sink_fd;

		fst_requires(switch_core_new_memory_pool(&test_pool) == SWITCH_STATUS_SUCCESS);
		sink_fd = make_udp_sink(&sink_port);
		fst_requires(sink_fd >= 0);
		raw_rtp = new_raw_write_rtp(test_pool, sink_port, SWITCH_TRUE);
		fst_requires(raw_rtp);

		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1000, &out));
		fst_check(out == 1000);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1000 + 48000, &out));
		fst_check(out == 1320);

		fst_requires(switch_rtp_change_interval(raw_rtp, 20 * 1000, 160) == SWITCH_STATUS_SUCCESS);
		switch_rtp_clear_flag(raw_rtp, SWITCH_RTP_FLAG_PAUSE);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1640, &out));
		fst_check(out == 1640);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1800, &out));
		fst_check(out == 1800);

		switch_rtp_destroy(&raw_rtp);
		close(sink_fd);
		switch_core_destroy_memory_pool(&test_pool);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_raw_write_ts_rebase_keeps_rfc2833_event_timestamp)
	{
		switch_memory_pool_t *test_pool = NULL;
		switch_rtp_t *raw_rtp = NULL;
		switch_port_t sink_port = 0;
		uint32_t out = 0;
		int sink_fd, i;

		fst_requires(switch_core_new_memory_pool(&test_pool) == SWITCH_STATUS_SUCCESS);
		sink_fd = make_udp_sink(&sink_port);
		fst_requires(sink_fd >= 0);
		raw_rtp = new_raw_write_rtp(test_pool, sink_port, SWITCH_TRUE);
		fst_requires(raw_rtp);
		switch_rtp_set_telephony_event(raw_rtp, 101);

		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1000, &out));
		fst_check(out == 1000);
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1000 + 48000, &out));
		fst_check(out == 1320);

		for (i = 0; i < 3; i++) {
			fst_requires(raw_write_event_ts(raw_rtp, sink_fd, 1000 + 48320, &out));
			fst_check(out == 1640);
		}

		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1000 + 48640, &out));
		fst_check(out == 1960);

		switch_rtp_destroy(&raw_rtp);
		close(sink_fd);
		switch_core_destroy_memory_pool(&test_pool);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_raw_write_ts_normalised_timestamps_unchanged_by_rebase)
	{
		switch_memory_pool_t *test_pool = NULL;
		switch_rtp_t *raw_rtp = NULL;
		switch_port_t sink_port = 0;
		uint32_t src[] = { 1000, 1000 + 48000, 1000 + 48320, 1000 + 48320 + 2560, 1000 - 48000 };
		uint32_t out[2][5] = { { 0 } };
		int sink_fd, pass;
		size_t i;

		fst_requires(switch_core_new_memory_pool(&test_pool) == SWITCH_STATUS_SUCCESS);
		sink_fd = make_udp_sink(&sink_port);
		fst_requires(sink_fd >= 0);

		for (pass = 0; pass < 2; pass++) {
			raw_rtp = new_raw_write_rtp(test_pool, sink_port, pass ? SWITCH_TRUE : SWITCH_FALSE);
			fst_requires(raw_rtp);
			switch_rtp_intentional_bugs(raw_rtp, RTP_BUG_SEND_NORMALISED_TIMESTAMPS);
			for (i = 0; i < sizeof(src) / sizeof(src[0]); i++) {
				fst_requires(raw_write_ts(raw_rtp, sink_fd, src[i], &out[pass][i]));
			}
			switch_rtp_destroy(&raw_rtp);
		}

		for (i = 0; i < sizeof(src) / sizeof(src[0]); i++) {
			fst_check(out[0][i] == out[1][i]);
		}

		close(sink_fd);
		switch_core_destroy_memory_pool(&test_pool);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_raw_write_ts_rebase_over_srtp)
	{
		const char *why = NULL;

		if (!srtp_create || !srtp_unprotect || !srtp_dealloc || !srtp_crypto_policy_set_rtp_default) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "SKIPPED test_raw_write_ts_rebase_over_srtp: no SRTP in this build\n");
		} else {
			switch_bool_t ok = run_srtp_rebase_case(&why);

			fst_xcheck(ok, why ? why : "srtp rebase case");
		}
	}
	FST_TEST_END()
}
FST_SUITE_END()
}
FST_CORE_END()

