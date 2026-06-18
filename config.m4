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
  AC_MSG_RESULT([found in $DUCKDB_STATIC_DIR])

  PHP_REQUIRE_CXX()
  PHP_ADD_INCLUDE([$DUCKDB_STATIC_DIR])

  dnl Pass the whole archive set as a single comma-joined -Wl, token. libtool
  dnl otherwise reorders/de-duplicates loose .a arguments and drops archives
  dnl (symptom: a smaller module and an undefined duckdb::… symbol at load); one
  dnl -Wl, token is forwarded to the compiler verbatim.
  DUCKDB_STATIC_ARCHIVES=`echo $DUCKDB_STATIC_DIR/*.a | tr ' ' ','`
  case `uname -s 2>/dev/null` in
    Darwin)
      dnl macOS: ld64 is multi-pass (no --start-group). libtool links the bundle
      dnl with `cc -undefined suppress`, so the DuckDB C++ runtime symbols are
      dnl NOT auto-resolved — link libc++ explicitly (a system dylib; dynamic is
      dnl fine, no GLIBCXX-style portability concern on macOS).
      PDO_DUCKDB_SHARED_LIBADD="-Wl,$DUCKDB_STATIC_ARCHIVES -lc++"
      ;;
    *)
      dnl GNU ld: --start-group resolves the circular references between the
      dnl DuckDB archives; pull libstdc++/libgcc in statically so the module is
      dnl independent of the host's GLIBCXX/libgcc ABI (matters for prebuilts).
      PDO_DUCKDB_SHARED_LIBADD="-Wl,--start-group,$DUCKDB_STATIC_ARCHIVES,--end-group -static-libstdc++ -static-libgcc"
      ;;
  esac

elif test "$PHP_PDO_DUCKDB" != "no"; then
  PHP_CHECK_PDO_INCLUDES

  if test "$PHP_PDO_DUCKDB" = "yes"; then
    SEARCH_PATH="/usr/local /usr /opt/duckdb /opt/homebrew /usr/local/opt/duckdb"
  else
    SEARCH_PATH="$PHP_PDO_DUCKDB"
  fi

  AC_MSG_CHECKING([for duckdb.h])
  DUCKDB_DIR=""
  DUCKDB_INCDIR=""
  for i in $SEARCH_PATH; do
    if test -r "$i/include/duckdb.h"; then
      DUCKDB_DIR=$i
      DUCKDB_INCDIR=$i/include
      break
    elif test -r "$i/duckdb.h"; then
      DUCKDB_DIR=$i
      DUCKDB_INCDIR=$i
      break
    fi
  done

  if test -z "$DUCKDB_DIR"; then
    AC_MSG_RESULT([not found])
    AC_MSG_ERROR([Cannot find duckdb.h. Install the DuckDB C library, or point at it with --with-pdo-duckdb=DIR])
  fi
  AC_MSG_RESULT([found in $DUCKDB_INCDIR])

  PHP_ADD_INCLUDE([$DUCKDB_INCDIR])
  PHP_ADD_LIBRARY_WITH_PATH([duckdb], [$DUCKDB_DIR/$PHP_LIBDIR], [PDO_DUCKDB_SHARED_LIBADD])

  PHP_CHECK_LIBRARY([duckdb], [duckdb_open],
    [],
    [AC_MSG_ERROR([Could not find a usable libduckdb. Check config.log for details.])],
    [-L$DUCKDB_DIR/$PHP_LIBDIR])
fi

if test "$PHP_PDO_DUCKDB" != "no"; then
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
