#include <mod_happy_voicemail.h>


SWITCH_DECLARE(switch_status_t) hv_vm_state_get_from_s3_to_mem(hv_http_req_t *req, const char *ext, hv_settings_t *settings)
{
	if (!req || zstr(ext) || !settings) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Bad params (for %s)\n", ext);
		return SWITCH_STATUS_FALSE;
	}

	if (SWITCH_STATUS_SUCCESS != hv_ext_to_s3_vm_state_url(ext, req->url, sizeof(req->url), settings)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to create S3 resource URL (for %s)\n", ext);
		return SWITCH_STATUS_FALSE;
	}

	return hv_http_get_to_mem(req);
}

SWITCH_DECLARE(switch_status_t) hv_vm_state_upload(const char *ext, cJSON *vms, hv_settings_t *settings)
{
	hv_http_req_t upload = { 0 };
	char *s = NULL;

	if (!vms || zstr(ext) || !settings) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Bad params (for %s)\n", ext);
		return SWITCH_STATUS_FALSE;
	}

	if (SWITCH_STATUS_SUCCESS != hv_ext_to_s3_vm_state_url(ext, upload.url, sizeof(upload.url), settings)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to create S3 resource URL (for %s)\n", ext);
		return SWITCH_STATUS_FALSE;
	}

	s = cJSON_Print(vms);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Uploading JSON state (for %s):\n%s\n", ext, s);

	upload.memory = s;
	upload.sizeleft = strlen(s);
	if (SWITCH_STATUS_SUCCESS != hv_http_upload_from_mem(&upload)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to upload JSON state to S3 (for %s)\n", ext);
		goto fail;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Upload OK (for %s)\n", ext);

	if (s) {
		free(s);
		s = NULL;
	}

	return SWITCH_STATUS_SUCCESS;

fail:
	if (s) {
		free(s);
		s = NULL;
	}
	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) hv_vm_state_update(const char *cld, const char *voicemail_name, hv_settings_t *settings)
{
	cJSON *vms = NULL;
	hv_http_req_t req = { 0 };

	if (zstr(cld) || zstr(voicemail_name) || !settings) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Bad params (for %s)\n", cld);
		goto fail;
	}

	if (SWITCH_STATUS_SUCCESS != hv_vm_state_get_from_s3_to_mem(&req, cld, settings)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "GET for vm JSON state from S3 failed (for %s)\n", cld);
		goto fail;
	} else {

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "GET for vm JSON state from S3 successful (for %s)\n", cld);

		if ((req.http_code == 200 || req.http_code == 201) && req.size > 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Got vm JSON state from S3 (for %s), will get updated\n", cld);
			vms = cJSON_Parse(req.memory);
			if (!vms) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Failed to parse vm JSON state from S3 into JSON (for %s)\n", cld);
				goto fail;
			}
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "No JSON state on S3 (for %s), will get created\n", cld);
			if (!vms) {
				vms = hv_json_vm_state_create();
				if (!vms) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to create JSON state (for %s)\n", cld);
				}
			}
		}
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Adding new voicemail to JSON state (for %s)\n", cld);

	if (SWITCH_STATUS_SUCCESS != hv_json_vm_state_add_new_voicemail(vms, voicemail_name)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to add voicemail to JSON state (for %s)\n", cld);
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Updating remote JSON state (for %s)\n", cld);

	if (SWITCH_STATUS_SUCCESS != hv_vm_state_upload(cld, vms, settings)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to upload JSON state to S3 (for %s)\n", cld);
		goto fail;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "State updated (for %s)\n", cld);

	hv_http_req_destroy(&req);
	cJSON_Delete(vms);
	return SWITCH_STATUS_SUCCESS;

fail:
	hv_http_req_destroy(&req);
	if (vms) cJSON_Delete(vms);
	return SWITCH_STATUS_FALSE;
}
