--TEST--
pdo_duckdb: Appender scalar cast failure is rejected up front without poisoning
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec('CREATE TABLE t (d DATE)');
$app = $db->duckdbAppender('t');

// A bad scalar is caught by the pre-append CAST probe, before any native
// append touches the row: the appender stays usable (like any other
// whole-row validation rejection), and the failed row leaves nothing behind.
try {
    $app->appendRow('not-a-date');
    echo "BAD: malformed date accepted\n";
} catch (PDOException $e) {
    echo 'probe_msg=', str_starts_with($e->getMessage(), 'Failed to append value:') ? "yes\n" : ('no:' . $e->getMessage() . "\n");
}

$app->appendRow('2026-01-01');
$app->flush();
echo 'rows=', $db->query('SELECT count(*) FROM t')->fetchColumn(), "\n";
echo 'val=', $db->query('SELECT d FROM t')->fetchColumn(), "\n";
?>
--EXPECT--
probe_msg=yes
rows=1
val=2026-01-01
