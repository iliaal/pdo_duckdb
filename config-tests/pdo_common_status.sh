#!/usr/bin/env bash
# Exercise the common-suite wrapper without PHP, DuckDB, or php-src installed.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
harness="${1:-$repo/scripts/pdo-common-tests.sh}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir "$work/bin" "$work/extensions"
touch "$work/extensions/pdo.so"

cat > "$work/bin/php" <<'PHP'
#!/usr/bin/env bash
if [ "${1:-}" = -m ]; then
    printf '%s\n' "${FAKE_MODULE_NAME-PDO}"
    # Much larger than a pipe buffer, with PDO at the start: an early grep -q
    # exit must not make a successful module probe look like missing PDO.
    if [ "${FAKE_LARGE_MODULES:-0}" = 1 ]; then
        awk 'BEGIN { for (i = 0; i < 65536; i++) print "other_module" }' || exit $?
    fi
    exit "${FAKE_MODULE_STATUS:-0}"
fi
: > "$FAKE_RUNNER_MARKER"
if [ "${CHECK_PDO_ARGUMENT:-0}" = 1 ]; then
    found=0
    for arg in "$@"; do
        [ "$arg" != "extension=$FAKE_EXTENSION_DIR/pdo.so" ] || found=1
    done
    if [ "$found" != "$EXPECTED_PDO_ARGUMENT" ]; then
        echo "unexpected PDO loading argument: $found" >&2
        exit 255
    fi
fi
if [ "${FAKE_EMPTY:-0}" = 0 ]; then
    printf 'Number of tests : %5s          %8s\n' \
        "${FAKE_TEST_COUNT:-2}" "${FAKE_EXECUTED_COUNT-2}"
    cat <<'SUMMARY'
Tests borked    : 0
Tests leaked    : 0
SUMMARY
    if [ "${FAKE_FAILURES:-1}" = 1 ]; then
        cat <<'SUMMARY'
FAILED TEST SUMMARY
bug 36798 [bug_36798.phpt]
bug 43130 [bug_43130.phpt]
=====
SUMMARY
    fi
fi
exit "${FAKE_PHP_STATUS:-1}"
PHP
cat > "$work/bin/php-config" <<'CONFIG'
#!/usr/bin/env bash
printf '%s\n' "$FAKE_EXTENSION_DIR"
CONFIG
cat > "$work/bin/tee" <<'TEE'
#!/usr/bin/env bash
# Preserve realistic output while simulating an output-file write failure.
cat > "$1"
cat "$1"
exit "${FAKE_TEE_STATUS:-0}"
TEE
chmod +x "$work/bin/php" "$work/bin/php-config" "$work/bin/tee"
export FAKE_EXTENSION_DIR="$work/extensions" FAKE_RUNNER_MARKER="$work/runner.started"

check() {
    local label="$1" expected="$2" php_status="$3" tee_status="$4" empty="$5"
    local status=0
    rm -f "$FAKE_RUNNER_MARKER"
    PATH="$work/bin:$PATH" PHP="$work/bin/php" RUN_TESTS="$work/run-tests.php" \
        COMMON_DIR="$work/common" EXT="$work/pdo_duckdb.so" \
        EXPECTED_FAILS="${ALLOWLIST-bug_36798.phpt bug_43130.phpt}" \
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

# An explicitly empty allowlist means that every common test must pass.
ALLOWLIST= FAKE_FAILURES=0 check 'clean run with empty allowlist' 0 0 0 0
ALLOWLIST= check 'unexpected failures with empty allowlist' 1 1 0 0
ALLOWLIST= FAKE_FAILURES=0 check 'missing run with empty allowlist' 1 0 0 1
ALLOWLIST= FAKE_FAILURES=0 FAKE_TEST_COUNT=0 check 'zero tests with empty allowlist' 1 0 0 0

# The first summary count includes skipped tests; the second counts execution.
ALLOWLIST= FAKE_FAILURES=0 FAKE_EXECUTED_COUNT=0 check 'all tests skipped with empty allowlist' 1 0 0 0
ALLOWLIST= FAKE_FAILURES=0 FAKE_TEST_COUNT=10 FAKE_EXECUTED_COUNT=1 check 'some tests skipped with empty allowlist' 0 0 0 0
ALLOWLIST= FAKE_FAILURES=0 FAKE_EXECUTED_COUNT= check 'missing executed-test count' 1 0 0 0

# This file is run by the validate CI job; exercise the actual wrapper and argv.
CHECK_PDO_ARGUMENT=1 EXPECTED_PDO_ARGUMENT=0 check 'already loaded PDO' 0 0 0 0
CHECK_PDO_ARGUMENT=1 EXPECTED_PDO_ARGUMENT=0 FAKE_MODULE_NAME=pDo check 'case-insensitive PDO name' 0 0 0 0
CHECK_PDO_ARGUMENT=1 EXPECTED_PDO_ARGUMENT=0 FAKE_LARGE_MODULES=1 check 'large module output with PDO first' 0 0 0 0
CHECK_PDO_ARGUMENT=1 EXPECTED_PDO_ARGUMENT=1 FAKE_MODULE_NAME=pdo_duckdb check 'whole module name required' 0 0 0 0
CHECK_PDO_ARGUMENT=1 EXPECTED_PDO_ARGUMENT=1 FAKE_MODULE_NAME= check 'missing PDO loads shared module' 0 0 0 0
FAKE_MODULE_STATUS=1 check 'module probe failure stops the runner' 1 0 0 0
[ ! -e "$FAKE_RUNNER_MARKER" ]
FAKE_MODULE_STATUS=143 check 'terminated module probe stops the runner' 1 0 0 0
[ ! -e "$FAKE_RUNNER_MARKER" ]
rm "$work/extensions/pdo.so"
CHECK_PDO_ARGUMENT=1 EXPECTED_PDO_ARGUMENT=0 FAKE_MODULE_NAME= check 'missing PDO without shared module' 0 0 0 0
