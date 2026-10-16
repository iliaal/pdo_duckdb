#!/usr/bin/env bash
# Fail if a required test is missing or was skipped. LOG is run-tests.php output
# produced with SKIP in -g, so skipped tests are listed.
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
    if awk -v t="[$t]" '
        {
            gsub(/\033\[[0-9;]*m/, "")
            n = split($0, lines, "\r")
            for (i = 1; i <= n; i++) {
                if (lines[i] ~ /^SKIP / && index(lines[i], t)) found = 1
            }
        }
        END { exit !found }
    ' "$log"; then
        echo "::error::Required test $t was skipped." >&2
        status=1
    fi
done
exit "$status"
