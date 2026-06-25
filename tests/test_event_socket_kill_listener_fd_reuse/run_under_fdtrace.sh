#!/usr/bin/env bash
# Run the eslfdrace reproducer under fdsentry's bpftrace detector (fdtrace-comm.bt).
# Needs root (kernel tracepoints). Usage:
#   sudo bash run_under_fdtrace.sh        # BUGGY  -> expect [STALE-CLOSE]
#   sudo bash run_under_fdtrace.sh fix    # FIXED  -> detector stays silent
set -u

BT=/home/damir/work/fdsentry/fdtrace-comm.bt
DIR=/home/damir/work/freeswitch/tests/test_event_socket_kill_listener_fd_reuse
OUT=/tmp/eslfdrace-bt.out
MODE="${1:-}"

[ -x "$DIR/eslfdrace" ] || cc -O0 -g -pthread "$DIR/eslfdrace.c" -o "$DIR/eslfdrace"

echo "[*] arming bpftrace on comm 'eslfdrace' ..."
bpftrace "$BT" eslfdrace >"$OUT" 2>&1 &
BTPID=$!

# wait until probes are attached (BEGIN prints "armed")
for _ in $(seq 1 100); do grep -q "armed" "$OUT" 2>/dev/null && break; sleep 0.2; done

echo "[*] launching reproducer (mode=${MODE:-buggy}) ..."
"$DIR/eslfdrace" ${MODE:+"$MODE"}
sleep 1

kill -INT "$BTPID" 2>/dev/null
wait "$BTPID" 2>/dev/null

echo
echo "===================== bpftrace output ====================="
cat "$OUT"
