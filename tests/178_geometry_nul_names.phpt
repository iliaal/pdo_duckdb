--TEST--
pdo_duckdb: geometry rendering preserves NUL names and values with the engine's VARIANT field order
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
$db = new PDO('duckdb::memory:');
$expr = "from_json('{\"a\\u0000b\":1}', '{\"a\\u0000b\":\"INTEGER\"}')";
[$type, $json] = $db->query("SELECT typeof($expr), to_json($expr)")->fetch(PDO::FETCH_NUM);
if ($type === 'STRUCT(a INTEGER)' && $json === '{"a":null}') {
    die('skip DuckDB from_json truncates NUL field names before the driver receives them');
}
?>
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$hex = '0101000000000000000000F03F0000000000000040';
$fields = "from_json('{\"a\\u0000b\":\"c\\u0000d\"}', '{\"a\\u0000b\":\"VARCHAR\"}')";
$names = "struct_concat(struct_pack(g := 'POINT (1 2)'::GEOMETRY), $fields)";
$value = $db->query("SELECT $names")->fetchColumn();
if ($value !== "{'g': $hex, 'a\0b': c\0d}") throw new Exception(bin2hex($value));
echo "geometry and NUL struct name OK\n";
$hexNames = "struct_concat(struct_pack(g := '$hex'), $fields)";
[$got,$want] = $db->query("SELECT $names::VARIANT, CAST($hexNames::VARIANT AS VARCHAR)")->fetch(PDO::FETCH_NUM);
if ($got !== $want || !str_contains($got, "'a\0b': c\0d") || !str_contains($got, $hex)) {
    throw new Exception(bin2hex($got) . ' != ' . bin2hex($want));
}
echo "geometry and NUL VARIANT name/order OK\n";
?>
--EXPECT--
geometry and NUL struct name OK
geometry and NUL VARIANT name/order OK
