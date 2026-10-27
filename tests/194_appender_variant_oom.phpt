--TEST--
pdo_duckdb: Appender frees prior VARIANT cells and typed container children after a memory-limit bailout
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
if (!function_exists('proc_open')) die('skip proc_open() unavailable');
if (getenv('TEST_PHP_EXTRA_ARGS') === false) die('skip needs run-tests.php child arguments');
require __DIR__ . '/_bailout_shutdown.inc';
if (!pdo_duckdb_oom_runs_shutdown()) die('skip memory_limit exhaustion exits without shutdown functions');
$env = getenv();
$env['USE_ZEND_ALLOC'] = '0';
$env['USE_TRACKED_ALLOC'] = '1';
$p = proc_open([PHP_BINARY, '-n', '-r', 'ini_set("memory_limit", "4M"); str_repeat("x", 8 << 20);'],
    [1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes, null, $env);
$out = stream_get_contents($pipes[1]);
fclose($pipes[1]);
proc_close($p);
if (!str_contains($out, 'Allowed memory size')) die('skip tracked allocator does not enforce memory_limit');
?>
--FILE--
<?php
$child = <<<'PHP'
$mode = (int)$argv[1];
$reserve = str_repeat('r', 2 << 20);
$db = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb::memory:;threads=1') : new PDO('duckdb::memory:;threads=1');
$types = ['a VARIANT,b VARIANT', 'v VARIANT[]', 's STRUCT(a VARIANT,b VARIANT)', 'm MAP(VARCHAR,VARIANT)'];
$db->exec('CREATE TABLE t (' . $types[$mode] . ')');
$app = $db->duckdbAppender('t');
$first = (object)['payload' => str_repeat('x', 1 << 20)];
$second = array_fill(0, 20000, 42);
$row = match ($mode) {
    0 => [$first, $second],
    1 => [[$first, $second]],
    2 => [['a' => $first, 'b' => $second]],
    3 => [['a' => $first, 'b' => $second]],
};
register_shutdown_function(function () {
    global $reserve, $row, $first, $second, $app, $db, $mode;
    $reserve = null;
    $row = $first = $second = null;
    ini_set('memory_limit', '-1');
    $error = error_get_last();
    if (!$error || !str_contains($error['message'], 'Allowed memory size')) return;
    if ($mode === 0) $app->appendRow(null, null);
    else $app->appendRow(null);
    $app->close();
    if ($db->query('SELECT count(*) FROM t')->fetchColumn() !== 1) throw new Exception('partial row survived');
    $app = $db = null;
    echo "clean shutdown\n";
});
ini_set('memory_limit', (string)(memory_get_usage(true) + 40000));
echo "appending\n";
$app->appendRow(...$row);
echo "unexpected success\n";
PHP;
$php = escapeshellarg(getenv('TEST_PHP_EXECUTABLE')) . ' ' . getenv('TEST_PHP_EXTRA_ARGS');
$env = getenv();
$env['USE_ZEND_ALLOC'] = '0';
$env['USE_TRACKED_ALLOC'] = '1';
for ($mode = 0; $mode < 4; $mode++) {
    $cmd = $php . ' -d display_errors=1 -r ' . escapeshellarg($child) . ' -- ' . $mode;
    $p = proc_open($cmd, [1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes, null, $env);
    $output = stream_get_contents($pipes[1]);
    fclose($pipes[1]);
    $status = proc_close($p);
    if ($status !== 255 || !str_contains($output, "appending\n") || !str_contains($output, 'Allowed memory size')
        || !str_contains($output, "clean shutdown\n") || str_contains($output, 'unexpected success')) {
        throw new Exception("mode $mode exit $status: $output");
    }
    echo "mode $mode cleanup OK\n";
}
?>
--EXPECT--
mode 0 cleanup OK
mode 1 cleanup OK
mode 2 cleanup OK
mode 3 cleanup OK
