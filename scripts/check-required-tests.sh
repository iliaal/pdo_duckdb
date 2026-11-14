#!/usr/bin/env bash
# Require a PASS result for each required test. LOG is run-tests.php output
# produced with PASS and SKIP in -g, so both results are listed.
#
# Usage: scripts/check-required-tests.sh LOG tests/117_x.phpt [...]
set -euo pipefail
log="${1:?set the run-tests.php output file}"
shift
status=0
for t in "$@"; do
    if [ ! -f "$t" ]; then
        echo "::error::Required test $t does not exist." >&2
        status=1
        continue
    fi
    if ! awk -v t="[$t]" '
        {
            gsub(/\033\[[0-9;]*m/, "")
            n = split($0, lines, "\r")
            for (i = 1; i <= n; i++) {
                if (index(lines[i], t)) {
                    if (lines[i] ~ /^PASS /) passed = 1
                    if (lines[i] ~ /^SKIP /) skipped = 1
                }
            }
        }
        END { exit !(passed && !skipped) }
    ' "$log"; then
        echo "::error::Required test $t has no verified PASS result (missing, skipped, or unreadable log)." >&2
        status=1
    fi
done
exit "$status"
