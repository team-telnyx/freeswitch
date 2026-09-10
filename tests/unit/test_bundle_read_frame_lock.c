/* Regression tests for the BUNDLE read and drain defects found alongside
 * TELCORE-453: TELCORE-454 (the flush freeing a frame a reader still holds),
 * TELCORE-455 (bundle_drain_thread_stop leaving the handle joinable twice),
 * TELCORE-456 (the drain thread holding no session read lock), TELCORE-457
 * (SFF_DYNAMIC copied onto a pool-embedded frame) and TELCORE-459 (the BUNDLE
 * group rebuilt under the SIP thread while the drain thread traverses it).
 *
 * struct switch_media_handle_s and struct switch_rtp_engine_s are private to
 * switch_core_media.c and the drain helpers are static, so these drive the
 * switch_core_media_test_* seam declared in switch_core_media.h.
 *
 * Every case asserts a steady state held open rather than racing for a window,
 * so none of them is probabilistic. Each pins one production line; see the
 * per-test comments. */

#define SWITCH_CORE_MEDIA_TEST_HOOKS
#include <switch.h>
#include <test/switch_test.h>

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

/* A malloc-backed clone of the shape bundle_frame_dup() produces
 * (switch_core_media.c:4062-4106): SFF_DYNAMIC, heap frame, heap data. */
static switch_frame_t *make_dynamic_frame(void)
{
	switch_frame_t *frame = malloc(sizeof(*frame));

	if (!frame) return NULL;

	memset(frame, 0, sizeof(*frame));
	frame->data = malloc(SWITCH_RECOMMENDED_BUFFER_SIZE);
	if (!frame->data) {
		free(frame);
		return NULL;
	}
	frame->buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;
	frame->datalen = 160;
	switch_set_flag(frame, SFF_DYNAMIC);

	return frame;
}

/* Stands in for the drain thread: parks until released, so a stop() that reaches
 * switch_thread_join() stays inside the join for as long as the test wants. */
struct parked_thread {
	volatile int release;
	volatile int running;
};

static void *SWITCH_THREAD_FUNC parked_thread_fn(switch_thread_t *thread, void *obj)
{
	struct parked_thread *p = (struct parked_thread *) obj;

	p->running = 1;
	while (!p->release) {
		switch_yield(5000);
	}

	return NULL;
}

struct demux_job {
	switch_core_session_t *session;
	volatile int started;
	volatile int done;
};

static void *SWITCH_THREAD_FUNC demux_thread_fn(switch_thread_t *thread, void *obj)
{
	struct demux_job *job = (struct demux_job *) obj;

	job->started = 1;
	switch_core_media_test_demux_media_type(job->session, "0", 0x1234, 96);
	job->done = 1;

	return NULL;
}

struct stop_job {
	switch_core_session_t *session;
	volatile int done;
};

static void *SWITCH_THREAD_FUNC stop_thread_fn(switch_thread_t *thread, void *obj)
{
	struct stop_job *job = (struct stop_job *) obj;

	switch_core_media_test_drain_thread_stop(job->session);
	job->done = 1;

	return NULL;
}

FST_CORE_BEGIN("./conf")
{
	FST_SUITE_BEGIN(bundle_read_frame_lock)
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

		/* The bundled video read branch leaves engine->read_frame aliasing the
		 * popped clone and hands the caller &engine->read_frame, then releases
		 * read_mutex before returning. So a flush that frees engine->read_fb_frame
		 * pulls the block out from under a caller that is still using it -- taking
		 * the read mutex inside the flush does not help, because the caller is no
		 * longer holding it. The invariant is that the flush must not touch the
		 * in-flight frame at all; the reader owns it until its next pop, and
		 * switch_media_handle_destroy() owns it after teardown.
		 *
		 * The queue-draining half of the flush is unchanged, so it needs no new
		 * assertion here. */
		FST_TEST_BEGIN(test_flush_queued_read_frames_leaves_the_in_flight_frame)
		{
			switch_core_session_t *session = NULL;
			switch_frame_t *frame = NULL;
			switch_frame_t *queued = NULL;
			switch_frame_t *popped = NULL;

			session = originate_null_session();
			fst_requires(session);
			fst_requires(attach_media_handle(session) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_media_test_prepare_read_fb(session, SWITCH_MEDIA_TYPE_VIDEO) == SWITCH_STATUS_SUCCESS);

			/* Stands in for the frame a reader has already been handed. */
			frame = make_dynamic_frame();
			fst_requires(frame);
			switch_core_media_test_set_read_fb_frame(session, SWITCH_MEDIA_TYPE_VIDEO, frame);
			fst_requires(switch_core_media_test_get_read_fb_frame(session, SWITCH_MEDIA_TYPE_VIDEO) == frame);

			/* Something queued, so the flush has real work to do -- and so this test
			 * can tell "left the in-flight frame alone" from "did nothing at all". */
			queued = make_dynamic_frame();
			fst_requires(queued);
			fst_requires(switch_core_media_test_push_read_fb(session, SWITCH_MEDIA_TYPE_VIDEO, queued) == SWITCH_STATUS_SUCCESS);

			/* The realistic case: the reader has returned, so nothing holds the mutex. */
			switch_core_media_test_flush_queued_read_frames(session, SWITCH_MEDIA_TYPE_VIDEO);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] in-flight frame after flush = %p (expect %p)\n",
							  (void *) switch_core_media_test_get_read_fb_frame(session, SWITCH_MEDIA_TYPE_VIDEO),
							  (void *) frame);
			fst_check(switch_core_media_test_get_read_fb_frame(session, SWITCH_MEDIA_TYPE_VIDEO) == frame);

			/* THE OTHER HALF: the queue must be empty. Without this a flush whose body
			 * is `return;` satisfies the assertion above and the test is vacuous. */
			popped = switch_core_media_test_pop_read_fb(session, SWITCH_MEDIA_TYPE_VIDEO);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] queued frame still in the buffer after flush = %p (expect NULL)\n", (void *) popped);
			fst_check(popped == NULL);

			switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session);
		}
		FST_TEST_END()

		/* bundle_drain_thread_stop() latches the thread handle at
		 * switch_core_media.c:4448, drops bundle_drain_mutex at :4458, joins at
		 * :4478 and only then clears the handle at :4482. A second stop arriving
		 * in that window -- switch_core_media_activate_rtp() has no reentrancy
		 * guard, and mod_sofia calls it both locked (sofia_media_activate_rtp)
		 * and unlocked (sofia_media_activate_rtp_unlocked) -- reads the same
		 * non-NULL handle and joins it a second time. The loser then stores NULL
		 * over whatever handle the winner has since started, orphaning a live
		 * drain thread that keeps using the session after it is destroyed.
		 *
		 * The invariant: once a stop has committed to tearing the thread down it
		 * must publish that, so no concurrent stop can latch the same handle.
		 * This test parks the drain thread inside the join and asserts the handle
		 * is no longer visible. */
		FST_TEST_BEGIN(test_drain_thread_stop_does_not_expose_a_joined_handle)
		{
			switch_core_session_t *session = NULL;
			switch_memory_pool_t *pool = NULL;
			switch_threadattr_t *thd_attr = NULL;
			switch_thread_t *parked = NULL, *stopper = NULL;
			struct parked_thread park = { 0, 0 };
			struct stop_job job;
			switch_thread_t *visible = NULL;
			switch_status_t st;
			int waited;

			session = originate_null_session();
			fst_requires(session);
			fst_requires(attach_media_handle(session) == SWITCH_STATUS_SUCCESS);

			pool = switch_core_session_get_pool(session);
			switch_threadattr_create(&thd_attr, pool);
			switch_threadattr_detach_set(thd_attr, 0);
			switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
			fst_requires(switch_thread_create(&parked, thd_attr, parked_thread_fn, &park, pool) == SWITCH_STATUS_SUCCESS);

			for (waited = 0; waited < 500 && !park.running; waited++) {
				switch_yield(10000);
			}
			fst_requires(park.running == 1);

			switch_core_media_test_arm_drain(session, parked);
			fst_requires(switch_core_media_test_get_drain_thread(session) == parked);

			job.session = session;
			job.done = 0;
			fst_requires(switch_thread_create(&stopper, thd_attr, stop_thread_fn, &job, pool) == SWITCH_STATUS_SUCCESS);

			/* The stopper is now inside switch_thread_join() on the parked thread. */
			switch_yield(300000);
			fst_requires(job.done == 0);

			/* THE CRUX. A concurrent stop reads this handle and joins it too. */
			visible = switch_core_media_test_get_drain_thread(session);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] drain thread handle still visible during join = %p (expect NULL)\n", (void *) visible);
			fst_check(visible == NULL);

			park.release = 1;

			for (waited = 0; waited < 500 && !job.done; waited++) {
				switch_yield(10000);
			}
			fst_check(job.done == 1);

			switch_thread_join(&st, stopper);
			fst_check(switch_core_media_test_get_drain_thread(session) == NULL);

			switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session);
		}
		FST_TEST_END()

		/* Every other session-attached thread in switch_core_media.c opens with
		 * switch_core_session_read_lock() and closes with switch_core_session_rwunlock():
		 * video_write_thread (:10811), audio_write_thread (:11248), text_helper_thread
		 * (:11362), video_helper_thread (:11526), dtls_init_job (:22060).
		 * bundle_drain_thread_func (:4227) takes none. Its only lifetime guarantee is
		 * the switch_thread_join() in bundle_drain_thread_stop(), so any path that
		 * misses that join leaves it running against a destroyed session -- and its
		 * own thread handle is allocated from that session's pool.
		 *
		 * Measured as a controlled experiment, because the test itself holds a read
		 * lock from originate: release that first, confirm the write lock is then
		 * takeable (so the probe means something), start the drain thread, and confirm
		 * it is no longer takeable. */
		FST_TEST_BEGIN(test_drain_thread_holds_a_session_read_lock)
		{
			switch_core_session_t *session = NULL;
			switch_memory_pool_t *pool = NULL;
			switch_rtp_t *rtp = NULL;
			switch_rtp_flag_t rtp_flags[SWITCH_RTP_FLAG_INVALID] = { 0 };
			const char *err = NULL;
			switch_status_t before, during;
			int waited;

			session = originate_null_session();
			fst_requires(session);
			fst_requires(attach_media_handle(session) == SWITCH_STATUS_SUCCESS);

			pool = switch_core_session_get_pool(session);
			rtp = switch_rtp_new("127.0.0.1", 12380, "127.0.0.1", 12382, 8, 8000, 20 * 1000,
								 rtp_flags, "soft", &err, pool);
			fst_requires(rtp);
			fst_requires(switch_rtp_ready(rtp));

			fst_requires(switch_core_media_test_prepare_bundle_drain(session, rtp) == SWITCH_STATUS_SUCCESS);

			/* Drop the read lock originate handed us, or the probe below can never
			 * succeed and the measurement would be meaningless. */
			switch_core_session_rwunlock(session);

			/* CONTROL: with no drain thread running the write lock must be takeable.
			 * If this fails the probe proves nothing and the test is invalid. */
			before = switch_core_media_test_try_session_write_lock(session);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] control: write lock takeable before drain starts = %d (expect 1)\n",
							  before == SWITCH_STATUS_SUCCESS);
			fst_requires(before == SWITCH_STATUS_SUCCESS);

			switch_core_media_test_drain_thread_start(session);

			for (waited = 0; waited < 500 && !switch_core_media_test_get_drain_thread(session); waited++) {
				switch_yield(10000);
			}
			fst_requires(switch_core_media_test_get_drain_thread(session) != NULL);
			switch_yield(200000);

			/* THE CRUX: a running session-attached thread must hold a read lock. */
			during = switch_core_media_test_try_session_write_lock(session);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] write lock still takeable while the drain thread runs = %d (expect 0)\n",
							  during == SWITCH_STATUS_SUCCESS);
			fst_check(during == SWITCH_STATUS_FALSE);

			switch_core_media_test_drain_thread_stop(session);

			fst_requires(switch_core_session_read_lock(session) == SWITCH_STATUS_SUCCESS);
			switch_rtp_destroy(&rtp);
			switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session);
		}
		FST_TEST_END()

		/* bundle_drain_thread_func() takes the session read lock with a TRYlock that
		 * also fails when the channel is already down, and it does that after
		 * bundle_drain_thread_start() has published BUNDLE_DRAIN_STARTING. If the
		 * failure path returns without unwinding that state, the drain state is
		 * stranded at STARTING with a dead thread: start() then refuses forever
		 * (state != INACTIVE) and an audio reader that lands on the STARTING branch
		 * spins on switch_yield(1000) inside a loop whose only other condition,
		 * SCMF_RUNNING, is never cleared anywhere in the tree.
		 *
		 * State values mirror the private defines: 0 INACTIVE, 1 STARTING. */
		FST_TEST_BEGIN(test_drain_thread_unwinds_state_when_the_read_lock_fails)
		{
			switch_core_session_t *session = NULL;
			switch_memory_pool_t *pool = NULL;
			switch_rtp_t *rtp = NULL;
			switch_rtp_flag_t rtp_flags[SWITCH_RTP_FLAG_INVALID] = { 0 };
			const char *err = NULL;
			int waited, state, held_write_lock = 0;

			session = originate_null_session();
			fst_requires(session);
			fst_requires(attach_media_handle(session) == SWITCH_STATUS_SUCCESS);

			pool = switch_core_session_get_pool(session);
			rtp = switch_rtp_new("127.0.0.1", 12388, "127.0.0.1", 12390, 8, 8000, 20 * 1000,
								 rtp_flags, "soft", &err, pool);
			fst_requires(rtp);
			fst_requires(switch_core_media_test_prepare_bundle_drain(session, rtp) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_media_test_get_drain_state(session) == 0);

			/* Make the read lock fail while leaving the channel UP. Hanging up instead
			 * would make this vacuous: with the channel down the thread promotes to
			 * RUNNING, the loop condition fails immediately and the tail sets INACTIVE
			 * anyway, so state 0 would be satisfied by a thread that never attempted
			 * the lock at all. Holding the session write lock makes the trylock fail
			 * and nothing else. */
			switch_core_session_rwunlock(session);
			held_write_lock = 1;
			fst_requires(switch_core_media_test_hold_session_write_lock(session) == SWITCH_STATUS_SUCCESS);

			switch_core_media_test_drain_thread_start(session);
			fst_requires(switch_core_media_test_get_drain_thread(session) != NULL);

			/* Let the thread reach its read-lock attempt and give up. */
			for (waited = 0; waited < 200 && switch_core_media_test_get_drain_state(session) != 0; waited++) {
				switch_yield(10000);
			}

			state = switch_core_media_test_get_drain_state(session);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] drain state after a failed read lock = %d (expect 0 INACTIVE)\n", state);
			fst_check(state == 0);

			switch_core_media_test_release_session_write_lock(session);
			held_write_lock = 0;
			fst_requires(switch_core_session_read_lock(session) == SWITCH_STATUS_SUCCESS);

			switch_core_media_test_drain_thread_stop(session);
			switch_rtp_destroy(&rtp);
			switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session);
			(void) held_write_lock;
		}
		FST_TEST_END()

		/* Callers of bundle_drain_thread_stop() rely on "returned means the thread
		 * is joined": switch_core_media_deactivate_rtp() destroys the RTP session
		 * the drain thread blocks inside, and switch_media_handle_destroy()
		 * destroys the frame buffer it pushes into. Since 4941a8254f takes the
		 * handle exclusively, a second concurrent stop finds NULL -- it must WAIT
		 * for the owner to finish the join, not return early. Concurrent stops are
		 * reachable: sofia drops tech_pvt->sofia_mutex across
		 * sofia_media_activate_rtp_unlocked(). */
		FST_TEST_BEGIN(test_second_drain_thread_stop_waits_for_the_join)
		{
			switch_core_session_t *session = NULL;
			switch_memory_pool_t *pool = NULL;
			switch_threadattr_t *thd_attr = NULL;
			switch_thread_t *stopper = NULL, *parked = NULL;
			struct parked_thread park = { 0, 0 };
			struct stop_job job;
			switch_status_t st;
			int waited, second_returned_early;

			session = originate_null_session();
			fst_requires(session);
			fst_requires(attach_media_handle(session) == SWITCH_STATUS_SUCCESS);
			pool = switch_core_session_get_pool(session);

			switch_threadattr_create(&thd_attr, pool);
			switch_threadattr_detach_set(thd_attr, 0);
			switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);

			/* A stand-in drain thread that stays alive until released, so the first
			 * stop is provably parked inside switch_thread_join(). */
			fst_requires(switch_thread_create(&parked, thd_attr, parked_thread_fn, &park, pool) == SWITCH_STATUS_SUCCESS);
			for (waited = 0; waited < 500 && !park.running; waited++) {
				switch_yield(10000);
			}
			fst_requires(park.running == 1);
			switch_core_media_test_arm_drain(session, parked);

			job.session = session;
			job.done = 0;
			fst_requires(switch_thread_create(&stopper, thd_attr, stop_thread_fn, &job, pool) == SWITCH_STATUS_SUCCESS);

			/* First stopper is now inside the join and owns the handle. */
			switch_yield(300000);
			fst_requires(job.done == 0);
			fst_requires(switch_core_media_test_get_drain_thread(session) == NULL);

			/* THE CRUX: a second stop must block until the thread is really gone. */
			{
				struct stop_job job2;
				switch_thread_t *stopper2 = NULL;

				job2.session = session;
				job2.done = 0;
				fst_requires(switch_thread_create(&stopper2, thd_attr, stop_thread_fn, &job2, pool) == SWITCH_STATUS_SUCCESS);
				switch_yield(300000);
				second_returned_early = job2.done;
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "[TEST] second stop returned while the thread was still running = %d (expect 0)\n",
								  second_returned_early);
				fst_check(second_returned_early == 0);

				/* Release, then both stops must complete. */
				park.release = 1;
				for (waited = 0; waited < 500 && !(job.done && job2.done); waited++) {
					switch_yield(10000);
				}
				fst_check(job.done == 1);
				fst_check(job2.done == 1);
				switch_thread_join(&st, stopper2);
			}

			switch_thread_join(&st, stopper);
			switch_thread_join(&st, parked);

			switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session);
		}
		FST_TEST_END()

		/* bundle_frame_dup() marks its clones SFF_DYNAMIC (switch_core_media.c:4072,
		 * :4098) because they are malloc-backed and released with switch_frame_free().
		 * The bundled-video read branch then copies the whole frame, flags included,
		 * onto engine->read_frame (:4675), which is embedded in switch_rtp_engine_t
		 * inside the session-pool media handle -- and hands it to the caller.
		 *
		 * switch_frame_free() treats SFF_DYNAMIC as "this is heap, free it": it would
		 * free() an interior pointer into the session pool and release the clone's
		 * packet out from under engine->read_fb_frame. Nothing in tree frees a
		 * returned read frame today, so this is latent rather than live, but the flag
		 * stops being a usable ownership discriminator. */
		FST_TEST_BEGIN(test_bundled_video_read_frame_is_not_marked_dynamic)
		{
			switch_core_session_t *session = NULL;
			switch_memory_pool_t *pool = NULL;
			switch_rtp_t *rtp = NULL;
			switch_rtp_flag_t rtp_flags[SWITCH_RTP_FLAG_INVALID] = { 0 };
			const char *err = NULL;
			switch_frame_t *clone = NULL, *out = NULL;
			switch_status_t st;

			session = originate_null_session();
			fst_requires(session);
			fst_requires(attach_media_handle(session) == SWITCH_STATUS_SUCCESS);

			pool = switch_core_session_get_pool(session);
			rtp = switch_rtp_new("127.0.0.1", 12360, "127.0.0.1", 12362, 8, 8000, 20 * 1000,
								 rtp_flags, "soft", &err, pool);
			fst_requires(rtp);
			fst_requires(switch_rtp_ready(rtp));

			fst_requires(switch_core_media_test_prepare_bundle_drain(session, rtp) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_media_test_prepare_engine_read(session, SWITCH_MEDIA_TYPE_VIDEO, rtp) == SWITCH_STATUS_SUCCESS);

			clone = make_dynamic_frame();
			fst_requires(clone);
			fst_requires(switch_test_flag(clone, SFF_DYNAMIC));
			fst_requires(switch_core_media_test_push_read_fb(session, SWITCH_MEDIA_TYPE_VIDEO, clone) == SWITCH_STATUS_SUCCESS);

			st = switch_core_media_read_frame(session, &out, SWITCH_IO_FLAG_NONE, 0, SWITCH_MEDIA_TYPE_VIDEO);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] read_frame status=%d frame=%p\n", st, (void *) out);
			fst_requires(st == SWITCH_STATUS_SUCCESS);
			fst_requires(out != NULL);
			/* Without this the dummy CNG frame the branch returns on a pop miss would
			 * satisfy every assertion below and the test would be vacuous. */
			fst_requires(switch_core_media_test_get_read_fb_frame(session, SWITCH_MEDIA_TYPE_VIDEO) == clone);
			fst_requires(out->data == clone->data);

			/* THE CRUX: the frame handed out lives in the session pool, so it must
			 * not advertise itself as heap-owned. */
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] returned read frame carries SFF_DYNAMIC = %d (expect 0)\n",
							  switch_test_flag(out, SFF_DYNAMIC) ? 1 : 0);
			fst_check(!switch_test_flag(out, SFF_DYNAMIC));

			switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session);
		}
		FST_TEST_END()

		/* The SIP thread re-initialises smh->bundle wholesale on every received SDP
		 * (switch_bundle_group_init memsets the group, then add_mline repopulates
		 * it), while the drain thread and the audio read path traverse the same
		 * group per packet and write SSRC bindings into it. Nothing serialised the
		 * two. The demux must therefore not proceed while a writer holds the bundle
		 * lock. Deterministic: the lock is held open, not raced for. */
		FST_TEST_BEGIN(test_bundle_demux_serialises_against_the_writer)
		{
			switch_core_session_t *session = NULL;
			switch_memory_pool_t *pool = NULL;
			switch_threadattr_t *thd_attr = NULL;
			switch_thread_t *reader = NULL;
			struct demux_job job;
			switch_status_t st;
			int waited;

			session = originate_null_session();
			fst_requires(session);
			fst_requires(attach_media_handle(session) == SWITCH_STATUS_SUCCESS);
			pool = switch_core_session_get_pool(session);

			/* Stands in for the SIP thread inside switch_core_media_bundle_populate_from_sdp(). */
			switch_core_media_test_lock_bundle(session);

			job.session = session;
			job.done = 0;
			job.started = 0;

			switch_threadattr_create(&thd_attr, pool);
			switch_threadattr_detach_set(thd_attr, 0);
			switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
			fst_requires(switch_thread_create(&reader, thd_attr, demux_thread_fn, &job, pool) == SWITCH_STATUS_SUCCESS);

			for (waited = 0; waited < 500 && !job.started; waited++) {
				switch_yield(10000);
			}
			/* Proves the reader really reached the demux, so job.done == 0 below
			 * cannot mean "the thread never ran". */
			fst_requires(job.started == 1);

			switch_yield(300000);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] demux completed while the bundle writer held the lock = %d (expect 0)\n", job.done);
			fst_check(job.done == 0);

			switch_core_media_test_unlock_bundle(session);

			for (waited = 0; waited < 500 && !job.done; waited++) {
				switch_yield(10000);
			}
			fst_check(job.done == 1);
			switch_thread_join(&st, reader);

			switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
