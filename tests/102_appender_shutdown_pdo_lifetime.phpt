--TEST--
pdo_duckdb: request shutdown frees an open appender and its PDO safely in either order
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
if (!function_exists('proc_open')) die('skip proc_open() unavailable');
if (getenv('TEST_PHP_EXTRA_ARGS') === false) die('skip needs run-tests.php child arguments');
?>
--FILE--
<?php
// Request shutdown frees objects by descending handle (or via the shutdown
// GC), ignoring the appender's reference to its PDO object, so here the PDO
// object is freed first. Each scenario runs in a child process; the parent
// then reopens the database file to see which rows the teardown committed.
//
// Expected rows per scenario:
// - txn=0: an unclosed appender flushes at teardown in autocommit, as it does
//   when the appender is freed first, so both rows land (2). This includes
//   mode=fatal, where PHP skips destructors after the fatal error.
// - txn=1: PDO rolls back the open transaction when its object is freed; the
//   appender's buffered rows belong to that transaction and are discarded (0),
//   matching the order where the appender flushes into the transaction first.
$child = <<<'PHP'
<?php
[, $path, $mode, $txn] = $argv;
if ($mode === 'store') {
    gc_disable();
}
$placeholder = new stdClass;
$db = new PDO('duckdb:' . $path, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('CREATE TABLE t (i INTEGER)');
// The appender reuses the lower handle, so the object-store sweep reaches
// the PDO object first.
unset($placeholder);
if ($txn === '1') {
    $db->beginTransaction();
}
$ap = $db->duckdbAppender('t');
$ap->appendRow(1);
$ap->appendRow(2);
if (spl_object_id($ap) >= spl_object_id($db)) {
    echo "BAD handle order\n";
}
$holder = new stdClass;
$holder->self = $holder;
$holder->db = $db;
$holder->ap = $ap;
unset($ap, $db);
if ($mode === 'gc') {
    unset($holder);
}
if ($mode === 'fatal') {
    // A fatal error skips destructors; teardown still flushes or discards.
    ini_set('memory_limit', '16M');
    $big = str_repeat('x', 64 * 1024 * 1024);
}
echo "end of script\n";
PHP;

$dir = __DIR__ . '/102_work';
@mkdir($dir);
$script = $dir . '/child.php';
file_put_contents($script, $child);

// Children get the runner's own -n/-d arguments, which is how CI loads
// pdo_duckdb (by absolute path through TEST_PHP_ARGS).
$php = escapeshellarg(getenv('TEST_PHP_EXECUTABLE')) . ' ' . getenv('TEST_PHP_EXTRA_ARGS');

// Without ZendMM, only the tracked allocator enforces memory_limit.
$env = getenv();
if (($env['USE_ZEND_ALLOC'] ?? '') === '0') {
    $env['USE_TRACKED_ALLOC'] = '1';
}

foreach (['store', 'gc', 'fatal'] as $mode) {
    foreach (['0', '1'] as $txn) {
        $path = $dir . "/{$mode}_{$txn}.duckdb";
        @unlink($path);
        $proc = proc_open($php . ' ' . implode(' ', array_map('escapeshellarg', [$script, $path, $mode, $txn])),
            [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes, null, $env);
        fclose($pipes[0]);
        $out = stream_get_contents($pipes[1]);
        fclose($pipes[1]);
        $rc = proc_close($proc);
        $clean = $mode === 'fatal'
            ? $rc === 255 && str_contains($out, 'Allowed memory size')
            : $rc === 0 && $out === "end of script\n";
        if (!$clean) {
            echo "mode=$mode txn=$txn rc=$rc\n$out\n";
            continue;
        }
        $db = new PDO('duckdb:' . $path, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
        printf("mode=%s txn=%s rows=%d\n", $mode, $txn, $db->query('SELECT count(*) FROM t')->fetchColumn());
        $db = null;
    }
}
echo "done\n";
?>
--CLEAN--
<?php
$dir = __DIR__ . '/102_work';
foreach (glob($dir . '/*') ?: [] as $f) {
    @unlink($f);
}
@rmdir($dir);
?>
--EXPECT--
mode=store txn=0 rows=2
mode=store txn=1 rows=0
mode=gc txn=0 rows=2
mode=gc txn=1 rows=0
mode=fatal txn=0 rows=2
mode=fatal txn=1 rows=0
done
