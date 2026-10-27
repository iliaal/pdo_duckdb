--TEST--
pdo_duckdb: shared file shutdown preserves commits and contains appender cleanup
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
$directory = __DIR__ . '/159_work';
@mkdir($directory);
$script = $directory . '/child.php';
file_put_contents($script, <<<'PHP'
<?php
[, $path, $mode] = $argv;
$options = [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
if ($mode === 'persistent') {
    $options[PDO::ATTR_PERSISTENT] = true;
}
$a = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb:' . $path, null, null, $options)
    : new PDO('duckdb:' . $path, null, null, $options);
$a->exec('CREATE TABLE t (id INTEGER)');
if (str_starts_with($mode, 'appender_')) {
    $appender = $a->duckdbAppender('t');
    $appender->appendRow(7);
    if ($mode === 'appender_refused') {
        $a->exec('SET lock_configuration=true');
    }
    ini_set('open_basedir', __DIR__);
    $appender = null;
    echo "ready\n";
    exit;
}
$a->exec('INSERT INTO t VALUES (1)');
$b = new PDO('duckdb:' . $path, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$a->exec('INSERT INTO t VALUES (2)');
$b->exec('INSERT INTO t VALUES (3)');
$a->beginTransaction();
$a->exec('INSERT INTO t VALUES (99)');
if ($mode === 'a_early') {
    $a = null;
}
echo "ready\n";
PHP
);
$php = escapeshellarg(getenv('TEST_PHP_EXECUTABLE')) . ' ' . getenv('TEST_PHP_EXTRA_ARGS');
foreach (['both_live', 'a_early', 'persistent', 'appender_escalated', 'appender_refused'] as $mode) {
    $path = $directory . '/' . $mode . '.duckdb';
    @unlink($path);
    @unlink($path . '.wal');
    $command = $php . ' ' . implode(' ', array_map('escapeshellarg', [$script, $path, $mode]));
    $process = proc_open($command, [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes);
    fclose($pipes[0]);
    $output = stream_get_contents($pipes[1]);
    fclose($pipes[1]);
    $status = proc_close($process);
    echo $mode, '_shutdown=', $status === 0 && $output === "ready\n" ? 'clean' : "BAD status=$status output=$output", "\n";
    $db = new PDO('duckdb:' . $path, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    echo $mode, '_reopened=', implode(',', $db->query('SELECT id FROM t ORDER BY id')->fetchAll(PDO::FETCH_COLUMN)), "\n";
    $db = null;
}
$path = $directory . '/warning_reentry.duckdb';
@unlink($path);
@unlink($path . '.wal');
$a = PHP_VERSION_ID >= 80400 ? PDO::connect('duckdb:' . $path) : new PDO('duckdb:' . $path);
$a->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$a->exec('CREATE TABLE t (id INTEGER PRIMARY KEY)');
$b = new PDO('duckdb:' . $path, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$appender = $a->duckdbAppender('t');
$appender->appendRow(1);
$appender->appendRow(1);
$warned = false;
set_error_handler(static function (int $severity, string $message) use ($b, &$warned): bool {
    if (!str_contains($message, 'close during destruction failed') || !str_contains($message, 'PRIMARY KEY')) {
        throw new RuntimeException($message);
    }
    $warned = true;
    echo 'warning_reentry=', $b->query('SELECT 42')->fetchColumn(), "\n";
    return true;
});
$appender = null;
restore_error_handler();
echo 'warning_observed=', $warned ? 'yes' : 'BAD', "\n";
$a = $b = null;
?>
--CLEAN--
<?php
$directory = __DIR__ . '/159_work';
foreach (['both_live', 'a_early', 'persistent', 'appender_escalated', 'appender_refused', 'warning_reentry'] as $mode) {
    @unlink($directory . '/' . $mode . '.duckdb');
    @unlink($directory . '/' . $mode . '.duckdb.wal');
}
@unlink($directory . '/child.php');
@rmdir($directory);
?>
--EXPECT--
both_live_shutdown=clean
both_live_reopened=1,2,3
a_early_shutdown=clean
a_early_reopened=1,2,3
persistent_shutdown=clean
persistent_reopened=1,2,3
appender_escalated_shutdown=clean
appender_escalated_reopened=7
appender_refused_shutdown=clean
appender_refused_reopened=
warning_reentry=42
warning_observed=yes
