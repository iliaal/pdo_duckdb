--TEST--
pdo_duckdb: standalone bindParam and bindValue follow PDO rebinding semantics
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);
$stmt = $db->prepare('SELECT i FROM range(3) t(i) WHERE i >= ?');
$stmt->execute([0]);
echo "first=", $stmt->fetchColumn(), "\n";
$stmt->bindValue(1, 1);
echo "after_bind_value=", var_export($stmt->fetchColumn(), true), "\n";
$stmt->bindParam(1, $value);
$value = 2;
echo "after_bind_param=", var_export($stmt->fetchColumn(), true), "\n";

$db->exec('CREATE TABLE t (v INTEGER)');
$insert = $db->prepare('INSERT INTO t VALUES (?)');
$insert->execute([1]);
echo "insert_before=", $insert->rowCount(), "\n";
$insert->bindValue(1, 2);
echo "insert_after_bind_value=", $insert->rowCount(), "\n";
$insert->bindParam(1, $value);
$value = 3;
echo "insert_after_bind_param=", $insert->rowCount(), "\n";
?>
--EXPECT--
first=0
after_bind_value=1
after_bind_param=2
insert_before=1
insert_after_bind_value=1
insert_after_bind_param=1
