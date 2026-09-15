/*
 * Regression test: switch_core_session_read_frame() unlocking a destroyed read codec.
 *
 * Production stack (1.10.12-telv112.0.b466):
 *   __GI___pthread_mutex_unlock_usercnt()   pthread_mutex_unlock.c:51
 *   fspr_thread_mutex_unlock()              libfreeswitch.so.1
 *   switch_mutex_unlock()                   switch_apr.c:322
 *   switch_core_session_read_frame()        switch_core_io.c:1088   <-- crash
 *   switch_ivr_parse_event()                switch_ivr.c:563
 *   switch_ivr_parse_next_event()           switch_ivr.c:828
 *   switch_ivr_parse_all_events()           switch_ivr.c:966
 *   conference_loop_output()                conference_loop.c:1645
 *   conference_function()                   mod_conference.c:2542
 *
 * read_frame() locks session->read_codec->mutex at switch_core_io.c:137/221/251 and
 * unlocks by re-evaluating that expression at :1087. switch_core_codec_destroy()
 * memsets the codec (switch_core_codec.c:963) after releasing codec->mutex and
 * without holding codec_read_mutex, so the re-read can be NULL.
 *
 * Two mod_conference threads, one session, both on &member.read_codec
 * (mod_conference.c:1955, installed at :2476):
 *
 *   conference_function()   -> switch_ivr_parse_all_events() -> read_frame()
 *   conference_loop_input() -> conference_member_setup_media()   conference_loop.c:915
 *                              -> switch_core_codec_destroy(&member->read_codec) + memset
 *
 * setup_media() runs on member->reset_media, i.e. CF_CONFERENCE_RESET_MEDIA
 * (switch_core_media.c:4202/:16318). member->read_mutex is taken only by
 * conference_loop_input(), and switch_core_codec_destroy() does not take
 * codec_read_mutex.
 *
 * Distinct from the TEL-6515 fix (test_inuse_race.c), which serialises a read_codec
 * pointer swap behind codec_read_mutex: here the codec is destroyed in place and the
 * pointer never changes.
 *
 * The production interleave - the destroying thread completing its memset between
 * read_frame() taking the codec mutex and re-reading it to unlock - is a few
 * instructions wide, so this test freezes the end state instead. A SMBF_READ_PING
 * media bug callback runs from the done: block of read_frame()
 * (switch_core_io.c:~1048), inside the region holding the codec mutex and ~20 lines
 * before the unlock, and memsets the session read codec there. A baseline read first
 * asserts the callback is reached.
 *
 * Unfixed: SIGSEGV in __pthread_mutex_unlock_usercnt.
 * Fixed:   read_frame() unlocks the pointer it locked and returns.
 */

#include <switch.h>
#include <test/switch_test.h>

static switch_core_session_t *g_session = NULL;

/* Proves the injection point was reached. */
static volatile int g_ping_count = 0;

/* Arm/disarm the simulated teardown, and the saved codec used to restore it. */
static volatile int g_tear_down_codec = 0;
static switch_codec_t *g_torn_codec = NULL;
static switch_codec_t g_saved_codec;

/*
 * Stands in for the conference input thread. Runs at switch_core_io.c:~1048, while
 * the caller holds session->read_codec->mutex and session->codec_read_mutex.
 */
static switch_bool_t teardown_bug_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	if (type != SWITCH_ABC_TYPE_READ_PING) {
		return SWITCH_TRUE;
	}

	g_ping_count++;

	if (g_tear_down_codec) {
		switch_codec_t *codec = switch_core_session_get_read_codec(g_session);

		g_tear_down_codec = 0;

		if (codec) {
			/* The state switch_core_codec_destroy() leaves behind: memset(codec, 0,
			 * sizeof(*codec)) at switch_core_codec.c:963, after unlocking codec->mutex.
			 * Saved so the session can still be torn down if the tree is fixed. */
			g_saved_codec = *codec;
			g_torn_codec = codec;
			memset(codec, 0, sizeof(*codec));

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] read codec %p zeroed mid-read_frame\n", (void *) codec);
		}
	}

	return SWITCH_TRUE;
}

FST_CORE_BEGIN("./conf")
{
	FST_SUITE_BEGIN(read_frame_codec_teardown)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_loopback");
			g_session = NULL;
			g_ping_count = 0;
			g_tear_down_codec = 0;
			g_torn_codec = NULL;
			memset(&g_saved_codec, 0, sizeof(g_saved_codec));
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		FST_TEST_BEGIN(test_read_frame_codec_destroyed_under_reader)
		{
			switch_core_session_t *session = NULL;
			switch_channel_t *channel = NULL;
			switch_call_cause_t cause = SWITCH_CAUSE_NONE;
			switch_media_bug_t *bug = NULL;
			switch_frame_t *frame = NULL;
			switch_status_t status;
			int i;

			/* 1. A session whose read codec is owned by the endpoint's private data,
			 *    like mod_conference's member->read_codec. */
			status = switch_ivr_originate(NULL, &session, &cause,
										  "null/+15553334444",
										  0, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_requires(session);

			g_session = session;
			channel = switch_core_session_get_channel(session);
			fst_requires(switch_core_session_get_read_codec(session));

			/* 2. READ_PING bug: callback runs from the done: block of read_frame(),
			 *    inside the codec-mutex region. */
			status = switch_core_media_bug_add(session, "read_codec_teardown", NULL,
											   teardown_bug_callback, NULL, 0,
											   SMBF_READ_PING | SMBF_NO_PAUSE,
											   &bug);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_requires(bug);

			/* 3. Baseline: the injection point must be reached, or step 5 proves nothing. */
			for (i = 0; i < 10 && g_ping_count == 0; i++) {
				status = switch_core_session_read_frame(session, &frame, SWITCH_IO_FLAG_NONE, 0);
				fst_check(SWITCH_READ_ACCEPTABLE(status));
			}
			fst_requires(g_ping_count > 0);

			/* 4. Arm the teardown. */
			g_tear_down_codec = 1;

			/* 5. read_frame() exits through even_more_done: and unlocks the codec mutex
			 *    of a codec zeroed underneath it. Unfixed: SIGSEGV here. */
			status = switch_core_session_read_frame(session, &frame, SWITCH_IO_FLAG_NONE, 0);

			/* Reached only on a fixed tree. Not crashing is the assertion; the checks
			 * below confirm the injection happened and a usable frame came back.
			 * SWITCH_STATUS_SUCCESS is legitimate: the frame was already read when the
			 * codec was torn down, and even_more_done: substitutes the dummy CNG frame. */
			fst_requires(g_torn_codec);
			fst_check(frame != NULL);

			/* Restore the codec so the session can be hung up normally. */
			*g_torn_codec = g_saved_codec;
			g_torn_codec = NULL;

			switch_core_media_bug_remove(session, &bug);
			switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session);
			g_session = NULL;
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
