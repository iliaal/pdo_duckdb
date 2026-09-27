#!/bin/sh
set -eu

source_repo=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd -P)
phpize=${PHPIZE:-phpize}
php_config=${PHP_CONFIG:-php-config}
duckdb_prefix=${DUCKDB_PREFIX:-${HOME}/duckdb}
work=$(mktemp -d /tmp/pdo-duckdb-prefix.XXXXXXXX)
cleanup() {
    status=$?
    set +e
    trap - EXIT HUP INT TERM
    if test "${status}" -ne 0 && test "${status}" -ne 42; then
        for log in "${work}"/*build.log; do
            test ! -f "${log}" || tail -n 30 "${log}" >&2
        done
    fi
    rm -rf "${work}"
    exit "${status}"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

work=$(CDPATH='' cd -- "${work}" && pwd -P)
case "${work}/" in
    "${source_repo}/"*)
        echo 'probe scratch directory must be outside the checkout' >&2
        exit 1
        ;;
    *) ;;
esac

if test "${PDO_DUCKDB_PROBE_FORCE_FAILURE:-}" = 1; then
    exit 42
fi

if test ! -r "${duckdb_prefix}/include/duckdb.h" || test ! -r "${duckdb_prefix}/lib/libduckdb.so"; then
    echo "DUCKDB_PREFIX must contain include/duckdb.h and lib/libduckdb.so" >&2
    exit 77
fi
duckdb_prefix=$(CDPATH='' cd -- "${duckdb_prefix}" && pwd -P)
prefix_sha=$(sha256sum "${duckdb_prefix}/lib/libduckdb.so")
prefix_sha=${prefix_sha%% *}

repo="${work}/source"
out_source="${work}/out-source"
mkdir "${repo}" "${out_source}"
cp "${source_repo}"/*.c "${source_repo}"/*.h "${source_repo}/config.m4" "${repo}/"
cp "${repo}"/* "${out_source}/"
relative_prefix='build/Duck DB'
static_prefix="${work}/Duck Static"
mkdir -p "${static_prefix}"
ln -s "${duckdb_prefix}/include/duckdb.h" "${static_prefix}/duckdb.h"
ar rcs "${static_prefix}/libduckdb_static.a"
ar rcs "${static_prefix}/libduckdb_math.a"

mkdir -p "${work}/Duck DB/include" "${work}/Duck DB/lib"
mkdir -p "${repo}/build"
ln -s "${work}/Duck DB" "${repo}/${relative_prefix}"
ln -s "${duckdb_prefix}/include/duckdb.h" "${work}/Duck DB/include/duckdb.h"
ln -s "${duckdb_prefix}/lib/libduckdb.so" "${work}/Duck DB/lib/libduckdb.so.1"
ln -s libduckdb.so.1 "${work}/Duck DB/lib/libduckdb.so"

cd "${repo}"
"${phpize}" >"${work}/phpize.log" 2>&1
(cd "${out_source}" && "${phpize}" >"${work}/out-of-tree-phpize.log" 2>&1)
out_tree="${work}/out-of-tree"
mkdir "${out_tree}"
if ! (cd "${out_tree}" && "${out_source}/configure" --with-pdo-duckdb="${duckdb_prefix}" --with-php-config="${php_config}") >"${work}/out-of-tree.log" 2>&1; then
    cat "${work}/out-of-tree.log" >&2
    exit 1
fi
make -C "${out_tree}" -n >"${work}/out-of-tree-make.log" 2>&1
make -C "${out_tree}" -j2 >"${work}/out-of-tree-build.log" 2>&1
cp "${repo}/configure" "${work}/configure.shared"
failed_prefix="${work}/Failed Prefix"
mkdir -p "${failed_prefix}/include"
ln -s "${duckdb_prefix}/include/duckdb.h" "${failed_prefix}/include/duckdb.h"
if ! ./configure --with-pdo-duckdb="${duckdb_prefix}" --with-php-config="${php_config}" >"${work}/baseline.log" 2>&1; then
    cat "${work}/baseline.log" >&2
    exit 1
fi
make -n >"${work}/baseline-make.log" 2>&1
make -j2 >"${work}/baseline-build.log" 2>&1
if ./configure --with-pdo-duckdb="${failed_prefix}" --with-php-config="${php_config}" >"${work}/failed-reconfigure.log" 2>&1; then
    echo 'invalid DuckDB prefix unexpectedly configured' >&2
    exit 1
fi
staged=$(find "${repo}/build" -maxdepth 1 -name '.duckdb-config-*' -print -quit)
test -z "${staged}"
make -n >"${work}/after-failed-make.log" 2>&1
make clean >"${work}/after-failed-clean.log" 2>&1
make -j2 >"${work}/after-failed-build.log" 2>&1
failed_static_prefix="${work}/Failed Static"
if ./configure --with-pdo-duckdb-static="${failed_static_prefix}" --with-php-config="${php_config}" >"${work}/failed-static.log" 2>&1; then
    echo 'invalid static DuckDB prefix unexpectedly configured' >&2
    exit 1
fi
grep -F "${failed_static_prefix}" "${work}/failed-static.log" >/dev/null
make -n >"${work}/after-failed-static-make.log" 2>&1
if ! ./configure --with-pdo-duckdb="${relative_prefix}" --with-php-config="${php_config}" >"${work}/dynamic.log" 2>&1; then
    cat "${work}/dynamic.log" >&2
    exit 1
fi
grep -F 'build/duckdb-config-include' Makefile >/dev/null
case "$(uname -s)" in
    Windows_NT | MINGW* | MSYS* | CYGWIN*)
        if grep -F -- '-Wl,-rpath,' Makefile >/dev/null; then
            echo 'Windows configure emitted an invalid GNU RUNPATH token' >&2
            exit 1
        fi
        ;;
    *)
        grep -E 'PDO_DUCKDB_SHARED_LIBADD = "?\-Wl,-rpath,' Makefile >/dev/null
        ;;
esac
make -n >"${work}/dynamic-make.log" 2>&1
make -j2 >"${work}/dynamic-build.log" 2>&1
make clean >"${work}/dynamic-clean.log" 2>&1
make -j2 >"${work}/after-clean-build.log" 2>&1
make install INSTALL_ROOT="${work}/install" >"${work}/install.log" 2>&1
module=$(find "${work}/install" -name pdo_duckdb.so -type f -print -quit)
awk '
    /^  ext_output="yes, shared"$/ { force_shared = 1 }
    force_shared && /^  ext_shared=yes$/ {
        sub(/yes$/, "no")
        count++
        force_shared = 0
    }
    { print }
    END { if (count != 1) exit 1 }
' configure >configure.nonshared
chmod +x configure.nonshared
mv configure.nonshared configure
if ! ./configure --with-pdo-duckdb="${relative_prefix}" --with-php-config="${php_config}" >"${work}/nonshared.log" 2>&1; then
    cat "${work}/nonshared.log" >&2
    exit 1
fi
grep -F 'build/duckdb-config-libdir' Makefile >/dev/null
grep -F "LIBS=\"-lduckdb \$LIBS" configure >/dev/null
for variable in EXTRA_LDFLAGS EXTRA_LDFLAGS_PROGRAM; do
    if test "${variable}" = EXTRA_LDFLAGS_PROGRAM && ! grep -q "^${variable} =" Makefile; then
        continue
    fi
    flags=$(sed -n "s/^${variable} = //p" Makefile)
    case "${flags}" in
        *"\"-Wl,-rpath,${work}/Duck DB/lib\""*) ;;
        *)
            echo "${variable} lost the quoted DuckDB runtime path: ${flags}" >&2
            exit 1
            ;;
    esac
done
nonshared_ldflags=$(sed -n 's/^LDFLAGS = //p' Makefile)
case "${nonshared_ldflags}" in
    *-rpath*duckdb-config-libdir*)
        echo "nonshared LDFLAGS uses the build alias as a runtime path" >&2
        exit 1
        ;;
    *) ;;
esac
cp "${work}/configure.shared" configure

test -n "${module}"
runpath=$(readelf -d "${module}" | sed -n 's/.*RUNPATH.*\[\([^]]*\)\].*/\1/p')
test -n "${runpath}"
case "${runpath}" in
    *"${repo}/build/duckdb-config-libdir"*)
        echo "RUNPATH points at build alias" >&2
        exit 1
        ;;
    *) ;;
esac
case "${runpath}" in
    *"${work}/Duck DB/lib"*) ;;
    *)
        echo "RUNPATH does not contain the configured DuckDB libdir: ${runpath}" >&2
        exit 1
        ;;
esac

php=$("${php_config}" --php-binary)
set -- -n
if ! "${php}" -n -r 'exit(extension_loaded("PDO") ? 0 : 1);'; then
    extension_dir=$("${php_config}" --extension-dir)
    set -- "$@" -d "extension=${extension_dir}/pdo.so"
fi
env -u LD_LIBRARY_PATH "${php}" "$@" -d "extension=${module}" <<'PHP'
<?php
$pdo = new PDO("duckdb::memory:");
exit($pdo->query("SELECT 42")->fetchColumn() === 42 ? 0 : 1);
PHP

if ! ./configure --with-pdo-duckdb-static="${static_prefix}" --with-php-config="${php_config}" >"${work}/static.log" 2>&1; then
    cat "${work}/static.log" >&2
    exit 1
fi
grep -F 'build/duckdb-config-static' Makefile >/dev/null
make -n >"${work}/static-make.log" 2>&1
test -e "${repo}/build/duckdb-config-static/duckdb.h"
make clean >"${work}/static-clean.log" 2>&1
test -r "${repo}/build/duckdb-config-static/libduckdb_static.a"
test -r "${repo}/build/duckdb-config-static/libduckdb_math.a"

if ! ./configure --with-pdo-duckdb="${duckdb_prefix}" --with-php-config="${php_config}" >"${work}/restore.log" 2>&1; then
    cat "${work}/restore.log" >&2
    exit 1
fi
grep -F 'build/duckdb-config-libdir' Makefile >/dev/null
test -r "${duckdb_prefix}/lib/libduckdb.so"
current_sha=$(sha256sum "${duckdb_prefix}/lib/libduckdb.so")
current_sha=${current_sha%% *}
test "${current_sha}" = "${prefix_sha}"
echo 'configure prefix whitespace probe: ok'
