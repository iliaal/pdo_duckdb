--TEST--
pdo_duckdb: sandbox escalation clears sibling query loggers before locking the shared instance
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
$path = __DIR__ . '/155_shared.duckdb';
$log = __DIR__ . '/155_sibling.sql';
foreach ([$path, $path . '.wal', $log] as $file) {
    @unlink($file);
}
$options = [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION];
$a = new PDO('duckdb:' . $path, null, null, $options);
$b = new PDO('duckdb:' . $path, null, null, $options);
$before = 'before_' . bin2hex(random_bytes(16));
$after = 'forbidden_' . bin2hex(random_bytes(16));
$b->exec('SET log_query_path=' . $b->quote($log));
$b->exec('SELECT ' . $b->quote($before));
$originalLog = file_get_contents($log);
echo 'logger_armed=', str_contains($originalLog, $before) ? 'yes' : 'BAD', "\n";

ini_set('open_basedir', __DIR__);
echo 'a_usable=', $a->query('SELECT 41')->fetchColumn(), "\n";
echo 'b_usable=', $b->query('SELECT 42')->fetchColumn(), "\n";
$b->exec('SELECT ' . $b->quote($after));
echo 'sibling_log_stopped=', file_get_contents($log) === $originalLog ? 'yes' : 'BAD', "\n";
foreach ([$a, $b] as $db) {
    echo 'external=', $db->query("SELECT current_setting('enable_external_access')::VARCHAR")->fetchColumn(), "\n";
    try {
        $db->exec('SET log_query_path=' . $db->quote($log));
        echo "rearm=BAD\n";
    } catch (PDOException $e) {
        echo 'rearm=', str_contains($e->getMessage(), 'configuration') && str_contains($e->getMessage(), 'locked') ? 'blocked' : 'BAD: ' . $e->getMessage(), "\n";
    }
    $db->exec('SELECT ' . $db->quote($after));
}
echo 'sibling_log_still_stopped=', file_get_contents($log) === $originalLog ? 'yes' : 'BAD', "\n";
$a = $b = $db = null;
echo 'teardown_log_stopped=', file_get_contents($log) === $originalLog ? 'yes' : 'BAD', "\n";
?>
--CLEAN--
<?php
@unlink(__DIR__ . '/155_shared.duckdb');
@unlink(__DIR__ . '/155_shared.duckdb.wal');
@unlink(__DIR__ . '/155_sibling.sql');
?>
--EXPECT--
logger_armed=yes
a_usable=41
b_usable=42
sibling_log_stopped=yes
external=false
rearm=blocked
external=false
rearm=blocked
sibling_log_still_stopped=yes
teardown_log_stopped=yes
