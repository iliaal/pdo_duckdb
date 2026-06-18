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

### Security
- When `open_basedir` is set, connections disable DuckDB's external SQL file
  access (`read_csv`/`COPY`/`ATTACH`/httpfs) via `enable_external_access=false`,
  and fail closed if that configuration cannot be applied.

[Unreleased]: https://github.com/iliaal/pdo_duckdb/compare/HEAD...HEAD
