#ifndef SWITCH_TELNYX_H
#define SWITCH_TELNYX_H

#include "switch.h"

SWITCH_BEGIN_EXTERN_C

typedef int (*switch_telnyx_hangup_cause_to_sip_func)(switch_core_session_t *, switch_call_cause_t);
typedef void (*switch_telnyx_sofia_on_hangup_func)(switch_core_session_t *);
typedef switch_bool_t (*switch_telnyx_sofia_on_init_func)(switch_core_session_t *);
typedef void (*switch_telnyx_channel_on_answer_func)(switch_core_session_t *);
typedef void (*switch_telnyx_on_set_variable_func)(switch_channel_t*, const char*, const char*);
typedef int (*switch_telnyx_recover_func)(switch_core_session_t *);
typedef void (*switch_telnyx_on_channel_recover_func)(switch_channel_t*);
typedef void (*switch_telnyx_on_populate_event_func)(switch_event_t *);
typedef void (*switch_telnyx_on_populate_core_heartbeat_func)(switch_event_t *);
typedef void (*switch_telnyx_on_populate_plain_status_func)(switch_stream_handle_t *);
typedef void (*switch_telnyx_on_populate_json_status_func)(cJSON *);
typedef void (*switch_telnyx_publish_json_event_func)(const char*);
typedef void (*switch_telnyx_process_audio_stats_func)(switch_core_session_t*,switch_rtp_stats_t*);
typedef void (*switch_telnyx_process_audio_quality_func)(switch_core_session_t*,double);
typedef void (*switch_telnyx_process_flaws_func)(switch_rtp_t *,int);
typedef void (*switch_telnyx_process_packet_loss_func)(switch_rtp_t *,int);
typedef void (*switch_telnyx_set_current_trace_message_func)(const char*);
typedef switch_call_cause_t (*switch_telnyx_recompute_cause_code_func)(switch_channel_t*, int , switch_call_cause_t);
typedef switch_bool_t (*switch_telnyx_sip_cause_to_q850_func)(int, switch_call_cause_t*);
typedef switch_bool_t (*switch_telnyx_on_media_timeout_func)(switch_channel_t*, switch_rtp_t*);
typedef void (*switch_telnyx_channel_event_set_basic_data_func)(switch_channel_t*, switch_event_t*);
typedef switch_bool_t (*switch_telnyx_on_add_media_bug_func)(switch_media_bug_t**, switch_media_bug_t*, const char*, const char*);

/* Acquire a ref-counted NUA handle for a named mod_sofia profile.
 * Returns nua_t* as void*. The caller MUST call switch_telnyx_sofia_release_profile()
 * when done using the NUA pointer to release the read-lock on the profile.
 * The profile_handle output parameter receives an opaque handle that must be
 * passed to the release function. Returns NULL if profile is not found/running. */
typedef void * (*switch_telnyx_sofia_find_nua_func)(const char *profile_name, void **profile_handle);

/* Release the profile read-lock acquired by switch_telnyx_sofia_find_nua().
 * Must be called once for every successful find_nua call. */
typedef void (*switch_telnyx_sofia_release_profile_func)(void *profile_handle);

/* Get the SIP URL for a named mod_sofia profile (e.g. "IP:PORT").
 * When use_tls is true, returns the TLS IP:PORT instead.
 * Writes into buf up to buflen bytes. Returns SWITCH_STATUS_SUCCESS on success. */
typedef switch_status_t (*switch_telnyx_sofia_find_profile_url_func)(const char *profile_name, int use_tls, char *buf, switch_size_t buflen);

/* NUA event handler callback for external modules that own NUA handles.
 *
 * When set, mod_sofia dispatches ALL NUA events to this handler before its own
 * processing. This is necessary because external modules bind their own structs
 * (not sofia_private_t) to NUA handles — if mod_sofia processed these events,
 * it would dereference the wrong struct type.
 *
 * The handler identifies its own handles via a magic sentinel in hmagic and
 * returns SWITCH_TRUE to consume the event, SWITCH_FALSE to fall through.
 *
 * Performance: mod_sofia pre-filters by event type — only register/unregister
 * responses and inbound events on handles with non-NULL hmagic are dispatched.
 * Call-path events (INVITE, BYE, ACK, etc.) on mod_sofia's own handles are
 * never dispatched to this handler. */
typedef switch_bool_t (*switch_telnyx_sofia_register_handler_func)(
	int event, int status, const char *phrase,
	void *nua, void *nh, void *hmagic,
	const void *sip, void *tags);

typedef struct switch_telnyx_event_dispatch_s {
	switch_telnyx_hangup_cause_to_sip_func switch_telnyx_hangup_cause_to_sip;
	switch_telnyx_sofia_on_init_func switch_telnyx_sofia_on_init;
	switch_telnyx_sofia_on_hangup_func switch_telnyx_sofia_on_hangup;
	switch_telnyx_channel_on_answer_func switch_telnyx_channel_on_answer;
	switch_telnyx_on_set_variable_func switch_telnyx_on_set_variable;
	switch_telnyx_recover_func switch_telnyx_call_recover;
	switch_telnyx_on_channel_recover_func switch_telnyx_on_channel_recover;
	switch_telnyx_publish_json_event_func switch_telnyx_publish_json_event;
	switch_telnyx_process_audio_stats_func switch_telnyx_process_audio_stats;
	switch_telnyx_process_audio_quality_func switch_telnyx_process_audio_quality;
	switch_telnyx_process_flaws_func switch_telnyx_process_flaws;
	switch_telnyx_process_packet_loss_func switch_telnyx_process_packet_loss;
	switch_telnyx_set_current_trace_message_func switch_telnyx_set_current_trace_message;
	switch_telnyx_recompute_cause_code_func switch_telnyx_recompute_cause_code;
	switch_telnyx_sip_cause_to_q850_func switch_telnyx_sip_cause_to_q850;
	switch_telnyx_on_media_timeout_func switch_telnyx_on_media_timeout;
	switch_telnyx_channel_event_set_basic_data_func switch_telnyx_channel_event_set_basic_data;
	switch_telnyx_on_add_media_bug_func switch_telnyx_on_add_media_bug;
	switch_telnyx_sofia_register_handler_func switch_telnyx_sofia_register_handler;
	switch_telnyx_sofia_find_nua_func switch_telnyx_sofia_find_nua;
	switch_telnyx_sofia_release_profile_func switch_telnyx_sofia_release_profile;
	switch_telnyx_sofia_find_profile_url_func switch_telnyx_sofia_find_profile_url;
} switch_telnyx_event_dispatch_t;

SWITCH_DECLARE(void) switch_telnyx_init(switch_memory_pool_t *pool);
SWITCH_DECLARE(void) switch_telnyx_deinit(void);
SWITCH_DECLARE(int) switch_telnyx_hangup_cause_to_sip(switch_core_session_t *session, switch_call_cause_t cause);
SWITCH_DECLARE(switch_bool_t) switch_telnyx_sofia_on_init(switch_core_session_t *session);
SWITCH_DECLARE(void) switch_telnyx_sofia_on_hangup(switch_core_session_t *session);
SWITCH_DECLARE(void) switch_telnyx_channel_on_answer(switch_core_session_t *session);
SWITCH_DECLARE(switch_telnyx_event_dispatch_t*) switch_telnyx_event_dispatch(void);
SWITCH_DECLARE(void) switch_telnyx_on_set_variable(switch_channel_t* channel, const char* name, const char* value);
SWITCH_DECLARE(int) switch_telnyx_call_recover(switch_core_session_t* session);
SWITCH_DECLARE(void) switch_telnyx_on_channel_recover(switch_channel_t* channel);
SWITCH_DECLARE(void) switch_telnyx_on_populate_event(switch_event_t* event);
SWITCH_DECLARE(void) switch_telnyx_on_populate_core_heartbeat(switch_event_t* event);
SWITCH_DECLARE(void) switch_telnyx_add_populate_event_callback(switch_telnyx_on_populate_event_func cb);
SWITCH_DECLARE(void) switch_telnyx_on_populate_api_plain_status(switch_stream_handle_t *stream);
SWITCH_DECLARE(void) switch_telnyx_on_populate_api_json_status(cJSON* reply);
SWITCH_DECLARE(void) switch_telnyx_add_core_heartbeat_callback(switch_telnyx_on_populate_core_heartbeat_func cb);
SWITCH_DECLARE(void) switch_telnyx_add_populate_api_plain_status_callback(switch_telnyx_on_populate_plain_status_func cb);
SWITCH_DECLARE(void) switch_telnyx_add_populate_api_json_status_callback(switch_telnyx_on_populate_json_status_func cb);
SWITCH_DECLARE(void) switch_telnyx_publish_json_event(const char* json);
SWITCH_DECLARE(void) switch_telnyx_process_audio_stats(switch_core_session_t* session, switch_rtp_stats_t* stats);
SWITCH_DECLARE(void) switch_telnyx_process_audio_quality(switch_core_session_t* session, double r);
SWITCH_DECLARE(void) switch_telnyx_process_flaws(switch_rtp_t* rtp_session, int penalty);
SWITCH_DECLARE(void) switch_telnyx_process_packet_loss(switch_rtp_t* rtp_session, int loss);
SWITCH_DECLARE(void) switch_telnyx_set_current_trace_message(const char* msg);
SWITCH_DECLARE(switch_call_cause_t) switch_telnyx_recompute_cause_code(switch_channel_t* channel, int status, switch_call_cause_t cause);
SWITCH_DECLARE(switch_bool_t) switch_telnyx_sip_cause_to_q850(int status, switch_call_cause_t* cause);
SWITCH_DECLARE(switch_bool_t) switch_telnyx_sip_on_media_timeout(switch_channel_t* channel, switch_rtp_t* rtp_session);
SWITCH_DECLARE(void) switch_telnyx_channel_event_set_basic_data(switch_channel_t *channel, switch_event_t *event);
SWITCH_DECLARE(switch_bool_t) switch_telnyx_on_add_media_bug(switch_media_bug_t **list, switch_media_bug_t *bug, const char* function, const char* target);

SWITCH_DECLARE(void *) switch_telnyx_sofia_find_nua(const char *profile_name, void **profile_handle);
SWITCH_DECLARE(void) switch_telnyx_sofia_release_profile(void *profile_handle);
SWITCH_DECLARE(switch_status_t) switch_telnyx_sofia_find_profile_url(const char *profile_name, int use_tls, char *buf, switch_size_t buflen);

SWITCH_DECLARE(switch_bool_t) switch_telnyx_sofia_register_handler(
	int event, int status, const char *phrase,
	void *nua, void *nh, void *hmagic,
	const void *sip, void *tags);

SWITCH_END_EXTERN_C

#endif /* SWITCH_TELNYX_H */

