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

static switch_status_t hv_load_config(void)
{
	switch_xml_t cfg = NULL, xml = NULL, xml_settings = NULL, param = NULL;
	hv_settings_t settings = { 0 };

	if (!(xml = switch_xml_open_cfg(HV_CONFIG_FILE_NAME, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of %s failed\n", HV_CONFIG_FILE_NAME);
		return SWITCH_STATUS_TERM;
	}

	if ((xml_settings = switch_xml_child(cfg, "settings"))) {
		for (param = switch_xml_child(xml_settings, "param"); param; param = param->next) {
			char *var = (char *) switch_xml_attr_soft(param, "name");
			char *val = (char *) switch_xml_attr_soft(param, "value");

			if (!zstr(var)) {
				if (!strcasecmp(var, "tone-spec")) {
					strncpy(settings.tone_spec, val, sizeof(settings.tone_spec));
					settings.tone_spec[HV_BUFLEN-1] = '\0';
					settings.configured.tone_spec = 1;
				} else if (!strcasecmp(var, "record-silence-threshold")) {
					settings.record_silence_threshold = atoi(val);
					settings.configured.record_silence_threshold = 1;
				} else if (!strcasecmp(var, "record-silence-hits")) {
					settings.record_silence_hits = atoi(val);
					settings.configured.record_silence_hits = 1;
				} else if (!strcasecmp(var, "record-sample-rate")) {
					settings.record_sample_rate = atoi(val);
					settings.configured.record_sample_rate = 1;
				} else if (!strcasecmp(var, "record-max-len")) {
					settings.record_max_len = atoi(val);
					settings.configured.record_max_len = 1;
				} else if (!strcasecmp(var, "record-check-silence")) {
					settings.record_check_silence = atoi(val);
					settings.configured.record_check_silence = 1;
				} else if (!strcasecmp(var, "s3-url")) {
					strncpy(settings.s3_url, val, sizeof(settings.s3_url));
					settings.s3_url[HV_BUFLEN-1] = '\0';
					settings.configured.s3_url = 1;
				} else if (!strcasecmp(var, "file-system-folder-in")) {
					strncpy(settings.file_system_folder_in, val, sizeof(settings.file_system_folder_in));
					settings.file_system_folder_in[HV_BUFLEN-1] = '\0';
					settings.configured.file_system_folder_in = 1;
				} else if (!strcasecmp(var, "file-system-folder-out")) {
					strncpy(settings.file_system_folder_out, val, sizeof(settings.file_system_folder_out));
					settings.file_system_folder_out[HV_BUFLEN-1] = '\0';
					settings.configured.file_system_folder_out = 1;
				} else if (!strcasecmp(var, "dump-events")) {
					settings.dump_events = switch_true(val);
					settings.configured.dump_events = 1;
				} else if (!strcasecmp(var, "record-file-ext")) {
					strncpy(settings.record_file_ext, val, sizeof(settings.record_file_ext));
					settings.record_file_ext[HV_BUFLEN-1] = '\0';
					settings.configured.record_file_ext = 1;
				} else if (!strcasecmp(var, "cache-enabled")) {
					settings.cache_enabled = switch_true(val);
					settings.configured.cache_enabled = 1;
				} else if (!strcasecmp(var, "voice")) {
					strncpy(settings.voice, val, sizeof(settings.voice));
					settings.voice[HV_BUFLEN-1] = '\0';
					settings.configured.voice = 1;
				} else if (!strcasecmp(var, "pin-check")) {
					settings.pin_check = switch_true(val);
					settings.configured.pin_check = 1;
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

static switch_status_t hv_check_settings(hv_settings_t *settings)
{
	if (!settings) {
		return SWITCH_STATUS_FALSE;
	}

	if (!settings->configured.tone_spec) {
		strncpy(settings->tone_spec, HV_DEFAULT_BEEP, sizeof(settings->tone_spec));
		settings->tone_spec[HV_BUFLEN-1] = '\0';
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Settings missing: tone-spec (using default value: %s)\n", HV_DEFAULT_BEEP);
	}

	if (!settings->configured.record_max_len) {
		settings->record_max_len = HV_DEFAULT_RECORD_MAX_LEN_S;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Settings missing: record-max-len (using default value: %u seconds)\n", HV_DEFAULT_RECORD_MAX_LEN_S);
	}

	if (!settings->configured.record_check_silence) {
		settings->record_check_silence = switch_true(HV_DEFAULT_RECORD_CHECK_SILENCE);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Settings missing: record-check-silence (using default value: %s)\n", HV_DEFAULT_RECORD_CHECK_SILENCE);
	}

	if (!settings->configured.s3_url) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Settings missing: S3 url\n");
		return SWITCH_STATUS_FALSE;
	}

	if (!settings->configured.file_system_folder_in) {
		strncpy(settings->file_system_folder_in, HV_DEFAULT_FILE_SYSTEM_FOLDER_IN, sizeof(settings->file_system_folder_in));
		settings->file_system_folder_in[HV_BUFLEN-1] = '\0';
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Settings missing: file-system-folder-in (using default value: %s)\n", HV_DEFAULT_FILE_SYSTEM_FOLDER_IN);
	}

	if (!settings->configured.file_system_folder_out) {
		strncpy(settings->file_system_folder_out, HV_DEFAULT_FILE_SYSTEM_FOLDER_OUT, sizeof(settings->file_system_folder_out));
		settings->file_system_folder_out[HV_BUFLEN-1] = '\0';
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Settings missing: file-system-folder-out (using default value: %s)\n", HV_DEFAULT_FILE_SYSTEM_FOLDER_OUT);
	}

	if (!settings->configured.record_file_ext) {
		strncpy(settings->record_file_ext, HV_DEFAULT_RECORD_FILE_EXT, sizeof(settings->record_file_ext));
		settings->record_file_ext[HV_BUFLEN-1] = '\0';
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Settings missing: record-file-ext (using default value: %s)\n", HV_DEFAULT_RECORD_FILE_EXT);
	}

	if (!settings->configured.cache_enabled) {
		settings->cache_enabled = switch_true(HV_DEFAULT_CACHE_ENABLED);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Settings missing: cache-enabled (using default value: %s)\n", HV_DEFAULT_CACHE_ENABLED);
	}

	if (!settings->configured.voice) {
		strncpy(settings->voice, HV_DEFAULT_VOICE, sizeof(settings->voice));
		settings->voice[HV_BUFLEN-1] = '\0';
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Settings missing: voice (using default value: %s)\n", HV_DEFAULT_VOICE);
	}

	if (!settings->configured.pin_check) {
		settings->pin_check = switch_true(HV_DEFAULT_PIN_CHECK);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Settings missing: pin-check (using default value: %s)\n", HV_DEFAULT_PIN_CHECK);
	}

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status_t) hv_file_name_to_s3_url(const char *name, const char *ext, char *url, uint32_t len, hv_settings_t *settings)
{
	if (zstr(name) || zstr(ext) || !url || !len || !settings) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Bad params\n");
		return SWITCH_STATUS_FALSE;
	}

	snprintf(url, len, "%s/%s/%s", settings->s3_url, ext, name);
	url[len-1] = '\0';
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status_t) hv_ext_to_s3_vm_state_url(const char *ext, char *url, uint32_t len, hv_settings_t *settings)
{
	if (zstr(ext) || !url || !len || !settings) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Bad params\n");
		return SWITCH_STATUS_FALSE;
	}

	snprintf(url, len, "%s/%s/%s", settings->s3_url, ext, HV_JSON_S3_NAME);
	url[len-1] = '\0';
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(void) hv_get_uuid(char *buf, uint32_t len)
{
	uuid_t binuuid;
	uuid_generate_random(binuuid);

	if (len < 37) {
		return;
	}

	uuid_unparse_lower(binuuid, buf);
	buf[36] = '\0';
}

SWITCH_DECLARE(switch_status_t) hv_get_new_voicemail_name(char *buf, uint32_t len, hv_settings_t *settings)
{
	char uuid[37] = { 0 };

	if (!buf || !len || !settings) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Bad params\n");
		return SWITCH_STATUS_FALSE;
	}

	hv_get_uuid(uuid, sizeof(uuid));
	snprintf(buf, len, "%s.%s", uuid, settings->record_file_ext);
	buf[len-1] = '\0';
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_APP(hv_deposit_app)
{
	switch_channel_t *channel = NULL;
	hv_settings_t settings = { 0 };
	char voicemail_name[HV_BUFLEN] = { 0 };
	char voicemail_path[2*HV_BUFLEN] = { 0 };

	channel = switch_core_session_get_channel(session);
	if (!channel) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Channel missing\n");
		return;
	}

	switch_mutex_lock(globals.mutex);
	memcpy(&settings, &globals.settings, sizeof(settings));
	switch_mutex_unlock(globals.mutex);

	if (SWITCH_STATUS_SUCCESS != hv_get_new_voicemail_name(voicemail_name, sizeof(voicemail_name), &settings)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to create new voicemail file name\n");
		return;
	}

	switch_channel_set_variable(channel, "hv_voicemail_name", voicemail_name);

	snprintf(voicemail_path, sizeof(voicemail_path), "%s/%s", settings.file_system_folder_out, voicemail_name);
	switch_channel_set_variable(channel, "hv_voicemail_path", voicemail_path);

	switch_mutex_lock(globals.mutex);
	switch_dir_make_recursive(settings.file_system_folder_out, SWITCH_DEFAULT_DIR_PERMS, globals.pool);
	switch_mutex_unlock(globals.mutex);

	hv_deposit_app_exec(session, voicemail_path, &settings);
}

SWITCH_STANDARD_APP(hv_retrieval_app)
{
	hv_settings_t settings = { 0 };

	switch_mutex_lock(globals.mutex);
	memcpy(&settings, &globals.settings, sizeof(settings));
	switch_mutex_unlock(globals.mutex);

	switch_mutex_lock(globals.mutex);
	switch_dir_make_recursive(settings.file_system_folder_in, SWITCH_DEFAULT_DIR_PERMS, globals.pool);
	switch_mutex_unlock(globals.mutex);

	hv_retrieval_app_exec(session, NULL, &settings);
}

SWITCH_STANDARD_API(hv_deposit_api)
{
	hv_settings_t settings = { 0 };

	switch_mutex_lock(globals.mutex);
	memcpy(&settings, &globals.settings, sizeof(settings));
	switch_mutex_unlock(globals.mutex);

	switch_mutex_lock(globals.mutex);
	switch_dir_make_recursive(settings.file_system_folder_in, SWITCH_DEFAULT_DIR_PERMS, globals.pool);
	switch_mutex_unlock(globals.mutex);

	return hv_deposit_api_exec(NULL, session, stream, &settings);
}

SWITCH_STANDARD_API(hv_retrieval_api)
{
	hv_settings_t settings = { 0 };

	switch_mutex_lock(globals.mutex);
	memcpy(&settings, &globals.settings, sizeof(settings));
	switch_mutex_unlock(globals.mutex);

	return hv_retrieval_api_exec(NULL, session, stream, &settings);
}

SWITCH_STANDARD_API(hv_http_upload_api)
{
	hv_settings_t settings = { 0 };

	switch_mutex_lock(globals.mutex);
	memcpy(&settings, &globals.settings, sizeof(settings));
	switch_mutex_unlock(globals.mutex);

	if (SWITCH_STATUS_SUCCESS != hv_http_upload_api_exec(NULL, session, stream, &settings)) {
		char prompt[2*HV_BUFLEN] = { 0 };
		snprintf(prompt, sizeof(prompt), "Service temporarily unavailable. Message could not be saved.");
		hv_ivr_speak_text(prompt, session, &settings, NULL);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Voicemail deposit failed\n");
		return SWITCH_STATUS_FALSE;
	}

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_happy_voicemail_load)
{
	switch_application_interface_t *app_interface = NULL;
	switch_api_interface_t *api_interface = NULL;

	memset(&globals, 0, sizeof(globals));
	globals.pool = pool;

	switch_mutex_init(&globals.mutex, SWITCH_MUTEX_NESTED, globals.pool);

	if (SWITCH_STATUS_SUCCESS != hv_load_config()) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Cannot load settings\n");
		return SWITCH_STATUS_FALSE;
	}

	if (SWITCH_STATUS_SUCCESS != hv_check_settings(&globals.settings)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Settings invalid\n");
		return SWITCH_STATUS_FALSE;
	}

	switch_dir_make_recursive(globals.settings.file_system_folder_in, SWITCH_DEFAULT_DIR_PERMS, globals.pool);
	switch_dir_make_recursive(globals.settings.file_system_folder_out, SWITCH_DEFAULT_DIR_PERMS, globals.pool);

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	SWITCH_ADD_APP(app_interface, "happy_voicemail_deposit", "Voicemail deposit", HV_NAME, hv_deposit_app, "happy_voicemail_deposit", SAF_NONE);
	SWITCH_ADD_APP(app_interface, "happy_voicemail_retrieval", "Voicemail retrieval", HV_NAME, hv_retrieval_app, "happy_voicemail_retrieval", SAF_NONE);

	SWITCH_ADD_API(api_interface, "happy_voicemail_deposit", "Voicemail deposit", hv_deposit_api, "happy_voicemail_deposit");
	SWITCH_ADD_API(api_interface, "happy_voicemail_retrieval", "Voicemail retrieval", hv_retrieval_api, "happy_voicemail_retrieval");
	SWITCH_ADD_API(api_interface, "happy_voicemail_upload", "Voicemail upload", hv_http_upload_api, "happy_voicemail_upload");

	return SWITCH_STATUS_SUCCESS;
}


SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_happy_voicemail_shutdown)
{
	//switch_mutex_unlock(globals.mutex);
	switch_mutex_destroy(globals.mutex);

	return SWITCH_STATUS_SUCCESS;
}
