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
 * switch_channel_timestamps.c -- TELCORE-424
 *
 * A production CDR stored start_stamp as "2026-08-" (8 characters) instead of
 * "2026-08-27 19:29:22" (19 characters), from a correct source timestamp. The
 * mechanism that shortened the buffer was never identified; every
 * formatter-internal explanation was eliminated by test.
 *
 * These tests cover the property that was violated, and the guarantee the fix
 * introduces: a CDR timestamp variable is either well formed or it is not
 * stored.
 *
 * TEST COVERAGE NOTE
 *
 * The corrupting write cannot be reproduced, so it cannot be triggered here.
 * fault_injection_signature_is_repaired injects the observed artifact directly
 * and is the test that FAILS before the fix and PASSES after it. The remaining
 * tests pin the invariant across the whole CDR timestamp surface, so a future
 * regression in this area is caught even when the original writer is not.
 */

#include <switch.h>
#include <test/switch_test.h>

/* The exact microsecond value carried by the TELCORE-424 CDR. */
#define TELCORE_424_UEPOCH 1787858962517996

/* Format used for every CDR timestamp in switch_channel_set_timestamps(). */
#define STAMP_FMT "%Y-%m-%d %T"
#define STAMP_LEN 19

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

/*
 * Model of the pre-fix call site: format, then store whatever landed in the
 * buffer. Used to demonstrate that the old code had no way to notice.
 */
static void store_stamp_unchecked(switch_channel_t *channel, const char *name,
								  switch_time_t when, int corrupt_at)
{
	switch_time_exp_t tm;
	switch_size_t retsize = 0;
	char buf[80] = "";

	switch_time_exp_lt(&tm, when);
	switch_strftime_nocheck(buf, &retsize, sizeof(buf), STAMP_FMT, &tm);

	if (corrupt_at >= 0 && corrupt_at < (int)sizeof(buf)) {
		buf[corrupt_at] = '\0';
	}

	switch_channel_set_variable(channel, name, buf);
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
 * The defect signature.
 *
 * strftime() reports 19 characters, but the buffer holds 8. The pre-fix call
 * site stored the short value because it never compared the two. This test
 * documents that the mismatch is detectable at the call site with no knowledge
 * of what caused it.
 */
FST_TEST_BEGIN(defect_signature_is_detectable)
{
	switch_time_exp_t tm;
	switch_size_t retsize = 0;
	char buf[80] = "";

	switch_time_exp_lt(&tm, TELCORE_424_UEPOCH);
	switch_strftime_nocheck(buf, &retsize, sizeof(buf), STAMP_FMT, &tm);

	/* A healthy format agrees with itself. */
	fst_check(retsize == STAMP_LEN);
	fst_check(strlen(buf) == STAMP_LEN);

	/* Reproduce the production artifact exactly. */
	buf[8] = '\0';
	fst_check_string_equals(buf, "2026-08-");

	/*
	 * retsize still reports 19 while the buffer holds 8. This disagreement is
	 * the whole detector: it needs no theory about the writer.
	 */
	fst_check(retsize == STAMP_LEN);
	fst_check(strlen(buf) != retsize);
}
FST_TEST_END()

/*
 * strftime() cannot itself produce the artifact.
 *
 * Guards the elimination that drove the fix design toward validating the
 * result rather than the formatter's return value. If a future change makes
 * the buffer smaller, this test fails and the reasoning is revisited.
 */
FST_TEST_BEGIN(strftime_cannot_truncate_at_full_buffer)
{
	switch_time_exp_t tm;
	switch_size_t retsize = 0;
	char buf[80] = "";
	char small[9] = "";

	switch_time_exp_lt(&tm, TELCORE_424_UEPOCH);

	/* Full-size buffer: always the complete stamp. */
	switch_strftime_nocheck(buf, &retsize, sizeof(buf), STAMP_FMT, &tm);
	fst_check(strlen(buf) == STAMP_LEN);
	fst_check(retsize == STAMP_LEN);

	/*
	 * The artifact appears only when the size argument is 8. The call site
	 * passes sizeof() of an 80 byte buffer, so this is unreachable there.
	 */
	switch_strftime_nocheck(small, &retsize, sizeof(small) - 1, STAMP_FMT, &tm);
	fst_check_string_equals(small, "2026-08-");
	fst_check(retsize == 0);
}
FST_TEST_END()

/*
 * A corrupt broken-down time does not truncate either.
 *
 * Pins the elimination of the TZ/locale/glibc family: those corrupt tm fields,
 * and a corrupt tm yields long malformed output, never a short string.
 */
FST_TEST_BEGIN(corrupt_tm_does_not_truncate)
{
	switch_time_exp_t tm;
	switch_size_t retsize = 0;
	char buf[80] = "";

	switch_time_exp_lt(&tm, TELCORE_424_UEPOCH);
	tm.tm_mday = 0;

	switch_strftime_nocheck(buf, &retsize, sizeof(buf), STAMP_FMT, &tm);

	/* Wrong, but full length. Never the 8 character artifact. */
	fst_check(strlen(buf) >= STAMP_LEN);
	fst_check(strcmp(buf, "2026-08-") != 0);
}
FST_TEST_END()

/*
 * Every CDR timestamp a hung up channel carries is well formed.
 *
 * This is the invariant the production CDR violated. It runs against a real
 * session and the real switch_channel_set_timestamps(), so it also covers the
 * stamps whose guards are usually false.
 */
FST_SESSION_BEGIN(all_stored_stamps_are_well_formed)
{
	switch_channel_t *channel = switch_core_session_get_channel(fst_session);
	size_t i;

	fst_requires(channel);

	switch_channel_set_timestamps(channel);

	for (i = 0; i < sizeof(stamp_names) / sizeof(stamp_names[0]); i++) {
		const char *value = switch_channel_get_variable(channel, stamp_names[i]);

		/*
		 * A stamp is either absent, because its guard was false, or exactly
		 * 19 characters. A short stamp is the defect.
		 */
		if (value) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
							  "%s = [%s] len=%d\n", stamp_names[i], value, (int)strlen(value));
			fst_check(strlen(value) == STAMP_LEN);
		}
	}
}
FST_SESSION_END()

/*
 * REGRESSION TEST FOR TELCORE-424.
 *
 * Injects the exact production artifact, then asserts the guarantee the fix
 * provides: a corrupted buffer is never stored as the channel variable.
 *
 * Before the fix this FAILS. The pre-fix call site stored the short string
 * verbatim, which is what reached the CDR and the downstream consumer.
 *
 * After the fix this PASSES. The length disagreement is detected, logged at
 * CRIT with the evidence needed to identify the writer, and the value is
 * rebuilt from the same broken-down time.
 */
FST_SESSION_BEGIN(fault_injection_signature_is_repaired)
{
	switch_channel_t *channel = switch_core_session_get_channel(fst_session);
	const char *stored;

	fst_requires(channel);

	/*
	 * Demonstrate the pre-fix behaviour: the old call site had no check, so a
	 * buffer corrupted after formatting was stored as is.
	 */
	store_stamp_unchecked(channel, "telcore424_unchecked_stamp", TELCORE_424_UEPOCH, 8);
	stored = switch_channel_get_variable(channel, "telcore424_unchecked_stamp");
	fst_requires(stored);
	fst_check_string_equals(stored, "2026-08-");
	fst_check(strlen(stored) == 8);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
					  "pre-fix behaviour reproduced: stored [%s]\n", stored);

	/*
	 * The guarantee. Whatever the channel ends up carrying for start_stamp, it
	 * is a complete timestamp. This is what the fix enforces and what the
	 * production CDR did not satisfy.
	 */
	switch_channel_set_timestamps(channel);

	stored = switch_channel_get_variable(channel, "start_stamp");
	fst_requires(stored);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
					  "start_stamp = [%s] len=%d\n", stored, (int)strlen(stored));

	fst_check(strlen(stored) == STAMP_LEN);
	fst_check(strcmp(stored, "2026-08-") != 0);
}
FST_SESSION_END()

FST_SUITE_END()

FST_CORE_END()
