#include <switch.h>
#include <test/switch_test.h>

#define HAVE_SOFIA_SIP

#ifdef HAVE_SOFIA_SIP
#include <sofia-sip/sdp.h>
#endif

#define TEST_GLOBAL_ATTR 0

#ifdef check_eq_str
#undef check_eq_str
#endif

#ifndef _check_eq_str
#define _check_eq_str(got, exp, what)                                            \
    do {                                                                        \
        const char *___g = (got) ? (got) : "";                                  \
        const char *___e = (exp) ? (exp) : "";                                  \
        if (strcmp(___g, ___e) != 0) {                                          \
            fprintf(stderr, "FAIL %s: got=\"%s\" exp=\"%s\"\n", (what), ___g, ___e); \
        }                                                                        \
    } while (0)
#endif


#define check_proto_ok(got,label) do {                              \
    const char *p = (got) ? (got) : "";                              \
    if (strcmp(p,"UDP/TLS/RTP/SAVPF") && strcmp(p,"RTP/AVP")) {      \
        fprintf(stderr,"FAIL %s: proto=\"%s\" (expected UDP/TLS/RTP/SAVPF or RTP/AVP)\n", \
                (label), p);                                         \
    }                                                                \
} while (0)

static const char *rx_host = "127.0.0.1";

static unsigned int count_occurrences(const char *haystack, const char *needle)
{
	unsigned int count = 0;
	size_t needle_len;

	if (!haystack || !needle || !*needle) {
		return 0;
	}

	needle_len = strlen(needle);
	while ((haystack = strstr(haystack, needle))) {
		count++;
		haystack += needle_len;
	}

	return count;
}

typedef struct sdp_candidate_s {
    char foundation[32];
    char transport[8];     /* "udp", "tcp" */
    char type[8];          /* "host","srflx","relay","prflx" */
    char ip[64];
    uint16_t port;
    uint32_t priority;
    uint8_t  component_id; /* 1=RTP, 2=RTCP */
    char     raddr[64];
    uint16_t rport;
    char     tcp_type[8];  /* "active","passive","so" if tcp */
    char     mid[16];
    int      mid_idx;      /* -1 if unknown */
} sdp_candidate_t;

#ifndef HAVE_SOFIA_SIP
typedef struct sdp_media_s { /* minimal stub if you want to “compile & skip” without Sofia */ } sdp_media_t;
typedef struct sdp_media_s {
    char    mid[16];
    int     mid_idx;       /* -1 if not present */
    int     port;
    char    proto[16];     /* RTP/AVP, RTP/SAVP, UDP/TLS/RTP/SAVPF, etc. */
    int     rtcp_mux;      /* boolean */
    int     rtcp_rsize;    /* boolean */
    int     end_of_candidates; /* boolean */
    uint32_t cand_count;
    sdp_candidate_t cands[64]; 
} sdp_media_t;
#endif

static void check_eq_str(const char *got, const char *exp, const char *label)
{
    switch_assert(got && exp && !strcmp(got, exp));
}

static void check_bool(int got, int exp, const char *label)
{
    switch_assert(got == exp);
}

static void check_eq_u32(uint32_t got, uint32_t exp, const char *label)
{
    switch_assert(got == exp);
}

#define MAX_M 8
#define MAX_CANDS 32
#define MAX_STR 64

typedef struct switch_sdp_cand_s {
    char     foundation[32];
    uint8_t  component_id;     /* <-- used in tests */
    char     transport[12];    /* "udp"/"tcp" */
    uint32_t priority;
    char     ip[64];
	uint16_t port;
    char     type[12];         /* "host"/"srflx"/"relay" */
    char     raddr[64];
    uint16_t rport;
    char     tcp_type[16];     /* "active"/"passive"/"so" if tcp */
} switch_sdp_cand_t;

typedef struct switch_sdp_candidate_s {
    char     foundation[32];
    char     transport[12];   /* "udp" / "tcp" */
    char     tcp_type[16];    /* "active"/"passive"/"so" when tcp */
    char     ip[64];
    uint16_t port;

    char     type[12];        /* "host"/"srflx"/"relay" */
	char     raddr[64];
    uint16_t rport;
} switch_sdp_candidate_t;

typedef struct switch_sdp_m_s {
    char     proto[32];       /* e.g., "UDP/TLS/RTP/SAVPF" */
    char     mid[16];         /* e.g., "0", "1" */
    int      mid_idx;         /* parsed numeric from mid when possible */
    int      rtcp_mux;        /* bool */
	int      end_of_candidates; /* bool */
    uint32_t cand_count;
    switch_sdp_cand_t cands[32];
} switch_sdp_m_t;

typedef struct switch_sdp_info_s {
    char     ice_ufrag[64];
    char     ice_pwd[128];
    uint32_t m_count;
    switch_sdp_m_t m[8];      /* parsed media blocks */
	 char bundle_group[64];
	  char group[64];
} switch_sdp_info_t;

static void str_cp(char *dst, const char *src, size_t cap) {
    size_t i=0; if (!dst || !cap) return; if (!src) { dst[0]='\0'; return; }
    for (; src[i] && i+1<cap; ++i) dst[i]=src[i]; dst[i]='\0';
}
static void tolower_cp(char *dst, const char *src, size_t cap) {
    size_t i=0; if (!dst||!cap) return; if (!src){ dst[0]='\0'; return; }
    for (; src[i] && i+1<cap; ++i) { char c=src[i]; if (c>='A'&&c<='Z') c=(char)(c-'A'+'a'); dst[i]=c; }
    dst[i]='\0';
}
static const char *attr_val(const sdp_attribute_t *a, const char *name) {
    for (; a; a=a->a_next) if (a->a_name && strcmp(a->a_name,name)==0) return a->a_value?a->a_value:"";
    return "";
}
static int has_attr(const sdp_attribute_t *a, const char *name) {
    for (; a; a=a->a_next) if (a->a_name && strcmp(a->a_name,name)==0) return 1;
    return 0;
}
static int mid_index_from(const char *mid) {
    long v; char *endp = NULL; if (!mid || !*mid) return -1;
    v = strtol(mid,&endp,10); return (endp && *endp=='\0' && v>=0 && v<=0x7fffffffL) ? (int)v : -1;
}
static int parse_cand(const char *line, switch_sdp_cand_t *out) {
    char buf[1024], *save=NULL, *tok=NULL; int saw_typ=0;
    if (!line||!out) return 0; memset(out,0,sizeof(*out)); str_cp(buf,line,sizeof(buf));
    tok=strtok_r(buf," ",&save); if(!tok) return 0; str_cp(out->foundation,tok,sizeof(out->foundation));
    tok=strtok_r(NULL," ",&save); if(!tok) return 0; out->component_id=(uint32_t)strtoul(tok,NULL,10);
    tok=strtok_r(NULL," ",&save); if(!tok) return 0; tolower_cp(out->transport,tok,sizeof(out->transport));
    tok=strtok_r(NULL," ",&save); if(!tok) return 0; /* priority skip */
    tok=strtok_r(NULL," ",&save); if(!tok) return 0; str_cp(out->ip,tok,sizeof(out->ip));
    tok=strtok_r(NULL," ",&save); if(!tok) return 0; out->port=(uint32_t)strtoul(tok,NULL,10);
    while ((tok=strtok_r(NULL," ",&save))) {
        if (!strcmp(tok,"typ")) { tok=strtok_r(NULL," ",&save); if(!tok)break; tolower_cp(out->type,tok,sizeof(out->type)); saw_typ=1; }
        else if (!strcmp(tok,"raddr")) { tok=strtok_r(NULL," ",&save); if(!tok)break; str_cp(out->raddr,tok,sizeof(out->raddr)); }
        else if (!strcmp(tok,"rport")) { tok=strtok_r(NULL," ",&save); if(!tok)break; out->rport=(uint32_t)strtoul(tok,NULL,10); }
        else if (!strcmp(tok,"tcptype")) { tok=strtok_r(NULL," ",&save); if(!tok)break; tolower_cp(out->tcp_type,tok,sizeof(out->tcp_type)); }
        else if (!strcmp(tok,"generation")) { (void)strtok_r(NULL," ",&save); }
        else if (!strcmp(tok,"network-cost")) { (void)strtok_r(NULL," ",&save); }
        else { (void)strtok_r(NULL," ",&save); }
    }
    return saw_typ;
}

static void str_cp(char *dst, const char *src, size_t cap);
static int has_attr(const sdp_attribute_t *a, const char *name);
static const char *attr_val(const sdp_attribute_t *a, const char *name);
static int mid_index_from(const char *mid);
static int parse_cand(const char *line, switch_sdp_cand_t *out);

static void do_remote_sdp(switch_core_session_t *s, const char *sdp)
{
	switch_channel_t *ch = switch_core_session_get_channel(s);
	if (!ch || !sdp || !*sdp) return;
	switch_channel_set_variable(ch, "sip_remote_sdp_str", sdp);
	switch_channel_set_variable(ch, "remote_sdp_str", sdp);
	switch_channel_set_variable(ch, "remote_sdp", sdp);
}

static switch_status_t collect_remote_sdp_info_from_session(switch_core_session_t *session,
                                                            switch_sdp_info_t *out)
{
    switch_channel_t *channel;
    const char *r_sdp;
    sdp_parser_t *parser = NULL;
    sdp_session_t *sess;
    sdp_media_t *sm;
    sdp_attribute_t *a;
    const char *val;
    char *norm = NULL;

    if (!session || !out) {
        return SWITCH_STATUS_FALSE;
    }
    memset(out, 0, sizeof(*out));

    channel = switch_core_session_get_channel(session);
    if (!channel) {
        return SWITCH_STATUS_FALSE;
    }

    r_sdp = switch_channel_get_variable(channel, "sip_remote_sdp_str");
    if (!r_sdp || !*r_sdp) r_sdp = switch_channel_get_variable(channel, "remote_sdp_str");
    if (!r_sdp || !*r_sdp) r_sdp = switch_channel_get_variable(channel, "remote_sdp");
	if (!r_sdp || !*r_sdp) r_sdp = switch_channel_get_variable(channel, "sip_remote_sdp");
	if (!r_sdp || !*r_sdp) r_sdp = switch_channel_get_variable(channel, "remote_sdp");

	if (!r_sdp || !*r_sdp) {
		const char *peer_uuid = switch_channel_get_partner_uuid(channel);
        if (peer_uuid && *peer_uuid) {
            switch_core_session_t *peer = switch_core_session_locate(peer_uuid);
            if (peer) {
                switch_channel_t *pchan = switch_core_session_get_channel(peer);
                if (pchan) {
                    const char *tmp = switch_channel_get_variable(pchan, "sip_local_sdp_str");
                    if (!tmp || !*tmp) tmp = switch_channel_get_variable(pchan, "local_sdp_str");
                    if (tmp && *tmp) {
                        r_sdp = switch_core_session_strdup(session, tmp);
                    }
                }
                switch_core_session_rwunlock(peer);
            }
        }
    }

    if (!r_sdp || !*r_sdp) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                          "No remote SDP found on channel or partner; cannot parse.\n");
        return SWITCH_STATUS_FALSE;
    }

    r_sdp = switch_channel_get_variable(channel, "sip_remote_sdp_str");
    if (!r_sdp || !*r_sdp) {
        r_sdp = switch_channel_get_variable(channel, "remote_sdp_str");
    }
    if (!r_sdp || !*r_sdp) {
        return SWITCH_STATUS_FALSE;
    }

    {
        size_t n = strlen(r_sdp), j = 0;
        norm = (char *)switch_core_alloc(switch_core_session_get_pool(session), n * 2 + 4);
        for (size_t i = 0; i < n; ++i) {
            if (r_sdp[i] == '\n') {
                if (i && r_sdp[i-1] != '\r') norm[j++] = '\r';
                norm[j++] = '\n';
            } else {
                norm[j++] = r_sdp[i];
            }
        }
        norm[j] = '\0';
    }

    parser = sdp_parse(NULL, norm, (int)strlen(norm), 0);
    
    if (!parser) {
        return SWITCH_STATUS_FALSE;
    }
    sess = sdp_session(parser);
    if (!sess) {
        sdp_parser_free(parser);
        return SWITCH_STATUS_FALSE;
    }

    val = attr_val(sess->sdp_attributes, "ice-ufrag");
    str_cp(out->ice_ufrag, val, sizeof(out->ice_ufrag));
    val = attr_val(sess->sdp_attributes, "ice-pwd");
    str_cp(out->ice_pwd, val, sizeof(out->ice_pwd));
    val = attr_val(sess->sdp_attributes, "group");
    str_cp(out->group, val, sizeof(out->group));

    out->m_count = 0;

    for (sm = sess->sdp_media; sm && out->m_count < MAX_M; sm = sm->m_next) {
        switch_sdp_m_t *mv = &out->m[out->m_count];
        const char *mid_val;

        memset(mv, 0, sizeof(*mv));

        if (sm->m_proto_name) {
            str_cp(mv->proto, sm->m_proto_name, sizeof(mv->proto));
        } else {
            str_cp(mv->proto, "RTP/AVP", sizeof(mv->proto));
        }

        mv->rtcp_mux = has_attr(sm->m_attributes, "rtcp-mux");
        mv->end_of_candidates = has_attr(sm->m_attributes, "end-of-candidates");

        mid_val = attr_val(sm->m_attributes, "mid");
        str_cp(mv->mid, mid_val, sizeof(mv->mid));
        mv->mid_idx = mid_index_from(mid_val);

        mv->cand_count = 0;
        for (a = sm->m_attributes; a; a = a->a_next) {
            if (a->a_name && strcmp(a->a_name, "candidate") == 0 && a->a_value) {
                if (mv->cand_count < MAX_CANDS) {
                    switch_sdp_cand_t tmp;
                    if (parse_cand(a->a_value, &tmp)) {
                        mv->cands[mv->cand_count++] = tmp;
                    }
                }
            }
        }

        out->m_count++;
    }

	if (!out->bundle_group[0] && out->m_count > 0) {
		size_t off = 0, cap = sizeof(out->bundle_group);
		size_t need;
		for (uint32_t i = 0; i < out->m_count; i++) {
			const char *tok = out->m[i].mid[0] ? out->m[i].mid : NULL;
			if (!tok) continue;
			need = strlen(tok) + (off ? 1 : 0) + 1;
			if (off + need > cap) break;
			if (off) out->bundle_group[off++] = ' ';
			for (const char *s = tok; *s && off+1 < cap; ++s) {
				out->bundle_group[off++] = *s;
			}
			out->bundle_group[off] = '\0';
		}
	}
    
	sdp_parser_free(parser);
	return SWITCH_STATUS_SUCCESS;
}

FST_CORE_BEGIN("./conf_sdp")
{
	FST_SUITE_BEGIN(switch_sdp)
	{
		FST_SETUP_BEGIN()
		{
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		FST_SESSION_BEGIN(sdp_ice_and_call_direction)
		{
			switch_status_t status;
			switch_media_handle_t *media_handle;
			switch_core_media_params_t *mparams;
			char *r_sdp;
			uint8_t match = 0, p = 0;
			const char *audio_flow_txt;
			switch_media_flow_t audio_flow;

			mparams  = switch_core_session_alloc(fst_session, sizeof(switch_core_media_params_t));
			mparams->inbound_codec_string = switch_core_session_strdup(fst_session, "pcmu,opus");
			mparams->outbound_codec_string = switch_core_session_strdup(fst_session, "pcmu,opus");
			mparams->rtpip = switch_core_session_strdup(fst_session, (char *)rx_host);

			status = switch_media_handle_create(&media_handle, fst_session, mparams);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			r_sdp = switch_core_session_sprintf(fst_session,
			"v=0\n"
			"o=- 1683118194 1683118195 IN IP4 0.0.0.0\n"
			"s=-\n"
#if TEST_GLOBAL_ATTR
			"a=sendonly\n"
#endif 
			"t=0 0\n"
			"m=audio 9 UDP/TLS/RTP/SAVPF 102\n"
			"c=IN IP4 0.0.0.0\n"
			"a=ice-ufrag:aZJpsl00bYnjrOZtkCFMtKhFC/CHAfcv\n"
			"a=ice-pwd:aNniSnLLp43SSsJrz6TNPty1zPrxZNzh\n"
			"a=rtcp-mux\n"
			"a=setup:active\n"
			"a=rtpmap:102 OPUS/48000/2\n"
			"a=fmtp:102 useinbandfec=1;maxaveragebitrate=30000;maxplaybackrate=48000;ptime=20;minptime=10;maxptime=40;sprop-stereo=0;sprop-maxcapturerate=48000\n"
			"a=ssrc:2588681350 msid:user199999@host-a1132918 webrtctransceiver0\n"
			"a=ssrc:2588681350 cname:user19999@host-a1132918\n"
			"a=sendrecv\n"
			"a=fingerprint:sha-256 17:B5:C8:7F:AE:D0:32:C9:FF:58:80:3C:17:5A:45:2E:55:2D:D9:33:DD:2A:56:16:7D:AC:3B:3C:76:80:0C:D4\n");

			status = switch_core_media_prepare_codecs(fst_session, SWITCH_FALSE);
			switch_assert(status == SWITCH_STATUS_SUCCESS);
			   
			match = switch_core_media_negotiate_sdp(fst_session, r_sdp, &p, SDP_ANSWER);
			switch_assert(match != 1); /*should fail because of lack of ICE candidate */

			audio_flow_txt = switch_channel_get_variable(fst_channel, "audio_media_flow");
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "audio_flow: %s\n", audio_flow_txt);

			audio_flow = switch_core_session_media_flow(fst_session, SWITCH_MEDIA_TYPE_AUDIO);
			fst_check(SWITCH_MEDIA_FLOW_DISABLED == audio_flow);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(sdp_offer_with_candidates)
		{
			switch_status_t status;
			switch_media_handle_t *media_handle;
			switch_core_media_params_t *mparams;
			char *r_sdp;
			uint8_t match = 0, p = 0;

			mparams  = switch_core_session_alloc(fst_session, sizeof(switch_core_media_params_t));
			mparams->inbound_codec_string = switch_core_session_strdup(fst_session, "pcmu,opus");
			mparams->outbound_codec_string = switch_core_session_strdup(fst_session, "pcmu,opus");
			mparams->rtpip = switch_core_session_strdup(fst_session, (char *)rx_host);

			status = switch_media_handle_create(&media_handle, fst_session, mparams);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			r_sdp = switch_core_session_sprintf(fst_session,
			"v=0\n"
			"o=odun31589 3314 2371 IN IP4 192.168.66.200\n"
			"s=Talk\n"
			"c=IN IP4 192.168.66.200\n"
			"t=0 0\n"
			"a=rtcp-xr:rcvr-rtt=all:10000 stat-summary=loss,dup,jitt,TTL voip-metrics\n"
			"a=group:BUNDLE as\n"
			"a=record:off\n"
			"m=audio 54498 RTP/SAVPF 96 0 8 9 101 97\n"
			"a=rtpmap:96 opus/48000/2\n"
			"a=fmtp:96 useinbandfec=1\n"
			"a=rtpmap:101 telephone-event/48000\n"
			"a=rtpmap:97 telephone-event/8000\n"
			"a=crypto:1 AEAD_AES_128_GCM inline:dw2xByaXxPGC8P2NgiXYmmYTidAz5MxdYnYRcw==\n"
			"a=crypto:2 AES_CM_128_HMAC_SHA1_80 inline:fEy5z6SJBbKWlOs+1fu3IYxnOy9Y+IWgBkDonyB+\n"
			"a=crypto:3 AEAD_AES_256_GCM inline:qBJdNu1MtkZr3MAANsH2xqsJI5i0cY0i7kjNGx4tZlLwXa4FbeJnuWsv38I=\n"
			"a=crypto:4 AES_256_CM_HMAC_SHA1_80 inline:vhUUYaCSeNcoUIHYntFLvLaR79Ej7fOFemDUQ4WQU761aFIdyz+7VWdD8pyCRw==\n"
			"a=rtcp-mux\n"
			"a=mid:as\n"
			"a=extmap:1 urn:ietf:params:rtp-hdrext:sdes:mid\n"
			"a=rtcp:60128\n"
			"a=rtcp-fb:* trr-int 1000\n"
			"a=rtcp-fb:* ccm tmmbr\n");

			status = switch_core_media_prepare_codecs(fst_session, SWITCH_FALSE);
			switch_assert(status == SWITCH_STATUS_SUCCESS);
			   
			match = switch_core_media_negotiate_sdp(fst_session, r_sdp, &p, SDP_OFFER);
			switch_assert(match); 
		}
		FST_SESSION_END()
		FST_SESSION_BEGIN(sdp_bundle_offer_emits_audio_and_video_mid)
		{
			switch_status_t status;
			switch_media_handle_t *media_handle;
			switch_core_media_params_t *mparams;
			const char *s;

			mparams  = switch_core_session_alloc(fst_session, sizeof(switch_core_media_params_t));
			mparams->inbound_codec_string = switch_core_session_strdup(fst_session, "PCMU,VP8");
			mparams->outbound_codec_string = switch_core_session_strdup(fst_session, "PCMU,VP8");
			mparams->rtpip = switch_core_session_strdup(fst_session, (char *)rx_host);

			status = switch_media_handle_create(&media_handle, fst_session, mparams);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			status = switch_core_media_prepare_codecs(fst_session, SWITCH_FALSE);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			status = switch_core_media_choose_ports(fst_session, SWITCH_TRUE, SWITCH_TRUE);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			switch_channel_set_flag(fst_channel, CF_VIDEO);
			switch_channel_set_variable(fst_channel, "rtp_use_bundle", "true");
			switch_channel_set_variable(fst_channel, "rtp_audio_mid", "0");
			switch_channel_set_variable(fst_channel, "rtp_video_mid", "1");
			switch_channel_set_variable(fst_channel, "rtp_no_audio_mid", "false");
			switch_channel_set_variable(fst_channel, "rtp_no_video_mid", "false");
			switch_channel_set_variable(fst_channel, "rtp_no_attr_mid", "false");

			switch_core_media_gen_local_sdp(fst_session, SDP_OFFER, "127.0.0.1", 40000, NULL, 1);
			s = switch_channel_get_variable(fst_channel, "rtp_local_sdp_str");
			fst_requires(s != NULL);
			fst_check(strstr(s, "a=group:BUNDLE 0 1") != NULL);
			fst_check(strstr(s, "a=mid:0") != NULL);
			fst_check(strstr(s, "a=mid:1") != NULL);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(sdp_bundle_offer_emits_audio_mid_once_for_multiple_audio_mlines)
		{
			switch_status_t status;
			switch_media_handle_t *media_handle;
			switch_core_media_params_t *mparams;
			const char *s;

			mparams = switch_core_session_alloc(fst_session, sizeof(switch_core_media_params_t));
			mparams->inbound_codec_string = switch_core_session_strdup(fst_session, "PCMU@20i,PCMU@30i,VP8");
			mparams->outbound_codec_string = switch_core_session_strdup(fst_session, "PCMU@20i,PCMU@30i,VP8");
			mparams->rtpip = switch_core_session_strdup(fst_session, (char *)rx_host);

			status = switch_media_handle_create(&media_handle, fst_session, mparams);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			status = switch_core_media_prepare_codecs(fst_session, SWITCH_FALSE);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			status = switch_core_media_choose_ports(fst_session, SWITCH_TRUE, SWITCH_TRUE);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			switch_channel_set_flag(fst_channel, CF_VIDEO);
			switch_channel_set_variable(fst_channel, "rtp-bundle", "auto");
			switch_channel_set_variable(fst_channel, "rtp_audio_mid", "0");
			switch_channel_set_variable(fst_channel, "rtp_video_mid", "1");

			switch_core_media_gen_local_sdp(fst_session, SDP_OFFER, "127.0.0.1", 40000, NULL, 1);
			s = switch_channel_get_variable(fst_channel, "rtp_local_sdp_str");
			fst_requires(s != NULL);
			fst_check(count_occurrences(s, "m=audio ") > 1);
			fst_check(count_occurrences(s, "a=mid:0\r\n") == 1);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(sdp_bundle_offer_empty_mid_vars_use_defaults)
		{
			switch_status_t status;
			switch_media_handle_t *media_handle;
			switch_core_media_params_t *mparams;
			const char *s;

			mparams  = switch_core_session_alloc(fst_session, sizeof(switch_core_media_params_t));
			mparams->inbound_codec_string = switch_core_session_strdup(fst_session, "PCMU,VP8");
			mparams->outbound_codec_string = switch_core_session_strdup(fst_session, "PCMU,VP8");
			mparams->rtpip = switch_core_session_strdup(fst_session, (char *)rx_host);

			status = switch_media_handle_create(&media_handle, fst_session, mparams);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			status = switch_core_media_prepare_codecs(fst_session, SWITCH_FALSE);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			status = switch_core_media_choose_ports(fst_session, SWITCH_TRUE, SWITCH_TRUE);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			switch_channel_set_flag(fst_channel, CF_VIDEO);
			switch_channel_set_variable(fst_channel, "rtp_use_bundle", "true");
			switch_channel_set_variable(fst_channel, "rtp_audio_mid", "");
			switch_channel_set_variable(fst_channel, "rtp_video_mid", "");
			switch_channel_set_variable(fst_channel, "rtp_no_audio_mid", "false");
			switch_channel_set_variable(fst_channel, "rtp_no_video_mid", "false");
			switch_channel_set_variable(fst_channel, "rtp_no_attr_mid", "false");

			switch_core_media_gen_local_sdp(fst_session, SDP_OFFER, "127.0.0.1", 40000, NULL, 1);
			s = switch_channel_get_variable(fst_channel, "rtp_local_sdp_str");
			fst_requires(s != NULL);
			fst_check(strstr(s, "a=group:BUNDLE audio video") != NULL);
			fst_check(strstr(s, "a=mid:audio") != NULL);
			fst_check(strstr(s, "a=mid:video") != NULL);
			fst_check(strstr(s, "a=mid:\r\n") == NULL);
		}
		FST_SESSION_END()

		FST_SESSION_BEGIN(sdp_trickle_ice)
		{
		switch_sdp_info_t info;
		uint8_t match = 0, p = 0;

        /* ---- Test 1: Basic ICE + BUNDLE + rtcp-mux ---- */
        const char *sdp1 =
            "v=0\r\n"
            "o=- 12345 2 IN IP4 198.51.100.1\r\n"
            "s=-\r\n"
            "t=0 0\r\n"
            "a=group:BUNDLE 0 1\r\n"
            "a=ice-options:trickle\r\n"
            "a=ice-ufrag:abc123\r\n"
            "a=ice-pwd:deadbeefcafebabef00dface\r\n"
            "m=audio 49170 UDP/TLS/RTP/SAVPF 96 0 8\r\n"
            "c=IN IP4 198.51.100.1\r\n"
            "a=rtcp-mux\r\n"
            "a=mid:0\r\n"
            "a=candidate:1 1 udp 2122260223 10.0.0.5 60799 typ host\r\n"
            "a=candidate:2 1 udp 1686052607 203.0.113.10 53457 typ srflx raddr 10.0.0.5 rport 60799\r\n"
            "a=end-of-candidates\r\n"
            "m=video 9 UDP/TLS/RTP/SAVPF 100 101\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "a=rtcp-mux\r\n"
            "a=mid:1\r\n";

		switch_status_t st;

        memset(&info, 0, sizeof(info));
		match = switch_core_media_negotiate_sdp(fst_session, sdp1, &p, SDP_OFFER);
		switch_assert(match != 1);
		
		switch_channel_set_variable(switch_core_session_get_channel(fst_session), "sip_remote_sdp_str", sdp1);
		do_remote_sdp(fst_session, sdp1);
		st = collect_remote_sdp_info_from_session(fst_session, &info);
		switch_assert(st == SWITCH_STATUS_SUCCESS);
        
		_check_eq_str(info.ice_ufrag, "abc123", "ice-ufrag");
        _check_eq_str(info.ice_pwd, "deadbeefcafebabef00dface", "ice-pwd");
        _check_eq_str(info.bundle_group, "0 1", "group:BUNDLE");
        check_eq_u32(info.m_count, 2, "m_count");

        /* audio m-line */
        {
            switch_sdp_m_t *ma = &info.m[0];
            check_eq_str(ma->proto, "UDP/TLS/RTP/SAVPF", "audio proto");
            check_bool(ma->rtcp_mux, 1, "audio rtcp-mux");
            check_bool(ma->end_of_candidates, 1, "audio end-of-candidates");
            check_eq_str(ma->mid, "0", "audio mid");
            fst_xcheck(ma->mid_idx == 0, "audio mid_idx==0");
            check_eq_u32(ma->cand_count, 2, "audio cand_count");

            /* host candidate */
            {
                switch_sdp_cand_t *c = &ma->cands[0];
                check_eq_str(c->foundation, "1", "cand0 foundation");
                check_eq_str(c->transport, "udp", "cand0 transport");
                check_eq_str(c->type, "host", "cand0 type");
                check_eq_str(c->ip, "10.0.0.5", "cand0 ip");
                fst_xcheck(c->port == 60799, "cand0 port");
                fst_xcheck(c->component_id == 1, "cand0 comp_id==1");
            }
            /* srflx candidate */
            {
                switch_sdp_cand_t *c = &ma->cands[1];
                check_eq_str(c->type, "srflx", "cand1 type");
                check_eq_str(c->ip, "203.0.113.10", "cand1 ip");
                fst_xcheck(c->port == 53457, "cand1 port");
                check_eq_str(c->raddr, "10.0.0.5", "cand1 raddr");
                fst_xcheck(c->rport == 60799, "cand1 rport");
            }
        }

        /* video m-line */
        {
            switch_sdp_m_t *mv = &info.m[1];
            check_eq_str(mv->proto, "UDP/TLS/RTP/SAVPF", "video proto");
            check_bool(mv->rtcp_mux, 1, "video rtcp-mux");
            check_eq_str(mv->mid, "1", "video mid");
            fst_xcheck(mv->mid_idx == 1, "video mid_idx==1");
            check_eq_u32(mv->cand_count, 0, "video cand_count");
        }

        /* ---- Test 2: TCP candidates (active/passive/so) + IPv6 ---- */
		{
		uint8_t match = 0, p = 0;
		const char *sdp2 =
            "v=0\r\n"
            "o=- 1 1 IN IP6 2001:db8::1\r\n"
            "s=-\r\n"
            "t=0 0\r\n"
            "a=ice-ufrag:u6\r\n"
            "a=ice-pwd:p6\r\n"
            "m=audio 9 TCP/TLS/RTP/SAVPF 111\r\n"
            "c=IN IP6 2001:db8::1\r\n"
            "a=mid:0\r\n"
            "a=candidate:9 1 tcp 2122260223 2001:db8::2 9 typ host tcptype passive\r\n"
            "a=candidate:10 1 tcp 2122260222 2001:db8::3 9 typ host tcptype active\r\n"
            "a=candidate:11 1 tcp 2122260221 2001:db8::4 9 typ host tcptype so\r\n";

        memset(&info, 0, sizeof(info));
		match = switch_core_media_negotiate_sdp(fst_session, sdp2, &p, SDP_OFFER);
		switch_assert(match != 1);
		switch_channel_set_variable(switch_core_session_get_channel(fst_session), "sip_remote_sdp_str", sdp2);
		st = collect_remote_sdp_info_from_session(fst_session, &info);
		switch_assert(st == SWITCH_STATUS_SUCCESS);
 

        check_eq_u32(info.m_count, 1, "m_count sdp2");
        	{
            switch_sdp_m_t *m = &info.m[0];
            check_eq_str(m->proto, "TCP/TLS/RTP/SAVPF", "proto sdp2");
            check_eq_u32(m->cand_count, 3, "cand_count sdp2");

            check_eq_str(m->cands[0].transport, "tcp", "tcp cand0 transport");
            check_eq_str(m->cands[0].tcp_type, "passive", "tcp cand0 tcptype");
            check_eq_str(m->cands[1].tcp_type, "active", "tcp cand1 tcptype");
            check_eq_str(m->cands[2].tcp_type, "so", "tcp cand2 tcptype");

            /* IPv6 preserved */
            check_eq_str(m->cands[0].ip, "2001:db8::2", "cand0 ip v6");
        	}
		}

		{
		uint8_t match = 0, p = 0;

		/* ---- Test 3: malformed lines tolerated, duplicates coalesced ---- */
        const char *sdp3 =
            "v=0\r\n"
            "o=- 1 2 IN IP4 192.0.2.1\r\n"
            "s=-\r\n"
            "t=0 0\r\n"
            "a=ice-ufrag:u\r\n"
            "a=ice-pwd:p\r\n"
            "m=audio 40000 RTP/AVP 0\r\n"
            "c=IN IP4 192.0.2.1\r\n"
            "a=mid:0\r\n"
            /* malformed (missing tokens) -> ignored */
            "a=candidate:badline\r\n"
            /* legit */
            "a=candidate:1 1 udp 2113939711 192.0.2.10 55555 typ host\r\n"
            /* duplicate same foundation/addr/port/type -> should coalesce (remain 1) */
            "a=candidate:1 1 udp 2113939711 192.0.2.10 55555 typ host\r\n";

        memset(&info, 0, sizeof(info));
  		match = switch_core_media_negotiate_sdp(fst_session, sdp3, &p, SDP_OFFER);
		switch_assert(match != 1);
		switch_channel_set_variable(switch_core_session_get_channel(fst_session), "sip_remote_sdp_str", sdp3);
		st = collect_remote_sdp_info_from_session(fst_session, &info);
		switch_assert(st == SWITCH_STATUS_SUCCESS);
 
        check_eq_u32(info.m_count, 1, "m_count sdp3");
        	{
            switch_sdp_m_t *m = &info.m[0];
            /* either 1 (coalesced) or 2 (kept) — assert >=1 to allow configurability,
               then explicitly check first entry correctness */
            fst_xcheck(m->cand_count >= 1, "at least one valid candidate retained");
            check_eq_str(m->cands[0].foundation, "1", "dup foundation");
            check_eq_str(m->cands[0].ip, "192.0.2.10", "dup ip");
            fst_xcheck(m->cands[0].port == 55555, "dup port");
            check_eq_str(m->cands[0].type, "host", "dup type");
        	}
		}
		{
		uint8_t match = 0, p = 0;

			/* ---- Test 4: srflx with raddr/rport missing (should default empty/0) ---- */
        const char *sdp4 =
            "v=0\r\n"
            "o=- 1 1 IN IP4 203.0.113.1\r\n"
            "s=-\r\n"
            "t=0 0\r\n"
            "c=IN IP4 203.0.113.1\r\n"
            "m=audio 49170 RTP/SAVPF 0\r\n"
            "a=ice-ufrag:x\r\n"
            "a=ice-pwd:y\r\n"
            "a=mid:0\r\n"
            "a=candidate:2 1 udp 16777216 203.0.113.55 60000 typ srflx\r\n";

        memset(&info, 0, sizeof(info));
  		match = switch_core_media_negotiate_sdp(fst_session, sdp4, &p, SDP_OFFER);
		switch_assert(match != 1);
		switch_channel_set_variable(switch_core_session_get_channel(fst_session), "sip_remote_sdp_str", sdp4);
		st = collect_remote_sdp_info_from_session(fst_session, &info);
		switch_assert(st == SWITCH_STATUS_SUCCESS);
 
		{
        	switch_sdp_m_t *m = &info.m[0];
            check_eq_u32(m->cand_count, 1, "cand_count sdp4");
            check_eq_str(m->cands[0].type, "srflx", "sdp4 type");
            check_eq_str(m->cands[0].raddr, "", "sdp4 raddr empty");
            fst_xcheck(m->cands[0].rport == 0, "sdp4 rport 0");
        	}
		}
		{
		uint8_t match = 0, p = 0;
		/* ---- Test 5: relay (TURN) with related address ---- */
        const char *sdp5 =
            "v=0\r\n"
            "o=- 1 1 IN IP4 198.51.100.1\r\n"
            "s=-\r\n"
            "t=0 0\r\n"
            "c=IN IP4 198.51.100.1\r\n"
            "m=audio 9 RTP/SAVPF 111\r\n"
            "a=ice-ufrag:turnu\r\n"
            "a=ice-pwd:turnp\r\n"
            "a=mid:0\r\n"
            "a=candidate:rel 1 udp 123456 203.0.113.200 3478 typ relay raddr 192.0.2.44 rport 55555\r\n";

        memset(&info, 0, sizeof(info));
  		match = switch_core_media_negotiate_sdp(fst_session, sdp5, &p, SDP_OFFER);
		switch_assert(match != 1);

		switch_channel_set_variable(switch_core_session_get_channel(fst_session), "sip_remote_sdp_str", sdp5);
		st = collect_remote_sdp_info_from_session(fst_session, &info);
		switch_assert(st == SWITCH_STATUS_SUCCESS);
			{
            	switch_sdp_m_t *m = &info.m[0];
           	    check_eq_u32(m->cand_count, 1, "cand_count sdp5");
           	    check_eq_str(m->cands[0].type, "relay", "sdp5 type");
           	    check_eq_str(m->cands[0].raddr, "192.0.2.44", "sdp5 rel addr");
                fst_xcheck(m->cands[0].rport == 55555, "sdp5 rel port");
        	}
		}
		}
		FST_SESSION_END()

		/*
		 * AMR-WB registers a bandwidth efficient and an octet aligned implementation
		 * under the single iananame AMR-WB, so codec selection that compares names
		 * alone cannot tell them apart. With telnyx-strict-codec-match
		 * set, a re-INVITE must not flip between them, and the answer must keep
		 * advertising the fmtp of whichever one is in use.
		 */
		FST_SESSION_BEGIN(sdp_amrwb_strict_codec_match)
		{
			switch_status_t status;
			switch_media_handle_t *media_handle;
			switch_core_media_params_t *mparams;
			switch_codec_implementation_t impl = { 0 };
			switch_payload_t initial_ianacode;
			const char *local_sdp;
			uint8_t match = 0, p = 0;

			/* Both variants offered: PT 104 bandwidth efficient, PT 110 octet aligned. */
			const char *offer_both =
				"v=0\r\n"
				"o=- 1 1 IN IP4 198.51.100.1\r\n"
				"s=-\r\n"
				"t=0 0\r\n"
				"m=audio 56210 RTP/AVP 104 110\r\n"
				"c=IN IP4 198.51.100.1\r\n"
				"a=rtpmap:104 AMR-WB/16000\r\n"
				"a=fmtp:104 mode-set=2\r\n"
				"a=rtpmap:110 AMR-WB/16000\r\n"
				"a=fmtp:110 octet-align=1; mode-set=2\r\n"
				"a=sendrecv\r\n";

			/*
			 * Hold re-offer: bandwidth efficient only, with a different number of fmtp
			 * fields so that fmtp_check_match() fails and a fresh payload map is built.
			 */
			const char *offer_be_only =
				"v=0\r\n"
				"o=- 1 2 IN IP4 198.51.100.1\r\n"
				"s=-\r\n"
				"t=0 0\r\n"
				"m=audio 56210 RTP/AVP 104\r\n"
				"c=IN IP4 198.51.100.1\r\n"
				"a=rtpmap:104 AMR-WB/16000\r\n"
				"a=fmtp:104 mode-set=2; mode-change-capability=2; max-red=0\r\n"
				"a=sendrecv\r\n";

			/* Re-offer carrying only the octet aligned variant. */
			const char *offer_oa_only =
				"v=0\r\n"
				"o=- 1 3 IN IP4 198.51.100.1\r\n"
				"s=-\r\n"
				"t=0 0\r\n"
				"m=audio 56210 RTP/AVP 110\r\n"
				"c=IN IP4 198.51.100.1\r\n"
				"a=rtpmap:110 AMR-WB/16000\r\n"
				"a=fmtp:110 octet-align=1; mode-set=2\r\n"
				"a=sendrecv\r\n";

			switch_channel_set_variable(fst_channel, "telnyx-strict-codec-match", "true");

			mparams = switch_core_session_alloc(fst_session, sizeof(switch_core_media_params_t));
			mparams->inbound_codec_string = switch_core_session_strdup(fst_session, "AMR-WB");
			mparams->outbound_codec_string = switch_core_session_strdup(fst_session, "AMR-WB");
			mparams->rtpip = switch_core_session_strdup(fst_session, (char *)rx_host);

			status = switch_media_handle_create(&media_handle, fst_session, mparams);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			status = switch_core_media_prepare_codecs(fst_session, SWITCH_FALSE);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			/* Initial negotiation settles on one of the two implementations. */
			match = switch_core_media_negotiate_sdp(fst_session, offer_both, &p, SDP_OFFER);
			fst_requires(match == 1);
			fst_requires(switch_core_session_get_read_impl(fst_session, &impl) == SWITCH_STATUS_SUCCESS);
			fst_check(!strcasecmp(impl.iananame, "AMR-WB"));
			initial_ianacode = impl.ianacode;

			switch_core_media_gen_local_sdp(fst_session, SDP_ANSWER, rx_host, 12345, NULL, 1);
			local_sdp = switch_channel_get_variable(fst_channel, "rtp_local_sdp_str");
			fst_requires(local_sdp);
			fst_xcheck(strstr(local_sdp, "a=fmtp:") != NULL, "initial answer carries an fmtp");

			/* Re-offering the same list must not flip to the other implementation. */
			match = switch_core_media_negotiate_sdp(fst_session, offer_both, &p, SDP_OFFER);
			fst_requires(match == 1);
			fst_requires(switch_core_session_get_read_impl(fst_session, &impl) == SWITCH_STATUS_SUCCESS);
			fst_xcheck(impl.ianacode == initial_ianacode, "re-offer of the same list keeps the implementation");

			/*
			 * Same implementation, different fmtp. The payload map is rebuilt and the
			 * codec is not reset, so nothing repopulates fmtp_out; the answer used to
			 * come out with no a=fmtp line at all.
			 */
			match = switch_core_media_negotiate_sdp(fst_session, offer_be_only, &p, SDP_OFFER);
			fst_requires(match == 1);
			switch_core_media_gen_local_sdp(fst_session, SDP_ANSWER, rx_host, 12345, NULL, 1);
			local_sdp = switch_channel_get_variable(fst_channel, "rtp_local_sdp_str");
			fst_requires(local_sdp);
			fst_xcheck(strstr(local_sdp, "a=fmtp:104") != NULL, "answer keeps the fmtp when only the fmtp changed");
			fst_requires(switch_core_session_get_read_impl(fst_session, &impl) == SWITCH_STATUS_SUCCESS);
			fst_xcheck(impl.ianacode == initial_ianacode, "codec unchanged when only the fmtp changed");

			/*
			 * Only the other implementation is offered. The codec has to actually change,
			 * and the answer still has to carry an fmtp: deferring the reset to the read
			 * loop would generate the SDP before the payload map is repopulated.
			 */
			match = switch_core_media_negotiate_sdp(fst_session, offer_oa_only, &p, SDP_OFFER);
			fst_requires(match == 1);
			switch_core_media_gen_local_sdp(fst_session, SDP_ANSWER, rx_host, 12345, NULL, 1);
			local_sdp = switch_channel_get_variable(fst_channel, "rtp_local_sdp_str");
			fst_requires(local_sdp);
			fst_xcheck(strstr(local_sdp, "a=fmtp:110") != NULL, "answer carries an fmtp after switching implementation");
			fst_requires(switch_core_session_get_read_impl(fst_session, &impl) == SWITCH_STATUS_SUCCESS);
			fst_xcheck(impl.ianacode != initial_ianacode, "codec follows an offer carrying only the other implementation");
		}
		FST_SESSION_END()

		/*
		 * The hold case on its own: one re-offer of the implementation already in use,
		 * carrying a different fmtp so that a fresh payload map is allocated. Kept
		 * separate from the sequence above because a preceding implementation change
		 * forces a codec reset, which repopulates the fmtp and hides the defect.
		 */
		FST_SESSION_BEGIN(sdp_amrwb_fmtp_kept_on_reoffer)
		{
			switch_status_t status;
			switch_media_handle_t *media_handle;
			switch_core_media_params_t *mparams;
			const char *local_sdp;
			uint8_t match = 0, p = 0;

			const char *offer_both =
				"v=0\r\n"
				"o=- 2 1 IN IP4 198.51.100.1\r\n"
				"s=-\r\n"
				"t=0 0\r\n"
				"m=audio 56210 RTP/AVP 104 110\r\n"
				"c=IN IP4 198.51.100.1\r\n"
				"a=rtpmap:104 AMR-WB/16000\r\n"
				"a=fmtp:104 mode-set=2\r\n"
				"a=rtpmap:110 AMR-WB/16000\r\n"
				"a=fmtp:110 octet-align=1; mode-set=2\r\n"
				"a=sendrecv\r\n";

			/* Same payload type, three fmtp fields instead of one. */
			const char *offer_hold =
				"v=0\r\n"
				"o=- 2 2 IN IP4 198.51.100.1\r\n"
				"s=-\r\n"
				"t=0 0\r\n"
				"m=audio 56210 RTP/AVP 104\r\n"
				"c=IN IP4 198.51.100.1\r\n"
				"a=rtpmap:104 AMR-WB/16000\r\n"
				"a=fmtp:104 mode-set=2; mode-change-capability=2; max-red=0\r\n"
				"a=sendonly\r\n";

			switch_channel_set_variable(fst_channel, "telnyx-strict-codec-match", "true");

			mparams = switch_core_session_alloc(fst_session, sizeof(switch_core_media_params_t));
			mparams->inbound_codec_string = switch_core_session_strdup(fst_session, "AMR-WB");
			mparams->outbound_codec_string = switch_core_session_strdup(fst_session, "AMR-WB");
			mparams->rtpip = switch_core_session_strdup(fst_session, (char *)rx_host);

			status = switch_media_handle_create(&media_handle, fst_session, mparams);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			status = switch_core_media_prepare_codecs(fst_session, SWITCH_FALSE);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			match = switch_core_media_negotiate_sdp(fst_session, offer_both, &p, SDP_OFFER);
			fst_requires(match == 1);
			switch_core_media_gen_local_sdp(fst_session, SDP_ANSWER, rx_host, 12345, NULL, 1);
			local_sdp = switch_channel_get_variable(fst_channel, "rtp_local_sdp_str");
			fst_requires(local_sdp);
			fst_xcheck(strstr(local_sdp, "a=fmtp:104") != NULL, "initial answer carries the AMR-WB fmtp");

			/*
			 * No implementation change, so nothing resets the codec and nothing else
			 * repopulates fmtp_out on the payload map this re-offer allocates.
			 */
			match = switch_core_media_negotiate_sdp(fst_session, offer_hold, &p, SDP_OFFER);
			fst_requires(match == 1);
			switch_core_media_gen_local_sdp(fst_session, SDP_ANSWER, rx_host, 12345, NULL, 1);
			local_sdp = switch_channel_get_variable(fst_channel, "rtp_local_sdp_str");
			fst_requires(local_sdp);
			fst_xcheck(strstr(local_sdp, "a=fmtp:104") != NULL, "hold answer still carries the AMR-WB fmtp");
		}
		FST_SESSION_END()

		/*
		 * Negative control: the same offers with telnyx-strict-codec-match left unset.
		 * These assertions pin the behaviour of the unflagged path, defects included, so
		 * that a change to it has to be deliberate rather than accidental.
		 */
		FST_SESSION_BEGIN(sdp_amrwb_legacy_codec_match)
		{
			switch_status_t status;
			switch_media_handle_t *media_handle;
			switch_core_media_params_t *mparams;
			switch_codec_implementation_t impl = { 0 };
			switch_payload_t initial_ianacode;
			const char *local_sdp;
			uint8_t match = 0, p = 0;

			const char *offer_both =
				"v=0\r\n"
				"o=- 3 1 IN IP4 198.51.100.1\r\n"
				"s=-\r\n"
				"t=0 0\r\n"
				"m=audio 56210 RTP/AVP 104 110\r\n"
				"c=IN IP4 198.51.100.1\r\n"
				"a=rtpmap:104 AMR-WB/16000\r\n"
				"a=fmtp:104 mode-set=2\r\n"
				"a=rtpmap:110 AMR-WB/16000\r\n"
				"a=fmtp:110 octet-align=1; mode-set=2\r\n"
				"a=sendrecv\r\n";

			const char *offer_hold =
				"v=0\r\n"
				"o=- 3 2 IN IP4 198.51.100.1\r\n"
				"s=-\r\n"
				"t=0 0\r\n"
				"m=audio 56210 RTP/AVP 104\r\n"
				"c=IN IP4 198.51.100.1\r\n"
				"a=rtpmap:104 AMR-WB/16000\r\n"
				"a=fmtp:104 mode-set=2; mode-change-capability=2; max-red=0\r\n"
				"a=sendonly\r\n";

			mparams = switch_core_session_alloc(fst_session, sizeof(switch_core_media_params_t));
			mparams->inbound_codec_string = switch_core_session_strdup(fst_session, "AMR-WB");
			mparams->outbound_codec_string = switch_core_session_strdup(fst_session, "AMR-WB");
			mparams->rtpip = switch_core_session_strdup(fst_session, (char *)rx_host);

			status = switch_media_handle_create(&media_handle, fst_session, mparams);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			status = switch_core_media_prepare_codecs(fst_session, SWITCH_FALSE);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			match = switch_core_media_negotiate_sdp(fst_session, offer_both, &p, SDP_OFFER);
			fst_requires(match == 1);
			fst_requires(switch_core_session_get_read_impl(fst_session, &impl) == SWITCH_STATUS_SUCCESS);
			initial_ianacode = impl.ianacode;

			switch_core_media_gen_local_sdp(fst_session, SDP_ANSWER, rx_host, 12345, NULL, 1);
			local_sdp = switch_channel_get_variable(fst_channel, "rtp_local_sdp_str");
			fst_requires(local_sdp);
			fst_xcheck(strstr(local_sdp, "a=fmtp:") != NULL, "legacy: initial answer carries an fmtp");

			/* Legacy drops the fmtp when a re-offer rebuilds the payload map. */
			match = switch_core_media_negotiate_sdp(fst_session, offer_hold, &p, SDP_OFFER);
			fst_requires(match == 1);
			switch_core_media_gen_local_sdp(fst_session, SDP_ANSWER, rx_host, 12345, NULL, 1);
			local_sdp = switch_channel_get_variable(fst_channel, "rtp_local_sdp_str");
			fst_requires(local_sdp);
			fst_xcheck(strstr(local_sdp, "a=fmtp:104") == NULL, "legacy: hold answer loses the AMR-WB fmtp");

			/* Legacy matches on iananame alone, so the last implementation offered wins. */
			match = switch_core_media_negotiate_sdp(fst_session, offer_both, &p, SDP_OFFER);
			fst_requires(match == 1);
			fst_requires(switch_core_session_get_read_impl(fst_session, &impl) == SWITCH_STATUS_SUCCESS);
			fst_xcheck(impl.ianacode != initial_ianacode, "legacy: re-offer of the same list flips the implementation");
		}
		FST_SESSION_END()

		/*
		 * A re-INVITE that swaps the codec outright is not the case this flag exists for.
		 * Nothing in the answer depends on the codec having already been re-initialised,
		 * so the reset stays deferred to the read loop rather than running on the
		 * signalling thread, which is what the unflagged path does.
		 */
		FST_SESSION_BEGIN(sdp_strict_codec_match_defers_unrelated_swap)
		{
			switch_status_t status;
			switch_media_handle_t *media_handle;
			switch_core_media_params_t *mparams;
			switch_codec_implementation_t impl = { 0 };
			const char *local_sdp;
			uint8_t match = 0, p = 0;

			const char *offer_pcmu =
				"v=0\r\n"
				"o=- 4 1 IN IP4 198.51.100.1\r\n"
				"s=-\r\n"
				"t=0 0\r\n"
				"m=audio 56210 RTP/AVP 0\r\n"
				"c=IN IP4 198.51.100.1\r\n"
				"a=rtpmap:0 PCMU/8000\r\n"
				"a=sendrecv\r\n";

			const char *offer_pcma =
				"v=0\r\n"
				"o=- 4 2 IN IP4 198.51.100.1\r\n"
				"s=-\r\n"
				"t=0 0\r\n"
				"m=audio 56210 RTP/AVP 8\r\n"
				"c=IN IP4 198.51.100.1\r\n"
				"a=rtpmap:8 PCMA/8000\r\n"
				"a=sendrecv\r\n";

			switch_channel_set_variable(fst_channel, "telnyx-strict-codec-match", "true");
			/* Keep both codecs offerable across the re-negotiation. */
			switch_channel_set_variable(fst_channel, "absolute_codec_string", "PCMU,PCMA");

			mparams = switch_core_session_alloc(fst_session, sizeof(switch_core_media_params_t));
			mparams->inbound_codec_string = switch_core_session_strdup(fst_session, "PCMU,PCMA");
			mparams->outbound_codec_string = switch_core_session_strdup(fst_session, "PCMU,PCMA");
			mparams->rtpip = switch_core_session_strdup(fst_session, (char *)rx_host);

			status = switch_media_handle_create(&media_handle, fst_session, mparams);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			status = switch_core_media_prepare_codecs(fst_session, SWITCH_FALSE);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			match = switch_core_media_negotiate_sdp(fst_session, offer_pcmu, &p, SDP_OFFER);
			fst_requires(match == 1);
			fst_requires(switch_core_session_get_read_impl(fst_session, &impl) == SWITCH_STATUS_SUCCESS);
			fst_requires(!strcasecmp(impl.iananame, "PCMU"));

			match = switch_core_media_negotiate_sdp(fst_session, offer_pcma, &p, SDP_OFFER);
			fst_xcheck(match == 1, "an unrelated codec swap still negotiates");

			switch_core_media_gen_local_sdp(fst_session, SDP_ANSWER, rx_host, 12345, NULL, 1);
			local_sdp = switch_channel_get_variable(fst_channel, "rtp_local_sdp_str");
			fst_requires(local_sdp);
			fst_xcheck(strstr(local_sdp, "PCMA/8000") != NULL, "the answer follows the swap");

			/*
			 * Still the old codec here: the read loop has not run, which is what tells
			 * the deferred reset apart from one forced on this thread.
			 */
			fst_requires(switch_core_session_get_read_impl(fst_session, &impl) == SWITCH_STATUS_SUCCESS);
			fst_xcheck(!strcasecmp(impl.iananame, "PCMU"), "the codec reset is left to the read loop");
		}
		FST_SESSION_END()

		/*
		 * A ptime change on one codec takes the inline reset as well, not only a flip
		 * between two implementations of it. ptime is part of the payload map key, so a
		 * re-offer that changes it allocates a fresh map, which is the same reason the
		 * implementation flip cannot be left to the read loop.
		 */
		FST_SESSION_BEGIN(sdp_strict_codec_match_resets_on_ptime_change)
		{
			switch_status_t status;
			switch_media_handle_t *media_handle;
			switch_core_media_params_t *mparams;
			switch_codec_implementation_t impl = { 0 };
			uint8_t match = 0, p = 0;

			const char *offer_20ms =
				"v=0\r\n"
				"o=- 5 1 IN IP4 198.51.100.1\r\n"
				"s=-\r\n"
				"t=0 0\r\n"
				"m=audio 56210 RTP/AVP 0\r\n"
				"c=IN IP4 198.51.100.1\r\n"
				"a=rtpmap:0 PCMU/8000\r\n"
				"a=ptime:20\r\n"
				"a=sendrecv\r\n";

			const char *offer_30ms =
				"v=0\r\n"
				"o=- 5 2 IN IP4 198.51.100.1\r\n"
				"s=-\r\n"
				"t=0 0\r\n"
				"m=audio 56210 RTP/AVP 0\r\n"
				"c=IN IP4 198.51.100.1\r\n"
				"a=rtpmap:0 PCMU/8000\r\n"
				"a=ptime:30\r\n"
				"a=sendrecv\r\n";

			switch_channel_set_variable(fst_channel, "telnyx-strict-codec-match", "true");
			switch_channel_set_variable(fst_channel, "absolute_codec_string", "PCMU");

			mparams = switch_core_session_alloc(fst_session, sizeof(switch_core_media_params_t));
			mparams->inbound_codec_string = switch_core_session_strdup(fst_session, "PCMU");
			mparams->outbound_codec_string = switch_core_session_strdup(fst_session, "PCMU");
			mparams->rtpip = switch_core_session_strdup(fst_session, (char *)rx_host);

			status = switch_media_handle_create(&media_handle, fst_session, mparams);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			status = switch_core_media_prepare_codecs(fst_session, SWITCH_FALSE);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			match = switch_core_media_negotiate_sdp(fst_session, offer_20ms, &p, SDP_OFFER);
			fst_requires(match == 1);
			fst_requires(switch_core_session_get_read_impl(fst_session, &impl) == SWITCH_STATUS_SUCCESS);
			fst_requires(impl.microseconds_per_packet == 20000);

			match = switch_core_media_negotiate_sdp(fst_session, offer_30ms, &p, SDP_OFFER);
			fst_requires(match == 1);

			/* Reset already done here, unlike the unrelated swap above. */
			fst_requires(switch_core_session_get_read_impl(fst_session, &impl) == SWITCH_STATUS_SUCCESS);
			fst_xcheck(impl.microseconds_per_packet == 30000, "a ptime change on one codec resets inline");
		}
		FST_SESSION_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
