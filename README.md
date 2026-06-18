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

## Usage notes

- **Placeholders.** Positional `?` placeholders are bound natively. Named
  placeholders (`:name`) are rewritten to positional by PDO.
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
