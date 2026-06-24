/*
 * TELCORE-226 reproducer: data race / crash in libspandsp t38_gateway.
 *
 * In mod_spandsp gateway mode a single t38_gateway_state_t is driven by TWO
 * FreeSWITCH session threads with no mutex:
 *
 *   - audio leg  (t38_gateway_on_consume_media): t38_gateway_rx()
 *   - T.38 leg   (t38_gateway_on_soft_execute):  udptl_rx_packet()
 *                                                  -> t38_core_rx_ifp_packet()
 *
 * The T.38 leg, on receiving a V.21 control frame (e.g. CFR) or carrier
 * transition, calls restart_rx_modem() which does:
 *
 *     hdlc_rx_init(&t->hdlc_rx, ...):  memset(s,0,sizeof *s); ... s->frame_user_data = user_data;
 *     fsk_rx_init(&t->v21_rx,  ...):  memset of the V.21 receiver
 *
 * ...reinitialising the exact hdlc_rx / v21_rx structs that the audio leg is
 * concurrently reading inside t38_gateway_rx() -> fsk_rx() ->
 * report_status_change() -> hdlc_rx_status(), which reads t->frame_user_data
 * and dereferences it in span_log(&s->logging, ...). A torn read of
 * frame_user_data yields a non-NULL garbage pointer -> SIGSEGV in
 * span_log_test() reading s->level. That is the crash in TELCORE-226.
 *
 * Build with ThreadSanitizer to observe the race directly:
 *     see run.sh
 *
 * This program is standalone: it links libspandsp only, no FreeSWITCH.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>

#include <spandsp.h>

/* T.30 frame control field, internal (post bit-reverse) value. */
#ifndef T30_CFR
#define T30_CFR 0x84
#endif

static volatile int g_saw_restart = 0;

/* Capture spandsp log lines so we can confirm the CFR / modem-restart path
 * is actually exercised by the crafted IFP packets. */
static void log_message(void *user_data, int level, const char *text)
{
    (void) user_data;
    (void) level;
    if (strstr(text, "Restart rx modem") || strstr(text, "CFR"))
        g_saw_restart = 1;
    /* Uncomment for verbose tracing:
    fputs(text, stderr);
    */
}

/* The T.38 tx packet handler is invoked when the gateway emits T.38 toward
 * the (imaginary) far end. We just drop the packets. */
static int tx_packet_handler(t38_core_state_t *s, void *user_data,
                             const uint8_t *buf, int len, int count)
{
    (void) s; (void) user_data; (void) buf; (void) len; (void) count;
    return 0;
}

/* Build a V.21 IFP data packet (T.38 version 0 encoding) carrying one HDLC
 * frame field followed by an FCS_OK field, so process_rx_data() ->
 * monitor_control_messages() -> restart_rx_modem() fires for a CFR frame.
 *
 * Wire bytes of the HDLC frame: 0xFF 0x03 0x21  (address, control, CFR FCF).
 * spandsp bit-reverses them internally: 0x21 -> 0x84 == T30_CFR.
 */
static int build_cfr_ifp(uint8_t *out)
{
    int n = 0;
    /* type-of-msg octet:
     *   bit7 data_field_present = 1
     *   bit6 type = T38_TYPE_OF_MSG_T30_DATA (1)
     *   bit5 extension = 0
     *   bits1-4 data_type = T38_DATA_V21 (0)
     */
    out[n++] = 0x80 | 0x40 | (T38_DATA_V21 << 1);
    out[n++] = 2;                       /* number of fields */

    /* Field 0: HDLC_DATA with 3 octets of data (version-0 field encoding). */
    out[n++] = 0x80 | (T38_FIELD_HDLC_DATA << 4);  /* data present, field type */
    out[n++] = 0x00;                    /* field length hi: numocts-1 = 2 */
    out[n++] = 0x02;                    /* field length lo */
    out[n++] = 0xFF;                    /* HDLC address */
    out[n++] = 0x03;                    /* HDLC control */
    out[n++] = 0x21;                    /* CFR FCF (bit-reversed -> 0x84) */

    /* Field 1: HDLC_FCS_OK, no data field (version-0 "other half" octet). */
    out[n++] = (T38_FIELD_HDLC_FCS_OK << 4);
    return n;
}

/* Two single-byte indicator IFP packets (no data field):
 *   type=indicator(0), bits1-4 = indicator value. */
static const uint8_t ind_v21_preamble[1] = { (T38_IND_V21_PREAMBLE << 1) };
static const uint8_t ind_no_signal[1]    = { (T38_IND_NO_SIGNAL << 1) };

#define ITERATIONS 200000

static t38_gateway_state_t *gw;
static t38_core_state_t *core;

/* Audio leg: exactly what t38_gateway_on_consume_media does. */
static void *audio_thread(void *arg)
{
    (void) arg;
    int16_t amp[160];
    int i;
    /* Non-silent input keeps the V.21 receiver alive so it generates the
     * status-change callbacks that read hdlc_rx.frame_user_data. */
    for (i = 0; i < 160; i++)
        amp[i] = (int16_t) ((i * 1373) ^ (i << 7));

    for (i = 0; i < ITERATIONS; i++) {
        t38_gateway_rx(gw, amp, 160);
        int16_t out[160];
        t38_gateway_tx(gw, out, 160);
    }
    return NULL;
}

/* T.38 leg: exactly what t38_gateway_on_soft_execute does via udptl_rx_packet. */
static void *t38_thread(void *arg)
{
    (void) arg;
    uint8_t cfr[32];
    int cfr_len = build_cfr_ifp(cfr);
    uint16_t seq = 0;
    int i;

    for (i = 0; i < ITERATIONS; i++) {
        /* Alternate carrier indicators and a CFR control frame; both drive
         * concurrent mutation of the gateway's rx-modem state. */
        t38_core_rx_ifp_packet(core, ind_v21_preamble, 1, seq++);
        t38_core_rx_ifp_packet(core, cfr, cfr_len, seq++);
        t38_core_rx_ifp_packet(core, ind_no_signal, 1, seq++);
    }
    return NULL;
}

int main(void)
{
    pthread_t a, b;

    gw = t38_gateway_init(NULL, tx_packet_handler, NULL);
    if (gw == NULL) {
        fprintf(stderr, "t38_gateway_init failed\n");
        return 2;
    }
    core = t38_gateway_get_t38_core_state(gw);
    t38_gateway_set_supported_modems(gw, T30_SUPPORT_V17 | T30_SUPPORT_V29 | T30_SUPPORT_V27TER);
    t38_gateway_set_ecm_capability(gw, true);
    t38_set_t38_version(core, 0);
    t38_set_sequence_number_handling(core, false);

    span_log_set_message_handler(t38_gateway_get_logging_state(gw), log_message, NULL);
    span_log_set_message_handler(t38_core_get_logging_state(core), log_message, NULL);
    span_log_set_level(t38_gateway_get_logging_state(gw),
                       SPAN_LOG_SHOW_SEVERITY | SPAN_LOG_SHOW_PROTOCOL | SPAN_LOG_FLOW);
    span_log_set_level(t38_core_get_logging_state(core),
                       SPAN_LOG_SHOW_SEVERITY | SPAN_LOG_SHOW_PROTOCOL | SPAN_LOG_FLOW);

    fprintf(stderr, "Driving one t38_gateway_state from 2 threads (%d iters each)...\n", ITERATIONS);

    pthread_create(&a, NULL, audio_thread, NULL);
    pthread_create(&b, NULL, t38_thread, NULL);
    pthread_join(a, NULL);
    pthread_join(b, NULL);

    fprintf(stderr, "Done. restart_rx_modem/CFR path exercised: %s\n",
            g_saw_restart ? "yes" : "no");
    return 0;
}
