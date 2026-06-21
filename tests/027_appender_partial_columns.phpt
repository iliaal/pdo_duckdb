--TEST--
pdo_duckdb: Appender with a column subset (omitted columns take their DEFAULT)
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec("CREATE SEQUENCE seq START 100");
$db->exec("CREATE TABLE t (
    id   BIGINT DEFAULT nextval('seq'),
    ts   TIMESTAMP DEFAULT TIMESTAMP '2000-01-01',
    name VARCHAR,
    note VARCHAR DEFAULT 'none'
)");

// append only 'name'; id/ts/note fall back to their defaults
$ap = $db->duckdbAppender('t', null, ['name']);
$ap->appendRow('alice')->appendRow('bob');
$ap->flush();
foreach ($db->query('SELECT id, ts, name, note FROM t ORDER BY id') as $r) {
    echo "{$r['id']}|{$r['ts']}|{$r['name']}|{$r['note']}\n";
}

// a subset given in a different order than the table definition
$db->exec('DELETE FROM t');
$db->duckdbAppender('t', null, ['note', 'name'])->appendRow('hi', 'carol')->flush();
$r = $db->query('SELECT name, note FROM t')->fetch(PDO::FETCH_ASSOC);
echo "reorder: {$r['name']}/{$r['note']}\n";

// arity is checked against the active subset, not the whole table
try {
    $db->duckdbAppender('t', null, ['name', 'note'])->appendRow('x');
} catch (\ValueError $e) {
    echo "arity: ValueError\n";
}

// error cases
foreach (['unknown' => ['nope'], 'empty' => [], 'non-string' => [123]] as $label => $cols) {
    try {
        $db->duckdbAppender('t', null, $cols);
        echo "$label: no error (BUG)\n";
    } catch (\Throwable $e) {
        echo "$label: ", get_class($e), "\n";
    }
}

// no subset -> every column still required (unchanged behavior)
$db->exec('DELETE FROM t');
$db->duckdbAppender('t')->appendRow(1, '2020-01-01 00:00:00', 'dave', 'x')->flush();
echo "full row: ", $db->query('SELECT count(*) FROM t')->fetchColumn(), "\n";
?>
--EXPECT--
100|2000-01-01 00:00:00|alice|none
101|2000-01-01 00:00:00|bob|none
reorder: carol/hi
arity: ValueError
unknown: PDOException
empty: ValueError
non-string: TypeError
full row: 1
--CLEAN--
<?php
?>
