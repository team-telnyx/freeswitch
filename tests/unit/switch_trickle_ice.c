#include <switch.h>
#include <switch_rtp.h>
#include <switch_stun.h>
#include <test/switch_test.h>
#include <private/switch_rtp_pvt.h>
#include "switch_telnyx.h"
#include <sofia-sip/sdp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define USE_SWITCH_RTP_NEW_IPPORT 1

extern char *fst_getenv_default(const char *, char *, switch_bool_t);
static void _silence_unused(void) { (void)fst_getenv_default; }
static const char *rx_host = "127.0.0.1";

static switch_size_t build_authenticated_ice_request_full(uint8_t *buf, switch_size_t buflen,
	const char *username, const char *password, switch_bool_t use_candidate,
	switch_bool_t peer_controlling, switch_bool_t add_fingerprint)
{
	static const char tie_breaker[8] = { 0x01, 0x23, 0x45, 0x67, 0x11, 0x22, 0x33, 0x44 };
	switch_stun_packet_t *packet;
	switch_size_t bytes;

	switch_assert(buf);
	switch_assert(buflen >= 128);
	switch_assert(username);
	switch_assert(password);
	memset(buf, 0, buflen);
	packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_REQUEST, NULL, buf);
	switch_stun_packet_attribute_add_priority(packet, 0x6e0001ff);
	switch_stun_packet_attribute_add_username(packet, (char *)username, (uint16_t)strlen(username));
	if (use_candidate) {
		switch_stun_packet_attribute_add_use_candidate(packet);
	}
	if (peer_controlling) {
		switch_stun_packet_attribute_add_controlling_value(packet, tie_breaker);
	} else {
		switch_stun_packet_attribute_add_controlled_value(packet, tie_breaker);
	}
	switch_stun_packet_attribute_add_integrity(packet, password);
	if (add_fingerprint) {
		switch_stun_packet_attribute_add_fingerprint(packet);
	}
	bytes = switch_stun_packet_length(packet);
	switch_assert(bytes <= buflen);

	return bytes;
}

static switch_size_t build_authenticated_ice_request_ex(uint8_t *buf, switch_size_t buflen,
	const char *username, const char *password, switch_bool_t use_candidate)
{
	return build_authenticated_ice_request_full(buf, buflen, username, password, use_candidate,
		SWITCH_TRUE, SWITCH_TRUE);
}

static switch_size_t build_authenticated_ice_request_role(uint8_t *buf, switch_size_t buflen,
	const char *username, const char *password, switch_bool_t use_candidate,
	switch_bool_t peer_controlling)
{
	return build_authenticated_ice_request_full(buf, buflen, username, password, use_candidate,
		peer_controlling, SWITCH_TRUE);
}

static switch_size_t build_authenticated_ice_request(uint8_t *buf, switch_size_t buflen,
	const char *username, const char *password)
{
	return build_authenticated_ice_request_ex(buf, buflen, username, password, SWITCH_TRUE);
}

static switch_size_t build_authenticated_ice_request_zero_length_role(uint8_t *buf,
	switch_size_t buflen, const char *username, const char *password)
{
	switch_stun_packet_t *packet;
	switch_stun_packet_attribute_t *attribute;
	switch_size_t bytes;

	switch_assert(buf);
	switch_assert(buflen >= 128);
	memset(buf, 0, buflen);
	packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_REQUEST, NULL, buf);
	switch_stun_packet_attribute_add_priority(packet, 0x6e0001ff);
	switch_stun_packet_attribute_add_username(packet, (char *)username, (uint16_t)strlen(username));
	switch_stun_packet_attribute_add_use_candidate(packet);
	attribute = (switch_stun_packet_attribute_t *)(buf + switch_stun_packet_length(packet));
	attribute->type = htons(SWITCH_STUN_ATTR_CONTROLLING);
	attribute->length = htons(0);
	packet->header.length += htons((uint16_t)sizeof(*attribute));
	switch_stun_packet_attribute_add_integrity(packet, password);
	switch_stun_packet_attribute_add_fingerprint(packet);
	bytes = switch_stun_packet_length(packet);
	switch_assert(bytes <= buflen);

	return bytes;
}

static switch_size_t build_unauthenticated_ice_request(uint8_t *buf, switch_size_t buflen,
	const char *username, switch_bool_t use_candidate)
{
	static const char tie_breaker[8] = { 0x01, 0x23, 0x45, 0x67, 0x11, 0x22, 0x33, 0x44 };
	switch_stun_packet_t *packet;

	memset(buf, 0, buflen);
	packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_REQUEST, NULL, buf);
	switch_stun_packet_attribute_add_priority(packet, 0x6e0001ff);
	switch_stun_packet_attribute_add_username(packet, (char *)username, (uint16_t)strlen(username));
	if (use_candidate) {
		switch_stun_packet_attribute_add_use_candidate(packet);
	}
	switch_stun_packet_attribute_add_controlling_value(packet, tie_breaker);
	switch_stun_packet_attribute_add_fingerprint(packet);
	return switch_stun_packet_length(packet);
}

static switch_size_t build_authenticated_ice_response(uint8_t *buf, switch_size_t buflen,
	const char transaction_id[13], const char *password, const char *host, switch_port_t port)
{
	switch_stun_packet_t *packet;
	switch_size_t bytes;

	switch_assert(buf);
	switch_assert(buflen >= 128);
	switch_assert(transaction_id);
	switch_assert(password);
	switch_assert(host);
	memset(buf, 0, buflen);
	packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_RESPONSE, (char *)transaction_id, buf);
	switch_stun_packet_attribute_add_xor_binded_address(packet, (char *)host, port, AF_INET);
	switch_stun_packet_attribute_add_integrity(packet, password);
	switch_stun_packet_attribute_add_fingerprint(packet);
	bytes = switch_stun_packet_length(packet);
	switch_assert(bytes <= buflen);

	return bytes;
}

static switch_size_t build_unauthenticated_ice_response(uint8_t *buf, switch_size_t buflen,
	const char transaction_id[13], const char *host, switch_port_t port)
{
	switch_stun_packet_t *packet;

	memset(buf, 0, buflen);
	packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_RESPONSE, (char *)transaction_id, buf);
	switch_stun_packet_attribute_add_xor_binded_address(packet, (char *)host, port, AF_INET);
	switch_stun_packet_attribute_add_fingerprint(packet);
	return switch_stun_packet_length(packet);
}

static switch_status_t reverse_ice_username(const char *ice_user, char *incoming, switch_size_t incoming_len)
{
	const char *colon;
	switch_size_t left_len;
	switch_size_t right_len;

	if (zstr(ice_user) || !incoming || !incoming_len || !(colon = strchr(ice_user, ':'))) {
		return SWITCH_STATUS_FALSE;
	}

	left_len = (switch_size_t)(colon - ice_user);
	right_len = strlen(colon + 1);
	if (!left_len || !right_len || left_len + right_len + 2 > incoming_len) {
		return SWITCH_STATUS_FALSE;
	}

	memcpy(incoming, colon + 1, right_len);
	incoming[right_len] = ':';
	memcpy(incoming + right_len + 1, ice_user, left_len);
	incoming[right_len + left_len + 1] = '\0';

	return SWITCH_STATUS_SUCCESS;
}

static int bind_udp_sink_fd(switch_port_t port)
{
	struct sockaddr_in addr;
	struct timeval timeout;
	int socket_fd;
	int reuse = 1;

	socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (socket_fd < 0) {
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	timeout.tv_sec = 0;
	timeout.tv_usec = 100000;
	setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	if (bind(socket_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(socket_fd);
		return -1;
	}
	return socket_fd;
}

static switch_status_t send_udp_packet_fd(int socket_fd, switch_port_t port,
	const void *data, switch_size_t len)
{
	struct sockaddr_in addr;
	ssize_t sent;

	if (socket_fd < 0 || !port || !data || !len) {
		return SWITCH_STATUS_FALSE;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sent = sendto(socket_fd, data, len, 0, (struct sockaddr *)&addr, sizeof(addr));

	return sent == (ssize_t)len ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

static switch_status_t recv_udp_flight_fd(int socket_fd, uint8_t *buf, switch_size_t capacity,
	switch_size_t *flight_len)
{
	ssize_t packet_len;
	switch_size_t total = 0;

	if (socket_fd < 0 || !buf || !capacity || !flight_len) {
		return SWITCH_STATUS_FALSE;
	}
	while (total < capacity) {
		packet_len = recv(socket_fd, buf + total, capacity - total, 0);
		if (packet_len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			break;
		}
		if (packet_len <= 0) {
			return SWITCH_STATUS_FALSE;
		}
		if (buf[total] < 20 || buf[total] > 23) {
			continue;
		}
		total += (switch_size_t)packet_len;
	}
	*flight_len = total;
	return total ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_TIMEOUT;
}

static switch_status_t copy_sockaddr_tuple(switch_sockaddr_t *addr, char *host, switch_size_t host_len,
	switch_port_t *port)
{
	if (!addr || !host || !host_len || !port) {
		return SWITCH_STATUS_FALSE;
	}

	if (!switch_get_addr(host, host_len, addr)) {
		return SWITCH_STATUS_FALSE;
	}
	*port = switch_sockaddr_get_port(addr);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t copy_sdp_attribute(const char *sdp, const char *attribute, char *value, switch_size_t value_len)
{
	char needle[64];
	const char *start;
	const char *end;
	switch_size_t len;

	if (zstr(sdp) || zstr(attribute) || !value || !value_len) {
		return SWITCH_STATUS_FALSE;
	}

	switch_snprintf(needle, sizeof(needle), "a=%s:", attribute);
	if (!(start = strstr(sdp, needle))) {
		return SWITCH_STATUS_FALSE;
	}

	start += strlen(needle);
	end = strpbrk(start, "\r\n");
	len = end ? (switch_size_t)(end - start) : strlen(start);
	if (!len || len >= value_len) {
		return SWITCH_STATUS_FALSE;
	}

	memcpy(value, start, len);
	value[len] = '\0';
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t copy_sdp_media_attribute(const char *sdp, const char *media, const char *attribute,
		char *value, switch_size_t value_len)
{
	char media_needle[64];
	char attr_needle[64];
	const char *section;
	const char *next_section;
	const char *start;
	const char *end;
	switch_size_t len;

	if (zstr(sdp) || zstr(media) || zstr(attribute) || !value || !value_len) {
		return SWITCH_STATUS_FALSE;
	}

	switch_snprintf(media_needle, sizeof(media_needle), "m=%s ", media);
	if (!(section = strstr(sdp, media_needle))) {
		return SWITCH_STATUS_FALSE;
	}

	next_section = strstr(section + strlen(media_needle), "\nm=");
	switch_snprintf(attr_needle, sizeof(attr_needle), "a=%s:", attribute);
	if (!(start = strstr(section, attr_needle)) || (next_section && start > next_section)) {
		return SWITCH_STATUS_FALSE;
	}

	start += strlen(attr_needle);
	end = strpbrk(start, "\r\n");
	len = end ? (switch_size_t)(end - start) : strlen(start);
	if (!len || len >= value_len) {
		return SWITCH_STATUS_FALSE;
	}

	memcpy(value, start, len);
	value[len] = '\0';
	return SWITCH_STATUS_SUCCESS;
}

typedef struct trickle_captured_s {
	int called;
	int last_mline;
	int last_eoc;
	char last_mid[64];
	switch_rtp_ice_cand_t last_cand;
} trickle_captured_t;

static void on_local_candidate_cb(void *user_data,
                                  const char *mid,
                                  int mline_index,
                                  const switch_rtp_ice_cand_t *cand,
                                  int end_of_candidates)
{
	trickle_captured_t *cap = (trickle_captured_t *)user_data;

	cap->called++;
	cap->last_mline = mline_index;
	cap->last_eoc   = end_of_candidates;
	switch_snprintf(cap->last_mid, sizeof(cap->last_mid), "%s", mid ? mid : "(null)");

	memset(&cap->last_cand, 0, sizeof(cap->last_cand));
	if (cand) {
		cap->last_cand.component_id = cand->component_id;
		switch_snprintf(cap->last_cand.ip, sizeof(cap->last_cand.ip), "%s", cand->ip);
		switch_snprintf(cap->last_cand.transport, sizeof(cap->last_cand.transport), "%s", cand->transport);
		cap->last_cand.port = cand->port;
		cap->last_cand.priority = cand->priority;
	}
}

/* A second callback to prove overwrite semantics */
static void on_local_candidate_cb_2(void *user_data,
                                    const char *mid,
                                    int mline_index,
                                    const switch_rtp_ice_cand_t *cand,
                                    int end_of_candidates)
{
	trickle_captured_t *cap = (trickle_captured_t *)user_data;
	/* Mark different values so we can tell which handler ran */
	cap->called += 10;
	cap->last_mline = mline_index + 100;
	cap->last_eoc   = end_of_candidates ? 99 : 98;
	switch_snprintf(cap->last_mid, sizeof(cap->last_mid), "cb2:%s", mid ? mid : "(null)");

	memset(&cap->last_cand, 0, sizeof(cap->last_cand));
	if (cand) {
		cap->last_cand.component_id = cand->component_id + 100;
		switch_snprintf(cap->last_cand.ip, sizeof(cap->last_cand.ip), "cb2-%s", cand->ip[0] ? cand->ip : "none");
		switch_snprintf(cap->last_cand.transport, sizeof(cap->last_cand.transport), "cb2-%s", cand->transport[0] ? cand->transport : "none");
		cap->last_cand.port = cand->port + 100;
		cap->last_cand.priority = cand->priority + 100;
	}
}

static switch_status_t make_real_rtp(switch_memory_pool_t *pool,
                                     switch_rtp_t **out_rtp,
                                     const char **out_err)
{

	switch_rtp_t *rtp;
	switch_payload_t payload = 0;        /* payload doesn't matter here */
	uint32_t spi = 160;                  /* 20ms @ 8kHz */
	uint32_t mpp = 20;                   /* 20ms packets */
	switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID] = { 0 };
	const char *dummy_err = NULL;
	const char **errp = out_err ? out_err : &dummy_err;

	rtp = switch_rtp_new("127.0.0.1", /* rx_host */
			1234,           /* rx_port => ephemeral */
			"127.0.0.1",        /* tx_host */
			5432,           /* tx_port */
			payload,
			spi,
			mpp,
			flags,      
			NULL,
			errp,
			pool);
	
	if (rtp) { *out_rtp = rtp; return SWITCH_STATUS_SUCCESS; }
	return SWITCH_STATUS_FALSE;
}


/* Declare the production trickle ICE function */
extern switch_status_t switch_core_media_trickle_remote_candidate_and_recheck(switch_core_session_t *session, switch_media_handle_t *smh, void *sdp_session, switch_sdp_type_t sdp_type, const char *mid, int mline_index, const char *cand_line, int end_of_candidates);

static void cleanup_rtp(switch_rtp_t **rtp)
{
	if (rtp && *rtp) {
		switch_rtp_destroy(rtp);
	}
}

static void cleanup_session_and_media(switch_core_session_t *session)
{
	switch_media_handle_t *smh = NULL;

	if (!session) return;

	switch_channel_clear_flag(switch_core_session_get_channel(session), CF_VIDEO_PASSIVE);
	switch_core_session_wake_video_thread(session);
	switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
	smh = switch_core_session_get_media_handle(session);
	if (smh) {
		switch_core_media_deactivate_rtp(session);
	}

	switch_core_session_rwunlock(session);
}

static void cleanup_session_media_and_sdp(switch_core_session_t *session, void *sdp_session, sdp_parser_t *parser)
{
	/* Clean up SDP parser if allocated */
	if (parser) {
		sdp_parser_free(parser);
	}
	cleanup_session_and_media(session);
}

static switch_status_t make_session_and_rtp_with_sdp_ex(switch_core_session_t **out_session,
                                                         switch_rtp_t **out_rtp,
                                                         void **out_sdp_session,
                                                         sdp_parser_t **out_parser,
                                                         const char *offer_sdp,
                                                         const char *codec_string,
                                                         switch_bool_t use_bundle,
                                                         switch_bool_t activate_rtp,
                                                         const char *ice_role)
{
	switch_status_t st;
	switch_call_cause_t cause = SWITCH_CAUSE_NONE, cancel = SWITCH_CAUSE_NONE;
	switch_core_session_t *session = NULL;
	switch_media_handle_t *media_handle;
	switch_core_media_params_t *mparams;
	switch_status_t status;
	switch_channel_t *chan = NULL;
	char *r_sdp;
	uint8_t match = 0, p = 0;
	const char *selected_codec = zstr(codec_string) ? "PCMU" : codec_string;

	const char *br = "{"
		"absolute_codec_string=PCMU,"
		"codec_string=PCMU,"
		"codec_ms=20,"
		"rtp_disable_crypto=true,"
		"rtp_enable_timer=false,"
		"rtp_timer_name=none,"
		"hangup_after_bridge=false,"
		"ignore_early_media=true,"
		"loopback_bowout=false,"
		"media_webrtc=false,"
		"rtp_trickle_ice=true"
		"}loopback/9999";

	st = switch_ivr_originate(
			NULL,                  /* a-leg session */
			&session,              /* out: b-leg */
			&cause,                /* out: cause */
			br,                    /* bridgeto */
			5,                     /* timeout (sec) */
			NULL, NULL, NULL, NULL,/* table, cid_name, cid_num, outbound_profile_uuid */
			NULL,                  /* ovars (NULL, since we inlined) */
			SOF_NONE,              /* flags */
			&cancel,               /* out: cancel cause */
			NULL);                 /* dial handle */

	if (st != SWITCH_STATUS_SUCCESS || !session) return SWITCH_STATUS_FALSE;

	chan = switch_core_session_get_channel(session);

	mparams = switch_core_session_alloc(session, sizeof(switch_core_media_params_t));
	mparams->inbound_codec_string = switch_core_session_strdup(session, selected_codec);
	mparams->outbound_codec_string = switch_core_session_strdup(session, selected_codec);
	mparams->rtpip = switch_core_session_strdup(session, (char *)rx_host);
	mparams->rtpip4 = switch_core_session_strdup(session, (char *)rx_host);

	status = switch_media_handle_create(&media_handle, session, mparams);
	if (status != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "switch_media_handle_create() failed\n");
		return SWITCH_STATUS_FALSE;
	}

	switch_channel_set_variable(chan, "absolute_codec_string", selected_codec);
	switch_channel_set_variable(chan, "rtp_use_bundle", use_bundle ? "true" : "false");
	switch_channel_set_variable(chan, "send_silence_when_idle", "-1");
	switch_channel_set_variable(chan, "rtp_timer_name", "soft");
	switch_channel_set_variable(chan, "media_timeout", "1000");
	switch_channel_set_variable(chan, "rtp_trickle_ice", "true");
	if (!zstr(ice_role)) {
		switch_channel_set_variable(chan, "rtp_ice_role", ice_role);
	}

	r_sdp = switch_core_session_sprintf(session,
			"v=0\n"
			"o=- 1683118194 1683118195 IN IP4 0.0.0.0\n"
			"s=-\n"
			"t=0 0\n"
			"a=group:BUNDLE 0\n"
			"a=extmap-allow-mixed\n"
			"m=audio 9 UDP/TLS/RTP/SAVPF 0\n"
			"c=IN IP4 0.0.0.0\n"
			"a=ice-ufrag:aZJpsl00bYnjrOZtkCFMtKhFC/CHAfcv\n"
			"a=ice-pwd:aNniSnLLp43SSsJrz6TNPty1zPrxZNzh\n"
			"a=ice-options:trickle\n"
			"a=rtcp-mux\n"
			"a=setup:active\n"
			"a=rtpmap:0 PCMU/8000\n"
			"a=ssrc:2588681350 msid:user199999@host-a1132918 webrtctransceiver0\n"
			"a=ssrc:2588681350 cname:user19999@host-a1132918\n"
			"a=sendrecv\n"
			"a=fingerprint:sha-256 17:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
			"a=mid:0\n"
			"a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level\n"
			"a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time\n"
			"a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\n"
			"a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid\n"
			"a=msid:de61dc02-51d0-4164-9d7a-b74141a4548e 9dc86822-54a5-4506-8476-9be2238be778\n"
			"a=rtcp-rsize\n");

	if (!zstr(offer_sdp)) {
		r_sdp = switch_core_session_strdup(session, offer_sdp);
	}

	if (switch_core_media_prepare_codecs(session, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to prepare codecs\n");
		goto fail;
	}

	match = switch_core_media_negotiate_sdp(session, r_sdp, &p, SDP_OFFER);

	if (match) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "SDP negotiation successful (match=%d, proceed=%d)\n", match, p);
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to negotiate SDP\n");
		goto fail;
	}

	/* Parse SDP for trickle ICE */
	if (out_sdp_session) {
		sdp_parser_t *parser = sdp_parse(NULL, r_sdp, strlen(r_sdp), 0);
		if (parser) {
			sdp_session_t *parsed_sdp = sdp_session(parser);
			if (parsed_sdp) {
				*out_sdp_session = (void*)parsed_sdp;
				if (out_parser) {
					*out_parser = parser;
				}
			} else {
				*out_sdp_session = NULL;
				sdp_parser_free(parser);
				if (out_parser) {
					*out_parser = NULL;
				}
			}
		} else {
			*out_sdp_session = NULL;
			if (out_parser) {
				*out_parser = NULL;
			}
		}
	}

	if (switch_core_media_choose_ports(session, SWITCH_TRUE, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to choose ports\n");
		goto fail;
	}
	if (activate_rtp && switch_core_media_activate_rtp(session) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to activate RTP\n");
		goto fail;
	}

	*out_session = session;
	*out_rtp = activate_rtp ? switch_core_media_get_rtp_session(session, SWITCH_MEDIA_TYPE_AUDIO) : NULL;
	if (activate_rtp && !*out_rtp) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to get RTP session\n");
		goto fail;
	}

	return SWITCH_STATUS_SUCCESS;

fail:
	switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
	switch_core_session_rwunlock(session);
	return SWITCH_STATUS_FALSE;
}

static switch_status_t make_session_and_rtp_with_sdp(switch_core_session_t **out_session,
                                                      switch_rtp_t **out_rtp,
                                                      void **out_sdp_session,
                                                      sdp_parser_t **out_parser)
{
	return make_session_and_rtp_with_sdp_ex(out_session, out_rtp, out_sdp_session, out_parser,
		NULL, "PCMU", SWITCH_TRUE, SWITCH_TRUE, NULL);
}

typedef struct dtls_restart_case_result_s {
	switch_bool_t preserved;
	switch_bool_t failed_closed;
	switch_port_t before_port;
	switch_port_t after_port;
	switch_core_media_ice_type_t before_ice_type;
	switch_core_media_ice_type_t after_ice_type;
} dtls_restart_case_result_t;

static switch_status_t run_dtls_restart_identity_case(const char *initial_sdp, const char *restart_sdp,
	uint32_t setup_timeout_ms, uint32_t delay_us, switch_bool_t change_local_identity,
	dtls_restart_case_result_t *result)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel;
	switch_rtp_t *rtp = NULL;
	void *sdp_session = NULL;
	sdp_parser_t *parser = NULL;
	switch_rtp_pvt_transport_snapshot_t before;
	switch_rtp_pvt_transport_snapshot_t after;
	switch_status_t status;
	uint8_t match;
	uint8_t proceed = 0;
	char host[80] = "";
	char timeout_value[32] = "";
	switch_rtp_pvt_dtls_identity_t association_identity;

	if (!result) {
		return SWITCH_STATUS_FALSE;
	}
	memset(result, 0, sizeof(*result));
	status = make_session_and_rtp_with_sdp_ex(&session, &rtp, &sdp_session, &parser,
		initial_sdp, "PCMU", SWITCH_TRUE, SWITCH_TRUE, NULL);
	if (status != SWITCH_STATUS_SUCCESS || !session || !rtp) {
		return SWITCH_STATUS_FALSE;
	}
	channel = switch_core_session_get_channel(session);
	status = switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &before);
	if (!channel || status != SWITCH_STATUS_SUCCESS || !before.dtls_context || !before.dtls_ssl ||
		copy_sockaddr_tuple(switch_rtp_session_get_remote_addr(rtp), host, sizeof(host),
			&result->before_port) != SWITCH_STATUS_SUCCESS) {
		cleanup_session_media_and_sdp(session, sdp_session, parser);
		return SWITCH_STATUS_FALSE;
	}
	result->before_ice_type = before.ice_type;
	if (change_local_identity) {
		memset(&association_identity, 0, sizeof(association_identity));
		if (switch_rtp_pvt_get_dtls_association_identity(rtp, DTLS_TYPE_RTP,
			&association_identity) != SWITCH_STATUS_SUCCESS || zstr(association_identity.local_value)) {
			cleanup_session_media_and_sdp(session, sdp_session, parser);
			return SWITCH_STATUS_FALSE;
		}
		association_identity.local_value[0] =
			association_identity.local_value[0] == 'A' ? 'B' : 'A';
		switch_rtp_pvt_set_dtls_association_identity(rtp, DTLS_TYPE_RTP, &association_identity);
	}
	if (setup_timeout_ms) {
		switch_snprintf(timeout_value, sizeof(timeout_value), "%u", setup_timeout_ms);
		switch_channel_set_variable(channel, "media_dtls_setup_timeout", timeout_value);
	}
	if (delay_us) {
		switch_rtp_session_set_dtls_checks_started(rtp, switch_micro_time_now() - delay_us);
	}

	switch_core_media_clear_ice(session);
	switch_channel_set_flag(channel, CF_REINVITE);
	match = switch_core_media_negotiate_sdp(session, restart_sdp, &proceed, SDP_OFFER);
	if (!match) {
		if (switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &after) == SWITCH_STATUS_SUCCESS &&
			copy_sockaddr_tuple(switch_rtp_session_get_remote_addr(rtp), host, sizeof(host),
				&result->after_port) == SWITCH_STATUS_SUCCESS) {
			result->failed_closed = after.dtls_state == DS_FAIL &&
				after.dtls_context == before.dtls_context && after.dtls_ssl == before.dtls_ssl &&
				result->after_port == result->before_port ? SWITCH_TRUE : SWITCH_FALSE;
		}
		cleanup_session_media_and_sdp(session, sdp_session, parser);
		return result->failed_closed ? SWITCH_STATUS_TIMEOUT : SWITCH_STATUS_FALSE;
	}
	switch_core_media_gen_local_sdp(session, SDP_ANSWER, NULL, 0, NULL, 0);
	status = switch_core_media_activate_rtp(session);
	if (status != SWITCH_STATUS_SUCCESS ||
		switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &after) != SWITCH_STATUS_SUCCESS ||
		copy_sockaddr_tuple(switch_rtp_session_get_remote_addr(rtp), host, sizeof(host),
			&result->after_port) != SWITCH_STATUS_SUCCESS) {
		cleanup_session_media_and_sdp(session, sdp_session, parser);
		return SWITCH_STATUS_FALSE;
	}

	result->preserved = after.dtls_state == DS_HANDSHAKE &&
		after.dtls_context == before.dtls_context && after.dtls_ssl == before.dtls_ssl ?
		SWITCH_TRUE : SWITCH_FALSE;
	result->after_ice_type = after.ice_type;
	cleanup_session_media_and_sdp(session, sdp_session, parser);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t build_dtls_pair_sdp_ex(char *buf, switch_size_t buf_len, uint32_t version,
	switch_port_t port, const char *setup, const char *ufrag, const char *pwd,
	switch_bool_t bundle)
{
	int written;

	if (!buf || !buf_len || zstr(setup) || zstr(ufrag) || zstr(pwd)) {
		return SWITCH_STATUS_FALSE;
	}

	written = switch_snprintf(buf, buf_len,
		"v=0\n"
		"o=- 1683118194 %u IN IP4 0.0.0.0\n"
		"s=-\n"
		"t=0 0\n"
		"%s"
		"m=audio %u UDP/TLS/RTP/SAVPF 0\n"
		"c=IN IP4 127.0.0.1\n"
		"a=ice-ufrag:%s\n"
		"a=ice-pwd:%s\n"
		"a=ice-options:trickle\n"
		"a=candidate:1 1 udp 2130706431 127.0.0.1 %u typ host\n"
		"a=rtcp-mux\n"
		"a=setup:%s\n"
		"a=rtpmap:0 PCMU/8000\n"
		"a=sendrecv\n"
		"a=fingerprint:sha-256 17:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
		"a=mid:0\n",
		version, bundle ? "a=group:BUNDLE 0\n" : "", (unsigned)port, ufrag, pwd,
		(unsigned)port, setup);

	return written > 0 && (switch_size_t)written < buf_len ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

static switch_status_t build_dtls_pair_sdp(char *buf, switch_size_t buf_len, uint32_t version,
	switch_port_t port, const char *setup, const char *ufrag, const char *pwd)
{
	return build_dtls_pair_sdp_ex(buf, buf_len, version, port, setup, ufrag, pwd, SWITCH_TRUE);
}

static switch_status_t add_rtp_candidate_to_sdp(char *sdp, switch_size_t sdp_len,
	uint32_t foundation, uint32_t priority, switch_port_t port)
{
	char candidate[160];
	char *marker;
	switch_size_t candidate_len;
	switch_size_t tail_len;
	int written;

	if (zstr(sdp) || !(marker = strstr(sdp, "a=rtcp-mux\n"))) {
		return SWITCH_STATUS_FALSE;
	}
	written = switch_snprintf(candidate, sizeof(candidate),
		"a=candidate:%u 1 udp %u 127.0.0.1 %u typ host\n",
		(unsigned)foundation, (unsigned)priority, (unsigned)port);
	if (written <= 0 || (switch_size_t)written >= sizeof(candidate)) {
		return SWITCH_STATUS_FALSE;
	}
	candidate_len = (switch_size_t)written;
	tail_len = strlen(marker);
	if (strlen(sdp) + candidate_len >= sdp_len) {
		return SWITCH_STATUS_FALSE;
	}
	memmove(marker + candidate_len, marker, tail_len + 1);
	memcpy(marker, candidate, candidate_len);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t restart_and_nominate_controlled_dtls(switch_core_session_t *session,
	switch_rtp_t *rtp, const char *restart_sdp, switch_port_t nominated_port,
	switch_rtp_pvt_transport_snapshot_t *snapshot)
{
	switch_channel_t *channel;
	char ice_user[513] = "";
	char incoming_user[513] = "";
	char local_pwd[256] = "";
	char remote_pwd[256] = "";
	uint8_t stun_packet[512];
	switch_size_t stun_len;
	switch_bool_t has_addr = SWITCH_FALSE;
	uint8_t match;
	uint8_t proceed = 0;

	if (!session || !rtp || zstr(restart_sdp) || !snapshot) {
		return SWITCH_STATUS_FALSE;
	}
	channel = switch_core_session_get_channel(session);
	if (!channel) {
		return SWITCH_STATUS_FALSE;
	}

	switch_core_media_clear_ice(session);
	switch_channel_set_flag(channel, CF_REINVITE);
	match = switch_core_media_negotiate_sdp(session, restart_sdp, &proceed, SDP_OFFER);
	if (!match) {
		return SWITCH_STATUS_FALSE;
	}
	switch_core_media_gen_local_sdp(session, SDP_ANSWER, NULL, 0, NULL, 0);
	if (switch_core_media_activate_rtp(session) != SWITCH_STATUS_SUCCESS ||
		switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, snapshot) != SWITCH_STATUS_SUCCESS ||
		!(snapshot->ice_type & ICE_CONTROLLED) || !snapshot->dtls_restart_pending) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_rtp_pvt_get_ice_state(rtp, IPR_RTP,
		ice_user, sizeof(ice_user), local_pwd, sizeof(local_pwd),
		remote_pwd, sizeof(remote_pwd), &has_addr) != SWITCH_STATUS_SUCCESS ||
		reverse_ice_username(ice_user, incoming_user, sizeof(incoming_user)) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}
	stun_len = build_authenticated_ice_request(stun_packet, sizeof(stun_packet), incoming_user, local_pwd);
	if (switch_rtp_pvt_handle_ice_from(rtp, IPR_RTP, "127.0.0.1", nominated_port,
		stun_packet, stun_len) != SWITCH_STATUS_SUCCESS ||
		switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, snapshot) != SWITCH_STATUS_SUCCESS ||
		snapshot->dtls_restart_pending || !snapshot->dtls_restart_migrated ||
		!snapshot->ice_ready || !snapshot->ice_rready) {
		return SWITCH_STATUS_FALSE;
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t restart_nominate_then_activate_controlled_dtls(switch_core_session_t *session,
	switch_rtp_t *rtp, const char *restart_sdp, switch_port_t nominated_port,
	switch_rtp_pvt_transport_snapshot_t *nominated_snapshot,
	switch_rtp_pvt_transport_snapshot_t *activated_snapshot)
{
	switch_channel_t *channel;
	char ice_user[513] = "";
	char incoming_user[513] = "";
	char local_pwd[256] = "";
	char remote_pwd[256] = "";
	uint8_t stun_packet[512];
	switch_size_t stun_len;
	switch_bool_t has_addr = SWITCH_FALSE;
	uint8_t match;
	uint8_t proceed = 0;

	if (!session || !rtp || zstr(restart_sdp) || !nominated_snapshot || !activated_snapshot) {
		return SWITCH_STATUS_FALSE;
	}
	channel = switch_core_session_get_channel(session);
	if (!channel) {
		return SWITCH_STATUS_FALSE;
	}

	switch_core_media_clear_ice(session);
	switch_channel_set_flag(channel, CF_REINVITE);
	match = switch_core_media_negotiate_sdp(session, restart_sdp, &proceed, SDP_OFFER);
	if (!match || switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP,
		nominated_snapshot) != SWITCH_STATUS_SUCCESS ||
		!(nominated_snapshot->ice_type & ICE_CONTROLLED) ||
		!nominated_snapshot->dtls_restart_pending) {
		return SWITCH_STATUS_FALSE;
	}
	if (switch_rtp_pvt_get_ice_state(rtp, IPR_RTP,
		ice_user, sizeof(ice_user), local_pwd, sizeof(local_pwd),
		remote_pwd, sizeof(remote_pwd), &has_addr) != SWITCH_STATUS_SUCCESS ||
		reverse_ice_username(ice_user, incoming_user, sizeof(incoming_user)) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}
	stun_len = build_authenticated_ice_request(stun_packet, sizeof(stun_packet), incoming_user, local_pwd);
	if (switch_rtp_pvt_handle_ice_from(rtp, IPR_RTP, "127.0.0.1", nominated_port,
		stun_packet, stun_len) != SWITCH_STATUS_SUCCESS ||
		switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP,
			nominated_snapshot) != SWITCH_STATUS_SUCCESS ||
		nominated_snapshot->dtls_restart_pending || !nominated_snapshot->dtls_restart_migrated) {
		return SWITCH_STATUS_FALSE;
	}

	switch_core_media_gen_local_sdp(session, SDP_ANSWER, NULL, 0, NULL, 0);
	if (switch_core_media_activate_rtp(session) != SWITCH_STATUS_SUCCESS ||
		switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP,
			activated_snapshot) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t drive_dtls_pair_ready(switch_rtp_t *client_rtp, switch_rtp_t *server_rtp)
{
	uint8_t client_to_server[32768];
	uint8_t server_to_client[32768];
	switch_size_t client_to_server_len = 0;
	switch_size_t server_to_client_len = 0;
	switch_rtp_pvt_transport_snapshot_t client_snapshot;
	switch_rtp_pvt_transport_snapshot_t server_snapshot;
	switch_status_t status;
	int step;

	if (!client_rtp || !server_rtp) {
		return SWITCH_STATUS_FALSE;
	}
	status = switch_rtp_pvt_dtls_step(client_rtp, DTLS_TYPE_RTP, NULL, 0,
		client_to_server, sizeof(client_to_server), &client_to_server_len);
	if (status != SWITCH_STATUS_SUCCESS || !client_to_server_len) {
		return SWITCH_STATUS_FALSE;
	}
	for (step = 0; step < 32; ++step) {
		server_to_client_len = 0;
		status = switch_rtp_pvt_dtls_step(server_rtp, DTLS_TYPE_RTP,
			client_to_server, client_to_server_len, server_to_client,
			sizeof(server_to_client), &server_to_client_len);
		if (status != SWITCH_STATUS_SUCCESS) {
			return SWITCH_STATUS_FALSE;
		}
		client_to_server_len = 0;
		status = switch_rtp_pvt_dtls_step(client_rtp, DTLS_TYPE_RTP,
			server_to_client, server_to_client_len, client_to_server,
			sizeof(client_to_server), &client_to_server_len);
		if (status != SWITCH_STATUS_SUCCESS ||
			switch_rtp_pvt_get_transport_snapshot(client_rtp, IPR_RTP,
				&client_snapshot) != SWITCH_STATUS_SUCCESS ||
			switch_rtp_pvt_get_transport_snapshot(server_rtp, IPR_RTP,
				&server_snapshot) != SWITCH_STATUS_SUCCESS) {
			return SWITCH_STATUS_FALSE;
		}
		if (client_snapshot.dtls_state == DS_READY && server_snapshot.dtls_state == DS_READY) {
			return SWITCH_STATUS_SUCCESS;
		}
	}

	return SWITCH_STATUS_TIMEOUT;
}

static switch_status_t run_ready_invalid_identity_restart(const char *restart_sdp)
{
	switch_core_session_t *target_session = NULL;
	switch_core_session_t *peer_session = NULL;
	switch_rtp_t *target_rtp = NULL;
	switch_rtp_t *peer_rtp = NULL;
	void *target_sdp_session = NULL;
	void *peer_sdp_session = NULL;
	sdp_parser_t *target_parser = NULL;
	sdp_parser_t *peer_parser = NULL;
	switch_channel_t *target_channel;
	switch_rtp_pvt_transport_snapshot_t before;
	switch_rtp_pvt_transport_snapshot_t after;
	switch_status_t status = SWITCH_STATUS_FALSE;
	char target_base[2048];
	char peer_sdp[2048];
	char *target_sdp = NULL;
	uint8_t match;
	uint8_t proceed = 0;

	if (zstr(restart_sdp) ||
		build_dtls_pair_sdp_ex(target_base, sizeof(target_base), 1683118280, 18910,
			"passive", "invalidIdentityTargetUfrag1", "invalidIdentityTargetPassword1234561",
			SWITCH_FALSE) != SWITCH_STATUS_SUCCESS ||
		build_dtls_pair_sdp_ex(peer_sdp, sizeof(peer_sdp), 1683118281, 18920,
			"active", "invalidIdentityPeerUfrag1", "invalidIdentityPeerPassword1234561",
			SWITCH_FALSE) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}
	target_sdp = switch_string_replace(target_base, "a=fingerprint:",
		"a=tls-id:association-1\na=fingerprint:");
	if (!target_sdp) {
		return SWITCH_STATUS_FALSE;
	}
	if (make_session_and_rtp_with_sdp_ex(&target_session, &target_rtp,
		&target_sdp_session, &target_parser, target_sdp, "PCMU", SWITCH_FALSE,
		SWITCH_TRUE, "controlled") != SWITCH_STATUS_SUCCESS || !target_session || !target_rtp ||
		make_session_and_rtp_with_sdp_ex(&peer_session, &peer_rtp,
		&peer_sdp_session, &peer_parser, peer_sdp, "PCMU", SWITCH_FALSE,
		SWITCH_TRUE, NULL) != SWITCH_STATUS_SUCCESS || !peer_session || !peer_rtp ||
		drive_dtls_pair_ready(target_rtp, peer_rtp) != SWITCH_STATUS_SUCCESS ||
		switch_rtp_pvt_get_transport_snapshot(target_rtp, IPR_RTP, &before) != SWITCH_STATUS_SUCCESS ||
		before.dtls_state != DS_READY) {
		goto done;
	}
	target_channel = switch_core_session_get_channel(target_session);
	if (!target_channel) {
		goto done;
	}
	switch_core_media_clear_ice(target_session);
	switch_channel_set_flag(target_channel, CF_REINVITE);
	match = switch_core_media_negotiate_sdp(target_session, restart_sdp, &proceed, SDP_OFFER);
	if (!match) {
		goto done;
	}
	switch_core_media_gen_local_sdp(target_session, SDP_ANSWER, NULL, 0, NULL, 0);
	if (switch_core_media_activate_rtp(target_session) != SWITCH_STATUS_SUCCESS ||
		switch_rtp_pvt_get_transport_snapshot(target_rtp, IPR_RTP, &after) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}
	if (after.dtls_state != DS_READY && after.dtls_destroy_count == before.dtls_destroy_count + 1 &&
		after.srtp_send_ready == SWITCH_FALSE && after.srtp_recv_ready == SWITCH_FALSE) {
		status = SWITCH_STATUS_SUCCESS;
	}

done:
	if (target_session) {
		cleanup_session_media_and_sdp(target_session, target_sdp_session, target_parser);
	}
	if (peer_session) {
		cleanup_session_media_and_sdp(peer_session, peer_sdp_session, peer_parser);
	}
	switch_safe_free(target_sdp);
	return status;
}

static volatile int trickle_ev_seen = 0;
static char last_cand_line[512];

static void trickle_event_handler(switch_event_t *ev)
{
	const char *sub = switch_event_get_header(ev, "Event-Subclass");
	if (sub && !strcmp(sub, "sofia::trickle-ice")) {
		const char *cand = switch_event_get_header(ev, "a-candidate");
		if (cand) switch_snprintf(last_cand_line, sizeof(last_cand_line), "%s", cand);
		trickle_ev_seen = 1;
	}
}

typedef struct dtls_restart_teardown_race_s {
	switch_rtp_t *rtp;
	switch_core_session_t *session;
	switch_mutex_t *mutex;
	switch_thread_cond_t *cond;
	int ready;
	int start;
	int completed;
	int abort;
	switch_interval_time_t abort_completion_delay;
	uint8_t stun_packet[512];
	switch_size_t stun_len;
	uint8_t dtls_packet[1];
	switch_status_t ice_status;
	switch_status_t dtls_status;
	switch_status_t identity_status;
	switch_status_t teardown_status;
	switch_rtp_pvt_dtls_restart_result_t preparation_result;
} dtls_restart_teardown_race_t;

typedef struct dtls_datawait_activation_race_s {
	switch_rtp_t *rtp;
	switch_mutex_t *mutex;
	switch_thread_cond_t *cond;
	ice_t restart_ice;
	int reader_started;
	int reader_done;
	int activation_started;
	int activation_done;
	switch_status_t reader_status;
	switch_status_t activation_status;
} dtls_datawait_activation_race_t;

typedef struct rtcp_activation_race_s {
	switch_rtp_t *rtp;
	switch_mutex_t *mutex;
	switch_thread_cond_t *cond;
	ice_t ice_params;
	switch_core_media_ice_type_t ice_type;
	char remote_ufrag[256];
	char local_ufrag[256];
	char local_pwd[256];
	char remote_pwd[256];
	int reader_started;
	int reader_done;
	int activation_started;
	int activation_write_locked;
	int activation_done;
	int stun_dispatch_started;
	int dispatch_overlapped;
	int rtcp_dtls_removed;
	switch_status_t reader_status;
	switch_status_t activation_status;
	switch_status_t rtcp_dtls_remove_status;
} rtcp_activation_race_t;

typedef struct dtls_destination_lifecycle_race_s {
	switch_rtp_t *rtp;
	switch_mutex_t *mutex;
	switch_thread_cond_t *cond;
	switch_rtp_pvt_dtls_identity_t identity;
	int reader_started;
	int reader_done;
	int dtls_ice_locked;
	int lifecycle_started;
	int lifecycle_write_locked;
	int lifecycle_done;
	switch_status_t reader_status;
	switch_status_t lifecycle_status;
} dtls_destination_lifecycle_race_t;

typedef struct bounded_test_watchdog_s {
	switch_mutex_t *mutex;
	switch_thread_cond_t *cond;
	switch_interval_time_t timeout;
	const char *label;
	int stopped;
} bounded_test_watchdog_t;

static void *SWITCH_THREAD_FUNC bounded_test_watchdog_thread(switch_thread_t *thread, void *obj)
{
	bounded_test_watchdog_t *watchdog = obj;
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_time_t deadline = switch_micro_time_now() + watchdog->timeout;
	switch_interval_time_t remaining;
	int expired;
	(void)thread;

	switch_mutex_lock(watchdog->mutex);
	while (!watchdog->stopped && status != SWITCH_STATUS_TIMEOUT) {
		remaining = deadline - switch_micro_time_now();
		if (remaining <= 0) {
			status = SWITCH_STATUS_TIMEOUT;
			break;
		}
		status = switch_thread_cond_timedwait(watchdog->cond, watchdog->mutex, remaining);
	}
	expired = watchdog->stopped ? 0 : 1;
	switch_mutex_unlock(watchdog->mutex);
	if (expired) {
		fprintf(stderr, "fatal %s watchdog timeout\n", watchdog->label);
		fflush(stderr);
		_exit(1);
	}

	return NULL;
}

static switch_status_t bounded_test_watchdog_start(bounded_test_watchdog_t *watchdog,
	switch_threadattr_t *thread_attr, switch_memory_pool_t *pool,
	switch_interval_time_t timeout, const char *label, switch_thread_t **watchdog_thread)
{
	if (!watchdog || !thread_attr || !pool || !label || !watchdog_thread || timeout <= 0) {
		return SWITCH_STATUS_FALSE;
	}
	memset(watchdog, 0, sizeof(*watchdog));
	watchdog->timeout = timeout;
	watchdog->label = label;
	if (switch_mutex_init(&watchdog->mutex, SWITCH_MUTEX_NESTED, pool) != SWITCH_STATUS_SUCCESS ||
		switch_thread_cond_create(&watchdog->cond, pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}
	return switch_thread_create(watchdog_thread, thread_attr,
		bounded_test_watchdog_thread, watchdog, pool);
}

static void bounded_test_watchdog_stop(bounded_test_watchdog_t *watchdog,
	switch_thread_t *watchdog_thread)
{
	switch_status_t thread_status = SWITCH_STATUS_FALSE;

	if (!watchdog || !watchdog_thread) {
		_exit(1);
	}
	switch_mutex_lock(watchdog->mutex);
	watchdog->stopped = 1;
	switch_thread_cond_broadcast(watchdog->cond);
	switch_mutex_unlock(watchdog->mutex);
	if (switch_thread_join(&thread_status, watchdog_thread) != SWITCH_STATUS_SUCCESS ||
		thread_status != SWITCH_STATUS_SUCCESS) {
		fprintf(stderr, "fatal %s watchdog join failure\n", watchdog->label);
		fflush(stderr);
		_exit(1);
	}
}

static void dtls_datawait_activation_signal(dtls_datawait_activation_race_t *race,
	int *state)
{
	switch_mutex_lock(race->mutex);
	*state = 1;
	switch_thread_cond_broadcast(race->cond);
	switch_mutex_unlock(race->mutex);
}

static switch_bool_t dtls_datawait_activation_wait(dtls_datawait_activation_race_t *race,
	int *state, switch_interval_time_t timeout)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_bool_t completed;
	switch_time_t deadline = switch_micro_time_now() + timeout;
	switch_interval_time_t remaining;

	switch_mutex_lock(race->mutex);
	while (!*state && status != SWITCH_STATUS_TIMEOUT) {
		remaining = deadline - switch_micro_time_now();
		if (remaining <= 0) {
			status = SWITCH_STATUS_TIMEOUT;
			break;
		}
		status = switch_thread_cond_timedwait(race->cond, race->mutex, remaining);
	}
	completed = *state ? SWITCH_TRUE : SWITCH_FALSE;
	switch_mutex_unlock(race->mutex);
	return completed;
}

static switch_bool_t dtls_datawait_activation_state(dtls_datawait_activation_race_t *race,
	int *state)
{
	switch_bool_t value;

	switch_mutex_lock(race->mutex);
	value = *state ? SWITCH_TRUE : SWITCH_FALSE;
	switch_mutex_unlock(race->mutex);
	return value;
}

static switch_bool_t rtp_read_lock_wait(switch_rtp_t *rtp, switch_interval_time_t timeout)
{
	switch_time_t deadline = switch_micro_time_now() + timeout;

	while (switch_micro_time_now() < deadline) {
		if (switch_rtp_pvt_read_lock_held(rtp)) {
			return SWITCH_TRUE;
		}
		switch_sleep(1000);
	}

	return SWITCH_FALSE;
}

static void *SWITCH_THREAD_FUNC dtls_datawait_reader_thread(switch_thread_t *thread, void *obj)
{
	dtls_datawait_activation_race_t *race = obj;
	uint8_t data[SWITCH_RTP_MAX_BUF_LEN];
	uint32_t datalen = sizeof(data);
	switch_payload_t payload = 0;
	switch_frame_flag_t flags = 0;
	(void)thread;

	dtls_datawait_activation_signal(race, &race->reader_started);
	race->reader_status = switch_rtp_read(race->rtp, data, &datalen, &payload, &flags,
		SWITCH_IO_FLAG_NONE);
	dtls_datawait_activation_signal(race, &race->reader_done);
	return NULL;
}

static void rtcp_activation_signal(rtcp_activation_race_t *race, int *state)
{
	switch_mutex_lock(race->mutex);
	*state = 1;
	switch_thread_cond_broadcast(race->cond);
	switch_mutex_unlock(race->mutex);
}

static switch_bool_t rtcp_activation_wait(rtcp_activation_race_t *race, int *state,
	switch_interval_time_t timeout)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_bool_t completed;
	switch_time_t deadline = switch_micro_time_now() + timeout;
	switch_interval_time_t remaining;

	switch_mutex_lock(race->mutex);
	while (!*state && status != SWITCH_STATUS_TIMEOUT) {
		remaining = deadline - switch_micro_time_now();
		if (remaining <= 0) {
			status = SWITCH_STATUS_TIMEOUT;
			break;
		}
		status = switch_thread_cond_timedwait(race->cond, race->mutex, remaining);
	}
	completed = *state ? SWITCH_TRUE : SWITCH_FALSE;
	switch_mutex_unlock(race->mutex);

	return completed;
}

static void rtcp_activation_write_locked_hook(void *obj)
{
	rtcp_activation_race_t *race = obj;

	fprintf(stderr, "RTCP_LOCK_TEST activation holds write mutex\n");
	fflush(stderr);
	rtcp_activation_signal(race, &race->activation_write_locked);
	rtcp_activation_wait(race, &race->stun_dispatch_started, 2000000);
}

static void rtcp_activation_stun_dispatch_hook(void *obj)
{
	rtcp_activation_race_t *race = obj;

	switch_mutex_lock(race->mutex);
	race->stun_dispatch_started = 1;
	if (race->activation_write_locked && !race->activation_done) {
		race->dispatch_overlapped = 1;
	}
	fprintf(stderr, "RTCP_LOCK_TEST STUN dispatch overlapped activation\n");
	fflush(stderr);
	switch_thread_cond_broadcast(race->cond);
	switch_mutex_unlock(race->mutex);
}

static void rtcp_dtls_remove_hook(void *obj)
{
	rtcp_activation_race_t *race = obj;

	race->rtcp_dtls_remove_status = switch_rtp_del_dtls(race->rtp, DTLS_TYPE_RTCP);
	rtcp_activation_signal(race, &race->rtcp_dtls_removed);
}

static void *SWITCH_THREAD_FUNC rtcp_activation_reader_thread(switch_thread_t *thread, void *obj)
{
	rtcp_activation_race_t *race = obj;
	uint8_t data[SWITCH_RTP_MAX_BUF_LEN];
	uint32_t datalen = sizeof(data);
	switch_payload_t payload = 0;
	switch_frame_flag_t flags = 0;
	(void)thread;

	rtcp_activation_signal(race, &race->reader_started);
	race->reader_status = switch_rtp_read(race->rtp, data, &datalen, &payload, &flags,
		SWITCH_IO_FLAG_NONE);
	rtcp_activation_signal(race, &race->reader_done);

	return NULL;
}

static void *SWITCH_THREAD_FUNC rtcp_activation_worker_thread(switch_thread_t *thread, void *obj)
{
	rtcp_activation_race_t *race = obj;
	(void)thread;

	rtcp_activation_signal(race, &race->activation_started);
	race->activation_status = switch_rtp_activate_ice(race->rtp, race->remote_ufrag,
		race->local_ufrag, race->local_pwd, race->remote_pwd, IPR_RTCP,
		race->ice_type, &race->ice_params);
	rtcp_activation_signal(race, &race->activation_done);

	return NULL;
}

static void dtls_destination_lifecycle_signal(dtls_destination_lifecycle_race_t *race,
	int *state)
{
	switch_mutex_lock(race->mutex);
	*state = 1;
	switch_thread_cond_broadcast(race->cond);
	switch_mutex_unlock(race->mutex);
}

static switch_bool_t dtls_destination_lifecycle_wait(dtls_destination_lifecycle_race_t *race,
	int *state, switch_interval_time_t timeout)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_bool_t completed;
	switch_time_t deadline = switch_micro_time_now() + timeout;
	switch_interval_time_t remaining;

	switch_mutex_lock(race->mutex);
	while (!*state && status != SWITCH_STATUS_TIMEOUT) {
		remaining = deadline - switch_micro_time_now();
		if (remaining <= 0) {
			status = SWITCH_STATUS_TIMEOUT;
			break;
		}
		status = switch_thread_cond_timedwait(race->cond, race->mutex, remaining);
	}
	completed = *state ? SWITCH_TRUE : SWITCH_FALSE;
	switch_mutex_unlock(race->mutex);

	return completed;
}

static switch_bool_t dtls_destination_lifecycle_state(dtls_destination_lifecycle_race_t *race,
	int *state)
{
	switch_bool_t value;

	switch_mutex_lock(race->mutex);
	value = *state ? SWITCH_TRUE : SWITCH_FALSE;
	switch_mutex_unlock(race->mutex);
	return value;
}

static void dtls_destination_ice_locked_hook(void *obj)
{
	dtls_destination_lifecycle_race_t *race = obj;

	fprintf(stderr, "DTLS_LOCK_TEST receive holds ICE mutex\n");
	fflush(stderr);
	dtls_destination_lifecycle_signal(race, &race->dtls_ice_locked);
	if (!dtls_destination_lifecycle_wait(race, &race->lifecycle_write_locked, 2000000)) {
		_exit(1);
	}
}

static void dtls_destination_lifecycle_write_locked_hook(void *obj)
{
	dtls_destination_lifecycle_race_t *race = obj;

	fprintf(stderr, "DTLS_LOCK_TEST lifecycle holds write mutex\n");
	fflush(stderr);
	dtls_destination_lifecycle_signal(race, &race->lifecycle_write_locked);
}

static void *SWITCH_THREAD_FUNC dtls_destination_reader_thread(switch_thread_t *thread, void *obj)
{
	dtls_destination_lifecycle_race_t *race = obj;
	uint8_t data[SWITCH_RTP_MAX_BUF_LEN];
	uint32_t datalen = sizeof(data);
	switch_payload_t payload = 0;
	switch_frame_flag_t flags = 0;
	switch_time_t deadline = switch_micro_time_now() + 2000000;
	switch_bool_t reached_destination_update = SWITCH_FALSE;
	(void)thread;

	fprintf(stderr, "DTLS_LOCK_TEST reader started\n");
	fflush(stderr);
	dtls_destination_lifecycle_signal(race, &race->reader_started);
	do {
		datalen = sizeof(data);
		payload = 0;
		flags = 0;
		race->reader_status = switch_rtp_read(race->rtp, data, &datalen, &payload, &flags,
			SWITCH_IO_FLAG_SINGLE_READ);
		switch_mutex_lock(race->mutex);
		reached_destination_update = race->dtls_ice_locked ? SWITCH_TRUE : SWITCH_FALSE;
		switch_mutex_unlock(race->mutex);
	} while (!reached_destination_update && switch_micro_time_now() < deadline);
	fprintf(stderr, "DTLS_LOCK_TEST reader returned status=%d\n", race->reader_status);
	fflush(stderr);
	dtls_destination_lifecycle_signal(race, &race->reader_done);

	return NULL;
}

static void *SWITCH_THREAD_FUNC dtls_destination_lifecycle_thread(switch_thread_t *thread,
	void *obj)
{
	dtls_destination_lifecycle_race_t *race = obj;
	(void)thread;

	dtls_destination_lifecycle_signal(race, &race->lifecycle_started);
	race->lifecycle_status = switch_rtp_pvt_get_dtls_association_identity(race->rtp,
		DTLS_TYPE_RTP, &race->identity);
	dtls_destination_lifecycle_signal(race, &race->lifecycle_done);

	return NULL;
}

static void *SWITCH_THREAD_FUNC dtls_datawait_activation_thread(switch_thread_t *thread, void *obj)
{
	dtls_datawait_activation_race_t *race = obj;
	(void)thread;

	dtls_datawait_activation_signal(race, &race->activation_started);
	race->activation_status = switch_rtp_activate_ice(race->rtp,
		"datawaitRemoteUfrag2", "datawaitLocalUfrag2",
		"datawaitLocalPassword1234562", "datawaitRemotePassword1234562",
		IPR_RTP, ICE_VANILLA | ICE_CONTROLLED, &race->restart_ice);
	dtls_datawait_activation_signal(race, &race->activation_done);
	return NULL;
}

static switch_status_t dtls_datawait_activation_start_workers(
	dtls_datawait_activation_race_t *race, switch_threadattr_t *thread_attr,
	switch_memory_pool_t *pool, switch_thread_t **reader_thread,
	switch_thread_t **activation_thread, int fail_at, int *started)
{
	if (!race || !thread_attr || !pool || !reader_thread || !activation_thread || !started) {
		return SWITCH_STATUS_FALSE;
	}
	*reader_thread = NULL;
	*activation_thread = NULL;
	*started = 0;
	if (fail_at == 0 || switch_thread_create(reader_thread, thread_attr,
		dtls_datawait_reader_thread, race, pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}
	(*started)++;
	if (fail_at == 1 || switch_thread_create(activation_thread, thread_attr,
		dtls_datawait_activation_thread, race, pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}
	(*started)++;

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t dtls_datawait_activation_abort_and_join(
	dtls_datawait_activation_race_t *race, switch_thread_t *reader_thread,
	switch_thread_t *activation_thread, int started,
	switch_status_t *reader_thread_status, switch_status_t *activation_thread_status)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (!race || started < 0 || started > 2) {
		return SWITCH_STATUS_FALSE;
	}
	switch_rtp_clear_flag(race->rtp, SWITCH_RTP_FLAG_DATAWAIT);
	switch_rtp_break(race->rtp);
	if (started > 0 && !dtls_datawait_activation_wait(race, &race->reader_done, 1000000)) {
		switch_rtp_kill_socket(race->rtp);
		if (!dtls_datawait_activation_wait(race, &race->reader_done, 2000000)) {
			return SWITCH_STATUS_TIMEOUT;
		}
	}
	if (started > 1 && !dtls_datawait_activation_wait(race, &race->activation_done, 5000000)) {
		return SWITCH_STATUS_TIMEOUT;
	}
	if (started > 0 && (!reader_thread || switch_thread_join(reader_thread_status,
		reader_thread) != SWITCH_STATUS_SUCCESS)) {
		status = SWITCH_STATUS_FALSE;
	}
	if (started > 1 && (!activation_thread || switch_thread_join(activation_thread_status,
		activation_thread) != SWITCH_STATUS_SUCCESS)) {
		status = SWITCH_STATUS_FALSE;
	}

	return status;
}

static switch_bool_t dtls_restart_teardown_race_wait(dtls_restart_teardown_race_t *race)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_time_t deadline = switch_micro_time_now() + 5000000;
	switch_interval_time_t remaining;
	int abort;

	switch_mutex_lock(race->mutex);
	race->ready++;
	switch_thread_cond_broadcast(race->cond);
	while (!race->start) {
		remaining = deadline - switch_micro_time_now();
		if (remaining <= 0) {
			status = SWITCH_STATUS_TIMEOUT;
			break;
		}
		status = switch_thread_cond_timedwait(race->cond, race->mutex, remaining);
	}
	abort = race->abort;
	switch_mutex_unlock(race->mutex);

	return status == SWITCH_STATUS_TIMEOUT || abort ? SWITCH_FALSE : SWITCH_TRUE;
}

static void dtls_restart_teardown_race_complete(dtls_restart_teardown_race_t *race)
{
	switch_interval_time_t abort_completion_delay = 0;

	switch_mutex_lock(race->mutex);
	if (race->abort) {
		abort_completion_delay = race->abort_completion_delay;
	}
	switch_mutex_unlock(race->mutex);
	if (abort_completion_delay > 0) {
		switch_sleep(abort_completion_delay);
	}
	switch_mutex_lock(race->mutex);
	race->completed++;
	switch_thread_cond_broadcast(race->cond);
	switch_mutex_unlock(race->mutex);
}

static void *SWITCH_THREAD_FUNC dtls_restart_teardown_ice_thread(switch_thread_t *thread, void *obj)
{
	dtls_restart_teardown_race_t *race = obj;
	(void)thread;

	if (dtls_restart_teardown_race_wait(race)) {
		race->ice_status = switch_rtp_pvt_handle_ice_from(race->rtp, IPR_RTP, "127.0.0.1", 18611,
			race->stun_packet, race->stun_len);
	} else {
		race->ice_status = SWITCH_STATUS_TIMEOUT;
	}
	dtls_restart_teardown_race_complete(race);
	return NULL;
}

static void *SWITCH_THREAD_FUNC dtls_restart_teardown_dtls_thread(switch_thread_t *thread, void *obj)
{
	dtls_restart_teardown_race_t *race = obj;
	uint8_t output[64];
	switch_size_t output_len = 0;
	(void)thread;

	if (dtls_restart_teardown_race_wait(race)) {
		race->dtls_status = switch_rtp_pvt_dtls_step(race->rtp, DTLS_TYPE_RTP,
			race->dtls_packet, sizeof(race->dtls_packet), output, sizeof(output), &output_len);
	} else {
		race->dtls_status = SWITCH_STATUS_TIMEOUT;
	}
	dtls_restart_teardown_race_complete(race);
	return NULL;
}

static void *SWITCH_THREAD_FUNC dtls_restart_teardown_identity_thread(switch_thread_t *thread, void *obj)
{
	dtls_restart_teardown_race_t *race = obj;
	switch_rtp_pvt_dtls_identity_t identity;
	(void)thread;

	memset(&identity, 0, sizeof(identity));
	if (dtls_restart_teardown_race_wait(race)) {
		race->identity_status = switch_rtp_pvt_get_dtls_association_identity(race->rtp,
			DTLS_TYPE_RTP, &identity);
		if (race->identity_status == SWITCH_STATUS_SUCCESS) {
			race->preparation_result = switch_rtp_pvt_prepare_dtls_ice_restart(race->rtp,
				&identity, 30000, ICE_VANILLA | ICE_CONTROLLED,
				"raceRemoteUfrag3", "raceLocalUfrag3", "raceLocalPassword1234563",
				"raceRemotePassword1234563");
		}
	} else {
		race->identity_status = SWITCH_STATUS_TIMEOUT;
	}
	dtls_restart_teardown_race_complete(race);
	return NULL;
}

static void *SWITCH_THREAD_FUNC dtls_restart_teardown_thread(switch_thread_t *thread, void *obj)
{
	dtls_restart_teardown_race_t *race = obj;
	(void)thread;

	if (dtls_restart_teardown_race_wait(race)) {
		switch_channel_hangup(switch_core_session_get_channel(race->session),
			SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_media_deactivate_rtp(race->session);
		race->teardown_status = SWITCH_STATUS_SUCCESS;
	} else {
		race->teardown_status = SWITCH_STATUS_TIMEOUT;
	}
	dtls_restart_teardown_race_complete(race);
	return NULL;
}

static switch_status_t dtls_restart_teardown_start_workers(dtls_restart_teardown_race_t *race,
	switch_threadattr_t *thread_attr, switch_memory_pool_t *pool, switch_thread_t *threads[4],
	int fail_at, int *started)
{
	switch_thread_start_t workers[4] = {
		dtls_restart_teardown_ice_thread,
		dtls_restart_teardown_dtls_thread,
		dtls_restart_teardown_identity_thread,
		dtls_restart_teardown_thread
	};
	int i;

	if (!race || !thread_attr || !pool || !threads || !started) {
		return SWITCH_STATUS_FALSE;
	}
	*started = 0;
	for (i = 0; i < 4; ++i) {
		threads[i] = NULL;
		if (i == fail_at || switch_thread_create(&threads[i], thread_attr, workers[i], race, pool) !=
			SWITCH_STATUS_SUCCESS) {
			return SWITCH_STATUS_FALSE;
		}
		(*started)++;
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t dtls_restart_teardown_abort_and_join(dtls_restart_teardown_race_t *race,
	switch_thread_t *threads[4], int started, switch_interval_time_t timeout)
{
	switch_status_t thread_status;
	switch_status_t wait_status = SWITCH_STATUS_SUCCESS;
	switch_time_t deadline;
	switch_interval_time_t remaining;
	int completed;
	int active;
	int i;

	if (!race || !threads || started < 0 || started > 4) {
		return SWITCH_STATUS_FALSE;
	}
	switch_mutex_lock(race->mutex);
	race->abort = 1;
	race->start = 1;
	switch_thread_cond_broadcast(race->cond);
	deadline = switch_micro_time_now() + timeout;
	while (race->completed < started && wait_status != SWITCH_STATUS_TIMEOUT) {
		remaining = deadline - switch_micro_time_now();
		if (remaining <= 0) {
			wait_status = SWITCH_STATUS_TIMEOUT;
			break;
		}
		wait_status = switch_thread_cond_timedwait(race->cond, race->mutex, remaining);
	}
	completed = race->completed;
	active = started - completed;
	switch_mutex_unlock(race->mutex);
	if (completed != started) {
		fprintf(stderr, "fatal cleanup timeout with %d worker(s) still active\n",
			active);
		fflush(stderr);
		_exit(1);
	}
	for (i = 0; i < started; ++i) {
		if (!threads[i] || switch_thread_join(&thread_status, threads[i]) != SWITCH_STATUS_SUCCESS ||
			thread_status != SWITCH_STATUS_SUCCESS) {
			fprintf(stderr, "fatal cleanup join failure for worker %d\n", i);
			fflush(stderr);
			_exit(1);
		}
	}
	return SWITCH_STATUS_SUCCESS;
}

FCT_BGN()
{
	FCT_FIXTURE_SUITE_BGN(switch_trickle_ice)
	{
		switch_memory_pool_t *pool = NULL;
		static switch_memory_pool_t *telnyx_pool = NULL;

		FCT_SETUP_BGN()
		{
			const char *confdir = "conf_trickle"; /* tests/unit/conf_trickle */
			_silence_unused();

			fct_req(switch_core_new_memory_pool(&telnyx_pool) == SWITCH_STATUS_SUCCESS);
			switch_telnyx_init(telnyx_pool);
			fst_init_core_and_modload(confdir, confdir, 0, 0 /* flags */);
			fct_req(switch_core_new_memory_pool(&pool) == SWITCH_STATUS_SUCCESS);
			fct_req(pool != NULL);
			do {
				int sps_total = 10000;
				switch_core_session_ctl(SCSC_SPS, &sps_total);
				switch_sleep(1000000); /* allow softtimer_runtime to apply SPS */
			} while(0);
		}
		FCT_SETUP_END();

		FCT_TEARDOWN_BGN()
		{
			if (pool) { switch_core_destroy_memory_pool(&pool); pool = NULL; }
			switch_telnyx_deinit();
			if (telnyx_pool) { switch_core_destroy_memory_pool(&telnyx_pool); telnyx_pool = NULL; }
		}
		FCT_TEARDOWN_END();

		FCT_TEST_BGN(same_generation_clear_preserves_credentials_and_rearms_unchanged_media)
		{
			switch_core_session_t *session = NULL;
			switch_core_session_t *peer_session = NULL;
			switch_channel_t *channel = NULL;
			switch_media_handle_t *smh = NULL;
			switch_rtp_t *rtp = NULL;
			switch_rtp_t *peer_rtp = NULL;
			switch_status_t status;
			void *sdp_session = NULL;
			void *peer_sdp_session = NULL;
			sdp_parser_t *parser = NULL;
			sdp_parser_t *peer_parser = NULL;
			switch_rtp_pvt_transport_snapshot_t ready_before;
			switch_rtp_pvt_transport_snapshot_t ready_after;
			char initial_ice_user[256] = "";
			char initial_local_pwd[256] = "";
			char initial_remote_pwd[256] = "";
			char rearmed_ice_user[256] = "";
			char rearmed_local_pwd[256] = "";
			char rearmed_remote_pwd[256] = "";
			char answer_local_ufrag[256] = "";
			char answer_local_pwd[256] = "";
			const char *initial_local_ufrag;
			const char *rearmed_local_ufrag;
			const char *local_sdp;
			char peer_sdp[2048];
			switch_bool_t has_addr = SWITCH_FALSE;
			uint8_t match;
			uint8_t proceed = 0;
			const char *same_generation_sdp =
				"v=0\n"
				"o=- 1683118194 1683118197 IN IP4 0.0.0.0\n"
				"s=-\n"
				"t=0 0\n"
				"a=group:BUNDLE 0\n"
				"m=audio 18215 UDP/TLS/RTP/SAVPF 0\n"
				"c=IN IP4 50.114.144.39\n"
				"a=ice-ufrag:aZJpsl00bYnjrOZtkCFMtKhFC/CHAfcv\n"
				"a=ice-pwd:aNniSnLLp43SSsJrz6TNPty1zPrxZNzh\n"
				"a=ice-options:trickle\n"
				"a=candidate:265031753 1 udp 1685921533 50.114.144.39 18215 typ srflx raddr 100.69.211.204 rport 54081\n"
				"a=rtcp-mux\n"
				"a=setup:active\n"
				"a=rtpmap:0 PCMU/8000\n"
				"a=sendrecv\n"
				"a=fingerprint:sha-256 17:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=mid:0\n";

			status = make_session_and_rtp_with_sdp(&session, &rtp, &sdp_session, &parser);
			fst_requires(status == SWITCH_STATUS_SUCCESS && session && rtp && sdp_session && parser);
			channel = switch_core_session_get_channel(session);
			smh = switch_core_session_get_media_handle(session);
			fst_requires(channel != NULL && smh != NULL);

			status = switch_core_media_trickle_remote_candidate_and_recheck(
				session, smh, sdp_session, SDP_TYPE_REQUEST, "0", 0,
				"candidate:265031753 1 udp 1685921533 50.114.144.39 18215 typ srflx raddr 100.69.211.204 rport 54081", 0);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			status = switch_rtp_pvt_get_ice_state(rtp, IPR_RTP,
				initial_ice_user, sizeof(initial_ice_user),
				initial_local_pwd, sizeof(initial_local_pwd),
				initial_remote_pwd, sizeof(initial_remote_pwd), &has_addr);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			initial_local_ufrag = strchr(initial_ice_user, ':');
			fst_requires(initial_local_ufrag != NULL && initial_local_ufrag[1] != '\0');
			initial_local_ufrag++;
			fst_check(has_addr == SWITCH_TRUE);
			fst_requires(build_dtls_pair_sdp(peer_sdp, sizeof(peer_sdp), 1683118198,
				18230, "passive", "sameGenerationPeerUfrag1",
				"sameGenerationPeerPassword1234561") == SWITCH_STATUS_SUCCESS);
			status = make_session_and_rtp_with_sdp_ex(&peer_session, &peer_rtp,
				&peer_sdp_session, &peer_parser, peer_sdp, "PCMU", SWITCH_TRUE,
				SWITCH_TRUE, NULL);
			fst_requires(status == SWITCH_STATUS_SUCCESS && peer_session && peer_rtp);
			fst_requires(drive_dtls_pair_ready(peer_rtp, rtp) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP,
				&ready_before) == SWITCH_STATUS_SUCCESS);
			fst_requires(ready_before.dtls_state == DS_READY && ready_before.dtls_ssl);
			fst_requires(ready_before.srtp_send_ready == SWITCH_TRUE &&
				ready_before.srtp_recv_ready == SWITCH_TRUE);

			switch_core_media_clear_ice(session);
			switch_channel_set_flag(channel, CF_REINVITE);
			match = switch_core_media_negotiate_sdp(session, same_generation_sdp, &proceed, SDP_OFFER);
			fst_requires(match != 0);
			switch_core_media_gen_local_sdp(session, SDP_ANSWER, NULL, 0, NULL, 0);
			local_sdp = switch_channel_get_variable(channel, "rtp_local_sdp_str");
			fst_requires(copy_sdp_attribute(local_sdp, "ice-ufrag", answer_local_ufrag,
				sizeof(answer_local_ufrag)) == SWITCH_STATUS_SUCCESS);
			fst_requires(copy_sdp_attribute(local_sdp, "ice-pwd", answer_local_pwd,
				sizeof(answer_local_pwd)) == SWITCH_STATUS_SUCCESS);
			status = switch_core_media_activate_rtp(session);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			has_addr = SWITCH_FALSE;
			status = switch_rtp_pvt_get_ice_state(rtp, IPR_RTP,
				rearmed_ice_user, sizeof(rearmed_ice_user),
				rearmed_local_pwd, sizeof(rearmed_local_pwd),
				rearmed_remote_pwd, sizeof(rearmed_remote_pwd), &has_addr);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			rearmed_local_ufrag = strchr(rearmed_ice_user, ':');
			fst_requires(rearmed_local_ufrag != NULL && rearmed_local_ufrag[1] != '\0');
			rearmed_local_ufrag++;
			fst_check_string_equals(rearmed_ice_user, initial_ice_user);
			fst_check_string_equals(rearmed_local_pwd, initial_local_pwd);
			fst_check_string_equals(rearmed_remote_pwd, initial_remote_pwd);
			fst_check_string_equals(answer_local_ufrag, initial_local_ufrag);
			fst_check_string_equals(answer_local_pwd, initial_local_pwd);
			fst_check(has_addr == SWITCH_TRUE);
			fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP,
				&ready_after) == SWITCH_STATUS_SUCCESS);
			fst_check(ready_after.dtls_state == DS_READY);
			fst_check(ready_after.dtls_context == ready_before.dtls_context);
			fst_check(ready_after.dtls_ssl == ready_before.dtls_ssl);
			fst_check(ready_after.dtls_read_bio == ready_before.dtls_read_bio);
			fst_check(ready_after.dtls_write_bio == ready_before.dtls_write_bio);
			fst_check(ready_after.dtls_destroy_count == ready_before.dtls_destroy_count);
			fst_check(ready_after.srtp_send_ready == SWITCH_TRUE);
			fst_check(ready_after.srtp_recv_ready == SWITCH_TRUE);
			fst_check(switch_rtp_pvt_srtp_round_trip(rtp, peer_rtp) == SWITCH_STATUS_SUCCESS);

			cleanup_session_media_and_sdp(session, sdp_session, parser);
			cleanup_session_media_and_sdp(peer_session, peer_sdp_session, peer_parser);
		}
		FCT_TEST_END();

		FCT_TEST_BGN(ice_restart_waits_for_authenticated_nomination_and_preserves_inflight_dtls)
		{
			switch_core_session_t *session = NULL;
			switch_channel_t *channel = NULL;
			switch_rtp_t *rtp = NULL;
			switch_status_t status;
			switch_rtp_pvt_transport_snapshot_t before;
			switch_rtp_pvt_transport_snapshot_t after_sdp;
			switch_rtp_pvt_transport_snapshot_t after_nomination;
			void *sdp_session = NULL;
			sdp_parser_t *parser = NULL;
			char ice_user[513] = "";
			char incoming_user[513] = "";
			char local_pwd[256] = "";
			char remote_pwd[256] = "";
			char initial_ice_user[513] = "";
			char initial_incoming_user[513] = "";
			char initial_local_pwd[256] = "";
			char initial_remote_pwd[256] = "";
			char nomination_id[13] = "";
			char pending_nomination_id[13] = "";
			char invalid_nomination_id[13] = "badresponse!";
			char before_host[80] = "";
			char after_sdp_host[80] = "";
			char after_nomination_host[80] = "";
			switch_port_t before_port = 0;
			switch_port_t after_sdp_port = 0;
			switch_port_t after_nomination_port = 0;
			switch_bool_t has_addr = SWITCH_FALSE;
			uint8_t stun_packet[512];
			uint8_t valid_nomination_packet[512];
			switch_size_t stun_len;
			switch_size_t valid_nomination_len;
			uint8_t match;
			uint8_t proceed = 0;
			int role_case;
			const char *ice_role;
			const char *initial_sdp =
				"v=0\n"
				"o=- 1683118194 1683118195 IN IP4 0.0.0.0\n"
				"s=-\n"
				"t=0 0\n"
				"a=group:BUNDLE 0\n"
				"m=audio 18215 UDP/TLS/RTP/SAVPF 0\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:initialRemoteUfrag\n"
				"a=ice-pwd:initialRemotePassword123456\n"
				"a=ice-options:trickle\n"
				"a=candidate:1 1 udp 2130706431 127.0.0.1 18215 typ host\n"
				"a=rtcp-mux\n"
				"a=setup:active\n"
				"a=rtpmap:0 PCMU/8000\n"
				"a=sendrecv\n"
				"a=fingerprint:sha-256 17:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=mid:0\n";
			const char *restart_sdp =
				"v=0\n"
				"o=- 1683118194 1683118196 IN IP4 0.0.0.0\n"
				"s=-\n"
				"t=0 0\n"
				"a=group:BUNDLE 0\n"
				"m=audio 18216 UDP/TLS/RTP/SAVPF 0\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:restartRemoteUfrag\n"
				"a=ice-pwd:restartRemotePassword123456\n"
				"a=ice-options:trickle\n"
				"a=candidate:2 1 udp 2130706430 127.0.0.1 18216 typ host\n"
				"a=rtcp-mux\n"
				"a=setup:active\n"
				"a=rtpmap:0 PCMU/8000\n"
				"a=sendrecv\n"
				"a=fingerprint:sha-256 17:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=mid:0\n";

			for (role_case = 0; role_case < 2; ++role_case) {
				ice_role = role_case ? "controlled" : NULL;
				session = NULL;
				channel = NULL;
				rtp = NULL;
				sdp_session = NULL;
				parser = NULL;
				memset(&before, 0, sizeof(before));
				memset(&after_sdp, 0, sizeof(after_sdp));
				memset(&after_nomination, 0, sizeof(after_nomination));
				memset(ice_user, 0, sizeof(ice_user));
				memset(incoming_user, 0, sizeof(incoming_user));
				memset(local_pwd, 0, sizeof(local_pwd));
				memset(remote_pwd, 0, sizeof(remote_pwd));
				memset(initial_ice_user, 0, sizeof(initial_ice_user));
				memset(initial_incoming_user, 0, sizeof(initial_incoming_user));
				memset(initial_local_pwd, 0, sizeof(initial_local_pwd));
				memset(initial_remote_pwd, 0, sizeof(initial_remote_pwd));
				memset(nomination_id, 0, sizeof(nomination_id));
				memset(pending_nomination_id, 0, sizeof(pending_nomination_id));
				memset(valid_nomination_packet, 0, sizeof(valid_nomination_packet));
				valid_nomination_len = 0;
				proceed = 0;
				has_addr = SWITCH_FALSE;

				status = make_session_and_rtp_with_sdp_ex(&session, &rtp, &sdp_session, &parser,
					initial_sdp, "PCMU", SWITCH_TRUE, SWITCH_TRUE, ice_role);
				fst_requires(status == SWITCH_STATUS_SUCCESS && session && rtp && sdp_session && parser);
				channel = switch_core_session_get_channel(session);
				fst_requires(channel != NULL);
				status = switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &before);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				fst_requires(before.dtls_state == DS_HANDSHAKE);
				fst_requires(before.dtls_context != NULL && before.dtls_ssl != NULL);
				status = copy_sockaddr_tuple(switch_rtp_session_get_remote_addr(rtp), before_host,
					sizeof(before_host), &before_port);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				fst_check_string_equals(before_host, "127.0.0.1");
				fst_check(before_port == 18215);
				status = switch_rtp_pvt_get_ice_state(rtp, IPR_RTP,
					initial_ice_user, sizeof(initial_ice_user), initial_local_pwd,
					sizeof(initial_local_pwd), initial_remote_pwd, sizeof(initial_remote_pwd),
					&has_addr);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				fst_requires(reverse_ice_username(initial_ice_user, initial_incoming_user,
					sizeof(initial_incoming_user)) == SWITCH_STATUS_SUCCESS);

				switch_core_media_clear_ice(session);
				switch_channel_set_flag(channel, CF_REINVITE);
				match = switch_core_media_negotiate_sdp(session, restart_sdp, &proceed, SDP_OFFER);
				fst_requires(match != 0);
				switch_core_media_gen_local_sdp(session, SDP_ANSWER, NULL, 0, NULL, 0);
				status = switch_core_media_activate_rtp(session);
				fst_requires(status == SWITCH_STATUS_SUCCESS);

				status = switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &after_sdp);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				fst_check(after_sdp.dtls_state == DS_HANDSHAKE);
				fst_check(after_sdp.dtls_context == before.dtls_context);
				fst_check(after_sdp.dtls_ssl == before.dtls_ssl);
				fst_check(after_sdp.dtls_restart_pending == SWITCH_TRUE);
				fst_check(after_sdp.dtls_restart_migrated == SWITCH_FALSE);
				fst_check(after_sdp.dtls_association_started_us == before.dtls_association_started_us);
				fst_check(after_sdp.dtls_restart_deadline_us > after_sdp.dtls_association_started_us);
				status = copy_sockaddr_tuple(switch_rtp_session_get_remote_addr(rtp), after_sdp_host,
					sizeof(after_sdp_host), &after_sdp_port);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				fst_check_string_equals(after_sdp_host, before_host);
				fst_check(after_sdp_port == before_port);

				status = switch_rtp_pvt_get_ice_state(rtp, IPR_RTP,
					ice_user, sizeof(ice_user), local_pwd, sizeof(local_pwd),
					remote_pwd, sizeof(remote_pwd), &has_addr);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				fst_requires(reverse_ice_username(ice_user, incoming_user,
					sizeof(incoming_user)) == SWITCH_STATUS_SUCCESS);
				fst_check(!strncmp(ice_user, "restartRemoteUfrag:",
					strlen("restartRemoteUfrag:")));
				fst_check_string_equals(remote_pwd, "restartRemotePassword123456");

				if (after_sdp.ice_type & ICE_CONTROLLED) {
					stun_len = build_authenticated_ice_request(stun_packet, sizeof(stun_packet),
						initial_incoming_user, initial_local_pwd);
					status = switch_rtp_pvt_handle_ice_from(rtp, IPR_RTP, "127.0.0.1", 18216,
						stun_packet, stun_len);
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					stun_len = build_unauthenticated_ice_request(stun_packet, sizeof(stun_packet),
						incoming_user, SWITCH_TRUE);
					status = switch_rtp_pvt_handle_ice_from(rtp, IPR_RTP, "127.0.0.1", 18216,
						stun_packet, stun_len);
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					stun_len = build_authenticated_ice_request(stun_packet, sizeof(stun_packet),
						incoming_user, "wrong-current-generation-password");
					status = switch_rtp_pvt_handle_ice_from(rtp, IPR_RTP, "127.0.0.1", 18216,
						stun_packet, stun_len);
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					status = switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &after_nomination);
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					fst_check(after_nomination.dtls_context == before.dtls_context);
					fst_check(after_nomination.dtls_ssl == before.dtls_ssl);
					fst_check(after_nomination.dtls_restart_pending == SWITCH_TRUE);
					fst_check(after_nomination.dtls_restart_migrated == SWITCH_FALSE);
					status = copy_sockaddr_tuple(switch_rtp_session_get_remote_addr(rtp), after_nomination_host,
						sizeof(after_nomination_host), &after_nomination_port);
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					fst_check(after_nomination_port == before_port);

					stun_len = build_authenticated_ice_request_ex(stun_packet, sizeof(stun_packet),
						incoming_user, local_pwd, SWITCH_FALSE);
					status = switch_rtp_pvt_handle_ice_from(rtp, IPR_RTP, "127.0.0.1", 18216,
						stun_packet, stun_len);
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					status = switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &after_nomination);
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					fst_check(after_nomination.dtls_restart_pending == SWITCH_TRUE);
					fst_check(after_nomination.dtls_restart_migrated == SWITCH_FALSE);
					status = copy_sockaddr_tuple(switch_rtp_session_get_remote_addr(rtp), after_nomination_host,
						sizeof(after_nomination_host), &after_nomination_port);
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					fst_check(after_nomination_port == before_port);
				} else {
					stun_len = build_authenticated_ice_request_role(stun_packet,
						sizeof(stun_packet), incoming_user, local_pwd, SWITCH_TRUE, SWITCH_TRUE);
					status = switch_rtp_pvt_handle_ice_from(rtp, IPR_RTP, "127.0.0.1", 18216,
						stun_packet, stun_len);
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					status = switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &after_nomination);
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					fst_check(after_nomination.dtls_restart_pending == SWITCH_TRUE);
					fst_check(after_nomination.dtls_restart_migrated == SWITCH_FALSE);
					status = copy_sockaddr_tuple(switch_rtp_session_get_remote_addr(rtp),
						after_nomination_host, sizeof(after_nomination_host), &after_nomination_port);
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					fst_check(after_nomination_port == before_port);
				}

				stun_len = build_authenticated_ice_request_role(stun_packet, sizeof(stun_packet),
					incoming_user, local_pwd, SWITCH_TRUE,
					(after_sdp.ice_type & ICE_CONTROLLED) ? SWITCH_FALSE : SWITCH_TRUE);
				status = switch_rtp_pvt_handle_ice_from(rtp, IPR_RTP, "192.0.2.11", 18219,
					stun_packet, stun_len);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				stun_len = build_authenticated_ice_request_full(stun_packet, sizeof(stun_packet),
					incoming_user, local_pwd, SWITCH_TRUE,
					(after_sdp.ice_type & ICE_CONTROLLED) ? SWITCH_TRUE : SWITCH_FALSE,
					SWITCH_FALSE);
				status = switch_rtp_pvt_handle_ice_from(rtp, IPR_RTP, "192.0.2.12", 18220,
					stun_packet, stun_len);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				stun_len = build_authenticated_ice_request_role(stun_packet, sizeof(stun_packet),
					incoming_user, local_pwd, SWITCH_TRUE,
					(after_sdp.ice_type & ICE_CONTROLLED) ? SWITCH_TRUE : SWITCH_FALSE);
				stun_packet[stun_len - 1] ^= 0x01;
				status = switch_rtp_pvt_handle_ice_from(rtp, IPR_RTP, "192.0.2.13", 18221,
					stun_packet, stun_len);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				memset(stun_packet, 0, 8);
				status = switch_rtp_pvt_handle_ice_from(rtp, IPR_RTP, "192.0.2.14", 18222,
					stun_packet, 8);
				fst_check(status == SWITCH_STATUS_SUCCESS || status == SWITCH_STATUS_FALSE);
				stun_len = build_authenticated_ice_request_zero_length_role(stun_packet,
					sizeof(stun_packet), incoming_user, local_pwd);
				status = switch_rtp_pvt_handle_ice_from(rtp, IPR_RTP, "127.0.0.1", 18216,
					stun_packet, stun_len);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				status = switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &after_nomination);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				fst_check(after_nomination.dtls_context == before.dtls_context);
				fst_check(after_nomination.dtls_ssl == before.dtls_ssl);
				fst_check(after_nomination.dtls_restart_pending == SWITCH_TRUE);
				fst_check(after_nomination.dtls_restart_migrated == SWITCH_FALSE);
				status = copy_sockaddr_tuple(switch_rtp_session_get_remote_addr(rtp),
					after_nomination_host, sizeof(after_nomination_host), &after_nomination_port);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				fst_check(after_nomination_port == before_port);

				if (after_sdp.ice_type & ICE_CONTROLLED) {
					stun_len = build_authenticated_ice_request(stun_packet, sizeof(stun_packet),
						incoming_user, local_pwd);
					switch_copy_string(nomination_id, "controlled", sizeof(nomination_id));
				} else {
					status = switch_rtp_pvt_get_controlling_nomination_id(rtp, IPR_RTP,
						nomination_id, sizeof(nomination_id));
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					stun_len = build_authenticated_ice_response(stun_packet, sizeof(stun_packet),
						nomination_id, "wrong-current-generation-password", "127.0.0.1", 18216);
					status = switch_rtp_pvt_handle_ice_from(rtp, IPR_RTP, "127.0.0.1", 18216,
						stun_packet, stun_len);
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					stun_len = build_unauthenticated_ice_response(stun_packet, sizeof(stun_packet),
						nomination_id, "127.0.0.1", 18216);
					status = switch_rtp_pvt_handle_ice_from(rtp, IPR_RTP, "127.0.0.1", 18216,
						stun_packet, stun_len);
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					stun_len = build_authenticated_ice_response(stun_packet, sizeof(stun_packet),
						nomination_id, remote_pwd, "127.0.0.1", 18216);
					status = switch_rtp_pvt_handle_ice_from(rtp, IPR_RTP, "192.0.2.50", 18218,
						stun_packet, stun_len);
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					stun_len = build_authenticated_ice_response(stun_packet, sizeof(stun_packet),
						invalid_nomination_id, remote_pwd, "127.0.0.1", 18216);
					status = switch_rtp_pvt_handle_ice_from(rtp, IPR_RTP, "127.0.0.1", 18216,
						stun_packet, stun_len);
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					status = switch_rtp_pvt_get_controlling_nomination_id(rtp, IPR_RTP,
						pending_nomination_id, sizeof(pending_nomination_id));
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					fst_check(!memcmp(pending_nomination_id, nomination_id, 12));
					status = switch_rtp_pvt_ice_timer_tick(rtp, IPR_RTP,
						switch_micro_time_now() + 1100000);
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					memset(pending_nomination_id, 0, sizeof(pending_nomination_id));
					status = switch_rtp_pvt_get_controlling_nomination_id(rtp, IPR_RTP,
						pending_nomination_id, sizeof(pending_nomination_id));
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					fst_check(!memcmp(pending_nomination_id, nomination_id, 12));
					stun_len = build_authenticated_ice_response(stun_packet, sizeof(stun_packet),
						nomination_id, remote_pwd, "127.0.0.1", 18216);
				}
				memcpy(valid_nomination_packet, stun_packet, stun_len);
				valid_nomination_len = stun_len;

				status = switch_rtp_pvt_handle_ice_from(rtp, IPR_RTP,
					(after_sdp.ice_type & ICE_CONTROLLED) ? "192.0.2.10" : "127.0.0.1",
					(after_sdp.ice_type & ICE_CONTROLLED) ? 18217 : 18216,
					stun_packet, stun_len);
				fst_requires(status == SWITCH_STATUS_SUCCESS);

				status = switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &after_nomination);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				fst_check(after_nomination.ice_ready == SWITCH_TRUE);
				fst_check(after_nomination.ice_rready == SWITCH_TRUE);
				fst_check(after_nomination.rtp_chosen == SWITCH_TRUE);
				fst_check(after_nomination.dtls_state == DS_HANDSHAKE);
				fst_check(after_nomination.dtls_context == before.dtls_context);
				fst_check(after_nomination.dtls_ssl == before.dtls_ssl);
				fst_check(after_nomination.dtls_restart_pending == SWITCH_FALSE);
				fst_check(after_nomination.dtls_restart_migrated == SWITCH_TRUE);
				fst_check(after_nomination.dtls_association_started_us == before.dtls_association_started_us);
				fst_check(after_nomination.dtls_restart_deadline_us == after_sdp.dtls_restart_deadline_us);
				status = copy_sockaddr_tuple(switch_rtp_session_get_remote_addr(rtp), after_nomination_host,
					sizeof(after_nomination_host), &after_nomination_port);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				fst_check_string_equals(after_nomination_host,
					(after_sdp.ice_type & ICE_CONTROLLED) ? "192.0.2.10" : "127.0.0.1");
				fst_check(after_nomination_port == ((after_sdp.ice_type & ICE_CONTROLLED) ? 18217 : 18216));

				stun_len = build_authenticated_ice_request_role(stun_packet, sizeof(stun_packet),
					initial_incoming_user, initial_local_pwd, SWITCH_TRUE, SWITCH_TRUE);
				status = switch_rtp_pvt_handle_ice_from(rtp, IPR_RTP, "127.0.0.1", before_port,
					stun_packet, stun_len);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				status = switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &after_nomination);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				fst_check(after_nomination.dtls_context == before.dtls_context);
				fst_check(after_nomination.dtls_ssl == before.dtls_ssl);
				fst_check(after_nomination.dtls_restart_pending == SWITCH_FALSE);
				fst_check(after_nomination.dtls_restart_migrated == SWITCH_TRUE);
				status = copy_sockaddr_tuple(switch_rtp_session_get_remote_addr(rtp),
					after_nomination_host, sizeof(after_nomination_host), &after_nomination_port);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				fst_check_string_equals(after_nomination_host,
					(after_sdp.ice_type & ICE_CONTROLLED) ? "192.0.2.10" : "127.0.0.1");
				fst_check(after_nomination_port == ((after_sdp.ice_type & ICE_CONTROLLED) ? 18217 : 18216));

				memcpy(stun_packet, valid_nomination_packet, valid_nomination_len);
				stun_len = valid_nomination_len;
				status = switch_rtp_pvt_handle_ice_from(rtp, IPR_RTP,
					(after_sdp.ice_type & ICE_CONTROLLED) ? "192.0.2.10" : "127.0.0.1",
					(after_sdp.ice_type & ICE_CONTROLLED) ? 18217 : 18216,
					stun_packet, stun_len);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				status = switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &after_nomination);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				fst_check(after_nomination.dtls_context == before.dtls_context);
				fst_check(after_nomination.dtls_ssl == before.dtls_ssl);
				fst_check(after_nomination.dtls_read_bio == before.dtls_read_bio);
				fst_check(after_nomination.dtls_write_bio == before.dtls_write_bio);
				fst_check(after_nomination.dtls_association_started_us == before.dtls_association_started_us);
				fst_check(after_nomination.dtls_restart_pending == SWITCH_FALSE);
				fst_check(after_nomination.dtls_restart_migrated == SWITCH_TRUE);
				fst_check(after_nomination.dtls_restart_deadline_us == after_sdp.dtls_restart_deadline_us);
				status = copy_sockaddr_tuple(switch_rtp_session_get_remote_addr(rtp), after_nomination_host,
					sizeof(after_nomination_host), &after_nomination_port);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				fst_check(after_nomination_port == ((after_sdp.ice_type & ICE_CONTROLLED) ? 18217 : 18216));

				cleanup_session_media_and_sdp(session, sdp_session, parser);
			}
		}
		FCT_TEST_END();

		FCT_TEST_BGN(ice_restart_nomination_before_activation_remains_transport_authoritative)
		{
			switch_core_session_t *session = NULL;
			switch_rtp_t *rtp = NULL;
			void *sdp_session = NULL;
			sdp_parser_t *parser = NULL;
			switch_rtp_pvt_transport_snapshot_t before;
			switch_rtp_pvt_transport_snapshot_t nominated;
			switch_rtp_pvt_transport_snapshot_t activated;
			char initial_sdp[2048];
			char restart_sdp[2048];
			char remote_host[80] = "";
			switch_port_t remote_port = 0;
			switch_status_t status;

			fst_requires(build_dtls_pair_sdp(initial_sdp, sizeof(initial_sdp),
				1683118198, 18218, "active", "preActivateRemoteUfrag1",
				"preActivateRemotePassword1234561") == SWITCH_STATUS_SUCCESS);
			fst_requires(build_dtls_pair_sdp(restart_sdp, sizeof(restart_sdp),
				1683118199, 18219, "active", "preActivateRemoteUfrag2",
				"preActivateRemotePassword1234562") == SWITCH_STATUS_SUCCESS);
			status = make_session_and_rtp_with_sdp_ex(&session, &rtp, &sdp_session, &parser,
				initial_sdp, "PCMU", SWITCH_TRUE, SWITCH_TRUE, "controlled");
			fst_requires(status == SWITCH_STATUS_SUCCESS && session && rtp);
			fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP,
				&before) == SWITCH_STATUS_SUCCESS);
			fst_requires(before.dtls_state == DS_HANDSHAKE);

			status = restart_nominate_then_activate_controlled_dtls(session, rtp,
				restart_sdp, 18219, &nominated, &activated);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check(nominated.dtls_context == before.dtls_context);
			fst_check(nominated.dtls_ssl == before.dtls_ssl);
			fst_check(nominated.dtls_restart_pending == SWITCH_FALSE);
			fst_check(nominated.dtls_restart_migrated == SWITCH_TRUE);
			fst_check(activated.dtls_context == before.dtls_context);
			fst_check(activated.dtls_ssl == before.dtls_ssl);
			fst_check(activated.dtls_restart_pending == SWITCH_FALSE);
			fst_check(activated.dtls_restart_migrated == SWITCH_TRUE);
			fst_check(activated.dtls_restart_deadline_us == nominated.dtls_restart_deadline_us);
			fst_requires(copy_sockaddr_tuple(switch_rtp_session_get_remote_addr(rtp),
				remote_host, sizeof(remote_host), &remote_port) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(remote_host, "127.0.0.1");
			fst_check(remote_port == 18219);

			cleanup_session_media_and_sdp(session, sdp_session, parser);
		}
		FCT_TEST_END();

		FCT_TEST_BGN(preserved_dtls_terminal_openssl_failure_fails_closed)
		{
			switch_core_session_t *session = NULL;
			switch_rtp_t *rtp = NULL;
			void *sdp_session = NULL;
			sdp_parser_t *parser = NULL;
			switch_rtp_pvt_transport_snapshot_t migrated;
			switch_rtp_pvt_transport_snapshot_t failed;
			char initial_sdp[2048];
			char restart_sdp[2048];
			const uint8_t fatal_alert[] = {
				0x15, 0xfe, 0xfd, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x28
			};
			switch_status_t status;

			fst_requires(build_dtls_pair_sdp(initial_sdp, sizeof(initial_sdp),
				1683118204, 18228, "active", "fatalRemoteUfrag1",
				"fatalRemotePassword1234561") == SWITCH_STATUS_SUCCESS);
			fst_requires(build_dtls_pair_sdp(restart_sdp, sizeof(restart_sdp),
				1683118205, 18229, "active", "fatalRemoteUfrag2",
				"fatalRemotePassword1234562") == SWITCH_STATUS_SUCCESS);
			status = make_session_and_rtp_with_sdp_ex(&session, &rtp, &sdp_session, &parser,
				initial_sdp, "PCMU", SWITCH_TRUE, SWITCH_TRUE, "controlled");
			fst_requires(status == SWITCH_STATUS_SUCCESS && session && rtp);
			status = restart_and_nominate_controlled_dtls(session, rtp, restart_sdp, 18229,
				&migrated);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_requires(migrated.dtls_state == DS_HANDSHAKE);
			fst_requires(migrated.dtls_restart_migrated == SWITCH_TRUE);

			status = switch_rtp_pvt_dtls_input_from(rtp, DTLS_TYPE_RTP, "127.0.0.1",
				18229, fatal_alert, sizeof(fatal_alert));
			fst_check(status == SWITCH_STATUS_FALSE);
			fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP,
				&failed) == SWITCH_STATUS_SUCCESS);
			fst_check(failed.dtls_state == DS_FAIL);
			fst_check(failed.dtls_restart_pending == SWITCH_FALSE);
			fst_check(failed.dtls_restart_migrated == SWITCH_FALSE);
			fst_check(failed.dtls_restart_deadline_us == 0);

			cleanup_session_media_and_sdp(session, sdp_session, parser);
		}
		FCT_TEST_END();

		FCT_TEST_BGN(ice_restart_preserves_real_dtls_flights_through_ready_and_srtp)
		{
			switch_core_session_t *target_session = NULL;
			switch_core_session_t *peer_session = NULL;
			switch_rtp_t *target_rtp = NULL;
			switch_rtp_t *peer_rtp = NULL;
			void *target_sdp_session = NULL;
			void *peer_sdp_session = NULL;
			sdp_parser_t *target_parser = NULL;
			sdp_parser_t *peer_parser = NULL;
			switch_channel_t *target_channel = NULL;
			switch_rtp_pvt_transport_snapshot_t target_before;
			switch_rtp_pvt_transport_snapshot_t target_migrated;
			switch_rtp_pvt_transport_snapshot_t target_repeated;
			switch_rtp_pvt_transport_snapshot_t target_after;
			switch_rtp_pvt_transport_snapshot_t target_failover;
			switch_rtp_pvt_transport_snapshot_t target_ready_restart;
			switch_rtp_pvt_transport_snapshot_t peer_after;
			int replay_sink_fd = -1;
			uint8_t target_to_peer[32768];
			uint8_t peer_to_target[32768];
			uint8_t duplicate_peer_flight[32768];
			uint8_t original_target_flight[32768];
			uint8_t replayed_target_flight[32768];
			uint8_t timeout_target_flight[32768];
			switch_size_t target_to_peer_len;
			switch_size_t peer_to_target_len;
			switch_size_t duplicate_peer_flight_len;
			switch_size_t original_target_flight_len;
			switch_size_t replayed_target_flight_len;
			switch_size_t timeout_target_flight_len;
			char target_sdp[2048];
			char peer_sdp[2048];
			char restart_sdp[2048];
			char ready_restart_sdp[2048];
			char ice_user[513];
			char incoming_user[513];
			char local_pwd[256];
			char remote_pwd[256];
			uint8_t stun_packet[512];
			switch_size_t stun_len;
			switch_bool_t has_addr;
			const char *target_setup;
			const char *peer_setup;
			switch_port_t target_port;
			switch_port_t peer_port;
			switch_port_t restart_port;
			switch_port_t failover_port;
			switch_status_t status;
			int role_case;
			int recovery_case;
			int step;
			switch_time_t old_restart_deadline;
			switch_time_t edge_now;
			switch_time_t edge_wait;
			uint8_t match;
			uint8_t proceed;

			for (role_case = 0; role_case < 2; ++role_case) {
				for (recovery_case = 0; recovery_case < 2; ++recovery_case) {
				target_session = NULL;
				peer_session = NULL;
				target_rtp = NULL;
				peer_rtp = NULL;
				target_sdp_session = NULL;
				peer_sdp_session = NULL;
				target_parser = NULL;
				peer_parser = NULL;
				target_channel = NULL;
				replay_sink_fd = -1;
				memset(&target_before, 0, sizeof(target_before));
				memset(&target_migrated, 0, sizeof(target_migrated));
				memset(&target_repeated, 0, sizeof(target_repeated));
				memset(&target_after, 0, sizeof(target_after));
				memset(&target_failover, 0, sizeof(target_failover));
				memset(&target_ready_restart, 0, sizeof(target_ready_restart));
				memset(&peer_after, 0, sizeof(peer_after));
				memset(target_to_peer, 0, sizeof(target_to_peer));
				memset(peer_to_target, 0, sizeof(peer_to_target));
				memset(duplicate_peer_flight, 0, sizeof(duplicate_peer_flight));
				memset(original_target_flight, 0, sizeof(original_target_flight));
				memset(replayed_target_flight, 0, sizeof(replayed_target_flight));
				memset(timeout_target_flight, 0, sizeof(timeout_target_flight));
				memset(ice_user, 0, sizeof(ice_user));
				memset(incoming_user, 0, sizeof(incoming_user));
				memset(local_pwd, 0, sizeof(local_pwd));
				memset(remote_pwd, 0, sizeof(remote_pwd));
				target_to_peer_len = 0;
				peer_to_target_len = 0;
				duplicate_peer_flight_len = 0;
				original_target_flight_len = 0;
				replayed_target_flight_len = 0;
				timeout_target_flight_len = 0;
				target_setup = role_case == 0 ? "active" : "passive";
				peer_setup = role_case == 0 ? "passive" : "active";
				target_port = (switch_port_t)(18310 + (role_case * 20) + (recovery_case * 2));
				peer_port = (switch_port_t)(18410 + (role_case * 20) + (recovery_case * 2));
				restart_port = (switch_port_t)(target_port + 1);
				failover_port = (switch_port_t)(target_port + 2);
				proceed = 0;
				has_addr = SWITCH_FALSE;
				old_restart_deadline = 0;
				edge_now = 0;
				edge_wait = 0;

				fst_requires(build_dtls_pair_sdp(target_sdp, sizeof(target_sdp),
					1683118200 + (role_case * 10) + recovery_case,
					target_port, target_setup, role_case ? "targetClientUfrag1" : "targetServerUfrag1",
					role_case ? "targetClientPassword1234561" : "targetServerPassword1234561") == SWITCH_STATUS_SUCCESS);
				fst_requires(build_dtls_pair_sdp(peer_sdp, sizeof(peer_sdp),
					1683118300 + (role_case * 10) + recovery_case,
					peer_port, peer_setup, role_case ? "peerServerUfrag1" : "peerClientUfrag1",
					role_case ? "peerServerPassword1234561" : "peerClientPassword1234561") == SWITCH_STATUS_SUCCESS);
				fst_requires(build_dtls_pair_sdp(restart_sdp, sizeof(restart_sdp),
					1683118400 + (role_case * 10) + recovery_case,
					restart_port, target_setup, role_case ? "targetClientUfrag2" : "targetServerUfrag2",
					role_case ? "targetClientPassword1234562" : "targetServerPassword1234562") == SWITCH_STATUS_SUCCESS);
				fst_requires(add_rtp_candidate_to_sdp(restart_sdp, sizeof(restart_sdp), 3,
					2130706429, failover_port) == SWITCH_STATUS_SUCCESS);
				fst_requires(build_dtls_pair_sdp(ready_restart_sdp, sizeof(ready_restart_sdp),
					1683118500 + (role_case * 10) + recovery_case,
					(switch_port_t)(failover_port + 1), target_setup,
					role_case ? "targetClientUfrag3" : "targetServerUfrag3",
					role_case ? "targetClientPassword1234563" : "targetServerPassword1234563") == SWITCH_STATUS_SUCCESS);
				replay_sink_fd = bind_udp_sink_fd(restart_port);
				fst_requires(replay_sink_fd >= 0);

				status = make_session_and_rtp_with_sdp_ex(&target_session, &target_rtp,
					&target_sdp_session, &target_parser, target_sdp, "PCMU", SWITCH_TRUE,
					SWITCH_TRUE, "controlled");
				fst_requires(status == SWITCH_STATUS_SUCCESS && target_session && target_rtp);
				target_channel = switch_core_session_get_channel(target_session);
				fst_requires(target_channel != NULL);
				status = make_session_and_rtp_with_sdp_ex(&peer_session, &peer_rtp,
					&peer_sdp_session, &peer_parser, peer_sdp, "PCMU", SWITCH_TRUE,
					SWITCH_TRUE, NULL);
				fst_requires(status == SWITCH_STATUS_SUCCESS && peer_session && peer_rtp);

				if (role_case == 0) {
					status = switch_rtp_pvt_dtls_step(peer_rtp, DTLS_TYPE_RTP, NULL, 0,
						peer_to_target, sizeof(peer_to_target), &peer_to_target_len);
					fst_requires(status == SWITCH_STATUS_SUCCESS && peer_to_target_len > 0);
					memcpy(duplicate_peer_flight, peer_to_target, peer_to_target_len);
					duplicate_peer_flight_len = peer_to_target_len;
					status = switch_rtp_pvt_dtls_step(target_rtp, DTLS_TYPE_RTP,
						peer_to_target, peer_to_target_len, target_to_peer, sizeof(target_to_peer),
						&target_to_peer_len);
					fst_requires(status == SWITCH_STATUS_SUCCESS && target_to_peer_len > 0);
					memcpy(original_target_flight, target_to_peer, target_to_peer_len);
					original_target_flight_len = target_to_peer_len;
					if (recovery_case == 1) {
						peer_to_target_len = 0;
						status = switch_rtp_pvt_dtls_step(peer_rtp, DTLS_TYPE_RTP,
							target_to_peer, target_to_peer_len, peer_to_target,
							sizeof(peer_to_target), &peer_to_target_len);
						fst_requires(status == SWITCH_STATUS_SUCCESS && peer_to_target_len > 0);
						memcpy(duplicate_peer_flight, peer_to_target, peer_to_target_len);
						duplicate_peer_flight_len = peer_to_target_len;
					}
				} else {
					status = switch_rtp_pvt_dtls_step(target_rtp, DTLS_TYPE_RTP, NULL, 0,
						target_to_peer, sizeof(target_to_peer), &target_to_peer_len);
					fst_requires(status == SWITCH_STATUS_SUCCESS && target_to_peer_len > 0);
					status = switch_rtp_pvt_dtls_step(peer_rtp, DTLS_TYPE_RTP,
						target_to_peer, target_to_peer_len, peer_to_target, sizeof(peer_to_target),
						&peer_to_target_len);
					fst_requires(status == SWITCH_STATUS_SUCCESS && peer_to_target_len > 0);
					memcpy(duplicate_peer_flight, peer_to_target, peer_to_target_len);
					duplicate_peer_flight_len = peer_to_target_len;
					target_to_peer_len = 0;
					status = switch_rtp_pvt_dtls_step(target_rtp, DTLS_TYPE_RTP,
						peer_to_target, peer_to_target_len, target_to_peer, sizeof(target_to_peer),
						&target_to_peer_len);
					fst_requires(status == SWITCH_STATUS_SUCCESS && target_to_peer_len > 0);
					memcpy(original_target_flight, target_to_peer, target_to_peer_len);
					original_target_flight_len = target_to_peer_len;
					if (recovery_case == 1) {
						peer_to_target_len = 0;
						status = switch_rtp_pvt_dtls_step(peer_rtp, DTLS_TYPE_RTP,
							target_to_peer, target_to_peer_len, peer_to_target,
							sizeof(peer_to_target), &peer_to_target_len);
						fst_requires(status == SWITCH_STATUS_SUCCESS && peer_to_target_len > 0);
						memcpy(duplicate_peer_flight, peer_to_target, peer_to_target_len);
						duplicate_peer_flight_len = peer_to_target_len;
					}
				}

				status = switch_rtp_pvt_get_transport_snapshot(target_rtp, IPR_RTP, &target_before);
				fst_requires(status == SWITCH_STATUS_SUCCESS && target_before.dtls_state == DS_HANDSHAKE);
				fst_requires(target_before.dtls_context && target_before.dtls_ssl &&
					target_before.dtls_read_bio && target_before.dtls_write_bio);
				if (role_case == 0 && recovery_case == 0) {
					switch_channel_set_variable(target_channel, "media_dtls_setup_timeout", "5000");
					switch_rtp_session_set_dtls_checks_started(target_rtp,
						switch_micro_time_now() - 1500000);
				}
				status = restart_and_nominate_controlled_dtls(target_session, target_rtp,
					restart_sdp, restart_port, &target_migrated);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				fst_check(target_migrated.dtls_context == target_before.dtls_context);
				fst_check(target_migrated.dtls_ssl == target_before.dtls_ssl);
				fst_check(target_migrated.dtls_read_bio == target_before.dtls_read_bio);
				fst_check(target_migrated.dtls_write_bio == target_before.dtls_write_bio);
				fst_check(target_migrated.dtls_association_started_us ==
					target_before.dtls_association_started_us);
				fst_check(target_migrated.dtls_restart_deadline_us >
					target_migrated.dtls_association_started_us);
				old_restart_deadline = target_migrated.dtls_restart_deadline_us;
				fst_requires(switch_rtp_pvt_get_ice_state(target_rtp, IPR_RTP,
					ice_user, sizeof(ice_user), local_pwd, sizeof(local_pwd),
					remote_pwd, sizeof(remote_pwd), &has_addr) == SWITCH_STATUS_SUCCESS);
				fst_requires(reverse_ice_username(ice_user, incoming_user,
					sizeof(incoming_user)) == SWITCH_STATUS_SUCCESS);
				stun_len = build_authenticated_ice_request(stun_packet, sizeof(stun_packet),
					incoming_user, local_pwd);
				fst_requires(switch_rtp_pvt_handle_ice_from(target_rtp, IPR_RTP,
					"127.0.0.1", restart_port, stun_packet, stun_len) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_rtp_pvt_get_transport_snapshot(target_rtp, IPR_RTP,
					&target_repeated) == SWITCH_STATUS_SUCCESS);
				fst_check(target_repeated.rtp_remote_addr == target_migrated.rtp_remote_addr);
				fst_check(target_repeated.dtls_remote_addr == target_migrated.dtls_remote_addr);
				fst_check(target_repeated.rtp_remote_port == target_migrated.rtp_remote_port);
				fst_check(target_repeated.dtls_remote_port == target_migrated.dtls_remote_port);
				fst_check(target_repeated.dtls_context == target_migrated.dtls_context);
				fst_check(target_repeated.dtls_restart_deadline_us ==
					target_migrated.dtls_restart_deadline_us);

				if (recovery_case == 0) {
					status = switch_rtp_pvt_dtls_input_from(target_rtp, DTLS_TYPE_RTP,
						"127.0.0.1", target_port, duplicate_peer_flight,
						duplicate_peer_flight_len);
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					status = recv_udp_flight_fd(replay_sink_fd, replayed_target_flight,
						sizeof(replayed_target_flight), &replayed_target_flight_len);
					fst_requires(status == SWITCH_STATUS_SUCCESS && replayed_target_flight_len > 0);
					fst_check(replayed_target_flight_len == original_target_flight_len);
					fst_check(!memcmp(replayed_target_flight, original_target_flight,
						replayed_target_flight_len));
					memcpy(target_to_peer, replayed_target_flight, replayed_target_flight_len);
					target_to_peer_len = replayed_target_flight_len;
				} else {
					switch_sleep(1100000);
					status = switch_rtp_pvt_dtls_timer_tick(target_rtp, DTLS_TYPE_RTP);
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					status = recv_udp_flight_fd(replay_sink_fd, timeout_target_flight,
						sizeof(timeout_target_flight), &timeout_target_flight_len);
					fst_requires(status == SWITCH_STATUS_SUCCESS && timeout_target_flight_len > 0);
					peer_to_target_len = 0;
					status = switch_rtp_pvt_dtls_step(peer_rtp, DTLS_TYPE_RTP,
						timeout_target_flight, timeout_target_flight_len, peer_to_target,
						sizeof(peer_to_target), &peer_to_target_len);
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					if (!peer_to_target_len) {
						memcpy(peer_to_target, duplicate_peer_flight, duplicate_peer_flight_len);
						peer_to_target_len = duplicate_peer_flight_len;
					}
					status = switch_rtp_pvt_dtls_input_from(target_rtp, DTLS_TYPE_RTP,
						"127.0.0.1", restart_port, peer_to_target, peer_to_target_len);
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					status = recv_udp_flight_fd(replay_sink_fd, target_to_peer,
						sizeof(target_to_peer), &target_to_peer_len);
					if (status == SWITCH_STATUS_TIMEOUT) {
						target_to_peer_len = 0;
					} else {
						fst_requires(status == SWITCH_STATUS_SUCCESS);
					}
				}
				status = switch_rtp_pvt_dtls_timer_tick(target_rtp, DTLS_TYPE_RTP);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_rtp_pvt_get_transport_snapshot(target_rtp, IPR_RTP,
					&target_after) == SWITCH_STATUS_SUCCESS);

				for (step = 0; step < 32; ++step) {
					peer_to_target_len = 0;
					status = switch_rtp_pvt_dtls_step(peer_rtp, DTLS_TYPE_RTP,
						target_to_peer, target_to_peer_len, peer_to_target, sizeof(peer_to_target),
						&peer_to_target_len);
					fst_requires(status == SWITCH_STATUS_SUCCESS);
					target_to_peer_len = 0;
					if (peer_to_target_len > 0) {
						status = switch_rtp_pvt_dtls_input_from(target_rtp, DTLS_TYPE_RTP,
							"127.0.0.1", restart_port, peer_to_target, peer_to_target_len);
						fst_requires(status == SWITCH_STATUS_SUCCESS);
						status = recv_udp_flight_fd(replay_sink_fd, target_to_peer,
							sizeof(target_to_peer), &target_to_peer_len);
						if (status == SWITCH_STATUS_TIMEOUT) {
							target_to_peer_len = 0;
						} else {
							fst_requires(status == SWITCH_STATUS_SUCCESS);
						}
					}
					fst_requires(switch_rtp_pvt_dtls_timer_tick(target_rtp,
						DTLS_TYPE_RTP) == SWITCH_STATUS_SUCCESS);
					fst_requires(switch_rtp_pvt_get_transport_snapshot(target_rtp, IPR_RTP,
						&target_after) == SWITCH_STATUS_SUCCESS);
					fst_requires(switch_rtp_pvt_get_transport_snapshot(peer_rtp, IPR_RTP,
						&peer_after) == SWITCH_STATUS_SUCCESS);
					if (target_after.dtls_state == DS_READY && peer_after.dtls_state == DS_READY) {
						break;
					}
				}

				fst_check(target_after.dtls_state == DS_READY);
				fst_check(peer_after.dtls_state == DS_READY);
				fst_check(target_after.dtls_context == target_before.dtls_context);
				fst_check(target_after.dtls_ssl == target_before.dtls_ssl);
				fst_check(target_after.dtls_read_bio == target_before.dtls_read_bio);
				fst_check(target_after.dtls_write_bio == target_before.dtls_write_bio);
				fst_check(target_after.srtp_send_ready == SWITCH_TRUE);
				fst_check(target_after.srtp_recv_ready == SWITCH_TRUE);
				fst_check(peer_after.srtp_send_ready == SWITCH_TRUE);
				fst_check(peer_after.srtp_recv_ready == SWITCH_TRUE);
				fst_check(target_after.dtls_restart_pending == SWITCH_FALSE);
				fst_check(target_after.dtls_restart_migrated == SWITCH_FALSE);
				fst_check(target_after.dtls_restart_deadline_us == 0);
				if (role_case == 0 && recovery_case == 0) {
					edge_now = switch_micro_time_now();
					edge_wait = old_restart_deadline > edge_now ?
						old_restart_deadline - edge_now + 100000 : 100000;
					switch_sleep(edge_wait);
					fst_requires(switch_rtp_pvt_dtls_timer_tick(target_rtp,
						DTLS_TYPE_RTP) == SWITCH_STATUS_SUCCESS);
					fst_requires(switch_rtp_pvt_get_transport_snapshot(target_rtp, IPR_RTP,
						&target_after) == SWITCH_STATUS_SUCCESS);
					fst_check(target_after.dtls_state == DS_READY);
					fst_check(target_after.dtls_context == target_before.dtls_context);
					fst_check(target_after.dtls_ssl == target_before.dtls_ssl);
				}
				fst_check(switch_rtp_pvt_srtp_round_trip(target_rtp, peer_rtp) ==
					SWITCH_STATUS_SUCCESS);
				switch_channel_set_variable(target_channel, "rtp_ice_mid_call_failover", "false");
				fst_requires(switch_rtp_pvt_get_ice_state(target_rtp, IPR_RTP,
					ice_user, sizeof(ice_user), local_pwd, sizeof(local_pwd),
					remote_pwd, sizeof(remote_pwd), &has_addr) == SWITCH_STATUS_SUCCESS);
				fst_requires(reverse_ice_username(ice_user, incoming_user,
					sizeof(incoming_user)) == SWITCH_STATUS_SUCCESS);
				stun_len = build_authenticated_ice_request(stun_packet, sizeof(stun_packet),
					incoming_user, local_pwd);
				fst_requires(switch_rtp_pvt_handle_ice_from(target_rtp, IPR_RTP,
					"127.0.0.1", failover_port, stun_packet, stun_len) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_rtp_pvt_get_transport_snapshot(target_rtp, IPR_RTP,
					&target_failover) == SWITCH_STATUS_SUCCESS);
				fst_check(target_failover.dtls_state == DS_READY);
				fst_check(target_failover.dtls_context == target_before.dtls_context);
				fst_check(target_failover.dtls_ssl == target_before.dtls_ssl);
				fst_check(target_failover.ice_remote_port == restart_port);
				fst_check(target_failover.rtp_remote_port == restart_port);
				fst_check(target_failover.rtcp_remote_port == restart_port);
				fst_check(target_failover.dtls_remote_port == restart_port);
				fst_check(target_failover.rtp_remote_addr == target_after.rtp_remote_addr);
				fst_check(target_failover.dtls_remote_addr == target_after.dtls_remote_addr);
				fst_check(target_failover.ice_ready == SWITCH_TRUE);
				fst_check(target_failover.ice_rready == SWITCH_TRUE);
				replayed_target_flight_len = 0;
				status = switch_rtp_pvt_dtls_input_from(target_rtp, DTLS_TYPE_RTP, "127.0.0.1",
					target_port, duplicate_peer_flight, duplicate_peer_flight_len);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				status = recv_udp_flight_fd(replay_sink_fd, replayed_target_flight,
					sizeof(replayed_target_flight), &replayed_target_flight_len);
				fst_check(status == SWITCH_STATUS_TIMEOUT);
				fst_check(replayed_target_flight_len == 0);

				switch_core_media_clear_ice(target_session);
				switch_channel_set_flag(target_channel, CF_REINVITE);
				match = switch_core_media_negotiate_sdp(target_session, ready_restart_sdp,
					&proceed, SDP_OFFER);
				fst_requires(match != 0);
				switch_core_media_gen_local_sdp(target_session, SDP_ANSWER, NULL, 0, NULL, 0);
				status = switch_core_media_activate_rtp(target_session);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_rtp_pvt_get_transport_snapshot(target_rtp, IPR_RTP,
					&target_ready_restart) == SWITCH_STATUS_SUCCESS);
				fst_check(target_ready_restart.dtls_state != DS_READY);
				fst_check(target_ready_restart.dtls_context != target_before.dtls_context);
				fst_check(target_ready_restart.dtls_destroy_count ==
					target_failover.dtls_destroy_count + 1);
				fst_check(target_ready_restart.srtp_send_ready == SWITCH_FALSE);
				fst_check(target_ready_restart.srtp_recv_ready == SWITCH_FALSE);
				close(replay_sink_fd);

				cleanup_session_media_and_sdp(target_session, target_sdp_session, target_parser);
				cleanup_session_media_and_sdp(peer_session, peer_sdp_session, peer_parser);
			}
			}
		}
		FCT_TEST_END();

		FCT_TEST_BGN(ready_dtls_rejects_ambiguous_identity_snapshots)
		{
			char restart_base[4096];
			char same_generation_base[4096];
			char *conflicting_tls_id_sdp;
			char *empty_tls_id_sdp;
			char *session_fingerprint_sdp;
			char *empty_fingerprint_override_sdp;
			char *fingerprint_overflow_sdp;
			char *same_generation_changed_tls_id_sdp;
			char *same_generation_empty_tls_id_sdp;
			char *same_generation_session_fingerprint_sdp;
			char *same_generation_empty_fingerprint_sdp;
			const char *fingerprint =
				"a=fingerprint:sha-256 17:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n";
			const char *overflow_fingerprints =
				"a=tls-id:association-1\n"
				"a=fingerprint:sha-256 01:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=fingerprint:sha-256 02:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=fingerprint:sha-256 03:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=fingerprint:sha-256 04:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=fingerprint:sha-256 05:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=fingerprint:sha-256 06:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=fingerprint:sha-256 07:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=fingerprint:sha-256 08:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=fingerprint:sha-256 09:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=fingerprint:";

			fst_requires(build_dtls_pair_sdp_ex(restart_base, sizeof(restart_base), 1683118282,
				18911, "passive", "invalidIdentityTargetUfrag2",
				"invalidIdentityTargetPassword1234562", SWITCH_FALSE) == SWITCH_STATUS_SUCCESS);
			fst_requires(build_dtls_pair_sdp_ex(same_generation_base,
				sizeof(same_generation_base), 1683118283, 18910, "passive",
				"invalidIdentityTargetUfrag1", "invalidIdentityTargetPassword1234561",
				SWITCH_FALSE) == SWITCH_STATUS_SUCCESS);
			conflicting_tls_id_sdp = switch_string_replace(restart_base, "a=fingerprint:",
				"a=tls-id:association-1\na=tls-id:association-2\na=fingerprint:");
			empty_tls_id_sdp = switch_string_replace(restart_base, "a=fingerprint:",
				"a=tls-id:association-1\na=tls-id:\na=fingerprint:");
			session_fingerprint_sdp = switch_string_replace(restart_base, "t=0 0\n",
				"t=0 0\na=fingerprint:sha-256 01:02:03:04:05:06:07:08:09:0A:0B:0C:0D:0E:0F:10:11:12:13:14:15:16:17:18:19:1A:1B:1C:1D:1E:1F:20\n");
			empty_fingerprint_override_sdp = session_fingerprint_sdp ?
				switch_string_replace(session_fingerprint_sdp, fingerprint, "a=fingerprint:\n") : NULL;
			fingerprint_overflow_sdp = switch_string_replace(restart_base, "a=fingerprint:",
				overflow_fingerprints);
			same_generation_changed_tls_id_sdp = switch_string_replace(same_generation_base,
				"a=fingerprint:", "a=tls-id:association-2\na=fingerprint:");
			same_generation_empty_tls_id_sdp = switch_string_replace(same_generation_base,
				"a=fingerprint:", "a=tls-id:\na=fingerprint:");
			same_generation_session_fingerprint_sdp = switch_string_replace(same_generation_base,
				"t=0 0\n",
				"t=0 0\na=fingerprint:sha-256 01:02:03:04:05:06:07:08:09:0A:0B:0C:0D:0E:0F:10:11:12:13:14:15:16:17:18:19:1A:1B:1C:1D:1E:1F:20\n");
			same_generation_empty_fingerprint_sdp = same_generation_session_fingerprint_sdp ?
				switch_string_replace(same_generation_session_fingerprint_sdp, fingerprint,
					"a=fingerprint:\n") : NULL;
			fst_requires(conflicting_tls_id_sdp != NULL && empty_tls_id_sdp != NULL &&
				session_fingerprint_sdp != NULL && empty_fingerprint_override_sdp != NULL &&
				fingerprint_overflow_sdp != NULL && same_generation_changed_tls_id_sdp != NULL &&
				same_generation_empty_tls_id_sdp != NULL &&
				same_generation_session_fingerprint_sdp != NULL &&
				same_generation_empty_fingerprint_sdp != NULL);
			fst_check(run_ready_invalid_identity_restart(conflicting_tls_id_sdp) ==
				SWITCH_STATUS_SUCCESS);
			fst_check(run_ready_invalid_identity_restart(empty_tls_id_sdp) ==
				SWITCH_STATUS_SUCCESS);
			fst_check(run_ready_invalid_identity_restart(empty_fingerprint_override_sdp) ==
				SWITCH_STATUS_SUCCESS);
			fst_check(run_ready_invalid_identity_restart(fingerprint_overflow_sdp) ==
				SWITCH_STATUS_SUCCESS);
			fst_check(run_ready_invalid_identity_restart(same_generation_changed_tls_id_sdp) ==
				SWITCH_STATUS_SUCCESS);
			fst_check(run_ready_invalid_identity_restart(same_generation_empty_tls_id_sdp) ==
				SWITCH_STATUS_SUCCESS);
			fst_check(run_ready_invalid_identity_restart(same_generation_empty_fingerprint_sdp) ==
				SWITCH_STATUS_SUCCESS);
			switch_safe_free(conflicting_tls_id_sdp);
			switch_safe_free(empty_tls_id_sdp);
			switch_safe_free(session_fingerprint_sdp);
			switch_safe_free(empty_fingerprint_override_sdp);
			switch_safe_free(fingerprint_overflow_sdp);
			switch_safe_free(same_generation_changed_tls_id_sdp);
			switch_safe_free(same_generation_empty_tls_id_sdp);
			switch_safe_free(same_generation_session_fingerprint_sdp);
			switch_safe_free(same_generation_empty_fingerprint_sdp);
		}
		FCT_TEST_END();

		FCT_TEST_BGN(ice_restart_without_duplicate_flight_expires_once_and_fails_closed)
		{
			switch_core_session_t *session = NULL;
			switch_channel_t *channel = NULL;
			switch_rtp_t *rtp = NULL;
			void *sdp_session = NULL;
			sdp_parser_t *parser = NULL;
			switch_rtp_pvt_transport_snapshot_t migrated;
			switch_rtp_pvt_transport_snapshot_t retransmitted;
			switch_rtp_pvt_transport_snapshot_t expired;
			int timeout_sink_fd = -1;
			uint8_t output[64];
			uint8_t retransmitted_flight[32768];
			uint8_t initial_flight[32768];
			switch_size_t output_len = 0;
			switch_size_t retransmitted_flight_len = 0;
			switch_size_t initial_flight_len = 0;
			char initial_sdp[2048];
			char restart_sdp[2048];
			switch_status_t status;

			fst_requires(build_dtls_pair_sdp(initial_sdp, sizeof(initial_sdp), 1683118230,
				18510, "passive", "timeoutRemoteUfrag1", "timeoutRemotePassword1234561") ==
				SWITCH_STATUS_SUCCESS);
			fst_requires(build_dtls_pair_sdp(restart_sdp, sizeof(restart_sdp), 1683118231,
				18511, "passive", "timeoutRemoteUfrag2", "timeoutRemotePassword1234562") ==
				SWITCH_STATUS_SUCCESS);
			timeout_sink_fd = bind_udp_sink_fd(18511);
			fst_requires(timeout_sink_fd >= 0);
			status = make_session_and_rtp_with_sdp_ex(&session, &rtp, &sdp_session, &parser,
				initial_sdp, "PCMU", SWITCH_TRUE, SWITCH_FALSE, "controlled");
			fst_requires(status == SWITCH_STATUS_SUCCESS && session && !rtp);
			channel = switch_core_session_get_channel(session);
			fst_requires(channel != NULL);
			switch_channel_set_variable(channel, "media_dtls_setup_timeout", "2500");
			status = switch_core_media_activate_rtp(session);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			rtp = switch_core_media_get_rtp_session(session, SWITCH_MEDIA_TYPE_AUDIO);
			fst_requires(rtp != NULL);
			status = switch_rtp_pvt_dtls_step(rtp, DTLS_TYPE_RTP, NULL, 0,
				initial_flight, sizeof(initial_flight), &initial_flight_len);
			fst_requires(status == SWITCH_STATUS_SUCCESS && initial_flight_len > 0);
			status = restart_and_nominate_controlled_dtls(session, rtp, restart_sdp, 18511, &migrated);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check(migrated.dtls_state == DS_HANDSHAKE);
			fst_check(migrated.dtls_restart_migrated == SWITCH_TRUE);

			switch_sleep(1100000);
			status = switch_rtp_pvt_dtls_timer_tick(rtp, DTLS_TYPE_RTP);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			status = recv_udp_flight_fd(timeout_sink_fd, retransmitted_flight,
				sizeof(retransmitted_flight), &retransmitted_flight_len);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check(retransmitted_flight_len > 0);
			fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &retransmitted) ==
				SWITCH_STATUS_SUCCESS);
			fst_check(retransmitted.dtls_state == DS_HANDSHAKE);
			fst_check(retransmitted.dtls_restart_migrated == SWITCH_TRUE);

			switch_sleep(1600000);
			status = switch_rtp_pvt_dtls_step(rtp, DTLS_TYPE_RTP, NULL, 0,
				output, sizeof(output), &output_len);
			fst_check(status == SWITCH_STATUS_FALSE);
			fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &expired) ==
				SWITCH_STATUS_SUCCESS);
			fst_check(expired.dtls_state == DS_FAIL);
			fst_check(expired.dtls_restart_pending == SWITCH_FALSE);
			fst_check(expired.dtls_restart_migrated == SWITCH_FALSE);
			fst_check(output_len == 0);
			close(timeout_sink_fd);

			cleanup_session_media_and_sdp(session, sdp_session, parser);
		}
		FCT_TEST_END();

		FCT_TEST_BGN(expired_prepared_restart_cannot_publish_partial_ice_generation)
		{
			switch_core_session_t *session = NULL;
			switch_rtp_t *rtp = NULL;
			void *sdp_session = NULL;
			sdp_parser_t *parser = NULL;
			switch_rtp_pvt_transport_snapshot_t before;
			switch_rtp_pvt_transport_snapshot_t after;
			switch_rtp_pvt_dtls_identity_t identity;
			switch_rtp_pvt_dtls_restart_result_t restart_result;
			ice_t restart_ice;
			char initial_sdp[2048];
			char old_ice_user[513] = "";
			char old_local_pwd[256] = "";
			char old_remote_pwd[256] = "";
			char active_ice_user[513] = "";
			char active_local_pwd[256] = "";
			char active_remote_pwd[256] = "";
			char host[80] = "";
			switch_port_t before_port = 0;
			switch_port_t after_port = 0;
			switch_bool_t has_addr = SWITCH_FALSE;
			switch_status_t status;

			memset(&before, 0, sizeof(before));
			memset(&after, 0, sizeof(after));
			memset(&identity, 0, sizeof(identity));
			memset(&restart_ice, 0, sizeof(restart_ice));
			fst_requires(build_dtls_pair_sdp(initial_sdp, sizeof(initial_sdp), 1683118235,
				18550, "active", "atomicRemoteUfrag1", "atomicRemotePassword1234561") ==
				SWITCH_STATUS_SUCCESS);
			status = make_session_and_rtp_with_sdp_ex(&session, &rtp, &sdp_session, &parser,
				initial_sdp, "PCMU", SWITCH_TRUE, SWITCH_TRUE, "controlled");
			fst_requires(status == SWITCH_STATUS_SUCCESS && session && rtp);
			fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &before) ==
				SWITCH_STATUS_SUCCESS);
			fst_requires(before.dtls_state == DS_HANDSHAKE && before.dtls_context && before.dtls_ssl);
			fst_requires(switch_rtp_pvt_get_dtls_association_identity(rtp, DTLS_TYPE_RTP,
				&identity) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_rtp_pvt_get_ice_state(rtp, IPR_RTP,
				old_ice_user, sizeof(old_ice_user), old_local_pwd, sizeof(old_local_pwd),
				old_remote_pwd, sizeof(old_remote_pwd), &has_addr) == SWITCH_STATUS_SUCCESS);
			fst_requires(has_addr == SWITCH_TRUE);
			fst_requires(copy_sockaddr_tuple(switch_rtp_session_get_remote_addr(rtp), host,
				sizeof(host), &before_port) == SWITCH_STATUS_SUCCESS);

			restart_result = switch_rtp_pvt_prepare_dtls_ice_restart(rtp, &identity, 30000,
				before.ice_type, "atomicRemoteUfrag2", "atomicLocalUfrag2",
				"atomicLocalPassword1234562", "atomicRemotePassword1234562");
			fst_requires(restart_result == SWITCH_RTP_PVT_DTLS_RESTART_PRESERVE);
			fst_requires(switch_rtp_pvt_expire_prepared_dtls_ice_restart(rtp) == SWITCH_STATUS_SUCCESS);

			restart_ice.cands[0][IPR_RTP].foundation = "2";
			restart_ice.cands[0][IPR_RTP].component_id = 1;
			restart_ice.cands[0][IPR_RTP].transport = "udp";
			restart_ice.cands[0][IPR_RTP].priority = 2130706430;
			restart_ice.cands[0][IPR_RTP].con_addr = "127.0.0.1";
			restart_ice.cands[0][IPR_RTP].con_port = 18551;
			restart_ice.cands[0][IPR_RTP].cand_type = "host";
			restart_ice.cands[0][IPR_RTP].ready = 1;
			restart_ice.cand_idx[IPR_RTP] = 1;
			restart_ice.chosen[IPR_RTP] = 0;
			restart_ice.is_chosen[IPR_RTP] = 1;
			switch_rtp_prepare_ice_restart(rtp, IPR_RTP, SWITCH_TRUE);
			status = switch_rtp_activate_ice(rtp, "atomicRemoteUfrag2", "atomicLocalUfrag2",
				"atomicLocalPassword1234562", "atomicRemotePassword1234562", IPR_RTP,
				before.ice_type, &restart_ice);
			fst_check(status == SWITCH_STATUS_TIMEOUT);
			fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &after) ==
				SWITCH_STATUS_SUCCESS);
			fst_check(after.dtls_state == DS_FAIL);
			fst_check(after.dtls_context == before.dtls_context);
			fst_check(after.dtls_ssl == before.dtls_ssl);
			fst_requires(copy_sockaddr_tuple(switch_rtp_session_get_remote_addr(rtp), host,
				sizeof(host), &after_port) == SWITCH_STATUS_SUCCESS);
			fst_check(after_port == before_port);
			has_addr = SWITCH_FALSE;
			fst_requires(switch_rtp_pvt_get_ice_state(rtp, IPR_RTP,
				active_ice_user, sizeof(active_ice_user), active_local_pwd, sizeof(active_local_pwd),
				active_remote_pwd, sizeof(active_remote_pwd), &has_addr) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(active_ice_user, old_ice_user);
			fst_check_string_equals(active_local_pwd, old_local_pwd);
			fst_check_string_equals(active_remote_pwd, old_remote_pwd);
			fst_check(has_addr == SWITCH_TRUE);

			cleanup_session_media_and_sdp(session, sdp_session, parser);
		}
		FCT_TEST_END();

		FCT_TEST_BGN(ice_restart_activation_progresses_with_datawait_control_only_reader)
		{
			switch_core_session_t *session = NULL;
			switch_rtp_t *rtp = NULL;
			void *sdp_session = NULL;
			sdp_parser_t *parser = NULL;
			switch_rtp_pvt_transport_snapshot_t before;
			switch_rtp_pvt_dtls_identity_t identity;
			switch_rtp_pvt_dtls_restart_result_t restart_result;
			dtls_datawait_activation_race_t race;
			bounded_test_watchdog_t watchdog;
			switch_threadattr_t *thread_attr = NULL;
			switch_thread_t *reader_thread = NULL;
			switch_thread_t *activation_thread = NULL;
			switch_thread_t *watchdog_thread = NULL;
			switch_status_t reader_thread_status = SWITCH_STATUS_FALSE;
			switch_status_t activation_thread_status = SWITCH_STATUS_FALSE;
			switch_status_t create_status = SWITCH_STATUS_FALSE;
			switch_status_t cleanup_status = SWITCH_STATUS_FALSE;
			switch_status_t status;
			switch_bool_t reader_blocked;
			switch_bool_t reader_entered;
			switch_bool_t failure_reader_started;
			switch_bool_t activation_completed_while_reader_blocked;
			int started = 0;
			char initial_sdp[2048];

			memset(&before, 0, sizeof(before));
			memset(&identity, 0, sizeof(identity));
			memset(&race, 0, sizeof(race));
			race.reader_status = SWITCH_STATUS_FALSE;
			race.activation_status = SWITCH_STATUS_FALSE;
			fst_requires(build_dtls_pair_sdp(initial_sdp, sizeof(initial_sdp), 1683118238,
				18640, "active", "datawaitRemoteUfrag1", "datawaitRemotePassword1234561") ==
				SWITCH_STATUS_SUCCESS);
			status = make_session_and_rtp_with_sdp_ex(&session, &rtp, &sdp_session, &parser,
				initial_sdp, "PCMU", SWITCH_TRUE, SWITCH_TRUE, "controlled");
			fst_requires(status == SWITCH_STATUS_SUCCESS && session && rtp);
			fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &before) ==
				SWITCH_STATUS_SUCCESS);
			fst_requires(before.dtls_state == DS_HANDSHAKE && before.dtls_context && before.dtls_ssl);
			fst_requires(switch_rtp_pvt_get_dtls_association_identity(rtp, DTLS_TYPE_RTP,
				&identity) == SWITCH_STATUS_SUCCESS);

			restart_result = switch_rtp_pvt_prepare_dtls_ice_restart(rtp, &identity, 30000,
				before.ice_type, "datawaitRemoteUfrag2", "datawaitLocalUfrag2",
				"datawaitLocalPassword1234562", "datawaitRemotePassword1234562");
			fst_requires(restart_result == SWITCH_RTP_PVT_DTLS_RESTART_PRESERVE);
			memset(&race.restart_ice, 0, sizeof(race.restart_ice));
			race.restart_ice.cands[0][IPR_RTP].foundation = "2";
			race.restart_ice.cands[0][IPR_RTP].component_id = 1;
			race.restart_ice.cands[0][IPR_RTP].transport = "udp";
			race.restart_ice.cands[0][IPR_RTP].priority = 2130706430;
			race.restart_ice.cands[0][IPR_RTP].con_addr = "127.0.0.1";
			race.restart_ice.cands[0][IPR_RTP].con_port = 18641;
			race.restart_ice.cands[0][IPR_RTP].cand_type = "host";
			race.restart_ice.cands[0][IPR_RTP].ready = 1;
			race.restart_ice.cand_idx[IPR_RTP] = 1;
			race.restart_ice.chosen[IPR_RTP] = 0;
			race.restart_ice.is_chosen[IPR_RTP] = 1;
			switch_rtp_prepare_ice_restart(rtp, IPR_RTP, SWITCH_TRUE);

			race.rtp = rtp;
			fst_requires(switch_mutex_init(&race.mutex, SWITCH_MUTEX_NESTED, pool) ==
				SWITCH_STATUS_SUCCESS);
			fst_requires(switch_thread_cond_create(&race.cond, pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_threadattr_create(&thread_attr, pool) == SWITCH_STATUS_SUCCESS);
			if (bounded_test_watchdog_start(&watchdog, thread_attr, pool, 30000000,
					"DATAWAIT activation", &watchdog_thread) != SWITCH_STATUS_SUCCESS) {
				_exit(1);
			}
			switch_rtp_clear_flag(rtp, SWITCH_RTP_FLAG_USE_TIMER);
			switch_rtp_set_flag(rtp, SWITCH_RTP_FLAG_DATAWAIT);
			create_status = switch_thread_create(&reader_thread, thread_attr,
				dtls_datawait_reader_thread, &race, pool);
			if (create_status == SWITCH_STATUS_SUCCESS) {
				started = 1;
			}
			reader_entered = started == 1 &&
				dtls_datawait_activation_wait(&race, &race.reader_started, 2000000) &&
				rtp_read_lock_wait(race.rtp, 2000000) ? SWITCH_TRUE : SWITCH_FALSE;
			if (reader_entered) {
				create_status = switch_thread_create(&activation_thread, thread_attr,
					dtls_datawait_activation_thread, &race, pool);
				if (create_status == SWITCH_STATUS_SUCCESS) {
					started = 2;
				}
			}
			if (started == 2) {
				dtls_datawait_activation_wait(&race, &race.activation_started, 2000000);
			}
			activation_completed_while_reader_blocked = started == 2 &&
				dtls_datawait_activation_wait(&race, &race.activation_done, 1000000) &&
				!dtls_datawait_activation_state(&race, &race.reader_done) &&
				switch_rtp_pvt_read_lock_held(rtp) ? SWITCH_TRUE : SWITCH_FALSE;
			reader_blocked = !dtls_datawait_activation_state(&race, &race.reader_done);
			cleanup_status = dtls_datawait_activation_abort_and_join(&race, reader_thread,
				activation_thread, started, &reader_thread_status, &activation_thread_status);
			if (cleanup_status != SWITCH_STATUS_SUCCESS) {
				_exit(1);
			}
			fst_check(reader_entered == SWITCH_TRUE);
			fst_check(started == 2);
			fst_check(reader_blocked == SWITCH_TRUE);
			fst_check(activation_completed_while_reader_blocked == SWITCH_TRUE);
			fst_check(race.activation_status == SWITCH_STATUS_SUCCESS);
			fst_check(reader_thread_status == SWITCH_STATUS_SUCCESS);
			fst_check(activation_thread_status == SWITCH_STATUS_SUCCESS);

			race.reader_started = 0;
			race.reader_done = 0;
			race.activation_started = 0;
			race.activation_done = 0;
			race.reader_status = SWITCH_STATUS_FALSE;
			race.activation_status = SWITCH_STATUS_FALSE;
			reader_thread = NULL;
			activation_thread = NULL;
			reader_thread_status = SWITCH_STATUS_FALSE;
			activation_thread_status = SWITCH_STATUS_FALSE;
			started = 0;
			switch_rtp_set_flag(rtp, SWITCH_RTP_FLAG_DATAWAIT);
			create_status = dtls_datawait_activation_start_workers(&race, thread_attr, pool,
				&reader_thread, &activation_thread, 1, &started);
			failure_reader_started = started == 1 &&
				dtls_datawait_activation_wait(&race, &race.reader_started, 2000000) ?
				SWITCH_TRUE : SWITCH_FALSE;
			cleanup_status = dtls_datawait_activation_abort_and_join(&race, reader_thread,
				activation_thread, started, &reader_thread_status, &activation_thread_status);
			if (cleanup_status != SWITCH_STATUS_SUCCESS) {
				_exit(1);
			}
			fst_check(create_status == SWITCH_STATUS_FALSE);
			fst_check(started == 1);
			fst_check(failure_reader_started == SWITCH_TRUE);

			cleanup_session_media_and_sdp(session, sdp_session, parser);
			bounded_test_watchdog_stop(&watchdog, watchdog_thread);
		}
		FCT_TEST_END();

		FCT_TEST_BGN(rtcp_ice_activation_progresses_with_nonmux_stun_reader)
		{
			switch_core_session_t *session = NULL;
			switch_rtp_t *rtp = NULL;
			void *sdp_session = NULL;
			sdp_parser_t *parser = NULL;
			switch_threadattr_t *thread_attr = NULL;
			switch_thread_t *reader_thread = NULL;
			switch_thread_t *activation_thread = NULL;
			switch_thread_t *watchdog_thread = NULL;
			switch_status_t reader_thread_status = SWITCH_STATUS_FALSE;
			switch_status_t activation_thread_status = SWITCH_STATUS_FALSE;
			switch_status_t send_status = SWITCH_STATUS_FALSE;
			switch_status_t status;
			switch_rtp_pvt_transport_snapshot_t snapshot;
			rtcp_activation_race_t race;
			bounded_test_watchdog_t watchdog;
			char ice_user[513] = "";
			char incoming_user[513] = "";
			char local_pwd[256] = "";
			char remote_pwd[256] = "";
			char *colon;
			switch_size_t remote_ufrag_len;
			uint8_t stun_packet[512];
			uint8_t wake_packet[8] = { 0x7f };
			const uint8_t fatal_alert[] = {
				0x15, 0xfe, 0xfd, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x28
			};
			switch_size_t stun_len;
			switch_bool_t has_addr = SWITCH_FALSE;
			switch_bool_t reader_entered = SWITCH_FALSE;
			switch_bool_t activation_completed = SWITCH_FALSE;
			switch_bool_t dispatch_overlapped = SWITCH_FALSE;
			switch_bool_t stun_processed = SWITCH_FALSE;
			switch_port_t local_rtcp_port;
			switch_port_t local_rtp_port;
			switch_time_t deadline;
			int sender_fd = -1;
			int reader_started = 0;
			int activation_started = 0;
			int removal_reader_started = 0;
			int i;
			const char *nonmux_sdp =
				"v=0\n"
				"o=- 1683118194 1683118290 IN IP4 0.0.0.0\n"
				"s=-\n"
				"t=0 0\n"
				"m=audio 19680 UDP/TLS/RTP/SAVPF 0\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:rtcpRaceRemoteUfrag\n"
				"a=ice-pwd:rtcpRaceRemotePassword123456\n"
				"a=candidate:1 1 udp 2130706431 127.0.0.1 19680 typ host\n"
				"a=candidate:2 2 udp 2130706430 127.0.0.1 19681 typ host\n"
				"a=rtcp:19681 IN IP4 127.0.0.1\n"
				"a=setup:active\n"
				"a=rtpmap:0 PCMU/8000\n"
				"a=sendrecv\n"
				"a=fingerprint:sha-256 17:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=mid:0\n";

			memset(&snapshot, 0, sizeof(snapshot));
			memset(&race, 0, sizeof(race));
			race.reader_status = SWITCH_STATUS_FALSE;
			race.activation_status = SWITCH_STATUS_FALSE;
			status = make_session_and_rtp_with_sdp_ex(&session, &rtp, &sdp_session, &parser,
				nonmux_sdp, "PCMU", SWITCH_FALSE, SWITCH_TRUE, "controlled");
			fst_requires(status == SWITCH_STATUS_SUCCESS && session && rtp);
			switch_rtp_set_media_timeout(rtp, 0);
			fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTCP, &snapshot) ==
				SWITCH_STATUS_SUCCESS);
			fst_requires(snapshot.socket != NULL && snapshot.dtls_context != NULL);
			fst_requires(switch_rtp_pvt_get_ice_state(rtp, IPR_RTCP, ice_user,
				sizeof(ice_user), local_pwd, sizeof(local_pwd), remote_pwd,
				sizeof(remote_pwd), &has_addr) == SWITCH_STATUS_SUCCESS);
			fst_requires(has_addr == SWITCH_TRUE);
			fst_requires(reverse_ice_username(ice_user, incoming_user,
				sizeof(incoming_user)) == SWITCH_STATUS_SUCCESS);
			colon = strchr(ice_user, ':');
			fst_requires(colon != NULL && colon != ice_user && colon[1] != '\0');
			remote_ufrag_len = (switch_size_t)(colon - ice_user);
			fst_requires(remote_ufrag_len < sizeof(race.remote_ufrag));
			memcpy(race.remote_ufrag, ice_user, remote_ufrag_len);
			race.remote_ufrag[remote_ufrag_len] = '\0';
			switch_copy_string(race.local_ufrag, colon + 1, sizeof(race.local_ufrag));
			switch_copy_string(race.local_pwd, local_pwd, sizeof(race.local_pwd));
			switch_copy_string(race.remote_pwd, remote_pwd, sizeof(race.remote_pwd));
			race.ice_type = snapshot.ice_type;
			race.rtp = rtp;
			race.ice_params.cands[0][IPR_RTCP].foundation = "2";
			race.ice_params.cands[0][IPR_RTCP].component_id = 2;
			race.ice_params.cands[0][IPR_RTCP].transport = "udp";
			race.ice_params.cands[0][IPR_RTCP].priority = 2130706430;
			race.ice_params.cands[0][IPR_RTCP].con_addr = "127.0.0.1";
			race.ice_params.cands[0][IPR_RTCP].con_port = 19681;
			race.ice_params.cands[0][IPR_RTCP].cand_type = "host";
			race.ice_params.cands[0][IPR_RTCP].ready = 1;
			race.ice_params.cand_idx[IPR_RTCP] = 1;
			race.ice_params.chosen[IPR_RTCP] = 0;
			race.ice_params.is_chosen[IPR_RTCP] = 1;
			fst_requires(switch_mutex_init(&race.mutex, SWITCH_MUTEX_NESTED, pool) ==
				SWITCH_STATUS_SUCCESS);
			fst_requires(switch_thread_cond_create(&race.cond, pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_threadattr_create(&thread_attr, pool) == SWITCH_STATUS_SUCCESS);
			local_rtp_port = switch_rtp_get_local_port(rtp);
			local_rtcp_port = local_rtp_port + 1;
			sender_fd = bind_udp_sink_fd(19681);
			fst_requires(sender_fd >= 0 && local_rtp_port > 0 && local_rtcp_port > 1);
			stun_len = build_authenticated_ice_request(stun_packet, sizeof(stun_packet),
				incoming_user, local_pwd);
			if (bounded_test_watchdog_start(&watchdog, thread_attr, pool, 30000000,
					"non-mux RTCP activation", &watchdog_thread) != SWITCH_STATUS_SUCCESS) {
				_exit(1);
			}

			switch_rtp_clear_flag(rtp, SWITCH_RTP_FLAG_USE_TIMER);
			switch_rtp_set_flag(rtp, SWITCH_RTP_FLAG_DATAWAIT);
			switch_rtp_pvt_set_lock_test_hooks(rtp, rtcp_activation_write_locked_hook,
				rtcp_activation_stun_dispatch_hook, NULL, NULL, &race);
			status = switch_thread_create(&reader_thread, thread_attr,
				rtcp_activation_reader_thread, &race, pool);
			if (status == SWITCH_STATUS_SUCCESS) {
				reader_started = 1;
			}
			reader_entered = reader_started &&
				rtcp_activation_wait(&race, &race.reader_started, 2000000) &&
				rtp_read_lock_wait(race.rtp, 2000000) ? SWITCH_TRUE : SWITCH_FALSE;
			if (reader_entered) {
				status = switch_thread_create(&activation_thread, thread_attr,
					rtcp_activation_worker_thread, &race, pool);
				if (status == SWITCH_STATUS_SUCCESS) {
					activation_started = 1;
				}
			}
			if (activation_started && rtcp_activation_wait(&race, &race.activation_write_locked,
				2000000)) {
				for (i = 0; i < 256; ++i) {
					send_status = send_udp_packet_fd(sender_fd, local_rtcp_port, stun_packet,
						stun_len);
					if (send_status == SWITCH_STATUS_SUCCESS) {
						send_status = send_udp_packet_fd(sender_fd, local_rtp_port, wake_packet,
							sizeof(wake_packet));
					}
					if (send_status != SWITCH_STATUS_SUCCESS) {
						break;
					}
					if (rtcp_activation_wait(&race, &race.stun_dispatch_started, 10000)) {
						break;
					}
					switch_sleep(500);
				}
			}
			dispatch_overlapped = rtcp_activation_wait(&race, &race.dispatch_overlapped,
				2000000);
			activation_completed = activation_started &&
				rtcp_activation_wait(&race, &race.activation_done, 5000000) ?
				SWITCH_TRUE : SWITCH_FALSE;
			if (activation_started && !activation_completed) {
				_exit(1);
			}
			if (activation_completed && send_status == SWITCH_STATUS_SUCCESS) {
				deadline = switch_micro_time_now() + 2000000;
				while (switch_micro_time_now() < deadline) {
					memset(&snapshot, 0, sizeof(snapshot));
					if (switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTCP, &snapshot) ==
						SWITCH_STATUS_SUCCESS && snapshot.ice_rready) {
						stun_processed = SWITCH_TRUE;
						break;
					}
					switch_sleep(1000);
				}
			}

			switch_rtp_clear_flag(rtp, SWITCH_RTP_FLAG_DATAWAIT);
			switch_rtp_break(rtp);
			if (reader_started && !rtcp_activation_wait(&race, &race.reader_done, 5000000)) {
				_exit(1);
			}
			if (activation_started && switch_thread_join(&activation_thread_status,
				activation_thread) != SWITCH_STATUS_SUCCESS) {
				_exit(1);
			}
			if (reader_started && switch_thread_join(&reader_thread_status,
				reader_thread) != SWITCH_STATUS_SUCCESS) {
				_exit(1);
			}
			switch_rtp_pvt_set_lock_test_hooks(rtp, NULL, NULL, NULL, NULL, NULL);
			fst_check(reader_entered == SWITCH_TRUE);
			fst_check(activation_started == 1);
			fst_check(activation_completed == SWITCH_TRUE);
			fst_check(dispatch_overlapped == SWITCH_TRUE);
			fst_check(race.activation_status == SWITCH_STATUS_SUCCESS);
			fst_check(send_status == SWITCH_STATUS_SUCCESS);
			fst_check(stun_processed == SWITCH_TRUE);
			fst_check(reader_thread_status == SWITCH_STATUS_SUCCESS);
			fst_check(activation_thread_status == SWITCH_STATUS_SUCCESS);

			switch_mutex_lock(race.mutex);
			race.reader_started = 0;
			race.reader_done = 0;
			race.rtcp_dtls_removed = 0;
			switch_mutex_unlock(race.mutex);
			race.reader_status = SWITCH_STATUS_FALSE;
			race.rtcp_dtls_remove_status = SWITCH_STATUS_FALSE;
			reader_thread = NULL;
			reader_thread_status = SWITCH_STATUS_FALSE;
			switch_rtp_set_flag(rtp, SWITCH_RTP_FLAG_DATAWAIT);
			switch_rtp_pvt_set_lock_test_hooks(rtp, NULL, NULL, NULL,
				rtcp_dtls_remove_hook, &race);
			status = switch_thread_create(&reader_thread, thread_attr,
				rtcp_activation_reader_thread, &race, pool);
			if (status == SWITCH_STATUS_SUCCESS) {
				removal_reader_started = 1;
			}
			if (!removal_reader_started ||
				!rtcp_activation_wait(&race, &race.reader_started, 2000000) ||
				!rtp_read_lock_wait(race.rtp, 2000000)) {
				_exit(1);
			}
			for (i = 0; i < 256 &&
				!rtcp_activation_wait(&race, &race.rtcp_dtls_removed, 10000); ++i) {
				if (send_udp_packet_fd(sender_fd, local_rtcp_port, fatal_alert,
						sizeof(fatal_alert)) != SWITCH_STATUS_SUCCESS ||
					send_udp_packet_fd(sender_fd, local_rtp_port, wake_packet,
						sizeof(wake_packet)) != SWITCH_STATUS_SUCCESS) {
					_exit(1);
				}
			}
			if (!rtcp_activation_wait(&race, &race.rtcp_dtls_removed, 2000000)) {
				_exit(1);
			}
			switch_rtp_clear_flag(rtp, SWITCH_RTP_FLAG_DATAWAIT);
			switch_rtp_break(rtp);
			if (!rtcp_activation_wait(&race, &race.reader_done, 5000000) ||
				switch_thread_join(&reader_thread_status, reader_thread) != SWITCH_STATUS_SUCCESS) {
				_exit(1);
			}
			switch_rtp_pvt_set_lock_test_hooks(rtp, NULL, NULL, NULL, NULL, NULL);
			memset(&snapshot, 0, sizeof(snapshot));
			if (switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTCP, &snapshot) !=
				SWITCH_STATUS_SUCCESS) {
				_exit(1);
			}
			fst_check(race.rtcp_dtls_remove_status == SWITCH_STATUS_SUCCESS);
			fst_check(snapshot.dtls_context == NULL);
			fst_check(reader_thread_status == SWITCH_STATUS_SUCCESS);

			close(sender_fd);

			cleanup_session_media_and_sdp(session, sdp_session, parser);
			bounded_test_watchdog_stop(&watchdog, watchdog_thread);
		}
		FCT_TEST_END();

		FCT_TEST_BGN(dtls_nominated_destination_update_obeys_lifecycle_lock_order)
		{
			switch_core_session_t *session = NULL;
			switch_rtp_t *rtp = NULL;
			void *sdp_session = NULL;
			sdp_parser_t *parser = NULL;
			switch_threadattr_t *thread_attr = NULL;
			switch_thread_t *reader_thread = NULL;
			switch_thread_t *lifecycle_thread = NULL;
			switch_thread_t *watchdog_thread = NULL;
			switch_status_t reader_thread_status = SWITCH_STATUS_FALSE;
			switch_status_t lifecycle_thread_status = SWITCH_STATUS_FALSE;
			switch_status_t status;
			switch_rtp_pvt_transport_snapshot_t before;
			switch_rtp_pvt_transport_snapshot_t nominated;
			switch_rtp_pvt_transport_snapshot_t after;
			dtls_destination_lifecycle_race_t race;
			bounded_test_watchdog_t watchdog;
			char initial_sdp[2048];
			const uint8_t fatal_alert[] = {
				0x15, 0xfe, 0xfd, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x28
			};
			switch_bool_t reader_completed;
			switch_bool_t lifecycle_completed;
			switch_port_t local_rtp_port;
			int sender_fd = -1;
			int send_attempt;

			memset(&before, 0, sizeof(before));
			memset(&nominated, 0, sizeof(nominated));
			memset(&after, 0, sizeof(after));
			memset(&race, 0, sizeof(race));
			race.reader_status = SWITCH_STATUS_FALSE;
			race.lifecycle_status = SWITCH_STATUS_FALSE;

			fst_requires(build_dtls_pair_sdp(initial_sdp, sizeof(initial_sdp), 1683118248,
				19690, "active", "dtlsLockRemoteUfrag", "dtlsLockRemotePassword123456") ==
				SWITCH_STATUS_SUCCESS);
			fst_requires(add_rtp_candidate_to_sdp(initial_sdp, sizeof(initial_sdp), 2,
				2130706430, 19691) == SWITCH_STATUS_SUCCESS);
			status = make_session_and_rtp_with_sdp_ex(&session, &rtp, &sdp_session, &parser,
				initial_sdp, "PCMU", SWITCH_TRUE, SWITCH_TRUE, "controlled");
			fst_requires(status == SWITCH_STATUS_SUCCESS && session && rtp);
			fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &before) ==
				SWITCH_STATUS_SUCCESS);
			fst_requires(before.dtls_state == DS_HANDSHAKE && before.rtp_remote_port == 19690);
			fst_requires(switch_rtp_pvt_mark_ice_candidate_nominated(rtp, IPR_RTP,
				"127.0.0.1", 19691) == SWITCH_STATUS_SUCCESS);
			fprintf(stderr, "DTLS_LOCK_TEST nominated alternate tuple\n");
			fflush(stderr);
			fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &nominated) ==
				SWITCH_STATUS_SUCCESS);
			fst_requires(nominated.rtp_remote_port == 19690 && nominated.dtls_state == DS_HANDSHAKE);

			race.rtp = rtp;
			fst_requires(switch_mutex_init(&race.mutex, SWITCH_MUTEX_NESTED, pool) ==
				SWITCH_STATUS_SUCCESS);
			fst_requires(switch_thread_cond_create(&race.cond, pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_threadattr_create(&thread_attr, pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_rtp_pvt_get_dtls_association_identity(rtp, DTLS_TYPE_RTP,
				&race.identity) == SWITCH_STATUS_SUCCESS);
			local_rtp_port = switch_rtp_get_local_port(rtp);
			sender_fd = bind_udp_sink_fd(19691);
			fst_requires(sender_fd >= 0 && local_rtp_port > 0);
			if (bounded_test_watchdog_start(&watchdog, thread_attr, pool, 15000000,
					"DTLS destination lifecycle", &watchdog_thread) != SWITCH_STATUS_SUCCESS) {
				_exit(1);
			}
			switch_rtp_clear_flag(rtp, SWITCH_RTP_FLAG_USE_TIMER);
			switch_rtp_set_flag(rtp, SWITCH_RTP_FLAG_DATAWAIT);
			switch_rtp_pvt_set_lock_test_hooks(rtp,
				dtls_destination_lifecycle_write_locked_hook, NULL,
				dtls_destination_ice_locked_hook, NULL, &race);

			status = switch_thread_create(&reader_thread, thread_attr,
				dtls_destination_reader_thread, &race, pool);
			if (status != SWITCH_STATUS_SUCCESS ||
				!dtls_destination_lifecycle_wait(&race, &race.reader_started, 2000000)) {
				_exit(1);
			}
			for (send_attempt = 0; send_attempt < 20 &&
				!dtls_destination_lifecycle_state(&race, &race.dtls_ice_locked); ++send_attempt) {
				if (send_udp_packet_fd(sender_fd, local_rtp_port, fatal_alert,
						sizeof(fatal_alert)) != SWITCH_STATUS_SUCCESS) {
					_exit(1);
				}
				dtls_destination_lifecycle_wait(&race, &race.dtls_ice_locked, 100000);
			}
			if (!dtls_destination_lifecycle_state(&race, &race.dtls_ice_locked)) {
				_exit(1);
			}
			fprintf(stderr, "DTLS_LOCK_TEST real receive reached destination update\n");
			fflush(stderr);
			status = switch_thread_create(&lifecycle_thread, thread_attr,
				dtls_destination_lifecycle_thread, &race, pool);
			if (status != SWITCH_STATUS_SUCCESS ||
				!dtls_destination_lifecycle_wait(&race, &race.lifecycle_started, 2000000)) {
				_exit(1);
			}
			reader_completed = dtls_destination_lifecycle_wait(&race, &race.reader_done,
				5000000);
			lifecycle_completed = dtls_destination_lifecycle_wait(&race,
				&race.lifecycle_done, 5000000);
			if (!reader_completed || !lifecycle_completed) {
				_exit(1);
			}
			if (switch_thread_join(&reader_thread_status, reader_thread) != SWITCH_STATUS_SUCCESS ||
				switch_thread_join(&lifecycle_thread_status, lifecycle_thread) !=
					SWITCH_STATUS_SUCCESS) {
				_exit(1);
			}
			switch_rtp_pvt_set_lock_test_hooks(rtp, NULL, NULL, NULL, NULL, NULL);
			close(sender_fd);
			if (switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &after) !=
					SWITCH_STATUS_SUCCESS) {
				_exit(1);
			}
			fst_check(dtls_destination_lifecycle_state(&race, &race.dtls_ice_locked) == SWITCH_TRUE);
			fst_check(dtls_destination_lifecycle_state(&race,
				&race.lifecycle_write_locked) == SWITCH_TRUE);
			fst_check(race.lifecycle_status == SWITCH_STATUS_SUCCESS);
			fst_check(lifecycle_thread_status == SWITCH_STATUS_SUCCESS);
			fst_check(reader_thread_status == SWITCH_STATUS_SUCCESS);
			fst_check(after.rtp_remote_port == 19691);

			cleanup_session_media_and_sdp(session, sdp_session, parser);
			bounded_test_watchdog_stop(&watchdog, watchdog_thread);
		}
		FCT_TEST_END();

		FCT_TEST_BGN(ice_restart_teardown_partial_worker_start_is_bounded)
		{
			dtls_restart_teardown_race_t race;
			bounded_test_watchdog_t watchdog;
			switch_threadattr_t *thread_attr = NULL;
			switch_thread_t *watchdog_thread = NULL;
			switch_thread_t *threads[4] = { NULL };
			switch_status_t status;
			const char *inject_cleanup_timeout;
			int fail_at;
			int started;

			inject_cleanup_timeout = getenv("SWITCH_TRICKLE_ICE_INJECT_CLEANUP_TIMEOUT");
			fst_requires(switch_threadattr_create(&thread_attr, pool) == SWITCH_STATUS_SUCCESS);
			if (bounded_test_watchdog_start(&watchdog, thread_attr, pool, 15000000,
					"partial worker cleanup", &watchdog_thread) != SWITCH_STATUS_SUCCESS) {
				_exit(1);
			}
			for (fail_at = 0; fail_at < 4; ++fail_at) {
				memset(&race, 0, sizeof(race));
				if (!zstr(inject_cleanup_timeout) && fail_at == 1) {
					race.abort_completion_delay = 1000000;
				}
				if (switch_mutex_init(&race.mutex, SWITCH_MUTEX_NESTED, pool) !=
						SWITCH_STATUS_SUCCESS ||
					switch_thread_cond_create(&race.cond, pool) != SWITCH_STATUS_SUCCESS) {
					_exit(1);
				}
				status = dtls_restart_teardown_start_workers(&race, thread_attr, pool, threads,
					fail_at, &started);
				if (status != SWITCH_STATUS_FALSE || started != fail_at ||
					dtls_restart_teardown_abort_and_join(&race, threads, started,
						race.abort_completion_delay ? 1000 : 5000000) != SWITCH_STATUS_SUCCESS) {
					_exit(1);
				}
				fst_check(race.completed == started);
			}
			bounded_test_watchdog_stop(&watchdog, watchdog_thread);
		}
		FCT_TEST_END();

		FCT_TEST_BGN(ice_restart_timeout_serializes_concurrent_ice_dtls_and_teardown)
		{
			switch_core_session_t *session = NULL;
			switch_channel_t *channel = NULL;
			switch_rtp_t *rtp = NULL;
			void *sdp_session = NULL;
			sdp_parser_t *parser = NULL;
			switch_rtp_pvt_transport_snapshot_t migrated;
			switch_rtp_pvt_transport_snapshot_t destroyed;
			switch_rtp_pvt_transport_snapshot_t destroyed_again;
			dtls_restart_teardown_race_t race;
			bounded_test_watchdog_t watchdog;
			switch_threadattr_t *thread_attr = NULL;
			switch_thread_t *watchdog_thread = NULL;
			switch_thread_t *threads[4] = { NULL };
			switch_status_t thread_status[4];
			switch_status_t create_status;
			char initial_sdp[2048];
			char restart_sdp[2048];
			char ice_user[513] = "";
			char incoming_user[513] = "";
			char local_pwd[256] = "";
			char remote_pwd[256] = "";
			switch_bool_t has_addr = SWITCH_FALSE;
			switch_status_t status;
			switch_status_t wait_status = SWITCH_STATUS_SUCCESS;
			switch_time_t deadline;
			switch_interval_time_t remaining;
			int started = 0;
			int i;

			memset(&race, 0, sizeof(race));
			memset(&destroyed, 0, sizeof(destroyed));
			memset(&destroyed_again, 0, sizeof(destroyed_again));
			race.ice_status = SWITCH_STATUS_FALSE;
			race.dtls_status = SWITCH_STATUS_SUCCESS;
			race.identity_status = SWITCH_STATUS_FALSE;
			race.teardown_status = SWITCH_STATUS_FALSE;
			race.preparation_result = SWITCH_RTP_PVT_DTLS_RESTART_RESET;
			race.dtls_packet[0] = 22;
			fst_requires(build_dtls_pair_sdp(initial_sdp, sizeof(initial_sdp), 1683118240,
				18610, "active", "raceRemoteUfrag1", "raceRemotePassword1234561") ==
				SWITCH_STATUS_SUCCESS);
			fst_requires(build_dtls_pair_sdp(restart_sdp, sizeof(restart_sdp), 1683118241,
				18611, "active", "raceRemoteUfrag2", "raceRemotePassword1234562") ==
				SWITCH_STATUS_SUCCESS);
			status = make_session_and_rtp_with_sdp_ex(&session, &rtp, &sdp_session, &parser,
				initial_sdp, "PCMU", SWITCH_TRUE, SWITCH_FALSE, "controlled");
			fst_requires(status == SWITCH_STATUS_SUCCESS && session && !rtp);
			channel = switch_core_session_get_channel(session);
			fst_requires(channel != NULL);
			switch_channel_set_variable(channel, "media_dtls_setup_timeout", "100");
			status = switch_core_media_activate_rtp(session);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			rtp = switch_core_media_get_rtp_session(session, SWITCH_MEDIA_TYPE_AUDIO);
			fst_requires(rtp != NULL);
			status = restart_and_nominate_controlled_dtls(session, rtp, restart_sdp, 18611, &migrated);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check(migrated.dtls_restart_migrated == SWITCH_TRUE);

			status = switch_rtp_pvt_get_ice_state(rtp, IPR_RTP,
				ice_user, sizeof(ice_user), local_pwd, sizeof(local_pwd),
				remote_pwd, sizeof(remote_pwd), &has_addr);
			fst_requires(status == SWITCH_STATUS_SUCCESS && has_addr == SWITCH_TRUE);
			fst_requires(reverse_ice_username(ice_user, incoming_user,
				sizeof(incoming_user)) == SWITCH_STATUS_SUCCESS);
			race.stun_len = build_authenticated_ice_request(race.stun_packet,
				sizeof(race.stun_packet), incoming_user, local_pwd);
			race.rtp = rtp;
			race.session = session;
			fst_requires(switch_mutex_init(&race.mutex, SWITCH_MUTEX_NESTED, pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_thread_cond_create(&race.cond, pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_threadattr_create(&thread_attr, pool) == SWITCH_STATUS_SUCCESS);
			if (bounded_test_watchdog_start(&watchdog, thread_attr, pool, 20000000,
					"restart teardown concurrency", &watchdog_thread) != SWITCH_STATUS_SUCCESS) {
				_exit(1);
			}

			/* Serialize all transport operations through teardown. */
			switch_sleep(150000);
			create_status = dtls_restart_teardown_start_workers(&race, thread_attr, pool,
				threads, -1, &started);
			if (create_status != SWITCH_STATUS_SUCCESS) {
				if (dtls_restart_teardown_abort_and_join(&race, threads, started, 5000000) !=
					SWITCH_STATUS_SUCCESS) {
					_exit(1);
				}
				_exit(1);
			}
			if (started != 4) {
				_exit(1);
			}
			switch_mutex_lock(race.mutex);
			deadline = switch_micro_time_now() + 5000000;
			while (race.ready < 4 && wait_status != SWITCH_STATUS_TIMEOUT) {
				remaining = deadline - switch_micro_time_now();
				if (remaining <= 0) {
					wait_status = SWITCH_STATUS_TIMEOUT;
					break;
				}
				wait_status = switch_thread_cond_timedwait(race.cond, race.mutex, remaining);
			}
			if (race.ready != 4) {
				switch_mutex_unlock(race.mutex);
				_exit(1);
			}
			race.start = 1;
			switch_thread_cond_broadcast(race.cond);
			switch_mutex_unlock(race.mutex);

			wait_status = SWITCH_STATUS_SUCCESS;
			switch_mutex_lock(race.mutex);
			deadline = switch_micro_time_now() + 5000000;
			while (race.completed < 4 && wait_status != SWITCH_STATUS_TIMEOUT) {
				remaining = deadline - switch_micro_time_now();
				if (remaining <= 0) {
					wait_status = SWITCH_STATUS_TIMEOUT;
					break;
				}
				wait_status = switch_thread_cond_timedwait(race.cond, race.mutex, remaining);
			}
			if (race.completed != 4) {
				switch_mutex_unlock(race.mutex);
				_exit(1);
			}
			switch_mutex_unlock(race.mutex);
			for (i = 0; i < 4; ++i) {
				if (switch_thread_join(&thread_status[i], threads[i]) != SWITCH_STATUS_SUCCESS ||
					thread_status[i] != SWITCH_STATUS_SUCCESS) {
					_exit(1);
				}
			}
			fst_check(race.ice_status == SWITCH_STATUS_SUCCESS || race.ice_status == SWITCH_STATUS_FALSE);
			fst_check(race.dtls_status == SWITCH_STATUS_FALSE);
			fst_check(race.identity_status == SWITCH_STATUS_SUCCESS ||
				race.identity_status == SWITCH_STATUS_FALSE);
			if (race.identity_status == SWITCH_STATUS_SUCCESS) {
				fst_check(race.preparation_result == SWITCH_RTP_PVT_DTLS_RESTART_PRESERVE ||
					race.preparation_result == SWITCH_RTP_PVT_DTLS_RESTART_RESET ||
					race.preparation_result == SWITCH_RTP_PVT_DTLS_RESTART_EXPIRED);
			}
			fst_check(race.teardown_status == SWITCH_STATUS_SUCCESS);
			for (i = 0; i < 4; ++i) {
				fst_check(thread_status[i] == SWITCH_STATUS_SUCCESS);
			}
			if (switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &destroyed) !=
					SWITCH_STATUS_SUCCESS) {
				_exit(1);
			}
			fst_check(destroyed.dtls_context == NULL);
			fst_check(destroyed.dtls_destroy_count == 1);

			switch_core_media_deactivate_rtp(session);
			if (switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &destroyed_again) !=
					SWITCH_STATUS_SUCCESS) {
				_exit(1);
			}
			fst_check(destroyed_again.dtls_context == NULL);
			fst_check(destroyed_again.dtls_destroy_count == destroyed.dtls_destroy_count);
			if (parser) {
				sdp_parser_free(parser);
			}
			switch_core_session_rwunlock(session);
			bounded_test_watchdog_stop(&watchdog, watchdog_thread);
		}
		FCT_TEST_END();

		FCT_TEST_BGN(ice_restart_preserves_only_exact_dtls_association_identity)
		{
			dtls_restart_case_result_t result;
			switch_status_t status;
			char *no_mux_sdp;
			char *no_bundle_sdp;
			char *component_changed_sdp;
			char *bundle_membership_changed_sdp;
			char *bundle_tag_changed_sdp;
			char *bundle_group_sdp;
			char *fingerprint_algorithm_changed_sdp;
			char *tls_id_removed_sdp;
			char *initial_without_tls_id_sdp;
			char *stale_ufrag_sdp;
			char *stale_pwd_sdp;
			char non_bundle_initial_sdp[2048];
			char non_bundle_restart_sdp[2048];
			const char *initial_sdp =
				"v=0\n"
				"o=- 1683118194 1683118195 IN IP4 0.0.0.0\n"
				"s=-\n"
				"t=0 0\n"
				"a=group:BUNDLE 0\n"
				"m=audio 18225 UDP/TLS/RTP/SAVPF 0\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:identityRemoteUfrag1\n"
				"a=ice-pwd:identityRemotePassword1234561\n"
				"a=ice-options:trickle\n"
				"a=candidate:1 1 udp 2130706431 127.0.0.1 18225 typ host\n"
				"a=rtcp-mux\n"
				"a=setup:active\n"
				"a=tls-id:association-1\n"
				"a=fingerprint:sha-256 17:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=fingerprint:sha-384 01:02:03:04:05:06:07:08:09:0A:0B:0C:0D:0E:0F:10:11:12:13:14:15:16:17:18:19:1A:1B:1C:1D:1E:1F:20:21:22:23:24:25:26:27:28:29:2A:2B:2C:2D:2E:2F:30\n"
				"a=rtpmap:0 PCMU/8000\n"
				"a=sendrecv\n"
				"a=mid:0\n";
			const char *same_identity_sdp =
				"v=0\n"
				"o=- 1683118194 1683118196 IN IP4 0.0.0.0\n"
				"s=-\n"
				"t=0 0\n"
				"a=group:BUNDLE 0\n"
				"m=audio 18226 UDP/TLS/RTP/SAVPF 0\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:identityRemoteUfrag2\n"
				"a=ice-pwd:identityRemotePassword1234562\n"
				"a=candidate:2 1 udp 2130706430 127.0.0.1 18226 typ host\n"
				"a=rtcp-mux\n"
				"a=setup:active\n"
				"a=tls-id:association-1\n"
				"a=fingerprint:sha-384 01:02:03:04:05:06:07:08:09:0A:0B:0C:0D:0E:0F:10:11:12:13:14:15:16:17:18:19:1A:1B:1C:1D:1E:1F:20:21:22:23:24:25:26:27:28:29:2A:2B:2C:2D:2E:2F:30\n"
				"a=fingerprint:sha-256 17:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=rtpmap:0 PCMU/8000\n"
				"a=sendrecv\n"
				"a=mid:0\n";
			const char *fingerprint_set_changed_sdp =
				"v=0\n"
				"o=- 1683118194 1683118196 IN IP4 0.0.0.0\n"
				"s=-\n"
				"t=0 0\n"
				"a=group:BUNDLE 0\n"
				"m=audio 18226 UDP/TLS/RTP/SAVPF 0\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:identityRemoteUfrag2\n"
				"a=ice-pwd:identityRemotePassword1234562\n"
				"a=candidate:2 1 udp 2130706430 127.0.0.1 18226 typ host\n"
				"a=rtcp-mux\n"
				"a=setup:active\n"
				"a=tls-id:association-1\n"
				"a=fingerprint:sha-256 17:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=rtpmap:0 PCMU/8000\n"
				"a=sendrecv\n"
				"a=mid:0\n";
			const char *tls_id_changed_sdp =
				"v=0\n"
				"o=- 1683118194 1683118196 IN IP4 0.0.0.0\n"
				"s=-\n"
				"t=0 0\n"
				"a=group:BUNDLE 0\n"
				"m=audio 18226 UDP/TLS/RTP/SAVPF 0\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:identityRemoteUfrag2\n"
				"a=ice-pwd:identityRemotePassword1234562\n"
				"a=candidate:2 1 udp 2130706430 127.0.0.1 18226 typ host\n"
				"a=rtcp-mux\n"
				"a=setup:active\n"
				"a=tls-id:association-2\n"
				"a=fingerprint:sha-256 17:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=fingerprint:sha-384 01:02:03:04:05:06:07:08:09:0A:0B:0C:0D:0E:0F:10:11:12:13:14:15:16:17:18:19:1A:1B:1C:1D:1E:1F:20:21:22:23:24:25:26:27:28:29:2A:2B:2C:2D:2E:2F:30\n"
				"a=rtpmap:0 PCMU/8000\n"
				"a=sendrecv\n"
				"a=mid:0\n";
			const char *role_changed_sdp =
				"v=0\n"
				"o=- 1683118194 1683118196 IN IP4 0.0.0.0\n"
				"s=-\n"
				"t=0 0\n"
				"a=group:BUNDLE 0\n"
				"a=ice-lite\n"
				"m=audio 18226 UDP/TLS/RTP/SAVPF 0\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:identityRemoteUfrag2\n"
				"a=ice-pwd:identityRemotePassword1234562\n"
				"a=candidate:2 1 udp 2130706430 127.0.0.1 18226 typ host\n"
				"a=rtcp-mux\n"
				"a=setup:passive\n"
				"a=tls-id:association-1\n"
				"a=fingerprint:sha-256 17:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=fingerprint:sha-384 01:02:03:04:05:06:07:08:09:0A:0B:0C:0D:0E:0F:10:11:12:13:14:15:16:17:18:19:1A:1B:1C:1D:1E:1F:20:21:22:23:24:25:26:27:28:29:2A:2B:2C:2D:2E:2F:30\n"
				"a=rtpmap:0 PCMU/8000\n"
				"a=sendrecv\n"
				"a=mid:0\n";
			const char *stale_credentials_sdp =
				"v=0\n"
				"o=- 1683118194 1683118196 IN IP4 0.0.0.0\n"
				"s=-\n"
				"t=0 0\n"
				"a=group:BUNDLE 0\n"
				"m=audio 18226 UDP/TLS/RTP/SAVPF 0\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:identityRemoteUfrag1\n"
				"a=ice-pwd:identityRemotePassword1234561\n"
				"a=candidate:2 1 udp 2130706430 127.0.0.1 18226 typ host\n"
				"a=rtcp-mux\n"
				"a=setup:active\n"
				"a=tls-id:association-1\n"
				"a=fingerprint:sha-256 17:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=fingerprint:sha-384 01:02:03:04:05:06:07:08:09:0A:0B:0C:0D:0E:0F:10:11:12:13:14:15:16:17:18:19:1A:1B:1C:1D:1E:1F:20:21:22:23:24:25:26:27:28:29:2A:2B:2C:2D:2E:2F:30\n"
				"a=rtpmap:0 PCMU/8000\n"
				"a=sendrecv\n"
				"a=mid:0\n";
			const char *bundled_video_tail =
				"m=video 18226 UDP/TLS/RTP/SAVPF 31\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:identityRemoteUfrag2\n"
				"a=ice-pwd:identityRemotePassword1234562\n"
				"a=candidate:2 1 udp 2130706430 127.0.0.1 18226 typ host\n"
				"a=rtcp-mux\n"
				"a=setup:active\n"
				"a=tls-id:association-1\n"
				"a=fingerprint:sha-256 17:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=rtpmap:31 PROXY-VID/90000\n"
				"a=sendrecv\n"
				"a=mid:1\n";

			no_mux_sdp = switch_string_replace(same_identity_sdp, "a=rtcp-mux\n", "");
			no_bundle_sdp = switch_string_replace(same_identity_sdp, "a=group:BUNDLE 0\n", "");
			component_changed_sdp = switch_string_replace(no_mux_sdp,
				"a=candidate:2 1 udp 2130706430 127.0.0.1 18226 typ host\n",
				"a=candidate:2 1 udp 2130706430 127.0.0.1 18226 typ host\n"
				"a=candidate:3 2 udp 2130706429 127.0.0.1 18227 typ host\n"
				"a=rtcp:18227 IN IP4 127.0.0.1\n");
			bundle_group_sdp = switch_string_replace(same_identity_sdp,
				"a=group:BUNDLE 0\n", "a=group:BUNDLE 0 1\n");
			bundle_membership_changed_sdp = switch_mprintf("%s%s", bundle_group_sdp, bundled_video_tail);
			switch_safe_free(bundle_group_sdp);
			bundle_group_sdp = switch_string_replace(same_identity_sdp,
				"a=group:BUNDLE 0\n", "a=group:BUNDLE 1 0\n");
			bundle_tag_changed_sdp = switch_mprintf("%s%s", bundle_group_sdp, bundled_video_tail);
			switch_safe_free(bundle_group_sdp);
			fingerprint_algorithm_changed_sdp = switch_string_replace(same_identity_sdp, "sha-384", "sha-512");
			tls_id_removed_sdp = switch_string_replace(same_identity_sdp,
				"a=tls-id:association-1\n", "");
			initial_without_tls_id_sdp = switch_string_replace(initial_sdp,
				"a=tls-id:association-1\n", "");
			stale_ufrag_sdp = switch_string_replace(same_identity_sdp,
				"identityRemoteUfrag2", "identityRemoteUfrag1");
			stale_pwd_sdp = switch_string_replace(same_identity_sdp,
				"identityRemotePassword1234562", "identityRemotePassword1234561");
			fst_requires(no_mux_sdp != NULL && no_bundle_sdp != NULL && component_changed_sdp != NULL &&
				bundle_membership_changed_sdp != NULL && bundle_tag_changed_sdp != NULL &&
				fingerprint_algorithm_changed_sdp != NULL && tls_id_removed_sdp != NULL &&
				initial_without_tls_id_sdp != NULL && stale_ufrag_sdp != NULL && stale_pwd_sdp != NULL);
			fst_requires(build_dtls_pair_sdp_ex(non_bundle_initial_sdp,
				sizeof(non_bundle_initial_sdp), 1683118260, 18810, "active",
				"nonBundleIdentityUfrag1", "nonBundleIdentityPassword1234561", SWITCH_FALSE) ==
				SWITCH_STATUS_SUCCESS);
			fst_requires(build_dtls_pair_sdp_ex(non_bundle_restart_sdp,
				sizeof(non_bundle_restart_sdp), 1683118261, 18811, "active",
				"nonBundleIdentityUfrag2", "nonBundleIdentityPassword1234562", SWITCH_FALSE) ==
				SWITCH_STATUS_SUCCESS);

			status = run_dtls_restart_identity_case(initial_sdp, same_identity_sdp, 0, 0,
				SWITCH_FALSE, &result);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check(result.preserved == SWITCH_TRUE);
			fst_check(result.before_port == 18225 && result.after_port == 18225);

			status = run_dtls_restart_identity_case(initial_sdp, fingerprint_set_changed_sdp, 0, 0,
				SWITCH_FALSE, &result);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check(result.preserved == SWITCH_FALSE);

			status = run_dtls_restart_identity_case(initial_sdp, fingerprint_algorithm_changed_sdp, 0, 0,
				SWITCH_FALSE, &result);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check(result.preserved == SWITCH_FALSE);

			status = run_dtls_restart_identity_case(initial_sdp, tls_id_changed_sdp, 0, 0,
				SWITCH_FALSE, &result);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check(result.preserved == SWITCH_FALSE);

			status = run_dtls_restart_identity_case(initial_sdp, tls_id_removed_sdp, 0, 0,
				SWITCH_FALSE, &result);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check(result.preserved == SWITCH_FALSE);

			status = run_dtls_restart_identity_case(initial_without_tls_id_sdp,
				same_identity_sdp, 0, 0, SWITCH_FALSE, &result);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check(result.preserved == SWITCH_FALSE);

			status = run_dtls_restart_identity_case(initial_sdp, same_identity_sdp, 0, 0,
				SWITCH_TRUE, &result);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check(result.preserved == SWITCH_FALSE);

			status = run_dtls_restart_identity_case(initial_sdp, role_changed_sdp, 0, 0,
				SWITCH_FALSE, &result);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check(result.preserved == SWITCH_FALSE);
			fst_check((result.before_ice_type & ICE_LITE_INBOUND) == 0);
			fst_check((result.after_ice_type & ICE_LITE_INBOUND) != 0);

			status = run_dtls_restart_identity_case(initial_sdp, stale_credentials_sdp, 0, 0,
				SWITCH_FALSE, &result);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check(result.preserved == SWITCH_FALSE);

			status = run_dtls_restart_identity_case(initial_sdp, stale_ufrag_sdp, 0, 0,
				SWITCH_FALSE, &result);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check(result.preserved == SWITCH_FALSE);

			status = run_dtls_restart_identity_case(initial_sdp, stale_pwd_sdp, 0, 0,
				SWITCH_FALSE, &result);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check(result.preserved == SWITCH_FALSE);

			status = run_dtls_restart_identity_case(initial_sdp, same_identity_sdp, 1, 3000,
				SWITCH_FALSE, &result);
			fst_requires(status == SWITCH_STATUS_TIMEOUT);
			fst_check(result.failed_closed == SWITCH_TRUE);
			fst_check(result.before_port == 18225 && result.after_port == 18225);

			status = run_dtls_restart_identity_case(initial_sdp, no_mux_sdp, 0, 0,
				SWITCH_FALSE, &result);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check(result.preserved == SWITCH_FALSE);

			status = run_dtls_restart_identity_case(initial_sdp, no_bundle_sdp, 0, 0,
				SWITCH_FALSE, &result);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check(result.preserved == SWITCH_FALSE);

			status = run_dtls_restart_identity_case(non_bundle_initial_sdp,
				non_bundle_restart_sdp, 0, 0, SWITCH_FALSE, &result);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check(result.preserved == SWITCH_TRUE);
			fst_check(result.before_port == 18810 && result.after_port == 18810);

			status = run_dtls_restart_identity_case(initial_sdp, component_changed_sdp, 0, 0,
				SWITCH_FALSE, &result);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check(result.preserved == SWITCH_FALSE);

			status = run_dtls_restart_identity_case(initial_sdp, bundle_membership_changed_sdp, 0, 0,
				SWITCH_FALSE, &result);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check(result.preserved == SWITCH_FALSE);

			status = run_dtls_restart_identity_case(initial_sdp, bundle_tag_changed_sdp, 0, 0,
				SWITCH_FALSE, &result);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check(result.preserved == SWITCH_FALSE);
			switch_safe_free(no_mux_sdp);
			switch_safe_free(no_bundle_sdp);
			switch_safe_free(component_changed_sdp);
			switch_safe_free(bundle_membership_changed_sdp);
			switch_safe_free(bundle_tag_changed_sdp);
			switch_safe_free(fingerprint_algorithm_changed_sdp);
			switch_safe_free(tls_id_removed_sdp);
			switch_safe_free(initial_without_tls_id_sdp);
			switch_safe_free(stale_ufrag_sdp);
			switch_safe_free(stale_pwd_sdp);
		}
		FCT_TEST_END();

		FCT_TEST_BGN(bundled_video_reinvite_preserves_audio_owned_ice_credentials)
		{
			switch_core_session_t *session = NULL;
			switch_channel_t *channel = NULL;
			switch_media_handle_t *smh = NULL;
			switch_rtp_t *audio_rtp = NULL;
			switch_rtp_t *video_rtp = NULL;
			void *sdp_session = NULL;
			sdp_parser_t *parser = NULL;
			switch_status_t status;
			uint8_t match;
			uint8_t proceed = 0;
			char advertised_audio_ufrag[256] = "";
			char advertised_audio_pwd[256] = "";
			char advertised_video_ufrag[256] = "";
			char advertised_video_pwd[256] = "";
			char active_ice_user[513] = "";
			char active_local_pwd[256] = "";
			char active_remote_pwd[256] = "";
			const char *active_local_ufrag;
			const char *local_sdp;
			switch_bool_t has_addr = SWITCH_FALSE;
			const char *initial_sdp =
				"v=0\n"
				"o=- 1683118194 1683118195 IN IP4 0.0.0.0\n"
				"s=-\n"
				"t=0 0\n"
				"a=group:BUNDLE audio video\n"
				"a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid\n"
				"m=audio 18215 UDP/TLS/RTP/SAVPF 0\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:bundleRemoteUfrag1\n"
				"a=ice-pwd:bundleRemotePassword1234561\n"
				"a=ice-options:trickle\n"
				"a=candidate:1 1 udp 2130706431 127.0.0.1 18215 typ host\n"
				"a=rtcp-mux\n"
				"a=setup:active\n"
				"a=rtpmap:0 PCMU/8000\n"
				"a=sendrecv\n"
				"a=fingerprint:sha-256 17:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=mid:audio\n"
				"m=video 18215 UDP/TLS/RTP/SAVPF 31\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:bundleRemoteUfrag1\n"
				"a=ice-pwd:bundleRemotePassword1234561\n"
				"a=ice-options:trickle\n"
				"a=candidate:1 1 udp 2130706431 127.0.0.1 18215 typ host\n"
				"a=rtcp-mux\n"
				"a=setup:active\n"
				"a=rtpmap:31 PROXY-VID/90000\n"
				"a=sendrecv\n"
				"a=fingerprint:sha-256 17:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=mid:video\n";
			const char *restart_sdp =
				"v=0\n"
				"o=- 1683118194 1683118196 IN IP4 0.0.0.0\n"
				"s=-\n"
				"t=0 0\n"
				"a=group:BUNDLE audio video\n"
				"a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid\n"
				"m=audio 18215 UDP/TLS/RTP/SAVPF 0\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:bundleRemoteUfrag2\n"
				"a=ice-pwd:bundleRemotePassword1234562\n"
				"a=ice-options:trickle\n"
				"a=candidate:1 1 udp 2130706431 127.0.0.1 18215 typ host\n"
				"a=rtcp-mux\n"
				"a=setup:active\n"
				"a=rtpmap:0 PCMU/8000\n"
				"a=sendrecv\n"
				"a=fingerprint:sha-256 17:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=mid:audio\n"
				"m=video 18215 UDP/TLS/RTP/SAVPF 31\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:bundleRemoteUfrag2\n"
				"a=ice-pwd:bundleRemotePassword1234562\n"
				"a=ice-options:trickle\n"
				"a=candidate:1 1 udp 2130706431 127.0.0.1 18215 typ host\n"
				"a=rtcp-mux\n"
				"a=setup:active\n"
				"a=rtpmap:31 PROXY-VID/90000\n"
				"a=sendrecv\n"
				"a=fingerprint:sha-256 17:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=mid:video\n";

			status = make_session_and_rtp_with_sdp_ex(&session, &audio_rtp, &sdp_session, &parser,
				initial_sdp, "PCMU,PROXY-VID", SWITCH_TRUE, SWITCH_FALSE, NULL);
			fst_requires(status == SWITCH_STATUS_SUCCESS && session && !audio_rtp && sdp_session && parser);
			channel = switch_core_session_get_channel(session);
			smh = switch_core_session_get_media_handle(session);
			fst_requires(channel != NULL && smh != NULL);
			switch_channel_set_flag(channel, CF_VIDEO);
			switch_channel_set_flag(channel, CF_VIDEO_POSSIBLE);
			status = switch_core_media_choose_port(session, SWITCH_MEDIA_TYPE_VIDEO, 0);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			switch_core_media_gen_local_sdp(session, SDP_ANSWER, NULL, 0, NULL, 0);
			local_sdp = switch_channel_get_variable(channel, "rtp_local_sdp_str");
			fst_requires(copy_sdp_media_attribute(local_sdp, "audio", "ice-ufrag", advertised_audio_ufrag,
				sizeof(advertised_audio_ufrag)) == SWITCH_STATUS_SUCCESS);
			fst_requires(copy_sdp_media_attribute(local_sdp, "audio", "ice-pwd", advertised_audio_pwd,
				sizeof(advertised_audio_pwd)) == SWITCH_STATUS_SUCCESS);
			fst_requires(copy_sdp_media_attribute(local_sdp, "video", "ice-ufrag", advertised_video_ufrag,
				sizeof(advertised_video_ufrag)) == SWITCH_STATUS_SUCCESS);
			fst_requires(copy_sdp_media_attribute(local_sdp, "video", "ice-pwd", advertised_video_pwd,
				sizeof(advertised_video_pwd)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(advertised_video_ufrag, advertised_audio_ufrag);
			fst_check_string_equals(advertised_video_pwd, advertised_audio_pwd);

			status = switch_core_media_activate_rtp(session);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			audio_rtp = switch_core_media_get_rtp_session(session, SWITCH_MEDIA_TYPE_AUDIO);
			video_rtp = switch_core_media_get_rtp_session(session, SWITCH_MEDIA_TYPE_VIDEO);
			fst_requires(audio_rtp != NULL && video_rtp == audio_rtp);
			fst_check(switch_core_media_video_is_bundled(session) == SWITCH_TRUE);

			switch_channel_set_flag(channel, CF_REINVITE);
			match = switch_core_media_negotiate_sdp(session, restart_sdp, &proceed, SDP_OFFER);
			fst_requires(match != 0);

			status = switch_rtp_pvt_get_ice_state(audio_rtp, IPR_RTP,
				active_ice_user, sizeof(active_ice_user), active_local_pwd, sizeof(active_local_pwd),
				active_remote_pwd, sizeof(active_remote_pwd), &has_addr);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			active_local_ufrag = strchr(active_ice_user, ':');
			fst_requires(active_local_ufrag != NULL && active_local_ufrag[1] != '\0');
			active_local_ufrag++;
			fst_check(!strncmp(active_ice_user, "bundleRemoteUfrag2:", strlen("bundleRemoteUfrag2:")));
			fst_check_string_equals(active_local_ufrag, advertised_audio_ufrag);
			fst_check_string_equals(active_local_pwd, advertised_audio_pwd);
			fst_check_string_equals(active_remote_pwd, "bundleRemotePassword1234562");

			cleanup_session_media_and_sdp(session, sdp_session, parser);
		}
		FCT_TEST_END();

		FCT_TEST_BGN(rtcp_mux_prflx_late_srflx_preserves_inflight_dtls)
		{
			switch_core_session_t *session = NULL;
			switch_core_session_t *peer_session = NULL;
			switch_channel_t *channel = NULL;
			switch_media_handle_t *smh = NULL;
			switch_rtp_t *rtp = NULL;
			switch_rtp_t *peer_rtp = NULL;
			void *sdp_session = NULL;
			void *peer_sdp_session = NULL;
			sdp_parser_t *parser = NULL;
			sdp_parser_t *peer_parser = NULL;
			switch_status_t status;
			switch_rtp_pvt_transport_snapshot_t before;
			switch_rtp_pvt_transport_snapshot_t candidate_ready;
			switch_rtp_pvt_transport_snapshot_t nominated;
			switch_rtp_pvt_transport_snapshot_t after;
			switch_rtp_pvt_transport_snapshot_t handshake_restart;
			switch_rtp_pvt_transport_snapshot_t migrated_restart;
			switch_rtp_pvt_transport_snapshot_t ready_restart;
			char ice_user[256] = "";
			char incoming_user[256] = "";
			char local_pwd[256] = "";
			char remote_pwd[256] = "";
			char nominated_host[80] = "";
			char after_host[80] = "";
			char *chosen_addr = NULL;
			switch_port_t chosen_port = 0;
			switch_port_t nominated_port = 0;
			switch_port_t after_port = 0;
			switch_bool_t has_addr = SWITCH_FALSE;
			uint8_t stun_packet[512];
			switch_size_t stun_len;
			char handshake_restart_sdp[2048];
			char ready_restart_sdp[2048];
			char peer_sdp[2048];
			uint8_t match;
			uint8_t proceed = 0;

			status = make_session_and_rtp_with_sdp_ex(&session, &rtp, &sdp_session, &parser,
				NULL, "PCMU", SWITCH_TRUE, SWITCH_FALSE, NULL);
			fst_requires(status == SWITCH_STATUS_SUCCESS && session && !rtp && sdp_session && parser);
			channel = switch_core_session_get_channel(session);
			smh = switch_core_session_get_media_handle(session);
			fst_requires(channel != NULL && smh != NULL);

			switch_channel_set_variable(channel, "rtp_ice_prflx_bootstrap", "true");
			switch_channel_set_variable(channel, "rtp_ice_prflx_bootstrap_ms", "5000");
			switch_channel_set_variable(channel, "rtp_ice_role", "controlled");
			status = switch_core_media_activate_rtp(session);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			rtp = switch_core_media_get_rtp_session(session, SWITCH_MEDIA_TYPE_AUDIO);
			fst_requires(rtp != NULL);

			status = switch_rtp_pvt_get_ice_state(rtp, IPR_RTP,
				ice_user, sizeof(ice_user), local_pwd, sizeof(local_pwd),
				remote_pwd, sizeof(remote_pwd), &has_addr);
			fst_requires(status == SWITCH_STATUS_SUCCESS && has_addr == SWITCH_FALSE);
			fst_requires(reverse_ice_username(ice_user, incoming_user,
				sizeof(incoming_user)) == SWITCH_STATUS_SUCCESS);
			status = switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &before);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_requires((before.ice_type & ICE_CONTROLLED) != 0);
			fst_requires(before.dtls_state == DS_HANDSHAKE);
			fst_requires(before.dtls_context != NULL && before.dtls_ssl != NULL &&
				before.socket != NULL);
			fst_check(before.rtp_chosen == SWITCH_FALSE);
			fst_check(before.rtcp_chosen == SWITCH_FALSE);

			status = switch_core_media_trickle_remote_candidate_and_recheck(
				session, smh, sdp_session, SDP_TYPE_REQUEST, "0", 0,
				"candidate:265031752 1 udp 2122260223 192.0.2.10 54302 typ host", 0);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			status = switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &candidate_ready);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check((candidate_ready.ice_type & ICE_CONTROLLED) != 0);
			fst_check(candidate_ready.rtp_chosen == SWITCH_FALSE);
			fst_check(candidate_ready.dtls_state == DS_HANDSHAKE);
			fst_check(candidate_ready.dtls_context == before.dtls_context);
			fst_check(candidate_ready.dtls_ssl == before.dtls_ssl);

			stun_len = build_authenticated_ice_request(stun_packet, sizeof(stun_packet),
				incoming_user, local_pwd);
			status = switch_rtp_pvt_handle_ice_from(rtp, IPR_RTP, "192.0.2.10", 54302,
				stun_packet, stun_len);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			status = switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &nominated);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_requires(nominated.ice_ready == SWITCH_TRUE && nominated.ice_rready == SWITCH_TRUE);
			fst_requires(nominated.rtp_chosen == SWITCH_TRUE);
			fst_check(nominated.rtcp_chosen == SWITCH_FALSE);
			fst_check(nominated.dtls_state == DS_HANDSHAKE);
			fst_check(nominated.dtls_context == before.dtls_context);
			fst_check(nominated.dtls_ssl == before.dtls_ssl);
			fst_check(nominated.socket == before.socket);
			status = switch_core_media_get_chosen_ice_candidate(session, SWITCH_MEDIA_TYPE_AUDIO,
				&chosen_addr, &chosen_port);
			fst_requires(status == SWITCH_STATUS_SUCCESS && chosen_addr != NULL);
			fst_check_string_equals(chosen_addr, "192.0.2.10");
			fst_check(chosen_port == 54302);
			status = copy_sockaddr_tuple(switch_rtp_session_get_remote_addr(rtp), nominated_host,
				sizeof(nominated_host), &nominated_port);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(nominated_host, "192.0.2.10");
			fst_check(nominated_port == 54302);

			status = switch_core_media_trickle_remote_candidate_and_recheck(
				session, smh, sdp_session, SDP_TYPE_REQUEST, "0", 0,
				"candidate:265031753 1 udp 1685921533 198.51.100.20 54302 typ srflx raddr 192.0.2.10 rport 54302", 0);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			status = switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP, &after);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check(after.ice_ready == nominated.ice_ready);
			fst_check(after.ice_rready == nominated.ice_rready);
			fst_check(after.rtp_chosen == nominated.rtp_chosen);
			fst_check(after.rtcp_chosen == nominated.rtcp_chosen);
			fst_check(after.dtls_state == DS_HANDSHAKE);
			fst_check(after.dtls_context == before.dtls_context);
			fst_check(after.dtls_ssl == before.dtls_ssl);
			fst_check(after.socket == before.socket);
			status = switch_core_media_get_chosen_ice_candidate(session, SWITCH_MEDIA_TYPE_AUDIO,
				&chosen_addr, &chosen_port);
			fst_requires(status == SWITCH_STATUS_SUCCESS && chosen_addr != NULL);
			fst_check_string_equals(chosen_addr, "192.0.2.10");
			fst_check(chosen_port == 54302);
			status = copy_sockaddr_tuple(switch_rtp_session_get_remote_addr(rtp), after_host,
				sizeof(after_host), &after_port);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(after_host, nominated_host);
			fst_check(after_port == nominated_port);

			fst_requires(build_dtls_pair_sdp(handshake_restart_sdp,
				sizeof(handshake_restart_sdp), 1683118270, 54303, "active",
				"candidateLessRemoteUfrag2", "candidateLessRemotePassword1234562") ==
				SWITCH_STATUS_SUCCESS);
			switch_core_media_clear_ice(session);
			switch_channel_set_flag(channel, CF_REINVITE);
			match = switch_core_media_negotiate_sdp(session, handshake_restart_sdp, &proceed,
				SDP_OFFER);
			fst_requires(match != 0);
			switch_core_media_gen_local_sdp(session, SDP_ANSWER, NULL, 0, NULL, 0);
			fst_requires(switch_core_media_activate_rtp(session) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP,
				&handshake_restart) == SWITCH_STATUS_SUCCESS);
			fst_check(handshake_restart.dtls_state == DS_HANDSHAKE);
			fst_check(handshake_restart.dtls_context == before.dtls_context);
			fst_check(handshake_restart.dtls_ssl == before.dtls_ssl);
			fst_check(handshake_restart.dtls_restart_pending == SWITCH_TRUE);

			memset(ice_user, 0, sizeof(ice_user));
			memset(incoming_user, 0, sizeof(incoming_user));
			memset(local_pwd, 0, sizeof(local_pwd));
			memset(remote_pwd, 0, sizeof(remote_pwd));
			fst_requires(switch_rtp_pvt_get_ice_state(rtp, IPR_RTP,
				ice_user, sizeof(ice_user), local_pwd, sizeof(local_pwd),
				remote_pwd, sizeof(remote_pwd), &has_addr) == SWITCH_STATUS_SUCCESS);
			fst_requires(reverse_ice_username(ice_user, incoming_user,
				sizeof(incoming_user)) == SWITCH_STATUS_SUCCESS);
			stun_len = build_authenticated_ice_request(stun_packet, sizeof(stun_packet),
				incoming_user, local_pwd);
			fst_requires(switch_rtp_pvt_handle_ice_from(rtp, IPR_RTP, "127.0.0.1", 54303,
				stun_packet, stun_len) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP,
				&migrated_restart) == SWITCH_STATUS_SUCCESS);
			fst_check(migrated_restart.dtls_context == before.dtls_context);
			fst_check(migrated_restart.dtls_ssl == before.dtls_ssl);
			fst_check(migrated_restart.dtls_restart_pending == SWITCH_FALSE);
			fst_check(migrated_restart.dtls_restart_migrated == SWITCH_TRUE);

			fst_requires(build_dtls_pair_sdp(peer_sdp, sizeof(peer_sdp), 1683118271,
				54310, "passive", "candidateLessPeerUfrag1",
				"candidateLessPeerPassword1234561") == SWITCH_STATUS_SUCCESS);
			status = make_session_and_rtp_with_sdp_ex(&peer_session, &peer_rtp,
				&peer_sdp_session, &peer_parser, peer_sdp, "PCMU", SWITCH_TRUE,
				SWITCH_TRUE, NULL);
			fst_requires(status == SWITCH_STATUS_SUCCESS && peer_session && peer_rtp);
			fst_requires(drive_dtls_pair_ready(peer_rtp, rtp) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP,
				&ready_restart) == SWITCH_STATUS_SUCCESS);
			fst_requires(ready_restart.dtls_state == DS_READY);
			fst_check(ready_restart.dtls_context == before.dtls_context);
			fst_check(ready_restart.dtls_ssl == before.dtls_ssl);
			fst_check(ready_restart.dtls_tuple_authoritative == SWITCH_TRUE);
			fst_requires(copy_sockaddr_tuple(switch_rtp_session_get_remote_addr(rtp), after_host,
				sizeof(after_host), &after_port) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(after_host, "127.0.0.1");
			fst_check(after_port == 54303);

			fst_requires(build_dtls_pair_sdp(ready_restart_sdp, sizeof(ready_restart_sdp),
				1683118272, 54304, "active", "candidateLessRemoteUfrag3",
				"candidateLessRemotePassword1234563") == SWITCH_STATUS_SUCCESS);
			switch_core_media_clear_ice(session);
			switch_channel_set_flag(channel, CF_REINVITE);
			proceed = 0;
			match = switch_core_media_negotiate_sdp(session, ready_restart_sdp, &proceed,
				SDP_OFFER);
			fst_requires(match != 0);
			switch_core_media_gen_local_sdp(session, SDP_ANSWER, NULL, 0, NULL, 0);
			fst_requires(switch_core_media_activate_rtp(session) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp, IPR_RTP,
				&ready_restart) == SWITCH_STATUS_SUCCESS);
			fst_check(ready_restart.dtls_state != DS_READY);
			fst_check(ready_restart.dtls_context != before.dtls_context);
			fst_check(ready_restart.srtp_send_ready == SWITCH_FALSE);
			fst_check(ready_restart.srtp_recv_ready == SWITCH_FALSE);

			cleanup_session_media_and_sdp(session, sdp_session, parser);
			cleanup_session_media_and_sdp(peer_session, peer_sdp_session, peer_parser);
		}
		FCT_TEST_END();

		FCT_TEST_BGN(local_answer_recovery_with_ready_candidates_avoids_duplicate_ice_activation)
		{
			switch_core_session_t *session = NULL;
			switch_channel_t *channel = NULL;
			switch_rtp_t *rtp = NULL;
			void *sdp_session = NULL;
			sdp_parser_t *parser = NULL;
			switch_status_t status;
			uint8_t match;
			uint8_t proceed = 0;
			char initial_rtp_user[256] = "";
			char initial_rtp_local_pwd[256] = "";
			char initial_rtp_remote_pwd[256] = "";
			char initial_rtcp_user[256] = "";
			char initial_rtcp_local_pwd[256] = "";
			char initial_rtcp_remote_pwd[256] = "";
			char offered_local_ufrag[256] = "";
			char offered_local_pwd[256] = "";
			char restarted_rtp_user[256] = "";
			char restarted_rtp_local_pwd[256] = "";
			char restarted_rtp_remote_pwd[256] = "";
			char restarted_rtcp_user[256] = "";
			char restarted_rtcp_local_pwd[256] = "";
			char restarted_rtcp_remote_pwd[256] = "";
			const char *initial_local_ufrag;
			const char *restarted_local_ufrag;
			const char *local_sdp;
			switch_bool_t has_addr = SWITCH_FALSE;
			const char *same_remote_sdp =
				"v=0\n"
				"o=- 1683118194 1683118198 IN IP4 0.0.0.0\n"
				"s=-\n"
				"t=0 0\n"
				"m=audio 18215 UDP/TLS/RTP/SAVPF 0\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:stableRemoteUfrag\n"
				"a=ice-pwd:stableRemotePassword123456\n"
				"a=candidate:1 1 udp 2130706431 127.0.0.1 18215 typ host\n"
				"a=candidate:2 2 udp 2130706430 127.0.0.1 18216 typ host\n"
				"a=rtcp:18216 IN IP4 127.0.0.1\n"
				"a=setup:active\n"
				"a=rtpmap:0 PCMU/8000\n"
				"a=sendrecv\n"
				"a=fingerprint:sha-256 17:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=mid:0\n";
			const char *recovery_remote_sdp =
				"v=0\n"
				"o=- 1683118194 1683118199 IN IP4 0.0.0.0\n"
				"s=-\n"
				"t=0 0\n"
				"m=audio 18215 UDP/TLS/RTP/SAVPF 0\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:stableRemoteUfrag\n"
				"a=ice-pwd:stableRemotePassword123456\n"
				"a=candidate:1 1 udp 2130706431 127.0.0.1 18215 typ host\n"
				"a=candidate:2 2 udp 2130706430 127.0.0.1 18216 typ host\n"
				"a=rtcp:18216 IN IP4 127.0.0.1\n"
				"a=setup:active\n"
				"a=rtpmap:0 PCMU/8000\n"
				"a=sendrecv\n"
				"a=fingerprint:sha-256 18:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=mid:0\n";

			status = make_session_and_rtp_with_sdp_ex(&session, &rtp, &sdp_session, &parser,
				same_remote_sdp, "PCMU", SWITCH_FALSE, SWITCH_TRUE, NULL);
			fst_requires(status == SWITCH_STATUS_SUCCESS && session && rtp && sdp_session && parser);
			channel = switch_core_session_get_channel(session);
			fst_requires(channel != NULL);

			status = switch_rtp_pvt_get_ice_state(rtp, IPR_RTP,
				initial_rtp_user, sizeof(initial_rtp_user), initial_rtp_local_pwd, sizeof(initial_rtp_local_pwd),
				initial_rtp_remote_pwd, sizeof(initial_rtp_remote_pwd), &has_addr);
			fst_requires(status == SWITCH_STATUS_SUCCESS && has_addr == SWITCH_TRUE);
			initial_local_ufrag = strchr(initial_rtp_user, ':');
			fst_requires(initial_local_ufrag != NULL && initial_local_ufrag[1] != '\0');
			initial_local_ufrag++;
			has_addr = SWITCH_FALSE;
			status = switch_rtp_pvt_get_ice_state(rtp, IPR_RTCP,
				initial_rtcp_user, sizeof(initial_rtcp_user), initial_rtcp_local_pwd, sizeof(initial_rtcp_local_pwd),
				initial_rtcp_remote_pwd, sizeof(initial_rtcp_remote_pwd), &has_addr);
			fst_requires(status == SWITCH_STATUS_SUCCESS && has_addr == SWITCH_TRUE);

			switch_core_session_stop_media(session);
			switch_core_media_gen_local_sdp(session, SDP_OFFER, NULL, 0, NULL, 0);
			local_sdp = switch_channel_get_variable(channel, "rtp_local_sdp_str");
			fst_requires(copy_sdp_attribute(local_sdp, "ice-ufrag", offered_local_ufrag,
				sizeof(offered_local_ufrag)) == SWITCH_STATUS_SUCCESS);
			fst_requires(copy_sdp_attribute(local_sdp, "ice-pwd", offered_local_pwd,
				sizeof(offered_local_pwd)) == SWITCH_STATUS_SUCCESS);
			fst_check(strcmp(offered_local_ufrag, initial_local_ufrag) != 0);
			fst_check(strcmp(offered_local_pwd, initial_rtp_local_pwd) != 0);

			switch_core_media_clear_ice(session);
			switch_channel_set_flag(channel, CF_REINVITE);
			switch_channel_set_flag(channel, CF_RECOVERING);
			match = switch_core_media_negotiate_sdp(session, recovery_remote_sdp, &proceed, SDP_ANSWER);
			fst_requires(match != 0);
			status = switch_core_media_activate_rtp(session);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			has_addr = SWITCH_FALSE;
			status = switch_rtp_pvt_get_ice_state(rtp, IPR_RTP,
				restarted_rtp_user, sizeof(restarted_rtp_user), restarted_rtp_local_pwd, sizeof(restarted_rtp_local_pwd),
				restarted_rtp_remote_pwd, sizeof(restarted_rtp_remote_pwd), &has_addr);
			fst_requires(status == SWITCH_STATUS_SUCCESS && has_addr == SWITCH_TRUE);
			restarted_local_ufrag = strchr(restarted_rtp_user, ':');
			fst_requires(restarted_local_ufrag != NULL && restarted_local_ufrag[1] != '\0');
			restarted_local_ufrag++;
			fst_check(!strncmp(restarted_rtp_user, "stableRemoteUfrag:", strlen("stableRemoteUfrag:")));
			fst_check_string_equals(restarted_rtp_remote_pwd, "stableRemotePassword123456");
			fst_check_string_equals(restarted_local_ufrag, offered_local_ufrag);
			fst_check_string_equals(restarted_rtp_local_pwd, offered_local_pwd);

			has_addr = SWITCH_FALSE;
			status = switch_rtp_pvt_get_ice_state(rtp, IPR_RTCP,
				restarted_rtcp_user, sizeof(restarted_rtcp_user), restarted_rtcp_local_pwd, sizeof(restarted_rtcp_local_pwd),
				restarted_rtcp_remote_pwd, sizeof(restarted_rtcp_remote_pwd), &has_addr);
			fst_requires(status == SWITCH_STATUS_SUCCESS && has_addr == SWITCH_TRUE);
			fst_check_string_equals(restarted_rtcp_user, restarted_rtp_user);
			fst_check_string_equals(restarted_rtcp_local_pwd, offered_local_pwd);
			fst_check_string_equals(restarted_rtcp_remote_pwd, "stableRemotePassword123456");
			fst_check(strcmp(restarted_rtcp_user, initial_rtcp_user) != 0);
			fst_check(strcmp(restarted_rtcp_local_pwd, initial_rtcp_local_pwd) != 0);
			fst_check_string_equals(initial_rtp_remote_pwd, initial_rtcp_remote_pwd);

			cleanup_session_media_and_sdp(session, sdp_session, parser);
		}
		FCT_TEST_END();

		FCT_TEST_BGN(recovery_with_provisional_candidates_rearms_prflx_bootstrap)
		{
			switch_core_session_t *session = NULL;
			switch_channel_t *channel = NULL;
			switch_media_handle_t *smh = NULL;
			switch_rtp_t *rtp = NULL;
			switch_status_t status;
			void *sdp_session = NULL;
			sdp_parser_t *parser = NULL;
			char initial_ice_user[256] = "";
			char initial_local_pwd[256] = "";
			char initial_remote_pwd[256] = "";
			char restarted_ice_user[256] = "";
			char restarted_local_pwd[256] = "";
			char restarted_remote_pwd[256] = "";
			char restarted_sdp_local_ufrag[256] = "";
			char restarted_sdp_local_pwd[256] = "";
			const char *initial_local_ufrag;
			const char *restarted_local_ufrag;
			const char *local_sdp;
			switch_bool_t has_addr = SWITCH_FALSE;
			uint8_t match;
			uint8_t proceed = 0;
			const char *restart_sdp =
				"v=0\n"
				"o=- 1683118194 1683118197 IN IP4 0.0.0.0\n"
				"s=-\n"
				"t=0 0\n"
				"a=group:BUNDLE 0\n"
				"m=audio 18215 UDP/TLS/RTP/SAVPF 0\n"
				"c=IN IP4 50.114.144.39\n"
				"a=ice-ufrag:restartRemoteUfrag\n"
				"a=ice-pwd:restartRemotePassword123456\n"
				"a=ice-options:trickle\n"
				"a=rtcp-mux\n"
				"a=setup:active\n"
				"a=rtpmap:0 PCMU/8000\n"
				"a=sendrecv\n"
				"a=fingerprint:sha-256 18:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n"
				"a=mid:0\n";

			status = make_session_and_rtp_with_sdp(&session, &rtp, &sdp_session, &parser);
			fst_requires(status == SWITCH_STATUS_SUCCESS && session && rtp && sdp_session && parser);
			channel = switch_core_session_get_channel(session);
			smh = switch_core_session_get_media_handle(session);
			fst_requires(channel != NULL && smh != NULL);

			status = switch_core_media_trickle_remote_candidate_and_recheck(
				session, smh, sdp_session, SDP_TYPE_REQUEST, "0", 0,
				"candidate:265031753 1 udp 1685921533 50.114.144.39 18215 typ srflx raddr 100.69.211.204 rport 54081", 0);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			status = switch_rtp_pvt_get_ice_state(rtp, IPR_RTP,
				initial_ice_user, sizeof(initial_ice_user),
				initial_local_pwd, sizeof(initial_local_pwd),
				initial_remote_pwd, sizeof(initial_remote_pwd), &has_addr);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			initial_local_ufrag = strchr(initial_ice_user, ':');
			fst_requires(initial_local_ufrag != NULL && initial_local_ufrag[1] != '\0');
			initial_local_ufrag++;
			fst_check(!strncmp(initial_ice_user, "aZJpsl00bYnjrOZtkCFMtKhFC/CHAfcv:",
				strlen("aZJpsl00bYnjrOZtkCFMtKhFC/CHAfcv:")));
			fst_check_string_equals(initial_remote_pwd, "aNniSnLLp43SSsJrz6TNPty1zPrxZNzh");
			fst_check(initial_local_pwd[0] != '\0');
			fst_check(has_addr == SWITCH_TRUE);

			switch_core_media_clear_ice(session);
			switch_channel_set_flag(channel, CF_REINVITE);
			switch_channel_set_flag(channel, CF_RECOVERING);
			switch_channel_set_variable(channel, "rtp_ice_prflx_bootstrap", "true");
			match = switch_core_media_negotiate_sdp(session, restart_sdp, &proceed, SDP_OFFER);
			fst_requires(match != 0);
			switch_core_media_gen_local_sdp(session, SDP_ANSWER, NULL, 0, NULL, 0);
			local_sdp = switch_channel_get_variable(channel, "rtp_local_sdp_str");
			fst_requires(copy_sdp_attribute(local_sdp, "ice-ufrag", restarted_sdp_local_ufrag,
				sizeof(restarted_sdp_local_ufrag)) == SWITCH_STATUS_SUCCESS);
			fst_requires(copy_sdp_attribute(local_sdp, "ice-pwd", restarted_sdp_local_pwd,
				sizeof(restarted_sdp_local_pwd)) == SWITCH_STATUS_SUCCESS);
			status = switch_core_media_activate_rtp(session);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			has_addr = SWITCH_TRUE;
			status = switch_rtp_pvt_get_ice_state(rtp, IPR_RTP,
				restarted_ice_user, sizeof(restarted_ice_user),
				restarted_local_pwd, sizeof(restarted_local_pwd),
				restarted_remote_pwd, sizeof(restarted_remote_pwd), &has_addr);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			restarted_local_ufrag = strchr(restarted_ice_user, ':');
			fst_requires(restarted_local_ufrag != NULL && restarted_local_ufrag[1] != '\0');
			restarted_local_ufrag++;
			fst_check(!strncmp(restarted_ice_user, "restartRemoteUfrag:", strlen("restartRemoteUfrag:")));
			fst_check_string_equals(restarted_remote_pwd, "restartRemotePassword123456");
			fst_check(strcmp(restarted_local_ufrag, initial_local_ufrag) != 0);
			fst_check(strcmp(restarted_local_pwd, initial_local_pwd) != 0);
			fst_check_string_equals(restarted_sdp_local_ufrag, restarted_local_ufrag);
			fst_check_string_equals(restarted_sdp_local_pwd, restarted_local_pwd);
			fst_check(has_addr == SWITCH_FALSE);

			cleanup_session_media_and_sdp(session, sdp_session, parser);
		}
		FCT_TEST_END();

		FCT_TEST_BGN(unbundled_video_ready_recovery_avoids_duplicate_ice_activation)
		{
			switch_core_session_t *session = NULL;
			switch_channel_t *channel = NULL;
			switch_rtp_t *audio_rtp = NULL;
			switch_rtp_t *video_rtp = NULL;
			void *sdp_session = NULL;
			sdp_parser_t *parser = NULL;
			switch_status_t status;
			uint8_t match;
			uint8_t proceed = 0;
			char initial_video_user[256] = "";
			char initial_video_pwd[256] = "";
			char initial_video_remote_pwd[256] = "";
			char offered_video_ufrag[256] = "";
			char offered_video_pwd[256] = "";
			char restarted_video_user[256] = "";
			char restarted_video_pwd[256] = "";
			char restarted_video_remote_pwd[256] = "";
			char restarted_video_rtcp_user[256] = "";
			char restarted_video_rtcp_pwd[256] = "";
			char restarted_video_rtcp_remote_pwd[256] = "";
			const char *initial_video_local_ufrag;
			const char *restarted_video_local_ufrag;
			const char *local_sdp;
			switch_bool_t has_addr = SWITCH_FALSE;
			const char *audio_video_sdp =
				"v=0\n"
				"o=- 1683118194 1683118195 IN IP4 0.0.0.0\n"
				"s=-\n"
				"t=0 0\n"
				"m=audio 18215 UDP/TLS/RTP/SAVPF 0\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:audioStableUfrag\n"
				"a=ice-pwd:audioStablePassword123456\n"
				"a=candidate:1 1 udp 2130706431 127.0.0.1 18215 typ host\n"
				"a=candidate:2 2 udp 2130706430 127.0.0.1 18216 typ host\n"
				"a=rtcp:18216 IN IP4 127.0.0.1\n"
				"a=rtpmap:0 PCMU/8000\n"
				"a=sendrecv\n"
				"a=mid:0\n"
				"m=video 18217 UDP/TLS/RTP/SAVPF 96\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:videoStableUfrag\n"
				"a=ice-pwd:videoStablePassword123456\n"
				"a=candidate:3 1 udp 2130706431 127.0.0.1 18217 typ host\n"
				"a=candidate:4 2 udp 2130706430 127.0.0.1 18218 typ host\n"
				"a=rtcp:18218 IN IP4 127.0.0.1\n"
				"a=rtpmap:96 VP8/90000\n"
				"a=sendrecv\n"
				"a=mid:1\n";
			const char *restart_sdp =
				"v=0\n"
				"o=- 1683118194 1683118196 IN IP4 0.0.0.0\n"
				"s=-\n"
				"t=0 0\n"
				"m=audio 18215 UDP/TLS/RTP/SAVPF 0\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:audioStableUfrag\n"
				"a=ice-pwd:audioStablePassword123456\n"
				"a=candidate:1 1 udp 2130706431 127.0.0.1 18215 typ host\n"
				"a=candidate:2 2 udp 2130706430 127.0.0.1 18216 typ host\n"
				"a=rtcp:18216 IN IP4 127.0.0.1\n"
				"a=rtpmap:0 PCMU/8000\n"
				"a=sendrecv\n"
				"a=mid:0\n"
				"m=video 18217 UDP/TLS/RTP/SAVPF 96\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:videoRestartUfrag\n"
				"a=ice-pwd:videoRestartPassword123456\n"
				"a=candidate:3 1 udp 2130706431 127.0.0.1 18217 typ host\n"
				"a=candidate:4 2 udp 2130706430 127.0.0.1 18218 typ host\n"
				"a=rtcp:18218 IN IP4 127.0.0.1\n"
				"a=rtpmap:96 VP8/90000\n"
				"a=sendrecv\n"
				"a=mid:1\n";

			status = make_session_and_rtp_with_sdp_ex(&session, &audio_rtp, &sdp_session, &parser,
				audio_video_sdp, "PCMU,VP8", SWITCH_FALSE, SWITCH_TRUE, NULL);
			fst_requires(status == SWITCH_STATUS_SUCCESS && session && audio_rtp && sdp_session && parser);
			channel = switch_core_session_get_channel(session);
			video_rtp = switch_core_media_get_rtp_session(session, SWITCH_MEDIA_TYPE_VIDEO);
			fst_requires(channel != NULL && video_rtp != NULL);

			status = switch_rtp_pvt_get_ice_state(video_rtp, IPR_RTP,
				initial_video_user, sizeof(initial_video_user), initial_video_pwd, sizeof(initial_video_pwd),
				initial_video_remote_pwd, sizeof(initial_video_remote_pwd), &has_addr);
			fst_requires(status == SWITCH_STATUS_SUCCESS && has_addr == SWITCH_TRUE);
			initial_video_local_ufrag = strchr(initial_video_user, ':');
			fst_requires(initial_video_local_ufrag != NULL && initial_video_local_ufrag[1] != '\0');
			initial_video_local_ufrag++;

			switch_channel_clear_flag(channel, CF_VIDEO);
			switch_core_media_clear_ice(session);
			switch_channel_set_flag(channel, CF_REINVITE);
			switch_channel_set_flag(channel, CF_RECOVERING);
			match = switch_core_media_negotiate_sdp(session, restart_sdp, &proceed, SDP_OFFER);
			fst_requires(match != 0);
			switch_core_media_gen_local_sdp(session, SDP_ANSWER, NULL, 0, NULL, 0);
			local_sdp = switch_channel_get_variable(channel, "rtp_local_sdp_str");
			fst_requires(copy_sdp_media_attribute(local_sdp, "video", "ice-ufrag", offered_video_ufrag,
				sizeof(offered_video_ufrag)) == SWITCH_STATUS_SUCCESS);
			fst_requires(copy_sdp_media_attribute(local_sdp, "video", "ice-pwd", offered_video_pwd,
				sizeof(offered_video_pwd)) == SWITCH_STATUS_SUCCESS);
			fst_check(strcmp(offered_video_ufrag, initial_video_local_ufrag) != 0);
			fst_check(strcmp(offered_video_pwd, initial_video_pwd) != 0);
			status = switch_core_media_activate_rtp(session);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			has_addr = SWITCH_FALSE;
			status = switch_rtp_pvt_get_ice_state(video_rtp, IPR_RTP,
				restarted_video_user, sizeof(restarted_video_user), restarted_video_pwd, sizeof(restarted_video_pwd),
				restarted_video_remote_pwd, sizeof(restarted_video_remote_pwd), &has_addr);
			fst_requires(status == SWITCH_STATUS_SUCCESS && has_addr == SWITCH_TRUE);
			restarted_video_local_ufrag = strchr(restarted_video_user, ':');
			fst_requires(restarted_video_local_ufrag != NULL && restarted_video_local_ufrag[1] != '\0');
			restarted_video_local_ufrag++;
			fst_check(!strncmp(restarted_video_user, "videoRestartUfrag:", strlen("videoRestartUfrag:")));
			fst_check_string_equals(restarted_video_local_ufrag, offered_video_ufrag);
			fst_check_string_equals(restarted_video_pwd, offered_video_pwd);
			fst_check_string_equals(restarted_video_remote_pwd, "videoRestartPassword123456");

			has_addr = SWITCH_FALSE;
			status = switch_rtp_pvt_get_ice_state(video_rtp, IPR_RTCP,
				restarted_video_rtcp_user, sizeof(restarted_video_rtcp_user), restarted_video_rtcp_pwd,
				sizeof(restarted_video_rtcp_pwd), restarted_video_rtcp_remote_pwd,
				sizeof(restarted_video_rtcp_remote_pwd), &has_addr);
			fst_requires(status == SWITCH_STATUS_SUCCESS && has_addr == SWITCH_TRUE);
			fst_check_string_equals(restarted_video_rtcp_user, restarted_video_user);
			fst_check_string_equals(restarted_video_rtcp_pwd, offered_video_pwd);
			fst_check_string_equals(restarted_video_rtcp_remote_pwd, "videoRestartPassword123456");

			cleanup_session_media_and_sdp(session, sdp_session, parser);
		}
		FCT_TEST_END();

		FCT_TEST_BGN(unbundled_video_provisional_recovery_rearms_prflx_bootstrap)
		{
			switch_core_session_t *session = NULL;
			switch_channel_t *channel = NULL;
			switch_rtp_t *audio_rtp = NULL;
			switch_rtp_t *video_rtp = NULL;
			void *sdp_session = NULL;
			sdp_parser_t *parser = NULL;
			switch_status_t status;
			uint8_t match;
			uint8_t proceed = 0;
			char initial_video_user[256] = "";
			char initial_video_pwd[256] = "";
			char initial_video_remote_pwd[256] = "";
			char offered_video_ufrag[256] = "";
			char offered_video_pwd[256] = "";
			char restarted_video_user[256] = "";
			char restarted_video_pwd[256] = "";
			char restarted_video_remote_pwd[256] = "";
			char restarted_video_rtcp_user[256] = "";
			char restarted_video_rtcp_pwd[256] = "";
			char restarted_video_rtcp_remote_pwd[256] = "";
			const char *restarted_video_local_ufrag;
			const char *local_sdp;
			switch_bool_t has_addr = SWITCH_FALSE;
			const char *initial_sdp =
				"v=0\n"
				"o=- 1683118194 1683118195 IN IP4 0.0.0.0\n"
				"s=-\n"
				"t=0 0\n"
				"m=audio 18215 UDP/TLS/RTP/SAVPF 0\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:audioInitialUfrag\n"
				"a=ice-pwd:audioInitialPassword123456\n"
				"a=candidate:1 1 udp 2130706431 127.0.0.1 18215 typ host\n"
				"a=candidate:2 2 udp 2130706430 127.0.0.1 18216 typ host\n"
				"a=rtcp:18216 IN IP4 127.0.0.1\n"
				"a=rtpmap:0 PCMU/8000\n"
				"a=sendrecv\n"
				"a=mid:0\n"
				"m=video 18217 UDP/TLS/RTP/SAVPF 96\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:videoInitialUfrag\n"
				"a=ice-pwd:videoInitialPassword123456\n"
				"a=candidate:3 1 udp 2130706431 127.0.0.1 18217 typ host\n"
				"a=candidate:4 2 udp 2130706430 127.0.0.1 18218 typ host\n"
				"a=rtcp:18218 IN IP4 127.0.0.1\n"
				"a=rtpmap:96 VP8/90000\n"
				"a=sendrecv\n"
				"a=mid:1\n";
			const char *restart_sdp =
				"v=0\n"
				"o=- 1683118194 1683118196 IN IP4 0.0.0.0\n"
				"s=-\n"
				"t=0 0\n"
				"m=audio 18215 UDP/TLS/RTP/SAVPF 0\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:audioInitialUfrag\n"
				"a=ice-pwd:audioInitialPassword123456\n"
				"a=candidate:1 1 udp 2130706431 127.0.0.1 18215 typ host\n"
				"a=candidate:2 2 udp 2130706430 127.0.0.1 18216 typ host\n"
				"a=rtcp:18216 IN IP4 127.0.0.1\n"
				"a=rtpmap:0 PCMU/8000\n"
				"a=sendrecv\n"
				"a=mid:0\n"
				"m=video 18217 UDP/TLS/RTP/SAVPF 96\n"
				"c=IN IP4 127.0.0.1\n"
				"a=ice-ufrag:videoRestartUfrag\n"
				"a=ice-pwd:videoRestartPassword123456\n"
				"a=rtcp:18218 IN IP4 127.0.0.1\n"
				"a=rtpmap:96 VP8/90000\n"
				"a=sendrecv\n"
				"a=mid:1\n";

			status = make_session_and_rtp_with_sdp_ex(&session, &audio_rtp, &sdp_session, &parser,
				initial_sdp, "PCMU,VP8", SWITCH_FALSE, SWITCH_TRUE, NULL);
			fst_requires(status == SWITCH_STATUS_SUCCESS && session && audio_rtp && sdp_session && parser);
			channel = switch_core_session_get_channel(session);
			video_rtp = switch_core_media_get_rtp_session(session, SWITCH_MEDIA_TYPE_VIDEO);
			fst_requires(channel != NULL && video_rtp != NULL);
			status = switch_rtp_pvt_get_ice_state(video_rtp, IPR_RTP,
				initial_video_user, sizeof(initial_video_user), initial_video_pwd, sizeof(initial_video_pwd),
				initial_video_remote_pwd, sizeof(initial_video_remote_pwd), &has_addr);
			fst_requires(status == SWITCH_STATUS_SUCCESS && has_addr == SWITCH_TRUE);

			switch_channel_clear_flag(channel, CF_VIDEO);
			switch_core_media_clear_ice(session);
			switch_channel_set_flag(channel, CF_REINVITE);
			switch_channel_set_flag(channel, CF_RECOVERING);
			switch_channel_set_variable(channel, "rtp_ice_prflx_bootstrap", "true");
			match = switch_core_media_negotiate_sdp(session, restart_sdp, &proceed, SDP_OFFER);
			fst_requires(match != 0);
			switch_core_media_gen_local_sdp(session, SDP_ANSWER, NULL, 0, NULL, 0);
			local_sdp = switch_channel_get_variable(channel, "rtp_local_sdp_str");
			fst_requires(copy_sdp_media_attribute(local_sdp, "video", "ice-ufrag", offered_video_ufrag,
				sizeof(offered_video_ufrag)) == SWITCH_STATUS_SUCCESS);
			fst_requires(copy_sdp_media_attribute(local_sdp, "video", "ice-pwd", offered_video_pwd,
				sizeof(offered_video_pwd)) == SWITCH_STATUS_SUCCESS);
			status = switch_core_media_activate_rtp(session);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			has_addr = SWITCH_TRUE;
			status = switch_rtp_pvt_get_ice_state(video_rtp, IPR_RTP,
				restarted_video_user, sizeof(restarted_video_user), restarted_video_pwd, sizeof(restarted_video_pwd),
				restarted_video_remote_pwd, sizeof(restarted_video_remote_pwd), &has_addr);
			fst_requires(status == SWITCH_STATUS_SUCCESS && has_addr == SWITCH_FALSE);
			restarted_video_local_ufrag = strchr(restarted_video_user, ':');
			fst_requires(restarted_video_local_ufrag != NULL && restarted_video_local_ufrag[1] != '\0');
			restarted_video_local_ufrag++;
			fst_check(!strncmp(restarted_video_user, "videoRestartUfrag:", strlen("videoRestartUfrag:")));
			fst_check_string_equals(restarted_video_local_ufrag, offered_video_ufrag);
			fst_check_string_equals(restarted_video_pwd, offered_video_pwd);
			fst_check_string_equals(restarted_video_remote_pwd, "videoRestartPassword123456");

			has_addr = SWITCH_TRUE;
			status = switch_rtp_pvt_get_ice_state(video_rtp, IPR_RTCP,
				restarted_video_rtcp_user, sizeof(restarted_video_rtcp_user), restarted_video_rtcp_pwd,
				sizeof(restarted_video_rtcp_pwd), restarted_video_rtcp_remote_pwd,
				sizeof(restarted_video_rtcp_remote_pwd), &has_addr);
			fst_requires(status == SWITCH_STATUS_SUCCESS && has_addr == SWITCH_FALSE);
			fst_check_string_equals(restarted_video_rtcp_user, restarted_video_user);
			fst_check_string_equals(restarted_video_rtcp_pwd, offered_video_pwd);
			fst_check_string_equals(restarted_video_rtcp_remote_pwd, "videoRestartPassword123456");

			cleanup_session_media_and_sdp(session, sdp_session, parser);
		}
		FCT_TEST_END();

		/* 1) Registration toggles + candidate emit */
		FCT_TEST_BGN(registration_and_emit_basic)
		{
			switch_core_session_t *session = NULL;
			switch_rtp_t *rtp = NULL; trickle_captured_t cap; switch_rtp_ice_cand_t c;
			void *sdp_session = NULL;
			sdp_parser_t *parser = NULL;

			memset(&cap, 0, sizeof(cap));
			fct_req(make_session_and_rtp_with_sdp(&session, &rtp, &sdp_session, &parser) == SWITCH_STATUS_SUCCESS && rtp != NULL);

			/* make_session_and_rtp_with_sdp() auto-registers a callback because rtp_trickle_ice=true */
			fct_chk(switch_rtp_trickle_is_registered(rtp) == SWITCH_TRUE);

			/* unregister the auto-registered callback */
			switch_rtp_set_ice_candidate_cb(rtp, NULL, NULL);
			fct_chk(switch_rtp_trickle_is_registered(rtp) == SWITCH_FALSE);

			/* Now register our test callback */

			switch_rtp_set_ice_candidate_cb(rtp, on_local_candidate_cb, &cap);
			fct_chk(switch_rtp_trickle_is_registered(rtp) == SWITCH_TRUE);

			memset(&c, 0, sizeof(c));
			c.component_id = 1; switch_snprintf(c.ip, sizeof(c.ip), "%s", "192.168.2.1");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp"); c.port = 40000; c.priority = 100u;
			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 0);
			fct_chk(cap.called == 1 && strcmp(cap.last_mid, "audio") == 0 && cap.last_mline == 0 && cap.last_eoc == 0);
			fct_chk(cap.last_cand.component_id == 1 && strcmp(cap.last_cand.ip, "192.168.2.1") == 0);
			fct_chk(cap.last_cand.port == 40000 && cap.last_cand.priority == 100u);

			/* unregister by setting cb = NULL */
			switch_rtp_set_ice_candidate_cb(rtp, NULL, NULL);
			fct_chk(switch_rtp_trickle_is_registered(rtp) == SWITCH_FALSE);
			cleanup_session_media_and_sdp(session, sdp_session, parser);
		}
		FCT_TEST_END();

		/* 2) Overwrite callback: last registration wins */
		FCT_TEST_BGN(overwrite_callback_last_wins)
		{
			switch_rtp_t *rtp = NULL; trickle_captured_t cap; switch_rtp_ice_cand_t c; const char *err = NULL;
			memset(&cap, 0, sizeof(cap));
			fct_req(make_real_rtp(pool, &rtp, &err) == SWITCH_STATUS_SUCCESS && rtp != NULL);

			switch_rtp_set_ice_candidate_cb(rtp, on_local_candidate_cb, &cap);
			switch_rtp_set_ice_candidate_cb(rtp, on_local_candidate_cb_2, &cap); /* overwrite */

			memset(&c, 0, sizeof(c));
			c.component_id = 1; switch_snprintf(c.ip, sizeof(c.ip), "%s", "192.168.100.5");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp"); c.port = 50000; c.priority = 200u;
			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 1, 0);

			/* cb2 transforms values in a distinctive way (see implementation) */
			fct_chk(cap.called == 10);                /* +10 from cb2 */
			fct_chk(strncmp(cap.last_mid, "cb2:", 4) == 0);
			fct_chk(cap.last_mline == 101);          /* mline+100 */
			fct_chk(cap.last_eoc == 98);             /* not EOC => 98 */
			fct_chk(cap.last_cand.component_id == 101);
			fct_chk(strcmp(cap.last_cand.ip, "cb2-192.168.100.5") == 0);
			fct_chk(cap.last_cand.port == 50100);
			fct_chk(cap.last_cand.priority == 300u);
			cleanup_rtp(&rtp);
		}
		FCT_TEST_END();

		/* 3) Component 2 (RTCP) candidate */
		FCT_TEST_BGN(component_2_rtcp_candidate)
		{
			switch_rtp_t *rtp = NULL; trickle_captured_t cap; switch_rtp_ice_cand_t c; const char *err = NULL;
			memset(&cap, 0, sizeof(cap));
			fct_req(make_real_rtp(pool, &rtp, &err) == SWITCH_STATUS_SUCCESS && rtp != NULL);
			switch_rtp_set_ice_candidate_cb(rtp, on_local_candidate_cb, &cap);

			memset(&c, 0, sizeof(c));
			c.component_id = 2; switch_snprintf(c.ip, sizeof(c.ip), "%s", "192.168.113.9");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp"); c.port = 40002; c.priority = 999u;
			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 0);

			fct_chk(cap.called == 1);
			fct_chk(cap.last_cand.component_id == 2);
			fct_chk(strcmp(cap.last_cand.ip, "192.168.113.9") == 0);
			cleanup_rtp(&rtp);
		}
		FCT_TEST_END();

		/* 4) End-of-candidates only (NULL cand pointer) */
		FCT_TEST_BGN(end_of_candidates_only)
		{
			switch_rtp_t *rtp = NULL; trickle_captured_t cap; const char *err = NULL;
			memset(&cap, 0, sizeof(cap));
			fct_req(make_real_rtp(pool, &rtp, &err) == SWITCH_STATUS_SUCCESS && rtp != NULL);
			switch_rtp_set_ice_candidate_cb(rtp, on_local_candidate_cb, &cap);

			switch_rtp_trickle_emit_local_candidate(rtp, NULL, "video", 1, 1);
			fct_chk(cap.called == 1);
			fct_chk(strcmp(cap.last_mid, "video") == 0);
			fct_chk(cap.last_mline == 1);
			fct_chk(cap.last_eoc == 1);
			/* last_cand is zeroed */
			fct_chk(cap.last_cand.component_id == 0 && cap.last_cand.port == 0);
			cleanup_rtp(&rtp);
		}
		FCT_TEST_END();

		/* 5) Mixed EOC and candidates */
		FCT_TEST_BGN(mixed_emit_and_eoc_sequence)
		{
			switch_rtp_t *rtp = NULL; trickle_captured_t cap; switch_rtp_ice_cand_t c; const char *err = NULL;
			memset(&cap, 0, sizeof(cap));
			fct_req(make_real_rtp(pool, &rtp, &err) == SWITCH_STATUS_SUCCESS && rtp != NULL);
			switch_rtp_set_ice_candidate_cb(rtp, on_local_candidate_cb, &cap);

			memset(&c, 0, sizeof(c));
			c.component_id = 1; switch_snprintf(c.ip, sizeof(c.ip), "%s", "10.0.0.10");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp"); c.port = 10000; c.priority = 1u;

			switch_rtp_trickle_emit_local_candidate(rtp, NULL, "audio", 0, 1);
			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 0);

			fst_check(cap.called == 1);
			fst_check(cap.last_eoc == 1);
			fst_check(cap.last_cand.component_id == 0 && cap.last_cand.port == 0);
			
			fst_check(cap.last_cand.ip[0] == '\0');
			cleanup_rtp(&rtp);
		}
		FCT_TEST_END();

		/* 6) Different mids / mline indexes in one session */
		FCT_TEST_BGN(mids_and_mlines_variations)
		{
			switch_rtp_t *rtp = NULL; trickle_captured_t cap; switch_rtp_ice_cand_t c; const char *err = NULL;
			memset(&cap, 0, sizeof(cap));
			fct_req(make_real_rtp(pool, &rtp, &err) == SWITCH_STATUS_SUCCESS && rtp != NULL);
			switch_rtp_set_ice_candidate_cb(rtp, on_local_candidate_cb, &cap);

			memset(&c, 0, sizeof(c));
			c.component_id = 1; switch_snprintf(c.ip, sizeof(c.ip), "%s", "10.0.0.11");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp"); c.port = 10001; c.priority = 2u;

			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 0);
			fct_chk(cap.called == 1 && strcmp(cap.last_mid, "audio") == 0 && cap.last_mline == 0);

			switch_rtp_trickle_emit_local_candidate(rtp, &c, "video", 1, 0);
			fct_chk(cap.called == 2 && strcmp(cap.last_mid, "video") == 0 && cap.last_mline == 1);
			cleanup_rtp(&rtp);
		}
		FCT_TEST_END();

		/* 7) NULL mid tolerated (becomes "(null)" in our handler) */
		FCT_TEST_BGN(null_mid_tolerated)
		{
			switch_rtp_t *rtp = NULL; trickle_captured_t cap; switch_rtp_ice_cand_t c; const char *err = NULL;
			memset(&cap, 0, sizeof(cap));
			fct_req(make_real_rtp(pool, &rtp, &err) == SWITCH_STATUS_SUCCESS && rtp != NULL);
			switch_rtp_set_ice_candidate_cb(rtp, on_local_candidate_cb, &cap);

			memset(&c, 0, sizeof(c));
			c.component_id = 1; switch_snprintf(c.ip, sizeof(c.ip), "%s", "10.0.0.12");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp"); c.port = 10002; c.priority = 3u;

			switch_rtp_trickle_emit_local_candidate(rtp, &c, NULL, 2, 0);
			fct_chk(cap.called == 1);
			fct_chk(strcmp(cap.last_mid, "(null)") == 0);
			fct_chk(cap.last_mline == 2);
			cleanup_rtp(&rtp);
		}
		FCT_TEST_END();

		/* 8) Burst emission ordering & final state */
		FCT_TEST_BGN(burst_emission_ordering)
		{
			switch_rtp_t *rtp = NULL; trickle_captured_t cap; switch_rtp_ice_cand_t c; const char *err = NULL;
			int i;
			memset(&cap, 0, sizeof(cap));
			fct_req(make_real_rtp(pool, &rtp, &err) == SWITCH_STATUS_SUCCESS && rtp != NULL);
			switch_rtp_set_ice_candidate_cb(rtp, on_local_candidate_cb, &cap);

			memset(&c, 0, sizeof(c));
			c.component_id = 1; switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp"); c.priority = 777u;

			for (i = 0; i < 50; ++i) {
				switch_snprintf(c.ip, sizeof(c.ip), "198.168.0.%d", (i % 250) + 1);
				c.port = 20000 + i;
				switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 0);
			}
			fct_chk(cap.called == 50);
			fct_chk(strcmp(cap.last_cand.transport, "udp") == 0);
			fct_chk(cap.last_cand.port == 20000 + 49);
			cleanup_rtp(&rtp);
		}
		FCT_TEST_END();
		FCT_TEST_BGN(trickle_emit_fires_custom_event)
		{
			switch_core_session_t *session = NULL;
			switch_status_t st;
			switch_rtp_t *rtp;
			switch_rtp_ice_cand_t c;
			void *sdp_session = NULL;
			sdp_parser_t *parser = NULL;

			/* bind listener */
			switch_event_bind("trickle-ut", SWITCH_EVENT_CUSTOM, "sofia::trickle-ice", trickle_event_handler, NULL);

			st = make_session_and_rtp_with_sdp(&session, &rtp, &sdp_session, &parser);
			fst_requires(st == SWITCH_STATUS_SUCCESS && session && rtp);

			/* register trickle on audio */
			switch_core_media_trickle_register(session, "audio", 0);

			/* get the audio RTP and emit a fake local candidate */
			rtp = switch_core_media_get_rtp_session(session, SWITCH_MEDIA_TYPE_AUDIO);
			fct_req(rtp != NULL);

			memset(&c, 0, sizeof(c));
			switch_snprintf(c.foundation, sizeof(c.foundation), "%s", "1");
			c.component_id = 1;
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp");
			c.priority = 2122260223u;
			switch_snprintf(c.ip, sizeof(c.ip), "%s", "203.0.113.7");
			c.port = 40002;
			switch_snprintf(c.cand_type, sizeof(c.cand_type), "%s", "host");

			trickle_ev_seen = 0;
			memset(last_cand_line, 0, sizeof(last_cand_line));

			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 0);

			/* give the event loop a tick */
			switch_yield(100000);

			fct_chk(trickle_ev_seen == 1);
			fct_chk(strstr(last_cand_line, "a=candidate:1 1 udp") != NULL);

			switch_event_unbind_callback(trickle_event_handler);
			cleanup_session_media_and_sdp(session, sdp_session, parser);
		}
		FCT_TEST_END();
		FCT_TEST_BGN(unregister_stops_emission)
		{
			switch_rtp_t *rtp = NULL; trickle_captured_t cap; switch_rtp_ice_cand_t c; const char *err = NULL;
			memset(&cap, 0, sizeof(cap));
			fct_req(make_real_rtp(pool, &rtp, &err) == SWITCH_STATUS_SUCCESS && rtp != NULL);

			switch_rtp_set_ice_candidate_cb(rtp, on_local_candidate_cb, &cap);
			memset(&c, 0, sizeof(c));
			c.component_id = 1;
			switch_snprintf(c.ip, sizeof(c.ip), "%s", "10.10.10.10");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp");
			c.port = 30000; c.priority = 1234u;
			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 0);
			fct_chk(cap.called == 1);

			switch_rtp_set_ice_candidate_cb(rtp, NULL, NULL);
			fct_chk(switch_rtp_trickle_is_registered(rtp) == SWITCH_FALSE);

			memset(&c, 0, sizeof(c));
			c.component_id = 2;
			switch_snprintf(c.ip, sizeof(c.ip), "%s", "10.10.10.11");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp");
			c.port = 30002; c.priority = 5678u;
			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 0);

			fct_chk(cap.called == 1);

			cleanup_rtp(&rtp);
		}
		FCT_TEST_END();
		FCT_TEST_BGN(emit_local_candidates_EOC)
		{
			switch_core_session_t *session = NULL;
			switch_rtp_t *rtp = NULL;
			trickle_captured_t cap;
			switch_status_t st;
			switch_rtp_ice_cand_t c;
			void *sdp_session = NULL;
			sdp_parser_t *parser = NULL;

			memset(&cap, 0, sizeof(cap));
			memset(&c, 0, sizeof(c));

			st = make_session_and_rtp_with_sdp(&session, &rtp, &sdp_session, &parser);
			fst_requires(st == SWITCH_STATUS_SUCCESS && session && rtp);

			switch_rtp_set_ice_candidate_cb(rtp, on_local_candidate_cb, &cap);
			fst_check(switch_rtp_trickle_is_registered(rtp) == SWITCH_TRUE);

			c.component_id = 1;
			switch_snprintf(c.ip, sizeof(c.ip), "%s", "10.10.10.10");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp");
			c.port = 40000;
			c.priority = 100u;
			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 0);
			fst_check(cap.called == 1);

			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 1);
			fst_check(cap.called == 2);

			switch_rtp_trickle_emit_local_candidate(rtp, &c, "audio", 0, 0);
			fst_xcheck(cap.called == 2, "EOC seal should prevent further emissions");

			switch_rtp_trickle_emit_local_candidate(rtp, NULL, "audio", 0, 1);
			fst_xcheck(cap.called == 2, "NULL-candidate EOC should be ignored after seal");

			switch_rtp_set_ice_candidate_cb(rtp, NULL, NULL);
			cleanup_session_media_and_sdp(session, sdp_session, parser);
		}
		FCT_TEST_END();
		FCT_TEST_BGN(emit_local_candidate_after_destroy)
		{
			switch_rtp_t *rtp1 = NULL, *rtp2 = NULL;
			trickle_captured_t cap1, cap2;
			switch_status_t st;
			switch_rtp_ice_cand_t c;

			memset(&cap1, 0, sizeof(cap1));
			memset(&cap2, 0, sizeof(cap2));
			memset(&c, 0, sizeof(c));

			st = make_real_rtp(pool, &rtp1, NULL);
			fst_requires(st == SWITCH_STATUS_SUCCESS && rtp1);

			switch_rtp_set_ice_candidate_cb(rtp1, on_local_candidate_cb, &cap1);
			fst_check(switch_rtp_trickle_is_registered(rtp1) == SWITCH_TRUE);

			c.component_id = 1;
			switch_snprintf(c.ip, sizeof(c.ip), "%s", "192.168.2.1");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp");
			c.port = 45000;
			c.priority = 200u;
			switch_rtp_trickle_emit_local_candidate(rtp1, &c, "audio", 0, 1);
			fst_check(cap1.called == 1);

			switch_rtp_set_ice_candidate_cb(rtp1, NULL, NULL);
			switch_rtp_destroy(&rtp1);
			rtp1 = NULL;

			st = make_real_rtp(pool, &rtp2, NULL);
			fst_requires(st == SWITCH_STATUS_SUCCESS && rtp2);

			switch_rtp_set_ice_candidate_cb(rtp2, on_local_candidate_cb, &cap2);
			fst_check(switch_rtp_trickle_is_registered(rtp2) == SWITCH_TRUE);

			memset(&c, 0, sizeof(c));
			c.component_id = 1;
			switch_snprintf(c.ip, sizeof(c.ip), "%s", "172.16.0.10");
			switch_snprintf(c.transport, sizeof(c.transport), "%s", "udp");
			c.port = 46000;
			c.priority = 300u;
			switch_rtp_trickle_emit_local_candidate(rtp2, &c, "audio", 0, 0);
			fst_xcheck(cap2.called == 1, "Fresh session must be able to emit new candidates");

			switch_rtp_set_ice_candidate_cb(rtp2, NULL, NULL);
			switch_rtp_destroy(&rtp2);
		}
		FCT_TEST_END();

	/* Test ACL filtering: private IP rejected, public IP chosen */
	FCT_TEST_BGN(test_acl_filtering_chooses_public_ip)
	{
		switch_core_session_t *session = NULL;
		switch_rtp_t *rtp = NULL;
		switch_status_t st;
		switch_media_handle_t *smh = NULL;
		void *sdp_session = NULL;
		sdp_parser_t *parser = NULL;
		/* Create a real session to simulate production environment */
		st = make_session_and_rtp_with_sdp(&session, &rtp, &sdp_session, &parser);
		fst_requires(st == SWITCH_STATUS_SUCCESS);
		fst_requires(session != NULL);
		fst_requires(rtp != NULL);

		/* Get the media handle from the session */
		smh = switch_core_session_get_media_handle(session);
		fst_requires(smh != NULL);

		/* Test the full production trickle ICE workflow */
		/* This should replicate the exact sequence from the production logs */

		/* First candidate: host candidate (private IP - should be rejected by ACL) */
		st = switch_core_media_trickle_remote_candidate_and_recheck(
			session, smh, sdp_session, SDP_TYPE_REQUEST,
			"0", 0,
			"candidate:2913552865 1 udp 24977407 192.168.0.1 49412 typ host raddr 162.120.214.184 rport 55197",
			0
		);
		fst_check(st == SWITCH_STATUS_SUCCESS);

		/* Second candidate: IPv6 srflx candidate (should be dropped - no network path) */
		st = switch_core_media_trickle_remote_candidate_and_recheck(
			session, smh, sdp_session, SDP_TYPE_REQUEST,
			"0", 0,
			"candidate:265031753 1 udp 1685921537 fd7a:115c:a1e0:ab12:4843:cd96:625d:273b 18215 typ srflx raddr 100.69.211.204 rport 54081",
			0
		);
		fst_check(st == SWITCH_STATUS_SUCCESS);

		/* Third candidate: .local hostname candidate (should be dropped - not an IP) */
		st = switch_core_media_trickle_remote_candidate_and_recheck(
			session, smh, sdp_session, SDP_TYPE_REQUEST,
			"0", 0,
			"candidate:265031753 1 udp 1685921535 490f301c-75b1-45ea-b4ef-259ba8aade9b.local 18215 typ host raddr 100.69.211.204 rport 54081",
			0
		);
		fst_check(st == SWITCH_STATUS_SUCCESS);

		/* Fourth candidate: public IP srflx candidate (THIS SHOULD BE CHOSEN!) */
		st = switch_core_media_trickle_remote_candidate_and_recheck(
			session, smh, sdp_session, SDP_TYPE_REQUEST,
			"0", 0,
			"candidate:265031753 1 udp 1685921533 50.114.144.39 18215 typ srflx raddr 100.69.211.204 rport 54081",
			0
		);
		fst_check(st == SWITCH_STATUS_SUCCESS);

		/* Test end-of-candidates marker */
		st = switch_core_media_trickle_remote_candidate_and_recheck(
			session, smh, sdp_session, SDP_TYPE_REQUEST,
			"0", 0,
			NULL,
			1  /* end_of_candidates */
		);
		fst_check(st == SWITCH_STATUS_SUCCESS);

		/* Verify the correct candidate was chosen using the new helper function */
		{
			char *chosen_addr = NULL;
			switch_port_t chosen_port = 0;

			st = switch_core_media_get_chosen_ice_candidate(session, SWITCH_MEDIA_TYPE_AUDIO, &chosen_addr, &chosen_port);
			fst_check(st == SWITCH_STATUS_SUCCESS);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
				"Chosen ICE candidate: %s:%d\n",
				chosen_addr ? chosen_addr : "(null)", chosen_port);

			/* Verify the correct candidate was chosen:
			 * - NOT the private IP 192.168.0.1:49412
			 * - SHOULD BE the public IP 50.114.144.39:18215 */
			fst_check_string_equals(chosen_addr, "50.114.144.39");
			fst_check(chosen_port == 18215);
		}

		switch_sleep(1000 * 1000);
		cleanup_session_media_and_sdp(session, sdp_session, parser);
	}
	FCT_TEST_END();
	
	}

	FCT_FIXTURE_SUITE_END();
}
FCT_END()
