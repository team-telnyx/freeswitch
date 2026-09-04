/*
 * Bounded single-tone spectral confirmation for mod_avmd.
 */

#include <math.h>
#include <string.h>

#include "avmd_goertzel.h"
#include "avmd_spectral.h"

#define AVMD_SPECTRAL_SECONDARY_GUARD_HZ (12.0)
#define AVMD_SPECTRAL_ACTIVITY_BLOCK_MS (1u)
#define AVMD_SPECTRAL_ACTIVITY_MIN_RATIO (0.02)
#define AVMD_SPECTRAL_CANDIDATE_BLOCK_MS (2u)
#define AVMD_SPECTRAL_CANDIDATE_MIN_PURITY (0.70)

extern int avmd_spectral_is_fax_cng(double frequency)
{
	return fabs(frequency - AVMD_FAX_CNG_FREQUENCY_HZ) <=
			AVMD_FAX_CNG_TOLERANCE_HZ;
}

extern int avmd_spectral_result_accepted(const avmd_spectral_result_t *result,
		double min_purity,
		uint8_t reject_fax_cng)
{
	if (result == NULL || result->purity < min_purity) {
		return 0;
	}
	return !reject_fax_cng ||
			!avmd_spectral_is_fax_cng(result->dominant_frequency);
}

extern int avmd_spectral_window_is_continuous(const double *samples,
		size_t num,
		uint32_t rate)
{
	size_t block_samples;
	size_t block_start;
	size_t block_end;
	size_t i;
	double block_energy;
	double max_block_energy;

	if (samples == NULL || num == 0 || rate == 0) {
		return 0;
	}
	block_samples = ((size_t)rate * AVMD_SPECTRAL_ACTIVITY_BLOCK_MS) / 1000u;
	if (block_samples == 0) {
		block_samples = 1;
	}
	max_block_energy = 0.0;
	block_start = 0;
	while (block_start < num) {
		block_end = block_start + block_samples;
		if (block_end > num) {
			block_end = num;
		}
		block_energy = 0.0;
		for (i = block_start; i < block_end; ++i) {
			block_energy += samples[i] * samples[i];
		}
		block_energy /= (double)(block_end - block_start);
		if (block_energy > max_block_energy) {
			max_block_energy = block_energy;
		}
		block_start = block_end;
	}
	if (max_block_energy <= 0.0) {
		return 0;
	}

	block_start = 0;
	while (block_start < num) {
		block_end = block_start + block_samples;
		if (block_end > num) {
			block_end = num;
		}
		block_energy = 0.0;
		for (i = block_start; i < block_end; ++i) {
			block_energy += samples[i] * samples[i];
		}
		block_energy /= (double)(block_end - block_start);
		if (block_energy < AVMD_SPECTRAL_ACTIVITY_MIN_RATIO * max_block_energy) {
			return 0;
		}
		block_start = block_end;
	}
	return 1;
}

extern int avmd_spectral_window_has_continuous_candidate(
		const double *samples,
		size_t num,
		uint32_t rate,
		double candidate_frequency)
{
	size_t block_samples;
	size_t block_start;
	size_t block_end;
	size_t i;
	double mean;
	double total_energy;
	double centered;
	double candidate_power;
	double purity;

	if (samples == NULL || num == 0 || rate == 0 ||
			candidate_frequency <= 0.0 ||
			candidate_frequency >= 0.5 * (double)rate) {
		return 0;
	}
	block_samples = ((size_t)rate * AVMD_SPECTRAL_CANDIDATE_BLOCK_MS) /
			1000u;
	if (block_samples == 0) {
		block_samples = 1;
	}

	block_start = 0;
	while (block_start < num) {
		if (num - block_start < 2u * block_samples) {
			block_end = num;
		} else {
			block_end = block_start + block_samples;
		}
		mean = 0.0;
		for (i = block_start; i < block_end; ++i) {
			mean += samples[i];
		}
		mean /= (double)(block_end - block_start);
		total_energy = 0.0;
		for (i = block_start; i < block_end; ++i) {
			centered = samples[i] - mean;
			total_energy += centered * centered;
		}
		if (total_energy <= 0.0) {
			return 0;
		}
		candidate_power = avmd_goertzel(samples + block_start,
				block_end - block_start, rate, candidate_frequency, mean);
		purity = (2.0 * candidate_power) /
				((double)(block_end - block_start) * total_energy);
		if (purity < AVMD_SPECTRAL_CANDIDATE_MIN_PURITY) {
			return 0;
		}
		block_start = block_end;
	}
	return 1;
}

extern int avmd_spectral_analyze(const double *samples,
		size_t num,
		uint32_t rate,
		double candidate_frequency,
		double min_frequency,
		double max_frequency,
		double search_radius,
		double search_step,
		avmd_spectral_result_t *result)
{
	double mean = 0.0;
	double total_energy = 0.0;
	double start_frequency;
	double stop_frequency;
	double frequency;
	double power;
	double centered;
	double scale;
	double secondary_power = 0.0;
	double secondary_frequency = 0.0;
	size_t i;

	if (result == NULL) {
		return 0;
	}
	memset(result, 0, sizeof(*result));

	if (samples == NULL || num < 2 || rate == 0 ||
			candidate_frequency <= 0.0 || min_frequency <= 0.0 ||
			max_frequency <= min_frequency || search_radius <= 0.0 ||
			search_step <= 0.0 || max_frequency >= 0.5 * (double)rate) {
		return 0;
	}

	for (i = 0; i < num; i++) {
		mean += samples[i];
	}
	mean /= (double)num;

	for (i = 0; i < num; i++) {
		centered = samples[i] - mean;
		total_energy += centered * centered;
	}
	if (total_energy <= 0.0) {
		return 0;
	}

	start_frequency = candidate_frequency - search_radius;
	if (start_frequency < min_frequency) {
		start_frequency = min_frequency;
	}
	stop_frequency = candidate_frequency + search_radius;
	if (stop_frequency > max_frequency) {
		stop_frequency = max_frequency;
	}
	if (stop_frequency < start_frequency) {
		return 0;
	}

	frequency = start_frequency;
	while (frequency <= stop_frequency + (0.5 * search_step)) {
		power = avmd_goertzel(samples, num, rate, frequency, mean);
		if (power > result->dominant_power) {
			result->dominant_power = power;
			result->dominant_frequency = frequency;
		}
		frequency += search_step;
	}

	frequency = start_frequency;
	while (frequency <= stop_frequency + (0.5 * search_step)) {
		if (fabs(frequency - result->dominant_frequency) >= AVMD_SPECTRAL_SECONDARY_GUARD_HZ) {
			power = avmd_goertzel(samples, num, rate, frequency, mean);
			if (power > secondary_power) {
				secondary_power = power;
				secondary_frequency = frequency;
			}
		}
		frequency += search_step;
	}

	result->secondary_power = secondary_power;
	result->secondary_frequency = secondary_frequency;
	result->total_energy = total_energy;
	scale = 2.0 / ((double)num * total_energy);
	result->purity = result->dominant_power * scale;
	result->secondary_purity = result->secondary_power * scale;
	if (result->purity > 1.0) {
		result->purity = 1.0;
	}
	if (result->secondary_purity > 1.0) {
		result->secondary_purity = 1.0;
	}

	return 1;
}
