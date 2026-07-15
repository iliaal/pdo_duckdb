# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed
- Reject callback-capable `PDO::DUCKDB_ATTR_CONFIG` values before conversion,
  preventing Stringable reentrancy during connection setup.
- Synchronize raw SQL transaction control with PDO state, including prepared
  statements, partially failing multi-statements, and DuckDB's `END` / `ABORT`
  aliases. Track transaction control wrapped by `EXPLAIN ANALYZE` explicitly
  because DuckDB executes it while exposing only the outer `EXPLAIN` type.
- Report non-NULL `VARIANT` fetches as unsupported instead of silently returning
  PHP `null`; explicit SQL casts and genuine NULL cells continue to work.
- Render 1 BC DATE/TIMESTAMP values through DuckDB's canonical formatter rather
  than emitting the nonexistent year zero.
- Accept PHP integer array keys for numeric STRUCT field names in Appender rows.
- Reject unsupported scroll cursors during `prepare()` rather than on the first
  non-forward fetch.
- Clear partial DuckDB parameter bindings after a failed bind round, so the next
  `execute([])` cannot reuse values from the failed attempt.
- Keep native error messages statement-local, so one statement's `errorInfo()`
  is not overwritten by a later failure on another statement.
- Apply the `open_basedir` sandbox latch before creating an appender, matching
  the other SQL/catalog entry points.
- Return `UNKNOWN` rather than `VARCHAR` from `getColumnMeta()` for future
  DuckDB type ids the driver does not know yet.
- Release unbuffered results from `closeCursor()`, allowing another statement
  to run while a streaming result is partially consumed.
- Surface DuckDB fetch errors that arrive when `duckdb_fetch_chunk()` returns no
  chunk, rather than treating them as ordinary end-of-result.

### Security
- Harden the `open_basedir` DuckDB sandbox profile by rejecting path and
  extension-loading config keys, clearing runtime path allowlists before
  disabling external access, and locking further DuckDB configuration changes.
- Redact raw `duckdb_open_ext()` failure details from connection exceptions, so
  filesystem paths and OS error text from DuckDB are not exposed.
- Pass release tags and package names to Linux/macOS and Windows release scripts
  through environment variables and validate their filename grammar, preventing
  GitHub expression values from becoming shell or PowerShell source code.
- Build Linux release archives inside pinned Debian 12/PHP containers and macOS
  archives with an 11.0 deployment target, then gate the final archive's glibc
  and Mach-O minimum versions. Stage every platform artifact before one
  least-privilege publisher uploads the complete set.

### Performance
- Cache nested render descriptors and STRUCT names per result column, and bypass
  portable 128-bit division for 64-bit values.
- Add direct string renderers for `TIME_TZ`, `TIMESTAMP_TZ`, monthless
  `INTERVAL`, and `BIT` result cells, avoiding per-cell `duckdb_value`
  reconstruction on those scalar types.
- Add a cached direct renderer for nested `LIST`/`ARRAY`/`STRUCT`/`MAP`/`UNION`
  values whose leaves are safe to render without DuckDB's quoting rules
  (booleans, integers, `DECIMAL`, `DATE`, and `UUID`). Nested string-like,
  floating-point, and time-like leaves keep the DuckDB renderer.

### Tests
- Add CI coverage for the advertised DuckDB 1.5.3 source-build floor and
  for the portable integer renderers without compiler `int128` support, plus
  regression coverage for config reentrancy, raw persistent transactions,
  transaction aliases and executing `EXPLAIN` transaction wrappers, BC temporal values,
  top-level and nested VARIANT errors, numeric STRUCT fields, and cursor modes.
- Add regression coverage for `PDO::quote()`, sandbox config and allowlists,
  persistent-option reuse, unbuffered cursor cleanup, appender destructor
  warnings, statement-execute sandboxing, interleaved unbuffered streams,
  extended scalar rendering, nested direct rendering, statement-local error
  messages, and failed-bind cleanup.

## [0.4.1] - 2026-07-03

### Fixed
- Reject embedded NUL bytes in `PDO::DUCKDB_ATTR_CONFIG` option names and values,
  which previously truncated silently and could apply an unintended option.
- Clear DuckDB prepared-statement bindings once per `execute()`, so re-running with
  fewer parameters reports the missing parameter instead of reusing a stale value.
- Unregister the PDO driver in MINIT when appender initialization fails, instead
  of leaving a half-registered `duckdb` driver.
- Report `rowCount()` as 0 for a non-DML statement in unbuffered mode instead of
  reading `rows_changed` on a streaming SELECT result.
- Prevalidate scalar integer ranges in the appender so an out-of-range value is
  rejected before it poisons the native appender state.

### Security
- Redact DSN and config values from all connection-error messages, so secrets in
  the DSN tail or `PDO::DUCKDB_ATTR_CONFIG` no longer leak into exception text.

### Performance
- Cache result column types and add a stack-buffer fast path for string-rendered
  types (HUGEINT, DECIMAL, UUID, DATE/TIME/TIMESTAMP, ENUM), cutting per-cell
  allocations (~3.6x faster conversion of large results of these types). The
  appender also skips a per-cell type-id lookup.

## [0.4.0] - 2026-06-21

### Added
- `GEOMETRY` columns now return WKB bytes as an uppercase hex string instead of `NULL`
  (round-trips via `ST_GeomFromHEXWKB()`; use `ST_AsText()` for WKT).
- `duckdbTableNames(string $query, bool $qualified = false)` returns the tables a
  query references, using DuckDB's parser.
- `duckdbLastProfile()` returns the last query's profiling tree (operator timings,
  cardinalities) as a nested array, or `null` until `PRAGMA enable_profiling` is set.

### Fixed
- A connection that fails because the database path can't be resolved no longer
  reports it as an `open_basedir` denial; the two cases now give distinct messages.

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

[Unreleased]: https://github.com/iliaal/pdo_duckdb/compare/0.4.1...HEAD
[0.4.1]: https://github.com/iliaal/pdo_duckdb/releases/tag/0.4.1
[0.4.0]: https://github.com/iliaal/pdo_duckdb/releases/tag/0.4.0
[0.3.0]: https://github.com/iliaal/pdo_duckdb/releases/tag/0.3.0
[0.2.1]: https://github.com/iliaal/pdo_duckdb/releases/tag/0.2.1
[0.2.0]: https://github.com/iliaal/pdo_duckdb/releases/tag/0.2.0
[0.1.0]: https://github.com/iliaal/pdo_duckdb/releases/tag/0.1.0
