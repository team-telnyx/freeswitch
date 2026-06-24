# TELCORE-226 — libspandsp t38_gateway crash reproducer

Standalone reproducer for the B2BUA segfault in `libspandsp` during T.38
gateway processing (crash in `span_log_test` ← `span_log` ← `hdlc_rx_status`).

## Root cause

In `mod_spandsp` gateway mode, a single `t38_gateway_state_t` is driven by **two
FreeSWITCH session threads with no mutex**:

| Thread        | FreeSWITCH handler                  | spandsp entry            |
|---------------|-------------------------------------|--------------------------|
| Audio leg     | `t38_gateway_on_consume_media`      | `t38_gateway_rx()`       |
| T.38 leg      | `t38_gateway_on_soft_execute`       | `udptl_rx_packet()` → `t38_core_rx_ifp_packet()` |

When an incoming T.38 control message (e.g. CFR) on the T.38 leg drives
`restart_rx_modem()`, it calls `hdlc_rx_init()` which `memset`s the `hdlc_rx`
struct and then restores `frame_user_data`. Meanwhile the audio leg is inside
`hdlc_rx_status()` reading that same `frame_user_data` and dereferencing it in
`span_log(&s->logging, ...)`. A torn read yields a non-NULL garbage pointer →
SIGSEGV in `span_log_test()` reading `s->level`.

## Running

```sh
./run.sh crash   # link the installed libspandsp.so, run -> SIGSEGV (production stack)
./run.sh tsan    # build TSan-instrumented libspandsp, run -> data-race report
./run.sh fixed   # same, but -DSERIALIZE (models the fix) -> no race, clean exit
```

`tsan` mode needs the spandsp source tree (default `~/work/fs_docker_deps/spandsp`,
override with `SPANDSP_SRC=...`). On newer kernels TSan needs `setarch -R` to avoid
the "unexpected memory mapping" ASLR failure; `run.sh` already does this.

## Captured evidence

`tsan-report.txt` contains a captured run: the race on
`hdlc_rx.frame_user_data` (write at `hdlc.c:389` in `hdlc_rx_init` from the T.38
leg vs. read at `t38_gateway.c:1722` in `hdlc_rx_status` from the audio leg), the
TSan-caught SEGV at the exact production crash line, and the full list of 122
distinct race sites on the shared state.

## Fix

Serialize access to the gateway state with the existing per-`pvt` mutex
(`pvt->mutex` in `mod_spandsp_fax.c`): take it around
`t38_gateway_rx`/`t38_gateway_rx_fillin`/`t38_gateway_tx` (audio leg, in
`t38_gateway_on_consume_media`) and `udptl_rx_packet` (T.38 leg, in
`t38_gateway_on_soft_execute`). This mirrors the lock already used for
terminal mode. `./run.sh fixed` validates the approach: the same workload
under `-DSERIALIZE` runs to completion with zero TSan reports.
