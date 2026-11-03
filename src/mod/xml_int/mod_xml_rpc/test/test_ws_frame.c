/*
 * Standalone unit test for ws_read_frame() in ws.c.
 *
 * Includes ws.c directly and stubs the abyss connection calls it uses, so
 * the bytes a client sends can be delivered in chosen read chunks.
 */

#include "../ws.c"

#include <assert.h>

/* ── abyss stubs ─────────────────────────────────────────────────────────── */

#define MAX_CHUNKS 64

static struct {
	const uint8_t *data[MAX_CHUNKS];
	size_t len[MAX_CHUNKS];
	int count;
	int next;
} chunks;

static uint8_t written[4096];
static size_t written_len;

void ConnRead(TConn *const connectionP, uint32_t const timeout, bool *const eofP,
			  bool *const timedOutP, const char **const errorP)
{
	(void)timeout; (void)eofP; (void)timedOutP;

	if (chunks.next >= chunks.count) {
		*errorP = strdup("no more data");
		return;
	}

	assert(connectionP->buffersize + chunks.len[chunks.next] <= BUFFER_SIZE);
	memcpy(connectionP->buffer.b + connectionP->buffersize, chunks.data[chunks.next], chunks.len[chunks.next]);
	connectionP->buffersize += (uint32_t)chunks.len[chunks.next];
	chunks.next++;
	*errorP = NULL;
}

void ConnReadInit(TConn *const connectionP)
{
	if (connectionP->buffersize > connectionP->bufferpos) {
		connectionP->buffersize -= connectionP->bufferpos;
		memmove(connectionP->buffer.b, connectionP->buffer.b + connectionP->bufferpos, connectionP->buffersize);
		connectionP->bufferpos = 0;
	} else {
		connectionP->buffersize = connectionP->bufferpos = 0;
	}
}

bool ConnWrite(TConn *const connectionP, const void *const buffer, uint32_t const size)
{
	(void)connectionP;
	assert(written_len + size <= sizeof(written));
	memcpy(written + written_len, buffer, size);
	written_len += size;
	return true;
}

/* ── helpers ─────────────────────────────────────────────────────────────── */

static TConn conn;
static TSession session;
static wsh_t wsh;

static void reset(void)
{
	memset(&chunks, 0, sizeof(chunks));
	memset(&conn, 0, sizeof(conn));
	memset(&session, 0, sizeof(session));
	memset(&wsh, 0, sizeof(wsh));
	written_len = 0;
	session.connP = &conn;
	wsh.tsession = &session;
	wsh.buflen = sizeof(wsh.buffer);
	wsh.handshake = 1;
}

static void add_chunk(const uint8_t *data, size_t len)
{
	assert(chunks.count < MAX_CHUNKS);
	chunks.data[chunks.count] = data;
	chunks.len[chunks.count] = len;
	chunks.count++;
}

/* Append a masked client frame to out, return its length */
static size_t client_frame(uint8_t *out, ws_opcode_t oc, const char *payload, size_t plen)
{
	static const uint8_t mask[4] = { 0x11, 0x22, 0x33, 0x44 };
	size_t n = 0, i;

	out[n++] = (uint8_t)(0x80 | oc);
	if (plen < 126) {
		out[n++] = (uint8_t)(0x80 | plen);
	} else {
		out[n++] = 0x80 | 126;
		out[n++] = (uint8_t)(plen >> 8);
		out[n++] = (uint8_t)plen;
	}
	memcpy(out + n, mask, 4);
	n += 4;
	for (i = 0; i < plen; i++) {
		out[n++] = (uint8_t)payload[i] ^ mask[i % 4];
	}

	return n;
}

static void expect_frame(ws_opcode_t want_oc, const char *want)
{
	ws_opcode_t oc;
	uint8_t *data;
	issize_t r = ws_read_frame(&wsh, &oc, &data);

	if (r != (issize_t)strlen(want) || oc != want_oc || memcmp(data, want, strlen(want))) {
		fprintf(stderr, "FAIL: wanted opcode %d \"%s\", got %d opcode %d\n", want_oc, want, r, oc);
		exit(1);
	}
}

/* ── tests ───────────────────────────────────────────────────────────────── */

static void test_back_to_back_short_frames(void)
{
	uint8_t buf[64];
	size_t n = 0;

	reset();
	n += client_frame(buf + n, WSOC_TEXT, "a", 1);
	n += client_frame(buf + n, WSOC_TEXT, "b", 1);
	add_chunk(buf, n);

	expect_frame(WSOC_TEXT, "a");
	expect_frame(WSOC_TEXT, "b");
	assert(!wsh.down);
	printf("PASS test_back_to_back_short_frames\n");
}

static void test_ping_then_text(void)
{
	uint8_t buf[64];
	size_t n = 0;

	reset();
	n += client_frame(buf + n, WSOC_PING, "p", 1);
	n += client_frame(buf + n, WSOC_TEXT, "hi", 2);
	add_chunk(buf, n);

	expect_frame(WSOC_TEXT, "hi");
	assert(written_len == 3 && written[0] == (0x80 | WSOC_PONG) && written[1] == 1 && written[2] == 'p');
	printf("PASS test_ping_then_text\n");
}

static void test_extended_length_then_short_frame(void)
{
	static uint8_t buf[512];
	char big[300];
	size_t n = 0;

	reset();
	memset(big, 'x', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	n += client_frame(buf + n, WSOC_TEXT, big, strlen(big));
	n += client_frame(buf + n, WSOC_TEXT, "c", 1);
	add_chunk(buf, n);

	expect_frame(WSOC_TEXT, big);
	expect_frame(WSOC_TEXT, "c");
	printf("PASS test_extended_length_then_short_frame\n");
}

static void test_frame_one_byte_per_read(void)
{
	uint8_t buf[64];
	size_t n = 0, i;

	reset();
	n += client_frame(buf + n, WSOC_TEXT, "hello", 5);
	for (i = 0; i < n; i++) {
		add_chunk(buf + i, 1);
	}

	expect_frame(WSOC_TEXT, "hello");
	printf("PASS test_frame_one_byte_per_read\n");
}

static void test_truncated_header(void)
{
	uint8_t buf[64];
	ws_opcode_t oc;
	uint8_t *data;
	size_t n;

	reset();
	n = client_frame(buf, WSOC_TEXT, "hello", 5);
	add_chunk(buf, 4);	/* stops inside the mask */
	(void)n;

	assert(ws_read_frame(&wsh, &oc, &data) == -WS_PROTO_ERR);
	assert(oc == WSOC_CLOSE && wsh.down);
	printf("PASS test_truncated_header\n");
}

int main(void)
{
	test_back_to_back_short_frames();
	test_ping_then_text();
	test_extended_length_then_short_frame();
	test_frame_one_byte_per_read();
	test_truncated_header();
	return 0;
}
