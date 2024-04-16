/*
 *
 * mod_siprec_src -- Module for SIPREC Session Recording Client
 *
 */

#include <switch.h>
#include <g711.h>
#include <switch_cJSON.h>
#include <switch_stun.h>

static const char SIP_SIPREC_HEADER_PREFIX[] = "siprec_sip_h_";
#define SIPREC_PRIVATE "_siprec_"
#define SIPREC_BUG_NAME_READ "siprec_read"
#define SIPREC_SRS_NAME "drachtio"
#define SIP_SIPREC_HEADER_PREFIX_LEN (sizeof(SIP_SIPREC_HEADER_PREFIX)-1)

SWITCH_MODULE_LOAD_FUNCTION(mod_siprec_src_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_siprec_src_shutdown);
SWITCH_MODULE_DEFINITION(mod_siprec_src, mod_siprec_src_load, mod_siprec_src_shutdown, NULL);

typedef struct siprec_session_s {
	switch_core_session_t *session;
	switch_core_session_t *ssession;
	switch_channel_t *channel;
	switch_port_t read_rtp_port;
	switch_port_t write_rtp_port;
	char remote_rtp_port_r[10];
	char remote_rtp_port_w[10];
	switch_rtp_t *read_rtp_stream;
	switch_rtp_t *write_rtp_stream;
	switch_codec_implementation_t read_impl;
	switch_codec_implementation_t write_impl;
	uint32_t read_cnt;
	uint32_t write_cnt;
	switch_media_bug_t *read_bug;
    switch_audio_resampler_t *read_resampler;
    switch_audio_resampler_t *write_resampler;
	switch_call_cause_t cause;
} siprec_session_t;

typedef enum {
	FS_SIPREC_READ,
	FS_SIPREC_WRITE,
	FS_SIPREC_BOTH
} siprec_stream_type_t;

static struct {
	char local_media_ip[35];
	char siprec_srs_host[35];
	int siprec_srs_port;
	siprec_stream_type_t stream_type;
	char *telnyx_subdomain;
} globals;

typedef enum {
	FS_SIPREC_START,
	FS_SIPREC_STOP
} siprec_status_t;


static int siprec_tear_down_rtp(siprec_session_t *siprec, siprec_stream_type_t type)
{
	if ((type == FS_SIPREC_READ || type == FS_SIPREC_BOTH) && siprec->read_rtp_stream) {
		switch_rtp_release_port(globals.local_media_ip, siprec->read_rtp_port);
		switch_rtp_destroy(&siprec->read_rtp_stream);
		siprec->read_rtp_port = 0;
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(siprec->session), SWITCH_LOG_DEBUG, "Destroyed read rtp\n");
	}

	if ((type == FS_SIPREC_WRITE || type == FS_SIPREC_BOTH) && siprec->write_rtp_stream) {
		switch_rtp_release_port(globals.local_media_ip, siprec->write_rtp_port);
		switch_rtp_destroy(&siprec->write_rtp_stream);
		siprec->write_rtp_port = 0;
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(siprec->session), SWITCH_LOG_DEBUG, "Destroyed write rtp\n");
	}
	return 0;
}

static void get_extra_headers(switch_channel_t *channel)
{
	switch_event_header_t *ei = NULL;
	for (ei = switch_channel_variable_first(channel);
	     ei;
	     ei = ei->next) {
		const char *name = ei->name;
		char *value = ei->value;
		if (!strncasecmp(name, SIP_SIPREC_HEADER_PREFIX, SIP_SIPREC_HEADER_PREFIX_LEN)) {
			if (strstr(name, "DestHost")) {
				globals.telnyx_subdomain = value;
			}
		}
	}
	switch_channel_variable_last(channel);
}

static int allocate_rtp_port(siprec_session_t *siprec, siprec_stream_type_t type, switch_codec_implementation_t *codec_impl)
{
	switch_port_t rtp_port = 0;
	int res = 0;
	const char *type_str = type == FS_SIPREC_READ ? "read" : "write";

	if (!(rtp_port = switch_rtp_request_port(globals.local_media_ip))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to allocate %s RTP port for IP %s\n", type_str, globals.local_media_ip);
		res = -1;
		goto done;
	}
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Allocated %s port %d for local IP %s\n", type_str,
			rtp_port, globals.local_media_ip);


done:
	if (res == -1) {
		if (rtp_port) {
			switch_rtp_release_port(globals.local_media_ip, rtp_port);
		}
	} else {
		if (type == FS_SIPREC_READ) {
			siprec->read_rtp_port = rtp_port;
		} else {
			siprec->write_rtp_port = rtp_port;
		}
	}
	
	return res;
}

static void get_r_port(const char **r_sdp, switch_port_t *rport)
{
	const char *ptr;
	char pbuf[10] = "";

	if ((ptr = switch_stristr("m=audio", *r_sdp))) {
		int i = 0;

		ptr += 7;
		// remove m=audio to catch the next one
		*r_sdp = strdup(ptr);

		while(*ptr == ' ') {
			ptr++;
		}
		while(*ptr && *ptr != ' ' && *ptr != '\r' && *ptr != '\n') {
			pbuf[i++] = *ptr++;
		}
	}

	*rport = (switch_port_t)atoi(pbuf);
}

static int create_rtp_stream(siprec_session_t *siprec, siprec_stream_type_t type, switch_codec_implementation_t *codec_impl, switch_port_t rtp_port, const char *remote_media_ip, switch_port_t remote_rtp_port)
{
	switch_rtp_t *rtp_stream = NULL;
	switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID] = {0};
	int res = 0;
	const char *type_str = type == FS_SIPREC_READ ? "read" : "write";
	const char  *err = "unknown error";

	rtp_stream = switch_rtp_new(globals.local_media_ip, rtp_port,
			(const char *) remote_media_ip, remote_rtp_port,
			0, /* PCMU IANA*/
			codec_impl->samples_per_packet,
			codec_impl->microseconds_per_packet,
								flags, NULL, &err, switch_core_session_get_pool(siprec->session));
	if (!rtp_stream) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to create %s RTP stream at %s:%d: %s\n",
				type_str, globals.local_media_ip, rtp_port, err);
		res = -1;
		goto done;
	}

    switch_rtp_intentional_bugs(rtp_stream, RTP_BUG_SEND_LINEAR_TIMESTAMPS);

done:
	if (res == -1) {
		if (rtp_stream) {
			switch_rtp_destroy(&rtp_stream);
		}
	} else {
		if (type == FS_SIPREC_READ) {
			siprec->read_rtp_stream = rtp_stream;
		} else {
			siprec->write_rtp_stream = rtp_stream;
		}
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Successfully created %s RTP stream at %s:%d at %dms@%dHz to %s:%d\n",
				type_str, globals.local_media_ip, rtp_port, codec_impl->microseconds_per_packet/1000, codec_impl->samples_per_second, remote_media_ip, remote_rtp_port);

	return res;
}

static void fire_siprec_start_event(switch_core_session_t *session)
{
	switch_event_t *event;
	switch_channel_t *channel = NULL;

	if (!session) {
		return;
	}

	channel = switch_core_session_get_channel(session);
	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, "siprec_start::") == SWITCH_STATUS_SUCCESS) {
		switch_channel_event_set_data(channel, event);
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "siprec_start_time", "%ld", switch_micro_time_now() / 1000);
		switch_event_fire(&event);
	}
}

static void fire_siprec_stop_event(siprec_session_t *siprec)
{
	switch_event_t *event;
	switch_channel_t *channel = NULL;

	if (!siprec->session) {
		return;
	}

	channel = switch_core_session_get_channel(siprec->session);
	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, "siprec_stop::") == SWITCH_STATUS_SUCCESS) {
		switch_channel_event_set_data(channel, event);
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "siprec_stop_time", "%ld", switch_micro_time_now() / 1000);
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "hangup_cause", "%s", switch_channel_cause2str(siprec->cause));
		switch_event_fire(&event);
	}
}

static void set_sdp_data(switch_stream_handle_t sdp, siprec_session_t *siprec, siprec_stream_type_t stream_type)
{

	sdp.write_function(&sdp, "application/sdp:~\r\n");
	sdp.write_function(&sdp, "v=0\r\n");
	sdp.write_function(&sdp, "o=FreeSWITCH 1632033305 1632033306 IN IP4 %s\r\n", globals.local_media_ip);
	sdp.write_function(&sdp, "s=Telnyx Media Gateway\r\n");
	sdp.write_function(&sdp, "c=IN IP4 %s\r\n", globals.local_media_ip);
	sdp.write_function(&sdp, "t=0 0\r\n");

	if (stream_type == FS_SIPREC_READ) {
		sdp.write_function(&sdp, "m=audio %d RTP/AVP 0\r\n", siprec->read_rtp_port);
		sdp.write_function(&sdp, "a=rtpmap:0 PCMU/%d\r\n", siprec->read_impl.samples_per_second);
		sdp.write_function(&sdp, "a=ptime:20\r\n");
		sdp.write_function(&sdp, "a=maxptime:20\r\n");
		sdp.write_function(&sdp, "a=sendonly\r\n");
		sdp.write_function(&sdp, "a=label:inbound\r\n");
	} else if (stream_type == FS_SIPREC_WRITE) {
		sdp.write_function(&sdp, "m=audio %d RTP/AVP 0\r\n", siprec->write_rtp_port);
		sdp.write_function(&sdp, "a=rtpmap:0 PCMU/%d\r\n", siprec->write_impl.samples_per_second);
		sdp.write_function(&sdp, "a=ptime:20\r\n");
		sdp.write_function(&sdp, "a=maxptime:20\r\n");
		sdp.write_function(&sdp, "a=sendonly\r\n");
		sdp.write_function(&sdp, "a=label:outbound\r\n");
	} else {
		sdp.write_function(&sdp, "m=audio %d RTP/AVP 0\r\n", siprec->read_rtp_port);
		sdp.write_function(&sdp, "a=rtpmap:0 PCMU/%d\r\n", siprec->read_impl.samples_per_second);
		sdp.write_function(&sdp, "a=ptime:20\r\n");
		sdp.write_function(&sdp, "a=maxptime:20\r\n");
		sdp.write_function(&sdp, "a=sendonly\r\n");
		sdp.write_function(&sdp, "a=label:inbound\r\n");
		sdp.write_function(&sdp, "m=audio %d RTP/AVP 0\r\n", siprec->write_rtp_port);
		sdp.write_function(&sdp, "a=rtpmap:0 PCMU/%d\r\n", siprec->write_impl.samples_per_second);
		sdp.write_function(&sdp, "a=ptime:20\r\n");
		sdp.write_function(&sdp, "a=maxptime:20\r\n");
		sdp.write_function(&sdp, "a=sendonly\r\n");
		sdp.write_function(&sdp, "a=label:outbound\r\n");
	}
}

static void set_rs_metadata(switch_stream_handle_t rs, siprec_session_t *siprec, switch_channel_t *channel, siprec_stream_type_t stream_type)
{
	const char *session_uuid = switch_core_session_get_uuid(siprec->session);
	const char *from_uri = switch_channel_get_variable(channel, "sip_from_uri");
	const char *to_uri = switch_channel_get_variable(channel, "sip_to_uri");
	const char *sip_call_id = switch_channel_get_variable(channel, "sip_call_id");
	char participant_from_id[25];
	char participant_to_id[25];
	char read_stream_id[25];
	char write_stream_id[25];
	switch_time_exp_t tm;
	char date[80] = "";
	switch_size_t retsize;
	switch_time_t ts = switch_micro_time_now();

	participant_from_id[24] = '\0';
	participant_to_id[24] = '\0';
	read_stream_id[24] = '\0';
	write_stream_id[24] = '\0';
	switch_stun_random_string(participant_from_id, 24, NULL);
	switch_stun_random_string(participant_to_id, 24, NULL);
	switch_stun_random_string(read_stream_id, 24, NULL);
	switch_stun_random_string(write_stream_id, 24, NULL);
	switch_time_exp_lt(&tm, ts);
	switch_strftime_nocheck(date, &retsize, sizeof(date), "%Y-%m-%dT%H:%M:%S%z", &tm);

	rs.write_function(&rs, "application/rs-metadata+xml:~\r\n");
	rs.write_function(&rs, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n");
	rs.write_function(&rs, "<recording xmlns=\"urn:ietf:params:xml:ns:recording:1\">\r\n");
	rs.write_function(&rs, "<session session_id=\"%s\">\r\n", session_uuid);
	rs.write_function(&rs, "<sipSessionID>%s</sipSessionID>\r\n", sip_call_id);
	rs.write_function(&rs, "</session>\r\n");
	rs.write_function(&rs, "<participant participant_id=\"%s\">\r\n", participant_from_id);
	rs.write_function(&rs, "<nameID aor=\"%s\"/>\r\n", from_uri);
	rs.write_function(&rs, "</participant>\r\n");
	rs.write_function(&rs, "<participant participant_id=\"%s\">\r\n", participant_to_id);
	rs.write_function(&rs, "<nameID aor=\"%s\"/>\r\n", to_uri);
	rs.write_function(&rs, "</participant>\r\n");

	if (stream_type == FS_SIPREC_READ) {
		rs.write_function(&rs, "<stream stream_id=\"%s\" session_id=\"%s\">\r\n", read_stream_id, session_uuid);
		rs.write_function(&rs, "<label>inbound</label>\r\n");
		rs.write_function(&rs, "</stream>\r\n");
	} else if (stream_type == FS_SIPREC_WRITE) {
		rs.write_function(&rs, "<stream stream_id=\"%s\" session_id=\"%s\">\r\n", write_stream_id, session_uuid);
		rs.write_function(&rs, "<label>outbound</label>\r\n");
		rs.write_function(&rs, "</stream>\r\n");
	} else {
		rs.write_function(&rs, "<stream stream_id=\"%s\" session_id=\"%s\">\r\n", read_stream_id, session_uuid);
		rs.write_function(&rs, "<label>inbound</label>\r\n");
		rs.write_function(&rs, "</stream>\r\n");
		rs.write_function(&rs, "<stream stream_id=\"%s\" session_id=\"%s\">\r\n", write_stream_id, session_uuid);
		rs.write_function(&rs, "<label>outbound</label>\r\n");
		rs.write_function(&rs, "</stream>\r\n");
	}

	rs.write_function(&rs, "<sessionrecordingassoc session_id=\"%s\">\r\n", session_uuid);
	rs.write_function(&rs, "<associate-time>%s</associate-time>\r\n", date);
	rs.write_function(&rs, "</sessionrecordingassoc>\r\n");
	rs.write_function(&rs, "</recording>\r\n");
}

static int start_siprec_session(siprec_session_t *siprec, siprec_status_t status)
{
	switch_channel_t *channel = NULL;
	switch_core_session_t *session = siprec->session;
	switch_status_t codec_status = SWITCH_STATUS_SUCCESS;
	switch_stream_handle_t sdp = { 0 };
	switch_stream_handle_t rs = { 0 };
	switch_caller_profile_t *caller_profile = NULL;
	const char *caller_id_number = NULL;
	const char *remote_media_ip = NULL;
	switch_port_t remote_rtp_port_r = 0, remote_rtp_port_w = 0;
	char *originate_str = NULL;
	const char *sdp_in;
	uint32_t timeout = 60;
	int rc = 0;

	channel = switch_core_session_get_channel(session);
	SWITCH_STANDARD_STREAM(sdp);
	SWITCH_STANDARD_STREAM(rs);

	if (status == FS_SIPREC_STOP) {
		rc = -1;
		goto done;
	}

	siprec->ssession = NULL;
	siprec->cause = SWITCH_CAUSE_NONE;

	if (globals.stream_type == FS_SIPREC_READ || globals.stream_type == FS_SIPREC_BOTH) {
		if ((codec_status = switch_core_session_get_read_impl(siprec->session, &siprec->read_impl)) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "No read codec implementation available!\n");
			rc = -1;
			goto done;
		}
		
		if (allocate_rtp_port(siprec, FS_SIPREC_READ, &siprec->read_impl)) {
			rc = -1;
			goto done;
		}
	}

	if (globals.stream_type == FS_SIPREC_WRITE || globals.stream_type == FS_SIPREC_BOTH) { 
		if ((codec_status = switch_core_session_get_write_impl(siprec->session, &siprec->write_impl)) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "No write codec implementation available!\n");
			rc = -1;
			goto done;
		}

		if (allocate_rtp_port(siprec, FS_SIPREC_WRITE, &siprec->write_impl)) {
			rc = -1;
			goto done;
		}
	}

	set_sdp_data(sdp, siprec, globals.stream_type);
	set_rs_metadata(rs, siprec, channel, globals.stream_type);
	get_extra_headers(channel);

	caller_profile = switch_channel_get_caller_profile(channel);

	/* Get caller meta data */
	caller_id_number = switch_caller_get_field_by_name(caller_profile, "caller_id_number");

	if (!zstr(globals.telnyx_subdomain)) {
		caller_id_number = SIPREC_SRS_NAME;
	}

	originate_str = switch_mprintf("{sip_h_X-DestHost=%s,sip_multipart[0]=%s,sip_multipart[1]=%s}sofia/outbound/%s@%s:%d", globals.telnyx_subdomain, sdp.data, rs.data, caller_id_number, globals.siprec_srs_host, globals.siprec_srs_port);
	switch_ivr_originate(NULL, &siprec->ssession, &siprec->cause, originate_str, timeout, NULL, NULL, NULL, caller_profile, NULL, SOF_NONE, NULL, NULL);
	switch_safe_free(originate_str);

	if (siprec->cause != SWITCH_CAUSE_SUCCESS && siprec->cause != SWITCH_CAUSE_NONE) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "SIPREC session fails. Hangup Cause: %s\n", switch_channel_cause2str(siprec->cause));
		status = FS_SIPREC_STOP;
		goto done;
	}

	if (siprec->ssession) {
		siprec->channel = switch_core_session_get_channel(siprec->ssession);

		if (switch_channel_up(siprec->channel)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(siprec->ssession), SWITCH_LOG_DEBUG, "SIPREC channel answered\n");

			// Get remote RTP address and ports
			remote_media_ip = switch_channel_get_variable(siprec->channel, SWITCH_REMOTE_MEDIA_IP_VARIABLE);
			sdp_in = switch_channel_get_variable(siprec->channel, SWITCH_R_SDP_VARIABLE);

			get_r_port(&sdp_in, &remote_rtp_port_r);
			get_r_port(&sdp_in, &remote_rtp_port_w);

			// Push streams
			if (globals.stream_type == FS_SIPREC_READ || globals.stream_type == FS_SIPREC_BOTH) { 
				create_rtp_stream(siprec, FS_SIPREC_READ, &siprec->read_impl, siprec->read_rtp_port, remote_media_ip, remote_rtp_port_r);
			}
			if (globals.stream_type == FS_SIPREC_WRITE || globals.stream_type == FS_SIPREC_BOTH) { 
				create_rtp_stream(siprec, FS_SIPREC_WRITE, &siprec->write_impl, siprec->write_rtp_port, remote_media_ip, remote_rtp_port_w);
			}

			// create siprec start event
			fire_siprec_start_event(siprec->session);
			
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(siprec->ssession), SWITCH_LOG_DEBUG, "Failed to connect SIPREC call\n");
			switch_channel_hangup(siprec->channel, SWITCH_CAUSE_ORIGINATOR_CANCEL);
		}
		switch_core_session_rwunlock(siprec->ssession);
	}


	done:

		if (sdp.data) {
			free(sdp.data);
		}

		if (rs.data) {
			free(rs.data);
		}

		if (status == FS_SIPREC_STOP) {
			siprec_tear_down_rtp(siprec, globals.stream_type);
			fire_siprec_stop_event(siprec);
			if (siprec->ssession) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(siprec->ssession), SWITCH_LOG_INFO, "Destroying SIPREC session\n");
				if (siprec->channel)
					switch_channel_hangup(siprec->channel, SWITCH_CAUSE_NORMAL_CLEARING);

				switch_core_session_rwunlock(siprec->ssession);
			}
		}


	return rc;
}

static switch_bool_t siprec_audio_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	siprec_session_t *siprec = user_data;
	switch_core_session_t *session = siprec->session;
	switch_frame_t pcmu_frame = { 0 };
    switch_frame_t *linear_frame, raw_frame = { 0 };
	uint8_t pcmu_data[SWITCH_RECOMMENDED_BUFFER_SIZE];
	uint8_t raw_data[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
	uint8_t resample_data[SWITCH_RECOMMENDED_BUFFER_SIZE];
    uint32_t linear_len = 0;
	uint32_t i = 0;
	int16_t *linear_samples = NULL;



	if (type == SWITCH_ABC_TYPE_READ_REPLACE || type == SWITCH_ABC_TYPE_WRITE_REPLACE || type == SWITCH_ABC_TYPE_READ_PING) {
        int16_t *data;

        if (type == SWITCH_ABC_TYPE_READ_REPLACE || type == SWITCH_ABC_TYPE_READ_PING) {

            if (type == SWITCH_ABC_TYPE_READ_REPLACE) {
                linear_frame = switch_core_media_bug_get_read_replace_frame(bug);
            } else {
                switch_status_t status;

                raw_frame.data = raw_data;
                raw_frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;
                linear_frame = &raw_frame;

                status = switch_core_media_bug_read(bug, &raw_frame, SWITCH_FALSE);
                if (status != SWITCH_STATUS_SUCCESS && status != SWITCH_STATUS_BREAK) {
                    return SWITCH_TRUE;
                }
            }

            if (siprec->read_resampler) {
                data = (int16_t *) linear_frame->data;
                switch_resample_process(siprec->read_resampler, data, (int) linear_frame->datalen / 2);
                linear_len = siprec->read_resampler->to_len * 2;
                memcpy(resample_data, siprec->read_resampler->to, linear_len);
                linear_samples = (int16_t *)resample_data;
            } else {
                linear_samples = linear_frame->data;
                linear_len = linear_frame->datalen;
            }
        }

        if (type == SWITCH_ABC_TYPE_WRITE_REPLACE) {
            linear_frame = switch_core_media_bug_get_write_replace_frame(bug);

            if (siprec->write_resampler) {
                data = (int16_t *) linear_frame->data;
                switch_resample_process(siprec->write_resampler, data, (int) linear_frame->datalen / 2);
                linear_len = siprec->write_resampler->to_len * 2;
                memcpy(resample_data, siprec->write_resampler->to, linear_len);
                linear_samples = (int16_t *)resample_data;
            } else {
                linear_samples = linear_frame->data;
                linear_len = linear_frame->datalen;
            }
        }

		/* convert the L16 frame into PCMU */
		memset(&pcmu_frame, 0, sizeof(pcmu_frame));
		for (i = 0; i < linear_len / sizeof(int16_t); i++) {
			pcmu_data[i] = linear_to_ulaw(linear_samples[i]);
		}
		pcmu_frame.source = __SWITCH_FUNC__;
		pcmu_frame.data = pcmu_data;
		pcmu_frame.datalen = i;
		pcmu_frame.payload = 0;
	}

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		{
            switch_codec_implementation_t read_impl;

            switch_core_session_get_read_impl(session, &read_impl);

            if (read_impl.actual_samples_per_second != 8000) {
                switch_resample_create(&siprec->read_resampler,
                                       read_impl.actual_samples_per_second,
                                       8000,
                                       320, SWITCH_RESAMPLE_QUALITY, 1);

                switch_resample_create(&siprec->write_resampler,
                                       read_impl.actual_samples_per_second,
                                       8000,
                                       320, SWITCH_RESAMPLE_QUALITY, 1);
            }


			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Starting SIPREC audio stream\n");
			start_siprec_session(siprec, FS_SIPREC_START);
		}
		break;
	case SWITCH_ABC_TYPE_CLOSE:
		{
            if (siprec->read_resampler) {
                switch_resample_destroy(&siprec->read_resampler);
            }

            if (siprec->write_resampler) {
                switch_resample_destroy(&siprec->write_resampler);
            }

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Stopping SIPREC audio stream\n");
			start_siprec_session(siprec, FS_SIPREC_STOP);
		}
		break;
	case SWITCH_ABC_TYPE_READ_REPLACE:
    case SWITCH_ABC_TYPE_READ_PING:
		{
			if (pcmu_frame.datalen) {
				if (switch_rtp_write_frame(siprec->read_rtp_stream, &pcmu_frame) > 0) {
					siprec->read_cnt++;
					if (siprec->read_cnt < 10) {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "SIPREC wrote %u bytes! (read)\n", pcmu_frame.datalen);
					}
				} else if (globals.stream_type == FS_SIPREC_WRITE || siprec->cause != SWITCH_CAUSE_NONE) {
					// Do nothing
				} else {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to write %u bytes! (read)\n", pcmu_frame.datalen);
				}
			}
		}
		break;
	case SWITCH_ABC_TYPE_WRITE_REPLACE:
		{
			if (pcmu_frame.datalen) {
				if (switch_rtp_write_frame(siprec->write_rtp_stream, &pcmu_frame) > 0) {
					siprec->write_cnt++;
					if (siprec->write_cnt < 10) {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "SIPREC wrote %u bytes! (write)\n", pcmu_frame.datalen);
					}
				} else if (globals.stream_type == FS_SIPREC_READ || siprec->cause != SWITCH_CAUSE_NONE) {
					// Do nothing
				} else {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to write %u bytes! (write)\n", pcmu_frame.datalen);
				}
			}
		}
        break;
	default:
		break;
	}

	return SWITCH_TRUE;
}


SWITCH_STANDARD_APP(siprec_start_function)
{
	switch_status_t status;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	siprec_session_t *siprec = NULL;
	switch_media_bug_t *bug = NULL;
	char *argv[6];
	int argc;
    int flags = 0;
	char *lbuf = NULL;

	if ((siprec = (siprec_session_t *) switch_channel_get_private(channel, SIPREC_PRIVATE))) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Cannot run siprec src 2 times on the same session!\n");
		return;
	}
	
	globals.stream_type = FS_SIPREC_BOTH;

	if (!zstr(data)) {
		const char *parse_end = NULL;
		cJSON* item;
		cJSON* params = cJSON_ParseWithOpts(data, &parse_end, 0);
		if (!params) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "siprec_start INVALID JSON %s\n", parse_end);
		} else {
			if ((item = cJSON_GetObjectItem(params, "siprec_srs_host")) && item->type == cJSON_String) {
				snprintf(globals.siprec_srs_host, sizeof(globals.siprec_srs_host), "%s", item->valuestring);
			} 

			if ((item = cJSON_GetObjectItem(params, "siprec_srs_port")) && item->type == cJSON_Number) {
				globals.siprec_srs_port = item->valueint;
			}

			if ((item = cJSON_GetObjectItem(params, "direction")) && item->type == cJSON_String) {
				if (strstr(item->valuestring, "read")) {
					globals.stream_type = FS_SIPREC_READ;
				} else if (strstr(item->valuestring, "write")) {
					globals.stream_type = FS_SIPREC_WRITE;
				}	
			}

			cJSON_Delete(params);
		}
	}

	siprec = switch_core_session_alloc(session, sizeof(*siprec));
	switch_assert(siprec);
	memset(siprec, 0, sizeof(*siprec));

	if (data && (lbuf = switch_core_session_strdup(session, data))
		&& (argc = switch_separate_string(lbuf, ' ', argv, (sizeof(argv) / sizeof(argv[0]))))) {
	}

	siprec->session = session;

    flags = SMBF_READ_REPLACE | SMBF_WRITE_REPLACE | SMBF_ANSWER_REQ;

	status = switch_core_media_bug_add(session, SIPREC_BUG_NAME_READ, NULL, siprec_audio_callback, siprec, 0, flags, &bug);

	if (status != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to attach siprec to media stream!\n");
		return;
	}
	siprec->read_bug = bug;
	bug = NULL;
	switch_channel_set_private(channel, SIPREC_PRIVATE, siprec);

}

SWITCH_STANDARD_APP(siprec_stop_function)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	siprec_session_t *siprec = NULL;

	if ((siprec = (siprec_session_t *) switch_channel_get_private(channel, SIPREC_PRIVATE))) {
		switch_channel_set_private(channel, SIPREC_PRIVATE, NULL);
		if (siprec->read_bug) {
			switch_core_media_bug_remove(session, &siprec->read_bug);
			siprec->read_bug = NULL;
		}
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Stopped SIPREC SRC\n");
		return;
	}

}

#define SIPREC_XML_CONFIG "siprec.conf"
static int load_config(void)
{
	switch_xml_t cfg, xml, settings, param;
	if (!(xml = switch_xml_open_cfg(SIPREC_XML_CONFIG, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to open XML configuration '%s'\n", SIPREC_XML_CONFIG);
		return -1;
	}

	if ((settings = switch_xml_child(cfg, "settings"))) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			char *var = (char *) switch_xml_attr_soft(param, "name");
			char *val = (char *) switch_xml_attr_soft(param, "value");
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Found parameter %s=%s\n", var, val);
			if (!strcasecmp(var, "local-media-ip")) {
				snprintf(globals.local_media_ip, sizeof(globals.local_media_ip), "%s", val);
			} else if (!strcasecmp(var, "siprec-srs-host")) {
				snprintf(globals.siprec_srs_host, sizeof(globals.siprec_srs_host), "%s", val);
			} else if (!strcasecmp(var, "siprec-srs-port")) {
				globals.siprec_srs_port = atoi(val);
			}
		}
	}

	switch_xml_free(xml);
	return 0;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_siprec_src_load)
{
	switch_application_interface_t *app_interface = NULL;

	memset(&globals, 0, sizeof(globals));

	if (load_config()) {
		return SWITCH_STATUS_UNLOAD;
	}

	if (zstr(globals.local_media_ip)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "No local media address specified!\n");
		return SWITCH_STATUS_UNLOAD;
	}

	if (zstr(globals.siprec_srs_host)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "No siprec srs address specified!\n");
		return SWITCH_STATUS_UNLOAD;
	}

	if (!globals.siprec_srs_port) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "No sip server port specified!\n");
		return SWITCH_STATUS_UNLOAD;
	}

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	SWITCH_ADD_APP(app_interface, "siprec_start", "Send media to SIPREC server", "Send media to SIPREC server", siprec_start_function, "", SAF_NONE);
	SWITCH_ADD_APP(app_interface, "siprec_stop", "Stop SIPREC", "Send media to SIPREC server", siprec_stop_function, "", SAF_NONE);

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_siprec_src_shutdown)
{
	return SWITCH_STATUS_UNLOAD;
}