#!/usr/bin/env bash
#
# Run PHP's shared "PDO Common" test suite (ext/pdo/tests) against the duckdb
# driver via the --REDIRECTTEST-- harness. These tests are NOT bundled with this
# out-of-tree extension; point COMMON_DIR at a php-src checkout's ext/pdo/tests.
#
# The suite is run against a FILE-backed database, not ":memory:": several common
# tests reconnect in their --CLEAN-- section (a fresh PDOTest::factory()), and a
# duckdb::memory: reconnect is a different, empty database, so the cleanup DROP
# fails and the test BORKs. A file DSN makes the reconnect see the same data.
#
# Outcome is compared against a strict expected-fail allowlist (EXPECTED_FAILS):
# the run passes iff the set of FAIL/BORK/LEAK tests equals the allowlist exactly.
# A new failure fails the job; an allowlisted test that starts passing ALSO fails
# it (so the list gets tightened rather than silently masking a future regression).
#
# Default allowlist (DuckDB SQL-dialect strictness, not driver bugs):
#   - bug_36798: DuckDB rejects non-UTF-8 bytes in string literals.
#   - bug_43130: ":id-value" rewrites to "$1-value", which DuckDB rejects at
#     prepare; other drivers defer to an HY093 execute warning.
#
# Usage:
#   PHP=/path/to/bin/php \
#   RUN_TESTS=/path/to/php-src/run-tests.php \
#   COMMON_DIR=/path/to/php-src/ext/pdo/tests \
#   DUCKDB_PREFIX=$HOME/duckdb \
#   EXT=/path/to/modules/pdo_duckdb.so \
#   [EXPECTED_FAILS="bug_36798.phpt bug_43130.phpt"] \
#   [PDOTEST_DSN="duckdb:/custom/path.db"] \
#   scripts/pdo-common-tests.sh
set -euo pipefail

PHP="${PHP:?set PHP to a php binary}"
RUN_TESTS="${RUN_TESTS:?set RUN_TESTS to a run-tests.php path}"
COMMON_DIR="${COMMON_DIR:?set COMMON_DIR to a php-src ext/pdo/tests directory}"
DUCKDB_PREFIX="${DUCKDB_PREFIX:-$HOME/duckdb}"
EXTDIR="$("$(dirname "$PHP")/php-config" --extension-dir 2>/dev/null || echo)"
EXPECTED_FAILS="${EXPECTED_FAILS:-bug_36798.phpt bug_43130.phpt}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

DSN="${PDOTEST_DSN:-duckdb:$work/common.duckdb}"

cat > "$work/common.phpt" <<EOF
--TEST--
DuckDB (PDO common tests)
--EXTENSIONS--
pdo
pdo_duckdb
--REDIRECTTEST--
return ['ENV' => ['PDOTEST_DSN' => '$DSN'], 'TESTS' => '$COMMON_DIR'];
EOF

export LD_LIBRARY_PATH="$DUCKDB_PREFIX/lib:${LD_LIBRARY_PATH:-}"

ext_args=()
# setup-php builds pdo statically (no pdo.so); only add it when present as a
# shared module. A locally-built shared pdo.so may still load RTLD_LOCAL and fail
# pdo_dbh_new/pdo_parse_params at load — use a pdo-static PHP for local runs.
if [ -n "$EXTDIR" ] && [ -f "$EXTDIR/pdo.so" ]; then
    ext_args+=(-d "extension=$EXTDIR/pdo.so")
fi
ext_args+=(-d "extension=${EXT:-$EXTDIR/pdo_duckdb.so}")

out="$work/out.log"
# run-tests exits non-zero on any failure; don't let that abort before the
# allowlist comparison.
set +e
TEST_PHP_EXECUTABLE="$PHP" NO_INTERACTION=1 "$PHP" "$RUN_TESTS" "${ext_args[@]}" \
    -g FAIL,BORK,LEAK,XLEAK -p "$PHP" "$work/common.phpt" 2>&1 | tee "$out"

clean="$work/clean.log"
sed -E 's/\x1b\[[0-9;]*m//g' "$out" | tr '\r' '\n' > "$clean"

# Failed-test basenames come from the FAILED TEST SUMMARY block: it is stable
# across run-tests versions and -g flags, and it already excludes XFAIL (an
# upstream-marked expected fail). Drop common.phpt — that's the REDIRECT parent.
actual="$(sed -n '/FAILED TEST SUMMARY/,/^=====/p' "$clean" \
    | grep -oE '[A-Za-z0-9_]+\.phpt' | grep -v '^common\.phpt$' | sort -u)"
expected="$(printf '%s\n' "$EXPECTED_FAILS" | tr ', ' '\n' | grep -E '\.phpt$' | sort -u)"

# A BORK/LEAK is not in the FAIL summary; the file DSN should yield none, so treat
# any as a regression.
borked="$(grep -oE 'Tests (borked|leaked)[[:space:]]*:[[:space:]]*[0-9]+' "$clean" \
    | grep -oE '[0-9]+$' | awk '{s+=$1} END{print s+0}')"
set -e
borked="${borked:-0}"

if [ "$actual" = "$expected" ] && [ "$borked" -eq 0 ]; then
    echo "PDO common: failure set matches the expected allowlist:"
    printf '%s\n' "${expected:-(none)}" | sed 's/^/  /'
    exit 0
fi

echo "::error::PDO common result differs from the expected allowlist (borked/leaked: $borked)."
echo "--- new/unexpected failures (fix, or add to EXPECTED_FAILS) ---"
comm -13 <(printf '%s\n' "$expected") <(printf '%s\n' "$actual") | sed 's/^/  /'
echo "--- allowlisted but now passing (remove from EXPECTED_FAILS) ---"
comm -23 <(printf '%s\n' "$expected") <(printf '%s\n' "$actual") | sed 's/^/  /'
exit 1
