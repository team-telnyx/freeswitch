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

#if defined(__ATOMIC_RELAXED)
SWITCH_DECLARE_DATA uint32_t switch_refresh_telemetry_users = 0;
#define RDIAG_USERS_ADD(n) __atomic_add_fetch(&switch_refresh_telemetry_users, (n), __ATOMIC_RELAXED)
#else
SWITCH_DECLARE_DATA uint32_t switch_refresh_telemetry_users = 1;
#define RDIAG_USERS_ADD(n)
#endif

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
	RDIAG_USERS_ADD(1);
	diag->prev = rdiag_current;
	rdiag_current = diag;
}

SWITCH_DECLARE(void) switch_refresh_diag_pop(switch_refresh_diag_t *diag)
{
	rdiag_current = diag->prev;
	diag->prev = NULL;
	RDIAG_USERS_ADD(-1);
}

SWITCH_DECLARE(switch_refresh_diag_t *) switch_refresh_diag_current(void)
{
	return SWITCH_REFRESH_TELEMETRY_IDLE() ? NULL : rdiag_current;
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
	switch_refresh_diag_t *diag = SWITCH_REFRESH_TELEMETRY_IDLE() ? NULL : rdiag_current;
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

/* Session-thread stall observer.

   The owner thread is the only writer of a watch's stack, its published "current operation" fields and its ring
   head; the observer is the only writer of the ring tail and of the fields under "observer only". Every field both
   sides touch is accessed with atomic builtins, so there is no plain-data race. The current operation is published
   with a sequence counter (odd while the fields change); a reader that sees it change skips the watch for this pass
   instead of retrying. A watch is reached by the observer only through the registry, under rwatch.mutex, so
   switch_refresh_watch_destroy (which unlinks under that mutex) can free it at once. */

#if defined(__ATOMIC_ACQUIRE) && defined(__GCC_ATOMIC_INT_LOCK_FREE) && defined(__GCC_ATOMIC_LLONG_LOCK_FREE) && \
	__GCC_ATOMIC_INT_LOCK_FREE == 2 && __GCC_ATOMIC_LLONG_LOCK_FREE == 2 && __GCC_ATOMIC_POINTER_LOCK_FREE == 2
#define RWATCH_SUPPORTED 1
#define RWATCH_LOAD(_p, _o) __atomic_load_n(_p, _o)
#define RWATCH_STORE(_p, _v, _o) __atomic_store_n(_p, _v, _o)
#define RWATCH_XCHG(_p, _v) __atomic_exchange_n(_p, _v, __ATOMIC_RELAXED)
#define RWATCH_ADD(_p, _v) __atomic_add_fetch(_p, _v, __ATOMIC_RELAXED)
#define RWATCH_FENCE(_o) __atomic_thread_fence(_o)
#else
#define RWATCH_SUPPORTED 0
#endif

#define RWATCH_DEPTH 8
#define RWATCH_RING 16
#define RWATCH_POLL_US 20000
#define RWATCH_CALL_WINDOW_US 5000000
#define RWATCH_CALL_MAX 3
#define RWATCH_PROCESS_WINDOW_US 1000000
#define RWATCH_PROCESS_MAX 100
#define RWATCH_BATCH 256
#define RWATCH_ORPHANS 256
#define RWATCH_HEALTH_US 60000000

static const char *rwatch_stage_names[SWITCH_RWATCH_STAGE_MAX] = {
	"read",
	"write",
	"bug_tap_native_read",
	"bug_read_replace",
	"bug_read",
	"bug_read_ping",
	"bug_tap_native_write",
	"bug_write",
	"bug_write_replace"
};

typedef struct {
	uint64_t op;
	switch_time_t start;
	switch_core_session_t *target;
	uint8_t stage;
} rwatch_frame_t;

typedef struct {
	uint64_t op;
	uint64_t parent_op;
	switch_time_t start;
	switch_time_t elapsed;
	uint8_t stage;
	uint8_t depth;
	char target[SWITCH_UUID_FORMATTED_LENGTH + 1];
} rwatch_entry_t;

typedef struct {
	rwatch_entry_t e;
	uint8_t active;
	uint8_t cross;
	char owner[SWITCH_UUID_FORMATTED_LENGTH + 1];
	uint32_t lost;
	uint32_t overflow;
	uint32_t suppressed_call;
} rwatch_record_t;

struct switch_refresh_watch_s {
	/* set at creation */
	char uuid[SWITCH_UUID_FORMATTED_LENGTH + 1];
	switch_channel_t *channel;
	switch_time_t stall_us;

	/* owner thread only */
	int depth;
	uint64_t next_op;
	uint32_t seq;
	rwatch_frame_t stack[RWATCH_DEPTH];

	/* shared, atomic */
	uint32_t armed;
	uint32_t pub_seq;
	uint64_t pub_op;
	uint64_t pub_parent_op;
	int64_t pub_start;
	uint32_t pub_info;
	uint32_t ring_head;
	uint32_t ring_tail;
	uint32_t lost;
	uint32_t overflow;
	rwatch_entry_t ring[RWATCH_RING];

	/* observer only, under rwatch.mutex */
	uint64_t active_reported[RWATCH_DEPTH + 1];	/* by depth: the op last reported active at that depth */
	uint32_t carry_lost;
	uint32_t carry_overflow;
	switch_refresh_diag_limit_t limit;
	struct switch_refresh_watch_s *prev;
	struct switch_refresh_watch_s *next;
};

static struct {
	switch_memory_pool_t *pool;
	switch_mutex_t *mutex;			/* registry, orphans, health totals, observer start/stop */
	switch_mutex_t *poll_mutex;		/* one observer pass at a time; the batch and process limit */
	switch_refresh_watch_t *head;
	uint32_t count;
	switch_thread_t *thread;
	uint32_t running;
	uint32_t stopping;
	rwatch_record_t orphans[RWATCH_ORPHANS];
	uint32_t orphan_count;
	uint64_t lost_total;
	uint64_t overflow_total;
	uint64_t suppressed_call_total;
	uint64_t suppressed_process_total;
	uint64_t orphan_dropped_total;
	uint64_t reported_sum;
	switch_time_t health_last;
	switch_refresh_diag_limit_t process_limit;
	rwatch_record_t batch[RWATCH_BATCH];
} rwatch;

static RDIAG_THREAD_LOCAL switch_refresh_watch_t *rwatch_current = NULL;

SWITCH_DECLARE(void) switch_refresh_watch_bind(switch_refresh_watch_t *watch)
{
	rwatch_current = watch;
}

SWITCH_DECLARE(switch_refresh_watch_t *) switch_refresh_watch_current(void)
{
	return SWITCH_REFRESH_TELEMETRY_IDLE() ? NULL : rwatch_current;
}

#if RWATCH_SUPPORTED

SWITCH_DECLARE(void) switch_refresh_watch_init(switch_memory_pool_t *pool)
{
	if (rwatch.mutex) {
		return;
	}

	switch_core_new_memory_pool(&rwatch.pool);
	switch_mutex_init(&rwatch.mutex, SWITCH_MUTEX_NESTED, rwatch.pool);
	switch_mutex_init(&rwatch.poll_mutex, SWITCH_MUTEX_NESTED, rwatch.pool);
}

static void *SWITCH_THREAD_FUNC rwatch_observer_run(switch_thread_t *thread, void *obj)
{
	while (!RWATCH_LOAD(&rwatch.stopping, __ATOMIC_ACQUIRE)) {
		switch_refresh_watch_poll();
		switch_yield(RWATCH_POLL_US);
	}

	return NULL;
}

SWITCH_DECLARE(switch_status_t) switch_refresh_watch_observer_start(void)
{
	switch_threadattr_t *thd_attr = NULL;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (!rwatch.mutex) {
		return SWITCH_STATUS_FALSE;
	}

	switch_mutex_lock(rwatch.mutex);
	if (!rwatch.thread && !RWATCH_LOAD(&rwatch.stopping, __ATOMIC_RELAXED)) {
		switch_threadattr_create(&thd_attr, rwatch.pool);
		switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
		if ((status = switch_thread_create(&rwatch.thread, thd_attr, rwatch_observer_run, NULL, rwatch.pool)) != SWITCH_STATUS_SUCCESS) {
			rwatch.thread = NULL;
		}
	}
	switch_mutex_unlock(rwatch.mutex);

	return status;
}

SWITCH_DECLARE(void) switch_refresh_watch_shutdown(void)
{
	switch_thread_t *thread;
	switch_status_t st;

	if (!rwatch.mutex) {
		return;
	}

	switch_mutex_lock(rwatch.mutex);
	RWATCH_STORE(&rwatch.stopping, 1, __ATOMIC_RELEASE);
	thread = rwatch.thread;
	rwatch.thread = NULL;
	switch_mutex_unlock(rwatch.mutex);

	if (thread) {
		switch_thread_join(&st, thread);
	}

	switch_refresh_watch_poll();
}

SWITCH_DECLARE(switch_refresh_watch_t *) switch_refresh_watch_create(const char *uuid, switch_channel_t *channel, switch_time_t stall_us)
{
	switch_refresh_watch_t *watch;

	if (!rwatch.mutex || zstr(uuid) || !channel || stall_us <= 0) {
		return NULL;
	}

	if (!(watch = calloc(1, sizeof(*watch)))) {
		return NULL;
	}

	switch_copy_string(watch->uuid, uuid, sizeof(watch->uuid));
	watch->channel = channel;
	watch->stall_us = stall_us;

	switch_mutex_lock(rwatch.mutex);
	watch->next = rwatch.head;
	if (rwatch.head) {
		rwatch.head->prev = watch;
	}
	rwatch.head = watch;
	rwatch.count++;
	switch_mutex_unlock(rwatch.mutex);
	RDIAG_USERS_ADD(1);

	return watch;
}

SWITCH_DECLARE(void) switch_refresh_watch_arm(switch_refresh_watch_t *watch)
{
	if (watch) {
		RWATCH_STORE(&watch->armed, 1, __ATOMIC_RELAXED);
	}
}

/* Owner thread: publish the innermost open operation (op 0: none). */
static void rwatch_publish(switch_refresh_watch_t *watch)
{
	const rwatch_frame_t *f = watch->depth ? &watch->stack[watch->depth - 1] : NULL;

	RWATCH_STORE(&watch->pub_seq, ++watch->seq, __ATOMIC_RELAXED);
	RWATCH_FENCE(__ATOMIC_RELEASE);
	RWATCH_STORE(&watch->pub_op, f ? f->op : 0, __ATOMIC_RELAXED);
	RWATCH_STORE(&watch->pub_parent_op, watch->depth > 1 ? watch->stack[watch->depth - 2].op : 0, __ATOMIC_RELAXED);
	RWATCH_STORE(&watch->pub_start, f ? (int64_t) f->start : 0, __ATOMIC_RELAXED);
	RWATCH_STORE(&watch->pub_info, f ? (uint32_t) f->stage | ((uint32_t) watch->depth << 8) | (f->target ? 1u << 16 : 0) : 0,
				 __ATOMIC_RELAXED);
	RWATCH_STORE(&watch->pub_seq, ++watch->seq, __ATOMIC_RELEASE);
}

SWITCH_DECLARE(int) switch_refresh_watch_begin(switch_refresh_watch_t *watch, switch_refresh_watch_stage_t stage, switch_core_session_t *target)
{
	rwatch_frame_t *f;

	if (!watch || stage >= SWITCH_RWATCH_STAGE_MAX || !RWATCH_LOAD(&watch->armed, __ATOMIC_RELAXED)) {
		return -1;
	}

	if (watch->depth >= RWATCH_DEPTH) {
		RWATCH_ADD(&watch->overflow, 1);
		return -1;
	}

	f = &watch->stack[watch->depth];
	f->op = ++watch->next_op;
	f->stage = (uint8_t) stage;
	f->target = target && switch_core_session_get_channel(target) != watch->channel ? target : NULL;
	f->start = switch_refresh_diag_now();
	watch->depth++;
	rwatch_publish(watch);

	return watch->depth - 1;
}

SWITCH_DECLARE(void) switch_refresh_watch_end(switch_refresh_watch_t *watch, int handle)
{
	rwatch_frame_t *f;
	rwatch_entry_t *e;
	switch_time_t elapsed;
	uint32_t head;

	if (!watch || handle < 0 || handle >= watch->depth) {
		return;
	}

	f = &watch->stack[handle];
	elapsed = switch_refresh_diag_now() - f->start;
	watch->depth = handle;
	rwatch_publish(watch);

	if (elapsed <= watch->stall_us) {
		return;
	}

	head = RWATCH_LOAD(&watch->ring_head, __ATOMIC_RELAXED);
	if (head - RWATCH_LOAD(&watch->ring_tail, __ATOMIC_ACQUIRE) >= RWATCH_RING) {
		RWATCH_ADD(&watch->lost, 1);
		return;
	}

	e = &watch->ring[head % RWATCH_RING];
	e->op = f->op;
	e->parent_op = handle ? watch->stack[handle - 1].op : 0;
	e->start = f->start;
	e->elapsed = elapsed;
	e->stage = f->stage;
	e->depth = (uint8_t) (handle + 1);
	if (f->target) {
		switch_copy_string(e->target, switch_core_session_get_uuid(f->target), sizeof(e->target));
	} else {
		e->target[0] = '\0';
	}
	RWATCH_STORE(&watch->ring_head, head + 1, __ATOMIC_RELEASE);
}

/* Observer, under rwatch.mutex: fill out with what the watch has to report (ring entries, then an active operation
   past the threshold unless include_active is false), applying the per-call allowance. Returns the number added. */
static uint32_t rwatch_collect(switch_refresh_watch_t *watch, switch_time_t now, rwatch_record_t *out, uint32_t room, int include_active)
{
	uint32_t n = 0, lost, overflow, tail, head;
	rwatch_record_t rec;
	int have;

	if ((lost = RWATCH_XCHG(&watch->lost, 0))) {
		watch->carry_lost += lost;
		rwatch.lost_total += lost;
	}

	if ((overflow = RWATCH_XCHG(&watch->overflow, 0))) {
		watch->carry_overflow += overflow;
		rwatch.overflow_total += overflow;
	}

	tail = RWATCH_LOAD(&watch->ring_tail, __ATOMIC_RELAXED);
	head = RWATCH_LOAD(&watch->ring_head, __ATOMIC_ACQUIRE);

	for (;;) {
		have = 0;
		memset(&rec, 0, sizeof(rec));

		if (tail != head) {
			if (n >= room) {
				break;
			}
			rec.e = watch->ring[tail % RWATCH_RING];
			tail++;
			have = 1;
		} else if (include_active) {
			uint32_t s1, s2, info;

			include_active = 0;
			s1 = RWATCH_LOAD(&watch->pub_seq, __ATOMIC_ACQUIRE);
			rec.e.op = RWATCH_LOAD(&watch->pub_op, __ATOMIC_RELAXED);
			rec.e.parent_op = RWATCH_LOAD(&watch->pub_parent_op, __ATOMIC_RELAXED);
			rec.e.start = (switch_time_t) RWATCH_LOAD(&watch->pub_start, __ATOMIC_RELAXED);
			info = RWATCH_LOAD(&watch->pub_info, __ATOMIC_RELAXED);
			RWATCH_FENCE(__ATOMIC_ACQUIRE);
			s2 = RWATCH_LOAD(&watch->pub_seq, __ATOMIC_RELAXED);

			rec.e.depth = (uint8_t) ((info >> 8) & 0xff);

			/* Remembered per depth: a parent seen again after its nested operation ended keeps its report. */
			if (!(s1 & 1) && s1 == s2 && rec.e.op && rec.e.depth >= 1 && rec.e.depth <= RWATCH_DEPTH &&
				rec.e.op != watch->active_reported[rec.e.depth] && now > rec.e.start && now - rec.e.start > watch->stall_us &&
				n < room) {
				watch->active_reported[rec.e.depth] = rec.e.op;
				rec.e.elapsed = now - rec.e.start;
				rec.e.stage = (uint8_t) (info & 0xff);
				rec.cross = (info >> 16) & 1;
				rec.active = 1;
				have = 1;
			}
		}

		if (!have) {
			break;
		}

		/* Counted in the totals when it happens: a watch destroyed before its next record keeps no other trace. */
		if (!switch_refresh_diag_limit_take(&watch->limit, now, RWATCH_CALL_WINDOW_US, RWATCH_CALL_MAX)) {
			rwatch.suppressed_call_total++;
			continue;
		}

		switch_copy_string(rec.owner, watch->uuid, sizeof(rec.owner));
		rec.suppressed_call = switch_refresh_diag_limit_drain_suppressed(&watch->limit);
		rec.lost = watch->carry_lost;
		rec.overflow = watch->carry_overflow;
		watch->carry_lost = watch->carry_overflow = 0;
		out[n++] = rec;
	}

	RWATCH_STORE(&watch->ring_tail, tail, __ATOMIC_RELEASE);

	return n;
}

SWITCH_DECLARE(void) switch_refresh_watch_destroy(switch_refresh_watch_t **watchp)
{
	switch_refresh_watch_t *watch;

	if (!watchp || !(watch = *watchp)) {
		return;
	}

	*watchp = NULL;

	if (rwatch_current == watch) {
		rwatch_current = NULL;
	}

	/* The owner thread no longer times anything on this watch: its records go to the orphan list for the next pass
	   (what does not fit is counted), and its active operation, if any, is not reported. */
	switch_mutex_lock(rwatch.mutex);
	rwatch.orphan_count += rwatch_collect(watch, switch_refresh_diag_now(), rwatch.orphans + rwatch.orphan_count,
										  RWATCH_ORPHANS - rwatch.orphan_count, 0);
	rwatch.orphan_dropped_total += RWATCH_LOAD(&watch->ring_head, __ATOMIC_RELAXED) - RWATCH_LOAD(&watch->ring_tail, __ATOMIC_RELAXED);

	if (watch->prev) {
		watch->prev->next = watch->next;
	} else {
		rwatch.head = watch->next;
	}
	if (watch->next) {
		watch->next->prev = watch->prev;
	}
	rwatch.count--;
	switch_mutex_unlock(rwatch.mutex);
	RDIAG_USERS_ADD(-1);

	free(watch);
}

static void rwatch_emit(const rwatch_record_t *rec, uint32_t suppressed_process)
{
	char target[SWITCH_UUID_FORMATTED_LENGTH + 16] = "";

	if (*rec->e.target) {
		switch_snprintf(target, sizeof(target), " target=%s", rec->e.target);
	} else if (rec->cross) {
		switch_snprintf(target, sizeof(target), " target=other");
	}

	switch_log_printf(SWITCH_CHANNEL_UUID_LOG(rec->owner), SWITCH_LOG_NOTICE,
					  "SIP_REFRESH_WORK schema=1 kind=stall status=%s stage=%s op=%" SWITCH_UINT64_T_FMT " parent_op=%" SWITCH_UINT64_T_FMT
					  " depth=%u elapsed_us=%" SWITCH_TIME_T_FMT " start_mono_us=%" SWITCH_TIME_T_FMT "%s lost=%u overflow=%u"
					  " suppressed_call=%u suppressed_process=%u\n",
					  rec->active ? "active" : "completed", rwatch_stage_names[rec->e.stage], rec->e.op, rec->e.parent_op,
					  rec->e.depth, rec->e.elapsed, rec->e.start, target, rec->lost, rec->overflow, rec->suppressed_call,
					  suppressed_process);
}

SWITCH_DECLARE(void) switch_refresh_watch_poll(void)
{
	switch_refresh_watch_t *watch;
	switch_time_t now;
	uint32_t n = 0, i, watches;
	uint64_t sum;

	if (!rwatch.mutex) {
		return;
	}

	switch_mutex_lock(rwatch.poll_mutex);

	switch_mutex_lock(rwatch.mutex);
	now = switch_refresh_diag_now();

	for (i = 0; i < rwatch.orphan_count && n < RWATCH_BATCH; i++) {
		rwatch.batch[n++] = rwatch.orphans[i];
	}
	if (i < rwatch.orphan_count) {
		memmove(rwatch.orphans, rwatch.orphans + i, (rwatch.orphan_count - i) * sizeof(rwatch.orphans[0]));
	}
	rwatch.orphan_count -= i;

	for (watch = rwatch.head; watch && n < RWATCH_BATCH; watch = watch->next) {
		n += rwatch_collect(watch, now, rwatch.batch + n, RWATCH_BATCH - n, 1);
	}
	watches = rwatch.count;
	switch_mutex_unlock(rwatch.mutex);

	for (i = 0; i < n; i++) {
		if (switch_refresh_diag_limit_take(&rwatch.process_limit, switch_refresh_diag_now(), RWATCH_PROCESS_WINDOW_US, RWATCH_PROCESS_MAX)) {
			uint32_t suppressed_process = switch_refresh_diag_limit_drain_suppressed(&rwatch.process_limit);

			rwatch.suppressed_process_total += suppressed_process;
			rwatch_emit(&rwatch.batch[i], suppressed_process);
		}
	}

	switch_mutex_lock(rwatch.mutex);
	sum = rwatch.lost_total + rwatch.overflow_total + rwatch.suppressed_call_total + rwatch.orphan_dropped_total +
		rwatch.suppressed_process_total + rwatch.process_limit.suppressed;
	if (sum != rwatch.reported_sum && (!rwatch.health_last || now - rwatch.health_last >= RWATCH_HEALTH_US)) {
		rwatch.reported_sum = sum;
		rwatch.health_last = now;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
						  "SIP_REFRESH_WORK schema=1 kind=stall_health watches=%u lost=%" SWITCH_UINT64_T_FMT " overflow=%" SWITCH_UINT64_T_FMT
						  " suppressed_call=%" SWITCH_UINT64_T_FMT " suppressed_process=%" SWITCH_UINT64_T_FMT " orphan_dropped=%" SWITCH_UINT64_T_FMT "\n",
						  watches, rwatch.lost_total, rwatch.overflow_total, rwatch.suppressed_call_total,
						  rwatch.suppressed_process_total + rwatch.process_limit.suppressed, rwatch.orphan_dropped_total);
	}
	switch_mutex_unlock(rwatch.mutex);

	switch_mutex_unlock(rwatch.poll_mutex);
}

SWITCH_DECLARE(void) switch_refresh_watch_totals(switch_refresh_watch_totals_t *totals)
{
	memset(totals, 0, sizeof(*totals));

	if (!rwatch.mutex) {
		return;
	}

	switch_mutex_lock(rwatch.mutex);
	totals->watches = rwatch.count;
	totals->lost = rwatch.lost_total;
	totals->overflow = rwatch.overflow_total;
	totals->suppressed_call = rwatch.suppressed_call_total;
	totals->orphan_dropped = rwatch.orphan_dropped_total;
	switch_mutex_unlock(rwatch.mutex);

	/* the process limit belongs to the pass in progress, if any */
	switch_mutex_lock(rwatch.poll_mutex);
	totals->suppressed_process = rwatch.suppressed_process_total + rwatch.process_limit.suppressed;
	switch_mutex_unlock(rwatch.poll_mutex);
}

#else

SWITCH_DECLARE(void) switch_refresh_watch_totals(switch_refresh_watch_totals_t *totals)
{
	memset(totals, 0, sizeof(*totals));
}

SWITCH_DECLARE(void) switch_refresh_watch_init(switch_memory_pool_t *pool)
{
}

SWITCH_DECLARE(switch_status_t) switch_refresh_watch_observer_start(void)
{
	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(void) switch_refresh_watch_shutdown(void)
{
}

SWITCH_DECLARE(switch_refresh_watch_t *) switch_refresh_watch_create(const char *uuid, switch_channel_t *channel, switch_time_t stall_us)
{
	return NULL;
}

SWITCH_DECLARE(void) switch_refresh_watch_arm(switch_refresh_watch_t *watch)
{
}

SWITCH_DECLARE(int) switch_refresh_watch_begin(switch_refresh_watch_t *watch, switch_refresh_watch_stage_t stage, switch_core_session_t *target)
{
	return -1;
}

SWITCH_DECLARE(void) switch_refresh_watch_end(switch_refresh_watch_t *watch, int handle)
{
}

SWITCH_DECLARE(void) switch_refresh_watch_destroy(switch_refresh_watch_t **watchp)
{
}

SWITCH_DECLARE(void) switch_refresh_watch_poll(void)
{
}

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
