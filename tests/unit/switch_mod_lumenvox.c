/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * switch_mod_lumenvox.c -- unit tests for mod_lumenvox (LumenVox gRPC ASR)
 *
 * These tests exercise the FreeSWITCH-facing ASR interface (switch_asr_interface_t)
 * that mod_lumenvox registers, driving it black-box through the public
 * switch_core_asr_* API -- exactly how the core (play_and_detect_speech, etc.)
 * uses it.
 *
 * The suite runs in one of two modes:
 *
 *  - Offline (default, CI): conf_lumenvox points the profile at a closed local
 *    port (127.0.0.1:1). lv_session_open() fails closed when no session_id
 *    arrives within connect-timeout-ms, so switch_core_asr_open() against a
 *    dead backend must FAIL promptly -- that fail-closed contract is what the
 *    offline tests assert. Everything that needs an open handle is live-only.
 *
 *  - Live: set LV_TEST_TARGET (host:port of a reachable LumenVox gRPC API) and
 *    LV_TEST_DEPLOYMENT / LV_TEST_OPERATOR (tenancy UUIDs) to run the full
 *    interface surface against a real server: open/close, grammar bookkeeping,
 *    params, guards, handle lifecycle. Recognition itself is still not asserted
 *    to succeed -- that depends on the ASR engine being provisioned on the
 *    target deployment.
 *
 * TTS is intentionally not covered here: the LumenVox TTS engine is not reliably
 * available on the dev deployment, and this suite is scoped to ASR.
 */
#include <switch.h>
#include <stdlib.h>

#include <test/switch_test.h>

/* ASR interface name mod_lumenvox registers (mod_lumenvox.cpp asr_open). If the
 * module is ever switched to the transparent "unimrcp" drop-in name, change this
 * single define. */
#define LV_ENGINE "lumenvox"

/* Live mode: a reachable LumenVox server was provided via the environment
 * (conf_lumenvox/freeswitch.xml picks the same variables up via exec-set). */
static int lv_live(void)
{
	const char *t = getenv("LV_TEST_TARGET");
	return t && *t;
}

/* Open an ASR handle on the default profile; used by most tests. */
static switch_status_t lv_open(switch_asr_handle_t *ah, switch_memory_pool_t *pool)
{
	switch_asr_flag_t flags = SWITCH_ASR_FLAG_NONE;
	return switch_core_asr_open(ah, LV_ENGINE, "L16", 8000, "", &flags, pool);
}

static switch_status_t lv_close(switch_asr_handle_t *ah)
{
	switch_asr_flag_t flags = SWITCH_ASR_FLAG_NONE;
	return switch_core_asr_close(ah, &flags);
}

FST_CORE_BEGIN("./conf_lumenvox")
{
	FST_SUITE_BEGIN(switch_mod_lumenvox)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_lumenvox");
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		/* Live: a handle can be opened on the default profile and cleanly
		 * closed. Offline: open must fail CLOSED, and promptly (bounded by
		 * connect-timeout-ms) -- a dead backend must not hand out a half-open
		 * handle that only breaks later, mid-recognition. */
		FST_TEST_BEGIN(asr_open_close)
		{
			switch_asr_handle_t ah = { 0 };
			if (lv_live()) {
				fst_requires(lv_open(&ah, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_check(ah.private_info != NULL);
				fst_requires(lv_close(&ah) == SWITCH_STATUS_SUCCESS);
			} else {
				switch_time_t started = switch_time_now();
				fst_check(lv_open(&ah, fst_pool) != SWITCH_STATUS_SUCCESS);
				fst_check(switch_time_now() - started < 3000000); /* well under 3 s */
			}
		}
		FST_TEST_END()

		/* Opening against an unknown profile is rejected (not a crash). */
		FST_TEST_BEGIN(asr_open_unknown_profile)
		{
			switch_asr_handle_t ah = { 0 };
			switch_asr_flag_t flags = SWITCH_ASR_FLAG_NONE;
			switch_status_t status = switch_core_asr_open(&ah, LV_ENGINE, "L16", 8000, "no-such-profile", &flags, fst_pool);
			fst_check(status != SWITCH_STATUS_SUCCESS);
		}
		FST_TEST_END()

		/* Grammar bookkeeping is server-independent once a handle is open, and
		 * is asserted exactly: load, enable/disable, disable-all, unload, and
		 * the correct failure codes for operations on unknown grammar names.
		 * Live-only: opening a handle requires a reachable server. */
		FST_TEST_BEGIN(asr_grammar_management)
		{
			if (lv_live()) {
			switch_asr_handle_t ah = { 0 };
			fst_requires(lv_open(&ah, fst_pool) == SWITCH_STATUS_SUCCESS);

			/* load two grammars */
			fst_check(switch_core_asr_load_grammar(&ah, "builtin:digits", "g1") == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_asr_load_grammar(&ah, "builtin:boolean", "g2") == SWITCH_STATUS_SUCCESS);

			/* empty grammar is rejected by the core wrapper */
			fst_check(switch_core_asr_load_grammar(&ah, "", "g3") != SWITCH_STATUS_SUCCESS);

			/* enable/disable a known grammar */
			fst_check(switch_core_asr_enable_grammar(&ah, "g1") == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_asr_disable_grammar(&ah, "g1") == SWITCH_STATUS_SUCCESS);

			/* enable/disable an unknown grammar fails cleanly */
			fst_check(switch_core_asr_enable_grammar(&ah, "nope") != SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_asr_disable_grammar(&ah, "nope") != SWITCH_STATUS_SUCCESS);

			/* disable all, then re-enable a still-loaded grammar */
			fst_check(switch_core_asr_disable_all_grammars(&ah) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_asr_enable_grammar(&ah, "g2") == SWITCH_STATUS_SUCCESS);

			/* unload known; unload of an unknown name is idempotent (succeeds) */
			fst_check(switch_core_asr_unload_grammar(&ah, "g1") == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_asr_unload_grammar(&ah, "nope") == SWITCH_STATUS_SUCCESS);

			fst_requires(lv_close(&ah) == SWITCH_STATUS_SUCCESS);
			}
		}
		FST_TEST_END()

		/* Param setters accept known/unknown params without crashing, and with
		 * no recognition in flight the result accessors report "nothing yet".
		 * Live-only: needs an open handle. */
		FST_TEST_BEGIN(asr_params_and_empty_results)
		{
			if (lv_live()) {
			switch_asr_handle_t ah = { 0 };
			switch_asr_flag_t flags = SWITCH_ASR_FLAG_NONE;
			char *xmlstr = NULL;
			switch_event_t *headers = NULL;

			fst_requires(lv_open(&ah, fst_pool) == SWITCH_STATUS_SUCCESS);

			/* text / numeric / float params (mapped and unmapped) */
			switch_core_asr_text_param(&ah, "speech-language", "en-US");
			switch_core_asr_text_param(&ah, "unmapped-text", "x");
			switch_core_asr_numeric_param(&ah, "no-input-timeout", 5000);
			switch_core_asr_numeric_param(&ah, "recognition-timeout", 10000);
			switch_core_asr_numeric_param(&ah, "confidence-threshold", 500);
			switch_core_asr_numeric_param(&ah, "n-best-list-length", 3);
			switch_core_asr_float_param(&ah, "confidence-threshold", 0.5);

			/* nothing recognized yet */
			fst_check(switch_core_asr_check_results(&ah, &flags) != SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_asr_get_results(&ah, &xmlstr, &flags) != SWITCH_STATUS_SUCCESS);
			fst_check(xmlstr == NULL);
			fst_check(switch_core_asr_get_result_headers(&ah, &headers, &flags) != SWITCH_STATUS_SUCCESS);
			fst_check(headers == NULL);

			fst_requires(lv_close(&ah) == SWITCH_STATUS_SUCCESS);
			}
		}
		FST_TEST_END()

		/* Feeding audio before recognition has started is a no-op that succeeds
		 * (the interface must tolerate early frames). Live-only: needs an open
		 * handle. */
		FST_TEST_BEGIN(asr_feed_before_start)
		{
			if (lv_live()) {
			switch_asr_handle_t ah = { 0 };
			switch_asr_flag_t flags = SWITCH_ASR_FLAG_NONE;
			int16_t frame[160];                 /* 20 ms @ 8 kHz mono L16 */
			memset(frame, 0, sizeof(frame));

			fst_requires(lv_open(&ah, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_asr_load_grammar(&ah, "builtin:digits", "g1") == SWITCH_STATUS_SUCCESS);

			fst_check(switch_core_asr_feed(&ah, frame, sizeof(frame), &flags) == SWITCH_STATUS_SUCCESS);

			fst_requires(lv_close(&ah) == SWITCH_STATUS_SUCCESS);
			}
		}
		FST_TEST_END()

		/* With no recognition in flight the guarded operations behave
		 * deterministically regardless of what is provisioned server-side:
		 * resume() with no enabled grammar fails locally, DTMF has no
		 * interaction to target, and the local-only operations (timers,
		 * cancel, pause) succeed. Live-only: needs an open handle. */
		FST_TEST_BEGIN(asr_guards_without_recognition)
		{
			if (lv_live()) {
			switch_asr_handle_t ah = { 0 };
			switch_asr_flag_t flags = SWITCH_ASR_FLAG_NONE;
			switch_dtmf_t dtmf = { 0 };
			dtmf.digit = '1';
			dtmf.duration = 2000;

			fst_requires(lv_open(&ah, fst_pool) == SWITCH_STATUS_SUCCESS);

			/* no grammar loaded/enabled -> recognition start fails cleanly */
			fst_check(switch_core_asr_resume(&ah) != SWITCH_STATUS_SUCCESS);

			/* no active interaction -> DTMF injection fails cleanly */
			fst_check(switch_core_asr_feed_dtmf(&ah, &dtmf, &flags) != SWITCH_STATUS_SUCCESS);

			/* local-only operations still succeed */
			fst_check(switch_core_asr_start_input_timers(&ah) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_asr_try_cancel(&ah) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_asr_pause(&ah) == SWITCH_STATUS_SUCCESS);

			fst_requires(lv_close(&ah) == SWITCH_STATUS_SUCCESS);
			}
		}
		FST_TEST_END()

		/* pause -> resume -> pause cycles on one handle must never crash: they
		 * used to overwrite a still-joinable writer std::thread, which calls
		 * std::terminate(). resume()'s status is deliberately not asserted --
		 * it succeeds only where the ASR engine is provisioned -- the test is
		 * that the lifecycle is safe either way. Live-only. */
		FST_TEST_BEGIN(asr_pause_resume_cycle)
		{
			if (lv_live()) {
			int i;
			switch_asr_handle_t ah = { 0 };

			fst_requires(lv_open(&ah, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_asr_load_grammar(&ah, "builtin:digits", "g1") == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_asr_enable_grammar(&ah, "g1") == SWITCH_STATUS_SUCCESS);

			for (i = 0; i < 2; i++) {
				switch_core_asr_resume(&ah);
				fst_check(switch_core_asr_pause(&ah) == SWITCH_STATUS_SUCCESS);
			}

			fst_requires(lv_close(&ah) == SWITCH_STATUS_SUCCESS);
			}
		}
		FST_TEST_END()

		/* Repeated open/close cycles must not crash or leak handles/threads.
		 * Live-only: needs an open handle. */
		FST_TEST_BEGIN(asr_lifecycle_repeat)
		{
			if (lv_live()) {
			int i;
			for (i = 0; i < 3; i++) {
				switch_asr_handle_t ah = { 0 };
				fst_requires(lv_open(&ah, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_check(switch_core_asr_load_grammar(&ah, "builtin:digits", "g1") == SWITCH_STATUS_SUCCESS);
				fst_requires(lv_close(&ah) == SWITCH_STATUS_SUCCESS);
			}
			}
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
