#ifndef MOD_HAPPY_VOICEMAIL_H
#define MOD_HAPPY_VOICEMAIL_H

#include <switch.h>
#include <uuid/uuid.h>


#define HV_CONFIG_FILE_NAME "happy_voicemail.conf"
#define HV_NAME "Happy Voicemail"

#define HV_DEFAULT_BEEP "%(1300, 0, 640)"
#define HV_DEFAULT_MAX_RECORD_LEN_S 600
#define HV_DEFAULT_FILE_SYSTEM_FOLDER "/tmp/voicemails"
#define HV_DEFAULT_RECORDING_FILE_EXT "wav"
#define HV_DEFAULT_RECORD_CHECK_SILENCE "false"

#define HV_BUFLEN 1000

#define HV_JSON_NEW_VOICEMAILS_ARRAY_NAME "new_voicemails"
#define HV_JSON_S3_NAME "vms.json"

struct hv_settings_s {
	char tone_spec[HV_BUFLEN];
	uint32_t record_silence_threshold;
	uint32_t record_silence_hits;
	uint32_t record_sample_rate;
	uint8_t record_check_silence;
	uint32_t max_record_len;
	char s3_url[HV_BUFLEN];
	char file_system_folder[HV_BUFLEN];
	uint8_t dump_events;
	char recording_file_ext[HV_BUFLEN];
};
typedef struct hv_settings_s hv_settings_t;

// Dialplan interface
void hv_deposit_app_exec(switch_core_session_t *session, const char *data, hv_settings_t *settings);
void hv_retrieval_app_exec(switch_core_session_t *session, const char *data, hv_settings_t *settings);

// API interface
switch_status_t hv_deposit_api_exec(_In_opt_z_ const char *cmd, _In_opt_ switch_core_session_t *session, _In_ switch_stream_handle_t *stream, hv_settings_t *settings);
switch_status_t hv_retrieval_api_exec(_In_opt_z_ const char *cmd, _In_opt_ switch_core_session_t *session, _In_ switch_stream_handle_t *stream, hv_settings_t *settings);
switch_status_t hv_http_upload_api_exec(_In_opt_z_ const char *cmd, _In_opt_ switch_core_session_t *session, _In_ switch_stream_handle_t *stream, hv_settings_t *settings);

struct hv_http_req_s {
	char url[HV_BUFLEN];
	char *memory;
	size_t size;
};
typedef struct hv_http_req_s hv_http_req_t;

SWITCH_DECLARE(switch_status_t) hv_file_name_to_s3_url(const char *name, const char *ext, char *url, uint32_t len, hv_settings_t *settings);
SWITCH_DECLARE(switch_status_t) hv_ext_to_s3_vm_state_url(const char *ext, char *url, uint32_t len, hv_settings_t *settings);
SWITCH_DECLARE(void) hv_get_uuid(char *buf, uint32_t len);
SWITCH_DECLARE(switch_status_t) hv_http_upload_from_disk(const char *file_name, const char *url);
SWITCH_DECLARE(void) hv_http_req_destroy(hv_http_req_t *req);
SWITCH_DECLARE(switch_status_t) hv_http_get_to_mem(hv_http_req_t *req);

SWITCH_DECLARE(cJSON*) hv_json_vm_state_create(void);
SWITCH_DECLARE(switch_status_t) hv_json_vm_state_add_new_voicemail(cJSON *v, const char *name);

#endif // MOD_HAPPY_VOICEMAIL_H
