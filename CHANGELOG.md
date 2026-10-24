# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- Under `open_basedir`, stop refusing a connection that is sandboxed after it
  opened just because another process was installing a DuckDB extension into
  the same extension directory at that moment. Listing the loaded extensions
  now retries DuckDB IO errors for up to half a second before refusing, and a
  refusal for an unlistable extension set now includes DuckDB's error.

## [0.8.0] - 2026-10-16

### Security

- Keep an unclosed `Pdo\Duckdb\Appender` from reading its PDO connection after
  garbage collection or request shutdown has freed it; the appender's native
  state is now released before the connection closes.
- Initialize DuckDB type handles before publishing them, so running out of
  `memory_limit` while creating an appender or caching result columns no longer
  frees uninitialized handles.
- Reject a row that holds a non-NULL `VARIANT` value before any of its columns
  are read, so a warning-mode error handler or output callback can no longer
  run while PDO is still reading the row.
- Under `open_basedir`, refuse a connection that is sandboxed after it opened
  (the limit was tightened at runtime, or a persistent connection is reused)
  when a DuckDB extension the sandbox can't govern is loaded, and stop that
  instance's quack listeners. The trusted extensions are autocomplete,
  core_functions, excel, fts, httpfs, icu, inet, json, parquet, spatial, tpcds,
  tpch, and vss. A refused persistent connection is replaced with a freshly
  sandboxed one.
- Under `open_basedir`, stop writing to a DuckDB file log sink that was enabled
  before the sandbox applied, and keep `CALL enable_logging()` from re-arming it.
  Tightening `open_basedir` on an open handle now sets
  `logging_storage='memory'` on all DuckDB versions, so a file log silently
  stops receiving entries; read `duckdb_logs` instead.
- On DuckDB 2.0 builds under `open_basedir`, reject the
  `allow_extension_repositories` and `extension_repository_directory` options
  and forbid extension repositories.

### Added

- Build against the DuckDB 2.0 preview. Configure detects `duckdb_v2.h` next to
  the selected `duckdb.h`, defines `HAVE_DUCKDB_V2`, and stops when the headers
  and library come from different releases. Builds against DuckDB 1.5.x are
  unchanged.
- Run the test suite in a blocking CI lane against a DuckDB 2.0 preview pinned
  by URL and SHA-256.
- On DuckDB 2.0 builds, fetch `TIMESTAMPTZ_NS` values at the top level and inside
  nested values as their UTC string instead of `null`, report `TIMESTAMP_TZ_NS`
  from `getColumnMeta()`, and check `TIMESTAMPTZ_NS` values before appending
  them.
- Document DuckDB 2.0 behavior that affects PDO users: storage-version interop
  (`storage_compatibility_version`, `ATTACH ... (STORAGE_VERSION ...)`),
  transaction invalidation after errors, division by zero, lambda syntax, the
  profiling layout, unbuffered error timing, and a DuckDB 2.0 defect that makes
  `query()` and `prepare()` fail while a connection is `CONNECT`ed.

### Changed

- Build, test, and ship against DuckDB 1.5.6 (was 1.5.5). Source builds still
  accept DuckDB 1.5.3 or newer.
- A row holding a non-NULL `VARIANT` value now fails the fetch: `fetch()`
  returns `false`, or throws under `ERRMODE_EXCEPTION`, instead of returning the
  cell as `null`. A `while ($row = $stmt->fetch())` loop stops at that row, so
  cast `VARIANT` columns to `VARCHAR` in SQL or check `errorInfo()` after the
  loop.
- An unclosed `Pdo\Duckdb\Appender` flushes its buffered rows when it or its
  connection is torn down, including after a fatal error. When PDO rolls back
  an open transaction at teardown, the appender's buffered rows are discarded
  with it, whichever object PHP frees first. Rows left buffered by a failed
  append, flush, or close are discarded instead of being flushed later.
- On DuckDB 2.0 builds, report `PDO::PARAM_NULL` for columns of DuckDB's `NULL`
  type, and raise a PDO error instead of returning `null` for a non-NULL value
  of a type the build doesn't recognize.

### Fixed

- Closing a connection no longer aborts the PHP process when a DuckDB file log
  sink can't write its files; the failed query still raises a `PDOException`.
- Compute bind positions without signed overflow for `execute()` array keys at
  `PHP_INT_MAX`; such keys still fail as out-of-range parameters.
- Quote nested `TIME` and `TIMESTAMP` values in `LIST`, `STRUCT`, and `MAP`
  exactly as `CAST(... AS VARCHAR)` does, for example
  `['2020-01-01 10:00:00']`.
- Report invalid UTF-8 in a nested appender string as
  `contains a string that is not valid UTF-8` instead of
  `is of unsupported type string`.
- Don't leak nested-type render descriptors when `memory_limit` runs out while
  caching result columns.
- On DuckDB 2.0 builds, fetch an empty `STRUCT` nested next to other fields
  instead of returning the whole value as `null`, and accept `[]` for it in the
  appender.
- Read the open_basedir sandbox helpers' query results through DuckDB's
  data-chunk API, which avoids a crash with DuckDB 2.0 libraries.
- On DuckDB 2.0 builds, discard a reused persistent connection that is still
  `CONNECT`ed to a remote database instead of sending its reset statements to
  the remote.
- Stop every quack listener of a refused connection, including one whose URI
  contains a NUL byte, and name the reason when an attached database name or
  path containing a NUL byte blocks the sandbox.
- Correct the streaming notes: another statement on the connection aborts a
  partially read unbuffered result. Under `ERRMODE_SILENT` and `ERRMODE_WARNING`,
  check `errorCode()` right after the fetch loop ends.
- Document that NUL bytes truncate nested `VARCHAR` members and how the
  appender handles invalid UTF-8, including the top-level `VARCHAR` case that
  loses the whole buffered batch.
- Return an empty array from `PDO::duckdbTableNames()` for valid `UPDATE`,
  `DELETE`, and `MERGE` statements instead of throwing.
- Check the complete PHP module listing before loading shared PDO in the common
  test runner, avoiding duplicate loads after an early pipe close, and stop if
  the module probe fails.
- Preserve apostrophes and backslashes in the PDO common test directory and
  database path instead of generating invalid or altered redirect PHP.
- Reject finite floating-point overflow in nested appender `FLOAT` values instead
  of silently storing infinity; validation failures leave the appender usable.
- Reject booleans, integers, and floats that DuckDB cannot cast to their
  appender column before appending them, instead of invalidating the appender
  and discarding its buffered rows. This covers `FLOAT` overflow, out-of-range
  values for integer and `DECIMAL` columns, and numbers or booleans for
  temporal, `UUID`, and `BLOB` columns.
- Clear stale statement results when parameter binding fails during `execute()`,
  while preserving active results and row counts after failed standalone
  `bindValue()` or `bindParam()` calls.
- Reject empty or entirely skipped ASan test runs in the local helper and CI,
  instead of treating a successful runner process as runtime validation.
- Match DuckDB's dollar-quote tag grammar, including non-ASCII identifier bytes.
- Support DuckDB prefixes containing spaces in shared and static builds, including
  out-of-tree builds, macOS shared libraries, relative library symlinks, and
  rebuilds after `make clean`. Run configure regression probes in an isolated
  directory so they cannot remove libraries or alter the source checkout.
- Accept a directory value for the `--with-pdo-duckdb-static` PIE option, which
  configure requires; PIE previously rejected or dropped the bundle path.
- Finish macOS load-command inspection before selecting the minimum version,
  preventing broken-pipe failures in binary validation and release archive checks.

### Known limitation

- PHP PDO converts execute-array strings before invoking driver hooks; immediate
  metadata/row-count reset for first and later conversion failures remains an
  explicit XFAIL until php-src exposes a conversion-failure hook.

## [0.7.1] - 2026-09-04

### Fixed
- Free substituted GEOMETRY child types in nested `STRUCT`/`UNION` containers
  (24-byte logical-type leak per fetch, LSan-clean).

## [0.7.0] - 2026-09-04

### Security
- Reject `PDO::PARAM_LOB` binds over 64MB (worker OOM guardrail); non-seekable
  streams bind the remainder on re-execute instead of failing.
- Reject appender rows nested deeper than 128 levels (C stack guardrail).

### Added
- Driver error taxonomy in `errorInfo()[1]`: 1 general, 2/`08000` connect,
  3/`42000` syntax, 4 sandbox, 5 streaming.

### Changed
- Probe-caught appender cast failures throw without poisoning the appender;
  native-path failures still invalidate it. Documented exception matrix.

### Fixed
- Nested `GEOMETRY` containers fetch as uppercase-hex renders, not NULL.

## [0.6.0] - 2026-08-30

### Security
- Reject `force_mbedtls_unsafe` at connect time; a falsy value SIGSEGVs
  libduckdb 1.5.3–1.5.5 (`ForceMbedtlsUnsafeSetting::SetGlobal` on a NULL
  `DatabaseInstance`). `SET` after open still works.
- Reject DECIMAL types whose width/scale fall outside DuckDB's
  `1 <= width <= 38 && scale <= width` before the fixed-buffer fast renderer.
  Crafted catalog/Parquet metadata can present any `uint8` scale; the renderer
  wrote `2+scale` bytes into a 43-byte stack buffer.
- Persistent checkout now closes the DuckDB instance before a liveness FAILURE
  (PDO core can leak the discarded persistent dbh).
- Persistent checkout disables profiling and `RESET`s `http_proxy` /
  `http_proxy_username` / `http_proxy_password` so the next request cannot steer
  httpfs through another request's proxy. httpfs-only TLS settings are reset
  only if httpfs is already loaded; `RESET` of those options otherwise autoloads
  the extension.
- Convert parameters in EXEC_PRE on a local zval copy, so a `__toString` or
  stream callback that re-enters `execute()` can't make the driver write through
  a freed bound-param struct. Re-entrant `execute()` still crashes inside PDO
  core, which caches `bound_params` across the conversion; don't call
  `execute()` from `__toString()` on the same statement.

### Changed
- A `PDO::PARAM_LOB` stream is rewound after the driver drains it, so
  re-executing a statement bound to the same stream binds the same bytes
  instead of an empty value.
- A `PDO::PARAM_STR` / `PDO::PARAM_INT` value bound by reference with
  `bindParam()` is now converted from the variable's current value on every
  `execute()`. Previously the first conversion was written back into the bound
  parameter, so later executes reused the stale converted value.

## [0.5.0] - 2026-07-26

### Changed
- Build, test, and ship against DuckDB 1.5.5. The prebuilt binaries link it; a
  source build still works against the advertised 1.5.3 floor.

### Fixed
- Render `DECIMAL` values whose type has no integer digits (`width == scale`,
  e.g. `DECIMAL(2,2)`) as DuckDB does: `.05`, not `0.05`. Affected scalar cells
  since 0.4.1 and nested `LIST`/`ARRAY`/`STRUCT`/`MAP` cells since the direct
  nested renderer was added.
- Rebuild `PDOStatement` column metadata after re-execute when columns were
  freed by `set_column_count(0)`, fixing a SEGV in `getColumnMeta()` without an
  intervening fetch.
- Open-time `open_basedir` sandbox for file-backed DBs now runs the full
  post-connect escalate sequence (empty `temp_directory`, clear allowlists,
  then external access off + lock) so DuckDB cannot permanently allowlist
  `{db}.tmp` for SQL FS.
- Runtime open_basedir escalate no longer seeds `temp_directory` with the first
  basedir entry (which DuckDB permanently allowlisted, allowing in-basedir
  `read_csv` and surviving basedir re-narrow). Temp is cleared instead; a
  re-narrowed basedir after escalate fails closed. Fetch re-latches the sandbox
  for unbuffered streams; appender destruction latches before implicit close.
- Map `PDO::DUCKDB_ATTR_CONFIG` PHP bools to DuckDB `"true"`/`"false"`; treat
  string `"0"` as false for `UNBUFFERED`/`AUTOCOMMIT`. Clear sticky `errorInfo`
  on successful `quote`, avoid double WARNING on quote NUL, message
  `PDO_PARAM_STMT` rejects, fail LOB binds when stream read fails, and zero
  `columnCount` when a result is torn down. Reset the bind latch if EXEC_PRE
  leaves an exception.
- Escalate the `open_basedir` sandbox from `beginTransaction()` / `commit()` /
  `rollBack()` and from live `Appender` methods (`appendRow` / `flush` / `close`),
  matching prepare/exec so sticky writers and OOB attachments cannot survive a
  mid-request basedir tighten.
- Clear sticky driver `errorInfo` payloads on successful operations and on
  persistent `check_liveness`, and record quote NUL rejections through the
  normal error path under all ERRMODE values.
- Zero `PDOStatement::rowCount()` when a re-execute fails after a prior success.
- Guard ARRAY slow-path reconstruction against `row * n` overflow (parity with
  the fast nested renderer) and reject out-of-range ENUM dictionary indices on
  the slow path.
- Align `getColumnMeta()` `pdo_type` for `BOOLEAN` with fetch (`PDO::PARAM_INT`
  / PHP int 0/1) and document cast-failure appender poison plus profile metric
  nullability.
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
- Release the native result (including unbuffered streams) after a mid-fetch
  DuckDB error so the connection is not pinned until a later re-execute.
- Reset the per-execute bind latch when sandbox escalate fails after `EXEC_PRE`,
  so the next execute does not skip `duckdb_clear_bindings`.
- Drop the latent `in_txn` toggle for unclassified `TRANSACTION` statements;
  only explicit OPEN/CLOSE effects update PDO transaction state.

### Security
- On open-time and runtime/`check_liveness` open_basedir sandbox, clear
  `temp_directory` (to empty, never the basedir root), clear sticky
  `log_query_path` / `profiling_output` writers, clear `allowed_configs` and
  path allowlists, and `DETACH` out-of-basedir attachments before
  `enable_external_access=false` (DuckDB's OnSet re-allowlists non-empty temp
  and attached paths after that flip and they cannot be cleared afterward).
  Re-narrowing `open_basedir` after sandbox fails closed.
- Reject `allowed_configs` under open_basedir at connect time (lock-bypass list).
- Harden the `open_basedir` DuckDB sandbox profile by rejecting path and
  extension-loading config keys, applying the full escalate sequence after
  connect for file-backed DBs, and locking further DuckDB configuration changes.
  The lock blocks every later `SET`, including security-irrelevant ones
  (`threads`, `memory_limit`, …); pass those as connect-time DSN or
  `PDO::DUCKDB_ATTR_CONFIG` options instead. `TimeZone` has no connect-time
  equivalent and is therefore not settable under `open_basedir`.
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
- Re-latch the `open_basedir` sandbox once per fetched data chunk instead of once
  per row, and compare the recorded basedir by string rather than by hash. Under
  `open_basedir` the per-row cost no longer scales with the basedir length: a
  400k-row scan with a 223-character `open_basedir` went from 133 ns/row back to
  51 ns/row, and bulk `Appender::appendRow()` from 196 ns/row back to 96 ns/row.
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
- Add regression coverage for `DECIMAL(w,w)` canonical rendering (scalar, list,
  and struct) and for a mid-fetch `open_basedir` re-narrow failing closed at the
  next chunk pull.
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
- Linux prebuilt binaries are now self-contained. The 0.2.0 Linux `.so` failed
  to load on a clean host (`undefined symbol:
  _ZTVN10__cxxabiv120__function_type_infoE`) because the gcc C-driver link
  ignored the g++-only `-static-libstdc++` flag; the build now links static
  `libstdc++`/`libgcc_eh` explicitly. macOS and Windows were unaffected. The
  broken 0.2.0 Linux assets were removed, so 0.2.0 pins fall back to a source
  build on Linux.

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
  connection at every SQL entry point, which covers persistent-connection reuse
  and `open_basedir` tightened mid-request after a connection opened.
- Embedded NUL bytes in SQL (`query`/`prepare`/`exec`) and in `duckdbAppender()`
  table/schema names are rejected, rather than silently truncating the statement
  or identifier at the NUL.

[Unreleased]: https://github.com/iliaal/pdo_duckdb/compare/0.8.0...HEAD
[0.8.0]: https://github.com/iliaal/pdo_duckdb/releases/tag/0.8.0
[0.7.1]: https://github.com/iliaal/pdo_duckdb/releases/tag/0.7.1
[0.7.0]: https://github.com/iliaal/pdo_duckdb/releases/tag/0.7.0
[0.6.0]: https://github.com/iliaal/pdo_duckdb/releases/tag/0.6.0
[0.5.0]: https://github.com/iliaal/pdo_duckdb/releases/tag/0.5.0
[0.4.1]: https://github.com/iliaal/pdo_duckdb/releases/tag/0.4.1
[0.4.0]: https://github.com/iliaal/pdo_duckdb/releases/tag/0.4.0
[0.3.0]: https://github.com/iliaal/pdo_duckdb/releases/tag/0.3.0
[0.2.1]: https://github.com/iliaal/pdo_duckdb/releases/tag/0.2.1
[0.2.0]: https://github.com/iliaal/pdo_duckdb/releases/tag/0.2.0
[0.1.0]: https://github.com/iliaal/pdo_duckdb/releases/tag/0.1.0
