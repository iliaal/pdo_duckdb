--TEST--
pdo_duckdb: unsupported scroll cursors are rejected during prepare
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);

try {
    $db->prepare('SELECT 1', [PDO::ATTR_CURSOR => PDO::CURSOR_SCROLL]);
    echo "scroll accepted (BUG)\n";
} catch (PDOException $e) {
    echo str_contains($e->getMessage(), 'forward-only') ? 'scroll rejected' : $e->getMessage(), "\n";
}

$stmt = $db->prepare('SELECT 1', [PDO::ATTR_CURSOR => PDO::CURSOR_FWDONLY]);
$stmt->execute();
var_dump($stmt->fetchColumn());
?>
--EXPECT--
scroll rejected
int(1)
