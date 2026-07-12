--TEST--
pdo_duckdb: Appender scalar cast failure closes the invalidated native appender
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec('CREATE TABLE t (d DATE)');
$app = $db->duckdbAppender('t');

try {
    $app->appendRow('not-a-date');
    echo "BAD: malformed date accepted\n";
} catch (PDOException $e) {
    echo "cast failed\n";
}

try {
    $app->appendRow('2026-01-01');
    echo "BAD: invalidated appender reused\n";
} catch (Error $e) {
    echo str_contains($e->getMessage(), 'closed') ? "appender closed\n" : $e->getMessage(), "\n";
}
?>
--EXPECT--
cast failed
appender closed
