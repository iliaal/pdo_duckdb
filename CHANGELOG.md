# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- PHP 8.1 and 8.2 support; the minimum supported version is now 8.1.

### Changed
- `PDO::ATTR_AUTOCOMMIT` now rejects an attempt to disable autocommit rather
  than silently accepting it. DuckDB is autocommit-by-default with no
  session-level toggle; use `beginTransaction()` for explicit transactions.

### Fixed
- `Pdo\Duckdb\Appender::appendRow()` now validates the column count and every
  value's type before appending anything, so a bad row no longer leaves the
  native appender mid-row (which previously poisoned it — later appends failed
  with "too many appends"/"incomplete append"). A failed native append now marks
  the appender unusable per DuckDB's invalidation contract.
- Switched the appender error path from the deprecated `duckdb_appender_error`
  to `duckdb_appender_error_data`.

### Security
- When `open_basedir` is set, connections disable DuckDB's external SQL file
  access (`read_csv`/`COPY`/`ATTACH`/httpfs) via `enable_external_access=false`,
  and fail closed if that configuration cannot be applied.
- A persistent connection cached without `open_basedir` can no longer be reused
  to bypass a later-tightened `open_basedir`: on reuse the driver escalates the
  live connection (disables external access), so external file access is denied
  even when `handle_factory` is skipped on the cached handle.

[Unreleased]: https://github.com/iliaal/pdo_duckdb/compare/HEAD...HEAD
