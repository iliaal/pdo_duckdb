#!/usr/bin/env bash
# Evaluate the generated redirect with PHP, without DuckDB or php-src installed.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
harness="${1:-$repo/scripts/pdo-common-tests.sh}"
php="${PHP:-php}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cat > "$work/runner.php" <<'PHP'
<?php
$redirect = file_get_contents($argv[count($argv) - 1]);
$sections = explode("--REDIRECTTEST--\n", $redirect, 2);
$actual = eval($sections[1]);
$expectedDsn = getenv('EXPECTED_DSN') ?: 'duckdb:' . dirname($argv[count($argv) - 1]) . '/common.duckdb';
if ($actual['TESTS'] !== getenv('COMMON_DIR') ||
    $actual['ENV']['PDOTEST_DSN'] !== $expectedDsn) {
    fwrite(STDERR, "Redirect did not preserve the supplied paths\n");
    exit(2);
}
echo "Number of tests : 1 1\nTests borked : 0\nTests leaked : 0\n";
PHP

check() {
    local label="$1" directory="$2" dsn="$3"
    PHP="$php" RUN_TESTS="$work/runner.php" COMMON_DIR="$directory" \
        PDOTEST_DSN="$dsn" EXPECTED_DSN="$dsn" EXPECTED_FAILS= \
        EXT="$work/pdo_duckdb.so" bash "$harness" > "$work/output" 2>&1 || {
        echo "FAIL: $label" >&2
        cat "$work/output" >&2
        exit 1
    }
    echo "PASS: $label"
}

check 'plain paths' "$work/common" "duckdb:$work/common.db"
check 'spaces in paths' "$work/PDO common" "duckdb:$work/my database.db"
check 'apostrophe in common directory' "$work/O'Brien/common" "duckdb:$work/common.db"
check 'apostrophe in database path' "$work/common" "duckdb:$work/O'Brien.db"
check 'backslashes in paths' "$work/back\\\\slash" "duckdb:$work/back\\\\slash.db"
check 'newlines in paths' "$work/"$'line\nbreak' "duckdb:$work/"$'line\nbreak.db'

mkdir "$work/O'Brien tmp"
TMPDIR="$work/O'Brien tmp" check 'default database path in quoted TMPDIR' "$work/common" ''
