
/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2021, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * Dragos Oancea <dragos@signalwire.com>
 *
 *
 * sipp-based-tests.c - Test FreeSwitch using sipp (https://github.com/SIPp/sipp)
 *
 */

#include <switch.h>
#include <test/switch_test.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <pthread.h>

extern char **environ;

int test_success = 0;
int test_sofia_debug = 1;
static int timeout_sec = 10;

static void test_wait_for_uuid(char *uuid)
{
	switch_stream_handle_t stream = { 0 };
	int loop_count = 50;
	char *channel_data=NULL;

	do {
		SWITCH_STANDARD_STREAM(stream);
		switch_api_execute("show", "channels", NULL, &stream);

		if (stream.data && !strncmp((char *)stream.data, "uuid,", 5)) {
			channel_data = switch_mprintf("%s", (char *)stream.data);
			switch_safe_free(stream.data);
			break;
		}
		switch_safe_free(stream.data);
		switch_sleep(100 * 1000);
	} while (loop_count--);

	if (channel_data) {
		char *temp = NULL;
		int i;

		if ((temp = strchr(channel_data, '\n'))) {
			temp++;
			for (i = 0; temp[i] != ',' && i < 99; i++) {
				uuid[i] = temp[i];
			}
		}
		free(channel_data);
	}
}

static const char *test_wait_for_chan_var(switch_channel_t *channel, const char *seq) 
{
	int loop_count = 50;
	const char *var=NULL;
	do {
		const char *sip_cseq = switch_channel_get_variable(channel, "sip_cseq");

		if (sip_cseq && seq && !strcmp(sip_cseq, seq)){
			switch_sleep(100 * 1000);
			var = switch_channel_get_variable(channel, "rtp_local_sdp_str");
			break;
		}

		switch_sleep(100 * 1000);
	} while(loop_count--);

	return var;
}

static switch_bool_t has_ipv6(void)
{
	switch_stream_handle_t stream = { 0 };
	SWITCH_STANDARD_STREAM(stream);
	switch_api_execute("sofia", "status profile external-ipv6", NULL, &stream);

	if (strstr((char *)stream.data, "Invalid Profile")) {

		switch_safe_free(stream.data);

		return SWITCH_FALSE;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "STATUS PROFILE: %s\n", (char *) stream.data);
	
	switch_safe_free(stream.data);

	return SWITCH_TRUE;
}

static void register_gw(void)
{
	switch_stream_handle_t stream = { 0 };
	SWITCH_STANDARD_STREAM(stream);
	switch_api_execute("sofia", "profile external register testgw", NULL, &stream);
	switch_safe_free(stream.data);
}

static void unregister_gw(void)
{
	switch_stream_handle_t stream = { 0 };
	SWITCH_STANDARD_STREAM(stream);
	switch_api_execute("sofia", "profile external unregister testgw", NULL, &stream);
	switch_safe_free(stream.data);
}

static int start_sipp_uac(const char *ip, int remote_port, const char *dialed_number, const char *scenario_uac, const char *extra)
{
	char *cmd = switch_mprintf("sipp %s:%d -nr -p 5062 -m 1 -s %s -recv_timeout 10000 -timeout 10s -sf %s -bg %s", ip, remote_port, dialed_number, scenario_uac, extra);
	int sys_ret = switch_system(cmd, SWITCH_TRUE);

	printf("%s\n", cmd);
	switch_safe_free(cmd);
	switch_sleep(1000 * 1000);

	return sys_ret;
} 

static int start_sipp_uas(const char *ip, int listen_port, const char *scenario_uas, const char *extra)
{
	char *cmd = switch_mprintf("sipp %s -p %d -nr -m 1 -s 1001 -recv_timeout 10000 -timeout 10s -sf %s -bg %s", ip, listen_port, scenario_uas, extra);
	int sys_ret = switch_system(cmd, SWITCH_TRUE);

	printf("%s\n", cmd);
	switch_safe_free(cmd);
	switch_sleep(1000 * 1000);

	return sys_ret;
}
static int run_sipp(const char *ip, int remote_port, int listen_port, const char *dialed_number, const char *scenario_uac, const char *auth_password, const char *extra)
{
	char *cmd = switch_mprintf("sipp %s:%d -nr -p %d -m 1 -s %s -recv_timeout 10000 -timeout 10s -sf %s -au %s -ap %s -bg %s", ip, remote_port, listen_port, dialed_number, scenario_uac, dialed_number, auth_password, extra);
	int sys_ret = switch_system(cmd, SWITCH_TRUE);

	printf("%s\n", cmd);
	switch_safe_free(cmd);
	switch_sleep(1000 * 1000);

	return sys_ret;
}

static void kill_sipp(void)
{
	switch_system("pkill -x sipp", SWITCH_TRUE);
	switch_sleep(1000 * 1000);
}

/* Keep this peer in the foreground so its exit status belongs to this test. */
static int start_t38_sipp(const char *ip, pid_t *pid)
{
	char *target = switch_mprintf("%s:5080", ip);
	char *argv[] = {
		"sipp", target, "-nr", "-p", "5062", "-m", "1", "-s", "1212121212",
		"-recv_timeout", "10000", "-timeout", "15s", "-timeout_error", "-nostdin",
		"-sf", "sipp-scenarios/uac_t38_identical_refresh.xml", "-trace_err", NULL
	};
	int result = posix_spawnp(pid, "sipp", NULL, NULL, argv, environ);

	switch_safe_free(target);
	return result;
}

/* Return 1 only after reaping the owned child; 0 is timeout, -1 is wait failure. */
static int wait_t38_sipp(pid_t *pid, int *status, int timeout_ms)
{
	int remaining = timeout_ms / 100;
	pid_t result;

	do {
		result = waitpid(*pid, status, WNOHANG);
		if (result == *pid) {
			*pid = -1;
			return 1;
		}
		if (result < 0 && errno != EINTR) {
			if (errno == ECHILD) {
				*pid = -1;
			}
			return -1;
		}
		if (remaining) {
			switch_sleep(100 * 1000);
		}
	} while (remaining--);

	return 0;
}

static int stop_t38_sipp(pid_t *pid)
{
	int status;

	if (*pid <= 0) {
		return 1;
	}
	/* Never signal a process by name: other tests may have their own SIPp. */
	if (wait_t38_sipp(pid, &status, 0) == 1) {
		return 1;
	}
	if (*pid <= 0) {
		return 0;
	}
	kill(*pid, SIGTERM);
	if (wait_t38_sipp(pid, &status, 2000) == 1) {
		return 1;
	}
	if (*pid <= 0) {
		return 0;
	}
	kill(*pid, SIGKILL);
	return wait_t38_sipp(pid, &status, 2000) == 1;
}

static int start_delayed_offer_sipp(const char *ip, const char *scenario, pid_t *pid)
{
	char *target = switch_mprintf("%s:5090", ip);
	char *argv[] = {
		"sipp", target, "-nr", "-p", "5062", "-m", "1", "-s", "3434343434",
		"-recv_timeout", "15000", "-timeout", "20s", "-timeout_error", "-nostdin",
		"-sf", (char *)scenario, "-trace_err", NULL
	};
	int result = posix_spawnp(pid, "sipp", NULL, NULL, argv, environ);

	switch_safe_free(target);
	return result;
}

typedef struct {
	const char *cmd;
	char *arg;
	char *reply;
	switch_time_t started_at;
	switch_time_t elapsed_ms;
	pthread_t thread;
	int started;
} api_call_t;

static void *api_call_run(void *obj)
{
	api_call_t *call = (api_call_t *)obj;
	switch_stream_handle_t stream = { 0 };
	switch_time_t start = switch_time_now();

	call->started_at = start;
	SWITCH_STANDARD_STREAM(stream);
	switch_api_execute(call->cmd, call->arg, NULL, &stream);
	call->elapsed_ms = (switch_time_now() - start) / 1000;
	call->reply = stream.data ? strdup((char *)stream.data) : strdup("");
	switch_safe_free(stream.data);
	return NULL;
}

static void api_call_start(api_call_t *call, const char *cmd, const char *fmt, const char *uuid)
{
	call->cmd = cmd;
	call->arg = switch_mprintf(fmt, uuid);
	call->started = !pthread_create(&call->thread, NULL, api_call_run, call);
}

static void api_call_join(api_call_t *call)
{
	if (call->started) {
		pthread_join(call->thread, NULL);
		call->started = 0;
	}
}

static void api_call_finish(api_call_t *call)
{
	api_call_join(call);
	switch_safe_free(call->arg);
	switch_safe_free(call->reply);
}

static switch_bool_t api_call_ok(api_call_t *call)
{
	return (call->reply && !strncmp(call->reply, "+OK", 3)) ? SWITCH_TRUE : SWITCH_FALSE;
}

/* Inbound no-SDP call on the delayed-offer profile, parked after ring_ready. */
static switch_core_session_t *delayed_offer_wait_parked(char *uuid)
{
	switch_core_session_t *session = NULL;
	int loop_count = 100;

	test_wait_for_uuid(uuid);
	if (zstr(uuid) || !(session = switch_core_session_locate(uuid))) {
		return NULL;
	}
	while (loop_count--) {
		const char *app = switch_channel_get_variable(switch_core_session_get_channel(session), "current_application");
		if (app && !strcmp(app, "park")) {
			break;
		}
		switch_sleep(20 * 1000);
	}
	return session;
}

/* Joins of the pending answer logged by the endpoint for one call: proof that a request reached the
   endpoint while the answer was still waiting for its ACK. */
static struct {
	pthread_mutex_t mutex;
	char uuid[100];
	int joins;
	switch_time_t last_join;
} join_watch = { PTHREAD_MUTEX_INITIALIZER, "", 0, 0 };

static switch_status_t join_watch_logger(const switch_log_node_t *node, switch_log_level_t level)
{
	if (node->data && node->userdata && strstr(node->data, "waiting for the pending answer")) {
		pthread_mutex_lock(&join_watch.mutex);
		if (!zstr(join_watch.uuid) && !strcmp(node->userdata, join_watch.uuid)) {
			join_watch.joins++;
			join_watch.last_join = node->timestamp;
		}
		pthread_mutex_unlock(&join_watch.mutex);
	}
	return SWITCH_STATUS_SUCCESS;
}

typedef enum {
	ACK_RACE_RECORD_ONLY,
	ACK_RACE_DUPLICATE_ANSWER
} ack_race_extra_t;

typedef struct {
	int sipp_started;
	int parked;
	int sipp_ok;
	int up;
	int media;
	switch_time_t answered_at;
	int joins;
	switch_time_t last_join;
	api_call_t answer;
	api_call_t record;
	api_call_t extra;
} ack_race_t;

/* Answer an offerless call (offer in our 200 OK) while the peer holds its ACK for 600 ms, and send media
   requests from other threads before that ACK arrives. */
static void delayed_offer_ack_race(const char *ip, ack_race_extra_t extra, ack_race_t *r)
{
	switch_core_session_t *session = NULL;
	switch_channel_t *channel = NULL;
	char uuid[100] = "";
	pid_t sipp_pid = -1;
	int sipp_status = 0;

	if (start_delayed_offer_sipp(ip, "sipp-scenarios/uac_delayed_offer_slow_ack.xml", &sipp_pid) != 0) {
		return;
	}
	r->sipp_started = 1;

	if (!(session = delayed_offer_wait_parked(uuid))) {
		goto end;
	}
	r->parked = 1;
	channel = switch_core_session_get_channel(session);
	pthread_mutex_lock(&join_watch.mutex);
	switch_set_string(join_watch.uuid, uuid);
	join_watch.joins = 0;
	join_watch.last_join = 0;
	pthread_mutex_unlock(&join_watch.mutex);
	switch_log_bind_logger(join_watch_logger, SWITCH_LOG_DEBUG, SWITCH_FALSE);

	api_call_start(&r->answer, "uuid_answer", "%s", uuid);
	switch_sleep(150 * 1000);
	api_call_start(&r->record, "uuid_record", "%s start /tmp/delayed_offer_ack_wait.wav", uuid);
	if (extra == ACK_RACE_DUPLICATE_ANSWER) {
		switch_sleep(50 * 1000);
		api_call_start(&r->extra, "uuid_answer", "%s", uuid);
	}
	api_call_join(&r->answer);
	api_call_join(&r->record);
	api_call_join(&r->extra);

	switch_sleep(200 * 1000);
	switch_log_unbind_logger(join_watch_logger);
	pthread_mutex_lock(&join_watch.mutex);
	r->joins = join_watch.joins;
	r->last_join = join_watch.last_join;
	join_watch.uuid[0] = '\0';
	pthread_mutex_unlock(&join_watch.mutex);
	r->answered_at = switch_channel_get_timetable(channel) ? switch_channel_get_timetable(channel)->answered : 0;
	r->up = switch_channel_ready(channel) && switch_channel_test_flag(channel, CF_ANSWERED);
	r->media = switch_core_media_ready(session, SWITCH_MEDIA_TYPE_AUDIO);

	switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
	r->sipp_ok = wait_t38_sipp(&sipp_pid, &sipp_status, 10000) == 1 && WIFEXITED(sipp_status) && WEXITSTATUS(sipp_status) == 0;

 end:
	stop_t38_sipp(&sipp_pid);
	if (session) {
		if (!switch_channel_down_nosig(channel)) {
			switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		}
		switch_core_session_rwunlock(session);
	}
}

/* Each racing request reached the endpoint while the answer was still waiting for its ACK: the endpoint
   logged a join of the pending answer for it, before the ACK completed the answer. */
static switch_bool_t ack_race_joined(ack_race_t *r, int requests)
{
	return (r->joins >= requests && r->answered_at && r->last_join < r->answered_at) ? SWITCH_TRUE : SWITCH_FALSE;
}

static void ack_race_free(ack_race_t *r)
{
	api_call_finish(&r->answer);
	api_call_finish(&r->record);
	api_call_finish(&r->extra);
}

static void show_event(switch_event_t *event) {
	char *str;
	/*print the event*/
	switch_event_serialize_json(event, &str);
	if (str) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s\n", str);
		switch_safe_free(str);
	}
}

static void event_handler(switch_event_t *event) 
{
	const char *new_ev = switch_event_get_header(event, "Event-Subclass");

	if (new_ev && !strcmp(new_ev, "sofia::gateway_invalid_digest_req")) { 
		test_success = 1;
	}

	show_event(event);
}

static void event_handler_reg_ok(switch_event_t *event) 
{
	const char *new_ev = switch_event_get_header(event, "Event-Subclass");
	
	if (new_ev && !strcmp(new_ev, "sofia::gateway_state")) {
		const char *state = switch_event_get_header(event, "State");
		if (state && !strcmp(state, "REGED")) {
			test_success++;
		}
	}

	show_event(event);
}

static void event_handler_reg_fail(switch_event_t *event) 
{
	const char *new_ev = switch_event_get_header(event, "Event-Subclass");

	if (new_ev && !strcmp(new_ev, "sofia::gateway_state")) {
		const char *state = switch_event_get_header(event, "State");
		if (state && !strcmp(state, "FAIL_WAIT")) {
			test_success++;
		}
	}

	show_event(event);
}

FST_CORE_EX_BEGIN("./conf-sipp", SCF_VG | SCF_USE_SQL)
{
	FST_MODULE_BEGIN(mod_sofia, uac-uas)
	{
		FST_SETUP_BEGIN()
		{
			switch_stream_handle_t stream = { 0 };
			SWITCH_STANDARD_STREAM(stream);
			switch_api_execute("sofia", "global siptrace on", NULL, &stream);
			if (test_sofia_debug) {
				switch_api_execute("sofia", "loglevel all 9", NULL, &stream);
				switch_api_execute("sofia", "tracelevel debug", NULL, &stream);
			}
			switch_safe_free(stream.data);

			switch_core_set_variable("spawn_instead_of_system", "true");

			if (getenv("FST_DELAYED_OFFER_ONLY")) {
				fst_requires_module("mod_sofia");
				fst_requires_module("mod_dialplan_xml");
				fst_requires_module("mod_dptools");
				fst_requires_module("mod_commands");
				fst_requires_module("mod_sndfile");
			} else if (getenv("FST_T38_ONLY")) {
				fst_requires_module("mod_sofia");
				fst_requires_module("mod_dialplan_xml");
				fst_requires_module("mod_dptools");
				fst_requires_module("mod_commands");
			} else {
				fst_requires_module("mod_sndfile");
				fst_requires_module("mod_voicemail");
				fst_requires_module("mod_sofia");
				fst_requires_module("mod_loopback");
				fst_requires_module("mod_console");
				fst_requires_module("mod_dptools");
				fst_requires_module("mod_dialplan_xml");
				fst_requires_module("mod_commands");
				fst_requires_module("mod_say_en");
				fst_requires_module("mod_tone_stream");
			}

		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
				switch_sleep(200 * 1000);
		}
		FST_TEARDOWN_END()

		FST_TEST_BEGIN(uac_telephone_event_check)
		{
			const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
			char uuid[100] = "";
			int sipp_ret;
			int sdp_count = 0;

			sipp_ret = start_sipp_uac(local_ip_v4, 5080, "1212121212", "sipp-scenarios/uac_telephone_event.xml", "");
			if (sipp_ret < 0 || sipp_ret == 127) {
				fst_check(!"sipp not found");
			} else {
				test_wait_for_uuid(uuid);
				if (!zstr(uuid)) {
					const char *sdp_str1 = NULL, *sdp_str2 = NULL;
					switch_core_session_t *session = switch_core_session_locate(uuid);
					switch_channel_t *channel = switch_core_session_get_channel(session);
					fst_check(channel);

					sdp_str1 = test_wait_for_chan_var(channel,"1");
					sdp_str2 = test_wait_for_chan_var(channel,"2");

					if (sdp_str1 && sdp_str2 && (strstr(sdp_str1,"telephone-event")) && (strstr(sdp_str2,"telephone-event"))){
						char *temp = NULL;
						sdp_count = 1;

						if ((temp = strstr(sdp_str2,"RTP/AVP"))) {
							int count = 0, i;

							for (i = 7; temp[i] != '\n' && i < 99; i++) {
								/* checking for payload-type 101.*/
								if(temp[i++] == '1' && temp[i++] == '0' && temp[i++] == '1')
									count++;
							}
							if (count > 1) {
								switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Duplicate entry of payload in SDP.\n");
								sdp_count = 0;
							}
						}

					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Telephone-event missing in SDP.\n");
					}
					switch_core_session_rwunlock(session);

				} else {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Uuid not found in Channel Data.\n");
				}

				fst_check(sdp_count == 1);
				/* sipp should timeout, attempt kill, just in case.*/
				kill_sipp();
			}
		}
		FST_TEST_END()

		FST_TEST_BEGIN(t38_identical_session_refresh_keeps_image)
		{
			const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
			switch_core_session_t *session = NULL;
			switch_channel_t *channel = NULL;
			switch_t38_options_t *t38_options = NULL;
			switch_core_session_message_t msg = { 0 };
			switch_status_t status;
			char uuid[100] = "";
			pid_t sipp_pid = -1;
			int sipp_status = 0;
			int result;
			int loop_count = 200;
			switch_bool_t t38_active = SWITCH_FALSE;

			result = start_t38_sipp(local_ip_v4, &sipp_pid);
			fst_xcheck(result == 0, "Unable to start SIPp T.38 peer");
			if (result != 0) {
				/* posix_spawnp does not define the PID on failure. */
				sipp_pid = -1;
				goto t38_cleanup;
			}

			test_wait_for_uuid(uuid);
			fst_xcheck(!zstr(uuid), "SIPp call did not create a FreeSWITCH channel");
			if (zstr(uuid)) {
				goto t38_cleanup;
			}
			session = switch_core_session_locate(uuid);
			fst_xcheck(session != NULL, "Unable to locate SIPp call session");
			if (!session) {
				goto t38_cleanup;
			}

			channel = switch_core_session_get_channel(session);
			status = switch_channel_wait_for_flag(channel, CF_ANSWERED, SWITCH_TRUE, 5000, NULL);
			fst_xcheck(status == SWITCH_STATUS_SUCCESS, "Initial audio call was not answered");
			if (status != SWITCH_STATUS_SUCCESS) {
				goto t38_cleanup;
			}

			switch_channel_set_variable(channel, "fax_enable_t38", "true");
			/* Model the fax application's capability, not successful negotiation. */
			switch_channel_set_app_flag_key("T38", channel, CF_APP_T38_POSSIBLE);
			t38_options = switch_core_session_alloc(session, sizeof(*t38_options));
			t38_options->T38FaxVersion = 0;
			t38_options->T38MaxBitRate = 14400;
			t38_options->T38FaxFillBitRemoval = 1;
			t38_options->T38FaxRateManagement = switch_core_session_strdup(session, "transferredTCF");
			t38_options->T38FaxMaxBuffer = 2000;
			t38_options->T38FaxMaxDatagram = 400;
			t38_options->T38FaxUdpEC = switch_core_session_strdup(session, "t38UDPRedundancy");
			switch_channel_set_private(channel, "t38_options", t38_options);
			switch_channel_set_app_flag_key("T38", channel, CF_APP_T38_REQ);

			msg.from = __FILE__;
			msg.message_id = SWITCH_MESSAGE_INDICATE_REQUEST_IMAGE_MEDIA;
			status = switch_core_session_receive_message(session, &msg);
			fst_xcheck(status == SWITCH_STATUS_SUCCESS, "Failed to request T.38 image media");
			if (status != SWITCH_STATUS_SUCCESS) {
				goto t38_cleanup;
			}

			while (loop_count-- && !switch_channel_down_nosig(channel)) {
				if (switch_channel_test_flag(channel, CF_IMAGE_SDP) &&
					switch_channel_test_app_flag_key("T38", channel, CF_APP_T38)) {
					t38_active = SWITCH_TRUE;
					break;
				}
				switch_sleep(10 * 1000);
			}
			fst_xcheck(t38_active, "T.38 negotiation never became active");

			result = wait_t38_sipp(&sipp_pid, &sipp_status, 15000);
			fst_xcheck(result == 1, "SIPp did not finish within the T.38 test deadline");
			fst_xcheck(result == 1 && WIFEXITED(sipp_status) && WEXITSTATUS(sipp_status) == 0,
					"SIPp T.38 scenario failed; inspect uac_t38_identical_refresh_*_errors.log");

		 t38_cleanup:
			fst_xcheck(stop_t38_sipp(&sipp_pid), "Unable to reap the owned SIPp process");
			if (session) {
				if (!switch_channel_down_nosig(channel)) {
					switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
				}
				switch_core_session_rwunlock(session);
			}
		}
		FST_TEST_END()

		FST_TEST_BEGIN(delayed_offer_media_request_during_ack_wait)
		{
			ack_race_t r = { 0 };

			delayed_offer_ack_race(switch_core_get_variable("local_ip_v4"), ACK_RACE_RECORD_ONLY, &r);
			fst_xcheck(r.sipp_started && r.parked, "SIPp call did not reach park");
			fst_xcheck(ack_race_joined(&r, 1), "record request did not reach the endpoint while the ACK was pending");
			fst_xcheck(api_call_ok(&r.answer) && r.answer.elapsed_ms < 3000, "uuid_answer did not complete when the ACK arrived");
			fst_xcheck(api_call_ok(&r.record), "uuid_record failed");
			fst_xcheck(r.up && r.media, "call not up with media after the ACK");
			fst_xcheck(r.sipp_ok, "SIPp delayed-offer scenario failed; inspect uac_delayed_offer_slow_ack_*_errors.log");
			ack_race_free(&r);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(delayed_offer_duplicate_answer_during_ack_wait)
		{
			ack_race_t r = { 0 };

			delayed_offer_ack_race(switch_core_get_variable("local_ip_v4"), ACK_RACE_DUPLICATE_ANSWER, &r);
			fst_xcheck(r.sipp_started && r.parked, "SIPp call did not reach park");
			fst_xcheck(ack_race_joined(&r, 2), "second uuid_answer and record did not reach the endpoint while the ACK was pending");
			fst_xcheck(api_call_ok(&r.answer) && r.answer.elapsed_ms < 3000, "uuid_answer did not complete when the ACK arrived");
			fst_xcheck(api_call_ok(&r.extra) && r.extra.elapsed_ms < 3000, "second uuid_answer did not complete when the ACK arrived");
			fst_xcheck(api_call_ok(&r.record), "uuid_record failed");
			fst_xcheck(r.up && r.media, "call not up with media after the ACK");
			fst_xcheck(r.sipp_ok, "SIPp delayed-offer scenario failed; inspect uac_delayed_offer_slow_ack_*_errors.log");
			ack_race_free(&r);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(uac_savp_check)
		{
			const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
			char uuid[100] = "";
			int sipp_ret;
			int sdp_count = 0;

			sipp_ret = start_sipp_uac(local_ip_v4, 5080, "1212121212", "sipp-scenarios/uac_savp_check.xml", "");
			if (sipp_ret < 0 || sipp_ret == 127) {
				fst_check(!"sipp not found");
			} else {
				test_wait_for_uuid(uuid);
				if (!zstr(uuid)) {
					const char *sdp_str1 = NULL, *sdp_str2 = NULL;
					const char *temp = NULL, *temp1 = NULL;
					switch_core_session_t *session = switch_core_session_locate(uuid);
					switch_channel_t *channel = switch_core_session_get_channel(session);
					fst_check(channel);

					sdp_str1 = test_wait_for_chan_var(channel,"1");
					sdp_str2 = test_wait_for_chan_var(channel,"2");

					if (sdp_str1 && sdp_str2 && (temp = strstr(sdp_str2,"RTP/SAVP")) && (temp1 = strstr(temp,"crypto"))) {
						int i = 0;

						sdp_count = 1;
						for (i = 0; temp1[i]; i++) {

							if ((temp = strstr(temp1,"RTP/SAVP"))) {
								if ((temp1 = strstr(temp,"crypto"))) {
									i = 0;
								} else {
									switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Fail due to no crypto found with SAVP.\n");
									sdp_count = 0;
									break;
								}
							}

						}

					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "SAVP not found in SDP.\n");
					}
					switch_core_session_rwunlock(session);

				} else {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Uuid not found in Channel Data.\n");
				}

				fst_check(sdp_count == 1); 
				/* sipp should timeout, attempt kill, just in case.*/
				kill_sipp();
			}
		}
		FST_TEST_END()

		FST_TEST_BEGIN(uac_digest_leak_udp)
		{
			switch_core_session_t *session;
			switch_call_cause_t cause;
			switch_status_t status;
			switch_channel_t *channel;
			const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
			int sipp_ret;

			switch_event_bind("sofia", SWITCH_EVENT_CUSTOM, NULL, event_handler, NULL);

			status = switch_ivr_originate(NULL, &session, &cause, "loopback/+15553334444", timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
			sipp_ret = start_sipp_uac(local_ip_v4, 5080, "1001", "sipp-scenarios/uac_digest_leak.xml", "");
			if (sipp_ret < 0 || sipp_ret == 127) {
				fst_check(!"sipp not found");
			} else {

				fst_check(status == SWITCH_STATUS_SUCCESS);
				if (!session) {
					fst_check(!"no session");
				} else {
					channel = switch_core_session_get_channel(session);
					fst_xcheck(switch_channel_get_state(channel) < CS_HANGUP, "Expect call not to be hung up");

					while (1) {
						int ret;
						switch_sleep(1000 * 1000);
						ret = switch_system("pidof sipp", SWITCH_TRUE);
						if (!ret) {
							break;
						}
					}

					switch_sleep(5000 * 1000);

					switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);

					switch_core_session_rwunlock(session);
					switch_sleep(1000 * 1000);

					switch_event_unbind_callback(event_handler);
					/* sipp should timeout, attempt kill, just in case.*/
					kill_sipp();
					fst_check(test_success);
				}
			}

			test_success = 0;
		}
		FST_TEST_END()

		FST_TEST_BEGIN(uac_digest_leak_tcp)
		{
			switch_core_session_t *session;
			switch_call_cause_t cause;
			switch_status_t status;
			switch_channel_t *channel;
			const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
			int sipp_ret;

			switch_event_bind("sofia", SWITCH_EVENT_CUSTOM, NULL, event_handler, NULL);

			status = switch_ivr_originate(NULL, &session, &cause, "loopback/+15553334444", timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
			sipp_ret = start_sipp_uac(local_ip_v4, 5080, "1001", "sipp-scenarios/uac_digest_leak-tcp.xml", "-t t1");
			if (sipp_ret < 0 || sipp_ret == 127) {
				fst_check(!"sipp not found");
			} else {
				fst_check(status == SWITCH_STATUS_SUCCESS);
				if (!session) {
					fst_check(!"no session");
				}

				channel = switch_core_session_get_channel(session);
				fst_xcheck(switch_channel_get_state(channel) < CS_HANGUP, "Expect call not to be hung up");

				while (1) {
					int ret;
					switch_sleep(1000 * 1000);
					ret = switch_system("pidof sipp", SWITCH_TRUE);
					if (!ret) {
						break;
					}
				}

				switch_sleep(5000 * 1000);

				switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);

				switch_core_session_rwunlock(session);
				switch_sleep(1000 * 1000);

				switch_event_unbind_callback(event_handler);
				/* sipp should timeout, attempt kill, just in case.*/
				kill_sipp();
				fst_check(test_success);
			}

			test_success = 0;
		}
		FST_TEST_END()

		FST_TEST_BEGIN(uac_digest_leak_udp_ipv6)
		{
			switch_core_session_t *session;
			switch_call_cause_t cause;
			switch_status_t status;
			switch_channel_t *channel;
			const char *local_ip_v6 = switch_core_get_variable("local_ip_v6");
			int sipp_ret;
			char *ipv6 = NULL;

			if (!has_ipv6()) {
				goto skiptest;
			}
			switch_event_bind("sofia", SWITCH_EVENT_CUSTOM, NULL, event_handler, NULL);

			if (!strchr(local_ip_v6,'[')) {
				ipv6 = switch_mprintf("[%s]", local_ip_v6);
			}
			status = switch_ivr_originate(NULL, &session, &cause, "loopback/+15553334444", timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);

			if (!ipv6) {
				sipp_ret = start_sipp_uac(local_ip_v6, 6060, "1001", "sipp-scenarios/uac_digest_leak-ipv6.xml", "-i [::1]");
			} else {
				sipp_ret = start_sipp_uac(ipv6, 6060, "1001", "sipp-scenarios/uac_digest_leak-ipv6.xml", "-i [::1] -mi [::1]");
			}

			if (sipp_ret < 0 || sipp_ret == 127) {
				fst_check(!"sipp not found");
			} else {
				fst_check(status == SWITCH_STATUS_SUCCESS);
				if (!session) {
					fst_check(!"no session");
				}

				channel = switch_core_session_get_channel(session);
				fst_xcheck(switch_channel_get_state(channel) < CS_HANGUP, "Expect call not to be hung up");

				while (1) {
					int ret;
					switch_sleep(1000 * 1000);
					ret = switch_system("pidof sipp", SWITCH_TRUE);
					if (!ret) {
						break;
					}
				}

				switch_sleep(5000 * 1000);

				switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);

				switch_core_session_rwunlock(session);
				switch_sleep(1000 * 1000);

				switch_event_unbind_callback(event_handler);
				/* sipp should timeout, attempt kill, just in case.*/
				kill_sipp();
				switch_safe_free(ipv6);
				fst_check(test_success);
			}
skiptest:
			test_success = 0;
		}
		FST_TEST_END()

		FST_TEST_BEGIN(register_ok)
		{
			const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
			int sipp_ret;

			switch_event_bind("sofia", SWITCH_EVENT_CUSTOM, NULL, event_handler_reg_ok, NULL);

			sipp_ret = start_sipp_uas(local_ip_v4, 6080, "sipp-scenarios/uas_register.xml", "");
			if (sipp_ret < 0 || sipp_ret == 127) {
				fst_check(!"sipp not found");
			} else {
				switch_sleep(1000 * 1000);

				register_gw();

				switch_sleep(5000 * 1000);

				switch_event_unbind_callback(event_handler_reg_ok);
				/* sipp should timeout, attempt kill, just in case.*/
				kill_sipp();
				fst_check(test_success);
			}

			test_success = 0;
		}
		FST_TEST_END()

		FST_TEST_BEGIN(register_403)
		{
			const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
			int sipp_ret;

			switch_event_bind("sofia", SWITCH_EVENT_CUSTOM, NULL, event_handler_reg_fail, NULL);

			sipp_ret = start_sipp_uas(local_ip_v4, 6080, "sipp-scenarios/uas_register_403.xml", "");
			if (sipp_ret < 0 || sipp_ret == 127) {
				fst_check(!"sipp not found");
			} else {
				switch_sleep(1000 * 1000);

				register_gw();

				switch_sleep(5000 * 1000);

				switch_event_unbind_callback(event_handler_reg_fail);
				/* sipp should timeout, attempt kill, just in case.*/
				kill_sipp();
				fst_check(test_success);
			}

			test_success = 0;
		}
		FST_TEST_END()

		FST_TEST_BEGIN(subscribe_auth_check)
		{
			const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
			const char *auth_password = switch_core_get_variable("default_password");
			switch_cache_db_handle_t *dbh = NULL;
			char *dsn = "sofia_reg_internal";
			char count[20]="";
			char count1[20]="";
			int sipp_ret;

			/* check without 407 Proxy Authentication. If count not 0 fail case. */
			sipp_ret = run_sipp(local_ip_v4, 5060, 6091, "1001", "sipp-scenarios/uac_subscriber.xml", auth_password, "");
			if (sipp_ret < 0 || sipp_ret == 127) {
				fst_check(!"sipp not found");
			} else {
				switch_sleep(100 * 1000);

				if (switch_cache_db_get_db_handle_dsn(&dbh, dsn) == SWITCH_STATUS_SUCCESS) {
					switch_cache_db_execute_sql2str(dbh, "select count(*) from  sip_subscriptions where contact like \"%1001%6091%\";", (char *)&count1, 20, NULL);
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Count : %s\n", count1);
				}
				fst_check_string_equals(count1, "0");

				/* check with 407  Proxy Authentication Required. If count not 1 fail case. */
				sipp_ret = run_sipp(local_ip_v4, 5060, 6090, "1001", "sipp-scenarios/uac_407_subscriber.xml", auth_password, "");
				if (sipp_ret < 0 || sipp_ret == 127) {
					fst_check(!"sipp not found");
				} else {
					switch_sleep(100 * 1000);

					switch_cache_db_execute_sql2str(dbh, "select count(*) from  sip_subscriptions where contact like \"%1001%6090%\";", (char *)&count, 20, NULL);
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Count : %s\n", count);
					fst_check_string_equals(count, "1");

				}
			}

			/* sipp should timeout, attempt kill, just in case.*/
			kill_sipp();
		}
		FST_TEST_END()

		FST_TEST_BEGIN(register_no_challange)
		{
			const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
			int sipp_ret;

			switch_event_bind("sofia", SWITCH_EVENT_CUSTOM, NULL, event_handler_reg_ok, NULL);

			sipp_ret = start_sipp_uas(local_ip_v4, 6080, "sipp-scenarios/uas_register_no_challange.xml", "");
			if (sipp_ret < 0 || sipp_ret == 127) {
				fst_check(!"sipp not found");
			} else {
				switch_sleep(1000 * 1000);

				register_gw();

				switch_sleep(5000 * 1000);

				/*the REGISTER with Expires 0 */
				unregister_gw();

				switch_sleep(1000 * 1000);

				register_gw();

				switch_sleep(1000 * 1000);

				switch_event_unbind_callback(event_handler_reg_ok);

				/* sipp should timeout, attempt kill, just in case.*/
				kill_sipp();
				fst_check(test_success);
			}

			test_success = 0;
		}
		FST_TEST_END()

		FST_TEST_BEGIN(invite_407)
		{
			const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
			int sipp_ret;
			switch_core_session_t *session; 
			switch_call_cause_t cause;
			switch_status_t status;
			switch_channel_t *channel;
			char *to;
			const int inv_sipp_port = 6082;

			sipp_ret = start_sipp_uas(local_ip_v4, inv_sipp_port, "sipp-scenarios/uas_407.xml", "");
			if (sipp_ret < 0 || sipp_ret == 127) {
				fst_check(!"sipp not found");
			} else {
				switch_sleep(1000 * 1000);
				to = switch_mprintf("sofia/gateway/testgw-noreg/sipp@%s:%d", local_ip_v4, inv_sipp_port);
				/*originate will fail if the 407 we get from sipp is dropped due to wrong IP.*/
				status = switch_ivr_originate(NULL, &session, &cause, to, timeout_sec, NULL, NULL, NULL, NULL, NULL, SOF_NONE, NULL, NULL);
				fst_check(status == SWITCH_STATUS_SUCCESS);

				/*test is considered PASSED if we get a session*/
				fst_check(session);
				if (session) {
					switch_sleep(1000 * 1000);
					channel = switch_core_session_get_channel(session);
					switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
					switch_core_session_rwunlock(session);
				}

				switch_safe_free(to);
				/* sipp should timeout, attempt kill, just in case.*/
				kill_sipp();
			}
		}
		FST_TEST_END()
		FST_TEST_BEGIN(uac_trickle_ice_info)
		{
			const char *local_ip_v4 = switch_core_get_variable("local_ip_v4");
			int sipp_ret;

			sipp_ret = start_sipp_uac(local_ip_v4, 5080, "1212121212",
					switch_core_sprintf(fst_pool, "sipp-scenarios/%s", "uac_trickle_ice_info.xml"),
					"");

			if (sipp_ret < 0 || sipp_ret == 127) {
				fst_check(!"sipp not found");
			} else {
				switch_sleep(4000 * 1000);
				kill_sipp();
				fst_check(test_success == 0); /* we only check that sipp ran and FS didn't error */
			}
		}
		FST_TEST_END()
	}
	FST_MODULE_END()
}
FST_CORE_END()
