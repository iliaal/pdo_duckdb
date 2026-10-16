--TEST--
pdo_duckdb: duckdbLastProfile() returns the last query's profiling tree
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
require __DIR__ . '/duckdb_v2.inc';
$v2 = pdo_duckdb_v2_headers();
// On 8.4+ use PDO::connect() -> Pdo\Duckdb subclass, whose methods avoid the
// 8.5 deprecation of base-PDO driver methods. On 8.1-8.3 use new PDO().
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec("CREATE TABLE t AS SELECT range AS id FROM range(100)");

$db->query("SELECT count(*) FROM t")->fetchColumn();
var_dump($db->duckdbLastProfile());

$db->exec("PRAGMA enable_profiling='no_output'");
$db->query("SELECT count(*) FROM t WHERE id % 2 = 0")->fetchColumn();

$p = $db->duckdbLastProfile();
var_dump(is_array($p));
var_dump(array_keys($p));
var_dump(is_array($p['metrics']));
if ($v2) {
    // DuckDB 2.0 leaves the root metrics empty and moves the query text into
    // an unnamed category child.
    if ($p['metrics'] !== []) {
        echo "BAD: DuckDB 2.0 root metrics are not empty\n";
    }
    var_dump(pdo_duckdb_profile_group($p, 'sql')['metrics']['sql'] ?? null);
} else {
    var_dump($p['metrics']['QUERY_NAME']);
}
var_dump(count($p['children']) >= 1);
var_dump(array_keys($p['children'][0]));
if ($v2) {
    $ops = pdo_duckdb_profile_group($p, 'type');
    var_dump($ops !== null
        && $ops['metrics']['type'] === 'RESULT_COLLECTOR'
        && pdo_duckdb_profile_has_operator($ops, 'UNGROUPED_AGGREGATE')
        && pdo_duckdb_profile_has_operator($ops, 'TABLE_SCAN'));
} else {
    var_dump(isset($p['children'][0]['metrics']['OPERATOR_NAME']));
}
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
