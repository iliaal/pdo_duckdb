--TEST--
pdo_duckdb: non-NULL VARIANT fetches report an error instead of silently becoming NULL
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

$fetches = [
    'fetch' => static fn(PDOStatement $stmt) => $stmt->fetch(PDO::FETCH_ASSOC),
    'fetchColumn' => static fn(PDOStatement $stmt) => $stmt->fetchColumn(),
    'fetchAll' => static fn(PDOStatement $stmt) => $stmt->fetchAll(PDO::FETCH_ASSOC),
];
foreach ($fetches as $label => $fetch) {
    try {
        $fetch($db->query('SELECT CAST(42 AS VARIANT) AS value'));
        echo "$label accepted (BUG)\n";
    } catch (PDOException $e) {
        echo $label, ': ', str_contains($e->getMessage(), 'VARIANT') ? 'VARIANT error' : $e->getMessage(), "\n";
    }
}

var_dump($db->query('SELECT CAST(NULL AS VARIANT)')->fetchColumn());
var_dump($db->query('SELECT CAST(CAST(42 AS VARIANT) AS VARCHAR)')->fetchColumn());
var_dump($db->query('SELECT 7')->fetchColumn());

$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);
$stmt = $db->query('SELECT CAST(42 AS VARIANT)');
var_dump($stmt->fetchColumn());
var_dump($stmt->errorCode());

$stmt = $db->query('SELECT CAST(NULL AS VARIANT)');
var_dump($stmt->fetchColumn());
var_dump($stmt->errorCode());
?>
--EXPECT--
fetch: VARIANT error
fetchColumn: VARIANT error
fetchAll: VARIANT error
NULL
string(2) "42"
int(7)
NULL
string(5) "HY000"
NULL
string(5) "00000"
