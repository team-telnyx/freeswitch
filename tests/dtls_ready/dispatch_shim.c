/* Exact do_dtls orchestration with real SSL/BIOs and isolated runtime doubles. */
#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include "private/switch_dtls_ready.h"

typedef size_t switch_size_t;
typedef uint64_t switch_time_t;
typedef unsigned short switch_port_t;
typedef int switch_bool_t;
typedef void switch_channel_t;
typedef struct { int use_candidate; char *con_addr; int con_port; } icand_t;
typedef struct { int cand_idx[1]; icand_t cands[1][1]; } test_ice_params_t;
typedef struct {
	char *ice_user;
	int rready, ready, type, cand_responsive, proto;
	void *addr;
	test_ice_params_t *ice_params;
} test_ice_t;
typedef struct {
	int state, type, handshake_peer_set;
	int preserve_restart_pending, preserve_restart_migrated;
	size_t bytes;
	size_t last_flight_len;
	unsigned int last_flight_packet_count;
	switch_time_t restart_deadline_us, last_flight_sent_us;
	void *data, *remote_addr, *handshake_peer_addr, *restart_old_peer_addr, *sock_output;
	SSL *ssl;
	BIO *read_bio, *write_bio, *filter_bio;
	switch_dtls_ready_t ready_receive;
} switch_dtls_t;
typedef struct {
	test_ice_t ice;
	void *session, *from_addr, *pool;
	switch_dtls_t *dtls, *rtcp_dtls;
} switch_rtp_t;

enum { DS_OFF, DS_HANDSHAKE, DS_SETUP, DS_READY, DS_FAIL };
#define DTLS_TYPE_CLIENT 1
#define DTLS_TYPE_SERVER 2
#define SWITCH_FALSE 0
#define SWITCH_TRUE 1
#define SWITCH_STATUS_SUCCESS 0
#define SWITCH_UNSPEC 0
#define ICE_LITE 1
#define CF_RECOVERING 1
#define SWITCH_RTP_RECOVERY_NOMINATION_PERSISTED 1
#define SWITCH_RTP_RECOVERY_NOMINATION_NONE 0
#define MAX_DTLS_MTU 4096
#define SWITCH_CHANNEL_SESSION_LOG(s) (s)
#define SWITCH_LOG_ERROR 0
#define SWITCH_LOG_WARNING 0
#define SWITCH_LOG_DEBUG 0
#define SWITCH_LOG_DEBUG5 0
#define SWITCH_LOG_DEBUG6 0
#define switch_assert assert
#define zstr(s) (!(s) || !*(s))
#define rtp_type(s) "test"
#define switch_core_session_get_channel(s) (s)
#define switch_channel_test_flag(s, f) ((void)(s), (void)(f), 0)
#define switch_cmp_addr(a, b, f) ((a) == (b))
#define switch_sockaddr_get_port(a) 0
#define switch_get_addr(b, n, a) ((void)(b), (void)(n), (void)(a), (char *)"127.0.0.1")
#define switch_sockaddr_info_get(a, h, u, p, f, pool) 0
#define switch_rtp_pvt_should_guard_recovery_dtls_tuple(a, b, s, r, c, n) \
	((void)(a), (void)(b), (void)(s), (void)(r), (void)(c), (void)(n), 0)
#define switch_rtp_change_ice_dest(s, i, h, p) ((void)0)
#define switch_mono_micro_time_now() 1000000
#define switch_micro_time_now() 1000000
#define dtls_set_state(d, s) ((d)->state = (s))

static unsigned char sent[MAX_DTLS_MTU];
static size_t sent_len;
static int setup_calls, fail_setup, result_state, result_disabled;
static unsigned int result_responses;

static void switch_log_printf(void *session, int level, const char *format, ...)
{
	(void)session; (void)level; (void)format;
}

static int switch_socket_sendto(void *socket, void *address, int flags, void *data, size_t *len)
{
	(void)socket; (void)address; (void)flags;
	assert(sent_len + *len <= sizeof(sent));
	memcpy(sent + sent_len, data, *len);
	sent_len += *len;
	return SWITCH_STATUS_SUCCESS;
}

static int dtls_state_setup(switch_rtp_t *rtp, switch_dtls_t *dtls)
{
	(void)rtp;
	++setup_calls;
	dtls->state = fail_setup ? DS_FAIL : DS_READY;
	return fail_setup ? -1 : 0;
}

static int dtls_state_fail(switch_rtp_t *rtp, switch_dtls_t *dtls)
{
	(void)rtp; (void)dtls;
	return -1;
}

static int dtls_update_nominated_destination(switch_rtp_t *rtp, switch_dtls_t *dtls,
	const char *host, switch_port_t port)
{
	(void)rtp; (void)dtls; (void)host; (void)port;
	return 1;
}

static int dtls_peer_packet_is_duplicate(switch_dtls_t *dtls, const void *packet,
	switch_size_t packet_len)
{
	(void)dtls; (void)packet; (void)packet_len;
	return 0;
}

static int dtls_replay_last_flight(switch_rtp_t *rtp, switch_dtls_t *dtls)
{
	(void)rtp; (void)dtls;
	return 0;
}

static switch_bool_t dtls_capture_flight_packet(switch_rtp_t *rtp, switch_dtls_t *dtls,
	const void *packet, switch_size_t packet_len, switch_bool_t first_packet)
{
	(void)rtp; (void)dtls; (void)packet; (void)packet_len; (void)first_packet;
	return SWITCH_TRUE;
}

static switch_bool_t dtls_capture_peer_packet(switch_rtp_t *rtp, switch_dtls_t *dtls,
	const void *packet, switch_size_t packet_len)
{
	(void)rtp; (void)dtls; (void)packet; (void)packet_len;
	return SWITCH_TRUE;
}

static int test_handshake(switch_rtp_t *rtp, switch_dtls_t *dtls)
{
	(void)rtp;
	(void)SSL_do_handshake(dtls->ssl);
	if (SSL_is_init_finished(dtls->ssl)) dtls->state = DS_SETUP;
	return 0;
}

static int test_idle(switch_rtp_t *rtp, switch_dtls_t *dtls)
{
	(void)rtp; (void)dtls;
	return 0;
}

static int (*dtls_states[])(switch_rtp_t *, switch_dtls_t *) = {
	NULL, test_handshake, dtls_state_setup, test_idle, test_idle
};

/* Generated from the current switch_rtp.c, not a copy of the dispatch logic. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "do_dtls.inc"
#pragma GCC diagnostic pop

int dispatch_receive(SSL *ssl, BIO *in, BIO *out, const void *packet, size_t len,
	int state, int server, int failure, int reject)
{
	switch_rtp_t rtp;
	switch_dtls_t dtls;
	int ret;
	memset(&rtp, 0, sizeof(rtp));
	memset(&dtls, 0, sizeof(dtls));
	dtls.ssl = ssl; dtls.read_bio = in; dtls.write_bio = out; dtls.filter_bio = out;
	dtls.state = state; dtls.type = server ? DTLS_TYPE_SERVER : DTLS_TYPE_CLIENT;
	dtls.data = (void *)packet; dtls.bytes = len;
	rtp.dtls = &dtls;
	if (reject) rtp.ice.ice_user = (char *)"test";
	sent_len = 0; setup_calls = 0; fail_setup = failure;
	ret = do_dtls(&rtp, &dtls);
	result_state = dtls.state;
	result_disabled = dtls.ready_receive.disabled;
	result_responses = dtls.ready_receive.responses;
	return ret;
}

size_t dispatch_output(void *buffer, size_t capacity)
{
	assert(capacity >= sent_len);
	memcpy(buffer, sent, sent_len);
	return sent_len;
}
int dispatch_setup_calls(void) { return setup_calls; }
int dispatch_state(void) { return result_state; }
int dispatch_disabled(void) { return result_disabled; }
unsigned int dispatch_responses(void) { return result_responses; }
