#ifndef __SWITCH_CORE_MEDIA_PVT_H__
#define __SWITCH_CORE_MEDIA_PVT_H__

/* Core-internal VBR ptime inference, declared here (not in switch_core_media.h) for the unit tests only. */

SWITCH_BEGIN_EXTERN_C

typedef struct switch_vbr_ptime_state_s {
	uint32_t last_ts;
	uint16_t last_seq;
	uint32_t last_codec_ms;
	uint32_t mismatch_count;
} switch_vbr_ptime_state_t;

/*!
  \brief Feed one received audio frame to the VBR ptime inference
  \param session the session to log against (may be NULL)
  \param state the inference state, zeroed to start over
  \param ts the frame's RTP timestamp
  \param seq the frame's RTP sequence number
  \param samples_per_second the codec clock rate
  \param cur_ms the ptime currently in use
  \return the inferred ptime once consecutive frames agree on one other than cur_ms, otherwise 0
*/
SWITCH_DECLARE(uint32_t) switch_core_media_pvt_vbr_ptime_observe(switch_core_session_t *session, switch_vbr_ptime_state_t *state,
															     uint32_t ts, uint16_t seq, uint32_t samples_per_second, uint32_t cur_ms);
/*!
  \brief Adopt an inferred VBR ptime if the running codec can load it
  \param session the session to log against and flag for a conference media reset (may be NULL)
  \param codec_mutex the lock codec replacement takes; only tried, never waited for
  \param codec the running read codec
  \param pmap the payload map the codec was set from
  \param codec_ms the inferred ptime
  \param reset_codec set to request a codec reset when adopted
  \param codec_generation the codec generation codec replacement advances under codec_mutex (NULL to skip the check)
  \param observed_generation the generation the inference was built on
  \return SWITCH_TRUE if pmap now carries codec_ms and a reset is pending
*/
SWITCH_DECLARE(switch_bool_t) switch_core_media_pvt_vbr_ptime_adopt(switch_core_session_t *session, switch_mutex_t *codec_mutex, switch_codec_t *codec,
																    payload_map_t *pmap, uint32_t codec_ms, uint8_t *reset_codec,
																    volatile switch_atomic_t *codec_generation, uint32_t observed_generation);

SWITCH_END_EXTERN_C

#endif

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
