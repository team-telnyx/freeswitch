/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
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
 *
 * switch_refresh_watch.c -- Session-thread stall observer tests
 *
 */

#include <switch.h>
#include <switch_refresh_diag.h>
#include <test/switch_test.h>

#define MAX_RECORDS 512
#define STALL_US 50000
#define SLOW_US 80000

static switch_memory_pool_t *suite_pool = NULL;
static switch_mutex_t *records_mutex = NULL;
static char *records[MAX_RECORDS];
static int record_count = 0;

static int flush_seen = 0;

/* Bound loggers get lines from the log thread, in order: a marker line tells when earlier lines have arrived. The test
   framework disables logging between tests, so what the setup's pass logs is dropped: it only drains earlier tests. */
static switch_status_t capture_log(const switch_log_node_t *node, switch_log_level_t level)
{
	const char *p;

	if (!node->data) {
		return SWITCH_STATUS_SUCCESS;
	}

	switch_mutex_lock(records_mutex);
	if ((p = strstr(node->data, "RWATCH_TEST_FLUSH "))) {
		flush_seen = atoi(p + strlen("RWATCH_TEST_FLUSH "));
	} else if (strstr(node->data, "SIP_REFRESH_WORK schema=1 kind=stall ") && record_count < MAX_RECORDS) {
		records[record_count++] = strdup(node->data);
	}
	switch_mutex_unlock(records_mutex);

	return SWITCH_STATUS_SUCCESS;
}

static void flush_log(void)
{
	static int seq = 0;
	int i, seen = 0;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "RWATCH_TEST_FLUSH %d\n", ++seq);
	for (i = 0; i < 2000 && !seen; i++) {
		switch_mutex_lock(records_mutex);
		seen = flush_seen == seq;
		switch_mutex_unlock(records_mutex);
		if (!seen) {
			switch_sleep(1000);
		}
	}
}

static void poll_and_flush(void)
{
	switch_refresh_watch_poll();
	flush_log();
}

static void clear_records(void)
{
	int i;

	switch_mutex_lock(records_mutex);
	for (i = 0; i < record_count; i++) {
		free(records[i]);
		records[i] = NULL;
	}
	record_count = 0;
	switch_mutex_unlock(records_mutex);
}

/* Records matching every non-NULL needle; *first gets the first match */
static int count_records(const char *a, const char *b, const char *c, const char **first)
{
	int i, n = 0;

	if (first) {
		*first = NULL;
	}

	switch_mutex_lock(records_mutex);
	for (i = 0; i < record_count; i++) {
		if ((!a || strstr(records[i], a)) && (!b || strstr(records[i], b)) && (!c || strstr(records[i], c))) {
			if (first && !*first) {
				*first = records[i];
			}
			n++;
		}
	}
	switch_mutex_unlock(records_mutex);

	return n;
}

static long long field(const char *rec, const char *name)
{
	char key[64];
	const char *p;

	switch_snprintf(key, sizeof(key), " %s=", name);
	if (!rec || !(p = strstr(rec, key))) {
		return -1;
	}

	return atoll(p + strlen(key));
}

static void slow_op(switch_refresh_watch_t *w, switch_refresh_watch_stage_t stage, switch_time_t us)
{
	int h = switch_refresh_watch_begin(w, stage, NULL);

	switch_sleep(us);
	switch_refresh_watch_end(w, h);
}

typedef struct {
	switch_abc_type_t slow_type;
	switch_time_t sleep_us;
	int calls;
} slow_bug_t;

static switch_bool_t slow_bug_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	slow_bug_t *sb = (slow_bug_t *) user_data;

	if (type == sb->slow_type) {
		sb->calls++;
		switch_sleep(sb->sleep_us);
	}

	return SWITCH_TRUE;
}

FST_CORE_BEGIN("./conf")
{
	FST_SUITE_BEGIN(switch_refresh_watch)
	{
		FST_SETUP_BEGIN()
		{
			/* fst_pool is destroyed after each test: the mutex lives in a pool kept for the whole suite */
			if (!suite_pool) {
				switch_core_new_memory_pool(&suite_pool);
				switch_mutex_init(&records_mutex, SWITCH_MUTEX_NESTED, suite_pool);
				switch_log_bind_logger(capture_log, SWITCH_LOG_DEBUG, SWITCH_FALSE);
			}
			poll_and_flush();
			clear_records();
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
			switch_refresh_watch_bind(NULL);
		}
		FST_TEARDOWN_END()

		FST_SESSION_BEGIN(unbound_or_unarmed_times_nothing)
		{
			switch_refresh_watch_t *w;

			fst_check(switch_refresh_watch_current() == NULL);
			fst_check(switch_refresh_watch_begin(NULL, SWITCH_RWATCH_STAGE_READ, NULL) == -1);

			w = switch_refresh_watch_create(switch_core_session_get_uuid(fst_session), fst_channel, STALL_US);
			fst_requires(w);
			switch_refresh_watch_bind(w);
			fst_check(switch_refresh_watch_current() == w);

			fst_check(switch_refresh_watch_begin(w, SWITCH_RWATCH_STAGE_READ, NULL) == -1);
			switch_refresh_watch_end(w, -1);

			switch_refresh_watch_arm(w);
			fst_check(switch_refresh_watch_begin(w, SWITCH_RWATCH_STAGE_MAX, NULL) == -1);
			fst_check(switch_refresh_watch_begin(w, SWITCH_RWATCH_STAGE_READ, NULL) == 0);
			switch_refresh_watch_end(w, 0);

			switch_refresh_watch_bind(NULL);
			fst_check(switch_refresh_watch_current() == NULL);
			switch_refresh_watch_destroy(&w);
			fst_check(w == NULL);

			poll_and_flush();
			fst_check(count_records(NULL, NULL, NULL, NULL) == 0);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(completed_stall_between_polls_and_threshold)
		{
			switch_refresh_watch_t *w = switch_refresh_watch_create(switch_core_session_get_uuid(fst_session), fst_channel, STALL_US);
			const char *rec;

			fst_requires(w);
			switch_refresh_watch_arm(w);

			slow_op(w, SWITCH_RWATCH_STAGE_READ, 10000);
			slow_op(w, SWITCH_RWATCH_STAGE_WRITE, SLOW_US);

			poll_and_flush();
			fst_check(count_records(NULL, NULL, NULL, NULL) == 1);
			fst_requires(count_records("status=completed", " stage=write ", NULL, &rec) == 1);
			fst_check(field(rec, "elapsed_us") >= SLOW_US);
			fst_check(field(rec, "op") == 2);
			fst_check(field(rec, "depth") == 1);
			fst_check(field(rec, "parent_op") == 0);
			fst_check(strstr(rec, "target=") == NULL);
			fst_check(strstr(rec, "SIP_REFRESH_WORK schema=1 kind=stall status=completed stage=write ") != NULL);

			poll_and_flush();
			fst_check(count_records(NULL, NULL, NULL, NULL) == 1);

			switch_refresh_watch_destroy(&w);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(active_stall_reported_once_then_completed)
		{
			switch_refresh_watch_t *w = switch_refresh_watch_create(switch_core_session_get_uuid(fst_session), fst_channel, STALL_US);
			const char *rec;
			int h;

			fst_requires(w);
			switch_refresh_watch_arm(w);

			h = switch_refresh_watch_begin(w, SWITCH_RWATCH_STAGE_READ, NULL);
			switch_sleep(SLOW_US);
			poll_and_flush();
			fst_requires(count_records("status=active", " stage=read ", NULL, &rec) == 1);
			fst_check(field(rec, "elapsed_us") > STALL_US);
			fst_check(field(rec, "op") == 1);

			switch_sleep(20000);
			poll_and_flush();
			fst_check(count_records("status=active", NULL, NULL, NULL) == 1);

			switch_refresh_watch_end(w, h);
			poll_and_flush();
			fst_requires(count_records("status=completed", " op=1 ", NULL, &rec) == 1);
			fst_check(field(rec, "elapsed_us") >= SLOW_US + 20000);

			switch_refresh_watch_destroy(&w);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(active_parent_not_repeated_after_nested)
		{
			switch_refresh_watch_t *w = switch_refresh_watch_create(switch_core_session_get_uuid(fst_session), fst_channel, STALL_US);
			switch_refresh_watch_totals_t before, after;
			int outer, inner;

			fst_requires(w);
			switch_refresh_watch_arm(w);
			switch_refresh_watch_totals(&before);

			outer = switch_refresh_watch_begin(w, SWITCH_RWATCH_STAGE_READ, NULL);
			switch_sleep(SLOW_US);
			poll_and_flush();
			inner = switch_refresh_watch_begin(w, SWITCH_RWATCH_STAGE_BUG_READ, NULL);
			switch_sleep(SLOW_US);
			poll_and_flush();
			switch_refresh_watch_end(w, inner);
			switch_sleep(20000);
			poll_and_flush();
			switch_sleep(20000);
			poll_and_flush();

			/* the parent is seen again after the nested operation: reported once, nothing re-sent or suppressed */
			fst_check(count_records("status=active", " op=1 ", NULL, NULL) == 1);
			fst_check(count_records("status=active", " op=2 ", NULL, NULL) == 1);
			fst_check(count_records("status=completed", " op=2 ", NULL, NULL) == 1);
			switch_refresh_watch_totals(&after);
			fst_check(after.suppressed_call == before.suppressed_call);

			switch_refresh_watch_end(w, outer);
			switch_refresh_watch_destroy(&w);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(suppression_counted_when_watch_destroyed)
		{
			switch_refresh_watch_t *w = switch_refresh_watch_create(switch_core_session_get_uuid(fst_session), fst_channel, 1000);
			switch_refresh_watch_totals_t before, after;
			int i;

			fst_requires(w);
			switch_refresh_watch_arm(w);
			switch_refresh_watch_totals(&before);

			for (i = 0; i < 4; i++) {
				slow_op(w, SWITCH_RWATCH_STAGE_WRITE, 2000);
			}
			poll_and_flush();
			fst_check(count_records(NULL, NULL, NULL, NULL) == 3);

			switch_refresh_watch_destroy(&w);
			poll_and_flush();
			switch_refresh_watch_totals(&after);
			fst_check(after.suppressed_call == before.suppressed_call + 1);
			fst_check(after.watches == before.watches - 1);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(nested_operations_and_depth_overflow)
		{
			switch_refresh_watch_t *w = switch_refresh_watch_create(switch_core_session_get_uuid(fst_session), fst_channel, STALL_US);
			const char *rec;
			int h[10], i, outer, inner;

			fst_requires(w);
			switch_refresh_watch_arm(w);

			outer = switch_refresh_watch_begin(w, SWITCH_RWATCH_STAGE_READ, NULL);
			inner = switch_refresh_watch_begin(w, SWITCH_RWATCH_STAGE_BUG_READ, NULL);
			fst_check(outer == 0 && inner == 1);
			switch_sleep(SLOW_US);
			switch_refresh_watch_end(w, inner);
			switch_refresh_watch_end(w, outer);

			poll_and_flush();
			fst_requires(count_records(" stage=bug_read ", " depth=2 ", " parent_op=1 ", &rec) == 1);
			fst_check(field(rec, "op") == 2);
			fst_requires(count_records(" stage=read ", " depth=1 ", " parent_op=0 ", &rec) == 1);
			fst_check(field(rec, "op") == 1);
			clear_records();

			for (i = 0; i < 10; i++) {
				h[i] = switch_refresh_watch_begin(w, SWITCH_RWATCH_STAGE_READ, NULL);
			}
			fst_check(h[7] == 7);
			fst_check(h[8] == -1 && h[9] == -1);
			for (i = 9; i >= 0; i--) {
				switch_refresh_watch_end(w, h[i]);
			}

			/* the per-call allowance (3 per 5 s) has one record left */
			slow_op(w, SWITCH_RWATCH_STAGE_WRITE, SLOW_US);
			poll_and_flush();
			fst_check(count_records(" stage=write ", " overflow=2 ", " depth=1 ", NULL) == 1);

			switch_refresh_watch_destroy(&w);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(ring_overflow_and_per_call_limit)
		{
			switch_refresh_watch_t *w = switch_refresh_watch_create(switch_core_session_get_uuid(fst_session), fst_channel, 1000);
			const char *rec;
			int i;

			fst_requires(w);
			switch_refresh_watch_arm(w);

			/* 20 slow operations without a pass: the ring keeps 16 */
			for (i = 0; i < 20; i++) {
				slow_op(w, SWITCH_RWATCH_STAGE_WRITE, 2000);
			}

			poll_and_flush();
			fst_check(count_records(NULL, NULL, NULL, NULL) == 3);
			fst_check(count_records(" op=1 ", " lost=4 ", " suppressed_call=0 ", NULL) == 1);
			fst_check(count_records(" op=2 ", " lost=0 ", NULL, NULL) == 1);
			fst_check(count_records(" op=17 ", NULL, NULL, NULL) == 0);

			switch_sleep(5100000);
			slow_op(w, SWITCH_RWATCH_STAGE_WRITE, 2000);
			poll_and_flush();
			fst_requires(count_records(" op=21 ", NULL, NULL, &rec) == 1);
			fst_check(field(rec, "suppressed_call") == 13);
			fst_check(field(rec, "lost") == 0);

			switch_refresh_watch_destroy(&w);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(process_limit_across_watches)
		{
			switch_refresh_watch_t *w[41];
			const char *rec;
			int i, j;

			/* start in a fresh one-second process window */
			switch_sleep(1100000);

			for (i = 0; i < 41; i++) {
				w[i] = switch_refresh_watch_create(switch_core_session_get_uuid(fst_session), fst_channel, 1000);
				fst_requires(w[i]);
				switch_refresh_watch_arm(w[i]);
			}

			for (i = 0; i < 40; i++) {
				for (j = 0; j < 3; j++) {
					slow_op(w[i], SWITCH_RWATCH_STAGE_READ, 1500);
				}
			}

			poll_and_flush();
			fst_check(count_records(NULL, NULL, NULL, NULL) == 100);
			clear_records();

			switch_sleep(1100000);
			slow_op(w[40], SWITCH_RWATCH_STAGE_READ, 1500);
			poll_and_flush();
			fst_requires(count_records(" stage=read ", NULL, NULL, &rec) == 1);
			fst_check(field(rec, "suppressed_process") == 20);

			for (i = 0; i < 41; i++) {
				switch_refresh_watch_destroy(&w[i]);
			}
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(destroy_keeps_pending_records)
		{
			switch_refresh_watch_t *w = switch_refresh_watch_create(switch_core_session_get_uuid(fst_session), fst_channel, STALL_US);
			fst_requires(w);
			switch_refresh_watch_arm(w);
			switch_refresh_watch_bind(w);
			slow_op(w, SWITCH_RWATCH_STAGE_READ, SLOW_US);

			/* destroy also unbinds the calling thread when the watch is its own */
			switch_refresh_watch_destroy(&w);
			fst_check(switch_refresh_watch_current() == NULL);

			poll_and_flush();
			fst_check(count_records("status=completed", " stage=read ", NULL, NULL) == 1);

			/* the record carries no channel name */
			fst_check(count_records(NULL, NULL, NULL, NULL) == 1);
			fst_check(count_records(switch_channel_get_name(fst_channel), NULL, NULL, NULL) == 0);
			fst_check(count_records("15553334444", NULL, NULL, NULL) == 0);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(read_frame_and_bug_callback_timed)
		{
			switch_refresh_watch_t *w = switch_refresh_watch_create(switch_core_session_get_uuid(fst_session), fst_channel, STALL_US);
			switch_media_bug_t *bug = NULL;
			slow_bug_t sb = { SWITCH_ABC_TYPE_READ_REPLACE, SLOW_US, 0 };
			switch_frame_t *frame = NULL;
			const char *rec;
			int i;

			fst_requires(w);
			switch_refresh_watch_arm(w);
			fst_requires(switch_core_media_bug_add(fst_session, "refresh_watch_test", NULL, slow_bug_callback, &sb, 0,
												   SMBF_READ_REPLACE | SMBF_NO_PAUSE, &bug) == SWITCH_STATUS_SUCCESS);

			switch_refresh_watch_bind(w);
			for (i = 0; i < 5 && !sb.calls; i++) {
				switch_core_session_read_frame(fst_session, &frame, SWITCH_IO_FLAG_NONE, 0);
			}
			switch_refresh_watch_bind(NULL);
			switch_core_media_bug_remove(fst_session, &bug);

			fst_requires(sb.calls == 1);
			poll_and_flush();
			fst_requires(count_records(" stage=bug_read_replace ", " depth=2 ", NULL, &rec) == 1);
			fst_check(field(rec, "elapsed_us") >= SLOW_US);
			fst_requires(count_records(" stage=read ", " depth=1 ", NULL, &rec) == 1);
			fst_check(field(rec, "elapsed_us") >= SLOW_US);
			fst_check(strstr(rec, "target=") == NULL);

			switch_refresh_watch_destroy(&w);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(cross_session_write_attributed_to_writer)
		{
			switch_refresh_watch_t *w = switch_refresh_watch_create(switch_core_session_get_uuid(fst_session), fst_channel, STALL_US);
			switch_core_session_t *other = NULL;
			switch_call_cause_t cause = SWITCH_CAUSE_NONE;
			switch_media_bug_t *bug = NULL;
			slow_bug_t sb = { SWITCH_ABC_TYPE_WRITE_REPLACE, SLOW_US, 0 };
			switch_codec_implementation_t impl = { 0 };
			switch_frame_t frame = { 0 };
			unsigned char data[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			char target[SWITCH_UUID_FORMATTED_LENGTH + 16];
			const char *rec;

			fst_requires(w);
			switch_refresh_watch_arm(w);
			fst_requires(switch_ivr_originate(NULL, &other, &cause, "null/+15553335555", 2, NULL, NULL, NULL, NULL, NULL, SOF_NONE,
											  NULL, NULL) == SWITCH_STATUS_SUCCESS && other);
			fst_requires(switch_core_session_get_write_impl(other, &impl) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_media_bug_add(other, "refresh_watch_test", NULL, slow_bug_callback, &sb, 0,
												   SMBF_WRITE_REPLACE | SMBF_NO_PAUSE, &bug) == SWITCH_STATUS_SUCCESS);

			frame.data = data;
			frame.buflen = sizeof(data);
			frame.codec = switch_core_session_get_write_codec(other);
			frame.datalen = impl.decoded_bytes_per_packet;
			frame.samples = impl.samples_per_packet;
			frame.rate = impl.actual_samples_per_second;

			switch_refresh_watch_bind(w);
			switch_core_session_write_frame(other, &frame, SWITCH_IO_FLAG_NONE, 0);
			switch_refresh_watch_bind(NULL);

			switch_core_media_bug_remove(other, &bug);
			switch_snprintf(target, sizeof(target), " target=%s ", switch_core_session_get_uuid(other));
			switch_channel_hangup(switch_core_session_get_channel(other), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(other);

			fst_requires(sb.calls == 1);
			poll_and_flush();
			fst_requires(count_records(" stage=write ", target, " depth=1 ", &rec) == 1);
			fst_check(field(rec, "elapsed_us") >= SLOW_US);
			fst_check(count_records(" stage=bug_write_replace ", target, " depth=2 ", NULL) == 1);

			switch_refresh_watch_destroy(&w);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(enable_is_idempotent_and_answer_arms)
		{
			switch_media_bug_t *bug = NULL;
			slow_bug_t sb = { SWITCH_ABC_TYPE_READ_REPLACE, SLOW_US, 0 };
			switch_frame_t *frame = NULL;
			int i;

			/* enabled on an unanswered call from another thread: not bound, not armed */
			switch_channel_clear_flag(fst_channel, CF_ANSWERED);
			fst_check(switch_core_session_refresh_watch_enable(fst_session, STALL_US) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_session_refresh_watch_enable(fst_session, STALL_US) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_refresh_watch_current() == NULL);

			fst_requires(switch_core_media_bug_add(fst_session, "refresh_watch_test", NULL, slow_bug_callback, &sb, 0,
												   SMBF_READ_REPLACE | SMBF_NO_PAUSE, &bug) == SWITCH_STATUS_SUCCESS);

			/* the session's own thread binds the watch; this test thread stands in for it */
			switch_refresh_watch_bind(switch_core_session_get_refresh_watch(fst_session));
			fst_requires(switch_refresh_watch_current() != NULL);
			for (i = 0; i < 5 && !sb.calls; i++) {
				switch_core_session_read_frame(fst_session, &frame, SWITCH_IO_FLAG_NONE, 0);
			}
			poll_and_flush();
			fst_check(count_records(NULL, NULL, NULL, NULL) == 0);

			switch_channel_mark_answered(fst_channel);
			sb.calls = 0;
			for (i = 0; i < 5 && !sb.calls; i++) {
				switch_core_session_read_frame(fst_session, &frame, SWITCH_IO_FLAG_NONE, 0);
			}
			switch_refresh_watch_bind(NULL);
			switch_core_media_bug_remove(fst_session, &bug);

			poll_and_flush();
			fst_check(count_records("status=completed", " stage=read ", NULL, NULL) == 1);
			fst_check(count_records("status=completed", " stage=bug_read_replace ", NULL, NULL) == 1);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(late_enable_binds_on_owner_thread)
		{
			switch_media_bug_t *bug = NULL;
			slow_bug_t sb = { SWITCH_ABC_TYPE_READ_REPLACE, SLOW_US, 0 };
			int i;

			/* the session thread is already running: it must pick the watch up at its own next frame call */
			fst_check(switch_core_session_refresh_watch_enable(fst_session, STALL_US) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_refresh_watch_current() == NULL);
			fst_requires(switch_core_media_bug_add(fst_session, "refresh_watch_test", NULL, slow_bug_callback, &sb, 0,
												   SMBF_READ_REPLACE | SMBF_NO_PAUSE, &bug) == SWITCH_STATUS_SUCCESS);
			/* park: the session's own thread loops reading frames */
			switch_channel_set_state(fst_channel, CS_PARK);

			for (i = 0; i < 200 && !count_records("status=completed", " stage=bug_read_replace ", NULL, NULL); i++) {
				switch_sleep(10000);
				poll_and_flush();
			}
			switch_core_media_bug_remove(fst_session, &bug);

			fst_check(sb.calls > 0);
			fst_check(count_records("status=completed", " stage=bug_read_replace ", " depth=2 ", NULL) >= 1);
			fst_check(count_records("status=completed", " stage=read ", " depth=1 ", NULL) >= 1);
			fst_check(count_records("target=", NULL, NULL, NULL) == 0);
			fst_check(switch_refresh_watch_current() == NULL);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(observer_thread_reports_without_manual_poll)
		{
			switch_refresh_watch_t *w = switch_refresh_watch_create(switch_core_session_get_uuid(fst_session), fst_channel, STALL_US);
			int i;

			fst_requires(w);
			switch_refresh_watch_arm(w);
			fst_check(switch_refresh_watch_observer_start() == SWITCH_STATUS_SUCCESS);
			fst_check(switch_refresh_watch_observer_start() == SWITCH_STATUS_SUCCESS);

			slow_op(w, SWITCH_RWATCH_STAGE_WRITE, SLOW_US);
			for (i = 0; i < 50 && !count_records("status=completed", NULL, NULL, NULL); i++) {
				switch_sleep(10000);
			}
			fst_check(count_records("status=completed", " stage=write ", NULL, NULL) == 1);

			switch_refresh_watch_destroy(&w);
		}
		FST_SESSION_END()
	}
	FST_SUITE_END()
}
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
