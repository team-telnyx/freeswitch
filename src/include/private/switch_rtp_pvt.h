#ifndef __SWITCH_RTP_PVT_H__
#define __SWITCH_RTP_PVT_H__

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
		uint8_t prflx_bootstrap_cached;
		uint8_t prflx_bootstrap_enabled;
		uint8_t prflx_bootstrap_require_use_candidate;
		uint32_t prflx_bootstrap_ms;
		int prflx_bootstrap_idx;
		switch_time_t prflx_bootstrap_us;
} switch_rtp_ice_t;

SWITCH_DECLARE(void) switch_rtp_pvt_handle_ice(switch_rtp_t *rtp_session, switch_rtp_ice_t *ice, void *data, switch_size_t len);

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
