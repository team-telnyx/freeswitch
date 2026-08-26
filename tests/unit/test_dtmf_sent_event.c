/*
 * TELCORE-373: confirmation event for outbound DTMF.
 *
 * FreeSWITCH fires SWITCH_EVENT_DTMF only on the receive side
 * (switch_channel_dequeue_dtmf). Nothing is emitted when we transmit a digit,
 * so the B2BUA cannot tell a customer that a requested DTMF actually went out
 * on the wire -- the "+OK" from uuid_send_dtmf only means "queued".
 *
 * This test drives the real RFC 2833 send path: a digit is queued on an RTP
 * session and do_2833() is pumped until the end-of-digit packets are written.
 * At that point -- and only then -- a CUSTOM/telnyx::dtmf_sent event must be
 * fired for the channel, carrying the digit, the requested duration and the
 * duration actually transmitted.
 *
 * The "call_control" header assertion matters as much as the rest: the ZMQ
 * publisher in mod_telnyx filters the call_control queue on that header
 * (conf/telnyx/zmq_publishers.json), so an event built without full channel
 * data would never reach the B2BUA.
 */

#include <switch.h>
#include <test/switch_test.h>

static const char *rx_host = "127.0.0.1";
static switch_port_t rx_port = 12344;
static const char *tx_host = "127.0.0.1";
static switch_port_t tx_port = 12346;

static const switch_payload_t TEST_PT = 8;
static const switch_payload_t TEST_TE_PT = 101;

static switch_memory_pool_t *event_pool = NULL;
static switch_mutex_t *event_mutex = NULL;
static switch_event_t *dtmf_sent_event = NULL;
static int dtmf_sent_event_count = 0;

static void dtmf_sent_event_handler(switch_event_t *event)
{
	const char *subclass = switch_event_get_header(event, "Event-Subclass");

	if (zstr(subclass) || strcmp(subclass, "telnyx::dtmf_sent")) {
		return;
	}

	switch_mutex_lock(event_mutex);
	if (!dtmf_sent_event) {
		switch_event_dup(&dtmf_sent_event, event);
	}
	dtmf_sent_event_count++;
	switch_mutex_unlock(event_mutex);
}

static int got_dtmf_sent_event(void)
{
	int got;

	switch_mutex_lock(event_mutex);
	got = dtmf_sent_event_count;
	switch_mutex_unlock(event_mutex);

	return got;
}

static const char *dtmf_sent_header(const char *name)
{
	const char *value;

	switch_mutex_lock(event_mutex);
	value = dtmf_sent_event ? switch_event_get_header(dtmf_sent_event, name) : NULL;
	switch_mutex_unlock(event_mutex);

	return value;
}

FST_CORE_BEGIN("./conf")
{
FST_SUITE_BEGIN(test_dtmf_sent_event)
{
FST_SETUP_BEGIN()
{
	fst_requires_module("mod_loopback");

	/* Not fst_pool: FST_TEARDOWN_BEGIN() destroys fst_pool inside the macro, before the
	   teardown body that unbinds the handler ever runs (switch_test.h), so a mutex taken
	   from fst_pool would be freed while the handler is still bound. This pool is created
	   once for the whole suite and left to the core to reclaim at shutdown, which also
	   keeps the mutex valid for a dispatch already in flight when we unbind. */
	if (!event_pool) {
		switch_core_new_memory_pool(&event_pool);
		switch_mutex_init(&event_mutex, SWITCH_MUTEX_NESTED, event_pool);
	}

	dtmf_sent_event = NULL;
	dtmf_sent_event_count = 0;
	/* Bound here rather than in the test body: an aborted assertion skips the rest of
	   the body, so a bind done there could never be undone. */
	switch_event_bind("test_dtmf_sent_event", SWITCH_EVENT_CUSTOM, SWITCH_EVENT_SUBCLASS_ANY, dtmf_sent_event_handler, NULL);
}
FST_SETUP_END()

FST_TEARDOWN_BEGIN()
{
	switch_event_unbind_callback(dtmf_sent_event_handler);
	switch_event_destroy(&dtmf_sent_event);
}
FST_TEARDOWN_END()

FST_TEST_BEGIN(dtmf_sent_event_fires_when_rfc2833_digit_finishes)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_memory_pool_t *pool = NULL;
	switch_rtp_t *rtp_session = NULL;
	switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID] = { 0 };
	/* 800 samples == 100ms at 8kHz */
	switch_dtmf_t dtmf = { '5', 800, 0, SWITCH_DTMF_APP };
	switch_call_cause_t cause;
	switch_status_t status;
	const char *err = NULL;
	const char *value;
	int i;

	switch_core_new_memory_pool(&pool);

	status = switch_ivr_originate(NULL, &session, &cause, "null/+15553334444", 2, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_requires(session);
	fst_check(status == SWITCH_STATUS_SUCCESS);

	channel = switch_core_session_get_channel(session);
	fst_requires(channel);
	switch_channel_set_variable(channel, "call_control", "true");

	switch_core_memory_pool_set_data(pool, "__session", session);
	/* 160 samples per 20ms interval == 8kHz */
	rtp_session = switch_rtp_new(rx_host, rx_port, tx_host, tx_port, TEST_PT, 160, 20 * 1000, flags, "soft", &err, pool);
	fst_requires(rtp_session);
	fst_requires(switch_rtp_ready(rtp_session));
	switch_rtp_set_default_payload(rtp_session, TEST_PT);
	switch_rtp_set_telephony_event(rtp_session, TEST_TE_PT);

	status = switch_rtp_queue_rfc2833(rtp_session, &dtmf);
	fst_xcheck(status == SWITCH_STATUS_SUCCESS, "queue outbound RFC 2833 digit");

	/* No event may be fired while the digit is still on the wire. */
	fst_xcheck(got_dtmf_sent_event() == 0, "no confirmation before the digit is transmitted");

	for (i = 0; i < 100 && !got_dtmf_sent_event(); i++) {
		do_2833(rtp_session);
		switch_yield(20000);
	}

	fst_xcheck(got_dtmf_sent_event() == 1, "exactly one confirmation event for one digit");
	fst_requires(dtmf_sent_event);

	value = dtmf_sent_header("DTMF-Digit");
	fst_xcheck(!zstr(value) && !strcmp(value, "5"), "DTMF-Digit is the transmitted digit");

	value = dtmf_sent_header("DTMF-Source");
	fst_xcheck(!zstr(value) && !strcmp(value, "APP"), "DTMF-Source distinguishes injected digits from relayed ones");

	value = dtmf_sent_header("DTMF-Method");
	fst_xcheck(!zstr(value) && !strcmp(value, "RFC2833"), "DTMF-Method is the transport used");

	value = dtmf_sent_header("DTMF-Duration");
	fst_xcheck(!zstr(value) && atoi(value) == 800, "DTMF-Duration is the requested duration in 8kHz samples");

	value = dtmf_sent_header("DTMF-Duration-MS");
	fst_xcheck(!zstr(value) && atoi(value) == 100, "DTMF-Duration-MS reports the requested length in ms");

	value = dtmf_sent_header("DTMF-Duration-Clock-Rate");
	fst_xcheck(!zstr(value) && atoi(value) == 8000, "DTMF-Duration is always in the 8kHz domain");

	value = dtmf_sent_header("DTMF-Duration-Sent");
	fst_xcheck(!zstr(value) && atoi(value) == 800, "DTMF-Duration-Sent is the duration actually transmitted");

	value = dtmf_sent_header("DTMF-Duration-Sent-MS");
	fst_xcheck(!zstr(value) && atoi(value) == 100, "DTMF-Duration-Sent-MS reports the transmitted length in ms");

	value = dtmf_sent_header("DTMF-Duration-Sent-Clock-Rate");
	fst_xcheck(!zstr(value) && atoi(value) == 8000, "DTMF-Duration-Sent-Clock-Rate states the rate DTMF-Duration-Sent is in");

	value = dtmf_sent_header("Unique-ID");
	fst_xcheck(!zstr(value) && !strcmp(value, switch_core_session_get_uuid(session)), "event is bound to the sending channel");

	value = dtmf_sent_header("call_control");
	fst_xcheck(!zstr(value) && !strcmp(value, "true"), "event carries the call_control header the ZMQ publisher filters on");

	switch_rtp_destroy(&rtp_session);
	switch_core_session_rwunlock(session);
	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()

FST_TEST_BEGIN(dtmf_sent_event_keeps_the_two_clock_domains_apart)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_memory_pool_t *pool = NULL;
	switch_rtp_t *rtp_session = NULL;
	switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID] = { 0 };
	/* 2000 == SWITCH_DEFAULT_DTMF_DURATION, i.e. 250ms in FS's 8kHz convention. */
	switch_dtmf_t dtmf = { '5', 2000, 0, SWITCH_DTMF_APP };
	switch_call_cause_t cause;
	switch_status_t status;
	const char *err = NULL;
	const char *value;
	int i;

	switch_core_new_memory_pool(&pool);

	status = switch_ivr_originate(NULL, &session, &cause, "null/+15553334444", 2, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_requires(session);
	fst_check(status == SWITCH_STATUS_SUCCESS);

	channel = switch_core_session_get_channel(session);
	fst_requires(channel);
	switch_channel_set_variable(channel, "call_control", "true");

	switch_core_memory_pool_set_data(pool, "__session", session);
	/* 960 samples per 20ms interval == 48kHz, the rate an Opus leg is created at
	   (switch_core_media.c). At 8kHz the requested and transmitted domains coincide
	   and the distinction is invisible, so this is the case that pins it down. */
	rtp_session = switch_rtp_new(rx_host, rx_port, tx_host, tx_port, TEST_PT, 960, 20 * 1000, flags, "soft", &err, pool);
	fst_requires(rtp_session);
	fst_requires(switch_rtp_ready(rtp_session));
	switch_rtp_set_default_payload(rtp_session, TEST_PT);
	switch_rtp_set_telephony_event(rtp_session, TEST_TE_PT);

	status = switch_rtp_queue_rfc2833(rtp_session, &dtmf);
	fst_xcheck(status == SWITCH_STATUS_SUCCESS, "queue outbound RFC 2833 digit");

	for (i = 0; i < 100 && !got_dtmf_sent_event(); i++) {
		do_2833(rtp_session);
		switch_yield(20000);
	}

	fst_xcheck(got_dtmf_sent_event() == 1, "exactly one confirmation event for one digit");
	fst_requires(dtmf_sent_event);

	value = dtmf_sent_header("DTMF-Duration");
	fst_xcheck(!zstr(value) && atoi(value) == 2000, "DTMF-Duration stays in the 8kHz domain regardless of the session rate");

	value = dtmf_sent_header("DTMF-Duration-Clock-Rate");
	fst_xcheck(!zstr(value) && atoi(value) == 8000, "DTMF-Duration-Clock-Rate is 8kHz, not the session rate");

	value = dtmf_sent_header("DTMF-Duration-MS");
	fst_xcheck(!zstr(value) && atoi(value) == 250, "DTMF-Duration-MS is the requested 250ms");

	value = dtmf_sent_header("DTMF-Duration-Sent-Clock-Rate");
	fst_xcheck(!zstr(value) && atoi(value) == 48000, "DTMF-Duration-Sent-Clock-Rate is the session rate");

	/* do_2833() compares out_digit_sofar (48kHz samples) against out_digit_dur (8kHz
	   samples), so the digit ends after ceil(2000/960) == 3 intervals: 2880 samples,
	   60ms, far short of the 250ms asked for. That truncation is pre-existing core
	   behaviour; what matters here is that the event describes it without lying -- the
	   two MS values are directly comparable and disagree. */
	value = dtmf_sent_header("DTMF-Duration-Sent");
	fst_xcheck(!zstr(value) && atoi(value) == 2880, "DTMF-Duration-Sent is in session samples");

	value = dtmf_sent_header("DTMF-Duration-Sent-MS");
	fst_xcheck(!zstr(value) && atoi(value) == 60, "DTMF-Duration-Sent-MS is comparable to DTMF-Duration-MS");

	switch_rtp_destroy(&rtp_session);
	switch_core_session_rwunlock(session);
	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()

FST_TEST_BEGIN(no_dtmf_sent_event_when_the_end_packets_are_rejected)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_memory_pool_t *pool = NULL;
	switch_rtp_t *rtp_session = NULL;
	switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID] = { 0 };
	switch_dtmf_t dtmf = { '5', 800, 0, SWITCH_DTMF_APP };
	switch_call_cause_t cause;
	switch_status_t status;
	const char *err = NULL;
	int i;

	switch_core_new_memory_pool(&pool);

	status = switch_ivr_originate(NULL, &session, &cause, "null/+15553334444", 2, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_requires(session);
	fst_check(status == SWITCH_STATUS_SUCCESS);

	channel = switch_core_session_get_channel(session);
	fst_requires(channel);
	switch_channel_set_variable(channel, "call_control", "true");

	switch_core_memory_pool_set_data(pool, "__session", session);
	rtp_session = switch_rtp_new(rx_host, rx_port, tx_host, tx_port, TEST_PT, 160, 20 * 1000, flags, "soft", &err, pool);
	fst_requires(rtp_session);
	fst_requires(switch_rtp_ready(rtp_session));
	switch_rtp_set_default_payload(rtp_session, TEST_PT);
	switch_rtp_set_telephony_event(rtp_session, TEST_TE_PT);

	status = switch_rtp_queue_rfc2833(rtp_session, &dtmf);
	fst_xcheck(status == SWITCH_STATUS_SUCCESS, "queue outbound RFC 2833 digit");

	/* Take IO away after queueing: switch_rtp_write_manual() now fails switch_rtp_ready()
	   and returns -1 for every packet, so nothing reaches the socket even though do_2833()
	   still walks the digit to completion. */
	switch_rtp_clear_flag(rtp_session, SWITCH_RTP_FLAG_IO);

	/* 800 samples at 160 per interval completes in 5 pumps; 20 is well past the point
	   where the working case above has already fired. */
	for (i = 0; i < 20; i++) {
		do_2833(rtp_session);
		switch_yield(20000);
	}

	fst_xcheck(got_dtmf_sent_event() == 0, "a digit whose end packets were all rejected is never confirmed");

	switch_rtp_destroy(&rtp_session);
	switch_core_session_rwunlock(session);
	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()

FST_TEST_BEGIN(no_dtmf_sent_event_for_an_event_sensitive_digit)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	switch_memory_pool_t *pool = NULL;
	switch_rtp_t *rtp_session = NULL;
	switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID] = { 0 };
	switch_dtmf_t dtmf = { '7', 800, DTMF_FLAG_EVENT_SENSITIVE, SWITCH_DTMF_APP };
	switch_call_cause_t cause;
	switch_status_t status;
	const char *err = NULL;
	int i;

	switch_core_new_memory_pool(&pool);

	status = switch_ivr_originate(NULL, &session, &cause, "null/+15553334444", 2, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
	fst_requires(session);
	fst_check(status == SWITCH_STATUS_SUCCESS);

	channel = switch_core_session_get_channel(session);
	fst_requires(channel);
	switch_channel_set_variable(channel, "call_control", "true");

	switch_core_memory_pool_set_data(pool, "__session", session);
	rtp_session = switch_rtp_new(rx_host, rx_port, tx_host, tx_port, TEST_PT, 160, 20 * 1000, flags, "soft", &err, pool);
	fst_requires(rtp_session);
	fst_requires(switch_rtp_ready(rtp_session));
	switch_rtp_set_default_payload(rtp_session, TEST_PT);
	switch_rtp_set_telephony_event(rtp_session, TEST_TE_PT);

	status = switch_rtp_queue_rfc2833(rtp_session, &dtmf);
	fst_xcheck(status == SWITCH_STATUS_SUCCESS, "queue outbound RFC 2833 digit");

	/* Pump past the point where a non-sensitive digit would have been confirmed. */
	for (i = 0; i < 20; i++) {
		do_2833(rtp_session);
		switch_yield(20000);
	}

	fst_xcheck(got_dtmf_sent_event() == 0, "a digit flagged event-sensitive is sent but never reported");

	switch_rtp_destroy(&rtp_session);
	switch_core_session_rwunlock(session);
	switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()

}
FST_SUITE_END()
}
FST_CORE_END()
