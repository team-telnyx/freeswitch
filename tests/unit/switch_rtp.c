
#define SWITCH_RTP_TEST_HOOKS
#include <switch.h>
#include <signal.h>
#include <stdlib.h>
#include <test/switch_test.h>
#include <unistd.h>
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
#define TEST_MID_EXT_ID 10
#define SOURCE_MID_EXT_ID 4
#define TEST_MID_TOGGLE_TIMEOUT_SEC 15
switch_rtp_packet_t rtp_packet;
switch_frame_flag_t *frame_flags;
switch_io_flag_t io_flags;
switch_payload_t read_pt;
int send_rtcp_test_success = 0;

static switch_memory_pool_t *media_pool = NULL;
static switch_rtp_t *media_rtp = NULL;
static switch_core_session_t *media_session = NULL;
static switch_pollfd_t *media_poll = NULL;
static payload_map_t media_map, *media_maps = &media_map;
static switch_port_t media_port;
static int media_fd = -1, media_fork_fd = -1;

static void media_cleanup(void)
{
	if (media_session) switch_core_session_set_fork_read_frame(media_session, NULL);
	if (media_rtp) {
		switch_rtp_fork_deactivate(media_rtp, FORK_DIRECTION_RX);
		switch_rtp_destroy(&media_rtp);
		media_rtp = NULL;
	}
	media_port = 0;
	if (media_fd >= 0) close(media_fd);
	if (media_fork_fd >= 0) close(media_fork_fd);
	media_fd = media_fork_fd = -1;
	media_poll = NULL;
	if (media_pool) switch_core_destroy_memory_pool(&media_pool);
	if (media_session) {
		switch_channel_hangup(switch_core_session_get_channel(media_session), SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(media_session);
		media_session = NULL;
	}
}

static switch_bool_t send_udp_packet(int fd, switch_port_t port, const uint8_t *packet, size_t packet_len)
{
	struct sockaddr_in addr;
	ssize_t sent;

	if (fd < 0 || !port || !packet || !packet_len) return SWITCH_FALSE;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(port);
	sent = sendto(fd, packet, packet_len, 0, (const struct sockaddr *)&addr, sizeof(addr));
	return sent == (ssize_t)packet_len ? SWITCH_TRUE : SWITCH_FALSE;
}

static switch_bool_t recv_rtp_packet(int fd, uint8_t *packet, size_t packet_size, size_t *packet_len, int timeout_ms)
{
	fd_set read_fds;
	struct timeval timeout;
	ssize_t bytes;
	int ready;

	if (fd < 0 || !packet || !packet_size || !packet_len || timeout_ms < 0) return SWITCH_FALSE;
	FD_ZERO(&read_fds);
	FD_SET(fd, &read_fds);
	timeout.tv_sec = timeout_ms / 1000;
	timeout.tv_usec = (timeout_ms % 1000) * 1000;
	ready = select(fd + 1, &read_fds, NULL, NULL, &timeout);
	if (ready != 1) return SWITCH_FALSE;

	bytes = recvfrom(fd, packet, packet_size, 0, NULL, NULL);
	if (bytes < SWITCH_RTP_HEADER_LEN) return SWITCH_FALSE;
	*packet_len = (size_t)bytes;
	return SWITCH_TRUE;
}

static void media_fork_packet(uint8_t *packet, uint16_t seq)
{
	uint32_t ts = htonl(16000 + (uint32_t)seq * 160);
	uint16_t net_seq = htons(seq);
	uint32_t ssrc = htonl(0x11223344);

	memset(packet, seq & 0xff, 172);
	packet[0] = 0x80;
	packet[1] = 0;
	memcpy(packet + 2, &net_seq, sizeof(net_seq));
	memcpy(packet + 4, &ts, sizeof(ts));
	memcpy(packet + 8, &ssrc, sizeof(ssrc));
}

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

static switch_bool_t media_open(int timed)
{
	switch_rtp_flag_t test_flags[SWITCH_RTP_FLAG_INVALID] = {0};
	switch_port_t peer_port;
	const char *error = NULL;

	if (switch_core_new_memory_pool(&media_pool) != SWITCH_STATUS_SUCCESS) return SWITCH_FALSE;
	if (media_session) switch_core_memory_pool_set_data(media_pool, "__session", media_session);
	media_fd = make_udp_sink(&peer_port);
	if (media_fd < 0 || !(media_port = switch_rtp_request_port(rx_host))) return SWITCH_FALSE;
	test_flags[SWITCH_RTP_FLAG_USE_TIMER] = timed;
	media_rtp = switch_rtp_new(rx_host, media_port, tx_host, peer_port, 0, 160, 20000,
		test_flags, timed ? "soft" : NULL, &error, media_pool);
	if (!media_rtp) return SWITCH_FALSE;
	switch_rtp_clear_flag(media_rtp, SWITCH_RTP_FLAG_PAUSE);
	switch_rtp_clear_flag(media_rtp, SWITCH_RTP_FLAG_FLUSH);
	memset(&media_map, 0, sizeof(media_map));
	media_map.allocated = media_map.negotiated = 1;
	return switch_rtp_set_payload_map(media_rtp, &media_maps) == SWITCH_STATUS_SUCCESS &&
		switch_socket_create_pollfd(&media_poll, switch_rtp_get_rtp_socket(media_rtp), SWITCH_POLLIN,
			NULL, media_pool) == SWITCH_STATUS_SUCCESS;
}

static switch_bool_t media_ready(void)
{
	int ready = 0;

	/* Wait for queued input without retrying the RTP operation under test. */
	return switch_poll(media_poll, 1, &ready, 1000000) == SWITCH_STATUS_SUCCESS;
}

static switch_status_t media_read(switch_frame_t *frame, switch_io_flag_t io_flags)
{
	memset(frame, 0, sizeof(*frame));
	return switch_rtp_zerocopy_read_frame(media_rtp, frame, io_flags);
}

static switch_bool_t recv_rtp_pt_ts(int fd, switch_payload_t pt, uint32_t *ts, uint8_t *m, int timeout_ms)
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
	if ((packet[1] & 0x7f) != pt) return recv_rtp_pt_ts(fd, pt, ts, m, timeout_ms);

	*ts = ((uint32_t)packet[4] << 24) | ((uint32_t)packet[5] << 16) | ((uint32_t)packet[6] << 8) | packet[7];
	if (m) *m = packet[1] >> 7;
	return SWITCH_TRUE;
}

static switch_bool_t recv_rtp_ts(int fd, uint32_t *ts, uint8_t *m, int timeout_ms)
{
	return recv_rtp_pt_ts(fd, TEST_PT, ts, m, timeout_ms);
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

	return recv_rtp_pt_ts(sink_fd, 101, out_ts, NULL, 1000);
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

	return recv_rtp_ts(sink_fd, out_ts, NULL, 1000);
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

	return recv_rtp_ts(sink_fd, out_ts, NULL, 1000);
}

static switch_bool_t recv_rtp_hdr(int fd, switch_payload_t pt, uint32_t *ts, uint16_t *seq, uint8_t *m, int timeout_ms)
{
	fd_set read_fds;
	struct timeval timeout;
	uint8_t packet[SWITCH_RTP_MAX_PACKET_LEN];
	ssize_t bytes;

	FD_ZERO(&read_fds);
	FD_SET(fd, &read_fds);
	timeout.tv_sec = timeout_ms / 1000;
	timeout.tv_usec = (timeout_ms % 1000) * 1000;
	if (select(fd + 1, &read_fds, NULL, NULL, &timeout) != 1) return SWITCH_FALSE;

	bytes = recvfrom(fd, packet, sizeof(packet), 0, NULL, NULL);
	if (bytes < SWITCH_RTP_HEADER_LEN) return SWITCH_FALSE;
	if ((packet[1] & 0x7f) != pt) return recv_rtp_hdr(fd, pt, ts, seq, m, timeout_ms);

	*m = (packet[1] & 0x80) ? 1 : 0;
	*seq = (uint16_t)(((uint16_t)packet[2] << 8) | packet[3]);
	*ts = ((uint32_t)packet[4] << 24) | ((uint32_t)packet[5] << 16) | ((uint32_t)packet[6] << 8) | packet[7];
	return SWITCH_TRUE;
}

static switch_bool_t payload_write_hdr(switch_rtp_t *raw_rtp, int sink_fd, uint32_t src_ts, switch_bool_t event,
									   uint32_t *out_ts, uint16_t *out_seq, uint8_t *out_m)
{
	uint8_t data[4] = { 0xaa, 0, 0, 0 };
	switch_frame_t frame = { 0 };

	if (event) {
		data[0] = 1;
		data[1] = 10;
	}
	frame.data = data;
	frame.datalen = event ? 4 : 1;
	frame.payload = TEST_PT;
	frame.timestamp = src_ts;
	if (event) {
		frame.flags = SFF_RFC2833;
	}

	if (switch_rtp_write_frame(raw_rtp, &frame) <= 0) return SWITCH_FALSE;

	return recv_rtp_hdr(sink_fd, event ? 101 : TEST_PT, out_ts, out_seq, out_m, 1000);
}

static switch_bool_t run_generated_after_rebase_case(const char **why)
{
	switch_memory_pool_t *test_pool = NULL;
	switch_rtp_t *raw_rtp = NULL;
	switch_port_t sink_port = 0;
	uint32_t out = 0, prev;
	int sink_fd = -1;
	switch_bool_t ok = SWITCH_FALSE;

	*why = "setup";
	if (switch_core_new_memory_pool(&test_pool) != SWITCH_STATUS_SUCCESS) goto end;
	if ((sink_fd = make_udp_sink(&sink_port)) < 0) goto end;
	if (!(raw_rtp = new_raw_write_rtp(test_pool, sink_port, SWITCH_TRUE))) goto end;

	*why = "first packet";
	if (!payload_write_ts(raw_rtp, sink_fd, 1000, &out) || out != 1000) goto end;
	*why = "jump not rebased";
	if (!payload_write_ts(raw_rtp, sink_fd, 1000 + 48000, &out) || out != 1320) goto end;
	prev = out;
	/* generated frame (no timestamp) right after the rebased jump */
	*why = "generated frame not sent after the last ts";
	if (!payload_write_ts(raw_rtp, sink_fd, 0, &out) || (int32_t) (out - prev) <= 0) goto end;
	prev = out;
	*why = "forwarded frame not after the generated one";
	if (!payload_write_ts(raw_rtp, sink_fd, 1000 + 48000 + 320, &out) || (int32_t) (out - prev) <= 0) goto end;

	ok = SWITCH_TRUE;
	*why = NULL;

 end:
	if (raw_rtp) switch_rtp_destroy(&raw_rtp);
	if (sink_fd >= 0) close(sink_fd);
	if (test_pool) switch_core_destroy_memory_pool(&test_pool);
	return ok;
}

static switch_bool_t run_mapped_zero_case(int which, const char **why)
{
	static const uint32_t jump_in[] = { 0xfffffec0, 100000, 100320, 100640 };
	static const uint32_t jump_out[] = { 0xfffffec0, 0, 320, 640 };
	static const uint32_t cross_in[] = { 0xfffffd80, 100000, 100320, 100640, 100960 };
	static const uint32_t cross_out[] = { 0xfffffd80, 0xfffffec0, 0, 320, 640 };
	static const uint32_t off_in[] = { 0xfffffec0, 100000, 100320 };
	const uint32_t *in = NULL, *expect = NULL;
	switch_memory_pool_t *test_pool = NULL;
	switch_rtp_t *raw_rtp = NULL;
	switch_port_t sink_port = 0;
	uint32_t out = 0;
	uint16_t seq = 0, prev_seq = 0;
	uint8_t m = 0;
	int sink_fd = -1, n = 0, i;
	switch_bool_t ok = SWITCH_FALSE;

	*why = "setup";
	if (switch_core_new_memory_pool(&test_pool) != SWITCH_STATUS_SUCCESS) goto end;
	if ((sink_fd = make_udp_sink(&sink_port)) < 0) goto end;
	if (!(raw_rtp = new_raw_write_rtp(test_pool, sink_port, which == 3 ? SWITCH_FALSE : SWITCH_TRUE))) goto end;
	switch_rtp_set_telephony_event(raw_rtp, 101);

	switch (which) {
	case 0:
		*why = "jump mapped to zero";
		in = jump_in;
		expect = jump_out;
		n = 4;
		break;
	case 1:
	case 2:
		*why = which == 1 ? "existing offset crossing zero" : "event mapped to zero";
		in = cross_in;
		expect = cross_out;
		n = which == 1 ? 5 : 2;
		break;
	case 3:
		*why = "rebase off";
		in = off_in;
		expect = off_in;
		n = 3;
		break;
	case 4:
		*why = "absent timestamp";
		in = cross_in;
		expect = cross_out;
		n = 2;
		break;
	}

	for (i = 0; i < n; i++) {
		if (!payload_write_hdr(raw_rtp, sink_fd, in[i], SWITCH_FALSE, &out, &seq, &m)) goto end;
		if (out != expect[i]) goto end;
		if (i && seq != (uint16_t)(prev_seq + 1)) goto end;
		if (which == 1 && i == 2 && m) goto end;
		prev_seq = seq;
	}

	if (which == 2) {
		for (i = 0; i < 3; i++) {
			if (!payload_write_hdr(raw_rtp, sink_fd, 100320, SWITCH_TRUE, &out, &seq, &m)) goto end;
			if (out != 0 || seq != (uint16_t)(prev_seq + 1)) goto end;
			prev_seq = seq;
		}
		if (!payload_write_hdr(raw_rtp, sink_fd, 100640, SWITCH_FALSE, &out, &seq, &m)) goto end;
		if (out != 320 || seq != (uint16_t)(prev_seq + 1)) goto end;
		prev_seq = seq;
		if (!payload_write_hdr(raw_rtp, sink_fd, 100960, SWITCH_FALSE, &out, &seq, &m)) goto end;
		if (out != 640 || seq != (uint16_t)(prev_seq + 1)) goto end;
	}

	if (which == 4) {
		if (!payload_write_hdr(raw_rtp, sink_fd, 0, SWITCH_FALSE, &out, &seq, &m)) goto end;
		/* no timestamp: a generated frame, on the forwarded clock after the last ts sent */
		if ((int32_t) (out - 0xfffffec0) <= 0 || (out - 0xfffffec0) % 320 || seq != (uint16_t)(prev_seq + 1)) goto end;
	}

	ok = SWITCH_TRUE;
	*why = NULL;

 end:
	if (raw_rtp) switch_rtp_destroy(&raw_rtp);
	if (sink_fd >= 0) close(sink_fd);
	if (test_pool) switch_core_destroy_memory_pool(&test_pool);
	return ok;
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

static switch_timer_interface_t arrival_timer_interface;
static switch_timer_interface_t *arrival_original_interface;
static switch_bool_t arrival_sent, arrival_empty;

static switch_status_t media_arrival_next(switch_timer_t *timer)
{
	uint8_t packet[172];
	int ready = 0;

	arrival_empty = switch_poll(media_poll, 1, &ready, 0) == SWITCH_STATUS_TIMEOUT;
	media_fork_packet(packet, 2);
	arrival_sent = send_udp_packet(media_fd, media_port, packet, sizeof(packet)) && media_ready();
	return arrival_original_interface->timer_next(timer);
}

static switch_bool_t media_arrival_case(int mode, int rejects)
{
	switch_call_cause_t cause;
	switch_port_t fork_port;
	switch_timer_t *timer;
	switch_frame_t frame;
	switch_status_t status;
	uint8_t packet[172], received[2048], processed[160];
	const uint8_t rejected[] = {0x80, 20, 0, 7, 0, 0, 0x7d, 0, 0x55, 0x66, 0x77, 0x88};
	size_t received_len;
	int i, j;
	switch_bool_t ok = SWITCH_FALSE;

	if (switch_ivr_originate(NULL, &media_session, &cause, "null/+15553334444", 2,
		NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS) goto done;
	switch_channel_set_variable(switch_core_session_get_channel(media_session), "read_codec", "PCMU");
	if (!media_open(1)) goto done;
	media_fork_fd = make_udp_sink(&fork_port);
	if (media_fork_fd < 0 || switch_rtp_fork_set(media_rtp, FORK_DIRECTION_RX, rx_host, fork_port,
		0x11223344, "start", "PCMU") != SWITCH_STATUS_SUCCESS ||
		switch_rtp_fork_activate(media_rtp, FORK_DIRECTION_RX) != SWITCH_STATUS_SUCCESS) goto done;
	if (mode == 1) switch_rtp_set_flag(media_rtp, SWITCH_RTP_FLAG_AUTOFLUSH);
	if (mode == 2) switch_rtp_set_flag(media_rtp, SWITCH_RTP_FLAG_STICKY_FLUSH);
	timer = switch_rtp_get_media_timer(media_rtp);
	if (!timer) goto done;
	arrival_original_interface = timer->timer_interface;
	arrival_timer_interface = *arrival_original_interface;
	arrival_timer_interface.timer_next = media_arrival_next;

	for (i = 1; i <= 4; i++) {
		if (i == 2) {
			for (j = 0; j < rejects; j++) {
				if (!send_udp_packet(media_fd, media_port, rejected, sizeof(rejected))) goto done;
			}
			if (rejects && !media_ready()) goto done;
			arrival_sent = SWITCH_FALSE;
			/* Inject after any pre-read, before the original timer returns. */
			timer->timer_interface = &arrival_timer_interface;
		} else {
			media_fork_packet(packet, (uint16_t)i);
			if (!send_udp_packet(media_fd, media_port, packet, sizeof(packet)) || !media_ready()) goto done;
		}
		status = media_read(&frame, SWITCH_IO_FLAG_NONE);
		timer->timer_interface = arrival_original_interface;
		if (i == 2 && (!arrival_sent || (mode && !arrival_empty))) goto done;
		if (status != SWITCH_STATUS_SUCCESS || switch_test_flag(&frame, SFF_CNG)) goto done;
		media_fork_packet(packet, (uint16_t)i);
		if (frame.seq != i || frame.payload != 0 || frame.timestamp != 16000 + i * 160 ||
			frame.datalen != 160 || memcmp(frame.data, packet + 12, 160)) goto done;
		memcpy(processed, frame.data, sizeof(processed));
		processed[0] ^= 0x5a;
		frame.packet = NULL;
		frame.data = processed;
		frame.buflen = sizeof(processed);
		if (switch_core_session_set_fork_read_frame(media_session, &frame) != SWITCH_STATUS_SUCCESS) goto done;
		if (i > 1) {
			if (!recv_rtp_packet(media_fork_fd, received, sizeof(received), &received_len, 1000)) goto done;
			media_fork_packet(packet, (uint16_t)(i - 1));
			packet[12] ^= 0x5a;
			if (received_len != sizeof(packet) || memcmp(received, packet, sizeof(packet))) goto done;
		}
	}
	ok = SWITCH_TRUE;
 done:
	media_cleanup();
	return ok;
}

static void mid_toggle_timeout_handler(int signal_number)
{
	static const char message[] = "switch_rtp: MID toggle stress test timed out\n";

	(void)signal_number;
	(void)write(STDERR_FILENO, message, sizeof(message) - 1);
	_exit(EXIT_FAILURE);
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


typedef struct received_mid_packet_s {
	switch_bool_t has_extension;
	switch_bool_t has_complete_mid;
	char mid[SWITCH_RTP_MID_MAX_LEN + 1];
	size_t mid_len;
	uint8_t payload;
} received_mid_packet_t;

static switch_bool_t recv_rtp_mid_packet(int fd, received_mid_packet_t *state, int timeout_ms)
{
	fd_set read_fds;
	struct timeval timeout;
	uint8_t packet[SWITCH_RTP_MAX_PACKET_LEN];
	size_t offset, ext_data_bytes, ext_offset;
	ssize_t bytes;
	int ready;

	if (fd < 0 || !state || timeout_ms < 0) return SWITCH_FALSE;
	memset(state, 0, sizeof(*state));

	FD_ZERO(&read_fds);
	FD_SET(fd, &read_fds);
	timeout.tv_sec = timeout_ms / 1000;
	timeout.tv_usec = (timeout_ms % 1000) * 1000;
	ready = select(fd + 1, &read_fds, NULL, NULL, &timeout);
	if (ready != 1) return SWITCH_FALSE;

	bytes = recvfrom(fd, packet, sizeof(packet), 0, NULL, NULL);
	if (bytes < SWITCH_RTP_HEADER_LEN) return SWITCH_FALSE;

	offset = SWITCH_RTP_HEADER_LEN + ((size_t)(packet[0] & 0x0f) * sizeof(uint32_t));
	if (offset > (size_t)bytes) return SWITCH_FALSE;
	state->has_extension = packet[0] & 0x10 ? SWITCH_TRUE : SWITCH_FALSE;
	if (state->has_extension) {
		if (offset + 4 > (size_t)bytes || packet[offset] != 0xbe || packet[offset + 1] != 0xde) return SWITCH_FALSE;
		ext_data_bytes = (size_t)(((uint16_t)packet[offset + 2] << 8) | packet[offset + 3]) * 4;
		offset += 4;
		if (offset + ext_data_bytes > (size_t)bytes) return SWITCH_FALSE;
		for (ext_offset = 0; ext_offset < ext_data_bytes;) {
			uint8_t element = packet[offset + ext_offset];
			uint8_t ext_id;
			size_t ext_len;

			if (!element) {
				ext_offset++;
				continue;
			}
			ext_id = (uint8_t)(element >> 4);
			if (ext_id == 15) break;
			ext_len = (size_t)(element & 0x0f) + 1;
			if (ext_offset + 1 + ext_len > ext_data_bytes) return SWITCH_FALSE;
			if (ext_id == TEST_MID_EXT_ID && ext_len <= SWITCH_RTP_MID_MAX_LEN) {
				memcpy(state->mid, packet + offset + ext_offset + 1, ext_len);
				state->mid_len = ext_len;
				state->has_complete_mid = SWITCH_TRUE;
			}
			ext_offset += 1 + ext_len;
		}
		offset += ext_data_bytes;
	}
	if (offset >= (size_t)bytes) return SWITCH_FALSE;
	state->payload = packet[offset];
	return SWITCH_TRUE;
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

static void prepare_peer_mid_write_packet(switch_rtp_packet_t *packet, uint32_t timestamp, uint8_t mid_ext_id)
{
	switch_rtp_hdr_ext_t *ext;

	memset(packet, 0, sizeof(*packet));
	packet->header.version = 2;
	packet->header.pt = TEST_PT;
	packet->header.ts = htonl(timestamp);
	packet->header.ssrc = htonl(0x11223344);
	packet->header.x = 1;
	ext = (switch_rtp_hdr_ext_t *)packet->body;
	ext->profile = htons(0xBEDE);
	ext->length = htons(1);
	packet->body[4] = (char)(mid_ext_id << 4);
	packet->body[5] = '1';
	packet->body[8] = (char)0xaa;
}

static void prepare_peer_audio_level_and_mid_write_packet(switch_rtp_packet_t *packet, uint32_t timestamp, uint8_t mid_ext_id)
{
	switch_rtp_hdr_ext_t *ext;

	memset(packet, 0, sizeof(*packet));
	packet->header.version = 2;
	packet->header.pt = TEST_PT;
	packet->header.ts = htonl(timestamp);
	packet->header.ssrc = htonl(0x11223344);
	packet->header.x = 1;
	ext = (switch_rtp_hdr_ext_t *)packet->body;
	ext->profile = htons(0xBEDE);
	ext->length = htons(2);
	packet->body[4] = 0x10;
	packet->body[5] = 0x55;
	packet->body[6] = (char)(mid_ext_id << 4);
	packet->body[7] = '1';
	packet->body[12] = (char)0xaa;
}

static void prepare_peer_audio_level_write_packet(switch_rtp_packet_t *packet, uint32_t timestamp)
{
	switch_rtp_hdr_ext_t *ext;

	memset(packet, 0, sizeof(*packet));
	packet->header.version = 2;
	packet->header.pt = TEST_PT;
	packet->header.ts = htonl(timestamp);
	packet->header.ssrc = htonl(0x11223344);
	packet->header.x = 1;
	ext = (switch_rtp_hdr_ext_t *)packet->body;
	ext->profile = htons(0xBEDE);
	ext->length = htons(1);
	packet->body[4] = 0;
	packet->body[5] = 0x10;
	packet->body[6] = 0x55;
	packet->body[8] = (char)0xaa;
}

typedef struct mid_toggle_context_s {
	switch_rtp_t *rtp_session;
	int iterations;
	volatile int failed;
} mid_toggle_context_t;

static void *SWITCH_THREAD_FUNC toggle_mid_state(switch_thread_t *thread, void *obj)
{
	mid_toggle_context_t *context = (mid_toggle_context_t *)obj;
	int i;

	(void)thread;
	for (i = 0; i < context->iterations; i++) {
		if (switch_rtp_configure_mid(context->rtp_session, 0, NULL, TEST_MID_EXT_ID) != SWITCH_STATUS_SUCCESS ||
			switch_rtp_test_set_received_mid(context->rtp_session, "video") != SWITCH_STATUS_SUCCESS) {
			context->failed = 1;
			break;
		}
		switch_yield(10);
		if (switch_rtp_configure_mid(context->rtp_session, TEST_MID_EXT_ID, "audio", TEST_MID_EXT_ID) != SWITCH_STATUS_SUCCESS) {
			context->failed = 1;
			break;
		}
		switch_yield(10);
		if (switch_rtp_disable_mid(context->rtp_session) != SWITCH_STATUS_SUCCESS) {
			context->failed = 1;
			break;
		}
		switch_yield(10);
	}

	return NULL;
}


/* Exercise both clocks through the shared transport, including generated audio
 * after video. Keep ownership here so a failed check still tears down the call. */
static switch_bool_t bundle_audio_write_state_case(const char **why, switch_bool_t rebase, switch_bool_t raw_video)
{
	switch_core_session_t *session = NULL;
	switch_memory_pool_t *test_pool = NULL;
	switch_rtp_t *shared_rtp = NULL;
	switch_rtp_write_state_t *video_state = NULL;
	switch_call_cause_t cause;
	switch_port_t sink_port;
	switch_frame_t video = { 0 };
	switch_rtp_packet_t video_packet = { 0 };
	uint8_t data = 0xaa;
	uint32_t audio_ts, report_ts, video_ts, first_video_ts = 0, count;
	switch_bool_t kept, ok = SWITCH_FALSE;
	int sink_fd = -1, i;

	*why = "session";
	if (switch_ivr_originate(NULL, &session, &cause, "null/+15553334444", 2,
		NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS) goto end;
	*why = "pool";
	if (switch_core_new_memory_pool(&test_pool) != SWITCH_STATUS_SUCCESS) goto end;
	switch_core_memory_pool_set_data(test_pool, "__session", session);
	*why = "sink";
	sink_fd = make_udp_sink(&sink_port);
	if (sink_fd < 0) goto end;
	*why = "shared RTP";
	shared_rtp = new_raw_write_rtp(test_pool, sink_port, rebase);
	if (!shared_rtp) goto end;
	switch_rtp_set_bundle_has_video(shared_rtp, SWITCH_TRUE);
	switch_rtp_set_bundle_audio_pt(shared_rtp, TEST_PT);
	if (switch_rtp_write_state_create(&video_state, test_pool) != SWITCH_STATUS_SUCCESS) goto end;

	for (i = 0; i < 3; i++) {
		*why = "audio timestamp and send report";
		if (!payload_write_ts(shared_rtp, sink_fd, rebase && i ? 1000000 + (i - 1) * 320 : 1000 + i * 320, &audio_ts) || audio_ts != 1000 + i * 320) goto end;
		count = switch_rtp_get_sent_frames(shared_rtp, &report_ts, &kept);
		if (count != (uint32_t)(i + 1) || report_ts != audio_ts || !kept) goto end;

		video.data = &data;
		video.datalen = video.buflen = 1;
		video.payload = 96;
		video.timestamp = 900000 + i * 3000;
		if (raw_video) {
			video_packet.header.version = 2;
			video_packet.header.pt = 96;
			video_packet.header.ts = htonl(video.timestamp);
			video_packet.header.ssrc = htonl(0x55667788);
			video_packet.body[0] = data;
			video.packet = &video_packet;
			video.packetlen = 13;
			video.data = video_packet.body;
			video.flags = SFF_RAW_RTP;
		}
		*why = "video write";
		if (switch_rtp_write_frame_ex_state(shared_rtp, &video, video_state,
			0x55667788, 0, NULL, SWITCH_TRUE, 96) <= 0) goto end;
		if (!recv_rtp_pt_ts(sink_fd, 96, &video_ts, NULL, 1000)) goto end;
		if (!i) first_video_ts = video_ts;
		*why = "independent video clock";
		if (video_ts != first_video_ts + i * 3000) goto end;
		*why = "video must not change the audio send report";
		if (switch_rtp_get_sent_frames(shared_rtp, &report_ts, &kept) != count || report_ts != audio_ts || !kept) goto end;
	}
	*why = "generated audio must not inherit the video clock";
	if (!payload_write_ts(shared_rtp, sink_fd, 0, &audio_ts) || audio_ts < 1960 || audio_ts >= 900000) goto end;
	if (switch_rtp_get_sent_frames(shared_rtp, &report_ts, &kept) != 4 || report_ts != audio_ts) goto end;
	ok = SWITCH_TRUE;
	*why = NULL;
 end:
	if (shared_rtp) switch_rtp_destroy(&shared_rtp);
	if (sink_fd >= 0) close(sink_fd);
	if (test_pool) switch_core_destroy_memory_pool(&test_pool);
	if (session) {
		switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
	}
	return ok;
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
	media_cleanup();
}
FST_TEARDOWN_END()
	FST_TEST_BEGIN(test_received_mid_clears_per_packet)
	{
		switch_memory_pool_t *test_pool = NULL;
		switch_rtp_t *mid_rtp = NULL;
		switch_rtp_flag_t mid_flags[SWITCH_RTP_FLAG_INVALID] = {0};
		const char *mid_err = NULL;
		char mid[SWITCH_RTP_MID_MAX_LEN + 1] = "";
		switch_frame_t frame = { 0 };
		switch_frame_flag_t read_flags = SFF_NONE;
		uint8_t read_buf[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
		uint32_t read_len;
		switch_payload_t malformed_pt = 0;
		switch_port_t local_port = 0;
		switch_port_t remote_port = 0;
		switch_status_t status;
		int read_attempts = 0;
		int send_fd = -1;
		switch_jb_t *mid_jb = NULL;
		switch_rtp_packet_t old_packet = { 0 }, new_packet = { 0 }, dequeued_packet = { 0 };
		switch_size_t old_packet_len = 0, new_packet_len = 0, dequeued_packet_len = 0;
		switch_size_t old_payload_offset = 0;
		uint8_t forwarded_packet[SWITCH_RTP_MAX_PACKET_LEN] = { 0 };
		size_t forwarded_len = 0;
		int wrote;
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
		uint8_t packet_mid_mapping_race[] = {
			0x90, TEST_PT, 0x00, 0x09, 0x00, 0x00, 0x00, 0x09, 0x11, 0x22, 0x33, 0x44,
			0xbe, 0xde, 0x00, 0x01, (TEST_MID_EXT_ID << 4) | 0x00, '1', 0x00, 0x00,
			0xef
		};
		uint8_t packet_after_mapping_change[] = {
			0x90, TEST_PT, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x0a, 0x11, 0x22, 0x33, 0x44,
			0xbe, 0xde, 0x00, 0x01, (SOURCE_MID_EXT_ID << 4) | 0x00, '2', 0x00, 0x00,
			0xf0
		};

		fst_requires(switch_core_new_memory_pool(&test_pool) == SWITCH_STATUS_SUCCESS);
		local_port = switch_rtp_request_port(rx_host);
		fst_requires(local_port > 0);

		send_fd = make_udp_sink(&remote_port);
		fst_requires(send_fd >= 0);

		mid_rtp = switch_rtp_new(rx_host, local_port, tx_host, remote_port, TEST_PT, 8000, 20 * 1000, mid_flags, "soft", &mid_err, test_pool);
		fst_requires(mid_rtp != NULL);
		fst_requires(switch_rtp_ready(mid_rtp));
		fst_check(switch_rtp_enable_mid_receive(mid_rtp, TEST_MID_EXT_ID) == SWITCH_STATUS_SUCCESS);
		fst_check(switch_rtp_copy_received_mid(mid_rtp, mid, sizeof(mid)) == SWITCH_STATUS_FALSE);
		switch_rtp_clear_flag(mid_rtp, SWITCH_RTP_FLAG_PAUSE);

		fst_requires(send_udp_packet(send_fd, local_port, packet_with_mid, sizeof(packet_with_mid)));
		do {
			memset(&frame, 0, sizeof(frame));
			status = switch_rtp_zerocopy_read_frame(mid_rtp, &frame, SWITCH_IO_FLAG_NONE);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			read_attempts++;
			if (switch_test_flag(&frame, SFF_CNG) && read_attempts < 3) {
				fst_requires(send_udp_packet(send_fd, local_port, packet_with_mid, sizeof(packet_with_mid)));
			}
		} while (switch_test_flag(&frame, SFF_CNG) && read_attempts < 3);
		fst_requires(!switch_test_flag(&frame, SFF_CNG));
		fst_check(frame.rtp_extensions.mid == TEST_MID_EXT_ID);
		fst_requires(switch_rtp_copy_received_mid(mid_rtp, mid, sizeof(mid)) == SWITCH_STATUS_SUCCESS);
		fst_check(!strcmp(mid, "1"));

		fst_requires(send_udp_packet(send_fd, local_port, packet_padding_before_mid, sizeof(packet_padding_before_mid)));
		status = switch_rtp_zerocopy_read_frame(mid_rtp, &frame, SWITCH_IO_FLAG_NONE);
		fst_requires(status == SWITCH_STATUS_SUCCESS);
		fst_requires(switch_rtp_copy_received_mid(mid_rtp, mid, sizeof(mid)) == SWITCH_STATUS_SUCCESS);
		fst_check(!strcmp(mid, "1"));

		fst_requires(send_udp_packet(send_fd, local_port, packet_malformed_mid_ext, sizeof(packet_malformed_mid_ext)));
		fst_requires(send_udp_packet(send_fd, local_port, packet_truncated_csrc_ext, sizeof(packet_truncated_csrc_ext)));
		fst_requires(send_udp_packet(send_fd, local_port, packet_short_ext_header, sizeof(packet_short_ext_header)));
		fst_requires(send_udp_packet(send_fd, local_port, packet_oversized_ext_block, sizeof(packet_oversized_ext_block)));
		fst_requires(send_udp_packet(send_fd, local_port, packet_malformed_padding, sizeof(packet_malformed_padding)));

		/* Non-zero id=0 nibble is malformed padding, but the media packet is still
		 * usable. The extension parser should stop parsing, clear remote MID, and
		 * keep the packet instead of dropping media. */
		memset(&frame, 0, sizeof(frame));
		status = switch_rtp_zerocopy_read_frame(mid_rtp, &frame, SWITCH_IO_FLAG_NONE);
		fst_requires(status == SWITCH_STATUS_SUCCESS);
		fst_check(switch_rtp_copy_received_mid(mid_rtp, mid, sizeof(mid)) == SWITCH_STATUS_FALSE);
		fst_check(frame.datalen == 1);
		fst_check(((uint8_t *) frame.data)[0] == 0xaa);

		fst_requires(send_udp_packet(send_fd, local_port, packet_reserved_ext_id, sizeof(packet_reserved_ext_id)));

		/* id=15 is reserved in the one-byte form. Treat it as end-of-parse, not
		 * as a packet-level failure. */
		memset(&frame, 0, sizeof(frame));
		status = switch_rtp_zerocopy_read_frame(mid_rtp, &frame, SWITCH_IO_FLAG_NONE);
		fst_requires(status == SWITCH_STATUS_SUCCESS);
		fst_check(switch_rtp_copy_received_mid(mid_rtp, mid, sizeof(mid)) == SWITCH_STATUS_FALSE);
		fst_check(frame.datalen == 1);
		fst_check(((uint8_t *) frame.data)[0] == 0xbb);

		fst_requires(send_udp_packet(send_fd, local_port, packet_without_mid, sizeof(packet_without_mid)));

		read_len = sizeof(read_buf);
		status = switch_rtp_read(mid_rtp, read_buf, &read_len, &malformed_pt, &read_flags, SWITCH_IO_FLAG_NONE);
		fst_requires(status == SWITCH_STATUS_SUCCESS);
		fst_check(switch_rtp_copy_received_mid(mid_rtp, mid, sizeof(mid)) == SWITCH_STATUS_FALSE);
		fst_check(read_len == 1);
		fst_check(read_buf[0] == 0xee);

		/* Queue packets from two negotiation generations and prove the older packet
		 * retains its source-leg MID id through jitter-buffer reordering. */
		fst_check(switch_rtp_configure_mid(mid_rtp, 0, NULL, TEST_MID_EXT_ID) == SWITCH_STATUS_SUCCESS);
		fst_requires(send_udp_packet(send_fd, local_port, packet_mid_mapping_race, sizeof(packet_mid_mapping_race)));
		memset(&frame, 0, sizeof(frame));
		status = switch_rtp_zerocopy_read_frame(mid_rtp, &frame, SWITCH_IO_FLAG_NONE);
		fst_requires(status == SWITCH_STATUS_SUCCESS);
		fst_check(frame.rtp_extensions.mid == TEST_MID_EXT_ID);
		fst_requires(frame.packet != NULL);
		old_packet = *(switch_rtp_packet_t *)frame.packet;
		old_packet_len = frame.packetlen;
		old_payload_offset = (switch_size_t)((uint8_t *)frame.data - (uint8_t *)frame.packet);
		fst_check(old_packet.mid_ext_id == TEST_MID_EXT_ID);

		fst_check(switch_rtp_configure_mid(mid_rtp, 0, NULL, SOURCE_MID_EXT_ID) == SWITCH_STATUS_SUCCESS);
		fst_requires(send_udp_packet(send_fd, local_port, packet_after_mapping_change, sizeof(packet_after_mapping_change)));
		memset(&frame, 0, sizeof(frame));
		status = switch_rtp_zerocopy_read_frame(mid_rtp, &frame, SWITCH_IO_FLAG_NONE);
		fst_requires(status == SWITCH_STATUS_SUCCESS);
		fst_requires(frame.packet != NULL);
		new_packet = *(switch_rtp_packet_t *)frame.packet;
		new_packet_len = frame.packetlen;
		fst_check(new_packet.mid_ext_id == SOURCE_MID_EXT_ID);

		fst_requires(switch_jb_create(&mid_jb, SJB_AUDIO, 1, 2, test_pool) == SWITCH_STATUS_SUCCESS);
		fst_requires(switch_jb_put_packet(mid_jb, &old_packet, old_packet_len) == SWITCH_STATUS_SUCCESS);
		fst_requires(switch_jb_put_packet(mid_jb, &new_packet, new_packet_len) == SWITCH_STATUS_SUCCESS);
		fst_requires(switch_jb_get_packet_by_seq(mid_jb, old_packet.header.seq,
			&dequeued_packet, &dequeued_packet_len) == SWITCH_STATUS_SUCCESS);
		fst_check(dequeued_packet.mid_ext_id == TEST_MID_EXT_ID);

		dequeued_packet.ext = NULL;
		dequeued_packet.ebody = NULL;
		memset(&frame, 0, sizeof(frame));
		frame.packet = &dequeued_packet;
		frame.packetlen = dequeued_packet_len;
		frame.data = (uint8_t *)&dequeued_packet + old_payload_offset;
		frame.datalen = 1;
		frame.payload = TEST_PT;
		frame.timestamp = 9;
		frame.flags = SFF_RAW_RTP | SFF_EXTERNAL;
		frame.rtp_extensions.mid = dequeued_packet.mid_ext_id;
		wrote = switch_rtp_write_frame(mid_rtp, &frame);
		fst_requires(wrote > 0);
		fst_requires(recv_rtp_packet(send_fd, forwarded_packet, sizeof(forwarded_packet), &forwarded_len, 1000));
		fst_check(forwarded_len == SWITCH_RTP_HEADER_LEN + 1);
		fst_check((forwarded_packet[0] & 0x10) == 0);
		fst_check(forwarded_packet[SWITCH_RTP_HEADER_LEN] == 0xef);

		fst_requires(switch_jb_get_packet_by_seq(mid_jb, new_packet.header.seq,
			&dequeued_packet, &dequeued_packet_len) == SWITCH_STATUS_SUCCESS);
		fst_check(dequeued_packet.mid_ext_id == SOURCE_MID_EXT_ID);
		switch_jb_destroy(&mid_jb);

		close(send_fd);
		switch_rtp_destroy(&mid_rtp);
		switch_core_destroy_memory_pool(&test_pool);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_disable_mid_stops_outbound_header_extension)
	{
		switch_memory_pool_t *test_pool = NULL;
		switch_core_session_t *test_session = NULL;
		switch_rtp_t *mid_rtp = NULL;
		switch_rtp_flag_t mid_flags[SWITCH_RTP_FLAG_INVALID] = { 0 };
		const char *mid_err = NULL;
		switch_call_cause_t cause = SWITCH_CAUSE_NONE;
		switch_port_t local_port = 0;
		switch_port_t sink_port = 0;
		switch_rtp_packet_t packet;
		switch_frame_t frame = { 0 };
		received_mid_packet_t received = { 0 };
		mid_toggle_context_t toggle_context = { 0 };
		switch_threadattr_t *thread_attr = NULL;
		switch_thread_t *toggle_thread = NULL;
		switch_status_t thread_status;
		int sink_fd = -1;
		int i;
		int wrote;
		int saw_stress_mid = 0;
		int saw_stress_without_mid = 0;
		int saw_received_mid = 0;
		char received_mid[SWITCH_RTP_MID_MAX_LEN + 1] = "";
		uint8_t forwarded_packet[SWITCH_RTP_MAX_PACKET_LEN];
		size_t forwarded_len = 0;
		const char *max_ascii_mid = "1234567890abcdef";
		const char *too_long_ascii_mid = "1234567890abcdefg";
		const char *max_utf8_mid = "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9";
		const char *too_long_utf8_mid = "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9";

		fst_requires(switch_ivr_originate(NULL, &test_session, &cause, "null/+15553334444", 2,
			NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) == SWITCH_STATUS_SUCCESS);
		fst_requires(test_session != NULL);
		test_pool = switch_core_session_get_pool(test_session);
		fst_requires(test_pool != NULL);
		sink_fd = make_udp_sink(&sink_port);
		fst_requires(sink_fd >= 0);
		local_port = switch_rtp_request_port(rx_host);
		fst_requires(local_port > 0);
		mid_flags[SWITCH_RTP_FLAG_RAW_WRITE] = 1;
		mid_rtp = switch_rtp_new(rx_host, local_port, tx_host, sink_port, TEST_PT, 8000, 20 * 1000,
			mid_flags, "soft", &mid_err, test_pool);
		fst_requires(mid_rtp != NULL);
		fst_requires(switch_rtp_ready(mid_rtp));
		switch_rtp_clear_flag(mid_rtp, SWITCH_RTP_FLAG_PAUSE);

		prepare_peer_mid_write_packet(&packet, 9000, SOURCE_MID_EXT_ID);
		frame.packet = &packet;
		frame.packetlen = SWITCH_RTP_HEADER_LEN + 9;
		frame.data = packet.body + 8;
		frame.datalen = 1;
		frame.payload = TEST_PT;
		frame.timestamp = 9000;
		frame.flags = SFF_RAW_RTP | SFF_EXTERNAL;
		frame.rtp_extensions.mid = SOURCE_MID_EXT_ID;

		/* The packet carries the source leg's MID id even though the destination leg did not negotiate MID. */
		wrote = switch_rtp_write_frame(mid_rtp, &frame);
		fst_requires(wrote > 0);
		fst_requires(recv_rtp_mid_packet(sink_fd, &received, 1000));
		fst_check(received.has_extension == SWITCH_FALSE);
		fst_check(received.payload == 0xaa);

		/* The destination receive id must not be used against source-leg extensions. */
		fst_requires(switch_rtp_enable_mid_receive(mid_rtp, 1) == SWITCH_STATUS_SUCCESS);
		prepare_peer_audio_level_and_mid_write_packet(&packet, 9040, SOURCE_MID_EXT_ID);
		frame.packetlen = SWITCH_RTP_HEADER_LEN + 13;
		frame.data = packet.body + 12;
		frame.timestamp = 9040;
		frame.flags = SFF_RAW_RTP | SFF_EXTERNAL;
		wrote = switch_rtp_write_frame(mid_rtp, &frame);
		fst_requires(wrote > 0);
		fst_requires(recv_rtp_packet(sink_fd, forwarded_packet, sizeof(forwarded_packet), &forwarded_len, 1000));
		fst_check(forwarded_len == SWITCH_RTP_HEADER_LEN + 9);
		fst_check((forwarded_packet[0] & 0x10) != 0);
		fst_check(forwarded_packet[SWITCH_RTP_HEADER_LEN + 4] == 0x10);
		fst_check(forwarded_packet[SWITCH_RTP_HEADER_LEN + 5] == 0x55);
		fst_check(forwarded_packet[SWITCH_RTP_HEADER_LEN + 8] == 0xaa);

		/* A BEDE block without the receive-side MID id is byte-preserved. */
		prepare_peer_audio_level_write_packet(&packet, 9060);
		frame.packetlen = SWITCH_RTP_HEADER_LEN + 9;
		frame.data = packet.body + 8;
		frame.timestamp = 9060;
		frame.flags = SFF_RAW_RTP | SFF_EXTERNAL;
		wrote = switch_rtp_write_frame(mid_rtp, &frame);
		fst_requires(wrote > 0);
		fst_requires(recv_rtp_packet(sink_fd, forwarded_packet, sizeof(forwarded_packet), &forwarded_len, 1000));
		fst_check(forwarded_len == SWITCH_RTP_HEADER_LEN + 9);
		fst_check(!memcmp(forwarded_packet + SWITCH_RTP_HEADER_LEN, packet.body, 9));

		/* Without an outbound MID, malformed extensions pass through rather than dropping media. */
		prepare_mid_write_packet(&packet, SWITCH_TRUE, 9080);
		frame.packetlen = SWITCH_RTP_HEADER_LEN + 9;
		frame.data = packet.body + 8;
		frame.timestamp = 9080;
		frame.flags = SFF_RAW_RTP | SFF_EXTERNAL;
		wrote = switch_rtp_write_frame(mid_rtp, &frame);
		fst_requires(wrote > 0);
		fst_requires(recv_rtp_packet(sink_fd, forwarded_packet, sizeof(forwarded_packet), &forwarded_len, 1000));
		fst_check(forwarded_len == SWITCH_RTP_HEADER_LEN + 9);
		fst_check((forwarded_packet[0] & 0x10) != 0);
		fst_check(!memcmp(forwarded_packet + SWITCH_RTP_HEADER_LEN, packet.body, 9));

		prepare_mid_write_packet(&packet, SWITCH_TRUE, 9120);
		frame.packetlen = SWITCH_RTP_HEADER_LEN + 9;
		frame.data = packet.body + 8;
		frame.timestamp = 9120;
		frame.flags = SFF_PROXY_PACKET | SFF_EXTERNAL;
		wrote = switch_rtp_write_frame(mid_rtp, &frame);
		fst_requires(wrote > 0);
		fst_requires(recv_rtp_packet(sink_fd, forwarded_packet, sizeof(forwarded_packet), &forwarded_len, 1000));
		fst_check(forwarded_len == SWITCH_RTP_HEADER_LEN + 9);
		fst_check((forwarded_packet[0] & 0x10) != 0);
		fst_check(!memcmp(forwarded_packet + SWITCH_RTP_HEADER_LEN, packet.body, 9));

		prepare_mid_write_packet(&packet, SWITCH_FALSE, 9160);
		frame.packetlen = SWITCH_RTP_HEADER_LEN + 1;
		frame.data = packet.body;
		frame.timestamp = 9160;
		frame.flags = SFF_RAW_RTP | SFF_EXTERNAL;
		fst_requires(switch_rtp_enable_mid(mid_rtp, TEST_MID_EXT_ID, "audio") == SWITCH_STATUS_SUCCESS);
		wrote = switch_rtp_write_frame(mid_rtp, &frame);
		fst_requires(wrote > 0);
		fst_requires(recv_rtp_mid_packet(sink_fd, &received, 1000));
		fst_check(received.has_extension == SWITCH_TRUE);
		fst_check(received.has_complete_mid == SWITCH_TRUE);
		fst_check(!strcmp(received.mid, "audio"));
		fst_check(received.payload == 0xaa);

		/* Audio-level insertion moves the trusted payload boundary before MID rewriting. */
		prepare_mid_write_packet(&packet, SWITCH_FALSE, 9200);
		forwarded_len = SWITCH_RTP_HEADER_LEN + 1;
		fst_check(switch_rtp_test_add_audio_level_and_rewrite_mid(mid_rtp, &packet, &forwarded_len,
			TEST_MID_EXT_ID, "audio", SWITCH_RTP_HEADER_LEN) == SWITCH_STATUS_SUCCESS);
		fst_check(forwarded_len == SWITCH_RTP_HEADER_LEN + 13);
		fst_check(packet.header.x == 1);
		fst_check((uint8_t)packet.body[0] == 0xbe);
		fst_check((uint8_t)packet.body[1] == 0xde);
		fst_check((uint8_t)packet.body[4] == (uint8_t)((TEST_MID_EXT_ID << 4) | 4));
		fst_check(!memcmp(packet.body + 5, "audio", 5));
		fst_check((uint8_t)packet.body[12] == 0xaa);

		fst_requires(switch_rtp_configure_mid(mid_rtp, TEST_MID_EXT_ID, max_ascii_mid, TEST_MID_EXT_ID) == SWITCH_STATUS_SUCCESS);
		prepare_mid_write_packet(&packet, SWITCH_FALSE, 9240);
		frame.packetlen = SWITCH_RTP_HEADER_LEN + 1;
		frame.data = packet.body;
		frame.timestamp = 9240;
		wrote = switch_rtp_write_frame(mid_rtp, &frame);
		fst_requires(wrote > 0);
		fst_requires(recv_rtp_mid_packet(sink_fd, &received, 1000));
		fst_check(received.has_complete_mid == SWITCH_TRUE);
		fst_check(received.mid_len == SWITCH_RTP_MID_MAX_LEN);
		fst_check(!strcmp(received.mid, max_ascii_mid));

		fst_requires(switch_rtp_disable_mid(mid_rtp) == SWITCH_STATUS_SUCCESS);
		fst_check(switch_rtp_configure_mid(mid_rtp, TEST_MID_EXT_ID, too_long_ascii_mid, TEST_MID_EXT_ID) == SWITCH_STATUS_FALSE);
		prepare_mid_write_packet(&packet, SWITCH_FALSE, 9280);
		frame.packetlen = SWITCH_RTP_HEADER_LEN + 1;
		frame.data = packet.body;
		frame.timestamp = 9280;
		wrote = switch_rtp_write_frame(mid_rtp, &frame);
		fst_requires(wrote > 0);
		fst_requires(recv_rtp_mid_packet(sink_fd, &received, 1000));
		fst_check(received.has_extension == SWITCH_FALSE);

		fst_requires(switch_rtp_configure_mid(mid_rtp, TEST_MID_EXT_ID, max_utf8_mid, TEST_MID_EXT_ID) == SWITCH_STATUS_SUCCESS);
		prepare_mid_write_packet(&packet, SWITCH_FALSE, 9300);
		frame.packetlen = SWITCH_RTP_HEADER_LEN + 1;
		frame.data = packet.body;
		frame.timestamp = 9300;
		wrote = switch_rtp_write_frame(mid_rtp, &frame);
		fst_requires(wrote > 0);
		fst_requires(recv_rtp_mid_packet(sink_fd, &received, 1000));
		fst_check(received.has_complete_mid == SWITCH_TRUE);
		fst_check(received.mid_len == SWITCH_RTP_MID_MAX_LEN);
		fst_check(!memcmp(received.mid, max_utf8_mid, SWITCH_RTP_MID_MAX_LEN));

		fst_requires(switch_rtp_disable_mid(mid_rtp) == SWITCH_STATUS_SUCCESS);
		fst_check(switch_rtp_configure_mid(mid_rtp, TEST_MID_EXT_ID, too_long_utf8_mid, TEST_MID_EXT_ID) == SWITCH_STATUS_FALSE);

		fst_requires(switch_rtp_disable_mid(mid_rtp) == SWITCH_STATUS_SUCCESS);
		prepare_mid_write_packet(&packet, SWITCH_FALSE, 9320);
		frame.packetlen = SWITCH_RTP_HEADER_LEN + 1;
		frame.data = packet.body;
		frame.timestamp = 9320;
		wrote = switch_rtp_write_frame(mid_rtp, &frame);
		fst_requires(wrote > 0);
		fst_requires(recv_rtp_mid_packet(sink_fd, &received, 1000));
		fst_check(received.has_extension == SWITCH_FALSE);
		fst_check(received.payload == 0xaa);

		/* A writer must observe either complete MID state or disabled state. */
		toggle_context.rtp_session = mid_rtp;
		toggle_context.iterations = 10000;
		fst_requires(switch_threadattr_create(&thread_attr, test_pool) == SWITCH_STATUS_SUCCESS);
		fst_requires(switch_thread_create(&toggle_thread, thread_attr, toggle_mid_state, &toggle_context, test_pool) == SWITCH_STATUS_SUCCESS);
		for (i = 0; i < 1000; i++) {
			prepare_mid_write_packet(&packet, SWITCH_FALSE, (uint32_t)(9480 + (i * 160)));
			frame.packetlen = SWITCH_RTP_HEADER_LEN + 1;
			frame.data = packet.body;
			frame.timestamp = (uint32_t)(9480 + (i * 160));
			wrote = switch_rtp_write_frame(mid_rtp, &frame);
			fst_requires(wrote > 0);
			fst_requires(recv_rtp_mid_packet(sink_fd, &received, 1000));
			fst_check(received.payload == 0xaa);
			if (received.has_extension) {
				fst_check(received.has_complete_mid == SWITCH_TRUE);
				saw_stress_mid = 1;
			} else {
				saw_stress_without_mid = 1;
			}
			if (switch_rtp_copy_received_mid(mid_rtp, received_mid, sizeof(received_mid)) == SWITCH_STATUS_SUCCESS) {
				fst_check(!strcmp(received_mid, "video"));
				saw_received_mid = 1;
			}
		}
		fst_requires(signal(SIGALRM, mid_toggle_timeout_handler) != SIG_ERR);
		alarm(TEST_MID_TOGGLE_TIMEOUT_SEC);
		switch_thread_join(&thread_status, toggle_thread);
		alarm(0);
		fst_check(toggle_context.failed == 0);
		fst_check(saw_stress_mid == 1);
		fst_check(saw_stress_without_mid == 1);
		fst_check(saw_received_mid == 1);

		close(sink_fd);
		switch_rtp_destroy(&mid_rtp);
		switch_channel_hangup(switch_core_session_get_channel(test_session), SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(test_session);
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
		const uint8_t short_length_mid[] = { 0xbe, 0xde, 0x00, 0x00, 0x40, 0x30, 0x00, 0x00, 0xaa, 0xbb };
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
		memcpy(packet.body, exact_peer_mid_leak, sizeof(exact_peer_mid_leak));
		bytes = SWITCH_RTP_HEADER_LEN + sizeof(exact_peer_mid_leak);
		fst_check(switch_rtp_test_strip_header_extensions(&packet, &bytes, 0) == SWITCH_STATUS_SUCCESS);
		fst_check(packet.header.x == 0);
		fst_check(bytes == SWITCH_RTP_HEADER_LEN + 2);
		fst_check((uint8_t)packet.body[0] == 0xaa);
		fst_check((uint8_t)packet.body[1] == 0xbb);

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

		memset(&packet, 0, sizeof(packet));
		packet.header.x = 1;
		memcpy(packet.body, short_length_mid, sizeof(short_length_mid));
		bytes = SWITCH_RTP_HEADER_LEN + sizeof(short_length_mid);
		fst_check(switch_rtp_test_rewrite_mid_extension(&packet, &bytes, 1, "0", SWITCH_TRUE,
			SWITCH_RTP_HEADER_LEN + 8) == SWITCH_STATUS_SUCCESS);
		fst_check(bytes == SWITCH_RTP_HEADER_LEN + sizeof(expected_repaired_mid));
		fst_check(!memcmp(body, expected_repaired_mid, sizeof(expected_repaired_mid)));

		memset(&packet, 0, sizeof(packet));
		packet.header.x = 1;
		memcpy(packet.body, malformed_length_mid, sizeof(malformed_length_mid));
		bytes = SWITCH_RTP_HEADER_LEN + sizeof(malformed_length_mid);
		fst_check(switch_rtp_test_strip_header_extensions(&packet, &bytes, 0) == SWITCH_STATUS_FALSE);
		fst_check(packet.header.x == 1);

		memset(&packet, 0, sizeof(packet));
		packet.header.x = 1;
		memcpy(packet.body, malformed_length_mid, sizeof(malformed_length_mid));
		bytes = SWITCH_RTP_HEADER_LEN + sizeof(malformed_length_mid);
		fst_check(switch_rtp_test_strip_header_extensions(&packet, &bytes, SWITCH_RTP_HEADER_LEN + 8) == SWITCH_STATUS_SUCCESS);
		fst_check(packet.header.x == 0);
		fst_check(bytes == SWITCH_RTP_HEADER_LEN + 2);
		fst_check((uint8_t)packet.body[0] == 0xaa);
		fst_check((uint8_t)packet.body[1] == 0xbb);

		memset(&packet, 0, sizeof(packet));
		packet.header.x = 1;
		memcpy(packet.body, short_length_mid, sizeof(short_length_mid));
		bytes = SWITCH_RTP_HEADER_LEN + sizeof(short_length_mid);
		fst_check(switch_rtp_test_strip_header_extensions(&packet, &bytes, SWITCH_RTP_HEADER_LEN + 8) == SWITCH_STATUS_SUCCESS);
		fst_check(packet.header.x == 0);
		fst_check(bytes == SWITCH_RTP_HEADER_LEN + 2);
		fst_check((uint8_t)packet.body[0] == 0xaa);
		fst_check((uint8_t)packet.body[1] == 0xbb);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_mid_rewrite_respects_caller_packet_buffer_size)
	{
		/* Exercise the real caller-owned packet capacity and guard its adjacent allocation. */
		struct pool_neighbour {
			uint8_t          packet_buf[SWITCH_RTP_MAX_BUF_LEN];
			switch_app_log_t node1;
			switch_app_log_t node2;
			uint8_t          guard[64];
		} *sim = malloc(sizeof(struct pool_neighbour));
		char app_name[] = "playback";
		char app_arg[] = "/tmp/greeting.wav";
		switch_rtp_hdr_t *hdr;
		switch_size_t bytes;
		int guard_ok = 1;
		size_t i;

		fst_requires(sim != NULL);

		memset(sim, 0, sizeof(struct pool_neighbour));
		memset(sim->guard, 0xDD, sizeof(sim->guard));

		sim->node1.app = app_name;
		sim->node1.arg = app_arg;
		sim->node1.next = &sim->node2;
		sim->node2.app = app_name;
		sim->node2.arg = app_arg;
		sim->node2.next = NULL;

		/* An ordinary outbound video packet: 12 byte header, no CSRC, no extension. */
		hdr = (switch_rtp_hdr_t *) sim->packet_buf;
		hdr->version = 2;
		hdr->pt = 96;
		memset(sim->packet_buf + SWITCH_RTP_HEADER_LEN, 0x41, 1200);
		bytes = SWITCH_RTP_HEADER_LEN + 1200;

		fst_check(switch_rtp_test_rewrite_mid_extension((switch_rtp_packet_t *) sim->packet_buf,
			&bytes, 1, "0", SWITCH_FALSE, 0) == SWITCH_STATUS_SUCCESS);

		/* The rewrite must stay inside the buffer it was given. */
		fst_check(bytes == SWITCH_RTP_HEADER_LEN + 1208);
		fst_check(sim->node1.next == &sim->node2);
		fst_check(sim->node1.app == app_name);
		fst_check(sim->node2.app == app_name);
		fst_check(sim->node2.arg == app_arg);

		for (i = 0; i < sizeof(sim->guard); i++) {
			if (sim->guard[i] != 0xDD) {
				guard_ok = 0;
			}
		}
		fst_check(guard_ok);

		/* A one-byte MID adds an eight-byte padded extension block. */
		memset(sim->packet_buf, 0, sizeof(sim->packet_buf));
		hdr = (switch_rtp_hdr_t *) sim->packet_buf;
		hdr->version = 2;
		hdr->pt = 96;
		memset(sim->packet_buf + SWITCH_RTP_HEADER_LEN, 0x42,
			SWITCH_RTP_MAX_BUF_LEN - SWITCH_RTP_HEADER_LEN - 8);
		bytes = SWITCH_RTP_MAX_BUF_LEN - 8;
		fst_check(switch_rtp_test_rewrite_mid_extension((switch_rtp_packet_t *) sim->packet_buf,
			&bytes, 1, "0", SWITCH_FALSE, 0) == SWITCH_STATUS_SUCCESS);
		fst_check(bytes == SWITCH_RTP_MAX_BUF_LEN);
		fst_check(sim->packet_buf[SWITCH_RTP_MAX_BUF_LEN - 1] == 0x42);
		fst_check(sim->node1.next == &sim->node2);
		fst_check(sim->node1.app == app_name);
		fst_check(sim->node2.app == app_name);
		fst_check(sim->node2.arg == app_arg);

		memset(sim->packet_buf, 0, sizeof(sim->packet_buf));
		hdr = (switch_rtp_hdr_t *) sim->packet_buf;
		hdr->version = 2;
		hdr->pt = 96;
		memset(sim->packet_buf + SWITCH_RTP_HEADER_LEN, 0x43,
			SWITCH_RTP_MAX_BUF_LEN - SWITCH_RTP_HEADER_LEN - 7);
		bytes = SWITCH_RTP_MAX_BUF_LEN - 7;
		fst_check(switch_rtp_test_rewrite_mid_extension((switch_rtp_packet_t *) sim->packet_buf,
			&bytes, 1, "0", SWITCH_FALSE, 0) == SWITCH_STATUS_FALSE);
		fst_check(bytes == SWITCH_RTP_MAX_BUF_LEN - 7);
		fst_check(sim->node1.next == &sim->node2);
		fst_check(sim->node1.app == app_name);
		fst_check(sim->node2.app == app_name);
		fst_check(sim->node2.arg == app_arg);

		free(sim);
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

	FST_TEST_BEGIN(test_bundle_and_bounded_read_flags_are_independent)
	{
		fst_check((SWITCH_IO_FLAG_BUNDLE_DRAIN & SWITCH_IO_FLAG_BOUNDED_READ) == 0);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_bundle_payload_video_preserves_rebased_audio)
	{
		const char *why = NULL;
		switch_bool_t ok = bundle_audio_write_state_case(&why, SWITCH_TRUE, SWITCH_FALSE);
		if (!ok) fprintf(stderr, "BUNDLE rebase: %s\n", why);
		fst_check(ok);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_bundle_raw_video_preserves_rebased_audio)
	{
		const char *why = NULL;
		switch_bool_t ok = bundle_audio_write_state_case(&why, SWITCH_TRUE, SWITCH_TRUE);
		if (!ok) fprintf(stderr, "BUNDLE rebase: %s\n", why);
		fst_check(ok);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_bundle_video_preserves_audio_write_state)
	{
		const char *why = NULL;
		switch_bool_t ok = bundle_audio_write_state_case(&why, SWITCH_FALSE, SWITCH_FALSE);

		fst_xcheck(ok, why ? why : "independent BUNDLE audio/video write state");
	}
	FST_TEST_END()

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

	FST_TEST_BEGIN(test_media_fork_rejected_preread)
	{
		int mode, rejects;
		char description[64];

		for (mode = 0; mode < 3; mode++) {
			for (rejects = 0; rejects <= 3; rejects += 3) {
				snprintf(description, sizeof(description), "arrival mode=%d rejects=%d", mode, rejects);
				fst_xcheck(media_arrival_case(mode, rejects), description);
			}
		}
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_media_fork_unnegotiated_triplets)
	{
		switch_call_cause_t cause;
		switch_port_t fork_port;
		switch_frame_t frame = {0};
		uint8_t packet[172], received[2048], processed[160];
		const uint8_t rejected[] = {0x80, 20, 0, 7, 0, 0, 0x7d, 0, 0x55, 0x66, 0x77, 0x88};
		size_t received_len;
		int i, j;

		fst_requires(switch_ivr_originate(NULL, &media_session, &cause, "null/+15553334444", 2,
			NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) == SWITCH_STATUS_SUCCESS);
		switch_channel_set_variable(switch_core_session_get_channel(media_session), "read_codec", "PCMU");
		fst_requires(media_open(1));
		media_fork_fd = make_udp_sink(&fork_port);
		fst_requires(media_fork_fd >= 0);
		fst_requires(switch_rtp_fork_set(media_rtp, FORK_DIRECTION_RX, rx_host, fork_port,
			0x11223344, "start", "PCMU") == SWITCH_STATUS_SUCCESS);
		fst_requires(switch_rtp_fork_activate(media_rtp, FORK_DIRECTION_RX) == SWITCH_STATUS_SUCCESS);

		for (i = 1; i <= 6; i++) {
			if (i == 2 || i == 4) {
				for (j = 0; j < 3; j++) fst_requires(send_udp_packet(media_fd, media_port, rejected, sizeof(rejected)));
			}
			media_fork_packet(packet, (uint16_t)i);
			fst_requires(send_udp_packet(media_fd, media_port, packet, sizeof(packet)));
		}
		for (i = 1; i <= 6; i++) {
			fst_requires(media_ready());
			fst_requires(media_read(&frame, SWITCH_IO_FLAG_NONE) == SWITCH_STATUS_SUCCESS);
			fst_requires(!switch_test_flag(&frame, SFF_CNG));
			media_fork_packet(packet, (uint16_t)i);
			fst_check(frame.seq == i && frame.payload == 0 && frame.timestamp == 16000 + i * 160);
			fst_requires(frame.datalen == 160);
			fst_check(memcmp(frame.data, packet + 12, 160) == 0);

			memcpy(processed, frame.data, sizeof(processed));
			processed[0] ^= 0x5a;
			frame.packet = NULL;
			frame.data = processed;
			frame.buflen = sizeof(processed);
			fst_requires(switch_core_session_set_fork_read_frame(media_session, &frame) == SWITCH_STATUS_SUCCESS);
			if (i > 1) {
				fst_requires(recv_rtp_packet(media_fork_fd, received, sizeof(received), &received_len, 1000));
				media_fork_packet(packet, (uint16_t)(i - 1));
				packet[12] ^= 0x5a;
				fst_check(received_len == sizeof(packet) && memcmp(received, packet, sizeof(packet)) == 0);
			}
		}
	}
	FST_TEST_END()
	FST_TEST_BEGIN(test_unnegotiated_payload_controls)
	{
		uint8_t packet[172];
		switch_frame_t frame = {0};
		int mode, i;

		for (mode = 0; mode < 2; mode++) {
			fst_requires(media_open(mode));
			media_fork_packet(packet, 1);
			packet[1] = 20;
			for (i = 0; i < 3; i++) fst_requires(send_udp_packet(media_fd, media_port, packet, 12));
			fst_requires(media_ready());
			fst_check(media_read(&frame, SWITCH_IO_FLAG_NOBLOCK) == SWITCH_STATUS_BREAK);
			/* Exhaust the discard budget, then read the remaining audio. */
			for (i = 0; i < 24; i++) fst_requires(send_udp_packet(media_fd, media_port, packet, 12));
			media_fork_packet(packet, 1);
			fst_requires(send_udp_packet(media_fd, media_port, packet, sizeof(packet)));
			fst_requires(media_ready());
			fst_check(media_read(&frame, SWITCH_IO_FLAG_NOBLOCK) == SWITCH_STATUS_BREAK);
			fst_requires(media_ready());
			fst_requires(media_read(&frame, SWITCH_IO_FLAG_NOBLOCK) == SWITCH_STATUS_SUCCESS);
			fst_check(!switch_test_flag(&frame, SFF_CNG) && frame.seq == 1 && frame.payload == 0);
			fst_requires(frame.datalen == 160);
			fst_check(memcmp(frame.data, packet + 12, 160) == 0);

			for (i = 0; i < 2; i++) {
				switch_payload_t cn_pt = i ? 98 : 13;
				uint32_t speech_ts = htonl(16000 + (uint32_t)(4 + i * 2) * 160);
				switch_rtp_set_cng_pt(media_rtp, cn_pt);
				media_fork_packet(packet, (uint16_t)(2 + i * 2));
				packet[1] = cn_pt;
				packet[12] = 0x41;
				packet[13] = 0;
				fst_requires(send_udp_packet(media_fd, media_port, packet, 14));
				media_fork_packet(packet, (uint16_t)(3 + i * 2));
				/* Skip the timestamp already emitted by the CN path. */
				memcpy(packet + 4, &speech_ts, sizeof(speech_ts));
				fst_requires(send_udp_packet(media_fd, media_port, packet, sizeof(packet)));
				fst_requires(media_ready());
				fst_requires(media_read(&frame, SWITCH_IO_FLAG_NOBLOCK) == SWITCH_STATUS_SUCCESS);
				fst_check(switch_test_flag(&frame, SFF_CNG) && frame.payload == cn_pt && frame.datalen == 2);
				fst_requires(media_ready());
				fst_requires(media_read(&frame, SWITCH_IO_FLAG_NOBLOCK) == SWITCH_STATUS_SUCCESS);
				fst_check(!switch_test_flag(&frame, SFF_CNG) && frame.payload == 0 && frame.seq == 3 + i * 2);
				fst_requires(frame.datalen == 160);
				fst_check(memcmp(frame.data, packet + 12, 160) == 0);
			}
			media_cleanup();
		}
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

	/* a 3-frame packet every 60 ms read out as 3 chunks: its timestamp, then two written back to back with none */
	FST_TEST_BEGIN(test_chunks_of_a_multi_frame_packet_get_consecutive_timestamps)
	{
		uint32_t in[9], out[9];
		uint8_t m[9];
		int delay[9], i;

		for (i = 0; i < 9; i++) {
			in[i] = (i % 3 == 0) ? 0x40000000 + (uint32_t) i * 160 : 0;
			delay[i] = (i && i % 3 == 0) ? 60 : 0;
		}
		fst_requires(write_raw_audio_ts(in, delay, 9, out, m));
		for (i = 0; i < 9; i++) {
			fst_check_int_equals(out[i], 0x40000000 + (uint32_t) i * 160);
		}
		for (i = 1; i < 9; i++) {
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

	FST_TEST_BEGIN(test_raw_write_ts_real_pause_with_jitter_is_kept)
	{
		switch_memory_pool_t *test_pool = NULL;
		switch_rtp_t *raw_rtp = NULL;
		switch_port_t sink_port = 0;
		uint32_t out = 0, last, src;
		switch_time_t sent_at, before_anchor;
		uint32_t step, gap;
		int sink_fd;

		fst_requires(switch_core_new_memory_pool(&test_pool) == SWITCH_STATUS_SUCCESS);
		sink_fd = make_udp_sink(&sink_port);
		fst_requires(sink_fd >= 0);
		raw_rtp = new_raw_write_rtp(test_pool, sink_port, SWITCH_TRUE);
		fst_requires(raw_rtp);

		/* The packet after the pause arrives 1 s late: the source gap is 1 s
		 * shorter than the elapsed time. */
		fst_requires(raw_write_ts(raw_rtp, sink_fd, 1000, &out));
		fst_check(out == 1000);
		sent_at = switch_time_ref();
		switch_yield(3500000);
		step = (uint32_t) ((switch_time_ref() - sent_at) * 16000 / 1000000);
		fst_requires(step > 48000);
		src = 1000 + step - 16000;
		fst_requires(raw_write_ts(raw_rtp, sink_fd, src, &out));
		fst_check(out == src);
		last = out;

		/* The packet before the pause arrived 150 ms late: the source gap is
		 * 150 ms longer than the elapsed time. */
		sent_at = switch_time_ref();
		switch_yield(2500000);
		step = (uint32_t) ((switch_time_ref() - sent_at) * 16000 / 1000000);
		fst_requires(step > 32000);
		src += step + 2400;
		before_anchor = switch_time_ref();
		fst_requires(raw_write_ts(raw_rtp, sink_fd, src, &out));
		fst_check(out == src);
		last = out;

		/* A source gap 10 s longer than the elapsed time is not a pause. A stall
		 * adds elapsed time, so the rejection is only asserted when the elapsed
		 * time, bounded from before the anchor write to after this write, is
		 * still too short to accept the gap; otherwise the case is inconclusive. */
		sent_at = switch_time_ref();
		switch_yield(2500000);
		step = (uint32_t) ((switch_time_ref() - sent_at) * 16000 / 1000000);
		fst_requires(step > 32000);
		gap = step + 160000;
		src += gap;
		fst_requires(raw_write_ts(raw_rtp, sink_fd, src, &out));
		if (gap > (uint32_t) ((switch_time_ref() - before_anchor) * 16000 / 1000000) + 3200) {
			fst_check(out == last + 320);
		}

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

	FST_TEST_BEGIN(test_raw_write_ts_generated_frame_after_rebase_moves_forward)
	{
		const char *why = NULL;
		switch_bool_t ok = run_generated_after_rebase_case(&why);

		fst_xcheck(ok, why ? why : "generated frame after rebase");
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_raw_write_ts_payload_frame_mapped_to_zero_is_kept)
	{
		const char *why = NULL;
		switch_bool_t ok;
		int i;

		for (i = 0; i < 5; i++) {
			why = NULL;
			ok = run_mapped_zero_case(i, &why);
			fst_xcheck(ok, why ? why : "mapped zero case");
		}
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
