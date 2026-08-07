/*
* FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
* Copyright (C) 2005-2026, Anthony Minessale II <anthm@freeswitch.org>
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
* Contributor(s):
* Dmitry Verenitsin <morbit85@gmail.com>
*
* switch_stun.c -- tests STUN (https://www.rfc-editor.org/rfc/rfc5389).
*/


#include <switch.h>
#include <switch_stun.h>
#include <test/switch_test.h>
#include <private/switch_rtp_pvt.h>

static const switch_payload_t TEST_PT = 8;
static const char *rx_host = "127.0.0.1";
static switch_port_t rx_port = 1234;
static const char *tx_host = "127.0.0.1";
static switch_port_t tx_port = 54320;

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

	FST_TEST_BEGIN(test_stun_add_binded_address_ipv6)
	{
		/*
		 * Encode an IPv6 XOR-MAPPED-ADDRESS attribute and verify the
		 * attribute type, length, address family, and the raw 16-byte
		 * address payload at its expected offset inside the value.
		 */
		uint8_t buf[512];
		switch_stun_packet_t *packet;
		switch_stun_packet_attribute_t *attr;
		const char *ipv6_str = "2001:db8::dead:beef";
		uint8_t expected[16];
		uint8_t *value_bytes;

		memset(buf, 0, sizeof(buf));
		packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_RESPONSE, NULL, buf);
		fst_xcheck(inet_pton(AF_INET6, ipv6_str, expected) == 1, "test IPv6 literal parses");

		switch_stun_packet_attribute_add_binded_address(packet, (char *)ipv6_str, 12345, AF_INET6);

		attr = (switch_stun_packet_attribute_t *)packet->first_attribute;
		fst_xcheck(ntohs(attr->type) == SWITCH_STUN_ATTR_XOR_MAPPED_ADDRESS, "attribute type is XOR_MAPPED_ADDRESS");
		fst_xcheck(ntohs(attr->length) == 20, "attribute length is 20 for IPv6");

		/* Attribute value layout: wasted(1) + family(1) + port(2) + address(16). */
		value_bytes = (uint8_t *)attr->value;
		fst_xcheck(value_bytes[1] == 2, "attribute family byte is 2 for IPv6");
		fst_xcheck(memcmp(value_bytes + 4, expected, 16) == 0, "16-byte IPv6 address written at offset 4 of attribute value");
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_stun_add_xor_binded_address_ipv6)
	{
		/*
		 * Encode then decode an IPv6 XOR-MAPPED-ADDRESS attribute and
		 * confirm the round-trip recovers the original IPv6 string —
		 * the write path must XOR the address with the transaction ID
		 * symmetrically to the read path.
		 */
		uint8_t buf[512];
		switch_stun_packet_t *packet;
		switch_stun_packet_attribute_t *attr;
		const char *ipv6_str = "2001:db8::dead:beef";
		char out_ip[64] = { 0 };
		uint16_t out_port = 0;

		memset(buf, 0, sizeof(buf));
		packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_RESPONSE, NULL, buf);

		switch_stun_packet_attribute_add_xor_binded_address(packet, (char *)ipv6_str, 12345, AF_INET6);

		attr = (switch_stun_packet_attribute_t *)packet->first_attribute;
		fst_xcheck(ntohs(attr->type) == SWITCH_STUN_ATTR_XOR_MAPPED_ADDRESS, "attribute type is XOR_MAPPED_ADDRESS");
		fst_xcheck(ntohs(attr->length) == 20, "attribute length is 20 for IPv6");

		switch_stun_packet_attribute_get_xor_mapped_address(attr, &packet->header, out_ip, sizeof(out_ip), &out_port);
		fst_check_string_equals(out_ip, ipv6_str);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_stun_add_binded_address_ipv4)
	{
		/*
		 * Encode an IPv4 XOR-MAPPED-ADDRESS attribute and verify the
		 * attribute type, length, address family, and the raw 4-byte
		 * address payload at its expected offset inside the value.
		 */
		uint8_t buf[512];
		switch_stun_packet_t *packet;
		switch_stun_packet_attribute_t *attr;
		const char *ipv4_str = "192.0.2.42";
		uint8_t expected[4];
		uint8_t *value_bytes;

		memset(buf, 0, sizeof(buf));
		packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_RESPONSE, NULL, buf);
		fst_xcheck(inet_pton(AF_INET, ipv4_str, expected) == 1, "test IPv4 literal parses");

		switch_stun_packet_attribute_add_binded_address(packet, (char *)ipv4_str, 12345, AF_INET);

		attr = (switch_stun_packet_attribute_t *)packet->first_attribute;
		fst_xcheck(ntohs(attr->type) == SWITCH_STUN_ATTR_XOR_MAPPED_ADDRESS, "attribute type is XOR_MAPPED_ADDRESS");
		fst_xcheck(ntohs(attr->length) == 8, "attribute length is 8 for IPv4");

		/* Attribute value layout: wasted(1) + family(1) + port(2) + address(4). */
		value_bytes = (uint8_t *)attr->value;
		fst_xcheck(value_bytes[1] == 1, "attribute family byte is 1 for IPv4");
		fst_xcheck(memcmp(value_bytes + 4, expected, 4) == 0, "4-byte IPv4 address written at offset 4 of attribute value");
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_stun_add_xor_binded_address_ipv4)
	{
		/*
		 * Encode then decode an IPv4 XOR-MAPPED-ADDRESS attribute and
		 * confirm the round-trip recovers the original IPv4 string —
		 * the write path must XOR the address with the magic cookie
		 * symmetrically to the read path.
		 */
		uint8_t buf[512];
		switch_stun_packet_t *packet;
		switch_stun_packet_attribute_t *attr;
		const char *ipv4_str = "192.0.2.42";
		char out_ip[64] = { 0 };
		uint16_t out_port = 0;

		memset(buf, 0, sizeof(buf));
		packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_RESPONSE, NULL, buf);

		switch_stun_packet_attribute_add_xor_binded_address(packet, (char *)ipv4_str, 12345, AF_INET);

		attr = (switch_stun_packet_attribute_t *)packet->first_attribute;
		fst_xcheck(ntohs(attr->type) == SWITCH_STUN_ATTR_XOR_MAPPED_ADDRESS, "attribute type is XOR_MAPPED_ADDRESS");
		fst_xcheck(ntohs(attr->length) == 8, "attribute length is 8 for IPv4");

		switch_stun_packet_attribute_get_xor_mapped_address(attr, &packet->header, out_ip, sizeof(out_ip), &out_port);
		fst_check_string_equals(out_ip, ipv4_str);
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
}
FST_SUITE_END()
}
FST_CORE_END()
