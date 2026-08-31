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
 * switch_ivr_async.c -- Async IVR tests
 *
 */
#include <switch.h>
#include <stdlib.h>

#include <test/switch_test.h>

static switch_status_t partial_play_and_collect_input_callback(switch_core_session_t *session, void *input, switch_input_type_t input_type, void *data, __attribute__((unused))unsigned int len)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	int *count = (int *)data;

	if (input_type == SWITCH_INPUT_TYPE_EVENT) {
		switch_event_t *event = (switch_event_t *)input;

		if (event->event_id == SWITCH_EVENT_DETECTED_SPEECH) {
			const char *speech_type = switch_event_get_header(event, "Speech-Type");
                        char *body;

			if (zstr(speech_type) || strcmp(speech_type, "detected-partial-speech")) {
				return status;
			}

			(*count)++;
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "partial events count: %d\n", *count);

			body = switch_event_get_body(event);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "body=[%s]\n", body);
		}
	} else if (input_type == SWITCH_INPUT_TYPE_DTMF) {
		// never mind
	}

	return status;
}

struct asr_stop_helper {
	switch_core_session_t *session;
	int stopped;
};

/* Closes the ASR handle from a thread other than the one pumping media, which
 * is what uuid_detect_speech stop does in production. Waits for mod_test to
 * report that the core speech thread is parked inside asr_get_results, so the
 * close lands in the window rather than by luck. */
static void *SWITCH_THREAD_FUNC asr_stop_thread(switch_thread_t *thread, void *obj)
{
	struct asr_stop_helper *helper = (struct asr_stop_helper *) obj;
	int sanity = 5000;

	while (sanity-- > 0 && !switch_true(switch_core_get_variable("mod_test_asr_in_get_results"))) {
		switch_yield(1000);
	}

	if (switch_core_session_read_lock(helper->session) == SWITCH_STATUS_SUCCESS) {
		if (switch_ivr_stop_detect_speech(helper->session) == SWITCH_STATUS_SUCCESS) {
			helper->stopped = 1;
		}
		switch_core_session_rwunlock(helper->session);
	}

	return NULL;
}

FST_CORE_BEGIN("./conf_async")
{
	FST_SUITE_BEGIN(switch_ivr_play_async)
	{
		FST_SETUP_BEGIN()
		{
			if (0) {
				partial_play_and_collect_input_callback(NULL, NULL, 0, NULL, 0);
			}
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

		FST_SESSION_BEGIN(session_record_pause)
		{
			const char *record_filename = switch_core_session_sprintf(fst_session, "%s%s%s.wav", SWITCH_GLOBAL_dirs.temp_dir, SWITCH_PATH_SEPARATOR, switch_core_session_get_uuid(fst_session));
                        const char *duration_ms_str;
                        int duration_ms;
			switch_status_t status;
			status = switch_ivr_record_session_event(fst_session, record_filename, 0, NULL, NULL);
			fst_xcheck(status == SWITCH_STATUS_SUCCESS, "Expect switch_ivr_record_session() to return SWITCH_STATUS_SUCCESS");

			status = switch_ivr_play_file(fst_session, NULL, "tone_stream://%(400,200,400,450);%(400,2000,400,450)", NULL);
			fst_xcheck(status == SWITCH_STATUS_SUCCESS, "Expect switch_ivr_play_file() to return SWITCH_STATUS_SUCCESS");

			status = switch_ivr_record_session_pause(fst_session, record_filename, SWITCH_TRUE);
			fst_xcheck(status == SWITCH_STATUS_SUCCESS, "Expect switch_ivr_record_session_pause(SWITCH_TRUE) to return SWITCH_STATUS_SUCCESS");

			switch_ivr_play_file(fst_session, NULL, "silence_stream://1000,0", NULL);

			status = switch_ivr_record_session_pause(fst_session, record_filename, SWITCH_FALSE);
			fst_xcheck(status == SWITCH_STATUS_SUCCESS, "Expect switch_ivr_record_session_pause(SWITCH_FALSE) to return SWITCH_STATUS_SUCCESS");

			status = switch_ivr_play_file(fst_session, NULL, "tone_stream://%(400,200,400,450)", NULL);
			fst_xcheck(status == SWITCH_STATUS_SUCCESS, "Expect switch_ivr_play_file() to return SWITCH_STATUS_SUCCESS");

			status = switch_ivr_stop_record_session(fst_session, record_filename);
			fst_xcheck(status == SWITCH_STATUS_SUCCESS, "Expect switch_ivr_stop_record_session() to return SWITCH_STATUS_SUCCESS");

			switch_ivr_play_file(fst_session, NULL, "silence_stream://100,0", NULL);

			fst_xcheck(switch_file_exists(record_filename, fst_pool) == SWITCH_STATUS_SUCCESS, "Expect recording file to exist");

			unlink(record_filename);

			duration_ms_str = switch_channel_get_variable(fst_channel, "record_ms");
			fst_requires(duration_ms_str != NULL);
			duration_ms = atoi(duration_ms_str);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(fst_session), SWITCH_LOG_NOTICE, "Recording duration is %s ms\n", duration_ms_str);
			fst_xcheck(duration_ms > 3500 && duration_ms < 3700, "Expect recording to be between 3500 and 3700 ms");
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(session_record_event_vars)
		{
			const char *record_filename = switch_core_session_sprintf(fst_session, "%s%s%s.wav", SWITCH_GLOBAL_dirs.temp_dir, SWITCH_PATH_SEPARATOR, switch_core_session_get_uuid(fst_session));
			switch_event_t *rec_vars = NULL;
			switch_status_t status;

			switch_event_create_subclass(&rec_vars, SWITCH_EVENT_CLONE, SWITCH_EVENT_SUBCLASS_ANY);
			fst_requires(rec_vars != NULL);

			// record READ stream only- should be complete silence which will trigger the initial timeout.
			// Min seconds set to 2, which will cause the recording to be discarded.
			// Expect the record_start_test_pass and record_stop_test_pass variables set to true
			switch_event_add_header_string(rec_vars, SWITCH_STACK_BOTTOM, "execute_on_record_start", "set record_start_test_pass=true");
			switch_event_add_header_string(rec_vars, SWITCH_STACK_BOTTOM, "execute_on_record_stop", "set record_stop_test_pass=true");
			switch_event_add_header_string(rec_vars, SWITCH_STACK_BOTTOM, SWITCH_RECORD_POST_PROCESS_EXEC_APP_VARIABLE, "set record_post_process_test_pass=true");
			switch_event_add_header_string(rec_vars, SWITCH_STACK_BOTTOM, "RECORD_READ_ONLY", "true");
			switch_event_add_header_string(rec_vars, SWITCH_STACK_BOTTOM, "RECORD_INITIAL_TIMEOUT_MS", "500");
			switch_event_add_header_string(rec_vars, SWITCH_STACK_BOTTOM, "RECORD_MIN_SEC", "2");

			status = switch_ivr_record_session_event(fst_session, record_filename, 0, NULL, rec_vars);
			fst_xcheck(status == SWITCH_STATUS_SUCCESS, "Expect switch_ivr_record_session() to return SWITCH_STATUS_SUCCESS");

			status = switch_ivr_play_file(fst_session, NULL, "tone_stream://%(400,200,400,450);%(400,2000,400,450)", NULL);
			fst_xcheck(status == SWITCH_STATUS_SUCCESS, "Expect switch_ivr_play_file() to return SWITCH_STATUS_SUCCESS");

			status = switch_ivr_record_session_pause(fst_session, record_filename, SWITCH_TRUE);
			fst_xcheck(status != SWITCH_STATUS_SUCCESS, "Expect switch_ivr_record_session_pause(SWITCH_TRUE) not to return SWITCH_STATUS_SUCCESS because the recording has already stopped");

			fst_xcheck(switch_file_exists(record_filename, fst_pool) != SWITCH_STATUS_SUCCESS, "Expect recording file not to exist since it was less than 2 seconds in duration");

			fst_xcheck(switch_channel_var_true(fst_channel, "record_start_test_pass"), "Expect record_start_test_pass channel variable set to true");
			fst_xcheck(switch_channel_var_true(fst_channel, "record_stop_test_pass"), "Expect record_stop_test_pass channel variable set to true");
			fst_xcheck(switch_channel_var_true(fst_channel, "record_post_process_test_pass"), "Expect record_post_process_test_pass channel variable set to true");

			unlink(record_filename);
			switch_event_destroy(&rec_vars);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(session_record_chan_vars)
		{
			const char *record_filename = switch_core_session_sprintf(fst_session, "%s%s%s.wav", SWITCH_GLOBAL_dirs.temp_dir, SWITCH_PATH_SEPARATOR, switch_core_session_get_uuid(fst_session));
			switch_status_t status;

			// record READ stream only- should be complete silence which will trigger the initial timeout.
			// Min seconds set to 2, which will cause the recording to be discarded.
			// Expect the record_start_test_pass and record_stop_test_pass variables set to true
			switch_channel_set_variable(fst_channel, "execute_on_record_start", "set record_start_test_pass=true");
			switch_channel_set_variable(fst_channel, "execute_on_record_stop", "set record_stop_test_pass=true");
			switch_channel_set_variable(fst_channel, SWITCH_RECORD_POST_PROCESS_EXEC_APP_VARIABLE, "set record_post_process_test_pass=true");
			switch_channel_set_variable(fst_channel, "RECORD_READ_ONLY", "true");
			switch_channel_set_variable(fst_channel, "RECORD_INITIAL_TIMEOUT_MS", "500");
			switch_channel_set_variable(fst_channel, "RECORD_MIN_SEC", "2");

			status = switch_ivr_record_session_event(fst_session, record_filename, 0, NULL, NULL);
			fst_xcheck(status == SWITCH_STATUS_SUCCESS, "Expect switch_ivr_record_session() to return SWITCH_STATUS_SUCCESS");

			status = switch_ivr_play_file(fst_session, NULL, "tone_stream://%(400,200,400,450);%(400,2000,400,450)", NULL);
			fst_xcheck(status == SWITCH_STATUS_SUCCESS, "Expect switch_ivr_play_file() to return SWITCH_STATUS_SUCCESS");

			status = switch_ivr_record_session_pause(fst_session, record_filename, SWITCH_TRUE);
			fst_xcheck(status != SWITCH_STATUS_SUCCESS, "Expect switch_ivr_record_session_pause(SWITCH_TRUE) not to return SWITCH_STATUS_SUCCESS because the recording has already stopped");

			fst_xcheck(switch_file_exists(record_filename, fst_pool) != SWITCH_STATUS_SUCCESS, "Expect recording file not to exist since it was less than 2 seconds in duration");

			fst_xcheck(switch_channel_var_true(fst_channel, "record_start_test_pass"), "Expect record_start_test_pass channel variable set to true");
			fst_xcheck(switch_channel_var_true(fst_channel, "record_stop_test_pass"), "Expect record_stop_test_pass channel variable set to true");
			fst_xcheck(switch_channel_var_true(fst_channel, "record_post_process_test_pass"), "Expect record_post_process_test_pass channel variable set to true");

			unlink(record_filename);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(detect_speech_stop_during_get_results)
		{
			switch_threadattr_t *thd_attr = NULL;
			switch_thread_t *thread = NULL;
			struct asr_stop_helper helper = { 0 };
			switch_status_t status;

			switch_core_set_variable("mod_test_asr_in_get_results", NULL);
			switch_core_set_variable("mod_test_asr_use_after_close", NULL);

			status = switch_ivr_detect_speech(fst_session, "test", "test", "test", "", NULL);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			// A no-input timeout is enough to make mod_test report a result, so no speech is needed.
			// The delay then holds the core speech thread inside asr_get_results while the stop runs.
			fst_requires(switch_ivr_set_param_detect_speech(fst_session, "no-input-timeout", "500") == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_ivr_set_param_detect_speech(fst_session, "get-results-delay-ms", "1500") == SWITCH_STATUS_SUCCESS);

			helper.session = fst_session;
			switch_threadattr_create(&thd_attr, fst_pool);
			switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
			fst_requires(switch_thread_create(&thread, thd_attr, asr_stop_thread, &helper, fst_pool) == SWITCH_STATUS_SUCCESS);

			switch_ivr_play_file(fst_session, NULL, "silence_stream://5000,0", NULL);

			switch_thread_join(&status, thread);

			fst_xcheck(helper.stopped, "Expect switch_ivr_stop_detect_speech() to have closed the media bug");
			fst_xcheck(!switch_true(switch_core_get_variable("mod_test_asr_use_after_close")),
					   "Expect asr_close not to run while the speech thread is still inside the module");
		}
		FST_SESSION_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()

