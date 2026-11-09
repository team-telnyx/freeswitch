/*
 * End to end: a range_star MSRP SEND whose body and CRLF fill the worker's
 * read buffer exactly, so the end-line arrives as the next read. The message
 * must complete, and a message sent after it must be delivered on its own.
 */

#include <switch.h>
#include <test/switch_test.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define MSRP_PORT 22856
/* The worker's read buffer, MSRP_BUFF_SIZE in switch_msrp.c */
#define WORKER_BUFF_SIZE (SWITCH_RTP_MAX_BUF_LEN - 32)

static int msrp_connect(void)
{
	struct sockaddr_in sa = { 0 };
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	sa.sin_family = AF_INET;
	sa.sin_port = htons(MSRP_PORT);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa))) {
		if (fd >= 0) close(fd);
		return -1;
	}

	return fd;
}

static switch_bool_t send_all(int fd, const char *data, size_t len)
{
	while (len) {
		ssize_t w = send(fd, data, len, 0);

		if (w <= 0) return SWITCH_FALSE;
		data += w;
		len -= (size_t)w;
	}

	return SWITCH_TRUE;
}

/* Read until the reply to the first SEND arrives (the worker has then bound
   the connection and will read the next bytes into an empty buffer). */
static switch_bool_t wait_for_ok(int fd)
{
	char buf[1024];
	size_t n = 0;
	struct timeval tv = { 5, 0 };

	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	while (n < sizeof(buf) - 1) {
		ssize_t r = recv(fd, buf + n, sizeof(buf) - 1 - n, 0);

		if (r <= 0) return SWITCH_FALSE;
		n += (size_t)r;
		buf[n] = '\0';
		if (strstr(buf, "200 OK")) return SWITCH_TRUE;
	}

	return SWITCH_FALSE;
}

FST_CORE_BEGIN("./conf_msrp")
{
FST_SUITE_BEGIN(msrp_range_star)
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

	FST_SESSION_BEGIN(end_line_after_a_full_buffer)
	{
		switch_channel_t *channel = switch_core_session_get_channel(fst_session);
		const char *uuid = switch_core_session_get_uuid(fst_session);
		switch_msrp_session_t *ms;
		switch_media_handle_t *smh = NULL;
		char *first, *headers, *big, *after;
		size_t hlen, blen;
		int fd, got_final = 0, got_after = 0, other = 0;
		switch_time_t deadline;

		if (!switch_core_session_get_media_handle(fst_session)) {
			switch_core_media_params_t *mparams = switch_core_session_alloc(fst_session, sizeof(*mparams));

			mparams->rtpip = mparams->rtpip4 = switch_core_session_strdup(fst_session, "127.0.0.1");
			fst_requires(switch_media_handle_create(&smh, fst_session, mparams) == SWITCH_STATUS_SUCCESS);
		}
		switch_channel_set_cap(channel, CC_MSRP);
		switch_channel_set_variable(channel, "sip_enable_msrp", "true");
		switch_core_media_gen_local_sdp(fst_session, SDP_OFFER, "127.0.0.1", 4000, NULL, 1);
		ms = switch_core_media_get_msrp_session(fst_session);
		fst_requires(ms);

		fd = msrp_connect();
		fst_requires(fd >= 0);

		/* Binds the connection to the session */
		first = switch_core_session_sprintf(fst_session,
			"MSRP t0 SEND\r\n"
			"To-Path: msrp://127.0.0.1:%d/%s;tcp\r\n"
			"From-Path: msrp://127.0.0.1:9/peer;tcp\r\n"
			"Message-ID: m0\r\n"
			"-------t0$\r\n", MSRP_PORT, uuid);
		fst_requires(send_all(fd, first, strlen(first)));
		fst_requires(wait_for_ok(fd));

		/* Headers, body and its CRLF fill the worker's buffer exactly */
		headers = switch_core_session_sprintf(fst_session,
			"MSRP t1 SEND\r\n"
			"To-Path: msrp://127.0.0.1:%d/%s;tcp\r\n"
			"From-Path: msrp://127.0.0.1:9/peer;tcp\r\n"
			"Message-ID: m1\r\n"
			"Byte-Range: 1-*/*\r\n"
			"Content-Type: text/plain\r\n"
			"\r\n", MSRP_PORT, uuid);
		hlen = strlen(headers);
		fst_requires(hlen + 2 < WORKER_BUFF_SIZE);
		blen = WORKER_BUFF_SIZE - hlen - 2;
		big = switch_core_session_alloc(fst_session, WORKER_BUFF_SIZE + 1);
		memcpy(big, headers, hlen);
		memset(big + hlen, 'a', blen);
		memcpy(big + hlen + blen, "\r\n", 2);
		fst_requires(send_all(fd, big, WORKER_BUFF_SIZE));
		switch_yield(300000);

		fst_requires(send_all(fd, "-------t1$\r\n", 12));
		switch_yield(300000);

		after = switch_core_session_sprintf(fst_session,
			"MSRP t2 SEND\r\n"
			"To-Path: msrp://127.0.0.1:%d/%s;tcp\r\n"
			"From-Path: msrp://127.0.0.1:9/peer;tcp\r\n"
			"Message-ID: m2\r\n"
			"Byte-Range: 1-5/5\r\n"
			"Content-Type: text/plain\r\n"
			"\r\n"
			"after\r\n"
			"-------t2$\r\n", MSRP_PORT, uuid);
		fst_requires(send_all(fd, after, strlen(after)));

		deadline = switch_micro_time_now() + 3000000;
		while (switch_micro_time_now() < deadline && !got_after) {
			switch_msrp_msg_t *msg = switch_msrp_session_pop_msg(ms);
			const char *id;

			if (!msg) continue;
			id = switch_msrp_msg_get_header(msg, MSRP_H_MESSAGE_ID);
			if (id && !strcmp(id, "m1") && msg->payload_bytes == 0) {
				got_final = 1;
			} else if (id && !strcmp(id, "m2")) {
				got_after = msg->payload_bytes == 5 && !memcmp(msg->payload, "after", 5);
			} else if (!id || strcmp(id, "m1")) {
				other++;
			}
			switch_msrp_msg_destroy(&msg);
		}

		fst_check(got_final);
		fst_check(got_after);
		fst_check(other == 0);

		close(fd);
		switch_yield(500000);
	}
	FST_SESSION_END()
}
FST_SUITE_END()
}
FST_CORE_END()
