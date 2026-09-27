--TEST--
pdo_duckdb: a failed bind round releases a partially consumed unbuffered result
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_SILENT,
    PDO::DUCKDB_ATTR_UNBUFFERED => true,
]);
$stmt = $db->prepare('SELECT i FROM range(4) t(i) WHERE i >= ? ORDER BY i');
$stmt->execute([0]);
echo "first=", $stmt->fetchColumn(), "\n";
echo "failed=", $stmt->execute([0, 999]) ? 'no' : 'yes', "\n";
echo "columns=", $stmt->columnCount(), "\n";
echo "next=", var_export($stmt->fetchColumn(), true), "\n";

$stmt->execute([2]);
echo "rows=", implode(',', $stmt->fetchAll(PDO::FETCH_COLUMN)), "\n";
?>
--EXPECT--
first=0
failed=yes
columns=0
next=false
rows=2,3
