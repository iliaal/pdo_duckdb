#!/usr/bin/env bash
set -Eeuo pipefail
shopt -s inherit_errexit

: "${PHP_CONFIG:?set PHP_CONFIG to the php-config from a ZTS embed installation}"
: "${EXT:?set EXT to the newly built pdo_duckdb module}"
: "${DUCKDB_PREFIX:?set DUCKDB_PREFIX to the matching library prefix}"
repo="$(cd "$(dirname "$0")/.." && pwd)"
prefix="$("${PHP_CONFIG}" --prefix)"
work="$(mktemp -d)"
trap 'LD_PRELOAD="" rm -rf "${work}"' EXIT
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
preload="${libasan}${libstdcpp:+ ${libstdcpp}}"
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"
export LSAN_OPTIONS="${LSAN_OPTIONS:-suppressions=${repo}/.github/lsan-suppressions.txt:print_suppressions=0}"
# Use PHP's memory-checker DSO retention: run module shutdown, retaining roots
# and symbols for LeakSanitizer's process-exit scan.
export ZEND_DONT_UNLOAD_MODULES=1
# Each scenario: name, PHP script, workers, requests, max_execution_time,
# USE_ZEND_ALLOC. A timeout's fatal error leaves request memory to the Zend
# arena reset, which only the Zend allocator performs; LeakSanitizer still
# checks persistent allocations there.
scenarios=(
	"instance zts_instance_threads.php 2 2 0 0"
	"shared-reads zts_instance_latency.php 4 1 0 0"
	"registry zts_instance_latency.php 3 1 0 0"
	"timeout zts_instance_latency.php 1 2 2 1"
)
selected=("$@")
ran=0
for entry in "${scenarios[@]}"; do
	read -r name script workers requests max_execution_time zend_alloc <<<"${entry}"
	if [[ ${#selected[@]} -gt 0 && " ${selected[*]} " != *" ${name} "* ]]; then
		continue
	fi
	ran=$((ran + 1))
	dir="${work}/${name}"
	mkdir "${dir}" "${dir}/inside" "${dir}/outside"
	status=0
	PDO_DUCKDB_ZTS_DIR="${dir}" PDO_DUCKDB_ZTS_SCRIPT="${repo}/config-tests/${script}" \
		PDO_DUCKDB_ZTS_SCENARIO="${name}" USE_ZEND_ALLOC="${zend_alloc}" LD_PRELOAD='' timeout -k 5s 90s env LD_PRELOAD="${preload}" \
		"${work}/zts-instance-threads" "${EXT}" "${workers}" "${requests}" "${max_execution_time}" \
		>"${dir}/output" 2>&1 || status=$?
	output="$(<"${dir}/output")"
	printf '[%s]\n%s\n' "${name}" "${output}"
	if [[ "${status}" -ne 0 ]]; then
		echo "error: scenario ${name} exited with status ${status}" >&2
		exit "${status}"
	fi
	if [[ "${output}" != 'PASS: concurrent PDO requests and shutdown' ]]; then
		echo "error: scenario ${name} produced unexpected output" >&2
		exit 1
	fi
done
if [[ "${ran}" -ne ${#selected[@]} && ${#selected[@]} -gt 0 ]] || [[ "${ran}" -eq 0 ]]; then
	echo 'error: unknown scenario requested' >&2
	exit 2
fi
