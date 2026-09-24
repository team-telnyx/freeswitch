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
 * transfer_after_the_hunt_discards_the_stale_extension covers the other half of the
 * window - a transfer landing after the dialplan hunt has already read the caller
 * profile - by holding the session thread in the post-dialplan hook, which sits between
 * the hunt and switch_channel_set_caller_extension().
 *
 * xferext_transfer_wakes_a_thread_asleep_in_routing covers the sibling handoff,
 * switch_channel_transfer_to_extension(), called directly while the session thread is
 * asleep in CS_ROUTING - the case where set_state() is a same-state no-op and so wakes
 * nobody.
 *
 * queued_extension_consumed_by_the_pass_is_not_discarded guards the converse: the
 * re-route must NOT fire when the pass already consumed the transfer. The xferext
 * payload is a queued extension that switch_channel_get_queued_extension() pops
 * destructively, so discarding that pass loses it outright.
 *
 * Not covered: the sleep branch's own backstop at the bottom of the loop. Reaching it
 * needs a bump to land after the branch has read the generation, which happens either
 * when wake_session_thread() gives up after ten failed trylocks or, more commonly, when
 * it sets CF_STATE_REPEAT instead and that shadows the generation check for one
 * iteration. Neither is reachable from here without a test-only hook.
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
/* Outlives fst_pool; see the setup. */
static switch_memory_pool_t *test_pool = NULL;

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

/*
 * The other half of the window: a transfer that lands after the dialplan hunt has read
 * the caller profile.
 *
 * switch_core_standard_on_routing() reads the caller profile, hunts an extension from it,
 * then installs that extension and sets CS_EXECUTE. A transfer arriving between the read
 * and the install replaces the profile, but the handler still commits the extension it
 * hunted from the old one. From that point state != running_state, so neither the
 * top-of-loop guard nor the sleep branch consults the generation again: the stale
 * extension runs, and because the production payload is a blocking park, the new transfer
 * is deferred for the life of the call.
 *
 * Reaching that point deterministically needs a hook inside the handler, between the hunt
 * and switch_channel_set_caller_extension(). The post-dialplan function is exactly there
 * - it runs only when sofia_profile_name is set, which the test sets itself. The callback
 * parks the session thread until the test has delivered the second transfer, so the race
 * is not raced at all.
 */
static switch_mutex_t *hunt_mutex = NULL;
static char hunt_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1] = { 0 };
static int hunt_armed = 0;
static int hunt_reached = 0;
static int hunt_release = 0;
/* Which extension the hook was handed - the test asserts it is the FIRST transfer's, or
   the hook fired at the wrong point and the trial proves nothing. */
static int hunt_saw_first = 0;

static void hunt_post_dialplan(switch_core_session_t *session, switch_caller_extension_t *extension, const char *profile_name)
{
	const char *uuid = switch_core_session_get_uuid(session);
	switch_time_t deadline;
	int mine = 0;

	switch_mutex_lock(hunt_mutex);
	if (hunt_armed && uuid && !strcmp(uuid, hunt_uuid)) {
		switch_caller_application_t *app = extension ? extension->current_application : NULL;

		hunt_armed = 0;
		hunt_reached = 1;
		mine = 1;
		/* XFER_A_DEST's first application is "set <MARKER_A_VAR>=ran". */
		hunt_saw_first = (app && app->application_data && strstr(app->application_data, MARKER_A_VAR)) ? 1 : 0;
	}
	switch_mutex_unlock(hunt_mutex);

	if (!mine) {
		return;
	}

	/* Bounded: a test that never releases must not strand the session thread forever. */
	deadline = switch_micro_time_now() + 5000000;
	while (switch_micro_time_now() < deadline) {
		int go;

		switch_mutex_lock(hunt_mutex);
		go = hunt_release;
		switch_mutex_unlock(hunt_mutex);

		if (go) {
			break;
		}
		switch_cond_next();
	}
}

static void hunt_arm(const char *uuid)
{
	switch_mutex_lock(hunt_mutex);
	switch_set_string(hunt_uuid, uuid ? uuid : "");
	hunt_armed = uuid ? 1 : 0;
	hunt_reached = 0;
	hunt_release = 0;
	hunt_saw_first = 0;
	switch_mutex_unlock(hunt_mutex);
}

static void hunt_let_go(void)
{
	switch_mutex_lock(hunt_mutex);
	hunt_armed = 0;
	hunt_release = 1;
	switch_mutex_unlock(hunt_mutex);
}

static int hunt_saw_first_extension(void)
{
	int saw;

	switch_mutex_lock(hunt_mutex);
	saw = hunt_saw_first;
	switch_mutex_unlock(hunt_mutex);

	return saw;
}

static int hunt_is_reached(void)
{
	int reached;

	switch_mutex_lock(hunt_mutex);
	reached = hunt_reached;
	switch_mutex_unlock(hunt_mutex);

	return reached;
}

/* One trial. *hunted is set only if the session thread was confirmed inside the hook,
   having been handed the FIRST transfer's extension. */
static trial_result_t run_post_hunt_trial(int settle_ms, int *hunted, int *first_ran, switch_bool_t *both_ok)
{
	switch_bool_t ok_a = SWITCH_FALSE, ok_b = SWITCH_FALSE;
	switch_core_session_t *session = NULL;
	switch_call_cause_t cause = SWITCH_CAUSE_NORMAL_CLEARING;
	switch_channel_t *channel = NULL;
	const char *uuid = NULL;
	const char *marker = NULL;
	trial_result_t result = TRIAL_UNKNOWN;
	switch_time_t deadline;

	if (switch_ivr_originate(NULL, &session, &cause, "null/+15553334444", 5,
							 NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS || !session) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[transfer-handoff] post-hunt trial: originate failed: %s\n",
						  switch_channel_cause2str(cause));
		return TRIAL_UNKNOWN;
	}

	channel = switch_core_session_get_channel(session);
	uuid = switch_core_session_get_uuid(session);

	park_watch_reset(uuid);
	uuid_transfer(uuid, XFER_A_DEST);

	if (switch_channel_wait_for_flag(channel, CF_PARK, SWITCH_TRUE, 5000, NULL) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[transfer-handoff] post-hunt trial: channel never parked\n");
		goto done;
	}

	switch_channel_set_variable(channel, MARKER_VAR, NULL);
	/* The precondition transfer used XFER_A_DEST too, so clear its marker. */
	switch_channel_set_variable(channel, MARKER_A_VAR, NULL);
	park_watch_reset(uuid);

	/* The hook only runs when this is set; the value is not otherwise used here. */
	switch_channel_set_variable(channel, "sofia_profile_name", "transfer_handoff");
	switch_channel_set_post_dialplan_function(channel, hunt_post_dialplan);

	hunt_arm(uuid);
	ok_a = uuid_transfer(uuid, XFER_A_DEST);

	deadline = switch_micro_time_now() + 3000000;
	while (switch_micro_time_now() < deadline) {
		if (hunt_is_reached()) {
			break;
		}
		switch_cond_next();
	}

	if (!hunt_is_reached()) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "[transfer-handoff] post-hunt trial: hook never reached (state=%s running=%s)\n",
						  switch_channel_state_name(switch_channel_get_state(channel)),
						  switch_channel_state_name(switch_channel_get_running_state(channel)));
		hunt_let_go();
		goto done;
	}

	if (!hunt_saw_first_extension()) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "[transfer-handoff] post-hunt trial: hook fired on the wrong extension\n");
		hunt_let_go();
		goto done;
	}

	if (hunted) {
		*hunted = 1;
	}

	/* The A extension is hunted and built; the profile it came from is about to be
	   replaced underneath it. */
	ok_b = uuid_transfer(uuid, XFER_B_DEST);

	hunt_let_go();

	if (both_ok) {
		*both_ok = (ok_a && ok_b);
	}

	deadline = switch_micro_time_now() + (settle_ms * 1000);
	while (switch_micro_time_now() < deadline) {
		marker = switch_channel_get_variable(channel, MARKER_VAR);
		if (marker && !strcmp(marker, MARKER_VALUE)) {
			break;
		}
		switch_sleep(20000);
	}

	marker = switch_channel_get_variable(channel, MARKER_VAR);

	if (marker && !strcmp(marker, MARKER_VALUE)) {
		result = TRIAL_REACHED;
	} else if (switch_channel_get_state(channel) == CS_ROUTING &&
			   switch_channel_get_running_state(channel) == CS_ROUTING) {
		result = TRIAL_WEDGED;
	} else if (park_watch_count() > 0) {
		result = TRIAL_REPARKED;
	}

	if (first_ran && switch_channel_get_variable(channel, MARKER_A_VAR)) {
		*first_ran = 1;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					  "[transfer-handoff] post-hunt trial -> %s state=%s running_state=%s park_events=%d marker=%s stale_extension_ran=%s\n",
					  trial_result_name(result),
					  switch_channel_state_name(switch_channel_get_state(channel)),
					  switch_channel_state_name(switch_channel_get_running_state(channel)),
					  park_watch_count(), marker ? marker : "(unset)",
					  switch_channel_get_variable(channel, MARKER_A_VAR) ? "yes" : "no");

  done:
	hunt_arm(NULL);
	hunt_let_go();
	park_watch_reset(NULL);

	if (switch_channel_up(channel)) {
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
	}
	switch_core_session_rwunlock(session);
	switch_sleep(200000);

	return result;
}

/*
 * switch_channel_transfer_to_extension() - the sibling handoff, reachable from ESL
 * xferext - does the same bump and the same switch_channel_set_state(CS_ROUTING), but
 * unlike switch_ivr_session_transfer() it is not followed by anything that wakes the
 * session thread. set_state() is a silent no-op when the channel is already CS_ROUTING,
 * so a cross-thread call landing on a channel asleep in routing strands the bump.
 *
 * To hold a session thread asleep in CS_ROUTING deterministically, veto the routing
 * handler: a core pre-exec on_routing returning anything but SWITCH_STATUS_SUCCESS makes
 * STATE_MACRO skip switch_core_standard_on_routing(), so nothing moves the state on. The
 * loop then finds state == running_state == CS_ROUTING and goes into
 * switch_thread_cond_wait(). CF_THREAD_SLEEPING is set across that wait, so the test can
 * confirm the thread really is parked there before taking the shot - without that the
 * result would prove nothing.
 *
 * The handler disarms itself, so the routing pass that the wake provokes runs the
 * standard handler, which picks the queued extension up.
 */
static switch_mutex_t *stall_mutex = NULL;
static char stall_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1] = { 0 };
static int stall_armed = 0;
static int stall_applied = 0;

static switch_status_t stall_on_routing(switch_core_session_t *session)
{
	const char *uuid = switch_core_session_get_uuid(session);
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_mutex_lock(stall_mutex);
	if (stall_armed && uuid && !strcmp(uuid, stall_uuid)) {
		stall_armed = 0;
		stall_applied = 1;
		status = SWITCH_STATUS_FALSE;
	}
	switch_mutex_unlock(stall_mutex);

	return status;
}

static switch_state_handler_table_t stall_handlers = {
	/*.on_init */ NULL,
	/*.on_routing */ stall_on_routing,
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

static void stall_arm(const char *uuid)
{
	switch_mutex_lock(stall_mutex);
	switch_set_string(stall_uuid, uuid ? uuid : "");
	stall_armed = uuid ? 1 : 0;
	stall_applied = 0;
	switch_mutex_unlock(stall_mutex);
}

static int stall_is_applied(void)
{
	int applied;

	switch_mutex_lock(stall_mutex);
	applied = stall_applied;
	switch_mutex_unlock(stall_mutex);

	return applied;
}

/* One trial. *stalled is set only if the thread was confirmed asleep in CS_ROUTING. */
static trial_result_t run_xferext_trial(int settle_ms, int *stalled)
{
	switch_core_session_t *session = NULL;
	switch_call_cause_t cause = SWITCH_CAUSE_NORMAL_CLEARING;
	switch_channel_t *channel = NULL;
	switch_caller_extension_t *extension = NULL;
	const char *uuid = NULL;
	const char *marker = NULL;
	trial_result_t result = TRIAL_UNKNOWN;
	switch_time_t deadline;

	if (switch_ivr_originate(NULL, &session, &cause, "null/+15553334444", 5,
							 NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS || !session) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[transfer-handoff] xferext trial: originate failed: %s\n",
						  switch_channel_cause2str(cause));
		return TRIAL_UNKNOWN;
	}

	channel = switch_core_session_get_channel(session);
	uuid = switch_core_session_get_uuid(session);

	park_watch_reset(uuid);
	uuid_transfer(uuid, XFER_A_DEST);

	if (switch_channel_wait_for_flag(channel, CF_PARK, SWITCH_TRUE, 5000, NULL) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[transfer-handoff] xferext trial: channel never parked\n");
		goto done;
	}

	switch_channel_set_variable(channel, MARKER_VAR, NULL);
	park_watch_reset(uuid);

	/* Park the session thread asleep in CS_ROUTING. */
	stall_arm(uuid);
	uuid_transfer(uuid, XFER_A_DEST);

	deadline = switch_micro_time_now() + 3000000;
	while (switch_micro_time_now() < deadline) {
		if (stall_is_applied() &&
			switch_channel_get_state(channel) == CS_ROUTING &&
			switch_channel_get_running_state(channel) == CS_ROUTING &&
			switch_channel_test_flag(channel, CF_THREAD_SLEEPING)) {
			break;
		}
		switch_cond_next();
	}

	if (!(stall_is_applied() &&
		  switch_channel_get_state(channel) == CS_ROUTING &&
		  switch_channel_get_running_state(channel) == CS_ROUTING &&
		  switch_channel_test_flag(channel, CF_THREAD_SLEEPING))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "[transfer-handoff] xferext trial: stall not achieved (applied=%d state=%s running=%s sleeping=%d)\n",
						  stall_is_applied(), switch_channel_state_name(switch_channel_get_state(channel)),
						  switch_channel_state_name(switch_channel_get_running_state(channel)),
						  switch_channel_test_flag(channel, CF_THREAD_SLEEPING) ? 1 : 0);
		goto done;
	}

	/* The shot: the exported sibling path, called directly, same state, asleep. */
	if (!(extension = switch_caller_extension_new(session, "xferext", "xferext"))) {
		goto done;
	}
	switch_caller_extension_add_application(session, extension, "set", MARKER_VAR "=" MARKER_VALUE);
	switch_caller_extension_add_application(session, extension, "park", NULL);

	/* Set last, so the probe means "the shot was fired", not "we got close". */
	if (stalled) {
		*stalled = 1;
	}

	switch_channel_transfer_to_extension(channel, extension);

	deadline = switch_micro_time_now() + (settle_ms * 1000);
	while (switch_micro_time_now() < deadline) {
		marker = switch_channel_get_variable(channel, MARKER_VAR);
		if (marker && !strcmp(marker, MARKER_VALUE)) {
			break;
		}
		switch_sleep(20000);
	}

	marker = switch_channel_get_variable(channel, MARKER_VAR);

	if (marker && !strcmp(marker, MARKER_VALUE)) {
		result = TRIAL_REACHED;
	} else if (switch_channel_get_state(channel) == CS_ROUTING &&
			   switch_channel_get_running_state(channel) == CS_ROUTING) {
		result = TRIAL_WEDGED;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					  "[transfer-handoff] xferext trial -> %s state=%s running_state=%s sleeping=%d marker=%s\n",
					  trial_result_name(result),
					  switch_channel_state_name(switch_channel_get_state(channel)),
					  switch_channel_state_name(switch_channel_get_running_state(channel)),
					  switch_channel_test_flag(channel, CF_THREAD_SLEEPING) ? 1 : 0,
					  marker ? marker : "(unset)");

  done:
	stall_arm(NULL);
	park_watch_reset(NULL);

	if (switch_channel_up(channel)) {
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
	}
	switch_core_session_rwunlock(session);
	switch_sleep(200000);

	return result;
}

/*
 * The re-route must not discard a pass that already consumed the transfer.
 *
 * switch_channel_transfer_to_extension() does not install a caller profile - its payload
 * is channel->queued_extension, and switch_channel_get_queued_extension() pops it
 * destructively. So a transfer landing after the state machine's snapshot but before the
 * pop is consumed correctly by that very pass. Re-routing it anyway throws the decision
 * away, and pass two finds nothing queued and a caller profile the xferext never touched:
 * it re-hunts the pre-transfer destination, or hangs up with NO_ROUTE_DESTINATION. The
 * transfer is lost - a worse outcome than the bug this file exists for.
 *
 * The window is ROUTING-entry-snapshot -> pop. A core pre-exec on_routing handler runs
 * inside it (STATE_MACRO calls the pre-exec chain before switch_core_standard_on_routing),
 * so the gate below holds the session thread exactly there, returns SUCCESS so the
 * standard handler still runs, and the test fires the xferext while it waits.
 */
static switch_mutex_t *gate_mutex = NULL;
static char gate_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1] = { 0 };
static int gate_armed = 0;
static int gate_reached = 0;
static int gate_release = 0;

static switch_status_t gate_on_routing(switch_core_session_t *session)
{
	const char *uuid = switch_core_session_get_uuid(session);
	switch_time_t deadline;
	int mine = 0;

	switch_mutex_lock(gate_mutex);
	if (gate_armed && uuid && !strcmp(uuid, gate_uuid)) {
		gate_armed = 0;
		gate_reached = 1;
		mine = 1;
	}
	switch_mutex_unlock(gate_mutex);

	if (!mine) {
		return SWITCH_STATUS_SUCCESS;
	}

	/* Bounded: a test that never releases must not strand the session thread forever. */
	deadline = switch_micro_time_now() + 5000000;
	while (switch_micro_time_now() < deadline) {
		int go;

		switch_mutex_lock(gate_mutex);
		go = gate_release;
		switch_mutex_unlock(gate_mutex);

		if (go) {
			break;
		}
		switch_yield(1000);
	}

	/* SUCCESS, so switch_core_standard_on_routing() still runs and does the pop. */
	return SWITCH_STATUS_SUCCESS;
}

static switch_state_handler_table_t gate_handlers = {
	/*.on_init */ NULL,
	/*.on_routing */ gate_on_routing,
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

static void gate_arm(const char *uuid)
{
	switch_mutex_lock(gate_mutex);
	switch_set_string(gate_uuid, uuid ? uuid : "");
	gate_armed = uuid ? 1 : 0;
	gate_reached = 0;
	gate_release = 0;
	switch_mutex_unlock(gate_mutex);
}

static void gate_let_go(void)
{
	switch_mutex_lock(gate_mutex);
	gate_armed = 0;
	gate_release = 1;
	switch_mutex_unlock(gate_mutex);
}

static int gate_is_reached(void)
{
	int reached;

	switch_mutex_lock(gate_mutex);
	reached = gate_reached;
	switch_mutex_unlock(gate_mutex);

	return reached;
}

/* One trial. *gated is set only if the thread was confirmed held inside the window. */
static trial_result_t run_queued_extension_trial(int settle_ms, int *gated, int *first_ran)
{
	switch_core_session_t *session = NULL;
	switch_call_cause_t cause = SWITCH_CAUSE_NORMAL_CLEARING;
	switch_channel_t *channel = NULL;
	switch_caller_extension_t *extension = NULL;
	const char *uuid = NULL;
	const char *marker = NULL;
	trial_result_t result = TRIAL_UNKNOWN;
	switch_time_t deadline;

	if (switch_ivr_originate(NULL, &session, &cause, "null/+15553334444", 5,
							 NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS || !session) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[transfer-handoff] queued-ext trial: originate failed: %s\n",
						  switch_channel_cause2str(cause));
		return TRIAL_UNKNOWN;
	}

	channel = switch_core_session_get_channel(session);
	uuid = switch_core_session_get_uuid(session);

	park_watch_reset(uuid);
	uuid_transfer(uuid, XFER_A_DEST);

	if (switch_channel_wait_for_flag(channel, CF_PARK, SWITCH_TRUE, 5000, NULL) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[transfer-handoff] queued-ext trial: channel never parked\n");
		goto done;
	}

	switch_channel_set_variable(channel, MARKER_VAR, NULL);
	/* The precondition transfer used XFER_A_DEST too, so clear its marker. */
	switch_channel_set_variable(channel, MARKER_A_VAR, NULL);
	park_watch_reset(uuid);

	/* Arm, then send a transfer that puts the thread into a routing pass and stops it
	   inside the snapshot -> pop window. */
	gate_arm(uuid);
	uuid_transfer(uuid, XFER_A_DEST);

	deadline = switch_micro_time_now() + 3000000;
	while (switch_micro_time_now() < deadline) {
		if (gate_is_reached()) {
			break;
		}
		switch_yield(1000);
	}

	if (!gate_is_reached()) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "[transfer-handoff] queued-ext trial: gate never reached (state=%s running=%s)\n",
						  switch_channel_state_name(switch_channel_get_state(channel)),
						  switch_channel_state_name(switch_channel_get_running_state(channel)));
		gate_let_go();
		goto done;
	}

	/* The xferext lands in the window: queued, generation bumped, set_state a no-op. */
	if (!(extension = switch_caller_extension_new(session, "xferext", "xferext"))) {
		gate_let_go();
		goto done;
	}
	switch_caller_extension_add_application(session, extension, "set", MARKER_VAR "=" MARKER_VALUE);
	switch_caller_extension_add_application(session, extension, "park", NULL);

	switch_channel_transfer_to_extension(channel, extension);

	if (gated) {
		*gated = 1;
	}

	gate_let_go();

	deadline = switch_micro_time_now() + (settle_ms * 1000);
	while (switch_micro_time_now() < deadline) {
		marker = switch_channel_get_variable(channel, MARKER_VAR);
		if (marker && !strcmp(marker, MARKER_VALUE)) {
			break;
		}
		switch_sleep(20000);
	}

	marker = switch_channel_get_variable(channel, MARKER_VAR);

	if (marker && !strcmp(marker, MARKER_VALUE)) {
		result = TRIAL_REACHED;
	} else if (switch_channel_get_state(channel) == CS_ROUTING &&
			   switch_channel_get_running_state(channel) == CS_ROUTING) {
		result = TRIAL_WEDGED;
	} else if (park_watch_count() > 0) {
		result = TRIAL_REPARKED;
	}

	/* The discarded-pass symptom: the pre-transfer destination runs instead. */
	if (first_ran && switch_channel_get_variable(channel, MARKER_A_VAR)) {
		*first_ran = 1;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					  "[transfer-handoff] queued-ext trial -> %s state=%s running_state=%s up=%d park_events=%d "
					  "marker=%s pre_transfer_dest_ran=%s\n",
					  trial_result_name(result),
					  switch_channel_state_name(switch_channel_get_state(channel)),
					  switch_channel_state_name(switch_channel_get_running_state(channel)),
					  switch_channel_up(channel) ? 1 : 0,
					  park_watch_count(), marker ? marker : "(unset)",
					  switch_channel_get_variable(channel, MARKER_A_VAR) ? "yes" : "no");

  done:
	gate_arm(NULL);
	gate_let_go();
	park_watch_reset(NULL);

	if (switch_channel_up(channel)) {
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
	}
	switch_core_session_rwunlock(session);
	switch_sleep(200000);

	return result;
}

/*
 * Two commands that are not transfers, and must not be treated as one.
 *
 * (a) switch_ivr_uuid_bridge() steps BOTH caller profiles
 *     (switch_channel_step_caller_profile(), which clones and installs a new profile
 *     object) and then sets CS_HIBERNATE. A post-ROUTING check that re-routes on "the
 *     caller profile changed" therefore reads a bridge as a transfer, and because
 *     HIBERNATE -> ROUTING is a legal transition it rewrites the bridge's state: the
 *     originator re-hunts its dialplan while the originatee waits for a bridge that
 *     never starts. mod_fifo steps the profiles the same way.
 *
 * (b) A transfer that is superseded before its ROUTING pass ever happens. Only a ROUTING
 *     entry re-snapshots routed_generation, so the bump outlives the command: if a later
 *     bridge moves the channel to HIBERNATE and then RESET, the sleep branch still sees
 *     a stale generation and drags the channel back to CS_ROUTING - resurrecting an
 *     earlier transfer on top of a later command.
 *
 * Neither trial drives a real uuid_bridge: a bridge needs two sessions and lands in a
 * window too narrow to schedule. They reproduce what the bridge does to THIS channel -
 * step the profile, set the state - at the point the checks read it, which is what the
 * checks are being asked to get right.
 */
static switch_mutex_t *veto_mutex = NULL;
static char veto_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1] = { 0 };
static int veto_armed = 0;
static int veto_reached = 0;
static int veto_release = 0;

/* Vetoes, so switch_core_standard_on_routing() never runs and never overwrites the state
   the test sets while this is held. */
static switch_status_t veto_on_routing(switch_core_session_t *session)
{
	const char *uuid = switch_core_session_get_uuid(session);
	switch_time_t deadline;
	int mine = 0;

	switch_mutex_lock(veto_mutex);
	if (veto_armed && uuid && !strcmp(uuid, veto_uuid)) {
		veto_armed = 0;
		veto_reached = 1;
		mine = 1;
	}
	switch_mutex_unlock(veto_mutex);

	if (!mine) {
		return SWITCH_STATUS_SUCCESS;
	}

	deadline = switch_micro_time_now() + 5000000;
	while (switch_micro_time_now() < deadline) {
		int go;

		switch_mutex_lock(veto_mutex);
		go = veto_release;
		switch_mutex_unlock(veto_mutex);

		if (go) {
			break;
		}
		switch_yield(1000);
	}

	return SWITCH_STATUS_FALSE;
}

static switch_state_handler_table_t veto_handlers = {
	/*.on_init */ NULL,
	/*.on_routing */ veto_on_routing,
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

static void veto_arm(const char *uuid)
{
	switch_mutex_lock(veto_mutex);
	switch_set_string(veto_uuid, uuid ? uuid : "");
	veto_armed = uuid ? 1 : 0;
	veto_reached = 0;
	veto_release = 0;
	switch_mutex_unlock(veto_mutex);
}

static void veto_let_go(void)
{
	switch_mutex_lock(veto_mutex);
	veto_armed = 0;
	veto_release = 1;
	switch_mutex_unlock(veto_mutex);
}

static int veto_is_reached(void)
{
	int reached;

	switch_mutex_lock(veto_mutex);
	reached = veto_reached;
	switch_mutex_unlock(veto_mutex);

	return reached;
}

/*
 * (a) Hold the routing pass, do to the channel what uuid_bridge does - step the caller
 * profile, then set CS_HIBERNATE - and require that the state survives the pass.
 */
static int run_profile_swap_trial(int settle_ms, int *held)
{
	switch_core_session_t *session = NULL;
	switch_call_cause_t cause = SWITCH_CAUSE_NORMAL_CLEARING;
	switch_channel_t *channel = NULL;
	const char *uuid = NULL;
	switch_channel_state_t state = CS_NONE;
	switch_time_t deadline;
	int ok = 0;

	if (switch_ivr_originate(NULL, &session, &cause, "null/+15553334444", 5,
							 NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS || !session) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[transfer-handoff] profile-swap trial: originate failed\n");
		return 0;
	}

	channel = switch_core_session_get_channel(session);
	uuid = switch_core_session_get_uuid(session);

	uuid_transfer(uuid, XFER_A_DEST);

	if (switch_channel_wait_for_flag(channel, CF_PARK, SWITCH_TRUE, 5000, NULL) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[transfer-handoff] profile-swap trial: never parked\n");
		goto done;
	}

	switch_channel_set_variable(channel, MARKER_A_VAR, NULL);

	veto_arm(uuid);
	uuid_transfer(uuid, XFER_A_DEST);

	deadline = switch_micro_time_now() + 3000000;
	while (switch_micro_time_now() < deadline) {
		if (veto_is_reached()) {
			break;
		}
		switch_yield(1000);
	}

	if (!veto_is_reached()) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "[transfer-handoff] profile-swap trial: routing never held\n");
		veto_let_go();
		goto done;
	}

	/* What switch_ivr_uuid_bridge() does to this channel. Neither bumps the transfer
	   generation, because neither is a transfer. */
	switch_channel_step_caller_profile(channel);
	switch_channel_set_state(channel, CS_HIBERNATE);

	if (held) {
		*held = 1;
	}

	veto_let_go();

	switch_sleep(settle_ms * 1000);

	state = switch_channel_get_state(channel);
	/* CS_HIBERNATE must stand. A re-route rewrites it to CS_ROUTING. */
	ok = (state == CS_HIBERNATE && !switch_channel_get_variable(channel, MARKER_A_VAR));

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					  "[transfer-handoff] profile-swap trial -> %s state=%s running_state=%s dialplan_reran=%s\n",
					  ok ? "HIBERNATE STOOD" : "OVERRIDDEN",
					  switch_channel_state_name(state),
					  switch_channel_state_name(switch_channel_get_running_state(channel)),
					  switch_channel_get_variable(channel, MARKER_A_VAR) ? "yes" : "no");

  done:
	veto_arm(NULL);
	veto_let_go();

	if (switch_channel_up(channel)) {
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
	}
	switch_core_session_rwunlock(session);
	switch_sleep(200000);

	return ok;
}

/*
 * (b) Transfer, then move the channel elsewhere before its routing pass runs, the way a
 * later uuid_bridge would. The transfer must not resurrect out of the sleep branch.
 */
static int run_superseded_trial(int settle_ms, int *held)
{
	switch_core_session_t *session = NULL;
	switch_call_cause_t cause = SWITCH_CAUSE_NORMAL_CLEARING;
	switch_channel_t *channel = NULL;
	const char *uuid = NULL;
	const char *marker = NULL;
	switch_channel_state_t state = CS_NONE;
	switch_time_t deadline;
	int ok = 0;

	if (switch_ivr_originate(NULL, &session, &cause, "null/+15553334444", 5,
							 NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL) != SWITCH_STATUS_SUCCESS || !session) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[transfer-handoff] superseded trial: originate failed\n");
		return 0;
	}

	channel = switch_core_session_get_channel(session);
	uuid = switch_core_session_get_uuid(session);

	uuid_transfer(uuid, XFER_A_DEST);

	if (switch_channel_wait_for_flag(channel, CF_PARK, SWITCH_TRUE, 5000, NULL) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[transfer-handoff] superseded trial: never parked\n");
		goto done;
	}

	switch_channel_set_variable(channel, MARKER_VAR, NULL);

	/* Stop the thread at the top of the loop, so the transfer below bumps the generation
	   and sets CS_ROUTING but no ROUTING pass ever consumes it. */
	switch_channel_set_flag(channel, CF_BLOCK_STATE);

	uuid_transfer(uuid, XFER_B_DEST);

	deadline = switch_micro_time_now() + 3000000;
	while (switch_micro_time_now() < deadline) {
		if (switch_channel_get_state(channel) == CS_ROUTING &&
			switch_channel_get_running_state(channel) == CS_EXECUTE) {
			break;
		}
		switch_yield(1000);
	}

	if (!(switch_channel_get_state(channel) == CS_ROUTING &&
		  switch_channel_get_running_state(channel) == CS_EXECUTE)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "[transfer-handoff] superseded trial: block not achieved (state=%s running=%s)\n",
						  switch_channel_state_name(switch_channel_get_state(channel)),
						  switch_channel_state_name(switch_channel_get_running_state(channel)));
		switch_channel_clear_flag(channel, CF_BLOCK_STATE);
		goto done;
	}

	/* The later command supersedes the transfer. */
	switch_channel_set_state(channel, CS_HIBERNATE);

	if (held) {
		*held = 1;
	}

	switch_channel_clear_flag(channel, CF_BLOCK_STATE);

	switch_sleep(settle_ms * 1000);

	state = switch_channel_get_state(channel);
	marker = switch_channel_get_variable(channel, MARKER_VAR);
	/* The later state must stand and the transfer's extension must not have run. */
	ok = (state == CS_HIBERNATE && !(marker && !strcmp(marker, MARKER_VALUE)));

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					  "[transfer-handoff] superseded trial -> %s state=%s running_state=%s transfer_resurrected=%s\n",
					  ok ? "LATER COMMAND WON" : "TRANSFER RESURRECTED",
					  switch_channel_state_name(state),
					  switch_channel_state_name(switch_channel_get_running_state(channel)),
					  (marker && !strcmp(marker, MARKER_VALUE)) ? "yes" : "no");

  done:
	switch_channel_clear_flag(channel, CF_BLOCK_STATE);

	if (switch_channel_up(channel)) {
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
	}
	switch_core_session_rwunlock(session);
	switch_sleep(200000);

	return ok;
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
			/*
			 * NOT fst_pool: that is destroyed the instant the test body ends, and these
			 * mutexes are reachable afterwards. switch_core_remove_state_handler() takes
			 * runtime.global_mutex but switch_core_get_state_handler() reads the array
			 * unlocked, so removal is not a barrier against an in-flight handler; and
			 * switch_channel_set_post_dialplan_function() asserts non-NULL, so the hook
			 * cannot be unset at all and lives until the channel is destroyed. One pool
			 * for the whole binary, never destroyed.
			 */
			if (!test_pool) {
				fst_requires(switch_core_new_memory_pool(&test_pool) == SWITCH_STATUS_SUCCESS);
				switch_mutex_init(&park_mutex, SWITCH_MUTEX_NESTED, test_pool);
				switch_mutex_init(&freeze_mutex, SWITCH_MUTEX_NESTED, test_pool);
				switch_mutex_init(&stall_mutex, SWITCH_MUTEX_NESTED, test_pool);
				switch_mutex_init(&hunt_mutex, SWITCH_MUTEX_NESTED, test_pool);
				switch_mutex_init(&gate_mutex, SWITCH_MUTEX_NESTED, test_pool);
				switch_mutex_init(&veto_mutex, SWITCH_MUTEX_NESTED, test_pool);
			}
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
			fst_requires(switch_core_add_state_handler(&freeze_handlers) >= 0);

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

		FST_TEST_BEGIN(xferext_transfer_wakes_a_thread_asleep_in_routing)
		{
			int settle_ms = env_int("TRANSFER_HANDOFF_SETTLE_MS", 1200);
			int trials = env_int("TRANSFER_HANDOFF_TRIALS", 2);
			int t, stalled_total = 0, reached = 0, wedged = 0, unknown = 0;

			fst_requires(switch_event_bind("transfer_handoff", SWITCH_EVENT_CHANNEL_PARK, SWITCH_EVENT_SUBCLASS_ANY,
										   park_event_handler, NULL) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_add_state_handler(&stall_handlers) >= 0);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "========== TELCORE-412 xferext into a sleeping routing thread x %d ==========\n", trials);

			for (t = 0; t < trials; t++) {
				int stalled = 0;
				trial_result_t r = run_xferext_trial(settle_ms, &stalled);

				stalled_total += stalled;

				switch (r) {
				case TRIAL_REACHED: reached++; break;
				case TRIAL_WEDGED:  wedged++;  break;
				default:            unknown++; break;
				}
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "========== TELCORE-412 xferext: stalled %d/%d | reached %d | wedged %d | unknown %d ==========\n",
							  stalled_total, trials, reached, wedged, unknown);

			switch_core_remove_state_handler(&stall_handlers);
			switch_event_unbind_callback(park_event_handler);

			/* The probe itself must work, or the result below means nothing. */
			fst_requires(stalled_total == trials);

			fst_check(reached == trials);
			fst_check(wedged == 0);
			fst_check(unknown == 0);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(transfer_after_the_hunt_discards_the_stale_extension)
		{
			int settle_ms = env_int("TRANSFER_HANDOFF_SETTLE_MS", 1200);
			int trials = env_int("TRANSFER_HANDOFF_TRIALS", 2);
			int t, hunted_total = 0, first_ran_total = 0, ok_but_dropped = 0;
			int reached = 0, wedged = 0, reparked = 0, unknown = 0;

			fst_requires(switch_event_bind("transfer_handoff", SWITCH_EVENT_CHANNEL_PARK, SWITCH_EVENT_SUBCLASS_ANY,
										   park_event_handler, NULL) == SWITCH_STATUS_SUCCESS);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "========== TELCORE-412 transfer landing after the hunt x %d ==========\n", trials);

			for (t = 0; t < trials; t++) {
				int hunted = 0, first_ran = 0;
				switch_bool_t both_ok = SWITCH_FALSE;
				trial_result_t r = run_post_hunt_trial(settle_ms, &hunted, &first_ran, &both_ok);

				hunted_total += hunted;
				first_ran_total += first_ran;

				if (r != TRIAL_REACHED && both_ok) {
					ok_but_dropped++;
				}

				switch (r) {
				case TRIAL_REACHED:  reached++;  break;
				case TRIAL_WEDGED:   wedged++;   break;
				case TRIAL_REPARKED: reparked++; break;
				default:             unknown++;  break;
				}
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "========== TELCORE-412 post-hunt: hooked %d/%d | reached %d | wedged %d | reparked %d "
							  "| unknown %d | stale extension ran: %d | +OK but dropped: %d ==========\n",
							  hunted_total, trials, reached, wedged, reparked, unknown, first_ran_total, ok_but_dropped);

			switch_event_unbind_callback(park_event_handler);

			/* The probe itself must work, or the result below means nothing. */
			fst_requires(hunted_total == trials);

			/* The point of the test: the extension hunted from the replaced profile - the
			   blocking park - must never run. */
			fst_check(first_ran_total == 0);

			fst_check(reached == trials);
			fst_check(wedged == 0);
			fst_check(unknown == 0);
			fst_check(ok_but_dropped == 0);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(queued_extension_consumed_by_the_pass_is_not_discarded)
		{
			int settle_ms = env_int("TRANSFER_HANDOFF_SETTLE_MS", 1200);
			int trials = env_int("TRANSFER_HANDOFF_TRIALS", 2);
			int t, gated_total = 0, first_ran_total = 0;
			int reached = 0, wedged = 0, reparked = 0, unknown = 0;

			fst_requires(switch_event_bind("transfer_handoff", SWITCH_EVENT_CHANNEL_PARK, SWITCH_EVENT_SUBCLASS_ANY,
										   park_event_handler, NULL) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_add_state_handler(&gate_handlers) >= 0);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "========== TELCORE-412 xferext landing before the pop x %d ==========\n", trials);

			for (t = 0; t < trials; t++) {
				int gated = 0, first_ran = 0;
				trial_result_t r = run_queued_extension_trial(settle_ms, &gated, &first_ran);

				gated_total += gated;
				first_ran_total += first_ran;

				switch (r) {
				case TRIAL_REACHED:  reached++;  break;
				case TRIAL_WEDGED:   wedged++;   break;
				case TRIAL_REPARKED: reparked++; break;
				default:             unknown++;  break;
				}
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "========== TELCORE-412 queued-ext: gated %d/%d | reached %d | wedged %d | reparked %d "
							  "| unknown %d | pre-transfer destination ran: %d ==========\n",
							  gated_total, trials, reached, wedged, reparked, unknown, first_ran_total);

			switch_core_remove_state_handler(&gate_handlers);
			switch_event_unbind_callback(park_event_handler);

			/* The probe itself must work, or the result below means nothing. */
			fst_requires(gated_total == trials);

			/* The queued extension must run. Re-routing on the generation instead loses
			   it: pass two has nothing queued and an untouched caller profile, so it
			   re-hunts the pre-transfer destination or hangs the call up. */
			fst_check(reached == trials);
			fst_check(first_ran_total == 0);
			fst_check(wedged == 0);
			fst_check(unknown == 0);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(a_later_command_is_not_overridden_by_the_reroute)
		{
			int settle_ms = env_int("TRANSFER_HANDOFF_SETTLE_MS", 1200);
			int trials = env_int("TRANSFER_HANDOFF_TRIALS", 2);
			int t, held_total = 0, stood = 0;

			fst_requires(switch_core_add_state_handler(&veto_handlers) >= 0);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "========== TELCORE-412 bridge-style profile swap during routing x %d ==========\n", trials);

			for (t = 0; t < trials; t++) {
				int held = 0;

				stood += run_profile_swap_trial(settle_ms, &held);
				held_total += held;
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "========== TELCORE-412 profile swap: held %d/%d | state stood %d ==========\n",
							  held_total, trials, stood);

			switch_core_remove_state_handler(&veto_handlers);

			/* The probe itself must work, or the result below means nothing. */
			fst_requires(held_total == trials);

			/* A caller-profile swap is not a transfer. The state the other command chose
			   must survive the routing pass. */
			fst_check(stood == trials);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(a_superseded_transfer_does_not_resurrect)
		{
			int settle_ms = env_int("TRANSFER_HANDOFF_SETTLE_MS", 1200);
			int trials = env_int("TRANSFER_HANDOFF_TRIALS", 2);
			int t, held_total = 0, won = 0;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "========== TELCORE-412 transfer superseded before it routed x %d ==========\n", trials);

			for (t = 0; t < trials; t++) {
				int held = 0;

				won += run_superseded_trial(settle_ms, &held);
				held_total += held;
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "========== TELCORE-412 superseded: held %d/%d | later command won %d ==========\n",
							  held_total, trials, won);

			/* The probe itself must work, or the result below means nothing. */
			fst_requires(held_total == trials);

			/* The stale generation must not drag the channel back to CS_ROUTING. */
			fst_check(won == trials);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
