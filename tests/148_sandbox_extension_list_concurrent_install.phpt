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
$stop = "$work/stop";
file_put_contents("$work/installer.php", <<<'PHP'
<?php
[, $extdir, $file, $ready, $stop] = $argv;
$db = new PDO('duckdb::memory:;allow_unsigned_extensions=true', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('SET extension_directory=' . $db->quote($extdir));
$db->exec('SET allow_extensions_metadata_mismatch=true');
$install = 'FORCE INSTALL ' . $db->quote($file);
$db->exec($install);
touch($ready);
$installs = 1;
$deadline = microtime(true) + 60;
while (!file_exists($stop) && microtime(true) < $deadline) {
    $db->exec($install);
    $installs++;
}
echo $installs > 1 ? "installer looped\n" : "installer ran once\n";
PHP);

$handles = [];
for ($i = 0; $i < 100; $i++) {
    $db = new PDO('duckdb::memory:;threads=1', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    $db->exec('SET extension_directory=' . $db->quote($extdir));
    $handles[] = $db;
}

$php = escapeshellarg(getenv('TEST_PHP_EXECUTABLE')) . ' ' . getenv('TEST_PHP_EXTRA_ARGS');
$cmd = $php . ' ' . implode(' ', array_map('escapeshellarg', ["$work/installer.php", $extdir, $dummy, $ready, $stop]));
$proc = proc_open($cmd, [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes);
fclose($pipes[0]);
$deadline = microtime(true) + 30;
while (!file_exists($ready) && microtime(true) < $deadline && proc_get_status($proc)['running']) {
    usleep(10000);
}
echo file_exists($ready) ? "installer ready\n" : "installer not ready\n";

ini_set('open_basedir', $work);

$refused = [];
foreach ($handles as $db) {
    try {
        $db->exec('SELECT 1');
    } catch (PDOException $e) {
        $refused[] = $e->getMessage();
    }
}
touch($stop);
echo stream_get_contents($pipes[1]);
fclose($pipes[1]);
echo 'installer rc=', proc_close($proc), "\n";

echo 'refused=', count($refused), "\n";
foreach (array_slice(array_unique($refused), 0, 3) as $message) {
    echo $message, "\n";
}
echo 'external_access=', $handles[99]->query("SELECT current_setting('enable_external_access')")->fetchColumn() ? 'on' : 'off', "\n";
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
