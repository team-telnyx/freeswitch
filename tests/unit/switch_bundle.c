#include <switch.h>
#include <test/switch_test.h>

FST_CORE_BEGIN("./conf")
{
FST_SUITE_BEGIN(switch_bundle)
{
	FST_TEST_BEGIN(test_policy_parse)
	{
		fst_check(switch_bundle_policy_parse(NULL, SWITCH_BUNDLE_POLICY_OFF) == SWITCH_BUNDLE_POLICY_OFF);
		fst_check(switch_bundle_policy_parse("off", SWITCH_BUNDLE_POLICY_AUTO) == SWITCH_BUNDLE_POLICY_OFF);
		fst_check(switch_bundle_policy_parse("auto", SWITCH_BUNDLE_POLICY_OFF) == SWITCH_BUNDLE_POLICY_AUTO);
		fst_check(switch_bundle_policy_parse("force", SWITCH_BUNDLE_POLICY_OFF) == SWITCH_BUNDLE_POLICY_FORCE);
		fst_check(!strcmp(switch_bundle_policy_str(SWITCH_BUNDLE_POLICY_AUTO), "auto"));
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_group_first_validation)
	{
		switch_bundle_group_t group;
		switch_bundle_mline_t *audio, *video;
		switch_bundle_group_init(&group, SWITCH_BUNDLE_POLICY_AUTO);
		fst_check(switch_bundle_group_set_offered_mids(&group, "BUNDLE 0 1") == SWITCH_STATUS_SUCCESS);
		audio = switch_bundle_group_add_mline(&group, 0, SWITCH_MEDIA_TYPE_AUDIO, "0", 10000, SWITCH_TRUE, SWITCH_FALSE, SWITCH_FALSE);
		video = switch_bundle_group_add_mline(&group, 1, SWITCH_MEDIA_TYPE_VIDEO, "1", 10000, SWITCH_TRUE, SWITCH_FALSE, SWITCH_FALSE);
		fst_check(audio && video);
		fst_check(audio->in_bundle_group && audio->bundle_tag);
		fst_check(video->in_bundle_group && !video->bundle_tag);
		fst_check(switch_bundle_mline_set_remote_mid_ext(audio, 1) == SWITCH_STATUS_SUCCESS);
		fst_check(switch_bundle_mline_set_remote_mid_ext(video, 1) == SWITCH_STATUS_SUCCESS);
		fst_check(switch_bundle_group_validate(&group) == SWITCH_STATUS_SUCCESS);
		fst_check(group.state == SWITCH_BUNDLE_STATE_ACCEPTED);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_mlines_first_validation)
	{
		switch_bundle_group_t group;
		switch_bundle_mline_t *audio, *video;
		switch_bundle_group_init(&group, SWITCH_BUNDLE_POLICY_AUTO);
		audio = switch_bundle_group_add_mline(&group, 0, SWITCH_MEDIA_TYPE_AUDIO, "audio", 20000, SWITCH_TRUE, SWITCH_FALSE, SWITCH_FALSE);
		video = switch_bundle_group_add_mline(&group, 1, SWITCH_MEDIA_TYPE_VIDEO, "video", 20000, SWITCH_TRUE, SWITCH_FALSE, SWITCH_FALSE);
		fst_check(audio && video);
		fst_check(switch_bundle_group_set_offered_mids(&group, "audio video") == SWITCH_STATUS_SUCCESS);
		fst_check(audio->in_bundle_group && audio->bundle_tag);
		fst_check(video->in_bundle_group);
		fst_check(switch_bundle_group_validate(&group) == SWITCH_STATUS_SUCCESS);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_policy_off_rejects)
	{
		switch_bundle_group_t group;
		switch_bundle_group_init(&group, SWITCH_BUNDLE_POLICY_OFF);
		fst_check(switch_bundle_group_set_offered_mids(&group, "BUNDLE 0 1") == SWITCH_STATUS_SUCCESS);
		fst_check(switch_bundle_group_add_mline(&group, 0, SWITCH_MEDIA_TYPE_AUDIO, "0", 10000, SWITCH_TRUE, SWITCH_FALSE, SWITCH_FALSE));
		fst_check(switch_bundle_group_add_mline(&group, 1, SWITCH_MEDIA_TYPE_VIDEO, "1", 10000, SWITCH_TRUE, SWITCH_FALSE, SWITCH_FALSE));
		fst_check(switch_bundle_group_validate(&group) == SWITCH_STATUS_FALSE);
		fst_check(group.state == SWITCH_BUNDLE_STATE_REJECTED);
		fst_check(strstr(switch_bundle_group_reject_reason(&group), "policy"));
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_missing_mid_rejects)
	{
		switch_bundle_group_t group;
		switch_bundle_group_init(&group, SWITCH_BUNDLE_POLICY_AUTO);
		fst_check(switch_bundle_group_set_offered_mids(&group, "BUNDLE 0 1") == SWITCH_STATUS_SUCCESS);
		fst_check(switch_bundle_group_add_mline(&group, 0, SWITCH_MEDIA_TYPE_AUDIO, "0", 10000, SWITCH_TRUE, SWITCH_FALSE, SWITCH_FALSE));
		fst_check(switch_bundle_group_validate(&group) == SWITCH_STATUS_FALSE);
		fst_check(group.state == SWITCH_BUNDLE_STATE_REJECTED);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_missing_rtcp_mux_rejects)
	{
		switch_bundle_group_t group;
		switch_bundle_group_init(&group, SWITCH_BUNDLE_POLICY_AUTO);
		fst_check(switch_bundle_group_set_offered_mids(&group, "BUNDLE 0 1") == SWITCH_STATUS_SUCCESS);
		fst_check(switch_bundle_group_add_mline(&group, 0, SWITCH_MEDIA_TYPE_AUDIO, "0", 10000, SWITCH_TRUE, SWITCH_FALSE, SWITCH_FALSE));
		fst_check(switch_bundle_group_add_mline(&group, 1, SWITCH_MEDIA_TYPE_VIDEO, "1", 10002, SWITCH_FALSE, SWITCH_FALSE, SWITCH_FALSE));
		fst_check(switch_bundle_group_validate(&group) == SWITCH_STATUS_FALSE);
		fst_check(strstr(switch_bundle_group_reject_reason(&group), "rtcp-mux"));
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_force_requires_mid_ext)
	{
		switch_bundle_group_t group;
		switch_bundle_mline_t *audio, *video;
		switch_bundle_group_init(&group, SWITCH_BUNDLE_POLICY_FORCE);
		fst_check(switch_bundle_group_set_offered_mids(&group, "BUNDLE 0 1") == SWITCH_STATUS_SUCCESS);
		audio = switch_bundle_group_add_mline(&group, 0, SWITCH_MEDIA_TYPE_AUDIO, "0", 10000, SWITCH_TRUE, SWITCH_FALSE, SWITCH_FALSE);
		video = switch_bundle_group_add_mline(&group, 1, SWITCH_MEDIA_TYPE_VIDEO, "1", 10000, SWITCH_TRUE, SWITCH_FALSE, SWITCH_FALSE);
		fst_check(audio && video);
		fst_check(switch_bundle_group_validate(&group) == SWITCH_STATUS_FALSE);
		fst_check(strstr(switch_bundle_group_reject_reason(&group), "MID RTP extension"));
		fst_check(switch_bundle_mline_set_remote_mid_ext(audio, 1) == SWITCH_STATUS_SUCCESS);
		fst_check(switch_bundle_mline_set_remote_mid_ext(video, 1) == SWITCH_STATUS_SUCCESS);
		fst_check(switch_bundle_group_validate(&group) == SWITCH_STATUS_SUCCESS);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_bundle_only_zero_port)
	{
		switch_bundle_group_t group;
		switch_bundle_mline_t *audio, *video;
		switch_bundle_group_init(&group, SWITCH_BUNDLE_POLICY_AUTO);
		fst_check(switch_bundle_group_set_offered_mids(&group, "BUNDLE 0 1") == SWITCH_STATUS_SUCCESS);
		audio = switch_bundle_group_add_mline(&group, 0, SWITCH_MEDIA_TYPE_AUDIO, "0", 10000, SWITCH_TRUE, SWITCH_FALSE, SWITCH_FALSE);
		video = switch_bundle_group_add_mline(&group, 1, SWITCH_MEDIA_TYPE_VIDEO, "1", 0, SWITCH_FALSE, SWITCH_TRUE, SWITCH_FALSE);
		fst_check(audio && video);
		fst_check(video->zero_port && video->bundle_only && !video->rejected);
		fst_check(switch_bundle_group_validate(&group) == SWITCH_STATUS_SUCCESS);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_payload_type_uniqueness)
	{
		switch_bundle_group_t group;
		switch_bundle_mline_t *audio, *video, *match = NULL;
		switch_bundle_group_init(&group, SWITCH_BUNDLE_POLICY_AUTO);
		fst_check(switch_bundle_group_set_offered_mids(&group, "BUNDLE 0 1") == SWITCH_STATUS_SUCCESS);
		audio = switch_bundle_group_add_mline(&group, 0, SWITCH_MEDIA_TYPE_AUDIO, "0", 10000, SWITCH_TRUE, SWITCH_FALSE, SWITCH_FALSE);
		video = switch_bundle_group_add_mline(&group, 1, SWITCH_MEDIA_TYPE_VIDEO, "1", 10000, SWITCH_TRUE, SWITCH_FALSE, SWITCH_FALSE);
		fst_check(audio && video);
		fst_check(switch_bundle_mline_add_payload_type(audio, 111) == SWITCH_STATUS_SUCCESS);
		fst_check(switch_bundle_mline_add_payload_type(video, 96) == SWITCH_STATUS_SUCCESS);
		fst_check(switch_bundle_group_payload_type_unique(&group, 96, &match) == SWITCH_TRUE);
		fst_check(match == video);
		fst_check(switch_bundle_mline_add_payload_type(audio, 96) == SWITCH_STATUS_SUCCESS);
		fst_check(switch_bundle_group_payload_type_unique(&group, 96, &match) == SWITCH_FALSE);
	}
	FST_TEST_END()
	FST_TEST_BEGIN(test_rtp_demux_order_mid_ssrc_pt)
	{
		switch_bundle_group_t group;
		switch_bundle_mline_t *audio, *video, *match;
		switch_bundle_demux_t method = SWITCH_BUNDLE_DEMUX_NONE;

		switch_bundle_group_init(&group, SWITCH_BUNDLE_POLICY_AUTO);
		fst_check(switch_bundle_group_set_offered_mids(&group, "BUNDLE 0 1") == SWITCH_STATUS_SUCCESS);
		audio = switch_bundle_group_add_mline(&group, 0, SWITCH_MEDIA_TYPE_AUDIO, "0", 10000, SWITCH_TRUE, SWITCH_FALSE, SWITCH_FALSE);
		video = switch_bundle_group_add_mline(&group, 1, SWITCH_MEDIA_TYPE_VIDEO, "1", 10000, SWITCH_TRUE, SWITCH_FALSE, SWITCH_FALSE);
		fst_check(audio && video);
		fst_check(switch_bundle_mline_add_payload_type(audio, 111) == SWITCH_STATUS_SUCCESS);
		fst_check(switch_bundle_mline_add_payload_type(video, 96) == SWITCH_STATUS_SUCCESS);
		fst_check(switch_bundle_group_validate(&group) == SWITCH_STATUS_SUCCESS);

		match = switch_bundle_group_demux_rtp(&group, "1", 2222, 111, &method);
		fst_check(match == video);
		fst_check(method == SWITCH_BUNDLE_DEMUX_MID);
		fst_check(video->remote_ssrc == 2222);

		match = switch_bundle_group_demux_rtp(&group, NULL, 2222, 111, &method);
		fst_check(match == video);
		fst_check(method == SWITCH_BUNDLE_DEMUX_SSRC);

		match = switch_bundle_group_demux_rtp(&group, NULL, 0, 96, &method);
		fst_check(match == video);
		fst_check(method == SWITCH_BUNDLE_DEMUX_PAYLOAD_TYPE);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_rtp_demux_ambiguous_payload_drops)
	{
		switch_bundle_group_t group;
		switch_bundle_mline_t *audio, *video, *match;
		switch_bundle_demux_t method = SWITCH_BUNDLE_DEMUX_MID;

		switch_bundle_group_init(&group, SWITCH_BUNDLE_POLICY_AUTO);
		fst_check(switch_bundle_group_set_offered_mids(&group, "BUNDLE 0 1") == SWITCH_STATUS_SUCCESS);
		audio = switch_bundle_group_add_mline(&group, 0, SWITCH_MEDIA_TYPE_AUDIO, "0", 10000, SWITCH_TRUE, SWITCH_FALSE, SWITCH_FALSE);
		video = switch_bundle_group_add_mline(&group, 1, SWITCH_MEDIA_TYPE_VIDEO, "1", 10000, SWITCH_TRUE, SWITCH_FALSE, SWITCH_FALSE);
		fst_check(audio && video);
		fst_check(switch_bundle_mline_add_payload_type(audio, 96) == SWITCH_STATUS_SUCCESS);
		fst_check(switch_bundle_mline_add_payload_type(video, 96) == SWITCH_STATUS_SUCCESS);
		fst_check(switch_bundle_group_validate(&group) == SWITCH_STATUS_SUCCESS);

		match = switch_bundle_group_demux_rtp(&group, NULL, 0, 96, &method);
		fst_check(match == NULL);
		fst_check(method == SWITCH_BUNDLE_DEMUX_NONE);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_rtp_demux_rejects_ssrc_collision)
	{
		switch_bundle_group_t group;
		switch_bundle_mline_t *audio, *video;

		switch_bundle_group_init(&group, SWITCH_BUNDLE_POLICY_AUTO);
		fst_check(switch_bundle_group_set_offered_mids(&group, "BUNDLE 0 1") == SWITCH_STATUS_SUCCESS);
		audio = switch_bundle_group_add_mline(&group, 0, SWITCH_MEDIA_TYPE_AUDIO, "0", 10000, SWITCH_TRUE, SWITCH_FALSE, SWITCH_FALSE);
		video = switch_bundle_group_add_mline(&group, 1, SWITCH_MEDIA_TYPE_VIDEO, "1", 10000, SWITCH_TRUE, SWITCH_FALSE, SWITCH_FALSE);
		fst_check(audio && video);
		fst_check(switch_bundle_group_validate(&group) == SWITCH_STATUS_SUCCESS);

		fst_check(switch_bundle_group_learn_remote_ssrc(&group, audio, 4444) == SWITCH_STATUS_SUCCESS);
		fst_check(switch_bundle_group_learn_remote_ssrc(&group, video, 4444) == SWITCH_STATUS_FALSE);
		fst_check(video->remote_ssrc == 0);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(test_rtp_demux_drops_mid_ssrc_collision)
	{
		switch_bundle_group_t group;
		switch_bundle_mline_t *audio, *video, *match;
		switch_bundle_demux_t method = SWITCH_BUNDLE_DEMUX_NONE;

		switch_bundle_group_init(&group, SWITCH_BUNDLE_POLICY_AUTO);
		fst_check(switch_bundle_group_set_offered_mids(&group, "BUNDLE 0 1") == SWITCH_STATUS_SUCCESS);
		audio = switch_bundle_group_add_mline(&group, 0, SWITCH_MEDIA_TYPE_AUDIO, "0", 10000, SWITCH_TRUE, SWITCH_FALSE, SWITCH_FALSE);
		video = switch_bundle_group_add_mline(&group, 1, SWITCH_MEDIA_TYPE_VIDEO, "1", 10000, SWITCH_TRUE, SWITCH_FALSE, SWITCH_FALSE);
		fst_check(audio && video);
		fst_check(switch_bundle_group_validate(&group) == SWITCH_STATUS_SUCCESS);
		fst_check(switch_bundle_group_learn_remote_ssrc(&group, audio, 5555) == SWITCH_STATUS_SUCCESS);

		match = switch_bundle_group_demux_rtp(&group, "1", 5555, 96, &method);
		fst_check(match == NULL);
		fst_check(method == SWITCH_BUNDLE_DEMUX_NONE);
		fst_check(video->remote_ssrc == 0);
	}
	FST_TEST_END()

}
FST_SUITE_END()
}
FST_CORE_END()
