--TEST--
pdo_duckdb: nested native values preserve scalar mappings and precision
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::DUCKDB_ATTR_NATIVE_STRUCTURES => true]);
$expr = "struct_pack(
 b := true, i := -7::INTEGER, f := 1.25::DOUBLE,
 u := 18446744073709551615::UBIGINT,
 h := 170141183460469231731687303715884105727::HUGEINT,
 d := 1.123456789012345678::DECIMAL(38,18),
 t := TIMESTAMP '2024-02-29 12:34:56.123456',
 s := 'a'||chr(0)||'b', x := '\\x00\\xFF'::BLOB,
 a := [1,NULL,3]::INTEGER[3], e := []::INTEGER[], n := NULL::INTEGER)";
$got = $db->query("SELECT $expr")->fetchColumn();
$type = $db->query("SELECT typeof($expr)")->fetchColumn();
$want = ['b'=>1,'i'=>-7,'f'=>1.25,'u'=>'18446744073709551615',
 'h'=>'170141183460469231731687303715884105727','d'=>'1.123456789012345678',
 't'=>'2024-02-29 12:34:56.123456','s'=>"a\0b",'x'=>"\0\xff",
 'a'=>[1,null,3],'e'=>[],'n'=>null];
if (get_debug_type($got) !== 'stdClass' || get_object_vars($got) !== ['type'=>$type,'value'=>$want]) {
    throw new Exception(var_export($got, true));
}
echo "nested scalar mappings OK\n";
$got = $db->query("SELECT [struct_pack(k := 1), NULL, struct_pack(k := 2)]")->fetchColumn();
if (get_debug_type($got) !== 'stdClass' || get_object_vars($got) !== ['type'=>'STRUCT(k INTEGER)[]','value'=>[['k'=>1],null,['k'=>2]]]) {
    throw new Exception('nested validity');
}
echo "nested nulls OK\n";
?>
--EXPECT--
nested scalar mappings OK
nested nulls OK
