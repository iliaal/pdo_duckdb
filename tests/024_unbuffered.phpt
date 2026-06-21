--TEST--
pdo_duckdb: opt-in unbuffered (streaming) result mode
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
// Streaming is opt-in via PDO::DUCKDB_ATTR_UNBUFFERED; default stays buffered.
$db = new PDO('duckdb::memory:', null, null, [PDO::DUCKDB_ATTR_UNBUFFERED => true]);
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

var_dump($db->getAttribute(PDO::DUCKDB_ATTR_UNBUFFERED));

// a large result streams chunk-by-chunk; values are correct
$st = $db->query('SELECT i FROM range(50000) t(i)');
$sum = 0; $rows = 0;
while (($v = $st->fetchColumn()) !== false) { $sum += (int) $v; $rows++; }
echo "rows=$rows sum=$sum\n";
$st = null;

// streamed data matches the buffered path
$buffered = new PDO('duckdb::memory:');
$a = $buffered->query('SELECT i FROM range(5) t(i)')->fetchAll(PDO::FETCH_COLUMN);
$b = $db->query('SELECT i FROM range(5) t(i)')->fetchAll(PDO::FETCH_COLUMN);
var_dump($a === $b);

// nested types also reconstruct correctly under streaming
echo $db->query('SELECT [1, 2, 3] AS c')->fetchColumn(), "\n";

// DML reports rowCount() under streaming too (affected rows, like the buffered path)
$db->exec('CREATE TABLE t (i INTEGER)');
$ins = $db->prepare('INSERT INTO t VALUES (1), (2), (3)');
$ins->execute();
echo "unbuffered INSERT rowCount=", $ins->rowCount(), "\n";

// toggling the attribute off returns to buffered execution
$db->setAttribute(PDO::DUCKDB_ATTR_UNBUFFERED, false);
var_dump($db->getAttribute(PDO::DUCKDB_ATTR_UNBUFFERED));
echo $db->query('SELECT 42')->fetchColumn(), "\n";
?>
--EXPECT--
bool(true)
rows=50000 sum=1249975000
bool(true)
[1, 2, 3]
unbuffered INSERT rowCount=3
bool(false)
42
--CLEAN--
<?php
?>
