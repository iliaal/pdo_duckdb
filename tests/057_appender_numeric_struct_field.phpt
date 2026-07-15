--TEST--
pdo_duckdb: Appender accepts PHP integer keys for numeric STRUCT field names
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec('CREATE TABLE t (s STRUCT("0" INTEGER, "01" INTEGER, "-1" INTEGER))');

$appender = $db->duckdbAppender('t');
$appender->appendRow(['0' => 10, '01' => 20, '-1' => -10]);
$appender->flush();

$row = $db->query('SELECT s."0", s."01", s."-1" FROM t')->fetch(PDO::FETCH_NUM);
var_dump($row);
?>
--EXPECT--
array(3) {
  [0]=>
  int(10)
  [1]=>
  int(20)
  [2]=>
  int(-10)
}
