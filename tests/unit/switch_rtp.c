
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
#define TEST_MID_EXT_ID 10
switch_rtp_packet_t rtp_packet;
switch_frame_flag_t *frame_flags;
switch_io_flag_t io_flags;
switch_payload_t read_pt;
int send_rtcp_test_success = 0;

SWITCH_DECLARE(switch_status_t) switch_rtp_test_rewrite_mid_extension(switch_rtp_packet_t *packet, switch_size_t *bytes, uint8_t ext_id, const char *mid, switch_bool_t drop_peer_extensions);

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


	FST_TEST_BEGIN(test_received_mid_clears_per_packet)
	{
		switch_memory_pool_t *test_pool = NULL;
		switch_rtp_t *mid_rtp = NULL;
		switch_socket_t *send_sock = NULL;
		switch_sockaddr_t *send_bind_addr = NULL, *rtp_addr = NULL, *local_sa = NULL;
		switch_rtp_flag_t mid_flags[SWITCH_RTP_FLAG_INVALID] = {0};
		const char *mid_err = NULL;
		const char *mid = NULL;
		switch_frame_t frame = { 0 };
		switch_frame_flag_t read_flags = SFF_NONE;
		uint8_t read_buf[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
		uint32_t read_len;
		switch_payload_t malformed_pt = 0;
		switch_port_t local_port = 0;
		switch_port_t remote_port = 0;
		switch_size_t packet_len;
		switch_status_t status;
		uint8_t packet_with_mid[] = {
			0x90, TEST_PT, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x11, 0x22, 0x33, 0x44,
			0xbe, 0xde, 0x00, 0x01, (TEST_MID_EXT_ID << 4) | 0x00, '1', 0x00, 0x00,
			0xff
		};
		uint8_t packet_padding_before_mid[] = {
			0x90, TEST_PT, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x11, 0x22, 0x33, 0x44,
			0xbe, 0xde, 0x00, 0x01, 0x00, (TEST_MID_EXT_ID << 4) | 0x00, '1', 0x00,
			0xcc
		};
		uint8_t packet_malformed_mid_ext[] = {
			0x90, TEST_PT, 0x00, 0x03, 0x00, 0x00, 0x00, 0x03, 0x11, 0x22, 0x33, 0x44,
			0xbe, 0xde, 0x00, 0x01, (TEST_MID_EXT_ID << 4) | 0x0f, 0x00, 0x00, 0x00,
			0xdd
		};
		uint8_t packet_truncated_csrc_ext[] = {
			0x91, TEST_PT, 0x00, 0x03, 0x00, 0x00, 0x00, 0x03, 0x11, 0x22, 0x33, 0x44,
			0xcc
		};
		uint8_t packet_short_ext_header[] = {
			0x90, TEST_PT, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x11, 0x22, 0x33, 0x44,
			0xbe
		};
		uint8_t packet_oversized_ext_block[] = {
			0x90, TEST_PT, 0x00, 0x05, 0x00, 0x00, 0x00, 0x05, 0x11, 0x22, 0x33, 0x44,
			0xbe, 0xde, 0x00, 0x02, (TEST_MID_EXT_ID << 4) | 0x00, '2', 0x00, 0x00
		};
		uint8_t packet_malformed_padding[] = {
			0x90, TEST_PT, 0x00, 0x06, 0x00, 0x00, 0x00, 0x06, 0x11, 0x22, 0x33, 0x44,
			0xbe, 0xde, 0x00, 0x01, 0x0f, 0x00, 0x00, 0x00,
			0xaa
		};
		uint8_t packet_reserved_ext_id[] = {
			0x90, TEST_PT, 0x00, 0x07, 0x00, 0x00, 0x00, 0x07, 0x11, 0x22, 0x33, 0x44,
			0xbe, 0xde, 0x00, 0x01, 0xf0, 'r', 0x00, 0x00,
			0xbb
		};
		uint8_t packet_without_mid[] = {
			0x80, TEST_PT, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08, 0x11, 0x22, 0x33, 0x44,
			0xee
		};

		fst_requires(switch_core_new_memory_pool(&test_pool) == SWITCH_STATUS_SUCCESS);
		local_port = switch_rtp_request_port(rx_host);
		fst_requires(local_port > 0);

		/* The unit-test conf exposes a single RTP port, so derive the sender port from an OS-assigned ephemeral bind rather than the shared allocator. */
		fst_requires(switch_sockaddr_info_get(&send_bind_addr, rx_host, SWITCH_UNSPEC, 0, 0, test_pool) == SWITCH_STATUS_SUCCESS);
		fst_requires(switch_socket_create(&send_sock, switch_sockaddr_get_family(send_bind_addr), SOCK_DGRAM, 0, test_pool) == SWITCH_STATUS_SUCCESS);
		fst_requires(switch_socket_bind(send_sock, send_bind_addr) == SWITCH_STATUS_SUCCESS);
		fst_requires(switch_socket_addr_get(&local_sa, SWITCH_FALSE, send_sock) == SWITCH_STATUS_SUCCESS);
		remote_port = switch_sockaddr_get_port(local_sa);
		fst_requires(remote_port > 0);

		mid_rtp = switch_rtp_new(rx_host, local_port, tx_host, remote_port, TEST_PT, 8000, 20 * 1000, mid_flags, "soft", &mid_err, test_pool);
		fst_requires(mid_rtp != NULL);
		fst_requires(switch_rtp_ready(mid_rtp));
		fst_check(switch_rtp_enable_mid_receive(mid_rtp, TEST_MID_EXT_ID) == SWITCH_STATUS_SUCCESS);
		fst_check(switch_rtp_get_received_mid(mid_rtp) == NULL);
		switch_rtp_clear_flag(mid_rtp, SWITCH_RTP_FLAG_PAUSE);

		fst_requires(switch_sockaddr_info_get(&rtp_addr, rx_host, SWITCH_UNSPEC, local_port, 0, test_pool) == SWITCH_STATUS_SUCCESS);

		packet_len = sizeof(packet_with_mid);
		fst_requires(switch_socket_sendto(send_sock, rtp_addr, 0, (const char *) packet_with_mid, &packet_len) == SWITCH_STATUS_SUCCESS);
		status = switch_rtp_zerocopy_read_frame(mid_rtp, &frame, SWITCH_IO_FLAG_NONE);
		fst_requires(status == SWITCH_STATUS_SUCCESS);
		mid = switch_rtp_get_received_mid(mid_rtp);
		fst_requires(mid != NULL);
		fst_check(!strcmp(mid, "1"));

		packet_len = sizeof(packet_padding_before_mid);
		fst_requires(switch_socket_sendto(send_sock, rtp_addr, 0, (const char *) packet_padding_before_mid, &packet_len) == SWITCH_STATUS_SUCCESS);
		status = switch_rtp_zerocopy_read_frame(mid_rtp, &frame, SWITCH_IO_FLAG_NONE);
		fst_requires(status == SWITCH_STATUS_SUCCESS);
		mid = switch_rtp_get_received_mid(mid_rtp);
		fst_requires(mid != NULL);
		fst_check(!strcmp(mid, "1"));

		packet_len = sizeof(packet_malformed_mid_ext);
		fst_requires(switch_socket_sendto(send_sock, rtp_addr, 0, (const char *) packet_malformed_mid_ext, &packet_len) == SWITCH_STATUS_SUCCESS);
		packet_len = sizeof(packet_truncated_csrc_ext);
		fst_requires(switch_socket_sendto(send_sock, rtp_addr, 0, (const char *) packet_truncated_csrc_ext, &packet_len) == SWITCH_STATUS_SUCCESS);
		packet_len = sizeof(packet_short_ext_header);
		fst_requires(switch_socket_sendto(send_sock, rtp_addr, 0, (const char *) packet_short_ext_header, &packet_len) == SWITCH_STATUS_SUCCESS);
		packet_len = sizeof(packet_oversized_ext_block);
		fst_requires(switch_socket_sendto(send_sock, rtp_addr, 0, (const char *) packet_oversized_ext_block, &packet_len) == SWITCH_STATUS_SUCCESS);
		packet_len = sizeof(packet_malformed_padding);
		fst_requires(switch_socket_sendto(send_sock, rtp_addr, 0, (const char *) packet_malformed_padding, &packet_len) == SWITCH_STATUS_SUCCESS);

		/* Non-zero id=0 nibble is malformed padding, but the media packet is still
		 * usable. The extension parser should stop parsing, clear remote MID, and
		 * keep the packet instead of dropping media. */
		memset(&frame, 0, sizeof(frame));
		status = switch_rtp_zerocopy_read_frame(mid_rtp, &frame, SWITCH_IO_FLAG_NONE);
		fst_requires(status == SWITCH_STATUS_SUCCESS);
		fst_check(switch_rtp_get_received_mid(mid_rtp) == NULL);
		fst_check(frame.datalen == 1);
		fst_check(((uint8_t *) frame.data)[0] == 0xaa);

		packet_len = sizeof(packet_reserved_ext_id);
		fst_requires(switch_socket_sendto(send_sock, rtp_addr, 0, (const char *) packet_reserved_ext_id, &packet_len) == SWITCH_STATUS_SUCCESS);

		/* id=15 is reserved in the one-byte form. Treat it as end-of-parse, not
		 * as a packet-level failure. */
		memset(&frame, 0, sizeof(frame));
		status = switch_rtp_zerocopy_read_frame(mid_rtp, &frame, SWITCH_IO_FLAG_NONE);
		fst_requires(status == SWITCH_STATUS_SUCCESS);
		fst_check(switch_rtp_get_received_mid(mid_rtp) == NULL);
		fst_check(frame.datalen == 1);
		fst_check(((uint8_t *) frame.data)[0] == 0xbb);

		packet_len = sizeof(packet_without_mid);
		fst_requires(switch_socket_sendto(send_sock, rtp_addr, 0, (const char *) packet_without_mid, &packet_len) == SWITCH_STATUS_SUCCESS);

		read_len = sizeof(read_buf);
		status = switch_rtp_read(mid_rtp, read_buf, &read_len, &malformed_pt, &read_flags, SWITCH_IO_FLAG_NONE);
		fst_requires(status == SWITCH_STATUS_SUCCESS);
		fst_check(switch_rtp_get_received_mid(mid_rtp) == NULL);
		fst_check(read_len == 1);
		fst_check(read_buf[0] == 0xee);

		if (send_sock) {
			switch_socket_close(send_sock);
		}
		switch_rtp_destroy(&mid_rtp);
		switch_core_destroy_memory_pool(&test_pool);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_mid_rewrite_drops_peer_extensions_but_preserves_local_extensions)
	{
		switch_rtp_packet_t packet = { 0 };
		switch_size_t bytes;
		uint8_t *body;
		const uint8_t exact_peer_mid_leak[] = { 0xbe, 0xde, 0x00, 0x01, 0x40, 0x30, 0x10, 0x30, 0xaa, 0xbb };
		const uint8_t expected_clean_mid[] = { 0xbe, 0xde, 0x00, 0x01, 0x10, 0x30, 0x00, 0x00, 0xaa, 0xbb };
		const uint8_t local_audio_level_plus_mid[] = { 0xbe, 0xde, 0x00, 0x01, 0x10, 0x55, 0x40, 0x39, 0xcc };
		const uint8_t expected_preserved_audio_level[] = { 0xbe, 0xde, 0x00, 0x01, 0x10, 0x55, 0x40, 0x30, 0xcc };
		const uint8_t csrc_peer_mid_leak[] = {
			0x11, 0x22, 0x33, 0x44,
			0xbe, 0xde, 0x00, 0x01, 0x40, 0x30, 0x10, 0x30, 0xdd
		};
		const uint8_t expected_csrc_clean_mid[] = {
			0x11, 0x22, 0x33, 0x44,
			0xbe, 0xde, 0x00, 0x01, 0x10, 0x30, 0x00, 0x00, 0xdd
		};

		body = (uint8_t *) packet.body;

		memset(&packet, 0, sizeof(packet));
		packet.header.x = 1;
		packet.ebody = packet.body;
		packet.ext = (switch_rtp_hdr_ext_t *) packet.body;
		memcpy(packet.body, exact_peer_mid_leak, sizeof(exact_peer_mid_leak));
		bytes = SWITCH_RTP_HEADER_LEN + sizeof(exact_peer_mid_leak);
		fst_check(switch_rtp_test_rewrite_mid_extension(&packet, &bytes, 1, "0", SWITCH_TRUE) == SWITCH_STATUS_SUCCESS);
		fst_check(bytes == SWITCH_RTP_HEADER_LEN + sizeof(expected_clean_mid));
		fst_check(!memcmp(body, expected_clean_mid, sizeof(expected_clean_mid)));

		memset(&packet, 0, sizeof(packet));
		packet.header.x = 1;
		packet.ebody = packet.body;
		packet.ext = (switch_rtp_hdr_ext_t *) packet.body;
		memcpy(packet.body, local_audio_level_plus_mid, sizeof(local_audio_level_plus_mid));
		bytes = SWITCH_RTP_HEADER_LEN + sizeof(local_audio_level_plus_mid);
		fst_check(switch_rtp_test_rewrite_mid_extension(&packet, &bytes, 4, "0", SWITCH_FALSE) == SWITCH_STATUS_SUCCESS);
		fst_check(bytes == SWITCH_RTP_HEADER_LEN + sizeof(expected_preserved_audio_level));
		fst_check(!memcmp(body, expected_preserved_audio_level, sizeof(expected_preserved_audio_level)));

		memset(&packet, 0, sizeof(packet));
		packet.header.x = 1;
		packet.header.cc = 1;
		packet.ebody = packet.body + sizeof(uint32_t);
		packet.ext = (switch_rtp_hdr_ext_t *) packet.ebody;
		memcpy(packet.body, csrc_peer_mid_leak, sizeof(csrc_peer_mid_leak));
		bytes = SWITCH_RTP_HEADER_LEN + sizeof(csrc_peer_mid_leak);
		fst_check(switch_rtp_test_rewrite_mid_extension(&packet, &bytes, 1, "0", SWITCH_TRUE) == SWITCH_STATUS_SUCCESS);
		fst_check(bytes == SWITCH_RTP_HEADER_LEN + sizeof(expected_csrc_clean_mid));
		fst_check(!memcmp(body, expected_csrc_clean_mid, sizeof(expected_csrc_clean_mid)));
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
		memcpy((char *)&servaddr_rtp.sin_addr.s_addr, (char *)server->h_addr, server->h_length);

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

}
FST_SUITE_END()
}
FST_CORE_END()

