# freeswitch

Telnyx fork of the SignalWire FreeSWITCH codebase — a large, loadable-module
softswitch written primarily in C (with C++ in some modules). This fork
carries Telnyx-specific bugfixes, stability patches, and build integrations
for the `b2bua` / `b2bua-rtc` runtime images.

## Upstream origin
| Field | Value |
|---|---|
| Upstream URL | https://github.com/signalwire/freeswitch |
| Fork pattern | Full-source fork; the sibling `mod_*` Telnyx-original modules are NOT in this repo — they are dropped into `src/mod/{category}/mod_xxx/` at build time by `telnyx_b2bua_builder/Makefile` |
| Default branch  | `telnyx/telephony/master` (yes, nested — NOT plain `master` or `main`) |
| Deploy branch   | `telnyx/telephony/deploy-development` |
| Tag scheme      | `1.10.x` upstream version combined with Telnyx patch tags |

## Ecosystem position
| Field | Value |
|---|---|
| Layer | Core FreeSWITCH codebase (forked) |
| Loaded in | **both** `b2bua` and `b2bua-rtc` — this IS the softswitch runtime |
| Build integration | `telnyx_b2bua_builder/Makefile` clones this repo at `FS_BRANCH` (auto-picks `telnyx/telephony/<push-branch>` if available, else falls back to `telnyx/telephony/master`), then `git clone -b` drops sibling Telnyx modules into `src/mod/applications/`, `src/mod/endpoints/`, etc. |
| Branch-sync targets | `telnyx_b2bua_builder`, `b2bua`, `b2bua-rtc` (all three — via `trigger-b2bua-builder.yml`) |
| GHA `upstream-build` dispatch | `telnyx_b2bua_builder` on push / PR to `telnyx/telephony/master`, `telnyx/telephony/deploy-*`, `telnyx/telephony/release-*` |

See `core-tel-ai-agents/docs/FS-ECOSYSTEM-LINKAGE.md` for the full 43-repo topology.

## Source tree — top level
```
freeswitch/
├── Makefile.am              Top-level autotools
├── configure.ac             Build configuration / feature flags / deps
├── bootstrap.sh             Runs autoreconf to regenerate configure
├── build/                   Per-OS Makefile fragments, lint helpers, limits.conf
├── clients/                 CLI clients (fs_cli source is here + ESL bindings)
├── conf/                    Sample runtime configs (NOT what b2bua uses — b2bua
│                            has its own root_file_system/etc/freeswitch/ tree)
├── debian/                  Debian packaging
├── docker/{base_image,master,release}/
│                            Upstream's Docker examples (we do NOT use these —
│                            telnyx_b2bua_builder has its own Dockerfile)
├── docs/                    Upstream docs (outdated relative to Telnyx fork)
├── libs/                    Vendored libs: apr, esl, iksemel, libteletone,
│                            libnatpmp, miniupnpc, srtp, xmlrpc-c, libscgi,
│                            freetype, unimrcp (older), win32 bits
├── scripts/                 Misc perl/shell utilities
├── src/                     Core engine + modules (see below)
├── support-d/               Docker dev-support recipes
├── tests/                   Unit tests
└── w32/                     Windows-specific bits
```

## `src/` — the engine core

### Core subsystems (top-level `switch_*.c`)
| File | Lines | Role |
|---|---|---|
| `switch_core.c`                | 3,701  | Core lifecycle: `switch_core_init`, `switch_core_init_and_modload`, `switch_core_destroy`, global state |
| `switch_core_session.c`        | 3,370  | Session lifecycle, refcount, pool management, thread launching |
| `switch_core_media.c`          | 20,432 | **Biggest file**. Media engines, SDP, RTP attach/detach, re-invite handling, codec negotiation, VP8/H.264 video glue. Start here for audio/video stack questions |
| `switch_core_media_bug.c`      | 1,605  | Media bug framework (session-attached audio/video tap for record/stream/analysis) |
| `switch_core_io.c`             | 1,428  | Read/write frame abstractions used by endpoints |
| `switch_core_state_machine.c`  | 1,013  | Channel state machine (CS_NEW → CS_INIT → … → CS_HANGUP) |
| `switch_core_codec.c`          | 977    | Codec load/init, sample-rate & ptime negotiation |
| `switch_core_sqldb.c`          | 4,011  | Internal SQLite and core-recovery DB |
| `switch_core_file.c`           | 1,118  | File I/O abstraction for `play_and_get_digits`, etc. |
| `switch_core_memory.c`         | 761    | APR pool management |
| `switch_core_rwlock.c`         | 181    | Read/write lock primitives |
| `switch_core_video.c`          | 3,913  | libfreeswitch video utilities (YUV, RGB, overlays) |
| `switch_core_cert.c`           | 532    | TLS/DTLS certificates |
| `switch_channel.c`             | 5,970  | Channel object: variables, flags, state, capabilities, media-handle attachment |
| `switch_ivr.c`                 | 4,550  | IVR primitives: uuid ops, grammars, park, transfer |
| `switch_ivr_async.c`           | 6,520  | Async IVR: session bgapi, speech detect, media stream, DTMF async |
| `switch_ivr_bridge.c`          | 2,788  | `bridge` app — B2BUA audio/video bridging loop |
| `switch_ivr_originate.c`       | 5,251  | `originate` API — outbound leg creation |
| `switch_ivr_play_say.c`        | 3,684  | `playback`, `say`, `play_and_get_digits` |
| `switch_rtp.c`                 | 12,017 | **RTP/SRTP implementation**. Packet send/receive, jitter buffer, RTCP probe hooks, DTLS, ICE, STUN. `mod_homer_rtcp` attaches here. |
| `switch_stun.c`                | 1,279  | STUN protocol |
| `switch_xml.c`                 | 3,843  | XML parser (used by config, dialplan, directory) |
| `switch_time.c`                | 2,514  | Time primitives, scheduler |
| `switch_utils.c`               | 4,965  | Grab-bag utilities (hashing, cron, UUID, etc.) |
| `switch_vpx.c`                 | 2,067  | VP8 codec (Telnyx has patches) |
| `switch_loadable_module.c`     | 3,271  | Module loader — implements `load`, `unload`, `reload`, enumerates app/api/endpoint interfaces |
| `switch_pcm.c`                 | 1,121  | PCM raw codec plumbing |
| `switch_speex.c`               | 644    | Speex codec |
| `switch_resample.c`            | 614    | Sample-rate conversion |

The core headers are in `src/include/` — start at `switch.h` (public API entry).
Any `switch_*` function referenced from sibling Telnyx modules is declared there.

### `src/mod/` — module subdirectories
61 applications, 22 codecs, and several other categories (dialplans, asr/tts,
endpoints, event-handlers, etc.). At runtime, `modules.conf.xml` (in the
`b2bua` / `b2bua-rtc` repos, NOT here) decides which are actually loaded.

| Category | Path | Representative modules |
|---|---|---|
| Applications       | `src/mod/applications/`  | `mod_abstraction`, `mod_av`, `mod_avmd`, `mod_callcenter`, `mod_cluechoo`, `mod_commands`, `mod_conference`, `mod_cidlookup`, `mod_dptools`, `mod_easyroute`, `mod_enum`, `mod_esf`, `mod_esl`, `mod_expr`, `mod_fax`, `mod_fifo`, `mod_fsk`, `mod_fsv`, `mod_hash`, `mod_http_cache`, `mod_httapi`, `mod_ladspa`, `mod_limit`, `mod_memcache`, `mod_mongo`, `mod_mp4`, `mod_nibblebill`, `mod_redis`, `mod_rss`, `mod_signalwire`, `mod_sms`, `mod_snapshot`, `mod_snom`, `mod_soundtouch`, `mod_spandsp`, `mod_spy`, `mod_stress`, `mod_translate`, `mod_valet_parking`, `mod_voicemail`, `mod_voicemail_ivr`, `mod_xml_interfaces` … |
| ASR/TTS            | `src/mod/asr_tts/`       | `mod_cepstral`, `mod_flite`, `mod_pocketsphinx`, `mod_tts_commandline`, `mod_unimrcp` (upstream version — distinct from Telnyx `mod_unimrcp`) |
| Codecs             | `src/mod/codecs/`        | `mod_amr`, `mod_amrwb`, `mod_b64`, `mod_bv`, `mod_clearmode`, `mod_codec2`, `mod_com_g729`, `mod_dahdi_codec`, `mod_g723_1`, `mod_g729`, `mod_h26x`, `mod_ilbc`, `mod_isac`, `mod_mp4v`, `mod_opus`, `mod_png`, `mod_silk`, `mod_siren`, `mod_skel_codec`, `mod_speex`, `mod_theora`, `mod_vpx` |
| Databases          | `src/mod/databases/`     | `mod_db` (SQLite), `mod_mariadb`, `mod_pgsql` |
| Dialplans          | `src/mod/dialplans/`     | `mod_dialplan_asterisk`, `mod_dialplan_directory`, `mod_dialplan_xml` |
| Directories        | `src/mod/directories/`   | `mod_ldap` |
| Endpoints          | `src/mod/endpoints/`     | **`mod_sofia`** (SIP — ~35,454 lines; the critical one), `mod_alsa`, `mod_gsmopen`, `mod_h323`, `mod_khomp`, `mod_loopback`, `mod_opal`, `mod_portaudio`, `mod_reference`, `mod_rtc`, `mod_rtmp`, `mod_skinny`, `mod_skypopen`, `mod_unicall`, `mod_verto` |
| Event handlers     | `src/mod/event_handlers/` | `mod_amqp`, `mod_cdr_csv`, `mod_cdr_mongodb`, `mod_cdr_pg_csv`, `mod_cdr_sqlite`, `mod_erlang_event`, `mod_event_multicast`, `mod_event_socket` (ESL — mgmt port 8021), `mod_fail2ban`, `mod_json_cdr`, `mod_kazoo`, `mod_radius_cdr`, `mod_rayo`, `mod_smpp`, `mod_snmp` |
| Formats            | `src/mod/formats/`       | `mod_local_stream`, `mod_native_file`, `mod_opusfile`, `mod_portaudio_stream`, `mod_shell_stream`, `mod_shout`, `mod_sndfile`, `mod_tone_stream` |
| Languages          | `src/mod/languages/`     | `mod_java`, `mod_lua`, `mod_managed`, `mod_perl`, `mod_python`, `mod_v8`, `mod_yaml` |
| Loggers            | `src/mod/loggers/`       | `mod_console`, `mod_graylog2`, `mod_logfile`, `mod_syslog` |
| Say                | `src/mod/say/`           | Per-locale `say` implementations (say_de, say_en, say_es, say_fr, …) |
| Timers             | `src/mod/timers/`        | `mod_timerfd`, `mod_posix_timer`, `mod_zrtp` |
| XML int            | `src/mod/xml_int/`       | `mod_xml_cdr`, `mod_xml_curl`, `mod_xml_ldap`, `mod_xml_radius`, `mod_xml_rpc`, `mod_xml_scgi` |

**Critical: `mod_sofia`** is the SIP endpoint. It lives at
`src/mod/endpoints/mod_sofia/` and spans ~35,454 lines across `mod_sofia.c`,
`sofia.c`, `sofia_glue.c`, `sofia_presence.c`, `sofia_reg.c`, `sofia_json_api.c`,
`sofia_media.c`, and several headers. Any SIP-signaling question lands here.
Uses the vendored `sofia-sip` fork (sibling repo `team-telnyx/sofia-sip`).

## `libs/` — vendored libraries built in-tree
| Dir | Library |
|---|---|
| `libs/apr`           | Apache Portable Runtime (process/thread/IO/pool) |
| `libs/esl`           | Event Socket Library (C client for ESL) |
| `libs/iksemel`       | XMPP / XML helper |
| `libs/libteletone`   | DTMF / tone generation |
| `libs/libnatpmp`     | NAT-PMP support |
| `libs/miniupnpc`     | UPnP support |
| `libs/srtp`          | libsrtp (SRTP/DTLS-SRTP) |
| `libs/xmlrpc-c`      | XML-RPC over HTTP (used by `mod_xml_rpc`) |
| `libs/libscgi`       | SCGI (used by `mod_xml_scgi`) |
| `libs/freetype`      | Font rendering (video overlays) |

External Telnyx-owned libs (NOT vendored here — sibling repos, pulled by
`fs_docker_deps`): `sofia-sip`, `spandsp`, `bcg729`, `libhdsp`,
`lib3gpp-evs-fp`, `rnnoise-internal`, `soundtouch`, `libks`, `unimrcp`,
`IXWebSocket`.

## Build system

### Autotools
- `bootstrap.sh` → regenerates `configure` from `configure.ac` via `autoreconf`.
- `./configure` → detects deps (APR, Sofia-SIP, SpanDSP, etc.), picks which modules to build (via `modules.conf`).
- `make` → builds shared libs + all enabled modules.
- `make install` → installs into prefix.

### How Telnyx builds it
`telnyx_b2bua_builder/Makefile` does this flow (simplified):
1. `git clone -b <FS_BRANCH> https://github.com/team-telnyx/freeswitch.git`
   (FS_BRANCH auto-detected — matches the triggering push branch if possible)
2. For every sibling Telnyx module: `git clone -b <branch> https://github.com/team-telnyx/mod_<x>.git freeswitch/src/mod/<category>/mod_<x>`
3. `cd freeswitch && ./bootstrap.sh && ./configure && make && make install-strip`
4. Packages the result into `telnyx_b2bua-<FS_VERSION>-<TELNYX_REV>.deb`.

So when CI merges a branch here, it triggers `telnyx_b2bua_builder`, which
pulls this branch plus all the matching module branches into one tree and
builds the combined `.deb`. That deb is then consumed by `b2bua` and
`b2bua-rtc` Dockerfiles via `FROM ${DEPS_IMAGE}` + deb install.

### Upstream Docker dirs (unused by Telnyx)
- `docker/base_image/Dockerfile` — minimal `FROM scratch`
- `docker/master/Dockerfile` — `FROM debian:${DEBIAN_VERSION}` (bookworm by default) building master branch for dev testing
- `docker/release/Dockerfile` — `FROM debian:jessie` installing `freeswitch-all` deb (old)

Telnyx does NOT use these. The real Docker build is in
`telnyx_b2bua_builder/Dockerfile` + `freeswitch_docker_base/Dockerfile`.

## GHA workflows
- **`reviewpr.yml`** — `team-telnyx/reviewpr-internal@main` AI-powered PR review on every PR open/reopen/synchronize. Secret: `OPENAI_API_KEY`. Model: `gpt-5.2-codex`.
- **`trigger-b2bua-builder.yml`** — mirrors the pattern of every `mod_*` repo:
  - On push / PR to `master | main | deploy-* | release-*`: dispatches `upstream-build` to `telnyx_b2bua_builder`.
  - On push to `deploy-*` / `release-*`: creates the same-named branch in downstream targets.
  - **Downstream targets for branch-sync from this repo**: `telnyx_b2bua_builder`, `b2bua`, **AND** `b2bua-rtc` (this repo syncs to BOTH b2bua variants; many `mod_*` repos only sync to `b2bua`).
  - On delete of `deploy-*` / `release-*` (except `deploy-development`): deletes branches in the same targets.
  - Uses `GIT_TOKEN` secret.

## Jenkinsfile
Present but minimal — legacy trigger only. GHA is the primary CI path.

## Core concepts (quick reference for the agent)

### Module ABI
Every loadable module registers via `SWITCH_MODULE_DEFINITION(name, load, shutdown, runtime)`. `load` fills in the module interface (apps, APIs, endpoints, codecs, etc.) via `SWITCH_ADD_APP` / `SWITCH_ADD_API` / `switch_loadable_module_set_protocol`, etc. See `src/switch_loadable_module.c` for the loader.

### Channel lifecycle
State transitions in `switch_core_state_machine.c`:
`CS_NEW → CS_INIT → CS_ROUTING → CS_EXECUTE → CS_HANGUP → CS_REPORTING → CS_DESTROY`.
Each state fires `state_handler_*` callbacks registered by endpoints/applications.

### RTP hooks (relevant to `mod_homer_rtcp`)
- `switch_rtp_set_create_probe(callback)` — called when any RTP session becomes active
- `switch_rtp_set_rtcp_probe(rtp, callback)` — per-session RTCP packet hook
- `CF_ENABLE_RTCP_PROBE` channel flag enables emission

### Session locking
Sessions are APR rwlocks. Calling `switch_core_session_locate(uuid)` gets a read-locked session — **must** pair with `switch_core_session_rwunlock`. Forgetting this leaks session references and blocks cleanup.

### SDP/media negotiation
Owned by `switch_core_media.c`. Call path roughly:
`sofia receives INVITE` → `sofia_glue_tech_choose_codec` → `switch_core_media_choose_port` → `switch_core_media_choose_video_port` → send 200.
Negotiation respects `global-codec-prefs` and `inbound-codec-prefs` from the Sofia profile XML.

## Active Telnyx branches (naming convention)
- `telnyx/telephony/master`                — default (integration target)
- `telnyx/telephony/deploy-development`    — dev environment
- `telnyx/telephony/deploy-<cluster>`      — per-cluster prod deploys
- `telnyx/telephony/release-<tag>`         — version release cuts
- `telnyx/bug/<TICKET>`                    — one-off bugfix branches
- `telnyx/feature/<TICKET>`                — feature branches
- `deploy-jira-<ticket>`                   — short-lived deploy branches (newer convention)

Not every PR needs to target `master` — Telnyx flow often opens PRs against
`deploy-development` for faster dev-cluster iteration.

## Things to read when answering questions about this repo
- "Where is X bridged?" → `src/switch_ivr_bridge.c` + `src/switch_core_media.c`
- "How does codec X get picked?" → `src/switch_core_media.c::switch_core_media_choose_port` and the `switch_core_codec.c` init path, plus `src/mod/codecs/mod_<x>/`
- "What happens on SIP INVITE?" → `src/mod/endpoints/mod_sofia/sofia.c` (sofia_queue_* / sofia_handle_sip_i_invite), then `sofia_glue.c::sofia_glue_attach_private`
- "Channel state transitions?" → `src/switch_core_state_machine.c`
- "RTP packet send path?" → `src/switch_rtp.c::switch_rtp_write_frame` (and the encryption/DTLS wrappers)
- "Why is this codec negotiation failing?" → `src/switch_core_media.c` + mod_sofia's SDP-handling in `sofia_glue.c`
- "How are events fired/bound?" → `src/switch_event.c` + any module's `switch_event_bind` calls
- "How is the dialplan resolved?" → `src/mod/dialplans/mod_dialplan_xml/` (default), possibly `mod_dialplan_asterisk` or `mod_xml_curl` for dynamic lookups

## Integration with Telnyx-specific modules (cross-references)
When a Telnyx sibling module references a `switch_*` function, it's declared here:
- `mod_telnyx`, `mod_call_control`, `mod_call_recovery`, `mod_q2s_mapping`, `mod_backtrace`, `mod_xml_http`, `mod_happy_voicemail`, `mod_dynamic_gateway` — all live OUTSIDE this repo; get dropped into `src/mod/applications/` at build time.
- `mod_homer_rtcp`, `mod_denoiser`, `mod_krisp_nc`, `mod_deep_filter_net`, `mod_ai_acoustics`, `mod_audio_stream-src`, `mod_siprec_src`, `mod_polly`, `mod_gstt`, `mod_unimrcp` (Telnyx variant) — audio/media modules dropped into `src/mod/applications/`.
- `mod_telnyx_rtc`, `mod_telnyx_video` — dropped into `src/mod/endpoints/` (they augment mod_sofia's role for WebRTC).
- `mod_bcg729`, `mod_evs` — codec modules dropped into `src/mod/codecs/`.

## Rules
- Follow the 23 FreeSWITCH coding rules at
  `core-tel-ai-agents/core_tel_ai_agents/agents/fs_knowledge/system_prompt.py` §9.
- Module-specific style + memory / threading conventions live in
  `.llm-repo-instructions` in this repo — style guidance, not duplicated here.
- This fork is MASSIVE (millions of lines). When answering, prefer the
  "where is X?" index above plus `fs_read_file` on the specific line range
  over trying to describe whole subsystems inline.
