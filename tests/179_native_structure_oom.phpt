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
$reserve = str_repeat('r', 1024 * 1024);
$db = new PDO('duckdb::memory:;threads=1', null, null, [PDO::DUCKDB_ATTR_NATIVE_STRUCTURES => true]);
$queries = [
 'SELECT range(500000)::JSON',
 'SELECT list(struct_pack(a := i, b := i::VARCHAR)) FROM range(500000) t(i)',
 'SELECT MAP(range(500000),range(500000))',
];
$st = $db->query($queries[(int)$argv[1]]);
register_shutdown_function(function () {
    global $reserve, $st, $db;
    $reserve = null;
    $error = error_get_last();
    if (!$error || !str_contains($error['message'], 'Allowed memory size')) return;
    $st = null;
    $db = null;
    echo "clean shutdown\n";
});
echo "fetching\n";
$st->fetchColumn();
echo "unexpected success\n";
PHP;
$php = escapeshellarg(getenv('TEST_PHP_EXECUTABLE')) . ' ' . getenv('TEST_PHP_EXTRA_ARGS');
$env = getenv();
$env['USE_ZEND_ALLOC'] = '1';
for ($kind=0; $kind<3; $kind++) {
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
