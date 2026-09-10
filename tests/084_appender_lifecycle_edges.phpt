--TEST--
pdo_duckdb: Appender flush/close edge lifecycle and column-name validation
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:') : new PDO('duckdb::memory:');
$db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$db->exec('CREATE TABLE t (id INTEGER, v VARCHAR)');

$app = $db->duckdbAppender('t');
$app->flush();
echo "empty_flush_ok\n";

$app->appendRow(1, 'a');
$app->flush();
echo 'rows=', $db->query('SELECT count(*) FROM t')->fetchColumn(), "\n";

$app->close();
try {
    $app->close();
    echo "BAD: double close accepted\n";
} catch (Error $e) {
    echo str_contains($e->getMessage(), 'closed') ? "double_close_refused\n" : ('other=' . $e->getMessage() . "\n");
}

try {
    $app->flush();
    echo "BAD: flush after close accepted\n";
} catch (Error $e) {
    echo str_contains($e->getMessage(), 'closed') ? "flush_after_close_refused\n" : ('other=' . $e->getMessage() . "\n");
}

unset($app);
gc_collect_cycles();
echo "close_then_gc_silent\n";

try {
    $db->duckdbAppender('t', null, ['id', "v\0bad"]);
    echo "BAD: NUL column name accepted\n";
} catch (ValueError $e) {
    echo str_contains($e->getMessage(), 'NUL byte') ? "nul_column_rejected\n" : ('other=' . $e->getMessage() . "\n");
}

echo 'count=', $db->query('SELECT count(*) FROM t')->fetchColumn(), "\n";
?>
--EXPECT--
empty_flush_ok
rows=1
double_close_refused
flush_after_close_refused
close_then_gc_silent
nul_column_rejected
count=1
