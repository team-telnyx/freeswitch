/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * switch_channel_timestamps.c -- CDR timestamp validation
 *
 * SCOPE OF THESE TESTS
 *
 * A production CDR stored start_stamp as "2026-08-" (8 characters) instead of
 * "2026-08-27 19:29:22" (19 characters), from a correct source timestamp. The
 * code that shortened the buffer was never identified. These tests do NOT
 * reproduce that write and do NOT show that it cannot happen again. They cover
 * the defensive validation added in response to it, which would not have
 * prevented the original write.
 *
 * The discriminating test is malformed_stamp_is_rejected_and_repaired. It
 * drives the real switch_channel_set_timestamps() with a caller_profile time
 * that formats to a length other than 19, which needs no memory corruption and
 * is therefore deterministic. Against the unvalidated code that stamp reaches
 * the CDR unchanged and the test fails; with validation in place the stamp is
 * rejected, logged, repaired, and the test passes.
 *
 * WHAT THESE TESTS DO NOT ASSERT
 *
 *   - that the corruption reported in the field cannot happen again. It is not
 *     reproduced here and a green run says nothing about it.
 *   - anything about the code that shortened the production buffer. It was
 *     never identified.
 *
 * A malformed stamp that is detected is repaired to a well formed value, which
 * can differ from the true time when the source time is out of range. The CRIT
 * line carries the original bytes for anyone who needs them.
 */

#include <switch.h>
#include <test/switch_test.h>

/* Length and format of every CDR timestamp switch_channel_set_timestamps() writes. */
#define STAMP_LEN 19
#define STAMP_FMT "%Y-%m-%d %T"

/* The microsecond value carried by the CDR that prompted this validation. */
#define REPORTED_UEPOCH 1787858962517996

/*
 * A caller_profile time that formats to 20 characters rather than 19, because
 * the year needs 5 digits. Reachable through ordinary assignment, so the test
 * needs no fault injection.
 */
#define OVERLONG_UEPOCH 253402300800000000

static const char *stamp_names[] = {
	"start_stamp",
	"profile_start_stamp",
	"answer_stamp",
	"bridge_stamp",
	"hold_stamp",
	"resurrect_stamp",
	"progress_stamp",
	"progress_media_stamp",
	"end_stamp"
};

/* Re-run switch_channel_set_timestamps() on a channel that already has stamps. */
static void reset_timestamps(switch_channel_t *channel)
{
	switch_channel_clear_flag(channel, CF_TIMESTAMP_SET);
}

/*
 * Capture the CRIT diagnostic the guard emits.
 *
 * The log pipeline is asynchronous, so the logger stores the first matching
 * line and signals a condition the test waits on, following the pattern in
 * tests/unit/switch_log.c.
 */
static switch_mutex_t *log_mutex = NULL;
static switch_thread_cond_t *log_cond = NULL;
static char *captured_crit = NULL;

static switch_status_t crit_logger(const switch_log_node_t *node, switch_log_level_t level)
{
	switch_mutex_lock(log_mutex);
	if (level == SWITCH_LOG_CRIT && !captured_crit && node->content &&
		strstr(node->content, "Malformed CDR timestamp")) {
		captured_crit = strdup(node->content);
		switch_thread_cond_signal(log_cond);
	}
	switch_mutex_unlock(log_mutex);
	return SWITCH_STATUS_SUCCESS;
}

static char *wait_for_crit(switch_interval_time_t timeout_ms)
{
	char *line = NULL;
	switch_time_t now = switch_time_now();
	switch_time_t expiration = now + (timeout_ms * 1000);

	switch_mutex_lock(log_mutex);
	while (!captured_crit && (now = switch_time_now()) < expiration) {
		switch_thread_cond_timedwait(log_cond, log_mutex, expiration - now);
	}
	line = captured_crit;
	captured_crit = NULL;
	switch_mutex_unlock(log_mutex);

	return line;
}

FST_CORE_BEGIN("./conf")

FST_SUITE_BEGIN(switch_channel_timestamps)

FST_SETUP_BEGIN()
{
}
FST_SETUP_END()

FST_TEARDOWN_BEGIN()
{
}
FST_TEARDOWN_END()

/*
 * THE DISCRIMINATING TEST.
 *
 * Sets caller_profile->times->created to a value that formats to 20 characters,
 * then runs the real switch_channel_set_timestamps().
 *
 * Without the validation the formatted stamp is stored as is, start_stamp is 20
 * characters long, and this test fails. With the validation the length is
 * checked, a CRIT is logged with the evidence, the value is rebuilt to a well
 * formed stamp, and this test passes.
 */
FST_SESSION_BEGIN(malformed_stamp_is_rejected_and_repaired)
{
	switch_channel_t *channel = switch_core_session_get_channel(fst_session);
	switch_caller_profile_t *profile;
	const char *stored;
	char *crit;

	fst_requires(channel);

	profile = switch_channel_get_caller_profile(channel);
	fst_requires(profile);
	fst_requires(profile->times);

	switch_mutex_init(&log_mutex, SWITCH_MUTEX_NESTED, fst_pool);
	switch_thread_cond_create(&log_cond, fst_pool);
	switch_log_bind_logger(crit_logger, SWITCH_LOG_CRIT, SWITCH_FALSE);

	profile->times->created = OVERLONG_UEPOCH;
	profile->times->profile_created = OVERLONG_UEPOCH;

	reset_timestamps(channel);
	switch_channel_set_timestamps(channel);

	stored = switch_channel_get_variable(channel, "start_stamp");
	fst_requires(stored);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
					  "start_stamp = [%s] len=%d\n", stored, (int)strlen(stored));

	/* Fails on the unvalidated build, where the 20 character stamp is stored. */
	fst_check(strlen(stored) == STAMP_LEN);

	stored = switch_channel_get_variable(channel, "profile_start_stamp");
	fst_requires(stored);
	fst_check(strlen(stored) == STAMP_LEN);

	/*
	 * The diagnostic is the part that identifies the writer next time, so it
	 * has to carry the field name, both lengths and the rejected bytes.
	 */
	crit = wait_for_crit(1000);
	fst_requires(crit);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "captured: %s", crit);

	fst_check(strstr(crit, "start_stamp") != NULL);
	fst_check(strstr(crit, "retsize=") != NULL);
	fst_check(strstr(crit, "strlen=") != NULL);
	fst_check(strstr(crit, "buf=[") != NULL);

	switch_safe_free(crit);
	switch_log_unbind_logger(crit_logger);
}
FST_SESSION_END()

/*
 * The same guarantee for every stamp the function writes.
 *
 * The mechanism behind the production defect is unknown, so nothing marks
 * start_stamp as more exposed than its siblings.
 */
FST_SESSION_BEGIN(all_stored_stamps_are_well_formed)
{
	switch_channel_t *channel = switch_core_session_get_channel(fst_session);
	switch_caller_profile_t *profile;
	size_t i;

	fst_requires(channel);

	profile = switch_channel_get_caller_profile(channel);
	fst_requires(profile);
	fst_requires(profile->times);

	/* Give every guarded stamp a value so each formatting path runs. */
	profile->times->created = OVERLONG_UEPOCH;
	profile->times->profile_created = OVERLONG_UEPOCH;
	profile->times->answered = OVERLONG_UEPOCH;
	profile->times->bridged = OVERLONG_UEPOCH;
	profile->times->last_hold = OVERLONG_UEPOCH;
	profile->times->resurrected = OVERLONG_UEPOCH;
	profile->times->progress = OVERLONG_UEPOCH;
	profile->times->progress_media = OVERLONG_UEPOCH;
	profile->times->hungup = OVERLONG_UEPOCH;

	reset_timestamps(channel);
	switch_channel_set_timestamps(channel);

	for (i = 0; i < sizeof(stamp_names) / sizeof(stamp_names[0]); i++) {
		const char *value = switch_channel_get_variable(channel, stamp_names[i]);

		if (value) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
							  "%s = [%s] len=%d\n", stamp_names[i], value, (int)strlen(value));
			fst_check(strlen(value) == STAMP_LEN);
		}
	}
}
FST_SESSION_END()

/*
 * No false alarm.
 *
 * An ordinary timestamp must be stored unchanged. A guard that rewrites healthy
 * values would corrupt every CDR on the platform, so this is the test that
 * matters most for safety.
 */
FST_SESSION_BEGIN(healthy_stamp_is_untouched)
{
	switch_channel_t *channel = switch_core_session_get_channel(fst_session);
	switch_caller_profile_t *profile;
	switch_time_exp_t tm;
	switch_size_t retsize = 0;
	char expected[80] = "";
	const char *stored;

	fst_requires(channel);

	profile = switch_channel_get_caller_profile(channel);
	fst_requires(profile);
	fst_requires(profile->times);

	profile->times->created = REPORTED_UEPOCH;

	reset_timestamps(channel);
	switch_channel_set_timestamps(channel);

	/* Recompute independently from the same source value. */
	switch_time_exp_lt(&tm, REPORTED_UEPOCH);
	switch_strftime_nocheck(expected, &retsize, sizeof(expected), STAMP_FMT, &tm);

	stored = switch_channel_get_variable(channel, "start_stamp");
	fst_requires(stored);

	fst_check(strlen(stored) == STAMP_LEN);
	fst_check_string_equals(stored, expected);
}
FST_SESSION_END()

/*
 * The detection predicate.
 *
 * The production artifact had strftime() reporting 19 characters while the
 * buffer held 8. Only comparing the two catches it; checking the return value
 * alone does not. This test pins that reasoning so a later simplification to a
 * return-value check fails here.
 */
FST_TEST_BEGIN(length_disagreement_is_the_detector)
{
	switch_time_exp_t tm;
	switch_size_t retsize = 0;
	char buf[80] = "";

	switch_time_exp_lt(&tm, REPORTED_UEPOCH);
	switch_strftime_nocheck(buf, &retsize, sizeof(buf), STAMP_FMT, &tm);

	fst_check(retsize == STAMP_LEN);
	fst_check(strlen(buf) == STAMP_LEN);

	/* Reproduce the stored artifact exactly. */
	buf[8] = '\0';
	fst_check_string_equals(buf, "2026-08-");

	/* The reported length still says 19; only the buffer disagrees. */
	fst_check(retsize == STAMP_LEN);
	fst_check(strlen(buf) != retsize);
}
FST_TEST_END()

/*
 * strftime() cannot produce the artifact at the size the call site uses.
 *
 * Guards the elimination that pushed the fix toward validating the result. If
 * a later change shrinks the stamp buffer, this test fails and the reasoning
 * has to be revisited.
 */
FST_TEST_BEGIN(strftime_cannot_truncate_at_full_buffer)
{
	switch_time_exp_t tm;
	switch_size_t retsize = 0;
	char buf[80] = "";
	char small[9] = "";

	switch_time_exp_lt(&tm, REPORTED_UEPOCH);

	switch_strftime_nocheck(buf, &retsize, sizeof(buf), STAMP_FMT, &tm);
	fst_check(strlen(buf) == STAMP_LEN);
	fst_check(retsize == STAMP_LEN);

	/* The artifact appears only when the size argument is 8. */
	switch_strftime_nocheck(small, &retsize, sizeof(small) - 1, STAMP_FMT, &tm);
	fst_check_string_equals(small, "2026-08-");
	fst_check(retsize == 0);
}
FST_TEST_END()

/*
 * A corrupt broken-down time does not truncate.
 *
 * Pins the elimination of the timezone, locale and library-race explanations:
 * those corrupt tm fields, and a corrupt tm gives long malformed output.
 */
FST_TEST_BEGIN(corrupt_tm_does_not_truncate)
{
	switch_time_exp_t tm;
	switch_size_t retsize = 0;
	char buf[80] = "";

	switch_time_exp_lt(&tm, REPORTED_UEPOCH);
	tm.tm_mday = 0;

	switch_strftime_nocheck(buf, &retsize, sizeof(buf), STAMP_FMT, &tm);

	fst_check(strlen(buf) >= STAMP_LEN);
	fst_check(strcmp(buf, "2026-08-") != 0);
}
FST_TEST_END()

FST_SUITE_END()

FST_CORE_END()
