/*
 * switch_ivr_originate() starts an early-media thread (oglobals.ethread) when a
 * leg's peer brings early media and bridge_early_media is on, and joins it in
 * that leg's cleanup. oglobals.ethread was never cleared after the join, so on
 * failover ("|") a later leg that does not start its own early-media thread
 * joined the first leg's thread a second time. What a second pthread_join()
 * on a finished thread does depends on what became of its descriptor: an
 * error, a wait on whichever thread reuses it, or a fault.
 *
 * Leg 1 is a loopback leg that pre-answers, so originate bridges its early
 * media and starts the thread, then hangs up. Leg 2 is a loopback leg that
 * hangs up straight away without early media.
 *
 * To make the second join observable without depending on what glibc does
 * with the old descriptor, this binary wraps pthread_create()/pthread_join()
 * (libfreeswitch calls them through the PLT, so the definitions here win). A
 * join of a handle that was already joined, and not handed out again by
 * pthread_create() since, is counted and refused instead of passed on.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <switch.h>
#include <test/switch_test.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>

#define MAX_JOINED 1024
#define EARLY_MEDIA_LOG "bridge_early_media: "

static pthread_mutex_t track_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t joined[MAX_JOINED];
static int njoined = 0;
static int repeat_joins = 0;

static int (*real_pthread_create)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *) = NULL;
static int (*real_pthread_join)(pthread_t, void **) = NULL;

static void forget_joined(pthread_t t)
{
	int i;

	for (i = 0; i < njoined; i++) {
		if (pthread_equal(joined[i], t)) {
			joined[i] = joined[--njoined];
			return;
		}
	}
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg)
{
	int rc;

	if (!real_pthread_create) {
		*(void **) (&real_pthread_create) = dlsym(RTLD_NEXT, "pthread_create");
	}

	rc = real_pthread_create(thread, attr, start_routine, arg);

	if (rc == 0) {
		pthread_mutex_lock(&track_lock);
		forget_joined(*thread);
		pthread_mutex_unlock(&track_lock);
	}

	return rc;
}

int pthread_join(pthread_t thread, void **retval)
{
	int i, rc;

	if (!real_pthread_join) {
		*(void **) (&real_pthread_join) = dlsym(RTLD_NEXT, "pthread_join");
	}

	pthread_mutex_lock(&track_lock);
	for (i = 0; i < njoined; i++) {
		if (pthread_equal(joined[i], thread)) {
			repeat_joins++;
			pthread_mutex_unlock(&track_lock);
			return ESRCH;
		}
	}
	pthread_mutex_unlock(&track_lock);

	rc = real_pthread_join(thread, retval);

	if (rc == 0) {
		pthread_mutex_lock(&track_lock);
		if (njoined < MAX_JOINED) {
			joined[njoined++] = thread;
		}
		pthread_mutex_unlock(&track_lock);
	}

	return rc;
}

static switch_mutex_t *log_mutex = NULL;
static int early_media_bridged = 0;

/* Log bindings run on the log thread. */
static switch_status_t capture_logger(const switch_log_node_t *node, switch_log_level_t level)
{
	if (node->data && strstr(node->data, EARLY_MEDIA_LOG)) {
		switch_mutex_lock(log_mutex);
		early_media_bridged++;
		switch_mutex_unlock(log_mutex);
	}

	return SWITCH_STATUS_SUCCESS;
}

FST_CORE_BEGIN("./conf")
{
	FST_SUITE_BEGIN(originate_early_thread_join)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_loopback");
			fst_requires_module("mod_dptools");
			switch_mutex_init(&log_mutex, SWITCH_MUTEX_NESTED, fst_pool);
			early_media_bridged = 0;
			switch_log_bind_logger(capture_logger, SWITCH_LOG_DEBUG, SWITCH_FALSE);
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
			switch_log_unbind_logger(capture_logger);
		}
		FST_TEARDOWN_END()

		FST_TEST_BEGIN(failover_after_bridged_early_media)
		{
			switch_core_session_t *caller = NULL, *peer = NULL;
			switch_call_cause_t cause = SWITCH_CAUSE_NONE;
			int repeats;

			fst_requires(switch_ivr_originate(NULL, &caller, &cause, "null/+15553334444", 2,
											  NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) == SWITCH_STATUS_SUCCESS);
			fst_requires(caller);

			pthread_mutex_lock(&track_lock);
			repeat_joins = 0;
			pthread_mutex_unlock(&track_lock);

			/* Inline dialplans use ';' between apps ("m:;:") because ',' separates legs. */
			switch_ivr_originate(caller, &peer, &cause,
								 "{bridge_early_media=true}"
								 "[leg_timeout=3]loopback/m:;:pre_answer;sleep:1000;hangup/default/inline"
								 "|[leg_timeout=3]loopback/m:;:hangup/default/inline", 0,
								 NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
			if (peer) {
				switch_channel_hangup(switch_core_session_get_channel(peer), SWITCH_CAUSE_NORMAL_CLEARING);
				switch_core_session_rwunlock(peer);
			}

			pthread_mutex_lock(&track_lock);
			repeats = repeat_joins;
			pthread_mutex_unlock(&track_lock);

			/* Let the log thread deliver what is still queued. */
			switch_sleep(500000);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "[TEST] early media bridged %d time(s), repeated joins %d\n",
							  early_media_bridged, repeats);

			/* Leg 1's early media was bridged, so the early-media thread ran. */
			switch_mutex_lock(log_mutex);
			fst_check(early_media_bridged >= 1);
			switch_mutex_unlock(log_mutex);
			/* Every thread is joined at most once. */
			fst_check_int_equals(repeats, 0);

			switch_channel_hangup(switch_core_session_get_channel(caller), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(caller);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
