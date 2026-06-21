--TEST--
pdo_duckdb: duckdbLastProfile() returns the last query's profiling tree
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec("CREATE TABLE t AS SELECT range AS id FROM range(100)");

// Without profiling enabled, null.
$db->query("SELECT count(*) FROM t")->fetchColumn();
var_dump($db->duckdbLastProfile());

// Enable profiling, run a query, read its profile.
$db->exec("PRAGMA enable_profiling='no_output'");
$db->query("SELECT count(*) FROM t WHERE id % 2 = 0")->fetchColumn();

$p = $db->duckdbLastProfile();
var_dump(is_array($p));
var_dump(array_keys($p));
var_dump(is_array($p['metrics']));
var_dump($p['metrics']['QUERY_NAME']);
// the tree has at least one operator child, each itself a node
var_dump(count($p['children']) >= 1);
var_dump(array_keys($p['children'][0]));
var_dump(isset($p['children'][0]['metrics']['OPERATOR_NAME']));
?>
--EXPECT--
NULL
bool(true)
array(2) {
  [0]=>
  string(7) "metrics"
  [1]=>
  string(8) "children"
}
bool(true)
string(39) "SELECT count(*) FROM t WHERE id % 2 = 0"
bool(true)
array(2) {
  [0]=>
  string(7) "metrics"
  [1]=>
  string(8) "children"
}
bool(true)
--CLEAN--
<?php
?>
