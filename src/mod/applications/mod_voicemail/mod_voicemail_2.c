#include <mod_voicemail.h>

switch_status_t vm2_create_file(switch_core_session_t *session, vm_profile_t *profile,
								   char *file_path)
{
	switch_file_handle_t fh = { 0 };
	switch_input_args_t args = { 0 };
	char input[10] = "";
	switch_codec_implementation_t read_impl = { 0 };

	switch_core_session_get_read_impl(session, &read_impl);


		switch_ivr_gentones(session, profile->tone_spec, 0, NULL);

		memset(&fh, 0, sizeof(fh));
		fh.thresh = profile->record_threshold;
		fh.silence_hits = profile->record_silence_hits;
		fh.samplerate = profile->record_sample_rate;

		memset(input, 0, sizeof(input));
		//args.input_callback = cancel_on_dtmf;
		args.buf = input;
		args.buflen = sizeof(input);

		unlink(file_path);

		switch_ivr_record_file(session, &fh, file_path, &args, profile->max_record_len);

		return SWITCH_STATUS_SUCCESS;
}
