#!/bin/sh

PATH=/opt/homebrew/bin:$PATH
export PATH

if ! command -v sipp >/dev/null 2>&1; then
	exit 77
fi

cd "$(dirname "$0")" || exit 1
FST_BLIND_TRANSFER_ONLY=1
export FST_BLIND_TRANSFER_ONLY
rc=0
./sipp-based-tests blind_transfer_caller_hangup_while_ringing || rc=1
exit $rc
