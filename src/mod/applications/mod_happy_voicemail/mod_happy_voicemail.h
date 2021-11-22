#ifndef MOD_HAPPY_VOICEMAIL_H
#define MOD_HAPPY_VOICEMAIL_H

#include <switch.h>
#include <uuid/uuid.h>
#include <curl/curl.h>


#define HV_CONFIG_FILE_NAME "happy_voicemail.conf"
#define HV_NAME "Happy Voicemail"
#define HV_VARIABLE_PIN_NAME "variable_telnyx_voicemail_user_pin"
#define HV_VARIABLE_DIALED_EXTENSION "variable_telnyx_dialed_extension"

#define HV_DEFAULT_BEEP "%(1300, 0, 640)"
#define HV_DEFAULT_RECORD_MAX_LEN_S 600
#define HV_DEFAULT_FILE_SYSTEM_FOLDER_IN	"/tmp/in/voicemails"
#define HV_DEFAULT_FILE_SYSTEM_FOLDER_OUT	"/tmp/out/voicemails"
#define HV_DEFAULT_RECORD_FILE_EXT "wav"
#define HV_DEFAULT_RECORD_CHECK_SILENCE "false"
#define HV_DEFAULT_CACHE_ENABLED "false"
#define HV_DEFAULT_VOICE "slt"
#define HV_DEFAULT_PIN_CHECK "yes"
#define HV_DEFAULT_VM_STATE_CREATE "yes"

#define HV_DEFAULT_USE_S3_AUTH "no"
#define HV_S3_DEFAULT_BASE_DOMAIN "s3.%s.amazonaws.com"
#define HV_S3_DEFAULT_EXPIRATION_TIME 604800

#define HV_BUFLEN 1000

#define HV_JSON_NEW_VOICEMAILS_ARRAY_NAME "new_voicemails"
#define HV_JSON_S3_NAME "vms.json"
#define HV_JSON_KEY_VOICEMAIL_NAME "name"
#define HV_JSON_KEY_VOICEMAIL_TIMESTAMP "timestamp"

#define HV_MENU_LOOPS_MAX_N 5
#define HV_IVR_PIN_TERMINATOR_CHAR '#'
#define HV_IVR_PIN_TERMINATOR_CHAR_NAME "hash"
#define HV_IVR_TIMEOUT_SINGLE_DIGIT_MS 30000
#define HV_IVR_TIMEOUT_TOTAL_MS 90000
#define HV_IVR_LOOPS_MAX_PIN_CHECK 3
#define HV_IVR_LOOPS_MAX_MAIN_MENU 3
#define HV_IVR_LOOPS_MAX_SUB_MENU 3


struct hv_settings_s {
	char tone_spec[HV_BUFLEN];
	uint32_t record_silence_threshold;
	uint32_t record_silence_hits;
	uint32_t record_sample_rate;
	uint8_t record_check_silence;
	uint32_t record_max_len;
	char record_file_ext[HV_BUFLEN];
	uint8_t use_s3_auth;
	char s3_id[HV_BUFLEN];
	char s3_key[HV_BUFLEN];
	char s3_region[HV_BUFLEN];
	char s3_bucket[HV_BUFLEN];
	char s3_base_domain[HV_BUFLEN + 100];
	char file_system_folder_in[HV_BUFLEN];
	char file_system_folder_out[HV_BUFLEN];
	uint8_t dump_events;
	uint8_t cache_enabled;
	char voice[HV_BUFLEN];
	uint8_t pin_check;
	uint8_t vm_state_create;

	struct configured {
		uint8_t tone_spec;
		uint8_t record_silence_threshold;
		uint8_t record_silence_hits;
		uint8_t record_sample_rate;
		uint8_t record_check_silence;
		uint8_t record_max_len;
		uint8_t record_file_ext;
		uint8_t use_s3_auth;
		uint8_t s3_id;
		uint8_t s3_key;
		uint8_t s3_region;
		uint8_t s3_bucket;
		uint8_t file_system_folder_in;
		uint8_t file_system_folder_out;
		uint8_t dump_events;
		uint8_t cache_enabled;
		uint8_t voice;
		uint8_t pin_check;
		uint8_t vm_state_create;
	} configured;
};
typedef struct hv_settings_s hv_settings_t;

// Dialplan interface
void hv_deposit_app_exec(switch_core_session_t *session, const char *data, hv_settings_t *settings);
void hv_retrieval_app_exec(switch_core_session_t *session, const char *data, hv_settings_t *settings);
void hv_s3_test_app_exec(switch_core_session_t *session, const char *data, hv_settings_t *settings);

// API interface
switch_status_t hv_deposit_api_exec(_In_opt_z_ const char *cmd, _In_opt_ switch_core_session_t *session, _In_ switch_stream_handle_t *stream, hv_settings_t *settings);
switch_status_t hv_retrieval_api_exec(_In_opt_z_ const char *cmd, _In_opt_ switch_core_session_t *session, _In_ switch_stream_handle_t *stream, hv_settings_t *settings);
switch_status_t hv_http_upload_api_exec(_In_opt_z_ const char *cmd, _In_opt_ switch_core_session_t *session, _In_ switch_stream_handle_t *stream, hv_settings_t *settings);
switch_status_t hv_s3_test_api_exec(_In_opt_z_ const char *cmd, _In_opt_ switch_core_session_t *session, _In_ switch_stream_handle_t *stream, hv_settings_t *settings);

struct hv_s3_req_s {
	char bucket[HV_BUFLEN];
	char object[HV_BUFLEN];
	char time_stamp[HV_BUFLEN];
	char date_stamp[HV_BUFLEN];
	char id[HV_BUFLEN];
	char key[HV_BUFLEN];
	char region[HV_BUFLEN];
	char base_domain[HV_BUFLEN + 100];
};
typedef struct hv_s3_req_s hv_s3_req_t;

struct hv_http_req_s {
	char url[HV_BUFLEN];
	char *memory;
	size_t size;
	size_t sizeleft;
	long http_code;
	uint8_t use_s3_auth;
	hv_s3_req_t s3;
};
typedef struct hv_http_req_s hv_http_req_t;

SWITCH_DECLARE(switch_status_t) hv_http_req_prepare(const char *name, const char *ext, char *url, uint32_t len, hv_settings_t *settings, hv_http_req_t *req);
SWITCH_DECLARE(switch_status_t) hv_file_name_to_s3_url(const char *name, const char *ext, char *url, uint32_t len, hv_settings_t *settings);
SWITCH_DECLARE(switch_status_t) hv_ext_to_s3_vm_state_url(const char *ext, char *url, uint32_t len, hv_settings_t *settings, hv_http_req_t *req);
SWITCH_DECLARE(void) hv_get_uuid(char *buf, uint32_t len);
SWITCH_DECLARE(switch_status_t) hv_http_upload_from_disk(const char *file_name, hv_http_req_t *req);
SWITCH_DECLARE(switch_status_t) hv_http_upload_from_mem(hv_http_req_t *upload);
SWITCH_DECLARE(void) hv_http_req_destroy(hv_http_req_t *req);
SWITCH_DECLARE(switch_status_t) hv_http_get_to_mem(hv_http_req_t *req);
SWITCH_DECLARE(switch_status_t) hv_http_get_to_disk(hv_http_req_t *req, const char *file_name);
SWITCH_DECLARE(switch_status_t) hv_http_delete(hv_http_req_t *req);
SWITCH_DECLARE(switch_status_t) hv_http_add_s3_authentication(CURL *h, hv_http_req_t *req, const char *method);

SWITCH_DECLARE(cJSON*) hv_json_vm_state_create(void);
SWITCH_DECLARE(switch_status_t) hv_json_vm_state_add_new_voicemail(cJSON *v, const char *name);

SWITCH_DECLARE(switch_status_t) hv_vm_state_get_from_s3_to_mem(hv_http_req_t *req, const char *cld, hv_settings_t *settings);
SWITCH_DECLARE(switch_status_t) hv_vm_state_upload(const char *ext, cJSON *vms, hv_settings_t *settings);
SWITCH_DECLARE(switch_status_t) hv_vm_state_update(const char *cld, const char *voicemail_name, hv_settings_t *settings);

typedef struct hv_ivr_timeout_s {
	switch_time_t start_ms;
	switch_time_t timeout_ms;
} hv_ivr_timeout_t;

SWITCH_DECLARE(void) hv_ivr_timeout_set_ms(hv_ivr_timeout_t *t, switch_time_t ms);
SWITCH_DECLARE(uint8_t) hv_ivr_timeout_expired(hv_ivr_timeout_t *t);

SWITCH_DECLARE(void) hv_ivr_speak_text(const char *text, switch_core_session_t *session, hv_settings_t *settings, switch_input_args_t *args);
SWITCH_DECLARE(switch_status_t) hv_ivr_pin_check(switch_core_session_t *session, uint64_t pin, hv_settings_t *settings);
SWITCH_DECLARE(void) hv_ivr_run(switch_core_session_t *session, cJSON *vm_state, char *cli, uint64_t pin, hv_settings_t *settings);

SWITCH_DECLARE(char *) hv_s3_authentication_create(hv_http_req_t *req, const char *method);

#endif // MOD_HAPPY_VOICEMAIL_H
