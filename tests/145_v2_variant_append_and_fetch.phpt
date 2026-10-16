--TEST--
pdo_duckdb: DuckDB 2.0 VARIANT columns accept appended scalars, report metadata, and still reject non-NULL fetches
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
require __DIR__ . '/duckdb_v2.inc';
if (!pdo_duckdb_v2_headers()) die('skip needs a build against the DuckDB 2.0 headers');
?>
--FILE--
<?php
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]) : new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('CREATE TABLE v (id INTEGER, v VARIANT)');
$ap = $db->duckdbAppender('v');
$ap->appendRow(1, 'abc');
$ap->appendRow(2, 42);
$ap->appendRow(3, 1.5);
$ap->appendRow(4, true);
$ap->appendRow(5, null);
try {
    $ap->appendRow(6, ['a' => 1]);
} catch (TypeError $e) {
    echo get_class($e), ': ', $e->getMessage(), "\n";
}
$ap->close();

foreach ($db->query('SELECT id, CAST(v AS VARCHAR), variant_typeof(v) FROM v ORDER BY id')->fetchAll(PDO::FETCH_NUM) as $row) {
    echo json_encode($row), "\n";
}

$st = $db->query('SELECT v FROM v WHERE id = 1');
$m = $st->getColumnMeta(0);
echo $m['native_type'], ' pdo_type=', $m['pdo_type'], "\n";
try {
    $st->fetch();
} catch (PDOException $e) {
    echo $e->getMessage(), "\n";
}
var_dump($db->query('SELECT v FROM v WHERE id = 5')->fetchColumn());
?>
--EXPECT--
TypeError: Pdo\Duckdb\Appender::appendRow(): argument #2 is an array but the target column is not a nested type
[1,"abc","VARCHAR"]
[2,"42","INT64"]
[3,"1.5","DOUBLE"]
[4,"true","BOOL_TRUE"]
[5,null,"VARIANT_NULL"]
VARIANT pdo_type=2
SQLSTATE[HY000]: General error: 5 DuckDB VARIANT values are not supported by the C result API; cast the value to VARCHAR in SQL
NULL
