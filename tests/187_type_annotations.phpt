--TEST--
pdo_duckdb: retained collations use logical ordinal paths for every container
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$cases = [
    ["'Ab' COLLATE nocase", [[[], 'nocase']]],
    ["['Ab' COLLATE noaccent]", [[[0], 'noaccent']]],
    ["array_value('Ab' COLLATE nocase)", [[[0], 'nocase']]],
    ["struct_pack(plain := 1, \"a.b\" := ['Ab' COLLATE nocase], \"0\" := 'Cd' COLLATE noaccent)",
        [[[1, 0], 'nocase'], [[2], 'noaccent']]],
    ["row(1, 'Ab' COLLATE nocase)", [[[1], 'nocase']]],
    ["map(['Ab' COLLATE nocase], ['Cd' COLLATE noaccent])", [[[0], 'nocase'], [[1], 'noaccent']]],
    ["union_value(word := ['Ab' COLLATE nocase])", [[[0, 0], 'nocase']]],
    ["struct_insert(from_json('{\"a\\u0000b\":1}', '{\"a\\u0000b\":\"INTEGER\"}'), word := 'Ab' COLLATE nocase)",
        [[[1], 'nocase']]],
    ["struct_pack(plain := [1, 2], text := 'Ab')", []],
    ["NULL::VARCHAR", []],
];
foreach ($cases as [$sql, $expected]) {
    $expected = array_map(static fn($entry) => ['path' => $entry[0], 'collation' => $entry[1]], $expected);
    foreach ([false, true] as $rows) {
        $st = $db->query('SELECT ' . $sql . ' AS value WHERE ' . ($rows ? 'true' : 'false'));
        $meta = $st->getColumnMeta(0);
        if ($meta['duckdb:type_annotations'] !== $expected) {
            throw new Exception($sql . ': ' . var_export($meta, true));
        }
        if (str_contains($meta['duckdb:decl_type'], 'COLLATE')) {
            throw new Exception('Collation was inserted into a SQL type declaration');
        }
        if (!$rows && $st->fetch() !== false) {
            throw new Exception('Empty annotation test returned a row');
        }
        $st->closeCursor();
    }
}
echo "PASS: scalar, list, array, struct, tuple, map, union, and ordinal paths\n";
?>
--EXPECT--
PASS: scalar, list, array, struct, tuple, map, union, and ordinal paths
