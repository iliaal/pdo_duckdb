# Security policy

pdo_duckdb is a PDO driver exposing DuckDB, an in-process analytical
database, to PHP through the standard PDO API. The realistic threat
surface is SQL reaching the DuckDB engine, opening untrusted `.db`
files or DSN config options (DuckDB can read and write local files and
load extensions), and the C-to-DuckDB-C-API marshaling boundary in the
driver.

## Supported versions

| Version | Supported          |
|---------|--------------------|
| 0.4.x   | :white_check_mark: |

Pre-1.0: the current minor gets security fixes, and the API may still
move between minors until 1.0.

## Reporting a vulnerability

**Do not file a public GitHub issue for security vulnerabilities.**

Use GitHub's private security advisory feature at
<https://github.com/iliaal/pdo_duckdb/security/advisories/new>
or email Ilia Alshanetsky <ilia@ilia.ws> directly.

Please include:

- Affected pdo_duckdb version (`php -r 'echo phpversion("pdo_duckdb");'`)
- DuckDB library version. Prebuilt binaries statically link a pinned
  DuckDB; source builds use your system libduckdb.
- PHP version (`php -v`)
- A minimal reproducing case (PHP code plus the DSN, query, or `.db`
  fixture that triggers it, small enough to inline in the report)
- Impact: crash / RCE / info disclosure / DoS / etc.
- Whether you've coordinated disclosure with anyone else

Acknowledgement within 7 days, fix or status update within 30. Once a
fix is released the advisory becomes public.

## Scope

In scope:

- Crashes, memory corruption, or read-after-free in the driver's own
  code (`pdo_duckdb.c`, `duckdb_driver.c`, `duckdb_statement.c`,
  `duckdb_appender.c`) reachable through PDO: `PDO::__construct` with a
  `duckdb:` DSN, `query()`, `prepare()`/`execute()` with bound
  parameters, `getColumnMeta()`, and the driver methods
  `duckdbAppender()`, `duckdbTableNames()`, `duckdbLastProfile()`.
- Type-conversion and appender bugs (`Pdo\Duckdb\Appender::appendRow()`
  and the DuckDB-value-to-PHP marshaling) when a result or bound
  parameter declares unusual types, widths, or NULL patterns.
- `open_basedir` bypasses. On the database file, the driver runs the
  `duckdb:` DSN path through `php_check_open_basedir()` before opening
  it. When `open_basedir` is set, it applies a locked DuckDB sandbox
  profile: external file access is disabled (`enable_external_access=false`),
  path allowlists are rejected or cleared, extension auto-install/load is
  disabled, and runtime configuration changes are blocked. SQL-level reads
  such as `read_csv`, `COPY`, `ATTACH`, and extension loading are blocked
  entirely, including for paths inside `open_basedir`. A bypass of either
  gate is in scope.
- Parameter-binding flaws that break the prepared-statement boundary (a
  bound value altering statement structure).
- Arginfo / ZPP mismatches that cause undefined behavior reachable from
  PHP.

Out of scope:

- SQL injection in application code. Prepared statements are supported;
  concatenating user input into SQL is the application's
  responsibility, not a driver bug.
- Vulnerabilities in DuckDB itself. Report those to the DuckDB project.
  Prebuilt binaries statically link a pinned DuckDB release, so a
  DuckDB-side fix ships here as a rebuild once upstream releases it.
- Consequences of deliberately opening an untrusted `.db` file with
  file access enabled, or loading a DuckDB extension, when
  `open_basedir` is not set. Treat an untrusted database file as any
  other untrusted input and constrain it at the application layer.
- Isolation between PDO persistent handles with the same DSN. Persistence
  reuses the DuckDB connection/session by design, so catalog state,
  attachments, and `SET` options can survive across requests in the same
  PHP process.
