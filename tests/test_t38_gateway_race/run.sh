#!/usr/bin/env bash
#
# TELCORE-226: reproduce (and verify the fix for) the libspandsp t38_gateway
# crash / data race.
#
# Modes:
#   ./run.sh crash   - link against the installed libspandsp.so and just run.
#                      Segfaults with the exact production stack (span_log_test
#                      <- span_log <- hdlc_rx_status <- ... <- t38_gateway_rx).
#   ./run.sh tsan    - build a ThreadSanitizer-instrumented libspandsp from
#                      source and run under it; TSan reports the data race on
#                      hdlc_rx.frame_user_data between the audio leg and the
#                      T.38 leg (see tsan-report.txt for a captured run).
#   ./run.sh fixed   - same as tsan, but compiled with -DSERIALIZE so the two
#                      threads take a shared lock around every spandsp call
#                      (what the mod_spandsp fix does). TSan reports no race and
#                      the program completes cleanly.
#
# Override the spandsp source tree with SPANDSP_SRC=/path/to/spandsp.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
src="$here/t38_gateway_race.c"
SPANDSP_SRC="${SPANDSP_SRC:-$HOME/work/fs_docker_deps/spandsp}"
build="${TMPDIR:-/tmp}/telcore226-spandsp-tsan"
mode="${1:-tsan}"
TS="-fsanitize=thread -g -O1 -fno-omit-frame-pointer"

build_tsan_lib() {
    if [ ! -f "$build/src/.libs/libspandsp.a" ]; then
        echo ">>> building TSan-instrumented libspandsp (one-time) ..."
        [ -x "$SPANDSP_SRC/configure" ] || (cd "$SPANDSP_SRC" && ./autogen.sh)
        rm -rf "$build"; mkdir -p "$build"; cd "$build"
        "$SPANDSP_SRC/configure" --enable-static --disable-shared --disable-doc >/dev/null
        make -C src -j"$(nproc)" CFLAGS="$TS" libspandsp.la >/dev/null
    fi
}

case "$mode" in
crash)
    cc -O1 -g -I/usr/local/include "$src" -o "$here/gwrace" \
       -L/usr/local/lib -lspandsp -lpthread -lm
    echo ">>> running (expect SIGSEGV in span_log_test) ..."
    LD_LIBRARY_PATH=/usr/local/lib "$here/gwrace"
    ;;
tsan|fixed)
    [ "$mode" = fixed ] && def=-DSERIALIZE || def=
    out="$here/gwrace_${mode}"
    build_tsan_lib
    cc $def $TS -I"$build/src" -I/usr/local/include "$src" \
       "$build/src/.libs/libspandsp.a" -ltiff -ljpeg -lm -lpthread -o "$out"
    echo ">>> running under ThreadSanitizer (setarch -R works around the"
    echo "    'unexpected memory mapping' ASLR issue on newer kernels) ..."
    # halt_on_error=1: stop at the first race so the output is the smoking gun.
    TSAN_OPTIONS="halt_on_error=1 history_size=4" setarch -R "$out"
    ;;
*)
    echo "usage: $0 {crash|tsan|fixed}" >&2; exit 2;;
esac
