--TEST--
pdo_duckdb: annotation paths survive deep nesting, empty results, and cursor reuse
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$depth = 96;
$expression = str_repeat('[', $depth) . '(?::VARCHAR COLLATE nocase)' . str_repeat(']', $depth);
foreach ([false, true] as $unbuffered) {
    $db->setAttribute(PDO::DUCKDB_ATTR_UNBUFFERED, $unbuffered);
    $st = $db->prepare('SELECT ' . $expression . ' AS value WHERE ?::INTEGER = 1');
    foreach ([0, 1, 0, 1] as $rows) {
        $st->execute(['Ab', $rows]);
        $meta = $st->getColumnMeta(0);
        if ($meta['duckdb:type_annotations'] !== [['path' => array_fill(0, $depth, 0), 'collation' => 'nocase']]
                || $meta['duckdb:decl_type'] !== 'VARCHAR' . str_repeat('[]', $depth)
                || $meta['native_type'] !== 'LIST' || $meta['pdo_type'] !== PDO::PARAM_STR) {
            throw new Exception('Nested metadata mismatch');
        }
        $value = $st->fetchColumn();
        if (($rows === 0 && $value !== false) || ($rows === 1 && !is_string($value))) {
            throw new Exception('Wrong result shape');
        }
        if ($st->getColumnMeta(0) !== $meta) {
            throw new Exception('Fetching changed annotation metadata');
        }
        $st->closeCursor();
    }
}
echo "PASS: deep paths, empty results, buffered/unbuffered execution, and reuse\n";
?>
--EXPECT--
PASS: deep paths, empty results, buffered/unbuffered execution, and reuse
