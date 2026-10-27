--TEST--
pdo_duckdb: SQL NULL containers remain null in canonical, native, stringified, and bound fetches
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$types = [
    'INTEGER[]', 'VARCHAR[2]', 'STRUCT(a INTEGER)',
    'MAP(INTEGER, VARCHAR)', 'UNION(i INTEGER, s VARCHAR)', 'JSON', 'VARIANT',
];
$db = new PDO('duckdb::memory:');
foreach ([false, true] as $native) {
    $db->setAttribute(PDO::DUCKDB_ATTR_NATIVE_STRUCTURES, $native);
    foreach ([false, true] as $stringify) {
        $db->setAttribute(PDO::ATTR_STRINGIFY_FETCHES, $stringify);
        foreach ($types as $type) {
            $stmt = $db->prepare("SELECT NULL::$type AS v UNION ALL SELECT NULL::$type");
            for ($execution = 0; $execution < 2; $execution++) {
                $stmt->execute();
                if ($stmt->fetchAll(PDO::FETCH_COLUMN) !== [null, null]) {
                    throw new Exception("SQL NULL changed: $type / $native / $stringify");
                }
            }
            $bound = 'unchanged';
            $stmt->bindColumn(1, $bound, PDO::PARAM_STR);
            $stmt->execute();
            if (!$stmt->fetch(PDO::FETCH_BOUND) || $bound !== null) {
                throw new Exception("bound SQL NULL changed: $type / $native / $stringify");
            }
            $stmt->closeCursor();
        }
    }
}
$db->setAttribute(PDO::DUCKDB_ATTR_NATIVE_STRUCTURES, false);
$db->setAttribute(PDO::ATTR_STRINGIFY_FETCHES, false);
if ($db->query("SELECT 'null'::JSON")->fetchColumn() !== 'null') {
    throw new Exception('JSON null became SQL NULL');
}
$db->setAttribute(PDO::DUCKDB_ATTR_NATIVE_STRUCTURES, true);
$value = $db->query("SELECT 'null'::JSON")->fetchColumn();
if (!$value instanceof stdClass || get_object_vars($value) !== ['type'=>'JSON', 'value'=>null]) {
    throw new Exception('native JSON null became SQL NULL');
}
echo "SQL NULL preserved across all fetch paths\n";
?>
--EXPECT--
SQL NULL preserved across all fetch paths
