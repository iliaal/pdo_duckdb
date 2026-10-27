--TEST--
pdo_duckdb: Appender accepts positional tuples and rejects malformed rows before appending
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
require __DIR__ . '/duckdb_v2.inc';
if (!pdo_duckdb_v2_headers()) die('skip DuckDB 1.5 cannot insert stored unnamed STRUCT values');
?>
--FILE--
<?php
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
$db->exec("CREATE TABLE tuples AS SELECT row(0::INTEGER, ''::VARCHAR) AS pair,
    [row(0::INTEGER, ''::VARCHAR)] AS pairs WHERE false");
$app = $db->duckdbAppender('tuples');
$app->appendRow([1, 'first'], [[2, 'nested']]);
foreach ([
    'named keys' => [['x' => 3, 'y' => 'bad'], []],
    'reordered keys' => [[1 => 'bad', 0 => 3], []],
    'missing value' => [[3], []],
    'extra value' => [[3, 'bad', 4], []],
    'nested keys' => [[3, 'bad'], [['x' => 4, 'y' => 'bad']]],
] as $label => $values) {
    try {
        $app->appendRow(...$values);
        echo "$label accepted\n";
    } catch (ValueError $e) {
        echo "$label rejected\n";
    }
}
$app->appendRow([5, 'last'], [[6, 'finish']]);
$app->close();
var_dump($db->query('SELECT pair[1], pair[2], pairs[1][1], pairs[1][2] FROM tuples ORDER BY pair[1]')->fetchAll(PDO::FETCH_NUM));
?>
--EXPECT--
named keys rejected
reordered keys rejected
missing value rejected
extra value rejected
nested keys rejected
array(2) {
  [0]=>
  array(4) {
    [0]=>
    int(1)
    [1]=>
    string(5) "first"
    [2]=>
    int(2)
    [3]=>
    string(6) "nested"
  }
  [1]=>
  array(4) {
    [0]=>
    int(5)
    [1]=>
    string(4) "last"
    [2]=>
    int(6)
    [3]=>
    string(6) "finish"
  }
}
