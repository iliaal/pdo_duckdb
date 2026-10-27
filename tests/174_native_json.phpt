--TEST--
pdo_duckdb: JSON preserves duplicate keys, exact number lexemes, and decoded strings
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
function json_column(PDOStatement $statement): array {
    $value = $statement->fetchColumn();
    if (get_debug_type($value) !== 'stdClass') throw new Exception('Expected stdClass column wrapper');
    $properties = get_object_vars($value);
    if (array_keys($properties) !== ['type','value']) throw new Exception('Unexpected column wrapper properties');
    return $properties;
}
$db = new PDO('duckdb::memory:', null, null, [PDO::DUCKDB_ATTR_NATIVE_STRUCTURES => true]);
$st = $db->prepare('SELECT CAST(? AS JSON)');
$st->execute(['{"a":9007199254740993,"a":-0,"\u0000":1.20e+03,"s":"\ud83d\ude00\n","x":[],"y":{},"z":[null,true,false]}']);
$got = json_column($st);
$want = ['type'=>'JSON','value'=>['type'=>'object','value'=>[
 ['key'=>'a','value'=>['type'=>'number','value'=>'9007199254740993']],
 ['key'=>'a','value'=>['type'=>'number','value'=>'-0']],
 ['key'=>"\0",'value'=>['type'=>'number','value'=>'1.20e+03']],
 ['key'=>'s','value'=>"😀\n"], ['key'=>'x','value'=>[]],
 ['key'=>'y','value'=>['type'=>'object','value'=>[]]],
 ['key'=>'z','value'=>[null,true,false]]]]];
if ($got !== $want) throw new Exception(var_export($got, true));
echo "lossless JSON object OK\n";
foreach (['-1E-9999','1e9999','0.000000000000000000001','-0.0','18446744073709551616'] as $number) {
    $st->execute([$number]);
    if (json_column($st) !== ['type'=>'JSON','value'=>['type'=>'number','value'=>$number]]) throw new Exception($number);
}
echo "exact JSON numbers OK\n";
if (json_column($db->query("SELECT ['null'::JSON, NULL::JSON]")) !== ['type'=>'JSON[]','value'=>[['type'=>'JSON','value'=>null],null]]) throw new Exception('JSON null distinction');
echo "JSON null distinction OK\n";
$valid = $db->prepare('SELECT json_valid(?)');
$count = 0;
foreach (['nan','inf','infinity'] as $word) {
    for ($mask=0; $mask<(1 << strlen($word)); $mask++) {
        $spelling = '';
        for ($i=0; $i<strlen($word); $i++) {
            $spelling .= ($mask & (1 << $i)) ? strtoupper($word[$i]) : $word[$i];
        }
        foreach (['','-'] as $sign) {
            $token = $sign . $spelling;
            $valid->execute([$token]);
            if ($valid->fetchColumn() !== 1) throw new Exception("Engine rejected $token");
            $number = ['type'=>'number','value'=>$token];
            $st->execute([$token]);
            if (json_column($st) !== ['type'=>'JSON','value'=>$number]) throw new Exception("Non-finite scalar $token");
            $st->execute(['[' . $token . ',{"n":' . $token . '}]']);
            $want = ['type'=>'JSON','value'=>[$number,['type'=>'object','value'=>[['key'=>'n','value'=>$number]]]]];
            if (json_column($st) !== $want) throw new Exception("Non-finite nested value $token");
            $count++;
        }
    }
}
if ($count !== 544) throw new Exception("Non-finite spelling coverage $count");
echo "544 non-finite JSON spellings preserved OK\n";
foreach (['+NaN','+Inf','+Infinity','NaN0','NaNx','Infinityx','Infinit','infi','--Infinity','+-Infinity','-','+1','01','1.','0x10','-.Inf'] as $token) {
    $valid->execute([$token]);
    if ($valid->fetchColumn() !== 0) throw new Exception("Engine accepted invalid $token");
    try {
        $st->execute([$token]);
        throw new Exception("Invalid JSON accepted: $token");
    } catch (PDOException $e) {
    }
}
echo "invalid JSON tokens rejected OK\n";
$got = json_column($db->query("SELECT to_json(['NaN'::DOUBLE,'Infinity'::DOUBLE,'-Infinity'::DOUBLE])"));
$want = ['type'=>'JSON','value'=>[
 ['type'=>'number','value'=>'NaN'],['type'=>'number','value'=>'Infinity'],['type'=>'number','value'=>'-Infinity']]];
if ($got !== $want) throw new Exception(var_export($got, true));
echo "engine-generated non-finite JSON OK\n";
?>
--EXPECT--
lossless JSON object OK
exact JSON numbers OK
JSON null distinction OK
544 non-finite JSON spellings preserved OK
invalid JSON tokens rejected OK
engine-generated non-finite JSON OK
