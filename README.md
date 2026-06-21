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
- For a source build only: the DuckDB C library (`libduckdb` + `duckdb.h`),
  available as a prebuilt bundle from the [DuckDB installation page](https://duckdb.org/docs/installation/)
  or via your package manager. Prebuilt installs (below) need nothing else.

## 🚀 Installation

### PIE

```sh
pie install iliaal/pdo_duckdb
```

On Linux (x86_64/arm64), macOS (Apple Silicon), and Windows x64, PIE downloads a
self-contained prebuilt binary. No DuckDB install or build toolchain needed. On
other platforms or older PHP it falls back to a source build, which needs
`libduckdb` + `duckdb.h`; point it at the prefix if they aren't in a standard
location:

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

## DSN

```
duckdb:/path/to/database.duckdb   # file-backed database
duckdb::memory:                   # in-memory database
duckdb:                           # in-memory database (empty path)
```

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

Any DuckDB setting name works (`access_mode`, `memory_limit`, `threads`,
`temp_directory`, ...); an unknown option fails the connection. When
`open_basedir` is set, external file access stays disabled whatever you pass.

## 🛠️ Bulk insert (Appender)

For fast bulk loads, `PDO::duckdbAppender()` returns a `Pdo\Duckdb\Appender`
wrapping DuckDB's native appender, far faster than row-by-row `INSERT`:

```php
$db->exec('CREATE TABLE events (id INTEGER, name VARCHAR, ts TIMESTAMP)');

$app = $db->duckdbAppender('events');      // optional 2nd arg: schema name
foreach ($rows as $r) {
    $app->appendRow($r['id'], $r['name'], $r['ts']);
}
$app->flush();                              // or $app->close() to finalize
```

`appendRow(...$values)` takes one argument per column (left to right) and
returns the appender for chaining. PHP `null`/`bool`/`int`/`float`/`string` map
to DuckDB values; DuckDB casts them to the target column types. For nested
columns, pass a PHP array: a list fills `LIST`/`ARRAY`, and an associative array
fills `STRUCT` (by field name) or `MAP`.

```php
$db->exec('CREATE TABLE t (tags VARCHAR[], attrs STRUCT(x INTEGER, y VARCHAR))');
$app = $db->duckdbAppender('t');
$app->appendRow(['php', 'duckdb'], ['x' => 1, 'y' => 'hi']);
$app->flush();
```

Pass a column list as the third argument to append only some columns; the rest
take their `DEFAULT` (or `NULL`). Handy for tables with generated keys or
timestamps:

```php
$db->exec("CREATE TABLE events (id BIGINT DEFAULT nextval('seq'), ts TIMESTAMP DEFAULT now(), payload VARCHAR)");
$app = $db->duckdbAppender('events', null, ['payload']);
$app->appendRow('hello')->appendRow('world');   // id and ts fill themselves
$app->flush();
```

On PHP 8.4+, `PDO::connect('duckdb:…')` returns a `Pdo\Duckdb` instance and
`duckdbAppender()` lives on that subclass. On `new PDO('duckdb:…')` (and on PHP
8.1-8.3) the method is available on the PDO object directly; note PHP 8.5 emits a
deprecation for driver methods called on the base `PDO` class, so prefer
`PDO::connect()` on 8.4+.

## 🧩 DuckDB extensions

DuckDB extensions load through ordinary SQL, no special API:

```php
$db->exec('LOAD json');                     // bundled extensions load offline
$db->exec('INSTALL httpfs; LOAD httpfs;');  // downloadable extensions
```

## Usage notes

- **Placeholders.** Positional `?` and named `:name` placeholders are supported;
  PDO rewrites them to DuckDB `$N` parameters. A repeated `:name` is bound once.
  Because `:` is reserved for placeholders, inline `STRUCT`/`MAP` literals must
  keep a space after the colon (`{'k': 1}`, not `{'k':1}`) in prepared queries.
- **Transactions.** `beginTransaction()` / `commit()` / `rollBack()` map to
  DuckDB `BEGIN TRANSACTION` / `COMMIT` / `ROLLBACK`. DuckDB is
  autocommit-by-default with no session toggle, so `setAttribute(PDO::ATTR_AUTOCOMMIT,
  false)` is rejected; use `beginTransaction()` for explicit transactions.
- **`open_basedir`.** When `open_basedir` is set, DuckDB's SQL-level external
  file access (`read_csv`, `COPY`, `ATTACH`, `httpfs`, …) is disabled so the
  sandbox holds at the SQL layer, not just for the database file path.
- **`lastInsertId()`** is not supported; DuckDB has no implicit rowid. Use a
  sequence and `currval()` if you need generated keys.
- **Type mapping.** Integers up to 64-bit signed return as `int`, `FLOAT`/`DOUBLE`
  as `float`, `BLOB` as a binary string, and everything else (`VARCHAR`,
  `DATE`/`TIME`/`TIMESTAMP`, `DECIMAL`, `HUGEINT`/`UBIGINT`, nested types) as its
  canonical string form. `getColumnMeta()` reports the real DuckDB type name per
  column, plus `precision`/`scale` for `DECIMAL`.
- **Streaming results.** By default `execute()` returns a materialized result:
  DuckDB buffers the full result set before PDO fetches, so a large `SELECT` is
  bounded by available memory. For large scans, set `PDO::DUCKDB_ATTR_UNBUFFERED`
  to fetch chunks lazily through DuckDB's pending-result API instead:

  ```php
  $db->setAttribute(PDO::DUCKDB_ATTR_UNBUFFERED, true);
  ```

  DuckDB keeps one streaming result active per connection at a time, so consume a
  statement before running the next on the same handle.

## Status

Early release. Result columns are decoded with DuckDB's data-chunk/vector API:
native scalars go straight to PHP values, nested and extended types via their
canonical string form.

## 🔗 PHP Performance Toolkit

Companion native PHP extensions for high-throughput PHP workloads:

- **[php_clickhouse](https://github.com/iliaal/php_clickhouse)**: native ClickHouse client speaking the wire protocol directly. Picks up where SeasClick left off.
- **[fastchart](https://github.com/iliaal/fastchart)**: native chart-rendering extension. 26 chart types behind one fluent OO API, SVG-canonical with PNG/JPG/WebP output (no libgd dependency).
- **[php_excel](https://github.com/iliaal/php_excel)**: native Excel I/O. 7-10× faster than PhpSpreadsheet, full XLS/XLSX with formulas, conditional formatting, and rich text. Powered by LibXL.

## License

BSD 3-Clause. See [LICENSE](LICENSE).

---

[Follow @iliaa on X](https://x.com/iliaa) • [Blog](https://ilia.ws) • If this got DuckDB into your PHP stack, ⭐ star it!
