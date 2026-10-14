/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
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
 * Contributor(s):
 *
 * switch_refresh_diag.h -- Opt-in timing of in-dialog SIP request processing
 *
 */
/*!
  \file switch_refresh_diag.h
  \brief Opt-in timing of in-dialog SIP request processing

  An endpoint pushes a diagnostic context on its own thread around the processing of one SIP event.
  While the context is current, core code on that thread adds its stage durations, traversal counts
  and lock waits to it. The context is thread-local, so work done by other threads is never
  attributed to it. With no current context the instrumented core paths only check a thread-local
  pointer.
*/

#ifndef SWITCH_REFRESH_DIAG_H
#define SWITCH_REFRESH_DIAG_H

#include <switch.h>

SWITCH_BEGIN_EXTERN_C

typedef enum {
	SWITCH_RDIAG_LOCK_VARIABLE_ITER,
	SWITCH_RDIAG_LOCK_SET_VARIABLE,
	SWITCH_RDIAG_LOCK_GET_VARIABLES,
	SWITCH_RDIAG_LOCK_RTP_STATS,
	SWITCH_RDIAG_LOCK_SOFIA,
	SWITCH_RDIAG_LOCK_MAX
} switch_refresh_diag_lock_t;

typedef enum {
	SWITCH_RDIAG_STAGE_RESPOND_100,
	SWITCH_RDIAG_STAGE_SESSION_ID,
	SWITCH_RDIAG_STAGE_SET_VARS,
	SWITCH_RDIAG_STAGE_RECOVERY,
	SWITCH_RDIAG_STAGE_EXTRA_HEADERS,
	SWITCH_RDIAG_STAGE_REINVITE_HOOK,
	SWITCH_RDIAG_STAGE_HEADER_VARS,
	SWITCH_RDIAG_STAGE_DUP_RESPONSE,
	SWITCH_RDIAG_STAGE_MAX
} switch_refresh_diag_stage_t;

typedef enum {
	SWITCH_RDIAG_RECOVERY_NONE,
	SWITCH_RDIAG_RECOVERY_NO_CALLBACK,
	SWITCH_RDIAG_RECOVERY_INELIGIBLE,
	SWITCH_RDIAG_RECOVERY_NO_XML,
	SWITCH_RDIAG_RECOVERY_TRACKED
} switch_refresh_diag_recovery_t;

typedef struct {
	uint32_t count;
	switch_time_t max_us;
	switch_time_t total_us;
} switch_refresh_diag_lock_stat_t;

typedef struct switch_refresh_diag_s {
	switch_time_t lock_threshold_us;

	switch_time_t stage_us[SWITCH_RDIAG_STAGE_MAX];
	uint8_t stage_seen[SWITCH_RDIAG_STAGE_MAX];

	uint32_t recovery_calls;
	switch_refresh_diag_recovery_t recovery_status;
	switch_time_t recovery_build_us;
	switch_time_t recovery_serialize_us;
	switch_time_t recovery_xml_free_us;
	switch_time_t recovery_callback_us;
	switch_time_t recovery_text_free_us;
	switch_size_t recovery_bytes;

	uint32_t xml_builds;
	switch_time_t xml_channel_us;
	switch_time_t xml_stats_us;
	switch_time_t xml_vars_us;
	switch_time_t xml_history_us;
	switch_time_t xml_callflow_us;
	uint32_t xml_vars;
	uint32_t xml_var_values;
	uint32_t xml_err_seen;
	uint32_t xml_err_emitted;
	uint32_t xml_apps;
	uint32_t xml_holds;
	uint32_t xml_profiles;
	uint32_t xml_ext_apps;

	uint32_t hook_searches;
	uint32_t hook_seen;
	uint32_t hook_matched;
	switch_time_t hook_globals_us;
	switch_time_t hook_channel_us;
	switch_time_t hook_merge_us;
	switch_time_t hook_exec_us;

	switch_refresh_diag_lock_stat_t locks[SWITCH_RDIAG_LOCK_MAX];

	struct switch_refresh_diag_s *prev;
} switch_refresh_diag_t;

typedef struct {
	switch_time_t window_start;
	uint32_t used;
	uint32_t suppressed;
} switch_refresh_diag_limit_t;

/*! \brief Reset a context and set the lock-wait threshold in microseconds */
SWITCH_DECLARE(void) switch_refresh_diag_init(switch_refresh_diag_t *diag, switch_time_t lock_threshold_us);

/*! \brief Make diag the calling thread's current context, remembering the previous one */
SWITCH_DECLARE(void) switch_refresh_diag_push(switch_refresh_diag_t *diag);

/*! \brief Restore the context that was current before diag was pushed */
SWITCH_DECLARE(void) switch_refresh_diag_pop(switch_refresh_diag_t *diag);

/*! \brief The calling thread's current context, or NULL */
SWITCH_DECLARE(switch_refresh_diag_t *) switch_refresh_diag_current(void);

/*! \brief Add a stage duration */
SWITCH_DECLARE(void) switch_refresh_diag_stage(switch_refresh_diag_t *diag, switch_refresh_diag_stage_t stage, switch_time_t elapsed_us);

/*! \brief Record a lock wait when it is strictly longer than the context's threshold */
SWITCH_DECLARE(void) switch_refresh_diag_lock_wait(switch_refresh_diag_t *diag, switch_refresh_diag_lock_t lock, switch_time_t waited_us);

/*! \brief Lock a mutex, timing the acquisition when the calling thread has a current context */
SWITCH_DECLARE(void) switch_refresh_diag_mutex_lock(switch_mutex_t *mutex, switch_refresh_diag_lock_t lock);

/*! \brief The longest recorded lock wait across all locks */
SWITCH_DECLARE(switch_time_t) switch_refresh_diag_max_lock_wait(const switch_refresh_diag_t *diag);

/*!
  \brief Append the context's measurements to buf as space-separated key=value fields
  \return the number of characters written, excluding the terminating NUL

  Only measured values are written; a stage, section or lock that was not reached is left out
  rather than written as zero. No variable names or values are written.
*/
SWITCH_DECLARE(switch_size_t) switch_refresh_diag_format(const switch_refresh_diag_t *diag, char *buf, switch_size_t len);

/*!
  \brief Take one record from a fixed-window allowance
  \return SWITCH_TRUE when the record may be emitted; otherwise the suppressed count is increased
*/
SWITCH_DECLARE(switch_bool_t) switch_refresh_diag_limit_take(switch_refresh_diag_limit_t *limit, switch_time_t now_us,
															 switch_time_t window_us, uint32_t max);

/*! \brief Return and clear the number of records suppressed so far */
SWITCH_DECLARE(uint32_t) switch_refresh_diag_limit_drain_suppressed(switch_refresh_diag_limit_t *limit);

#define switch_refresh_diag_now() switch_mono_micro_time_now()

SWITCH_END_EXTERN_C
#endif
/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
