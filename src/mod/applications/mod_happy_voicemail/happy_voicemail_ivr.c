#include <mod_happy_voicemail.h>


SWITCH_DECLARE(void) hv_ivr_speak_text(const char *text, switch_core_session_t *session, hv_settings_t *settings, switch_input_args_t *args)
{
	if (!session || zstr(text) || !settings) {
		return;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "IVR playing: %s \n", text);
	switch_ivr_speak_text(session, "flite", settings->voice, text, args);
}

SWITCH_DECLARE(void) hv_ivr_timeout_set_ms(hv_ivr_timeout_t *t, switch_time_t ms)
{
	if (!t) {
		return;
	}
	t->start_ms = switch_micro_time_now() / 1000;
	t->timeout_ms = ms;
}

SWITCH_DECLARE(uint8_t) hv_ivr_timeout_expired(hv_ivr_timeout_t *t)
{
	switch_time_t now_ms = switch_micro_time_now() / 1000;
	if (!t) {
		return 0;
	}
	return (t->start_ms + t->timeout_ms < now_ms ? 1 : 0);
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

SWITCH_DECLARE(switch_status_t) hv_ivr_pin_check(switch_core_session_t *session, uint64_t pin, hv_settings_t *settings)
{
	char digits[65] = { 0 };
	char pressed = 0;
	int digits_n = 0;
	switch_channel_t *channel = NULL;
	hv_ivr_timeout_t total_timeout = { 0 };
	hv_ivr_timeout_t single_digit_timeout = { 0 };
	char prompt[4*HV_BUFLEN] = { 0 };
	uint64_t pin_collected = 0;

	channel = switch_core_session_get_channel(session);
	if (!channel) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Channel missing\n");
		goto fail;
	}

	switch_channel_flush_dtmf(channel);
	hv_ivr_timeout_set_ms(&single_digit_timeout, HV_IVR_TIMEOUT_SINGLE_DIGIT_MS);
	hv_ivr_timeout_set_ms(&total_timeout, HV_IVR_TIMEOUT_TOTAL_MS);

	while (!hv_ivr_timeout_expired(&total_timeout) && switch_channel_ready(channel)) {

		switch_channel_flush_dtmf(channel);
		hv_ivr_timeout_set_ms(&single_digit_timeout, HV_IVR_TIMEOUT_SINGLE_DIGIT_MS);
		pressed = 0;

		while (!hv_ivr_timeout_expired(&single_digit_timeout) && switch_channel_ready(channel)) {

			if (switch_channel_has_dtmf(channel)) {
				switch_dtmf_t d = { 0 };
				switch_channel_dequeue_dtmf(channel, &d);
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "DTMF: PIN: %c\n", d.digit);

				pressed = d.digit;
				digits[digits_n] = pressed;
				if (pressed == HV_IVR_PIN_TERMINATOR_CHAR || digits_n == 64) {
					digits[digits_n] = 0;
					pin_collected = strtoull(digits, NULL, 10);
					if (pin == pin_collected) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "DTMF: PIN: correct (%s)\n", digits);
						return SWITCH_STATUS_SUCCESS;
					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "DTMF: PIN: invalid (%s)\n", digits);
						return SWITCH_STATUS_FALSE;
					}
				}

				digits[digits_n] = pressed;
				digits_n++;
				break;
			}
		}

		if (!switch_channel_ready(channel)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Channel closed\n");
			return SWITCH_STATUS_BREAK;
		}

		if (!pressed) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "DTMF: PIN: single digit timeout\n");
			break;
		}
	}

	if (!switch_channel_ready(channel)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Channel closed\n");
		return SWITCH_STATUS_BREAK;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "DTMF: PIN: timeout\n");
	snprintf(prompt, sizeof(prompt), "Time out.");
	hv_ivr_speak_text(prompt, session, settings, NULL);

fail:
	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) hv_ivr_on_dtmf(switch_core_session_t *session, void *input, switch_input_type_t itype, void *buf, unsigned int buflen)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Input callback: called for reason %d\n", itype);

	switch (itype) {
	case SWITCH_INPUT_TYPE_DTMF:
		{
			switch_dtmf_t *dtmf = (switch_dtmf_t *) input;
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "DTMF: %c pressed\n", dtmf->digit);
			if (buf && buflen) {
				char *bp = (char *) buf;
				*bp = dtmf->digit;
			}
			return SWITCH_STATUS_BREAK;
		}
		break;
	default:
		break;
	}

	return SWITCH_STATUS_SUCCESS;
}

static void hv_ivr_play_message_desc(int i, char *ts, switch_core_session_t *session, hv_settings_t *settings, switch_input_args_t *args)
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
	hv_ivr_speak_text(prompt, session, settings, args);
}

SWITCH_DECLARE(void) hv_ivr_run(switch_core_session_t *session, cJSON *vm_state, char *cli, uint64_t pin, hv_settings_t *settings)
{
	cJSON *vms = NULL, *vms_original = NULL, *vm = NULL;
	hv_http_req_t req = { 0 };
	char voicemail_path[2*HV_BUFLEN] = { 0 };
	char prompt[4*HV_BUFLEN] = { 0 };
	int vms_n = 0, i = 0;
	int err = 0;
	int loops = 0;
	int main_menu_loops = 0;
	int sub_menu_loops = 0;
	int pin_check_loops = 0;
	char dtmf = 0;
	hv_ivr_timeout_t timeout = { 0 };
	switch_channel_t *channel = NULL;

	switch_input_args_t dtmf_args = { .buf = &dtmf, .buflen = 1, .input_callback = hv_ivr_on_dtmf };

	if (!session || !vm_state || zstr(cli) || !settings) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Bad params\n");
		goto fail;
	}

	channel = switch_core_session_get_channel(session);
	if (!channel) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Channel missing\n");
		goto fail;
	}

	vms = cJSON_GetObjectItemCaseSensitive(vm_state, HV_JSON_NEW_VOICEMAILS_ARRAY_NAME);
	if (!vms || !cJSON_IsArray(vms)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error parsing JSON (for %s), %s missing\n", cli, HV_JSON_NEW_VOICEMAILS_ARRAY_NAME);
		goto fail;
	}

	vms_original = cJSON_Duplicate(vms, 1);
	if (!vms_original || !cJSON_IsArray(vms_original)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error parsing JSON (for %s)\n", cli);
		goto fail;
	}

	// Check PIN
	if (settings->pin_check) {
		snprintf(prompt, sizeof(prompt), "Please enter your PIN, followed by the %s.", HV_IVR_PIN_TERMINATOR_CHAR_NAME);
		hv_ivr_speak_text(prompt, session, settings, NULL);
		while (switch_channel_ready(channel) && (SWITCH_STATUS_SUCCESS != hv_ivr_pin_check(session, pin, settings))) {
			if (!switch_channel_ready(channel)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Channel closed (for %s)\n", cli);
				goto end;
			}
			pin_check_loops++;
			if (pin_check_loops < HV_IVR_LOOPS_MAX_PIN_CHECK) {
				snprintf(prompt, sizeof(prompt), "PIN is invalid.");
				hv_ivr_speak_text(prompt, session, settings, NULL);
				snprintf(prompt, sizeof(prompt), "Please try again.");
				hv_ivr_speak_text(prompt, session, settings, NULL);
			} else {
				snprintf(prompt, sizeof(prompt), "You've reached maximum number of failures, please dial again.");
				hv_ivr_speak_text(prompt, session, settings, NULL);
				goto end;
			}
		}

		snprintf(prompt, sizeof(prompt), "PIN correct.");
		hv_ivr_speak_text(prompt, session, settings, NULL);
	}

	if (!switch_channel_ready(channel)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Channel closed (for %s)\n", cli);
		goto end;
	}

	// Prompt
	vms_n = cJSON_GetArraySize(vms);
	if (vms_n > 0) {
		snprintf(prompt, sizeof(prompt), "You have %d message%s", vms_n, vms_n == 1 ? "." : "s.");
		hv_ivr_speak_text(prompt, session, settings, NULL);
	} else {
		snprintf(prompt, sizeof(prompt), "You have no messages.");
		hv_ivr_speak_text(prompt, session, settings, NULL);
		goto end;
	}

menu_main:

	if (!switch_channel_ready(channel)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Channel closed (for %s)\n", cli);
		goto cleanup;
	}

	i = 0;
	loops++;
	main_menu_loops++;
	sub_menu_loops = 0;

	switch_channel_flush_dtmf(channel);
	dtmf = 0;

	snprintf(prompt, sizeof(prompt), "To listen to your messages: ");
	hv_ivr_speak_text(prompt, session, settings, &dtmf_args);
	if (!dtmf) {
		snprintf(prompt, sizeof(prompt), "press 1. ");
		hv_ivr_speak_text(prompt, session, settings, &dtmf_args);
		if (!dtmf) {
			snprintf(prompt, sizeof(prompt), "To hangup: ");
			hv_ivr_speak_text(prompt, session, settings, &dtmf_args);
			if (!dtmf) {
				snprintf(prompt, sizeof(prompt), "press 2, or simply hangup.");
				hv_ivr_speak_text(prompt, session, settings, &dtmf_args);
			}
		}
	}

	if (!switch_channel_ready(channel)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Channel closed (for %s)\n", cli);
		goto cleanup;
	}

	hv_ivr_timeout_set_ms(&timeout, 10000);

	while (!dtmf && switch_channel_ready(channel)) {
		if (channel) {
			if (switch_channel_has_dtmf(channel)) {
				switch_dtmf_t d = { 0 };
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "DTMF: channel has DTMF\n");
				switch_channel_dequeue_dtmf(channel, &d);
				hv_ivr_on_dtmf(session, (void *) &d, SWITCH_INPUT_TYPE_DTMF, dtmf_args.buf, dtmf_args.buflen);
			}
		}
		if (hv_ivr_timeout_expired(&timeout)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Main menu: Timeout (for %s)\n", cli);
			break;
		}
	}

	if (!switch_channel_ready(channel)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Channel closed (for %s)\n", cli);
		goto cleanup;
	}

	if (switch_channel_has_dtmf(channel)) {
		switch_dtmf_t d = { 0 };
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "DTMF: channel has DTMF\n");
		switch_channel_dequeue_dtmf(channel, &d);
		hv_ivr_on_dtmf(session, (void *) &d, SWITCH_INPUT_TYPE_DTMF, dtmf_args.buf, dtmf_args.buflen);
	}

	if (dtmf) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Main menu: DTMF %c pressed (for %s)\n", dtmf, cli);
		if (dtmf == '1') {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Sub menu: LISTEN (for %s)\n", cli);
		} else if (dtmf == '2') {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Sub menu: HANGUP (for %s)\n", cli);
			goto cleanup;
		} else if (dtmf == '3') {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Sub menu: BAD ENTRY (for %s)\n", cli);
			if (main_menu_loops < HV_IVR_LOOPS_MAX_MAIN_MENU) {
				snprintf(prompt, sizeof(prompt), "Please press 1, 2, or hangup.");
				hv_ivr_speak_text(prompt, session, settings, &dtmf_args);
				goto menu_main;
			} else {
				snprintf(prompt, sizeof(prompt), "You've reached maximum number of failures, please dial again.");
				hv_ivr_speak_text(prompt, session, settings, NULL);
				goto cleanup;
			}
		}
	} else {
		if (main_menu_loops < HV_IVR_LOOPS_MAX_MAIN_MENU) {
			snprintf(prompt, sizeof(prompt), "Please try again.");
			hv_ivr_speak_text(prompt, session, settings, &dtmf_args);
			goto menu_main;
		} else {
			snprintf(prompt, sizeof(prompt), "You've reached maximum number of failures, please dial again.");
			hv_ivr_speak_text(prompt, session, settings, NULL);
			goto cleanup;
		}
	}

	if (!switch_channel_ready(channel)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Channel closed (for %s)\n", cli);
		goto cleanup;
	}

iterate_messages:
	cJSON_ArrayForEach(vm, vms) {

		cJSON *name = cJSON_GetObjectItemCaseSensitive(vm, HV_JSON_KEY_VOICEMAIL_NAME);
		cJSON *timestamp = cJSON_GetObjectItemCaseSensitive(vm, HV_JSON_KEY_VOICEMAIL_TIMESTAMP);
		
		if (!switch_channel_ready(channel)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Channel closed (for %s)\n", cli);
			goto cleanup;
		}

		if (!name || !timestamp || !cJSON_IsString(name) || !cJSON_IsString(timestamp)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error parsing JSON (for %s)\n", cli);
			err = 1;
			continue;
		}

		switch_channel_flush_dtmf(channel);
		dtmf = 0;

		i++;

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Processing %s (for %s)\n", name->valuestring, cli);
		snprintf(voicemail_path, sizeof(voicemail_path), "%s/%s", settings->file_system_folder_in, name->valuestring);

		hv_ivr_play_message_desc(i, timestamp->valuestring, session, settings, &dtmf_args);
		if (dtmf) {
			goto process_playback_dtmf;
		}

		if (!switch_channel_ready(channel)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Channel closed (for %s)\n", cli);
			goto cleanup;
		}

		if (SWITCH_STATUS_SUCCESS != switch_file_exists(voicemail_path, switch_core_session_get_pool(session))) {

			if (!switch_channel_ready(switch_core_session_get_channel(session))) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Channel closed (for %s)\n", cli);
				goto cleanup;
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Need to download %s (for %s)\n", name->valuestring, cli);

			if (SWITCH_STATUS_SUCCESS != hv_file_name_to_s3_url(name->valuestring, cli, req.url, sizeof(req.url), settings)) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to create S3 resource URL\n");
				err = 1;
				continue;
			}

			if (SWITCH_STATUS_SUCCESS != hv_http_get_to_disk(&req, voicemail_path)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to GET voicemail to disk (%s) (for %s)\n", voicemail_path, cli);
				hv_http_req_destroy(&req);
				err = 1;
				continue;
			} else {
				if (((req.http_code != 200 && req.http_code != 201)) || req.size == 0) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Did not download voicemail file (to %s) (for %s)\n", voicemail_path, cli);
					hv_http_req_destroy(&req);
					err = 1;
					continue;
				}
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "%s saved to disk (%s) (for %s)\n", name->valuestring, voicemail_path, cli);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Playback %s from disk (for %s)\n", name->valuestring, cli);
		}

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Playback start %s (for %s)\n", name->valuestring, cli);

		{
			switch_ivr_play_file(session, NULL, voicemail_path, &dtmf_args);
		}

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Playback end %s (for %s)\n", name->valuestring, cli);

		if (!switch_channel_ready(channel)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Channel closed (for %s)\n", cli);
			goto cleanup;
		}	

menu_sub:

		sub_menu_loops++;

		if (!switch_channel_ready(channel)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Channel closed (for %s)\n", cli);
			goto cleanup;
		}

		if (dtmf) {
			goto process_playback_dtmf;
		}

		snprintf(prompt, sizeof(prompt), "To listen to next message: ");
		hv_ivr_speak_text(prompt, session, settings, &dtmf_args);
		if (!dtmf) {
			snprintf(prompt, sizeof(prompt), "press 1. ");
			hv_ivr_speak_text(prompt, session, settings, &dtmf_args);
			if (!dtmf) {
				snprintf(prompt, sizeof(prompt), "To delete: ");
				hv_ivr_speak_text(prompt, session, settings, &dtmf_args);
				if (!dtmf) {
					snprintf(prompt, sizeof(prompt), "Press 2.");
					hv_ivr_speak_text(prompt, session, settings, &dtmf_args);
					if (!dtmf) {
						snprintf(prompt, sizeof(prompt), "To hangup: ");
						hv_ivr_speak_text(prompt, session, settings, &dtmf_args);
						if (!dtmf) {
							snprintf(prompt, sizeof(prompt), "Press 3, or just hangup.");
							hv_ivr_speak_text(prompt, session, settings, &dtmf_args);
						}
					}
				}
			}
		}

		if (!switch_channel_ready(channel)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Channel closed (for %s)\n", cli);
			goto cleanup;
		}

		hv_ivr_timeout_set_ms(&timeout, 5000);

		while (!dtmf && switch_channel_ready(channel)) {
			if (channel) {
				if (switch_channel_has_dtmf(channel)) {
					switch_dtmf_t d = { 0 };
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "DTMF: channel has DTMF\n");
					switch_channel_dequeue_dtmf(channel, &d);
					hv_ivr_on_dtmf(session, (void *) &d, SWITCH_INPUT_TYPE_DTMF, dtmf_args.buf, dtmf_args.buflen);
				}
			}
			if (hv_ivr_timeout_expired(&timeout)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Main menu: Timeout (for %s)\n", cli);
				break;
			}
		}

		if (!switch_channel_ready(channel)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Channel closed (for %s)\n", cli);
			goto cleanup;
		}

process_playback_dtmf:

		if (dtmf) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Sub menu: DTMF %c pressed (for %s)\n", dtmf, cli);
			if (dtmf == '1') {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Sub menu: NEXT (for %s)\n", cli);
			} else if (dtmf == '2') {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Sub menu: DELETE (for %s)\n", cli);
				{
					cJSON *prev = vm->prev;
					int first = 0;
					hv_http_req_t r = { 0 };

					if (vm == vms->child) {
						first = 1;
					}

					cJSON_DetachItemViaPointer(vms, vm);

					if (SWITCH_STATUS_SUCCESS != hv_file_name_to_s3_url(name->valuestring, cli, r.url, sizeof(r.url), settings)) {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to create S3 resource URL\n");
						err = 1;
					} else {
						if (SWITCH_STATUS_SUCCESS !=  hv_http_delete(&r)) {
							switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to DELETE voicemail %s from S3 (for %s)\n", name->valuestring, cli);
							err = 1;
						}
						snprintf(prompt, sizeof(prompt), "Deleted.");
						hv_ivr_speak_text(prompt, session, settings, NULL);
					}

					hv_http_req_destroy(&r);

					if (!first) {
						vm = prev;
					} else {
						vm = NULL;
						goto iterate_messages;
					}
				}
			} else if (dtmf == '3') {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Sub menu: HANGUP (for %s)\n", cli);
				goto cleanup;
			} else {
				if (sub_menu_loops < HV_IVR_LOOPS_MAX_SUB_MENU) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Sub menu: BAD ENTRY (for %s)\n", cli);
					snprintf(prompt, sizeof(prompt), "Please press 1, 2, 3, or hangup.");
					hv_ivr_speak_text(prompt, session, settings, NULL);
					goto menu_sub;
				} else {
					snprintf(prompt, sizeof(prompt), "You've reached maximum number of failures, please dial again.");
					hv_ivr_speak_text(prompt, session, settings, NULL);
					goto cleanup;
				}
			}
		} else {

			if (sub_menu_loops < HV_IVR_LOOPS_MAX_SUB_MENU) {
				snprintf(prompt, sizeof(prompt), "Please try again.");
				hv_ivr_speak_text(prompt, session, settings, NULL);
				goto menu_sub;
			} else {
				snprintf(prompt, sizeof(prompt), "You've reached maximum number of failures, please dial again.");
				hv_ivr_speak_text(prompt, session, settings, NULL);
				goto cleanup;
			}
		}

		// We've reached end of menu, honour the user by resetting the counters
		main_menu_loops = 0;
		sub_menu_loops = 0;

		hv_http_req_destroy(&req);
	}

	snprintf(prompt, sizeof(prompt), "End of messages.");
	hv_ivr_speak_text(prompt, session, settings, NULL);
	if (loops < HV_MENU_LOOPS_MAX_N) {
		goto menu_main;
	}

cleanup:

	if (!settings->cache_enabled) {
		cJSON_ArrayForEach(vm, vms_original) {
			cJSON *name = cJSON_GetObjectItemCaseSensitive(vm, HV_JSON_KEY_VOICEMAIL_NAME);
			snprintf(voicemail_path, sizeof(voicemail_path), "%s/%s", settings->file_system_folder_in, name->valuestring);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Removing %s from local storage %s (for %s)\n", name->valuestring, voicemail_path, cli);
			unlink(voicemail_path);
		}
	}

	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Updating JSON state (for %s)\n", cli);

		if (SWITCH_STATUS_SUCCESS != hv_vm_state_upload(cli, vm_state, settings)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to upload JSON state to S3 (for %s)\n", cli);
			goto fail;
		}
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Updated JSON state uploaded (for %s)\n", cli);
	}

end:

	hv_http_req_destroy(&req);
	if (err) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Voicemail retrieval failed (for %s)\n", cli);
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Voicemail retrieval OK (for %s)\n", cli);
	}

	if (vms_original) {
		cJSON_Delete(vms_original);
	}

	return;

fail:
	if (vms_original) {
		cJSON_Delete(vms_original);
	}
	return;
}
