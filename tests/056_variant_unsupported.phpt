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
var_dump($db->query('SELECT [CAST(NULL AS VARIANT)], {\'v\': CAST(NULL AS VARIANT)}')->fetch(PDO::FETCH_NUM));

/* The fetcher rejects the whole row before any column is read, so the
 * rejection does not depend on which column the caller asks for, and a row
 * after the first still fails fetchAll(). */
$rows = 'SELECT i, CASE WHEN i = 1 THEN CAST(i AS VARIANT) END AS v FROM range(3) t(i)';
foreach (['fetchAll' => static fn(PDOStatement $stmt) => $stmt->fetchAll(PDO::FETCH_NUM),
          'fetchColumn(0)' => static function (PDOStatement $stmt) { $stmt->fetchColumn(0); return $stmt->fetchColumn(0); },
          'nested' => static fn(PDOStatement $stmt) => $stmt->fetch(),
] as $label => $fetch) {
    try {
        $fetch($db->query($label === 'nested' ? 'SELECT [CAST(1 AS VARIANT)]' : $rows));
        echo "$label accepted (BUG)\n";
    } catch (PDOException $e) {
        echo $label, ': ', str_contains($e->getMessage(), 'VARIANT') ? 'VARIANT error' : $e->getMessage(), "\n";
    }
}

$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);
$stmt = $db->query('SELECT CAST(42 AS VARIANT)');
var_dump($stmt->fetchColumn());
var_dump($stmt->errorCode());

$stmt = $db->query($rows);
var_dump($stmt->fetch(PDO::FETCH_NUM), $stmt->fetch(PDO::FETCH_NUM), $stmt->errorInfo()[2], $stmt->fetch(PDO::FETCH_NUM));

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
array(2) {
  [0]=>
  string(6) "[NULL]"
  [1]=>
  string(11) "{'v': NULL}"
}
fetchAll: VARIANT error
fetchColumn(0): VARIANT error
nested: VARIANT error
bool(false)
string(5) "HY000"
array(2) {
  [0]=>
  int(0)
  [1]=>
  NULL
}
bool(false)
string(93) "DuckDB VARIANT values are not supported by the C result API; cast the value to VARCHAR in SQL"
array(2) {
  [0]=>
  int(2)
  [1]=>
  NULL
}
NULL
string(5) "00000"
