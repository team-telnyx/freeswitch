#!/usr/bin/env bash
#
# Build and run the T.38 gateway CNG regression test without autotools. It
# needs only a C compiler and libspandsp (headers and library).
#
#   test/run.sh                         # the two scripts and the sweep; exit status 0 = pass
#   test/run.sh late -v                 # one script, with every log line of the gateway
#   test/run.sh sweep                   # one line for each timing of the sweep
#   SPANDSP_PREFIX=/path test/run.sh    # test a different libspandsp build
set -eu

cd "$(dirname "$0")"
PREFIX="${SPANDSP_PREFIX:-/usr/local}"
CC="${CC:-cc}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

$CC -g -O1 -Wall -Wextra -I"$PREFIX/include" -o "$OUT/test_t38_gateway_cng" test_t38_gateway_cng.c -L"$PREFIX/lib" -lspandsp -lm
LD_LIBRARY_PATH="$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$OUT/test_t38_gateway_cng" "$@"
