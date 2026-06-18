# pdo_duckdb

A [PDO](https://www.php.net/pdo) driver for [DuckDB](https://duckdb.org/), the
in-process analytical (OLAP) database. Connect to DuckDB through the standard
PDO API you already use for SQLite, MySQL and PostgreSQL.

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

- PHP 8.3 or newer with the `pdo` extension
- The DuckDB C library (`libduckdb` + `duckdb.h`). Download a prebuilt
  `libduckdb` bundle from the [DuckDB installation page](https://duckdb.org/docs/installation/)
  or install it via your package manager.

## Installation

### PIE

```sh
pie install iliaal/pdo_duckdb
```

If `duckdb.h` and `libduckdb` are not in a standard location, point the build at
the DuckDB install prefix:

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

## Bulk insert (Appender)

For fast bulk loads, `PDO::duckdbAppender()` returns a `Pdo\Duckdb\Appender`
wrapping DuckDB's native appender — far faster than row-by-row `INSERT`:

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
to DuckDB values; DuckDB casts them to the target column types.

On PHP 8.4+, `PDO::connect('duckdb:…')` returns a `Pdo\Duckdb` instance and
`duckdbAppender()` lives on that subclass. On `new PDO('duckdb:…')` (and on PHP
8.3) the method is available on the PDO object directly; note PHP 8.5 emits a
deprecation for driver methods called on the base `PDO` class, so prefer
`PDO::connect()` on 8.4+.

## DuckDB extensions

DuckDB extensions load through ordinary SQL — no special API:

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
  DuckDB `BEGIN TRANSACTION` / `COMMIT` / `ROLLBACK`.
- **`lastInsertId()`** is not supported — DuckDB has no implicit rowid. Use a
  sequence and `currval()` if you need generated keys.
- **Type mapping.** Integers up to 64-bit signed return as `int`, `FLOAT`/`DOUBLE`
  as `float`, `BLOB` as a binary string, and everything else (`VARCHAR`,
  `DATE`/`TIME`/`TIMESTAMP`, `DECIMAL`, `HUGEINT`/`UBIGINT`, nested types) as its
  canonical string form.

## Status

Early release. The result path currently uses DuckDB's legacy row-value C API;
migration to the data-chunk/vector API is planned for performance and to track
DuckDB's forward API.

## License

BSD 3-Clause. See [LICENSE](LICENSE).
