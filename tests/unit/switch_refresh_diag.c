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
 *
 *
 * switch_refresh_diag.c -- tests the opt-in in-dialog request timing context
 *
 */
#include <switch.h>
#include <switch_refresh_diag.h>
#include <test/switch_test.h>

typedef struct {
	switch_mutex_t *mutex;
	switch_time_t hold_us;
	volatile int locked;
	switch_refresh_diag_t *seen;
} rdiag_holder_t;

static void *SWITCH_THREAD_FUNC rdiag_holder_thread(switch_thread_t *thread, void *obj)
{
	rdiag_holder_t *h = (rdiag_holder_t *) obj;

	h->seen = switch_refresh_diag_current();
	switch_mutex_lock(h->mutex);
	h->locked = 1;
	switch_sleep(h->hold_us);
	switch_mutex_unlock(h->mutex);

	return NULL;
}

static void rdiag_start_holder(rdiag_holder_t *h, switch_memory_pool_t *pool, switch_thread_t **thread)
{
	switch_threadattr_t *attr;
	int i;

	switch_threadattr_create(&attr, pool);
	switch_thread_create(thread, attr, rdiag_holder_thread, h, pool);

	for (i = 0; i < 1000 && !h->locked; i++) {
		switch_sleep(1000);
	}
}

static int rdiag_tracked_calls = 0;

static void rdiag_track_cb(switch_core_session_t *session, const char *xml)
{
	if (xml && *xml) {
		rdiag_tracked_calls++;
	}
}

FST_CORE_BEGIN("./conf")
{
	FST_SUITE_BEGIN(switch_refresh_diag)
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

		FST_TEST_BEGIN(push_pop_nesting)
		{
			switch_refresh_diag_t a, b;

			switch_refresh_diag_init(&a, 1000);
			switch_refresh_diag_init(&b, 1000);

			fst_check(switch_refresh_diag_current() == NULL);
			switch_refresh_diag_push(&a);
			fst_check(switch_refresh_diag_current() == &a);
			switch_refresh_diag_push(&b);
			fst_check(switch_refresh_diag_current() == &b);
			switch_refresh_diag_pop(&b);
			fst_check(switch_refresh_diag_current() == &a);
			switch_refresh_diag_pop(&a);
			fst_check(switch_refresh_diag_current() == NULL);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(context_is_per_thread)
		{
			switch_refresh_diag_t a;
			rdiag_holder_t h = { 0 };
			switch_thread_t *thread = NULL;
			switch_status_t st;

			switch_refresh_diag_init(&a, 1000);
			switch_mutex_init(&h.mutex, SWITCH_MUTEX_NESTED, fst_pool);
			h.hold_us = 1000;
			h.seen = &a;

			switch_refresh_diag_push(&a);
			rdiag_start_holder(&h, fst_pool, &thread);
			switch_thread_join(&st, thread);
			switch_refresh_diag_pop(&a);

			fst_check(h.seen == NULL);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(lock_wait_threshold_is_strict)
		{
			switch_refresh_diag_t d;

			switch_refresh_diag_init(&d, 1000);
			switch_refresh_diag_lock_wait(&d, SWITCH_RDIAG_LOCK_SET_VARIABLE, 1000);
			fst_check(d.locks[SWITCH_RDIAG_LOCK_SET_VARIABLE].count == 0);

			switch_refresh_diag_lock_wait(&d, SWITCH_RDIAG_LOCK_SET_VARIABLE, 1001);
			switch_refresh_diag_lock_wait(&d, SWITCH_RDIAG_LOCK_SET_VARIABLE, 5000);
			switch_refresh_diag_lock_wait(&d, SWITCH_RDIAG_LOCK_SET_VARIABLE, 2000);
			fst_check(d.locks[SWITCH_RDIAG_LOCK_SET_VARIABLE].count == 3);
			fst_check(d.locks[SWITCH_RDIAG_LOCK_SET_VARIABLE].max_us == 5000);
			fst_check(d.locks[SWITCH_RDIAG_LOCK_SET_VARIABLE].total_us == 8001);
			fst_check(switch_refresh_diag_max_lock_wait(&d) == 5000);

			switch_refresh_diag_lock_wait(NULL, SWITCH_RDIAG_LOCK_SET_VARIABLE, 5000);
			switch_refresh_diag_lock_wait(&d, SWITCH_RDIAG_LOCK_MAX, 5000);
			fst_check(d.locks[SWITCH_RDIAG_LOCK_SET_VARIABLE].count == 3);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(contended_mutex_is_attributed)
		{
			switch_refresh_diag_t d;
			rdiag_holder_t h = { 0 };
			switch_thread_t *thread = NULL;
			switch_status_t st;

			switch_refresh_diag_init(&d, 1000);
			switch_mutex_init(&h.mutex, SWITCH_MUTEX_NESTED, fst_pool);
			h.hold_us = 50000;

			rdiag_start_holder(&h, fst_pool, &thread);
			fst_requires(h.locked);

			switch_refresh_diag_push(&d);
			switch_refresh_diag_mutex_lock(h.mutex, SWITCH_RDIAG_LOCK_RTP_STATS);
			switch_refresh_diag_pop(&d);
			switch_mutex_unlock(h.mutex);
			switch_thread_join(&st, thread);

			fst_check(d.locks[SWITCH_RDIAG_LOCK_RTP_STATS].count == 1);
			fst_check(d.locks[SWITCH_RDIAG_LOCK_RTP_STATS].max_us >= 20000);
			fst_check(d.locks[SWITCH_RDIAG_LOCK_SET_VARIABLE].count == 0);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(uncontended_and_no_context)
		{
			switch_refresh_diag_t d;
			switch_mutex_t *m;

			switch_mutex_init(&m, SWITCH_MUTEX_NESTED, fst_pool);
			switch_refresh_diag_init(&d, 1000);

			switch_refresh_diag_mutex_lock(m, SWITCH_RDIAG_LOCK_SOFIA);
			switch_mutex_unlock(m);

			switch_refresh_diag_push(&d);
			switch_refresh_diag_mutex_lock(m, SWITCH_RDIAG_LOCK_SOFIA);
			switch_mutex_unlock(m);
			switch_refresh_diag_pop(&d);

			fst_check(d.locks[SWITCH_RDIAG_LOCK_SOFIA].count == 0);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(format_leaves_out_unmeasured)
		{
			switch_refresh_diag_t d;
			char buf[1024];
			switch_size_t n;

			switch_refresh_diag_init(&d, 1000);
			n = switch_refresh_diag_format(&d, buf, sizeof(buf));
			fst_check(n == 0);
			fst_check_string_equals(buf, "");

			switch_refresh_diag_stage(&d, SWITCH_RDIAG_STAGE_RESPOND_100, 0);
			switch_refresh_diag_stage(&d, SWITCH_RDIAG_STAGE_RECOVERY, 1500);
			switch_refresh_diag_stage(&d, SWITCH_RDIAG_STAGE_RECOVERY, 500);
			switch_refresh_diag_lock_wait(&d, SWITCH_RDIAG_LOCK_SOFIA, 4000);
			n = switch_refresh_diag_format(&d, buf, sizeof(buf));

			fst_check(n == strlen(buf));
			fst_check(strstr(buf, "stage_respond_100_us=0") != NULL);
			fst_check(strstr(buf, "stage_recovery_us=2000") != NULL);
			fst_check(strstr(buf, "stage_session_id_us") == NULL);
			fst_check(strstr(buf, "recovery_calls") == NULL);
			fst_check(strstr(buf, "xml_builds") == NULL);
			fst_check(strstr(buf, "hook_searches") == NULL);
			fst_check(strstr(buf, "lock_sofia_count=1 lock_sofia_max_us=4000 lock_sofia_total_us=4000") != NULL);
			fst_check(strstr(buf, "lock_set_variable") == NULL);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(format_truncates_safely)
		{
			switch_refresh_diag_t d;
			char buf[24];
			int i;

			switch_refresh_diag_init(&d, 0);
			for (i = 0; i < SWITCH_RDIAG_STAGE_MAX; i++) {
				switch_refresh_diag_stage(&d, (switch_refresh_diag_stage_t) i, 123456);
			}

			memset(buf, 'x', sizeof(buf));
			switch_refresh_diag_format(&d, buf, sizeof(buf));
			fst_check(strlen(buf) < sizeof(buf));
		}
		FST_TEST_END()

		FST_TEST_BEGIN(limit_fixed_window)
		{
			switch_refresh_diag_limit_t l = { 0 };

			fst_check(switch_refresh_diag_limit_take(&l, 1000000, 5000000, 3));
			fst_check(switch_refresh_diag_limit_take(&l, 1100000, 5000000, 3));
			fst_check(switch_refresh_diag_limit_take(&l, 1200000, 5000000, 3));
			fst_check(!switch_refresh_diag_limit_take(&l, 1300000, 5000000, 3));
			fst_check(!switch_refresh_diag_limit_take(&l, 5999999, 5000000, 3));
			fst_check(switch_refresh_diag_limit_drain_suppressed(&l) == 2);
			fst_check(switch_refresh_diag_limit_drain_suppressed(&l) == 0);
			fst_check(switch_refresh_diag_limit_take(&l, 6000000, 5000000, 3));
			fst_check(l.used == 1);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(limit_late_timestamp_keeps_window)
		{
			switch_refresh_diag_limit_t l = { 0 };

			fst_check(switch_refresh_diag_limit_take(&l, 1000100, 1000000, 2));
			fst_check(switch_refresh_diag_limit_take(&l, 1000200, 1000000, 2));
			/* sampled before the window opened: still counted against it, never a reset */
			fst_check(!switch_refresh_diag_limit_take(&l, 1000000, 1000000, 2));
			fst_check(!switch_refresh_diag_limit_take(&l, 900000, 1000000, 2));
			fst_check(l.window_start == 1000100);
			fst_check(l.used == 2);
			fst_check(switch_refresh_diag_limit_drain_suppressed(&l) == 2);
			fst_check(switch_refresh_diag_limit_take(&l, 2000100, 1000000, 2));
			fst_check(l.used == 1);
		}
		FST_TEST_END()

		FST_SESSION_BEGIN(xml_cdr_counts_without_values)
		{
			switch_refresh_diag_t d;
			switch_xml_t cdr = NULL;
			char buf[2048];

			switch_channel_set_variable(fst_channel, "rdiag_private_name", "rdiag_private_value");

			switch_refresh_diag_init(&d, 1000);
			switch_refresh_diag_push(&d);
			fst_check(switch_ivr_generate_xml_cdr(fst_session, &cdr) == SWITCH_STATUS_SUCCESS);
			switch_refresh_diag_pop(&d);
			switch_xml_free(cdr);

			fst_check(d.xml_builds == 1);
			fst_check(d.xml_vars > 0);
			fst_check(d.xml_var_values >= d.xml_vars);
			fst_check(d.xml_profiles >= 1);

			switch_refresh_diag_format(&d, buf, sizeof(buf));
			fst_check(strstr(buf, "xml_builds=1") != NULL);
			fst_check(strstr(buf, "rdiag_private") == NULL);
			fst_check(strstr(buf, "5551112222") == NULL);
			fst_check(strstr(buf, "5553334444") == NULL);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(xml_cdr_untouched_without_context)
		{
			switch_refresh_diag_t d;
			switch_xml_t cdr = NULL;

			switch_refresh_diag_init(&d, 1000);
			fst_check(switch_ivr_generate_xml_cdr(fst_session, &cdr) == SWITCH_STATUS_SUCCESS);
			switch_xml_free(cdr);

			fst_check(d.xml_builds == 0);
			fst_check(d.xml_vars == 0);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(hook_search_counts)
		{
			switch_refresh_diag_t d;

			switch_channel_set_variable(fst_channel, "execute_on_rdiag_test", "set rdiag_hook_ran=yes");

			switch_refresh_diag_init(&d, 1000);
			switch_refresh_diag_push(&d);
			switch_channel_execute_on(fst_channel, "execute_on_rdiag_test");
			switch_channel_execute_on(fst_channel, "execute_on_rdiag_nothing");
			switch_channel_api_on(fst_channel, "api_on_rdiag_nothing");
			switch_refresh_diag_pop(&d);

			fst_check_string_equals(switch_str_nil(switch_channel_get_variable(fst_channel, "rdiag_hook_ran")), "yes");
			fst_check(d.hook_searches == 3);
			fst_check(d.hook_matched == 1);
			fst_check(d.hook_seen > 0);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(recovery_status_and_phases)
		{
			switch_refresh_diag_t d;

			rdiag_tracked_calls = 0;

			switch_refresh_diag_init(&d, 1000);
			switch_refresh_diag_push(&d);
			switch_core_recovery_track(fst_session);
			switch_refresh_diag_pop(&d);
			fst_check(d.recovery_calls == 1);
			fst_check(d.recovery_status == SWITCH_RDIAG_RECOVERY_NO_CALLBACK);

			switch_core_recovery_track_callback(rdiag_track_cb);

			switch_channel_clear_flag(fst_channel, CF_TRACKABLE);
			switch_refresh_diag_init(&d, 1000);
			switch_refresh_diag_push(&d);
			switch_core_recovery_track(fst_session);
			switch_refresh_diag_pop(&d);
			fst_check(d.recovery_status == SWITCH_RDIAG_RECOVERY_INELIGIBLE);
			fst_check(d.xml_builds == 0);
			fst_check(rdiag_tracked_calls == 0);

			switch_channel_mark_answered(fst_channel);
			switch_channel_set_flag(fst_channel, CF_TRACKABLE);
			switch_refresh_diag_init(&d, 1000);
			switch_refresh_diag_push(&d);
			switch_core_recovery_track(fst_session);
			switch_refresh_diag_pop(&d);

			switch_core_recovery_track_callback(NULL);
			switch_channel_clear_flag(fst_channel, CF_TRACKABLE);
			switch_channel_clear_flag(fst_channel, CF_TRACKED);

			fst_check(rdiag_tracked_calls == 1);
			fst_check(d.recovery_status == SWITCH_RDIAG_RECOVERY_TRACKED);
			fst_check(d.xml_builds == 1);
			fst_check(d.recovery_bytes > 0);
		}
		FST_SESSION_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
