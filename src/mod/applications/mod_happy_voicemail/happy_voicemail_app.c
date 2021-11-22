#include <mod_happy_voicemail.h>


void hv_deposit_app_exec(switch_core_session_t *session, const char *file_path, hv_settings_t *settings)
{
	switch_channel_t *channel = NULL;
	switch_file_handle_t fh = { 0 };
	switch_input_args_t args = { 0 };
	char input[10] = "";
	switch_codec_implementation_t read_impl = { 0 };
	char *cld = NULL;
	char prompt[4*HV_BUFLEN] = { 0 };

	if (!session || zstr(file_path) || !settings) {
		return;
	}

	channel = switch_core_session_get_channel(session);
	if (!channel) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Channel missing\n");
		goto fail;
	}

	{
		switch_event_t *event = NULL;
		char *v = NULL;

		if ((SWITCH_STATUS_SUCCESS != switch_event_create_plain(&event, SWITCH_EVENT_CHANNEL_DATA)) || !event) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Cannot get destination (callee) number (dump event failed)\n");
			goto fail;
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
			return;
		}

		cld = strdup(v);

		switch_event_destroy(&event);
	}

	if (zstr(cld)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Missing destination (callee) number\n");
		goto fail;
	}

	snprintf(prompt, sizeof(prompt), "This is a voicemail service for %s. Please leave your message after the tone.", cld);
	hv_ivr_speak_text(prompt, session, settings, NULL);

	switch_core_session_get_read_impl(session, &read_impl);
	switch_ivr_gentones(session, settings->tone_spec, 0, NULL);

	memset(&fh, 0, sizeof(fh));
	fh.thresh = settings->record_silence_threshold;
	fh.silence_hits = settings->record_silence_hits;
	fh.samplerate = settings->record_sample_rate;

	memset(input, 0, sizeof(input));
	args.buf = input;
	args.buflen = sizeof(input);

	unlink(file_path);

	switch_channel_set_variable(channel, "record_post_process_exec_api", "happy_voicemail_upload");
	switch_channel_set_variable(channel, "record_check_silence", settings->record_check_silence ? "true" : "false");

	switch_ivr_record_file(session, &fh, file_path, &args, settings->record_max_len);

	if (cld) {
		free(cld);
	}
	return;

fail:
	snprintf(prompt, sizeof(prompt), "Service temporarily unavailable.");
	hv_ivr_speak_text(prompt, session, settings, NULL);
	if (cld) {
		free(cld);
	}
}

void hv_retrieval_app_exec(switch_core_session_t *session, const char *data, hv_settings_t *settings)
{
	char *cli = NULL;
	char *pin_var = NULL;
	cJSON *json = NULL;
	hv_http_req_t req = { 0 }; //upload = { 0 };
	char *s = NULL;
	switch_channel_t *channel = NULL;
	char prompt[4*HV_BUFLEN] = { 0 };
	uint64_t pin = 0;

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
		char *v = NULL;

		if ((SWITCH_STATUS_SUCCESS != switch_event_create_plain(&event, SWITCH_EVENT_CHANNEL_DATA)) || !event) {
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

		v = switch_event_get_header(event, HV_VARIABLE_DIALED_EXTENSION);
		if (zstr(v)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Missing origination (caller) number\n");
			switch_event_destroy(&event);
			return;
		}

		cli = strdup(v);

		v = switch_event_get_header(event, HV_VARIABLE_PIN_NAME);
		if (zstr(v)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Missing PIN number\n");
			switch_event_destroy(&event);
			free(cli);
			return;
		}

		pin_var = strdup(v);

		switch_event_destroy(&event);
	}

	if (zstr(cli)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Missing origination (caller) number\n");
		return;
	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "Got origination (caller) number: %s\n", cli);
	}

	if (zstr(pin_var)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "Missing PIN number\n");
		if (settings->pin_check) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Missing PIN number, but it is required\n");
			free(cli);
			return;
		}
	} else {
		pin = strtoull(pin_var, NULL, 10);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "Got PIN number: %s (%zu)\n", pin_var, pin);
	}

	if (SWITCH_STATUS_SUCCESS != hv_vm_state_get_from_s3_to_mem(&req, cli, settings)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "GET for vm JSON state from S3 failed (for %s)\n", cli);
		goto voicemail_not_enabled;
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
			if (!settings->vm_state_create) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "JSON state missing on S3 and won't get created cause vm-state-create not set (for %s)\n", cli);
				goto fail;
			}
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Creating JSON state on S3 (for %s)\n", cli);
			json = hv_json_vm_state_create();
			if (!json) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to create JSON state (for %s)\n", cli);
				goto fail;
			}
		}
	}

	s = cJSON_Print(json);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Handling JSON state (for %s):\n%s\n", cli, s);

	snprintf(prompt, sizeof(prompt), "This is a voicemail service for %s.", cli);
	hv_ivr_speak_text(prompt, session, settings, NULL);

	hv_ivr_run(session, json, cli, pin, settings);

	if (s) {
		free(s);
		s = NULL;
	}

	hv_http_req_destroy(&req);
	cJSON_Delete(json);
	if (cli) {
		free(cli);
	}
	if (pin_var) {
		free(pin_var);
	}
	return;

fail:
	hv_ivr_speak_text("Service temporarily not available", session, settings, NULL);
	if (s) {
		free(s);
		s = NULL;
	}
	hv_http_req_destroy(&req);
	if (json) cJSON_Delete(json);
	if (cli) {
		free(cli);
	}
	if (pin_var) {
		free(pin_var);
	}
	return;

voicemail_not_enabled:
	hv_ivr_speak_text("You do not have voicemail enabled", session, settings, NULL);
	if (s) {
		free(s);
		s = NULL;
	}
	hv_http_req_destroy(&req);
	if (json) cJSON_Delete(json);
	if (cli) {
		free(cli);
	}
	if (pin_var) {
		free(pin_var);
	}
	return;
}

void hv_s3_test_app_exec(switch_core_session_t *session, const char *data, hv_settings_t *settings)
{
	hv_http_req_t req = { 0 };

	// GET
	if (SWITCH_STATUS_SUCCESS != hv_http_req_prepare("test.txt", "test/cli", req.url, sizeof(req.url), settings, &req)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to create S3 resource URL\n");
		return;
	}
	if (SWITCH_STATUS_SUCCESS != hv_http_get_to_disk(&req, "/tmp/test.txt")) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed S3 test\n");
		return;
	}
	hv_http_req_destroy(&req);

	// PUT
	memset(&req, 0, sizeof(req));
	if (SWITCH_STATUS_SUCCESS != hv_http_req_prepare("test_uploaded.txt", "test/cli", req.url, sizeof(req.url), settings, &req)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to create S3 resource URL\n");
		return;
	}
	if (SWITCH_STATUS_SUCCESS != hv_http_upload_from_disk("/tmp/test.txt", &req)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed S3 test\n");
		return;
	}
	hv_http_req_destroy(&req);

	// DELETE
	// ...but first, upload something to delete 
	memset(&req, 0, sizeof(req));
	if (SWITCH_STATUS_SUCCESS != hv_http_req_prepare("test_uploaded_z.txt", "test/cli", req.url, sizeof(req.url), settings, &req)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to create S3 resource URL\n");
		return;
	}
	if (SWITCH_STATUS_SUCCESS != hv_http_upload_from_disk("/tmp/test.txt", &req)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed S3 test\n");
		return;
	}
	hv_http_req_destroy(&req);

	// now DELETE
	memset(&req, 0, sizeof(req));
	if (SWITCH_STATUS_SUCCESS != hv_http_req_prepare("test_uploaded_z.txt", "test/cli", req.url, sizeof(req.url), settings, &req)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to create S3 resource URL\n");
		return;
	}
	if (SWITCH_STATUS_SUCCESS != hv_http_delete(&req)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed S3 test\n");
		return;
	}
	hv_http_req_destroy(&req);
}
