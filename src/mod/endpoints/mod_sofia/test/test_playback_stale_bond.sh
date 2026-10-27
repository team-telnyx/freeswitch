#!/bin/sh

PATH=/opt/homebrew/bin:$PATH
export PATH

if ! command -v sipp >/dev/null 2>&1; then
	exit 77
fi

cd "$(dirname "$0")" || exit 1
FST_PLAYBACK_BOND_ONLY=1
export FST_PLAYBACK_BOND_ONLY
./sipp-based-tests peer_hangup_does_not_stop_the_playback_of_a_later_bridge
