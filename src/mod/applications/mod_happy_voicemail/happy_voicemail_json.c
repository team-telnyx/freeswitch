#include <mod_happy_voicemail.h>


SWITCH_DECLARE(cJSON*) hv_json_vm_state_create(void)
{
	cJSON *v = NULL;

	v = cJSON_CreateObject();
	if (!v) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Cannot get JSON\n");
		goto fail;
	}

	if (!cJSON_AddArrayToObject(v, HV_JSON_NEW_VOICEMAILS_ARRAY_NAME)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Cannot add %s to JSON\n", HV_JSON_NEW_VOICEMAILS_ARRAY_NAME);
		goto fail;
	}

	return v;


fail:
	if (v) cJSON_Delete(v);
	return NULL;
}

SWITCH_DECLARE(switch_status_t) hv_json_vm_state_add_new_voicemail(cJSON *v, const char *name)
{
	cJSON *vms = NULL;
	char *s = NULL;
	cJSON *vm = NULL;

	if (!v) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "JSON missing\n");
		goto fail;
	}

	vms = cJSON_GetObjectItemCaseSensitive(v, HV_JSON_NEW_VOICEMAILS_ARRAY_NAME);
	if (!vms) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s in JSON missing\n", HV_JSON_NEW_VOICEMAILS_ARRAY_NAME);
		goto fail;
	}

	{
		switch_time_t ts = switch_micro_time_now();
		char timestamp[100] = { 0 };

		vm = cJSON_CreateObject();
		if (!vm) {
			goto fail;
		}

		snprintf(timestamp, sizeof(timestamp), "%zu", ts);
		cJSON_AddStringToObject(vm, HV_JSON_KEY_VOICEMAIL_NAME, name);
		cJSON_AddStringToObject(vm, HV_JSON_KEY_VOICEMAIL_TIMESTAMP, timestamp);
		cJSON_InsertItemInArray(vms, 0, vm);

		s = cJSON_Print(vm);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "JSON: added new voicemail:\n%s\n", s);
		if (s) free(s);
	}

	return SWITCH_STATUS_SUCCESS;


fail:

	if (vm) cJSON_Delete(vm);
	return SWITCH_STATUS_FALSE;
}
