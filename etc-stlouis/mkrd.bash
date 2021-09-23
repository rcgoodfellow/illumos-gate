#! /bin/bash

set -eu
set -o pipefail

SIZE=13312

ramdisk="$("$(dirname "$0")/mkrd-helper.bash" -s "${SIZE}" 2>/dev/null)"

if [ $? != 0 -o -z "${ramdisk}" ]; then
	printf 'mkrd-helper.bash failed; run it manually to debug\n' >&2
	exit 1
else
	printf '%s\n' "${ramdisk}"
	exit 0
fi
