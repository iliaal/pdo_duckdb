--TEST--
pdo_duckdb: memory_limit bailout while duckdbAppender() caches column types shuts down cleanly
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
if (!function_exists('proc_open')) die('skip proc_open() unavailable');
$env = getenv();
$env['USE_ZEND_ALLOC'] = '0';
$env['USE_TRACKED_ALLOC'] = '1';
$p = proc_open([PHP_BINARY, '-n', '-r', 'ini_set("memory_limit", "4M"); $s = str_repeat("x", 8 << 20); echo "no limit";'],
    [1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes, null, $env);
$out = stream_get_contents($pipes[1]);
proc_close($p);
if (!str_contains($out, 'Allowed memory size')) die('skip memory_limit is not enforced by the tracked allocator');
?>
--FILE--
<?php
// The child runs on the tracked allocator so memory_limit applies per byte. The
// sweep moves the limit across every allocation duckdbAppender() makes while
// caching column metadata; whichever one hits the limit, the request-shutdown
// free of the half-built appender must not touch uninitialized handles.
$child = <<<'PHP'
<?php
[, $n, $headroom] = $argv;
$n = (int)$n;
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$cols = [];
for ($i = 0; $i < $n; $i++) {
    $cols[] = "c$i INTEGER";
}
$db->exec('CREATE TABLE t (' . implode(', ', $cols) . ')');
unset($cols);
// Populates PDO core's lazy driver-method table before the limit is set.
$warm = $db->duckdbAppender('t');
unset($warm);
// Stale non-NULL bytes for the column-type array to land on.
$junk = [];
for ($i = 0; $i < 4; $i++) {
    $junk[] = str_repeat("\x41", 8 * $n);
}
unset($junk);
ini_set('memory_limit', (string)(memory_get_usage(true) + (int)$headroom));
$ap = $db->duckdbAppender('t');
echo "survived\n";
PHP;

$dir = __DIR__ . '/103_work';
@mkdir($dir);
$script = $dir . '/child.php';
file_put_contents($script, $child);

$ext_dir = ini_get('extension_dir');
$cmd = [PHP_BINARY, '-n', '-d', 'extension_dir=' . $ext_dir,
    '-d', 'display_startup_errors=0', '-d', 'log_errors=0'];
foreach (['pdo', 'pdo_duckdb'] as $ext) {
    if (is_file($ext_dir . '/' . $ext . '.' . PHP_SHLIB_SUFFIX)) {
        $cmd[] = '-d';
        $cmd[] = 'extension=' . $ext;
    }
}
$env = getenv();
$env['USE_ZEND_ALLOC'] = '0';
$env['USE_TRACKED_ALLOC'] = '1';

$n = 200;
$fatal = $survived = 0;
// The type-cache arrays take about 30 bytes per column after the 8-byte
// handles, so a 512-byte step lands several limits inside that window. The
// last run has headroom for the whole setup.
$limits = range(512, 40 * $n, 512);
$limits[] = 8 * 1024 * 1024;
foreach ($limits as $headroom) {
    $proc = proc_open(array_merge($cmd, [$script, (string)$n, (string)$headroom]),
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
echo "done\n";
?>
--CLEAN--
<?php
$dir = __DIR__ . '/103_work';
foreach (glob($dir . '/*') ?: [] as $f) {
    @unlink($f);
}
@rmdir($dir);
?>
--EXPECT--
bool(true)
bool(true)
done
