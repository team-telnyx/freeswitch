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
		char *v = NULL;

		if ((SWITCH_STATUS_SUCCESS != switch_event_create_plain(&event, SWITCH_EVENT_CHANNEL_DATA)) || !event) {
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

		v = switch_event_get_header(event, HV_VARIABLE_DIALED_EXTENSION);
		if (zstr(v)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Missing destination (callee) number\n");
			switch_event_destroy(&event);
			return SWITCH_STATUS_FALSE;
		}

		cld = strdup(v);
		switch_event_destroy(&event);
	}

	if (zstr(cld)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Missing destination (callee) number\n");
		return SWITCH_STATUS_FALSE;
	}

	if (SWITCH_STATUS_SUCCESS != hv_file_name_to_s3_url(voicemail_name, cld, url, sizeof(url), settings)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to create S3 resource URL\n");
		goto fail;
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Executing voicemail upload for (CLD: %s, file: %s, s3 resource url: %s)\n", cld, voicemail_name, url);

	if (SWITCH_STATUS_SUCCESS != hv_http_upload_from_disk(voicemail_path, url)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to upload to S3\n");
		goto fail;
	}

	unlink(voicemail_path);

	if (SWITCH_STATUS_SUCCESS != hv_vm_state_update(cld, voicemail_name, settings)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to update vm JSON state on S3 (for %s)\n", cld);
		goto fail;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Voicemail deposit OK (for %s)\n", cld);

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
