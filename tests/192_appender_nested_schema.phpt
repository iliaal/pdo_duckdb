--TEST--
pdo_duckdb: Appender targets nested schemas with quoted segments, column subsets, and defaults
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
require __DIR__ . '/duckdb_v2.inc';
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
if (!pdo_duckdb_v2_headers()) {
    try {
        $db->duckdbAppender('t', ['outer', 'inner']);
        throw new Exception('old engine accepted a nested schema path');
    } catch (PDOException $e) {
        if (!str_contains(strtolower($e->getMessage()), 'nested') || !str_contains($e->getMessage(), '2.0')) throw $e;
    }
    echo "nested schema contract verified\n";
    return;
}
function ident(string $name): string { return '"' . str_replace('"', '""', $name) . '"'; }
$outer = 'outer.dot';
$inner = 'inner"; DROP TABLE sentinel; --';
$leaf = 'leaf.dot';
$table = 'rows"quoted';
$column = 'value"quoted';
$db->exec('CREATE TABLE sentinel (i INTEGER)');
$db->exec('INSERT INTO sentinel VALUES (987654321)');
$path = [];
foreach ([$outer, $inner, $leaf] as $segment) {
    $path[] = $segment;
    $db->exec('CREATE SCHEMA ' . implode('.', array_map('ident', $path)));
}
$target = implode('.', array_map('ident', $path)) . '.' . ident($table);
$db->exec('CREATE TABLE ' . $target . ' (id INTEGER, ' . ident($column) . ' VARCHAR, omitted INTEGER DEFAULT 29)');
$app = $db->duckdbAppender($table, $path, [$column, 'id']);
$app->appendRow('first', 1)->appendRow('second', 2)->close();
$rows = $db->query('SELECT id, ' . ident($column) . ', omitted FROM ' . $target . ' ORDER BY id')->fetchAll(PDO::FETCH_NUM);
if ($rows !== [[1, 'first', 29], [2, 'second', 29]]) throw new Exception('wrong target, column order, or defaults');
if ($db->query('SELECT i FROM sentinel')->fetchColumn() !== 987654321) throw new Exception('identifier executed as SQL');
echo "nested schema contract verified\n";
?>
--EXPECT--
nested schema contract verified
