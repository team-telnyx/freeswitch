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

	FST_TEST_BEGIN(test_stun_get_xor_mapped_address_short_ipv6)
	{
		/*
		 * An XOR-MAPPED-ADDRESS attribute whose family byte claims IPv6 (2)
		 * but whose value is only the 8-byte IPv4 size must be rejected:
		 * switch_stun_packet_attribute_get_xor_mapped_address must not read
		 * or XOR a 20-byte IPv6 address out of the 8-byte value. The packet
		 * is routed through switch_stun_packet_parse first so the attribute
		 * length is in host byte order, as the live callers see it.
		 */
		uint8_t buf[512] = { 0 };
		switch_stun_packet_t *packet;
		switch_stun_packet_attribute_t *attr;
		char out_ip[64];
		uint16_t out_port = 0xffff;
		uint8_t ret;

		packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_RESPONSE, NULL, buf);

		/* Value layout: wasted(1) + family(1) + port(2) + address(4) = 8 bytes. */
		attr = (switch_stun_packet_attribute_t *)packet->first_attribute;
		attr->type = htons(SWITCH_STUN_ATTR_XOR_MAPPED_ADDRESS);
		attr->length = htons(8);
		attr->value[0] = 0;	/* wasted */
		attr->value[1] = 2;	/* family byte claims IPv6 */
		attr->value[2] = 0;
		attr->value[3] = 0;
		attr->value[4] = 0x01;
		attr->value[5] = 0x02;
		attr->value[6] = 0x03;
		attr->value[7] = 0x04;
		packet->header.length = htons(4 + 8);

		packet = switch_stun_packet_parse(buf, 20 + 4 + 8);
		fst_requires(packet != NULL);

		attr = (switch_stun_packet_attribute_t *)packet->first_attribute;
		fst_xcheck(attr->length == 8, "parse accepts the 8-byte family-2 XOR-MAPPED-ADDRESS attribute");

		memset(out_ip, 'x', sizeof(out_ip));
		ret = switch_stun_packet_attribute_get_xor_mapped_address(attr, &packet->header, out_ip, sizeof(out_ip), &out_port);
		fst_xcheck(ret == 0, "short IPv6 XOR-MAPPED-ADDRESS attribute is rejected");
		fst_xcheck(out_ip[0] == '\0', "output address string left empty on reject");
		fst_xcheck(out_port == 0, "output port left defined on reject");
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

	FST_TEST_BEGIN(test_stun_verify_integrity_accepts_valid_hmac)
	{
		/* A packet signed with add_integrity under a given key must verify against that same key,
		   including when a leading attribute precedes MESSAGE-INTEGRITY (it is part of the HMAC input). */
		uint8_t buf[512] = { 0 };
		char software[] = "sw";
		switch_stun_packet_t *packet;
		uint32_t len;

		packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_REQUEST, NULL, buf);
		switch_stun_packet_attribute_add_software(packet, software, (uint16_t)strlen(software));
		switch_stun_packet_attribute_add_integrity(packet, "secret");
		len = (uint32_t)switch_stun_packet_length(packet);

		fst_xcheck(switch_stun_packet_verify_integrity(buf, len, "secret") == SWITCH_STATUS_SUCCESS,
				   "valid MESSAGE-INTEGRITY verifies against the signing key");
		fst_xcheck(switch_stun_packet_verify_integrity(buf, len, "wrong") == SWITCH_STATUS_FALSE,
				   "MESSAGE-INTEGRITY does not verify against a different key");
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_stun_verify_integrity_rejects_zeroed_hmac)
	{
		/* An all-zero MESSAGE-INTEGRITY value must not verify: this is the shape a sender produces when
		   it fills the field with zeros instead of computing the HMAC. MI is the first attribute, so its
		   20-byte value sits at offset 24 (20-byte header plus 4-byte attribute header). */
		uint8_t buf[512] = { 0 };
		switch_stun_packet_t *packet;
		uint32_t len;

		packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_REQUEST, NULL, buf);
		switch_stun_packet_attribute_add_integrity(packet, "secret");
		len = (uint32_t)switch_stun_packet_length(packet);
		memset(buf + SWITCH_STUN_PACKET_MIN_LEN + 4, 0, 20);

		fst_xcheck(switch_stun_packet_verify_integrity(buf, len, "secret") == SWITCH_STATUS_FALSE,
				   "a zeroed MESSAGE-INTEGRITY value is rejected");
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_stun_verify_integrity_trailing_fingerprint)
	{
		/* MESSAGE-INTEGRITY followed by FINGERPRINT (the layout our own responses use) must still verify:
		   the HMAC input's length field reads as if the message ended right after MESSAGE-INTEGRITY, so the
		   trailing FINGERPRINT is excluded from the computation. */
		uint8_t buf[512] = { 0 };
		switch_stun_packet_t *packet;
		uint32_t len;

		packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_RESPONSE, NULL, buf);
		switch_stun_packet_attribute_add_integrity(packet, "secret");
		switch_stun_packet_attribute_add_fingerprint(packet);
		len = (uint32_t)switch_stun_packet_length(packet);

		fst_xcheck(switch_stun_packet_verify_integrity(buf, len, "secret") == SWITCH_STATUS_SUCCESS,
				   "MESSAGE-INTEGRITY verifies with a trailing FINGERPRINT present");
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_stun_verify_integrity_absent)
	{
		/* A packet with no MESSAGE-INTEGRITY attribute reports NOTFOUND, distinct from a mismatch, so the
		   caller can apply its own present-or-absent policy. */
		uint8_t buf[512] = { 0 };
		char software[] = "sw";
		switch_stun_packet_t *packet;
		uint32_t len;

		packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_REQUEST, NULL, buf);
		switch_stun_packet_attribute_add_software(packet, software, (uint16_t)strlen(software));
		len = (uint32_t)switch_stun_packet_length(packet);

		fst_xcheck(switch_stun_packet_verify_integrity(buf, len, "secret") == SWITCH_STATUS_NOTFOUND,
				   "a packet with no MESSAGE-INTEGRITY reports NOTFOUND");
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_stun_verify_integrity_rejects_attr_after_mi)
	{
		/* Only MESSAGE-INTEGRITY-SHA256 and FINGERPRINT may follow MESSAGE-INTEGRITY. A USE-CANDIDATE
		   appended after MI leaves the HMAC prefix - and therefore the signature - valid, so it must be
		   rejected outright rather than verified and then acted on. */
		uint8_t buf[512] = { 0 };
		switch_stun_packet_t *packet;
		uint32_t len;

		packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_REQUEST, NULL, buf);
		switch_stun_packet_attribute_add_integrity(packet, "secret");
		switch_stun_packet_attribute_add_use_candidate(packet);	/* lands after MESSAGE-INTEGRITY */
		len = (uint32_t)switch_stun_packet_length(packet);

		fst_xcheck(switch_stun_packet_verify_integrity(buf, len, "secret") == SWITCH_STATUS_FALSE,
				   "a non-FINGERPRINT attribute after MESSAGE-INTEGRITY is rejected");
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_stun_verify_integrity_oversized_attr_length)
	{
		/* An attribute length with the high bit set must not walk the cursor backward or read out of
		   bounds: the walk treats the padded length as unsigned and stops once it exceeds the bytes that
		   remain. With a leading oversized attribute, MESSAGE-INTEGRITY is never reached (NOTFOUND) and no
		   out-of-bounds access occurs (ASAN would catch a regression here). */
		uint8_t buf[64] = { 0 };
		switch_stun_packet_t *packet;
		switch_stun_packet_attribute_t *attr;
		uint32_t len;

		packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_REQUEST, NULL, buf);
		attr = (switch_stun_packet_attribute_t *)packet->first_attribute;
		attr->type = htons(SWITCH_STUN_ATTR_USERNAME);
		attr->length = htons(0x8000);
		packet->header.length = htons(4);	/* declare just the 4-byte attribute header */
		len = SWITCH_STUN_PACKET_MIN_LEN + 4;

		fst_xcheck(switch_stun_packet_verify_integrity(buf, len, "secret") == SWITCH_STATUS_NOTFOUND,
				   "an attribute length with the high bit set stops the walk without an out-of-bounds read");
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_stun_verify_integrity_allows_sha256_after_mi)
	{
		/* RFC 8489 permits MESSAGE-INTEGRITY-SHA256 to follow MESSAGE-INTEGRITY. It is inert (carries no
		   ICE state) and outside the SHA-1 HMAC's coverage, so a dual-hash sender's packet must still
		   verify on its SHA-1 MESSAGE-INTEGRITY rather than be rejected as a disallowed trailing attribute. */
		uint8_t buf[512] = { 0 };
		switch_stun_packet_t *packet;
		switch_stun_packet_attribute_t *sha256;
		uint32_t off;
		uint32_t len;

		packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_REQUEST, NULL, buf);
		switch_stun_packet_attribute_add_integrity(packet, "secret");

		/* Append a MESSAGE-INTEGRITY-SHA256 attribute (32-byte value, left zero: the SHA-1 verifier does
		   not inspect it) immediately after MESSAGE-INTEGRITY. */
		off = SWITCH_STUN_PACKET_MIN_LEN + ntohs(packet->header.length);
		sha256 = (switch_stun_packet_attribute_t *)(buf + off);
		sha256->type = htons(SWITCH_STUN_ATTR_MESSAGE_INTEGRITY_SHA256);
		sha256->length = htons(32);
		packet->header.length = htons((uint16_t)(ntohs(packet->header.length) + 4 + 32));
		len = (uint32_t)switch_stun_packet_length(packet);

		fst_xcheck(switch_stun_packet_verify_integrity(buf, len, "secret") == SWITCH_STATUS_SUCCESS,
				   "a MESSAGE-INTEGRITY-SHA256 attribute after MESSAGE-INTEGRITY is tolerated");
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_stun_get_username_terminates)
	{
		/* get_username must always NUL-terminate within the caller's buffer, even when the
		   attribute value is as long as or longer than the buffer: callers use the result as a C string. */
		uint8_t abuf[128] = { 0 };
		switch_stun_packet_attribute_t *attr = (switch_stun_packet_attribute_t *)abuf;
		char dst[32];
		char *ret;
		int i;

		attr->type = htons(SWITCH_STUN_ATTR_USERNAME);
		attr->length = 64;	/* host order: the accessor reads attribute->length directly, as post-parse callers do */
		for (i = 0; i < 64; i++) {
			attr->value[i] = 'A';
		}

		memset(dst, 'x', sizeof(dst));
		ret = switch_stun_packet_attribute_get_username(attr, dst, sizeof(dst));
		fst_xcheck(ret == dst, "get_username returns the destination buffer");
		fst_xcheck(dst[sizeof(dst) - 1] == '\0', "over-long USERNAME is NUL-terminated at the last byte");
		fst_xcheck(strlen(dst) == sizeof(dst) - 1, "over-long USERNAME is truncated to len-1");

		attr->length = 5;
		memcpy(attr->value, "abcde", 5);
		memset(dst, 'x', sizeof(dst));
		switch_stun_packet_attribute_get_username(attr, dst, sizeof(dst));
		fst_xcheck(strlen(dst) == 5, "short USERNAME is copied and terminated at its own length");
		fst_check_string_equals(dst, "abcde");
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_stun_next_attribute_walks_whole_attributes)
	{
		/* Positive test: the iterator visits each whole attribute and stops exactly at the end of the
		   last one, including an attribute whose value ends on the buffer boundary. Guards against a
		   future change breaking normal iteration; it does not distinguish the header/TLV bounds fix
		   (a naive iterator passes it too). */
		uint8_t buf[8] = { 0 };
		switch_stun_packet_attribute_t *attr;
		void *end = buf + sizeof(buf);

		attr = (switch_stun_packet_attribute_t *)buf;
		attr->type = htons(SWITCH_STUN_ATTR_USERNAME);
		attr->length = 0;
		attr = (switch_stun_packet_attribute_t *)(buf + 4);
		attr->type = htons(SWITCH_STUN_ATTR_PRIORITY);
		attr->length = 0;

		attr = (switch_stun_packet_attribute_t *)buf;
		fst_xcheck(switch_stun_packet_next_attribute(attr, end) != 0, "iterator advances to the second whole attribute");
		fst_xcheck((uint8_t *)attr == buf + 4, "iterator lands exactly on the second attribute");
		fst_xcheck(switch_stun_packet_next_attribute(attr, end) == 0, "iterator stops after the last whole attribute");
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_stun_next_attribute_truncated_header)
	{
		/* After the first attribute only 2 bytes remain before end, so the next attribute's 4-byte
		   header does not fit. The iterator must confirm the header is fully in-bounds before reading
		   it and stop; reading attribute->length here would run past the buffer. The trailing bytes are
		   non-zero so the type sentinel does not stop the walk first, so the out-of-bounds read (if the
		   header check is missing) is exercised and caught under ASAN. */
		uint8_t buf[6] = { 0 };
		switch_stun_packet_attribute_t *attr = (switch_stun_packet_attribute_t *)buf;
		void *end = buf + sizeof(buf);

		attr->type = htons(SWITCH_STUN_ATTR_USERNAME);
		attr->length = 0;
		buf[4] = 0xff;	/* non-zero type for the truncated trailing header */
		buf[5] = 0xff;

		fst_xcheck(switch_stun_packet_next_attribute(attr, end) == 0, "iterator stops at a truncated trailing attribute header without reading past end");
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_stun_next_attribute_hbo)
	{
		/* Positive test for the _hbo iterator, which reads attribute lengths in network byte order
		   (ntohs) for callers walking raw on-the-wire packets that have not been through
		   switch_stun_packet_parse, since parse byte-swaps type and length in place: it must advance by
		   the network-order length and stop at the end. The plain macro would misread these
		   network-order lengths; this confirms the _hbo variant walks normal attributes, not a bounds
		   regression. */
		uint8_t buf[16] = { 0 };
		switch_stun_packet_attribute_t *attr;
		void *end = buf + sizeof(buf);

		attr = (switch_stun_packet_attribute_t *)buf;
		attr->type = htons(SWITCH_STUN_ATTR_USERNAME);
		attr->length = htons(4);	/* network order: _hbo applies ntohs, the plain macro would misread this */
		attr = (switch_stun_packet_attribute_t *)(buf + 8);
		attr->type = htons(SWITCH_STUN_ATTR_PRIORITY);
		attr->length = htons(4);

		attr = (switch_stun_packet_attribute_t *)buf;
		fst_xcheck(switch_stun_packet_next_attribute_hbo(attr, end) != 0, "hbo iterator advances to the second attribute");
		fst_xcheck((uint8_t *)attr == buf + 8, "hbo iterator lands on the second attribute using the network-order length");
		fst_xcheck(switch_stun_packet_next_attribute_hbo(attr, end) == 0, "hbo iterator stops after the last attribute");
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_stun_next_attribute_value_overruns)
	{
		/* An attribute whose 4-byte header fits before end but whose declared value extends past end
		   (more value bytes than remain) must be rejected, so the caller never reads the value out of
		   bounds. This exercises the value-length bound distinctly from a truncated header. */
		uint8_t buf[12] = { 0 };
		switch_stun_packet_attribute_t *attr;
		void *end = buf + 10;	/* ends inside the second attribute's declared value */

		attr = (switch_stun_packet_attribute_t *)buf;
		attr->type = htons(SWITCH_STUN_ATTR_USERNAME);
		attr->length = 0;	/* host order: empty leading attribute */
		attr = (switch_stun_packet_attribute_t *)(buf + 4);
		attr->type = htons(SWITCH_STUN_ATTR_PRIORITY);
		attr->length = 4;	/* value would occupy buf[8..11], past end (buf+10) */

		attr = (switch_stun_packet_attribute_t *)buf;
		fst_xcheck(switch_stun_packet_next_attribute(attr, end) == 0, "attribute whose value overruns end is rejected");
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_stun_walk_reaches_trailing_attribute)
	{
		/* Positive test: parse + walk over a well-formed multi-attribute packet reaches every attribute,
		   including a trailing one whose value ends on the last byte of the message. end_buf is computed
		   here rather than taken from switch_stun_lookup or handle_ice, so how those callers derive it is
		   not covered. */
		uint8_t buf[512] = { 0 };
		switch_stun_packet_t *packet;
		switch_stun_packet_attribute_t *attr;
		void *end_buf;
		int count;

		packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_RESPONSE, NULL, buf);

		attr = (switch_stun_packet_attribute_t *)packet->first_attribute;
		attr->type = htons(SWITCH_STUN_ATTR_USERNAME);
		attr->length = htons(4);
		memcpy(attr->value, "abcd", 4);

		attr = (switch_stun_packet_attribute_t *)(packet->first_attribute + 8);
		attr->type = htons(SWITCH_STUN_ATTR_USERNAME);
		attr->length = htons(4);
		memcpy(attr->value, "efgh", 4);

		packet->header.length = htons(8 + 8);

		packet = switch_stun_packet_parse(buf, SWITCH_STUN_PACKET_MIN_LEN + 8 + 8);
		fst_requires(packet != NULL);

		/* Same end_buf the iterator's callers use: the 20-byte header plus the attribute section. */
		end_buf = buf + SWITCH_STUN_PACKET_MIN_LEN + packet->header.length;

		switch_stun_packet_first_attribute(packet, attr);
		count = 1;
		while (switch_stun_packet_next_attribute(attr, end_buf)) {
			count++;
		}
		fst_xcheck(count == 2, "iterator reaches the trailing attribute in the last bytes of the message");
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

		ice.controlling_failover_state = SWITCH_RTP_ICE_CONTROLLING_STARTUP_NOMINATING;
		fst_check(!switch_rtp_pvt_controlling_failover_response_matches(&ice,
			SWITCH_RTP_ICE_CONTROLLING_STARTUP_NOMINATING, 3, nomination_id, SWITCH_FALSE,
			local_socket, 27018, AF_INET));
		fst_check(!switch_rtp_pvt_controlling_failover_response_matches(&ice,
			SWITCH_RTP_ICE_CONTROLLING_STARTUP_NOMINATING, 2, nomination_id, SWITCH_TRUE,
			local_socket, 27018, AF_INET));
		fst_check(!switch_rtp_pvt_controlling_failover_response_matches(&ice,
			SWITCH_RTP_ICE_CONTROLLING_STARTUP_NOMINATING, 3, wrong_id, SWITCH_TRUE,
			local_socket, 27018, AF_INET));
		fst_check(!switch_rtp_pvt_controlling_failover_response_matches(&ice,
			SWITCH_RTP_ICE_CONTROLLING_STARTUP_NOMINATING, 3, nomination_id, SWITCH_TRUE,
			other_socket, 27018, AF_INET));
		fst_check(!switch_rtp_pvt_controlling_failover_response_matches(&ice,
			SWITCH_RTP_ICE_CONTROLLING_STARTUP_NOMINATING, 3, nomination_id, SWITCH_TRUE,
			local_socket, 27019, AF_INET));
		fst_check(!switch_rtp_pvt_controlling_failover_response_matches(&ice,
			SWITCH_RTP_ICE_CONTROLLING_STARTUP_NOMINATING, 3, nomination_id, SWITCH_TRUE,
			local_socket, 27018, AF_INET6));
		fst_check(switch_rtp_pvt_controlling_failover_response_matches(&ice,
			SWITCH_RTP_ICE_CONTROLLING_STARTUP_NOMINATING, 3, nomination_id, SWITCH_TRUE,
			local_socket, 27018, AF_INET));
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
}
FST_SUITE_END()
}
FST_CORE_END()
