#ifndef SWITCH_BUNDLE_H
#define SWITCH_BUNDLE_H

#include <switch.h>

SWITCH_BEGIN_EXTERN_C

#define SWITCH_BUNDLE_MAX_MLINES 8
#define SWITCH_BUNDLE_MAX_MIDS 8
#define SWITCH_BUNDLE_MAX_MID_LEN 31
#define SWITCH_BUNDLE_MAX_PAYLOADS 32
#define SWITCH_BUNDLE_REJECT_REASON_LEN 128

typedef enum {
	SWITCH_BUNDLE_POLICY_OFF = 0,
	SWITCH_BUNDLE_POLICY_AUTO,
	SWITCH_BUNDLE_POLICY_FORCE
} switch_bundle_policy_t;

typedef enum {
	SWITCH_BUNDLE_STATE_NONE = 0,
	SWITCH_BUNDLE_STATE_OFFERED,
	SWITCH_BUNDLE_STATE_ACCEPTED,
	SWITCH_BUNDLE_STATE_REJECTED
} switch_bundle_state_t;

typedef struct switch_bundle_mline_s {
	int mline_index;
	switch_media_type_t media_type;
	char mid[SWITCH_BUNDLE_MAX_MID_LEN + 1];
	uint8_t in_bundle_group;
	uint8_t bundle_tag;
	uint8_t bundle_only;
	uint8_t rtcp_mux;
	uint8_t rtcp_mux_only;
	uint8_t rejected;
	uint8_t zero_port;
	uint8_t local_mid_ext_id;
	uint8_t remote_mid_ext_id;
	switch_payload_t payloads[SWITCH_BUNDLE_MAX_PAYLOADS];
	uint32_t payload_count;
	uint32_t local_ssrc;
	uint32_t remote_ssrc;
} switch_bundle_mline_t;

typedef struct switch_bundle_group_s {
	switch_bundle_policy_t policy;
	switch_bundle_state_t state;
	uint32_t generation;
	char offered_mids[SWITCH_BUNDLE_MAX_MIDS][SWITCH_BUNDLE_MAX_MID_LEN + 1];
	uint32_t offered_mid_count;
	int bundle_tag_mline_index;
	char bundle_tag_mid[SWITCH_BUNDLE_MAX_MID_LEN + 1];
	switch_bundle_mline_t mlines[SWITCH_BUNDLE_MAX_MLINES];
	uint32_t mline_count;
	char reject_reason[SWITCH_BUNDLE_REJECT_REASON_LEN];
} switch_bundle_group_t;

SWITCH_DECLARE(switch_bundle_policy_t) switch_bundle_policy_parse(const char *value, switch_bundle_policy_t dft);
SWITCH_DECLARE(const char *) switch_bundle_policy_str(switch_bundle_policy_t policy);
SWITCH_DECLARE(const char *) switch_bundle_state_str(switch_bundle_state_t state);
SWITCH_DECLARE(void) switch_bundle_group_init(switch_bundle_group_t *group, switch_bundle_policy_t policy);
SWITCH_DECLARE(void) switch_bundle_group_reset(switch_bundle_group_t *group);
SWITCH_DECLARE(switch_status_t) switch_bundle_group_set_offered_mids(switch_bundle_group_t *group, const char *value);
SWITCH_DECLARE(switch_bundle_mline_t *) switch_bundle_group_add_mline(switch_bundle_group_t *group, int mline_index, switch_media_type_t media_type, const char *mid, switch_port_t port, switch_bool_t rtcp_mux, switch_bool_t bundle_only, switch_bool_t rtcp_mux_only);
SWITCH_DECLARE(switch_bundle_mline_t *) switch_bundle_group_find_mline_by_mid(switch_bundle_group_t *group, const char *mid);
SWITCH_DECLARE(switch_bundle_mline_t *) switch_bundle_group_find_mline_by_index(switch_bundle_group_t *group, int mline_index);
SWITCH_DECLARE(switch_status_t) switch_bundle_mline_set_remote_mid_ext(switch_bundle_mline_t *mline, uint8_t ext_id);
SWITCH_DECLARE(switch_status_t) switch_bundle_mline_set_local_mid_ext(switch_bundle_mline_t *mline, uint8_t ext_id);
SWITCH_DECLARE(switch_status_t) switch_bundle_mline_add_payload_type(switch_bundle_mline_t *mline, switch_payload_t pt);
SWITCH_DECLARE(switch_bool_t) switch_bundle_group_payload_type_unique(switch_bundle_group_t *group, switch_payload_t pt, switch_bundle_mline_t **mline_out);
SWITCH_DECLARE(switch_status_t) switch_bundle_group_validate(switch_bundle_group_t *group);
SWITCH_DECLARE(const char *) switch_bundle_group_reject_reason(const switch_bundle_group_t *group);

SWITCH_END_EXTERN_C

#endif
