--TEST--
pdo_duckdb: unbuffered pending failures release native state and allow reuse
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
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

$invalid = $db->prepare('SELECT CAST(? AS INTEGER)');
try {
    $invalid->execute(['not-an-integer']);
    echo "BAD: invalid cast accepted\n";
} catch (PDOException $e) {
    echo str_contains($e->getMessage(), 'Conversion Error')
        ? "pending execution failed\n"
        : "BAD: unexpected execution error\n";
}
$invalid->execute(['8']);
echo $invalid->fetchColumn(), "\n";

echo $db->query('SELECT 42')->fetchColumn(), "\n";
?>
--EXPECT--
pending creation failed
7
pending execution failed
8
42
