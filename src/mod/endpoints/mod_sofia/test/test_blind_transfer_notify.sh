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
./sipp-based-tests blind_transfer_early_media_then_answer || rc=1
./sipp-based-tests blind_transfer_early_media_then_busy || rc=1
./sipp-based-tests blind_transfer_caller_hangup_during_early_media || rc=1
./sipp-based-tests blind_transfer_caller_hangup_while_ringing || rc=1
./sipp-based-tests blind_transfer_referrer_moved_before_result || rc=1
./sipp-based-tests blind_transfer_referrer_reparked_before_result || rc=1
./sipp-based-tests blind_transfer_rerouted_then_second_target_answers || rc=1
./sipp-based-tests blind_transfer_rerouted_then_second_target_busy || rc=1
./sipp-based-tests blind_transfer_queued_transfer_to_park || rc=1
./sipp-based-tests blind_transfer_queued_app_resumes_bridge || rc=1
exit $rc
