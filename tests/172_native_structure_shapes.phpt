--TEST--
pdo_duckdb: maps preserve typed keys, unions preserve tags, and tuples preserve order
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
$db = new PDO('duckdb::memory:', null, null, [PDO::DUCKDB_ATTR_NATIVE_STRUCTURES => true]);
check_native('list keys', $db->query('SELECT MAP([[1,2],[3]], [true,false])')->fetchColumn(), 'MAP(INTEGER[], BOOLEAN)',
    [['key'=>[1,2],'value'=>1],['key'=>[3],'value'=>0]]);
check_native('NUL keys', $db->query("SELECT MAP(['a'||chr(0)||'b','0'], [1,2])")->fetchColumn(), 'MAP(VARCHAR, INTEGER)',
    [['key'=>"a\0b",'value'=>1],['key'=>'0','value'=>2]]);
check_native('empty map', $db->query('SELECT MAP([]::INTEGER[], []::VARCHAR[])')->fetchColumn(), 'MAP(INTEGER, VARCHAR)', []);
check_native('empty list', $db->query('SELECT []::INTEGER[]')->fetchColumn(), 'INTEGER[]', []);
check_native('union', $db->query("SELECT union_value(s := ['a',NULL])")->fetchColumn(), 'UNION(s VARCHAR[])', ['tag'=>'s','value'=>['a',null]]);
check_native('union null', $db->query("SELECT union_value(s := NULL::VARCHAR)")->fetchColumn(), 'UNION(s VARCHAR)', ['tag'=>'s','value'=>null]);
$tupleType = $db->query("SELECT typeof((7,'x',[2,3]))")->fetchColumn();
check_native('tuple', $db->query("SELECT (7,'x',[2,3])")->fetchColumn(), $tupleType, [7,'x',[2,3]]);
$numeric = $db->query('SELECT struct_pack("0" := 9, "01" := 8, "18446744073709551616" := 7)')->fetchColumn();
$numericType = $db->query('SELECT typeof(struct_pack("0" := 9, "01" := 8, "18446744073709551616" := 7))')->fetchColumn();
check_native('numeric struct wrapper', $numeric, $numericType, [0=>9,'01'=>8,'18446744073709551616'=>7]);
$numeric = $numeric->value;
check('numeric struct names', array_keys($numeric), [0,'01','18446744073709551616']);
check('numeric struct access', [$numeric['0'],$numeric['01'],$numeric['18446744073709551616']], [9,8,7]);
?>
--EXPECT--
list keys OK
NUL keys OK
empty map OK
empty list OK
union OK
union null OK
tuple OK
numeric struct wrapper OK
numeric struct names OK
numeric struct access OK
