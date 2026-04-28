#include <switch.h>

static void bundle_reject(switch_bundle_group_t *group, const char *reason)
{
	if (group) {
		group->state = SWITCH_BUNDLE_STATE_REJECTED;
		switch_copy_string(group->reject_reason, reason ? reason : "BUNDLE rejected", sizeof(group->reject_reason));
	}
}

static switch_bool_t bundle_mid_eq(const char *a, const char *b)
{
	return (!zstr(a) && !zstr(b) && !strcmp(a, b)) ? SWITCH_TRUE : SWITCH_FALSE;
}

static switch_bool_t bundle_group_has_mid(const switch_bundle_group_t *group, const char *mid)
{
	uint32_t i;
	if (!group || zstr(mid)) return SWITCH_FALSE;
	for (i = 0; i < group->offered_mid_count; i++) {
		if (bundle_mid_eq(group->offered_mids[i], mid)) return SWITCH_TRUE;
	}
	return SWITCH_FALSE;
}

static void bundle_refresh_mline_membership(switch_bundle_group_t *group, switch_bundle_mline_t *mline)
{
	if (!group || !mline || zstr(mline->mid)) return;
	if (bundle_group_has_mid(group, mline->mid)) {
		mline->in_bundle_group = 1;
		if (bundle_mid_eq(group->bundle_tag_mid, mline->mid)) {
			mline->bundle_tag = 1;
			group->bundle_tag_mline_index = mline->mline_index;
		}
	}
}

SWITCH_DECLARE(switch_bundle_policy_t) switch_bundle_policy_parse(const char *value, switch_bundle_policy_t dft)
{
	if (zstr(value)) return dft;
	if (!strcasecmp(value, "off") || !strcasecmp(value, "false") || !strcasecmp(value, "disabled")) return SWITCH_BUNDLE_POLICY_OFF;
	if (!strcasecmp(value, "auto") || !strcasecmp(value, "true") || !strcasecmp(value, "on")) return SWITCH_BUNDLE_POLICY_AUTO;
	if (!strcasecmp(value, "force") || !strcasecmp(value, "required")) return SWITCH_BUNDLE_POLICY_FORCE;
	return dft;
}

SWITCH_DECLARE(const char *) switch_bundle_policy_str(switch_bundle_policy_t policy)
{
	switch (policy) {
	case SWITCH_BUNDLE_POLICY_OFF: return "off";
	case SWITCH_BUNDLE_POLICY_AUTO: return "auto";
	case SWITCH_BUNDLE_POLICY_FORCE: return "force";
	}
	return "unknown";
}

SWITCH_DECLARE(const char *) switch_bundle_state_str(switch_bundle_state_t state)
{
	switch (state) {
	case SWITCH_BUNDLE_STATE_NONE: return "none";
	case SWITCH_BUNDLE_STATE_OFFERED: return "offered";
	case SWITCH_BUNDLE_STATE_ACCEPTED: return "accepted";
	case SWITCH_BUNDLE_STATE_REJECTED: return "rejected";
	}
	return "unknown";
}

SWITCH_DECLARE(void) switch_bundle_group_init(switch_bundle_group_t *group, switch_bundle_policy_t policy)
{
	if (!group) return;
	memset(group, 0, sizeof(*group));
	group->policy = policy;
	group->state = SWITCH_BUNDLE_STATE_NONE;
	group->bundle_tag_mline_index = -1;
}

SWITCH_DECLARE(void) switch_bundle_group_reset(switch_bundle_group_t *group)
{
	switch_bundle_policy_t policy;
	uint32_t generation;
	if (!group) return;
	policy = group->policy;
	generation = group->generation + 1;
	switch_bundle_group_init(group, policy);
	group->generation = generation;
}

SWITCH_DECLARE(switch_status_t) switch_bundle_group_set_offered_mids(switch_bundle_group_t *group, const char *value)
{
	char buf[SWITCH_BUNDLE_MAX_MIDS * (SWITCH_BUNDLE_MAX_MID_LEN + 2)] = { 0 };
	char *argv[SWITCH_BUNDLE_MAX_MIDS + 2] = { 0 };
	int argc, i, start = 0;
	if (!group || zstr(value)) return SWITCH_STATUS_FALSE;
	switch_copy_string(buf, value, sizeof(buf));
	argc = switch_separate_string(buf, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	if (argc < 1) return SWITCH_STATUS_FALSE;
	if (!strcasecmp(argv[0], "BUNDLE")) start = 1;
	group->offered_mid_count = 0;
	group->bundle_tag_mid[0] = '\0';
	group->bundle_tag_mline_index = -1;
	for (i = start; i < argc && group->offered_mid_count < SWITCH_BUNDLE_MAX_MIDS; i++) {
		if (zstr(argv[i])) continue;
		switch_copy_string(group->offered_mids[group->offered_mid_count], argv[i], sizeof(group->offered_mids[group->offered_mid_count]));
		if (!group->offered_mid_count) switch_copy_string(group->bundle_tag_mid, argv[i], sizeof(group->bundle_tag_mid));
		group->offered_mid_count++;
	}
	if (!group->offered_mid_count) return SWITCH_STATUS_FALSE;
	group->state = SWITCH_BUNDLE_STATE_OFFERED;
	for (i = 0; i < (int) group->mline_count; i++) {
		group->mlines[i].in_bundle_group = 0;
		group->mlines[i].bundle_tag = 0;
		bundle_refresh_mline_membership(group, &group->mlines[i]);
	}
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_bundle_mline_t *) switch_bundle_group_add_mline(switch_bundle_group_t *group, int mline_index, switch_media_type_t media_type, const char *mid, switch_port_t port, switch_bool_t rtcp_mux, switch_bool_t bundle_only, switch_bool_t rtcp_mux_only)
{
	switch_bundle_mline_t *mline;
	if (!group || group->mline_count >= SWITCH_BUNDLE_MAX_MLINES) return NULL;
	mline = &group->mlines[group->mline_count++];
	memset(mline, 0, sizeof(*mline));
	mline->mline_index = mline_index;
	mline->media_type = media_type;
	if (!zstr(mid)) switch_copy_string(mline->mid, mid, sizeof(mline->mid));
	mline->zero_port = port ? 0 : 1;
	mline->rejected = (port || bundle_only) ? 0 : 1;
	mline->rtcp_mux = rtcp_mux ? 1 : 0;
	mline->bundle_only = bundle_only ? 1 : 0;
	mline->rtcp_mux_only = rtcp_mux_only ? 1 : 0;
	bundle_refresh_mline_membership(group, mline);
	return mline;
}

SWITCH_DECLARE(switch_bundle_mline_t *) switch_bundle_group_find_mline_by_mid(switch_bundle_group_t *group, const char *mid)
{
	uint32_t i;
	if (!group || zstr(mid)) return NULL;
	for (i = 0; i < group->mline_count; i++) if (bundle_mid_eq(group->mlines[i].mid, mid)) return &group->mlines[i];
	return NULL;
}

SWITCH_DECLARE(switch_bundle_mline_t *) switch_bundle_group_find_mline_by_index(switch_bundle_group_t *group, int mline_index)
{
	uint32_t i;
	if (!group) return NULL;
	for (i = 0; i < group->mline_count; i++) if (group->mlines[i].mline_index == mline_index) return &group->mlines[i];
	return NULL;
}

SWITCH_DECLARE(switch_status_t) switch_bundle_mline_set_remote_mid_ext(switch_bundle_mline_t *mline, uint8_t ext_id)
{
	if (!mline || !ext_id) return SWITCH_STATUS_FALSE;
	mline->remote_mid_ext_id = ext_id;
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status_t) switch_bundle_mline_set_local_mid_ext(switch_bundle_mline_t *mline, uint8_t ext_id)
{
	if (!mline || !ext_id) return SWITCH_STATUS_FALSE;
	mline->local_mid_ext_id = ext_id;
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status_t) switch_bundle_mline_add_payload_type(switch_bundle_mline_t *mline, switch_payload_t pt)
{
	uint32_t i;
	if (!mline) return SWITCH_STATUS_FALSE;
	for (i = 0; i < mline->payload_count; i++) if (mline->payloads[i] == pt) return SWITCH_STATUS_SUCCESS;
	if (mline->payload_count >= SWITCH_BUNDLE_MAX_PAYLOADS) return SWITCH_STATUS_FALSE;
	mline->payloads[mline->payload_count++] = pt;
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status_t) switch_bundle_group_learn_remote_ssrc(switch_bundle_group_t *group, switch_bundle_mline_t *mline, uint32_t ssrc)
{
	uint32_t i;

	if (!group || !mline || !ssrc || !mline->in_bundle_group || mline->rejected) {
		return SWITCH_STATUS_FALSE;
	}

	for (i = 0; i < group->mline_count; i++) {
		switch_bundle_mline_t *other = &group->mlines[i];

		if (other == mline || !other->in_bundle_group || other->rejected) {
			continue;
		}

		if (other->remote_ssrc == ssrc) {
			return SWITCH_STATUS_FALSE;
		}
	}

	if (mline->remote_ssrc && mline->remote_ssrc != ssrc) {
		return SWITCH_STATUS_FALSE;
	}

	mline->remote_ssrc = ssrc;
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_bundle_mline_t *) switch_bundle_group_demux_rtp(switch_bundle_group_t *group,
												 const char *mid,
												 uint32_t ssrc,
												 switch_payload_t pt,
												 switch_bundle_demux_t *method)
{
	uint32_t i;
	switch_bundle_mline_t *mline = NULL;

	if (method) {
		*method = SWITCH_BUNDLE_DEMUX_NONE;
	}

	if (!group || group->state != SWITCH_BUNDLE_STATE_ACCEPTED) {
		return NULL;
	}

	if (!zstr(mid)) {
		mline = switch_bundle_group_find_mline_by_mid(group, mid);
		if (!mline || !mline->in_bundle_group || mline->rejected) {
			return NULL;
		}

		if (ssrc && switch_bundle_group_learn_remote_ssrc(group, mline, ssrc) != SWITCH_STATUS_SUCCESS) {
			return NULL;
		}

		if (method) {
			*method = SWITCH_BUNDLE_DEMUX_MID;
		}
		return mline;
	}

	if (ssrc) {
		for (i = 0; i < group->mline_count; i++) {
			switch_bundle_mline_t *candidate = &group->mlines[i];

			if (!candidate->in_bundle_group || candidate->rejected) {
				continue;
			}

			if (candidate->remote_ssrc == ssrc) {
				if (method) {
					*method = SWITCH_BUNDLE_DEMUX_SSRC;
				}
				return candidate;
			}
		}
	}

	if (switch_bundle_group_payload_type_unique(group, pt, &mline) == SWITCH_TRUE) {
		if (ssrc && switch_bundle_group_learn_remote_ssrc(group, mline, ssrc) != SWITCH_STATUS_SUCCESS) {
			return NULL;
		}
		if (method) {
			*method = SWITCH_BUNDLE_DEMUX_PAYLOAD_TYPE;
		}
		return mline;
	}

	return NULL;
}

SWITCH_DECLARE(switch_bool_t) switch_bundle_group_payload_type_unique(switch_bundle_group_t *group, switch_payload_t pt, switch_bundle_mline_t **mline_out)
{
	uint32_t i, j, matches = 0;
	switch_bundle_mline_t *match = NULL;
	if (mline_out) *mline_out = NULL;
	if (!group) return SWITCH_FALSE;
	for (i = 0; i < group->mline_count; i++) {
		switch_bundle_mline_t *mline = &group->mlines[i];
		if (!mline->in_bundle_group || mline->rejected) continue;
		for (j = 0; j < mline->payload_count; j++) {
			if (mline->payloads[j] == pt) {
				matches++;
				match = mline;
				break;
			}
		}
	}
	if (matches == 1 && mline_out) *mline_out = match;
	return matches == 1 ? SWITCH_TRUE : SWITCH_FALSE;
}

SWITCH_DECLARE(switch_status_t) switch_bundle_group_validate(switch_bundle_group_t *group)
{
	uint32_t i;
	if (!group) return SWITCH_STATUS_FALSE;
	group->reject_reason[0] = '\0';
	if (!group->offered_mid_count) {
		group->state = SWITCH_BUNDLE_STATE_NONE;
		return SWITCH_STATUS_SUCCESS;
	}
	if (group->policy == SWITCH_BUNDLE_POLICY_OFF) {
		bundle_reject(group, "rtp-bundle policy is off");
		return SWITCH_STATUS_FALSE;
	}
	for (i = 0; i < group->offered_mid_count; i++) {
		switch_bundle_mline_t *mline = switch_bundle_group_find_mline_by_mid(group, group->offered_mids[i]);
		if (!mline) {
			bundle_reject(group, "BUNDLE group references missing MID");
			return SWITCH_STATUS_FALSE;
		}
		mline->in_bundle_group = 1;
		if (i == 0) {
			mline->bundle_tag = 1;
			group->bundle_tag_mline_index = mline->mline_index;
			if (mline->rejected || mline->zero_port) {
				bundle_reject(group, "BUNDLE-tag m-line is rejected");
				return SWITCH_STATUS_FALSE;
			}
		}
		if (zstr(mline->mid)) {
			bundle_reject(group, "accepted bundled m-line is missing MID");
			return SWITCH_STATUS_FALSE;
		}
		if (!mline->rtcp_mux && !(mline->bundle_only && mline->zero_port)) {
			bundle_reject(group, "accepted bundled m-line is missing rtcp-mux");
			return SWITCH_STATUS_FALSE;
		}
		if (!mline->remote_mid_ext_id && group->policy == SWITCH_BUNDLE_POLICY_FORCE) {
			bundle_reject(group, "accepted bundled m-line is missing MID RTP extension");
			return SWITCH_STATUS_FALSE;
		}
	}
	group->state = SWITCH_BUNDLE_STATE_ACCEPTED;
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(const char *) switch_bundle_group_reject_reason(const switch_bundle_group_t *group)
{
	return (!group || zstr(group->reject_reason)) ? "" : group->reject_reason;
}
