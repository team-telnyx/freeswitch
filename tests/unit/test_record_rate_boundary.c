/*
 * Regression test for the recording-thread buffer ceiling's stream accounting.
 *
 * The ceiling discards audio from the front of the recording thread's queue
 * when a stalled writer lets it grow past record_buffer_max_ms.  Queued input
 * rate changes are recorded as ABSOLUTE positions in that byte stream
 * (bytes_out + inuse), and the recording thread only advances bytes_out for
 * bytes it actually reads.  A discard removes bytes the thread never reads, so
 * unless the discard is counted as drained the stream position collapses and
 * every queued rate boundary is applied that far into the wrong audio -- the
 * corruption the boundaries exist to prevent.
 *
 * The invariant: the stream position counts everything ever enqueued, so it can
 * never fall below the number of bytes discarded from it.
 *
 *   counted   : between two discards the position advances by what was
 *               discarded, because a saturated queue discards all it takes in
 *   uncounted : the discard cancels the enqueue and the position does not move
 *
 * The test stalls the recorder on a FIFO it holds open but does not drain, lets
 * the ceiling discard for long enough that the dropped total dwarfs the queue,
 * and checks the invariant at the last discard.  It fails loudly if no discard
 * happened at all, so a stall that stops working cannot make it pass vacuously.
 */
#include <switch.h>
#include <test/switch_test.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

static struct {
	switch_mutex_t *mutex;
	int drops;
	switch_size_t last_dropped, prev_dropped;
	switch_size_t last_pos, prev_pos;
} g_obs;

static switch_status_t obs_logger(const switch_log_node_t *node, switch_log_level_t level)
{
	const char *p;
	unsigned long long dropped = 0, pos = 0;

	if (!node || !node->data) return SWITCH_STATUS_SUCCESS;

	if ((p = strstr(node->data, "discarding oldest audio ("))) {
		if (sscanf(p, "discarding oldest audio (%llu bytes dropped so far, stream position %llu)",
				   &dropped, &pos) == 2) {
			switch_mutex_lock(g_obs.mutex);
			g_obs.drops++;
			g_obs.prev_dropped = g_obs.last_dropped;
			g_obs.prev_pos = g_obs.last_pos;
			g_obs.last_dropped = (switch_size_t) dropped;
			g_obs.last_pos = (switch_size_t) pos;
			switch_mutex_unlock(g_obs.mutex);
		}
	}

	return SWITCH_STATUS_SUCCESS;
}

/* Read whatever the recorder has managed to push, so the writer unblocks and the
 * close can complete.  Bounded: a wedged writer must not hang the suite. */
static void drain_fifo(int fd, int max_ms)
{
	char buf[8192];
	int waited = 0;

	while (waited < max_ms) {
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n > 0) continue;
		switch_yield(20000);
		waited += 20;
	}
}

FST_CORE_BEGIN("./conf_async")
{
	FST_SUITE_BEGIN(record_stream_accounting)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_loopback");
			fst_requires_module("mod_sndfile");
			fst_requires_module("mod_dptools");
			memset(&g_obs, 0, sizeof(g_obs));
			switch_mutex_init(&g_obs.mutex, SWITCH_MUTEX_NESTED, fst_pool);
			switch_log_bind_logger(obs_logger, SWITCH_LOG_DEBUG, SWITCH_FALSE);
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
			switch_log_unbind_logger(obs_logger);
		}
		FST_TEARDOWN_END()

		FST_TEST_BEGIN(ceiling_discard_keeps_the_stream_position)
		{
			switch_core_session_t *session = NULL;
			switch_channel_t *channel = NULL;
			switch_call_cause_t cause = SWITCH_CAUSE_NONE;
			char fifo[256];
			int rfd = -1, i;
			int drops;
			switch_size_t d_dropped, d_pos;

			switch_snprintf(fifo, sizeof(fifo), "%s%srec-accounting-%d.raw",
							SWITCH_GLOBAL_dirs.temp_dir, SWITCH_PATH_SEPARATOR, (int) getpid());
			unlink(fifo);
			fst_requires(mkfifo(fifo, 0600) == 0);

			/* Hold the read end open so the recorder's open() succeeds, but never
			 * drain it while recording: once the pipe fills every write blocks and
			 * the queue climbs past the ceiling. */
			rfd = open(fifo, O_RDONLY | O_NONBLOCK);
			fst_requires(rfd >= 0);

			switch_core_set_variable("record_buffer_max_ms", "100");
			switch_core_set_variable("record_close_timeout_ms", "1000");

			fst_requires(switch_ivr_originate(NULL, &session, &cause,
											  "loopback/app=playback:silence_stream://60000", 5,
											  NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL)
						 == SWITCH_STATUS_SUCCESS);
			fst_requires(session);
			channel = switch_core_session_get_channel(session);
			switch_channel_answer(channel);

			fst_requires(switch_ivr_record_session_event(session, fifo, 0, NULL, NULL) == SWITCH_STATUS_SUCCESS);

			/* switch_ivr_sleep reads frames; switch_yield does not, and the record
			 * bug only fires from the session's read loop.  Pump long enough that
			 * the pipe fills, the ceiling engages, and the dropped total grows well
			 * past the queue depth -- the drop warning is rate limited to 5s, and
			 * the late one is the one that discriminates. */
			for (i = 0; i < 14; i++) {
				switch_ivr_sleep(session, 1000, SWITCH_TRUE, NULL);
			}

			switch_mutex_lock(g_obs.mutex);
			drops = g_obs.drops;
			d_dropped = g_obs.last_dropped - g_obs.prev_dropped;
			d_pos = g_obs.last_pos - g_obs.prev_pos;
			switch_mutex_unlock(g_obs.mutex);

			/* Unblock the writer before tearing down. */
			drain_fifo(rfd, 3000);
			switch_ivr_stop_record_session(session, fifo);
			drain_fifo(rfd, 2000);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] drops=%d d_dropped=%" SWITCH_SIZE_T_FMT
							  " d_stream_pos=%" SWITCH_SIZE_T_FMT "\n", drops, d_dropped, d_pos);

			/* Control: without a discard the invariant is untested, so refuse to
			 * report a pass. */
			fst_check(drops >= 2);

			/* The regression.  While the queue sits at the ceiling every byte
			 * enqueued is discarded, so between two discards the stream position
			 * must advance by what was discarded.  Uncounted, the discard cancels
			 * the enqueue exactly and the position does not move at all.  Half is
			 * the threshold: the real margin is 100% against 0%. */
			fst_check(d_pos >= d_dropped / 2);

			switch_core_session_rwunlock(session);
			if (rfd >= 0) close(rfd);
			unlink(fifo);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
