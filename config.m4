PHP_ARG_WITH([pdo-duckdb],
  [for DuckDB support for PDO],
  [AS_HELP_STRING([--with-pdo-duckdb=DIR],
    [PDO: DuckDB support. DIR is the DuckDB install prefix containing
     include/duckdb.h and lib/libduckdb.])])

PHP_ARG_WITH([pdo-duckdb-static],
  [for a self-contained DuckDB build (static libduckdb)],
  [AS_HELP_STRING([--with-pdo-duckdb-static=DIR],
    [PDO: build a self-contained pdo_duckdb by statically linking DuckDB's
     static-libs bundle. DIR holds duckdb.h and the lib*.a archives. Produces a
     .so/.dll that needs no libduckdb at runtime. Used for prebuilt binaries.])],
  [no],
  [no])

PHP_ARG_ENABLE([pdo-duckdb-dev],
  [whether to enable developer build flags for pdo_duckdb],
  [AS_HELP_STRING([--enable-pdo-duckdb-dev],
    [PDO_DUCKDB: build with -Wall -Wextra -Werror for release verification])],
  [no],
  [no])

PHP_ARG_WITH([pdo-duckdb-bridge-source],
  [for verified DuckDB C++ bridge headers],
  [AS_HELP_STRING([--with-pdo-duckdb-bridge-source=DIR],
    [Use the exact-source cache produced by scripts/duckdb-bridge-source.py.
     The selected DuckDB library must have the same version and source ID.])],
  [no],
  [no])

pdo_duckdb_cleanup_staged_aliases() {
  rm -rf "$DUCKDB_CONFIG_STATIC_STAGE" "$DUCKDB_CONFIG_INCLUDE_STAGE" "$DUCKDB_CONFIG_LIBDIR_STAGE" "$DUCKDB_CONFIG_BRIDGE_STAGE"
}

pdo_duckdb_promote_alias() {
  pdo_stage=$1
  pdo_final=$2
  pdo_backup="$2.backup-$$"
  pdo_had_final=0
  if test -e "$pdo_final" || test -L "$pdo_final"; then
    pdo_had_final=1
    if ! mv -f "$pdo_final" "$pdo_backup"; then
      return 1
    fi
  fi
  if ! mv -f "$pdo_stage" "$pdo_final"; then
    if test "$pdo_had_final" = 1; then
      mv -f "$pdo_backup" "$pdo_final" || return 1
    fi
    return 1
  fi
  if test "$pdo_had_final" = 1 && ! rm -rf "$pdo_backup"; then
    return 1
  fi
  return 0
}

pdo_duckdb_promote_aliases() {
  pdo_stage_include=$1
  pdo_final_include=$2
  pdo_stage_libdir=$3
  pdo_final_libdir=$4
  pdo_backup_include="$2.backup-$$"
  pdo_backup_libdir="$4.backup-$$"
  pdo_had_include=0
  pdo_had_libdir=0
  if test -e "$pdo_final_include" || test -L "$pdo_final_include"; then
    pdo_had_include=1
    mv -f "$pdo_final_include" "$pdo_backup_include" || return 1
  fi
  if test -e "$pdo_final_libdir" || test -L "$pdo_final_libdir"; then
    pdo_had_libdir=1
    mv -f "$pdo_final_libdir" "$pdo_backup_libdir" || {
      test "$pdo_had_include" = 1 && mv -f "$pdo_backup_include" "$pdo_final_include"
      return 1
    }
  fi
  if ! mv -f "$pdo_stage_include" "$pdo_final_include"; then
    test "$pdo_had_include" = 1 && mv -f "$pdo_backup_include" "$pdo_final_include"
    test "$pdo_had_libdir" = 1 && mv -f "$pdo_backup_libdir" "$pdo_final_libdir"
    return 1
  fi
  if ! mv -f "$pdo_stage_libdir" "$pdo_final_libdir"; then
    rm -rf "$pdo_final_include"
    test "$pdo_had_include" = 1 && mv -f "$pdo_backup_include" "$pdo_final_include"
    test "$pdo_had_libdir" = 1 && mv -f "$pdo_backup_libdir" "$pdo_final_libdir"
    return 1
  fi
  test "$pdo_had_include" = 0 || rm -rf "$pdo_backup_include" || return 1
  test "$pdo_had_libdir" = 0 || rm -rf "$pdo_backup_libdir" || return 1
  return 0
}
if test "$PHP_PDO_DUCKDB_STATIC" != "no"; then
  dnl --- Self-contained build: statically link the whole DuckDB archive set.
  dnl libduckdb is C++, so link via the C++ driver and pull libstdc++/libgcc in
  dnl statically; the resulting module depends on neither libduckdb nor a
  dnl specific libstdc++ ABI (matters for prebuilt binaries on old glibc/GLIBCXX).
  PHP_PDO_DUCKDB="yes"
  PHP_CHECK_PDO_INCLUDES
  DUCKDB_STATIC_DIR="$PHP_PDO_DUCKDB_STATIC"

  AC_MSG_CHECKING([for the DuckDB static-libs bundle])
  if test ! -r "$DUCKDB_STATIC_DIR/duckdb.h" || test ! -r "$DUCKDB_STATIC_DIR/libduckdb_static.a"; then
    AC_MSG_RESULT([not found])
    AC_MSG_ERROR([Need duckdb.h and libduckdb_static.a in $DUCKDB_STATIC_DIR (the DuckDB static-libs bundle).])
  fi
  case "$DUCKDB_STATIC_DIR" in
    /*) ;;
    *) DUCKDB_STATIC_DIR=`cd "$DUCKDB_STATIC_DIR" && pwd -P` ;;
  esac
  DUCKDB_HEADER_DIR="$DUCKDB_STATIC_DIR"
  AC_MSG_RESULT([found in $DUCKDB_STATIC_DIR])

  PHP_REQUIRE_CXX()
  if ! mkdir -p build; then
    pdo_duckdb_cleanup_staged_aliases
    AC_MSG_ERROR([Unable to create the DuckDB build directory.])
  fi
  DUCKDB_CONFIG_STATIC_FINAL="build/duckdb-config-static"
  DUCKDB_CONFIG_STATIC_STAGE="build/.duckdb-config-static-$$"
  DUCKDB_CONFIG_STATIC_DIR="$DUCKDB_CONFIG_STATIC_STAGE"
  dnl Directory aliases survive PHP's recursive `make clean` and preserve
  dnl relative symlinks between the original bundle's files.
  if ! ln -s "$DUCKDB_STATIC_DIR" "$DUCKDB_CONFIG_STATIC_STAGE"; then
    pdo_duckdb_cleanup_staged_aliases
    AC_MSG_ERROR([Unable to stage the DuckDB static bundle.])
  fi
  DUCKDB_CONFIG_INCLUDE="$DUCKDB_CONFIG_STATIC_STAGE"

  dnl Pass the whole archive set as a single comma-joined -Wl, token. libtool
  dnl otherwise reorders/de-duplicates loose .a arguments and drops archives
  dnl (symptom: a smaller module and an undefined duckdb::… symbol at load); one
  dnl -Wl, token is forwarded to the compiler verbatim.
  DUCKDB_STATIC_ARCHIVES=""
  for DUCKDB_ARCHIVE in "$DUCKDB_CONFIG_STATIC_DIR"/*.a; do
    if test -z "$DUCKDB_STATIC_ARCHIVES"; then
      DUCKDB_STATIC_ARCHIVES="$DUCKDB_ARCHIVE"
    else
      DUCKDB_STATIC_ARCHIVES="$DUCKDB_STATIC_ARCHIVES,$DUCKDB_ARCHIVE"
    fi
  done
  case `uname -s 2>/dev/null` in
    Darwin)
      dnl macOS: ld64 is multi-pass (no --start-group). libtool links the bundle
      dnl with `cc -undefined suppress`, so the DuckDB C++ runtime symbols are
      dnl not auto-resolved; link libc++ explicitly (a system dylib, so dynamic
      dnl linking has no GLIBCXX-style portability concern). Exclude bundled
      dnl jemalloc: DuckDB uses the system allocator on macOS, and jemalloc's
      dnl malloc-zone registration abort()s inside a dlopened bundle (SIGABRT at
      dnl the first query, no exception text).
      DUCKDB_MAC_ARCHIVES=""
      for DUCKDB_ARCHIVE in "$DUCKDB_CONFIG_STATIC_DIR"/*.a; do
        case "$DUCKDB_ARCHIVE" in
          *jemalloc*) continue ;;
        esac
        if test -z "$DUCKDB_MAC_ARCHIVES"; then
          DUCKDB_MAC_ARCHIVES="$DUCKDB_ARCHIVE"
        else
          DUCKDB_MAC_ARCHIVES="$DUCKDB_MAC_ARCHIVES,$DUCKDB_ARCHIVE"
        fi
      done
      dnl -twolevel_namespace overrides libtool's hardcoded, deprecated
      dnl `-flat_namespace`. Under flat namespace DuckDB's statically-linked ICU
      dnl binds malloc/free across library boundaries, so memory it allocates is
      dnl freed through the wrong zone -> find_zone_and_free abort inside
      dnl TimeZone::detectHostTimeZone() at duckdb_open. ld64 honours the last
      dnl namespace flag, and this token is appended after libtool's. libc++abi
      dnl supplies the top-level std:: exception types + EH runtime that libc++
      dnl alone leaves undefined.
      PDO_DUCKDB_SHARED_LIBADD="-Wl,$DUCKDB_MAC_ARCHIVES -lc++ -lc++abi -Wl,-twolevel_namespace"
      ;;
    *)
      dnl GNU ld: --start-group resolves the circular references between the
      dnl DuckDB archives. This extension is all-.c, so gcc drives the link; it
      dnl doesn't link libstdc++ and ignores `-static-libstdc++`. The module then
      dnl loads where libstdc++ is already in the process (the build host) but
      dnl fails `dlopen` on a clean glibc box with `undefined symbol:
      dnl _ZTVN10__cxxabiv120__function_type_infoE`. Link the static libstdc++ and
      dnl libgcc_eh archives explicitly, inside the group.
      LIBSTDCXX_A=`${CXX:-g++} -print-file-name=libstdc++.a 2>/dev/null`
      LIBGCC_EH_A=`${CC:-gcc} -print-file-name=libgcc_eh.a 2>/dev/null`
      if test ! -f "$LIBSTDCXX_A"; then
        pdo_duckdb_cleanup_staged_aliases
        AC_MSG_ERROR([static libstdc++.a not found via '${CXX:-g++} -print-file-name=libstdc++.a'. This self-contained static Linux build requires a GNU toolchain with the static libstdc++ archive. On GNU/gcc, install the static libstdc++ (the libstdc++-*-dev / libstdc++-static package). If CXX is a non-GNU compiler such as clang++ (which uses libc++, not libstdc++), this link mode is unsupported -- set CXX=g++. Refusing to build a module that would not load on a clean host.])
      fi
      PDO_DUCKDB_SHARED_LIBADD="-Wl,--start-group,$DUCKDB_STATIC_ARCHIVES,$LIBSTDCXX_A,$LIBGCC_EH_A,--end-group -static-libgcc"
      ;;
  esac

elif test "$PHP_PDO_DUCKDB" != "no"; then
  PHP_CHECK_PDO_INCLUDES

  AC_MSG_CHECKING([for duckdb.h])
  DUCKDB_DIR=""
  DUCKDB_INCDIR=""
  if test "$PHP_PDO_DUCKDB" = "yes"; then
    for i in /usr/local /usr /opt/duckdb /opt/homebrew /usr/local/opt/duckdb; do
      if test -r "$i/include/duckdb.h"; then
        DUCKDB_DIR="$i"
        DUCKDB_INCDIR="$i/include"
        break
      elif test -r "$i/duckdb.h"; then
        DUCKDB_DIR="$i"
        DUCKDB_INCDIR="$i"
        break
      fi
    done
  elif test -r "$PHP_PDO_DUCKDB/include/duckdb.h"; then
    DUCKDB_DIR="$PHP_PDO_DUCKDB"
    DUCKDB_INCDIR="$PHP_PDO_DUCKDB/include"
  elif test -r "$PHP_PDO_DUCKDB/duckdb.h"; then
    DUCKDB_DIR="$PHP_PDO_DUCKDB"
    DUCKDB_INCDIR="$PHP_PDO_DUCKDB"
  fi

  if test -z "$DUCKDB_DIR"; then
    AC_MSG_RESULT([not found])
    AC_MSG_ERROR([Cannot find duckdb.h. Install the DuckDB C library, or point at it with --with-pdo-duckdb=DIR])
  fi
  AC_MSG_RESULT([found in $DUCKDB_INCDIR])
  case "$DUCKDB_DIR" in
    /*) ;;
    *) DUCKDB_DIR=`cd "$DUCKDB_DIR" && pwd -P` ;;
  esac
  case "$DUCKDB_INCDIR" in
    /*) ;;
    *) DUCKDB_INCDIR=`cd "$DUCKDB_INCDIR" && pwd -P` ;;
  esac
  if ! mkdir -p build; then
    pdo_duckdb_cleanup_staged_aliases
    AC_MSG_ERROR([Unable to create the DuckDB build directory.])
  fi
  DUCKDB_CONFIG_INCLUDE_FINAL="build/duckdb-config-include"
  DUCKDB_CONFIG_LIBDIR_FINAL="build/duckdb-config-libdir"
  DUCKDB_CONFIG_INCLUDE_STAGE="build/.duckdb-config-include-$$"
  DUCKDB_CONFIG_LIBDIR_STAGE="build/.duckdb-config-libdir-$$"
  DUCKDB_CONFIG_INCLUDE="$DUCKDB_CONFIG_INCLUDE_STAGE"
  DUCKDB_CONFIG_LIBDIR="$DUCKDB_CONFIG_LIBDIR_STAGE"
  DUCKDB_RUNTIME_LIBDIR="$DUCKDB_DIR/$PHP_LIBDIR"
  if ! rm -rf "$DUCKDB_CONFIG_INCLUDE_STAGE" "$DUCKDB_CONFIG_LIBDIR_STAGE" ||
      ! ln -s "$DUCKDB_INCDIR" "$DUCKDB_CONFIG_INCLUDE_STAGE"; then
    pdo_duckdb_cleanup_staged_aliases
    AC_MSG_ERROR([Unable to stage the DuckDB headers.])
  fi
  dnl Let the native linker select .so, .dylib, or its platform equivalent.
  if test ! -d "$DUCKDB_RUNTIME_LIBDIR" ||
      ! ln -s "$DUCKDB_RUNTIME_LIBDIR" "$DUCKDB_CONFIG_LIBDIR_STAGE"; then
    pdo_duckdb_cleanup_staged_aliases
    AC_MSG_ERROR([Unable to stage the DuckDB shared library.])
  fi

  save_CHECK_LDFLAGS="$LDFLAGS"
  LDFLAGS="$LDFLAGS -L$DUCKDB_CONFIG_LIBDIR_STAGE"
  PHP_CHECK_LIBRARY([duckdb], [duckdb_appender_error_data],
    [],
    [pdo_duckdb_cleanup_staged_aliases; AC_MSG_ERROR([Could not find a DuckDB 1.5.3-compatible libduckdb. Check config.log for details.])],
    [])
  LDFLAGS="$save_CHECK_LDFLAGS"

fi

if test "$PHP_PDO_DUCKDB" != "no"; then
  if test "$PHP_PDO_DUCKDB_BRIDGE_SOURCE" = "no" || test "$PHP_PDO_DUCKDB_BRIDGE_SOURCE" = "yes"; then
    pdo_duckdb_cleanup_staged_aliases
    AC_MSG_ERROR([PDO DuckDB requires matching C++ source headers. Stage a verified cache with scripts/duckdb-bridge-source.py and pass --with-pdo-duckdb-bridge-source=DIR. Configure never downloads sources.])
  fi
  save_CPPFLAGS="$CPPFLAGS"
  dnl AC_CHECK_DECLS expands CPPFLAGS as shell words; use the stable,
  dnl whitespace-free symlink created for the compiler build flags.
  CPPFLAGS="$CPPFLAGS -I$DUCKDB_CONFIG_INCLUDE"
  AC_CHECK_DECLS([DUCKDB_TYPE_VARIANT], [],
    [pdo_duckdb_cleanup_staged_aliases; AC_MSG_ERROR([DuckDB 1.5.3 or newer headers are required (DUCKDB_TYPE_VARIANT is missing).])],
    [[#include <duckdb.h>]])
  dnl DuckDB 1.5.3 through 1.5.5 report MERGE INTO as DUCKDB_STATEMENT_TYPE_INVALID.
  AC_CHECK_DECLS([DUCKDB_STATEMENT_TYPE_MERGE_INTO], [], [], [[#include <duckdb.h>]])
  AC_CHECK_DECLS([DUCKDB_TYPE_TIMESTAMP_TZ_NS], [], [], [[#include <duckdb.h>]])

  dnl DuckDB 2.0 keeps reporting DUCKDB_API_VERSION 1.5.6 in duckdb.h, so the
  dnl generation is detected from the versioned v2 header it ships alongside.
  dnl Only the copy next to the selected duckdb.h counts: a stray duckdb_v2.h
  dnl on the default include path must not flip a 1.5.x build.
  AC_MSG_CHECKING([for the DuckDB 2.0 C API (duckdb_v2.h)])
  pdo_duckdb_have_v2=no
  if test -r "$DUCKDB_CONFIG_INCLUDE/duckdb_v2.h"; then
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#include "$DUCKDB_CONFIG_INCLUDE/duckdb.h"
#include "$DUCKDB_CONFIG_INCLUDE/duckdb_v2.h"
#if !defined(DUCKDB_V2_API_VERSION_MAJOR) || DUCKDB_V2_API_VERSION_MAJOR < 2
#error DuckDB 2.0 C API not found
#endif
    ]], [[]])], [pdo_duckdb_have_v2=yes], [])
  fi
  AC_MSG_RESULT([$pdo_duckdb_have_v2])
  CPPFLAGS="$save_CPPFLAGS"

  if test "$pdo_duckdb_have_v2" = "yes"; then
    if test "$ac_cv_have_decl_DUCKDB_TYPE_TIMESTAMP_TZ_NS" != "yes"; then
      pdo_duckdb_cleanup_staged_aliases
      AC_MSG_ERROR([duckdb_v2.h is DuckDB 2.0 but duckdb.h lacks DUCKDB_TYPE_TIMESTAMP_TZ_NS; the DuckDB headers come from different releases.])
    fi
    if test -n "$DUCKDB_CONFIG_LIBDIR_STAGE"; then
      save_CHECK_LDFLAGS="$LDFLAGS"
      LDFLAGS="$LDFLAGS -L$DUCKDB_CONFIG_LIBDIR_STAGE"
      PHP_CHECK_LIBRARY([duckdb], [duckdb_create_timestamp_tz_ns],
        [],
        [pdo_duckdb_cleanup_staged_aliases; AC_MSG_ERROR([The DuckDB headers are DuckDB 2.0 but libduckdb is older (no duckdb_create_timestamp_tz_ns). Point --with-pdo-duckdb at a matching header and library set.])],
        [])
      LDFLAGS="$save_CHECK_LDFLAGS"
    fi
    AC_DEFINE([HAVE_DUCKDB_V2], [1], [Whether the DuckDB 2.0 C API headers are available])
    if test -n "$DUCKDB_CONFIG_STATIC_FINAL"; then
      if test ! -r "$DUCKDB_CONFIG_INCLUDE/duckdb_static_extension.h"; then
        pdo_duckdb_cleanup_staged_aliases
        AC_MSG_ERROR([DuckDB 2.0 static builds require the complete header set, including duckdb_static_extension.h.])
      fi
      dnl Preserve the extensions bundled before 2.0. New optional archives
      dnl such as httpfs require additional third-party static dependencies.
      for pdo_extension in autocomplete core_functions icu json parquet tpcds tpch; do
        if test ! -r "$DUCKDB_CONFIG_STATIC_DIR/lib${pdo_extension}_extension.a"; then
          pdo_duckdb_cleanup_staged_aliases
          AC_MSG_ERROR([The DuckDB 2.0 static bundle is missing lib${pdo_extension}_extension.a.])
        fi
      done
      for pdo_capability in httplib loadable_extensions; do
        if test ! -r "$DUCKDB_CONFIG_STATIC_DIR/libduckdb_${pdo_capability}.a"; then
          pdo_duckdb_cleanup_staged_aliases
          AC_MSG_ERROR([The DuckDB 2.0 static bundle is missing libduckdb_${pdo_capability}.a.])
        fi
      done
      save_STATIC_LIBS="$LIBS"
      save_STATIC_CPPFLAGS="$CPPFLAGS"
      save_STATIC_CFLAGS="$CFLAGS"
      save_STATIC_LDFLAGS="$LDFLAGS"
      dnl PHP_REQUIRE_CXX adds -lstdc++ to LIBS. Leaving it in this probe
      dnl would mask an incomplete static runtime with the host's shared one.
      LIBS="$PDO_DUCKDB_SHARED_LIBADD -lm"
      CPPFLAGS="$CPPFLAGS -I$DUCKDB_CONFIG_INCLUDE"
      case `uname -s 2>/dev/null` in
        Linux)
          CFLAGS="$CFLAGS -fPIC"
          LDFLAGS="$LDFLAGS -shared -Wl,-z,defs"
          dnl GA's archives retain this GCC 13 ABI import even when the
          dnl selected API paths do not reference it through a relocation.
          dnl -z defs alone therefore misses the incomplete runtime.
          if ! pdo_duckdb_static_imports=`${NM:-nm} -u "$DUCKDB_STATIC_DIR/libduckdb_static.a" 2>/dev/null`; then
            pdo_duckdb_cleanup_staged_aliases
            AC_MSG_ERROR([Unable to inspect the DuckDB static archive with ${NM:-nm}.])
          fi
          if printf '%s\n' "$pdo_duckdb_static_imports" | grep '_ZSt21ios_base_library_initv' >/dev/null; then
            LDFLAGS="$LDFLAGS -Wl,--require-defined=_ZSt21ios_base_library_initv"
          fi
          ;;
      esac
      AC_MSG_CHECKING([whether DuckDB 2.0 static extensions and the C++ runtime link])
      AC_LINK_IFELSE([AC_LANG_PROGRAM([[
#include <duckdb_static_extension.h>
extern int32_t duckdb_extension_core_functions_describe(duckdb_extension_descriptor *);
      ]], [[return duckdb_register_static_extension(duckdb_extension_core_functions_describe);]])],
        [AC_MSG_RESULT([yes])],
        [pdo_duckdb_cleanup_staged_aliases; AC_MSG_RESULT([no]); AC_MSG_ERROR([The DuckDB 2.0 static bundle cannot link with the selected C++ runtime. Use matching archives and a compatible runtime; the official Linux GA bundle requires GCC 13 or newer libstdc++. A Bookworm-built GCC 13 preserves the Debian 12 deployment baseline. See config.log.])])
      LIBS="$save_STATIC_LIBS"
      CPPFLAGS="$save_STATIC_CPPFLAGS"
      CFLAGS="$save_STATIC_CFLAGS"
      LDFLAGS="$save_STATIC_LDFLAGS"
      AC_DEFINE([HAVE_DUCKDB_STATIC_EXTENSIONS], [1], [Whether bundled DuckDB extensions need explicit registration])
    fi
  elif test "$ac_cv_have_decl_DUCKDB_TYPE_TIMESTAMP_TZ_NS" = "yes"; then
    AC_MSG_WARN([duckdb.h declares DUCKDB_TYPE_TIMESTAMP_TZ_NS but duckdb_v2.h is not next to it. If this is DuckDB 2.0, install its full header set to enable the DuckDB 2.0 code paths. The PHPT suite's DuckDB 2.0 branches are then off as well, so the engine-behaviour tests (030, 035, 051, 078, 093) fail against a DuckDB 2.0 library; those failures are not driver bugs.])
  fi
  if test "$PHP_PDO_DUCKDB_BRIDGE_SOURCE" != "no"; then
    if test "$PHP_PDO_DUCKDB_BRIDGE_SOURCE" = "yes"; then
      AC_MSG_ERROR([--with-pdo-duckdb-bridge-source requires a verified source-cache directory.])
    fi
    AC_PATH_PROG([PDO_DUCKDB_PYTHON], [python3])
    if test -z "$PDO_DUCKDB_PYTHON" ||
        ! "$PDO_DUCKDB_PYTHON" "$srcdir/scripts/duckdb-bridge-source.py" verify "$PHP_PDO_DUCKDB_BRIDGE_SOURCE"; then
      pdo_duckdb_cleanup_staged_aliases
      AC_MSG_ERROR([DuckDB bridge source-cache verification failed. Stage matching sources with scripts/duckdb-bridge-source.py.])
    fi
    AC_MSG_CHECKING([whether the selected DuckDB C headers match the verified source])
    if ! "$PDO_DUCKDB_PYTHON" "$srcdir/scripts/check-duckdb-headers.py" "$PHP_PDO_DUCKDB_BRIDGE_SOURCE/include" "$DUCKDB_CONFIG_INCLUDE"; then
      pdo_duckdb_cleanup_staged_aliases
      AC_MSG_RESULT([no])
      AC_MSG_ERROR([Install the matching complete DuckDB C header set; DuckDB 2.0 requires all five release headers.])
    fi
    AC_MSG_RESULT([yes])
    DUCKDB_BRIDGE_SOURCE=`cd "$PHP_PDO_DUCKDB_BRIDGE_SOURCE" && pwd -P`
    DUCKDB_CONFIG_BRIDGE_STAGE="build/.duckdb-config-bridge-$$"
    DUCKDB_CONFIG_BRIDGE_FINAL="build/duckdb-config-bridge"
    if ! ln -s "$DUCKDB_BRIDGE_SOURCE/include" "$DUCKDB_CONFIG_BRIDGE_STAGE"; then
      pdo_duckdb_cleanup_staged_aliases
      AC_MSG_ERROR([Unable to stage the DuckDB bridge headers.])
    fi
    PHP_REQUIRE_CXX()
    save_BRIDGE_CPPFLAGS="$CPPFLAGS"
    save_BRIDGE_LIBS="$LIBS"
    save_BRIDGE_LDFLAGS="$LDFLAGS"
    save_BRIDGE_CXXFLAGS="$CXXFLAGS"
    CPPFLAGS="$CPPFLAGS -I$DUCKDB_CONFIG_INCLUDE -I$DUCKDB_CONFIG_BRIDGE_STAGE"
    CXXFLAGS="$CXXFLAGS -std=c++17"
    if test -n "$DUCKDB_CONFIG_STATIC_FINAL"; then
      LIBS="$PDO_DUCKDB_SHARED_LIBADD -lm"
    else
      LDFLAGS="$LDFLAGS -L$DUCKDB_CONFIG_LIBDIR"
      LIBS="-lduckdb $LIBS"
    fi
    save_BRIDGE_LD_LIBRARY_PATH=${LD_LIBRARY_PATH-}
    save_BRIDGE_DYLD_LIBRARY_PATH=${DYLD_LIBRARY_PATH-}
    bridge_had_ld_path=${LD_LIBRARY_PATH+yes}
    bridge_had_dyld_path=${DYLD_LIBRARY_PATH+yes}
    if test -n "$DUCKDB_RUNTIME_LIBDIR"; then
      LD_LIBRARY_PATH="$DUCKDB_RUNTIME_LIBDIR${LD_LIBRARY_PATH+:$LD_LIBRARY_PATH}"
      DYLD_LIBRARY_PATH="$DUCKDB_RUNTIME_LIBDIR${DYLD_LIBRARY_PATH+:$DYLD_LIBRARY_PATH}"
      export LD_LIBRARY_PATH DYLD_LIBRARY_PATH
    fi
    AC_LANG_PUSH([C++])
    AC_MSG_CHECKING([whether DuckDB bridge headers exactly match the linked runtime])
    AC_RUN_IFELSE([AC_LANG_PROGRAM([[
#include <duckdb.hpp>
#include <pdo_duckdb_bridge_build.h>
#include <cstring>
    ]], [[
      return std::strcmp(duckdb::DuckDB::LibraryVersion(), PDO_DUCKDB_BRIDGE_EXPECTED_VERSION) ||
        std::strcmp(duckdb::DuckDB::SourceID(), PDO_DUCKDB_BRIDGE_EXPECTED_SOURCE_ID);
    ]])], [pdo_bridge_identity=yes], [pdo_bridge_identity=no],
      [pdo_bridge_identity=cross-compiling])
    AC_MSG_RESULT([$pdo_bridge_identity])
    AC_LANG_POP([C++])
    CPPFLAGS="$save_BRIDGE_CPPFLAGS"
    LIBS="$save_BRIDGE_LIBS"
    LDFLAGS="$save_BRIDGE_LDFLAGS"
    CXXFLAGS="$save_BRIDGE_CXXFLAGS"
    if test "$bridge_had_ld_path" = yes; then
      LD_LIBRARY_PATH="$save_BRIDGE_LD_LIBRARY_PATH"
      export LD_LIBRARY_PATH
    else
      unset LD_LIBRARY_PATH
    fi
    if test "$bridge_had_dyld_path" = yes; then
      DYLD_LIBRARY_PATH="$save_BRIDGE_DYLD_LIBRARY_PATH"
      export DYLD_LIBRARY_PATH
    else
      unset DYLD_LIBRARY_PATH
    fi
    if test "$pdo_bridge_identity" != yes; then
      pdo_duckdb_cleanup_staged_aliases
      AC_MSG_ERROR([DuckDB bridge header/library identity mismatch or runtime probe failure. Native execution of the matching version and source ID is required; see config.log.])
    fi
  fi
  if test -n "$DUCKDB_CONFIG_STATIC_FINAL"; then
    if ! pdo_duckdb_promote_alias "$DUCKDB_CONFIG_STATIC_STAGE" "$DUCKDB_CONFIG_STATIC_FINAL"; then
      pdo_duckdb_cleanup_staged_aliases
      AC_MSG_ERROR([Unable to promote the DuckDB static build alias.])
    fi
    DUCKDB_CONFIG_STATIC_DIR="$DUCKDB_CONFIG_STATIC_FINAL"
    DUCKDB_CONFIG_INCLUDE="$DUCKDB_CONFIG_STATIC_FINAL"
    PDO_DUCKDB_SHARED_LIBADD=`printf '%s\n' "$PDO_DUCKDB_SHARED_LIBADD" | sed "s|$DUCKDB_CONFIG_STATIC_STAGE|$DUCKDB_CONFIG_STATIC_FINAL|g"`
    PHP_ADD_INCLUDE([$DUCKDB_CONFIG_INCLUDE])
  else
    if ! pdo_duckdb_promote_aliases "$DUCKDB_CONFIG_INCLUDE_STAGE" "$DUCKDB_CONFIG_INCLUDE_FINAL" "$DUCKDB_CONFIG_LIBDIR_STAGE" "$DUCKDB_CONFIG_LIBDIR_FINAL"; then
      pdo_duckdb_cleanup_staged_aliases
      AC_MSG_ERROR([Unable to promote DuckDB build aliases.])
    fi
    DUCKDB_CONFIG_INCLUDE="$DUCKDB_CONFIG_INCLUDE_FINAL"
    DUCKDB_CONFIG_LIBDIR="$DUCKDB_CONFIG_LIBDIR_FINAL"
    PHP_ADD_INCLUDE([$DUCKDB_CONFIG_INCLUDE])
    save_DUCKDB_LDFLAGS="$LDFLAGS"
    save_PHP_RPATHS="$PHP_RPATHS"
    PHP_ADD_LIBRARY_WITH_PATH([duckdb], [$DUCKDB_CONFIG_LIBDIR], [PDO_DUCKDB_SHARED_LIBADD])
    if test "$ext_shared" = "yes"; then
      if test -n "$ld_runpath_switch"; then
        PDO_DUCKDB_SHARED_LIBADD="\"$ld_runpath_switch$DUCKDB_RUNTIME_LIBDIR\" -L$DUCKDB_CONFIG_LIBDIR -lduckdb"
      else
        PDO_DUCKDB_SHARED_LIBADD="-L$DUCKDB_CONFIG_LIBDIR -lduckdb"
      fi
    else
      LDFLAGS="$save_DUCKDB_LDFLAGS -L$DUCKDB_CONFIG_LIBDIR"
      PHP_RPATHS="$save_PHP_RPATHS"
      dnl PHP_UTILIZE_RPATHS splits paths on whitespace. Quote the runtime
      dnl path only in final-link flags, outside configure's compiler probes.
      if test -n "$ld_runpath_switch"; then
        EXTRA_LDFLAGS="$EXTRA_LDFLAGS \"$ld_runpath_switch$DUCKDB_RUNTIME_LIBDIR\""
        EXTRA_LDFLAGS_PROGRAM="$EXTRA_LDFLAGS_PROGRAM \"$ld_runpath_switch$DUCKDB_RUNTIME_LIBDIR\""
      fi
    fi
  fi

  if test "$PHP_PDO_DUCKDB_DEV" != "no"; then
    dnl -Wno-unused-parameter: PDO's handler ABI passes context args many
    dnl handlers legitimately ignore; the warning is pure noise under -Werror.
    PDO_DUCKDB_CFLAGS="-Wall -Wextra -Wno-unused-parameter -Werror"
  else
    PDO_DUCKDB_CFLAGS=""
  fi

  PDO_DUCKDB_SOURCES="pdo_duckdb.c duckdb_driver.c duckdb_statement.c duckdb_appender.c duckdb_static_extensions.c pdo_duckdb_sql.c duckdb_value.c"
  if test "$PHP_PDO_DUCKDB_BRIDGE_SOURCE" != "no"; then
    if ! pdo_duckdb_promote_alias "$DUCKDB_CONFIG_BRIDGE_STAGE" "$DUCKDB_CONFIG_BRIDGE_FINAL"; then
      pdo_duckdb_cleanup_staged_aliases
      AC_MSG_ERROR([Unable to promote the DuckDB bridge header alias.])
    fi
    PHP_ADD_INCLUDE([$DUCKDB_CONFIG_BRIDGE_FINAL])
    if test -z "$DUCKDB_CONFIG_STATIC_FINAL"; then
      case `uname -s 2>/dev/null` in
        Darwin) PDO_DUCKDB_SHARED_LIBADD="$PDO_DUCKDB_SHARED_LIBADD -lc++" ;;
        *) PDO_DUCKDB_SHARED_LIBADD="$PDO_DUCKDB_SHARED_LIBADD -lstdc++" ;;
      esac
    fi
    AC_DEFINE([HAVE_PDO_DUCKDB_BRIDGE], [1], [Whether the exact-source DuckDB C++ bridge is enabled])
    PDO_DUCKDB_SOURCES="$PDO_DUCKDB_SOURCES pdo_duckdb_bridge.cpp pdo_duckdb_bridge_query.cpp pdo_duckdb_bridge_types.cpp pdo_duckdb_bridge_variant.cpp pdo_duckdb_bridge_appender.cpp pdo_duckdb_bridge_logging.cpp pdo_duckdb_bridge_filesystem.cpp"
    CXXFLAGS="$CXXFLAGS -std=c++17"
  fi

  PHP_SUBST([PDO_DUCKDB_SHARED_LIBADD])
  PHP_NEW_EXTENSION([pdo_duckdb],
    [$PDO_DUCKDB_SOURCES],
    [$ext_shared],,[$PDO_DUCKDB_CFLAGS])

  PHP_ADD_EXTENSION_DEP(pdo_duckdb, pdo)
fi
