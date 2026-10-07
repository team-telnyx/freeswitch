/*
 * Regression test: the libspandsp T.38 gateway must not hold back the messages
 * of the calling fax terminal while it plays the calling tone (CNG).
 *
 * In mod_spandsp gateway mode one t38_gateway_state_t is driven like this:
 *
 *   - audio leg  (t38_gateway_on_consume_media), for each 20 ms audio frame:
 *                    t38_gateway_rx(), then t38_gateway_tx()
 *   - T.38 leg   (t38_gateway_on_soft_execute), for each UDPTL packet:
 *                    udptl_rx_packet() -> t38_core_rx_ifp_packet()
 *
 * This program makes the same calls from one thread, so each run gives the
 * same result.
 *
 * The fault: the answering terminal starts its DIS (V.21) before the T.38
 * session starts, so the gateway finds the V.21 flags before the calling
 * terminal sends its CNG indicator. The gateway queues every T.38 indicator
 * and frame, and takes the next item from the queue only when the current
 * transmit handler returns a short buffer. The CNG generator never does. The
 * only thing that stops CNG is "V.21 flags found on the audio side while CNG
 * is in progress", and that already happened. So the reply of the calling
 * terminal (TSI, DCS, training check) waits until the answering terminal sends
 * its DIS again, 11.5 s later.
 *
 * The test makes the call again and again, and changes only the times: the
 * start of the DIS, in 20 ms steps from 2 s before to 1 s after the start of
 * the gateway, and the CNG indicator, at 150, 225 and 750 ms. The answering
 * terminal sends its DIS again 11.5 s after the first one, and the calling
 * terminal answers the first DIS it receives 530 ms later. The contents of the
 * DIS have no effect on the result. It prints the ranges of DIS start that
 * give each result.
 *
 * Exit status: 0 when the DCS is relayed in time in all runs, 1 when it is
 * not, 2 when the harness itself has a problem. -v prints every log line of
 * the gateway.
 *
 * This program links libspandsp only, no FreeSWITCH. To run it against another
 * libspandsp build:
 *
 *   cc -Wall -Wextra -I<prefix>/include test_t38_gateway_cng.c -L<prefix>/lib -lspandsp -lm -o t
 *   LD_LIBRARY_PATH=<prefix>/lib ./t
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <spandsp.h>

#define SAMPLES_PER_TICK	160		/* 20 ms at 8000 samples/s, as mod_spandsp */
#define MS_PER_TICK		20
#define LEAD_IN_MS		220		/* audio that precedes the start of the gateway */
#define RELAY_LIMIT_MS		1000		/* a correct relay needs about 350 ms */
#define MAX_EVENTS		256
#define MAX_BURSTS		2
#define PREAMBLE_FLAGS		44		/* about 1.2 s of V.21 flags */

/* The start of the DIS, in ms from the start of the gateway. */
#define SWEEP_FIRST_MS		-2000
#define SWEEP_LAST_MS		1000
#define SWEEP_STEP_MS		MS_PER_TICK
#define SWEEP_RUNS		((SWEEP_LAST_MS - SWEEP_FIRST_MS) / SWEEP_STEP_MS + 1)
#define SWEEP_RUN_MS		20000
#define DIS_REPEAT_MS		11520		/* the answering terminal sends its DIS again 11.5 s later */
#define SWEEP_CNG_TIMES		3		/* see sweep_cng_ms */
#define REPLY_MS		530		/* the calling terminal answers a DIS 0.53 s after it receives it */

/* T.30 frame control fields, in the bit order libspandsp uses internally. */
#define FCF_DIS			0x80
#define FCF_TSI			0x42
#define FCF_DCS			0x82

/* Offsets inside one attempt of the calling terminal, from its V.21 preamble indicator. */
#define TSI_OFFSET_MS		1035
#define DCS_OFFSET_MS		1751
#define SIG_END_OFFSET_MS	2013
#define TCF_IND_OFFSET_MS	2089
#define TCF_DATA_OFFSET_MS	2320
#define TCF_PACKETS		38		/* 38 x 48 octets of zeros at 9600 bit/s = 1.5 s */
#define TCF_PACKET_MS		40
#define TCF_END_OFFSET_MS	3851

enum {
	EV_INDICATOR,
	EV_TSI,
	EV_DCS,
	EV_HDLC_SIG_END,
	EV_TCF_DATA,
	EV_TCF_END
};

typedef struct {
	int ms;
	int kind;
	int arg;
} event_t;

typedef struct {
	hdlc_tx_state_t *hdlc;
	fsk_tx_state_t *fsk;
	int stage;
	bool active;
} carrier_t;

/* DIS of the answering terminal: V.27ter, V.29 and V.17, fine resolution, 2-D coding, no ECM. */
static const uint8_t dis_frame[] = { 0xFF, 0x13, FCF_DIS, 0x00, 0xEE, 0x78 };
/* TSI of the calling terminal: 20 spaces. The contents have no effect on the gateway. */
static const uint8_t tsi_frame[] = { 0xFF, 0x03, FCF_TSI | 1,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
	0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20 };
/* DCS of the calling terminal: V.29 9600 bit/s, fine resolution, 2-D coding. */
static const uint8_t dcs_frame[] = { 0xFF, 0x13, FCF_DCS | 1, 0x00, 0xC6, 0x70 };

static const char verdict_ok[] = "OK";
static const char verdict_fault[] = "FAULT";
static const char verdict_error[] = "HARNESS ERROR";
static const char *verdict;
static const char *sweep_verdict[SWEEP_RUNS];
/* Times of the CNG indicator after the start of T.38. */
static const int sweep_cng_ms[SWEEP_CNG_TIMES] = { 150, 225, 750 };
static int sweep_cng_at_ms;

static t38_gateway_state_t *gw;
static t38_core_state_t *gw_core;	/* receive side of the gateway: packets from the calling terminal */
static t38_core_state_t *fax_core;	/* encoder for the packets of the calling terminal */
static t38_core_state_t *mon_core;	/* decoder for the packets the gateway sends to the calling terminal */
static uint16_t fax_seq;
static uint16_t mon_seq;

static event_t events[MAX_EVENTS];
static int n_events;
static int next_event;		/* first event not sent yet */
static int burst_onset_ms[MAX_BURSTS];
static int n_bursts;

static int now_ms;
static int verbose;
static int reply_due_ms = -1;

/* Results */
static int dcs_in_ms = -1;		/* first DCS handed to the gateway */
static int dcs_out_ms = -1;		/* first DCS found in the audio the gateway sent */
static int dis_relayed_ms = -1;		/* gateway relayed the DIS to the T.38 side */

static uint8_t mon_frame[256];
static int mon_frame_len;

static void log_message(void *user_data, int level, const char *text)
{
	(void) user_data;
	(void) level;
	if (verbose) {
		printf("  [%6d ms] %s", now_ms, text);
	}
}

/* ---- the answering terminal: V.21 audio toward the gateway ---- */

static void carrier_underflow(void *user_data)
{
	carrier_t *c = (carrier_t *) user_data;

	if (c->stage == 0) {
		/* The preamble is complete. Send the DIS. */
		hdlc_tx_frame(c->hdlc, dis_frame, sizeof(dis_frame));
		c->stage = 1;
	} else if (c->stage == 1) {
		/* The DIS is complete. Stop after the closing flags. */
		hdlc_tx_frame(c->hdlc, NULL, 0);
		c->stage = 2;
	}
}

static int carrier_start_burst(carrier_t *c)
{
	if (!(c->hdlc = hdlc_tx_init(c->hdlc, false, 2, false, carrier_underflow, c))) {
		return -1;
	}
	hdlc_tx_flags(c->hdlc, PREAMBLE_FLAGS);
	if (!(c->fsk = fsk_tx_init(c->fsk, &preset_fsk_specs[FSK_V21CH2], (get_bit_func_t) hdlc_tx_get_bit, c->hdlc))) {
		return -1;
	}
	c->stage = 0;
	c->active = true;
	return 0;
}

static void carrier_audio(carrier_t *c, int16_t amp[], int len)
{
	int n = 0;

	if (c->active) {
		n = fsk_tx(c->fsk, amp, len);
		if (n < len) {
			c->active = false;
		}
	}
	memset(&amp[n], 0, (len - n) * sizeof(int16_t));
}

/* ---- the calling terminal: T.38 packets toward the gateway ---- */

static int fax_tx_packet_handler(t38_core_state_t *s, void *user_data, const uint8_t *buf, int len, int count)
{
	(void) s;
	(void) user_data;
	(void) count;
	/* This is what mod_spandsp does with each new UDPTL packet. */
	return t38_core_rx_ifp_packet(gw_core, buf, len, fax_seq++);
}

/* t38_core calls this one without a NULL check; the other handlers that the test
   does not need are NULL, because the core never calls them here. */
static int unused_rx_missing(t38_core_state_t *s, void *user_data, int rx_seq_no, int expected_seq_no)
{
	(void) s;
	(void) user_data;
	(void) rx_seq_no;
	(void) expected_seq_no;
	return 0;
}

/* Send one complete HDLC frame plus "FCS OK" in one packet, as the calling terminal does. */
static void fax_send_frame(const uint8_t *frame, int len)
{
	uint8_t wire[64];
	t38_data_field_t field[2];

	if (len > (int) sizeof(wire)) {
		return;
	}
	/* T.38 carries each octet with the bit order reversed. */
	bit_reverse(wire, frame, len);
	field[0].field_type = T38_FIELD_HDLC_DATA;
	field[0].field = wire;
	field[0].field_len = len;
	field[1].field_type = T38_FIELD_HDLC_FCS_OK;
	field[1].field = NULL;
	field[1].field_len = 0;
	t38_core_send_data_multi_field(fax_core, T38_DATA_V21, field, 2, T38_PACKET_CATEGORY_CONTROL_DATA);
}

static void fax_send(const event_t *ev)
{
	static const uint8_t zeros[48] = { 0 };

	switch (ev->kind) {
	case EV_INDICATOR:
		/* 0x100 forces the packet out when the indicator is the same as the last one. */
		t38_core_send_indicator(fax_core, ev->arg | 0x100);
		break;
	case EV_TSI:
		fax_send_frame(tsi_frame, sizeof(tsi_frame));
		break;
	case EV_DCS:
		if (dcs_in_ms < 0) {
			dcs_in_ms = now_ms;
		}
		fax_send_frame(dcs_frame, sizeof(dcs_frame));
		break;
	case EV_HDLC_SIG_END:
		t38_core_send_data(fax_core, T38_DATA_V21, T38_FIELD_HDLC_SIG_END, NULL, 0, T38_PACKET_CATEGORY_CONTROL_DATA_END);
		break;
	case EV_TCF_DATA:
		t38_core_send_data(fax_core, T38_DATA_V29_9600, T38_FIELD_T4_NON_ECM_DATA, zeros, sizeof(zeros), T38_PACKET_CATEGORY_IMAGE_DATA);
		break;
	case EV_TCF_END:
		t38_core_send_data(fax_core, T38_DATA_V29_9600, T38_FIELD_T4_NON_ECM_SIG_END, NULL, 0, T38_PACKET_CATEGORY_IMAGE_DATA_END);
		break;
	}
}

/* Keeps the events not sent yet in time order, also for the attempts added while the call runs. */
static void add_event(int ms, int kind, int arg)
{
	int i;

	if (n_events < MAX_EVENTS) {
		for (i = n_events; i > next_event && events[i - 1].ms > ms; --i) {
			events[i] = events[i - 1];
		}
		events[i].ms = ms;
		events[i].kind = kind;
		events[i].arg = arg;
		++n_events;
	}
}

/* One attempt of the calling terminal: V.21 preamble, TSI, DCS, then the training check. */
static void add_attempt(int ms)
{
	int i;

	add_event(ms, EV_INDICATOR, T38_IND_V21_PREAMBLE);
	add_event(ms + TSI_OFFSET_MS, EV_TSI, 0);
	add_event(ms + DCS_OFFSET_MS, EV_DCS, 0);
	add_event(ms + SIG_END_OFFSET_MS, EV_HDLC_SIG_END, 0);
	add_event(ms + TCF_IND_OFFSET_MS, EV_INDICATOR, T38_IND_V29_9600_TRAINING);
	for (i = 0; i < TCF_PACKETS; ++i) {
		add_event(ms + TCF_DATA_OFFSET_MS + i * TCF_PACKET_MS, EV_TCF_DATA, 0);
	}
	add_event(ms + TCF_END_OFFSET_MS, EV_TCF_END, 0);
}

/* Times are in ms from the start of the gateway, which is the start of the T.38 session.
   The DIS starts at dis_start_ms, and again 11.5 s later. The calling terminal sends CNG,
   then answers the first DIS it receives (see mon_rx_data). */
static void build_script(int dis_start_ms)
{
	add_event(22, EV_INDICATOR, T38_IND_NO_SIGNAL);
	burst_onset_ms[n_bursts++] = dis_start_ms;
	burst_onset_ms[n_bursts++] = dis_start_ms + DIS_REPEAT_MS;
	add_event(sweep_cng_at_ms, EV_INDICATOR, T38_IND_CNG);
}

/* ---- what the gateway sends to the calling terminal ---- */

static int gw_tx_packet_handler(t38_core_state_t *s, void *user_data, const uint8_t *buf, int len, int count)
{
	(void) s;
	(void) user_data;
	(void) count;
	t38_core_rx_ifp_packet(mon_core, buf, len, mon_seq++);
	return 0;
}

static int mon_rx_indicator(t38_core_state_t *s, void *user_data, int indicator)
{
	(void) s;
	(void) user_data;
	if (verbose && indicator == T38_IND_V21_PREAMBLE) {
		printf("  [%6d ms] gateway -> T.38 side: V.21 preamble (flags found on the audio side)\n", now_ms);
	}
	return 0;
}

static int mon_rx_data(t38_core_state_t *s, void *user_data, int data_type, int field_type, const uint8_t *buf, int len)
{
	(void) s;
	(void) user_data;
	if (data_type != T38_DATA_V21) {
		return 0;
	}
	if (field_type == T38_FIELD_HDLC_DATA) {
		if (len > 0 && mon_frame_len + len <= (int) sizeof(mon_frame)) {
			memcpy(&mon_frame[mon_frame_len], buf, len);
			mon_frame_len += len;
		}
	} else {
		if ((field_type == T38_FIELD_HDLC_FCS_OK || field_type == T38_FIELD_HDLC_FCS_OK_SIG_END) &&
			mon_frame_len >= 3 && bit_reverse8(mon_frame[2]) == FCF_DIS) {
			if (verbose) {
				printf("  [%6d ms] gateway -> T.38 side: DIS relayed\n", now_ms);
			}
			if (dis_relayed_ms < 0) {
				/* The calling terminal answers the first DIS it receives. */
				dis_relayed_ms = now_ms;
				reply_due_ms = now_ms + REPLY_MS;
			}
		}
		mon_frame_len = 0;
	}
	return 0;
}

/* ---- what the gateway sends to the answering terminal ---- */

static void out_frame_handler(void *user_data, const uint8_t *pkt, int len, int ok)
{
	(void) user_data;
	if (len < 3 || !ok || (pkt[2] & 0xFE) != FCF_DCS) {
		return;		/* a status report, not a frame, or not the DCS */
	}
	if (verbose) {
		printf("  [%6d ms] gateway -> audio side: DCS\n", now_ms);
	}
	if (dcs_out_ms < 0) {
		dcs_out_ms = now_ms;
	}
}

static void reset_state(void)
{
	gw = NULL;
	gw_core = NULL;
	fax_core = NULL;
	mon_core = NULL;
	fax_seq = 0;
	mon_seq = 0;
	n_events = 0;
	next_event = 0;
	n_bursts = 0;
	dcs_in_ms = -1;
	dcs_out_ms = -1;
	dis_relayed_ms = -1;
	mon_frame_len = 0;
	reply_due_ms = -1;
	verdict = verdict_error;
}

/* Returns 0 when the DCS is relayed in time, 1 when it is not, 2 for a harness problem. */
static int run_script(int dis_start_ms)
{
	carrier_t carrier = { NULL, NULL, 0, false };
	hdlc_rx_state_t *out_hdlc = NULL;
	fsk_rx_state_t *out_fsk = NULL;
	int16_t in[SAMPLES_PER_TICK];
	int16_t out[SAMPLES_PER_TICK];
	int next_burst = 0;
	int start_ms = -LEAD_IN_MS;
	int len;
	int rc = 2;

	reset_state();
	build_script(dis_start_ms);
	if (burst_onset_ms[0] < start_ms) {
		start_ms = burst_onset_ms[0];
	}

	/* The gateway, set up as mod_spandsp does for "t38_gateway" with transferredTCF. */
	if (!(gw = t38_gateway_init(NULL, gw_tx_packet_handler, NULL))) {
		fprintf(stderr, "t38_gateway_init failed\n");
		goto done;
	}
	gw_core = t38_gateway_get_t38_core_state(gw);
	t38_gateway_set_transmit_on_idle(gw, true);
	t38_gateway_set_supported_modems(gw, T30_SUPPORT_V17 | T30_SUPPORT_V29 | T30_SUPPORT_V27TER);
	t38_gateway_set_tep_mode(gw, false);
	t38_gateway_set_ecm_capability(gw, true);
	t38_set_t38_version(gw_core, 0);
	t38_set_data_rate_management_method(gw_core, T38_DATA_RATE_MANAGEMENT_TRANSFERRED_TCF);

	span_log_set_message_handler(t38_gateway_get_logging_state(gw), log_message, NULL);
	span_log_set_message_handler(t38_core_get_logging_state(gw_core), log_message, NULL);
	span_log_set_level(t38_gateway_get_logging_state(gw), SPAN_LOG_SHOW_SEVERITY | SPAN_LOG_SHOW_PROTOCOL | SPAN_LOG_FLOW);
	span_log_set_level(t38_core_get_logging_state(gw_core), SPAN_LOG_SHOW_SEVERITY | SPAN_LOG_SHOW_PROTOCOL | SPAN_LOG_FLOW);

	fax_core = t38_core_init(NULL, NULL, NULL, NULL, NULL, fax_tx_packet_handler, NULL);
	mon_core = t38_core_init(NULL, mon_rx_indicator, mon_rx_data, unused_rx_missing, NULL, NULL, NULL);
	out_hdlc = hdlc_rx_init(NULL, false, false, 5, out_frame_handler, NULL);
	if (out_hdlc) {
		out_fsk = fsk_rx_init(NULL, &preset_fsk_specs[FSK_V21CH2], FSK_FRAME_MODE_SYNC, (put_bit_func_t) hdlc_rx_put_bit, out_hdlc);
	}
	if (!fax_core || !mon_core || !out_hdlc || !out_fsk) {
		fprintf(stderr, "cannot set up the test harness\n");
		goto done;
	}
	t38_set_t38_version(fax_core, 0);
	t38_set_t38_version(mon_core, 0);
	t38_set_sequence_number_handling(mon_core, false);

	for (now_ms = start_ms; now_ms < SWEEP_RUN_MS; now_ms += MS_PER_TICK) {
		if (next_burst < n_bursts && now_ms >= burst_onset_ms[next_burst]) {
			if (carrier_start_burst(&carrier) < 0) {
				fprintf(stderr, "cannot start the V.21 transmitter\n");
				goto done;
			}
			++next_burst;
		}
		carrier_audio(&carrier, in, SAMPLES_PER_TICK);
		if (now_ms < 0) {
			continue;		/* the T.38 session, and so the gateway, has not started */
		}

		/* T.38 leg */
		if (reply_due_ms >= 0 && now_ms >= reply_due_ms) {
			add_attempt(reply_due_ms);
			reply_due_ms = -1;
		}
		while (next_event < n_events && events[next_event].ms <= now_ms) {
			fax_send(&events[next_event++]);
		}

		/* audio leg */
		t38_gateway_rx(gw, in, SAMPLES_PER_TICK);
		len = t38_gateway_tx(gw, out, SAMPLES_PER_TICK);
		if (len > 0) {
			fsk_rx(out_fsk, out, len);
		}
	}

	if (dcs_in_ms < 0) {
		verdict = verdict_error;
	} else if (dcs_out_ms < 0 || dcs_out_ms - dcs_in_ms > RELAY_LIMIT_MS) {
		verdict = verdict_fault;
		rc = 1;
	} else {
		verdict = verdict_ok;
		rc = 0;
	}

 done:
	if (out_fsk) {
		fsk_rx_free(out_fsk);
	}
	if (out_hdlc) {
		hdlc_rx_free(out_hdlc);
	}
	if (carrier.fsk) {
		fsk_tx_free(carrier.fsk);
	}
	if (carrier.hdlc) {
		hdlc_tx_free(carrier.hdlc);
	}
	if (mon_core) {
		t38_core_free(mon_core);
	}
	if (fax_core) {
		t38_core_free(fax_core);
	}
	if (gw) {
		t38_gateway_free(gw);
	}
	return rc;
}

/* Runs the call for each time of the CNG indicator and each start of the DIS, and
   prints the ranges of DIS start that give each result. */
int main(int argc, char *argv[])
{
	int rc = 0;
	int run_rc;
	int k;
	int i;
	int j;

	if (argc > 2 || (argc == 2 && strcmp(argv[1], "-v"))) {
		fprintf(stderr, "usage: %s [-v]\n", argv[0]);
		return 2;
	}
	verbose = (argc == 2);
	for (k = 0; k < SWEEP_CNG_TIMES; ++k) {
		sweep_cng_at_ms = sweep_cng_ms[k];
		printf("CNG indicator at %d ms, DIS start from %d to %d ms in %d ms steps (ms from the start of the gateway)\n",
			sweep_cng_at_ms, SWEEP_FIRST_MS, SWEEP_LAST_MS, SWEEP_STEP_MS);
		for (i = 0; i < SWEEP_RUNS; ++i) {
			run_rc = run_script(SWEEP_FIRST_MS + i * SWEEP_STEP_MS);
			sweep_verdict[i] = verdict;
			if (run_rc > rc) {
				rc = run_rc;
			}
		}
		for (i = 0; i < SWEEP_RUNS; i = j) {
			for (j = i + 1; j < SWEEP_RUNS && sweep_verdict[j] == sweep_verdict[i]; ++j) {
			}
			printf("  DIS start %6d to %6d ms: %s\n",
				SWEEP_FIRST_MS + i * SWEEP_STEP_MS, SWEEP_FIRST_MS + (j - 1) * SWEEP_STEP_MS, sweep_verdict[i]);
		}
		printf("\n");
	}
	printf("RESULT: %s\n", (rc == 0) ? "OK (no timing holds back the DCS)" : (rc == 1) ? "FAULT (some timings hold back the DCS)" : "HARNESS ERROR");
	return rc;
}
