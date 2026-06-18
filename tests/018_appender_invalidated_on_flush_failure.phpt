--TEST--
pdo_duckdb: Appender is invalidated after a failed flush()/close() (DuckDB contract)
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
$db->exec('CREATE TABLE t (id INTEGER PRIMARY KEY, v VARCHAR)');

$ap = $db->duckdbAppender('t');
$ap->appendRow(1, 'a');
$ap->appendRow(1, 'dup'); // duplicate primary key — the error surfaces at flush

try {
    $ap->flush();
    echo "BAD: flush succeeded\n";
} catch (\PDOException $e) {
    echo "flush failed\n";
}

// DuckDB invalidates the appender on a failed flush; further use must be refused
// rather than silently working against a poisoned native appender.
try {
    $ap->appendRow(2, 'after');
    echo "BAD: appendRow accepted on an invalidated appender\n";
} catch (\Error $e) {
    var_dump(str_contains($e->getMessage(), 'closed'));
}
?>
--EXPECT--
flush failed
bool(true)
