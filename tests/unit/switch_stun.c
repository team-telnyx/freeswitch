#include <switch.h>
#include <switch_stun.h>
#include <test/switch_test.h>
#include <private/switch_rtp_pvt.h>
#ifdef SWITCH_STUN_HAVE_SOFIA
#include <sofia-sip/sdp.h>
extern switch_status_t switch_core_media_trickle_remote_candidate_and_recheck(switch_core_session_t *,
	switch_media_handle_t *, void *, switch_sdp_type_t, const char *, int, const char *, int);
#endif

static const switch_payload_t TEST_PT = 8;
static const char *rx_host = "127.0.0.1";
static switch_port_t rx_port = 1234;
static const char *tx_host = "127.0.0.1";
static switch_port_t tx_port = 54320;

typedef struct {
	switch_rtp_t *rtp;
	switch_socket_t *sender;
	switch_sockaddr_t *target;
	switch_atomic_t running;
	switch_atomic_t reader_done;
	switch_atomic_t reset_done;
} reset_reader_test_t;

static void *SWITCH_THREAD_FUNC send_control_only(switch_thread_t *thread, void *data)
{
	reset_reader_test_t *ctx = data;
	uint8_t rr[32] = { 0x81, 201, 0, 7, 0x12, 0x34, 0x56, 0x78 };
	uint32_t report_ssrc = htonl(switch_rtp_get_ssrc(ctx->rtp));
	switch_size_t len;
	int i;
	(void)thread;
	memcpy(rr + 8, &report_ssrc, sizeof(report_ssrc));
	while (switch_atomic_read(&ctx->running)) {
		for (i = 0; i < 64; i++) {
			len = sizeof(rr);
			switch_socket_sendto(ctx->sender, ctx->target, 0, (char *)rr, &len);
		}
		switch_sleep(100);
	}
	return NULL;
}

static void *SWITCH_THREAD_FUNC read_control_only(switch_thread_t *thread, void *data)
{
	reset_reader_test_t *ctx = data;
	switch_frame_t frame = { 0 };
	(void)thread;
	switch_rtp_zerocopy_read_frame(ctx->rtp, &frame, 0);
	switch_atomic_set(&ctx->reader_done, 1);
	return NULL;
}

static void *SWITCH_THREAD_FUNC reset_during_control(switch_thread_t *thread, void *data)
{
	reset_reader_test_t *ctx = data;
	(void)thread;
	switch_rtp_reset(ctx->rtp);
	switch_atomic_set(&ctx->reset_done, 1);
	return NULL;
}

static switch_size_t build_authenticated_request_with_role(uint8_t *buf, switch_size_t buflen,
	const char *username, const char *password, switch_bool_t use_candidate,
	switch_bool_t peer_controlled, switch_bool_t peer_controlling)
{
	static const char tie_breaker[8] = { 0x01, 0x23, 0x45, 0x67, 0x11, 0x22, 0x33, 0x44 };
	switch_stun_packet_t *packet;
	switch_size_t bytes;

	switch_assert(buf);
	switch_assert(buflen >= 128);
	memset(buf, 0, buflen);
	packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_REQUEST, NULL, buf);
	switch_stun_packet_attribute_add_priority(packet, 0x6e0001ff);
	if (username) {
		switch_stun_packet_attribute_add_username(packet, (char *)username, (uint16_t)strlen(username));
	}
	if (use_candidate) {
		switch_stun_packet_attribute_add_use_candidate(packet);
	}
	if (peer_controlled) {
		switch_stun_packet_attribute_add_controlled_value(packet, tie_breaker);
	}
	if (peer_controlling) {
		switch_stun_packet_attribute_add_controlling_value(packet, tie_breaker);
	}
	switch_stun_packet_attribute_add_integrity(packet, password);
	switch_stun_packet_attribute_add_fingerprint(packet);
	bytes = switch_stun_packet_length(packet);
	switch_assert(bytes <= buflen);

	return bytes;
}

static switch_size_t build_authenticated_request(uint8_t *buf, switch_size_t buflen, const char *username,
	const char *password, switch_bool_t use_candidate)
{
	return build_authenticated_request_with_role(buf, buflen, username, password, use_candidate,
		SWITCH_FALSE, SWITCH_FALSE);
}

static switch_size_t build_authenticated_response(uint8_t *buf, switch_size_t buflen,
	char *transaction_id, const char *password)
{
	switch_stun_packet_t *packet;
	switch_size_t bytes;

	switch_assert(buf);
	switch_assert(buflen >= 128);
	switch_assert(transaction_id);
	memset(buf, 0, buflen);
	packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_RESPONSE, transaction_id, buf);
	switch_stun_packet_attribute_add_integrity(packet, password);
	switch_stun_packet_attribute_add_fingerprint(packet);
	bytes = switch_stun_packet_length(packet);
	switch_assert(bytes <= buflen);

	return bytes;
}

static switch_size_t build_authenticated_role_conflict(uint8_t *buf, char *transaction, const char *password)
{
	switch_stun_packet_t *packet;
	switch_stun_packet_attribute_t *attr;

	memset(buf, 0, 512);
	packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_ERROR_RESPONSE, transaction, buf);
	attr = (switch_stun_packet_attribute_t *)(buf + sizeof(packet->header));
	attr->type = htons(SWITCH_STUN_ATTR_ERROR_CODE);
	attr->length = htons(4);
	attr->value[2] = 4;
	attr->value[3] = 87;
	packet->header.length = htons(8);
	switch_stun_packet_attribute_add_integrity(packet, password);
	switch_stun_packet_attribute_add_fingerprint(packet);
	return switch_stun_packet_length(packet);
}

static void fsctl_debug(switch_core_session_t *session) 
{
		switch_stream_handle_t stream = { 0 };

		SWITCH_STANDARD_STREAM(stream);
		switch_api_execute("fsctl", "debug_level 9", session, &stream);
		switch_safe_free(stream.data);
}


FST_CORE_BEGIN("./conf_stun")
{
FST_SUITE_BEGIN(switch_stun)
{
FST_SETUP_BEGIN()
{
}
FST_SETUP_END()

FST_TEARDOWN_BEGIN()
{
}
FST_TEARDOWN_END()
	FST_TEST_BEGIN(test_binding_indication)
	{
		switch_stun_packet_t *packet;
		/*   STUN Indication:  A STUN message that does not receive a response. */
		static uint8_t incoming[] =
			"\x00\x11\x00\x08\x21\x12\xa4\x42\x36\xde\x03\x48\x25\xa7\x69\x43\x31\x18\x96\x90\x80\x28\x00\x04\x07\x2c\xa6\xb4";

		packet = switch_stun_packet_parse(incoming, sizeof(incoming) - 1);

		switch_assert(packet);
	}
	FST_TEST_END()
	FST_TEST_BEGIN(test_stun_auth_validation)
	{
		static const uint8_t authenticated_request[] = {
			0x00, 0x01, 0x00, 0x58, 0x21, 0x12, 0xa4, 0x42,
			0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86,
			0xfa, 0x87, 0xdf, 0xae, 0x80, 0x22, 0x00, 0x10,
			0x53, 0x54, 0x55, 0x4e, 0x20, 0x74, 0x65, 0x73,
			0x74, 0x20, 0x63, 0x6c, 0x69, 0x65, 0x6e, 0x74,
			0x00, 0x24, 0x00, 0x04, 0x6e, 0x00, 0x01, 0xff,
			0x80, 0x29, 0x00, 0x08, 0x93, 0x2f, 0xf9, 0xb1,
			0x51, 0x26, 0x3b, 0x36, 0x00, 0x06, 0x00, 0x09,
			0x65, 0x76, 0x74, 0x6a, 0x3a, 0x68, 0x36, 0x76,
			0x59, 0x20, 0x20, 0x20, 0x00, 0x08, 0x00, 0x14,
			0x9a, 0xea, 0xa7, 0x0c, 0xbf, 0xd8, 0xcb, 0x56,
			0x78, 0x1e, 0xf2, 0xb5, 0xb2, 0xd3, 0xf2, 0x49,
			0xc1, 0xb5, 0x71, 0xa2, 0x80, 0x28, 0x00, 0x04,
			0xe5, 0x7a, 0x3b, 0xcf
		};
		/* RFC 5769 Section 2.1 request vector. */
		static const char password[] = "VOkJxbRl1RmTxUk/WvJxBt";
		static const char generated_password[] = "test-local-ice-password";
		uint8_t tampered[sizeof(authenticated_request)];
		uint8_t late_use_candidate[128];
		switch_size_t late_use_candidate_len;
		switch_size_t fingerprint_offset;
		uint16_t attribute_type;
		uint16_t attribute_length = 0;
		uint16_t message_length;
		uint32_t fingerprint;

		fst_check(switch_stun_packet_validate_auth(authenticated_request, sizeof(authenticated_request), password));
		fst_check(!switch_stun_packet_validate_auth(authenticated_request, sizeof(authenticated_request), "wrong-password"));

		memcpy(tampered, authenticated_request, sizeof(tampered));
		tampered[80] ^= 0x01;
		fst_check(!switch_stun_packet_validate_auth(tampered, sizeof(tampered), password));

		memcpy(tampered, authenticated_request, sizeof(tampered));
		tampered[104] ^= 0x01;
		fst_check(!switch_stun_packet_validate_auth(tampered, sizeof(tampered), password));

		late_use_candidate_len = build_authenticated_request(late_use_candidate, sizeof(late_use_candidate), "remote:local", generated_password, SWITCH_FALSE);
		fst_check(switch_stun_packet_validate_auth(late_use_candidate, (uint32_t)late_use_candidate_len, generated_password));

		fingerprint_offset = late_use_candidate_len - 8;
		memmove(late_use_candidate + fingerprint_offset + 4, late_use_candidate + fingerprint_offset, 8);
		attribute_type = htons(SWITCH_STUN_ATTR_USE_CAND);
		memcpy(late_use_candidate + fingerprint_offset, &attribute_type, sizeof(attribute_type));
		memcpy(late_use_candidate + fingerprint_offset + 2, &attribute_length, sizeof(attribute_length));
		memcpy(&message_length, late_use_candidate + 2, sizeof(message_length));
		message_length = htons((uint16_t)(ntohs(message_length) + 4));
		memcpy(late_use_candidate + 2, &message_length, sizeof(message_length));
		late_use_candidate_len += 4;
		fingerprint = htonl(switch_crc32_8bytes(late_use_candidate, (uint32_t)fingerprint_offset + 4) ^ 0x5354554e);
		memcpy(late_use_candidate + fingerprint_offset + 8, &fingerprint, sizeof(fingerprint));
		fst_check(!switch_stun_packet_validate_auth(late_use_candidate, (uint32_t)late_use_candidate_len, generated_password));
	}
	FST_TEST_END()
	FST_TEST_BEGIN(test_controlling_failover_requires_exact_authenticated_response)
	{
		switch_rtp_ice_t ice = { 0 };
		static const char probe_id[13] = "probe-id-123";
		static const char nomination_id[13] = "nominate-123";
		static const char wrong_id[13] = "wrong-id-123";
		switch_socket_t *local_socket = (switch_socket_t *)&ice;
		switch_socket_t *other_socket = (switch_socket_t *)&probe_id;
		int restart;

		ice.controlling_failover_idx = 3;
		ice.controlling_failover_state = SWITCH_RTP_ICE_CONTROLLING_FAILOVER_PROBING;
		ice.controlling_failover_local_port = 27018;
		ice.controlling_failover_local_family = AF_INET;
		ice.controlling_failover_socket = local_socket;
		memcpy(ice.controlling_failover_probe_id, probe_id, 12);
		memcpy(ice.controlling_failover_nomination_id, nomination_id, 12);

		fst_check(!switch_rtp_pvt_controlling_failover_response_matches(&ice,
			SWITCH_RTP_ICE_CONTROLLING_FAILOVER_PROBING, 3, probe_id, SWITCH_FALSE,
			local_socket, 27018, AF_INET));
		fst_check(!switch_rtp_pvt_controlling_failover_response_matches(&ice,
			SWITCH_RTP_ICE_CONTROLLING_FAILOVER_PROBING, 2, probe_id, SWITCH_TRUE,
			local_socket, 27018, AF_INET));
		fst_check(!switch_rtp_pvt_controlling_failover_response_matches(&ice,
			SWITCH_RTP_ICE_CONTROLLING_FAILOVER_PROBING, 3, wrong_id, SWITCH_TRUE,
			local_socket, 27018, AF_INET));
		fst_check(!switch_rtp_pvt_controlling_failover_response_matches(&ice,
			SWITCH_RTP_ICE_CONTROLLING_FAILOVER_NOMINATING, 3, nomination_id, SWITCH_TRUE,
			local_socket, 27018, AF_INET));
		fst_check(!switch_rtp_pvt_controlling_failover_response_matches(&ice,
			SWITCH_RTP_ICE_CONTROLLING_FAILOVER_PROBING, 3, probe_id, SWITCH_TRUE,
			other_socket, 27018, AF_INET));
		fst_check(!switch_rtp_pvt_controlling_failover_response_matches(&ice,
			SWITCH_RTP_ICE_CONTROLLING_FAILOVER_PROBING, 3, probe_id, SWITCH_TRUE,
			local_socket, 27019, AF_INET));
		fst_check(!switch_rtp_pvt_controlling_failover_response_matches(&ice,
			SWITCH_RTP_ICE_CONTROLLING_FAILOVER_PROBING, 3, probe_id, SWITCH_TRUE,
			local_socket, 27018, AF_INET6));
		fst_check(switch_rtp_pvt_controlling_failover_response_matches(&ice,
			SWITCH_RTP_ICE_CONTROLLING_FAILOVER_PROBING, 3, probe_id, SWITCH_TRUE,
			local_socket, 27018, AF_INET));

		ice.controlling_failover_state = SWITCH_RTP_ICE_CONTROLLING_FAILOVER_NOMINATING;
		fst_check(!switch_rtp_pvt_controlling_failover_response_matches(&ice,
			SWITCH_RTP_ICE_CONTROLLING_FAILOVER_NOMINATING, 3, probe_id, SWITCH_TRUE,
			local_socket, 27018, AF_INET));
		fst_check(switch_rtp_pvt_controlling_failover_response_matches(&ice,
			SWITCH_RTP_ICE_CONTROLLING_FAILOVER_NOMINATING, 3, nomination_id, SWITCH_TRUE,
			local_socket, 27018, AF_INET));

		for (restart = 0; restart < 2; restart++) {
			ice.controlling_failover_state = restart ? SWITCH_RTP_ICE_CONTROLLING_RESTART_NOMINATING :
				SWITCH_RTP_ICE_CONTROLLING_STARTUP_NOMINATING;
			fst_check(!switch_rtp_pvt_controlling_failover_response_matches(&ice,
				ice.controlling_failover_state, 3, nomination_id, SWITCH_FALSE,
				local_socket, 27018, AF_INET));
			fst_check(!switch_rtp_pvt_controlling_failover_response_matches(&ice,
				ice.controlling_failover_state, 2, nomination_id, SWITCH_TRUE,
				local_socket, 27018, AF_INET));
			fst_check(!switch_rtp_pvt_controlling_failover_response_matches(&ice,
				ice.controlling_failover_state, 3, wrong_id, SWITCH_TRUE,
				local_socket, 27018, AF_INET));
			fst_check(!switch_rtp_pvt_controlling_failover_response_matches(&ice,
				ice.controlling_failover_state, 3, nomination_id, SWITCH_TRUE,
				other_socket, 27018, AF_INET));
			fst_check(!switch_rtp_pvt_controlling_failover_response_matches(&ice,
				ice.controlling_failover_state, 3, nomination_id, SWITCH_TRUE,
				local_socket, 27019, AF_INET));
			fst_check(!switch_rtp_pvt_controlling_failover_response_matches(&ice,
				ice.controlling_failover_state, 3, nomination_id, SWITCH_TRUE,
				local_socket, 27018, AF_INET6));
			fst_check(switch_rtp_pvt_controlling_failover_response_matches(&ice,
				ice.controlling_failover_state, 3, nomination_id, SWITCH_TRUE,
				local_socket, 27018, AF_INET));
		}
	}
	FST_TEST_END()
	FST_TEST_BEGIN(test_role_conflict_requires_authenticated_exact_transaction_and_tuple)
	{
		switch_memory_pool_t *pool = NULL;
		switch_sockaddr_t *current_addr = NULL;
		switch_sockaddr_t *alternative_addr = NULL;
		switch_rtp_ice_t ice = { 0 };
		static const char selected_id[13] = "selected-123";
		static const char probe_id[13] = "probe-id-123";
		static const char wrong_id[13] = "wrong-id-123";
		switch_socket_t *local_socket = (switch_socket_t *)&ice;
		switch_bool_t sent_controlling = SWITCH_FALSE;

		fst_xcheck(switch_core_new_memory_pool(&pool) == SWITCH_STATUS_SUCCESS, "switch_core_new_memory_pool()");
		fst_xcheck(switch_sockaddr_info_get(&current_addr, "192.0.2.10", SWITCH_UNSPEC, 40000, 0, pool) ==
			SWITCH_STATUS_SUCCESS, "current address");
		fst_xcheck(switch_sockaddr_info_get(&alternative_addr, "192.0.2.20", SWITCH_UNSPEC, 50000, 0, pool) ==
			SWITCH_STATUS_SUCCESS, "alternative address");

		ice.addr = current_addr;
		memcpy(ice.selected_pair_check_ids[0], selected_id, 12);
		ice.selected_pair_check_controlling[0] = 1;
		ice.selected_pair_check_remote_addr[0] = current_addr;
		ice.selected_pair_check_count = 1;
		ice.selected_pair_check_socket = local_socket;
		ice.selected_pair_check_local_port = 27018;
		ice.selected_pair_check_local_family = AF_INET;
		fst_check(!switch_rtp_pvt_ice_role_conflict_response_matches(&ice, current_addr, 0,
			selected_id, SWITCH_FALSE, local_socket, 27018, AF_INET, &sent_controlling));
		/* A destination migration must not make the old transaction match the
		 * new current tuple; it remains correlated to its request destination. */
		ice.addr = alternative_addr;
		fst_check(!switch_rtp_pvt_ice_role_conflict_response_matches(&ice, alternative_addr, 1,
			selected_id, SWITCH_TRUE, local_socket, 27018, AF_INET, &sent_controlling));
		fst_check(!switch_rtp_pvt_ice_role_conflict_response_matches(&ice, current_addr, 0,
			wrong_id, SWITCH_TRUE, local_socket, 27018, AF_INET, &sent_controlling));
		fst_check(switch_rtp_pvt_ice_role_conflict_response_matches(&ice, current_addr, 0,
			selected_id, SWITCH_TRUE, local_socket, 27018, AF_INET, &sent_controlling));
		fst_check(sent_controlling);
		fst_check(!switch_rtp_pvt_ice_role_conflict_response_matches(&ice, current_addr, 0,
			selected_id, SWITCH_TRUE, local_socket, 27018, AF_INET, &sent_controlling));

		/* Apply the role carried by the matched request, even if current state
		 * changed while that transaction was in flight. */
		ice.type = ICE_VANILLA | ICE_CONTROLLED;
		switch_rtp_pvt_ice_role_conflict_apply(&ice, SWITCH_TRUE);
		fst_check(ice.type & ICE_CONTROLLED);
		ice.type = ICE_VANILLA;
		switch_rtp_pvt_ice_role_conflict_apply(&ice, SWITCH_FALSE);
		fst_check(!(ice.type & ICE_CONTROLLED));

		ice.controlling_failover_state = SWITCH_RTP_ICE_CONTROLLING_FAILOVER_PROBING;
		ice.controlling_failover_idx = 1;
		ice.controlling_failover_local_port = 27018;
		ice.controlling_failover_local_family = AF_INET;
		ice.controlling_failover_socket = local_socket;
		memcpy(ice.controlling_failover_probe_id, probe_id, 12);
		fst_check(!switch_rtp_pvt_ice_role_conflict_response_matches(&ice, alternative_addr, 1,
			probe_id, SWITCH_TRUE, local_socket, 27019, AF_INET, &sent_controlling));
		fst_check(switch_rtp_pvt_ice_role_conflict_response_matches(&ice, alternative_addr, 1,
			probe_id, SWITCH_TRUE, local_socket, 27018, AF_INET, &sent_controlling));
		fst_check(sent_controlling);
		fst_check(!switch_rtp_pvt_ice_role_conflict_response_matches(&ice, alternative_addr, 1,
			probe_id, SWITCH_TRUE, local_socket, 27018, AF_INET, &sent_controlling));

		switch_core_destroy_memory_pool(&pool);
	}
	FST_TEST_END()
	FST_TEST_BEGIN(test_role_conflict_cancels_transactions_and_rotates_tie_breaker)
	{
		switch_rtp_ice_t rtp_ice = { 0 };
		switch_rtp_ice_t rtcp_ice = { 0 };
		char tie_breaker[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
		char old_tie_breaker[8];

		rtp_ice.controlling_failover_state = SWITCH_RTP_ICE_CONTROLLING_FAILOVER_PROBING;
		rtp_ice.controlling_failover_idx = 2;
		memcpy(rtp_ice.controlling_failover_probe_id, "probe-id-123", 12);
		memcpy(rtp_ice.selected_pair_check_ids[0], "selected-123", 12);
		rtp_ice.selected_pair_check_controlling[0] = 1;
		rtp_ice.selected_pair_check_remote_addr[0] = (switch_sockaddr_t *)&rtp_ice;
		rtp_ice.selected_pair_check_count = 1;
		memcpy(rtp_ice.last_sent_id, "selected-123", 12);
		memcpy(rtcp_ice.selected_pair_check_ids[0], "rtcp-check12", 12);
		rtcp_ice.selected_pair_check_count = 1;

		switch_rtp_pvt_ice_role_conflict_cancel(&rtp_ice);
		switch_rtp_pvt_ice_role_conflict_cancel(&rtcp_ice);
		fst_check(rtp_ice.controlling_failover_state == SWITCH_RTP_ICE_CONTROLLING_FAILOVER_IDLE);
		fst_check(rtp_ice.controlling_failover_idx == -1);
		fst_check(!rtp_ice.selected_pair_check_count);
		fst_check(!rtcp_ice.selected_pair_check_count);
		fst_check(!rtp_ice.selected_pair_check_ids[0][0]);
		fst_check(!rtp_ice.selected_pair_check_remote_addr[0]);
		fst_check(!rtcp_ice.selected_pair_check_ids[0][0]);
		fst_check(!rtp_ice.last_sent_id[0]);

		rtp_ice.controlling_failover_state = SWITCH_RTP_ICE_CONTROLLING_STARTUP_NOMINATING;
		rtp_ice.controlling_failover_idx = 1;
		memcpy(rtp_ice.controlling_failover_nomination_id, "nominate-123", 12);
		switch_rtp_pvt_ice_role_conflict_cancel(&rtp_ice);
		fst_check(rtp_ice.controlling_failover_state == SWITCH_RTP_ICE_CONTROLLING_FAILOVER_IDLE);
		fst_check(rtp_ice.controlling_failover_idx == -1);
		fst_check(!rtp_ice.controlling_failover_nomination_id[0]);

		memcpy(old_tie_breaker, tie_breaker, sizeof(tie_breaker));
		switch_rtp_pvt_ice_role_conflict_rotate_tie_breaker(tie_breaker);
		fst_check(memcmp(tie_breaker, old_tie_breaker, sizeof(tie_breaker)));
	}
	FST_TEST_END()
	FST_TEST_BEGIN(test_local_prflx_priority_uses_component_id)
	{
		fst_check(switch_rtp_pvt_ice_local_prflx_priority(SWITCH_FALSE, SWITCH_FALSE) == 0x6effffffU);
		fst_check(switch_rtp_pvt_ice_local_prflx_priority(SWITCH_TRUE, SWITCH_TRUE) == 0x6effffffU);
		fst_check(switch_rtp_pvt_ice_local_prflx_priority(SWITCH_TRUE, SWITCH_FALSE) == 0x6efffffeU);
	}
	FST_TEST_END()
	FST_TEST_BEGIN(test_controlling_failover_retransmission_timer)
	{
		switch_time_t started_us = 1000000;

		fst_check(switch_rtp_pvt_controlling_failover_timer_action(
			SWITCH_RTP_ICE_CONTROLLING_FAILOVER_IDLE, 0, 0, 0, started_us) ==
			SWITCH_RTP_ICE_CONTROLLING_TIMER_NONE);
		fst_check(switch_rtp_pvt_controlling_failover_timer_action(
			SWITCH_RTP_ICE_CONTROLLING_FAILOVER_PROBING, started_us, started_us, 1,
			started_us + 999000) == SWITCH_RTP_ICE_CONTROLLING_TIMER_NONE);
		fst_check(switch_rtp_pvt_controlling_failover_timer_action(
			SWITCH_RTP_ICE_CONTROLLING_FAILOVER_PROBING, started_us, started_us, 1,
			started_us + 1000000) == SWITCH_RTP_ICE_CONTROLLING_TIMER_RETRANSMIT);
		fst_check(switch_rtp_pvt_controlling_failover_timer_action(
			SWITCH_RTP_ICE_CONTROLLING_FAILOVER_NOMINATING, started_us, started_us + 3000000, 4,
			started_us + 3999000) == SWITCH_RTP_ICE_CONTROLLING_TIMER_NONE);
		fst_check(switch_rtp_pvt_controlling_failover_timer_action(
			SWITCH_RTP_ICE_CONTROLLING_FAILOVER_NOMINATING, started_us, started_us + 3000000, 4,
			started_us + 4000000) == SWITCH_RTP_ICE_CONTROLLING_TIMER_EXPIRE);
		fst_check(switch_rtp_pvt_controlling_failover_timer_action(
			SWITCH_RTP_ICE_CONTROLLING_FAILOVER_PROBING, started_us, started_us + 4000000, 2,
			started_us + 5000000) == SWITCH_RTP_ICE_CONTROLLING_TIMER_EXPIRE);
		fst_check(switch_rtp_pvt_controlling_failover_timer_action(
			SWITCH_RTP_ICE_CONTROLLING_STARTUP_NOMINATING, started_us, started_us, 1,
			started_us + 1000000) == SWITCH_RTP_ICE_CONTROLLING_TIMER_RETRANSMIT);
		fst_check(switch_rtp_pvt_controlling_failover_timer_action(
			SWITCH_RTP_ICE_CONTROLLING_RESTART_NOMINATING, started_us, started_us, 1,
			started_us + 1000000) == SWITCH_RTP_ICE_CONTROLLING_TIMER_RETRANSMIT);
		fst_check(switch_rtp_pvt_controlling_failover_timer_action(
			SWITCH_RTP_ICE_CONTROLLING_RESTART_NOMINATING, started_us, started_us + 4000000, 2,
			started_us + 5000000) == SWITCH_RTP_ICE_CONTROLLING_TIMER_EXPIRE);
	}
	FST_TEST_END()
	FST_TEST_BEGIN(test_ice_role_attributes_accept_stable_tie_breaker)
	{
		uint8_t buf[128] = { 0 };
		static const char tie_breaker[8] = { 0x01, 0x23, 0x45, 0x67, 0x11, 0x22, 0x33, 0x44 };
		switch_stun_packet_t *packet;
		switch_stun_packet_attribute_t *attribute;

		packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_REQUEST, NULL, buf);
		fst_check(switch_stun_packet_attribute_add_controlling_value(packet, tie_breaker));
		attribute = (switch_stun_packet_attribute_t *)&packet->first_attribute;
		fst_check(ntohs(attribute->type) == SWITCH_STUN_ATTR_CONTROLLING);
		fst_check(ntohs(attribute->length) == sizeof(tie_breaker));
		fst_check(!memcmp(attribute->value, tie_breaker, sizeof(tie_breaker)));

		memset(buf, 0, sizeof(buf));
		packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_REQUEST, NULL, buf);
		fst_check(switch_stun_packet_attribute_add_controlled_value(packet, tie_breaker));
		attribute = (switch_stun_packet_attribute_t *)&packet->first_attribute;
		fst_check(ntohs(attribute->type) == SWITCH_STUN_ATTR_CONTROLLED);
		fst_check(ntohs(attribute->length) == sizeof(tie_breaker));
		fst_check(!memcmp(attribute->value, tie_breaker, sizeof(tie_breaker)));
	}
	FST_TEST_END()
	FST_TEST_BEGIN(test_restart_prflx_requires_explicit_authenticated_current_generation)
	{
		switch_rtp_ice_t ice = { 0 };
		switch_time_t now = 10000000;

		fst_check(!switch_rtp_pvt_restart_prflx_allowed(&ice, DS_READY, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE,
			SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, 5000, now));

		ice.restart_provisional = 1;
		ice.restart_provisional_us = now - 1000;
		fst_check(switch_rtp_pvt_restart_prflx_allowed(&ice, DS_READY, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE,
			SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, 5000, now));
		fst_check(!switch_rtp_pvt_restart_prflx_allowed(&ice, DS_HANDSHAKE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE,
			SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, 5000, now));
		fst_check(!switch_rtp_pvt_restart_prflx_allowed(&ice, DS_READY, SWITCH_FALSE, SWITCH_TRUE, SWITCH_TRUE,
			SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, 5000, now));
		fst_check(!switch_rtp_pvt_restart_prflx_allowed(&ice, DS_READY, SWITCH_TRUE, SWITCH_FALSE, SWITCH_TRUE,
			SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, 5000, now));
		fst_check(!switch_rtp_pvt_restart_prflx_allowed(&ice, DS_READY, SWITCH_TRUE, SWITCH_TRUE, SWITCH_FALSE,
			SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, 5000, now));
		fst_check(!switch_rtp_pvt_restart_prflx_allowed(&ice, DS_READY, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE,
			SWITCH_FALSE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, 5000, now));
		fst_check(!switch_rtp_pvt_restart_prflx_allowed(&ice, DS_READY, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE,
			SWITCH_TRUE, SWITCH_FALSE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, 5000, now));
		fst_check(!switch_rtp_pvt_restart_prflx_allowed(&ice, DS_READY, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE,
			SWITCH_TRUE, SWITCH_TRUE, SWITCH_FALSE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, 5000, now));
		fst_check(!switch_rtp_pvt_restart_prflx_allowed(&ice, DS_READY, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE,
			SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_FALSE, SWITCH_TRUE, SWITCH_TRUE, 5000, now));
		fst_check(!switch_rtp_pvt_restart_prflx_allowed(&ice, DS_READY, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE,
			SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_FALSE, SWITCH_TRUE, 5000, now));
		fst_check(!switch_rtp_pvt_restart_prflx_allowed(&ice, DS_READY, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE,
			SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_FALSE, 5000, now));

		ice.restart_provisional_us = now - 6000000;
		fst_check(!switch_rtp_pvt_restart_prflx_allowed(&ice, DS_READY, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE,
			SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, 5000, now));
		fst_check(!ice.restart_provisional);
		fst_check(!ice.restart_provisional_us);

		ice.restart_provisional = 1;
		ice.restart_provisional_us = now + 1;
		fst_check(!switch_rtp_pvt_restart_prflx_allowed(&ice, DS_READY, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE,
			SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, SWITCH_TRUE, 5000, now));
		fst_check(!ice.restart_provisional);
		fst_check(!ice.restart_provisional_us);
	}
	FST_TEST_END()
	FST_TEST_BEGIN(test_preserve_active_dtls_tuple_requires_exact_current_peer)
	{
		switch_memory_pool_t *pool = NULL;
		switch_sockaddr_t *current_addr = NULL;
		switch_sockaddr_t *different_port_addr = NULL;
		switch_sockaddr_t *handshake_peer_addr = NULL;

		fst_xcheck(switch_core_new_memory_pool(&pool) == SWITCH_STATUS_SUCCESS, "switch_core_new_memory_pool()");
		fst_xcheck(switch_sockaddr_info_get(&current_addr, "192.0.2.10", SWITCH_UNSPEC, 40000, 0, pool) == SWITCH_STATUS_SUCCESS,
			"current address");
		fst_xcheck(switch_sockaddr_info_get(&different_port_addr, "192.0.2.10", SWITCH_UNSPEC, 40002, 0, pool) == SWITCH_STATUS_SUCCESS,
			"different-port address");
		fst_xcheck(switch_sockaddr_info_get(&handshake_peer_addr, "192.0.2.10", SWITCH_UNSPEC, 40000, 0, pool) == SWITCH_STATUS_SUCCESS,
			"handshake peer address");

		fst_check(switch_rtp_pvt_should_preserve_active_dtls_tuple(current_addr, handshake_peer_addr, DS_HANDSHAKE, SWITCH_TRUE, SWITCH_FALSE));
		fst_check(!switch_rtp_pvt_should_preserve_active_dtls_tuple(different_port_addr, handshake_peer_addr, DS_HANDSHAKE, SWITCH_TRUE, SWITCH_FALSE));
		fst_check(!switch_rtp_pvt_should_preserve_active_dtls_tuple(current_addr, handshake_peer_addr, DS_OFF, SWITCH_TRUE, SWITCH_FALSE));
		fst_check(!switch_rtp_pvt_should_preserve_active_dtls_tuple(current_addr, handshake_peer_addr, DS_READY, SWITCH_TRUE, SWITCH_FALSE));
		fst_check(!switch_rtp_pvt_should_preserve_active_dtls_tuple(current_addr, handshake_peer_addr, DS_HANDSHAKE, SWITCH_FALSE, SWITCH_FALSE));
		fst_check(!switch_rtp_pvt_should_preserve_active_dtls_tuple(current_addr, handshake_peer_addr, DS_HANDSHAKE, SWITCH_TRUE, SWITCH_TRUE));

		switch_core_destroy_memory_pool(&pool);
	}
	FST_TEST_END()
	FST_TEST_BEGIN(test_rtcp_mux_prflx_selection_blocks_late_candidate_reselection)
	{
		ice_t ice = { 0 };

		fst_check(!switch_rtp_pvt_ice_selection_complete(NULL, SWITCH_TRUE));
		fst_check(!switch_rtp_pvt_ice_selection_complete(&ice, SWITCH_TRUE));

		/* Production ordering: authenticated peer-reflexive nomination selects
		 * RTP before the late advertised host/srflx candidate is processed.
		 * RTCP-mux makes that one selected component a complete ICE transport. */
		ice.is_chosen[IPR_RTP] = 1;
		fst_check(switch_rtp_pvt_ice_selection_complete(&ice, SWITCH_TRUE));
		fst_check(!switch_rtp_pvt_ice_selection_complete(&ice, SWITCH_FALSE));

		ice.is_chosen[IPR_RTCP] = 1;
		fst_check(switch_rtp_pvt_ice_selection_complete(&ice, SWITCH_FALSE));
	}
	FST_TEST_END()
	FST_TEST_BEGIN(test_same_generation_late_trickle_preserves_dtls_handshake)
	{
		switch_memory_pool_t *pool = NULL;
		switch_sockaddr_t *current_rtp_addr = NULL;
		switch_sockaddr_t *same_rtp_addr = NULL;
		switch_sockaddr_t *different_rtp_addr = NULL;
		switch_rtp_pvt_ice_tuple_t rtp_tuple = { 0 };

		fst_xcheck(switch_core_new_memory_pool(&pool) == SWITCH_STATUS_SUCCESS, "switch_core_new_memory_pool()");
		fst_xcheck(switch_sockaddr_info_get(&current_rtp_addr, "198.51.100.10", SWITCH_UNSPEC, 40000, 0, pool) == SWITCH_STATUS_SUCCESS,
			"current RTP address");
		fst_xcheck(switch_sockaddr_info_get(&same_rtp_addr, "198.51.100.10", SWITCH_UNSPEC, 40000, 0, pool) == SWITCH_STATUS_SUCCESS,
			"same RTP address");
		fst_xcheck(switch_sockaddr_info_get(&different_rtp_addr, "198.51.100.10", SWITCH_UNSPEC, 40002, 0, pool) == SWITCH_STATUS_SUCCESS,
			"different RTP address");

		rtp_tuple.selected = SWITCH_TRUE;
		rtp_tuple.ready = SWITCH_TRUE;
		rtp_tuple.transport = "udp";
		rtp_tuple.current_addr = current_rtp_addr;
		rtp_tuple.selected_addr = same_rtp_addr;

		fst_check(switch_rtp_pvt_should_preserve_trickle_dtls(SWITCH_TRUE, SWITCH_TRUE,
			&rtp_tuple, SWITCH_TRUE,
			"remoteUfrag", "remotePassword", "remoteUfrag", "remotePassword",
			DS_HANDSHAKE, SWITCH_TRUE));
		fst_check(!switch_rtp_pvt_should_preserve_trickle_dtls(SWITCH_TRUE, SWITCH_TRUE,
			&rtp_tuple, SWITCH_FALSE,
			"remoteUfrag", "remotePassword", "remoteUfrag", "remotePassword",
			DS_HANDSHAKE, SWITCH_TRUE));
		fst_check(!switch_rtp_pvt_should_preserve_trickle_dtls(SWITCH_FALSE, SWITCH_TRUE,
			&rtp_tuple, SWITCH_TRUE,
			"remoteUfrag", "remotePassword", "remoteUfrag", "remotePassword",
			DS_HANDSHAKE, SWITCH_TRUE));
		fst_check(!switch_rtp_pvt_should_preserve_trickle_dtls(SWITCH_TRUE, SWITCH_FALSE,
			&rtp_tuple, SWITCH_TRUE,
			"remoteUfrag", "remotePassword", "remoteUfrag", "remotePassword",
			DS_HANDSHAKE, SWITCH_TRUE));

		rtp_tuple.selected = SWITCH_FALSE;
		fst_check(!switch_rtp_pvt_should_preserve_trickle_dtls(SWITCH_TRUE, SWITCH_TRUE,
			&rtp_tuple, SWITCH_TRUE,
			"remoteUfrag", "remotePassword", "remoteUfrag", "remotePassword",
			DS_HANDSHAKE, SWITCH_TRUE));
		rtp_tuple.selected = SWITCH_TRUE;
		rtp_tuple.ready = SWITCH_FALSE;
		fst_check(!switch_rtp_pvt_should_preserve_trickle_dtls(SWITCH_TRUE, SWITCH_TRUE,
			&rtp_tuple, SWITCH_TRUE,
			"remoteUfrag", "remotePassword", "remoteUfrag", "remotePassword",
			DS_HANDSHAKE, SWITCH_TRUE));
		rtp_tuple.ready = SWITCH_TRUE;
		rtp_tuple.transport = "tcp";
		fst_check(!switch_rtp_pvt_should_preserve_trickle_dtls(SWITCH_TRUE, SWITCH_TRUE,
			&rtp_tuple, SWITCH_TRUE,
			"remoteUfrag", "remotePassword", "remoteUfrag", "remotePassword",
			DS_HANDSHAKE, SWITCH_TRUE));
		rtp_tuple.transport = NULL;
		fst_check(!switch_rtp_pvt_should_preserve_trickle_dtls(SWITCH_TRUE, SWITCH_TRUE,
			&rtp_tuple, SWITCH_TRUE,
			"remoteUfrag", "remotePassword", "remoteUfrag", "remotePassword",
			DS_HANDSHAKE, SWITCH_TRUE));
		rtp_tuple.transport = "udp";
		rtp_tuple.selected_addr = different_rtp_addr;
		fst_check(!switch_rtp_pvt_should_preserve_trickle_dtls(SWITCH_TRUE, SWITCH_TRUE,
			&rtp_tuple, SWITCH_TRUE,
			"remoteUfrag", "remotePassword", "remoteUfrag", "remotePassword",
			DS_HANDSHAKE, SWITCH_TRUE));
		rtp_tuple.selected_addr = same_rtp_addr;

		fst_check(!switch_rtp_pvt_should_preserve_trickle_dtls(SWITCH_TRUE, SWITCH_TRUE,
			&rtp_tuple, SWITCH_TRUE,
			"remoteUfrag", "remotePassword", "newRemoteUfrag", "remotePassword",
			DS_HANDSHAKE, SWITCH_TRUE));
		fst_check(!switch_rtp_pvt_should_preserve_trickle_dtls(SWITCH_TRUE, SWITCH_TRUE,
			&rtp_tuple, SWITCH_TRUE,
			"remoteUfrag", "remotePassword", "remoteUfrag", "newRemotePassword",
			DS_HANDSHAKE, SWITCH_TRUE));
		fst_check(!switch_rtp_pvt_should_preserve_trickle_dtls(SWITCH_TRUE, SWITCH_TRUE,
			&rtp_tuple, SWITCH_TRUE,
			NULL, "remotePassword", "remoteUfrag", "remotePassword",
			DS_HANDSHAKE, SWITCH_TRUE));
		fst_check(!switch_rtp_pvt_should_preserve_trickle_dtls(SWITCH_TRUE, SWITCH_TRUE,
			&rtp_tuple, SWITCH_TRUE,
			"remoteUfrag", NULL, "remoteUfrag", "remotePassword",
			DS_HANDSHAKE, SWITCH_TRUE));
		fst_check(!switch_rtp_pvt_should_preserve_trickle_dtls(SWITCH_TRUE, SWITCH_TRUE,
			&rtp_tuple, SWITCH_TRUE,
			"remoteUfrag", "remotePassword", "remoteUfrag", "remotePassword",
			DS_READY, SWITCH_TRUE));
		fst_check(!switch_rtp_pvt_should_preserve_trickle_dtls(SWITCH_TRUE, SWITCH_TRUE,
			&rtp_tuple, SWITCH_TRUE,
			"remoteUfrag", "remotePassword", "remoteUfrag", "remotePassword",
			DS_HANDSHAKE, SWITCH_FALSE));

		switch_core_destroy_memory_pool(&pool);
	}
	FST_TEST_END()
	FST_TEST_BEGIN(test_recovery_dtls_nomination_proof_requires_authenticated_direct_request)
	{
		fst_check(switch_rtp_pvt_recovery_dtls_nomination_proof(SWITCH_FALSE, SWITCH_FALSE) ==
			SWITCH_RTP_RECOVERY_NOMINATION_NONE);
		fst_check(switch_rtp_pvt_recovery_dtls_nomination_proof(SWITCH_FALSE, SWITCH_TRUE) ==
			SWITCH_RTP_RECOVERY_NOMINATION_NONE);
		fst_check(switch_rtp_pvt_recovery_dtls_nomination_proof(SWITCH_TRUE, SWITCH_FALSE) ==
			SWITCH_RTP_RECOVERY_NOMINATION_NONE);
		fst_check(switch_rtp_pvt_recovery_dtls_nomination_proof(SWITCH_TRUE, SWITCH_TRUE) ==
			SWITCH_RTP_RECOVERY_NOMINATION_AUTHENTICATED_CURRENT);
	}
	FST_TEST_END()
	FST_TEST_BEGIN(test_authenticated_recovery_nomination_syncs_nonmux_ice_addr)
	{
		switch_memory_pool_t *pool = NULL;
		switch_sockaddr_t *current_addr = NULL;
		switch_sockaddr_t *remote_addr = NULL;
		switch_sockaddr_t *nominated_addr = NULL;
		switch_sockaddr_t *other_addr = NULL;
		switch_sockaddr_t *ice_addr = NULL;

		fst_xcheck(switch_core_new_memory_pool(&pool) == SWITCH_STATUS_SUCCESS, "switch_core_new_memory_pool()");
		fst_xcheck(switch_sockaddr_info_get(&current_addr, "198.51.100.10", SWITCH_UNSPEC, 40000, 0, pool) == SWITCH_STATUS_SUCCESS,
			"current address");
		fst_xcheck(switch_sockaddr_info_get(&remote_addr, "198.51.100.20", SWITCH_UNSPEC, 50000, 0, pool) == SWITCH_STATUS_SUCCESS,
			"remote address");
		fst_xcheck(switch_sockaddr_info_get(&nominated_addr, "198.51.100.20", SWITCH_UNSPEC, 50000, 0, pool) == SWITCH_STATUS_SUCCESS,
			"nominated address");
		fst_xcheck(switch_sockaddr_info_get(&other_addr, "198.51.100.20", SWITCH_UNSPEC, 50002, 0, pool) == SWITCH_STATUS_SUCCESS,
			"other address");

		ice_addr = current_addr;
		fst_check(switch_rtp_pvt_sync_authenticated_recovery_ice_addr(&ice_addr, remote_addr, nominated_addr,
			DS_HANDSHAKE, SWITCH_FALSE, SWITCH_FALSE, SWITCH_TRUE,
			SWITCH_RTP_RECOVERY_NOMINATION_AUTHENTICATED_CURRENT));
		fst_check(ice_addr == remote_addr);
		fst_check(!switch_rtp_pvt_should_guard_recovery_dtls_tuple(ice_addr, nominated_addr, DS_HANDSHAKE,
			SWITCH_FALSE, SWITCH_TRUE, SWITCH_RTP_RECOVERY_NOMINATION_NONE));

		ice_addr = current_addr;
		fst_check(!switch_rtp_pvt_sync_authenticated_recovery_ice_addr(&ice_addr, remote_addr, nominated_addr,
			DS_HANDSHAKE, SWITCH_FALSE, SWITCH_FALSE, SWITCH_TRUE, SWITCH_RTP_RECOVERY_NOMINATION_PERSISTED));
		fst_check(ice_addr == current_addr);
		fst_check(!switch_rtp_pvt_sync_authenticated_recovery_ice_addr(&ice_addr, remote_addr, other_addr,
			DS_HANDSHAKE, SWITCH_FALSE, SWITCH_FALSE, SWITCH_TRUE,
			SWITCH_RTP_RECOVERY_NOMINATION_AUTHENTICATED_CURRENT));
		fst_check(ice_addr == current_addr);
		fst_check(!switch_rtp_pvt_sync_authenticated_recovery_ice_addr(&ice_addr, remote_addr, nominated_addr,
			DS_HANDSHAKE, SWITCH_TRUE, SWITCH_FALSE, SWITCH_TRUE,
			SWITCH_RTP_RECOVERY_NOMINATION_AUTHENTICATED_CURRENT));
		fst_check(!switch_rtp_pvt_sync_authenticated_recovery_ice_addr(&ice_addr, remote_addr, nominated_addr,
			DS_HANDSHAKE, SWITCH_FALSE, SWITCH_TRUE, SWITCH_TRUE,
			SWITCH_RTP_RECOVERY_NOMINATION_AUTHENTICATED_CURRENT));
		fst_check(!switch_rtp_pvt_sync_authenticated_recovery_ice_addr(&ice_addr, remote_addr, nominated_addr,
			DS_READY, SWITCH_FALSE, SWITCH_FALSE, SWITCH_TRUE,
			SWITCH_RTP_RECOVERY_NOMINATION_AUTHENTICATED_CURRENT));
		fst_check(!switch_rtp_pvt_sync_authenticated_recovery_ice_addr(&ice_addr, remote_addr, nominated_addr,
			DS_SETUP, SWITCH_FALSE, SWITCH_FALSE, SWITCH_FALSE,
			SWITCH_RTP_RECOVERY_NOMINATION_AUTHENTICATED_CURRENT));
		fst_check(ice_addr == current_addr);

		switch_core_destroy_memory_pool(&pool);
	}
	FST_TEST_END()
	FST_TEST_BEGIN(test_recovery_dtls_restart_tuple_mutation_policy)
	{
		switch_memory_pool_t *pool = NULL;
		switch_sockaddr_t *current_addr = NULL;
		switch_sockaddr_t *same_addr = NULL;
		switch_sockaddr_t *different_port_addr = NULL;
		switch_sockaddr_t *relay_addr = NULL;
		switch_rtp_recovery_nomination_proof_t trusted_proof;

		fst_xcheck(switch_core_new_memory_pool(&pool) == SWITCH_STATUS_SUCCESS, "switch_core_new_memory_pool()");
		fst_xcheck(switch_sockaddr_info_get(&current_addr, "198.51.100.10", SWITCH_UNSPEC, 40000, 0, pool) == SWITCH_STATUS_SUCCESS,
			"current address");
		fst_xcheck(switch_sockaddr_info_get(&same_addr, "198.51.100.10", SWITCH_UNSPEC, 40000, 0, pool) == SWITCH_STATUS_SUCCESS,
			"same address");
		fst_xcheck(switch_sockaddr_info_get(&different_port_addr, "198.51.100.10", SWITCH_UNSPEC, 40002, 0, pool) == SWITCH_STATUS_SUCCESS,
			"different-port address");
		fst_xcheck(switch_sockaddr_info_get(&relay_addr, "198.51.100.20", SWITCH_UNSPEC, 50000, 0, pool) == SWITCH_STATUS_SUCCESS,
			"relay address");

		trusted_proof = switch_rtp_pvt_recovery_dtls_nomination_proof(SWITCH_TRUE, SWITCH_TRUE);
		fst_check(switch_rtp_pvt_should_guard_recovery_dtls_tuple(current_addr, relay_addr, DS_HANDSHAKE,
			SWITCH_FALSE, SWITCH_TRUE, SWITCH_RTP_RECOVERY_NOMINATION_NONE));
		fst_check(switch_rtp_pvt_should_guard_recovery_dtls_tuple(current_addr, different_port_addr, DS_SETUP,
			SWITCH_FALSE, SWITCH_TRUE, SWITCH_RTP_RECOVERY_NOMINATION_NONE));
		fst_check(!switch_rtp_pvt_should_guard_recovery_dtls_tuple(current_addr, relay_addr, DS_HANDSHAKE,
			SWITCH_FALSE, SWITCH_TRUE, trusted_proof));
		/* DTLS packets cannot renew persisted nomination proof. */
		fst_check(switch_rtp_pvt_should_guard_recovery_dtls_tuple(current_addr, relay_addr, DS_HANDSHAKE,
			SWITCH_FALSE, SWITCH_TRUE, SWITCH_RTP_RECOVERY_NOMINATION_PERSISTED));
		/* A moved ICE tuple needs no off-current bypass. */
		fst_check(!switch_rtp_pvt_should_guard_recovery_dtls_tuple(current_addr, same_addr, DS_HANDSHAKE,
			SWITCH_FALSE, SWITCH_TRUE, SWITCH_RTP_RECOVERY_NOMINATION_NONE));

		switch_core_destroy_memory_pool(&pool);
	}
	FST_TEST_END()
	FST_TEST_BEGIN(test_recovery_dtls_candidate_response_and_fallback_policy)
	{
		switch_memory_pool_t *pool = NULL;
		switch_sockaddr_t *current_addr = NULL;
		switch_sockaddr_t *candidate_addr = NULL;
		switch_rtp_recovery_nomination_proof_t trusted_proof;

		fst_xcheck(switch_core_new_memory_pool(&pool) == SWITCH_STATUS_SUCCESS, "switch_core_new_memory_pool()");
		fst_xcheck(switch_sockaddr_info_get(&current_addr, "203.0.113.10", SWITCH_UNSPEC, 41000, 0, pool) == SWITCH_STATUS_SUCCESS,
			"current address");
		fst_xcheck(switch_sockaddr_info_get(&candidate_addr, "203.0.113.20", SWITCH_UNSPEC, 51000, 0, pool) == SWITCH_STATUS_SUCCESS,
			"candidate address");

		trusted_proof = switch_rtp_pvt_recovery_dtls_nomination_proof(SWITCH_TRUE, SWITCH_TRUE);
		/* Binding responses cannot prove current-generation nomination. */
		fst_check(switch_rtp_pvt_should_guard_recovery_dtls_tuple(current_addr, candidate_addr, DS_HANDSHAKE,
			SWITCH_FALSE, SWITCH_TRUE, SWITCH_RTP_RECOVERY_NOMINATION_NONE));
		fst_check(switch_rtp_pvt_should_guard_recovery_dtls_tuple(current_addr, candidate_addr, DS_SETUP,
			SWITCH_FALSE, SWITCH_TRUE, SWITCH_RTP_RECOVERY_NOMINATION_PERSISTED));
		/* Missed-candidate fallback may use only proof from this request. */
		fst_check(switch_rtp_pvt_should_guard_recovery_dtls_tuple(current_addr, candidate_addr, DS_HANDSHAKE,
			SWITCH_FALSE, SWITCH_TRUE, SWITCH_RTP_RECOVERY_NOMINATION_PERSISTED));
		fst_check(!switch_rtp_pvt_should_guard_recovery_dtls_tuple(current_addr, candidate_addr, DS_HANDSHAKE,
			SWITCH_FALSE, SWITCH_TRUE, trusted_proof));

		switch_core_destroy_memory_pool(&pool);
	}
	FST_TEST_END()
	FST_TEST_BEGIN(test_recovery_dtls_tuple_policy_leaves_other_paths_unchanged)
	{
		switch_memory_pool_t *pool = NULL;
		switch_sockaddr_t *current_addr = NULL;
		switch_sockaddr_t *candidate_addr = NULL;

		fst_xcheck(switch_core_new_memory_pool(&pool) == SWITCH_STATUS_SUCCESS, "switch_core_new_memory_pool()");
		fst_xcheck(switch_sockaddr_info_get(&current_addr, "192.0.2.10", SWITCH_UNSPEC, 42000, 0, pool) == SWITCH_STATUS_SUCCESS,
			"current address");
		fst_xcheck(switch_sockaddr_info_get(&candidate_addr, "192.0.2.20", SWITCH_UNSPEC, 52000, 0, pool) == SWITCH_STATUS_SUCCESS,
			"candidate address");

		fst_check(!switch_rtp_pvt_should_guard_recovery_dtls_tuple(current_addr, candidate_addr, DS_OFF,
			SWITCH_FALSE, SWITCH_TRUE, SWITCH_RTP_RECOVERY_NOMINATION_PERSISTED));
		fst_check(!switch_rtp_pvt_should_guard_recovery_dtls_tuple(current_addr, candidate_addr, DS_READY,
			SWITCH_FALSE, SWITCH_TRUE, SWITCH_RTP_RECOVERY_NOMINATION_PERSISTED));
		fst_check(!switch_rtp_pvt_should_guard_recovery_dtls_tuple(current_addr, candidate_addr, DS_HANDSHAKE,
			SWITCH_FALSE, SWITCH_FALSE, SWITCH_RTP_RECOVERY_NOMINATION_NONE));
		fst_check(!switch_rtp_pvt_should_guard_recovery_dtls_tuple(current_addr, candidate_addr, DS_HANDSHAKE,
			SWITCH_TRUE, SWITCH_TRUE, SWITCH_RTP_RECOVERY_NOMINATION_NONE));
		fst_check(!switch_rtp_pvt_should_guard_recovery_dtls_tuple(NULL, candidate_addr, DS_HANDSHAKE,
			SWITCH_FALSE, SWITCH_TRUE, SWITCH_RTP_RECOVERY_NOMINATION_NONE));
		fst_check(!switch_rtp_pvt_should_guard_recovery_dtls_tuple(current_addr, NULL, DS_HANDSHAKE,
			SWITCH_FALSE, SWITCH_TRUE, SWITCH_RTP_RECOVERY_NOMINATION_NONE));

		switch_core_destroy_memory_pool(&pool);
	}
	FST_TEST_END()
		FST_SESSION_BEGIN(test_stun_msg)
		{
				/* Binding Success Response */
				static uint8_t incoming[] = "\x01\x01\x00\x0c\x21\x12\xa4\x42\x53\x4f\x70\x43\x69\x69\x35\x4a\x66\x63\x31\x7a\x00\x01\x00\x08\x00\x01\x11\xfc\x0a\x0a\x0a\x2e";
				switch_core_session_t *session = NULL;
				switch_channel_t *channel = NULL;
				switch_status_t status;
				switch_call_cause_t cause;
				switch_rtp_ice_t ice = { 0 };
				switch_memory_pool_t *pool = NULL;
				static switch_rtp_t *rtp_session = NULL;
				static switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID] = {0};
				const char *err = NULL;

				switch_core_new_memory_pool(&pool);

				status = switch_ivr_originate(NULL, &session, &cause, "null/+15553334444", 2, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
				fst_check(session);
				fst_check(status == SWITCH_STATUS_SUCCESS);

				fsctl_debug(session);

				channel = switch_core_session_get_channel(session);
				fst_check(channel);

				switch_core_memory_pool_set_data(pool, "__session", session);
				session = switch_core_memory_pool_get_data(pool, "__session");
				fst_check(session);
				rtp_session = switch_rtp_new(rx_host, rx_port, tx_host, tx_port, TEST_PT, 8000, 20 * 1000, flags, "soft", &err, pool);
				fst_xcheck(rtp_session != NULL, "switch_rtp_new()");
				fst_check(switch_rtp_ready(rtp_session));
				switch_rtp_activate_rtcp(rtp_session, 5, rx_port + 1, 0);
				switch_rtp_set_default_payload(rtp_session, TEST_PT);
				switch_core_media_set_rtp_session(session, SWITCH_MEDIA_TYPE_AUDIO, rtp_session);

				ice.user_ice = "user_ice";
				ice.ice_user = "ice_user";

				ice.type = ICE_VANILLA;
				ice.addr = switch_rtp_session_get_remote_addr(rtp_session);
				switch_rtp_pvt_handle_ice(rtp_session, &ice, incoming, sizeof(incoming));

				fst_check(ice.rready);
				switch_rtp_destroy(&rtp_session);
				switch_core_session_rwunlock(session);
				switch_core_destroy_memory_pool(&pool);
		}
		FST_SESSION_END()
		FST_SESSION_BEGIN(test_stun_binding_request)
		{
			/* Binding Request */
			static uint8_t incoming[] = "\x00\x01\x00\x74\x21\x12\xa4\x42\x86\x39\xf9\xf7\x2d\xaa\x83\x0e"
										 "\x13\x8a\x1c\x90\x80\x22\x00\x1c\x72\x74\x70\x65\x6e\x67\x69\x6e"
										 "\x65\x2d\x31\x31\x2d\x33\x2d\x31\x2d\x39\x2d\x31\x2d\x62\x70\x6f"
										 "\x31\x30\x2d\x31\x00\x06\x00\x19\x53\x4f\x38\x76\x72\x5a\x35\x6e"
										 "\x72\x31\x77\x72\x4b\x75\x64\x56\x3a\x46\x56\x75\x4a\x79\x70\x74"
										 "\x52\x00\x00\x00\x80\x2a\x00\x08\x0b\xc8\x6e\xb5\xce\x88\xb3\x50"
										 "\x00\x24\x00\x04\x6e\xff\xff\xff\00\x08\x00\x14\xb9\xed\x8c\xc7"
										 "\x88\xa6\xd0\x62\xf8\xf4\xfe\x17\x6e\xa0\xd2\x13\xd7\x84\xde\x0c"
										 "\x80\x28\x00\x04\xd2\xea\x8f\x63";
			static uint8_t id[] = "\x86\x39\xf9\xf7\x2d\xaa\x83\x0e\x13\x8a\x1c\x90";
			switch_core_session_t *session = NULL;
			switch_channel_t *channel = NULL;
			switch_status_t status;
			switch_call_cause_t cause;
			switch_rtp_ice_t ice = { 0 };
			switch_memory_pool_t *pool = NULL;
			static switch_rtp_t *rtp_session = NULL;
			static switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID] = { 0 };
			const char *err = NULL;
			ice_t ice_params = { 0 };

			switch_core_new_memory_pool(&pool);

			status = switch_ivr_originate(NULL, &session, &cause, "null/+15553334444", 2, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
			fst_check(session);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			fsctl_debug(session);

			channel = switch_core_session_get_channel(session);
			fst_check(channel);

			switch_core_memory_pool_set_data(pool, "__session", session);
			session = switch_core_memory_pool_get_data(pool, "__session");
			fst_check(session);
			rtp_session = switch_rtp_new(rx_host, rx_port, tx_host, tx_port, TEST_PT, 8000, 20 * 1000, flags, "soft", &err, pool);
			fst_xcheck(rtp_session != NULL, "switch_rtp_new()");
			fst_check(switch_rtp_ready(rtp_session));
			switch_rtp_activate_rtcp(rtp_session, 5, rx_port + 1, 0);
			switch_rtp_set_default_payload(rtp_session, TEST_PT);
			switch_core_media_set_rtp_session(session, SWITCH_MEDIA_TYPE_AUDIO, rtp_session);

			ice.ice_params = &ice_params;

			ice.user_ice = "user_ice";
			ice.ice_user = "ice_user";
			ice.pass = "pass";
			ice.type = ICE_VANILLA;
			ice.addr = switch_rtp_session_get_remote_addr(rtp_session);
			ice.proto = 0;
			ice.ice_params->chosen[ice.proto] = 0;
			ice.ice_params->cand_idx[ice.proto] = 1;
			ice.ice_params->cands[0][ice.proto].con_addr = (char *) tx_host;
			ice.ice_params->cands[0][ice.proto].con_port = tx_port;
			ice.ice_params->cands[0][ice.proto].priority = 197684917;
			ice.ice_params->cands[0][ice.proto].cand_type = NULL;

			memcpy(ice.last_sent_id, id, 12);

			switch_rtp_pvt_handle_ice(rtp_session, &ice, incoming, sizeof(incoming));

			fst_check(ice.ready);

			switch_rtp_destroy(&rtp_session);
			switch_core_session_rwunlock(session);
			switch_core_destroy_memory_pool(&pool);
		}
		FST_SESSION_END()
		FST_SESSION_BEGIN(test_provisional_ice_requires_explicit_bootstrap)
		{
			switch_core_session_t *session = NULL;
			switch_channel_t *channel = NULL;
			switch_status_t status;
			switch_call_cause_t cause;
			switch_rtp_ice_t ice = { 0 };
			switch_rtp_ice_t timeout_ice = { 0 };
			switch_memory_pool_t *pool = NULL;
			static switch_rtp_t *rtp_session = NULL;
			static switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID] = { 0 };
			const char *err = NULL;
			ice_t ice_params = { 0 };
			ice_t timeout_params = { 0 };
			uint8_t tampered[128];
			uint8_t missing_username[128];
			uint8_t wrong_username[128];
			uint8_t successful_request[128];
			uint8_t controlling_request[128];
			uint8_t nomination_response[128];
			char bootstrap_host[80] = { 0 };
			char learned_host[80] = { 0 };
			char remote_host[80] = { 0 };
			char wrong_nomination_id[13] = "wrong-id-123";
			switch_size_t missing_username_len;
			switch_size_t wrong_username_len;
			switch_size_t successful_request_len;
			switch_size_t no_use_candidate_len;
			switch_size_t controlling_request_len;
			switch_size_t nomination_response_len;
			switch_sockaddr_t *remote_addr;
			int initial_cand_idx;
			int initial_chosen;
			int reused_idx;

			switch_core_new_memory_pool(&pool);
			status = switch_ivr_originate(NULL, &session, &cause, "null/+15553334444", 2, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
			fst_check(session);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			channel = switch_core_session_get_channel(session);
			fst_check(channel);
			switch_channel_set_variable(channel, "rtp_ice_prflx_bootstrap", "true");
			switch_channel_set_variable(channel, "rtp_ice_prflx_bootstrap_ms", "5000");

			switch_core_memory_pool_set_data(pool, "__session", session);
			fst_xcheck(switch_find_local_ip(bootstrap_host, sizeof(bootstrap_host), NULL, AF_INET) == SWITCH_STATUS_SUCCESS, "switch_find_local_ip()");
			fst_xcheck(strncmp(bootstrap_host, "127.", 4), "non-loopback bootstrap host");
			flags[SWITCH_RTP_FLAG_RTCP_MUX] = 1;
			rtp_session = switch_rtp_new(bootstrap_host, rx_port, tx_host, tx_port, TEST_PT, 8000, 20 * 1000, flags, "soft", &err, pool);
			fst_xcheck(rtp_session != NULL, "switch_rtp_new()");
			fst_check(switch_rtp_ready(rtp_session));
			switch_core_media_set_rtp_session(session, SWITCH_MEDIA_TYPE_AUDIO, rtp_session);

			ice.ice_params = &ice_params;
			ice.user_ice = "remoteUfrag:localUfrag";
			ice.ice_user = "localUfrag:remoteUfrag";
			ice.pass = "test-local-ice-password";
			ice.rpass = "test-remote-ice-password";
			ice.type = ICE_VANILLA | ICE_CONTROLLED;
			ice.proto = IPR_RTP;
			ice.prflx_bootstrap_require_use_candidate = 1;
			ice.initializing = 1;
			ice.ice_params->chosen[ice.proto] = 0;
			ice.ice_params->cand_idx[ice.proto] = 1;
			ice.ice_params->cands[0][ice.proto].priority = 0x6e0001ff;

			ice.missed_count = 7;
			initial_cand_idx = ice.ice_params->cand_idx[ice.proto];
			initial_chosen = ice.ice_params->chosen[ice.proto];
			missing_username_len = build_authenticated_request(missing_username, sizeof(missing_username), NULL, ice.pass, SWITCH_TRUE);
			fst_check(switch_stun_packet_validate_auth(missing_username, (uint32_t)missing_username_len, ice.pass));
			switch_rtp_pvt_handle_ice(rtp_session, &ice, missing_username, missing_username_len);
			fst_check(!ice.ready);
			fst_check(!ice.rready);
			fst_check(!ice.cand_responsive);
			fst_check(ice.addr == NULL);
			fst_check(ice.ice_params->cand_idx[ice.proto] == initial_cand_idx);
			fst_check(ice.ice_params->chosen[ice.proto] == initial_chosen);
			fst_check(ice.missed_count == 7);

			wrong_username_len = build_authenticated_request(wrong_username, sizeof(wrong_username), "wrong:username", ice.pass, SWITCH_TRUE);
			fst_check(switch_stun_packet_validate_auth(wrong_username, (uint32_t)wrong_username_len, ice.pass));
			switch_rtp_pvt_handle_ice(rtp_session, &ice, wrong_username, wrong_username_len);
			fst_check(!ice.ready);
			fst_check(!ice.rready);
			fst_check(!ice.cand_responsive);
			fst_check(ice.addr == NULL);
			fst_check(ice.ice_params->cand_idx[ice.proto] == initial_cand_idx);
			fst_check(ice.ice_params->chosen[ice.proto] == initial_chosen);
			fst_check(ice.missed_count == 7);

			successful_request_len = build_authenticated_request(successful_request, sizeof(successful_request), ice.user_ice, ice.pass, SWITCH_TRUE);
			memcpy(tampered, successful_request, successful_request_len);
			tampered[successful_request_len - 28] ^= 0x01;
			switch_rtp_pvt_handle_ice(rtp_session, &ice, tampered, successful_request_len);
			fst_check(ice.addr == NULL);
			fst_check(ice.ice_params->cand_idx[ice.proto] == initial_cand_idx);
			fst_check(ice.ice_params->chosen[ice.proto] == initial_chosen);
			fst_check(ice.missed_count == 7);

			memcpy(tampered, successful_request, successful_request_len);
			tampered[successful_request_len - 4] ^= 0x01;
			switch_rtp_pvt_handle_ice(rtp_session, &ice, tampered, successful_request_len);
			fst_check(ice.addr == NULL);
			fst_check(ice.ice_params->cand_idx[ice.proto] == initial_cand_idx);
			fst_check(ice.ice_params->chosen[ice.proto] == initial_chosen);
			fst_check(ice.missed_count == 7);

			no_use_candidate_len = build_authenticated_request(tampered, sizeof(tampered), ice.user_ice, ice.pass, SWITCH_FALSE);
			fst_check(switch_stun_packet_validate_auth(tampered, (uint32_t)no_use_candidate_len, ice.pass));
			switch_rtp_pvt_handle_ice(rtp_session, &ice, tampered, no_use_candidate_len);

			fst_check(!ice.ready);
			fst_check(!ice.rready);
			fst_check(!ice.cand_responsive);
			fst_check(ice.addr == NULL);
			fst_check(ice.ice_params->cand_idx[ice.proto] == initial_cand_idx);
			fst_check(ice.ice_params->chosen[ice.proto] == initial_chosen);
			fst_check(ice.missed_count == 7);

			fst_check(switch_stun_packet_validate_auth(successful_request, (uint32_t)successful_request_len, ice.pass));
			switch_rtp_pvt_handle_ice(rtp_session, &ice, successful_request, successful_request_len);
			fst_check(ice.ready);
			fst_check(ice.rready);
			fst_check(ice.cand_responsive);
			fst_check(ice.addr != NULL);
			fst_check(ice.ice_params->cand_idx[ice.proto] == 2);
			fst_check(ice.ice_params->chosen[ice.proto] == 1);
			fst_check(ice.missed_count == 0);
			if (ice.addr) {
				fst_check(!strcmp(switch_get_addr(learned_host, sizeof(learned_host), ice.addr), bootstrap_host));
				fst_check(switch_sockaddr_get_port(ice.addr) == rx_port);
			}
			remote_addr = switch_rtp_session_get_remote_addr(rtp_session);
			fst_check(remote_addr != NULL);
			if (remote_addr) {
				fst_check(!strcmp(switch_get_addr(remote_host, sizeof(remote_host), remote_addr), bootstrap_host));
				fst_check(switch_sockaddr_get_port(remote_addr) == rx_port);
			}
			if (ice.ice_params->cand_idx[ice.proto] == 2) {
				fst_check(ice.ice_params->cands[1][ice.proto].con_addr != NULL);
				if (ice.ice_params->cands[1][ice.proto].con_addr) {
					fst_check(!strcmp(ice.ice_params->cands[1][ice.proto].con_addr, bootstrap_host));
				}
				fst_check(ice.ice_params->cands[1][ice.proto].con_port == rx_port);
				fst_check(ice.ice_params->cands[1][ice.proto].cand_type != NULL);
				if (ice.ice_params->cands[1][ice.proto].cand_type) {
					fst_check(!strcmp(ice.ice_params->cands[1][ice.proto].cand_type, "prflx"));
				}
				fst_check(ice.ice_params->cands[1][ice.proto].responsive);
				fst_check(ice.ice_params->cands[1][ice.proto].ready);
				fst_check(ice.ice_params->cands[1][ice.proto].use_candidate);
			}

			memset(&ice, 0, sizeof(ice));
			memset(&ice_params, 0, sizeof(ice_params));
			ice.ice_params = &ice_params;
			ice.user_ice = "remoteUfrag:localUfrag";
			ice.ice_user = "localUfrag:remoteUfrag";
			ice.pass = "test-local-ice-password";
			ice.rpass = "test-remote-ice-password";
			ice.type = ICE_VANILLA;
			ice.proto = IPR_RTP;
			ice.prflx_bootstrap_require_use_candidate = 1;
			ice.initializing = 1;
			ice.controlling_failover_idx = -1;
			ice.prflx_bootstrap_idx = -1;
			ice.ice_params->chosen[ice.proto] = 0;
			ice.ice_params->cand_idx[ice.proto] = 1;
			ice.ice_params->cands[0][ice.proto].priority = 0x6e0001ff;

			no_use_candidate_len = build_authenticated_request(tampered, sizeof(tampered),
				ice.user_ice, ice.pass, SWITCH_FALSE);
			switch_rtp_pvt_handle_ice(rtp_session, &ice, tampered, no_use_candidate_len);
			fst_check(ice.ice_params->cand_idx[ice.proto] == 1);
			fst_check(ice.controlling_failover_state == SWITCH_RTP_ICE_CONTROLLING_FAILOVER_IDLE);

			no_use_candidate_len = build_authenticated_request_with_role(tampered, sizeof(tampered),
				ice.user_ice, ice.pass, SWITCH_FALSE, SWITCH_FALSE, SWITCH_TRUE);
			switch_rtp_pvt_handle_ice(rtp_session, &ice, tampered, no_use_candidate_len);
			fst_check(ice.ice_params->cand_idx[ice.proto] == 1);
			fst_check(ice.controlling_failover_state == SWITCH_RTP_ICE_CONTROLLING_FAILOVER_IDLE);

			controlling_request_len = build_authenticated_request_with_role(controlling_request,
				sizeof(controlling_request), ice.user_ice, ice.pass, SWITCH_FALSE,
				SWITCH_TRUE, SWITCH_FALSE);
			fst_check(switch_stun_packet_validate_auth(controlling_request,
				(uint32_t)controlling_request_len, ice.pass));
			switch_rtp_pvt_handle_ice(rtp_session, &ice, controlling_request, controlling_request_len);

			fst_check(!ice.ready);
			fst_check(!ice.rready);
			fst_check(!ice.cand_responsive);
			fst_check(ice.addr == NULL);
			fst_check(ice.ice_params->cand_idx[ice.proto] == 2);
			fst_check(ice.ice_params->chosen[ice.proto] == 0);
			fst_check(ice.controlling_failover_state == SWITCH_RTP_ICE_CONTROLLING_STARTUP_NOMINATING);
			fst_check(ice.controlling_failover_idx == 1);
			fst_check(ice.controlling_failover_nomination_id[0]);
			fst_check(!ice.ice_params->cands[1][ice.proto].use_candidate);
			fst_check(!ice.ice_params->cands[1][ice.proto].responsive);
			fst_check(!ice.ice_params->cands[1][ice.proto].ready);

			nomination_response_len = build_authenticated_response(nomination_response,
				sizeof(nomination_response), ice.controlling_failover_nomination_id, "wrong-password");
			switch_rtp_pvt_handle_ice(rtp_session, &ice, nomination_response, nomination_response_len);
			fst_check(!ice.ready);
			fst_check(ice.controlling_failover_state == SWITCH_RTP_ICE_CONTROLLING_STARTUP_NOMINATING);
			fst_check(!ice.ice_params->cands[1][ice.proto].responsive);
			fst_check(!ice.ice_params->cands[1][ice.proto].ready);

			nomination_response_len = build_authenticated_response(nomination_response,
				sizeof(nomination_response), wrong_nomination_id, ice.rpass);
			fst_check(switch_stun_packet_validate_auth(nomination_response,
				(uint32_t)nomination_response_len, ice.rpass));
			switch_rtp_pvt_handle_ice(rtp_session, &ice, nomination_response, nomination_response_len);
			fst_check(!ice.ready);
			fst_check(ice.controlling_failover_state == SWITCH_RTP_ICE_CONTROLLING_STARTUP_NOMINATING);
			fst_check(!ice.ice_params->cands[1][ice.proto].responsive);
			fst_check(!ice.ice_params->cands[1][ice.proto].ready);

			nomination_response_len = build_authenticated_response(nomination_response,
				sizeof(nomination_response), ice.controlling_failover_nomination_id, ice.rpass);
			fst_check(switch_stun_packet_validate_auth(nomination_response,
				(uint32_t)nomination_response_len, ice.rpass));
			switch_rtp_pvt_handle_ice(rtp_session, &ice, nomination_response, nomination_response_len);

			fst_check(ice.ready);
			fst_check(ice.rready);
			fst_check(ice.cand_responsive);
			fst_check(ice.addr != NULL);
			fst_check(ice.ice_params->chosen[ice.proto] == 1);
			fst_check(ice.ice_params->cands[1][ice.proto].use_candidate);
			fst_check(ice.ice_params->cands[1][ice.proto].responsive);
			fst_check(ice.ice_params->cands[1][ice.proto].ready);
			fst_check(ice.controlling_failover_state == SWITCH_RTP_ICE_CONTROLLING_FAILOVER_IDLE);
			fst_check(ice.controlling_failover_idx == -1);
			if (ice.addr) {
				fst_check(!strcmp(switch_get_addr(learned_host, sizeof(learned_host), ice.addr), bootstrap_host));
				fst_check(switch_sockaddr_get_port(ice.addr) == rx_port);
			}
			remote_addr = switch_rtp_session_get_remote_addr(rtp_session);
			fst_check(remote_addr != NULL);
			if (remote_addr) {
				fst_check(!strcmp(switch_get_addr(remote_host, sizeof(remote_host), remote_addr), bootstrap_host));
				fst_check(switch_sockaddr_get_port(remote_addr) == rx_port);
			}

			/* A startup nomination timeout leaves the uncommitted prflx slot reusable,
			 * so a changed authenticated NAT tuple can be nominated without growing
			 * or blocking the candidate list. */
			timeout_ice.ice_params = &timeout_params;
			timeout_ice.proto = IPR_RTP;
			timeout_ice.prflx_bootstrap_idx = 1;
			timeout_ice.controlling_failover_idx = -1;
			timeout_ice.controlling_failover_state = SWITCH_RTP_ICE_CONTROLLING_FAILOVER_IDLE;
			timeout_ice.ice_params->chosen[timeout_ice.proto] = 0;
			timeout_ice.ice_params->cand_idx[timeout_ice.proto] = 2;
			timeout_ice.ice_params->cands[1][timeout_ice.proto].cand_type = (char *)"prflx";
			timeout_ice.ice_params->cands[1][timeout_ice.proto].con_addr = (char *)"192.0.2.10";
			timeout_ice.ice_params->cands[1][timeout_ice.proto].con_port = 40000;
			reused_idx = switch_rtp_pvt_reuse_pending_startup_prflx_candidate(rtp_session,
				&timeout_ice, "192.0.2.20", 50000, 0x6e0001fe);
			fst_check(reused_idx == 1);
			fst_check(timeout_ice.ice_params->cand_idx[timeout_ice.proto] == 2);
			fst_check(!strcmp(timeout_ice.ice_params->cands[1][timeout_ice.proto].con_addr, "192.0.2.20"));
			fst_check(timeout_ice.ice_params->cands[1][timeout_ice.proto].con_port == 50000);
			fst_check(timeout_ice.ice_params->cands[1][timeout_ice.proto].priority == 0x6e0001fe);
			fst_check(!timeout_ice.ice_params->cands[1][timeout_ice.proto].responsive);
			fst_check(!timeout_ice.ice_params->cands[1][timeout_ice.proto].ready);
			fst_check(!timeout_ice.ice_params->cands[1][timeout_ice.proto].use_candidate);

			switch_rtp_destroy(&rtp_session);
			switch_core_session_rwunlock(session);
			switch_core_destroy_memory_pool(&pool);
		}
		FST_SESSION_END()
		FST_SESSION_BEGIN(test_reset_progress_with_control_only_reader)
		{
			switch_core_session_t *session = NULL;
			switch_memory_pool_t *pool;
			switch_call_cause_t cause;
			switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID] = { 0 };
			reset_reader_test_t ctx = { 0 };
			switch_thread_t *sender, *reader, *resetter;
			switch_threadattr_t *attr;
			switch_status_t thread_status;
			switch_rtcp_frame_t report;
			switch_time_t deadline;
			const char *err = NULL;
			int observed = 0, completed;
			void (*old_alarm_handler)(int);

			/* The watchdog also bounds cleanup if a regression strands a thread. */
			old_alarm_handler = signal(SIGALRM, SIG_DFL);
			alarm(10);
			fst_requires(switch_ivr_originate(NULL, &session, &cause, "null/+15553334444", 2,
				NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) == SWITCH_STATUS_SUCCESS);
			pool = switch_core_session_get_pool(session);
			switch_core_memory_pool_set_data(pool, "__session", session);
			flags[SWITCH_RTP_FLAG_RTCP_MUX] = 1;
			flags[SWITCH_RTP_FLAG_DATAWAIT] = 1;
			ctx.rtp = switch_rtp_new(rx_host, 26300, rx_host, 26302, TEST_PT, 160, 20000, flags, NULL, &err, pool);
			fst_requires(ctx.rtp != NULL);
			fst_requires(switch_rtp_activate_rtcp(ctx.rtp, 100, 26302, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_socket_create(&ctx.sender, AF_INET, SOCK_DGRAM, 0, pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_sockaddr_info_get(&ctx.target, rx_host, SWITCH_UNSPEC, 26300, 0, pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_threadattr_create(&attr, pool) == SWITCH_STATUS_SUCCESS);
			switch_atomic_set(&ctx.running, 1);
			fst_requires(switch_thread_create(&sender, attr, send_control_only, &ctx, pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_thread_create(&reader, attr, read_control_only, &ctx, pool) == SWITCH_STATUS_SUCCESS);
			deadline = switch_micro_time_now() + 1000000;
			do {
				memset(&report, 0, sizeof(report));
				if (switch_rtcp_zerocopy_read_frame(ctx.rtp, &report) == SWITCH_STATUS_SUCCESS && report.report_count && report.reports[0].ssrc == switch_rtp_get_ssrc(ctx.rtp)) observed = 1;
				if (!observed) switch_sleep(1000);
			} while (!observed && switch_micro_time_now() < deadline);
			fst_check(observed);
			fst_check(!switch_atomic_read(&ctx.reader_done));
			fst_requires(switch_thread_create(&resetter, attr, reset_during_control, &ctx, pool) == SWITCH_STATUS_SUCCESS);
			deadline = switch_micro_time_now() + 500000;
			while (!switch_atomic_read(&ctx.reset_done) && switch_micro_time_now() < deadline) switch_sleep(1000);
			completed = switch_atomic_read(&ctx.reset_done);
			switch_atomic_set(&ctx.running, 0);
			switch_thread_join(&thread_status, sender);
			switch_rtp_kill_socket(ctx.rtp);
			switch_thread_join(&thread_status, reader);
			switch_thread_join(&thread_status, resetter);
			fst_check(completed);
			switch_socket_close(ctx.sender);
			switch_rtp_destroy(&ctx.rtp);
			switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session);
			alarm(0);
			signal(SIGALRM, old_alarm_handler);
		}
		FST_SESSION_END()
		FST_SESSION_BEGIN(test_ready_dtls_restart_nominates_observed_tuple)
		{
			switch_core_session_t *sessions[2] = { NULL, NULL };
			switch_rtp_t *rtp[2] = { NULL, NULL };
			switch_memory_pool_t *pools[2] = { NULL, NULL };
			switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID] = { 0 };
			dtls_fingerprint_t fingerprints[2] = { { 0 }, { 0 } };
			switch_rtp_pvt_transport_snapshot_t before, after, prior;
			switch_call_cause_t cause;
			switch_channel_t *channel;
			switch_frame_t frame = { 0 };
			switch_rtcp_frame_t rtcp_frame;
			switch_sockaddr_t *source = NULL;
			ice_t params = { 0 };
			const char *err = NULL;
			char host[80] = "";
			char remote_ufrag[32], username[80];
			char transaction[13] = "";
			char stale_transaction[13] = "";
			char ordinary_transaction[13] = "";
			uint8_t packet[512], wire[512], payload[160];
			switch_size_t len, wire_len;
			switch_status_t rx_status;
			switch_stun_packet_t *check;
			switch_stun_packet_attribute_t *attr;
			switch_time_t deadline, retry_at;
			switch_port_t ports[2] = { 25000, 25002 };
			switch_port_t learned_port;
			int i, mode, bad, received, nominated;
			void (*old_alarm_handler)(int);

			/* Bound the entire test, including failures inside the RTP read path. */
			old_alarm_handler = signal(SIGALRM, SIG_DFL);
			alarm(60);
			fst_requires(switch_find_local_ip(host, sizeof(host), NULL, AF_INET) == SWITCH_STATUS_SUCCESS);
			fst_requires(strncmp(host, "127.", 4));
			flags[SWITCH_RTP_FLAG_RTCP_MUX] = 1;
			flags[SWITCH_RTP_FLAG_NOBLOCK] = 1;
			for (i = 0; i < 2; i++) {
				fst_requires(switch_ivr_originate(NULL, &sessions[i], &cause, "null/+15553334444", 2,
					NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) == SWITCH_STATUS_SUCCESS);
				pools[i] = switch_core_session_get_pool(sessions[i]);
				fst_requires(pools[i] != NULL);
				switch_core_memory_pool_set_data(pools[i], "__session", sessions[i]);
				rtp[i] = switch_rtp_new(rx_host, ports[i], rx_host, ports[1-i], TEST_PT, 160, 20000,
					flags, NULL, &err, pools[i]);
				fst_requires(rtp[i] != NULL);
				fst_requires(switch_socket_timeout_set(switch_rtp_get_rtp_socket(rtp[i]), 10000) == SWITCH_STATUS_SUCCESS);
				switch_core_media_set_rtp_session(sessions[i], SWITCH_MEDIA_TYPE_AUDIO, rtp[i]);
				switch_rtp_clear_flag(rtp[i], SWITCH_RTP_FLAG_PAUSE);
				switch_rtp_intentional_bugs(rtp[i], RTP_BUG_ACCEPT_ANY_PAYLOAD);
				fingerprints[i].type = "sha-256";
				fst_requires(switch_core_cert_gen_fingerprint(DTLS_SRTP_FNAME, &fingerprints[i]));
			}
			fst_requires(switch_rtp_add_dtls(rtp[0], &fingerprints[0], &fingerprints[1],
				DTLS_TYPE_RTP | DTLS_TYPE_CLIENT, 1) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_rtp_add_dtls(rtp[1], &fingerprints[1], &fingerprints[0],
				DTLS_TYPE_RTP | DTLS_TYPE_SERVER, 1) == SWITCH_STATUS_SUCCESS);
			/* A readable datagram drives the no-timer RTP receive path and starts
			 * the client flight; no fabricated DTLS state is used. */
			fst_requires(switch_sockaddr_info_get(&source, rx_host, SWITCH_UNSPEC, ports[0], 0, pools[0]) == SWITCH_STATUS_SUCCESS);
			memset(packet, 0, 13);
			packet[0] = 0x80;
			packet[1] = TEST_PT;
			len = 13;
			fst_requires(switch_socket_sendto(switch_rtp_get_rtp_socket(rtp[1]), source, 0, (char *)packet, &len) == SWITCH_STATUS_SUCCESS);
			deadline = switch_micro_time_now() + 5000000;
			do {
				for (i = 0; i < 2; i++) {
					if (switch_rtp_dtls_state(rtp[i], DTLS_TYPE_RTP) != DS_READY) {
						fst_requires(switch_sockaddr_info_get(&source, rx_host, SWITCH_UNSPEC, ports[i], 0, pools[0]) == SWITCH_STATUS_SUCCESS);
						len = 13;
						switch_socket_sendto(switch_rtp_get_rtp_socket(rtp[1-i]), source, 0, (char *)packet, &len);
					}
					memset(&frame, 0, sizeof(frame));
					switch_rtp_zerocopy_read_frame(rtp[i], &frame, SWITCH_IO_FLAG_NOBLOCK | SWITCH_IO_FLAG_BUNDLE_DRAIN);
				}
				if (switch_rtp_dtls_state(rtp[0], DTLS_TYPE_RTP) == DS_READY &&
					switch_rtp_dtls_state(rtp[1], DTLS_TYPE_RTP) == DS_READY) break;
				switch_sleep(1000);
			} while (switch_micro_time_now() < deadline);
			fst_requires(switch_rtp_dtls_state(rtp[0], DTLS_TYPE_RTP) == DS_READY);
			fst_requires(switch_rtp_dtls_state(rtp[1], DTLS_TYPE_RTP) == DS_READY);
			/* Wildcard binds accept both local-interface ICE and loopback DTLS traffic. */
			fst_requires(switch_rtp_set_local_address(rtp[0], "0.0.0.0", ports[0], &err) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_rtp_set_remote_address(rtp[0], host, ports[1], 0, SWITCH_FALSE, &err) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_rtp_activate_rtcp(rtp[0], 100, ports[1], SWITCH_TRUE) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp[0], IPR_RTP, &before) == SWITCH_STATUS_SUCCESS);
			channel = switch_core_session_get_channel(sessions[0]);
			switch_channel_set_variable(channel, "rtp_ice_prflx_bootstrap", "true");
			switch_channel_set_variable(channel, "rtp_ice_prflx_bootstrap_ms", "10000");
			params.cand_idx[IPR_RTP] = 1;
			params.cands[0][IPR_RTP].con_addr = host;
			params.cands[0][IPR_RTP].con_port = ports[1];
			params.cands[0][IPR_RTP].ready = 1;
			params.cands[0][IPR_RTP].cand_type = "srflx";
			fst_requires(switch_rtp_activate_ice(rtp[0], "remote0", "local0", "local-password0",
				"remote-password0", IPR_RTP, ICE_VANILLA, &params) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_sockaddr_info_get(&source, rx_host, SWITCH_UNSPEC, ports[0], 0, pools[0]) == SWITCH_STATUS_SUCCESS);

			/* Signaled+trickle, provisional+487, expired transaction, expired 487
			 * handoff+trickle, and successful provisional controlling restart. */
			for (mode = 0; mode < 6; mode++) {
				switch_snprintf(remote_ufrag, sizeof(remote_ufrag), "remote%d", mode + 1);
				switch_snprintf(username, sizeof(username), "local1:%s", remote_ufrag);
				fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp[0], IPR_RTP, &prior) == SWITCH_STATUS_SUCCESS);
				switch_channel_set_variable(channel, "rtp_ice_prflx_bootstrap_ms", mode == 3 ? "4000" : "10000");
				learned_port = 25004 + mode * 2;
				switch_rtp_clear_flag(rtp[1], SWITCH_RTP_FLAG_ENABLE_RTCP);
				fst_requires(switch_rtp_set_local_address(rtp[1], "0.0.0.0", learned_port, &err) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_rtp_set_remote_address(rtp[1], host, ports[0], 0, SWITCH_FALSE, &err) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_rtp_activate_rtcp(rtp[1], 100, ports[0], SWITCH_TRUE) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_socket_timeout_set(switch_rtp_get_rtp_socket(rtp[1]), 10000) == SWITCH_STATUS_SUCCESS);
				memset(&params, 0, sizeof(params));
				params.cand_idx[IPR_RTP] = 1;
				params.cands[0][IPR_RTP].con_addr = host;
				params.cands[0][IPR_RTP].con_port = 25100;
				params.cands[0][IPR_RTP].ready = (mode == 1 || mode == 3 || mode == 4) ? 0 : 1;
				params.cands[0][IPR_RTP].cand_type = "srflx";
				switch_rtp_prepare_ice_restart(rtp[0], IPR_RTP, SWITCH_TRUE);
				fst_requires(switch_rtp_activate_ice(rtp[0], remote_ufrag, "local1", "local-password1",
					"remote-password1", IPR_RTP, ICE_VANILLA, &params) == SWITCH_STATUS_SUCCESS);

				for (bad = 0; bad < 6; bad++) {
					len = build_authenticated_request_with_role(packet, sizeof(packet),
						bad == 0 ? "local0:remote0" : username,
						bad == 1 ? "local-password0" : "local-password1", SWITCH_FALSE,
						bad == 4 ? SWITCH_FALSE : SWITCH_TRUE, bad == 4 ? SWITCH_TRUE : SWITCH_FALSE);
					if (bad == 2) packet[len - 12] ^= 1; /* Invalid MESSAGE-INTEGRITY. */
					if (bad == 3) packet[len - 1] ^= 1; /* Invalid FINGERPRINT. */
					if (bad == 5) len -= 32; /* Missing authentication attributes. */
					fst_requires(switch_rtp_pvt_handle_ice_from(rtp[0], IPR_RTP, host, learned_port, packet, len) == SWITCH_STATUS_SUCCESS);
					fst_check(params.cand_idx[IPR_RTP] == 1);
					fst_check(params.chosen[IPR_RTP] == 0);
				}
				len = build_authenticated_request_with_role(packet, sizeof(packet),
					username, "local-password1", SWITCH_FALSE, SWITCH_TRUE, SWITCH_FALSE);
				/* The peer must actually transmit on its new socket before awaiting
				 * UDP responses, as in a real ICE connectivity check. */
				fst_requires(switch_sockaddr_info_get(&source, host, SWITCH_UNSPEC, ports[0], 0, pools[0]) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_socket_sendto(switch_rtp_get_rtp_socket(rtp[1]), source, 0, (char *)packet, &len) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_rtp_pvt_handle_ice_from(rtp[0], IPR_RTP, host, learned_port, packet, len) == SWITCH_STATUS_SUCCESS);
				fst_requires(params.cand_idx[IPR_RTP] == 2);
				fst_check(params.chosen[IPR_RTP] == 0);
				fst_check(!params.cands[1][IPR_RTP].ready && !params.cands[1][IPR_RTP].use_candidate);
				fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp[0], IPR_RTP, &after) == SWITCH_STATUS_SUCCESS);
				fst_check(after.dtls_context == before.dtls_context && after.dtls_ssl == before.dtls_ssl);
				fst_check(after.dtls_remote_port == prior.dtls_remote_port);

				/* Inspect the actual outbound check, not an internal transaction stub. */
				fst_requires(switch_socket_timeout_set(switch_rtp_get_rtp_socket(rtp[1]), 10000) == SWITCH_STATUS_SUCCESS);
				nominated = 0;
				retry_at = switch_micro_time_now() + 200000;
				deadline = switch_micro_time_now() + 3000000;
				do {
					wire_len = sizeof(wire);
					rx_status = switch_socket_recvfrom(source, switch_rtp_get_rtp_socket(rtp[1]), 0, (char *)wire, &wire_len);
					if (rx_status == SWITCH_STATUS_SUCCESS &&
						wire_len >= 20 && wire[0] == 0 && wire[1] == 1) {
						fst_requires(switch_stun_packet_validate_auth(wire, (uint32_t)wire_len, "remote-password1"));
						check = switch_stun_packet_parse(wire, (uint32_t)wire_len);
						fst_requires(check != NULL);
						memcpy(transaction, check->header.id, 12);
						switch_stun_packet_first_attribute(check, attr);
						do { if (attr->type == SWITCH_STUN_ATTR_USE_CAND) nominated = 1; }
						while (switch_stun_packet_next_attribute(attr, (void *)(wire + wire_len)));
					}
					if (!nominated && switch_micro_time_now() >= retry_at) {
						len = build_authenticated_request_with_role(packet, sizeof(packet), username,
							"local-password1", SWITCH_FALSE, SWITCH_TRUE, SWITCH_FALSE);
						fst_requires(switch_sockaddr_info_get(&source, host, SWITCH_UNSPEC, ports[0], 0, pools[0]) == SWITCH_STATUS_SUCCESS);
						fst_requires(switch_socket_sendto(switch_rtp_get_rtp_socket(rtp[1]), source, 0, (char *)packet, &len) == SWITCH_STATUS_SUCCESS);
						fst_requires(switch_rtp_pvt_handle_ice_from(rtp[0], IPR_RTP, host, learned_port, packet, len) == SWITCH_STATUS_SUCCESS);
						retry_at = switch_micro_time_now() + 200000;
					}
					if (!nominated) switch_sleep(1000);
				} while (!nominated && switch_micro_time_now() < deadline);
				fst_requires(nominated);
				fst_requires(switch_rtp_pvt_ice_restart_pending(rtp[0]));
				switch_rtp_prepare_ice_restart(rtp[0], IPR_RTP, SWITCH_FALSE);
				fst_requires(switch_rtp_activate_ice(rtp[0], remote_ufrag, "local1", "local-password1",
					"remote-password1", IPR_RTP, ICE_VANILLA, &params) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_rtp_pvt_ice_restart_pending(rtp[0]));

				for (bad = 0; bad < 3; bad++) {
					len = build_authenticated_response(packet, sizeof(packet), bad == 0 ? "wrong-id-123" : transaction,
						bad == 1 ? "wrong-password" : "remote-password1");
					fst_requires(switch_rtp_pvt_handle_ice_from(rtp[0], IPR_RTP, host,
						bad == 2 ? learned_port + 20 : learned_port, packet, len) == SWITCH_STATUS_SUCCESS);
					fst_check(params.chosen[IPR_RTP] == 0);
				}
				if (mode) {
					len = build_authenticated_response(packet, sizeof(packet), stale_transaction, "remote-password1");
					fst_requires(switch_rtp_pvt_handle_ice_from(rtp[0], IPR_RTP, host, learned_port, packet, len) == SWITCH_STATUS_SUCCESS);
					fst_check(params.chosen[IPR_RTP] == 0);
				}
				if (mode == 1 || mode == 3) {
					len = build_authenticated_role_conflict(packet, transaction, "remote-password1");
					fst_requires(switch_stun_packet_validate_auth(packet, (uint32_t)len, "remote-password1"));
					fst_requires(switch_rtp_pvt_handle_ice_from(rtp[0], IPR_RTP, host, learned_port, packet, len) == SWITCH_STATUS_SUCCESS);
					/* Duplicate error cannot toggle the role back or select the candidate. */
					fst_requires(switch_rtp_pvt_handle_ice_from(rtp[0], IPR_RTP, host, learned_port, packet, len) == SWITCH_STATUS_SUCCESS);
					fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp[0], IPR_RTP, &after) == SWITCH_STATUS_SUCCESS);
					fst_requires(after.ice_type & ICE_CONTROLLED);
					fst_check(!after.ice_ready && params.chosen[IPR_RTP] == 0);
					len = build_authenticated_response(packet, sizeof(packet), transaction, "remote-password1");
					fst_requires(switch_rtp_pvt_handle_ice_from(rtp[0], IPR_RTP, host, learned_port, packet, len) == SWITCH_STATUS_SUCCESS);
					fst_check(params.chosen[IPR_RTP] == 0);
					/* Same credentials must preserve the negotiated role too. */
					switch_rtp_prepare_ice_restart(rtp[0], IPR_RTP, SWITCH_TRUE);
					fst_requires(switch_rtp_activate_ice(rtp[0], remote_ufrag, "local1", "local-password1",
						"remote-password1", IPR_RTP, ICE_VANILLA, &params) == SWITCH_STATUS_SUCCESS);
				}
				if (mode == 1) {
					/* A regular check must succeed without nominating or moving media. */
					len = build_authenticated_request_with_role(packet, sizeof(packet), username,
						"local-password1", SWITCH_FALSE, SWITCH_FALSE, SWITCH_TRUE);
					memcpy(ordinary_transaction, ((switch_stun_packet_t *)packet)->header.id, 12);
					fst_requires(switch_rtp_pvt_handle_ice_from(rtp[0], IPR_RTP, host, learned_port, packet, len) == SWITCH_STATUS_SUCCESS);
					received = 0;
					deadline = switch_micro_time_now() + 1000000;
					do {
						wire_len = sizeof(wire);
						rx_status = switch_socket_recvfrom(source, switch_rtp_get_rtp_socket(rtp[1]), 0, (char *)wire, &wire_len);
						if (rx_status == SWITCH_STATUS_SUCCESS && wire_len >= 20 && wire[0] == 1 && wire[1] == 1 &&
							!memcmp(((switch_stun_packet_t *)wire)->header.id, ordinary_transaction, 12)) {
							fst_requires(switch_stun_packet_validate_auth(wire, (uint32_t)wire_len, "local-password1"));
							received = 1;
						}
					} while (!received && switch_micro_time_now() < deadline);
					fst_requires(received);
					fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp[0], IPR_RTP, &after) == SWITCH_STATUS_SUCCESS);
					fst_check(!after.ice_ready && params.chosen[IPR_RTP] == 0 && !params.cands[1][IPR_RTP].use_candidate);
					fst_check(after.dtls_remote_port == prior.dtls_remote_port);
				}
				if (mode == 5) {
					/* Reset must invalidate the pending response before the next generation exists. */
					fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp[0], IPR_RTP, &after) == SWITCH_STATUS_SUCCESS);
					fst_requires(after.nomination_state == SWITCH_RTP_ICE_CONTROLLING_RESTART_NOMINATING);
					switch_rtp_pvt_reset_ice(rtp[0], &params);
					fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp[0], IPR_RTP, &after) == SWITCH_STATUS_SUCCESS);
					fst_check(after.nomination_state == SWITCH_RTP_ICE_CONTROLLING_FAILOVER_IDLE);
					fst_check(params.cand_idx[IPR_RTP] == 0);
					len = build_authenticated_response(packet, sizeof(packet), transaction, "remote-password1");
					fst_requires(switch_rtp_pvt_handle_ice_from(rtp[0], IPR_RTP, host, learned_port, packet, len) == SWITCH_STATUS_SUCCESS);
					fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp[0], IPR_RTP, &after) == SWITCH_STATUS_SUCCESS);
					fst_check(!after.ice_ready && !after.ice_rready);
					fst_check(after.dtls_state == DS_READY && after.dtls_ssl == before.dtls_ssl);
					fst_check(after.dtls_remote_port == prior.dtls_remote_port);
					fst_check(switch_sockaddr_get_port(switch_rtp_session_get_remote_addr(rtp[0])) == prior.dtls_remote_port);
					continue;
				}
				if (mode == 2 || mode == 3) {
					/* A late but otherwise authentic success cannot revive an expired check. */
					switch_sleep(mode == 2 ? 5100000 : 4100000);
					switch_rtp_prepare_ice_restart(rtp[0], IPR_RTP, SWITCH_FALSE);
					fst_requires(switch_rtp_activate_ice(rtp[0], remote_ufrag, "local1", "local-password1",
						"remote-password1", IPR_RTP, ICE_VANILLA, &params) == SWITCH_STATUS_SUCCESS);
					len = build_authenticated_response(packet, sizeof(packet), transaction, "remote-password1");
					fst_requires(switch_rtp_pvt_handle_ice_from(rtp[0], IPR_RTP, host, learned_port, packet, len) == SWITCH_STATUS_SUCCESS);
					fst_check(params.chosen[IPR_RTP] == 0 && !params.cands[1][IPR_RTP].use_candidate);
					fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp[0], IPR_RTP, &after) == SWITCH_STATUS_SUCCESS);
					fst_check(after.dtls_state == DS_READY && after.dtls_context == before.dtls_context && after.dtls_ssl == before.dtls_ssl);
					fst_check(after.dtls_remote_port == prior.dtls_remote_port);
					fst_check(switch_sockaddr_get_port(switch_rtp_session_get_remote_addr(rtp[0])) == prior.dtls_remote_port);
					if (mode == 3) {
						len = build_authenticated_request_with_role(packet, sizeof(packet), username,
							"local-password1", SWITCH_TRUE, SWITCH_FALSE, SWITCH_TRUE);
						fst_requires(switch_rtp_pvt_handle_ice_from(rtp[0], IPR_RTP, host, learned_port, packet, len) == SWITCH_STATUS_SUCCESS);
						fst_check(params.chosen[IPR_RTP] == 0 && !params.cands[1][IPR_RTP].use_candidate);
						fst_requires(switch_rtp_pvt_ice_restart_pending(rtp[0]));
					}
					continue;
				}
				memcpy(stale_transaction, transaction, sizeof(transaction));
				len = mode == 1 ? build_authenticated_request_with_role(packet, sizeof(packet), username,
					"local-password1", SWITCH_TRUE, SWITCH_FALSE, SWITCH_TRUE) :
					build_authenticated_response(packet, sizeof(packet), transaction, "remote-password1");
				fst_requires(switch_rtp_pvt_handle_ice_from(rtp[0], IPR_RTP, host, learned_port, packet, len) == SWITCH_STATUS_SUCCESS);
				fst_requires(params.chosen[IPR_RTP] == 1);
				fst_check(params.cands[1][IPR_RTP].use_candidate);
				fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp[0], IPR_RTP, &after) == SWITCH_STATUS_SUCCESS);
				fst_check(after.dtls_state == DS_READY && after.ice_ready && after.ice_rready);
				fst_check(after.dtls_context == before.dtls_context && after.dtls_ssl == before.dtls_ssl && after.socket == before.socket);
				fst_check(after.ice_remote_port == learned_port && after.dtls_remote_port == learned_port);
				fst_check(switch_sockaddr_get_port(switch_rtp_session_get_remote_addr(rtp[0])) == learned_port);

				fst_check(switch_rtcp_get_remote_port(rtp[0]) == learned_port);
				/* Successful decryption both ways proves the established SRTP keys survived. */
				for (i = 0; i < 2; i++) {
					memset(payload, 0x51 + i + mode * 2, sizeof(payload));
					received = 0;
					deadline = switch_micro_time_now() + 1000000;
					do {
						memset(&frame, 0, sizeof(frame));
						frame.data = payload;
						frame.datalen = sizeof(payload);
						frame.buflen = sizeof(payload);
						frame.samples = 160;
						frame.payload = TEST_PT;
						fst_requires(switch_rtp_write_frame(rtp[i], &frame) > 0);
						memset(&frame, 0, sizeof(frame));
						rx_status = switch_rtp_zerocopy_read_frame(rtp[1-i], &frame, SWITCH_IO_FLAG_NOBLOCK | SWITCH_IO_FLAG_BUNDLE_DRAIN);
						if (rx_status == SWITCH_STATUS_SUCCESS &&
							!switch_test_flag(&frame, SFF_CNG) && frame.datalen == sizeof(payload) && !memcmp(frame.data, payload, sizeof(payload))) received = 1;
						if (!received) switch_sleep(20000);
					} while (!received && switch_micro_time_now() < deadline);
					fst_requires(received);
				}
				/* A decrypted SR/RR report block on the RTP socket proves mux RTCP output follows
				 * the nominated tuple, using the original SRTP association. */
				received = 0;
				deadline = switch_micro_time_now() + 4500000;
				do {
					switch_rtp_ping(rtp[0]);
					memset(&frame, 0, sizeof(frame));
					switch_rtp_zerocopy_read_frame(rtp[1], &frame, SWITCH_IO_FLAG_NOBLOCK | SWITCH_IO_FLAG_BUNDLE_DRAIN);
					memset(&rtcp_frame, 0, sizeof(rtcp_frame));
					if (switch_rtcp_zerocopy_read_frame(rtp[1], &rtcp_frame) == SWITCH_STATUS_SUCCESS &&
						rtcp_frame.report_count > 0 && rtcp_frame.reports[0].ssrc == switch_rtp_get_ssrc(rtp[1])) received = 1;
					if (!received) switch_sleep(10000);
				} while (!received && switch_micro_time_now() < deadline);
				fst_requires(received);
			}
#ifdef SWITCH_STUN_HAVE_SOFIA
			for (mode = 0; mode < 2; mode++) {
				switch_media_handle_t *smh;
				switch_core_media_params_t *mparams;
				sdp_parser_t *parser;
				char *sdp, *candidate, *recheck_sdp, *colon;
				char ice_user[256], local_pwd[256], remote_pwd[256];
				switch_bool_t has_addr;
				uint8_t proceed = 0;

				/* Use the real engine and recheck caller so candidate insertion cannot bypass nomination. */
				mparams = switch_core_session_alloc(sessions[0], sizeof(*mparams));
				mparams->inbound_codec_string = mparams->outbound_codec_string = "PCMA";
				mparams->rtpip = mparams->rtpip4 = host;
				fst_requires(switch_media_handle_create(&smh, sessions[0], mparams) == SWITCH_STATUS_SUCCESS);
				switch_channel_set_variable(channel, "absolute_codec_string", "PCMA");
				switch_channel_set_variable(channel, "rtp_trickle_ice", "true");
				switch_channel_set_variable(channel, "rtp_ice_role", "controlling");
				switch_channel_set_variable(channel, "apply-candidate-acl", "any_v4.auto");
				switch_channel_set_variable(channel, "rtp_ice_prflx_bootstrap_ms", mode ? "1000" : "10000");
				fst_requires(switch_core_media_prepare_codecs(sessions[0], SWITCH_TRUE) == SWITCH_STATUS_SUCCESS);
				switch_core_media_set_rtp_session(sessions[0], SWITCH_MEDIA_TYPE_AUDIO, rtp[0]);
				fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp[0], IPR_RTP, &prior) == SWITCH_STATUS_SUCCESS);
				switch_core_media_clear_ice(sessions[0]);
				switch_channel_set_flag(channel, CF_REINVITE);
				switch_channel_set_flag(channel, CF_RECOVERING);
				sdp = switch_core_session_sprintf(sessions[0],
					"v=0\no=- 1 2 IN IP4 %s\ns=-\nt=0 0\n"
					"m=audio 25002 UDP/TLS/RTP/SAVPF 8\nc=IN IP4 %s\n"
					"a=ice-ufrag:callerRestart\na=ice-pwd:callerRemotePassword123456\n"
					"a=ice-options:trickle\na=rtcp-mux\na=setup:active\na=rtpmap:8 PCMA/8000\n"
					"a=mid:0\na=sendrecv\n", host, rx_host);
				if (!mode) sdp = switch_core_session_sprintf(sessions[0],
					"%sa=candidate:advertised 1 udp 2000000000 198.51.100.20 25100 typ srflx\n", sdp);
				fst_requires(switch_core_media_negotiate_sdp(sessions[0], sdp, &proceed, SDP_OFFER));
				fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp[0], IPR_RTP, &after) == SWITCH_STATUS_SUCCESS);
				fst_check(after.dtls_remote_port == prior.dtls_remote_port);
				fst_check(switch_sockaddr_get_port(switch_rtp_session_get_remote_addr(rtp[0])) == prior.dtls_remote_port);
				fst_requires(switch_core_media_activate_rtp(sessions[0]) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_rtp_pvt_ice_restart_pending(rtp[0]));
				fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp[0], IPR_RTP, &after) == SWITCH_STATUS_SUCCESS);
				fst_check(after.dtls_remote_port == prior.dtls_remote_port);
				fst_check(switch_sockaddr_get_port(switch_rtp_session_get_remote_addr(rtp[0])) == prior.dtls_remote_port);
				switch_channel_clear_flag(channel, CF_REINVITE);
				switch_channel_clear_flag(channel, CF_RECOVERING);
				fst_requires(switch_rtp_pvt_get_ice_state(rtp[0], IPR_RTP, ice_user, sizeof(ice_user),
					local_pwd, sizeof(local_pwd), remote_pwd, sizeof(remote_pwd), &has_addr) == SWITCH_STATUS_SUCCESS);
				colon = strchr(ice_user, ':');
				fst_requires(colon != NULL);
				*colon++ = '\0';
				switch_snprintf(username, sizeof(username), "%s:%s", colon, ice_user);
				if (!mode) {
					/* Advertised-candidate restart must nominate the observed port before restoring media. */
					len = build_authenticated_request_with_role(packet, sizeof(packet), username, local_pwd,
						SWITCH_FALSE, SWITCH_TRUE, SWITCH_FALSE);
					fst_requires(switch_sockaddr_info_get(&source, host, SWITCH_UNSPEC, ports[0], 0, pools[0]) == SWITCH_STATUS_SUCCESS);
					fst_requires(switch_socket_sendto(switch_rtp_get_rtp_socket(rtp[1]), source, 0, (char *)packet, &len) == SWITCH_STATUS_SUCCESS);
					fst_requires(switch_rtp_pvt_handle_ice_from(rtp[0], IPR_RTP, host, learned_port, packet, len) == SWITCH_STATUS_SUCCESS);
					nominated = 0;
					deadline = switch_micro_time_now() + 3000000;
					do {
						wire_len = sizeof(wire);
						rx_status = switch_socket_recvfrom(source, switch_rtp_get_rtp_socket(rtp[1]), 0, (char *)wire, &wire_len);
						if (rx_status == SWITCH_STATUS_SUCCESS && wire_len >= 20 && wire[0] == 0 && wire[1] == 1 &&
							switch_stun_packet_validate_auth(wire, (uint32_t)wire_len, remote_pwd)) {
							check = switch_stun_packet_parse(wire, (uint32_t)wire_len);
							fst_requires(check != NULL);
							memcpy(transaction, check->header.id, 12);
							switch_stun_packet_first_attribute(check, attr);
							do { if (attr->type == SWITCH_STUN_ATTR_USE_CAND) nominated = 1; }
							while (switch_stun_packet_next_attribute(attr, (void *)(wire + wire_len)));
						}
					} while (!nominated && switch_micro_time_now() < deadline);
					fst_requires(nominated);
					len = build_authenticated_response(packet, sizeof(packet), transaction, remote_pwd);
					fst_requires(switch_rtp_pvt_handle_ice_from(rtp[0], IPR_RTP, host, learned_port, packet, len) == SWITCH_STATUS_SUCCESS);
					fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp[0], IPR_RTP, &after) == SWITCH_STATUS_SUCCESS);
					fst_check(after.ice_ready && after.ice_rready && after.dtls_remote_port == learned_port);
					fst_check(after.dtls_ssl == prior.dtls_ssl && after.dtls_state == DS_READY);
					for (i = 0; i < 2; i++) {
						memset(payload, 0x70 + i, sizeof(payload));
						received = 0;
						deadline = switch_micro_time_now() + 1000000;
						do {
							memset(&frame, 0, sizeof(frame));
							frame.data = payload;
							frame.datalen = frame.buflen = sizeof(payload);
							frame.samples = 160;
							frame.payload = TEST_PT;
							fst_requires(switch_rtp_write_frame(rtp[i], &frame) > 0);
							memset(&frame, 0, sizeof(frame));
							rx_status = switch_rtp_zerocopy_read_frame(rtp[1-i], &frame, SWITCH_IO_FLAG_NOBLOCK | SWITCH_IO_FLAG_BUNDLE_DRAIN);
							if (rx_status == SWITCH_STATUS_SUCCESS && !switch_test_flag(&frame, SFF_CNG) &&
								frame.datalen == sizeof(payload) && !memcmp(frame.data, payload, sizeof(payload))) received = 1;
							if (!received) switch_sleep(20000);
						} while (!received && switch_micro_time_now() < deadline);
						fst_requires(received);
					}
					continue;
				}
				/* A separate RTP list exercises the caller's merge-and-reactivate path. */
				memset(&params, 0, sizeof(params));
				fst_requires(switch_rtp_activate_ice(rtp[0], ice_user, colon, local_pwd,
					remote_pwd, IPR_RTP, ICE_VANILLA, &params) == SWITCH_STATUS_SUCCESS);
				len = build_authenticated_request_with_role(packet, sizeof(packet), username, local_pwd,
					SWITCH_FALSE, SWITCH_TRUE, SWITCH_FALSE);
				fst_requires(switch_rtp_pvt_handle_ice_from(rtp[0], IPR_RTP, host, learned_port, packet, len) == SWITCH_STATUS_SUCCESS);
				fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp[0], IPR_RTP, &prior) == SWITCH_STATUS_SUCCESS);
				for (i = 0; i < 2; i++) {
					if (i) switch_sleep(1100000);
					candidate = switch_core_session_sprintf(sessions[0],
						"candidate:late%d 1 udp 2000000000 198.51.100.20 %u typ srflx", i, 25200 + i * 2);
					recheck_sdp = switch_core_session_sprintf(sessions[0], "%sa=%s\n", sdp, candidate);
					parser = sdp_parse(NULL, recheck_sdp, strlen(recheck_sdp), 0);
					fst_requires(parser != NULL && sdp_session(parser) != NULL);
					fst_requires(switch_core_media_trickle_remote_candidate_and_recheck(sessions[0], smh,
						sdp_session(parser), SDP_OFFER, "0", 0, candidate, 0) == SWITCH_STATUS_SUCCESS);
					fst_requires(switch_rtp_pvt_ice_restart_pending(rtp[0]));
					fst_requires(switch_rtp_pvt_get_transport_snapshot(rtp[0], IPR_RTP, &after) == SWITCH_STATUS_SUCCESS);
					fst_check(after.dtls_ssl == prior.dtls_ssl && after.dtls_state == DS_READY);
					fst_check(after.dtls_remote_port == prior.dtls_remote_port);
					fst_check(switch_sockaddr_get_port(switch_rtp_session_get_remote_addr(rtp[0])) == prior.dtls_remote_port);
					fst_check(!after.ice_ready && !after.ice_rready);
					sdp_parser_free(parser);
				}
			}
#endif
			for (i = 0; i < 2; i++) {
				switch_core_media_set_rtp_session(sessions[i], SWITCH_MEDIA_TYPE_AUDIO, NULL);
				switch_rtp_destroy(&rtp[i]);
				switch_channel_hangup(switch_core_session_get_channel(sessions[i]), SWITCH_CAUSE_NORMAL_CLEARING);
				switch_core_session_rwunlock(sessions[i]);
			}
			alarm(0);
			signal(SIGALRM, old_alarm_handler);
		}
		FST_SESSION_END()
}
FST_SUITE_END()
}
FST_CORE_END()
