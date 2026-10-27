--TEST--
pdo_duckdb: native conversion memory exhaustion leaves nested values and statements destroyable
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
if (!function_exists('proc_open')) die('skip proc_open() unavailable');
if (getenv('TEST_PHP_EXTRA_ARGS') === false) die('skip needs run-tests.php child arguments');
require __DIR__ . '/_bailout_shutdown.inc';
if (!pdo_duckdb_oom_runs_shutdown()) die('skip memory_limit exhaustion exits without shutdown functions');
?>
--FILE--
<?php
$child = <<<'PHP'
$reserve = str_repeat('r', ((int)$argv[1] === 7 ? 20 : 1) * 1024 * 1024);
$db = new PDO('duckdb::memory:;threads=1', null, null, [PDO::DUCKDB_ATTR_NATIVE_STRUCTURES => true]);
$queries = [
 'range(500000)::JSON',
 '(SELECT list(struct_pack(a := i, b := i::VARCHAR)) FROM range(500000) t(i))',
 'MAP(range(500000),range(500000))',
 'list_resize([]::INTEGER[], 4000000, 0)',
 "to_json(repeat('x', 70*1024*1024))",
 'range(500000)::VARIANT',
 "list_resize([]::VARCHAR[], 600000, repeat('x',128))",
 'list_resize([]::DOUBLE[], 3000000, 123456789.1255)',
];
$st = $db->query('SELECT [7] AS ready, 7 AS bound, ' . $queries[(int)$argv[1]] . ' AS exhausted');
$bound = 'unchanged';
$row = 'unassigned';
$st->bindColumn(2, $bound, PDO::PARAM_STR);
register_shutdown_function(function () {
    global $reserve, $st, $db, $row, $bound;
    $reserve = null;
    $error = error_get_last();
    if (!$error || !str_contains($error['message'], 'Allowed memory size')) return;
    if ($row !== 'unassigned' || $bound !== 'unchanged') {
        echo "partial row exposed\n";
        return;
    }
    $st = null;
    $db = null;
    echo "clean shutdown\n";
});
echo "fetching\n";
$row = $st->fetch(PDO::FETCH_NUM);
echo "unexpected success\n";
PHP;
$php = escapeshellarg(getenv('TEST_PHP_EXECUTABLE')) . ' ' . getenv('TEST_PHP_EXTRA_ARGS');
$env = getenv();
if (($env['USE_ZEND_ALLOC'] ?? '') !== '0' || ($env['USE_TRACKED_ALLOC'] ?? '') !== '1') {
    $env['USE_ZEND_ALLOC'] = '1';
}
for ($kind=0; $kind<8; $kind++) {
    $cmd = $php . ' -d memory_limit=32M -d display_errors=1 -r ' . escapeshellarg($child) . ' -- ' . $kind;
    $proc = proc_open($cmd, [1=>['pipe','w'],2=>['redirect',1]], $pipes, null, $env);
    $output = stream_get_contents($pipes[1]);
    fclose($pipes[1]);
    $status = proc_close($proc);
    if ($status !== 255 || !str_contains($output, "fetching\n") || !str_contains($output, 'Allowed memory size')
        || !str_contains($output, "clean shutdown\n") || str_contains($output, 'unexpected success')) {
        throw new Exception("conversion $kind exit $status: $output");
    }
    echo "conversion $kind OOM cleanup OK\n";
}
?>
--EXPECT--
conversion 0 OOM cleanup OK
conversion 1 OOM cleanup OK
conversion 2 OOM cleanup OK
conversion 3 OOM cleanup OK
conversion 4 OOM cleanup OK
conversion 5 OOM cleanup OK
conversion 6 OOM cleanup OK
conversion 7 OOM cleanup OK
