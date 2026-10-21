/* Exercise the exact private helper compiled into switch_rtp.c. */
#include <stdlib.h>
#include "private/switch_dtls_ready.h"

void *ready_new(void) { return calloc(1, sizeof(switch_dtls_ready_t)); }
void ready_free(void *state) { free(state); }
int ready_receive(void *state, SSL *ssl, BIO *in, BIO *out, const void *packet, size_t len, uint64_t now)
{
	return switch_dtls_ready_receive(state, ssl, in, out, packet, len, now);
}
unsigned int ready_responses(void *state) { return ((switch_dtls_ready_t *)state)->responses; }
int ready_disabled(void *state) { return ((switch_dtls_ready_t *)state)->disabled; }
size_t ready_record(const unsigned char *data, size_t len, size_t *offset)
{
	return switch_dtls_ready_record(data, len, offset);
}

#ifdef TEST_SRTP
#include <string.h>
#include <srtp.h>
typedef struct {
	srtp_t tx;
	srtp_t rx;
} ready_srtp_pair_t;

void *ready_srtp_new(const unsigned char *exported, int server)
{
	ready_srtp_pair_t *pair;
	srtp_policy_t policy;
	unsigned char key[30];
	int i, side;
	static int initialized = 0;

	if (!initialized) {
		if (srtp_init() != srtp_err_status_ok) return NULL;
		initialized = 1;
	}
	pair = calloc(1, sizeof(*pair));
	if (!pair) return NULL;
	for (i = 0; i < 2; ++i) {
		side = i ? !server : server;
		memcpy(key, exported + side * 16, 16);
		memcpy(key + 16, exported + 32 + side * 14, 14);
		memset(&policy, 0, sizeof(policy));
		srtp_crypto_policy_set_rtp_default(&policy.rtp);
		srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
		policy.ssrc.type = i ? ssrc_any_inbound : ssrc_any_outbound;
		policy.key = key;
		policy.window_size = 128;
		if (srtp_create(i ? &pair->rx : &pair->tx, &policy) != srtp_err_status_ok) {
			if (pair->tx) srtp_dealloc(pair->tx);
			free(pair);
			return NULL;
		}
	}
	return pair;
}

void ready_srtp_free(void *value)
{
	ready_srtp_pair_t *pair = value;
	if (!pair) return;
	srtp_dealloc(pair->tx);
	srtp_dealloc(pair->rx);
	free(pair);
}

int ready_srtp_exchange(void *sender, void *receiver, int rtcp)
{
	ready_srtp_pair_t *from = sender, *to = receiver;
	unsigned char packet[128], plain[32];
	int len = sizeof(plain);
	srtp_err_status_t status;

	memset(plain, 0x21, sizeof(plain));
	plain[0] = rtcp ? 0x81 : 0x80;
	plain[1] = rtcp ? 201 : 0;
	plain[2] = 0;
	plain[3] = rtcp ? 7 : 1;
	memcpy(packet, plain, sizeof(plain));
	status = rtcp ? srtp_protect_rtcp(from->tx, packet, &len) : srtp_protect(from->tx, packet, &len);
	if (status != srtp_err_status_ok || len <= (int)sizeof(plain)) return 0;
	status = rtcp ? srtp_unprotect_rtcp(to->rx, packet, &len) : srtp_unprotect(to->rx, packet, &len);
	return status == srtp_err_status_ok && len == (int)sizeof(plain) && !memcmp(plain, packet, sizeof(plain));
}
#endif
