#ifndef __SWITCH_RTP_PVT_H__
#define __SWITCH_RTP_PVT_H__

typedef enum {
	SWITCH_RTP_ICE_CONTROLLING_FAILOVER_IDLE = 0,
	SWITCH_RTP_ICE_CONTROLLING_FAILOVER_PROBING,
	SWITCH_RTP_ICE_CONTROLLING_FAILOVER_NOMINATING,
	SWITCH_RTP_ICE_CONTROLLING_STARTUP_NOMINATING
} switch_rtp_ice_controlling_failover_state_t;

typedef enum {
	SWITCH_RTP_ICE_CONTROLLING_TIMER_NONE = 0,
	SWITCH_RTP_ICE_CONTROLLING_TIMER_RETRANSMIT,
	SWITCH_RTP_ICE_CONTROLLING_TIMER_EXPIRE
} switch_rtp_ice_controlling_timer_action_t;

#define SWITCH_RTP_ICE_SELECTED_PAIR_CHECK_HISTORY 4

#define SWITCH_RTP_DTLS_IDENTITY_MAX_FINGERPRINTS 8
#define SWITCH_RTP_DTLS_IDENTITY_ALGORITHM_LEN 32
#define SWITCH_RTP_DTLS_IDENTITY_VALUE_LEN 192
#define SWITCH_RTP_DTLS_IDENTITY_TLS_ID_LEN 256
#define SWITCH_RTP_DTLS_IDENTITY_BUNDLE_LEN 512
#define SWITCH_RTP_DTLS_IDENTITY_MID_LEN 128

typedef struct {
	char algorithm[SWITCH_RTP_DTLS_IDENTITY_ALGORITHM_LEN];
	char value[SWITCH_RTP_DTLS_IDENTITY_VALUE_LEN];
} switch_rtp_pvt_dtls_fingerprint_t;

typedef struct {
	switch_bool_t valid;
	switch_bool_t rtcp_mux;
	switch_bool_t bundled;
	switch_bool_t owns_rtp;
	switch_bool_t tls_id_present;
	uint8_t media_type;
	uint8_t component;
	uint8_t remote_fingerprint_count;
	dtls_type_t local_role;
	char local_algorithm[SWITCH_RTP_DTLS_IDENTITY_ALGORITHM_LEN];
	char local_value[SWITCH_RTP_DTLS_IDENTITY_VALUE_LEN];
	char tls_id[SWITCH_RTP_DTLS_IDENTITY_TLS_ID_LEN];
	char bundle_group[SWITCH_RTP_DTLS_IDENTITY_BUNDLE_LEN];
	char bundle_tag[SWITCH_RTP_DTLS_IDENTITY_MID_LEN];
	char mid[SWITCH_RTP_DTLS_IDENTITY_MID_LEN];
	switch_rtp_pvt_dtls_fingerprint_t remote_fingerprints[SWITCH_RTP_DTLS_IDENTITY_MAX_FINGERPRINTS];
} switch_rtp_pvt_dtls_identity_t;

typedef struct {
		char *ice_user;
		char *user_ice;
		char *luser_ice;
		char *pass;
		char *rpass;
		switch_sockaddr_t *addr;
		uint32_t funny_stun;
		switch_time_t next_run;
		switch_core_media_ice_type_t type;
		ice_t *ice_params;
		ice_proto_t proto;
		uint8_t sending;
		uint8_t ready;
		uint8_t rready;
		uint8_t remote_eoc_received;
		uint8_t initializing;
		int missed_count;
		char last_sent_id[13];
		switch_time_t last_ok;
		uint8_t cand_responsive;
		uint8_t dtls_handshake;
		switch_time_t first_responsive_us;
		uint8_t promoted_to_controlling;
		uint8_t nomination_fallback_cached;
		uint8_t nomination_fallback_enabled;
		uint32_t nomination_fallback_ms;
		uint8_t mid_call_failover_cached;
		uint8_t mid_call_failover_enabled;
		uint32_t mid_call_failover_ms;
		int mid_call_nominated_idx;
		switch_time_t mid_call_nominated_us;
		uint8_t controlling_failover_cached;
		uint8_t controlling_failover_enabled;
		uint32_t controlling_failover_ms;
		switch_rtp_ice_controlling_failover_state_t controlling_failover_state;
		int controlling_failover_idx;
		switch_time_t controlling_failover_candidate_us;
		switch_time_t controlling_failover_started_us;
		switch_time_t controlling_failover_sent_us;
		uint8_t controlling_failover_attempts;
		char controlling_failover_probe_id[13];
		char controlling_failover_nomination_id[13];
		switch_socket_t *controlling_failover_socket;
		switch_port_t controlling_failover_local_port;
		int controlling_failover_local_family;
		char selected_pair_check_ids[SWITCH_RTP_ICE_SELECTED_PAIR_CHECK_HISTORY][13];
		uint8_t selected_pair_check_controlling[SWITCH_RTP_ICE_SELECTED_PAIR_CHECK_HISTORY];
		switch_sockaddr_t *selected_pair_check_remote_addr[SWITCH_RTP_ICE_SELECTED_PAIR_CHECK_HISTORY];
		uint8_t selected_pair_check_pos;
		uint8_t selected_pair_check_count;
		switch_socket_t *selected_pair_check_socket;
		switch_port_t selected_pair_check_local_port;
		int selected_pair_check_local_family;
		switch_time_t selected_pair_last_response_us;
		int inbound_media_idx;
		switch_time_t inbound_media_us;
		uint8_t prflx_bootstrap_cached;
		uint8_t prflx_bootstrap_enabled;
		uint8_t prflx_bootstrap_require_use_candidate;
		uint32_t prflx_bootstrap_ms;
		int prflx_bootstrap_idx;
		switch_time_t prflx_bootstrap_us;
		uint8_t restart_pending;
		uint8_t restart_provisional;
		switch_time_t restart_provisional_us;
} switch_rtp_ice_t;

typedef enum {
	SWITCH_RTP_RECOVERY_NOMINATION_NONE = 0,
	SWITCH_RTP_RECOVERY_NOMINATION_PERSISTED,
	SWITCH_RTP_RECOVERY_NOMINATION_AUTHENTICATED_CURRENT
} switch_rtp_recovery_nomination_proof_t;

typedef struct {
	switch_bool_t selected;
	switch_bool_t ready;
	const char *transport;
	switch_sockaddr_t *current_addr;
	switch_sockaddr_t *selected_addr;
} switch_rtp_pvt_ice_tuple_t;

typedef struct {
	switch_core_media_ice_type_t ice_type;
	switch_bool_t ice_ready;
	switch_bool_t ice_rready;
	switch_bool_t rtp_chosen;
	switch_bool_t rtcp_chosen;
	dtls_state_t dtls_state;
	const void *dtls_context;
	const void *dtls_ssl;
	const void *dtls_read_bio;
	const void *dtls_write_bio;
	switch_bool_t dtls_restart_pending;
	switch_bool_t dtls_restart_migrated;
	switch_bool_t dtls_tuple_authoritative;
	switch_bool_t srtp_send_ready;
	switch_bool_t srtp_recv_ready;
	switch_time_t dtls_association_started_us;
	switch_time_t dtls_restart_deadline_us;
	switch_time_t ice_selected_pair_last_response_us;
	switch_port_t ice_remote_port;
	switch_port_t rtp_remote_port;
	switch_port_t rtcp_remote_port;
	switch_port_t dtls_remote_port;
	const void *rtp_remote_addr;
	const void *dtls_remote_addr;
	uint32_t dtls_destroy_count;
	const void *socket;
} switch_rtp_pvt_transport_snapshot_t;

typedef enum {
	SWITCH_RTP_PVT_DTLS_RESTART_RESET = 0,
	SWITCH_RTP_PVT_DTLS_RESTART_PRESERVE,
	SWITCH_RTP_PVT_DTLS_RESTART_EXPIRED
} switch_rtp_pvt_dtls_restart_result_t;

SWITCH_DECLARE(void) switch_rtp_pvt_handle_ice(switch_rtp_t *rtp_session, switch_rtp_ice_t *ice, void *data, switch_size_t len);
SWITCH_DECLARE(switch_status_t) switch_rtp_pvt_handle_ice_from(switch_rtp_t *rtp_session, ice_proto_t proto,
	const char *host, switch_port_t port, void *data, switch_size_t len);
SWITCH_DECLARE(switch_status_t) switch_rtp_pvt_get_ice_state(switch_rtp_t *rtp_session, ice_proto_t proto,
	char *ice_user, switch_size_t ice_user_len, char *local_pwd, switch_size_t local_pwd_len,
	char *remote_pwd, switch_size_t remote_pwd_len, switch_bool_t *has_addr);
SWITCH_DECLARE(switch_status_t) switch_rtp_pvt_mark_ice_candidate_nominated(switch_rtp_t *rtp_session,
	ice_proto_t proto, const char *host, switch_port_t port);
SWITCH_DECLARE(switch_status_t) switch_rtp_pvt_send_ice_check(switch_rtp_t *rtp_session,
	ice_proto_t proto, char *transaction_id, switch_size_t transaction_id_len);
SWITCH_DECLARE(switch_status_t) switch_rtp_pvt_get_controlling_nomination_id(switch_rtp_t *rtp_session,
	ice_proto_t proto, char *transaction_id, switch_size_t transaction_id_len);
SWITCH_DECLARE(switch_status_t) switch_rtp_pvt_ice_timer_tick(switch_rtp_t *rtp_session,
	ice_proto_t proto, switch_time_t now);
SWITCH_DECLARE(switch_status_t) switch_rtp_pvt_get_transport_snapshot(switch_rtp_t *rtp_session,
	ice_proto_t proto, switch_rtp_pvt_transport_snapshot_t *snapshot);
SWITCH_DECLARE(switch_bool_t) switch_rtp_pvt_read_lock_held(switch_rtp_t *rtp_session);
typedef void (*switch_rtp_pvt_lock_test_hook_t)(void *data);
SWITCH_DECLARE(void) switch_rtp_pvt_set_lock_test_hooks(switch_rtp_t *rtp_session,
	switch_rtp_pvt_lock_test_hook_t write_locked_hook,
	switch_rtp_pvt_lock_test_hook_t rtcp_stun_dispatch_hook,
	switch_rtp_pvt_lock_test_hook_t dtls_destination_update_hook,
	switch_rtp_pvt_lock_test_hook_t rtcp_dtls_post_process_hook, void *data);
SWITCH_DECLARE(switch_status_t) switch_rtp_pvt_srtp_round_trip(switch_rtp_t *left, switch_rtp_t *right);
SWITCH_DECLARE(void) switch_rtp_pvt_set_dtls_association_identity(switch_rtp_t *rtp_session,
	dtls_type_t type, const switch_rtp_pvt_dtls_identity_t *identity);
SWITCH_DECLARE(switch_status_t) switch_rtp_pvt_get_dtls_association_identity(switch_rtp_t *rtp_session,
	dtls_type_t type, switch_rtp_pvt_dtls_identity_t *identity);
SWITCH_DECLARE(switch_bool_t) switch_rtp_pvt_dtls_association_identity_matches(switch_rtp_t *rtp_session,
	const switch_rtp_pvt_dtls_identity_t *identity);
SWITCH_DECLARE(switch_bool_t) switch_rtp_pvt_dtls_association_identity_compatible(switch_rtp_t *rtp_session,
	const switch_rtp_pvt_dtls_identity_t *identity);
SWITCH_DECLARE(switch_rtp_pvt_dtls_restart_result_t) switch_rtp_pvt_prepare_dtls_ice_restart(switch_rtp_t *rtp_session,
	const switch_rtp_pvt_dtls_identity_t *identity, uint32_t setup_timeout_ms,
	switch_core_media_ice_type_t ice_type, const char *remote_ufrag, const char *local_ufrag,
	const char *local_pwd, const char *remote_pwd);
SWITCH_DECLARE(switch_bool_t) switch_rtp_pvt_dtls_ice_restart_transport_authoritative(switch_rtp_t *rtp_session);
SWITCH_DECLARE(switch_status_t) switch_rtp_pvt_set_remote_sdp_address(switch_rtp_t *rtp_session,
	const char *host, switch_port_t port, switch_port_t remote_rtcp_port,
	switch_bool_t change_adv_addr, const char **err);
SWITCH_DECLARE(switch_status_t) switch_rtp_pvt_expire_prepared_dtls_ice_restart(switch_rtp_t *rtp_session);
SWITCH_DECLARE(switch_status_t) switch_rtp_pvt_dtls_step(switch_rtp_t *rtp_session, dtls_type_t type,
	const void *input, switch_size_t input_len, void *output, switch_size_t output_capacity,
	switch_size_t *output_len);
SWITCH_DECLARE(switch_status_t) switch_rtp_pvt_dtls_input_from(switch_rtp_t *rtp_session,
	dtls_type_t type, const char *host, switch_port_t port, const void *input, switch_size_t input_len);
SWITCH_DECLARE(switch_status_t) switch_rtp_pvt_dtls_timer_tick(switch_rtp_t *rtp_session, dtls_type_t type);
SWITCH_DECLARE(switch_bool_t) switch_rtp_pvt_should_preserve_active_dtls_tuple(switch_sockaddr_t *current_addr,
	switch_sockaddr_t *handshake_peer_addr, dtls_state_t dtls_state, switch_bool_t handshake_peer_set, switch_bool_t is_rtcp);
SWITCH_DECLARE(switch_bool_t) switch_rtp_pvt_ice_selection_complete(const ice_t *ice, switch_bool_t rtcp_muxed);
SWITCH_DECLARE(switch_bool_t) switch_rtp_pvt_should_preserve_trickle_dtls(switch_bool_t new_ice,
	switch_bool_t rtp_ready, const switch_rtp_pvt_ice_tuple_t *rtp_tuple,
	switch_bool_t rtcp_muxed, const char *active_remote_ufrag, const char *active_remote_pwd,
	const char *engine_remote_ufrag, const char *engine_remote_pwd,
	dtls_state_t dtls_state, switch_bool_t is_trickle_recheck);
SWITCH_DECLARE(switch_rtp_recovery_nomination_proof_t) switch_rtp_pvt_recovery_dtls_nomination_proof(
	switch_bool_t authenticated_vanilla_use_candidate, switch_bool_t direct_username_match);
SWITCH_DECLARE(switch_bool_t) switch_rtp_pvt_should_guard_recovery_dtls_tuple(switch_sockaddr_t *current_addr,
	switch_sockaddr_t *packet_addr, dtls_state_t dtls_state, switch_bool_t is_rtcp,
	switch_bool_t recovering, switch_rtp_recovery_nomination_proof_t nomination_proof);
SWITCH_DECLARE(switch_bool_t) switch_rtp_pvt_sync_authenticated_recovery_ice_addr(switch_sockaddr_t **ice_addr,
	switch_sockaddr_t *remote_addr, switch_sockaddr_t *nominated_addr, dtls_state_t dtls_state,
	switch_bool_t is_rtcp, switch_bool_t rtcp_mux, switch_bool_t recovering,
	switch_rtp_recovery_nomination_proof_t nomination_proof);
SWITCH_DECLARE(switch_bool_t) switch_rtp_pvt_restart_prflx_allowed(switch_rtp_ice_t *ice, dtls_state_t dtls_state,
	switch_bool_t provisional_ice, switch_bool_t direct_username_match, switch_bool_t stun_auth_valid,
	switch_bool_t got_message_integrity, switch_bool_t got_fingerprint, switch_bool_t got_use_candidate,
	switch_bool_t got_use_candidate_covered, switch_bool_t has_priority, switch_bool_t within_bootstrap_window,
	uint32_t bootstrap_ms, switch_time_t now);
SWITCH_DECLARE(switch_bool_t) switch_rtp_pvt_controlling_failover_response_matches(switch_rtp_ice_t *ice,
	switch_rtp_ice_controlling_failover_state_t expected_state, int candidate_idx, const char *transaction_id,
	switch_bool_t authenticated, switch_socket_t *local_socket, switch_port_t local_port, int local_family);
SWITCH_DECLARE(switch_rtp_ice_controlling_timer_action_t) switch_rtp_pvt_controlling_failover_timer_action(
	switch_rtp_ice_controlling_failover_state_t state, switch_time_t started_us, switch_time_t sent_us,
	uint8_t attempts, switch_time_t now);
SWITCH_DECLARE(switch_bool_t) switch_rtp_pvt_ice_role_conflict_response_matches(switch_rtp_ice_t *ice,
	switch_sockaddr_t *from_addr, int candidate_idx, const char *transaction_id, switch_bool_t authenticated,
	switch_socket_t *local_socket, switch_port_t local_port, int local_family, switch_bool_t *sent_controlling);
SWITCH_DECLARE(void) switch_rtp_pvt_ice_role_conflict_apply(switch_rtp_ice_t *ice, switch_bool_t sent_controlling);
SWITCH_DECLARE(void) switch_rtp_pvt_ice_role_conflict_cancel(switch_rtp_ice_t *ice);
SWITCH_DECLARE(void) switch_rtp_pvt_ice_role_conflict_rotate_tie_breaker(char tie_breaker[8]);
SWITCH_DECLARE(uint32_t) switch_rtp_pvt_ice_local_prflx_priority(switch_bool_t is_rtcp, switch_bool_t rtcp_mux);
SWITCH_DECLARE(int) switch_rtp_pvt_reuse_pending_startup_prflx_candidate(switch_rtp_t *rtp_session,
	switch_rtp_ice_t *ice, const char *host, switch_port_t port, uint32_t priority);

#endif /* __SWITCH_RTP_PVT_H__ */

/* For Emacs:
+ * Local Variables:
+ * mode:c
+ * indent-tabs-mode:t
+ * tab-width:4
+ * c-basic-offset:4
+ * End:
+ * For VIM:
+ * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
+ */
