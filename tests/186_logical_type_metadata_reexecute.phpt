--TEST--
pdo_duckdb: logical metadata refreshes across empty results, changed binding types, and cursor reuse
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$statement = $db->prepare('SELECT CAST(? AS DECIMAL(38,9)) AS amount, CAST([?] AS INTEGER[1]) AS items WHERE CAST(? AS INTEGER) = 1');
foreach ([0, 1, 0, 1] as $rows) {
    $statement->execute(['123456789012345678901234.123456789', 7, $rows]);
    $decimal = $statement->getColumnMeta(0);
    $array = $statement->getColumnMeta(1);
    if ($decimal['duckdb:decl_type'] !== 'DECIMAL(38,9)' || $decimal['precision'] !== 38
            || $decimal['scale'] !== 9 || $array['duckdb:decl_type'] !== 'INTEGER[1]') {
        throw new Exception('Re-execution lost type parameters');
    }
    $result = $statement->fetch(PDO::FETCH_NUM);
    if (($rows === 0 && $result !== false)
            || ($rows === 1 && $result !== ['123456789012345678901234.123456789', '[7]'])) {
        throw new Exception('Metadata changed fetched values');
    }
    if ($statement->getColumnMeta(0) !== $decimal || $statement->getColumnMeta(1) !== $array) {
        throw new Exception('Fetching changed metadata');
    }
    $statement->closeCursor();
}
$statement = $db->prepare('SELECT ? AS value');
foreach ([[7, PDO::PARAM_INT, 'BIGINT'], ['word', PDO::PARAM_STR, 'VARCHAR'], [8, PDO::PARAM_INT, 'BIGINT']] as [$value, $binding, $type]) {
    $statement->bindValue(1, $value, $binding);
    $statement->execute();
    $meta = $statement->getColumnMeta(0);
    if ($meta['duckdb:decl_type'] !== $type || $meta['native_type'] !== $type) {
        throw new Exception('Stale binding type metadata');
    }
    if ($statement->fetchColumn() !== $value) {
        throw new Exception('Binding type test did not fetch its actual value');
    }
}
echo "PASS: empty results, exact values, changed types, and re-execution\n";
?>
--EXPECT--
PASS: empty results, exact values, changed types, and re-execution
