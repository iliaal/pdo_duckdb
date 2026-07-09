--TEST--
pdo_duckdb: closeCursor releases an unbuffered result so another stream can run
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    PDO::DUCKDB_ATTR_UNBUFFERED => true,
]);

$st = $db->query('SELECT i FROM range(100000) t(i)');
echo "first=", $st->fetchColumn(), "\n";
var_dump($st->closeCursor());

$st2 = $db->query('SELECT i FROM range(3) t(i)');
echo implode(',', $st2->fetchAll(PDO::FETCH_COLUMN)), "\n";
?>
--EXPECT--
first=0
bool(true)
0,1,2
