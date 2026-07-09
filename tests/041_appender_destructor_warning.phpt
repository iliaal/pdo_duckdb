--TEST--
pdo_duckdb: Appender destruction warns when implicit close fails
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
function connect() {
    return PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
}

$db = connect();
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec('CREATE TABLE t (id INTEGER PRIMARY KEY, v VARCHAR)');

$ap = $db->duckdbAppender('t');
$ap->appendRow(1, 'a');
$ap->appendRow(1, 'dup');
unset($ap);
gc_collect_cycles();
echo "after unset\n";
?>
--EXPECTF--
Warning: %sPdo\Duckdb\Appender close during destruction failed: %sPRIMARY KEY%s
after unset
