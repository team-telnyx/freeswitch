#include <mod_happy_voicemail.h>


void hv_deposit_app_exec(switch_core_session_t *session, const char *file_path, hv_settings_t *settings)
{
	switch_channel_t *channel = NULL;
	switch_file_handle_t fh = { 0 };
	switch_input_args_t args = { 0 };
	char input[10] = "";
	switch_codec_implementation_t read_impl = { 0 };

	if (!session || zstr(file_path) || !settings) {
		return;
	}

	channel = switch_core_session_get_channel(session);
	if (!channel) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Channel missing\n");
		return;
	}

	switch_core_session_get_read_impl(session, &read_impl);
	switch_ivr_gentones(session, settings->tone_spec, 0, NULL);

	memset(&fh, 0, sizeof(fh));
	fh.thresh = settings->record_silence_threshold;
	fh.silence_hits = settings->record_silence_hits;
	fh.samplerate = settings->record_sample_rate;

	memset(input, 0, sizeof(input));
	//args.input_callback = cancel_on_dtmf;
	args.buf = input;
	args.buflen = sizeof(input);

	unlink(file_path);

	switch_channel_set_variable(channel, "record_post_process_exec_api", "happy_voicemail_upload");
	switch_channel_set_variable(channel, "record_check_silence", settings->record_check_silence ? "true" : "false");
	switch_ivr_record_file(session, &fh, file_path, &args, settings->record_max_len);
}

void hv_retrieval_app_exec(switch_core_session_t *session, const char *data, hv_settings_t *settings)
{
	char *cli = NULL;
	cJSON *json = NULL;
	hv_http_req_t req = { 0 }; //upload = { 0 };
	char *s = NULL;
	switch_channel_t *channel = NULL;
	int err = 0;

	if (!session || !settings) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Bad params\n");
		return;
	}

	channel = switch_core_session_get_channel(session);
	if (!channel) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Channel missing\n");
		return;
	}

	{
		switch_event_t *event = NULL;

		if (SWITCH_STATUS_SUCCESS != switch_event_create_plain(&event, SWITCH_EVENT_CHANNEL_DATA)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Cannot get origination (caller) number (dump event failed)\n");
			return;
		}
		switch_channel_event_set_data(channel, event);

		if (settings->dump_events) {
			char *buf = NULL;
			switch_event_serialize(event, &buf, SWITCH_FALSE);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "Voicemail CHANNEL_DATA:\n%s\n", buf);
			free(buf);
		}

		cli = strdup(switch_event_get_header(event, "variable_telnyx_dialed_extension"));
		switch_event_destroy(&event);
	}

	if (zstr(cli)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Missing origination (caller) number\n");
		return;
	}

	if (SWITCH_STATUS_SUCCESS != hv_vm_state_get_from_s3_to_mem(&req, cli, settings)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "GET for vm JSON state from S3 failed (for %s)\n", cli);
		// TODO play "you do not have voicemail"
		goto have_no_voicemail;
	} else {

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "GET for vm JSON state from S3 successful (for %s)\n", cli);

		if ((req.http_code == 200 || req.http_code == 201) && req.size > 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Got vm JSON state from S3 (for %s)\n", cli);
			json = cJSON_Parse(req.memory);
			if (!json) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Failed to parse vm JSON state from S3 into JSON (for %s)\n", cli);
				goto fail;
			}
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "No JSON state on S3 (for %s)\n", cli);
			goto have_no_voicemail;
		}
	}

	s = cJSON_Print(json);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Handling JSON state (for %s):\n%s\n", cli, s);

	{
		cJSON *vms = NULL, *vm = NULL;
		hv_http_req_t req = { 0 };
		char voicemail_path[2*HV_BUFLEN] = { 0 };

		vms = cJSON_GetObjectItemCaseSensitive(json, HV_JSON_NEW_VOICEMAILS_ARRAY_NAME);
		if (!vms || !cJSON_IsArray(vms)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error parsing JSON (for %s), %s missing\n", cli, HV_JSON_NEW_VOICEMAILS_ARRAY_NAME);
			goto fail;
		}

		cJSON_ArrayForEach(vm, vms) {

			cJSON *name = cJSON_GetObjectItemCaseSensitive(vm, HV_JSON_KEY_VOICEMAIL_NAME);
			cJSON *timestamp = cJSON_GetObjectItemCaseSensitive(vm, HV_JSON_KEY_VOICEMAIL_TIMESTAMP);

			if (!name || !timestamp || !cJSON_IsString(name) || !cJSON_IsString(timestamp)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error parsing JSON (for %s)\n", cli);
				err = 1;
				continue;
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "VM: processing %s (for %s)\n", name->valuestring, cli);

			snprintf(voicemail_path, sizeof(voicemail_path), "%s/%s", settings->file_system_folder_in, name->valuestring);

			if (SWITCH_STATUS_SUCCESS != hv_file_name_to_s3_url(name->valuestring, cli, req.url, sizeof(req.url), settings)) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to create S3 resource URL\n");
				err = 1;
				continue;
			}

			if (SWITCH_STATUS_SUCCESS != hv_http_get_to_disk(&req, voicemail_path)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to save voicemail to disk at %s (for %s)\n", voicemail_path, cli);
				err = 1;
				continue;
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "VM: %s saved to disk (%s) (for %s)\n", name->valuestring, voicemail_path, cli);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "VM: playback start %s (for %s)\n", name->valuestring, cli);


			
			{
				switch_input_args_t args = { 0 };
				switch_cc_t cc = { 0 };

				switch_file_handle_t fh = { 0 };
				memset(&fh, 0, sizeof(fh));

				//args.input_callback = cancel_on_dtmf;

				//cc.profile = profile;
				cc.fh = &fh;
				cc.noexit = 1;
				args.buf = &cc;
				switch_ivr_play_file(session, &fh, voicemail_path, &args);
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "VM: playback end %s (for %s)\n", name->valuestring, cli);
	
			if (!settings->cache_enabled) {
				unlink(voicemail_path);
			}
		}
	}

	if (err) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Voicemail retrieval failed (for %s)\n", cli);
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Voicemail retrieval OK (for %s)\n", cli);
	}

	if (s) {
		free(s);
		s = NULL;
	}

	hv_http_req_destroy(&req);
	cJSON_Delete(json);
	return;

fail:
	if (s) {
		free(s);
		s = NULL;
	}
	hv_http_req_destroy(&req);
	if (json) cJSON_Delete(json);
	return;

have_no_voicemail:
	if (s) {
		free(s);
		s = NULL;
	}
	hv_http_req_destroy(&req);
	if (json) cJSON_Delete(json);
	return;
}
