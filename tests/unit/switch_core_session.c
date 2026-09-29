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

/*
 * TELCORE-564: observe SWITCH_MESSAGE_INDICATE_BLIND_TRANSFER_RESPONSE
 * delivered to the parked REFER sender. A receive_message event hook on the
 * REFER sender's session fires after the endpoint has processed the
 * message, so counting there catches every producer (bridge, answer-time,
 * state-machine failure and blind_transfer_ack).
 */
static int bt_response_count = 0;
static int bt_response_numeric_arg = -1;

static switch_status_t bt_response_hook(switch_core_session_t *session, switch_core_session_message_t *message)
{
	if (message->message_id == SWITCH_MESSAGE_INDICATE_BLIND_TRANSFER_RESPONSE) {
		bt_response_count++;
		bt_response_numeric_arg = message->numeric_arg;
	}

	return SWITCH_STATUS_SUCCESS;
}

/* originate an extra null-endpoint session (returns read-locked, like switch_ivr_originate itself) */
static switch_core_session_t *bt_originate_null(void)
{
	switch_core_session_t *session = NULL;
	switch_call_cause_t cause;

	if (switch_ivr_originate(NULL, &session, &cause, "null/+15553334444", 2,
							 NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS || !session) {
		return NULL;
	}

	return session;
}

static void bt_release_session(switch_core_session_t *session)
{
	if (session) {
		switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
	}
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
		 * test_and_clear_flag_semantics: the atomic helper must report the
		 * previous value and clear the flag in one step, so that competing
		 * producers of the blind transfer outcome cannot both believe they
		 * won (a second terminated NOTIFY would be a protocol error).
		 */
		FST_SESSION_BEGIN(test_and_clear_flag_semantics)
		{
			switch_channel_set_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER);

			fst_check(switch_channel_test_and_clear_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER) == SWITCH_TRUE);
			fst_check(!switch_channel_test_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER));
			/* already cleared: a second caller must lose */
			fst_check(switch_channel_test_and_clear_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER) == SWITCH_FALSE);
			/* still unset: also FALSE */
			fst_check(switch_channel_test_and_clear_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER) == SWITCH_FALSE);
		}
		FST_SESSION_END()

		/*
		 * blind_transfer_defer_on_early_media: TELCORE-564. When the bridge
		 * proceeds on early media (peer not CF_ANSWERED), INDICATE_BRIDGE must
		 * NOT deliver the success BLIND_TRANSFER_RESPONSE, and must leave
		 * CF_CONFIRM_BLIND_TRANSFER set so the answer-time producer (or a
		 * failure producer) can still fire.
		 */
		FST_SESSION_BEGIN(blind_transfer_defer_on_early_media)
		{
			switch_core_session_t *r_session = NULL; /* parked REFER sender */
			switch_core_session_t *p_session = NULL; /* bridge peer in early media */
			switch_core_session_message_t msg = { 0 };

			fst_requires((r_session = bt_originate_null()));
			fst_requires(switch_core_event_hook_add_receive_message(r_session, bt_response_hook) == SWITCH_STATUS_SUCCESS);
			fst_requires((p_session = bt_originate_null()));
			/* null endpoints come up answered; force the early-media-only state */
			switch_channel_clear_flag(switch_core_session_get_channel(p_session), CF_ANSWERED);

			bt_response_count = 0;
			bt_response_numeric_arg = -1;

			switch_channel_set_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER);
			switch_channel_set_variable(fst_channel, "blind_transfer_uuid", switch_core_session_get_uuid(r_session));

			msg.message_id = SWITCH_MESSAGE_INDICATE_BRIDGE;
			msg.from = __FILE__;
			msg.string_arg = switch_core_session_strdup(fst_session, switch_core_session_get_uuid(p_session));
			fst_requires(switch_core_session_receive_message(fst_session, &msg) == SWITCH_STATUS_SUCCESS);

			fst_check(bt_response_count == 0);
			fst_check(switch_channel_test_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER));

			/* cleanup */
			switch_channel_clear_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER);
			bt_release_session(r_session);
			bt_release_session(p_session);
		}
		FST_SESSION_END()

		/*
		 * blind_transfer_success_when_peer_answered: when the bridge peer is
		 * CF_ANSWERED, INDICATE_BRIDGE delivers exactly one success response
		 * (numeric_arg=1), clears the flag, and a repeated INDICATE_BRIDGE
		 * delivers nothing.
		 */
		FST_SESSION_BEGIN(blind_transfer_success_when_peer_answered)
		{
			switch_core_session_t *r_session = NULL; /* parked REFER sender */
			switch_core_session_t *p_session = NULL; /* answered bridge peer */
			switch_core_session_message_t msg = { 0 };

			fst_requires((r_session = bt_originate_null()));
			fst_requires(switch_core_event_hook_add_receive_message(r_session, bt_response_hook) == SWITCH_STATUS_SUCCESS);
			fst_requires((p_session = bt_originate_null()));
			/* null endpoints come up answered; make it explicit anyway */
			switch_channel_set_flag(switch_core_session_get_channel(p_session), CF_ANSWERED);

			bt_response_count = 0;
			bt_response_numeric_arg = -1;

			switch_channel_set_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER);
			switch_channel_set_variable(fst_channel, "blind_transfer_uuid", switch_core_session_get_uuid(r_session));

			msg.message_id = SWITCH_MESSAGE_INDICATE_BRIDGE;
			msg.from = __FILE__;
			msg.string_arg = switch_core_session_strdup(fst_session, switch_core_session_get_uuid(p_session));
			fst_requires(switch_core_session_receive_message(fst_session, &msg) == SWITCH_STATUS_SUCCESS);

			fst_check(bt_response_count == 1);
			fst_check(bt_response_numeric_arg == 1);
			fst_check(!switch_channel_test_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER));

			/* second INDICATE_BRIDGE must not deliver a second success */
			fst_requires(switch_core_session_receive_message(fst_session, &msg) == SWITCH_STATUS_SUCCESS);
			fst_check(bt_response_count == 1);

			bt_release_session(r_session);
			bt_release_session(p_session);
		}
		FST_SESSION_END()

		/*
		 * blind_transfer_answer_time_producer: TELCORE-564. After an
		 * early-media bridge defers the confirmation, the target's answer
		 * must deliver exactly one success response through the
		 * mark_answered originator hook.
		 */
		FST_SESSION_BEGIN(blind_transfer_answer_time_producer)
		{
			switch_core_session_t *r_session = NULL; /* parked REFER sender */
			switch_core_session_t *t_session = NULL; /* transfer target */
			switch_channel_t *t_channel;

			fst_requires((r_session = bt_originate_null()));
			fst_requires(switch_core_event_hook_add_receive_message(r_session, bt_response_hook) == SWITCH_STATUS_SUCCESS);
			fst_requires((t_session = bt_originate_null()));
			t_channel = switch_core_session_get_channel(t_session);
			/* mark_answered early-returns on an already answered channel */
			switch_channel_clear_flag(t_channel, CF_ANSWERED);

			bt_response_count = 0;
			bt_response_numeric_arg = -1;

			switch_channel_set_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER);
			switch_channel_set_variable(fst_channel, "blind_transfer_uuid", switch_core_session_get_uuid(r_session));
			switch_channel_set_variable(t_channel, SWITCH_ORIGINATOR_VARIABLE, switch_core_session_get_uuid(fst_session));

			/* the target answers after the early-media bridge */
			switch_channel_mark_answered(t_channel);

			fst_check(bt_response_count == 1);
			fst_check(bt_response_numeric_arg == 1);
			fst_check(!switch_channel_test_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER));

			bt_release_session(r_session);
			bt_release_session(t_session);
		}
		FST_SESSION_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
