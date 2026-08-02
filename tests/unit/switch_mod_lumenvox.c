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

			/* Keep this test about bookkeeping only. By default (matching
			 * mod_unimrcp) loading a grammar disables the others and starts
			 * recognition immediately, which would make every assertion below
			 * depend on the ASR engine being provisioned server-side. */
			switch_core_asr_text_param(&ah, "start-recognize", "false");

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

			/* The core only ever routes params through asr_text_param, so the
			 * numeric/float params must also be reachable as text. Both
			 * confidence-threshold spellings are exercised: integer (0-1000)
			 * and float (0.0-1.0). */
			switch_core_asr_text_param(&ah, "no-input-timeout", "5000");
			switch_core_asr_text_param(&ah, "recognition-timeout", "10000");
			switch_core_asr_text_param(&ah, "speech-complete-timeout", "2000");
			switch_core_asr_text_param(&ah, "n-best-list-length", "3");
			switch_core_asr_text_param(&ah, "confidence-threshold", "500");
			switch_core_asr_text_param(&ah, "confidence-threshold", "0.5");

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

		/* builtin:special/cpa and /amd select CPA and AMD interactions instead
		 * of a grammar recognition. The classification is case-insensitive and
		 * tolerates a ?query suffix. Asserted through load+enable bookkeeping
		 * with recognition suppressed, so this does not depend on CPA being
		 * provisioned. Live-only: needs an open handle. */
		FST_TEST_BEGIN(asr_cpa_amd_grammar_classification)
		{
			if (lv_live()) {
			switch_asr_handle_t ah = { 0 };
			fst_requires(lv_open(&ah, fst_pool) == SWITCH_STATUS_SUCCESS);
			switch_core_asr_text_param(&ah, "start-recognize", "false");

			fst_check(switch_core_asr_load_grammar(&ah, "builtin:special/cpa", "cpa") == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_asr_load_grammar(&ah, "BUILTIN:SPECIAL/AMD", "amd") == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_asr_load_grammar(&ah, "builtin:special/cpa?x=1", "cpaq") == SWITCH_STATUS_SUCCESS);

			fst_check(switch_core_asr_enable_grammar(&ah, "cpa") == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_asr_disable_grammar(&ah, "cpa") == SWITCH_STATUS_SUCCESS);

			fst_requires(lv_close(&ah) == SWITCH_STATUS_SUCCESS);
			}
		}
		FST_TEST_END()

		/* CPA and AMD cannot run in one recognition: lv_session tracks a single
		 * interaction. The start must fail cleanly rather than picking one
		 * arbitrarily or crashing. This is stricter than mod_unimrcp, which
		 * would have passed both URIs to the MRCP server. Live-only. */
		FST_TEST_BEGIN(asr_mixed_cpa_amd_rejected)
		{
			if (lv_live()) {
			switch_asr_handle_t ah = { 0 };
			fst_requires(lv_open(&ah, fst_pool) == SWITCH_STATUS_SUCCESS);
			switch_core_asr_text_param(&ah, "start-recognize", "false");

			fst_check(switch_core_asr_load_grammar(&ah, "builtin:special/cpa", "cpa") == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_asr_load_grammar(&ah, "builtin:special/amd", "amd") == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_asr_enable_grammar(&ah, "cpa") == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_asr_enable_grammar(&ah, "amd") == SWITCH_STATUS_SUCCESS);

			fst_check(switch_core_asr_resume(&ah) != SWITCH_STATUS_SUCCESS);

			/* Disabling one of them makes the recognition startable again. */
			fst_check(switch_core_asr_disable_grammar(&ah, "amd") == SWITCH_STATUS_SUCCESS);

			fst_requires(lv_close(&ah) == SWITCH_STATUS_SUCCESS);
			}
		}
		FST_TEST_END()

		/* start-input-timers=false defers the no-input timer, and arming it
		 * makes the module enforce the timeout itself: the withheld timeout is
		 * not sent to the server, so the completion below is generated locally
		 * and does not depend on any engine being provisioned.
		 *
		 * Asserts the whole deferred-timer mechanism end to end: cause 002, the
		 * mod_unimrcp sentinel body, and the ASR-Completion-Cause header.
		 * Live-only: creating the interaction needs a reachable server. */
		FST_TEST_BEGIN(asr_deferred_no_input_fires)
		{
			if (lv_live()) {
			switch_asr_handle_t ah = { 0 };
			switch_asr_flag_t flags = SWITCH_ASR_FLAG_NONE;
			int16_t frame[160];                 /* 20 ms @ 8 kHz mono L16 */
			char *xmlstr = NULL;
			switch_event_t *headers = NULL;
			int i, got = 0;

			memset(frame, 0, sizeof(frame));
			fst_requires(lv_open(&ah, fst_pool) == SWITCH_STATUS_SUCCESS);
			switch_core_asr_text_param(&ah, "start-input-timers", "false");
			switch_core_asr_text_param(&ah, "no-input-timeout", "300");

			fst_check(switch_core_asr_load_grammar(&ah, "builtin:special/cpa", "cpa") == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_asr_start_input_timers(&ah) == SWITCH_STATUS_SUCCESS);

			/* The deadline is evaluated from check_results, which the core
			 * drives from the media bug on every frame. Drive it the same way. */
			for (i = 0; i < 100 && !got; i++) {
				switch_core_asr_feed(&ah, frame, sizeof(frame), &flags);
				if (switch_core_asr_check_results(&ah, &flags) == SWITCH_STATUS_SUCCESS) {
					got = 1;
					break;
				}
				switch_sleep(20000);
			}
			fst_check(got == 1);

			if (got) {
				fst_check(switch_core_asr_get_results(&ah, &xmlstr, &flags) == SWITCH_STATUS_SUCCESS);
				fst_check(xmlstr != NULL);
				if (xmlstr) {
					fst_check(strstr(xmlstr, "Completion-Cause: 002") != NULL);
					switch_safe_free(xmlstr);
				}
				fst_check(switch_core_asr_get_result_headers(&ah, &headers, &flags) == SWITCH_STATUS_SUCCESS);
				if (headers) {
					const char *cause = switch_event_get_header(headers, "ASR-Completion-Cause");
					fst_check(cause != NULL && !strcmp(cause, "002"));
					switch_event_destroy(&headers);
				}
			}

			fst_requires(lv_close(&ah) == SWITCH_STATUS_SUCCESS);
			}
		}
		FST_TEST_END()

		/* The deferred timeout must survive a restart. Withholding it from the
		 * create request must not consume the configured value, or the second
		 * interaction silently defers nothing -- which is exactly the shape of
		 * a sequential CPA-then-AMD run, the documented way to use both.
		 * Live-only. */
		FST_TEST_BEGIN(asr_deferred_no_input_survives_restart)
		{
			if (lv_live()) {
			switch_asr_handle_t ah = { 0 };
			switch_asr_flag_t flags = SWITCH_ASR_FLAG_NONE;
			int16_t frame[160];
			char *xmlstr = NULL;
			int round, i, got;

			memset(frame, 0, sizeof(frame));
			fst_requires(lv_open(&ah, fst_pool) == SWITCH_STATUS_SUCCESS);
			switch_core_asr_text_param(&ah, "start-input-timers", "false");
			switch_core_asr_text_param(&ah, "no-input-timeout", "300");

			for (round = 0; round < 2; round++) {
				got = 0;
				fst_check(switch_core_asr_load_grammar(&ah, "builtin:special/cpa", "cpa") == SWITCH_STATUS_SUCCESS);
				fst_check(switch_core_asr_start_input_timers(&ah) == SWITCH_STATUS_SUCCESS);

				for (i = 0; i < 100 && !got; i++) {
					switch_core_asr_feed(&ah, frame, sizeof(frame), &flags);
					if (switch_core_asr_check_results(&ah, &flags) == SWITCH_STATUS_SUCCESS) {
						got = 1;
						break;
					}
					switch_sleep(20000);
				}
				/* The second round is the regression: it fails if the first
				 * consumed the configured timeout. */
				fst_check(got == 1);
				if (got && switch_core_asr_get_results(&ah, &xmlstr, &flags) == SWITCH_STATUS_SUCCESS) {
					switch_safe_free(xmlstr);
				}
				fst_check(switch_core_asr_pause(&ah) == SWITCH_STATUS_SUCCESS);
			}

			fst_requires(lv_close(&ah) == SWITCH_STATUS_SUCCESS);
			}
		}
		FST_TEST_END()

		/* Pausing disarms the deadline. check_results keeps being called while
		 * paused, so an armed deadline would otherwise expire and deliver a
		 * no-input for a recognition that is no longer running. Live-only. */
		FST_TEST_BEGIN(asr_paused_deadline_does_not_fire)
		{
			if (lv_live()) {
			switch_asr_handle_t ah = { 0 };
			switch_asr_flag_t flags = SWITCH_ASR_FLAG_NONE;
			int i;

			fst_requires(lv_open(&ah, fst_pool) == SWITCH_STATUS_SUCCESS);
			switch_core_asr_text_param(&ah, "start-input-timers", "false");
			switch_core_asr_text_param(&ah, "no-input-timeout", "200");

			fst_check(switch_core_asr_load_grammar(&ah, "builtin:special/cpa", "cpa") == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_asr_start_input_timers(&ah) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_asr_pause(&ah) == SWITCH_STATUS_SUCCESS);

			/* Well past the deadline, nothing may be delivered. */
			for (i = 0; i < 30; i++) {
				fst_check(switch_core_asr_check_results(&ah, &flags) != SWITCH_STATUS_SUCCESS);
				switch_sleep(20000);
			}

			fst_requires(lv_close(&ah) == SWITCH_STATUS_SUCCESS);
			}
		}
		FST_TEST_END()

		/* Arming the timer after start-of-input has fired is a no-op, mirroring
		 * mod_unimrcp: a late start-input-timers must not kill a recognition
		 * that is already underway. With no recognition in flight the call is
		 * simply harmless, which is what is asserted offline-safely here.
		 * Live-only: needs an open handle. */
		FST_TEST_BEGIN(asr_start_input_timers_is_idempotent)
		{
			if (lv_live()) {
			switch_asr_handle_t ah = { 0 };
			switch_asr_flag_t flags = SWITCH_ASR_FLAG_NONE;

			fst_requires(lv_open(&ah, fst_pool) == SWITCH_STATUS_SUCCESS);
			switch_core_asr_text_param(&ah, "start-input-timers", "false");
			switch_core_asr_text_param(&ah, "no-input-timeout", "5000");
			fst_check(switch_core_asr_load_grammar(&ah, "builtin:special/cpa", "cpa") == SWITCH_STATUS_SUCCESS);

			/* Repeated arming must not re-arm or otherwise disturb the state. */
			fst_check(switch_core_asr_start_input_timers(&ah) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_asr_start_input_timers(&ah) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_asr_start_input_timers(&ah) == SWITCH_STATUS_SUCCESS);

			/* Nowhere near the 5 s deadline, so nothing may be delivered yet. */
			fst_check(switch_core_asr_check_results(&ah, &flags) != SWITCH_STATUS_SUCCESS);

			fst_requires(lv_close(&ah) == SWITCH_STATUS_SUCCESS);
			}
		}
		FST_TEST_END()

		/* End-to-end CPA classification through the module: feed real speech
		 * and assert the deployment classifies it, with the NLSML and the
		 * completion headers arriving the way a dialplan would see them.
		 *
		 * Needs a speech sample, so it is gated on LV_TEST_AUDIO (raw 8 kHz
		 * mono S16LE) as well as LV_TEST_TARGET -- no suitable audio ships in
		 * the tree. Generate one with any TTS, e.g. AWS Polly with
		 * OutputFormat=pcm and SampleRate=8000.
		 *
		 * Note what CPA does and does not tell you: it returns HUMAN RESIDENCE,
		 * HUMAN BUSINESS, UNKNOWN SPEECH or UNKNOWN SILENCE, and *every* one of
		 * them is completion cause 000. There is no MACHINE classification --
		 * an answering machine surfaces as UNKNOWN SPEECH, which is
		 * indistinguishable from a human who talks past human-business-time.
		 * So the assertion here is on the transcript, never on the cause. */
		FST_TEST_BEGIN(asr_cpa_classifies_speech)
		{
			const char *audio_path = getenv("LV_TEST_AUDIO");
			if (lv_live() && audio_path && *audio_path) {
			switch_asr_handle_t ah = { 0 };
			switch_asr_flag_t flags = SWITCH_ASR_FLAG_NONE;
			int16_t frame[160];                 /* 20 ms @ 8 kHz mono L16 */
			char *xmlstr = NULL;
			switch_event_t *headers = NULL;
			FILE *f = fopen(audio_path, "rb");
			int got = 0, i;
			size_t n;

			fst_requires(f != NULL);
			fst_requires(lv_open(&ah, fst_pool) == SWITCH_STATUS_SUCCESS);

			/* Exercise the CPA param surface on the way through, so a field
			 * the server rejects shows up as a failure to classify rather than
			 * passing unnoticed -- a rejected request is only a warning now.
			 * speech-complete-timeout is VadSettings.eos_delay_ms, which the
			 * Call Control API drives as after_greeting_silence_millis. */
			switch_core_asr_text_param(&ah, "cpa-human-business-time", "4000");
			switch_core_asr_text_param(&ah, "speech-complete-timeout", "1200");

			/* start-recognize defaults true, so loading starts the CPA
			 * interaction immediately -- the production shape. */
			fst_check(switch_core_asr_load_grammar(&ah, "builtin:special/cpa", "cpa") == SWITCH_STATUS_SUCCESS);

			/* Feed at roughly twice real time. The classifier measures speech
			 * in audio duration, not wall clock, but pacing keeps the audio
			 * queue well inside its bound. */
			while ((n = fread(frame, 1, sizeof(frame), f)) > 0 && !got) {
				if (switch_core_asr_feed(&ah, frame, (unsigned int) n, &flags) != SWITCH_STATUS_SUCCESS) {
					break;
				}
				if (switch_core_asr_check_results(&ah, &flags) == SWITCH_STATUS_SUCCESS) {
					got = 1;
					break;
				}
				switch_sleep(10000);
			}
			fclose(f);

			/* Audio may run out before the classifier commits; keep polling. */
			memset(frame, 0, sizeof(frame));
			for (i = 0; i < 400 && !got; i++) {
				switch_core_asr_feed(&ah, frame, sizeof(frame), &flags);
				if (switch_core_asr_check_results(&ah, &flags) == SWITCH_STATUS_SUCCESS) {
					got = 1;
					break;
				}
				switch_sleep(10000);
			}

			fst_check(got == 1);
			if (got) {
				fst_check(switch_core_asr_get_results(&ah, &xmlstr, &flags) == SWITCH_STATUS_SUCCESS);
				fst_check(xmlstr != NULL);
				if (xmlstr) {
					/* A real classification, not the no-result sentinel. */
					fst_check(strstr(xmlstr, "Completion-Cause:") == NULL);
					fst_check(strstr(xmlstr, "<interpretation") != NULL);
					fst_check(strstr(xmlstr, "HUMAN") != NULL || strstr(xmlstr, "UNKNOWN") != NULL);
					switch_safe_free(xmlstr);
				}
				fst_check(switch_core_asr_get_result_headers(&ah, &headers, &flags) == SWITCH_STATUS_SUCCESS);
				if (headers) {
					const char *cause = switch_event_get_header(headers, "ASR-Completion-Cause");
					fst_check(cause != NULL && !strcmp(cause, "000"));
					switch_event_destroy(&headers);
				}
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
