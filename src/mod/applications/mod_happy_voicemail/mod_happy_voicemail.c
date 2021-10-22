#include <mod_happy_voicemail.h>


#define xml_safe_free(_x) if (_x) switch_xml_free(_x); _x = NULL

SWITCH_MODULE_LOAD_FUNCTION(mod_happy_voicemail_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_happy_voicemail_shutdown);
SWITCH_MODULE_DEFINITION(mod_happy_voicemail, mod_happy_voicemail_load, mod_happy_voicemail_shutdown, NULL);

static struct {
	switch_mutex_t *mutex;
	switch_memory_pool_t *pool;
	hv_settings_t settings;
} globals;

static switch_status_t load_config(void)
{
	switch_xml_t cfg = NULL, xml = NULL, xml_settings = NULL, param = NULL;
	hv_settings_t settings = { 0 };

	if (!(xml = switch_xml_open_cfg(HAPPY_VOICEMAIL_CONFIG_FILE_PATH, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of %s failed\n", HAPPY_VOICEMAIL_CONFIG_FILE_PATH);
		return SWITCH_STATUS_TERM;
	}

	if ((xml_settings = switch_xml_child(cfg, "settings"))) {
		for (param = switch_xml_child(xml_settings, "param"); param; param = param->next) {
			char *var = (char *) switch_xml_attr_soft(param, "name");
			char *val = (char *) switch_xml_attr_soft(param, "value");

			if (!zstr(var)) {
				if (!strcasecmp(var, "tone-spec")) {
					strncpy(settings.tone_spec, val, sizeof(settings.tone_spec));
				} else if (!strcasecmp(var, "record-silence-threshold")) {
					settings.record_silence_threshold = atoi(val);
				} else if (!strcasecmp(var, "record-silence-hits")) {
					settings.record_silence_hits = atoi(val);
				} else if (!strcasecmp(var, "record-sample-rate")) {
					settings.record_sample_rate = atoi(val);
				} else if (!strcasecmp(var, "max-record-len")) {
					settings.max_record_len = atoi(val);
				}
			}
		}
	}

	switch_mutex_lock(globals.mutex);
	memcpy(&globals.settings, &settings, sizeof(settings));
	switch_mutex_unlock(globals.mutex);

	switch_xml_free(xml);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_APP(hv_deposit_app)
{
	hv_settings_t settings = { 0 };

	switch_mutex_lock(globals.mutex);
	memcpy(&settings, &globals.settings, sizeof(settings));
	switch_mutex_unlock(globals.mutex);

	hv_deposit_app_exec(session, "/tmp/vm.wav", &settings);
}

SWITCH_STANDARD_APP(hv_retrieval_app)
{
	hv_settings_t settings = { 0 };

	switch_mutex_lock(globals.mutex);
	memcpy(&settings, &globals.settings, sizeof(settings));
	switch_mutex_unlock(globals.mutex);

	hv_retrieval_app_exec(session, "/tmp/vm.wav", &settings);
}

SWITCH_STANDARD_API(hv_deposit_api)
{
	hv_settings_t settings = { 0 };

	switch_mutex_lock(globals.mutex);
	memcpy(&settings, &globals.settings, sizeof(settings));
	switch_mutex_unlock(globals.mutex);

	return hv_deposit_api_exec("", session, stream, &settings);
}

SWITCH_STANDARD_API(hv_retrieval_api)
{
	hv_settings_t settings = { 0 };

	switch_mutex_lock(globals.mutex);
	memcpy(&settings, &globals.settings, sizeof(settings));
	switch_mutex_unlock(globals.mutex);

	return hv_retrieval_api_exec("", session, stream, &settings);
}

SWITCH_MODULE_LOAD_FUNCTION(mod_happy_voicemail_load)
{
	switch_application_interface_t *app_interface = NULL;
	switch_api_interface_t *api_interface = NULL;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	memset(&globals, 0, sizeof(globals));
	globals.pool = pool;

	switch_mutex_init(&globals.mutex, SWITCH_MUTEX_NESTED, globals.pool);

	if ((status = load_config()) != SWITCH_STATUS_SUCCESS) {
		return status;
	}

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	SWITCH_ADD_APP(app_interface, "happy_voicemail_deposit", "Voicemail deposit", HAPPY_VOICEMAIL_NAME, hv_deposit_app, "happy_voicemail_deposit", SAF_NONE);
	SWITCH_ADD_APP(app_interface, "happy_voicemail_retrieval", "Voicemail retrieval", HAPPY_VOICEMAIL_NAME, hv_retrieval_app, "happy_voicemail_retrieval", SAF_NONE);

	SWITCH_ADD_API(api_interface, "happy_voicemail_deposit", "Voicemail deposit", hv_deposit_api, "happy_voicemail_deposit");
	SWITCH_ADD_API(api_interface, "happy_voicemail_retrieval", "Voicemail retrieval", hv_retrieval_api, "happy_voicemail_retrieval");

	return SWITCH_STATUS_SUCCESS;
}


SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_happy_voicemail_shutdown)
{
	switch_mutex_unlock(globals.mutex);
	switch_mutex_destroy(globals.mutex);

	return SWITCH_STATUS_SUCCESS;
}
