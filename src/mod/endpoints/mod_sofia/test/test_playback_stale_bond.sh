#!/bin/sh

PATH=/opt/homebrew/bin:$PATH
export PATH

if ! command -v sipp >/dev/null 2>&1; then
	exit 77
fi

cd "$(dirname "$0")" || exit 1
FST_PLAYBACK_BOND_ONLY=1
export FST_PLAYBACK_BOND_ONLY
rc=0
./sipp-based-tests peer_hangup_does_not_stop_the_playback_of_a_later_bridge || rc=1
./sipp-based-tests peer_hangup_reports_no_cause_on_a_leg_that_outlives_the_bridge || rc=1
./sipp-based-tests peer_hangup_reports_a_cause_on_a_leg_that_only_parks_after_an_early_bridge || rc=1
./sipp-based-tests peer_hangup_reports_a_cause_on_a_leg_that_hangs_up_explicitly_after_the_bridge || rc=1
./sipp-based-tests peer_hangup_reports_a_cause_on_a_leg_whose_peer_never_answered || rc=1
exit $rc
