/*
 * switch_ivr_originate() picks a leg's ringback from the peer's
 * "outbound_ringback" variable, and that string belongs to the peer session.
 * ringback_data was never reset between failover legs ("|"), so when the next
 * leg had no outbound_ringback and the caller had no ringback of its own,
 * setup_ringback() for that leg was handed the previous leg's string, after the
 * previous leg's peer had already been released.
 *
 * Leg 1 is a loopback leg with outbound_ringback set; it is routed, so originate
 * takes its ringback, then hangs up without ringing. Leg 2 is a loopback leg with
 * no ringback configured anywhere that signals ring_ready a few seconds later,
 * well after leg 1's session is gone.
 *
 * setup_ringback() logs the file it is about to open. Leg 2 must not open one.
 */

#include <switch.h>
#include <test/switch_test.h>

#define LEG1_RINGBACK "silence_stream://-1"
#define USE_OUTBOUND_LOG "Use Outbound Ringback: " LEG1_RINGBACK
#define PLAY_LOG_PREFIX "Play Ringback File ["
#define MAX_PLAYS 8

static switch_mutex_t *log_mutex = NULL;
static char played[MAX_PLAYS][256];
static int plays = 0;
static int leg1_ringback_taken = 0;

/* Log bindings run on the log thread. */
static switch_status_t capture_logger(const switch_log_node_t *node, switch_log_level_t level)
{
	const char *p, *end;

	if (!node->data) {
		return SWITCH_STATUS_SUCCESS;
	}

	switch_mutex_lock(log_mutex);

	if (strstr(node->data, USE_OUTBOUND_LOG)) {
		leg1_ringback_taken++;
	}

	if ((p = strstr(node->data, PLAY_LOG_PREFIX)) && plays < MAX_PLAYS) {
		size_t len;

		p += strlen(PLAY_LOG_PREFIX);
		end = strrchr(p, ']');
		len = end ? (size_t)(end - p) : strlen(p);
		if (len >= sizeof(played[0])) {
			len = sizeof(played[0]) - 1;
		}
		memcpy(played[plays], p, len);
		played[plays][len] = '\0';
		plays++;
	}

	switch_mutex_unlock(log_mutex);

	return SWITCH_STATUS_SUCCESS;
}

FST_CORE_BEGIN("./conf")
{
	FST_SUITE_BEGIN(originate_failover_ringback)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_loopback");
			fst_requires_module("mod_dptools");
			fst_requires_module("mod_tone_stream");
			switch_mutex_init(&log_mutex, SWITCH_MUTEX_NESTED, fst_pool);
			plays = 0;
			leg1_ringback_taken = 0;
			switch_log_bind_logger(capture_logger, SWITCH_LOG_DEBUG, SWITCH_FALSE);
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
			switch_log_unbind_logger(capture_logger);
		}
		FST_TEARDOWN_END()

		FST_TEST_BEGIN(failover_leg_without_outbound_ringback)
		{
			switch_core_session_t *caller = NULL, *peer = NULL;
			switch_call_cause_t cause = SWITCH_CAUSE_NONE;
			int i;

			fst_requires(switch_ivr_originate(NULL, &caller, &cause, "null/+15553334444", 2,
											  NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) == SWITCH_STATUS_SUCCESS);
			fst_requires(caller);
			fst_requires(!switch_channel_get_variable(switch_core_session_get_channel(caller), "ringback"));

			/* Inline dialplans use ';' between apps ("m:;:") because ',' separates legs. */
			switch_ivr_originate(caller, &peer, &cause,
								 "[outbound_ringback=" LEG1_RINGBACK ",leg_timeout=3]loopback/m:;:sleep:300;hangup/default/inline"
								 "|[leg_timeout=6]loopback/m:;:sleep:3000;ring_ready;park/default/inline", 0,
								 NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
			if (peer) {
				switch_channel_hangup(switch_core_session_get_channel(peer), SWITCH_CAUSE_NORMAL_CLEARING);
				switch_core_session_rwunlock(peer);
			}

			/* Let the log thread deliver what is still queued. */
			switch_sleep(500000);

			switch_mutex_lock(log_mutex);
			for (i = 0; i < plays; i++) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "[TEST] setup_ringback #%d opened [%s]\n", i + 1, played[i]);
			}
			/* Leg 1 was routed, so originate took its outbound_ringback. */
			fst_check_int_equals(leg1_ringback_taken, 1);
			/* Leg 1 hangs up without ringing and leg 2 has no ringback, so no leg opens one. */
			fst_check_int_equals(plays, 0);
			switch_mutex_unlock(log_mutex);

			switch_channel_hangup(switch_core_session_get_channel(caller), SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(caller);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
