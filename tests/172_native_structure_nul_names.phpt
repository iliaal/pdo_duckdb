--TEST--
pdo_duckdb: native STRUCT preserves NUL field names when the SQL producer supports them
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
$db = new PDO('duckdb::memory:', null, null, [PDO::DUCKDB_ATTR_NATIVE_STRUCTURES => true]);
$value = $db->query("SELECT from_json('{\"a\\u0000b\":1}', '{\"a\\u0000b\":\"INTEGER\"}')")->fetchColumn();
if (get_debug_type($value) !== 'stdClass'
    || get_object_vars($value) !== ['type'=>"STRUCT(\"a\0b\" INTEGER)",'value'=>["a\0b"=>1]]
    || $value->value["a\0b"] !== 1) throw new Exception(var_export($value, true));
echo "NUL struct name and access OK\n";
?>
--EXPECT--
NUL struct name and access OK
