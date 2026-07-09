--TEST--
pdo_duckdb: persistent reuse resets unbuffered and refuses array config
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$dsn = 'duckdb::memory:';

$db = new PDO($dsn, null, null, [
    PDO::ATTR_PERSISTENT => true,
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    PDO::DUCKDB_ATTR_UNBUFFERED => true,
]);
var_dump($db->getAttribute(PDO::DUCKDB_ATTR_UNBUFFERED));
unset($db);

$db2 = new PDO($dsn, null, null, [
    PDO::ATTR_PERSISTENT => true,
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
]);
var_dump($db2->getAttribute(PDO::DUCKDB_ATTR_UNBUFFERED));
unset($db2);

try {
    new PDO($dsn, null, null, [
        PDO::ATTR_PERSISTENT => true,
        PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
        PDO::DUCKDB_ATTR_CONFIG => ['memory_limit' => '64MB'],
    ]);
    echo "BAD: persistent CONFIG accepted\n";
} catch (PDOException $e) {
    var_dump(str_contains($e->getMessage(), 'DUCKDB_ATTR_CONFIG'));
}

$db3 = new PDO('duckdb::memory:', null, null, [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    PDO::DUCKDB_ATTR_CONFIG => ['memory_limit' => '64MB'],
]);
try {
    $db3->setAttribute(PDO::DUCKDB_ATTR_CONFIG, ['memory_limit' => '128MB']);
    echo "BAD: runtime CONFIG accepted\n";
} catch (PDOException $e) {
    var_dump(str_contains($e->getMessage(), 'DUCKDB_ATTR_CONFIG'));
}
?>
--EXPECT--
bool(true)
bool(false)
bool(true)
bool(true)
