--TEST--
pdo_duckdb: ENUM labels retain binary lengths with cached and interleaved rendering
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$db->exec("CREATE TYPE binary_labels AS ENUM (SELECT v FROM (VALUES
    ('a'||chr(0)||'b'), ('a'||chr(0)||'c'), (chr(0)), (''), ('plain')) t(v))");
$labels = ["a\0b", "a\0c", "\0", '', 'plain', null];
foreach ([false, true] as $native) {
    $db->setAttribute(PDO::DUCKDB_ATTR_NATIVE_STRUCTURES, $native);
    foreach ([false, true] as $stringify) {
        $db->setAttribute(PDO::ATTR_STRINGIFY_FETCHES, $stringify);
        foreach ([false, true] as $interleave) {
            $stmt = $db->prepare('SELECT v, CAST(v AS VARCHAR) AS wanted, [v] AS nested
                FROM (SELECT CAST(? AS binary_labels) AS v)');
            foreach ($labels as $label) {
                $stmt->execute([$label]);
                if ($interleave) {
                    $db->query('SELECT 1')->fetchColumn();
                }
                $row = $stmt->fetch(PDO::FETCH_ASSOC);
                if ($row['v'] !== $label || $row['wanted'] !== $label) {
                    throw new Exception('ENUM label changed: ' . bin2hex((string)$label));
                }
                if ($native && !$stringify) {
                    if (!$row['nested'] instanceof stdClass || $row['nested']->value !== [$label]) {
                        throw new Exception('nested ENUM label changed');
                    }
                } else {
                    $expected = $db->prepare('SELECT CAST([CAST(? AS binary_labels)] AS VARCHAR)');
                    $expected->execute([$label]);
                    if ($row['nested'] !== $expected->fetchColumn()) {
                        throw new Exception('canonical nested ENUM changed');
                    }
                }
            }
            $stmt->closeCursor();
        }
    }
}
echo "binary ENUM labels and SQL NULL preserved\n";
?>
--EXPECT--
binary ENUM labels and SQL NULL preserved
