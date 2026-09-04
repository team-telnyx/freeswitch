/*
 * Bounded single-tone spectral confirmation for mod_avmd.
 */

#ifndef __AVMD_SPECTRAL_H__
#define __AVMD_SPECTRAL_H__

#include <stddef.h>
#include <stdint.h>

typedef struct {
	double dominant_frequency;
	double dominant_power;
	double secondary_frequency;
	double secondary_power;
	double total_energy;
	double purity;
	double secondary_purity;
} avmd_spectral_result_t;

#define AVMD_FAX_CNG_FREQUENCY_HZ (1100.0)
#define AVMD_FAX_CNG_TOLERANCE_HZ (40.0)

extern int avmd_spectral_is_fax_cng(double frequency);
extern int avmd_spectral_result_accepted(const avmd_spectral_result_t *result,
		double min_purity,
		uint8_t reject_fax_cng);

extern int avmd_spectral_window_is_continuous(const double *samples,
		size_t num,
		uint32_t rate);

extern int avmd_spectral_window_has_continuous_candidate(
		const double *samples,
		size_t num,
		uint32_t rate,
		double candidate_frequency);

/*
 * Search a narrow band around the DESA candidate. A true single tone should
 * explain most of the window energy. Equal-power dual tones explain roughly
 * half of it at either component and are rejected by a high purity threshold.
 */
extern int avmd_spectral_analyze(const double *samples,
		size_t num,
		uint32_t rate,
		double candidate_frequency,
		double min_frequency,
		double max_frequency,
		double search_radius,
		double search_step,
		avmd_spectral_result_t *result);

#endif /* __AVMD_SPECTRAL_H__ */
