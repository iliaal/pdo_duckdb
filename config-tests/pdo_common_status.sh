#!/usr/bin/env bash
# Exercise the common-suite wrapper without PHP, DuckDB, or php-src installed.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
harness="${1:-$repo/scripts/pdo-common-tests.sh}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir "$work/bin"

cat > "$work/bin/php" <<'PHP'
#!/usr/bin/env bash
if [ "${1:-}" = -m ]; then
    echo PDO
    exit 0
fi
if [ "${FAKE_EMPTY:-0}" = 0 ]; then
    cat <<'SUMMARY'
Tests borked    : 0
Tests leaked    : 0
FAILED TEST SUMMARY
bug 36798 [bug_36798.phpt]
bug 43130 [bug_43130.phpt]
=====
SUMMARY
fi
exit "${FAKE_PHP_STATUS:-1}"
PHP
cat > "$work/bin/tee" <<'TEE'
#!/usr/bin/env bash
# Preserve realistic output while simulating an output-file write failure.
cat > "$1"
cat "$1"
exit "${FAKE_TEE_STATUS:-0}"
TEE
chmod +x "$work/bin/php" "$work/bin/tee"

check() {
    local label="$1" expected="$2" php_status="$3" tee_status="$4" empty="$5"
    local status=0
    PATH="$work/bin:$PATH" PHP="$work/bin/php" RUN_TESTS="$work/run-tests.php" \
        COMMON_DIR="$work/common" EXT="$work/pdo_duckdb.so" \
        EXPECTED_FAILS='bug_36798.phpt bug_43130.phpt' \
        FAKE_PHP_STATUS="$php_status" FAKE_TEE_STATUS="$tee_status" FAKE_EMPTY="$empty" \
        bash "$harness" > "$work/output" 2>&1 || status=$?
    if [ "$status" -ne "$expected" ]; then
        echo "FAIL: $label (expected $expected, got $status)" >&2
        cat "$work/output" >&2
        exit 1
    fi
    echo "PASS: $label"
}

check 'allowlisted failures' 0 1 0 0
check 'allowlisted failures with REPORT_EXIT_STATUS disabled' 0 0 0 0
check 'fatal runner error after matching summary' 1 255 0 0
check 'terminated runner after matching summary' 1 143 0 0
check 'tee failure after matching summary' 1 1 1 0
check 'runner and tee failure after matching summary' 1 255 1 0
check 'runner failure without summary' 1 255 0 1
check 'missing expected failures' 1 0 0 1
