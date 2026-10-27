--TEST--
pdo_duckdb: GEOMETRY keeps hexadecimal WKB at every depth with length-aware rendering
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
function check($name, $got, $want) {
    if ($got !== $want) throw new Exception($name . ': ' . var_export($got, true));
    echo "$name OK\n";
}
function check_native($name, $got, $type, $value) {
    if (get_debug_type($got) !== 'stdClass') throw new Exception($name . ': expected stdClass');
    check($name, get_object_vars($got), ['type'=>$type,'value'=>$value]);
}
$db = new PDO('duckdb::memory:');
$hex = '0101000000000000000000F03F0000000000000040';
$geometry = "'POINT (1 2)'::GEOMETRY";
$mixed = "struct_pack(g := $geometry, s := 'a'||chr(0)||'b')";
check('canonical geometry', $db->query("SELECT $geometry")->fetchColumn(), $hex);
check('canonical mixed NUL', $db->query("SELECT $mixed")->fetchColumn(), "{'g': $hex, 's': a\0b}");
check('canonical list', $db->query("SELECT [$geometry]")->fetchColumn(), "[$hex]");
check('canonical array', $db->query("SELECT [$geometry]::GEOMETRY[1]")->fetchColumn(), "[$hex]");
check('canonical union', $db->query("SELECT union_value(g := $geometry)")->fetchColumn(), $hex);
check('canonical map', $db->query("SELECT MAP(['a'||chr(0)||'b'], [$geometry])")->fetchColumn(), "{a\0b=$hex}");
check('canonical tuple', $db->query("SELECT ($geometry, 'a'||chr(0)||'b')")->fetchColumn(), "($hex, a\0b)");
check('canonical variant geometry', $db->query("SELECT $geometry::VARIANT")->fetchColumn(), $hex);
$db->setAttribute(PDO::DUCKDB_ATTR_NATIVE_STRUCTURES, true);
check('native scalar geometry', $db->query("SELECT $geometry")->fetchColumn(), $hex);
check_native('native mixed', $db->query("SELECT $mixed")->fetchColumn(), 'STRUCT(g GEOMETRY, s VARCHAR)', ['g'=>$hex,'s'=>"a\0b"]);
check_native('native array', $db->query("SELECT [$geometry]::GEOMETRY[1]")->fetchColumn(), 'GEOMETRY[1]', [$hex]);
check_native('native union', $db->query("SELECT union_value(g := $geometry)")->fetchColumn(), 'UNION(g GEOMETRY)', ['tag'=>'g','value'=>$hex]);
check_native('native variant', $db->query("SELECT $geometry::VARIANT")->fetchColumn(), 'GEOMETRY', $hex);
?>
--EXPECT--
canonical geometry OK
canonical mixed NUL OK
canonical list OK
canonical array OK
canonical union OK
canonical map OK
canonical tuple OK
canonical variant geometry OK
native scalar geometry OK
native mixed OK
native array OK
native union OK
native variant OK
