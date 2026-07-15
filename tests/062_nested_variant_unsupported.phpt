--TEST--
pdo_duckdb: nested non-NULL VARIANT leaves report an error instead of silently becoming NULL
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

$cases = [
    'list' => '[CAST(42 AS VARIANT)]',
    'struct' => "{'v': CAST(42 AS VARIANT)}",
    'map' => 'MAP {1: CAST(42 AS VARIANT)}',
    'union' => 'union_value(v := CAST(42 AS VARIANT))::UNION(v VARIANT, i INTEGER)',
];
foreach ($cases as $label => $expression) {
    try {
        $db->query("SELECT $expression")->fetchColumn();
        echo "$label accepted (BUG)\n";
    } catch (PDOException $e) {
        echo $label, ': ', str_contains($e->getMessage(), 'VARIANT') ? 'VARIANT error' : $e->getMessage(), "\n";
    }
}

$stmt = $db->query('SELECT [CAST(NULL AS VARIANT)]');
var_dump($stmt->fetchColumn());
var_dump($stmt->errorCode());

$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);
$stmt = $db->query('SELECT [CAST(42 AS VARIANT)]');
var_dump($stmt->fetchColumn());
var_dump($stmt->errorCode());
?>
--EXPECT--
list: VARIANT error
struct: VARIANT error
map: VARIANT error
union: VARIANT error
string(6) "[NULL]"
string(5) "00000"
NULL
string(5) "HY000"
