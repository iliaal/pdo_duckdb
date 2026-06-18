PHP_ARG_WITH([pdo-duckdb],
  [for DuckDB support for PDO],
  [AS_HELP_STRING([--with-pdo-duckdb=DIR],
    [PDO: DuckDB support. DIR is the DuckDB install prefix containing
     include/duckdb.h and lib/libduckdb.])])

PHP_ARG_ENABLE([pdo-duckdb-dev],
  [whether to enable developer build flags for pdo_duckdb],
  [AS_HELP_STRING([--enable-pdo-duckdb-dev],
    [PDO_DUCKDB: build with -Wall -Wextra -Werror for release verification])],
  [no],
  [no])

if test "$PHP_PDO_DUCKDB" != "no"; then
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

  if test "$PHP_PDO_DUCKDB_DEV" != "no"; then
    dnl -Wno-unused-parameter: PDO's handler ABI passes context args many
    dnl handlers legitimately ignore; the warning is pure noise under -Werror.
    PDO_DUCKDB_CFLAGS="-Wall -Wextra -Wno-unused-parameter -Werror"
  else
    PDO_DUCKDB_CFLAGS=""
  fi

  PHP_SUBST([PDO_DUCKDB_SHARED_LIBADD])
  PHP_NEW_EXTENSION([pdo_duckdb],
    [pdo_duckdb.c duckdb_driver.c duckdb_statement.c],
    [$ext_shared],,[$PDO_DUCKDB_CFLAGS])

  PHP_ADD_EXTENSION_DEP(pdo_duckdb, pdo)
fi
