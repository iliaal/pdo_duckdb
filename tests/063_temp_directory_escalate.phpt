--TEST--
pdo_duckdb: runtime open_basedir escalate must not re-allowlist attacker temp_directory
--EXTENSIONS--
pdo
pdo_duckdb
--FILE--
<?php
// DuckDB EnableExternalAccessSetting::OnSet re-adds temporary_directory to
// allowed_directories when external access is turned off. The driver must
// reset temp_directory into open_basedir (or clear it) before that flip.
$outside = sys_get_temp_dir() . '/pdo_duckdb_oob_temp_' . getmypid();
@mkdir($outside, 0700, true);
file_put_contents("$outside/leak.csv", "col\nsecret\n");

$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('SET temp_directory = ' . $db->quote($outside));

ini_set('open_basedir', __DIR__);

echo 'external=', $db->query("SELECT current_setting('enable_external_access')::VARCHAR")->fetchColumn(), "\n";
echo 'locked=', $db->query("SELECT current_setting('lock_configuration')::VARCHAR")->fetchColumn(), "\n";

$temp = $db->query("SELECT current_setting('temp_directory')")->fetchColumn();
echo 'temp_inside_basedir=', (str_starts_with((string)$temp, __DIR__) || $temp === '' || $temp === null) ? 'yes' : 'no', "\n";

try {
    $rows = $db->query('SELECT * FROM read_csv(' . $db->quote("$outside/leak.csv") . ', header=true)')->fetchAll();
    echo 'BAD: leak rows=', json_encode($rows), "\n";
} catch (PDOException $e) {
    echo str_contains($e->getMessage(), 'disabled by configuration')
        || str_contains($e->getMessage(), 'Permission Error')
        || str_contains($e->getMessage(), 'Cannot access file')
        ? "blocked\n" : $e->getMessage() . "\n";
}

// In-basedir queries still work after escalate.
var_dump((int)$db->query('SELECT 7')->fetchColumn());
?>
--CLEAN--
<?php
$outside = sys_get_temp_dir() . '/pdo_duckdb_oob_temp_' . getmypid();
@unlink("$outside/leak.csv");
@rmdir($outside);
?>
--EXPECT--
external=false
locked=true
temp_inside_basedir=yes
blocked
int(7)
