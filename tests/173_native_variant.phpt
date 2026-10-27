--TEST--
pdo_duckdb: native VARIANT preserves raw shapes, scalar types, and nested nulls
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
function check($name, $got, $want) {
    if (is_array($want) && array_key_exists('type', $want)) {
        if (get_debug_type($got) !== 'stdClass') throw new Exception($name . ': expected stdClass');
        $got = get_object_vars($got);
    }
    if ($got !== $want) throw new Exception($name . ': ' . var_export($got, true));
    echo "$name OK\n";
}
$db = new PDO('duckdb::memory:', null, null, [PDO::DUCKDB_ATTR_NATIVE_STRUCTURES => true]);
check('heterogeneous', $db->query("SELECT [1::VARIANT,'x'::VARIANT,NULL::VARIANT]::VARIANT")->fetchColumn(),
 ['type'=>'ARRAY','value'=>[['type'=>'INTEGER','value'=>1],['type'=>'VARCHAR','value'=>'x'],['type'=>'NULL','value'=>null]]]);
check('empty object key', $db->query("SELECT '{\"\":3,\"a\":[1,2]}'::JSON::VARIANT")->fetchColumn(),
 ['type'=>'OBJECT','value'=>[
 ['key'=>'','value'=>['type'=>'UBIGINT','value'=>'3']],
 ['key'=>'a','value'=>['type'=>'ARRAY','value'=>[['type'=>'UBIGINT','value'=>'1'],['type'=>'UBIGINT','value'=>'2']]]]]]);
check('decimal declaration', $db->query('SELECT 1.000000001::DECIMAL(38,9)::VARIANT')->fetchColumn(),
 ['type'=>'DECIMAL(38,9)','value'=>'1.000000001']);
check('SQL null', $db->query('SELECT NULL::VARIANT')->fetchColumn(), null);
check('nested variant', $db->query("SELECT struct_pack(v := 'a'::VARIANT)")->fetchColumn(),
 ['type'=>'STRUCT(v VARIANT)','value'=>['v'=>['type'=>'VARCHAR','value'=>'a']]]);
check('empty array', $db->query('SELECT []::INTEGER[]::VARIANT')->fetchColumn(), ['type'=>'ARRAY','value'=>[]]);
check('empty object', $db->query("SELECT '{}'::JSON::VARIANT AS v")->fetchColumn(), ['type'=>'OBJECT','value'=>[]]);
check('NUL object key', $db->query("SELECT json_object('a'||chr(0)||'b', 'c'||chr(0)||'d')::VARIANT")->fetchColumn(),
 ['type'=>'OBJECT','value'=>[['key'=>"a\0b",'value'=>['type'=>'VARCHAR','value'=>"c\0d"]]]]);
check('mixed empty-key object', $db->query("SELECT '[{\"\":42},[1,\"x\"]]'::JSON::VARIANT AS v")->fetchColumn(),
 ['type'=>'ARRAY','value'=>[
 ['type'=>'OBJECT','value'=>[['key'=>'','value'=>['type'=>'UBIGINT','value'=>'42']]]],
 ['type'=>'ARRAY','value'=>[['type'=>'UBIGINT','value'=>'1'],['type'=>'VARCHAR','value'=>'x']]]]]);
$db->setAttribute(PDO::DUCKDB_ATTR_NATIVE_STRUCTURES, false);
check('canonical empty key', $db->query("SELECT '{\"\":3,\"a\":[1,2]}'::JSON::VARIANT")->fetchColumn(), "{'': 3, 'a': [1, 2]}");
check('canonical nested empty key', $db->query("SELECT '[{\"\":42},[1,\"x\"]]'::JSON::VARIANT AS v")->fetchColumn(), "[{'': 42}, [1, x]]");
?>
--EXPECT--
heterogeneous OK
empty object key OK
decimal declaration OK
SQL null OK
nested variant OK
empty array OK
empty object OK
NUL object key OK
mixed empty-key object OK
canonical empty key OK
canonical nested empty key OK
