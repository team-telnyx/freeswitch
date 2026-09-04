/* Candidate continuity state shared by runtime and standalone tests. */

#ifndef AVMD_CANDIDATE_H
#define AVMD_CANDIDATE_H

#include <stddef.h>
#include <stdint.h>

typedef struct avmd_candidate_state {
	size_t samples;
	size_t last_confirmation_samples;
	size_t confirmed_samples;
	size_t confirmed_start_sample;
	size_t confirmed_end_sample;
	size_t next_confirmation_sample;
	double frequency;
	uint8_t active;
	uint8_t confirmation_attempts;
} avmd_candidate_state_t;

typedef struct avmd_candidate_segment {
	size_t start_sample;
	size_t end_sample;
	double frequency;
	uint8_t active;
	uint8_t starts_at_frame_start;
	uint8_t detection_seen;
} avmd_candidate_segment_t;

void avmd_candidate_segment_reset(avmd_candidate_segment_t *segment);

void avmd_candidate_segment_observe_frequency(avmd_candidate_segment_t *segment,
		size_t sample_position,
		double frequency,
		uint8_t valid,
		uint8_t first_evaluated_sample);

void avmd_candidate_segment_mark_detection(avmd_candidate_segment_t *segment);

size_t avmd_candidate_segment_samples(const avmd_candidate_segment_t *segment);

void avmd_candidate_reset(avmd_candidate_state_t *state);

void avmd_candidate_observe(avmd_candidate_state_t *state,
		double frequency,
		double tolerance,
		size_t qualifying_samples,
		uint8_t continues_from_previous_frame,
		size_t rearm_samples,
		uint8_t max_confirmation_attempts);

int avmd_candidate_confirmation_due(const avmd_candidate_state_t *state,
		size_t required_samples,
		size_t retry_samples,
		uint8_t max_confirmation_attempts);

void avmd_candidate_record_acceptance(avmd_candidate_state_t *state,
		size_t start_sample,
		size_t end_sample);

void avmd_candidate_record_rejection(avmd_candidate_state_t *state);

size_t avmd_candidate_confirmation_window_end(avmd_candidate_state_t *state,
		size_t total_samples,
		size_t window_samples,
		size_t available_history_samples);

size_t avmd_candidate_frame_position(size_t detector_position,
		size_t session_position,
		size_t overlap_samples,
		uint8_t lagged,
		uint8_t hardened);

#endif
