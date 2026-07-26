--TEST--
pdo_duckdb: CONFIG bool maps to true/false; PARAM_BOOL bind; UNBUFFERED "0"
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
// false must become DuckDB "false", not empty string.
$db = new PDO('duckdb::memory:', null, null, [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    PDO::DUCKDB_ATTR_CONFIG => [
        'preserve_insertion_order' => false,
        'enable_object_cache' => true,
    ],
]);
echo 'preserve_order=', $db->query("SELECT current_setting('preserve_insertion_order')::VARCHAR")->fetchColumn(), "\n";
echo 'object_cache=', $db->query("SELECT current_setting('enable_object_cache')::VARCHAR")->fetchColumn(), "\n";

// PARAM_BOOL
$st = $db->prepare('SELECT ?::BOOLEAN AS b');
$st->bindValue(1, true, PDO::PARAM_BOOL);
$st->execute();
var_dump($st->fetchColumn());
$st->bindValue(1, false, PDO::PARAM_BOOL);
$st->execute();
var_dump($st->fetchColumn());

// String "0" must not enable unbuffered (zend_is_true trap).
$db->setAttribute(PDO::DUCKDB_ATTR_UNBUFFERED, '0');
echo 'unbuf0=', $db->getAttribute(PDO::DUCKDB_ATTR_UNBUFFERED) ? 'on' : 'off', "\n";
$db->setAttribute(PDO::DUCKDB_ATTR_UNBUFFERED, '1');
echo 'unbuf1=', $db->getAttribute(PDO::DUCKDB_ATTR_UNBUFFERED) ? 'on' : 'off', "\n";
?>
--EXPECT--
preserve_order=false
object_cache=true
int(1)
int(0)
unbuf0=off
unbuf1=on
