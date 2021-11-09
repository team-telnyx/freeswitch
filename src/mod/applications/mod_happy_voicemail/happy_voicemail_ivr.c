#include <mod_happy_voicemail.h>


SWITCH_DECLARE(void) hv_ivr_speak_text(const char *text, switch_core_session_t *session, hv_settings_t *settings)
{
	if (!session || zstr(text) || !settings) {
		return;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "VM: IVR playing: %s \n", text);
	switch_ivr_speak_text(session, "flite", settings->voice, text, NULL);
}

static const char* hv_ivr_int_to_cardinal(int i)
{
	switch (i) {
		case 1:
			return "first";
		case 2:
			return "second";
		case 3:
			return "third";
		case 4:
			return "fourth";
		case 5:
			return "fifth";
		case 6:
			return "sixth";
		case 7:
			return "seventh";
		case 8:
			return "eighth";
		case 9:
			return "ninth";
		case 10:
			return "tenth";
		case 11:
			return "eleventh";
		case 12:
			return "twelfth";
		case 13:
			return "thirteenth";
		case 14:
			return "fourteenth";
		case 15:
			return "fifteenth";
		case 16:
			return "sixteenth";
		case 17:
			return "seventeenth";
		case 18:
			return "eighteenth";
		case 19:
			return "nineteenth";
		case 20:
			return "twentieth";
		case 21:
			return "twenty first";
		case 22:
			return "twenty second";
		case 23:
			return "twenty third";
		case 24:
			return "twenty fourth";
		case 25:
			return "twenty fifth";
		case 26:
			return "twenty sixth";
		case 27:
			return "twenty seventh";
		case 28:
			return "twenty eighth";
		case 29:
			return "twenty ninth";
		case 30:
			return "thirty";
		case 31:
			return "thirty first";
		default:
			return "next";
	}
}

static void hv_ivr_play_message_desc(int i, char *ts, switch_core_session_t *session, hv_settings_t *settings)
{
	char prompt[4*HV_BUFLEN] = { 0 };
	time_t timestamp_us = 0;
	time_t timestamp_s = 0;
	char day[10] = { 0 };
	int day_n = 0;
	char day2[20] = { 0 };
	char ts_human[100] = { 0 };
	struct tm t = { 0};

	if (!ts || !session || !settings) {
		return;
	}

	timestamp_us = strtoull(ts, NULL, 10);
	timestamp_s = timestamp_us / 1000000;
	gmtime_r(&timestamp_s, &t);
	strftime(day, 10, "%d", &t);
	day_n = atoi(day);
	strftime(day2, 20, "%A", &t);
	strftime(ts_human, 100, "%B, at %I:%M %p", &t);

	snprintf(prompt, sizeof(prompt), "%s message. Message received: %s %s %s.", hv_ivr_int_to_cardinal(i), day2, hv_ivr_int_to_cardinal(day_n), ts_human);
	hv_ivr_speak_text(prompt, session, settings);
}

SWITCH_DECLARE(void) hv_ivr_run(switch_core_session_t *session, cJSON *vm_state, char *cli, hv_settings_t *settings)
{
	cJSON *vms = NULL, *vm = NULL;
	hv_http_req_t req = { 0 };
	char voicemail_path[2*HV_BUFLEN] = { 0 };
	char prompt[4*HV_BUFLEN] = { 0 };
	int vms_n = 0, i = 0;
	int err = 0;
	int loops = 0;

	if (!session || !vm_state || zstr(cli) || !settings) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Bad params\n");
		goto fail;
	}

	vms = cJSON_GetObjectItemCaseSensitive(vm_state, HV_JSON_NEW_VOICEMAILS_ARRAY_NAME);
	if (!vms || !cJSON_IsArray(vms)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error parsing JSON (for %s), %s missing\n", cli, HV_JSON_NEW_VOICEMAILS_ARRAY_NAME);
		goto fail;
	}

	// Prompt
	vms_n = cJSON_GetArraySize(vms);
	if (vms_n > 0) {
		snprintf(prompt, sizeof(prompt), "You have %d message%s", vms_n, vms_n == 1 ? "." : "s.");
		hv_ivr_speak_text(prompt, session, settings);
	} else {
		snprintf(prompt, sizeof(prompt), "You have no messages.");
		hv_ivr_speak_text(prompt, session, settings);
		goto end;
	}

	if (!switch_channel_ready(switch_core_session_get_channel(session))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Channel closed (for %s)\n", cli);
		goto end;
	}

menu_main:
	i = 0;
	loops++;
	snprintf(prompt, sizeof(prompt), "To listen to your messages: press 1. To hangup: press 2 or hangup.");
	hv_ivr_speak_text(prompt, session, settings);

	cJSON_ArrayForEach(vm, vms) {

		cJSON *name = cJSON_GetObjectItemCaseSensitive(vm, HV_JSON_KEY_VOICEMAIL_NAME);
		cJSON *timestamp = cJSON_GetObjectItemCaseSensitive(vm, HV_JSON_KEY_VOICEMAIL_TIMESTAMP);

		if (!name || !timestamp || !cJSON_IsString(name) || !cJSON_IsString(timestamp)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error parsing JSON (for %s)\n", cli);
			err = 1;
			continue;
		}

		if (!switch_channel_ready(switch_core_session_get_channel(session))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Channel closed (for %s)\n", cli);
			goto cleanup;
		}

		i++;

		// Message description
		hv_ivr_play_message_desc(i, timestamp->valuestring, session, settings);

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "VM: processing %s (for %s)\n", name->valuestring, cli);

		snprintf(voicemail_path, sizeof(voicemail_path), "%s/%s", settings->file_system_folder_in, name->valuestring);

		if (SWITCH_STATUS_SUCCESS != switch_file_exists(voicemail_path, switch_core_session_get_pool(session))) {

			if (!switch_channel_ready(switch_core_session_get_channel(session))) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Channel closed (for %s)\n", cli);
				goto cleanup;
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "VM: need to download %s (for %s)\n", name->valuestring, cli);

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
		}

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

		// SUBMENU
		snprintf(prompt, sizeof(prompt), "To listen to next message: press 1. To delete: press 2, to hangup: press 3 or hangup.");
		hv_ivr_speak_text(prompt, session, settings);
	}

	snprintf(prompt, sizeof(prompt), "End of messages.");
	hv_ivr_speak_text(prompt, session, settings);
	if (loops < HV_MENU_LOOPS_MAX_N) {
		goto menu_main;
	}

cleanup:

	if (!settings->cache_enabled) {

		// Remove messages from local storage

		cJSON_ArrayForEach(vm, vms) {

			cJSON *name = cJSON_GetObjectItemCaseSensitive(vm, HV_JSON_KEY_VOICEMAIL_NAME);

			snprintf(voicemail_path, sizeof(voicemail_path), "%s/%s", settings->file_system_folder_in, name->valuestring);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "VM: removing %s from local storage %s (for %s)\n", name->valuestring, voicemail_path, cli);

			unlink(voicemail_path);
		}
	}

end:

	if (err) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Voicemail retrieval failed (for %s)\n", cli);
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Voicemail retrieval OK (for %s)\n", cli);
	}

	return;

fail:
	return;
}
