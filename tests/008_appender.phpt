--TEST--
pdo_duckdb: Appender bulk insert (duckdbAppender)
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
// On 8.4+ use PDO::connect() -> Pdo\Duckdb subclass, whose duckdbAppender()
// avoids the 8.5 deprecation of base-PDO driver methods. On 8.3 use new PDO().
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec('CREATE TABLE t (id INTEGER, name VARCHAR, score DOUBLE, ok BOOLEAN)');

$app = $db->duckdbAppender('t');
var_dump($app instanceof Pdo\Duckdb\Appender);

// chaining + mixed types incl. NULL
$app->appendRow(1, 'alice', 9.5, true)
    ->appendRow(2, 'bob', 7.25, false)
    ->appendRow(3, null, null, null);
$app->flush();

foreach ($db->query('SELECT id, name, score, ok FROM t ORDER BY id') as $r) {
    printf("%d|%s|%s|%s\n", $r['id'], $r['name'] ?? 'NULL', $r['score'] ?? 'NULL', $r['ok'] ?? 'NULL');
}

// close() then use -> Error
$app->close();
try {
    $app->appendRow(4, 'x', 1.0, true);
} catch (\Error $e) {
    echo "after close: ", $e->getMessage(), "\n";
}

// appender on a missing table -> PDOException
try {
    $db->duckdbAppender('nope_missing');
} catch (\PDOException $e) {
    echo "missing table: caught PDOException\n";
}

// unsupported value type -> TypeError
$app2 = $db->duckdbAppender('t');
try {
    $app2->appendRow(5, [1, 2], 1.0, true);
} catch (\TypeError $e) {
    echo "bad type: caught TypeError\n";
}
?>
--EXPECT--
bool(true)
1|alice|9.5|1
2|bob|7.25|0
3|NULL|NULL|NULL
after close: Pdo\Duckdb\Appender is closed
missing table: caught PDOException
bad type: caught TypeError
--CLEAN--
<?php
?>
