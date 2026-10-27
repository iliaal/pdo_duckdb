--TEST--
pdo_duckdb: sandbox escalation stops the global QueryLog before inspection SQL or refusal cleanup
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
$directory = __DIR__ . '/155_global_log_work';
@mkdir($directory);
$script = $directory . '/child.php';
file_put_contents($script, <<<'PHP'
<?php
[, $root, $mode, $marker] = $argv;
$path = $root . '/allowed/db.duckdb';
@unlink($path);
@unlink($path . '.wal');
@unlink($root . '/log/duckdb_log_entries.csv');
@unlink($root . '/log/duckdb_log_contexts.csv');
$options = [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
if ($mode === 'reuse') $options[PDO::ATTR_PERSISTENT] = true;
$db = new PDO('duckdb:' . $path, null, null, $options);
$db->exec("CALL enable_logging('QueryLog', storage='file', storage_path=" . $db->quote($root . '/log') . ')');
$db->exec('SELECT ' . $db->quote($marker));
if ($mode === 'locked') {
    $db->exec('SET lock_configuration=true');
} elseif (in_array($mode, ['close', 'destroy', 'reuse'], true)) {
    $db->setAttribute(PDO::DUCKDB_ATTR_UNBUFFERED, true);
    $statement = $db->query('SELECT i FROM range(100000) r(i)');
    if ($statement->fetchColumn() !== 0) throw new RuntimeException('Stream did not start');
}
echo "armed\n";
fflush(STDOUT);
if (fgets(STDIN) !== "tighten\n") throw new RuntimeException('Parent did not inspect the logging control');
ini_set('open_basedir', $root . '/allowed');
try {
    if ($mode === 'close') {
        echo 'cursor=', $statement->closeCursor() ? 'closed' : 'BAD', "\n";
    } elseif ($mode === 'destroy') {
        $statement = null;
        echo "cursor=destroyed\n";
    } elseif ($mode === 'reuse') {
        $checkout = new PDO('duckdb:' . $path, null, null, $options);
        $preserved = true;
        for ($row = 1; $row < 10000; $row++) {
            if ($statement->fetchColumn() !== $row) {
                $preserved = false;
                break;
            }
        }
        echo 'stream=', $preserved ? 'preserved' : 'BAD', "\n";
    } else {
        $value = $db->query('SELECT 42')->fetchColumn();
        echo 'query=', $mode === 'unlocked' && $value === 42 ? 'usable' : 'BAD', "\n";
    }
} catch (PDOException $e) {
    echo 'query=', $mode === 'locked' && $e->errorInfo[0] === 'HY000' && $e->errorInfo[1] === 4
        && str_contains($e->getMessage(), 'sandbox') ? 'refused' : 'BAD: ' . $e->getMessage(), "\n";
}
echo "after_action\n";
fflush(STDOUT);
if (fgets(STDIN) !== "close\n") throw new RuntimeException('Parent did not inspect logging after the query');
$statement = $checkout = $db = null;
PHP
);
$php = escapeshellarg(getenv('TEST_PHP_EXECUTABLE')) . ' ' . getenv('TEST_PHP_EXTRA_ARGS');
foreach (['unlocked', 'close', 'destroy', 'reuse', 'locked'] as $mode) {
    $root = $directory . '/' . $mode;
    @mkdir($root);
    @mkdir($root . '/allowed');
    $marker = 'control_' . bin2hex(random_bytes(16));
    $command = $php . ' ' . implode(' ', array_map('escapeshellarg', [$script, $root, $mode, $marker]));
    $process = proc_open($command, [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['file', $root . '/stderr.txt', 'w']], $pipes);
    $before = [];
    $observed = false;
    echo "mode=$mode\n";
    while (($line = fgets($pipes[1])) !== false) {
        if ($line === "armed\n") {
            foreach (['entries', 'contexts'] as $name) {
                $before[$name] = file_get_contents($root . '/log/duckdb_log_' . $name . '.csv');
            }
            echo 'querylog_armed=', str_contains($before['entries'], $marker) && $before['contexts'] !== '' ? 'yes' : 'BAD', "\n";
            fwrite($pipes[0], "tighten\n");
            fflush($pipes[0]);
        } elseif ($line === "after_action\n") {
            $observed = true;
            foreach (['entries', 'contexts'] as $name) {
                echo $name, '_after_action=', file_get_contents($root . '/log/duckdb_log_' . $name . '.csv') === $before[$name] ? 'unchanged' : 'BAD', "\n";
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
    echo 'action_observed=', $observed ? 'yes' : 'BAD', "\n";
    echo 'child_exit=', $status, "\n";
    foreach (['entries', 'contexts'] as $name) {
        echo $name, '_after_teardown=', file_get_contents($root . '/log/duckdb_log_' . $name . '.csv') === $before[$name] ? 'unchanged' : 'BAD', "\n";
    }
    echo 'stderr=', file_get_contents($root . '/stderr.txt') === '' ? 'empty' : 'BAD', "\n";
}
?>
--CLEAN--
<?php
$directory = __DIR__ . '/155_global_log_work';
foreach (['unlocked', 'locked', 'close', 'destroy', 'reuse'] as $mode) {
    $root = $directory . '/' . $mode;
    foreach (['duckdb_log_entries.csv', 'duckdb_log_contexts.csv'] as $name) {
        @unlink($root . '/log/' . $name);
    }
    foreach (['stderr.txt', 'allowed/db.duckdb', 'allowed/db.duckdb.wal'] as $name) {
        @unlink($root . '/' . $name);
    }
    @rmdir($root . '/log');
    @rmdir($root . '/allowed');
    @rmdir($root);
}
@unlink($directory . '/child.php');
@rmdir($directory);
?>
--EXPECT--
mode=unlocked
querylog_armed=yes
query=usable
entries_after_action=unchanged
contexts_after_action=unchanged
action_observed=yes
child_exit=0
entries_after_teardown=unchanged
contexts_after_teardown=unchanged
stderr=empty
mode=close
querylog_armed=yes
cursor=closed
entries_after_action=unchanged
contexts_after_action=unchanged
action_observed=yes
child_exit=0
entries_after_teardown=unchanged
contexts_after_teardown=unchanged
stderr=empty
mode=destroy
querylog_armed=yes
cursor=destroyed
entries_after_action=unchanged
contexts_after_action=unchanged
action_observed=yes
child_exit=0
entries_after_teardown=unchanged
contexts_after_teardown=unchanged
stderr=empty
mode=reuse
querylog_armed=yes
stream=preserved
entries_after_action=unchanged
contexts_after_action=unchanged
action_observed=yes
child_exit=0
entries_after_teardown=unchanged
contexts_after_teardown=unchanged
stderr=empty
mode=locked
querylog_armed=yes
query=refused
entries_after_action=unchanged
contexts_after_action=unchanged
action_observed=yes
child_exit=0
entries_after_teardown=unchanged
contexts_after_teardown=unchanged
stderr=empty
