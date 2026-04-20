# freeswitch

Telnyx fork of the SignalWire FreeSWITCH codebase — a large, modular softswitch
written primarily in C (some C++ in selected modules). This fork carries
Telnyx-specific bugfixes, stability patches, and the integration points for
the `b2bua` / `b2bua-rtc` runtime images.

Scale: ~128,000 lines just in `src/switch_*.c` core files; `mod_sofia` alone
is ~35,454 lines. Entire tree spans hundreds of modules and millions of
lines including vendored libs. This CLAUDE.md is an INDEX — agents should
use `fs_read_file` for specific line ranges rather than trying to fit full
source in context.

## Upstream origin
| Field | Value |
|---|---|
| Upstream URL | https://github.com/signalwire/freeswitch |
| Fork pattern | Full-source fork. Sibling Telnyx-original `mod_*` modules are NOT in this repo — they're dropped into `src/mod/<category>/mod_xxx/` at build time by `telnyx_b2bua_builder/Makefile` |
| Default branch  | `telnyx/telephony/master` (nested — NOT plain `master`) |
| Deploy branch   | `telnyx/telephony/deploy-development` |
| License         | MPL-1.1 (see `LICENSE`) |
| Version family  | 1.10.x (aligned with upstream) |

## Ecosystem position
| Field | Value |
|---|---|
| Layer | Core FreeSWITCH engine (forked) |
| Loaded in | **both** `b2bua` and `b2bua-rtc` — this IS the runtime softswitch |
| Built via | `telnyx_b2bua_builder/Makefile` — clones this repo, drops in sibling Telnyx modules, runs autotools, packages `.deb` |
| Branch-sync targets | `telnyx_b2bua_builder`, `b2bua`, **`b2bua-rtc`** (all three — this repo syncs to BOTH b2bua variants) |
| Deploy metadata | service name: none (this is a lib; the runtime is `b2bua` / `b2bua-rtc`) |

See `core-tel-ai-agents/docs/FS-ECOSYSTEM-LINKAGE.md` for the full 43-repo topology.

## Source tree — top level
```
freeswitch/
├── Makefile.am              Top-level autotools
├── Makefile.swig            SWIG bindings
├── configure.ac             Build configuration / feature flags / dep detection
├── acinclude.m4             Autoconf macros
├── bootstrap.sh             Regenerates configure via autoreconf
├── devel-bootstrap.sh       Dev-mode bootstrap (for upstream contribution)
├── cc.sh                    Wrapper compiler script
├── build/                   Per-OS Makefile fragments, lint helpers, limits.conf,
│                            modmake.rulesam (shared module build rules), debian/
├── clients/                 CLI clients (fs_cli source, ESL bindings)
├── cmake_modules/           CMake find-modules (for parts that use CMake)
├── conf/                    Sample runtime configs (NOT what b2bua uses — b2bua has
│                            its own root_file_system/etc/freeswitch/ tree)
├── debian/                  Debian packaging rules
├── docker/{base_image,master,release}/  Upstream example Dockerfiles
│                            (NOT used by Telnyx — telnyx_b2bua_builder has its own)
├── docs/                    Upstream docs (outdated relative to Telnyx fork)
├── dtd/                     SGML/XML DTDs
├── fonts/                   Bitmap fonts (video overlays)
├── freeswitch*.spec         RPM packaging specs (upstream-maintained)
├── html/                    Legacy docs
├── libs/                    Vendored libs: apr, esl, iksemel, libteletone,
│                            libnatpmp, miniupnpc, srtp, xmlrpc-c, libscgi,
│                            freetype, unimrcp (older), win32 bits, libsofia-sip-ua-mw
├── scripts/                 Misc perl/shell helper scripts
├── src/                     Core engine + modules (see §"src/" below)
├── support-d/               Docker dev-support recipes, devel-bootstrap helpers
├── tests/                   Unit tests, integration helpers
└── w32/                     Windows-specific build bits
```

## `src/` — the engine core

### Core subsystems (top-level `switch_*.c`)

| File | Lines | Role |
|---|---|---|
| `switch_core.c`                | 3,701  | Core lifecycle: `switch_core_init`, `switch_core_init_and_modload`, `switch_core_destroy`. Global state, runtime flags, system start/stop. |
| `switch_core_session.c`        | 3,370  | Session lifecycle, refcount, APR pool per session, thread launch. Session = one call leg with its own memory pool + event thread. |
| `switch_core_media.c`          | 20,432 | **Biggest file**. Media engines, SDP build/parse, RTP attach/detach, re-INVITE handling, codec negotiation, ICE/DTLS, VP8/H.264 video glue. Start here for anything media-related. |
| `switch_core_media_bug.c`      | 1,605  | Media bug framework — inline audio/video tap attached to a session for record/stream/transcribe. |
| `switch_core_io.c`             | 1,428  | Read/write frame abstractions used by endpoints; frame timing, silence generation. |
| `switch_core_state_machine.c`  | 1,013  | Channel state machine (CS_NEW → CS_INIT → CS_ROUTING → CS_CONSUME_MEDIA → CS_EXECUTE → CS_EXCHANGE_MEDIA → CS_PARK → CS_HANGUP → CS_REPORTING → CS_DESTROY). |
| `switch_core_codec.c`          | 977    | Codec load/init/destroy, sample-rate/ptime/channels negotiation. |
| `switch_core_sqldb.c`          | 4,011  | Internal recovery/state SQLite (or Postgres/MariaDB via modules). Tracks channels, calls, registrations. |
| `switch_core_file.c`           | 1,118  | File I/O for `playback`/`play_and_get_digits`/`record`. |
| `switch_core_memory.c`         | 761    | APR memory-pool wrappers (`switch_core_alloc`, `switch_core_session_alloc`). |
| `switch_core_rwlock.c`         | 181    | Read/write lock primitives (APR rwlocks). |
| `switch_core_video.c`          | 3,913  | Video utilities: YUV/RGB/BGRA conversion, overlay, picture-in-picture. |
| `switch_core_cert.c`           | 532    | TLS/DTLS certificates, SDES/DTLS-SRTP setup. |
| `switch_core_asr.c`            | 382    | ASR interface (implemented by `mod_unimrcp`, `mod_pocketsphinx`, `mod_gstt`). |
| `switch_core_speech.c`         | 317    | TTS interface (implemented by `mod_flite`, `mod_polly`, `mod_cepstral`, `mod_tts_commandline`). |
| `switch_core_timer.c`          | 142    | Timer interface (implemented by `mod_timerfd`, `mod_posix_timer`, `mod_zrtp`). |
| `switch_core_db.c`             | 430    | SQLite wrapper. |
| `switch_core_hash.c`           | 384    | Hash table primitives. |
| `switch_core_directory.c`      | 99     | Directory interface stubs (populated by `mod_ldap`, etc.). |
| `switch_core_event_hook.c`     | 58     | Core event-hook registration. |
| `switch_core_port_allocator.c` | 262    | RTP port allocator (min_rtp_port..max_rtp_port range). |
| `switch_channel.c`             | 5,970  | Channel object: variables, flags, state, capabilities, private data. A channel is paired with a session. Every call variable you set via `<action application="set" data="X=Y"/>` ends up here. |
| `switch_ivr.c`                 | 4,550  | IVR primitives: `switch_ivr_park`, `switch_ivr_parse_event`, `switch_ivr_session_transfer`, `switch_ivr_uuid_kill`, grammar management. |
| `switch_ivr_async.c`           | 6,520  | Async IVR: `switch_ivr_broadcast`, `switch_ivr_eavesdrop_session`, `switch_ivr_displace_session`, speech/DTMF detection, recording. |
| `switch_ivr_bridge.c`          | 2,788  | `bridge` app implementation — B2BUA audio/video bridging loop. Default B2BUA behavior lives here. |
| `switch_ivr_originate.c`       | 5,251  | `originate` API — outbound leg creation, dial-string parsing, call-forwarding, group dial (`|`, `,`, `:_:`), early-media handling. |
| `switch_ivr_play_say.c`        | 3,684  | `playback`, `say`, `play_and_get_digits`, tone generation. |
| `switch_ivr_menu.c`            | 996    | IVR menu engine (`ivr` dialplan app). |
| `switch_ivr_say.c`             | 230    | Core say dispatcher (delegates to `mod_say_*` per locale). |
| `switch_rtp.c`                 | 12,017 | **RTP/SRTP implementation**. Packet send/receive, jitter buffer, RTCP-SR/RR/XR, DTLS, ICE/STUN integration, NAT keepalive. `mod_homer_rtcp` and other RTP hooks attach here. |
| `switch_stun.c`                | 1,279  | STUN protocol encoder/decoder. |
| `switch_xml.c`                 | 3,843  | XML parser (used by config, dialplan, directory, presence). |
| `switch_xml_config.c`          | 502    | XML configuration helper — `switch_xml_config_item_t` declarative config. |
| `switch_time.c`                | 2,514  | Time primitives, scheduler (`switch_scheduler_add_task`). |
| `switch_scheduler.c`           | 472    | Scheduled task runner (works with switch_time). |
| `switch_utils.c`               | 4,965  | Grab-bag utilities: hashing, cron, UUID, URL encode, IP parsing, MD5, Base64. |
| `switch_vpx.c`                 | 2,067  | VP8 codec glue. |
| `switch_loadable_module.c`     | 3,271  | Module loader — implements `load`, `unload`, `reload`, enumerates application/api/endpoint/codec/chat/say/directory/dialplan/speech/asr/file/timer interfaces. |
| `switch_pcm.c`                 | 1,121  | PCM raw codec plumbing, format conversion. |
| `switch_resample.c`            | 614    | Sample-rate conversion. |
| `switch_speex.c`               | 644    | Speex codec integration. |
| `switch_spandsp.c`             | 65     | SpanDSP glue stub. |
| `switch_utf8.c`                | 483    | UTF-8 handling. |
| `switch_regex.c`               | 331    | PCRE wrapper (`switch_regex_*`). |
| `switch_profile.c`             | 348    | CPU/memory profiling. |
| `switch_vad.c`                 | 267    | Voice activity detection. |
| `switch_swig.c`                | 294    | SWIG language-binding scaffolding. |
| `switch_version.c`             | 65     | Version string. |
| `switch_sdp.c`                 | 30     | SDP helper stubs (real logic in `switch_core_media.c`). |

Public headers: `src/include/switch.h` (283 lines — entry point). The public
API is split across `switch_types.h` (3,011 lines — all enums, typedefs,
status codes, event types), `switch_core.h` (3,017 lines — the `switch_core_*`
API), `switch_channel.h` (754 lines), `switch_ivr.h` (1,125 lines),
`switch_event.h` (478 lines), `switch_loadable_module.h` (628 lines),
`switch_apr.h` (1,498 lines — APR wrapper), plus a few dozen smaller headers.

Any `switch_*` function referenced from sibling Telnyx modules is declared in
one of these.

### `src/mod/` — module subdirectories (complete census)

#### Applications — `src/mod/applications/` (61 modules)
```
mod_abstraction         mod_av                  mod_avmd
mod_bert                mod_blacklist           mod_callcenter
mod_cidlookup           mod_cluechoo            mod_commands
mod_conference          mod_curl                mod_cv
mod_db                  mod_directory           mod_distributor
mod_dptools             mod_easyroute           mod_enum
mod_esf                 mod_esl                 mod_expr
mod_fax                 mod_fifo                mod_fsk
mod_fsv                 mod_hash                mod_http_cache
mod_httapi              mod_ladspa              mod_lcr
mod_limit               mod_memcache            mod_mongo
mod_mp4                 mod_nibblebill          mod_oreka
mod_osp                 mod_png                 mod_rad_auth
mod_redis               mod_rss                 mod_sangoma_codec
mod_signalwire          mod_sms                 mod_snapshot
mod_snom                mod_sonar               mod_soundtouch
mod_spandsp             mod_spy                 mod_stress
mod_test                mod_translate           mod_valet_parking
mod_video_filter        mod_voicemail           mod_voicemail_ivr
mod_xml_interfaces
```

Key ones:
- **`mod_commands`** — registers 150 core APIs. fs_cli / ESL commands (`originate`, `uuid_kill`, `show channels`, `show calls`, `fsctl`, `reload`, `reloadxml`, `bgapi`, `expand`, `find_user_xml`, `hupall`, `regex`, `replace`, etc.).
- **`mod_dptools`** — registers 141 dialplan apps (`bridge`, `playback`, `answer`, `hangup`, `set`, `export`, `execute_extension`, `ivr`, `conference`, `record_session`, `eavesdrop`, `att_xfer`, `deflect`, `early_hangup`, `transfer`, `park`, `answer`, `pre_answer`, `ring_ready`, etc.). Most calls run mostly through mod_dptools apps.
- **`mod_conference`** — multi-party conferencing engine (video composition included).
- **`mod_httapi`** — HTTP API server (external control via REST-ish calls).
- **`mod_curl`** — outbound HTTP from dialplan.
- **`mod_fifo`** — FIFO queue management.
- **`mod_voicemail`** / `mod_voicemail_ivr` — mailbox app.
- **`mod_callcenter`** — ACD / contact center queuing.
- **`mod_http_cache`** — HTTP cache for `playback http://...`.

#### ASR/TTS — `src/mod/asr_tts/`
```
mod_cepstral            mod_flite               mod_pocketsphinx
mod_tts_commandline     mod_unimrcp  (upstream — Telnyx also has its own mod_unimrcp dropped on top at build time)
```

#### Codecs — `src/mod/codecs/` (22 modules)
```
mod_amr                 mod_amrwb               mod_b64
mod_bv                  mod_clearmode           mod_codec2
mod_com_g729            mod_dahdi_codec         mod_g723_1
mod_g729                mod_h26x                mod_ilbc
mod_isac                mod_mp4v                mod_opus
mod_png                 mod_silk                mod_siren
mod_skel_codec          mod_speex               mod_theora
mod_vpx
```
Sibling Telnyx codec modules (`mod_bcg729`, `mod_evs`) are dropped in at build
time from separate repos.

#### Databases — `src/mod/databases/`
```
mod_db  (SQLite)        mod_mariadb             mod_pgsql
```

#### Dialplans — `src/mod/dialplans/`
```
mod_dialplan_asterisk   mod_dialplan_directory  mod_dialplan_xml
```
`mod_dialplan_xml` is the default and is what b2bua uses. Looks up
`<context name="...">` / `<extension>` / `<condition>` / `<action>` in the
XML tree served by either static files or `mod_xml_http` / `mod_xml_curl`.

#### Directories — `src/mod/directories/`
```
mod_ldap
```

#### Endpoints — `src/mod/endpoints/`
```
mod_sofia               ← THE SIP endpoint. ~35,454 lines. See §mod_sofia below.
mod_verto               ← WebRTC (Verto protocol)
mod_rtc                 ← WebRTC (older)
mod_rtmp                ← RTMP
mod_skinny              ← Cisco SCCP
mod_loopback            ← Internal loop (test/loopback endpoint)
mod_portaudio           ← PortAudio soundcard (console audio)
mod_reference           ← Template / reference endpoint
mod_alsa                ← ALSA soundcard (Linux)
mod_gsmopen             ← GSM modem
mod_h323                ← H.323 (legacy)
mod_khomp               ← Khomp hardware
mod_opal                ← Opal stack (H.323/SIP)
mod_skypopen            ← Skype
mod_unicall             ← Telephony signaling
```

Sibling Telnyx WebRTC modules (`mod_telnyx_rtc`, `mod_telnyx_video`) are
dropped in at build time.

#### Event handlers — `src/mod/event_handlers/`
```
mod_amqp                mod_cdr_csv             mod_cdr_mongodb
mod_cdr_pg_csv          mod_cdr_sqlite          mod_erlang_event
mod_event_multicast     mod_event_socket        mod_event_test
mod_fail2ban            mod_json_cdr            mod_kazoo
mod_radius_cdr          mod_rayo                mod_smpp
mod_snmp
```
Most important:
- **`mod_event_socket`** (ESL) — external TCP interface on port 8021 for
  programmatic control. `mod_telnyx_rtc` builds on this pattern.
- **`mod_json_cdr`** — posts call detail records over HTTP as JSON.

#### Formats — `src/mod/formats/`
```
mod_local_stream        mod_native_file         mod_opusfile
mod_portaudio_stream    mod_shell_stream        mod_shout
mod_sndfile             mod_tone_stream
```

#### Languages — `src/mod/languages/`
```
mod_java                mod_lua                 mod_managed
mod_perl                mod_python              mod_v8
mod_yaml
```
`mod_lua` is the most commonly used for dialplan logic in FS installs.

#### Loggers — `src/mod/loggers/`
```
mod_console             mod_graylog2            mod_logfile
mod_syslog
```

#### Say — `src/mod/say/` (per-locale say implementations)
`mod_say_de mod_say_en mod_say_es mod_say_fa mod_say_fr mod_say_he mod_say_hr mod_say_hu mod_say_it mod_say_ja mod_say_nl mod_say_pl mod_say_pt mod_say_ru mod_say_sv mod_say_th mod_say_zh`

#### Timers — `src/mod/timers/`
```
mod_timerfd  (Linux timerfd, default)
mod_posix_timer
mod_zrtp  (ZRTP timing)
```

#### SDK — `src/mod/sdk/`
`autoload_template` + `auth/mod_skeleton_auth` — starter skeletons for
module authors.

#### XML int — `src/mod/xml_int/`
```
mod_xml_cdr             mod_xml_curl            mod_xml_ldap
mod_xml_radius          mod_xml_rpc             mod_xml_scgi
```

### Module ABI
Every loadable module registers via:
```c
SWITCH_MODULE_DEFINITION(mod_<name>, mod_<name>_load, mod_<name>_shutdown, mod_<name>_runtime);
SWITCH_MODULE_LOAD_FUNCTION(mod_<name>_load) {
    switch_application_interface_t *app_interface;
    switch_api_interface_t *api_interface;
    *module_interface = switch_loadable_module_create_module_interface(pool, modname);
    SWITCH_ADD_APP(app_interface, "app_name", "short", "long", app_callback, "syntax", SAF_...);
    SWITCH_ADD_API(api_interface, "api_name", "description", api_callback, "syntax");
    return SWITCH_STATUS_SUCCESS;
}
```
Defined in `switch_loadable_module.h:628`. Loader lives in `switch_loadable_module.c:3,271`.

Other registration helpers:
- `switch_loadable_module_set_protocol` — chat/im
- `switch_core_add_codec` / `switch_core_codec_set_private` — codec
- `switch_loadable_module_set_interface` — generic interface injection
- `switch_core_speech_init` / `switch_core_asr_init` — speech engines

Module interface pointers visible in `switch_loadable_module_interface_t`:
`module_interface`, `application_interface`, `api_interface`, `endpoint_interface`,
`codec_interface`, `chat_interface`, `say_interface`, `directory_interface`,
`file_interface`, `speech_interface`, `asr_interface`, `dialplan_interface`,
`timer_interface`, `management_interface`, `limit_interface`.

## `libs/` — vendored libraries built in-tree
| Path | Library | Purpose |
|---|---|---|
| `libs/apr`           | Apache Portable Runtime | Cross-platform threads, files, pools, sockets, mutexes, rwlocks. The foundation of FS concurrency. |
| `libs/esl`           | Event Socket Library | C client for external apps to talk to `mod_event_socket` port 8021. |
| `libs/iksemel`       | XMPP helper            | XML + presence/chat protocol support. |
| `libs/libteletone`   | DTMF/tone engine       | `gentones`, DTMF detect/emit. |
| `libs/libnatpmp`     | NAT-PMP                | NAT port mapping. |
| `libs/miniupnpc`     | UPnP                   | NAT-UPnP mapping. |
| `libs/srtp`          | libsrtp                | SRTP + DTLS-SRTP. Used by `switch_rtp.c`. |
| `libs/xmlrpc-c`      | XML-RPC over HTTP      | Used by `mod_xml_rpc`. |
| `libs/libscgi`       | SCGI                   | `mod_xml_scgi`. |
| `libs/freetype`      | Font rendering         | Video overlays. |

External Telnyx-owned libs (NOT here — separate sibling repos pulled by
`fs_docker_deps`): `sofia-sip`, `spandsp`, `bcg729`, `libhdsp`,
`lib3gpp-evs-fp`, `rnnoise-internal`, `soundtouch`, `libks`, `unimrcp`,
`IXWebSocket`.

## Key concepts reference

### Data structures
- **`switch_core_session_t`** — one call leg. Owns an APR memory pool
  (every allocation with `switch_core_session_alloc` lives until the session
  is destroyed). Holds a channel, a read/write codec pair, media handle,
  and one dedicated thread for state-machine execution.
- **`switch_channel_t`** — associated with a session. Holds channel
  variables (key/value strings), flags (`CF_*` bit set — e.g.
  `CF_ANSWERED`, `CF_BRIDGED`, `CF_ENABLE_RTCP_PROBE`), state machine
  cursor, capabilities.
- **`switch_rtp_t`** — one RTP session (audio or video, each direction).
  Opaque to callers; manipulated via `switch_rtp_*` functions. Owned by the
  media handle; attached to / detached from the session at activate/destroy.
- **`switch_frame_t`** — a media frame (PCM samples or encoded packet +
  timestamp + flags).
- **`switch_memory_pool_t`** — APR pool. Allocations are bulk-freed when
  the pool is destroyed. FS creates pools for the core, per-module, and
  per-session.
- **`switch_event_t`** — one event on the event bus. Has a type
  (`switch_event_types_t`), subclass, headers (key/value), optional body.
- **`switch_xml_t`** — XML DOM handle.
- **`switch_codec_t`** / **`switch_codec_implementation_t`** — codec runtime.
- **`switch_status_t`** — enum: `SWITCH_STATUS_SUCCESS`, `_FALSE`,
  `_TIMEOUT`, `_RESTART`, `_NOTFOUND`, `_UNLOAD`, `_NOUNLOAD`, `_IGNORE`,
  `_BREAK`, `_SOCKERR`, `_MORE_DATA`, `_NOT_INITALIZED`, `_TOO_SMALL`,
  `_TERM`, `_ABORT`, `_GENERR`, `_MEMERR`, `_XBREAK`, `_WINBREAK`, `_FOUND`.

### Channel state machine (`switch_core_state_machine.c`)
```
CS_NEW           Just allocated — not yet routed
CS_INIT          Initialized, entering state machine
CS_ROUTING       Looking up dialplan / deciding what app to execute
CS_CONSUME_MEDIA Media-engine owned; typically when the channel is being
                 run by an outbound dialplan
CS_EXECUTE       Running the next dialplan app (playback, bridge, etc.)
CS_EXCHANGE_MEDIA Actively exchanging media (bridged)
CS_PARK          Parked (suspended — doing nothing, waiting for external
                 unpark or event)
CS_HANGUP        Hangup requested; cleanup in progress
CS_REPORTING     Firing CDR/reporting hooks
CS_DESTROY       About to be freed
```
Each state has `state_handler_*` callbacks registered by
endpoints/applications that fire as the channel transitions in/out of it.

### Event types (`switch_event_types_t` in `switch_types.h`)
`SWITCH_EVENT_CUSTOM`, `_CLONE`, `_CHANNEL_CREATE`, `_CHANNEL_DESTROY`,
`_CHANNEL_STATE`, `_CHANNEL_CALLSTATE`, `_CHANNEL_ANSWER`, `_CHANNEL_HANGUP`,
`_CHANNEL_HANGUP_COMPLETE`, `_CHANNEL_EXECUTE`, `_CHANNEL_EXECUTE_COMPLETE`,
`_CHANNEL_HOLD`, `_CHANNEL_UNHOLD`, `_CHANNEL_BRIDGE`, `_CHANNEL_UNBRIDGE`,
`_CHANNEL_PROGRESS`, `_CHANNEL_PROGRESS_MEDIA`, `_CHANNEL_OUTGOING`,
`_CHANNEL_PARK`, `_CHANNEL_UNPARK`, `_CHANNEL_APPLICATION`,
`_CHANNEL_ORIGINATE`, `_CHANNEL_UUID`, `_API`, `_LOG`, `_INBOUND_CHAN`,
`_OUTBOUND_CHAN`, `_STARTUP`, `_SHUTDOWN`, `_PUBLISH`, `_UNPUBLISH`, `_TALK`,
`_NOTALK`, `_SESSION_CRASH`, `_MODULE_LOAD`, `_MODULE_UNLOAD`, `_DTMF`,
`_MESSAGE`, `_PRESENCE_IN`, `_NOTIFY_IN`, `_PRESENCE_OUT`, `_PRESENCE_PROBE`,
`_MESSAGE_WAITING`, `_MESSAGE_QUERY`, `_ROSTER`, `_CODEC`, `_BACKGROUND_JOB`,
`_DETECTED_SPEECH`, `_DETECTED_TONE`, `_PRIVATE_COMMAND`, `_HEARTBEAT`,
`_TRAP`, `_ADD_SCHEDULE`, `_DEL_SCHEDULE`, `_EXE_SCHEDULE`, `_RE_SCHEDULE`,
`_RELOADXML`, `_NOTIFY`, `_PHONE_FEATURE`, `_PHONE_FEATURE_SUBSCRIBE`,
`_SEND_MESSAGE`, `_RECV_MESSAGE`, `_REQUEST_PARAMS`, `_CHANNEL_DATA`,
`_GENERAL`, `_COMMAND`, `_SESSION_HEARTBEAT`, `_CLIENT_DISCONNECTED`,
`_SERVER_DISCONNECTED`, `_SEND_INFO`, `_RECV_INFO`, `_RECV_RTCP_MESSAGE`,
`_SEND_RTCP_MESSAGE`, `_CALL_SECURE`, `_NAT`, `_RECORD_START`, `_RECORD_STOP`,
`_PLAYBACK_START`, `_PLAYBACK_STOP`, `_CALL_UPDATE`, `_FAILURE`,
`_SOCKET_DATA`, `_MEDIA_BUG_START`, `_MEDIA_BUG_STOP`, `_CONFERENCE_DATA_QUERY`,
`_CONFERENCE_DATA`, `_CALL_SETUP_REQ`, `_CALL_SETUP_RESULT`, `_CALL_DETAIL`,
`_DEVICE_STATE`, `_ALL`.

Consumers bind via `switch_event_bind(id, event_type, subclass, callback, user_data)`. `mod_event_socket`, `mod_json_cdr`, `mod_amqp`, etc. all subscribe. `_ALL` means subscribe to every type.

### Core APIs exposed by `mod_commands` (partial list of the 150)
Most-used:
- `originate`, `uuid_kill`, `uuid_break`, `uuid_park`, `uuid_bridge`,
  `uuid_transfer`, `uuid_setvar`, `uuid_getvar`, `uuid_hold`, `uuid_answer`,
  `uuid_pre_answer`, `uuid_display`, `uuid_record`, `uuid_broadcast`,
  `uuid_deflect`, `uuid_debug_media`, `uuid_dump`, `uuid_media`,
  `uuid_session_heartbeat`, `uuid_fileman`
- `show channels`, `show calls`, `show registrations`, `show codec`,
  `show modules`, `show tasks`, `show status`, `show api`
- `fsctl` — core control (`fsctl loglevel ...`, `fsctl shutdown`,
  `fsctl reclaim_mem`, `fsctl sync_clock`, `fsctl pause`, `fsctl resume`)
- `reload <modname>`, `load <modname>`, `unload <modname>`, `reloadxml`, `reloadacl`
- `bgapi <cmd>` — async API (returns job UUID via `BACKGROUND_JOB` event)
- `expand` — apply channel-variable substitution (`${var}`)
- `hupall`, `global_getvar`, `global_setvar`
- `status`, `version`, `help`, `memory`, `pool_stats`, `timer_test`, `regex`

### Dialplan apps exposed by `mod_dptools` (partial list of the 141)
Most-used:
- `answer`, `pre_answer`, `ring_ready`, `hangup`, `bridge`,
  `bridge_export`, `set`, `export`, `unset`, `set_global`,
  `set_profile_var`, `set_zombie_exec`
- `playback`, `endless_playback`, `preferred_codec`, `record`,
  `record_session`, `stop_record_session`
- `park`, `unpark`, `park_state`, `valet_park`, `valet_park_answer`
- `execute_extension`, `transfer`, `deflect`, `att_xfer`, `intercept`
- `ivr`, `send_display`, `send_info`, `send_dtmf`, `queue_dtmf`, `flush_dtmf`
- `detect_speech`, `detect_audio`, `detect_silence`, `play_and_detect_speech`
- `fax_detect`, `tone_detect`, `block_dtmf`
- `eavesdrop`, `displace_session`, `spy`, `three_way`
- `capture`, `capture_text`, `loop_playback`
- `system`, `bgsystem`, `lua`, `perl`, `python`, `phrase`
- `hold`, `unhold`, `info`, `verbose_events`, `enable_heartbeat`,
  `enable_keepalive`, `session_loglevel`

### Channel flags (`CF_*` — common ones)
`CF_ANSWERED`, `CF_BRIDGED`, `CF_CONSUME_ON_ORIGINATE`, `CF_DIALPLAN`,
`CF_ORIGINATOR`, `CF_HOLD`, `CF_TRANSFER`, `CF_PROXY_MEDIA`, `CF_OUTBOUND`,
`CF_INBOUND`, `CF_LOCKED`, `CF_EARLY_MEDIA`, `CF_HAVE_MEDIA`,
`CF_MEDIA_ACTIVE`, `CF_REINVITE`, `CF_RECOVERED`, `CF_VIDEO_READY`,
`CF_ENABLE_RTCP_PROBE`. Full set in `switch_types.h`.

### Call flow — an incoming SIP INVITE
1. **Sofia receives packet** — `mod_sofia/sofia.c` → Sofia-SIP stack → `sofia_handle_sip_i_invite` is invoked.
2. **Session allocation** — `switch_core_session_request` allocates a new session + channel + memory pool.
3. **SDP parse / codec pick** — `mod_sofia/sofia_glue.c::sofia_glue_negotiate_sdp` → `switch_core_media.c::switch_core_media_choose_port`.
4. **Channel variables populated** — `sip_from_uri`, `sip_to_uri`, `sip_call_id`, `sip_contact_uri`, `Caller-Channel-Name`, destination number from the Request-URI, SIP headers as `sip_h_X-...`.
5. **State transition CS_NEW → CS_INIT → CS_ROUTING** — state machine runs.
6. **CS_ROUTING**: `mod_dialplan_xml` (or other active dialplan module) walks the XML tree for `<context><extension><condition>` matching, emits `<action application="..." data="..."/>` to execute.
7. **CS_EXECUTE**: each action is pushed into the execution stack; `switch_core_session_execute_application` fires the app (e.g. `bridge user/100`).
8. **bridge creates B-leg** — `switch_ivr_originate.c::switch_ivr_originate` starts the outbound leg; `switch_ivr_bridge.c` runs the audio/video forwarding loop once both legs are answered.
9. **CS_EXCHANGE_MEDIA** — packets flow both directions via `switch_rtp.c` read/write.
10. **BYE received** — sofia `handle_sip_i_bye` → session goes to CS_HANGUP → cleanup → CS_REPORTING (fires CDRs) → CS_DESTROY.

### RTP hooks (relevant to `mod_homer_rtcp`, Telnyx audio stack)
- `switch_rtp_set_create_probe(callback)` — called once when any RTP session becomes active
- `switch_rtp_set_rtcp_probe(rtp, callback)` — per-session RTCP packet hook
- `switch_rtp_set_incoming_callback(rtp, callback)` — per-session inbound RTP packet hook
- `switch_rtp_add_dtmf(rtp, dtmf)` — inject DTMF
- `CF_ENABLE_RTCP_PROBE` channel flag enables RTCP probe emission

### Session locking model
Sessions are protected by an APR rwlock. Getting a session pointer by UUID:
```c
if ((session = switch_core_session_locate(uuid))) {
    // session is now read-locked
    ...
    switch_core_session_rwunlock(session);  // REQUIRED
}
```
Forgetting the `rwunlock` leaks the reference and the session can never
be destroyed → memory creep. Equivalent: `switch_core_session_read_lock`,
`switch_core_session_write_lock`. When working with bridged partners, use
`switch_channel_get_partner_uuid()` to walk to the other leg, then
`switch_core_session_locate` + unlock.

### SDP / media negotiation
Owned by `switch_core_media.c`. Key functions:
- `switch_core_media_negotiate_sdp` — parse offer, pick codecs, build answer
- `switch_core_media_choose_port` — allocate RTP port from configured range
- `switch_core_media_activate_rtp` — create the `switch_rtp_t` and start I/O
- `switch_core_media_set_codec` — apply chosen codec to the session

Codec preference comes from:
1. `inbound-codec-prefs` / `outbound-codec-prefs` in the Sofia profile XML
2. Channel var `codec_string` if set
3. Global `global-codec-prefs` in `switch.conf.xml`

### Database layer
- Core recovery state: `$${db_dir}/core.db` (SQLite by default) — used by
  `mod_core_recovery` to replay calls after crash.
- Alternative: `mod_pgsql` / `mod_mariadb` can back core/recovery state.
- Channel variables are NOT persisted — they live in the channel's APR pool
  and die with the session.
- Individual modules often maintain their own sqlite (voicemail, callcenter,
  fifo).

### Media bug framework
`switch_core_media_bug_add(session, name, callback, user_data, stop_time, flags)`
attaches an inline audio/video tap to a session. Used by:
- `record_session` dialplan app (dumps to WAV)
- `mod_audio_stream` (Telnyx — streams audio over WebSocket)
- `mod_gstt`, `mod_unimrcp` (speech detection)
- `mod_siprec_src` (Telnyx SIPREC)
- `mod_spandsp` (fax tones)

The callback receives each frame and can read/write/substitute it.

### Configuration loading pattern
Modules typically load XML config in their `LOAD` function:
```c
switch_xml_t cfg, xml, settings, param;
if (!(xml = switch_xml_open_cfg("mymod.conf", &cfg, NULL))) {
    return SWITCH_STATUS_TERM;
}
if ((settings = switch_xml_child(cfg, "settings"))) {
    for (param = switch_xml_child(settings, "param"); param; param = param->next) {
        const char *name = switch_xml_attr(param, "name");
        const char *value = switch_xml_attr(param, "value");
        // apply
    }
}
switch_xml_free(xml);
```
`switch_xml_open_cfg` walks the XML search path (typically
`conf/autoload_configs/<cfg>`). Reload is typically handled by subscribing
to `SWITCH_EVENT_RELOADXML`.

### Logging
`switch_log_printf(channel, level, fmt, ...)` where channel is one of:
- `SWITCH_CHANNEL_LOG`, `SWITCH_CHANNEL_LOG_CLEAN`
- `SWITCH_CHANNEL_SESSION_LOG(session)` — tags with UUID automatically
- `SWITCH_CHANNEL_UUID_LOG(uuid)`
- `SWITCH_CHANNEL_ID_SESSION` (numeric)

Levels: `SWITCH_LOG_CONSOLE`, `_DEBUG10`..`_DEBUG1`, `_DEBUG`, `_INFO`,
`_NOTICE`, `_WARNING`, `_ERROR`, `_CRIT`, `_ALERT`, `_EMERG`.

Log channel routing is configured in `switch.conf.xml` (`console-log-level`,
`log-date-format`) + `mod_logfile.conf.xml` / `mod_console.conf.xml`.

## `mod_sofia` — deep dive

`src/mod/endpoints/mod_sofia/` — the SIP endpoint. ~35,454 lines across:

| File | Lines | Role |
|---|---|---|
| `mod_sofia.c`       | … | Module entry, `SWITCH_MODULE_LOAD_FUNCTION`, API/app registration, profile management |
| `sofia.c`           | … | Sofia-SIP event loop, `sofia_handle_sip_i_invite`, `_bye`, `_cancel`, `_info`, `_message`, `_notify`, `_options`, `_refer`, `_register`, `_update` |
| `sofia_glue.c`      | … | Glue between Sofia-SIP events and FS channel state. SDP negotiation entry. |
| `sofia_presence.c`  | 5,122 | Presence / subscribe / notify / message-waiting |
| `sofia_reg.c`       | 3,829 | REGISTER processing, registration DB, digest auth, challenge |
| `sofia_media.c`     | 230 | Media helpers |
| `sofia_json_api.c`  | 229 | JSON API surface |
| `mod_sofia.h`       | … | Internal types (`private_object_t`, `sofia_profile_t`, `sofia_gateway_t`) |

Profiles are loaded from `<profiles>` in `sofia.conf.xml`. Each profile has
its own SIP listener (IP:port), NAT settings, codec prefs, TLS cert,
authentication rules, and a set of gateways (upstream SIP peers to REGISTER
against).

Sofia-SIP internals live in the sibling `team-telnyx/sofia-sip` fork,
vendored into `fs_docker_deps` and installed via `libsofia-sip-ua.so`.

## Build system

### Autotools flow
1. `./bootstrap.sh` — regenerates `configure` from `configure.ac` via autoreconf. Needs autoconf, automake, libtool.
2. `./configure [flags]` — detects deps (APR, Sofia-SIP, SpanDSP, libpthread, libcurl, libssl, libspeex, libsqlite, libcrypto, libsndfile, libjpeg, libfreetype, etc.), picks which modules to build (from `modules.conf`).
3. `make` — builds `libfreeswitch.la`, `freeswitch` binary, all enabled modules.
4. `make install` — installs into `--prefix` (default `/usr/local/freeswitch`).
5. `make install-strip` — installs with debug symbols stripped.

### How Telnyx builds it (`telnyx_b2bua_builder/Makefile`)
1. `git clone -b <FS_BRANCH> https://github.com/team-telnyx/freeswitch.git` — FS_BRANCH is auto-detected: prefers `telnyx/telephony/<push-branch>` on a deploy/release branch, falls back to `telnyx/telephony/master`.
2. For each sibling Telnyx module: `git clone -b <branch> https://github.com/team-telnyx/mod_<x>.git freeswitch/src/mod/<category>/mod_<x>` — drops modules directly into the tree.
3. `cd freeswitch && ./bootstrap.sh && ./configure --enable-... && make && make install-strip`
4. Packages the result into `telnyx_b2bua-<FS_VERSION>-<TELNYX_REV>.deb` via the Debian packaging in `debian/`.
5. The deb is what `b2bua` and `b2bua-rtc` Dockerfiles install via `apt-get install`.

### Upstream Docker dirs (UNUSED by Telnyx)
- `docker/base_image/Dockerfile` — minimal `FROM scratch`
- `docker/master/Dockerfile` — `FROM debian:bookworm` building master for dev testing
- `docker/release/Dockerfile` — `FROM debian:jessie` installing old upstream `freeswitch-all` deb

Telnyx uses its own Dockerfiles in `telnyx_b2bua_builder` + `freeswitch_docker_base`.

## GHA workflows
- **`reviewpr.yml`** — `team-telnyx/reviewpr-internal@main` AI-powered PR review on every PR open/reopen/synchronize. Secret: `OPENAI_API_KEY`. Model: `gpt-5.2-codex`.
- **`trigger-b2bua-builder.yml`** — mirrors the pattern of every `mod_*` repo:
  - Trigger: push / PR to `master | main | deploy-* | release-*`, plus `delete` events
  - Job "Create branch in downstream repos" (on push to `deploy-*`/`release-*`): creates same-named branch in **`telnyx_b2bua_builder`, `b2bua`, AND `b2bua-rtc`** (this repo syncs to all three; many `mod_*` repos only sync to `b2bua`)
  - Job "Delete branch in downstream repos" (on delete of those branches, except `deploy-development`): deletes the branches
  - Job "Trigger upstream build" (on every push / PR): dispatches `upstream-build` to `telnyx_b2bua_builder` with `{ branch, source_repo, run_url }` payload
  - Secrets: `GIT_TOKEN`, `OPENAI_API_KEY`

## Jenkinsfile
Present for legacy compatibility. GHA is the primary CI path.

## Active Telnyx branch naming
- `telnyx/telephony/master`                — default integration target
- `telnyx/telephony/deploy-development`    — dev cluster
- `telnyx/telephony/deploy-<cluster>`      — per-cluster prod deploys
- `telnyx/telephony/release-<tag>`         — release cuts
- `telnyx/bug/<TICKET>`                    — one-off bugfix branches
- `telnyx/feature/<TICKET>`                — feature branches
- `deploy-jira-<ticket>`                   — short-lived deploy branches (newer convention)

PRs typically target `telnyx/telephony/deploy-development` for faster
dev-cluster iteration; merges to `telnyx/telephony/master` happen after soak.

## Integration with sibling Telnyx modules
At build time `telnyx_b2bua_builder` drops these into the tree:

| Sibling repo | Dropped into | Role |
|---|---|---|
| `mod_telnyx`               | `src/mod/applications/`    | Core Telnyx tooling APIs |
| `mod_call_control`         | `src/mod/applications/`    | Call-control API + Telnyx dialplan apps |
| `mod_call_recovery`        | `src/mod/applications/`    | Recovery after crash/failover |
| `mod_q2s_mapping`          | `src/mod/applications/`    | Q.850 ↔ SIP code mapping |
| `mod_backtrace`            | `src/mod/applications/`    | Crash backtrace hook |
| `mod_xml_http`             | `src/mod/xml_int/`         | Telnyx XML-HTTP dialplan dispatcher (more featureful than upstream `mod_xml_curl`) |
| `mod_happy_voicemail`      | `src/mod/applications/`    | Voicemail |
| `mod_dynamic_gateway`      | `src/mod/applications/`    | Dynamic gateway registration |
| `mod_homer_rtcp`           | `src/mod/applications/`    | Homer RTCP/HEP forwarding |
| `mod_denoiser`             | `src/mod/applications/`    | Noise suppression |
| `mod_krisp_nc`             | `src/mod/applications/`    | Krisp NC |
| `mod_deep_filter_net`      | `src/mod/applications/`    | DeepFilterNet NS |
| `mod_ai_acoustics`         | `src/mod/applications/`    | ai-coustics SDK |
| `mod_audio_stream-src`     | `src/mod/applications/mod_audio_stream` | Audio-stream over WebSocket |
| `mod_siprec_src`           | `src/mod/applications/mod_siprec`      | SIPREC source |
| `mod_polly`                | `src/mod/applications/`    | AWS Polly TTS |
| `mod_gstt`                 | `src/mod/applications/`    | Google STT |
| `mod_unimrcp` (Telnyx)     | overlays upstream mod_unimrcp | MRCP ASR/TTS (Telnyx-patched) |
| `mod_telnyx_rtc`           | `src/mod/endpoints/`       | Telnyx WebRTC endpoint |
| `mod_telnyx_video`         | `src/mod/endpoints/mod_janus` | Janus audio/video bridge |
| `mod_bcg729`               | `src/mod/codecs/`          | G.729 (Telnyx-patched bcg729) |
| `mod_evs`                  | `src/mod/codecs/`          | 3GPP EVS codec |

## "Where is X?" quick index (for the agent)
| Question | Start here |
|---|---|
| Where is codec negotiation? | `src/switch_core_media.c::switch_core_media_negotiate_sdp` + mod_sofia's `sofia_glue.c::sofia_glue_negotiate_sdp` |
| Where is the bridge loop? | `src/switch_ivr_bridge.c` |
| Where is `originate`? | `src/switch_ivr_originate.c::switch_ivr_originate` |
| Where are SIP INVITEs handled? | `src/mod/endpoints/mod_sofia/sofia.c::sofia_handle_sip_i_invite` |
| Where is REGISTER handled? | `src/mod/endpoints/mod_sofia/sofia_reg.c` |
| Where are channel state transitions? | `src/switch_core_state_machine.c` |
| Where is the RTP write path? | `src/switch_rtp.c::switch_rtp_write_frame` (+ DTLS/SRTP wrappers) |
| Where are dialplan apps executed? | `src/switch_core_session.c::switch_core_session_execute_application` |
| Where are events fired / bound? | `src/switch_event.c` — `switch_event_fire`, `switch_event_bind` |
| Where is the XML dialplan resolved? | `src/mod/dialplans/mod_dialplan_xml/mod_dialplan_xml.c` |
| Where is the scheduler? | `src/switch_scheduler.c` + `src/switch_time.c` |
| Where are channel vars stored? | `src/switch_channel.c` — `switch_channel_set_variable*` / `switch_channel_get_variable*` |
| Where is the codec registry? | `src/switch_core_codec.c` — `switch_core_codec_add`, `switch_core_codec_init` |
| Where is UUID→session lookup? | `src/switch_core_session.c::switch_core_session_locate` |
| Where is the crash handler? | `src/switch_core.c` — signal handlers (ties to `mod_backtrace`) |
| Where is the core recovery code? | `src/mod/applications/mod_core_recovery` (NOTE: Telnyx also has `mod_call_recovery` dropped in) |
| Where is the ESL server? | `src/mod/event_handlers/mod_event_socket/mod_event_socket.c` (port 8021) |
| Where is media-bug attach/detach? | `src/switch_core_media_bug.c` |
| Where is `fs_cli`? | `clients/fs_cli/fs_cli.c` — links against `libesl` in `libs/esl/` |
| Where is ICE/DTLS? | `src/switch_rtp.c` + `src/switch_stun.c` |

## Rules
- Follow the 23 FreeSWITCH coding rules at
  `core-tel-ai-agents/core_tel_ai_agents/agents/fs_knowledge/system_prompt.py` §9.
- Module-specific style + memory/threading conventions are in
  `.llm-repo-instructions` — reference, don't duplicate here.
- This codebase is MASSIVE. When answering, prefer the "where is X?" index
  above + `fs_read_file` with a line range over trying to describe whole
  subsystems inline. Every `switch_*` function pointer is declared in
  `src/include/switch_*.h`; grep there first.
- Mutations to this repo rebuild the WHOLE b2bua deb — changes here have
  wide blast radius. Always read `.llm-repo-instructions` + the 23 rules
  before proposing code changes.
