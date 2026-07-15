--TEST--
pdo_duckdb: nested fast-rendered values match DuckDB canonical strings
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

$cases = [
    'list_int' => "[1, 2, NULL]::INTEGER[]",
    'nested_int' => "[[1, 2], [], [NULL, 3]]::INTEGER[][]",
    'bools' => "[true, false, NULL]::BOOLEAN[]",
    'struct_int' => "{'x': 1::INTEGER, 'y': 2::BIGINT}",
    'struct_nested' => "{'inner': {'a': 1::INTEGER, 'b': NULL::INTEGER}, 'z': [4, 5]::INTEGER[]}",
    'map_int' => "MAP {1: 10::INTEGER, 2: NULL::INTEGER}",
    'dates' => "[DATE '2026-01-01', NULL]",
    'uuids' => "['12345678-1234-5678-1234-567812345678'::UUID, NULL]",
    'decimals' => "[123.45::DECIMAL(10,2), NULL]",
    'wide_decimals' => "[CAST('99999999999999999999999999999999999999' AS DECIMAL(38,0)), CAST('-99999999999999999999999999999999999999' AS DECIMAL(38,0))]",
    'ubigints' => "[0::UBIGINT, 18446744073709551615::UBIGINT]",
    'hugeints' => "[CAST('-170141183460469231731687303715884105728' AS HUGEINT), CAST('170141183460469231731687303715884105727' AS HUGEINT)]",
    'uhugeints' => "[0::UHUGEINT, 340282366920938463463374607431768211455::UHUGEINT]",
    'unions' => "[union_value(a := 42)::UNION(a INTEGER, b INTEGER), union_value(b := 7)::UNION(a INTEGER, b INTEGER)]",
    'struct_name_fallback' => "{'unsafe name': 1::INTEGER}",
    'string_fallback' => "['a,b', CAST(42 AS VARCHAR)]::VARCHAR[]",
];

foreach ($cases as $label => $expr) {
    $row = $db->query("SELECT $expr AS got, CAST($expr AS VARCHAR) AS want")->fetch(PDO::FETCH_ASSOC);
    echo $label, ': ', ($row['got'] === $row['want'] ? 'ok' : "MISMATCH {$row['got']} != {$row['want']}"), "\n";
}

$stmt = $db->prepare(
    'SELECT [CAST(? AS INTEGER), CAST(? AS INTEGER)] AS got, '
    . 'CAST([CAST(? AS INTEGER), CAST(? AS INTEGER)] AS VARCHAR) AS want'
);
$reexecute = true;
foreach ([1, 2, 3] as $value) {
    $stmt->execute([$value, $value + 1, $value, $value + 1]);
    $row = $stmt->fetch(PDO::FETCH_ASSOC);
    $reexecute = $reexecute && $row['got'] === $row['want'];
}
echo 'reexecute: ', $reexecute ? 'ok' : 'MISMATCH', "\n";
?>
--EXPECT--
list_int: ok
nested_int: ok
bools: ok
struct_int: ok
struct_nested: ok
map_int: ok
dates: ok
uuids: ok
decimals: ok
wide_decimals: ok
ubigints: ok
hugeints: ok
uhugeints: ok
unions: ok
struct_name_fallback: ok
string_fallback: ok
reexecute: ok
