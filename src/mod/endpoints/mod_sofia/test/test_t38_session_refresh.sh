#!/bin/sh

PATH=/opt/homebrew/bin:$PATH
export PATH

if ! command -v sipp >/dev/null 2>&1; then
	exit 77
fi

cd "$(dirname "$0")" || exit 1
FST_T38_ONLY=1
export FST_T38_ONLY
exec ./sipp-based-tests t38_identical_session_refresh_keeps_image
