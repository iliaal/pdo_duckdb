--TEST--
pdo_duckdb: named placeholders, including a repeated name
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

$db->exec("CREATE TABLE t (id INTEGER, name VARCHAR)");
$ins = $db->prepare("INSERT INTO t VALUES (:id, :name)");
$ins->execute([':id' => 1, ':name' => 'alice']);
$ins->execute([':id' => 2, ':name' => 'bob']);

// distinct named params
$q = $db->prepare("SELECT name FROM t WHERE id = :id");
$q->execute([':id' => 2]);
var_dump($q->fetchColumn());

// a repeated named placeholder used in two comparison positions: PDO coalesces
// ":v" to a single $1, bound once, applied in both spots.
$db->exec("CREATE TABLE pair (a INTEGER, b INTEGER)");
$db->exec("INSERT INTO pair VALUES (3, 9), (9, 7), (1, 2)");
$r = $db->prepare("SELECT a, b FROM pair WHERE a = :v OR b = :v ORDER BY a");
$r->execute([':v' => 9]);
foreach ($r->fetchAll(PDO::FETCH_ASSOC) as $row) {
    printf("a=%d b=%d\n", $row['a'], $row['b']);
}
?>
--EXPECT--
string(3) "bob"
a=3 b=9
a=9 b=7
