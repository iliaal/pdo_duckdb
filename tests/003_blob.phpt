--TEST--
pdo_duckdb: BLOB round-trip with binary data incl. NUL bytes
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

$db->exec("CREATE TABLE b (id INTEGER, data BLOB)");

$payload = "\x00\x01\x02\xfe\xff\x00ABC";
$ins = $db->prepare("INSERT INTO b VALUES (?, ?)");
$ins->bindValue(1, 1, PDO::PARAM_INT);
$ins->bindValue(2, $payload, PDO::PARAM_LOB);
$ins->execute();

$got = $db->query("SELECT data FROM b WHERE id = 1")->fetchColumn();
var_dump($got === $payload);
var_dump(bin2hex($got));
var_dump(strlen($got));

// empty blob
$db->exec("INSERT INTO b VALUES (2, ''::BLOB)");
$empty = $db->query("SELECT data FROM b WHERE id = 2")->fetchColumn();
var_dump($empty);
?>
--EXPECT--
bool(true)
string(18) "000102feff00414243"
int(9)
string(0) ""
