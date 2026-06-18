--TEST--
pdo_duckdb: large result set fetch iteration
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

$n = (int) $db->query("SELECT count(*) FROM range(10000)")->fetchColumn();
var_dump($n);

$rows = $db->query("SELECT range AS i FROM range(10000) ORDER BY range")
           ->fetchAll(PDO::FETCH_COLUMN);
var_dump(count($rows));
var_dump($rows[0]);
var_dump($rows[9999]);

// running sum sanity check over the whole set
var_dump(array_sum($rows));
?>
--EXPECT--
int(10000)
int(10000)
int(0)
int(9999)
int(49995000)
