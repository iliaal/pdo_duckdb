--TEST--
pdo_duckdb: request shutdown frees an open appender and its PDO safely in either order
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
if (!function_exists('proc_open')) die('skip proc_open() unavailable');
?>
--FILE--
<?php
// Request shutdown frees objects by descending handle (or via the shutdown
// GC), ignoring the appender's reference to its PDO object. Each scenario runs
// in a child process; the parent then reopens the database file to see which
// rows the child's teardown committed.
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
    // A fatal error skips destructors, so buffered rows are discarded.
    ini_set('memory_limit', '16M');
    $big = str_repeat('x', 64 * 1024 * 1024);
}
echo "end of script\n";
PHP;

$dir = __DIR__ . '/102_work';
@mkdir($dir);
$script = $dir . '/child.php';
file_put_contents($script, $child);

$ext_dir = ini_get('extension_dir');
// display_startup_errors=0 hides "already loaded" when pdo is static but a
// pdo.so also sits in extension_dir.
$cmd = [PHP_BINARY, '-n', '-d', 'extension_dir=' . $ext_dir,
    '-d', 'display_startup_errors=0', '-d', 'log_errors=0'];
foreach (['pdo', 'pdo_duckdb'] as $ext) {
    if (is_file($ext_dir . '/' . $ext . '.' . PHP_SHLIB_SUFFIX)) {
        $cmd[] = '-d';
        $cmd[] = 'extension=' . $ext;
    }
}

// Without ZendMM, only the tracked allocator enforces memory_limit.
$env = getenv();
if (($env['USE_ZEND_ALLOC'] ?? '') === '0') {
    $env['USE_TRACKED_ALLOC'] = '1';
}

foreach (['store', 'gc', 'fatal'] as $mode) {
    foreach (['0', '1'] as $txn) {
        $path = $dir . "/{$mode}_{$txn}.duckdb";
        @unlink($path);
        $proc = proc_open(array_merge($cmd, [$script, $path, $mode, $txn]),
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
mode=fatal txn=0 rows=0
mode=fatal txn=1 rows=0
done
