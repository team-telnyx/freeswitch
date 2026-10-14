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
 * switch_refresh_diag.c -- Opt-in timing of in-dialog SIP request processing
 *
 */

#include <switch.h>
#include <switch_refresh_diag.h>

#if defined(_MSC_VER)
#define RDIAG_THREAD_LOCAL __declspec(thread)
#else
#define RDIAG_THREAD_LOCAL __thread
#endif

static RDIAG_THREAD_LOCAL switch_refresh_diag_t *rdiag_current = NULL;

static const char *rdiag_stage_names[SWITCH_RDIAG_STAGE_MAX] = {
	"respond_100",
	"session_id",
	"set_vars",
	"recovery",
	"extra_headers",
	"reinvite_hook",
	"header_vars",
	"dup_response"
};

static const char *rdiag_lock_names[SWITCH_RDIAG_LOCK_MAX] = {
	"variable_iter",
	"set_variable",
	"get_variables",
	"rtp_stats",
	"sofia"
};

static const char *rdiag_recovery_names[] = {
	"none",
	"no_callback",
	"ineligible",
	"no_xml",
	"tracked"
};

SWITCH_DECLARE(void) switch_refresh_diag_init(switch_refresh_diag_t *diag, switch_time_t lock_threshold_us)
{
	memset(diag, 0, sizeof(*diag));
	diag->lock_threshold_us = lock_threshold_us;
}

SWITCH_DECLARE(void) switch_refresh_diag_push(switch_refresh_diag_t *diag)
{
	diag->prev = rdiag_current;
	rdiag_current = diag;
}

SWITCH_DECLARE(void) switch_refresh_diag_pop(switch_refresh_diag_t *diag)
{
	rdiag_current = diag->prev;
	diag->prev = NULL;
}

SWITCH_DECLARE(switch_refresh_diag_t *) switch_refresh_diag_current(void)
{
	return rdiag_current;
}

SWITCH_DECLARE(void) switch_refresh_diag_stage(switch_refresh_diag_t *diag, switch_refresh_diag_stage_t stage, switch_time_t elapsed_us)
{
	if (!diag || stage >= SWITCH_RDIAG_STAGE_MAX) {
		return;
	}

	diag->stage_us[stage] += elapsed_us;
	diag->stage_seen[stage] = 1;
}

SWITCH_DECLARE(void) switch_refresh_diag_lock_wait(switch_refresh_diag_t *diag, switch_refresh_diag_lock_t lock, switch_time_t waited_us)
{
	switch_refresh_diag_lock_stat_t *stat;

	if (!diag || lock >= SWITCH_RDIAG_LOCK_MAX || waited_us <= diag->lock_threshold_us) {
		return;
	}

	stat = &diag->locks[lock];
	stat->count++;
	stat->total_us += waited_us;
	if (waited_us > stat->max_us) {
		stat->max_us = waited_us;
	}
}

SWITCH_DECLARE(void) switch_refresh_diag_mutex_lock(switch_mutex_t *mutex, switch_refresh_diag_lock_t lock)
{
	switch_refresh_diag_t *diag = rdiag_current;
	switch_time_t start;

	if (!diag) {
		switch_mutex_lock(mutex);
		return;
	}

	start = switch_refresh_diag_now();
	switch_mutex_lock(mutex);
	switch_refresh_diag_lock_wait(diag, lock, switch_refresh_diag_now() - start);
}

SWITCH_DECLARE(switch_time_t) switch_refresh_diag_max_lock_wait(const switch_refresh_diag_t *diag)
{
	switch_time_t max = 0;
	int i;

	for (i = 0; i < SWITCH_RDIAG_LOCK_MAX; i++) {
		if (diag->locks[i].count && diag->locks[i].max_us > max) {
			max = diag->locks[i].max_us;
		}
	}

	return max;
}

static void rdiag_append(char *buf, switch_size_t len, switch_size_t *used, const char *fmt, ...)
{
	va_list ap;
	int r;

	if (*used + 1 >= len) {
		return;
	}

	va_start(ap, fmt);
	r = vsnprintf(buf + *used, len - *used, fmt, ap);
	va_end(ap);

	if (r < 0) {
		buf[*used] = '\0';
		return;
	}

	if ((switch_size_t) r >= len - *used) {
		*used = len - 1;
		buf[*used] = '\0';
		return;
	}

	*used += (switch_size_t) r;
}

SWITCH_DECLARE(switch_size_t) switch_refresh_diag_format(const switch_refresh_diag_t *diag, char *buf, switch_size_t len)
{
	switch_size_t used = 0;
	const char *sep = "";
	int i;

	if (!buf || !len) {
		return 0;
	}

	*buf = '\0';

	for (i = 0; i < SWITCH_RDIAG_STAGE_MAX; i++) {
		if (diag->stage_seen[i]) {
			rdiag_append(buf, len, &used, "%sstage_%s_us=%" SWITCH_TIME_T_FMT, sep, rdiag_stage_names[i], diag->stage_us[i]);
			sep = " ";
		}
	}

	if (diag->recovery_calls) {
		rdiag_append(buf, len, &used, "%srecovery_calls=%u recovery_status=%s", sep, diag->recovery_calls,
					 rdiag_recovery_names[diag->recovery_status]);
		sep = " ";

		if (diag->recovery_status == SWITCH_RDIAG_RECOVERY_TRACKED || diag->recovery_status == SWITCH_RDIAG_RECOVERY_NO_XML) {
			rdiag_append(buf, len, &used, " recovery_build_us=%" SWITCH_TIME_T_FMT " recovery_serialize_us=%" SWITCH_TIME_T_FMT
						 " recovery_xml_free_us=%" SWITCH_TIME_T_FMT,
						 diag->recovery_build_us, diag->recovery_serialize_us, diag->recovery_xml_free_us);
		}

		if (diag->recovery_status == SWITCH_RDIAG_RECOVERY_TRACKED) {
			rdiag_append(buf, len, &used, " recovery_callback_us=%" SWITCH_TIME_T_FMT " recovery_text_free_us=%" SWITCH_TIME_T_FMT
						 " recovery_bytes=%" SWITCH_SIZE_T_FMT,
						 diag->recovery_callback_us, diag->recovery_text_free_us, diag->recovery_bytes);
		}
	}

	if (diag->xml_builds) {
		rdiag_append(buf, len, &used, "%sxml_builds=%u xml_channel_us=%" SWITCH_TIME_T_FMT " xml_stats_us=%" SWITCH_TIME_T_FMT
					 " xml_vars_us=%" SWITCH_TIME_T_FMT " xml_history_us=%" SWITCH_TIME_T_FMT " xml_callflow_us=%" SWITCH_TIME_T_FMT
					 " xml_vars=%u xml_var_values=%u xml_err_seen=%u xml_err_emitted=%u xml_apps=%u xml_holds=%u"
					 " xml_profiles=%u xml_ext_apps=%u",
					 sep, diag->xml_builds, diag->xml_channel_us, diag->xml_stats_us, diag->xml_vars_us, diag->xml_history_us,
					 diag->xml_callflow_us, diag->xml_vars, diag->xml_var_values, diag->xml_err_seen, diag->xml_err_emitted,
					 diag->xml_apps, diag->xml_holds, diag->xml_profiles, diag->xml_ext_apps);
		sep = " ";
	}

	if (diag->hook_searches) {
		rdiag_append(buf, len, &used, "%shook_searches=%u hook_seen=%u hook_matched=%u hook_globals_us=%" SWITCH_TIME_T_FMT
					 " hook_channel_us=%" SWITCH_TIME_T_FMT " hook_merge_us=%" SWITCH_TIME_T_FMT " hook_exec_us=%" SWITCH_TIME_T_FMT,
					 sep, diag->hook_searches, diag->hook_seen, diag->hook_matched, diag->hook_globals_us, diag->hook_channel_us,
					 diag->hook_merge_us, diag->hook_exec_us);
		sep = " ";
	}

	for (i = 0; i < SWITCH_RDIAG_LOCK_MAX; i++) {
		const switch_refresh_diag_lock_stat_t *stat = &diag->locks[i];

		if (stat->count) {
			rdiag_append(buf, len, &used, "%slock_%s_count=%u lock_%s_max_us=%" SWITCH_TIME_T_FMT " lock_%s_total_us=%" SWITCH_TIME_T_FMT,
						 sep, rdiag_lock_names[i], stat->count, rdiag_lock_names[i], stat->max_us, rdiag_lock_names[i], stat->total_us);
			sep = " ";
		}
	}

	return used;
}

SWITCH_DECLARE(switch_bool_t) switch_refresh_diag_limit_take(switch_refresh_diag_limit_t *limit, switch_time_t now_us,
															 switch_time_t window_us, uint32_t max)
{
	/* A timestamp older than the window start (a caller that sampled the clock before another caller opened the
	   window) counts against the current window; only a timestamp a full window later opens a new one. */
	if (!limit->window_start || (now_us > limit->window_start && now_us - limit->window_start >= window_us)) {
		limit->window_start = now_us ? now_us : 1;
		limit->used = 0;
	}

	if (limit->used >= max) {
		limit->suppressed++;
		return SWITCH_FALSE;
	}

	limit->used++;
	return SWITCH_TRUE;
}

SWITCH_DECLARE(uint32_t) switch_refresh_diag_limit_drain_suppressed(switch_refresh_diag_limit_t *limit)
{
	uint32_t suppressed = limit->suppressed;

	limit->suppressed = 0;
	return suppressed;
}

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
