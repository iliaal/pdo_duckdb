--TEST--
pdo_duckdb: sandbox escalation on CONNECT dispatches no cleanup SQL remotely and contains local writers
--EXTENSIONS--
pdo
pdo_duckdb
--SKIPIF--
<?php
require __DIR__ . '/duckdb_v2.inc';
if (!pdo_duckdb_v2_headers()) die('skip needs a build against the DuckDB 2.0 headers');
if (!function_exists('proc_open') || !function_exists('stream_socket_server')) die('skip process or socket functions unavailable');
if (getenv('TEST_PHP_EXTRA_ARGS') === false) die('skip needs run-tests.php child arguments');
try {
    $db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    if ($dir = getenv('PDO_DUCKDB_TEST_EXTENSION_DIR')) {
        $db->exec('SET extension_directory=' . $db->quote($dir));
    }
    $db->exec('INSTALL httpfs; INSTALL quack; LOAD httpfs; LOAD quack');
} catch (PDOException $e) {
    die('skip quack extension not available: ' . strtok($e->getMessage(), "\n"));
}
?>
--FILE--
<?php
$directory = __DIR__ . '/155_connect_work';
@mkdir($directory);
@mkdir($directory . '/allowed');
$script = $directory . '/child.php';
file_put_contents($script, <<<'PHP'
<?php
$directory = __DIR__;
$path = $directory . '/allowed/local.duckdb';
$remoteLog = $directory . '/remote.sql';
$localLog = $directory . '/local.sql';
$localProfile = $directory . '/local.json';
foreach ([$path, $path . '.wal', $remoteLog, $localLog, $localProfile] as $file) {
    @unlink($file);
}
$options = [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
function load_quack(PDO $db): void {
    if ($dir = getenv('PDO_DUCKDB_TEST_EXTENSION_DIR')) {
        $db->exec('SET extension_directory=' . $db->quote($dir));
    }
    $db->exec('LOAD httpfs; LOAD quack');
}
$server = new PDO('duckdb::memory:', null, null, $options);
load_quack($server);
$socket = stream_socket_server('tcp://127.0.0.1:0', $errno, $errstr);
$port = (int) substr(strrchr(stream_socket_get_name($socket, false), ':'), 1);
fclose($socket);
$uri = "quack:127.0.0.1:$port";
$server->query("CALL quack_serve('$uri', token='tttt', disable_ssl=true)")->fetchAll();
$a = new PDO('duckdb:' . $path, null, null, $options);
$b = new PDO('duckdb:' . $path, null, null, $options);
load_quack($b);
$localControl = 'local_' . bin2hex(random_bytes(16));
$b->exec('SET log_query_path=' . $b->quote($localLog));
$b->exec("PRAGMA enable_profiling='json'");
$b->exec('SET profiling_output=' . $b->quote($localProfile));
$b->query('SELECT ' . $b->quote($localControl))->fetchAll();
echo 'local_logger_armed=', str_contains(file_get_contents($localLog), $localControl) ? 'yes' : 'BAD', "\n";
echo 'local_profiler_armed=', str_contains(file_get_contents($localProfile), $localControl) ? 'yes' : 'BAD', "\n";
$b->exec("CONNECT '$uri' (TOKEN 'tttt', DISABLE_SSL true)");
$b->exec('SET log_query_path=' . $b->quote($remoteLog));
$remoteControl = 'remote_' . bin2hex(random_bytes(16));
$remoteBefore = is_file($remoteLog) ? file_get_contents($remoteLog) : '';
$b->exec('SELECT ' . $b->quote($remoteControl));
$remoteAfter = file_get_contents($remoteLog);
echo 'remote_logger_armed=', strlen($remoteAfter) > strlen($remoteBefore)
    && !str_contains($remoteBefore, $remoteControl) && str_contains($remoteAfter, $remoteControl) ? 'yes' : 'BAD', "\n";
echo "ready\n";
fflush(STDOUT);
if (fgets(STDIN) !== "tighten\n") throw new RuntimeException('Parent did not capture writer baselines');
ini_set('open_basedir', $directory . '/allowed');
foreach ([$b, $a] as $db) {
    try {
        $db->exec('SELECT 424242');
        echo "sandbox_refused=BAD\n";
    } catch (PDOException $e) {
        echo 'sandbox_refused=', str_contains($e->getMessage(), 'not contained by it (quack)') ? 'yes' : 'BAD: ' . $e->getMessage(), "\n";
    }
}
echo "escalated\n";
fflush(STDOUT);
if (fgets(STDIN) !== "close\n") throw new RuntimeException('Parent did not inspect escalation writes');
$a = $b = $db = $server = null;
echo "closed\n";
PHP
);
$php = escapeshellarg(getenv('TEST_PHP_EXECUTABLE')) . ' ' . getenv('TEST_PHP_EXTRA_ARGS');
$process = proc_open($php . ' ' . escapeshellarg($script), [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['file', $directory . '/stderr.txt', 'w']], $pipes);
$files = ['remote' => $directory . '/remote.sql', 'local_log' => $directory . '/local.sql', 'local_profile' => $directory . '/local.json'];
$before = [];
$escalated = false;
while (($line = fgets($pipes[1])) !== false) {
    if ($line === "ready\n") {
        foreach ($files as $name => $file) {
            $before[$name] = file_get_contents($file);
        }
        fwrite($pipes[0], "tighten\n");
        fflush($pipes[0]);
    } elseif ($line === "escalated\n") {
        $escalated = true;
        foreach ($files as $name => $file) {
            $after = file_get_contents($file);
            if ($name === 'remote') {
                echo 'remote_escalation_growth=', strlen($after) - strlen($before[$name]), "\n";
            }
            echo $name, '_escalation_unchanged=', $after === $before[$name] ? 'yes' : 'BAD', "\n";
        }
        fwrite($pipes[0], "close\n");
        fflush($pipes[0]);
    } else {
        echo $line;
    }
}
fclose($pipes[0]);
fclose($pipes[1]);
$status = proc_close($process);
echo 'escalation_observed=', $escalated ? 'yes' : 'BAD', "\n";
echo 'child_exit=', $status, "\n";
foreach ($files as $name => $file) {
    $after = file_get_contents($file);
    if ($name === 'remote') {
        echo 'remote_teardown_growth=', strlen($after) - strlen($before[$name]), "\n";
    }
    echo $name, '_teardown_unchanged=', $after === $before[$name] ? 'yes' : 'BAD', "\n";
}
echo 'stderr=', file_get_contents($directory . '/stderr.txt') === '' ? 'empty' : 'BAD', "\n";
?>
--CLEAN--
<?php
$directory = __DIR__ . '/155_connect_work';
foreach (['child.php', 'remote.sql', 'local.sql', 'local.json', 'stderr.txt', 'allowed/local.duckdb', 'allowed/local.duckdb.wal'] as $name) {
    @unlink($directory . '/' . $name);
}
@rmdir($directory . '/allowed');
@rmdir($directory);
?>
--EXPECT--
local_logger_armed=yes
local_profiler_armed=yes
remote_logger_armed=yes
sandbox_refused=yes
sandbox_refused=yes
remote_escalation_growth=0
remote_escalation_unchanged=yes
local_log_escalation_unchanged=yes
local_profile_escalation_unchanged=yes
closed
escalation_observed=yes
child_exit=0
remote_teardown_growth=0
remote_teardown_unchanged=yes
local_log_teardown_unchanged=yes
local_profile_teardown_unchanged=yes
stderr=empty
