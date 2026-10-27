--TEST--
pdo_duckdb: NUL column names remain distinct in metadata, associative fetches, and bindings
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
$db = new PDO('duckdb::memory:');
$expr = "from_json('{\"a\\u0000b\":1}', '{\"a\\u0000b\":\"INTEGER\"}')";
[$type, $json] = $db->query("SELECT typeof($expr), to_json($expr)")->fetch(PDO::FETCH_NUM);
if ($type === 'STRUCT(a INTEGER)' && $json === '{"a":null}') {
    die('skip DuckDB from_json truncates NUL field names before the driver receives them');
}
?>
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$sql = "SELECT unnest(from_json('{\"a\":1,\"a\\u0000b\":2}',
    '{\"a\":\"INTEGER\",\"a\\u0000b\":\"INTEGER\"}'))";
foreach ([false, true] as $streaming) {
    $db->setAttribute(PDO::DUCKDB_ATTR_UNBUFFERED, $streaming);
    foreach ([PDO::CASE_NATURAL, PDO::CASE_UPPER] as $case) {
        $db->setAttribute(PDO::ATTR_CASE, $case);
        $names = $case === PDO::CASE_NATURAL ? ['a', "a\0b"] : ['A', "A\0B"];
        $stmt = $db->prepare($sql);
        for ($execution = 0; $execution < 2; $execution++) {
            $stmt->execute();
            foreach ($names as $index => $name) {
                $meta = $stmt->getColumnMeta($index);
                if ($meta['name'] !== $name || $meta['duckdb:decl_type'] !== 'INTEGER') {
                    throw new Exception('column metadata lost a name');
                }
            }
            if ($stmt->fetch(PDO::FETCH_ASSOC) !== [$names[0]=>1, $names[1]=>2]) {
                throw new Exception('associative keys collided');
            }
            $stmt->closeCursor();
        }
        $stmt->execute();
        if ($stmt->fetch(PDO::FETCH_NAMED) !== [$names[0]=>1, $names[1]=>2]) {
            throw new Exception('distinct columns became duplicate names');
        }
        $stmt->closeCursor();
        $stmt->bindColumn($names[1], $bound, PDO::PARAM_INT);
        $stmt->execute();
        if (!$stmt->fetch(PDO::FETCH_BOUND) || $bound !== 2) {
            throw new Exception('bound name selected the wrong column');
        }
        $stmt->closeCursor();
        $empty = $db->query($sql . ' WHERE false');
        if ($empty->getColumnMeta(1)['name'] !== $names[1] || $empty->fetch() !== false) {
            throw new Exception('empty result lost column metadata');
        }
        $empty->closeCursor();
    }
}
echo "binary column names preserve both values\n";
?>
--EXPECT--
binary column names preserve both values
