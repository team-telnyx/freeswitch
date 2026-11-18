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
 * Watches PLAYBACK_STOP for one specific channel, recording both the playback
 * status and any hangup cause the event carried. The two tests below assert
 * opposite things about the cause: a peer hangup must report one, and a stop
 * the customer asked for must not, although both report a break.
 */
static char playback_watch_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1] = "";
static char playback_watch_status[32] = "";
static char playback_watch_cause[64] = "";
static int playback_watch_events = 0;
static int playback_watch_with_cause = 0;

static void on_playback_stop_watch(switch_event_t *event)
{
	const char *uuid = switch_event_get_header(event, "Unique-ID");

	if (uuid && *playback_watch_uuid && !strcmp(uuid, playback_watch_uuid)) {
		const char *cause = switch_event_get_header(event, "Hangup-Cause");

		if (!zstr(cause)) {
			++playback_watch_with_cause;
		}

		/* before the status and the count: the callers wait on those, and would
		 * read a stale cause if either went up first */
		switch_set_string(playback_watch_cause, switch_str_nil(cause));
		switch_set_string(playback_watch_status, switch_str_nil(switch_event_get_header(event, "Playback-Status")));
		++playback_watch_events;
	}
}

/* Two null-endpoint legs, parked the way the session harness does, then bridged
 * so a real audio_bridge_thread runs on each. Returns 0 if the fixture could not
 * be established; whatever was created is still left for the teardown. */
static int bring_up_bridged_pair(switch_core_session_t **legs, switch_channel_t **chans)
{
	switch_call_cause_t cause = SWITCH_CAUSE_NORMAL_CLEARING;
	int i;

	for (i = 0; i < 2; ++i) {
		if (switch_ivr_originate(NULL, &legs[i], &cause, "null/+15553334444", 2,
								 NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS || !legs[i]) {
			return 0;
		}
		chans[i] = switch_core_session_get_channel(legs[i]);
		switch_channel_set_variable(chans[i], "send_silence_when_idle", "-1");
		/* park it the way the session harness does, so uuid_bridge takes over
		 * from a known state. The _timeout form only: the plain
		 * switch_channel_wait_for_state is an unbounded for(;;). */
		switch_channel_set_state(chans[i], CS_SOFT_EXECUTE);
		if (!switch_channel_wait_for_state_timeout(chans[i], CS_SOFT_EXECUTE, 5000) || !switch_channel_up(chans[i])) {
			return 0;
		}
	}

	if (switch_ivr_uuid_bridge(switch_core_session_get_uuid(legs[0]),
							   switch_core_session_get_uuid(legs[1])) != SWITCH_STATUS_SUCCESS) {
		return 0;
	}

	/* both legs must really be inside audio_bridge_thread, or this measures
	 * something else entirely */
	if (switch_channel_wait_for_flag(chans[0], CF_BRIDGED, SWITCH_TRUE, 5000, NULL) != SWITCH_STATUS_SUCCESS ||
		switch_channel_wait_for_flag(chans[1], CF_BRIDGED, SWITCH_TRUE, 5000, NULL) != SWITCH_STATUS_SUCCESS) {
		return 0;
	}

	return 1;
}

static void tear_down_bridged_pair(switch_core_session_t **legs, switch_channel_t **chans)
{
	int i;

	for (i = 0; i < 2; ++i) {
		if (legs[i]) {
			if (switch_channel_ready(chans[i])) {
				switch_channel_hangup(chans[i], SWITCH_CAUSE_NORMAL_CLEARING);
			}
			switch_core_session_rwunlock(legs[i]);
		}
	}
}

static void reset_playback_watch(void)
{
	*playback_watch_status = '\0';
	*playback_watch_cause = '\0';
	playback_watch_events = 0;
	playback_watch_with_cause = 0;
}

/*
 * One run of the broadcast-stop scenario over a real two-leg bridge.
 * bcast_index picks which leg carries the broadcast, so the caller can drive
 * both directions.
 *
 * customer_stop picks why the playback ends. 0 kills the bridge peer inside
 * the lead-frames window, which is the bug under test, hanging up with
 * USER_BUSY so the reported cause cannot be confused with the fallback. 1
 * leaves both legs up and raises the stop the customer asks for, which must
 * report no cause at all.
 *
 * Leaves the outcome in playback_watch_status, "break" once the stop landed
 * and still empty if the playback never ended, and the reported hangup cause
 * in playback_watch_cause, still empty when the event carried none.
 *
 * Plain C rather than fct macros: fst_requires expands to a bare "break",
 * which inside a loop would escape only that loop and let the caller run on
 * with half-built fixtures.
 *
 * Returns 0 if the fixture could not be established, so nothing was measured.
 * Sets *in_window to 1 if the hangup really did land before CF_BROADCAST. It
 * is not meaningful when customer_stop is set, which waits for CF_BROADCAST.
 */
static int run_peer_hangup_lead_frames_case(int bcast_index, int customer_stop, int *in_window)
{
	switch_core_session_t *legs[2] = { NULL, NULL };
	switch_channel_t *chans[2] = { NULL, NULL };
	switch_event_t *event = NULL;
	int ok = 0;
	int waited;

	*in_window = 0;
	reset_playback_watch();

	if (!bring_up_bridged_pair(legs, chans)) {
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

	if (customer_stop) {
		/* Shaped like the stop a customer asks for. uuid_break takes exactly
		 * one of two branches, so wait for CF_BROADCAST and take the same one
		 * it would: stop_broadcast alone, which raises CF_BREAK itself. The
		 * point here is the reported cause, not the pre-CF_BROADCAST window.
		 * Both legs stay up. */
		for (waited = 0; waited < 10000 && !switch_channel_test_flag(chans[bcast_index], CF_BROADCAST); waited += 100) {
			switch_sleep(100000);
		}
		if (!switch_channel_test_flag(chans[bcast_index], CF_BROADCAST)) {
			goto done;
		}
		switch_channel_stop_broadcast(chans[bcast_index]);
	} else {
		/* USER_BUSY, not NORMAL_CLEARING: NORMAL_CLEARING is also the fallback
		 * in switch_ivr_play_file_detailed(), so asserting it would pass even if
		 * the peer's real cause were never carried. */
		switch_channel_hangup(chans[!bcast_index], SWITCH_CAUSE_USER_BUSY);
	}

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

	tear_down_bridged_pair(legs, chans);

	return ok;
}

/*
 * A priority broadcast runs nested inside a running playback, and hold music is
 * queued that way. switch_core_session_dequeue_private_event() takes the
 * priority queue first and gates it on CF_EVENT_LOCK_PRI alone, so the
 * event-lock the outer broadcast holds does not stop it, and the playback frame
 * loop dequeues it through switch_ivr_parse_all_events().
 *
 * One peer hangup then cuts both playbacks short, so both have to report it.
 * The customer sees the outer one, so a marker that only the innermost playback
 * can read leaves the reported playback looking like a file that played to the
 * end.
 *
 * Returns 0 if the fixture could not be established, so nothing was measured.
 */
static int run_nested_priority_case(void)
{
	switch_core_session_t *legs[2] = { NULL, NULL };
	switch_channel_t *chans[2] = { NULL, NULL };
	switch_event_t *event = NULL;
	int ok = 0;
	int waited;

	reset_playback_watch();

	if (!bring_up_bridged_pair(legs, chans)) {
		goto done;
	}

	switch_set_string(playback_watch_uuid, switch_core_session_get_uuid(legs[0]));
	if (switch_event_bind("nested_priority_broadcast", SWITCH_EVENT_PLAYBACK_STOP, SWITCH_EVENT_SUBCLASS_ANY,
						  on_playback_stop_watch, NULL) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}

	/* the outer broadcast: the one the customer started. event-lock, the way
	 * switch_ivr_broadcast() queues it, which holds CF_EVENT_LOCK and not
	 * CF_EVENT_LOCK_PRI. */
	if (switch_event_create(&event, SWITCH_EVENT_COMMAND) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "call-command", "execute");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-name", "playback");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-arg", "silence_stream://20000");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "event-lock", "true");
	if (switch_core_session_queue_private_event(legs[0], &event, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}

	/* it has to be playing, not still in the lead-frames wait, or the nested
	 * command lands in a window this case is not about */
	for (waited = 0; waited < 10000 && !switch_channel_test_flag(chans[0], CF_BROADCAST); waited += 100) {
		switch_sleep(100000);
	}
	if (!switch_channel_test_flag(chans[0], CF_BROADCAST)) {
		goto done;
	}
	switch_sleep(500000);

	/* the nested one, on the priority queue, shaped like hold music */
	if (switch_event_create(&event, SWITCH_EVENT_COMMAND) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "call-command", "execute");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-name", "playback");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-arg", "silence_stream://20000");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "event-lock", "true");
	if (switch_core_session_queue_private_event(legs[0], &event, SWITCH_TRUE) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}
	switch_sleep(1500000);

	switch_channel_hangup(chans[1], SWITCH_CAUSE_USER_BUSY);

	/* both playbacks end: the nested one, then the one it nested in */
	for (waited = 0; waited < 12000 && playback_watch_events < 2; waited += 100) {
		switch_sleep(100000);
	}

	ok = 1;

  done:

	switch_event_unbind_callback(on_playback_stop_watch);
	*playback_watch_uuid = '\0';
	switch_event_destroy(&event);

	tear_down_bridged_pair(legs, chans);

	return ok;
}

/*
 * The nested priority broadcast finishes first, and the peer hangs up only
 * afterwards, while the playback it nested in is still running.
 *
 * Before this change, a nested frame on a bridged channel could clear
 * CF_BROADCAST while the outer playback still ran. The hangup-state stop and
 * bridge-exit marker both tested that flag, so a later peer hangup armed nothing
 * and the outer playback ran to the end of its file.
 *
 * Reports through flag_down_after_nested whether the flag really was down once
 * the nested playback had ended, so the case states what it measured instead of
 * only passing.
 *
 * Returns 0 if the fixture could not be established, so nothing was measured.
 */
static int run_nested_completes_first_case(int *flag_down_after_nested, char *cause_out, switch_size_t cause_len)
{
	switch_core_session_t *legs[2] = { NULL, NULL };
	switch_channel_t *chans[2] = { NULL, NULL };
	switch_event_t *event = NULL;
	int ok = 0;
	int waited;

	*flag_down_after_nested = 0;
	*cause_out = '\0';
	reset_playback_watch();

	if (!bring_up_bridged_pair(legs, chans)) {
		goto done;
	}

	switch_set_string(playback_watch_uuid, switch_core_session_get_uuid(legs[0]));
	if (switch_event_bind("nested_completes_first", SWITCH_EVENT_PLAYBACK_STOP, SWITCH_EVENT_SUBCLASS_ANY,
						  on_playback_stop_watch, NULL) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}

	/* the outer broadcast, long enough that it cannot end on its own before
	 * this case finishes */
	if (switch_event_create(&event, SWITCH_EVENT_COMMAND) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "call-command", "execute");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-name", "playback");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-arg", "silence_stream://30000");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "event-lock", "true");
	if (switch_core_session_queue_private_event(legs[0], &event, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}

	for (waited = 0; waited < 10000 && !switch_channel_test_flag(chans[0], CF_BROADCAST); waited += 100) {
		switch_sleep(100000);
	}
	if (!switch_channel_test_flag(chans[0], CF_BROADCAST)) {
		goto done;
	}
	switch_sleep(500000);

	/* the nested one, on the priority queue the way hold music arrives, and
	 * short, so it ends while the outer playback is still going */
	if (switch_event_create(&event, SWITCH_EVENT_COMMAND) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "call-command", "execute");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-name", "playback");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-arg", "silence_stream://400");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "event-lock", "true");
	if (switch_core_session_queue_private_event(legs[0], &event, SWITCH_TRUE) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}

	/* the nested playback ends and its frame cleans up */
	for (waited = 0; waited < 10000 && !playback_watch_events; waited += 100) {
		switch_sleep(100000);
	}
	if (!playback_watch_events) {
		goto done;
	}
	switch_sleep(400000);

	*flag_down_after_nested = !switch_channel_test_flag(chans[0], CF_BROADCAST);

	/* now the peer goes away, with the outer playback still running */
	switch_channel_hangup(chans[1], SWITCH_CAUSE_USER_BUSY);

	/* well under the outer file's remaining time, so a playback that does not
	 * stop shows up as a missing second event rather than as a slow pass */
	for (waited = 0; waited < 8000 && playback_watch_events < 2; waited += 100) {
		switch_sleep(100000);
	}

	switch_copy_string(cause_out, playback_watch_cause, cause_len);
	ok = playback_watch_events >= 2 ? 1 : 0;

  done:

	switch_event_unbind_callback(on_playback_stop_watch);
	*playback_watch_uuid = '\0';
	switch_event_destroy(&event);

	tear_down_bridged_pair(legs, chans);

	return ok;
}

/*
 * A setter on another thread checks CF_BROADCAST, then stalls while the
 * broadcast it saw finishes and cleans up, then resumes and writes the marker.
 * The write itself is what survives, so the case is reproduced here without
 * threads: run a broadcast, keep the generation that was current inside it,
 * let it end, then write the marker with that generation, which is exactly
 * what the resumed setter would have written.
 *
 * A later playback on the surviving leg must then report no cause, whether it
 * ends on its own or the customer stops it. It enters switch_ivr_play_file()
 * directly, so it never reaches switch_ivr_parse_event() and nothing clears
 * the marker for it, which is the entry a dialplan app takes as well; only the
 * generation makes the stale value unreadable.
 *
 * It runs on this thread, not on the leg's own thread, which is still in
 * audio_bridge_thread: a dialplan app would have the session to itself. So two
 * threads read frames on the session, switch_core_session_read_frame() hands
 * whichever loses codec_read_mutex a CNG frame, and the bridge thread can
 * consume the break the customer_stop half raises, in which case that half
 * measures a playback that ran to its end. Both halves therefore assert only
 * the one thing that holds on either outcome: no cause is reported. Giving the
 * playback the session to itself needs a fixture that ends the bridge without
 * ending the leg.
 *
 * Returns 0 if the fixture could not be established, so nothing was measured.
 */
static int run_stale_setter_case(int customer_stop, char *cause_out, switch_size_t cause_len, char *status_out, switch_size_t status_len)
{
	switch_core_session_t *legs[2] = { NULL, NULL };
	switch_channel_t *chans[2] = { NULL, NULL };
	switch_event_t *event = NULL;
	uint32_t stale_gen = 0;
	int ok = 0;
	int waited;

	*cause_out = '\0';
	*status_out = '\0';
	reset_playback_watch();

	if (!bring_up_bridged_pair(legs, chans)) {
		goto done;
	}

	/* a short broadcast, only to open and close one generation */
	if (switch_event_create(&event, SWITCH_EVENT_COMMAND) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "call-command", "execute");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-name", "playback");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-arg", "silence_stream://200");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "event-lock", "true");
	if (switch_core_session_queue_private_event(legs[0], &event, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}

	for (waited = 0; waited < 10000 && !switch_channel_test_flag(chans[0], CF_BROADCAST); waited += 100) {
		switch_sleep(100000);
	}
	if (!switch_channel_test_flag(chans[0], CF_BROADCAST)) {
		goto done;
	}

	/* what the stalled setter read, before the broadcast cleaned up */
	stale_gen = switch_channel_get_broadcast_generation(chans[0]);

	for (waited = 0; waited < 10000 && switch_channel_test_flag(chans[0], CF_BROADCAST); waited += 100) {
		switch_sleep(100000);
	}
	if (switch_channel_test_flag(chans[0], CF_BROADCAST)) {
		goto done;
	}

	/* the resumed setter. Both legs are still up: nothing went away. */
	switch_channel_set_variable_printf(chans[0], SWITCH_PLAYBACK_PEER_HANGUP_VARIABLE, "%u", stale_gen);

	reset_playback_watch();
	switch_set_string(playback_watch_uuid, switch_core_session_get_uuid(legs[0]));
	if (switch_event_bind("stale_setter", SWITCH_EVENT_PLAYBACK_STOP, SWITCH_EVENT_SUBCLASS_ANY,
						  on_playback_stop_watch, NULL) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}

	if (customer_stop) {
		/* the stop the customer asks for, raised before the file opens so the
		 * playback breaks on its first frame without another thread */
		switch_channel_set_flag(chans[0], CF_BREAK);
	}

	/* straight into the app, never switch_ivr_parse_event(), the way a dialplan
	 * playback reaches it, but on this thread rather than the leg's own */
	switch_ivr_play_file(legs[0], NULL, "silence_stream://400", NULL);

	for (waited = 0; waited < 5000 && !playback_watch_events; waited += 100) {
		switch_sleep(100000);
	}

	switch_copy_string(cause_out, playback_watch_cause, cause_len);
	switch_copy_string(status_out, playback_watch_status, status_len);
	ok = playback_watch_events ? 1 : 0;

  done:

	switch_event_unbind_callback(on_playback_stop_watch);
	*playback_watch_uuid = '\0';
	switch_event_destroy(&event);

	tear_down_bridged_pair(legs, chans);

	return ok;
}

struct peer_gone_watch {
	const char *uuid;
	switch_queue_t *events;
};

static void on_peer_gone_stop(switch_event_t *event)
{
	struct peer_gone_watch *watch = event->bind_user_data;
	const char *uuid = switch_event_get_header(event, "Unique-ID");
	switch_event_t *copy = NULL;

	if (uuid && !strcmp(uuid, watch->uuid) && switch_event_dup(&copy, event) == SWITCH_STATUS_SUCCESS) {
		if (switch_queue_trypush(watch->events, copy) != SWITCH_STATUS_SUCCESS) {
			switch_event_destroy(&copy);
		}
	}
}

/* Drive the real peer-missing check from the state left by a hangup writer
 * between broadcast iterations. Seeding that boundary avoids a timing race;
 * playback and its reported cause still come from the real entry point. The
 * leg is a bridge originator, whose exit reads the peer's answer state. */
static switch_event_t *run_peer_gone_marker_case(switch_core_session_t *session, const char *kind)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	struct peer_gone_watch watch = { 0 };
	switch_event_t *command = NULL, *stopped = NULL, *extra = NULL;
	uint32_t generation;

	watch.uuid = switch_core_session_get_uuid(session);
	if (switch_queue_create(&watch.events, 4, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
		return NULL;
	}
	if (switch_event_create(&command, SWITCH_EVENT_COMMAND) != SWITCH_STATUS_SUCCESS) {
		return NULL;
	}
	if (switch_event_bind("peer_gone_marker", SWITCH_EVENT_PLAYBACK_STOP, SWITCH_EVENT_SUBCLASS_ANY,
						  on_peer_gone_stop, &watch) != SWITCH_STATUS_SUCCESS) {
		switch_event_destroy(&command);
		return NULL;
	}

	switch_channel_set_flag(channel, CF_BROADCAST);
	switch_channel_inc_broadcast_generation(channel);
	generation = switch_channel_get_broadcast_generation(channel);
	switch_channel_set_flag(channel, CF_BRIDGED);
	switch_channel_set_flag(channel, CF_BRIDGE_ORIGINATOR);
	switch_channel_set_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE, "00000000-0000-0000-0000-000000000000");
	switch_channel_set_variable(channel, SWITCH_PARK_AFTER_BRIDGE_VARIABLE, "true");
	switch_channel_set_variable(channel, "last_bridge_hangup_cause", "USER_BUSY");
	if (strcmp(kind, "missing")) {
		/* no cause in the record, so the reader takes last_bridge_hangup_cause */
		switch_channel_set_variable_printf(channel, SWITCH_PLAYBACK_PEER_HANGUP_VARIABLE, "%u:%d:",
										   !strcmp(kind, "stale") ? generation - 1 : generation,
										   strcmp(kind, "unanswered") ? 1 : 0);
	}
	switch_event_add_header_string(command, SWITCH_STACK_BOTTOM, "call-command", "execute");
	switch_event_add_header_string(command, SWITCH_STACK_BOTTOM, "execute-app-name", "playback");
	switch_event_add_header_string(command, SWITCH_STACK_BOTTOM, "execute-app-arg", "silence_stream://200");
	switch_ivr_parse_event(session, command);
	/* Bound a missing event, without making elapsed time a success condition. */
	switch_queue_pop_timeout(watch.events, (void **) &stopped, 5000000);
	switch_event_unbind_callback(on_peer_gone_stop);
	while (switch_queue_trypop(watch.events, (void **) &extra) == SWITCH_STATUS_SUCCESS) {
		switch_event_destroy(&extra);
	}
	switch_event_destroy(&command);
	switch_channel_clear_flag(channel, CF_BRIDGED);
	switch_channel_clear_flag(channel, CF_BRIDGE_ORIGINATOR);
	switch_channel_clear_flag(channel, CF_BROADCAST);
	switch_channel_clear_flag(channel, CF_STOP_BROADCAST);
	switch_channel_clear_flag(channel, CF_BREAK);
	switch_channel_inc_broadcast_generation(channel);
	switch_channel_set_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE, NULL);
	switch_channel_set_variable(channel, SWITCH_PLAYBACK_PEER_HANGUP_VARIABLE, NULL);
	switch_channel_set_variable(channel, SWITCH_PARK_AFTER_BRIDGE_VARIABLE, NULL);
	switch_channel_set_variable(channel, "last_bridge_hangup_cause", NULL);
	return stopped;
}

struct bond_playback_watch {
	switch_core_session_t *session;
	switch_core_session_t *peer;
	switch_queue_t *events;
};

struct bond_playback_result {
	switch_event_t *stopped;
	int started_unbridged;
	int hangup_completed_first;
};

static void on_bond_playback_event(switch_event_t *event)
{
	struct bond_playback_watch *watch = event->bind_user_data;
	const char *uuid = switch_event_get_header(event, "Unique-ID");
	switch_event_t *copy = NULL;
	int playing, hangup;

	if (!uuid) return;
	playing = !strcmp(uuid, switch_core_session_get_uuid(watch->session)) &&
		(event->event_id == SWITCH_EVENT_PLAYBACK_START || event->event_id == SWITCH_EVENT_PLAYBACK_STOP);
	hangup = watch->peer && event->event_id == SWITCH_EVENT_CHANNEL_HANGUP_COMPLETE &&
		!strcmp(uuid, switch_core_session_get_uuid(watch->peer));
	if ((!playing && !hangup) || switch_event_dup(&copy, event) != SWITCH_STATUS_SUCCESS) return;

	if (playing && watch->peer && event->event_id == SWITCH_EVENT_PLAYBACK_START) {
		switch_channel_t *channel = switch_core_session_get_channel(watch->session);
		switch_channel_t *peer_channel = switch_core_session_get_channel(watch->peer);

		switch_event_add_header_string(copy, SWITCH_STACK_BOTTOM, "Test-Unbridged-Broadcast",
			switch_channel_test_flag(channel, CF_BROADCAST) && !switch_channel_test_flag(channel, CF_BRIDGED) &&
			!switch_channel_test_flag(peer_channel, CF_BRIDGED) ? "true" : "false");
		switch_channel_hangup(peer_channel, SWITCH_CAUSE_USER_BUSY);
	}
	if (switch_queue_trypush(watch->events, copy) != SWITCH_STATUS_SUCCESS) {
		switch_event_destroy(&copy);
	}
}

/* Both sessions stay read-locked until the callback is unbound. The optional
 * peer hangs up only after real playback starts; no timer triggers the defect. */
static int run_bond_playback(switch_core_session_t *session, switch_core_session_t *peer,
							 const char *file, struct bond_playback_result *result)
{
	struct bond_playback_watch watch = { 0 };
	switch_event_t *command = NULL, *event = NULL, *started = NULL, *complete = NULL;
	unsigned long start_seq, complete_seq, stop_seq;
	int i, ok;

	memset(result, 0, sizeof(*result));
	watch.session = session;
	watch.peer = peer;
	if (switch_queue_create(&watch.events, 8, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS ||
		switch_event_create(&command, SWITCH_EVENT_COMMAND) != SWITCH_STATUS_SUCCESS) return 0;
	if (switch_event_bind("bond_playback", SWITCH_EVENT_ALL, SWITCH_EVENT_SUBCLASS_ANY,
						  on_bond_playback_event, &watch) != SWITCH_STATUS_SUCCESS) {
		switch_event_destroy(&command);
		return 0;
	}
	switch_event_add_header_string(command, SWITCH_STACK_BOTTOM, "call-command", "execute");
	switch_event_add_header_string(command, SWITCH_STACK_BOTTOM, "execute-app-name", "playback");
	switch_event_add_header_string(command, SWITCH_STACK_BOTTOM, "execute-app-arg", file);
	switch_ivr_parse_event(session, command);
	for (i = 0; i < (peer ? 3 : 2); ++i) {
		if (switch_queue_pop_timeout(watch.events, (void **) &event, 5000000) != SWITCH_STATUS_SUCCESS) break;
		if (event->event_id == SWITCH_EVENT_PLAYBACK_START && !started) {
			started = event;
		} else if (event->event_id == SWITCH_EVENT_PLAYBACK_STOP && !result->stopped) {
			result->stopped = event;
		} else if (event->event_id == SWITCH_EVENT_CHANNEL_HANGUP_COMPLETE && !complete) {
			complete = event;
		} else {
			switch_event_destroy(&event);
		}
	}
	switch_event_unbind_callback(on_bond_playback_event);
	while (switch_queue_trypop(watch.events, (void **) &event) == SWITCH_STATUS_SUCCESS) {
		switch_event_destroy(&event);
	}
	ok = started && result->stopped && (!peer || complete);
	if (ok && peer) {
		start_seq = strtoul(switch_event_get_header_nil(started, "Event-Sequence"), NULL, 10);
		complete_seq = strtoul(switch_event_get_header_nil(complete, "Event-Sequence"), NULL, 10);
		stop_seq = strtoul(switch_event_get_header_nil(result->stopped, "Event-Sequence"), NULL, 10);
		result->started_unbridged = switch_true(switch_event_get_header(started, "Test-Unbridged-Broadcast"));
		result->hangup_completed_first = start_seq && complete_seq > start_seq && stop_seq > complete_seq;
	}
	switch_event_destroy(&started);
	switch_event_destroy(&complete);
	switch_event_destroy(&command);
	return ok;
}

static switch_core_session_t *start_bond_peer(void)
{
	switch_core_session_t *peer = NULL;
	switch_channel_t *channel;
	switch_call_cause_t cause = SWITCH_CAUSE_NORMAL_CLEARING;

	if (switch_ivr_originate(NULL, &peer, &cause, "null/+15553334444", 2,
							 NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS || !peer) return NULL;
	channel = switch_core_session_get_channel(peer);
	switch_channel_set_state(channel, CS_SOFT_EXECUTE);
	if (!switch_channel_wait_for_state_timeout(channel, CS_SOFT_EXECUTE, 5000) || !switch_channel_up(channel)) {
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(peer);
		return NULL;
	}
	return peer;
}

/* Seed only the unbridged bond boundary. The null peer's own state thread runs
 * the real hangup handler; CHANNEL_HANGUP_COMPLETE proves it finished. */
static int run_unbridged_bond_case(switch_core_session_t *session, const char *bond,
								   struct bond_playback_result *result)
{
	switch_core_session_t *peer = start_bond_peer();
	switch_channel_t *channel = switch_core_session_get_channel(session);
	int ok = 0;

	memset(result, 0, sizeof(*result));
	if (!peer) return 0;
	switch_channel_set_variable(channel, bond, switch_core_session_get_uuid(peer));
	switch_channel_set_variable(switch_core_session_get_channel(peer), SWITCH_SIGNAL_BOND_VARIABLE,
								switch_core_session_get_uuid(session));
	ok = run_bond_playback(session, peer, "silence_stream://2000", result);
	switch_channel_set_variable(channel, bond, NULL);
	switch_channel_hangup(switch_core_session_get_channel(peer), SWITCH_CAUSE_NORMAL_CLEARING);
	switch_core_session_rwunlock(peer);
	return ok;
}

/* Reproduce the transfer boundary without racing a transfer thread: the bridge
 * flag still exists after the current signal bond was removed. */
static int run_current_bond_case(switch_core_session_t *session, const char *kind,
								 struct bond_playback_result *result)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_core_session_t *peer = NULL;
	const char *missing_uuid = "00000000-0000-0000-0000-000000000000";
	int ok;

	memset(result, 0, sizeof(*result));
	if (!strcmp(kind, "live") && !(peer = start_bond_peer())) return 0;
	switch_channel_set_variable(channel, SWITCH_PLAYBACK_PEER_HANGUP_VARIABLE, NULL);
	switch_channel_set_variable(channel, SWITCH_ORIGINATE_SIGNAL_BOND_VARIABLE, strcmp(kind, "absent") ? missing_uuid : NULL);
	switch_channel_set_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE,
		peer ? switch_core_session_get_uuid(peer) : (!strcmp(kind, "missing") ? missing_uuid : NULL));
	switch_channel_set_flag(channel, CF_BRIDGED);
	ok = run_bond_playback(session, NULL, "silence_stream://200", result);
	switch_channel_clear_flag(channel, CF_BRIDGED);
	switch_channel_set_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE, NULL);
	switch_channel_set_variable(channel, SWITCH_ORIGINATE_SIGNAL_BOND_VARIABLE, NULL);
	if (peer) {
		switch_channel_hangup(switch_core_session_get_channel(peer), SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(peer);
	}
	return ok;
}

/* A case name alone does not prove that its session and playback ran. Keep the
 * observed result with the test output, including setup failure and late peers. */
static void report_bond_playback(const char *name, const char *kind, switch_core_session_t *session,
								 int setup, const struct bond_playback_result *result)
{
	switch_event_t *stopped = result->stopped;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	const char *marker = switch_channel_get_variable(channel, SWITCH_PLAYBACK_PEER_HANGUP_VARIABLE);

	printf("\nBOND_PLAYBACK case=%s kind=%s uuid=%s setup=%d unbridged=%d ordered=%d "
		   "status=%s code=%s samples=%s cause=%s stop_marker=%s marker=%s live=%d\n",
		   name, kind, switch_core_session_get_uuid(session), setup, result->started_unbridged,
		   result->hangup_completed_first,
		   stopped ? switch_event_get_header_nil(stopped, "Playback-Status") : "",
		   stopped ? switch_event_get_header_nil(stopped, "Playback-Status-Code") : "",
		   stopped ? switch_event_get_header_nil(stopped, "variable_playback_samples") : "",
		   stopped ? switch_event_get_header_nil(stopped, "Hangup-Cause") : "",
		   stopped ? switch_event_get_header_nil(stopped, "variable_playback_peer_hangup") : "",
		   switch_str_nil(marker), switch_channel_ready(channel) ? 1 : 0);
}

/* Sets "name=value;name=value" on a channel. */
static void set_channel_vars(switch_channel_t *channel, const char *vars)
{
	char *dup = strdup(vars);
	char *pairs[16] = { 0 };
	int n = switch_separate_string(dup, ';', pairs, (sizeof(pairs) / sizeof(pairs[0])));
	int i;

	for (i = 0; i < n; ++i) {
		char *value = strchr(pairs[i], '=');

		if (value) {
			*value++ = '\0';
			switch_channel_set_variable(channel, pairs[i], value);
		}
	}

	free(dup);
}

/* One row of the bridge exit table: the playing leg (1 for the bridge originator,
 * plus 2 for a leg with CF_XFER_ZOMBIE), the peer state that the record holds, the
 * variables the exit reads, whether the dialplan has another application after the
 * bridge, and the cause the leg must report. */
struct exit_row {
	const char *name;
	int leg;
	const char *peer;
	const char *vars;
	int next_app;
	const char *expect;
};

static const char *exit_vars[] = {
	"hangup_after_bridge", "park_after_bridge", "park_after_early_bridge", "transfer_after_bridge",
	"transfer_after_early_bridge", "exec_after_bridge_app", "continue_on_fail", "failure_causes", "transfer_on_fail"
};

/*
 * Counts what one leg reports while a broadcast runs on it: PLAYBACK_START,
 * PLAYBACK_STOP and the CHANNEL_EXECUTE_COMPLETE of play_and_get_digits.
 */
static char leg_watch_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1] = "";
static int leg_watch_starts = 0;
static int leg_watch_stops = 0;
static char leg_watch_stop_cause[64] = "";
static int leg_watch_completes = 0;
static char leg_watch_complete_cause[64] = "";
static switch_time_t leg_watch_complete_time = 0;

static void on_leg_watch(switch_event_t *event)
{
	const char *uuid = switch_event_get_header(event, "Unique-ID");

	if (!uuid || !*leg_watch_uuid || strcmp(uuid, leg_watch_uuid)) {
		return;
	}

	/* the cause and the time before the count: the callers wait on the count */
	if (event->event_id == SWITCH_EVENT_PLAYBACK_START) {
		++leg_watch_starts;
	} else if (event->event_id == SWITCH_EVENT_PLAYBACK_STOP) {
		switch_set_string(leg_watch_stop_cause, switch_event_get_header_nil(event, "Hangup-Cause"));
		++leg_watch_stops;
	} else if (event->event_id == SWITCH_EVENT_CHANNEL_EXECUTE_COMPLETE &&
			   !strcmp(switch_event_get_header_nil(event, "Application"), "play_and_get_digits")) {
		switch_set_string(leg_watch_complete_cause, switch_event_get_header_nil(event, "Hangup-Cause"));
		leg_watch_complete_time = switch_micro_time_now();
		++leg_watch_completes;
	}
}

/*
 * A broadcast on the originatee of a bridged pair, then a hangup of the
 * originator with USER_BUSY. vars ("name=value;...") go on the playing leg
 * first. The hangup comes settle_ms after the playing leg reported min_starts
 * PLAYBACK_STARTs. Then it waits wait_ms, or less once play_and_get_digits
 * completed. *hangup_time is the time of the hangup. Returns 0 if the fixture
 * could not be established, so nothing was measured.
 */
static int run_originatee_broadcast_case(const char *vars, const char *app, const char *arg, const char *loops,
										 int min_starts, int settle_ms, int wait_ms, switch_time_t *hangup_time)
{
	switch_core_session_t *legs[2] = { NULL, NULL };
	switch_channel_t *chans[2] = { NULL, NULL };
	switch_event_t *event = NULL;
	int ok = 0;
	int waited;

	leg_watch_starts = leg_watch_stops = leg_watch_completes = 0;
	*leg_watch_stop_cause = *leg_watch_complete_cause = '\0';
	leg_watch_complete_time = 0;
	*hangup_time = 0;

	if (!bring_up_bridged_pair(legs, chans)) {
		goto done;
	}

	if (vars) {
		set_channel_vars(chans[1], vars);
	}

	switch_set_string(leg_watch_uuid, switch_core_session_get_uuid(legs[1]));
	if (switch_event_bind("leg_watch", SWITCH_EVENT_PLAYBACK_START, SWITCH_EVENT_SUBCLASS_ANY, on_leg_watch, NULL) != SWITCH_STATUS_SUCCESS ||
		switch_event_bind("leg_watch", SWITCH_EVENT_PLAYBACK_STOP, SWITCH_EVENT_SUBCLASS_ANY, on_leg_watch, NULL) != SWITCH_STATUS_SUCCESS ||
		switch_event_bind("leg_watch", SWITCH_EVENT_CHANNEL_EXECUTE_COMPLETE, SWITCH_EVENT_SUBCLASS_ANY, on_leg_watch, NULL) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}

	if (switch_event_create(&event, SWITCH_EVENT_COMMAND) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "call-command", "execute");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-name", app);
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-arg", arg);
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "event-lock", "true");
	if (loops) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "loops", loops);
	}
	if (switch_core_session_queue_private_event(legs[1], &event, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS) {
		goto done;
	}

	for (waited = 0; waited < 10000 && leg_watch_starts < min_starts; waited += 50) {
		switch_sleep(50000);
	}
	if (leg_watch_starts < min_starts) {
		goto done;
	}
	switch_sleep(settle_ms * 1000);

	*hangup_time = switch_micro_time_now();
	switch_channel_hangup(chans[0], SWITCH_CAUSE_USER_BUSY);

	for (waited = 0; waited < wait_ms && !leg_watch_completes; waited += 50) {
		switch_sleep(50000);
	}

	ok = 1;

  done:

	switch_event_unbind_callback(on_leg_watch);
	*leg_watch_uuid = '\0';
	/* no-op once the queue took ownership and NULLed it */
	switch_event_destroy(&event);

	tear_down_bridged_pair(legs, chans);

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

		FST_TEST_BEGIN(unbridged_originate_peer_hangup_keeps_playback)
		{
			switch_core_session_t *session;
			struct bond_playback_result result;
			int setup;

			fst_requires_module("mod_loopback");
			session = start_bond_peer();
			fst_check(session != NULL);
			if (session) {
				switch_channel_t *channel = switch_core_session_get_channel(session);

				setup = run_unbridged_bond_case(session, SWITCH_ORIGINATE_SIGNAL_BOND_VARIABLE, &result);
				report_bond_playback("unbridged_originate_peer_hangup_keeps_playback", "originate", session, setup, &result);
				fst_check(setup);
				fst_check(result.started_unbridged);
				fst_check(result.hangup_completed_first);
				fst_check(result.stopped != NULL);
				if (result.stopped) {
					fst_check_string_equals(switch_event_get_header(result.stopped, "Playback-Status"), "done");
					fst_check_string_equals(switch_event_get_header(result.stopped, "Playback-Status-Code"), "0");
					fst_check_string_equals(switch_event_get_header(result.stopped, "variable_playback_samples"), "16000");
					fst_check_string_equals(switch_event_get_header(result.stopped, "Hangup-Cause"), NULL);
					switch_event_destroy(&result.stopped);
				}
				fst_check(!switch_channel_get_variable(channel, SWITCH_PLAYBACK_PEER_HANGUP_VARIABLE));
				fst_check(!switch_channel_test_flag(channel, CF_STOP_BROADCAST));
				fst_check(switch_channel_ready(channel));
				switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
				switch_core_session_rwunlock(session);
			}
		}
		FST_TEST_END()

		FST_TEST_BEGIN(unbridged_signal_peer_hangup_keeps_playback)
		{
			switch_core_session_t *session;
			struct bond_playback_result result;
			int setup;

			fst_requires_module("mod_loopback");
			session = start_bond_peer();
			fst_check(session != NULL);
			if (session) {
				switch_channel_t *channel = switch_core_session_get_channel(session);

				setup = run_unbridged_bond_case(session, SWITCH_SIGNAL_BOND_VARIABLE, &result);
				report_bond_playback("unbridged_signal_peer_hangup_keeps_playback", "signal", session, setup, &result);
				fst_check(setup);
				fst_check(result.started_unbridged);
				fst_check(result.hangup_completed_first);
				fst_check(result.stopped != NULL);
				if (result.stopped) {
					fst_check_string_equals(switch_event_get_header(result.stopped, "Playback-Status"), "done");
					fst_check_string_equals(switch_event_get_header(result.stopped, "Playback-Status-Code"), "0");
					fst_check_string_equals(switch_event_get_header(result.stopped, "variable_playback_samples"), "16000");
					fst_check_string_equals(switch_event_get_header(result.stopped, "Hangup-Cause"), NULL);
					switch_event_destroy(&result.stopped);
				}
				fst_check(!switch_channel_get_variable(channel, SWITCH_PLAYBACK_PEER_HANGUP_VARIABLE));
				fst_check(!switch_channel_test_flag(channel, CF_STOP_BROADCAST));
				fst_check(switch_channel_ready(channel));
				switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
				switch_core_session_rwunlock(session);
			}
		}
		FST_TEST_END()

		FST_TEST_BEGIN(broadcast_peer_check_ignores_originate_bond)
		{
			const char *kinds[] = { "absent", "stale", "live", "missing" };
			switch_core_session_t *session;
			struct bond_playback_result result;
			int i, setup;

			fst_requires_module("mod_loopback");
			session = start_bond_peer();
			fst_check(session != NULL);
			if (session) {
				switch_channel_t *channel = switch_core_session_get_channel(session);

				for (i = 0; i < 4; ++i) {
					setup = run_current_bond_case(session, kinds[i], &result);
					report_bond_playback("broadcast_peer_check_ignores_originate_bond", kinds[i], session, setup, &result);
					fst_xcheck(setup, kinds[i]);
					fst_check(result.stopped != NULL);
					if (result.stopped) {
						fst_check_string_equals(switch_event_get_header(result.stopped, "Playback-Status"), i == 3 ? "break" : "done");
						fst_check_string_equals(switch_event_get_header(result.stopped, "Hangup-Cause"), i == 3 ? "NORMAL_CLEARING" : NULL);
						if (i != 3) {
							fst_check_string_equals(switch_event_get_header(result.stopped, "Playback-Status-Code"), "0");
							fst_check_string_equals(switch_event_get_header(result.stopped, "variable_playback_samples"), "1600");
							fst_check(!switch_channel_get_variable(channel, SWITCH_PLAYBACK_PEER_HANGUP_VARIABLE));
						}
						switch_event_destroy(&result.stopped);
					}
					fst_check(switch_channel_ready(channel));
				}
				switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
				switch_core_session_rwunlock(session);
			}
		}
		FST_TEST_END()

		/*
		 * The peer check in the broadcast loop reads a peer that is hanging up
		 * but still exists. The peer names no bond back, so its own hangup state
		 * records nothing, and the cause in the record can only come from that
		 * read. last_bridge_hangup_cause holds another cause.
		 */
		FST_TEST_BEGIN(peer_check_records_the_cause_of_a_peer_that_is_hanging_up)
		{
			switch_core_session_t *session, *peer;
			struct bond_playback_result result;
			int setup;

			fst_requires_module("mod_loopback");
			session = start_bond_peer();
			peer = start_bond_peer();
			fst_check(session != NULL);
			fst_check(peer != NULL);
			if (session && peer) {
				switch_channel_t *channel = switch_core_session_get_channel(session);
				switch_channel_t *peer_channel = switch_core_session_get_channel(peer);
				const char *marker;
				int waited;

				/* the read lock from start_bond_peer() keeps the peer in memory while it hangs up */
				switch_channel_hangup(peer_channel, SWITCH_CAUSE_USER_BUSY);
				for (waited = 0; waited < 5000 && !switch_channel_down_nosig(peer_channel); waited += 50) {
					switch_sleep(50000);
				}
				switch_channel_set_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE, switch_core_session_get_uuid(peer));
				switch_channel_set_variable(channel, "last_bridge_hangup_cause", "CALL_REJECTED");
				switch_channel_set_flag(channel, CF_BRIDGED);
				setup = switch_channel_down_nosig(peer_channel) && run_bond_playback(session, NULL, "silence_stream://2000", &result);
				switch_channel_clear_flag(channel, CF_BRIDGED);
				marker = switch_channel_get_variable(channel, SWITCH_PLAYBACK_PEER_HANGUP_VARIABLE);
				report_bond_playback("peer_check_records_the_cause_of_a_peer_that_is_hanging_up", "hanging", session, setup, &result);
				fst_check(setup);
				fst_check(result.stopped != NULL);
				if (result.stopped) {
					fst_check_string_equals(switch_event_get_header(result.stopped, "Playback-Status"), "break");
					fst_check_string_equals(switch_event_get_header(result.stopped, "Hangup-Cause"), "USER_BUSY");
					switch_event_destroy(&result.stopped);
				}
				fst_check(marker && strlen(marker) > 10 && !strcmp(marker + strlen(marker) - 10, ":USER_BUSY"));
				switch_channel_set_variable(channel, SWITCH_SIGNAL_BOND_VARIABLE, NULL);
				switch_channel_set_variable(channel, "last_bridge_hangup_cause", NULL);
				switch_channel_set_variable(channel, SWITCH_PLAYBACK_PEER_HANGUP_VARIABLE, NULL);
				fst_check(switch_channel_ready(channel));
			}
			if (peer) {
				switch_core_session_rwunlock(peer);
			}
			if (session) {
				switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
				switch_core_session_rwunlock(session);
			}
		}
		FST_TEST_END()

		/* The peer check finds no peer to read, so it records an answered peer, on
		 * which this originator parks. It must keep a record of the current
		 * broadcast: an unanswered peer makes this originator hang up. */
		FST_SESSION_BEGIN(peer_gone_preserves_current_answered_marker)
		{
			const char *kinds[] = { "answered", "unanswered", "stale", "missing" };
			switch_event_t *stopped;
			int i;

			for (i = 0; i < 4; ++i) {
				stopped = run_peer_gone_marker_case(fst_session, kinds[i]);
				fst_check(stopped != NULL);
				if (stopped) {
					fst_check_string_equals(switch_event_get_header(stopped, "Playback-Status"), "break");
					fst_check_string_equals(switch_event_get_header(stopped, "Hangup-Cause"), i == 1 ? "USER_BUSY" : NULL);
					switch_event_destroy(&stopped);
				}
				fst_check(switch_channel_ready(fst_channel));
			}
		}
		FST_SESSION_END()

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
			fst_requires(run_peer_hangup_lead_frames_case(0, 0, &in_window));
			fst_xcheck(in_window, "A-leg: expect the peer hangup to land before CF_BROADCAST is raised");
			fst_xcheck(*playback_watch_status, "A-leg: expect the playback to end once the bridge peer is gone, not after the whole file");
			fst_xcheck(!strcmp(playback_watch_status, "break"), "A-leg: expect PLAYBACK_STOP to report a break, not a file played to the end");
			fst_xcheck(!strcmp(playback_watch_cause, "USER_BUSY"), "A-leg: expect PLAYBACK_STOP to carry the peer's real cause, not the fallback");

			/* direction 2: the bridge originatee carries the broadcast */
			fst_requires(run_peer_hangup_lead_frames_case(1, 0, &in_window));
			fst_xcheck(in_window, "B-leg: expect the peer hangup to land before CF_BROADCAST is raised");
			fst_xcheck(*playback_watch_status, "B-leg: expect the playback to end once the bridge peer is gone, not after the whole file");
			fst_xcheck(!strcmp(playback_watch_status, "break"), "B-leg: expect PLAYBACK_STOP to report a break, not a file played to the end");
			fst_xcheck(!strcmp(playback_watch_cause, "USER_BUSY"), "B-leg: expect PLAYBACK_STOP to carry the peer's real cause, not the fallback");
		}
		FST_TEST_END()

		/*
		 * The other half of the same contract. A playback stop the customer
		 * asked for reports a break too, so the break alone must not be read as
		 * a hangup. Nothing hangs up here: both legs stay up, and the stop is
		 * raised on the leg that is playing.
		 *
		 * This guards the marker's lifetime as much as its set sites. A marker
		 * that is set when no leg went away, or that outlives the playback it
		 * belongs to, shows up here as a cause on a live leg.
		 */
		FST_TEST_BEGIN(customer_stop_during_bridged_broadcast_reports_no_cause)
		{
			int in_window = 0;

			fst_requires_module("mod_commands");
			fst_requires_module("mod_loopback");

			fst_requires(run_peer_hangup_lead_frames_case(0, 1, &in_window));
			fst_xcheck(*playback_watch_status, "A-leg: expect the customer stop to end the playback");
			fst_xcheck(!strcmp(playback_watch_status, "break"), "A-leg: expect a customer stop to report a break");
			fst_xcheck(!*playback_watch_cause, "A-leg: expect no hangup cause on a customer stop, or it reads as a hangup");

			fst_requires(run_peer_hangup_lead_frames_case(1, 1, &in_window));
			fst_xcheck(*playback_watch_status, "B-leg: expect the customer stop to end the playback");
			fst_xcheck(!strcmp(playback_watch_status, "break"), "B-leg: expect a customer stop to report a break");
			fst_xcheck(!*playback_watch_cause, "B-leg: expect no hangup cause on a customer stop, or it reads as a hangup");
		}
		FST_TEST_END()

		/*
		 * The marker has to outlive a nested playback. Hold music is queued on
		 * the priority private queue, which the event-lock of a running
		 * broadcast does not hold shut, so it plays nested inside the playback
		 * the customer started. One peer hangup cuts both short. A marker that
		 * the innermost playback consumes leaves the outer PLAYBACK_STOP, the
		 * one the customer is told about, with no cause at all.
		 */
		FST_TEST_BEGIN(nested_priority_broadcast_reports_peer_hangup_on_both_playbacks)
		{
			fst_requires_module("mod_commands");
			fst_requires_module("mod_loopback");

			fst_requires(run_nested_priority_case());
			fst_xcheck(playback_watch_events == 2, "expect a PLAYBACK_STOP from the nested playback and from the one it nested in");
			fst_xcheck(playback_watch_with_cause == 2, "expect both playbacks to report the peer hangup, not only the innermost");
			fst_xcheck(!strcmp(playback_watch_cause, "USER_BUSY"),
					   "expect the outer playback to carry the peer's real cause, not the fallback");
		}
		FST_TEST_END()

		/*
		 * The nested priority broadcast ends first, then the peer hangs up with
		 * the outer playback still running. The outer playback must still stop
		 * and still report the peer's cause, whatever CF_BROADCAST looks like by
		 * then.
		 */
		FST_TEST_BEGIN(peer_hangup_after_a_nested_broadcast_ends_stops_the_outer_playback)
		{
			char cause[64] = "";
			int flag_down = 0;

			fst_requires_module("mod_commands");
			fst_requires_module("mod_loopback");

			fst_requires(run_nested_completes_first_case(&flag_down, cause, sizeof(cause)));
			fst_xcheck(!strcmp(cause, "USER_BUSY"),
					   "the outer playback must report the peer's cause after a nested broadcast ended");
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
							  "nested cleanup left CF_BROADCAST %s\n", flag_down ? "clear" : "set");
		}
		FST_TEST_END()

		/*
		 * The marker is written by threads that cannot hold the broadcast still
		 * while they write: switch_channel_test_flag() releases flag_mutex
		 * before it returns, and the variable setter takes profile_mutex, so the
		 * check and the write are not one transaction. A setter that resumes
		 * after the broadcast it saw has gone must not leave a marker that a
		 * later playback reads, or a leg that survives on park_after_bridge
		 * reports a hangup for a playback that nothing interrupted.
		 */
		FST_TEST_BEGIN(stale_peer_hangup_marker_is_not_read_by_a_later_playback)
		{
			char cause[64] = "";
			char status[32] = "";

			fst_requires_module("mod_commands");
			fst_requires_module("mod_loopback");

			fst_requires(run_stale_setter_case(0, cause, sizeof(cause), status, sizeof(status)));
			fst_xcheck(!strcmp(status, "done"), "the playback with no stop must reach the end of its file");
			fst_xcheck(!*cause, "a playback that ended on its own must not report a cause from a stale marker");

			/* the bridge thread can consume this break, see run_stale_setter_case() */
			fst_requires(run_stale_setter_case(1, cause, sizeof(cause), status, sizeof(status)));
			fst_xcheck(!strcmp(status, "break") || !strcmp(status, "done"), "expect a PLAYBACK_STOP with a status");
			fst_xcheck(!*cause, "a customer-stopped playback must not report a cause from a stale marker");
		}
		FST_TEST_END()

		/*
		 * The cause a stopped broadcast reports follows the bridge exit of the
		 * side the leg is on. Every row also sets last_bridge_hangup_cause to
		 * CALL_REJECTED, so a row that expects the record's own cause proves the
		 * record wins over that variable.
		 */
		FST_SESSION_BEGIN(broadcast_peer_hangup_cause_follows_the_bridge_exit)
		{
			static const struct exit_row rows[] = {
				{ "originator at the end of its dialplan", 1, ":1:USER_BUSY", NULL, 0, "USER_BUSY" },
				{ "originator that goes on in its dialplan", 1, ":1:USER_BUSY", "hangup_after_bridge=false", 1, NULL },
				{ "originator with hangup_after_bridge", 1, ":1:USER_BUSY", "hangup_after_bridge=true", 1, "USER_BUSY" },
				{ "originator that parks before hangup_after_bridge", 1, ":1:USER_BUSY", "park_after_bridge=true;hangup_after_bridge=true", 0, NULL },
				{ "originator with explicit", 1, ":1:USER_BUSY", "park_after_bridge=true;hangup_after_bridge=explicit", 1, "USER_BUSY" },
				{ "originator with exec_after_bridge_app", 1, ":1:USER_BUSY", "exec_after_bridge_app=park", 0, NULL },
				{ "originatee that parks despite explicit", 0, ":1:USER_BUSY", "park_after_bridge=true;hangup_after_bridge=explicit", 0, NULL },
				{ "originatee that parks after an unanswered peer", 0, ":0:USER_BUSY", "park_after_bridge=true", 0, NULL },
				{ "originatee that transfers", 0, ":1:USER_BUSY", "transfer_after_bridge=1000", 0, NULL },
				{ "originatee is hung up whatever its dialplan", 0, ":1:USER_BUSY", "hangup_after_bridge=false", 1, "USER_BUSY" },
				{ "originatee with park_after_early_bridge only", 0, ":1:USER_BUSY", "park_after_early_bridge=true", 0, "USER_BUSY" },
				{ "originator whose early peer failed", 1, ":0:USER_BUSY", "park_after_bridge=true", 1, "USER_BUSY" },
				{ "originator that continues on fail", 1, ":0:USER_BUSY", "continue_on_fail=true", 1, NULL },
				{ "originator that continues on a listed cause", 1, ":0:USER_BUSY", "continue_on_fail=NORMAL_TEMPORARY_FAILURE,17", 1, NULL },
				{ "originator that does not continue on fail", 1, ":0:USER_BUSY", "continue_on_fail=false", 1, "USER_BUSY" },
				{ "originator whose failure_causes leave the cause out", 1, ":0:USER_BUSY", "failure_causes=NORMAL_CLEARING", 1, NULL },
				{ "originator after no answer", 1, ":0:NO_ANSWER", NULL, 1, NULL },
				{ "originator that parks after an early bridge", 1, ":0:USER_BUSY", "continue_on_fail=true;park_after_early_bridge=true", 0, NULL },
				{ "originator that transfers on fail", 1, ":0:USER_BUSY", "transfer_on_fail=USER_BUSY 1000", 0, NULL },
				{ "zombie originatee", 2, ":1:USER_BUSY", NULL, 0, NULL },
				{ "zombie originator at the end of its dialplan", 3, ":1:USER_BUSY", "hangup_after_bridge=true", 0, "USER_BUSY" },
				{ "zombie originator that goes on in its dialplan", 3, ":1:USER_BUSY", "hangup_after_bridge=true", 1, NULL },
				{ "record without a cause", 1, ":1:", NULL, 0, "CALL_REJECTED" }
			};
			int i, j;

			switch_channel_set_variable(fst_channel, "last_bridge_hangup_cause", "CALL_REJECTED");

			for (i = 0; i < (int) (sizeof(rows) / sizeof(rows[0])); ++i) {
				const struct exit_row *row = &rows[i];
				switch_caller_extension_t *extension = switch_caller_extension_new(fst_session, "exit_row", "exit_row");
				switch_call_cause_t got;

				for (j = 0; j < (int) (sizeof(exit_vars) / sizeof(exit_vars[0])); ++j) {
					switch_channel_set_variable(fst_channel, exit_vars[j], NULL);
				}
				if (row->vars) {
					set_channel_vars(fst_channel, row->vars);
				}
				if (row->next_app) {
					switch_caller_extension_add_application(fst_session, extension, "park", NULL);
				}
				switch_channel_set_caller_extension(fst_channel, extension);
				if (row->leg & 1) {
					switch_channel_set_flag(fst_channel, CF_BRIDGE_ORIGINATOR);
				} else {
					switch_channel_clear_flag(fst_channel, CF_BRIDGE_ORIGINATOR);
				}
				if (row->leg & 2) {
					switch_channel_set_flag(fst_channel, CF_XFER_ZOMBIE);
				} else {
					switch_channel_clear_flag(fst_channel, CF_XFER_ZOMBIE);
				}

				/* a broadcast generation of its own for each row */
				switch_channel_inc_broadcast_generation(fst_channel);
				switch_channel_set_variable_printf(fst_channel, SWITCH_PLAYBACK_PEER_HANGUP_VARIABLE, "%u%s",
												   switch_channel_get_broadcast_generation(fst_channel), row->peer);
				got = switch_channel_get_broadcast_peer_hangup_cause(fst_channel);
				printf("\nEXIT_ROW %s: expect=%s got=%s\n", row->name, row->expect ? row->expect : "none",
					   got == SWITCH_CAUSE_NONE ? "none" : switch_channel_cause2str(got));
				fst_xcheck(row->expect ? got == switch_channel_str2cause(row->expect) : got == SWITCH_CAUSE_NONE, row->name);
			}

			/* the record of a broadcast that has ended is never read */
			switch_channel_inc_broadcast_generation(fst_channel);
			fst_check(switch_channel_get_broadcast_peer_hangup_cause(fst_channel) == SWITCH_CAUSE_NONE);
			switch_channel_clear_flag(fst_channel, CF_BRIDGE_ORIGINATOR);
			switch_channel_clear_flag(fst_channel, CF_XFER_ZOMBIE);
		}
		FST_SESSION_END()

		/*
		 * Every writer takes the same mutex and checks the generation there, so
		 * one that read the generation of a broadcast that has ended since leaves
		 * the record of the current broadcast alone.
		 */
		FST_SESSION_BEGIN(broadcast_peer_hangup_record_belongs_to_its_generation)
		{
			char expected[64];
			uint32_t ended, current;

			switch_channel_inc_broadcast_generation(fst_channel);
			ended = switch_channel_get_broadcast_generation(fst_channel);
			switch_channel_inc_broadcast_generation(fst_channel);
			switch_channel_inc_broadcast_generation(fst_channel);
			current = switch_channel_get_broadcast_generation(fst_channel);

			switch_snprintf(expected, sizeof(expected), "%u:0:USER_BUSY", current);
			switch_channel_set_variable(fst_channel, SWITCH_PLAYBACK_PEER_HANGUP_VARIABLE, expected);

			switch_channel_set_broadcast_peer_hangup(fst_channel, ended, fst_channel);
			fst_check_string_equals(switch_channel_get_variable(fst_channel, SWITCH_PLAYBACK_PEER_HANGUP_VARIABLE), expected);
			switch_channel_set_broadcast_peer_hangup(fst_channel, ended, NULL);
			fst_check_string_equals(switch_channel_get_variable(fst_channel, SWITCH_PLAYBACK_PEER_HANGUP_VARIABLE), expected);

			/* with no peer to read, a record of the current broadcast stays */
			switch_channel_set_broadcast_peer_hangup(fst_channel, current, NULL);
			fst_check_string_equals(switch_channel_get_variable(fst_channel, SWITCH_PLAYBACK_PEER_HANGUP_VARIABLE), expected);

			/* and with no record yet, it records an answered peer */
			switch_channel_set_variable(fst_channel, SWITCH_PLAYBACK_PEER_HANGUP_VARIABLE, NULL);
			switch_channel_set_broadcast_peer_hangup(fst_channel, current, NULL);
			switch_snprintf(expected, sizeof(expected), "%u:1:", current);
			fst_check_string_equals(switch_channel_get_variable(fst_channel, SWITCH_PLAYBACK_PEER_HANGUP_VARIABLE), expected);

			/* a hangup writer records the state of the peer it read; this one is still up, so no cause */
			switch_channel_set_broadcast_peer_hangup(fst_channel, current, fst_channel);
			switch_snprintf(expected, sizeof(expected), "%u:%d:", current, switch_channel_test_flag(fst_channel, CF_ANSWERED) ? 1 : 0);
			fst_check_string_equals(switch_channel_get_variable(fst_channel, SWITCH_PLAYBACK_PEER_HANGUP_VARIABLE), expected);
			switch_channel_set_variable(fst_channel, SWITCH_PLAYBACK_PEER_HANGUP_VARIABLE, NULL);
		}
		FST_SESSION_END()

		/*
		 * A peer hangup ends a whole playback_delimiter list, not only the file
		 * that plays when the peer goes away.
		 */
		FST_TEST_BEGIN(peer_hangup_ends_every_file_of_a_playback_list)
		{
			switch_time_t hangup_time;

			fst_requires_module("mod_commands");
			fst_requires_module("mod_loopback");

			fst_requires(run_originatee_broadcast_case("playback_delimiter=!", "playback", "silence_stream://3000!silence_stream://3000",
													   NULL, 1, 500, 6000, &hangup_time));
			fst_xcheck(leg_watch_starts == 1, "expect no second file to start after the peer hangup");
			fst_xcheck(leg_watch_stops == 1, "expect one PLAYBACK_STOP, for the file the hangup cut short");
			fst_xcheck(!strcmp(leg_watch_stop_cause, "USER_BUSY"), "expect the stop to carry the peer's cause");
		}
		FST_TEST_END()

		/*
		 * A peer hangup in the second of three broadcast loops ends the loops and
		 * reports the peer's cause for that iteration.
		 */
		FST_TEST_BEGIN(peer_hangup_in_a_later_loop_ends_the_broadcast)
		{
			switch_time_t hangup_time;

			fst_requires_module("mod_commands");
			fst_requires_module("mod_loopback");

			fst_requires(run_originatee_broadcast_case(NULL, "playback", "silence_stream://1500", "3", 2, 300, 4000, &hangup_time));
			fst_xcheck(leg_watch_starts == 2, "expect no third loop after the peer hangup");
			fst_xcheck(leg_watch_stops == 2, "expect the first loop to end and the second to be cut short");
			fst_xcheck(!strcmp(leg_watch_stop_cause, "USER_BUSY"), "expect the cut loop to carry the peer's cause");
		}
		FST_TEST_END()

		/*
		 * play_and_get_digits waits for digits after its prompt and ignores the
		 * break. On a leg that goes away with its bridge, a peer hangup must end
		 * that wait at once and report the peer's cause on the
		 * CHANNEL_EXECUTE_COMPLETE, the event the end of a gather is read from.
		 * On a leg that outlives its bridge the gather keeps running.
		 */
		FST_TEST_BEGIN(peer_hangup_ends_the_digit_wait_of_a_leg_that_goes_away)
		{
			switch_time_t hangup_time;
			const char *gather = "1 1 1 8000 # silence_stream://300 silence_stream://300 gather_digits \\d+";

			fst_requires_module("mod_commands");
			fst_requires_module("mod_loopback");

			fst_requires(run_originatee_broadcast_case(NULL, "play_and_get_digits", gather, NULL, 1, 1000, 12000, &hangup_time));
			fst_xcheck(leg_watch_completes == 1, "expect the gather to end");
			fst_xcheck(leg_watch_complete_time && leg_watch_complete_time - hangup_time < 3000000,
					   "expect the gather to end at the peer hangup, not after its digit timeout");
			fst_xcheck(!strcmp(leg_watch_complete_cause, "USER_BUSY"), "expect the gather end to carry the peer's cause");

			fst_requires(run_originatee_broadcast_case("park_after_bridge=true", "play_and_get_digits",
													   "1 1 1 6000 # silence_stream://300 silence_stream://300 gather_digits \\d+",
													   NULL, 1, 1000, 12000, &hangup_time));
			fst_xcheck(leg_watch_completes == 1, "expect the gather of the surviving leg to end");
			fst_xcheck(leg_watch_complete_time && leg_watch_complete_time - hangup_time >= 2000000,
					   "expect the gather of the surviving leg to wait for digits after the peer hangup");
			fst_xcheck(!*leg_watch_complete_cause, "expect no cause on the gather of a leg that outlives its bridge");
		}
		FST_TEST_END()

		/*
		 * A playback on a held leg waits while the hold lasts. A break must end
		 * that wait, or the stop of a peer hangup waits for the hold to end.
		 */
		FST_SESSION_BEGIN(break_ends_a_playback_on_hold)
		{
			switch_stream_handle_t stream = { 0 };
			char cmd[256];
			switch_status_t status;

			fst_requires_module("mod_commands");

			/* held as switch_ivr_hold() leaves a leg; the call state lets uuid_hold off end the hold */
			switch_channel_set_flag(fst_channel, CF_HOLD);
			switch_channel_set_callstate(fst_channel, CCS_HELD);

			/* the break in 1 s, and the end of the hold in 6 s so that a regressed playback ends too */
			SWITCH_STANDARD_STREAM(stream);
			switch_snprintf(cmd, sizeof(cmd), "+1 none uuid_break %s", switch_core_session_get_uuid(fst_session));
			fst_requires(switch_api_execute("sched_api", cmd, NULL, &stream) == SWITCH_STATUS_SUCCESS);
			switch_snprintf(cmd, sizeof(cmd), "+6 none uuid_hold off %s", switch_core_session_get_uuid(fst_session));
			fst_requires(switch_api_execute("sched_api", cmd, NULL, &stream) == SWITCH_STATUS_SUCCESS);
			switch_safe_free(stream.data);

			fst_time_mark();
			status = switch_ivr_play_file(fst_session, NULL, "silence_stream://20000", NULL);
			fst_check(status == SWITCH_STATUS_BREAK);
			fst_check_duration(1500, 1000);

			switch_channel_clear_flag(fst_channel, CF_HOLD);
		}
		FST_SESSION_END()

		/*
		 * A recovered session starts its broadcast generations again, so a peer
		 * hangup record copied from the old session could match a new broadcast.
		 */
		FST_TEST_BEGIN(recovered_session_drops_the_peer_hangup_record)
		{
			switch_endpoint_interface_t *endpoint = NULL;
			switch_core_session_t *session = NULL;
			switch_xml_t xml = NULL;
			char uuid[SWITCH_UUID_FORMATTED_LENGTH + 1] = "";
			char *body = NULL;

			fst_requires_module("mod_loopback");

			switch_uuid_str(uuid, sizeof(uuid));
			body = switch_mprintf("<xml><variables><uuid>%s</uuid><recovery_probe>kept</recovery_probe>"
								  "<playback_peer_hangup>1%%3A1%%3AUSER_BUSY</playback_peer_hangup></variables>"
								  "<callflow><caller_profile><username>t</username><dialplan>XML</dialplan>"
								  "<caller_id_name>t</caller_id_name><caller_id_number>1</caller_id_number>"
								  "<network_addr>127.0.0.1</network_addr><ani>1</ani><destination_number>1</destination_number>"
								  "<context>default</context></caller_profile></callflow></xml>", uuid);
			xml = switch_xml_parse_str_dynamic(body, SWITCH_TRUE);
			endpoint = switch_loadable_module_get_endpoint_interface("null");
			fst_check(xml != NULL);
			fst_check(endpoint != NULL);
			if (xml && endpoint) {
				session = switch_core_session_request_xml(endpoint, NULL, xml);
			}
			fst_check(session != NULL);
			if (session) {
				switch_channel_t *channel = switch_core_session_get_channel(session);

				/* the restore ran, and dropped the record of the old session */
				fst_check_string_equals(switch_channel_get_variable(channel, "recovery_probe"), "kept");
				fst_check(!switch_channel_get_variable(channel, SWITCH_PLAYBACK_PEER_HANGUP_VARIABLE));
				switch_core_session_destroy(&session);
			}
			UNPROTECT_INTERFACE(endpoint);
			switch_xml_free(xml);
			switch_safe_free(body);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()

