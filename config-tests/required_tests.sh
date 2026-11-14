#!/usr/bin/env bash
# Exercise the required-test gate without PHP or DuckDB installed.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
harness="${1:-$repo/scripts/check-required-tests.sh}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/tests"
touch "$work/tests/required.phpt" "$work/tests/other.phpt"
cd "$work"

check() {
    local label="$1" expected="$2" log="$3"
    shift 3
    local status=0
    bash "$harness" "$log" "$@" >"$work/output" 2>&1 || status=$?
    if { [[ $expected == pass ]] && ((status != 0)); } ||
       { [[ $expected == fail ]] && ((status == 0)); }; then
        printf 'FAIL: %s (status %s)\n' "$label" "$status" >&2
        cat "$work/output" >&2
        exit 1
    fi
    printf 'PASS: %s\n' "$label"
}

printf 'PASS required test [tests/required.phpt] \n' > log
check 'passing required test' pass log tests/required.phpt
check 'missing test file' fail log tests/missing.phpt
check 'missing log' fail missing.log tests/required.phpt
check 'directory instead of log' fail tests tests/required.phpt
: > log
check 'empty log' fail log tests/required.phpt
printf 'Number of tests : 1 1\nTests passed : 1\n' > log
check 'summary alone is not per-test evidence' fail log tests/required.phpt
printf 'PASS other test [tests/other.phpt]\n' > log
check 'unrelated pass' fail log tests/required.phpt
printf 'PASS suffix [tests/required.phpt.extra]\n' > log
check 'path suffix does not match' fail log tests/required.phpt
for status in SKIP FAIL BORK XFAIL WARN; do
    printf '%s required test [tests/required.phpt]\n' "$status" > log
    check "$status is not a pass" fail log tests/required.phpt
done
printf 'PASS required test [tests/required.phpt]\nSKIP required test [tests/required.phpt]\n' > log
check 'skip overrides pass' fail log tests/required.phpt
printf 'TEST 1/1 [tests/required.phpt]\r\033[1;32mPASS\033[0m required test [tests/required.phpt] \n' > log
check 'ANSI colour and carriage-return progress' pass log tests/required.phpt
check 'each required test must pass' fail log tests/required.phpt tests/other.phpt
printf 'PASS other test [tests/other.phpt]\n' >> log
check 'multiple required tests' pass log tests/required.phpt tests/other.phpt
