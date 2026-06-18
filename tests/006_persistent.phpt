--TEST--
pdo_duckdb: persistent connection is reused within a process
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$opts = [PDO::ATTR_PERSISTENT => true, PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];

$db1 = new PDO('duckdb::memory:', null, null, $opts);
$db1->exec("CREATE TABLE p (x INTEGER)");
$db1->exec("INSERT INTO p VALUES (7)");
unset($db1);

// Same DSN + persistent => PDO hands back the same underlying handle, so the
// in-memory table created above is still visible.
$db2 = new PDO('duckdb::memory:', null, null, $opts);
var_dump((int) $db2->query("SELECT x FROM p")->fetchColumn());
?>
--EXPECT--
int(7)
