# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- `GEOMETRY` columns (from the spatial extension) now return the WKB bytes as an
  uppercase hex string instead of `NULL`. The value round-trips via
  `ST_GeomFromHEXWKB()`; use `ST_AsText()` in SQL for WKT.

## [0.3.0] - 2026-06-20

### Added
- Set DuckDB config options on the DSN (`duckdb:file.db;access_mode=read_only;memory_limit=2GB`)
  or with a `PDO::DUCKDB_ATTR_CONFIG` array. `open_basedir` still disables external
  file access whatever you pass.
- `getColumnMeta()` returns the real DuckDB type per column, with `precision` and
  `scale` for `DECIMAL`.
- The appender accepts PHP arrays for `LIST`, `ARRAY`, `STRUCT`, and `MAP` columns.
- `duckdbAppender()` takes an optional column list (`duckdbAppender($table, $schema, $columns)`);
  omitted columns fall back to their `DEFAULT`.
- Stream large queries instead of buffering them in memory with `PDO::DUCKDB_ATTR_UNBUFFERED`.

### Fixed
- Reading an empty `LIST`, `ARRAY`, or `MAP` no longer crashes.
- `TIMESTAMP_S`/`TIMESTAMP_MS`/`TIMESTAMP_NS`, `TIME_NS`, `UNION`, and `VARINT`
  columns now return their value instead of `NULL`.

### Security
- Bounds-check the result decoder against malformed storage files: a `UNION`
  tag, a `DECIMAL` internal-width, a zero-length `BIT`, and `LIST`/`MAP`
  `{offset,length}` entries are validated before use, so a crafted `.duckdb`
  file can no longer drive an out-of-bounds read on fetch. Not reachable from
  ordinary SQL or user rows.

## [0.2.1] - 2026-06-18

### Fixed
- Linux prebuilt binaries are now genuinely self-contained. The 0.2.0 Linux
  `.so` failed to load on a clean host (`undefined symbol:
  _ZTVN10__cxxabiv120__function_type_infoE`) because the bundled DuckDB C++
  runtime was never statically linked — the gcc C-driver link ignored the
  g++-only `-static-libstdc++` flag. The build now links the static
  `libstdc++`/`libgcc_eh` explicitly. macOS and Windows binaries were
  unaffected. 0.2.0's broken Linux assets were removed, so installs pinned to
  0.2.0 fall back to a source build on Linux.

## [0.2.0] - 2026-06-18

### Added
- Prebuilt binaries are now published with each release, so
  `pie install iliaal/pdo_duckdb` installs a ready-to-use extension with no
  local DuckDB library or build toolchain. Coverage: Linux glibc x86_64 and
  arm64, macOS Apple Silicon (arm64), and Windows x64 (thread-safe and
  non-thread-safe). PIE falls back to a source build when no prebuilt matches.
- `--with-pdo-duckdb-static=DIR` configure mode, which statically links
  DuckDB's static-libs bundle into a self-contained extension (how the Linux
  and macOS prebuilts are built; the Windows package bundles `duckdb.dll`).

## [0.1.0] - 2026-06-18

Initial release. A PDO driver for DuckDB.

### Added
- Connect to a file-backed or in-memory DuckDB database through the standard PDO
  API: `new PDO('duckdb:…')` and, on PHP 8.4+, `PDO::connect('duckdb:…')`.
- Prepared statements with positional `?` and named `:name` placeholders
  (rewritten to DuckDB `$N`; a repeated `:name` binds once).
- Result decoding via DuckDB's data-chunk/vector API: native scalars map to PHP
  `int`/`float`/`string`/`bool`; `UBIGINT`/`HUGEINT`/`DECIMAL`/temporal/`UUID`/
  `ENUM`/`BIT` and nested `LIST`/`ARRAY`/`STRUCT`/`MAP` render to their canonical
  string form. `execute()` materializes the result; rows are then fetched chunk
  by chunk.
- `Pdo\Duckdb\Appender` (via `PDO::duckdbAppender()`) for fast bulk inserts over
  DuckDB's native appender. It validates the whole row before appending and is
  marked unusable after a failed append/flush/close, per DuckDB's contract. On
  PHP 8.4+ the method lives on a driver-specific `Pdo\Duckdb` subclass.
- Transactions (`beginTransaction`/`commit`/`rollBack`), `PDO::quote()`, and
  DuckDB extension loading through ordinary SQL (`LOAD`/`INSTALL`).
- PHP 8.1 through 8.5 support.

### Security
- When `open_basedir` is set, the connection disables DuckDB's external SQL file
  access (`read_csv`/`COPY`/`ATTACH`/httpfs) via `enable_external_access=false`,
  fails closed if it cannot be applied, and re-enforces the sandbox on the live
  connection at every SQL entry point — covering persistent-connection reuse and
  `open_basedir` tightened mid-request after a connection opened.
- Embedded NUL bytes in SQL (`query`/`prepare`/`exec`) and in `duckdbAppender()`
  table/schema names are rejected, rather than silently truncating the statement
  or identifier at the NUL.

[Unreleased]: https://github.com/iliaal/pdo_duckdb/compare/0.3.0...HEAD
[0.3.0]: https://github.com/iliaal/pdo_duckdb/releases/tag/0.3.0
[0.2.1]: https://github.com/iliaal/pdo_duckdb/releases/tag/0.2.1
[0.2.0]: https://github.com/iliaal/pdo_duckdb/releases/tag/0.2.0
[0.1.0]: https://github.com/iliaal/pdo_duckdb/releases/tag/0.1.0
