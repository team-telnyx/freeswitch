/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2011, Anthony Minessale II <anthm@freeswitch.org>
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
 * Mathieu Rene <mrene@avgs.ca>
 *
 * rtp.c -- RTP Controllable Channel Module
 *
 */

#include <switch.h>
#include "mod_sofia.h"
#include "prometheus_metrics.h"

#define kLOCALADDR "local_addr"
#define kLOCALPORT "local_port"
#define kREMOTEADDR "remote_addr"
#define kREMOTEPORT "remote_port"
#define kCODEC "codec"
#define kPTIME "ptime"
#define kPT "pt"
#define kRFC2833PT "rfc2833_pt"
#define kMODE "mode"
#define kRATE "rate"

static struct {
    switch_memory_pool_t *pool;
    switch_endpoint_interface_t *endpoint_interface;
    char *default_local_ip;
} crtp;

typedef struct {
    switch_core_session_t *session;
    switch_channel_t *channel;
    switch_codec_t read_codec, write_codec;
    switch_frame_t read_frame;



    switch_rtp_bug_flag_t rtp_bugs;
    switch_rtp_t *rtp_session;

    uint32_t timestamp_send;

    const char *local_address;
    const char *remote_address;
    const char *codec;
    int ptime;

    const switch_codec_implementation_t *negotiated_codecs[SWITCH_MAX_CODECS];
	int num_negotiated_codecs;

    char *origin;

    switch_port_t local_port;
    switch_port_t remote_port;
    switch_payload_t agreed_pt; /*XXX*/
    switch_core_media_dtmf_t dtmf_type;
    enum {
        RTP_SENDONLY,
        RTP_RECVONLY,
        RTP_SENDRECV
    } mode;
} crtp_private_t;

static switch_status_t channel_on_init(switch_core_session_t *session);
static switch_status_t channel_on_destroy(switch_core_session_t *session);
static switch_call_cause_t channel_outgoing_channel(switch_core_session_t *session, switch_event_t *var_event,
													switch_caller_profile_t *outbound_profile,
													switch_core_session_t **new_session,
													switch_memory_pool_t **pool,
													switch_originate_flag_t flags, switch_call_cause_t *cancel_cause);
static switch_status_t channel_read_frame(switch_core_session_t *session, switch_frame_t **frame, switch_io_flag_t flags, int stream_id);
static switch_status_t channel_write_frame(switch_core_session_t *session, switch_frame_t *frame, switch_io_flag_t flags, int stream_id);
static switch_status_t channel_receive_message(switch_core_session_t *session, switch_core_session_message_t *msg);
static switch_status_t channel_send_dtmf(switch_core_session_t *session, const switch_dtmf_t *dtmf);
static switch_status_t channel_receive_event(switch_core_session_t *session, switch_event_t *event);

switch_state_handler_table_t crtp_state_handlers = {
	/*on_init */channel_on_init,
	/*on_routing */ NULL,
	/*on_execute */ NULL,
	/*on_hangup*/ NULL,
	/*on_exchange_media*/ NULL,
	/*on_soft_execute*/ NULL,
	/*on_consume_media*/ NULL,
	/*on_hibernate*/ NULL,
	/*on_reset*/ NULL,
	/*on_park*/ NULL,
	/*on_reporting*/ NULL,
	/*on_destroy*/ channel_on_destroy

};

switch_io_routines_t crtp_io_routines = {
	/*outgoing_channel*/ channel_outgoing_channel,
	/*read_frame*/ channel_read_frame,
	/*write_frame*/ channel_write_frame,
	/*kill_channel*/ NULL,
	/*send_dtmf*/ channel_send_dtmf,
	/*receive_message*/ channel_receive_message,
	/*receive_event*/ channel_receive_event,
	/*state_change*/ NULL,
	/*read_video_frame*/ NULL,
	/*write_video_frame*/ NULL,
	/*read_text_frame*/ NULL,
	/*write_text_frame*/ NULL,
	/*state_run*/ NULL


};

#define CRTP_MEDIA_MODIFY_SYNTAX "<uuid> <remote ip> <remote port>"
SWITCH_STANDARD_API(crtp_media_modify_function)
{
	switch_core_session_t *psession = NULL;
	char *mycmd = NULL, *argv[3] = { 0 };
	int argc = 0;

	if (!zstr(cmd) && (mycmd = strdup(cmd))) {
		argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
		if (argc == 3 && !(zstr(argv[0]) || zstr(argv[1]) || zstr(argv[2]))) {
			char *uuid = argv[0];
			char *ip = argv[1];
			char *port = argv[2];

			if ((psession = switch_core_session_locate(uuid))) {
				switch_event_t *event;
				if (switch_event_create(&event, SWITCH_EVENT_COMMAND) == SWITCH_STATUS_SUCCESS) {
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "command", "media_modify");
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, kREMOTEADDR, ip);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, kREMOTEPORT, port);
					if (switch_core_session_receive_event(psession, &event) != SWITCH_STATUS_SUCCESS) {
						switch_event_destroy(&event);
						stream->write_function(stream, "-ERR Send failed\n");
					} else {
						stream->write_function(stream, "+OK\n");
					}
				}
				switch_core_session_rwunlock(psession);
			} else {
				stream->write_function(stream, "-ERR No such channel %s!\n", uuid);
			}
			goto done;
		}
	}

	stream->write_function(stream, "-USAGE: %s\n", CRTP_MEDIA_MODIFY_SYNTAX);

done:

	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

static void crtp_config(void)
{
	switch_xml_t xml = NULL, cfg, param, settings;

	if (!(xml = switch_xml_open_cfg("sofia.conf", &cfg, NULL))) {
		goto done;
	}

	crtp.default_local_ip = NULL;

	if ((settings = switch_xml_child(cfg, "crtp_settings"))) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			char *var = (char *) switch_xml_attr_soft(param, "name");
			char *val = (char *) switch_xml_attr_soft(param, "value");
			if (!strcasecmp(var, "local-ip")) {
				char *ip = NULL;
				char buf[64] = { 0 };

				if (zstr(val)) {
					ip = mod_sofia_globals.guess_ip;
				} else if (!strcmp(val, "0.0.0.0")) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid IP 0.0.0.0 replaced with %s\n", mod_sofia_globals.guess_ip);
				} else if (!strncasecmp(val, "interface:", 10)) {
					char *ifname = val+10;
					int family = AF_UNSPEC;
					if (!strncasecmp(ifname, "auto/", 5)) { ifname += 5; family = AF_UNSPEC; }
					if (!strncasecmp(ifname, "ipv4/", 5)) { ifname += 5; family = AF_INET;   }
					if (!strncasecmp(ifname, "ipv6/", 5)) { ifname += 5; family = AF_INET6;  }
					if (switch_find_interface_ip(buf, sizeof(buf), NULL, ifname, family) == SWITCH_STATUS_SUCCESS) {
						ip = buf;
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Using %s IP for interface %s for rtp-ip\n", ip, val+10);
					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unknown IP for interface %s for rtp-ip\n", val+10);
					}
				} else {
					ip = strcasecmp(val, "auto") ? val : mod_sofia_globals.guess_ip;
				}

				if (!zstr(ip)) {
					crtp.default_local_ip = switch_core_strdup(crtp.pool, ip);
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "RTP Endpoint will be using default ip: %s\n", ip);
				}
			}
		}
	}

done:
    
	if (zstr(crtp.default_local_ip)) {
		crtp.default_local_ip = switch_core_strdup(crtp.pool, mod_sofia_globals.guess_ip);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "RTP Endpoint will be using default ip: %s\n", mod_sofia_globals.guess_ip);
	}

	if (xml) {
		switch_xml_free(xml);
	}
}

void crtp_init(switch_loadable_module_interface_t *mod_interface)
{
    switch_loadable_module_interface_t **module_interface = &mod_interface;
    switch_endpoint_interface_t *endpoint_interface;
    switch_api_interface_t *api_interface;

    crtp.pool = mod_interface->pool;
    crtp_config();

    endpoint_interface = switch_loadable_module_create_interface(mod_interface, SWITCH_ENDPOINT_INTERFACE);
    endpoint_interface->interface_name = "rtp";
    endpoint_interface->io_routines = &crtp_io_routines;
    endpoint_interface->state_handler = &crtp_state_handlers;
    crtp.endpoint_interface = endpoint_interface;

    //SWITCH_ADD_API(api_interface, "rtp_test", "test", test_function, "");
    SWITCH_ADD_API(api_interface, "crtp_media_modify", "Modify CRTP remote media address", crtp_media_modify_function, CRTP_MEDIA_MODIFY_SYNTAX);
    switch_console_set_complete("add crtp_media_modify ::console::list_uuid");

}

/* Create the RTP session for a crtp leg, retrying with a freshly allocated
 * port on transient bind failures.
 *
 * The underlying problem is a timing race rather than real port exhaustion:
 * when a previous session's pool has not yet been collected, its bound UDP
 * socket can outlive the release of its port back to the allocator, and
 * the next caller that picks the same port hits EADDRINUSE. Requesting a
 * different port and trying again is usually enough to escape the conflict.
 *
 * Only retries on bind-specific failures ("Bind Error" / "RTCP Bind Error"
 * from switch_rtp_set_local_address / enable_local_rtcp_socket) -- other
 * failures (missing args, socket create errors, etc.) are terminal and
 * surfaced immediately.
 *
 * Port lifecycle: switch_rtp_new always disposes of the port it was given
 * -- on success the new rtp_session owns it (and switch_rtp_destroy will
 * release it later); on failure switch_rtp_new calls
 * switch_rtp_release_port itself before returning NULL. This helper
 * therefore never needs to release a port on its own NULL-return paths,
 * and the caller must not release tech_pvt->local_port either (clearing
 * tech_pvt->local_port to 0 is the right pattern to keep
 * channel_on_destroy from double-releasing).
 *
 * Returns the new rtp_session on success (with tech_pvt->local_port
 * reflecting the actually-bound port -- it may have been updated mid-way
 * if a retry was needed). Returns NULL on failure with *err set.
 *
 * Retry budget defaults to 3. Override per call via the
 * "rtp_bind_max_retries" channel variable (0..10).
 */
static switch_rtp_t *create_rtp_session_with_bind_retry(crtp_private_t *tech_pvt,
                                                        switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID],
                                                        const char **err)
{
    switch_channel_t *channel = tech_pvt->channel;
    switch_rtp_t *rtp_session = NULL;
    int max_bind_retries = 3;
    int retry_count = 0;
    const char *var;

    var = switch_channel_get_variable(channel, "rtp_bind_max_retries");
    if (!zstr(var)) {
        char *end = NULL;
        long v;
        errno = 0;
        v = strtol(var, &end, 10);
        if (errno != 0 || end == var || *end != '\0') {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                              "rtp_bind_max_retries='%s' is not a valid integer, using default %d\n",
                              var, max_bind_retries);
        } else if (v < 0 || v > 10) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                              "rtp_bind_max_retries=%ld out of range [0,10], using default %d\n",
                              v, max_bind_retries);
        } else {
            max_bind_retries = (int)v;
        }
    }

    for (;;) {
        rtp_session = switch_rtp_new(tech_pvt->local_address, tech_pvt->local_port,
                                     tech_pvt->remote_address, tech_pvt->remote_port,
                                     tech_pvt->agreed_pt,
                                     tech_pvt->read_codec.implementation->samples_per_packet,
                                     tech_pvt->ptime * 1000,
                                     flags, "soft", err,
                                     switch_core_session_get_pool(tech_pvt->session));
        if (rtp_session) {
            return rtp_session;
        }

        /* switch_rtp_new released tech_pvt->local_port back to the
         * allocator before returning NULL -- do not release it again. */

        /* Match the full known error prefixes including the trailing '!'
         * so a future error message that happens to start with "Bind
         * Error" (e.g. "Bind ErrorValidation") is not silently retried. */
        if (!*err
            || (strncmp(*err, "Bind Error!", 11) != 0
                && strncmp(*err, "RTCP Bind Error!", 16) != 0)
            || retry_count >= max_bind_retries) {
            return NULL;
        }

        retry_count++;
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                          "RTP bind failure on %s:%d [%s], retrying with fresh port (attempt %d/%d)\n",
                          tech_pvt->local_address, tech_pvt->local_port, *err,
                          retry_count, max_bind_retries);

        {
            switch_port_t new_port = switch_rtp_request_port(tech_pvt->local_address);
            if (new_port == 0) {
                /* Previous iteration's port was already released by
                 * switch_rtp_new; no new port was obtained here, so
                 * nothing to release. Overwrite *err so the caller's
                 * log reflects the real reason for giving up instead
                 * of the previous iteration's bind error. */
                *err = "Port allocator exhausted during bind retry";
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                                  "RTP bind retry: port allocator returned no free port\n");
                return NULL;
            }
            tech_pvt->local_port = new_port;
            switch_channel_set_variable_printf(channel, "local_media_port", "%d", new_port);
        }
    }
}

static switch_call_cause_t channel_outgoing_channel(switch_core_session_t *session, switch_event_t *var_event,
													switch_caller_profile_t *outbound_profile,
													switch_core_session_t **new_session,
													switch_memory_pool_t **pool,
													switch_originate_flag_t flags, switch_call_cause_t *cancel_cause)
{
    switch_channel_t *channel;
    char name[128];
    crtp_private_t *tech_pvt = NULL;
    switch_caller_profile_t *caller_profile;
    switch_rtp_flag_t rtp_flags[SWITCH_RTP_FLAG_INVALID] = {0};

    const char *err = NULL;


    const char  *local_addr = switch_event_get_header_nil(var_event, kLOCALADDR),
                *remote_addr = switch_event_get_header_nil(var_event, kREMOTEADDR),
                *szremote_port = switch_event_get_header_nil(var_event, kREMOTEPORT),
                *codec  = switch_event_get_header_nil(var_event, kCODEC),
                *szptime  = switch_event_get_header_nil(var_event, kPTIME),
                *mode  = switch_event_get_header_nil(var_event, kMODE),
                //*szrfc2833_pt = switch_event_get_header_nil(var_event, kRFC2833PT),
                *szrate = switch_event_get_header_nil(var_event, kRATE),
                *szpt = switch_event_get_header_nil(var_event, kPT);


    switch_port_t local_port = 0;
    switch_port_t remote_port = !zstr(szremote_port) ? (switch_port_t)atoi(szremote_port) : 0;

    int ptime  = !zstr(szptime) ? atoi(szptime) : 0,
        //rfc2833_pt = !zstr(szrfc2833_pt) ? atoi(szrfc2833_pt) : 0,
        rate = !zstr(szrate) ? atoi(szrate) : 8000,
        pt = !zstr(szpt) ? atoi(szpt) : 0;

    // Track total RTP endpoint origination attempts
    prometheus_increment_rtp_outgoing_calls();

    if (zstr(local_addr)) {
        local_addr = crtp.default_local_ip;
    }

    if ((zstr(remote_addr) || remote_port == 0) && (zstr(local_addr) || zstr(codec) || zstr(szpt))) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Missing required arguments\n");
        goto fail;
    }

    local_port = switch_rtp_request_port(local_addr);

    if (local_port == 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to allocate local port\n");
        goto fail;
    }

    if (!(*new_session = switch_core_session_request(crtp.endpoint_interface, SWITCH_CALL_DIRECTION_OUTBOUND, 0, pool))) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't request session.\n");
        goto fail;
    }

    channel = switch_core_session_get_channel(*new_session);

    tech_pvt = switch_core_session_alloc(*new_session, sizeof *tech_pvt);
    tech_pvt->session = *new_session;
    tech_pvt->channel = channel;
    tech_pvt->local_address = switch_core_session_strdup(*new_session, local_addr);
    tech_pvt->local_port = local_port;
    tech_pvt->remote_address = switch_core_session_strdup(*new_session, remote_addr);
    tech_pvt->remote_port = remote_port;
    tech_pvt->ptime = ptime;
    tech_pvt->agreed_pt = (switch_payload_t)pt;
    tech_pvt->dtmf_type = DTMF_2833; /* XXX */

    if (zstr(local_addr) || local_port == 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "The local address and port must be set\n");
        goto fail;
    } else if (zstr(remote_addr) || remote_port == 0) {
        tech_pvt->mode = RTP_RECVONLY;
    } else {
        tech_pvt->mode = RTP_SENDRECV;
    }

    if (!zstr(mode)) {
        if (!strcasecmp(mode, "recvonly")) {
            tech_pvt->mode = RTP_RECVONLY;
        } else if (!strcasecmp(mode, "sendonly")) {
            tech_pvt->mode = RTP_SENDONLY;
        } else {
            tech_pvt->mode = RTP_SENDRECV;
        }
    }

    switch_core_session_set_private(*new_session, tech_pvt);

    caller_profile = switch_caller_profile_clone(*new_session, outbound_profile);
    switch_channel_set_caller_profile(channel, caller_profile);


    snprintf(name, sizeof(name), "rtp/%s", outbound_profile->destination_number);
	switch_channel_set_name(channel, name);

    switch_channel_set_state(channel, CS_INIT);

	if (switch_core_codec_init(&tech_pvt->read_codec,
							   codec,
							   NULL,
							   NULL,
							   rate,
							   ptime,
							   1,
							   /*SWITCH_CODEC_FLAG_ENCODE |*/ SWITCH_CODEC_FLAG_DECODE,
							   NULL, switch_core_session_get_pool(tech_pvt->session)) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Can't load codec?\n");
        goto fail;
	} else {
		if (switch_core_codec_init(&tech_pvt->write_codec,
								   codec,
								   NULL,
								   NULL,
								   rate,
								   ptime,
								   1,
								   SWITCH_CODEC_FLAG_ENCODE /*| SWITCH_CODEC_FLAG_DECODE*/,
								   NULL, switch_core_session_get_pool(tech_pvt->session)) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Can't load codec?\n");
            goto fail;
		}
	}

    if (switch_core_session_set_read_codec(*new_session, &tech_pvt->read_codec) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Can't set read codec?\n");
        goto fail;
    }

    if (switch_core_session_set_write_codec(*new_session, &tech_pvt->write_codec) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Can't set write codec?\n");
        goto fail;
    }

    switch_channel_set_variable(channel, "local_media_ip", local_addr);
    switch_channel_set_variable_printf(channel, "local_media_port", "%d", local_port);

    if (!(tech_pvt->rtp_session = create_rtp_session_with_bind_retry(tech_pvt, rtp_flags, &err))) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't setup RTP session: [%s]\n", err);
        /* The helper has already released (via switch_rtp_new) every port
         * it tried and never allocated a replacement on the failure path;
         * clear the cached value so channel_on_destroy does not call
         * switch_rtp_release_port a second time. */
        tech_pvt->local_port = 0;
        /* The helper may have updated "local_media_port" to the last
         * (failed) retry port; unset it so downstream consumers / events
         * / CDR do not see a port that this session never owned. */
        switch_channel_set_variable(channel, "local_media_port", NULL);
        goto fail;
    }
    /* The helper may have migrated tech_pvt->local_port to a different
     * port after a retry; keep the stack `local_port` in sync so any
     * future fail-branch using `local_port` does not act on the
     * already-released original. */
    local_port = tech_pvt->local_port;

    if (switch_core_session_thread_launch(*new_session) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't start session thread.\n");
        goto fail;
    }

    switch_channel_mark_answered(channel);

    return SWITCH_CAUSE_SUCCESS;

fail:
     // Track failed RTP endpoint origination
     prometheus_increment_rtp_outgoing_failed_calls();

    if (*new_session) {
        // Session was created, channel_on_destroy handles all cleanup
        switch_core_session_destroy(new_session);
    } else if (local_port != 0) {
        // Session creation failed but port was already allocated
        switch_rtp_release_port(local_addr, local_port);
    }
    return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
}

static switch_status_t channel_on_init(switch_core_session_t *session)
{

    switch_channel_t *channel = switch_core_session_get_channel(session);

    switch_channel_set_state(channel, CS_CONSUME_MEDIA);

    return SWITCH_STATUS_FALSE;
}

static switch_status_t channel_on_destroy(switch_core_session_t *session)
{
    crtp_private_t *tech_pvt = NULL;

 	if ((tech_pvt = switch_core_session_get_private(session))) {

		if (tech_pvt->read_codec.implementation) {
			switch_core_codec_destroy(&tech_pvt->read_codec);
		}

		if (tech_pvt->write_codec.implementation) {
			switch_core_codec_destroy(&tech_pvt->write_codec);
		}

		// Clean up RTP session or release port
		if (tech_pvt->rtp_session) {
			// Destroy RTP session (releases port internally)
			switch_rtp_destroy(&tech_pvt->rtp_session);
		} else if (tech_pvt->local_port != 0) {
			// Port was allocated but RTP session was never created
			switch_rtp_release_port(tech_pvt->local_address, tech_pvt->local_port);
		}
	}

    return SWITCH_STATUS_SUCCESS;
}


static switch_status_t channel_read_frame(switch_core_session_t *session, switch_frame_t **frame, switch_io_flag_t flags, int stream_id)
{
    crtp_private_t *tech_pvt;
    switch_channel_t *channel;
    switch_status_t status;

    channel = switch_core_session_get_channel(session);
	assert(channel != NULL);

	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);

    if (!tech_pvt->rtp_session || tech_pvt->mode == RTP_SENDONLY) {
	switch_yield(20000); /* replace by local timer XXX */
        goto cng;
    }

    if (switch_rtp_has_dtmf(tech_pvt->rtp_session)) {
        switch_dtmf_t dtmf = { 0 };
        switch_rtp_dequeue_dtmf(tech_pvt->rtp_session, &dtmf);
        switch_channel_queue_dtmf(channel, &dtmf);
    }

    tech_pvt->read_frame.flags = SFF_NONE;
    tech_pvt->read_frame.codec = &tech_pvt->read_codec;
    status = switch_rtp_zerocopy_read_frame(tech_pvt->rtp_session, &tech_pvt->read_frame, flags);

    if (status != SWITCH_STATUS_SUCCESS && status != SWITCH_STATUS_BREAK) {
        goto cng;
    }

    *frame = &tech_pvt->read_frame;
    return SWITCH_STATUS_SUCCESS;

cng:
    *frame = &tech_pvt->read_frame;
    tech_pvt->read_frame.codec = &tech_pvt->read_codec;
    tech_pvt->read_frame.flags |= SFF_CNG;
    tech_pvt->read_frame.datalen = 0;

    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_write_frame(switch_core_session_t *session, switch_frame_t *frame, switch_io_flag_t flags, int stream_id)
{
    crtp_private_t *tech_pvt;
#ifndef NDEBUG
    switch_channel_t *channel;
    //int frames = 0, bytes = 0, samples = 0;

    channel = switch_core_session_get_channel(session);
	assert(channel != NULL);
#endif
	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);


#if 0
    if (!switch_test_flag(frame, SFF_CNG) && !switch_test_flag(frame, SFF_PROXY_PACKET)) {
		if (tech_pvt->read_codec.implementation->encoded_bytes_per_packet) {
			bytes = tech_pvt->read_codec.implementation->encoded_bytes_per_packet;
			frames = ((int) frame->datalen / bytes);
		} else
			frames = 1;

		samples = frames * tech_pvt->read_codec.implementation->samples_per_packet;
	}

    tech_pvt->timestamp_send += samples;
#endif
    if (tech_pvt->mode == RTP_RECVONLY) {
	return SWITCH_STATUS_SUCCESS;
    }

    switch_rtp_write_frame(tech_pvt->rtp_session, frame);

    return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_send_dtmf(switch_core_session_t *session, const switch_dtmf_t *dtmf)
{
	crtp_private_t *tech_pvt = NULL;

	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);

    switch(tech_pvt->dtmf_type) {
        case DTMF_2833:
        {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Enqueuing RFC2833 DTMF %c of length %d\n", dtmf->digit, dtmf->duration);
            return switch_rtp_queue_rfc2833(tech_pvt->rtp_session, dtmf);
        }
        case DTMF_NONE:
        default:
		{
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Discarding DTMF %c of length %d, DTMF type is NONE\n", dtmf->digit, dtmf->duration);
		}
    }

    return SWITCH_STATUS_SUCCESS;
}

static switch_bool_t compare_var(switch_event_t *event, switch_channel_t *channel, const char *varname)
{
    const char *chan_val = switch_channel_get_variable_dup(channel, varname, SWITCH_FALSE, -1);
    const char *event_val = switch_event_get_header(event, varname);

    if (zstr(chan_val) || zstr(event_val)) {
        return 0;
    }

    return strcasecmp(chan_val, event_val);
}

static switch_status_t channel_receive_event(switch_core_session_t *session, switch_event_t *event)
{
    const char *command = switch_event_get_header(event, "command");
    switch_channel_t *channel = switch_core_session_get_channel(session);
    crtp_private_t *tech_pvt = switch_core_session_get_private(session);
    char *codec  = switch_event_get_header_nil(event, kCODEC);
    char *szptime  = switch_event_get_header_nil(event, kPTIME);
    char *szrate = switch_event_get_header_nil(event, kRATE);
    char *szpt = switch_event_get_header_nil(event, kPT);

    int ptime  = !zstr(szptime) ? atoi(szptime) : 0,
		rate = !zstr(szrate) ? atoi(szrate) : 8000,
		pt = !zstr(szpt) ? atoi(szpt) : 0;


    if (!zstr(command) && !strcasecmp(command, "media_modify")) {
        /* Compare parameters */
        if (compare_var(event, channel, kREMOTEADDR) ||
            compare_var(event, channel, kREMOTEPORT)) {
            char *remote_addr = switch_event_get_header(event, kREMOTEADDR);
            char *szremote_port = switch_event_get_header(event, kREMOTEPORT);
            switch_port_t remote_port = !zstr(szremote_port) ? (switch_port_t)atoi(szremote_port) : 0;
            const char *err;


            switch_channel_set_variable(channel, kREMOTEADDR, remote_addr);
            switch_channel_set_variable(channel, kREMOTEPORT, szremote_port);

            if (switch_rtp_set_remote_address(tech_pvt->rtp_session, remote_addr, remote_port, 0, SWITCH_TRUE, &err) != SWITCH_STATUS_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error setting RTP remote address: %s\n", err);
            } else {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Set RTP remote: %s:%d\n", remote_addr, (int)remote_port);
                tech_pvt->mode = RTP_SENDRECV;
            }
        }

        if (compare_var(event, channel, kCODEC) ||
            compare_var(event, channel, kPTIME) ||
            compare_var(event, channel, kPT) ||
            compare_var(event, channel, kRATE)) {
		/* Reset codec */
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "Switching codec updating \n");

		if (switch_core_codec_init(&tech_pvt->read_codec,
								   codec,
								   NULL,
								   NULL,
								   rate,
								   ptime,
								   1,
					/*SWITCH_CODEC_FLAG_ENCODE |*/ SWITCH_CODEC_FLAG_DECODE,
					NULL, switch_core_session_get_pool(tech_pvt->session)) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Can't load codec?\n");
			goto fail;
		} else {
			if (switch_core_codec_init(&tech_pvt->write_codec,
									   codec,
									   NULL,
									   NULL,
									   rate,
									   ptime,
									   1,
									   SWITCH_CODEC_FLAG_ENCODE /*| SWITCH_CODEC_FLAG_DECODE*/,
									   NULL, switch_core_session_get_pool(tech_pvt->session)) != SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Can't load codec?\n");
				goto fail;
			}
		}

		if (switch_core_session_set_read_codec(session, &tech_pvt->read_codec) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Can't set read codec?\n");
			goto fail;
		}

		if (switch_core_session_set_write_codec(session, &tech_pvt->write_codec) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Can't set write codec?\n");
			goto fail;
		}

		switch_rtp_set_default_payload(tech_pvt->rtp_session, (switch_payload_t)pt);
		//switch_rtp_set_recv_pt(tech_pvt->rtp_session, pt);
	}

        if (compare_var(event, channel, kRFC2833PT)) {
            const char *szpt = switch_channel_get_variable(channel, kRFC2833PT);
            int pt = !zstr(szpt) ? atoi(szpt) : 0;

            switch_channel_set_variable(channel, kRFC2833PT, szpt);
            switch_rtp_set_telephony_event(tech_pvt->rtp_session, (switch_payload_t)pt);
        }

    } else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Received unknown command [%s] in event.\n", !command ? "null" : command);
    }

    return SWITCH_STATUS_SUCCESS;

fail:
	if (tech_pvt->read_codec.implementation) {
		switch_core_codec_destroy(&tech_pvt->read_codec);
	}

	if (tech_pvt->write_codec.implementation) {
		switch_core_codec_destroy(&tech_pvt->write_codec);
	}

	switch_core_session_destroy(&session);

    return SWITCH_STATUS_FALSE;
}

static switch_status_t channel_receive_message(switch_core_session_t *session, switch_core_session_message_t *msg)
{
    crtp_private_t *tech_pvt = NULL;

	tech_pvt = switch_core_session_get_private(session);
	assert(tech_pvt != NULL);

    switch (msg->message_id) {
        case SWITCH_MESSAGE_INDICATE_DEBUG_MEDIA:
        {
            if (switch_rtp_ready(tech_pvt->rtp_session) && !zstr(msg->string_array_arg[0]) && !zstr(msg->string_array_arg[1])) {
				switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID] = {0};
				int x = 0;

                if (!strcasecmp(msg->string_array_arg[0], "read")) {
                    flags[SWITCH_RTP_FLAG_DEBUG_RTP_READ]++;x++;
                } else if (!strcasecmp(msg->string_array_arg[0], "write")) {
                    flags[SWITCH_RTP_FLAG_DEBUG_RTP_WRITE]++;x++;
                } else if (!strcasecmp(msg->string_array_arg[0], "both")) {
                    flags[SWITCH_RTP_FLAG_DEBUG_RTP_READ]++;x++;
					flags[SWITCH_RTP_FLAG_DEBUG_RTP_WRITE]++;
                }

                if (x) {
                    if (switch_true(msg->string_array_arg[1])) {
                        switch_rtp_set_flags(tech_pvt->rtp_session, flags);
                    } else {
                        switch_rtp_clear_flags(tech_pvt->rtp_session, flags);
                    }
                } else {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Invalid Options\n");
                }
            }
            break;
        }
        case SWITCH_MESSAGE_INDICATE_AUDIO_SYNC:
            if (switch_rtp_ready(tech_pvt->rtp_session)) {
                rtp_flush_read_buffer(tech_pvt->rtp_session, SWITCH_RTP_FLUSH_ONCE);
            }
            break;
        case SWITCH_MESSAGE_INDICATE_JITTER_BUFFER:
		{
			if (switch_rtp_ready(tech_pvt->rtp_session)) {
				int len = 0, maxlen = 0, qlen = 0, maxqlen = 50;

				if (msg->string_arg) {
					char *p;
					const char *s;

					if (!strcasecmp(msg->string_arg, "pause")) {
						switch_rtp_pause_jitter_buffer(tech_pvt->rtp_session, SWITCH_TRUE);
						goto end;
					} else if (!strcasecmp(msg->string_arg, "resume")) {
						switch_rtp_pause_jitter_buffer(tech_pvt->rtp_session, SWITCH_FALSE);
						goto end;
					} else if (!strncasecmp(msg->string_arg, "debug:", 6)) {
						s = msg->string_arg + 6;
						if (!strcmp(s, "off")) {
							s = NULL;
						}
                        switch_rtp_debug_jitter_buffer(tech_pvt->rtp_session, s);
						goto end;
					}


					if ((len = atoi(msg->string_arg))) {
						qlen = len / (tech_pvt->read_codec.implementation->microseconds_per_packet / 1000);
						if (qlen < 1) {
							qlen = 3;
						}
					}

					if (qlen) {
						if ((p = strchr(msg->string_arg, ':'))) {
							p++;
							maxlen = atol(p);
						}
					}


					if (maxlen) {
						maxqlen = maxlen / (tech_pvt->read_codec.implementation->microseconds_per_packet / 1000);
					}
				}

				if (qlen) {
					if (maxqlen < qlen) {
						maxqlen = qlen * 5;
					}
					if (switch_rtp_activate_jitter_buffer(tech_pvt->rtp_session, qlen, maxqlen,
														  tech_pvt->read_codec.implementation->samples_per_packet,
														  tech_pvt->read_codec.implementation->samples_per_second) == SWITCH_STATUS_SUCCESS) {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(tech_pvt->session),
										  SWITCH_LOG_DEBUG, "Setting Jitterbuffer to %dms (%d frames) (%d max frames)\n",
										  len, qlen, maxqlen);
						switch_channel_set_flag(tech_pvt->channel, CF_JITTERBUFFER);
						if (!switch_false(switch_channel_get_variable(tech_pvt->channel, "rtp_jitter_buffer_plc"))) {
							switch_channel_set_flag(tech_pvt->channel, CF_JITTERBUFFER_PLC);
						}
					} else {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(tech_pvt->session),
										  SWITCH_LOG_WARNING, "Error Setting Jitterbuffer to %dms (%d frames)\n", len, qlen);
					}

				} else {
					switch_rtp_deactivate_jitter_buffer(tech_pvt->rtp_session);
				}
			}
		}
            break;

        default:
            break;
    }
end:
    return SWITCH_STATUS_SUCCESS;
}

