--TEST--
pdo_duckdb: cursor destruction and live persistent checkout contain profiling before any SQL
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
$directory = __DIR__ . '/158_lifecycle_work';
@mkdir($directory);
$script = $directory . '/child.php';
file_put_contents($script, <<<'PHP'
<?php
[, $root, $action, $marker] = $argv;
$allowed = $root . '/allowed';
$path = $allowed . '/db.duckdb';
$profile = $root . '/profile.json';
foreach ([$path, $path . '.wal', $profile] as $file) {
    @unlink($file);
}
$options = [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
if ($action === 'reuse') {
    $options[PDO::ATTR_PERSISTENT] = true;
}
$dsn = 'duckdb:' . $path;
$db = new PDO($dsn, null, null, $options);
$db->exec('CREATE TABLE pending (marker VARCHAR)');
$observer = new PDO($dsn, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$before = 'armed_' . bin2hex(random_bytes(16));
$db->exec("PRAGMA enable_profiling='json'");
$db->exec('SET profiling_output=' . $db->quote($profile));
$db->query('SELECT ' . $db->quote($before))->fetchAll();
echo 'armed=', str_contains(file_get_contents($profile), $before) ? 'yes' : 'BAD', "\n";
$db->beginTransaction();
$pending = 'pending_' . bin2hex(random_bytes(16));
$db->exec('INSERT INTO pending VALUES (' . $db->quote($pending) . ')');
$db->setAttribute(PDO::DUCKDB_ATTR_UNBUFFERED, true);
$stmt = $db->query('SELECT ' . $db->quote($marker) . ' AS marker, i FROM range(1000000) r(i)');
echo 'first_row=', $stmt->fetch(PDO::FETCH_NUM) === [$marker, 0] ? 'yes' : 'BAD', "\n";
echo "before_action\n";
fflush(STDOUT);
if (fgets(STDIN) !== "act\n") throw new RuntimeException('Parent did not inspect the initial profile');
ini_set('open_basedir', $allowed);
try {
    if ($action === 'reuse') {
        $checkout = new PDO($dsn, null, null, $options);
    } elseif ($action === 'destroy') {
        $stmt = null;
    } elseif (!$stmt->closeCursor()) {
        throw new RuntimeException('closeCursor returned false');
    }
    echo "action=ok\n";
} catch (PDOException $e) {
    echo 'action=BAD: ', $e->getMessage(), "\n";
}
echo "after_action\n";
fflush(STDOUT);
if (fgets(STDIN) !== "continue\n") throw new RuntimeException('Parent did not inspect the action profile');
if ($action === 'reuse') {
    $preserved = true;
    try {
        for ($seen = 1; $seen < 10000; $seen++) {
            if ($stmt->fetch(PDO::FETCH_NUM) !== [$marker, $seen]) {
                $preserved = false;
                break;
            }
        }
    } catch (PDOException $e) {
        $preserved = false;
    }
    echo 'stream_preserved=', $preserved ? 'yes' : 'BAD', "\n";
    echo 'cursor_closed=', $stmt->closeCursor() ? 'yes' : 'BAD', "\n";
    echo "after_result_close\n";
    fflush(STDOUT);
    if (fgets(STDIN) !== "continue\n") throw new RuntimeException('Parent did not inspect the final cursor profile');
}
echo 'transaction_active=', $db->inTransaction() ? 'yes' : 'BAD', "\n";
echo 'pending_write=', $db->query('SELECT count(*) FROM pending WHERE marker=' . $db->quote($pending))->fetchColumn(), "\n";
echo 'pending_isolated=', $observer->query('SELECT count(*) FROM pending')->fetchColumn(), "\n";
$db->rollBack();
echo 'rollback=', $observer->query('SELECT count(*) FROM pending')->fetchColumn(), "\n";
$stmt = $checkout = $db = $observer = null;
PHP
);
$php = escapeshellarg(getenv('TEST_PHP_EXECUTABLE')) . ' ' . getenv('TEST_PHP_EXTRA_ARGS');
foreach (['close', 'destroy', 'reuse'] as $action) {
    $root = $directory . '/' . $action;
    @mkdir($root);
    @mkdir($root . '/allowed');
    $marker = 'forbidden_' . bin2hex(random_bytes(16));
    $command = $php . ' ' . implode(' ', array_map('escapeshellarg', [$script, $root, $action, $marker]));
    $process = proc_open($command, [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['file', $root . '/stderr.txt', 'w']], $pipes);
    $observed = [];
    $beforeProfile = null;
    echo "mode=$action\n";
    while (($line = fgets($pipes[1])) !== false) {
        $stage = rtrim($line, "\r\n");
        if (in_array($stage, ['before_action', 'after_action', 'after_result_close'], true)) {
            $observed[] = $stage;
            $profile = file_get_contents($root . '/profile.json');
            if ($stage === 'before_action') $beforeProfile = $profile;
            echo $stage, '_profile=', is_string($beforeProfile) && $profile === $beforeProfile
                && !str_contains($profile, $marker) ? 'contained' : 'BAD', "\n";
            fwrite($pipes[0], $stage === 'before_action' ? "act\n" : "continue\n");
            fflush($pipes[0]);
        } else {
            echo $line;
        }
    }
    fclose($pipes[0]);
    fclose($pipes[1]);
    $status = proc_close($process);
    $stages = $action === 'reuse' ? ['before_action', 'after_action', 'after_result_close'] : ['before_action', 'after_action'];
    echo 'stages_observed=', $observed === $stages ? 'yes' : 'BAD', "\n";
    echo 'child_exit=', $status, "\n";
    echo 'teardown_profile=', is_string($beforeProfile)
        && file_get_contents($root . '/profile.json') === $beforeProfile ? 'contained' : 'BAD', "\n";
    echo 'stderr=', file_get_contents($root . '/stderr.txt') === '' ? 'empty' : 'BAD', "\n";
}
?>
--CLEAN--
<?php
$directory = __DIR__ . '/158_lifecycle_work';
foreach (['close', 'destroy', 'reuse'] as $action) {
    $root = $directory . '/' . $action;
    foreach (['profile.json', 'stderr.txt', 'allowed/db.duckdb', 'allowed/db.duckdb.wal'] as $name) {
        @unlink($root . '/' . $name);
    }
    @rmdir($root . '/allowed');
    @rmdir($root);
}
@unlink($directory . '/child.php');
@rmdir($directory);
?>
--EXPECT--
mode=close
armed=yes
first_row=yes
before_action_profile=contained
action=ok
after_action_profile=contained
transaction_active=yes
pending_write=1
pending_isolated=0
rollback=0
stages_observed=yes
child_exit=0
teardown_profile=contained
stderr=empty
mode=destroy
armed=yes
first_row=yes
before_action_profile=contained
action=ok
after_action_profile=contained
transaction_active=yes
pending_write=1
pending_isolated=0
rollback=0
stages_observed=yes
child_exit=0
teardown_profile=contained
stderr=empty
mode=reuse
armed=yes
first_row=yes
before_action_profile=contained
action=ok
after_action_profile=contained
stream_preserved=yes
cursor_closed=yes
after_result_close_profile=contained
transaction_active=yes
pending_write=1
pending_isolated=0
rollback=0
stages_observed=yes
child_exit=0
teardown_profile=contained
stderr=empty
