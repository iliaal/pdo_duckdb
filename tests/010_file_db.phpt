--TEST--
pdo_duckdb: file-backed database persists across close/reopen
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$file = __DIR__ . '/010_file_db.db';
@unlink($file);

$db = new PDO('duckdb:' . $file);
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec('CREATE TABLE t (id INTEGER, name VARCHAR)');
$db->exec("INSERT INTO t VALUES (1, 'persisted'), (2, 'durable')");
unset($db);                 // disconnect + close -> checkpoint to disk

var_dump(file_exists($file));

// reopen the same file in a fresh handle
$db2 = new PDO('duckdb:' . $file);
$db2->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
var_dump((int) $db2->query('SELECT count(*) FROM t')->fetchColumn());
var_dump($db2->query('SELECT name FROM t WHERE id = 2')->fetchColumn());

// a persistent file handle reused within the process sees the same data
$opts = [PDO::ATTR_PERSISTENT => true, PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
$p1 = new PDO('duckdb:' . $file, null, null, $opts);
$p1->exec("INSERT INTO t VALUES (3, 'via-persistent')");
unset($p1);
$p2 = new PDO('duckdb:' . $file, null, null, $opts);
var_dump((int) $p2->query('SELECT count(*) FROM t')->fetchColumn());
unset($p2);
?>
--EXPECT--
bool(true)
int(2)
string(7) "durable"
int(3)
--CLEAN--
<?php
@unlink(__DIR__ . '/010_file_db.db');
@unlink(__DIR__ . '/010_file_db.db.wal');
?>
