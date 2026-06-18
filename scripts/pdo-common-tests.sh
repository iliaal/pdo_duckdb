#!/usr/bin/env bash
#
# Run PHP's shared "PDO Common" test suite (ext/pdo/tests) against the duckdb
# driver via the --REDIRECTTEST-- harness. These tests are NOT bundled with this
# out-of-tree extension; point COMMON_DIR at a php-src checkout's ext/pdo/tests.
#
# Usage:
#   PHP=/path/to/bin/php \
#   RUN_TESTS=/path/to/php-src/run-tests.php \
#   COMMON_DIR=/path/to/php-src/ext/pdo/tests \
#   DUCKDB_PREFIX=$HOME/duckdb \
#   scripts/pdo-common-tests.sh
#
# Known expected failures (DuckDB SQL-dialect strictness, not driver bugs):
#   - bug_36798: DuckDB rejects non-UTF-8 bytes in string literals.
#   - bug_43130: ":id-value" rewrites to "$1-value", which DuckDB rejects at
#     prepare; other drivers defer to an HY093 execute warning.
set -euo pipefail

PHP="${PHP:?set PHP to a php binary}"
RUN_TESTS="${RUN_TESTS:?set RUN_TESTS to a run-tests.php path}"
COMMON_DIR="${COMMON_DIR:?set COMMON_DIR to a php-src ext/pdo/tests directory}"
DUCKDB_PREFIX="${DUCKDB_PREFIX:-$HOME/duckdb}"
EXTDIR="$("$(dirname "$PHP")/php-config" --extension-dir 2>/dev/null || echo)"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cat > "$work/common.phpt" <<EOF
--TEST--
DuckDB (PDO common tests)
--EXTENSIONS--
pdo
pdo_duckdb
--REDIRECTTEST--
return ['ENV' => ['PDOTEST_DSN' => 'duckdb::memory:'], 'TESTS' => '$COMMON_DIR'];
EOF

export LD_LIBRARY_PATH="$DUCKDB_PREFIX/lib:${LD_LIBRARY_PATH:-}"

ext_args=()
if [ -n "$EXTDIR" ] && [ -f "$EXTDIR/pdo.so" ]; then
    ext_args+=(-d "extension=$EXTDIR/pdo.so")
fi
ext_args+=(-d "extension=${EXT:-$EXTDIR/pdo_duckdb.so}")

TEST_PHP_EXECUTABLE="$PHP" "$PHP" "$RUN_TESTS" "${ext_args[@]}" -p "$PHP" "$work/common.phpt"
