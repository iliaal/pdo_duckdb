--TEST--
pdo_duckdb: metadata allocation failure releases native declarations and annotations
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
$db = new PDO('duckdb::memory:;threads=1', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
if ((int)$argv[1] === 0) {
    $fields = [];
    for ($i = 0; $i < 16000; $i++) {
        $fields[] = 'field' . $i . " := 'Ab' COLLATE nocase";
    }
    $st = $db->query('SELECT struct_pack(' . implode(', ', $fields) . ') AS value');
    unset($fields);
} else {
    $db->exec("CREATE TYPE large_label AS ENUM (SELECT repeat('x', 20*1024*1024))");
    $st = $db->query('SELECT NULL::large_label AS value');
}
$metadata = 'unassigned';
register_shutdown_function(function () {
    global $reserve, $st, $db, $metadata;
    $reserve = null;
    $error = error_get_last();
    if (!$error || !str_contains($error['message'], 'Allowed memory size')) return;
    if ($metadata !== 'unassigned') {
        echo "partial metadata exposed\n";
        return;
    }
    $st = null;
    $db = null;
    echo "clean shutdown\n";
});
ini_set('memory_limit', (int)$argv[1] === 0
    ? (string)(memory_get_usage(true) + 4 * 1024 * 1024) : '8M');
echo "reading metadata\n";
$metadata = $st->getColumnMeta(0);
echo "unexpected success\n";
PHP;
$php = escapeshellarg(getenv('TEST_PHP_EXECUTABLE')) . ' ' . getenv('TEST_PHP_EXTRA_ARGS');
$env = getenv();
if (($env['USE_ZEND_ALLOC'] ?? '') !== '0' || ($env['USE_TRACKED_ALLOC'] ?? '') !== '1') {
    $env['USE_ZEND_ALLOC'] = '1';
}
foreach (['annotations', 'declaration'] as $kind => $name) {
    $cmd = $php . ' -d memory_limit=128M -d display_errors=1 -r ' . escapeshellarg($child) . ' -- ' . $kind;
    $proc = proc_open($cmd, [1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes, null, $env);
    $output = stream_get_contents($pipes[1]);
    fclose($pipes[1]);
    $status = proc_close($proc);
    if ($status !== 255 || !str_contains($output, "reading metadata\n")
            || !str_contains($output, 'Allowed memory size') || !str_contains($output, "clean shutdown\n")
            || str_contains($output, 'unexpected success')) {
        throw new Exception("metadata $name allocation exit $status: $output");
    }
    echo "PASS: $name allocation failure and native cleanup\n";
}
?>
--EXPECT--
PASS: annotations allocation failure and native cleanup
PASS: declaration allocation failure and native cleanup
