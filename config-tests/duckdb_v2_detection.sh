#!/bin/sh
set -eu

source_repo=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd -P)
phpize=${PHPIZE:-phpize}
php_config=${PHP_CONFIG:-php-config}
duckdb_prefix=${DUCKDB_PREFIX:-${HOME}/duckdb}
: "${BRIDGE_SOURCE:?Set BRIDGE_SOURCE to the verified exact-source cache}"
work=$(mktemp -d /tmp/pdo-duckdb-v2-detect.XXXXXXXX)
cleanup() {
    status=$?
    set +e
    trap - EXIT HUP INT TERM
    rm -rf "${work}"
    exit "${status}"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if test ! -r "${duckdb_prefix}/include/duckdb.h" || test ! -r "${duckdb_prefix}/lib/libduckdb.so"; then
    echo "DUCKDB_PREFIX must contain include/duckdb.h and lib/libduckdb.so" >&2
    exit 77
fi
if grep -q DUCKDB_TYPE_TIMESTAMP_TZ_NS "${duckdb_prefix}/include/duckdb.h" ||
    test -e "${duckdb_prefix}/include/duckdb_v2.h"; then
    echo "DUCKDB_PREFIX must be a DuckDB 1.5.x install" >&2
    exit 77
fi
duckdb_prefix=$(CDPATH='' cd -- "${duckdb_prefix}" && pwd -P)
bridge_source=$(CDPATH='' cd -- "${BRIDGE_SOURCE}" && pwd -P)

repo="${work}/source"
mkdir -p "${repo}/scripts"
cp "${source_repo}"/*.c "${source_repo}"/*.h "${source_repo}"/*.cpp \
    "${source_repo}"/*.hpp "${source_repo}/config.m4" "${repo}/"
cp "${source_repo}/scripts/duckdb-bridge-source.py" "${repo}/scripts/"
cp "${source_repo}/scripts/check-duckdb-headers.py" "${repo}/scripts/"

stray="${work}/stray"
mkdir -p "${stray}"
cat >"${stray}/duckdb_v2.h" <<'EOF'
#define DUCKDB_V2_API_VERSION_MAJOR 2
#define DUCKDB_V2_API_VERSION_MINOR 0
#define DUCKDB_V2_API_VERSION_PATCH 0
EOF

mixed="${work}/mixed"
mkdir -p "${mixed}/include" "${mixed}/lib"
ln -s "${duckdb_prefix}/include/duckdb.h" "${mixed}/include/duckdb.h"
cp "${stray}/duckdb_v2.h" "${mixed}/include/duckdb_v2.h"
ln -s "${duckdb_prefix}/lib/libduckdb.so" "${mixed}/lib/libduckdb.so"

expect_no_v2() {
    if ! grep -qx '/\* #undef HAVE_DUCKDB_V2 \*/' config.h; then
        echo "$1: HAVE_DUCKDB_V2 must stay undefined for a DuckDB 1.5.x prefix" >&2
        grep HAVE_DUCKDB_V2 config.h >&2 || true
        exit 1
    fi
}

cd "${repo}"
"${phpize}" >"${work}/phpize.log" 2>&1

if ! ./configure --with-pdo-duckdb="${duckdb_prefix}" --with-php-config="${php_config}" --with-pdo-duckdb-bridge-source="${bridge_source}" >"${work}/baseline.log" 2>&1; then
    cat "${work}/baseline.log" >&2
    exit 1
fi
expect_no_v2 baseline

if ! CPPFLAGS="-I${stray}" C_INCLUDE_PATH="${stray}" \
    ./configure --with-pdo-duckdb="${duckdb_prefix}" --with-php-config="${php_config}" --with-pdo-duckdb-bridge-source="${bridge_source}" >"${work}/stray.log" 2>&1; then
    cat "${work}/stray.log" >&2
    exit 1
fi
expect_no_v2 'stray duckdb_v2.h on the include path'

if ./configure --with-pdo-duckdb="${mixed}" --with-php-config="${php_config}" --with-pdo-duckdb-bridge-source="${bridge_source}" >"${work}/mixed.log" 2>&1; then
    echo 'a DuckDB 2.0 duckdb_v2.h next to a 1.5.x duckdb.h unexpectedly configured' >&2
    exit 1
fi
grep -F 'the DuckDB headers come from different releases' "${work}/mixed.log" >/dev/null
staged=$(find "${repo}/build" -maxdepth 1 -name '.duckdb-config-*' -print -quit)
test -z "${staged}"

echo 'configure DuckDB 2.0 detection probe: ok'
