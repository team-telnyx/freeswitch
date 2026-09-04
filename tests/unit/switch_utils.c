/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2018, Anthony Minessale II <anthm@freeswitch.org>
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
 * Windy Wang <xiaofengcanyuexp@163.com>
 *
 * switch_utils.c -- tests switch_utils
 *
 */

#include <switch.h>
#include <test/switch_test.h>

FST_MINCORE_BEGIN("./conf")

FST_SUITE_BEGIN(switch_hash)

FST_SETUP_BEGIN()
{
}
FST_SETUP_END()

FST_TEARDOWN_BEGIN()
{
}
FST_TEARDOWN_END()

FST_TEST_BEGIN(benchmark)
{
    char encoded[1024];
    char *s = "ABCD";

    switch_url_encode(s, encoded, sizeof(encoded));
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "encoded: [%s]\n", encoded);
    fst_check_string_equals(encoded, "ABCD");

    s = "&bryän#!杜金房";
    switch_url_encode(s, encoded, sizeof(encoded));
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "encoded: [%s]\n", encoded);
    fst_check_string_equals(encoded, "%26bry%C3%A4n%23!%E6%9D%9C%E9%87%91%E6%88%BF");
}
FST_TEST_END()

FST_TEST_BEGIN(b64)
{
    switch_size_t size;
    char *str = "ABC";
    unsigned char b64_str[6];
    char decoded_str[4];
    switch_status_t status = switch_b64_encode((unsigned char *)str, strlen(str), b64_str, sizeof(b64_str));
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "b64_str: %s\n", b64_str);
    fst_check(status == SWITCH_STATUS_SUCCESS);
    fst_check_string_equals((const char *)b64_str, "QUJD");

    size = switch_b64_decode((const char *)b64_str, decoded_str, sizeof(decoded_str));
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "decoded_str: %s\n", decoded_str);
    fst_check_string_equals(decoded_str, str);
    fst_check(size == 4);
}
FST_TEST_END()

FST_TEST_BEGIN(frame_buffer_dup_without_data_rebuilds_base_header)
{
	switch_frame_buffer_t *fb = NULL;
	switch_frame_t orig = { 0 };
	switch_frame_t *clone = NULL;
	switch_rtp_packet_t packet = { 0 };
	switch_rtp_packet_t *cloned_packet;

	packet.header.version = 2;
	packet.header.p = 1;
	packet.header.x = 1;
	packet.header.cc = 1;
	orig.packet = &packet;
	orig.data = NULL;
	orig.datalen = 16;
	orig.packetlen = SWITCH_RTP_HEADER_LEN;
	orig.buflen = sizeof(packet);

	fst_requires(switch_frame_buffer_create(&fb, 1) == SWITCH_STATUS_SUCCESS);
	fst_requires(switch_frame_buffer_dup(fb, &orig, &clone) == SWITCH_STATUS_SUCCESS);
	fst_requires(clone != NULL);
	cloned_packet = (switch_rtp_packet_t *) clone->packet;
	fst_check(clone->packetlen == SWITCH_RTP_HEADER_LEN);
	fst_check(clone->datalen == 0);
	fst_check(clone->data == (uint8_t *) clone->packet + SWITCH_RTP_HEADER_LEN);
	fst_check(cloned_packet->header.cc == 0);
	fst_check(cloned_packet->header.x == 0);
	fst_check(cloned_packet->header.p == 0);

	switch_frame_buffer_free(fb, &clone);
	switch_frame_buffer_destroy(&fb);
}
FST_TEST_END()

FST_TEST_BEGIN(is_file_path)
{
    switch_bool_t b = switch_is_file_path("{av_record_audio_only=true");
    fst_requires(b == SWITCH_FALSE);
    b = switch_is_file_path("{av_record_audio_only=true,future_json_var='{key=value}'");
    fst_requires(b == SWITCH_FALSE);
    b = switch_is_file_path("{av_record_audio_only=true,future_json_var='{key=value}'}");
    fst_requires(b == SWITCH_FALSE);
    b = switch_is_file_path("{av_record_audio_only=true,future_json_var='{key=value}'}/foo");
    fst_requires(b == SWITCH_TRUE);
}
FST_TEST_END()

FST_TEST_BEGIN(redact_file_target)
{
    char buf[512];
    const char *out;

    /* mod_av appends "pubUser=... pubPasswd=..." to the RTMP target, and record_session
     * carries its own {} parameters, so neither may reach a log verbatim. */
    out = switch_redact_file_target("{auth_username=alice,auth_password=hunter2}"
                                    "rtmp://recorder.example.com/live/streamkey", buf, sizeof(buf));
    fst_check_string_equals(out, "rtmp://recorder.example.com/...");
    fst_check(strstr(out, "hunter2") == NULL);
    fst_check(strstr(out, "streamkey") == NULL);

    out = switch_redact_file_target("rtmp://recorder.example.com/live/streamkey "
                                    "pubUser=alice pubPasswd=hunter2 flashver=FMLE/3.0", buf, sizeof(buf));
    fst_check_string_equals(out, "rtmp://recorder.example.com/...");
    fst_check(strstr(out, "hunter2") == NULL);

    /* Several parameter groups, and the [] form the record path also accepts. */
    out = switch_redact_file_target("{a=1}{auth_password=hunter2} [b=2]rtmp://host.example.com/app", buf, sizeof(buf));
    fst_check_string_equals(out, "rtmp://host.example.com/...");

    /* Userinfo in the authority goes too. */
    out = switch_redact_file_target("rtmps://bob:s3cret@recorder.example.com:1935/live/key", buf, sizeof(buf));
    fst_check_string_equals(out, "rtmps://recorder.example.com:1935/...");
    fst_check(strstr(out, "s3cret") == NULL);

    /* So does a query string. */
    out = switch_redact_file_target("rtmp://recorder.example.com?token=abc123", buf, sizeof(buf));
    fst_check_string_equals(out, "rtmp://recorder.example.com/...");

    /* A host-only target keeps its shape rather than gaining a phantom path. */
    out = switch_redact_file_target("rtmp://recorder.example.com", buf, sizeof(buf));
    fst_check_string_equals(out, "rtmp://recorder.example.com");

    /* A local path holds no secret, and the log is more useful naming it. */
    out = switch_redact_file_target("/var/lib/freeswitch/recordings/call.wav", buf, sizeof(buf));
    fst_check_string_equals(out, "/var/lib/freeswitch/recordings/call.wav");

    out = switch_redact_file_target("{av_record_audio_only=true}/tmp/call.mp4", buf, sizeof(buf));
    fst_check_string_equals(out, "/tmp/call.mp4");

    /* Nothing usable to render. */
    out = switch_redact_file_target(NULL, buf, sizeof(buf));
    fst_check_string_equals(out, "(unknown)");
    out = switch_redact_file_target("", buf, sizeof(buf));
    fst_check_string_equals(out, "(unknown)");

    /* An unterminated parameter group would leave the secret in the remainder. */
    out = switch_redact_file_target("{auth_password=hunter2 rtmp://host.example.com/app", buf, sizeof(buf));
    fst_check_string_equals(out, "(unknown)");
}
FST_TEST_END()

FST_SUITE_END()

FST_MINCORE_END()

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
