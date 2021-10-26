#include <mod_happy_voicemail.h>

switch_status_t hv_deposit_api_exec(_In_opt_z_ const char *cmd, _In_opt_ switch_core_session_t *session, _In_ switch_stream_handle_t *stream, hv_settings_t *settings)
{
	(void) cmd;
	(void) session;
	(void) stream;
	return SWITCH_STATUS_SUCCESS;
}

switch_status_t hv_retrieval_api_exec(_In_opt_z_ const char *cmd, _In_opt_ switch_core_session_t *session, _In_ switch_stream_handle_t *stream, hv_settings_t *settings)
{
	(void) cmd;
	(void) session;
	(void) stream;
	return SWITCH_STATUS_SUCCESS;
}

switch_status_t hv_http_upload_api_exec(_In_opt_z_ const char *cmd, _In_opt_ switch_core_session_t *session, _In_ switch_stream_handle_t *stream, hv_settings_t *settings)
{
	const char *voicemail_name = NULL;
	const char *voicemail_path = NULL;
	char *cld = NULL;
	char url[HV_BUFLEN] = { 0 };
	switch_channel_t *channel = NULL;

	(void) cmd;
	(void) stream;

	if (!session || !settings) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Bad params\n");
		return SWITCH_STATUS_FALSE;
	}

	channel = switch_core_session_get_channel(session);
	if (!channel) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Channel missing\n");
		return SWITCH_STATUS_FALSE;
	}

	voicemail_name = switch_channel_get_variable(channel, "hv_voicemail_name");
	if (zstr(voicemail_name)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Voicemail file name missing\n");
		return SWITCH_STATUS_FALSE;
	}

	voicemail_path = switch_channel_get_variable(channel, "hv_voicemail_path");
	if (zstr(voicemail_path)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Voicemail file path missing\n");
		return SWITCH_STATUS_FALSE;
	}

	{
		switch_event_t *event = NULL;

		if (SWITCH_STATUS_SUCCESS != switch_event_create_plain(&event, SWITCH_EVENT_CHANNEL_DATA)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Cannot get destination (callee) number (dump event failed)\n");
			return SWITCH_STATUS_FALSE;
		}
		switch_channel_event_set_data(channel, event);

		if (settings->dump_events) {
			char *buf = NULL;
			switch_event_serialize(event, &buf, SWITCH_FALSE);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "Voicemail CHANNEL_DATA:\n%s\n", buf);
			free(buf);
		}

		cld = strdup(switch_event_get_header(event, "variable_telnyx_dialed_extension"));
		switch_event_destroy(&event);
	}
	
	if (zstr(cld)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Cannot get destination (callee) number\n");
		return SWITCH_STATUS_FALSE;
	}

	if (SWITCH_STATUS_SUCCESS != hv_file_name_to_s3_url(voicemail_name, cld, url, sizeof(url), settings)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to create S3 resource URL\n");
		if (cld) {
			free(cld);
		}
		return SWITCH_STATUS_FALSE;
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Executing voicemail upload for (CLD: %s, file: %s, s3 resource url: %s)\n", cld, voicemail_name, url);

	if (SWITCH_STATUS_SUCCESS != hv_http_upload_from_disk(voicemail_path, url)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to upload to S3\n");
		goto fail;
	}

	unlink(voicemail_path);

	{
		cJSON *vms = NULL;
		hv_http_req_t req = { 0 };
		char *s = NULL;

		if (SWITCH_STATUS_SUCCESS != hv_ext_to_s3_vm_state_url(cld, req.url, sizeof(req.url), settings)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to create S3 resource URL (for %s)\n", cld);
			goto fail;
		}

		if (SWITCH_STATUS_SUCCESS != hv_http_get_to_mem(&req)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "Failed to GET vm JSON state from S3 (for %s), will get overwritten with updated state\n", cld);
		} else {
			if (req.size > 0) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "Got vm JSON state from S3 (for %s), will be updated\n", cld);
			} else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "No JSON state on S3 (for %s), will get created\n", cld);
			}
			vms = cJSON_Parse(req.memory);
			if (!vms) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "Failed to parse vm JSON state from S3 into JSON, will get overwritten with updated state (for %s)\n", cld);
			}
		}

		hv_http_req_destroy(&req);

		if (!vms) {
			vms = hv_json_vm_state_create();
			if (!vms) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to create JSON state (for %s)\n", cld);
			}
		}

		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "Adding new voicemail to JSON state (for %s)\n", cld);

		if (SWITCH_STATUS_SUCCESS != hv_json_vm_state_add_new_voicemail(vms, voicemail_name)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed add voicemail to JSON state (for %s)\n", cld);
		}

		hv_http_req_destroy(&req);

		// upload state
		s = cJSON_Print(vms);

		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "Uploading JSON state (for %s):\n%s\n", cld, s);
		
		if (s) {
			free(s);
			s = NULL;
		}

		cJSON_Delete(vms);
	}

	free(cld);
	cld = NULL;

	return SWITCH_STATUS_SUCCESS;

fail:

	if (voicemail_path) {
		unlink(voicemail_path);;
	}

	if (cld) {
		free(cld);
	}

	return SWITCH_STATUS_FALSE;
}
