#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "avmd_candidate.h"

#define TEST_MAX_ATTEMPTS 3u
#define TEST_REARM_SAMPLES 8000u

static unsigned int failures;

static void check_result(int condition, const char *description)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", description);
		++failures;
	}
}

static void fill_until_due(avmd_candidate_state_t *state, size_t frame_samples)
{
	while (state->samples < 800u) {
		avmd_candidate_observe(state, 660.0, 33.0, frame_samples, 1u,
				TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
	}
}

static void test_continuity_and_variable_frames(void)
{
	avmd_candidate_state_t state;

	avmd_candidate_reset(&state);
	avmd_candidate_observe(&state, 660.0, 33.0, 80u, 0u,
			TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
	check_result(state.active && state.samples == 80u,
			"first observation counts its exact qualifying range");
	avmd_candidate_observe(&state, 662.0, 33.0, 80u, 1u,
			TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
	avmd_candidate_observe(&state, 661.0, 33.0, 160u, 1u,
			TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
	check_result(state.samples == 320u,
			"candidate accumulates actual variable frame sizes");
	check_result(fabs(state.frequency - 661.0) < 1.0,
			"candidate frequency follows a continuous tone");

	avmd_candidate_reset(&state);
	check_result(!state.active && state.samples == 0u,
			"an interrupted candidate resets continuity");
}

static void test_frequency_shift_restarts_candidate(void)
{
	avmd_candidate_state_t state;

	avmd_candidate_reset(&state);
	avmd_candidate_observe(&state, 660.0, 33.0, 160u, 0u,
			TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
	avmd_candidate_observe(&state, 662.0, 33.0, 160u, 1u,
			TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
	avmd_candidate_observe(&state, 760.0, 33.0, 160u, 1u,
			TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
	check_result(state.samples == 160u && fabs(state.frequency - 760.0) < 0.01,
			"frequency shift starts a new candidate epoch");
}

static void test_rejected_epoch_rearms(void)
{
	avmd_candidate_state_t state;
	unsigned int attempt;

	avmd_candidate_reset(&state);
	avmd_candidate_observe(&state, 660.0, 33.0, 160u, 0u,
			TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
	fill_until_due(&state, 160u);

	for (attempt = 1; attempt <= TEST_MAX_ATTEMPTS; ++attempt) {
		check_result(avmd_candidate_confirmation_due(&state, 800u, 400u,
				TEST_MAX_ATTEMPTS), "confirmation becomes due in retry epoch");
		avmd_candidate_record_rejection(&state);
		check_result(state.confirmation_attempts == attempt,
				"confirmation attempt is counted");
		if (attempt < TEST_MAX_ATTEMPTS) {
			while (state.samples - state.last_confirmation_samples < 400u) {
				avmd_candidate_observe(&state, 660.0, 33.0, 160u, 1u,
						TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
			}
		}
	}

	while (state.samples - state.last_confirmation_samples < TEST_REARM_SAMPLES) {
		avmd_candidate_observe(&state, 660.0, 33.0, 160u, 1u,
				TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
		check_result(state.confirmation_attempts == TEST_MAX_ATTEMPTS,
				"same-frequency retry remains capped during cooldown");
	}
	avmd_candidate_observe(&state, 660.0, 33.0, 160u, 1u,
			TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
	check_result(state.active && state.confirmation_attempts == 0u &&
			state.samples == 160u,
			"same-frequency candidate rearms after bounded cooldown");
	fill_until_due(&state, 160u);
	check_result(avmd_candidate_confirmation_due(&state, 800u, 400u,
			TEST_MAX_ATTEMPTS),
			"later pure same-frequency tone is reconsidered");
	avmd_candidate_record_rejection(&state);
}

static void test_legacy_duration_boundary(void)
{
	avmd_candidate_state_t state;

	avmd_candidate_reset(&state);
	avmd_candidate_observe(&state, 660.0, 33.0, 15u, 0u,
			TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
	check_result(!avmd_candidate_confirmation_due(&state, 16u, 1u,
			TEST_MAX_ATTEMPTS),
			"fifteen samples remain below the legacy duration boundary");
	avmd_candidate_observe(&state, 660.0, 33.0, 1u, 1u,
			TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
	check_result(avmd_candidate_confirmation_due(&state, 16u, 1u,
			TEST_MAX_ATTEMPTS),
			"legacy two-millisecond duration remains immediately eligible");
	avmd_candidate_record_rejection(&state);
}

static void test_exact_qualifying_segments(void)
{
	avmd_candidate_segment_t segment;

	avmd_candidate_segment_reset(&segment);
	avmd_candidate_segment_observe_frequency(&segment, 1u, 660.0, 1u, 1u);
	avmd_candidate_segment_mark_detection(&segment);
	avmd_candidate_segment_observe_frequency(&segment, 10u, 662.0, 1u, 0u);
	check_result(avmd_candidate_segment_samples(&segment) == 10u &&
			segment.starts_at_frame_start && segment.detection_seen,
			"continuous match records its exact frame range");
	avmd_candidate_segment_observe_frequency(&segment, 11u, 0.0, 0u, 0u);
	check_result(!segment.active && avmd_candidate_segment_samples(&segment) == 0u,
			"an in-frame gap clears the qualifying range");
	avmd_candidate_segment_observe_frequency(&segment, 150u, 660.0, 1u, 0u);
	avmd_candidate_segment_observe_frequency(&segment, 160u, 658.0, 1u, 0u);
	check_result(avmd_candidate_segment_samples(&segment) == 11u &&
			!segment.starts_at_frame_start,
			"a late pulse records only its exact samples and cannot bridge frames");
	avmd_candidate_segment_observe_frequency(&segment, 161u, 760.0, 1u, 0u);
	check_result(avmd_candidate_segment_samples(&segment) == 1u &&
			fabs(segment.frequency - 760.0) < 0.01,
			"an in-frame frequency shift starts a new qualifying range");
}

static void test_sparse_pulses_do_not_satisfy_duration(void)
{
	avmd_candidate_state_t state;
	unsigned int frame;

	avmd_candidate_reset(&state);
	for (frame = 0; frame < 10u; ++frame) {
		avmd_candidate_observe(&state, 660.0, 33.0, 16u, 0u,
				TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
		check_result(state.samples == 16u,
				"separate short pulses do not accumulate frame duration");
	}
	for (frame = 0; frame < 5u; ++frame) {
		avmd_candidate_observe(&state, 660.0, 33.0, 160u, 1u,
				TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
	}
	check_result(state.samples == 816u,
			"only the final continuous pulse and tone are accumulated");
	check_result(!avmd_candidate_confirmation_due(&state, 2400u, 400u,
			TEST_MAX_ATTEMPTS),
			"sparse hits plus one hundred milliseconds cannot satisfy three hundred milliseconds");
}

static void test_exact_290_300_ms_boundary(void)
{
	avmd_candidate_state_t state;
	unsigned int frame;

	avmd_candidate_reset(&state);
	avmd_candidate_observe(&state, 660.0, 33.0, 80u, 0u,
			TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
	for (frame = 0; frame < 14u; ++frame) {
		avmd_candidate_observe(&state, 660.0, 33.0, 160u, 1u,
				TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
	}
	check_result(state.samples == 2320u &&
			!avmd_candidate_confirmation_due(&state, 2400u, 400u,
				TEST_MAX_ATTEMPTS),
			"two thousand three hundred twenty samples remain below three hundred milliseconds");
	avmd_candidate_observe(&state, 660.0, 33.0, 80u, 1u,
			TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
	check_result(state.samples == 2400u &&
			avmd_candidate_confirmation_due(&state, 2400u, 400u,
				TEST_MAX_ATTEMPTS),
			"exactly three hundred milliseconds becomes eligible");
}

static void test_long_rejection_is_rate_bounded(void)
{
	avmd_candidate_state_t state;
	unsigned int frame;
	unsigned int confirmations;

	avmd_candidate_reset(&state);
	confirmations = 0;
	for (frame = 0; frame < 500u; ++frame) {
		avmd_candidate_observe(&state, 460.0, 23.0, 160u,
				(uint8_t)(frame != 0u),
				TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
		if (avmd_candidate_confirmation_due(&state, 800u, 400u,
					TEST_MAX_ATTEMPTS)) {
			avmd_candidate_record_rejection(&state);
			++confirmations;
		}
	}
	check_result(confirmations > TEST_MAX_ATTEMPTS,
			"continuous candidate is eventually reconsidered");
	check_result(confirmations <= 33u,
			"ten-second continuous rejection obeys confirmation rate bound");
}

static void test_confirmed_windows_are_contiguous_duration(void)
{
	avmd_candidate_state_t state;
	unsigned int frame;

	avmd_candidate_reset(&state);
	for (frame = 0; frame < 5u; ++frame) {
		avmd_candidate_observe(&state, 660.0, 33.0, 160u,
				(uint8_t)(frame != 0u), TEST_REARM_SAMPLES,
				TEST_MAX_ATTEMPTS);
	}
	check_result(avmd_candidate_confirmation_due(&state, 800u, 800u,
			TEST_MAX_ATTEMPTS),
			"first complete spectral window becomes due");
	avmd_candidate_record_acceptance(&state, 0u, 800u);
	check_result(state.confirmed_samples == 800u,
			"one accepted window credits exactly one hundred milliseconds");
	for (frame = 0; frame < 5u; ++frame) {
		avmd_candidate_observe(&state, 660.0, 33.0, 160u, 1u,
				TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
	}
	check_result(avmd_candidate_confirmation_due(&state, 800u, 800u,
			TEST_MAX_ATTEMPTS),
			"the next non-overlapping spectral window becomes due");
	avmd_candidate_record_acceptance(&state, 800u, 1600u);
	check_result(state.confirmed_samples == 1600u,
			"adjacent accepted windows accumulate without overlap");
	avmd_candidate_record_acceptance(&state, 1800u, 2600u);
	check_result(state.confirmed_samples == 800u &&
			state.confirmed_start_sample == 1800u &&
			state.confirmed_end_sample == 2600u,
			"an unconfirmed range between windows restarts duration");
	avmd_candidate_record_rejection(&state);
	check_result(state.confirmed_samples == 0u,
			"a rejected window resets confirmed tone duration");
	avmd_candidate_observe(&state, 660.0, 33.0, 0u, 0u,
			TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
	check_result(!state.active && state.confirmed_samples == 0u,
			"a candidate gap resets all confirmed duration");
}

static void test_confirmed_range_290_300_ms_boundary(void)
{
	avmd_candidate_state_t state;

	avmd_candidate_reset(&state);
	avmd_candidate_record_acceptance(&state, 0u, 800u);
	avmd_candidate_record_acceptance(&state, 800u, 1600u);
	avmd_candidate_record_acceptance(&state, 1600u, 2320u);
	check_result(state.confirmed_samples == 2320u &&
			state.confirmed_samples < 2400u,
			"confirmed ranges retain the exact two-hundred-ninety-millisecond boundary");
	avmd_candidate_record_acceptance(&state, 2320u, 2400u);
	check_result(state.confirmed_samples == 2400u,
			"only a confirmed adjacent range reaches exactly three hundred milliseconds");
}

static void test_non_divisible_frame_window_endpoints(void)
{
	avmd_candidate_state_t state;
	size_t end_sample;

	avmd_candidate_reset(&state);
	avmd_candidate_observe(&state, 660.0, 33.0, 120u, 0u,
			TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
	end_sample = avmd_candidate_confirmation_window_end(&state,
			120u, 100u, 120u);
	check_result(end_sample == 100u,
			"first window is anchored to the candidate start inside a 120-sample frame");
	avmd_candidate_record_acceptance(&state, 0u, end_sample);
	state.next_confirmation_sample = end_sample + 100u;
	avmd_candidate_observe(&state, 660.0, 33.0, 120u, 1u,
			TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
	end_sample = avmd_candidate_confirmation_window_end(&state,
			240u, 100u, 240u);
	check_result(end_sample == 200u,
			"second window uses its absolute endpoint inside the next frame");
	avmd_candidate_record_acceptance(&state, 100u, end_sample);
	state.next_confirmation_sample = end_sample + 100u;
	avmd_candidate_observe(&state, 660.0, 33.0, 60u, 1u,
			TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
	end_sample = avmd_candidate_confirmation_window_end(&state,
			300u, 100u, 240u);
	check_result(end_sample == 300u,
			"a final 60-sample frame exposes the exact 300-sample endpoint");
	avmd_candidate_record_acceptance(&state, 200u, end_sample);
	check_result(state.confirmed_samples == 300u,
			"three absolute adjacent windows prove exactly three hundred samples");

	avmd_candidate_reset(&state);
	avmd_candidate_observe(&state, 660.0, 33.0, 120u, 0u,
			TEST_REARM_SAMPLES, TEST_MAX_ATTEMPTS);
	end_sample = avmd_candidate_confirmation_window_end(&state,
			170u, 100u, 170u);
	check_result(end_sample == 150u,
			"the first window endpoint follows a non-zero absolute candidate start");

	state.next_confirmation_sample = 460u;
	end_sample = avmd_candidate_confirmation_window_end(&state,
			700u, 100u, 120u);
	check_result(end_sample == 700u,
			"an unavailable historical endpoint falls back to the current frame end");
	avmd_candidate_record_acceptance(&state, 600u, end_sample);
	check_result(state.confirmed_samples == 100u,
			"a history gap cannot extend an earlier confirmed range");
}

static void test_frame_positions(void)
{
	check_result(avmd_candidate_frame_position(100u, 200u, 5u, 0u, 0u) == 100u,
			"legacy detector retains its cursor");
	check_result(avmd_candidate_frame_position(100u, 200u, 5u, 1u, 0u) == 105u,
			"legacy lagged detector advances by overlap");
	check_result(avmd_candidate_frame_position(100u, 200u, 5u, 0u, 1u) == 200u,
			"hardened detector uses current frame cursor");
	check_result(avmd_candidate_frame_position(100u, 200u, 5u, 1u, 1u) == 200u,
			"hardened lagged detector uses current frame cursor");
}

int main(void)
{
	test_continuity_and_variable_frames();
	test_frequency_shift_restarts_candidate();
	test_rejected_epoch_rearms();
	test_legacy_duration_boundary();
	test_exact_qualifying_segments();
	test_sparse_pulses_do_not_satisfy_duration();
	test_exact_290_300_ms_boundary();
	test_long_rejection_is_rate_bounded();
	test_confirmed_windows_are_contiguous_duration();
	test_confirmed_range_290_300_ms_boundary();
	test_non_divisible_frame_window_endpoints();
	test_frame_positions();

	if (failures != 0u) {
		fprintf(stderr, "%u candidate test(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	printf("all candidate tests passed\n");
	return EXIT_SUCCESS;
}
