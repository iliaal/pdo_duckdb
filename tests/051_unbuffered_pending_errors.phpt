--TEST--
pdo_duckdb: unbuffered pending failures release native state and allow reuse
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
require __DIR__ . '/duckdb_v2.inc';
$db = new PDO('duckdb::memory:', null, null, [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    PDO::DUCKDB_ATTR_UNBUFFERED => true,
]);

$missing = $db->prepare('SELECT CAST(? AS INTEGER)');
try {
    $missing->execute();
    echo "BAD: missing parameter accepted\n";
} catch (PDOException $e) {
    echo str_contains($e->getMessage(), 'Values were not provided')
        ? "pending creation failed\n"
        : "BAD: unexpected creation error\n";
}
$missing->execute([7]);
echo $missing->fetchColumn(), "\n";
$missing->closeCursor();

$invalid = $db->prepare('SELECT CAST(? AS INTEGER)');
if (pdo_duckdb_v2_headers()) {
    // DuckDB 2.0 reports the cast failure from the first streamed chunk, not
    // from the pending execution.
    try {
        $invalid->execute(['not-an-integer']);
    } catch (PDOException $e) {
        echo "BAD: DuckDB 2.0 rejected the cast at execute: ", $e->getMessage(), "\n";
    }
    try {
        $invalid->fetchColumn();
        echo "BAD: invalid cast accepted\n";
    } catch (PDOException $e) {
        echo str_contains($e->getMessage(), 'Conversion Error')
            ? "invalid cast rejected\n"
            : "BAD: unexpected fetch error\n";
    }
} else {
    try {
        $invalid->execute(['not-an-integer']);
        echo "BAD: invalid cast accepted\n";
    } catch (PDOException $e) {
        echo str_contains($e->getMessage(), 'Conversion Error')
            ? "invalid cast rejected\n"
            : "BAD: unexpected execution error\n";
    }
}
$invalid->execute(['8']);
echo $invalid->fetchColumn(), "\n";
$invalid->closeCursor();

echo $db->query('SELECT 42')->fetchColumn(), "\n";
?>
--EXPECT--
pending creation failed
7
invalid cast rejected
8
42
