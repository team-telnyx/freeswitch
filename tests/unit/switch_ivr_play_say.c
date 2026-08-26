/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2020, Anthony Minessale II <anthm@freeswitch.org>
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
 * Chris Rienzo <chris@signalwire.com>
 *
 *
 * switch_ivr_play_say.c -- IVR tests
 *
 */
#include <switch.h>
#include <stdlib.h>

#include <test/switch_test.h>

static void on_record_start(switch_event_t *event)
{
	char *str = NULL;
	const char *uuid = switch_event_get_header(event, "Unique-ID");
	switch_event_serialize(event, &str, SWITCH_FALSE);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s", str);
	switch_safe_free(str);
	if (uuid) {
		switch_core_session_t *session = switch_core_session_locate(uuid);
		if (session) {
			switch_channel_t *channel = switch_core_session_get_channel(session);
			const char *recording_id = switch_event_get_header_nil(event, "Recording-Variable-ID");
			if (!strcmp(recording_id, "foo")) {
				switch_channel_set_variable(channel, "record_start_event_test_pass", "true");
			}
			switch_core_session_rwunlock(session);
		}
	}
}

static void on_record_stop(switch_event_t *event)
{
	char *str = NULL;
	const char *uuid = switch_event_get_header(event, "Unique-ID");
	switch_event_serialize(event, &str, SWITCH_FALSE);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s", str);
	switch_safe_free(str);
	if (uuid) {
		switch_core_session_t *session = switch_core_session_locate(uuid);
		if (session) {
			switch_channel_t *channel = switch_core_session_get_channel(session);
			const char *recording_id = switch_event_get_header_nil(event, "Recording-Variable-ID");
			if (!strcmp(recording_id, "foo")) {
				switch_channel_set_variable(channel, "record_stop_event_test_pass", "true");
			}
			switch_core_session_rwunlock(session);
		}
	}
}

static switch_status_t partial_play_and_collect_input_callback(switch_core_session_t *session, void *input, switch_input_type_t input_type, void *data, __attribute__((unused))unsigned int len)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	int *count = (int *)data;

	if (input_type == SWITCH_INPUT_TYPE_EVENT) {
		switch_event_t *event = (switch_event_t *)input;

		if (event->event_id == SWITCH_EVENT_DETECTED_SPEECH) {
			const char *speech_type = switch_event_get_header(event, "Speech-Type");
			char *body = switch_event_get_body(event);

			if (zstr(speech_type) || strcmp(speech_type, "detected-partial-speech")) {
				return status;
			}

			(*count)++;
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "partial events count: %d\n", *count);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "body=[%s]\n", body);
		}
	} else if (input_type == SWITCH_INPUT_TYPE_DTMF) {
		// never mind
	}

	return status;
}

/*
 * Watches PLAYBACK_STOP for one specific channel. Used by
 * peer_hangup_during_lead_frames_ends_bridged_broadcast, which asserts both
 * that the playback ends promptly once the bridge peer is gone and that it is
 * reported as a break rather than as a file that played to the end.
 */
static char playback_watch_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1] = "";
static char playback_watch_status[32] = "";

static void on_playback_stop_watch(switch_event_t *event)
{
	const char *uuid = switch_event_get_header(event, "Unique-ID");

	if (uuid && *playback_watch_uuid && !strcmp(uuid, playback_watch_uuid)) {
		switch_set_string(playback_watch_status, switch_str_nil(switch_event_get_header(event, "Playback-Status")));
	}
}

/*
 * One run of the peer-hangup-during-lead-frames scenario over a real two-leg
 * bridge. bcast_index picks which leg carries the broadcast, so the caller can
 * drive both directions.
 *
 * Leaves the outcome in playback_watch_status: "break" once the stop landed,
 * still empty if the playback never ended.
 *
 * Plain C rather than fct macros: fst_requires expands to a bare "break",
 * which inside a loop would escape only that loop and let the caller run on
 * with half-built fixtures.
 *
 * Returns 0 if the fixture could not be established, so nothing was measured.
 * Sets *in_window to 1 if the hangup really did land before CF_BROADCAST.
 */
static int run_peer_hangup_lead_frames_case(int bcast_index, int *in_window)
{
	switch_core_session_t *legs[2] = { NULL, NULL };
	switch_channel_t *chans[2] = { NULL, NULL };
	switch_call_cause_t cause = SWITCH_CAUSE_NORMAL_CLEARING;
	switch_event_t *event = NULL;
	int ok = 0;
	int i, waited;

	*in_window = 0;
	*playback_watch_status = '\0';

	for (i = 0; i < 2; i++) {
		if (switch_ivr_originate(NULL, &legs[i], &cause, "null/+15553334444", 2,
								 NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS || !legs[i]) {
			goto done;
		}
		chans[i] = switch_core_session_get_channel(legs[i]);
		switch_channel_set_variable(chans[i], "send_silence_when_idle", "-1");
		/* park it the way the session harness does, so uuid_bridge takes over
		 * from a known state. The _timeout form only: the plain
		 * switch_channel_wait_for_state is an unbounded for(;;). */
		switch_channel_set_state(chans[i], CS_SOFT_EXECUTE);
		if (!switch_channel_wait_for_state_timeout(chans[i], CS_SOFT_EXECUTE, 5000) || !switch_channel_up(chans[i])) {
			goto done;
		}
	}

	if (switch_ivr_uuid_bridge(switch_core_session_get_uuid(legs[0]),
							   switch_core_session_get_uuid(legs[1])) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}

	/* both legs must really be inside audio_bridge_thread, or this measures
	 * something else entirely */
	if (switch_channel_wait_for_flag(chans[0], CF_BRIDGED, SWITCH_TRUE, 5000, NULL) != SWITCH_STATUS_SUCCESS ||
		switch_channel_wait_for_flag(chans[1], CF_BRIDGED, SWITCH_TRUE, 5000, NULL) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}

	switch_set_string(playback_watch_uuid, switch_core_session_get_uuid(legs[bcast_index]));
	if (switch_event_bind("peer_gone_broadcast", SWITCH_EVENT_PLAYBACK_STOP, SWITCH_EVENT_SUBCLASS_ANY,
						  on_playback_stop_watch, NULL) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}

	/* shaped exactly like the command switch_ivr_broadcast queues, with the
	 * lead-frames window widened so the hangup lands inside it
	 * deterministically instead of racing 100ms */
	if (switch_event_create(&event, SWITCH_EVENT_COMMAND) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "call-command", "execute");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-name", "playback");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-arg", "silence_stream://20000");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "event-lock", "true");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "lead-frames", "150");
	if (switch_core_session_queue_private_event(legs[bcast_index], &event, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}

	/* let the bridge thread pick it up and settle into the wait */
	switch_sleep(1500000);

	/* the window under test is "dequeued, but CF_BROADCAST not raised yet" */
	*in_window = (switch_core_session_private_event_count(legs[bcast_index]) == 0 &&
				  !switch_channel_test_flag(chans[bcast_index], CF_BROADCAST));

	switch_channel_hangup(chans[!bcast_index], SWITCH_CAUSE_NORMAL_CLEARING);

	/* Fixed: the lead-frames wait ends, the peer check raises the stop, and
	 * the app breaks on its first frame -- PLAYBACK_STOP within ~2s of the
	 * hangup. Regressed: nothing interrupts the bridge thread and
	 * PLAYBACK_STOP only arrives after the whole 20s file. The 12s bound sits
	 * well clear of the 6000ms lead-frames cap and well short of the file. */
	for (waited = 0; waited < 12000 && !*playback_watch_status; waited += 100) {
		switch_sleep(100000);
	}

	ok = 1;

  done:

	/* no-op when never bound */
	switch_event_unbind_callback(on_playback_stop_watch);
	*playback_watch_uuid = '\0';
	/* no-op once the queue took ownership and NULLed it */
	switch_event_destroy(&event);

	for (i = 0; i < 2; i++) {
		if (legs[i]) {
			if (switch_channel_ready(chans[i])) {
				switch_channel_hangup(chans[i], SWITCH_CAUSE_NORMAL_CLEARING);
			}
			switch_core_session_rwunlock(legs[i]);
		}
	}

	return ok;
}

FST_CORE_BEGIN("./conf_playsay")
{
	FST_SUITE_BEGIN(switch_ivr_play_say)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_tone_stream");
			fst_requires_module("mod_sndfile");
			fst_requires_module("mod_dptools");
			fst_requires_module("mod_test");
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		FST_SESSION_BEGIN(play_and_collect_input_failure)
		{
			char terminator_collected = 0;
			char *digits_collected = NULL;
			cJSON *recognition_result = NULL;

			// args
			const char *play_files = "silence_stream://2000";
			const char *speech_engine = "test";
			const char *terminators = "#";
			int min_digits = 1;
			int max_digits = 3;
			int digit_timeout = 15000;
			int no_input_timeout = digit_timeout;
			int speech_complete_timeout = digit_timeout;
			int speech_recognition_timeout = digit_timeout;
			char *speech_grammar_args = switch_core_session_sprintf(fst_session, "{start-input-timers=false,no-input-timeout=%d,vad-silence-ms=%d,speech-timeout=%d,language=en-US}default",
													  no_input_timeout, speech_complete_timeout, speech_recognition_timeout);

			switch_status_t status;

			// collect input - 1#
			fst_sched_recv_dtmf("+1", "1");
			fst_sched_recv_dtmf("+2", "2");
			fst_sched_recv_dtmf("+3", "3");
			status = switch_ivr_play_and_collect_input(fst_session, play_files, speech_engine, speech_grammar_args, min_digits, max_digits, terminators, digit_timeout, &recognition_result, &digits_collected, &terminator_collected, NULL);
			fst_check(recognition_result == NULL);
			fst_check(status == SWITCH_STATUS_SUCCESS); // might be break?
			fst_check_string_equals(cJSON_GetObjectCstr(recognition_result, "text"), NULL);
			fst_check_string_equals(digits_collected, "123");
			fst_check(terminator_collected == 0);
			cJSON_Delete(recognition_result);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(play_and_collect_input_success)
		{
			char terminator_collected = 0;
			char *digits_collected = NULL;
			cJSON *recognition_result = NULL;

			// args
			const char *play_files = "silence_stream://1000";
			const char *speech_engine = "test";
			const char *terminators = "#";
			int min_digits = 1;
			int max_digits = 99;
			int digit_timeout = 5000;
			int no_input_timeout = digit_timeout;
			int speech_complete_timeout = digit_timeout;
			int speech_recognition_timeout = 60000;
			char *speech_grammar_args = switch_core_session_sprintf(fst_session, "{start-input-timers=false,no-input-timeout=%d,vad-silence-ms=%d,speech-timeout=%d,language=en-US}default",
													  no_input_timeout, speech_complete_timeout, speech_recognition_timeout);

			switch_status_t status;

			// collect input - 1#
			fst_sched_recv_dtmf("+2", "1#");
			terminator_collected = 0;
			digits_collected = NULL;
			recognition_result = NULL;
			fst_time_mark();
			status = switch_ivr_play_and_collect_input(fst_session, play_files, speech_engine, speech_grammar_args, min_digits, max_digits, terminators, digit_timeout, &recognition_result, &digits_collected, &terminator_collected, NULL);
			fst_check(recognition_result == NULL);
			// check results
			fst_check_duration(2500, 1000); // should return immediately when term digit is received
			fst_check(status == SWITCH_STATUS_SUCCESS); // might be break?
			fst_check_string_equals(cJSON_GetObjectCstr(recognition_result, "text"), NULL);
			fst_check_string_equals(digits_collected, "1");
			fst_check(terminator_collected == '#');

			// collect input - 1# again, same session
			fst_sched_recv_dtmf("+2", "1#");
			terminator_collected = 0;
			digits_collected = NULL;
			if (recognition_result) cJSON_Delete(recognition_result);
			recognition_result = NULL;
			fst_time_mark();
			status = switch_ivr_play_and_collect_input(fst_session, play_files, speech_engine, speech_grammar_args, min_digits, max_digits, terminators, digit_timeout, &recognition_result, &digits_collected, &terminator_collected, NULL);

			// check results
			fst_check(status == SWITCH_STATUS_SUCCESS); // might be break?
			fst_check_duration(2500, 1000); // should return immediately when term digit is received
			fst_check_string_equals(cJSON_GetObjectCstr(recognition_result, "text"), NULL);
			fst_check_string_equals(digits_collected, "1");
			fst_check(terminator_collected == '#');

			// collect input - 1
			fst_sched_recv_dtmf("+2", "1");
			terminator_collected = 0;
			digits_collected = NULL;
			if (recognition_result) cJSON_Delete(recognition_result);
			recognition_result = NULL;
			fst_time_mark();
			status = switch_ivr_play_and_collect_input(fst_session, play_files, speech_engine, speech_grammar_args, min_digits, max_digits, terminators, digit_timeout, &recognition_result, &digits_collected, &terminator_collected, NULL);
			fst_check(recognition_result == NULL);
			// check results
			fst_check(status == SWITCH_STATUS_SUCCESS); // might be break?
			fst_check_duration(7000, 1000); // should return after timeout when prompt finishes playing
			fst_check_string_equals(cJSON_GetObjectCstr(recognition_result, "text"), NULL);
			fst_check_string_equals(digits_collected, "1");
			fst_check(terminator_collected == 0);

			// collect input - 12#
			fst_sched_recv_dtmf("+2", "12#");
			terminator_collected = 0;
			digits_collected = NULL;
			if (recognition_result) cJSON_Delete(recognition_result);
			recognition_result = NULL;
			fst_time_mark();
			status = switch_ivr_play_and_collect_input(fst_session, play_files, speech_engine, speech_grammar_args, min_digits, max_digits, terminators, digit_timeout, &recognition_result, &digits_collected, &terminator_collected, NULL);
			fst_check(recognition_result == NULL);
			// check results
			fst_check(status == SWITCH_STATUS_SUCCESS); // might be break?
			fst_check_duration(2500, 1000); // should return after timeout when prompt finishes playing
			fst_check_string_equals(cJSON_GetObjectCstr(recognition_result, "text"), NULL);
			fst_check_string_equals(digits_collected, "12");
			fst_check(terminator_collected == '#');

			// collect input - 12# - long spacing
			fst_sched_recv_dtmf("+2", "1");
			fst_sched_recv_dtmf("+4", "2");
			fst_sched_recv_dtmf("+6", "3");
			fst_sched_recv_dtmf("+8", "4");
			fst_sched_recv_dtmf("+10", "#");

			terminator_collected = 0;
			digits_collected = NULL;
			if (recognition_result) cJSON_Delete(recognition_result);
			recognition_result = NULL;
			fst_time_mark();
			status = switch_ivr_play_and_collect_input(fst_session, play_files, speech_engine, speech_grammar_args, min_digits, max_digits, terminators, digit_timeout, &recognition_result, &digits_collected, &terminator_collected, NULL);
			fst_check(recognition_result == NULL);
			// check results
			fst_check(status == SWITCH_STATUS_SUCCESS); // might be break?
			fst_check_duration(10000, 1000); // should return when dtmf terminator is pressed
			fst_check_string_equals(cJSON_GetObjectCstr(recognition_result, "text"), NULL);
			fst_check_string_equals(digits_collected, "1234");
			fst_check(terminator_collected == '#');

			// collect input - make an utterance
			speech_complete_timeout = 500; // 'auto' mode...
			speech_grammar_args = switch_core_session_sprintf(fst_session, "{start-input-timers=false,no-input-timeout=%d,vad-silence-ms=%d,speech-timeout=%d,language=en-US}default",
													  no_input_timeout, speech_complete_timeout, speech_recognition_timeout);
			switch_ivr_displace_session(fst_session, "file_string://silence_stream://500,0!tone_stream://%%(2000,0,350,440)", 0, "r");
			terminator_collected = 0;
			digits_collected = NULL;
			if (recognition_result) cJSON_Delete(recognition_result);
			recognition_result = NULL;
			fst_time_mark();
			status = switch_ivr_play_and_collect_input(fst_session, play_files, speech_engine, speech_grammar_args, min_digits, max_digits, terminators, digit_timeout, &recognition_result, &digits_collected, &terminator_collected, NULL);
			fst_requires(recognition_result);
			// check results
			fst_check(status == SWITCH_STATUS_SUCCESS); // might be break?
			fst_check_duration(2500, 1000); // returns when utterance is done
			fst_check_string_equals(cJSON_GetObjectCstr(recognition_result, "text"), "agent");
			fst_check_string_equals(digits_collected, NULL);
			fst_check(terminator_collected == 0);

			// single digit test
			fst_sched_recv_dtmf("+2", "2");
			max_digits = 1;
			terminator_collected = 0;
			digits_collected = NULL;
			if (recognition_result) cJSON_Delete(recognition_result);
			recognition_result = NULL;
			fst_time_mark();
			status = switch_ivr_play_and_collect_input(fst_session, play_files, speech_engine, speech_grammar_args, min_digits, max_digits, terminators, digit_timeout, &recognition_result, &digits_collected, &terminator_collected, NULL);
			fst_check(recognition_result == NULL);
			// check results
			fst_check(status == SWITCH_STATUS_SUCCESS); // might be break?
			fst_check_duration(2500, 1000); // returns when single digit is pressed
			fst_check_string_equals(cJSON_GetObjectCstr(recognition_result, "text"), NULL);
			fst_check_string_equals(digits_collected, "2");
			fst_check(terminator_collected == 0);

			// three digit test
			fst_sched_recv_dtmf("+2", "259");
			min_digits = 1;
			max_digits = 3;
			terminator_collected = 0;
			digits_collected = NULL;
			if (recognition_result) cJSON_Delete(recognition_result);
			recognition_result = NULL;
			fst_time_mark();
			status = switch_ivr_play_and_collect_input(fst_session, play_files, speech_engine, speech_grammar_args, min_digits, max_digits, terminators, digit_timeout, &recognition_result, &digits_collected, &terminator_collected, NULL);
			fst_check(recognition_result == NULL);
			// check results
			fst_check(status == SWITCH_STATUS_SUCCESS); // might be break?
			fst_check_duration(2000, 1000); // returns when single digit is pressed
			fst_check_string_equals(cJSON_GetObjectCstr(recognition_result, "text"), NULL);
			fst_check_string_equals(digits_collected, "259");
			fst_check(terminator_collected == 0);

			// min digit test
			fst_sched_recv_dtmf("+2", "25");
			min_digits = 3;
			max_digits = 3;
			terminator_collected = 0;
			digits_collected = NULL;
			if (recognition_result) cJSON_Delete(recognition_result);
			recognition_result = NULL;
			fst_time_mark();
			status = switch_ivr_play_and_collect_input(fst_session, play_files, speech_engine, speech_grammar_args, min_digits, max_digits, terminators, digit_timeout, &recognition_result, &digits_collected, &terminator_collected, NULL);
			fst_requires(recognition_result);
			// check results
			fst_check(status == SWITCH_STATUS_SUCCESS); // might be break?
			fst_check_duration(7000, 1000); // inter-digit timeout after 2nd digit pressed
			fst_check_string_equals(cJSON_GetObjectCstr(recognition_result, "text"), "");
			fst_check_string_equals(digits_collected, NULL);
			fst_check(terminator_collected == 0);
			if (recognition_result) cJSON_Delete(recognition_result);
			recognition_result = NULL;
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(play_and_collect_input_partial)
		{
			char terminator_collected = 0;
			char *digits_collected = NULL;
			cJSON *recognition_result = NULL;

			// args
			const char *play_files = "silence_stream://1000";
			const char *speech_engine = "test";
			const char *terminators = "#";
			int min_digits = 1;
			int max_digits = 99;
			int digit_timeout = 500;
			int no_input_timeout = digit_timeout;
			int speech_complete_timeout = digit_timeout;
			int speech_recognition_timeout = 60000;
			char *speech_grammar_args = switch_core_session_sprintf(fst_session, "{start-input-timers=false,no-input-timeout=%d,vad-silence-ms=%d,speech-timeout=%d,language=en-US,partial=true}default",
											no_input_timeout, speech_complete_timeout, speech_recognition_timeout);
			switch_status_t status;
			switch_input_args_t collect_input_args = { 0 };
			switch_input_args_t *args = NULL;
			int count = 0;

			switch_ivr_displace_session(fst_session, "file_string://silence_stream://500,0!tone_stream://%%(2000,0,350,440)", 0, "r");
			terminator_collected = 0;
			digits_collected = NULL;
			if (recognition_result) cJSON_Delete(recognition_result);
			recognition_result = NULL;
			fst_time_mark();
			status = switch_ivr_play_and_collect_input(fst_session, play_files, speech_engine, speech_grammar_args, min_digits, max_digits, terminators, digit_timeout, &recognition_result, &digits_collected, &terminator_collected, NULL);
			fst_requires(recognition_result);
			// check results
			fst_check(status == SWITCH_STATUS_SUCCESS); // might be break?
			fst_check_duration(2500, 1000); // returns when utterance is done
			fst_check_string_equals(cJSON_GetObjectCstr(recognition_result, "text"), "agent");
			fst_check_string_equals(digits_collected, NULL);
			fst_check(terminator_collected == 0);


			switch_ivr_displace_session(fst_session, "file_string://silence_stream://500,0!tone_stream://%%(2000,0,350,440)", 0, "r");
			terminator_collected = 0;
			digits_collected = NULL;
			if (recognition_result) cJSON_Delete(recognition_result);
			recognition_result = NULL;

			args = &collect_input_args;
			args->input_callback = partial_play_and_collect_input_callback;
			args->buf = &count;
			args->buflen = sizeof(int);

			fst_time_mark();
			status = switch_ivr_play_and_collect_input(fst_session, play_files, speech_engine, speech_grammar_args, min_digits, max_digits, terminators, digit_timeout, &recognition_result, &digits_collected, &terminator_collected, args);
			fst_requires(recognition_result);
			// check results
			fst_check(status == SWITCH_STATUS_SUCCESS); // might be break?
			fst_check_duration(2500, 1000); // returns when utterance is done
			fst_check_string_equals(cJSON_GetObjectCstr(recognition_result, "text"), "agent");
			fst_check_string_equals(digits_collected, NULL);
			fst_check(terminator_collected == 0);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "xxx count = %d\n", count);
			fst_check(count == 3); // 3 partial results
			cJSON_Delete(recognition_result);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(record_file_event_vars)
		{
			const char *record_filename = switch_core_session_sprintf(fst_session, "%s" SWITCH_PATH_SEPARATOR "record_file_event_vars-tmp-%s.wav", SWITCH_GLOBAL_dirs.temp_dir, switch_core_session_get_uuid(fst_session));
			switch_event_t *rec_vars = NULL;
			switch_status_t status;
			switch_event_create_subclass(&rec_vars, SWITCH_EVENT_CLONE, SWITCH_EVENT_SUBCLASS_ANY);
			fst_requires(rec_vars);
			switch_event_bind("record_file_event", SWITCH_EVENT_RECORD_START, SWITCH_EVENT_SUBCLASS_ANY, on_record_start, NULL);
			switch_event_bind("record_file_event", SWITCH_EVENT_RECORD_STOP, SWITCH_EVENT_SUBCLASS_ANY, on_record_stop, NULL);
			switch_event_add_header_string(rec_vars, SWITCH_STACK_BOTTOM, "execute_on_record_start", "set record_start_test_pass=true");
			switch_event_add_header_string(rec_vars, SWITCH_STACK_BOTTOM, "execute_on_record_stop", "set record_stop_test_pass=true");
			switch_event_add_header_string(rec_vars, SWITCH_STACK_BOTTOM, "ID", "foo");
			switch_ivr_displace_session(fst_session, "file_string://silence_stream://500,0!tone_stream://%%(2000,0,350,440)", 0, "r");
			status = switch_ivr_record_file_event(fst_session, NULL, record_filename, NULL, 4, rec_vars);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_xcheck(switch_channel_var_true(fst_channel, "record_start_test_pass"), "Expect record_start_test_pass channel variable set to true");
			fst_xcheck(switch_channel_var_true(fst_channel, "record_stop_test_pass"), "Expect record_stop_test_pass channel variable set to true");
			switch_sleep(1000 * 1000);
			fst_xcheck(switch_channel_var_true(fst_channel, "record_start_event_test_pass"), "Expect RECORD_START event received with Recording-Variable-ID set");
			fst_xcheck(switch_channel_var_true(fst_channel, "record_stop_event_test_pass"), "Expect RECORD_STOP event received with Recording-Variable-ID set");
			switch_event_unbind_callback(on_record_start);
			switch_event_unbind_callback(on_record_stop);
			switch_event_destroy(&rec_vars);
			fst_xcheck(switch_file_exists(record_filename, fst_pool) == SWITCH_STATUS_SUCCESS, "Expect recording file to exist");
			unlink(record_filename);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(record_file_event_chan_vars)
		{
			const char *record_filename = switch_core_session_sprintf(fst_session, "%s" SWITCH_PATH_SEPARATOR "record_file_event_chan_vars-tmp-%s.wav", SWITCH_GLOBAL_dirs.temp_dir, switch_core_session_get_uuid(fst_session));
			switch_event_t *rec_vars = NULL;
			switch_status_t status;
			switch_event_create_subclass(&rec_vars, SWITCH_EVENT_CLONE, SWITCH_EVENT_SUBCLASS_ANY);
			fst_requires(rec_vars);
			switch_event_bind("record_file_event", SWITCH_EVENT_RECORD_START, SWITCH_EVENT_SUBCLASS_ANY, on_record_start, NULL);
			switch_event_bind("record_file_event", SWITCH_EVENT_RECORD_STOP, SWITCH_EVENT_SUBCLASS_ANY, on_record_stop, NULL);
			switch_channel_set_variable(fst_channel, "execute_on_record_start_1", "set record_start_test_pass=true");
			switch_channel_set_variable(fst_channel, "execute_on_record_stop_1", "set record_stop_test_pass=true");
			switch_event_add_header_string(rec_vars, SWITCH_STACK_BOTTOM, "ID", "foo");
			switch_ivr_displace_session(fst_session, "file_string://silence_stream://500,0!tone_stream://%%(2000,0,350,440)", 0, "r");
			status = switch_ivr_record_file_event(fst_session, NULL, record_filename, NULL, 4, rec_vars);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_xcheck(switch_channel_var_true(fst_channel, "record_start_test_pass"), "Expect record_start_test_pass channel variable set to true");
			fst_xcheck(switch_channel_var_true(fst_channel, "record_stop_test_pass"), "Expect record_stop_test_pass channel variable set to true");
			switch_sleep(1000 * 1000);
			fst_xcheck(switch_channel_var_true(fst_channel, "record_start_event_test_pass"), "Expect RECORD_START event received with Recording-Variable-ID set");
			fst_xcheck(switch_channel_var_true(fst_channel, "record_stop_event_test_pass"), "Expect RECORD_STOP event received with Recording-Variable-ID set");
			switch_event_unbind_callback(on_record_start);
			switch_event_unbind_callback(on_record_stop);
			switch_event_destroy(&rec_vars);
			fst_xcheck(switch_file_exists(record_filename, fst_pool) == SWITCH_STATUS_SUCCESS, "Expect recording file to exist");
			unlink(record_filename);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(record_file_event_chan_vars_only)
		{
			const char *record_filename = switch_core_session_sprintf(fst_session, "%s" SWITCH_PATH_SEPARATOR "record_file_event_chan_vars-tmp-%s.wav", SWITCH_GLOBAL_dirs.temp_dir, switch_core_session_get_uuid(fst_session));
			switch_status_t status;
			switch_channel_set_variable(fst_channel, "execute_on_record_start_1", "set record_start_test_pass=true");
			switch_channel_set_variable(fst_channel, "execute_on_record_stop_1", "set record_stop_test_pass=true");
			switch_ivr_displace_session(fst_session, "file_string://silence_stream://500,0!tone_stream://%%(2000,0,350,440)", 0, "r");
			status = switch_ivr_record_file_event(fst_session, NULL, record_filename, NULL, 4, NULL);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_xcheck(switch_channel_var_true(fst_channel, "record_start_test_pass"), "Expect record_start_test_pass channel variable set to true");
			fst_xcheck(switch_channel_var_true(fst_channel, "record_stop_test_pass"), "Expect record_stop_test_pass channel variable set to true");
			switch_sleep(1000 * 1000);
			fst_xcheck(switch_file_exists(record_filename, fst_pool) == SWITCH_STATUS_SUCCESS, "Expect recording file to exist");
			unlink(record_filename);
		}
		FST_SESSION_END()

		/*
		 * break_during_lead_frames_stops_playback: end-to-end guard for the
		 * playback_stop race. A queued broadcast waits out its lead-frames
		 * before the app runs; a break raised inside that wait must survive
		 * into switch_ivr_play_file and stop the file.
		 *
		 * Before the fix, switch_ivr_parse_event cleared CF_STOP_BROADCAST
		 * after the wait and switch_core_session_exec cleared CF_BREAK just
		 * before the app, so the stop was discarded and the whole 20s file
		 * played. The duration assertion is what detects that: ~3-6s of
		 * lead-frames plus an immediate break, versus 20s+ of playback.
		 */
		FST_SESSION_BEGIN(break_during_lead_frames_stops_playback)
		{
			switch_event_t *event = NULL;
			switch_stream_handle_t stream = { 0 };
			switch_status_t status;
			char cmd[256];

			fst_requires_module("mod_commands");

			/*
			 * The lead-frames wait is gated on media being up, and both
			 * assertions below depend on that wait actually running. Assert it
			 * rather than letting the test quietly measure something else.
			 */
			fst_requires(switch_channel_media_ready(fst_channel));

			fst_requires(switch_event_create(&event, SWITCH_EVENT_COMMAND) == SWITCH_STATUS_SUCCESS);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "call-command", "execute");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-name", "playback");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-arg", "silence_stream://20000");
			/* same header switch_ivr_broadcast stamps on every queued command */
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "lead-frames", "150");

			/* fire the break ~2s from now, i.e. inside the lead-frames wait */
			switch_snprintf(cmd, sizeof(cmd), "+2 none uuid_break %s", switch_core_session_get_uuid(fst_session));
			SWITCH_STANDARD_STREAM(stream);
			fst_requires(switch_api_execute("sched_api", cmd, NULL, &stream) == SWITCH_STATUS_SUCCESS);
			switch_safe_free(stream.data);

			fst_time_mark();
			status = switch_ivr_parse_event(fst_session, event);

			/*
			 * SUCCESS, not BREAK. Two things have to line up for that.
			 *
			 * switch_ivr_play_file consumes the flag -- it clears CF_BREAK
			 * before returning -- and parse_event's return expression is
			 * "test_flag(CF_BREAK) ? BREAK : status", so the flag is already
			 * gone by then. Nothing re-raises it, because CF_STOP_BROADCAST is
			 * never set: parse_event raises CF_BROADCAST itself, but only
			 * after the lead-frames wait, and uuid_break fires during the
			 * wait. Seeing CF_BROADCAST still clear, uuid_break takes its else
			 * branch and sets CF_BREAK alone.
			 *
			 * That ordering is exactly what the media_ready guard above
			 * protects. Without media the wait is skipped, CF_BROADCAST is
			 * already up when uuid_break fires, it calls stop_broadcast
			 * instead, and the post-loop CF_STOP_BROADCAST branch re-raises
			 * CF_BREAK -- parse_event would return BREAK and this would fail.
			 */
			fst_check(status == SWITCH_STATUS_SUCCESS);

			/*
			 * Duration is the real assertion. With media up the wait runs 150
			 * non-CNG frames, a 3000ms floor capped at 6000ms by max_frames,
			 * then the surviving break cuts playback immediately. 1500-7500
			 * covers that range with margin and still fails hard on the
			 * regression, which is 20s+ of audio.
			 */
			fst_check_duration(4500, 3000);

			switch_event_destroy(&event);
		}
		FST_SESSION_END()


		/*
		 * peer_hangup_during_lead_frames_ends_bridged_broadcast: guard for the
		 * zombie-call race, over a real two-leg bridge with a real
		 * audio_bridge_thread on each leg, run in both directions.
		 *
		 * The zombie is produced by the bridge thread itself: it dequeues the
		 * broadcast, waits out lead-frames, and only then raises
		 * CF_BROADCAST. A peer hangup inside that wait arms nothing --
		 * switch_channel_stop_broadcast() is gated on CF_BROADCAST -- so the
		 * thread used to play the whole file before returning to the bridge
		 * loop where it would finally notice the peer was gone.
		 *
		 * The assertion is on PLAYBACK_STOP -- that it arrives promptly and
		 * reports a break -- rather than on teardown timing, so it does not
		 * depend on hangup_after_bridge semantics.
		 */
		FST_TEST_BEGIN(peer_hangup_during_lead_frames_ends_bridged_broadcast)
		{
			int in_window = 0;

			fst_requires_module("mod_commands");
			fst_requires_module("mod_loopback");

			/* direction 1: the bridge originator carries the broadcast */
			fst_requires(run_peer_hangup_lead_frames_case(0, &in_window));
			fst_xcheck(in_window, "A-leg: expect the peer hangup to land before CF_BROADCAST is raised");
			fst_xcheck(*playback_watch_status, "A-leg: expect the playback to end once the bridge peer is gone, not after the whole file");
			fst_xcheck(!strcmp(playback_watch_status, "break"), "A-leg: expect PLAYBACK_STOP to report a break, not a file played to the end");

			/* direction 2: the bridge originatee carries the broadcast */
			fst_requires(run_peer_hangup_lead_frames_case(1, &in_window));
			fst_xcheck(in_window, "B-leg: expect the peer hangup to land before CF_BROADCAST is raised");
			fst_xcheck(*playback_watch_status, "B-leg: expect the playback to end once the bridge peer is gone, not after the whole file");
			fst_xcheck(!strcmp(playback_watch_status, "break"), "B-leg: expect PLAYBACK_STOP to report a break, not a file played to the end");
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()

