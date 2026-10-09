/*
 * switch_ivr_bridge_dtx.c -- far-end DTX across a bridge
 *
 * Leg A: 1 frame + (period - 1) CNG frames per interval, bridge_generate_comfort_noise=true.
 * Compares frames written to leg B with frames read from leg A.
 */

#include <switch.h>
#include <test/switch_test.h>

#define DTX_PERIOD 8
#define MEASURE_MS 1000

static uint32_t frame_count(switch_core_session_t *session, const char *var)
{
	const char *count = switch_channel_get_variable(switch_core_session_get_channel(session), var);

	return count ? (uint32_t) atoi(count) : 0;
}

static switch_atomic_t tap_native_read_calls, write_replace_calls;

/* media bug that changes the audio it replaces; counts native taps and write replacements */
static switch_bool_t replace_audio_bug(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	switch_frame_t *frame = NULL;
	uint32_t i;

	if (type == SWITCH_ABC_TYPE_TAP_NATIVE_READ) {
		switch_atomic_inc(&tap_native_read_calls);
		return SWITCH_TRUE;
	}
	if (type == SWITCH_ABC_TYPE_WRITE_REPLACE) {
		switch_atomic_inc(&write_replace_calls);
		frame = switch_core_media_bug_get_write_replace_frame(bug);
	} else if (type == SWITCH_ABC_TYPE_READ_REPLACE) {
		frame = switch_core_media_bug_get_read_replace_frame(bug);
	}
	if (frame && frame->data && frame->datalen) {
		int16_t *pcm = (int16_t *) frame->data;

		for (i = 0; i < frame->datalen / 2; i++) {
			pcm[i] = (int16_t) (pcm[i] / 2 + 100);
		}
		if (type == SWITCH_ABC_TYPE_WRITE_REPLACE) {
			switch_core_media_bug_set_write_replace_frame(bug, frame);
		} else {
			switch_core_media_bug_set_read_replace_frame(bug, frame);
		}
	}

	return SWITCH_TRUE;
}

/* frames read from A vs written to B, once media flows; bug_flags_a/b: optional replace_audio_bug on each leg */
static switch_bool_t measure_bridge_bugs(const char *codec_vars_a, const char *codec_vars_b, int dtx_period, uint32_t bug_flags_a, uint32_t bug_flags_b,
										 uint32_t *read_from_a, uint32_t *written_to_b)
{
	switch_core_session_t *a = NULL, *b = NULL;
	switch_call_cause_t cause;
	char dial_a[256], dial_b[256];
	uint32_t read_start, write_start;
	switch_bool_t ok = SWITCH_FALSE;
	int waited;

	*read_from_a = *written_to_b = 0;

	switch_snprintf(dial_a, sizeof(dial_a), "{%s,null_dtx_period=%d,null_count_frames=true,bridge_generate_comfort_noise=true}null/+15550001111",
					codec_vars_a, dtx_period);
	switch_snprintf(dial_b, sizeof(dial_b), "{%s,null_count_frames=true}null/+15550002222", codec_vars_b);

	if (switch_ivr_originate(NULL, &a, &cause, dial_a, 5, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS || !a) {
		return SWITCH_FALSE;
	}
	if (switch_ivr_originate(NULL, &b, &cause, dial_b, 5, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS || !b) {
		switch_channel_hangup(switch_core_session_get_channel(a), SWITCH_CAUSE_NORMAL_CLEARING);
		switch_core_session_rwunlock(a);
		return SWITCH_FALSE;
	}

	if (switch_ivr_uuid_bridge(switch_core_session_get_uuid(a), switch_core_session_get_uuid(b)) == SWITCH_STATUS_SUCCESS) {
		for (waited = 0; waited < 2000 && !frame_count(b, "null_write_count"); waited += 10) {
			switch_yield(10000);
		}
		/* bugs start mid-call, once media flows */
		if (bug_flags_a) {
			switch_media_bug_t *bug = NULL;
			switch_core_media_bug_add(a, "bridge_dtx_test", NULL, replace_audio_bug, NULL, 0, bug_flags_a, &bug);
		}
		if (bug_flags_b) {
			switch_media_bug_t *bug = NULL;
			switch_core_media_bug_add(b, "bridge_dtx_test", NULL, replace_audio_bug, NULL, 0, bug_flags_b, &bug);
		}
		switch_atomic_set(&tap_native_read_calls, 0);
		switch_atomic_set(&write_replace_calls, 0);
		read_start = frame_count(a, "null_read_count");
		write_start = frame_count(b, "null_write_count");
		switch_yield(MEASURE_MS * 1000);
		*read_from_a = frame_count(a, "null_read_count") - read_start;
		*written_to_b = frame_count(b, "null_write_count") - write_start;
		ok = SWITCH_TRUE;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "[bridge-dtx] %s -> %s: read %u frames from leg A, wrote %u frames to leg B\n",
					  codec_vars_a, codec_vars_b, *read_from_a, *written_to_b);

	switch_channel_hangup(switch_core_session_get_channel(a), SWITCH_CAUSE_NORMAL_CLEARING);
	switch_channel_hangup(switch_core_session_get_channel(b), SWITCH_CAUSE_NORMAL_CLEARING);
	switch_core_session_rwunlock(a);
	switch_core_session_rwunlock(b);

	return ok;
}

static switch_bool_t measure_bridge_ex(const char *codec_vars_a, const char *codec_vars_b, int dtx_period, uint32_t *read_from_a, uint32_t *written_to_b)
{
	return measure_bridge_bugs(codec_vars_a, codec_vars_b, dtx_period, 0, 0, read_from_a, written_to_b);
}

static switch_bool_t measure_bridge(const char *codec_vars, uint32_t *read_from_a, uint32_t *written_to_b)
{
	return measure_bridge_ex(codec_vars, codec_vars, DTX_PERIOD, read_from_a, written_to_b);
}

FST_CORE_BEGIN("./conf")
{
FST_SUITE_BEGIN(switch_ivr_bridge_dtx)
{
	FST_SETUP_BEGIN()
	{
		fst_requires_module("mod_loopback");
		fst_requires_module("mod_amrwb");
		fst_requires_module("mod_amr");
	}
	FST_SETUP_END()

	FST_TEARDOWN_BEGIN()
	{
	}
	FST_TEARDOWN_END()

	/* native DTX passthrough: forwarded frames only, no fill */
	FST_TEST_BEGIN(test_native_dtx_passthrough_forwards_frames_without_fill)
	{
		uint32_t read_from_a, written_to_b;

		fst_requires(measure_bridge("null_codec=AMR-WB,rate=16000", &read_from_a, &written_to_b));
		fst_requires(read_from_a >= DTX_PERIOD * 2);
		fst_check(written_to_b > 0);
		fst_xcheck(written_to_b * DTX_PERIOD <= read_from_a * 2, "only the forwarded frames reach leg B, no generated fill");
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_native_dtx_passthrough_forwards_frames_without_fill_amr)
	{
		uint32_t read_from_a, written_to_b;

		fst_requires(measure_bridge("null_codec=AMR,rate=8000", &read_from_a, &written_to_b));
		fst_requires(read_from_a >= DTX_PERIOD * 2);
		fst_check(written_to_b > 0);
		fst_xcheck(written_to_b * DTX_PERIOD <= read_from_a * 2, "only the forwarded frames reach leg B, no generated fill");
	}
	FST_TEST_END()

	/* far end stops sending: fill resumes after the hold-off */
	FST_TEST_BEGIN(test_native_dtx_passthrough_fills_when_far_end_stops_sending)
	{
		uint32_t read_from_a, written_to_b;

		fst_requires(measure_bridge_ex("null_codec=AMR-WB,rate=16000", "null_codec=AMR-WB,rate=16000", 1000, &read_from_a, &written_to_b));
		fst_requires(read_from_a >= DTX_PERIOD * 2);
		/* everything but the hold-off is filled */
		fst_xcheck(written_to_b > 0 && written_to_b + 480 / 20 + 4 >= read_from_a, "generated fill after the far end went quiet");
	}
	FST_TEST_END()

	/* transcoded pair: fill is kept */
	FST_TEST_BEGIN(test_native_dtx_transcoded_keeps_generated_fill)
	{
		uint32_t read_from_a, written_to_b;

		fst_requires(measure_bridge_ex("null_codec=AMR-WB,rate=16000", "null_codec=L16,rate=16000", DTX_PERIOD, &read_from_a, &written_to_b));
		fst_requires(read_from_a >= DTX_PERIOD * 2);
		fst_xcheck(written_to_b * 4 >= read_from_a * 3, "generated fill keeps leg B continuous");
	}
	FST_TEST_END()

	/* write-replace bug on leg B changes the audio: B re-encodes, SIDs become speech frames, fill is kept */
	FST_TEST_BEGIN(test_native_dtx_with_write_replace_bug_keeps_generated_fill)
	{
		uint32_t read_from_a, written_to_b;

		fst_requires(measure_bridge_bugs("null_codec=AMR-WB,rate=16000", "null_codec=AMR-WB,rate=16000", DTX_PERIOD, 0, SMBF_WRITE_REPLACE,
										 &read_from_a, &written_to_b));
		fst_requires(read_from_a >= DTX_PERIOD * 2);
		fst_xcheck(written_to_b * 4 >= read_from_a * 3, "generated fill keeps leg B continuous");
	}
	FST_TEST_END()

	/* read-replace bug on leg A changes the audio before it is forwarded: fill is kept */
	FST_TEST_BEGIN(test_native_dtx_with_read_replace_bug_keeps_generated_fill)
	{
		uint32_t read_from_a, written_to_b;

		fst_requires(measure_bridge_bugs("null_codec=AMR-WB,rate=16000", "null_codec=AMR-WB,rate=16000", DTX_PERIOD, SMBF_READ_REPLACE, 0,
										 &read_from_a, &written_to_b));
		fst_requires(read_from_a >= DTX_PERIOD * 2);
		fst_xcheck(written_to_b * 4 >= read_from_a * 3, "generated fill keeps leg B continuous");
	}
	FST_TEST_END()

	/* a native tap on leg A leaves the frames untouched: still only the forwarded frames, no fill */
	FST_TEST_BEGIN(test_native_dtx_with_native_tap_forwards_frames_without_fill)
	{
		uint32_t read_from_a, written_to_b;

		fst_requires(measure_bridge_bugs("null_codec=AMR-WB,rate=16000", "null_codec=AMR-WB,rate=16000", DTX_PERIOD, SMBF_TAP_NATIVE_READ, 0,
										 &read_from_a, &written_to_b));
		fst_requires(read_from_a >= DTX_PERIOD * 2);
		fst_xcheck(switch_atomic_read(&tap_native_read_calls) > 0, "the native tap on leg A runs");
		fst_check(written_to_b > 0);
		fst_xcheck(written_to_b * DTX_PERIOD <= read_from_a * 2, "only the forwarded frames reach leg B, no generated fill");
	}
	FST_TEST_END()

	/* native tap on leg A (its read path stays tap-only and skips the partner-bug check) and a write-replace bug on
	 * leg B that changes the audio: B re-encodes, SIDs become speech frames, fill is kept */
	FST_TEST_BEGIN(test_native_dtx_with_native_tap_and_write_replace_bug_keeps_generated_fill)
	{
		uint32_t read_from_a, written_to_b;

		fst_requires(measure_bridge_bugs("null_codec=AMR-WB,rate=16000", "null_codec=AMR-WB,rate=16000", DTX_PERIOD, SMBF_TAP_NATIVE_READ,
										 SMBF_WRITE_REPLACE, &read_from_a, &written_to_b));
		fst_requires(read_from_a >= DTX_PERIOD * 2);
		fst_xcheck(switch_atomic_read(&tap_native_read_calls) > 0, "the native tap on leg A runs");
		fst_xcheck(switch_atomic_read(&write_replace_calls) > 0, "the write-replace bug on leg B runs");
		fst_xcheck(written_to_b * 4 >= read_from_a * 3, "generated fill keeps leg B continuous");
	}
	FST_TEST_END()

	/* no native DTX: fill is kept */
	FST_TEST_BEGIN(test_non_dtx_passthrough_keeps_generated_fill)
	{
		uint32_t read_from_a, written_to_b;

		fst_requires(measure_bridge("null_codec=L16,rate=8000", &read_from_a, &written_to_b));
		fst_requires(read_from_a >= DTX_PERIOD * 2);
		fst_xcheck(written_to_b * 4 >= read_from_a * 3, "generated fill keeps leg B continuous");
	}
	FST_TEST_END()
}
FST_SUITE_END()
}
FST_CORE_END()
