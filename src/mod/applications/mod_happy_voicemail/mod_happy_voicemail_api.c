#include <mod_happy_voicemail.h>

switch_status_t hv_deposit_api_exec(_In_opt_z_ const char *cmd, _In_opt_ switch_core_session_t *session, _In_ switch_stream_handle_t *stream, hv_settings_t *settings)
{
	(void) cmd;
	(void) session;
	(void) stream;
	return SWITCH_STATUS_SUCCESS;
}

switch_status_t hv_retrieval_api_exec(_In_opt_z_ const char *cmd, _In_opt_ switch_core_session_t *session, _In_ switch_stream_handle_t *stream, hv_settings_t *settings)
{
	(void) cmd;
	(void) session;
	(void) stream;
	return SWITCH_STATUS_SUCCESS;
}
