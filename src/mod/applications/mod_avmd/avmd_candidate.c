#include <math.h>
#include <stddef.h>

#include "avmd_candidate.h"

void avmd_candidate_segment_reset(avmd_candidate_segment_t *segment)
{
	if (segment == NULL) {
		return;
	}

	segment->start_sample = 0;
	segment->end_sample = 0;
	segment->frequency = 0.0;
	segment->active = 0;
	segment->starts_at_frame_start = 0;
	segment->detection_seen = 0;
}

void avmd_candidate_segment_observe_frequency(avmd_candidate_segment_t *segment,
		size_t sample_position,
		double frequency,
		uint8_t valid,
		uint8_t first_evaluated_sample)
{
	double tolerance;

	if (segment == NULL) {
		return;
	}
	if (!valid || frequency <= 0.0) {
		avmd_candidate_segment_reset(segment);
		return;
	}
	tolerance = 0.05 * fabs(segment->frequency);
	if (tolerance < 20.0) {
		tolerance = 20.0;
	}
	if (!segment->active || fabs(frequency - segment->frequency) > tolerance) {
		avmd_candidate_segment_reset(segment);
		segment->start_sample = sample_position;
		segment->end_sample = sample_position;
		segment->frequency = frequency;
		segment->active = 1;
		segment->starts_at_frame_start = first_evaluated_sample;
		return;
	}
	segment->end_sample = sample_position;
	segment->frequency = 0.5 * (segment->frequency + frequency);
}

void avmd_candidate_segment_mark_detection(avmd_candidate_segment_t *segment)
{
	if (segment != NULL && segment->active) {
		segment->detection_seen = 1;
	}
}

size_t avmd_candidate_segment_samples(const avmd_candidate_segment_t *segment)
{
	if (segment == NULL || !segment->active ||
			segment->end_sample < segment->start_sample) {
		return 0;
	}
	return segment->end_sample - segment->start_sample + 1;
}

void avmd_candidate_reset(avmd_candidate_state_t *state)
{
	if (state == NULL) {
		return;
	}

	state->samples = 0;
	state->last_confirmation_samples = 0;
	state->confirmed_samples = 0;
	state->confirmed_start_sample = 0;
	state->confirmed_end_sample = 0;
	state->next_confirmation_sample = 0;
	state->frequency = 0.0;
	state->active = 0;
	state->confirmation_attempts = 0;
}

void avmd_candidate_observe(avmd_candidate_state_t *state,
		double frequency,
		double tolerance,
		size_t qualifying_samples,
		uint8_t continues_from_previous_frame,
		size_t rearm_samples,
		uint8_t max_confirmation_attempts)
{
	if (state == NULL || qualifying_samples == 0) {
		if (state != NULL) {
			avmd_candidate_reset(state);
		}
		return;
	}

	if (!state->active || !continues_from_previous_frame ||
			fabs(frequency - state->frequency) > tolerance) {
		state->samples = qualifying_samples;
		state->last_confirmation_samples = 0;
		state->confirmed_samples = 0;
		state->confirmed_start_sample = 0;
		state->confirmed_end_sample = 0;
		state->next_confirmation_sample = 0;
		state->frequency = frequency;
		state->active = 1;
		state->confirmation_attempts = 0;
		return;
	}

	/* Bound retries for an otherwise continuous candidate. */
	if (state->confirmation_attempts >= max_confirmation_attempts) {
		if (state->samples - state->last_confirmation_samples >= rearm_samples) {
			avmd_candidate_reset(state);
			state->samples = qualifying_samples;
			state->frequency = frequency;
			state->active = 1;
			return;
		}
		state->samples += qualifying_samples;
		state->frequency = 0.5 * (state->frequency + frequency);
		return;
	}

	state->samples += qualifying_samples;
	state->frequency = 0.5 * (state->frequency + frequency);
}

int avmd_candidate_confirmation_due(const avmd_candidate_state_t *state,
		size_t required_samples,
		size_t retry_samples,
		uint8_t max_confirmation_attempts)
{
	if (state == NULL || !state->active ||
			state->samples < required_samples ||
			state->confirmation_attempts >= max_confirmation_attempts) {
		return 0;
	}

	if (state->last_confirmation_samples != 0 &&
			state->samples - state->last_confirmation_samples < retry_samples) {
		return 0;
	}

	return 1;
}

void avmd_candidate_record_acceptance(avmd_candidate_state_t *state,
		size_t start_sample,
		size_t end_sample)
{
	if (state == NULL || end_sample <= start_sample) {
		return;
	}
	if (state->confirmed_samples == 0 ||
			start_sample > state->confirmed_end_sample ||
			end_sample < state->confirmed_start_sample) {
		state->confirmed_start_sample = start_sample;
		state->confirmed_end_sample = end_sample;
	} else {
		if (start_sample < state->confirmed_start_sample) {
			state->confirmed_start_sample = start_sample;
		}
		if (end_sample > state->confirmed_end_sample) {
			state->confirmed_end_sample = end_sample;
		}
	}
	state->confirmed_samples = state->confirmed_end_sample -
			state->confirmed_start_sample;
	state->confirmation_attempts = 0;
	state->last_confirmation_samples = state->samples;
}

void avmd_candidate_record_rejection(avmd_candidate_state_t *state)
{
	if (state == NULL) {
		return;
	}
	state->confirmed_samples = 0;
	state->confirmed_start_sample = 0;
	state->confirmed_end_sample = 0;
	++state->confirmation_attempts;
	state->last_confirmation_samples = state->samples;
}

size_t avmd_candidate_confirmation_window_end(avmd_candidate_state_t *state,
		size_t total_samples,
		size_t window_samples,
		size_t available_history_samples)
{
	size_t candidate_start_sample;
	size_t end_sample;
	size_t age;

	if (state == NULL || window_samples == 0 ||
			available_history_samples < window_samples) {
		return 0;
	}
	if (state->next_confirmation_sample == 0) {
		if (state->samples < window_samples || state->samples > total_samples) {
			return 0;
		}
		candidate_start_sample = total_samples - state->samples;
		state->next_confirmation_sample = candidate_start_sample + window_samples;
	}
	if (total_samples < state->next_confirmation_sample) {
		return 0;
	}
	end_sample = state->next_confirmation_sample;
	age = total_samples - end_sample;
	if (end_sample < window_samples ||
			age > available_history_samples ||
			window_samples > available_history_samples - age) {
		end_sample = total_samples;
	}
	if (end_sample < window_samples) {
		return 0;
	}
	return end_sample;
}

size_t avmd_candidate_frame_position(size_t detector_position,
		size_t session_position,
		size_t overlap_samples,
		uint8_t lagged,
		uint8_t hardened)
{
	if (hardened) {
		return session_position;
	}
	if (lagged) {
		return detector_position + overlap_samples;
	}
	return detector_position;
}
