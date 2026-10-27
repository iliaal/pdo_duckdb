--TEST--
pdo_duckdb: shared-instance sandbox escalation stops every connection's profiling file sink
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
$directory = __DIR__ . '/158_work';
@mkdir($directory);
$script = $directory . '/child.php';
file_put_contents($script, <<<'PHP'
<?php
$path = __DIR__ . '/shared.duckdb';
$profile = __DIR__ . '/profile.json';
foreach ([$path, $path . '.wal', $profile] as $file) {
    @unlink($file);
}
$options = [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
$a = new PDO('duckdb:' . $path, null, null, $options);
$b = new PDO('duckdb:' . $path, null, null, $options);
$before = 'before_' . bin2hex(random_bytes(16));
$after = 'forbidden_' . bin2hex(random_bytes(16));
$b->exec("PRAGMA enable_profiling='json'");
$b->exec('SET profiling_output=' . $b->quote($profile));
$b->query('SELECT ' . $b->quote($before))->fetchAll();
$originalProfile = file_get_contents($profile);
echo 'profile_armed=', str_contains($originalProfile, $before) ? 'yes' : 'BAD', "\n";
ini_set('open_basedir', __DIR__);
echo 'a_usable=', $a->query('SELECT 41')->fetchColumn(), "\n";
echo 'b_usable=', $b->query('SELECT 42')->fetchColumn(), "\n";
$b->query('SELECT ' . $b->quote($after))->fetchAll();
echo 'sibling_profile_stopped=', file_get_contents($profile) === $originalProfile ? 'yes' : 'BAD', "\n";
try {
    $b->exec('SET profiling_output=' . $b->quote($profile));
    echo "rearm=BAD\n";
} catch (PDOException $e) {
    echo 'rearm=', str_contains($e->getMessage(), 'configuration') && str_contains($e->getMessage(), 'locked') ? 'blocked' : 'BAD: ' . $e->getMessage(), "\n";
}
$b->query('SELECT ' . $b->quote($after))->fetchAll();
echo 'sibling_profile_still_stopped=', file_get_contents($profile) === $originalProfile ? 'yes' : 'BAD', "\n";
PHP
);
$php = escapeshellarg(getenv('TEST_PHP_EXECUTABLE')) . ' ' . getenv('TEST_PHP_EXTRA_ARGS');
$process = proc_open($php . ' ' . escapeshellarg($script), [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['file', $directory . '/stderr.txt', 'w']], $pipes);
fclose($pipes[0]);
$output = stream_get_contents($pipes[1]);
fclose($pipes[1]);
$status = proc_close($process);
echo $output;
echo 'child_exit=', $status, "\n";
echo 'profile_stderr=', file_get_contents($directory . '/stderr.txt') === '' ? 'empty' : 'BAD', "\n";

$streamScript = $directory . '/stream.php';
$marker = 'forbidden_stream_' . bin2hex(random_bytes(16));
@mkdir($directory . '/allowed');
file_put_contents($streamScript, <<<'PHP'
<?php
[, $marker] = $argv;
$allowed = __DIR__ . '/allowed';
$path = $allowed . '/shared.duckdb';
$profile = __DIR__ . '/stream-profile.json';
$csv = $allowed . '/blocked.csv';
foreach ([$path, $path . '.wal', $profile] as $file) {
    @unlink($file);
}
file_put_contents($csv, "value\nforbidden\n");
$options = [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
$a = new PDO('duckdb:' . $path, null, null, $options);
$a->exec('CREATE TABLE tx_probe (marker VARCHAR)');
$b = new PDO('duckdb:' . $path, null, null, $options);
$before = 'armed_' . bin2hex(random_bytes(16));
$b->exec("PRAGMA enable_profiling='json'");
$b->exec('SET profiling_output=' . $b->quote($profile));
$b->query('SELECT ' . $b->quote($before))->fetchAll();
echo 'stream_profile_armed=', str_contains(file_get_contents($profile), $before) ? 'yes' : 'BAD', "\n";
$b->beginTransaction();
$pending = 'pending_' . bin2hex(random_bytes(16));
$b->exec('INSERT INTO tx_probe VALUES (' . $b->quote($pending) . ')');
$b->setAttribute(PDO::DUCKDB_ATTR_UNBUFFERED, true);
$stream = $b->query('SELECT ' . $b->quote($marker) . ' AS marker, i FROM range(1000000) r(i)');
echo 'stream_first_row=', $stream->fetch(PDO::FETCH_NUM) === [$marker, 0] ? 'yes' : 'BAD', "\n";
echo 'stream_marker_initially_absent=', !str_contains(file_get_contents($profile), $marker) ? 'yes' : 'BAD', "\n";
echo "stream_before_action\n";
fflush(STDOUT);
if (fgets(STDIN) !== "continue\n") {
    throw new RuntimeException('Parent did not inspect the initial stream profile');
}
ini_set('open_basedir', $allowed);
try {
    $value = $a->query('SELECT 41')->fetchColumn();
    echo 'stream_a_usable=', $value, "\n";
} catch (PDOException $e) {
    echo 'stream_a_usable=BAD: ', $e->getMessage(), "\n";
}
$preserved = true;
try {
    for ($seen = 1; $seen < 10000; $seen++) {
        if ($stream->fetch(PDO::FETCH_NUM) !== [$marker, $seen]) {
            $preserved = false;
            break;
        }
    }
} catch (PDOException $e) {
    $preserved = false;
}
echo 'stream_contract=', $preserved ? 'yes' : 'BAD', "\n";
try {
    echo 'stream_close=', $stream->closeCursor() ? 'yes' : 'BAD', "\n";
} catch (PDOException $e) {
    echo 'stream_close=BAD: ', $e->getMessage(), "\n";
}
echo "stream_cursor_closed\n";
fflush(STDOUT);
if (fgets(STDIN) !== "continue\n") {
    throw new RuntimeException('Parent did not inspect the closed cursor profile');
}
echo 'stream_transaction_write=', $b->query('SELECT count(*) FROM tx_probe WHERE marker=' . $b->quote($pending))->fetchColumn(), "\n";
echo 'stream_transaction_isolated=', $a->query('SELECT count(*) FROM tx_probe WHERE marker=' . $a->quote($pending))->fetchColumn(), "\n";
$b->rollBack();
echo 'stream_transaction_rollback=', $a->query('SELECT count(*) FROM tx_probe')->fetchColumn(), "\n";
foreach ([$a, $b] as $db) {
    echo 'stream_external=', $db->query("SELECT current_setting('enable_external_access')::VARCHAR")->fetchColumn(), "\n";
    try {
        $db->query('SELECT * FROM read_csv(' . $db->quote($csv) . ', header=true)')->fetchAll();
        echo "stream_sql_read=BAD\n";
    } catch (PDOException $e) {
        echo 'stream_sql_read=', str_contains($e->getMessage(), 'disabled by configuration') ? 'blocked' : 'BAD: ' . $e->getMessage(), "\n";
    }
}
$stream = $a = $b = $db = null;
PHP
);
$command = $php . ' ' . implode(' ', array_map('escapeshellarg', [$streamScript, $marker]));
$process = proc_open($command, [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['file', $directory . '/stream-stderr.txt', 'w']], $pipes);
$observedClose = false;
$beforeProfile = null;
while (($line = fgets($pipes[1])) !== false) {
    if ($line === "stream_before_action\n") {
        $beforeProfile = file_get_contents($directory . '/stream-profile.json');
        fwrite($pipes[0], "continue\n");
        fflush($pipes[0]);
    } elseif ($line === "stream_cursor_closed\n") {
        $observedClose = true;
        echo 'stream_close_profile=', is_string($beforeProfile)
            && file_get_contents($directory . '/stream-profile.json') === $beforeProfile ? 'contained' : 'BAD', "\n";
        fwrite($pipes[0], "continue\n");
        fflush($pipes[0]);
    } else {
        echo $line;
    }
}
fclose($pipes[0]);
fclose($pipes[1]);
$status = proc_close($process);
echo 'stream_close_observed=', $observedClose ? 'yes' : 'BAD', "\n";
echo 'stream_child_exit=', $status, "\n";
echo 'stream_teardown_profile=', is_string($beforeProfile)
    && file_get_contents($directory . '/stream-profile.json') === $beforeProfile ? 'contained' : 'BAD', "\n";
echo 'stream_stderr=', file_get_contents($directory . '/stream-stderr.txt') === '' ? 'empty' : 'BAD', "\n";
?>
--CLEAN--
<?php
$directory = __DIR__ . '/158_work';
foreach (['child.php', 'shared.duckdb', 'shared.duckdb.wal', 'profile.json', 'stderr.txt', 'stream.php', 'stream-profile.json', 'stream-stderr.txt', 'allowed/shared.duckdb', 'allowed/shared.duckdb.wal', 'allowed/blocked.csv'] as $name) {
    @unlink($directory . '/' . $name);
}
@rmdir($directory . '/allowed');
@rmdir($directory);
?>
--EXPECT--
profile_armed=yes
a_usable=41
b_usable=42
sibling_profile_stopped=yes
rearm=blocked
sibling_profile_still_stopped=yes
child_exit=0
profile_stderr=empty
stream_profile_armed=yes
stream_first_row=yes
stream_marker_initially_absent=yes
stream_a_usable=41
stream_contract=yes
stream_close=yes
stream_close_profile=contained
stream_transaction_write=1
stream_transaction_isolated=0
stream_transaction_rollback=0
stream_external=false
stream_sql_read=blocked
stream_external=false
stream_sql_read=blocked
stream_close_observed=yes
stream_child_exit=0
stream_teardown_profile=contained
stream_stderr=empty
