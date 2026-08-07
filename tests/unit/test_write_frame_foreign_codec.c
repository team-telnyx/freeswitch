/*
 * Regression test for the crash in switch_core_session_write_frame().
 *
 * Production stack (release build line numbers):
 *   switch_mutex_unlock()                 switch_apr.c
 *   switch_core_session_write_frame()     switch_core_media.c   <-- crash
 *   switch_ivr_originate()                switch_ivr_originate.c
 *   audio_bridge_function()               mod_dptools.c
 *
 * Root cause
 * ----------
 * switch_core_session_write_frame() does, near the top:
 *
 *      switch_mutex_lock(session->write_codec->mutex);
 *      switch_mutex_lock(frame->codec->mutex);          // <-- frame->codec
 *      ...
 *  error:
 *      switch_mutex_unlock(session->write_codec->mutex);
 *      switch_mutex_unlock(frame->codec->mutex);        // <-- crash
 *      switch_mutex_unlock(session->codec_write_mutex);
 *
 * It holds `session->codec_write_mutex`, which only serialises against the
 * SAME session's codec teardown. But `frame->codec` is frequently a codec that
 * belongs to a *different* session. In switch_ivr_originate()'s ringback /
 * bridge_early_media path, setup_ringback() sets:
 *
 *      write_frame->codec = switch_core_session_get_write_codec(peer_session);
 *
 * so write_frame() locks/unlocks the PEER's codec mutex while holding only its
 * own codec_write_mutex. When that peer codec is torn down concurrently (codec
 * renegotiation, or the peer session being reaped during originate), its
 * `mutex` field is cleared / freed, and write_frame() dereferences it after the
 * readiness check at the top - faulting inside __pthread_mutex_lock/unlock.
 *
 * This is the cross-session sibling of the read_frame crash fixed in TEL-6515
 * ("Properly lock mutexes", test_inuse_race.c): that fix made a session's own
 * teardown take codec_read_mutex/codec_write_mutex, which closes the
 * SAME-session race but cannot protect a codec borrowed from another session.
 *
 * What this test does
 * -------------------
 * It reproduces the exact faulting condition deterministically:
 *
 *   1. Originate an answered "null" session to write TO (kept alive/rwlocked).
 *   2. Build a frame whose ->codec is a SEPARATE codec we own (standing in for
 *      the peer's codec passed by setup_ringback()).
 *   3. Baseline write -> succeeds, proving the frame->codec->mutex lock path is
 *      actually exercised.
 *   4. Simulate the codec being torn down underneath the writer by clearing its
 *      ->mutex (this is the state switch_core_codec_destroy() leaves it in: the
 *      whole codec struct is memset, so ->mutex is NULL). The codec is otherwise
 *      still flagged ready, mimicking the narrow window where write_frame() has
 *      already passed its readiness check but the codec is being destroyed.
 *   5. Write again -> on a buggy tree write_frame() dereferences the now-NULL
 *      frame->codec->mutex and crashes; on a fixed tree it must bail safely.
 *
 * Expected:
 *   - BUGGY tree  : SIGSEGV inside switch_core_session_write_frame().
 *   - FIXED tree  : write_frame() returns without crashing; test passes.
 */

#include <switch.h>
#include <test/switch_test.h>

/* Originate one answered "null" session. Returns it rwlocked (caller unlocks). */
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

FST_CORE_BEGIN("./conf")
{
	FST_SUITE_BEGIN(write_frame_foreign_codec)
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

		FST_TEST_BEGIN(test_write_frame_codec_mutex_torndown)
		{
			switch_core_session_t *session_w = NULL; /* written to, stays alive */
			switch_channel_t *channel_w = NULL;
			switch_codec_t foreign_codec = { 0 };    /* stands in for peer's codec */
			switch_codec_implementation_t write_impl = { 0 };
			switch_mutex_t *saved_mutex = NULL;
			switch_frame_t write_frame = { 0 };
			switch_status_t status;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "========== WRITE_FRAME FOREIGN-CODEC TEARDOWN TEST ==========\n");

			/* 1. Session we write to - keep rwlocked so it stays alive/writable. */
			session_w = originate_null_session();
			fst_requires(session_w);
			channel_w = switch_core_session_get_channel(session_w);
			fst_requires(switch_core_session_get_write_impl(session_w, &write_impl) == SWITCH_STATUS_SUCCESS);

			/* 2. A codec NOT owned by session_w, matching its write impl, exactly
			 *    like the foreign codec setup_ringback() hands to write_frame(). */
			fst_requires(switch_core_codec_init(&foreign_codec,
												"L16", NULL, NULL,
												write_impl.actual_samples_per_second,
												write_impl.microseconds_per_packet / 1000,
												write_impl.number_of_channels,
												SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
												NULL, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_codec_ready(&foreign_codec));

			switch_zmalloc(write_frame.data, SWITCH_RECOMMENDED_BUFFER_SIZE);
			write_frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;
			write_frame.codec = &foreign_codec;
			write_frame.datalen = foreign_codec.implementation->decoded_bytes_per_packet;
			write_frame.samples = write_frame.datalen / 2;
			write_frame.payload = (switch_payload_t) foreign_codec.implementation->ianacode;
			write_frame.rate = foreign_codec.implementation->actual_samples_per_second;
			/* zeroed data -> SLN silence; no SFF_CNG/SFF_PROXY_PACKET so write_frame()
			 * takes the path that locks frame->codec->mutex. */

			/* 3. Baseline: prove the frame->codec->mutex lock path is exercised. */
			status = switch_core_session_write_frame(session_w, &write_frame, SWITCH_IO_FLAG_NONE, 0);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] baseline write (codec intact) returned %d - path exercised\n", status);

			/* 4. Simulate the foreign codec being torn down underneath the writer:
			 *    switch_core_codec_destroy() memsets the codec, so ->mutex becomes
			 *    NULL. Clear it directly to freeze that state deterministically. */
			saved_mutex = foreign_codec.mutex;
			foreign_codec.mutex = NULL;
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] cleared frame->codec->mutex (simulated teardown); writing again...\n");

			/* 5. The crash: write_frame() locks/unlocks frame->codec->mutex (NULL).
			 *    Buggy tree -> SIGSEGV here. Fixed tree -> safe bail. */
			status = switch_core_session_write_frame(session_w, &write_frame, SWITCH_IO_FLAG_NONE, 0);

			/* Only reached on a fixed tree. */
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] write_frame survived torn-down codec (status=%d) - FIXED\n", status);
			fst_check(status != SWITCH_STATUS_SUCCESS); /* it must NOT have written */

			/* Cleanup. */
			foreign_codec.mutex = saved_mutex;
			switch_core_codec_destroy(&foreign_codec);
			switch_safe_free(write_frame.data);
			switch_channel_hangup(channel_w, SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session_w);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "========== TEST COMPLETED ==========\n\n");
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
