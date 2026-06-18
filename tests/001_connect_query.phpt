--TEST--
pdo_duckdb: connect in-memory, create/insert/select with bound params
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

$db->exec('CREATE TABLE t (id INTEGER, name VARCHAR, score DOUBLE)');

$stmt = $db->prepare('INSERT INTO t VALUES (?, ?, ?)');
$stmt->execute([1, 'alice', 9.5]);
$stmt->execute([2, 'bob', 7.25]);

$sel = $db->query('SELECT id, name, score FROM t ORDER BY id');
foreach ($sel->fetchAll(PDO::FETCH_ASSOC) as $row) {
    var_dump($row);
}

$p = $db->prepare('SELECT name FROM t WHERE id = ?');
$p->execute([2]);
var_dump($p->fetchColumn());
?>
--EXPECT--
array(3) {
  ["id"]=>
  int(1)
  ["name"]=>
  string(5) "alice"
  ["score"]=>
  float(9.5)
}
array(3) {
  ["id"]=>
  int(2)
  ["name"]=>
  string(3) "bob"
  ["score"]=>
  float(7.25)
}
string(3) "bob"
