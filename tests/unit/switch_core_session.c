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

#define PRIVQ_MAX_SEEN 32

static char *privq_media_cmds[] = { "playback", "speak", "say" };
static switch_mutex_t *privq_mutex = NULL;
static char privq_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1];
static char privq_seen[PRIVQ_MAX_SEEN][64];
static int privq_seen_count = 0;

static void privq_on_interrupted(switch_event_t *event)
{
	const char *uuid = switch_event_get_header(event, "Unique-ID");
	const char *arg = switch_event_get_header(event, "execute-app-arg");

	if (!uuid || strcmp(uuid, privq_uuid)) {
		return;
	}

	switch_mutex_lock(privq_mutex);
	if (privq_seen_count < PRIVQ_MAX_SEEN) {
		switch_snprintf(privq_seen[privq_seen_count], sizeof(privq_seen[0]), "%s", switch_str_nil(arg));
	}
	privq_seen_count++;
	switch_mutex_unlock(privq_mutex);
}

static void privq_reset(void)
{
	switch_mutex_lock(privq_mutex);
	privq_seen_count = 0;
	switch_mutex_unlock(privq_mutex);
}

static switch_status_t privq_start(switch_core_session_t *session, switch_memory_pool_t *pool)
{
	if (switch_mutex_init(&privq_mutex, SWITCH_MUTEX_NESTED, pool) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}
	switch_snprintf(privq_uuid, sizeof(privq_uuid), "%s", switch_core_session_get_uuid(session));
	privq_seen_count = 0;
	return switch_event_bind("privq_test", SWITCH_EVENT_COMMAND_INTERRUPTED, SWITCH_EVENT_SUBCLASS_ANY, privq_on_interrupted, NULL);
}

static void privq_stop(void)
{
	switch_event_unbind_callback(privq_on_interrupted);
}

/* waits up to 5 s for the interrupt events to be delivered, then a little longer to catch any extra ones */
static int privq_wait(int want)
{
	int i, count = 0;

	for (i = 0; i < 500; i++) {
		switch_mutex_lock(privq_mutex);
		count = privq_seen_count;
		switch_mutex_unlock(privq_mutex);
		if (count >= want) {
			break;
		}
		switch_yield(10000);
	}
	switch_yield(100000);
	switch_mutex_lock(privq_mutex);
	count = privq_seen_count;
	switch_mutex_unlock(privq_mutex);
	return count;
}

static int privq_seen_times(const char *arg)
{
	int i, n = 0;

	switch_mutex_lock(privq_mutex);
	for (i = 0; i < privq_seen_count && i < PRIVQ_MAX_SEEN; i++) {
		if (!strcmp(privq_seen[i], arg)) {
			n++;
		}
	}
	switch_mutex_unlock(privq_mutex);
	return n;
}

static switch_status_t privq_queue(switch_core_session_t *session, const char *command, const char *app_name, const char *app_arg, switch_bool_t priority)
{
	switch_event_t *event = NULL;

	if (switch_event_create(&event, SWITCH_EVENT_COMMAND) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}
	if (command) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "call-command", command);
	}
	if (app_name) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-name", app_name);
	}
	if (app_arg) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-arg", app_arg);
	}
	if (switch_core_session_queue_private_event(session, &event, priority) != SWITCH_STATUS_SUCCESS) {
		if (event) {
			switch_event_destroy(&event);
		}
		return SWITCH_STATUS_FALSE;
	}
	return SWITCH_STATUS_SUCCESS;
}

static void privq_reached(const char *test)
{
	fprintf(stderr, "privq: %s reached the media flush\n", test);
	fflush(stderr);
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

		/*
		 * The event locks keep the session thread from consuming the queued commands, so only the flushes below
		 * touch them. The media flush ignores the locks.
		 */
		FST_SESSION_BEGIN(private_media_flush_keeps_headerless_priority_commands)
		{
			uint32_t removed, kept;

			switch_channel_set_flag(fst_channel, CF_EVENT_LOCK_PRI);
			switch_channel_set_flag(fst_channel, CF_EVENT_LOCK);
			fst_requires(privq_start(fst_session, fst_pool) == SWITCH_STATUS_SUCCESS);

			fst_check(privq_queue(fst_session, "hangup", NULL, "pri-hangup", SWITCH_TRUE) == SWITCH_STATUS_SUCCESS);
			fst_check(privq_queue(fst_session, "nomedia", NULL, NULL, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS);
			fst_check(privq_queue(fst_session, "execute", NULL, "pri-execute-no-name", SWITCH_TRUE) == SWITCH_STATUS_SUCCESS);
			fst_check(privq_queue(fst_session, "execute", "", "pri-execute-empty-name", SWITCH_TRUE) == SWITCH_STATUS_SUCCESS);
			fst_check(privq_queue(fst_session, "execute", "playback", "pri-playback", SWITCH_TRUE) == SWITCH_STATUS_SUCCESS);

			privq_reached("priority");
			removed = switch_core_session_flush_and_publish_private_media_events(fst_session, privq_media_cmds, 3);
			fst_check(removed == 1);
			fst_check(privq_wait(1) == 1);
			fst_check(privq_seen_times("pri-playback") == 1);

			privq_reset();
			kept = switch_core_session_flush_and_publish_private_events(fst_session);
			fst_check(kept == 4);
			fst_check(privq_wait(4) == 4);
			fst_check(privq_seen_times("pri-hangup") == 1);
			fst_check(privq_seen_times("") == 1);
			fst_check(privq_seen_times("pri-execute-no-name") == 1);
			fst_check(privq_seen_times("pri-execute-empty-name") == 1);

			privq_stop();
			switch_channel_clear_flag(fst_channel, CF_EVENT_LOCK);
			switch_channel_clear_flag(fst_channel, CF_EVENT_LOCK_PRI);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(private_media_flush_keeps_headerless_normal_commands)
		{
			uint32_t removed, kept;

			switch_channel_set_flag(fst_channel, CF_EVENT_LOCK_PRI);
			switch_channel_set_flag(fst_channel, CF_EVENT_LOCK);
			fst_requires(privq_start(fst_session, fst_pool) == SWITCH_STATUS_SUCCESS);

			fst_check(privq_queue(fst_session, "unicast", NULL, "norm-unicast", SWITCH_FALSE) == SWITCH_STATUS_SUCCESS);
			fst_check(privq_queue(fst_session, "execute", "SPEAK", "norm-speak", SWITCH_FALSE) == SWITCH_STATUS_SUCCESS);
			fst_check(privq_queue(fst_session, "xferext", NULL, "norm-xferext", SWITCH_FALSE) == SWITCH_STATUS_SUCCESS);
			fst_check(privq_queue(fst_session, "execute", "set", "norm-set", SWITCH_FALSE) == SWITCH_STATUS_SUCCESS);
			fst_check(privq_queue(fst_session, "hangup", NULL, "norm-hangup", SWITCH_FALSE) == SWITCH_STATUS_SUCCESS);
			fst_check(privq_queue(fst_session, "execute", "Say", "norm-say", SWITCH_FALSE) == SWITCH_STATUS_SUCCESS);

			privq_reached("normal");
			removed = switch_core_session_flush_and_publish_private_media_events(fst_session, privq_media_cmds, 3);
			fst_check(removed == 2);
			fst_check(privq_wait(2) == 2);
			fst_check(privq_seen_times("norm-speak") == 1);
			fst_check(privq_seen_times("norm-say") == 1);

			privq_reset();
			kept = switch_core_session_flush_and_publish_private_events(fst_session);
			fst_check(kept == 4);
			fst_check(privq_wait(4) == 4);
			fst_check(privq_seen_times("norm-unicast") == 1);
			fst_check(privq_seen_times("norm-xferext") == 1);
			fst_check(privq_seen_times("norm-set") == 1);
			fst_check(privq_seen_times("norm-hangup") == 1);

			privq_stop();
			switch_channel_clear_flag(fst_channel, CF_EVENT_LOCK);
			switch_channel_clear_flag(fst_channel, CF_EVENT_LOCK_PRI);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(private_media_flush_without_commands_keeps_everything)
		{
			uint32_t removed, kept;

			switch_channel_set_flag(fst_channel, CF_EVENT_LOCK_PRI);
			switch_channel_set_flag(fst_channel, CF_EVENT_LOCK);
			fst_requires(privq_start(fst_session, fst_pool) == SWITCH_STATUS_SUCCESS);

			fst_check(privq_queue(fst_session, "hangup", NULL, "ctl-hangup", SWITCH_TRUE) == SWITCH_STATUS_SUCCESS);
			fst_check(privq_queue(fst_session, "execute", "playback", "ctl-pri-playback", SWITCH_TRUE) == SWITCH_STATUS_SUCCESS);
			fst_check(privq_queue(fst_session, "nomedia", NULL, "ctl-nomedia", SWITCH_FALSE) == SWITCH_STATUS_SUCCESS);
			fst_check(privq_queue(fst_session, "execute", "playback", "ctl-playback", SWITCH_FALSE) == SWITCH_STATUS_SUCCESS);

			removed = switch_core_session_flush_and_publish_private_media_events(fst_session, privq_media_cmds, 0);
			fst_check(removed == 0);
			fst_check(privq_wait(1) == 0);

			kept = switch_core_session_flush_and_publish_private_events(fst_session);
			fst_check(kept == 4);
			fst_check(privq_wait(4) == 4);
			fst_check(privq_seen_times("ctl-hangup") == 1);
			fst_check(privq_seen_times("ctl-pri-playback") == 1);
			fst_check(privq_seen_times("ctl-nomedia") == 1);
			fst_check(privq_seen_times("ctl-playback") == 1);

			privq_stop();
			switch_channel_clear_flag(fst_channel, CF_EVENT_LOCK);
			switch_channel_clear_flag(fst_channel, CF_EVENT_LOCK_PRI);
		}
		FST_SESSION_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
