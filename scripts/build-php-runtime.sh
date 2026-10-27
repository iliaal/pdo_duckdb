#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  printf 'Usage: %s --prefix DIR --thread-safety nts|zts --url HTTPS_URL --sha256 HASH --version VERSION [--jobs 1..64]\n' "${0##*/}"
}

die() {
  printf '%s\n' "$*" >&2
  exit 1
}

main() {
  local prefix='' thread_safety='' url='' sha256='' version='' jobs=4 option
  local script_dir work_dir actual_hash source_version dep entry
  local -a configure_args
  while (($#)); do
    option=$1
    case "${option}" in
      --help)
        usage
        return 0
        ;;
      --prefix | --thread-safety | --url | --sha256 | --version | --jobs)
        if [[ $# -lt 2 || -z $2 || $2 == -* ]]; then
          usage >&2
          return 64
        fi
        case "${option}" in
          --prefix) prefix=$2 ;;
          --thread-safety) thread_safety=$2 ;;
          --url) url=$2 ;;
          --sha256) sha256=$2 ;;
          --version) version=$2 ;;
          --jobs) jobs=$2 ;;
          *) return 64 ;;
        esac
        shift 2
        ;;
      *)
        usage >&2
        return 64
        ;;
    esac
  done
  [[ ${prefix} == /* && ${prefix} != / && ${prefix} != */ ]] || die 'Prefix must be an absolute directory other than /, without a trailing slash'
  case "${prefix}/" in
    *'/../'* | *'/./'* | *'//'*) die 'Prefix must not contain dot components or repeated slashes' ;;
    *) ;;
  esac
  [[ ${thread_safety} == nts || ${thread_safety} == zts ]] || die 'Thread safety must be nts or zts'
  [[ ${url} == https://* ]] || die 'Source URL must use HTTPS'
  [[ ${sha256} =~ ^[0-9a-f]{64}$ ]] || die 'SHA256 must contain 64 lowercase hexadecimal digits'
  [[ ${version} =~ ^8\.[1-9][0-9]*\.[0-9]+(RC[0-9]+)?$ ]] || die 'Version must be an exact PHP 8 release version'
  if [[ ! ${jobs} =~ ^[1-9][0-9]?$ ]] || ((jobs > 64)); then
    die 'Jobs must be an integer from 1 to 64'
  fi
  [[ ! -L ${prefix} ]] || die 'Prefix must not be a symbolic link'
  if [[ -e ${prefix} ]]; then
    [[ -d ${prefix} ]] || die 'Prefix exists and is not a directory'
    for entry in "${prefix}"/* "${prefix}"/.[!.]* "${prefix}"/..?*; do
      if [[ -e ${entry} || -L ${entry} ]]; then
        die 'Prefix must be empty; validate cached runtimes with php-build-probe.php'
      fi
    done
  fi
  for dep in curl tar make; do
    command -v "${dep}" >/dev/null || die "Required command is unavailable: ${dep}"
  done
  script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
  work_dir=$(mktemp -d "${TMPDIR:-/tmp}/pdo-duckdb-php.XXXXXXXX")
  runtime_work_dir=${work_dir}
  trap 'rm -rf -- "${runtime_work_dir}"' EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
  curl --fail --show-error --location --proto '=https' --tlsv1.2 --retry 3 \
    --output "${work_dir}/php.tar.xz" "${url}"
  if command -v sha256sum >/dev/null; then
    actual_hash=$(sha256sum "${work_dir}/php.tar.xz")
  elif command -v shasum >/dev/null; then
    actual_hash=$(shasum -a 256 "${work_dir}/php.tar.xz")
  else
    die 'Required SHA256 tool is unavailable (sha256sum or shasum)'
  fi
  [[ ${actual_hash%% *} == "${sha256}" ]] || die 'PHP source checksum mismatch'
  mkdir -- "${work_dir}/source" "${work_dir}/stage"
  tar -xJf "${work_dir}/php.tar.xz" --strip-components=1 -C "${work_dir}/source"
  source_version=$(sed -n 's/^#define PHP_VERSION "\([^"]*\)"$/\1/p' "${work_dir}/source/main/php_version.h")
  [[ ${source_version} == "${version}" ]] || die "Source version mismatch: expected ${version}, found ${source_version}"
  export CFLAGS="${CFLAGS:-${PHP_CFLAGS:--O2}}"
  export CPPFLAGS="${CPPFLAGS:-${PHP_CPPFLAGS:-}}"
  export LDFLAGS="${LDFLAGS:-${PHP_LDFLAGS:-}}"
  configure_args=("--prefix=${prefix}" --disable-all --enable-cli --enable-pdo --disable-cgi --disable-phpdbg --without-pear)
  if [[ ${thread_safety} == zts ]]; then
    configure_args+=(--enable-zts)
  else
    configure_args+=(--disable-zts)
  fi
  (
    cd -- "${work_dir}/source"
    ./configure "${configure_args[@]}"
    make -j "${jobs}"
    sapi/cli/php -n "${script_dir}/php-build-probe.php" --version "${version}" --thread-safety "${thread_safety}"
    make install INSTALL_ROOT="${work_dir}/stage"
  )
  mkdir -p -- "${prefix}"
  cp -R "${work_dir}/stage${prefix}/." "${prefix}/"
  "${prefix}/bin/php" -n "${script_dir}/php-build-probe.php" --version "${version}" --thread-safety "${thread_safety}"
  rm -rf -- "${work_dir}"
  unset runtime_work_dir
  trap - EXIT INT TERM
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
  main "$@"
fi
