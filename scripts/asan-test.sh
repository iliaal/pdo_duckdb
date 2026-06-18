#!/usr/bin/env bash
#
# Run the pdo_duckdb phpt suite under AddressSanitizer.
#
# Prerequisites:
#   - PHP built with ASan (-fsanitize=address) and PDO available.
#   - pdo_duckdb built with -fsanitize=address against that PHP.
#   - libduckdb reachable (DUCKDB_PREFIX/lib).
#
# Why the LD_PRELOAD dance: the prebuilt libduckdb links *shared* libstdc++ and
# throws C++ exceptions internally during ordinary query binding
# (LogicalType::NormalizeType). ASan's __cxa_throw interceptor resolves the real
# __cxa_throw lazily; if libstdc++/libasan are not loaded first it stays NULL and
# ASan aborts ("AddressSanitizer CHECK failed: real___cxa_throw != 0") on the
# first internal DuckDB exception -- which looks like a crash but is purely a
# C++-runtime load-order issue, not a driver bug. Preloading libasan + libstdc++
# fixes it.
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

libasan=$(ldd "$PHP" | awk '/libasan/ {print $3; exit}')
if [ -z "$libasan" ]; then
    echo "error: $PHP is not linked against libasan (not an ASan build)" >&2
    exit 1
fi
# libstdc++ is pulled in by libduckdb, not by php directly -- resolve it from
# libduckdb's own deps, falling back to the system loader cache. It must be
# preloaded alongside libasan or ASan's __cxa_throw interceptor stays NULL.
libstdcpp=$(ldd "$DUCKDB_PREFIX/lib/libduckdb.so" 2>/dev/null | awk '/libstdc\+\+/ {print $3; exit}')
[ -z "$libstdcpp" ] && libstdcpp=$(ldconfig -p 2>/dev/null | awk '/libstdc\+\+\.so\.6/ {print $NF; exit}')

export LD_LIBRARY_PATH="$DUCKDB_PREFIX/lib:${LD_LIBRARY_PATH:-}"
export LD_PRELOAD="$libasan${libstdcpp:+ $libstdcpp}"
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1}"

echo "PHP=$PHP"
echo "LD_PRELOAD=$LD_PRELOAD"
echo "ASAN_OPTIONS=$ASAN_OPTIONS"

# pdo_duckdb may be built into PHP or loaded as a shared module from EXT.
TEST_PHP_EXECUTABLE="$PHP" "$PHP" -d extension="$EXT" "$RUN_TESTS" -p "$PHP" tests/
