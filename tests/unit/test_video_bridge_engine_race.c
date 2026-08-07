/*
 * Regression test for the video-bridge engine-function use-after-free crash.
 *
 * Production stack (release build line numbers):
 *   switch_core_session_perform_kill_channel()  switch_core_io.c:1165   <-- crash
 *   video_bridge_thread()                        switch_ivr_bridge.c:293
 *   video_helper_thread()                        switch_core_media.c:9953
 *   dummy_worker() / start_thread() / clone3()
 *
 * Root cause
 * ----------
 * A bridge runs on the audio bridge thread and, for video, hands a
 * *stack-allocated* context (`struct vid_helper vh`) to another thread via:
 *
 *      switch_core_media_start_engine_function(session_a, VIDEO,
 *                                              video_bridge_thread, &vh);
 *
 * start_engine_function() only publishes the job:
 *      engine->engine_function      = video_bridge_thread;   // set
 *      engine->engine_user_data     = &vh;                   // set (stack ptr!)
 *      engine->engine_function_running = 0;                  // NOT yet started
 *
 * The media thread (video_helper_thread) flips engine_function_running to 1
 * only later, when it actually picks the job up.
 *
 * At bridge teardown the audio thread decides whether it must wait for that job
 * with:
 *      if (switch_core_media_check_engine_function(session_a, VIDEO)) {
 *          ...
 *          switch_core_media_end_engine_function(session_a, VIDEO);   // waits
 *      }
 *
 * but check_engine_function() reported only `engine_function_running > 0`. In
 * the window between "job submitted" and "job picked up", that is 0, so the
 * bridge SKIPS end_engine_function(), returns, and its stack frame (holding
 * `vh`) is destroyed. The media thread then finally runs the job with
 * user_data == &vh pointing at a dead stack frame -> video_bridge_thread()
 * dereferences a dangling `vh->session_b` and kill_channel() faults on its
 * garbage endpoint_interface.
 *
 * The fix
 * -------
 *  - check_engine_function() reports an outstanding job as soon as it is
 *    submitted (engine_function != NULL), not only once it is running.
 *  - end_engine_function() cancels a submitted-but-not-started job in place.
 *  - the media-thread pickup latches the job under control_mutex and re-checks
 *    engine_function, so a concurrent cancel is race-free.
 *
 * What this test does
 * -------------------
 * It drives the exact public contract the bridge relies on, deterministically,
 * with no reliance on real video media:
 *
 *   1. Originate an answered "null" session and give it a media handle.
 *   2. Submit an engine function to the VIDEO engine while CF_VIDEO is OFF, so
 *      start_video_thread() is a no-op and NOBODY can pick the job up: it stays
 *      pending forever. This freezes the exact race window deterministically.
 *   3. Assert check_engine_function() == true.  <-- the crux.
 *          BUGGY tree: returns 0  -> assertion FAILS (this is the bug: the
 *                      bridge would skip the wait and free the stack context).
 *          FIXED tree: returns 1.
 *   4. Run the bridge's teardown guard verbatim
 *          if (check_engine_function()) end_engine_function();
 *      and assert the pending job is gone afterwards (check == false), i.e. it
 *      is now safe to free the context. On a buggy tree the guard is skipped
 *      and the job would still be live.
 *   5. Free the heap context that stood in for `vh`, then start a REAL video
 *      helper thread and give it time to pick up anything left behind. On a
 *      fixed tree nothing is left (job cancelled) so the helper never touches
 *      the freed context; on a buggy tree the stale job fires and reads the
 *      freed context (SIGSEGV / ASan use-after-free), exactly as in production.
 */

#include <switch.h>
#include <test/switch_test.h>

/* Stand-in for the bridge's stack-allocated `struct vid_helper vh`. Heap so we
 * can free it (modelling the stack frame going out of scope) and hand the media
 * thread a dangling pointer, exactly like the production bug. */
typedef struct {
	uint32_t canary;
	switch_core_session_t *session_b; /* like vh->session_b */
	volatile int *fn_ran;
} engine_ctx_t;

#define ENGINE_CTX_CANARY 0xC0FFEEu

static volatile int g_fn_ran = 0;

/* Stands in for video_bridge_thread(): the media thread invokes this with the
 * user_data captured at submit time. Its final act mirrors
 * video_bridge_thread()'s: switch_core_session_kill_channel(vh->session_b, ...).
 * When the context is dangling, session_b is garbage and kill_channel() faults
 * on its endpoint_interface exactly like switch_core_io.c:1165 in production. */
static void test_engine_function(switch_core_session_t *session, void *user_data)
{
	engine_ctx_t *ctx = (engine_ctx_t *) user_data;

	(void) session;
	g_fn_ran = 1;
	/* video_bridge_thread()'s final act (switch_ivr_bridge.c:292). On a dangling
	 * context session_b is a wild pointer and this faults in
	 * switch_core_session_perform_kill_channel(), the production frame 0. */
	switch_core_session_kill_channel(ctx->session_b, SWITCH_SIG_BREAK);
}

static switch_core_session_t *originate_null_session(void)
{
	switch_core_session_t *session = NULL;
	switch_call_cause_t cause = SWITCH_CAUSE_NONE;
	switch_status_t status;

	status = switch_ivr_originate(NULL, &session, &cause,
								  "null/+15553334444",
								  0, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);

	if (status != SWITCH_STATUS_SUCCESS || !session) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "[TEST] originate failed: status=%d cause=%d\n", status, cause);
		return NULL;
	}
	return session;
}

/* Give the session a media handle so the engine-function API (which needs
 * smh->control_mutex) is usable. */
static switch_status_t attach_media_handle(switch_core_session_t *session)
{
	switch_media_handle_t *media_handle = NULL;
	switch_core_media_params_t *mparams;

	mparams = switch_core_session_alloc(session, sizeof(*mparams));
	mparams->num_codecs = 1;
	mparams->inbound_codec_string = switch_core_session_strdup(session, "PCMU");
	mparams->outbound_codec_string = switch_core_session_strdup(session, "PCMU");
	mparams->rtpip = switch_core_session_strdup(session, "127.0.0.1");

	return switch_media_handle_create(&media_handle, session, mparams);
}

FST_CORE_BEGIN("./conf")
{
	FST_SUITE_BEGIN(video_bridge_engine_race)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_loopback");
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		/*
		 * The deterministic contract test. Fails on a buggy tree at step 3,
		 * passes on a fixed tree.
		 */
		FST_TEST_BEGIN(test_pending_engine_function_is_waited_on)
		{
			switch_core_session_t *session = NULL;
			switch_channel_t *channel = NULL;
			engine_ctx_t *ctx = NULL;
			int fn_ran_flag = 0;
			int outstanding;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "===== VIDEO-BRIDGE ENGINE-FUNCTION RACE TEST =====\n");

			session = originate_null_session();
			fst_requires(session);
			channel = switch_core_session_get_channel(session);
			fst_requires(channel);
			fst_requires(attach_media_handle(session) == SWITCH_STATUS_SUCCESS);

			/* Heap context standing in for the bridge's stack `vh`. */
			ctx = malloc(sizeof(*ctx));
			fst_requires(ctx);
			ctx->canary = ENGINE_CTX_CANARY;
			ctx->session_b = session; /* stands in for vh->session_b */
			ctx->fn_ran = &fn_ran_flag;
			g_fn_ran = 0;

			/* CF_VIDEO is OFF: start_video_thread() is a no-op, so no media
			 * thread exists to pick the job up. The job is pinned "pending",
			 * freezing the production race window deterministically. */
			fst_requires(!switch_channel_test_flag(channel, CF_VIDEO));

			switch_core_media_start_engine_function(session, SWITCH_MEDIA_TYPE_VIDEO,
													test_engine_function, ctx);

			/* THE CRUX: a job was submitted, so the bridge's teardown guard must
			 * see an outstanding job and wait for it. */
			outstanding = switch_core_media_check_engine_function(session, SWITCH_MEDIA_TYPE_VIDEO);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] check_engine_function after submit = %d (expect 1)\n", outstanding);
			fst_check(outstanding == 1);

			/* Bridge teardown guard, verbatim (switch_ivr_bridge.c:1221). */
			if (switch_core_media_check_engine_function(session, SWITCH_MEDIA_TYPE_VIDEO)) {
				switch_core_media_end_engine_function(session, SWITCH_MEDIA_TYPE_VIDEO);
			}

			/* After the guard the job must be resolved, so freeing the context
			 * is safe. */
			outstanding = switch_core_media_check_engine_function(session, SWITCH_MEDIA_TYPE_VIDEO);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] check_engine_function after teardown guard = %d (expect 0)\n", outstanding);
			fst_check(outstanding == 0);

			/* Free the stand-in for the stack context, then overwrite the freed
			 * slot with garbage - modelling the bridge's stack frame going out
			 * of scope and being reused. glibc hands the just-freed block back
			 * to the next same-size malloc, so ctx->session_b now reads as a
			 * wild pointer, exactly like a dangling vh->session_b. */
			free(ctx);
			{
				int i;
				for (i = 0; i < 8; i++) {
					void *p = malloc(sizeof(engine_ctx_t));
					memset(p, 0xAB, sizeof(engine_ctx_t));
				}
			}
			/* ctx is intentionally left pointing at the freed/overwritten slot. */

			/* Now let a REAL video helper thread run and pick up anything that
			 * was (incorrectly) left pending. On a fixed tree the job was
			 * cancelled, so it must never fire against the freed context; on a
			 * buggy tree it invokes test_engine_function() -> kill_channel() on
			 * the wild session_b and SIGSEGVs, reproducing the production stack. */
			switch_channel_set_flag(channel, CF_VIDEO);
			switch_core_session_start_video_thread(session);

			switch_yield(500000); /* 0.5s for the helper to loop a few times */

			fst_check(g_fn_ran == 0);
			fst_check(fn_ran_flag == 0);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] stale engine function ran = %d (expect 0)\n", g_fn_ran);

			switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "===== TEST COMPLETED =====\n\n");
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
