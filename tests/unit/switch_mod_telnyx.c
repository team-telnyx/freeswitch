#include <switch.h>
#include <stdlib.h>

#include <test/switch_test.h>

int timeout_sec = 10;

FST_CORE_BEGIN("./conf_telnyx")
{
	FST_SUITE_BEGIN(switch_mod_telnyx)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_telnyx");
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		FST_TEST_BEGIN(post_dialplan)
		{
			switch_core_session_t *session = NULL;
			switch_call_cause_t cause;
			char *originate_str = switch_mprintf("sofia/gateway/test_gateway/+15553332901");

			switch_ivr_originate(NULL, &session, &cause, originate_str, timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
			switch_safe_free(originate_str);
			fst_requires(session);

			if (session) {
				switch_channel_t *channel = switch_core_session_get_channel(session);

				fst_requires(channel);

				switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
				switch_core_session_rwunlock(session);
			}
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
