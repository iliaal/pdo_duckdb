--TEST--
pdo_duckdb: unbuffered SELECT rowCount is 0 and does not disturb streaming
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::DUCKDB_ATTR_UNBUFFERED => true]);
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

$st = $db->query('SELECT i FROM range(5) t(i)');
echo "select rowCount before fetch=", $st->rowCount(), "\n";
echo "values=", implode(',', $st->fetchAll(PDO::FETCH_COLUMN)), "\n";
echo "select rowCount after fetch=", $st->rowCount(), "\n";

$db->exec('CREATE TABLE t (i INTEGER)');
$ins = $db->prepare('INSERT INTO t VALUES (1), (2), (3)');
$ins->execute();
echo "insert rowCount=", $ins->rowCount(), "\n";
?>
--EXPECT--
select rowCount before fetch=0
values=0,1,2,3,4
select rowCount after fetch=0
insert rowCount=3

