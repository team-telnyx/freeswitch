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
 * switch_vbr_ptime.c -- VBR ptime inference and switch_core_codec_ptime_supported() against real codec selection
 *
 */
#include <switch.h>
#include <stdlib.h>

#include <test/switch_test.h>
#include <private/switch_core_media_pvt.h>

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

/* A second provider of AMR-WB with a 40 ms implementation only, registered after mod_amrwb, so a lookup by name alone
 * finds it first while a codec opened from mod_amrwb keeps running. */
#define FAKE_AMRWB_FILE "switch_vbr_ptime_fake_amrwb"

static switch_status_t fake_amrwb_init(switch_codec_t *codec, switch_codec_flag_t flags, const switch_codec_settings_t *codec_settings)
{
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t fake_amrwb_destroy(switch_codec_t *codec)
{
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t fake_amrwb_load(switch_loadable_module_interface_t **module_interface, switch_memory_pool_t *pool)
{
	switch_codec_interface_t *codec_interface;

	*module_interface = switch_loadable_module_create_module_interface(pool, "mod_fake_amrwb");
	SWITCH_ADD_CODEC(codec_interface, "AMR-WB 40 ms");
	switch_core_codec_add_implementation(pool, codec_interface, SWITCH_CODEC_TYPE_AUDIO, 100, "AMR-WB", NULL,
										 16000, 16000, 23850, 40000, 640, 1280, 0, 1, 1,
										 fake_amrwb_init, NULL, NULL, fake_amrwb_destroy);

	return SWITCH_STATUS_SUCCESS;
}

typedef enum {
	TWO_PROVIDERS_RUN_ALL,
	TWO_PROVIDERS_STOP_AFTER_OPEN,
	TWO_PROVIDERS_STOP_AFTER_LOAD,
	TWO_PROVIDERS_STOP_AFTER_LOOKUP
} two_providers_stop_t;

#define CASE_CHECK(_cond) do { if (!(_cond)) { switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "check failed: %s\n", #_cond); bad++; } } while (0)

/* Open an AMR-WB codec from mod_amrwb, register the 40 ms provider and check the helper against both, as one
 * transaction: every acquisition is released on every path, so a failed check never leaves the provider registered,
 * the codec open or a lookup reference held. stop ends it right after that acquisition, as a failed check would.
 * Returns the number of failed checks. */
static int two_providers_case(two_providers_stop_t stop, switch_memory_pool_t *pool)
{
	switch_codec_t running;
	switch_codec_interface_t *amrwb = NULL, *fake = NULL;
	int opened = 0, loaded = 0, bad = 0, amrwb_refs = 0, fake_refs = 0;
	const char *err = NULL;

	if (switch_loadable_module_exists(FAKE_AMRWB_FILE) == SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s is already registered\n", FAKE_AMRWB_FILE);
		return 1;
	}

	if (open_codec(&running, "AMR-WB", "mod_amrwb", "octet-align=0", 16000, 20, 1, pool) != SWITCH_STATUS_SUCCESS) {
		return 1;
	}
	opened = 1;

	if (stop == TWO_PROVIDERS_STOP_AFTER_OPEN) {
		goto done;
	}

	if (switch_loadable_module_build_dynamic((char *) FAKE_AMRWB_FILE, fake_amrwb_load, NULL, NULL, SWITCH_FALSE) != SWITCH_STATUS_SUCCESS) {
		bad++;
		goto done;
	}
	loaded = 1;

	if (stop == TWO_PROVIDERS_STOP_AFTER_LOAD) {
		goto done;
	}

	amrwb = switch_loadable_module_get_codec_interface("AMR-WB", "mod_amrwb");
	fake = switch_loadable_module_get_codec_interface("AMR-WB", NULL);

	if (stop == TWO_PROVIDERS_STOP_AFTER_LOOKUP) {
		goto done;
	}

	CASE_CHECK(amrwb && fake && amrwb == running.codec_interface && fake != amrwb && !strcmp(fake->parent->module_name, "mod_fake_amrwb"));
	if (bad) {
		goto done;
	}
	amrwb_refs = amrwb->refs;
	fake_refs = fake->refs;

	/* no module named: the rebuild loads the newest provider, which has 40 ms and no 20 ms */
	CASE_CHECK(switch_core_codec_ptime_supported(&running, "AMR-WB", NULL, NULL, 16000, 40, 1, 0));
	CASE_CHECK(!switch_core_codec_ptime_supported(&running, "AMR-WB", NULL, NULL, 16000, 20, 1, 0));
	CASE_CHECK(mismatches_with_init(&running, "AMR-WB", NULL, NULL, 16000, 1, pool) == 0);

	/* the running module named: its own implementations decide */
	CASE_CHECK(switch_core_codec_ptime_supported(&running, "AMR-WB", "mod_amrwb", "octet-align=0", 16000, 20, 1, 0));
	CASE_CHECK(!switch_core_codec_ptime_supported(&running, "AMR-WB", "mod_amrwb", "octet-align=0", 16000, 40, 1, 0));
	CASE_CHECK(mismatches_with_init(&running, "AMR-WB", "mod_amrwb", "octet-align=0", 16000, 1, pool) == 0);

	/* the other provider named: not the running codec's module */
	CASE_CHECK(!switch_core_codec_ptime_supported(&running, "AMR-WB", "mod_fake_amrwb", NULL, 16000, 40, 1, 0));

	/* accepted and refused checks leave no reference behind */
	CASE_CHECK(amrwb->refs == amrwb_refs && fake->refs == fake_refs);

  done:
	UNPROTECT_INTERFACE(fake);
	UNPROTECT_INTERFACE(amrwb);
	if (opened) {
		switch_core_codec_destroy(&running);
	}
	if (loaded && switch_loadable_module_unload_module(NULL, FAKE_AMRWB_FILE, SWITCH_FALSE, &err) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "unloading %s failed: %s\n", FAKE_AMRWB_FILE, err ? err : "");
		bad++;
	}

	return bad;
}

/* The registry is as before the case: mod_amrwb is the only AMR-WB provider again and holds no reference of it. */
static int two_providers_restored(int amrwb_refs)
{
	switch_codec_interface_t *head = switch_loadable_module_get_codec_interface("AMR-WB", NULL);
	int ok = head && !strcmp(head->parent->module_name, "mod_amrwb") && head->refs == amrwb_refs + 1 &&
		switch_loadable_module_exists(FAKE_AMRWB_FILE) != SWITCH_STATUS_SUCCESS;

	UNPROTECT_INTERFACE(head);

	return ok;
}

/* Feed frames whose timestamps advance by the given intervals (ms at 16 kHz, one sequence step each) and return the
 * index of the first frame for which an adoption is proposed, or -1. *proposed receives the proposed ptime. */
static int feed(switch_vbr_ptime_state_t *st, uint32_t *ts, uint16_t *seq, const uint32_t *ms, int n, uint32_t cur_ms, uint32_t *proposed)
{
	int i;

	for (i = 0; i < n; i++) {
		uint32_t r;

		*ts += ms[i] * 16;
		(*seq)++;
		if (!*seq) {
			(*seq)++;
		}
		if ((r = switch_core_media_pvt_vbr_ptime_observe(NULL, st, *ts, *seq, 16000, cur_ms))) {
			if (proposed) {
				*proposed = r;
			}
			return i;
		}
	}

	return -1;
}

static void anchor(switch_vbr_ptime_state_t *st, uint32_t *ts, uint16_t *seq, uint32_t start_ts, uint16_t start_seq)
{
	memset(st, 0, sizeof(*st));
	*ts = start_ts;
	*seq = start_seq;
	switch_core_media_pvt_vbr_ptime_observe(NULL, st, *ts, *seq, 16000, 20);
}

typedef struct {
	switch_mutex_t *mutex;
	switch_codec_t *codec;
	switch_atomic_t holding;
	switch_atomic_t release;
	int destroy;
	int churn;
	switch_atomic_t *generation;
} lock_holder_t;

static void *SWITCH_THREAD_FUNC hold_codec_lock(switch_thread_t *thread, void *obj)
{
	lock_holder_t *h = (lock_holder_t *) obj;

	if (h->churn) {
		switch_atomic_set(&h->holding, 1);
		while (!switch_atomic_read(&h->release)) {
			switch_mutex_lock(h->mutex);
			switch_atomic_inc(h->generation);
			switch_mutex_unlock(h->mutex);
			switch_yield(200);
		}
		return NULL;
	}

	switch_mutex_lock(h->mutex);
	if (h->destroy) {
		switch_core_codec_destroy(h->codec);
	}
	if (h->generation) {
		switch_atomic_inc(h->generation);
	}
	switch_atomic_set(&h->holding, 1);
	while (!switch_atomic_read(&h->release)) {
		switch_yield(1000);
	}
	switch_mutex_unlock(h->mutex);

	return NULL;
}

static switch_thread_t *start_holder(lock_holder_t *h, switch_memory_pool_t *pool)
{
	switch_threadattr_t *attr = NULL;
	switch_thread_t *thread = NULL;
	int waited = 0;

	switch_threadattr_create(&attr, pool);
	if (switch_thread_create(&thread, attr, hold_codec_lock, h, pool) != SWITCH_STATUS_SUCCESS) {
		return NULL;
	}
	while (!switch_atomic_read(&h->holding) && waited++ < 5000) {
		switch_yield(1000);
	}

	return thread;
}

static void stop_holder(lock_holder_t *h, switch_thread_t *thread)
{
	switch_status_t st;

	switch_atomic_set(&h->release, 1);
	if (thread) {
		switch_thread_join(&st, thread);
	}
}

static void opus_payload_map(payload_map_t *pmap, const char *fmtp)
{
	memset(pmap, 0, sizeof(*pmap));
	pmap->iananame = (char *) "opus";
	pmap->rm_fmtp = (char *) fmtp;
	pmap->rm_rate = 48000;
	pmap->channels = 1;
	pmap->codec_ms = 20;
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

		FST_TEST_BEGIN(vbr_silence_run_does_not_prime_the_next_gap)
		{
			uint32_t run[11] = { 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 0 };
			uint32_t ts, gap;
			uint16_t seq;
			switch_vbr_ptime_state_t st;

			for (gap = 40; gap <= 120; gap += 20) {
				anchor(&st, &ts, &seq, 1000, 100);
				run[10] = gap;
				fst_check(feed(&st, &ts, &seq, run, 11, 20, NULL) == -1);
				fst_check(st.mismatch_count == 0);
			}
		}
		FST_TEST_END()

		FST_TEST_BEGIN(vbr_consecutive_change_is_still_adopted)
		{
			uint32_t speech[20], forty[7] = { 40, 40, 40, 40, 40, 40, 40 }, silence[8] = { 160, 160, 160, 160, 160, 160, 160, 160 };
			uint32_t ts, proposed = 0;
			uint16_t seq;
			switch_vbr_ptime_state_t st;
			int i;

			for (i = 0; i < 20; i++) speech[i] = 20;

			anchor(&st, &ts, &seq, 5000, 7);
			fst_check(feed(&st, &ts, &seq, speech, 20, 20, NULL) == -1);
			fst_check(feed(&st, &ts, &seq, forty, 7, 20, &proposed) == 6);
			fst_check(proposed == 40);

			anchor(&st, &ts, &seq, 5000, 7);
			fst_check(feed(&st, &ts, &seq, silence, 8, 20, NULL) == -1);
			proposed = 0;
			fst_check(feed(&st, &ts, &seq, forty, 7, 20, &proposed) == 6);
			fst_check(proposed == 40);

			anchor(&st, &ts, &seq, 5000, 7);
			fst_check(feed(&st, &ts, &seq, forty, 6, 20, NULL) == -1);
			fst_check(feed(&st, &ts, &seq, speech, 1, 20, NULL) == -1);
			fst_check(feed(&st, &ts, &seq, forty, 6, 20, NULL) == -1);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(vbr_alternating_and_oversized_gaps_never_count)
		{
			uint32_t alt[40], big[20];
			uint32_t ts;
			uint16_t seq;
			switch_vbr_ptime_state_t st;
			int i;

			for (i = 0; i < 40; i++) alt[i] = (i % 2) ? 60 : 40;
			for (i = 0; i < 20; i++) big[i] = 140;

			anchor(&st, &ts, &seq, 1, 1);
			fst_check(feed(&st, &ts, &seq, alt, 40, 20, NULL) == -1);
			fst_check(feed(&st, &ts, &seq, big, 20, 20, NULL) == -1);
			fst_check(st.mismatch_count <= 5);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(vbr_sequence_and_timestamp_edges)
		{
			switch_vbr_ptime_state_t st;
			uint32_t ts;
			uint16_t seq;
			int i;

			/* a lost packet halves the measured interval */
			anchor(&st, &ts, &seq, 1000, 10);
			fst_check(switch_core_media_pvt_vbr_ptime_observe(NULL, &st, 1000 + 1280, 12, 16000, 20) == 0);
			fst_check(st.last_codec_ms == 40);

			/* sequence wrap counts as one step */
			anchor(&st, &ts, &seq, 1000, 65535);
			fst_check(switch_core_media_pvt_vbr_ptime_observe(NULL, &st, 1000 + 640, 1, 16000, 20) == 0);
			fst_check(st.last_codec_ms == 20);

			/* sequence number 0 is an ordinary step, not a missing one */
			anchor(&st, &ts, &seq, 1000, 65535);
			fst_check(switch_core_media_pvt_vbr_ptime_observe(NULL, &st, 1000 + 320, 0, 16000, 20) == 0);
			fst_check(st.last_ts == 1000 + 320 && st.last_seq == 0 && st.last_codec_ms == 20);
			fst_check(switch_core_media_pvt_vbr_ptime_observe(NULL, &st, 1000 + 640, 1, 16000, 20) == 0);
			fst_check(st.last_seq == 1 && st.last_codec_ms == 20);

			/* timestamp wrap is a normal forward step */
			anchor(&st, &ts, &seq, 0xFFFFFF00u, 300);
			fst_check(switch_core_media_pvt_vbr_ptime_observe(NULL, &st, 0xFFFFFF00u + 320, 301, 16000, 20) == 0);
			fst_check(st.last_codec_ms == 20);

			/* backward timestamps and reordered sequence numbers measure nothing */
			anchor(&st, &ts, &seq, 50000, 500);
			for (i = 0; i < 10; i++) {
				fst_check(switch_core_media_pvt_vbr_ptime_observe(NULL, &st, 50000 - (uint32_t) (i + 1) * 640, (uint16_t) (501 + i), 16000, 20) == 0);
			}
			fst_check(st.mismatch_count == 0);
			anchor(&st, &ts, &seq, 50000, 500);
			fst_check(switch_core_media_pvt_vbr_ptime_observe(NULL, &st, 50640, 499, 16000, 20) == 0);
			fst_check(st.last_codec_ms == 0);

			/* missing timestamp or clock starts over */
			st.mismatch_count = 4;
			fst_check(switch_core_media_pvt_vbr_ptime_observe(NULL, &st, 0, 600, 16000, 20) == 0);
			fst_check(st.mismatch_count == 0 && st.last_ts == 0);
			fst_check(switch_core_media_pvt_vbr_ptime_observe(NULL, NULL, 1, 1, 16000, 20) == 0);

			/* the first frame after a restart is only an anchor */
			memset(&st, 0, sizeof(st));
			fst_check(switch_core_media_pvt_vbr_ptime_observe(NULL, &st, 123456, 77, 16000, 20) == 0);
			fst_check(st.last_ts == 123456 && st.last_seq == 77 && st.last_codec_ms == 0 && st.mismatch_count == 0);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(vbr_threshold_needs_seven_matching_intervals)
		{
			uint32_t forty[7] = { 40, 40, 40, 40, 40, 40, 40 };
			uint32_t ts;
			uint16_t seq;
			switch_vbr_ptime_state_t st;
			int i;

			anchor(&st, &ts, &seq, 9000, 900);
			for (i = 0; i < 6; i++) {
				fst_check(feed(&st, &ts, &seq, forty + i, 1, 20, NULL) == -1);
				fst_check(st.mismatch_count == (uint32_t) i);
			}
			fst_check(feed(&st, &ts, &seq, forty, 1, 20, NULL) == 0);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(adoption_never_waits_for_a_held_codec_lock)
		{
			const char *fmtp = "useinbandfec=1; ptime=20; minptime=10; maxptime=40";
			switch_codec_t codec;
			switch_mutex_t *mutex = NULL;
			payload_map_t pmap;
			lock_holder_t h = { 0 };
			switch_thread_t *thread;
			switch_time_t started;
			uint8_t reset = 0;

			switch_mutex_init(&mutex, SWITCH_MUTEX_NESTED, fst_pool);
			fst_requires(open_codec(&codec, "opus", "mod_opus", fmtp, 48000, 20, 1, fst_pool) == SWITCH_STATUS_SUCCESS);
			opus_payload_map(&pmap, fmtp);

			h.mutex = mutex;
			h.codec = &codec;
			thread = start_holder(&h, fst_pool);
			fst_check(thread != NULL && switch_atomic_read(&h.holding));

			started = switch_micro_time_now();
			fst_check(!switch_core_media_pvt_vbr_ptime_adopt(NULL, mutex, &codec, &pmap, 40, &reset, NULL, 0));
			fst_check(switch_micro_time_now() - started < 100000);
			fst_check(pmap.codec_ms == 20 && reset == 0 && !switch_test_flag(&codec, SWITCH_CODEC_FLAG_RESET_PENDING));

			stop_holder(&h, thread);

			fst_check(switch_core_media_pvt_vbr_ptime_adopt(NULL, mutex, &codec, &pmap, 40, &reset, NULL, 0));
			fst_check(pmap.codec_ms == 40 && reset == 2 && switch_test_flag(&codec, SWITCH_CODEC_FLAG_RESET_PENDING));
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(adoption_skips_a_codec_destroyed_under_the_lock)
		{
			const char *fmtp = "useinbandfec=1; ptime=20; minptime=10; maxptime=40";
			switch_codec_t codec;
			switch_mutex_t *mutex = NULL;
			payload_map_t pmap;
			lock_holder_t h = { 0 };
			switch_thread_t *thread;
			uint8_t reset = 0;

			switch_mutex_init(&mutex, SWITCH_MUTEX_NESTED, fst_pool);
			fst_requires(open_codec(&codec, "opus", "mod_opus", fmtp, 48000, 20, 1, fst_pool) == SWITCH_STATUS_SUCCESS);
			opus_payload_map(&pmap, fmtp);

			h.mutex = mutex;
			h.codec = &codec;
			h.destroy = 1;
			thread = start_holder(&h, fst_pool);
			fst_check(thread != NULL && switch_atomic_read(&h.holding));
			fst_check(!switch_core_media_pvt_vbr_ptime_adopt(NULL, mutex, &codec, &pmap, 40, &reset, NULL, 0));
			stop_holder(&h, thread);

			fst_check(!switch_core_media_pvt_vbr_ptime_adopt(NULL, mutex, &codec, &pmap, 40, &reset, NULL, 0));
			fst_check(pmap.codec_ms == 20 && reset == 0);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(adoption_refuses_evidence_from_before_a_codec_rebuild)
		{
			const char *fmtp = "useinbandfec=1; ptime=20; minptime=10; maxptime=40";
			switch_codec_t codec;
			switch_mutex_t *mutex = NULL;
			payload_map_t pmap;
			lock_holder_t h = { 0 };
			switch_thread_t *thread;
			switch_atomic_t generation;
			uint32_t observed;
			uint8_t reset = 0;

			switch_mutex_init(&mutex, SWITCH_MUTEX_NESTED, fst_pool);
			fst_requires(open_codec(&codec, "opus", "mod_opus", fmtp, 48000, 20, 1, fst_pool) == SWITCH_STATUS_SUCCESS);
			opus_payload_map(&pmap, fmtp);

			switch_atomic_set(&generation, 7);
			observed = switch_atomic_read(&generation);
			h.mutex = mutex;
			h.codec = &codec;
			h.generation = &generation;
			thread = start_holder(&h, fst_pool);
			fst_check(thread != NULL && switch_atomic_read(&h.holding));
			stop_holder(&h, thread);
			fst_check(switch_atomic_read(&generation) == observed + 1);

			fst_check(!switch_core_media_pvt_vbr_ptime_adopt(NULL, mutex, &codec, &pmap, 40, &reset, &generation, observed));
			fst_check(pmap.codec_ms == 20 && reset == 0 && !switch_test_flag(&codec, SWITCH_CODEC_FLAG_RESET_PENDING));

			fst_check(switch_core_media_pvt_vbr_ptime_adopt(NULL, mutex, &codec, &pmap, 40, &reset, &generation, switch_atomic_read(&generation)));
			fst_check(pmap.codec_ms == 40 && reset == 2);
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(adoption_never_uses_a_stale_generation_under_churn)
		{
			const char *fmtp = "useinbandfec=1; ptime=20; minptime=10; maxptime=40";
			switch_codec_t codec;
			switch_mutex_t *mutex = NULL;
			payload_map_t pmap;
			lock_holder_t h = { 0 };
			switch_thread_t *thread;
			switch_atomic_t generation;
			uint8_t reset = 0;
			int round, stale_checked = 0, waited;

			switch_mutex_init(&mutex, SWITCH_MUTEX_NESTED, fst_pool);
			fst_requires(open_codec(&codec, "opus", "mod_opus", fmtp, 48000, 20, 1, fst_pool) == SWITCH_STATUS_SUCCESS);
			opus_payload_map(&pmap, fmtp);
			switch_atomic_set(&generation, 0);

			h.mutex = mutex;
			h.codec = &codec;
			h.generation = &generation;
			h.churn = 1;
			thread = start_holder(&h, fst_pool);
			fst_check(thread != NULL && switch_atomic_read(&h.holding));

			for (round = 0; round < 200; round++) {
				uint32_t snapshot = switch_atomic_read(&generation);

				for (waited = 0; switch_atomic_read(&generation) == snapshot && waited < 10000; waited++) {
					switch_yield(100);
				}
				if (switch_atomic_read(&generation) != snapshot) {
					stale_checked++;
					fst_check(!switch_core_media_pvt_vbr_ptime_adopt(NULL, mutex, &codec, &pmap, 40, &reset, &generation, snapshot));
				}
			}
			stop_holder(&h, thread);

			fst_check(stale_checked == 200);
			fst_check(pmap.codec_ms == 20 && reset == 0 && !switch_test_flag(&codec, SWITCH_CODEC_FLAG_RESET_PENDING));
			fst_check(switch_core_media_pvt_vbr_ptime_adopt(NULL, mutex, &codec, &pmap, 40, &reset, &generation, switch_atomic_read(&generation)));
			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(adoption_refuses_unsupported_or_unchanged_ptime)
		{
			switch_codec_t codec;
			switch_mutex_t *mutex = NULL;
			payload_map_t pmap;
			uint8_t reset = 0;
			uint32_t ms;

			switch_mutex_init(&mutex, SWITCH_MUTEX_NESTED, fst_pool);
			fst_requires(open_codec(&codec, "AMR-WB", "mod_amrwb", "octet-align=0", 16000, 20, 1, fst_pool) == SWITCH_STATUS_SUCCESS);
			memset(&pmap, 0, sizeof(pmap));
			pmap.iananame = (char *) "AMR-WB";
			pmap.rm_fmtp = (char *) "octet-align=0";
			pmap.rm_rate = 16000;
			pmap.channels = 1;
			pmap.codec_ms = 20;

			for (ms = 40; ms <= 120; ms += 20) {
				fst_check(!switch_core_media_pvt_vbr_ptime_adopt(NULL, mutex, &codec, &pmap, ms, &reset, NULL, 0));
			}
			fst_check(!switch_core_media_pvt_vbr_ptime_adopt(NULL, mutex, &codec, &pmap, 20, &reset, NULL, 0));
			fst_check(!switch_core_media_pvt_vbr_ptime_adopt(NULL, NULL, &codec, &pmap, 40, &reset, NULL, 0));
			fst_check(pmap.codec_ms == 20 && reset == 0 && !switch_test_flag(&codec, SWITCH_CODEC_FLAG_RESET_PENDING));
			fst_check(switch_mutex_trylock(mutex) == SWITCH_STATUS_SUCCESS);
			switch_mutex_unlock(mutex);
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

		FST_TEST_BEGIN(support_follows_the_provider_the_rebuild_loads)
		{
			switch_codec_interface_t *amrwb = switch_loadable_module_get_codec_interface("AMR-WB", "mod_amrwb");
			int amrwb_refs = amrwb ? amrwb->refs - 1 : 0;
			two_providers_stop_t stop;

			UNPROTECT_INTERFACE(amrwb);
			fst_requires(amrwb);

			/* a case stopped right after each acquisition still restores the registry, so it can run again */
			for (stop = TWO_PROVIDERS_STOP_AFTER_OPEN; stop <= TWO_PROVIDERS_STOP_AFTER_LOOKUP; stop++) {
				fst_check(two_providers_case(stop, fst_pool) == 0);
				fst_check(two_providers_restored(amrwb_refs));
			}

			fst_check(two_providers_case(TWO_PROVIDERS_RUN_ALL, fst_pool) == 0);
			fst_check(two_providers_restored(amrwb_refs));
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
