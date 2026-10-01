
#define SWITCH_RTP_TEST_HOOKS
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

static switch_bool_t recv_rtp_seq(int fd, uint16_t *seq, int timeout_ms)
{
	fd_set read_fds;
	struct timeval timeout;
	uint8_t packet[SWITCH_RTP_MAX_PACKET_LEN];
	ssize_t bytes;
	int ready;

	if (fd < 0 || !seq || timeout_ms < 0) return SWITCH_FALSE;

	FD_ZERO(&read_fds);
	FD_SET(fd, &read_fds);
	timeout.tv_sec = timeout_ms / 1000;
	timeout.tv_usec = (timeout_ms % 1000) * 1000;
	ready = select(fd + 1, &read_fds, NULL, NULL, &timeout);
	if (ready != 1) return SWITCH_FALSE;

	bytes = recvfrom(fd, packet, sizeof(packet), 0, NULL, NULL);
	if (bytes < SWITCH_RTP_HEADER_LEN) return SWITCH_FALSE;

	*seq = (uint16_t)(((uint16_t)packet[2] << 8) | packet[3]);
	return SWITCH_TRUE;
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

static void prepare_mid_write_packet(switch_rtp_packet_t *packet, switch_bool_t malformed, uint32_t timestamp)
{
	switch_rtp_hdr_ext_t *ext;

	memset(packet, 0, sizeof(*packet));
	packet->header.version = 2;
	packet->header.pt = TEST_PT;
	packet->header.ts = htonl(timestamp);
	packet->header.ssrc = htonl(0x11223344);

	if (malformed) {
		packet->header.x = 1;
		ext = (switch_rtp_hdr_ext_t *)packet->body;
		ext->profile = htons(0xBEDE);
		ext->length = htons(16);
		packet->ext = ext;
		packet->ebody = packet->body;
		packet->body[4] = (char)(TEST_MID_EXT_ID << 4);
		packet->body[5] = '0';
		packet->body[8] = (char)0xaa;
	} else {
		packet->body[0] = (char)0xaa;
	}
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
		const uint8_t malformed_length_mid[] = { 0xbe, 0xde, 0x00, 0x10, 0x40, 0x30, 0x00, 0x00, 0xaa, 0xbb };
		const uint8_t expected_repaired_mid[] = { 0xbe, 0xde, 0x00, 0x01, 0x10, 0x30, 0x00, 0x00, 0xaa, 0xbb };

		body = (uint8_t *) packet.body;

		memset(&packet, 0, sizeof(packet));
		packet.header.x = 1;
		packet.ebody = packet.body;
		packet.ext = (switch_rtp_hdr_ext_t *) packet.body;
		memcpy(packet.body, exact_peer_mid_leak, sizeof(exact_peer_mid_leak));
		bytes = SWITCH_RTP_HEADER_LEN + sizeof(exact_peer_mid_leak);
		fst_check(switch_rtp_test_rewrite_mid_extension(&packet, &bytes, 1, "0", SWITCH_TRUE, 0) == SWITCH_STATUS_SUCCESS);
		fst_check(bytes == SWITCH_RTP_HEADER_LEN + sizeof(expected_clean_mid));
		fst_check(!memcmp(body, expected_clean_mid, sizeof(expected_clean_mid)));

		memset(&packet, 0, sizeof(packet));
		packet.header.x = 1;
		packet.ebody = packet.body;
		packet.ext = (switch_rtp_hdr_ext_t *) packet.body;
		memcpy(packet.body, local_audio_level_plus_mid, sizeof(local_audio_level_plus_mid));
		bytes = SWITCH_RTP_HEADER_LEN + sizeof(local_audio_level_plus_mid);
		fst_check(switch_rtp_test_rewrite_mid_extension(&packet, &bytes, 4, "0", SWITCH_FALSE, 0) == SWITCH_STATUS_SUCCESS);
		fst_check(bytes == SWITCH_RTP_HEADER_LEN + sizeof(expected_preserved_audio_level));
		fst_check(!memcmp(body, expected_preserved_audio_level, sizeof(expected_preserved_audio_level)));

		memset(&packet, 0, sizeof(packet));
		packet.header.x = 1;
		packet.header.cc = 1;
		packet.ebody = packet.body + sizeof(uint32_t);
		packet.ext = (switch_rtp_hdr_ext_t *) packet.ebody;
		memcpy(packet.body, csrc_peer_mid_leak, sizeof(csrc_peer_mid_leak));
		bytes = SWITCH_RTP_HEADER_LEN + sizeof(csrc_peer_mid_leak);
		fst_check(switch_rtp_test_rewrite_mid_extension(&packet, &bytes, 1, "0", SWITCH_TRUE, 0) == SWITCH_STATUS_SUCCESS);
		fst_check(bytes == SWITCH_RTP_HEADER_LEN + sizeof(expected_csrc_clean_mid));
		fst_check(!memcmp(body, expected_csrc_clean_mid, sizeof(expected_csrc_clean_mid)));

		memset(&packet, 0, sizeof(packet));
		packet.header.x = 1;
		packet.ebody = packet.body;
		packet.ext = (switch_rtp_hdr_ext_t *) packet.body;
		memcpy(packet.body, malformed_length_mid, sizeof(malformed_length_mid));
		bytes = SWITCH_RTP_HEADER_LEN + sizeof(malformed_length_mid);
		fst_check(switch_rtp_test_rewrite_mid_extension(&packet, &bytes, 1, "0", SWITCH_TRUE, 0) == SWITCH_STATUS_FALSE);

		memset(&packet, 0, sizeof(packet));
		packet.header.x = 1;
		packet.ebody = packet.body;
		packet.ext = (switch_rtp_hdr_ext_t *) packet.body;
		memcpy(packet.body, malformed_length_mid, sizeof(malformed_length_mid));
		bytes = SWITCH_RTP_HEADER_LEN + sizeof(malformed_length_mid);
		fst_check(switch_rtp_test_rewrite_mid_extension(&packet, &bytes, 1, "0", SWITCH_TRUE,
			SWITCH_RTP_HEADER_LEN + 8) == SWITCH_STATUS_SUCCESS);
		fst_check(bytes == SWITCH_RTP_HEADER_LEN + sizeof(expected_repaired_mid));
		fst_check(!memcmp(body, expected_repaired_mid, sizeof(expected_repaired_mid)));
	}
	FST_TEST_END()
	FST_TEST_BEGIN(test_mid_rewrite_drop_is_not_transport_deferred)
	{
		switch_memory_pool_t *test_pool = NULL;
		switch_rtp_t *drop_rtp = NULL;
		switch_rtp_flag_t drop_flags[SWITCH_RTP_FLAG_INVALID] = { 0 };
		const char *drop_err = NULL;
		switch_port_t local_port = 0;
		switch_port_t sink_port = 0;
		switch_rtp_packet_t packet;
		switch_frame_t frame = { 0 };
		switch_rtp_stats_t *stats;
		uint8_t small_payload = 0xaa;
		uint8_t *large_payload = NULL;
		uint64_t packets_before_drop;
		uint16_t first_seq = 0;
		uint16_t second_seq = 0;
		int sink_fd = -1;
		int wrote;

		fst_requires(switch_core_new_memory_pool(&test_pool) == SWITCH_STATUS_SUCCESS);
		sink_fd = make_udp_sink(&sink_port);
		fst_requires(sink_fd >= 0);
		fst_requires(sink_port > 0);

		local_port = switch_rtp_request_port(rx_host);
		fst_requires(local_port > 0);
		drop_flags[SWITCH_RTP_FLAG_RAW_WRITE] = 1;
		drop_rtp = switch_rtp_new(rx_host, local_port, tx_host, sink_port, TEST_PT, 8000, 20 * 1000,
			drop_flags, "soft", &drop_err, test_pool);
		fst_requires(drop_rtp != NULL);
		fst_requires(switch_rtp_ready(drop_rtp));
		fst_requires(switch_rtp_enable_mid(drop_rtp, TEST_MID_EXT_ID, "0") == SWITCH_STATUS_SUCCESS);
		switch_rtp_clear_flag(drop_rtp, SWITCH_RTP_FLAG_PAUSE);

		/* Common forwarded path: an untrusted malformed extension is a packet
		 * failure, not the transport-not-ready result. Its sequence is rolled back. */
		prepare_mid_write_packet(&packet, SWITCH_FALSE, 9000);
		memset(&frame, 0, sizeof(frame));
		frame.packet = &packet;
		frame.packetlen = SWITCH_RTP_HEADER_LEN + 1;
		frame.data = packet.body;
		frame.datalen = 1;
		frame.payload = TEST_PT;
		frame.timestamp = 9000;
		frame.flags = SFF_RAW_RTP | SFF_EXTERNAL;
		wrote = switch_rtp_write_frame(drop_rtp, &frame);
		fst_requires(wrote > 0);
		fst_requires(recv_rtp_seq(sink_fd, &first_seq, 1000));

		prepare_mid_write_packet(&packet, SWITCH_TRUE, 9160);
		frame.packet = &packet;
		frame.packetlen = SWITCH_RTP_HEADER_LEN + 9;
		frame.data = NULL;
		frame.timestamp = 9160;
		wrote = switch_rtp_write_frame(drop_rtp, &frame);
		fst_check(wrote < 0);
		fst_check(!recv_rtp_seq(sink_fd, &second_seq, 100));

		prepare_mid_write_packet(&packet, SWITCH_FALSE, 9160);
		frame.packet = &packet;
		frame.packetlen = SWITCH_RTP_HEADER_LEN + 1;
		frame.data = packet.body;
		wrote = switch_rtp_write_frame(drop_rtp, &frame);
		fst_requires(wrote > 0);
		fst_requires(recv_rtp_seq(sink_fd, &second_seq, 1000));
		fst_check(second_seq == (uint16_t)(first_seq + 1));

		/* Proxy path has the same result and rollback contract. */
		prepare_mid_write_packet(&packet, SWITCH_FALSE, 9320);
		memset(&frame, 0, sizeof(frame));
		frame.packet = &packet;
		frame.packetlen = SWITCH_RTP_HEADER_LEN + 1;
		frame.data = packet.body;
		frame.datalen = 1;
		frame.payload = TEST_PT;
		frame.timestamp = 9320;
		frame.flags = SFF_PROXY_PACKET | SFF_EXTERNAL;
		wrote = switch_rtp_write_frame(drop_rtp, &frame);
		fst_requires(wrote > 0);
		fst_requires(recv_rtp_seq(sink_fd, &first_seq, 1000));

		prepare_mid_write_packet(&packet, SWITCH_TRUE, 9480);
		frame.packet = &packet;
		frame.packetlen = SWITCH_RTP_HEADER_LEN + 9;
		frame.data = NULL;
		frame.timestamp = 9480;
		wrote = switch_rtp_write_frame(drop_rtp, &frame);
		fst_check(wrote < 0);
		fst_check(!recv_rtp_seq(sink_fd, &second_seq, 100));

		prepare_mid_write_packet(&packet, SWITCH_FALSE, 9480);
		frame.packet = &packet;
		frame.packetlen = SWITCH_RTP_HEADER_LEN + 1;
		frame.data = packet.body;
		wrote = switch_rtp_write_frame(drop_rtp, &frame);
		fst_requires(wrote > 0);
		fst_requires(recv_rtp_seq(sink_fd, &second_seq, 1000));
		fst_check(second_seq == (uint16_t)(first_seq + 1));

		/* Manual path: a locally built packet that cannot fit the negotiated MID
		 * is dropped without being counted or consuming a sequence number. */
		memset(&frame, 0, sizeof(frame));
		frame.data = &small_payload;
		frame.datalen = 1;
		frame.payload = TEST_PT;
		frame.timestamp = 9640;
		frame.flags = SFF_RTP_HEADER;
		wrote = switch_rtp_write_frame(drop_rtp, &frame);
		fst_requires(wrote > 0);
		fst_requires(recv_rtp_seq(sink_fd, &first_seq, 1000));

		stats = switch_rtp_get_stats(drop_rtp, NULL);
		fst_requires(stats != NULL);
		packets_before_drop = stats->outbound.packet_count;
		large_payload = switch_core_alloc(test_pool, SWITCH_RTP_MAX_BUF_LEN - 4);
		memset(large_payload, 0xaa, SWITCH_RTP_MAX_BUF_LEN - 4);
		frame.data = large_payload;
		frame.datalen = SWITCH_RTP_MAX_BUF_LEN - 4;
		frame.timestamp = 9800;
		wrote = switch_rtp_write_frame(drop_rtp, &frame);
		fst_check(wrote < 0);
		fst_check(!recv_rtp_seq(sink_fd, &second_seq, 100));
		stats = switch_rtp_get_stats(drop_rtp, NULL);
		fst_requires(stats != NULL);
		fst_check(stats->outbound.packet_count == packets_before_drop);

		frame.data = &small_payload;
		frame.datalen = 1;
		frame.timestamp = 9800;
		wrote = switch_rtp_write_frame(drop_rtp, &frame);
		fst_requires(wrote > 0);
		fst_requires(recv_rtp_seq(sink_fd, &second_seq, 1000));
		fst_check(second_seq == (uint16_t)(first_seq + 1));

		switch_rtp_destroy(&drop_rtp);
		close(sink_fd);
		switch_core_destroy_memory_pool(&test_pool);
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

	/* a computed ts of 0 at the 32-bit wrap is still sent as a timestamp, not replaced by the local clock */
	FST_TEST_BEGIN(test_raw_write_audio_ts_wraps_through_zero)
	{
		/* generated after 0xffffff60, and a forwarded frame shifted onto 0 */
		uint32_t in[5] = { 0xfffffd80, 0, 0, 0, 0xfffffe20 };
		uint32_t gen[2] = { 0xffffff60, 0 };
		uint32_t out[5];
		uint8_t m[5];
		int i;

		fst_requires(write_raw_audio_ts(gen, NULL, 2, out, m));
		fst_check(out[1] - out[0] == 159 || out[1] - out[0] == 160);
		fst_check_int_equals(m[1], 0);

		fst_requires(write_raw_audio_ts(in, NULL, 5, out, m));
		for (i = 1; i < 5; i++) {
			fst_check(out[i] - out[i - 1] == 159 || out[i] - out[i - 1] == 160);
			fst_check_int_equals(m[i], 0);
		}
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_legacy_accept_any_payload_keeps_inbound_stats)
	{
		switch_memory_pool_t *test_pool = NULL;
		switch_rtp_t *legacy_rtp = NULL;
		switch_socket_t *send_sock = NULL;
		switch_sockaddr_t *send_bind_addr = NULL, *rtp_addr = NULL, *local_sa = NULL;
		switch_rtp_flag_t legacy_flags[SWITCH_RTP_FLAG_INVALID] = {0};
		payload_map_t negotiated_map = {0};
		payload_map_t *negotiated_maps = &negotiated_map;
		const char *legacy_err = NULL;
		switch_frame_t frame = { 0 };
		switch_rtp_stats_t *stats;
		switch_port_t local_port = 0;
		switch_port_t remote_port = 0;
		switch_size_t packet_len;
		switch_status_t status;
		uint8_t packet[] = {
			0x80, 96, 0x00, 0x01, 0x00, 0x00, 0x00, 0xa0, 0x11, 0x22, 0x33, 0x44,
			0x7f
		};

		fst_requires(switch_core_new_memory_pool(&test_pool) == SWITCH_STATUS_SUCCESS);
		local_port = switch_rtp_request_port(rx_host);
		fst_requires(local_port > 0);
		fst_requires(switch_sockaddr_info_get(&send_bind_addr, rx_host, SWITCH_UNSPEC, 0, 0, test_pool) == SWITCH_STATUS_SUCCESS);
		fst_requires(switch_socket_create(&send_sock, switch_sockaddr_get_family(send_bind_addr), SOCK_DGRAM, 0, test_pool) == SWITCH_STATUS_SUCCESS);
		fst_requires(switch_socket_bind(send_sock, send_bind_addr) == SWITCH_STATUS_SUCCESS);
		fst_requires(switch_socket_addr_get(&local_sa, SWITCH_FALSE, send_sock) == SWITCH_STATUS_SUCCESS);
		remote_port = switch_sockaddr_get_port(local_sa);
		fst_requires(remote_port > 0);

		legacy_rtp = switch_rtp_new(rx_host, local_port, tx_host, remote_port, TEST_PT, 8000, 20 * 1000,
			legacy_flags, "soft", &legacy_err, test_pool);
		fst_requires(legacy_rtp != NULL);
		fst_requires(switch_rtp_ready(legacy_rtp));
		negotiated_map.allocated = 1;
		negotiated_map.negotiated = 1;
		negotiated_map.pt = TEST_PT;
		negotiated_map.recv_pt = TEST_PT;
		fst_requires(switch_rtp_set_payload_map(legacy_rtp, &negotiated_maps) == SWITCH_STATUS_SUCCESS);
		switch_rtp_intentional_bugs(legacy_rtp, RTP_BUG_ACCEPT_ANY_PAYLOAD);
		switch_rtp_clear_flag(legacy_rtp, SWITCH_RTP_FLAG_PAUSE);
		fst_requires(switch_sockaddr_info_get(&rtp_addr, rx_host, SWITCH_UNSPEC, local_port, 0, test_pool) == SWITCH_STATUS_SUCCESS);

		packet_len = sizeof(packet);
		fst_requires(switch_socket_sendto(send_sock, rtp_addr, 0, (const char *)packet, &packet_len) == SWITCH_STATUS_SUCCESS);
		status = switch_rtp_zerocopy_read_frame(legacy_rtp, &frame, SWITCH_IO_FLAG_NONE);
		fst_requires(status == SWITCH_STATUS_SUCCESS);
		/* The first returned frame may be jitter-buffer PLC using the negotiated PT.
		 * The regression invariant is that legacy accept-any packets reach stats/JB. */
		stats = switch_rtp_get_stats(legacy_rtp, NULL);
		fst_requires(stats != NULL);
		fst_check(stats->inbound.packet_count == 1);
		fst_check(stats->inbound.media_packet_count == 1);

		if (send_sock) {
			switch_socket_close(send_sock);
		}
		switch_rtp_destroy(&legacy_rtp);
		switch_core_destroy_memory_pool(&test_pool);
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
