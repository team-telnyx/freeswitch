/*
 * Regression test for RECORD_WRITE_ERROR_GRACE_MS.
 *
 * A recording whose writes keep failing is abandoned once the failures have run
 * for longer than the grace period.  0 turns that off and restores the retry
 * forever behaviour the grace period replaced, so it is the config-only way back.
 *
 *   default : writes that keep failing are given up on after 1000ms
 *   0       : writes keep failing and the recording is never given up on
 *
 * The recorder writes to /dev/full through a .raw path, so the open succeeds and
 * every write fails with ENOSPC.  The default case is the control: it proves the
 * writes really fail, so the 0 case cannot pass on a recording that never erred.
 */
#include <switch.h>
#include <test/switch_test.h>
#include <unistd.h>

static struct {
	switch_mutex_t *mutex;
	int write_errors;
	int gave_up;
} g_obs;

static switch_status_t obs_logger(const switch_log_node_t *node, switch_log_level_t level)
{
	if (!node || !node->data) return SWITCH_STATUS_SUCCESS;

	if (strstr(node->data, "Error writing ")) {
		switch_mutex_lock(g_obs.mutex);
		g_obs.write_errors++;
		switch_mutex_unlock(g_obs.mutex);
	} else if (strstr(node->data, "Giving up on ")) {
		switch_mutex_lock(g_obs.mutex);
		g_obs.gave_up++;
		switch_mutex_unlock(g_obs.mutex);
	}

	return SWITCH_STATUS_SUCCESS;
}

/* The FST checks only work inside a test body, so setup failures are returned. */
static int record_to_full_device(const char *grace, int *write_errors, int *gave_up)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_call_cause_t cause = SWITCH_CAUSE_NONE;
	char path[256];
	int i;

	switch_snprintf(path, sizeof(path), "%s%srec-grace-%d.raw",
					SWITCH_GLOBAL_dirs.temp_dir, SWITCH_PATH_SEPARATOR, (int) getpid());
	unlink(path);
	if (symlink("/dev/full", path) != 0) {
		return 0;
	}

	switch_mutex_lock(g_obs.mutex);
	g_obs.write_errors = 0;
	g_obs.gave_up = 0;
	switch_mutex_unlock(g_obs.mutex);

	if (switch_ivr_originate(NULL, &session, &cause, "loopback/app=playback:silence_stream://60000", 5,
							 NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS || !session) {
		unlink(path);
		return 0;
	}
	channel = switch_core_session_get_channel(session);
	switch_channel_answer(channel);

	/* Without this the core buffers 64KB before the first write reaches the file. */
	switch_channel_set_variable(channel, "enable_file_write_buffering", "false");

	if (grace) {
		switch_channel_set_variable(channel, "RECORD_WRITE_ERROR_GRACE_MS", grace);
	}

	if (switch_ivr_record_session_event(session, path, 0, NULL, NULL) != SWITCH_STATUS_SUCCESS) {
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(session);
		unlink(path);
		return 0;
	}

	/* switch_ivr_sleep reads frames; the record bug only fires from the read loop.
	 * Three seconds is three times the default grace. */
	for (i = 0; i < 3; i++) {
		switch_ivr_sleep(session, 1000, SWITCH_TRUE, NULL);
	}

	switch_mutex_lock(g_obs.mutex);
	*write_errors = g_obs.write_errors;
	*gave_up = g_obs.gave_up;
	switch_mutex_unlock(g_obs.mutex);

	switch_ivr_stop_record_session(session, path);
	switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
	switch_core_session_rwunlock(session);
	unlink(path);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "[TEST] grace=%s write_errors=%d gave_up=%d\n",
					  grace ? grace : "default", *write_errors, *gave_up);

	return 1;
}

FST_CORE_BEGIN("./conf_async")
{
	FST_SUITE_BEGIN(record_write_error_grace)
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

		FST_TEST_BEGIN(default_grace_gives_up)
		{
			int write_errors = 0, gave_up = 0;

			fst_requires(record_to_full_device(NULL, &write_errors, &gave_up));

			fst_check(write_errors >= 1);
			fst_check(gave_up == 1);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(zero_grace_never_gives_up)
		{
			int write_errors = 0, gave_up = 0;

			fst_requires(record_to_full_device("0", &write_errors, &gave_up));

			fst_check(write_errors >= 1);
			fst_check(gave_up == 0);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
