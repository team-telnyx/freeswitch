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
 * test_refresh_diag.c -- Tests for the refresh telemetry around Sofia signal dispatch
 *
 */

#include <switch.h>
#include <test/switch_test.h>
#include "../mod_sofia.h"

/* The test binary links its own static copy of mod_sofia: it initialises that copy's refresh_diag_mutex itself. */

typedef struct {
	sofia_dispatch_event_t de;
	nua_event_data_t data;
	sip_t sip;
	sip_cseq_t cseq;
} fake_event_t;

typedef struct {
	switch_time_t sleep_us;
	int saw_context;
	switch_refresh_diag_t *context;
	int reads;
	switch_core_session_t *nested_session;
	private_object_t *nested_pvt;
	fake_event_t *nested_event;
	switch_refresh_diag_t *after_nested;
} fake_dispatch_t;

static __thread fake_dispatch_t *fake_dispatch_state = NULL;

#define MAX_RECORDS 256

static switch_memory_pool_t *suite_pool = NULL;
static switch_mutex_t *records_mutex = NULL;
static char *records[MAX_RECORDS];
static int record_count = 0;

static switch_status_t capture_log(const switch_log_node_t *node, switch_log_level_t level)
{
	if (node->data && strstr(node->data, "SIP_REFRESH_WORK")) {
		switch_mutex_lock(records_mutex);
		if (record_count < MAX_RECORDS) {
			records[record_count++] = strdup(node->data);
		}
		switch_mutex_unlock(records_mutex);
	}

	return SWITCH_STATUS_SUCCESS;
}

static void clear_records(void)
{
	int i;

	switch_mutex_lock(records_mutex);
	for (i = 0; i < record_count; i++) {
		switch_safe_free(records[i]);
	}
	record_count = 0;
	switch_mutex_unlock(records_mutex);
}

/* Logging is asynchronous: wait until the record count stops changing. */
static int settled_records(void)
{
	int last = -1, now, stable = 0, i;

	for (i = 0; i < 100 && stable < 3; i++) {
		switch_yield(20000);
		switch_mutex_lock(records_mutex);
		now = record_count;
		switch_mutex_unlock(records_mutex);
		stable = now == last ? stable + 1 : 0;
		last = now;
	}

	return last;
}

static const char *record(int i)
{
	return i < record_count ? records[i] : "";
}

static switch_time_t field_us(const char *line, const char *key)
{
	const char *p = strstr(line, key);

	return p ? (switch_time_t) atoll(p + strlen(key)) : -1;
}

static fake_event_t *fake_event(nua_event_t event, uint32_t cseq, switch_time_t queued_us)
{
	fake_event_t *fe = calloc(1, sizeof(*fe));
	switch_time_t now = switch_refresh_diag_now();

	fe->data.e_event = event;
	fe->cseq.cs_seq = cseq;
	fe->sip.sip_cseq = &fe->cseq;
	fe->de.data = &fe->data;
	fe->de.sip = &fe->sip;
	fe->de.diag_created = now - queued_us - 10;
	fe->de.diag_handoff = now - queued_us;

	return fe;
}

/* Stands in for sofia_process_dispatch_event: reads the event, may nest or sleep, then frees it as the real one does. */
static void fake_process(sofia_dispatch_event_t **dep)
{
	sofia_dispatch_event_t *de = *dep;
	fake_dispatch_t *st = fake_dispatch_state;

	if (st) {
		st->reads += de->data->e_event == nua_i_invite;
		st->context = switch_refresh_diag_current();
		st->saw_context = st->context != NULL;

		if (st->nested_event) {
			fake_event_t *nested = st->nested_event;

			st->nested_event = NULL;
			sofia_dispatch_signal_data(st->nested_session, st->nested_pvt, &nested->de, fake_process);
			st->after_nested = switch_refresh_diag_current();
		}

		if (st->sleep_us) {
			switch_yield(st->sleep_us);
		}
	}

	/* the real dispatch frees the event and clears the caller's pointer */
	memset(de, 0x5a, sizeof(fake_event_t));
	free(de);
	*dep = NULL;
}

static private_object_t *fake_pvt(switch_core_session_t *session, int enabled, switch_time_t slow_us)
{
	private_object_t *tech_pvt = switch_core_session_alloc(session, sizeof(*tech_pvt));

	switch_mutex_init(&tech_pvt->sofia_mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));
	tech_pvt->refresh_diag_enabled = enabled;
	tech_pvt->refresh_diag_slow_us = slow_us;
	tech_pvt->refresh_diag_lock_us = 1000;

	return tech_pvt;
}

static void reset_process_limit(void)
{
	switch_mutex_lock(mod_sofia_globals.refresh_diag_mutex);
	memset(&mod_sofia_globals.refresh_diag_limit, 0, sizeof(mod_sofia_globals.refresh_diag_limit));
	switch_mutex_unlock(mod_sofia_globals.refresh_diag_mutex);
}

typedef struct {
	switch_mutex_t *mutex;
	switch_time_t hold_us;
	volatile int held;
} holder_t;

static void *SWITCH_THREAD_FUNC hold_lock(switch_thread_t *thread, void *obj)
{
	holder_t *h = obj;

	switch_mutex_lock(h->mutex);
	h->held = 1;
	switch_yield(h->hold_us);
	switch_mutex_unlock(h->mutex);

	return NULL;
}

typedef struct {
	switch_core_session_t *session;
	private_object_t **pvts;
	int npvts;
	int per_pvt;
} burst_t;

static void *SWITCH_THREAD_FUNC burst(switch_thread_t *thread, void *obj)
{
	burst_t *b = obj;
	int i, j;

	for (j = 0; j < b->per_pvt; j++) {
		for (i = 0; i < b->npvts; i++) {
			sofia_dispatch_signal_data(b->session, b->pvts[i], &fake_event(nua_i_state, 1, 1000)->de, fake_process);
		}
	}

	return NULL;
}

FST_CORE_BEGIN("./conf-refresh-diag")
{
	FST_SUITE_BEGIN(refresh_diag)
	{
		FST_SETUP_BEGIN()
		{
			/* fst_pool is destroyed after each test: the mutexes live in a pool kept for the whole suite */
			if (!suite_pool) {
				switch_core_new_memory_pool(&suite_pool);
				switch_mutex_init(&records_mutex, SWITCH_MUTEX_NESTED, suite_pool);
				switch_mutex_init(&mod_sofia_globals.refresh_diag_mutex, SWITCH_MUTEX_NESTED, suite_pool);
				switch_log_bind_logger(capture_log, SWITCH_LOG_DEBUG, SWITCH_FALSE);
			}
			reset_process_limit();
			clear_records();
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
			fake_dispatch_state = NULL;
		}
		FST_TEARDOWN_END()

		FST_SESSION_BEGIN(disabled_or_unanswered_dispatches_without_context)
		{
			fake_dispatch_t st = { 0 };
			private_object_t *off = fake_pvt(fst_session, 0, 0);
			private_object_t *on = fake_pvt(fst_session, 1, 0);

			fake_dispatch_state = &st;

			switch_channel_set_flag(fst_channel, CF_ANSWERED);
			sofia_dispatch_signal_data(fst_session, off, &fake_event(nua_i_invite, 1, 0)->de, fake_process);
			fst_check(st.reads == 1);
			fst_check(!st.saw_context);

			switch_channel_clear_flag(fst_channel, CF_ANSWERED);
			sofia_dispatch_signal_data(fst_session, on, &fake_event(nua_i_invite, 2, 0)->de, fake_process);
			fst_check(st.reads == 2);
			fst_check(!st.saw_context);

			/* answered and enabled, but not an event kind that is timed */
			switch_channel_set_flag(fst_channel, CF_ANSWERED);
			sofia_dispatch_signal_data(fst_session, on, &fake_event(nua_i_bye, 3, 0)->de, fake_process);
			fst_check(!st.saw_context);

			fst_check(switch_refresh_diag_current() == NULL);
			fst_check(settled_records() == 0);
			fst_check(on->refresh_diag_baseline == 0);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(baseline_record_once_per_event_kind)
		{
			fake_dispatch_t st = { 0 };
			private_object_t *pvt = fake_pvt(fst_session, 1, 10000000);
			const char *line;

			fake_dispatch_state = &st;
			switch_channel_set_flag(fst_channel, CF_ANSWERED);

			sofia_dispatch_signal_data(fst_session, pvt, &fake_event(nua_i_invite, 101, 0)->de, fake_process);
			fst_check(st.saw_context);
			fst_check(switch_refresh_diag_current() == NULL);
			sofia_dispatch_signal_data(fst_session, pvt, &fake_event(nua_i_invite, 102, 0)->de, fake_process);
			sofia_dispatch_signal_data(fst_session, pvt, &fake_event(nua_i_ack, 102, 0)->de, fake_process);

			fst_check(settled_records() == 2);
			line = record(0);
			fst_check(strstr(line, "schema=1 kind=dispatch event=nua_i_invite cseq=101 reason=baseline ") != NULL);
			fst_check(strstr(line, " handoff_us=") != NULL);
			fst_check(strstr(line, " queue_us=") != NULL);
			fst_check(strstr(line, " suppressed_call=0 suppressed_process=0") != NULL);
			fst_check(strstr(record(1), "event=nua_i_ack cseq=102 reason=baseline ") != NULL);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(delayed_state_event_with_fast_handler_is_queue_time)
		{
			fake_dispatch_t st = { 0 };
			private_object_t *pvt = fake_pvt(fst_session, 1, 10000);

			fake_dispatch_state = &st;
			switch_channel_set_flag(fst_channel, CF_ANSWERED);
			pvt->refresh_diag_baseline = 7;

			/* fast INVITE handler: under the slow threshold, nothing reported */
			sofia_dispatch_signal_data(fst_session, pvt, &fake_event(nua_i_invite, 5, 0)->de, fake_process);
			/* the following state event waited 50 ms in the queue, then ran fast */
			sofia_dispatch_signal_data(fst_session, pvt, &fake_event(nua_i_state, 5, 50000)->de, fake_process);

			fst_check(settled_records() == 1);
			fst_check(strstr(record(0), "event=nua_i_state cseq=5 reason=slow ") != NULL);
			fst_check(field_us(record(0), " queue_us=") >= 50000);
			fst_check(field_us(record(0), " handler_us=") < 10000);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(slow_handler_is_handler_time)
		{
			fake_dispatch_t st = { 0 };
			private_object_t *pvt = fake_pvt(fst_session, 1, 10000);

			fake_dispatch_state = &st;
			switch_channel_set_flag(fst_channel, CF_ANSWERED);
			pvt->refresh_diag_baseline = 7;
			st.sleep_us = 30000;

			sofia_dispatch_signal_data(fst_session, pvt, &fake_event(nua_i_ack, 9, 0)->de, fake_process);

			fst_check(settled_records() == 1);
			fst_check(strstr(record(0), "event=nua_i_ack cseq=9 reason=slow ") != NULL);
			fst_check(field_us(record(0), " handler_us=") >= 30000);
			fst_check(field_us(record(0), " queue_us=") < 10000);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(default_slow_threshold_skips_one_frame_queue_wait)
		{
			fake_dispatch_t st = { 0 };
			sofia_profile_t *profile = switch_core_session_alloc(fst_session, sizeof(*profile));
			private_object_t *pvt = fake_pvt(fst_session, 0, 0);

			profile->refresh_work_telemetry = 1;
			sofia_glue_copy_refresh_diag_settings(pvt, profile);
			fst_check(pvt->refresh_diag_enabled == 1);
			fst_check(pvt->refresh_diag_slow_us == SOFIA_REFRESH_WORK_SLOW_MS_DEFAULT * 1000);
			fst_check(pvt->refresh_diag_lock_us == SOFIA_REFRESH_WORK_LOCK_MS_DEFAULT * 1000);
			fst_check(SOFIA_REFRESH_WORK_SLOW_MS_DEFAULT * 1000 > 2 * 20000);

			fake_dispatch_state = &st;
			switch_channel_set_flag(fst_channel, CF_ANSWERED);
			pvt->refresh_diag_baseline = 7;

			/* a state event that waited one 20 ms frame (plus scheduling) for the session thread: not slow */
			sofia_dispatch_signal_data(fst_session, pvt, &fake_event(nua_i_state, 3, 25000)->de, fake_process);
			fst_check(settled_records() == 0);
			/* one that waited past the default: reported, attributed to queue time */
			sofia_dispatch_signal_data(fst_session, pvt, &fake_event(nua_i_state, 3, SOFIA_REFRESH_WORK_SLOW_MS_DEFAULT * 1000 + 20000)->de,
									   fake_process);
			fst_check(settled_records() == 1);
			fst_check(strstr(record(0), "event=nua_i_state cseq=3 reason=slow ") != NULL);
			fst_check(field_us(record(0), " queue_us=") > SOFIA_REFRESH_WORK_SLOW_MS_DEFAULT * 1000);

			/* an explicit profile value still wins */
			profile->refresh_work_slow_ms = 20;
			profile->refresh_work_lock_ms = 5;
			sofia_glue_copy_refresh_diag_settings(pvt, profile);
			fst_check(pvt->refresh_diag_slow_us == 20000);
			fst_check(pvt->refresh_diag_lock_us == 5000);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(sofia_lock_wait_is_attributed)
		{
			fake_dispatch_t st = { 0 };
			private_object_t *pvt = fake_pvt(fst_session, 1, 10000000);
			holder_t h = { 0 };
			switch_thread_t *thread;
			switch_threadattr_t *attr;
			switch_status_t ret;
			const char *line;

			fake_dispatch_state = &st;
			switch_channel_set_flag(fst_channel, CF_ANSWERED);
			pvt->refresh_diag_baseline = 7;

			h.mutex = pvt->sofia_mutex;
			h.hold_us = 40000;
			switch_threadattr_create(&attr, fst_pool);
			switch_thread_create(&thread, attr, hold_lock, &h, fst_pool);
			while (!h.held) {
				switch_yield(1000);
			}

			sofia_dispatch_signal_data(fst_session, pvt, &fake_event(nua_i_invite, 11, 0)->de, fake_process);
			switch_thread_join(&ret, thread);

			fst_check(settled_records() == 1);
			line = record(0);
			fst_check(strstr(line, "event=nua_i_invite cseq=11 reason=lock ") != NULL);
			fst_check(field_us(line, " sofia_lock_us=") >= 20000);
			fst_check(strstr(line, " lock_sofia_count=1 ") != NULL);
			fst_check(field_us(line, " lock_sofia_max_us=") == field_us(line, " sofia_lock_us="));
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(nested_dispatch_restores_outer_context)
		{
			fake_dispatch_t st = { 0 };
			private_object_t *outer = fake_pvt(fst_session, 1, 10000000);
			private_object_t *inner = fake_pvt(fst_session, 1, 10000000);
			switch_refresh_diag_t *outer_context;

			fake_dispatch_state = &st;
			switch_channel_set_flag(fst_channel, CF_ANSWERED);
			st.nested_session = fst_session;
			st.nested_pvt = inner;
			st.nested_event = fake_event(nua_i_ack, 21, 0);

			sofia_dispatch_signal_data(fst_session, outer, &fake_event(nua_i_invite, 21, 0)->de, fake_process);

			/* st.context was overwritten by the inner dispatch; the outer one must be current again after it */
			outer_context = st.after_nested;
			fst_check(outer_context != NULL);
			fst_check(outer_context != st.context);
			fst_check(switch_refresh_diag_current() == NULL);

			fst_check(settled_records() == 2);
			fst_check(strstr(record(0), "event=nua_i_ack cseq=21 reason=baseline ") != NULL);
			fst_check(strstr(record(1), "event=nua_i_invite cseq=21 reason=baseline ") != NULL);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(per_call_limit_and_suppressed_count)
		{
			fake_dispatch_t st = { 0 };
			private_object_t *pvt = fake_pvt(fst_session, 1, 0);
			int i;

			fake_dispatch_state = &st;
			switch_channel_set_flag(fst_channel, CF_ANSWERED);
			pvt->refresh_diag_baseline = 7;

			/* 1 ms of queue time makes every event exceed the 0 us slow threshold (a sub-microsecond dispatch measures 0) */
			for (i = 0; i < 5; i++) {
				sofia_dispatch_signal_data(fst_session, pvt, &fake_event(nua_i_state, 30 + i, 1000)->de, fake_process);
			}
			fst_check(settled_records() == 3);
			fst_check(pvt->refresh_diag_limit.suppressed == 2);

			/* end the 5 s window without waiting for it */
			pvt->refresh_diag_limit.window_start -= 5000000;
			sofia_dispatch_signal_data(fst_session, pvt, &fake_event(nua_i_state, 40, 1000)->de, fake_process);
			fst_check(settled_records() == 4);
			fst_check(strstr(record(3), " suppressed_call=2 ") != NULL);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(process_limit_holds_under_concurrency)
		{
			enum { THREADS = 4, PVTS = 15 };
			burst_t b[THREADS];
			private_object_t *pvts[THREADS][PVTS];
			switch_thread_t *threads[THREADS];
			switch_threadattr_t *attr;
			switch_status_t ret;
			switch_time_t start, elapsed;
			int t, i, n;

			switch_channel_set_flag(fst_channel, CF_ANSWERED);
			switch_threadattr_create(&attr, fst_pool);

			for (t = 0; t < THREADS; t++) {
				for (i = 0; i < PVTS; i++) {
					pvts[t][i] = fake_pvt(fst_session, 1, 0);
					pvts[t][i]->refresh_diag_baseline = 7;
				}
				b[t].session = fst_session;
				b[t].pvts = pvts[t];
				b[t].npvts = PVTS;
				b[t].per_pvt = 3;
			}

			/* 4 x 15 calls x 3 slow events = 180 reportable records, all inside one process window */
			start = switch_refresh_diag_now();
			for (t = 0; t < THREADS; t++) {
				switch_thread_create(&threads[t], attr, burst, &b[t], fst_pool);
			}
			for (t = 0; t < THREADS; t++) {
				switch_thread_join(&ret, threads[t]);
			}
			elapsed = switch_refresh_diag_now() - start;

			n = settled_records();
			fst_check(elapsed < 900000);
			fst_check(n == 100);
			fst_check(mod_sofia_globals.refresh_diag_limit.suppressed == 80);
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
