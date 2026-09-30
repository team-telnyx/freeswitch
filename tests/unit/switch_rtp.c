
#include <switch.h>
#include <test/switch_test.h>

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

static switch_bool_t recv_rtp_ts(int fd, uint32_t *ts, uint8_t *m, int timeout_ms)
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

	*ts = ((uint32_t)packet[4] << 24) | ((uint32_t)packet[5] << 16) | ((uint32_t)packet[6] << 8) | packet[7];
	if (m) *m = packet[1] >> 7;
	return SWITCH_TRUE;
}

/* write 20 ms audio frames on a RAW_WRITE session (timestamp 0: generated locally), collect the sent ts and M;
 * delay_ms: optional pause before each frame */
static switch_bool_t write_raw_audio_ts(const uint32_t *in, const int *delay_ms, int n, uint32_t *out, uint8_t *m)
{
	switch_memory_pool_t *test_pool = NULL;
	switch_rtp_t *raw_rtp = NULL;
	switch_rtp_flag_t raw_flags[SWITCH_RTP_FLAG_INVALID] = { 0 };
	const char *raw_err = NULL;
	switch_port_t local_port, sink_port = 0;
	switch_frame_t frame = { 0 };
	uint8_t payload[160];
	switch_bool_t ok = SWITCH_TRUE;
	int sink_fd, i;

	if (switch_core_new_memory_pool(&test_pool) != SWITCH_STATUS_SUCCESS) return SWITCH_FALSE;
	sink_fd = make_udp_sink(&sink_port);
	local_port = switch_rtp_request_port(rx_host);
	/* default for audio legs unless rtp_rewrite_timestamps is set */
	raw_flags[SWITCH_RTP_FLAG_RAW_WRITE] = 1;
	if (sink_fd < 0 || !local_port ||
		!(raw_rtp = switch_rtp_new(rx_host, local_port, tx_host, sink_port, TEST_PT, 160, 20 * 1000, raw_flags, "soft", &raw_err, test_pool))) {
		ok = SWITCH_FALSE;
		goto end;
	}
	switch_rtp_clear_flag(raw_rtp, SWITCH_RTP_FLAG_PAUSE);
	memset(payload, 0xd5, sizeof(payload));

	for (i = 0; ok && i < n; i++) {
		if (delay_ms && delay_ms[i]) {
			switch_yield(delay_ms[i] * 1000);
		}
		memset(&frame, 0, sizeof(frame));
		frame.data = payload;
		frame.datalen = sizeof(payload);
		frame.samples = 160;
		frame.payload = TEST_PT;
		frame.timestamp = in[i];
		ok = switch_rtp_write_frame(raw_rtp, &frame) > 0 && recv_rtp_ts(sink_fd, &out[i], &m[i], 1000);
	}

end:
	if (raw_rtp) switch_rtp_destroy(&raw_rtp);
	if (sink_fd >= 0) close(sink_fd);
	switch_core_destroy_memory_pool(&test_pool);
	return ok;
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
	/* DTX: forwarded SIDs (far-end ts) with generated fill (ts 0) between them */
	FST_TEST_BEGIN(test_forwarded_and_generated_audio_frames_keep_rtp_timestamps_monotonic)
	{
		uint32_t in[24], out[24];
		uint8_t m[24];
		int i;

		for (i = 0; i < 24; i++) in[i] = (i % 8 == 0) ? 0x40000000 + (uint32_t) i * 160 : 0;
		fst_requires(write_raw_audio_ts(in, NULL, 24, out, m));
		for (i = 0; i < 24; i++) {
			fst_check_int_equals(out[i], 0x40000000 + (uint32_t) i * 160);
		}
		for (i = 1; i < 24; i++) {
			fst_check_int_equals(m[i], 0);
		}
	}
	FST_TEST_END()

	/* locally generated audio before forwarding (e.g. ringback) leaves forwarded timestamps as they are */
	FST_TEST_BEGIN(test_generated_audio_before_forwarding_keeps_forwarded_timestamps)
	{
		uint32_t in[9] = { 0, 0, 0, 0, 0, 0x40000000, 0x40000000 + 160, 0x40000000 + 1600, 0x40000000 + 1760 };
		uint32_t out[9];
		uint8_t m[9];
		int i;

		fst_requires(write_raw_audio_ts(in, NULL, 9, out, m));
		for (i = 5; i < 9; i++) {
			fst_check_int_equals(out[i], in[i]);
		}
	}
	FST_TEST_END()

	/* far end resumes before the generated fill caught up: forwarded ts shifted by a constant, spacing kept */
	FST_TEST_BEGIN(test_forwarded_audio_after_fill_is_shifted_not_rewritten)
	{
		uint32_t in[11] = { 0x40000000, 0, 0, 0, 0, 0, 0, 0, 0x40000000 + 480, 0x40000000 + 640, 0x40000000 + 2240 };
		uint32_t out[11];
		uint8_t m[11];
		int i;

		fst_requires(write_raw_audio_ts(in, NULL, 11, out, m));
		for (i = 1; i < 9; i++) {
			fst_check_int_equals(out[i], out[0] + (uint32_t) i * 160);
			fst_check_int_equals(m[i], 0);
		}
		fst_check_int_equals(out[9], out[8] + 160);
		fst_check_int_equals(out[10], out[8] + 1760);
	}
	FST_TEST_END()
	/* nothing sent for 480 ms, then generated frames: the gap shows as a forward jump with M=1 */
	FST_TEST_BEGIN(test_generated_audio_after_a_gap_jumps_forward)
	{
		uint32_t in[4] = { 0x40000000, 0, 0, 0x40000000 + 4000 };
		int delay_ms[4] = { 0, 480, 0, 0 };
		uint32_t out[4];
		uint8_t m[4];

		fst_requires(write_raw_audio_ts(in, delay_ms, 4, out, m));
		fst_check((out[1] - out[0]) % 160 == 0);
		fst_check(out[1] - out[0] >= 1600 && out[1] - out[0] <= 4800);
		fst_check_int_equals(m[1], 1);
		fst_check_int_equals(out[2], out[1] + 160);
		fst_check_int_equals(m[2], 0);
		fst_check((int32_t) (out[3] - out[2]) > 0);
	}
	FST_TEST_END()

	/* generated frames follow the elapsed time from the last forwarded frame, rounded down: no drift from jitter */
	FST_TEST_BEGIN(test_generated_audio_timestamps_do_not_drift_with_jitter)
	{
		uint32_t in[4] = { 0x40000000, 0, 0, 0 };
		int delay_ms[4] = { 0, 31, 9, 25 };
		uint32_t out[4];
		uint8_t m[4];
		int i;

		fst_requires(write_raw_audio_ts(in, delay_ms, 4, out, m));
		for (i = 1; i < 4; i++) {
			fst_check_int_equals(out[i] - out[0], (uint32_t) i * 160);
			fst_check_int_equals(m[i], 0);
		}
	}
	FST_TEST_END()

	/* a computed ts of 0 at the 32-bit wrap is sent as 0xffffffff and what follows is shifted by -1:
	 * no jump to the local clock, no marker, steps stay one interval */
	FST_TEST_BEGIN(test_raw_write_audio_ts_wraps_through_zero)
	{
		/* generated frame landing on 0, then forwarded and generated after it */
		uint32_t gen[4] = { 0xffffff60, 0, 160, 0 };
		/* forwarded frame shifted onto 0, then forwarded and generated after it */
		uint32_t in[7] = { 0xfffffd80, 0, 0, 0, 0xfffffe20, 0xfffffec0, 0 };
		uint32_t out[7];
		uint8_t m[7];
		int i;

		fst_requires(write_raw_audio_ts(gen, NULL, 4, out, m));
		fst_check_int_equals(out[1], 0xffffffff);
		for (i = 1; i < 4; i++) {
			fst_check_int_equals(out[i] - out[i - 1], i == 1 ? 159 : 160);
			fst_check_int_equals(m[i], 0);
		}

		fst_requires(write_raw_audio_ts(in, NULL, 7, out, m));
		fst_check_int_equals(out[4], 0xffffffff);
		for (i = 1; i < 7; i++) {
			fst_check_int_equals(out[i] - out[i - 1], i == 4 ? 159 : 160);
			fst_check_int_equals(m[i], 0);
		}
	}
	FST_TEST_END()


}
FST_SUITE_END()
}
FST_CORE_END()

