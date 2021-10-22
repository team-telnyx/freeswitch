#ifndef MOD_VM2
#define MOD_VM2

#include <switch.h>

#define VM_PROFILE_CONFIGITEM_COUNT 100

#ifdef _MSC_VER
#define TRY_CODE(code) for(;;) {status = code; if (status != SWITCH_STATUS_SUCCESS && status != SWITCH_STATUS_BREAK) { goto end; } break;}
#else
#define TRY_CODE(code) do { status = code; if (status != SWITCH_STATUS_SUCCESS && status != SWITCH_STATUS_BREAK) { goto end; } break;} while(status)
#endif

typedef enum {
	VM_DATE_FIRST,
	VM_DATE_LAST,
	VM_DATE_NEVER
} date_location_t;

struct vm_profile {
	char *name;
	char *dbname;
	char *odbc_dsn;
	char *play_new_messages_lifo;
	char *play_saved_messages_lifo;
	char terminator_key[2];
	char play_new_messages_key[2];
	char play_saved_messages_key[2];

	char login_keys[16];
	char main_menu_key[2];
	char skip_greet_key[2];
	char skip_info_key[2];
	char config_menu_key[2];
	char record_greeting_key[2];
	char choose_greeting_key[2];
	char record_name_key[2];
	char change_pass_key[2];

	char record_file_key[2];
	char listen_file_key[2];
	char save_file_key[2];
	char delete_file_key[2];
	char undelete_file_key[2];
	char email_key[2];
	char callback_key[2];
	char pause_key[2];
	char restart_key[2];
	char ff_key[2];
	char rew_key[2];
	char prev_msg_key[2];
	char next_msg_key[2];
	char repeat_msg_key[2];
	char urgent_key[2];
	char operator_key[2];
	char vmain_key[2];
	char forward_key[2];
	char prepend_key[2];
	char file_ext[10];
	char *record_title;
	char *record_comment;
	char *record_copyright;
	char *operator_ext;
	char *vmain_ext;
	char *tone_spec;
	char *storage_dir;
	switch_bool_t storage_dir_shared;
	char *callback_dialplan;
	char *callback_context;
	char *email_body;
	char *email_headers;
	char *notify_email_body;
	char *notify_email_headers;
	char *web_head;
	char *web_tail;
	char *email_from;
	char *date_fmt;
	char *convert_cmd;
	char *convert_ext;
	date_location_t play_date_announcement;
	uint32_t digit_timeout;
	uint32_t max_login_attempts;
	uint32_t min_record_len;
	uint32_t max_record_len;
	uint32_t max_retries;
	switch_mutex_t *mutex;
	uint32_t record_threshold;
	uint32_t record_silence_hits;
	uint32_t record_sample_rate;
	switch_bool_t auto_playback_recordings;
	switch_bool_t db_password_override;
	switch_bool_t allow_empty_password_auth;
	switch_bool_t send_full_vm_header;
	switch_thread_rwlock_t *rwlock;
	switch_memory_pool_t *pool;
	uint32_t flags;

	switch_xml_config_item_t config[VM_PROFILE_CONFIGITEM_COUNT];
	switch_xml_config_string_options_t config_str_pool;
};
typedef struct vm_profile vm_profile_t;

typedef enum {
	PFLAG_DESTROY = 1 << 0
} vm_flags_t;

typedef enum {
	VM_MOVE_NEXT,
	VM_MOVE_PREV,
	VM_MOVE_SAME
} msg_move_t;

typedef enum {
	MWI_REASON_UNKNOWN = 0,
	MWI_REASON_NEW = 1,
	MWI_REASON_DELETE = 2,
	MWI_REASON_SAVE = 3,
	MWI_REASON_PURGE = 4,
	MWI_REASON_READ = 5
} mwi_reason_t;

struct mwi_reason_table {
	const char *name;
	int state;
};

switch_status_t vm2_create_file(switch_core_session_t *session, vm_profile_t *profile, char *file_path);

#endif // MOD_VM2
