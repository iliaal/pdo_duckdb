#!/usr/bin/env bash
set -Eeuo pipefail
shopt -s inherit_errexit

main() (
  if (($# < 2 || $# > 3)); then
    printf 'Usage: %s VERIFIED_SOURCE_CACHE LIBRARY_DIRECTORY [GA_EXTENSION_DIRECTORY]\n' "${0##*/}" >&2
    return 64
  fi
  local script_dir repo source_cache library_dir
  script_dir=$(cd -- "${BASH_SOURCE[0]%/*}" && pwd -P)
  repo=${script_dir%/*}
  source_cache=$(cd -- "$1" && pwd -P)
  library_dir=$(cd -- "$2" && pwd -P)
  python3 "${script_dir}/duckdb-bridge-source.py" verify "${source_cache}"
  build_dir=$(mktemp -d "${TMPDIR:-/tmp}/pdo-duckdb-bridge.XXXXXXXX")
  trap 'rm -rf -- "${build_dir}"' EXIT
  local -a includes=(-I"${repo}" -I"${source_cache}/include")
  local -a cxx_flags=(-std=c++17 -Wall -Wextra -Wno-unused-parameter -Werror)
  local -a links=(-L"${library_dir}" "-Wl,-rpath,${library_dir}" -lduckdb)
  "${CXX:-c++}" "${cxx_flags[@]}" "${includes[@]}" \
    -c "${repo}/pdo_duckdb_bridge.cpp" -o "${build_dir}/bridge.o"
  "${CXX:-c++}" "${cxx_flags[@]}" "${includes[@]}" \
    -c "${repo}/pdo_duckdb_bridge_types.cpp" -o "${build_dir}/types-bridge.o"
  "${CXX:-c++}" "${cxx_flags[@]}" "${includes[@]}" \
    -c "${repo}/pdo_duckdb_bridge_variant.cpp" -o "${build_dir}/variant-bridge.o"
  local test_name
  for test_name in types variant; do
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror "${includes[@]}" \
      -c "${repo}/tests/bridge/${test_name}.c" -o "${build_dir}/${test_name}.o"
    "${CXX:-c++}" "${build_dir}/bridge.o" "${build_dir}/types-bridge.o" "${build_dir}/variant-bridge.o" \
      "${build_dir}/${test_name}.o" "${links[@]}" -o "${build_dir}/${test_name}"
    "${build_dir}/${test_name}"
  done
  "${CXX:-c++}" "${cxx_flags[@]}" "${includes[@]}" "${build_dir}/bridge.o" \
    "${build_dir}/types-bridge.o" "${build_dir}/variant-bridge.o" \
    "${repo}/tests/bridge/geometry.cpp" "${links[@]}" -o "${build_dir}/geometry"
  "${build_dir}/geometry"
  "${CXX:-c++}" "${cxx_flags[@]}" "${includes[@]}" "${build_dir}/bridge.o" \
    "${repo}/pdo_duckdb_bridge_query.cpp" "${repo}/tests/bridge/query.cpp" "${links[@]}" -o "${build_dir}/query"
  "${build_dir}/query" "${3:-}"
  "${CXX:-c++}" "${cxx_flags[@]}" "${includes[@]}" "${build_dir}/bridge.o" \
    "${repo}/pdo_duckdb_bridge_appender.cpp" "${repo}/tests/bridge/appender.cpp" "${links[@]}" -o "${build_dir}/appender"
  "${build_dir}/appender"
  "${CXX:-c++}" "${cxx_flags[@]}" "${includes[@]}" -pthread "${build_dir}/bridge.o" \
    "${repo}/pdo_duckdb_bridge_filesystem.cpp" "${repo}/tests/bridge/filesystem.cpp" "${links[@]}" -o "${build_dir}/filesystem"
  "${build_dir}/filesystem"
  "${CXX:-c++}" "${cxx_flags[@]}" "${includes[@]}" -pthread "${build_dir}/bridge.o" \
    "${repo}/pdo_duckdb_bridge_query.cpp" "${repo}/pdo_duckdb_bridge_logging.cpp" \
    "${repo}/tests/bridge/logging.cpp" "${links[@]}" -o "${build_dir}/logging"
  "${build_dir}/logging" "${3:-}"
)

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
  main "$@"
fi
