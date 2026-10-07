# DTLS READY final-flight regression tests

These tests compile `ready_shim.c` against the **same private helper used by
`switch_rtp.c`**, then drive real OpenSSL DTLS 1.2 peers through memory BIOs.
They do not start or modify a FreeSWITCH service. Temporary keys and binaries
are deleted on exit. Python 3, a C compiler, the OpenSSL CLI, headers and shared
libraries are required. No network sockets are used by the peers.

From a configured build tree:

```sh
make check-dtls-ready
```

Or without configuring FreeSWITCH:

```sh
OPENSSL_CFLAGS="$(pkg-config --cflags openssl)" \
OPENSSL_LIBS="$(pkg-config --libs openssl)" \
python3 tests/dtls_ready/test_ready.py
```

When selecting a non-system OpenSSL, set `OPENSSL_LIB_DIR` to the same library
directory used in `OPENSSL_LIBS`. The directory must contain `libssl.so` and
`libcrypto.so` (or their `.dylib` equivalents on macOS). If unset, the platform
loader discovers the versioned libraries. The C helper is compiled with
`-std=gnu89 -Wall -Wextra -Werror -Werror=declaration-after-statement`.

## Coverage

- Successful no-loss control; loss of the first server final flight.
- Session tickets on/off; coalesced and split client final-flight records.
- Original Finished content, exporter/SRTP keys and SSL association preserved;
  client handshake completes without renegotiation.
- Bad authentication, exact replay, malformed/truncated/oversized input,
  excessive records, wrong epoch/version, and application-data rejection.
- Rate-limited traffic is not queued; a retry at the next allowed interval
  still recovers. A thousand-packet burst cannot trigger extra processing.
- Twelve authenticated retries can elicit twelve responses; further input
  does not enter OpenSSL's thirteenth-retry fatal path.
- Input queued during SETUP does not disable READY recovery. Terminal SSL
  results disable post-handshake processing once, without recreating the
  association or changing the established keys.
- An authenticated renegotiation request does not start a new handshake.

The optional SRTP test also creates the server's SRTP state **before recovery**
and checks encrypted/authenticated RTP and RTCP in both directions afterward.
Enable it with `SRTP_LIBS`, pointing to the libsrtp library built from the bundled
headers, or a matching `libfreeswitch` that exports those symbols. For example:

```sh
SRTP_LIBS="/usr/local/freeswitch/lib/libfreeswitch.so.1 -Wl,-rpath,/usr/local/freeswitch/lib" \
OPENSSL_CFLAGS="$(pkg-config --cflags openssl)" \
OPENSSL_LIBS="$(pkg-config --libs openssl)" \
python3 tests/dtls_ready/test_ready.py
```

Without `SRTP_LIBS` that one test is explicitly skipped; all DTLS tests still
run. No exporter/key bytes are printed.

## Receive policy and integration

The server READY receive branch runs after existing peer/ICE admission checks.
It accepts packets up to 4096 bytes and eight records, extracts exactly one
epoch-1 encrypted handshake record, and lets OpenSSL authenticate it. Other
records cannot accumulate in the READY input BIO. It performs at most one
`SSL_read()` per 100 ms per DTLS association, with at most twelve output-producing
calls over that association's lifetime. Invalid/replayed input does not spend
the response budget, but does spend the rate slot if it reaches OpenSSL.

`SSL_OP_NO_RENEGOTIATION` is set before post-handshake reads. Libraries without
that option retain the previous no-processing behavior. The helper has been
tested with OpenSSL 3.0.20 and 3.6.4.

WANT_READ/WANT_WRITE are nonfatal. The existing packet-preserving output drain
runs afterward, including when SSL_read returns WANT_READ while emitting a
retransmission. Terminal/unexpected results disable only subsequent READY DTLS
reads and are logged once; existing SRTP state is retained. The helper never
recreates SSL state, installs keys, changes ICE nomination or changes a peer
tuple. Each `switch_dtls_t` owns its own rate/budget state.

These are library/helper regression tests, not a full patched-service or
browser/ICE/TURN test. Integration validation with a patched FreeSWITCH build
should repeat the loss-relay packet-capture test and verify bidirectional
SRTP/SRTCP with muxed and separate RTCP before deployment. Retransmitted DTLS
record sequence numbers change; compare Finished content and key continuity,
not byte-identical UDP datagrams.
