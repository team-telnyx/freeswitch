/*
 * record_callback(ABC_TYPE_CLOSE) frees the recording's helper, and the core
 * then hands that same pointer back as ABC_TYPE_DESTROY_USER_DATA, because
 * switch_core_media_bug_destroy() clears bug->user_data only afterwards
 * (switch_core_media_bug.c:85-87). The guard at the top of record_callback()
 * locks rh->flag_mutex, so the freed pool gets dereferenced as a mutex:
 *
 *   ___pthread_mutex_lock()                     pthread_mutex_lock.c:80  <-- crash
 *   fspr_thread_mutex_lock()
 *   switch_mutex_lock()                         switch_apr.c:317
 *   record_callback()                           switch_ivr_async.c:1706
 *   switch_core_media_bug_destroy()             switch_core_media_bug.c:87
 *   switch_core_media_bug_remove_all_function() switch_core_media_bug.c:1379
 *   switch_core_session_hangup_state()          switch_core_state_machine.c:875
 *
 * (Return addresses from the crashing build, so each frame sits one line past
 * its call site and the numbers are that tree's, not this one's.)
 *
 * Covers the normal CLOSE tail only. The write-failure site in the same case and
 * the transfer hand-off in switch_core_media_bug_transfer_callback() are not
 * exercised here.
 *
 * Deterministic - no threads, no transfer, no race.
 * switch_core_media_bug_remove_all_function() closes every bug, then destroys
 * them in a second loop whose list it builds by prepending, so the first bug
 * closed is the last destroyed. A probe bug added with SMBF_LAST closes after
 * the recording and is destroyed before it; its CLOSE is the window where the
 * recording's helper is already freed but its bug is still alive.
 *
 * The probe compares pointers and never dereferences the helper, so the
 * assertion holds on a plain -O2 build. FS_UAF_POISON=1 additionally reclaims
 * the freed pool node and scribbles on it, turning the dangling read into the
 * SEGV above.
 *
 * Poison mode assumes glibc malloc and is not for sanitizer builds, and it
 * scribbles on any pool node freed in its window, so a crash under it is not by
 * itself proof of this particular use-after-free.
 *
 * Env: FS_REAP_MS (default 2500) is how long the probe holds teardown open so
 * the pool reaper (switch_core_memory.c, 1s settle then drain) really frees the
 * helper - raise it on a slow or instrumented build, where too short a window
 * makes poison mode fail with "reclaimed=no". FS_UAF_POISON=1 enables the crash
 * reproducer; it is off by default because a segfaulting test reports nothing.
 *
 * Own binary on purpose: the record-transfer harness this grew out of has a
 * pre-existing SEGV in its own writer thread, which would stop this test ever
 * running under make check.
 */

#include <switch.h>
#include <stdlib.h>
#include <test/switch_test.h>

/* PER_POOL_LOCK means a pool owns its allocator, so destroying it free()s the
 * node; same-size mallocs claim it back. */
#define POISON_CHUNKS 256
#define POISON_BYTES 8192   /* fspr's minimum node size */

struct destroy_probe {
	switch_media_bug_t *record_bug;      /* the recording's bug, set before teardown */
	void *helper_at_start;               /* helper address, captured before teardown */
	const char *record_file;
	int close_seen;
	int destroy_user_data_seen;
	int record_private_cleared;          /* CLOSE reached record_helper_destroy() */
	void *record_user_data_after_close;  /* pointer identity only, never dereferenced */
	int reap_ms;
	int poison;
	int poison_hit;                      /* a scratch chunk covered the helper */
	void *scratch[POISON_CHUNKS];
};

static int poison_enabled(void)
{
	const char *env = getenv("FS_UAF_POISON");
	return env && switch_true(env);
}

/* Must outlast the reaper's 1s settle plus the drain. */
static int reap_delay_ms(void)
{
	const char *env = getenv("FS_REAP_MS");
	int ms = env ? atoi(env) : 0;
	return ms > 0 ? ms : 2500;
}

static switch_bool_t destroy_probe_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	struct destroy_probe *p = (struct destroy_probe *) user_data;
	switch_core_session_t *session = switch_core_media_bug_get_session(bug);
	switch_channel_t *channel = switch_core_session_get_channel(session);

	switch (type) {
	case SWITCH_ABC_TYPE_CLOSE:
		p->close_seen = 1;

		/* Cleared at the top of record_callback's CLOSE case
		 * (switch_ivr_async.c:1979), so this only witnesses that CLOSE ran. */
		p->record_private_cleared = (switch_channel_get_private(channel, p->record_file) == NULL);

		/* The bug is still alive (destroyed in the second loop); the helper
		 * it points at is not. */
		p->record_user_data_after_close = switch_core_media_bug_get_user_data(p->record_bug);

		if (p->poison) {
			int i;

			/* Only poison mode needs the reaper to have really freed the pool;
			 * the assertion below is about the pointer, not the memory. */
			switch_yield((switch_interval_time_t) p->reap_ms * 1000);

			for (i = 0; i < POISON_CHUNKS; i++) {
				if (!(p->scratch[i] = malloc(POISON_BYTES))) {
					break;
				}

				/* Comparing our own allocation's range against a stale address
				 * is arithmetic, not a dereference. Uses the address captured
				 * before teardown: once fixed the user_data is NULL, and
				 * checking against that would make this mode vacuous. */
				if ((char *) p->helper_at_start >= (char *) p->scratch[i] &&
					(char *) p->helper_at_start < (char *) p->scratch[i] + POISON_BYTES) {
					p->poison_hit = 1;
				}

				memset(p->scratch[i], 0xAB, POISON_BYTES);
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] poisoned %d chunk(s), helper node reclaimed=%s\n",
							  i, p->poison_hit ? "YES" : "no");
		}
		break;
	case SWITCH_ABC_TYPE_DESTROY_USER_DATA:
		p->destroy_user_data_seen = 1;
		break;
	default:
		break;
	}

	return SWITCH_TRUE;
}

/* Originate one answered "null" session parked in CS_SOFT_EXECUTE, so it has
 * media and a read/write codec. Returned rwlocked; caller unlocks. */
static switch_core_session_t *originate_parked_null_session(void)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_call_cause_t cause = SWITCH_CAUSE_NONE;
	switch_event_t *vars = NULL;
	switch_status_t status;

	if (switch_event_create_plain(&vars, SWITCH_EVENT_CHANNEL_DATA) != SWITCH_STATUS_SUCCESS) {
		return NULL;
	}
	switch_event_add_header_string(vars, SWITCH_STACK_BOTTOM, "origination_caller_id_number", "+15551112222");
	switch_event_add_header(vars, SWITCH_STACK_BOTTOM, "rate", "%d", 8000);

	status = switch_ivr_originate(NULL, &session, &cause, "null/+15553334444",
								  2, NULL, NULL, NULL, NULL, vars, SOF_NONE, NULL, NULL);
	switch_event_destroy(&vars);

	if (status != SWITCH_STATUS_SUCCESS || !session) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "[TEST] originate failed: status=%d cause=%d\n", status, cause);
		return NULL;
	}

	channel = switch_core_session_get_channel(session);
	switch_channel_set_state(channel, CS_SOFT_EXECUTE);
	switch_channel_wait_for_state(channel, NULL, CS_SOFT_EXECUTE);
	return session;
}

static switch_media_bug_t *get_record_bug(switch_core_session_t *session)
{
	switch_media_bug_t *bug = NULL;

	if (switch_core_media_bug_pop(session, "session_record", &bug) != SWITCH_STATUS_SUCCESS || !bug) {
		return NULL;
	}
	switch_core_media_bug_clear_flag(bug, SMBF_LOCK); /* pop() sets it */
	return bug;
}

FST_CORE_BEGIN("./conf_async")
{
	FST_SUITE_BEGIN(record_helper_destroy_uaf)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_loopback");
			fst_requires_module("mod_sndfile");
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		FST_TEST_BEGIN(test_record_helper_dangling_at_bug_destroy)
		{
			switch_core_session_t *session = NULL;
			switch_channel_t *channel = NULL;
			switch_media_bug_t *probe_bug = NULL;
			struct destroy_probe probe = { 0 };
			const char *record_file = NULL;
			void *rh = NULL;
			switch_status_t status;
			int i;
			int setup_ok = 0;

			session = originate_parked_null_session();
			fst_requires(session);   /* nothing locked yet */
			channel = switch_core_session_get_channel(session);

			record_file = switch_core_session_sprintf(session, "%s%s%s-destroy.wav",
													  SWITCH_GLOBAL_dirs.temp_dir,
													  SWITCH_PATH_SEPARATOR,
													  switch_core_session_get_uuid(session));

			probe.record_file = record_file;
			probe.reap_ms = reap_delay_ms();
			probe.poison = poison_enabled();

			/* From here on every failure goes to done: bailing out of the test
			 * body would leave the session read-locked, and its state thread
			 * then blocks forever in switch_core_session_write_lock(). */
			status = switch_ivr_record_session_event(session, (char *) record_file, 0, NULL, NULL);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			if (status != SWITCH_STATUS_SUCCESS) goto done;

			/* SMBF_LAST pins the probe behind the recording whatever
			 * switch_core_add_media_bug_last() and the other bugs' flags say
			 * (switch_core_media_bug.c:1074-1087), so the recording still
			 * closes first and is destroyed last. */
			status = switch_core_media_bug_add(session, "uaf_destroy_probe", NULL,
											   destroy_probe_callback, &probe, 0,
											   SMBF_READ_STREAM | SMBF_LAST, &probe_bug);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			if (status != SWITCH_STATUS_SUCCESS) goto done;

			probe.record_bug = get_record_bug(session);
			fst_check(probe.record_bug != NULL);
			if (!probe.record_bug) goto done;

			rh = switch_core_media_bug_get_user_data(probe.record_bug);
			fst_check(rh != NULL);
			if (!rh) goto done;

			probe.helper_at_start = rh;
			setup_ok = 1;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] recording bug=%p helper=%p, reap window %dms\n",
							  (void *) probe.record_bug, rh, probe.reap_ms);

			/* The production teardown (switch_core_state_machine.c:873). */
			switch_core_media_bug_remove_all(session);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] after CLOSE freed helper=%p, its bug carries user_data=%p -> %s\n",
							  rh, probe.record_user_data_after_close,
							  probe.record_user_data_after_close == rh ? "DANGLING (same pointer)" : "cleared");

done:
			for (i = 0; i < POISON_CHUNKS; i++) {
				switch_safe_free(probe.scratch[i]);
			}
			switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session);

			if (!setup_ok) {
				break;
			}

			/* The probe must have closed after the recording, or this observed
			 * nothing. */
			fst_check(probe.close_seen);
			fst_check(probe.record_private_cleared);

			/* The core re-enters a callback after CLOSE whenever user_data is
			 * non-NULL (switch_core_media_bug.c:85-86) - seen here on the
			 * probe's own bug. Without this the assertion below could pass
			 * because the core stopped calling back at all. */
			fst_check(probe.destroy_user_data_seen);

			/* Poison mode proves nothing unless it reclaimed the helper's node. */
			if (probe.poison) {
				fst_check(probe.poison_hit);
			}

			/* THE DEFECT: the bug carried a freed helper into the destroy loop,
			 * so record_callback() ran again and locked rh->flag_mutex, a mutex
			 * in the pool CLOSE destroyed. The fix clears user_data when the
			 * helper dies, so this reads NULL. */
			fst_check(probe.record_user_data_after_close == NULL);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
