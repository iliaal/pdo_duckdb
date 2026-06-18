# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[Unreleased]: https://github.com/iliaal/pdo_duckdb/compare/0.1.0...HEAD
[0.1.0]: https://github.com/iliaal/pdo_duckdb/releases/tag/0.1.0
