/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2021, Anthony Minessale II <anthm@freeswitch.org>
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
 * Seven Du <seven@signalwire.com>
 *
 *
 * test_sofia_funcs.c -- tests sofia functions
 *
 */

#include <switch.h>
#include <test/switch_test.h>

int protect_dest_uri(switch_caller_profile_t *cp);
char *sofia_glue_sanitize_display_name(char *name);

static int timeout_sec = 10;
static switch_interval_time_t delay_start_ms = 5000;

FST_CORE_EX_BEGIN("./conf", SCF_VG | SCF_USE_SQL)

FST_MODULE_BEGIN(mod_sofia, sofia)

FST_SETUP_BEGIN()
{
	/* Give mod_sofia time to spinup profile threads */
	if (delay_start_ms) {
		switch_sleep(delay_start_ms * 1000);
		delay_start_ms = 0;
	}
}
FST_SETUP_END()

FST_TEARDOWN_BEGIN()
{
}
FST_TEARDOWN_END()

FST_TEST_BEGIN(test_sanitize_display_name)
{
	char buf[128];
	char *result;

	/* Clean input passes through unchanged. */
	switch_copy_string(buf, "Alice", sizeof(buf));
	result = sofia_glue_sanitize_display_name(buf);
	fst_check_string_equals(result, "Alice");

	/* Trailing backslash runs of length 1/2/3 (odd strips, even keeps). */
	switch_copy_string(buf, "Sami\\", sizeof(buf));
	result = sofia_glue_sanitize_display_name(buf);
	fst_check_string_equals(result, "Sami");

	switch_copy_string(buf, "Sami\\\\", sizeof(buf));
	result = sofia_glue_sanitize_display_name(buf);
	fst_check_string_equals(result, "Sami\\\\");

	switch_copy_string(buf, "Sami\\\\\\", sizeof(buf));
	result = sofia_glue_sanitize_display_name(buf);
	fst_check_string_equals(result, "Sami\\\\");

	/* Customer-reported wire shape "Sami\ ": lone '\<space>' neutralized. */
	switch_copy_string(buf, "Sami\\ ", sizeof(buf));
	result = sofia_glue_sanitize_display_name(buf);
	fst_check_string_equals(result, "Sami  ");

	/* Raw '"' neutralized; valid '\"' / '\\' quoted-pairs left intact. */
	switch_copy_string(buf, "Bob\"Smith", sizeof(buf));
	result = sofia_glue_sanitize_display_name(buf);
	fst_check_string_equals(result, "Bob Smith");

	switch_copy_string(buf, "foo\\\"bar", sizeof(buf));
	result = sofia_glue_sanitize_display_name(buf);
	fst_check_string_equals(result, "foo\\\"bar");

	switch_copy_string(buf, "foo\\\\bar", sizeof(buf));
	result = sofia_glue_sanitize_display_name(buf);
	fst_check_string_equals(result, "foo\\\\bar");

	/* Stray '\X' is neutralized (sofia-strict, stricter than RFC 3261). */
	switch_copy_string(buf, "Joe\\,Bloggs", sizeof(buf));
	result = sofia_glue_sanitize_display_name(buf);
	fst_check_string_equals(result, "Joe ,Bloggs");

	switch_copy_string(buf, "a\"b\"c", sizeof(buf));
	result = sofia_glue_sanitize_display_name(buf);
	fst_check_string_equals(result, "a b c");

	/* NULL / empty are no-ops. */
	fst_check(sofia_glue_sanitize_display_name(NULL) == NULL);
	switch_copy_string(buf, "", sizeof(buf));
	result = sofia_glue_sanitize_display_name(buf);
	fst_check_string_equals(result, "");
}
FST_TEST_END()

FST_TEST_BEGIN(test_protect_url)
{
	int ret;
	switch_caller_profile_t cp = { 0 };
	switch_core_new_memory_pool(&cp.pool);

	cp.destination_number = switch_core_strdup(cp.pool, "1234");
	ret = protect_dest_uri(&cp);
	fst_check(ret == 0);
	fst_check_string_equals(cp.destination_number, "1234");

	cp.destination_number = switch_core_strdup(cp.pool, "1234@ip");
	ret = protect_dest_uri(&cp);
	fst_check(ret == 0);
	fst_check_string_equals(cp.destination_number, "1234@ip");

	cp.destination_number = switch_core_strdup(cp.pool, "external/1234@ip");
	ret = protect_dest_uri(&cp);
	fst_check(ret == 0);
	fst_check_string_equals(cp.destination_number, "external/1234@ip");

	cp.destination_number = switch_core_strdup(cp.pool, "external/sip:1234@ip");
	ret = protect_dest_uri(&cp);
	fst_check(ret == 0);
	fst_check_string_equals(cp.destination_number, "external/1234@ip");

	cp.destination_number = switch_core_strdup(cp.pool, "external/sips:1234@ip");
	ret = protect_dest_uri(&cp);
	fst_check(ret == 0);
	fst_check_string_equals(cp.destination_number, "external/1234@ip");

	cp.destination_number = switch_core_strdup(cp.pool, "external/bryän&!杜金房@freeswitch-testing:9080");
	ret = protect_dest_uri(&cp);
	fst_check(ret == 1);
	fst_check_string_equals(cp.destination_number, "external/bry%C3%A4n%26!%E6%9D%9C%E9%87%91%E6%88%BF@freeswitch-testing:9080");

	cp.destination_number = switch_core_strdup(cp.pool, "external/" SWITCH_URL_UNSAFE "@freeswitch-testing:9080");
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "before: [%s]\n", cp.destination_number);
	ret = protect_dest_uri(&cp);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "after: [%s]\n", cp.destination_number);
	fst_check(ret == 1);
	fst_check_string_equals(cp.destination_number, "external/%0D%0A%20%23%25%26%2B%3A%3B%3C%3D%3E%3F@[\\]^`{|}\"@freeswitch-testing:9080");

	switch_core_destroy_memory_pool(&cp.pool);
}
FST_TEST_END()

FST_TEST_BEGIN(originate_test)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_status_t status;
	switch_call_cause_t cause;
	const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
	status = switch_ivr_originate(NULL, &session, &cause, switch_core_sprintf(fst_pool, "{ignore_early_media=true}sofia/internal/park@%s:53060", local_ip_v4), timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_check(session);
	fst_check(status == SWITCH_STATUS_SUCCESS);
	if (session) {
		channel = switch_core_session_get_channel(session);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
		switch_sleep(1 * 1000 * 1000);
	}
}
FST_TEST_END()

#if HAVE_STIRSHAKEN
FST_TEST_BEGIN(sofia_verify_identity_test_no_identity)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_status_t status;
	switch_call_cause_t cause;
	const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
	status = switch_ivr_originate(NULL, &session, &cause, switch_core_sprintf(fst_pool, "{ignore_early_media=true}sofia/internal/verifyidentity@%s:53060", local_ip_v4), timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_check(status != SWITCH_STATUS_SUCCESS);
	fst_check(cause == SWITCH_CAUSE_NO_IDENTITY);
	if (session) {
		channel = switch_core_session_get_channel(session);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
		switch_sleep(1 * 1000 * 1000);
	}
}
FST_TEST_END()

FST_TEST_BEGIN(sofia_verify_identity_test_bad_identity)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_status_t status;
	switch_call_cause_t cause;
	const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
	status = switch_ivr_originate(NULL, &session, &cause, switch_core_sprintf(fst_pool, "{ignore_early_media=true,sip_h_identity=foo;info=bar}sofia/internal/verifyidentity@%s:53060", local_ip_v4), timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_check(status != SWITCH_STATUS_SUCCESS);
	fst_check(cause == SWITCH_CAUSE_INVALID_IDENTITY);
	if (session) {
		channel = switch_core_session_get_channel(session);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
		switch_sleep(1 * 1000 * 1000);
	}
}
FST_TEST_END()

FST_TEST_BEGIN(sofia_verify_identity_test_valid_identity_no_cert_available)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_status_t status;
	switch_call_cause_t cause;
	const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
	status = switch_ivr_originate(NULL, &session, &cause, switch_core_sprintf(fst_pool, "{origination_caller_id_number=+15551231234,ignore_early_media=true,sip_h_identity=eyJhbGciOiJFUzI1NiIsInBwdCI6InNoYWtlbiIsInR5cCI6InBhc3Nwb3J0IiwieDV1IjoiaHR0cDovLzEyNy4wLjAuMS80MDQucGVtIn0.eyJhdHRlc3QiOiJBIiwiZGVzdCI6eyJ0biI6WyIxNTU1MzIxNDMyMSJdfSwiaWF0IjoxNjE4Mjc5OTYzLCJvcmlnIjp7InRuIjoiMTU1NTEyMzEyMzQifSwib3JpZ2lkIjoiMTMxMzEzMTMifQ.Cm34sISkFWYB6ohtjjJEO71Hyz4TQ5qrTDyYmCXBj-ni5Fe7IbNjmMyvY_lD_Go0u2csWQNe8n03fHSO7Z7nNw;info=<http://127.0.0.1/404.pem>;alg=ES256;ppt=shaken}sofia/internal/+15553214321@%s:53060", local_ip_v4), timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_check(status != SWITCH_STATUS_SUCCESS);
	fst_check(cause == SWITCH_CAUSE_INVALID_IDENTITY);
	if (session) {
		channel = switch_core_session_get_channel(session);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
		switch_sleep(1 * 1000 * 1000);
	}
}
FST_TEST_END()

FST_TEST_BEGIN(sofia_auth_identity_test_attest_a)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_status_t status;
	switch_call_cause_t cause;
	const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
	status = switch_ivr_originate(NULL, &session, &cause, switch_core_sprintf(fst_pool, "{origination_caller_id_number=+15551231234,ignore_early_media=true,sip_stir_shaken_attest=A}sofia/internal/+15553214322@%s:53060", local_ip_v4), timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_check(status == SWITCH_STATUS_SUCCESS);
	fst_check(session);
	if (session) {
		channel = switch_core_session_get_channel(session);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
		switch_sleep(1 * 1000 * 1000);
	}
}
FST_TEST_END()

FST_TEST_BEGIN(sofia_auth_identity_test_attest_b)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_status_t status;
	switch_call_cause_t cause;
	const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
	status = switch_ivr_originate(NULL, &session, &cause, switch_core_sprintf(fst_pool, "{origination_caller_id_number=+15551231234,ignore_early_media=true,sip_stir_shaken_attest=B}sofia/internal/+15553214322@%s:53060", local_ip_v4), timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_check(status == SWITCH_STATUS_SUCCESS);
	fst_check(session);
	if (session) {
		channel = switch_core_session_get_channel(session);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
		switch_sleep(1 * 1000 * 1000);
	}
}
FST_TEST_END()

FST_TEST_BEGIN(sofia_auth_identity_test_attest_c)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_status_t status;
	switch_call_cause_t cause;
	const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
	status = switch_ivr_originate(NULL, &session, &cause, switch_core_sprintf(fst_pool, "{origination_caller_id_number=+15551231234,ignore_early_media=true,sip_stir_shaken_attest=C}sofia/internal/+15553214322@%s:53060", local_ip_v4), timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_check(status == SWITCH_STATUS_SUCCESS);
	fst_check(session);
	if (session) {
		channel = switch_core_session_get_channel(session);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
		switch_sleep(1 * 1000 * 1000);
	}
}
FST_TEST_END()

FST_TEST_BEGIN(sofia_verify_identity_test_verified_attest_a_expired)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_status_t status;
	switch_call_cause_t cause;
	const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
	status = switch_ivr_originate(NULL, &session, &cause, switch_core_sprintf(fst_pool, "{origination_caller_id_number=+15551231234,ignore_early_media=true,sip_h_identity=eyJhbGciOiJFUzI1NiIsInBwdCI6InNoYWtlbiIsInR5cCI6InBhc3Nwb3J0IiwieDV1IjoiaHR0cDovLzEyNy4wLjAuMTo4MDgwL2NlcnQucGVtIn0.eyJhdHRlc3QiOiJBIiwiZGVzdCI6eyJ0biI6WyIxNTU1MzIxNDMyMiJdfSwiaWF0IjoxNjE4MzczMTc0LCJvcmlnIjp7InRuIjoiMTU1NTEyMzEyMzQifSwib3JpZ2lkIjoiMzliZDYzZDQtOTE1Mi00MzU0LWFkNjctNjg5NjQ2NmI4ZDI3In0.mUaikwHSOb8RVPwwMZTsqBe57MZY29CgbIqmiiEmyq9DzKZO-y4qShiIVT3serg-xHgC9SCMjUOBWaDfeXnEvA;info=<http://127.0.0.1:8080/cert.pem>;alg=ES256;ppt=shaken}sofia/internal/+15553214322@%s:53060", local_ip_v4), timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_check(status != SWITCH_STATUS_SUCCESS);
	fst_check(cause == SWITCH_CAUSE_CALL_REJECTED);
	if (session) {
		channel = switch_core_session_get_channel(session);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
		switch_sleep(1 * 1000 * 1000);
	}
}
FST_TEST_END()

FST_TEST_BEGIN(sofia_auth_identity_test_attest_a_date)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_status_t status;
	switch_call_cause_t cause;
	const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
	status = switch_ivr_originate(NULL, &session, &cause, switch_core_sprintf(fst_pool, "{origination_caller_id_number=+15551231235,ignore_early_media=true,sip_stir_shaken_attest=A}sofia/internal/+15553214323@%s:53060", local_ip_v4), timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_check(status == SWITCH_STATUS_SUCCESS);
	fst_check(session);
	if (session) {
		channel = switch_core_session_get_channel(session);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
		switch_sleep(10 * 1000 * 1000);
	}
}
FST_TEST_END()

FST_TEST_BEGIN(sofia_trickle_supported_and_accept_headers)
{
	const char *profile;
	char *cmd_args;
	switch_stream_handle_t stream = { 0 };
	char *out;

	profile = switch_core_get_variable("sofia_profile");
	if (zstr(profile)) {
		profile = "internal";
	}

	cmd_args = switch_core_sprintf(fst_pool, "status profile %s", profile);
	SWITCH_STANDARD_STREAM(stream);
	switch_api_execute("sofia", cmd_args, NULL, &stream);
	out = (char *)stream.data;

	fst_check_string_not_empty(out);

	fst_check(out && strstr(out, "Supported:"));
	fst_check(out && switch_stristr(out, "trickle-ice"));

	if (out && switch_stristr(out, "trickle-ice")) {
		fst_check(strstr(out, "Accept:") != NULL);
		fst_check(switch_stristr(out, "application/trickle-ice-sdpfrag") != NULL);
	}

	switch_safe_free(stream.data);
}
FST_TEST_END()

FST_TEST_BEGIN(sofia_trickle_recv_info_advertised_if_enabled)
{
	const char *profile;
	char *cmd_args;
	switch_stream_handle_t stream = { 0 };
	char *out;
	const char *recv_info_pos;

	profile = switch_core_get_variable("sofia_profile");
	if (zstr(profile)) {
		profile = "internal";
	}

	cmd_args = switch_core_sprintf(fst_pool, "status profile %s", profile);
	SWITCH_STANDARD_STREAM(stream);
	switch_api_execute("sofia", cmd_args, NULL, &stream);
	out = (char *)stream.data;

	fst_check_string_not_empty(out);

	if (out && switch_stristr(out, "trickle-ice")) {
		recv_info_pos = switch_stristr(out, "Recv-Info:");
		fst_check((recv_info_pos == NULL) || (switch_stristr(recv_info_pos, "trickle-ice") != NULL));
	}

	switch_safe_free(stream.data);
}
FST_TEST_END()

#endif 

FST_MODULE_END()

FST_CORE_END()


/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
