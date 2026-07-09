--TEST--
pdo_duckdb: PDO::quote doubles quotes and rejects embedded NUL bytes
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

$quoted = $db->quote("O'Reilly");
var_dump($quoted);
var_dump($db->query("SELECT $quoted")->fetchColumn());

try {
    $db->quote("a\0b");
    echo "BAD: NUL accepted\n";
} catch (PDOException $e) {
    var_dump(str_contains($e->getMessage(), 'null bytes'));
}
?>
--EXPECT--
string(11) "'O''Reilly'"
string(8) "O'Reilly"
bool(true)
