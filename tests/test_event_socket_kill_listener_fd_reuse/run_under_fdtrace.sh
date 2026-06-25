#!/usr/bin/env bash
# Run the eslfdrace reproducer under fdsentry's bpftrace detector (fdtrace-comm.bt).
# Needs root (kernel tracepoints). Usage:
#   sudo bash run_under_fdtrace.sh        # BUGGY  -> expect [STALE-CLOSE]
#   sudo bash run_under_fdtrace.sh fix    # FIXED  -> detector stays silent
set -u

# Path to fdsentry's fdtrace-comm.bt (lives in the separate fdsentry repo).
# Override with: FDSENTRY_BT=/path/to/fdtrace-comm.bt sudo -E bash run_under_fdtrace.sh
BT="${FDSENTRY_BT:-<foobar>/fdsentry/fdtrace-comm.bt}"
# This test directory, auto-detected from the script's own location.
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
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
