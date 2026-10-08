/*
 * switch_core_session_read_frame() and short decoded frames.
 *
 * When a decoded frame is shorter than the read codec's frame, read_frame()
 * keeps it in raw_read_buffer and reads again until a full frame exists
 * (switch_core_io.c, "Engaging Read Buffer"). A peer sending tiny payloads
 * (e.g. 1-byte G722 every 100 ms) makes one read_frame() call take the time
 * of the many packets a full frame needs, so a caller that has to stay
 * responsive between reads (originate waiting for an answer) is stuck.
 *
 * SWITCH_IO_FLAG_BOUNDED_READ asks read_frame() to give up after one frame
 * interval: it returns a CNG frame and keeps the buffered audio for the next
 * read. Without the flag the behaviour is unchanged.
 *
 * A read_frame event hook replaces the frames of a null endpoint session (L16)
 * with frames of a real PCMU or G722 codec, so the core decodes, buffers and
 * re-reads exactly as for RTP. The hook blocks for the packet interval, as an
 * RTP read waiting for the next packet does.
 */

#include <switch.h>
#include <test/switch_test.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Wall clock seen by the core, shifted back by g_clock_back_us once armed. With
 * monotonic timing off, switch_mono_micro_time_now() reads CLOCK_REALTIME. */
static volatile int64_t g_clock_back_us = 0;

int clock_gettime(clockid_t clk_id, struct timespec *tp)
{
	int r = (int) syscall(SYS_clock_gettime, clk_id, tp);

	if (!r && clk_id == CLOCK_REALTIME && g_clock_back_us) {
		int64_t ns = (int64_t) tp->tv_sec * 1000000000 + tp->tv_nsec - g_clock_back_us * 1000;

		tp->tv_sec = ns / 1000000000;
		tp->tv_nsec = ns % 1000000000;
	}

	return r;
}

/* Watchdog: past this many endpoint reads the hook hangs the channel up, so a
 * read that never returns ends the test instead of hanging it. */
#define FEED_LIMIT 400

static switch_codec_t g_feed_codec;
static switch_frame_t g_feed;
static unsigned char g_feed_buf[SWITCH_RECOMMENDED_BUFFER_SIZE];
static uint32_t g_feed_len = 0;
static int g_feed_numbered = 0;
static uint32_t g_feed_interval_us = 0;
/* Endpoint read number (1-based) that delivers a full 20 ms PCMU frame instead; 0 = never. */
static int g_feed_full_at = 0;
/* Endpoint read number (1-based) at which the wall clock steps back; 0 = never. */
static int g_feed_clock_back_at = 0;
static volatile int g_packets = 0;

static switch_status_t feed_hook(switch_core_session_t *session, switch_frame_t **frame, switch_io_flag_t flags, int stream_id)
{
	switch_yield(g_feed_interval_us);

	if (++g_packets > FEED_LIMIT) {
		switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_RECOVERY_ON_TIMER_EXPIRE);
		return SWITCH_STATUS_FALSE;
	}

	if (g_packets == g_feed_clock_back_at) {
		g_clock_back_us = 200000;
	}

	if (g_feed_numbered) {
		/* PCMU byte per packet: 0x80 | packet number, so order and loss show in the PCM. */
		memset(g_feed_buf, 0x80 | ((g_packets - 1) & 0x7f), g_packets == g_feed_full_at ? 160 : g_feed_len);
	}

	memset(&g_feed, 0, sizeof(g_feed));
	g_feed.codec = &g_feed_codec;
	g_feed.data = g_feed_buf;
	g_feed.datalen = g_packets == g_feed_full_at ? 160 : g_feed_len;
	g_feed.buflen = sizeof(g_feed_buf);
	g_feed.samples = g_feed.datalen;
	*frame = &g_feed;

	return SWITCH_STATUS_SUCCESS;
}

static switch_core_session_t *new_session(const char *rate)
{
	switch_core_session_t *session = NULL;
	switch_call_cause_t cause = SWITCH_CAUSE_NONE;
	switch_frame_t *frame = NULL;
	switch_status_t status;

	status = switch_ivr_originate(NULL, &session, &cause, "null/+15553334444", 0, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	if (status != SWITCH_STATUS_SUCCESS || !session) {
		return NULL;
	}

	if (rate) {
		/* null_channel_read_frame() switches its L16 codec on the next read. */
		switch_channel_set_variable(switch_core_session_get_channel(session), "null_switch_rate", rate);
		switch_core_session_read_frame(session, &frame, SWITCH_IO_FLAG_NONE, 0);
	}

	return session;
}

static void start_feed(switch_core_session_t *session, uint32_t len, int numbered, uint32_t interval_ms)
{
	g_feed_len = len;
	g_feed_interval_us = interval_ms * 1000;
	g_feed_full_at = 0;
	g_feed_clock_back_at = 0;
	g_clock_back_us = 0;
	g_feed_numbered = numbered;
	memset(g_feed_buf, 0xff, sizeof(g_feed_buf));
	g_packets = 0;
	switch_core_event_hook_add_read_frame(session, feed_hook);
}

static void end_session(switch_core_session_t *session)
{
	switch_core_event_hook_remove_read_frame(session, feed_hook);
	switch_channel_hangup(switch_core_session_get_channel(session), SWITCH_CAUSE_NORMAL_CLEARING);
	switch_core_session_rwunlock(session);
	switch_core_codec_destroy(&g_feed_codec);
}

static uint32_t full_frame_bytes(switch_core_session_t *session)
{
	switch_codec_implementation_t read_impl = { 0 };

	switch_core_session_get_read_impl(session, &read_impl);
	return read_impl.decoded_bytes_per_packet;
}

/* The linear sample a numbered PCMU packet decodes to. */
static int16_t numbered_sample(int packet)
{
	unsigned char in = 0x80 | (packet & 0x7f);
	int16_t out[4] = { 0 };
	uint32_t out_len = sizeof(out), rate = 8000, flag = 0;

	switch_core_codec_decode(&g_feed_codec, NULL, &in, 1, 8000, out, &out_len, &rate, &flag);
	return out[0];
}

/* Bounded reads until a full (non-CNG) frame comes back; returns it, or NULL. */
static switch_frame_t *bounded_read_until_audio(switch_core_session_t *session, int *cng_frames)
{
	switch_frame_t *frame = NULL;
	switch_status_t status;

	*cng_frames = 0;

	while (g_packets < FEED_LIMIT) {
		int before = g_packets;

		status = switch_core_session_read_frame(session, &frame, SWITCH_IO_FLAG_BOUNDED_READ, 0);
		if (status != SWITCH_STATUS_SUCCESS || !frame) {
			return NULL;
		}

		/* Bounded: a read ends one frame interval after the first short frame,
		 * i.e. after a few endpoint reads (timer jitter can add one). */
		if (g_packets - before > 4) {
			return NULL;
		}

		if (!switch_test_flag(frame, SFF_CNG)) {
			return frame;
		}

		(*cng_frames)++;
	}

	return NULL;
}

FST_CORE_BEGIN("./conf")
{
	FST_SUITE_BEGIN(read_frame_accumulation)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_loopback");
			memset(&g_feed_codec, 0, sizeof(g_feed_codec));
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		/* Without the flag one read takes every tiny packet a full frame needs. */
		FST_TEST_BEGIN(unbounded_read_waits_for_a_full_frame)
		{
			switch_core_session_t *session = new_session(NULL);
			switch_frame_t *frame = NULL;
			switch_status_t status;
			uint32_t full;

			fst_requires(session);
			fst_requires(switch_core_codec_init(&g_feed_codec, "PCMU", NULL, NULL, 8000, 20, 1,
												SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL,
												switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS);
			full = full_frame_bytes(session);
			fst_requires(full == 320);

			start_feed(session, 1, 0, 2);
			status = switch_core_session_read_frame(session, &frame, SWITCH_IO_FLAG_NONE, 0);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_requires(frame);
			fst_check(!switch_test_flag(frame, SFF_CNG));
			fst_check(frame->datalen == full);
			fst_check(g_packets == (int) (full / 2));

			end_session(session);
		}
		FST_TEST_END()

		/* 1-byte PCMU: bounded reads return CNG at once, and no audio is lost. */
		FST_TEST_BEGIN(bounded_read_returns_cng_and_keeps_tiny_pcmu)
		{
			switch_core_session_t *session = new_session(NULL);
			switch_frame_t *frame = NULL;
			int cng = 0;
			uint32_t full;

			fst_requires(session);
			fst_requires(switch_core_codec_init(&g_feed_codec, "PCMU", NULL, NULL, 8000, 20, 1,
												SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL,
												switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS);
			full = full_frame_bytes(session);
			fst_requires(full == 320);

			start_feed(session, 1, 0, 25);
			frame = bounded_read_until_audio(session, &cng);
			fst_requires(frame);
			fst_check(frame->datalen == full);
			/* Reads came back early with CNG, and no packet was dropped. */
			fst_check(g_packets == (int) (full / 2));
			fst_check(cng > 0);

			end_session(session);
		}
		FST_TEST_END()

		/* The production shape: 1-byte G722 decoded to 16 kHz L16 (640-byte frames). */
		FST_TEST_BEGIN(bounded_read_returns_cng_and_keeps_tiny_g722)
		{
			switch_core_session_t *session = NULL;
			switch_frame_t *frame = NULL;
			int cng = 0;
			uint32_t full;

			fst_requires_module("mod_spandsp");
			session = new_session("16000");
			fst_requires(session);
			fst_requires(switch_core_codec_init(&g_feed_codec, "G722", NULL, NULL, 8000, 20, 1,
												SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL,
												switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS);
			full = full_frame_bytes(session);
			fst_requires(full == 640);

			start_feed(session, 1, 0, 25);
			frame = bounded_read_until_audio(session, &cng);
			fst_requires(frame);
			fst_check(frame->datalen == full);
			/* One G722 byte decodes to two 16-bit samples. */
			fst_check(g_packets == (int) (full / 4));
			fst_check(cng > 0);

			end_session(session);
		}
		FST_TEST_END()

		/* Full 20 ms frames: bounded reads behave exactly like plain reads. */
		FST_TEST_BEGIN(bounded_read_full_frames_unchanged)
		{
			switch_core_session_t *session = new_session(NULL);
			switch_frame_t *frame = NULL;
			switch_status_t status;
			int i;

			fst_requires(session);
			fst_requires(switch_core_codec_init(&g_feed_codec, "PCMU", NULL, NULL, 8000, 20, 1,
												SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL,
												switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS);

			start_feed(session, 160, 0, 20);
			for (i = 0; i < 5; i++) {
				status = switch_core_session_read_frame(session, &frame, SWITCH_IO_FLAG_BOUNDED_READ, 0);
				fst_check(status == SWITCH_STATUS_SUCCESS);
				fst_requires(frame);
				fst_check(!switch_test_flag(frame, SFF_CNG));
				fst_check(frame->datalen == 320);
			}
			fst_check(g_packets == 5);

			end_session(session);
		}
		FST_TEST_END()

		/* 10 ms frames into a 20 ms read: every sample arrives once and in order. */
		FST_TEST_BEGIN(bounded_read_keeps_order_of_split_frames)
		{
			switch_core_session_t *session = new_session(NULL);
			switch_frame_t *frame = NULL;
			int16_t expect[8];
			int cng = 0, i, k;

			fst_requires(session);
			fst_requires(switch_core_codec_init(&g_feed_codec, "PCMU", NULL, NULL, 8000, 20, 1,
												SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL,
												switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS);

			for (k = 0; k < 8; k++) {
				unsigned char in = 0x80 | k;
				int16_t out[4] = { 0 };
				uint32_t out_len = sizeof(out), rate = 8000, flag = 0;

				fst_requires(switch_core_codec_decode(&g_feed_codec, NULL, &in, 1, 8000, out, &out_len, &rate, &flag) == SWITCH_STATUS_SUCCESS);
				expect[k] = out[0];
			}

			start_feed(session, 80, 1, 5);
			for (i = 0; i < 4; i++) {
				int16_t *pcm;

				frame = bounded_read_until_audio(session, &cng);
				fst_requires(frame);
				fst_requires(frame->datalen == 320);
				/* Both halves arrive inside one frame interval: no CNG in between. */
				fst_check(cng == 0);
				pcm = (int16_t *) frame->data;
				/* Frame i = packets 2i and 2i+1, 80 samples each. */
				fst_check(pcm[0] == expect[2 * i]);
				fst_check(pcm[79] == expect[2 * i]);
				fst_check(pcm[80] == expect[2 * i + 1]);
				fst_check(pcm[159] == expect[2 * i + 1]);
			}
			fst_check(g_packets == 8);

			end_session(session);
		}
		FST_TEST_END()

		/* A full packet after a bounded yield passes straight through (the existing
		 * full-frame path); the buffered samples stay queued and come out later, in
		 * order, ahead of the short packets that follow. */
		FST_TEST_BEGIN(bounded_read_keeps_queued_samples_across_a_full_frame)
		{
			switch_core_session_t *session = new_session(NULL);
			switch_frame_t *frame = NULL;
			int16_t *pcm;
			int cng = 0, full_at;

			fst_requires(session);
			fst_requires(switch_core_codec_init(&g_feed_codec, "PCMU", NULL, NULL, 8000, 20, 1,
												SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL,
												switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS);

			start_feed(session, 1, 1, 25);
			/* Short packets until at least one bounded read has yielded CNG. */
			while (cng == 0 && g_packets < 20) {
				fst_requires(switch_core_session_read_frame(session, &frame, SWITCH_IO_FLAG_BOUNDED_READ, 0) == SWITCH_STATUS_SUCCESS);
				fst_requires(frame);
				if (switch_test_flag(frame, SFF_CNG)) {
					cng++;
				}
			}
			fst_requires(cng > 0);
			full_at = g_packets + 1;
			g_feed_full_at = full_at;

			/* The next audio frame is the full packet itself. */
			frame = bounded_read_until_audio(session, &cng);
			fst_requires(frame);
			fst_check(g_packets == full_at);
			pcm = (int16_t *) frame->data;
			fst_check(pcm[0] == numbered_sample(full_at - 1));
			fst_check(pcm[159] == numbered_sample(full_at - 1));

			/* Then the queued short packets, oldest first, then the ones after the full packet. */
			frame = bounded_read_until_audio(session, &cng);
			fst_requires(frame);
			fst_requires(frame->datalen == 320);
			pcm = (int16_t *) frame->data;
			fst_check(pcm[0] == numbered_sample(0));
			fst_check(pcm[full_at - 2] == numbered_sample(full_at - 2));
			fst_check(pcm[full_at - 1] == numbered_sample(full_at));
			fst_check(pcm[full_at] == numbered_sample(full_at + 1));

			end_session(session);
		}
		FST_TEST_END()

		/* A session reset drops the partial buffer: nothing from before it leaks into
		 * the next frame. */
		FST_TEST_BEGIN(bounded_read_partial_buffer_dropped_on_reset)
		{
			switch_core_session_t *session = new_session(NULL);
			switch_frame_t *frame = NULL;
			int16_t *pcm;
			int cng = 0, first_after;

			fst_requires(session);
			fst_requires(switch_core_codec_init(&g_feed_codec, "PCMU", NULL, NULL, 8000, 20, 1,
												SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL,
												switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS);

			start_feed(session, 1, 1, 25);
			while (cng == 0 && g_packets < 20) {
				fst_requires(switch_core_session_read_frame(session, &frame, SWITCH_IO_FLAG_BOUNDED_READ, 0) == SWITCH_STATUS_SUCCESS);
				fst_requires(frame);
				if (switch_test_flag(frame, SFF_CNG)) {
					cng++;
				}
			}
			fst_requires(cng > 0);

			switch_core_session_reset(session, SWITCH_FALSE, SWITCH_TRUE);
			first_after = g_packets;

			frame = bounded_read_until_audio(session, &cng);
			fst_requires(frame);
			fst_requires(frame->datalen == 320);
			pcm = (int16_t *) frame->data;
			fst_check(pcm[0] == numbered_sample(first_after));
			fst_check(g_packets == first_after + 160);

			end_session(session);
		}
		FST_TEST_END()

		/* Monotonic timing off and the clock stepping back after the first short frame:
		 * the read still comes back at once instead of waiting for the clock. */
		FST_TEST_BEGIN(bounded_read_returns_when_the_clock_steps_back)
		{
			switch_core_session_t *session = new_session(NULL);
			switch_frame_t *frame = NULL;
			switch_status_t status;

			fst_requires(session);
			fst_requires(switch_core_codec_init(&g_feed_codec, "PCMU", NULL, NULL, 8000, 20, 1,
												SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL,
												switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS);

			start_feed(session, 1, 0, 25);
			g_feed_clock_back_at = 2;
			switch_time_set_monotonic(SWITCH_FALSE);
			status = switch_core_session_read_frame(session, &frame, SWITCH_IO_FLAG_BOUNDED_READ, 0);
			/* Drop the shift before re-enabling: that resyncs the core clock offset. */
			g_clock_back_us = 0;
			switch_time_set_monotonic(SWITCH_TRUE);

			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_requires(frame);
			fst_check(switch_test_flag(frame, SFF_CNG));
			/* First short frame starts the budget, the second sees the clock behind it. */
			fst_check(g_packets == 2);

			end_session(session);
		}
		FST_TEST_END()

		/* Frames that decode to nothing: bounded reads still come back. */
		FST_TEST_BEGIN(bounded_read_returns_on_empty_frames)
		{
			switch_core_session_t *session = new_session(NULL);
			switch_frame_t *frame = NULL;
			switch_status_t status;
			int i;

			fst_requires(session);
			fst_requires(switch_core_codec_init(&g_feed_codec, "PCMU", NULL, NULL, 8000, 20, 1,
												SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL,
												switch_core_session_get_pool(session)) == SWITCH_STATUS_SUCCESS);

			start_feed(session, 0, 0, 25);
			for (i = 0; i < 5; i++) {
				status = switch_core_session_read_frame(session, &frame, SWITCH_IO_FLAG_BOUNDED_READ, 0);
				fst_check(status == SWITCH_STATUS_SUCCESS);
				fst_requires(frame);
				fst_check(switch_test_flag(frame, SFF_CNG));
			}
			fst_check(g_packets <= 20);

			end_session(session);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
