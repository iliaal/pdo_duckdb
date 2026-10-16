#!/usr/bin/env bash
# Check argument forwarding without requiring an ASan PHP or DuckDB build.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
harness="${1:-$repo/scripts/asan-test.sh}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin" "$work/Duck DB/modules" "$work/plain/modules"

# Supply library names to the wrapper; the fake PHP clears LD_PRELOAD before
# launching any subprocess, so no ASan runtime is required for this test.
cat > "$work/bin/ldd" <<'LDD'
#!/usr/bin/env bash
printf 'libasan.so => %s (0x0)\n' "$FAKE_LIBRARY"
printf 'libstdc++.so => %s (0x0)\n' "$FAKE_LIBRARY"
LDD
cat > "$work/bin/php" <<'PHP'
#!/usr/bin/env bash
set -euo pipefail
unset LD_PRELOAD
[ "$1" = -d ]
[ "$2" = "extension=$EXPECTED_EXT" ]
[ "$3" = "$RUN_TESTS" ]
shift 3
# Model run-tests.php's literal-space split of TEST_PHP_ARGS. Quoting inside
# that environment string does not protect a path with spaces.
IFS=' ' read -r -a extra <<< "${TEST_PHP_ARGS-}"
set -- "$@" "${extra[@]}"
extension_dir=
caller_option=
while [ "$#" -gt 0 ]; do
    case "$1" in
        -d)
            case "$2" in
                extension_dir=*) extension_dir="${2#extension_dir=}" ;;
                memory_limit=*) caller_option="-d $2" ;;
                *) exit 1 ;;
            esac
            shift 2
            ;;
        -p) [ "$2" = "$PHP" ]; shift 2 ;;
        tests/) shift ;;
        *) printf 'unexpected runner argument: <%s>\n' "$1" >&2; exit 1 ;;
    esac
done
[ "$extension_dir" = "$EXPECTED_DIR" ]
[ "$caller_option" = "$EXPECTED_ARGS" ]
[ "$TEST_PHP_EXECUTABLE" = "$PHP" ]
[ "$USE_ZEND_ALLOC" = 0 ]
if [ "${FAKE_EMPTY:-0}" = 0 ]; then
    printf 'Number of tests : %5s          %8s\n' \
        "${FAKE_TOTAL-2}" "${FAKE_EXECUTED-2}"
fi
exit "${FAKE_STATUS:-0}"
PHP
cat > "$work/bin/tee" <<'TEE'
#!/usr/bin/env bash
cat > "$1"
cat "$1"
exit "${FAKE_TEE_STATUS:-0}"
TEE
chmod +x "$work/bin/ldd" "$work/bin/php" "$work/bin/tee"
# Use a real, harmless shared library as the preload stand-in. This test runs
# on Linux, just like the ldd-based ASan helper it exercises.
library=$(ldd /bin/sh | awk '/libc\.so/ {print $3}')
[ -f "$library" ]

check() {
    local label="$1" directory="$2" caller_args="$3" expected_status="$4" status=0
    PATH="$work/bin:$PATH" PHP="$work/bin/php" RUN_TESTS="$work/run tests.php" \
        DUCKDB_PREFIX="$work/Duck DB" EXT="$directory/pdo_duckdb.so" \
        EXPECTED_EXT="$directory/pdo_duckdb.so" EXPECTED_DIR="$directory" \
        EXPECTED_ARGS="$caller_args" TEST_PHP_ARGS="$caller_args" \
        FAKE_LIBRARY="$library" FAKE_STATUS="${RUNNER_STATUS-$expected_status}" \
        bash "$harness" > "$work/output" 2>&1 || status=$?
    if [ "$status" -ne "$expected_status" ]; then
        echo "FAIL: $label (expected $expected_status, got $status)" >&2
        cat "$work/output" >&2
        exit 1
    fi
    echo "PASS: $label"
}

check 'plain module directory' "$work/plain/modules" '' 0
check 'module directory with spaces' "$work/Duck DB/modules" '' 0
check 'preserve caller PHP options' "$work/Duck DB/modules" '-d memory_limit=256M' 0
check 'preserve runner failure status' "$work/Duck DB/modules" '' 42

RUNNER_STATUS=0 FAKE_EMPTY=1 check 'missing summary' "$work/plain/modules" '' 1
RUNNER_STATUS=0 FAKE_TOTAL=0 FAKE_EXECUTED=0 check 'zero tests' "$work/plain/modules" '' 1
RUNNER_STATUS=0 FAKE_TOTAL=100 FAKE_EXECUTED=0 check 'all tests skipped' "$work/plain/modules" '' 1
RUNNER_STATUS=0 FAKE_EXECUTED= check 'missing executed count' "$work/plain/modules" '' 1
RUNNER_STATUS=0 FAKE_TOTAL=100 FAKE_EXECUTED=1 check 'partial skips still execute' "$work/plain/modules" '' 0
RUNNER_STATUS=0 FAKE_TOTAL=1 FAKE_EXECUTED=1 check 'one executed test' "$work/plain/modules" '' 0
RUNNER_STATUS=0 FAKE_TEE_STATUS=42 check 'preserve logging failure status' "$work/plain/modules" '' 42
RUNNER_STATUS=143 check 'preserve interrupted runner status' "$work/plain/modules" '' 143

# Exercise the actual CI summary gate, including the old guard for reversion
# testing. Replace only its fixed log path; keep the production predicate/body.
# Several jobs share the run-step shape, so read only the ASan step's script.
asan_step=$(awk '
    /^      - name: Run tests under ASAN$/ { in_step = 1; next }
    in_step && (/^      - name: / || /^  [^ ]/) { exit }
    in_step
' "$repo/.github/workflows/tests.yml")
[ -n "$asan_step" ]
ci_gate=$(sed -n '/^          bash scripts\/check-executed-tests.sh /p; /^          if ! grep -qP .Number of tests/,+3p' <<< "$asan_step")
[ -n "$ci_gate" ]
ci_gate=${ci_gate//\/tmp\/test-output.txt/\"\$SUMMARY_LOG\"}
summary_check() {
    local label="$1" expected="$2" contents="$3" status=0
    printf '%b' "$contents" > "$work/summary.log"
    (cd "$repo" && SUMMARY_LOG="$work/summary.log" bash -euo pipefail -c "$ci_gate") \
        > "$work/output" 2>&1 || status=$?
    if [ "$status" -ne "$expected" ]; then
        echo "FAIL: CI $label (expected $expected, got $status)" >&2
        cat "$work/output" >&2
        exit 1
    fi
    echo "PASS: CI $label"
}
summary_check 'all skipped' 1 'Number of tests : 100 0\n'
summary_check 'zero tests' 1 'Number of tests : 0 0\n'
summary_check 'missing summary' 1 ''
summary_check 'missing executed count' 1 'Number of tests : 100\n'
summary_check 'one executed test' 0 'Number of tests : 1 1\n'
summary_check 'partial skips' 0 'Number of tests : 100 1\n'
summary_check 'multi-digit executed count' 0 'Number of tests : 100 100\n'
summary_check 'CRLF and ANSI' 0 '\033[32mNumber of tests : 100 1\033[0m\r\n'
summary_check 'carriage-return progress' 0 'progress\rNumber of tests : 1 1\n'
summary_check 'negative executed count' 1 'Number of tests : 100 -1\n'
summary_check 'zero total, positive executed' 1 'Number of tests : 0 1\n'
summary_check 'trailing junk' 1 'Number of tests : 100 1 junk\n'
summary_check 'partial numeric count' 1 'Number of tests : 100 1.5\n'
summary_check 'misleading prefix' 1 'not Number of tests : 100 1\n'
summary_check 'no final newline' 0 'Number of tests : 1 1'

# Feed a completed mock pipeline into the actual CI status and summary body.
ci_body=$(sed -n '/^          test_status=/,$p' <<< "$asan_step")
[ -n "$ci_body" ]
ci_body=${ci_body//\/tmp\/test-output.txt/\"\$SUMMARY_LOG\"}
ci_status_check() {
    local label="$1" expected="$2" runner="$3" logger="$4" extra="${5:-}" status=0
    printf 'Number of tests : 100 98\n%s\n' "$extra" > "$work/summary.log"
    (cd "$repo" && SUMMARY_LOG="$work/summary.log" RUNNER_STATUS="$runner" LOGGER_STATUS="$logger" \
        bash -uo pipefail -c '(exit "$RUNNER_STATUS") | (cat; exit "$LOGGER_STATUS")
'"$ci_body") > "$work/output" 2>&1 || status=$?
    if [ "$status" -ne "$expected" ]; then
        echo "FAIL: CI $label (expected $expected, got $status)" >&2
        cat "$work/output" >&2
        exit 1
    fi
    echo "PASS: CI $label"
}
ci_status_check 'successful runner and logger' 0 0 0
ci_status_check 'ordinary runner failure' 1 1 0
ci_status_check 'runner crash' 42 42 0
ci_status_check 'runner interruption' 143 143 0
ci_status_check 'logger failure' 42 0 42
ci_status_check 'runner failure takes precedence' 143 143 42
ci_status_check 'existing XFAIL allowed' 0 0 0 'Expected fail : 2'
ci_status_check 'existing XLEAK rejected' 1 0 0 'Expected leak : 1'
