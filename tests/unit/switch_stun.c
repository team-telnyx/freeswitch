#include <switch.h>
#include <switch_stun.h>
#include <test/switch_test.h>
#include <private/switch_rtp_pvt.h>

static const switch_payload_t TEST_PT = 8;
static const char *rx_host = "127.0.0.1";
static switch_port_t rx_port = 1234;
static const char *tx_host = "127.0.0.1";
static switch_port_t tx_port = 54320;

static void fsctl_debug(switch_core_session_t *session) 
{
		switch_stream_handle_t stream = { 0 };

		SWITCH_STANDARD_STREAM(stream);
		switch_api_execute("fsctl", "debug_level 9", session, &stream);
		switch_safe_free(stream.data);
}


FST_CORE_BEGIN("./conf_stun")
{
FST_SUITE_BEGIN(switch_stun)
{
FST_SETUP_BEGIN()
{
}
FST_SETUP_END()

FST_TEARDOWN_BEGIN()
{
}
FST_TEARDOWN_END()
	FST_TEST_BEGIN(test_binding_indication)
	{
		switch_stun_packet_t *packet;
		/*   STUN Indication:  A STUN message that does not receive a response. */
		static uint8_t incoming[] =
			"\x00\x11\x00\x08\x21\x12\xa4\x42\x36\xde\x03\x48\x25\xa7\x69\x43\x31\x18\x96\x90\x80\x28\x00\x04\x07\x2c\xa6\xb4";

		packet = switch_stun_packet_parse(incoming, sizeof(incoming) - 1);

		switch_assert(packet);
	}
	FST_TEST_END()
    FST_SESSION_BEGIN(test_stun_msg)
	{
        /* Binding Success Response */
        static uint8_t incoming[] = "\x01\x01\x00\x0c\x21\x12\xa4\x42\x53\x4f\x70\x43\x69\x69\x35\x4a\x66\x63\x31\x7a\x00\x01\x00\x08\x00\x01\x11\xfc\x0a\x0a\x0a\x2e";
        switch_core_session_t *session = NULL;
        switch_channel_t *channel = NULL;
        switch_status_t status;
        switch_call_cause_t cause;
        switch_rtp_ice_t ice = { 0 };
        switch_memory_pool_t *pool = NULL;
        static switch_rtp_t *rtp_session = NULL;
        static switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID] = {0};
        const char *err = NULL;

        switch_core_new_memory_pool(&pool);

        status = switch_ivr_originate(NULL, &session, &cause, "null/+15553334444", 2, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
        fst_check(session);
        fst_check(status == SWITCH_STATUS_SUCCESS);

        fsctl_debug(session);

        channel = switch_core_session_get_channel(session);
        fst_check(channel);

        switch_core_memory_pool_set_data(pool, "__session", session);
        session = switch_core_memory_pool_get_data(pool, "__session");
        fst_check(session);
        rtp_session = switch_rtp_new(rx_host, rx_port, tx_host, tx_port, TEST_PT, 8000, 20 * 1000, flags, "soft", &err, pool, 0, 0);
        fst_xcheck(rtp_session != NULL, "switch_rtp_new()");
        fst_check(switch_rtp_ready(rtp_session));
        switch_rtp_activate_rtcp(rtp_session, 5, rx_port + 1, 0);
        switch_rtp_set_default_payload(rtp_session, TEST_PT);
        switch_core_media_set_rtp_session(session, SWITCH_MEDIA_TYPE_AUDIO, rtp_session);

        ice.user_ice = "user_ice";
        ice.ice_user = "ice_user";

        ice.type = ICE_VANILLA;
        switch_rtp_pvt_handle_ice(rtp_session, &ice, incoming, sizeof(incoming));

        fst_check(ice.rready);
        switch_rtp_destroy(&rtp_session);
        switch_core_session_rwunlock(session);
        switch_core_destroy_memory_pool(&pool);
	}
	FST_SESSION_END()
}
FST_SUITE_END()
}
FST_CORE_END()
