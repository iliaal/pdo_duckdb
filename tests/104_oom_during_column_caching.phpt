--TEST--
pdo_duckdb: memory_limit bailout while caching result columns leaves a destroyable statement
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
if (!function_exists('proc_open')) die('skip proc_open() unavailable');
require __DIR__ . '/_bailout_shutdown.inc';
if (!pdo_duckdb_oom_runs_shutdown()) {
    die("skip memory_limit exhaustion exits without running shutdown functions\n");
}
?>
--FILE--
<?php
/* The child caps memory_limit headroom below one ZendMM chunk, so execute()
 * cannot map a new chunk and bails once it has used the pages left free in
 * the current one. A probe run fills those pages one at a time until it
 * bails, which counts them; each sweep run then fills all but a few and
 * frees a chosen number back. The parent measures execute()'s footprint and
 * sweeps the freed amount up to it, so the bailout lands early (inside
 * column caching) and late (in PDO core's column setup) whatever the
 * footprint of the PHP and DuckDB build. Builds that allocate per column
 * while caching (3dc1a58 and earlier) bail inside the caching loop; the
 * freed filler pages hold non-zero bytes that the column arrays reuse. */
$child = <<<'PHP'
$n = 2000;
[$mode, $filled, $freed] = [$argv[1], (int)($argv[2] ?? 0), (int)($argv[3] ?? 0)];
$pdo = new PDO('duckdb::memory:;threads=1', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$st = $pdo->prepare('SELECT ' . implode(',', array_fill(0, $n, '[[[[1]]]]')));
if ($mode === 'measure') {
    $before = memory_get_usage();
    $st->execute();
    echo 'footprint=', memory_get_usage() - $before, "\n";
    return;
}
$fill = array_fill(0, 4096, null);
$pad = str_repeat('x', 64 * 1024 * 1024 - memory_get_usage(true) - 256 * 1024);
/* 4071 bytes plus the zend_string header is exactly one 4 KiB page. */
if ($mode === 'probe') {
    for ($i = 0; $i < 4096; $i++) {
        $fill[$i] = str_repeat("\x41", 4071);
        echo '.';
    }
    return;
}
for ($i = 0; $i < $filled; $i++) {
    $fill[$i] = str_repeat("\x41", 4071);
}
for ($i = 1; $i <= $freed; $i++) {
    $fill[$filled - $i] = null;
}
echo "executing\n";
$st->execute();
echo "survived\n";
PHP;

/* run-tests exports the test's own php arguments, already shell-quoted, so
 * the child loads the extensions exactly as this process did (by absolute
 * path in CI). Probing by name is the fallback for a direct run. */
function run_child(array|string $cmd): array {
    $env = getenv();
    $env['USE_ZEND_ALLOC'] = '1';
    $proc = proc_open($cmd, [1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes, null, $env);
    $out = stream_get_contents($pipes[1]);
    fclose($pipes[1]);
    return [proc_close($proc), $out];
}

function child_command(array $tail): array|string {
    static $base = null;

    $extra = getenv('TEST_PHP_EXTRA_ARGS');
    if ($extra !== false) {
        return escapeshellarg(PHP_BINARY) . ' ' . $extra . ' -d memory_limit=64M '
            . implode(' ', array_map('escapeshellarg', $tail));
    }
    if ($base === null) {
        $base = [PHP_BINARY, '-n', '-d', 'extension_dir=' . ini_get('extension_dir'), '-d', 'memory_limit=64M'];
        foreach (['pdo', 'pdo_duckdb'] as $ext) {
            [, $loaded] = run_child(array_merge($base, ['-r', "echo extension_loaded('$ext') ? 1 : 0;"]));
            if (trim($loaded) !== '1') {
                $base[] = '-d';
                $base[] = "extension=$ext";
            }
        }
    }
    return array_merge($base, $tail);
}

[, $probe] = run_child(child_command(['-r', "echo extension_loaded('pdo_duckdb') ? 'loaded' : 'missing';"]));
if (trim($probe) !== 'loaded') {
    echo "child cannot load pdo_duckdb:\n", $probe, "\n";
}

[, $measured] = run_child(child_command(['-r', $child, '--', 'measure']));
[, $probed] = run_child(child_command(['-r', $child, '--', 'probe']));
$footprint = preg_match('/^footprint=(\d+)$/m', $measured, $m) ? (int)$m[1] : 0;
$free_pages = strspn($probed, '.');
if ($footprint < 65536 || $free_pages < 16 || !str_contains($probed, 'Allowed memory size')) {
    echo "cannot calibrate the sweep:\n", $measured, "\n", $probed, "\n";
}
/* Two pages of margin keep the sweep runs' own filling clear of the limit.
 * Freeing more than execute() needs would let it finish. */
$filled = $free_pages - 2;
$span = min($filled, intdiv($footprint, 4096));

$crashes = 0;
$oom_in_execute = 0;
for ($step = 0; $step < 6; $step++) {
    $freed = intdiv($span * (2 * $step + 1), 12) + 1;
    [$rc, $out] = run_child(child_command(['-r', $child, '--', 'run', (string)$filled, (string)$freed]));
    $oom = str_contains($out, "executing\n") && str_contains($out, 'Allowed memory size')
        && !str_contains($out, 'survived');
    if ($oom) {
        $oom_in_execute++;
    }
    if (!($rc === 0 && str_contains($out, 'survived')) && !($rc === 255 && $oom)) {
        $crashes++;
        echo "freed $freed of $filled pages (footprint $footprint): rc=$rc\n", $out, "\n";
    }
}
echo "crashes: $crashes\n";
echo "bailouts during execute: ", $oom_in_execute > 0 ? 'yes' : 'no', "\n";
?>
--EXPECT--
crashes: 0
bailouts during execute: yes
