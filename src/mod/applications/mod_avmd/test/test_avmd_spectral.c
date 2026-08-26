/*
 * Deterministic tests for the standalone AVMD spectral confirmation helper.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "avmd_candidate.h"
#include "avmd_spectral.h"

#if !defined(M_PI)
#define M_PI 3.14159265358979323846264338327
#endif

#define TEST_WINDOW_MS (100u)
#define TEST_SEARCH_RADIUS_HZ (80.0)
#define TEST_SEARCH_STEP_HZ (1.0)
#define TEST_PURITY_THRESHOLD (0.80)

#ifndef AVMD_TEST_FIXTURE_DIR
#define AVMD_TEST_FIXTURE_DIR "test/fixtures"
#endif

static int failures = 0;

static void check_result(int condition, const char *name)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", name);
		failures++;
	}
}

static void generate_tone(double *samples,
		size_t num,
		uint32_t rate,
		double frequency,
		double amplitude,
		double phase)
{
	size_t i;

	for (i = 0; i < num; i++) {
		samples[i] = amplitude * sin(((2.0 * M_PI * frequency * (double)i) / (double)rate) + phase);
	}
}

static void add_tone(double *samples,
		size_t num,
		uint32_t rate,
		double frequency,
		double amplitude,
		double phase)
{
	size_t i;

	for (i = 0; i < num; i++) {
		samples[i] += amplitude * sin(((2.0 * M_PI * frequency * (double)i) / (double)rate) + phase);
	}
}

static avmd_spectral_result_t analyze(const double *samples,
		size_t num,
		uint32_t rate,
		double candidate)
{
	avmd_spectral_result_t result;
	int status;

	status = avmd_spectral_analyze(samples,
			num,
			rate,
			candidate,
			440.0,
			2000.0,
			TEST_SEARCH_RADIUS_HZ,
			TEST_SEARCH_STEP_HZ,
			&result);
	check_result(status == 1, "analysis succeeds");
	return result;
}

static void test_single_tone(uint32_t rate, double phase, double amplitude)
{
	size_t num;
	double *samples;
	avmd_spectral_result_t result;

	num = ((size_t)rate * TEST_WINDOW_MS) / 1000u;
	samples = (double *)calloc(num, sizeof(*samples));
	check_result(samples != NULL, "single-tone allocation");
	if (samples == NULL) {
		return;
	}

	generate_tone(samples, num, rate, 660.0, amplitude, phase);
	result = analyze(samples, num, rate, 657.5);
	check_result(fabs(result.dominant_frequency - 660.0) <= TEST_SEARCH_STEP_HZ,
			"single tone dominant frequency");
	check_result(result.purity >= TEST_PURITY_THRESHOLD, "single tone purity");
	check_result(result.secondary_purity < 0.10, "single tone has no comparable second peak");
	check_result(avmd_spectral_window_has_continuous_candidate(samples, num,
				rate, 657.5),
			"single tone preserves candidate-frequency continuity across short blocks");

	free(samples);
}

static void test_frequency_boundaries(uint32_t rate)
{
	size_t num;
	double *samples;
	avmd_spectral_result_t result;

	num = ((size_t)rate * TEST_WINDOW_MS) / 1000u;
	samples = (double *)calloc(num, sizeof(*samples));
	check_result(samples != NULL, "boundary allocation");
	if (samples == NULL) {
		return;
	}

	generate_tone(samples, num, rate, 440.0, 6000.0, M_PI / 11.0);
	result = analyze(samples, num, rate, 440.0);
	check_result(fabs(result.dominant_frequency - 440.0) <= TEST_SEARCH_STEP_HZ,
			"minimum boundary dominant frequency");
	check_result(result.purity >= TEST_PURITY_THRESHOLD, "minimum boundary accepted");

	generate_tone(samples, num, rate, 2000.0, 6000.0, M_PI / 13.0);
	result = analyze(samples, num, rate, 2000.0);
	check_result(fabs(result.dominant_frequency - 2000.0) <= TEST_SEARCH_STEP_HZ,
			"maximum boundary dominant frequency");
	check_result(result.purity >= TEST_PURITY_THRESHOLD, "maximum boundary accepted");

	free(samples);
}

static void test_noisy_single_tone(uint32_t rate)
{
	size_t num;
	size_t i;
	double *samples;
	avmd_spectral_result_t result;
	uint32_t noise_state;

	num = ((size_t)rate * TEST_WINDOW_MS) / 1000u;
	samples = (double *)calloc(num, sizeof(*samples));
	check_result(samples != NULL, "noisy-tone allocation");
	if (samples == NULL) {
		return;
	}
	generate_tone(samples, num, rate, 660.0, 5000.0, M_PI / 17.0);
	noise_state = 0x5a17u;
	for (i = 0; i < num; i++) {
		noise_state = (1664525u * noise_state) + 1013904223u;
		samples[i] += 1500.0 * (((double)(noise_state & 0xffffu) / 32767.5) - 1.0);
	}
	result = analyze(samples, num, rate, 660.0);
	check_result(result.purity >= TEST_PURITY_THRESHOLD, "single tone survives deterministic noise");
	free(samples);
}

static void test_greeting_like_interference(uint32_t rate)
{
	size_t num;
	double *samples;
	avmd_spectral_result_t result;

	num = ((size_t)rate * TEST_WINDOW_MS) / 1000u;
	samples = (double *)calloc(num, sizeof(*samples));
	check_result(samples != NULL, "greeting-like allocation");
	if (samples == NULL) {
		return;
	}
	generate_tone(samples, num, rate, 1045.0, 4000.0, 0.0);
	add_tone(samples, num, rate, 880.0, 4000.0, M_PI / 5.0);
	add_tone(samples, num, rate, 1210.0, 4000.0, M_PI / 7.0);
	result = analyze(samples, num, rate, 1045.0);
	check_result(result.purity < TEST_PURITY_THRESHOLD,
			"greeting-like multi-frequency candidate rejected");
	free(samples);
}

static void test_speech_like_negative(uint32_t rate)
{
	size_t num;
	size_t i;
	double envelope;
	double *samples;
	avmd_spectral_result_t result;

	num = ((size_t)rate * TEST_WINDOW_MS) / 1000u;
	samples = (double *)calloc(num, sizeof(*samples));
	check_result(samples != NULL, "speech-like allocation");
	if (samples == NULL) {
		return;
	}
	generate_tone(samples, num, rate, 140.0, 2500.0, 0.0);
	add_tone(samples, num, rate, 280.0, 1800.0, M_PI / 9.0);
	add_tone(samples, num, rate, 420.0, 1400.0, M_PI / 7.0);
	add_tone(samples, num, rate, 700.0, 2600.0, M_PI / 5.0);
	add_tone(samples, num, rate, 840.0, 1700.0, M_PI / 3.0);
	add_tone(samples, num, rate, 1120.0, 2200.0, M_PI / 11.0);
	for (i = 0; i < num; ++i) {
		envelope = 0.65 + 0.35 * sin((2.0 * M_PI * 4.0 * (double)i) / (double)rate);
		samples[i] *= envelope;
	}
	result = analyze(samples, num, rate, 700.0);
	check_result(result.purity < TEST_PURITY_THRESHOLD,
			"labelled synthetic speech-like formants rejected");
	free(samples);
}

static void test_music_like_negative(uint32_t rate)
{
	size_t num;
	double *samples;
	avmd_spectral_result_t result;

	num = ((size_t)rate * TEST_WINDOW_MS) / 1000u;
	samples = (double *)calloc(num, sizeof(*samples));
	check_result(samples != NULL, "music-like allocation");
	if (samples == NULL) {
		return;
	}
	generate_tone(samples, num, rate, 523.25, 3500.0, 0.0);
	add_tone(samples, num, rate, 659.25, 3500.0, M_PI / 5.0);
	add_tone(samples, num, rate, 783.99, 3500.0, M_PI / 7.0);
	result = analyze(samples, num, rate, 659.25);
	check_result(result.purity < TEST_PURITY_THRESHOLD,
			"labelled synthetic major chord rejected");
	free(samples);
}

static void test_window_continuity(uint32_t rate)
{
	size_t num;
	size_t i;
	size_t gap_start;
	double *samples;
	avmd_spectral_result_t result;

	num = ((size_t)rate * TEST_WINDOW_MS) / 1000u;
	samples = (double *)calloc(num, sizeof(*samples));
	check_result(samples != NULL, "window-continuity allocation");
	if (samples == NULL) {
		return;
	}
	generate_tone(samples, num, rate, 660.0, 6000.0, M_PI / 13.0);
	check_result(avmd_spectral_window_is_continuous(samples, num, rate),
			"a complete tone window is continuous");
	check_result(avmd_spectral_window_has_continuous_candidate(samples, num,
				rate, 660.0),
			"a complete tone window contains the candidate throughout");

	gap_start = num - (((size_t)rate * 10u) / 1000u);
	for (i = gap_start; i < num; ++i) {
		samples[i] = 0.0;
	}
	result = analyze(samples, num, rate, 660.0);
	check_result(result.purity >= TEST_PURITY_THRESHOLD,
			"ninety-millisecond tone still passes the purity threshold");
	check_result(!avmd_spectral_window_is_continuous(samples, num, rate),
			"a ten-millisecond partial-window gap is not credited");

	generate_tone(samples, num, rate, 660.0, 6000.0, M_PI / 13.0);
	gap_start = ((size_t)rate * 40u) / 1000u;
	for (i = gap_start; i < gap_start + (((size_t)rate * 20u) / 1000u); ++i) {
		samples[i] = 0.0;
	}
	check_result(!avmd_spectral_window_is_continuous(samples, num, rate),
			"an internal twenty-millisecond gap is not credited");

	generate_tone(samples, num, rate, 660.0, 6000.0, M_PI / 13.0);
	gap_start = num - (((size_t)rate * 10u) / 1000u);
	for (i = gap_start; i < num; ++i) {
		samples[i] = 6000.0 * sin((2.0 * M_PI * 1000.0 * (double)i) /
				(double)rate + M_PI / 13.0);
	}
	result = analyze(samples, num, rate, 660.0);
	check_result(result.purity >= TEST_PURITY_THRESHOLD,
			"ninety milliseconds of candidate plus ten milliseconds of another tone passes whole-window purity");
	check_result(avmd_spectral_window_is_continuous(samples, num, rate),
			"wrong-frequency audio remains continuously active");
	check_result(!avmd_spectral_window_has_continuous_candidate(samples, num,
				rate, 660.0),
			"active wrong-frequency audio cannot count as continuous candidate tone");
	free(samples);
}

static void confirm_duration_window(avmd_candidate_state_t *state,
		const double *samples,
		size_t start_sample,
		size_t window_samples,
		uint32_t rate)
{
	avmd_spectral_result_t result;

	result = analyze(samples + start_sample, window_samples, rate, 660.0);
	if (avmd_spectral_result_accepted(&result, TEST_PURITY_THRESHOLD, 0u) &&
			avmd_spectral_window_is_continuous(samples + start_sample,
				window_samples, rate) &&
			avmd_spectral_window_has_continuous_candidate(
				samples + start_sample, window_samples, rate, 660.0)) {
		avmd_candidate_record_acceptance(state, start_sample,
				start_sample + window_samples);
	} else {
		avmd_candidate_record_rejection(state);
	}
}

static void test_production_duration_boundaries(uint32_t rate)
{
	size_t window_samples;
	size_t duration_samples;
	size_t gap_start;
	size_t i;
	double *samples;
	avmd_candidate_state_t state;

	window_samples = ((size_t)rate * TEST_WINDOW_MS) / 1000u;
	duration_samples = ((size_t)rate * 300u) / 1000u;
	samples = (double *)calloc(duration_samples, sizeof(*samples));
	check_result(samples != NULL, "production-duration allocation");
	if (samples == NULL) {
		return;
	}
	generate_tone(samples, duration_samples, rate, 660.0, 6000.0, M_PI / 17.0);
	avmd_candidate_reset(&state);
	confirm_duration_window(&state, samples, 0u, window_samples, rate);
	confirm_duration_window(&state, samples, window_samples, window_samples, rate);
	check_result(state.confirmed_samples == 2u * window_samples &&
			state.confirmed_samples < duration_samples,
			"a partial two-hundred-millisecond prefix remains below the duration gate");
	confirm_duration_window(&state, samples, 2u * window_samples,
			window_samples, rate);
	check_result(state.confirmed_samples == duration_samples,
			"exactly three hundred milliseconds of proven tone satisfies the gate");

	gap_start = ((size_t)rate * 290u) / 1000u;
	for (i = gap_start; i < duration_samples; ++i) {
		samples[i] = 0.0;
	}
	avmd_candidate_reset(&state);
	confirm_duration_window(&state, samples, 0u, window_samples, rate);
	confirm_duration_window(&state, samples, window_samples, window_samples, rate);
	confirm_duration_window(&state, samples, 2u * window_samples,
			window_samples, rate);
	check_result(state.confirmed_samples < duration_samples,
			"two hundred ninety milliseconds plus silence cannot satisfy three hundred");

	generate_tone(samples, duration_samples, rate, 660.0, 6000.0,
			M_PI / 17.0);
	gap_start = ((size_t)rate * 290u) / 1000u;
	for (i = gap_start; i < duration_samples; ++i) {
		samples[i] = 6000.0 * sin((2.0 * M_PI * 1000.0 * (double)i) /
				(double)rate + M_PI / 17.0);
	}
	avmd_candidate_reset(&state);
	confirm_duration_window(&state, samples, 0u, window_samples, rate);
	confirm_duration_window(&state, samples, window_samples, window_samples, rate);
	confirm_duration_window(&state, samples, 2u * window_samples,
			window_samples, rate);
	check_result(state.confirmed_samples < duration_samples,
			"two hundred ninety milliseconds plus an active wrong tone cannot satisfy three hundred");
	free(samples);
}

static void confirm_variable_frames(avmd_candidate_state_t *state,
		const double *samples,
		const unsigned int *frame_ends_ms,
		size_t frame_count,
		uint32_t rate)
{
	size_t window_samples;
	size_t previous_frame_end;
	size_t frame_end;
	size_t confirmation_end;
	size_t i;

	window_samples = ((size_t)rate * TEST_WINDOW_MS) / 1000u;
	previous_frame_end = 0;
	for (i = 0; i < frame_count; ++i) {
		frame_end = ((size_t)rate * frame_ends_ms[i]) / 1000u;
		avmd_candidate_observe(state, 660.0, 33.0,
				frame_end - previous_frame_end, (uint8_t)(i != 0u),
				(size_t)rate, 3u);
		for (;;) {
			confirmation_end = avmd_candidate_confirmation_window_end(state,
					frame_end, window_samples, frame_end);
			if (confirmation_end == 0) {
				break;
			}
			confirm_duration_window(state, samples,
					confirmation_end - window_samples, window_samples, rate);
			state->next_confirmation_sample = confirmation_end + window_samples;
		}
		previous_frame_end = frame_end;
	}
}

static void test_production_variable_frames(uint32_t rate)
{
	static const unsigned int frame_ends_300_ms[] = {120u, 240u, 300u};
	static const unsigned int frame_ends_large_ms[] = {240u, 300u};
	static const unsigned int frame_ends_290_ms[] = {120u, 240u, 290u, 300u};
	size_t duration_samples;
	size_t boundary_sample;
	size_t i;
	double *samples;
	avmd_candidate_state_t state;

	duration_samples = ((size_t)rate * 300u) / 1000u;
	boundary_sample = ((size_t)rate * 290u) / 1000u;
	samples = (double *)calloc(duration_samples, sizeof(*samples));
	check_result(samples != NULL, "variable-frame duration allocation");
	if (samples == NULL) {
		return;
	}
	generate_tone(samples, duration_samples, rate, 660.0, 6000.0, M_PI / 19.0);
	avmd_candidate_reset(&state);
	confirm_variable_frames(&state, samples, frame_ends_300_ms,
			sizeof(frame_ends_300_ms) / sizeof(frame_ends_300_ms[0]), rate);
	check_result(state.confirmed_samples == duration_samples,
			"120/120/60-millisecond frames accept exactly three hundred milliseconds");
	avmd_candidate_reset(&state);
	confirm_variable_frames(&state, samples, frame_ends_large_ms,
			sizeof(frame_ends_large_ms) / sizeof(frame_ends_large_ms[0]), rate);
	check_result(state.confirmed_samples == duration_samples,
			"a 240-millisecond frame consumes every complete anchored window");

	for (i = boundary_sample; i < duration_samples; ++i) {
		samples[i] = 0.0;
	}
	avmd_candidate_reset(&state);
	confirm_variable_frames(&state, samples, frame_ends_290_ms,
			sizeof(frame_ends_290_ms) / sizeof(frame_ends_290_ms[0]), rate);
	check_result(state.confirmed_samples < duration_samples,
			"120/120/50-millisecond tone plus final silence remains below three hundred milliseconds");

	generate_tone(samples, duration_samples, rate, 660.0, 6000.0, M_PI / 19.0);
	for (i = boundary_sample; i < duration_samples; ++i) {
		samples[i] = 6000.0 * sin((2.0 * M_PI * 1000.0 * (double)i) /
				(double)rate + M_PI / 19.0);
	}
	avmd_candidate_reset(&state);
	confirm_variable_frames(&state, samples, frame_ends_290_ms,
			sizeof(frame_ends_290_ms) / sizeof(frame_ends_290_ms[0]), rate);
	check_result(state.confirmed_samples < duration_samples,
			"120/120/50-millisecond tone plus final wrong-frequency audio remains below three hundred milliseconds");
	free(samples);
}

static void test_production_twenty_ms_wrong_frequency(uint32_t rate)
{
	size_t frame_samples;
	size_t window_samples;
	size_t duration_samples;
	size_t frame_end;
	size_t confirmation_end;
	size_t wrong_start;
	size_t i;
	double *samples;
	avmd_candidate_state_t state;

	frame_samples = ((size_t)rate * 20u) / 1000u;
	window_samples = ((size_t)rate * TEST_WINDOW_MS) / 1000u;
	duration_samples = ((size_t)rate * 300u) / 1000u;
	samples = (double *)calloc(duration_samples, sizeof(*samples));
	check_result(samples != NULL, "twenty-millisecond-frame allocation");
	if (samples == NULL) {
		return;
	}
	generate_tone(samples, duration_samples, rate, 660.0, 6000.0,
			M_PI / 23.0);
	wrong_start = ((size_t)rate * 290u) / 1000u;
	for (i = wrong_start; i < duration_samples; ++i) {
		samples[i] = 6000.0 * sin((2.0 * M_PI * 1000.0 * (double)i) /
				(double)rate + M_PI / 23.0);
	}
	avmd_candidate_reset(&state);
	frame_end = frame_samples;
	while (frame_end <= duration_samples) {
		avmd_candidate_observe(&state, 660.0, 33.0, frame_samples,
				(uint8_t)(frame_end != frame_samples), (size_t)rate, 3u);
		confirmation_end = avmd_candidate_confirmation_window_end(&state,
				frame_end, window_samples, frame_end);
		if (confirmation_end != 0) {
			confirm_duration_window(&state, samples,
					confirmation_end - window_samples, window_samples, rate);
			state.next_confirmation_sample = confirmation_end + window_samples;
		}
		frame_end += frame_samples;
	}
	check_result(state.confirmed_samples < duration_samples,
			"twenty-millisecond frames cannot credit a final half-frame at the wrong frequency");

	generate_tone(samples, duration_samples, rate, 660.0, 6000.0,
			M_PI / 23.0);
	avmd_candidate_reset(&state);
	frame_end = frame_samples;
	while (frame_end <= duration_samples) {
		avmd_candidate_observe(&state, 660.0, 33.0, frame_samples,
				(uint8_t)(frame_end != frame_samples), (size_t)rate, 3u);
		confirmation_end = avmd_candidate_confirmation_window_end(&state,
				frame_end, window_samples, frame_end);
		if (confirmation_end != 0) {
			confirm_duration_window(&state, samples,
					confirmation_end - window_samples, window_samples, rate);
			state.next_confirmation_sample = confirmation_end + window_samples;
		}
		frame_end += frame_samples;
	}
	check_result(state.confirmed_samples == duration_samples,
			"twenty-millisecond frames accept exactly three hundred milliseconds of candidate tone");
	free(samples);
}

static void test_odd_window_candidate_continuity(uint32_t rate)
{
	size_t num;
	unsigned int phase_index;
	double phase;
	double *samples;
	avmd_spectral_result_t result;

	num = ((size_t)rate * 101u) / 1000u;
	samples = (double *)calloc(num, sizeof(*samples));
	check_result(samples != NULL, "odd-window allocation");
	if (samples == NULL) {
		return;
	}
	for (phase_index = 0; phase_index < 8u; ++phase_index) {
		phase = ((double)phase_index * M_PI) / 8.0;
		generate_tone(samples, num, rate, 660.0, 6000.0, phase);
		check_result(avmd_spectral_window_has_continuous_candidate(samples,
					num, rate, 660.0),
				"phase-varied 101-millisecond tone merges the short remainder");
		result = analyze(samples, num, rate, 660.0);
		check_result(avmd_spectral_result_accepted(&result, 0.95, 0u),
				"high whole-window purity remains independent of the continuity threshold");
	}
	free(samples);
}

static void test_transfer_ringback(uint32_t rate)
{
	size_t num;
	double *samples;
	avmd_spectral_result_t result;

	num = ((size_t)rate * TEST_WINDOW_MS) / 1000u;
	samples = (double *)calloc(num, sizeof(*samples));
	check_result(samples != NULL, "ringback allocation");
	if (samples == NULL) {
		return;
	}

	generate_tone(samples, num, rate, 440.0, 6000.0, 0.0);
	add_tone(samples, num, rate, 480.0, 6000.0, M_PI / 3.0);
	result = analyze(samples, num, rate, 457.6);
	check_result(result.purity < TEST_PURITY_THRESHOLD, "440+480 Hz ringback rejected");
	check_result(result.purity > 0.40 && result.purity < 0.60,
			"ringback dominant tone explains about half the energy");
	check_result(result.secondary_purity > 0.40, "ringback exposes a comparable second peak");

	free(samples);
}

static void test_dtmf(uint32_t rate)
{
	size_t num;
	double *samples;
	avmd_spectral_result_t result;

	num = ((size_t)rate * TEST_WINDOW_MS) / 1000u;
	samples = (double *)calloc(num, sizeof(*samples));
	check_result(samples != NULL, "DTMF allocation");
	if (samples == NULL) {
		return;
	}

	generate_tone(samples, num, rate, 697.0, 5000.0, 0.0);
	add_tone(samples, num, rate, 1209.0, 5000.0, M_PI / 4.0);
	result = analyze(samples, num, rate, 953.0);
	check_result(result.purity < TEST_PURITY_THRESHOLD, "DTMF rejected");

	free(samples);
}

static void test_fax_cng_rejection_band(uint32_t rate)
{
	size_t num;
	double *samples;
	avmd_spectral_result_t result;

	num = ((size_t)rate * TEST_WINDOW_MS) / 1000u;
	samples = (double *)calloc(num, sizeof(*samples));
	check_result(samples != NULL, "fax allocation");
	if (samples == NULL) {
		return;
	}

	generate_tone(samples, num, rate, 1100.0, 5000.0, M_PI / 7.0);
	result = analyze(samples, num, rate, 1098.0);
	check_result(result.purity >= TEST_PURITY_THRESHOLD,
			"fax CNG is spectrally pure before classification");
	check_result(avmd_spectral_is_fax_cng(result.dominant_frequency),
			"fax CNG is classified for optional rejection");
	check_result(avmd_spectral_result_accepted(&result, TEST_PURITY_THRESHOLD, 0u),
			"fax CNG remains accepted when rejection is disabled");
	check_result(!avmd_spectral_result_accepted(&result, TEST_PURITY_THRESHOLD, 1u),
			"fax CNG is rejected when rejection is enabled");
	check_result(!avmd_spectral_is_fax_cng(1045.0),
			"fax rejection band remains narrow below CNG");
	check_result(!avmd_spectral_is_fax_cng(1150.0),
			"fax rejection band remains narrow above CNG");

	free(samples);
}

static void test_silence(uint32_t rate)
{
	size_t num;
	double *samples;
	avmd_spectral_result_t result;
	int status;

	num = ((size_t)rate * TEST_WINDOW_MS) / 1000u;
	samples = (double *)calloc(num, sizeof(*samples));
	check_result(samples != NULL, "silence allocation");
	if (samples == NULL) {
		return;
	}

	status = avmd_spectral_analyze(samples, num, rate, 660.0, 440.0, 2000.0,
			TEST_SEARCH_RADIUS_HZ, TEST_SEARCH_STEP_HZ, &result);
	check_result(status == 0, "silence rejected");

	free(samples);
}

static int16_t decode_ulaw(uint8_t value)
{
	int sign;
	int exponent;
	int mantissa;
	int sample;

	value = (uint8_t)~value;
	sign = value & 0x80;
	exponent = (value >> 4) & 0x07;
	mantissa = value & 0x0f;
	sample = ((mantissa << 3) + 0x84) << exponent;
	sample -= 0x84;
	return (int16_t)(sign ? -sample : sample);
}

static double *load_ulaw_fixture(const char *name, size_t *samples_n)
{
	char path[1024];
	FILE *file;
	long file_size;
	uint8_t *encoded;
	double *samples;
	size_t read_n;
	size_t i;

	if (samples_n == NULL) {
		return NULL;
	}
	*samples_n = 0;
	if (snprintf(path, sizeof(path), "%s/%s", AVMD_TEST_FIXTURE_DIR, name) < 0) {
		return NULL;
	}
	file = fopen(path, "rb");
	if (file == NULL) {
		fprintf(stderr, "unable to open fixture: %s\n", path);
		return NULL;
	}
	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return NULL;
	}
	file_size = ftell(file);
	if (file_size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return NULL;
	}
	encoded = (uint8_t *)malloc((size_t)file_size);
	samples = (double *)malloc((size_t)file_size * sizeof(*samples));
	if (encoded == NULL || samples == NULL) {
		free(encoded);
		free(samples);
		fclose(file);
		return NULL;
	}
	read_n = fread(encoded, 1, (size_t)file_size, file);
	fclose(file);
	if (read_n != (size_t)file_size) {
		free(encoded);
		free(samples);
		return NULL;
	}
	for (i = 0; i < read_n; i++) {
		samples[i] = (double)decode_ulaw(encoded[i]);
	}
	free(encoded);
	*samples_n = read_n;
	return samples;
}

static double fixture_max_purity(const double *samples,
		size_t samples_n,
		double candidate,
		double *max_secondary_purity,
		double *max_time_seconds)
{
	avmd_spectral_result_t result;
	double max_purity;
	size_t window_samples;
	size_t offset;
	int status;

	max_purity = 0.0;
	window_samples = (8000u * TEST_WINDOW_MS) / 1000u;
	if (max_secondary_purity != NULL) {
		*max_secondary_purity = 0.0;
	}
	if (max_time_seconds != NULL) {
		*max_time_seconds = 0.0;
	}
	for (offset = 0; offset + window_samples <= samples_n; offset += window_samples) {
		status = avmd_spectral_analyze(samples + offset,
				window_samples,
				8000u,
				candidate,
				440.0,
				2000.0,
				TEST_SEARCH_RADIUS_HZ,
				TEST_SEARCH_STEP_HZ,
				&result);
		if (status == 1 && result.purity > max_purity) {
			max_purity = result.purity;
			if (max_time_seconds != NULL) {
				*max_time_seconds = (double)offset / 8000.0;
			}
		}
		if (status == 1 && max_secondary_purity != NULL &&
				result.secondary_purity > *max_secondary_purity) {
			*max_secondary_purity = result.secondary_purity;
		}
	}
	return max_purity;
}

static void test_positive_pcap_fixture(void)
{
	double *samples;
	double beep_purity;
	double beep_time;
	size_t samples_n;

	samples = load_ulaw_fixture("positive-voicemail-beep.ulaw", &samples_n);
	check_result(samples != NULL, "positive PCAP-derived fixture loads");
	if (samples == NULL) {
		return;
	}
	check_result(samples_n == 4800u, "positive fixture sample count");
	beep_purity = fixture_max_purity(samples, samples_n, 660.0, NULL, &beep_time);
	check_result(beep_purity >= 0.95, "positive fixture beep accepted");
	check_result(beep_time <= 0.3, "positive fixture beep position");
	free(samples);
}

static void test_negative_pcap_fixture(void)
{
	double *samples;
	double purity;
	double secondary_purity;
	size_t samples_n;

	samples = load_ulaw_fixture("negative-transfer-ringback.ulaw", &samples_n);
	check_result(samples != NULL, "negative PCAP-derived fixture loads");
	if (samples == NULL) {
		return;
	}
	check_result(samples_n == 4800u, "negative fixture sample count");
	purity = fixture_max_purity(samples, samples_n, 457.6, &secondary_purity, NULL);
	check_result(purity < TEST_PURITY_THRESHOLD, "negative transfer-ringback fixture rejected");
	check_result(secondary_purity > 0.20, "negative fixture exposes competing spectral energy");
	free(samples);
}

int main(void)
{
	static const uint32_t rates[] = { 8000u, 16000u, 48000u };
	size_t i;

	for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
		test_single_tone(rates[i], 0.0, 12000.0);
		test_single_tone(rates[i], M_PI / 5.0, 1200.0);
		test_frequency_boundaries(rates[i]);
		test_noisy_single_tone(rates[i]);
		test_greeting_like_interference(rates[i]);
		test_speech_like_negative(rates[i]);
		test_music_like_negative(rates[i]);
		test_window_continuity(rates[i]);
		test_production_duration_boundaries(rates[i]);
		test_production_variable_frames(rates[i]);
		test_production_twenty_ms_wrong_frequency(rates[i]);
		test_odd_window_candidate_continuity(rates[i]);
		test_transfer_ringback(rates[i]);
		test_dtmf(rates[i]);
		test_fax_cng_rejection_band(rates[i]);
		test_silence(rates[i]);
	}
	test_positive_pcap_fixture();
	test_negative_pcap_fixture();

	if (failures != 0) {
		fprintf(stderr, "%d spectral test(s) failed\n", failures);
		return EXIT_FAILURE;
	}

	printf("all AVMD spectral tests passed\n");
	return EXIT_SUCCESS;
}
