/* Scan a PCMU fixture with the same bounded spectral helper used by mod_avmd. */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "avmd_spectral.h"

#define SCAN_RATE (8000u)
#define SCAN_MIN_FREQUENCY (1.0)
#define SCAN_MAX_FREQUENCY (2000.0)
#define SCAN_SEARCH_RADIUS (80.0)
#define SCAN_SEARCH_STEP (1.0)

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

static double parse_double(const char *value, const char *name)
{
	char *end;
	double parsed;

	errno = 0;
	parsed = strtod(value, &end);
	if (errno != 0 || end == value || *end != '\0' || !isfinite(parsed)) {
		fprintf(stderr, "invalid %s: %s\n", name, value);
		exit(EXIT_FAILURE);
	}
	return parsed;
}

static unsigned long parse_unsigned(const char *value, const char *name)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed == 0) {
		fprintf(stderr, "invalid %s: %s\n", name, value);
		exit(EXIT_FAILURE);
	}
	return parsed;
}

int main(int argc, char **argv)
{
	FILE *file;
	long file_size;
	uint8_t *encoded;
	double *samples;
	double candidate;
	double max_purity;
	double max_time;
	double max_secondary;
	double max_dominant_frequency;
	double max_secondary_frequency;
	double max_competing_purity;
	double competing_time;
	double competing_dominant_frequency;
	double competing_purity;
	double competing_secondary_frequency;
	double competing_secondary_purity;
	unsigned long window_ms;
	unsigned long step_ms;
	size_t window_samples;
	size_t step_samples;
	size_t offset;
	size_t index;
	avmd_spectral_result_t result;
	int status;

	if (argc != 5) {
		fprintf(stderr, "usage: %s <fixture.ulaw> <candidate-hz> <window-ms> <step-ms>\n", argv[0]);
		return EXIT_FAILURE;
	}
	candidate = parse_double(argv[2], "candidate frequency");
	window_ms = parse_unsigned(argv[3], "window duration");
	step_ms = parse_unsigned(argv[4], "step duration");
	window_samples = (SCAN_RATE * window_ms) / 1000u;
	step_samples = (SCAN_RATE * step_ms) / 1000u;
	if (window_samples < 2 || step_samples == 0) {
		fprintf(stderr, "window and step must produce at least one sample\n");
		return EXIT_FAILURE;
	}

	file = fopen(argv[1], "rb");
	if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
		perror(argv[1]);
		return EXIT_FAILURE;
	}
	file_size = ftell(file);
	if (file_size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
		fprintf(stderr, "empty or unreadable fixture: %s\n", argv[1]);
		fclose(file);
		return EXIT_FAILURE;
	}
	encoded = (uint8_t *)malloc((size_t)file_size);
	samples = (double *)malloc((size_t)file_size * sizeof(*samples));
	if (encoded == NULL || samples == NULL || fread(encoded, 1, (size_t)file_size, file) != (size_t)file_size) {
		fprintf(stderr, "unable to load fixture: %s\n", argv[1]);
		free(encoded);
		free(samples);
		fclose(file);
		return EXIT_FAILURE;
	}
	fclose(file);
	for (index = 0; index < (size_t)file_size; index++) {
		samples[index] = (double)decode_ulaw(encoded[index]);
	}
	free(encoded);

	max_purity = 0.0;
	max_time = 0.0;
	max_secondary = 0.0;
	max_dominant_frequency = 0.0;
	max_secondary_frequency = 0.0;
	max_competing_purity = 0.0;
	competing_time = 0.0;
	competing_dominant_frequency = 0.0;
	competing_purity = 0.0;
	competing_secondary_frequency = 0.0;
	competing_secondary_purity = 0.0;
	for (offset = 0; offset + window_samples <= (size_t)file_size; offset += step_samples) {
		status = avmd_spectral_analyze(samples + offset,
				window_samples,
				SCAN_RATE,
				candidate,
				SCAN_MIN_FREQUENCY,
				SCAN_MAX_FREQUENCY,
				SCAN_SEARCH_RADIUS,
				SCAN_SEARCH_STEP,
				&result);
		if (status == 1 && result.purity > max_purity) {
			max_purity = result.purity;
			max_time = (double)offset / SCAN_RATE;
			max_secondary = result.secondary_purity;
			max_dominant_frequency = result.dominant_frequency;
			max_secondary_frequency = result.secondary_frequency;
		}
		if (status == 1 && result.secondary_purity > max_competing_purity) {
			max_competing_purity = result.secondary_purity;
			competing_time = (double)offset / SCAN_RATE;
			competing_dominant_frequency = result.dominant_frequency;
			competing_purity = result.purity;
			competing_secondary_frequency = result.secondary_frequency;
			competing_secondary_purity = result.secondary_purity;
		}
	}
	free(samples);
	printf("fixture=%s candidate_hz=%.2f window_ms=%lu step_ms=%lu max_time_s=%.3f dominant_hz=%.2f purity=%.6f secondary_hz=%.2f secondary_purity=%.6f competing_time_s=%.3f competing_dominant_hz=%.2f competing_purity=%.6f competing_secondary_hz=%.2f competing_secondary_purity=%.6f\n",
			argv[1], candidate, window_ms, step_ms, max_time, max_dominant_frequency,
			max_purity, max_secondary_frequency, max_secondary, competing_time,
			competing_dominant_frequency, competing_purity, competing_secondary_frequency,
			competing_secondary_purity);
	return EXIT_SUCCESS;
}
