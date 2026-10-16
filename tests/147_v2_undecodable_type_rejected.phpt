--TEST--
pdo_duckdb: DuckDB 2.0 builds reject non-NULL cells of a type id the driver cannot decode
--DESCRIPTION--
A type newer than the compile-time headers has an unknown vector layout. This
test runs only on a build configured with
CFLAGS=-DPDO_DUCKDB_TEST_UNDECODABLE_TYPE=13, which makes the driver treat
DATE (type id 13) as such a type.
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
require __DIR__ . '/duckdb_v2.inc';
if (!pdo_duckdb_v2_headers()) die('skip needs a build against the DuckDB 2.0 headers');
ob_start();
(new ReflectionExtension('pdo_duckdb'))->info();
if (!str_contains(ob_get_clean(), 'Test-only undecodable type id => 13')) {
    die('skip needs a build with -DPDO_DUCKDB_TEST_UNDECODABLE_TYPE=13');
}
?>
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
foreach ([
    "SELECT DATE '2020-01-01' AS d",
    "SELECT 1 AS i, [DATE '2020-01-01'] AS l",
    "SELECT {'d': DATE '2020-01-01'} AS s",
    // The error names the cell that blocked the row, not another column's type.
    "SELECT CAST(42 AS VARIANT) AS v, NULL::DATE AS d",
    "SELECT NULL::VARIANT AS v, DATE '2020-01-01' AS d",
    "SELECT {'v': CAST(42 AS VARIANT), 'd': NULL::DATE} AS s",
    "SELECT {'v': NULL::VARIANT, 'd': DATE '2020-01-01'} AS s",
] as $sql) {
    $st = $db->query($sql);
    echo $st->getColumnMeta($st->columnCount() - 1)['native_type'], ': ';
    try {
        var_dump($st->fetch(PDO::FETCH_NUM));
    } catch (PDOException $e) {
        echo $e->getMessage(), "\n";
    }
}
var_dump($db->query('SELECT NULL::DATE AS d, [NULL::DATE] AS l')->fetch(PDO::FETCH_NUM));
var_dump($db->query("SELECT CAST(DATE '2020-01-01' AS VARCHAR)")->fetchColumn());

$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);
$st = $db->query("SELECT i, CASE WHEN i = 1 THEN DATE '2020-01-01' END AS d FROM range(3) t(i)");
var_dump($st->fetch(PDO::FETCH_NUM), $st->fetch(PDO::FETCH_NUM), $st->errorInfo()[2]);
?>
--EXPECT--
DATE: SQLSTATE[HY000]: General error: 5 DuckDB type id 13 is not supported by this pdo_duckdb build; cast the value to VARCHAR in SQL
LIST: SQLSTATE[HY000]: General error: 5 DuckDB type id 13 is not supported by this pdo_duckdb build; cast the value to VARCHAR in SQL
STRUCT: SQLSTATE[HY000]: General error: 5 DuckDB type id 13 is not supported by this pdo_duckdb build; cast the value to VARCHAR in SQL
DATE: SQLSTATE[HY000]: General error: 5 DuckDB VARIANT values are not supported by the C result API; cast the value to VARCHAR in SQL
DATE: SQLSTATE[HY000]: General error: 5 DuckDB type id 13 is not supported by this pdo_duckdb build; cast the value to VARCHAR in SQL
STRUCT: SQLSTATE[HY000]: General error: 5 DuckDB VARIANT values are not supported by the C result API; cast the value to VARCHAR in SQL
STRUCT: SQLSTATE[HY000]: General error: 5 DuckDB type id 13 is not supported by this pdo_duckdb build; cast the value to VARCHAR in SQL
array(2) {
  [0]=>
  NULL
  [1]=>
  string(6) "[NULL]"
}
string(10) "2020-01-01"
array(2) {
  [0]=>
  int(0)
  [1]=>
  NULL
}
bool(false)
string(93) "DuckDB type id 13 is not supported by this pdo_duckdb build; cast the value to VARCHAR in SQL"
