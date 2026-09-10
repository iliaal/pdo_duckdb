#!/usr/bin/env bash
#
# Run the pdo_duckdb phpt suite under AddressSanitizer.
#
# Prerequisites:
#   - PHP built with ASan (-fsanitize=address) and PDO available.
#   - pdo_duckdb built with -fsanitize=address against that PHP.
#   - libduckdb reachable (DUCKDB_PREFIX/lib).
#
# Preload libasan and libstdc++ so ASan resolves __cxa_throw before DuckDB's
# first internal C++ exception; otherwise the interceptor aborts.
#
# Usage:
#   PHP=/path/to/asan/bin/php \
#   RUN_TESTS=/path/to/php-src/run-tests.php \
#   DUCKDB_PREFIX=$HOME/duckdb \
#   scripts/asan-test.sh
set -euo pipefail

PHP="${PHP:?set PHP to an ASan-built php binary}"
RUN_TESTS="${RUN_TESTS:?set RUN_TESTS to a run-tests.php path}"
DUCKDB_PREFIX="${DUCKDB_PREFIX:-$HOME/duckdb}"
EXT="${EXT:-$(pwd)/modules/pdo_duckdb.so}"
EXT_DIR="$(cd "$(dirname "$EXT")" && pwd -P)"
EXT="$EXT_DIR/$(basename "$EXT")"

libasan=$(ldd "$PHP" | awk '/libasan/ {print $3; exit}')
if [ -z "$libasan" ]; then
    echo "error: $PHP is not linked against libasan (not an ASan build)" >&2
    exit 1
fi
# libstdc++ is a dependency of libduckdb, not PHP.
libstdcpp=$(ldd "$DUCKDB_PREFIX/lib/libduckdb.so" 2>/dev/null | awk '/libstdc\+\+/ {print $3; exit}')
[ -z "$libstdcpp" ] && libstdcpp=$(ldconfig -p 2>/dev/null | awk '/libstdc\+\+\.so\.6/ {print $NF; exit}')

export LD_LIBRARY_PATH="$DUCKDB_PREFIX/lib:${LD_LIBRARY_PATH:-}"
export LD_PRELOAD="$libasan${libstdcpp:+ $libstdcpp}"
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1}"
# Mirror CI's narrow library-scoped leak suppressions (libduckdb init state, the
# spatial extension's bundled sqlite3/GDAL) so a local run matches the ASan job.
SUPP="$(pwd)/.github/lsan-suppressions.txt"
[ -f "$SUPP" ] && export LSAN_OPTIONS="${LSAN_OPTIONS:-suppressions=$SUPP:print_suppressions=0}"
# Bypass ZendMM's pools so ASan can detect use-after-efree.
export USE_ZEND_ALLOC=0

echo "PHP=$PHP"
echo "LD_PRELOAD=$LD_PRELOAD"
echo "ASAN_OPTIONS=$ASAN_OPTIONS"

# pdo_duckdb may be built into PHP or loaded as a shared module from EXT.
export TEST_PHP_ARGS="-d extension_dir=$EXT_DIR${TEST_PHP_ARGS:+ $TEST_PHP_ARGS}"
TEST_PHP_EXECUTABLE="$PHP" "$PHP" -d extension="$EXT" "$RUN_TESTS" -p "$PHP" tests/
