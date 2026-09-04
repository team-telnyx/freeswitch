/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * test_member_media_reset.c -- conference_member_setup_media() vs the session read path
 *
 * setup_media() destroys and re-initialises member->read_codec in place. It runs on
 * the member input thread (conference_loop.c:915, member->reset_media /
 * CF_CONFERENCE_RESET_MEDIA) while conference_function() can be in
 * switch_core_session_read_frame() on the same session holding that codec's mutex.
 * switch_core_codec_destroy() zeroes the codec, mutex pointer included, without
 * taking codec_read_mutex, so setup_media() holds codec_read_mutex across the
 * destroy/re-init.
 *
 * Covered here: setup_media() still succeeds on a reset (only the second and later
 * calls take the destroy path), releases codec_read_mutex on every exit path, and
 * leaves the session read path working. Not covered: the race itself, which is
 * timing-dependent; the core side of it is in
 * tests/unit/test_read_frame_codec_teardown.c.
 *
 * The reader runs on its own thread and the check is the frame, not a hang.
 * codec_read_mutex is SWITCH_MUTEX_NESTED (switch_core_session.c:2674), so a leaking
 * thread re-acquires it, and read_frame() only trylocks it (switch_core_io.c:93),
 * returning runtime.dummy_cng_frame on failure. A missing unlock therefore shows up
 * as SFF_CNG. The null endpoint's silence is generated SLN and carries no SFF_CNG.
 *
 * setup_media() is linked from libmodconference rather than #include-ing
 * conference_member.c the way test_member.c and test_image.c do: that include is not
 * a tracked build dependency, so those objects go stale when conference_member.c
 * changes.
 */
#include <switch.h>
#include <stdlib.h>
#include <mod_conference.h>

#include <test/switch_test.h>

static switch_core_session_t *g_session = NULL;
static volatile int g_read_done = 0;
static volatile switch_status_t g_read_status = SWITCH_STATUS_FALSE;
static volatile int g_read_was_cng = 0;

/* Reads one frame and records whether it was real audio or the CNG placeholder
 * read_frame() returns when it cannot take codec_read_mutex. */
static void *SWITCH_THREAD_FUNC reader_thread(switch_thread_t *thread, void *obj)
{
	switch_frame_t *frame = NULL;

	g_read_status = switch_core_session_read_frame(g_session, &frame, SWITCH_IO_FLAG_NONE, 0);
	g_read_was_cng = (!frame || switch_test_flag(frame, SFF_CNG));
	g_read_done = 1;

	return NULL;
}

/* True if a read from another thread completed within ~2s. */
static int read_frame_off_thread(switch_memory_pool_t *pool)
{
	switch_threadattr_t *thd_attr = NULL;
	switch_thread_t *thread = NULL;
	int i;

	g_read_done = 0;
	g_read_status = SWITCH_STATUS_FALSE;
	g_read_was_cng = 0;

	switch_threadattr_create(&thd_attr, pool);
	switch_threadattr_detach_set(thd_attr, 1);
	switch_thread_create(&thread, thd_attr, reader_thread, NULL, pool);

	for (i = 0; i < 200 && !g_read_done; i++) {
		switch_yield(10000);
	}

	return g_read_done;
}

FST_CORE_BEGIN("./conf")
{
	FST_SUITE_BEGIN(conference_member_media_reset)
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

		FST_TEST_BEGIN(setup_media_survives_a_reset)
		{
			conference_member_t smember = { 0 };
			conference_member_t *member = &smember;
			conference_obj_t conference = { 0 };
			switch_core_session_t *session = NULL;
			switch_channel_t *channel = NULL;
			switch_call_cause_t cause = SWITCH_CAUSE_NONE;
			switch_codec_implementation_t read_impl = { 0 };
			switch_status_t status;
			int rc;

			status = switch_ivr_originate(NULL, &session, &cause,
										  "null/+15553334444",
										  0, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_requires(session);
			channel = switch_core_session_get_channel(session);
			fst_requires(switch_core_session_get_read_impl(session, &read_impl) == SWITCH_STATUS_SUCCESS);

			member->session = session;
			member->channel = channel;
			member->pool = switch_core_session_get_pool(session);
			switch_mutex_init(&member->audio_out_mutex, SWITCH_MUTEX_NESTED, member->pool);

			conference.rate = read_impl.actual_samples_per_second;
			conference.channels = read_impl.number_of_channels;
			conference.pool = member->pool;

			/* 1. Join-time setup: nothing to destroy yet. */
			rc = conference_member_setup_media(member, &conference);
			fst_requires(rc == 0);
			fst_check(switch_core_codec_ready(&member->read_codec));

			/* 2. Install the member codec as conference_function() does
			 *    (mod_conference.c:2476), so the read path runs through the codec
			 *    setup_media() rebuilds. */
			fst_requires(switch_core_session_set_read_codec(session, &member->read_codec) == SWITCH_STATUS_SUCCESS);

			/* 3. Baseline read. A real frame here is what makes step 5 meaningful. */
			g_session = session;
			fst_requires(read_frame_off_thread(member->pool));
			fst_requires(SWITCH_READ_ACCEPTABLE(g_read_status));
			fst_requires(!g_read_was_cng);

			/* 4. The reset path: destroy + re-init under codec_read_mutex. */
			rc = conference_member_setup_media(member, &conference);
			fst_requires(rc == 0);
			fst_check(switch_core_codec_ready(&member->read_codec));

			/* 5. Still real audio. A leaked codec_read_mutex gives the CNG
			 *    placeholder; a destroyed-but-published codec fails the status. */
			fst_check(read_frame_off_thread(member->pool));
			fst_check(SWITCH_READ_ACCEPTABLE(g_read_status));
			fst_check(!g_read_was_cng);

			/* Cleanup: put the session back on its own codec first. */
			switch_core_session_set_read_codec(session, NULL);
			switch_core_codec_destroy(&member->read_codec);
			switch_core_codec_destroy(&member->write_codec);
			if (member->read_resampler) {
				switch_resample_destroy(&member->read_resampler);
			}
			if (member->audio_buffer) {
				switch_buffer_destroy(&member->audio_buffer);
			}
			if (member->mux_buffer) {
				switch_buffer_destroy(&member->mux_buffer);
			}
			if (member->resample_buffer) {
				switch_buffer_destroy(&member->resample_buffer);
			}

			switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
