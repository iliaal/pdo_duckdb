--TEST--
pdo_duckdb: Appender::appendRow validates the whole row up front and isn't poisoned by a bad row
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
function connect() {
    return PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
}

$db = connect();
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec('CREATE TABLE t (a INTEGER, b VARCHAR)');
$ap = $db->duckdbAppender('t');

// Wrong column count is rejected before any native append (no partial row).
try {
    $ap->appendRow(1);
    echo "BAD: wrong column count accepted\n";
} catch (\ValueError $e) {
    var_dump(str_contains($e->getMessage(), 'column'));
}

// Unsupported value type is rejected before any native append. (A PHP array is
// no longer "unsupported" — it builds a nested value — so use an object, which
// has no DuckDB mapping.)
try {
    $ap->appendRow(1, new stdClass);
    echo "BAD: object value accepted\n";
} catch (\TypeError $e) {
    var_dump(str_contains($e->getMessage(), 'unsupported type'));
}

// The appender is NOT poisoned by the two rejected rows: valid rows still work.
$ap->appendRow(1, 'ok');
$ap->appendRow(2, 'two');
$ap->close();

var_dump((int) $db->query('SELECT count(*) FROM t')->fetchColumn());
foreach ($db->query('SELECT a, b FROM t ORDER BY a') as $r) {
    echo "{$r['a']}={$r['b']}\n";
}
?>
--EXPECT--
bool(true)
bool(true)
int(2)
1=ok
2=two
