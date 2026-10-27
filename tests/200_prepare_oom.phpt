--TEST--
pdo_duckdb: memory_limit bailout while preparing parameter slots shuts down cleanly
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
if (!function_exists('proc_open')) die('skip proc_open() unavailable');
if (getenv('TEST_PHP_EXTRA_ARGS') === false) die('skip needs run-tests.php child arguments');
require __DIR__ . '/_bailout_shutdown.inc';
if (!pdo_duckdb_oom_runs_shutdown()) {
    die("skip memory_limit exhaustion exits without running shutdown functions\n");
}
$env = getenv();
$env['USE_ZEND_ALLOC'] = '0';
$env['USE_TRACKED_ALLOC'] = '1';
$p = proc_open([PHP_BINARY, '-n', '-r', 'ini_set("memory_limit", "4M"); $s = str_repeat("x", 8 << 20); echo "no limit";'],
    [1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes, null, $env);
$out = stream_get_contents($pipes[1]);
fclose($pipes[1]);
proc_close($p);
if (!str_contains($out, 'Allowed memory size')) die('skip memory_limit is not enforced by the tracked allocator');
?>
--FILE--
<?php
$child = <<<'PHP'
<?php
$db = new PDO('duckdb::memory:');
$sql = 'SELECT ' . implode(',', array_fill(0, 200, '?'));
ini_set('memory_limit', (string)(memory_get_usage(true) + (int)$argv[1]));
$stmt = $db->prepare($sql);
echo "survived\n";
PHP;
$dir = __DIR__ . '/200_prepare_oom_work';
mkdir($dir);
$script = $dir . '/child.php';
file_put_contents($script, $child);
$php = escapeshellarg(getenv('TEST_PHP_EXECUTABLE')) . ' ' . getenv('TEST_PHP_EXTRA_ARGS');
$env = getenv();
$env['USE_ZEND_ALLOC'] = '0';
$env['USE_TRACKED_ALLOC'] = '1';
$fatal = $survived = 0;
$limits = range(512, 65536, 512);
$limits[] = 8 * 1024 * 1024;
foreach ($limits as $headroom) {
    $proc = proc_open($php . ' ' . escapeshellarg($script) . ' ' . $headroom,
        [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes, null, $env);
    fclose($pipes[0]);
    $out = stream_get_contents($pipes[1]);
    fclose($pipes[1]);
    $rc = proc_close($proc);
    if ($rc === 0 && $out === "survived\n") {
        $survived++;
    } elseif ($rc === 255 && preg_match('/^\s*(PHP )?Fatal error: +Allowed memory size of \d+ bytes exhausted[^\n]*\n?$/', $out)) {
        $fatal++;
    } else {
        echo "headroom=$headroom rc=$rc\n$out\n";
    }
}
var_dump($fatal > 0, $survived > 0);
?>
--CLEAN--
<?php
$dir = __DIR__ . '/200_prepare_oom_work';
foreach (glob($dir . '/*') ?: [] as $file) {
    unlink($file);
}
@rmdir($dir);
?>
--EXPECT--
bool(true)
bool(true)
