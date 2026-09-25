#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
phpize=${PHPIZE:-phpize}
php_config=${PHP_CONFIG:-php-config}
duckdb_prefix=${DUCKDB_PREFIX:-$HOME/duckdb}
work=$(mktemp -d)
cp "$repo/run-tests.php" "$work/run-tests.php"
relative_prefix="build/Duck DB $$"
static_prefix="$repo/build/Duck Static $$"
mkdir -p "$repo/build"
rm -rf "$repo"/build/.duckdb-config-include-* "$repo"/build/.duckdb-config-libdir-* "$repo"/build/.duckdb-config-static-*
rm -f "$repo/confdefs.h"
cleanup() {
    status=$?
    set +e
    trap - EXIT HUP INT TERM
    cp "$work/run-tests.php" "$repo/run-tests.php"
    rm -f "$repo/$relative_prefix"
    if test -f "$work/configure.shared"; then
        cp "$work/configure.shared" "$repo/configure"
    fi
    rm -rf "$work" "$static_prefix"
    rm -rf "$repo/build/duckdb-config-static"
    rm -rf "$repo"/build/.duckdb-config-include-* "$repo"/build/.duckdb-config-libdir-* "$repo"/build/.duckdb-config-static-*
    if test "$status" -ne 0 && test -x "$repo/configure"; then
        (cd "$repo" && ./configure --with-pdo-duckdb="$duckdb_prefix" --with-php-config="$php_config" >/dev/null 2>&1)
    fi
    rm -f "$repo/modules/pdo_duckdb.so" "$repo/modules/pdo_duckdb.la"
    if test "$status" -eq 0; then
        if ! (cd "$repo" && make -n >/dev/null 2>&1); then
            status=1
        fi
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if test "${PDO_DUCKDB_PROBE_FORCE_FAILURE:-}" = 1; then
    exit 42
fi

if test ! -r "$duckdb_prefix/include/duckdb.h" || test ! -r "$duckdb_prefix/lib/libduckdb.so"; then
    echo "DUCKDB_PREFIX must contain include/duckdb.h and lib/libduckdb.so" >&2
    exit 77
fi
duckdb_prefix=$(CDPATH= cd -- "$duckdb_prefix" && pwd -P)

rm -rf "$static_prefix"
mkdir -p "$static_prefix"
ln -s "$duckdb_prefix/include/duckdb.h" "$static_prefix/duckdb.h"
ar rcs "$static_prefix/libduckdb_static.a"
ar rcs "$static_prefix/libduckdb_math.a"

mkdir -p "$work/Duck DB/include" "$work/Duck DB/lib"
ln -s "$work/Duck DB" "$repo/$relative_prefix"
ln -s "$duckdb_prefix/include/duckdb.h" "$work/Duck DB/include/duckdb.h"
ln -s "$duckdb_prefix/lib/libduckdb.so" "$work/Duck DB/lib/libduckdb.so"

cd "$repo"
"$phpize" >"$work/phpize.log" 2>&1
out_source="$work/source"
mkdir "$out_source"
cp -R "$repo/." "$out_source/"
rm -rf "$out_source/build" "$out_source/modules" "$out_source/.libs" "$out_source/config.log" "$out_source/confdefs.h" "$out_source/config.status" "$out_source/config.cache" "$out_source/Makefile" "$out_source/libtool" "$out_source/config.h" "$out_source/config.h.in"
(cd "$out_source" && "$phpize" >"$work/out-of-tree-phpize.log" 2>&1)
out_tree="$work/out-of-tree"
mkdir "$out_tree"
if ! (cd "$out_tree" && "$out_source/configure" --with-pdo-duckdb="$duckdb_prefix" --with-php-config="$php_config") >"$work/out-of-tree.log" 2>&1; then
    cat "$work/out-of-tree.log" >&2
    exit 1
fi
make -C "$out_tree" -n >"$work/out-of-tree-make.log" 2>&1
cp "$repo/configure" "$work/configure.shared"
failed_prefix="$work/Failed Prefix"
mkdir -p "$failed_prefix/include"
ln -s "$duckdb_prefix/include/duckdb.h" "$failed_prefix/include/duckdb.h"
if ! ./configure --with-pdo-duckdb="$duckdb_prefix" --with-php-config="$php_config" >"$work/baseline.log" 2>&1; then
    cat "$work/baseline.log" >&2
    exit 1
fi
make -n >"$work/baseline-make.log" 2>&1
if ./configure --with-pdo-duckdb="$failed_prefix" --with-php-config="$php_config" >"$work/failed-reconfigure.log" 2>&1; then
    echo 'invalid DuckDB prefix unexpectedly configured' >&2
    exit 1
fi
staged=$(find "$repo/build" -maxdepth 1 -name '.duckdb-config-*' -print -quit)
test -z "$staged"
make -n >"$work/after-failed-make.log" 2>&1
failed_static_prefix="$work/Failed Static"
if ./configure --with-pdo-duckdb-static="$failed_static_prefix" --with-php-config="$php_config" >"$work/failed-static.log" 2>&1; then
    echo 'invalid static DuckDB prefix unexpectedly configured' >&2
    exit 1
fi
grep -F "$failed_static_prefix" "$work/failed-static.log" >/dev/null
make -n >"$work/after-failed-static-make.log" 2>&1
if ! ./configure --with-pdo-duckdb="$relative_prefix" --with-php-config="$php_config" >"$work/dynamic.log" 2>&1; then
    cat "$work/dynamic.log" >&2
    exit 1
fi
grep -F 'build/duckdb-config-include' Makefile >/dev/null
case "$(uname -s)" in
  Windows_NT|MINGW*|MSYS*|CYGWIN*)
    if grep -F -- '-Wl,-rpath,' Makefile >/dev/null; then
      echo 'Windows configure emitted an invalid GNU RUNPATH token' >&2
      exit 1
    fi
    ;;
  *)
    grep -E 'PDO_DUCKDB_SHARED_LIBADD = "?\-Wl,-rpath,' Makefile >/dev/null
    ;;
esac
test -r "$duckdb_prefix/lib/libduckdb.so"
make clean >/dev/null 2>&1
test -r "$duckdb_prefix/lib/libduckdb.so"
if ! ./configure --with-pdo-duckdb="$relative_prefix" --with-php-config="$php_config" >"$work/after-clean-configure.log" 2>&1; then
    cat "$work/after-clean-configure.log" >&2
    exit 1
fi
make -n >"$work/dynamic-make.log" 2>&1
make -j2 >"$work/dynamic-build.log" 2>&1
make install INSTALL_ROOT="$work/install" >"$work/install.log" 2>&1
module=$(find "$work/install" -name pdo_duckdb.so -type f -print -quit)
rewrite_count=$(awk 'BEGIN { seen = 0; count = 0 } { if (NR > 4700 && NR < 4900 && $0 ~ /^[[:space:]]*ext_shared=yes$/) { if (seen++ > 0) { sub(/yes$/, "no"); count++ } } } END { print count + 0 }' configure)
test "$rewrite_count" -eq 1

awk 'BEGIN { seen = 0 } { if (NR > 4700 && NR < 4900 && $0 ~ /^[[:space:]]*ext_shared=yes$/) { if (seen++ > 0) sub(/yes$/, "no") } print }' configure > configure.nonshared
chmod +x configure.nonshared
mv configure.nonshared configure
if ! ./configure --with-pdo-duckdb="$relative_prefix" --with-php-config="$php_config" >"$work/nonshared.log" 2>&1; then
    cat "$work/nonshared.log" >&2
    exit 1
fi
grep -F 'build/duckdb-config-libdir' Makefile >/dev/null
grep -F 'LIBS="-lduckdb $LIBS' configure >/dev/null
cp "$work/configure.shared" configure

test -n "$module"
runpath=$(readelf -d "$module" | sed -n 's/.*RUNPATH.*\[\([^]]*\)\].*/\1/p')
test -n "$runpath"
case "$runpath" in
  *"$repo/build/duckdb-config-libdir"*) echo "RUNPATH points at build alias" >&2; exit 1 ;;
esac
case "$runpath" in
  *"$work/Duck DB/lib"*) ;;
  *) echo "RUNPATH does not contain the configured DuckDB libdir: $runpath" >&2; exit 1 ;;
esac

if ! ./configure --with-pdo-duckdb-static="$static_prefix" --with-php-config="$php_config" >"$work/static.log" 2>&1; then
    cat "$work/static.log" >&2
    exit 1
fi
grep -F 'build/duckdb-config-static' Makefile >/dev/null
make -n >"$work/static-make.log" 2>&1
test -e "$repo/build/duckdb-config-static/duckdb.h"

if ! ./configure --with-pdo-duckdb="$duckdb_prefix" --with-php-config="$php_config" >"$work/restore.log" 2>&1; then
    cat "$work/restore.log" >&2
    exit 1
fi
grep -F 'build/duckdb-config-libdir' Makefile >/dev/null
test -r "$duckdb_prefix/lib/libduckdb.so"
echo 'configure prefix whitespace probe: ok'
