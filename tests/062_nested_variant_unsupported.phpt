--TEST--
pdo_duckdb: nested VARIANT values retain exact canonical text and native dynamic types
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

$cases = [
    'list' => ['[CAST(42 AS VARIANT)]', '[42]', 'VARIANT[]', [['type'=>'INTEGER','value'=>42]]],
    'struct' => ["{'v': CAST(42 AS VARIANT)}", "{'v': 42}", 'STRUCT(v VARIANT)', ['v'=>['type'=>'INTEGER','value'=>42]]],
    'map' => ['MAP {1: CAST(42 AS VARIANT)}', '{1=42}', 'MAP(INTEGER, VARIANT)', [['key'=>1,'value'=>['type'=>'INTEGER','value'=>42]]]],
    'union' => ['union_value(v := CAST(42 AS VARIANT))::UNION(v VARIANT, i INTEGER)', '42', 'UNION(v VARIANT, i INTEGER)', ['tag'=>'v','value'=>['type'=>'INTEGER','value'=>42]]],
];
foreach ($cases as $label => [$expression, $canonical, $type, $payload]) {
    $db->setAttribute(PDO::DUCKDB_ATTR_NATIVE_STRUCTURES, false);
    if ($db->query("SELECT $expression AS v")->fetchColumn() !== $canonical) throw new Exception("$label canonical");
    $db->setAttribute(PDO::DUCKDB_ATTR_NATIVE_STRUCTURES, true);
    $value = $db->query("SELECT $expression AS v")->fetchColumn();
    if (!$value instanceof stdClass || get_object_vars($value) !== ['type'=>$type,'value'=>$payload]) {
        throw new Exception("$label native: " . var_export($value, true));
    }
    echo "$label canonical/native OK\n";
}

$db->setAttribute(PDO::DUCKDB_ATTR_NATIVE_STRUCTURES, false);
$stmt = $db->query('SELECT [CAST(NULL AS VARIANT)]');
var_dump($stmt->fetchColumn());
var_dump($stmt->errorCode());

$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);
$stmt = $db->query('SELECT [CAST(42 AS VARIANT)]');
var_dump($stmt->fetchColumn());
var_dump($stmt->errorCode());
?>
--EXPECT--
list canonical/native OK
struct canonical/native OK
map canonical/native OK
union canonical/native OK
string(6) "[NULL]"
string(5) "00000"
string(4) "[42]"
string(5) "00000"
