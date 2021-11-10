#ifndef MOD_HAPPY_VOICEMAIL_H
#define MOD_HAPPY_VOICEMAIL_H

#include <switch.h>
#include <uuid/uuid.h>


#define HV_CONFIG_FILE_NAME "happy_voicemail.conf"
#define HV_NAME "Happy Voicemail"

#define HV_DEFAULT_BEEP "%(1300, 0, 640)"
#define HV_DEFAULT_RECORD_MAX_LEN_S 600
#define HV_DEFAULT_FILE_SYSTEM_FOLDER_IN	"/tmp/in/voicemails"
#define HV_DEFAULT_FILE_SYSTEM_FOLDER_OUT	"/tmp/out/voicemails"
#define HV_DEFAULT_RECORD_FILE_EXT "wav"
#define HV_DEFAULT_RECORD_CHECK_SILENCE "false"
#define HV_DEFAULT_CACHE_ENABLED "false"
#define HV_DEFAULT_VOICE "slt"

#define HV_BUFLEN 1000

#define HV_JSON_NEW_VOICEMAILS_ARRAY_NAME "new_voicemails"
#define HV_JSON_S3_NAME "vms.json"
#define HV_JSON_KEY_VOICEMAIL_NAME "name"
#define HV_JSON_KEY_VOICEMAIL_TIMESTAMP "timestamp"

#define HV_MENU_LOOPS_MAX_N 5

struct hv_settings_s {
	char tone_spec[HV_BUFLEN];
	uint32_t record_silence_threshold;
	uint32_t record_silence_hits;
	uint32_t record_sample_rate;
	uint8_t record_check_silence;
	uint32_t record_max_len;
	char record_file_ext[HV_BUFLEN];
	char s3_url[HV_BUFLEN];
	char file_system_folder_in[HV_BUFLEN];
	char file_system_folder_out[HV_BUFLEN];
	uint8_t dump_events;
	uint8_t cache_enabled;
	char voice[HV_BUFLEN];

	struct configured {
		uint8_t tone_spec;
		uint8_t record_silence_threshold;
		uint8_t record_silence_hits;
		uint8_t record_sample_rate;
		uint8_t record_check_silence;
		uint8_t record_max_len;
		uint8_t record_file_ext;
		uint8_t s3_url;
		uint8_t file_system_folder_in;
		uint8_t file_system_folder_out;
		uint8_t dump_events;
		uint8_t cache_enabled;
		uint8_t voice;
	} configured;
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
	size_t sizeleft;
	long http_code;
};
typedef struct hv_http_req_s hv_http_req_t;

SWITCH_DECLARE(switch_status_t) hv_file_name_to_s3_url(const char *name, const char *ext, char *url, uint32_t len, hv_settings_t *settings);
SWITCH_DECLARE(switch_status_t) hv_ext_to_s3_vm_state_url(const char *ext, char *url, uint32_t len, hv_settings_t *settings);
SWITCH_DECLARE(void) hv_get_uuid(char *buf, uint32_t len);
SWITCH_DECLARE(switch_status_t) hv_http_upload_from_disk(const char *file_name, const char *url);
SWITCH_DECLARE(switch_status_t) hv_http_upload_from_mem(hv_http_req_t *upload);
SWITCH_DECLARE(void) hv_http_req_destroy(hv_http_req_t *req);
SWITCH_DECLARE(switch_status_t) hv_http_get_to_mem(hv_http_req_t *req);
SWITCH_DECLARE(switch_status_t) hv_http_get_to_disk(hv_http_req_t *req, const char *file_name);

SWITCH_DECLARE(cJSON*) hv_json_vm_state_create(void);
SWITCH_DECLARE(switch_status_t) hv_json_vm_state_add_new_voicemail(cJSON *v, const char *name);

SWITCH_DECLARE(switch_status_t) hv_vm_state_get_from_s3_to_mem(hv_http_req_t *req, const char *cld, hv_settings_t *settings);
SWITCH_DECLARE(switch_status_t) hv_vm_state_update(const char *cld, const char *voicemail_name, hv_settings_t *settings);

struct call_control_s {
	switch_file_handle_t *fh;
	char buf[4];
	int noexit;
};
typedef struct call_control_s switch_cc_t;

typedef struct hv_ivr_timeout_s {
	switch_time_t start_ms;
	switch_time_t timeout_ms;
} hv_ivr_timeout_t;

SWITCH_DECLARE(void) hv_ivr_timeout_set_ms(hv_ivr_timeout_t *t, switch_time_t ms);
SWITCH_DECLARE(uint8_t) hv_ivr_timeout_expired(hv_ivr_timeout_t *t);

SWITCH_DECLARE(void) hv_ivr_speak_text(const char *text, switch_core_session_t *session, hv_settings_t *settings, switch_input_args_t *args);
SWITCH_DECLARE(void) hv_ivr_run(switch_core_session_t *session, cJSON *vm_state, char *cli, hv_settings_t *settings);

#endif // MOD_HAPPY_VOICEMAIL_H
