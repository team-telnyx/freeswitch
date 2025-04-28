#include <switch.h>
#include <stdlib.h>

#include <test/switch_test.h>

int timeout_sec = 10;

static void show_event(switch_event_t *event) {
	char *str;
	/*print the event*/
	switch_event_serialize_json(event, &str);
	if (str) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s\n", str);
		switch_safe_free(str);
	}
}

static void on_tts_event(switch_event_t *event)
{
	const char *new_ev = switch_event_get_header(event, "Event-Subclass");

	if (new_ev)
		assert(new_ev == strstr(new_ev, "polly::"));

	show_event(event);
}

FST_CORE_BEGIN("./conf_gstt")
{
	FST_SUITE_BEGIN(switch_core_asr)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_gstt");
			fst_requires_module("mod_sofia");
			fst_requires_module("mod_tone_stream");
			fst_requires_module("mod_polly");
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		FST_TEST_BEGIN(gstt)
		{
			switch_core_session_t *session = NULL;
			switch_status_t status;
			switch_call_cause_t cause;

			status = switch_ivr_originate(NULL, &session, &cause, "{ignore_early_media=true}sofia/gateway/test_gateway/+15553332900", timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
			fst_requires(session);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			if (session) {
				switch_stream_handle_t stream = { 0 };
				const char *uuid = switch_core_session_get_uuid(session);
				switch_channel_t *channel = NULL;

				channel = switch_core_session_get_channel(session);
				fst_requires(channel);

				if (uuid) {
					char *off_uuid = switch_mprintf("%s start en", uuid);

					SWITCH_STANDARD_STREAM(stream);
					switch_api_execute("uuid_gstt_transcribe", off_uuid, NULL, &stream);
					switch_safe_free(stream.data);
					switch_ivr_play_file(session, NULL, "silence_stream://100,0", NULL);
					switch_sleep(20);
				}
				switch_core_session_rwunlock(session);
			}
		}
		FST_TEST_END()

		FST_TEST_BEGIN(polly)
		{
			switch_stream_handle_t stream = { 0 };
			switch_core_session_t *session = NULL;
			switch_status_t status;
			switch_call_cause_t cause;

			switch_event_bind("test", SWITCH_EVENT_CUSTOM, SWITCH_EVENT_SUBCLASS_ANY, on_tts_event, NULL);
			status = switch_ivr_originate(NULL, &session, &cause, "{ignore_early_media=true}sofia/gateway/test_gateway/+15553332900", timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
			fst_requires(session);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			switch_safe_free(stream.data);
			switch_yield(2000 * 1000);
			switch_event_unbind_callback(on_tts_event);
			switch_core_session_rwunlock(session);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
