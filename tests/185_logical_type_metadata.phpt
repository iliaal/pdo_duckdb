--TEST--
pdo_duckdb: complete logical declarations are additive for empty and populated results
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
require __DIR__ . '/duckdb_v2.inc';
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec("CREATE TYPE mood AS ENUM ('happy', 'sad'); CREATE TYPE price AS DECIMAL(18,2)");
$cases = [
    ['NULL::DECIMAL(38,12)', 'DECIMAL(38,12)', 'DECIMAL', PDO::PARAM_STR, 38, 12],
    ['NULL::STRUCT("a b" DECIMAL(20,3), items VARCHAR[2][])', 'STRUCT("a b" DECIMAL(20,3), items VARCHAR[2][])', 'STRUCT', PDO::PARAM_STR, 0, null],
    ['NULL::MAP(INTEGER, STRUCT(v DECIMAL(8,2)))', 'MAP(INTEGER, STRUCT(v DECIMAL(8,2)))', 'MAP', PDO::PARAM_STR, 0, null],
    ['NULL::UNION(i INTEGER, s VARCHAR)', 'UNION(i INTEGER, s VARCHAR)', 'UNION', PDO::PARAM_STR, 0, null],
    ['NULL::INTEGER[2][3]', 'INTEGER[2][3]', 'ARRAY', PDO::PARAM_STR, 0, null],
    ["(1, 'x')", (pdo_duckdb_v2_headers() ? 'TUPLE' : 'STRUCT') . '(INTEGER, VARCHAR)', 'STRUCT', PDO::PARAM_STR, 0, null],
    ["'[]'::JSON", 'JSON', 'VARCHAR', PDO::PARAM_STR, 0, null],
    ["'happy'::mood", "ENUM('happy', 'sad')", 'ENUM', PDO::PARAM_STR, 0, null],
    ['1::price', 'DECIMAL(18,2)', 'DECIMAL', PDO::PARAM_STR, 18, 2],
    ['true', 'BOOLEAN', 'BOOLEAN', PDO::PARAM_INT, 0, null],
    ['1::UBIGINT', 'UBIGINT', 'UBIGINT', PDO::PARAM_STR, 0, null],
    ["from_hex('0061ff')", 'BLOB', 'BLOB', PDO::PARAM_LOB, 0, null],
];
foreach ([false, true] as $stringify) {
    $db->setAttribute(PDO::ATTR_STRINGIFY_FETCHES, $stringify);
    foreach ([false, true] as $rows) {
        foreach ($cases as [$sql, $declaration, $native, $pdo, $precision, $scale]) {
            $statement = $db->query('SELECT ' . $sql . ' AS value WHERE ' . ($rows ? 'true' : 'false'));
            $meta = $statement->getColumnMeta(0);
            if ($meta['duckdb:decl_type'] !== $declaration || $meta['native_type'] !== $native
                    || $meta['pdo_type'] !== $pdo || $meta['precision'] !== $precision
                    || ($meta['scale'] ?? null) !== $scale || $meta['name'] !== 'value') {
                throw new Exception('Metadata mismatch for ' . $declaration . ': ' . var_export($meta, true));
            }
            if (!$rows && $statement->fetch() !== false) {
                throw new Exception('Empty result unexpectedly had a row');
            }
            $statement->closeCursor();
        }
    }
}
echo "PASS: declarations, aliases, dimensions, tuples, and unchanged scalar metadata\n";
?>
--EXPECT--
PASS: declarations, aliases, dimensions, tuples, and unchanged scalar metadata
