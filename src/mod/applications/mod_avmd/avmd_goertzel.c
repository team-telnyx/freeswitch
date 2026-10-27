/*
 * Contributor(s):
 *
 * Eric des Courtis <eric.des.courtis@benbria.com>
 */


#include <math.h>

#ifndef __AVMD_GOERTZEL_H__
	#include "avmd_goertzel.h"
#endif

extern double avmd_goertzel(const double *samples, size_t num, uint32_t rate, double frequency_hz, double mean)
{
	double s = 0.0;
	double p = 0.0;
	double p2 = 0.0;
	double coeff;
	size_t i;

	if (samples == NULL || num == 0 || rate == 0 || frequency_hz <= 0.0 || frequency_hz >= 0.5 * (double)rate) {
		return 0.0;
	}

	coeff = 2.0 * cos((2.0 * M_PI * frequency_hz) / (double)rate);

	for (i = 0; i < num; i++) {
		s = (samples[i] - mean) + (coeff * p) - p2;
		p2 = p;
		p = s;
	}

	return (p2 * p2) + (p * p) - (coeff * p2 * p);
}
