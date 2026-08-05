#include <switch.h>
#include <switch_stun.h>
#include <test/switch_test.h>
#include <private/switch_rtp_pvt.h>

static const switch_payload_t TEST_PT = 8;
static const char *rx_host = "127.0.0.1";
static switch_port_t rx_port = 1234;
static const char *tx_host = "127.0.0.1";
static switch_port_t tx_port = 54320;

static switch_size_t build_authenticated_request(uint8_t *buf, switch_size_t buflen, const char *username,
												 const char *password, switch_bool_t use_candidate)
{
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
	switch_stun_packet_attribute_add_integrity(packet, password);
	switch_stun_packet_attribute_add_fingerprint(packet);
	bytes = switch_stun_packet_length(packet);
	switch_assert(bytes <= buflen);

	return bytes;
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
			switch_memory_pool_t *pool = NULL;
			static switch_rtp_t *rtp_session = NULL;
			static switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID] = { 0 };
			const char *err = NULL;
			ice_t ice_params = { 0 };
			uint8_t tampered[128];
			uint8_t missing_username[128];
			uint8_t wrong_username[128];
			uint8_t successful_request[128];
			char bootstrap_host[80] = { 0 };
			char learned_host[80] = { 0 };
			char remote_host[80] = { 0 };
			switch_size_t missing_username_len;
			switch_size_t wrong_username_len;
			switch_size_t successful_request_len;
			switch_size_t no_use_candidate_len;
			switch_sockaddr_t *remote_addr;
			int initial_cand_idx;
			int initial_chosen;

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

			switch_rtp_destroy(&rtp_session);
			switch_core_session_rwunlock(session);
			switch_core_destroy_memory_pool(&pool);
		}
		FST_SESSION_END()
}
FST_SUITE_END()
}
FST_CORE_END()
