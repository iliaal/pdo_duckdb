--TEST--
pdo_duckdb: memory_limit bailout while caching result columns leaves a destroyable statement
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
if (!function_exists('proc_open')) die('skip proc_open() unavailable');
?>
--FILE--
<?php
/* The bailout must land inside execute()'s per-column caching loop, after the
 * column arrays exist. ZendMM only enforces memory_limit when it maps a new
 * chunk, so each child leaves a different amount of headroom and the sweep
 * covers several chunk boundaries inside the loop; the nested columns make
 * the loop allocate several chunks' worth on debug and release builds. The
 * freed junk strings leave non-zero bytes in the pages the column arrays
 * reuse. */
$child = <<<'PHP'
$n = 10000;
$slack = (int)$argv[1];
$pdo = new PDO('duckdb::memory:;threads=1', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$st = $pdo->prepare('SELECT ' . implode(',', array_fill(0, $n, '[[[[1]]]]')));
$pad = str_repeat('x', 64 * 1024 * 1024 - memory_get_usage(true) - $slack);
$junk = [];
for ($j = 0; $j < 4; $j++) {
    $junk[] = str_repeat("\x41", 8 * $n - 64);
}
unset($junk);
echo "executing\n";
$st->execute();
echo "survived\n";
PHP;

function run_child(array $args): array {
    $env = getenv();
    $env['USE_ZEND_ALLOC'] = '1';
    $proc = proc_open($args, [1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes, null, $env);
    $out = stream_get_contents($pipes[1]);
    fclose($pipes[1]);
    return [proc_close($proc), $out];
}

$base = [PHP_BINARY, '-n', '-d', 'extension_dir=' . ini_get('extension_dir'), '-d', 'memory_limit=64M'];
foreach (['pdo', 'pdo_duckdb'] as $ext) {
    [, $loaded] = run_child(array_merge($base, ['-r', "echo extension_loaded('$ext') ? 1 : 0;"]));
    if (trim($loaded) !== '1') {
        $base[] = '-d';
        $base[] = "extension=$ext";
    }
}

$crashes = 0;
$oom_in_execute = 0;
foreach ([256, 2304, 4352] as $kib) {
    [$rc, $out] = run_child(array_merge($base, ['-r', $child, '--', (string)($kib * 1024)]));
    $oom = str_contains($out, "executing\n") && str_contains($out, 'Allowed memory size')
        && !str_contains($out, 'survived');
    if ($oom) {
        $oom_in_execute++;
    }
    if (!($rc === 0 && str_contains($out, 'survived')) && !($rc === 255 && $oom)) {
        $crashes++;
        echo "headroom {$kib}KiB: rc=$rc\n", $out, "\n";
    }
}
echo "crashes: $crashes\n";
echo "bailouts during execute: ", $oom_in_execute > 0 ? 'yes' : 'no', "\n";
?>
--EXPECT--
crashes: 0
bailouts during execute: yes
