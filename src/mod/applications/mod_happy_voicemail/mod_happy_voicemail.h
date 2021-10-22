#ifndef MOD_HAPPY_VOICEMAIL_H
#define MOD_HAPPY_VOICEMAIL_H

#include <switch.h>


#define HAPPY_VOICEMAIL_CONFIG_FILE_PATH "happy_voicemail.conf"
#define HAPPY_VOICEMAIL_NAME "Happy Voicemail"

#define HV_BUFLEN 200

struct hv_settings_s {
	char tone_spec[HV_BUFLEN];
	uint32_t record_silence_threshold;
	uint32_t record_silence_hits;
	uint32_t record_sample_rate;
	uint32_t max_record_len;
};
typedef struct hv_settings_s hv_settings_t;

// Dialplan interface
void hv_deposit_app_exec(switch_core_session_t *session, const char *data, hv_settings_t *settings);
void hv_retrieval_app_exec(switch_core_session_t *session, const char *data, hv_settings_t *settings);

// API interface
switch_status_t hv_deposit_api_exec(_In_opt_z_ const char *cmd, _In_opt_ switch_core_session_t *session, _In_ switch_stream_handle_t *stream, hv_settings_t *settings);
switch_status_t hv_retrieval_api_exec(_In_opt_z_ const char *cmd, _In_opt_ switch_core_session_t *session, _In_ switch_stream_handle_t *stream, hv_settings_t *settings);

#endif // MOD_HAPPY_VOICEMAIL_H
