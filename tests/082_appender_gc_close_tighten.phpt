--TEST--
pdo_duckdb: appender GC close after open_basedir tighten stays sandboxed and silent
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
// The destructor close flushes buffered rows into the table, but it applies
// the sandbox first: in-memory rows still land, file access stays denied,
// and a clean close emits no destruction warning.
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec('CREATE TABLE t (id INTEGER)');
$app = $db->duckdbAppender('t');
$app->appendRow(1);

ini_set('open_basedir', __DIR__);

unset($app);
gc_collect_cycles();
echo "gc_close_silent\n";

echo 'rows=', $db->query('SELECT count(*) FROM t')->fetchColumn(), "\n";

try {
    $db->query("SELECT * FROM read_csv('/etc/hostname')")->fetchAll();
    echo "BAD: external read after GC close\n";
} catch (PDOException $e) {
    echo str_contains($e->getMessage(), 'disabled by configuration') ? "external_blocked\n" : ('other=' . $e->getMessage() . "\n");
}
?>
--EXPECT--
gc_close_silent
rows=1
external_blocked
