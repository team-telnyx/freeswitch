/*
 * @brief   Goertzel algorithm.
 *
 * Contributor(s):
 *
 * Eric des Courtis <eric.des.courtis@benbria.com>
 */


#ifndef __AVMD_GOERTZEL_H__
#define __AVMD_GOERTZEL_H__


#include <stddef.h>
#include <stdint.h>

#if !defined(M_PI)
	/* C99 systems may not define M_PI */
	#define M_PI 3.14159265358979323846264338327
#endif


/*! \brief Identify frequency components of a signal
 * @author Eric des Courtis
 * @param samples Linear PCM samples
 * @param num Number of samples to look at
 * @param rate Sample rate in Hertz
 * @param frequency_hz Frequency to look at in Hertz
 * @param mean Mean sample value to remove before analysis
 * @return A power estimate for frequency_hz in the supplied sample window
 */
extern double avmd_goertzel(const double *samples, size_t num, uint32_t rate, double frequency_hz, double mean);


#endif /* __AVMD_GOERTZEL_H__ */
