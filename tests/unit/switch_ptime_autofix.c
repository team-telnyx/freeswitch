/*
 * TELCORE-363: choppy outbound G722 audio despite good MOS.
 *
 * Root cause
 * ----------
 * The SCMF_AUTOFIX_TIMING "broken PTIME" autofix in
 * switch_core_media_read_frame() derives the remote ptime from RTP timestamp
 * deltas divided by implementation->samples_per_second. For G722 the RTP
 * clock (8 kHz, RFC 3551 section 4.5.2) differs from the actual sampling rate
 * (16 kHz), so timestamp artifacts (elastic jitter buffer PLC/acceleration,
 * recording resampler) make a clean 20 ms stream look like 40 ms. The autofix
 * then rewrote codec_ms from the negotiated 20 ms row (spf=160) to the 40 ms
 * row (spf=320) and re-initialized the write timer, producing only a quarter
 * of the audio: 518 packets in ~38.7 s (~13 pps instead of 50 pps), heard as
 * choppy/"sped-up" audio while MOS stayed at 4.5 because no packet was lost.
 *
 * Fix under test
 * --------------
 * switch_core_media_codec_ptime_autofix_ok() only allows the autofix when the
 * codec's RTP clock rate equals its actual sampling rate. This test verifies
 * the guard's decision for real codec implementations:
 *
 *   - G722  (8000 RTP clock / 16000 actual)  -> autofix must be DISALLOWED
 *   - PCMU  (8000 / 8000)                    -> autofix stays allowed
 *   - PCMA  (8000 / 8000)                    -> autofix stays allowed
 *   - OPUS  (48000 / 48000)                  -> autofix stays allowed
 *   - NULL / zeroed implementations          -> disallowed (defensive)
 */

#include <switch.h>
#include <test/switch_test.h>

FST_CORE_BEGIN("./conf")
{
	FST_SUITE_BEGIN(switch_ptime_autofix)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_spandsp");
			fst_requires_module("mod_opus");
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		FST_TEST_BEGIN(test_g722_autofix_disallowed)
		{
			switch_codec_t codec = { 0 };
			switch_codec_settings_t codec_settings = {{ 0 }};
			switch_status_t status;

			status = switch_core_codec_init(&codec,
											"G722",
											"mod_spandsp",
											NULL,
											8000,
											20,
											1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
											&codec_settings, fst_pool);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_requires(codec.implementation != NULL);

			/* Confirm the RFC 3551 quirk this guard exists for: the RTP clock
			 * and the actual sampling rate must differ for G722. */
			fst_check_int_equals(codec.implementation->samples_per_second, 8000);
			fst_check_int_equals(codec.implementation->actual_samples_per_second, 16000);

			/* The CBR ptime autofix must never run for G722: its timestamp
			 * math divides by the wrong clock and rewrites 20 ms -> 40 ms,
			 * quartering the produced audio (TELCORE-363). */
			fst_xcheck(switch_core_media_codec_ptime_autofix_ok(codec.implementation) == SWITCH_FALSE,
					   "G722 (8k RTP clock / 16k sample rate) must be excluded from the ptime autofix");

			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_pcmu_pcma_autofix_allowed)
		{
			switch_codec_t codec = { 0 };
			switch_codec_settings_t codec_settings = {{ 0 }};
			switch_status_t status;
			const char *names[] = { "PCMU", "PCMA" };
			int i;

			for (i = 0; i < 2; i++) {
				memset(&codec, 0, sizeof(codec));

				status = switch_core_codec_init(&codec,
												names[i],
												NULL,
												NULL,
												8000,
												20,
												1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
												&codec_settings, fst_pool);
				fst_requires(status == SWITCH_STATUS_SUCCESS);
				fst_requires(codec.implementation != NULL);

				fst_check_int_equals(codec.implementation->samples_per_second,
									 codec.implementation->actual_samples_per_second);

				/* Legacy behaviour must be preserved for normal codecs: the
				 * autofix still protects against genuinely broken remote
				 * ptime senders. */
				fst_xcheck(switch_core_media_codec_ptime_autofix_ok(codec.implementation) == SWITCH_TRUE,
						   "codec with matching RTP/sample clocks must keep the ptime autofix enabled");

				switch_core_codec_destroy(&codec);
			}
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_opus_autofix_allowed)
		{
			switch_codec_t codec = { 0 };
			switch_codec_settings_t codec_settings = {{ 0 }};
			switch_status_t status;

			status = switch_core_codec_init(&codec,
											"OPUS",
											"mod_opus",
											NULL,
											48000,
											20,
											1, SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
											&codec_settings, fst_pool);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_requires(codec.implementation != NULL);

			fst_check_int_equals(codec.implementation->samples_per_second,
								 codec.implementation->actual_samples_per_second);

			fst_xcheck(switch_core_media_codec_ptime_autofix_ok(codec.implementation) == SWITCH_TRUE,
					   "OPUS (48k/48k) must keep the ptime autofix enabled");

			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_defensive_inputs)
		{
			switch_codec_implementation_t zeroed = { 0 };
			switch_codec_implementation_t partial = { 0 };

			/* NULL implementation must never allow the autofix. */
			fst_xcheck(switch_core_media_codec_ptime_autofix_ok(NULL) == SWITCH_FALSE,
					   "NULL implementation must be rejected");

			/* Fully zeroed implementation (no clock info) must be rejected. */
			fst_xcheck(switch_core_media_codec_ptime_autofix_ok(&zeroed) == SWITCH_FALSE,
					   "zeroed implementation must be rejected");

			/* Missing actual_samples_per_second must be rejected rather than
			 * treated as matching. */
			partial.samples_per_second = 8000;
			partial.actual_samples_per_second = 0;
			fst_xcheck(switch_core_media_codec_ptime_autofix_ok(&partial) == SWITCH_FALSE,
					   "implementation without actual sample rate must be rejected");
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
