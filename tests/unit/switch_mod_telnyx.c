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

		FST_SESSION_BEGIN(test_switch_caller_extension_insert_applications)
		{
		    switch_caller_extension_t *caller_extension = NULL;

			// Prepare test data
			const char *application_names[] = { "app1", "app2", "app3" };
			const char *application_data[] = { "data1", "data2", "data3" };
			const size_t app_count = sizeof(application_names) / sizeof(application_names[0]);

			// Low priority applications (ensure they remain at the end)
			char *low_priority[] = { "low_priority_app" };
			const size_t low_priority_count = sizeof(low_priority) / sizeof(low_priority[0]);
			switch_caller_application_t *current;
			switch_caller_application_t *low_prio;

		    // Initialize a caller extension
		    caller_extension = switch_core_session_alloc(fst_session, sizeof(switch_caller_extension_t));
		    fst_check(caller_extension != NULL);

			switch_caller_extension_add_application(fst_session, caller_extension, "low_priority_app", "data");
			low_prio = caller_extension->applications;

		    // Insert applications into the caller extension
		    switch_caller_extension_insert_applications(
		        fst_session,
		        caller_extension,
		        application_names,
		        application_data,
		        app_count,
		        low_priority,
		        low_priority_count
		    );

		    // Verify the applications are inserted correctly
		    current = caller_extension->applications;

		    // Check the order of applications
		    for (size_t i = 0; i < app_count; ++i) {
		        fst_check(current != NULL);
		        fst_check(strcmp(current->application_name, application_names[i]) == 0);
		        fst_check(strcmp(current->application_data, application_data[i]) == 0);
		        current = current->next;
		    }

		    // Verify the last_application pointer
		    fst_check(caller_extension->last_application != NULL);
		    fst_check(current == low_prio);
		}
		FST_SESSION_END()

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
