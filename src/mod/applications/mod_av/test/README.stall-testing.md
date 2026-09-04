# Testing recording against a stalled RTMP recorder

An overloaded RTMP recorder does not refuse connections — it accepts them and
then stops answering. Before the network timeout settings existed, the connect
and RTMP handshake ran on whichever thread called `switch_core_file_open()`,
which for a recording is the session thread, so the call lost its media for as
long as the recorder took to answer: dead air, and a channel that would not hang
up.

`test_avformat.c` covers the deterministic half of this (an unreachable address,
no server needed). This describes the half that needs a live call.

## Settings

`autoload_configs/av.conf.xml`, `avformat.conf` section — all default to 0/false,
i.e. the historical behaviour:

| setting | effect |
| --- | --- |
| `network-rw-timeout` | read/write timeout when the record command carries no `rw_timeout` of its own |
| `network-max-rw-timeout` | ceiling applied to a caller-supplied `rw_timeout` |
| `network-connect-timeout` | budget for establishing the connection |
| `network-async-open` | connect on the writer's thread instead of the caller's |

Any of the four can also be set for a single recording, which is how you tune or
disable this without restarting. Precedence is `{}` on the record command, then the
channel variable, then `av.conf.xml`:

| channel variable | record command param | unit |
| --- | --- | --- |
| `RECORD_NETWORK_CONNECT_TIMEOUT_MS` | `network_connect_timeout` | ms |
| `RECORD_NETWORK_RW_TIMEOUT_MS` | `network_rw_timeout` | ms |
| `RECORD_NETWORK_MAX_RW_TIMEOUT_MS` | `network_max_rw_timeout` | ms |
| `RECORD_NETWORK_ASYNC_OPEN` | `network_async_open` | bool |
| `RECORD_NETWORK_RESILIENCY` | `network_resiliency` | bool |

`RECORD_NETWORK_RESILIENCY=false` is the kill switch -- it turns off the timeouts and
the async open together for that recording. It is disable-only; setting it true where the
config has the feature off does nothing.

The individual settings are not disable-only. Each takes effect in either direction, so
`RECORD_NETWORK_ASYNC_OPEN=true` with a `RECORD_NETWORK_CONNECT_TIMEOUT_MS` gets the full
behaviour for one recording on a box that has the feature off -- which is how to try it on
a single call. A recording may also loosen a budget the config has set.

`network_max_rw_timeout` may only be *tightened* per recording. It is the ceiling that
stops a caller-supplied `rw_timeout` defeating the bound, so a looser value is refused
and logged rather than honoured.

Core-side, in `vars.xml` or per recording:

| variable | effect |
| --- | --- |
| `record_buffer_max_ms` / `RECORD_BUFFER_MAX_MS` | ceiling on audio queued for the recording thread |
| `record_close_timeout_ms` / `RECORD_CLOSE_TIMEOUT_MS` | how long the recording thread may make *no progress* at close before its I/O is aborted. A thread still draining its queue keeps the budget alive, so a slow-but-working recorder is not cut off; an overall cap of ten times this value stops trickling progress holding the channel up |
| `record_write_error_grace_ms` / `RECORD_WRITE_ERROR_GRACE_MS` | how long consecutive write failures are tolerated before the recording is abandoned. Defaults to 1000ms rather than off, because the alternative is retrying a dead destination on every frame. 0 gives up on the first failure; a value larger than any call effectively never gives up |

## Running it

Start the stand-in recorder:

    python3 test/blackhole_rtmp.py 11935 accept

Then place a call and start a recording against it, executing `record_session`
on the session thread the way production does (dialplan or `execute_on_*`,
`uuid_broadcast` being the easiest equivalent from the CLI):

    originate {origination_uuid=t1}loopback/park &playback(tone_stream://%(600000,0,440))
    uuid_record t1 start /tmp/probe.wav
    uuid_broadcast t1 record_session::rtmp://127.0.0.1:11935/live/stalled aleg

`/tmp/probe.wav` is the measurement: it is written by the same session thread,
so compare its duration against wall clock. Any shortfall is the media that the
call did not process — the dead air.

## What to expect

With everything off, the recording never connects and the call loses its media
entirely until the socket gives up; hangup blocks too.

With `network-connect-timeout` and `network-async-open` set, the probe should
track wall clock with no measurable gap, the recording should be abandoned
shortly after the connect budget expires, and hangup should be prompt. The
failed recording should log its error once rather than once per frame.
