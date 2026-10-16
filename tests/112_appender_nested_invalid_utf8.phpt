--TEST--
pdo_duckdb: appender reports invalid UTF-8 in nested string leaves and stays usable
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec('CREATE TABLE t (i INTEGER, l VARCHAR[], st STRUCT(x INTEGER, "y z" VARCHAR), m MAP(VARCHAR, VARCHAR))');

$a = $db->duckdbAppender('t');
$a->appendRow(1, ['ok'], ['x' => 1, 'y z' => 'ok'], ['k' => 'v']);
$bad = [
    'list' => [2, ["\xff"], null, null],
    'struct' => [2, null, ['x' => 1, 'y z' => "\xff"], null],
    'map value' => [2, null, null, ['k' => "\xff"]],
    'map key' => [2, null, null, ["\xff" => 'v']],
];
foreach ($bad as $label => $row) {
    try {
        $a->appendRow(...$row);
        echo "$label: accepted\n";
    } catch (TypeError $e) {
        echo "$label: ", $e->getMessage(), "\n";
    }
}
$a->appendRow(3, ['b'], null, null);
$a->close();
echo $db->query('SELECT string_agg(i::VARCHAR, \',\' ORDER BY i) FROM t')->fetchColumn(), "\n";
?>
--EXPECT--
list: Pdo\Duckdb\Appender::appendRow(): argument #2 contains a string that is not valid UTF-8
struct: Pdo\Duckdb\Appender::appendRow(): argument #3 contains a string that is not valid UTF-8
map value: Pdo\Duckdb\Appender::appendRow(): argument #4 contains a string that is not valid UTF-8
map key: Pdo\Duckdb\Appender::appendRow(): argument #4 contains a string that is not valid UTF-8
1,3
