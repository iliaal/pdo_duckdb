# pdo_duckdb

[![Tests](https://github.com/iliaal/pdo_duckdb/actions/workflows/tests.yml/badge.svg)](https://github.com/iliaal/pdo_duckdb/actions/workflows/tests.yml)
[![Version](https://img.shields.io/github/v/release/iliaal/pdo_duckdb)](https://github.com/iliaal/pdo_duckdb/releases)
[![License: BSD-3-Clause](https://img.shields.io/badge/License-BSD--3--Clause-green.svg)](LICENSE)
[![Follow @iliaa](https://img.shields.io/badge/Follow-@iliaa-000000?style=flat&logo=x&logoColor=white)](https://x.com/intent/follow?screen_name=iliaa)

![pdo_duckdb: a PDO driver for DuckDB](images/pdo_duckdb-hero.jpg)

A [PDO](https://www.php.net/pdo) driver for [DuckDB](https://duckdb.org/), the
in-process analytical (OLAP) database. Connect to DuckDB through the standard
PDO API you already use for SQLite, MySQL, and PostgreSQL.

```php
$db = new PDO('duckdb:/path/to/analytics.duckdb');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

$stmt = $db->prepare('SELECT region, SUM(amount) AS total FROM sales WHERE year = ? GROUP BY region');
$stmt->execute([2026]);
foreach ($stmt as $row) {
    printf("%s: %s\n", $row['region'], $row['total']);
}
```

## Requirements

- PHP 8.1 or newer with the `pdo` extension
- For a source build only: DuckDB 1.5.3 or newer (`libduckdb` + `duckdb.h`),
  available as a prebuilt bundle from the [DuckDB installation page](https://duckdb.org/docs/installation/)
  or via your package manager. Prebuilt installs (below) need nothing else.
  Source builds against the DuckDB 2.0 preview also work; see [DuckDB 2.0](#duckdb-20).

## 🚀 Installation

### PIE

```sh
pie install iliaal/pdo_duckdb
```

On Linux (x86_64/arm64), macOS (Apple Silicon), and Windows x64, PIE downloads a
self-contained prebuilt binary. No DuckDB install or build toolchain needed. On
Linux the prebuilt baseline is glibc 2.36 (Debian 12); Apple Silicon binaries
target macOS 11.0. On other platforms, older operating systems, or older PHP,
PIE falls back to a source build, which needs `libduckdb` + `duckdb.h`. If
they aren't in a standard location, point it at the prefix:

```sh
pie install iliaal/pdo_duckdb --with-pdo-duckdb=/opt/duckdb
```

### From source

```sh
phpize
./configure --with-pdo-duckdb=/opt/duckdb
make
make install
```

Then enable it in `php.ini` (after `pdo`):

```ini
extension=pdo_duckdb
```

To build against the DuckDB 2.0 preview, see [DuckDB 2.0](#duckdb-20).

### Other DuckDB PDO drivers

[thomas-0816/pdo-duckdb-php](https://github.com/thomas-0816/pdo-duckdb-php) is
a separate driver that uses the same extension name (`pdo_duckdb`) and PDO
driver name (`duckdb`). Install only one of them. Both drivers define
`PDO::DUCKDB_ATTR_CONFIG` and `PDO::DUCKDB_ATTR_UNBUFFERED`, with the values
swapped: this driver uses 1000 and 1001, and the other driver uses 1001 and
1000. If you switch drivers, refer to these attributes by constant name, never
by integer value.

## DSN

```
duckdb:/path/to/database.duckdb   # file-backed database
duckdb::memory:                   # in-memory database
duckdb:                           # in-memory database (empty path)
```
Only the exact path `:memory:` (or an empty path) opens an in-memory database.
Anything else is a file path, including `:memory:foo`, which creates a file
named `:memory:foo`. For a named in-memory database, attach one in SQL:
`ATTACH ':memory:' AS name`.

### Connection options

Append DuckDB configuration as `;key=value` pairs on the DSN, or pass them as a
`PDO::DUCKDB_ATTR_CONFIG` array:

```php
// open a database read-only, with a memory cap
$db = new PDO('duckdb:/data/analytics.duckdb;access_mode=read_only;memory_limit=2GB');

// equivalent, via the options array
$db = new PDO('duckdb::memory:', null, null, [
    PDO::DUCKDB_ATTR_CONFIG => ['threads' => 4, 'memory_limit' => '2GB'],
]);
```

Any DuckDB setting name works (`access_mode`, `memory_limit`, `threads`, ...);
an unknown option fails the connection. `force_mbedtls_unsafe` is rejected at
connect time because libduckdb 1.5.3-1.5.6 crashes on a falsy value before the
database exists; if you need it, `SET` it after open.
`PDO::DUCKDB_ATTR_CONFIG` is connect-time only and is refused with persistent
handles, because PDO's persistent key doesn't include driver option arrays.

Persistent connections reuse the same DuckDB connection for a matching DSN.
DuckDB session and catalog state (temporary tables, `SET` options,
attachments, `:memory:` contents) can survive across requests in the same PHP
process, so don't use persistence as a tenant or request isolation boundary. On
reuse the driver resets `http_proxy` (and proxy username/password) and turns
profiling off, so those don't carry into the next request.

When `open_basedir` is set, external file access stays disabled whatever you
pass. The driver also rejects path/security-sensitive DuckDB settings such as
`allowed_directories`, `allowed_paths`, `allowed_configs`, `temp_directory`,
`extension_directory`, and extension auto-install/load knobs, and locks DuckDB
configuration after applying the sandbox profile. DuckDB has no
descriptor-based open API, so the database-file path check and DuckDB's open
are separate filesystem operations. For file-backed databases, keep every
writable path component below trusted, non-writable directory ancestry so an
attacker can't swap in a file or symlink between the check and the open.

## 🛠️ Bulk insert (Appender)

For fast bulk loads, `PDO::duckdbAppender()` returns a `Pdo\Duckdb\Appender`
wrapping DuckDB's native appender, far faster than row-by-row `INSERT`:

```php
$db->exec('CREATE TABLE events (id INTEGER, name VARCHAR, ts TIMESTAMP)');

$app = $db->duckdbAppender('events');      // optional 2nd arg: schema name
foreach ($rows as $r) {
    $app->appendRow($r['id'], $r['name'], $r['ts']);
}
$app->flush();   // push buffered rows; appender stays open for more rows
$app->close();   // flush + finalize; further append/flush/close throw
```

`flush()` commits buffered rows and leaves the appender usable. `close()`
flushes, finalizes the native appender, and marks the PHP object closed.
Prefer an explicit `close()`. If you don't close an appender, PHP closes it when
the appender or its connection is destroyed:

- The buffered rows are flushed when you unset the appender, during garbage
  collection, and at the end of the request, including after a fatal error. A
  failed flush raises a warning. In autocommit mode the rows are committed; in
  an open transaction they join it.
- If a transaction is still open when the PDO object is destroyed, PDO rolls it
  back and the appender's buffered rows are discarded with it, whichever of the
  two objects PHP frees first. On PHP versions where several PDO objects share
  one persistent handle and PDO rolls the handle back when the first of them
  is freed, an appender that belongs to another of those PDO objects keeps its
  rows and flushes them in autocommit, as after an explicit `rollBack()`.
- An explicit `rollBack()` doesn't discard the buffer. Rows you appended but
  didn't flush stay buffered and are written by the next flush, in autocommit
  or in whatever transaction is open then. Call `flush()` before you commit or
  roll back so the rows land in the transaction you intend.
- DuckDB flushes the appender on its own every 204,800 rows (DuckDB 1.5).
  Those rows join the transaction that is open at that moment, so a later
  discard of the buffer doesn't remove them.
- Rows that a failed append, flush, or close leaves in the buffer are
  discarded. They are never flushed later.

Soft validation failures (`ValueError`/`TypeError` for arity, types, ranges)
leave the appender live so you can retry the row. Scalar cast validation also
runs before any native append touches the row, using range checks or a prepared
`CAST` probe where the target type supports one. This covers invalid strings
(e.g. `'not-a-date'` into `DATE`), uncastable booleans and numbers, and numeric
overflow. It doesn't check `VARCHAR` strings for valid UTF-8 (see below).
`BLOB` columns accept only strings or `null`; other scalars throw `TypeError`.
Cast-probe failures and finite overflow in a top-level `FLOAT` column throw
`PDOException("Failed to append value: …")`, preserving the appender and its
buffered rows. Hard DuckDB failures on append/flush/close (including casts
without an available preflight probe) poison the appender:
later use throws `Error` and you must create a new one. Rows already flushed
survive, but buffered rows are lost, so call `flush()` per unit of work to
bound the loss.

The appender doesn't check strings for valid UTF-8 before a top-level
`VARCHAR` append. If you pass invalid UTF-8 for a `VARCHAR` column,
`appendRow()` accepts it, and the next `flush()` or `close()` throws
`PDOException` and poisons the appender, losing every buffered row, including
valid rows appended before and after the bad one. Invalid UTF-8 inside a `LIST`,
`ARRAY`, `STRUCT`, or `MAP` value throws `TypeError` (`argument #N contains a
string that is not valid UTF-8`) and leaves the appender live. Invalid UTF-8
for a `UNION` member throws `PDOException` from `appendRow()` and poisons the
appender. If your input can contain arbitrary bytes, validate it first, for
example with `mb_check_encoding($s, 'UTF-8')`, or store it in a `BLOB` column.

`appendRow(...$values)` takes one argument per column (left to right) and
returns the appender for chaining. PHP `null`/`bool`/`int`/`float`/`string` map
to DuckDB values; DuckDB casts them to the target column types. For nested
columns, pass a PHP array: a list fills `LIST`/`ARRAY`, and an associative array
fills `STRUCT` (by field name) or `MAP`. Nesting deeper than 128 levels is
rejected.

```php
$db->exec('CREATE TABLE t (tags VARCHAR[], attrs STRUCT(x INTEGER, y VARCHAR))');
$app = $db->duckdbAppender('t');
$app->appendRow(['php', 'duckdb'], ['x' => 1, 'y' => 'hi']);
$app->flush();
```

Pass a column list as the third argument to append only some columns; the rest
take their `DEFAULT` (or `NULL`). This helps with tables that generate keys or
timestamps:

```php
$db->exec("CREATE SEQUENCE seq");
$db->exec("CREATE TABLE events (id BIGINT DEFAULT nextval('seq'), ts TIMESTAMP DEFAULT now(), payload VARCHAR)");
$app = $db->duckdbAppender('events', null, ['payload']);
$app->appendRow('hello')->appendRow('world');   // id and ts fill themselves
$app->flush();
```

On PHP 8.4+, `PDO::connect('duckdb:…')` returns a `Pdo\Duckdb` instance and
`duckdbAppender()` lives on that subclass. On `new PDO('duckdb:…')` (and on PHP
8.1-8.3) the method is available on the PDO object directly. PHP 8.5 emits a
deprecation for driver methods called on the base `PDO` class, so on 8.4+
prefer `PDO::connect()`.

## 🔍 Query helpers

Two driver-specific methods, available on the same object as `duckdbAppender()`:

```php
// Tables a query references, resolved by DuckDB's parser (UPDATE, DELETE, and
// MERGE return []). Pass true to include a non-default schema.
$db->duckdbTableNames('SELECT * FROM users u JOIN s.orders o ON u.id = o.id');
// ['orders', 'users']
$db->duckdbTableNames('SELECT * FROM s.orders', true);   // ['s.orders']

// Profiling tree of the last executed query. Enable profiling first; the method
// reads the recorded profile and runs nothing itself. Returns null until then.
$db->exec("PRAGMA enable_profiling='no_output'");
$db->query('SELECT count(*) FROM events WHERE ts > now() - INTERVAL 1 DAY');
$profile = $db->duckdbLastProfile();
// ['metrics' => ['QUERY_NAME' => '…', 'LATENCY' => '0.004', …],
//  'children' => [ ['metrics' => ['OPERATOR_NAME' => 'SEQ_SCAN', …], 'children' => […]] ]]
```

Profiling metric values are strings, or PHP `null` when DuckDB reports a SQL NULL
for that metric; cast the numeric strings as needed. DuckDB defines the metric
names and the tree layout, and DuckDB 2.0 reshapes both (see
[DuckDB 2.0](#duckdb-20)).

## 🧩 DuckDB extensions

DuckDB extensions load through ordinary SQL, no special API:

```php
$db->exec('LOAD json');                     // bundled extensions load offline
$db->exec('INSTALL httpfs; LOAD httpfs;');  // downloadable extensions
```

## Usage notes

### Placeholders

Positional `?` and named `:name` placeholders both work; PDO rewrites them to
DuckDB `$N` parameters. A repeated `:name` is bound once. Because `:` is
reserved for placeholders, inline `STRUCT`/`MAP` literals in prepared queries
must keep a space after the colon (`{'k': 1}`, not `{'k':1}`).

### Parameter binding

The driver re-reads each bound value on every `execute()`. A value bound by
reference with `bindParam()` is converted from the variable's current contents
each time, so assigning to the variable between executes takes effect without
re-binding. A `PDO::PARAM_LOB` stream is read to the end and then rewound, so
re-executing the same statement binds the same bytes rather than an empty
value. Streams larger than 64MB are rejected instead of buffered whole.
Non-seekable streams can't be rewound, so re-executing binds the remainder.

Don't call `execute()` on a statement from inside a `__toString()` that the
same statement is binding: PDO core caches its bound-parameter table across the
conversion and crashes. The bug is in PDO itself and affects every driver.

### Cursors

Cursors are forward-only. DuckDB returns results one chunk at a time in a
single direction, so `PDO::ATTR_CURSOR => PDO::CURSOR_SCROLL` is rejected at
`prepare()` rather than failing on the first backwards fetch. If you need
random access, use `fetchAll()` and index the array.

### Transactions

`beginTransaction()` / `commit()` / `rollBack()` map to DuckDB
`BEGIN TRANSACTION` / `COMMIT` / `ROLLBACK`. DuckDB is autocommit-by-default
with no session toggle, so `setAttribute(PDO::ATTR_AUTOCOMMIT, false)` is
rejected; use `beginTransaction()` for explicit transactions.

The driver also reflects raw transaction-control SQL through `inTransaction()`,
so a persistent handle can't keep an invisible transaction after PHP releases
the PDO object. This covers DuckDB's `END` and `ABORT` aliases and transaction
control wrapped by `EXPLAIN ANALYZE`. DuckDB reports only the outer `EXPLAIN`
statement type, so the driver tracks the wrapped effect itself after the
statement succeeds.

### Multi-statement `exec()`

`exec()` returns the last statement's row count:
`exec("BEGIN; INSERT ...; COMMIT")` reports `COMMIT`'s count (`0`), not the
INSERT's. If you need the intermediate `rowCount()`, split the statements.

### `open_basedir`

When `open_basedir` is set, DuckDB's SQL-level external file access
(`read_csv`, `COPY`, `ATTACH`, `httpfs`, …) is disabled, so the sandbox covers
SQL as well as the database file path. If `open_basedir` is tightened after a
handle exists, the driver clears DuckDB path allowlists before disabling
external access and locks the connection configuration. You can still load an
extension compiled into DuckDB, such as `LOAD json`; the sandbox blocks
extension files and downloads.

Tightening `open_basedir` on an open handle also switches DuckDB logging to
in-memory storage (`logging_storage='memory'`), on every DuckDB version. If the
connection was logging to a file (`CALL enable_logging(storage='file', ...)`),
that file gets no new lines from then on, and neither the driver nor DuckDB
reports the switch. `CALL enable_logging()` doesn't restore the file sink. Read
later entries from `duckdb_logs`, or open the connection with the final
`open_basedir` already in place if you need a file log outside the sandbox.

Disabling external access doesn't stop an extension that opens files, sockets,
or listeners itself. So when the driver sandboxes a connection after it opens
(you tighten `open_basedir` mid-request, or a persistent connection is reused),
it first checks the loaded extensions. If any extension outside `autocomplete`,
`core_functions`, `excel`, `fts`, `httpfs`, `icu`, `inet`, `json`, `parquet`,
`spatial`, `tpcds`, `tpch`, and `vss` is loaded (for example `quack`,
`sqlite_scanner`, or `postgres_scanner`), every later SQL call on that
connection fails with driver code 4, and the message names the extensions. The
driver also stops the connection's `quack` listeners. A reused persistent
connection is replaced by a new connection that's sandboxed when it opens. On
PHP releases before 8.4.26 and 8.5.11 (including every 8.1–8.3 release), PDO
core leaks the discarded persistent handle's bookkeeping (191 to 223 bytes in
local leak-checked runs) on each replacement; the driver doesn't control that
path. If you need one of those extensions, load it only on connections that
never run under a tightened `open_basedir`.

Locking the configuration blocks *every* later `SET`, including settings with
no security impact (`threads`, `memory_limit`, `preserve_insertion_order`,
`default_null_order`, …). Pass those at connect time instead, in the DSN tail
or `PDO::DUCKDB_ATTR_CONFIG`, where the sandbox allows anything that isn't
path- or extension-related. `TimeZone` is the exception: it's a SQL-only
setting with no `duckdb_set_config` equivalent, so under `open_basedir` it
stays at DuckDB's default for the life of the handle.

### `lastInsertId()`

`lastInsertId()` isn't supported because DuckDB has no implicit rowid. For
generated keys, use a sequence and `currval()`.

### Type mapping

| DuckDB type | PHP value | DuckDB 2.0+ build |
|-------------|-----------|-------------------|
| `BOOLEAN` | `int` `0`/`1` (not `bool`) | Same |
| `TINYINT`, `SMALLINT`, `INTEGER`, `BIGINT`, `UTINYINT`, `USMALLINT`, `UINTEGER` | `int` (a string for `BIGINT`/`UINTEGER` values outside the PHP integer range on 32-bit PHP) | Same |
| `FLOAT`, `DOUBLE` | `float` | Same |
| `BLOB` | binary string | Same |
| everything else (`VARCHAR`, `DATE`/`TIME`/`TIMESTAMP`, `DECIMAL`, `HUGEINT`/`UBIGINT`/`UHUGEINT`, nested types) | canonical string form (exceptions for `VARIANT`, `GEOMETRY`, and `TIMESTAMPTZ` below) | Same, including the zero-field `STRUCT` (`{}`) that only DuckDB 2.0 can build |
| `TIMESTAMPTZ_NS` | DuckDB 1.5 has no such type. If the build loads a DuckDB 2.0 `libduckdb`, every value fetches as `null` (see the warning below) | canonical string form of the UTC instant, like `TIMESTAMPTZ` |
| A type newer than the DuckDB headers the driver was built with | `null`, even for non-NULL values (see the warning below) | `null` when NULL; any other value reports a PDO error, like `VARIANT` |
| SQL NULL | `null` | Same |

**Warning:** Build the driver against the headers of the `libduckdb` it loads.
The library's file name carries no version, so a build against the DuckDB 1.5
headers can load a DuckDB 2.0 library. That build doesn't know the types
DuckDB 2.0 adds, and it loses their data without an error: a `TIMESTAMPTZ_NS`
value fetches as `null`, `[ts]` fetches as `[NULL]`, and `getColumnMeta()`
reports `UNKNOWN`.

`getColumnMeta()` reports the real DuckDB type name per column and a `pdo_type`
that matches the fetch shape: `PDO::PARAM_INT` for exactly `BOOLEAN`,
`TINYINT`, `SMALLINT`, `INTEGER`, `BIGINT`, `UTINYINT`, `USMALLINT`,
`UINTEGER`; `PDO::PARAM_LOB` for `BLOB`; `PDO::PARAM_STR` for everything else
(so `UBIGINT` and `HUGEINT` stay `PARAM_STR`). `DECIMAL` columns also report
`precision`/`scale`. On a DuckDB 2.0 build, `TIMESTAMPTZ_NS` columns report
`TIMESTAMP_TZ_NS`, and a column of DuckDB's `NULL` type reports `NULL` with
`PDO::PARAM_NULL`. DuckDB 2.0 gives a bare `SELECT NULL` that type; DuckDB 1.5
types it `INTEGER`.

Nested values with boolean, integer, `FLOAT`/`DOUBLE`, `DECIMAL`, `DATE`,
`TIME`, `TIMESTAMP`, `UUID`, and `VARCHAR` leaves use a direct renderer that
applies DuckDB's quoting rules, so
`[TIMESTAMP '2020-01-01 10:00:00']` returns `['2020-01-01 10:00:00']`, exactly
like `CAST(... AS VARCHAR)`. Other nested values use DuckDB's own renderer.
Nested fetches return canonical strings, not PHP arrays; for a different
PHP-facing shape, use SQL projections such as `unnest`, `struct_extract`, or
`json`.

A NUL byte inside a nested `VARCHAR` member truncates the fetched string,
because the driver matches DuckDB's value renderer, which returns a
NUL-terminated C string. With the direct renderer, the member is cut at the NUL
and loses its closing quote, but later members still render:
`['it''s' || chr(0), 'x', 'admin']` returns `['it\'s, x, admin]`. With DuckDB's
renderer, everything after the NUL is dropped, closing brackets included. In
both cases, you can't reliably split the result into elements. Top-level
`VARCHAR` and `BLOB` columns aren't affected. If nested strings can contain NUL
bytes, unnest them in SQL or render them yourself, for example with
`to_json()` or `hex()`.

`GEOMETRY` returns its WKB bytes as an uppercase hex string. Nested `GEOMETRY` elements inside `LIST`/`ARRAY`/`STRUCT`/`MAP`/
`UNION` render the same uppercase hex per element, because the C API has no
geometry value constructor and the driver declares those containers with
`VARCHAR` in place of `GEOMETRY`. For WKT, select `CAST(col AS VARCHAR)`, or
call `ST_AsText()` from the spatial extension.

`TIMESTAMPTZ` native fetches render the instant in UTC (`+00`), both at the top
level and as a nested leaf. For DuckDB's session-`TimeZone` rendering, select
`CAST(col AS VARCHAR)`. `TIMESTAMPTZ_NS` on DuckDB 2.0 follows the same rule,
but don't cast `TIMESTAMPTZ_NS` columns to `VARCHAR` on the DuckDB 2.0 preview:
a DuckDB defect in that cast can invalidate the whole database. See
[Types](#types) in the DuckDB 2.0 section.

DuckDB's C result API can't extract non-NULL `VARIANT` cells safely, so
fetching a row that holds one reports a PDO error; cast it to `VARCHAR` (or
another concrete SQL type) in the query.

### Streaming results

By default `execute()` returns a materialized result: DuckDB buffers the full
result set before PDO fetches, so available memory bounds a large `SELECT`.
For large scans, set `PDO::DUCKDB_ATTR_UNBUFFERED` to fetch chunks lazily
through DuckDB's pending-result API:

```php
$db->setAttribute(PDO::DUCKDB_ATTR_UNBUFFERED, true);
```

An unbuffered result needs the connection to itself until you finish reading
it. Preparing or running another statement on the same connection, including
`prepare()`, `query()`, and `exec()`, makes DuckDB abort the partially read
result. Rows from the chunk already fetched (up to 2048) stay readable, and the
fetch after them throws a `PDOException` (driver code 5, `Attempting to execute
an unsuccessful or closed ... result`), even when the result had no more rows.
This applies to DuckDB 1.5 and 2.0. If you need to interleave queries, read the
stream to the end first, use a buffered result, or open a second connection.
After an abort, call `PDOStatement::closeCursor()` and `execute()` again to
rerun the query. To release the native result early, call `closeCursor()`.

Under `PDO::ERRMODE_SILENT` or `PDO::ERRMODE_WARNING`, the aborted fetch returns
`false`, so a fetch loop ends as if the result were complete. Check
`errorCode()` right after the loop: it's `HY000` after an abort, and
`errorInfo()[1]` is 5. Another fetch resets `errorCode()` to `00000`.

Persistent connections reset `DUCKDB_ATTR_UNBUFFERED` to false on every
checkout (`check_liveness`). Pass it again in the constructor options, or call
`setAttribute` after each `new PDO(..., [PDO::ATTR_PERSISTENT => true])`.

## DuckDB 2.0

DuckDB 2.0 is in preview. The prebuilt binaries use DuckDB 1.5.x, and a build
against DuckDB 1.5.x behaves as before. You can build the driver against the
2.0 preview C library to test your application. This section describes
`v2.0.0-alpha46057`; later previews can change what it covers.

### Build against the preview

Download the `duckdb-shared-libs-<platform>.tar.gz` bundle from the
[DuckDB preview install page](https://duckdb.org/install/preview.html). Put
every header from the bundle in `include/`, put the shared library in `lib/`,
and point `configure` at that prefix:

```sh
./configure --with-pdo-duckdb=/opt/duckdb-2.0
```

DuckDB 2.0 still reports C API version 1.5.6 in `duckdb.h`, so `configure`
detects 2.0 from `duckdb_v2.h` in the same directory as `duckdb.h`:

- If `duckdb_v2.h` is missing, `configure` prints a warning and builds without
  the DuckDB 2.0 code paths. To fix it, copy the full header set.
- If the headers are 2.0 but `libduckdb` is older, `configure` stops with an
  error.

A 2.0 build reports `DuckDB 2.0 headers => yes` in `phpinfo()`.

### Storage version

By default DuckDB 2.0 writes database files that DuckDB 1.5 can't open:

```text
IO Error: Trying to read a database file with version number 69, but we can only read versions between 64 and 68.
```

If DuckDB 1.5 must also read a file, set `storage_compatibility_version` when
you open it, in the DSN or in `PDO::DUCKDB_ATTR_CONFIG`:

```php
$db = new PDO('duckdb:/data/shared.duckdb;storage_compatibility_version=v1.5.6');

$db = new PDO('duckdb:/data/shared.duckdb', null, null, [
    PDO::DUCKDB_ATTR_CONFIG => ['storage_compatibility_version' => 'v1.5.6'],
]);
```

To write one attached database in the older format, pass `STORAGE_VERSION` to
`ATTACH`:

```php
$db->exec("ATTACH '/data/export.duckdb' AS export (STORAGE_VERSION 'v1.5.6')");
```

To check the format of an open database, read its `storage_version` tag. It
reports `v2.0.0+` for the 2.0 format. DuckDB 1.5 can open a file with an older
tag, such as `v1.5.0+`:

```sql
SELECT database_name, tags['storage_version'] FROM duckdb_databases();
```

### SQL behavior changes

These changes come from DuckDB itself. Each one has a setting that restores
the 1.5 behavior, which you can pass in the DSN tail or
`PDO::DUCKDB_ATTR_CONFIG`. Under `open_basedir` the driver locks the
configuration, so set them when you connect. DuckDB 1.5 refuses the connection
for `default_transaction_invalidation_policy`, `error_on_division_by_zero`, and
`enable_legacy_parser` (`The following options were not recognized`), so pass
those only to a 2.0 build. DuckDB 1.5 accepts `lambda_syntax`.

- **Errors abort the transaction.** Inside `beginTransaction()`, any failed
  statement, including a parser or catalog error, aborts the transaction.
  Every later statement fails with `TransactionContext Error: Current
  transaction is aborted (please ROLLBACK)`. DuckDB 1.5 keeps the transaction
  open after parser and catalog errors, and aborts it on execution errors
  such as a constraint violation. In both versions,
  `commit()` on an aborted transaction returns `true` and keeps nothing, so
  call `rollBack()` after a failed statement. To keep parser and catalog errors
  from aborting, set
  `default_transaction_invalidation_policy=SYNTACTIC_ERRORS_DO_NOT_INVALIDATE`.
- **Integer division by zero throws.** `1 // 0` and `1 % 0` raise `Invalid
  Input Error: Division by zero` instead of returning `NULL`. `1 / 0` still
  returns `INF`. To return `NULL`, wrap the expression in `TRY(...)` or set
  `error_on_division_by_zero=false`.
- **Single-arrow lambdas are rejected.** `list_transform(l, x -> x + 1)` raises
  `Binder Error: Deprecated lambda arrow (->) detected`. Write
  `lambda x: x + 1`, which also works on 1.5, or set
  `lambda_syntax=ENABLE_SINGLE_ARROW`. DuckDB plans to remove that setting.
- **New default parser.** Parser error messages point at a different token, for
  example `syntax error at or near "1"` instead of `"SELEC"` for `SELEC 1`. If
  your code matches parser error text, set `enable_legacy_parser=true`. That
  setting loads the downloadable `legacy_parser` extension, so it isn't
  available under `open_basedir`.

### New SQL features

- **Triggers.** `CREATE TRIGGER` runs through `exec()`. `rowCount()` and the
  `exec()` return value count only the rows of the statement you ran, not the
  rows its triggers write. A `FOR EACH ROW` trigger body must reference `NEW` or
  `OLD`.
- **VARIANT and JSON.** `variant_*` and `json_*` functions work in plain SQL,
  for example `variant_extract(v, 'a')::INTEGER`. Fetching a non-NULL `VARIANT`
  value still reports a PDO error, as on 1.5; cast it to a concrete type in
  the query.

### Types

A build against the DuckDB 2.0 headers handles the types that DuckDB 2.0 adds
or changes. The [type mapping](#type-mapping) table lists the PHP values.

- **`TIMESTAMPTZ_NS`.** Fetches return the UTC instant with up to nine
  fractional digits, for example `2020-01-01 00:00:00.123456789+00`, at the top
  level and inside `LIST`, `ARRAY`, `STRUCT`, `MAP`, and `UNION` values. The
  session `TimeZone` doesn't change the result. The appender casts strings into
  `TIMESTAMPTZ_NS` columns. It checks each scalar before appending, so a string
  that DuckDB can't parse, or an integer or float, throws a `PDOException` and
  leaves the appender usable. A bad value inside a nested column throws a
  `ValueError`.
- **`NULL` type.** A bare `SELECT NULL`, or a parameter bound to `null` in a
  select list, produces a column of DuckDB's `NULL` type. It fetches as `null`
  and reports `native_type` `NULL` with `PDO::PARAM_NULL`. On DuckDB 1.5 the
  same column is an `INTEGER`.
- **`GEOMETRY`.** Unchanged: fetches return uppercase hex WKB. DuckDB 2.0 adds
  a core `BLOB` to `GEOMETRY` cast, so `CAST(from_hex(?) AS GEOMETRY)` turns a
  fetched value back into a geometry without the spatial extension.
- **`VARIANT`.** Unchanged: fetching a non-NULL value reports a PDO error. The
  appender stores PHP scalars into a `VARIANT` column as `VARCHAR`, `INT64`,
  `DOUBLE`, or boolean values.
- **Empty `STRUCT`.** DuckDB 2.0 can build a `STRUCT` with no fields (`{}`).
  It fetches as `{}`, and the appender accepts an empty PHP array for it.
- **Newer types.** If DuckDB returns a type that the driver's headers don't
  define, a non-NULL value reports `DuckDB type id N is not supported by this
  pdo_duckdb build; cast the value to VARCHAR in SQL`, and a NULL value fetches
  as `null`. The driver can't turn such a value into a string: DuckDB's C API
  reads a cell only through the memory layout of its type, and the driver has
  no layout for a type its headers don't define. Reporting an error is safer
  than reading memory it can't interpret or returning a wrong `null`.

**Warning:** DuckDB's own `CAST(x AS VARCHAR)` mishandles a `TIMESTAMPTZ_NS`
instant before 1970 that falls less than one microsecond before a whole
second. In a session with `SET TimeZone='UTC'`, it renders
`make_timestamp_ns(-1)` as `1970-01-01 00:00:00.00000000/+00` instead of
`1969-12-31 23:59:59.999999999+00`, and for other values, such as
`make_timestamp_ns(-500)`, it raises `INTERNAL Error: Information loss on
integer cast`. If DuckDB raises that error while it runs the query, for
example on a table column, it invalidates the whole database: every later
statement, even `SELECT 1`, fails with `FATAL Error: Failed: database has been
invalidated because of a previous fatal error` until you open the database
again. The database stays usable only when DuckDB evaluates the cast before it
runs the query, which happens only when the cast's input is built from
constants and bound parameters alone; any column reference in the input
puts the database at risk.
The driver's own fetch of these values is correct, so fetch the column instead
of casting it.

### `CONNECT`

While a connection is redirected with DuckDB 2.0's `CONNECT` statement,
`prepare()` and `query()` fail with `INTERNAL Error: PREPARE succeeded but the
prepared statement was not registered`, and DuckDB has already run the
statement on the remote database by then. A failed `prepare()` of an `INSERT`
still inserts the row. `exec()` works. This is a DuckDB 2.0 preview defect in
`duckdb_prepare()`, so the driver can't avoid it. Until DuckDB fixes it, use
`exec()` for statements while connected, or `ATTACH` the remote database and
query it with qualified names instead of `CONNECT`. `DISCONNECT` restores
normal behavior.

The `ATTACH` workaround has its own limits on a quack remote. `prepare()` of a
statement with a placeholder fails with `Invalid Input Error: Values were not
provided for the following parameters: 1`, so statements on the remote can't
use bound parameters. With both `CONNECT` and `ATTACH`, `exec()` returns `0`
and `rowCount()` reports `0` for an `INSERT`, `UPDATE`, or `DELETE` on the
remote, however many rows it changed, because DuckDB reports no changed rows
for it. Count the rows in SQL if you need the number.

### Driver differences

- **Profiling.** `duckdbLastProfile()` keeps its `['metrics' => …, 'children' =>
  …]` shape, but DuckDB 2.0 fills it differently. The root `metrics` array is
  empty, and the root has one child per category. The operator tree child has
  lower-case keys (`type`, `timing`, `extra_info`), and the query text moves to
  the `sql` key of another child. Find metrics by key instead of by position.
- **Unbuffered errors.** With `PDO::DUCKDB_ATTR_UNBUFFERED`, DuckDB 2.0
  reports some execution errors, such as a failed cast of a bound value, from
  the first fetch instead of from `execute()`. Wrap the fetch loop, not only
  `execute()`, in your error handling.

## Errors and exceptions

Driver errors report a numeric driver code (`errorInfo()[1]`) alongside the
SQLSTATE (`errorInfo()[0]`):

| driver code | SQLSTATE | meaning                                                                |
|-------------|----------|------------------------------------------------------------------------|
| 1           | `HY000`  | general errors (the default)                                           |
| 2           | `08000`  | connection and open failures                                           |
| 3           | `42000`  | SQL syntax and prepare failures                                        |
| 4           | `HY000`  | `open_basedir` sandbox denials (the denial message text is unchanged)  |
| 5           | `HY000`  | streaming / fetch errors                                               |

Code 4 covers the driver's own sandbox refusals (fail-closed re-narrowing,
rejected sandbox-bypass options, and connections refused for a loaded
extension). When DuckDB itself refuses the work because the sandbox disabled
external access (e.g. `read_csv()` failing at prepare with an access error), the failure carries the code of the phase that reported
it (3 for prepare failures) and the engine's message intact.

Whether a failure throws depends on the entry point and `PDO::ATTR_ERRMODE`:

| entry point | NUL byte in input | failure mode |
|-------------|-------------------|--------------|
| `query()` / `prepare()` / `exec()` | rejected (`SQL statement contains a NUL byte`) | ERRMODE-gated: `PDOException` under `ERRMODE_EXCEPTION`, otherwise warning/`false` |
| `quote()` | rejected (`DuckDB PDO::quote does not support null bytes`) | ERRMODE-gated, same as above |
| `PDO::DUCKDB_ATTR_CONFIG` keys/values | rejected (`… must not contain a NUL byte`) | always `PDOException`, raised while opening the connection |
| `duckdbTableNames()` query; `duckdbAppender()` table, schema, and column names | rejected | always `ValueError`, regardless of ERRMODE |

Other contracts:

- **Closed or poisoned appender.** `appendRow()`, `flush()`, and `close()` on a
  closed or poisoned appender throw `Error("Pdo\Duckdb\Appender is closed")`.
  `close()` is not idempotent: closing an already-closed appender throws the
  same `Error`. A failed native append/flush/close poisons the appender (later
  use throws `Error`); rows already flushed survive, but unflushed buffered
  rows are lost. Probe rejections (`Failed to append value: …`) and soft
  validation failures don't poison.
- **`VARIANT`.** A row holding a non-NULL `VARIANT` value, at any nesting
  depth, fails the fetch before any of its columns are read: under
  `ERRMODE_EXCEPTION` the fetch throws, otherwise it returns `false` with the
  error in `errorInfo()`, and the next fetch continues with the following row.
  SQL NULL `VARIANT` cells read back as `null`. Before PHP 8.5, `fetchAll()`
  reports a failure only when it happens on its first row, so under
  `ERRMODE_WARNING` or `ERRMODE_SILENT` a later `VARIANT` row ends the result
  early with no warning; check `errorInfo()`. Because the failed fetch returns
  `false`, a `while ($row = $stmt->fetch())` loop stops at that row as if the
  result had ended. Cast `VARIANT` columns to `VARCHAR` in SQL to fetch the
  value, or check `errorInfo()` after the loop.
- **`getAttribute()`.** Returns the library version for
  `ATTR_CLIENT_VERSION`/`ATTR_SERVER_VERSION`, `"duckdb"` for
  `ATTR_DRIVER_NAME`, and the streaming flag for
  `PDO::DUCKDB_ATTR_UNBUFFERED`. `PDO::DUCKDB_ATTR_CONFIG` and
  `PDO::ATTR_AUTOCOMMIT` are not gettable.
- **`duckdbTableNames()`.** An unparseable query throws `PDOException` with a
  detail-free message (`could not parse the query`); `prepare()` the query
  for the engine's specific error. `UPDATE`, `DELETE`, and `MERGE` yield `[]`
  (`MERGE` needs DuckDB 1.5.5 or newer and throws on older releases);
  `INSERT ... SELECT` lists the tables it reads. DDL behavior is unspecified.

## Status

Early release. Result columns are decoded with DuckDB's data-chunk/vector API:
native scalars go straight to PHP values, nested and extended types via their
canonical string form.

## 🔗 Native PHP extensions

Companion native PHP extensions:

- **[php_excel](https://github.com/iliaal/php_excel)**: native Excel I/O via LibXL. 7-10× faster than PhpSpreadsheet, full XLS/XLSX with formulas, formatting, and styling.
- **[mdparser](https://github.com/iliaal/mdparser)**: native CommonMark + GFM markdown parser via md4c. 15-30× faster than pure-PHP libraries.
- **[php_clickhouse](https://github.com/iliaal/php_clickhouse)**: native ClickHouse client speaking the wire protocol directly. Picks up where SeasClick left off.
- **[fastjson](https://github.com/iliaal/fastjson)**: drop-in faster `ext/json`, backed by yyjson. 6× encode, 2.7× decode, 5× validate.
- **[phpser](https://github.com/iliaal/phpser)**: decoder-optimized binary serializer for cache workloads. Faster than igbinary on packed numerics and DTO batches.
- **[fast_uuid](https://github.com/iliaal/fast_uuid)**: high-throughput UUID generation (v1/v4/v7), batched CSPRNG and SIMD hex formatter, ramsey-compatible API.
- **[fastchart](https://github.com/iliaal/fastchart)**: native chart-rendering extension. 38 chart types behind one fluent OO API, SVG-canonical with PNG/JPG/WebP and optional PDF output.
- **[statgrab](https://github.com/iliaal/statgrab)**: system statistics (CPU, memory, disk, network) via libstatgrab, no parsing /proc by hand.
- **[phonetic](https://github.com/iliaal/phonetic)**: native phonetic name matching (Double Metaphone, Beider-Morse, Daitch-Mokotoff, NYSIIS, Match Rating), the encoders PHP core lacks.

## License

BSD 3-Clause. See [LICENSE](LICENSE).

---

[Follow @iliaa on X](https://x.com/iliaa) • [Blog](https://ilia.ws) • If this got DuckDB into your PHP stack, ⭐ star it!
