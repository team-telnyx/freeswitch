/* Post-handshake DTLS final-flight recovery. Internal to switch_rtp.c. */
#ifndef SWITCH_DTLS_READY_H
#define SWITCH_DTLS_READY_H

#include <stddef.h>
#include <stdint.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#define SWITCH_DTLS_READY_MAX_PACKET 4096
#define SWITCH_DTLS_READY_MAX_RECORDS 8
#define SWITCH_DTLS_READY_INTERVAL_US 100000
/* OpenSSL's duplicate-Finished timeout counter fails on response 13. */
#define SWITCH_DTLS_READY_MAX_RESPONSES 12

typedef struct {
	uint64_t last_read_us;
	unsigned int reads;
	unsigned int responses;
	int disabled;
	int ssl_error;
} switch_dtls_ready_t;

/*
 * Retried client flights may coalesce epoch-0 handshake/CCS with Finished.
 * Only pass one complete epoch-1 handshake record to OpenSSL. In particular,
 * never queue stale plaintext flights, application data, or partial records
 * in the READY BIO. OpenSSL, not this header parser, authenticates Finished.
 */
static size_t switch_dtls_ready_record(const unsigned char *data, size_t len, size_t *offset)
{
	size_t pos = 0, size, found = 0;
	unsigned int count = 0, epoch, type;

	if (!data || len > SWITCH_DTLS_READY_MAX_PACKET) return 0;
	while (pos < len) {
		if (++count > SWITCH_DTLS_READY_MAX_RECORDS || len - pos < 13) return 0;
		type = data[pos];
		/* DTLS 1.0 and 1.2 record versions; cipher/version checks remain in SSL. */
		if (data[pos + 1] != 0xfe || (data[pos + 2] != 0xff && data[pos + 2] != 0xfd)) return 0;
		epoch = ((unsigned int)data[pos + 3] << 8) | data[pos + 4];
		size = 13 + ((size_t)data[pos + 11] << 8) + data[pos + 12];
		if (size > len - pos || size == 13) return 0;
		if (epoch == 1 && type == SSL3_RT_HANDSHAKE) {
			if (found) return 0;
			*offset = pos;
			found = size;
		} else if (epoch != 0 || (type != SSL3_RT_HANDSHAKE && type != SSL3_RT_CHANGE_CIPHER_SPEC)) {
			return 0;
		}
		pos += size;
	}
	return found;
}

/*
 * Return 1 when driven, 0 when ignored, -1 on the first terminal error.
 * Caller owns the SSL/BIOs and drains its usual packet-preserving output
 * filter even on WANT_READ. write_bio must be the underlying memory BIO:
 * querying FreeSWITCH's filter's PENDING would consume its packet metadata.
 * State and calls are serialized by the same RTP receive path as the handshake.
 */
static int switch_dtls_ready_receive(switch_dtls_ready_t *state, SSL *ssl,
	BIO *read_bio, BIO *write_bio, const void *packet, size_t len, uint64_t now_us)
{
	size_t offset = 0, size;
	size_t pending;
	int ret, error;
	unsigned char discard[1];

	if (state->disabled || state->responses >= SWITCH_DTLS_READY_MAX_RESPONSES) return 0;
	size = switch_dtls_ready_record((const unsigned char *)packet, len, &offset);
	if (!size) return 0;
	if (state->reads && now_us >= state->last_read_us &&
		now_us - state->last_read_us < SWITCH_DTLS_READY_INTERVAL_US) return 0;
	/* Do not enable renegotiation on older libraries without this control. */
#ifndef SSL_OP_NO_RENEGOTIATION
	return 0;
#else
	SSL_set_options(ssl, SSL_OP_NO_RENEGOTIATION);
#endif
	state->last_read_us = now_us;
	state->reads = 1;
	if (!SSL_is_init_finished(ssl)) {
		state->disabled = 1;
		(void)BIO_reset(read_bio);
		return -1;
	}
	/* SETUP may have queued a retry before switching to READY. Do not retain it. */
	if (BIO_ctrl_pending(read_bio)) (void)BIO_reset(read_bio);
	pending = BIO_ctrl_pending(write_bio);
	ERR_clear_error();
	ret = BIO_write(read_bio, (const unsigned char *)packet + offset, (int)size);
	if (ret != (int)size) {
		state->disabled = 1;
		(void)BIO_reset(read_bio);
		return -1;
	}
	/* One bounded encrypted record; AUTO_RETRY consumes non-app handshake data. */
	ret = SSL_read(ssl, discard, sizeof(discard));
	error = SSL_get_error(ssl, ret);
	if (BIO_ctrl_pending(write_bio) > pending) ++state->responses;
	if (ret <= 0 && (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) &&
		SSL_is_init_finished(ssl) && !BIO_ctrl_pending(read_bio)) return 1;

	/* Stop processing DTLS on terminal/unexpected input; keep existing SRTP. */
	state->disabled = 1;
	state->ssl_error = error;
	(void)BIO_reset(read_bio);
	return -1;
}

#endif
