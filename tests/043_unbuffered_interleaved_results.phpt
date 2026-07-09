--TEST--
pdo_duckdb: unbuffered results can be interleaved without a driver-side guard
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    PDO::DUCKDB_ATTR_UNBUFFERED => true,
]);

$first = $db->query('SELECT i FROM range(10) t(i)');
echo "first=", $first->fetchColumn(), "\n";

$second = $db->query('SELECT 99');
echo "second=", $second->fetchColumn(), "\n";

echo "first-next=", $first->fetchColumn(), "\n";
?>
--EXPECT--
first=0
second=99
first-next=1
