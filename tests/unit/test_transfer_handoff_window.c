/*
 * TELCORE-412 - a uuid_transfer that lands in the state machine's handoff window is lost.
 *
 * switch_ivr_session_transfer() installs a caller profile and calls
 * switch_channel_set_state(CS_ROUTING). That is edge-triggered on (state, running_state),
 * so a transfer landing between on_routing setting CS_EXECUTE and the loop re-entering
 * drags state back to CS_ROUTING, leaves state == running_state, and the loop - which
 * only runs a handler when they differ - sleeps without routing the profile. Every later
 * set_state(CS_ROUTING) then hits the equality guard, so the channel never recovers.
 *
 * transfer_pair_is_not_coalesced replays the real tel-apps command pair. It does NOT
 * reproduce at their 11ms spacing, nor across a 0-50ms sweep - the window is far narrower
 * than any delay schedulable from a test. Kept as that measurement.
 *
 * second_transfer_survives_the_handoff_window freezes the session thread inside the
 * window with CF_BLOCK_STATE, set from a CORE pre-exec on_routing handler - a per-channel
 * one would be removed by switch_channel_clear_state_handler() inside the transfer itself.
 * It also asserts the FIRST transfer's extension did not run.
 *
 * Both assert correct behaviour, so they fail against the unfixed handoff. Revert the
 * switch_ivr.c and switch_core_state_machine.c hunks and the frozen window test wedges
 * every trial.
 *
 * Not covered: a transfer landing after the dialplan hunt has read the profile. The
 * generation survives, but a blocking extension defers it until the app returns.
 *
 * Env: TRANSFER_HANDOFF_TRIALS, TRANSFER_HANDOFF_DELAYS (csv ms), TRANSFER_HANDOFF_SETTLE_MS.
 */

#include <switch.h>
#include <test/switch_test.h>

#define MARKER_VAR   "transfer_handoff_second_extension"
#define MARKER_VALUE "reached"

/* First half of the pair; its own marker proves this extension did not run instead. */
#define MARKER_A_VAR "transfer_handoff_first_extension"
#define XFER_A_DEST  "m:~:set:" MARKER_A_VAR "=ran~park"
/* Second half: the marker stands in for the production set~bridge~park payload. */
#define XFER_B_DEST "m:~:set:" MARKER_VAR "=" MARKER_VALUE "~park"

typedef enum {
	TRIAL_REACHED,
	TRIAL_WEDGED,
	TRIAL_REPARKED,
	TRIAL_UNKNOWN
} trial_result_t;

static const char *trial_result_name(trial_result_t r)
{
	switch (r) {
	case TRIAL_REACHED:  return "REACHED";
	case TRIAL_WEDGED:   return "WEDGED";
	case TRIAL_REPARKED: return "REPARKED";
	default:             return "UNKNOWN";
	}
}

/* CHANNEL_PARK counter, so we can tell "re-parked" from "never got there". */
static switch_mutex_t *park_mutex = NULL;
static char park_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1] = { 0 };
static int park_count = 0;

static void park_event_handler(switch_event_t *event)
{
	const char *uuid = switch_event_get_header(event, "Unique-ID");

	if (!uuid) {
		return;
	}

	switch_mutex_lock(park_mutex);
	if (*park_uuid && !strcmp(uuid, park_uuid)) {
		park_count++;
	}
	switch_mutex_unlock(park_mutex);
}

static void park_watch_reset(const char *uuid)
{
	switch_mutex_lock(park_mutex);
	switch_set_string(park_uuid, uuid ? uuid : "");
	park_count = 0;
	switch_mutex_unlock(park_mutex);
}

static int park_watch_count(void)
{
	int count;

	switch_mutex_lock(park_mutex);
	count = park_count;
	switch_mutex_unlock(park_mutex);

	return count;
}

/* Run uuid_transfer exactly the way tel-apps does, and report whether it said +OK. */
static switch_bool_t uuid_transfer(const char *uuid, const char *dest)
{
	switch_stream_handle_t stream = { 0 };
	char *args = switch_mprintf("%s %s inline", uuid, dest);
	switch_bool_t ok;

	SWITCH_STANDARD_STREAM(stream);
	switch_api_execute("uuid_transfer", args, NULL, &stream);

	ok = (stream.data && !strncmp((char *) stream.data, "+OK", 3));

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
					  "[transfer-handoff] uuid_transfer %.60s%s -> %s",
					  args, strlen(args) > 60 ? "..." : "",
					  stream.data ? (char *) stream.data : "(no response)\n");

	switch_safe_free(stream.data);
	switch_safe_free(args);

	return ok;
}

/*
 * One trial: park a fresh channel the way the inbound dialplan does, fire the
 * tel-apps transfer pair separated by delay_ms, then classify where the channel
 * ended up.
 */
static trial_result_t run_trial(int delay_ms, int settle_ms, switch_bool_t *both_ok)
{
	switch_core_session_t *session = NULL;
	switch_call_cause_t cause = SWITCH_CAUSE_NORMAL_CLEARING;
	switch_channel_t *channel = NULL;
	const char *uuid = NULL;
	const char *marker = NULL;
	switch_channel_state_t state, running_state;
	trial_result_t result = TRIAL_UNKNOWN;
	switch_time_t deadline;
	switch_bool_t ok_a = SWITCH_FALSE, ok_b = SWITCH_FALSE;

	if (switch_ivr_originate(NULL, &session, &cause, "null/+15553334444", 5,
							 NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS || !session) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[transfer-handoff] originate failed: %s\n",
						  switch_channel_cause2str(cause));
		return TRIAL_UNKNOWN;
	}

	channel = switch_core_session_get_channel(session);
	uuid = switch_core_session_get_uuid(session);

	/*
	 * Precondition: the production A-leg sits in the park application inside
	 * CS_EXECUTE, left there by the inbound dialplan (... ring_ready, park).
	 * One transfer to "park inline" puts this channel in the same place.
	 */
	park_watch_reset(uuid);
	uuid_transfer(uuid, XFER_A_DEST);

	if (switch_channel_wait_for_flag(channel, CF_PARK, SWITCH_TRUE, 5000, NULL) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[transfer-handoff] channel never reached park\n");
		goto done;
	}

	/* Now the trial proper. */
	switch_channel_set_variable(channel, MARKER_VAR, NULL);
	park_watch_reset(uuid);

	ok_a = uuid_transfer(uuid, XFER_A_DEST);

	if (delay_ms > 0) {
		switch_sleep(delay_ms * 1000);
	}

	ok_b = uuid_transfer(uuid, XFER_B_DEST);

	if (both_ok) {
		*both_ok = (ok_a && ok_b);
	}

	/* Poll rather than sleep a fixed settle, so a slow box is not reported as UNKNOWN. */
	deadline = switch_micro_time_now() + (settle_ms * 1000);
	while (switch_micro_time_now() < deadline) {
		marker = switch_channel_get_variable(channel, MARKER_VAR);
		if (marker && !strcmp(marker, MARKER_VALUE)) {
			break;
		}
		switch_sleep(20000);
	}

	marker = switch_channel_get_variable(channel, MARKER_VAR);
	state = switch_channel_get_state(channel);
	running_state = switch_channel_get_running_state(channel);

	if (marker && !strcmp(marker, MARKER_VALUE)) {
		result = TRIAL_REACHED;
	} else if (state == CS_ROUTING && running_state == CS_ROUTING) {
		result = TRIAL_WEDGED;
	} else if (park_watch_count() > 0) {
		result = TRIAL_REPARKED;
	} else {
		result = TRIAL_UNKNOWN;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					  "[transfer-handoff] delay=%dms result=%s state=%s running_state=%s CF_TRANSFER=%d park_events=%d marker=%s\n",
					  delay_ms, trial_result_name(result),
					  switch_channel_state_name(state), switch_channel_state_name(running_state),
					  switch_channel_test_flag(channel, CF_TRANSFER) ? 1 : 0,
					  park_watch_count(), marker ? marker : "(unset)");

  done:
	park_watch_reset(NULL);

	if (switch_channel_up(channel)) {
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
	}
	switch_core_session_rwunlock(session);

	/* Let the wedged channel finish tearing down before the next trial. */
	switch_sleep(200000);

	return result;
}

/*
 * Deterministic variant.
 *
 * The wall-clock sweep above only reproduces the defect if the second command lands
 * inside the handoff window. Racing it with a spin thread is not good enough either:
 * by the time switch_api_execute() has parsed the command and switch_ivr_session_transfer()
 * has done its preamble, the session thread has long since moved on. So instead of racing
 * the session thread, freeze it inside the window with CF_BLOCK_STATE and take the shot
 * at leisure.
 *
 * The window that matters is state=CS_EXECUTE / running_state=CS_ROUTING: on_routing has
 * hunted an extension and set CS_EXECUTE, but the state machine loop has not re-entered
 * yet. (The earlier state=CS_ROUTING / running=CS_ROUTING window is benign: a dropped
 * set_state there costs nothing, because inline_dialplan_hunt() re-reads the channel's
 * current caller profile, so a transfer that lands before the hunt is still honoured.)
 *
 * A global, pre-exec on_routing handler sets CF_BLOCK_STATE for the channel under test.
 * The state machine loop checks that flag at the top of each iteration, so the session
 * thread freezes on the iteration right after on_routing returns - exactly in the window.
 * It must be a CORE handler, not a channel handler: switch_ivr_session_transfer() calls
 * switch_channel_clear_state_handler(channel, NULL) and would remove a per-channel one.
 *
 * With the thread frozen there:
 *   set_state(CS_ROUTING) from CS_EXECUTE is a legal transition, so it is accepted and
 *   drags state back to CS_ROUTING. state == running_state == CS_ROUTING now, so when the
 *   thread is released the loop's "state != running_state" guard is false, the ROUTING
 *   handler never re-runs, and the thread goes into switch_thread_cond_wait().
 *   The transfer is lost and the channel is unrecoverable by further transfers.
 */
static switch_mutex_t *freeze_mutex = NULL;
static char freeze_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1] = { 0 };
static int freeze_armed = 0;
static int freeze_applied = 0;

static switch_status_t freeze_on_routing(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	const char *uuid = switch_core_session_get_uuid(session);

	switch_mutex_lock(freeze_mutex);
	if (freeze_armed && uuid && !strcmp(uuid, freeze_uuid)) {
		freeze_armed = 0;
		switch_channel_set_flag(channel, CF_BLOCK_STATE);
		freeze_applied = 1;
	}
	switch_mutex_unlock(freeze_mutex);

	return SWITCH_STATUS_SUCCESS;
}

static switch_state_handler_table_t freeze_handlers = {
	/*.on_init */ NULL,
	/*.on_routing */ freeze_on_routing,
	/*.on_execute */ NULL,
	/*.on_hangup */ NULL,
	/*.on_exchange_media */ NULL,
	/*.on_soft_execute */ NULL,
	/*.on_consume_media */ NULL,
	/*.on_hibernate */ NULL,
	/*.on_reset */ NULL,
	/*.on_park */ NULL,
	/*.on_reporting */ NULL,
	/*.on_destroy */ NULL,
	/*.flags */ SSH_FLAG_PRE_EXEC | SSH_FLAG_STICKY
};

static void freeze_arm(const char *uuid)
{
	switch_mutex_lock(freeze_mutex);
	switch_set_string(freeze_uuid, uuid ? uuid : "");
	freeze_armed = uuid ? 1 : 0;
	freeze_applied = 0;
	switch_mutex_unlock(freeze_mutex);
}

/* Disarm and clear under the handler's lock; doing them separately lets the handler
   re-set CF_BLOCK_STATE after the clear and strand the session thread forever. */
static void freeze_release(switch_channel_t *channel)
{
	switch_mutex_lock(freeze_mutex);
	freeze_armed = 0;
	freeze_applied = 0;
	switch_channel_clear_flag(channel, CF_BLOCK_STATE);
	switch_mutex_unlock(freeze_mutex);


}

static int freeze_is_applied(void)
{
	int applied;

	switch_mutex_lock(freeze_mutex);
	applied = freeze_applied;
	switch_mutex_unlock(freeze_mutex);

	return applied;
}

static trial_result_t run_freeze_trial(int settle_ms, int *froze, switch_bool_t *both_ok, int *recovered,
									   int *first_ran)
{
	switch_core_session_t *session = NULL;
	switch_call_cause_t cause = SWITCH_CAUSE_NORMAL_CLEARING;
	switch_channel_t *channel = NULL;
	const char *uuid = NULL;
	const char *marker = NULL;
	switch_channel_state_t pre_state = CS_NONE, pre_running = CS_NONE;
	switch_channel_state_t post_state = CS_NONE, post_running = CS_NONE;
	switch_channel_state_t state, running_state;
	trial_result_t result = TRIAL_UNKNOWN;
	switch_time_t deadline;
	switch_bool_t ok_a = SWITCH_FALSE, ok_b = SWITCH_FALSE;

	if (switch_ivr_originate(NULL, &session, &cause, "null/+15553334444", 5,
							 NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS || !session) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[transfer-handoff] freeze trial: originate failed: %s\n",
						  switch_channel_cause2str(cause));
		return TRIAL_UNKNOWN;
	}

	channel = switch_core_session_get_channel(session);
	uuid = switch_core_session_get_uuid(session);

	park_watch_reset(uuid);
	if (!uuid_transfer(uuid, XFER_A_DEST)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "[transfer-handoff] freeze trial: precondition transfer refused, channel state %s\n",
						  switch_channel_state_name(switch_channel_get_state(channel)));
	}

	if (switch_channel_wait_for_flag(channel, CF_PARK, SWITCH_TRUE, 5000, NULL) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "[transfer-handoff] freeze trial: channel never parked, state %s running %s\n",
						  switch_channel_state_name(switch_channel_get_state(channel)),
						  switch_channel_state_name(switch_channel_get_running_state(channel)));
		goto done;
	}

	switch_channel_set_variable(channel, MARKER_VAR, NULL);
	switch_channel_set_variable(channel, MARKER_A_VAR, NULL);
	park_watch_reset(uuid);

	/* Arm the freeze, then send the first half of the pair. */
	freeze_arm(uuid);
	ok_a = uuid_transfer(uuid, XFER_A_DEST);

	/* Wait for the session thread to actually be parked in the window. */
	deadline = switch_micro_time_now() + 3000000;
	while (switch_micro_time_now() < deadline) {
		if (freeze_is_applied() &&
			switch_channel_get_state(channel) == CS_EXECUTE &&
			switch_channel_get_running_state(channel) == CS_ROUTING) {
			break;
		}
		switch_cond_next();
	}

	pre_state = switch_channel_get_state(channel);
	pre_running = switch_channel_get_running_state(channel);

	if (!(freeze_is_applied() && pre_state == CS_EXECUTE && pre_running == CS_ROUTING)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "[transfer-handoff] freeze not achieved (applied=%d state=%s running=%s)\n",
						  freeze_is_applied(), switch_channel_state_name(pre_state),
						  switch_channel_state_name(pre_running));
		freeze_release(channel);
		goto done;
	}

	if (froze) {
		*froze = 1;
	}

	/* Second half of the pair, delivered into the frozen window. */
	ok_b = uuid_transfer(uuid, XFER_B_DEST);

	post_state = switch_channel_get_state(channel);
	post_running = switch_channel_get_running_state(channel);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					  "[transfer-handoff] frozen window: before %s/%s -> after %s/%s\n",
					  switch_channel_state_name(pre_state), switch_channel_state_name(pre_running),
					  switch_channel_state_name(post_state), switch_channel_state_name(post_running));

	/* Release the session thread and let it act on whatever it now sees. */
	freeze_release(channel);

	if (both_ok) {
		*both_ok = (ok_a && ok_b);
	}

	switch_sleep(settle_ms * 1000);

	marker = switch_channel_get_variable(channel, MARKER_VAR);
	state = switch_channel_get_state(channel);
	running_state = switch_channel_get_running_state(channel);

	if (marker && !strcmp(marker, MARKER_VALUE)) {
		result = TRIAL_REACHED;
	} else if (state == CS_ROUTING && running_state == CS_ROUTING) {
		result = TRIAL_WEDGED;
	} else if (park_watch_count() > 0) {
		result = TRIAL_REPARKED;
	}

	if (first_ran && switch_channel_get_variable(channel, MARKER_A_VAR)) {
		*first_ran = 1;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					  "[transfer-handoff] freeze trial -> %s state=%s running_state=%s CF_TRANSFER=%d park_events=%d marker=%s\n",
					  trial_result_name(result),
					  switch_channel_state_name(state), switch_channel_state_name(running_state),
					  switch_channel_test_flag(channel, CF_TRANSFER) ? 1 : 0,
					  park_watch_count(), marker ? marker : "(unset)");

	/*
	 * Once wedged, is the channel recoverable? In production the customer's next two
	 * transfers also returned +OK and also did nothing. Send one more and see.
	 */
	if (result == TRIAL_WEDGED) {
		switch_bool_t ok_c = uuid_transfer(uuid, XFER_B_DEST);

		switch_sleep(500000);
		marker = switch_channel_get_variable(channel, MARKER_VAR);

		if (recovered) {
			*recovered = (marker && !strcmp(marker, MARKER_VALUE)) ? 1 : 0;
		}

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
						  "[transfer-handoff] retry on wedged channel: +OK=%d state=%s running_state=%s marker=%s\n",
						  ok_c ? 1 : 0,
						  switch_channel_state_name(switch_channel_get_state(channel)),
						  switch_channel_state_name(switch_channel_get_running_state(channel)),
						  marker ? marker : "(unset)");
	}

  done:
	freeze_release(channel);
	park_watch_reset(NULL);

	if (switch_channel_up(channel)) {
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
	}
	switch_core_session_rwunlock(session);
	switch_sleep(200000);

	return result;
}

static int env_int(const char *name, int dflt)
{
	const char *val = getenv(name);
	int n;

	if (zstr(val)) {
		return dflt;
	}

	n = atoi(val);

	return n > 0 ? n : dflt;
}

FST_CORE_BEGIN("./conf_transfer_handoff")
{
	FST_SUITE_BEGIN(transfer_handoff_window)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_loopback");
			fst_requires_module("mod_commands");
			fst_requires_module("mod_dptools");

			/* Per test, never cached: fst_pool is created and destroyed per test. */
			switch_mutex_init(&park_mutex, SWITCH_MUTEX_NESTED, fst_pool);
			switch_mutex_init(&freeze_mutex, SWITCH_MUTEX_NESTED, fst_pool);
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		FST_TEST_BEGIN(transfer_pair_is_not_coalesced)
		{
			const char *delays_env = getenv("TRANSFER_HANDOFF_DELAYS");
			char delays_buf[256];
			char *argv[32] = { 0 };
			int argc, i, t;
			int trials = env_int("TRANSFER_HANDOFF_TRIALS", 2);
			int settle_ms = env_int("TRANSFER_HANDOFF_SETTLE_MS", 1200);
			int total = 0, reached = 0, wedged = 0, reparked = 0, unknown = 0;
			int ok_but_dropped = 0;

			fst_requires(switch_event_bind("transfer_handoff", SWITCH_EVENT_CHANNEL_PARK, SWITCH_EVENT_SUBCLASS_ANY,
										   park_event_handler, NULL) == SWITCH_STATUS_SUCCESS);

			switch_set_string(delays_buf, zstr(delays_env) ? "11" : delays_env);
			argc = switch_separate_string(delays_buf, ',', argv, (sizeof(argv) / sizeof(argv[0])));

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "========== TELCORE-412 transfer pair race: %d delays x %d trials ==========\n",
							  argc, trials);

			for (i = 0; i < argc; i++) {
				int delay_ms = atoi(argv[i]);
				int d_reached = 0, d_wedged = 0, d_reparked = 0, d_unknown = 0;

				for (t = 0; t < trials; t++) {
					switch_bool_t both_ok = SWITCH_FALSE;
					trial_result_t r = run_trial(delay_ms, settle_ms, &both_ok);

					total++;

					switch (r) {
					case TRIAL_REACHED:  d_reached++;  reached++;  break;
					case TRIAL_WEDGED:   d_wedged++;   wedged++;   break;
					case TRIAL_REPARKED: d_reparked++; reparked++; break;
					default:             d_unknown++;  unknown++;  break;
					}

					/* The customer-visible contradiction: +OK for work that never happened. */
					if (r != TRIAL_REACHED && both_ok) {
						ok_but_dropped++;
					}
				}

				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "[transfer-handoff] delay %3dms | reached %d | wedged %d | reparked %d | unknown %d\n",
								  delay_ms, d_reached, d_wedged, d_reparked, d_unknown);
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "========== TELCORE-412 totals: %d trials | reached %d | wedged %d | reparked %d | unknown %d "
							  "| returned +OK but dropped the transfer: %d ==========\n",
							  total, reached, wedged, reparked, unknown, ok_but_dropped);

			switch_event_unbind_callback(park_event_handler);

			/*
			 * Correct behaviour: the second transfer's extension always runs. Note this
			 * held before the fix too - the window is far narrower than any delay we can
			 * schedule from here - so this is a measurement, not the regression guard.
			 * second_transfer_survives_the_handoff_window is the guard.
			 */
			/* Separate: unknown means the harness stalled, not that a transfer dropped. */
			fst_check(total > 0);
			fst_check(unknown == 0);
			fst_check(reached == total - unknown);
			fst_check(ok_but_dropped == 0);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(second_transfer_survives_the_handoff_window)
		{
			int settle_ms = env_int("TRANSFER_HANDOFF_SETTLE_MS", 1200);
			int trials = env_int("TRANSFER_HANDOFF_TRIALS", 2);
			int t, froze_total = 0, wedged = 0, reparked = 0, reached = 0, unknown = 0;
			int ok_but_dropped = 0, recovered_total = 0, first_ran_total = 0;


			fst_requires(switch_event_bind("transfer_handoff", SWITCH_EVENT_CHANNEL_PARK, SWITCH_EVENT_SUBCLASS_ANY,
										   park_event_handler, NULL) == SWITCH_STATUS_SUCCESS);
			switch_core_add_state_handler(&freeze_handlers);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "========== TELCORE-412 frozen handoff window x %d ==========\n", trials);

			for (t = 0; t < trials; t++) {
				int froze = 0, recovered = 0, first_ran = 0;
				switch_bool_t both_ok = SWITCH_FALSE;
				trial_result_t r = run_freeze_trial(settle_ms, &froze, &both_ok, &recovered, &first_ran);

				froze_total += froze;
				recovered_total += recovered;
				first_ran_total += first_ran;

				switch (r) {
				case TRIAL_REACHED:  reached++;  break;
				case TRIAL_WEDGED:   wedged++;   break;
				case TRIAL_REPARKED: reparked++; break;
				default:             unknown++;  break;
				}

				if (r != TRIAL_REACHED && both_ok) {
					ok_but_dropped++;
				}
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "========== TELCORE-412 frozen window: froze %d/%d | reached %d | wedged %d "
							  "| reparked %d | unknown %d | +OK but dropped: %d | wedges recovered by a retry: %d/%d ==========\n",
							  froze_total, trials, reached, wedged, reparked, unknown, ok_but_dropped,
							  recovered_total, wedged);

			switch_core_remove_state_handler(&freeze_handlers);
			switch_event_unbind_callback(park_event_handler);

			/* The probe itself must work, or the result below means nothing. */
			fst_requires(froze_total == trials);

			/* The first transfer's extension must never have run: routing the second
			   transfer is the requirement, not running the first one instead. */
			fst_check(first_ran_total == 0);

			/* Correct behaviour: the second transfer is honoured even from inside the window. */
			fst_check(reached == trials);
			fst_check(wedged == 0);
			fst_check(ok_but_dropped == 0);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
