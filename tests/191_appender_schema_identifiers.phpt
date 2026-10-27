--TEST--
pdo_duckdb: Appender keeps schema strings literal and validates explicit identifier paths
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
function ident(string $name): string { return '"' . str_replace('"', '""', $name) . '"'; }
$db->exec('CREATE TABLE sentinel (value INTEGER)');
$db->exec('INSERT INTO sentinel VALUES (87654321)');
$table = 'row"; DROP TABLE sentinel; --';
$column = 'value"; DROP TABLE sentinel; --';
foreach (['literal.dot', '"literal"."path"', 'odd"schema'] as $index => $schema) {
    $db->exec('CREATE SCHEMA ' . ident($schema));
    $target = ident($schema) . '.' . ident($table);
    $db->exec('CREATE TABLE ' . $target . ' (' . ident($column) . ' INTEGER, omitted INTEGER DEFAULT 17, label VARCHAR)');
    $app = $db->duckdbAppender($table, $schema, ['label', $column]);
    $app->appendRow('literal', 1);
    $app->close();
    $app = $db->duckdbAppender($table, [$schema], [$column, 'label']);
    $app->appendRow(2, 'segment');
    $app->close();
    echo $index, ':', json_encode($db->query('SELECT ' . ident($column) . ', omitted, label FROM ' . $target . ' ORDER BY 1')->fetchAll(PDO::FETCH_NUM)), "\n";
}
foreach ([
    'empty path' => [],
    'keyed path' => ['schema' => 'main'],
    'empty segment' => [''],
    'NUL segment' => ["main\0wrong"],
    'integer segment' => [123],
    'nested segment' => [['main']],
] as $label => $path) {
    try {
        $db->duckdbAppender('sentinel', $path);
        echo "$label accepted\n";
    } catch (ValueError|TypeError $e) {
        echo "$label rejected\n";
    }
}
var_dump($db->query('SELECT value FROM sentinel')->fetchColumn());
?>
--EXPECT--
0:[[1,17,"literal"],[2,17,"segment"]]
1:[[1,17,"literal"],[2,17,"segment"]]
2:[[1,17,"literal"],[2,17,"segment"]]
empty path rejected
keyed path rejected
empty segment rejected
NUL segment rejected
integer segment rejected
nested segment rejected
int(87654321)
