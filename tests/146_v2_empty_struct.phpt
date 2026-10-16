--TEST--
pdo_duckdb: DuckDB 2.0 empty STRUCT values fetch like the engine renders them
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
require __DIR__ . '/duckdb_v2.inc';
if (!pdo_duckdb_v2_headers()) die('skip needs a build against the DuckDB 2.0 headers');
?>
--FILE--
<?php
// DuckDB 1.5 cannot build a zero-field STRUCT ({} is a parser error there).
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('CREATE TABLE e AS SELECT * FROM (VALUES (1, {}), (2, NULL)) t(id, s)');
// r has a BLOB sibling, which takes DuckDB's value renderer instead of the
// driver's direct nested renderer.
$st = $db->query("SELECT s, [s, s] AS l, MAP {'k': s} AS m, {'inner': s} AS n, {'e': s, 'b': 'x'::BLOB} AS r,
    CAST(s AS VARCHAR) AS ws, CAST([s, s] AS VARCHAR) AS wl, CAST(MAP {'k': s} AS VARCHAR) AS wm,
    CAST({'inner': s} AS VARCHAR) AS wn, CAST({'e': s, 'b': 'x'::BLOB} AS VARCHAR) AS wr FROM e ORDER BY id");
foreach (['s', 'l', 'm', 'n', 'r'] as $i => $name) {
    $m = $st->getColumnMeta($i);
    echo "$name: {$m['native_type']} pdo_type={$m['pdo_type']}\n";
}
foreach ($st->fetchAll(PDO::FETCH_ASSOC) as $row) {
    echo json_encode([$row['s'], $row['l'], $row['m'], $row['n'], $row['r']]), ' ',
        ($row['s'] === $row['ws'] && $row['l'] === $row['wl'] && $row['m'] === $row['wm'] && $row['n'] === $row['wn']
            && $row['r'] === $row['wr']
            ? 'matches engine' : 'MISMATCH ' . json_encode($row)), "\n";
}
$ap = $db->duckdbAppender('e');
$ap->appendRow(3, []);
try {
    $ap->appendRow(4, ['a' => 1]);
} catch (ValueError $e) {
    echo get_class($e), ': ', $e->getMessage(), "\n";
}
$ap->close();
var_dump($db->query('SELECT s FROM e WHERE id = 3')->fetchAll(PDO::FETCH_COLUMN));
?>
--EXPECT--
s: STRUCT pdo_type=2
l: LIST pdo_type=2
m: MAP pdo_type=2
n: STRUCT pdo_type=2
r: STRUCT pdo_type=2
["{}","[{}, {}]","{k={}}","{'inner': {}}","{'e': {}, 'b': x}"] matches engine
[null,"[NULL, NULL]","{k=NULL}","{'inner': NULL}","{'e': NULL, 'b': x}"] matches engine
ValueError: Pdo\Duckdb\Appender::appendRow(): argument #2 expects exactly 0 struct field(s), got 1
array(1) {
  [0]=>
  string(2) "{}"
}
