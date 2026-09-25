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

pdo_duckdb_cleanup_staged_aliases() {
  rm -rf "$DUCKDB_CONFIG_STATIC_STAGE" "$DUCKDB_CONFIG_INCLUDE_STAGE" "$DUCKDB_CONFIG_LIBDIR_STAGE"
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
  if ! mkdir -p "$DUCKDB_CONFIG_STATIC_STAGE" ||
      ! cp -p "$DUCKDB_STATIC_DIR/duckdb.h" "$DUCKDB_CONFIG_STATIC_STAGE/duckdb.h"; then
    pdo_duckdb_cleanup_staged_aliases
    AC_MSG_ERROR([Unable to stage the DuckDB static bundle.])
  fi
  for DUCKDB_ARCHIVE in "$DUCKDB_STATIC_DIR"/*.a; do
    DUCKDB_ARCHIVE_NAME=${DUCKDB_ARCHIVE##*/}
    if ! ln "$DUCKDB_ARCHIVE" "$DUCKDB_CONFIG_STATIC_STAGE/$DUCKDB_ARCHIVE_NAME" 2>/dev/null; then
      cp -p "$DUCKDB_ARCHIVE" "$DUCKDB_CONFIG_STATIC_STAGE/$DUCKDB_ARCHIVE_NAME" || {
        pdo_duckdb_cleanup_staged_aliases
        AC_MSG_ERROR([Unable to stage a DuckDB static archive.])
      }
    fi
  done
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
      ! mkdir -p "$DUCKDB_CONFIG_INCLUDE_STAGE" "$DUCKDB_CONFIG_LIBDIR_STAGE" ||
      ! cp -p "$DUCKDB_INCDIR/duckdb.h" "$DUCKDB_CONFIG_INCLUDE_STAGE/duckdb.h"; then
    pdo_duckdb_cleanup_staged_aliases
    AC_MSG_ERROR([Unable to stage the DuckDB headers.])
  fi
  if ! ln "$DUCKDB_DIR/$PHP_LIBDIR/libduckdb.so" "$DUCKDB_CONFIG_LIBDIR_STAGE/libduckdb.so" 2>/dev/null &&
      ! cp -p "$DUCKDB_DIR/$PHP_LIBDIR/libduckdb.so" "$DUCKDB_CONFIG_LIBDIR_STAGE/libduckdb.so"; then
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
  save_CPPFLAGS="$CPPFLAGS"
  dnl AC_CHECK_DECLS expands CPPFLAGS as shell words; use the stable,
  dnl whitespace-free symlink created for the compiler build flags.
  CPPFLAGS="$CPPFLAGS -I$DUCKDB_CONFIG_INCLUDE"
  AC_CHECK_DECLS([DUCKDB_TYPE_VARIANT], [],
    [pdo_duckdb_cleanup_staged_aliases; AC_MSG_ERROR([DuckDB 1.5.3 or newer headers are required (DUCKDB_TYPE_VARIANT is missing).])],
    [[#include <duckdb.h>]])
  CPPFLAGS="$save_CPPFLAGS"
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
    save_PHP_RPATHS="$PHP_RPATHS"
    PHP_ADD_LIBRARY_WITH_PATH([duckdb], [$DUCKDB_CONFIG_LIBDIR], [PDO_DUCKDB_SHARED_LIBADD])
    if test "$ext_shared" = "yes"; then
      if test -n "$ld_runpath_switch"; then
        PDO_DUCKDB_SHARED_LIBADD="\"$ld_runpath_switch$DUCKDB_RUNTIME_LIBDIR\" -L$DUCKDB_CONFIG_LIBDIR -lduckdb"
      else
        PDO_DUCKDB_SHARED_LIBADD="-L$DUCKDB_CONFIG_LIBDIR -lduckdb"
      fi
    else
      PHP_RPATHS="$save_PHP_RPATHS $DUCKDB_RUNTIME_LIBDIR"
    fi
  fi

  if test "$PHP_PDO_DUCKDB_DEV" != "no"; then
    dnl -Wno-unused-parameter: PDO's handler ABI passes context args many
    dnl handlers legitimately ignore; the warning is pure noise under -Werror.
    PDO_DUCKDB_CFLAGS="-Wall -Wextra -Wno-unused-parameter -Werror"
  else
    PDO_DUCKDB_CFLAGS=""
  fi

  PHP_SUBST([PDO_DUCKDB_SHARED_LIBADD])
  PHP_NEW_EXTENSION([pdo_duckdb],
    [pdo_duckdb.c duckdb_driver.c duckdb_statement.c duckdb_appender.c],
    [$ext_shared],,[$PDO_DUCKDB_CFLAGS])

  PHP_ADD_EXTENSION_DEP(pdo_duckdb, pdo)
fi
