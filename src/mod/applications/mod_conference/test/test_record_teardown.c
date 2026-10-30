/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * test_record_teardown.c -- TELCORE-193 regression test
 *
 * Reproduces, deterministically and through the real conference call path, the
 * crash a conference record thread hits when it races conference teardown.
 *
 * Production stack (release build):
 *   __assert_fail()
 *   switch_event_merge()                       switch_event.c
 *   conference_event_add_data_with_member()    conference_event.c   (frame 6)
 *   conference_event_add_data()                conference_event.c   (frame 7)
 *   conference_record_thread_run()             conference_record.c  (frame 8)
 *
 * Root cause
 * ----------
 * conference_event_add_data_with_member() ends with
 *
 *     switch_event_merge(event, conference->variables);
 *
 * The conference teardown thread destroys conference->variables
 * (switch_event_destroy sets it to NULL) and frees the conference pool, while a
 * record thread that grabbed its read lock just after the teardown's write-lock
 * drain is still running. The record thread then passes that now-NULL pointer
 * as `tomerge`. On an unfixed tree switch_event_merge() asserts
 * `tomerge && event` and aborts.
 *
 * What this test does
 * -------------------
 * It drives conference_event_add_data() directly (it is non-static and linked in
 * from libmodconference) with a minimal conference whose ->variables is NULL -
 * exactly the state the teardown leaves behind. member is NULL and members is
 * empty, so the function only reads the handful of scalar/string fields set
 * below before reaching the merge.
 *
 * Expected:
 *   - BUGGY tree : switch_assert(tomerge && event) fires -> SIGABRT.
 *   - FIXED tree : switch_event_merge() tolerates the NULL source, the conference
 *                  headers are added, and the test passes.
 */
#include <switch.h>
#include <stdlib.h>
#include <mod_conference.h>

#include <test/switch_test.h>

FST_CORE_BEGIN("./conf")
{
	FST_SUITE_BEGIN(conference_record_teardown)
	{
		FST_SETUP_BEGIN()
		{
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		FST_TEST_BEGIN(event_add_data_after_variables_destroyed)
		{
			conference_obj_t *conference = NULL;
			switch_event_t *event = NULL;
			switch_status_t status;

			/* A conference whose teardown has already destroyed ->variables.
			 * switch_core_alloc zeroes the struct, so members/count/variables are
			 * NULL/0; the assignments below are explicit for clarity. */
			conference = switch_core_alloc(fst_pool, sizeof(*conference));
			fst_requires(conference);
			conference->name = "3001";
			conference->domain = "test";
			conference->profile_name = "default";
			conference->uuid_str = "00000000-0000-0000-0000-000000000000";
			conference->members = NULL;
			conference->count = 0;
			conference->count_ghosts = 0;
			conference->variables = NULL;   /* destroyed by the teardown thread */

			/* conference_event_add_data_with_member() takes flag_mutex around the
			 * conference->variables merge (upstream d22aec67c6, "[mod_conference]
			 * Avoid race conditions touching conference->variables without a
			 * mutex"). In production this mutex is always initialised by
			 * conference_new(); only ->variables is destroyed by the teardown
			 * thread, which is what this test models. The fixture must therefore
			 * provide it, or the lock dereferences NULL before the merge is
			 * ever reached. */
			switch_mutex_init(&conference->flag_mutex, SWITCH_MUTEX_NESTED, fst_pool);

			status = switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, "conference::maintenance");
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			/* Frames 7 -> 6 -> 5 of the production stack. On an unfixed tree the
			 * NULL ->variables aborts inside switch_event_merge(). */
			status = conference_event_add_data(conference, event);

			/* Only reached on a fixed tree: the merge bailed on the NULL source
			 * and the conference headers were still added. */
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check(!zstr(switch_event_get_header(event, "Conference-Name")));
			fst_check(!zstr(switch_event_get_header(event, "Conference-Unique-ID")));

			switch_event_destroy(&event);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
