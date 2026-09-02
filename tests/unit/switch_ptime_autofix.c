/*
 * TELCORE-363: tests for switch_core_media_codec_ptime_autofix_ok().
 * The PTIME autofix must be disallowed for codecs whose RTP clock differs
 * from the actual sample rate (G722: 8 kHz clock / 16 kHz samples), and
 * stay allowed for matching-clock codecs (PCMU/PCMA/OPUS).
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

			/* G722: 8 kHz RTP clock, 16 kHz sample rate (RFC 3551 4.5.2) */
			fst_check_int_equals(codec.implementation->samples_per_second, 8000);
			fst_check_int_equals(codec.implementation->actual_samples_per_second, 16000);

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

			fst_xcheck(switch_core_media_codec_ptime_autofix_ok(NULL) == SWITCH_FALSE,
					   "NULL implementation must be rejected");

			fst_xcheck(switch_core_media_codec_ptime_autofix_ok(&zeroed) == SWITCH_FALSE,
					   "zeroed implementation must be rejected");

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
