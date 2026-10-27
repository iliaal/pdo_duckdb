--TEST--
pdo_duckdb: interleaving preserves rows in the current unbuffered chunk and closing allows reuse
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
require __DIR__ . '/duckdb_v2.inc';
$db = new PDO('duckdb::memory:', null, null, [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    PDO::DUCKDB_ATTR_UNBUFFERED => true,
]);

$first = $db->query('SELECT i FROM range(10) t(i)');
echo "first=", $first->fetchColumn(), "\n";

if (pdo_duckdb_v2_headers()) {
    try {
        $db->query('SELECT 99');
        echo "BAD: another query accepted with an open result\n";
    } catch (PDOException $e) {
        if (!str_contains($e->getMessage(), 'connection has an open result')) {
            throw $e;
        }
    }
} else {
    $second = $db->query('SELECT 99');
    $secondValue = $second->fetchColumn();
}

echo "first-next=", $first->fetchColumn(), "\n";
if (pdo_duckdb_v2_headers()) {
    if (!$first->closeCursor()) {
        throw new RuntimeException('closeCursor failed');
    }
    $secondValue = $db->query('SELECT 99')->fetchColumn();
}
echo "second=", $secondValue, "\n";
?>
--EXPECT--
first=0
first-next=1
second=99
