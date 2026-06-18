--TEST--
pdo_duckdb: DuckDB extensions load via plain SQL (no special API needed)
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

// Bundled extensions load through standard exec() -- no driver-specific method.
$db->exec('LOAD json');

var_dump((bool) $db->query("SELECT loaded FROM duckdb_extensions() WHERE extension_name = 'json'")->fetchColumn());
var_dump((int) $db->query("SELECT json_valid('{\"a\":1}')")->fetchColumn());
var_dump($db->query("SELECT json_extract('{\"a\":42}', '\$.a')")->fetchColumn());
?>
--EXPECT--
bool(true)
int(1)
string(2) "42"
--CLEAN--
<?php
?>
