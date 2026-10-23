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
#include "private/switch_core_pvt.h"

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

#define PRIVQ_MAX_SEEN 128

static char *privq_media_cmds[] = { "playback", "speak", "say" };
static switch_mutex_t *privq_mutex = NULL;
static char privq_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1];
static char privq_seen[PRIVQ_MAX_SEEN][64];
static char privq_seen_name[PRIVQ_MAX_SEEN][64];
static int privq_seen_count = 0;

static void privq_on_interrupted(switch_event_t *event)
{
	const char *uuid = switch_event_get_header(event, "Unique-ID");
	const char *name = switch_event_get_header(event, "execute-app-name");
	const char *arg = switch_event_get_header(event, "execute-app-arg");

	if (!uuid || strcmp(uuid, privq_uuid)) {
		return;
	}

	switch_mutex_lock(privq_mutex);
	if (privq_seen_count < PRIVQ_MAX_SEEN) {
		switch_snprintf(privq_seen_name[privq_seen_count], sizeof(privq_seen_name[0]), "%s", switch_str_nil(name));
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

/* how many interrupt events named this command (app name and argument; "" for a missing header) */
static int privq_seen_times(const char *name, const char *arg)
{
	int i, n = 0;

	switch_mutex_lock(privq_mutex);
	for (i = 0; i < privq_seen_count && i < PRIVQ_MAX_SEEN; i++) {
		if (!strcmp(privq_seen_name[i], name) && !strcmp(privq_seen[i], arg)) {
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

static switch_io_routines_t privq_io_routines = { 0 };
static switch_state_handler_table_t privq_state_handlers = { 0 };

/* A session without its own thread or channel driver, so only the test pops its private queues. */
static switch_core_session_t *privq_session_new(switch_memory_pool_t *pool)
{
	switch_loadable_module_interface_t *module_interface = switch_loadable_module_create_module_interface(pool, "privq_test");
	switch_endpoint_interface_t *endpoint_interface = switch_loadable_module_create_interface(module_interface, SWITCH_ENDPOINT_INTERFACE);

	endpoint_interface->interface_name = "privq_test";
	endpoint_interface->io_routines = &privq_io_routines;
	endpoint_interface->state_handler = &privq_state_handlers;
	return switch_core_session_request_uuid(endpoint_interface, SWITCH_CALL_DIRECTION_OUTBOUND, SOF_NO_LIMITS, NULL, NULL);
}

static void privq_session_free(switch_core_session_t **session)
{
	switch_event_t *event = NULL;

	switch_channel_clear_flag(switch_core_session_get_channel(*session), CF_EVENT_LOCK);
	switch_channel_clear_flag(switch_core_session_get_channel(*session), CF_EVENT_LOCK_PRI);
	while (switch_core_session_dequeue_private_event(*session, &event) == SWITCH_STATUS_SUCCESS) {
		switch_event_destroy(&event);
	}
	switch_core_session_destroy(session);
}

/* Queues a named command and returns the event object now owned by the queue, NULL if it wasn't queued. */
static switch_event_t *privq_push(switch_core_session_t *session, const char *app_name, const char *app_arg, switch_bool_t priority)
{
	switch_event_t *event = NULL, *queued;

	if (switch_event_create(&event, SWITCH_EVENT_COMMAND) != SWITCH_STATUS_SUCCESS) {
		return NULL;
	}
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "call-command", "execute");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-name", app_name);
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "execute-app-arg", app_arg);
	queued = event;
	if (switch_core_session_queue_private_event(session, &event, priority) != SWITCH_STATUS_SUCCESS) {
		if (event) {
			switch_event_destroy(&event);
		}
		return NULL;
	}
	return queued;
}

/* Pops the next private command and checks it is the expected object; the event is destroyed either way. */
static switch_bool_t privq_pop_is(switch_core_session_t *session, switch_event_t *expected, const char *app_arg)
{
	switch_event_t *event = NULL;
	switch_bool_t ok;

	if (switch_core_session_dequeue_private_event(session, &event) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_FALSE;
	}
	ok = (event == expected && !strcmp(switch_str_nil(switch_event_get_header(event, "execute-app-arg")), app_arg)) ? SWITCH_TRUE : SWITCH_FALSE;
	switch_event_destroy(&event);
	return ok;
}

static switch_bool_t privq_empty(switch_core_session_t *session)
{
	switch_event_t *event = NULL;

	if (switch_core_session_dequeue_private_event(session, &event) == SWITCH_STATUS_SUCCESS) {
		switch_event_destroy(&event);
		return SWITCH_FALSE;
	}
	return SWITCH_TRUE;
}

/* Concurrent producers, a consumer and media flushes on one thread-less session: every queued command ends exactly once. */
#define PRIVQ_STRESS_PRODUCERS 3
#define PRIVQ_STRESS_FLUSHERS 2
#define PRIVQ_STRESS_PER_PRODUCER 1000
#define PRIVQ_STRESS_TOTAL (PRIVQ_STRESS_PRODUCERS * PRIVQ_STRESS_PER_PRODUCER)

typedef struct {
	switch_core_session_t *session;
	switch_mutex_t *mutex;
	char uuid[SWITCH_UUID_FORMATTED_LENGTH + 1];
	int queued[PRIVQ_STRESS_TOTAL];
	int popped[PRIVQ_STRESS_TOTAL];
	int interrupted[PRIVQ_STRESS_TOTAL];
	int bad_events;
	int producers_done;
	int flushers_done;
	int consumer_done;
	uint32_t removed;
} privq_stress_t;

static privq_stress_t *privq_stress = NULL;

typedef struct {
	privq_stress_t *st;
	int index;
} privq_stress_arg_t;

/* the command id from an execute-app-arg "s<id>", -1 if it isn't one */
static int privq_stress_id(const char *arg)
{
	int id;

	if (!arg || *arg != 's' || !switch_is_number(arg + 1)) {
		return -1;
	}
	id = atoi(arg + 1);
	return (id >= 0 && id < PRIVQ_STRESS_TOTAL) ? id : -1;
}

static void privq_stress_on_interrupted(switch_event_t *event)
{
	privq_stress_t *st = privq_stress;
	const char *uuid = switch_event_get_header(event, "Unique-ID");
	const char *name = switch_event_get_header(event, "execute-app-name");
	int id;

	if (!st || !uuid || strcmp(uuid, st->uuid)) {
		return;
	}
	id = privq_stress_id(switch_event_get_header(event, "execute-app-arg"));
	switch_mutex_lock(st->mutex);
	if (id < 0 || !name || strcmp(name, "playback")) {
		st->bad_events++;
	} else {
		st->interrupted[id]++;
	}
	switch_mutex_unlock(st->mutex);
}

static void *SWITCH_THREAD_FUNC privq_stress_producer(switch_thread_t *thread, void *obj)
{
	privq_stress_arg_t *a = (privq_stress_arg_t *) obj;
	privq_stress_t *st = a->st;
	char arg[16];
	int i, id;

	for (i = 0; i < PRIVQ_STRESS_PER_PRODUCER; i++) {
		id = a->index * PRIVQ_STRESS_PER_PRODUCER + i;
		switch_snprintf(arg, sizeof(arg), "s%d", id);
		/* half media, half not; every third one on the priority queue */
		if (privq_push(st->session, (id % 2) ? "playback" : "set", arg, (id % 3) ? SWITCH_FALSE : SWITCH_TRUE)) {
			switch_mutex_lock(st->mutex);
			st->queued[id]++;
			switch_mutex_unlock(st->mutex);
		}
		if (!(i % 64)) {
			switch_cond_next();
		}
	}
	switch_mutex_lock(st->mutex);
	st->producers_done++;
	switch_mutex_unlock(st->mutex);
	return NULL;
}

static void *SWITCH_THREAD_FUNC privq_stress_flusher(switch_thread_t *thread, void *obj)
{
	privq_stress_arg_t *a = (privq_stress_arg_t *) obj;
	privq_stress_t *st = a->st;
	uint32_t removed;
	int done;

	do {
		removed = switch_core_session_flush_and_publish_private_media_events(st->session, privq_media_cmds, 3);
		switch_mutex_lock(st->mutex);
		st->removed += removed;
		done = (st->producers_done == PRIVQ_STRESS_PRODUCERS);
		switch_mutex_unlock(st->mutex);
		switch_cond_next();
	} while (!done);
	switch_mutex_lock(st->mutex);
	st->flushers_done++;
	switch_mutex_unlock(st->mutex);
	return NULL;
}

static void *SWITCH_THREAD_FUNC privq_stress_consumer(switch_thread_t *thread, void *obj)
{
	privq_stress_arg_t *a = (privq_stress_arg_t *) obj;
	privq_stress_t *st = a->st;
	switch_event_t *event = NULL;
	int id, done = 0, go = 0;

	/* start consuming only once a flush has removed something (or nothing more can be queued), so both ends are exercised */
	while (!go) {
		switch_mutex_lock(st->mutex);
		go = (st->removed > 0 || st->producers_done == PRIVQ_STRESS_PRODUCERS);
		switch_mutex_unlock(st->mutex);
		if (!go) {
			switch_cond_next();
		}
	}

	for (;;) {
		if (switch_core_session_dequeue_private_event(st->session, &event) == SWITCH_STATUS_SUCCESS) {
			id = privq_stress_id(switch_event_get_header(event, "execute-app-arg"));
			switch_mutex_lock(st->mutex);
			if (id < 0) {
				st->bad_events++;
			} else {
				st->popped[id]++;
			}
			switch_mutex_unlock(st->mutex);
			switch_event_destroy(&event);
			continue;
		}
		/* empty: stop only once nothing can be queued any more */
		if (done) {
			break;
		}
		switch_mutex_lock(st->mutex);
		done = (st->producers_done == PRIVQ_STRESS_PRODUCERS && st->flushers_done == PRIVQ_STRESS_FLUSHERS);
		switch_mutex_unlock(st->mutex);
		if (!done) {
			switch_cond_next();
		}
	}
	switch_mutex_lock(st->mutex);
	st->consumer_done++;
	switch_mutex_unlock(st->mutex);
	return NULL;
}

/*
 * Worker threads still running would use the test's pool and stack after the test returns, so a thread that can't
 * start or doesn't finish ends the whole suite here instead.
 */
static void privq_stress_fail(const char *why)
{
	fprintf(stderr, "privq stress: %s, aborting the suite\n", why);
	fflush(stderr);
	abort();
}

/* waits up to 300 s (long enough under valgrind) for cond(st) to hold; returns whether it did */
static switch_bool_t privq_stress_wait(privq_stress_t *st, switch_bool_t (*cond)(privq_stress_t *st))
{
	int i;
	switch_bool_t ok = SWITCH_FALSE;

	for (i = 0; i < 30000 && !ok; i++) {
		if (switch_mutex_trylock(st->mutex) == SWITCH_STATUS_SUCCESS) {
			ok = cond(st);
			switch_mutex_unlock(st->mutex);
		}
		if (!ok) {
			switch_yield(10000);
		}
	}
	return ok;
}

static switch_bool_t privq_stress_threads_done(privq_stress_t *st)
{
	return (st->producers_done == PRIVQ_STRESS_PRODUCERS && st->flushers_done == PRIVQ_STRESS_FLUSHERS && st->consumer_done == 1) ? SWITCH_TRUE : SWITCH_FALSE;
}

static switch_bool_t privq_stress_events_done(privq_stress_t *st)
{
	uint32_t n = 0;
	int i;

	for (i = 0; i < PRIVQ_STRESS_TOTAL; i++) {
		n += st->interrupted[i];
	}
	return n >= st->removed ? SWITCH_TRUE : SWITCH_FALSE;
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
		 * blind_transfer_defer_on_early_media: when the bridge
		 * proceeds on early media (peer not CF_ANSWERED), INDICATE_BRIDGE must
		 * NOT deliver the success BLIND_TRANSFER_RESPONSE, and must leave
		 * CF_CONFIRM_BLIND_TRANSFER set so the answer-time producer (or a
		 * failure producer) can still fire. Same contract when the peer has
		 * already rejected: at CS_HANGUP switch_core_session_locate
		 * refuses it, and an unlocatable peer never answered.
		 */
		FST_SESSION_BEGIN(blind_transfer_defer_on_early_media)
		{
			switch_core_session_t *r_session = NULL; /* parked REFER sender */
			switch_core_session_t *p_session = NULL; /* bridge peer in early media */
			switch_core_session_message_t msg = { 0 };

			fst_requires((r_session = start_peer()));
			fst_requires((p_session = start_peer()));
			/* null endpoints come up answered; force the early-media-only state */
			switch_channel_clear_flag(switch_core_session_get_channel(p_session), CF_ANSWERED);

			switch_channel_set_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER);
			switch_channel_set_variable(fst_channel, "blind_transfer_uuid", switch_core_session_get_uuid(r_session));

			msg.message_id = SWITCH_MESSAGE_INDICATE_BRIDGE;
			msg.from = __FILE__;
			msg.string_arg = switch_core_session_strdup(fst_session, switch_core_session_get_uuid(p_session));
			fst_requires(switch_core_session_receive_message(fst_session, &msg) == SWITCH_STATUS_SUCCESS);

			fst_check(switch_atomic_read(&blind_transfer_response_count) == 0);
			fst_check(switch_channel_test_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER));

			/* peer rejects after early media: locate now fails, still no success */
			switch_channel_hangup(switch_core_session_get_channel(p_session), SWITCH_CAUSE_USER_BUSY);
			fst_requires(switch_core_session_receive_message(fst_session, &msg) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_atomic_read(&blind_transfer_response_count) == 0);
			fst_check(switch_channel_test_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER));

			/* cleanup */
			switch_channel_clear_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER);
			stop_peer(r_session);
			stop_peer(p_session);
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

			fst_requires((r_session = start_peer()));
			fst_requires((p_session = start_peer()));
			/* null endpoints come up answered; make it explicit anyway */
			switch_channel_set_flag(switch_core_session_get_channel(p_session), CF_ANSWERED);

			switch_channel_set_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER);
			switch_channel_set_variable(fst_channel, "blind_transfer_uuid", switch_core_session_get_uuid(r_session));

			msg.message_id = SWITCH_MESSAGE_INDICATE_BRIDGE;
			msg.from = __FILE__;
			msg.string_arg = switch_core_session_strdup(fst_session, switch_core_session_get_uuid(p_session));
			fst_requires(switch_core_session_receive_message(fst_session, &msg) == SWITCH_STATUS_SUCCESS);

			fst_check(switch_atomic_read(&blind_transfer_response_count) == 1);
			fst_check(blind_transfer_response_numeric_arg == 1);
			fst_check(!switch_channel_test_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER));

			/* second INDICATE_BRIDGE must not deliver a second success */
			fst_requires(switch_core_session_receive_message(fst_session, &msg) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_atomic_read(&blind_transfer_response_count) == 1);

			/* REFER sender gone: nothing to answer, the flag must not stay set */
			switch_channel_set_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER);
			switch_channel_set_variable(fst_channel, "blind_transfer_uuid", "00000000-0000-0000-0000-000000000000");
			fst_requires(switch_core_session_receive_message(fst_session, &msg) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_atomic_read(&blind_transfer_response_count) == 1);
			fst_check(!switch_channel_test_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER));

			stop_peer(r_session);
			stop_peer(p_session);
		}
		FST_SESSION_END()

		/*
		 * blind_transfer_answer_time_producer: after an early-media bridge
		 * defers the confirmation, the answer of the bridge peer delivers
		 * exactly one success, also when bridge_to is already gone (the
		 * bridge was handed off, for example to the T.38 gateway). A leg
		 * that the transferred leg originated but has not bridged yet (for
		 * example a group_confirm leg) must not report success when it
		 * answers.
		 */
		FST_SESSION_BEGIN(blind_transfer_answer_time_producer)
		{
			switch_core_session_t *r_session = NULL; /* parked REFER sender */
			switch_core_session_t *o_session = NULL; /* originated, not bridged yet */
			switch_core_session_t *t_session = NULL; /* transfer target bridged on early media */
			switch_channel_t *o_channel;
			switch_channel_t *t_channel;
			switch_core_session_message_t msg = { 0 };

			fst_requires((r_session = start_peer()));
			fst_requires((o_session = start_peer()));
			fst_requires((t_session = start_peer()));
			o_channel = switch_core_session_get_channel(o_session);
			t_channel = switch_core_session_get_channel(t_session);
			/* mark_answered early-returns on an already answered channel */
			switch_channel_clear_flag(o_channel, CF_ANSWERED);
			switch_channel_clear_flag(t_channel, CF_ANSWERED);

			switch_channel_set_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER);
			switch_channel_set_variable(fst_channel, "blind_transfer_uuid", switch_core_session_get_uuid(r_session));

			/* answered but not bridged: no result yet */
			switch_channel_set_variable(o_channel, SWITCH_ORIGINATOR_VARIABLE, switch_core_session_get_uuid(fst_session));
			switch_channel_mark_answered(o_channel);
			fst_check(switch_atomic_read(&blind_transfer_response_count) == 0);
			fst_check(switch_channel_test_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER));

			/* bridge to the target on early media: deferred */
			msg.message_id = SWITCH_MESSAGE_INDICATE_BRIDGE;
			msg.from = __FILE__;
			msg.string_arg = switch_core_session_strdup(fst_session, switch_core_session_get_uuid(t_session));
			fst_requires(switch_core_session_receive_message(fst_session, &msg) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_atomic_read(&blind_transfer_response_count) == 0);

			/* the target answers with no bridge_to left (bridge handed off) */
			fst_check(switch_channel_get_variable(t_channel, SWITCH_BRIDGE_VARIABLE) == NULL);
			switch_channel_mark_answered(t_channel);

			fst_check(switch_atomic_read(&blind_transfer_response_count) == 1);
			fst_check(blind_transfer_response_numeric_arg == 1);
			fst_check(!switch_channel_test_flag(fst_channel, CF_CONFIRM_BLIND_TRANSFER));
			fst_check(switch_channel_get_variable(t_channel, "blind_transfer_deferred_uuid") == NULL);

			stop_peer(r_session);
			stop_peer(o_session);
			stop_peer(t_session);
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
			fst_check(privq_seen_times("playback", "pri-playback") == 1);

			privq_reset();
			kept = switch_core_session_flush_and_publish_private_events(fst_session);
			fst_check(kept == 4);
			fst_check(privq_wait(4) == 4);
			fst_check(privq_seen_times("", "pri-hangup") == 1);
			fst_check(privq_seen_times("", "") == 1);
			fst_check(privq_seen_times("", "pri-execute-no-name") == 1);
			fst_check(privq_seen_times("", "pri-execute-empty-name") == 1);

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
			fst_check(privq_seen_times("SPEAK", "norm-speak") == 1);
			fst_check(privq_seen_times("Say", "norm-say") == 1);

			privq_reset();
			kept = switch_core_session_flush_and_publish_private_events(fst_session);
			fst_check(kept == 4);
			fst_check(privq_wait(4) == 4);
			fst_check(privq_seen_times("", "norm-unicast") == 1);
			fst_check(privq_seen_times("", "norm-xferext") == 1);
			fst_check(privq_seen_times("set", "norm-set") == 1);
			fst_check(privq_seen_times("", "norm-hangup") == 1);

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
			fst_check(privq_seen_times("", "ctl-hangup") == 1);
			fst_check(privq_seen_times("playback", "ctl-pri-playback") == 1);
			fst_check(privq_seen_times("", "ctl-nomedia") == 1);
			fst_check(privq_seen_times("playback", "ctl-playback") == 1);

			privq_stop();
			switch_channel_clear_flag(fst_channel, CF_EVENT_LOCK);
			switch_channel_clear_flag(fst_channel, CF_EVENT_LOCK_PRI);
		}
		FST_SESSION_END()

		FST_TEST_BEGIN(private_media_flush_keeps_queues_and_survivors)
		{
			switch_core_session_t *session;
			switch_queue_t *queue, *queue_pri;
			switch_event_t *p1, *n1, *n3, *n5;
			uint32_t removed;

			session = privq_session_new(fst_pool);
			fst_requires(session);
			queue = session->private_event_queue;
			queue_pri = session->private_event_queue_pri;
			fst_check(privq_start(session, fst_pool) == SWITCH_STATUS_SUCCESS);

			p1 = privq_push(session, "set", "p1", SWITCH_TRUE);
			fst_check(privq_push(session, "say", "p2", SWITCH_TRUE) != NULL);
			n1 = privq_push(session, "set", "n1", SWITCH_FALSE);
			fst_check(privq_push(session, "playback", "n2", SWITCH_FALSE) != NULL);
			n3 = privq_push(session, "answer", "n3", SWITCH_FALSE);
			fst_check(privq_push(session, "speak", "n4", SWITCH_FALSE) != NULL);
			n5 = privq_push(session, "set", "n5", SWITCH_FALSE);
			fst_check(p1 && n1 && n3 && n5);

			removed = switch_core_session_flush_and_publish_private_media_events(session, privq_media_cmds, 3);
			fst_check(removed == 3);
			fst_check(session->private_event_queue == queue);
			fst_check(session->private_event_queue_pri == queue_pri);
			fst_check(privq_pop_is(session, p1, "p1"));
			fst_check(privq_pop_is(session, n1, "n1"));
			fst_check(privq_pop_is(session, n3, "n3"));
			fst_check(privq_pop_is(session, n5, "n5"));
			fst_check(privq_empty(session));

			fst_check(privq_wait(3) == 3);
			fst_check(privq_seen_times("say", "p2") == 1);
			fst_check(privq_seen_times("playback", "n2") == 1);
			fst_check(privq_seen_times("speak", "n4") == 1);

			privq_stop();
			privq_session_free(&session);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(private_media_flush_full_and_wrapped_queue)
		{
			switch_core_session_t *session;
			switch_event_t *ev[407];
			char args[407][16];
			uint32_t removed;
			int i;

			session = privq_session_new(fst_pool);
			fst_requires(session);
			fst_check(privq_start(session, fst_pool) == SWITCH_STATUS_SUCCESS);

			/* consume part of the ring, then fill it so the stored items wrap around its end */
			for (i = 0; i < 200; i++) {
				switch_snprintf(args[i], sizeof(args[i]), "w%d", i);
				ev[i] = privq_push(session, "set", args[i], SWITCH_FALSE);
				fst_check(ev[i] != NULL);
			}
			for (i = 0; i < 150; i++) {
				fst_check(privq_pop_is(session, ev[i], args[i]));
			}
			for (i = 200; i < 406; i++) {
				switch_snprintf(args[i], sizeof(args[i]), "f%d", i);
				ev[i] = privq_push(session, (i % 2) ? "playback" : "set", args[i], SWITCH_FALSE);
				fst_check(ev[i] != NULL);
			}
			fst_check(privq_push(session, "set", "overflow", SWITCH_FALSE) == NULL);

			removed = switch_core_session_flush_and_publish_private_media_events(session, privq_media_cmds, 3);
			fst_check(removed == 103);

			switch_snprintf(args[406], sizeof(args[406]), "after");
			ev[406] = privq_push(session, "set", args[406], SWITCH_FALSE);
			fst_check(ev[406] != NULL);

			for (i = 150; i < 406; i++) {
				if (i < 200 || !(i % 2)) {
					fst_check(privq_pop_is(session, ev[i], args[i]));
				}
			}
			fst_check(privq_pop_is(session, ev[406], args[406]));
			fst_check(privq_empty(session));

			fst_check(privq_wait(103) == 103);
			for (i = 201; i < 406; i += 2) {
				fst_check(privq_seen_times("playback", args[i]) == 1);
			}

			privq_stop();
			privq_session_free(&session);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(private_media_flush_empty_all_and_no_match)
		{
			switch_core_session_t *session;
			switch_channel_t *channel;
			switch_event_t *ev[10];
			char args[10][16];
			uint32_t removed = 0;
			int i;

			session = privq_session_new(fst_pool);
			fst_requires(session);
			channel = switch_core_session_get_channel(session);
			fst_check(privq_start(session, fst_pool) == SWITCH_STATUS_SUCCESS);

			fst_check(switch_core_session_flush_and_publish_private_media_events(session, privq_media_cmds, 3) == 0);

			for (i = 0; i < 10; i++) {
				switch_snprintf(args[i], sizeof(args[i]), "all%d", i);
				fst_check(privq_push(session, privq_media_cmds[i % 3], args[i], (i % 2) ? SWITCH_TRUE : SWITCH_FALSE) != NULL);
			}
			fst_check(switch_core_session_flush_and_publish_private_media_events(session, privq_media_cmds, 3) == 10);
			fst_check(privq_empty(session));

			for (i = 0; i < 10; i++) {
				switch_snprintf(args[i], sizeof(args[i]), "none%d", i);
				ev[i] = privq_push(session, "set", args[i], SWITCH_FALSE);
				fst_check(ev[i] != NULL);
			}
			switch_channel_set_flag(channel, CF_EVENT_LOCK);
			switch_channel_set_flag(channel, CF_EVENT_LOCK_PRI);
			for (i = 0; i < 1000; i++) {
				removed += switch_core_session_flush_and_publish_private_media_events(session, privq_media_cmds, 3);
			}
			fst_check(removed == 0);
			fst_check(switch_channel_test_flag(channel, CF_EVENT_LOCK));
			fst_check(switch_channel_test_flag(channel, CF_EVENT_LOCK_PRI));
			switch_channel_clear_flag(channel, CF_EVENT_LOCK);
			switch_channel_clear_flag(channel, CF_EVENT_LOCK_PRI);

			for (i = 0; i < 10; i++) {
				fst_check(privq_pop_is(session, ev[i], args[i]));
			}
			fst_check(privq_empty(session));

			fst_check(privq_wait(10) == 10);
			for (i = 0; i < 10; i++) {
				switch_snprintf(args[i], sizeof(args[i]), "all%d", i);
				fst_check(privq_seen_times(privq_media_cmds[i % 3], args[i]) == 1);
			}

			privq_stop();
			privq_session_free(&session);
		}
		FST_TEST_END()

		/*
		 * Producers, a consumer and two media flushes run at once on one session: every queued command is either
		 * consumed or interrupted, exactly once, and only media commands are interrupted. The threads must also
		 * finish, so a lock-order problem between the flush and the queue shows up as a failure here.
		 */
		FST_TEST_BEGIN(private_media_flush_concurrent_producers_and_consumer)
		{
			privq_stress_t *st;
			privq_stress_arg_t args[PRIVQ_STRESS_PRODUCERS + PRIVQ_STRESS_FLUSHERS + 1];
			switch_thread_t *threads[PRIVQ_STRESS_PRODUCERS + PRIVQ_STRESS_FLUSHERS + 1] = { 0 };
			switch_threadattr_t *thd_attr = NULL;
			switch_status_t retval;
			int i, n = 0, queued = 0, popped = 0, interrupted = 0, lost = 0, twice = 0, wrong = 0, bad;
			uint32_t removed;
			switch_bool_t delivered;

			st = switch_core_alloc(fst_pool, sizeof(*st));
			fst_requires(st);
			st->session = privq_session_new(fst_pool);
			fst_requires(st->session);
			fst_requires(switch_mutex_init(&st->mutex, SWITCH_MUTEX_NESTED, fst_pool) == SWITCH_STATUS_SUCCESS);
			switch_snprintf(st->uuid, sizeof(st->uuid), "%s", switch_core_session_get_uuid(st->session));
			privq_stress = st;
			fst_requires(switch_event_bind("privq_stress", SWITCH_EVENT_COMMAND_INTERRUPTED, SWITCH_EVENT_SUBCLASS_ANY, privq_stress_on_interrupted, NULL) == SWITCH_STATUS_SUCCESS);

			switch_threadattr_create(&thd_attr, fst_pool);
			switch_threadattr_detach_set(thd_attr, 0);
			switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
			args[n].st = st;
			args[n].index = 0;
			if (switch_thread_create(&threads[n], thd_attr, privq_stress_consumer, &args[n], fst_pool) != SWITCH_STATUS_SUCCESS) {
				privq_stress_fail("a thread didn't start");
			}
			n++;
			for (i = 0; i < PRIVQ_STRESS_FLUSHERS; i++, n++) {
				args[n].st = st;
				args[n].index = i;
				if (switch_thread_create(&threads[n], thd_attr, privq_stress_flusher, &args[n], fst_pool) != SWITCH_STATUS_SUCCESS) {
					privq_stress_fail("a thread didn't start");
				}
			}
			for (i = 0; i < PRIVQ_STRESS_PRODUCERS; i++, n++) {
				args[n].st = st;
				args[n].index = i;
				if (switch_thread_create(&threads[n], thd_attr, privq_stress_producer, &args[n], fst_pool) != SWITCH_STATUS_SUCCESS) {
					privq_stress_fail("a thread didn't start");
				}
			}

			if (!privq_stress_wait(st, privq_stress_threads_done)) {
				privq_stress_fail("the threads didn't finish");
			}
			for (i = 0; i < n; i++) {
				switch_thread_join(&retval, threads[i]);
			}
			delivered = privq_stress_wait(st, privq_stress_events_done);
			fst_check(delivered);

			/* no callback runs once unbind returns (it takes the event registry write lock), so the counts are final */
			switch_event_unbind_callback(privq_stress_on_interrupted);
			privq_stress = NULL;

			switch_mutex_lock(st->mutex);
			for (i = 0; i < PRIVQ_STRESS_TOTAL; i++) {
				int ends = st->popped[i] + st->interrupted[i];

				queued += st->queued[i];
				popped += st->popped[i];
				interrupted += st->interrupted[i];
				if (st->queued[i] && !ends) {
					lost++;
				}
				if (ends > st->queued[i]) {
					twice++;
				}
				if (st->interrupted[i] && !(i % 2)) {
					wrong++;
				}
			}
			bad = st->bad_events;
			removed = st->removed;
			switch_mutex_unlock(st->mutex);

			fprintf(stderr, "privq stress: queued %d, consumed %d, interrupted %d (flush returned %u)\n", queued, popped, interrupted, removed);
			fst_check(queued > 0);
			fst_check(popped > 0);
			fst_check(interrupted > 0);
			fst_check(lost == 0);
			fst_check(twice == 0);
			fst_check(wrong == 0);
			fst_check(bad == 0);
			fst_check((uint32_t) interrupted == removed);
			fst_check(queued == popped + interrupted);
			fst_check(privq_empty(st->session));

			privq_session_free(&st->session);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
