--TEST--
pdo_duckdb: file-backed open under open_basedir does not allowlist temp for SQL FS
--EXTENSIONS--
pdo
pdo_duckdb
--INI--
open_basedir={PWD}
--FILE--
<?php
$dbf = __DIR__ . '/074_sandbox_' . getmypid() . '.duckdb';
@unlink($dbf);

$db = new PDO('duckdb:' . $dbf, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$temp = (string)$db->query("SELECT current_setting('temp_directory')")->fetchColumn();
echo 'temp_empty=', ($temp === '' ? 'yes' : 'no'), "\n";
echo 'external=', $db->query("SELECT current_setting('enable_external_access')::VARCHAR")->fetchColumn(), "\n";
echo 'locked=', $db->query("SELECT current_setting('lock_configuration')::VARCHAR")->fetchColumn(), "\n";

// Even if DuckDB still reports a non-empty temp, SQL FS under that path must fail.
$probe = $temp !== '' ? $temp . '/leak.csv' : __DIR__ . '/074_leak_' . getmypid() . '.csv';
if ($temp !== '') {
    @mkdir($temp, 0700, true);
}
file_put_contents($probe, "col\nSECRET\n");
try {
    $db->query('SELECT * FROM read_csv(' . $db->quote($probe) . ', header=true)');
    echo "sql_fs=BAD\n";
} catch (PDOException $e) {
    echo "sql_fs=blocked\n";
}

// In-DB SQL still works.
$db->exec('CREATE TABLE t(i INTEGER)');
echo 'select=', (int)$db->query('SELECT 1')->fetchColumn(), "\n";

unset($db);
@unlink($probe);
@unlink($dbf);
if ($temp !== '') {
    @rmdir($temp);
}
?>
--CLEAN--
<?php
foreach (glob(__DIR__ . '/074_sandbox_*.duckdb') ?: [] as $f) {
    @unlink($f);
}
foreach (glob(__DIR__ . '/074_leak_*.csv') ?: [] as $f) {
    @unlink($f);
}
?>
--EXPECT--
temp_empty=yes
external=false
locked=true
sql_fs=blocked
select=1
