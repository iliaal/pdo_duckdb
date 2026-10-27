--TEST--
pdo_duckdb: runtime open_basedir escalation survives an extension install racing in another process
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
require __DIR__ . '/duckdb_v2.inc';
$dsn = 'duckdb::memory:;threads=1' . (pdo_duckdb_v2_headers() ? ';async_threads=1' : '');
// Escalation lists loaded extensions through duckdb_extensions(), which reads
// the .info file of everything in the extension directory. An INSTALL in
// another process locks its temporary .info file while writing it, so a
// colliding listing fails with an IO error; that must not refuse the handle.
$work = __DIR__ . '/148_work_' . getmypid();
$extdir = "$work/ext";
mkdir($work);
mkdir($extdir);
// Unsigned and metadata-mismatched, so it installs offline; it is never loaded.
$dummy = "$work/pdo148dummy.duckdb_extension";
file_put_contents($dummy, str_repeat("\0", 4096));
$ready = "$work/ready";
$running = "$work/running";
$stop = "$work/stop";
file_put_contents("$work/installer.php", <<<'PHP'
<?php
[, $dsn, $extdir, $file, $ready, $running, $stop] = $argv;
$db = new PDO($dsn . ';allow_unsigned_extensions=true', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('SET extension_directory=' . $db->quote($extdir));
$db->exec('SET allow_extensions_metadata_mismatch=true');
$install = 'FORCE INSTALL ' . $db->quote($file);
$db->exec($install);
touch($ready);
if (fgets(STDIN) !== "start\n") {
    throw new RuntimeException('Installer did not receive the start signal');
}
$installs = 0;
$deadline = microtime(true) + 60;
while (!file_exists($stop) && microtime(true) < $deadline) {
    // Pause so each install holds its lock for a fraction of the time, as a
    // real INSTALL does; a retrying listing then finds a gap well within its
    // budget, while a single listing still collides often.
    usleep(5000);
    $db->exec($install);
    $installs++;
    if ($installs === 1) {
        touch($running);
    }
}
if (!file_exists($stop)) {
    throw new RuntimeException('Installer timed out before all sandbox gates completed');
}
echo $installs > 1 ? "installer looped\n" : "installer ran once\n";
PHP);

function pdo148_wait(string $path, $proc): void {
    $deadline = microtime(true) + 30;
    while (!file_exists($path) && microtime(true) < $deadline && proc_get_status($proc)['running']) {
        usleep(10000);
    }
    if (!file_exists($path)) {
        throw new RuntimeException('Installer handshake failed: ' . basename($path));
    }
}

// Fork before the parent owns 100 independent databases and their thread stacks.
$php = escapeshellarg(getenv('TEST_PHP_EXECUTABLE')) . ' ' . getenv('TEST_PHP_EXTRA_ARGS');
$cmd = $php . ' ' . implode(' ', array_map('escapeshellarg', ["$work/installer.php", $dsn, $extdir, $dummy, $ready, $running, $stop]));
$proc = proc_open($cmd, [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes);
if (!is_resource($proc)) {
    throw new RuntimeException('Unable to start the installer');
}
try {
    pdo148_wait($ready, $proc);
    echo "installer ready\n";
    $handles = [];
    for ($i = 0; $i < 100; $i++) {
        $db = new PDO($dsn, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
        $db->exec('SET extension_directory=' . $db->quote($extdir));
        $handles[] = $db;
    }
    if (fwrite($pipes[0], "start\n") !== 6) {
        throw new RuntimeException('Unable to start the installer loop');
    }
    fclose($pipes[0]);
    pdo148_wait($running, $proc);
    ini_set('open_basedir', $work);

    $refused = [];
    $externalEnabled = 0;
    foreach ($handles as $db) {
        try {
            $db->exec('SELECT 1');
            if ($db->query("SELECT current_setting('enable_external_access')")->fetchColumn()) {
                $externalEnabled++;
            }
        } catch (PDOException $e) {
            $refused[] = $e->getMessage();
        }
    }
    if (!proc_get_status($proc)['running']) {
        throw new RuntimeException('Installer stopped before all 100 sandbox gates completed');
    }
} finally {
    touch($stop);
    if (is_resource($pipes[0])) {
        fclose($pipes[0]);
    }
    echo stream_get_contents($pipes[1]);
    fclose($pipes[1]);
    echo 'installer rc=', proc_close($proc), "\n";
}

echo 'refused=', count($refused), "\n";
foreach (array_slice(array_unique($refused), 0, 3) as $message) {
    echo $message, "\n";
}
echo 'external_access=', $externalEnabled ? 'on' : 'off', "\n";
?>
--CLEAN--
<?php
function pdo148_rmtree(string $path): void {
    if (is_dir($path) && !is_link($path)) {
        foreach (scandir($path) ?: [] as $entry) {
            if ($entry !== '.' && $entry !== '..') {
                pdo148_rmtree("$path/$entry");
            }
        }
        @rmdir($path);
    } else {
        @unlink($path);
    }
}
foreach (glob(__DIR__ . '/148_work_*') ?: [] as $dir) {
    pdo148_rmtree($dir);
}
?>
--EXPECT--
installer ready
installer looped
installer rc=0
refused=0
external_access=off
