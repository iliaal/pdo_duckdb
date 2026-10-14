#!/usr/bin/env bash
# run-tests.php reports total tests, then executed tests (excluding SKIP/BORK).
# A successful process with only skipped tests is not runtime validation.
set -euo pipefail
log="${1:?set the run-tests.php output file}"
if ! awk '
    {
        gsub(/\033\[[0-9;]*m/, "")
        n = split($0, lines, "\r")
        for (i = 1; i <= n; i++) {
            if (lines[i] ~ /^Number of tests[[:space:]]*:[[:space:]]*[1-9][0-9]*[[:space:]]+[1-9][0-9]*[[:space:]]*$/) found = 1
        }
    }
    END { exit !found }
' "$log"; then
    echo '::error::Test harness did not complete a non-empty executed test run.' >&2
    exit 1
fi
