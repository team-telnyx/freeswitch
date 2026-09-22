/*
 * Regression test for a codec's concealment path tearing down a transcoding
 * bridge.
 *
 * Production symptom
 * ------------------
 * A G722 caller bridged to a G729 callee answers, no RTP flows on the B-leg,
 * and the B2BUA sends BYE within ~40 ms:
 *
 *   [DEBUG]   mod_bcg729.c:206        g729 zero length frame
 *   [WARNING] switch_core_media.c     resample output 16384 exceeds buffer 8192
 *                                     (to=16000 from=8000 datalen=8192 ch=1)
 *   [ERR]     switch_core_media.c     Encoded write frame datalen 16384 or 8192
 *                                     greater than recommended size 8192
 *   [DEBUG]   switch_ivr_bridge.c     ending bridge by request from write function
 *
 * Root cause (in the codec module, fixed separately in team-telnyx/mod_bcg729)
 * ---------------------------------------------------------------------------
 * With G.729 Annex B negotiated, the B-leg's re-encode emits untransmitted
 * frames, so a zero-length frame reaches the G729 decoder. Its PLC branch
 * returned SWITCH_STATUS_SUCCESS without writing through decoded_data_len - it
 * assigned the local POINTER, "decoded_data_len = (uint32_t *) 160;".
 *
 * Why that is fatal here
 * ----------------------
 * switch_core_session_write_frame() pre-sets its decoded length to the size of
 * the raw write buffer before decoding:
 *
 *      session->raw_write_frame.datalen = session->raw_write_frame.buflen;   // 8192
 *      status = switch_core_codec_decode(frame->codec, ..., &session->raw_write_frame.datalen, ...);
 *
 * so a decoder that never writes that field leaves 8192 in place, and the core
 * carries it on as 8192 bytes of valid PCM. Writing into a 16 kHz far leg then
 * resamples 8000 -> 16000, doubling it to 16384, which trips the encode-size
 * guard; the write fails and switch_ivr_bridge ends the bridge.
 *
 * What this test pins down
 * ------------------------
 * The core contract, not mod_bcg729: no codec module should be able to end a
 * call by mis-reporting a concealment length. switch_core_codec_decode() now
 * rejects a zero-length decode that claims more than one packet of PCM and
 * returns SWITCH_STATUS_BREAK, which both the read and write paths already
 * handle as "no audio this tick".
 *
 * It installs a stub codec whose decode callback reproduces the mod_bcg729 PLC
 * branch verbatim (returns SUCCESS, never touches *decoded_data_len), bridges
 * it into a 16 kHz write path, and checks:
 *
 *   1. switch_core_codec_decode() clamps it and reports BREAK.
 *   2. A well-behaved concealment decoder is still passed through untouched.
 *   3. switch_core_session_write_frame() survives the frame.
 *
 * Expected:
 *   - BUGGY tree : decode returns SUCCESS with 8192, and write_frame() returns
 *                  SWITCH_STATUS_FALSE - the bridge teardown.
 *   - FIXED tree : decode returns BREAK with 0, write_frame() returns SUCCESS.
 */

#include <switch.h>
#include <test/switch_test.h>

/* The negotiated G729 shape on the failing calls: 8 kHz, 20 ms. */
#define STUB_RATE 8000
#define STUB_PTIME_MS 20
#define STUB_DECODED_BYTES_PER_PACKET 320	/* 160 samples * 2 bytes */
#define STUB_ENCODED_BYTES_PER_PACKET 20	/* 2 x 10 byte G.729 frames */
/* A codec_id the core will not mistake for the far leg's, so write_frame() takes
 * the transcode path rather than the ptime-mismatch one (as G729 vs G722 does). */
#define STUB_CODEC_ID 0x515

static int stub_decode_calls = 0;

/*
 * The defect, reproduced verbatim: concealment that returns SUCCESS without
 * writing the produced length through the caller's pointer.
 */
static switch_status_t stub_decode_leaky_plc(switch_codec_t *codec, switch_codec_t *other_codec,
											 void *encoded_data, uint32_t encoded_data_len, uint32_t encoded_rate,
											 void *decoded_data, uint32_t *decoded_data_len, uint32_t *decoded_rate,
											 unsigned int *flag)
{
	stub_decode_calls++;

	if (encoded_data_len == 0) {
		/* mod_bcg729.c:205 did "decoded_data_len = (uint32_t *) 160;" here,
		 * which assigns the local pointer. The caller's preset length survives,
		 * so leaving it alone models the bug exactly. Only one 10 ms frame of
		 * PCM is actually produced. */
		memset(decoded_data, 0, 160);
		return SWITCH_STATUS_SUCCESS;
	}

	*decoded_data_len = (encoded_data_len / 10) * 160;
	memset(decoded_data, 0, *decoded_data_len);
	return SWITCH_STATUS_SUCCESS;
}

/* The same path done right: conceal one packet and say so. */
static switch_status_t stub_decode_correct_plc(switch_codec_t *codec, switch_codec_t *other_codec,
											   void *encoded_data, uint32_t encoded_data_len, uint32_t encoded_rate,
											   void *decoded_data, uint32_t *decoded_data_len, uint32_t *decoded_rate,
											   unsigned int *flag)
{
	stub_decode_calls++;

	if (encoded_data_len == 0) {
		*decoded_data_len = STUB_DECODED_BYTES_PER_PACKET;
		memset(decoded_data, 0, *decoded_data_len);
		return SWITCH_STATUS_SUCCESS;
	}

	*decoded_data_len = (encoded_data_len / 10) * 160;
	memset(decoded_data, 0, *decoded_data_len);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t stub_encode_noop(switch_codec_t *codec, switch_codec_t *other_codec,
										void *decoded_data, uint32_t decoded_data_len, uint32_t decoded_rate,
										void *encoded_data, uint32_t *encoded_data_len, uint32_t *encoded_rate,
										unsigned int *flag)
{
	*encoded_data_len = (decoded_data_len / 160) * 10;
	memset(encoded_data, 0, *encoded_data_len);
	return SWITCH_STATUS_SUCCESS;
}

/*
 * Stand up a codec that looks like the negotiated G729 to the core. A real
 * codec is initialised first so the handle has its pool, mutex and ready flags,
 * then its implementation is swapped for one carrying the stub callbacks - the
 * unit-test tree has no G729 module to load.
 */
static switch_status_t stub_codec_open(switch_codec_t *codec, switch_codec_implementation_t *impl,
									   switch_core_codec_decode_func_t decode, switch_memory_pool_t *pool)
{
	switch_status_t status;

	status = switch_core_codec_init(codec, "L16", NULL, NULL, STUB_RATE, STUB_PTIME_MS, 1,
									SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL, pool);
	if (status != SWITCH_STATUS_SUCCESS) {
		return status;
	}

	*impl = *codec->implementation;
	impl->iananame = "G729stub";
	impl->ianacode = 18;
	impl->codec_id = STUB_CODEC_ID;
	impl->samples_per_second = STUB_RATE;
	impl->actual_samples_per_second = STUB_RATE;
	impl->microseconds_per_packet = STUB_PTIME_MS * 1000;
	impl->samples_per_packet = STUB_DECODED_BYTES_PER_PACKET / 2;
	impl->decoded_bytes_per_packet = STUB_DECODED_BYTES_PER_PACKET;
	impl->encoded_bytes_per_packet = STUB_ENCODED_BYTES_PER_PACKET;
	impl->number_of_channels = 1;
	impl->decode = decode;
	impl->encode = stub_encode_noop;
	impl->next = NULL;

	codec->implementation = impl;
	return SWITCH_STATUS_SUCCESS;
}

/* Originate one answered "null" session. Returns it rwlocked (caller unlocks). */
static switch_core_session_t *originate_null_session(void)
{
	switch_core_session_t *session = NULL;
	switch_call_cause_t cause = SWITCH_CAUSE_NONE;
	switch_status_t status;

	status = switch_ivr_originate(NULL, &session, &cause,
								  "null/+15553334444",
								  0, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);

	if (status != SWITCH_STATUS_SUCCESS || !session) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
						  "[TEST] originate failed: status=%d cause=%d\n", status, cause);
		return NULL;
	}
	return session;
}

FST_CORE_BEGIN("./conf")
{
	FST_SUITE_BEGIN(g729_zero_length_plc)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_loopback");
			stub_decode_calls = 0;
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		/*
		 * The contract, checked directly: given no encoded bytes, a decoder
		 * cannot have produced more than one packet of concealment, so a larger
		 * reported length is the caller's own preset value leaking through.
		 */
		FST_TEST_BEGIN(test_zero_length_decode_cannot_claim_the_callers_buffer)
		{
			switch_codec_t codec = { 0 };
			switch_codec_implementation_t impl = { 0 };
			uint8_t encoded[STUB_ENCODED_BYTES_PER_PACKET] = { 0 };
			uint8_t decoded[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t decoded_len;
			uint32_t rate = STUB_RATE;
			unsigned int flag = 0;
			switch_status_t status;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "========== ZERO-LENGTH DECODE CONTRACT ==========\n");

			fst_requires(stub_codec_open(&codec, &impl, stub_decode_leaky_plc, fst_pool) == SWITCH_STATUS_SUCCESS);

			/* Exactly what switch_core_session_write_frame() does before decoding. */
			decoded_len = sizeof(decoded);
			status = switch_core_codec_decode(&codec, NULL, encoded, 0, STUB_RATE,
											  decoded, &decoded_len, &rate, &flag);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] leaky PLC decode -> status=%d datalen=%u\n", status, decoded_len);

			fst_check(stub_decode_calls == 1);
			/* Buggy tree: SUCCESS with 8192. Fixed tree: BREAK with 0. */
			fst_check(status == SWITCH_STATUS_BREAK);
			fst_check(decoded_len <= STUB_DECODED_BYTES_PER_PACKET);

			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/* A decoder that conceals one packet and reports it must be left alone. */
		FST_TEST_BEGIN(test_well_behaved_concealment_is_not_clamped)
		{
			switch_codec_t codec = { 0 };
			switch_codec_implementation_t impl = { 0 };
			uint8_t encoded[STUB_ENCODED_BYTES_PER_PACKET] = { 0 };
			uint8_t decoded[SWITCH_RECOMMENDED_BUFFER_SIZE];
			uint32_t decoded_len;
			uint32_t rate = STUB_RATE;
			unsigned int flag = 0;
			switch_status_t status;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "========== WELL-BEHAVED CONCEALMENT PASSES THROUGH ==========\n");

			fst_requires(stub_codec_open(&codec, &impl, stub_decode_correct_plc, fst_pool) == SWITCH_STATUS_SUCCESS);

			decoded_len = sizeof(decoded);
			status = switch_core_codec_decode(&codec, NULL, encoded, 0, STUB_RATE,
											  decoded, &decoded_len, &rate, &flag);

			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check(decoded_len == STUB_DECODED_BYTES_PER_PACKET);

			/* And a normal payload is untouched either way. */
			decoded_len = sizeof(decoded);
			status = switch_core_codec_decode(&codec, NULL, encoded, sizeof(encoded), STUB_RATE,
											  decoded, &decoded_len, &rate, &flag);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check(decoded_len == STUB_DECODED_BYTES_PER_PACKET);

			switch_core_codec_destroy(&codec);
		}
		FST_TEST_END()

		/*
		 * The production path end to end: the zero-length frame arrives on a
		 * bridge whose far leg runs at 16 kHz, so the write resampler doubles
		 * whatever the decoder reported. write_frame() must not fail - a failed
		 * write is what switch_ivr_bridge turns into "ending bridge by request
		 * from write function".
		 */
		FST_TEST_BEGIN(test_zero_length_frame_does_not_end_the_bridge)
		{
			switch_core_session_t *session = NULL;
			switch_channel_t *channel = NULL;
			switch_codec_t far_codec = { 0 };	/* the 16 kHz leg, standing in for G722 */
			switch_codec_t stub_codec = { 0 };	/* the 8 kHz leg, standing in for G729 */
			switch_codec_implementation_t impl = { 0 };
			switch_frame_t frame = { 0 };
			switch_status_t status;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "========== ZERO-LENGTH FRAME ACROSS A RESAMPLING BRIDGE ==========\n");

			session = originate_null_session();
			fst_requires(session);
			channel = switch_core_session_get_channel(session);

			/* Far leg at 16 kHz: this is what makes the write resampler double
			 * the decoded length, exactly as G722 did in production. Both
			 * directions, because switch_core_session_set_read_codec() is also
			 * what points raw_write_frame.codec at this leg - that is how
			 * write_frame() decides it has a "perfect" encode, which is the
			 * branch whose size guard failed in production. */
			fst_requires(switch_core_codec_init(&far_codec, "L16", NULL, NULL, 16000, STUB_PTIME_MS, 1,
												SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
												NULL, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_session_set_read_codec(session, &far_codec) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_session_set_write_codec(session, &far_codec) == SWITCH_STATUS_SUCCESS);

			fst_requires(stub_codec_open(&stub_codec, &impl, stub_decode_leaky_plc, fst_pool) == SWITCH_STATUS_SUCCESS);

			switch_zmalloc(frame.data, SWITCH_RECOMMENDED_BUFFER_SIZE);
			frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;
			frame.codec = &stub_codec;
			frame.rate = STUB_RATE;
			frame.payload = (switch_payload_t) stub_codec.implementation->ianacode;

			/* Baseline: a normal 20 ms payload transcodes fine. */
			frame.datalen = STUB_ENCODED_BYTES_PER_PACKET;
			frame.samples = STUB_DECODED_BYTES_PER_PACKET / 2;
			status = switch_core_session_write_frame(session, &frame, SWITCH_IO_FLAG_NONE, 0);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] baseline 20-byte frame -> status=%d\n", status);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			/* The failing frame: an untransmitted Annex B frame, datalen 0. */
			frame.datalen = 0;
			frame.samples = 0;
			status = switch_core_session_write_frame(session, &frame, SWITCH_IO_FLAG_NONE, 0);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "[TEST] zero-length frame -> status=%d (buggy tree: %d = SWITCH_STATUS_FALSE)\n",
							  status, SWITCH_STATUS_FALSE);

			fst_check(status == SWITCH_STATUS_SUCCESS);
			fst_check(switch_channel_ready(channel));

			switch_safe_free(frame.data);
			switch_core_codec_destroy(&stub_codec);
			switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
			switch_core_session_rwunlock(session);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "========== TEST COMPLETED ==========\n\n");
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
