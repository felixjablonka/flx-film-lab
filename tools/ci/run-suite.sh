#!/usr/bin/env bash
# Runs one build.sh test target for CI and reports each failed check as an
# annotation on the run, with the check's own message, instead of only "exit
# code 1". Exits with the suite's own status.
#
#   tools/ci/run-suite.sh checks

set -uo pipefail
cd "$(dirname "$0")/../.."
target="${1:?usage: tools/ci/run-suite.sh TARGET}"
log="$(mktemp)"

./build.sh "$target" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}

grep -E '^ +FAIL ' "$log" | sed -E "s/^ +FAIL (.*)/::error title=$target::\1/" || true
rm -f "$log"
exit "$status"
