# TEL-6738 BUNDLE / Unified Plan v2 Design Note

Branch: `jira-tel-6738-bundle-v2`  
Base: `origin/telnyx/telephony/deploy-development`  
Prototype/reference only: `origin/jira-tel-6738-new`  
Normative target: RFC 9143

## 1. What was wrong or incomplete in `jira-tel-6738-new`

The prototype proved useful, but it is not production-ready and must not be merged wholesale.

Main problems:

1. **BUNDLE activation was not negotiated strictly enough.**
   - Several paths set `CF_BUNDLE_MEDIA`, `rtp_use_bundle`, or `rtp_group_bundle` directly from SDP hints or channel variables.
   - Runtime media setup then treated those flags as sufficient to create bundled media, instead of relying on a validated offer/answer result.

2. **No single negotiated BUNDLE state object.**
   - State is spread across channel variables such as `rtp_audio_mid`, `rtp_video_mid`, `rtp_mid_ext_id`, `rtp_group_bundle`, and per-engine booleans like `bundled_with_audio`.
   - This makes renegotiation, rejected m-lines, bundle-only m-lines, extmap changes, and BUNDLE rejection unsafe.

3. **Shared RTP ownership is ad hoc.**
   - The prototype creates secondary video RTP sessions without sockets and links them to audio/master behavior through flags and pointer references.
   - Lifetime and ownership semantics are unclear. Secondary sessions can still touch state that should belong only to the BUNDLE-tag/master transport.

4. **MID support is too loose.**
   - MID IDs are inferred from channel variables or defaulted to `1` in some paths.
   - The prototype does not model local and remote MID extmap IDs per m-line as negotiated state.
   - Extension ID collisions, ID 0 rejection, missing MID extension handling, and two-byte header extension behavior are not fully specified.

5. **RTP demux is incomplete.**
   - The prototype added MID and SSRC routing, but fallback behavior is not sufficiently strict.
   - Payload-type demux must only be used when payload types uniquely identify one bundled m-line.
   - Unknown MID/SSRC must never fall through into the audio/master engine accidentally.

6. **RTCP demux is not a first-class implementation.**
   - Video RTCP feedback must reach the video engine. It cannot be swallowed by the audio/master RTP session.
   - SR/RR/SDES/BYE/NACK/PLI/FIR/REMB/transport-cc need explicit routing rules.

7. **Renegotiation handling is incomplete.**
   - ICE restart, DTLS role changes, video add/remove, rejected m-lines, MID/extmap changes, payload type changes, BUNDLE rejection, and BUNDLE-tag rejection are not modeled as state transitions.

8. **Debug and workaround commits polluted the branch.**
   - The prototype contains many debugging commits and local macOS build fixes that are not part of the production BUNDLE feature.
   - Those are useful for learning, but not for the v2 patch series.

9. **Video bridge/JB debugging revealed architecture issues.**
   - Later prototype debugging showed symptoms such as JB pointer mismatch, blocking video reads, and one-way helper threads.
   - Those should not be solved by layering more bridge workarounds on top of a weak BUNDLE state model.

## 2. Prototype parts worth reusing

Reusable as reference, not blind cherry-picks:

1. **SDP parsing touchpoints.**
   - Existing parse locations for `a=group:BUNDLE`, `a=mid`, and `a=extmap` show where FreeSWITCH currently sees the relevant SDP attributes.
   - These should be replaced with structured parsing into a negotiated model.

2. **MID RTP extension packet mechanics.**
   - The RTP extension parsing/writing code is useful as a starting point.
   - It must be made runtime-negotiated and validated, with local/remote extmap state per bundled m-line.

3. **Video helper findings.**
   - `SWITCH_IO_FLAG_SINGLE_READ` and the DTLS/media readiness findings are useful context for not blocking video bridge threads.
   - They are bridge stability improvements, not substitutes for correct BUNDLE negotiation.

4. **Test cases and SIPp artifacts.**
   - The prototype tests around BUNDLE SDP, non-BUNDLE SDP, VP8/VP9, and RTP flow are useful seeds.
   - They need to be expanded into RFC 9143 SDP/RTP/RTCP/interop coverage.

5. **mod_telnyx_rtc write-video discovery.**
   - The missing `write_video_frame` path in `mod_telnyx_rtc` must be rechecked. If still missing on the v2 base, it belongs in the companion mod_telnyx_rtc branch.

## 3. Prototype parts to reject or rewrite

Reject/rewrite:

1. Channel-variable-driven BUNDLE activation.
2. Default/guessed MIDs such as implicit `audio`, `video`, `0`, `1` except as explicitly documented interoperability fallback before negotiation, never as accepted BUNDLE state.
3. Default/guessed MID extmap ID `1` in production paths.
4. Compile-time or shortcut MID behavior such as `HAVE_MID_EXT` style gating.
5. Secondary RTP sessions that share master internals without explicit ownership/refcount rules.
6. Demux by payload type unless uniqueness is proven within the negotiated BUNDLE group.
7. Any path where unknown bundled RTP enters the audio/master engine.
8. SDP generation that emits BUNDLE based only on `CF_BUNDLE_MEDIA`, `rtp_use_bundle`, or non-WebRTC profile variables.
9. Debug-only VIDEO/JB logs as permanent behavior, except minimal guarded diagnostics.
10. Unsafe SSRC generation or predictable SSRC arithmetic.

## 4. New architecture for RFC 9143 negotiated BUNDLE support

### 4.1 Feature gate

Add an explicit feature policy:

```text
rtp-bundle=off|auto|force
```

- `off`: never offer BUNDLE and do not accept BUNDLE.
- `auto`: accept/answer BUNDLE only when the remote offer is valid and all required transport/MID/RTCP conditions are satisfied.
- `force`: require valid BUNDLE; reject/fail invalid BUNDLE instead of silently falling back into a half-bundled call.

Default should be conservative. Non-WebRTC and legacy SIP/RTP behavior must stay unchanged.

### 4.2 Negotiated BUNDLE model

Add a media-core object owned by `switch_media_handle_t`, for example:

```c
typedef struct switch_bundle_group_s switch_bundle_group_t;
typedef struct switch_bundle_mline_s switch_bundle_mline_t;
```

It should track:

- policy: off/auto/force
- remote offered BUNDLE
- local offered BUNDLE
- BUNDLE accepted
- BUNDLE rejected reason
- BUNDLE-tag MID / primary m-line index
- local/remote m-line count and indexes
- per m-line:
  - media type
  - local MID
  - remote MID
  - bundle membership
  - bundle-only
  - rejected/zero-port
  - rtcp-mux / rtcp-mux-only
  - local and remote MID RTP extmap IDs
  - negotiated payload types
  - known local and remote SSRCs
- master transport owner
- secondary bundled media references
- generation/version for renegotiation

Channel variables may remain as debug/compatibility output, but production media decisions must read this object.

### 4.3 SDP negotiation flow

Offer/answer behavior:

1. Parse all m-lines first.
2. Parse session-level `a=group:BUNDLE` and validate each referenced MID exists.
3. Parse per-m-line `a=mid`, `a=bundle-only`, `a=rtcp-mux`, `a=rtcp-mux-only`, `a=extmap`, payload types, ICE and DTLS attributes.
4. Reject invalid accepted BUNDLE states:
   - BUNDLE group references missing MID.
   - Accepted bundled m-line lacks MID.
   - MID extension required for RTP demux but absent, unless a safe documented fallback exists.
   - BUNDLE-tag m-line is rejected without safe fallback.
   - Required rtcp-mux missing for bundled RTP/RTCP on one 5-tuple.
5. Store accepted state in `switch_bundle_group_t` only after offer/answer succeeds.
6. SDP generation reads from the negotiated state and policy, not from loose flags.
7. If BUNDLE is rejected or not negotiated, use existing separate RTP sessions unchanged.

### 4.4 Shared RTP transport model

The BUNDLE-tag m-line owns the shared transport:

- UDP socket
- ICE state
- DTLS state
- SRTP state
- selected candidate pair
- RTP/RTCP mux transport

Secondary bundled media sessions hold references to the master through a small explicit transport object, for example:

```c
typedef struct switch_rtp_bundle_transport_s {
    switch_rtp_t *owner;
    switch_mutex_t *mutex;
    switch_refcount_t refs;
    switch_bool_t closing;
} switch_rtp_bundle_transport_t;
```

Rules:

- Only the owner closes/frees/reinitializes the socket/ICE/DTLS/SRTP transport.
- Secondary sessions may enqueue/dequeue media frames and read negotiated maps, but must not destroy transport state.
- ReINVITE/renegotiation must update the bundle generation atomically or safely reject the change.

### 4.5 RTP demux flow

Incoming RTP on the master shared transport:

1. Parse RTP header and extensions.
2. If MID RTP header extension exists:
   - Validate extension ID matches a negotiated remote MID extmap.
   - Route to the matching m-line.
   - Learn SSRC -> m-line mapping.
3. Else if SSRC is already known:
   - Route by SSRC.
4. Else if payload type uniquely maps to one bundled m-line:
   - Route by payload type and learn SSRC carefully.
5. Else:
   - Drop/log or queue briefly until MID/SSRC is known.
   - Never route unknown media into audio by default.

### 4.6 RTCP demux flow

Incoming RTCP on the master shared transport:

1. Parse packet type and report/feedback SSRCs.
2. Route using known SSRC mappings first.
3. Use RTCP SDES MID item when present to learn/confirm mapping.
4. Route feedback packets (PLI/NACK/FIR/REMB/transport-cc) to the media engine for the media SSRC they reference.
5. Route RR/SR/SDES/BYE to the correct m-line state.
6. Unknown RTCP SSRC is logged and handled safely, not swallowed by audio.

### 4.7 Renegotiation model

Each offer/answer creates a candidate bundle state. Commit only when valid.

Supported or safely rejected cases:

- ICE restart: update master candidate/ICE generation if BUNDLE remains valid.
- DTLS role change: update owner DTLS state only through master.
- hold/resume: preserve accepted bundle mapping unless m-line rejection/removal changes it.
- video added after audio-only: create/attach secondary media only after valid negotiated BUNDLE state.
- video removed/rejected/port zero: detach secondary media without disturbing master audio.
- changed MID or extmap ID: treat as renegotiation; update maps atomically or reject safely.
- changed payload types: recompute uniqueness for payload-type fallback.
- BUNDLE rejected in answer: fall back to separate RTP only if still valid and safe.
- BUNDLE-tag rejected: reject BUNDLE or call, unless a valid replacement BUNDLE-tag is negotiated.

## 5. Patch series to implement

1. **Design/docs and feature gate**
   - Add this design note.
   - Add `rtp-bundle=off|auto|force` config parsing with conservative defaults.

2. **SDP parser and BUNDLE validation**
   - Add structured parsing for `a=group:BUNDLE`, `a=mid`, `a=bundle-only`, `a=rtcp-mux`, `a=rtcp-mux-only`, MID extmap, rejected m-lines, zero-port m-lines, ICE/DTLS ownership attributes.
   - Add validation helpers and rejection reasons.

3. **Negotiated BUNDLE state object**
   - Add `switch_bundle_group_t` / per-m-line model under media handle.
   - Store negotiated local/remote MIDs, extmaps, payload types, SSRC maps, accepted/rejected states.

4. **SDP generation from negotiated state**
   - Emit BUNDLE only when policy and negotiation permit.
   - Preserve legacy/non-BUNDLE generation unchanged.
   - Generate correct BUNDLE-tag, bundled m-line ports, zero-port/rejected behavior, `a=bundle-only`, MID and extmap attributes.

5. **Shared RTP transport ownership**
   - Introduce explicit master/secondary transport reference model.
   - Ensure secondary sessions cannot close/free/reset master transport.

6. **MID RTP extension runtime support**
   - Validate negotiated extmap IDs.
   - Store local/remote IDs separately.
   - Add/extract one-byte MID extensions.
   - Add two-byte receive support if compatible with existing FreeSWITCH RTP extension parsing.

7. **RTP demux**
   - Implement MID -> learned SSRC -> unique payload-type fallback -> safe drop/queue.
   - Add SSRC collision handling and random SSRC generation with collision checks.

8. **RTCP demux**
   - Route SR/RR/SDES/BYE/NACK/PLI/FIR/REMB/transport-cc by SSRC/MID mapping.
   - Ensure video feedback reaches video engine.

9. **Renegotiation handling**
   - Add candidate/committed bundle state transitions.
   - Safely support or reject ICE restart, DTLS role changes, hold/resume, video add/remove, port zero, changed MID/extmap/PTs, browser re-offer, BUNDLE rejection.

10. **mod_telnyx_rtc companion audit/fix**
    - Verify `write_video_frame` and bundled WebRTC media behavior.
    - Add a clean companion branch if mod changes are required.

11. **Tests**
    - SDP unit tests.
    - RTP demux unit tests.
    - RTCP demux unit tests.
    - SIPp/manual Chrome/Firefox/Safari interop plan.
    - Regression tests for non-BUNDLE audio/video and audio-only WebRTC.

12. **Cleanup**
    - Remove debug-only logs.
    - Remove old channel-var production decisions.
    - Document limitations and remaining risks.

## 6. Initial known limitations to decide before implementation

1. Two-byte RTP header extension support may require additional RTP parser work. If not implemented immediately, one-byte support should be explicit and two-byte packets should be safely rejected/logged.
2. Some renegotiation paths may be safer to reject initially. If so, the rejection must be explicit and must not leave a half-bundled call.
3. RTCP feedback coverage depends on existing FreeSWITCH RTCP parsing capabilities. Any unsupported feedback type must be logged and safely ignored, not misrouted.

## 7. Immediate next steps

1. Review the current base for existing prototype remnants already present in `deploy-development`.
2. Implement patch 1 and 2 first: feature gate + structured parser/validator.
3. Add tests before enabling transport sharing.
4. Audit `mod_telnyx_rtc` against the final media write/read contract.

## 8. FS-coder review addendum

A second FreeSWITCH-focused review of `origin/jira-tel-6738-new` found one additional hard constraint for v2: the prototype branch is not just an incomplete BUNDLE implementation. It also carries unrelated regressions against `deploy-development`. v2 must avoid those regressions entirely.

Examples of unrelated changes to exclude from v2:

- RFC2833 / DTMF guard changes such as `out_digit_last_progress_us` and `PFLAG_IGNORE_RTP_DURING_DTMF`.
- 603 Network Blocked passthrough handling.
- Sofia deadlock avoidance around `sofia_mutex` / RTP activation.
- Extended media bug API/state such as `switch_core_media_bug_add_ex` and `SMBF_EXT_NO_READ_DEMUX`.
- Partial-frame flush behavior in `switch_core_media_bug_read`.
- Channel variable locking and CHANNEL_DESTROY variable snapshot safety fixes.
- Recording teardown timed waits.
- External NUA dispatch hooks and scoped Sofia parameter handling.
- Codec destruction mutex protections.
- Removed or reverted tests and utility files.

Additional implementation cautions from the review:

- Do not reuse payload-type fallback as a normal demux path. Payload-type routing is only safe when uniqueness is proven inside the negotiated BUNDLE group.
- Do not create a socketless secondary RTP session and then patch every `switch_rtp_ready()` callsite. v2 needs explicit master/secondary transport ownership.
- Do not keep production WARNING-level debug counters, especially static counters shared across sessions.
- Do not globally change non-BUNDLE video behavior, such as video poll timeout or CNG behavior, unless it is a separate reviewed fix.
- Do not keep the prototype early-packet queue unless packet length, ownership, and cleanup semantics are made safe.

Reusable pieces confirmed by review:

- MID RTP header extension parsing/writing mechanics are valuable, but must be tied to runtime-negotiated extmap IDs.
- `switch_rtp_enable_mid()` style API is a reasonable primitive.
- Media-level `a=extmap` generation for MID is worth preserving.
- Existing unit tests for MID and SDP BUNDLE can seed the v2 test suite.
- The `mod_telnyx_rtc` `write_video_frame` discovery should be audited in the companion repo.

This addendum strengthens the implementation rule: every v2 commit must be checked against `deploy-development` to ensure it only introduces the intended BUNDLE change and does not remove unrelated production fixes.
