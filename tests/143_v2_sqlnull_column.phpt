--TEST--
pdo_duckdb: DuckDB 2.0 NULL-typed result columns fetch as null and report PDO::PARAM_NULL
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
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);

// DuckDB 2.0 lets the NULL type leave the binder; 1.5 typed these INTEGER.
$st = $db->query('SELECT NULL AS n, 1 AS i, [NULL] AS l, {\'a\': NULL} AS s FROM range(3)');
for ($i = 0; $i < $st->columnCount(); $i++) {
    $m = $st->getColumnMeta($i);
    echo $m['name'], ': ', $m['native_type'], ' pdo_type=', $m['pdo_type'], "\n";
}
var_dump(PDO::PARAM_NULL === $st->getColumnMeta(0)['pdo_type']);
foreach ($st->fetchAll(PDO::FETCH_NUM) as $row) {
    echo json_encode($row), "\n";
}

$p = $db->prepare('SELECT ? AS p');
$p->execute([null]);
echo $p->getColumnMeta(0)['native_type'], ' ', var_export($p->fetchColumn(), true), "\n";

$un = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION, PDO::DUCKDB_ATTR_UNBUFFERED => true]);
var_dump($un->query('SELECT NULL AS n')->fetch(PDO::FETCH_ASSOC));
?>
--EXPECT--
n: NULL pdo_type=0
i: INTEGER pdo_type=1
l: LIST pdo_type=2
s: STRUCT pdo_type=2
bool(true)
[null,1,"[NULL]","{'a': NULL}"]
[null,1,"[NULL]","{'a': NULL}"]
[null,1,"[NULL]","{'a': NULL}"]
NULL NULL
array(1) {
  ["n"]=>
  NULL
}
