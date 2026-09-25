--TEST--
pdo_duckdb: standalone bindParam and bindValue do not erase the last result
--EXTENSIONS--
pdo
pdo_duckdb
--XFAIL--
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_SILENT);
$stmt = $db->prepare('SELECT ? AS v');
$stmt->execute([11]);
echo "before=", $stmt->fetchColumn(), "\n";
$stmt->bindValue(1, 22);
echo "after_bind_value=", $stmt->fetchColumn(), "\n";
$stmt->bindParam(1, $value);
$value = 33;
echo "after_bind_param=", $stmt->fetchColumn(), "\n";

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
before=11
after_bind_value=11
after_bind_param=11
insert_before=1
insert_after_bind_value=1
insert_after_bind_param=1
