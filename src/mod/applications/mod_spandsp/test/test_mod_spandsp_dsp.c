/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
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
 * test_mod_spandsp_dsp.c -- TDD (V.18) send and detect over a loopback call
 *
 * Every TDD function of mod_spandsp_dsp.c creates its modem with v18_init(),
 * and the arguments of v18_init() and the names of the V.18 modes changed in
 * spandsp 3.1. These tests send text from one leg of a loopback call and
 * check that a detector (spandsp_tdd_decode_session) on the other leg
 * receives it, for each 5 bit (Baudot) mode, through each sender:
 *
 *   - spandsp_send_tdd    spandsp_tdd_send_session, writes its own frames
 *   - spandsp_inject_tdd  spandsp_tdd_encode_session, a write media bug
 *   - uuid_send_tdd       spandsp_tdd_encode_session, from the API
 *
 * The detector is started with the spandsp_detect_tdd application on the far
 * leg, or with the start_tdd_detect API on the near leg.
 */

#include <switch.h>
#include <test/switch_test.h>

#define TDD_TEXT "HELLO 123"
#define TDD_WAIT_MS 10000


/* The detector adds each message it receives to "tdd_messages". When there is
 * more than one, the variable holds an ARRAY::a|:b list. Check the joined text,
 * with the spaces and separators removed, because the receiver may split the
 * text in more than one message. */
static int tdd_received(switch_channel_t *channel, const char *want)
{
	const char *var = switch_channel_get_variable_dup(channel, "tdd_messages", SWITCH_FALSE, -1);
	char got[256] = "", wanted[256] = "";
	size_t g = 0, w = 0;
	const char *p;

	if (zstr(var)) {
		return 0;
	}

	if (!strncmp(var, "ARRAY::", 7)) {
		var += 7;
	}

	for (p = var; *p && g < sizeof(got) - 1; p++) {
		if (*p == '|' && *(p + 1) == ':') {
			p++;
			continue;
		}
		if (*p != ' ' && *p != '\r' && *p != '\n') {
			got[g++] = *p;
		}
	}

	for (p = want; *p && w < sizeof(wanted) - 1; p++) {
		if (*p != ' ') {
			wanted[w++] = *p;
		}
	}

	return strstr(got, wanted) != NULL;
}

/* Drive the media of the near leg and wait until the channel of uuid has
 * received want. With read set, frames are read from session (its read media
 * bug sees the audio of the far leg); otherwise silence is written to session
 * (its write media bug replaces it with TDD tones). */
static int wait_for_tdd(switch_core_session_t *session, const char *uuid, int read, const char *want)
{
	switch_core_session_t *rx_session;
	switch_time_t until = switch_micro_time_now() + TDD_WAIT_MS * 1000;
	switch_codec_t codec = { 0 };
	switch_frame_t write_frame = { 0 };
	switch_timer_t timer = { 0 };
	int16_t silence[160] = { 0 };
	int ok = 0;

	if (!(rx_session = switch_core_session_locate(uuid))) {
		return 0;
	}

	if (!read) {
		if (switch_core_codec_init(&codec, "L16", NULL, NULL, 8000, 20, 1,
								   SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL,
								   switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
			switch_core_session_rwunlock(rx_session);
			return 0;
		}
		write_frame.codec = &codec;

		/* TDD tones need evenly paced frames */
		if (switch_core_timer_init(&timer, "soft", 20, 160, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
			switch_core_codec_destroy(&codec);
			switch_core_session_rwunlock(rx_session);
			return 0;
		}
	}

	while (switch_micro_time_now() < until) {
		if (tdd_received(switch_core_session_get_channel(rx_session), want)) {
			ok = 1;
			break;
		}

		if (read) {
			switch_frame_t *frame;

			if (!SWITCH_READ_ACCEPTABLE(switch_core_session_read_frame(session, &frame, SWITCH_IO_FLAG_NONE, 0))) {
				break;
			}
		} else {
			write_frame.data = silence;
			write_frame.buflen = sizeof(silence);
			write_frame.datalen = sizeof(silence);
			write_frame.samples = 160;
			memset(silence, 0, sizeof(silence));

			if (switch_core_session_write_frame(session, &write_frame, SWITCH_IO_FLAG_NONE, 0) != SWITCH_STATUS_SUCCESS) {
				break;
			}

			switch_core_timer_next(&timer);
		}
	}

	if (!ok) {
		const char *var = switch_channel_get_variable(switch_core_session_get_channel(rx_session), "tdd_messages");

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Wanted [%s], got [%s]\n", want, var ? var : "(nothing)");
	}

	if (!read) {
		switch_core_timer_destroy(&timer);
		switch_core_codec_destroy(&codec);
	}

	switch_core_session_rwunlock(rx_session);

	return ok;
}

/* Wait until the channel of uuid has a TDD detector. */
static int wait_for_detector(const char *uuid)
{
	int i;

	for (i = 0; i < 100; i++) {
		switch_core_session_t *session;
		int found = 0;

		if ((session = switch_core_session_locate(uuid))) {
			found = switch_channel_get_private(switch_core_session_get_channel(session), "tdd_decode") != NULL;
			switch_core_session_rwunlock(session);
		}

		if (found) {
			return 1;
		}

		switch_yield(20000);
	}

	return 0;
}

static int run_api(const char *cmd, const char *arg)
{
	switch_stream_handle_t stream = { 0 };
	int ok;

	SWITCH_STANDARD_STREAM(stream);
	switch_api_execute(cmd, arg, NULL, &stream);
	ok = stream.data && !strncmp((char *)stream.data, "+OK", 3);
	if (!ok) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s %s: %s\n", cmd, arg, stream.data ? (char *)stream.data : "");
	}
	switch_safe_free(stream.data);

	return ok;
}

static switch_core_session_t *originate(const char *mode, const char *far_leg_apps)
{
	switch_core_session_t *session = NULL;
	switch_call_cause_t cause = SWITCH_CAUSE_NONE;
	char dialstr[512];

	switch_snprintf(dialstr, sizeof(dialstr), "{v18_mode=%s}loopback/m:^:set:v18_mode=%s^%s/default/inline", mode, mode, far_leg_apps);

	if (switch_ivr_originate(NULL, &session, &cause, dialstr, 5, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "originate [%s] failed: %s\n", dialstr, switch_channel_cause2str(cause));
		return NULL;
	}

	return session;
}

/* Hang up and wait until both legs are gone, so that no TDD media bug is left
 * running when the next test starts or the core unloads mod_spandsp. */
static void hangup(switch_core_session_t *session)
{
	int i;

	if (session) {
		switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
	}

	for (i = 0; i < 250 && switch_core_session_count() > 0; i++) {
		switch_yield(20000);
	}
}

/* The far leg sends with the given application. The near leg detects (API).
 * Returns 1 when the near leg receives the text. */
static int far_sends(const char *mode, const char *app)
{
	switch_core_session_t *session;
	char apps[256];
	int ok = 0;

	switch_snprintf(apps, sizeof(apps), "answer^sleep:500^%s:%s^set:send_silence_when_idle=-1^sleep:8000^hangup", app, TDD_TEXT);

	if (!(session = originate(mode, apps))) {
		return 0;
	}

	if (run_api("start_tdd_detect", switch_core_session_get_uuid(session))) {
		ok = wait_for_tdd(session, switch_core_session_get_uuid(session), 1, TDD_TEXT);
	}

	hangup(session);

	return ok;
}

/* The far leg detects (application). The near leg sends with uuid_send_tdd.
 * Returns 1 when the far leg receives the text. */
static int near_sends_api(const char *mode)
{
	switch_core_session_t *session;
	const char *other_uuid;
	char cmd[256];
	int ok = 0;

	if (!(session = originate(mode, "answer^spandsp_detect_tdd^park"))) {
		return 0;
	}

	other_uuid = switch_channel_get_variable(switch_core_session_get_channel(session), "other_loopback_leg_uuid");

	if (other_uuid && wait_for_detector(other_uuid)) {
		switch_snprintf(cmd, sizeof(cmd), "%s %s", switch_core_session_get_uuid(session), TDD_TEXT);
		if (run_api("uuid_send_tdd", cmd)) {
			ok = wait_for_tdd(session, other_uuid, 0, TDD_TEXT);
		}
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "the far leg has no TDD detector\n");
	}

	hangup(session);

	return ok;
}

FST_CORE_BEGIN("./conf")
{
	FST_MODULE_BEGIN(mod_spandsp, mod_spandsp_dsp)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_loopback");
			fst_requires_module("mod_dptools");
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		FST_TEST_BEGIN(send_tdd_5bit_45)
		{
			fst_check(far_sends("5BIT_45", "spandsp_send_tdd"));
		}
		FST_TEST_END()

		FST_TEST_BEGIN(send_tdd_5bit_50)
		{
			fst_check(far_sends("5BIT_50", "spandsp_send_tdd"));
		}
		FST_TEST_END()

		FST_TEST_BEGIN(inject_tdd_5bit_45)
		{
			fst_check(far_sends("5BIT_45", "spandsp_inject_tdd"));
		}
		FST_TEST_END()

		FST_TEST_BEGIN(inject_tdd_5bit_50)
		{
			fst_check(far_sends("5BIT_50", "spandsp_inject_tdd"));
		}
		FST_TEST_END()

		FST_TEST_BEGIN(uuid_send_tdd_5bit_45)
		{
			fst_check(near_sends_api("5BIT_45"));
		}
		FST_TEST_END()

		FST_TEST_BEGIN(uuid_send_tdd_5bit_50)
		{
			fst_check(near_sends_api("5BIT_50"));
		}
		FST_TEST_END()
	}
	FST_MODULE_END()
}
FST_CORE_END()
