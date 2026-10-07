/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2018, Anthony Minessale II <anthm@freeswitch.org>
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
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * switch_vbr_ptime.c -- switch_core_codec_ptime_supported() against real codec selection
 *
 */
#include <switch.h>
#include <stdlib.h>

#include <test/switch_test.h>

#define CODEC_FLAGS (SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE)

static int matcher_calls = 0;
static switch_core_codec_matches_fmtp_func_t real_matcher = NULL;

static switch_status_t counting_matcher(const char *fmtp, const char *codec_fmtp)
{
	matcher_calls++;
	return real_matcher ? real_matcher(fmtp, codec_fmtp) : SWITCH_STATUS_SUCCESS;
}

static switch_status_t open_codec(switch_codec_t *codec, const char *name, const char *modname, const char *fmtp,
								  uint32_t rate, int ms, int channels, switch_memory_pool_t *pool)
{
	switch_codec_settings_t settings = {{ 0 }};

	memset(codec, 0, sizeof(*codec));
	return switch_core_codec_init_with_bitrate(codec, name, modname, fmtp, rate, ms, channels, 0, CODEC_FLAGS, &settings, pool);
}

/* For every ms from 1 to 120 the helper must agree with whether a real init of that request succeeds. */
static int mismatches_with_init(switch_codec_t *running, const char *name, const char *modname, const char *fmtp,
								uint32_t rate, int channels, switch_memory_pool_t *pool)
{
	int ms, bad = 0;

	for (ms = 1; ms <= 120; ms++) {
		switch_codec_t probe;
		switch_bool_t supported = switch_core_codec_ptime_supported(running, name, modname, fmtp, rate, ms, channels, 0);
		switch_bool_t loads = open_codec(&probe, name, modname, fmtp, rate, ms, channels, pool) == SWITCH_STATUS_SUCCESS;

		if (loads) {
			switch_core_codec_destroy(&probe);
		}

		if (supported != loads) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s %d ms: supported=%d loads=%d\n", name, ms, supported, loads);
			bad++;
		}
	}

	return bad;
}

FST_CORE_BEGIN("./conf")
{
	FST_SUITE_BEGIN(switch_vbr_ptime)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_amrwb");
			fst_requires_module("mod_amr");
			fst_requires_module("mod_opus");
			fst_requires_module("mod_spandsp");
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		FST_TEST_BEGIN(amrwb_only_its_running_ptime)
		{
			const char *fmtps[] = { "octet-align=0; mode-set=0,1,2; max-red=0; mode-change-capability=2", "octet-align=1", NULL };
			int i, ms;

			for (i = 0; fmtps[i]; i++) {
				switch_codec_t codec;

				fst_requires(open_codec(&codec, "AMR-WB", "mod_amrwb", fmtps[i], 16000, 20, 1, fst_pool) == SWITCH_STATUS_SUCCESS);
				fst_check(switch_core_codec_ptime_supported(&codec, "AMR-WB", "mod_amrwb", fmtps[i], 16000, 20, 1, 0));
				for (ms = 40; ms <= 120; ms += 20) {
					fst_check(!switch_core_codec_ptime_supported(&codec, "AMR-WB", "mod_amrwb", fmtps[i], 16000, ms, 1, 0));
				}
				fst_check(mismatches_with_init(&codec, "AMR-WB", "mod_amrwb", fmtps[i], 16000, 1, fst_pool) == 0);
				switch_core_codec_destroy(&codec);
			}
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amr_only_its_running_ptime)
		{
			switch_codec_t codec;

			fst_requires(open_codec(&codec, "AMR", "mod_amr", "octet-align=0", 8000, 20, 1, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_codec_ptime_supported(&codec, "AMR", "mod_amr", "octet-align=0", 8000, 20, 1, 0));
			fst_check(!switch_core_codec_ptime_supported(&codec, "AMR", "mod_amr", "octet-align=0", 8000, 40, 1, 0));
			fst_check(mismatches_with_init(&codec, "AMR", "mod_amr", "octet-align=0", 8000, 1, fst_pool) == 0);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(opus_keeps_its_supported_ptime_change)
		{
			const char *fmtp = "useinbandfec=1; maxaveragebitrate=30000; maxplaybackrate=48000; ptime=20; minptime=10; maxptime=40";
			switch_codec_t codec;

			fst_requires(open_codec(&codec, "opus", "mod_opus", fmtp, 48000, 20, 1, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_codec_ptime_supported(&codec, "opus", "mod_opus", fmtp, 48000, 40, 1, 0));
			fst_check(!switch_core_codec_ptime_supported(&codec, "opus", "mod_opus", fmtp, 48000, 120, 1, 0));
			fst_check(mismatches_with_init(&codec, "opus", "mod_opus", fmtp, 48000, 1, fst_pool) == 0);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(opus_fmtp_decides_like_init)
		{
			const char *fmtp = "useinbandfec=1; ptime=20; minptime=20; maxptime=20";
			switch_codec_t codec;

			fst_requires(open_codec(&codec, "opus", "mod_opus", fmtp, 48000, 20, 1, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(!switch_core_codec_ptime_supported(&codec, "opus", "mod_opus", fmtp, 48000, 40, 1, 0));
			fst_check(mismatches_with_init(&codec, "opus", "mod_opus", fmtp, 48000, 1, fst_pool) == 0);
			fst_check(mismatches_with_init(&codec, "opus", "mod_opus", fmtp, 48000, 2, fst_pool) == 0);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(amrwb_rejection_never_reaches_the_fmtp_matcher)
		{
			const char *fmtp = "octet-align=0; mode-set=0,1,2";
			switch_codec_t codec;
			switch_codec_implementation_t *iptr;
			switch_core_codec_matches_fmtp_func_t saved[16] = { 0 };
			int n = 0, i, round, ms, ready = 1;

			fst_requires(open_codec(&codec, "AMR-WB", "mod_amrwb", fmtp, 16000, 20, 1, fst_pool) == SWITCH_STATUS_SUCCESS);

			for (iptr = codec.codec_interface->implementations; iptr; iptr = iptr->next) {
				if (n == 16 || !iptr->matches_fmtp) {
					ready = 0;
				}
				n++;
			}
			fst_check(n > 0 && n <= 16);
			fst_check(ready);

			if (ready) {
				for (i = 0, iptr = codec.codec_interface->implementations; iptr; iptr = iptr->next, i++) {
					saved[i] = iptr->matches_fmtp;
				}
				real_matcher = saved[0];
				for (iptr = codec.codec_interface->implementations; iptr; iptr = iptr->next) {
					iptr->matches_fmtp = counting_matcher;
				}

				matcher_calls = 0;
				for (round = 0; round < 100; round++) {
					for (ms = 40; ms <= 120; ms += 20) {
						fst_check(!switch_core_codec_ptime_supported(&codec, "AMR-WB", "mod_amrwb", fmtp, 16000, ms, 1, 0));
					}
				}
				fst_check(matcher_calls == 0);

				fst_check(switch_core_codec_ptime_supported(&codec, "AMR-WB", "mod_amrwb", fmtp, 16000, 20, 1, 0));
				fst_check(matcher_calls > 0);

				for (i = 0, iptr = codec.codec_interface->implementations; iptr; iptr = iptr->next, i++) {
					iptr->matches_fmtp = saved[i];
				}
				real_matcher = NULL;
			}

			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(bitrate_and_g722_follow_the_selector)
		{
			switch_codec_t codec;

			fst_requires(open_codec(&codec, "AMR-WB", "mod_amrwb", "octet-align=0", 16000, 20, 1, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_codec_ptime_supported(&codec, "AMR-WB", "mod_amrwb", "octet-align=0", 16000, 20, 1,
														(uint32_t) codec.implementation->bits_per_second));
			fst_check(!switch_core_codec_ptime_supported(&codec, "AMR-WB", "mod_amrwb", "octet-align=0", 16000, 20, 1,
														 (uint32_t) codec.implementation->bits_per_second + 1));
			switch_core_codec_destroy(&codec);

			fst_requires(open_codec(&codec, "G722", "mod_spandsp", NULL, 8000, 20, 1, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_codec_ptime_supported(&codec, "G722", "mod_spandsp", NULL, 8000, 40, 1, 0));
			fst_check(!switch_core_codec_ptime_supported(&codec, "G722", "mod_spandsp", NULL, 16000, 40, 1, 0));
			fst_check(mismatches_with_init(&codec, "G722", "mod_spandsp", NULL, 8000, 1, fst_pool) == 0);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(running_rate_must_match_the_request)
		{
			const char *fmtp = "useinbandfec=1; ptime=20; minptime=10; maxptime=40";
			switch_codec_t codec, other;

			fst_requires(open_codec(&other, "opus", "mod_opus", fmtp, 16000, 20, 1, fst_pool) == SWITCH_STATUS_SUCCESS);
			switch_core_codec_destroy(&other);
			fst_requires(open_codec(&codec, "opus", "mod_opus", fmtp, 48000, 20, 1, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(!switch_core_codec_ptime_supported(&codec, "opus", "mod_opus", fmtp, 16000, 20, 1, 0));
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(request_must_describe_the_running_codec)
		{
			switch_codec_t codec;

			fst_check(!switch_core_codec_ptime_supported(NULL, "AMR-WB", "mod_amrwb", NULL, 16000, 20, 1, 0));
			fst_requires(open_codec(&codec, "AMR-WB", "mod_amrwb", "octet-align=0", 16000, 20, 1, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_check(switch_core_codec_ptime_supported(&codec, "AMR-WB", NULL, "octet-align=0", 16000, 20, 1, 0));
			fst_check(!switch_core_codec_ptime_supported(&codec, "opus", "mod_opus", NULL, 48000, 20, 1, 0));
			fst_check(!switch_core_codec_ptime_supported(&codec, "AMR-WB", "mod_opus", "octet-align=0", 16000, 20, 1, 0));
			fst_check(!switch_core_codec_ptime_supported(&codec, "mod_amrwb.AMR-WB", NULL, "octet-align=0", 16000, 20, 1, 0));
			fst_check(!switch_core_codec_ptime_supported(&codec, "AMR-WB", "mod_amrwb", "octet-align=0", 16000, 0, 1, 0));
			fst_check(!switch_core_codec_ptime_supported(&codec, "AMR-WB", "mod_amrwb", "octet-align=0", 8000, 20, 1, 0));
			fst_check(!switch_core_codec_ptime_supported(&codec, "AMR-WB", "mod_amrwb", "octet-align=0", 16000, 20, 2, 0));
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
