--TEST--
pdo_duckdb: Appender into BLOB column round-trips binary data
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec('CREATE TABLE b (id INTEGER, payload BLOB, label VARCHAR)');

$bin = "\x00\x01\x02\xfe\xff\x00binary";
$app = $db->duckdbAppender('b');
$app->appendRow(1, $bin, 'text-here');   // BLOB col gets raw bytes, VARCHAR col text
$app->appendRow(2, '', 'empty-blob');
$app->flush();

$row = $db->query('SELECT payload, label FROM b WHERE id = 1')->fetch(PDO::FETCH_ASSOC);
var_dump($row['payload'] === $bin);
var_dump(bin2hex($row['payload']));
var_dump($row['label']);

$empty = $db->query('SELECT payload FROM b WHERE id = 2')->fetchColumn();
var_dump($empty);
?>
--EXPECT--
bool(true)
string(24) "000102feff0062696e617279"
string(9) "text-here"
string(0) ""
--CLEAN--
<?php
?>
