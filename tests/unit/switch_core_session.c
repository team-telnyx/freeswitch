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
 * switch_core_session.c -- tests sessions
 *
 */
#include <switch.h>
#include <test/switch_test.h>

/* Counts BLIND_TRANSFER_RESPONSE messages delivered to the parked peer. The hook
   writes numeric_arg before the atomic increment, which publishes it to the reader. */
static switch_atomic_t blind_transfer_response_count;
static volatile int blind_transfer_response_numeric_arg = -1;

static switch_status_t blind_transfer_response_hook(switch_core_session_t *session, switch_core_session_message_t *msg)
{
	if (msg->message_id == SWITCH_MESSAGE_INDICATE_BLIND_TRANSFER_RESPONSE) {
		blind_transfer_response_numeric_arg = msg->numeric_arg;
		switch_atomic_inc(&blind_transfer_response_count);
	}
	return SWITCH_STATUS_SUCCESS;
}

/* A null-endpoint session stands in for the parked transferor (the REFER sender). */
static switch_core_session_t *start_peer(void)
{
	switch_core_session_t *peer_session = NULL;
	switch_call_cause_t cause = SWITCH_CAUSE_NONE;

	if (switch_ivr_originate(NULL, &peer_session, &cause, "null/+15553334444",
							 0, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS) {
		return NULL;
	}
	switch_atomic_set(&blind_transfer_response_count, 0);
	blind_transfer_response_numeric_arg = -1;
	switch_core_event_hook_add_receive_message(peer_session, blind_transfer_response_hook);
	return peer_session;
}

/* Waits up to about 5 s for the first response to reach the peer. */
static void wait_for_response(void)
{
	int i;

	for (i = 0; i < 500 && switch_atomic_read(&blind_transfer_response_count) == 0; ++i) {
		switch_yield(10000);
	}
}

/* Waits up to about 5 s for the hung-up leg to finish its hangup state, where the
   teardown producer runs, so that every response it could send has arrived.
   Returns SWITCH_FALSE on timeout: the counts are then not final. */
static switch_bool_t wait_for_destroy(switch_channel_t *channel)
{
	int i;

	for (i = 0; i < 500 && switch_channel_get_state(channel) != CS_DESTROY; ++i) {
		switch_yield(10000);
	}
	if (switch_channel_get_state(channel) != CS_DESTROY) {
		return SWITCH_FALSE;
	}
	switch_yield(100000);
	return SWITCH_TRUE;
}

/* Removes the hook and ends the peer. */
static void stop_peer(switch_core_session_t *peer_session)
{
	switch_core_event_hook_remove_receive_message(peer_session, blind_transfer_response_hook);
	switch_channel_hangup(switch_core_session_get_channel(peer_session), SWITCH_CAUSE_NORMAL_CLEARING);
	switch_core_session_rwunlock(peer_session);
}

/* the null endpoint's read_frame, replaced for a read that returns one large frame first */
static switch_io_read_frame_t null_read_frame = NULL;
static switch_frame_t *large_frame = NULL;

static switch_status_t large_frame_read(switch_core_session_t *session, switch_frame_t **frame, switch_io_flag_t flags, int stream_id)
{
	if (large_frame) {
		*frame = large_frame;
		large_frame = NULL;
		return SWITCH_STATUS_SUCCESS;
	}

	return null_read_frame(session, frame, flags, stream_id);
}


FST_CORE_BEGIN("./conf")
{
	FST_SUITE_BEGIN(switch_core_session)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_dptools");
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		FST_SESSION_BEGIN(session_external_id)
		{
			switch_core_session_t *session;
			fst_check(switch_core_session_set_external_id(fst_session, switch_core_session_get_uuid(fst_session)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(switch_core_session_get_external_id(fst_session), switch_core_session_get_uuid(fst_session));
			fst_check(switch_core_session_set_external_id(fst_session, "foo") == SWITCH_STATUS_SUCCESS);
			session = switch_core_session_locate("foo");
			fst_requires(session);
			fst_check_string_equals(switch_core_session_get_uuid(session), switch_core_session_get_uuid(fst_session));
			fst_check_string_equals(switch_core_session_get_external_id(session), "foo");
			fst_check(switch_core_session_set_external_id(fst_session, "bar") == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(switch_core_session_get_external_id(session), "bar");
			fst_requires(switch_core_session_locate("foo") == NULL);
			switch_core_session_rwunlock(session);
			session = switch_core_session_locate("bar");
			fst_requires(session);
			switch_core_session_rwunlock(session);
			session = switch_core_session_locate(switch_core_session_get_uuid(fst_session));
			fst_requires(session);
			switch_core_session_rwunlock(session);
			switch_channel_hangup(fst_channel, SWITCH_CAUSE_NORMAL_CLEARING);
			session = switch_core_session_locate("bar");
			fst_check(session == NULL);
		}
		FST_SESSION_END()

		/*
		 * exec_clears_break_direct_path: switch_core_session_exec (the
		 * direct/dialplan wrapper) must clear CF_BREAK before running the
		 * app -- this is the original behaviour, preserved by the exec_impl
		 * refactor. If this regresses, a stale break from a prior app in a
		 * dialplan chain kills the next one.
		 */
		FST_SESSION_BEGIN(exec_clears_break_direct_path)
		{
			switch_application_interface_t *app_interface =
				switch_loadable_module_get_application_interface("set");
			fst_requires(app_interface);

			switch_channel_set_flag(fst_channel, CF_BREAK);
			switch_core_session_exec(fst_session, app_interface, "break_test_a=1");
			fst_check(!switch_channel_test_flag(fst_channel, CF_BREAK));
			/* the app must actually have run, not just left the flag alone */
			fst_check_string_equals(switch_str_nil(switch_channel_get_variable(fst_channel, "break_test_a")), "1");

			UNPROTECT_INTERFACE(app_interface);
		}
		FST_SESSION_END()

		/*
		 * exec_preserves_break_event_path: switch_core_session_execute_application_event
		 * (used when a command was dequeued from a private event, e.g.
		 * uuid_broadcast / ESL sendmsg) must NOT clear CF_BREAK. A break
		 * raised after the event was queued (e.g. telnyx_break during the
		 * lead-frames wait or file-open) must survive into the app.
		 */
		FST_SESSION_BEGIN(exec_preserves_break_event_path)
		{
			switch_status_t status;

			/*
			 * Value 2 specifically: telnyx_break/uuid_break use it for
			 * "all"/"media" and switch_ivr_play_file only ends a whole
			 * delimited file list when it sees 2. Preserving the flag but
			 * downgrading it to 1 would silently change that, so assert the
			 * value rather than mere truthiness.
			 */
			switch_channel_set_flag_value(fst_channel, CF_BREAK, 2);
			status = switch_core_session_execute_application_event(fst_session, "set", "break_test_b=2");
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_test_flag(fst_channel, CF_BREAK) == 2);
			/* and the preserved break must not have suppressed the app itself */
			fst_check_string_equals(switch_str_nil(switch_channel_get_variable(fst_channel, "break_test_b")), "2");

			/* don't leave a live break feeding the read loop at teardown */
			switch_channel_clear_flag(fst_channel, CF_BREAK);
		}
		FST_SESSION_END()

		/*
		 * exec_get_flags_clears_break: the direct-path execute_application_get_flags
		 * wrapper must still clear CF_BREAK -- it delegates to the same impl with
		 * clear_break=TRUE, preserving the original behaviour for all non-event
		 * callers (dialplan, phrase, menu, httapi, etc.).
		 */
		FST_SESSION_BEGIN(exec_get_flags_clears_break)
		{
			switch_status_t status;

			switch_channel_set_flag(fst_channel, CF_BREAK);
			status = switch_core_session_execute_application_get_flags(fst_session, "set", "break_test_c=3", NULL);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check(!switch_channel_test_flag(fst_channel, CF_BREAK));
			fst_check_string_equals(switch_str_nil(switch_channel_get_variable(fst_channel, "break_test_c")), "3");
		}
		FST_SESSION_END()

		/*
		 * nested_chain_does_not_leak_break: simulates an app chain
		 * (execute_extension / lua / phrase) where each app is dispatched via
		 * the direct exec path. A break set during one app must be cleared
		 * before the next app runs, regardless of CF_EVENT_PARSE refcount.
		 * This is why C2 threads clear_break as a function parameter rather
		 * than keying on the channel-global CF_EVENT_PARSE flag.
		 */
		FST_SESSION_BEGIN(nested_chain_does_not_leak_break)
		{
			switch_application_interface_t *app_interface =
				switch_loadable_module_get_application_interface("set");
			fst_requires(app_interface);

			/*
			 * Stand in for being inside switch_ivr_parse_event executing a
			 * queued command: CF_EVENT_PARSE is set on the channel for the
			 * whole of that window, including any app the queued app itself
			 * dispatches. Those nested apps go through the direct path and
			 * must STILL clear CF_BREAK, or one break would skip every
			 * remaining app in the chain. Deciding this from CF_EVENT_PARSE
			 * rather than the clear_break parameter fails here.
			 */
			switch_channel_set_flag_recursive(fst_channel, CF_EVENT_PARSE);

			/* first app in chain -- break set, then cleared by exec */
			switch_channel_set_flag(fst_channel, CF_BREAK);
			switch_core_session_exec(fst_session, app_interface, "chain_a=1");
			fst_check(!switch_channel_test_flag(fst_channel, CF_BREAK));
			fst_check_string_equals(switch_str_nil(switch_channel_get_variable(fst_channel, "chain_a")), "1");

			/* second app in chain -- independently set and cleared */
			switch_channel_set_flag(fst_channel, CF_BREAK);
			switch_core_session_exec(fst_session, app_interface, "chain_b=2");
			fst_check(!switch_channel_test_flag(fst_channel, CF_BREAK));
			fst_check_string_equals(switch_str_nil(switch_channel_get_variable(fst_channel, "chain_b")), "2");

			switch_channel_clear_flag_recursive(fst_channel, CF_EVENT_PARSE);

			UNPROTECT_INTERFACE(app_interface);
		}
		FST_SESSION_END()

		/*
		 * parse_event_clears_stale_break_flags: switch_ivr_parse_event must
		 * clear stale CF_STOP_BROADCAST and CF_BREAK at the top (before the
		 * lead-frames wait) for CMD_EXECUTE commands. This covers the ESL
		 * sync-sendmsg path, where mod_event_socket calls parse_event directly
		 * -- siting the clear in dequeue_private_event instead would leave
		 * CF_STOP_BROADCAST permanently sticky and wedge the session.
		 */
		FST_SESSION_BEGIN(parse_event_clears_stale_break_flags)
		{
			switch_event_t *event = NULL;
			switch_status_t status;

			fst_requires(switch_event_create(&event, SWITCH_EVENT_COMMAND) == SWITCH_STATUS_SUCCESS);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "call-command", "execute");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-name", "set");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-arg", "parse_event_test=1");

			/* set stale flags that should be cleared at the top of parse_event */
			switch_channel_set_flag(fst_channel, CF_STOP_BROADCAST);
			switch_channel_set_flag(fst_channel, CF_BREAK);

			status = switch_ivr_parse_event(fst_session, event);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			fst_check(!switch_channel_test_flag(fst_channel, CF_STOP_BROADCAST));
			fst_check(!switch_channel_test_flag(fst_channel, CF_BREAK));
			fst_check_string_equals(switch_str_nil(switch_channel_get_variable(fst_channel, "parse_event_test")), "1");

			switch_event_destroy(&event);

			/*
			 * Second command on the same session, with both flags raised again
			 * in between. This is the ESL sync-sendmsg failure mode: siting the
			 * clear in dequeue_private_event would never run here (parse_event
			 * is called directly, nothing is dequeued), leaving
			 * CF_STOP_BROADCAST sticky and breaking every later command.
			 */
			fst_requires(switch_event_create(&event, SWITCH_EVENT_COMMAND) == SWITCH_STATUS_SUCCESS);
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "call-command", "execute");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-name", "set");
			switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-arg", "parse_event_test2=2");

			switch_channel_set_flag(fst_channel, CF_STOP_BROADCAST);
			switch_channel_set_flag(fst_channel, CF_BREAK);

			status = switch_ivr_parse_event(fst_session, event);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			fst_check(!switch_channel_test_flag(fst_channel, CF_STOP_BROADCAST));
			fst_check(!switch_channel_test_flag(fst_channel, CF_BREAK));
			fst_check_string_equals(switch_str_nil(switch_channel_get_variable(fst_channel, "parse_event_test2")), "2");

			switch_event_destroy(&event);
		}
		FST_SESSION_END()

		/* The flag-holding leg hangs up mid-transfer: the parked peer gets one failure response. */
		FST_SESSION_BEGIN(blind_transfer_response_on_hangup)
		{
			switch_core_session_t *peer_session = start_peer();

			fst_requires(peer_session);
			/* Set the flag as the REFER handler does: a state flag that becomes a
			   channel flag at the next state change, here the change to CS_HANGUP. */
			switch_channel_set_state_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER);
			switch_channel_set_variable(fst_channel, "blind_transfer_uuid", switch_core_session_get_uuid(peer_session));

			switch_channel_hangup(fst_channel, SWITCH_CAUSE_NORMAL_CLEARING);
			fst_xcheck(wait_for_destroy(fst_channel), "hung-up leg reached CS_DESTROY");

			fst_xcheck(switch_atomic_read(&blind_transfer_response_count) == 1, "one failure response delivered to the peer on hangup");
			fst_xcheck(blind_transfer_response_numeric_arg == 0, "failure response carries numeric_arg=0");
			fst_xcheck(!switch_channel_test_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER), "flag cleared after the response");
			stop_peer(peer_session);
		}
		FST_SESSION_END()

		/* After an explicit ack, hangup must not send a second response. */
		FST_SESSION_BEGIN(blind_transfer_ack_suppresses_hangup_producer)
		{
			switch_core_session_t *peer_session = start_peer();

			fst_requires(peer_session);
			switch_channel_set_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER);
			switch_channel_set_variable(fst_channel, "blind_transfer_uuid", switch_core_session_get_uuid(peer_session));

			fst_check(switch_ivr_blind_transfer_ack(fst_session, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS);
			wait_for_response();
			fst_xcheck(switch_atomic_read(&blind_transfer_response_count) == 1, "one success response delivered to the peer");
			fst_xcheck(blind_transfer_response_numeric_arg == 1, "success response carries numeric_arg=1");
			fst_xcheck(!switch_channel_test_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER), "flag cleared by the explicit ack");

			switch_channel_hangup(fst_channel, SWITCH_CAUSE_NORMAL_CLEARING);
			fst_xcheck(wait_for_destroy(fst_channel), "hung-up leg reached CS_DESTROY");
			fst_xcheck(switch_atomic_read(&blind_transfer_response_count) == 1, "no second response after the explicit ack");
			stop_peer(peer_session);
		}
		FST_SESSION_END()

		/*
		 * A frame that decodes to more audio than one packet, read through a read codec at a higher rate
		 * (a conference pushes one at its own rate): upsampled it no longer fits the frame it was decoded
		 * into, and must reach the read buffer without overrunning it. 8000 bytes of L16 at 8 kHz become
		 * 48000 at 48 kHz, past the session's read buffers.
		 */
		FST_SESSION_BEGIN(read_frame_upsampled_past_the_frame_is_buffered)
		{
			switch_codec_t codec_8k = { 0 }, codec_48k = { 0 };
			switch_endpoint_interface_t *endpoint;
			switch_frame_t frame = { 0 }, *read_frame = NULL;
			switch_log_level_t loglevel = switch_core_session_get_loglevel(fst_session);
			int16_t *samples;
			switch_status_t status;
			int i;

			fst_requires(switch_core_codec_init(&codec_8k, "L16", NULL, NULL, 8000, 20, 1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
												NULL, switch_core_session_get_pool(fst_session)) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_codec_init(&codec_48k, "L16", NULL, NULL, 48000, 20, 1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
												NULL, switch_core_session_get_pool(fst_session)) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_session_set_read_codec(fst_session, &codec_48k) == SWITCH_STATUS_SUCCESS);
			fst_requires((endpoint = switch_loadable_module_get_endpoint_interface("null")));
			fst_requires(endpoint->io_routines && endpoint->io_routines->read_frame);

			samples = switch_core_session_alloc(fst_session, 8000);
			for (i = 0; i < 4000; i++) {
				samples[i] = 0x4141;
			}
			frame.codec = &codec_8k;
			frame.data = samples;
			frame.datalen = frame.buflen = 8000;
			frame.samples = 4000;
			frame.rate = 8000;
			frame.channels = 1;

			null_read_frame = endpoint->io_routines->read_frame;
			endpoint->io_routines->read_frame = large_frame_read;
			large_frame = &frame;

			/* the first packet of the 48000 bytes (25 packets of 1920 at 48 kHz) */
			status = switch_core_session_read_frame(fst_session, &read_frame, SWITCH_IO_FLAG_NONE, 0);

			endpoint->io_routines->read_frame = null_read_frame;
			large_frame = NULL;
			UNPROTECT_INTERFACE(endpoint);

			/* nothing past the read buffers was written over */
			fst_check(switch_core_session_get_loglevel(fst_session) == loglevel);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			if (status == SWITCH_STATUS_SUCCESS && read_frame) {
				int16_t v = ((int16_t *) read_frame->data)[480];

				fst_check(read_frame->datalen == 1920);
				/* past the resampler's start-up, the constant */
				fst_check(read_frame->datalen != 1920 || (v > 0x4141 - 64 && v < 0x4141 + 64));
			}

			switch_core_session_set_read_codec(fst_session, NULL);
			switch_core_codec_destroy(&codec_48k);
			switch_core_codec_destroy(&codec_8k);
		}
		FST_SESSION_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
