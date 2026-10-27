#!/usr/bin/env bash
set -Eeuo pipefail
shopt -s inherit_errexit

: "${PHP_CONFIG:?set PHP_CONFIG to the php-config from a ZTS embed installation}"
: "${EXT:?set EXT to the newly built pdo_duckdb module}"
: "${DUCKDB_PREFIX:?set DUCKDB_PREFIX to the matching library prefix}"
repo="$(cd "$(dirname "$0")/.." && pwd)"
prefix="$("${PHP_CONFIG}" --prefix)"
work="$(mktemp -d)"
trap 'LD_PRELOAD= rm -rf "${work}"' EXIT
mkdir "${work}/inside" "${work}/outside"
libasan="$(ldd "${prefix}/bin/php" | awk '/libasan|libclang_rt.asan/ && !found {print $3; found = 1}')"
if [[ -z "${libasan}" || ! -f "${libasan}" ]]; then
	echo 'error: this concurrency gate requires an ASan-built PHP embed library' >&2
	exit 1
fi
compiler="${CC:-cc}"
default_flags='-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer'
if [[ "${libasan}" == *libclang_rt.asan* ]]; then
	compiler="${CC:-clang}"
	default_flags+=' -shared-libasan'
fi
include_flags="$("${PHP_CONFIG}" --includes)"
read -r -a includes <<<"${include_flags}"
read -r -a flags <<<"${CFLAGS:-${default_flags}}"
"${compiler}" "${flags[@]}" -Wall -Wextra -Wno-unused-parameter -Werror \
	"${includes[@]}" -I"${prefix}/include/php/sapi/embed" \
	"${repo}/config-tests/zts_instance_threads.c" -L"${prefix}/lib" \
	-Wl,-rpath,"${prefix}/lib" -lphp -lpthread -o "${work}/zts-instance-threads"
libstdcpp="$(ldd "${DUCKDB_PREFIX}/lib/libduckdb.so" | awk '/libstdc\+\+/ && !found {print $3; found = 1}')"
export LD_LIBRARY_PATH="${DUCKDB_PREFIX}/lib:${prefix}/lib:${LD_LIBRARY_PATH:-}"
export LD_PRELOAD="${libasan}${libstdcpp:+ ${libstdcpp}}"
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
export LSAN_OPTIONS="${LSAN_OPTIONS:-suppressions=${repo}/.github/lsan-suppressions.txt:print_suppressions=0}"
export USE_ZEND_ALLOC=0
# Use PHP's memory-checker DSO retention: run module shutdown, retaining roots
# and symbols for LeakSanitizer's process-exit scan.
export ZEND_DONT_UNLOAD_MODULES=1
export PDO_DUCKDB_ZTS_DIR="${work}"
export PDO_DUCKDB_ZTS_SCRIPT="${repo}/config-tests/zts_instance_threads.php"
status=0
timeout -k 5s 90s "${work}/zts-instance-threads" "${EXT}" >"${work}/output" 2>&1 || status=$?
output="$(<"${work}/output")"
printf '%s\n' "${output}"
if [[ "${status}" -ne 0 ]]; then
	exit "${status}"
fi
if [[ "${output}" != 'PASS: concurrent PDO requests and shutdown' ]]; then
	echo 'error: concurrent PDO test produced unexpected output' >&2
	exit 1
fi
