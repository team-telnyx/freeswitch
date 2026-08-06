/*
 * Regression test for the record_callback() use-after-free on the shared
 * `struct record_helper` (rh) during recording_follow_transfer.
 *
 * Production stack (release build line numbers):
 *   record_callback()                    switch_ivr_async.c:1551     <-- crash
 *   switch_core_media_bug_close()        switch_core_media_bug.c:1396
 *   switch_core_media_bug_prune()        switch_core_media_bug.c:1521
 *   switch_core_session_write_frame()    switch_core_media.c:19615
 *   audio_bridge_thread()                switch_ivr_bridge.c:1143
 *   audio_bridge_on_exchange_media()     switch_ivr_bridge.c:1311
 *   switch_core_session_run() / _thread() / _thread_pool_worker()
 *
 * (The reported frames were resolved without the minus-one adjustment, so each
 * one points a line past its call site: :1399 -> the callback call at :1396,
 * :1522 -> the close call at :1521, :19619 -> the prune call at :19615.)
 *
 * Root cause
 * ----------
 * `struct record_helper` lives in its OWN memory pool (record_helper_create(),
 * switch_ivr_async.c:3305), not the session pool, and record_helper_destroy()
 * (:3343) destroys that pool. So rh dies independently of any session.
 *
 * switch_ivr_transfer_recordings() -> switch_core_media_bug_transfer_callback()
 * shares that ONE rh between the old bug and the new one. The "dup" function
 * switch_ivr_record_user_data_dup() (:2098) copies nothing - it mutates
 * rh->recording_session / rh->transfer_from_session and returns the SAME
 * pointer, which switch_core_media_bug_transfer_callback():1169 hands to the
 * new bug. One rh, N potential owners, no refcount and no lock. Whichever bug
 * reaches ABC_TYPE_CLOSE first calls record_helper_destroy() (:1791, :1844 or
 * :3981) and frees the pool out from under everyone else.
 *
 * The guard added by 2b8fa12baa ("[Core] Fix hangup race in
 * recording_follow_transfer") is meant to make the non-owning bug a no-op:
 *
 *      1550  // Check if the recording was transferred
 *      1551  if (rh->recording_session != session) {
 *      1552      return SWITCH_FALSE;
 *      1553  }
 *
 * but to decide whether it still owns rh it has to READ rh - it is a
 * use-after-free check performed through the freed pointer. That load is the
 * faulting instruction in production.
 *
 * It is also self-amplifying: returning SWITCH_FALSE from the WRITE callback
 * makes switch_core_session_write_frame() set SMBF_PRUNE
 * (switch_core_media.c:19608-19611) and call switch_core_media_bug_prune(),
 * which invokes the callback a SECOND time with ABC_TYPE_CLOSE - a second
 * dereference of the same shared rh, later in time. That second dereference is
 * frames 0-2 above. Note prune clears SMBF_LOCK and thread_id
 * (switch_core_media_bug.c:1519-1520) before closing, so the "BUG is thread
 * locked skipping" bailout at :1390 offers no protection on this path.
 *
 * The free is also asynchronous: switch_core_perform_destroy_memory_pool()
 * pushes the pool onto memory_manager.pool_queue (switch_core_memory.c:497) for
 * the reaper thread rather than destroying it inline, which is why the window
 * is wide and the crash is timing-dependent.
 *
 * What this test does
 * -------------------
 * 1. test_transfer_recordings_ownership - DETERMINISTIC, no threads. This is
 *    the one that FAILS on the current tree. After
 *    switch_ivr_transfer_recordings(A, B) it:
 *
 *      - logs, as hard evidence of the root cause, that A's bug and B's brand
 *        new bug are two DIFFERENT switch_media_bug_t carrying the IDENTICAL
 *        record_helper pointer. Observed:
 *            A's bug=0x...3d8b8 rh=0x...316980
 *            B's bug=0x...58a88 rh=0x...316980   -> SHARED
 *        This is not asserted: a refcounting fix would legitimately keep the
 *        pointer shared. What IS asserted is the fix-strategy-agnostic
 *        invariant that nothing may retain a handle to what it no longer owns:
 *
 *      a) A's channel must not retain a handle to the transferred bug.
 *         switch_ivr_record_session_event() published it with
 *         switch_channel_set_private(channel, file, bug) (:3962), the transfer
 *         never clears it, and switch_core_media_bug_transfer_callback():1171
 *         destroys that very bug. A is left with a dangling handle that
 *         switch_ivr_stop_record_session() (:2091),
 *         switch_ivr_record_session_pause() (:2073) and
 *         switch_ivr_record_session_event() (:3427) all dereference.
 *         FAILS today: A's private is unchanged across the transfer.
 *
 *      b) B must be able to address the recording it now owns - the transfer
 *         never publishes the new bug on B's channel.
 *         FAILS today: B's private is NULL.
 *
 *      c) The recording must be stoppable on its new owner.
 *         FAILS today: switch_ivr_stop_record_session(B, file) returns FALSE.
 *
 *    Two further consequences fall out of (a), both visible in the logs:
 *      - switch_ivr_stop_record_session() on A returns SUCCESS while removing
 *        nothing, because :2093 ignores switch_core_media_bug_remove()'s status.
 *      - that filename can never be recorded on A again: :3427 sees the stale
 *        handle and returns "Already recording".
 *
 * 2. test_record_helper_shared_across_transfer_uaf - CONCURRENT crash
 *    reproducer for the stack above. Four threads on two sessions:
 *      - transfer_thread : ping-pongs the recording A<->B, as mod_sofia and
 *                          switch_channel.c:936 do.
 *      - teardown_thread : stops the recording (-> record_helper_destroy() ->
 *                          the helper pool is destroyed) and starts a fresh one.
 *      - two writers     : switch_core_session_write_frame() on A and on B -
 *                          the exact frame 3 of the crash - so record_callback()
 *                          runs on both sides of a transfer, each racing to
 *                          decide whether it still owns rh. The two sessions
 *                          have separate bug_rwlocks, so nothing serialises a
 *                          callback on one against the helper being freed by
 *                          the other.
 *    A typical run reaches ~400 transfers, ~200 real helper destroys and 200k
 *    write frames.
 *
 *    It also carries a build-independent oracle (xa.dangling): after each stop
 *    it checks, by pointer identity only and without ever dereferencing, that
 *    no live bug still references the helper that was just destroyed.
 *
 * 3. test_record_helper_uaf_on_hangup_during_transfer - hangs session B up
 *    while the recording is being transferred onto it, sweeping the hangup
 *    across the transfer window over 120 rounds. This targets the trigger the
 *    guard's own commit is named after ("Fix hangup race in
 *    recording_follow_transfer") and that the production bridge stack implies.
 *
 * STATUS - what these tests do and do not establish
 * ------------------------------------------------
 * Established, reproducibly, on both a plain -O2 and an ASan build:
 *   - the shared helper (logged pointer identity across two distinct bugs)
 *   - all three ownership assertions in test 1 fail
 *   - the two knock-on effects noted above
 *
 * NOT reproduced: the use-after-free at :1551 itself. Tests 2 and 3 ran on an
 * ASan build (--enable-address-sanitizer) reaching ~290 transfers, ~370 real
 * record_helper_destroy() calls, ~50M write frames and 120 hangup-during-
 * transfer rounds, with no report.
 *
 * That negative is meaningful rather than a blind spot, because the detector
 * was validated in the same binary: PER_POOL_LOCK is defined
 * (switch_core_memory.c:48), so every pool owns its allocator (:418-435) and
 * fspr_pool_destroy() -> fspr_allocator_destroy() really does free() every node
 * (fspr_pools.c:804-806, :108-117). A probe that read a destroyed pool produced
 * "AddressSanitizer: heap-use-after-free" as expected. (So no special pool
 * build is needed here; --enable-pool-sanitizer targets the non-PER_POOL_LOCK
 * recycling path. Note it silently did nothing until configure.ac was fixed -
 * AC_ARG_ENABLE() was declared with an empty feature name, so the option was
 * parsed by autoconf's generic handler and then reset to "no" by its own
 * action-if-not-given branch. Re-run bootstrap.sh to pick the fix up.)
 *
 * Why the race did not fire: switch_core_media_bug_transfer_callback() holds
 * orig_session->bug_rwlock across unlink -> user_data_dup (which mutates rh) ->
 * add -> destroy (:1150-1197), while switch_core_session_write_frame() holds
 * the rdlock for the whole callback loop. On that path there is never a linked,
 * ready bug carrying a foreign rh. After a SUCCESSFUL transfer exactly one bug
 * exists and it owns rh, so hanging the new owner up frees the helper with
 * nothing else referencing it.
 *
 * So the remaining candidates for the production crash are the paths a unit
 * test cannot easily drive, all of which need a real bridge with an active
 * recording thread:
 *   - the transfer failure/revert path (media_bug.c:1173-1187), where
 *     user_data_dup() has already mutated rh before it is known whether
 *     switch_core_media_bug_add() succeeded, and the revert re-runs the same
 *     mutating function to undo it;
 *   - the INIT re-init handshake (switch_ivr_async.c:1566-1591), which hands
 *     rh->bug to the new bug, signals the recording thread and gives up after a
 *     ~2s sanity loop, leaving rh->bug and the real owner disagreeing;
 *   - the recording thread itself (:1435) reading rh->recording_session with
 *     nothing preventing the CLOSE path from having freed the pool.
 *
 * Treat tests 2 and 3 as regression harnesses for those, not as proof either
 * way. Test 1 is the one that fails today and pins the defect.
 */

#include <switch.h>
#include <stdlib.h>
#include <test/switch_test.h>

/* The race is driven for a wall-clock duration rather than a fixed iteration
 * count. Iteration counts do not survive a change of build flavour: under ASan
 * everything slows down unevenly, and a fixed-count transfer loop can burn
 * through all its iterations during a window when the teardown thread happens
 * to have no recording running - producing a vacuous "pass" with zero
 * transfers. Running to a deadline keeps all four threads interleaving for the
 * whole window whatever the build. */
#define RACE_DURATION_MS  10000   /* override with FS_RACE_MS (see race_duration_ms) */
#define RACE_TICK_US      200
#define HANGUP_ROUNDS     120     /* override with FS_HANGUP_ROUNDS */

/* The race window, in ms. Overridable via FS_RACE_MS because the default is
 * tuned for a native run: under valgrind everything is ~30x slower, so a 10s
 * window yields almost no transfers and the test degenerates into a vacuous
 * pass. Use FS_RACE_MS=120000 (or more) there. */
static int race_duration_ms(void)
{
	const char *env = getenv("FS_RACE_MS");
	int ms = env ? atoi(env) : 0;
	return ms > 0 ? ms : RACE_DURATION_MS;
}

/* Originate one answered "null" session, parked in CS_SOFT_EXECUTE so it has
 * media and a read/write codec (same setup FST_SESSION_BEGIN uses). Returned
 * rwlocked; caller unlocks. */
static switch_core_session_t *originate_parked_null_session(const char *label)
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
						  "[TEST] originate %s failed: status=%d cause=%d\n", label, status, cause);
		return NULL;
	}

	channel = switch_core_session_get_channel(session);
	switch_channel_set_state(channel, CS_SOFT_EXECUTE);
	switch_channel_wait_for_state(channel, NULL, CS_SOFT_EXECUTE);
	switch_channel_set_variable(channel, "send_silence_when_idle", "-1");
	/* recording_follow_transfer is what enables the production code path. */
	switch_channel_set_variable(channel, "recording_follow_transfer", "true");

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
					  "[TEST] session %s = %s (media_ready=%d)\n", label,
					  switch_core_session_get_uuid(session),
					  switch_channel_media_ready(channel));
	return session;
}

/* ---------------------------------------------------------------- test 2 --- */

static volatile int g_stop = 0;

struct writer_args {
	switch_core_session_t *session;
	const char *label;
	int frames_written;
};

/* Drives switch_core_session_write_frame() - production frame 3. This is what
 * walks session->bugs, invokes record_callback(), observes SWITCH_FALSE from
 * the :1551 guard, sets SMBF_PRUNE and calls switch_core_media_bug_prune(),
 * which re-enters record_callback() with ABC_TYPE_CLOSE. */
static void *SWITCH_THREAD_FUNC writer_thread(switch_thread_t *thread, void *obj)
{
	struct writer_args *a = (struct writer_args *) obj;
	switch_codec_implementation_t write_impl = { 0 };
	switch_frame_t write_frame = { 0 };
	switch_codec_t codec = { 0 };
	int i;

	(void) thread;

	if (switch_core_session_read_lock(a->session) != SWITCH_STATUS_SUCCESS) {
		return NULL;
	}

	if (switch_core_session_get_write_impl(a->session, &write_impl) != SWITCH_STATUS_SUCCESS) {
		switch_core_session_rwunlock(a->session);
		return NULL;
	}

	if (switch_core_codec_init(&codec, "L16", NULL, NULL,
							   write_impl.actual_samples_per_second,
							   write_impl.microseconds_per_packet / 1000,
							   write_impl.number_of_channels,
							   SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
							   NULL, switch_core_session_get_pool(a->session)) != SWITCH_STATUS_SUCCESS) {
		switch_core_session_rwunlock(a->session);
		return NULL;
	}

	switch_zmalloc(write_frame.data, SWITCH_RECOMMENDED_BUFFER_SIZE);
	write_frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;
	write_frame.codec = &codec;
	write_frame.datalen = codec.implementation->decoded_bytes_per_packet;
	write_frame.samples = write_frame.datalen / 2;
	write_frame.payload = (switch_payload_t) codec.implementation->ianacode;
	write_frame.rate = codec.implementation->actual_samples_per_second;

	for (i = 0; !g_stop; i++) {
		switch_core_session_write_frame(a->session, &write_frame, SWITCH_IO_FLAG_NONE, 0);
		a->frames_written++;

		/* Breathe periodically. Without this the writers monopolise the
		 * scheduler and the transfer/teardown threads barely run - harmless
		 * natively, fatal under valgrind, whose serialised scheduler let the
		 * writers reach 50M frames while only ONE transfer completed in 180s.
		 * A vacuous pass is worse than no test, so keep the other actors
		 * alive; the write path stays saturated either way. */
		if ((i & 0x3F) == 0x3F) {
			switch_yield(RACE_TICK_US);
		}
	}

	switch_safe_free(write_frame.data);
	switch_core_codec_destroy(&codec);
	switch_core_session_rwunlock(a->session);
	return NULL;
}

struct xfer_args {
	switch_core_session_t *a;
	switch_core_session_t *b;
	const char *file;
	int transfers;
	int stops;
	int restarts;
	int dangling;   /* live bug still pointing at a freed record_helper */
};

/* Return the record_helper a session's live "session_record" bug points at, or
 * NULL if it has none. Pointer identity only - the helper is never
 * dereferenced, so this oracle is safe on a plain (non-ASan) build. */
static void *peek_record_helper(switch_core_session_t *session)
{
	switch_media_bug_t *bug = NULL;
	void *rh;

	if (switch_core_media_bug_pop(session, "session_record", &bug) != SWITCH_STATUS_SUCCESS || !bug) {
		return NULL;
	}
	rh = switch_core_media_bug_get_user_data(bug);
	switch_core_media_bug_clear_flag(bug, SMBF_LOCK); /* pop() sets it */
	return rh;
}

/* Signalling thread: ping-pongs the recording between the two sessions, exactly
 * as mod_sofia / switch_channel.c:936 do when recording_follow_transfer is set. */
static void *SWITCH_THREAD_FUNC transfer_thread(switch_thread_t *thread, void *obj)
{
	struct xfer_args *x = (struct xfer_args *) obj;
	int i;

	(void) thread;

	for (i = 0; !g_stop; i++) {
		switch_core_session_t *from = (i & 1) ? x->b : x->a;
		switch_core_session_t *to   = (i & 1) ? x->a : x->b;

		if (switch_ivr_transfer_recordings(from, to) == SWITCH_STATUS_SUCCESS) {
			x->transfers++;
		}

		/* Yield, so this thread cannot spin through the whole run while the
		 * teardown thread is momentarily between a stop and a restart. */
		switch_yield(RACE_TICK_US);
	}

	return NULL;
}

/* Teardown thread: stops and restarts the recording, racing the transfers. The
 * stop is what reaches record_helper_destroy() (switch_ivr_async.c:1844) and
 * destroys rh->helper_pool, while the other session's write thread may be
 * inside record_callback() holding that very same rh. The two sessions have
 * separate bug_rwlocks, so nothing serialises those accesses - that is the race.
 *
 * Stop with "all", not by filename: by filename goes through
 * switch_channel_get_private() (switch_ivr_async.c:2091), and after a transfer
 * that handle is stale on one session and absent on the other, so the recording
 * is never actually closed (and :2093 reports SUCCESS anyway). "all" routes to
 * switch_core_media_bug_remove_callback(), which walks the real bug list. */
static void *SWITCH_THREAD_FUNC teardown_thread(switch_thread_t *thread, void *obj)
{
	struct xfer_args *x = (struct xfer_args *) obj;

	(void) thread;

	while (!g_stop) {
		void *rh_before = peek_record_helper(x->a);
		void *left_a, *left_b;
		const char *file;

		if (!rh_before) {
			rh_before = peek_record_helper(x->b);
		}

		if (switch_ivr_stop_record_session(x->a, "all") == SWITCH_STATUS_SUCCESS) x->stops++;
		if (switch_ivr_stop_record_session(x->b, "all") == SWITCH_STATUS_SUCCESS) x->stops++;

		/* Build-independent oracle. The helper behind rh_before has just been
		 * freed by record_helper_destroy(). No new helper can exist yet - only
		 * the restart below creates one, and nothing else in this test does - so
		 * a live bug still carrying that exact pointer is a dangling reference,
		 * not pool-address reuse. This is the state that makes record_callback()
		 * dereference freed memory at switch_ivr_async.c:1551. */
		left_a = peek_record_helper(x->a);
		left_b = peek_record_helper(x->b);
		if (rh_before && (left_a == rh_before || left_b == rh_before)) {
			x->dangling++;
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
							  "[TEST] DANGLING: helper %p was destroyed but a live bug still "
							  "references it (A=%p B=%p)\n", rh_before, left_a, left_b);
		}

		/* Restart under a FRESH filename each round. Reusing x->file would hit
		 * switch_ivr_record_session_event():3427, which sees the stale
		 * switch_channel_get_private() handle the transfer never cleared, logs
		 * "Already recording" and returns SUCCESS without starting anything -
		 * so no new record_helper would ever be created and the free path would
		 * never be exercised. (That short-circuit is itself a consequence of the
		 * same dangling handle: once a recording is transferred off a channel,
		 * that filename can never be recorded on it again.) */
		file = switch_core_session_sprintf(x->a, "%s%sxfer-race-%d.wav",
										   SWITCH_GLOBAL_dirs.temp_dir,
										   SWITCH_PATH_SEPARATOR, x->restarts);
		if (switch_ivr_record_session_event(x->a, (char *) file, 0, NULL, NULL) == SWITCH_STATUS_SUCCESS) {
			x->restarts++;
		}

		switch_yield(RACE_TICK_US);
	}

	return NULL;
}


/* ------------------------------------------------------- test 3: hangup --- */

/* The commit that introduced the :1551 guard is titled "Fix hangup race in
 * recording_follow_transfer", and the production stack is a bridge tearing
 * down. This drives exactly that: session B is hung up while the recording is
 * being transferred ONTO it, so B's teardown closes the freshly attached bug -
 * running record_callback(ABC_TYPE_CLOSE) -> record_helper_destroy() -> the
 * helper pool is destroyed - while switch_ivr_transfer_recordings() is still
 * working with that same rh, and while A's write path may still reach it.
 *
 * The hangup is fired from its own thread with a per-round delay that sweeps
 * across the transfer, so successive rounds land on different points of the
 * window instead of all hitting the same one. */
struct hangup_args {
	switch_core_session_t *victim;
	int delay_us;
};

static void *SWITCH_THREAD_FUNC hangup_thread(switch_thread_t *thread, void *obj)
{
	struct hangup_args *h = (struct hangup_args *) obj;

	(void) thread;
	switch_yield(h->delay_us);
	switch_channel_hangup(switch_core_session_get_channel(h->victim), SWITCH_CAUSE_NORMAL_CLEARING);
	return NULL;
}


/* ------------------------------------------- test 4: the revert path ------ */

/* Return the live "session_record" bug linked on a session, or NULL. Pointer
 * only; pop() does not unlink. */
static switch_media_bug_t *get_record_bug(switch_core_session_t *session)
{
	switch_media_bug_t *bug = NULL;

	if (switch_core_media_bug_pop(session, "session_record", &bug) != SWITCH_STATUS_SUCCESS || !bug) {
		return NULL;
	}
	switch_core_media_bug_clear_flag(bug, SMBF_LOCK); /* pop() sets it */
	return bug;
}

/* Holds the victim session's rwlock as a WRITE lock. While held,
 * switch_core_session_read_lock(victim) fails instantly - it is a tryrdlock
 * (switch_core_rwlock.c:88) - so the recording thread's lock attempt at
 * switch_ivr_async.c:1435 fails on every retry and the transfer INIT handshake
 * is forced into its 2s sanity timeout (:1584) -> INIT returns SWITCH_FALSE ->
 * switch_core_media_bug_add() fails -> transfer_callback runs the revert
 * (:1173-1187). This is the ONLY deterministic entry into the one code path
 * that ever re-links a media bug. */
struct wrlock_holder_args {
	switch_core_session_t *victim;
	volatile int locked;          /* holder sets 1 once the write lock is held */
	volatile int release;         /* main sets 1 to release (when auto_release_ms == 0) */
	int auto_release_ms;          /* if > 0, release after this long (sweep mode) */
};

static void *SWITCH_THREAD_FUNC wrlock_holder_thread(switch_thread_t *thread, void *obj)
{
	struct wrlock_holder_args *h = (struct wrlock_holder_args *) obj;

	(void) thread;
	switch_core_session_write_lock(h->victim);
	h->locked = 1;
	if (h->auto_release_ms > 0) {
		switch_yield((switch_interval_time_t) h->auto_release_ms * 1000);
	} else {
		while (!h->release) {
			switch_yield(10000);
		}
	}
	switch_core_session_rwunlock(h->victim);
	h->locked = 0;
	return NULL;
}

FST_CORE_BEGIN("./conf_async")
{
	FST_SUITE_BEGIN(record_transfer_helper_uaf)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_loopback");
			fst_requires_module("mod_sndfile");
			fst_requires_module("mod_dptools");
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		/*
		 * Deterministic ownership contract. No threads, no race: this simply
		 * shows that after a transfer the recording is reachable from the wrong
		 * places and unreachable from the right one.
		 */
		FST_TEST_BEGIN(test_transfer_recordings_ownership)
		{
			switch_core_session_t *sess_a = NULL, *sess_b = NULL;
			switch_channel_t *chan_a = NULL, *chan_b = NULL;
			switch_media_bug_t *bug_a = NULL, *bug_b = NULL;
			void *rh_a = NULL, *rh_b = NULL;
			const char *record_file = NULL;
			void *priv_a_before = NULL, *priv_a_after = NULL, *priv_b_after = NULL;
			switch_status_t status;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "===== RECORD TRANSFER OWNERSHIP CONTRACT =====\n");

			sess_a = originate_parked_null_session("A");
			fst_requires(sess_a);
			sess_b = originate_parked_null_session("B");
			fst_requires(sess_b);

			chan_a = switch_core_session_get_channel(sess_a);
			chan_b = switch_core_session_get_channel(sess_b);
			fst_requires(switch_channel_media_ready(chan_b));

			record_file = switch_core_session_sprintf(sess_a, "%s%s%s-xfer.wav",
													  SWITCH_GLOBAL_dirs.temp_dir,
													  SWITCH_PATH_SEPARATOR,
													  switch_core_session_get_uuid(sess_a));

			status = switch_ivr_record_session_event(sess_a, (char *) record_file, 0, NULL, NULL);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			/* switch_ivr_record_session_event():3962 publishes the bug here. */
			priv_a_before = switch_channel_get_private(chan_a, record_file);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] before transfer: A private=%p B private=%p\n",
							  priv_a_before, switch_channel_get_private(chan_b, record_file));
			fst_requires(priv_a_before != NULL);

			/* Capture the helper A's bug points at, so we can compare it with
			 * B's after the transfer. */
			fst_requires(switch_core_media_bug_pop(sess_a, "session_record", &bug_a) == SWITCH_STATUS_SUCCESS);
			switch_core_media_bug_clear_flag(bug_a, SMBF_LOCK); /* pop() sets it */
			rh_a = switch_core_media_bug_get_user_data(bug_a);
			fst_requires(rh_a != NULL);

			/* The production call: mod_sofia / mod_dptools / switch_channel.c:936
			 * all reach this when recording_follow_transfer is set. */
			status = switch_ivr_transfer_recordings(sess_a, sess_b);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] switch_ivr_transfer_recordings(A, B) = %d\n", status);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			priv_a_after = switch_channel_get_private(chan_a, record_file);
			priv_b_after = switch_channel_get_private(chan_b, record_file);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] after transfer:  A private=%p B private=%p\n",
							  priv_a_after, priv_b_after);

			/* THE ROOT CAUSE, stated directly: B's brand-new bug carries the
			 * *same* record_helper pointer A's bug had.
			 * switch_ivr_record_user_data_dup() (switch_ivr_async.c:2098) copies
			 * nothing; it mutates rh->recording_session and returns the same rh,
			 * which switch_core_media_bug_transfer_callback():1169 installs on
			 * the new bug. One helper in one private pool, two bugs, no refcount,
			 * no lock - and record_helper_destroy() (:3343) destroys that pool
			 * whole. This is not asserted (a refcounting fix would legitimately
			 * keep the pointer shared); it is logged as the fact the crash rests
			 * on. What IS asserted below is the invariant any fix must hold:
			 * nothing may retain a handle to a helper/bug it no longer owns. */
			fst_requires(switch_core_media_bug_pop(sess_b, "session_record", &bug_b) == SWITCH_STATUS_SUCCESS);
			switch_core_media_bug_clear_flag(bug_b, SMBF_LOCK);
			rh_b = switch_core_media_bug_get_user_data(bug_b);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] record_helper: A's bug=%p rh=%p | B's bug=%p rh=%p -> %s\n",
							  (void *) bug_a, rh_a, (void *) bug_b, rh_b,
							  rh_a == rh_b ? "SHARED (one helper, two owners)" : "separate");

			/* (a) The transferred bug was destroyed by
			 *     switch_core_media_bug_transfer_callback():1171, but nothing
			 *     cleared A's handle to it. A dangling pointer here is what lets
			 *     switch_ivr_stop_record_session()/_pause() on A dereference a
			 *     destroyed bug and, through it, the shared rh. */
			fst_check(priv_a_after == NULL);

			/* (b) The new bug on B was never published, so B cannot address the
			 *     recording it now owns. */
			fst_check(priv_b_after != NULL);

			/* (c) The recording must be stoppable on whichever session owns it.
			 *     Today neither by-name stop works correctly: B has no handle,
			 *     and A's handle points at freed memory. */
			status = switch_ivr_stop_record_session(sess_b, record_file);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] stop_record_session(B, file) = %d (expect SUCCESS=%d)\n",
							  status, SWITCH_STATUS_SUCCESS);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			/* Tear the sessions down; whatever is left of the recording is
			 * closed by the normal hangup path. */
			switch_channel_hangup(chan_a, SWITCH_CAUSE_NORMAL_CLEARING);
			switch_channel_hangup(chan_b, SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(sess_a);
			switch_core_session_rwunlock(sess_b);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "===== OWNERSHIP CONTRACT DONE =====\n\n");
		}
		FST_TEST_END()

		/*
		 * The crash itself. Races transfers against the write path so
		 * record_callback() runs on both sides of a transfer while the losing
		 * side's rh is being freed.
		 */
		FST_TEST_BEGIN(test_record_helper_shared_across_transfer_uaf)
		{
			switch_core_session_t *sess_a = NULL, *sess_b = NULL;
			switch_channel_t *chan_a = NULL, *chan_b = NULL;
			switch_thread_t *t_write_a = NULL, *t_write_b = NULL, *t_xfer = NULL, *t_down = NULL;
			switch_threadattr_t *thd_attr = NULL;
			struct writer_args wa = { 0 }, wb = { 0 };
			struct xfer_args xa = { 0 };
			const char *record_file = NULL;
			switch_status_t status, st;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "===== RECORD HELPER SHARED-ACROSS-TRANSFER UAF =====\n");

			g_stop = 0;

			sess_a = originate_parked_null_session("A");
			fst_requires(sess_a);
			sess_b = originate_parked_null_session("B");
			fst_requires(sess_b);

			chan_a = switch_core_session_get_channel(sess_a);
			chan_b = switch_core_session_get_channel(sess_b);

			record_file = switch_core_session_sprintf(sess_a, "%s%s%s-race.wav",
													  SWITCH_GLOBAL_dirs.temp_dir,
													  SWITCH_PATH_SEPARATOR,
													  switch_core_session_get_uuid(sess_a));

			status = switch_ivr_record_session_event(sess_a, (char *) record_file, 0, NULL, NULL);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			switch_threadattr_create(&thd_attr, fst_pool);
			switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);

			wa.session = sess_a; wa.label = "A";
			wb.session = sess_b; wb.label = "B";
			xa.a = sess_a; xa.b = sess_b; xa.file = record_file;

			switch_thread_create(&t_write_a, thd_attr, writer_thread, &wa, fst_pool);
			switch_thread_create(&t_write_b, thd_attr, writer_thread, &wb, fst_pool);
			switch_thread_create(&t_xfer, thd_attr, transfer_thread, &xa, fst_pool);
			switch_thread_create(&t_down, thd_attr, teardown_thread, &xa, fst_pool);

			/* Let the four threads race for the full window. On a buggy tree the
			 * fault lands in one of the writer threads, inside record_callback()
			 * at switch_ivr_async.c:1551, reached from
			 * switch_core_session_write_frame() -> _bug_prune() -> _bug_close().
			 * Nothing below this line executes in that case. */
			switch_yield((switch_interval_time_t) race_duration_ms() * 1000);
			g_stop = 1;

			switch_thread_join(&st, t_xfer);
			switch_thread_join(&st, t_down);
			switch_thread_join(&st, t_write_a);
			switch_thread_join(&st, t_write_b);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] survived: transfers=%d stops=%d restarts=%d dangling=%d framesA=%d framesB=%d\n",
							  xa.transfers, xa.stops, xa.restarts, xa.dangling,
							  wa.frames_written, wb.frames_written);

			/* Prove the race window was actually entered - otherwise a "pass"
			 * here is meaningless. */
			fst_check(xa.transfers > 0);
			fst_check(xa.stops > 0);   /* real closes -> record_helper_destroy() ran */
			fst_check(wa.frames_written > 0);
			fst_check(wb.frames_written > 0);

			/* No live media bug may reference a destroyed record_helper. */
			fst_check(xa.dangling == 0);

			switch_ivr_stop_record_session(sess_a, "all");
			switch_ivr_stop_record_session(sess_b, "all");

			switch_channel_hangup(chan_a, SWITCH_CAUSE_NORMAL_CLEARING);
			switch_channel_hangup(chan_b, SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(sess_a);
			switch_core_session_rwunlock(sess_b);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "===== UAF RACE TEST COMPLETED =====\n\n");
		}
		FST_TEST_END()

		/*
		 * The hangup race: tear the transfer target down underneath the
		 * transfer, sweeping the timing across the window.
		 */
		FST_TEST_BEGIN(test_record_helper_uaf_on_hangup_during_transfer)
		{
			switch_threadattr_t *thd_attr = NULL;
			int round, completed = 0, transfers = 0;
			const char *rounds_env = getenv("FS_HANGUP_ROUNDS");
			int hangup_rounds = rounds_env && atoi(rounds_env) > 0 ? atoi(rounds_env) : HANGUP_ROUNDS;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "===== HANGUP-DURING-TRANSFER RACE =====\n");

			switch_threadattr_create(&thd_attr, fst_pool);
			switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);

			for (round = 0; round < hangup_rounds; round++) {
				switch_core_session_t *sess_a = NULL, *sess_b = NULL;
				switch_thread_t *t_hup = NULL, *t_write = NULL;
				struct hangup_args ha = { 0 };
				struct writer_args wa = { 0 };
				const char *file;
				switch_status_t st;

				sess_a = originate_parked_null_session("A");
				if (!sess_a) break;
				sess_b = originate_parked_null_session("B");
				if (!sess_b) { switch_core_session_rwunlock(sess_a); break; }

				file = switch_core_session_sprintf(sess_a, "%s%shup-race-%d.wav",
												   SWITCH_GLOBAL_dirs.temp_dir,
												   SWITCH_PATH_SEPARATOR, round);

				if (switch_ivr_record_session_event(sess_a, (char *) file, 0, NULL, NULL) != SWITCH_STATUS_SUCCESS) {
					switch_core_session_rwunlock(sess_a);
					switch_core_session_rwunlock(sess_b);
					continue;
				}

				/* Keep A's write path running, so a callback can be in flight
				 * holding rh while B's teardown frees it. */
				g_stop = 0;
				wa.session = sess_a; wa.label = "A";
				switch_thread_create(&t_write, thd_attr, writer_thread, &wa, fst_pool);

				/* Sweep the hangup across the transfer window: 0..~2ms. */
				ha.victim = sess_b;
				ha.delay_us = (round % 40) * 50;
				switch_thread_create(&t_hup, thd_attr, hangup_thread, &ha, fst_pool);

				if (switch_ivr_transfer_recordings(sess_a, sess_b) == SWITCH_STATUS_SUCCESS) {
					transfers++;
				}

				/* Give both sides time to finish tearing down on top of each
				 * other, and the pool reaper time to actually free the helper -
				 * switch_core_perform_destroy_memory_pool() only queues it. */
				switch_yield(60000);

				switch_thread_join(&st, t_hup);
				g_stop = 1;
				switch_thread_join(&st, t_write);

				switch_ivr_stop_record_session(sess_a, "all");
				switch_channel_hangup(switch_core_session_get_channel(sess_a), SWITCH_CAUSE_NORMAL_CLEARING);
				switch_core_session_rwunlock(sess_a);
				switch_core_session_rwunlock(sess_b);

				completed++;
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] hangup race: rounds=%d transfers=%d\n", completed, transfers);
			fst_check(completed > 0);
			fst_check(transfers > 0);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "===== HANGUP RACE COMPLETED =====\n\n");
		}
		FST_TEST_END()

		/*
		 * 4a. The revert path, driven deterministically. A helper thread
		 * write-locks B; the recording thread's read_lock(B) then fails on
		 * every retry, the INIT handshake times out after ~2s, add fails, and
		 * switch_core_media_bug_transfer_callback() reverts (:1173-1187).
		 * This pins the revert path's post-conditions so a fix cannot silently
		 * break it - and it is the setup the boundary sweep (4c) races against.
		 */
		FST_TEST_BEGIN(test_transfer_revert_deterministic)
		{
			switch_core_session_t *sess_a = NULL, *sess_b = NULL;
			switch_media_bug_t *bug_before = NULL, *bug_after = NULL;
			void *rh_before = NULL;
			switch_thread_t *t_hold = NULL;
			switch_threadattr_t *thd_attr = NULL;
			struct wrlock_holder_args hold = { 0 };
			const char *record_file = NULL;
			switch_time_t t0;
			int dt_ms, sanity;
			switch_status_t status, st;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "===== REVERT PATH (DETERMINISTIC) =====\n");

			sess_a = originate_parked_null_session("A");
			fst_requires(sess_a);
			sess_b = originate_parked_null_session("B");
			fst_requires(sess_b);

			record_file = switch_core_session_sprintf(sess_a, "%s%s%s-revert.wav",
													  SWITCH_GLOBAL_dirs.temp_dir,
													  SWITCH_PATH_SEPARATOR,
													  switch_core_session_get_uuid(sess_a));
			fst_requires(switch_ivr_record_session_event(sess_a, (char *) record_file, 0, NULL, NULL) == SWITCH_STATUS_SUCCESS);
			switch_yield(100000); /* let the recording thread reach its cond wait */

			bug_before = get_record_bug(sess_a);
			fst_requires(bug_before);
			rh_before = switch_core_media_bug_get_user_data(bug_before);
			fst_requires(rh_before);

			/* Main holds B's read lock from originate; release it so the
			 * holder can take the write lock. B stays parked, nothing kills it. */
			switch_core_session_rwunlock(sess_b);

			hold.victim = sess_b;
			switch_threadattr_create(&thd_attr, fst_pool);
			switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
			switch_thread_create(&t_hold, thd_attr, wrlock_holder_thread, &hold, fst_pool);

			for (sanity = 0; sanity < 500 && !hold.locked; sanity++) switch_yield(10000);
			fst_requires(hold.locked);

			/* The transfer must fail via the handshake timeout, i.e. take ~2s -
			 * a fast failure would mean it aborted before the vulnerable path. */
			t0 = switch_time_now();
			status = switch_ivr_transfer_recordings(sess_a, sess_b);
			dt_ms = (int) ((switch_time_now() - t0) / 1000);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] transfer(A, wrlocked B) = %d in %d ms (expect fail after ~2000ms)\n",
							  status, dt_ms);
			fst_check(status != SWITCH_STATUS_SUCCESS);
			fst_check(dt_ms > 1500);

			/* Revert post-conditions: the SAME bug, carrying the SAME helper,
			 * relinked on A; nothing on B. */
			bug_after = get_record_bug(sess_a);
			fst_check(bug_after == bug_before);
			fst_check(bug_after && switch_core_media_bug_get_user_data(bug_after) == rh_before);
			fst_check(get_record_bug(sess_b) == NULL);

			/* On the revert path A's channel private is still the live bug, so
			 * stop-by-name must work AND actually remove it (contrast with the
			 * post-success state, where test 1 proves it is dangling). */
			st = switch_ivr_stop_record_session(sess_a, record_file);
			fst_check(st == SWITCH_STATUS_SUCCESS);
			fst_check(get_record_bug(sess_a) == NULL);

			hold.release = 1;
			switch_thread_join(&st, t_hold);

			fst_requires(switch_core_session_read_lock(sess_b) == SWITCH_STATUS_SUCCESS);
			switch_channel_hangup(switch_core_session_get_channel(sess_a), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_channel_hangup(switch_core_session_get_channel(sess_b), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(sess_a);
			switch_core_session_rwunlock(sess_b);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "===== REVERT PATH DONE =====\n\n");
		}
		FST_TEST_END()

		/*
		 * 4b. Transfer to a dead target must abort early (media_bug.c:1145)
		 * and leave the recording untouched on A.
		 */
		FST_TEST_BEGIN(test_transfer_to_dead_target)
		{
			switch_core_session_t *sess_a = NULL, *sess_b = NULL;
			switch_media_bug_t *bug_before = NULL;
			void *rh_before = NULL;
			const char *record_file = NULL;
			switch_time_t t0;
			int dt_ms;
			switch_status_t status;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "===== TRANSFER TO DEAD TARGET =====\n");

			sess_a = originate_parked_null_session("A");
			fst_requires(sess_a);
			sess_b = originate_parked_null_session("B");
			fst_requires(sess_b);

			record_file = switch_core_session_sprintf(sess_a, "%s%s%s-dead.wav",
													  SWITCH_GLOBAL_dirs.temp_dir,
													  SWITCH_PATH_SEPARATOR,
													  switch_core_session_get_uuid(sess_a));
			fst_requires(switch_ivr_record_session_event(sess_a, (char *) record_file, 0, NULL, NULL) == SWITCH_STATUS_SUCCESS);
			switch_yield(100000);

			bug_before = get_record_bug(sess_a);
			fst_requires(bug_before);
			rh_before = switch_core_media_bug_get_user_data(bug_before);

			switch_channel_hangup(switch_core_session_get_channel(sess_b), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_yield(100000);

			t0 = switch_time_now();
			status = switch_ivr_transfer_recordings(sess_a, sess_b);
			dt_ms = (int) ((switch_time_now() - t0) / 1000);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] transfer(A, dead B) = %d in %d ms (expect fast fail)\n", status, dt_ms);
			fst_check(status != SWITCH_STATUS_SUCCESS);
			fst_check(dt_ms < 1000);

			fst_check(get_record_bug(sess_a) == bug_before);
			fst_check(bug_before && switch_core_media_bug_get_user_data(bug_before) == rh_before);
			fst_check(switch_ivr_stop_record_session(sess_a, record_file) == SWITCH_STATUS_SUCCESS);

			switch_channel_hangup(switch_core_session_get_channel(sess_a), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(sess_a);
			switch_core_session_rwunlock(sess_b);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "===== DEAD TARGET DONE =====\n\n");
		}
		FST_TEST_END()

		/*
		 * 4c. The boundary sweep - the actual race hunt. The recording
		 * thread's handshake retries run on a 1s cond-timedwait cadence
		 * (RECORDING_THREAD_COND_TIMEOUT_US), so its retries land at ~1.0s and
		 * ~2.0s after the signal - and INIT's sanity timeout fires at exactly
		 * 2.0s. The thread's last retry and INIT's revert are naturally
		 * synchronized to collide. Each round write-locks B and releases at a
		 * different offset across [1500..2400]ms, sweeping the release across
		 * both retry points and the revert boundary, while A's write path
		 * runs. The unsynchronized fields the collision races on:
		 *   rh->bug                (INIT writes :1572,:1586; thread reads :1444)
		 *   rh->thread_needs_transfer (INIT :1573,:1588; thread :1432,:1448)
		 *   rh->recording_session  (dup writes :2104-2108; thread reads :1435,:1442)
		 * Oracles:
		 *   - ASan (freed rh dereference)
		 *   - exactly-one-owner: a bug linked on BOTH sessions = the
		 *     double-owner corruption; on NEITHER = the recording vanished
		 *   - helper identity: rh must not change except across stop/restart
		 *   - bounded lock wait: if a late-latched recording thread is left
		 *     holding B's read lock, the next round's write lock never
		 *     acquires - reported as a failure, not a hang
		 */
		FST_TEST_BEGIN(test_transfer_revert_boundary_sweep)
		{
			switch_core_session_t *sess_a = NULL, *sess_b = NULL;
			switch_thread_t *t_hold = NULL, *t_write = NULL;
			switch_threadattr_t *thd_attr = NULL;
			struct writer_args wa = { 0 };
			const char *rounds_env = getenv("FS_REVERT_ROUNDS");
			int rounds = rounds_env && atoi(rounds_env) > 0 ? atoi(rounds_env) : 13;
			void *rh_expected = NULL;
			const char *record_file = NULL;
			int round, reverts = 0, successes = 0;
			switch_status_t st;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "===== REVERT BOUNDARY SWEEP (%d rounds) =====\n", rounds);

			g_stop = 0;

			sess_a = originate_parked_null_session("A");
			fst_requires(sess_a);
			sess_b = originate_parked_null_session("B");
			fst_requires(sess_b);

			record_file = switch_core_session_sprintf(sess_a, "%s%ssweep-0.wav",
													  SWITCH_GLOBAL_dirs.temp_dir, SWITCH_PATH_SEPARATOR);
			fst_requires(switch_ivr_record_session_event(sess_a, (char *) record_file, 0, NULL, NULL) == SWITCH_STATUS_SUCCESS);
			switch_yield(100000);
			{
				switch_media_bug_t *b0 = get_record_bug(sess_a);
				fst_requires(b0);
				rh_expected = switch_core_media_bug_get_user_data(b0);
			}

			switch_threadattr_create(&thd_attr, fst_pool);
			switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);

			/* A's write path runs throughout - production frame 3. (No writer
			 * on B: it would hold B's read lock and deadlock the holder.) */
			wa.session = sess_a; wa.label = "A";
			switch_thread_create(&t_write, thd_attr, writer_thread, &wa, fst_pool);

			/* B's read lock (ours from originate) must be free for the holder. */
			switch_core_session_rwunlock(sess_b);

			for (round = 0; round < rounds; round++) {
				struct wrlock_holder_args hold = { 0 };
				switch_media_bug_t *bug_a, *bug_b;
				int sanity, restarted = 0;

				hold.victim = sess_b;
				/* Span the ~2.0s INIT-timeout boundary whatever the round count:
				 * releases distributed across [1700..2300]ms. A fixed step
				 * (formerly 1500+75*round) failed to reach the boundary at all
				 * when FS_REVERT_ROUNDS was small - every transfer succeeded
				 * and the reverts>0 assertion rightly failed the run. */
				hold.auto_release_ms = 1700 + (round * 600) / (rounds > 1 ? rounds - 1 : 1);
				switch_thread_create(&t_hold, thd_attr, wrlock_holder_thread, &hold, fst_pool);

				/* Bounded wait: a leaked read lock on B from a previous
				 * round's late-latched recording thread shows up here. */
				for (sanity = 0; sanity < 1000 && !hold.locked; sanity++) switch_yield(10000);
				if (!hold.locked) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
									  "[TEST] round %d: write lock on B not acquired in 10s - "
									  "a stale read lock is being held (late-latch leak)\n", round);
					fst_check(hold.locked);
					break;
				}

				st = switch_ivr_transfer_recordings(sess_a, sess_b);
				switch_thread_join(&st, t_hold); /* also syncs holder exit; st clobbered is fine */
				switch_yield(50000);

				bug_a = get_record_bug(sess_a);
				bug_b = get_record_bug(sess_b);

				/* Exactly one owner, always. */
				fst_check(!(bug_a && bug_b));
				fst_check(bug_a || bug_b);

				/* One shared helper, stable across transfers and reverts. */
				{
					switch_media_bug_t *owner = bug_a ? bug_a : bug_b;
					if (owner) {
						fst_check(switch_core_media_bug_get_user_data(owner) == rh_expected);
					}
				}

				if (bug_b) {
					successes++;
					/* Bring it home for the next round (normal fast transfer;
					 * A is read-lockable, its thread answers immediately). */
					st = switch_ivr_transfer_recordings(sess_b, sess_a);
					fst_check(st == SWITCH_STATUS_SUCCESS);
				} else {
					reverts++;
				}

				/* Rotate the helper through a real destroy/create every few
				 * rounds so freed-rh dereferences have something to hit. */
				if ((round % 4) == 3) {
					if (switch_ivr_stop_record_session(sess_a, "all") == SWITCH_STATUS_SUCCESS) {
						record_file = switch_core_session_sprintf(sess_a, "%s%ssweep-%d.wav",
																  SWITCH_GLOBAL_dirs.temp_dir,
																  SWITCH_PATH_SEPARATOR, round + 1);
						if (switch_ivr_record_session_event(sess_a, (char *) record_file, 0, NULL, NULL) == SWITCH_STATUS_SUCCESS) {
							switch_media_bug_t *nb;
							switch_yield(100000);
							nb = get_record_bug(sess_a);
							fst_check(nb != NULL);
							rh_expected = nb ? switch_core_media_bug_get_user_data(nb) : NULL;
							restarted = 1;
						}
					}
					fst_check(restarted);
				}

				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "[TEST] round %d: release=%dms -> %s%s\n",
								  round, hold.auto_release_ms,
								  bug_b ? "transfer SUCCEEDED" : "REVERTED",
								  restarted ? " (helper rotated)" : "");
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] sweep done: %d reverts, %d successes, framesA=%d\n",
							  reverts, successes, wa.frames_written);
			fst_check(reverts > 0);    /* the sweep must actually cross the boundary */
			fst_check(successes > 0);  /* ...from both sides */

			g_stop = 1;
			switch_thread_join(&st, t_write);

			switch_ivr_stop_record_session(sess_a, "all");
			fst_requires(switch_core_session_read_lock(sess_b) == SWITCH_STATUS_SUCCESS);
			switch_channel_hangup(switch_core_session_get_channel(sess_a), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_channel_hangup(switch_core_session_get_channel(sess_b), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(sess_a);
			switch_core_session_rwunlock(sess_b);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "===== BOUNDARY SWEEP DONE =====\n\n");
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
