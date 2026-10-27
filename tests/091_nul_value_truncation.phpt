--TEST--
pdo_duckdb: embedded NUL survives nested rendering and top-level VARCHAR/BLOB binding
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);

// Top-level VARCHAR and BLOB are length-aware: NUL bytes round-trip exactly,
// via bind, and via a SQL literal.
$db->exec('CREATE TABLE t (v VARCHAR, b BLOB)');
$payload = "a\0b";
$ins = $db->prepare('INSERT INTO t VALUES (?, ?)');
$ins->execute([$payload, $payload]);
$row = $db->query('SELECT v, b FROM t')->fetch(PDO::FETCH_NUM);
var_dump($row[0] === $payload);
var_dump($row[1] === $payload);
var_dump($db->query("SELECT 'a' || chr(0) || 'b'")->fetchColumn() === $payload);
var_dump(strlen($row[0]));
var_dump(bin2hex($row[1]));

$db->exec('CREATE TABLE n (c VARCHAR[])');
$db->exec("INSERT INTO n VALUES (['a' || chr(0) || 'b'])");
$got = $db->query('SELECT c FROM n')->fetchColumn();
$want = $db->query('SELECT CAST(c AS VARCHAR) FROM n')->fetchColumn();
var_dump($got === "[a\0b]");
var_dump($got === $want);
echo 'engine_keeps_tail=', str_contains($want, 'b') ? "yes\n" : "no\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
int(3)
string(6) "610062"
bool(true)
bool(true)
engine_keeps_tail=yes
