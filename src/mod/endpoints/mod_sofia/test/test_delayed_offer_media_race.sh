#!/bin/sh

PATH=/opt/homebrew/bin:$PATH
export PATH

if ! command -v sipp >/dev/null 2>&1; then
	exit 77
fi

cd "$(dirname "$0")" || exit 1
FST_DELAYED_OFFER_ONLY=1
export FST_DELAYED_OFFER_ONLY
rc=0
./sipp-based-tests delayed_offer_media_request_during_ack_wait || rc=1
./sipp-based-tests delayed_offer_duplicate_answer_during_ack_wait || rc=1
./sipp-based-tests early_offer_answer_and_media_during_prack_wait || rc=1
./sipp-based-tests ring_ready_early_offer_answer_during_prack_wait || rc=1
./sipp-based-tests ring_hook_pre_answer_during_prack_wait || rc=1
./sipp-based-tests pre_answer_hook_answers_on_prack || rc=1
./sipp-based-tests pre_answer_hook_runs_once_with_answer_waiting_on_prack || rc=1
exit $rc
