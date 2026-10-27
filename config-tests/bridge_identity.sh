#!/bin/sh
set -eu

source_repo=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd -P)
phpize=${PHPIZE:-phpize}
php_config=${PHP_CONFIG:-php-config}
: "${DUCKDB_PREFIX:?Set DUCKDB_PREFIX to the shared-library install prefix}"
: "${BRIDGE_SOURCE:?Set BRIDGE_SOURCE to its verified exact-source cache}"
: "${MISMATCH_BRIDGE_SOURCE:?Set MISMATCH_BRIDGE_SOURCE to a different release cache}"
work=$(mktemp -d "${TMPDIR:-/tmp}/pdo-duckdb-bridge-config.XXXXXXXX")
cleanup() {
    status=$?
    trap - EXIT HUP INT TERM
    rm -rf "${work}"
    exit "${status}"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

mkdir -p "${work}/source/scripts"
cp "${source_repo}"/*.c "${source_repo}"/*.h "${source_repo}/config.m4" "${work}/source/"
cp "${source_repo}/scripts/duckdb-bridge-source.py" "${work}/source/scripts/"
cp "${source_repo}/scripts/check-duckdb-headers.py" "${work}/source/scripts/"
cp -R "${BRIDGE_SOURCE}" "${work}/headers with spaces"
cd "${work}/source"
"${phpize}" >"${work}/phpize.log" 2>&1
if ! ./configure --with-php-config="${php_config}" --with-pdo-duckdb="${DUCKDB_PREFIX}" \
    --with-pdo-duckdb-bridge-source="${work}/headers with spaces" >"${work}/matching.log" 2>&1; then
    cat "${work}/matching.log" >&2
    exit 1
fi
grep -qx '#define HAVE_PDO_DUCKDB_BRIDGE 1' config.h
include_alias=$(readlink build/duckdb-config-include)
library_alias=$(readlink build/duckdb-config-libdir)
bridge_alias=$(readlink build/duckdb-config-bridge)
assert_aliases_preserved() {
    current_include=$(readlink build/duckdb-config-include)
    current_library=$(readlink build/duckdb-config-libdir)
    current_bridge=$(readlink build/duckdb-config-bridge)
    test "${current_include}" = "${include_alias}"
    test "${current_library}" = "${library_alias}"
    test "${current_bridge}" = "${bridge_alias}"
}
mkdir -p "${work}/alternate prefix/include" "${work}/alternate prefix/lib"
cp "${MISMATCH_BRIDGE_SOURCE}/include/"duckdb*.h "${work}/alternate prefix/include/"
ln -s "${DUCKDB_PREFIX}/lib/libduckdb.so" "${work}/alternate prefix/lib/libduckdb.so"
if ./configure --with-php-config="${php_config}" --with-pdo-duckdb="${work}/alternate prefix" \
    --with-pdo-duckdb-bridge-source="${MISMATCH_BRIDGE_SOURCE}" >"${work}/mismatched.log" 2>&1; then
    echo 'Mismatched DuckDB bridge source unexpectedly configured' >&2
    exit 1
fi
grep -F 'DuckDB bridge header/library identity mismatch' "${work}/mismatched.log" >/dev/null
assert_aliases_preserved
staged=$(find build -maxdepth 1 -name '.duckdb-config-*' -print -quit)
test -z "${staged}"

mkdir -p "${work}/changed headers/include" "${work}/changed headers/lib"
cp "${DUCKDB_PREFIX}/include/"*.h "${work}/changed headers/include/"
ln -s "${DUCKDB_PREFIX}/lib/libduckdb.so" "${work}/changed headers/lib/libduckdb.so"
printf '\n' >>"${work}/changed headers/include/duckdb.h"
if ./configure --with-php-config="${php_config}" --with-pdo-duckdb="${work}/changed headers" \
    --with-pdo-duckdb-bridge-source="${BRIDGE_SOURCE}" >"${work}/changed-headers.log" 2>&1; then
    echo 'Modified public DuckDB header unexpectedly configured' >&2
    exit 1
fi
grep -F 'DuckDB C header verification failed' "${work}/changed-headers.log" >/dev/null
assert_aliases_preserved
if test -f "${BRIDGE_SOURCE}/include/duckdb_v2.h"; then
    cp "${DUCKDB_PREFIX}/include/duckdb.h" "${work}/changed headers/include/duckdb.h"
    rm "${work}/changed headers/include/duckdb_extension_v2.h"
    if ./configure --with-php-config="${php_config}" --with-pdo-duckdb="${work}/changed headers" \
        --with-pdo-duckdb-bridge-source="${BRIDGE_SOURCE}" >"${work}/incomplete-headers.log" 2>&1; then
        echo 'Incomplete GA public DuckDB headers unexpectedly configured' >&2
        exit 1
    fi
    grep -F 'DuckDB C header verification failed' "${work}/incomplete-headers.log" >/dev/null
    assert_aliases_preserved
fi

cp -R "${BRIDGE_SOURCE}" "${work}/modified"
printf '\n' >>"${work}/modified/include/duckdb.hpp"
if ./configure --with-php-config="${php_config}" --with-pdo-duckdb="${DUCKDB_PREFIX}" \
    --with-pdo-duckdb-bridge-source="${work}/modified" >"${work}/modified.log" 2>&1; then
    echo 'Modified DuckDB source cache unexpectedly configured' >&2
    exit 1
fi
grep -F 'DuckDB bridge source-cache verification failed' "${work}/modified.log" >/dev/null
staged=$(find build -maxdepth 1 -name '.duckdb-config-*' -print -quit)
test -z "${staged}"
if ./configure --with-php-config="${php_config}" --with-pdo-duckdb="${DUCKDB_PREFIX}" >"${work}/missing.log" 2>&1; then
    echo 'Missing DuckDB bridge source unexpectedly configured' >&2
    exit 1
fi
grep -F 'PDO DuckDB requires matching C++ source headers' "${work}/missing.log" >/dev/null
assert_aliases_preserved
echo 'configure DuckDB bridge identity probe: ok (matching, spaced, mismatched, modified source/public headers, required, and failed-reconfigure preservation)'
