--TEST--
pdo_duckdb: DuckDB 2.0 VARIANT Appender values preserve metadata and exact canonical and native fetches
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
function check($got, $want): void {
    if ($want instanceof stdClass) {
        if (!is_object($got) || get_class($got) !== stdClass::class) throw new Exception('expected stdClass wrapper');
        $got = get_object_vars($got);
        $want = get_object_vars($want);
    }
    if (is_array($want) && is_array($got) && array_keys($got) === array_keys($want)) {
        foreach ($want as $key => $value) check($got[$key], $value);
        return;
    }
    if ($got !== $want) throw new Exception(var_export([$got, $want], true));
}
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]) : new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('CREATE TABLE v (id INTEGER, v VARIANT)');
$ap = $db->duckdbAppender('v');
foreach (['abc', 42, 1.5, true, null, ['a' => 1], [1, 'two', null]] as $i => $value) {
    $ap->appendRow($i + 1, $value);
}
$resource = fopen('php://memory', 'r+');
try {
    $ap->appendRow(99, $resource);
    throw new Exception('resource accepted');
} catch (TypeError $e) {
    echo "resource rejected before append\n";
}
fclose($resource);
$ap->appendRow(8, false);
$ap->close();

foreach ($db->query('SELECT id, CAST(v AS VARCHAR), variant_typeof(v) FROM v WHERE id <= 5 ORDER BY id')->fetchAll(PDO::FETCH_NUM) as $row) {
    echo json_encode($row), "\n";
}
$st = $db->query('SELECT v FROM v ORDER BY id');
$m = $st->getColumnMeta(0);
echo $m['native_type'], ' pdo_type=', $m['pdo_type'], "\n";
if ($st->fetchAll(PDO::FETCH_COLUMN) !== ['abc', '42', '1.5', 'true', null, "{'a': 1}", '[1, two, NULL]', 'false']) {
    throw new Exception('canonical VARIANT values changed');
}
echo "canonical VARIANT fetch OK\n";

$db->setAttribute(PDO::DUCKDB_ATTR_NATIVE_STRUCTURES, true);
$object = ['type' => 'OBJECT', 'value' => [['key' => 'a', 'value' => ['type' => 'BIGINT', 'value' => 1]]]];
$expected = [
    ['type' => 'VARCHAR', 'value' => 'abc'],
    ['type' => 'BIGINT', 'value' => 42],
    ['type' => 'DOUBLE', 'value' => 1.5],
    ['type' => 'BOOLEAN', 'value' => 1],
    null,
    $object,
    ['type' => 'ARRAY', 'value' => [
        ['type' => 'BIGINT', 'value' => 1],
        ['type' => 'VARCHAR', 'value' => 'two'],
        ['type' => 'NULL', 'value' => null],
    ]],
    ['type' => 'BOOLEAN', 'value' => 0],
];
$expected = array_map(static fn($value) => $value === null ? null : (object)$value, $expected);
check($db->query('SELECT v FROM v ORDER BY id')->fetchAll(PDO::FETCH_COLUMN), $expected);
echo "native VARIANT fetch OK\n";
check($db->query('SELECT [v], struct_pack(v := v) FROM v WHERE id = 6')->fetch(PDO::FETCH_NUM),
    [(object)['type' => 'VARIANT[]', 'value' => [$object]],
        (object)['type' => 'STRUCT(v VARIANT)', 'value' => ['v' => $object]]]);
echo "native nested VARIANT fetch OK\n";
?>
--EXPECT--
resource rejected before append
[1,"abc","VARCHAR"]
[2,"42","INT64"]
[3,"1.5","DOUBLE"]
[4,"true","BOOL_TRUE"]
[5,null,"VARIANT_NULL"]
VARIANT pdo_type=2
canonical VARIANT fetch OK
native VARIANT fetch OK
native nested VARIANT fetch OK
