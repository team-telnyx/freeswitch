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
	switch_ivr_record_file(session, &fh, file_path, &args, settings->max_record_len);
}

void hv_retrieval_app_exec(switch_core_session_t *session, const char *data, hv_settings_t *settings)
{
	return;
}
