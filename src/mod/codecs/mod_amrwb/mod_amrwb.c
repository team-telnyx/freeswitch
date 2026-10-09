/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Anthony Minessale II <anthm@freeswitch.org>
 * Brian K. West <brian@freeswitch.org>
 * Dragos Oancea <dragos.oancea@athonet.com>
 * Federico Favaro <federico.favaro@athonet.com>
 * Marco Sinibaldi <marco.sinibaldi@athonet.com>
 *
 * The amrwb codec itself is not distributed with this module.
 *
 * mod_amrwb.c -- GSM-AMRWB Codec Module
 *
 *
 * Two payload types are registered and both offered: 100 octet-aligned and 110 bandwidth
 * efficient, each with octet-align stated. Offers with crc, robust-sorting or interleaving
 * other than 0, or with a mode-set without a mode 0-8, are not matched; other entries of a
 * mode-set are dropped. Answers always state octet-align, return crc, robust-sorting and
 * interleaving as offered (0), and leave out other offered parameters. A codec runs on the
 * payload type of the format it uses, so the core transcodes between legs whose formats differ.
 *
 * A payload may carry up to 12 frames (RFC 4867 4.3/4.4); they are decoded one after the other,
 * except frames at RTP timestamps the previous payload already covered (the last frame of a
 * payload always is): a sender cannot make the codec produce more audio than time passes.
 * Encoding sends one frame (20 ms) per payload, from the first 20 ms of its input (less is
 * padded with silence), and never a mode request (CMR 15). The encoder makes no SID frames (no DTX); a SID decoded alone in a payload is
 * relayed by the encoder of the other leg while the audio passes unmodified (see silence-supp-off).
 *
 * Received mode requests within the mode-set lower the encoding mode, honouring
 * mode-change-neighbor and mode-change-period; CMR 15 ends a request (RFC 4867 4.3.1). The session
 * read codec, or the media bug copy of it, passes them to the session write codec. Undecodable
 * payloads (also: more than 3 bytes after the frames, unless RTP padding) are concealed as lost
 * frames, their CMR ignored, with a WARNING at the 1st, 251st, ... of them. A packet the jitter
 * buffer lost (SFF_PLC) comes with the previous payload, which is decoded again: closer to the
 * lost audio than concealment by the decoder.
 *
 * XML Parameters (on/off values: anything switch_true() accepts: true, on, yes, non-zero ...)
 *
 * default-bitrate
 *		Mode 0-8, default 8. Without a configured mode-set: offered, and answered to an offer
 *		without mode-set (with mode-set-overwrite, to every offer). With mode-set-overwrite and
 *		mode-set-overwrite-with-default-bitrate: offered and answered to every offer.
 *		Encoding starts at the highest mode it may use, not at default-bitrate; adjust-bitrate
 *		"default" moves it to the highest mode it may use not above default-bitrate (else the
 *		lowest one above).
 * volte
 *		Default off. Adds "max-red=0; mode-change-capability=2" to offers and answers
 *		(3GPP TS 26.114).
 * adjust-bitrate
 *		Default off. Vary bitrate according to feedback from RTCP, within the negotiated
 *		mode-set. Only with RTCP enabled.
 * force-oa
 *		Default off. Octet aligned when an offer does not state octet-align. The answer then
 *		differs from an offer that meant bandwidth efficient (RFC 4867 8.3.1): interop only.
 *		Offers are not changed. Whether the fmtp is an offer is taken from the call direction:
 *		on inbound legs (and codecs without a session) it is, on outbound legs it is the answer
 *		to our offer, which states octet-align for each payload type. Not so for a re-INVITE
 *		the far end of an outbound leg sends (force-oa does not apply) or the answer to one we
 *		send on an inbound leg (it does).
 * force-be
 *		Default off. Bandwidth efficient when an offer does not state octet-align. That is
 *		already the RFC 4867 default: its only effect is to cancel force-oa.
 * mode-set-overwrite
 *		Default off. Answer our mode-set instead of the offered one (deviates from RFC 4867
 *		8.3.1). Which one: see mode-set-overwrite-with-default-bitrate; with it (the default)
 *		offers carry default-bitrate as well.
 *		Encode within the offered and answered sets, else within the offered one.
 * mode-set-overwrite-with-default-bitrate
 *		Default on: mode-set-overwrite offers and answers default-bitrate.
 *		Off: it offers and answers mode-set (default-bitrate if none is configured).
 * invite-prefer-oa
 *		No effect for AMR-WB: the core selects AMR-WB with matches_fmtp only.
 * invite-prefer-be
 *		No effect for AMR-WB: the core selects AMR-WB with matches_fmtp only.
 * mode-set
 *		Default none. Modes 0-8 (others are dropped with a WARNING). Offered, and answered to an
 *		offer without mode-set; with mode-set-overwrite answered to every offer. Not used when
 *		mode-set-overwrite and mode-set-overwrite-with-default-bitrate are both on
 *		(default-bitrate instead).
 * debug
 *		Default off. Log FT, Q, frame flag and sizes of every payload at DEBUG, some malformed
 *		ones at ERROR. Also: API "amrwb_debug on|off". "amrwb_show" prints the configuration.
 * silence-supp-off
 *		Default off. No SID frames relayed. Sessions that set up an AMR-WB codec get
 *		suppress_cng: no CN, and 'a=silenceSupp:off - - - -' in the SDP. suppress_cng set on a
 *		session alone also stops SID relay to it.
 * fmtp-extra
 *		Default none. Appended to the fmtp of offers and answers.
 *
 */

#include "switch.h"

SWITCH_MODULE_LOAD_FUNCTION(mod_amrwb_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_amrwb_unload);
SWITCH_MODULE_DEFINITION(mod_amrwb, mod_amrwb_load, mod_amrwb_unload, NULL);

static switch_mutex_t *global_lock;
static volatile switch_atomic_t global_debug;
static char AMRWB_CONFIGURATION[2000];

#define SWITCH_AMRWB_MAX_FMTP_PARAMS 32
#define SWITCH_AMRWB_FMTP_SIZE 1024
#define SWITCH_AMRWB_MODES 10 /* Silence Indicator (SID) included */

static struct {
	switch_byte_t default_bitrate;
	switch_byte_t volte;
	switch_byte_t adjust_bitrate;
	switch_byte_t force_oa;
	switch_byte_t force_be;
	switch_byte_t mode_set_overwrite;
	switch_byte_t mode_set_overwrite_with_default_bitrate;
	switch_byte_t invite_prefer_oa;
	switch_byte_t invite_prefer_be;
	/* the configured mode-set */
	uint16_t enc_modes;
	char *fmtp_extra;
	switch_byte_t silence_supp_off;
} globals;

/* fmtp key: drop spaces before "=" */
static void amrwb_trim_key(char *key)
{
	size_t len = strlen(key);

	while (len && key[len - 1] == ' ') key[--len] = '\0';
}

#ifndef AMRWB_PASSTHROUGH
#include "opencore-amrwb/dec_if.h" /*AMR-WB decoder API*/
#include "vo-amrwbenc/enc_if.h" /*AMR-WB encoder API*/

#include "bitshift.h"

typedef enum {
	AMRWB_OPT_OCTET_ALIGN = (1 << 0),
	AMRWB_OPT_CRC = (1 << 1),
	AMRWB_OPT_MODE_CHANGE_NEIGHBOR = (1 << 2),
	AMRWB_OPT_ROBUST_SORTING = (1 << 3),
	AMRWB_OPT_INTERLEAVING = (1 << 4)
} amrwb_flag_t;

typedef enum {
	AMRWB_BITRATE_7K = 0,
	AMRWB_BITRATE_8K,
	AMRWB_BITRATE_12K,
	AMRWB_BITRATE_14K,
	AMRWB_BITRATE_16K,
	AMRWB_BITRATE_18K,
	AMRWB_BITRATE_20K,
	AMRWB_BITRATE_23K,
	AMRWB_BITRATE_24K
} amrwb_bitrate_t;

#define SWITCH_AMRWB_SID_FRAME_TYPE 9
#define SWITCH_AMRWB_SID_FRAME_SIZE 6
#define SWITCH_AMRWB_SPEECH_LOST_TOC ((14 << 3) | (1 << 2))
#define SWITCH_AMRWB_CMR_NONE 15

struct amrwb_context {
	/* first: kept across switch_core_codec_reset, the other leg's encoder may hold the mutex */
	void *decoded_sid_pcm;
	switch_mutex_t *decoded_sid_mutex;
	void *encoder_state;
	void *decoder_state;
	uint16_t enc_modes;
	switch_byte_t enc_mode;
	uint32_t change_period;
	switch_byte_t flags;
	int debug;
	switch_byte_t decoded_sid[SWITCH_AMRWB_SID_FRAME_SIZE];
	uint32_t decoded_sid_pcm_len;
	switch_bool_t decoded_sid_valid;
	switch_byte_t cmr;
	struct amrwb_cmr_slot *session_cmr;
	uint32_t id;
	switch_byte_t cur_mode;
	uint32_t frames;
	uint32_t concealed;
	/* RTP timestamp right after the frames of the last decoded payload */
	uint32_t next_ts;
	switch_bool_t next_ts_valid;
	/* destroyed, kept on the codec for its re-init (switch_core_codec_reset) */
	switch_bool_t destroyed;
	struct amrwb_framing {
		switch_byte_t octet_align;
		uint16_t answer_modes;
		uint16_t enc_modes;
		switch_byte_t neighbor;
		uint32_t period;
	} framing;
};

#define SWITCH_AMRWB_DEFAULT_BITRATE AMRWB_BITRATE_24K

static const int switch_amrwb_frame_sizes[] = {17, 23, 32, 36, 40, 46, 50, 58, 60, 5, 0, 0, 0, 0, 0, 0};
static const int switch_amrwb_frame_bits[] = {132, 177, 253, 285, 317, 365, 397, 461, 477, 40, 0, 0, 0, 0, 0, 0};

#define SWITCH_AMRWB_OUT_MAX_SIZE 62
#define SWITCH_AMRWB_MAX_FRAMES 12 /* per payload: 240 ms */

/* FT 10-13 are reserved; 14 (SPEECH_LOST) and 15 (NO_DATA) are valid and carry no bits */
#define invalid_frame_type (index >= SWITCH_AMRWB_MODES && index != 0xe && index != 0xf)

/* mode-set entry: a speech mode 0-8, else -1 */
static int amrwb_parse_mode(const char *str)
{
	char *end;
	long mode;

	while (*str == ' ') str++;
	if (*str < '0' || *str > '9') return -1;
	mode = strtol(str, &end, 10);
	while (*end == ' ') end++;
	return (*end || mode > SWITCH_AMRWB_MODES - 2) ? -1 : (int) mode;
}

static switch_bool_t amrwb_mode_allowed(struct amrwb_context *context, int mode)
{
	return (mode >= 0 && mode < SWITCH_AMRWB_MODES - 1 && (context->enc_modes & (1 << mode))) ? SWITCH_TRUE : SWITCH_FALSE;
}

/* highest allowed mode not above mode, else the lowest allowed mode */
static switch_byte_t amrwb_clamp_mode(struct amrwb_context *context, int mode)
{
	int m;

	for (m = mode; m >= 0; m--) {
		if (amrwb_mode_allowed(context, m)) return (switch_byte_t) m;
	}
	for (m = mode + 1; m < SWITCH_AMRWB_MODES - 1; m++) {
		if (amrwb_mode_allowed(context, m)) return (switch_byte_t) m;
	}
	return context->enc_mode;
}

/* mode-change-neighbor: one allowed mode per step; mode-change-period N: steps at every Nth frame */
static switch_byte_t amrwb_next_mode(struct amrwb_context *context, switch_byte_t target)
{
	int m = target, step;

	if (context->cur_mode >= SWITCH_AMRWB_MODES - 1) {
		context->cur_mode = target;
	} else if (context->cur_mode != target && (context->change_period < 2 || context->frames % context->change_period == 0)) {
		if (switch_test_flag(context, AMRWB_OPT_MODE_CHANGE_NEIGHBOR)) {
			step = target > context->cur_mode ? 1 : -1;
			for (m = context->cur_mode + step; m != target && !amrwb_mode_allowed(context, m); m += step);
		}
		context->cur_mode = (switch_byte_t) m;
	}
	context->frames++;

	return context->cur_mode;
}

/* CMR handoff from the session read codec to the session write codec: session memory, atomic access;
 * owner: id of the codec that published it */
struct amrwb_cmr_slot {
	switch_atomic_t cmr;
	switch_atomic_t owner;
};

static uint32_t amrwb_next_id = 0;

static struct amrwb_cmr_slot *amrwb_session_cmr(switch_codec_t *codec, struct amrwb_context *context)
{
	switch_channel_t *channel;
	struct amrwb_cmr_slot *shared;

	if (context->session_cmr || !codec->session) {
		return context->session_cmr;
	}

	channel = switch_core_session_get_channel(codec->session);
	switch_mutex_lock(global_lock);
	if (!(shared = switch_channel_get_private(channel, "mod_amrwb_cmr"))) {
		shared = switch_core_session_alloc(codec->session, sizeof(*shared));
		switch_atomic_set(&shared->cmr, SWITCH_AMRWB_CMR_NONE);
		switch_atomic_set(&shared->owner, 0);
		switch_channel_set_private(channel, "mod_amrwb_cmr", shared);
	}
	if (!context->id && !(context->id = ++amrwb_next_id)) {
		context->id = ++amrwb_next_id;
	}
	switch_mutex_unlock(global_lock);
	context->session_cmr = shared;

	return shared;
}

/* CMR 15: no mode request; the session read codec publishes it for the session write codec */
static void amrwb_set_cmr(switch_codec_t *codec, struct amrwb_context *context, uint8_t cmr)
{
	struct amrwb_cmr_slot *shared;

	/* not a speech mode or NO_DATA, or outside the mode-set: ignored (RFC 4867 4.3.1) */
	if (cmr != SWITCH_AMRWB_CMR_NONE && !amrwb_mode_allowed(context, cmr)) {
		return;
	}

	context->cmr = cmr;

	if (codec->session && codec == switch_core_session_get_read_codec(codec->session) && (shared = amrwb_session_cmr(codec, context))) {
		switch_atomic_set(&shared->owner, context->id);
		switch_atomic_set(&shared->cmr, cmr);
	}
}

static uint32_t amrwb_bits(const uint8_t *buf, uint32_t pos, int n)
{
	uint32_t v = 0;

	for (; n > 0; n--, pos++) {
		v = (v << 1) | ((buf[pos / 8] >> (7 - pos % 8)) & 1);
	}
	return v;
}

/* the frames of a payload, RFC 4867 4.3 (bandwidth efficient) and 4.4 (octet aligned), in the
 * storage format of the decoder: a byte with FT and Q, then the frame bits; the number of frames
 * and the bytes they take (*used), 0 if the payload is not valid */
static int amrwb_unpack(const uint8_t *payload, uint32_t len, switch_bool_t oa, uint8_t frames[][SWITCH_AMRWB_OUT_MAX_SIZE], uint32_t *used)
{
	uint8_t tocs[SWITCH_AMRWB_MAX_FRAMES];
	uint32_t pos = oa ? 8 : 4, total = len * 8;
	int n = 0, f, i, index, bits;

	/* ToC: F, FT, Q (6 bits; padded to 8 when octet aligned) */
	do {
		if (n == SWITCH_AMRWB_MAX_FRAMES || pos + 6 > total) {
			return 0;
		}
		tocs[n] = (uint8_t) amrwb_bits(payload, pos, 6);
		pos += oa ? 8 : 6;
		index = (tocs[n] >> 1) & 0x0f;
		if (invalid_frame_type) {
			return 0;
		}
	} while (tocs[n++] & 0x20);

	for (f = 0; f < n; f++) {
		index = (tocs[f] >> 1) & 0x0f;
		bits = switch_amrwb_frame_bits[index];
		if (pos + bits > total) {
			return 0;
		}
		memset(frames[f], 0, SWITCH_AMRWB_OUT_MAX_SIZE);
		frames[f][0] = (uint8_t) ((tocs[f] & 0x1f) << 2);
		for (i = 0; i < bits; i++, pos++) {
			if ((payload[pos / 8] >> (7 - pos % 8)) & 1) {
				frames[f][1 + i / 8] |= (uint8_t) (0x80 >> (i % 8));
			}
		}
		/* octet aligned: each frame padded to a byte */
		if (oa) {
			pos = (pos + 7) & ~7U;
		}
	}
	*used = (pos + 7) / 8;

	return n;
}

static void amrwb_put_bits(uint8_t *buf, uint32_t *pos, uint32_t value, int n)
{
	for (; n > 0; n--, (*pos)++) {
		if ((value >> (n - 1)) & 1) {
			buf[*pos / 8] |= (uint8_t) (0x80 >> (*pos % 8));
		}
	}
}

/* frames in the storage format of the encoder (as amrwb_unpack makes them) as a payload with CMR 15,
 * RFC 4867 4.3 (bandwidth efficient) and 4.4 (octet aligned); its length. payload: room for
 * 1 + n * SWITCH_AMRWB_OUT_MAX_SIZE bytes */
static uint32_t amrwb_pack(uint8_t frames[][SWITCH_AMRWB_OUT_MAX_SIZE], int n, switch_bool_t oa, uint8_t *payload)
{
	uint32_t pos = 0;
	int f, i, bits;

	memset(payload, 0, 1 + n * SWITCH_AMRWB_OUT_MAX_SIZE);
	/* CMR, then the 4 reserved bits when octet aligned */
	amrwb_put_bits(payload, &pos, SWITCH_AMRWB_CMR_NONE, 4);
	if (oa) {
		pos += 4;
	}
	/* ToC: F, FT, Q (padded to 8 bits when octet aligned) */
	for (f = 0; f < n; f++) {
		amrwb_put_bits(payload, &pos, ((f < n - 1) << 5) | ((frames[f][0] >> 2) & 0x1f), 6);
		if (oa) {
			pos += 2;
		}
	}
	for (f = 0; f < n; f++) {
		bits = switch_amrwb_frame_bits[(frames[f][0] >> 3) & 0x0f];
		for (i = 0; i < bits; i++) {
			amrwb_put_bits(payload, &pos, (frames[f][1 + i / 8] >> (7 - i % 8)) & 1, 1);
		}
		/* octet aligned: each frame padded to a byte */
		if (oa) {
			pos = (pos + 7) & ~7U;
		}
	}

	return (pos + 7) / 8;
}

/* SID frames not wanted toward this codec's peer: silence-supp-off, or suppress_cng on its session */
static switch_bool_t amrwb_sid_suppressed(switch_codec_t *codec)
{
	switch_media_handle_t *smh;

	if (globals.silence_supp_off) {
		return SWITCH_TRUE;
	}
	if (!codec->session) {
		return SWITCH_FALSE;
	}
	if (switch_channel_var_true(switch_core_session_get_channel(codec->session), "suppress_cng")) {
		return SWITCH_TRUE;
	}

	return ((smh = switch_core_session_get_media_handle(codec->session)) && switch_media_handle_test_media_flag(smh, SCMF_SUPPRESS_CNG)) ? SWITCH_TRUE : SWITCH_FALSE;
}

/* the SID the other leg decoded into decoded_data replaces frame (storage format) */
static switch_bool_t switch_amrwb_relay_sid(switch_codec_t *codec, switch_codec_t *other_codec, void *decoded_data, uint32_t decoded_data_len, switch_byte_t *frame, switch_byte_t mode)
{
	struct amrwb_context *other_context;
	switch_byte_t sid[SWITCH_AMRWB_SID_FRAME_SIZE];
	int size = 0;

	if (globals.silence_supp_off) {
		return SWITCH_FALSE;
	}

	if (!other_codec || !other_codec->implementation || !other_codec->implementation->iananame || !other_codec->implementation->modname ||
		strcasecmp(other_codec->implementation->iananame, "AMR-WB") ||
		strcmp(other_codec->implementation->modname, "mod_amrwb")) {
		return SWITCH_FALSE;
	}

	other_context = other_codec->private_info;
	if (!other_context || !other_context->decoded_sid_mutex) {
		return SWITCH_FALSE;
	}

	switch_mutex_lock(other_context->decoded_sid_mutex);
	/* Relay is safe only while the PCM is byte-identical to the source
	 * decoder output. Denoising, volume changes, resampling, or PLC must
	 * use the target encoder output instead of the cached source SID. */
	if (!other_context->decoded_sid_valid ||
		other_context->decoded_sid_pcm_len != decoded_data_len ||
		memcmp(other_context->decoded_sid_pcm, decoded_data, decoded_data_len)) {
		goto done;
	}

	memcpy(sid, other_context->decoded_sid, SWITCH_AMRWB_SID_FRAME_SIZE);
	size = SWITCH_AMRWB_SID_FRAME_SIZE;

done:
	switch_mutex_unlock(other_context->decoded_sid_mutex);

	if (!size || amrwb_sid_suppressed(codec)) {
		return SWITCH_FALSE;
	}

	memcpy(frame, sid, SWITCH_AMRWB_SID_FRAME_SIZE);
	/* mode indication: last 4 bits, the mode this encoder sends */
	frame[SWITCH_AMRWB_SID_FRAME_SIZE - 1] = (frame[SWITCH_AMRWB_SID_FRAME_SIZE - 1] & 0xf0) | (mode & 0x0f);

	return SWITCH_TRUE;
}

static switch_bool_t switch_amrwb_info(switch_codec_t *codec, unsigned char *encoded_buf, int encoded_data_len, int payload_format, char *print_text)
{
	uint8_t *tocs;
	int framesz, index, not_last_frame, q, ft;
	uint8_t shift_tocs[2] = {0x00, 0x00};

	if (!encoded_buf) {
		return SWITCH_FALSE;
	}
	if (encoded_data_len < 2) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(codec->session), SWITCH_LOG_ERROR,
			"%s: Invalid AMR-WB payload size: %d\n", print_text, encoded_data_len);
		return SWITCH_FALSE;
	}

	/* payload format can be OA (octet-aligned) or BE (bandwidth efficient)*/
	if (payload_format) {
		/* OA */
		encoded_buf++; /* CMR skip */
		tocs = encoded_buf;
		index = (tocs[0] >> 3) & 0x0f;
		if (invalid_frame_type) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(codec->session), SWITCH_LOG_ERROR, "AMRWB decoder (OA): Invalid TOC 0x%x\n", index);
			return SWITCH_FALSE;
		}
		framesz = switch_amrwb_frame_sizes[index];
		if (encoded_data_len < framesz + 2) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(codec->session), SWITCH_LOG_ERROR,
				"%s (OA): Invalid frame size: %d\n", print_text, encoded_data_len);
			return SWITCH_FALSE;
		}
		not_last_frame = (tocs[0] >> 7) & 1;
		q = (tocs[0] >> 2) & 1;
		ft = tocs[0] >> 3;
		ft &= 0x0f; /* Frame Type, without F */
	} else {
		/* BE */
		memcpy(shift_tocs, encoded_buf, 2);
		/* shift for BE */
		amrwb_array_lshift(4, shift_tocs, 2);
		not_last_frame = (shift_tocs[0] >> 7) & 1;
		q = (shift_tocs[0] >> 2) & 1;
		ft = shift_tocs[0] >> 3;
		ft &= 0x0f; /* Frame Type, without F */
		index = (shift_tocs[0] >> 3) & 0x0f;
		if (invalid_frame_type) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(codec->session), SWITCH_LOG_ERROR, "AMRWB decoder (BE): Invalid TOC 0x%x\n", index);
			return SWITCH_FALSE;
		}
		framesz = switch_amrwb_frame_sizes[index];
		if (encoded_data_len * 8 < switch_amrwb_frame_bits[index] + 10) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(codec->session), SWITCH_LOG_ERROR,
				"%s (BE): Invalid frame size: %d\n", print_text, encoded_data_len);
			return SWITCH_FALSE;
		}
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(codec->session), SWITCH_LOG_DEBUG, 
			"%s (%s): FT: [0x%x] Q: [0x%x] Frame flag: [%d]\n",
			print_text, payload_format ? "OA":"BE", ft, q, not_last_frame);
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(codec->session), SWITCH_LOG_DEBUG, 
			"%s (%s): AMRWB encoded voice payload sz: [%d] : | encoded_data_len: [%d]\n", 
			print_text, payload_format ? "OA":"BE", framesz, encoded_data_len);

	return SWITCH_TRUE;
}
#endif

static switch_status_t amrwb_parse_fmtp_cb(const char *fmtp, switch_codec_fmtp_t *codec_fmtp)
{
	/* The core uses IGNORE to skip an offer for "AMR" only: AMR-WB is matched with matches_fmtp */
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Considering fmtp %s\n", fmtp);

	if (!zstr(fmtp)) {
		int x, argc;
		char *argv[SWITCH_AMRWB_MAX_FMTP_PARAMS];
		char *fmtp_dup = strdup(fmtp);

		/* If there is no octet-align param on fmtp then default is 0 (bandwidth efficient). */
		int oa = 0;

		if (!fmtp_dup) {
			return SWITCH_STATUS_FALSE;
		}

		argc = switch_separate_string(fmtp_dup, ';', argv, (sizeof(argv) / sizeof(argv[0])));
		for (x = 0; x < argc; x++) {
			char *data = argv[x];
			char *arg;
			while (*data == ' ') {
				data++;
			}

			if ((arg = strchr(data, '='))) {
				*arg++ = '\0';
				amrwb_trim_key(data);

				if (!strcasecmp(data, "octet-align")) {
					oa = switch_true(arg);
				}
			}
		}
		free(fmtp_dup);

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "AMR-WB fmtp mode: %s\n", oa ? "octet aligned" : "bandwidth efficient");
		if ((oa == 0 && globals.invite_prefer_oa) || (oa == 1 && globals.invite_prefer_be)) {
			return SWITCH_STATUS_IGNORE;
		}
	}

	/* FALSE: as if there were no callback */
	return SWITCH_STATUS_FALSE;
}

#ifndef AMRWB_PASSTHROUGH
/* answered and encoded mode-sets for an offered one (0: none offered): mirror the offer,
 * else answer the configured mode-set (default-bitrate when none is configured, or with
 * mode-set-overwrite and mode-set-overwrite-with-default-bitrate) and encode within both,
 * else within the offer */
static void amrwb_mode_sets(uint16_t offered, uint16_t *answer_modes, uint16_t *enc_modes)
{
	if (offered && !globals.mode_set_overwrite) {
		*answer_modes = *enc_modes = offered;
		return;
	}

	*answer_modes = globals.enc_modes;
	if (!*answer_modes || (globals.mode_set_overwrite && globals.mode_set_overwrite_with_default_bitrate)) {
		*answer_modes = (uint16_t) (1 << globals.default_bitrate);
	}

	*enc_modes = *answer_modes;
	if (offered) {
		*enc_modes = (offered & *answer_modes) ? (offered & *answer_modes) : offered;
	}
}

/* force-oa/force-be: for an offer only. On an outbound leg the fmtp is the answer to our offer,
 * which states octet-align for each payload type; a codec without a session takes it as an offer. */
static switch_bool_t amrwb_force_applies(switch_core_session_t *session)
{
	return (!session || switch_channel_direction(switch_core_session_get_channel(session)) == SWITCH_CALL_DIRECTION_INBOUND) ? SWITCH_TRUE : SWITCH_FALSE;
}

/* octet-align when the fmtp does not state it */
static switch_byte_t amrwb_default_octet_align(switch_core_session_t *session)
{
	return (globals.force_oa && !globals.force_be && amrwb_force_applies(session)) ? 1 : 0;
}

/* what the running codec does with an fmtp: octet-align after force-oa/force-be,
 * answered and encoded mode-sets, mode-change-neighbor, mode-change-period (0 below 2) */
static void amrwb_fmtp_framing(const char *fmtp, switch_core_session_t *session, struct amrwb_framing *framing)
{
	char *argv[SWITCH_AMRWB_MAX_FMTP_PARAMS], *m_argv[SWITCH_AMRWB_MAX_FMTP_PARAMS];
	char *fmtp_dup;
	int argc, m_argc, x, y, mode;
	switch_bool_t octet_align_given = SWITCH_FALSE;
	uint16_t offered = 0;

	memset(framing, 0, sizeof(*framing));

	if (!zstr(fmtp) && (fmtp_dup = strdup(fmtp))) {
		argc = switch_separate_string(fmtp_dup, ';', argv, (sizeof(argv) / sizeof(argv[0])));
		for (x = 0; x < argc; x++) {
			char *data = argv[x], *arg;

			while (*data == ' ') data++;
			if (!(arg = strchr(data, '='))) continue;
			*arg++ = '\0';
			amrwb_trim_key(data);
			while (*arg == ' ') arg++;
			if (!strcasecmp(data, "octet-align")) {
				octet_align_given = SWITCH_TRUE;
				framing->octet_align = switch_true(arg) ? 1 : 0;
			} else if (!strcasecmp(data, "mode-change-neighbor")) {
				framing->neighbor = atoi(arg) ? 1 : 0;
			} else if (!strcasecmp(data, "mode-change-period")) {
				framing->period = atoi(arg) > 0 ? (uint32_t) atoi(arg) : 0;
			} else if (!strcasecmp(data, "mode-set")) {
				m_argc = switch_separate_string(arg, ',', m_argv, (sizeof(m_argv) / sizeof(m_argv[0])));
				for (y = 0; y < m_argc; y++) {
					if ((mode = amrwb_parse_mode(m_argv[y])) >= 0) {
						offered |= (1 << mode);
					}
				}
			}
		}
		free(fmtp_dup);
	}

	if (!octet_align_given) {
		framing->octet_align = amrwb_default_octet_align(session);
	}
	if (framing->period < 2) {
		framing->period = 0;
	}
	amrwb_mode_sets(offered, &framing->answer_modes, &framing->enc_modes);
}

static int extract_octet_align(const char *fmtp);

/* the implementation of the payload format the codec runs (the one whose fmtp states that
 * octet-align): the core passes payloads between two codecs of one implementation untranscoded */
static void amrwb_set_implementation(switch_codec_t *codec, int octet_align)
{
	const switch_codec_implementation_t *impl;

	if (!codec->codec_interface || !codec->implementation || extract_octet_align(codec->implementation->fmtp) == octet_align) {
		return;
	}
	for (impl = codec->codec_interface->implementations; impl; impl = impl->next) {
		if (impl->init == codec->implementation->init && impl->microseconds_per_packet == codec->implementation->microseconds_per_packet &&
			impl->number_of_channels == codec->implementation->number_of_channels && extract_octet_align(impl->fmtp) == octet_align) {
			codec->implementation = impl;
			return;
		}
	}
}
#endif

static switch_status_t switch_amrwb_init(switch_codec_t *codec, switch_codec_flag_t flags, const switch_codec_settings_t *codec_settings)
{
#ifdef AMRWB_PASSTHROUGH
	codec->flags |= SWITCH_CODEC_FLAG_PASSTHROUGH;
	if (codec->fmtp_in) {
		codec->fmtp_out = switch_core_strdup(codec->memory_pool, codec->fmtp_in);
	}

	return SWITCH_STATUS_SUCCESS;
#else
	struct amrwb_context *context = codec->private_info;
	int encoding, decoding;
	int x, i, argc, fmtptmp_pos;
	char *argv[SWITCH_AMRWB_MAX_FMTP_PARAMS];
	char fmtptmp[SWITCH_AMRWB_FMTP_SIZE];
	char *fmtp_dup = NULL;
	switch_core_session_t *session = codec->session;
	switch_bool_t octet_align_given = SWITCH_FALSE;
	uint16_t answer_modes;

	encoding = (flags & SWITCH_CODEC_FLAG_ENCODE);
	decoding = (flags & SWITCH_CODEC_FLAG_DECODE);

	/* switch_core_codec_reset: re-init after destroy, on the same pool; reuse what it allocated */
	if (context && context->destroyed) {
		switch_mutex_lock(context->decoded_sid_mutex);
		memset(&context->encoder_state, 0, sizeof(*context) - offsetof(struct amrwb_context, encoder_state));
		context->destroyed = SWITCH_TRUE;
		switch_mutex_unlock(context->decoded_sid_mutex);
	} else {
		context = NULL;
	}

	if (!(encoding || decoding) || (!context && !(context = switch_core_alloc(codec->memory_pool, sizeof(struct amrwb_context))))) {
		return SWITCH_STATUS_FALSE;
	} else {

		/* "mode" may mean two different things:
		 * "Octet Aligned" or "Bandwidth Efficient" encoding mode ,
		 * or the actual bitrate  which is set with FMTP param "mode-set". */
		/* https://tools.ietf.org/html/rfc4867 */

		context->enc_mode = globals.default_bitrate;

		/* octet-align = 0  - per RFC - if there's no `octet-align` FMTP value then BE is employed */
		switch_clear_flag(context, AMRWB_OPT_OCTET_ALIGN);

		if (codec->fmtp_in) {
			fmtp_dup = strdup(codec->fmtp_in);
			switch_assert(fmtp_dup);

			argc = switch_separate_string(fmtp_dup, ';', argv, (sizeof(argv) / sizeof(argv[0])));

			for (x = 0; x < argc; x++) {
				char *data = argv[x];
				char *arg;

				while (*data && *data == ' ') {
					data++;
				}

				if ((arg = strchr(data, '='))) {
					*arg++ = '\0';
					amrwb_trim_key(data);
					while (*arg == ' ') arg++;
					if (!strcasecmp(data, "octet-align")) {
						octet_align_given = SWITCH_TRUE;
						if (switch_true(arg)) {
							switch_set_flag(context, AMRWB_OPT_OCTET_ALIGN);
						} else {
							switch_clear_flag(context, AMRWB_OPT_OCTET_ALIGN);
						}
					} else if (!strcasecmp(data, "mode-change-neighbor")) {
						if (atoi(arg)) {
							switch_set_flag(context, AMRWB_OPT_MODE_CHANGE_NEIGHBOR);
						} else {
							switch_clear_flag(context, AMRWB_OPT_MODE_CHANGE_NEIGHBOR);
						}
					} else if (!strcasecmp(data, "crc") || !strcasecmp(data, "robust-sorting") || !strcasecmp(data, "interleaving")) {
						/* only 0 is matched (has_unsupported_option); returned in the answer, RFC 4867 8.3.1 */
						switch_set_flag(context, !strcasecmp(data, "crc") ? AMRWB_OPT_CRC :
										!strcasecmp(data, "robust-sorting") ? AMRWB_OPT_ROBUST_SORTING : AMRWB_OPT_INTERLEAVING);
					} else if (!strcasecmp(data, "mode-change-period")) {
						int period = atoi(arg);

						context->change_period = period > 0 ? (uint32_t) period : 0;
					} else if (!strcasecmp(data, "mode-set")) {
						int y, m_argc;
						char *m_argv[SWITCH_AMRWB_MAX_FMTP_PARAMS];

						m_argc = switch_separate_string(arg, ',', m_argv, (sizeof(m_argv) / sizeof(m_argv[0])));

						for (y = 0; y < m_argc; y++) {
							int mode = amrwb_parse_mode(m_argv[y]);

							if (mode < 0) {
								continue;
							}
							context->enc_modes |= (1 << mode);
							context->enc_mode = (switch_byte_t) mode;
						}
					}
				}
			}

			free(fmtp_dup);
		}

		/* force-oa / force-be only when the fmtp does not state octet-align */
		if (!octet_align_given && amrwb_default_octet_align(session)) {
			switch_set_flag(context, AMRWB_OPT_OCTET_ALIGN);
		}
		amrwb_set_implementation(codec, switch_test_flag(context, AMRWB_OPT_OCTET_ALIGN) ? 1 : 0);

		amrwb_mode_sets(context->enc_modes, &answer_modes, &context->enc_modes);

		fmtptmp_pos = switch_snprintf(fmtptmp, sizeof(fmtptmp), "mode-set=");
		for (i = 0; SWITCH_AMRWB_MODES-1 > i; ++i) {
			if (answer_modes & (1 << i)) {
				fmtptmp_pos += switch_snprintf(fmtptmp + fmtptmp_pos, sizeof(fmtptmp) - fmtptmp_pos, (size_t) fmtptmp_pos > strlen("mode-set=") ? ",%d" : "%d", i);
			}
			/* the highest mode we may encode */
			if (context->enc_modes & (1 << i)) {
				context->enc_mode = (switch_byte_t) i;
			}
		}

		if (globals.adjust_bitrate) {
			switch_set_flag(codec, SWITCH_CODEC_FLAG_HAS_ADJ_BITRATE);
		}

		if (!globals.volte) {
			fmtptmp_pos += switch_snprintf(fmtptmp + fmtptmp_pos, sizeof(fmtptmp) - fmtptmp_pos, ";octet-align=%d",
					switch_test_flag(context, AMRWB_OPT_OCTET_ALIGN) ? 1 : 0);
		} else {
			fmtptmp_pos += switch_snprintf(fmtptmp + fmtptmp_pos, sizeof(fmtptmp) - fmtptmp_pos, ";octet-align=%d;max-red=0;mode-change-capability=2",
					switch_test_flag(context, AMRWB_OPT_OCTET_ALIGN) ? 1 : 0);
		}

		if (switch_test_flag(context, AMRWB_OPT_CRC)) {
			fmtptmp_pos += switch_snprintf(fmtptmp + fmtptmp_pos, sizeof(fmtptmp) - fmtptmp_pos, ";crc=0");
		}
		if (switch_test_flag(context, AMRWB_OPT_ROBUST_SORTING)) {
			fmtptmp_pos += switch_snprintf(fmtptmp + fmtptmp_pos, sizeof(fmtptmp) - fmtptmp_pos, ";robust-sorting=0");
		}
		if (switch_test_flag(context, AMRWB_OPT_INTERLEAVING)) {
			fmtptmp_pos += switch_snprintf(fmtptmp + fmtptmp_pos, sizeof(fmtptmp) - fmtptmp_pos, ";interleaving=0");
		}

		if (!zstr(globals.fmtp_extra)) {
			fmtptmp_pos += switch_snprintf(fmtptmp + fmtptmp_pos, sizeof(fmtptmp) - fmtptmp_pos, "; %s", globals.fmtp_extra);
		}

		if (globals.silence_supp_off) {
			if (session) {
				switch_media_handle_t *smh;

				switch_channel_set_variable(switch_core_session_get_channel(session), "suppress_cng", "true");
				/* the variable alone reaches the media flag only when RTP starts, after the first answer */
				if ((smh = switch_core_session_get_media_handle(session))) {
					switch_media_handle_set_media_flag(smh, SCMF_SUPPRESS_CNG);
				}
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Turning CNG off (silence suppression off, suppress_cng=true) due to silence-supp-off=true\n");
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "silence-supp-off: no session for suppress_cng, SID frames are still not relayed\n");
			}
		}

		codec->fmtp_out = switch_core_strdup(codec->memory_pool, fmtptmp);

		context->encoder_state = NULL;
		context->decoder_state = NULL;
		if (!context->decoded_sid_pcm) {
			context->decoded_sid_pcm = switch_core_alloc(codec->memory_pool, codec->implementation->decoded_bytes_per_packet);
		}
		if (!context->decoded_sid_mutex) {
			switch_mutex_init(&context->decoded_sid_mutex, SWITCH_MUTEX_UNNESTED, codec->memory_pool);
		}

		if ((encoding && !(context->encoder_state = E_IF_init())) || (decoding && !(context->decoder_state = D_IF_init()))) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "AMRWB: cannot create the %s\n", context->encoder_state || !encoding ? "decoder" : "encoder");
			if (context->encoder_state) {
				E_IF_exit(context->encoder_state);
				context->encoder_state = NULL;
			}
			context->destroyed = SWITCH_TRUE;
			codec->private_info = context;
			return SWITCH_STATUS_FALSE;
		}

		context->cmr = SWITCH_AMRWB_CMR_NONE;
		amrwb_fmtp_framing(codec->fmtp_in, session, &context->framing);
		context->cur_mode = SWITCH_AMRWB_CMR_NONE;
		context->destroyed = SWITCH_FALSE;
		codec->private_info = context;

		return SWITCH_STATUS_SUCCESS;
	}
#endif
}

static switch_status_t switch_amrwb_destroy(switch_codec_t *codec)
{
#ifndef AMRWB_PASSTHROUGH
	struct amrwb_context *context = codec->private_info;

	if (!context) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (context->encoder_state) {
		E_IF_exit(context->encoder_state);
		context->encoder_state = NULL;
	}
	if (context->decoder_state) {
		D_IF_exit(context->decoder_state);
		context->decoder_state = NULL;
	}
	/* the request ends with the read codec that received it, unless another one published since */
	if (context->session_cmr && switch_atomic_read(&context->session_cmr->owner) == context->id) {
		switch_atomic_set(&context->session_cmr->cmr, SWITCH_AMRWB_CMR_NONE);
	}
	/* kept for switch_core_codec_reset, which calls init next on the same pool;
	 * switch_core_codec_destroy clears the codec */
	context->destroyed = SWITCH_TRUE;
#endif
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t switch_amrwb_encode(switch_codec_t *codec,
										   switch_codec_t *other_codec,
										   void *decoded_data,
										   uint32_t decoded_data_len,
										   uint32_t decoded_rate, void *encoded_data, uint32_t *encoded_data_len, uint32_t *encoded_rate,
										   unsigned int *flag)
{
#ifdef AMRWB_PASSTHROUGH
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "This codec is only usable in passthrough mode!\n");
	return SWITCH_STATUS_FALSE;
#else
	struct amrwb_context *context = codec->private_info;
	uint8_t frames[1][SWITCH_AMRWB_OUT_MAX_SIZE];
	uint8_t payload[1 + SWITCH_AMRWB_OUT_MAX_SIZE];
	int16_t padded[320] = { 0 };
	int16_t *pcm = decoded_data;
	uint32_t len;
	switch_byte_t mode, cmr;
	struct amrwb_cmr_slot *shared;

	if (!context || context->destroyed) {
		return SWITCH_STATUS_FALSE;
	}

	/* one frame (20 ms) per payload, as negotiated (ptime); less than 20 ms padded with silence */
	if (decoded_data_len < codec->implementation->decoded_bytes_per_packet) {
		memcpy(padded, decoded_data, decoded_data_len < sizeof(padded) ? decoded_data_len : sizeof(padded));
		pcm = padded;
	}

	/* Always advance the stateful encoder, even when the wire payload is replaced
	 * with the source SID, so speech resumes from the correct encoder history. */
	mode = context->enc_mode;
	cmr = (shared = amrwb_session_cmr(codec, context)) ? (switch_byte_t) switch_atomic_read(&shared->cmr) : context->cmr;
	if (cmr < SWITCH_AMRWB_MODES - 1) {
		switch_byte_t requested = amrwb_clamp_mode(context, cmr);

		if (requested < mode) {
			mode = requested;
		}
	}
	mode = amrwb_next_mode(context, mode);

	if (E_IF_encode(context->encoder_state, mode, pcm, frames[0], 0) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "AMRWB encoder: E_IF_encode() ERROR!\n");
		return SWITCH_STATUS_FALSE;
	}

	switch_amrwb_relay_sid(codec, other_codec, decoded_data, decoded_data_len, frames[0], mode);

	len = amrwb_pack(frames, 1, switch_test_flag(context, AMRWB_OPT_OCTET_ALIGN) ? SWITCH_TRUE : SWITCH_FALSE, payload);
	memcpy(encoded_data, payload, len);
	*encoded_data_len = len;

	if (switch_atomic_read(&global_debug)) {
		switch_amrwb_info(codec, encoded_data, *encoded_data_len, switch_test_flag(context, AMRWB_OPT_OCTET_ALIGN) ? 1 : 0, "AMRWB encoder");
	}

	return SWITCH_STATUS_SUCCESS;

#endif
}

static switch_status_t switch_amrwb_decode(switch_codec_t *codec,
										   switch_codec_t *other_codec,
										   void *encoded_data,
										   uint32_t encoded_data_len,
										   uint32_t encoded_rate, void *decoded_data, uint32_t *decoded_data_len, uint32_t *decoded_rate,
										   unsigned int *flag)
{
#ifdef AMRWB_PASSTHROUGH
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "This codec is only usable in passthrough mode!\n");
	return SWITCH_STATUS_FALSE;
#else
	struct amrwb_context *context = codec->private_info;
	uint8_t frames[SWITCH_AMRWB_MAX_FRAMES][SWITCH_AMRWB_OUT_MAX_SIZE];
	uint8_t *payload = encoded_data;
	uint32_t frame_bytes = codec->implementation->decoded_bytes_per_packet;
	uint32_t room = *decoded_data_len < frame_bytes ? frame_bytes : *decoded_data_len;
	uint32_t used = 0;
	int n, f, skip = 0, frame_type;
	switch_frame_t *frame = codec->cur_frame;

	if (!context || context->destroyed) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "AMRWB decoder: Invalid context\n");
		goto decode_error;
	}

	/* Never let a failed or non-SID decode reuse a SID cached by the
	 * preceding packet. */
	switch_mutex_lock(context->decoded_sid_mutex);
	context->decoded_sid_valid = SWITCH_FALSE;
	context->decoded_sid_pcm_len = 0;
	switch_mutex_unlock(context->decoded_sid_mutex);

	if (!payload || !encoded_data_len) {
		goto conceal;
	}

	if (switch_atomic_read(&global_debug)) {
		switch_amrwb_info(codec, payload, encoded_data_len, switch_test_flag(context, AMRWB_OPT_OCTET_ALIGN) ? 1 : 0, "AMRWB decoder");
	}

	if (!(n = amrwb_unpack(payload, encoded_data_len, switch_test_flag(context, AMRWB_OPT_OCTET_ALIGN) ? SWITCH_TRUE : SWITCH_FALSE, frames, &used))) {
		goto conceal;
	}

	/* trailing bytes: up to 3, or RTP padding (count in the last byte, RFC 3550 5.1) */
	if (encoded_data_len > used + 3 && payload[encoded_data_len - 1] != encoded_data_len - used) {
		goto conceal;
	}

	/* the CMR is in the first 4 bits in both payload formats; a media bug decodes with a copy of
	 * the read codec, without a session: the request is the one of the codec of the frame */
	if (frame && frame->codec && frame->codec != codec && frame->codec->implementation &&
		frame->codec->implementation->decode == codec->implementation->decode &&
		frame->codec->private_info && !((struct amrwb_context *) frame->codec->private_info)->destroyed) {
		amrwb_set_cmr(frame->codec, frame->codec->private_info, payload[0] >> 4);
	} else {
		amrwb_set_cmr(codec, context, payload[0] >> 4);
	}

	/* no more audio than the RTP timestamps advance: frames at timestamps the previous payload
	 * already covered are not decoded, the last frame always is */
	if (frame) {
		int32_t overlap = (int32_t) (context->next_ts - frame->timestamp);

		if (context->next_ts_valid && overlap > 0) {
			skip = (int) (overlap / (int32_t) codec->implementation->samples_per_packet);
			if (skip > n - 1) {
				skip = n - 1;
			}
		}
		context->next_ts = frame->timestamp + (uint32_t) n * codec->implementation->samples_per_packet;
		context->next_ts_valid = SWITCH_TRUE;
	}

	if ((uint32_t) (n - skip) * frame_bytes > room) {
		goto conceal;
	}

	for (f = skip; f < n; f++) {
		frame_type = (frames[f][0] >> 3) & 0x0f;
		/* Q=0: bad frame (bfi), decoded like a lost frame (opencore has no SPEECH_BAD/SID_BAD input) */
		D_IF_decode(context->decoder_state, frames[f], (int16_t *) ((uint8_t *) decoded_data + (f - skip) * frame_bytes),
					frame_type <= SWITCH_AMRWB_SID_FRAME_TYPE && !(frames[f][0] & 0x04));
	}
	*decoded_data_len = (n - skip) * frame_bytes;

	/* a SID alone in its payload: the encoder of the other leg may relay it */
	if (n == 1 && ((frames[0][0] >> 3) & 0x0f) == SWITCH_AMRWB_SID_FRAME_TYPE) {
		switch_mutex_lock(context->decoded_sid_mutex);
		memcpy(context->decoded_sid, frames[0], SWITCH_AMRWB_SID_FRAME_SIZE);
		memcpy(context->decoded_sid_pcm, decoded_data, frame_bytes);
		context->decoded_sid_pcm_len = frame_bytes;
		context->decoded_sid_valid = SWITCH_TRUE;
		switch_mutex_unlock(context->decoded_sid_mutex);
	}

	return SWITCH_STATUS_SUCCESS;

conceal:
	/* undecodable payload: treat as a lost frame */
	if (context->concealed++ % 250 == 0) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(codec->session), SWITCH_LOG_WARNING, "AMRWB decoder: %u undecodable payloads concealed\n", context->concealed);
	}
	frames[0][0] = SWITCH_AMRWB_SPEECH_LOST_TOC;
	D_IF_decode(context->decoder_state, frames[0], (int16_t *) decoded_data, 0);
	*decoded_data_len = frame_bytes;
	return SWITCH_STATUS_SUCCESS;

decode_error:
	/* Check if codec reset is in progress to prevent call termination */
	if (switch_test_flag(codec, SWITCH_CODEC_FLAG_RESET_PENDING)) {
		return SWITCH_STATUS_NOOP;
	}
	return SWITCH_STATUS_FALSE;
#endif
}

#ifndef AMRWB_PASSTHROUGH
static switch_status_t switch_amrwb_control(switch_codec_t *codec,
										   switch_codec_control_command_t cmd,
										   switch_codec_control_type_t ctype,
										   void *cmd_data,
										   switch_codec_control_type_t atype,
										   void *cmd_arg,
										   switch_codec_control_type_t *rtype,
										   void **ret_data)
{
	struct amrwb_context *context = codec->private_info;
	int debug = (int) switch_atomic_read(&global_debug);

	if (!context || context->destroyed) {
		return SWITCH_STATUS_FALSE;
	}

	switch(cmd) {
	case SCC_DEBUG:
		{
			int32_t level = *((uint32_t *) cmd_data);
			context->debug = level;
		}
		break;
	case SCC_CODEC_SPECIFIC:
		{
			const char *command = (const char *) cmd_data;

			if (!zstr(command) && !strcasecmp(command, "fmtp_changes_framing") && rtype && ret_data) {
				struct amrwb_framing framing;

				amrwb_fmtp_framing((const char *) cmd_arg, codec->session, &framing);
				*rtype = SCCT_STRING;
				*ret_data = (void *) ((framing.octet_align != context->framing.octet_align || framing.answer_modes != context->framing.answer_modes ||
									   framing.enc_modes != context->framing.enc_modes || framing.neighbor != context->framing.neighbor ||
									   framing.period != context->framing.period) ? "true" : "false");
			}
		}
		break;
	case SCC_AUDIO_ADJUST_BITRATE:
		{
			const char *cmd = (const char *)cmd_data;
			int mode;

			if (!strcasecmp(cmd, "increase")) {
				for (mode = context->enc_mode + 1; mode < SWITCH_AMRWB_MODES - 1 && !amrwb_mode_allowed(context, mode); mode++);
				if (mode < SWITCH_AMRWB_MODES - 1) {
					context->enc_mode = (switch_byte_t) mode;
				}
			} else if (!strcasecmp(cmd, "decrease")) {
				for (mode = context->enc_mode - 1; mode >= 0 && !amrwb_mode_allowed(context, mode); mode--);
				if (mode >= 0) {
					context->enc_mode = (switch_byte_t) mode;
				}
			} else if (!strcasecmp(cmd, "default")) {
				context->enc_mode = amrwb_clamp_mode(context, globals.default_bitrate);
			} else {
				context->enc_mode = amrwb_clamp_mode(context, 0);
			}

			if (debug || context->debug) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "AMRWB encoder: Adjusting mode to %d (%s)\n", context->enc_mode, cmd);
			}
		}
		break;
	default:
		break;
	}

	return SWITCH_STATUS_SUCCESS;
}
#endif

#ifndef AMRWB_PASSTHROUGH
static int extract_octet_align(const char *fmtp)
{
	int oa = 0;
	int argc;
	char *argv[SWITCH_AMRWB_MAX_FMTP_PARAMS];
	char *fmtp_dup;

	if (zstr(fmtp)) return oa;

	fmtp_dup = strdup(fmtp);
	if (!fmtp_dup) return oa;

	argc = switch_separate_string(fmtp_dup, ';', argv, (int)(sizeof(argv) / sizeof(argv[0])));
	for (int i = 0; i < argc; ++i) {
		char *data = argv[i];
		char *arg;
		while (*data == ' ') data++;
		arg = strchr(data, '=');
		if (arg) {
			*arg++ = '\0';
			amrwb_trim_key(data);
			while (*arg == ' ') arg++;
			if (!strcasecmp(data, "octet-align")) {
				oa = switch_true(arg);
			}
		}
	}

	switch_safe_free(fmtp_dup);
	return oa;
}

/* crc, robust-sorting and interleaving are not supported, nor a mode-set without a speech mode
 * 0-8 (RFC 4867 8.3.1: return the mode-set unmodified or reject; other entries of a mode-set
 * are dropped instead). More than one channel is stated in a=rtpmap, which the core does not
 * pass here; "channels" in the fmtp is not a valid parameter and is rejected as well */
static switch_bool_t has_unsupported_option(const char *fmtp)
{
	switch_bool_t unsupported = SWITCH_FALSE;
	int argc, m_argc, y, modes;
	char *argv[SWITCH_AMRWB_MAX_FMTP_PARAMS], *m_argv[SWITCH_AMRWB_MAX_FMTP_PARAMS];
	char *fmtp_dup;

	if (zstr(fmtp) || !(fmtp_dup = strdup(fmtp))) return SWITCH_FALSE;

	argc = switch_separate_string(fmtp_dup, ';', argv, (int)(sizeof(argv) / sizeof(argv[0])));
	for (int i = 0; i < argc && !unsupported; ++i) {
		char *data = argv[i];
		char *arg;
		while (*data == ' ') data++;
		if (!(arg = strchr(data, '='))) continue;
		*arg++ = '\0';
		amrwb_trim_key(data);
		while (*arg == ' ') arg++;
		if (!strcasecmp(data, "crc") || !strcasecmp(data, "robust-sorting") || !strcasecmp(data, "interleaving")) {
			unsupported = atoi(arg) != 0;
		} else if (!strcasecmp(data, "channels")) {
			unsupported = atoi(arg) > 1;
		} else if (!strcasecmp(data, "mode-set") && *arg) {
			m_argc = switch_separate_string(arg, ',', m_argv, (int)(sizeof(m_argv) / sizeof(m_argv[0])));
			for (y = 0, modes = 0; y < m_argc; y++) {
				modes += amrwb_parse_mode(m_argv[y]) >= 0;
			}
			unsupported = !modes;
		}
	}

	switch_safe_free(fmtp_dup);
	return unsupported;
}

static switch_status_t matches_fmtp(const char *fmtp, const char *codec_fmtp)
{
	int oa1 = extract_octet_align(fmtp);
	int oa2 = extract_octet_align(codec_fmtp);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "AMRWB fmtp: %s, codec_fmtp: %s\n", switch_str_nil(fmtp), switch_str_nil(codec_fmtp));

	if (has_unsupported_option(fmtp) || has_unsupported_option(codec_fmtp)) {
		return SWITCH_STATUS_FALSE;
	}

	return (oa1 == oa2) ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}
#endif

static char *generate_fmtp(switch_memory_pool_t *pool , int octet_align)
{
	char buf[SWITCH_AMRWB_FMTP_SIZE] = { 0 };
#ifndef AMRWB_PASSTHROUGH
	int i = 0, j =0;
#endif

	snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "octet-align=%d; ", octet_align);

#ifndef AMRWB_PASSTHROUGH
	if (globals.enc_modes && !(globals.mode_set_overwrite && globals.mode_set_overwrite_with_default_bitrate)) {
			for (i = 0; SWITCH_AMRWB_MODES-1 > i; ++i) {
				if (globals.enc_modes & (1 << i)) {
					j++;
					snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), j > 1 ? ",%d" : "mode-set=%d", i);
				}
			}
	} else {
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "mode-set=%d", globals.default_bitrate);
	}
	snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "; ");

	if (globals.volte) {
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "max-red=0; mode-change-capability=2; ");
	}
#endif

	if (!zstr(globals.fmtp_extra)) {
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s", globals.fmtp_extra);
	}

	if (end_of(buf) == ' ') {
		*(end_of_p(buf) - 1) = '\0';
	}

	return switch_core_strdup(pool, buf);
}

#ifndef AMRWB_PASSTHROUGH

#define AMRWB_DEBUG_SYNTAX "<on|off>"
SWITCH_STANDARD_API(mod_amrwb_debug)
{
	if (zstr(cmd)) {
		stream->write_function(stream, "-USAGE: %s\n", AMRWB_DEBUG_SYNTAX);
	} else {
		if (!strcasecmp(cmd, "on")) {
			switch_atomic_set(&global_debug, 1);
			stream->write_function(stream, "AMRWB Debug: on\n");
		} else if (!strcasecmp(cmd, "off")) {
			switch_atomic_set(&global_debug, 0);
			stream->write_function(stream, "AMRWB Debug: off\n");
		} else {
			stream->write_function(stream, "-USAGE: %s\n", AMRWB_DEBUG_SYNTAX);
		}
	}
	return SWITCH_STATUS_SUCCESS;
}
#endif

static void mod_amrwb_configuration_snprintf(void) {
	char modes[100] = { 0 };
	int i = 0, j = 0;
	int debug = (int) switch_atomic_read(&global_debug);

	snprintf(modes + strlen(modes), sizeof(modes) - strlen(modes), "[");
	for (i = 0; SWITCH_AMRWB_MODES-1 > i; ++i) {
		if (globals.enc_modes & (1 << i)) {
			j++;
			snprintf(modes + strlen(modes), sizeof(modes) - strlen(modes), j > 1 ? ",%d" : "%d", i);
		}
	}
	snprintf(modes + strlen(modes), sizeof(modes) - strlen(modes), "]");

	snprintf(AMRWB_CONFIGURATION, sizeof(AMRWB_CONFIGURATION),
			"modes: %s, "
			"mode-set-overwrite: %d, "
			"mode-set-overwrite-with-default-bitrate: %d, "
			"default-bitrate: %d, "
			"volte: %d, "
			"adjust-bitrate: %d, "
			"force-oa: %d, "
			"force-be: %d, "
			"invite-prefer-oa: %d, "
			"invite-prefer-be: %d, "
			"fmtp-extra: [%s], "
			"debug: %d, "
			"silence-supp-off: %d\n",
			modes,
			globals.mode_set_overwrite,
			globals.mode_set_overwrite_with_default_bitrate,
			globals.default_bitrate,
			globals.volte,
			globals.adjust_bitrate,
			globals.force_oa,
			globals.force_be,
			globals.invite_prefer_oa,
			globals.invite_prefer_be,
			!zstr(globals.fmtp_extra) ? globals.fmtp_extra : "",
			debug,
			globals.silence_supp_off
	);
}

#define AMRWB_SHOW_SYNTAX ""
SWITCH_STANDARD_API(mod_amrwb_show)
{
	if (stream && stream->write_function) {
		/* AMRWB_CONFIGURATION is shared */
		switch_mutex_lock(global_lock);
		mod_amrwb_configuration_snprintf();
		stream->write_function(stream, "%s", AMRWB_CONFIGURATION);
		switch_mutex_unlock(global_lock);
	}
	return SWITCH_STATUS_SUCCESS;
}

/* Registration */
SWITCH_MODULE_LOAD_FUNCTION(mod_amrwb_load)
{
	switch_codec_interface_t *codec_interface;
	char *default_fmtp_oa = NULL;
	char *default_fmtp_be = NULL;
	switch_api_interface_t *commands_api_interface;

#ifndef AMRWB_PASSTHROUGH
	char *cf = "amrwb.conf";
	switch_xml_t cfg, xml, settings, param;

	memset(&globals, 0, sizeof(globals));
	switch_atomic_set(&global_debug, 0);
	globals.default_bitrate = SWITCH_AMRWB_DEFAULT_BITRATE;
	globals.mode_set_overwrite_with_default_bitrate = 1;

	if ((xml = switch_xml_open_cfg(cf, &cfg, NULL))) {
		if ((settings = switch_xml_child(cfg, "settings"))) {
			for (param = switch_xml_child(settings, "param"); param; param = param->next) {
				char *var = (char *) switch_xml_attr_soft(param, "name");
				char *val = (char *) switch_xml_attr_soft(param, "value");
				if (!strcasecmp(var, "default-bitrate")) {
					int mode = amrwb_parse_mode(val);

					if (mode < 0) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "AMRWB: invalid default-bitrate %s, keeping %d\n", val, globals.default_bitrate);
					} else {
						globals.default_bitrate = (switch_byte_t) mode;
					}
				}
				if (!strcasecmp(var, "volte")) {
					/* 3GPP TS 26.114 (MTSI) */
					globals.volte = (switch_byte_t) switch_true(val);
				}
				if (!strcasecmp(var, "adjust-bitrate")) {
					globals.adjust_bitrate = (switch_byte_t) switch_true(val);
				}
				if (!strcasecmp(var, "force-oa")) {
					globals.force_oa = (switch_byte_t) switch_true(val);
				}
				if (!strcasecmp(var, "force-be")) {
					globals.force_be = (switch_byte_t) switch_true(val);
				}
				if (!strcasecmp(var, "mode-set-overwrite")) {
					globals.mode_set_overwrite = (switch_byte_t) switch_true(val);
				}
				if (!strcasecmp(var, "mode-set-overwrite-with-default-bitrate")) {
					globals.mode_set_overwrite_with_default_bitrate = (switch_byte_t) switch_true(val);
				}
				if (!strcasecmp(var, "invite-prefer-oa")) {
					globals.invite_prefer_oa = (switch_byte_t) switch_true(val);
				}
				if (!strcasecmp(var, "invite-prefer-be")) {
					globals.invite_prefer_be = (switch_byte_t) switch_true(val);
				}
				if (!strcasecmp(var, "mode-set")) {
					int y, m_argc;
					char *m_argv[SWITCH_AMRWB_MAX_FMTP_PARAMS];
					/* split a copy: val belongs to the shared XML tree */
					char *modes = switch_core_strdup(pool, val);

					m_argc = switch_separate_string(modes, ',', m_argv, (sizeof(m_argv) / sizeof(m_argv[0])));
					for (y = 0; y < m_argc; y++) {
						int mode = amrwb_parse_mode(m_argv[y]);

						if (mode < 0) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "AMRWB: invalid mode-set entry '%s' ignored\n", m_argv[y]);
							continue;
						}
						globals.enc_modes |= (1 << mode);
					}
				}
				if (!strcasecmp(var, "debug")) {
					switch_atomic_set(&global_debug, switch_true(val) ? 1 : 0);
				}
				if (!strcasecmp(var, "fmtp-extra")) {
					globals.fmtp_extra = switch_core_strdup(pool, val);
					switch_assert(globals.fmtp_extra);
				}
				if (!strcasecmp(var, "silence-supp-off")) {
					globals.silence_supp_off = (switch_byte_t) switch_true(val);
				}
			}
		}
	}

	if (xml) {
		switch_xml_free(xml);
	}
#endif

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

#ifndef AMRWB_PASSTHROUGH
	SWITCH_ADD_API(commands_api_interface, "amrwb_debug", "Set AMR-WB Debug", mod_amrwb_debug, AMRWB_DEBUG_SYNTAX);

	switch_console_set_complete("add amrwb_debug on");
	switch_console_set_complete("add amrwb_debug off");
#endif
	SWITCH_ADD_API(commands_api_interface, "amrwb_show", "Show AMR-WB configuration", mod_amrwb_show, AMRWB_SHOW_SYNTAX);

	SWITCH_ADD_CODEC(codec_interface, "AMR-WB / Octet Aligned");
	codec_interface->parse_fmtp = amrwb_parse_fmtp_cb;

	default_fmtp_oa = generate_fmtp(pool, 1);

	switch_core_codec_add_implementation(pool, codec_interface,
										 SWITCH_CODEC_TYPE_AUDIO, 100, "AMR-WB", default_fmtp_oa,
										 16000, 16000, 23850, 20000, 320, 640, 0, 1, 1,
										 switch_amrwb_init, switch_amrwb_encode, switch_amrwb_decode, switch_amrwb_destroy);
#ifndef AMRWB_PASSTHROUGH
	codec_interface->implementations->codec_control = switch_amrwb_control;
	codec_interface->implementations->matches_fmtp = matches_fmtp;
#endif

//	SWITCH_ADD_CODEC(codec_interface, "AMR-WB / Bandwidth Efficient");
	codec_interface->parse_fmtp = amrwb_parse_fmtp_cb;

	default_fmtp_be = generate_fmtp(pool, 0);

	switch_core_codec_add_implementation(pool, codec_interface,
										 SWITCH_CODEC_TYPE_AUDIO, 110, "AMR-WB", default_fmtp_be,
										 16000, 16000, 23850, 20000, 320, 640, 0, 1, 1,
										 switch_amrwb_init, switch_amrwb_encode, switch_amrwb_decode, switch_amrwb_destroy);
#ifndef AMRWB_PASSTHROUGH
	codec_interface->implementations->codec_control = switch_amrwb_control;
	codec_interface->implementations->matches_fmtp = matches_fmtp;
#endif

	switch_mutex_init(&global_lock, SWITCH_MUTEX_NESTED, pool);
	mod_amrwb_configuration_snprintf();
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "AMRWB config: %s", AMRWB_CONFIGURATION);

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_amrwb_unload) {
	switch_mutex_destroy(global_lock);
	global_lock = NULL;
	return SWITCH_STATUS_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
